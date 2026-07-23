#include "trig.h"

#include <stdint.h>

/* ─── Trig LUT ───────────────────────────────────────────────────── */

static const int8_t sin_lut[10] = { 0, 22, 44, 62, 78, 90, 100, 107, 113, 117 };

int isin128(int deg) {
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
int icos128(int deg) { return isin128(deg + 90); }

/* ─── Rotation helpers ───────────────────────────────────────────── */

float nearest_90(float a) {
    /* map to [0,360) then round to nearest multiple of 90 */
    while(a <   0.0f) a += 360.0f;
    while(a >= 360.0f) a -= 360.0f;
    int seg = (int)((a + 45.0f) / 90.0f) & 3;
    return (float)(seg * 90);
}

float angle_approach(float cur, float tgt, float step) {
    float d = tgt - cur;
    while(d >  180.0f) d -= 360.0f;
    while(d < -180.0f) d += 360.0f;
    if(d >= 0.0f) cur += (d < step) ? d : step;
    else { float n = -d; cur -= (n < step) ? n : step; }
    while(cur <   0.0f) cur += 360.0f;
    while(cur >= 360.0f) cur -= 360.0f;
    return cur;
}
