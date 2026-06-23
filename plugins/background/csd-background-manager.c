#include "config.h"

#include <gio/gio.h>
#include <gdk/gdk.h>
#include <cairo.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include "csd-background-manager.h"
#include "bg-source.h"
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

    BgSource    *source;
    gulong       changed_id;

    /* uri (char *) -> CachedImage *; one entry per distinct image in use.
     * Source pixbufs are only needed while rendering; the crossfade and surface
     * re-exposes run off the already-rendered textures. So we drop them a few
     * seconds after the last draw to keep steady-state RSS down, while staying
     * warm through a burst of changes (e.g. dragging the opacity slider). */
    GHashTable  *pixbuf_cache;
    guint        pixbuf_release_id;

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

static guint
get_monitor_count (CsdBackgroundManager *m)
{
    if (m->using_wayland)
        return m->windows ? m->windows->len : 0;

    GdkDisplay *display = gdk_display_get_default ();
    return display ? g_list_model_get_n_items (gdk_display_get_monitors (display)) : 0;
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

/* Drop cached images no longer assigned to any monitor (e.g. after a wallpaper
 * change or a slideshow tick) so the cache stays bounded to what is on screen. */
static void
prune_pixbuf_cache (CsdBackgroundManager *m)
{
    guint n = get_monitor_count (m);
    g_autoptr(GHashTable) keep = g_hash_table_new (g_str_hash, g_str_equal);

    for (guint i = 0; i < n; i++) {
        const char *uri = bg_source_get_uri_for_monitor (m->source, i);
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
placement_name (BgPlacement p)
{
    switch (p) {
        case BG_PLACEMENT_NONE:      return "none";
        case BG_PLACEMENT_WALLPAPER: return "wallpaper";
        case BG_PLACEMENT_CENTERED:  return "centered";
        case BG_PLACEMENT_SCALED:    return "scaled";
        case BG_PLACEMENT_STRETCHED: return "stretched";
        case BG_PLACEMENT_ZOOM:      return "zoom";
        case BG_PLACEMENT_SPANNED:   return "spanned";
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
    bg_source_get_input (m->source, &in);

    int union_x = 0, union_y = 0, union_w = 0, union_h = 0;
    if (in.placement == BG_PLACEMENT_SPANNED)
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
        if (in.placement == BG_PLACEMENT_SPANNED) {
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
            m, i, w, h, span_origin_x, span_origin_y, span_total_w, span_total_h);
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

static void
draw_background (CsdBackgroundManager *m)
{
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
on_source_changed (BgSource *source G_GNUC_UNUSED, CsdBackgroundManager *m)
{
    BgRenderInput in;
    bg_source_get_input (m->source, &in);

    g_debug ("Background changed: uri=%s placement=%s",
             bg_source_get_uri_for_monitor (m->source, 0),
             placement_name (in.placement));

    set_accountsservice_background (bg_source_get_uri_for_monitor (m->source, 0));

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

    set_accountsservice_background (bg_source_get_uri_for_monitor (m->source, 0));

    if (m->changed_id == 0)
        m->changed_id = g_signal_connect (m->source, "changed",
                                          G_CALLBACK (on_source_changed), m);
}

/* ---------- GObject ---------- */

static void
csd_background_manager_finalize (GObject *object)
{
    CsdBackgroundManager *m = CSD_BACKGROUND_MANAGER (object);

    disconnect_monitors_signal (m);

    if (m->source && m->changed_id) {
        g_signal_handler_disconnect (m->source, m->changed_id);
        m->changed_id = 0;
    }

    g_clear_object (&m->source);

    if (m->muffin_settings) {
        g_signal_handlers_disconnect_by_func (m->muffin_settings,
                                              on_muffin_transition_changed, m);
        g_clear_object (&m->muffin_settings);
    }

    g_clear_handle_id (&m->pixbuf_release_id, g_source_remove);
    g_clear_pointer (&m->pixbuf_cache, g_hash_table_destroy);
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

    m->source = bg_source_new ();

    setup_and_draw (m);

    return TRUE;
}

void
csd_background_manager_stop (CsdBackgroundManager *m)
{
    g_debug ("Stopping background manager");

    disconnect_monitors_signal (m);

    if (m->source && m->changed_id) {
        g_signal_handler_disconnect (m->source, m->changed_id);
        m->changed_id = 0;
    }

    g_clear_object (&m->source);

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
                              guint monitor_index,
                              int width, int height,
                              int span_origin_x, int span_origin_y,
                              int span_total_w, int span_total_h)
{
    BgRenderInput in;
    bg_source_get_input (m->source, &in);
    in.source = get_pixbuf_for_uri (m, bg_source_get_uri_for_monitor (m->source, monitor_index));

    cairo_surface_t *s =
        cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create (s);
    bg_renderer_paint (cr, &in, width, height,
                       span_origin_x, span_origin_y, span_total_w, span_total_h);
    cairo_destroy (cr);
    return s;
}

BgPlacement
csd_background_get_placement (CsdBackgroundManager *m)
{
    BgRenderInput in;
    bg_source_get_input (m->source, &in);
    return in.placement;
}
