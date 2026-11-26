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
#include <unistd.h>

#include "minijson.h"
#include "takes.h"

#define NV 32                     /* voice slots: KIT_VOICES sounding, the rest fading out */
#define NEV 256                   /* notes waiting for their frame */
#define NREQ 64                   /* file requests waiting */
#define NANS 32                   /* answers waiting */
#define NGARB 64                  /* sounds to free on the file thread */
#define NIDS 2048                 /* Samples' rows seen: id -> path */
#define FADE_MS 5                 /* a choke, a pad's own voice taken back, a mute */
#define DECLICK_MS 2              /* a stolen voice, a hard end */
#define RBUF 256                  /* (3b) the frames rendered at once into the kit's own buses */
#define KITS_MAX 256              /* (3b) saved kits listed */
#define NSH 48                    /* (3c) sounds in memory, shared by the pads that hold them (a take's slices) */

typedef struct {                  /* a sound in memory */
    int16_t *pcm;                 /* stereo, interleaved */
    uint32_t frames, rate;
} snd_t;
#define NGR 16                    /* (pages v2) grains a voice at most (Warp: 2; Granular: its density) */
typedef struct {
    double pos;                   /* the grain's read position (source frames) */
    double inc;                   /* .. source frames a frame (its pitch) */
    uint32_t age;                 /* frames since it started (its window) */
    int on;
} grain_t;
typedef struct {
    int on;
    int pad, semis;
    uint32_t age;
    double pos, inc;              /* source frames: where, a frame of output */
    int dir;                      /* +1, -1 (reverse) */
    double s, e;                  /* the region [s, e) in source frames */
    int mode, gate;               /* (pages v2) how it plays: B_ONE, B_MOM, B_LOOP, B_HELD */
    int stage;                    /* ST_* */
    float env;                    /* 0..1 */
    float a_step;                 /* attack: per frame */
    uint32_t hold_n, t;           /* hold: frames; t: frames in the stage */
    uint32_t rel_n;               /* release: frames */
    float r_from;
    uint32_t fade_n;              /* a fade out: its frames (ST_FADE) */
    float gl, gr2;                /* its level and pan: left, right */
    float sg[3];                  /* (3b) the pad's sends 1..3 (0..1) */
    const snd_t *snd;
    /* (round B) the decay (frames) to the sustain's level; one shot: legacy (Hold > 0, Decay 0, Sustain 100 %: the
     * release after the hold, as before round B), else the release a tail ending at the region's end */
    uint32_t dec_n;
    float sus, d_from;
    int legacy;
    int cents;                    /* its pitch (a step's lock, else the pad's) */
    /* (round B) the low-pass (a TPT state variable filter, stereo): off while open (bypassed: today's frames); the
     * cutoff's g glides to its target (no zipper), the coefficients made every FLT_N frames and ramped between.
     * (pages v2) its output morphs LP -> BP -> HP; its envelope moves the cutoff up to 8 octaves either way */
    int flt, cut_lk, res_lk;      /* on; the cutoff / resonance locked by a step (the pad's later changes not followed) */
    float g, g_tgt, kq;           /* tan(pi fc / fs) now and its target; 1 / Q */
    float a1, a2, a3, da1, da2, da3;
    uint32_t fn;                  /* frames to the next coefficients */
    float ic1[2], ic2[2];
    /* ---- (pages v2) */
    int eng, warp;                /* the pad's engine (KIT_M_*), Warp (KIT_W_*) as it plays */
    double ls, le, xf;            /* Loop: its loop [ls, le), the fade's source frames (0 none) */
    double binc;                  /* the rate before the pitch envelope (Off, Repitch) */
    float morph;                  /* the filter's 0 LP .. 1 HP */
    int morph_lk, fenv_lk;
    int cut_v;                    /* the cutoff aimed at (0..100: the screen's) */
    float fe_amt;                 /* the filter envelope: octaves at its top */
    int fe_st;                    /* 0 attack, 1 decay, 2 sustain, 3 release, 4 done */
    float fe_lvl, fe_s, fe_from;
    uint32_t fe_a, fe_d, fe_r, fe_t;   /* (in FLT_N blocks) */
    float pe_depth;               /* Pitch Env: semitones; its attack, release (frames), frames since the start */
    uint32_t pe_a, pe_r, pe_t;
    float pe_mul;                 /* .. the rate's multiplier now (every FLT_N frames) */
    uint32_t cn;                  /* frames to the next control block (the pitch envelope, LoFi, the filter's envelope) */
    float q_step;                 /* LoFi: the quantum (1: none), a sample held so many frames */
    int ds, ds_n;
    float hold_l, hold_r;
    double ph, spd;               /* Warp Grains / Beats: the playhead and its speed (source frames a frame) */
    double ginc;                  /* .. the grains' pitch (source frames a frame) */
    uint32_t glen;                /* a grain's frames (Warp, Granular) */
    float wstep;                  /* .. the window table's steps a frame */
    double jit;                   /* Warp Grains: the jitter's source frames; Beats: the search's */
    uint32_t gspawn, gnext;       /* Granular: frames between grains, to the next */
    float ggain;
    uint32_t rnd;
    grain_t gr[NGR];
    /* (2026-11-26) the hit's step locks on any setting: bit p of pm, pl[p] its value (the pad's edits leave them);
     * a locked Sync: us_lk its microseconds */
    uint64_t pm;
    int16_t pl[KIT_P_N];
    uint32_t us_lk;
    int vel;                      /* (2026-11-26) its velocity (a ghost trigger's Level / Pan again) */
} voice_t;
enum { ST_ATTACK, ST_HOLD, ST_DECAY, ST_SUSTAIN, ST_RELEASE, ST_FADE };
enum { B_ONE, B_MOM, B_LOOP, B_HELD };   /* (pages v2) a voice's play: once, held once, round its loop, held (Granular) */
#define FLT_N 32                  /* (round B) the filter's coefficients made every 32 frames */
#define FLT_GLIDE_MS 10           /* .. its cutoff's glide */
typedef struct {
    int pad, semis, vel;
    uint32_t frame;
    int mask;
    int16_t lk[KIT_LK_N];
    uint64_t pm;                  /* (2026-11-26) every other lock: bit p, pv[p]; us a locked Sync's microseconds */
    int16_t pv[KIT_P_N];
    uint32_t us;
} ev_t;
/* (2026-11-26, the owner's rule: "a sample should only ever play by pressing the pad or if I sequenced it, always
 * through the same routing path") the slicing screen's slices and Tap's pass play as a kit voice of a hidden pad SLP
 * (the defaults: one shot, 0 dB, centred, no sends) through the kit's bus, its level, pan, sends, filter, the FX and
 * the master; no preview or audition path any more */
#define SLP KIT_PADS
enum { RQ_LOAD = 1, RQ_LOAD_ROW, RQ_ROWS, RQ_PREVIEW, RQ_SAVE, RQ_OPEN, RQ_FOLDER, RQ_MERGE, RQ_EDIT, RQ_SLICE,
       RQ_FIND, RQ_PEAKS, RQ_KEEP };   /* (round A: FIND, PEAKS; round C: KEEP) */
typedef struct {
    int kind, pad, place, n, from;
    uint32_t id, gen;
    char path[KIT_PATH];
    /* (3c) MERGE: pad2 the first pad, path2 its file, op; EDIT: op; the regions (of KIT_FRAC): path's r0, path2's r1;
     * SLICE: id the take, op the method, us a step */
    int pad2, op;
    int r0[2], r1[2];
    uint32_t us;
    char path2[KIT_PATH];
    /* (round A) LOAD: trim (a take recorded into the pad: its lead); SLICE: n the Hits' sensitivity; PEAKS: zf, zc the
     * frames, n the peaks */
    int trim;
    uint32_t zf, zc;
    /* (round C) the project's folder when asked ("" none); LOAD: own (the sound copied in first); EDIT FREEZE: v the
     * pad's settings, us its Stretch */
    char proj[KIT_PATH];
    int own;
    int16_t v[KIT_P_N];
} req_t;
typedef struct {                  /* the file thread's answer, and the sound it made (the engine's thread puts it on) */
    kit_ans_t a;
    int16_t *pcm;                 /* LOADED OK, PREVIEW: the sound (the engine's from then); (3c) SLICES: the take */
    int preview;
    uint32_t gen;
    char abs[KIT_PATH];           /* (round C) LOADED, OWNED: the file (path is the core's: KIT_OWN.. in the project's) */
    char abs2[KIT_PATH];          /* .. OWNED: the file before */
} ans_t;
typedef struct {                  /* (3c, mu) a sound in memory and the pads (answers) holding it */
    int16_t *pcm;
    int refs;
    uint32_t frames, rate;
    long long mtime, size;
    int top;                      /* (round A) its peak (0: not known yet; the close-up's scale) */
    char path[KIT_PATH];
} sh_t;

struct kit {
    kit_cfg_t cfg;
    char samples[KIT_PATH];
    /* the engine's */
    snd_t snd[KIT_PADS + 1];      /* (2026-11-26) [SLP]: the slicing screen's take, played as a kit voice */
    int16_t v[KIT_PADS + 1][KIT_P_N];
    uint32_t st_us[KIT_PADS + 1];     /* (3b) Stretch: the microseconds start..end lasts (0: its pitch) */
    int mix[5];                   /* (3b) the kit's strip: level, pan, sends 1..3 (0..127) */
    volatile float master;        /* (2026-11-26) the volume knob's MASTER as a gain (1: untouched), as the firmwares' */
    float dry[2 * RBUF], sb[3][2 * RBUF];   /* .. the voices before it */
    char kits[KIT_PATH];          /* saved kits' folder ("" none) */
    kit_kit_t save;               /* (mu) the kit a save works on, save_busy while it does */
    char save_proj[KIT_PATH];     /* (mu, round C) .. the project's folder then */
    int save_busy;
    voice_t vo[NV];
    uint32_t age;
    ev_t ev[NEV];
    int nev;
    snd_t prev;                   /* (3c) the take sliced (SLP's sound while it plays) */
    uint32_t sl_take;             /* (3c) the take sliced (its sound the preview's; 0 none), the latest asked */
    uint32_t sl_gen;
    char ppath[KIT_PADS][KIT_PATH];   /* (3c) the file on each pad (the engine's) */
    int16_t *held[KIT_PADS];      /* (3c, mu) the sound each pad holds (the memory count) */
    sh_t sh[NSH];                 /* (3c, mu) */
    char edits[KIT_PATH];         /* (3c) new files' folder (kits/Edits; "" none) */
    uint32_t used[KIT_PADS];      /* (mu) each pad's bytes, as the file thread counts them */
    float glide;                  /* (round B) the filter's glide a FLT_N frames (0..1) */
    /* (pages v2) the kit bus's low-pass (a stereo TPT state variable filter on the voices' mix; open: bypassed) */
    int bus_c, bus_r;
    float bus_g, bus_gt, bus_k, bus_ic1[2], bus_ic2[2];
    int bus_on;
    float hann[1025];             /* (pages v2) a Hann window, the grains' (made at kit_new) */
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
    /* (2026-11-26) the Move Library: its folder ("" none); its index (the file thread's, built once at its first use):
     * the folders with sounds somewhere below them, the sounds this host reads (sorted relative paths) */
    char core[KIT_PATH];
    int cx_ok, cx_nd, cx_nf;
    char **cx_d, **cx_f;
    /* Samples' last listing (the file thread's) */
    int ls_pl;
    uint32_t ls_folder;
    int ls_ok, ls_n;
    struct { char name[256]; int dir; } *ls;
    /* (round C) the project's own sounds */
    char proj[KIT_PATH];          /* the open project's folder (the engine's; "" none) */
    char wlast[KIT_PADS][KIT_PATH];   /* (the file thread's) the file each pad's latest load or edit gave */
    struct { char src[KIT_PATH], dst[KIT_PATH]; long long mtime, size; } own[32];   /* (the file thread's) copies made */
    int own_n;
    int clean_due;                /* (mu) kit_clean asked: clean_dir's files but clean_keep's */
    char clean_dir[KIT_PATH];
    char clean_keep[64][256];
    int clean_n;
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
static void request(kit_t *k, const req_t *r0)
{
    static req_t rq;                                    /* (the engine's thread only: 3 KB not on its stack) */
    const req_t *r = &rq;
    rq = *r0;
    snprintf(rq.proj, sizeof rq.proj, "%s", k->proj);   /* (round C: the project's folder as it is now) */
    pthread_mutex_lock(&k->mu);
    if (r->kind == RQ_PREVIEW || r->kind == RQ_ROWS || r->kind == RQ_SLICE || r->kind == RQ_PEAKS || r->kind == RQ_FIND) {   /* (a newer one replaces one not
                                                         * started) */
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
/* (3c, mu held) one holder of p fewer: 1 when its memory is to go (the caller frees it; a sound not shared: at once) */
static int unref(kit_t *k, const int16_t *p)
{
    int i;
    if (!p)
        return 0;
    for (i = 0; i < NSH; i++)
        if (k->sh[i].pcm == p) {
            if (--k->sh[i].refs > 0)
                return 0;
            memset(&k->sh[i], 0, sizeof k->sh[i]);
            return 1;
        }
    return 1;
}
static void answer(kit_t *k, const ans_t *a)
{
    pthread_mutex_lock(&k->mu);
    if (k->ann == NANS) {                               /* (full: the oldest dropped, its sound freed) */
        if (unref(k, k->an[k->anh].pcm))
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
/* ---- (2026-11-26) the places with folders: My Samples (k->samples) and the Move Library (k->core). A row's id: FNV-1a
 * of its path in the place ("core:" before it in the Move Library's) */
static const char *pl_base(const kit_t *k, int pl) { return pl == KIT_PL_CORE ? k->core : pl == KIT_PL_SAMPLES ? k->samples : ""; }
static uint32_t pl_id(int pl, const char *sub)
{
    char t[300];
    if (pl != KIT_PL_CORE)
        return fnv(sub);
    snprintf(t, sizeof t, "core:%s", sub);
    return fnv(t);
}
static uint32_t pl_keep(kit_t *k, int pl, const char *sub)   /* its id, kept for its path */
{
    char t[300];
    const uint32_t id = pl_id(pl, sub);
    snprintf(t, sizeof t, "%s%s", pl == KIT_PL_CORE ? "core:" : "", sub);
    pthread_mutex_lock(&k->mu);
    id_keep(k, id, t);
    pthread_mutex_unlock(&k->mu);
    return id;
}
static int pl_rel(kit_t *k, int pl, uint32_t id, char *rel, size_t cap)
{
    char t[300];
    int c;
    if (!id_rel(k, id, t, sizeof t))
        return 0;
    c = !strncmp(t, "core:", 5);
    if (c != (pl == KIT_PL_CORE))
        return 0;
    snprintf(rel, cap, "%s", t + (c ? 5 : 0));
    return 1;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int in_set(char **v, int n, const char *s) { return n && bsearch(&s, v, (size_t)n, sizeof *v, cmp_str) != NULL; }
static int push_str(char ***v, int *n, int *cap, const char *s)
{
    if (*n == *cap) {
        char **g = realloc(*v, sizeof **v * (size_t)(*cap = *cap ? *cap * 2 : 256));
        if (!g)
            return -1;
        *v = g;
    }
    if (!((*v)[*n] = strdup(s)))
        return -1;
    (*n)++;
    return 0;
}
static int sound_ok(const char *path)                   /* a sound this host reads (PCM WAV, AIFF, AIFF-C sowt / NONE) */
{
    FILE *f = fopen(path, "rb");
    info_t in;
    int ok;
    if (!f)
        return 0;
    ok = probe(f, &in) == NULL && in.frames > 0;
    fclose(f);
    return ok;
}
#define CX_MAX 50000                                    /* the Move Library's sounds indexed at most */
static void cx_walk(kit_t *k, const char *rel, int depth, int *capd, int *capf)
{
    char path[KIT_PATH];
    DIR *d;
    struct dirent *e;
    snprintf(path, sizeof path, "%s%s%s", k->core, rel[0] ? "/" : "", rel);
    if (depth > KIT_DEPTH || !(d = opendir(path)))
        return;
    while ((e = readdir(d)) && k->cx_nf < CX_MAX) {
        char sub[256], p[KIT_PATH];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        if (snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof sub)
            continue;
        snprintf(p, sizeof p, "%s/%s", k->core, sub);
        if (stat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode))
            cx_walk(k, sub, depth + 1, capd, capf);
        else if (S_ISREG(st.st_mode) && sound_file(e->d_name) && sound_ok(p) && !push_str(&k->cx_f, &k->cx_nf, capf, sub)) {
            char up[256], *sl;                          /* (its folders: each one up to the top) */
            snprintf(up, sizeof up, "%s", sub);
            while ((sl = strrchr(up, '/'))) {
                *sl = 0;
                if (push_str(&k->cx_d, &k->cx_nd, capd, up))
                    break;
            }
        }
    }
    closedir(d);
}
static void cx_build(kit_t *k)                          /* (the file thread, once a launch) */
{
    int capd = 0, capf = 0, i, j;
    if (k->cx_ok || !k->core[0])
        return;
    k->cx_ok = 1;
    cx_walk(k, "", 0, &capd, &capf);
    if (k->cx_nf)
        qsort(k->cx_f, (size_t)k->cx_nf, sizeof *k->cx_f, cmp_str);
    if (k->cx_nd) {
        qsort(k->cx_d, (size_t)k->cx_nd, sizeof *k->cx_d, cmp_str);
        for (i = 1, j = 1; i < k->cx_nd; i++)         /* (each once) */
            if (strcmp(k->cx_d[i], k->cx_d[j - 1]))
                k->cx_d[j++] = k->cx_d[i];
            else
                free(k->cx_d[i]);
        k->cx_nd = j;
    }
    fprintf(stderr, "kit: the Move Library %s: %d sounds in %d folders\n", k->core, k->cx_nf, k->cx_nd);
}
/* a place's folder's rows (dot files skipped; folders, then WAV / AIFF files, by name; the Move Library: only the
 * folders with sounds below them and the sounds this host reads) */
static int list_folder(kit_t *k, int pl, uint32_t folder)
{
    char rel[256] = "", path[KIT_PATH];
    const char *base = pl_base(k, pl);
    DIR *d;
    struct dirent *e;
    int cap = 0;
    if (k->ls_ok && k->ls_folder == folder && k->ls_pl == pl)
        return 0;
    k->ls_ok = 0, k->ls_n = 0;
    if (!base[0] || (folder && !pl_rel(k, pl, folder, rel, sizeof rel)))
        return -1;
    if (pl == KIT_PL_CORE)
        cx_build(k);
    snprintf(path, sizeof path, "%s%s%s", base, rel[0] ? "/" : "", rel);
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
        if (pl == KIT_PL_CORE) {                        /* (the index's) */
            char sub[256];
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
            if (!in_set(S_ISDIR(st.st_mode) ? k->cx_d : k->cx_f, S_ISDIR(st.st_mode) ? k->cx_nd : k->cx_nf, sub))
                continue;
        }
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
    k->ls_ok = 1, k->ls_folder = folder, k->ls_pl = pl;
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
/* ---- (stage 3b) saved kits: a folder each in k->kits with its kit.json */
static int natcmp(const char *a, const char *b)         /* names with numbers in order: Kit 2 before Kit 10 */
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
            if (x != y)
                return x < y ? -1 : 1;
            continue;
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return tolower((unsigned char)*a) - tolower((unsigned char)*b);
        a++, b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int cmp_nat(const void *a, const void *b) { return natcmp(*(const char *const *)a, *(const char *const *)b); }
/* the kits saved, in name order (names[] malloc'd, each too; free with kits_free): how many */
static int kits_list(const kit_t *k, char ***names)
{
    DIR *d;
    struct dirent *e;
    int n = 0;
    *names = NULL;
    if (!k->kits[0] || !(d = opendir(k->kits)))
        return 0;
    if (!(*names = calloc(KITS_MAX, sizeof **names))) {
        closedir(d);
        return 0;
    }
    while ((e = readdir(d)) && n < KITS_MAX) {
        char p[KIT_PATH];
        struct stat st;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= KIT_NAME)
            continue;
        snprintf(p, sizeof p, "%s/%s/kit.json", k->kits, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode))
            continue;
        if (((*names)[n] = strdup(e->d_name)))
            n++;
    }
    closedir(d);
    qsort(*names, (size_t)n, sizeof **names, cmp_nat);
    return n;
}
static void kits_free(char **names, int n)
{
    while (n-- > 0)
        free(names[n]);
    free(names);
}
static uint32_t kit_id(const char *name)
{
    char t[64];
    snprintf(t, sizeof t, "kits:%s", name);
    return fnv(t);
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
    } else if ((r->place == KIT_PL_SAMPLES || r->place == KIT_PL_CORE) && !list_folder(k, r->place, r->id)) {
        char rel[256] = "";
        if (r->id)
            pl_rel(k, r->place, r->id, rel, sizeof rel);
        a.a.total = k->ls_n;
        for (i = 0; i < r->n && i < KIT_ROWS && r->from + i < k->ls_n; i++) {
            kit_row_t *o = &a.a.rows[i];
            char sub[256], path[KIT_PATH];
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", k->ls[r->from + i].name);
            o->id = pl_keep(k, r->place, sub), o->kind = k->ls[r->from + i].dir ? KIT_ROW_DIR : KIT_ROW_FILE;
            if (o->kind == KIT_ROW_DIR)
                snprintf(o->name, sizeof o->name, "%s", k->ls[r->from + i].name);
            else {
                base_name(k->ls[r->from + i].name, o->name, sizeof o->name);
                snprintf(path, sizeof path, "%s/%s", pl_base(k, r->place), sub);
                thumb_of(path, o);
            }
        }
        a.a.n = i;
    } else if (r->place == KIT_PL_KITS) {               /* (3b) the kits saved, a row each */
        char **names;
        const int n = kits_list(k, &names);
        a.a.total = n;
        for (i = 0; i < r->n && i < KIT_ROWS && r->from + i < n; i++) {
            kit_row_t *o = &a.a.rows[i];
            char rel[64];
            o->id = kit_id(names[r->from + i]), o->kind = KIT_ROW_KIT;
            snprintf(o->name, sizeof o->name, "%s", names[r->from + i]);
            snprintf(rel, sizeof rel, "kits:%s", names[r->from + i]);
            pthread_mutex_lock(&k->mu);
            id_keep(k, o->id, rel);
            pthread_mutex_unlock(&k->mu);
        }
        a.a.n = i;
        kits_free(names, n);
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
        {                                               /* (its source, bars, length: as its row shows them; 3c: not
                                                         * found there, tl_file's row stands) */
            tl_row_t f;
            if (tl_find(k->cfg.tl, id, &f))
                t = f;
        }
        take_name(&t, name, ncap);
        if (tr)
            *tr = t;
        return 1;
    }
    if (place == KIT_PL_SAMPLES || place == KIT_PL_CORE) {
        char rel[256];
        if (!pl_rel(k, place, id, rel, sizeof rel))
            return 0;
        snprintf(path, cap, "%s/%s", pl_base(k, place), rel);
        base_name(rel, name, ncap);
        return 1;
    }
    return 0;
}
/* ---- (3c) sounds shared: a file read once whatever the pads that hold it (a take's slices) */
static void file_id(const char *path, long long *mtime, long long *size)
{
    struct stat st;
    *mtime = 0, *size = -1;
    if (!stat(path, &st))
        *mtime = (long long)st.st_mtime, *size = (long long)st.st_size;
}
/* the file's sound in memory (unchanged since read), one holder more: NULL none */
static int16_t *shared_get(kit_t *k, const char *path, uint32_t *frames, uint32_t *rate)
{
    long long mt, sz;
    int16_t *p = NULL;
    int i;
    file_id(path, &mt, &sz);
    if (sz < 0)
        return NULL;
    pthread_mutex_lock(&k->mu);
    for (i = 0; i < NSH && !p; i++)
        if (k->sh[i].pcm && k->sh[i].mtime == mt && k->sh[i].size == sz && !strcmp(k->sh[i].path, path))
            p = k->sh[i].pcm, k->sh[i].refs++, *frames = k->sh[i].frames, *rate = k->sh[i].rate;
    pthread_mutex_unlock(&k->mu);
    return p;
}
static void shared_put(kit_t *k, const char *path, int16_t *pcm, uint32_t frames, uint32_t rate)
{
    int i;
    pthread_mutex_lock(&k->mu);
    for (i = 0; i < NSH; i++)
        if (!k->sh[i].pcm) {
            sh_t *e = &k->sh[i];
            e->pcm = pcm, e->refs = 1, e->frames = frames, e->rate = rate;
            snprintf(e->path, sizeof e->path, "%s", path);
            file_id(path, &e->mtime, &e->size);
            break;
        }
    pthread_mutex_unlock(&k->mu);                       /* (the table full: not shared, freed as its own) */
}
/* the bytes a new sound for pad may take: the kit's but what the others hold (a sound counted once) */
static uint64_t room_for(kit_t *k, int pad)
{
    uint64_t used = 0;
    int i, j;
    pthread_mutex_lock(&k->mu);
    for (i = 0; i < NSH; i++)
        if (k->sh[i].pcm && !(k->sh[i].pcm == k->held[pad] && k->sh[i].refs <= 1))
            used += (uint64_t)k->sh[i].frames * 4u;
    for (i = 0; i < KIT_PADS; i++) {                    /* (a pad's sound not in the table: its own) */
        int in = !k->held[i] || i == pad;
        for (j = 0; j < NSH && !in; j++)
            in = k->sh[j].pcm == k->held[i];
        for (j = 0; j < i && !in; j++)
            in = k->held[j] == k->held[i];
        if (!in)
            used += k->used[i];
    }
    pthread_mutex_unlock(&k->mu);
    return used < k->cfg.max_bytes ? k->cfg.max_bytes - used : 0;
}
/* ---- (round C) the project's own sounds */
static int mkdirs(const char *path);
static int copy_file(const char *from, const char *to);
static int in_dir(const char *dir, const char *path)  /* path a file right in dir */
{
    const size_t n = strlen(dir);
    return dir[0] && !strncmp(path, dir, n) && path[n] == '/' && path[n + 1] && !strchr(path + n + 1, '/');
}
/* the core's path for a file: KIT_OWN + its name in the project's folder, else the file's */
static void core_path(const char *proj, const char *abs, char *o, size_t cap)
{
    if (in_dir(proj, abs) && strlen(abs + strlen(proj) + 1) + 3 <= cap)
        snprintf(o, cap, "%s%s", KIT_OWN, abs + strlen(proj) + 1);
    else
        snprintf(o, cap, "%s", abs);
}
static void resolve_in(const char *proj, const char *in, char *o, size_t cap)
{
    if (!strncmp(in, KIT_OWN, 2) && proj[0])
        snprintf(o, cap, "%s/%s", proj, in + 2);
    else
        snprintf(o, cap, "%s", in);
}
void kit_resolve(const kit_t *k, const char *in, char *out, size_t cap)
{
    resolve_in(k ? k->proj : "", in ? in : "", out, cap);
}
static int same_bytes(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    static char ba[65536], bb[65536];                   /* (the file thread's) */
    int same = x && y;
    while (same) {
        const size_t n = fread(ba, 1, sizeof ba, x), m = fread(bb, 1, sizeof bb, y);
        same = n == m && !memcmp(ba, bb, n);
        if (!n || !m)
            break;
    }
    if (x)
        fclose(x);
    if (y)
        fclose(y);
    return same;
}
/* src kept: its copy in the project's folder into o (made now, or found: the same file copied before, or a file there
 * with the same bytes); a file already there, no folder, a failure: src itself. 1 a copy */
static int own_copy(kit_t *k, const char *proj, const char *src, char *o, size_t cap)
{
    struct stat st, sd;
    char base[256], ext[32], cand[KIT_PATH];
    const char *b = strrchr(src, '/'), *dot;
    int i;
    snprintf(o, cap, "%s", src);
    if (!proj[0] || !src[0] || in_dir(proj, src) || stat(src, &st) || !S_ISREG(st.st_mode))
        return 0;
    for (i = 0; i < k->own_n; i++)                      /* (the same file copied before: that copy) */
        if (!strcmp(k->own[i].src, src) && k->own[i].mtime == (long long)st.st_mtime &&
            k->own[i].size == (long long)st.st_size && in_dir(proj, k->own[i].dst) && !stat(k->own[i].dst, &sd) &&
            (long long)sd.st_size == k->own[i].size) {
            snprintf(o, cap, "%s", k->own[i].dst);
            return 1;
        }
    if (mkdirs(proj))
        return 0;
    snprintf(base, sizeof base, "%s", b ? b + 1 : src);
    ext[0] = 0;
    if ((dot = strrchr(base, '.')) && dot != base) {
        snprintf(ext, sizeof ext, "%s", dot);
        base[dot - base] = 0;
    }
    for (i = 0; base[i]; i++)                           /* (a readable name: printable) */
        if ((unsigned char)base[i] < 32 || (unsigned char)base[i] > 126)
            base[i] = '_';
    base[100] = 0;
    for (i = 1; i < 1000; i++) {
        if (i == 1)
            snprintf(cand, sizeof cand, "%s/%s%s", proj, base, ext);
        else
            snprintf(cand, sizeof cand, "%s/%s %d%s", proj, base, i, ext);
        if (!stat(cand, &sd)) {
            if (sd.st_size == st.st_size && same_bytes(src, cand))
                break;                                  /* (that sound there already) */
            continue;
        }
        {
            char tmp[KIT_PATH + 8];
            snprintf(tmp, sizeof tmp, "%s.tmp", cand);
            if (copy_file(src, tmp) || rename(tmp, cand)) {
                remove(tmp);
                return 0;
            }
        }
        break;
    }
    if (i >= 1000)
        return 0;
    {
        const int j = k->own_n < 32 ? k->own_n++ : (int)(fnv(src) % 32u);
        snprintf(k->own[j].src, sizeof k->own[j].src, "%s", src);
        snprintf(k->own[j].dst, sizeof k->own[j].dst, "%s", cand);
        k->own[j].mtime = (long long)st.st_mtime, k->own[j].size = (long long)st.st_size;
    }
    snprintf(o, cap, "%s", cand);
    return 1;
}
static void do_keep(kit_t *k, const req_t *r)
{
    ans_t a;
    char to[KIT_PATH];
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_OWNED, a.a.pad = r->pad;
    if (!k->wlast[r->pad][0] || !own_copy(k, r->proj, k->wlast[r->pad], to, sizeof to)) {
        fprintf(stderr, "kit: pad %d kept where it is (%s; project folder '%s')\n", r->pad + 1,
                k->wlast[r->pad][0] ? k->wlast[r->pad] : "no sound", r->proj);
        return;                                         /* (no sound; already the project's; not copied: as it was) */
    }
    snprintf(a.abs2, sizeof a.abs2, "%s", k->wlast[r->pad]);
    snprintf(a.abs, sizeof a.abs, "%s", to);
    core_path(r->proj, k->wlast[r->pad], a.a.was, sizeof a.a.was);
    core_path(r->proj, to, a.a.path, sizeof a.a.path);
    snprintf(k->wlast[r->pad], sizeof k->wlast[0], "%s", to);
    answer(k, &a);
}
static int cleaning_keeps(const char (*keep)[256], int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(keep[i], name))
            return 1;
    return 0;
}
/* (kit_work) the project's folder's files nothing refers to: gone (regular files right in it, a minute old) */
static void do_clean(kit_t *k)
{
    static char keep[64 + NANS + KIT_PADS][256];
    char dir[KIT_PATH];
    int n, i;
    DIR *d;
    struct dirent *e;
    const time_t now = time(NULL);
    pthread_mutex_lock(&k->mu);
    k->clean_due = 0;
    snprintf(dir, sizeof dir, "%s", k->clean_dir);
    n = k->clean_n;
    memcpy(keep, k->clean_keep, sizeof k->clean_keep[0] * (size_t)n);
    for (i = 0; i < k->ann; i++) {                      /* (answers on their way: theirs too) */
        const ans_t *x = &k->an[(k->anh + i) % NANS];
        if (in_dir(dir, x->abs))
            snprintf(keep[n++], sizeof keep[0], "%s", strrchr(x->abs, '/') + 1);
    }
    pthread_mutex_unlock(&k->mu);
    for (i = 0; i < KIT_PADS; i++)                      /* (and each pad's latest: a load not answered yet) */
        if (in_dir(dir, k->wlast[i]))
            snprintf(keep[n++], sizeof keep[0], "%s", strrchr(k->wlast[i], '/') + 1);
    if (!dir[0] || !k->kits[0] || strncmp(dir, k->kits, strlen(k->kits)) || !strstr(dir, "/Projects/") ||
        !(d = opendir(dir)))
        return;                                         /* (never outside a project's folder) */
    while ((e = readdir(d))) {
        char f[KIT_PATH + 256];
        struct stat st;
        if (e->d_name[0] == '.' || cleaning_keeps((const char (*)[256])keep, n, e->d_name))
            continue;
        snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
        if (lstat(f, &st) || !S_ISREG(st.st_mode) || now - st.st_mtime < 60)
            continue;
        if (!remove(f))
            fprintf(stderr, "kit: %s: not used by the project, deleted\n", f);
    }
    closedir(d);
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
    a.a.kind = KIT_A_LOADED, a.a.pad = r->pad, a.a.lead = -1;
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
        k->wlast[r->pad][0] = 0;
        answer(k, &a);
        return;
    }
    (void)i;
    if (r->own) {                                       /* (round C: kept: the project's own copy, read from there) */
        char cp[KIT_PATH];
        own_copy(k, r->proj, path, cp, sizeof cp);
        snprintf(path, sizeof path, "%s", cp);
    }
    a.pcm = shared_get(k, path, &a.a.frames, &a.a.rate);   /* (3c: in memory already, another pad's: shared) */
    if (!a.pcm) {
        max = room_for(k, r->pad);
        a.pcm = load_sound(path, max, 0, &a.a.frames, &a.a.rate, &a.a.status);
        if (a.pcm)
            shared_put(k, path, a.pcm, a.a.frames, a.a.rate);
    } else
        a.a.status = KIT_OK;
    core_path(r->proj, path, a.a.path, sizeof a.a.path);   /* (round C: the project's own: KIT_OWN..) */
    snprintf(a.abs, sizeof a.abs, "%s", path);
    snprintf(k->wlast[r->pad], sizeof k->wlast[0], "%s", path);
    if (a.pcm) {
        a.a.npk = peaks_of(a.pcm, a.a.frames, KIT_PEAKS, a.a.pk);
        if (tr.id)                                      /* (a take: named as its row, its length now known) */
            tr.frames = a.a.frames, tr.rate = a.a.rate, take_name(&tr, a.a.name, sizeof a.a.name);
        if (r->trim)                                    /* (round A) Trim silence: where its first sound starts */
            a.a.lead = (int32_t)kit_lead(a.pcm, a.a.frames, a.a.rate);
    }
    answer(k, &a);
}
/* ---- (round A) pad-first loading, the close-up */
uint32_t kit_lead(const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    uint32_t i;
    const uint32_t back = (rate ? rate : 48000) * KIT_LEAD_MS / 1000u;
    for (i = 0; pcm && i < frames; i++)
        if (abs(pcm[2 * i]) >= KIT_LEAD_LEVEL || abs(pcm[2 * i + 1]) >= KIT_LEAD_LEVEL)
            return i > back ? i - back : 0;
    return 0;                                           /* (silent all through: kept whole) */
}
/* FIND: a take (Takes, its row in the list) or a sound in Samples (its folders, their rows) */
static void do_find(kit_t *k, const req_t *r0)
{
    static req_t rr;                                    /* (round C: a copy of the project's: where its source is, when
                                                         * this run made it) */
    const req_t *r = &rr;
    int oi;
    rr = *r0;
    for (oi = 0; oi < k->own_n; oi++)
        if (!strcmp(k->own[oi].dst, r0->path) && !access(k->own[oi].src, F_OK)) {
            snprintf(rr.path, sizeof rr.path, "%s", k->own[oi].src);
            break;
        }
    ans_t a;
    tl_row_t t;
    int pi;
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_FOUND, a.a.pad = r->pad;
    /* (2026-11-26) no Takes place: a take is a file in My Samples (RemoteFM1/<day>/), found there as any other */
    (void)t;
    for (pi = 0; pi < 2 && !a.a.place; pi++) {
        const int pl = pi ? KIT_PL_CORE : KIT_PL_SAMPLES;
        const char *base = pl_base(k, pl);
        const size_t sl = strlen(base);
        char rel[256], sub[256];
        const char *q = r->path + sl + 1;
        int d = 0, ok = 1;
        uint32_t folder = 0;
        if (!sl || strncmp(r->path, base, sl) || r->path[sl] != '/')
            continue;                                   /* (each folder listed, its row) */
        snprintf(rel, sizeof rel, "%s", q);
        sub[0] = 0;
        while (ok) {
            const char *sep = strchr(q, '/');
            char name[256];
            int i, at = -1;
            const size_t len = sep ? (size_t)(sep - q) : strlen(q);
            if (!len || len >= sizeof name || (sep && d >= KIT_DEPTH)) {
                ok = 0;
                break;
            }
            memcpy(name, q, len), name[len] = 0;
            if (list_folder(k, pl, folder)) {
                ok = 0;
                break;
            }
            for (i = 0; i < k->ls_n && at < 0; i++)
                if (!strcmp(k->ls[i].name, name) && k->ls[i].dir == (sep != NULL))
                    at = i;
            if (at < 0) {
                ok = 0;
                break;
            }
            a.a.sels[d] = (int16_t)(at > 32767 ? 32767 : at);
            snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%s%s", sub[0] ? "/" : "", name);
            if (!sep) {
                a.a.take = pl_keep(k, pl, sub);
                break;
            }
            folder = pl_keep(k, pl, sub);
            a.a.folders[++d] = folder;
            q = sep + 1;
        }
        (void)rel;
        if (ok)
            a.a.place = pl, a.a.depth = d;
    }
    answer(k, &a);
}
/* PEAKS: the pad's frames [zf, zf + zc) as n peaks, scaled as its whole wave (its peak: once per sound) */
static void do_peaks(kit_t *k, const req_t *r)
{
    ans_t a;
    uint32_t frames = 0, rate = 0, c, i;
    int16_t *pcm, top = 0;
    int shared = 1, st, n = r->n < 1 ? 1 : r->n > KIT_PEAKS ? KIT_PEAKS : r->n, t = 0;
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_PEAKS, a.a.pad = r->pad, a.a.zfrom = r->zf, a.a.zcount = r->zc;
    (void)top;
    if (!(pcm = shared_get(k, r->path, &frames, &rate)))
        shared = 0, pcm = load_sound(r->path, k->cfg.max_bytes, 0, &frames, &rate, &st);
    if (pcm && frames) {
        int j;
        pthread_mutex_lock(&k->mu);
        for (j = 0; j < NSH && shared; j++)
            if (k->sh[j].pcm == pcm)
                t = k->sh[j].top;
        pthread_mutex_unlock(&k->mu);
        if (!t) {                                       /* (its peak, as peaks_of scales the pad's wave) */
            for (i = 0; i < frames; i++) {
                const int l = abs(pcm[2 * i]), rr = abs(pcm[2 * i + 1]);
                t = l > t ? l : t, t = rr > t ? rr : t;
            }
            if (!t)
                t = 1;
            pthread_mutex_lock(&k->mu);
            for (j = 0; j < NSH && shared; j++)
                if (k->sh[j].pcm == pcm)
                    k->sh[j].top = t;
            pthread_mutex_unlock(&k->mu);
        }
        for (c = 0; c < (uint32_t)n; c++) {
            const uint64_t f0 = r->zf + (uint64_t)r->zc * c / (uint32_t)n;
            uint64_t f1 = r->zf + (uint64_t)r->zc * (c + 1) / (uint32_t)n, f;
            int p = 0;
            if (f1 <= f0)
                f1 = f0 + 1;
            for (f = f0; f < f1 && f < frames; f++) {
                const int l = abs(pcm[2 * f]), rr = abs(pcm[2 * f + 1]);
                p = l > p ? l : p, p = rr > p ? rr : p;
            }
            a.a.pk[c] = (uint8_t)(p * 255 / t > 255 ? 255 : p * 255 / t);
        }
        a.a.npk = n;
    }
    if (pcm) {
        int drop = 1;
        if (shared) {
            pthread_mutex_lock(&k->mu);
            drop = unref(k, pcm);
            pthread_mutex_unlock(&k->mu);
        }
        if (drop)
            free(pcm);
    }
    answer(k, &a);
}
/* ---- (stage 3b) a kit saved, opened; a folder onto the pads */
static const char *const PKEY[KIT_P_N] = { "pitch", "start", "end", "mode", "attack", "hold", "release", "choke",
                                           "reverse", "level", "pan", "mute", "stretch", "send1", "send2", "send3",
                                           "decay", "sustain", "cutoff", "reso",   /* (round B: these four) */
                                           "loop_start", "loop_end", "loop_fade", "penv_depth", "penv_attack",
                                           "penv_release", "bits", "downsample", "grain_pos", "grain_size",
                                           "grain_density", "grain_spray", "grain_spread", "warp", "speed", "warp_pitch",
                                           "warp_grain", "warp_jitter", "warp_search", "morph", "fenv", "fattack",
                                           "fdecay", "fsustain", "frelease" };   /* (pages v2: kit.json version 2) */
static int mkdirs(const char *path)                     /* the folder and its parents: 0 there */
{
    char p[KIT_PATH];
    size_t i;
    struct stat st;
    snprintf(p, sizeof p, "%s", path);
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            mkdir(p, 0755);
            p[i] = '/';
        }
    mkdir(p, 0755);
    return !stat(p, &st) && S_ISDIR(st.st_mode) ? 0 : -1;
}
static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    int bad = 0;
    if (!a)
        return -1;
    if (!(b = fopen(to, "wb"))) {
        fclose(a);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        bad |= fwrite(buf, 1, n, b) != n;
    bad |= ferror(a);
    fclose(a);
    bad |= fclose(b) != 0;
    if (bad)
        remove(to);
    return bad ? -1 : 0;
}
static void do_save(kit_t *k)
{
    ans_t a;
    kit_kit_t *x = malloc(sizeof *x);
    char dir[KIT_PATH], name[KIT_NAME], files[KIT_PADS][KIT_NAME + 8];
    char **names;
    int n, i, j, top = 0, ok = 0;
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_SAVED, a.a.status = KIT_FAILED;
    if (x) {
        pthread_mutex_lock(&k->mu);
        *x = k->save;
        pthread_mutex_unlock(&k->mu);
    }
    if (x && k->kits[0] && !mkdirs(k->kits)) {
        n = kits_list(k, &names);                       /* (Kit N: one past the highest) */
        for (i = 0; i < n; i++) {
            int v;
            char c;
            if (sscanf(names[i], "Kit %d%c", &v, &c) == 1 && v > top)
                top = v;
        }
        kits_free(names, n);
        {
            DIR *d = opendir(k->kits);                  /* (a "Kit N" folder with no kit.json: past it too) */
            struct dirent *e;
            while (d && (e = readdir(d))) {
                int v;
                char c;
                if (sscanf(e->d_name, "Kit %d%c", &v, &c) == 1 && v > top)
                    top = v;
            }
            if (d)
                closedir(d);
        }
        snprintf(name, sizeof name, "Kit %d", top + 1);
        snprintf(dir, sizeof dir, "%s/%s", k->kits, name);
        ok = !mkdirs(dir);
    }
    if (ok) {
        FILE *f;
        char tmp[KIT_PATH], fin[KIT_PATH], esc[256];
        for (i = 0; i < KIT_PADS; i++) {                /* the sounds copied (the same file once; a name taken by
                                                         * another file: the pad's number before it) */
            const char *b;
            files[i][0] = 0;
            if (!x->path[i][0])
                continue;
            for (j = 0; j < i; j++)
                if (!strcmp(x->path[j], x->path[i]) && files[j][0])
                    snprintf(files[i], sizeof files[i], "%s", files[j]);
            if (files[i][0])
                continue;
            b = strrchr(x->path[i], '/');
            b = b ? b + 1 : x->path[i];
            snprintf(files[i], sizeof files[i], "%.*s", KIT_NAME - 1, b);
            for (j = 0; j < i; j++)
                if (!strcmp(files[j], files[i])) {
                    snprintf(files[i], sizeof files[i], "%02d %.*s", i + 1, KIT_NAME - 1, b);
                    break;
                }
            snprintf(fin, sizeof fin, "%s/%s", dir, files[i]);
            {                                           /* (round C: a project's own sound: its file there) */
                char src[KIT_PATH];
                resolve_in(k->save_proj, x->path[i], src, sizeof src);
                if (copy_file(src, fin))
                    files[i][0] = 0;                    /* (gone: the pad saved empty) */
            }
        }
        snprintf(tmp, sizeof tmp, "%s/kit.json.tmp", dir);
        snprintf(fin, sizeof fin, "%s/kit.json", dir);
        if ((f = fopen(tmp, "w"))) {
            int bad;
            mj_escape(name, esc, sizeof esc);
            fprintf(f, "{\n  \"format\": \"rfm1-kit\", \"version\": 2, \"name\": \"%s\", \"layout\": %d,\n", esc, x->layout);
            fprintf(f, "  \"mix\": [%d, %d, %d, %d, %d], \"trim\": %d, \"bus\": [%d, %d],\n  \"pads\": [\n", x->mix[0],
                    x->mix[1], x->mix[2], x->mix[3], x->mix[4], x->trim ? 1 : 0, x->bus[0], x->bus[1]);   /* (round A:
                                                         * the Trim silence; pages v2: the kit bus's low-pass) */
            for (i = 0; i < KIT_PADS; i++) {
                mj_escape(files[i], esc, sizeof esc);
                fprintf(f, "    { \"file\": \"%s\"", esc);
                for (j = 0; j < KIT_P_N; j++)
                    fprintf(f, ", \"%s\": %d", PKEY[j], x->v[i][j]);
                fprintf(f, " }%s\n", i + 1 < KIT_PADS ? "," : "");
            }
            fprintf(f, "  ]\n}\n");
            bad = ferror(f);
            bad |= fclose(f) != 0;
            if (!bad && !rename(tmp, fin)) {
                a.a.status = KIT_OK;
                snprintf(a.a.name, sizeof a.a.name, "%s", name);
            } else
                remove(tmp);
        }
    }
    free(x);
    pthread_mutex_lock(&k->mu);
    k->save_busy = 0;
    pthread_mutex_unlock(&k->mu);
    answer(k, &a);
}
static char *read_all(const char *path, long cap)       /* a small file, NUL-terminated (NULL: none, too big) */
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > cap || fseek(f, 0, SEEK_SET) || !(b = malloc((size_t)n + 1))) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    b[n] = 0;
    fclose(f);
    return b;
}
static void do_open(kit_t *k, const req_t *r)
{
    ans_t *a = calloc(1, sizeof *a);
    char rel[256], path[KIT_PATH], *js = NULL;
    int found;
    if (!a)
        return;
    a->a.kind = KIT_A_KIT, a->a.status = KIT_FAILED;
    found = id_rel(k, r->id, rel, sizeof rel);
    if (!found) {                                       /* (3c: the page's id, a kit not listed here yet) */
        char **names;
        const int n = kits_list(k, &names);
        int i;
        for (i = 0; i < n && !found; i++)
            if (kit_id(names[i]) == r->id)
                snprintf(rel, sizeof rel, "kits:%s", names[i]), found = 1;
        kits_free(names, n);
    }
    if (found && !strncmp(rel, "kits:", 5) && k->kits[0]) {
        snprintf(path, sizeof path, "%s/%s/kit.json", k->kits, rel + 5);
        js = read_all(path, 1 << 20);
    }
    if (js) {
        kit_kit_t *x = &a->a.kit;
        const char *it = mj_find(js, "pads"), *e;
        long v;
        int i = 0, j, ver;
        snprintf(x->name, sizeof x->name, "%s", rel + 5);   /* (its folder's name: renamed there, renamed here) */
        ver = !mj_int(js, "version", &v) ? (int)v : 1;
        x->layout = !mj_int(js, "layout", &v) ? (int)v : 0;
        if (ver < 2)                                    /* (pages v2) the right half: In Key as In Key 4ths, Off as
                                                         * Pitches */
            x->layout = x->layout == 1 ? 3 : 0;
        x->trim = !mj_int(js, "trim", &v) ? v != 0 : 1;   /* (round A; an older kit.json: on) */
        x->mix[0] = 100, x->mix[1] = 64;
        if ((e = mj_find(js, "mix")) && *e == '[') {
            const char *m = e, *y;
            for (j = 0; j < 5 && (y = mj_elem(&m)); j++)
                x->mix[j] = (uint8_t)clampi((int)strtol(y, NULL, 10), 0, 127);
        }
        for (i = 0; i < KIT_PADS; i++) {                /* (round B: a kit.json without them: Sustain 100, open; pages
                                                         * v2: as the core's defaults) */
            x->v[i][KIT_P_END] = KIT_FRAC, x->v[i][KIT_P_SUSTAIN] = 100, x->v[i][KIT_P_CUTOFF] = 100;
            x->v[i][KIT_P_LOOPE] = KIT_FRAC, x->v[i][KIT_P_PRELEASE] = 100, x->v[i][KIT_P_BITS] = 16;
            x->v[i][KIT_P_DOWNS] = 1, x->v[i][KIT_P_GSIZE] = 80, x->v[i][KIT_P_GDENS] = 4, x->v[i][KIT_P_SPEED] = 100;
            x->v[i][KIT_P_WGRAIN] = 50, x->v[i][KIT_P_WSEARCH] = 10, x->v[i][KIT_P_FDECAY] = 300;
            x->v[i][KIT_P_FRELEASE] = 300;
        }
        x->bus[0] = 100, x->bus[1] = 0;
        if ((e = mj_find(js, "bus")) && *e == '[') {   /* (pages v2) */
            const char *m = e, *y;
            for (j = 0; j < 2 && (y = mj_elem(&m)); j++)
                x->bus[j] = (uint8_t)clampi((int)strtol(y, NULL, 10), 0, 100);
        }
        if (it && *it != '[')
            it = NULL;
        for (i = 0; it && i < KIT_PADS && (e = mj_elem(&it)); i++) {
            char file[256];
            for (j = 0; j < KIT_P_N; j++)
                if (!mj_int(e, PKEY[j], &v))
                    x->v[i][j] = (int16_t)clampi((int)v, -32768, 32767);
            if (ver < 2) {                              /* (pages v2) an older kit.json: Toggle as Loop, Stretch as Warp
                                                         * Repitch */
                if (x->v[i][KIT_P_MODE] == 3)
                    x->v[i][KIT_P_MODE] = KIT_M_LOOP;
                if (x->v[i][KIT_P_STRETCH])
                    x->v[i][KIT_P_WARP] = KIT_W_REPITCH;
            }
            if (!mj_string(e, "file", file, sizeof file) && file[0] && !strchr(file, '/') && strcmp(file, "..")) {
                char src[KIT_PATH], cp[KIT_PATH], cpath[KIT_PATH];
                snprintf(src, sizeof src, "%s/%s/%s", k->kits, rel + 5, file);
                own_copy(k, r->proj, src, cp, sizeof cp);   /* (round C: into the project's folder: editing it never
                                                         * changes the saved kit) */
                core_path(r->proj, cp, cpath, sizeof cpath);
                if (strlen(cpath) < KIT_KPATH)
                    snprintf(x->path[i], sizeof x->path[i], "%s", cpath);
            }
        }
        a->a.status = KIT_OK;
        free(js);
    }
    answer(k, a);
    free(a);
}
char *kit_kits_json(const kit_t *k)
{
    char **names;
    const int n = k ? kits_list(k, &names) : 0;
    size_t cap = 32 + (size_t)n * (KIT_NAME * 2 + 40), at = 0;
    char *o = malloc(cap);
    int i;
    if (!o) {
        if (n)
            kits_free(names, n);
        return NULL;
    }
    at += (size_t)snprintf(o, cap, "{\"kits\":[");
    for (i = 0; i < n; i++) {
        char esc[KIT_NAME * 2 + 8];
        mj_escape(names[i], esc, sizeof esc);
        at += (size_t)snprintf(o + at, cap - at, "%s{\"id\":%u,\"name\":\"%s\"}", i ? "," : "", kit_id(names[i]), esc);
    }
    snprintf(o + at, cap - at, "]}");
    if (n)
        kits_free(names, n);
    return o;
}
static int cmp_name(const void *a, const void *b) { return natcmp(*(const char *const *)a, *(const char *const *)b); }
static void do_folder(kit_t *k, const req_t *r)
{
    char rel[256] = "", dir[KIT_PATH], **names = NULL;
    DIR *d = NULL;
    struct dirent *e;
    int n = 0, cap = 0, i;
    if ((r->place == KIT_PL_SAMPLES || r->place == KIT_PL_CORE) && pl_base(k, r->place)[0] &&
        (!r->id || pl_rel(k, r->place, r->id, rel, sizeof rel))) {
        snprintf(dir, sizeof dir, "%s%s%s", pl_base(k, r->place), rel[0] ? "/" : "", rel);
        d = opendir(dir);
    }
    while (d && (e = readdir(d))) {                     /* its sounds (no folders), by name */
        char p[KIT_PATH];
        struct stat st;
        if (e->d_name[0] == '.' || !sound_file(e->d_name))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode))
            continue;
        if (r->place == KIT_PL_CORE) {                  /* (the Move Library: the sounds this host reads) */
            char sub[256];
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
            cx_build(k);
            if (!in_set(k->cx_f, k->cx_nf, sub))
                continue;
        }
        if (n == cap) {
            char **g = realloc(names, sizeof *names * (size_t)(cap = cap ? cap * 2 : 64));
            if (!g)
                break;
            names = g;
        }
        if ((names[n] = strdup(e->d_name)))
            n++;
    }
    if (d)
        closedir(d);
    if (n)
        qsort(names, (size_t)n, sizeof names[0], cmp_name);
    for (i = 0; i < KIT_PADS; i++) {                    /* the first 16 onto pads 1..16, the rest of the pads emptied */
        req_t q;
        memset(&q, 0, sizeof q);
        q.kind = RQ_LOAD, q.pad = i, q.own = 1;      /* (round C: kept: copied in) */
        snprintf(q.proj, sizeof q.proj, "%s", r->proj);
        if (i < n) {
            snprintf(q.path, sizeof q.path, "%s/%s", dir, names[i]);
            do_load(k, &q);
        } else {
            ans_t a;
            memset(&a, 0, sizeof a);
            a.a.kind = KIT_A_LOADED, a.a.pad = i, a.a.status = KIT_EMPTY;
            answer(k, &a);
        }
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
}
/* ---- (stage 3c) making sounds: mix and join, a pad's own edits, a take's slices */
/* a 16-bit stereo WAV written beside (.tmp) and renamed into place: 0 done */
static int write_wav(const char *path, const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    char tmp[KIT_PATH + 8];
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
/* a new file's path in the Edits folder: name, else "name 2", "name 3" ..; 0 found (and the folder made) */
static int new_path(kit_t *k, const char *proj, const char *name, char *path, size_t cap)
{
    struct stat st;
    const char *dir = proj[0] ? proj : k->edits;        /* (round C: the project's own folder; none: Kits/Edits) */
    int i;
    if (!dir[0] || mkdirs(dir))
        return -1;
    for (i = 1; i < 1000; i++) {
        if (i == 1)
            snprintf(path, cap, "%s/%s.wav", dir, name);
        else
            snprintf(path, cap, "%s/%s %d.wav", dir, name, i);
        if (stat(path, &st))
            return 0;
    }
    return -1;
}
/* a file's region [r[0], r[1]) of KIT_FRAC into memory (cap: the kit's bytes): its frames, rate; NULL with *st */
static int16_t *read_region(kit_t *k, const char *path, const int r[2], uint32_t *n, uint32_t *rate, int *st)
{
    uint32_t frames, a, b;
    int16_t *p = load_sound(path, k->cfg.max_bytes, 0, &frames, rate, st);
    if (!p)
        return NULL;
    a = (uint32_t)((uint64_t)frames * (uint32_t)clampi(r[0], 0, KIT_FRAC) / KIT_FRAC);
    b = (uint32_t)((uint64_t)frames * (uint32_t)clampi(r[1], 0, KIT_FRAC) / KIT_FRAC);
    if (b > frames)
        b = frames;
    if (b <= a)
        b = a + 1 <= frames ? a + 1 : frames, a = b - 1;
    memmove(p, p + 2 * a, (size_t)(b - a) * 4u);
    *n = b - a;
    return p;
}
/* b (rate rb) at rate ra, linear: a new buffer (*n its frames), b freed */
static int16_t *resample(int16_t *b, uint32_t *n, uint32_t rb, uint32_t ra)
{
    uint64_t m;
    int16_t *o;
    uint32_t i;
    if (rb == ra || !rb || !ra)
        return b;
    m = (uint64_t)*n * ra / rb;
    if (!m || m > 0x7FFFFFFF || !(o = malloc((size_t)m * 4u))) {
        free(b);
        return NULL;
    }
    for (i = 0; i < m; i++) {
        const double x = (double)i * rb / ra;
        const uint32_t j = (uint32_t)x, j1 = j + 1 < *n ? j + 1 : j;
        const double f = x - j;
        o[2 * i] = (int16_t)lrint(b[2 * j] + (b[2 * j1] - b[2 * j]) * f);
        o[2 * i + 1] = (int16_t)lrint(b[2 * j + 1] + (b[2 * j1 + 1] - b[2 * j + 1]) * f);
    }
    free(b);
    *n = (uint32_t)m;
    return o;
}
static int peak_of(const int16_t *p, uint32_t frames)
{
    uint32_t i;
    int top = 0;
    for (i = 0; i < 2 * frames; i++)
        top = abs(p[i]) > top ? abs(p[i]) : top;
    return top;
}
/* MERGE (Mix: the first's region plus the second's, scaled under full scale if it went past; Join: the first's then
 * the second's, 2 ms faded at the seam) and EDIT (Trim: the region; Normalize: the peak at KIT_NORM_PEAK; Reverse):
 * a new file in Edits, onto the pad (LOADED) */
static void do_make(kit_t *k, const req_t *r)
{
    static const char *const ED[4] = { "", "trim", "norm", "rev" };
    ans_t a;
    int16_t *x = NULL, *y = NULL, *o = NULL;
    uint32_t nx = 0, ny = 0, rx = 0, ry = 0, no = 0, i;
    int st = KIT_FAILED;
    char name[KIT_NAME + 16], path[KIT_PATH];
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_LOADED, a.a.pad = r->pad, a.a.status = KIT_FAILED;
    if (r->kind == RQ_MERGE) {
        if ((x = read_region(k, r->path, r->r0, &nx, &rx, &st)) && (y = read_region(k, r->path2, r->r1, &ny, &ry, &st)) &&
            (y = resample(y, &ny, ry, rx))) {
            no = r->op == KIT_MG_JOIN ? nx + ny : nx > ny ? nx : ny;
            if ((uint64_t)no * 4u > room_for(k, r->pad))
                st = KIT_TOO_LONG;
            else if ((o = malloc((size_t)no * 4u))) {
                if (r->op == KIT_MG_JOIN) {
                    const uint32_t fd = rx / 500 ? rx / 500 : 1;   /* (2 ms each side of the seam) */
                    memcpy(o, x, (size_t)nx * 4u), memcpy(o + 2 * nx, y, (size_t)ny * 4u);
                    for (i = 0; i < fd && i < nx; i++) {
                        const double g = (double)i / fd;
                        o[2 * (nx - 1 - i)] = (int16_t)lrint(o[2 * (nx - 1 - i)] * g);
                        o[2 * (nx - 1 - i) + 1] = (int16_t)lrint(o[2 * (nx - 1 - i) + 1] * g);
                    }
                    for (i = 0; i < fd && i < ny; i++) {
                        const double g = (double)i / fd;
                        o[2 * (nx + i)] = (int16_t)lrint(o[2 * (nx + i)] * g);
                        o[2 * (nx + i) + 1] = (int16_t)lrint(o[2 * (nx + i) + 1] * g);
                    }
                    snprintf(name, sizeof name, "Join %d+%d", r->pad2 + 1, r->pad + 1);
                } else {
                    int32_t top = 0;
                    double g = 1.0;
                    for (i = 0; i < 2 * no; i++) {
                        const int32_t v = (i < 2 * nx ? x[i] : 0) + (i < 2 * ny ? y[i] : 0);
                        top = abs(v) > top ? abs(v) : top;
                    }
                    if (top > 32767)                    /* (past full scale: the whole under it, never clipped) */
                        g = (double)KIT_NORM_PEAK / top;
                    for (i = 0; i < 2 * no; i++)
                        o[i] = (int16_t)lrint(((i < 2 * nx ? x[i] : 0) + (i < 2 * ny ? y[i] : 0)) * g);
                    snprintf(name, sizeof name, "Mix %d+%d", r->pad2 + 1, r->pad + 1);
                }
            }
        }
        free(x), free(y);
    } else if (r->op >= KIT_ED_TRIM && r->op <= KIT_ED_REVERSE) {
        static const int whole[2] = { 0, KIT_FRAC };
        char b[KIT_NAME];
        if ((o = read_region(k, r->path, r->op == KIT_ED_TRIM ? r->r0 : whole, &no, &rx, &st))) {
            if (r->op == KIT_ED_NORMALIZE) {
                const int top = peak_of(o, no);
                if (top > 0)
                    for (i = 0; i < 2 * no; i++)
                        o[i] = (int16_t)lrint((double)o[i] * KIT_NORM_PEAK / top);
            } else if (r->op == KIT_ED_REVERSE)
                for (i = 0; i < no / 2; i++) {
                    int16_t t[2];
                    memcpy(t, o + 2 * i, 4), memcpy(o + 2 * i, o + 2 * (no - 1 - i), 4), memcpy(o + 2 * (no - 1 - i), t, 4);
                }
            if ((uint64_t)no * 4u > room_for(k, r->pad))
                free(o), o = NULL, st = KIT_TOO_LONG;
        }
        base_name(r->path, b, 16);
        snprintf(name, sizeof name, "%s %s", b, ED[r->op]);
        (void)whole;
    }
    if (o && no) {
        if (!new_path(k, r->proj, name, path, sizeof path) && !write_wav(path, o, no, rx)) {
            a.pcm = o, a.a.status = KIT_OK, a.a.frames = no, a.a.rate = rx;
            core_path(r->proj, path, a.a.path, sizeof a.a.path);
            snprintf(a.abs, sizeof a.abs, "%s", path);
            snprintf(k->wlast[r->pad], sizeof k->wlast[0], "%s", path);
            base_name(path, a.a.name, sizeof a.a.name);
            a.a.npk = peaks_of(o, no, KIT_PEAKS, a.a.pk);
            shared_put(k, path, o, no, rx);
        } else
            free(o), a.a.status = KIT_FAILED;
    } else
        a.a.status = st == KIT_OK ? KIT_FAILED : st;
    answer(k, &a);
}
static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
int kit_cuts(const int16_t *pcm, uint32_t frames, uint32_t rate, int method, uint32_t step_us, uint32_t *cuts)
{
    return kit_cuts_ex(pcm, frames, rate, method, step_us, 50, KIT_HIT_GAP_MS, cuts);
}
int kit_cuts_ex(const int16_t *pcm, uint32_t frames, uint32_t rate, int method, uint32_t step_us, int sens, int gap_ms,
                uint32_t *cuts)
{
    int n = 0, i;
    if (!frames)
        return 0;
    if (!rate)
        rate = 48000;
    if (method == KIT_SM_TAP) {                         /* (round B) Tap: the core's taps; the whole take first */
        cuts[0] = 0, cuts[1] = frames;
        return 1;
    }
    if (method == KIT_SM_STEPS && step_us) {            /* a slice a step at the tempo */
        const uint64_t st = (uint64_t)step_us * rate / 1000000u;
        if (st)
            for (n = 0; n < KIT_SLICES && (uint64_t)n * st < frames; n++)
                cuts[n] = (uint32_t)((uint64_t)n * st);
        if (n) {
            cuts[n] = (uint64_t)n * st < frames ? (uint32_t)((uint64_t)n * st) : frames;
            return n;
        }
        method = KIT_SM_EQ16;
    }
    if (method == KIT_SM_EQ16 || method == KIT_SM_EQ8 || method == KIT_SM_EQ4) {   /* equal parts */
        n = method == KIT_SM_EQ16 ? 16 : method == KIT_SM_EQ8 ? 8 : 4;
        if ((uint32_t)n > frames)
            n = (int)frames;
        for (i = 0; i <= n; i++)
            cuts[i] = (uint32_t)((uint64_t)frames * (uint32_t)i / (uint32_t)n);
        return n;
    }
    {   /* the hits: 5 ms windows' level; a window louder than 2x the 40 ms before it (and a tenth of the loudest) starts
         * one, 60 ms after the last at least; the 16 strongest kept (the first always), in their order: a slice from each
         * hit to the next (the last to the take's end; before the first: not a slice). (round A) The sensitivity
         * (1..100, 50 as before) scales the rise (2x at 50: 1.2x at 100, 2.8x at 1) and the floor (a tenth at 50: none
         * at 100, a fifth at 1); gap_ms the spacing (60 as before) */
        const uint32_t hop = rate / 200 ? rate / 200 : 1, nh = (frames + hop - 1) / hop;
        const int sv = sens < 1 ? 1 : sens > 100 ? 100 : sens;
        const uint32_t gap = (uint32_t)((gap_ms < 5 ? 5 : gap_ms) / 5);
        const float rise = 1.2f + 1.6f * (float)(100 - sv) / 100.0f, floor_ = 0.2f * (float)(100 - sv) / 100.0f;
        float *e = malloc(sizeof(float) * nh);
        struct { uint32_t at; float s; } on[256];
        int no = 0;
        float top = 0.0f;
        uint32_t j, last = 0;
        int have = 0;
        if (!e)
            return 0;
        for (j = 0; j < nh; j++) {
            const uint32_t a = j * hop, b = a + hop < frames ? a + hop : frames;
            uint32_t q;
            double sum = 0;
            for (q = a; q < b; q++)
                sum += abs(pcm[2 * q]) + abs(pcm[2 * q + 1]);
            e[j] = (float)(sum / (2.0 * (b - a)));
            top = e[j] > top ? e[j] : top;
        }
        for (j = 0; j < nh && top > 0.0f; j++) {
            float before = 0.0f;
            uint32_t q, c = 0;
            for (q = j >= 8 ? j - 8 : 0; q < j; q++)
                before += e[q], c++;
            before = c ? before / c : 0.0f;
            if (e[j] >= top * floor_ && e[j] > rise * before + top * 0.02f && (!have || j - last >= gap)) {
                if (no < (int)(sizeof on / sizeof on[0]))
                    on[no].at = j * hop, on[no].s = e[j] - before, no++;
                last = j, have = 1;
            }
        }
        free(e);
        while (no > KIT_SLICES) {                       /* (the weakest out, the first kept) */
            int w = 1;
            for (i = 2; i < no; i++)
                if (on[i].s < on[w].s)
                    w = i;
            memmove(&on[w], &on[w + 1], sizeof on[0] * (size_t)(no - w - 1));
            no--;
        }
        if (!no) {                                      /* (no hit: the whole take one slice) */
            cuts[0] = 0, cuts[1] = frames;
            return 1;
        }
        for (i = 0, n = 0; i < no && n < KIT_SLICES; i++)
            cuts[n++] = on[i].at;
        qsort(cuts, (size_t)n, sizeof cuts[0], cmp_u32);
        cuts[n] = frames;
        return n;
    }
}
/* SLICE: the take read, its cuts by the method; the take kept as the preview's sound for its slices */
static void do_slice(kit_t *k, const req_t *r)
{
    ans_t a;
    char path[KIT_PATH], nm[KIT_NAME];
    memset(&a, 0, sizeof a);
    a.preview = 2, a.gen = r->gen;
    a.a.kind = KIT_A_SLICES, a.a.take = r->id;
    if (row_path(k, KIT_PL_TAKES, r->id, path, sizeof path, nm, sizeof nm, NULL) &&
        (a.pcm = load_sound(path, k->cfg.max_bytes, 0, &a.a.frames, &a.a.rate, &a.a.status))) {
        a.a.n = kit_cuts_ex(a.pcm, a.a.frames, a.a.rate, r->op, r->us, r->n ? r->n : 50, KIT_HIT_GAP_MS, a.a.cuts);
        a.a.npk = peaks_of(a.pcm, a.a.frames, KIT_PEAKS, a.a.pk);
        snprintf(a.a.path, sizeof a.a.path, "%s", path);
    }
    answer(k, &a);
}
static void do_freeze(kit_t *k, const req_t *r);
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
        for (i = 0; i < ng; i++)                        /* (3c: a shared sound goes with its last holder) */
            if (!unref(k, g[i]))
                g[i] = NULL;
        pthread_mutex_unlock(&k->mu);
        for (i = 0; i < ng; i++)
            free(g[i]);
        switch (r.kind) {
        case RQ_LOAD:
        case RQ_LOAD_ROW: do_load(k, &r); break;
        case RQ_ROWS: do_rows(k, &r); break;
        case RQ_SAVE: do_save(k); break;
        case RQ_OPEN: do_open(k, &r); break;
        case RQ_FOLDER: do_folder(k, &r); break;
        case RQ_MERGE: do_make(k, &r); break;
        case RQ_EDIT:
            if (r.op == KIT_ED_FREEZE)
                do_freeze(k, &r);                       /* (round C) */
            else
                do_make(k, &r);
            break;
        case RQ_KEEP: do_keep(k, &r); break;
        case RQ_SLICE: do_slice(k, &r); break;
        case RQ_FIND: do_find(k, &r); break;             /* (round A) */
        case RQ_PEAKS: do_peaks(k, &r); break;
        default:
            pthread_mutex_lock(&k->mu);
            i = k->clean_due;
            pthread_mutex_unlock(&k->mu);
            if (i) {                                    /* (round C: after the requests before it) */
                do_clean(k);
                break;
            }
            return;                                     /* (nothing more) */
        }
    }
}
static void *own_thread(void *arg)
{
    kit_t *k = arg;
    pthread_mutex_lock(&k->mu);
    while (!k->quit) {
        if (!k->rqn && !k->ngarb && !k->clean_due) {
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
    snprintf(k->kits, sizeof k->kits, "%s", cfg->kits_dir ? cfg->kits_dir : "");
    if (cfg->core_dir && cfg->core_dir[0]) {            /* (2026-11-26) the Move Library: its Samples/ when there */
        struct stat st;
        char sm[KIT_PATH];
        snprintf(sm, sizeof sm, "%s/Samples", cfg->core_dir);
        if (!stat(sm, &st) && S_ISDIR(st.st_mode))
            snprintf(k->core, sizeof k->core, "%s", sm);
        else if (!stat(cfg->core_dir, &st) && S_ISDIR(st.st_mode))
            snprintf(k->core, sizeof k->core, "%s", cfg->core_dir);
    }
    if (k->kits[0])                                     /* (3c) the new files: Kits/Edits */
        snprintf(k->edits, sizeof k->edits, "%s/Edits", k->kits);
    k->mix[0] = 127, k->mix[1] = 64;
    k->master = 1.0f;                    /* (unity until the core's MIX says) */
    for (p = 0; p <= SLP; p++)
        k->v[p][KIT_P_END] = KIT_FRAC, k->v[p][KIT_P_SUSTAIN] = 100, k->v[p][KIT_P_CUTOFF] = 100;
    k->glide = (float)(1.0 - exp(-(double)FLT_N / ((double)k->cfg.fs * FLT_GLIDE_MS / 1000.0)));
    for (p = 0; p <= SLP; p++)                          /* (pages v2: as the core's defaults) */
        k->v[p][KIT_P_LOOPE] = KIT_FRAC, k->v[p][KIT_P_BITS] = 16, k->v[p][KIT_P_DOWNS] = 1, k->v[p][KIT_P_GSIZE] = 80,
        k->v[p][KIT_P_GDENS] = 4, k->v[p][KIT_P_SPEED] = 100, k->v[p][KIT_P_WGRAIN] = 50, k->v[p][KIT_P_WSEARCH] = 10;
    for (p = 0; p <= 1024; p++)                         /* (pages v2) the grains' window */
        k->hann[p] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * p / 1024.0));
    k->bus_c = 100;
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
    for (p = 0; p < KIT_PADS; p++)                      /* (3c: a shared sound freed once) */
        if (unref(k, k->snd[p].pcm))
            free(k->snd[p].pcm);
    free(k->prev.pcm);
    for (p = 0; p < k->cx_nd; p++)
        free(k->cx_d[p]);
    for (p = 0; p < k->cx_nf; p++)
        free(k->cx_f[p]);
    free(k->cx_d), free(k->cx_f);
    while (k->ann) {
        if (unref(k, k->an[k->anh].pcm))
            free(k->an[k->anh].pcm);
        k->anh = (k->anh + 1) % NANS, k->ann--;
    }
    for (p = 0; p < k->ngarb; p++)
        if (unref(k, k->garb[p]))
            free(k->garb[p]);
    free(k->ls);
    free(k->an);
    pthread_cond_destroy(&k->cv);
    pthread_mutex_destroy(&k->mu);
    free(k);
}

/* ================================================================ the engine's thread === */
static void load_as(kit_t *k, int pad, const char *path, int own, int trim)
{
    static req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_LOAD, r.pad = pad, r.own = own, r.trim = trim != 0;
    kit_resolve(k, path ? path : "", r.path, sizeof r.path);   /* (round C: KIT_OWN.. the project's folder's) */
    request(k, &r);
}
void kit_load(kit_t *k, int pad, const char *path) { load_as(k, pad, path, 0, 0); }
void kit_load_own(kit_t *k, int pad, const char *path) { load_as(k, pad, path, 1, 0); }
/* (round C: a take recorded into a pad: copied into the project's folder, the take library keeps its own) */
void kit_load_rec(kit_t *k, int pad, const char *path, int trim) { load_as(k, pad, path, 1, trim); }
void kit_find(kit_t *k, int pad, const char *path)
{
    static req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS || !path || !path[0])
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_FIND, r.pad = pad;
    kit_resolve(k, path, r.path, sizeof r.path);
    request(k, &r);
}
void kit_project(kit_t *k, const char *dir)
{
    if (k)
        snprintf(k->proj, sizeof k->proj, "%s", dir ? dir : "");
}
void kit_keep(kit_t *k, int pad)
{
    static req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_KEEP, r.pad = pad;
    request(k, &r);
}
void kit_clean(kit_t *k, const char *const *refs, int n)
{
    int i, j;
    char f[KIT_PATH];
    if (!k || !k->proj[0])
        return;
    pthread_mutex_lock(&k->mu);
    snprintf(k->clean_dir, sizeof k->clean_dir, "%s", k->proj);
    k->clean_n = 0;
    for (i = 0; i < n + KIT_PADS && k->clean_n < 64; i++) {   /* (the core's paths, then what the pads hold) */
        const char *p = i < n ? refs[i] : k->ppath[i - n];
        if (!p || !p[0])
            continue;
        resolve_in(k->proj, p, f, sizeof f);
        if (!in_dir(k->proj, f))
            continue;
        for (j = 0; j < k->clean_n && strcmp(k->clean_keep[j], strrchr(f, '/') + 1); j++)
            ;
        if (j == k->clean_n)
            snprintf(k->clean_keep[k->clean_n++], sizeof k->clean_keep[0], "%s", strrchr(f, '/') + 1);
    }
    k->clean_due = 1;
    k->posted = 0;                                      /* (called for now) */
    wake(k);
    pthread_mutex_unlock(&k->mu);
}
void kit_zoom(kit_t *k, int pad, uint32_t from, uint32_t count, int n)
{
    req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS || !k->ppath[pad][0] || !count)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_PEAKS, r.pad = pad, r.zf = from, r.zc = count, r.n = n;
    snprintf(r.path, sizeof r.path, "%s", k->ppath[pad]);
    request(k, &r);
}
void kit_audition(kit_t *k, int pad, uint32_t from, uint32_t count)   /* (2026-11-26: retired; nothing plays) */
{
    (void)k, (void)pad, (void)from, (void)count;
}
int kit_auditioning(const kit_t *k) { (void)k; return 0; }
static void load_row_as(kit_t *k, int pad, int place, uint32_t id, int own)
{
    static req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_LOAD_ROW, r.pad = pad, r.place = place, r.id = id, r.own = own;
    request(k, &r);
}
void kit_load_row(kit_t *k, int pad, int place, uint32_t id) { load_row_as(k, pad, place, id, 0); }
void kit_load_row_own(kit_t *k, int pad, int place, uint32_t id) { load_row_as(k, pad, place, id, 1); }
void kit_rows(kit_t *k, int place, uint32_t folder, int from, int n)
{
    req_t r;
    if (!k)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_ROWS, r.place = place, r.id = folder, r.from = from < 0 ? 0 : from, r.n = n;
    request(k, &r);
}
void kit_preview(kit_t *k, int place, uint32_t id, int on)   /* (2026-11-26: retired; nothing plays by itself) */
{
    (void)k, (void)place, (void)id, (void)on;
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
        if (a.preview == 2) {                           /* (3c) a take's slices: its sound kept for them (older: dropped) */
            if (a.gen != k->sl_gen) {
                garbage(k, a.pcm);
                continue;
            }
            voice_kill_pad(k, SLP);                     /* (its voices with the take before) */
            memset(&k->snd[SLP], 0, sizeof k->snd[SLP]);
            garbage(k, k->prev.pcm);
            k->prev.pcm = a.pcm, k->prev.frames = a.pcm ? a.a.frames : 0, k->prev.rate = a.a.rate;
            k->sl_take = a.pcm ? a.a.take : 0;
            if (!a.pcm)
                a.a.n = 0;
            *out = a.a;
            return 1;
        }
        if (a.preview) {                                /* (none asked any more) */
            garbage(k, a.pcm);
            continue;
        }
        if (a.a.kind == KIT_A_LOADED && (a.pcm || (a.a.status == KIT_OK && !a.a.path[0]) || a.a.status != KIT_OK)) {
            snd_t *s = &k->snd[a.a.pad];
            if (a.pcm || a.a.status == KIT_OK || a.a.status == KIT_EMPTY) {   /* (a sound, or emptied: the old one
                                                         * goes) */
                voice_kill_pad(k, a.a.pad);
                garbage(k, s->pcm);
                s->pcm = a.pcm, s->frames = a.pcm ? a.a.frames : 0, s->rate = a.pcm ? a.a.rate : 0;
                snprintf(k->ppath[a.a.pad], sizeof k->ppath[0], "%s", a.pcm ? (a.abs[0] ? a.abs : a.a.path) : "");
                pthread_mutex_lock(&k->mu);             /* (3c: what it holds, for the memory's count) */
                k->used[a.a.pad] = a.pcm ? a.a.frames * 4u : 0;
                k->held[a.a.pad] = a.pcm;
                pthread_mutex_unlock(&k->mu);
            }
        }
        if (a.a.kind == KIT_A_OWNED && !strcmp(k->ppath[a.a.pad], a.abs2))   /* (round C: the same sound, its copy) */
            snprintf(k->ppath[a.a.pad], sizeof k->ppath[0], "%s", a.abs);
        *out = a.a;
        return 1;
    }
}
/* ---- (round B) the pad's low-pass: a TPT state variable filter (Zavalishin / Simper). (pages v2) Its output morphs
 * LP -> BP -> HP (BP scaled to unity at its peak), its envelope moves the cutoff, the kit bus has one too */
static float flt_g(const kit_t *k, int v)              /* tan(pi fc / fs), fc 20 Hz .. 20 kHz (0.45 fs at most) */
{
    double fc = 20.0 * pow(1000.0, clampi(v, 0, 100) / 100.0);
    if (fc > 0.45 * k->cfg.fs)
        fc = 0.45 * k->cfg.fs;
    return (float)tan(3.14159265358979323846 * fc / k->cfg.fs);
}
static float flt_k(int r) { return 2.0f - 1.94f * (float)clampi(r, 0, 100) / 100.0f; }   /* 1 / Q: 0.5 .. 16 */
static void flt_coefs(float g, float kq, float *a1, float *a2, float *a3)
{
    *a1 = 1.0f / (1.0f + g * (g + kq)), *a2 = g * *a1, *a3 = g * *a2;
}
/* (pages v2) the filter needed: not open, or an envelope or a morph on it */
static int flt_needed(const voice_t *v, int c) { return c < 100 || v->fe_amt != 0.0f || v->morph > 0.0f; }
/* a voice's filter aimed at cutoff c, resonance r (c 100: open, the filter off once it has glided there) */
static void flt_aim(kit_t *k, voice_t *v, int c, int r, int start)
{
    const float go = flt_g(k, 100);
    v->cut_v = clampi(c, 0, 100);                       /* (pages v2: the screen's live bars) */
    v->kq = flt_k(r);
    v->g_tgt = c >= 100 ? go : flt_g(k, c);
    if (start) {                                        /* (a note-on: at once, from silence) */
        v->flt = flt_needed(v, c);
        v->g = v->g_tgt, v->fn = FLT_N, v->da1 = v->da2 = v->da3 = 0;
        flt_coefs(v->g, v->kq, &v->a1, &v->a2, &v->a3);
        v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0;
        if (v->flt && (v->fe_amt != 0.0f))
            v->fn = 0;                                  /* (the envelope's first value at once) */
    } else if (!v->flt && flt_needed(v, c)) {           /* (turned down while it sounds: from open) */
        v->flt = 1, v->g = go, v->fn = 0, v->da1 = v->da2 = v->da3 = 0;
        flt_coefs(v->g, v->kq, &v->a1, &v->a2, &v->a3);
        v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0;
    }
}
/* (pages v2) the filter envelope a block of FLT_N frames on: its level 0..1 */
static void fenv_next(voice_t *v)
{
    switch (v->fe_st) {
    case 0:
        v->fe_lvl = v->fe_a ? v->fe_lvl + 1.0f / (float)v->fe_a : 1.0f;
        if (v->fe_lvl >= 1.0f)
            v->fe_lvl = 1.0f, v->fe_st = 1, v->fe_t = 0;
        break;
    case 1:
        if (++v->fe_t >= v->fe_d)
            v->fe_lvl = v->fe_s, v->fe_st = 2;
        else
            v->fe_lvl = 1.0f + (v->fe_s - 1.0f) * (float)v->fe_t / (float)v->fe_d;
        break;
    case 3:
        if (++v->fe_t >= v->fe_r)
            v->fe_lvl = 0.0f, v->fe_st = 4;
        else
            v->fe_lvl = v->fe_from * (1.0f - (float)v->fe_t / (float)v->fe_r);
        break;
    default: break;
    }
}
/* (every FLT_N frames) the cutoff glides; the coefficients' next targets, ramped to over the next FLT_N frames */
static void flt_next(const kit_t *k, voice_t *v)
{
    float t1, t2, t3, g;
    int c;
    v->g += (v->g_tgt - v->g) * k->glide;
    if (v->fe_amt == 0.0f && v->morph <= 0.0f && v->g_tgt >= flt_g(k, 100) * 0.9999f && v->g >= v->g_tgt * 0.999f) {
        v->flt = 0;                                     /* (open again: bypassed) */
        return;
    }
    g = v->g;
    if (v->fe_amt != 0.0f) {                            /* (pages v2) the envelope: the cutoff times 2^(octaves) */
        double fc = atan((double)g) / 3.14159265358979323846 * exp2((double)(v->fe_amt * v->fe_lvl));
        fc = fc < 10.0 / k->cfg.fs ? 10.0 / k->cfg.fs : fc > 0.45 ? 0.45 : fc;
        g = (float)tan(3.14159265358979323846 * fc);
    }
    flt_coefs(g, v->kq, &t1, &t2, &t3);
    v->da1 = (t1 - v->a1) / FLT_N, v->da2 = (t2 - v->a2) / FLT_N, v->da3 = (t3 - v->a3) / FLT_N;
    v->fn = FLT_N;
    for (c = 0; c < 2; c++) {                           /* (no denormals in a quiet tail) */
        if (fabsf(v->ic1[c]) < 1e-15f)
            v->ic1[c] = 0;
        if (fabsf(v->ic2[c]) < 1e-15f)
            v->ic2[c] = 0;
    }
}
static void retune(kit_t *k, voice_t *v);
void kit_set(kit_t *k, int pad, int param, int value)
{
    int i;
    if (!k || pad < 0 || pad >= KIT_PADS || param < 0 || param >= KIT_P_N)
        return;
    k->v[pad][param] = (int16_t)value;
    for (i = 0; i < NV; i++) {
        voice_t *o = &k->vo[i];
        if (!o->on || o->pad != pad || (o->pm >> param & 1))
            continue;                                   /* (2026-11-26: a hit's lock stays) */
        if ((param == KIT_P_CUTOFF || param == KIT_P_RESO) && !o->cut_lk && !(param == KIT_P_RESO && o->res_lk))
            flt_aim(k, o, k->v[pad][KIT_P_CUTOFF], o->res_lk ? 0 : k->v[pad][KIT_P_RESO], 0);   /* (round B) its
                                                         * voices follow (a step's lock stays) */
        if (param == KIT_P_MORPH && !o->morph_lk) {     /* (pages v2) the morph and the envelope's amount follow */
            o->morph = (float)clampi(value, 0, 100) / 100.0f;
            flt_aim(k, o, o->cut_lk ? 100 : k->v[pad][KIT_P_CUTOFF], k->v[pad][KIT_P_RESO], 0);
        }
        if (param == KIT_P_FENV && !o->fenv_lk) {
            o->fe_amt = (float)clampi(value, -100, 100) * 0.08f;
            flt_aim(k, o, o->cut_lk ? 100 : k->v[pad][KIT_P_CUTOFF], k->v[pad][KIT_P_RESO], 0);
        }
        if (param == KIT_P_SPEED || param == KIT_P_WPITCH)
            retune(k, o);                               /* (pages v2) Warp's speed and pitch at once */
    }
    if (param == KIT_P_MUTE && value)                   /* (muted: its voices fade) */
        for (i = 0; i < NV; i++)
            if (k->vo[i].on && k->vo[i].pad == pad && k->vo[i].stage != ST_FADE)
                k->vo[i].stage = ST_FADE, k->vo[i].fade_n = k->cfg.fs * FADE_MS / 1000u, k->vo[i].t = 0, k->vo[i].r_from = k->vo[i].env;
}
void kit_note_lk(kit_t *k, int pad, int semis, int vel, uint32_t frame, int mask, const int16_t *lk)
{
    kit_note_pl(k, pad, semis, vel, frame, mask, lk, 0, NULL, 0);
}
void kit_note_pl(kit_t *k, int pad, int semis, int vel, uint32_t frame, int mask, const int16_t *lk, uint64_t pm,
                 const int16_t *pv, uint32_t us)
{
    ev_t *e;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    if (k->nev == NEV)
        return;                                         /* (a block's worth: more is lost) */
    e = &k->ev[k->nev++];
    e->pad = pad, e->semis = semis, e->vel = vel, e->frame = frame, e->mask = lk && vel > 0 ? mask : 0;
    if (e->mask)
        memcpy(e->lk, lk, sizeof e->lk);
    e->pm = pv && vel > 0 ? pm & ((1ull << KIT_P_N) - 1) : 0, e->us = us;
    if (e->pm)
        memcpy(e->pv, pv, sizeof e->pv);
}
void kit_note(kit_t *k, int pad, int semis, int vel, uint32_t frame) { kit_note_lk(k, pad, semis, vel, frame, 0, NULL); }
/* (2026-11-26, rfm1cs KIT_GHOST) a ghost trigger at frame: its locks onto the pad's voices sounding then (queued as a
 * note, vel -1) */
#define GHOST_P (1ull << KIT_P_CUTOFF | 1ull << KIT_P_RESO | 1ull << KIT_P_MORPH | 1ull << KIT_P_FENV | 1ull << KIT_P_LEVEL | \
                 1ull << KIT_P_PAN | 1ull << KIT_P_SEND1 | 1ull << KIT_P_SEND2 | 1ull << KIT_P_SEND3 | 1ull << KIT_P_PITCH | \
                 1ull << KIT_P_SPEED | 1ull << KIT_P_WPITCH)
void kit_ghost(kit_t *k, int pad, uint32_t frame, uint64_t pm, const int16_t *pv)
{
    ev_t *e;
    if (!k || pad < 0 || pad >= KIT_PADS || !pv || !(pm & GHOST_P) || k->nev == NEV)
        return;
    e = &k->ev[k->nev++];
    memset(e, 0, sizeof *e);
    e->pad = pad, e->vel = -1, e->frame = frame, e->pm = pm & GHOST_P;
    memcpy(e->pv, pv, sizeof e->pv);
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
/* a pad's region [s0, e0) in source frames (at least one frame) */
static void region_p(const kit_t *k, int pad, const int16_t *p, double *s0, double *e0)
{
    const snd_t *s = &k->snd[pad];
    *s0 = (double)s->frames * p[KIT_P_START] / KIT_FRAC;
    *e0 = (double)s->frames * p[KIT_P_END] / KIT_FRAC;
    if (*e0 <= *s0 + 1)
        *e0 = *s0 + 1 < s->frames ? *s0 + 1 : s->frames, *s0 = *e0 - 1;
}
static int warp_p(const int16_t *p)
{
    return p[KIT_P_MODE] == KIT_M_GRANULAR ? KIT_W_OFF : clampi(p[KIT_P_WARP], 0, KIT_W_N - 1);
}
static int warp_of(const kit_t *k, int pad) { return warp_p(k->v[pad]); }
/* (2026-11-26) a voice's settings: its pad's, its locks over them */
static const int16_t *vparams(const kit_t *k, const voice_t *v, int16_t *o)
{
    int i;
    if (!v->pm)
        return k->v[v->pad];
    memcpy(o, k->v[v->pad], sizeof k->v[0]);
    for (i = 0; i < KIT_P_N; i++)
        if (v->pm >> i & 1)
            o[i] = v->pl[i];
    return o;
}
/* (pages v2) a voice's rates: inc the source frames a frame of Off and Repitch (Repitch with Sync: the region in its
 * microseconds, Transpose and the notes' pitches ignored, the owner's rule); spd the playhead's (the time) and ginc the
 * grains' (the pitch) of Grains and Beats; the speed's multiplier on all but Off */
static void rates_p(const kit_t *k, int pad, const int16_t *p, uint32_t us, int semis, int cents, double *inc,
                    double *spd, double *ginc)
{
    const snd_t *s = &k->snd[pad];
    const int w = warp_p(p);
    const double sp = w ? clampi(p[KIT_P_SPEED], 25, 400) / 100.0 : 1.0, own = (double)s->rate / k->cfg.fs;
    const double pitch = pow(2.0, (cents + semis * 100) / 1200.0);
    double fit = 0;
    if (us && s->frames && w) {
        double s0, e0;
        region_p(k, pad, p, &s0, &e0);
        fit = (e0 - s0) / ((double)us * 1e-6 * k->cfg.fs);
    }
    if (inc)
        *inc = (w == KIT_W_REPITCH && fit > 0 ? fit : pitch * own) * sp;
    if (spd)
        *spd = (fit > 0 ? fit : own) * sp;
    if (ginc)
        *ginc = pitch * own * pow(2.0, clampi(p[KIT_P_WPITCH], -2400, 2400) / 1200.0);
}
static void rates(const kit_t *k, int pad, int semis, int cents, double *inc, double *spd, double *ginc)
{
    rates_p(k, pad, k->v[pad], k->st_us[pad], semis, cents, inc, spd, ginc);
}
double kit_rate_of(const kit_t *k, int pad, int semis)
{
    double inc, spd;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return 1.0;
    rates(k, pad, semis, k->v[pad][KIT_P_PITCH], &inc, &spd, NULL);
    return warp_of(k, pad) >= KIT_W_GRAINS ? spd : inc;
}
/* a voice's rates again (the tempo, the speed, the pitch changed while it sounds) */
static void retune(kit_t *k, voice_t *v)
{
    double inc, spd, ginc;
    int16_t pp[KIT_P_N];
    const int16_t *p = vparams(k, v, pp);
    rates_p(k, v->pad, p, v->pm >> KIT_P_STRETCH & 1 ? v->us_lk : k->st_us[v->pad], v->semis, v->cents, &inc, &spd, &ginc);
    v->binc = inc > 0 ? inc : 1.0, v->inc = v->binc * v->pe_mul, v->spd = spd, v->ginc = ginc * v->pe_mul;
}
static uint32_t rnd32(voice_t *v)                       /* (xorshift: the grains' jitter, spray and spread) */
{
    uint32_t x = v->rnd ? v->rnd : 2463534242u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return v->rnd = x;
}
static float rnd_bi(voice_t *v) { return (float)(rnd32(v) >> 8) * (2.0f / 16777216.0f) - 1.0f; }   /* -1..1 */
static void release(voice_t *v, uint32_t n);
static void level_pan(voice_t *v, const int16_t *p, int mask, const int16_t *lk, int vel);
static void note_on(kit_t *k, int pad, int semis, int vel, int mask, const int16_t *lk, uint64_t pm, const int16_t *pv,
                    uint32_t us)
{
    int16_t pp[KIT_P_N];
    const int16_t *p = k->v[pad];
    const snd_t *s = &k->snd[pad];
    voice_t *v = NULL;
    int i, live = 0, oldest = -1;
    double s0, e0;
    if (pm) {                                           /* (2026-11-26) the hit's locks over the pad's settings */
        memcpy(pp, k->v[pad], sizeof pp);
        for (i = 0; i < KIT_P_N; i++)
            if (pm >> i & 1)
                pp[i] = pv[i];
        p = pp;
    }
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
    v->pad = pad, v->pm = pm, v->us_lk = us, v->vel = vel;
    if (pm)
        memcpy(v->pl, pv, sizeof v->pl);
    region_p(k, pad, p, &s0, &e0);
    v->cents = mask & 1 << KIT_LK_PITCH ? lk[KIT_LK_PITCH] : p[KIT_P_PITCH];
    v->on = 1, v->pad = pad, v->semis = semis, v->age = ++k->age, v->snd = s;
    v->eng = clampi(p[KIT_P_MODE], 0, KIT_M_N - 1), v->warp = warp_p(p);
    v->pe_mul = 1.0f;
    v->rnd = 0x9E3779B9u * (uint32_t)v->age + (uint32_t)pad;
    retune(k, v);                                       /* (the rates from the whole region: a chop keeps them) */
    if (mask & 1 << KIT_LK_START) {                     /* (round B: a step's own start) */
        const double ls = (double)s->frames * clampi(lk[KIT_LK_START], 0, KIT_FRAC) / KIT_FRAC;
        s0 = ls < e0 - 1 ? ls : e0 - 1;
    }
    v->mode = v->eng == KIT_M_MOMENTARY ? B_MOM : v->eng == KIT_M_LOOP ? B_LOOP : v->eng == KIT_M_GRANULAR ? B_HELD : B_ONE;
    {                                                   /* Loop: its loop inside the region */
        double ls = (double)s->frames * clampi(p[KIT_P_LOOPS], 0, KIT_FRAC) / KIT_FRAC;
        double le = (double)s->frames * clampi(p[KIT_P_LOOPE], 0, KIT_FRAC) / KIT_FRAC;
        ls = ls < s0 ? s0 : ls > e0 - 1 ? e0 - 1 : ls;
        le = le > e0 ? e0 : le < ls + 1 ? ls + 1 : le;
        v->ls = ls, v->le = le;
    }
    if (mask & 1 << KIT_LK_CHOP) {                      /* (pages v2) the right half's Chop: chops a..b of the region,
                                                         * one once, a range round while held */
        const int a = clampi(lk[KIT_LK_CHOP], 0, 15), b = mask & 1 << KIT_LK_CHOPE ? clampi(lk[KIT_LK_CHOPE], a, 15) : a;
        const double len = e0 - s0, c0 = s0 + len * a / 16.0, c1 = s0 + len * (b + 1) / 16.0;
        s0 = c0, e0 = c1 > c0 + 1 ? c1 : c0 + 1;
        v->ls = s0, v->le = e0;
        if (v->eng != KIT_M_GRANULAR)
            v->mode = a == b ? B_ONE : B_LOOP;
    }
    v->xf = v->mode == B_LOOP ? (v->le - v->ls) * 0.5 * clampi(p[KIT_P_LOOPXF], 0, 100) / 100.0 : 0.0;
    v->s = s0, v->e = e0, v->dir = p[KIT_P_REVERSE] ? -1 : 1;
    v->pos = v->dir > 0 ? s0 : e0 - 1;
    v->gate = 1;
    v->stage = ST_ATTACK, v->env = 0.0f;
    v->a_step = p[KIT_P_ATTACK] > 0 ? 1.0f / (float)ms_n(k, p[KIT_P_ATTACK]) : 1.0f;
    v->hold_n = ms_n(k, p[KIT_P_HOLD]);
    v->rel_n = ms_n(k, p[KIT_P_RELEASE]);
    v->dec_n = ms_n(k, mask & 1 << KIT_LK_DECAY ? lk[KIT_LK_DECAY] : p[KIT_P_DECAY]);   /* (round B) */
    v->sus = (float)clampi(p[KIT_P_SUSTAIN], 0, 100) / 100.0f;
    v->legacy = v->hold_n && !v->dec_n && p[KIT_P_SUSTAIN] >= 100;
    /* (pages v2) the engines' own */
    if (v->eng == KIT_M_PITCHENV) {
        v->pe_depth = (float)clampi(p[KIT_P_PDEPTH], -48, 48);
        v->pe_a = ms_n(k, p[KIT_P_PATTACK]), v->pe_r = ms_n(k, p[KIT_P_PRELEASE]);
        v->pe_mul = v->pe_a ? 1.0f : exp2f((v->pe_r ? v->pe_depth : 0.0f) / 12.0f);
        v->inc = v->binc * v->pe_mul, v->ginc *= v->pe_mul;
    }
    v->q_step = 1.0f, v->ds = 1;
    v->morph_lk = (mask >> KIT_LK_MORPH) & 1, v->fenv_lk = (mask >> KIT_LK_FENV) & 1;
    v->morph = (float)clampi(v->morph_lk ? lk[KIT_LK_MORPH] : p[KIT_P_MORPH], 0, 100) / 100.0f;
    v->fe_amt = (float)clampi(v->fenv_lk ? lk[KIT_LK_FENV] : p[KIT_P_FENV], -100, 100) * 0.08f;   /* (100: 8 octaves) */
    v->fe_a = ms_n(k, p[KIT_P_FATTACK]) / FLT_N, v->fe_d = ms_n(k, p[KIT_P_FDECAY]) / FLT_N;
    v->fe_r = ms_n(k, p[KIT_P_FRELEASE]) / FLT_N, v->fe_s = (float)clampi(p[KIT_P_FSUSTAIN], 0, 100) / 100.0f;
    if (!v->fe_d)
        v->fe_d = 1;
    v->cut_lk = (mask >> KIT_LK_CUTOFF) & 1, v->res_lk = (mask >> KIT_LK_RESO) & 1;
    flt_aim(k, v, v->cut_lk ? lk[KIT_LK_CUTOFF] : p[KIT_P_CUTOFF], v->res_lk ? lk[KIT_LK_RESO] : p[KIT_P_RESO], 1);
    if (v->warp >= KIT_W_GRAINS || v->eng == KIT_M_GRANULAR) {   /* (pages v2) the grains */
        const int ms = v->eng == KIT_M_GRANULAR ? clampi(p[KIT_P_GSIZE], 10, 500) : clampi(p[KIT_P_WGRAIN], 20, 120);
        v->glen = ms_n(k, ms) > 16 ? ms_n(k, ms) : 16;
        v->wstep = 1024.0f / (float)v->glen;
        v->ph = v->pos;
        if (v->eng == KIT_M_GRANULAR)
            v->gnext = 0;                               /* (the first grain at once) */
        else {                                          /* Warp: two Hann grains half a grain apart (their windows sum
                                                         * to one): the first from its middle, so the attack is kept */
            v->jit = v->warp == KIT_W_BEATS ? (double)ms_n(k, clampi(p[KIT_P_WSEARCH], 1, 30))
                                            : v->glen * 0.5 * clampi(p[KIT_P_WJITTER], 0, 100) / 100.0 * v->ginc;
            v->gr[0].on = 1, v->gr[0].pos = v->pos, v->gr[0].inc = v->ginc, v->gr[0].age = v->glen / 2;
            v->gr[1].on = 1, v->gr[1].pos = v->pos, v->gr[1].inc = v->ginc, v->gr[1].age = 0;
        }
    }
    level_pan(v, p, mask, lk, vel);
}
/* a voice's level, pan and sends (its velocity; a step's LK locks over p) */
static void level_pan(voice_t *v, const int16_t *p, int mask, const int16_t *lk, int vel)
{
    const int lvl = mask & 1 << KIT_LK_LEVEL ? lk[KIT_LK_LEVEL] : p[KIT_P_LEVEL];
    const double lin = lvl <= -60 ? 0.0 : pow(10.0, lvl / 20.0);
    const double g = lin * (vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0;
    const int pan = clampi(mask & 1 << KIT_LK_PAN ? lk[KIT_LK_PAN] : p[KIT_P_PAN], -50, 50);
    int i;
    v->gl = (float)(g * (pan > 0 ? 1.0 - pan / 50.0 : 1.0));
    v->gr2 = (float)(g * (pan < 0 ? 1.0 + pan / 50.0 : 1.0));
    for (i = 0; i < 3; i++)                             /* (3b) its sends, after its level and pan */
        v->sg[i] = (float)clampi(p[KIT_P_SEND1 + i], 0, 100) / 100.0f;
}
/* (2026-11-26) a ghost trigger's locks onto the pad's sounding voices: they keep them */
static void ghost_on(kit_t *k, int pad, uint64_t pm, const int16_t *pv)
{
    int i, j;
    for (i = 0; i < NV; i++) {
        voice_t *v = &k->vo[i];
        int16_t pp[KIT_P_N];
        const int16_t *p;
        if (!v->on || v->pad != pad || v->stage == ST_FADE)
            continue;
        v->pm |= pm;
        for (j = 0; j < KIT_P_N; j++)
            if (pm >> j & 1)
                v->pl[j] = pv[j];
        p = vparams(k, v, pp);
        if (pm >> KIT_P_MORPH & 1)
            v->morph_lk = 0, v->morph = (float)clampi(p[KIT_P_MORPH], 0, 100) / 100.0f;
        if (pm >> KIT_P_FENV & 1)
            v->fenv_lk = 0, v->fe_amt = (float)clampi(p[KIT_P_FENV], -100, 100) * 0.08f;
        if (pm & (1ull << KIT_P_CUTOFF | 1ull << KIT_P_RESO | 1ull << KIT_P_MORPH | 1ull << KIT_P_FENV)) {
            if (pm >> KIT_P_CUTOFF & 1)
                v->cut_lk = 0;
            if (pm >> KIT_P_RESO & 1)
                v->res_lk = 0;
            flt_aim(k, v, p[KIT_P_CUTOFF], p[KIT_P_RESO], 0);   /* (gliding there, no zipper) */
        }
        if (pm & (1ull << KIT_P_LEVEL | 1ull << KIT_P_PAN | 1ull << KIT_P_SEND1 | 1ull << KIT_P_SEND2 | 1ull << KIT_P_SEND3))
            level_pan(v, p, 0, NULL, v->vel);
        if (pm >> KIT_P_PITCH & 1)
            v->cents = p[KIT_P_PITCH];
        if (pm & (1ull << KIT_P_PITCH | 1ull << KIT_P_SPEED | 1ull << KIT_P_WPITCH))
            retune(k, v);
    }
}

static void note_off(kit_t *k, int pad, int semis)
{
    int i;
    (void)k;
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
/* (pages v2) a source frame of the voice at p (linear between frames; past the sound's ends its edge) */
static void frame_at(const voice_t *v, double p, float *l, float *r)
{
    const int16_t *pcm = v->snd->pcm;
    const uint32_t last = v->snd->frames - 1;
    const double ps = p < 0 ? 0 : p;
    const uint32_t a = (uint32_t)ps > last ? last : (uint32_t)ps, b = a + 1 > last ? last : a + 1;
    const float fr = (float)(ps - a);
    *l = pcm[2 * a] + (pcm[2 * b] - pcm[2 * a]) * fr;
    *r = pcm[2 * a + 1] + (pcm[2 * b + 1] - pcm[2 * a + 1]) * fr;
}
/* .. a grain's position folded into the loop (looping) or the region */
static double fold(const voice_t *v, double p)
{
    if (v->mode == B_LOOP) {
        const double n = v->le - v->ls;
        if (p >= v->le)
            p = v->ls + fmod(p - v->ls, n);
        else if (p < v->ls && v->pos >= v->ls)
            p = v->le - fmod(v->ls - p, n);
    }
    return p < v->s ? v->s : p > v->e - 1 ? v->e - 1 : p;
}
/* (pages v2, Warp Beats) WSOLA: the start within +-jit of the playhead whose next frames best match the frames the
 * fading grain plays next (a normalised cross-correlation, mono, every 4th frame over 256; offsets every 4th) */
static double beats_align(const voice_t *v, double nat, double ph)
{
    const int16_t *pcm = v->snd->pcm;
    const int64_t last = (int64_t)v->snd->frames - 1, S = (int64_t)v->jit, L = 256;
    int64_t o, j, best = 0;
    double bestc = -1e30;
    const int64_t n0 = (int64_t)nat, p0 = (int64_t)ph;
    if (S < 4 || n0 < 0 || n0 + L * v->dir > last || n0 + L * v->dir < 0)
        return ph;
    for (o = -S; o <= S; o += 4) {
        const int64_t c = p0 + o;
        double xy = 0, yy = 1e-9;
        if (c < (int64_t)v->s || c > (int64_t)v->e - 1 || c + L * v->dir < 0 || c + L * v->dir > last)
            continue;
        for (j = 0; j < L; j += 4) {
            const int64_t ia = n0 + j * v->dir, ib = c + j * v->dir;
            const float x = (float)pcm[2 * ia] + pcm[2 * ia + 1], y = (float)pcm[2 * ib] + pcm[2 * ib + 1];
            xy += x * y, yy += y * y;
        }
        xy /= sqrt(yy);
        if (xy > bestc)
            bestc = xy, best = o;
    }
    return ph + (double)best;
}
/* (pages v2) a frame of the grains: Warp's two (one restarts at the playhead each half grain), Granular's cloud */
static void grains_frame(const kit_t *k, voice_t *v, float *l, float *r)
{
    float sl = 0, sr = 0;
    int i;
    if (v->eng == KIT_M_GRANULAR) {                     /* spawn: every grain / density frames, at the position
                                                         * (and its spray), its pitch spread at random */
        if (!v->gnext) {
            int16_t pp[KIT_P_N];
            const int16_t *p = vparams(k, v, pp);
            const int dens = clampi(p[KIT_P_GDENS], 1, NGR);
            const double len = v->e - v->s;
            const uint32_t gl = ms_n(k, clampi(p[KIT_P_GSIZE], 10, 500));
            if (gl > 16 && gl != v->glen)
                v->glen = gl, v->wstep = 1024.0f / (float)gl;
            v->gspawn = v->glen / (uint32_t)dens > 0 ? v->glen / (uint32_t)dens : 1;
            v->ggain = dens >= 2 ? 2.0f / (float)dens : 1.0f;
            for (i = 0; i < NGR && v->gr[i].on; i++) {}
            if (i < NGR) {
                const double spr = clampi(p[KIT_P_GSPREAD], 0, 24) * rnd_bi(v);
                const double inc = v->ginc * pow(2.0, spr / 12.0);
                double at = v->s + len * clampi(p[KIT_P_GPOS], 0, 100) / 100.0 +
                            len * 0.5 * clampi(p[KIT_P_GSPRAY], 0, 100) / 100.0 * rnd_bi(v);
                const double need = v->glen * inc;
                if (v->dir > 0)
                    at = at > v->e - need ? v->e - need : at;
                else
                    at = at < v->s + need ? v->s + need : at;
                v->gr[i].on = 1, v->gr[i].age = 0, v->gr[i].inc = inc, v->gr[i].pos = at < v->s ? v->s : at > v->e - 1 ? v->e - 1 : at;
            }
            v->gnext = v->gspawn;
        }
        v->gnext--;
    }
    for (i = 0; i < NGR; i++) {
        grain_t *g = &v->gr[i];
        float a, b, w;
        if (!g->on)
            continue;
        if (g->age >= v->glen) {
            if (v->eng == KIT_M_GRANULAR) {
                g->on = 0;
                continue;
            }
            {                                           /* (Warp) restarted at the playhead (Grains: its jitter; Beats:
                                                         * aligned with the other grain's next frames) */
                const grain_t *o = &v->gr[i ^ 1];
                double at = v->ph;
                if (v->warp == KIT_W_BEATS)
                    at = beats_align(v, fold(v, o->pos + v->dir * o->inc * o->age), v->ph);
                else if (v->jit > 0)
                    at += v->jit * rnd_bi(v);
                if (v->mode != B_LOOP) {                /* (the grain inside the region: no edge mid-grain) */
                    const double need = v->glen * v->ginc;
                    if (v->dir > 0 && at > v->e - need)
                        at = v->e - need;
                    if (v->dir < 0 && at < v->s + need)
                        at = v->s + need;
                }
                g->pos = fold(v, at), g->age = 0, g->inc = v->ginc;
            }
        }
        w = k->hann[(uint32_t)((float)g->age * v->wstep) & 1023];
        frame_at(v, fold(v, g->pos), &a, &b);
        sl += a * w, sr += b * w;
        g->pos += v->dir * g->inc, g->age++;
    }
    if (v->eng == KIT_M_GRANULAR)
        sl *= v->ggain, sr *= v->ggain;
    *l = sl, *r = sr;
}
/* (pages v2, every FLT_N frames) the pitch envelope, LoFi's settings, the filter's envelope */
static void control(const kit_t *k, voice_t *v)
{
    int16_t pp[KIT_P_N];
    const int16_t *p = vparams(k, v, pp);
    v->cn = FLT_N;
    if (v->eng == KIT_M_PITCHENV) {                     /* 0 to the depth over the attack, back to 0 over the release */
        float off;
        v->pe_t += FLT_N;
        if (v->pe_t < v->pe_a)
            off = v->pe_depth * (float)v->pe_t / (float)v->pe_a;
        else if (v->pe_t - v->pe_a < v->pe_r)
            off = v->pe_depth * (1.0f - (float)(v->pe_t - v->pe_a) / (float)v->pe_r);
        else
            off = 0.0f;
        {
            const float mul = exp2f(off / 12.0f);
            v->inc = v->binc * mul;
            v->ginc = v->ginc / v->pe_mul * mul;
            v->pe_mul = mul;
        }
    }
    if (v->eng == KIT_M_LOFI) {
        const int bits = clampi(p[KIT_P_BITS], 1, 16);
        v->q_step = (float)(1 << (16 - bits));
        v->ds = clampi(p[KIT_P_DOWNS], 1, 32);
    }
    if (v->fe_amt != 0.0f) {
        if (v->fe_st < 3 && (v->stage >= ST_RELEASE || (v->mode != B_ONE && !v->gate)))
            v->fe_st = 3, v->fe_t = 0, v->fe_from = v->fe_lvl;   /* (its release with the note's) */
        fenv_next(v);
    }
}
/* one voice, frames [0, n) of acc (and its sends' buses sb[0..2], the same frames) */
static void voice_run(const kit_t *k, voice_t *v, float *acc, float *const sb[3], uint32_t n)
{
    const int grains = v->warp >= KIT_W_GRAINS || v->eng == KIT_M_GRANULAR;
    uint32_t i;
    for (i = 0; i < n && v->on; i++) {
        double left;
        if (!v->cn)
            control(k, v);
        v->cn--;
        /* the envelope */
        switch (v->stage) {
        case ST_ATTACK:
            v->env += v->a_step;
            if (v->env >= 1.0f) {
                v->env = 1.0f, v->t = 0;
                if (v->hold_n)
                    v->stage = ST_HOLD;
                else if (v->dec_n)
                    v->stage = ST_DECAY, v->d_from = 1.0f;
                else
                    v->stage = ST_SUSTAIN, v->env = v->sus;
            }
            break;
        case ST_HOLD:
            if (++v->t >= v->hold_n) {
                if ((v->mode == B_ONE && v->legacy) || (v->mode != B_ONE && !v->gate))
                    release(v, v->rel_n);               /* (one shot as before round B: the hold's gate; a held mode
                                                         * let go meanwhile) */
                else if (v->dec_n)
                    v->stage = ST_DECAY, v->t = 0, v->d_from = v->env;
                else
                    v->stage = ST_SUSTAIN, v->t = 0, v->env = v->sus;
            }
            break;
        case ST_DECAY:                                  /* (round B) linear to the sustain's level */
            if (v->mode != B_ONE && !v->gate) {
                release(v, v->rel_n);
                break;
            }
            if (++v->t >= v->dec_n)
                v->env = v->sus, v->stage = ST_SUSTAIN, v->t = 0;
            else
                v->env = v->d_from + (v->sus - v->d_from) * (float)v->t / (float)v->dec_n;
            break;
        case ST_SUSTAIN:
            if (v->mode != B_ONE && !v->gate)
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
        /* one shot with no hold: the release is the tail, ending at the end (Warp's grains: the playhead's) */
        if (v->mode == B_HELD)
            left = 1e18;
        else if (grains)
            left = v->dir > 0 ? (v->e - v->ph) / v->spd : (v->ph - v->s) / v->spd;
        else
            left = v->dir > 0 ? (v->e - v->pos) / v->inc : (v->pos - v->s) / v->inc;
        if (v->mode == B_ONE && !v->legacy && v->stage <= ST_SUSTAIN && v->rel_n && left <= v->rel_n)
            release(v, (uint32_t)left ? (uint32_t)left : 1);
        if ((v->mode == B_ONE || v->mode == B_MOM) && left <= 0) {
            v->on = 0;
            break;
        }
        {
            float l, r, ol, orr;
            if (grains)
                grains_frame(k, v, &l, &r);
            else {
                frame_at(v, v->pos, &l, &r);            /* the frame: linear between the two source frames about pos */
                if (v->xf > 0) {                        /* (pages v2) Loop's fade: the loop's end into its start */
                    const double x0 = v->dir > 0 ? v->le - v->xf : v->ls + v->xf;
                    const double d = v->dir > 0 ? v->pos - x0 : x0 - v->pos;
                    if (d > 0 && v->pos >= v->ls && v->pos < v->le) {
                        const float w = (float)(d / v->xf);
                        float l2, r2;
                        frame_at(v, v->dir > 0 ? v->ls + d : v->le - d, &l2, &r2);
                        l += (l2 - l) * w, r += (r2 - r) * w;
                    }
                }
            }
            if (v->eng == KIT_M_LOFI) {                 /* (pages v2) the rate held, then the bits */
                if (v->ds > 1) {
                    if (v->ds_n <= 0)
                        v->hold_l = l, v->hold_r = r, v->ds_n = v->ds;
                    v->ds_n--;
                    l = v->hold_l, r = v->hold_r;
                }
                if (v->q_step > 1.0f)
                    l = floorf(l / v->q_step + 0.5f) * v->q_step, r = floorf(r / v->q_step + 0.5f) * v->q_step;
            }
            if (v->flt && !v->fn)                       /* (round B) the low-pass (open: bypassed, the frames as ever) */
                flt_next(k, v);
            if (v->flt) {
                float x[2];
                int c;
                v->a1 += v->da1, v->a2 += v->da2, v->a3 += v->da3, v->fn--;
                x[0] = l, x[1] = r;
                for (c = 0; c < 2; c++) {
                    const float v3 = x[c] - v->ic2[c];
                    const float v1 = v->a1 * v->ic1[c] + v->a2 * v3;
                    const float v2 = v->ic2[c] + v->a2 * v->ic1[c] + v->a3 * v3;
                    v->ic1[c] = 2.0f * v1 - v->ic1[c], v->ic2[c] = 2.0f * v2 - v->ic2[c];
                    if (v->morph <= 0.0f)
                        x[c] = v2;
                    else {                              /* (pages v2) LP -> BP (unity at its peak) -> HP */
                        const float bp = v->kq * v1, hp = x[c] - v->kq * v1 - v2, m2 = v->morph * 2.0f;
                        x[c] = m2 <= 1.0f ? v2 + (bp - v2) * m2 : bp + (hp - bp) * (m2 - 1.0f);
                    }
                }
                l = x[0], r = x[1];
            }
            ol = l * v->env * v->gl, orr = r * v->env * v->gr2;
            int j;
            acc[2 * i] += ol;
            acc[2 * i + 1] += orr;
            for (j = 0; j < 3; j++)
                if (v->sg[j] > 0.0f)
                    sb[j][2 * i] += ol * v->sg[j], sb[j][2 * i + 1] += orr * v->sg[j];
        }
        if (grains) {                                   /* (pages v2) Warp's playhead at its speed */
            if (v->eng != KIT_M_GRANULAR) {
                v->ph += v->dir * v->spd;
                if (v->mode == B_LOOP) {
                    const double len = v->le - v->ls;
                    if (v->dir > 0 && v->ph >= v->le)
                        v->ph -= len;
                    else if (v->dir < 0 && v->ph < v->ls)
                        v->ph += len;
                }
            }
            continue;
        }
        v->pos += v->dir * v->inc;
        if (v->mode == B_LOOP) {                        /* round its loop while held (and through its release); (pages
                                                         * v2) past a fade: on from the fade's end */
            const double len = v->le - v->ls - v->xf;
            if (v->dir > 0 && v->pos >= v->le)
                v->pos -= len;
            else if (v->dir < 0 && v->pos < v->ls)
                v->pos += len;
        }
    }
}
/* (pages v2) the kit bus's low-pass over n frames of k->dry (open: bypassed; the cutoff glides a frame at a time) */
static void bus_run(kit_t *k, uint32_t n)
{
    const float go = flt_g(k, 100), gt = k->bus_c >= 100 ? go : flt_g(k, k->bus_c), kq = flt_k(k->bus_r);
    const float a = 1.0f - expf(-1.0f / (0.01f * (float)k->cfg.fs));   /* (10 ms) */
    uint32_t i;
    int c;
    if (!k->bus_on) {
        if (k->bus_c >= 100)
            return;
        k->bus_on = 1, k->bus_g = go;                   /* (from open) */
        k->bus_ic1[0] = k->bus_ic1[1] = k->bus_ic2[0] = k->bus_ic2[1] = 0;
    }
    for (i = 0; i < n; i++) {
        float a1, a2, a3;
        k->bus_g += (gt - k->bus_g) * a;
        flt_coefs(k->bus_g, kq, &a1, &a2, &a3);
        for (c = 0; c < 2; c++) {
            const float x = k->dry[2 * i + c], v3 = x - k->bus_ic2[c];
            const float v1 = a1 * k->bus_ic1[c] + a2 * v3, v2 = k->bus_ic2[c] + a2 * k->bus_ic1[c] + a3 * v3;
            k->bus_ic1[c] = 2.0f * v1 - k->bus_ic1[c], k->bus_ic2[c] = 2.0f * v2 - k->bus_ic2[c];
            k->dry[2 * i + c] = v2;
        }
    }
    if (k->bus_c >= 100 && k->bus_g >= go * 0.999f)
        k->bus_on = 0;                                  /* (open again: bypassed) */
    for (c = 0; c < 2; c++) {
        if (fabsf(k->bus_ic1[c]) < 1e-15f)
            k->bus_ic1[c] = 0;
        if (fabsf(k->bus_ic2[c]) < 1e-15f)
            k->bus_ic2[c] = 0;
    }
}
int kit_live(const kit_t *k, int pad, int *cut, int *fe, int *fe_st, int *amp, int *amp_st)
{
    const voice_t *v = NULL;
    int i;
    for (i = 0; k && i < NV; i++)
        if (k->vo[i].on && k->vo[i].pad == pad && k->vo[i].stage != ST_FADE && (!v || k->vo[i].age > v->age))
            v = &k->vo[i];
    if (!v)
        return 0;
    {
        const float c = (float)v->cut_v + v->fe_amt * v->fe_lvl * 10.03f;   /* (100 steps are ten octaves) */
        *cut = c < 0 ? 0 : c > 100 ? 100 : (int)(c + 0.5f);
    }
    *fe = (int)(v->fe_lvl * 255.0f + 0.5f), *fe_st = v->fe_st > 3 ? 3 : v->fe_st;
    *amp = (int)(v->env * 255.0f + 0.5f);
    *amp_st = v->stage == ST_ATTACK ? 0 : v->stage <= ST_DECAY ? 1 : v->stage == ST_SUSTAIN ? 2 : 3;
    return 1;
}
void kit_bus(kit_t *k, int which, int value)
{
    if (!k)
        return;
    if (which == 0)
        k->bus_c = clampi(value, 0, 100);
    else if (which == 1)
        k->bus_r = clampi(value, 0, 100);
}
/* frames [at0, at0 + n) of the call (n <= RBUF) into k->dry and k->sb, the notes due in them first (*e: the next) */
static void render_part(kit_t *k, uint32_t at0, uint32_t n, int *e)
{
    uint32_t at = 0;
    int i, j;
    memset(k->dry, 0, sizeof(float) * 2u * n);
    for (j = 0; j < 3; j++)
        memset(k->sb[j], 0, sizeof(float) * 2u * n);
    while (at < n) {                                    /* in parts between the notes' frames */
        uint32_t end = n;
        float *sb[3];
        while (*e < k->nev && k->ev[*e].frame <= at0 + at) {
            const ev_t *x = &k->ev[(*e)++];
            if (x->vel > 0)
                note_on(k, x->pad, x->semis, x->vel, x->mask, x->lk, x->pm, x->pv, x->us);
            else if (x->vel < 0)
                ghost_on(k, x->pad, x->pm, x->pv);      /* (2026-11-26) a ghost trigger */
            else
                note_off(k, x->pad, x->semis);
        }
        if (*e < k->nev && k->ev[*e].frame < at0 + end)
            end = k->ev[*e].frame - at0;
        for (j = 0; j < 3; j++)
            sb[j] = k->sb[j] + 2 * at;
        for (i = 0; i < NV; i++)
            if (k->vo[i].on)
                voice_run(k, &k->vo[i], k->dry + 2 * at, sb, end - at);
        at = end;
    }
}
void kit_render_bus(kit_t *k, float *acc, float *const send[3], int16_t *own, uint32_t frames)
{
    uint32_t at0 = 0, i;
    int e = 0, j;
    float gl, gr, sg[3];
    if (!k)
        return;
    {                                                   /* (3b) the kit's strip: level, pan (a balance), sends */
        const float g = (float)clampi(k->mix[0], 0, 127) / 127.0f * k->master;
        const int pan = clampi(k->mix[1], 0, 127);
        gl = gr = g;
        if (pan < 64)
            gr = g * (float)pan / 64.0f;
        else if (pan > 64)
            gl = g * (float)(127 - pan) / 63.0f;
        for (j = 0; j < 3; j++)
            sg[j] = (float)clampi(k->mix[2 + j], 0, 127) / 127.0f;
    }
    while (at0 < frames) {
        const uint32_t n = frames - at0 < RBUF ? frames - at0 : RBUF;
        render_part(k, at0, n, &e);
        bus_run(k, n);                                  /* (pages v2) the kit bus's low-pass */
        for (i = 0; i < n; i++) {
            const float l = k->dry[2 * i] * gl, r = k->dry[2 * i + 1] * gr;
            acc[2 * (at0 + i)] += l, acc[2 * (at0 + i) + 1] += r;
            if (own) {
                const long a = lrintf(l), b = lrintf(r);
                own[2 * (at0 + i)] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                own[2 * (at0 + i) + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            for (j = 0; send && j < 3; j++)             /* a pad's own send and the strip's, after the strip's level */
                if (send[j]) {
                    send[j][2 * (at0 + i)] += (k->sb[j][2 * i] + k->dry[2 * i] * sg[j]) * gl;
                    send[j][2 * (at0 + i) + 1] += (k->sb[j][2 * i + 1] + k->dry[2 * i + 1] * sg[j]) * gr;
                }
        }
        at0 += n;
    }
    for (; e < k->nev; e++)                             /* (a frame past the block: now) */
        if (k->ev[e].vel > 0)
            note_on(k, k->ev[e].pad, k->ev[e].semis, k->ev[e].vel, k->ev[e].mask, k->ev[e].lk, k->ev[e].pm, k->ev[e].pv,
                    k->ev[e].us);
        else if (k->ev[e].vel < 0)
            ghost_on(k, k->ev[e].pad, k->ev[e].pm, k->ev[e].pv);
        else
            note_off(k, k->ev[e].pad, k->ev[e].semis);
    k->nev = 0;
}
void kit_render(kit_t *k, float *acc, uint32_t frames) { kit_render_bus(k, acc, NULL, NULL, frames); }
void kit_stretch(kit_t *k, int pad, uint32_t us)
{
    int i;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    k->st_us[pad] = us;
    for (i = 0; i < NV; i++)                            /* (a voice sounding: re-fitted at once) */
        if (k->vo[i].on && k->vo[i].pad == pad)
            retune(k, &k->vo[i]);
}
/* (2026-11-26) the volume knob's MASTER (0..1023) on the kit's whole output, its sends too, as each firmware's own
 * master: an audio taper (the square), 0 silent */
void kit_master(kit_t *k, int m)
{
    float f;
    if (!k)
        return;
    f = (float)(m < 0 ? 1023 : m > 1023 ? 1023 : m) / 1023.0f;
    k->master = f * f;
}
void kit_mix(kit_t *k, int level, int pan, const int sends[3])
{
    int j;
    if (!k)
        return;
    k->mix[0] = clampi(level, 0, 127), k->mix[1] = clampi(pan, 0, 127);
    for (j = 0; j < 3; j++)
        k->mix[2 + j] = sends ? clampi(sends[j], 0, 127) : 0;
}
void kit_save(kit_t *k, const kit_kit_t *kit)
{
    req_t r;
    int busy;
    if (!k || !kit)
        return;
    pthread_mutex_lock(&k->mu);
    busy = k->save_busy;
    if (!busy) {
        k->save = *kit, k->save_busy = 1;
        snprintf(k->save_proj, sizeof k->save_proj, "%s", k->proj);
    }
    pthread_mutex_unlock(&k->mu);
    if (busy) {                                         /* (one at a time) */
        ans_t a;
        memset(&a, 0, sizeof a);
        a.a.kind = KIT_A_SAVED, a.a.status = KIT_FAILED;
        answer(k, &a);
        return;
    }
    memset(&r, 0, sizeof r);
    r.kind = RQ_SAVE;
    request(k, &r);
}
void kit_open(kit_t *k, uint32_t id)
{
    req_t r;
    if (!k)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_OPEN, r.id = id;
    request(k, &r);
}
void kit_folder(kit_t *k, int place, uint32_t id)
{
    req_t r;
    if (!k)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_FOLDER, r.place = place, r.id = id;
    request(k, &r);
}
/* ---- (stage 3c) */
static void fail_now(kit_t *k, int pad)                 /* a pad's request refused at once (no sound to work on) */
{
    ans_t a;
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_LOADED, a.a.pad = pad, a.a.status = KIT_FAILED;
    answer(k, &a);
}
void kit_merge(kit_t *k, int dst, int src, int op)
{
    req_t r;
    if (!k || dst < 0 || dst >= KIT_PADS || src < 0 || src >= KIT_PADS)
        return;
    if (!k->ppath[src][0] || !k->ppath[dst][0] || src == dst || (op != KIT_MG_MIX && op != KIT_MG_JOIN)) {
        fail_now(k, dst);
        return;
    }
    memset(&r, 0, sizeof r);
    r.kind = RQ_MERGE, r.pad = dst, r.pad2 = src, r.op = op;
    snprintf(r.path, sizeof r.path, "%s", k->ppath[src]);
    snprintf(r.path2, sizeof r.path2, "%s", k->ppath[dst]);
    r.r0[0] = k->v[src][KIT_P_START], r.r0[1] = k->v[src][KIT_P_END];
    r.r1[0] = k->v[dst][KIT_P_START], r.r1[1] = k->v[dst][KIT_P_END];
    request(k, &r);
}
void kit_edit(kit_t *k, int pad, int op)
{
    req_t r;
    if (!k || pad < 0 || pad >= KIT_PADS)
        return;
    if (!k->ppath[pad][0] || op < KIT_ED_TRIM || op > KIT_ED_FREEZE) {
        fail_now(k, pad);
        return;
    }
    memset(&r, 0, sizeof r);
    r.kind = RQ_EDIT, r.pad = pad, r.op = op;
    snprintf(r.path, sizeof r.path, "%s", k->ppath[pad]);
    r.r0[0] = k->v[pad][KIT_P_START], r.r0[1] = k->v[pad][KIT_P_END];
    memcpy(r.v, k->v[pad], sizeof r.v);                 /* (round C: Freeze renders the pad as it is now) */
    r.us = k->st_us[pad];
    request(k, &r);
}
void kit_slice(kit_t *k, uint32_t take, int method, uint32_t step_us) { kit_slice_sens(k, take, method, step_us, 50); }
void kit_slice_sens(kit_t *k, uint32_t take, int method, uint32_t step_us, int sens)
{
    req_t r;
    if (!k)
        return;
    memset(&r, 0, sizeof r);
    r.kind = RQ_SLICE, r.id = take, r.op = method, r.us = step_us, r.gen = ++k->sl_gen, r.n = sens > 0 ? sens : 50;
    request(k, &r);
}
static const voice_t *slice_voice(const kit_t *k)       /* the slice's voice sounding (NULL none) */
{
    int i;
    for (i = 0; k && i < NV; i++)
        if (k->vo[i].on && k->vo[i].pad == SLP && k->vo[i].stage != ST_FADE)
            return &k->vo[i];
    return NULL;
}
uint32_t kit_slice_pos(const kit_t *k)
{
    const voice_t *v = slice_voice(k);
    return v ? (uint32_t)v->pos : 0;
}
/* (2026-11-26) a slice (Tap: the take's pass) as a kit voice: through the bus, as a pad's */
void kit_slice_play(kit_t *k, uint32_t from, uint32_t count)
{
    int i;
    if (!k)
        return;
    for (i = 0; i < NV; i++)                            /* (the one before stops: the latest replaces it) */
        if (k->vo[i].on && k->vo[i].pad == SLP)
            fade(k, &k->vo[i], FADE_MS);
    if (!count || !k->sl_take || !k->prev.pcm || from >= k->prev.frames)
        return;                                         /* (round B: count 0, Tap's pass stopped) */
    k->snd[SLP] = k->prev;
    note_on(k, SLP, 0, 127, 0, NULL, 0, NULL, 0);
    for (i = 0; i < NV; i++) {
        voice_t *v = &k->vo[i];
        if (v->on && v->pad == SLP && v->stage != ST_FADE) {   /* (its region the slice's, to the frame) */
            const double e = (double)from + count < k->prev.frames ? (double)from + count : (double)k->prev.frames;
            v->s = v->ls = v->pos = from, v->e = v->le = e > from + 1.0 ? e : from + 1.0;
        }
    }
}
void kit_slice_done(kit_t *k)
{
    if (!k)
        return;
    k->sl_gen++;                                        /* (an answer under way: dropped) */
    if (!k->sl_take)
        return;
    voice_kill_pad(k, SLP), memset(&k->snd[SLP], 0, sizeof k->snd[SLP]);
    k->sl_take = 0;
    garbage(k, k->prev.pcm);
    k->prev.pcm = NULL, k->prev.frames = 0;
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
    int p, q, dup;
    for (p = 0; k && p < KIT_PADS; p++) {
        for (q = 0, dup = 0; q < p && !dup; q++)        /* (3c: a sound several pads hold, counted once) */
            dup = k->snd[q].pcm && k->snd[q].pcm == k->snd[p].pcm;
        if (!dup)
            n += k->snd[p].frames * 4u;
    }
    return n;
}
const char *kit_pad_path(const kit_t *k, int pad) { return k && pad >= 0 && pad < KIT_PADS ? k->ppath[pad] : ""; }
uint32_t kit_frames(const kit_t *k, int pad) { return k && pad >= 0 && pad < KIT_PADS ? k->snd[pad].frames : 0; }
int kit_has_core(const kit_t *k) { return k && k->core[0]; }
int kit_previewing(const kit_t *k) { return slice_voice(k) != NULL; }   /* (a slice sounding) */
uint32_t kit_sounding(const kit_t *k)
{
    uint32_t b = 0;
    int i;
    for (i = 0; k && i < NV; i++)
        if (k->vo[i].on && k->vo[i].stage != ST_FADE && k->vo[i].pad < KIT_PADS)
            b |= 1u << k->vo[i].pad;
    return b;
}

/* ---- (round C) Freeze: the pad as it sounds now, rendered offline on the file thread by a voice of a kit of its own
 * (the same code that plays it): the region, pitch and fine, stretch, reverse, the envelope, the filter, level and
 * pan (not the sends: the FX run on the engine's thread). One shot: the whole note to its end (the release its tail);
 * the held modes: held for the region once, then released (its tail). At most KIT_FREEZE_S, the kit's memory. A new
 * file (16-bit stereo at the engine's rate, "<name> frz") in the project's folder, onto the pad (LOADED) */
static void do_freeze(kit_t *k, const req_t *r)
{
    ans_t a;
    kit_t *t = calloc(1, sizeof *t);
    int16_t *src = NULL, *o = NULL;
    uint32_t fr = 0, rate = 0, no = 0, cap = 0, gate_n = 0, max;
    int st = KIT_FAILED, gated = 1, i;
    voice_t *v = NULL;
    char name[KIT_NAME + 16], path[KIT_PATH], b[KIT_NAME];
    memset(&a, 0, sizeof a);
    a.a.kind = KIT_A_LOADED, a.a.pad = r->pad, a.a.status = KIT_FAILED;
    if (!t || !(src = load_sound(r->path, k->cfg.max_bytes, 0, &fr, &rate, &st))) {
        free(t);
        a.a.status = st == KIT_OK ? KIT_FAILED : st;
        answer(k, &a);
        return;
    }
    t->cfg = k->cfg, t->glide = k->glide;
    memcpy(t->hann, k->hann, sizeof t->hann);           /* (pages v2: the grains' window) */
    t->bus_c = 100;
    memcpy(t->v[0], r->v, sizeof t->v[0]);
    t->v[0][KIT_P_MUTE] = 0, t->v[0][KIT_P_CHOKE] = 0;
    t->st_us[0] = r->us;
    t->snd[0].pcm = src, t->snd[0].frames = fr, t->snd[0].rate = rate;
    note_on(t, 0, 0, 127, 0, NULL, 0, NULL, 0);
    for (i = 0; i < NV && !v; i++)
        if (t->vo[i].on)
            v = &t->vo[i];
    max = (uint32_t)((uint64_t)room_for(k, r->pad) / 4u);
    if (max > (uint32_t)KIT_FREEZE_S * t->cfg.fs)
        max = (uint32_t)KIT_FREEZE_S * t->cfg.fs;
    if (v) {
        v->sg[0] = v->sg[1] = v->sg[2] = 0.0f;         /* (no sends) */
        if (v->mode != B_ONE) {                         /* (held: the region once; pages v2: Warp's grains at their
                                                         * speed) */
            const double n = (v->e - v->s) / (v->warp >= KIT_W_GRAINS || v->eng == KIT_M_GRANULAR
                                                  ? (v->eng == KIT_M_GRANULAR ? v->ginc : v->spd) : v->inc);
            gate_n = n < 1 ? 1 : n > max ? max : (uint32_t)n;
            gated = 0;
        }
    }
    while (v && v->on && no < max) {
        static float buf[2 * 1024];
        float *sb[3] = { buf, buf, buf };
        uint32_t n = max - no < 1024 ? max - no : 1024, j;
        if (!gated && no >= gate_n) {                   /* (let go: its release) */
            gated = 1;
            v->gate = 0;
        }
        if (!gated && gate_n - no < n)
            n = gate_n - no;
        if (no + n > cap) {
            int16_t *g = realloc(o, (size_t)(cap = cap ? cap * 2 : 1u << 16) * 4u);
            if (!g)
                break;
            o = g;
            continue;
        }
        memset(buf, 0, sizeof(float) * 2u * n);
        voice_run(t, v, buf, sb, n);
        for (j = 0; j < 2 * n; j++) {
            const long x = lrintf(buf[j]);
            o[2 * no + j] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        no += n;
    }
    free(src);
    if (!max)
        a.a.status = KIT_TOO_LONG;
    if (o && no) {
        base_name(r->path, b, 16);
        snprintf(name, sizeof name, "%s frz", b);
        if (!new_path(k, r->proj, name, path, sizeof path) && !write_wav(path, o, no, t->cfg.fs)) {
            a.pcm = o, a.a.status = KIT_OK, a.a.frames = no, a.a.rate = t->cfg.fs;
            core_path(r->proj, path, a.a.path, sizeof a.a.path);
            snprintf(a.abs, sizeof a.abs, "%s", path);
            snprintf(k->wlast[r->pad], sizeof k->wlast[0], "%s", path);
            base_name(path, a.a.name, sizeof a.a.name);
            a.a.npk = peaks_of(o, no, KIT_PEAKS, a.a.pk);
            shared_put(k, path, o, no, t->cfg.fs);
            o = NULL;
        }
    }
    free(o);
    free(t);
    answer(k, &a);
}
