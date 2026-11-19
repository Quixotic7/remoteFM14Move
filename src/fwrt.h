/* SPDX-License-Identifier: MPL-2.0 */
/* fwrt: a compiled firmware (src/rfm1m.h) run as RFM1-FORMAT.md section 7 says, on the native ABI's device clock
 * (fm1core ABI 6: tick(ms) once per device ms, frame() every 15 ms and at ms 0 else idle(), audio() for each 128-frame
 * block due at 44100 Hz). The panel is the module's own, in its linear memory, at the offsets rfm1_layout gives.
 *
 * THREADS: everything on one instance from one thread (the engine's); traps are caught with wasm2c's one global
 * jump buffer, so all instances of a process run on that one thread too. fw_obj_open (dlopen) may run elsewhere. */
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

/* an instance made but not started: its memory allocated and its data loaded (wasm2c's instantiate: no firmware code
 * runs). Made on any thread (the storage thread: with every page of a locked process faulted in, a firmware's memory
 * takes milliseconds), started on the engine's (fw_create_from); a stopped one is freed on any thread (fw_inst_free) */
typedef struct fw_inst {
    struct fw_obj *obj;               /* (a reference) */
    void *inst;
    struct Z_wasi_snapshot_preview1_instance_t *wasi;
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
    /* ain1: the input buffer in the module's memory (rfm1_alloc'd at the first fw_ain_info(1, ..)), the state and
     * latency the module was last told (rfm1_audio_in_info), the blocks fed; ain_peak: the last block's peak */
    uint32_t ain_buf, ain_state, ain_lat, ain_blocks;
    int ain_said;                     /* (rfm1_audio_in_info was called at least once) */
    int ain_peak;
    /* the cost: nanoseconds spent in the module */
    uint64_t ns_total;
    uint32_t worst_us;
    /* its stdout / stderr: whole lines, at most FW_LOG_RATE a second (a burst of FW_LOG_BURST), the rest counted */
    char line[160];
    int line_n;
    double log_tokens;
    uint64_t log_ns;
    uint32_t log_dropped, log_said;
} fw_t;
#define FW_LOG_RATE 4
#define FW_LOG_BURST 20

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
const char *fw_label(fw_t *fw, int which);
/* ui_info into buf */
void fw_ui_info(fw_t *fw, char *buf, uint32_t n);

#endif
