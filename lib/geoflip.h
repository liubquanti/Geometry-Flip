/**
 * Geometry Flip for Flipper Zero — shared types and constants.
 *
 * This header holds everything that crosses module boundaries: the game
 * constants, the level/entity data types, and the top-level GeoApp state
 * struct. See lib/core/ for the actual subsystems (level parsing,
 * collision, physics/game logic, music playback, rendering, persistence).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ─── Constants ─────────────────────────────────────────────────── */

#define SCREEN_W        128
#define SCREEN_H        64
#define GROUND_Y        54
#define PLAYER_GX       2
#define PLAYER_SIZE     8
#define CELL            8
#define GRAVITY         0.3f
#define JUMP_VEL        (-3.2f)
#define SCROLL_SPEED    2
#define MAX_OBJECTS     512
#define MAX_DECORATIONS 32
#define LEVEL_DIR       "/ext/geoflip/levels"
#define MAX_LEVELS      16
#define MAX_FILENAME    64
#define MAX_PATH_LEN    256
#define ROT_SPEED_AIR   9.0f
#define ROT_SNAP_SPEED  18.0f
#define DEATH_PARTICLES 24
#define CAM_Y_TRIGGER_ROWS 4   /* camera starts rising once player climbs above this many blocks */
#define CAM_Y_SMOOTH    0.15f  /* lerp factor per frame for smooth vertical follow */
#define MUSIC_NOTES_MAX 10000    /* max length of the comma-separated note-list string */
#define MUSIC_TOKEN_MAX 24     /* max length of a single note token, e.g. "4F#6" */
#define MUSIC_VOLUME    1.0f
#define INTRO_HIDE_FRAMES 44  /* ~1s at 23ms per frame */
#define INTRO_ENTRY_FRAMES 20 /* player slides in before camera starts */
#define MENU_CUBE_PERIOD_FRAMES 174 /* ~4s at 23ms per frame */
#define MENU_JUMP_PERIOD_FRAMES 43  /* ~1s at 23ms per frame */
#define MENU_CUBE_SPEED ((float)(SCREEN_W + PLAYER_SIZE * 20) / MENU_CUBE_PERIOD_FRAMES)

/* How many cells ahead/behind to check each frame (must cover 1 full screen) */
#define WINDOW_CELLS    (SCREEN_W / CELL + 2)   /* 18 */

#define GRID_TO_SCREEN_Y(gy) (GROUND_Y - ((gy) + 1) * CELL)
#define GRID_TO_LEVEL_X(gx)  ((gx) * CELL)

/* ─── Types ─────────────────────────────────────────────────────── */

typedef enum {
    OBJ_BLOCK = 0, OBJ_SPIKE, OBJ_MINI_SPIKE, OBJ_MINI_BLOCK,
    OBJ_JUMPER, OBJ_SPHERE
} ObjType;
typedef enum { DEC_STAR = 0, DEC_CLOUD, DEC_PILLAR } DecType;

typedef struct {
    ObjType type;
    int16_t gx;
    int16_t gy;
    int8_t  rot; /* 0..3 => 0,90,180,270 degrees */
} LvlObject;

typedef struct {
    DecType  type;
    int16_t  x;   /* level-space X */
    int8_t   y;   /* screen Y */
} Decoration;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    uint8_t life;
} DeathParticle;

typedef struct {
    char        name[64];
    int16_t     speed;
    int16_t     gravity_pct;
    char        bg_style;
    char        difficulty[16];
    int32_t     length;
    int16_t     obj_count;
    int16_t     dec_count;
    LvlObject   objects[MAX_OBJECTS];
    Decoration  decorations[MAX_DECORATIONS];

    /* background music (Flipper Music Format-style note list) */
    int16_t     music_bpm;      /* 0 = no music */
    int16_t     music_duration; /* default note length denominator, e.g. 8 = eighth note */
    int16_t     music_octave;   /* default octave when a note token has none */
    char        music_notes[MUSIC_NOTES_MAX]; /* comma-separated tokens, e.g. "E6, P, 4A, F#5" */
} Level;

typedef enum {
    GAMESTATE_MENU = 0,
    GAMESTATE_SPLASH,
    GAMESTATE_MAINMENU,
    GAMESTATE_SKINS,
    GAMESTATE_OFFICIALS,
    GAMESTATE_PLAYING,
    GAMESTATE_DEAD,
    GAMESTATE_WIN,
    GAMESTATE_PAUSE,
} GameState;

typedef struct {
    /* physics */
    float    vy;
    float    py;
    float    prev_py;
    bool     on_ground;
    bool     jump_held;

    /* rotation */
    float    angle;
    bool     snapping;
    bool     landed_on_block;
    int8_t   lock_angle; /* frames to keep angle locked after landing */

    /* world */
    int32_t  cam_x;
    float    cam_y; /* smoothed vertical camera offset (0 = no vertical scroll) */
    int16_t  window_start; /* index of first object that might be on screen */
    Level    level;

    /* music playback — one shared player, fed by whichever track is
       currently selected (the level's tune while playing, the main-menu
       tune everywhere else, see music_use_level_track/music_use_menu_track) */
    const char* music_notes_src;    /* comma-separated FMF-style note tokens for the active track */
    int16_t  music_src_bpm;
    int16_t  music_src_duration;
    int16_t  music_src_octave;
    bool     music_active;   /* active track has a note list to play */
    bool     music_acquired; /* we currently own the speaker */
    uint16_t music_cursor;   /* byte offset of the next token in music_notes_src */
    uint32_t music_next_tick; /* furi_get_tick() value (ms) at which to advance to the next note */
    uint32_t music_pause_started; /* tick() when the tone was last paused/stopped */
    float    music_cur_freq; /* frequency of the note currently sounding (0 = rest) */
    bool     music_cur_rest;
    bool     music_is_menu;  /* true once the main-menu track has been loaded for this menu visit */

    /* state */
    GameState  state;
    int16_t    attempt;
    int16_t    best_pct;
    uint8_t    dead_timer;
    int8_t     dead_pct;
    bool       dead_new_best;
    uint8_t    death_particle_count;
    DeathParticle death_particles[DEATH_PARTICLES];
    uint32_t   frame;
    int32_t    menu_cam_x;
    uint16_t   splash_frames;
    float      menu_cube_x;
    float      menu_cube_y;
    float      menu_cube_vy;
    float      menu_cube_angle;
    bool       menu_cube_snapping;
    int8_t     menu_cube_lock_angle;
    uint16_t   menu_cube_timer;
    uint16_t   menu_jump_timer;
    bool       menu_cube_on_ground;
    int8_t     menu_cube_skin;

    /* level intro */
    bool       intro_active;
    uint16_t   intro_timer;
    float      intro_player_x;

    /* menu */
    char    level_files[MAX_LEVELS][MAX_PATH_LEN];
    char    level_names[MAX_LEVELS][64];
    char    level_difficulty[MAX_LEVELS][16];
    int8_t  level_count; /* custom levels from /ext/geoflip/levels */
    int8_t  menu_sel;
    int8_t  custom_sel;
    int8_t  official_sel;

    /* customization */
    int8_t  selected_skin; /* index of chosen skin */
    int8_t  skin_cursor;   /* UI cursor while selecting */
    int8_t* official_prog; /* pointer to saved progress percent per official level (0-100) */

    bool    current_is_official;
    int8_t  current_level_idx;

    bool    btn_jump;
} GeoApp;
