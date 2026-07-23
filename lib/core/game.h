/* Core gameplay: physics, scrolling, object collision, death/win
   transitions, and the death-particle burst. Drives (and is driven by)
   the per-frame main loop in lib/geoflip.c. */
#pragma once

#include <gui/gui.h>
#include "../geoflip.h"

/* Reset all per-attempt gameplay state (physics, camera, sliding window,
   death particles) and (re)start the level's music track. Does NOT
   re-parse app->level — callers that need a fresh level call
   game_start_level/game_start_official_level instead. */
void game_reset(GeoApp* app);

void death_particles_update(GeoApp* app);
void death_particles_draw(Canvas* canvas, const GeoApp* app);

/* Current progress through the level, as a 0..100 percentage of length. */
int game_pct(const GeoApp* app);

/* Load a custom level (by index into app->level_files) and enter
   GAMESTATE_PLAYING. No-op if idx is out of range or parsing fails. */
void game_start_level(GeoApp* app, int idx);

/* Load an embedded official level (by index into OFFICIAL_LEVELS) and
   enter GAMESTATE_PLAYING. No-op if idx is out of range or parsing fails. */
void game_start_official_level(GeoApp* app, int idx);

/* Restart the already-parsed current level (used after death) without
   re-reading/re-parsing it from storage. */
void game_restart_current_level(GeoApp* app);

/* Advance gameplay physics/collision by one frame. No-op unless
   app->state == GAMESTATE_PLAYING. */
void game_update(GeoApp* app);
