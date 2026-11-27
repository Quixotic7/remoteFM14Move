/* SPDX-License-Identifier: MPL-2.0 */
/* The looper: see looper.h. docs/design/LOOPER-SPEC.md says what it does; this file how.
 *
 * A sound (snd_t) is a loop's audio in memory, shared by the slots that hold it (a copy shares it, Undo's keep holds
 * it a while). Its holders are counted on the engine's thread; one let go by all goes to the file thread to be freed.
 * A recording writes into a spare buffer (made at looper_new); when it ends the spare becomes the new sound at once (it
 * plays from the bar it ended on) and the file thread makes its exact copy and writes its WAV, then hands the copy
 * back: the engine swaps it in and the spare is free again. The engine's thread only takes the mutex to post a request
 * or take an answer (as the kit does), and never allocates. */
#define _GNU_SOURCE
#include "looper.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "barcut.h"
#include "stretch.h"
#include "wav.h"

#ifdef LOOPER_TEST_ALLOC
#include "../tests/looper_alloc.h"                      /* (looper_test: every allocation counted by thread) */
#endif

#define NSND 128                                        /* sounds at once (32 slots, Undo's keep, copies on the way) */
#define NKEEP 6                                         /* sounds let go kept a while (Undo, a delete undone) */
#define NREQ 64
#define NANS 48
#define NGARB 64
#define STOPQ (LOOPER_SCENES)                           /* a lane's next: a stop */
#define FADE_IN_MS 3
#define FADE_OUT_MS 10
#define EDGE_MS 2                                       /* a loop's seam: faded this much each side */
#define MAXB 128                                        /* bar lines a recording keeps */
#define CLEAN_AGE_S 30                                  /* a file this new is never cleaned (a recording not yet saved) */
#define GRAIN_MS 60                                     /* (round 2) Grains / Beats: a grain's length */
#define JITTER 0.2                                      /* .. Grains: its jitter (of half a grain) */
#define SEARCH_MS 10                                    /* .. Beats: its search */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lvl_gain(int v) { return v <= 0 ? 0.0f : (float)(v > 127 ? 127 : v) / 100.0f; }

typedef struct {
    int16_t *pcm;
    uint32_t frames, rate;
    int refs;                                           /* (the engine's thread) */
    int spare;                                          /* the pcm is spare spare - 1: its exact copy is on the way */
    int dead;                                           /* let go before the copy came: the copy freed, the spare back */
    int used;                                           /* (mu) */
    char path[LOOPER_PATH];                             /* the core's ("@/Loops/..") */
} snd_t;

typedef struct {
    int slot;                                           /* -1 off */
    snd_t *s;
    double origin;                                      /* Repitch: the beat its cycles count from */
    double fpos;                                        /* Off: frames played since its start */
    float g, gstep;                                     /* the fade: gain now, per frame (+ in, - out) */
    st_grain_t gr[2];                                   /* (round 2) Grains / Beats: its two grains */
    int ginit;                                          /* .. started */
    uint32_t rnd;                                       /* .. Grains' jitter */
    double pw;                                          /* its cycle's place last frame (a new cycle: smaller) */
    int64_t at;                                         /* (an overdub) the frame it read last (-1 none) */
} voice_t;
typedef struct {
    voice_t v, old;                                     /* the loop, the one fading out */
    int next;                                           /* waiting: a scene, STOPQ, -1 */
    int nq, nres;                                       /* its quantization, its time found */
    uint32_t nframe;
    double nat;                                         /* .. its beat */
    int level;
    uint16_t pos, peak;
} lane_t;
typedef struct {
    int16_t *pcm;                                       /* the slot's sound's (a cache of s->pcm per block) */
    snd_t *s;
    int32_t v[LOOPER_P_N];
    int bars, bpm10, loading;
    uint32_t fgen;                                      /* (round 2) a file asked: its generation */
    char path[LOOPER_PATH];
} lslot_t;
enum { RQ_LOAD = 1, RQ_WRITE, RQ_CLEAN, RQ_FILE, RQ_PEAKS, RQ_DUB };
typedef struct {
    int kind, slot, bars, bpm10;
    snd_t *s;
    uint32_t frames;
    int place, n, spare;                                /* (round 2) FILE: the row's place, id; PEAKS: the columns; DUB:
                                                         * the layer's spare */
    uint32_t id, gen, from, count;                      /* .. FILE's generation; PEAKS', DUB's frames (DUB: count the
                                                         * layer's frames from 0, the rest the base's) */
    const int16_t *pcm;                                 /* (PEAKS, DUB) the sound's frames as the engine had them */
    char path[LOOPER_PATH];                             /* LOAD: the core's; WRITE: the name wanted */
    char proj[LOOPER_PATH];
} req_t;
typedef struct {
    int state;                                          /* LOOPER_RS_* (NONE: free) */
    int slot, src, track, bars, spare;                  /* (spare: its buffer while the recording owns it, -1 after) */
    int bufi;                                           /* the buffer it writes (its sound's once it ended) */
    uint64_t frames;                                    /* so far */
    int started, sres;                                  /* its start found / its time known */
    uint32_t sframe, eframe;                            /* the action's frames */
    int ereq, eres;                                     /* an end asked / its time known */
    double start_b, end_b;
    int s_off, e_off;                                   /* this block's offsets (-1 none) */
    uint32_t barf[MAXB + 1];                            /* frames at each bar line from the start */
    int nbar;
    int npk;                                            /* this bar's peaks so far */
    uint8_t pk[LOOPER_RPEAKS];
    uint32_t pk_acc, pk_n, level;
    snd_t *fin;                                         /* ending this block: its sound (made before the voices run) */
    uint64_t fin_frames;
    int fin_bars, fin_post;
    req_t fin_req;
} rec_t;

typedef struct {
    looper_ans_t a;                                     /* for the core (kind 0: the engine's own) */
    int own;                                            /* SWAP: s gets pcm (its exact copy); GOT: a loaded sound */
    snd_t *s;
    int16_t *pcm;
    uint32_t gen;                                       /* (round 2) FILE: the slot's generation asked */
} ans_t;
enum { OWN_NONE, OWN_SWAP, OWN_GOT, OWN_WFAIL, OWN_FILE, OWN_PEAKS, OWN_DUB };   /* (round 2: a file's sound,
                                                         * a peaks' reference let go, an overdub's sound) */
typedef struct {                                        /* (round 2, item 9) the overdub (one at a time) */
    int on;                                             /* 0 off, 1 waiting for its loop's next cycle, 2 layering, 3
                                                         * ended: its file on the way */
    int slot, spare, src, track;
    snd_t *base;
    int pass;                                           /* the cycles layered (0: the first: the layer's silence
                                                         * replaced) */
    int64_t hi;                                         /* .. the first pass's frames written [0, hi) */
    int64_t last;                                       /* the frame written last (a frame read twice: once) */
    int32_t idx[128];                                   /* this block's frame each output frame reads (-1 none) */
    int dead;                                           /* its slot changed while its file was made */
} dub_t;

struct looper {
    looper_cfg_t cfg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int th_on, quit;
    snd_t snd[NSND];
    int16_t *spare[LOOPER_SPARES];
    int spare_free[LOOPER_SPARES];                      /* (the engine's thread) */
    uint32_t spare_frames;
    req_t rq[NREQ];
    int rqh, rqn;
    ans_t *an;
    int anh, ann;
    snd_t *garb[NGARB];
    int ngarb;
    char cleanref[LOOPER_SLOTS + NKEEP + 8][LOOPER_PATH / 4];
    int ncleanref, clean_due;
    char clean_proj[LOOPER_PATH];
    /* the engine's thread */
    char proj[LOOPER_PATH];
    lslot_t sl[LOOPER_SLOTS];
    lane_t ln[LOOPER_LANES];
    rec_t rc[2];
    snd_t *keep[NKEEP];
    int nkeep;
    uint8_t mix[5];
    float master;
    int was_playing;
    looper_clock_t clk;
    uint64_t bytes;
    float lbuf[2 * 128];
    float dry[2 * 128], sb[3][2 * 128];
    uint16_t src_level;
    float hann[ST_HANN + 1];                            /* (round 2) the grains' window */
    dub_t dub;
    const int16_t *mon;                                 /* (round 2) this block's monitored input */
};

/* ================================================================ the mutex's queues === */
static void wake(looper_t *l) { pthread_cond_signal(&l->cv); }
static void request(looper_t *l, const req_t *r)       /* (the engine's thread) */
{
    pthread_mutex_lock(&l->mu);
    if (l->rqn < NREQ) {
        l->rq[(l->rqh + l->rqn) % NREQ] = *r;
        l->rqn++;
    }
    wake(l);
    pthread_mutex_unlock(&l->mu);
}
static void answer(looper_t *l, const ans_t *a)
{
    pthread_mutex_lock(&l->mu);
    if (l->ann < NANS) {
        l->an[(l->anh + l->ann) % NANS] = *a;
        l->ann++;
    }
    pthread_mutex_unlock(&l->mu);
}
static void core_answer(looper_t *l, int kind, int slot, int status)   /* (no sound: a status alone) */
{
    ans_t a;
    memset(&a, 0, sizeof a);
    a.a.kind = kind, a.a.slot = slot, a.a.status = status;
    answer(l, &a);
}
static snd_t *snd_new(looper_t *l)                      /* a free sound record (any thread) */
{
    int i;
    snd_t *s = NULL;
    pthread_mutex_lock(&l->mu);
    for (i = 0; i < NSND && !s; i++)
        if (!l->snd[i].used) {
            s = &l->snd[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
        }
    pthread_mutex_unlock(&l->mu);
    return s;
}
/* (the engine's thread) one holder fewer; the last: to the file thread (a spare not copied yet: dead, back later) */
static void snd_unref(looper_t *l, snd_t *s)
{
    if (!s || --s->refs > 0)
        return;
    l->bytes -= s->spare ? 0 : (uint64_t)s->frames * 4u;
    if (s->spare) {
        s->dead = 1;                                    /* (its copy frees it when it comes) */
        return;
    }
    pthread_mutex_lock(&l->mu);
    if (l->ngarb < NGARB)
        l->garb[l->ngarb++] = s;
    wake(l);
    pthread_mutex_unlock(&l->mu);
}
static void snd_ref(snd_t *s)
{
    if (s)
        s->refs++;
}

/* ================================================================ the file thread === */
static void resolve(const char *proj, const char *in, char *o, size_t cap)
{
    if (!strncmp(in, LOOPER_OWN, 2) && proj[0])
        snprintf(o, cap, "%s/%s", proj, in + 2);
    else
        snprintf(o, cap, "%s", in);
}
void looper_resolve(const looper_t *l, const char *in, char *out, int cap) { resolve(l->proj, in ? in : "", out, (size_t)cap); }
static int mkdirs(const char *path)
{
    char p[LOOPER_PATH];
    size_t i;
    snprintf(p, sizeof p, "%s", path);
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            if (mkdir(p, 0755) && errno != EEXIST)
                return -1;
            p[i] = '/';
        }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
static int write_wav(const char *path, const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    char tmp[LOOPER_PATH + 8];
    uint8_t h[44], buf[4096];
    const uint32_t bytes = frames * 4u;
    FILE *f;
    uint32_t i = 0;
    int bad = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb")))
        return -1;
#define W32(o, v) (h[o] = (uint8_t)(v), h[o + 1] = (uint8_t)((v) >> 8), h[o + 2] = (uint8_t)((v) >> 16), h[o + 3] = (uint8_t)((v) >> 24))
    memcpy(h, "RIFF", 4), W32(4, 36u + bytes), memcpy(h + 8, "WAVEfmt ", 8), W32(16, 16u);
    h[20] = 1, h[21] = 0, h[22] = 2, h[23] = 0, W32(24, rate), W32(28, rate * 4u), h[32] = 4, h[33] = 0, h[34] = 16, h[35] = 0;
    memcpy(h + 36, "data", 4), W32(40, bytes);
#undef W32
    bad |= fwrite(h, 1, 44, f) != 44;
    while (!bad && i < frames * 2u) {
        uint32_t n = 0;
        for (; n < sizeof buf / 2 && i < frames * 2u; n++, i++)
            buf[2 * n] = (uint8_t)((uint16_t)pcm[i] & 0xFF), buf[2 * n + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
        bad |= fwrite(buf, 2, n, f) != n;
    }
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}
static int peaks_of(const int16_t *pcm, uint32_t frames, int n, uint8_t *pk)
{
    int c, top = 1, raw[LOOPER_PEAKS];
    if (!frames || n > LOOPER_PEAKS)
        return 0;
    for (c = 0; c < n; c++) {
        const uint32_t a = (uint32_t)((uint64_t)frames * (uint32_t)c / (uint32_t)n);
        const uint32_t b = (uint32_t)((uint64_t)frames * (uint32_t)(c + 1) / (uint32_t)n);
        uint32_t i;
        int p = 0;
        for (i = a * 2; i < b * 2; i++)
            p = abs(pcm[i]) > p ? abs(pcm[i]) : p;
        raw[c] = p, top = p > top ? p : top;
    }
    for (c = 0; c < n; c++)
        pk[c] = (uint8_t)(raw[c] * 255 / top);
    return n;
}
/* (round 2) a new WAV in the project's Loops/: base.wav, "base 2.wav" .. (never over a file Undo may bring back); its
 * name (with .wav); 0 written */
static int write_new(looper_t *l, const char *proj, const char *base, const int16_t *pcm, uint32_t frames, char *name,
                     size_t ncap)
{
    char dir[LOOPER_PATH], path[LOOPER_PATH];
    struct stat st;
    int i, ok = 0;
    if (!proj[0])
        return -1;
    snprintf(dir, sizeof dir, "%s/Loops", proj);
    if (mkdirs(dir))
        return -1;
    for (i = 1; i < 1000 && !ok; i++) {
        if (i == 1)
            snprintf(name, ncap, "%s.wav", base);
        else
            snprintf(name, ncap, "%s %d.wav", base, i);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (stat(path, &st))
            ok = 1;
    }
    return ok && !write_wav(path, pcm, frames, l->cfg.fs) ? 0 : -1;
}
static void do_write(looper_t *l, const req_t *r)
{
    ans_t a;
    char dir[LOOPER_PATH], path[LOOPER_PATH], name[LOOPER_PATH];
    int16_t *copy = malloc((size_t)r->frames * 4u + 4u);
    struct stat st;
    int i, ok = 0;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_RECORDED, a.a.slot = r->slot, a.a.bars = r->bars, a.a.bpm10 = r->bpm10;
    a.a.frames = r->frames, a.a.rate = l->cfg.fs;
    a.own = OWN_SWAP, a.s = r->s, a.pcm = copy;
    if (copy)
        memcpy(copy, r->s->pcm, (size_t)r->frames * 4u);   /* (the spare: the engine only reads it now) */
    a.a.npk = peaks_of(copy ? copy : r->s->pcm, r->frames, LOOPER_PEAKS, a.a.pk);
    if (r->proj[0]) {
        snprintf(dir, sizeof dir, "%s/Loops", r->proj);
        if (!mkdirs(dir)) {
            for (i = 1; i < 1000 && !ok; i++) {         /* (a new name: never over a file Undo may bring back) */
                if (i == 1)
                    snprintf(name, sizeof name, "%s.wav", r->path);
                else
                    snprintf(name, sizeof name, "%s %d.wav", r->path, i);
                snprintf(path, sizeof path, "%s/%s", dir, name);
                if (stat(path, &st))
                    ok = 1;
            }
            if (ok && write_wav(path, copy ? copy : r->s->pcm, r->frames, l->cfg.fs))
                ok = 0;
        }
    }
    if (ok) {
        a.a.status = LOOPER_OK;
        snprintf(a.a.path, sizeof a.a.path, "%sLoops/%s", LOOPER_OWN, name);
    } else {
        a.a.status = LOOPER_FAILED;                     /* (no project folder, a disk error: not kept) */
        a.own = copy ? OWN_SWAP : OWN_WFAIL;
        fprintf(stderr, "looper: slot %d not written (%s)\n", r->slot, r->proj[0] ? r->proj : "no project folder");
    }
    answer(l, &a);
}
static void do_load(looper_t *l, const req_t *r)
{
    ans_t a;
    char path[LOOPER_PATH];
    const char *why = NULL;
    uint32_t frames = 0;
    int16_t *pcm;
    struct stat st;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_LOADED, a.a.slot = r->slot, a.a.bars = r->bars, a.a.bpm10 = r->bpm10;
    snprintf(a.a.path, sizeof a.a.path, "%s", r->path);
    resolve(r->proj, r->path, path, sizeof path);
    if (stat(path, &st)) {
        a.a.status = LOOPER_MISSING;
        answer(l, &a);
        return;
    }
    if ((uint64_t)st.st_size > l->cfg.max_bytes + 1024u) {
        a.a.status = LOOPER_TOO_LONG;
        answer(l, &a);
        return;
    }
    if (!(pcm = wav_read(path, &frames, &why)) || !frames) {
        free(pcm);
        a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    if (!(a.s = snd_new(l))) {
        free(pcm);
        a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    a.s->pcm = pcm, a.s->frames = frames, a.s->rate = l->cfg.fs;
    snprintf(a.s->path, sizeof a.s->path, "%s", r->path);
    a.a.status = LOOPER_OK, a.a.frames = frames, a.a.rate = l->cfg.fs;
    a.a.npk = peaks_of(pcm, frames, LOOPER_PEAKS, a.a.pk);
    a.own = OWN_GOT;
    answer(l, &a);
}
static void do_clean(looper_t *l)
{
    char dir[LOOPER_PATH], f[LOOPER_PATH + 300];
    DIR *x;
    struct dirent *e;
    const time_t t = time(NULL);
    pthread_mutex_lock(&l->mu);
    snprintf(dir, sizeof dir, "%s/Loops", l->clean_proj);
    l->clean_due = 0;
    pthread_mutex_unlock(&l->mu);
    if (!l->clean_proj[0] || !(x = opendir(dir)))
        return;
    while ((e = readdir(x))) {
        struct stat st;
        int i, kept = 0;
        const size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcmp(e->d_name + n - 4, ".wav"))
            continue;
        snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
        if (lstat(f, &st) || !S_ISREG(st.st_mode) || t - st.st_mtime < CLEAN_AGE_S)
            continue;
        pthread_mutex_lock(&l->mu);
        for (i = 0; i < l->ncleanref && !kept; i++)
            kept = !strcmp(l->cleanref[i], e->d_name);
        pthread_mutex_unlock(&l->mu);
        if (!kept)
            unlink(f);
    }
    closedir(x);
}
/* (round 2) a Files row's sound: read (cfg.read: any format the kit reads, at fs), copied into Loops/, its bars at the
 * tempo asked */
static void do_file(looper_t *l, const req_t *r)
{
    ans_t a;
    char nm[64], base[64], name[LOOPER_PATH];
    uint32_t fr = 0;
    int st = LOOPER_FAILED, i;
    int16_t *pcm;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_RECORDED, a.a.slot = r->slot, a.gen = r->gen, a.a.bpm10 = r->bpm10;
    nm[0] = 0;
    pcm = l->cfg.read ? l->cfg.read(l->cfg.rctx, r->place, r->id, l->cfg.fs, l->cfg.max_bytes, &fr, nm, sizeof nm, &st) : NULL;
    if (!pcm || !fr) {
        free(pcm);
        a.a.status = st != LOOPER_OK ? st : LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    for (i = 0; nm[i] && i < 48; i++)                   /* (a file name: printable, no '/') */
        base[i] = nm[i] == '/' || nm[i] < 32 || nm[i] > 126 ? '_' : nm[i];
    base[i] = 0;
    if (!base[0])
        snprintf(base, sizeof base, "Loop");
    {
        const double bar = (double)l->cfg.fs * 240.0 / (r->bpm10 > 0 ? r->bpm10 / 10.0 : 120.0);
        a.a.bars = clampi((int)llround(fr / bar), 1, MAXB > 64 ? 64 : MAXB);
    }
    if (write_new(l, r->proj, base, pcm, fr, name, sizeof name) || !(a.s = snd_new(l))) {
        free(pcm);
        a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    snprintf(a.s->path, sizeof a.s->path, "%sLoops/%s", LOOPER_OWN, name);
    a.s->pcm = pcm, a.s->frames = fr, a.s->rate = l->cfg.fs;
    snprintf(a.a.path, sizeof a.a.path, "%s", a.s->path);
    a.a.status = LOOPER_OK, a.a.frames = fr, a.a.rate = l->cfg.fs;
    a.a.npk = peaks_of(pcm, fr, LOOPER_PEAKS, a.a.pk);
    a.own = OWN_FILE;
    answer(l, &a);
}
static void do_peaks(looper_t *l, const req_t *r)       /* (round 2) the close-up's peaks, linear 0..255 */
{
    ans_t a;
    int c;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_PEAKS, a.a.slot = r->slot, a.a.status = LOOPER_OK, a.a.from = r->from, a.a.count = r->count;
    a.own = OWN_PEAKS, a.s = r->s;
    a.a.npk = r->n;
    for (c = 0; c < r->n; c++) {
        const uint32_t x0 = r->from + (uint32_t)((uint64_t)r->count * (uint32_t)c / (uint32_t)r->n);
        const uint32_t x1 = r->from + (uint32_t)((uint64_t)r->count * (uint32_t)(c + 1) / (uint32_t)r->n);
        uint32_t i;
        int p = 0;
        for (i = 2 * x0; i < 2 * x1 && i < 2 * r->frames; i++)
            p = abs(r->pcm[i]) > p ? abs(r->pcm[i]) : p;
        a.a.pk[c] = (uint8_t)(p * 255 / 32767);
    }
    answer(l, &a);
}
/* (item 9) an overdub ended: the loop and its layer (its first count frames) as a new sound and WAV */
static void do_dub(looper_t *l, const req_t *r)
{
    ans_t a;
    char base[64], name[LOOPER_PATH];
    int16_t *cp = malloc((size_t)r->frames * 4u + 4u);
    const int16_t *lay = l->spare[r->spare];
    uint32_t i;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_RECORDED, a.a.slot = r->slot, a.a.bars = r->bars, a.a.bpm10 = r->bpm10;
    a.own = OWN_DUB;
    if (!cp) {
        a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    for (i = 0; i < 2 * r->frames; i++) {
        const int v = r->pcm[i] + (i < 2 * r->count ? lay[i] : 0);
        cp[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    snprintf(base, sizeof base, "%d-%d %dbpm %db", r->slot / LOOPER_SCENES + 1, r->slot % LOOPER_SCENES + 1,
             (r->bpm10 + 5) / 10, r->bars);
    if (write_new(l, r->proj, base, cp, r->frames, name, sizeof name) || !(a.s = snd_new(l))) {
        free(cp);
        a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    snprintf(a.s->path, sizeof a.s->path, "%sLoops/%s", LOOPER_OWN, name);
    a.s->pcm = cp, a.s->frames = r->frames, a.s->rate = l->cfg.fs;
    snprintf(a.a.path, sizeof a.a.path, "%s", a.s->path);
    a.a.status = LOOPER_OK, a.a.frames = r->frames, a.a.rate = l->cfg.fs;
    a.a.npk = peaks_of(cp, r->frames, LOOPER_PEAKS, a.a.pk);
    answer(l, &a);
}
static int work(looper_t *l)                            /* one turn: 1 something done */
{
    req_t r;
    snd_t *g[NGARB];
    int ng, i, cl;
    pthread_mutex_lock(&l->mu);
    ng = l->ngarb;
    memcpy(g, l->garb, sizeof g[0] * (size_t)ng);
    l->ngarb = 0;
    r.kind = 0;
    if (l->rqn) {
        r = l->rq[l->rqh];
        l->rqh = (l->rqh + 1) % NREQ, l->rqn--;
    }
    cl = l->clean_due && !r.kind;
    for (i = 0; i < ng; i++) {                          /* (let go by all: freed, the record free) */
        int16_t *p = g[i]->pcm;
        g[i]->pcm = NULL, g[i]->used = 0;
        g[i]->path[0] = 0;
        pthread_mutex_unlock(&l->mu);
        free(p);
        pthread_mutex_lock(&l->mu);
    }
    pthread_mutex_unlock(&l->mu);
    switch (r.kind) {
    case RQ_LOAD: do_load(l, &r); break;
    case RQ_WRITE: do_write(l, &r); break;
    case RQ_FILE: do_file(l, &r); break;
    case RQ_PEAKS: do_peaks(l, &r); break;
    case RQ_DUB: do_dub(l, &r); break;
    default:
        if (cl)
            do_clean(l);
        break;
    }
    return ng || r.kind || cl;
}
static void *thread_main(void *arg)
{
    looper_t *l = arg;
    pthread_mutex_lock(&l->mu);
    while (!l->quit) {
        if (!l->rqn && !l->ngarb && !l->clean_due) {
            pthread_cond_wait(&l->cv, &l->mu);
            continue;
        }
        pthread_mutex_unlock(&l->mu);
        work(l);
        pthread_mutex_lock(&l->mu);
    }
    pthread_mutex_unlock(&l->mu);
    return NULL;
}
int looper_idle(looper_t *l)
{
    int idle;
    pthread_mutex_lock(&l->mu);
    idle = !l->rqn && !l->ngarb && !l->clean_due;
    pthread_mutex_unlock(&l->mu);
    return idle;
}

/* ================================================================ make, free === */
looper_t *looper_new(const looper_cfg_t *cfg)
{
    looper_t *l = calloc(1, sizeof *l);
    int i;
    if (!l)
        return NULL;
    l->cfg = *cfg;
    if (!l->cfg.fs)
        l->cfg.fs = 44100;
    if (!l->cfg.max_bytes)
        l->cfg.max_bytes = LOOPER_MAX_BYTES;
    if (!l->cfg.rec_s)
        l->cfg.rec_s = LOOPER_REC_S;
    l->spare_frames = l->cfg.fs * l->cfg.rec_s;
    if (!(l->an = calloc(NANS, sizeof *l->an))) {
        free(l);
        return NULL;
    }
    for (i = 0; i < LOOPER_SPARES; i++) {               /* (the recordings' buffers, made here: touched, so they are
                                                         * real memory before the engine writes them) */
        if (!(l->spare[i] = calloc((size_t)l->spare_frames, 4u))) {
            while (i--)
                free(l->spare[i]);
            free(l->an), free(l);
            return NULL;
        }
        l->spare_free[i] = 1;
    }
    for (i = 0; i < LOOPER_SLOTS; i++) {
        l->sl[i].v[LOOPER_P_LENGTH] = 16, l->sl[i].v[LOOPER_P_LEVEL] = 100, l->sl[i].v[LOOPER_P_PAN] = 64;
        l->sl[i].v[LOOPER_P_MEND] = LOOPER_MK_ONE;
    }
    st_hann_make(l->hann);
    l->dub.slot = -1;
    for (i = 0; i < LOOPER_LANES; i++)
        l->ln[i].v.slot = l->ln[i].old.slot = -1, l->ln[i].next = -1, l->ln[i].level = 100;
    l->rc[0].slot = l->rc[1].slot = -1;
    l->mix[0] = 100, l->mix[1] = 64;
    l->master = 1.0f;
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    if (pthread_create(&l->th, NULL, thread_main, l)) {
        looper_free(l);
        return NULL;
    }
    l->th_on = 1;
    return l;
}
void looper_free(looper_t *l)
{
    int i;
    if (!l)
        return;
    if (l->th_on) {
        pthread_mutex_lock(&l->mu);
        l->quit = 1;
        pthread_cond_signal(&l->cv);
        pthread_mutex_unlock(&l->mu);
        pthread_join(l->th, NULL);
    }
    for (i = 0; i < NSND; i++)                          /* (every sound: a spare's is the spare's) */
        if (l->snd[i].used && l->snd[i].pcm && !l->snd[i].spare)
            free(l->snd[i].pcm);
    while (l->ann) {
        ans_t *a = &l->an[l->anh];
        if (a->own == OWN_SWAP)
            free(a->pcm);                               /* (a copy no sound holds yet; a loaded one's: below) */
        else if ((a->own == OWN_FILE || a->own == OWN_DUB) && a->s && a->s->pcm)
            free(a->s->pcm), a->s->pcm = NULL;
        l->anh = (l->anh + 1) % NANS, l->ann--;
    }
    for (i = 0; i < LOOPER_SPARES; i++)
        free(l->spare[i]);
    free(l->an);
    pthread_cond_destroy(&l->cv);
    pthread_mutex_destroy(&l->mu);
    free(l);
}
void looper_project(looper_t *l, const char *dir)
{
    if (l)
        snprintf(l->proj, sizeof l->proj, "%s", dir ? dir : "");
}

/* ================================================================ the slots (the engine's thread) === */
static void voice_off(lane_t *n, int fade_frames)       /* the loop fades out (its voice to old) */
{
    if (n->v.slot < 0)
        return;
    n->old = n->v;
    n->old.gstep = -(n->old.g > 0 ? n->old.g : 1.0f) / (float)(fade_frames > 0 ? fade_frames : 1);
    n->v.slot = -1, n->v.s = NULL;
}
static void lane_kill_slot(looper_t *l, int slot)      /* a slot's sound going: its voices stop at once */
{
    lane_t *n = &l->ln[slot / LOOPER_SCENES];
    if (n->v.slot == slot)
        voice_off(n, (int)(l->cfg.fs * FADE_OUT_MS / 1000));
    if (n->old.slot == slot)
        n->old.slot = -1, n->old.s = NULL;
    if (n->next == slot % LOOPER_SCENES)
        n->next = -1;
}
static void keep_push(looper_t *l, snd_t *s)            /* a sound let go: kept a while (Undo) */
{
    if (!s)
        return;
    if (l->nkeep == NKEEP) {
        snd_unref(l, l->keep[0]);
        memmove(l->keep, l->keep + 1, sizeof l->keep[0] * (NKEEP - 1));
        l->nkeep--;
    }
    l->keep[l->nkeep++] = s;
}
static void slot_put(looper_t *l, int slot, snd_t *s)  /* the slot's sound (a held reference handed over) */
{
    lslot_t *d = &l->sl[slot];
    snd_t *was = d->s;
    if (was == s) {
        snd_unref(l, s);
        return;
    }
    if (l->dub.on && l->dub.slot == slot && s != l->dub.base) {   /* (item 9) its loop gone: the overdub with it */
        if (l->dub.on < 3) {
            l->spare_free[l->dub.spare] = 1;
            snd_unref(l, l->dub.base);
            memset(&l->dub, 0, sizeof l->dub), l->dub.slot = -1;
        } else
            l->dub.dead = 1;                            /* (its file on the way: dropped when it comes) */
    }
    lane_kill_slot(l, slot);
    if (l->ln[slot / LOOPER_SCENES].old.s == was)
        l->ln[slot / LOOPER_SCENES].old.slot = -1, l->ln[slot / LOOPER_SCENES].old.s = NULL;
    d->s = s;
    d->pcm = s ? s->pcm : NULL;
    if (s)
        snprintf(d->path, sizeof d->path, "%s", s->path);
    keep_push(l, was);
}
static snd_t *find_snd(looper_t *l, const char *path)  /* a sound with this path in a slot or the keep */
{
    int i;
    for (i = 0; i < LOOPER_SLOTS; i++)
        if (l->sl[i].s && !strcmp(l->sl[i].s->path, path))
            return l->sl[i].s;
    for (i = 0; i < l->nkeep; i++)
        if (!strcmp(l->keep[i]->path, path))
            return l->keep[i];
    return NULL;
}
static void loaded_answer(looper_t *l, int slot, const snd_t *s, int bars, int bpm10)
{
    ans_t a;
    memset(&a, 0, sizeof a);
    a.a.kind = LOOPER_A_LOADED, a.a.slot = slot, a.a.status = LOOPER_OK;
    a.a.frames = s->frames, a.a.rate = s->rate ? s->rate : l->cfg.fs, a.a.bars = bars, a.a.bpm10 = bpm10;
    snprintf(a.a.path, sizeof a.a.path, "%s", s->path);
    a.a.npk = s->pcm ? peaks_of(s->pcm, s->frames, LOOPER_PEAKS, a.a.pk) : 0;
    answer(l, &a);
}
void looper_load(looper_t *l, int slot, const char *path, int bars, int bpm10)
{
    lslot_t *d;
    snd_t *s;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS)
        return;
    d = &l->sl[slot];
    d->loading = 0;
    d->fgen++;                                          /* (round 2: a file asked before: not wanted now) */
    if (!path || !path[0]) {                            /* emptied */
        slot_put(l, slot, NULL);
        d->path[0] = 0;
        return;
    }
    d->bars = bars > 0 ? bars : d->bars, d->bpm10 = bpm10 > 0 ? bpm10 : d->bpm10;
    if (d->s && !strcmp(d->s->path, path)) {
        loaded_answer(l, slot, d->s, d->bars, d->bpm10);
        return;
    }
    if ((s = find_snd(l, path))) {                     /* (in memory: shared, no file read) */
        snd_ref(s);
        slot_put(l, slot, s);
        loaded_answer(l, slot, s, d->bars, d->bpm10);
        return;
    }
    {
        req_t r;
        memset(&r, 0, sizeof r);
        r.kind = RQ_LOAD, r.slot = slot, r.bars = d->bars, r.bpm10 = d->bpm10;
        snprintf(r.path, sizeof r.path, "%s", path);
        snprintf(r.proj, sizeof r.proj, "%s", l->proj);
        slot_put(l, slot, NULL);
        snprintf(d->path, sizeof d->path, "%s", path);
        d->loading = 1;
        request(l, &r);
    }
}
void looper_copy(looper_t *l, int dst, int src)
{
    ans_t a;
    if (!l || dst < 0 || dst >= LOOPER_SLOTS || src < 0 || src >= LOOPER_SLOTS || dst == src)
        return;
    l->sl[dst].bars = l->sl[src].bars, l->sl[dst].bpm10 = l->sl[src].bpm10;
    memcpy(l->sl[dst].v, l->sl[src].v, sizeof l->sl[dst].v);
    if (!l->sl[src].s) {
        memset(&a, 0, sizeof a);
        a.a.kind = LOOPER_A_LOADED, a.a.slot = dst, a.a.status = LOOPER_FAILED;
        answer(l, &a);
        return;
    }
    snd_ref(l->sl[src].s);
    slot_put(l, dst, l->sl[src].s);
    loaded_answer(l, dst, l->sl[dst].s, l->sl[dst].bars, l->sl[dst].bpm10);
}
void looper_set(looper_t *l, int slot, int p, int v)
{
    if (l && slot >= 0 && slot < LOOPER_SLOTS && p >= 0 && p < LOOPER_P_N)
        l->sl[slot].v[p] = v;
}
void looper_lane(looper_t *l, int lane, int p, int v)
{
    if (l && lane >= 0 && lane < LOOPER_LANES && p == LOOPER_L_LEVEL)
        l->ln[lane].level = clampi(v, 0, 127);          /* (Source, Length: each LOOP_REC carries them) */
}
void looper_mix(looper_t *l, int level, int pan, const int sends[3])
{
    int j;
    if (!l)
        return;
    l->mix[0] = (uint8_t)clampi(level, 0, 127), l->mix[1] = (uint8_t)clampi(pan, 0, 127);
    for (j = 0; j < 3; j++)
        l->mix[2 + j] = (uint8_t)clampi(sends ? sends[j] : 0, 0, 127);
}
void looper_master(looper_t *l, int m)
{
    float f;
    if (!l)
        return;
    f = (float)(m < 0 ? 1023 : m > 1023 ? 1023 : m) / 1023.0f;
    l->master = f * f;                                  /* (the firmwares' and the kit's audio taper) */
}

/* ================================================================ launching === */
void looper_launch(looper_t *l, int slot, int quant, uint32_t frame)
{
    lane_t *n;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS)
        return;
    n = &l->ln[slot / LOOPER_SCENES];
    if (l->rc[0].state && l->rc[0].slot / LOOPER_SCENES == slot / LOOPER_SCENES)
        return;                                         /* (the lane records: its loop comes when it ends) */
    n->next = slot % LOOPER_SCENES, n->nq = quant, n->nres = 0, n->nframe = frame;
}
void looper_stop(looper_t *l, int lane, int quant, uint32_t frame)
{
    lane_t *n;
    if (!l || lane < 0 || lane >= LOOPER_LANES)
        return;
    n = &l->ln[lane];
    if (n->v.slot < 0) {                                /* (nothing plays: a waiting launch goes) */
        n->next = -1;
        return;
    }
    n->next = STOPQ, n->nq = quant, n->nres = 0, n->nframe = frame;
}
static double q_beats(int q)
{
    return q == LOOPER_Q_INSTANT ? 0.0 : q == LOOPER_Q_BEAT ? 1.0 : q == LOOPER_Q_2BARS ? 8.0 : q == LOOPER_Q_4BARS ? 16.0 : 4.0;
}
static double len_beats(const lslot_t *d)               /* the part played, in its beats */
{
    const double b = d->v[LOOPER_P_LENGTH] / 4.0;
    return b > 0.25 ? b : 0.25;
}
/* a lane's waiting launch: its beat, now that the block's clock is known (playing) */
static void next_resolve(looper_t *l, lane_t *n, uint32_t frames)
{
    const looper_clock_t *c = &l->clk;
    const double now_b = c->beat0 + (double)(n->nframe < frames ? n->nframe : 0) * c->bpf;
    if (n->nq == LOOPER_Q_INSTANT)
        n->nat = now_b;
    else if (n->nq == LOOPER_Q_CLIP && n->v.slot >= 0 && l->sl[n->v.slot].v[LOOPER_P_WARP] != LOOPER_W_OFF) {
        const double L = len_beats(&l->sl[n->v.slot]), k = ceil((now_b - n->v.origin) / L - 1e-9);
        n->nat = n->v.origin + (k > 0 ? k : 0) * L;
    } else
        n->nat = bc_next_line(now_b, q_beats(n->nq == LOOPER_Q_CLIP ? LOOPER_Q_BAR : n->nq));
    n->nres = 1;
}
static void voice_start(looper_t *l, lane_t *n, int slot, double origin)
{
    lslot_t *d = &l->sl[slot];
    if (n->v.slot >= 0)
        voice_off(n, (int)(l->cfg.fs * FADE_OUT_MS / 1000));
    if (!d->s)
        return;
    n->v.slot = slot, n->v.s = d->s, n->v.origin = origin, n->v.fpos = 0;
    n->v.g = 0, n->v.gstep = 1.0f / (float)(l->cfg.fs * FADE_IN_MS / 1000);
    n->v.ginit = 0, n->v.pw = -1, n->v.rnd = 0x9E3779B9u * (uint32_t)(slot + 1);
}

/* ================================================================ recording (the engine's thread) === */
static int spare_take(looper_t *l)
{
    int i;
    for (i = 0; i < LOOPER_SPARES; i++)
        if (l->spare_free[i]) {
            l->spare_free[i] = 0;
            return i;
        }
    return -1;
}
static void rec_drop(looper_t *l, rec_t *r)
{
    if (r->state && r->spare >= 0)
        l->spare_free[r->spare] = 1;
    memset(r, 0, sizeof *r);
    r->slot = -1, r->spare = -1, r->s_off = r->e_off = -1;
}
void looper_rec(looper_t *l, int slot, int bars, int src, int played, uint32_t frame)
{
    rec_t *r;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS)
        return;
    if (l->rc[0].state == LOOPER_RS_ARMED && !l->rc[0].started)
        rec_drop(l, &l->rc[0]);                         /* (armed elsewhere: replaced) */
    if (l->rc[1].state)
        rec_drop(l, &l->rc[1]);
    r = l->rc[0].state ? &l->rc[1] : &l->rc[0];
    if (r == &l->rc[1] && !l->rc[0].ereq)
        l->rc[0].ereq = 1, l->rc[0].eframe = frame, l->rc[0].eres = 0;   /* (the one running ends at the next bar) */
    memset(r, 0, sizeof *r);
    l->sl[slot].fgen++;                                 /* (round 2: a file asked before: not wanted now) */
    r->slot = slot, r->bars = clampi(bars, 0, MAXB), r->src = src, r->track = clampi(played, 0, 2);
    r->sframe = frame, r->s_off = r->e_off = -1;
    if ((r->spare = spare_take(l)) < 0) {
        r->slot = -1;
        core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_FAILED);
        return;
    }
    r->bufi = r->spare;
    r->state = LOOPER_RS_ARMED;
    l->ln[slot / LOOPER_SCENES].next = -1;              /* (a launch waiting in its lane: the recording instead) */
}
void looper_rec_end(looper_t *l, int slot, uint32_t frame)
{
    rec_t *r = l && l->rc[0].slot == slot && l->rc[0].state ? &l->rc[0] : NULL;
    if (!r)
        return;
    if (!r->started) {
        rec_drop(l, r);
        return;
    }
    if (!r->ereq)
        r->ereq = 1, r->eframe = frame, r->eres = 0;
}
void looper_rec_cancel(looper_t *l, int slot)
{
    int i;
    if (!l)
        return;
    for (i = 1; i >= 0; i--)
        if (l->rc[i].state && l->rc[i].slot == slot) {
            rec_drop(l, &l->rc[i]);
            if (i == 0 && l->rc[1].state)
                l->rc[0] = l->rc[1], memset(&l->rc[1], 0, sizeof l->rc[1]), l->rc[1].slot = -1, l->rc[1].spare = -1;
        }
}
/* the block's recording times: rc[0]'s start (armed, playing) and end (asked: the next bar line; fixed: its bars);
 * rc[1] starts where rc[0] ends */
static void rec_resolve(looper_t *l, uint32_t frames)
{
    const looper_clock_t *c = &l->clk;
    rec_t *r = &l->rc[0];
    int i;
    for (i = 0; i < 2; i++)
        l->rc[i].s_off = l->rc[i].e_off = -1;
    if (!r->state)
        return;
    if (!c->playing) {
        if (r->started && l->was_playing) {             /* (the transport stopped: whole bars kept) */
            r->eres = 2;                                /* (cut at its last bar line) */
            r->e_off = 0;
        }
        return;
    }
    if (!r->started) {
        double B;
        if (!l->was_playing)
            B = c->beat0;                               /* (Play: its first frame is a bar line) */
        else
            B = bc_next_line(c->beat0 + (double)(r->sframe < frames ? r->sframe : 0) * c->bpf, 4.0);
        {
            const double o = bc_offset(c->beat0, c->bpf, B);
            if (o >= 0 && o < frames) {
                r->start_b = B, r->s_off = (int)o, r->started = 1, r->state = LOOPER_RS_RECORDING;
                if (r->bars)
                    r->end_b = B + 4.0 * r->bars, r->eres = 1;
            }
        }
        r->sframe = 0;                                  /* (the action's frame: its first block only) */
    }
    if (r->started && r->ereq && r->eres != 1) {        /* (asked: the next bar line from the ask, a bar at least) */
        double B = bc_next_line(c->beat0 + (double)(r->eframe < frames ? r->eframe : 0) * c->bpf, 4.0);
        if (B < r->start_b + 4.0)
            B = r->start_b + 4.0;
        if (r->bars && B > r->end_b)
            B = r->end_b;
        r->end_b = B, r->eres = 1;
    }
    if (r->started && r->eres == 1) {
        const double o = bc_offset(c->beat0, c->bpf, r->end_b);
        if (o < frames)
            r->e_off = o > (r->s_off > 0 ? r->s_off : 0) ? (int)o : (r->s_off > 0 ? r->s_off : 0);
    }
    if (r->e_off >= 0 && l->rc[1].state == LOOPER_RS_ARMED) {   /* (the next starts on that bar line) */
        rec_t *q = &l->rc[1];
        q->start_b = r->end_b, q->s_off = r->e_off, q->started = 1, q->state = LOOPER_RS_RECORDING;
        if (q->bars)
            q->end_b = q->start_b + 4.0 * q->bars, q->eres = 1;
    }
}
/* rc[0] ends at its e_off this block (a bar line): its sound made now, before the voices run, so its lane plays it from
 * that frame; its last frames come into the spare in looper_record, then the file thread's copy and file. A cut one
 * (the transport stopped, the spare full: eres 2) keeps its whole bars and does not play by itself */
static void rec_prefinish(looper_t *l, rec_t *r, uint32_t frames)
{
    const int slot = r->slot;
    uint64_t fr;
    int bars;
    snd_t *s;
    (void)frames;
    if (r->eres == 2) {
        bars = r->nbar;
        fr = bars > 0 ? r->barf[bars] : 0;
    } else {
        fr = r->frames + (uint64_t)(r->e_off - (r->s_off > 0 ? r->s_off : 0));
        bars = (int)llround((r->end_b - r->start_b) / 4.0);
        if (fr > l->spare_frames) {                     /* (past the spare: its whole bars) */
            bars = r->nbar, fr = bars > 0 ? r->barf[bars] : 0, r->eres = 2;
        }
    }
    r->fin_frames = fr, r->fin_bars = bars, r->fin = NULL;
    if (bars <= 0 || !fr) {
        core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_NOTHING);
        return;
    }
    if (l->bytes + fr * 4u > l->cfg.max_bytes) {
        core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_TOO_LONG);
        return;
    }
    if (!(s = snd_new(l))) {
        core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_FAILED);
        return;
    }
    s->pcm = l->spare[r->spare], s->frames = (uint32_t)fr, s->rate = l->cfg.fs, s->spare = r->spare + 1, s->refs = 1;
    snprintf(s->path, sizeof s->path, "%s(recording %d)", LOOPER_OWN, slot);
    l->bytes += fr * 4u;
    l->sl[slot].bars = bars;
    l->sl[slot].bpm10 = (int)llround(l->clk.bpf * l->cfg.fs * 60.0 * 10.0);
    memset(l->sl[slot].v, 0, sizeof l->sl[slot].v);
    l->sl[slot].v[LOOPER_P_LENGTH] = bars * 16, l->sl[slot].v[LOOPER_P_LEVEL] = 100, l->sl[slot].v[LOOPER_P_PAN] = 64;
    l->sl[slot].v[LOOPER_P_MEND] = LOOPER_MK_ONE;
    slot_put(l, slot, s);
    l->sl[slot].pcm = s->pcm;
    if (r->eres != 2)
        r->fin = s;                                     /* (lane_run starts it at e_off) */
    r->spare = -1;                                      /* (the sound's now) */
    {
        req_t q;
        const int lane = slot / LOOPER_SCENES;
        memset(&q, 0, sizeof q);
        q.kind = RQ_WRITE, q.slot = slot, q.s = s, q.frames = (uint32_t)fr, q.bars = bars, q.bpm10 = l->sl[slot].bpm10;
        snprintf(q.path, sizeof q.path, "%d-%d %dbpm %db", lane + 1, slot % LOOPER_SCENES + 1,
                 (l->sl[slot].bpm10 + 5) / 10, bars);
        snprintf(q.proj, sizeof q.proj, "%s", l->proj);
        r->fin_req = q;                                 /* (posted once its last frames are in) */
        r->fin_post = 1;
    }
}

/* ================================================================ the voices === */
static float edge(double q, double L, double e)         /* the seam's fade: 0 at a cycle's ends, 1 inside */
{
    float g = 1.0f;
    if (e <= 0)
        return 1.0f;
    if (q < e)
        g = (float)(q / e);
    if (L - q < e && (float)((L - q) / e) < g)
        g = (float)((L - q) / e);
    return g < 0 ? 0 : g;
}
/* (round 2) the region the markers say: frames [*S, *E) of the sound (one frame at least) */
static void region_of(const lslot_t *d, const snd_t *s, double *S, double *E)
{
    const double F = (double)s->frames;
    double a = F * clampi(d->v[LOOPER_P_MSTART], 0, LOOPER_MK_ONE) / LOOPER_MK_ONE;
    double b = F * clampi(d->v[LOOPER_P_MEND], 0, LOOPER_MK_ONE) / LOOPER_MK_ONE;
    if (b > F)
        b = F;
    if (b < a + 1)
        b = a + 1 <= F ? a + 1 : F, a = b - 1;
    *S = a, *E = b;
}
typedef struct {                                        /* (Grains / Beats) the fold's region */
    double s, e;
} reg_t;
static double reg_fold(const void *u, double p)         /* round the region (a loop) */
{
    const reg_t *r = u;
    const double n = r->e - r->s;
    if (n <= 0)
        return r->s;
    if (p >= r->e || p < r->s)
        p = r->s + fmod(fmod(p - r->s, n) + n, n);
    return p;
}
static float rnd_bi(uint32_t *x)                        /* (xorshift, -1..1: Grains' jitter) */
{
    uint32_t v = *x ? *x : 2463534242u;
    v ^= v << 13, v ^= v >> 17, v ^= v << 5;
    *x = v;
    return (float)(v >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
/* frame i of the block (beat b): the voice's sample (its gain applied), its cycle's place 0..1, the frame it reads
 * (*fr, an overdub's) */
static int voice_frame(const looper_t *l, voice_t *v, double b, float *lo, float *ro, double *where, int64_t *fr)
{
    const lslot_t *d = &l->sl[v->slot];
    const snd_t *s = v->s;
    const int w = clampi(d->v[LOOPER_P_WARP], 0, LOOPER_W_N - 1), rev = d->v[LOOPER_P_REVERSE] != 0;
    const double L = len_beats(d), r = pow(2.0, d->v[LOOPER_P_PITCH] / 1200.0);
    const double e = (double)l->cfg.fs * EDGE_MS / 1000.0;
    double S, E, RL, q, pos;
    uint32_t i0, i1;
    float fr0, g;
    region_of(d, s, &S, &E);
    RL = E - S;
    if (w == LOOPER_W_OFF) {                            /* (its own rate: frames since its start, round the region) */
        q = fmod(v->fpos, RL);
        *where = q / RL;
        g = edge(q, RL, e);
        pos = rev ? E - 1 - q : S + q;
    } else {                                            /* (the song's beat: the region fills Length's steps) */
        double x = b - v->origin;
        if (x < 0)
            x = 0;
        q = fmod(x, L);
        *where = q / L;
        g = edge(q, L, e * l->clk.bpf);
        if (w == LOOPER_W_REPITCH) {                    /* (resampled: the pitch follows; Pitch on top) */
            const double src = q * r;
            if (src >= L)
                return 0;                               /* (pitched up: its part over until the next cycle) */
            pos = rev ? E - src / L * RL : S + src / L * RL;
        } else {                                        /* Grains, Beats (stretch.h, the kit's): the playhead the song's
                                                         * place, the grains at the pitch's rate */
            const st_src_t xs = { s->pcm, s->frames, S, E, rev ? -1 : 1 };
            const reg_t rg = { S, E };
            const uint32_t glen = (uint32_t)(l->cfg.fs * GRAIN_MS / 1000) > 16 ? (uint32_t)(l->cfg.fs * GRAIN_MS / 1000) : 16;
            const float wstep = (float)ST_HANN / (float)glen;
            const int beats = w == LOOPER_W_BEATS;
            const double jit = beats ? (double)l->cfg.fs * SEARCH_MS / 1000.0 : glen * 0.5 * JITTER * r;
            double ph = rev ? E - q / L * RL : S + q / L * RL;
            float sl = 0, sr = 0;
            int k;
            ph = reg_fold(&rg, ph);
            if (!v->ginit)
                st_start(v->gr, ph, r, glen), v->ginit = 1;
            for (k = 0; k < 2; k++) {
                st_grain_t *gk = &v->gr[k];
                float a, c, wv;
                if (gk->age >= glen) {
                    const double at = st_restart(&xs, beats, jit, &v->gr[k ^ 1], ph, beats ? 0.0f : rnd_bi(&v->rnd), reg_fold, &rg);
                    gk->pos = reg_fold(&rg, at), gk->age = 0, gk->inc = r;
                }
                wv = l->hann[(uint32_t)((float)gk->age * wstep) & (ST_HANN - 1)];
                st_frame_at(s->pcm, s->frames, reg_fold(&rg, gk->pos), &a, &c);
                sl += a * wv, sr += c * wv;
                gk->pos += xs.dir * gk->inc, gk->age++;
            }
            *lo = sl * g, *ro = sr * g;
            *fr = (int64_t)ph;
            return 1;
        }
    }
    if (pos < 0)
        pos = 0;
    if (pos >= s->frames)
        pos = s->frames - 1;
    i0 = (uint32_t)pos;
    i1 = i0 + 1 < s->frames ? i0 + 1 : i0;
    fr0 = (float)(pos - i0);
    *fr = (int64_t)llround(pos) < (int64_t)s->frames ? (int64_t)llround(pos) : (int64_t)s->frames - 1;   /* (the overdub's
                                                         * frame: the nearest, steady at the loop's own tempo) */
    *lo = (s->pcm[2 * i0] + (s->pcm[2 * i1] - s->pcm[2 * i0]) * fr0) * g;
    *ro = (s->pcm[2 * i0 + 1] + (s->pcm[2 * i1 + 1] - s->pcm[2 * i0 + 1]) * fr0) * g;
    return 1;
}
/* (item 9) the overdub's layer at frame f of its sound: written yet (its first pass so far, or a whole pass) */
static int dub_has(const looper_t *l, int64_t f) { return f >= 0 && (l->dub.pass > 0 || f < l->dub.hi); }
/* a voice's frames [a, b) into the lane's buffer and the sends (its loop's level, pan, sends; the fade); dub: the
 * voice the overdub layers onto (its layer heard, the frames it reads noted) */
static void voice_run(looper_t *l, voice_t *v, int lane, uint32_t a, uint32_t b, double *where, int dub)
{
    const lslot_t *d;
    float gl, gr, sg[3];
    uint32_t i;
    int j;
    if (v->slot < 0 || !v->s || !v->s->pcm || !v->s->frames)
        return;
    d = &l->sl[v->slot];
    {
        const float g = d->v[LOOPER_P_MUTE] ? 0.0f : lvl_gain(d->v[LOOPER_P_LEVEL]);
        const int pan = clampi(d->v[LOOPER_P_PAN], 0, 127);
        gl = gr = g;
        if (pan < 64)
            gr = g * (float)pan / 64.0f;
        else if (pan > 64)
            gl = g * (float)(127 - pan) / 63.0f;
        for (j = 0; j < 3; j++)
            sg[j] = (float)clampi(d->v[LOOPER_P_SEND1 + j], 0, 127) / 127.0f;
    }
    (void)lane;
    {
        const double rate = pow(2.0, d->v[LOOPER_P_PITCH] / 1200.0);
        const int16_t *lay = dub && l->dub.on >= 2 ? l->spare[l->dub.spare] : NULL;
        for (i = a; i < b; i++) {
            float L = 0, R = 0;
            int64_t f = -1;
            double w = -1;
            const double bt = l->clk.beat0 + i * l->clk.bpf;
            if (voice_frame(l, v, bt, &L, &R, &w, &f)) {
                float x, y;
                if (lay && f >= 0 && f < (int64_t)l->spare_frames && dub_has(l, f))
                    L += lay[2 * f], R += lay[2 * f + 1];   /* (the layer so far) */
                x = L * gl * v->g, y = R * gr * v->g;
                l->lbuf[2 * i] += x, l->lbuf[2 * i + 1] += y;
                for (j = 0; j < 3; j++)
                    if (sg[j] > 0)
                        l->sb[j][2 * i] += x * sg[j], l->sb[j][2 * i + 1] += y * sg[j];
            }
            if (w >= 0)
                *where = w;
            if (dub) {                                  /* (item 9) the overdub: its cycles, the frame it writes */
                dub_t *o = &l->dub;
                const int wrap = w >= 0 && v->pw >= 0 && w < v->pw - 0.5;
                if (o->on == 1 && wrap)
                    o->on = 2, o->pass = 0, o->hi = 0;   /* (its loop's next cycle: from here) */
                else if (o->on == 2 && wrap) {
                    if (!o->pass && o->hi < (int64_t)v->s->frames) {   /* (the first pass's end: what it never read,
                                                         * silence: a pitch up's rest, a last frame or two) */
                        int16_t *lay0 = l->spare[o->spare];
                        memset(lay0 + 2 * o->hi, 0, (size_t)((int64_t)v->s->frames - o->hi) * 4u);
                        o->hi = v->s->frames;
                    }
                    o->pass++;
                }
                o->idx[i] = o->on == 2 && !d->v[LOOPER_P_REVERSE] && f >= 0 && f < (int64_t)l->spare_frames && f < (int64_t)v->s->frames ? (int32_t)f : -1;
                v->pw = w;
            }
            if (d->v[LOOPER_P_WARP] == LOOPER_W_OFF)
                v->fpos += rate;
            if (v->gstep) {
                v->g += v->gstep;
                if (v->g >= 1.0f)
                    v->g = 1.0f, v->gstep = 0;
                else if (v->g <= 0.0f) {
                    v->g = 0, v->gstep = 0, v->slot = -1, v->s = NULL;
                    return;
                }
            }
        }
    }
}
/* the lane's events this block, in order: its waiting launch / stop, its recording's start (the lane stops) and end
 * (its new loop plays) */
static void lane_run(looper_t *l, int k, uint32_t frames)
{
    lane_t *n = &l->ln[k];
    rec_t *r0 = &l->rc[0], *r1 = &l->rc[1];
    uint32_t at = 0;
    double where = -1;
    int ev_next = -1, ev_rs = -1, ev_re = -1, ev_rs1 = -1;
    if (l->clk.playing && n->next >= 0) {
        if (!n->nres)
            next_resolve(l, n, frames);
        {
            const double o = n->nq == LOOPER_Q_INSTANT ? (double)(n->nframe < frames ? n->nframe : 0)
                                                        : bc_offset(l->clk.beat0, l->clk.bpf, n->nat);
            ev_next = o < 0 ? 0 : o < frames ? (int)o : -1;
        }
    }
    if (r0->state && r0->slot / LOOPER_SCENES == k) {
        ev_rs = r0->s_off;
        ev_re = r0->e_off;
    }
    if (r1->state && r1->slot / LOOPER_SCENES == k)
        ev_rs1 = r1->s_off;
    memset(l->lbuf, 0, sizeof(float) * 2u * frames);
    while (at < frames) {
        uint32_t end = frames;
        const int evs[4] = { ev_next, ev_rs, ev_re, ev_rs1 };
        int e;
        for (e = 0; e < 4; e++)
            if (evs[e] >= 0 && (uint32_t)evs[e] >= at && (uint32_t)evs[e] < end)
                end = (uint32_t)evs[e];
        if (l->clk.playing) {
            voice_run(l, &n->old, k, at, end, &where, 0);
            voice_run(l, &n->v, k, at, end, &where,
                      l->dub.on && n->v.slot == l->dub.slot && n->v.s == l->dub.base);
        }
        at = end;
        if (at >= frames)
            break;
        if (ev_next >= 0 && (uint32_t)ev_next == at) {  /* the launch's time */
            if (n->next == STOPQ)
                voice_off(n, (int)(l->cfg.fs * FADE_OUT_MS / 1000));
            else {
                const int slot = k * LOOPER_SCENES + n->next;
                const double origin = n->nq == LOOPER_Q_INSTANT ? 4.0 * floor((n->nat + 1e-9) / 4.0) : n->nat;
                voice_start(l, n, slot, origin);
            }
            n->next = -1, ev_next = -1;
        }
        if ((ev_rs >= 0 && (uint32_t)ev_rs == at) || (ev_rs1 >= 0 && (uint32_t)ev_rs1 == at)) {
            voice_off(n, (int)(l->cfg.fs * FADE_OUT_MS / 1000));   /* (a recording in the lane: its loop stops) */
            n->next = -1;
            if (ev_rs >= 0 && (uint32_t)ev_rs == at)
                ev_rs = -1;
            if (ev_rs1 >= 0 && (uint32_t)ev_rs1 == at)
                ev_rs1 = -1;
        }
        if (ev_re >= 0 && (uint32_t)ev_re == at) {     /* (the recording ended: its loop at once, seamless: the bar
                                                         * it ended on, no fade) */
            if (r0->fin && l->sl[r0->slot].s == r0->fin) {
                voice_start(l, n, r0->slot, r0->end_b);
                n->v.g = 1.0f, n->v.gstep = 0;
            }
            ev_re = -1;
        }
    }
    if (n->v.slot >= 0 && where >= 0)
        n->pos = (uint16_t)(where * 65535.0);
    else if (n->v.slot < 0)
        n->pos = 0;
}
void looper_render_bus(looper_t *l, const looper_clock_t *c, float *acc, float *const send[3], int16_t *own, uint32_t frames)
{
    float gl, gr, sg[3];
    uint32_t i;
    int k, j;
    if (!l || !frames)
        return;
    if (frames > 128)
        frames = 128;
    l->clk = *c;
    if (l->clk.bpf <= 0)
        l->clk.bpf = 120.0 / 60.0 / l->cfg.fs;
    if (c->playing && !l->was_playing)                  /* (Play: every lane's loop from its first frame, a bar line) */
        for (k = 0; k < LOOPER_LANES; k++) {
            lane_t *n = &l->ln[k];
            if (n->v.slot >= 0)
                n->v.origin = c->beat0, n->v.fpos = 0, n->v.g = 0, n->v.gstep = 1.0f / (float)(l->cfg.fs * FADE_IN_MS / 1000),
                n->v.ginit = 0, n->v.pw = -1;
            if (n->next >= 0 && n->next != STOPQ)
                n->nq = LOOPER_Q_BAR, n->nat = c->beat0, n->nres = 1;
            else if (n->next == STOPQ)
                n->next = -1, voice_off(n, 1);
            n->old.slot = -1;
        }
    for (k = 0; k < LOOPER_SLOTS; k++)                  /* (a sound swapped for its exact copy: the slot's pointer) */
        l->sl[k].pcm = l->sl[k].s ? l->sl[k].s->pcm : NULL;
    for (k = 0; k < 128; k++)
        l->dub.idx[k] = -1;
    rec_resolve(l, frames);
    if (l->rc[0].state && l->rc[0].e_off >= 0)
        rec_prefinish(l, &l->rc[0], frames);            /* (ending this block: its sound before the voices) */
    {
        const float g = (float)clampi(l->mix[0], 0, 127) / 100.0f * l->master;
        const int pan = clampi(l->mix[1], 0, 127);
        gl = gr = g;
        if (pan < 64)
            gr = g * (float)pan / 64.0f;
        else if (pan > 64)
            gl = g * (float)(127 - pan) / 63.0f;
        for (j = 0; j < 3; j++)
            sg[j] = (float)clampi(l->mix[2 + j], 0, 127) / 127.0f;
    }
    memset(l->dry, 0, sizeof(float) * 2u * frames);
    for (j = 0; j < 3; j++)
        memset(l->sb[j], 0, sizeof(float) * 2u * frames);
    for (k = 0; k < LOOPER_LANES; k++) {
        float sbk[3][2 * 128];
        const float lg = lvl_gain(l->ln[k].level);
        int pk = 0;
        for (j = 0; j < 3; j++)                         /* (the lane's own sends: kept apart, then its level) */
            memcpy(sbk[j], l->sb[j], sizeof(float) * 2u * frames), memset(l->sb[j], 0, sizeof(float) * 2u * frames);
        lane_run(l, k, frames);
        for (i = 0; i < 2 * frames; i++) {
            const float x = l->lbuf[i] * lg;
            const int a = (int)(x < 0 ? -x : x);
            l->dry[i] += x;
            pk = a > pk ? a : pk;
            for (j = 0; j < 3; j++)
                sbk[j][i] += l->sb[j][i] * lg;
        }
        for (j = 0; j < 3; j++)
            memcpy(l->sb[j], sbk[j], sizeof(float) * 2u * frames);
        l->ln[k].peak = (uint16_t)(pk > 32767 ? 32767 : pk);
    }
    for (i = 0; i < frames; i++) {                      /* the Loops strip: level, pan, sends, the master */
        const float x = l->dry[2 * i] * gl, y = l->dry[2 * i + 1] * gr;
        const float ml = l->mon ? l->mon[2 * i] : 0.0f, mr = l->mon ? l->mon[2 * i + 1] : 0.0f;   /* (round 2: the
                                                         * monitored input, not in own) */
        acc[2 * i] += x + ml * gl, acc[2 * i + 1] += y + mr * gr;
        if (own) {
            const long a = lrintf(x), b = lrintf(y);
            own[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
            own[2 * i + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
        }
        for (j = 0; send && j < 3; j++)
            if (send[j]) {
                send[j][2 * i] += (l->sb[j][2 * i] + (l->dry[2 * i] + ml) * sg[j]) * gl;
                send[j][2 * i + 1] += (l->sb[j][2 * i + 1] + (l->dry[2 * i + 1] + mr) * sg[j]) * gr;
            }
    }
    l->mon = NULL;
    if (own && frames < 128)
        memset(own + 2 * frames, 0, sizeof(int16_t) * 2u * (128 - frames));
    l->was_playing = c->playing;
}

/* ================================================================ the recording's frames === */
static const int16_t *src_block(const looper_src_t *src, const int16_t *own, const rec_t *r)
{
    switch (r->src) {
    case LOOPER_SRC_PLAYED: return src->slot[r->track];
    case LOOPER_SRC_TRACK1:
    case LOOPER_SRC_TRACK2:
    case LOOPER_SRC_TRACK3: return src->slot[r->src - LOOPER_SRC_TRACK1];
    case LOOPER_SRC_KIT: return src->kit;
    case LOOPER_SRC_INPUT: return src->input;
    case LOOPER_SRC_MIX: return src->mix;
    default: return own;
    }
}
/* frames [a, b) of the block into r's buffer: the bar lines in it (not its first frame's when it starts there), the
 * peaks an eighth of a beat, the level; 1: the spare is full */
static int rec_copy(looper_t *l, rec_t *r, const int16_t *x, uint32_t a, uint32_t b)
{
    int16_t *buf = l->spare[r->bufi];
    const uint64_t f0 = r->frames;
    const uint32_t per = (uint32_t)(0.125 / l->clk.bpf) ? (uint32_t)(0.125 / l->clk.bpf) : 1;
    double B;
    uint32_t i;
    for (B = bc_first_line(l->clk.beat0, l->clk.bpf, 4.0);; B += 4.0) {   /* (barcut.h: the recorder's lines) */
        const double o = bc_offset(l->clk.beat0, l->clk.bpf, B);
        if (o >= b)
            break;
        if (o >= a && (f0 + (uint64_t)(o - a)) > 0 && r->nbar < MAXB &&
            f0 + (uint64_t)(o - a) > (r->nbar ? r->barf[r->nbar] : 0))
            r->barf[++r->nbar] = (uint32_t)(f0 + (uint64_t)(o - a));
    }
    for (i = a; i < b; i++) {
        if (r->frames >= l->spare_frames)
            return 1;
        if (r->nbar && r->barf[r->nbar] == r->frames && r->frames)
            r->npk = 0, r->pk_acc = 0, r->pk_n = 0;     /* (a new bar: its peaks from here) */
        if (x) {
            const int v0 = abs(x[2 * i]), v1 = abs(x[2 * i + 1]), v = v0 > v1 ? v0 : v1;
            buf[2 * r->frames] = x[2 * i], buf[2 * r->frames + 1] = x[2 * i + 1];
            r->pk_acc = (uint32_t)v > r->pk_acc ? (uint32_t)v : r->pk_acc;
            r->level = (uint32_t)v > r->level ? (uint32_t)v : r->level;
        } else
            buf[2 * r->frames] = 0, buf[2 * r->frames + 1] = 0;
        r->frames++;
        if (++r->pk_n >= per) {
            if (r->npk < LOOPER_RPEAKS)
                r->pk[r->npk++] = (uint8_t)(r->pk_acc > 32767 ? 255 : r->pk_acc * 255 / 32767);
            r->pk_acc = 0, r->pk_n = 0;
        }
    }
    return 0;
}
static void rec_next(looper_t *l)                       /* rc[1] (started where rc[0] ended) is rc[0] now */
{
    l->rc[0] = l->rc[1];
    memset(&l->rc[1], 0, sizeof l->rc[1]);
    l->rc[1].slot = -1, l->rc[1].spare = -1, l->rc[1].s_off = l->rc[1].e_off = -1;
}
/* (item 9) the overdub's frames this block: each output frame's source onto the frame its loop read (the first pass:
 * replacing the spare's old content, frames it skipped zeroed; later passes: summed, saturated) */
static void dub_write(looper_t *l, const looper_src_t *src, const int16_t *own, uint32_t frames)
{
    dub_t *o = &l->dub;
    rec_t q;
    const int16_t *x;
    int16_t *lay;
    uint32_t i;
    if (o->on != 2)
        return;
    memset(&q, 0, sizeof q);
    q.src = o->src, q.track = o->track;
    x = src_block(src, own, &q);
    lay = l->spare[o->spare];
    for (i = 0; i < frames; i++) {
        const int32_t f = o->idx[i];
        int c;
        if (f < 0)
            continue;
        if (!o->pass) {                                 /* (the first pass: the silence up to it, then the source) */
            int64_t z;
            for (z = o->hi; z < f; z++)
                lay[2 * z] = 0, lay[2 * z + 1] = 0;
            for (c = 0; c < 2; c++)
                lay[2 * f + c] = x ? x[2 * i + c] : 0;
            if (f >= o->hi)
                o->hi = (int64_t)f + 1;
            o->last = f;
            continue;
        }
        if (f == o->last || !x)
            continue;                                   /* (read twice: summed once) */
        o->last = f;
        for (c = 0; c < 2; c++) {
            const int v = lay[2 * f + c] + x[2 * i + c];
            lay[2 * f + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}
void looper_record(looper_t *l, const looper_src_t *src, const int16_t *own, uint32_t frames)
{
    rec_t *r;
    uint32_t a, b;
    if (!l)
        return;
    if (frames > 128)
        frames = 128;
    dub_write(l, src, own, frames);
    r = &l->rc[0];
    l->src_level = 0;
    if (!r->state || !r->started)
        return;
    r->level = 0;
    a = r->s_off > 0 ? (uint32_t)r->s_off : 0;
    b = r->e_off >= 0 ? (uint32_t)r->e_off : frames;
    if (b > a && l->clk.playing && rec_copy(l, r, src_block(src, own, r), a, b) && r->e_off < 0) {
        r->eres = 2, r->e_off = (int)b;                 /* (the spare full: its whole bars, ended here) */
        rec_prefinish(l, r, frames);
    }
    if (r->e_off >= 0) {                                /* (ended: its last frames are in) */
        const int e = r->e_off;
        if (r->fin_post) {
            r->fin_req.frames = (uint32_t)r->fin_frames;
            request(l, &r->fin_req);
        }
        rec_drop(l, r);
        if (l->rc[1].state == LOOPER_RS_RECORDING) {   /* (the next, from the same bar line) */
            rec_next(l);
            r = &l->rc[0];
            r->level = 0;
            if (rec_copy(l, r, src_block(src, own, r), (uint32_t)e, frames))
                r->eres = 2;
        }
    }
    l->src_level = (uint16_t)(r->level > 32767 ? 32767 : r->level);
}

/* ================================================================ the status, the answers === */
void looper_status(const looper_t *l, looper_status_t *st)
{
    int k, i;
    memset(st, 0, sizeof *st);
    for (i = 0; i < 2; i++)
        st->rec[i].slot = -1;
    if (!l)
        return;
    for (k = 0; k < LOOPER_LANES; k++) {
        const lane_t *n = &l->ln[k];
        st->lane[k].play = (int8_t)(n->v.slot >= 0 ? n->v.slot % LOOPER_SCENES : -1);
        st->lane[k].next = (int8_t)n->next;
        st->lane[k].pos = n->pos, st->lane[k].level = n->peak;
    }
    for (i = 0; i < 2; i++) {
        const rec_t *r = &l->rc[i];
        if (!r->state)
            continue;
        st->rec[i].slot = (int8_t)r->slot;
        st->rec[i].state = (uint8_t)(r->started ? (r->ereq ? LOOPER_RS_ENDING : LOOPER_RS_RECORDING) : LOOPER_RS_ARMED);
        st->rec[i].bar = (uint8_t)(r->started ? clampi(r->nbar + 1, 1, 255) : 0);
        st->rec[i].bars = (uint8_t)clampi(r->bars, 0, 255);
        if (i == 0) {
            st->npk = (uint8_t)r->npk;
            memcpy(st->pk, r->pk, sizeof st->pk);
        }
    }
    st->level = l->src_level;
    st->beat = (uint8_t)(l->clk.playing ? ((int)floor(l->clk.beat0 + 1e-9) % 4 + 4) % 4 : 0);
    st->bytes = (uint32_t)(l->bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : l->bytes);
    st->cap = (uint32_t)(l->cfg.max_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : l->cfg.max_bytes);
}
int looper_poll(looper_t *l, looper_ans_t *out)
{
    for (;;) {
        ans_t a;
        pthread_mutex_lock(&l->mu);
        if (!l->ann) {
            pthread_mutex_unlock(&l->mu);
            return 0;
        }
        a = l->an[l->anh];
        l->anh = (l->anh + 1) % NANS, l->ann--;
        pthread_mutex_unlock(&l->mu);
        if (a.own == OWN_SWAP || a.own == OWN_WFAIL) {  /* (a recording's exact copy: the spare free again) */
            snd_t *s = a.s;
            const int sp = s->spare - 1;
            if (s->dead) {                              /* (let go meanwhile: the copy freed, the record free) */
                s->pcm = a.pcm, s->spare = 0, s->refs = 1;
                snd_unref(l, s);
            } else if (a.pcm) {
                s->pcm = a.pcm, s->spare = 0;
                if (a.a.status == LOOPER_OK)
                    snprintf(s->path, sizeof s->path, "%s", a.a.path);
            }
            if (sp >= 0 && (a.pcm || s->dead))
                l->spare_free[sp] = 1;
            if (a.a.status == LOOPER_OK) {
                int k;
                for (k = 0; k < LOOPER_SLOTS; k++)
                    if (l->sl[k].s == s)
                        snprintf(l->sl[k].path, sizeof l->sl[k].path, "%s", a.a.path);
            } else if (!s->dead && a.a.slot >= 0 && a.a.slot < LOOPER_SLOTS && l->sl[a.a.slot].s == s)
                slot_put(l, a.a.slot, NULL);            /* (not written: not kept, as the core is told) */
        }
        if (a.own == OWN_GOT) {                         /* (a loaded sound: the slot still wants it) */
            lslot_t *d = &l->sl[a.a.slot];
            if (d->loading && !strcmp(d->path, a.a.path)) {
                if (l->bytes + (uint64_t)a.s->frames * 4u > l->cfg.max_bytes) {
                    a.s->refs = 1;
                    snd_unref(l, a.s);
                    d->loading = 0, d->path[0] = 0;
                    a.a.status = LOOPER_TOO_LONG;
                } else {
                    a.s->refs = 1;
                    l->bytes += (uint64_t)a.s->frames * 4u;
                    d->loading = 0;
                    slot_put(l, a.a.slot, a.s);
                }
            } else {
                a.s->refs = 1, l->bytes += (uint64_t)a.s->frames * 4u;
                snd_unref(l, a.s);
                continue;                               /* (another load since: this one's answer dropped) */
            }
        }
        if (a.own == OWN_PEAKS) {                       /* (round 2) the sound's reference let go */
            snd_unref(l, a.s);
        } else if (a.own == OWN_FILE && a.s) {          /* (round 2) a file's sound: the slot still wants it */
            lslot_t *d = &l->sl[a.a.slot];
            a.s->refs = 1, l->bytes += (uint64_t)a.s->frames * 4u;
            if (d->fgen != a.gen) {
                snd_unref(l, a.s);
                continue;                               /* (asked again since, or loaded, recorded) */
            }
            if (l->bytes > l->cfg.max_bytes) {
                snd_unref(l, a.s);
                a.a.status = LOOPER_TOO_LONG, a.a.path[0] = 0;
            } else {
                int k;
                slot_put(l, a.a.slot, a.s);
                d->bars = a.a.bars, d->bpm10 = a.a.bpm10;
                for (k = 0; k < LOOPER_P_N; k++)
                    d->v[k] = 0;
                d->v[LOOPER_P_LENGTH] = a.a.bars * 16, d->v[LOOPER_P_LEVEL] = 100, d->v[LOOPER_P_PAN] = 64;
                d->v[LOOPER_P_MEND] = LOOPER_MK_ONE;
            }
        } else if (a.own == OWN_DUB) {                  /* (item 9) an overdub's sound: swapped in, seamless */
            dub_t *o = &l->dub;
            const int ok = a.s && !o->dead && o->on == 3 && o->slot == a.a.slot && l->sl[a.a.slot].s == o->base;
            if (a.s)
                a.s->refs = 1, l->bytes += (uint64_t)a.s->frames * 4u;
            if (ok) {
                lslot_t *d = &l->sl[a.a.slot];
                lane_t *n = &l->ln[a.a.slot / LOOPER_SCENES];
                if (n->v.s == o->base)
                    n->v.s = a.s;
                if (n->old.s == o->base)
                    n->old.s = a.s;
                d->s = a.s, d->pcm = a.s->pcm;
                snprintf(d->path, sizeof d->path, "%s", a.s->path);
                keep_push(l, o->base);                  /* (the slot's reference: Undo's keep) */
            } else if (a.s)
                snd_unref(l, a.s);
            if (o->on == 3) {
                l->spare_free[o->spare] = 1;
                snd_unref(l, o->base);                  /* (the overdub's own reference) */
                memset(o, 0, sizeof *o), o->slot = -1;
            }
            if (!ok && a.a.status == LOOPER_OK)
                a.a.status = LOOPER_FAILED;
        }
        if (a.a.kind == LOOPER_A_LOADED && a.a.status != LOOPER_OK && a.a.slot >= 0 && a.a.slot < LOOPER_SLOTS)
            l->sl[a.a.slot].loading = 0;
        if (!a.a.kind)
            continue;
        *out = a.a;
        return 1;
    }
}
void looper_clean(looper_t *l, const char *const *refs, int n)
{
    int i;
    if (!l)
        return;
    pthread_mutex_lock(&l->mu);
    l->ncleanref = 0;
    for (i = 0; i < n && l->ncleanref < (int)(sizeof l->cleanref / sizeof l->cleanref[0]); i++) {
        const char *b = refs[i] ? strrchr(refs[i], '/') : NULL;
        if (b && !strncmp(refs[i], LOOPER_OWN "Loops/", 8))
            snprintf(l->cleanref[l->ncleanref++], sizeof l->cleanref[0], "%s", b + 1);
    }
    snprintf(l->clean_proj, sizeof l->clean_proj, "%s", l->proj);
    l->clean_due = 1;
    wake(l);
    pthread_mutex_unlock(&l->mu);
}
/* ================================================================ (round 2) files, peaks, overdub, monitor === */
void looper_file(looper_t *l, int slot, int place, uint32_t id, int bpm10)
{
    req_t r;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_FILE, r.slot = slot, r.place = place, r.id = id, r.bpm10 = bpm10 > 0 ? bpm10 : 1200;
    r.gen = ++l->sl[slot].fgen;
    snprintf(r.proj, sizeof r.proj, "%s", l->proj);
    request(l, &r);
}
void looper_peaks(looper_t *l, int slot, uint32_t from, uint32_t count, int n)
{
    req_t r;
    snd_t *s;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS || !(s = l->sl[slot].s) || !s->pcm || !s->frames || n <= 0 || !count)
        return;
    if (from >= s->frames)
        return;
    if (count > s->frames - from)
        count = s->frames - from;
    memset(&r, 0, sizeof r);
    r.kind = RQ_PEAKS, r.slot = slot, r.s = s, r.pcm = s->pcm, r.frames = s->frames, r.from = from, r.count = count;
    r.n = n > LOOPER_PEAKS ? LOOPER_PEAKS : n;
    snd_ref(s);                                         /* (let go when its answer comes: OWN_PEAKS) */
    request(l, &r);
}
void looper_dub(looper_t *l, int slot, int on, int src, int played)
{
    dub_t *o;
    lslot_t *d;
    if (!l || slot < 0 || slot >= LOOPER_SLOTS)
        return;
    o = &l->dub, d = &l->sl[slot];
    if (on) {
        int sp;
        if (o->on && o->slot == slot && o->on < 3)
            return;
        if (o->on && o->on < 3)
            looper_dub(l, o->slot, 0, 0, 0);            /* (one at a time: the other's committed) */
        if (o->on || !d->s || !d->s->pcm || d->s->frames > l->spare_frames ||
            l->ln[slot / LOOPER_SCENES].v.slot != slot || (l->rc[0].state && l->rc[0].slot / LOOPER_SCENES == slot / LOOPER_SCENES)) {
            core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_NOTHING);
            return;
        }
        if ((sp = spare_take(l)) < 0) {
            core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_FAILED);
            return;
        }
        memset(o, 0, sizeof *o);
        o->on = 1, o->slot = slot, o->spare = sp, o->src = src, o->track = clampi(played, 0, 2), o->base = d->s;
        snd_ref(d->s);
        return;
    }
    if (!o->on || o->slot != slot || o->on == 3)
        return;
    if (o->on == 1 || (!o->pass && !o->hi)) {           /* (nothing layered: nothing kept) */
        l->spare_free[o->spare] = 1;
        snd_unref(l, o->base);
        memset(o, 0, sizeof *o), o->slot = -1;
        core_answer(l, LOOPER_A_RECORDED, slot, LOOPER_NOTHING);
        return;
    }
    {
        req_t r;
        memset(&r, 0, sizeof r);
        r.kind = RQ_DUB, r.slot = slot, r.s = o->base, r.pcm = o->base->pcm, r.frames = o->base->frames;
        r.spare = o->spare, r.count = o->pass ? o->base->frames : (uint32_t)o->hi, r.bars = d->bars, r.bpm10 = d->bpm10;
        snprintf(r.proj, sizeof r.proj, "%s", l->proj);
        o->on = 3;                                      /* (its layer heard as it is until the swap) */
        request(l, &r);
    }
}
void looper_monitor(looper_t *l, const int16_t *in)
{
    if (l)
        l->mon = in;
}
int looper_input_armed(const looper_t *l)
{
    int i;
    if (!l)
        return 0;
    for (i = 0; i < 2; i++)
        if (l->rc[i].state && l->rc[i].src == LOOPER_SRC_INPUT)
            return 1;
    return l->dub.on && l->dub.on < 3 && l->dub.src == LOOPER_SRC_INPUT;
}
uint64_t looper_bytes(const looper_t *l) { return l ? l->bytes : 0; }
int looper_sounding(const looper_t *l)
{
    int k, n = 0;
    for (k = 0; l && k < LOOPER_LANES; k++)
        n += l->ln[k].v.slot >= 0;
    return n;
}
