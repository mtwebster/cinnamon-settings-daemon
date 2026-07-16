#pragma once

#include <glib-object.h>
#include <cairo.h>
#include "bg-geometry.h"

G_BEGIN_DECLS

#define CSD_TYPE_BACKGROUND_MANAGER (csd_background_manager_get_type ())
G_DECLARE_FINAL_TYPE (CsdBackgroundManager, csd_background_manager, CSD, BACKGROUND_MANAGER, GObject)

CsdBackgroundManager *csd_background_manager_new            (void);
gboolean              csd_background_manager_start          (CsdBackgroundManager *manager, GError **error);
void                  csd_background_manager_stop           (CsdBackgroundManager *manager);

cairo_surface_t      *csd_background_render_region          (CsdBackgroundManager *manager,
                                                             const char *connector,
                                                             int width, int height,
                                                             int span_origin_x, int span_origin_y,
                                                             int span_total_w, int span_total_h);
gboolean              csd_background_is_spanned             (CsdBackgroundManager *manager);

G_END_DECLS
