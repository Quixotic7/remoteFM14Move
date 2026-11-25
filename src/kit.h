/* SPDX-License-Identifier: MPL-2.0 */
/* The kit (rfm1cs-abletonmove's stage 3a; README "The kit"): 16 sampled pads the core's Kit view plays. Sounds in
 * memory as 16-bit stereo (WAV: PCM 16 / 24 / 32-bit, float; AIFF / AIFC: PCM 16 / 24 / 32-bit), 64 MB a kit (a sound
 * past it refused: KIT_TOO_LONG); voices (16 pads plus the right half's pitches, 24 at once, the oldest stolen) with
 * linear interpolation, an attack / hold / release envelope, one shot, momentary and loop, reverse, level and pan,
 * choke groups with short fades; a Kit bus the engine adds to its mix (before the recorder's tap: it is in the master's
 * takes); a preview voice (Files: a sound heard as the jog lands on it) added after the recorder's tap (never recorded).
 *
 * Threads: kit_note .. kit_poll, kit_render, kit_preview_mix from the engine's thread (never blocking: a short mutex
 * around a copy at most); every file is read on the file thread: the take library's worker (cfg.tl: tl_post's
 * TL_Q_JOB runs kit_work there), or, without one, the kit's own thread (cfg.own_thread), or the caller's (tests:
 * kit_work). The memory a sound leaves is freed there too, never on the engine's thread. libc + pthreads (+ libm). */
#ifndef RFM14_KIT_H
#define RFM14_KIT_H
#include <stdint.h>

struct tl;
#define KIT_PADS 16
#define KIT_VOICES 24             /* voices sounding at once (fading ones apart) */
#define KIT_PEAKS 64              /* a pad's wave: its whole sound */
#define KIT_THUMB 14              /* a file row's picture */
#define KIT_NAME 24               /* a sound's / a row's name with its NUL */
#define KIT_PATH 1024
#define KIT_ROWS 8                /* rows an answer holds, at most */
#define KIT_MAX_BYTES (64u << 20) /* the kit's sounds in memory (16-bit stereo) */
#define KIT_PREVIEW_S 30          /* a preview plays this much of a sound at most */
/* a pad's settings (as rfm1cs's RFM_MOVE_KIT_P_*) */
enum { KIT_P_PITCH, KIT_P_START, KIT_P_END, KIT_P_MODE, KIT_P_ATTACK, KIT_P_HOLD, KIT_P_RELEASE, KIT_P_CHOKE,
       KIT_P_REVERSE, KIT_P_LEVEL, KIT_P_PAN, KIT_P_MUTE, KIT_P_N };
#define KIT_FRAC 32767            /* START / END: the whole sound */
enum { KIT_M_ONESHOT, KIT_M_MOMENTARY, KIT_M_LOOP };
/* Files' places (as rfm1cs's RFM_MOVE_KIT_PL_*), a row's kind, a load's status (as RFM_MOVE_KIT_ROW_*, RFM_MOVE_KIT_*) */
enum { KIT_PL_TAKES = 1, KIT_PL_SAMPLES = 2 };
enum { KIT_ROW_DIR = 1, KIT_ROW_FILE = 2 };
enum { KIT_OK = 0, KIT_TOO_LONG = 1, KIT_FAILED = 2, KIT_MISSING = 3 };

typedef struct {
    uint32_t fs;                  /* the engine's frames a second (0: 48000) */
    uint32_t max_bytes;           /* 0: KIT_MAX_BYTES */
    const char *samples_dir;      /* Samples: the Move's sample library (NULL: none) */
    struct tl *tl;                /* Takes, and the file thread (NULL: none) */
    int own_thread;               /* no tl: a thread of the kit's own does the files (0: kit_work by the caller) */
} kit_cfg_t;
typedef struct {
    uint32_t id;                  /* never 0 (a take: its id; a folder or file: FNV-1a of its path in Samples) */
    int kind;                     /* KIT_ROW_* */
    char name[KIT_NAME];
    uint32_t frames, rate;
    uint8_t thumb[KIT_THUMB];
} kit_row_t;
enum { KIT_A_LOADED = 1, KIT_A_ROWS };
typedef struct {
    int kind;                     /* KIT_A_* */
    int pad, status;              /* LOADED: the pad, KIT_OK .. */
    char name[KIT_NAME];
    char path[KIT_PATH];          /* LOADED: the file (the project keeps it) */
    uint32_t frames, rate;
    int npk;
    uint8_t pk[KIT_PEAKS];
    int place;                    /* ROWS: as asked */
    uint32_t folder;
    int total, from, n;
    kit_row_t rows[KIT_ROWS];
} kit_ans_t;
typedef struct kit kit_t;

kit_t *kit_new(const kit_cfg_t *cfg);   /* NULL: no memory, or no thread */
void kit_free(kit_t *k);

/* ---- the engine's thread */
/* a pad plays (vel 1..127) or is let go (vel 0); semis: over its pitch (the right half); frame: into the next
 * kit_render's frames (later ones wait their frame) */
void kit_note(kit_t *k, int pad, int semis, int vel, uint32_t frame);
void kit_set(kit_t *k, int pad, int param, int value);
void kit_load(kit_t *k, int pad, const char *path);   /* a file onto a pad (NULL / "": the pad emptied) */
void kit_load_row(kit_t *k, int pad, int place, uint32_t id);   /* a row of Files (a take, a sample) onto a pad */
void kit_rows(kit_t *k, int place, uint32_t folder, int from, int n);
void kit_preview(kit_t *k, int place, uint32_t id, int on);
/* the next answer for the core (a load: its sound already on the pad), 1; the file work posted to the file thread */
int kit_poll(kit_t *k, kit_ans_t *a);
void kit_render(kit_t *k, float *acc, uint32_t frames);   /* adds the Kit bus (stereo, int16 scale) */
void kit_preview_mix(kit_t *k, int16_t *out, uint32_t frames);   /* adds the preview (after the recorder's tap) */
/* (status, tests) */
int kit_voices(const kit_t *k);           /* voices sounding */
uint32_t kit_bytes(const kit_t *k);       /* the kit's sounds in memory */
uint32_t kit_frames(const kit_t *k, int pad);   /* a pad's sound's frames (0 none) */
int kit_previewing(const kit_t *k);       /* the preview sounds */

/* ---- the file thread: the requests waiting, done (the take library's worker calls it through TL_Q_JOB) */
void kit_work(void *k);
#endif
