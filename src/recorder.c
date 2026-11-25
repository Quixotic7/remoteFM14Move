/* SPDX-License-Identifier: MPL-2.0 */
/* recorder.h
 *
 * One ring a stream (the capture memory + 2 s), written by the engine every block whether a take runs or not, and
 * read by the writer: a take is only a first frame, a last frame and its streams, sent to the writer as commands
 * through a lock-free queue. The engine never overwrites frames the writer still needs (its hold, and the first
 * frame of each take it has not picked up yet): it drops the block instead, ends a running take there
 * (REC_END_OVERRUN) and keeps captures from reaching back over the dropped frames. Bar lines are found from each
 * block's beat and tempo (the first frame at or after the line), so a tempo change during a take still cuts on bar
 * lines: a take of N bars is N bars of beats, however long they took. */
#define _GNU_SOURCE
#include "recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the rings go to disk as they are: a little-endian host"
#endif

#define LD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define LDX(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define STX(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

#define QN 64          /* commands, engine to writer */
#define ON 64          /* finished takes not polled yet (more: the oldest go) */
#define NLINES 64      /* bar lines kept (Capture goes back 16 at most) */
#define WCHUNK 4096    /* frames the writer writes at a time */
#define SILENT 65      /* a sample over it is loud: -54 dBFS (32768 * 10^(-54/20) = 65.4) */
#define NOLOUD 0xFFFFu
#define NOHOLD UINT64_MAX
#define NONE UINT64_MAX

enum { CMD_START, CMD_END, CMD_NOTHING };

typedef struct {
    int kind;
    uint32_t id;
    uint64_t f;          /* START: the first frame; END: one past the last */
    unsigned mask;       /* START: the streams */
    int source;          /* START, NOTHING: a stream or REC_SRC_STEMS */
    time_t t;            /* START: the wall clock at the first frame */
    int reason;          /* END, NOTHING */
    uint32_t bars;       /* END: 0, not bar-cut */
    double bpm;          /* END */
    char label[REC_NST][REC_LABEL];   /* START */
} cmd_t;

typedef struct {
    int source, mode, len, thr;
    char label[REC_NST][REC_LABEL];
} set_t;

typedef struct {   /* a block (chunk) the engine wrote: its loud frames per stream, for Capture's silences */
    uint64_t f0;
    uint32_t n;
    uint16_t first[REC_NST], last[REC_NST];   /* offsets (NOLOUD: none) */
} meta_t;

typedef struct {   /* the writer's take */
    cmd_t c;
    int have_end, reason, err;
    uint64_t end, rd, hdr_at, df_at;
    uint32_t bars;
    double bpm;
    char dir[1024];
    char head[32];               /* <MMDD>-<HHMMSS> */
    char sdir[1024];             /* a stems take: its folder while it records ("" not stems) */
    char name[REC_NST][256];
    char part[REC_NST][1024];
    FILE *f[REC_NST];
} wtake_t;

struct rec {
    char *dir;
    int day;
    uint32_t fs, blk, cap, R, maxf, pre, win, nmeta;
    uint64_t min_free;
    unsigned have;
    time_t (*now)(void);
    int16_t *ring[REC_NST];
    meta_t *meta;
    uint16_t *fpk;               /* the source's peak per frame of a chunk */

    /* shared (atomics, or under a mutex) */
    uint64_t pos;                /* frames in the rings */
    uint32_t presses;
    int dirty;
    pthread_mutex_t smu;         /* pend */
    set_t pend;
    cmd_t q[QN];
    uint32_t qhead, qtail;       /* the engine moves head, the writer tail */
    uint64_t whold;              /* the writer still reads from here (NOHOLD) */
    uint32_t wabort;             /* the take the writer stopped (disk low, write error) */
    int wbusy, quit;
    uint32_t delay_us;
    int s_state, s_peak, s_src, s_mode, s_len, s_thr;
    uint64_t s_frames;
    uint32_t s_bars, s_over, hist_w, hist_n;
    uint8_t hist[REC_HIST];
    pthread_mutex_t omu;         /* out */
    rec_take_t out[ON];
    uint32_t oh, on;

    /* the engine's */
    set_t cur;
    uint32_t seen_presses, nl, next_id, acc_n;
    int state, was_playing, acc_pk;
    uint64_t mcount, gap_hi;
    uint64_t lines[NLINES];
    double last_line, exp_beat;  /* (exp_beat: where the song is at the next block if it just plays on) */
    uint64_t seen[REC_NST];      /* one past the last frame each stream was present */
    unsigned present;
    /* the take (recording) and the arm */
    uint32_t id;
    unsigned mask;
    int tsrc, bar_take, end_set;
    uint64_t start, arm_f, end_frame;
    double start_beat, end_beat, bpm0;

    pthread_t th;
    int th_on;
    wtake_t *w;
};

static const char *const DEF_LABEL[REC_NST] = { "Master", "Slot 1", "Slot 2", "Slot 3", "Input", "Looper", "Kit" };

/* ------------------------------------------------------------------ the engine's side --- */
static uint32_t q_free(rec_t *r) { return QN - (r->qhead - LD(&r->qtail)); }

static void q_post(rec_t *r, const cmd_t *c)
{
    r->q[r->qhead % QN] = *c;
    ST(&r->qhead, r->qhead + 1);
}

/* the first frame the writer still needs: its own place, and each take it has not picked up yet */
static uint64_t hold(rec_t *r)
{
    uint32_t t = LD(&r->qtail);
    const uint32_t h = r->qhead;
    uint64_t m = LD(&r->whold);
    for (; t != h; t++)
        if (r->q[t % QN].kind == CMD_START && r->q[t % QN].f < m)
            m = r->q[t % QN].f;
    return m;
}

/* the oldest frame a take may start at (the capture memory, not over dropped frames) */
static uint64_t avail_lo(rec_t *r, uint64_t f0)
{
    const uint64_t lo = f0 > r->cap ? f0 - r->cap : 0;
    return lo > r->gap_hi ? lo : r->gap_hi;
}

static void nothing(rec_t *r, int source)
{
    cmd_t c;
    if (q_free(r) < 1)
        return;
    memset(&c, 0, sizeof c);
    c.kind = CMD_NOTHING;
    c.id = ++r->next_id;
    c.source = source;
    c.reason = REC_END_NOTHING;
    q_post(r, &c);
}

/* a take from frame f (f0: this block's first frame; live: a take that records from now on, not a capture's);
 * 0: not started (no streams, the queue full) */
static int start_take(rec_t *r, uint64_t f, uint64_t f0, unsigned mask, int live, int bar_take, double beat, double bpm)
{
    cmd_t c;
    time_t t = r->now ? r->now() : time(NULL);
    int s;
    if (!mask || q_free(r) < 2) {   /* (2: its end always fits) */
        nothing(r, r->cur.source);
        r->state = REC_IDLE;
        return 0;
    }
    memset(&c, 0, sizeof c);
    c.kind = CMD_START;
    c.id = ++r->next_id;
    c.f = f;
    c.mask = mask;
    c.source = r->cur.source;
    c.t = t - (time_t)(f < f0 ? (f0 - f) / r->fs : 0);
    for (s = 0; s < REC_NST; s++)
        memcpy(c.label[s], r->cur.label[s], REC_LABEL);
    q_post(r, &c);
    r->id = c.id;
    r->mask = mask;
    r->tsrc = c.source;
    r->start = f;
    r->bar_take = bar_take;
    r->start_beat = beat;
    r->bpm0 = bpm;
    r->end_set = 0;
    r->end_frame = NONE;
    r->state = REC_RECORDING;
    if (live) {
        STX(&r->hist_n, 0u);   /* (the wave: the take's own) */
        r->acc_n = 0;
        r->acc_pk = 0;
    }
    return 1;
}

static void end_take(rec_t *r, uint64_t e, int reason, uint32_t bars, double bpm)
{
    cmd_t c;
    memset(&c, 0, sizeof c);
    c.kind = CMD_END;
    c.id = r->id;
    c.f = e > r->start ? e : r->start;
    c.reason = reason;
    c.bars = bars;
    c.bpm = bpm;
    q_post(r, &c);   /* (fits: start_take kept room) */
    r->state = REC_IDLE;
}

/* the loud frames of mask in [a, b), inside chunk m: the first and last; 0: none */
static int loud_in(rec_t *r, const meta_t *m, unsigned mask, uint64_t a, uint64_t b, uint64_t *fl, uint64_t *ll)
{
    int s, any = 0;
    for (s = 0; s < REC_NST; s++) {
        uint64_t f, x = NONE, y = 0;
        if (!(mask >> s & 1) || m->first[s] == NOLOUD)
            continue;
        if (a <= m->f0 && b >= m->f0 + m->n) {
            x = m->f0 + m->first[s];
            y = m->f0 + m->last[s];
        } else {   /* (a part of the chunk: its samples) */
            for (f = a; f < b; f++) {
                const int16_t *p = r->ring[s] + 2u * (f % r->R);
                if (abs(p[0]) > SILENT || abs(p[1]) > SILENT) {
                    if (x == NONE)
                        x = f;
                    y = f;
                }
            }
            if (x == NONE)
                continue;
        }
        if (!any || x < *fl)
            *fl = x;
        if (!any || y > *ll)
            *ll = y;
        any = 1;
    }
    return any;
}

/* where the last silence of `need` frames or more in [lo, e) ends (its first loud frame after), loud frames at the
 * end of the range not counting; lo when none (the sound reaches back past lo); *loud 0: [lo, e) holds none */
static uint64_t silence_end(rec_t *r, unsigned mask, uint64_t lo, uint64_t e, uint64_t need, int *loud)
{
    uint64_t run = NONE, k, fl = 0, ll = 0;   /* run: the earliest loud frame seen */
    for (k = r->mcount; k-- > 0 && r->mcount - k < r->nmeta;) {
        const meta_t *m = &r->meta[k % r->nmeta];
        const uint64_t a = m->f0 > lo ? m->f0 : lo, b = m->f0 + m->n < e ? m->f0 + m->n : e;
        if (m->f0 + m->n <= lo)
            break;
        if (a >= b || !loud_in(r, m, mask, a, b, &fl, &ll))
            continue;
        if (run != NONE && run - (ll + 1) >= need) {
            *loud = 1;
            return run;
        }
        run = fl;
    }
    *loud = run != NONE;
    if (run == NONE)
        return e;
    return run - lo >= need ? run : lo;
}

static void capture(rec_t *r, uint64_t f0, int playing, double bpm)
{
    const uint64_t lo = avail_lo(r, f0);
    const int src = r->cur.source;
    unsigned mask = 0;
    uint64_t start, end = f0;
    uint32_t bars = 0;
    int s, loud;
    for (s = 0; s < REC_NST; s++)
        if ((r->have >> s & 1) && (src == REC_SRC_STEMS || s == src) && r->seen[s] > lo)
            mask |= 1u << s;
    if (!mask) {
        nothing(r, src);
        return;
    }
    if (playing && bpm > 0) {   /* whole bars, ending on the last bar line passed */
        uint64_t L[17];
        uint32_t i, k = 0, nb;
        const uint32_t kept = r->nl < NLINES ? r->nl : NLINES;
        for (i = 0; i < kept && k < 17; i++) {
            const uint64_t f = r->lines[(r->nl - 1 - i) % NLINES];
            if (f > f0)
                continue;
            if (f < lo)
                break;
            L[k++] = f;
        }
        nb = k ? k - 1 : 0;
        if (r->cur.len && (uint32_t)r->cur.len < nb)
            nb = (uint32_t)r->cur.len;
        while (nb && L[0] - L[nb] > r->maxf)
            nb--;
        if (!nb) {
            nothing(r, src);
            return;
        }
        if (r->cur.len) {
            silence_end(r, mask, L[nb], L[0], NONE, &loud);
        } else {   /* Auto: back to the last silence of 2 bars or more */
            const uint64_t Z = silence_end(r, mask, L[nb], L[0], (uint64_t)(8.0 * 60.0 * r->fs / bpm), &loud);
            for (i = 1; i < nb && L[i] > Z; i++)
                ;
            nb = i;   /* (the bar line at or before the sound's first frame) */
        }
        if (!loud) {
            nothing(r, src);
            return;
        }
        start = L[nb];
        end = L[0];
        bars = nb;
    } else {   /* stopped: back to the last silence of 2 s or more, ending now */
        start = silence_end(r, mask, lo, f0, 2u * (uint64_t)r->fs, &loud);
        if (!loud) {
            nothing(r, src);
            return;
        }
        if (f0 - start > r->maxf)
            start = f0 - r->maxf;
    }
    {
        const int st = r->state;
        if (start_take(r, start, f0, mask, 0, 0, 0.0, bpm))
            end_take(r, end, REC_END_OK, bars, bpm);
        r->state = st;   /* (Capture has no states of its own) */
    }
}

static void press(rec_t *r, uint64_t f0, int playing, double beat, double bpm, unsigned srcmask)
{
    switch (r->cur.mode) {
    case REC_MODE_CAPTURE:
        if (r->state != REC_RECORDING)
            capture(r, f0, playing, bpm);
        break;
    case REC_MODE_BARS:
        if (r->state == REC_IDLE) {
            r->state = REC_ARMED;
            r->arm_f = f0;
        } else if (r->state == REC_ARMED) {
            r->state = REC_IDLE;
        } else if (playing && bpm > 0) {   /* recording: it ends at the next bar line */
            const double bpf = bpm / 60.0 / r->fs;
            double B = 4.0 * ceil((beat - bpf) / 4.0);
            if (ceil((B - beat) / bpf - 1e-6) < 0)
                B += 4.0;
            while (B <= r->start_beat + 1e-6)
                B += 4.0;
            if (!r->end_set || B < r->end_beat) {
                r->end_beat = B;
                r->end_set = 1;
            }
        } else {
            end_take(r, f0, REC_END_OK, 0, r->bpm0);
        }
        break;
    case REC_MODE_THRESHOLD:
        if (r->state == REC_IDLE)
            r->state = REC_ARMED;
        else if (r->state == REC_ARMED)
            r->state = REC_IDLE;
        else
            end_take(r, f0, REC_END_OK, 0, r->bpm0);
        break;
    default:   /* Manual */
        if (r->state == REC_RECORDING)
            end_take(r, f0, REC_END_OK, 0, r->bpm0);
        else
            start_take(r, f0, f0, srcmask, 1, 0, beat, bpm);
        break;
    }
}

static void hist_push(rec_t *r, int v)
{
    const uint32_t w = r->hist_w, n = LDX(&r->hist_n);
    int x = (v * 255 + 16383) / 32767;
    STX(&r->hist[w % REC_HIST], (uint8_t)(x > 255 ? 255 : x));
    STX(&r->hist_w, w + 1);
    if (n < REC_HIST)
        STX(&r->hist_n, n + 1);
}

static void chunk(rec_t *r, const int16_t *const *st, uint32_t n, int playing, double beat, double bpm)
{
    const uint64_t f0 = r->pos;
    const int started = playing && !r->was_playing, tempo = playing && bpm > 0;
    meta_t *m = &r->meta[r->mcount % r->nmeta];
    uint64_t lf[8];
    double lb[8];
    int nlf = 0, s, drop, peak = 0;
    unsigned srcmask;
    uint32_t i, p;

    if (r->state == REC_RECORDING && LD(&r->wabort) == r->id)
        r->state = REC_IDLE;   /* (the writer stopped it: disk low, a write error) */
    if (r->state != REC_RECORDING && LDX(&r->dirty) && !pthread_mutex_trylock(&r->smu)) {
        const int mode = r->cur.mode;
        r->cur = r->pend;
        STX(&r->dirty, 0);
        pthread_mutex_unlock(&r->smu);
        if (r->state == REC_ARMED && r->cur.mode != mode)
            r->state = REC_IDLE;
        STX(&r->s_src, r->cur.source);
        STX(&r->s_mode, r->cur.mode);
        STX(&r->s_len, r->cur.len);
        STX(&r->s_thr, r->cur.thr);
    }

    /* the streams: present, peaks, loud frames; the source's peak per frame */
    r->present = 0;
    srcmask = 0;
    for (s = 0; s < REC_NST; s++)
        if (st[s] && (r->cur.source == REC_SRC_STEMS || s == r->cur.source))
            srcmask |= 1u << s;
    memset(r->fpk, 0, n * sizeof *r->fpk);
    m->f0 = f0;
    m->n = n;
    for (s = 0; s < REC_NST; s++) {
        const int16_t *x = st[s];
        uint16_t first = NOLOUD, last = NOLOUD;
        int mx = 0;
        if (x) {
            r->present |= 1u << s;
            for (i = 0; i < n; i++) {
                const int a = abs(x[2 * i]), b = abs(x[2 * i + 1]), v = a > b ? a : b;
                if (v > mx)
                    mx = v;
                if (v > SILENT) {
                    if (first == NOLOUD)
                        first = (uint16_t)i;
                    last = (uint16_t)i;
                }
                if ((srcmask >> s & 1) && v > r->fpk[i])
                    r->fpk[i] = (uint16_t)v;
            }
        }
        m->first[s] = first;
        m->last[s] = last;
        if ((srcmask >> s & 1) && mx > peak)
            peak = mx;
    }

    /* the song's position moved by itself while it plays (back to bar 1, a jump): the bar lines are the new
     * position's from this frame on, the old ones are forgotten (Capture then reaches back to here at most); a running
     * take keeps the beats it had left */
    if (tempo && r->was_playing) {
        const double d = beat - r->exp_beat;
        if (d < -0.1 || d > 0.25) {
            r->nl = 0;
            r->start_beat += d;
            r->end_beat += d;
        }
    }

    /* the bar lines in this block (the transport's first frame is one) */
    if (started) {
        r->nl = 0;
        r->lines[r->nl++ % NLINES] = f0;
        r->last_line = beat;
        lf[nlf] = f0;
        lb[nlf++] = beat;
    }
    if (tempo) {
        const double bpf = bpm / 60.0 / r->fs;
        double B = 4.0 * ceil((beat - bpf) / 4.0);
        for (; nlf < 8; B += 4.0) {
            const double o = ceil((B - beat) / bpf - 1e-6);
            if (o >= n)
                break;
            if (o < 0 || (r->nl && B <= r->last_line + 1e-6))
                continue;
            r->lines[r->nl++ % NLINES] = f0 + (uint64_t)o;
            r->last_line = B;
            lf[nlf] = f0 + (uint64_t)o;
            lb[nlf++] = B;
        }
    }

    /* frames the writer still needs are never overwritten: the block is dropped */
    drop = f0 + n > r->R && f0 + n - r->R > hold(r);
    if (drop) {
        r->gap_hi = f0 + n;
        STX(&r->s_over, r->s_over + 1);
        if (r->state == REC_RECORDING)
            end_take(r, f0, REC_END_OVERRUN, 0, r->bpm0);
    }

    p = LD(&r->presses);
    for (; r->seen_presses != p; r->seen_presses++)
        press(r, f0, playing, beat, bpm, srcmask);

    if (r->state == REC_ARMED && !drop) {   /* the start */
        if (r->cur.mode == REC_MODE_BARS && playing) {
            for (s = 0; s < nlf; s++)
                if (lf[s] >= r->arm_f) {
                    if (start_take(r, lf[s], f0, srcmask, 1, 1, lb[s], bpm) && r->cur.len) {
                        r->end_beat = lb[s] + 4.0 * r->cur.len;   /* (N bars of beats) */
                        r->end_set = 1;
                    }
                    break;
                }
        } else if (r->cur.mode == REC_MODE_THRESHOLD && peak >= r->cur.thr) {
            const uint64_t lo = avail_lo(r, f0), f = f0 > lo + r->pre ? f0 - r->pre : lo;
            if (start_take(r, f, f0, srcmask, 1, 0, beat, bpm) && r->cur.len && bpm > 0)
                r->end_frame = f + (uint64_t)llround(r->cur.len * 240.0 * r->fs / bpm);
        }
    }

    if (r->state == REC_RECORDING) {   /* the end */
        uint64_t e = NONE;
        int why = REC_END_OK;
        uint32_t bars = 0;
        if (r->bar_take && !playing) {
            e = f0;
            why = REC_END_TRANSPORT;
        } else {
            if (r->bar_take && r->end_set && tempo) {
                const double o = ceil((r->end_beat - beat) / (bpm / 60.0 / r->fs) - 1e-6);
                if (o < n) {
                    e = f0 + (uint64_t)(o > 0 ? o : 0);
                    bars = (uint32_t)llround((r->end_beat - r->start_beat) / 4.0);
                }
            }
            if (r->end_frame != NONE && r->end_frame <= f0 + n && r->end_frame < e) {
                e = r->end_frame > f0 ? r->end_frame : f0;
                bars = (uint32_t)r->cur.len;
            }
            if (r->start + r->maxf <= f0 + n && r->start + r->maxf < e) {
                e = r->start + r->maxf;
                why = REC_END_MAX_LENGTH;
                bars = 0;
            }
        }
        if (e != NONE)
            end_take(r, e, why, bars, r->bpm0);
    }

    /* the audio: every stream kept, absent ones as silence */
    if (!drop) {
        const uint32_t at = (uint32_t)(f0 % r->R), n1 = r->R - at < n ? r->R - at : n;
        for (s = 0; s < REC_NST; s++) {
            int16_t *d = r->ring[s];
            if (!(r->have >> s & 1))
                continue;
            if (st[s]) {
                memcpy(d + 2u * at, st[s], n1 * 4u);
                memcpy(d, st[s] + 2u * n1, (n - n1) * 4u);
                r->seen[s] = f0 + n;
            } else {
                memset(d + 2u * at, 0, n1 * 4u);
                memset(d, 0, (n - n1) * 4u);
            }
        }
    }
    r->mcount++;
    ST(&r->pos, f0 + n);
    r->was_playing = playing;
    r->exp_beat = beat + (tempo ? n * bpm / 60.0 / r->fs : 0.0);

    /* the status */
    STX(&r->s_peak, peak);
    for (i = 0; i < n; i++) {
        if (r->fpk[i] > r->acc_pk)
            r->acc_pk = r->fpk[i];
        if (++r->acc_n >= r->win) {
            hist_push(r, r->acc_pk);
            r->acc_n = 0;
            r->acc_pk = 0;
        }
    }
    if (r->state == REC_RECORDING) {
        const uint64_t fr = f0 + n > r->start ? f0 + n - r->start : 0;
        uint32_t bars = 0;
        if (r->bar_take && tempo) {
            const double b = (beat + n * bpm / 60.0 / r->fs - r->start_beat) / 4.0;
            bars = b > 0 ? (uint32_t)(b + 1e-9) : 0;
        } else if (r->bpm0 > 0) {
            bars = (uint32_t)(fr / (240.0 * r->fs / r->bpm0));
        }
        STX(&r->s_frames, fr);
        STX(&r->s_bars, bars);
    } else {
        STX(&r->s_frames, (uint64_t)0);
        STX(&r->s_bars, 0u);
    }
    STX(&r->s_state, r->state);
}

void rec_block(rec_t *r, const rec_block_t *b)
{
    uint32_t done = 0;
    while (done < b->frames) {   /* (longer than a block: in blocks, each with its beat) */
        const uint32_t n = b->frames - done < r->blk ? b->frames - done : r->blk;
        const int16_t *st[REC_NST];
        int s;
        for (s = 0; s < REC_NST; s++)
            st[s] = b->st[s] && (r->have >> s & 1) ? b->st[s] + 2u * done : NULL;
        chunk(r, st, n, b->playing, b->beat + (b->playing ? done * b->bpm / 60.0 / r->fs : 0.0), b->bpm);
        done += n;
    }
}

void rec_press(rec_t *r) { __atomic_add_fetch(&r->presses, 1u, __ATOMIC_RELEASE); }

/* ------------------------------------------------------------------ the settings, the status --- */
static void set_begin(rec_t *r) { pthread_mutex_lock(&r->smu); }
static void set_end(rec_t *r)
{
    STX(&r->dirty, 1);
    pthread_mutex_unlock(&r->smu);
}

void rec_set_source(rec_t *r, int source)
{
    if (source != REC_SRC_STEMS && (source < 0 || source >= REC_NST))
        return;
    set_begin(r);
    r->pend.source = source;
    set_end(r);
}

void rec_set_mode(rec_t *r, int mode)
{
    if (mode < REC_MODE_CAPTURE || mode > REC_MODE_MANUAL)
        return;
    set_begin(r);
    r->pend.mode = mode;
    set_end(r);
}

void rec_set_length_bars(rec_t *r, int bars)
{
    set_begin(r);
    r->pend.len = bars < 0 ? 0 : bars > 32 ? 32 : bars;
    set_end(r);
}

void rec_set_threshold(rec_t *r, int peak)
{
    set_begin(r);
    r->pend.thr = peak < 0 ? 0 : peak > 32767 ? 32767 : peak;
    set_end(r);
}

void rec_set_label(rec_t *r, int stream, const char *label)
{
    if (stream < 0 || stream >= REC_NST)
        return;
    set_begin(r);
    snprintf(r->pend.label[stream], REC_LABEL, "%s", label ? label : "");
    set_end(r);
}

void rec_get(rec_t *r, rec_status_t *s)
{
    uint32_t i, n, w;
    const uint32_t h = LD(&r->qhead), t = LD(&r->qtail);
    memset(s, 0, sizeof *s);
    s->state = LDX(&r->s_state);
    if (s->state == REC_IDLE && (h != t || LD(&r->wbusy)))
        s->state = REC_SAVING;
    s->source = LDX(&r->s_src);
    s->mode = LDX(&r->s_mode);
    s->length_bars = LDX(&r->s_len);
    s->threshold = LDX(&r->s_thr);
    s->frames = LDX(&r->s_frames);
    s->bars = LDX(&r->s_bars);
    s->peak = LDX(&r->s_peak);
    s->overruns = LDX(&r->s_over);
    w = LDX(&r->hist_w);
    n = LDX(&r->hist_n);
    n = n < REC_HIST ? n : REC_HIST;
    for (i = 0; i < n; i++)
        s->hist[i] = LDX(&r->hist[(w - n + i) % REC_HIST]);
    s->hist_n = (int)n;
}

int rec_poll_take(rec_t *r, rec_take_t *t)
{
    int got = 0;
    pthread_mutex_lock(&r->omu);
    if (r->on) {
        *t = r->out[r->oh % ON];
        r->oh++;
        r->on--;
        got = 1;
    }
    pthread_mutex_unlock(&r->omu);
    return got;
}

void rec_test_write_delay(rec_t *r, uint32_t us) { STX(&r->delay_us, us); }

/* ------------------------------------------------------------------ the writer --- */
static void result(rec_t *r, const char *path, int stream, uint32_t id, uint64_t frames, uint32_t bars, double bpm,
                   int reason)
{
    rec_take_t *t;
    pthread_mutex_lock(&r->omu);
    if (r->on == ON) {   /* (nobody polls: the oldest go) */
        r->oh++;
        r->on--;
    }
    t = &r->out[(r->oh + r->on) % ON];
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->stream = stream;
    t->take = id;
    t->first = r->w->c.id == id ? r->w->c.f : 0;   /* (the writer's take; a NOTHING has none) */
    t->frames = frames;
    t->bars = bars;
    t->bpm = bpm;
    t->reason = reason;
    r->on++;
    pthread_mutex_unlock(&r->omu);
}

/* a label as a file name's part: letters, digits, space and _ - . , ( ) + (others: _), no dots or spaces at the ends */
static void sanitize(char *d, size_t dn, const char *s, const char *def)
{
    size_t k = 0;
    int us = 0;
    for (; *s && k + 1 < dn && k < 40; s++) {
        const unsigned char c = (unsigned char)*s;
        const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" _-.,()+", c);
        if (ok) {
            d[k++] = (char)c;
            us = 0;
        } else if (!us) {
            d[k++] = '_';
            us = 1;
        }
    }
    d[k] = 0;
    while (k && (d[k - 1] == ' ' || d[k - 1] == '.'))
        d[--k] = 0;
    for (k = 0; d[k] == ' ' || d[k] == '.'; k++)
        ;
    memmove(d, d + k, strlen(d + k) + 1);
    if (!*d)
        snprintf(d, dn, "%s", def);
}

static int mkdirs(const char *path)
{
    char p[1024];
    size_t i;
    snprintf(p, sizeof p, "%s", path);
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            if (mkdir(p, 0755) && errno != EEXIST)
                return -1;
            p[i] = '/';
        }
    if (mkdir(p, 0755) && errno != EEXIST)
        return -1;
    {
        struct stat sb;
        return !stat(p, &sb) && S_ISDIR(sb.st_mode) ? 0 : -1;
    }
}

static int disk_low(rec_t *r, const char *dir)
{
    struct statvfs v;
    if (statvfs(dir, &v))
        return 0;   /* (no answer: not low) */
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize < r->min_free;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t *h, uint32_t fs, uint64_t frames)
{
    const uint32_t data = (uint32_t)(frames * 4u);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36u + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put32(h + 20, 1u | 2u << 16);         /* PCM, 2 channels */
    put32(h + 24, fs);
    put32(h + 28, fs * 4u);
    put32(h + 32, 4u | 16u << 16);        /* 4 bytes a frame, 16 bits */
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

/* the header in place with the frames so far (the stream's position stays at the end) */
static int header_update(rec_t *r, FILE *f, uint64_t frames)
{
    uint8_t h[44];
    wav_header(h, r->fs, frames);
    return fflush(f) == 0 && pwrite(fileno(f), h, 44, 0) == 44 ? 0 : -1;
}

static void close_files(wtake_t *w, int remove_them)
{
    int s;
    for (s = 0; s < REC_NST; s++)
        if (w->f[s]) {
            fclose(w->f[s]);
            w->f[s] = NULL;
            if (remove_them)
                unlink(w->part[s]);
        }
}

/* a stems file's name: Master, Track 1 <its firmware> (Track 1 with no label), Input, Looper, Kit */
static void stem_name(char *d, size_t dn, int s, const char *label)
{
    static const char *const FIXED[REC_NST] = { "Master", NULL, NULL, NULL, "Input", "Looper", "Kit" };
    char lab[REC_LABEL];
    if (FIXED[s]) {
        snprintf(d, dn, "%s", FIXED[s]);
        return;
    }
    sanitize(lab, sizeof lab, label, "");
    snprintf(d, dn, "Track %d%s%s", s - REC_ST_SLOT1 + 1, *lab ? " " : "", lab);
}

static void open_take(rec_t *r, wtake_t *w)
{
    struct tm tm;
    char *head = w->head;
    const int stems = w->c.source == REC_SRC_STEMS;
    int s;
    w->have_end = 0;
    w->err = 0;
    w->rd = w->c.f;
    w->hdr_at = w->df_at = w->c.f;
    w->sdir[0] = 0;
    memset(w->f, 0, sizeof w->f);
    localtime_r(&w->c.t, &tm);
    if (mkdirs(r->dir)) {
        w->err = REC_END_WRITE_ERROR;
        return;
    }
    if (r->day)
        snprintf(w->dir, sizeof w->dir, "%s/%04d-%02d-%02d", r->dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    else
        snprintf(w->dir, sizeof w->dir, "%s", r->dir);
    if (mkdirs(w->dir)) {
        w->err = REC_END_WRITE_ERROR;
        return;
    }
    if (disk_low(r, w->dir)) {
        w->err = REC_END_DISK_LOW;
        return;
    }
    snprintf(head, sizeof w->head, "%02d%02d-%02d%02d%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (stems) {   /* a folder of its own, under a temporary name until the take is whole (finish names it) */
        int k;
        for (k = 1; k < 1000; k++) {
            if (k == 1)
                snprintf(w->sdir, sizeof w->sdir, "%s/%s Stems.part", w->dir, head);
            else
                snprintf(w->sdir, sizeof w->sdir, "%s/%s Stems (%d).part", w->dir, head, k);
            if (!mkdir(w->sdir, 0755))
                break;
            if (errno != EEXIST)
                k = 1000;
        }
        if (k >= 1000) {
            w->sdir[0] = 0;
            w->err = REC_END_WRITE_ERROR;
            return;
        }
    }
    for (s = 0; s < REC_NST; s++) {
        char lab[REC_LABEL + 16];
        uint8_t h[44];
        int fd = -1, k;
        if (!(w->c.mask >> s & 1))
            continue;
        if (stems) {   /* (a fresh folder: the files have their final names in it) */
            stem_name(w->name[s], sizeof w->name[s], s, w->c.label[s]);
            snprintf(w->part[s], sizeof w->part[s], "%s/%s.wav", w->sdir, w->name[s]);
            fd = open(w->part[s], O_WRONLY | O_CREAT | O_EXCL, 0644);
        } else if (s >= REC_ST_SLOT1 && s <= REC_ST_SLOT3) {   /* (a slot: "Track 2 Felucca", as a stems file: the
                                                         * take library knows its source by it) */
            stem_name(lab, sizeof lab, s, w->c.label[s]);
            snprintf(w->name[s], sizeof w->name[s], "%s %s", head, lab);
        } else {
            sanitize(lab, sizeof lab, w->c.label[s], DEF_LABEL[s]);
            snprintf(w->name[s], sizeof w->name[s], "%s %s", head, lab);
        }
        for (k = 1; k < 1000 && fd < 0 && !stems; k++) {
            if (k == 1)
                snprintf(w->part[s], sizeof w->part[s], "%s/%s.part", w->dir, w->name[s]);
            else
                snprintf(w->part[s], sizeof w->part[s], "%s/%s (%d).part", w->dir, w->name[s], k);
            fd = open(w->part[s], O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0 && errno != EEXIST)
                break;
        }
        wav_header(h, r->fs, 0);
        if (fd < 0 || !(w->f[s] = fdopen(fd, "wb")) || fwrite(h, 1, 44, w->f[s]) != 44) {
            if (fd >= 0 && !w->f[s]) {
                close(fd);
                unlink(w->part[s]);
            }
            close_files(w, 1);
            if (w->sdir[0])
                rmdir(w->sdir);
            w->sdir[0] = 0;
            w->err = REC_END_WRITE_ERROR;
            return;
        }
    }
}

static void num(char *d, size_t dn, double v)   /* 97.3, 120: no trailing zeros */
{
    size_t k;
    snprintf(d, dn, "%.2f", v);
    k = strlen(d);
    while (k && d[k - 1] == '0')
        d[--k] = 0;
    if (k && d[k - 1] == '.')
        d[--k] = 0;
}

static void finish(rec_t *r, wtake_t *w, int reason)
{
    const uint64_t frames = w->rd - w->c.f;
    const uint32_t bars = reason == REC_END_OK ? w->bars : 0;
    char tail[64], a[32], fin[1024];
    int s, any = 0;
    if (!frames && reason < REC_END_NOTHING) {   /* (an empty take: no file) */
        close_files(w, 1);
        if (w->sdir[0])
            rmdir(w->sdir);
        result(r, "", w->c.source, w->c.id, 0, 0, w->bpm, REC_END_NOTHING);
        return;
    }
    if (bars) {
        num(a, sizeof a, w->bpm);
        snprintf(tail, sizeof tail, " %sbpm %ub", a, bars);
    } else {
        num(a, sizeof a, (double)frames / r->fs);
        snprintf(tail, sizeof tail, " %ss", a);
    }
    if (w->sdir[0]) {   /* stems: every file whole, then the folder named (never over a take: mkdir claims the name) */
        unsigned had = 0;
        int k, done = 0;
        for (s = 0; s < REC_NST; s++)
            if (w->f[s]) {
                any = 1;
                had |= 1u << s;
                if (header_update(r, w->f[s], frames) || fsync(fileno(w->f[s])))
                    reason = reason < REC_END_NOTHING ? REC_END_WRITE_ERROR : reason;
                fclose(w->f[s]);
                w->f[s] = NULL;
            }
        for (k = 1; k < 1000 && any && !done; k++) {
            if (k == 1)
                snprintf(fin, sizeof fin, "%s/%s Stems%s", w->dir, w->head, tail);
            else
                snprintf(fin, sizeof fin, "%s/%s Stems%s (%d)", w->dir, w->head, tail, k);
            if (mkdir(fin, 0755)) {
                if (errno == EEXIST)
                    continue;
                break;
            }
            if (!rename(w->sdir, fin))   /* (onto the empty folder just made) */
                done = 1;
            else {
                rmdir(fin);
                break;
            }
        }
        if (!any)
            rmdir(w->sdir);
        for (s = 0; s < REC_NST; s++)
            if (had >> s & 1) {
                char p[1300];
                snprintf(p, sizeof p, "%s/%s.wav", fin, w->name[s]);
                result(r, done ? p : "", s, w->c.id, frames, done ? bars : 0, w->bpm, done ? reason : REC_END_WRITE_ERROR);
            }
        w->sdir[0] = 0;
        if (!any)
            result(r, "", w->c.source, w->c.id, 0, 0, w->bpm, reason);
        return;
    }
    for (s = 0; s < REC_NST; s++) {
        int k, done = 0;
        if (!w->f[s])
            continue;
        any = 1;
        if (header_update(r, w->f[s], frames) || fsync(fileno(w->f[s])))
            reason = reason < REC_END_NOTHING ? REC_END_WRITE_ERROR : reason;
        fclose(w->f[s]);
        w->f[s] = NULL;
        for (k = 1; k < 1000 && !done; k++) {   /* never over a take: link fails on a name that is there */
            if (k == 1)
                snprintf(fin, sizeof fin, "%s/%s%s.wav", w->dir, w->name[s], tail);
            else
                snprintf(fin, sizeof fin, "%s/%s%s (%d).wav", w->dir, w->name[s], tail, k);
            if (!link(w->part[s], fin)) {
                unlink(w->part[s]);
                done = 1;
            } else if (errno == EEXIST || !access(fin, F_OK)) {
                continue;
            } else if (!rename(w->part[s], fin)) {   /* (no hard links on this volume) */
                done = 1;
            } else {
                break;
            }
        }
        result(r, done ? fin : "", s, w->c.id, frames, done ? bars : 0, w->bpm,
               done ? reason : REC_END_WRITE_ERROR);
    }
    if (!any)
        result(r, "", w->c.source, w->c.id, 0, 0, w->bpm, reason);
}

/* the frames [rd, rd + n) of every file; 0, or -1 a write error */
static int write_frames(rec_t *r, wtake_t *w, uint32_t n)
{
    const uint32_t at = (uint32_t)(w->rd % r->R), n1 = r->R - at < n ? r->R - at : n;
    int s;
    for (s = 0; s < REC_NST; s++) {
        if (!w->f[s])
            continue;
        if (fwrite(r->ring[s] + 2u * at, 4, n1, w->f[s]) != n1 ||
            (n > n1 && fwrite(r->ring[s], 4, n - n1, w->f[s]) != n - n1))
            return -1;
    }
    return 0;
}

static void *writer(void *arg)
{
    rec_t *r = arg;
    wtake_t *w = r->w;
    int cur = 0;
    for (;;) {
        int busy = 0;
        const uint32_t t = r->qtail;
        if (t != LD(&r->qhead)) {
            const cmd_t *c = &r->q[t % QN];
            STX(&r->wbusy, 1);
            if (c->kind == CMD_NOTHING) {
                result(r, "", c->source, c->id, 0, 0, 0.0, c->reason);
                ST(&r->qtail, t + 1);
                busy = 1;
            } else if (!cur && c->kind == CMD_START) {
                w->c = *c;
                ST(&r->whold, c->f);   /* (before the tail: the engine counts the take until then) */
                ST(&r->qtail, t + 1);
                open_take(r, w);
                cur = 1;
                busy = 1;
            } else if (c->kind == CMD_END && (!cur || c->id != w->c.id)) {
                ST(&r->qtail, t + 1);   /* (a take the writer stopped itself) */
                busy = 1;
            } else if (cur && c->kind == CMD_END && !w->have_end) {
                w->have_end = 1;
                w->end = c->f;
                w->reason = c->reason;
                w->bars = c->bars;
                w->bpm = c->bpm;
                ST(&r->qtail, t + 1);
                busy = 1;
            }
        }
        if (cur && !w->err) {
            const uint64_t pos = LD(&r->pos), lim = w->have_end && w->end < pos ? w->end : pos;
            if (w->rd < lim) {
                const uint32_t n = lim - w->rd < WCHUNK ? (uint32_t)(lim - w->rd) : WCHUNK;
                const uint32_t d = LDX(&r->delay_us);
                int s;
                if (d)
                    usleep(d);
                if (write_frames(r, w, n))
                    w->err = REC_END_WRITE_ERROR;
                else {
                    w->rd += n;
                    ST(&r->whold, w->rd);
                }
                if (!w->err && w->rd - w->hdr_at >= r->fs) {   /* (about once a second: a crash leaves a readable file) */
                    w->hdr_at = w->rd;
                    for (s = 0; s < REC_NST; s++)
                        if (w->f[s] && header_update(r, w->f[s], w->rd - w->c.f))
                            w->err = REC_END_WRITE_ERROR;
                    if (!w->err && disk_low(r, w->dir))
                        w->err = REC_END_DISK_LOW;
                }
                busy = 1;
            }
        }
        if (cur && w->err) {
            finish(r, w, w->err);
            ST(&r->wabort, w->c.id);
            ST(&r->whold, NOHOLD);
            cur = 0;
            busy = 1;
        } else if (cur && w->have_end && w->rd >= w->end) {
            finish(r, w, w->reason);
            ST(&r->whold, NOHOLD);
            cur = 0;
            busy = 1;
        }
        if (!cur && r->qtail == LD(&r->qhead)) {
            STX(&r->wbusy, 0);
            if (LD(&r->quit))
                break;
        }
        if (!busy) {
            const struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ new, free --- */
void rec_free(rec_t *r)
{
    int s;
    if (!r)
        return;
    if (r->th_on) {
        if (r->state == REC_RECORDING)
            end_take(r, r->pos, REC_END_CLOSED, 0, r->bpm0);
        ST(&r->quit, 1);
        pthread_join(r->th, NULL);
        pthread_mutex_destroy(&r->smu);
        pthread_mutex_destroy(&r->omu);
    }
    for (s = 0; s < REC_NST; s++)
        free(r->ring[s]);
    free(r->meta);
    free(r->fpk);
    free(r->w);
    free(r->dir);
    free(r);
}

rec_t *rec_new(const rec_cfg_t *cfg)
{
    rec_t *r = calloc(1, sizeof *r);
    int s;
    if (!r)
        return NULL;
    r->dir = strdup(cfg->dir && *cfg->dir ? cfg->dir : ".");
    r->day = cfg->day_folders;
    r->fs = cfg->fs ? cfg->fs : 44100;
    r->blk = cfg->block_frames ? cfg->block_frames : 128;
    if (r->blk > 4096)
        r->blk = 4096;
    r->cap = (cfg->capture_s ? cfg->capture_s : 64) * r->fs;
    r->R = r->cap + 2u * r->fs;   /* (2 s for the writer to pick up a capture that reaches back the whole memory) */
    r->maxf = (cfg->max_take_s ? cfg->max_take_s : 1800) * r->fs;
    r->pre = r->fs / 20u;   /* 50 ms */
    r->win = r->fs * 16u / 1000u;   /* ~16 ms */
    r->min_free = cfg->min_free ? cfg->min_free : 200ull * 1000 * 1000;
    r->have = cfg->streams ? cfg->streams & ((1u << REC_NST) - 1) : (1u << REC_NST) - 1;
    r->now = cfg->now;
    r->nmeta = r->R / r->blk + 16u;
    r->meta = calloc(r->nmeta, sizeof *r->meta);
    r->fpk = calloc(r->blk, sizeof *r->fpk);
    r->w = calloc(1, sizeof *r->w);
    if (!r->dir || !r->meta || !r->fpk || !r->w) {
        rec_free(r);
        return NULL;
    }
    memset(r->meta, 0, (size_t)r->nmeta * sizeof *r->meta);   /* (touched now) */
    for (s = 0; s < REC_NST; s++)
        if (r->have >> s & 1) {
            if (!(r->ring[s] = malloc((size_t)r->R * 4u))) {
                rec_free(r);
                return NULL;
            }
            memset(r->ring[s], 0, (size_t)r->R * 4u);   /* (touched now, not on the engine thread) */
        }
    r->cur.source = REC_ST_MASTER;
    r->cur.mode = REC_MODE_CAPTURE;
    r->cur.thr = 1000;
    r->pend = r->cur;
    r->s_src = r->cur.source;
    r->s_mode = r->cur.mode;
    r->s_thr = r->cur.thr;
    r->whold = NOHOLD;
    r->end_frame = NONE;
    pthread_mutex_init(&r->smu, NULL);
    pthread_mutex_init(&r->omu, NULL);
    if (pthread_create(&r->th, NULL, writer, r)) {
        pthread_mutex_destroy(&r->smu);
        pthread_mutex_destroy(&r->omu);
        rec_free(r);
        return NULL;
    }
    r->th_on = 1;
    return r;
}
