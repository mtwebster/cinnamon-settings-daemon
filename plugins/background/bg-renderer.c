#include "bg-renderer.h"

static void
paint_color (cairo_t *cr, const BgRenderInput *in,
             int span_origin_x, int span_origin_y,
             int span_total_w, int span_total_h)
{
    if (in->shading == BG_SHADING_SOLID) {
        gdk_cairo_set_source_rgba (cr, &in->primary);
        cairo_paint (cr);
        return;
    }

    cairo_pattern_t *g;
    if (in->shading == BG_SHADING_HORIZONTAL)
        g = cairo_pattern_create_linear (-span_origin_x, 0,
                                         -span_origin_x + span_total_w, 0);
    else
        g = cairo_pattern_create_linear (0, -span_origin_y,
                                         0, -span_origin_y + span_total_h);

    cairo_pattern_add_color_stop_rgba (g, 0, in->primary.red, in->primary.green,
                                       in->primary.blue, in->primary.alpha);
    cairo_pattern_add_color_stop_rgba (g, 1, in->secondary.red, in->secondary.green,
                                       in->secondary.blue, in->secondary.alpha);
    cairo_set_source (cr, g);
    cairo_paint (cr);
    cairo_pattern_destroy (g);
}

void
bg_renderer_paint (cairo_t *cr, const BgRenderInput *in,
                   int width, int height,
                   int span_origin_x, int span_origin_y,
                   int span_total_w, int span_total_h)
{
    paint_color (cr, in, span_origin_x, span_origin_y, span_total_w, span_total_h);

    if (in->placement == BG_PLACEMENT_NONE || in->source == NULL)
        return;

    int src_w = gdk_pixbuf_get_width (in->source);
    int src_h = gdk_pixbuf_get_height (in->source);

    if (in->placement == BG_PLACEMENT_WALLPAPER) {
        /* tile from the span origin so multi-monitor tiling stays aligned */
        cairo_save (cr);
        cairo_translate (cr, -(span_origin_x % src_w), -(span_origin_y % src_h));
        gdk_cairo_set_source_pixbuf (cr, in->source, 0, 0);
        cairo_pattern_set_extend (cairo_get_source (cr), CAIRO_EXTEND_REPEAT);
        cairo_paint_with_alpha (cr, in->opacity);
        cairo_restore (cr);
        return;
    }

    BgRect r = bg_geometry_image_rect (in->placement, src_w, src_h,
                                       span_origin_x, span_origin_y,
                                       span_total_w, span_total_h);

    cairo_save (cr);
    cairo_translate (cr, r.x, r.y);
    cairo_scale (cr, r.w / src_w, r.h / src_h);
    gdk_cairo_set_source_pixbuf (cr, in->source, 0, 0);
    cairo_pattern_set_extend (cairo_get_source (cr), CAIRO_EXTEND_PAD);
    cairo_paint_with_alpha (cr, in->opacity);
    cairo_restore (cr);
}
