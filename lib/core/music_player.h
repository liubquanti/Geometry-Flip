/* Background music playback (FMF-style note list) — one shared player fed
   by whichever track is currently selected: the level's tune while
   playing, the main-menu tune everywhere else. Playback is driven once
   per frame from the main loop, no blocking delays or worker thread. */
#pragma once

#include "../geoflip.h"

/* Advance the tune; only actually does work once the current note's
   duration has elapsed (timed against furi_get_tick(), not frame count). */
void music_update(GeoApp* app);

/* True when the Flipper's system-wide Stealth Mode (mute) toggle is on.
   Distinct from app->sound_volume being 0 — this overrides it, and the
   in-app volume overlay uses this to show a dedicated "muted at the
   system level" state instead of the usual bar. */
bool music_system_muted(void);

/* Silence the speaker (e.g. on pause/death/win) without releasing
   ownership, so resuming is instant. */
void music_pause(GeoApp* app);

/* Re-applies app->sound_volume to whatever note is currently sounding, for
   instant feedback while the user holds Up/Down on the volume overlay.
   Only use this where music_update() keeps ticking every frame (the main
   menu) — see music_play_volume_beep() for the paused-gameplay case. */
void music_apply_volume(GeoApp* app);

/* Short, self-terminating feedback blip for volume adjustments made while
   gameplay is frozen (GAMESTATE_PAUSE), where nothing else would ever
   silence a note restarted by music_apply_volume. Call
   music_tick_volume_beep() every frame afterward to cut it off. */
void music_play_volume_beep(GeoApp* app);
void music_tick_volume_beep(GeoApp* app);

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
