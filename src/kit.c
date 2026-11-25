/* SPDX-License-Identifier: MPL-2.0 */
/* kit.h: the kit's sounds and voices */
#include "kit.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "takes.h"

#define NV 32                     /* voice slots: KIT_VOICES sounding, the rest fading out */
#define NEV 256                   /* notes waiting for their frame */
#define NREQ 64                   /* file requests waiting */
#define NANS 32                   /* answers waiting */
#define NGARB 64                  /* sounds to free on the file thread */
#define NIDS 2048                 /* Samples' rows seen: id -> path */
#define FADE_MS 5                 /* a choke, a pad's own voice taken back, a mute */
#define DECLICK_MS 2              /* a stolen voice, a hard end */

typedef struct {                  /* a sound in memory */
    int16_t *pcm;                 /* stereo, interleaved */
    uint32_t frames, rate;
} snd_t;
typedef struct {
    int on;
    int pad, semis;
    uint32_t age;
    double pos, inc;              /* source frames: where, a frame of output */
    int dir;                      /* +1, -1 (reverse) */
    double s, e;                  /* the region [s, e) in source frames */
    int mode, gate;
    int stage;                    /* ST_* */
    float env;                    /* 0..1 */
    float a_step;                 /* attack: per frame */
    uint32_t hold_n, t;           /* hold: frames; t: frames in the stage */
    uint32_t rel_n;               /* release: frames */
    float r_from;
    uint32_t fade_n;              /* a fade out: its frames (ST_FADE) */
    float gl, gr;
    const snd_t *snd;
} voice_t;
enum { ST_ATTACK, ST_HOLD, ST_SUSTAIN, ST_RELEASE, ST_FADE };
typedef struct { int pad, semis, vel; uint32_t frame; } ev_t;
enum { RQ_LOAD = 1, RQ_LOAD_ROW, RQ_ROWS, RQ_PREVIEW };
typedef struct {
    int kind, pad, place, n, from;
    uint32_t id, gen;
    char path[KIT_PATH];
} req_t;
typedef struct {                  /* the file thread's answer, and the sound it made (the engine's thread puts it on) */
    kit_ans_t a;
    int16_t *pcm;                 /* LOADED OK, PREVIEW: the sound (the engine's from then) */
    int preview;
    uint32_t gen;
} ans_t;

struct kit {
    kit_cfg_t cfg;
    char samples[KIT_PATH];
    /* the engine's */
    snd_t snd[KIT_PADS];
    int16_t v[KIT_PADS][KIT_P_N];
    voice_t vo[NV];
    uint32_t age;
    ev_t ev[NEV];
    int nev;
    snd_t prev;                   /* the preview */
    double prev_pos, prev_inc;
    int prev_on;
    uint32_t prev_gen;            /* the latest preview asked (an older answer: dropped) */
    uint32_t used[KIT_PADS];      /* (mu) each pad's bytes, as the file thread counts them */
    /* the requests (mu) */
    pthread_mutex_t mu;
    req_t rq[NREQ];
    int rqh, rqn;
    ans_t *an;
    int anh, ann;
    int16_t *garb[NGARB];
    int ngarb;
    int posted;
    struct timespec posted_at;
    struct { uint32_t id; char rel[256]; } ids[NIDS];
    int idn;
    /* Samples' last listing (the file thread's) */
    uint32_t ls_folder;
    int ls_ok, ls_n;
    struct { char name[256]; int dir; } *ls;
    /* the own thread */
    pthread_t th;
    pthread_cond_t cv;
    int th_on, quit;
};

/* ================================================================ small things === */
static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return !*a && !*b;
}
static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d ? d + 1 : "";
}
static int sound_file(const char *n)
{
    const char *e = ext_of(n);
    return lower_eq(e, "wav") || lower_eq(e, "aif") || lower_eq(e, "aiff") || lower_eq(e, "aifc");
}
static void base_name(const char *path, char *o, int cap)   /* the file's name, no extension, printable */
{
    const char *b = strrchr(path, '/');
    int i, n;
    snprintf(o, (size_t)cap, "%s", b ? b + 1 : path);
    if (strrchr(o, '.') && strrchr(o, '.') != o)
        *strrchr(o, '.') = 0;
    n = (int)strlen(o);
    for (i = 0; i < n; i++)
        if ((unsigned char)o[i] < 32 || (unsigned char)o[i] > 126)
            o[i] = '?';
}

/* ================================================================ reading a sound === */
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static double ext80(const uint8_t *p)   /* an AIFF's 80-bit IEEE extended sample rate */
{
    const int e = (int)((p[0] & 0x7F) << 8 | p[1]) - 16383 - 63;
    uint64_t m = 0;
    int i;
    for (i = 0; i < 8; i++)
        m = m << 8 | p[2 + i];
    return ldexp((double)m, e) * (p[0] & 0x80 ? -1 : 1);
}
typedef struct {
    long data;                    /* the sample data's offset */
    uint32_t frames, rate, ch, bps;
    int fmt;                      /* 1 PCM little-endian, 3 float, 2 PCM big-endian (AIFF) */
} info_t;
/* the file's header: where its samples are and what they are. 0 ok, else why */
static const char *probe(FILE *f, info_t *in)
{
    uint8_t h[12], c[64];
    long size, at;
    memset(in, 0, sizeof *in);
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 12 || fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12)
        return "not a sound";
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint32_t fmt = 0, dlen = 0;
        int have_fmt = 0;
        for (at = 12; at + 8 <= size;) {
            uint32_t len;
            if (fseek(f, at, SEEK_SET) || fread(c, 1, 8, f) != 8)
                break;
            len = le32(c + 4);
            if (!memcmp(c, "fmt ", 4) && len >= 16) {
                if (fread(c + 8, 1, len >= 40 ? 40 : len, f) < 16)
                    break;
                fmt = le16(c + 8), in->ch = le16(c + 10), in->rate = le32(c + 12), in->bps = le16(c + 22) / 8;
                if (fmt == 0xFFFE && len >= 26)
                    fmt = le16(c + 8 + 24);
                have_fmt = 1;
            } else if (!memcmp(c, "data", 4)) {
                in->data = at + 8;
                dlen = (long)len <= size - (at + 8) ? len : (uint32_t)(size - (at + 8));
                break;
            }
            at += 8 + (long)len + (len & 1u);
        }
        if (!have_fmt || !in->data || !in->ch || !in->bps || !in->rate ||
            !((fmt == 1 && (in->bps == 2 || in->bps == 3 || in->bps == 4)) || (fmt == 3 && in->bps == 4)))
            return "not a PCM WAV";
        in->fmt = (int)fmt, in->frames = dlen / (in->bps * in->ch);
        return NULL;
    }
    if (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) {
        const int aifc = !memcmp(h + 8, "AIFC", 4);
        int have = 0, le = 0;
        uint32_t nf = 0;
        for (at = 12; at + 8 <= size;) {
            uint32_t len;
            if (fseek(f, at, SEEK_SET) || fread(c, 1, 8, f) != 8)
                break;
            len = be32(c + 4);
            if (!memcmp(c, "COMM", 4) && len >= 18) {
                if (fread(c + 8, 1, len >= 26 ? 26 : len, f) < 18)
                    break;
                in->ch = be16(c + 8), nf = be32(c + 10), in->bps = be16(c + 14) / 8;
                in->rate = (uint32_t)lrint(ext80(c + 16));
                if (aifc && len >= 22) {
                    if (!memcmp(c + 26, "sowt", 4))
                        le = 1;
                    else if (memcmp(c + 26, "NONE", 4))
                        return "a compressed AIFF";
                }
                have = 1;
            } else if (!memcmp(c, "SSND", 4) && len >= 8) {
                uint8_t o[8];
                if (fread(o, 1, 8, f) != 8)
                    break;
                in->data = at + 16 + (long)be32(o);
            }
            at += 8 + (long)len + (len & 1u);
        }
        if (!have || !in->data || !in->ch || !in->rate || !(in->bps == 2 || in->bps == 3 || in->bps == 4))
            return "not a PCM AIFF";
        in->fmt = le ? 1 : 2, in->frames = nf;
        if ((long)in->frames * in->bps * in->ch > size - in->data)
            in->frames = (uint32_t)((size - in->data) / (in->bps * in->ch));
        return NULL;
    }
    return "not a WAV or an AIFF";
}
static int16_t sample_of(const uint8_t *p, const info_t *in)
{
    int32_t v;
    if (in->fmt == 3) {
        float x;
        memcpy(&x, p, 4);
        x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
        return (int16_t)lrintf(x * 32767.0f);
    }
    if (in->fmt == 2) {                                 /* (big-endian: the high byte first) */
        v = (int8_t)p[0] * 256 + p[1];
        return (int16_t)v;
    }
    switch (in->bps) {
    case 2: return (int16_t)le16(p);
    case 3: return (int16_t)(uint16_t)((uint32_t)p[1] | (uint32_t)p[2] << 8);   /* (its top 16 bits) */
    default: return (int16_t)((int32_t)le32(p) >> 16);
    }
}
/* frames [from, from + n) as stereo int16 into out (mono: L = R) */
static int read_frames(FILE *f, const info_t *in, uint32_t from, uint32_t n, int16_t *out)
{
    uint8_t buf[4096];
    const uint32_t fb = in->bps * in->ch, per = (uint32_t)sizeof buf / fb;
    uint32_t done = 0;
    if (fseek(f, in->data + (long)from * fb, SEEK_SET))
        return -1;
    while (done < n) {
        const uint32_t k = n - done < per ? n - done : per;
        uint32_t i;
        if (fread(buf, fb, k, f) != k)
            return -1;
        for (i = 0; i < k; i++) {
            const uint8_t *p = buf + i * fb;
            out[2 * (done + i)] = sample_of(p, in);
            out[2 * (done + i) + 1] = in->ch > 1 ? sample_of(p + in->bps, in) : out[2 * (done + i)];
        }
        done += k;
    }
    return 0;
}
/* a whole sound into memory (max bytes at most: else *status KIT_TOO_LONG; limit: its first frames only, 0 all); NULL
 * with *status */
static int16_t *load_sound(const char *path, uint64_t max, uint32_t limit, uint32_t *frames, uint32_t *rate, int *status)
{
    FILE *f = fopen(path, "rb");
    info_t in;
    int16_t *pcm;
    *frames = 0, *rate = 0;
    if (!f) {
        *status = KIT_MISSING;
        return NULL;
    }
    if (probe(f, &in) || !in.frames) {
        fclose(f);
        *status = KIT_FAILED;
        return NULL;
    }
    if (limit && in.frames > limit)
        in.frames = limit;
    if ((uint64_t)in.frames * 4u > max) {
        fclose(f);
        *status = KIT_TOO_LONG;
        return NULL;
    }
    if (!(pcm = malloc((size_t)in.frames * 4u)) || read_frames(f, &in, 0, in.frames, pcm)) {
        free(pcm);
        fclose(f);
        *status = KIT_FAILED;
        return NULL;
    }
    fclose(f);
    *frames = in.frames, *rate = in.rate, *status = KIT_OK;
    return pcm;
}
/* 64 peaks of a sound, scaled to its own peak (a quiet sound shows its shape) */
static int peaks_of(const int16_t *pcm, uint32_t frames, int n, uint8_t *pk)
{
    int c, top = 1, raw[KIT_PEAKS];
    for (c = 0; c < n; c++) {
        const uint32_t a = (uint32_t)((uint64_t)frames * (uint32_t)c / (uint32_t)n);
        uint32_t b = (uint32_t)((uint64_t)frames * (uint32_t)(c + 1) / (uint32_t)n), i;
        int p = 0;
        if (b <= a)
            b = a + 1;
        for (i = a; i < b && i < frames; i++) {
            const int l = abs(pcm[2 * i]), r = abs(pcm[2 * i + 1]);
            p = l > p ? l : p, p = r > p ? r : p;
        }
        raw[c] = p, top = p > top ? p : top;
    }
    for (c = 0; c < n; c++)
        pk[c] = (uint8_t)(raw[c] * 255 / top);
    return n;
}
/* a file row's picture: KIT_THUMB short windows, not the whole file */
static void thumb_of(const char *path, kit_row_t *r)
{
    FILE *f = fopen(path, "rb");
    info_t in;
    int16_t w[2 * 256];
    int c, raw[KIT_THUMB], top = 1;
    memset(r->thumb, 0, sizeof r->thumb);
    if (!f)
        return;
    if (!probe(f, &in) && in.frames) {
        r->frames = in.frames, r->rate = in.rate;
        for (c = 0; c < KIT_THUMB; c++) {
            const uint32_t at = (uint32_t)((uint64_t)in.frames * (uint32_t)c / KIT_THUMB);
            const uint32_t n = in.frames - at < 256 ? in.frames - at : 256;
            uint32_t i;
            int p = 0;
            if (n && !read_frames(f, &in, at, n, w))
                for (i = 0; i < 2 * n; i++)
                    p = abs(w[i]) > p ? abs(w[i]) : p;
            raw[c] = p, top = p > top ? p : top;
        }
        for (c = 0; c < KIT_THUMB; c++)
            r->thumb[c] = (uint8_t)(raw[c] * 255 / top);
    }
    fclose(f);
}

/* ================================================================ the requests (any thread) === */
static void wake(kit_t *k)   /* (mu held) the file thread called for, once */
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (k->posted && now.tv_sec - k->posted_at.tv_sec < 2)
        return;                                         /* (posted, not run yet; a post lost to a full queue: again) */
    k->posted = 1, k->posted_at = now;
    if (k->th_on)
        pthread_cond_signal(&k->cv);
    else if (k->cfg.tl) {
        tl_req_t q;
        memset(&q, 0, sizeof q);
        q.kind = TL_Q_JOB, q.job = kit_work, q.arg = k;
        tl_post(k->cfg.tl, &q);
    }
}
static void request(kit_t *k, const req_t *r)
{
    pthread_mutex_lock(&k->mu);
    if (r->kind == RQ_PREVIEW || r->kind == RQ_ROWS) {   /* (a newer one replaces one not started) */
        int i;
        for (i = 0; i < k->rqn; i++)
            if (k->rq[(k->rqh + i) % NREQ].kind == r->kind)
                k->rq[(k->rqh + i) % NREQ].kind = 0;
    }
    if (k->rqn == NREQ)
        k->rqh = (k->rqh + 1) % NREQ, k->rqn--;
    k->rq[(k->rqh + k->rqn) % NREQ] = *r;
    k->rqn++;
    wake(k);
    pthread_mutex_unlock(&k->mu);
}
static void answer(kit_t *k, const ans_t *a)
{
    pthread_mutex_lock(&k->mu);
    if (k->ann == NANS) {                               /* (full: the oldest dropped, its sound freed) */
        free(k->an[k->anh].pcm);
        k->anh = (k->anh + 1) % NANS, k->ann--;
    }
    k->an[(k->anh + k->ann) % NANS] = *a;
    k->ann++;
    pthread_mutex_unlock(&k->mu);
}
static void garbage(kit_t *k, int16_t *p)   /* (engine thread) freed on the file thread */
{
    if (!p)
        return;
    pthread_mutex_lock(&k->mu);
    if (k->ngarb < NGARB)
        k->garb[k->ngarb++] = p, p = NULL;
    wake(k);
    pthread_mutex_unlock(&k->mu);
    free(p);                                            /* (the list full: here, rare) */
}
static void id_keep(kit_t *k, uint32_t id, const char *rel)   /* (mu held) */
{
    int i;
    for (i = 0; i < k->idn; i++)
        if (k->ids[i].id == id)
            return;
    i = k->idn < NIDS ? k->idn++ : (int)(id % NIDS);
    k->ids[i].id = id;
    snprintf(k->ids[i].rel, sizeof k->ids[i].rel, "%s", rel);
}
static int id_rel(kit_t *k, uint32_t id, char *rel, size_t cap)
{
    int i, ok = 0;
    pthread_mutex_lock(&k->mu);
    for (i = 0; i < k->idn && !ok; i++)
        if (k->ids[i].id == id)
            snprintf(rel, cap, "%s", k->ids[i].rel), ok = 1;
    pthread_mutex_unlock(&k->mu);
    return ok;
}

/* ================================================================ the file thread === */
static int cmp_ls(const void *a, const void *b)
{
    const struct { char name[256]; int dir; } *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;                         /* (folders first) */
    return strcasecmp(x->name, y->name);
}
/* Samples: a folder's rows (dot files skipped; folders, then WAV / AIFF files, by name) */
static int list_folder(kit_t *k, uint32_t folder)
{
    char rel[256] = "", path[KIT_PATH];
    DIR *d;
    struct dirent *e;
    int cap = 0;
    if (k->ls_ok && k->ls_folder == folder)
        return 0;
    k->ls_ok = 0, k->ls_n = 0;
    if (!k->samples[0] || (folder && !id_rel(k, folder, rel, sizeof rel)))
        return -1;
    snprintf(path, sizeof path, "%s%s%s", k->samples, rel[0] ? "/" : "", rel);
    if (!(d = opendir(path)))
        return -1;
    while ((e = readdir(d))) {
        char p[KIT_PATH];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (stat(p, &st))
            continue;
        if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && sound_file(e->d_name)))
            continue;
        if (k->ls_n == cap) {
            void *n = realloc(k->ls, sizeof *k->ls * (size_t)(cap = cap ? cap * 2 : 64));
            if (!n)
                break;
            k->ls = n;
        }
        snprintf(k->ls[k->ls_n].name, sizeof k->ls[0].name, "%s", e->d_name);
        k->ls[k->ls_n++].dir = S_ISDIR(st.st_mode);
    }
    closedir(d);
    qsort(k->ls, (size_t)k->ls_n, sizeof *k->ls, cmp_ls);
    k->ls_ok = 1, k->ls_folder = folder;
    return 0;
}
static void take_name(const tl_row_t *t, char *o, int cap)   /* a take's row: "M 4 Bars", "1 7.2 s" */
{
    static const char SIGN[] = "M123ISLK";
    const char s = t->src >= 0 && t->src < 8 ? SIGN[t->src] : '?';
    if (t->bars)
        snprintf(o, (size_t)cap, "%c %u Bar%s", s, t->bars, t->bars == 1 ? "" : "s");
    else {
        const uint64_t ms = t->rate ? t->frames * 1000u / t->rate : 0;
        snprintf(o, (size_t)cap, "%c %u.%u s", s, (unsigned)(ms / 1000u), (unsigned)(ms % 1000u / 100u));
    }
}
static void do_rows(kit_t *k, const req_t *r)
{
    ans_t a;
    int i;
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_ROWS, a.a.place = r->place, a.a.folder = r->id, a.a.from = r->from;
    if (r->place == KIT_PL_TAKES && k->cfg.tl) {
        tl_row_t t[KIT_ROWS];
        uint32_t f;
        const int n = tl_rows(k->cfg.tl, TL_ALL, r->from, r->n > KIT_ROWS ? KIT_ROWS : r->n, t, &a.a.total, &f);
        for (i = 0; i < n; i++) {
            kit_row_t *o = &a.a.rows[i];
            o->id = t[i].id, o->kind = KIT_ROW_FILE;
            take_name(&t[i], o->name, sizeof o->name);
            o->frames = (uint32_t)t[i].frames, o->rate = t[i].rate;
            memcpy(o->thumb, t[i].thumb, KIT_THUMB);
        }
        a.a.n = n;
    } else if (r->place == KIT_PL_SAMPLES && !list_folder(k, r->id)) {
        char rel[256] = "";
        if (r->id)
            id_rel(k, r->id, rel, sizeof rel);
        a.a.total = k->ls_n;
        for (i = 0; i < r->n && i < KIT_ROWS && r->from + i < k->ls_n; i++) {
            kit_row_t *o = &a.a.rows[i];
            char sub[256], path[KIT_PATH];
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", k->ls[r->from + i].name);
            o->id = fnv(sub), o->kind = k->ls[r->from + i].dir ? KIT_ROW_DIR : KIT_ROW_FILE;
            pthread_mutex_lock(&k->mu);
            id_keep(k, o->id, sub);
            pthread_mutex_unlock(&k->mu);
            if (o->kind == KIT_ROW_DIR)
                snprintf(o->name, sizeof o->name, "%s", k->ls[r->from + i].name);
            else {
                base_name(k->ls[r->from + i].name, o->name, sizeof o->name);
                snprintf(path, sizeof path, "%s/%s", k->samples, sub);
                thumb_of(path, o);
            }
        }
        a.a.n = i;
    }
    answer(k, &a);
}
/* a row's file: a take's WAV (a stems take: its Master; *take 1, tr its row), a sample's path. 1 found */
static int row_path(kit_t *k, int place, uint32_t id, char *path, size_t cap, char *name, int ncap, tl_row_t *tr)
{
    if (place == KIT_PL_TAKES && k->cfg.tl) {
        char own[KIT_PATH];
        tl_row_t t;
        if (!tl_file(k->cfg.tl, id, own, sizeof own, path, cap, &t))
            return 0;
        tl_find(k->cfg.tl, id, &t);                     /* (its source, bars, length: as its row shows them) */
        take_name(&t, name, ncap);
        if (tr)
            *tr = t;
        return 1;
    }
    if (place == KIT_PL_SAMPLES) {
        char rel[256];
        if (!id_rel(k, id, rel, sizeof rel))
            return 0;
        snprintf(path, cap, "%s/%s", k->samples, rel);
        base_name(rel, name, ncap);
        return 1;
    }
    return 0;
}
static void do_load(kit_t *k, const req_t *r)
{
    ans_t a;
    char path[KIT_PATH];
    uint64_t max;
    tl_row_t tr;
    int i;
    memset(&a, 0, sizeof a);
    memset(&tr, 0, sizeof tr);
    tr.id = 0;
    a.a.kind = KIT_A_LOADED, a.a.pad = r->pad;
    if (r->kind == RQ_LOAD) {
        snprintf(path, sizeof path, "%s", r->path);
        if (path[0])
            base_name(path, a.a.name, sizeof a.a.name);
    } else if (!row_path(k, r->place, r->id, path, sizeof path, a.a.name, sizeof a.a.name, &tr)) {
        a.a.status = KIT_MISSING;
        answer(k, &a);
        return;
    }
    if (!path[0]) {                                     /* (emptied: no sound) */
        a.a.status = KIT_OK;
        answer(k, &a);
        return;
    }
    pthread_mutex_lock(&k->mu);                         /* (the kit's memory but this pad's) */
    for (i = 0, max = 0; i < KIT_PADS; i++)
        max += i == r->pad ? 0 : k->used[i];
    pthread_mutex_unlock(&k->mu);
    max = max < k->cfg.max_bytes ? k->cfg.max_bytes - max : 0;
    a.pcm = load_sound(path, max, 0, &a.a.frames, &a.a.rate, &a.a.status);
    snprintf(a.a.path, sizeof a.a.path, "%s", path);
    if (a.pcm) {
        a.a.npk = peaks_of(a.pcm, a.a.frames, KIT_PEAKS, a.a.pk);
        if (tr.id)                                      /* (a take: named as its row, its length now known) */
            tr.frames = a.a.frames, tr.rate = a.a.rate, take_name(&tr, a.a.name, sizeof a.a.name);
        pthread_mutex_lock(&k->mu);
        k->used[r->pad] = a.a.frames * 4u;
        pthread_mutex_unlock(&k->mu);
    }
    answer(k, &a);
}
static void do_preview(kit_t *k, const req_t *r)
{
    ans_t a;
    char path[KIT_PATH], nm[KIT_NAME];
    int st;
    memset(&a, 0, sizeof a);
    a.preview = 1, a.gen = r->gen;
    if (r->id && row_path(k, r->place, r->id, path, sizeof path, nm, sizeof nm, NULL))   /* (heard: its first seconds) */
        a.pcm = load_sound(path, (uint64_t)KIT_PREVIEW_S * 4u * 192000u, KIT_PREVIEW_S * 96000u, &a.a.frames, &a.a.rate, &st);
    answer(k, &a);
}
void kit_work(void *arg)
{
    kit_t *k = arg;
    for (;;) {
        req_t r;
        int16_t *g[NGARB];
        int ng, i;
        pthread_mutex_lock(&k->mu);
        k->posted = 0;
        ng = k->ngarb;
        memcpy(g, k->garb, sizeof g[0] * (size_t)ng);
        k->ngarb = 0;
        r.kind = 0;
        while (k->rqn && !r.kind) {
            r = k->rq[k->rqh];
            k->rqh = (k->rqh + 1) % NREQ, k->rqn--;
        }
        pthread_mutex_unlock(&k->mu);
        for (i = 0; i < ng; i++)
            free(g[i]);
        switch (r.kind) {
        case RQ_LOAD:
        case RQ_LOAD_ROW: do_load(k, &r); break;
        case RQ_ROWS: do_rows(k, &r); break;
        case RQ_PREVIEW: do_preview(k, &r); break;
        default: return;                                /* (nothing more) */
        }
    }
}
static void *own_thread(void *arg)
{
    kit_t *k = arg;
    pthread_mutex_lock(&k->mu);
    while (!k->quit) {
        if (!k->rqn && !k->ngarb) {
            pthread_cond_wait(&k->cv, &k->mu);
            continue;
        }
        pthread_mutex_unlock(&k->mu);
        kit_work(k);
        pthread_mutex_lock(&k->mu);
    }
    pthread_mutex_unlock(&k->mu);
    return NULL;
}

/* ================================================================ make, free === */
kit_t *kit_new(const kit_cfg_t *cfg)
{
    kit_t *k = calloc(1, sizeof *k);
    int p;
    if (!k || !(k->an = calloc(NANS, sizeof *k->an))) {
        free(k);
        return NULL;
    }
    k->cfg = *cfg;
    if (!k->cfg.fs)
        k->cfg.fs = 48000;
    if (!k->cfg.max_bytes)
        k->cfg.max_bytes = KIT_MAX_BYTES;
    snprintf(k->samples, sizeof k->samples, "%s", cfg->samples_dir ? cfg->samples_dir : "");
    for (p = 0; p < KIT_PADS; p++)
        k->v[p][KIT_P_END] = KIT_FRAC;
    pthread_mutex_init(&k->mu, NULL);
    pthread_cond_init(&k->cv, NULL);
    if (!cfg->tl && cfg->own_thread) {
        if (pthread_create(&k->th, NULL, own_thread, k)) {
            pthread_cond_destroy(&k->cv);
            pthread_mutex_destroy(&k->mu);
            free(k->an);
            free(k);
            return NULL;
        }
        k->th_on = 1;
    }
    return k;
}
void kit_free(kit_t *k)
{
    int p;
    if (!k)
        return;
    if (k->th_on) {
        pthread_mutex_lock(&k->mu);
        k->quit = 1;
        pthread_cond_signal(&k->cv);
        pthread_mutex_unlock(&k->mu);
        pthread_join(k->th, NULL);
    }
    for (p = 0; p < KIT_PADS; p++)
        free(k->snd[p].pcm);
    free(k->prev.pcm);
    while (k->ann)
        free(k->an[k->anh].pcm), k->anh = (k->anh + 1) % NANS, k->ann--;
    for (p = 0; p < k->ngarb; p++)
        free(k->garb[p]);
    free(k->ls);
    free(k->an);
    pthread_cond_destroy(&k->cv);
    pthread_mutex_destroy(&k->mu);
    free(k);
}

/* ================================================================ the engine's thread === */
void kit_load(kit_t *k, int pad, const char *path)
{
    req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_LOAD, r.pad = pad;
    snprintf(r.path, sizeof r.path, "%s", path ? path : "");
    request(k, &r);
}
void kit_load_row(kit_t *k, int pad, int place, uint32_t id)
{
    req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_LOAD_ROW, r.pad = pad, r.place = place, r.id = id;
    request(k, &r);
}
void kit_rows(kit_t *k, int place, uint32_t folder, int from, int n)
{
    req_t r;
    if (!k)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_ROWS, r.place = place, r.id = folder, r.from = from < 0 ? 0 : from, r.n = n;
    request(k, &r);
}
void kit_preview(kit_t *k, int place, uint32_t id, int on)
{
    req_t r;
    if (!k)
        return;
    k->prev_gen++;
    k->prev_on = 0;                                     /* (what plays stops at once; a new one when it is read) */
    if (!on)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_PREVIEW, r.place = place, r.id = id, r.gen = k->prev_gen;
    request(k, &r);
}
static void voice_kill_pad(kit_t *k, int pad)          /* (a sound replaced: its voices gone at once) */
{
    int i;
    for (i = 0; i < NV; i++)
        if (k->vo[i].on && k->vo[i].pad == pad)
            k->vo[i].on = 0;
}
int kit_poll(kit_t *k, kit_ans_t *out)
{
    ans_t a;
    int got = 0;
    if (!k)
        return 0;
    for (;;) {
        pthread_mutex_lock(&k->mu);
        got = k->ann > 0;
        if (got) {
            a = k->an[k->anh];
            k->anh = (k->anh + 1) % NANS, k->ann--;
        }
        pthread_mutex_unlock(&k->mu);
        if (!got)
            return 0;
        if (a.preview) {                                /* the preview's sound: plays now (an older ask's: dropped) */
            if (a.gen != k->prev_gen || !a.pcm) {
                garbage(k, a.pcm);
                continue;
            }
            garbage(k, k->prev.pcm);
            k->prev.pcm = a.pcm, k->prev.frames = a.a.frames, k->prev.rate = a.a.rate;
            k->prev_pos = 0, k->prev_inc = (double)a.a.rate / k->cfg.fs, k->prev_on = 1;
            continue;
        }
        if (a.a.kind == KIT_A_LOADED && (a.pcm || (a.a.status == KIT_OK && !a.a.path[0]) || a.a.status != KIT_OK)) {
            snd_t *s = &k->snd[a.a.pad];
            if (a.pcm || a.a.status == KIT_OK) {        /* (a sound, or emptied: the old one goes) */
                voice_kill_pad(k, a.a.pad);
                garbage(k, s->pcm);
                s->pcm = a.pcm, s->frames = a.pcm ? a.a.frames : 0, s->rate = a.pcm ? a.a.rate : 0;
                if (!a.pcm) {
                    pthread_mutex_lock(&k->mu);
                    k->used[a.a.pad] = 0;
                    pthread_mutex_unlock(&k->mu);
                }
            }
        }
        *out = a.a;
        return 1;
    }
}
void kit_set(kit_t *k, int pad, int param, int value)
{
    int i;
    if (!k || pad < 0 || pad >= KIT_PADS || param < 0 || param >= KIT_P_N)
        return;
    k->v[pad][param] = (int16_t)value;
    if (param == KIT_P_MUTE && value)                   /* (muted: its voices fade) */
        for (i = 0; i < NV; i++)
            if (k->vo[i].on && k->vo[i].pad == pad && k->vo[i].stage != ST_FADE)
                k->vo[i].stage = ST_FADE, k->vo[i].fade_n = k->cfg.fs * FADE_MS / 1000u, k->vo[i].t = 0, k->vo[i].r_from = k->vo[i].env;
}
void kit_note(kit_t *k, int pad, int semis, int vel, uint32_t frame)
{
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    if (k->nev == NEV)
        return;                                         /* (a block's worth: more is lost) */
    k->ev[k->nev].pad = pad, k->ev[k->nev].semis = semis, k->ev[k->nev].vel = vel, k->ev[k->nev].frame = frame;
    k->nev++;
}
static void fade(kit_t *k, voice_t *v, int ms)
{
    if (v->stage == ST_FADE)
        return;
    v->stage = ST_FADE, v->t = 0, v->r_from = v->env, v->fade_n = k->cfg.fs * (uint32_t)ms / 1000u;
    if (!v->fade_n)
        v->fade_n = 1;
}
static uint32_t ms_n(const kit_t *k, int ms) { return (uint32_t)((uint64_t)k->cfg.fs * (uint32_t)(ms < 0 ? 0 : ms) / 1000u); }
static void note_on(kit_t *k, int pad, int semis, int vel)
{
    const int16_t *p = k->v[pad];
    const snd_t *s = &k->snd[pad];
    voice_t *v = NULL;
    int i, live = 0, oldest = -1;
    double s0, e0, ratio;
    if (!s->pcm || !s->frames || p[KIT_P_MUTE])
        return;
    for (i = 0; i < NV; i++) {                          /* its own voice taken back; the choke group's stopped */
        voice_t *o = &k->vo[i];
        if (!o->on || o->stage == ST_FADE)
            continue;
        if (o->pad == pad && o->semis == semis)
            fade(k, o, FADE_MS);
        else if (p[KIT_P_CHOKE] && o->pad != pad && k->v[o->pad][KIT_P_CHOKE] == p[KIT_P_CHOKE])
            fade(k, o, FADE_MS);
    }
    for (i = 0; i < NV; i++)
        if (k->vo[i].on && k->vo[i].stage != ST_FADE) {
            live++;
            if (oldest < 0 || k->vo[i].age < k->vo[oldest].age)
                oldest = i;
        }
    if (live >= KIT_VOICES && oldest >= 0)              /* (full: the oldest stolen) */
        fade(k, &k->vo[oldest], DECLICK_MS);
    for (i = 0; i < NV && !v; i++)
        if (!k->vo[i].on)
            v = &k->vo[i];
    if (!v) {                                           /* (every slot fading: the quietest given up) */
        int q = 0;
        for (i = 1; i < NV; i++)
            if (k->vo[i].env < k->vo[q].env)
                q = i;
        v = &k->vo[q];
    }
    memset(v, 0, sizeof *v);
    s0 = (double)s->frames * p[KIT_P_START] / KIT_FRAC;
    e0 = (double)s->frames * p[KIT_P_END] / KIT_FRAC;
    if (e0 <= s0 + 1)
        e0 = s0 + 1 < s->frames ? s0 + 1 : s->frames, s0 = e0 - 1;
    ratio = pow(2.0, (p[KIT_P_PITCH] + semis * 100) / 1200.0) * (double)s->rate / k->cfg.fs;
    v->on = 1, v->pad = pad, v->semis = semis, v->age = ++k->age, v->snd = s;
    v->s = s0, v->e = e0, v->dir = p[KIT_P_REVERSE] ? -1 : 1;
    v->pos = v->dir > 0 ? s0 : e0 - 1;
    v->inc = ratio > 0 ? ratio : 1.0;
    v->mode = clampi(p[KIT_P_MODE], KIT_M_ONESHOT, KIT_M_LOOP), v->gate = 1;
    v->stage = ST_ATTACK, v->env = 0.0f;
    v->a_step = p[KIT_P_ATTACK] > 0 ? 1.0f / (float)ms_n(k, p[KIT_P_ATTACK]) : 1.0f;
    v->hold_n = ms_n(k, p[KIT_P_HOLD]);
    v->rel_n = ms_n(k, p[KIT_P_RELEASE]);
    {
        const double lin = p[KIT_P_LEVEL] <= -60 ? 0.0 : pow(10.0, p[KIT_P_LEVEL] / 20.0);
        const double g = lin * (vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0;
        const int pan = clampi(p[KIT_P_PAN], -50, 50);
        v->gl = (float)(g * (pan > 0 ? 1.0 - pan / 50.0 : 1.0));
        v->gr = (float)(g * (pan < 0 ? 1.0 + pan / 50.0 : 1.0));
    }
}
static void note_off(kit_t *k, int pad, int semis)
{
    int i;
    for (i = 0; i < NV; i++) {
        voice_t *v = &k->vo[i];
        if (v->on && v->gate && v->pad == pad && v->semis == semis)
            v->gate = 0;                                /* (a one shot plays on; the held modes: their release) */
    }
}
static void release(voice_t *v, uint32_t n)
{
    v->stage = ST_RELEASE, v->t = 0, v->r_from = v->env, v->rel_n = n;
}
/* one voice, frames [0, n) of acc */
static void voice_run(voice_t *v, float *acc, uint32_t n)
{
    const int16_t *pcm = v->snd->pcm;
    const uint32_t last = v->snd->frames - 1;
    uint32_t i;
    for (i = 0; i < n && v->on; i++) {
        double left;
        /* the envelope */
        switch (v->stage) {
        case ST_ATTACK:
            v->env += v->a_step;
            if (v->env >= 1.0f)
                v->env = 1.0f, v->stage = v->hold_n ? ST_HOLD : ST_SUSTAIN, v->t = 0;
            break;
        case ST_HOLD:
            if (++v->t >= v->hold_n) {
                if (v->mode == KIT_M_ONESHOT || !v->gate)
                    release(v, v->rel_n);               /* (one shot: the hold's gate; a held mode let go meanwhile) */
                else
                    v->stage = ST_SUSTAIN, v->t = 0;
            }
            break;
        case ST_SUSTAIN:
            if (v->mode != KIT_M_ONESHOT && !v->gate)
                release(v, v->rel_n);
            break;
        default: break;
        }
        if (v->stage == ST_RELEASE || v->stage == ST_FADE) {
            const uint32_t len = v->stage == ST_FADE ? v->fade_n : v->rel_n;
            if (!len || v->t >= len) {
                v->on = 0;
                break;
            }
            v->env = v->r_from * (1.0f - (float)v->t / (float)len);
            v->t++;
        }
        /* one shot with no hold: the release is the tail, ending at the end */
        left = v->dir > 0 ? (v->e - v->pos) / v->inc : (v->pos - v->s) / v->inc;
        if (v->mode == KIT_M_ONESHOT && !v->hold_n && v->stage <= ST_SUSTAIN && v->rel_n && left <= v->rel_n)
            release(v, (uint32_t)left ? (uint32_t)left : 1);
        if (v->mode != KIT_M_LOOP && left <= 0) {
            v->on = 0;
            break;
        }
        {   /* the frame: linear between the two source frames about pos (either way it plays) */
            const double ps = v->pos < 0 ? 0 : v->pos;
            const uint32_t a = (uint32_t)ps > last ? last : (uint32_t)ps, b = a + 1 > last ? last : a + 1;
            const float fr = (float)(ps - a);
            const float l = pcm[2 * a] + (pcm[2 * b] - pcm[2 * a]) * fr;
            const float r = pcm[2 * a + 1] + (pcm[2 * b + 1] - pcm[2 * a + 1]) * fr;
            acc[2 * i] += l * v->env * v->gl;
            acc[2 * i + 1] += r * v->env * v->gr;
        }
        v->pos += v->dir * v->inc;
        if (v->mode == KIT_M_LOOP) {                    /* round start..end while held (and through its release) */
            const double len = v->e - v->s;
            if (v->dir > 0 && v->pos >= v->e)
                v->pos -= len;
            else if (v->dir < 0 && v->pos < v->s)
                v->pos += len;
        }
    }
}
void kit_render(kit_t *k, float *acc, uint32_t frames)
{
    uint32_t at = 0;
    int i, e = 0;
    if (!k)
        return;
    while (at < frames) {                               /* in parts between the notes' frames */
        uint32_t end = frames;
        while (e < k->nev && k->ev[e].frame <= at) {
            const ev_t *x = &k->ev[e++];
            if (x->vel > 0)
                note_on(k, x->pad, x->semis, x->vel);
            else
                note_off(k, x->pad, x->semis);
        }
        if (e < k->nev && k->ev[e].frame < end)
            end = k->ev[e].frame;
        for (i = 0; i < NV; i++)
            if (k->vo[i].on)
                voice_run(&k->vo[i], acc + 2 * at, end - at);
        at = end;
    }
    for (; e < k->nev; e++)                             /* (a frame past the block: now) */
        if (k->ev[e].vel > 0)
            note_on(k, k->ev[e].pad, k->ev[e].semis, k->ev[e].vel);
        else
            note_off(k, k->ev[e].pad, k->ev[e].semis);
    k->nev = 0;
}
void kit_preview_mix(kit_t *k, int16_t *out, uint32_t frames)
{
    uint32_t i;
    if (!k || !k->prev_on || !k->prev.pcm)
        return;
    for (i = 0; i < frames; i++) {
        const uint32_t a = (uint32_t)k->prev_pos;
        if (a + 1 >= k->prev.frames) {
            k->prev_on = 0;
            break;
        }
        {
            const float fr = (float)(k->prev_pos - a);
            int c;
            for (c = 0; c < 2; c++) {
                const float x = k->prev.pcm[2 * a + c] + (k->prev.pcm[2 * (a + 1) + c] - k->prev.pcm[2 * a + c]) * fr;
                const long y = out[2 * i + c] + lrintf(x);
                out[2 * i + c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
            }
        }
        k->prev_pos += k->prev_inc;
    }
}

/* ================================================================ status === */
int kit_voices(const kit_t *k)
{
    int i, n = 0;
    for (i = 0; k && i < NV; i++)
        n += k->vo[i].on;
    return n;
}
uint32_t kit_bytes(const kit_t *k)
{
    uint32_t n = 0;
    int p;
    for (p = 0; k && p < KIT_PADS; p++)
        n += k->snd[p].frames * 4u;
    return n;
}
uint32_t kit_frames(const kit_t *k, int pad) { return k && pad >= 0 && pad < KIT_PADS ? k->snd[pad].frames : 0; }
int kit_previewing(const kit_t *k) { return k && k->prev_on; }
