#pragma once

#include <gtk/gtk.h>
#include "bg-geometry.h"


typedef struct {
    CinnamonBgPlacement placement;
    CinnamonBgShading   shading;
    GdkRGBA     primary;
    GdkRGBA     secondary;
    GdkPixbuf  *source;      /* NULL when placement == NONE */
} BgRenderInput;

/* Paint into `cr` over the rect [0,0,width,height], where this region sits at
   (span_origin) within (span_total). Used for both an image surface (Wayland)
   and a cairo-xlib surface (X11). */
void bg_renderer_paint (cairo_t *cr, const BgRenderInput *in,
                        int width, int height,
                        int span_origin_x, int span_origin_y,
                        int span_total_w, int span_total_h);
