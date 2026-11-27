/* SPDX-License-Identifier: MPL-2.0 */
/* The output stage: the slots' blocks summed with their levels, then a soft limiter (the Move's speaker is loud).
 * The limiter: instant attack, ~150 ms release, holding the peak at LIM_THRESH (-1 dBFS). Below it the gain is exactly
 * 1, so one slot at level 100 passes bit for bit. Header-only: the engine and the unit test share it. */
#ifndef RFM14_ENGINE_DSP_H
#define RFM14_ENGINE_DSP_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define LIM_THRESH 0.8913f   /* -1 dBFS */
#define LIM_RELEASE 0.99985f /* per sample: the envelope falls ~37 % in 150 ms */

typedef struct {
    float acc[2 * 128];
    float env;
    uint32_t limited;        /* samples the limiter turned down (since start) */
} mixer_t;

static inline void mix_clear(mixer_t *m, int frames) { memset(m->acc, 0, sizeof(float) * 2u * (unsigned)frames); }

/* level 0..127 as a gain of level / 100 (the owner's, 2026-11-26: 100, the default, is unity, 0 dB; 127 +2.1 dB) */
#define LVL_UNITY 100
static inline float lvl_gain(int level) { return level <= 0 ? 0.0f : (float)(level > 127 ? 127 : level) / (float)LVL_UNITY; }
static inline void mix_add(mixer_t *m, const int16_t *in, int frames, int level)
{
    int i;
    if (level == LVL_UNITY) {
        for (i = 0; i < 2 * frames; i++)
            m->acc[i] += (float)in[i];
    } else if (level > 0) {
        const float g = lvl_gain(level);
        for (i = 0; i < 2 * frames; i++)
            m->acc[i] += (float)in[i] * g;
    }
}

/* a slot's level and pan (0..127, 64 centre: a balance, so the centre is exactly mix_add's unity) as two gains */
static inline void pan_gains(int level, int pan, float *gl, float *gr)
{
    const float g = lvl_gain(level);
    *gl = *gr = g;
    if (pan < 64)
        *gr = g * (float)(pan < 0 ? 0 : pan) / 64.0f;
    else if (pan > 64)
        *gl = g * (float)(127 - (pan > 127 ? 127 : pan)) / 63.0f;
}

/* level and pan; at pan 64 exactly mix_add (bit for bit) */
static inline void mix_add_lp(mixer_t *m, const int16_t *in, int frames, int level, int pan)
{
    float gl, gr;
    int i;
    if (pan == 64) {
        mix_add(m, in, frames, level);
        return;
    }
    pan_gains(level, pan, &gl, &gr);
    for (i = 0; i < frames; i++) {
        m->acc[2 * i] += (float)in[2 * i] * gl;
        m->acc[2 * i + 1] += (float)in[2 * i + 1] * gr;
    }
}

/* an audio FX's send bus: the sum of each slot's sound after its level and pan, times its send (0..127) */
typedef struct {
    float acc[2 * 128];
} bus_t;
static inline void bus_clear(bus_t *b, int frames) { memset(b->acc, 0, sizeof(float) * 2u * (unsigned)frames); }
static inline void bus_add(bus_t *b, const int16_t *in, int frames, int level, int pan, int send)
{
    float gl, gr;
    const float s = (float)(send > 127 ? 127 : send) / 127.0f;
    int i;
    if (send <= 0)
        return;
    pan_gains(level, pan, &gl, &gr);
    gl *= s, gr *= s;
    for (i = 0; i < frames; i++) {
        b->acc[2 * i] += (float)in[2 * i] * gl;
        b->acc[2 * i + 1] += (float)in[2 * i + 1] * gr;
    }
}
/* the bus as the FX takes it: int16 stereo (clipped) */
static inline void bus_out(const bus_t *b, int16_t *out, int frames)
{
    int i;
    for (i = 0; i < 2 * frames; i++) {
        const long v = lrintf(b->acc[i]);
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
/* an FX's return into the mix, at unity (a return of zeros leaves the mix bit for bit as it was) */
static inline void mix_add_ret(mixer_t *m, const int16_t *in, int frames)
{
    int i;
    for (i = 0; i < 2 * frames; i++)
        if (in[i])
            m->acc[i] += (float)in[i];
}
static inline int peak16(const int16_t *in, int n)
{
    int i, p = 0;
    for (i = 0; i < n; i++) {
        const int v = in[i] < 0 ? -in[i] : in[i];
        if (v > p)
            p = v;
    }
    return p;
}

static inline void mix_out(mixer_t *m, int16_t *out, int frames)
{
    const float T = LIM_THRESH * 32768.0f;
    int i;
    for (i = 0; i < frames; i++) {
        const float l = m->acc[2 * i], r = m->acc[2 * i + 1];
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        float g = 1.0f;
        m->env = m->env * LIM_RELEASE;
        if (a > m->env)
            m->env = a;
        if (m->env > T) {
            g = T / m->env;
            m->limited++;
        }
        {
            const float yl = l * g, yr = r * g;
            const long il = lrintf(yl), ir = lrintf(yr);
            out[2 * i] = (int16_t)(il > 32767 ? 32767 : il < -32768 ? -32768 : il);
            out[2 * i + 1] = (int16_t)(ir > 32767 ? 32767 : ir < -32768 ? -32768 : ir);
        }
    }
}

#endif
