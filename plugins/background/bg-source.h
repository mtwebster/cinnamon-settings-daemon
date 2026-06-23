#pragma once

#include <glib-object.h>
#include "bg-renderer.h"

#define BG_TYPE_SOURCE (bg_source_get_type ())
G_DECLARE_FINAL_TYPE (BgSource, bg_source, BG, SOURCE, GObject)

BgSource   *bg_source_new             (void);
void        bg_source_get_input       (BgSource *self, BgRenderInput *in);
const char *bg_source_get_uri         (BgSource *self);
const char *bg_source_get_uri_for_monitor (BgSource *self, guint monitor_index);
