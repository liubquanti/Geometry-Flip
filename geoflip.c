/**
 * Geometry Flip for Flipper Zero
 *
 * Controls:
 *   OK    — Jump (hold = auto-jump on every landing)
 *   Back  — Pause / Exit
 *   Up/Dn — Navigate menu
 *
 * Optimizations vs previous version:
 *   - Objects sorted by GX on load → O(log n) binary search each frame
 *   - Sliding window: only objects in [cam_x-CELL .. cam_x+SCREEN_W+CELL]
 *     are ever touched (~16 max at 128px screen with 8px cells)
 *   - window_start index advanced monotonically, never re-scanned
 *   - Integer-only physics hot path (float only for py/vy/angle)
 *   - draw_spike uses 3 lines instead of per-row loop
 *   - Background star field uses a single fixed pattern (no seed each frame)
 *   - No fmodf in rotation hot path (manual clamp instead)
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "levels.h"
#include "icons.h"

/* ─── Constants ─────────────────────────────────────────────────── */

#define SCREEN_W        128
#define SCREEN_H        64
#define GROUND_Y        54
#define PLAYER_GX       2
#define PLAYER_SIZE     8
#define CELL            8
#define GRAVITY         0.3f
#define JUMP_VEL        (-3.1f)
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
    int32_t     length;
    int16_t     obj_count;
    int16_t     dec_count;
    LvlObject   objects[MAX_OBJECTS];
    Decoration  decorations[MAX_DECORATIONS];
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
    int16_t  window_start; /* index of first object that might be on screen */
    Level    level;

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

    /* menu */
    char    level_files[MAX_LEVELS][MAX_PATH_LEN];
    char    level_names[MAX_LEVELS][64];
    int8_t  level_count; /* custom levels from /ext/geoflip/levels */
    int8_t  menu_sel;
    int8_t  custom_sel;
    int8_t  official_sel;

    /* customization */
    int8_t  selected_skin; /* index of chosen skin */
    int8_t  skin_cursor;   /* UI cursor while selecting */

    bool    current_is_official;
    int8_t  current_level_idx;

    bool    btn_jump;
} GeoApp;

/* ─── Trig LUT ───────────────────────────────────────────────────── */

static const int8_t sin_lut[10] = { 0, 22, 44, 62, 78, 90, 100, 107, 113, 117 };

static int isin128(int deg) {
    deg = ((deg % 360) + 360) % 360;
    int sign = (deg < 180) ? 1 : -1;
    int d = deg % 180;
    if(d > 90) d = 180 - d;
    int i0 = d / 10; if(i0 > 9) i0 = 9;
    int i1 = i0 + 1; if(i1 > 9) i1 = 9;
    int fr = (d % 10) * 256 / 10;
    int v  = ((int)sin_lut[i0] * (256 - fr) + (int)sin_lut[i1] * fr) >> 8;
    return sign * v;
}
static int icos128(int deg) { return isin128(deg + 90); }

/* ─── Draw rotated cube ──────────────────────────────────────────── */

/* Built-in 8x8 skins: 1 = black(+), 0 = white(-) */
static const int SKIN_COUNT = 4;
static const uint8_t SKINS[4][8] = {
    { 0xFF, 0x81, 0xA5, 0xA5, 0x81, 0xBD, 0x81, 0xFF },
    { 0xFF, 0x81, 0xBD, 0xA5, 0xA5, 0xBD, 0x81, 0xFF },
    { 0xFF, 0xC3, 0xA5, 0x99, 0x99, 0xA5, 0xC3, 0xFF },
    { 0xFF, 0xCF, 0xC9, 0xF9, 0x9F, 0x93, 0xF3, 0xFF },
};

static void draw_player_rotated(Canvas* canvas, int cx, int cy, float angle_f, int skin) {
    int ang = (int)angle_f;
    ang = ((ang % 360) + 360) % 360;
    int sa = isin128(ang), ca = icos128(ang);
    /* draw skin pixels (8x8) rotated with same trig to preserve animation */
    if(skin >= 0 && skin < SKIN_COUNT) {
        const uint8_t* bmp = SKINS[skin];
        /* pixel coordinates: i=0..7, j=0..7; center map to -R..R-1 */
        for(int j = 0; j < PLAYER_SIZE; j++) {
            uint8_t row = bmp[j];
            for(int i = 0; i < PLAYER_SIZE; i++) {
                if(row & (1 << (7 - i))) {
                    int x0 = i - (PLAYER_SIZE / 2);
                    int y0 = j - (PLAYER_SIZE / 2);
                    int pxp = cx + ((x0 * ca - y0 * sa + 64) >> 7);
                    int pyp = cy + ((x0 * sa + y0 * ca + 64) >> 7);
                    canvas_draw_box(canvas, pxp, pyp, 1, 1);
                }
            }
        }
    }
}

/* ─── Sort helpers ───────────────────────────────────────────────── */

/* Simple insertion sort — fast for nearly-sorted level data */
static void sort_objects(LvlObject* arr, int n) {
    for(int i = 1; i < n; i++) {
        LvlObject key = arr[i];
        int j = i - 1;
        while(j >= 0 && arr[j].gx > key.gx) {
            arr[j+1] = arr[j];
            j--;
        }
        arr[j+1] = key;
    }
}

/* ─── Level Parser ───────────────────────────────────────────────── */

static bool parse_level(const char* path, Level* lvl) {
    memset(lvl, 0, sizeof(Level));
    lvl->speed       = SCROLL_SPEED;
    lvl->gravity_pct = 100;
    lvl->bg_style    = '0';
    lvl->length      = 2000;
    strncpy(lvl->name, "Unnamed", sizeof(lvl->name) - 1);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File*    file    = storage_file_alloc(storage);

    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char   line[128];
    int    idx = 0;
    char   ch;

    while(storage_file_read(file, &ch, 1) == 1) {
        if(ch == '\n' || ch == '\r') {
            if(idx == 0) continue;
            line[idx] = '\0';
            idx = 0;
            if(line[0] == '#') continue;

            if     (strncmp(line, "NAME ",    5) == 0) strncpy(lvl->name, line+5, 63);
            else if(strncmp(line, "SPEED ",   6) == 0) lvl->speed       = (int16_t)atoi(line+6);
            else if(strncmp(line, "GRAVITY ", 8) == 0) lvl->gravity_pct = (int16_t)atoi(line+8);
            else if(strncmp(line, "BG ",      3) == 0) lvl->bg_style    = line[3];
            else if(strncmp(line, "LENGTH ",  7) == 0) lvl->length      = atoi(line+7);
            else if(strncmp(line, "OBJ ",     4) == 0 && lvl->obj_count < MAX_OBJECTS) {
                char ts[16] = {0};
                int  gx = 0, gy = 0;
                sscanf(line+4, "%15s %d %d", ts, &gx, &gy);
                ObjType t;
                if     (strcmp(ts,"BLOCK") == 0)      t = OBJ_BLOCK;
                else if(strcmp(ts,"SPIKE") == 0)      t = OBJ_SPIKE;
                else if(strcmp(ts,"MINI_SPIKE") == 0) t = OBJ_MINI_SPIKE;
                else if(strcmp(ts,"MINI_BLOCK") == 0) t = OBJ_MINI_BLOCK;
                else if(strcmp(ts,"JUMPER") == 0)     t = OBJ_JUMPER;
                else if(strcmp(ts,"SPHERE") == 0)     t = OBJ_SPHERE;
                else continue;
                lvl->objects[lvl->obj_count++] = (LvlObject){t, (int16_t)gx, (int16_t)gy};
            }
            else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                char ts[16] = {0};
                int  x = 0, y = 0;
                sscanf(line+4, "%15s %d %d", ts, &x, &y);
                DecType t;
                if     (strcmp(ts,"STAR")   == 0) t = DEC_STAR;
                else if(strcmp(ts,"CLOUD")  == 0) t = DEC_CLOUD;
                else if(strcmp(ts,"PILLAR") == 0) t = DEC_PILLAR;
                else continue;
                lvl->decorations[lvl->dec_count++] = (Decoration){t, (int16_t)x, (int8_t)y};
            }
        } else if(idx < (int)sizeof(line) - 1) {
            line[idx++] = ch;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    /* Sort objects by GX so we can use a sliding window during gameplay */
    sort_objects(lvl->objects, lvl->obj_count);
    return true;
}

static bool parse_level_from_text(const char* text, Level* lvl) {
    memset(lvl, 0, sizeof(Level));
    lvl->speed       = SCROLL_SPEED;
    lvl->gravity_pct = 100;
    lvl->bg_style    = '0';
    lvl->length      = 2000;
    strncpy(lvl->name, "Unnamed", sizeof(lvl->name) - 1);

    char line[128];
    int idx = 0;
    for(const char* p = text; ; p++) {
        char ch = *p;
        if(ch == '\n' || ch == '\r' || ch == '\0') {
            if(idx > 0) {
                line[idx] = '\0';
                idx = 0;
                if(line[0] != '#') {
                    if(strncmp(line, "NAME ", 5) == 0) strncpy(lvl->name, line + 5, 63);
                    else if(strncmp(line, "SPEED ", 6) == 0) lvl->speed = (int16_t)atoi(line + 6);
                    else if(strncmp(line, "GRAVITY ", 8) == 0) lvl->gravity_pct = (int16_t)atoi(line + 8);
                    else if(strncmp(line, "BG ", 3) == 0) lvl->bg_style = line[3];
                    else if(strncmp(line, "LENGTH ", 7) == 0) lvl->length = atoi(line + 7);
                    else if(strncmp(line, "OBJ ", 4) == 0 && lvl->obj_count < MAX_OBJECTS) {
                        char ts[16] = {0};
                        int gx = 0, gy = 0;
                        sscanf(line + 4, "%15s %d %d", ts, &gx, &gy);
                        ObjType t;
                        if(strcmp(ts, "BLOCK") == 0) t = OBJ_BLOCK;
                        else if(strcmp(ts, "SPIKE") == 0) t = OBJ_SPIKE;
                        else if(strcmp(ts, "MINI_SPIKE") == 0) t = OBJ_MINI_SPIKE;
                        else if(strcmp(ts, "MINI_BLOCK") == 0) t = OBJ_MINI_BLOCK;
                        else if(strcmp(ts, "JUMPER") == 0) t = OBJ_JUMPER;
                        else if(strcmp(ts, "SPHERE") == 0) t = OBJ_SPHERE;
                        else continue;
                        lvl->objects[lvl->obj_count++] = (LvlObject){t, (int16_t)gx, (int16_t)gy};
                    } else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                        char ts[16] = {0};
                        int x = 0, y = 0;
                        sscanf(line + 4, "%15s %d %d", ts, &x, &y);
                        DecType t;
                        if(strcmp(ts, "STAR") == 0) t = DEC_STAR;
                        else if(strcmp(ts, "CLOUD") == 0) t = DEC_CLOUD;
                        else if(strcmp(ts, "PILLAR") == 0) t = DEC_PILLAR;
                        else continue;
                        lvl->decorations[lvl->dec_count++] = (Decoration){t, (int16_t)x, (int8_t)y};
                    }
                }
            }
            if(ch == '\0') break;
        } else if(idx < (int)sizeof(line) - 1) {
            line[idx++] = ch;
        }
    }

    sort_objects(lvl->objects, lvl->obj_count);
    return true;
}

/* forward declarations for profile persistence */
static void save_player_profile(GeoApp* app);
static void load_player_profile(GeoApp* app);

/* ─── Level Discovery ────────────────────────────────────────────── */

static int discover_levels(char files[][MAX_PATH_LEN], int max) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LEVEL_DIR);
    File* dir   = storage_file_alloc(storage);
    int   count = 0;
    if(storage_dir_open(dir, LEVEL_DIR)) {
        FileInfo fi;
        char     name[MAX_FILENAME];
        while(count < max && storage_dir_read(dir, &fi, name, sizeof(name))) {
            if(!(fi.flags & FSF_DIRECTORY)) {
                int len = (int)strlen(name);
                if(len > 6 && strcmp(name + len - 6, ".gdlvl") == 0) {
                    snprintf(files[count], MAX_PATH_LEN, "%s/%s", LEVEL_DIR, name);
                    count++;
                }
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    return count;
}

/* ─── Collision ──────────────────────────────────────────────────── */

static bool rects_overlap(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh) {
    return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

/* ─── Rotation helpers ───────────────────────────────────────────── */

static float nearest_90(float a) {
    /* map to [0,360) then round to nearest multiple of 90 */
    while(a <   0.0f) a += 360.0f;
    while(a >= 360.0f) a -= 360.0f;
    int seg = (int)((a + 45.0f) / 90.0f) & 3;
    return (float)(seg * 90);
}

static float angle_approach(float cur, float tgt, float step) {
    float d = tgt - cur;
    while(d >  180.0f) d -= 360.0f;
    while(d < -180.0f) d += 360.0f;
    if(d >= 0.0f) cur += (d < step) ? d : step;
    else { float n = -d; cur -= (n < step) ? n : step; }
    while(cur <   0.0f) cur += 360.0f;
    while(cur >= 360.0f) cur -= 360.0f;
    return cur;
}

/* ─── Game Logic ─────────────────────────────────────────────────── */

static void game_reset(GeoApp* app) {
    app->vy           = 0.0f;
    app->py           = (float)(GROUND_Y - PLAYER_SIZE);
    app->prev_py      = app->py;
    app->on_ground    = true;
    app->jump_held    = false;
    app->btn_jump     = false;
    app->angle        = 0.0f;
    app->snapping     = false;
    app->landed_on_block = false;
    app->lock_angle    = 0;
    app->cam_x        = 0;
    app->frame        = 0;
    app->dead_timer   = 0;
    app->dead_pct     = 0;
    app->dead_new_best = false;
    app->death_particle_count = 0;
    /* reset sliding window to beginning of sorted object list */
    app->window_start = 0;
}

static void death_particles_spawn(GeoApp* app) {
    int cx = PLAYER_GX * CELL + PLAYER_SIZE / 2;
    int cy = (int)app->py + PLAYER_SIZE / 2;

    if(cy < 0) cy = 0;
    if(cy > SCREEN_H - 1) cy = SCREEN_H - 1;

    app->death_particle_count = DEATH_PARTICLES;
    for(uint8_t i = 0; i < DEATH_PARTICLES; i++) {
        int deg = (int)i * (360 / DEATH_PARTICLES);
        float speed = 0.7f + (float)(i % 4) * 0.22f;
        DeathParticle* p = &app->death_particles[i];
        p->x = (float)cx;
        p->y = (float)cy;
        p->vx = ((float)icos128(deg) / 128.0f) * speed;
        p->vy = ((float)isin128(deg) / 128.0f) * speed - 0.25f;
        p->life = 32 + (i % 12);
    }
}

static void death_particles_update(GeoApp* app) {
    for(uint8_t i = 0; i < app->death_particle_count; i++) {
        DeathParticle* p = &app->death_particles[i];
        if(p->life == 0) continue;
        p->x += p->vx;
        p->y += p->vy;
        p->vy += 0.10f;
        p->vx *= 0.985f;
        p->life--;
    }
}

static void death_particles_draw(Canvas* canvas, const GeoApp* app) {
    for(uint8_t i = 0; i < app->death_particle_count; i++) {
        const DeathParticle* p = &app->death_particles[i];
        if(p->life == 0) continue;
        int x = (int)p->x;
        int y = (int)p->y;
        if(x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) continue;
        canvas_draw_dot(canvas, x, y);
        if(p->life > 18 && x + 1 < SCREEN_W) canvas_draw_dot(canvas, x + 1, y);
    }
}

static int game_pct(const GeoApp* app) {
    if(app->level.length <= 0) return 0;
    int p = (int)((int64_t)app->cam_x * 100 / app->level.length);
    if(p < 0) p = 0;
    if(p > 100) p = 100;
    return p;
}

static void game_begin_death(GeoApp* app) {
    int p = game_pct(app);
    app->dead_pct      = (int8_t)p;
    app->dead_new_best = (p > app->best_pct);
    if(app->dead_new_best) app->best_pct = (int16_t)p;
    app->dead_timer    = 0;
    death_particles_spawn(app);
    app->state         = GAMESTATE_DEAD;
}

static void game_start_level(GeoApp* app, int idx) {
    if(idx < 0 || idx >= app->level_count) return;
    if(!parse_level(app->level_files[idx], &app->level)) return;
    app->current_is_official = false;
    app->current_level_idx = (int8_t)idx;
    app->attempt++;
    game_reset(app);
    app->state = GAMESTATE_PLAYING;
}

static void game_start_official_level(GeoApp* app, int idx) {
    if(idx < 0 || idx >= OFFICIAL_LEVEL_COUNT) return;
    if(!parse_level_from_text(OFFICIAL_LEVELS[idx].data, &app->level)) return;
    app->current_is_official = true;
    app->current_level_idx = (int8_t)idx;
    app->attempt++;
    game_reset(app);
    app->state = GAMESTATE_PLAYING;
}

static void game_restart_current_level(GeoApp* app) {
    if(app->current_is_official) game_start_official_level(app, app->current_level_idx);
    else game_start_level(app, app->current_level_idx);
}

static void game_update(GeoApp* app) {
    if(app->state != GAMESTATE_PLAYING) return;

    app->frame++;

    /* reset per-frame landing flag */
    app->landed_on_block = false;
    if(app->lock_angle > 0) app->lock_angle--;

    /* ── scroll ── */
    int speed = app->level.speed > 0 ? app->level.speed : SCROLL_SPEED;
    app->cam_x += speed;

    /* ── physics ── */
    app->prev_py = app->py;
    app->vy     += GRAVITY * (float)app->level.gravity_pct * 0.01f;
    app->py     += app->vy;

    /* ── ground ── */
    const float ground_limit = (float)(GROUND_Y - PLAYER_SIZE);
    bool was_airborne = !app->on_ground;
    app->on_ground = false;

    if(app->py >= ground_limit) {
        app->py        = ground_limit;
        app->vy        = 0.0f;
        app->on_ground = true;
    }

    if(app->py > (float)(SCREEN_H + 4)) { game_begin_death(app); return; }

    /* ── player hitbox (pre-computed once) ── */
    const int px_hit = PLAYER_GX * CELL + 1;
    const int py_hit = (int)app->py + 1;
    const int pw_hit = PLAYER_SIZE - 2;
    const int ph_hit = PLAYER_SIZE - 1;  /* slightly taller for better block adhesion */

        /* ── sliding window: advance only when objects fully leave the left edge ── */
        while(app->window_start < app->level.obj_count &&
                ((int)app->level.objects[app->window_start].gx * CELL + CELL) <= app->cam_x) {
                app->window_start++;
        }

    /* ── object collisions (window only) ── */
    int right_edge_gx = (app->cam_x + SCREEN_W + CELL) / CELL;

    for(int i = app->window_start; i < app->level.obj_count; i++) {
        const LvlObject* o = &app->level.objects[i];

        /* Objects are sorted — stop as soon as we pass the right edge */
        if(o->gx > right_edge_gx) break;

        int sx = o->gx * CELL - app->cam_x;
        int sy = GROUND_Y - (o->gy + 1) * CELL;

        if(o->type == OBJ_BLOCK) {
            /* Explicit stick-to-block logic: if player is very close to block top,
               snap them onto it (helps with seamless multi-block traversal) */
            int player_bottom = (int)app->py + PLAYER_SIZE;
            int block_top     = sy;
            if(!app->on_ground && app->vy >= 0.0f && 
               player_bottom > block_top - 2 && player_bottom < block_top + 4 &&
               px_hit + pw_hit > sx && px_hit < sx + CELL) {
                app->py        = (float)(sy - PLAYER_SIZE);
                app->vy        = 0.0f;
                app->on_ground = true;
                app->angle     = nearest_90(app->angle);
                app->snapping  = false;
                app->landed_on_block = true;
                app->lock_angle = 6;
                continue;  /* skip normal collision for this block */
            }

            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, sy, CELL, CELL)) {
                bool from_above = (int)app->prev_py + PLAYER_SIZE <= sy + 2;
                /* allow a slightly larger tolerance for landing detection */
                bool tolerant_above = (int)app->prev_py + PLAYER_SIZE <= sy + 4;
                if((from_above || tolerant_above) && app->vy >= 0.0f) {
                    app->py        = (float)(sy - PLAYER_SIZE);
                    app->vy        = 0.0f;
                    app->on_ground = true;
                    /* Immediately lock angle to nearest 90° for block landings
                       and mark landed_on_block so rotation logic knows to avoid
                       playing the snapping animation this frame. */
                    app->angle     = nearest_90(app->angle);
                    app->snapping  = false;
                    app->landed_on_block = true;
                    app->lock_angle = 6; /* keep angle locked for a few frames */
                } else {
                    game_begin_death(app); return;
                }
            }
        } else if(o->type == OBJ_SPIKE) {
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit,
                             sx+2, sy+4, 4, 4)) {
                game_begin_death(app); return;
            }
        } else if(o->type == OBJ_MINI_SPIKE) {
            /* Mini spike: full width, half height (bottom half of cell) */
            int msh = CELL / 2;
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit,
                             sx, sy + msh, CELL, msh)) {
                game_begin_death(app); return;
            }
        } else if(o->type == OBJ_MINI_BLOCK) {
            /* Mini block: 4px tall, can land on top like regular block */
            int mbh = CELL / 2;
            int player_bottom = (int)app->py + PLAYER_SIZE;
            int block_top     = sy;
            if(!app->on_ground && app->vy >= 0.0f && 
               player_bottom > block_top - 2 && player_bottom < block_top + 4 &&
               px_hit + pw_hit > sx && px_hit < sx + CELL) {
                app->py        = (float)(sy - PLAYER_SIZE);
                app->vy        = 0.0f;
                app->on_ground = true;
                app->angle     = nearest_90(app->angle);
                app->snapping  = false;
                app->landed_on_block = true;
                app->lock_angle = 6;
                continue;
            }
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, sy, CELL, mbh)) {
                bool from_above = (int)app->prev_py + PLAYER_SIZE <= sy + 2;
                bool tolerant_above = (int)app->prev_py + PLAYER_SIZE <= sy + 4;
                if((from_above || tolerant_above) && app->vy >= 0.0f) {
                    app->py        = (float)(sy - PLAYER_SIZE);
                    app->vy        = 0.0f;
                    app->on_ground = true;
                    app->angle     = nearest_90(app->angle);
                    app->snapping  = false;
                    app->landed_on_block = true;
                    app->lock_angle = 6;
                } else {
                    game_begin_death(app); return;
                }
            }
        } else if(o->type == OBJ_JUMPER) {
            /* Jumper pad: 2px tall at bottom, triggers auto-jump on any contact.
               Never causes death. */
            int jumper_h = 2;
            int jumper_y = sy + CELL - jumper_h;
            
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, jumper_y, CELL, jumper_h)) {
                /* Contact with jumper from any side → auto-jump */
                if(app->vy >= 0.0f) {
                    /* Coming from above or level */
                    app->py = (float)(sy - PLAYER_SIZE);
                }
                app->vy = JUMP_VEL;  /* Auto-jump */
                app->on_ground = false;
                app->angle = nearest_90(app->angle);
                app->snapping = false;
                app->lock_angle = 0;
            }
        } else if(o->type == OBJ_SPHERE) {
            /* Sphere: manual jump pad, only triggers when inside hitbox and jump pressed.
               Never causes death - safe to touch. */
            int sphere_hitbox = CELL;
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, sy, sphere_hitbox, sphere_hitbox)) {
                if(app->btn_jump && !app->jump_held) {
                    /* Player pressed jump while on/in sphere */
                    app->vy        = JUMP_VEL;
                    app->on_ground = false;
                    app->jump_held = true;
                    app->lock_angle = 0;
                }
            }
        }
    }

    /* ── jump ── */
    if(app->on_ground) app->jump_held = false;
    if(app->btn_jump && app->on_ground && !app->jump_held) {
        app->vy        = JUMP_VEL;
        app->on_ground = false;
        app->jump_held = true;
        app->lock_angle = 0;  /* Allow rotation immediately on jump */
    }
    if(!app->btn_jump) app->jump_held = false;

    /* ── rotation ── */
    if(!app->on_ground) {
        app->snapping = false;
        if(app->lock_angle <= 0) {
            app->angle   += ROT_SPEED_AIR;
            if(app->angle >= 360.0f) app->angle -= 360.0f;
        }
    } else {
        if(was_airborne && app->on_ground && !app->landed_on_block) app->snapping = true;
        if(app->snapping) {
            float tgt  = nearest_90(app->angle);
            app->angle = angle_approach(app->angle, tgt, ROT_SNAP_SPEED);
            float diff = app->angle - tgt;
            if(diff < 0.0f) diff = -diff;
            if(diff < 1.5f) { app->angle = tgt; app->snapping = false; }
        } else {
            /* Not snapping and on ground — lock to exact 90-degree orientation */
            app->angle = nearest_90(app->angle);
        }
    }

    /* ── win ── */
    if(app->cam_x >= app->level.length) {
        if(100 > app->best_pct) app->best_pct = 100;
        app->state = GAMESTATE_WIN;
    }
}

/* ─── Rendering ──────────────────────────────────────────────────── */

static void draw_spike(Canvas* canvas, int sx, int sy) {
    /* Filled isosceles triangle (pointing up) with an inner left-side highlight
       Fill by drawing horizontal scanlines — fast and deterministic on Furi */
    for(int r = 0; r < CELL; r++) {
        int half = (CELL - 1 - r) / 2;
        int x1 = sx + half;
        int x2 = sx + CELL - 1 - half;
        canvas_draw_line(canvas, x1, sy + r, x2, sy + r);
    }

    /* Inner left-side highlight similar to blocks: a thin white slanted line
       inset from the left edge, running toward the apex */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, sx + 1, sy + CELL - 2, sx + CELL/2, sy + 1);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_block(Canvas* canvas, int sx, int sy) {
    canvas_draw_box(canvas, sx, sy, CELL, CELL);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, sx+1, sy+1, sx+CELL-2, sy+1);
    canvas_draw_line(canvas, sx+1, sy+1, sx+1, sy+CELL-2);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_mini_spike(Canvas* canvas, int sx, int sy) {
    /* Mini spike: half height (4px), full width, same style as regular spike
       Triangle pointing up from bottom half of cell */
    int h = CELL / 2;
    int y_offset = sy + h;  /* Start from middle of cell */
    for(int r = 0; r < h; r++) {
        int half = (h - 1 - r) / 2;
        int x1 = sx + half;
        int x2 = sx + CELL - 1 - half;
        canvas_draw_line(canvas, x1, y_offset + r, x2, y_offset + r);
    }
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, sx + 1, y_offset + h - 2, sx + CELL/2, y_offset + 1);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_mini_block(Canvas* canvas, int sx, int sy) {
    /* Mini block: 4px tall, 8px wide, filled with highlight */
    int h = CELL / 2;
    canvas_draw_box(canvas, sx, sy, CELL, h);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, sx+1, sy+1, sx+CELL-2, sy+1);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_jumper(Canvas* canvas, int sx, int sy) {
    /* Jumper pad: thin pad at bottom (2px) with cross-hatch pattern */
    int h = 2;
    int y_pad = sy + CELL - h;
    canvas_draw_box(canvas, sx, y_pad, CELL, h);
    canvas_set_color(canvas, ColorWhite);
    for(int i = 0; i < CELL; i += 2) {
        canvas_draw_dot(canvas, sx + i, y_pad);
    }
    canvas_set_color(canvas, ColorBlack);
}

static void draw_sphere(Canvas* canvas, int sx, int sy) {
    /* Sphere: circular pad, drawn as a circle outline */
    int cx = sx + CELL / 2;
    int cy = sy + CELL / 2;
    int r = CELL / 2 - 1;
    /* Draw approximation of circle using canvas */
    canvas_draw_circle(canvas, cx, cy, r);
}


/*
 * Background star field — pre-baked positions, no per-frame RNG.
 * 20 stars at fixed (x % SCREEN_W, y) positions, scrolled by cam_x/4.
 */
static const uint8_t star_x[20] = {
     3, 15, 27, 40, 52, 64, 76, 88,100,112,
     8, 20, 33, 45, 57, 70, 82, 94,106,120
};
static const uint8_t star_y[20] = {
     5, 18, 10, 28, 15, 35,  3, 22,  8, 40,
    12, 50, 20,  7, 55, 25, 58, 14, 60, 45
};

static void draw_background(Canvas* canvas, char style, int cam_x) {
    if(style == '1') {
        int shift = (cam_x / 4) & 0x7f;   /* 0..127 */
        for(int i = 0; i < 20; i++) {
            int sx = (int)star_x[i] - shift;
            if(sx < 0) sx += SCREEN_W;
            int sy = (int)star_y[i];
            /* 2px star (2x2 block) */
            canvas_draw_dot(canvas, sx, sy);
            canvas_draw_dot(canvas, sx + 1, sy);
            canvas_draw_dot(canvas, sx, sy + 1);
            canvas_draw_dot(canvas, sx + 1, sy + 1);
        }
    } else if(style == '2') {
        int ox = -(cam_x & 15);   /* cam_x % 16, avoid division */
        for(int x = ox; x < SCREEN_W; x += 16)
            canvas_draw_line(canvas, x, 0, x, GROUND_Y-1);
        for(int y = 0; y < GROUND_Y; y += 16)
            canvas_draw_line(canvas, 0, y, SCREEN_W-1, y);
    }
}

static void draw_splash_screen(Canvas* canvas) {
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
        /* center-aligned title and subtitle above the icon */
        canvas_draw_str_aligned(canvas, SCREEN_W/2, 6, AlignCenter, AlignTop, "Video Game Module");
        canvas_draw_str_aligned(canvas, SCREEN_W/2, 51, AlignCenter, AlignTop, "Recommended");
        /* icon centered horizontally (I_video_game_module is 60px wide) */
        canvas_draw_icon(canvas, (SCREEN_W - 60) / 2, 19, &I_video_game_module);
}



static void draw_decorations(Canvas* canvas, const Level* lvl, int cam_x) {
    for(int i = 0; i < lvl->dec_count; i++) {
        const Decoration* d = &lvl->decorations[i];
        int sx = (int)d->x - cam_x / 2;
        int sy = (int)d->y;
        switch(d->type) {
        case DEC_STAR:
            /* skip if any star pixel would be off-screen */
            if(sx < 0 || sx + 1 >= SCREEN_W || sy < 0 || sy + 1 >= SCREEN_H) continue;
            canvas_draw_dot(canvas, sx,   sy);
            canvas_draw_dot(canvas, sx+1, sy);
            canvas_draw_dot(canvas, sx,   sy+1);
            break;
        case DEC_CLOUD:
            /* skip if cloud would be partially off-screen */
            if(sx < 0 || sx + 12 >= SCREEN_W || sy + 2 < 0 || sy + 7 >= SCREEN_H) continue;
            canvas_draw_frame(canvas, sx,   sy+2, 10, 5);
            canvas_draw_frame(canvas, sx+2, sy,   6,  4);
            break;
        case DEC_PILLAR:
            /* skip if pillar would be partially off-screen */
            if(sx < 0 || sx + 4 >= SCREEN_W || sy < 0 || sy + (GROUND_Y - 40) >= SCREEN_H) continue;
            canvas_draw_frame(canvas, sx, 40, 4, GROUND_Y-40);
            break;
        }
    }
}

/* Draw only objects in the visible window (same bounds as collision) */
static void draw_objects(Canvas* canvas, const GeoApp* app) {
    int right_edge_gx = (app->cam_x + SCREEN_W + CELL) / CELL;
    for(int i = app->window_start; i < app->level.obj_count; i++) {
        const LvlObject* o = &app->level.objects[i];
        if(o->gx > right_edge_gx) break;
        int sx = o->gx * CELL - app->cam_x;
        int sy = GROUND_Y - (o->gy + 1) * CELL;
        /* Compute vertical extents per object type to avoid partial vertical clipping
           and expensive draw operations when objects are off-screen above/below. */
        int topY = sy;
        int bottomY = sy + CELL;
        if(o->type == OBJ_MINI_SPIKE) {
            /* mini spike occupies bottom half of cell */
            topY = sy + (CELL / 2);
            bottomY = sy + CELL;
        } else if(o->type == OBJ_MINI_BLOCK) {
            /* mini block occupies top half of cell */
            topY = sy;
            bottomY = sy + (CELL / 2);
        } else if(o->type == OBJ_JUMPER) {
            int jumper_h = 2;
            topY = sy + CELL - jumper_h;
            bottomY = sy + CELL;
        }

        /* require full horizontal and vertical containment to draw */
        if(sx < 0 || sx + CELL > SCREEN_W) continue;
        if(topY < 0 || bottomY > SCREEN_H) continue;

        switch(o->type) {
        case OBJ_BLOCK:       draw_block(canvas, sx, sy); break;
        case OBJ_SPIKE:       draw_spike(canvas, sx, sy); break;
        case OBJ_MINI_SPIKE:  draw_mini_spike(canvas, sx, sy); break;
        case OBJ_MINI_BLOCK:  draw_mini_block(canvas, sx, sy); break;
        case OBJ_JUMPER:      draw_jumper(canvas, sx, sy); break;
        case OBJ_SPHERE:      draw_sphere(canvas, sx, sy); break;
        default: break;
        }
    }
}

static void render_callback(Canvas* canvas, void* ctx) {
    GeoApp* app = (GeoApp*)ctx;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* ─── SPLASH ─── */
    if(app->state == GAMESTATE_SPLASH) {
        draw_splash_screen(canvas);
        return;
    }

    /* ─── MENU ─── */
    if(app->state == GAMESTATE_MENU) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 18, 10, "CUSTOM LEVELS");
        canvas_set_font(canvas, FontSecondary);
        if(app->level_count == 0) {
            canvas_draw_str(canvas, 8, 30, "No custom levels!");
            canvas_draw_str(canvas, 8, 42, LEVEL_DIR);
            canvas_draw_str(canvas, 8, 54, "Add .gdlvl files");
        } else {
            const int ITEM_H    = 12;
            const int MAX_VIS   = 4;
            const int Y_START   = 22;
            int scroll = 0;
            if(app->custom_sel >= MAX_VIS) scroll = (app->custom_sel - MAX_VIS + 1) * ITEM_H;
            for(int i = 0; i < app->level_count; i++) {
                int y = Y_START + i * ITEM_H - scroll;
                if(y < 20 || y > SCREEN_H - 2) continue;
                if(i == app->custom_sel) {
                    canvas_draw_rbox(canvas, 2, y-9, SCREEN_W-4, 12, 2);
                    canvas_set_color(canvas, ColorWhite);
                }
                canvas_draw_str(canvas, 8, y, app->level_names[i]);
                canvas_set_color(canvas, ColorBlack);
            }
        }
        return;
    }

    /* ─── PLAYING / PAUSE / DEAD ─── */
    if(app->state == GAMESTATE_PLAYING ||
       app->state == GAMESTATE_PAUSE   ||
       app->state == GAMESTATE_DEAD) {

        draw_background(canvas, app->level.bg_style, app->cam_x);
        draw_decorations(canvas, &app->level, app->cam_x);

        /* Ground fill (white) */
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, GROUND_Y, SCREEN_W, SCREEN_H - GROUND_Y);
        canvas_set_color(canvas, ColorBlack);

        canvas_draw_line(canvas, 0, GROUND_Y, SCREEN_W-1, GROUND_Y);

        /* Attempt label — scrolls with level, visible at start */
        {
            int ax = 40 - app->cam_x;
            if(ax > -60 && ax < SCREEN_W) {
                canvas_set_font(canvas, FontPrimary);
                char buf[16];
                snprintf(buf, sizeof(buf), "Attempt %d", (int)app->attempt);
                canvas_draw_str(canvas, ax, GROUND_Y - 30, buf);
            }
        }

        draw_objects(canvas, app);

        /* Player (hidden when dead). Draw only when fully inside screen to avoid costly partial clipping */
        if(app->state != GAMESTATE_DEAD) {
            int cx = PLAYER_GX * CELL + PLAYER_SIZE / 2;
            int cy = (int)app->py + PLAYER_SIZE / 2;
            const int R = PLAYER_SIZE / 2;
            /* Extend buffer by 2px to give cushion at edges before skipping draw */
            const int buf = 2;
                if(cx - R - buf >= 0 && cx + R + buf < SCREEN_W && 
               cy - R - buf >= 0 && cy + R + buf < SCREEN_H) {
                draw_player_rotated(canvas, cx, cy, app->angle, app->selected_skin);
            }
        } else {
            death_particles_draw(canvas, app);
        }

        /* HUD — progress bar */
        int pct = game_pct(app);
        int bar_w = (SCREEN_W - 4) * pct / 100;
        canvas_draw_frame(canvas, 2, 1, SCREEN_W-4, 4);
        if(bar_w > 0) canvas_draw_box(canvas, 2, 1, bar_w, 4);
        canvas_set_font(canvas, FontSecondary);
        char hud[16];
        snprintf(hud, sizeof(hud), "%d%%", pct);
        canvas_draw_str(canvas, 2, 14, hud);

        /* Overlay */
        if(app->state == GAMESTATE_PAUSE) {
            const int card_x = 30;
            const int card_y = 24;
            const int card_w = 66;
            const int card_h = 16;
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, card_x, card_y, card_w, card_h, 3);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, card_x + 1, card_y + 1, card_w - 2, card_h - 2, 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, card_x + 2, card_y + 2, card_w - 4, card_h - 4, 2);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, card_x + 3, card_y + 3, card_w - 6, card_h - 6, 1);
            canvas_set_color(canvas, ColorBlack);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(
                canvas,
                card_x + card_w / 2,
                card_y + card_h / 2,
                AlignCenter,
                AlignCenter,
                "PAUSED");
        } else if(app->state == GAMESTATE_DEAD && app->dead_new_best) {
            const int card_x = 26;
            const int card_y = 18;
            const int card_w = 76;
            const int card_h = 30;
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, card_x, card_y, card_w, card_h, 3);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, card_x + 1, card_y + 1, card_w - 2, card_h - 2, 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, card_x + 2, card_y + 2, card_w - 4, card_h - 4, 2);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, card_x + 3, card_y + 3, card_w - 6, card_h - 6, 1);
            canvas_set_color(canvas, ColorBlack);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(
                canvas,
                card_x + card_w / 2,
                card_y + 10,
                AlignCenter,
                AlignCenter,
                "NEW BEST");
            canvas_set_font(canvas, FontSecondary);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", (int)app->dead_pct);
            canvas_draw_str_aligned(
                canvas,
                card_x + card_w / 2,
                card_y + 21,
                AlignCenter,
                AlignCenter,
                buf);
        }
        return;
    }

    /* ─── SKIN SELECT (CAROUSEL with 5 icons) ─── */
    if(app->state == GAMESTATE_SKINS) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 28, 10, "CHOOSE ICON");
        
        /* carousel: show 5 icons (left-far, left, center-selected, right, right-far) */
        const int icon_w = 8, icon_h = 8;
        const int center_y = SCREEN_H / 2 - icon_h / 2;  /* vertically centered */
        const int icon_spacing = 24;
        
        /* calculate icon indices for carousel display (5 icons total) */
        int indices[5];
        for(int i = 0; i < 5; i++) {
            indices[i] = (app->skin_cursor - 2 + i + SKIN_COUNT * 10) % SKIN_COUNT;
        }
        
        /* center icon position is at i=2 */
        int center_x_pos = 10 + 2 * icon_spacing;
        
        /* draw 5 icons in a row */
        for(int i = 0; i < 5; i++) {
            int icon_idx = indices[i];
            int x_pos = 10 + i * icon_spacing;
            
            /* skip if outside screen bounds */
            if(x_pos + icon_w > SCREEN_W) continue;
            
            canvas_set_color(canvas, ColorBlack);
            const uint8_t* bmp = SKINS[icon_idx];
            
            /* highlight center icon with box */
            if(i == 2) {  /* center position */
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, x_pos - 5, center_y - 5, icon_w + 10, icon_h + 10, 2);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_rbox(canvas, x_pos - 4, center_y - 4, icon_w + 8, icon_h + 8, 1);
                canvas_set_color(canvas, ColorBlack);
            }
            
            for(int y = 0; y < icon_h; y++) {
                uint8_t rowbits = bmp[y];
                for(int x = 0; x < icon_w; x++) {
                    if(rowbits & (1 << (7 - x))) {
                        canvas_draw_box(canvas, x_pos + x, center_y + y, 1, 1);
                    }
                }
            }
        }
        
        /* draw left/right arrow icons next to center card */
        canvas_set_color(canvas, ColorBlack);
        int arrow_y = center_y;  /* align to center of icon */
        canvas_draw_icon(canvas, center_x_pos - 12, arrow_y, &I_button_left);
        canvas_draw_icon(canvas, center_x_pos + icon_w + 8, arrow_y, &I_button_right);
        
        return;
    }

    /* ─── MAIN MENU (three-button) ─── */
    if(app->state == GAMESTATE_MAINMENU) {
        draw_background(canvas, '1', app->menu_cam_x);
        /* layout: big play in center, small cube left, list right */
        const int cx = SCREEN_W/2;
        const int cy = SCREEN_H/2;
        /* draw background title */
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 21, 10, "GEOMETRY FLIP");

        /* ground fill and moving cube hint (behind buttons) */
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, GROUND_Y, SCREEN_W, SCREEN_H - GROUND_Y);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_line(canvas, 0, GROUND_Y, SCREEN_W - 1, GROUND_Y);
        int menu_cube_x = (int)app->menu_cube_x;
        int menu_cube_y = (int)app->menu_cube_y;
        int mcx = menu_cube_x + PLAYER_SIZE / 2;
        int mcy = menu_cube_y + PLAYER_SIZE / 2;
        const int mr = PLAYER_SIZE / 2;
        const int mbuf = 2;
        if(mcx - mr - mbuf >= 0 && mcx + mr + mbuf < SCREEN_W &&
           mcy - mr - mbuf >= 0 && mcy + mr + mbuf < SCREEN_H) {
            draw_player_rotated(canvas, mcx, mcy, app->menu_cube_angle, app->menu_cube_skin);
        }

        /* left small cube button */
        int lx = cx - 52;
        int ly = cy - 12;
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, lx, ly, 24, 24, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, lx + 1, ly + 1, 22, 22, 2);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, lx + 2, ly + 2, 20, 20, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, lx + 3, ly + 3, 18, 18, 1);
        canvas_set_color(canvas, ColorBlack);
        /* character icon */
        canvas_draw_icon(canvas, lx + 5, ly + 5, &I_character);

        /* center play button (large) */
        int px = cx - 20;
        int py = cy - 20;
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, px, py, 40, 40, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, px + 1, py + 1, 38, 38, 2);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, px + 2, py + 2, 36, 36, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, px + 3, py + 3, 34, 34, 1);
        canvas_set_color(canvas, ColorBlack);
        /* play icon */
        canvas_draw_icon(canvas, px + 10, py + 10, &I_play);

        /* right list button */
        int rx = cx + 28;
        int ry = cy - 12;
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, rx, ry, 24, 24, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, rx + 1, ry + 1, 22, 22, 2);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, rx + 2, ry + 2, 20, 20, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, rx + 3, ry + 3, 18, 18, 1);
        canvas_set_color(canvas, ColorBlack);
        /* tools icon */
        canvas_draw_icon(canvas, rx + 5, ry + 5, &I_tools);

        /* bottom button hints: Left / OK / Right */
        const int hint_center_y = SCREEN_H - 14;
        const int hint_y = SCREEN_H - 22;
        const int hint_r = 6;
        const int hint_side_r = 5;
        const int hint_x_left = 23;
        const int hint_x_center = SCREEN_W / 2;
        const int hint_x_right = SCREEN_W - 24;

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_disc(canvas, hint_x_left, hint_y, hint_side_r);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_circle(canvas, hint_x_left, hint_y, hint_side_r);
        canvas_draw_icon(canvas, hint_x_left - 2, hint_y - 3, &I_button_left);

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_disc(canvas, hint_x_center, hint_center_y, hint_r);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_circle(canvas, hint_x_center, hint_center_y, hint_r);
        canvas_draw_icon(canvas, hint_x_center - 3, hint_center_y - 3, &I_button_center);

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_disc(canvas, hint_x_right, hint_y, hint_side_r);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_circle(canvas, hint_x_right, hint_y, hint_side_r);
        canvas_draw_icon(canvas, hint_x_right - 1, hint_y - 3, &I_button_right);
        return;
    }

    /* ─── OFFICIAL LEVELS CAROUSEL ─── */
    if(app->state == GAMESTATE_OFFICIALS) {
        /* show current official level as a full-screen card, allow left/right to change */
        int idx = app->official_sel;
        if(idx < 0) idx = 0;
        if(idx >= OFFICIAL_LEVEL_COUNT) idx = OFFICIAL_LEVEL_COUNT - 1;
        canvas_set_font(canvas, FontSecondary);
        if(OFFICIAL_LEVEL_COUNT > 0) {
            canvas_set_font(canvas, FontPrimary);
            /* draw big card only when there is an official level to show */
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 12, 10, SCREEN_W - 24, SCREEN_H - 20, 4);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, 13, 11, SCREEN_W - 26, SCREEN_H - 22, 3);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 14, 12, SCREEN_W - 28, SCREEN_H - 24, 2);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_rbox(canvas, 15, 13, SCREEN_W - 30, SCREEN_H - 26, 1);
            canvas_set_color(canvas, ColorBlack);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_icon(canvas, 3, 28, &I_button_left);
            canvas_draw_icon(canvas, SCREEN_W - 7, 28, &I_button_right);
            const char* name = OFFICIAL_LEVELS[idx].name;
            canvas_draw_str_aligned(
                canvas,
                SCREEN_W / 2,
                SCREEN_H / 2,
                AlignCenter,
                AlignCenter,
                name);
        } else {
            canvas_draw_str_aligned(
                canvas,
                SCREEN_W / 2,
                SCREEN_H / 2,
                AlignCenter,
                AlignCenter,
                "No official levels");
        }
        return;
    }

    /* ─── WIN ─── */
    if(app->state == GAMESTATE_WIN) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 30, 16, "LEVEL CLEAR!");
        canvas_set_font(canvas, FontSecondary);
        char buf[32];
        snprintf(buf, sizeof(buf), "Attempts: %d", (int)app->attempt);
        canvas_draw_str(canvas, 32, 34, buf);
        canvas_draw_str(canvas, 16, 50, "OK=Levels  Back=Retry");
    }
}

/* ─── Input ──────────────────────────────────────────────────────── */

static void input_callback(InputEvent* event, void* ctx) {
    furi_message_queue_put((FuriMessageQueue*)ctx, event, 0);
}

/* ─── Entry Point ────────────────────────────────────────────────── */

int32_t geoflip(void* p) {
    UNUSED(p);

    GeoApp* app = malloc(sizeof(GeoApp));
    memset(app, 0, sizeof(GeoApp));
    app->state    = GAMESTATE_SPLASH;
    app->menu_sel = 0;
    app->custom_sel = 0;
    app->official_sel = 0;
    app->selected_skin = 0;
    app->skin_cursor = 0;
    app->current_is_official = true;
    app->current_level_idx = 0;
    app->splash_frames = 0;
    app->menu_cube_x = -PLAYER_SIZE;
    app->menu_cube_y = (float)(GROUND_Y - PLAYER_SIZE);
    app->menu_cube_vy = 0.0f;
    app->menu_cube_angle = 0.0f;
    app->menu_cube_snapping = false;
    app->menu_cube_lock_angle = 0;
    app->menu_cube_timer = 0;
    app->menu_jump_timer = 0;
    app->menu_cube_on_ground = true;
    app->menu_cube_skin = 0;

    srand((unsigned)furi_get_tick());

    /* load persisted player profile if present */
    load_player_profile(app);

    app->level_count = (int8_t)discover_levels(app->level_files, MAX_LEVELS);
    for(int i = 0; i < app->level_count; i++) {
        Level* tmp = malloc(sizeof(Level));
        if(tmp && parse_level(app->level_files[i], tmp)) {
            strncpy(app->level_names[i], tmp->name, 63);
        } else {
            const char* sl = strrchr(app->level_files[i], '/');
            strncpy(app->level_names[i], sl ? sl+1 : app->level_files[i], 63);
        }
        app->level_names[i][63] = '\0';
        if(tmp) free(tmp);
    }

    ViewPort*         vp    = view_port_alloc();
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    view_port_draw_callback_set(vp, render_callback, app);
    view_port_input_callback_set(vp, input_callback, queue);

    Gui*             gui   = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);

    bool       running = true;
    InputEvent ev;

    GameState prev_state = app->state;
    while(running) {
        while(furi_message_queue_get(queue, &ev, 0) == FuriStatusOk) {
            bool pressed  = (ev.type == InputTypePress || ev.type == InputTypeRepeat);
            bool released = (ev.type == InputTypeRelease);

            switch(app->state) {
            case GAMESTATE_SPLASH:
                /* no input during splash */
                break;

            case GAMESTATE_MENU:
                if(pressed && ev.key == InputKeyUp) {
                    int n = app->level_count ? app->level_count : 1;
                    app->custom_sel = (int8_t)((app->custom_sel - 1 + n) % n);
                }
                if(pressed && ev.key == InputKeyDown) {
                    int n = app->level_count ? app->level_count : 1;
                    app->custom_sel = (int8_t)((app->custom_sel + 1) % n);
                }
                if(pressed && ev.key == InputKeyOk && app->level_count > 0) {
                    app->attempt = 0; app->best_pct = 0;
                    game_start_level(app, app->custom_sel);
                }
                if(pressed && ev.key == InputKeyBack) app->state = GAMESTATE_MAINMENU;
                break;
            case GAMESTATE_MAINMENU:
                /* direct mapping: Left -> cube chooser, OK -> officials, Right -> custom list */
                if(pressed && ev.key == InputKeyLeft) {
                    app->skin_cursor = app->selected_skin; /* start cursor at current */
                    app->state = GAMESTATE_SKINS;
                }
                if(pressed && ev.key == InputKeyOk) {
                    app->state = GAMESTATE_OFFICIALS;
                }
                if(pressed && ev.key == InputKeyRight) {
                    app->state = GAMESTATE_MENU; /* reuse existing custom list */
                }
                if(pressed && ev.key == InputKeyBack) running = false;
                break;

            case GAMESTATE_SKINS:
                if(pressed && ev.key == InputKeyLeft) {
                    app->skin_cursor = (int8_t)((app->skin_cursor - 1 + SKIN_COUNT) % SKIN_COUNT);
                }
                if(pressed && ev.key == InputKeyRight) {
                    app->skin_cursor = (int8_t)((app->skin_cursor + 1) % SKIN_COUNT);
                }
                if(pressed && ev.key == InputKeyOk) {
                    app->selected_skin = app->skin_cursor;
                    save_player_profile(app);
                    /* immediately reload to confirm persistence */
                    load_player_profile(app);
                    app->state = GAMESTATE_MAINMENU;
                }
                if(pressed && ev.key == InputKeyBack) {
                    app->state = GAMESTATE_MAINMENU;
                }
                break;

            case GAMESTATE_OFFICIALS:
                if(pressed && ev.key == InputKeyLeft) {
                    int n = OFFICIAL_LEVEL_COUNT ? OFFICIAL_LEVEL_COUNT : 1;
                    app->official_sel = (int8_t)((app->official_sel - 1 + n) % n);
                }
                if(pressed && ev.key == InputKeyRight) {
                    int n = OFFICIAL_LEVEL_COUNT ? OFFICIAL_LEVEL_COUNT : 1;
                    app->official_sel = (int8_t)((app->official_sel + 1) % n);
                } 
                if(pressed && ev.key == InputKeyOk && OFFICIAL_LEVEL_COUNT > 0) {
                    app->attempt = 0; app->best_pct = 0;
                    game_start_official_level(app, app->official_sel);
                }
                if(pressed && ev.key == InputKeyBack) app->state = GAMESTATE_MAINMENU;
                break;

            case GAMESTATE_PLAYING:
                if(ev.key == InputKeyOk) {
                    if(pressed)  app->btn_jump = true;
                    if(released) app->btn_jump = false;
                }
                if(pressed && ev.key == InputKeyBack) app->state = GAMESTATE_PAUSE;
                break;

            case GAMESTATE_PAUSE:
                if(pressed && ev.key == InputKeyOk)  app->state = GAMESTATE_PLAYING;
                if(pressed && ev.key == InputKeyBack) {
                    app->state = app->current_is_official ? GAMESTATE_OFFICIALS : GAMESTATE_MENU;
                }
                break;

            case GAMESTATE_DEAD:
                if(pressed && ev.key == InputKeyBack) {
                    app->state = app->current_is_official ? GAMESTATE_OFFICIALS : GAMESTATE_MENU;
                }
                break;

            case GAMESTATE_WIN:
                if(pressed && ev.key == InputKeyOk) {
                    app->state = app->current_is_official ? GAMESTATE_OFFICIALS : GAMESTATE_MENU;
                }
                if(pressed && ev.key == InputKeyBack) {
                    app->attempt++;
                    game_reset(app);
                    app->state = GAMESTATE_PLAYING;
                }
                break;
            }
        }

        if(app->state == GAMESTATE_SPLASH) {
            app->splash_frames++;
            if(app->splash_frames >= 130) {
                app->state = GAMESTATE_MAINMENU;
            }
        }

        if(app->state == GAMESTATE_MAINMENU && prev_state != GAMESTATE_MAINMENU) {
            app->menu_cube_skin = (int8_t)(rand() % SKIN_COUNT);
        }

        if(app->state == GAMESTATE_MENU && prev_state != GAMESTATE_MENU) {
            /* refresh custom levels list on entry */
            app->level_count = (int8_t)discover_levels(app->level_files, MAX_LEVELS);
            for(int i = 0; i < app->level_count; i++) {
                Level* tmp = malloc(sizeof(Level));
                if(tmp && parse_level(app->level_files[i], tmp)) {
                    strncpy(app->level_names[i], tmp->name, 63);
                } else {
                    const char* sl = strrchr(app->level_files[i], '/');
                    strncpy(app->level_names[i], sl ? sl+1 : app->level_files[i], 63);
                }
                app->level_names[i][63] = '\0';
                if(tmp) free(tmp);
            }
            /* keep cursor in bounds */
            if(app->custom_sel >= app->level_count) app->custom_sel = 0;
        }

        if(app->state == GAMESTATE_MAINMENU) {
            app->menu_cam_x += SCROLL_SPEED;
            app->menu_cube_timer++;
            app->menu_jump_timer++;

            bool was_airborne = !app->menu_cube_on_ground;
            if(app->menu_cube_lock_angle > 0) app->menu_cube_lock_angle--;

            if(app->menu_jump_timer >= MENU_JUMP_PERIOD_FRAMES) {
                app->menu_jump_timer = 0;
                if(app->menu_cube_on_ground && (rand() & 1)) {
                    app->menu_cube_vy = JUMP_VEL;
                    app->menu_cube_on_ground = false;
                    app->menu_cube_lock_angle = 0;
                }
            }

            app->menu_cube_x += MENU_CUBE_SPEED;
            if(app->menu_cube_timer >= MENU_CUBE_PERIOD_FRAMES ||
               app->menu_cube_x > SCREEN_W + PLAYER_SIZE) {
                app->menu_cube_timer = 0;
                app->menu_cube_x = -PLAYER_SIZE;
            }

            app->menu_cube_vy += GRAVITY;
            app->menu_cube_y += app->menu_cube_vy;
            float ground_y = (float)(GROUND_Y - PLAYER_SIZE);
            if(app->menu_cube_y >= ground_y) {
                app->menu_cube_y = ground_y;
                app->menu_cube_vy = 0.0f;
                app->menu_cube_on_ground = true;
            }

            if(!app->menu_cube_on_ground) {
                app->menu_cube_snapping = false;
                if(app->menu_cube_lock_angle <= 0) {
                    app->menu_cube_angle += ROT_SPEED_AIR;
                    if(app->menu_cube_angle >= 360.0f) app->menu_cube_angle -= 360.0f;
                }
            } else {
                if(was_airborne && app->menu_cube_on_ground) app->menu_cube_snapping = true;
                if(app->menu_cube_snapping) {
                    float tgt = nearest_90(app->menu_cube_angle);
                    app->menu_cube_angle = angle_approach(app->menu_cube_angle, tgt, ROT_SNAP_SPEED);
                    float diff = app->menu_cube_angle - tgt;
                    if(diff < 0.0f) diff = -diff;
                    if(diff < 1.5f) { app->menu_cube_angle = tgt; app->menu_cube_snapping = false; }
                } else {
                    app->menu_cube_angle = nearest_90(app->menu_cube_angle);
                }
            }
        }

        game_update(app);

        if(app->state == GAMESTATE_DEAD) {
            app->dead_timer++;
            death_particles_update(app);
            if(app->dead_timer == 1)  notification_message(notif, &sequence_reset_vibro);
            if(app->dead_timer >= 63) game_restart_current_level(app);
        }

        view_port_update(vp);
        prev_state = app->state;
        furi_delay_ms(23); /* must be 16 */
    }

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    /* ensure profile saved on exit */
    save_player_profile(app);
    free(app);
    return 0;
}

/* ─── Player profile (persistence) ───────────────────────────────── */
static void save_player_profile(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/profile.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    /* Try opening existing file for write first, otherwise create/truncate */
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        /* try create new */
        file = storage_file_alloc(storage);
        if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
            return;
        }
    }
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)app->selected_skin);
    storage_file_write(file, buf, len);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void load_player_profile(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/profile.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* create default file */
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        save_player_profile(app);
        return;
    }
    char buf[32] = {0};
    int read = storage_file_read(file, buf, sizeof(buf)-1);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(read > 0) {
        int v = atoi(buf);
        if(v >= 0 && v < SKIN_COUNT) app->selected_skin = (int8_t)v;
    }
}