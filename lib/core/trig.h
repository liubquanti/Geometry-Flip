/* Fixed-point trig LUT and angle-snapping helpers shared by the player,
   the death particles, the menu cube, and the music-free rotation math. */
#pragma once

/* Returns 128*sin(deg) / 128*cos(deg) as integers (fixed-point, Q7). */
int isin128(int deg);
int icos128(int deg);

/* Round `a` (degrees) to the nearest multiple of 90, wrapped to [0,360). */
float nearest_90(float a);

/* Step `cur` towards `tgt` (degrees) by at most `step`, taking the shortest
   direction around the circle. */
float angle_approach(float cur, float tgt, float step);
