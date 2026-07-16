#include "config.h"

#include "bg-x11.h"
#include "bg-geometry.h"

#ifdef GDK_WINDOWING_X11

#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cairo-xlib.h>

static void
set_root_pixmap_id (Display *xdisplay, Window root, Pixmap pm)
{
    Atom esetroot_atom = XInternAtom (xdisplay, "ESETROOT_PMAP_ID", False);
    Atom rootpmap_atom = XInternAtom (xdisplay, "_XROOTPMAP_ID", False);

    Atom            type;
    int             format;
    unsigned long   nitems, bytes_after;
    unsigned char  *data = NULL;

    int result = XGetWindowProperty (xdisplay, root, esetroot_atom,
                                     0L, 1L, False, XA_PIXMAP,
                                     &type, &format, &nitems, &bytes_after,
                                     &data);

    if (data != NULL) {
        if (result == Success && type == XA_PIXMAP && format == 32 && nitems == 1) {
            GdkDisplay *gdk_display = gdk_display_get_default ();
            gdk_x11_display_error_trap_push (gdk_display);
            XKillClient (xdisplay, *(Pixmap *) data);
            gdk_x11_display_error_trap_pop_ignored (gdk_display);
        }
        XFree (data);
    }

    XChangeProperty (xdisplay, root, esetroot_atom, XA_PIXMAP,
                     32, PropModeReplace, (unsigned char *) &pm, 1);
    XChangeProperty (xdisplay, root, rootpmap_atom, XA_PIXMAP,
                     32, PropModeReplace, (unsigned char *) &pm, 1);
}

void
bg_x11_set_background (GdkDisplay *display, CsdBackgroundManager *manager)
{
    Display *xdisplay = GDK_DISPLAY_XDISPLAY (display);
    int      screen   = DefaultScreen (xdisplay);
    Window   root     = gdk_x11_display_get_xrootwindow (display);

    GListModel *monitors = gdk_display_get_monitors (display);
    guint n = g_list_model_get_n_items (monitors);

    if (n == 0)
        return;

    int x1 = G_MAXINT, y1 = G_MAXINT, x2 = G_MININT, y2 = G_MININT;
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item (monitors, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry (mon, &geo);
        if (geo.x < x1)              x1 = geo.x;
        if (geo.y < y1)              y1 = geo.y;
        if (geo.x + geo.width  > x2) x2 = geo.x + geo.width;
        if (geo.y + geo.height > y2) y2 = geo.y + geo.height;
        g_object_unref (mon);
    }

    int sw = x2 - x1;
    int sh = y2 - y1;

    gboolean spanned = csd_background_is_spanned (manager);

    g_debug ("Setting X11 root pixmap: %dx%d across %u monitor(s)", sw, sh, n);

    /* Create the pixmap on a throwaway connection set to RetainPermanent so the
     * server keeps it after we close that connection. XKillClient on the previous
     * pixmap then reaps only that throwaway client, never the main GDK connection. */
    const char *dname = gdk_display_get_name (display);
    Display *td = XOpenDisplay (dname);
    if (td == NULL) {
        g_warning ("csd-background: could not open X display '%s' to create root pixmap",
                   dname ? dname : "NULL");
        return;
    }
    XSetCloseDownMode (td, RetainPermanent);
    Pixmap pm = XCreatePixmap (td, RootWindow (td, screen), sw, sh,
                               DefaultDepth (td, screen));
    XCloseDisplay (td);

    cairo_surface_t *xs = cairo_xlib_surface_create (xdisplay, pm,
                                                     DefaultVisual (xdisplay, screen),
                                                     sw, sh);
    if (cairo_surface_status (xs) != CAIRO_STATUS_SUCCESS) {
        g_warning ("csd-background: cannot draw to %dx%d root pixmap: %s",
                   sw, sh, cairo_status_to_string (cairo_surface_status (xs)));
        cairo_surface_destroy (xs);

        /* Reap the orphaned RetainPermanent pixmap. */
        gdk_x11_display_error_trap_push (display);
        XKillClient (xdisplay, pm);
        gdk_x11_display_error_trap_pop_ignored (display);
        return;
    }

    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item (monitors, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry (mon, &geo);

        int w = geo.width;
        int h = geo.height;

        int span_origin_x, span_origin_y, span_total_w, span_total_h;
        if (spanned) {
            span_origin_x = geo.x - x1;
            span_origin_y = geo.y - y1;
            span_total_w  = sw;
            span_total_h  = sh;
        } else {
            span_origin_x = 0;
            span_origin_y = 0;
            span_total_w  = w;
            span_total_h  = h;
        }

        cairo_surface_t *region = csd_background_render_region (
            manager, gdk_monitor_get_connector (mon), w, h,
            span_origin_x, span_origin_y,
            span_total_w, span_total_h);

        cairo_t *cr = cairo_create (xs);
        cairo_set_source_surface (cr, region, geo.x - x1, geo.y - y1);
        cairo_paint (cr);
        cairo_destroy (cr);
        cairo_surface_destroy (region);
        g_object_unref (mon);
    }

    cairo_surface_flush (xs);

    gdk_x11_display_grab (display);

    set_root_pixmap_id (xdisplay, root, pm);

    XSetWindowBackgroundPixmap (xdisplay, root, pm);
    XClearWindow (xdisplay, root);

    gdk_display_flush (display);

    gdk_x11_display_ungrab (display);

    cairo_surface_destroy (xs);

    g_debug ("X11 root pixmap installed");
}

#else /* !GDK_WINDOWING_X11 */

void
bg_x11_set_background (GdkDisplay           *display G_GNUC_UNUSED,
                        CsdBackgroundManager *manager G_GNUC_UNUSED)
{
}

#endif /* GDK_WINDOWING_X11 */
