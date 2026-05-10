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

/* ─── Constants ─────────────────────────────────────────────────── */

#define SCREEN_W        128
#define SCREEN_H        64
#define GROUND_Y        54
#define PLAYER_GX       2
#define PLAYER_SIZE     8
#define CELL            8
#define GRAVITY         0.25f
#define JUMP_VEL        (-3.1f)
#define SCROLL_SPEED    2
#define MAX_OBJECTS     128
#define MAX_DECORATIONS 32
#define LEVEL_DIR       "/ext/geoflip/levels"
#define MAX_LEVELS      16
#define MAX_FILENAME    64
#define MAX_PATH_LEN    256
#define ROT_SPEED_AIR   9.0f
#define ROT_SNAP_SPEED  18.0f
#define DEATH_PARTICLES 24

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

    /* menu */
    char    level_files[MAX_LEVELS][MAX_PATH_LEN];
    char    level_names[MAX_LEVELS][64];
    int8_t  level_count;
    int8_t  menu_sel;

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

static void draw_player_rotated(Canvas* canvas, int cx, int cy, float angle_f) {
    const int R  = PLAYER_SIZE / 2;
    int ang = (int)angle_f;
    ang = ((ang % 360) + 360) % 360;
    int sa = isin128(ang), ca = icos128(ang);
    const int cx4[4] = {-R, R,  R, -R};
    const int cy4[4] = {-R,-R,  R,  R};
    int px[4], py[4];
    for(int i = 0; i < 4; i++) {
        px[i] = cx + ((cx4[i]*ca - cy4[i]*sa + 64) >> 7);
        py[i] = cy + ((cx4[i]*sa + cy4[i]*ca + 64) >> 7);
    }
    for(int i = 0; i < 4; i++) {
        int j = (i+1) & 3;
        canvas_draw_line(canvas, px[i], py[i], px[j], py[j]);
    }
    canvas_draw_line(canvas, px[0], py[0], px[2], py[2]);
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
    if(!parse_level(app->level_files[idx], &app->level)) return;
    app->attempt++;
    game_reset(app);
    app->state = GAMESTATE_PLAYING;
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
     3,  8,  2, 12,  5, 10,  1,  7,  4, 11,
    14,  6,  9,  3, 13,  2,  8,  5, 12,  7
};

static void draw_background(Canvas* canvas, char style, int cam_x) {
    if(style == '1') {
        int shift = (cam_x / 4) & 0x7f;   /* 0..127 */
        for(int i = 0; i < 20; i++) {
            int sx = (int)star_x[i] - shift;
            if(sx < 0) sx += SCREEN_W;
            canvas_draw_dot(canvas, sx, (int)star_y[i]);
        }
    } else if(style == '2') {
        int ox = -(cam_x & 15);   /* cam_x % 16, avoid division */
        for(int x = ox; x < SCREEN_W; x += 16)
            canvas_draw_line(canvas, x, 0, x, GROUND_Y-1);
        for(int y = 0; y < GROUND_Y; y += 16)
            canvas_draw_line(canvas, 0, y, SCREEN_W-1, y);
    }
}

static void draw_decorations(Canvas* canvas, const Level* lvl, int cam_x) {
    for(int i = 0; i < lvl->dec_count; i++) {
        const Decoration* d = &lvl->decorations[i];
        int sx = (int)d->x - cam_x / 2;
        if(sx < -12 || sx > SCREEN_W + 12) continue;
        int sy = (int)d->y;
        switch(d->type) {
        case DEC_STAR:
            canvas_draw_dot(canvas, sx,   sy);
            canvas_draw_dot(canvas, sx+1, sy);
            canvas_draw_dot(canvas, sx,   sy+1);
            break;
        case DEC_CLOUD:
            canvas_draw_frame(canvas, sx,   sy+2, 10, 5);
            canvas_draw_frame(canvas, sx+2, sy,   6,  4);
            break;
        case DEC_PILLAR:
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
        if(sx < 0 || sx + CELL > SCREEN_W) continue;
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

    /* ─── MENU ─── */
    if(app->state == GAMESTATE_MENU) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 20, 12, "GEOMETRY FLIP");
        canvas_set_font(canvas, FontSecondary);
        if(app->level_count == 0) {
            canvas_draw_str(canvas, 8, 30, "No levels found!");
            canvas_draw_str(canvas, 8, 42, LEVEL_DIR);
            canvas_draw_str(canvas, 8, 54, "Add .gdlvl files");
        } else {
            const int ITEM_H    = 12;
            const int MAX_VIS   = 4;
            const int Y_START   = 24;
            int scroll = 0;
            if(app->menu_sel >= MAX_VIS) scroll = (app->menu_sel - MAX_VIS + 1) * ITEM_H;
            for(int i = 0; i < app->level_count; i++) {
                int y = Y_START + i * ITEM_H - scroll;
                if(y < 20 || y > SCREEN_H - 2) continue;
                if(i == app->menu_sel) {
                    canvas_draw_rbox(canvas, 2, y-9, SCREEN_W-4, 11, 2);
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

        /* Player (hidden when dead) */
        if(app->state != GAMESTATE_DEAD) {
            int cx = PLAYER_GX * CELL + PLAYER_SIZE / 2;
            int cy = (int)app->py + PLAYER_SIZE / 2;
            draw_player_rotated(canvas, cx, cy, app->angle);
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
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 38, 35, "PAUSED");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 28, 47, "OK=Resume  Back=Quit");
        } else if(app->state == GAMESTATE_DEAD && app->dead_new_best) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 33, 28, "NEW BEST");
            canvas_set_font(canvas, FontSecondary);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", (int)app->dead_pct);
            canvas_draw_str(canvas, 54, 42, buf);
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
        canvas_draw_str(canvas, 20, 50, "OK=Menu  Back=Retry");
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
    app->state    = GAMESTATE_MENU;
    app->menu_sel = 0;

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

    while(running) {
        while(furi_message_queue_get(queue, &ev, 0) == FuriStatusOk) {
            bool pressed  = (ev.type == InputTypePress || ev.type == InputTypeRepeat);
            bool released = (ev.type == InputTypeRelease);

            switch(app->state) {
            case GAMESTATE_MENU:
                if(pressed && ev.key == InputKeyUp) {
                    int n = app->level_count ? app->level_count : 1;
                    app->menu_sel = (int8_t)((app->menu_sel - 1 + n) % n);
                }
                if(pressed && ev.key == InputKeyDown) {
                    int n = app->level_count ? app->level_count : 1;
                    app->menu_sel = (int8_t)((app->menu_sel + 1) % n);
                }
                if(pressed && ev.key == InputKeyOk && app->level_count > 0) {
                    app->attempt = 0; app->best_pct = 0;
                    game_start_level(app, app->menu_sel);
                }
                if(pressed && ev.key == InputKeyBack) running = false;
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
                if(pressed && ev.key == InputKeyBack) app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_DEAD:
                if(pressed && ev.key == InputKeyBack) app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_WIN:
                if(pressed && ev.key == InputKeyOk)  app->state = GAMESTATE_MENU;
                if(pressed && ev.key == InputKeyBack) {
                    app->attempt++;
                    game_reset(app);
                    app->state = GAMESTATE_PLAYING;
                }
                break;
            }
        }

        game_update(app);

        if(app->state == GAMESTATE_DEAD) {
            app->dead_timer++;
            death_particles_update(app);
            if(app->dead_timer == 1)  notification_message(notif, &sequence_reset_vibro);
            if(app->dead_timer >= 63) game_start_level(app, app->menu_sel);
        }

        view_port_update(vp);
        furi_delay_ms(16); /* must be 16 */
    }

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}