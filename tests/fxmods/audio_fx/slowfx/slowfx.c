/* SPDX-License-Identifier: MPL-2.0 */
/* A test audio FX that passes its input through and takes 80 ms over set_param("state") (a module that rebuilds its
 * tables from a saved state): the host must not do that on its engine thread. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
typedef struct { int amount, restored; } slow_t;
static void *create(const char *dir, const char *cfg) { slow_t *s = calloc(1, sizeof *s); (void)dir, (void)cfg; if (s) s->amount = 64; return s; }
static void destroy(void *i) { free(i); }
static void process(void *i, int16_t *a, int n) { (void)i, (void)a, (void)n; }
static void setp(void *i, const char *k, const char *v)
{
    slow_t *s = i;
    if (!strcmp(k, "state")) {
        struct timespec t = { 0, 80 * 1000000L };
        nanosleep(&t, NULL);
        s->amount = atoi(v), s->restored++;
    } else if (!strcmp(k, "amount"))
        s->amount = atoi(v);
}
static int getp(void *i, const char *k, char *b, int n)
{
    slow_t *s = i;
    if (!strcmp(k, "state") || !strcmp(k, "amount"))
        return snprintf(b, (size_t)n, "%d", s->amount);
    if (!strcmp(k, "restored"))
        return snprintf(b, (size_t)n, "%d", s->restored);
    return -1;
}
static audio_fx_api_v2_t API = { AUDIO_FX_API_VERSION_2, create, destroy, process, setp, getp, NULL };
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *h) { (void)h; return &API; }
