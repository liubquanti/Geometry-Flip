/* Axis-aligned rect/sprite hit-testing used by both gameplay collision
   (lib/core/game.c) and rendering extent checks (lib/core/render.c). */
#pragma once

#include <stdint.h>
#include <stdbool.h>

bool rects_overlap(int ax, int ay, int aw, int ah,
                    int bx, int by, int bw, int bh);

/* Pixel-accurate hit test between an 8x8 sprite bitmap (rotated by `rot`,
   0..3 => 0/90/180/270 degrees) placed at (sx,sy) and the player's rect. */
bool sprite_hits_player(int sx, int sy, const uint8_t* bmp, int rot,
                         int px, int py, int pw, int ph);

/* Small hitbox for the mini-spike's pointed edge, oriented by `rot`. */
void mini_spike_hitbox_small(int sx, int sy, int rot, int* rx, int* ry, int* rw, int* rh);

/* Half-cell solid rect for the mini-block, oriented by `rot`. */
void mini_block_rect(int sx, int sy, int rot, int* rx, int* ry, int* rw, int* rh);
