/* SPDX-License-Identifier: MPL-2.0 */
/* The looper (src/looper.c) natively, its engine's thread here and its file thread its own: a recording block-exact at
 * the bar (its first frame the bar line's, its length whole bars of beats, its WAV the same frames), the loop playing
 * at once from the bar it ended on, a launch at the bar (and Instant, in phase), a stop, the tempo repitching it, the
 * mix path (the loop's level, pan, mute and sends, the lane's level, the Loops strip, the volume knob's master), a copy
 * and an emptied slot brought back from memory (Undo), Open and back-to-back recordings, the transport stopped during
 * one (whole bars kept), a cancel, the memory's cap, Warp Off, the clean-up, and no allocation on the engine's thread.
 * Ends with ALL LOOPER TESTS PASSED.
 *   tests/looper_test.sh */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "../src/looper.h"
#include "../src/wav.h"

pthread_t la_engine;
int la_engine_set;
long la_engine_allocs;

static int checks, fails;
#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        checks++;                                                                                                   \
        if (!(c)) {                                                                                                 \
            fails++;                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                  \
        }                                                                                                           \
    } while (0)
#define CHECKF(c, ...)                                                                                              \
    do {                                                                                                            \
        CHECK(c);                                                                                                   \
        if (!(c))                                                                                                   \
            fprintf(stderr, "  " __VA_ARGS__), fputc('\n', stderr);                                                   \
    } while (0)

#define FS 44100
#define N 128
static char PROJ[512];
static looper_t *L;
static uint64_t gf;                                     /* the song's frame (0 at Play) */
static int playing;
static double bpm = 120.0;
static float acc[2 * N], sbus[3][2 * N];
static int16_t own[2 * N], s1[2 * N];
static looper_ans_t ANS[64];
static int nans;

static int src_const;                                   /* (t_mix: a steady source) */
static const int16_t *mon_buf;                          /* (round 2: the monitored input, each block) */
static int16_t pat(uint64_t f) { return (int16_t)((int)(f % 16000u) + 100); }   /* (the source: a frame's own value) */
static double bpf(void) { return bpm / 60.0 / FS; }
static void blk(void)                                   /* one engine block */
{
    looper_clock_t c;
    looper_src_t src;
    looper_ans_t a;
    int i;
    c.playing = playing, c.bpf = bpf(), c.beat0 = playing ? (double)gf * bpf() : 0.0;
    for (i = 0; i < N; i++)
        s1[2 * i] = src_const ? 8000 : pat(gf + (uint64_t)i), s1[2 * i + 1] = (int16_t)(src_const ? -8000 : -pat(gf + (uint64_t)i));
    memset(acc, 0, sizeof acc), memset(sbus, 0, sizeof sbus);
    memset(&src, 0, sizeof src);
    src.slot[0] = s1;
    {
        float *sends[3] = { sbus[0], sbus[1], sbus[2] };
        if (mon_buf)
            looper_monitor(L, mon_buf);
        looper_render_bus(L, &c, acc, sends, own, N);
    }
    looper_record(L, &src, own, N);
    while (looper_poll(L, &a))
        if (nans < 64)
            ANS[nans++] = a;
    if (playing)
        gf += N;
}
/* a block's own output at its frame i: L */
static void run_blocks(int n)
{
    while (n-- > 0)
        blk();
}
static const looper_ans_t *wait_ans(int kind, int slot)  /* (the file thread's answer: blocks run meanwhile) */
{
    int t, i;
    for (t = 0; t < 4000; t++) {
        for (i = 0; i < nans; i++)
            if (ANS[i].kind == kind && ANS[i].slot == slot)
                return &ANS[i];
        blk();
        if (t % 50 == 49)
            usleep(1000);
    }
    return NULL;
}
static void ans_clear(void) { nans = 0; }
static void play_from_start(void) { gf = 0, playing = 1; }
static uint64_t bar_frame(int beat) { return (uint64_t)ceil(beat / bpf() - 1e-6); }   /* (the line's frame) */

/* ---- 1. a recording block-exact at the bar, its file, the loop at once from the bar it ended on */
static void t_record(void)
{
    const looper_ans_t *a;
    looper_status_t st;
    char path[1024];
    uint32_t frames = 0, i, bad = 0;
    const char *why;
    int16_t *w;
    play_from_start();
    run_blocks(100);                                    /* (inside the first bar) */
    looper_rec(L, 0, 1, LOOPER_SRC_TRACK1, 0, 10);
    blk();
    looper_status(L, &st);
    CHECK(st.rec[0].slot == 0 && st.rec[0].state == LOOPER_RS_ARMED);
    while (gf < bar_frame(4) + N)
        blk();
    looper_status(L, &st);
    CHECK(st.rec[0].slot == 0 && st.rec[0].state == LOOPER_RS_RECORDING && st.rec[0].bar == 1 && st.rec[0].bars == 1);
    while (gf < bar_frame(8))
        blk();
    blk();                                              /* (the block with the line at beat 8) */
    looper_status(L, &st);
    CHECK(st.rec[0].slot == -1 && st.lane[0].play == 0);   /* (it plays at once) */
    {                                                   /* the loop from the bar it ended on: the recording's frames */
        uint64_t f;
        int diff = 0, n = 0;
        while (gf < bar_frame(8) + 2000) {
            const uint64_t f0 = gf;
            blk();
            for (i = 0; i < N; i++) {
                f = f0 + i;
                if (f >= bar_frame(8) + 200 && f < bar_frame(8) + 1800) {
                    const int want = pat(bar_frame(4) + (f - bar_frame(8)));
                    const int d = abs(own[2 * i] - want);
                    diff = d > diff ? d : diff, n++;
                }
            }
        }
        CHECKF(n > 1000 && diff <= 2, "n %d diff %d", n, diff);
    }
    a = wait_ans(LOOPER_A_RECORDED, 0);
    CHECKF(a && a->status == LOOPER_OK && a->bars == 1 && a->frames == (uint32_t)(bar_frame(8) - bar_frame(4)) &&
           !strcmp(a->path, "@/Loops/1-1 120bpm 1b.wav") && a->bpm10 == 1200 && a->npk == LOOPER_PEAKS,
           "status %d bars %d frames %u path %s bpm10 %d", a ? a->status : -1, a ? a->bars : -1, a ? a->frames : 0,
           a ? a->path : "", a ? a->bpm10 : 0);
    looper_resolve(L, "@/Loops/1-1 120bpm 1b.wav", path, sizeof path);
    w = wav_read(path, &frames, &why);
    CHECK(w && frames == (uint32_t)(bar_frame(8) - bar_frame(4)));
    for (i = 0; w && i < frames; i++)                   /* (frame for frame the source from the bar line) */
        bad += w[2 * i] != pat(bar_frame(4) + i) || w[2 * i + 1] != -pat(bar_frame(4) + i);
    CHECKF(w && !bad, "%u frames differ", bad);
    free(w);
}

/* ---- 2. a launch at the bar, Instant in phase, a stop at the bar, the tempo */
static void t_launch(void)
{
    looper_status_t st;
    const looper_ans_t *a;
    uint64_t f, first = 0;
    int i;
    ans_clear();
    looper_stop(L, 0, LOOPER_Q_INSTANT, 0);
    run_blocks(10);
    looper_copy(L, 9, 0);                               /* (lane 2: the same audio, no file read) */
    a = wait_ans(LOOPER_A_LOADED, 9);
    CHECK(a && a->status == LOOPER_OK && a->frames == (uint32_t)(bar_frame(4)));
    while ((gf * bpf()) - 4.0 * floor(gf * bpf() / 4.0) < 1.0)
        blk();                                          /* (past a bar's first beat) */
    looper_launch(L, 9, LOOPER_Q_BAR, 64);
    blk();
    looper_status(L, &st);
    CHECK(st.lane[1].next == 1 && st.lane[1].play == -1);
    {
        const double nb = 4.0 * ceil(gf * bpf() / 4.0);
        const uint64_t line = (uint64_t)ceil(nb / bpf() - 1e-6);
        while (gf < line + 4 * N) {
            const uint64_t f0 = gf;
            blk();
            for (i = 0; i < N; i++)
                if (!first && own[2 * i])
                    first = f0 + i;
        }
        CHECKF(first >= line && first <= line + 2, "first sound %llu, the bar line %llu", (unsigned long long)first,
               (unsigned long long)line);                /* (its first frame faded in from 0) */
    }
    looper_status(L, &st);
    CHECK(st.lane[1].play == 1 && st.lane[1].next == -1);
    /* the tempo doubled: its place moves twice as fast (Repitch), still on the bar */
    {
        uint16_t p0, p1;
        double d1, d2;
        run_blocks(20);
        looper_status(L, &st), p0 = st.lane[1].pos;
        run_blocks(40);
        looper_status(L, &st), p1 = st.lane[1].pos;
        d1 = (double)(uint16_t)(p1 - p0);
        {   /* (the song's position continues at the new tempo: gf rescaled so the beat stays) */
            const double beat = gf * bpf();
            bpm = 240.0;
            gf = (uint64_t)llround(beat / bpf());
        }
        run_blocks(20);
        looper_status(L, &st), p0 = st.lane[1].pos;
        run_blocks(40);
        looper_status(L, &st), p1 = st.lane[1].pos;
        d2 = (double)(uint16_t)(p1 - p0);
        CHECKF(fabs(d2 / d1 - 2.0) < 0.05, "pos moved %.0f then %.0f", d1, d2);
        {                                               /* (the cycle still starts on the bar line) */
            const double b = gf * bpf();
            const double q = fmod(b, 4.0) / 4.0;
            looper_status(L, &st);
            CHECKF(fabs(st.lane[1].pos / 65535.0 - q) < 0.05 || fabs(st.lane[1].pos / 65535.0 - q) > 0.95,
                   "pos %u, the bar's %.3f", st.lane[1].pos, q);
        }
        bpm = 120.0;
        gf = (uint64_t)llround((double)gf * 2.0);
    }
    /* a stop at the bar */
    looper_stop(L, 1, LOOPER_Q_BAR, 0);
    blk();
    looper_status(L, &st);
    CHECK(st.lane[1].next == LOOPER_SCENES);
    run_blocks((int)(bar_frame(4) / N) + 2);
    looper_status(L, &st);
    CHECK(st.lane[1].play == -1);
    run_blocks(10);
    for (i = 0, f = 0; i < 2 * N; i++)
        f += own[i] != 0;
    CHECK(f == 0);
    /* Instant: now, in phase with the bar (its place the bar's) */
    looper_launch(L, 9, LOOPER_Q_INSTANT, 0);
    blk(), blk();
    looper_status(L, &st);
    {
        const double q = fmod(gf * bpf(), 4.0) / 4.0;
        CHECKF(st.lane[1].play == 1 && fabs(st.lane[1].pos / 65535.0 - q) < 0.02, "pos %u q %.3f", st.lane[1].pos, q);
    }
}

/* ---- 3. the mix path: the loop's level, pan, mute and sends, the lane's level, the strip, the master */
static double rms(void)
{
    double s = 0;
    int i;
    for (i = 0; i < 2 * N; i++)
        s += (double)own[i] * own[i];
    return sqrt(s / (2 * N));
}
static double avg_rms(int n)
{
    double s = 0;
    int k;
    for (k = 0; k < n; k++)
        blk(), s += rms();
    return s / n;
}
static void t_mix(void)
{
    double r0, r;
    float sb = 0;
    int i;
    const int sends0[3] = { 0, 0, 0 };
    const looper_ans_t *a;
    ans_clear();
    src_const = 1;                                      /* (a steady loop in lane 2: its level is the measure) */
    looper_rec(L, 10, 1, LOOPER_SRC_TRACK1, 0, 0);
    a = wait_ans(LOOPER_A_RECORDED, 10);
    CHECK(a && a->status == LOOPER_OK);
    src_const = 0;
    run_blocks(10);
    r0 = avg_rms(30);
    CHECK(r0 > 1000);
    looper_mix(L, 50, 64, sends0);                      /* (the strip at 50: -6 dB, half) */
    r = avg_rms(30);
    CHECKF(fabs(r / r0 - 0.5) < 0.03, "strip 50: %.3f", r / r0);
    looper_mix(L, 100, 64, sends0);
    looper_master(L, 0);                                /* (the volume knob down: silent) */
    r = avg_rms(5);
    CHECK(r == 0);
    looper_master(L, 1023);
    looper_lane(L, 1, LOOPER_L_LEVEL, 50);
    r = avg_rms(30);
    CHECKF(fabs(r / r0 - 0.5) < 0.03, "lane 50: %.3f", r / r0);
    looper_lane(L, 1, LOOPER_L_LEVEL, 100);
    looper_set(L, 10, LOOPER_P_LEVEL, 50);
    r = avg_rms(30);
    CHECKF(fabs(r / r0 - 0.5) < 0.03, "loop 50: %.3f", r / r0);
    looper_set(L, 10, LOOPER_P_LEVEL, 100);
    looper_set(L, 10, LOOPER_P_MUTE, 1);
    CHECK(avg_rms(5) == 0);
    looper_set(L, 10, LOOPER_P_MUTE, 0);
    looper_set(L, 10, LOOPER_P_PAN, 0);                 /* (hard left: the right side silent) */
    blk();
    for (i = 0; i < N; i++)
        CHECK(own[2 * i + 1] == 0);
    looper_set(L, 10, LOOPER_P_PAN, 64);
    looper_set(L, 10, LOOPER_P_SEND2, 127);             /* (its own send) */
    blk();
    for (i = 0; i < 2 * N; i++)
        sb += fabsf(sbus[1][i]);
    CHECK(sb > 1000);
    for (sb = 0, i = 0; i < 2 * N; i++)
        sb += fabsf(sbus[0][i]) + fabsf(sbus[2][i]);
    CHECK(sb == 0);
    looper_set(L, 10, LOOPER_P_SEND2, 0);
    {                                                   /* (the strip's send: the whole bus) */
        const int s3[3] = { 0, 0, 127 };
        looper_mix(L, 100, 64, s3);
        blk();
        for (sb = 0, i = 0; i < 2 * N; i++)
            sb += fabsf(sbus[2][i]);
        CHECK(sb > 1000);
        looper_mix(L, 100, 64, sends0);
    }
}

/* ---- 4. an emptied slot brought back from memory (Undo), Warp Off, Open and back-to-back recordings, a cancel, the
 * transport stopped during one */
static void t_more(void)
{
    const looper_ans_t *a, *b;
    looper_status_t st;
    ans_clear();
    looper_stop(L, 1, LOOPER_Q_INSTANT, 0);
    looper_launch(L, 9, LOOPER_Q_INSTANT, 0);
    run_blocks(4);
    looper_status(L, &st);
    CHECK(st.lane[1].play == 1);
    looper_load(L, 9, NULL, 0, 0);                      /* (emptied: its voice stops at once) */
    run_blocks(4);
    looper_status(L, &st);
    CHECK(st.lane[1].play == -1);
    looper_load(L, 9, "@/Loops/1-1 120bpm 1b.wav", 1, 1200);
    a = wait_ans(LOOPER_A_LOADED, 9);
    CHECK(a && a->status == LOOPER_OK && a->bars == 1);
    /* Warp Off: its own rate (the song's tempo does not move it) */
    looper_set(L, 9, LOOPER_P_WARP, LOOPER_W_OFF);
    looper_launch(L, 9, LOOPER_Q_INSTANT, 0);
    run_blocks(3);
    looper_status(L, &st);
    CHECK(st.lane[1].play == 1);
    {
        uint16_t p0, p1;
        looper_status(L, &st), p0 = st.lane[1].pos;
        bpm = 60.0;
        run_blocks(50);
        looper_status(L, &st), p1 = st.lane[1].pos;
        CHECKF(fabs((double)(uint16_t)(p1 - p0) - 50.0 * N * 65536.0 / 88200.0) < 200, "Warp Off moved %u",
               (unsigned)(uint16_t)(p1 - p0));
        bpm = 120.0;
    }
    looper_stop(L, 1, LOOPER_Q_INSTANT, 0);
    looper_set(L, 9, LOOPER_P_WARP, LOOPER_W_REPITCH);
    /* Open, then another in lane 4 while it runs: the first ends at the bar, the second starts on that line */
    ans_clear();
    looper_rec(L, 16, 0, LOOPER_SRC_TRACK1, 0, 0);     /* (lane 3, Open) */
    run_blocks((int)(bar_frame(8) / N) + 4);
    looper_status(L, &st);
    CHECK(st.rec[0].slot == 16 && st.rec[0].state == LOOPER_RS_RECORDING);
    looper_rec(L, 24, 1, LOOPER_SRC_TRACK1, 0, 0);     /* (lane 4, 1 bar) */
    blk();
    looper_status(L, &st);
    CHECK(st.rec[0].state == LOOPER_RS_ENDING && st.rec[1].slot == 24);
    a = wait_ans(LOOPER_A_RECORDED, 16);
    b = wait_ans(LOOPER_A_RECORDED, 24);
    CHECKF(a && a->status == LOOPER_OK && a->bars >= 2 && a->frames == (uint32_t)(a->bars * bar_frame(4)),
           "open: status %d bars %d frames %u", a ? a->status : -1, a ? a->bars : 0, a ? a->frames : 0);
    CHECKF(b && b->status == LOOPER_OK && b->bars == 1 && b->frames == (uint32_t)bar_frame(4), "next: %d %u",
           b ? b->status : -1, b ? b->frames : 0);
    if (a && b) {                                       /* (the second's first frame follows the first's last) */
        char p1[1024], p2[1024];
        uint32_t n1 = 0, n2 = 0;
        const char *why;
        int16_t *w1, *w2;
        looper_resolve(L, a->path, p1, sizeof p1), looper_resolve(L, b->path, p2, sizeof p2);
        w1 = wav_read(p1, &n1, &why), w2 = wav_read(p2, &n2, &why);
        CHECK(w1 && w2 && n1 && n2 && (uint16_t)(w2[0] - w1[2 * (n1 - 1)]) == 1);
        free(w1), free(w2);
    }
    /* a cancel: armed, nothing kept */
    ans_clear();
    looper_rec(L, 3, 0, LOOPER_SRC_TRACK1, 0, 0);
    blk();
    looper_rec_cancel(L, 3);
    run_blocks((int)(bar_frame(4) / N) + 4);
    looper_status(L, &st);
    CHECK(st.rec[0].slot == -1 && !wait_ans(LOOPER_A_RECORDED, 3));
    /* the transport stopped during one: its whole bars kept; armed while stopped, it starts with Play */
    ans_clear();
    looper_rec(L, 4, 0, LOOPER_SRC_TRACK1, 0, 0);
    run_blocks((int)(bar_frame(12) / N));
    playing = 0;
    blk();
    a = wait_ans(LOOPER_A_RECORDED, 4);
    CHECKF(a && a->status == LOOPER_OK && a->bars >= 1 && a->frames == (uint32_t)(a->bars * bar_frame(4)),
           "stopped: %d bars %d", a ? a->status : -1, a ? a->bars : 0);
    ans_clear();
    looper_rec(L, 5, 1, LOOPER_SRC_TRACK1, 0, 0);
    run_blocks(20);
    looper_status(L, &st);
    CHECK(st.rec[0].slot == 5 && st.rec[0].state == LOOPER_RS_ARMED);
    play_from_start();
    blk();
    looper_status(L, &st);
    CHECK(st.rec[0].slot == 5 && st.rec[0].state == LOOPER_RS_RECORDING);
    a = wait_ans(LOOPER_A_RECORDED, 5);
    CHECK(a && a->status == LOOPER_OK && a->bars == 1);
    if (a) {
        char p[1024];
        uint32_t n = 0, i, bad = 0;
        const char *why;
        int16_t *w;
        looper_resolve(L, a->path, p, sizeof p);
        w = wav_read(p, &n, &why);
        for (i = 0; w && i < n; i++)                    /* (from Play's first frame) */
            bad += w[2 * i] != pat(i);
        CHECK(w && !bad);
        free(w);
    }
}

/* ---- 5. the cap, the clean-up */
static void t_cap_clean(void)
{
    looper_cfg_t c;
    looper_t *k;
    const looper_ans_t *a;
    looper_t *was = L;
    char p[1024], q[1024];
    const char *refs[1];
    struct utimbuf old;
    struct stat st;
    memset(&c, 0, sizeof c);
    c.fs = FS, c.max_bytes = 100000, c.rec_s = 8;
    la_engine_set = 0;
    k = looper_new(&c);
    la_engine_set = 1;
    CHECK(k != NULL);
    L = k;
    looper_project(L, PROJ);
    ans_clear();
    play_from_start();
    looper_rec(L, 1, 1, LOOPER_SRC_TRACK1, 0, 0);
    a = wait_ans(LOOPER_A_RECORDED, 1);
    CHECK(a && a->status == LOOPER_TOO_LONG);
    /* the clean-up: a file nothing refers to (and old) goes, the referred one stays, a new one stays */
    looper_resolve(L, "@/Loops/1-1 120bpm 1b.wav", p, sizeof p);
    looper_resolve(L, "@/Loops/4-1 120bpm 1b.wav", q, sizeof q);
    old.actime = old.modtime = time(NULL) - 3600;
    CHECK(utime(p, &old) == 0 && utime(q, &old) == 0);
    refs[0] = "@/Loops/1-1 120bpm 1b.wav";
    looper_clean(L, refs, 1);
    {
        int t;
        for (t = 0; t < 2000 && !looper_idle(L); t++)
            usleep(1000);
    }
    CHECK(stat(p, &st) == 0 && stat(q, &st) != 0);
    la_engine_set = 0;
    looper_free(k);
    la_engine_set = 1;
    L = was;
}

/* ---- 6. (round 2) a Files row's sound (its copy in Loops/, its bars at the tempo), the markers and Steps, Warp's
 * engines (Repitch: the pitch follows; Grains, Beats: kept), the close-up's peaks, an overdub (its layer, its file,
 * Undo), the monitor, the input armed */
#define SINE_P 100                                      /* (441 Hz at 44.1 kHz) */
static int16_t sine_at(uint32_t i) { return (int16_t)lrint(4000.0 * sin(2.0 * 3.14159265358979 * (i % SINE_P) / SINE_P)); }
static int reads;
static int16_t *fake_read(void *ctx, int place, uint32_t id, uint32_t fs, uint64_t max, uint32_t *frames, char *name,
                          int ncap, int *status)
{
    const uint32_t n = id == 3 ? (uint32_t)(88200 * 3.7) : 88200;
    int16_t *p;
    uint32_t i;
    (void)ctx, (void)fs, (void)max;
    reads++;
    if (place != 2 || (id != 1 && id != 3)) {
        *status = LOOPER_MISSING;
        return NULL;
    }
    if (!(p = malloc((size_t)n * 4u))) {
        *status = LOOPER_FAILED;
        return NULL;
    }
    for (i = 0; i < n; i++)
        p[2 * i] = p[2 * i + 1] = sine_at(i);
    *frames = n, *status = LOOPER_OK;
    snprintf(name, (size_t)ncap, "%s", id == 3 ? "Long/Sine" : "Sine");
    return p;
}
static double crossings_hz(int blocks)                  /* the looper's own output's rising zero crossings a second */
{
    int b, i, n = 0, prev = 0;
    for (b = 0; b < blocks; b++) {
        blk();
        for (i = 0; i < N; i++) {
            const int v = own[2 * i], sg = v < -300 ? -1 : v > 300 ? 1 : 0;   /* (a hysteresis: no jitter counted) */
            if (sg && sg != prev) {
                n += prev < 0 && sg > 0;
                prev = sg;
            }
        }
    }
    return n / ((double)blocks * N / FS);
}
static void t_round2(void)
{
    const looper_ans_t *a;
    looper_status_t st;
    char p[1024];
    uint32_t n = 0, i;
    const char *why;
    int16_t *w;
    double hz;
    ans_clear();
    for (i = 0; i < LOOPER_LANES; i++)
        looper_stop(L, (int)i, LOOPER_Q_INSTANT, 0);
    play_from_start();
    run_blocks(4);
    /* item 1: a file into slot 20: its copy, 1 bar at 120; one 3.7 bars long: 4; one missing */
    looper_file(L, 20, 2, 1, 1200);
    a = wait_ans(LOOPER_A_RECORDED, 20);
    CHECKF(a && a->status == LOOPER_OK && a->bars == 1 && a->frames == 88200 && !strcmp(a->path, "@/Loops/Sine.wav"),
           "%d %d %u %s", a ? a->status : -1, a ? a->bars : 0, a ? a->frames : 0, a ? a->path : "");
    looper_resolve(L, "@/Loops/Sine.wav", p, sizeof p);
    w = wav_read(p, &n, &why);
    CHECK(w && n == 88200 && w[2 * 7] == sine_at(7));
    free(w);
    ans_clear();
    looper_file(L, 21, 2, 3, 1200);
    a = wait_ans(LOOPER_A_RECORDED, 21);
    CHECKF(a && a->status == LOOPER_OK && a->bars == 4 && !strcmp(a->path, "@/Loops/Long_Sine.wav"), "%d %d %s",
           a ? a->status : -1, a ? a->bars : 0, a ? a->path : "");
    ans_clear();
    looper_file(L, 22, 2, 9, 1200);
    a = wait_ans(LOOPER_A_RECORDED, 22);
    CHECK(a && a->status == LOOPER_MISSING);
    /* items 7, 8: Repitch: 441 Hz at its own length; Steps doubled: an octave down; Grains / Beats: the pitch kept */
    looper_launch(L, 20, LOOPER_Q_INSTANT, 0);
    run_blocks(8);
    hz = crossings_hz(200);
    CHECKF(fabs(hz - 441) < 15, "repitch %.1f Hz", hz);
    looper_set(L, 20, LOOPER_P_LENGTH, 32);
    run_blocks(4);
    hz = crossings_hz(200);
    CHECKF(fabs(hz - 220.5) < 12, "repitch, 2 bars: %.1f Hz", hz);
    looper_set(L, 20, LOOPER_P_WARP, LOOPER_W_GRAINS);
    run_blocks(4);
    hz = crossings_hz(300);
    CHECKF(fabs(hz - 441) < 30, "grains, 2 bars: %.1f Hz", hz);
    looper_status(L, &st);
    {                                                   /* (its place still the song's: half a cycle a bar) */
        const double q = fmod(gf * bpf(), 8.0) / 8.0;
        CHECKF(fabs(st.lane[2].pos / 65535.0 - q) < 0.03 || fabs(st.lane[2].pos / 65535.0 - q) > 0.97, "pos %u q %.3f",
               st.lane[2].pos, q);
    }
    looper_set(L, 20, LOOPER_P_WARP, LOOPER_W_BEATS);
    run_blocks(4);
    hz = crossings_hz(300);
    CHECKF(fabs(hz - 441) < 30, "beats, 2 bars: %.1f Hz", hz);
    /* the markers: its first half over a bar (Repitch): an octave down */
    looper_set(L, 20, LOOPER_P_WARP, LOOPER_W_REPITCH);
    looper_set(L, 20, LOOPER_P_LENGTH, 16);
    looper_set(L, 20, LOOPER_P_MEND, LOOPER_MK_ONE / 2);
    run_blocks(4);
    hz = crossings_hz(200);
    CHECKF(fabs(hz - 220.5) < 12, "first half over a bar: %.1f Hz", hz);
    looper_set(L, 20, LOOPER_P_MEND, LOOPER_MK_ONE);
    /* the close-up's peaks */
    ans_clear();
    looper_peaks(L, 20, 1000, 2000, 10);
    a = wait_ans(LOOPER_A_PEAKS, 20);
    CHECK(a && a->from == 1000 && a->count == 2000 && a->npk == 10 && a->pk[3] >= 30 && a->pk[3] <= 32);
    /* item 9: an overdub: Track 1's steady 8000 layered from its next cycle; ended: a new file of the loop + its layer,
     * the loop's sound swapped in seamlessly; Undo: the loop before it, from memory */
    ans_clear();
    src_const = 1;
    looper_dub(L, 20, 1, LOOPER_SRC_TRACK1, 0);
    run_blocks((int)(bar_frame(4) * 3 / 2 / N) + 2);   /* (a cycle to wait at most, then half of one) */
    looper_dub(L, 20, 0, 0, 0);
    src_const = 0;
    a = wait_ans(LOOPER_A_RECORDED, 20);
    CHECKF(a && a->status == LOOPER_OK && strcmp(a->path, "@/Loops/Sine.wav") && a->frames == 88200, "%d %s",
           a ? a->status : -1, a ? a->path : "");
    if (a) {
        uint32_t lay = 0, none = 0;
        looper_resolve(L, a->path, p, sizeof p);
        w = wav_read(p, &n, &why);
        for (i = 0; w && i < n; i++) {
            const int d = w[2 * i] - sine_at(i);
            lay += d == 8000 || d == 16000;
            none += d == 0;
        }
        CHECKF(w && lay > 0 && lay + none == n, "layered %u, untouched %u of %u", lay, none, n);
        free(w);
    }
    run_blocks(20);
    looper_status(L, &st);
    CHECK(st.lane[2].play == 4);                        /* (it played on) */
    ans_clear();
    reads = 0;
    looper_load(L, 20, "@/Loops/Sine.wav", 1, 1200);    /* (Undo) */
    a = wait_ans(LOOPER_A_LOADED, 20);
    CHECK(a && a->status == LOOPER_OK && !reads);
    /* item 6: the monitor: heard in the mix, never in the looper's own block */
    {
        static int16_t m[2 * N];
        double sa = 0;
        int k;
        for (i = 0; i < LOOPER_LANES; i++)
            looper_stop(L, (int)i, LOOPER_Q_INSTANT, 0);
        run_blocks(20);
        for (k = 0; k < 2 * N; k++)
            m[k] = 1000;
        mon_buf = m;
        blk();
        mon_buf = NULL;
        {
            int nz = 0;
            for (k = 0; k < 2 * N; k++)
                sa += acc[k], nz += own[k] != 0;
            CHECK(nz == 0);
        }
        CHECKF(fabs(sa / (2 * N) - 1000) < 1, "monitor %.1f", sa / (2 * N));
        blk();
        CHECK(acc[0] == 0);                             /* (a block's only) */
    }
    /* (item 7) the cost: four lanes in Beats (the dearest: its WSOLA search each half grain), a block's time */
    {
        static const int to[3] = { 4, 12, 28 };
        int k;
        clock_t t0;
        double us;
        for (k = 0; k < 3; k++)
            looper_copy(L, to[k], 20), looper_set(L, to[k], LOOPER_P_WARP, LOOPER_W_BEATS), looper_launch(L, to[k], LOOPER_Q_INSTANT, 0);
        looper_set(L, 20, LOOPER_P_WARP, LOOPER_W_BEATS), looper_set(L, 20, LOOPER_P_LENGTH, 32);
        looper_launch(L, 20, LOOPER_Q_INSTANT, 0);
        run_blocks(50);
        CHECK(looper_sounding(L) == 4);
        t0 = clock();
        run_blocks(2000);
        us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 2000.0;
        printf("  cost: 4 lanes in Beats, %.1f us a block of %d frames (%.1f %% of its %.0f us)\n", us, N,
               us * 100.0 / (N * 1e6 / FS), N * 1e6 / FS);
        for (k = 0; k < 3; k++)
            looper_load(L, to[k], NULL, 0, 0);
        looper_set(L, 20, LOOPER_P_WARP, LOOPER_W_REPITCH), looper_set(L, 20, LOOPER_P_LENGTH, 16);
    }
    /* Monitor Auto's cue: an input recording armed */
    looper_rec(L, 7, 0, LOOPER_SRC_INPUT, 0, 0);
    CHECK(looper_input_armed(L));
    looper_rec_cancel(L, 7);
    CHECK(!looper_input_armed(L));
}

int main(void)
{
    looper_cfg_t c;
    const char *d = getenv("LOOPER_TEST_DIR");
    snprintf(PROJ, sizeof PROJ, "%s/proj-%d", d && d[0] ? d : "/tmp", (int)getpid());
    mkdir(PROJ, 0755);
    memset(&c, 0, sizeof c);
    c.fs = FS;
    c.read = fake_read;                                 /* (round 2: looper_file's rows) */
    L = looper_new(&c);
    if (!L) {
        fprintf(stderr, "looper_new failed\n");
        return 1;
    }
    looper_project(L, PROJ);
    la_engine = pthread_self(), la_engine_set = 1;     /* (from here every looper call is the engine thread's) */
    t_record();
    t_launch();
    t_mix();
    t_more();
    t_round2();
    CHECKF(la_engine_allocs == 0, "%ld allocations on the engine's thread", la_engine_allocs);
    t_cap_clean();
    CHECKF(la_engine_allocs == 0, "%ld allocations on the engine's thread", la_engine_allocs);
    la_engine_set = 0;
    looper_free(L);
    printf("%d checks\n", checks);
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL LOOPER TESTS PASSED\n");
    return 0;
}
