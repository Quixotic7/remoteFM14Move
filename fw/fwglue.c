/* SPDX-License-Identifier: MPL-2.0 */
/* The glue of a compiled firmware object (src/rfm1m.h): built with the wasm2c output of one .rfm1,
 *   cc -DMOD=<module name> -DRFM1M_ID='"<id>"' -DRFM1M_SHA='"<sha256>"' [-DRFM1M_INST_ARGS=',HOSTI,WASII']
 *      -I<w2c dir> fwglue.c <mod>.c -shared
 * Every call is a thin wrapper over the module's export of the same name; the instance is an opaque pointer.
 * RFM1M_INST_ARGS: the import modules' instances wasm2c's instantiate takes after the module's own, in its order
 * (tools/build_fw.sh reads them from the header): WASII the WASI imports', HOSTI a Fantasy CFW's rfm1_host_1 (the
 * same host object: fwrt.h); none for a module that imports nothing. Default: the WASI imports' only. */
#include <stdlib.h>

#include "wasm-rt.h"
#include "../src/rfm1m.h"

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define STR_(a) #a
#define STR(a) STR_(a)
#include STR(MOD.h)

#define INST CAT(CAT(Z_, MOD), _instance_t)
#define X(name) CAT(CAT(CAT(Z_, MOD), Z_), name)
#define I ((INST *)i)

#ifndef RFM1M_ID
#define RFM1M_ID ""
#endif
#ifndef RFM1M_SHA
#define RFM1M_SHA ""
#endif

/* optional exports: absent from a module, these stay NULL */
extern void X(_initialize)(INST *) __attribute__((weak));
extern u32 X(rfm1_labels_epoch)(INST *) __attribute__((weak));
extern u32 X(rfm1_label)(INST *, u32) __attribute__((weak));

#ifndef RFM1M_INST_ARGS
#define RFM1M_INST_ARGS , WASII
#endif
struct Z_rfm1_host_1_instance_t;
#define WASII wasi
#define HOSTI ((struct Z_rfm1_host_1_instance_t *)(void *)wasi)

static void *create(struct Z_wasi_snapshot_preview1_instance_t *wasi)
{
    INST *i = calloc(1, sizeof *i);
    (void)wasi;
    if (i)
        CAT(CAT(Z_, MOD), _instantiate)(i RFM1M_INST_ARGS);
    return i;
}
static void destroy(void *i)
{
    if (!i)
        return;
    CAT(CAT(Z_, MOD), _free)(I);
    free(i);
}
static uint8_t *memory(void *i, uint64_t *size)
{
    wasm_rt_memory_t *m = X(memory)(I);
    *size = m->size;
    return m->data;
}
static void initialize(void *i) { X(_initialize)(I); }
static uint32_t version(void *i) { return X(rfm1_version)(I); }
static uint32_t get(void *i, uint32_t a) { return X(rfm1_get)(I, a); }
static uint32_t layout(void *i, uint32_t a) { return X(rfm1_layout)(I, a); }
static uint32_t alloc_(void *i, uint32_t n) { return X(rfm1_alloc)(I, n); }
static void free_(void *i, uint32_t p) { X(rfm1_free)(I, p); }
static void options(void *i, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { X(rfm1_options)(I, a, b, c, d); }
static uint32_t boot_options(void *i, uint32_t a, uint32_t b, uint32_t c) { return X(rfm1_boot_options)(I, a, b, c); }
static void init(void *i, uint32_t demo) { X(rfm1_init)(I, demo); }
static void tick(void *i, uint32_t ms) { X(rfm1_tick)(I, ms); }
static void frame(void *i) { X(rfm1_frame)(I); }
static void idle(void *i) { X(rfm1_idle)(I); }
static void audio(void *i, uint32_t out, uint32_t n) { X(rfm1_audio)(I, out, n); }
static uint32_t midi_in(void *i, uint32_t p) { return X(rfm1_midi_in)(I, p); }
static uint32_t midi_out_take(void *i, uint32_t p) { return X(rfm1_midi_out_take)(I, p); }
static uint32_t halted(void *i) { return X(rfm1_halted)(I); }
static void shutdown_(void *i) { X(rfm1_shutdown)(I); }
static uint32_t flash_stage(void *i, uint32_t b, uint32_t n) { return X(rfm1_flash_stage)(I, b, n); }
static uint32_t flash(void *i) { return X(rfm1_flash)(I); }
static uint32_t flash_dirty(void *i) { return X(rfm1_flash_dirty)(I); }
static uint32_t flash_sync(void *i) { return X(rfm1_flash_sync)(I); }
static void stats(void *i, uint32_t a, uint32_t b) { X(rfm1_stats)(I, a, b); }
static void ui_info(void *i, uint32_t a, uint32_t b) { X(rfm1_ui_info)(I, a, b); }
static uint32_t labels_epoch(void *i) { return X(rfm1_labels_epoch)(I); }
static uint32_t label(void *i, uint32_t w) { return X(rfm1_label)(I, w); }

__attribute__((visibility("default"))) const rfm1m_t *rfm1m_get(void);
const rfm1m_t *rfm1m_get(void)
{
    static rfm1m_t t;
    if (!t.abi) {
        t.id = RFM1M_ID;
        t.sha256 = RFM1M_SHA;
        t.wasm2c = "1.0.32";
        t.init_module = CAT(CAT(Z_, MOD), _init_module);
        t.create = create;
        t.destroy = destroy;
        t.memory = memory;
        t.initialize = X(_initialize) ? initialize : NULL;
        t.version = version;
        t.get = get;
        t.layout = layout;
        t.alloc = alloc_;
        t.free_ = free_;
        t.options = options;
        t.boot_options = boot_options;
        t.init = init;
        t.tick = tick;
        t.frame = frame;
        t.idle = idle;
        t.audio = audio;
        t.midi_in = midi_in;
        t.midi_out_take = midi_out_take;
        t.halted = halted;
        t.shutdown = shutdown_;
        t.flash_stage = flash_stage;
        t.flash = flash;
        t.flash_dirty = flash_dirty;
        t.flash_sync = flash_sync;
        t.stats = stats;
        t.ui_info = ui_info;
        t.labels_epoch = X(rfm1_labels_epoch) ? labels_epoch : NULL;
        t.label = X(rfm1_label) ? label : NULL;
        t.abi = RFM1M_ABI;
    }
    return &t;
}
