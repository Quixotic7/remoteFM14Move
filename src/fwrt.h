/* SPDX-License-Identifier: MPL-2.0 */
/* fwrt: a compiled firmware (src/rfm1m.h) run as RFM1-FORMAT.md section 7 says, on the native ABI's device clock
 * (fm1core ABI 6: tick(ms) once per device ms, frame() every 15 ms and at ms 0 else idle(), audio() for each 128-frame
 * block due at 44100 Hz). The panel is the module's own, in its linear memory, at the offsets rfm1_layout gives.
 *
 * THREADS (README "Threads"): every call on one instance from one thread at a time, in order (the engine's thread, or
 * the slot's render thread while the engine waits for it); different instances on different threads at once. The
 * runtime's trap state is per thread (tools/w2c_mt.sh); an object built before that (fw_obj_t.mt 0: its call depth is
 * a process global) gets a counter of its own (fw_obj_t.lg) and its instances take turns (fwrt.c). fw_obj_open (dlopen) and fw_instantiate may run
 * elsewhere. A module with cpu2 (fw_obj_t.cpu2) gets a core-1 thread of its own: its jobs run there on a twin
 * instance that shares the module's memory, between the module's rfm1_cpu2_1.run and .wait. */
#ifndef RFM14_FWRT_H
#define RFM14_FWRT_H
#include <stddef.h>
#include <stdint.h>

#include "rfm1m.h"

#define FW_FS 44100
#define FW_BLOCK 128
#define FW_LCD_W 240
#define FW_LCD_H 240
#define FW_NKEY 27
#define FW_NBTN 14
#define FW_NENC 7
#define FW_NLEVELS 44                 /* led[11][4] */
#define FW_FRAME_MS 15
#define FW_LBL_N 49                   /* live labels (DYNAMIC-LABELS) */

/* a loaded object: one per file, shared by its instances */
typedef struct fw_obj {
    void *so;
    const rfm1m_t *vt;
    char path[512];
    int refs;
    /* from its manifest (the .json beside it): a Fantasy CFW's file API (VIRTUAL-CARTS.md: "virtual", "needs":
     * ["fs1"], "folders") and its memory (build_fw.sh's "memory_pages", else the manifest's "memory_mib"; 0 unknown) */
    int virt, fs1;
    uint8_t access[3];                /* fsvc.h: audio, code, data: FSV_R | FSV_W */
    uint64_t mem_bytes;
    /* ain1 (VIRTUAL-CARTS.md section 8): the manifest lists "ain1" in "wants" or "needs" (ain1), and the object
     * exports rfm1_audio_in (ain: build_fw.sh's rfm1m_ain table); the host gives input only when both */
    int ain1;
    const rfm1m_ain_t *ain;
    const rfm1m_rd1_t *rd1;            /* rd1: the module exports rfm1_readout (readouts), else NULL */
    const rfm1m_tk1_t *tk1;            /* tk1: the module exports rfm1_tk, _tk_input, _tk_state (the takeover), else NULL */
    /* threads: built with a thread-local call depth (rfm1m_caps RFM1M_CAP_MT); core 1 (rfm1m_cpu2: NULL none) */
    int mt;
    const rfm1m_cpu2_t *cpu2;
    struct fw_lg *lg;                 /* an old object (mt 0): its own call depth and lock (fwrt.c), NULL: the global lock */
} fw_obj_t;

/* the imports' instance: which firmware calls (the type wasm2c's imports name); NULL until fw_create. A Fantasy
 * CFW's rfm1_host_1 imports get the same object (fwglue.c passes it for both modules), with its file service */
struct Z_wasi_snapshot_preview1_instance_t {
    struct fw *volatile fw;
    struct fsvc *fs;                  /* (NULL: the module has no file API, or no folder) */
};
struct Z_rfm1_host_1_instance_t {
    struct Z_wasi_snapshot_preview1_instance_t w;
};
/* cpu2's imports get the same object too (a twin's: fw NULL, so its run answers 0 and its output goes nowhere) */
struct Z_rfm1_cpu2_1_instance_t {
    struct Z_wasi_snapshot_preview1_instance_t w;
};

/* an instance made but not started: its memory allocated and its data loaded (wasm2c's instantiate: no firmware code
 * runs). Made on any thread (the storage thread: with every page of a locked process faulted in, a firmware's memory
 * takes milliseconds), started on the engine's (fw_create_from); a stopped one is freed on any thread (fw_inst_free) */
typedef struct fw_inst {
    struct fw_obj *obj;               /* (a reference) */
    void *inst;
    struct Z_wasi_snapshot_preview1_instance_t *wasi;
    void *twin;                       /* cpu2: the core-1 twin (rfm1m_cpu2_t.twin), NULL none */
    struct Z_wasi_snapshot_preview1_instance_t *twin_wasi;
} fw_inst_t;

/* the panel's inputs (fw_in), as the log records them */
enum {
    FWI_KEY = 1,      /* a key 0..26, b velocity 1..127 (down) or 0 (up) */
    FWI_LEAN = 2,     /* a key, b aftertouch 0..100 % */
    FWI_BTN = 3,      /* a matrix button 0..13, b down 1 / up 0 */
    FWI_ENC = 4,      /* a matrix encoder 0..6, b detents (+ clockwise) */
    FWI_MASTER = 5,   /* b 0..1023 */
    FWI_MIDI = 6,     /* c a USB-MIDI packet (rfm1_midi_in) */
    FWI_SYNC = 7,     /* flash_sync (it may change the firmware's state: logged like an input) */
    FWI_RELEASE = 8,  /* every key and button up */
    FWI_TAKE = 9,     /* a: how many packets midi_out_take gave (the replay takes as many) */
    FWI_TK_INPUT = 11,  /* c a Move surface packet (rfm1_tk_input: tk1, the takeover) */
    FWI_TK_STATE = 12,  /* a on 1 / off 0, b the claims granted, c the flags (rfm1_tk_state) */
    FWI_FRAMES = 10,  /* a: frame() every a device ms from now (1..255; FW_FRAME_MS the ABI's), idle() between: the
                       * host's screen rate (README "Frames"); logged, so a replay calls frame() as the host did */
};
typedef struct {
    uint32_t ms;      /* the device ms it was applied before (tick(ms)) */
    uint8_t op, a;
    int16_t b;
    uint32_t c;
} fw_log_t;

typedef struct fw {
    fw_obj_t *obj;
    void *inst;
    struct Z_wasi_snapshot_preview1_instance_t *wasi;
    uint8_t *mem;
    uint64_t mem_size;
    uint32_t L[34];
    uint32_t desc, panel, scratch;
    char id[32], name[48], version[32];
    char btn_name[FW_NBTN][16];       /* the firmware's own labels, FM1P_B_* order */
    int8_t keymap[6][11];
    int has_keymap;
    uint32_t flash_size;
    /* the clock */
    uint32_t ms, last_frame;          /* the next device ms to run; the last frame()'s */
    uint64_t frames_done, due;        /* blocks rendered (frames); frames due by the last ms run */
    int booted, dead;                 /* dead: trapped, halted or refused: silence from here */
    uint32_t halted;                  /* rfm1_halted's last non-zero value */
    int exit_code;                    /* proc_exit's, -1 none */
    char err[160];
    /* queued input (applied before the next tick whose frame is due) */
    struct { uint64_t due; uint8_t op, a; int16_t b; uint32_t c; } q[256];
    int qn;
    uint32_t keys_held, btns_held;    /* the host's view (for FWI_RELEASE) */
    /* MIDI messages fed whole (fw_midi_feed: SysEx from the page's editors): USB-MIDI packets given to rfm1_midi_in
     * before each tick, as many as it takes (0: no room, the rest waits), as rfm1_run --sysex does */
    uint32_t *mq;
    uint32_t mq_head, mq_tail, mq_fed;
    /* the log of applied input (optional: fw_log_start) */
    fw_log_t *log;
    uint32_t log_n, log_cap;
    /* what the host reads */
    uint32_t lbl_epoch;
    uint32_t frame_ms;                 /* frame() every this many device ms (0: FW_FRAME_MS); FWI_FRAMES sets it */
    uint32_t rd_ptr, rd_seen;          /* rd1: the readout record (0: none / not asked yet), the seq last taken */
    int rd_asked;
    uint32_t tk_ptr;                   /* tk1: the takeover record (fw_boot: checked once after rfm1_init), 0 none */
    /* ain1: the input buffer in the module's memory (rfm1_alloc'd at the first fw_ain_info(1, ..)), the state and
     * latency the module was last told (rfm1_audio_in_info), the blocks fed; ain_peak: the last block's peak */
    uint32_t ain_buf, ain_state, ain_lat, ain_blocks;
    int ain_said;                     /* (rfm1_audio_in_info was called at least once) */
    int ain_peak;
    /* the cost: nanoseconds spent in the module */
    uint64_t ns_total;
    uint64_t ns_audio, ns_tick, ns_frame;   /* of ns_total: rfm1_audio's, tick + idle's, frame's (n_frame calls) */
    uint32_t n_frame;
    uint32_t worst_us;
    /* cpu2: the core-1 thread (NULL: none, or off) */
    struct fw_c1 *c1;
    /* its stdout / stderr: whole lines, at most FW_LOG_RATE a second (a burst of FW_LOG_BURST), the rest counted */
    char line[160];
    int line_n;
    double log_tokens;
    uint64_t log_ns;
    uint32_t log_dropped, log_said;
} fw_t;
#define FW_LOG_RATE 4
#define FW_LOG_BURST 20

/* ---- threads */
/* cpu2 on (default) or off (RFM14_CPU2=0 in the environment says off too): set before the first instance */
void fw_cpu2_enable(int on);
/* called on each core-1 thread as it starts (the engine: its name, cores and priority), NULL: nothing */
extern void (*fw_on_c1_thread)(fw_t *fw);
typedef struct {
    int state;                        /* 0 none, 1 working, 2 off (a job trapped or its stack failed) */
    uint64_t jobs, inline_jobs;       /* jobs core 1 ran; jobs the module ran itself (run answered 0) */
    uint64_t job_ns, wait_ns;         /* core 1's time in jobs; core 0's time waiting in .wait */
    uint32_t job_worst_us;
    int tid;                          /* the core-1 thread's (Linux) */
} fw_c1_stats_t;
void fw_c1_stats(const fw_t *fw, fw_c1_stats_t *st);   /* (zeros: no core 1) */

/* ---- the object (any thread; its references are atomic) */
fw_obj_t *fw_obj_open(const char *path, char *err, size_t cap);
void fw_obj_ref(fw_obj_t *o);
void fw_obj_close(fw_obj_t *o);

/* ---- an instance made, or freed, off the engine's thread (see fw_inst_t) */
fw_inst_t *fw_instantiate(fw_obj_t *obj);   /* NULL: no memory (fw_inst_why: why, on this thread) */
const char *fw_inst_why(void);
/* the memory an instance may still take (MB): MemAvailable less a reserve for the system; -1 unknown.
 * RFM14_MEM_AVAIL_MB stands in for MemAvailable in the test build (the simulator's host) */
long fw_mem_free_mb(void);
#define FW_MEM_RESERVE_MB 160
void fw_inst_free(fw_inst_t *pre);           /* (NULL: nothing) */

/* ---- an instance (the engine's thread) */
/* instantiated, _initialize'd, descriptor and layout checked: 0, or -1 with fw->err */
int fw_create(fw_t *fw, fw_obj_t *obj);
/* the same from an instance fw_instantiate made (fw takes it, whatever the result) */
int fw_create_from(fw_t *fw, fw_inst_t *pre);
/* the power-on: from a saved flash image (NULL / 0: a fresh one). 0 ok, -1 trapped (fw->err) */
int fw_boot(fw_t *fw, const uint8_t *flash, uint32_t n);
void fw_destroy(fw_t *fw);
/* fw_destroy, but the instance's memory is handed back to be freed elsewhere (fw_inst_free; NULL: nothing to free) */
fw_inst_t *fw_release(fw_t *fw);
/* a "N lines dropped" line for what the rate limit held back, if any (the engine calls it now and then) */
void fw_log_flush(fw_t *fw);
/* the clock until the next block is due, then that block (128 stereo int16 frames). 0 ok; -1 silence (dead) */
int fw_render(fw_t *fw, int16_t out[FW_BLOCK * 2]);
/* ain1 (VIRTUAL-CARTS.md section 8): the module takes input (its manifest lists ain1 and it exports rfm1_audio_in) */
int fw_has_ain(const fw_t *fw);
/* the input's state (1 live, 0 off) and round-trip latency (frames), told to the module now (rfm1_audio_in_info) when
 * either changed: before the first input, after boot. 0, or -1 (no ain1, not booted, dead) */
int fw_ain_info(fw_t *fw, uint32_t state, uint32_t latency_frames);
/* fw_render with the block's input (128 stereo int16 frames) given through rfm1_audio_in right before rfm1_audio, as
 * rfm1_run --audio-in does; in NULL (or the state 0): no rfm1_audio_in for this block */
int fw_render_in(fw_t *fw, int16_t out[FW_BLOCK * 2], const int16_t *in);
/* an input, applied before the first device ms at or after frame due_frame of the firmware's own count (0: the next) */
void fw_in(fw_t *fw, int op, int a, int b, uint32_t c, uint64_t due_frame);
/* a whole MIDI message (n bytes) as USB-MIDI packets; returns 0, or -1 if not a message this sends */
int fw_midi(fw_t *fw, const uint8_t *msg, int n, uint64_t due_frame);
/* a whole MIDI message of any kind (SysEx F0..F7 too) as USB-MIDI packets, queued for rfm1_midi_in (FW_MQ packets at
 * most waiting): 0 queued, -1 no room or not a message */
#define FW_MQ 16384
int fw_midi_feed(fw_t *fw, const uint8_t *msg, int n);
/* USB-MIDI packets -> MIDI bytes (the out's: SysEx in pieces); returns the bytes written (at most 3 a packet) */
int fw_pkt_bytes(uint32_t pkt, uint8_t out[3]);
/* an input applied now, before the next device ms (between renders: the same point a queued one has) */
void fw_in_now(fw_t *fw, int op, int a, int b, uint32_t c);
/* the firmware's MIDI out: up to max USB-MIDI packets taken into pkts (logged as one FWI_TAKE); how many */
int fw_midi_out(fw_t *fw, uint32_t *pkts, int max);
void fw_log_start(fw_t *fw, uint32_t cap);
/* the panel as the firmware labels it (filled at init): label -> matrix button, role -> matrix encoder, its sense */
void fw_panel_ids(const fw_t *fw, uint8_t btn_id[FW_NBTN], uint8_t enc_id[FW_NENC], int8_t enc_dir[FW_NENC]);

/* ---- what the panel shows (pointers into the module's memory: read on the engine's thread) */
const uint8_t *fw_leds(const fw_t *fw);          /* FW_NLEVELS levels, led[col][line] */
uint8_t fw_play_green(const fw_t *fw);
const uint8_t *fw_lcd(const fw_t *fw);           /* 240 x 240 RGB565, big-endian */
uint32_t fw_lcd_writes(const fw_t *fw);
int32_t fw_master(const fw_t *fw);
/* the flash image now (flash_size bytes), NULL if none; fw_flash_dirty a counter that moves on every change */
const uint8_t *fw_flash(fw_t *fw);
uint32_t fw_flash_dirty(fw_t *fw);
/* live labels: the epoch (0 none) and one slot's text (NULL: not given) */
uint32_t fw_labels_epoch(fw_t *fw);
/* rd1 (FM1VST docs/RFM1.md "Readouts (rd1)"): the module's newest readout, if its seq moved since the last call
 * (1, out filled), else 0 (none new, no rd1, not booted). take 0: only remember the seq (a slot made active: its
 * older readouts are not shown) */
typedef struct {
    uint32_t seq;
    int control, kind, flags, frac, ms;
    char title[17], value[17];
} fw_readout_t;
int fw_readout(fw_t *fw, fw_readout_t *out, int take);
const char *fw_label(fw_t *fw, int which);
/* tk1, the Move takeover (FM1VST docs/RFM1-TAKEOVER.md): its record, rfm1_tk() asked once after rfm1_init (fw_boot)
 * and kept when it is version 1, size >= FW_TK_SIZE, in the module's memory, with a printable name. fw_tk: the record
 * now (a pointer into the module's memory: read it on the instance's thread, between its calls), NULL: no takeover
 * (no tk1, its record refused, not booted, dead). fw_tk_input / fw_tk_state call the exports now, under the trap guard
 * (in time order with the ticks: fw_in FWI_TK_INPUT / FWI_TK_STATE, which call these) */
#define FW_TK_SIZE 1468
#define FW_TK_CLAIMS 4                /* the record's fields (u32 claims, scr_seq, led_seq, say_seq; u8 say_flags) */
#define FW_TK_SCR_SEQ 8
#define FW_TK_LED_SEQ 12
#define FW_TK_SAY_SEQ 16
#define FW_TK_SAY_FLAGS 20
#define FW_TK_NAME 24                 /* char[16] */
#define FW_TK_SAY 40                  /* char[128] */
#define FW_TK_SCREEN 168              /* u8[1024]: 128 x 64, 8 pages of 128 columns, bit 0 the top */
#define FW_TK_LED 1192                /* u8[92][3] */
const uint8_t *fw_tk(const fw_t *fw);
void fw_tk_input(fw_t *fw, uint32_t pkt);
void fw_tk_state(fw_t *fw, uint32_t on, uint32_t claims, uint32_t flags);
/* ui_info into buf */
void fw_ui_info(fw_t *fw, char *buf, uint32_t n);

#endif
