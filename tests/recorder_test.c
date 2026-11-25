/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder (src/recorder.c), natively: every mode, stems, the files' frames sample-exact against what was fed,
 * names, errors, a slow writer and an overrun, the status. Each stream's signal is its own counter pattern (the frame
 * number in it), so a file's samples tell exactly which frames it holds.
 *   tests/recorder_test.sh      (builds it plain, with ASan + UBSan, with TSan, and runs each) */
#define _GNU_SOURCE
#include "../src/recorder.h"
#include "../src/wav.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FS 44100u
#define BLK 128u

static int checks, fails;
static char ROOT[512];
static time_t T0;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    checks++;
    fails += !ok;
}

static time_t fixed_now(void) { return T0; }

/* ------------------------------------------------------------------ the signals --- */
/* a stream's level over frames: 0 silent, 1 loud (the counter pattern), 2 quiet (under -54 dBFS), >= 1000: +/- that */
typedef struct {
    uint64_t a, b;
    int lvl;
} seg_t;
static struct {
    int def[REC_NST];
    seg_t seg[REC_NST][8];
    int nseg[REC_NST];
} SC;

static void scene(int def_all)
{
    int s;
    memset(&SC, 0, sizeof SC);
    for (s = 0; s < REC_NST; s++)
        SC.def[s] = def_all;
}

static void seg(int s, uint64_t a, uint64_t b, int lvl)
{
    SC.seg[s][SC.nseg[s]++] = (seg_t){ a, b, lvl };
}

static int level(int s, uint64_t i)
{
    int k;
    for (k = SC.nseg[s]; k-- > 0;)
        if (i >= SC.seg[s][k].a && i < SC.seg[s][k].b)
            return SC.seg[s][k].lvl;
    return SC.def[s];
}

static void sample(int s, uint64_t i, int16_t *lr)
{
    const int l = level(s, i);
    if (l == 0) {
        lr[0] = lr[1] = 0;
    } else if (l == 1) {
        lr[0] = (int16_t)(100 + (int)((i + (uint64_t)s * 4567u) % 32000u));
        lr[1] = (int16_t)-(100 + (int)((i / 32000u + (uint64_t)s * 101u) % 32000u));
    } else if (l == 2) {
        lr[0] = (int16_t)(1 + (int)((i * 7u + (uint64_t)s) % 40u));
        lr[1] = (int16_t)-(1 + (int)((i * 3u + (uint64_t)s) % 40u));
    } else {
        lr[0] = (int16_t)l;
        lr[1] = (int16_t)-l;
    }
}

/* ------------------------------------------------------------------ the engine, simulated --- */
typedef struct {
    rec_t *r;
    uint64_t i;          /* frames fed */
    int playing;
    double beat, bpm;
    unsigned present;
    double max_us;       /* rec_block's longest */
} sim_t;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void block(sim_t *S)
{
    static int16_t buf[REC_NST][BLK * 2];
    rec_block_t b;
    uint32_t j;
    int s;
    double t;
    memset(&b, 0, sizeof b);
    for (s = 0; s < REC_NST; s++)
        if (S->present >> s & 1) {
            for (j = 0; j < BLK; j++)
                sample(s, S->i + j, buf[s] + 2 * j);
            b.st[s] = buf[s];
        }
    b.frames = BLK;
    b.playing = S->playing;
    b.beat = S->playing ? S->beat : 0.0;
    b.bpm = S->bpm;
    t = now_us();
    rec_block(S->r, &b);
    t = now_us() - t;
    if (t > S->max_us)
        S->max_us = t;
    S->i += BLK;
    if (S->playing)
        S->beat += (double)BLK / FS * S->bpm / 60.0;
}

static void run_to(sim_t *S, uint64_t f)   /* whole blocks until the next one starts at or after f */
{
    while (S->i < f)
        block(S);
}

static void play(sim_t *S)   /* the transport starts with the next block, at beat 0 */
{
    S->playing = 1;
    S->beat = 0.0;
}

static void press(sim_t *S) { rec_press(S->r); }

/* bar line k after the transport started at frame p: the first frame at or after it */
static uint64_t barline(uint64_t p, double bpm, int k)
{
    const uint64_t b10 = (uint64_t)llround(bpm * 10.0);
    return p + ((uint64_t)k * 105840000u + b10 - 1) / b10;
}

/* ------------------------------------------------------------------ the recorder's side --- */
static rec_t *mk(const char *name, rec_cfg_t *c0)
{
    static char dir[700];
    rec_cfg_t c;
    if (c0)
        c = *c0;
    else
        memset(&c, 0, sizeof c);
    snprintf(dir, sizeof dir, "%s/%s", ROOT, name);
    if (!c.dir)
        c.dir = dir;
    c.now = fixed_now;
    return rec_new(&c);
}

static int takes(rec_t *r, rec_take_t *t, int want, int ms)
{
    int n = 0;
    double end = now_us() + ms * 1000.0;
    while (n < want && now_us() < end) {
        if (rec_poll_take(r, &t[n]))
            n++;
        else
            usleep(2000);
    }
    return n;
}

static int no_more(rec_t *r)
{
    rec_take_t t;
    usleep(40000);
    return !rec_poll_take(r, &t);
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the file: a 44-byte PCM 16 stereo header at 44.1 kHz, `frames` frames, stream s's frames from `first` on */
static int verify(const char *path, int s, uint64_t first, uint64_t frames, char *why, size_t wn)
{
    uint8_t h[44];
    FILE *f = fopen(path, "rb");
    long size;
    uint32_t n, j;
    const char *w;
    int16_t *pcm;
    if (!f || fread(h, 1, 44, f) != 44) {
        snprintf(why, wn, "cannot read %s", path);
        if (f)
            fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) || rd32(h + 16) != 16 || rd32(h + 20) != (1u | 2u << 16) ||
        rd32(h + 24) != FS || rd32(h + 28) != FS * 4 || rd32(h + 32) != (4u | 16u << 16) || memcmp(h + 36, "data", 4) ||
        rd32(h + 40) != frames * 4 || rd32(h + 4) != 36 + frames * 4 || size != (long)(44 + frames * 4)) {
        snprintf(why, wn, "header: data %u bytes, file %ld bytes, want %llu frames", rd32(h + 40), size,
                 (unsigned long long)frames);
        return 0;
    }
    if (!(pcm = wav_read(path, &n, &w)) || n != frames) {
        snprintf(why, wn, "wav_read: %s", pcm ? "frames" : w);
        free(pcm);
        return 0;
    }
    for (j = 0; j < n; j++) {
        int16_t e[2];
        sample(s, first + j, e);
        if (pcm[2 * j] != e[0] || pcm[2 * j + 1] != e[1]) {
            snprintf(why, wn, "frame %u (fed %llu): %d,%d want %d,%d", j, (unsigned long long)(first + j), pcm[2 * j],
                     pcm[2 * j + 1], e[0], e[1]);
            free(pcm);
            return 0;
        }
    }
    free(pcm);
    snprintf(why, wn, "%s", base(path));
    return 1;
}

static void expect_take(const rec_take_t *t, int s, uint64_t first, uint64_t frames, uint32_t bars, const char *what)
{
    char why[512];
    int ok = t->stream == s && t->frames == frames && t->bars == bars && *t->path;
    if (!ok) {
        check(0, "%s: stream %d frames %llu bars %u reason %d path '%s'; want stream %d frames %llu bars %u", what,
              t->stream, (unsigned long long)t->frames, t->bars, t->reason, t->path, s, (unsigned long long)frames,
              bars);
        return;
    }
    ok = verify(t->path, s, first, frames, why, sizeof why);
    check(ok, "%s: frames %llu..%llu exact (%s)", what, (unsigned long long)first,
          (unsigned long long)(first + frames - 1), why);
}

static int count_files(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    const size_t sl = strlen(suffix);
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        const size_t l = strlen(e->d_name);
        if (l > sl && !strcmp(e->d_name + l - sl, suffix))
            n++;
    }
    closedir(d);
    return n;
}

static int state(rec_t *r)
{
    rec_status_t s;
    rec_get(r, &s);
    return s.state;
}

/* ------------------------------------------------------------------ the tests --- */
static void t_bars(double bpm, int n)
{
    char name[64], what[96];
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t pf, st;
    int k;
    snprintf(name, sizeof name, "bars-%g-%d", bpm, n);
    S.r = mk(name, NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT1;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, n);
    play(&S);
    run_to(&S, barline(0, bpm, 1) + 30000);   /* (in bar 2) */
    pf = S.i;
    press(&S);
    block(&S);
    check(state(S.r) == REC_ARMED, "Bars %g BPM %d: armed after the press", bpm, n);
    for (k = 0; barline(0, bpm, k) < pf; k++)
        ;
    st = barline(0, bpm, k);
    run_to(&S, barline(0, bpm, k + n) + 20000);
    snprintf(what, sizeof what, "Bars %g BPM, %d bar%s (bar line %llu inside block %llu)", bpm, n, n > 1 ? "s" : "",
             (unsigned long long)st, (unsigned long long)(st / BLK));
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, st, barline(0, bpm, k + n) - st, (uint32_t)n, what);
        {
            char want[128], b[16];
            snprintf(b, sizeof b, "%g", bpm);
            snprintf(want, sizeof want, "1124-153000 Master %sbpm %db.wav", b, n);
            check(!strcmp(base(t.path), want), "Bars: named '%s' (want '%s')", base(t.path), want);
        }
    } else
        check(0, "%s: no take", what);
    check(no_more(S.r), "Bars: one take only (the source's, not slot 1's)");
    rec_free(S.r);
}

static void t_bars_open(void)
{
    const double bpm = 97.3;
    sim_t S = { 0 };
    rec_take_t t;
    S.r = mk("bars-open", NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 0);
    play(&S);
    run_to(&S, barline(0, bpm, 0) + 50000);
    press(&S);
    run_to(&S, barline(0, bpm, 3) + 40000);
    check(state(S.r) == REC_RECORDING, "Bars Open: recording past the first bars");
    press(&S);   /* (ends at the next bar line: bar 4) */
    run_to(&S, barline(0, bpm, 5));
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, barline(0, bpm, 1), barline(0, bpm, 4) - barline(0, bpm, 1), 3,
                    "Bars Open: a second press ends it at the next bar line, 3 bars");
    else
        check(0, "Bars Open: no take");
    /* a press while armed disarms */
    press(&S);
    block(&S);
    check(state(S.r) == REC_ARMED, "Bars: armed again");
    press(&S);
    block(&S);
    check(state(S.r) == REC_IDLE, "Bars: a press while armed disarms");
    run_to(&S, S.i + 3 * 105840000u / 973u);
    check(no_more(S.r), "Bars: no take after the disarm");
    rec_free(S.r);
}

static void t_bars_stopped_arm(void)
{
    const double bpm = 120.0;
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t p;
    S.r = mk("bars-arm-stopped", NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 2);
    run_to(&S, 50 * BLK);
    press(&S);
    run_to(&S, 80 * BLK);
    check(state(S.r) == REC_ARMED, "Bars, stopped: armed, waiting for Play");
    p = S.i;
    play(&S);
    run_to(&S, p + 3 * 88200);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, p, 2 * 88200, 2, "Bars, armed while stopped: starts with the first playing frame");
        check(t.first == p, "Bars, armed while stopped: rec_take_t.first the transport's first frame (%llu, want %llu)",
              (unsigned long long)t.first, (unsigned long long)p);
    } else
        check(0, "Bars, armed while stopped: no take");
    rec_free(S.r);
}

static void t_bars_transport_stop(void)
{
    const double bpm = 120.0;
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t stop;
    S.r = mk("bars-stop", NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 8);
    play(&S);
    run_to(&S, 1000);
    press(&S);
    run_to(&S, barline(0, bpm, 2) + 44100);   /* (1.5 bars in) */
    stop = S.i;
    S.playing = 0;
    run_to(&S, stop + 20000);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, 88200, stop - 88200, 0, "Bars: the transport's stop ends the take at that frame");
        check(t.reason == REC_END_TRANSPORT && strstr(base(t.path), "s.wav") && !strstr(base(t.path), "bpm"),
              "Bars stopped: reason transport, named in seconds ('%s')", base(t.path));
    } else
        check(0, "Bars stop: no take");
    rec_free(S.r);
}

static void t_capture_auto(void)
{
    const double bpm = 97.3;
    sim_t S = { 0 };
    rec_take_t t;
    rec_cfg_t c = { 0 };
    S.r = mk("capture-auto", NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT2;
    /* silence, then 5 bars of sound (from 300 frames after bar line 4 to past bar line 9) */
    scene(0);
    seg(REC_ST_MASTER, barline(0, bpm, 4) + 300, barline(0, bpm, 9) + 10000, 1);
    seg(REC_ST_SLOT2, 0, 1u << 30, 1);   /* (another stream sounding all along: not the source) */
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    rec_set_length_bars(S.r, 0);
    play(&S);
    run_to(&S, barline(0, bpm, 9) + 20000);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, barline(0, bpm, 4), barline(0, bpm, 9) - barline(0, bpm, 4), 5,
                    "Capture Auto: silence then 5 bars: whole bars, ends on the last bar line");
        check(strstr(base(t.path), " 97.3bpm 5b.wav") != NULL, "Capture Auto: named '%s'", base(t.path));
    } else
        check(0, "Capture Auto: no take");
    rec_free(S.r);

    /* a gap of 1 bar does not count as the silence; 3 bars before do */
    memset(&S, 0, sizeof S);
    S.r = mk("capture-auto-gap", NULL);
    S.bpm = bpm;
    S.present = 1u << REC_ST_MASTER;
    scene(0);
    seg(REC_ST_MASTER, barline(0, bpm, 3), barline(0, bpm, 5), 1);
    seg(REC_ST_MASTER, barline(0, bpm, 6), barline(0, bpm, 9) + 10000, 1);
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    play(&S);
    run_to(&S, barline(0, bpm, 9) + 20000);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, barline(0, bpm, 3), barline(0, bpm, 9) - barline(0, bpm, 3), 6,
                    "Capture Auto: a 1-bar gap is not the silence (6 bars)");
    else
        check(0, "Capture Auto gap: no take");
    rec_free(S.r);

    /* sound all along: 16 bars at most */
    memset(&S, 0, sizeof S);
    S.r = mk("capture-auto-16", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    play(&S);
    run_to(&S, barline(0, 120.0, 20) + 1000);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, barline(0, 120.0, 4), 16 * 88200, 16, "Capture Auto: sound all along: 16 bars");
    else
        check(0, "Capture Auto 16: no take");
    rec_free(S.r);
    (void)c;
}

static void t_capture_n(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    S.r = mk("capture-n", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    rec_set_length_bars(S.r, 2);
    play(&S);
    run_to(&S, barline(0, 120.0, 5) + 5000);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, 3 * 88200, 2 * 88200, 2, "Capture N=2: the last 2 whole bars");
    else
        check(0, "Capture N: no take");
    rec_free(S.r);
}

static void t_capture_stopped(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t p;
    S.r = mk("capture-stopped", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(0);
    seg(REC_ST_MASTER, 3 * FS + 77, 4 * FS, 1);       /* (a 1 s gap is not the silence) */
    seg(REC_ST_MASTER, 5 * FS, 6 * FS + 33, 1);
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    run_to(&S, 6 * FS + FS / 2);
    p = S.i;
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, 3 * FS + 77, p - (3 * FS + 77), 0,
                    "Capture stopped: back to the last 2 s silence, ending now");
        check(strstr(base(t.path), "bpm") == NULL, "Capture stopped: named in seconds ('%s')", base(t.path));
    } else
        check(0, "Capture stopped: no take");
    rec_free(S.r);
}

static void t_capture_silence(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    char dir[700];
    S.r = mk("capture-silence", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_INPUT;
    scene(2);   /* (quiet: under -54 dBFS) */
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    run_to(&S, 5 * FS);
    press(&S);
    block(&S);
    play(&S);
    run_to(&S, S.i + 5 * 88200);
    rec_set_source(S.r, REC_SRC_STEMS);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        check(t.reason == REC_END_NOTHING && !*t.path, "Capture of only silence, stopped: 'nothing to capture'");
    else
        check(0, "Capture silence: no result");
    if (takes(S.r, &t, 1, 5000) == 1)
        check(t.reason == REC_END_NOTHING && !*t.path && t.stream == REC_SRC_STEMS,
              "Capture of only silence, playing, stems: 'nothing to capture'");
    else
        check(0, "Capture silence stems: no result");
    snprintf(dir, sizeof dir, "%s/capture-silence", ROOT);
    check(count_files(dir, ".wav") == 0 && count_files(dir, ".part") == 0, "Capture of silence: no file");
    rec_free(S.r);
}

static void t_threshold(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    const uint64_t T = 100 * BLK + 50;
    S.r = mk("threshold", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(2);
    seg(REC_ST_MASTER, T, 1u << 30, 1);
    rec_set_mode(S.r, REC_MODE_THRESHOLD);
    rec_set_threshold(S.r, 1000);
    run_to(&S, 10 * BLK);
    press(&S);
    run_to(&S, 60 * BLK);
    check(state(S.r) == REC_ARMED, "Threshold: armed, quiet under the threshold");
    run_to(&S, 300 * BLK);
    check(state(S.r) == REC_RECORDING, "Threshold: recording once the peak reached it");
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, 100 * BLK - FS / 20, 300 * BLK - (100 * BLK - FS / 20), 0,
                    "Threshold: 50 ms before the block (quiet pre-roll, then the attack) to the press");
    else
        check(0, "Threshold: no take");

    /* Length 1 at 120: one bar's duration */
    rec_set_length_bars(S.r, 1);
    rec_set_threshold(S.r, 100);   /* (the pattern at frame 128007 is +107: just over it) */
    scene(2);
    seg(REC_ST_MASTER, 1000 * BLK + 7, 1u << 30, 1);
    block(&S);
    press(&S);
    play(&S);
    run_to(&S, 1000 * BLK + 2 * 88200);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, 1000 * BLK - FS / 20, 88200, 1, "Threshold, Length 1 at 120 BPM: 88200 frames");
    else
        check(0, "Threshold N: no take");
    scene(2);
    press(&S);
    block(&S);
    check(state(S.r) == REC_ARMED, "Threshold: armed again");
    press(&S);
    block(&S);
    check(state(S.r) == REC_IDLE, "Threshold: a press while armed disarms");
    check(no_more(S.r), "Threshold: no take after the disarm");
    rec_free(S.r);
}

static void t_manual(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    S.r = mk("manual", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT2;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    rec_set_source(S.r, REC_ST_SLOT2);
    run_to(&S, 20 * BLK);
    press(&S);
    run_to(&S, 120 * BLK);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_SLOT2, 20 * BLK, 100 * BLK, 0, "Manual: from the press's block to the press's block");
        check(strstr(base(t.path), " Track 2 ") != NULL, "Manual: slot 2 named 'Track 2' (no label: nothing more; '%s')",
              base(t.path));
    } else
        check(0, "Manual: no take");
    rec_free(S.r);
}

/* a stems take's files: each the right name in the folder want (under ROOT/stems/), sample-exact, one take */
static void stems_files(const rec_take_t *t, int n, const char *want, uint64_t first, uint64_t frames, const char *what)
{
    static const char *const NAME[REC_NST] = { "Master.wav", "Track 1 ChoralRoot.wav", "Track 2.wav",
                                              "Track 3 Felucca.wav", "Input.wav", "Looper.wav", "Kit.wav" };
    char dir[900], w[160];
    int k, ok = 1, names = 1, seen = 0;
    snprintf(dir, sizeof dir, "%s/stems/%s/", ROOT, want);
    for (k = 0; k < n; k++) {
        snprintf(w, sizeof w, "%s: stream %d", what, t[k].stream);
        expect_take(&t[k], t[k].stream, first, frames, 0, w);
        ok &= t[k].frames == t[0].frames && t[k].take == t[0].take;
        names &= t[k].stream >= 0 && !strncmp(t[k].path, dir, strlen(dir)) && !strcmp(t[k].path + strlen(dir), NAME[t[k].stream]);
        if (t[k].stream >= 0)
            seen |= 1 << t[k].stream;
    }
    check(ok && seen == 0x1F, "%s: equal lengths, one take, a file per present stream (the absent: none)", what);
    check(names, "%s: in their own folder '%s', named %s%s%s%s%s", what, want, base(t[0].path), n > 1 ? ", " : "",
          n > 1 ? base(t[1].path) : "", n > 2 ? ", ..." : "", n > 4 ? base(t[4].path) : "");
}

static void t_stems(void)
{
    sim_t S = { 0 };
    rec_take_t t[8];
    char dir[700], part[900];
    int n;
    const unsigned pres = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT1 | 1u << REC_ST_SLOT2 | 1u << REC_ST_SLOT3 |
                          1u << REC_ST_INPUT;
    uint64_t p;
    S.r = mk("stems", NULL);
    S.bpm = 120.0;
    S.present = pres;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    rec_set_source(S.r, REC_SRC_STEMS);
    rec_set_label(S.r, REC_ST_SLOT1, "ChoralRoot");
    rec_set_label(S.r, REC_ST_SLOT3, "Felucca");
    rec_set_label(S.r, REC_ST_INPUT, "Mic?");   /* (Master, Input: their own names whatever the label) */
    run_to(&S, 33 * BLK);
    press(&S);
    run_to(&S, 1033 * BLK);
    press(&S);
    block(&S);
    n = takes(S.r, t, 5, 5000);
    check(n == 5, "Stems: 5 files for 5 present streams (%d)", n);
    stems_files(t, n, "1124-153000 Stems 2.9s", 33 * BLK, 1000 * BLK, "Stems");
    check(no_more(S.r), "Stems: no more files");

    /* a second stems take the same second, with a slow writer: the folder under a temporary name while it records,
     * its files readable; then ' (2)' */
    rec_test_write_delay(S.r, 20000);
    snprintf(dir, sizeof dir, "%s/stems", ROOT);
    snprintf(part, sizeof part, "%s/1124-153000 Stems.part/Master.wav", dir);
    p = S.i;
    press(&S);
    run_to(&S, p + 1000 * BLK);
    press(&S);
    block(&S);
    usleep(400000);
    {
        uint8_t h[44];
        FILE *f = fopen(part, "rb");
        const int ok = f && fread(h, 1, 44, f) == 44 && rd32(h + 40) > 0 && rd32(h + 4) == 36 + rd32(h + 40);
        if (f)
            fclose(f);
        check(ok, "Stems recording: in '1124-153000 Stems.part/', Master.wav readable (header kept up to date)");
    }
    n = takes(S.r, t, 5, 10000);
    rec_test_write_delay(S.r, 0);
    check(n == 5, "Stems again the same second: 5 files (%d)", n);
    stems_files(t, n, "1124-153000 Stems 2.9s (2)", p, 1000 * BLK, "Stems the same second");
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        int dirs = 0, other = 0;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.')
                (strstr(e->d_name, ".part") || strncmp(e->d_name, "1124-153000 Stems 2.9s", 22)) ? other++ : dirs++;
        if (d)
            closedir(d);
        check(dirs == 2 && !other, "Stems: two folders, nothing else (no .part left)");
    }
    rec_free(S.r);
}

static void t_max(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    rec_cfg_t c = { 0 };
    c.max_take_s = 2;
    S.r = mk("max", &c);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    run_to(&S, 7 * BLK);
    press(&S);
    run_to(&S, 5 * FS);
    check(state(S.r) == REC_IDLE || state(S.r) == REC_SAVING, "Max length: the take stopped by itself");
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, 7 * BLK, 2 * FS, 0, "Max length 2 s: exactly 2 s");
        check(t.reason == REC_END_MAX_LENGTH, "Max length: reason max length (%d)", t.reason);
    } else
        check(0, "Max: no take");
    rec_free(S.r);
}

static void t_errors(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    rec_cfg_t c = { 0 };
    char blocker[700], dir[800];
    FILE *f;
    int k;
    snprintf(blocker, sizeof blocker, "%s/blocker", ROOT);
    f = fopen(blocker, "w");
    if (f)
        fclose(f);
    snprintf(dir, sizeof dir, "%s/takes", blocker);   /* (under a file: no folder can be made) */
    c.dir = dir;
    S.r = mk("x", &c);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    press(&S);
    run_to(&S, 2 * FS);
    if (takes(S.r, &t, 1, 5000) == 1)
        check(t.reason == REC_END_WRITE_ERROR && !*t.path, "A failing folder: a write error reported (%d)", t.reason);
    else
        check(0, "A failing folder: no result");
    run_to(&S, S.i + 10 * BLK);
    check(state(S.r) == REC_IDLE, "A failing folder: the engine side back to idle, still running");
    press(&S);
    run_to(&S, S.i + FS);
    press(&S);
    block(&S);
    k = takes(S.r, &t, 1, 5000);
    check(k == 1 && t.reason == REC_END_WRITE_ERROR, "A failing folder: the next take reports it too");
    {   /* (the second press above started a take of its own: the writer stopped the first at once) */
        rec_take_t tt[4];
        takes(S.r, tt, 4, 300);
    }
    S.present |= 1u << REC_ST_SLOT1;
    rec_set_source(S.r, REC_SRC_STEMS);
    block(&S);
    press(&S);
    run_to(&S, S.i + 10 * BLK);
    k = takes(S.r, &t, 1, 5000);
    check(k == 1 && t.reason == REC_END_WRITE_ERROR && !*t.path && t.stream == REC_SRC_STEMS && no_more(S.r),
          "A failing folder, stems: one write error for the take, no file (%d results, reason %d, stream %d)", k,
          t.reason, t.stream);
    check(S.max_us < 20000.0, "A failing folder: rec_block never waited (longest %.0f us)", S.max_us);
    rec_free(S.r);
    unlink(blocker);

    memset(&S, 0, sizeof S);
    memset(&c, 0, sizeof c);
    c.min_free = UINT64_MAX / 4;   /* (no volume has that much free) */
    S.r = mk("disk-low", &c);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    press(&S);
    run_to(&S, FS);
    if (takes(S.r, &t, 1, 5000) == 1)
        check(t.reason == REC_END_DISK_LOW && !*t.path, "Disk low: reported, no file (%d)", t.reason);
    else
        check(0, "Disk low: no result");
    run_to(&S, S.i + 10 * BLK);
    check(state(S.r) == REC_IDLE, "Disk low: the engine side back to idle");
    rec_free(S.r);
}

static void t_settings_mid_take(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    rec_status_t st;
    uint64_t p;
    S.r = mk("settings", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 1);
    play(&S);
    block(&S);
    press(&S);
    run_to(&S, 88200 + 30000);
    check(state(S.r) == REC_RECORDING, "Settings: a 1-bar take running");
    rec_set_length_bars(S.r, 4);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    rec_set_label(S.r, REC_ST_MASTER, "Choral/Root: \xc3\xa9*");
    rec_set_source(S.r, REC_ST_INPUT);
    block(&S);
    rec_get(S.r, &st);
    check(st.mode == REC_MODE_BARS && st.length_bars == 1 && st.source == REC_ST_MASTER,
          "Settings: changed mid-take, the take keeps its own");
    run_to(&S, 2 * 88200 + 1000);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_MASTER, 88200, 88200, 1, "Settings: the running take as it began (1 bar, Master)");
        check(strstr(base(t.path), " Master ") != NULL, "Settings: the label as it was ('%s')", base(t.path));
    } else
        check(0, "Settings: no take");
    block(&S);
    rec_get(S.r, &st);
    check(st.mode == REC_MODE_MANUAL && st.length_bars == 4 && st.source == REC_ST_INPUT, "Settings: in force after it");
    S.present |= 1u << REC_ST_INPUT;
    p = S.i;
    press(&S);
    run_to(&S, p + 100 * BLK);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1) {
        expect_take(&t, REC_ST_INPUT, p, 100 * BLK, 0, "Settings: the next take is Manual, of the input");
        check(strstr(base(t.path), " Input ") != NULL, "Settings: the input's own label ('%s')", base(t.path));
    } else
        check(0, "Settings: no second take");
    rec_set_source(S.r, REC_ST_MASTER);
    block(&S);
    p = S.i;
    press(&S);
    run_to(&S, p + 10 * BLK);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        check(strstr(base(t.path), "1124-153000 Choral_Root_ _ ") == base(t.path),
              "Names: the label sanitized ('%s')", base(t.path));
    else
        check(0, "Settings: no third take");
    rec_free(S.r);
}

static void t_names(void)
{
    sim_t S = { 0 };
    rec_take_t t[2];
    rec_cfg_t c = { 0 };
    int k;
    c.day_folders = 1;
    S.r = mk("names", &c);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    for (k = 0; k < 2; k++) {
        press(&S);
        run_to(&S, S.i + 3 * FS / 2);   /* (1.5 s: 66150 frames, whole blocks: 66176) */
        press(&S);
        block(&S);
    }
    if (takes(S.r, t, 2, 5000) == 2) {
        check(strstr(t[0].path, "/names/2026-11-24/1124-153000 Master 1.5s.wav") != NULL,
              "Names: a folder a day, '<MMDD>-<HHMMSS> <label> <seconds>s.wav' (%s)", t[0].path + strlen(ROOT));
        check(strstr(t[1].path, "/1124-153000 Master 1.5s (2).wav") != NULL, "Names: never over a take: ' (2)' (%s)",
              base(t[1].path));
    } else
        check(0, "Names: no takes");
    rec_free(S.r);
}

static void t_slow_writer(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    rec_status_t st;
    char dir[700], part[800];
    uint64_t p, e;
    rec_cfg_t c = { 0 };
    S.r = mk("slow", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    rec_test_write_delay(S.r, 20000);   /* (20 ms a chunk of 4096 frames: 20 s of audio takes the writer ~4 s) */
    run_to(&S, 5 * BLK);
    p = S.i;
    press(&S);
    run_to(&S, p + 20 * FS);   /* (as fast as it can: no waiting for the writer) */
    e = S.i;
    press(&S);
    block(&S);
    check(S.max_us < 20000.0, "Slow writer: rec_block never waited (longest %.0f us)", S.max_us);
    snprintf(dir, sizeof dir, "%s/slow", ROOT);
    usleep(1500000);
    check(count_files(dir, ".part") == 1 && count_files(dir, ".wav") == 0,
          "Slow writer: half written, a .part and no .wav");
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        part[0] = 0;
        while (d && (e = readdir(d)))
            if (strstr(e->d_name, ".part"))
                snprintf(part, sizeof part, "%s/%s", dir, e->d_name);
        if (d)
            closedir(d);
    }
    {
        uint8_t h[44];
        FILE *f = fopen(part, "rb");
        long size = 0;
        int ok = f && fread(h, 1, 44, f) == 44;
        if (f) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fclose(f);
        }
        check(ok && rd32(h + 40) > 0 && rd32(h + 40) % 4 == 0 && rd32(h + 40) <= (uint32_t)(size - 44) &&
                  rd32(h + 4) == 36 + rd32(h + 40),
              "Slow writer: the .part's header kept up to date (%u bytes of data, file %ld)", ok ? rd32(h + 40) : 0,
              size);
    }
    if (takes(S.r, &t, 1, 20000) == 1)
        expect_take(&t, REC_ST_MASTER, p, e - p, 0, "Slow writer: 20 s, nothing lost within the ring");
    else
        check(0, "Slow writer: no take");
    rec_get(S.r, &st);
    check(st.overruns == 0, "Slow writer: no overrun");
    rec_free(S.r);

    /* an overrun: a 2 s memory (a 4 s ring), a writer far slower than the feed */
    memset(&S, 0, sizeof S);
    c.capture_s = 2;
    S.r = mk("overrun", &c);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT1;
    scene(1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    rec_set_source(S.r, REC_SRC_STEMS);
    rec_test_write_delay(S.r, 30000);
    run_to(&S, 3 * BLK);
    p = S.i;
    press(&S);
    run_to(&S, p + 20 * FS);
    rec_get(S.r, &st);
    check(st.overruns > 0 && st.state != REC_RECORDING, "Overrun: counted (%u blocks), the take stopped", st.overruns);
    check(S.max_us < 20000.0, "Overrun: rec_block never waited (longest %.0f us)", S.max_us);
    rec_test_write_delay(S.r, 0);
    {
        rec_take_t two[2];
        int k;
        if (takes(S.r, two, 2, 20000) == 2)
            for (k = 0; k < 2; k++) {
                char what[96];
                snprintf(what, sizeof what, "Overrun: stream %d holds what came before it (%llu frames)",
                         two[k].stream, (unsigned long long)two[k].frames);
                check(two[k].reason == REC_END_OVERRUN && two[k].frames >= 4 * FS - 2 * BLK,
                      "Overrun: reported, nothing lost up to the ring's size (%llu frames)",
                      (unsigned long long)two[k].frames);
                expect_take(&two[k], two[k].stream, p, two[k].frames, 0, what);
            }
        else
            check(0, "Overrun: no takes");
    }
    rec_set_source(S.r, REC_ST_MASTER);
    block(&S);
    p = S.i;
    press(&S);
    run_to(&S, p + 2 * FS);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 10000) == 1)
        expect_take(&t, REC_ST_MASTER, p, 2 * FS + (BLK - 2 * FS % BLK) % BLK, 0, "Overrun: the next take whole again");
    else
        check(0, "Overrun: no next take");
    rec_free(S.r);
}

static void t_status(void)
{
    sim_t S = { 0 };
    rec_status_t st;
    rec_take_t t;
    int k, ok = 1;
    S.r = mk("status", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_SLOT1;
    scene(0);
    seg(REC_ST_MASTER, 0, 800 * BLK, 16384);
    seg(REC_ST_SLOT1, 0, 1u << 30, 32000);
    run_to(&S, 800 * BLK);
    rec_get(S.r, &st);
    check(st.state == REC_IDLE && st.peak == 16384, "Status: the source's peak in the latest block (%d)", st.peak);
    for (k = 0; k < st.hist_n; k++)
        ok &= st.hist[k] == 128;
    check(st.hist_n == REC_HIST && ok, "Status: idle, the last 128 windows of ~16 ms (%d), 16384 as 128 (%d)",
          st.hist_n, st.hist_n ? st.hist[0] : -1);
    run_to(&S, 810 * BLK);
    rec_get(S.r, &st);
    check(st.peak == 0 && st.hist[REC_HIST - 1] == 0 && st.hist[0] == 128, "Status: the wave scrolls (silence now)");
    rec_set_source(S.r, REC_ST_SLOT1);
    rec_set_mode(S.r, REC_MODE_MANUAL);
    block(&S);
    press(&S);
    run_to(&S, S.i + 30 * BLK);
    rec_get(S.r, &st);
    ok = st.hist_n > 0 && st.hist_n <= 6;
    for (k = 0; k < st.hist_n; k++)
        ok &= st.hist[k] == 249;
    check(st.state == REC_RECORDING && st.peak == 32000 && st.frames == 30 * BLK && ok,
          "Status: recording, the take's own wave (%d windows), frames %llu", st.hist_n,
          (unsigned long long)st.frames);
    press(&S);
    block(&S);
    rec_get(S.r, &st);
    check(st.state == REC_SAVING || st.state == REC_IDLE, "Status: saving, then idle");
    k = takes(S.r, &t, 1, 5000);
    usleep(20000);
    block(&S);
    rec_get(S.r, &st);
    check(k == 1 && st.state == REC_IDLE, "Status: idle when the file is done");
    rec_free(S.r);
}

static void t_capture_stems_overlap(void)
{
    /* two captures in a row and a stems capture: each its own take; a stream absent all along: no file */
    sim_t S = { 0 };
    rec_take_t t[4];
    int n;
    S.r = mk("capture-stems", NULL);
    S.bpm = 120.0;
    S.present = 1u << REC_ST_MASTER | 1u << REC_ST_KIT;
    scene(1);
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    rec_set_length_bars(S.r, 1);
    rec_set_source(S.r, REC_SRC_STEMS);
    play(&S);
    run_to(&S, barline(0, 120.0, 3) + 500);
    press(&S);
    block(&S);
    press(&S);
    block(&S);
    n = takes(S.r, t, 4, 5000);
    check(n == 4, "Capture stems twice: 2 files each (%d)", n);
    if (n == 4) {
        expect_take(&t[0], t[0].stream, 2 * 88200, 88200, 1, "Capture stems: first take");
        expect_take(&t[3], t[3].stream, 2 * 88200, 88200, 1, "Capture stems: second take");
        check(t[0].take != t[2].take && strstr(t[3].path, " Stems 120bpm 1b (2)/") && strstr(t[0].path, " Stems 120bpm 1b/"),
              "Capture stems: two takes, two folders, the second ' (2)' (%s)", t[3].path + strlen(ROOT));
    }
    rec_free(S.r);
}

/* the song jumps back to bar 1 while it plays (Back + Play; the beat goes back, the transport never stops): the bar
 * lines are the new song position's from that frame on */
static void t_jump(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t pj;
    S.r = mk("jump", NULL);
    S.bpm = 120;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 1);
    play(&S);
    run_to(&S, barline(0, 120, 3) + 40000);   /* (in bar 4) */
    pj = S.i;
    S.beat = 0.0;                              /* back to bar 1, still playing */
    run_to(&S, pj + 44100);
    press(&S);
    run_to(&S, barline(pj, 120, 2) + 20000);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, barline(pj, 120, 1), barline(pj, 120, 2) - barline(pj, 120, 1), 1,
                    "Jump to bar 1 while playing: Bars starts on the new bar line");
    else
        check(0, "Jump to bar 1 while playing: no take");
    rec_set_mode(S.r, REC_MODE_CAPTURE);
    run_to(&S, barline(pj, 120, 3) + 30000);
    press(&S);
    block(&S);
    if (takes(S.r, &t, 1, 5000) == 1)
        expect_take(&t, REC_ST_MASTER, barline(pj, 120, 2), barline(pj, 120, 3) - barline(pj, 120, 2), 1,
                    "Jump to bar 1 while playing: Capture 1 bar ends on the new bar line");
    else
        check(0, "Jump to bar 1 while playing: no capture");
    /* a jump during a take: it still runs its bars from where it started */
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 2);
    block(&S);
    press(&S);
    run_to(&S, barline(pj, 120, 4) + 44096);   /* (the take began at bar line 4; half a bar in, on a block's edge) */
    {
        const uint64_t pk = S.i;
        S.beat = 0.0;
        run_to(&S, pk + 3 * 88200);
        if (takes(S.r, &t, 1, 5000) == 1) {
            const uint64_t first = barline(pj, 120, 4);
            check(t.frames == 2 * 88200 && t.bars == 2, "A jump during a 2-bar take: still 2 bars long (%llu frames, %u bars)",
                  (unsigned long long)t.frames, t.bars);
            expect_take(&t, REC_ST_MASTER, first, 2 * 88200, 2, "A jump during a 2-bar take");
        } else
            check(0, "A jump during a 2-bar take: no take");
    }
    rec_free(S.r);
}

/* the tempo changes during a take: its bars are the song's bars */
static void t_tempo_change(void)
{
    sim_t S = { 0 };
    rec_take_t t;
    uint64_t st;
    S.r = mk("tempo", NULL);
    S.bpm = 120;
    S.present = 1u << REC_ST_MASTER;
    scene(1);
    rec_set_mode(S.r, REC_MODE_BARS);
    rec_set_length_bars(S.r, 2);
    play(&S);
    run_to(&S, 30000);
    press(&S);
    st = barline(0, 120, 1);
    run_to(&S, st + 60000);                    /* (in the take's first bar; the tempo halves at this block) */
    S.bpm = 60;
    {
        const uint64_t c = S.i - st;           /* frames at 120: c / 22050 beats; the other beats of its 8 at 60 */
        const uint64_t want = c + (uint64_t)llround((8.0 - c / 22050.0) * 44100.0);
        run_to(&S, st + want + 20000);
        if (takes(S.r, &t, 1, 5000) == 1)
            check(t.bars == 2 && t.frames + 1 >= want && t.frames <= want + 1,
                  "Tempo 120 -> 60 during a 2-bar take: 2 bars of the song (%llu frames, want %llu)",
                  (unsigned long long)t.frames, (unsigned long long)want);
        else
            check(0, "Tempo change during a take: no take");
    }
    rec_free(S.r);
}

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[1024];
        struct stat sb;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (!lstat(p, &sb) && S_ISDIR(sb.st_mode))
            rmtree(p);
        else
            unlink(p);
    }
    if (d)
        closedir(d);
    rmdir(path);
}

int main(void)
{
    struct tm tm = { 0 };
    const char *b = getenv("RECORDER_TEST_DIR");
    snprintf(ROOT, sizeof ROOT, "%s/run-%d", b ? b : "build/recorder-test", (int)getpid());
    if (mkdir(ROOT, 0755)) {
        perror(ROOT);
        return 2;
    }
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 24;
    tm.tm_hour = 15;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    T0 = mktime(&tm);
    printf("recorder_test (%s)\n", ROOT);
    t_bars(120.0, 1);
    t_bars(120.0, 4);
    t_bars(97.3, 1);
    t_bars(97.3, 4);
    t_bars_open();
    t_bars_stopped_arm();
    t_bars_transport_stop();
    t_jump();
    t_tempo_change();
    t_capture_auto();
    t_capture_n();
    t_capture_stopped();
    t_capture_silence();
    t_capture_stems_overlap();
    t_threshold();
    t_manual();
    t_stems();
    t_max();
    t_errors();
    t_settings_mid_take();
    t_names();
    t_status();
    t_slow_writer();
    if (fails) {
        printf("%d of %d checks FAILED (files kept in %s)\n", fails, checks, ROOT);
        return 1;
    }
    rmtree(ROOT);   /* (passed: the takes go) */
    printf("%d checks\nALL RECORDER TESTS PASSED\n", checks);
    return 0;
}
