/**
 * Geometry Flip for Flipper Zero
 *
 * Controls:
 *   OK    — Jump (hold = auto-jump on every landing)
 *   Back  — Pause / Exit
 *   Up/Dn — Navigate menu
 *
 * Level format: plain-text .gdlvl files
 *
 * Grid system:
 *   All objects placed on an 8×8 pixel grid.
 *   Grid origin (0,0) = bottom-left of the visible ground row.
 *   GX = grid column (0 = left edge of level, increases rightward)
 *   GY = grid row    (0 = ground level, 1 = one block above ground, ...)
 *
 *   Screen Y of object top = GROUND_Y - (GY+1)*CELL
 *   Screen X of object     = GX*CELL - cam_x
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
#define GROUND_Y        54          /* screen Y of the ground line         */
#define PLAYER_GX       2           /* player fixed grid column            */
#define PLAYER_SIZE     8
#define CELL            8           /* grid cell size in pixels            */
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

/* Convert grid Y to screen Y (top of cell) */
#define GRID_TO_SCREEN_Y(gy) (GROUND_Y - ((gy) + 1) * CELL)
/* Convert grid X to level-space X */
#define GRID_TO_LEVEL_X(gx)  ((gx) * CELL)

/* ─── Types ─────────────────────────────────────────────────────── */

typedef enum {
    OBJ_BLOCK = 0,  /* solid 8×8 block — can land on top           */
    OBJ_SPIKE,      /* triangle spike  — always kills on any touch  */
} ObjType;

typedef enum {
    DEC_STAR = 0,
    DEC_CLOUD,
    DEC_PILLAR,
} DecType;

typedef struct {
    ObjType type;
    int     gx;     /* grid X (column) */
    int     gy;     /* grid Y (row, 0 = ground level) */
} LvlObject;

typedef struct {
    DecType type;
    int     x;      /* level-space X (not grid, for finer parallax placement) */
    int     y;      /* screen Y */
} Decoration;

typedef struct {
    char        name[64];
    int         speed;        /* scroll px/frame (0 = default) */
    int         gravity_pct;
    char        bg_style;
    int         length;       /* level length in pixels */
    int         obj_count;
    int         dec_count;
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
    float   vy;
    float   py;         /* player screen Y (float, top of cube) */
    float   prev_py;
    bool    on_ground;
    bool    jump_held;

    float   angle;
    bool    snapping;

    int     cam_x;      /* camera = level-space X of left screen edge */
    Level   level;

    GameState   state;
    int         attempt;
    int         best_pct;
    uint32_t    frame;

    char    level_files[MAX_LEVELS][MAX_PATH_LEN];
    char    level_names[MAX_LEVELS][64];
    int     level_count;
    int     menu_sel;

    bool    btn_jump;
} GeoApp;

/* ─── Integer trig (LUT, no libm) ───────────────────────────────── */

static const int8_t sin_lut[10] = { 0, 22, 44, 62, 78, 90, 100, 107, 113, 117 };

static int isin128(int deg) {
    deg = ((deg % 360) + 360) % 360;
    int sign = (deg < 180) ? 1 : -1;
    int d = deg % 180;
    if(d > 90) d = 180 - d;
    int idx  = d / 10; if(idx  > 9) idx  = 9;
    int idx1 = idx + 1; if(idx1 > 9) idx1 = 9;
    int frac = (d % 10) * 256 / 10;
    int v = ((int)sin_lut[idx] * (256 - frac) + (int)sin_lut[idx1] * frac) >> 8;
    return sign * v;
}
static int icos128(int deg) { return isin128(deg + 90); }

/* ─── Draw rotated cube ──────────────────────────────────────────── */

static void draw_player_rotated(Canvas* canvas, int cx, int cy, float angle_f) {
    const int R = PLAYER_SIZE / 2;
    int ang = (int)angle_f % 360;
    if(ang < 0) ang += 360;
    int sa = isin128(ang);
    int ca = icos128(ang);
    int corners[4][2] = { {-R,-R}, {R,-R}, {R,R}, {-R,R} };
    int px[4], py[4];
    for(int i = 0; i < 4; i++) {
        int x0 = corners[i][0], y0 = corners[i][1];
        px[i] = cx + ((x0 * ca - y0 * sa + 64) >> 7);
        py[i] = cy + ((x0 * sa + y0 * ca + 64) >> 7);
    }
    for(int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        canvas_draw_line(canvas, px[i], py[i], px[j], py[j]);
    }
    canvas_draw_line(canvas, px[0], py[0], px[2], py[2]);
}

/* ─── Level Parser ───────────────────────────────────────────────── */

/*
 * Level file format (.gdlvl):
 *
 *   NAME   My Level
 *   SPEED  2          # scroll speed px/frame (default 2)
 *   GRAVITY 100       # gravity % (100 = default)
 *   BG     1          # background: 0=empty 1=stars 2=grid
 *   LENGTH 2000       # level length in pixels
 *
 *   # Objects placed on 8×8 grid:
 *   #   OBJ <TYPE> <GX> <GY>
 *   #   GX = grid column (0 = far left of level)
 *   #   GY = grid row    (0 = ground row, 1 = one block above, ...)
 *   #
 *   # Types: BLOCK  SPIKE
 *
 *   OBJ BLOCK  20  0    # block sitting on ground at column 20
 *   OBJ BLOCK  20  1    # block stacked one above
 *   OBJ SPIKE  25  0    # spike on ground at column 25
 *   OBJ SPIKE  25  1    # spike floating above ground (on top of block etc.)
 *
 *   # Decorations (level-space X, screen Y, no grid):
 *   DEC STAR   200 8
 *   DEC CLOUD  400 12
 *   DEC PILLAR 600 0
 */

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

            } else if(strncmp(line, "OBJ ", 4) == 0 && lvl->obj_count < MAX_OBJECTS) {
                char type_s[16] = {0};
                int  gx = 0, gy = 0;
                sscanf(line + 4, "%15s %d %d", type_s, &gx, &gy);
                LvlObject* o = &lvl->objects[lvl->obj_count];
                o->gx = gx;
                o->gy = gy;
                if     (strcmp(type_s, "BLOCK") == 0) o->type = OBJ_BLOCK;
                else if(strcmp(type_s, "SPIKE") == 0) o->type = OBJ_SPIKE;
                else continue; /* unknown type — skip */
                lvl->obj_count++;

            } else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                char type_s[16] = {0};
                int  x = 0, y = 0;
                sscanf(line + 4, "%15s %d %d", type_s, &x, &y);
                Decoration* d = &lvl->decorations[lvl->dec_count];
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

/* ─── Collision helpers ──────────────────────────────────────────── */

static bool rects_overlap(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

/*
 * Returns screen-space rect for an object.
 * ox, oy = top-left screen coords of the cell.
 */
static void obj_screen_rect(const LvlObject* o, int cam_x,
                             int* sx, int* sy) {
    *sx = GRID_TO_LEVEL_X(o->gx) - cam_x;
    *sy = GRID_TO_SCREEN_Y(o->gy);
}

/* ─── Rotation helpers ───────────────────────────────────────────── */

static float nearest_90(float a) {
    a = fmodf(a, 360.0f);
    if(a < 0.0f) a += 360.0f;
    int seg = (int)((a + 45.0f) / 90.0f) % 4;
    return (float)(seg * 90);
}

static float angle_approach(float current, float target, float step) {
    float diff = target - current;
    while(diff >  180.0f) diff -= 360.0f;
    while(diff < -180.0f) diff += 360.0f;
    if(diff >= 0.0f) current += (diff < step) ? diff : step;
    else { float n = -diff; current -= (n < step) ? n : step; }
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
    int speed = (app->level.speed > 0) ? app->level.speed : SCROLL_SPEED;
    app->cam_x += speed;

    /* ── physics ── */
    app->prev_py = app->py;
    float grav   = GRAVITY * (float)app->level.gravity_pct / 100.0f;
    app->vy     += grav;
    app->py     += app->vy;

    /* ── ground collision ── */
    float ground_limit = (float)(GROUND_Y - PLAYER_SIZE);
    bool  was_airborne = !app->on_ground;   /* state from END of last frame */
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

    /* ── player screen rect (shrunk 1px each side for fairness) ── */
    int player_screen_x = PLAYER_GX * CELL;
    int px_hit = player_screen_x + 1;
    int py_hit = (int)app->py + 1;
    int pw_hit = PLAYER_SIZE - 2;
    int ph_hit = PLAYER_SIZE - 2;

    /* ── object collisions ── */
    for(int i = 0; i < app->level.obj_count; i++) {
        const LvlObject* o = &app->level.objects[i];
        int sx, sy;
        obj_screen_rect(o, app->cam_x, &sx, &sy);

        /* cull off-screen */
        if(sx + CELL < 0 || sx > SCREEN_W) continue;

        if(o->type == OBJ_BLOCK) {
            /* Full CELL×CELL solid block */
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, sy, CELL, CELL)) {
                /* Landing on top: previous bottom was at/above block top */
                bool from_above = (int)app->prev_py + PLAYER_SIZE <= sy + 2;
                if(from_above && app->vy >= 0.0f) {
                    app->py        = (float)(sy - PLAYER_SIZE);
                    app->vy        = 0.0f;
                    app->on_ground = true;
                } else {
                    /* Hit from side or bottom → die */
                    app->state = GAMESTATE_DEAD;
                    return;
                }
            }
        } else { /* OBJ_SPIKE */
            /*
             * Spike hitbox: inner triangle approximation.
             * Use a smaller box: 4×4 centered bottom of the cell.
             */
            int hx = sx + 2;
            int hy = sy + 4;
            int hw = 4;
            int hh = 4;
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, hx, hy, hw, hh)) {
                app->state = GAMESTATE_DEAD;
                return;
            }
        }
    }

    /* ── jump (after collisions, on_ground is final) ── */
    if(app->on_ground) app->jump_held = false;

    if(app->btn_jump && app->on_ground && !app->jump_held) {
        app->vy        = JUMP_VEL;
        app->on_ground = false;
        app->jump_held = true;
    }

    if(!app->btn_jump) app->jump_held = false;

    /* ── rotation ── */
    bool grounded_this_frame = was_airborne && app->on_ground;

    if(!app->on_ground) {
        app->snapping = false;
        app->angle    = fmodf(app->angle + ROT_SPEED_AIR, 360.0f);
    } else {
        if(grounded_this_frame) app->snapping = true;
        if(app->snapping) {
            float target = nearest_90(app->angle);
            app->angle   = angle_approach(app->angle, target, ROT_SNAP_SPEED);
            float diff   = app->angle - target;
            if(diff < 0.0f) diff = -diff;
            if(diff < 1.5f) { app->angle = target; app->snapping = false; }
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

static void draw_spike(Canvas* canvas, int sx, int sy) {
    /* Triangle pointing up within CELL×CELL */
    int x = sx, y = sy;
    canvas_draw_line(canvas, x,        y + CELL - 1, x + CELL/2, y);
    canvas_draw_line(canvas, x + CELL/2, y,          x + CELL - 1, y + CELL - 1);
    canvas_draw_line(canvas, x,        y + CELL - 1, x + CELL - 1, y + CELL - 1);
}

static void draw_block(Canvas* canvas, int sx, int sy) {
    canvas_draw_box(canvas, sx, sy, CELL, CELL);
    /* hatching so it looks like a block, not a blob */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, sx + 1, sy + 1, sx + CELL - 2, sy + 1);
    canvas_draw_line(canvas, sx + 1, sy + 1, sx + 1, sy + CELL - 2);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_decoration(Canvas* canvas, const Decoration* d, int cam_x) {
    int sx = d->x - cam_x / 2;
    if(sx < -12 || sx > SCREEN_W + 12) return;
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
            seed = seed * 1103515245u + 12345u;
            int sx = (int)((seed >> 16) & 0x7fu) - (cam_x / 4 % SCREEN_W);
            if(sx < 0) sx += SCREEN_W;
            seed = seed * 1103515245u + 12345u;
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
            /* Menu scrolling: keep selected item visible */
            int item_height = 12;
            int menu_y_start = 24;
            int max_visible = 4;  /* max items visible on screen */
            int scroll_offset = 0;
            
            /* Calculate scroll offset to keep selected item visible */
            if(app->menu_sel > max_visible - 1) {
                scroll_offset = (app->menu_sel - max_visible + 1) * item_height;
            }
            
            for(int i = 0; i < app->level_count; i++) {
                int y = menu_y_start + i * item_height - scroll_offset;
                
                /* Only draw items visible on screen */
                if(y < 20 || y > SCREEN_H - 2) continue;
                
                if(i == app->menu_sel) {
                    canvas_draw_rbox(canvas, 2, y - 9, SCREEN_W - 4, 11, 2);
                    canvas_set_color(canvas, ColorWhite);
                }
                canvas_draw_str(canvas, 8, y, app->level_names[i]);
                canvas_set_color(canvas, ColorBlack);
            }
        }
        return;
    }

    /* ─── PLAYING / PAUSE ─── */
    if(app->state == GAMESTATE_PLAYING || app->state == GAMESTATE_PAUSE) {
        draw_background(canvas, app->level.bg_style, app->cam_x);

        for(int i = 0; i < app->level.dec_count; i++)
            draw_decoration(canvas, &app->level.decorations[i], app->cam_x);

        /* ground line */
        canvas_draw_line(canvas, 0, GROUND_Y, SCREEN_W - 1, GROUND_Y);

        /* objects */
        for(int i = 0; i < app->level.obj_count; i++) {
            const LvlObject* o = &app->level.objects[i];
            int sx, sy;
            obj_screen_rect(o, app->cam_x, &sx, &sy);
            if(sx + CELL < 0 || sx > SCREEN_W) continue;

            switch(o->type) {
            case OBJ_BLOCK: draw_block(canvas, sx, sy); break;
            case OBJ_SPIKE: draw_spike(canvas, sx, sy); break;
            }
        }

        /* player */
        int cx = PLAYER_GX * CELL + PLAYER_SIZE / 2;
        int cy = (int)app->py + PLAYER_SIZE / 2;
        draw_player_rotated(canvas, cx, cy, app->angle);

        /* HUD */
        int pct = (app->level.length > 0)
                  ? app->cam_x * 100 / app->level.length : 0;
        if(pct < 0) pct = 0;
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
        if(pct < 0) pct = 0;
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
    furi_message_queue_put((FuriMessageQueue*)ctx, event, 0);
}

/* ─── App Entry Point ────────────────────────────────────────────── */

int32_t geoflip(void* p) {
    UNUSED(p);

    GeoApp* app = malloc(sizeof(GeoApp));
    memset(app, 0, sizeof(GeoApp));
    app->state    = GAMESTATE_MENU;
    app->menu_sel = 0;

    app->level_count = discover_levels(app->level_files, MAX_LEVELS);
    for(int i = 0; i < app->level_count; i++) {
        Level* tmp = malloc(sizeof(Level));
        if(tmp && parse_level(app->level_files[i], tmp)) {
            strncpy(app->level_names[i], tmp->name, sizeof(app->level_names[i]) - 1);
        } else {
            const char* slash = strrchr(app->level_files[i], '/');
            strncpy(app->level_names[i],
                    slash ? slash + 1 : app->level_files[i],
                    sizeof(app->level_names[i]) - 1);
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
                if(pressed && ev.key == InputKeyOk)   app->state = GAMESTATE_PLAYING;
                if(pressed && ev.key == InputKeyBack)  app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_DEAD:
                if(pressed && ev.key == InputKeyOk)   game_start_level(app, app->menu_sel);
                if(pressed && ev.key == InputKeyBack)  app->state = GAMESTATE_MENU;
                break;

            case GAMESTATE_WIN:
                if(pressed && ev.key == InputKeyOk)   app->state = GAMESTATE_MENU;
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

        if(app->state == GAMESTATE_DEAD && app->frame == 1)
            notification_message(notif, &sequence_reset_vibro);

        furi_delay_ms(16);
    }

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}