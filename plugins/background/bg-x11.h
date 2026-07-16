#pragma once

#include <gdk/gdk.h>
#include "csd-background-manager.h"

G_BEGIN_DECLS

void bg_x11_set_background (GdkDisplay *display, CsdBackgroundManager *manager);

G_END_DECLS
