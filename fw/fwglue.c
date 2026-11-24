/* SPDX-License-Identifier: MPL-2.0 */
/* The glue of a compiled firmware object (src/rfm1m.h): built with the wasm2c output of one .rfm1,
 *   cc -DMOD=<module name> -DRFM1M_ID='"<id>"' -DRFM1M_SHA='"<sha256>"' [-DRFM1M_INST_ARGS=',HOSTI,WASII']
 *      -I<w2c dir> fwglue.c <mod>.c -shared
 * Every call is a thin wrapper over the module's export of the same name; the instance is an opaque pointer.
 * RFM1M_INST_ARGS: the import modules' instances wasm2c's instantiate takes after the module's own, in its order
 * (tools/build_fw.sh reads them from the header): WASII the WASI imports', HOSTI a Fantasy CFW's rfm1_host_1 (the
 * same host object: fwrt.h); none for a module that imports nothing. Default: the WASI imports' only.
 * RFM1M_AIN1: the module exports rfm1_audio_in (ain1): rfm1m_ain() too (src/rfm1m.h). RFM1M_TK1: it exports
 * rfm1_tk, rfm1_tk_input and rfm1_tk_state (the Move takeover): rfm1m_tk1() too. Without it the object is what it
 * always was. RFM1M_MT: built against tools/w2c_mt.sh's header (rfm1m_caps says RFM1M_CAP_MT). RFM1M_CPU2: the
 * module exports rfm1_c1_stack / rfm1_c1_call and imports rfm1_cpu2_1 (CPU2I in RFM1M_INST_ARGS) or, with
 * RFM1M_CPU2_REC, exports rfm1_cpu2 (FM1VST's docs/RFM1.md "cpu2": no import): rfm1m_cpu2() too. */
#include <stdlib.h>
#include <string.h>

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
struct Z_rfm1_cpu2_1_instance_t;
#define WASII wasi
#define HOSTI ((struct Z_rfm1_host_1_instance_t *)(void *)wasi)
#define CPU2I ((struct Z_rfm1_cpu2_1_instance_t *)(void *)wasi)

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

#ifdef RFM1M_AIN1
extern void X(rfm1_audio_in_info)(INST *, u32, u32) __attribute__((weak));
static void ain_in(void *i, uint32_t p, uint32_t n) { X(rfm1_audio_in)(I, p, n); }
static void ain_info(void *i, uint32_t s, uint32_t l) { X(rfm1_audio_in_info)(I, s, l); }
__attribute__((visibility("default"))) const rfm1m_ain_t *rfm1m_ain(void);
const rfm1m_ain_t *rfm1m_ain(void)
{
    static rfm1m_ain_t t;
    if (!t.abi) {
        t.audio_in = ain_in;
        t.audio_in_info = X(rfm1_audio_in_info) ? ain_info : NULL;
        t.abi = RFM1M_AIN_ABI;
    }
    return &t;
}
#endif

#ifdef RFM1M_RD1
/* rd1 (src/rfm1m.h rfm1m_rd1_t): the readout record's offset */
static uint32_t rd1_readout(void *i) { return X(rfm1_readout)(I); }
__attribute__((visibility("default"))) const rfm1m_rd1_t *rfm1m_rd1(void);
const rfm1m_rd1_t *rfm1m_rd1(void)
{
    static rfm1m_rd1_t t;
    if (!t.abi) {
        t.readout = rd1_readout;
        t.abi = RFM1M_RD1_ABI;
    }
    return &t;
}
#endif

#ifdef RFM1M_TK1
/* tk1, the Move takeover (src/rfm1m.h rfm1m_tk1_t): the record's offset, a packet in, the state */
static uint32_t tk1_tk(void *i) { return X(rfm1_tk)(I); }
static uint32_t tk1_input(void *i, uint32_t p) { return X(rfm1_tk_input)(I, p); }
static void tk1_state(void *i, uint32_t on, uint32_t c, uint32_t f) { X(rfm1_tk_state)(I, on, c, f); }
__attribute__((visibility("default"))) const rfm1m_tk1_t *rfm1m_tk1(void);
const rfm1m_tk1_t *rfm1m_tk1(void)
{
    static rfm1m_tk1_t t;
    if (!t.abi) {
        t.tk = tk1_tk;
        t.input = tk1_input;
        t.state = tk1_state;
        t.abi = RFM1M_TK1_ABI;
    }
    return &t;
}
#endif

#ifdef RFM1M_EP1
/* ep1, engine parameters (src/rfm1m.h rfm1m_ep1_t): the record's offset, a SET, the watched page */
static uint32_t ep1_ep(void *i) { return X(rfm1_ep)(I); }
static int32_t ep1_set(void *i, uint32_t op, uint32_t l, uint32_t p, uint32_t v) { return (int32_t)X(rfm1_ep_set)(I, op, l, p, v); }
static void ep1_watch(void *i, uint32_t l, uint32_t p) { X(rfm1_ep_watch)(I, l, p); }
__attribute__((visibility("default"))) const rfm1m_ep1_t *rfm1m_ep1(void);
const rfm1m_ep1_t *rfm1m_ep1(void)
{
    static rfm1m_ep1_t t;
    if (!t.abi) {
        t.ep = ep1_ep;
        t.set = ep1_set;
        t.watch = ep1_watch;
        t.abi = RFM1M_EP1_ABI;
    }
    return &t;
}
#endif

/* threads (src/rfm1m.h RFM1M_CAP_MT): built against tools/w2c_mt.sh's header, the call depth is thread-local */
__attribute__((visibility("default"))) uint32_t rfm1m_caps(void);
uint32_t rfm1m_caps(void)
{
#ifdef RFM1M_MT
    return RFM1M_CAP_MT;
#else
    return 0;
#endif
}

#ifdef RFM1M_CPU2
/* core 1 (src/rfm1m.h rfm1m_cpu2_t): the twin is instantiated as any instance (its own memory made and loaded, then
 * dropped for inst's: no module code runs here), so its globals and table are its own and its memory is inst's */
extern void X(rfm1_c1_stack)(INST *, u32);
extern void X(rfm1_c1_call)(INST *, u32, u32);
static void *c1_twin(void *i, struct Z_wasi_snapshot_preview1_instance_t *wasi)
{
    INST *t = calloc(1, sizeof *t);
    wasm_rt_memory_t *a, *b;
    if (!t)
        return NULL;
    CAT(CAT(Z_, MOD), _instantiate)(t RFM1M_INST_ARGS);
    a = X(memory)(I);
    b = X(memory)(t);
    wasm_rt_free_memory(b);
    *b = *a;
    return t;
}
static void c1_twin_free(void *t)
{
    wasm_rt_memory_t *b;
    if (!t)
        return;
    b = X(memory)((INST *)t);
    b->data = NULL;   /* (inst's: not freed here) */
    b->size = 0;
    CAT(CAT(Z_, MOD), _free)((INST *)t);
    free(t);
}
static void c1_stack(void *t, uint32_t top) { X(rfm1_c1_stack)((INST *)t, top); }
static void c1_call(void *t, uint32_t fn, uint32_t arg) { X(rfm1_c1_call)((INST *)t, fn, arg); }
#ifdef RFM1M_CPU2_REC
/* FM1VST's shim (docs/RFM1.md "cpu2"): rfm1_cpu2() -> {u16 version 1, u16 size, u32 run, u32 wait}, two indices into
 * the module's table whose entries are its one-core answers; inst's two entries become this host's run and wait (the
 * functions an importing module gets: fwrt.c), over wasi as their instance. The twin's table stays the module's: a job
 * that asks for core 1 runs its part itself */
extern u32 X(rfm1_cpu2)(INST *);
extern wasm_rt_funcref_table_t *X(__indirect_function_table)(INST *);
u32 Z_rfm1_cpu2_1Z_run(struct Z_rfm1_cpu2_1_instance_t *, u32, u32);
u32 Z_rfm1_cpu2_1Z_wait(struct Z_rfm1_cpu2_1_instance_t *);
static int c1_hook(void *i, struct Z_wasi_snapshot_preview1_instance_t *wasi)
{
    wasm_rt_memory_t *m = X(memory)(I);
    wasm_rt_funcref_table_t *t = X(__indirect_function_table)(I);
    const u32 r = X(rfm1_cpu2)(I);
    uint16_t ver, size;
    u32 run, wait;
    if (!t || !r || (uint64_t)r + 12u > m->size)
        return -1;
    memcpy(&ver, m->data + r, 2);
    memcpy(&size, m->data + r + 2, 2);
    memcpy(&run, m->data + r + 4, 4);
    memcpy(&wait, m->data + r + 8, 4);
    if (ver != 1 || size < 12 || run >= t->size || wait >= t->size || run == wait || !t->data[run].func ||
        !t->data[wait].func ||
        t->data[run].func_type != wasm_rt_register_func_type(2, 1, WASM_RT_I32, WASM_RT_I32, WASM_RT_I32) ||
        t->data[wait].func_type != wasm_rt_register_func_type(0, 1, WASM_RT_I32))
        return -1;
    t->data[run].func = (wasm_rt_function_ptr_t)Z_rfm1_cpu2_1Z_run;
    t->data[run].module_instance = CPU2I;
    t->data[wait].func = (wasm_rt_function_ptr_t)Z_rfm1_cpu2_1Z_wait;
    t->data[wait].module_instance = CPU2I;
    return 0;
}
#endif
__attribute__((visibility("default"))) const rfm1m_cpu2_t *rfm1m_cpu2(void);
const rfm1m_cpu2_t *rfm1m_cpu2(void)
{
    static rfm1m_cpu2_t t;
    if (!t.abi) {
        t.twin = c1_twin;
        t.twin_free = c1_twin_free;
        t.stack = c1_stack;
        t.call = c1_call;
#ifdef RFM1M_CPU2_REC
        t.hook = c1_hook;
#endif
        t.abi = RFM1M_CPU2_ABI;
    }
    return &t;
}
#endif
