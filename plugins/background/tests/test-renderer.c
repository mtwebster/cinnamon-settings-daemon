#include "bg-renderer.h"
#include <gtk/gtk.h>

static guint32
pixel_at (cairo_surface_t *s, int x, int y)
{
    cairo_surface_flush (s);
    unsigned char *d = cairo_image_surface_get_data (s);
    int stride = cairo_image_surface_get_stride (s);
    return *(guint32 *) (d + y * stride + x * 4);   /* ARGB32, premultiplied */
}

static void
test_solid_fills (void)
{
    cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_t *cr = cairo_create (s);
    BgRenderInput in = { BG_PLACEMENT_NONE, BG_SHADING_SOLID,
                         { 1, 0, 0, 1 }, { 0, 0, 0, 1 }, 1.0, NULL };
    bg_renderer_paint (cr, &in, 64, 64, 0, 0, 64, 64);
    guint32 p = pixel_at (s, 32, 32);
    g_assert_cmphex ((p >> 16) & 0xff, ==, 0xff);   /* red */
    g_assert_cmphex ((p >> 8) & 0xff, ==, 0x00);
    g_assert_cmphex (p & 0xff, ==, 0x00);
    cairo_destroy (cr);
    cairo_surface_destroy (s);
}

static void
test_horizontal_gradient_direction (void)
{
    cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_t *cr = cairo_create (s);
    BgRenderInput in = { BG_PLACEMENT_NONE, BG_SHADING_HORIZONTAL,
                         { 1, 0, 0, 1 }, { 0, 0, 1, 1 }, 1.0, NULL };
    bg_renderer_paint (cr, &in, 64, 64, 0, 0, 64, 64);
    guint32 left = pixel_at (s, 2, 32);
    guint32 right = pixel_at (s, 61, 32);
    g_assert_cmpint ((left >> 16) & 0xff, >, 0xf0);   /* red dominant on left */
    g_assert_cmpint (left & 0xff, <, 0x10);            /* little blue on left */
    g_assert_cmpint (right & 0xff, >, 0xf0);           /* blue dominant on right */
    g_assert_cmpint ((right >> 16) & 0xff, <, 0x10);   /* little red on right */
    cairo_destroy (cr);
    cairo_surface_destroy (s);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gtk_init ();
    g_test_add_func ("/renderer/solid", test_solid_fills);
    g_test_add_func ("/renderer/hgradient", test_horizontal_gradient_direction);
    return g_test_run ();
}
