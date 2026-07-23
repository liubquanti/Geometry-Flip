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
 *
 * Source layout: shared types/constants live in lib/geoflip.h; each
 * subsystem (level parsing, collision, physics, music, rendering,
 * persistence) lives under lib/core/. This file wires them together —
 * input handling and the per-frame main loop.
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#include "geoflip.h"
#include "core/trig.h"
#include "core/level_parser.h"
#include "core/game.h"
#include "core/music_player.h"
#include "core/render.h"
#include "core/profile.h"
#include "data/skins.h"
#include "data/levels.h"

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

    /* allocate official progress array */
    if(OFFICIAL_LEVEL_COUNT > 0) {
        app->official_prog = malloc(sizeof(int8_t) * OFFICIAL_LEVEL_COUNT);
        if(app->official_prog) memset(app->official_prog, 0, sizeof(int8_t) * OFFICIAL_LEVEL_COUNT);
    } else app->official_prog = NULL;

    srand((unsigned)furi_get_tick());

    /* make sure app directories exist before first read/write */
    ensure_app_storage_dirs();

    /* load persisted player profile if present */
    load_player_profile(app);
    /* load official-level progression (percent per level) */
    load_official_progress(app);

    app->level_count = (int8_t)discover_levels(app->level_files, MAX_LEVELS);
    for(int i = 0; i < app->level_count; i++) {
        Level* tmp = malloc(sizeof(Level));
        if(tmp && parse_level(app->level_files[i], tmp)) {
            strncpy(app->level_names[i], tmp->name, 63);
            strncpy(app->level_difficulty[i], tmp->difficulty[0] ? tmp->difficulty : "Easy", 15);
        } else {
            const char* sl = strrchr(app->level_files[i], '/');
            strncpy(app->level_names[i], sl ? sl+1 : app->level_files[i], 63);
            strncpy(app->level_difficulty[i], "Easy", 15);
        }
        app->level_names[i][63] = '\0';
        app->level_difficulty[i][15] = '\0';
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
                    int pages = (OFFICIAL_LEVEL_COUNT > 0) ? (OFFICIAL_LEVEL_COUNT + 1) : 1;
                    app->official_sel = (int8_t)((app->official_sel - 1 + pages) % pages);
                }
                if(pressed && ev.key == InputKeyRight) {
                    int pages = (OFFICIAL_LEVEL_COUNT > 0) ? (OFFICIAL_LEVEL_COUNT + 1) : 1;
                    app->official_sel = (int8_t)((app->official_sel + 1) % pages);
                }
                if(pressed && ev.key == InputKeyOk) {
                    /* Only start a level when a real official level is selected */
                    if(OFFICIAL_LEVEL_COUNT > 0 && app->official_sel < OFFICIAL_LEVEL_COUNT) {
                        app->attempt = 0; app->best_pct = 0;
                        game_start_official_level(app, app->official_sel);
                    }
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
                app->menu_cube_skin = (int8_t)(rand() % SKIN_COUNT);
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

        /* ── music state machine ──
           Driven by prev_state (last frame) vs app->state (this frame,
           after input/game_update/restart above have settled it). Held
           off while intro_active so the level tune starts once the
           slide-in intro finishes, not the instant GAMESTATE_PLAYING
           begins. Outside of gameplay (main menu/skins/level lists) the
           main-menu tune plays continuously — see music_use_menu_track —
           right up until a level starts. Held off during GAMESTATE_SPLASH
           too, so it only kicks in once the main menu itself is shown. */
        bool in_level = (app->state == GAMESTATE_PLAYING || app->state == GAMESTATE_PAUSE ||
                          app->state == GAMESTATE_DEAD || app->state == GAMESTATE_WIN);
        if(in_level) {
            app->music_is_menu = false;
            if(app->state == GAMESTATE_PLAYING && !app->intro_active) {
                if(prev_state == GAMESTATE_PAUSE) music_resume(app);
                music_update(app);
            } else if(prev_state == GAMESTATE_PLAYING &&
                      (app->state == GAMESTATE_PAUSE || app->state == GAMESTATE_DEAD ||
                       app->state == GAMESTATE_WIN)) {
                music_pause(app); /* stop the tone, keep the speaker for a quick resume */
            }
        } else if(app->state == GAMESTATE_SPLASH) {
            app->music_is_menu = false; /* not started yet — wait for the main menu */
        } else {
            if(!app->music_is_menu) {
                music_use_menu_track(app); /* just landed on a menu screen — (re)start the tune */
                app->music_is_menu = true;
            }
            music_update(app);
        }

        view_port_update(vp);
        prev_state = app->state;
        furi_delay_ms(23); /* must be 23 */
    }

    music_release(app);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    /* ensure profile saved on exit */
    save_player_profile(app);
    /* free official progress array */
    if(app->official_prog) free(app->official_prog);
    free(app);
    return 0;
}
