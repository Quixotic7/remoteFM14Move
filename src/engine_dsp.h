/* SPDX-License-Identifier: MPL-2.0 */
/* The output stage: the slots' blocks summed with their levels, then a soft limiter (the Move's speaker is loud).
 * The limiter: instant attack, ~150 ms release, holding the peak at LIM_THRESH (-1 dBFS). Below it the gain is exactly
 * 1, so one slot at level 127 passes bit for bit. Header-only: the engine and the unit test share it. */
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

/* level 0..127 (127: unity) */
static inline void mix_add(mixer_t *m, const int16_t *in, int frames, int level)
{
    int i;
    if (level >= 127) {
        for (i = 0; i < 2 * frames; i++)
            m->acc[i] += (float)in[i];
    } else if (level > 0) {
        const float g = (float)level / 127.0f;
        for (i = 0; i < 2 * frames; i++)
            m->acc[i] += (float)in[i] * g;
    }
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
