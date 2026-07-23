#include "game.h"

#include "trig.h"
#include "collision.h"
#include "level_parser.h"
#include "music_player.h"
#include "profile.h"
#include "../data/levels.h"
#include "../data/objects.h"

/* ─── Game Logic ─────────────────────────────────────────────────── */

void game_reset(GeoApp* app) {
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
    app->cam_y        = 0.0f;
    app->frame        = 0;
    app->dead_timer   = 0;
    app->dead_pct     = 0;
    app->dead_new_best = false;
    app->death_particle_count = 0;
    /* reset sliding window to beginning of sorted object list */
    app->window_start = 0;

    /* (re)start the tune from the beginning; music_acquired / hardware
       ownership is left untouched here — see music_pause/music_resume/
       music_release, driven by the state machine in the main loop. */
    music_use_level_track(app);
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

void death_particles_update(GeoApp* app) {
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

void death_particles_draw(Canvas* canvas, const GeoApp* app) {
    int camy = (int)app->cam_y;
    for(uint8_t i = 0; i < app->death_particle_count; i++) {
        const DeathParticle* p = &app->death_particles[i];
        if(p->life == 0) continue;
        int x = (int)p->x;
        int y = (int)p->y + camy;
        if(x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) continue;
        canvas_draw_dot(canvas, x, y);
        if(p->life > 18 && x + 1 < SCREEN_W) canvas_draw_dot(canvas, x + 1, y);
    }
}

int game_pct(const GeoApp* app) {
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
    if(app->dead_new_best) {
        app->best_pct = (int16_t)p;
        /* If this was an official level, update saved progression */
        if(app->current_is_official && app->current_level_idx >= 0 && app->current_level_idx < OFFICIAL_LEVEL_COUNT) {
            if(app->official_prog[app->current_level_idx] < (int8_t)p) {
                app->official_prog[app->current_level_idx] = (int8_t)p;
                save_official_progress(app);
            }
        }
    }
    app->dead_timer    = 0;
    death_particles_spawn(app);
    app->state         = GAMESTATE_DEAD;
}

void game_start_level(GeoApp* app, int idx) {
    if(idx < 0 || idx >= app->level_count) return;
    if(!parse_level(app->level_files[idx], &app->level)) return;
    app->current_is_official = false;
    app->current_level_idx = (int8_t)idx;
    app->attempt++;
    game_reset(app);
    app->intro_active = (app->attempt == 1);
    app->intro_timer = 0;
    app->intro_player_x = -PLAYER_SIZE;
    app->state = GAMESTATE_PLAYING;
}

void game_start_official_level(GeoApp* app, int idx) {
    if(idx < 0 || idx >= OFFICIAL_LEVEL_COUNT) return;
    if(!parse_level_from_text(OFFICIAL_LEVELS[idx].data, &app->level)) return;
    app->current_is_official = true;
    app->current_level_idx = (int8_t)idx;
    /* initialize best_pct from saved official progression so NEW BEST compares correctly */
    if(app->official_prog && idx >= 0 && idx < OFFICIAL_LEVEL_COUNT) {
        int v = (int)app->official_prog[idx];
        if(v < 0) v = 0;
        if(v > 100) v = 100;
        app->best_pct = (int16_t)v;
    } else {
        app->best_pct = 0;
    }
    app->attempt++;
    game_reset(app);
    app->intro_active = (app->attempt == 1);
    app->intro_timer = 0;
    app->intro_player_x = -PLAYER_SIZE;
    app->state = GAMESTATE_PLAYING;
}

void game_restart_current_level(GeoApp* app) {
    /* app->level is already parsed and unchanged between attempts — avoid
       re-reading/re-parsing it (slow, especially for large custom levels
       loaded from storage). Just reset gameplay state. */
    app->attempt++;
    game_reset(app);
    app->intro_active = (app->attempt == 1);
    app->intro_timer = 0;
    app->intro_player_x = -PLAYER_SIZE;
    app->state = GAMESTATE_PLAYING;
}

void game_update(GeoApp* app) {
    if(app->state != GAMESTATE_PLAYING) return;

    app->frame++;

    if(app->intro_active) {
        app->intro_timer++;
        app->btn_jump = false;
        if(app->intro_timer <= INTRO_HIDE_FRAMES) {
            return;
        }
        uint16_t t = app->intro_timer - INTRO_HIDE_FRAMES;
        if(t <= INTRO_ENTRY_FRAMES) {
            float start_x = -PLAYER_SIZE;
            float end_x = (float)(PLAYER_GX * CELL);
            float k = (float)t / (float)INTRO_ENTRY_FRAMES;
            if(k < 0.0f) k = 0.0f;
            if(k > 1.0f) k = 1.0f;
            app->intro_player_x = start_x + (end_x - start_x) * k;
            return;
        }
        app->intro_active = false;
        app->intro_player_x = (float)(PLAYER_GX * CELL);
        app->py = (float)(GROUND_Y - PLAYER_SIZE);
        app->prev_py = app->py;
        app->vy = 0.0f;
        app->on_ground = true;
        app->angle = 0.0f;
        app->snapping = false;
        app->lock_angle = 0;
    }

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
    const int px_hit = PLAYER_GX * CELL;
    const int py_hit = (int)app->py;
    const int pw_hit = PLAYER_SIZE;
    const int ph_hit = PLAYER_SIZE;
    const int player_right = px_hit + pw_hit;
    const float prev_bottom = app->prev_py + PLAYER_SIZE;
    const float curr_bottom = app->py + PLAYER_SIZE;

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
            /* Snap to the top face as soon as the cube is within a 1px band above it.
               This keeps the visible cube flush while still killing true side hits. */
            bool horizontal = player_right >= sx - 1 && px_hit <= sx + CELL + 1;
            bool supports_top = app->vy >= 0.0f && horizontal &&
                                prev_bottom <= (float)(sy + 1) &&
                                curr_bottom >= (float)(sy - 1);

            if(supports_top) {
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
            } else if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, sx, sy, CELL, CELL)) {
                game_begin_death(app); return;
            }
        } else if(o->type == OBJ_SPIKE) {
            if(sprite_hits_player(sx, sy, SPR_SPIKE, o->rot,
                                  px_hit, py_hit, pw_hit, ph_hit)) {
                game_begin_death(app); return;
            }
        } else if(o->type == OBJ_MINI_SPIKE) {
            int rx = 0, ry = 0, rw = 0, rh = 0;
            mini_spike_hitbox_small(sx, sy, o->rot, &rx, &ry, &rw, &rh);
            if(rects_overlap(px_hit, py_hit, pw_hit, ph_hit, rx, ry, rw, rh)) {
                game_begin_death(app); return;
            }
        } else if(o->type == OBJ_MINI_BLOCK) {
            /* Mini block: 4px tall, can land on top like regular block */
            int rx = 0, ry = 0, rw = 0, rh = 0;
            mini_block_rect(sx, sy, o->rot, &rx, &ry, &rw, &rh);
            bool horizontal = player_right >= rx - 1 && px_hit <= rx + rw + 1;
            bool supports_top = app->vy >= 0.0f && horizontal &&
                                prev_bottom <= (float)(ry + 1) &&
                                curr_bottom >= (float)(ry - 1);
            if(!app->on_ground && supports_top) {
                app->py        = (float)(ry - PLAYER_SIZE);
                app->vy        = 0.0f;
                app->on_ground = true;
                app->angle     = nearest_90(app->angle);
                app->snapping  = false;
                app->landed_on_block = true;
                app->lock_angle = 6;
                continue;
            }
            if(sprite_hits_player(sx, sy, SPR_MINI_BLOCK, o->rot,
                                  px_hit, py_hit, pw_hit, ph_hit)) {
                game_begin_death(app); return;
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

    /* If we just walked/fell off a block, don't keep the landing lock alive.
       This lets the cube rotate normally while dropping without a jump. */
    if(was_airborne == false && !app->on_ground) {
        app->lock_angle = 0;
    }

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

    /* ── vertical camera follow ──
       Once the player climbs above CAM_Y_TRIGGER_ROWS blocks, smoothly pan
       the camera up so the action stays on screen; eases back down when the
       player descends again. Purely visual — physics/collision stay in the
       original (unshifted) coordinate space above. */
    {
        float trigger_py = (float)(GROUND_Y - CAM_Y_TRIGGER_ROWS * CELL - PLAYER_SIZE);
        float target_cam_y = 0.0f;
        if(app->py < trigger_py) target_cam_y = trigger_py - app->py;
        app->cam_y += (target_cam_y - app->cam_y) * CAM_Y_SMOOTH;
    }

    /* ── win ── */
    if(app->cam_x >= app->level.length) {
        if(100 > app->best_pct) app->best_pct = 100;
        /* If an official level was completed, mark progress as 100% and save */
        if(app->current_is_official && app->current_level_idx >= 0 && app->current_level_idx < OFFICIAL_LEVEL_COUNT) {
            if(app->official_prog[app->current_level_idx] < 100) {
                app->official_prog[app->current_level_idx] = 100;
                save_official_progress(app);
            }
        }
        app->state = GAMESTATE_WIN;
    }
}
