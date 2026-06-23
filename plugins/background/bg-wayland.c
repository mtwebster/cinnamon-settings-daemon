#include "config.h"

#include <gtk/gtk.h>
#include <cairo.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include "bg-wayland.h"

/* ---------- BgFader: ease-out-cubic GPU crossfade between two paintables ----------
 *
 * GtkStack's crossfade samples a *linear* progress, which lingers at the 50/50
 * mid-blend and reads as sluggish. muffin's X11 background transition eases the
 * same 1500ms with ease-out-cubic (clutter default), so we drive the crossfade
 * ourselves off the frame clock with that curve to match its feel. The blend is
 * still a single gsk_cross_fade_node, so the textures stay zero-copy dmabuf. */

#define BG_TYPE_FADER (bg_fader_get_type ())
G_DECLARE_FINAL_TYPE (BgFader, bg_fader, BG, FADER, GtkWidget)

struct _BgFader
{
    GtkWidget     parent_instance;

    GdkPaintable *prev;
    GdkPaintable *cur;
    double        progress;     /* linear [0,1]; eased at snapshot time */
    guint         duration_ms;
    gint64        start_time;
    guint         tick_id;
};

G_DEFINE_FINAL_TYPE (BgFader, bg_fader, GTK_TYPE_WIDGET)

/* From clutter-easing.c (Robert Penner, MIT) -- the same curve muffin/clutter
 * use for the X11 background transition. */
static inline double
ease_out_cubic (double t)
{
    double p = t - 1.0;
    return p * p * p + 1.0;
}

static gboolean
bg_fader_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data G_GNUC_UNUSED)
{
    BgFader *self = BG_FADER (widget);
    gint64 now = gdk_frame_clock_get_frame_time (clock);

    if (self->start_time == 0)
        self->start_time = now;

    double t = (double) (now - self->start_time) / (self->duration_ms * 1000.0);

    if (t >= 1.0) {
        self->progress = 1.0;
        g_clear_object (&self->prev);
        self->tick_id = 0;
        gtk_widget_queue_draw (widget);
        return G_SOURCE_REMOVE;
    }

    self->progress = t;
    gtk_widget_queue_draw (widget);
    return G_SOURCE_CONTINUE;
}

static void
bg_fader_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    BgFader *self = BG_FADER (widget);
    int w = gtk_widget_get_width (widget);
    int h = gtk_widget_get_height (widget);

    if (w <= 0 || h <= 0 || self->cur == NULL)
        return;

    if (self->prev == NULL || self->progress >= 1.0) {
        gdk_paintable_snapshot (self->cur, GDK_SNAPSHOT (snapshot), w, h);
        return;
    }

    gtk_snapshot_push_cross_fade (snapshot, ease_out_cubic (self->progress));
    gdk_paintable_snapshot (self->prev, GDK_SNAPSHOT (snapshot), w, h);
    gtk_snapshot_pop (snapshot);
    gdk_paintable_snapshot (self->cur, GDK_SNAPSHOT (snapshot), w, h);
    gtk_snapshot_pop (snapshot);
}

static void
bg_fader_dispose (GObject *object)
{
    BgFader *self = BG_FADER (object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object (&self->prev);
    g_clear_object (&self->cur);

    G_OBJECT_CLASS (bg_fader_parent_class)->dispose (object);
}

static void
bg_fader_class_init (BgFaderClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = bg_fader_dispose;
    GTK_WIDGET_CLASS (klass)->snapshot = bg_fader_snapshot;
}

static void
bg_fader_init (BgFader *self)
{
    self->duration_ms = CSD_BG_FADE_DURATION_MS;
    self->progress = 1.0;
}

static void
bg_fader_set_paintable (BgFader *self, GdkPaintable *paintable, gboolean animate)
{
    if (self->tick_id) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
        self->tick_id = 0;
    }

    if (!animate || self->cur == NULL) {
        g_clear_object (&self->prev);
        g_set_object (&self->cur, paintable);
        self->progress = 1.0;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
    }

    g_clear_object (&self->prev);
    self->prev = self->cur;            /* transfer the existing ref */
    self->cur = NULL;
    g_set_object (&self->cur, paintable);
    self->progress = 0.0;
    self->start_time = 0;
    self->tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                                  bg_fader_tick, NULL, NULL);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---------- BgWaylandWindow ---------- */

struct _BgWaylandWindow
{
    GtkWindow    parent_instance;

    GdkMonitor  *monitor;
    BgFader     *fader;
    gboolean     first_frame_done;
};

enum { SIGNAL_FIRST_FRAME, N_WIN_SIGNALS };
static guint win_signals[N_WIN_SIGNALS];

G_DEFINE_FINAL_TYPE (BgWaylandWindow, bg_wayland_window, GTK_TYPE_WINDOW)

static void
bg_wayland_window_class_init (BgWaylandWindowClass *klass)
{
    /* Emitted once, after this monitor's surface has painted its first frame
     * (i.e. the wallpaper is actually on screen for this output). */
    win_signals[SIGNAL_FIRST_FRAME] =
        g_signal_new ("first-frame",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
bg_wayland_window_init (BgWaylandWindow *win G_GNUC_UNUSED)
{
}

static void
on_after_paint (GdkFrameClock *clock, BgWaylandWindow *win)
{
    if (win->first_frame_done)
        return;
    win->first_frame_done = TRUE;
    g_signal_handlers_disconnect_by_func (clock, on_after_paint, win);
    g_signal_emit (win, win_signals[SIGNAL_FIRST_FRAME], 0);
}

static void
log_compositor_diagnostics (GtkWindow *win)
{
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (win));

    g_message ("Compositor: gtk4-layer-shell %u.%u.%u, wlr-layer-shell protocol v%u",
               gtk_layer_get_major_version (),
               gtk_layer_get_minor_version (),
               gtk_layer_get_micro_version (),
               gtk_layer_get_protocol_version ());

    GdkDmabufFormats *formats = gdk_display_get_dmabuf_formats (display);
    gsize n_formats = formats ? gdk_dmabuf_formats_get_n_formats (formats) : 0;

    GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (win));
    const char *renderer_name = renderer ? G_OBJECT_TYPE_NAME (renderer) : "(none)";
    gboolean software = renderer == NULL ||
                        g_strcmp0 (renderer_name, "GskCairoRenderer") == 0;

    g_message ("Rendering: %s, %" G_GSIZE_FORMAT " compositor dmabuf format(s) -> %s",
               renderer_name, n_formats,
               (!software && n_formats > 0) ? "GPU buffers (zero-copy dmabuf)"
                                            : "software/shm fallback");
}

static void
on_window_map (GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    BgWaylandWindow *win = BG_WAYLAND_WINDOW (widget);

    static gboolean logged = FALSE;
    if (!logged) {
        logged = TRUE;
        log_compositor_diagnostics (GTK_WINDOW (widget));
    }

    GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
    if (clock && !win->first_frame_done)
        g_signal_connect (clock, "after-paint", G_CALLBACK (on_after_paint), win);
}

BgWaylandWindow *
bg_wayland_window_new (GdkMonitor *monitor)
{
    BgWaylandWindow *win = g_object_ref_sink (
        g_object_new (BG_TYPE_WAYLAND_WINDOW,
                      "decorated", FALSE,
                      NULL));

    gtk_layer_init_for_window (GTK_WINDOW (win));
    gtk_layer_set_layer (GTK_WINDOW (win), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_namespace (GTK_WINDOW (win), "csd-background");
    gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (win), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_monitor (GTK_WINDOW (win), monitor);

    win->monitor = monitor;

    g_debug ("Created layer-shell background window for monitor %s",
             gdk_monitor_get_connector (monitor));

    win->fader = g_object_new (BG_TYPE_FADER, NULL);
    gtk_widget_set_hexpand (GTK_WIDGET (win->fader), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (win->fader), TRUE);

    gtk_window_set_child (GTK_WINDOW (win), GTK_WIDGET (win->fader));

    g_signal_connect (win, "map", G_CALLBACK (on_window_map), NULL);
    gtk_window_present (GTK_WINDOW (win));

    return win;
}

GdkMonitor *
bg_wayland_window_get_monitor (BgWaylandWindow *win)
{
    return win->monitor;
}

void
bg_wayland_window_set_image (BgWaylandWindow *win,
                              cairo_surface_t *surface,
                              gboolean         animate)
{
    int w = cairo_image_surface_get_width (surface);
    int h = cairo_image_surface_get_height (surface);
    cairo_surface_flush (surface);

    g_debug ("Setting %dx%d image on monitor %s (animate=%s)",
             w, h, gdk_monitor_get_connector (win->monitor), animate ? "yes" : "no");

    /* Back the texture directly with the cairo surface's pixels instead of
     * copying them: hand the surface to the GBytes free_func so it stays alive
     * exactly as long as the texture references it. */
    int stride = cairo_image_surface_get_stride (surface);
    GBytes *bytes = g_bytes_new_with_free_func (
        cairo_image_surface_get_data (surface),
        (gsize) stride * h,
        (GDestroyNotify) cairo_surface_destroy,
        surface);
    GdkTexture *tex = GDK_TEXTURE (gdk_memory_texture_new (
        w, h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride));
    g_bytes_unref (bytes);

    bg_fader_set_paintable (win->fader, GDK_PAINTABLE (tex), animate);
    g_object_unref (tex);
    /* surface ownership now belongs to the GBytes free_func */
}
