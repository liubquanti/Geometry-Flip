/* Background music playback (FMF-style note list) — one shared player fed
   by whichever track is currently selected: the level's tune while
   playing, the main-menu tune everywhere else. Playback is driven once
   per frame from the main loop, no blocking delays or worker thread. */
#pragma once

#include "../geoflip.h"

/* Advance the tune; only actually does work once the current note's
   duration has elapsed (timed against furi_get_tick(), not frame count). */
void music_update(GeoApp* app);

/* Silence the speaker (e.g. on pause/death/win) without releasing
   ownership, so resuming is instant. */
void music_pause(GeoApp* app);

/* Re-sound whatever note was playing when paused, and push the next-note
   deadline forward by the pause duration. */
void music_resume(GeoApp* app);

/* Fully give up the speaker (leaving gameplay for a menu, or app exit). */
void music_release(GeoApp* app);

/* Point the shared player at the current level's embedded tune (called
   from game_reset whenever a level (re)starts). */
void music_use_level_track(GeoApp* app);

/* Point the shared player at the main-menu tune (splash, main menu, skins,
   official/custom level lists — anywhere outside of gameplay). */
void music_use_menu_track(GeoApp* app);
