/* SPDX-License-Identifier: MPL-2.0 */
/* A test audio FX with a multi-level ui_hierarchy (module.json: links, a null gap, more than 8 knobs on a level, enums,
 * units, a param whose metadata is chain_params'): it passes its input through and keeps every value it is set, by
 * key. set_param logs "pagesfx: set KEY=VALUE" (stderr: the host's log); get_param(KEY) gives it back; its "state" is
 * every value as "key=value;..", and a state set is taken back the same way. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#define NK 40
typedef struct { char k[NK][24], v[NK][32]; int n; } pg_t;
static void *create(const char *dir, const char *cfg) { (void)dir, (void)cfg; return calloc(1, sizeof(pg_t)); }
static void destroy(void *i) { free(i); }
static void process(void *i, int16_t *a, int n) { (void)i, (void)a, (void)n; }
static void put(pg_t *s, const char *k, const char *v)
{
    int i;
    for (i = 0; i < s->n && strcmp(s->k[i], k); i++)
        ;
    if (i == NK)
        return;
    if (i == s->n)
        snprintf(s->k[s->n++], sizeof s->k[0], "%s", k);
    snprintf(s->v[i], sizeof s->v[0], "%s", v);
}
static void setp(void *i, const char *k, const char *v)
{
    pg_t *s = i;
    if (!strcmp(k, "state")) {   /* key=value;.. */
        char b[2048], *p, *save = NULL;
        snprintf(b, sizeof b, "%s", v);
        for (p = strtok_r(b, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
            char *e = strchr(p, '=');
            if (e)
                *e = 0, put(s, p, e + 1);
        }
        return;
    }
    fprintf(stderr, "pagesfx: set %s=%s\n", k, v);
    put(s, k, v);
}
static int getp(void *i, const char *k, char *b, int n)
{
    pg_t *s = i;
    int j, w = 0;
    if (!strcmp(k, "state")) {
        b[0] = 0;
        for (j = 0; j < s->n && w < n; j++)
            w += snprintf(b + w, (size_t)(n - w), "%s=%s;", s->k[j], s->v[j]);
        return w < n ? w : -1;
    }
    for (j = 0; j < s->n; j++)
        if (!strcmp(s->k[j], k))
            return snprintf(b, (size_t)n, "%s", s->v[j]);
    return -1;
}
static audio_fx_api_v2_t API = { AUDIO_FX_API_VERSION_2, create, destroy, process, setp, getp, NULL };
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *h) { (void)h; return &API; }
