#pragma once

typedef enum {
    BG_PLACEMENT_NONE,
    BG_PLACEMENT_WALLPAPER,   /* tile  */
    BG_PLACEMENT_CENTERED,
    BG_PLACEMENT_SCALED,      /* fit, preserve aspect */
    BG_PLACEMENT_STRETCHED,   /* fill, ignore aspect  */
    BG_PLACEMENT_ZOOM,        /* fill, preserve aspect, crop */
    BG_PLACEMENT_SPANNED
} BgPlacement;

typedef struct { double x, y, w, h; } BgRect;

/* Destination rect (in target pixels) for the image under `mode`, given the
   source pixbuf size and the *span* rect (for non-spanned modes the span
   rect equals the target: origin 0,0 and total == target w/h). Caller clips
   to the target. WALLPAPER/NONE return {0,0,0,0}; callers handle them. */
BgRect bg_geometry_image_rect (BgPlacement mode,
                               int src_w, int src_h,
                               int span_origin_x, int span_origin_y,
                               int span_total_w, int span_total_h);
