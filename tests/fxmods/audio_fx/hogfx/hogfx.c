/* SPDX-License-Identifier: MPL-2.0 */
/* A test audio FX that spins: "spin_us" microseconds of busy CPU per block (default 1500, over the host's budget). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
typedef struct { int spin_us; } hog_t;
static void *create(const char *dir, const char *cfg) { hog_t *h = calloc(1, sizeof *h); (void)dir, (void)cfg; if (h) h->spin_us = 1500; return h; }
static void destroy(void *i) { free(i); }
static double us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static void process(void *i, int16_t *a, int n)
{
    const double t0 = us();
    (void)a, (void)n;
    while (us() - t0 < ((hog_t *)i)->spin_us)
        ;
}
static void setp(void *i, const char *k, const char *v) { if (k[0] == 's') ((hog_t *)i)->spin_us = atoi(v); }
static int getp(void *i, const char *k, char *b, int n) { return k[0] == 's' ? snprintf(b, (size_t)n, "%d", ((hog_t *)i)->spin_us) : -1; }
static audio_fx_api_v2_t API = { AUDIO_FX_API_VERSION_2, create, destroy, process, setp, getp, NULL };
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *h) { (void)h; return &API; }
