#include "bg-geometry.h"
#include <glib.h>

static void
test_stretched (void)
{
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_STRETCHED, 800, 600,
                                       0, 0, 1920, 1080);
    g_assert_cmpfloat (r.x, ==, 0);
    g_assert_cmpfloat (r.y, ==, 0);
    g_assert_cmpfloat (r.w, ==, 1920);
    g_assert_cmpfloat (r.h, ==, 1080);
}

static void
test_centered (void)
{
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_CENTERED, 800, 600,
                                       0, 0, 1920, 1080);
    g_assert_cmpfloat (r.w, ==, 800);
    g_assert_cmpfloat (r.h, ==, 600);
    g_assert_cmpfloat (r.x, ==, (1920 - 800) / 2.0);
    g_assert_cmpfloat (r.y, ==, (1080 - 600) / 2.0);
}

static void
test_scaled_fits_letterbox (void)
{
    /* 800x600 (4:3) into 1920x1080 (16:9): height-bound, scale = 1.8 */
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_SCALED, 800, 600,
                                       0, 0, 1920, 1080);
    g_assert_cmpfloat (r.h, ==, 1080);
    g_assert_cmpfloat (r.w, ==, 1440);
    g_assert_cmpfloat (r.x, ==, (1920 - 1440) / 2.0);
    g_assert_cmpfloat (r.y, ==, 0);
}

static void
test_zoom_fills_crop (void)
{
    /* 800x600 into 1920x1080: width-bound, scale = 2.4, height 1440 overflows */
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_ZOOM, 800, 600,
                                       0, 0, 1920, 1080);
    g_assert_cmpfloat (r.w, ==, 1920);
    g_assert_cmpfloat (r.h, ==, 1440);
    g_assert_cmpfloat (r.x, ==, 0);
    g_assert_cmpfloat (r.y, ==, (1080 - 1440) / 2.0);
}

static void
test_spanned_offsets_second_monitor (void)
{
    /* Spanning is a background-mode rather than a placement: the manager zooms
       the image to the monitor union and hands each monitor its slice via the
       span origin/total, so the placement passed here is plain zoom.
       Two 1920x1080 monitors side by side: span total 3840x1080. The rect is
       expressed relative to the second monitor (origin 1920,0). Source
       3840x1080 -> scale 1.0, so on monitor 2 the rect starts at x = -1920. */
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_ZOOM, 3840, 1080,
                                       1920, 0, 3840, 1080);
    g_assert_cmpfloat (r.w, ==, 3840);
    g_assert_cmpfloat (r.h, ==, 1080);
    g_assert_cmpfloat (r.x, ==, -1920);
    g_assert_cmpfloat (r.y, ==, 0);
}

static void
test_none_returns_zero (void)
{
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_NONE, 800, 600, 0, 0, 1920, 1080);
    g_assert_cmpfloat (r.w, ==, 0);
    g_assert_cmpfloat (r.h, ==, 0);
}

static void
test_wallpaper_returns_zero (void)
{
    BgRect r = bg_geometry_image_rect (CINNAMON_BG_PLACEMENT_WALLPAPER, 800, 600, 0, 0, 1920, 1080);
    g_assert_cmpfloat (r.w, ==, 0);
    g_assert_cmpfloat (r.h, ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/geometry/stretched", test_stretched);
    g_test_add_func ("/geometry/centered", test_centered);
    g_test_add_func ("/geometry/scaled", test_scaled_fits_letterbox);
    g_test_add_func ("/geometry/zoom", test_zoom_fills_crop);
    g_test_add_func ("/geometry/spanned", test_spanned_offsets_second_monitor);
    g_test_add_func ("/geometry/none", test_none_returns_zero);
    g_test_add_func ("/geometry/wallpaper", test_wallpaper_returns_zero);
    return g_test_run ();
}
