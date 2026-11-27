/* SPDX-License-Identifier: MPL-2.0 */
/* The looper (rfm1cs-abletonmove's looper stage 1; docs/design/LOOPER-SPEC.md): 4 lanes x 8 scenes of stereo loops the
 * core's Looper view and the sampler's Loops tab play. A loop is bar-exact audio recorded from a source at the bar
 * (src/barcut.h, the recorder's maths) into a preallocated buffer, never through the recorder's take, so the sampler
 * records meanwhile. Its position is the song's (the beat), so it never drifts; a tempo change repitches it (Warp
 * Repitch), or it plays at its own rate (Warp Off). A lane plays one loop at a time. Its voices go through one mix
 * path: each loop's level, pan and sends, the lane's level, then the Loops strip (level, pan, sends, the volume knob's
 * master), into the mix before the FX returns (looper_render_bus, beside the kit's bus).
 *
 * Threads: every call but new / free / the file work on the engine's thread (one caller). The engine's thread never
 * allocates: recordings go into spare buffers made at looper_new, a recorded loop plays from its spare at once, and
 * the file thread (its own) makes its exact copy, writes the WAV (the project's Loops/ folder), reads loaded loops,
 * frees what the engine let go. libc + pthreads. */
#ifndef RFM14_LOOPER_H
#define RFM14_LOOPER_H
#include <stdint.h>

#define LOOPER_LANES 4
#define LOOPER_SCENES 8
#define LOOPER_SLOTS 32
#define LOOPER_PEAKS 64
#define LOOPER_RPEAKS 32           /* a recording's bar: its peaks (an eighth of a beat each) */
#define LOOPER_PATH 1024
#define LOOPER_MAX_BYTES (256u << 20)   /* the loops in memory (16-bit stereo) */
#define LOOPER_REC_S 64            /* a recording's longest, seconds (its spare buffer) */
#define LOOPER_SPARES 3            /* spare buffers (a recording, the next, one being copied) */
#define LOOPER_OWN "@/"            /* a path in the project's own folder (as rfm1cs's RFM_MOVE_KIT_OWN) */
/* as rfm1cs's RFM_MOVE_LOOP_P_*, _L_*, _SRC_*, _W_*, RFM_MOVE_LQ_* */
/* (round 2) Length is Steps: the markers' region [MSTART, MEND) (of LOOPER_MK_ONE: the whole sound) fills exactly
 * that many steps at the tempo now, by the loop's Warp (Repitch resampled, Grains / Beats time-stretched: the pitch
 * kept; Off: its own rate round the region). Start: unused (0) */
enum { LOOPER_P_START, LOOPER_P_LENGTH, LOOPER_P_PITCH, LOOPER_P_REVERSE, LOOPER_P_WARP, LOOPER_P_LEVEL, LOOPER_P_PAN,
       LOOPER_P_SEND1, LOOPER_P_SEND2, LOOPER_P_SEND3, LOOPER_P_MUTE, LOOPER_P_MSTART, LOOPER_P_MEND, LOOPER_P_N };
#define LOOPER_MK_ONE 16777216
enum { LOOPER_L_SOURCE, LOOPER_L_LENGTH, LOOPER_L_LEVEL, LOOPER_L_N };
enum { LOOPER_SRC_PLAYED, LOOPER_SRC_TRACK1, LOOPER_SRC_TRACK2, LOOPER_SRC_TRACK3, LOOPER_SRC_KIT, LOOPER_SRC_INPUT,
       LOOPER_SRC_MIX, LOOPER_SRC_LOOPS, LOOPER_SRC_N };
enum { LOOPER_W_REPITCH, LOOPER_W_OFF, LOOPER_W_GRAINS, LOOPER_W_BEATS, LOOPER_W_N };   /* (round 2: the kit's
                                                         * engines, stretch.h) */
enum { LOOPER_Q_BAR, LOOPER_Q_INSTANT, LOOPER_Q_BEAT, LOOPER_Q_2BARS, LOOPER_Q_4BARS, LOOPER_Q_CLIP };
enum { LOOPER_EMPTY, LOOPER_LOADING, LOOPER_OK, LOOPER_MISSING, LOOPER_FAILED, LOOPER_TOO_LONG, LOOPER_NOTHING };
enum { LOOPER_RS_NONE, LOOPER_RS_ARMED, LOOPER_RS_RECORDING, LOOPER_RS_ENDING };

typedef struct {
    uint32_t fs;                   /* the engine's frames a second (0: 44100) */
    uint64_t max_bytes;            /* 0: LOOPER_MAX_BYTES */
    uint32_t rec_s;                /* 0: LOOPER_REC_S */
    /* (round 2) a Files row's sound (looper_file), on the looper's file thread: 16-bit stereo at fs, malloc'd (the
     * looper frees it), at most max bytes; its name (no extension); *status LOOPER_OK, _MISSING, _FAILED, _TOO_LONG.
     * NULL: no files (looper_file answers FAILED). The engine's: the kit's Files (kit_row_read) */
    int16_t *(*read)(void *ctx, int place, uint32_t id, uint32_t fs, uint64_t max, uint32_t *frames, char *name, int ncap,
                     int *status);
    void *rctx;
} looper_cfg_t;

/* the block's song position: playing, the beat at its first frame (4 a bar, 0 at Play), the beats a frame */
typedef struct {
    int playing;
    double beat0, bpf;
} looper_clock_t;
/* the sources a recording takes (int16 stereo blocks; NULL: silence): the firmware slots dry, the kit's output, the
 * Move's input, the mix (the slots and the kit at their levels, before the FX returns) */
typedef struct {
    const int16_t *slot[3], *kit, *input, *mix;
} looper_src_t;

typedef struct {                   /* as rfm1cs's rfm_move_loop_status_t */
    struct { int8_t play, next; uint16_t pos, level; } lane[LOOPER_LANES];
    struct { int8_t slot; uint8_t state, bar, bars; } rec[2];
    uint16_t level;
    uint8_t beat, npk;
    uint8_t pk[LOOPER_RPEAKS];
    uint32_t bytes, cap;
} looper_status_t;

enum { LOOPER_A_RECORDED = 1, LOOPER_A_LOADED, LOOPER_A_PEAKS };   /* (round 2: PEAKS: from, count, npk) */
typedef struct {
    int kind, slot, status;
    uint32_t from, count;          /* (PEAKS) the frames the peaks are of */
    char path[LOOPER_PATH];        /* RECORDED: "@/Loops/1-2 120bpm 4b.wav" */
    uint32_t frames, rate;
    int bars, bpm10, npk;
    uint8_t pk[LOOPER_PEAKS];
} looper_ans_t;

typedef struct looper looper_t;
looper_t *looper_new(const looper_cfg_t *cfg);   /* NULL: no memory (its spares, the thread) */
void looper_free(looper_t *l);
void looper_project(looper_t *l, const char *dir);   /* the open project's own folder (Kits/Projects/NN; "" none) */

/* ---- the engine's thread: the core's actions (frame: into the next block, as the sequencer stamps it) */
void looper_rec(looper_t *l, int slot, int bars, int src, int played_slot, uint32_t frame);   /* R_START */
void looper_rec_end(looper_t *l, int slot, uint32_t frame);
void looper_rec_cancel(looper_t *l, int slot);
void looper_launch(looper_t *l, int slot, int quant, uint32_t frame);
void looper_stop(looper_t *l, int lane, int quant, uint32_t frame);
void looper_set(looper_t *l, int slot, int param, int value);
void looper_lane(looper_t *l, int lane, int param, int value);
void looper_load(looper_t *l, int slot, const char *path, int bars, int bpm10);   /* NULL / "": emptied */
void looper_copy(looper_t *l, int dst, int src);
/* (round 2) a Files row's sound into the slot (cfg.read on the file thread): copied into the project's Loops/ as a
 * 16-bit WAV (the original never changed), its bars its length at the tempo bpm10 rounded (1..64); answered as a
 * recording (LOOPER_A_RECORDED); the slot's sound until then plays on */
void looper_file(looper_t *l, int slot, int place, uint32_t id, int bpm10);
/* (round 2) peaks of the slot's frames [from, from + count) in n columns (the markers' close-up): LOOPER_A_PEAKS */
void looper_peaks(looper_t *l, int slot, uint32_t from, uint32_t count, int n);
/* (round 2, item 9) overdub: on 1: the source (LOOPER_SRC_*; played the Played slot) layered onto the slot's loop as
 * it plays (from its next cycle; a cycle's first pass replaces the layer's silence, then sums; a preallocated spare,
 * no allocation); 0: ended: the file thread writes the loop + its layer as a new WAV and answers LOOPER_A_RECORDED
 * (the loop plays on with its layer meanwhile; its sound swapped in seamlessly). One at a time (another commits it) */
void looper_dub(looper_t *l, int slot, int on, int src, int played);
/* (round 2, item 6) this block's monitored input (the gain applied; NULL none): heard through the Loops strip (its
 * level, pan, sends, the volume knob), never in the looper's own block (the Loops source). Before render_bus */
void looper_monitor(looper_t *l, const int16_t *in);
int looper_input_armed(const looper_t *l);       /* (round 2: Monitor Auto) a recording or overdub of the input */
void looper_mix(looper_t *l, int level, int pan, const int sends[3]);   /* the Loops strip (0..127; 100 = 0 dB) */
void looper_master(looper_t *l, int m);          /* the volume knob's MASTER 0..1023 (-1: full) */
/* the voices into acc (stereo, int16 scale), their sends into send[i] (NULL: that FX not running), own the looper's
 * output alone (the Loops source, the recorder's Looper stream; NULL none) */
void looper_render_bus(looper_t *l, const looper_clock_t *c, float *acc, float *const send[3], int16_t *own, uint32_t frames);
/* .. then the recording: its source's block (own: the looper's own output, for the Loops source) */
void looper_record(looper_t *l, const looper_src_t *src, const int16_t *own, uint32_t frames);
void looper_status(const looper_t *l, looper_status_t *st);
int looper_poll(looper_t *l, looper_ans_t *a);   /* 1: an answer for the core */
/* the project saved: the Loops folder's files none of refs (paths as the core keeps them) refers to go (the file
 * thread) */
void looper_clean(looper_t *l, const char *const *refs, int n);
uint64_t looper_bytes(const looper_t *l);
int looper_sounding(const looper_t *l);          /* lanes with a voice (tests, the page) */
void looper_resolve(const looper_t *l, const char *in, char *out, int cap);   /* "@/x" as the project's file */

/* tests: the file thread's work done (1 nothing left), the engine thread's allocations (looper_test's counter) */
int looper_idle(looper_t *l);
#endif
