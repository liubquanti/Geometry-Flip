/* All screen drawing: gameplay world, HUD/overlays, splash, main menu,
   skin carousel and official-levels carousel. One draw callback per the
   Flipper GUI's fullscreen ViewPort model. */
#pragma once

#include <gui/gui.h>
#include "../geoflip.h"

void render_callback(Canvas* canvas, void* ctx);
