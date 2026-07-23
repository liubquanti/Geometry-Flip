#include "collision.h"
#include "../geoflip.h" /* CELL */

bool rects_overlap(int ax, int ay, int aw, int ah,
                    int bx, int by, int bw, int bh) {
    return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

bool sprite_hits_player(int sx, int sy, const uint8_t* bmp, int rot,
                         int px, int py, int pw, int ph) {
    int r = rot & 3;
    int px2 = px + pw;
    int py2 = py + ph;
    for(int y = 0; y < CELL; y++) {
        uint8_t row = bmp[y];
        for(int x = 0; x < CELL; x++) {
            if(row & (1 << (7 - x))) {
                int dx = x, dy = y;
                if(r == 1) {
                    dx = CELL - 1 - y;
                    dy = x;
                } else if(r == 2) {
                    dx = CELL - 1 - x;
                    dy = CELL - 1 - y;
                } else if(r == 3) {
                    dx = y;
                    dy = CELL - 1 - x;
                }
                int wx = sx + dx;
                int wy = sy + dy;
                if(wx >= px && wx < px2 && wy >= py && wy < py2) return true;
            }
        }
    }
    return false;
}

void mini_spike_hitbox_small(int sx, int sy, int rot, int* rx, int* ry, int* rw, int* rh) {
    int r = rot & 3;
    switch(r) {
    case 1: /* right */
        *rx = sx;     *ry = sy + 2; *rw = 4; *rh = 2; break;
    case 2: /* down */
        *rx = sx + 2; *ry = sy;     *rw = 4; *rh = 2; break;
    case 3: /* left */
        *rx = sx + 4; *ry = sy + 2; *rw = 4; *rh = 2; break;
    case 0:
    default: /* up */
        *rx = sx + 2; *ry = sy + 4; *rw = 4; *rh = 2; break;
    }
}

void mini_block_rect(int sx, int sy, int rot, int* rx, int* ry, int* rw, int* rh) {
    int r = rot & 3;
    int half = CELL / 2;
    switch(r) {
    case 1: /* right half */
        *rx = sx + half; *ry = sy; *rw = half; *rh = CELL; break;
    case 2: /* top half */
        *rx = sx; *ry = sy; *rw = CELL; *rh = half; break;
    case 3: /* left half */
        *rx = sx; *ry = sy; *rw = half; *rh = CELL; break;
    case 0:
    default: /* bottom half */
        *rx = sx; *ry = sy + half; *rw = CELL; *rh = half; break;
    }
}
