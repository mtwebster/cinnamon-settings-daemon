#include "bg-geometry.h"
#include <glib.h>

BgRect
bg_geometry_image_rect (BgPlacement mode,
                        int src_w, int src_h,
                        int span_origin_x, int span_origin_y,
                        int span_total_w, int span_total_h)
{
    BgRect r = { 0, 0, 0, 0 };

    if (src_w <= 0 || src_h <= 0)
        return r;

    switch (mode) {
    case BG_PLACEMENT_STRETCHED:
        r.w = span_total_w;
        r.h = span_total_h;
        break;
    case BG_PLACEMENT_CENTERED:
        r.w = src_w;
        r.h = src_h;
        break;
    case BG_PLACEMENT_SCALED: {
        double s = MIN ((double) span_total_w / src_w,
                        (double) span_total_h / src_h);
        r.w = src_w * s;
        r.h = src_h * s;
        break;
    }
    case BG_PLACEMENT_ZOOM:
    case BG_PLACEMENT_SPANNED: {
        double s = MAX ((double) span_total_w / src_w,
                        (double) span_total_h / src_h);
        r.w = src_w * s;
        r.h = src_h * s;
        break;
    }
    case BG_PLACEMENT_NONE:
    case BG_PLACEMENT_WALLPAPER:
        return r;
    }

    /* center within the span total, then translate into this region's space */
    r.x = (span_total_w - r.w) / 2.0 - span_origin_x;
    r.y = (span_total_h - r.h) / 2.0 - span_origin_y;
    return r;
}
