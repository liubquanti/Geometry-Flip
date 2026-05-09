/**
 * Geometry Dash for Flipper Zero
 * 
 * Controls:
 *   OK / Center button  — Jump
 *   Back               — Pause / Exit
 *   Up/Down            — Navigate menus
 * 
 * Level format: plain-text .gdlvl files (see levels/level1.gdlvl)
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
#define PLAYER_X        20
#define PLAYER_SIZE     8
#define GRAVITY         0.25f
#define JUMP_VEL        (-3.1f)
#define SCROLL_SPEED    2.0f
#define MAX_OBSTACLES   64
#define MAX_DECORATIONS 32
#define LEVEL_DIR       "/ext/geoflip/levels"
#define MAX_LEVELS      16
#define MAX_FILENAME    64
#define MAX_PATH_LEN    256

/* Rotation speed while airborne (degrees per frame) */
#define ROT_SPEED_AIR   9.0f
/* Snap-to-nearest-90 speed on landing (degrees per frame) */
#define ROT_SNAP_SPEED  18.0f

/* ─── Types ─────────────────────────────────────────────────────── */

typedef enum {
    OBS_SPIKE = 0,
    OBS_BLOCK,
    OBS_TALL_BLOCK,
    OBS_PLATFORM,
} ObstacleType;

typedef enum {
    DEC_STAR = 0,
    DEC_CLOUD,
    DEC_PILLAR,
} DecorationType;

typedef struct {
    ObstacleType type;
    int          x;
    int          y;
    int          w, h;
} Obstacle;

typedef struct {
    DecorationType type;
    int            x;
    int            y;
} Decoration;

typedef struct {
    char        name[64];
    int         speed;
    int         gravity_pct;
    char        bg_style;
    int         obs_count;
    int         dec_count;
    int         length;
    Obstacle    obstacles[MAX_OBSTACLES];
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
    /* player physics */
    float   vy;
    float   py;
    float   prev_py;
    bool    on_ground;
    bool    jump_held;

    /* rotation — continuous float angle, snaps to 90 on landing */
    float   angle;      /* current rotation in degrees [0, 360) */
    bool    snapping;   /* true while snap-to-90 is in progress */

    /* world */
    int     cam_x;
    Level   level;

    /* game state */
    GameState   state;
    int         attempt;
    int         best_pct;
    uint32_t    frame;

    /* menu */
    char    level_files[MAX_LEVELS][MAX_PATH_LEN];
    char    level_names[MAX_LEVELS][64];
    int     level_count;
    int     menu_sel;

    /* input */
    bool    btn_jump;
} GeoApp;

/* ─── Integer trig (no libm needed at runtime) ───────────────────── */
/*
 * LUT: sin values for 0..90 degrees in steps of 10, scaled * 128.
 * sin(0)=0, sin(10)≈0.174*128=22, sin(20)≈0.342*128=44, ...
 */
static const int8_t sin_lut[10] = { 0, 22, 44, 62, 78, 90, 100, 107, 113, 117 };

/* Returns sin(deg) * 128, integer. */
static int isin128(int deg) {
    deg = ((deg % 360) + 360) % 360;
    int sign = (deg < 180) ? 1 : -1;
    int d = deg % 180;
    if(d > 90) d = 180 - d;   /* mirror second half */
    int idx = d / 10;
    if(idx > 9) idx = 9;
    int idx1 = idx + 1; if(idx1 > 9) idx1 = 9;
    int frac = (d % 10) * 256 / 10;
    int v = ((int)sin_lut[idx] * (256 - frac) + (int)sin_lut[idx1] * frac) >> 8;
    return sign * v;
}
static int icos128(int deg) { return isin128(deg + 90); }

/* ─── Cube rotation rendering ────────────────────────────────────── */
/*
 * Draws a rotating square (cube top-view) with an interior diagonal
 * so you can see it actually spinning.  Uses integer trig only.
 *
 * cx, cy = centre of the cube in screen pixels
 * angle_f = current rotation angle in degrees
 */
static void draw_player_rotated(Canvas* canvas, int cx, int cy, float angle_f) {
    /* half-side length in pixels */
    const int R = PLAYER_SIZE / 2;  /* 4 */

    int ang = (int)angle_f % 360;
    if(ang < 0) ang += 360;

    int sa = isin128(ang);
    int ca = icos128(ang);

    /*
     * 4 corners of axis-aligned square (* 128 for fixed point):
     *   (-R,-R), (R,-R), (R,R), (-R,R)
     * After rotation:
     *   x' = x*cos - y*sin
     *   y' = x*sin + y*cos
     * All intermediate values are * 128, final pixel = (val + 64) >> 7
     */
    int px[4], py_c[4];
    int corners[4][2] = { {-R,-R}, {R,-R}, {R,R}, {-R,R} };
    for(int i = 0; i < 4; i++) {
        int x0 = corners[i][0];
        int y0 = corners[i][1];
        /* * 128: x0*ca, y0*sa are already *128 */
        int rx = x0 * ca - y0 * sa;  /* * 128 */
        int ry = x0 * sa + y0 * ca;  /* * 128 */
        px[i]   = cx + ((rx + 64) >> 7);
        py_c[i] = cy + ((ry + 64) >> 7);
    }

    /* draw 4 outer edges */
    for(int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        canvas_draw_line(canvas, px[i], py_c[i], px[j], py_c[j]);
    }

    /* inner diagonal corner[0]<->corner[2] shows rotation direction */
    canvas_draw_line(canvas, px[0], py_c[0], px[2], py_c[2]);
}

/* ─── Level Parser ───────────────────────────────────────────────── */

static bool parse_level(const char* path, Level* lvl) {
    memset(lvl, 0, sizeof(Level));
    lvl->speed       = (int)SCROLL_SPEED;
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
    size_t idx = 0;
    char   ch;

    while(storage_file_read(file, &ch, 1) == 1) {
        if(ch == '\n' || ch == '\r') {
            if(idx == 0) continue;
            line[idx] = '\0';
            idx = 0;

            if(line[0] == '#') continue;

            if(strncmp(line, "NAME ", 5) == 0) {
                strncpy(lvl->name, line + 5, sizeof(lvl->name) - 1);
            } else if(strncmp(line, "SPEED ", 6) == 0) {
                lvl->speed = atoi(line + 6);
            } else if(strncmp(line, "GRAVITY ", 8) == 0) {
                lvl->gravity_pct = atoi(line + 8);
            } else if(strncmp(line, "BG ", 3) == 0) {
                lvl->bg_style = line[3];
            } else if(strncmp(line, "LENGTH ", 7) == 0) {
                lvl->length = atoi(line + 7);
            } else if(strncmp(line, "OBS ", 4) == 0 && lvl->obs_count < MAX_OBSTACLES) {
                Obstacle* o = &lvl->obstacles[lvl->obs_count];
                char type_s[16] = {0};
                int  x = 0, y = 0, w = 0, h = 0;
                sscanf(line + 4, "%15s %d %d %d %d", type_s, &x, &y, &w, &h);
                o->x = x; o->y = y; o->w = w; o->h = h;
                if     (strcmp(type_s, "SPIKE")      == 0) o->type = OBS_SPIKE;
                else if(strcmp(type_s, "BLOCK")      == 0) o->type = OBS_BLOCK;
                else if(strcmp(type_s, "TALL_BLOCK") == 0) o->type = OBS_TALL_BLOCK;
                else if(strcmp(type_s, "PLATFORM")   == 0) o->type = OBS_PLATFORM;
                lvl->obs_count++;
            } else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                Decoration* d = &lvl->decorations[lvl->dec_count];
                char type_s[16] = {0};
                int  x = 0, y = 0;
                sscanf(line + 4, "%15s %d %d", type_s, &x, &y);
                d->x = x; d->y = y;
                if     (strcmp(type_s, "STAR")   == 0) d->type = DEC_STAR;
                else if(strcmp(type_s, "CLOUD")  == 0) d->type = DEC_CLOUD;
                else if(strcmp(type_s, "PILLAR") == 0) d->type = DEC_PILLAR;
                lvl->dec_count++;
            }
        } else if(idx < sizeof(line) - 1) {
            line[idx++] = ch;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
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
                size_t len = strlen(name);
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

/* ─── Collision Helpers ──────────────────────────────────────────── */

static bool rects_overlap(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

static void get_obstacle_rect(const Obstacle* o,
                               int* ox, int* oy, int* ow, int* oh) {
    *ox = o->x;
    switch(o->type) {
    case OBS_SPIKE:
        *oy = GROUND_Y - 8; *ow = 8;  *oh = 8;  break;
    case OBS_BLOCK:
        *oy = GROUND_Y - 8; *ow = 8;  *oh = 8;  break;
    case OBS_TALL_BLOCK:
        *oy = GROUND_Y - 16; *ow = 8; *oh = 16; break;
    case OBS_PLATFORM:
        *oy = (o->y ? o->y : 40);
        *ow = (o->w ? o->w : 28);
        *oh = (o->h ? o->h : 6);
        break;
    }
}

static void get_obstacle_hitbox(const Obstacle* o,
                                int* ox, int* oy, int* ow, int* oh) {
    if(o->type == OBS_SPIKE) {
        int fx, fy, fw, fh;
        get_obstacle_rect(o, &fx, &fy, &fw, &fh);
        *ow = fw / 2;
        *oh = fh / 2;
        *ox = o->x + (fw - *ow) / 2;
        *oy = GROUND_Y - *oh;
    } else {
        get_obstacle_rect(o, ox, oy, ow, oh);
    }
}

/* ─── Rotation helpers ───────────────────────────────────────────── */

/* Return the nearest multiple of 90 to angle (in [0, 360)). */
static float nearest_90(float a) {
    a = fmodf(a, 360.0f);
    if(a < 0.0f) a += 360.0f;
    int seg = (int)((a + 45.0f) / 90.0f) % 4;
    return (float)(seg * 90);
}

/* Move current toward target by at most step, wrapping correctly. */
static float angle_approach(float current, float target, float step) {
    float diff = target - current;
    while(diff >  180.0f) diff -= 360.0f;
    while(diff < -180.0f) diff += 360.0f;
    if(diff >= 0.0f) {
        current += (diff < step) ? diff : step;
    } else {
        float neg = -diff;
        current -= (neg < step) ? neg : step;
    }
    return fmodf(current + 360.0f, 360.0f);
}

/* ─── Game Logic ─────────────────────────────────────────────────── */

static void game_reset(GeoApp* app) {
    app->vy        = 0.0f;
    app->py        = (float)(GROUND_Y - PLAYER_SIZE);
    app->prev_py   = app->py;
    app->on_ground = true;
    app->jump_held = false;
    app->angle     = 0.0f;
    app->snapping  = false;
    app->cam_x     = 0;
    app->frame     = 0;
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

    /* ── scroll ── */
    int speed = (app->level.speed > 0) ? app->level.speed : (int)SCROLL_SPEED;
    app->cam_x += speed;

    /* ── physics ── */
    app->prev_py = app->py;
    float grav   = GRAVITY * (float)app->level.gravity_pct / 100.0f;
    app->vy     += grav;
    app->py     += app->vy;

    /* ── ground ── */
    float ground_limit = (float)(GROUND_Y - PLAYER_SIZE);

    /* was_airborne = state as resolved at END of PREVIOUS frame */
    bool was_airborne = !app->on_ground;

    /* Reset; ground/platform collisions below will set it back to true */
    app->on_ground = false;

    if(app->py >= ground_limit) {
        app->py        = ground_limit;
        app->vy        = 0.0f;
        app->on_ground = true;
    }

    /* ── fall out of screen ── */
    if(app->py > (float)(SCREEN_H + 4)) {
        app->state = GAMESTATE_DEAD;
        return;
    }

    /* ── obstacle collision ── */
    int py_i = (int)app->py;

    for(int i = 0; i < app->level.obs_count; i++) {
        const Obstacle* o = &app->level.obstacles[i];
        int ox, oy, ow, oh;
        get_obstacle_hitbox(o, &ox, &oy, &ow, &oh);
        int screen_ox = ox - app->cam_x;

        /* shrink player hitbox 1px for fairness */
        int hx = PLAYER_X + 1;
        int hy = py_i + 1;
        int hw = PLAYER_SIZE - 2;
        int hh = PLAYER_SIZE - 2;

        if(o->type == OBS_PLATFORM || o->type == OBS_BLOCK || o->type == OBS_TALL_BLOCK) {
            if(rects_overlap(hx, hy, hw, hh, screen_ox, oy, ow, oh)) {
                bool from_above = (int)(app->prev_py) + PLAYER_SIZE <= oy + 2;
                bool at_top     = hy + hh <= oy + oh / 2;
                if((from_above || at_top) && app->vy >= -1.0f) {
                    app->py        = (float)(oy - PLAYER_SIZE);
                    app->vy        = 0.0f;
                    app->on_ground = true;
                } else {
                    app->state = GAMESTATE_DEAD;
                    return;
                }
            }
        } else {
            if(rects_overlap(hx, hy, hw, hh, screen_ox, oy, ow, oh)) {
                app->state = GAMESTATE_DEAD;
                return;
            }
        }
    }

    /* ── jump input (after collisions so on_ground is final) ── */

    /* Reset jump_held when we land — this makes holding the button
     * auto-jump every time the cube touches ground, exactly like GD. */
    if(app->on_ground) {
        app->jump_held = false;
    }

    if(app->btn_jump && app->on_ground && !app->jump_held) {
        app->vy        = JUMP_VEL;
        app->on_ground = false;
        app->jump_held = true;
    }

    if(!app->btn_jump) {
        app->jump_held = false;
    }

    /* ── rotation (on_ground is now fully resolved for this frame) ── */
    {
        /* grounded_this_frame = landed THIS frame (was up, now down) */
        bool grounded_this_frame = was_airborne && app->on_ground;

        if(!app->on_ground) {
            /* Airborne: spin clockwise continuously */
            app->snapping = false;
            app->angle    = fmodf(app->angle + ROT_SPEED_AIR, 360.0f);
        } else {
            if(grounded_this_frame) {
                /* Just landed — begin snap to nearest 90° */
                app->snapping = true;
            }
            if(app->snapping) {
                float target = nearest_90(app->angle);
                app->angle   = angle_approach(app->angle, target, ROT_SNAP_SPEED);
                float diff   = app->angle - target;
                if(diff < 0.0f) diff = -diff;
                if(diff < 1.5f) {
                    app->angle    = target;
                    app->snapping = false;
                }
            }
            /* else: grounded and fully snapped — angle stays frozen */
        }
    }

    /* ── win ── */
    if(app->cam_x >= app->level.length) {
        if(100 > app->best_pct) app->best_pct = 100;
        app->state = GAMESTATE_WIN;
    } else {
        int pct = app->cam_x * 100 / app->level.length;
        if(pct > app->best_pct) app->best_pct = pct;
    }
}

/* ─── Rendering ──────────────────────────────────────────────────── */

static void draw_spike(Canvas* canvas, int x, int y) {
    canvas_draw_line(canvas, x,   y+7, x+4, y);
    canvas_draw_line(canvas, x+4, y,   x+7, y+7);
}

static void draw_block(Canvas* canvas, int x, int y, int w, int h) {
    canvas_draw_box(canvas, x, y, w, h);
    for(int i = 2; i < w; i += 4)
        canvas_draw_line(canvas, x+i, y+1, x+i, y+h-2);
}

static void draw_decoration(Canvas* canvas, const Decoration* d, int cam_x) {
    int sx = d->x - cam_x / 2;   /* parallax: half scroll speed */
    if(sx < -8 || sx > SCREEN_W + 8) return;
    switch(d->type) {
    case DEC_STAR:
        canvas_draw_dot(canvas, sx,   d->y);
        canvas_draw_dot(canvas, sx+1, d->y);
        canvas_draw_dot(canvas, sx,   d->y+1);
        break;
    case DEC_CLOUD:
        canvas_draw_frame(canvas, sx,   d->y+2, 10, 5);
        canvas_draw_frame(canvas, sx+2, d->y,   6,  4);
        break;
    case DEC_PILLAR:
        canvas_draw_frame(canvas, sx, 40, 4, GROUND_Y - 40);
        break;
    }
}

static void draw_background(Canvas* canvas, char style, int cam_x) {
    if(style == '1') {
        uint32_t seed = 12345;
        for(int i = 0; i < 20; i++) {
            seed = seed * 1103515245u + 12345;
            int sx = (int)((seed >> 16) & 0x7fu) - (cam_x / 4 % SCREEN_W);
            if(sx < 0) sx += SCREEN_W;
            seed = seed * 1103515245u + 12345;
            int sy = (int)((seed >> 16) & 0x1fu);
            canvas_draw_dot(canvas, sx, sy);
        }
    } else if(style == '2') {
        for(int x = -(cam_x % 16); x < SCREEN_W; x += 16)
            canvas_draw_line(canvas, x, 0, x, GROUND_Y - 1);
        for(int y = 0; y < GROUND_Y; y += 16)
            canvas_draw_line(canvas, 0, y, SCREEN_W - 1, y);
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
            for(int i = 0; i < app->level_count; i++) {
                int y = 24 + i * 12;
                if(i == app->menu_sel) {
                    canvas_draw_rbox(canvas, 2, y - 9, SCREEN_W - 4, 11, 2);
                    canvas_set_color(canvas, ColorWhite);
                }
                canvas_draw_str(canvas, 8, y, app->level_names[i]);
                canvas_set_color(canvas, ColorBlack);
            }
            canvas_draw_str(canvas, 2, 62, "OK=Play  Up/Dn=Select");
        }
        return;
    }

    /* ─── PLAYING / PAUSE ─── */
    if(app->state == GAMESTATE_PLAYING || app->state == GAMESTATE_PAUSE) {
        draw_background(canvas, app->level.bg_style, app->cam_x);

        for(int i = 0; i < app->level.dec_count; i++)
            draw_decoration(canvas, &app->level.decorations[i], app->cam_x);

        /* ground */
        canvas_draw_line(canvas, 0, GROUND_Y, SCREEN_W - 1, GROUND_Y);

        /* obstacles */
        for(int i = 0; i < app->level.obs_count; i++) {
            const Obstacle* o = &app->level.obstacles[i];
            int ox, oy, ow, oh;
            get_obstacle_rect(o, &ox, &oy, &ow, &oh);
            int sx = ox - app->cam_x;
            if(sx > SCREEN_W || sx + ow < 0) continue;
            switch(o->type) {
            case OBS_SPIKE:      draw_spike(canvas, sx, oy);          break;
            case OBS_BLOCK:      draw_block(canvas, sx, oy, ow, oh);  break;
            case OBS_TALL_BLOCK: draw_block(canvas, sx, oy, ow, oh);  break;
            case OBS_PLATFORM:   draw_block(canvas, sx, oy, ow, oh);  break;
            }
        }

        /* player — rotated cube, centre-based */
        int cx = PLAYER_X + PLAYER_SIZE / 2;
        int cy = (int)app->py + PLAYER_SIZE / 2;
        draw_player_rotated(canvas, cx, cy, app->angle);

        /* HUD: progress bar + percent */
        int pct = (app->level.length > 0)
                  ? app->cam_x * 100 / app->level.length : 0;
        if(pct > 100) pct = 100;

        int bar_w = (SCREEN_W - 4) * pct / 100;
        canvas_draw_frame(canvas, 2, 1, SCREEN_W - 4, 4);
        if(bar_w > 0) canvas_draw_box(canvas, 2, 1, bar_w, 4);

        canvas_set_font(canvas, FontSecondary);
        char hud[16];
        snprintf(hud, sizeof(hud), "%d%%", pct);
        canvas_draw_str(canvas, 2, 14, hud);

        if(app->state == GAMESTATE_PAUSE) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 38, 35, "PAUSED");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 28, 47, "OK=Resume  Back=Quit");
        }
        return;
    }

    /* ─── DEAD ─── */
    if(app->state == GAMESTATE_DEAD) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 35, 16, "YOU DIED");
        canvas_set_font(canvas, FontSecondary);
        char buf[32];
        int pct = (app->level.length > 0)
                  ? app->cam_x * 100 / app->level.length : 0;
        if(pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), "Reached: %d%%", pct);
        canvas_draw_str(canvas, 28, 30, buf);
        snprintf(buf, sizeof(buf), "Best:    %d%%", app->best_pct);
        canvas_draw_str(canvas, 28, 40, buf);
        snprintf(buf, sizeof(buf), "Attempt: %d", app->attempt);
        canvas_draw_str(canvas, 28, 50, buf);
        canvas_draw_str(canvas, 14, 62, "OK=Retry  Back=Menu");
        return;
    }

    /* ─── WIN ─── */
    if(app->state == GAMESTATE_WIN) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 30, 16, "LEVEL CLEAR!");
        canvas_set_font(canvas, FontSecondary);
        char buf[32];
        snprintf(buf, sizeof(buf), "Attempts: %d", app->attempt);
        canvas_draw_str(canvas, 32, 34, buf);
        canvas_draw_str(canvas, 20, 50, "OK=Menu  Back=Retry");
    }
}

/* ─── Input ──────────────────────────────────────────────────────── */

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = (FuriMessageQueue*)ctx;
    furi_message_queue_put(queue, event, 0);
}

/* ─── App Lifecycle ──────────────────────────────────────────────── */

int32_t geoflip(void* p) {
    UNUSED(p);

    GeoApp* app = malloc(sizeof(GeoApp));
    memset(app, 0, sizeof(GeoApp));
    app->state    = GAMESTATE_MENU;
    app->menu_sel = 0;

    /* discover levels & pre-load names */
    app->level_count = discover_levels(app->level_files, MAX_LEVELS);
    for(int i = 0; i < app->level_count; i++) {
        Level* tmp = malloc(sizeof(Level));
        if(tmp && parse_level(app->level_files[i], tmp)) {
            strncpy(app->level_names[i], tmp->name, sizeof(app->level_names[i]) - 1);
        } else {
            const char* slash = strrchr(app->level_files[i], '/');
            const char* fname = slash ? slash + 1 : app->level_files[i];
            strncpy(app->level_names[i], fname, sizeof(app->level_names[i]) - 1);
        }
        app->level_names[i][sizeof(app->level_names[i]) - 1] = '\0';
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
                    app->menu_sel = (app->menu_sel - 1 + n) % n;
                }
                if(pressed && ev.key == InputKeyDown) {
                    int n = app->level_count ? app->level_count : 1;
                    app->menu_sel = (app->menu_sel + 1) % n;
                }
                if(pressed && ev.key == InputKeyOk && app->level_count > 0) {
                    app->attempt  = 0;
                    app->best_pct = 0;
                    game_start_level(app, app->menu_sel);
                }
                if(pressed && ev.key == InputKeyBack)
                    running = false;
                break;

            case GAMESTATE_PLAYING:
                if(ev.key == InputKeyOk) {
                    if(pressed)  app->btn_jump = true;
                    if(released) app->btn_jump = false;
                }
                if(pressed && ev.key == InputKeyBack)
                    app->state = GAMESTATE_PAUSE;
                break;

            case GAMESTATE_PAUSE:
                if(pressed && ev.key == InputKeyOk)
                    app->state = GAMESTATE_PLAYING;
                if(pressed && ev.key == InputKeyBack)
                    app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_DEAD:
                if(pressed && ev.key == InputKeyOk)
                    game_start_level(app, app->menu_sel);
                if(pressed && ev.key == InputKeyBack)
                    app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_WIN:
                if(pressed && ev.key == InputKeyOk)
                    app->state = GAMESTATE_MENU;
                if(pressed && ev.key == InputKeyBack) {
                    app->attempt++;
                    game_reset(app);
                    app->state = GAMESTATE_PLAYING;
                }
                break;
            }
        }

        game_update(app);
        view_port_update(vp);

        /* vibrate once on death */
        if(app->state == GAMESTATE_DEAD && app->frame == 1)
            notification_message(notif, &sequence_reset_vibro);

        furi_delay_ms(16);   /* ~60 fps */
    }

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}