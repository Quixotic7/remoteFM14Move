/* SPDX-License-Identifier: MPL-2.0 */
/* takes.h
 *
 * The folder is the truth: every call lists it again (names only; a few dozen entries), so takes the recorder adds, a
 * file the page's browser removes, a day that ends, all show without being told. What costs is cached: a take's peak
 * table (one peak each TL_STEP frames, kept for the last few takes, checked against the file's size and time), which
 * gives the pictures and the wide windows; a narrow window (Trim zoomed) reads its frames. Edits stream (a take of half
 * an hour is never in memory whole). The undo is one file or folder in .trash/ and a line in .trash/undo.txt. */
#define _GNU_SOURCE
#include "takes.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "WAV samples are read as they are: a little-endian host"
#endif

#define LD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#define TL_STEP 256u      /* a peak table's frames a peak */
#define NCACHE 6          /* peak tables kept */
#define CHUNK 16384u      /* frames read or written at a time */
#define QN 32             /* requests not started */
#define AN 16             /* answers not polled (more: the oldest go) */
#define RING 32768u       /* playback read ahead, frames (0.74 s) */
#define PCHUNK 4096u      /* frames the player reads at a time */
#define MAXN 4096         /* names in a folder, at most */

typedef struct {          /* a WAV's data: PCM 16-bit, 1 or 2 channels */
    uint64_t off, frames;
    uint32_t rate;
    int ch;
} wav_t;

typedef struct {          /* a take's peak table */
    char path[1100];
    off_t size;
    time_t mtime;
    long mtime_ns;
    uint64_t frames;
    uint8_t *pk;
    uint32_t n;
    uint32_t used;
    uint8_t max;          /* the take's peak (its pictures are scaled to it) */
} ptab_t;

struct tl {
    char dir[1024];
    time_t (*now)(void);
    pthread_mutex_t mu;   /* the caches, the undo, the labels, the files (the worker's and the player's) */
    char labels[3][64];
    ptab_t cache[NCACHE];
    uint32_t tick;
    int undo_op;          /* the undo: TL_OP_* (0 none), the take's path, its copy in the trash */
    char undo_rel[TL_REL];
    char undo_trash[1400];
    int has_undo;         /* (atomic: the engine's status) */

    /* the worker */
    pthread_t wth;
    int w_on, quit;
    pthread_mutex_t qmu;
    pthread_cond_t qcv;
    tl_req_t q[QN];
    int qh, qn;
    pthread_mutex_t amu;
    tl_ans_t *ans;
    int ah, an;

    /* the player */
    pthread_t pth;
    int p_on, p_quit;
    pthread_mutex_t pmu;
    pthread_cond_t pcv;
    uint32_t req_gen, req_id;
    uint64_t req_from, req_count;
    int16_t *ring;
    uint32_t wr, rd;      /* free-running frame counts: the player moves wr, the engine rd */
    uint32_t want_gen, ack_gen, eof_gen;   /* the player asks the engine to drop what is in the ring (want), the engine
                                            * says it did (ack); the player read the take's last frame (eof) */
    uint32_t n_id;        /* (before want_gen) the take that plays from then, its first frame */
    uint64_t n_from;
    /* the engine's */
    uint32_t cur_gen, cur_id;
    uint64_t cur_pos;
    uint32_t pub_id;      /* (atomic) the take playing, its frame */
    uint64_t pub_pos;
};

/* ------------------------------------------------------------------ names --- */
static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h == 0 || h == TL_NEWEST || h == TL_ALL ? 1u : h;
}

static int is_day(const char *s, int *y, int *m, int *d)   /* YYYY-MM-DD */
{
    int i;
    for (i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? s[i] != '-' : (s[i] < '0' || s[i] > '9'))
            return 0;
    if (s[10])
        return 0;
    *y = atoi(s), *m = atoi(s + 5), *d = atoi(s + 8);
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

static int ends(const char *s, const char *t)
{
    const size_t a = strlen(s), b = strlen(t);
    return a >= b && !strcmp(s + a - b, t);
}

static int is_head(const char *s)   /* "<MMDD>-<HHMMSS> " */
{
    int i;
    for (i = 0; i < 11; i++)
        if (i == 4 ? s[i] != '-' : (s[i] < '0' || s[i] > '9'))
            return 0;
    return s[11] == ' ';
}

/* a take's label to its source ("Master", "Track 2 Felucca", "Slot 2", "Input", ..; else a slot's firmware's name) */
static int src_of(const tl_t *l, const char *lab)
{
    int k;
    if (!strcmp(lab, "Master"))
        return TL_SRC_MASTER;
    if (!strcmp(lab, "Input"))
        return TL_SRC_INPUT;
    if (!strcmp(lab, "Looper"))
        return TL_SRC_LOOPER;
    if (!strcmp(lab, "Kit"))
        return TL_SRC_KIT;
    if (!strncmp(lab, "Track ", 6) || !strncmp(lab, "Slot ", 5)) {
        const char *n = lab + (lab[0] == 'T' ? 6 : 5);
        if (n[0] >= '1' && n[0] <= '3' && (n[1] == 0 || n[1] == ' '))
            return TL_SRC_SLOT1 + n[0] - '1';
    }
    for (k = 0; k < 3; k++)
        if (l->labels[k][0] && !strcmp(lab, l->labels[k]))
            return TL_SRC_SLOT1 + k;
    return TL_SRC_NONE;
}

/* the name's parts: its source from its label (between the head and the tail), its bars and tempo
 * ("<bpm>bpm <bars>b"; "<seconds>s": none) */
static void parse(const tl_t *l, const char *name, int folder, int *src, uint32_t *bars, double *bpm)
{
    char s[TL_REL], *t, *sp;
    size_t n;
    *src = TL_SRC_NONE, *bars = 0, *bpm = 0;
    snprintf(s, sizeof s, "%s", name);
    if (!folder && ends(s, ".wav"))
        s[strlen(s) - 4] = 0;
    n = strlen(s);
    if (n > 4 && s[n - 1] == ')') {                     /* " (2)": never over a take */
        char *o = strrchr(s, '(');
        if (o && o > s && o[-1] == ' ')
            o[-1] = 0;
    }
    if (!is_head(s))
        return;
    t = s + 12;
    sp = strrchr(t, ' ');
    if (sp && sp > t && ends(sp + 1, "b") && sp[1] >= '0' && sp[1] <= '9') {   /* "<bpm>bpm <bars>b" */
        char *sp2;
        *sp = 0;
        sp2 = strrchr(t, ' ');
        if (sp2 && ends(sp2 + 1, "bpm")) {
            *bars = (uint32_t)atoi(sp + 1);
            *bpm = atof(sp2 + 1);
            *sp2 = 0;
        } else
            *sp = ' ';
    } else if (sp && sp > t && ends(sp + 1, "s") && sp[1] >= '0' && sp[1] <= '9')
        *sp = 0;                                        /* (the header has the length) */
    if (folder && !strcmp(t, "Stems"))
        *src = TL_SRC_STEMS;
    else
        *src = src_of(l, t);
}

/* ------------------------------------------------------------------ WAV --- */
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int wav_info(int fd, wav_t *w)
{
    uint8_t h[12], c[8], f[16];
    uint64_t at = 12;
    int fmt = 0;
    struct stat sb;
    memset(w, 0, sizeof *w);
    if (fstat(fd, &sb) || pread(fd, h, 12, 0) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    while (at + 8 <= (uint64_t)sb.st_size && pread(fd, c, 8, (off_t)at) == 8) {
        uint64_t sz = le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && sz >= 16 && pread(fd, f, 16, (off_t)(at + 8)) == 16) {
            const int tag = f[0] | f[1] << 8, bits = f[14] | f[15] << 8;
            w->ch = f[2] | f[3] << 8, w->rate = le32(f + 4);
            fmt = tag == 1 && bits == 16 && (w->ch == 1 || w->ch == 2);
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt)
                return -1;
            if (at + 8 + sz > (uint64_t)sb.st_size)    /* (a header not updated: what is there) */
                sz = (uint64_t)sb.st_size - at - 8;
            w->off = at + 8, w->frames = sz / (uint64_t)(2 * w->ch);
            return 0;
        }
        at += 8 + sz + (sz & 1);
    }
    return -1;
}

/* frames [at, at + n) as stereo int16 (mono: L = R); the frames read */
static uint32_t wav_read(int fd, const wav_t *w, uint64_t at, uint32_t n, int16_t *lr)
{
    ssize_t got;
    uint32_t k, i;
    if (at >= w->frames)
        return 0;
    if (at + n > w->frames)
        n = (uint32_t)(w->frames - at);
    got = pread(fd, lr, (size_t)n * 2u * (size_t)w->ch, (off_t)(w->off + at * 2u * (uint64_t)w->ch));
    if (got <= 0)
        return 0;
    k = (uint32_t)((size_t)got / (2u * (size_t)w->ch));
    if (w->ch == 1)
        for (i = k; i-- > 0;)
            lr[2 * i + 1] = lr[2 * i] = lr[i];
    return k;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void wav_header(uint8_t *h, uint32_t fs, uint64_t frames)   /* the recorder's: PCM, 2 channels, 16 bits */
{
    const uint32_t data = (uint32_t)(frames * 4u);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36u + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put32(h + 20, 1u | 2u << 16);
    put32(h + 24, fs);
    put32(h + 28, fs * 4u);
    put32(h + 32, 4u | 16u << 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

static int pk8(int v) { return v ? (v * 255 + 32767) / 32768 : 0; }   /* |sample| 0..32768 as 0..255 (not 0: >= 1) */
static int absv(int16_t s) { return s < 0 ? -(int)s : s; }

/* ------------------------------------------------------------------ the folder --- */
static int cmp_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }
static int cmp_asc(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* the names in dir that pass keep(), sorted (desc: newest, the greatest, first), in a malloc'd array of TL_REL each */
static int names(const char *dir, int (*keep)(const char *dir, const char *name), int desc, char (**out)[TL_REL])
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char (*v)[TL_REL] = NULL;
    int n = 0, cap = 0;
    *out = NULL;
    if (!d)
        return 0;
    while ((e = readdir(d)) && n < MAXN) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= TL_REL - 16 || !keep(dir, e->d_name))
            continue;
        if (n == cap) {
            char (*nv)[TL_REL] = realloc(v, (size_t)(cap = cap ? cap * 2 : 32) * TL_REL);
            if (!nv)
                break;
            v = nv;
        }
        snprintf(v[n++], TL_REL, "%s", e->d_name);
    }
    closedir(d);
    if (n)
        qsort(v, (size_t)n, TL_REL, desc ? cmp_desc : cmp_asc);
    *out = v;
    return n;
}

static int is_dir(const char *p)
{
    struct stat sb;
    return !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

/* a take in a day's folder: a WAV (not .part) or a stems folder (named, whole: not .part) */
static int keep_take(const char *dir, const char *name)
{
    char p[1400];
    if (ends(name, ".part") || !is_head(name))
        return 0;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (ends(name, ".wav"))
        return !is_dir(p);
    return !strncmp(name + 12, "Stems", 5) && is_dir(p);
}
static int keep_wav(const char *dir, const char *name)
{
    (void)dir;
    return ends(name, ".wav");
}
static int takes_in(const tl_t *l, const char *day, char (**out)[TL_REL])
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", l->dir, day);
    return names(p, keep_take, 1, out);
}
static int keep_day(const char *dir, const char *name)
{
    char p[1400];
    char (*t)[TL_REL];
    int y, m, d, n;
    if (!is_day(name, &y, &m, &d))
        return 0;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    n = names(p, keep_take, 1, &t);
    free(t);
    return n > 0;
}
static int days(const tl_t *l, char (**out)[TL_REL]) { return names(l->dir, keep_day, 1, out); }

static int is_stems(const tl_t *l, const char *rel)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", l->dir, rel);
    return is_dir(p);
}
/* the WAV that stands for a take (a stems folder: its Master.wav, else its first WAV) */
static void take_file(const tl_t *l, const char *rel, char *p, size_t cap)
{
    char d[1400];
    char (*w)[TL_REL];
    int n;
    snprintf(d, sizeof d, "%s/%s", l->dir, rel);
    if (!is_dir(d)) {
        snprintf(p, cap, "%s", d);
        return;
    }
    snprintf(p, cap, "%s/Master.wav", d);
    if (!access(p, F_OK))
        return;
    n = names(d, keep_wav, 0, &w);
    if (n)
        snprintf(p, cap, "%s/%s", d, w[0]);
    free(w);
}

/* ------------------------------------------------------------------ peaks --- */
/* the take's peak table (computed once, cached by its path, size and time); NULL: not a WAV this reads */
static ptab_t *ptab(tl_t *l, const char *path, wav_t *wout)
{
    struct stat sb;
    ptab_t *t, *v = NULL;
    wav_t w;
    int fd, k;
    int16_t *buf;
    uint64_t at;
    if (stat(path, &sb))
        return NULL;
    l->tick++;
    for (k = 0; k < NCACHE; k++) {
        t = &l->cache[k];
        if (t->pk && !strcmp(t->path, path) && t->size == sb.st_size && t->mtime == sb.st_mtime) {
            t->used = l->tick;
            if (wout) {
                memset(wout, 0, sizeof *wout);
                wout->frames = t->frames;
            }
            return t;
        }
        if (!v || (v->pk && (!t->pk || t->used < v->used)))
            v = t;                                      /* (a free one, else the least used) */
    }
    if ((fd = open(path, O_RDONLY)) < 0)
        return NULL;
    if (wav_info(fd, &w) || !(buf = malloc(CHUNK * 4u))) {
        close(fd);
        return NULL;
    }
    free(v->pk);
    memset(v, 0, sizeof *v);
    v->n = (uint32_t)((w.frames + TL_STEP - 1) / TL_STEP);
    if (!(v->pk = calloc(v->n ? v->n : 1, 1))) {
        free(buf);
        close(fd);
        return NULL;
    }
    for (at = 0; at < w.frames;) {
        const uint32_t got = wav_read(fd, &w, at, CHUNK, buf);
        uint32_t i;
        if (!got)
            break;
        for (i = 0; i < got; i++) {
            const uint32_t j = (uint32_t)((at + i) / TL_STEP);
            const int a = absv(buf[2 * i]) > absv(buf[2 * i + 1]) ? absv(buf[2 * i]) : absv(buf[2 * i + 1]);
            const uint8_t p = (uint8_t)pk8(a);
            if (p > v->pk[j])
                v->pk[j] = p;
        }
        at += got;
    }
    free(buf);
    close(fd);
    snprintf(v->path, sizeof v->path, "%s", path);
    v->size = sb.st_size, v->mtime = sb.st_mtime, v->frames = w.frames, v->used = l->tick;
    for (k = 0; k < (int)v->n; k++)
        v->max = v->pk[k] > v->max ? v->pk[k] : v->max;
    if (wout)
        *wout = w;
    return v;
}
static void ptab_forget(tl_t *l, const char *path)
{
    int k;
    for (k = 0; k < NCACHE; k++)
        if (l->cache[k].pk && !strncmp(l->cache[k].path, path, strlen(path))) {
            free(l->cache[k].pk);
            memset(&l->cache[k], 0, sizeof l->cache[k]);
        }
}

/* a picture's peaks scaled to the take's own peak (a quiet take shows its shape; the same scale in every window) */
static void scaled(const ptab_t *t, uint8_t *v, int n)
{
    int i;
    for (i = 0; t->max && i < n; i++) {
        const int s = (v[i] * 255 + t->max - 1) / t->max;
        v[i] = (uint8_t)(s > 255 ? 255 : s);
    }
}
/* n peaks of [from, from + count) of the file at path (a wide window from its table, a narrow one read), scaled */
static int peaks_of(tl_t *l, const char *path, uint64_t from, uint64_t count, int n, uint8_t *out)
{
    wav_t w;
    ptab_t *t = ptab(l, path, &w);
    int c;
    if (!t || n <= 0)
        return 0;
    if (!count || from + count > t->frames)
        count = from < t->frames ? t->frames - from : 0;
    memset(out, 0, (size_t)n);
    if (!count)
        return 1;
    if (count / (uint64_t)n >= TL_STEP) {
        for (c = 0; c < n; c++) {
            const uint64_t a = from + count * (uint64_t)c / (uint64_t)n, b = from + count * (uint64_t)(c + 1) / (uint64_t)n;
            uint64_t j;
            for (j = a / TL_STEP; j < (b + TL_STEP - 1) / TL_STEP && j < t->n; j++)
                out[c] = t->pk[j] > out[c] ? t->pk[j] : out[c];
        }
        scaled(t, out, n);
        return 1;
    }
    {                                                   /* (narrow: the frames themselves) */
        const int fd = open(path, O_RDONLY);
        int16_t *buf;
        uint32_t got, i;
        if (fd < 0)
            return 0;
        if (wav_info(fd, &w) || !(buf = malloc((size_t)count * 4u))) {
            close(fd);
            return 0;
        }
        got = wav_read(fd, &w, from, (uint32_t)count, buf);
        for (i = 0; i < got; i++) {
            const int cc = (int)((uint64_t)i * (uint64_t)n / count);
            const int a = absv(buf[2 * i]) > absv(buf[2 * i + 1]) ? absv(buf[2 * i]) : absv(buf[2 * i + 1]);
            const uint8_t p = (uint8_t)pk8(a);
            if (p > out[cc])
                out[cc] = p;
        }
        free(buf);
        close(fd);
    }
    scaled(t, out, n);
    return 1;
}

/* ------------------------------------------------------------------ rows --- */
static void today(const tl_t *l, int *y, int *m, int *d)
{
    struct tm tm;
    const time_t t = l->now ? l->now() : time(NULL);
    localtime_r(&t, &tm);
    *y = tm.tm_year + 1900, *m = tm.tm_mon + 1, *d = tm.tm_mday;
}
static void day_row(const tl_t *l, const char *name, tl_row_t *r)
{
    char (*t)[TL_REL];
    int y, m, d;
    memset(r, 0, sizeof *r);
    r->kind = TL_DAY, r->id = fnv(name), r->src = TL_SRC_NONE;
    snprintf(r->rel, sizeof r->rel, "%s", name);
    is_day(name, &r->year, &r->month, &r->day);
    today(l, &y, &m, &d);
    r->today = y == r->year && m == r->month && d == r->day;
    r->takes = takes_in(l, name, &t);
    free(t);
}
static void take_row_(tl_t *l, const char *day, const char *name, tl_row_t *r, int pic);
static void take_row(tl_t *l, const char *day, const char *name, tl_row_t *r) { take_row_(l, day, name, r, 1); }
static void take_row_(tl_t *l, const char *day, const char *name, tl_row_t *r, int pic)   /* pic 0: no picture */
{
    char p[1400];
    wav_t w;
    ptab_t *t;
    int c;
    memset(r, 0, sizeof *r);
    snprintf(r->rel, sizeof r->rel, "%s/%s", day, name);
    r->id = fnv(r->rel), r->folder = fnv(day);
    is_day(day, &r->year, &r->month, &r->day);
    r->kind = is_stems(l, r->rel) ? TL_STEMS : TL_TAKE;
    parse(l, name, r->kind == TL_STEMS, &r->src, &r->bars, &r->bpm);
    if (r->kind == TL_STEMS)
        r->src = TL_SRC_STEMS;
    take_file(l, r->rel, p, sizeof p);
    {
        const int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            if (!wav_info(fd, &w))
                r->frames = w.frames, r->rate = w.rate;
            close(fd);
        }
    }
    if (pic && (t = ptab(l, p, NULL)) && t->frames) {
        for (c = 0; c < TL_THUMB; c++) {
            const uint64_t a = t->frames * (uint64_t)c / TL_THUMB, b = t->frames * (uint64_t)(c + 1) / TL_THUMB;
            uint64_t j;
            for (j = a / TL_STEP; j < (b + TL_STEP - 1) / TL_STEP && j < t->n; j++)
                r->thumb[c] = t->pk[j] > r->thumb[c] ? t->pk[j] : r->thumb[c];
        }
        scaled(t, r->thumb, TL_THUMB);
    }
}

static int rows_locked(tl_t *l, uint32_t folder, int from, int n, tl_row_t *rows, int *total, uint32_t *fout)
{
    char (*dv)[TL_REL] = NULL, (*tv)[TL_REL] = NULL;
    int nd = days(l, &dv), i, k = 0, d = -1, nt;
    *total = 0, *fout = 0;
    n = n > TL_ROWS ? TL_ROWS : n;
    if (!folder) {
        *total = nd;
        for (i = from; i >= 0 && i < nd && k < n; i++)
            day_row(l, dv[i], &rows[k++]);
        free(dv);
        return k;
    }
    if (folder == TL_ALL) {                             /* every take: day after day (newest first), each day's newest
                                                         * first; only the rows asked are read */
        int at = 0;
        *fout = TL_ALL;
        for (i = 0; i < nd; i++) {
            int j;
            nt = takes_in(l, dv[i], &tv);
            for (j = 0; j < nt; j++, at++)
                if (from >= 0 && at >= from && k < n)
                    take_row(l, dv[i], tv[j], &rows[k++]);
            free(tv);
            tv = NULL;
        }
        *total = at;
        free(dv);
        return k;
    }
    for (i = 0; i < nd && d < 0; i++)
        if (folder == TL_NEWEST || fnv(dv[i]) == folder)
            d = i;
    if (d < 0) {
        free(dv);
        return 0;
    }
    nt = takes_in(l, dv[d], &tv);
    *total = nt, *fout = fnv(dv[d]);
    for (i = from; i >= 0 && i < nt && k < n; i++)
        take_row(l, dv[d], tv[i], &rows[k++]);
    free(tv);
    free(dv);
    return k;
}
int tl_rows(tl_t *l, uint32_t folder, int from, int n, tl_row_t *rows, int *total, uint32_t *fout)
{
    int k;
    pthread_mutex_lock(&l->mu);
    k = rows_locked(l, folder, from, n, rows, total, fout);
    pthread_mutex_unlock(&l->mu);
    return k;
}

/* the take (or day) with that id: its row (no picture when pic is 0) */
static int find_locked(tl_t *l, uint32_t id, tl_row_t *row, int pic)
{
    char (*dv)[TL_REL] = NULL, (*tv)[TL_REL] = NULL;
    const int nd = days(l, &dv);
    int i, j, nt, got = 0;
    for (i = 0; i < nd && !got; i++) {
        if (fnv(dv[i]) == id) {
            day_row(l, dv[i], row);
            got = 1;
            break;
        }
        nt = takes_in(l, dv[i], &tv);
        for (j = 0; j < nt && !got; j++) {
            char rel[TL_REL * 2];
            snprintf(rel, sizeof rel, "%s/%s", dv[i], tv[j]);
            if (fnv(rel) == id) {
                if (pic)
                    take_row(l, dv[i], tv[j], row);
                else {
                    memset(row, 0, sizeof *row);
                    snprintf(row->rel, sizeof row->rel, "%s", rel);
                    row->id = id, row->kind = is_stems(l, rel) ? TL_STEMS : TL_TAKE;
                }
                got = 1;
            }
        }
        free(tv);
        tv = NULL;
    }
    free(dv);
    return got;
}
int tl_find(tl_t *l, uint32_t id, tl_row_t *row)
{
    int r;
    pthread_mutex_lock(&l->mu);
    r = find_locked(l, id, row, 1);
    pthread_mutex_unlock(&l->mu);
    return r;
}
int tl_row_of(tl_t *l, const char *path, tl_row_t *row)
{
    const size_t n = strlen(l->dir);
    char rel[TL_REL], day[16], *s;
    int r = 0, y, m, d;
    memset(row, 0, sizeof *row);
    if (strncmp(path, l->dir, n) || path[n] != '/')
        return 0;
    snprintf(rel, sizeof rel, "%s", path + n + 1);
    s = strchr(rel, '/');
    if (!s || s - rel != 10)
        return 0;
    if (strchr(s + 1, '/'))                             /* (a stems take's file: its folder) */
        *strrchr(rel, '/') = 0;
    memcpy(day, rel, 10), day[10] = 0;
    if (!is_day(day, &y, &m, &d))
        return 0;
    pthread_mutex_lock(&l->mu);
    take_row(l, day, rel + 11, row);
    r = row->frames > 0;
    pthread_mutex_unlock(&l->mu);
    return r;
}
int tl_peaks(tl_t *l, uint32_t id, uint64_t from, uint64_t count, int n, uint8_t *out)
{
    tl_row_t r;
    char p[1400];
    int ok = 0;
    pthread_mutex_lock(&l->mu);
    if (find_locked(l, id, &r, 0) && r.kind != TL_DAY) {
        take_file(l, r.rel, p, sizeof p);
        ok = peaks_of(l, p, from, count, n > TL_WPEAKS ? TL_WPEAKS : n, out);
    }
    pthread_mutex_unlock(&l->mu);
    return ok;
}

/* ------------------------------------------------------------------ the trash, the undo --- */
static void rm_tree(const char *p)   /* a file, or a folder of files (a stems take); a link: only the link */
{
    DIR *d;
    struct dirent *e;
    struct stat sb;
    if (lstat(p, &sb))
        return;
    if (!S_ISDIR(sb.st_mode)) {
        unlink(p);
        return;
    }
    if ((d = opendir(p))) {
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                char c[1700];
                snprintf(c, sizeof c, "%s/%s", p, e->d_name);
                rm_tree(c);
            }
        closedir(d);
    }
    rmdir(p);
}
static void undo_save(tl_t *l)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof p, "%s/.trash/undo.txt", l->dir);
    if (!l->undo_op) {
        unlink(p);
        return;
    }
    if ((f = fopen(p, "w"))) {
        fprintf(f, "%d\n%s\n", l->undo_op, l->undo_rel);
        fclose(f);
    }
}
static void undo_drop(tl_t *l)   /* the undo there was: gone for good */
{
    if (l->undo_op && l->undo_trash[0])
        rm_tree(l->undo_trash);
    l->undo_op = 0, l->undo_rel[0] = l->undo_trash[0] = 0;
    ST(&l->has_undo, 0);
    undo_save(l);
}
static void trash_path(const tl_t *l, const char *rel, char *p, size_t cap)
{
    char flat[TL_REL];
    size_t i;
    snprintf(flat, sizeof flat, "%s", rel);
    for (i = 0; flat[i]; i++)
        flat[i] = flat[i] == '/' ? '_' : flat[i];
    if (!flat[0])                                       /* (never "", ".", "..": always a name inside the trash) */
        snprintf(flat, sizeof flat, "_");
    else if (flat[0] == '.')
        flat[0] = '_';
    snprintf(p, cap, "%s/.trash/%s", l->dir, flat);
}
static void undo_load(tl_t *l)
{
    char p[1100], line[TL_REL + 8];
    FILE *f;
    int op = 0;
    snprintf(p, sizeof p, "%s/.trash/undo.txt", l->dir);
    if (!(f = fopen(p, "r")))
        return;
    if (fgets(line, sizeof line, f))
        op = atoi(line);
    if (op >= TL_OP_DELETE && op <= TL_OP_FADE_OUT && fgets(line, sizeof line, f)) {
        char day[11];
        int y, m, d;
        line[strcspn(line, "\n")] = 0;
        snprintf(day, sizeof day, "%s", line);
        /* (only a take's own path, "<YYYY-MM-DD>/<name>": the undo never puts a file anywhere else) */
        if (strlen(line) > 11 && line[10] == '/' && is_day(day, &y, &m, &d) && line[11] != '.' &&
            !strchr(line + 11, '/')) {
            snprintf(l->undo_rel, sizeof l->undo_rel, "%s", line);
            trash_path(l, l->undo_rel, l->undo_trash, sizeof l->undo_trash);
            if (!access(l->undo_trash, F_OK))
                l->undo_op = op, ST(&l->has_undo, 1);
            else
                l->undo_rel[0] = l->undo_trash[0] = 0;
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ edits --- */
/* the take's frames, changed by op, into tmp (stereo int16 only): 0 done */
static int rewrite(const char *src, const char *tmp, int op, uint64_t from, uint64_t count, int ms, uint64_t *nframes)
{
    wav_t w;
    int fd = open(src, O_RDONLY), ok = 0;
    FILE *o = NULL;
    int16_t *buf = NULL;
    uint64_t a, b, k, N = 0, peak = 0, n;
    uint8_t h[44];
    if (fd < 0)
        return -1;
    if (wav_info(fd, &w) || w.ch != 2 || !(buf = malloc(CHUNK * 4u)) || !(o = fopen(tmp, "wb")))
        goto out;
    a = 0, b = w.frames;
    if (op == TL_OP_TRIM) {
        if (from >= w.frames || !count)
            goto out;
        a = from, b = from + count > w.frames ? w.frames : from + count;
        N = TL_CUT_FADE < (b - a) / 2 ? TL_CUT_FADE : (b - a) / 2;
    } else if (op == TL_OP_FADE_IN || op == TL_OP_FADE_OUT) {
        N = (uint64_t)(ms < 0 ? 0 : ms) * w.rate / 1000u;
        N = N > w.frames ? w.frames : N;
    } else if (op == TL_OP_NORMALIZE) {                 /* (the peak first) */
        for (k = 0; k < w.frames;) {
            const uint32_t got = wav_read(fd, &w, k, CHUNK, buf);
            uint32_t i;
            if (!got)
                goto out;
            for (i = 0; i < 2 * got; i++)
                peak = (uint64_t)absv(buf[i]) > peak ? (uint64_t)absv(buf[i]) : peak;
            k += got;
        }
    }
    n = b - a;
    wav_header(h, w.rate, n);
    if (fwrite(h, 1, 44, o) != 44)
        goto out;
    for (k = 0; k < n;) {
        const uint32_t want = n - k < CHUNK ? (uint32_t)(n - k) : CHUNK;
        uint32_t got, i;
        if (op == TL_OP_REVERSE) {                      /* (the frames from the end, each chunk turned) */
            got = wav_read(fd, &w, n - k - want, want, buf);
            if (got != want)
                goto out;
            for (i = 0; i < got / 2; i++) {
                const int16_t l0 = buf[2 * i], r0 = buf[2 * i + 1];
                buf[2 * i] = buf[2 * (got - 1 - i)], buf[2 * i + 1] = buf[2 * (got - 1 - i) + 1];
                buf[2 * (got - 1 - i)] = l0, buf[2 * (got - 1 - i) + 1] = r0;
            }
        } else if ((got = wav_read(fd, &w, a + k, want, buf)) != want)
            goto out;
        for (i = 0; i < got; i++) {
            const uint64_t f = k + i;                   /* (the frame in the new file) */
            int c;
            for (c = 0; c < 2; c++) {
                int64_t s = buf[2 * i + c];
                if (op == TL_OP_TRIM) {
                    if (a > 0 && f < N)
                        s = s * (int64_t)f / (int64_t)N;
                    if (b < w.frames && f >= n - N)
                        s = s * (int64_t)(n - 1 - f) / (int64_t)N;
                } else if (op == TL_OP_FADE_IN && f < N)
                    s = s * (int64_t)f / (int64_t)N;
                else if (op == TL_OP_FADE_OUT && f >= n - N)
                    s = s * (int64_t)(n - 1 - f) / (int64_t)N;
                else if (op == TL_OP_NORMALIZE && peak) {
                    const int64_t v = s * TL_NORM_PEAK, p2 = (int64_t)peak / 2;
                    s = v >= 0 ? (v + p2) / (int64_t)peak : -((-v + p2) / (int64_t)peak);
                    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
                }
                buf[2 * i + c] = (int16_t)s;
            }
        }
        if (fwrite(buf, 4, got, o) != got)
            goto out;
        k += got;
    }
    if (fflush(o) || fsync(fileno(o)))
        goto out;
    *nframes = n;
    ok = 1;
out:
    if (o && fclose(o))
        ok = 0;
    if (!ok)
        unlink(tmp);
    free(buf);
    close(fd);
    return ok ? 0 : -1;
}

/* move src aside as the undo's copy (a hard link: src stays until the rename over it) */
static int keep_aside(const char *src, const char *dst, int link_it)
{
    unlink(dst);
    if (link_it && !link(src, dst))
        return 0;
    return rename(src, dst);
}

static int edit_locked(tl_t *l, uint32_t id, int op, uint64_t from, uint64_t count, int ms, tl_row_t *after)
{
    tl_row_t r;
    char p[1400], tmp[1500], tp[1400], dir[1100];
    uint64_t nf = 0;
    memset(after, 0, sizeof *after);
    if (!find_locked(l, id, &r, 0) || (r.kind != TL_TAKE && r.kind != TL_STEMS))
        return 0;                                       /* (a day is never deleted or edited: only a take) */
    snprintf(dir, sizeof dir, "%s/.trash", l->dir);
    mkdir(dir, 0755);
    trash_path(l, r.rel, tp, sizeof tp);
    snprintf(p, sizeof p, "%s/%s", l->dir, r.rel);
    if (op == TL_OP_DELETE) {                           /* into the trash (the undo there was: gone) */
        undo_drop(l);
        if (keep_aside(p, tp, 0))
            return 0;
        ptab_forget(l, p);
    } else {
        char *slash;
        if (r.kind != TL_TAKE || op < TL_OP_TRIM || op > TL_OP_FADE_OUT)
            return 0;                                   /* (a stems take: no edits in this stage) */
        snprintf(tmp, sizeof tmp, "%s", p);
        slash = strrchr(tmp, '/');
        snprintf(slash + 1, sizeof tmp - (size_t)(slash + 1 - tmp), ".%s.tmp", strrchr(p, '/') + 1);
        if (rewrite(p, tmp, op, from, count, ms, &nf))
            return 0;
        undo_drop(l);
        if (keep_aside(p, tp, 1) || rename(tmp, p)) {   /* (the file before it: the undo; then the new one over it) */
            unlink(tmp);
            if (access(p, F_OK))
                rename(tp, p);
            return 0;
        }
        ptab_forget(l, p);
    }
    l->undo_op = op;
    snprintf(l->undo_rel, sizeof l->undo_rel, "%s", r.rel);
    snprintf(l->undo_trash, sizeof l->undo_trash, "%s", tp);
    ST(&l->has_undo, 1);
    undo_save(l);
    if (op != TL_OP_DELETE)
        find_locked(l, id, after, 1);
    return 1;
}
int tl_edit(tl_t *l, uint32_t id, int op, uint64_t from, uint64_t count, int ms, tl_row_t *after)
{
    int r;
    pthread_mutex_lock(&l->mu);
    r = edit_locked(l, id, op, from, count, ms, after);
    pthread_mutex_unlock(&l->mu);
    return r;
}
int tl_undo(tl_t *l, tl_row_t *after)
{
    char p[1400];
    int ok = 0;
    memset(after, 0, sizeof *after);
    pthread_mutex_lock(&l->mu);
    if (l->undo_op) {
        snprintf(p, sizeof p, "%s/%s", l->dir, l->undo_rel);
        if (l->undo_op == TL_OP_DELETE && !access(p, F_OK))
            ok = 0;                                     /* (a take came there meanwhile: never over it) */
        else if (!rename(l->undo_trash, p)) {           /* (an edit: the file before it, over the edited) */
            ptab_forget(l, p);
            ok = 1;
            find_locked(l, fnv(l->undo_rel), after, 1);
            l->undo_op = 0, l->undo_rel[0] = l->undo_trash[0] = 0;
            ST(&l->has_undo, 0);
            undo_save(l);
        }
    }
    pthread_mutex_unlock(&l->mu);
    return ok;
}
int tl_has_undo(tl_t *l) { return LD(&l->has_undo); }
void tl_set_labels(tl_t *l, const char *const labels[3])
{
    int k;
    pthread_mutex_lock(&l->mu);
    for (k = 0; k < 3; k++) {   /* (as the recorder's file names have them: letters, digits, " _-.,()+", others _) */
        const char *s = labels && labels[k] ? labels[k] : "";
        size_t i = 0;
        int us = 0;
        for (; *s && i + 1 < sizeof l->labels[k] && i < 40; s++) {
            const unsigned char c = (unsigned char)*s;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" _-.,()+", c))
                l->labels[k][i++] = (char)c, us = 0;
            else if (!us)
                l->labels[k][i++] = '_', us = 1;
        }
        l->labels[k][i] = 0;
    }
    pthread_mutex_unlock(&l->mu);
}

/* ------------------------------------------------------------------ the page's: rename, the list, the files --- */
static int bad_name(const char *s)
{
    const unsigned char *p;
    int seen = 0;
    if (!s || !*s || *s == '.' || strlen(s) > TL_NAME_MAX)
        return 1;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '/' || *p == '\\')
            return 1;
        seen |= *p != ' ';
    }
    return !seen;
}
static int rename_locked(tl_t *l, uint32_t id, const char *name, tl_row_t *after)
{
    tl_row_t r;
    char nm[TL_REL], rel[TL_REL + 16], from[1400], to[1400], day[11];
    const char *base;
    struct stat sb;
    memset(after, 0, sizeof *after);
    if (bad_name(name))
        return -1;
    if (!find_locked(l, id, &r, 0) || r.kind != TL_TAKE || !(base = strrchr(r.rel, '/')))
        return 0;                                       /* (a stems take keeps its name: its files are named in it) */
    base++;
    snprintf(day, sizeof day, "%.10s", r.rel);
    if (is_head(name) && !strncmp(name, base, 12))     /* (its own head kept; another one's: a name after ours) */
        snprintf(nm, sizeof nm, "%s", name);
    else
        snprintf(nm, sizeof nm, "%.12s%s", base, name);
    if (!ends(nm, ".wav") && strlen(nm) + 4 < sizeof nm)
        strcat(nm, ".wav");
    if (!ends(nm, ".wav"))
        return -1;
    if (!strcmp(nm, base)) {                            /* (the same name: nothing to do) */
        take_row_(l, day, nm, after, 0);
        return 1;
    }
    snprintf(rel, sizeof rel, "%s/%s", day, nm);
    snprintf(from, sizeof from, "%s/%s", l->dir, r.rel);
    snprintf(to, sizeof to, "%s/%s", l->dir, rel);
    if (!lstat(to, &sb))
        return -2;
    if (rename(from, to))
        return 0;
    ptab_forget(l, from);
    if (l->undo_op && !strcmp(l->undo_rel, r.rel)) {    /* (the undo follows the take: its copy renamed too) */
        char tp[1400];
        trash_path(l, rel, tp, sizeof tp);
        if (!rename(l->undo_trash, tp)) {
            snprintf(l->undo_rel, sizeof l->undo_rel, "%s", rel);
            snprintf(l->undo_trash, sizeof l->undo_trash, "%s", tp);
            undo_save(l);
        } else
            undo_drop(l);
    }
    take_row_(l, day, nm, after, 0);
    return 1;
}
int tl_rename(tl_t *l, uint32_t id, const char *name, tl_row_t *after)
{
    int r;
    pthread_mutex_lock(&l->mu);
    r = rename_locked(l, id, name, after);
    pthread_mutex_unlock(&l->mu);
    return r;
}

static int jesc(const char *s, char *out, size_t cap)   /* a JSON string's inside */
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            out[k++] = '\\', out[k++] = (char)c;
        else if (c < 0x20)
            k += (size_t)snprintf(out + k, cap - k, "\\u%04x", c);
        else
            out[k++] = (char)c;
    }
    out[k] = 0;
    return (int)k;
}
int tl_row_json(const tl_row_t *r, char *out, int cap)
{
    char e[TL_REL * 2];
    const char *base = strrchr(r->rel, '/');
    int n;
    jesc(base ? base + 1 : r->rel, e, sizeof e);
    n = snprintf(out, (size_t)cap, "{\"id\":%u,\"kind\":\"%s\",\"src\":%d,\"name\":\"%s\",\"date\":\"%04d-%02d-%02d\","
                 "\"bars\":%u,\"bpm\":%.2f,\"frames\":%llu,\"rate\":%u}", r->id,
                 r->kind == TL_DAY ? "day" : r->kind == TL_STEMS ? "stems" : "take", r->src, e, r->year, r->month, r->day,
                 r->bars, r->bpm, (unsigned long long)r->frames, r->rate);
    return n < 0 ? 0 : n >= cap ? cap - 1 : n;
}
typedef struct { char *s; size_t n, cap; int bad; } sb_t;
static void sb_put(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_put(sb_t *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (b->bad)
        return;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k < 0) {
            b->bad = 1;
            return;
        }
        if ((size_t)k < b->cap - b->n) {
            b->n += (size_t)k;
            return;
        }
        {
            char *t = realloc(b->s, b->cap * 2 + (size_t)k);
            if (!t) {
                b->bad = 1;
                return;
            }
            b->s = t, b->cap = b->cap * 2 + (size_t)k;
        }
    }
}
char *tl_list_json(tl_t *l, uint32_t day)
{
    char (*dv)[TL_REL] = NULL, (*tv)[TL_REL] = NULL;
    char row[TL_REL * 2 + 256];
    sb_t b = { malloc(4096), 0, 4096, 0 };
    int nd, nt, i, d = -1;
    tl_row_t r;
    if (!b.s)
        return NULL;
    pthread_mutex_lock(&l->mu);
    nd = days(l, &dv);
    for (i = 0; i < nd && d < 0 && day && day != TL_NEWEST; i++)
        if (fnv(dv[i]) == day)
            d = i;
    if (d < 0 && nd)                                    /* (the newest; a day gone meanwhile: the newest too) */
        d = 0;
    sb_put(&b, "{\"day\":%u,\"undo\":%d,\"days\":[", d >= 0 ? fnv(dv[d]) : 0u, l->undo_op != 0);
    for (i = 0; i < nd; i++) {
        day_row(l, dv[i], &r);
        sb_put(&b, "%s{\"id\":%u,\"date\":\"%s\",\"today\":%d,\"takes\":%d}", i ? "," : "", r.id, dv[i], r.today, r.takes);
    }
    sb_put(&b, "],\"takes\":[");
    nt = d >= 0 ? takes_in(l, dv[d], &tv) : 0;
    for (i = 0; i < nt; i++) {
        take_row_(l, dv[d], tv[i], &r, 0);
        tl_row_json(&r, row, sizeof row);
        sb_put(&b, "%s%s", i ? "," : "", row);
    }
    sb_put(&b, "]}");
    pthread_mutex_unlock(&l->mu);
    free(tv);
    free(dv);
    if (b.bad) {
        free(b.s);
        return NULL;
    }
    return b.s;
}
int tl_file(tl_t *l, uint32_t id, char *path, size_t pcap, char *wav, size_t wcap, tl_row_t *row)
{
    tl_row_t r;
    int ok;
    pthread_mutex_lock(&l->mu);
    ok = find_locked(l, id, &r, 0) && r.kind != TL_DAY;
    if (ok) {
        snprintf(path, pcap, "%s/%s", l->dir, r.rel);
        take_file(l, r.rel, wav, wcap);
        if (row)
            *row = r;
    }
    pthread_mutex_unlock(&l->mu);
    return ok;
}

/* ------------------------------------------------------------------ the worker --- */
void tl_post(tl_t *l, const tl_req_t *q)
{
    int i;
    pthread_mutex_lock(&l->qmu);
    if (q->kind == TL_Q_PEAKS && !q->tag)               /* (a newer view: the older one not started is dropped; the
                                                         * page's are each answered) */
        for (i = 0; i < l->qn; i++)
            if (l->q[(l->qh + i) % QN].kind == TL_Q_PEAKS && !l->q[(l->qh + i) % QN].tag)
                l->q[(l->qh + i) % QN].kind = 0;
    if (l->qn == QN)
        l->qh = (l->qh + 1) % QN, l->qn--;
    l->q[(l->qh + l->qn) % QN] = *q;
    l->qn++;
    pthread_cond_signal(&l->qcv);
    pthread_mutex_unlock(&l->qmu);
}
int tl_poll(tl_t *l, tl_ans_t *a)
{
    int got = 0;
    pthread_mutex_lock(&l->amu);
    if (l->an) {
        *a = l->ans[l->ah];
        l->ah = (l->ah + 1) % AN, l->an--;
        got = 1;
    }
    pthread_mutex_unlock(&l->amu);
    return got;
}
static void answer(tl_t *l, const tl_ans_t *a)
{
    pthread_mutex_lock(&l->amu);
    if (l->an == AN) {
        free(l->ans[l->ah].text);                       /* (an answer never polled: its text with it) */
        l->ah = (l->ah + 1) % AN, l->an--;
    }
    l->ans[(l->ah + l->an) % AN] = *a;
    l->an++;
    pthread_mutex_unlock(&l->amu);
}
static void *worker(void *arg)
{
    tl_t *l = arg;
    tl_ans_t *a = malloc(sizeof *a);
    tl_req_t q;
    if (!a)
        return NULL;
    for (;;) {
        pthread_mutex_lock(&l->qmu);
        while (!l->qn && !l->quit)
            pthread_cond_wait(&l->qcv, &l->qmu);
        if (l->quit) {
            pthread_mutex_unlock(&l->qmu);
            break;
        }
        q = l->q[l->qh];
        l->qh = (l->qh + 1) % QN, l->qn--;
        pthread_mutex_unlock(&l->qmu);
        memset(a, 0, sizeof *a);
        a->kind = q.kind, a->tag = q.tag;
        switch (q.kind) {
        case TL_Q_LIST:
            a->asked = q.take;
            a->text = tl_list_json(l, q.take);
            a->ok = a->text != NULL;
            break;
        case TL_Q_FILE: {
            char p[1400], w[1400];
            a->asked = q.take;
            a->ok = tl_file(l, q.take, p, sizeof p, w, sizeof w, &a->row);
            if (a->ok && (a->text = malloc(strlen(p) + strlen(w) + 2)))
                snprintf(a->text, strlen(p) + strlen(w) + 2, "%s\n%s", p, w);
            a->ok = a->ok && a->text;
            a->has_row = a->ok;
            break;
        }
        case TL_Q_RENAME:
            a->asked = q.take;
            q.path[sizeof q.path - 1] = 0;
            a->ok = tl_rename(l, q.take, q.path, &a->row);
            a->has_row = a->ok == 1;
            break;
        case TL_Q_ROWS:
            a->asked = q.take, a->from = (int)q.from;
            a->n = tl_rows(l, q.take, (int)q.from, q.n, a->rows, &a->total, &a->folder);
            if (!a->folder && q.take && q.take != TL_NEWEST)
                a->folder = q.take;                     /* (a day no longer there: answered empty) */
            break;
        case TL_Q_PEAKS:
            a->asked = q.take, a->pfrom = q.from, a->pcount = q.count;
            a->n = q.n > TL_WPEAKS ? TL_WPEAKS : q.n;
            if (!tl_peaks(l, q.take, q.from, q.count, a->n, a->pk))
                a->n = 0;
            break;
        case TL_Q_EDIT:
            a->op = q.op, a->asked = q.take;
            a->ok = tl_edit(l, q.take, q.op, q.from, q.count, q.value, &a->row);
            a->has_row = a->ok && a->row.id;
            break;
        case TL_Q_UNDO:
            a->op = TL_OP_UNDO;
            a->ok = tl_undo(l, &a->row);
            a->has_row = a->ok && a->row.id;
            break;
        case TL_Q_SAVED:
            a->mode = q.mode;
            a->has_row = tl_row_of(l, q.path, &a->row);
            break;
        default: continue;                              /* (dropped) */
        }
        answer(l, a);
    }
    free(a);
    return NULL;
}

/* ------------------------------------------------------------------ playback --- */
void tl_play(tl_t *l, uint32_t id, uint64_t from, uint64_t count)
{
    pthread_mutex_lock(&l->pmu);
    l->req_gen++, l->req_id = id, l->req_from = from, l->req_count = count;
    pthread_cond_signal(&l->pcv);
    pthread_mutex_unlock(&l->pmu);
}
void tl_stop(tl_t *l) { tl_play(l, 0, 0, 0); }

void tl_mix(tl_t *l, int16_t *out, uint32_t frames)
{
    const uint32_t want = LD(&l->want_gen);
    uint32_t w, avail, k, i;
    if (want != l->cur_gen) {                           /* (the player's new take: the ring emptied, from its frame) */
        ST(&l->rd, LD(&l->wr));
        l->cur_gen = want;
        l->cur_id = LD(&l->n_id);
        l->cur_pos = LD(&l->n_from);
        ST(&l->pub_pos, l->cur_pos);
        ST(&l->pub_id, l->cur_id);
        ST(&l->ack_gen, want);
    }
    if (!l->cur_id)
        return;
    w = LD(&l->wr);
    avail = w - l->rd;
    k = avail < frames ? avail : frames;
    for (i = 0; i < k; i++) {
        const uint32_t at = (l->rd + i) % RING;
        int c;
        for (c = 0; c < 2; c++) {
            const int v = out[2 * i + c] + l->ring[2 * at + c];
            out[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    ST(&l->rd, l->rd + k);
    l->cur_pos += k;
    ST(&l->pub_pos, l->cur_pos);
    if (k < frames && LD(&l->eof_gen) == l->cur_gen && LD(&l->wr) == l->rd) {
        l->cur_id = 0;                                  /* (all of it played) */
        ST(&l->pub_id, 0);
    }
}
void tl_playing(tl_t *l, uint32_t *id, uint64_t *pos)
{
    *id = LD(&l->pub_id);
    *pos = LD(&l->pub_pos);
}

static void *player(void *arg)
{
    tl_t *l = arg;
    uint32_t seen = 0, gen = 0;
    int fd = -1;
    wav_t w;
    uint64_t at = 0, end = 0;
    int16_t *buf = malloc(PCHUNK * 4u);
    if (!buf)
        return NULL;
    memset(&w, 0, sizeof w);
    for (;;) {
        uint32_t g, id;
        uint64_t from, count;
        pthread_mutex_lock(&l->pmu);
        if (l->req_gen == seen && !l->p_quit) {         /* (nothing new: wait, a few ms while a take streams) */
            if (fd < 0)
                pthread_cond_wait(&l->pcv, &l->pmu);
            else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 4000000;
                if (ts.tv_nsec >= 1000000000)
                    ts.tv_sec++, ts.tv_nsec -= 1000000000;
                pthread_cond_timedwait(&l->pcv, &l->pmu, &ts);
            }
        }
        if (l->p_quit) {
            pthread_mutex_unlock(&l->pmu);
            break;
        }
        g = l->req_gen, id = l->req_id, from = l->req_from, count = l->req_count;
        pthread_mutex_unlock(&l->pmu);
        if (g != seen) {                                /* a new play (or a stop) */
            tl_row_t r;
            char p[1400];
            seen = g;
            if (fd >= 0)
                close(fd), fd = -1;
            if (id) {
                pthread_mutex_lock(&l->mu);
                if (find_locked(l, id, &r, 0) && r.kind != TL_DAY)
                    take_file(l, r.rel, p, sizeof p);
                else
                    p[0] = 0;
                pthread_mutex_unlock(&l->mu);
                if (p[0] && (fd = open(p, O_RDONLY)) >= 0 && wav_info(fd, &w))
                    close(fd), fd = -1;
            }
            at = from, end = fd >= 0 ? (count && from + count < w.frames ? from + count : w.frames) : 0;
            if (fd >= 0 && at >= end)
                close(fd), fd = -1;
            gen++;
            ST(&l->n_id, fd >= 0 ? id : 0u);
            ST(&l->n_from, at);
            ST(&l->want_gen, gen);
            while (LD(&l->ack_gen) != gen) {            /* (the engine drops the ring at its next block) */
                pthread_mutex_lock(&l->pmu);
                const int again = l->req_gen != seen || l->p_quit;
                pthread_mutex_unlock(&l->pmu);
                if (again)
                    break;
                usleep(1000);
            }
            if (LD(&l->ack_gen) != gen)
                continue;
        }
        while (fd >= 0) {                               /* the ring filled from the file */
            const uint32_t used = LD(&l->wr) - LD(&l->rd), room = RING - used;
            uint32_t n = room < PCHUNK ? room : PCHUNK, got, i;
            if (n > end - at)
                n = (uint32_t)(end - at);
            if (!n)
                break;
            got = wav_read(fd, &w, at, n, buf);
            for (i = 0; i < got; i++) {
                const uint32_t pos = (l->wr + i) % RING;
                l->ring[2 * pos] = buf[2 * i], l->ring[2 * pos + 1] = buf[2 * i + 1];
            }
            ST(&l->wr, l->wr + got);
            at += got;
            if (!got || at >= end) {
                ST(&l->eof_gen, gen);
                close(fd), fd = -1;
            }
        }
    }
    if (fd >= 0)
        close(fd);
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ life --- */
tl_t *tl_new(const char *dir, time_t (*now)(void))
{
    tl_t *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    snprintf(l->dir, sizeof l->dir, "%s", dir);
    l->now = now;
    pthread_mutex_init(&l->mu, NULL);
    pthread_mutex_init(&l->qmu, NULL);
    pthread_cond_init(&l->qcv, NULL);
    pthread_mutex_init(&l->amu, NULL);
    pthread_mutex_init(&l->pmu, NULL);
    pthread_cond_init(&l->pcv, NULL);
    if (!(l->ans = calloc(AN, sizeof *l->ans)) || !(l->ring = calloc(RING, 4))) {
        tl_free(l);
        return NULL;
    }
    undo_load(l);
    l->w_on = !pthread_create(&l->wth, NULL, worker, l);
    l->p_on = !pthread_create(&l->pth, NULL, player, l);
    if (!l->w_on || !l->p_on) {
        tl_free(l);
        return NULL;
    }
    return l;
}
void tl_free(tl_t *l)
{
    int k;
    if (!l)
        return;
    if (l->w_on) {
        pthread_mutex_lock(&l->qmu);
        l->quit = 1;
        pthread_cond_signal(&l->qcv);
        pthread_mutex_unlock(&l->qmu);
        pthread_join(l->wth, NULL);
    }
    if (l->p_on) {
        pthread_mutex_lock(&l->pmu);
        l->p_quit = 1;
        pthread_cond_signal(&l->pcv);
        pthread_mutex_unlock(&l->pmu);
        pthread_join(l->pth, NULL);
    }
    for (k = 0; k < NCACHE; k++)
        free(l->cache[k].pk);
    for (k = 0; l->ans && k < l->an; k++)
        free(l->ans[(l->ah + k) % AN].text);
    free(l->ans);
    free(l->ring);
    pthread_mutex_destroy(&l->mu);
    pthread_mutex_destroy(&l->qmu);
    pthread_cond_destroy(&l->qcv);
    pthread_mutex_destroy(&l->amu);
    pthread_mutex_destroy(&l->pmu);
    pthread_cond_destroy(&l->pcv);
    free(l);
}
