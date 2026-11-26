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
 * kit_work). The memory a sound leaves is freed there too, never on the engine's thread. libc + pthreads (+ libm).
 *
 * (stage 3c, making sounds) Mix and join two pads, a pad's own edits (trim, normalize, reverse), a take sliced over
 * the pads: every new file written on the file thread (round C: into the project's own folder, below; without one)
 * into the kits' Edits folder (kits_dir/Edits:
 * Samples/RemoteFM1/Kits/Edits, seen by Move's browser; never listed as a kit, it has no kit.json), 16-bit stereo WAV
 * at the first sound's rate, its name the result's ("Join 2+5", "Kick trim"; " 2", " 3" .. when taken). A sound read
 * for several pads (a take's slices: each pad a region of it) is in memory once (shared, counted once). */
#ifndef RFM14_KIT_H
#define RFM14_KIT_H
#include <stdint.h>
#include <stdio.h>

/* (round C, 2026-11-26) the project's own sounds: every project has a folder of its own, kits_dir/Projects/NN (NN its
 * slot 01..32: Samples/RemoteFM1/Kits/Projects/03, seen by Move's browser; never listed as a kit, it has no kit.json).
 * A pad's sound kept (Files' click, Files left, All To Pads, To Kit, a kit loaded, a take recorded into a pad, the
 * sampler's next empty pad, a take's slices) is copied in first (once: a file already there, or the same file copied
 * before, is not copied again; its name the source's, " 2", " 3" .. when another file has it); every new file (mix,
 * join, trim, normalize, reverse, freeze) is written there. The core keeps such a path as KIT_OWN + its name
 * ("@/Kick.wav": the open project's folder, whatever its slot), so a project cut and pasted, or duplicated, keeps its
 * sounds (the storage moves or copies the folder with it). Older projects' paths (Samples, Takes, Kits/Edits) still
 * load; their sound is copied in at its first keep or edit. The folder's files nothing refers to (the pads, the
 * Undo, the answers on their way) are deleted when the project is saved (kit_clean), a minute old at least. */
#define KIT_OWN "@/"
static inline void kit_proj_dir(const char *kits, int slot, char *o, size_t cap)
{
    if (kits && kits[0] && slot >= 0)
        snprintf(o, cap, "%s/Projects/%02d", kits, slot + 1);
    else if (cap)
        o[0] = 0;
}
static inline void kit_proj_clip(const char *kits, char *o, size_t cap)   /* a project cut or copied: its folder */
{
    if (kits && kits[0])
        snprintf(o, cap, "%s/Projects/.clip", kits);
    else if (cap)
        o[0] = 0;
}

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
/* a pad's settings (as rfm1cs's RFM_MOVE_KIT_P_*; stage 3b: Stretch and the sends) */
enum { KIT_P_PITCH, KIT_P_START, KIT_P_END, KIT_P_MODE, KIT_P_ATTACK, KIT_P_HOLD, KIT_P_RELEASE, KIT_P_CHOKE,
       KIT_P_REVERSE, KIT_P_LEVEL, KIT_P_PAN, KIT_P_MUTE, KIT_P_STRETCH, KIT_P_SEND1, KIT_P_SEND2, KIT_P_SEND3,
       KIT_P_DECAY, KIT_P_SUSTAIN, KIT_P_CUTOFF, KIT_P_RESO,   /* (round B: AHDSR, the pad's low-pass: 100 open) */
       /* (pages v2, 2026-11-26) the engines' own, Warp (Sync is KIT_P_STRETCH), the filter's morph and envelope */
       KIT_P_LOOPS, KIT_P_LOOPE, KIT_P_LOOPXF,          /* Loop: its points (of KIT_FRAC, inside the region), fade % */
       KIT_P_PDEPTH, KIT_P_PATTACK, KIT_P_PRELEASE,     /* Pitch Env: semitones -48..48, ms, ms */
       KIT_P_BITS, KIT_P_DOWNS,                         /* LoFi: 1..16 bits, a sample held 1..32 frames */
       KIT_P_GPOS, KIT_P_GSIZE, KIT_P_GDENS, KIT_P_GSPRAY, KIT_P_GSPREAD,   /* Granular: %, ms, grains at once, %, st */
       KIT_P_WARP, KIT_P_SPEED, KIT_P_WPITCH,           /* Warp: KIT_W_*, speed x 100 (25..400), cents */
       KIT_P_WGRAIN, KIT_P_WJITTER, KIT_P_WSEARCH,      /* .. a grain's ms, Grains' jitter %, Beats' search ms */
       KIT_P_MORPH, KIT_P_FENV,                         /* the filter: 0 LP .. 50 BP .. 100 HP; its envelope -100..100 */
       KIT_P_FATTACK, KIT_P_FDECAY, KIT_P_FSUSTAIN, KIT_P_FRELEASE,   /* .. its ADSR (ms, ms, %, ms) */
       KIT_P_N };
enum { KIT_W_OFF, KIT_W_REPITCH, KIT_W_GRAINS, KIT_W_BEATS, KIT_W_N };   /* (pages v2) Warp's algorithms */
#define KIT_BUS 16                /* (pages v2) KIT_SET's index for the kit bus's low-pass: param 0 cutoff, 1 reso */
/* (round B) a step's locks on one hit (as rfm1cs's RFM_MOVE_KIT_LK_*), in the settings' units */
enum { KIT_LK_PITCH, KIT_LK_START, KIT_LK_CUTOFF, KIT_LK_RESO, KIT_LK_LEVEL, KIT_LK_PAN, KIT_LK_DECAY,
       KIT_LK_MORPH, KIT_LK_FENV,                       /* (pages v2) */
       KIT_LK_CHOP, KIT_LK_CHOPE,                       /* (pages v2, the right half's Chop) chops CHOP..CHOPE (0..15) of
                                                         * the region: one once, a range looped while held */
       KIT_LK_N };
#define KIT_KPATH 160             /* a saved kit's sound's path, as the core keeps it (RFM_MOVE_KIT_PATH) */
#define KIT_FRAC 32767            /* START / END: the whole sound */
/* (pages v2) the engines: One Shot, Momentary, Loop (its loop points while held), Pitch Env (One Shot, an AR envelope
 * on the pitch), LoFi (One Shot, bits and rate reduced), Granular (grains of the region while held). Round A's Toggle
 * (3) is gone: the core loads it as Loop; an older kit.json too */
enum { KIT_M_ONESHOT, KIT_M_MOMENTARY, KIT_M_LOOP, KIT_M_PITCHENV, KIT_M_LOFI, KIT_M_GRANULAR, KIT_M_N };
/* Files' places (as rfm1cs's RFM_MOVE_KIT_PL_*), a row's kind, a load's status (as RFM_MOVE_KIT_ROW_*, RFM_MOVE_KIT_*) */
enum { KIT_PL_TAKES = 1, KIT_PL_SAMPLES = 2, KIT_PL_KITS = 3 };
enum { KIT_ROW_DIR = 1, KIT_ROW_FILE = 2, KIT_ROW_KIT = 3 };
enum { KIT_OK = 0, KIT_TOO_LONG = 1, KIT_FAILED = 2, KIT_MISSING = 3, KIT_EMPTY = 5 };   /* (EMPTY: All To Pads past the
                                                                                      * folder's sounds) */

typedef struct {
    uint32_t fs;                  /* the engine's frames a second (0: 48000) */
    uint32_t max_bytes;           /* 0: KIT_MAX_BYTES */
    const char *samples_dir;      /* Samples: the Move's sample library (NULL: none) */
    const char *kits_dir;         /* (stage 3b) saved kits, a folder each (Samples/RemoteFM1/Kits; NULL: none) */
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
enum { KIT_A_LOADED = 1, KIT_A_ROWS, KIT_A_SAVED, KIT_A_KIT, KIT_A_SLICES, KIT_A_FOUND, KIT_A_PEAKS,
       KIT_A_OWNED };             /* (round C) OWNED: pad, was (the core's path) now path (the project's copy) */
#define KIT_DEPTH 8               /* (round A) folders deep in Samples, as rfm1cs's RFM_MOVE_KIT_DEPTH */
#define KIT_LEAD_LEVEL 104        /* (round A) Trim silence: -50 dBFS (of 32767) */
#define KIT_LEAD_MS 5             /* .. the start this much before the first sound */
#define KIT_HIT_GAP_MS 60         /* (round A) the Hits' minimum spacing (the default) */
/* (stage 3c) as rfm1cs's RFM_MOVE_KIT_MG_*, _ED_*, _SM_* */
enum { KIT_MG_COPY, KIT_MG_MIX, KIT_MG_JOIN };
enum { KIT_ED_TRIM = 1, KIT_ED_NORMALIZE, KIT_ED_REVERSE, KIT_ED_FREEZE };   /* (round C: Freeze) */
#define KIT_FREEZE_S 60           /* (round C) a freeze renders this much at most */
enum { KIT_SM_HITS, KIT_SM_EQ16, KIT_SM_EQ8, KIT_SM_EQ4, KIT_SM_STEPS, KIT_SM_TAP, KIT_SM_N };   /* (round B: Tap, the core
                                                         * taps the cuts: one slice, the whole take) */
#define KIT_SLICES 16
#define KIT_NORM_PEAK 31656       /* normalize, and a mix past full scale: the peak, -0.3 dBFS */
typedef struct {                  /* (stage 3b) a kit as saved: its pads' paths and settings, the right half, the strip */
    char name[KIT_NAME];
    int layout;
    uint8_t mix[5];               /* level, pan, sends 1..3 (0..127) */
    char path[KIT_PADS][KIT_KPATH];   /* ("" empty) */
    int16_t v[KIT_PADS][KIT_P_N];
    int trim;                     /* (round A) the Trim silence (kit.json "trim"; none: 1) */
    uint8_t bus[2];               /* (pages v2) the kit bus's low-pass: cutoff (100 open), resonance (kit.json "bus") */
} kit_kit_t;
typedef struct {
    int kind;                     /* KIT_A_* (SAVED: status KIT_OK and the name in name, else KIT_FAILED; KIT: kit, status
                                   * KIT_OK or KIT_FAILED) */
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
    kit_kit_t kit;                /* KIT */
    /* (3c) SLICES: take, path, frames, rate, npk / pk its whole wave; n slices (0 failed), cuts[0..n] in frames */
    uint32_t take;
    uint32_t cuts[KIT_SLICES + 1];
    /* (round A) LOADED by kit_load_rec: lead the frame its first sound starts at less KIT_LEAD_MS (-1: not asked, 0:
     * from its start); FOUND: pad, place (0 not found), depth, folders[0..depth], sels[0..depth], take the row's id;
     * PEAKS: pad, zfrom / zcount the frames, npk / pk */
    int32_t lead;
    int depth;
    uint32_t folders[KIT_DEPTH + 1];
    int16_t sels[KIT_DEPTH + 1];
    uint32_t zfrom, zcount;
    char was[KIT_PATH];           /* (round C) OWNED: the pad's path before */
} kit_ans_t;
typedef struct kit kit_t;

kit_t *kit_new(const kit_cfg_t *cfg);   /* NULL: no memory, or no thread */
void kit_free(kit_t *k);

/* ---- the engine's thread */
/* a pad plays (vel 1..127) or is let go (vel 0); semis: over its pitch (the right half); frame: into the next
 * kit_render's frames (later ones wait their frame) */
void kit_note(kit_t *k, int pad, int semis, int vel, uint32_t frame);
/* (round B) .. a note-on with its step's locks (bit l of mask: lk[l], KIT_LK_*) on this voice only */
void kit_note_lk(kit_t *k, int pad, int semis, int vel, uint32_t frame, int mask, const int16_t *lk);
void kit_set(kit_t *k, int pad, int param, int value);
/* (stage 3b) Stretch's microseconds (KIT_SET's `from`): the pad's start..end lasts so long (0: its Pitch); a voice
 * sounding follows at once */
void kit_stretch(kit_t *k, int pad, uint32_t us);
/* (pages v2) the kit bus's low-pass (which 0: cutoff 0..100, 100 open: bypassed; 1: resonance 0..100), on the voices'
 * mix before the strip's level, pan and sends */
void kit_bus(kit_t *k, int which, int value);
/* (pages v2, the screen) the pad's newest voice as it sounds: its cutoff now (0..100, the envelope in it), its filter
 * and amp envelopes' levels (0..255) and stages (0 attack, 1 decay, 2 sustain, 3 release); 0 none sounds */
int kit_live(const kit_t *k, int pad, int *cut, int *fe, int *fe_st, int *amp, int *amp_st);
/* the kit's own strip (the Mixer's fourth): level, pan (64 centre), sends 1..3, 0..127 */
void kit_mix(kit_t *k, int level, int pan, const int sends[3]);
/* saved as a new kit (Kit 1, Kit 2 ..: a folder of copies of its sounds and kit.json): answer KIT_A_SAVED. One at a
 * time (a save not done yet: this one refused, KIT_FAILED) */
void kit_save(kit_t *k, const kit_kit_t *kit);
void kit_open(kit_t *k, uint32_t id);   /* a Kits row's kit read: answer KIT_A_KIT */
/* All To Pads: a folder's first 16 sounds in name order onto pads 1..16, an answer each (KIT_EMPTY past them) */
void kit_folder(kit_t *k, int place, uint32_t id);
void kit_load(kit_t *k, int pad, const char *path);   /* a file onto a pad (NULL / "": the pad emptied) */
void kit_load_row(kit_t *k, int pad, int place, uint32_t id);   /* a row of Files (a take, a sample) onto a pad */
void kit_rows(kit_t *k, int place, uint32_t folder, int from, int n);
void kit_preview(kit_t *k, int place, uint32_t id, int on);
/* the next answer for the core (a load: its sound already on the pad), 1; the file work posted to the file thread */
int kit_poll(kit_t *k, kit_ans_t *a);
void kit_render(kit_t *k, float *acc, uint32_t frames);   /* adds the Kit bus (stereo, int16 scale) */
/* (stage 3b) .. and the sends: send[i] an audio FX's bus (NULL: that FX not running), own the kit's output alone (the
 * sampler's Kit source; NULL: none) */
void kit_render_bus(kit_t *k, float *acc, float *const send[3], int16_t *own, uint32_t frames);
void kit_preview_mix(kit_t *k, int16_t *out, uint32_t frames);   /* adds the preview (after the recorder's tap) */
/* (stage 3c) answers: KIT_A_LOADED for the pad that gets the new file (a failure: its status, the pad keeps its own) */
void kit_merge(kit_t *k, int dst, int src, int op);   /* Mix / Join: src's region then (or plus) dst's, onto dst */
void kit_edit(kit_t *k, int pad, int op);             /* KIT_ED_*: the pad's own new file */
/* a take's cuts by method (KIT_SM_*; Steps: step_us a step): answer KIT_A_SLICES; the take is kept for kit_slice_play
 * (the preview path, never recorded) until kit_slice_done or another preview */
void kit_slice(kit_t *k, uint32_t take, int method, uint32_t step_us);
/* (round A) .. the Hits' sensitivity 1..100 (0: 50) */
void kit_slice_sens(kit_t *k, uint32_t take, int method, uint32_t step_us, int sens);
void kit_slice_play(kit_t *k, uint32_t from, uint32_t count);   /* (round B: count 0 stops it) */
uint32_t kit_slice_pos(const kit_t *k);   /* (round B, status) the slice preview's frame now (0: none) */
void kit_slice_done(kit_t *k);
/* (the file thread, tests) a take's cuts as KIT_SLICE gives them: n slices (cuts[0..n]) of pcm (stereo) */
int kit_cuts(const int16_t *pcm, uint32_t frames, uint32_t rate, int method, uint32_t step_us, uint32_t *cuts);
/* (round A) .. the Hits by a sensitivity 1..100 (50: kit_cuts'; higher: softer hits count) and a minimum spacing (ms) */
int kit_cuts_ex(const int16_t *pcm, uint32_t frames, uint32_t rate, int method, uint32_t step_us, int sens, int gap_ms,
                uint32_t *cuts);
/* (round A) Trim silence: the frame the sound's first part over KIT_LEAD_LEVEL starts, less KIT_LEAD_MS (0: none) */
uint32_t kit_lead(const int16_t *pcm, uint32_t frames, uint32_t rate);
/* (round A, the engine's thread) a take recorded into a pad: as kit_load, its answer's lead found (trim 1) */
void kit_load_rec(kit_t *k, int pad, const char *path, int trim);
/* where a pad's sound sits in Files (Takes, or Samples' folders): answer KIT_A_FOUND */
void kit_find(kit_t *k, int pad, const char *path);
/* the close-up's peaks: the pad's frames [from, from + count) as n peaks (scaled as its whole wave's), on the file
 * thread: answer KIT_A_PEAKS (a newer ask replaces one not started) */
void kit_zoom(kit_t *k, int pad, uint32_t from, uint32_t count, int n);
/* the pad's frames [from, from + count) heard once at its own rate on the preview path (never recorded) */
void kit_audition(kit_t *k, int pad, uint32_t from, uint32_t count);
int kit_auditioning(const kit_t *k);
const char *kit_pad_path(const kit_t *k, int pad);
/* (the page's Kit tab, any thread: the folder read there) the kits saved, {"kits": [{"id", "name"}]}: malloc'd */
char *kit_kits_json(const kit_t *k);   /* (engine's thread, tests) the file on the pad ("" none) */
/* (status, tests) */
int kit_voices(const kit_t *k);           /* voices sounding */
uint32_t kit_bytes(const kit_t *k);       /* the kit's sounds in memory */
uint32_t kit_frames(const kit_t *k, int pad);   /* a pad's sound's frames (0 none) */
double kit_rate_of(const kit_t *k, int pad, int semis);   /* (3b) a voice's source frames an output frame */
int kit_previewing(const kit_t *k);       /* the preview sounds */
uint32_t kit_sounding(const kit_t *k);    /* (3c) the pads a voice sounds for (bits) */

/* ---- (round C) the project's own sounds (README "The kit (round C)") */
/* the open project's folder (kit_proj_dir; "" none: nothing copied, new files into Kits/Edits). Paths KIT_OWN.. are
 * read from it; set before the project's loads */
void kit_project(kit_t *k, const char *dir);
/* as kit_load / kit_load_row, the sound kept: copied into the project's folder first, the pad gets the copy */
void kit_load_own(kit_t *k, int pad, const char *path);
void kit_load_row_own(kit_t *k, int pad, int place, uint32_t id);
/* KIT_KEEP: the sound the pad has now (its latest load) copied into the project's folder: answer KIT_A_OWNED */
void kit_keep(kit_t *k, int pad);
/* the project saved: its folder's files that none of refs (the core's paths: rfm_move_kit_refs) nor a pad, nor an
 * answer on its way, refers to deleted on the file thread (regular files of that folder only, a minute old at least) */
void kit_clean(kit_t *k, const char *const *refs, int n);
/* a path as the core keeps it, as a file (KIT_OWN.. in the project's folder; others as they are) */
void kit_resolve(const kit_t *k, const char *in, char *out, size_t cap);

/* ---- the file thread: the requests waiting, done (the take library's worker calls it through TL_Q_JOB) */
void kit_work(void *k);
#endif
