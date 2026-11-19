/* SPDX-License-Identifier: MPL-2.0 */
/* A test audio FX that crashes in create_instance (a segfault): the host must refuse it and keep running. */
#include <stddef.h>
#include <stdint.h>
#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
static void *create(const char *dir, const char *cfg)
{
    volatile int *p = NULL;
    (void)dir, (void)cfg;
    *p = 1;
    return (void *)p;
}
static void destroy(void *i) { (void)i; }
static void process(void *i, int16_t *a, int n) { (void)i, (void)a, (void)n; }
static audio_fx_api_v2_t API = { AUDIO_FX_API_VERSION_2, create, destroy, process, NULL, NULL, NULL };
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *h) { (void)h; return &API; }
