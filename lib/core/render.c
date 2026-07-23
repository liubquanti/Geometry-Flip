#include "render.h"

#include <string.h>
#include <stdio.h>

#include "trig.h"
#include "collision.h"
#include "game.h"
#include "music_player.h"
#include "../data/skins.h"
#include "../data/objects.h"
#include "../data/levels.h"
#include "../interface/icons.h"

/* ─── Draw rotated cube ──────────────────────────────────────────── */

static void draw_player_rotated(Canvas* canvas, int cx, int cy, float angle_f, int skin) {
    int ang = (int)angle_f;
    ang = ((ang % 360) + 360) % 360;
    int sa = isin128(ang), ca = icos128(ang);
    /* draw skin pixels (8x8) rotated with same trig to preserve animation */
    if(skin >= 0 && skin < SKIN_COUNT) {
        const uint8_t* bmp = SKINS[skin];
        for(int j = 0; j < PLAYER_SIZE; j++) {
            uint8_t row = bmp[j];
            for(int i = 0; i < PLAYER_SIZE; i++) {
                if(row & (1 << (7 - i))) {
                    /* Rotate in half-pixel space so the 8x8 cube keeps all 8 columns/rows. */
                    int x0 = i * 2 - (PLAYER_SIZE - 1);
                    int y0 = j * 2 - (PLAYER_SIZE - 1);
                    int pxp = cx + ((x0 * ca - y0 * sa) >> 8);
                    int pyp = cy + ((x0 * sa + y0 * ca) >> 8);
                    canvas_draw_box(canvas, pxp, pyp, 1, 1);
                }
            }
        }
    }
}

/* ─── Rendering ──────────────────────────────────────────────────── */

static void draw_sprite(Canvas* canvas, int sx, int sy, const uint8_t* bmp) {
    for(int y = 0; y < CELL; y++) {
        int py = sy + y;
        if(py < 0 || py >= SCREEN_H) continue;
        uint8_t row = bmp[y];
        for(int x = 0; x < CELL; x++) {
            if(row & (1 << (7 - x))) {
                int px = sx + x;
                if(px < 0 || px >= SCREEN_W) continue;
                canvas_draw_dot(canvas, px, py);
            }
        }
    }
}

static void draw_sprite_rotated(Canvas* canvas, int sx, int sy, const uint8_t* bmp, int rot) {
    int r = rot & 3;
    if(r == 0) {
        draw_sprite(canvas, sx, sy, bmp);
        return;
    }
    for(int y = 0; y < CELL; y++) {
        uint8_t row = bmp[y];
        for(int x = 0; x < CELL; x++) {
            if(row & (1 << (7 - x))) {
                int dx = 0, dy = 0;
                if(r == 1) {
                    dx = CELL - 1 - y;
                    dy = x;
                } else if(r == 2) {
                    dx = CELL - 1 - x;
                    dy = CELL - 1 - y;
                } else {
                    dx = y;
                    dy = CELL - 1 - x;
                }
                int px = sx + dx;
                int py = sy + dy;
                if(px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
                canvas_draw_dot(canvas, px, py);
            }
        }
    }
}

static void draw_spike_rotated(Canvas* canvas, int sx, int sy, int rot) {
    draw_sprite_rotated(canvas, sx, sy, SPR_SPIKE, rot);
}

static void draw_block(Canvas* canvas, int sx, int sy, int rot) {
    draw_sprite_rotated(canvas, sx, sy, SPR_BLOCK, rot);
}

static void draw_mini_spike_rotated(Canvas* canvas, int sx, int sy, int rot) {
    draw_sprite_rotated(canvas, sx, sy, SPR_MINI_SPIKE, rot);
}

static void draw_mini_block(Canvas* canvas, int sx, int sy, int rot) {
    draw_sprite_rotated(canvas, sx, sy, SPR_MINI_BLOCK, rot);
}

static void draw_jumper(Canvas* canvas, int sx, int sy) {
    draw_sprite(canvas, sx, sy, SPR_JUMPER);
}

static void draw_sphere(Canvas* canvas, int sx, int sy) {
    draw_sprite(canvas, sx, sy, SPR_SPHERE);
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

/* Bottom-center overlay shown for VOLUME_OVERLAY_FRAMES after Up/Down
   changes app->sound_volume from the main menu or pause menu. Sized to
   its content (rather than a fixed width the content is centered inside)
   so the three gaps — margin-to-icon, icon-to-bar, last-segment-to-margin
   — come out equal by construction, with every bar segment the same
   width. */
static void draw_volume_overlay(Canvas* canvas, const GeoApp* app) {
    const int margin  = 6;
    const int icon_w  = 8;
    const int seg_w    = 3;
    const int seg_gap  = 1;
    const int bar_w = SOUND_VOLUME_MAX * seg_w + (SOUND_VOLUME_MAX - 1) * seg_gap;

    const int card_w = margin * 3 + icon_w + bar_w - ( margin / 2);
    const int card_h = 18;
    const int card_x = (SCREEN_W - card_w) / 2;
    const int card_y = SCREEN_H - card_h - 3;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rbox(canvas, card_x, card_y, card_w, card_h, 3);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, card_x + 1, card_y + 1, card_w - 2, card_h - 2, 2);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rbox(canvas, card_x + 2, card_y + 2, card_w - 4, card_h - 4, 2);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, card_x + 3, card_y + 3, card_w - 6, card_h - 6, 1);
    canvas_set_color(canvas, ColorBlack);

    if(music_system_muted()) {
        /* Stealth Mode is on — Up/Down can't change anything, so show
           three muted-speaker icons instead of the usual icon+bar to make
           that plain, rather than a bar that looks adjustable but isn't. */
        const int gap = margin;
        const int total_w = icon_w * 3 + gap * 2;
        int ix = card_x + (card_w - total_w) / 2;
        const int iy = card_y + 5;
        for(int i = 0; i < 3; i++) {
            canvas_draw_icon(canvas, ix, iy, &I_muted);
            ix += icon_w + gap;
        }
        return;
    }

    canvas_draw_icon(canvas, card_x + margin, card_y + 5, app->sound_volume > 0 ? &I_unmuted : &I_muted);

    const int bar_x = card_x + ( margin / 2) + icon_w + margin;
    const int bar_y = card_y + 6;
    const int bar_h = 6;
    int sx = bar_x;
    for(int i = 0; i < SOUND_VOLUME_MAX; i++) {
        if(i < app->sound_volume) canvas_draw_box(canvas, sx, bar_y, seg_w, bar_h);
        else canvas_draw_frame(canvas, sx, bar_y, seg_w, bar_h);
        sx += seg_w + seg_gap;
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



static void draw_decorations(Canvas* canvas, const Level* lvl, int cam_x, int cam_y) {
    for(int i = 0; i < lvl->dec_count; i++) {
        const Decoration* d = &lvl->decorations[i];
        int sx = (int)d->x - cam_x / 2;
        int sy = (int)d->y + cam_y;
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
    int camy = (int)app->cam_y;
    for(int i = app->window_start; i < app->level.obj_count; i++) {
        const LvlObject* o = &app->level.objects[i];
        if(o->gx > right_edge_gx) break;
            int sx = o->gx * CELL - app->cam_x;
            int sy = GROUND_Y - (o->gy + 1) * CELL + camy;

        /* Compute vertical extents per object type to avoid partial vertical clipping
           and expensive draw operations when objects are off-screen above/below. */
        int topY = sy;
        int bottomY = sy + CELL;
        if(o->type == OBJ_MINI_SPIKE) {
            /* mini spike bounds depend on rotation */
            if((o->rot & 3) == 0) {
                topY = sy + (CELL / 2);
                bottomY = sy + CELL;
            } else if((o->rot & 3) == 2) {
                topY = sy;
                bottomY = sy + (CELL / 2);
            } else {
                topY = sy;
                bottomY = sy + CELL;
            }
        } else if(o->type == OBJ_MINI_BLOCK) {
            int rx = 0, ry = 0, rw = 0, rh = 0;
            mini_block_rect(sx, sy, o->rot, &rx, &ry, &rw, &rh);
            topY = ry;
            bottomY = ry + rh;
        } else if(o->type == OBJ_JUMPER) {
            int jumper_h = 2;
            topY = sy + CELL - jumper_h;
            bottomY = sy + CELL;
        }

        /* Skip only objects that are completely off-screen — sprite drawing
           clips each pixel individually now, so partially visible objects
           (e.g. at the top/bottom edge while the camera pans) still draw
           correctly instead of popping out entirely. */
        if(sx + CELL <= 0 || sx >= SCREEN_W) continue;
        if(bottomY <= 0 || topY >= SCREEN_H) continue;

        switch(o->type) {
        case OBJ_BLOCK:       draw_block(canvas, sx, sy, o->rot); break;
        case OBJ_SPIKE:       draw_spike_rotated(canvas, sx, sy, o->rot); break;
        case OBJ_MINI_SPIKE:  draw_mini_spike_rotated(canvas, sx, sy, o->rot); break;
        case OBJ_MINI_BLOCK:  draw_mini_block(canvas, sx, sy, o->rot); break;
        case OBJ_JUMPER:      draw_jumper(canvas, sx, sy); break;
        case OBJ_SPHERE:      draw_sphere(canvas, sx, sy); break;
        default: break;
        }
    }
}

void render_callback(Canvas* canvas, void* ctx) {
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
            canvas_draw_str_aligned(
                canvas,
                SCREEN_W / 2,
                SCREEN_H / 2,
                AlignCenter,
                AlignCenter,
                "No custom levels found!");
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
                /* draw difficulty icon before the name */
                const Icon* diff_icon = NULL;
                const char* dstr = app->level_difficulty[i];
                if(dstr) {
                    if(strcmp(dstr, "Easy") == 0) diff_icon = &I_easy;
                    else if(strcmp(dstr, "Hard") == 0) diff_icon = &I_hard;
                    else if(strcmp(dstr, "Insane") == 0) diff_icon = &I_insane;
                    else if(strcmp(dstr, "Demon") == 0) diff_icon = &I_demon;
                }
                int name_x = 8;
                if(diff_icon) {
                    int icon_x = 4;
                    int icon_y = y - 8; /* align icon vertically with text */
                    canvas_draw_icon(canvas, icon_x, icon_y, diff_icon);
                    name_x = icon_x + 12;
                }
                canvas_draw_str(canvas, name_x, y, app->level_names[i]);
                canvas_set_color(canvas, ColorBlack);
            }
        }
        return;
    }

    /* ─── PLAYING / PAUSE / DEAD ─── */
    if(app->state == GAMESTATE_PLAYING ||
       app->state == GAMESTATE_PAUSE   ||
       app->state == GAMESTATE_DEAD) {

        int camy = (int)app->cam_y;
        int ground_sy = GROUND_Y + camy;

        draw_background(canvas, app->level.bg_style, app->cam_x);
        draw_decorations(canvas, &app->level, app->cam_x, camy);

        /* Ground fill (white) — moves down/off-screen as the camera pans up */
        if(ground_sy < SCREEN_H) {
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, 0, ground_sy, SCREEN_W, SCREEN_H - ground_sy);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_line(canvas, 0, ground_sy, SCREEN_W-1, ground_sy);
        }

        /* Attempt label — scrolls with level, visible at start */
        {
            int ax = 40 - app->cam_x;
            int ay = GROUND_Y - 30 + camy;
            if(ax > -60 && ax < SCREEN_W && ay >= 0 && ay < SCREEN_H) {
                canvas_set_font(canvas, FontPrimary);
                char buf[16];
                snprintf(buf, sizeof(buf), "Attempt %d", (int)app->attempt);
                canvas_draw_str(canvas, ax, ay, buf);
            }
        }

        draw_objects(canvas, app);

        /* Player (hidden when dead). Draw only when fully inside screen to avoid costly partial clipping */
        if(app->state != GAMESTATE_DEAD) {
            bool draw_player = true;
            int cx = PLAYER_GX * CELL + PLAYER_SIZE / 2;
            int cy = (int)app->py + PLAYER_SIZE / 2 + camy;
            if(app->intro_active) {
                if(app->intro_timer <= INTRO_HIDE_FRAMES) {
                    draw_player = false;
                } else {
                    cx = (int)app->intro_player_x + PLAYER_SIZE / 2;
                    cy = (int)(GROUND_Y - PLAYER_SIZE) + PLAYER_SIZE / 2 + camy;
                }
            }
            if(draw_player) {
                const int R = PLAYER_SIZE / 2;
                /* Extend buffer by 2px to give cushion at edges before skipping draw */
                const int buf = 2;
                if(cx - R - buf >= 0 && cx + R + buf < SCREEN_W &&
                   cy - R - buf >= 0 && cy + R + buf < SCREEN_H) {
                    draw_player_rotated(canvas, cx, cy, app->angle, app->selected_skin);
                }
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
            const int card_x = 31;
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
        if(app->state == GAMESTATE_PAUSE && app->volume_overlay_timer > 0) {
            draw_volume_overlay(canvas, app);
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

        if(app->volume_overlay_timer > 0) draw_volume_overlay(canvas, app);
        return;
    }

    /* ─── OFFICIAL LEVELS CAROUSEL ─── */
    if(app->state == GAMESTATE_OFFICIALS) {
        /* show current official level as a full-screen card, allow left/right to change
           an extra trailing page is reserved for "Coming Soon!" */
        int pages = (OFFICIAL_LEVEL_COUNT > 0) ? (OFFICIAL_LEVEL_COUNT + 1) : 1;
        int idx = app->official_sel;
        if(idx < 0) idx = 0;
        if(idx >= pages) idx = pages - 1;
        canvas_set_font(canvas, FontSecondary);
        if(OFFICIAL_LEVEL_COUNT > 0) {
            /* draw navigation arrows for both real levels and Coming Soon */
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_icon(canvas, 3, 28, &I_button_left);
            canvas_draw_icon(canvas, SCREEN_W - 7, 28, &I_button_right);
            /* if idx points to a real official level -> draw full card, else draw Coming Soon */
            if(idx < OFFICIAL_LEVEL_COUNT) {
                canvas_set_font(canvas, FontSecondary);
                /* draw big card */
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, 12, 10, SCREEN_W - 24, SCREEN_H - 20, 4);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_rbox(canvas, 13, 11, SCREEN_W - 26, SCREEN_H - 22, 3);
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, 14, 12, SCREEN_W - 28, SCREEN_H - 24, 2);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_rbox(canvas, 15, 13, SCREEN_W - 30, SCREEN_H - 26, 1);
                canvas_set_color(canvas, ColorBlack);
                /* arrows drawn above */
                const char* name = OFFICIAL_LEVELS[idx].name;
                canvas_draw_str_aligned(
                    canvas,
                    SCREEN_W / 2,
                    SCREEN_H / 2 - 3,
                    AlignCenter,
                    AlignCenter,
                    name);
                /* draw difficulty icon */
                const char* data = OFFICIAL_LEVELS[idx].data;
                const char* key = "DIFICULTY ";
                const char* p = data ? strstr(data, key) : NULL;
                const Icon* diff_icon = NULL;
                if(p) {
                    p += strlen(key);
                    char token[16] = {0};
                    int ti = 0;
                    while(*p && *p != '\n' && *p != '\r' && ti < (int)sizeof(token)-1) token[ti++] = *p++;
                    token[ti] = '\0';
                    if(strcmp(token, "Easy") == 0) diff_icon = &I_easy;
                    else if(strcmp(token, "Hard") == 0) diff_icon = &I_hard;
                    else if(strcmp(token, "Insane") == 0) diff_icon = &I_insane;
                    else if(strcmp(token, "Demon") == 0) diff_icon = &I_demon;
                }
                if(diff_icon) {
                    int icon_x = SCREEN_W/2 - 43; /* restored original placement */
                    int icon_y = SCREEN_H/2 - 8;
                    canvas_draw_icon(canvas, icon_x, icon_y, diff_icon);
                }
                /* draw progress bar */
                int prog = 0;
                if(idx >= 0 && idx < OFFICIAL_LEVEL_COUNT && app->official_prog) {
                    prog = app->official_prog[idx];
                    if(prog < 0) prog = 0;
                    if(prog > 100) prog = 100;
                }
                const int pb_x = 21;
                const int pb_w = 86;
                const int pb_h = 4;
                const int pb_y = SCREEN_H / 2 + 4;
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, pb_x, pb_y, pb_w, pb_h);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_box(canvas, pb_x + 1, pb_y + 1, pb_w - 2, pb_h - 2);
                int fill_w = (prog * (pb_w - 2)) / 100;
                if(fill_w > 0) {
                    canvas_set_color(canvas, ColorBlack);
                    canvas_draw_box(canvas, pb_x + 1, pb_y + 1, fill_w, pb_h - 2);
                }
            } else {
                /* Coming Soon page */
                canvas_set_font(canvas, FontSecondary);
                canvas_draw_str_aligned(
                    canvas,
                    SCREEN_W/2,
                    SCREEN_H/2,
                    AlignCenter,
                    AlignCenter,
                    "Coming Soon!");
            }
            /* draw page dots for pages */
            if(pages > 1) {
                const int dots_y = SCREEN_H - 5;
                const int dot_r = 2;
                const int spacing = 10;
                int total_w = (pages - 1) * spacing;
                int start_x = (SCREEN_W / 2) - (total_w / 2);
                for(int i = 0; i < pages; i++) {
                    int cx = start_x + i * spacing;
                    if(i == idx) {
                        canvas_set_color(canvas, ColorBlack);
                        canvas_draw_disc(canvas, cx, dots_y, dot_r);
                        canvas_set_color(canvas, ColorBlack);
                        canvas_draw_circle(canvas, cx, dots_y, dot_r);
                    } else {
                        canvas_set_color(canvas, ColorWhite);
                        canvas_draw_disc(canvas, cx, dots_y, dot_r);
                        canvas_set_color(canvas, ColorBlack);
                        canvas_draw_circle(canvas, cx, dots_y, dot_r);
                    }
                }
            }
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
        const int card_x = 21;
        const int card_y = 17;
        const int card_w = 86;
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
            "LEVEL CLEAR!");
        canvas_set_font(canvas, FontSecondary);
        char buf[32];
        snprintf(buf, sizeof(buf), "Attempts: %d", (int)app->attempt);
        /* draw single centered line with label and number */
        canvas_draw_str_aligned(
            canvas,
            card_x + card_w / 2,
            card_y + 21,
            AlignCenter,
            AlignCenter,
            buf);
    }
}
