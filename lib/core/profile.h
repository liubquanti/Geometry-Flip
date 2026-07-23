/* Player profile persistence: selected skin and per-official-level
   progress percentage, stored under /ext/geoflip/player. */
#pragma once

#include "../geoflip.h"

void save_player_profile(GeoApp* app);
void load_player_profile(GeoApp* app);

void save_official_progress(GeoApp* app);
void load_official_progress(GeoApp* app);
