#include "config.h"

#include <gio/gio.h>
#include <gdk/gdk.h>
#include <cairo.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <cinnamon-bg-list.h>
#include <cinnamon-bg-monitors.h>

#include "csd-background-manager.h"
#include "bg-renderer.h"
#include "bg-wayland.h"
#include "bg-x11.h"

typedef enum {
    BG_TRANSITION_NONE,
    BG_TRANSITION_FADE_IN,
    BG_TRANSITION_BLEND,
} BgTransition;

struct _CsdBackgroundManager
{
    GObject      parent_instance;

    CinnamonBgList *bglist;
    gulong          changed_id;

    /* DisplayConfig monitors, for the logical index used as a cross-session
       fallback when a stored entry's connector name no longer matches. */
    CinnamonBgMonitors *bg_monitors;
    gulong              bg_monitors_changed_id;

    /* uri (char *) -> CachedImage *; one entry per distinct image in use.
     * Source pixbufs are only needed while rendering; the crossfade and surface
     * re-exposes run off the already-rendered textures. So we drop them a few
     * seconds after the last draw to keep steady-state RSS down, while staying
     * warm through a burst of changes (e.g. clicking through the wallpaper picker). */
    GHashTable  *pixbuf_cache;
    guint        pixbuf_release_id;

    /* connector (char *) -> BgRenderInput * (heap); rebuilt each draw */
    GHashTable  *resolved;

    gboolean     spanned;       /* mode == CINNAMON_BG_MODE_SPANNED; refreshed each draw */

    gboolean     using_wayland;

    GPtrArray   *windows;
    gboolean     first_draw;

    /* readiness: emitted once the wallpaper is actually on screen */
    gboolean     ready;
    guint        frames_painted;

    /* monitor hotplug */
    GListModel  *monitors_model;
    gulong       monitors_changed_id;

    /* transition preference */
    GSettings   *muffin_settings;
    BgTransition transition;
};

G_DEFINE_TYPE (CsdBackgroundManager, csd_background_manager, G_TYPE_OBJECT)

enum { SIGNAL_READY, N_SIGNALS };
static guint signals[N_SIGNALS];

static void draw_background (CsdBackgroundManager *m);
static void setup_wayland_monitors (CsdBackgroundManager *m);

/* ---------- pixbuf cache ---------- */

typedef struct {
    GdkPixbuf *pixbuf;
    gint64     mtime;
} CachedImage;

static void
cached_image_free (gpointer data)
{
    CachedImage *ci = data;
    g_clear_object (&ci->pixbuf);
    g_free (ci);
}

static void
clear_pixbuf_cache (CsdBackgroundManager *m)
{
    if (m->pixbuf_cache)
        g_hash_table_remove_all (m->pixbuf_cache);
}

static gint64
file_mtime (const char *path)
{
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info =
        g_file_query_info (file,
                           G_FILE_ATTRIBUTE_TIME_MODIFIED,
                           G_FILE_QUERY_INFO_NONE,
                           NULL, NULL);
    if (!info)
        return 0;

    return (gint64) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
}

/* Load (or return cached) pixbuf for a uri, refreshing on mtime change. The
 * cache is keyed by uri so monitors sharing an image only load it once. */
static GdkPixbuf *
get_pixbuf_for_uri (CsdBackgroundManager *m, const char *uri)
{
    if (!uri || uri[0] == '\0')
        return NULL;

    g_autofree char *path = g_filename_from_uri (uri, NULL, NULL);
    if (!path)
        return NULL;

    gint64 mtime = file_mtime (path);

    CachedImage *ci = g_hash_table_lookup (m->pixbuf_cache, uri);
    if (ci && ci->mtime == mtime)
        return ci->pixbuf;

    g_autoptr(GError) err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file (path, &err);

    if (!pixbuf) {
        g_warning ("Failed to load background pixbuf from %s: %s",
                   path, err ? err->message : "unknown error");
        g_hash_table_remove (m->pixbuf_cache, uri);
        return NULL;
    }

    g_message ("Loaded background image %s (%dx%d)", path,
               gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf));

    ci = g_new0 (CachedImage, 1);
    ci->pixbuf = pixbuf;
    ci->mtime = mtime;
    g_hash_table_insert (m->pixbuf_cache, g_strdup (uri), ci);

    return pixbuf;
}

/* Map a CinnamonBgItem to the cairo paint parameters, loading its pixbuf when it
 * has one. The colour fields always apply: they paint the backdrop showing
 * around image placements that don't cover the whole monitor. Returns the image
 * uri (owned by @item) for logging, or NULL when there is no picture. */
static void
parse_item_color (GdkRGBA *rgba, const char *color)
{
    if (!gdk_rgba_parse (rgba, color)) {
        g_warning ("csd-background: invalid background color '%s', using black",
                   color ? color : "(null)");
        *rgba = (GdkRGBA) { 0.0, 0.0, 0.0, 1.0 };
    }
}

static const char *
item_to_render_input (CsdBackgroundManager *m, CinnamonBgItem *item, BgRenderInput *in)
{
    in->shading = cinnamon_bg_item_get_color_shading_type (item);
    parse_item_color (&in->primary, cinnamon_bg_item_get_primary_color (item));
    parse_item_color (&in->secondary, cinnamon_bg_item_get_secondary_color (item));

    if (cinnamon_bg_item_has_picture (item)) {
        const char *uri = cinnamon_bg_item_get_picture_uri (item);
        in->placement = cinnamon_bg_item_get_picture_options (item);
        in->source = get_pixbuf_for_uri (m, uri);
        return uri;
    }

    in->placement = CINNAMON_BG_PLACEMENT_NONE;
    in->source = NULL;
    return NULL;
}

static void
get_single_input (CsdBackgroundManager *m, BgRenderInput *in)
{
    item_to_render_input (m, cinnamon_bg_list_get_single (m->bglist), in);
}

/* Drop cached images no longer assigned to any monitor (e.g. after a wallpaper
 * change or a slideshow tick) so the cache stays bounded to what is on screen. */
static void
prune_pixbuf_cache (CsdBackgroundManager *m)
{
    g_autoptr(GHashTable) keep = g_hash_table_new (g_str_hash, g_str_equal);

    const char *single = cinnamon_bg_item_get_picture_uri (cinnamon_bg_list_get_single (m->bglist));
    if (single && single[0] != '\0')
        g_hash_table_add (keep, (gpointer) single);

    GListModel *items = G_LIST_MODEL (m->bglist);
    guint n = g_list_model_get_n_items (items);
    for (guint i = 0; i < n; i++) {
        g_autoptr(CinnamonBgItem) item = g_list_model_get_item (items, i);
        const char *uri = cinnamon_bg_item_get_picture_uri (item);
        if (uri && uri[0] != '\0')
            g_hash_table_add (keep, (gpointer) uri);
    }

    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, m->pixbuf_cache);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        if (!g_hash_table_contains (keep, key))
            g_hash_table_iter_remove (&iter);
    }
}

#define PIXBUF_IDLE_RELEASE_SECONDS 3

static gboolean
release_pixbufs_cb (gpointer data)
{
    CsdBackgroundManager *m = data;
    g_debug ("Releasing %u cached source pixbuf(s) after idle",
             g_hash_table_size (m->pixbuf_cache));
    clear_pixbuf_cache (m);
    m->pixbuf_release_id = 0;
    return G_SOURCE_REMOVE;
}

static void
schedule_pixbuf_release (CsdBackgroundManager *m)
{
    g_clear_handle_id (&m->pixbuf_release_id, g_source_remove);
    m->pixbuf_release_id = g_timeout_add_seconds (PIXBUF_IDLE_RELEASE_SECONDS,
                                                  release_pixbufs_cb, m);
}

/* ---------- transition preference ---------- */

static BgTransition
read_transition_pref (void)
{
    GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default ();
    g_autoptr(GSettingsSchema) schema =
        g_settings_schema_source_lookup (schema_source, "org.cinnamon.muffin", TRUE);

    if (!schema)
        return BG_TRANSITION_BLEND;

    g_autoptr(GSettings) settings = g_settings_new ("org.cinnamon.muffin");
    g_autoptr(GVariant) v = g_settings_get_value (settings, "background-transition");
    const char *nick = g_variant_get_string (v, NULL);

    if (g_str_equal (nick, "none"))
        return BG_TRANSITION_NONE;
    if (g_str_equal (nick, "fade-in"))
        return BG_TRANSITION_FADE_IN;
    return BG_TRANSITION_BLEND;
}

static void
on_muffin_transition_changed (GSettings            *settings G_GNUC_UNUSED,
                               const char           *key      G_GNUC_UNUSED,
                               CsdBackgroundManager *m)
{
    m->transition = read_transition_pref ();
}

static void
setup_transition_pref (CsdBackgroundManager *m)
{
    GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default ();
    g_autoptr(GSettingsSchema) schema =
        g_settings_schema_source_lookup (schema_source, "org.cinnamon.muffin", TRUE);

    m->transition = BG_TRANSITION_BLEND;

    if (!schema)
        return;

    m->muffin_settings = g_settings_new ("org.cinnamon.muffin");
    m->transition = read_transition_pref ();

    g_signal_connect (m->muffin_settings, "changed::background-transition",
                      G_CALLBACK (on_muffin_transition_changed), m);
}

/* ---------- accountsservice ---------- */

static void
on_accountsservice_set_done (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data G_GNUC_UNUSED)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &err);
    if (err)
        g_debug ("accountsservice SetBackgroundFile failed: %s", err->message);
    else
        g_debug ("accountsservice login-screen background updated");
}

static void
on_accountsservice_find_user_done (GObject      *source,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
    char *path = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &err);

    if (err) {
        g_debug ("accountsservice FindUserByName failed: %s", err->message);
        g_free (path);
        return;
    }

    char *object_path = NULL;
    g_variant_get (ret, "(o)", &object_path);

    GDBusConnection *conn = g_dbus_proxy_get_connection (G_DBUS_PROXY (source));

    /* Try org.freedesktop.DisplayManager.AccountsService first */
    g_dbus_connection_call (conn,
                            "org.freedesktop.Accounts",
                            object_path,
                            "org.freedesktop.DBus.Properties",
                            "Set",
                            g_variant_new ("(ssv)",
                                           "org.freedesktop.DisplayManager.AccountsService",
                                           "BackgroundFile",
                                           g_variant_new_string (path ? path : "")),
                            NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL,
                            on_accountsservice_set_done,
                            NULL);

    g_free (object_path);
    g_free (path);
}

static void
on_accountsservice_proxy_ready (GObject      *source G_GNUC_UNUSED,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
    char *path = user_data;
    g_autoptr(GError) err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (result, &err);

    if (!proxy) {
        g_debug ("accountsservice proxy creation failed: %s", err ? err->message : "unknown");
        g_free (path);
        return;
    }

    g_dbus_proxy_call (proxy,
                       "FindUserByName",
                       g_variant_new ("(s)", g_get_user_name ()),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1, NULL,
                       on_accountsservice_find_user_done,
                       path);

    /* proxy kept alive until callback fires via the async chain */
    g_object_unref (proxy);
}

static void
set_accountsservice_background (const char *uri)
{
    if (!uri || uri[0] == '\0')
        return;

    g_autofree char *path = g_filename_from_uri (uri, NULL, NULL);

    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                              G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                              NULL,
                              "org.freedesktop.Accounts",
                              "/org/freedesktop/Accounts",
                              "org.freedesktop.Accounts",
                              NULL,
                              on_accountsservice_proxy_ready,
                              g_strdup (path ? path : uri));
}

/* ---------- placement name (debug) ---------- */

static const char *
placement_name (CinnamonBgPlacement p)
{
    switch (p) {
        case CINNAMON_BG_PLACEMENT_NONE:      return "none";
        case CINNAMON_BG_PLACEMENT_WALLPAPER: return "wallpaper";
        case CINNAMON_BG_PLACEMENT_CENTERED:  return "centered";
        case CINNAMON_BG_PLACEMENT_SCALED:    return "scaled";
        case CINNAMON_BG_PLACEMENT_STRETCHED: return "stretched";
        case CINNAMON_BG_PLACEMENT_ZOOM:      return "zoom";
        default:                     return "unknown";
    }
}

static const char *
transition_name (BgTransition t)
{
    switch (t) {
        case BG_TRANSITION_NONE:    return "none";
        case BG_TRANSITION_FADE_IN: return "fade-in";
        case BG_TRANSITION_BLEND:   return "blend";
        default:                    return "unknown";
    }
}

/* ---------- wayland drawing ---------- */

static void
compute_monitor_union (GPtrArray *windows,
                       int *out_x, int *out_y, int *out_w, int *out_h)
{
    int x1 = G_MAXINT, y1 = G_MAXINT, x2 = G_MININT, y2 = G_MININT;

    for (guint i = 0; i < windows->len; i++) {
        BgWaylandWindow *win = g_ptr_array_index (windows, i);
        GdkMonitor *monitor = bg_wayland_window_get_monitor (win);
        GdkRectangle geo;
        int scale = gdk_monitor_get_scale_factor (monitor);
        gdk_monitor_get_geometry (monitor, &geo);
        int mx2 = (geo.x + geo.width)  * scale;
        int my2 = (geo.y + geo.height) * scale;
        if (geo.x * scale < x1) x1 = geo.x * scale;
        if (geo.y * scale < y1) y1 = geo.y * scale;
        if (mx2 > x2)           x2 = mx2;
        if (my2 > y2)           y2 = my2;
    }

    *out_x = x1;
    *out_y = y1;
    *out_w = x2 - x1;
    *out_h = y2 - y1;
}

static void
draw_wayland_backgrounds (CsdBackgroundManager *m)
{
    if (!m->windows || m->windows->len == 0)
        return;

    BgRenderInput in;
    get_single_input (m, &in);

    int union_x = 0, union_y = 0, union_w = 0, union_h = 0;
    if (m->spanned)
        compute_monitor_union (m->windows, &union_x, &union_y, &union_w, &union_h);

    gboolean animate = !m->first_draw && (m->transition != BG_TRANSITION_NONE);
    m->first_draw = FALSE;

    g_debug ("Drawing Wayland backgrounds on %u monitor(s): placement=%s animate=%s",
             m->windows->len, placement_name (in.placement), animate ? "yes" : "no");

    for (guint i = 0; i < m->windows->len; i++) {
        BgWaylandWindow *win = g_ptr_array_index (m->windows, i);
        GdkMonitor *monitor = bg_wayland_window_get_monitor (win);
        GdkRectangle geo;
        int scale = gdk_monitor_get_scale_factor (monitor);
        gdk_monitor_get_geometry (monitor, &geo);
        int w = geo.width  * scale;
        int h = geo.height * scale;

        int span_origin_x, span_origin_y, span_total_w, span_total_h;
        if (m->spanned) {
            span_origin_x = geo.x * scale - union_x;
            span_origin_y = geo.y * scale - union_y;
            span_total_w  = union_w;
            span_total_h  = union_h;
        } else {
            span_origin_x = 0;
            span_origin_y = 0;
            span_total_w  = w;
            span_total_h  = h;
        }

        cairo_surface_t *surface = csd_background_render_region (
            m, gdk_monitor_get_connector (monitor), w, h,
            span_origin_x, span_origin_y, span_total_w, span_total_h);
        bg_wayland_window_set_image (win, surface, animate);
    }
}

static void
wayland_window_free (gpointer p)
{
    gtk_window_destroy (GTK_WINDOW (p));
    g_object_unref (p);
}

static void
mark_ready (CsdBackgroundManager *m)
{
    if (m->ready)
        return;
    m->ready = TRUE;
    g_debug ("Background ready");
    g_signal_emit (m, signals[SIGNAL_READY], 0);
}

static void
on_window_first_frame (BgWaylandWindow *win G_GNUC_UNUSED, CsdBackgroundManager *m)
{
    m->frames_painted++;
    g_debug ("Monitor painted first frame (%u/%u)",
             m->frames_painted, m->windows ? m->windows->len : 0);
    if (m->windows && m->frames_painted >= m->windows->len)
        mark_ready (m);
}

static void
setup_wayland_monitors (CsdBackgroundManager *m)
{
    if (m->windows)
        g_ptr_array_set_size (m->windows, 0);
    else
        m->windows = g_ptr_array_new_with_free_func (wayland_window_free);

    m->frames_painted = 0;

    GdkDisplay *display = gdk_display_get_default ();
    GListModel *monitors = gdk_display_get_monitors (display);
    guint n = g_list_model_get_n_items (monitors);

    for (guint i = 0; i < n; i++) {
        GdkMonitor *monitor = g_list_model_get_item (monitors, i);
        BgWaylandWindow *win = bg_wayland_window_new (monitor);
        g_signal_connect (win, "first-frame", G_CALLBACK (on_window_first_frame), m);
        g_ptr_array_add (m->windows, win);
        g_object_unref (monitor);
    }

    g_debug ("Set up %u Wayland background window(s)", m->windows->len);
}

/* ---------- draw dispatch ---------- */

static char *
describe_resolved (const BgRenderInput *in, const char *uri)
{
    if (uri && uri[0] != '\0')
        return g_strdup_printf ("image %s (%s)", uri, placement_name (in->placement));

    const char *kind = in->shading == CINNAMON_BG_SHADING_HORIZONTAL ? "hgradient"
                     : in->shading == CINNAMON_BG_SHADING_VERTICAL   ? "vgradient" : "color";
    g_autofree char *p = gdk_rgba_to_string (&in->primary);
    if (in->shading == CINNAMON_BG_SHADING_SOLID)
        return g_strdup_printf ("%s %s", kind, p);
    g_autofree char *s = gdk_rgba_to_string (&in->secondary);
    return g_strdup_printf ("%s %s -> %s", kind, p, s);
}

/* The current session's logical-monitor index for a connector, from
 * DisplayConfig (-1 if unknown / not yet loaded). Fed to resolve() so a stored
 * entry still matches after a connector rename across X11/Wayland. */
static int
monitor_index_for_connector (CsdBackgroundManager *m, const char *connector)
{
    if (!m->bg_monitors)
        return -1;

    GListModel *model = G_LIST_MODEL (m->bg_monitors);
    guint n = g_list_model_get_n_items (model);

    for (guint i = 0; i < n; i++) {
        g_autoptr(CinnamonBgMonitorInfo) info = g_list_model_get_item (model, i);
        g_autofree char *conn = NULL;
        int index;

        g_object_get (info, "connector", &conn, "index", &index, NULL);
        if (g_strcmp0 (conn, connector) == 0)
            return index;
    }

    return -1;
}

/* Resolve a BgRenderInput (with its pixbuf loaded) for every monitor, keyed by
 * connector. The CinnamonBgList does the matching: mirror/spanned applies one
 * item to all; independent matches each monitor (left-to-right) and lets unconfigured
 * monitors inherit a neighbour, falling back to the single item when nothing
 * matched. */
static void
build_resolved (CsdBackgroundManager *m)
{
    g_hash_table_remove_all (m->resolved);

    GdkDisplay *display = gdk_display_get_default ();
    GListModel *monitors = gdk_display_get_monitors (display);
    guint n = g_list_model_get_n_items (monitors);
    if (n == 0)
        return;

    typedef struct { GdkMonitor *gm; const char *connector; int x; } Mon;
    g_autofree Mon *mon = g_new0 (Mon, n);
    guint valid = 0;
    for (guint i = 0; i < n; i++) {
        GdkMonitor *gm = g_list_model_get_item (monitors, i);   /* owned ref */
        const char *connector = gdk_monitor_get_connector (gm);
        if (connector == NULL || connector[0] == '\0') {
            /* A monitor mid-(dis)connect (e.g. turning off for DPMS) can briefly
               report no connector; skip it rather than key the table on NULL. */
            g_object_unref (gm);
            continue;
        }
        GdkRectangle geo;
        gdk_monitor_get_geometry (gm, &geo);
        mon[valid].gm = gm;
        mon[valid].connector = connector;
        mon[valid].x = geo.x;
        valid++;
    }
    n = valid;
    if (n == 0)
        return;

    for (guint a = 0; a < n; a++)                                /* tiny n: sort by x */
        for (guint b = a + 1; b < n; b++)
            if (mon[b].x < mon[a].x) { Mon t = mon[a]; mon[a] = mon[b]; mon[b] = t; }

    const char **connectors = g_new0 (const char *, n);
    int *indices = g_new0 (int, n);
    for (guint i = 0; i < n; i++) {
        connectors[i] = mon[i].connector;
        indices[i] = monitor_index_for_connector (m, mon[i].connector);
    }

    g_autoptr(GListModel) items = cinnamon_bg_list_resolve (m->bglist, connectors, indices, n);

    /* Spanned is a mode, not a per-item placement: the whole desktop shows one
       image across the monitor union. */
    CinnamonBgMode mode;
    g_object_get (m->bglist, "mode", &mode, NULL);
    m->spanned = (mode == CINNAMON_BG_MODE_SPANNED);
    g_debug ("Resolving backgrounds: mode=%d, %u monitor(s)", mode, n);

    for (guint i = 0; i < n; i++) {
        g_autoptr(CinnamonBgItem) item = g_list_model_get_item (items, i);
        BgRenderInput *in = g_new0 (BgRenderInput, 1);
        const char *uri = item_to_render_input (m, item, in);
        g_hash_table_insert (m->resolved, g_strdup (mon[i].connector), in);

        g_autofree char *desc = describe_resolved (in, uri);
        g_debug ("  %s: %s", mon[i].connector, desc);
    }

    g_free (connectors);
    g_free (indices);
    for (guint i = 0; i < n; i++) g_object_unref (mon[i].gm);
}

static void
draw_background (CsdBackgroundManager *m)
{
    build_resolved (m);

    if (m->using_wayland)
        draw_wayland_backgrounds (m);
    else
        bg_x11_set_background (gdk_display_get_default (), m);

    prune_pixbuf_cache (m);
    schedule_pixbuf_release (m);
}

/* ---------- monitor hotplug ---------- */

static void
on_monitors_changed (GListModel           *model   G_GNUC_UNUSED,
                     guint                 position G_GNUC_UNUSED,
                     guint                 removed  G_GNUC_UNUSED,
                     guint                 added    G_GNUC_UNUSED,
                     CsdBackgroundManager *m)
{
    g_debug ("Monitors changed, rebuilding backgrounds");

    if (m->using_wayland)
        setup_wayland_monitors (m);

    draw_background (m);
}

static void
connect_monitors_signal (CsdBackgroundManager *m)
{
    GdkDisplay *display = gdk_display_get_default ();
    if (!display)
        return;

    m->monitors_model = gdk_display_get_monitors (display);
    m->monitors_changed_id = g_signal_connect (m->monitors_model,
                                               "items-changed",
                                               G_CALLBACK (on_monitors_changed),
                                               m);
}

static void
disconnect_monitors_signal (CsdBackgroundManager *m)
{
    if (m->monitors_model)
        g_clear_signal_handler (&m->monitors_changed_id, m->monitors_model);
    m->monitors_model = NULL;
}

/* ---------- source changed ---------- */

static void
on_bglist_changed (CinnamonBgList *list G_GNUC_UNUSED, CsdBackgroundManager *m)
{
    const char *uri = cinnamon_bg_item_get_picture_uri (cinnamon_bg_list_get_single (m->bglist));

    g_debug ("Background changed: single uri=%s", uri);

    set_accountsservice_background (uri);

    draw_background (m);
}

/* ---------- initial draw ---------- */

static void
setup_and_draw (CsdBackgroundManager *m)
{
    g_debug ("Performing initial draw");

    if (m->using_wayland) {
        m->first_draw = TRUE;
        setup_wayland_monitors (m);
    }

    draw_background (m);

    /* X11 composites synchronously into the root pixmap, so it is on screen as
     * soon as the draw returns. Wayland reports readiness asynchronously once
     * each layer surface has painted its first frame. */
    if (!m->using_wayland)
        mark_ready (m);

    set_accountsservice_background (
        cinnamon_bg_item_get_picture_uri (cinnamon_bg_list_get_single (m->bglist)));

    if (m->changed_id == 0)
        m->changed_id = g_signal_connect (m->bglist, "changed",
                                          G_CALLBACK (on_bglist_changed), m);
}

/* ---------- GObject ---------- */

static void
csd_background_manager_finalize (GObject *object)
{
    CsdBackgroundManager *m = CSD_BACKGROUND_MANAGER (object);

    disconnect_monitors_signal (m);

    if (m->bglist && m->changed_id) {
        g_signal_handler_disconnect (m->bglist, m->changed_id);
        m->changed_id = 0;
    }

    g_clear_object (&m->bglist);

    if (m->bg_monitors && m->bg_monitors_changed_id) {
        g_signal_handler_disconnect (m->bg_monitors, m->bg_monitors_changed_id);
        m->bg_monitors_changed_id = 0;
    }

    g_clear_object (&m->bg_monitors);

    if (m->muffin_settings) {
        g_signal_handlers_disconnect_by_func (m->muffin_settings,
                                              on_muffin_transition_changed, m);
        g_clear_object (&m->muffin_settings);
    }

    g_clear_handle_id (&m->pixbuf_release_id, g_source_remove);
    g_clear_pointer (&m->pixbuf_cache, g_hash_table_destroy);
    g_clear_pointer (&m->resolved, g_hash_table_destroy);
    g_clear_pointer (&m->windows, g_ptr_array_unref);

    G_OBJECT_CLASS (csd_background_manager_parent_class)->finalize (object);
}

static void
csd_background_manager_class_init (CsdBackgroundManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->finalize = csd_background_manager_finalize;

    /* Emitted once when the wallpaper is on screen across all monitors. */
    signals[SIGNAL_READY] =
        g_signal_new ("ready",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
csd_background_manager_init (CsdBackgroundManager *m)
{
    m->pixbuf_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, cached_image_free);
    m->resolved = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

CsdBackgroundManager *
csd_background_manager_new (void)
{
    return g_object_new (CSD_TYPE_BACKGROUND_MANAGER, NULL);
}

gboolean
csd_background_manager_start (CsdBackgroundManager *m, GError **error G_GNUC_UNUSED)
{
    g_debug ("Starting background manager");

#ifdef GDK_WINDOWING_WAYLAND
    m->using_wayland = GDK_IS_WAYLAND_DISPLAY (gdk_display_get_default ());
#else
    m->using_wayland = FALSE;
#endif

    g_debug ("Using %s backend",
             m->using_wayland ? "Wayland (layer-shell)" : "X11 (root pixmap)");

    setup_transition_pref (m);
    g_debug ("Transition mode: %s", transition_name (m->transition));

    connect_monitors_signal (m);

    m->bglist = cinnamon_bg_list_new ();

    m->bg_monitors = cinnamon_bg_monitors_new ();
    m->bg_monitors_changed_id = g_signal_connect_swapped (m->bg_monitors, "changed",
                                                          G_CALLBACK (draw_background), m);

    setup_and_draw (m);

    return TRUE;
}

void
csd_background_manager_stop (CsdBackgroundManager *m)
{
    g_debug ("Stopping background manager");

    disconnect_monitors_signal (m);

    if (m->bglist && m->changed_id) {
        g_signal_handler_disconnect (m->bglist, m->changed_id);
        m->changed_id = 0;
    }

    g_clear_object (&m->bglist);

    if (m->bg_monitors && m->bg_monitors_changed_id) {
        g_signal_handler_disconnect (m->bg_monitors, m->bg_monitors_changed_id);
        m->bg_monitors_changed_id = 0;
    }

    g_clear_object (&m->bg_monitors);

    if (m->muffin_settings) {
        g_signal_handlers_disconnect_by_func (m->muffin_settings,
                                              on_muffin_transition_changed, m);
        g_clear_object (&m->muffin_settings);
    }

    g_clear_handle_id (&m->pixbuf_release_id, g_source_remove);
    clear_pixbuf_cache (m);
    g_clear_pointer (&m->windows, g_ptr_array_unref);
}

/* ---------- public renderer API ---------- */

cairo_surface_t *
csd_background_render_region (CsdBackgroundManager *m,
                              const char *connector,
                              int width, int height,
                              int span_origin_x, int span_origin_y,
                              int span_total_w, int span_total_h)
{
    BgRenderInput *in = connector ? g_hash_table_lookup (m->resolved, connector) : NULL;
    BgRenderInput fallback;
    if (!in) {
        get_single_input (m, &fallback);
        in = &fallback;
    }

    cairo_surface_t *s =
        cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status (s) != CAIRO_STATUS_SUCCESS) {
        g_warning ("csd-background: failed to create %dx%d surface for %s: %s",
                   width, height, connector ? connector : "(single)",
                   cairo_status_to_string (cairo_surface_status (s)));
        return s;
    }

    cairo_t *cr = cairo_create (s);
    bg_renderer_paint (cr, in, width, height,
                       span_origin_x, span_origin_y, span_total_w, span_total_h);
    cairo_destroy (cr);
    return s;
}

gboolean
csd_background_is_spanned (CsdBackgroundManager *m)
{
    return m->spanned;
}
