/* SPDX-License-Identifier: MPL-2.0 */
/* cpu2fix: a test firmware for core 1 (README "Threads", cpu2), an .rfm1 module written against RFM1-FORMAT.md's
 * exports directly (its own panel and descriptor at the offsets src/fwrt.c checks; no fm1core.h, no shim).
 *
 * The sound: 16 voices (one per held key, the rest a fixed drone), each a cheap FM pair computed with a scratch array
 * on the C stack (in linear memory: the shadow stack), so two parts sharing one stack pointer would corrupt each other.
 * Every 32-frame control block it splits like FIZZ and ChoralRoot do on the FM-1:
 *   core 0 updates the shared state (phases' steps from the keys), hands "voices 8..15 into bus B" to core 1
 *   (rfm1_cpu2_1.run), renders voices 0..7 into bus A, waits (rfm1_cpu2_1.wait), then sums A and B in a fixed order.
 * If run answers 0 or wait says not done, core 0 renders B itself: the same bytes either way. WORK sets how heavy a
 * voice is (the tests time it). tests/cpu2fix/build.sh makes cpu2fix-1.0.rfm1 (emcc). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EXPORT(name) __attribute__((export_name(#name), used))
#define CPU2_IMPORT(name) __attribute__((import_module("rfm1_cpu2_1"), import_name(#name)))

CPU2_IMPORT(run) uint32_t cpu2_run(uint32_t fn, uint32_t arg);
CPU2_IMPORT(wait) uint32_t cpu2_wait(void);
/* emscripten's stack (system/lib/compiler-rt/stack_ops.S, stack_limits.S) */
void _emscripten_stack_restore(uintptr_t sp);
void emscripten_stack_set_limits(void *base, void *end);

#ifndef WORK
#define WORK 24
#endif
#define NV 16
#define CB 32                       /* the control block */
#define BLOCK 128

/* ---- the panel and the descriptor, at the offsets of RFM1-FORMAT.md section 5 (fwrt.c WANT) */
#define PANEL_SIZE 115384
static uint8_t panel[PANEL_SIZE] __attribute__((aligned(16)));
#define P_KEYS 0
#define P_LCD 148
#define P_LCD_WRITES 115348
static uint8_t desc[192] __attribute__((aligned(16)));
static const int32_t LAYOUT[34] = {
    115384, 0, 4, 8, 12, 16, 44, 48, 75, 102, 146, 148, 115348, 115352, 115366, 115373, 115380,
    192, 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 92, 96, 100, 104, 108, -1,
};
static uint8_t flash_img[4096];

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

EXPORT(rfm1_version) int32_t rfm1_version(void) { return 1; }
EXPORT(rfm1_get) uint32_t rfm1_get(uint32_t abi)
{
    if (abi != 6)
        return 0;
    put32(desc + 0, 6);
    put32(desc + 4, (uint32_t)(uintptr_t)"cpu2fix");
    put32(desc + 8, (uint32_t)(uintptr_t)"CPU2 fixture");
    put32(desc + 12, (uint32_t)(uintptr_t)"1.0");
    put32(desc + 16, (uint32_t)(uintptr_t)"https://github.com/Quixotic7/remoteFM14Move");
    put32(desc + 24, (uint32_t)(uintptr_t)"MPL-2.0");
    put32(desc + 32, sizeof flash_img);
    put32(desc + 100, (uint32_t)(uintptr_t)panel);
    return (uint32_t)(uintptr_t)desc;
}
EXPORT(rfm1_layout) int32_t rfm1_layout(int32_t i) { return i >= 0 && i < 34 ? LAYOUT[i] : -1; }

/* a bump allocator (the host's scratch, its input, core 1's stack): never freed */
static uint8_t arena[1u << 20] __attribute__((aligned(16)));
static uint32_t arena_used;
EXPORT(rfm1_alloc) uint32_t rfm1_alloc(uint32_t n)
{
    uint32_t p;
    n = (n + 15u) & ~15u;
    if (arena_used + n > sizeof arena)
        return 0;
    p = (uint32_t)(uintptr_t)(arena + arena_used);
    arena_used += n;
    return p;
}
EXPORT(rfm1_free) void rfm1_free(uint32_t p) { (void)p; }
EXPORT(rfm1_options) void rfm1_options(uint32_t a, int32_t b, int32_t c, int32_t d) { (void)a, (void)b, (void)c, (void)d; }
EXPORT(rfm1_boot_options) int32_t rfm1_boot_options(int32_t a, uint32_t b, int32_t c) { (void)a, (void)b, (void)c; return 1; }

/* ---- the synth */
static uint32_t phase[NV], step[NV], mod[NV];
static int32_t bus_a[CB * 2], bus_b[CB * 2];
static uint32_t frames_done;
static int32_t jobs_core1, jobs_self;

static int32_t sine(uint32_t ph)   /* a parabola, integer: -32767..32767 */
{
    const int32_t x = (int32_t)(ph >> 16) - 32768;   /* -32768..32767 */
    const int32_t y = (int32_t)(((int64_t)x * (65536 - (x < 0 ? -x : x) * 2)) >> 15);
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

/* voices v0..v1-1 into bus (CB stereo frames); the scratch on the stack, address taken: the shadow stack */
static void voices(int v0, int v1, int32_t *bus)
{
    volatile int32_t tmp[CB];
    int v, i, w;
    memset(bus, 0, sizeof(int32_t) * CB * 2);
    for (v = v0; v < v1; v++) {
        uint32_t ph = phase[v];
        for (i = 0; i < CB; i++) {
            int32_t s = 0;
            for (w = 0; w < WORK; w++)   /* (the cost: WORK modulator steps a sample) */
                s += sine(ph + (uint32_t)w * mod[v] + (uint32_t)s * 4096u) >> 4;
            tmp[i] = s;
            ph += step[v];
        }
        phase[v] = ph;
        for (i = 0; i < CB; i++) {
            bus[2 * i] += tmp[i] * (v + 1) / NV / 16;
            bus[2 * i + 1] += tmp[i] * (NV - v) / NV / 16;
        }
    }
}

static void job_upper(uint32_t arg)   /* core 1's job: voices 8..15 into bus B */
{
    (void)arg;
    voices(NV / 2, NV, bus_b);
}

/* core 1's entry points on the twin (README "Threads"): its stack, and a job through the table */
EXPORT(rfm1_c1_stack) void rfm1_c1_stack(uint32_t top)
{
    emscripten_stack_set_limits((void *)(uintptr_t)top, (void *)(uintptr_t)(top - 64u * 1024u));
    _emscripten_stack_restore(top);
}
EXPORT(rfm1_c1_call) void rfm1_c1_call(uint32_t fn, uint32_t arg) { ((void (*)(uint32_t))(uintptr_t)fn)(arg); }

static void control_block(void)
{
    const uint32_t keys = get32(panel + P_KEYS);
    int v, posted;
    for (v = 0; v < NV; v++) {   /* the shared state, core 0 only, before the hand-over */
        const int held = v < 27 && (keys >> v & 1);
        step[v] = (held ? 0x01000000u : 0x00400000u) + (uint32_t)v * 0x00090000u + (frames_done >> 12);
        mod[v] = 0x00100000u * (uint32_t)(v + 1);
    }
    posted = (int)cpu2_run((uint32_t)(uintptr_t)job_upper, 0);
    voices(0, NV / 2, bus_a);
    if (!posted || cpu2_wait() != 0) {
        job_upper(0);
        jobs_self++;
    } else
        jobs_core1++;
}

EXPORT(rfm1_init) void rfm1_init(int32_t demo)
{
    (void)demo;
    memset(phase, 0, sizeof phase);
    put32(panel + P_LCD_WRITES, 1);
}
EXPORT(rfm1_tick) void rfm1_tick(uint32_t ms) { (void)ms; }
EXPORT(rfm1_frame) void rfm1_frame(void)
{
    /* the screen: a bar per voice */
    uint16_t *lcd = (uint16_t *)(void *)(panel + P_LCD);
    int v;
    for (v = 0; v < NV; v++)
        lcd[v * 15] = (uint16_t)(phase[v] >> 16);
    put32(panel + P_LCD_WRITES, get32(panel + P_LCD_WRITES) + 1);
}
EXPORT(rfm1_idle) void rfm1_idle(void) {}
EXPORT(rfm1_audio) void rfm1_audio(uint32_t out, uint32_t frames)
{
    int16_t *o = (int16_t *)(uintptr_t)out;
    uint32_t f, i;
    for (f = 0; f + CB <= frames; f += CB) {
        control_block();
        for (i = 0; i < CB * 2; i++) {   /* A then B: one core's order */
            int32_t x = bus_a[i] + bus_b[i];
            o[f * 2 + i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        frames_done += CB;
    }
}
EXPORT(rfm1_midi_in) int32_t rfm1_midi_in(uint32_t p) { (void)p; return 1; }
EXPORT(rfm1_midi_out_take) int32_t rfm1_midi_out_take(uint32_t p) { (void)p; return 0; }
EXPORT(rfm1_halted) int32_t rfm1_halted(void) { return 0; }
EXPORT(rfm1_shutdown) void rfm1_shutdown(void) {}
EXPORT(rfm1_flash_stage) int32_t rfm1_flash_stage(uint32_t b, uint32_t n)
{
    if (b && n == sizeof flash_img)
        memcpy(flash_img, (const void *)(uintptr_t)b, n);
    return 1;
}
EXPORT(rfm1_flash) uint32_t rfm1_flash(void) { return (uint32_t)(uintptr_t)flash_img; }
EXPORT(rfm1_flash_dirty) int32_t rfm1_flash_dirty(void) { return 0; }
EXPORT(rfm1_flash_sync) int32_t rfm1_flash_sync(void) { return 1; }
EXPORT(rfm1_stats) void rfm1_stats(uint32_t a, uint32_t b)
{
    if (a)
        put32((uint8_t *)(uintptr_t)a, 0);
    if (b)
        put32((uint8_t *)(uintptr_t)b, 0);
}
EXPORT(rfm1_ui_info) void rfm1_ui_info(uint32_t buf, uint32_t n)
{
    /* "core1 <jobs> self <jobs>" (fwrun --ui) */
    char *b = (char *)(uintptr_t)buf, t[48];
    int32_t vals[2] = { jobs_core1, jobs_self }, k, i = 0, j;
    const char *names[2] = { "core1 ", " self " };
    if (!b || !n)
        return;
    for (k = 0; k < 2; k++) {
        const char *s = names[k];
        int32_t v = vals[k], d = 1;
        while (*s && i < 40)
            t[i++] = *s++;
        while (v / d >= 10)
            d *= 10;
        for (j = 0; d && i < 46; d /= 10, j++)
            t[i++] = (char)('0' + v / d % 10);
    }
    t[i] = 0;
    for (k = 0; k < i && (uint32_t)k + 1 < n; k++)
        b[k] = t[k];
    b[k] = 0;
}
