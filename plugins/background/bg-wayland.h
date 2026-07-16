#pragma once

#include <gtk/gtk.h>
#include <cairo.h>

G_BEGIN_DECLS

#define CSD_BG_FADE_DURATION_MS 1500

#define BG_TYPE_WAYLAND_WINDOW (bg_wayland_window_get_type ())
G_DECLARE_FINAL_TYPE (BgWaylandWindow, bg_wayland_window, BG, WAYLAND_WINDOW, GtkWindow)

BgWaylandWindow *bg_wayland_window_new       (GdkMonitor *monitor);
GdkMonitor      *bg_wayland_window_get_monitor (BgWaylandWindow *win);
void             bg_wayland_window_set_image (BgWaylandWindow *win,
                                              cairo_surface_t *surface,
                                              gboolean         animate);

G_END_DECLS
