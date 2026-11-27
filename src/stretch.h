/* SPDX-License-Identifier: MPL-2.0 */
/* The time-stretch engines the kit's Warp (src/kit.c) and the looper's Warp (src/looper.c) share (looper round 2, the
 * owner's: "we should add different warp stretch engines"; factored out of kit.c's pages v2 code, unchanged):
 *  - Grains: two Hann grains half a grain apart (their windows sum to one) reading at the pitch's rate; each half
 *    grain the one whose window ended restarts at the playhead (plus a jitter).
 *  - Beats: the same, the restart aligned with the fading grain's next frames (WSOLA: the start within +-search of
 *    the playhead whose next frames best match, a normalised cross-correlation, mono, every 4th frame over 256).
 * The playhead moves at the time's rate (the kit: its speed; the looper: the song's position), so the time and the
 * pitch are apart. Header only (static inline), no allocation, real-time safe. */
#ifndef RFM14_STRETCH_H
#define RFM14_STRETCH_H
#include <math.h>
#include <stdint.h>

#define ST_HANN 1024              /* the window table's steps (st_hann_make fills ST_HANN + 1) */
typedef struct {
    double pos;                   /* the grain's read position (source frames) */
    double inc;                   /* .. source frames a frame (its pitch) */
    uint32_t age;                 /* frames since it started (its window) */
    int on;
} st_grain_t;
/* a sound and the part a stretch reads: [s, e) of frames, its way (+1, -1) */
typedef struct {
    const int16_t *pcm;           /* stereo, interleaved */
    uint32_t frames;
    double s, e;
    int dir;
} st_src_t;
typedef double (*st_fold_fn)(const void *u, double p);   /* a position folded into what plays (a loop, the region) */

static inline void st_hann_make(float h[ST_HANN + 1])
{
    int p;
    for (p = 0; p <= ST_HANN; p++)
        h[p] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * p / ST_HANN));
}
/* a source frame at p (linear between frames; past the sound's ends its edge) */
static inline void st_frame_at(const int16_t *pcm, uint32_t frames, double p, float *l, float *r)
{
    const uint32_t last = frames - 1;
    const double ps = p < 0 ? 0 : p;
    const uint32_t a = (uint32_t)ps > last ? last : (uint32_t)ps, b = a + 1 > last ? last : a + 1;
    const float fr = (float)(ps - a);
    *l = pcm[2 * a] + (pcm[2 * b] - pcm[2 * a]) * fr;
    *r = pcm[2 * a + 1] + (pcm[2 * b + 1] - pcm[2 * a + 1]) * fr;
}
/* (Beats) WSOLA: the start within +-jit of the playhead ph whose next frames best match those at nat (the frames the
 * fading grain plays next) */
static inline double st_align(const st_src_t *x, double jit, double nat, double ph)
{
    const int16_t *pcm = x->pcm;
    const int64_t last = (int64_t)x->frames - 1, S = (int64_t)jit, L = 256;
    int64_t o, j, best = 0;
    double bestc = -1e30;
    const int64_t n0 = (int64_t)nat, p0 = (int64_t)ph;
    if (S < 4 || n0 < 0 || n0 + L * x->dir > last || n0 + L * x->dir < 0)
        return ph;
    for (o = -S; o <= S; o += 4) {
        const int64_t c = p0 + o;
        double xy = 0, yy = 1e-9;
        if (c < (int64_t)x->s || c > (int64_t)x->e - 1 || c + L * x->dir < 0 || c + L * x->dir > last)
            continue;
        for (j = 0; j < L; j += 4) {
            const int64_t ia = n0 + j * x->dir, ib = c + j * x->dir;
            const float a = (float)pcm[2 * ia] + pcm[2 * ia + 1], b = (float)pcm[2 * ib] + pcm[2 * ib + 1];
            xy += a * b, yy += b * b;
        }
        xy /= sqrt(yy);
        if (xy > bestc)
            bestc = xy, best = o;
    }
    return ph + (double)best;
}
/* where grain g (its window ended) starts again: at the playhead; Beats: aligned with the other grain's next frames
 * (folded); Grains: plus jit times rnd (-1..1) */
static inline double st_restart(const st_src_t *x, int beats, double jit, const st_grain_t *other, double ph, float rnd,
                                st_fold_fn fold, const void *u)
{
    if (beats)
        return st_align(x, jit, fold(u, other->pos + x->dir * other->inc * other->age), ph);
    return jit > 0 ? ph + jit * rnd : ph;
}
/* the two grains' start: both at pos, the first from its middle (so the attack is kept) */
static inline void st_start(st_grain_t g[2], double pos, double inc, uint32_t glen)
{
    g[0].on = 1, g[0].pos = pos, g[0].inc = inc, g[0].age = glen / 2;
    g[1].on = 1, g[1].pos = pos, g[1].inc = inc, g[1].age = 0;
}
#endif
