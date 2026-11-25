/* SPDX-License-Identifier: MPL-2.0 */
/* Listen here: the tap on the Move's output and its mute (listen.h). */
#include "listen.h"

#include <stdio.h>
#include <string.h>

#include "../third_party/abletonmoveremote/common/ring.h"

static mr_ring_t g_ring;                 /* stereo int16 frames: audio thread -> web thread */
static int g_ok;
static volatile int g_on;                /* (web -> audio) listeners > 0: tap */
static volatile int g_mute;              /* (web -> audio) silence the Move */
static int g_listeners;                  /* (the web thread's; listen_get reads it atomically) */

/* the audio thread's */
static int32_t g_gain = 32768;           /* Q15: 32768 the Move hears all, 0 nothing */
static uint32_t g_linger;                /* frames the peaks are still measured after the last listener */
static int g_tp_acc, g_op_acc;
static uint32_t g_pk_frames;
static volatile int g_tap_peak, g_out_peak, g_silent;
static volatile uint32_t g_tapped, g_dropped;

#define PEAK_WINDOW 4410u                /* (100 ms) */
#define LINGER (10u * LISTEN_RATE)

int listen_init(void)
{
    if (g_ok)
        return 0;
    if (mr_ring_init(&g_ring, 4, LISTEN_RING) != 0)
        return -1;
    g_ok = 1;
    return 0;
}

static int peak(const int16_t *p, int n)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const int v = p[i] < 0 ? -p[i] : p[i];
        if (v > m)
            m = v;
    }
    return m;
}

void listen_audio(int16_t *out, int frames)
{
    const int on = g_ok && __atomic_load_n(&g_on, __ATOMIC_ACQUIRE);
    const int32_t target = on && __atomic_load_n(&g_mute, __ATOMIC_ACQUIRE) ? 0 : 32768;
    int measure;
    if (frames <= 0)
        return;
    if (on) {
        g_linger = LINGER;
        if (mr_ring_space(&g_ring) >= (uint32_t)frames) {   /* (whole blocks or none) */
            mr_ring_push(&g_ring, out, (uint32_t)frames);
            __atomic_store_n(&g_tapped, g_tapped + 1, __ATOMIC_RELAXED);
        } else
            __atomic_store_n(&g_dropped, g_dropped + 1, __ATOMIC_RELAXED);
    }
    measure = on || g_linger;
    if (measure) {
        const int p = peak(out, frames * 2);
        if (p > g_tp_acc)
            g_tp_acc = p;
    }
    if (g_gain != target) {   /* the ramp: LISTEN_RAMP frames from one end to the other */
        const int32_t step = 32768 / LISTEN_RAMP;
        int i;
        for (i = 0; i < frames; i++) {
            if (g_gain < target)
                g_gain = g_gain + step > target ? target : g_gain + step;
            else if (g_gain > target)
                g_gain = g_gain - step < target ? target : g_gain - step;
            out[2 * i] = (int16_t)((out[2 * i] * g_gain) >> 15);
            out[2 * i + 1] = (int16_t)((out[2 * i + 1] * g_gain) >> 15);
        }
    } else if (!target)
        memset(out, 0, (size_t)frames * 4u);
    __atomic_store_n(&g_silent, g_gain == 0, __ATOMIC_RELAXED);
    if (measure) {
        const int p = peak(out, frames * 2);
        if (p > g_op_acc)
            g_op_acc = p;
        g_pk_frames += (uint32_t)frames;
        if (g_pk_frames >= PEAK_WINDOW) {
            __atomic_store_n(&g_tap_peak, g_tp_acc, __ATOMIC_RELAXED);
            __atomic_store_n(&g_out_peak, g_op_acc, __ATOMIC_RELAXED);
            g_tp_acc = g_op_acc = 0;
            g_pk_frames = 0;
        }
        if (!on)
            g_linger = g_linger > (uint32_t)frames ? g_linger - (uint32_t)frames : 0;
    }
}

void listen_set(int listeners, int mute)
{
    const int was = __atomic_load_n(&g_listeners, __ATOMIC_RELAXED);
    const int was_mute = was > 0 && __atomic_load_n(&g_mute, __ATOMIC_RELAXED);
    mute = listeners > 0 && mute;
    if (listeners == was && mute == was_mute)
        return;
    if (!g_ok)
        return;
    if (listeners > 0 && !was)   /* (the first listener: nothing old in the ring; the audio thread is not pushing) */
        mr_ring_pop(&g_ring, NULL, mr_ring_count(&g_ring));
    __atomic_store_n(&g_mute, mute, __ATOMIC_RELEASE);
    __atomic_store_n(&g_listeners, listeners, __ATOMIC_RELAXED);
    __atomic_store_n(&g_on, listeners > 0, __ATOMIC_RELEASE);
    if (listeners > 0)
        fprintf(stderr, "listen: %d listener%s, the Move %s\n", listeners, listeners == 1 ? "" : "s",
                mute ? "silent" : "playing too");
    else
        fprintf(stderr, "listen: no listener, the Move's sound back\n");
}

int listen_read(int16_t *dst)
{
    if (!g_ok || mr_ring_count(&g_ring) < LISTEN_FRAME)
        return 0;
    mr_ring_pop(&g_ring, dst, LISTEN_FRAME);
    return 1;
}

void listen_get(listen_info_t *i)
{
    i->listeners = __atomic_load_n(&g_listeners, __ATOMIC_RELAXED);
    i->muted = i->listeners > 0 && __atomic_load_n(&g_mute, __ATOMIC_RELAXED);
    i->silent = __atomic_load_n(&g_silent, __ATOMIC_RELAXED);
    i->tap_peak = __atomic_load_n(&g_tap_peak, __ATOMIC_RELAXED);
    i->out_peak = __atomic_load_n(&g_out_peak, __ATOMIC_RELAXED);
    i->tapped = __atomic_load_n(&g_tapped, __ATOMIC_RELAXED);
    i->dropped = __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
}
