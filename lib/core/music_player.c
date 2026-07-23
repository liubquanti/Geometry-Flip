#include "music_player.h"

#include <furi.h>
#include <furi_hal_speaker.h>
#include <furi_hal_rtc.h>
#include <stdlib.h>
#include <string.h>

#include "../data/music.h"

/* ─── Background music (FMF-style note list) ─────────────────────────
   Notes are stored as a comma-separated token string, each token shaped
   like an FMF note: <duration><note|rest><sharp><octave><dots>. An
   optional leading duration-override digit(s), then either 'P'/'p' (rest)
   or a note letter A-G with an optional '#' (sharp) / 'b' (flat, a
   leniency beyond the official FMF grammar) and an optional trailing
   octave digit, followed by 0+ '.' dot characters (each dot multiplies
   the note's length by 1.5, compounding). Examples: "E6", "P", "4P",
   "F#", "Bb4", "4A#5.". Missing duration/octave fall back to the level's
   MUSIC BPM/DURATION/OCTAVE defaults. */

/* ─── Music note frequency LUT ───────────────────────────────────────
   12-tone equal temperament, A4 = 440Hz, indexed by MIDI note number
   (0..127). Avoids calling powf() at runtime. */
static const float NOTE_FREQ_HZ[128] = {
    8.18f, 8.66f, 9.18f, 9.72f, 10.30f, 10.91f, 11.56f, 12.25f,
    12.98f, 13.75f, 14.57f, 15.43f, 16.35f, 17.32f, 18.35f, 19.45f,
    20.60f, 21.83f, 23.12f, 24.50f, 25.96f, 27.50f, 29.14f, 30.87f,
    32.70f, 34.65f, 36.71f, 38.89f, 41.20f, 43.65f, 46.25f, 49.00f,
    51.91f, 55.00f, 58.27f, 61.74f, 65.41f, 69.30f, 73.42f, 77.78f,
    82.41f, 87.31f, 92.50f, 98.00f, 103.83f, 110.00f, 116.54f, 123.47f,
    130.81f, 138.59f, 146.83f, 155.56f, 164.81f, 174.61f, 185.00f, 196.00f,
    207.65f, 220.00f, 233.08f, 246.94f, 261.63f, 277.18f, 293.66f, 311.13f,
    329.63f, 349.23f, 369.99f, 392.00f, 415.30f, 440.00f, 466.16f, 493.88f,
    523.25f, 554.37f, 587.33f, 622.25f, 659.26f, 698.46f, 739.99f, 783.99f,
    830.61f, 880.00f, 932.33f, 987.77f, 1046.50f, 1108.73f, 1174.66f, 1244.51f,
    1318.51f, 1396.91f, 1479.98f, 1567.98f, 1661.22f, 1760.00f, 1864.66f, 1975.53f,
    2093.00f, 2217.46f, 2349.32f, 2489.02f, 2637.02f, 2793.83f, 2959.96f, 3135.96f,
    3322.44f, 3520.00f, 3729.31f, 3951.07f, 4186.01f, 4434.92f, 4698.64f, 4978.03f,
    5274.04f, 5587.65f, 5919.91f, 6271.93f, 6644.88f, 7040.00f, 7458.62f, 7902.13f,
    8372.02f, 8869.84f, 9397.27f, 9956.06f, 10548.08f, 11175.30f, 11839.82f, 12543.85f,
};

/* Pull the next comma-separated token out of `notes`. When `loop` is true,
   wraps back to the start once the end of the list is reached (menu tune,
   repeats forever); when false, signals `*out_end` instead of wrapping so
   the caller can stop playback after the last note (level tune, plays
   once). */
static bool music_next_token(const char* notes, uint16_t* cursor, char* out, int out_cap,
                              bool loop, bool* out_end) {
    *out_end = false;
    int len = (int)strlen(notes);
    if(len == 0) { *out_end = true; return false; }
    if(*cursor >= (uint16_t)len) {
        if(!loop) { *out_end = true; return false; }
        *cursor = 0;
    }

    int start = (int)*cursor;
    int i = start;
    while(i < len && notes[i] != ',') i++;
    int end = i;
    *cursor = (uint16_t)((i < len) ? i + 1 : len);

    while(start < end && notes[start] == ' ') start++;
    while(end > start && notes[end - 1] == ' ') end--;

    int tlen = end - start;
    if(tlen <= 0 || tlen >= out_cap) return false;
    memcpy(out, notes + start, (size_t)tlen);
    out[tlen] = '\0';
    return true;
}

/* Parse one note token into a frequency (0 = rest) and its duration in
   milliseconds. Returns false if the token was unusable (caller just
   retries next frame). */
static bool music_parse_token(const char* tok, int default_dur, int default_octave, int bpm,
                              float* out_freq, bool* out_rest, int* out_ms) {
    int i = 0;
    while(tok[i] >= '0' && tok[i] <= '9') i++;
    int dur = (i > 0) ? atoi(tok) : default_dur;
    if(dur <= 0) dur = default_dur > 0 ? default_dur : 8;

    bool is_rest = (tok[i] == 'P' || tok[i] == 'p');
    float freq = 0.0f;

    if(is_rest) {
        i++; /* consume the P */
    } else {
        char letter = tok[i];
        if(letter >= 'a' && letter <= 'g') letter = (char)(letter - 'a' + 'A');
        int semitone;
        switch(letter) {
        case 'C': semitone = 0;  break;
        case 'D': semitone = 2;  break;
        case 'E': semitone = 4;  break;
        case 'F': semitone = 5;  break;
        case 'G': semitone = 7;  break;
        case 'A': semitone = 9;  break;
        case 'B': semitone = 11; break;
        default:  semitone = -1; break;
        }
        if(semitone < 0) return false; /* unrecognized token */
        i++;
        int accidental = 0;
        if(tok[i] == '#')      { accidental = 1;  i++; }
        else if(tok[i] == 'b') { accidental = -1; i++; }

        int octave = default_octave > 0 ? default_octave : 5;
        if(tok[i] >= '0' && tok[i] <= '9') {
            octave = atoi(tok + i);
            while(tok[i] >= '0' && tok[i] <= '9') i++;
        }

        int midi = (octave + 1) * 12 + semitone + accidental;
        if(midi < 0) midi = 0;
        if(midi > 127) midi = 127;
        freq = NOTE_FREQ_HZ[midi];
    }

    /* FMF dotted notes: each trailing '.' multiplies the length by 1.5
       (compounding — "1.5^n" per the format spec, not the additive
       1+0.5+0.25… rule used in standard music notation). */
    int dots = 0;
    while(tok[i] == '.') { dots++; i++; }

    if(bpm <= 0) bpm = 120;
    /* whole note = 240/BPM seconds; this note = that / duration-denominator */
    float seconds = 240.0f / ((float)bpm * (float)dur);
    for(int d = 0; d < dots; d++) seconds *= 1.5f;
    int ms = (int)(seconds * 1000.0f + 0.5f);
    if(ms < 1) ms = 1;

    *out_freq = freq;
    *out_rest = is_rest;
    *out_ms   = ms;
    return true;
}

/* The Flipper's system-wide "Stealth Mode" (mute) toggle — held Down from
   the desktop, shown as a muted-speaker icon in the status bar. It only
   silences the OS's own notification sounds; anything using the speaker
   directly (as our music player does) has to check it itself. Exposed
   (not static) so the volume-overlay UI can tell this apart from the
   in-app volume being at 0 — Stealth Mode overrides sound_volume entirely
   and isn't something Up/Down can change, so the overlay shows a distinct
   "muted at the system level" state instead of the usual bar. */
bool music_system_muted(void) {
    return furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode);
}

/* Also honors the in-app volume slider (Up/Down in the main menu or pause
   menu) — volume 0 silences the speaker the same way. */
static bool music_muted(const GeoApp* app) {
    return app->sound_volume <= 0 || music_system_muted();
}

/* Speaker level for the current in-app volume setting, scaled against the
   track's base MUSIC_VOLUME. */
static float music_volume_level(const GeoApp* app) {
    return MUSIC_VOLUME * ((float)app->sound_volume / (float)SOUND_VOLUME_MAX);
}

/* Timed against the real-time tick counter rather than a frame count —
   the main loop's frame period is "furi_delay_ms(23) plus whatever
   rendering/physics took", which is never exactly 23ms, so counting
   frames drifts the tempo. furi_get_tick() sidesteps that. */
void music_update(GeoApp* app) {
    if(!app->music_active) return;
    uint32_t now = furi_get_tick();
    if(now < app->music_next_tick) return;

    char tok[MUSIC_TOKEN_MAX];
    bool end = false;
    if(!music_next_token(app->music_notes_src, &app->music_cursor, tok, sizeof(tok),
                          app->music_loop, &end)) {
        if(end) {
            /* reached the end of a non-looping (level) track — stop for good */
            app->music_active = false;
            if(app->music_acquired) furi_hal_speaker_stop();
            return;
        }
        app->music_next_tick = now + 1; /* skip a bad token, retry shortly */
        return;
    }

    float freq; bool rest; int ms;
    if(!music_parse_token(tok, app->music_src_duration, app->music_src_octave,
                          app->music_src_bpm, &freq, &rest, &ms)) {
        app->music_next_tick = now + 1;
        return;
    }

    /* Schedule the *next* note relative to this note's ideal start
       (music_next_tick), not to `now` — the main loop only polls once
       per frame, so `now` is always a little late (by however long the
       frame took) whenever a note boundary is noticed. Rebasing off
       `now` bakes that lateness into every single note transition,
       and it compounds over the whole track: a few late milliseconds
       per note times hundreds of notes adds up to seconds of drift,
       audibly dragging the tempo (this is why it was equally slower for
       both the level tune and the menu tune — same shared player).
       Only resync to `now` if we've fallen behind by more than this
       note's own length (e.g. just came out of the level intro delay,
       or a long pause) — otherwise a stall would fire every missed note
       in an instant burst trying to catch up. When that resync happens,
       count this note's full duration from `now` (rather than dropping
       it to `now` outright) — otherwise the note we just started playing
       gets an ~0ms deadline and is cut off on the very next frame, which
       is exactly what made the first note of a level's tune sound
       clipped (game_reset arms music_next_tick before the level-intro
       slide-in delay, so the very first note is always the one that
       falls behind). */
    uint32_t next_tick = app->music_next_tick + (uint32_t)ms;
    if(next_tick < now) next_tick = now + (uint32_t)ms;
    app->music_next_tick = next_tick;
    app->music_cur_freq  = freq;
    app->music_cur_rest  = rest;

    if(music_muted(app)) {
        if(app->music_acquired) furi_hal_speaker_stop();
        return;
    }
    if(!app->music_acquired) {
        if(furi_hal_speaker_acquire(1000)) app->music_acquired = true;
    }
    if(app->music_acquired) {
        if(rest) furi_hal_speaker_stop();
        else furi_hal_speaker_start(freq, music_volume_level(app));
    }
}

void music_pause(GeoApp* app) {
    if(app->music_acquired) furi_hal_speaker_stop();
    app->music_pause_started = furi_get_tick();
}

/* Re-applies the current volume to whatever note is sounding right now, so
   an Up/Down volume adjustment in the main menu is heard immediately
   instead of waiting for the next note boundary. Only safe to call while
   music_update() is still ticking every frame (i.e. the menu tune) — the
   note gets naturally superseded at the next note boundary. Do NOT use
   this while GAMESTATE_PAUSE: music_update() isn't running there, so the
   note it restarts would never get stopped again, playing forever; use
   music_play_volume_beep() for that case instead. */
void music_apply_volume(GeoApp* app) {
    if(!app->music_acquired) return;
    if(music_muted(app)) {
        furi_hal_speaker_stop();
        return;
    }
    if(!app->music_cur_rest) furi_hal_speaker_start(app->music_cur_freq, music_volume_level(app));
}

/* Short, self-terminating feedback blip for Up/Down volume adjustments
   made from the pause menu, where music_update() isn't ticking (gameplay
   is frozen) so nothing would ever call furi_hal_speaker_stop() on a note
   restarted there — call music_tick_volume_beep() every frame afterward
   to cut it off after VOLUME_BEEP_MS. */
void music_play_volume_beep(GeoApp* app) {
    if(music_muted(app)) return;
    if(!app->music_acquired) {
        if(!furi_hal_speaker_acquire(1000)) return;
        app->music_acquired = true;
    }
    float freq = app->music_cur_freq > 0.0f ? app->music_cur_freq : 659.26f; /* E5 fallback */
    furi_hal_speaker_start(freq, music_volume_level(app));
    app->volume_beep_until_tick = furi_get_tick() + VOLUME_BEEP_MS;
}

/* Cuts off a pending music_play_volume_beep() once its deadline passes.
   Caller (main loop) is expected to only invoke this while GAMESTATE_PAUSE
   is active — see music_play_volume_beep's comment for why. */
void music_tick_volume_beep(GeoApp* app) {
    if(app->volume_beep_until_tick == 0) return;
    if(furi_get_tick() >= app->volume_beep_until_tick) {
        if(app->music_acquired) furi_hal_speaker_stop();
        app->volume_beep_until_tick = 0;
    }
}

void music_resume(GeoApp* app) {
    app->volume_beep_until_tick = 0; /* any pending pause-menu beep is moot now */
    if(!app->music_active) return;
    app->music_next_tick += furi_get_tick() - app->music_pause_started;
    if(music_muted(app)) return;
    if(!app->music_acquired) {
        if(furi_hal_speaker_acquire(1000)) app->music_acquired = true;
    }
    if(app->music_acquired && !app->music_cur_rest) {
        furi_hal_speaker_start(app->music_cur_freq, music_volume_level(app));
    }
}

void music_release(GeoApp* app) {
    if(app->music_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        app->music_acquired = false;
    }
}

/* Pull the BPM/Duration/Octave header fields and the Notes: token list out
   of a track exported in real Flipper Music Format (as embedded in
   lib/data/music.c) — distinct from the simplified "MUSIC BPM=.. / NOTE .."
   directives level files use, hence the separate parser. */
static void music_load_fmf(const char* fmf, int16_t* bpm, int16_t* duration,
                            int16_t* octave, const char** notes) {
    *bpm = 0; *duration = 8; *octave = 5; *notes = "";
    const char* p;
    if((p = strstr(fmf, "BPM:")))      *bpm      = (int16_t)atoi(p + 4);
    if((p = strstr(fmf, "Duration:"))) *duration = (int16_t)atoi(p + 9);
    if((p = strstr(fmf, "Octave:")))   *octave   = (int16_t)atoi(p + 7);
    if((p = strstr(fmf, "Notes:")))    *notes    = p + 6;
}

/* Silences whatever note the previous track (the menu tune) left
   sounding — otherwise, if a note happened to be mid-tone at the switch,
   it would keep ringing until the new track's own playback got around to
   overwriting it (or forever, if the level has no music of its own). */
void music_use_level_track(GeoApp* app) {
    if(app->music_acquired) furi_hal_speaker_stop();
    app->music_notes_src    = app->level.music_notes;
    app->music_src_bpm      = app->level.music_bpm;
    app->music_src_duration = app->level.music_duration;
    app->music_src_octave   = app->level.music_octave;
    app->music_active       = (app->music_src_bpm > 0 && app->music_notes_src[0] != '\0');
    app->music_loop         = false; /* level tune plays once, then stays silent */
    app->music_cursor       = 0;
    app->music_next_tick    = furi_get_tick();
    app->music_cur_freq     = 0.0f;
    app->music_cur_rest     = true;
}

/* Same stuck-note guard as music_use_level_track, mirrored for the
   reverse switch (leaving a level back to a menu). */
void music_use_menu_track(GeoApp* app) {
    static const char* notes;
    static int16_t     bpm, duration, octave;
    static bool         parsed = false;
    if(!parsed) {
        music_load_fmf(MAIN_MENU_MUSIC, &bpm, &duration, &octave, &notes);
        parsed = true;
    }
    if(app->music_acquired) furi_hal_speaker_stop();
    app->music_notes_src    = notes;
    app->music_src_bpm      = bpm;
    app->music_src_duration = duration;
    app->music_src_octave   = octave;
    app->music_active       = (bpm > 0 && notes[0] != '\0');
    app->music_loop         = true; /* menu tune repeats continuously */
    app->music_cursor       = 0;
    app->music_next_tick    = furi_get_tick();
    app->music_cur_freq     = 0.0f;
    app->music_cur_rest     = true;
}
