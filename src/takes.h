/* SPDX-License-Identifier: MPL-2.0 */
/* The take library: the recorder's takes folder as the sampler app's Takes and Tools (rfm1cs 0.15.0 stage 2; README
 * "The take library"). It lists the days (a folder a day, YYYY-MM-DD) and their takes, newest first (a take: a WAV,
 * "<MMDD>-<HHMMSS> <label> <bpm>bpm <bars>b.wav" or "... <seconds>s.wav"; a stems take: a folder
 * "<MMDD>-<HHMMSS> Stems ..." whose Master.wav stands for it), with the source, bars and tempo from the names, a
 * picture (a few peaks) and the peaks of any window of a take; it plays a take (a reader thread reads ahead, the
 * engine adds the frames to its block and never waits); it deletes into a hidden trash (.trash/) and edits (trim,
 * normalize, reverse, fade in / out), each atomic on the file (written beside, renamed over it), the file before it
 * kept as the one undo. A WAV the recorder is still writing (*.part), dot files and the trash are never listed.
 * libc + pthreads only (native on macOS, the Move's Linux / aarch64 build).
 *
 * Threads: tl_post / tl_poll, tl_play / tl_stop / tl_mix / tl_playing / tl_has_undo from the engine's thread (never
 * blocking: a short mutex around a copy at most); the tl_* calls marked "now" do the work on the caller's thread (the
 * worker's, tests). tl_new / tl_free while the engine does not call it. */
#ifndef RFM14_TAKES_H
#define RFM14_TAKES_H
#include <stdint.h>
#include <time.h>

#define TL_THUMB 14       /* a take row's picture: peaks over its whole length */
#define TL_WPEAKS 64      /* a window's peaks, at most */
#define TL_ROWS 8         /* rows an answer holds, at most */
#define TL_NEWEST 0xFFFFFFFFu   /* TL_Q_ROWS: the newest day that has takes */
#define TL_ALL 0xFFFFFFFEu      /* TL_Q_ROWS: every take of every day, the newest first (the Move's Takes list: the
                                 * owner's 2026-11-25 "scroll through everything I've recorded, newest at the top") */
#define TL_REL 256

enum { TL_DAY = 1, TL_TAKE = 2, TL_STEMS = 3 };   /* a row's kind (as rfm1cs's RFM_MOVE_SMP_ROW_*) */
/* a take's source (as rfm1cs's RFM_MOVE_SMP_SRC_*) */
enum { TL_SRC_MASTER, TL_SRC_SLOT1, TL_SRC_SLOT2, TL_SRC_SLOT3, TL_SRC_INPUT, TL_SRC_STEMS, TL_SRC_LOOPER, TL_SRC_KIT,
       TL_SRC_NONE = 255 };
/* an edit (as rfm1cs's RFM_MOVE_SMP_OP_*) */
enum { TL_OP_DELETE = 1, TL_OP_TRIM, TL_OP_NORMALIZE, TL_OP_REVERSE, TL_OP_FADE_IN, TL_OP_FADE_OUT, TL_OP_UNDO };
#define TL_CUT_FADE 132   /* frames of fade at a trim's cut end (3 ms at 44.1 kHz) */
#define TL_NORM_PEAK 31656   /* normalize: the peak, -0.3 dBFS (32767 * 10^(-0.3 / 20)) */

typedef struct {
    uint32_t id;          /* FNV-1a of its path in the folder (a day: its folder's name); never 0 or TL_NEWEST */
    int kind;             /* TL_DAY, TL_TAKE, TL_STEMS */
    int src;              /* a take's: TL_SRC_* (stems: TL_SRC_STEMS) */
    int year, month, day; /* a day's (a take's: its day's) */
    int today;            /* a day: it is today */
    int takes;            /* a day: its takes */
    uint32_t bars;        /* a take: its bars (0: none) */
    double bpm;           /* .. its tempo (0: none) */
    uint64_t frames;      /* .. its length (a stems take: its Master's) */
    uint32_t rate;
    uint32_t folder;      /* a take: its day's id */
    uint8_t thumb[TL_THUMB];   /* (scaled to the take's peak, as tl_peaks) */
    char rel[TL_REL];     /* its path in the folder ("2026-11-24/1124-153000 Master 1.5s.wav"; a day: "2026-11-24") */
} tl_row_t;

typedef struct tl tl_t;

/* dir: the takes' folder (as the recorder's); now: the wall clock for "today" (NULL: time()) */
tl_t *tl_new(const char *dir, time_t (*now)(void));
void tl_free(tl_t *l);   /* stops its threads (a play stops) */
/* the slots' labels (the firmwares' names) to know an older take's source by ("Felucca": slot 2 when it is slot 2's) */
void tl_set_labels(tl_t *l, const char *const labels[3]);

/* ---- now (the caller's thread) */
/* rows of a folder: 0 the days, TL_NEWEST the newest day with takes, TL_ALL every take (answered as folder TL_ALL,
 * also with none), else a day's id; from, n (<= TL_ROWS). Returns
 * the rows given; *total the folder's rows, *folder_out the folder answered (0 and *total 0: no such folder / no
 * takes) */
int tl_rows(tl_t *l, uint32_t folder, int from, int n, tl_row_t *rows, int *total, uint32_t *folder_out);
int tl_find(tl_t *l, uint32_t id, tl_row_t *row);                     /* 1 found (a take or a day) */
int tl_row_of(tl_t *l, const char *path, tl_row_t *row);              /* a file the recorder saved: its take's row */
/* n peaks of the take's frames [from, from + count) (count 0: to its end): 0..255, linear, scaled to the take's own
 * peak (as its picture: a quiet take shows its shape, the same scale in every window). 1 done */
int tl_peaks(tl_t *l, uint32_t id, uint64_t from, uint64_t count, int n, uint8_t *out);
/* an edit (TL_OP_*): TRIM keeps [from, from + count); FADE_*: ms. 1 done (*after: the take now; DELETE: zeroed) */
int tl_edit(tl_t *l, uint32_t id, int op, uint64_t from, uint64_t count, int ms, tl_row_t *after);
int tl_undo(tl_t *l, tl_row_t *after);                                /* the last edit or delete undone: 1 */
int tl_has_undo(tl_t *l);                                             /* (any thread) */
/* (the page's Sampler window, README "The take library") */
/* a take renamed (a WAV take only, not a stems take): name the new file name, or its part after the head; the take's
 * own "<MMDD>-<HHMMSS> " head is put in front when the name lacks it, ".wav" after it when missing, so the list still
 * parses it. 1 done (*after: the take now, its new id; the undo follows it), 0 no such take (or a stems take, or the
 * rename failed), -1 a bad name ("/", a leading ".", empty, control characters, too long), -2 the name is taken */
#define TL_NAME_MAX 160
int tl_rename(tl_t *l, uint32_t id, const char *name, tl_row_t *after);
/* the takes for the page as JSON (malloc'd; NULL: no memory): {"day": the day listed (its id; 0: no takes), "days":
 * [{"id", "date": "YYYY-MM-DD", "today", "takes"}] newest first, "takes": [the day's takes as tl_row_json, newest first,
 * no pictures (tl_peaks has them)], "undo"}; day: a day's id, 0 or TL_NEWEST the newest with takes */
char *tl_list_json(tl_t *l, uint32_t day);
/* a row as JSON: {"id", "kind": "take" | "stems" | "day", "src", "name" (the file's), "date", "bars", "bpm", "frames",
 * "rate"}; the bytes written (cap - 1 at most) */
int tl_row_json(const tl_row_t *r, char *out, int cap);
/* a take's files: path its own (a WAV, or a stems take's folder), wav the WAV that stands for it (a stems take: its
 * Master.wav). 1 found (a take, not a day) */
int tl_file(tl_t *l, uint32_t id, char *path, size_t pcap, char *wav, size_t wcap, tl_row_t *row);

/* ---- the worker: requests in, answers out */
enum { TL_Q_ROWS = 1, TL_Q_PEAKS, TL_Q_EDIT, TL_Q_UNDO, TL_Q_SAVED, TL_Q_LIST, TL_Q_FILE, TL_Q_RENAME,
       TL_Q_JOB };            /* (the kit, kit.h) JOB: job(arg) runs on the worker, no answer */
typedef struct {
    int kind;             /* TL_Q_* */
    uint32_t take;        /* ROWS: the folder; PEAKS, EDIT, FILE, RENAME: the take; LIST: the day */
    uint64_t from, count; /* ROWS: rows; PEAKS, EDIT (TRIM): frames */
    int n;                /* ROWS: rows wanted; PEAKS: columns */
    int op, value;        /* EDIT: TL_OP_*, FADE's ms */
    int mode;             /* SAVED: the mode it was recorded in (given back) */
    char path[1024];      /* SAVED: the file the recorder saved; RENAME: the new name */
    uint32_t tag;         /* (the page's requests: not 0, given back; PEAKS with a tag are never dropped for a newer) */
    void (*job)(void *);  /* JOB: another module's file work on this thread (the kit's sounds) */
    void *arg;
} tl_req_t;
typedef struct {
    int kind;             /* the request's */
    uint32_t tag;         /* the request's */
    char *text;           /* LIST: its JSON; FILE: "<path>\n<wav>" (malloc'd: the poller frees it; NULL none) */
    uint32_t asked;       /* ROWS: the folder asked; PEAKS: the take */
    uint32_t folder;      /* ROWS: the folder answered */
    int total, from, n;   /* ROWS: its rows, the first, the rows given; PEAKS: n peaks */
    uint64_t pfrom, pcount;   /* PEAKS: the window */
    uint8_t pk[TL_WPEAKS];
    int op, ok, mode;     /* EDIT / UNDO: the op, done; RENAME: tl_rename's; LIST, FILE: 1 done; SAVED: the mode */
    int has_row;          /* EDIT, UNDO, SAVED, RENAME, FILE: row is the take now */
    tl_row_t row;
    tl_row_t rows[TL_ROWS];
} tl_ans_t;
void tl_post(tl_t *l, const tl_req_t *q);   /* a newer PEAKS replaces one not started (the latest wins) */
int tl_poll(tl_t *l, tl_ans_t *a);          /* 1: an answer */

/* ---- playback: what the engine adds to its block after the recorder's tap (never recorded) */
void tl_play(tl_t *l, uint32_t id, uint64_t from, uint64_t count);    /* count 0: to its end; one at a time */
void tl_stop(tl_t *l);
void tl_mix(tl_t *l, int16_t *out, uint32_t frames);                  /* the engine's thread, a block: adds it */
void tl_playing(tl_t *l, uint32_t *id, uint64_t *pos);               /* the take playing (0 none), its frame */

#endif
