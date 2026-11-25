/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder: takes of the engine's streams (the mix, each firmware slot, the input, the looper, the kit) as WAV
 * files. The engine thread calls rec_block once a block and never waits: every present stream goes into a preallocated
 * ring (the capture memory, always listening), and a writer thread drains a take's frames from it to disk. Takes start
 * and end on exact frames (a bar line inside a block, a threshold's block less 50 ms, a press's block). libc + pthreads
 * only (native on macOS, the Move's Linux/aarch64 build).
 *
 * Threads: rec_block on the engine thread (one caller); rec_press, the rec_set_* calls and rec_get from any thread
 * (a press and the settings take effect at the next rec_block: a press is the block's first frame); rec_poll_take from
 * one thread at a time. rec_new / rec_free while the engine is not calling rec_block. */
#ifndef RFM14_RECORDER_H
#define RFM14_RECORDER_H
#include <stdint.h>
#include <time.h>

/* the streams */
enum { REC_ST_MASTER, REC_ST_SLOT1, REC_ST_SLOT2, REC_ST_SLOT3, REC_ST_INPUT, REC_ST_LOOPER, REC_ST_KIT, REC_NST };
#define REC_SRC_STEMS (-1)   /* the source: every present stream, one file each, the same first and last frame, in a
                              * folder of its own (Master.wav, Track 1 <label>.wav .., Input.wav, Looper.wav, Kit.wav) */

enum { REC_MODE_CAPTURE, REC_MODE_BARS, REC_MODE_THRESHOLD, REC_MODE_MANUAL };
enum { REC_IDLE, REC_ARMED, REC_RECORDING, REC_SAVING };   /* (saving: idle, the writer still finishing a take) */

/* why a take ended (rec_take_t.reason); from REC_END_DISK_LOW on: an error, the file holds what came before it */
enum {
    REC_END_OK,           /* a press, its length, a capture */
    REC_END_TRANSPORT,    /* Bars: the transport stopped (not bar-cut) */
    REC_END_MAX_LENGTH,   /* the max take length */
    REC_END_CLOSED,       /* rec_free during a take */
    REC_END_NOTHING,      /* no take: a capture of silence, an empty take (no file) */
    REC_END_DISK_LOW,     /* under min_free on the volume */
    REC_END_WRITE_ERROR,  /* a folder or file could not be made or written */
    REC_END_OVERRUN       /* the writer fell a whole ring behind: the take ends where audio would have been lost */
};

#define REC_HIST 128     /* rec_status_t.hist: the last 128 windows */
#define REC_LABEL 48     /* a label's bytes, with its 0 */

typedef struct {
    const char *dir;        /* the takes' folder (made if missing) */
    int day_folders;        /* 1: in a sub-folder a day, YYYY-MM-DD/ */
    uint32_t fs;            /* frames a second (0: 44100) */
    uint32_t block_frames;  /* the engine's block (0: 128); a longer rec_block is cut into these */
    uint32_t capture_s;     /* the capture memory, seconds (0: 64); the ring is 2 s more */
    uint32_t max_take_s;    /* a take's longest (0: 1800) */
    uint64_t min_free;      /* bytes left free on the volume (0: 200 MB) */
    unsigned streams;       /* the streams to keep rings for, bit per REC_ST_* (0: all 7); others are never present */
    time_t (*now)(void);    /* the wall clock for file names (NULL: time()) */
} rec_cfg_t;

typedef struct {
    const int16_t *st[REC_NST];   /* each stream's block, int16 interleaved stereo; NULL: absent */
    uint32_t frames;
    int playing;                  /* the transport */
    double beat;                  /* the transport's position in beats at the block's first frame (4 a bar, 0 at start) */
    double bpm;
} rec_block_t;

typedef struct {
    int state;                    /* REC_IDLE .. REC_SAVING */
    int source, mode, length_bars, threshold;   /* the settings in force */
    uint64_t frames;              /* the take so far (recording) */
    uint32_t bars;                /* whole bars so far (Bars: by beat; else by the tempo; 0 with no tempo) */
    int peak;                     /* the source's peak in the latest block, 0..32767 (stems: the loudest stream) */
    int hist_n;                   /* valid entries in hist (oldest first); restarts with each take */
    uint8_t hist[REC_HIST];       /* the source's peak per ~16 ms window, 0..255 (linear) */
    uint32_t overruns;            /* blocks not kept: the writer a whole ring behind */
} rec_status_t;

typedef struct {
    char path[1024];              /* the finished file ("" with no file) */
    int stream;                   /* REC_ST_*; REC_SRC_STEMS for a stems take's result with no file */
    uint32_t take;                /* the take's number (a stems take: the same in each file's result) */
    uint64_t first;               /* its first frame, counted in frames fed to rec_block since rec_new */
    uint64_t frames;
    uint32_t bars;                /* 0: not bar-cut */
    double bpm;
    int reason;                   /* REC_END_* */
} rec_take_t;

typedef struct rec rec_t;

rec_t *rec_new(const rec_cfg_t *cfg);   /* NULL: no memory, or no writer thread */
void rec_free(rec_t *r);                /* a running take ends here (REC_END_CLOSED); waits for the writer */

/* the settings: kept, applied at a block while no take records (an armed take: a mode change disarms it) */
void rec_set_source(rec_t *r, int source);       /* REC_ST_* or REC_SRC_STEMS */
void rec_set_mode(rec_t *r, int mode);
void rec_set_length_bars(rec_t *r, int bars);    /* 0: Open (Bars, Threshold) / Auto (Capture); else 1..32 */
void rec_set_threshold(rec_t *r, int peak);      /* 0..32767 */
void rec_set_label(rec_t *r, int stream, const char *label);   /* the firmware's name; NULL / "": the default */

void rec_press(rec_t *r);                        /* the record key */
void rec_block(rec_t *r, const rec_block_t *b);  /* the engine thread, once a block: never blocks */

void rec_get(rec_t *r, rec_status_t *s);
int rec_poll_take(rec_t *r, rec_take_t *t);      /* 1: a finished take (one per call), 0: none */

/* tests: the writer sleeps this long before each chunk it writes */
void rec_test_write_delay(rec_t *r, uint32_t us);

#endif
