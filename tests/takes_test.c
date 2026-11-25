/* SPDX-License-Identifier: MPL-2.0 */
/* The take library (src/takes.c), natively: the days and takes listed (newest first, the names parsed, stems, a .part,
 * dot files and the trash never listed), pictures and windows of peaks, trim / normalize / reverse / fade in / out
 * sample-exact against a known signal, the one undo (after an edit, after a delete; kept over a restart), delete and
 * undelete (a stems folder too), the worker's requests and answers, playback (the frames exactly, the position, the
 * end, a stop) with the engine's side on this thread.
 *   tests/takes_test.sh      (builds it plain, with ASan + UBSan, with TSan, and runs each) */
#define _GNU_SOURCE
#include "../src/takes.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* ------------------------------------------------------------------ files --- */
/* the signal: each frame's samples tell its number (L) and its channel (R: its negative, offset) */
static int16_t sig(uint64_t i, int c, int amp)
{
    const int v = (int)((i * 7919u) % (uint64_t)(2 * amp + 1)) - amp;
    return (int16_t)(c ? -v / 2 : v);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
/* a WAV of n frames, ch channels (1, 2); sample(i, c) gives each; amp the signal's peak (0: silence) */
static void wav(const char *rel, uint64_t n, int ch, int amp)
{
    char p[1024];
    uint8_t h[44];
    FILE *f;
    uint64_t i;
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    {   /* (its folders) */
        char d[1024], *s;
        snprintf(d, sizeof d, "%s", p);
        for (s = d + strlen(ROOT) + 1; (s = strchr(s, '/')); s++) {
            *s = 0;
            mkdir(d, 0755);
            *s = '/';
        }
    }
    memcpy(h, "RIFF", 4), put32(h + 4, (uint32_t)(36 + n * 2 * ch)), memcpy(h + 8, "WAVEfmt ", 8), put32(h + 16, 16);
    put16(h + 20, 1), put16(h + 22, (uint16_t)ch), put32(h + 24, 44100), put32(h + 28, 44100u * 2u * (uint32_t)ch);
    put16(h + 32, (uint16_t)(2 * ch)), put16(h + 34, 16), memcpy(h + 36, "data", 4), put32(h + 40, (uint32_t)(n * 2 * ch));
    f = fopen(p, "wb");
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        int c;
        for (c = 0; c < ch; c++) {
            const int16_t s = amp ? sig(i, c, amp) : 0;
            fwrite(&s, 2, 1, f);
        }
    }
    fclose(f);
}
/* a WAV whose 14 parts each have their own peak (an envelope: part k's |sample| peaks at amp[k]) */
static void wav_env(const char *rel, uint64_t n, const int *amp, int parts)
{
    char p[1024];
    uint8_t h[44];
    FILE *f;
    uint64_t i;
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    memcpy(h, "RIFF", 4), put32(h + 4, (uint32_t)(36 + n * 4)), memcpy(h + 8, "WAVEfmt ", 8), put32(h + 16, 16);
    put16(h + 20, 1), put16(h + 22, 2), put32(h + 24, 44100), put32(h + 28, 44100u * 4u), put16(h + 32, 4);
    put16(h + 34, 16), memcpy(h + 36, "data", 4), put32(h + 40, (uint32_t)(n * 4));
    f = fopen(p, "wb");
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        const int k = (int)(i * (uint64_t)parts / n);
        const int16_t s[2] = { (int16_t)(i % 100 == 50 ? amp[k] : amp[k] / 3), (int16_t)(-(amp[k] / 4)) };
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}
/* a file's frames (stereo int16, after a 44-byte header; the library writes those) */
static int16_t *frames_of(const char *rel, uint64_t *n)
{
    char p[1024];
    FILE *f;
    long sz;
    int16_t *v;
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    *n = 0;
    if (!(f = fopen(p, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 44, SEEK_SET);
    v = malloc((size_t)(sz > 44 ? sz - 44 : 4));
    *n = (uint64_t)(sz > 44 ? (sz - 44) / 4 : 0);
    if (fread(v, 4, (size_t)*n, f) != (size_t)*n)
        *n = 0;
    fclose(f);
    return v;
}
static int exists(const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    return !access(p, F_OK);
}
static void touch_file(const char *rel)
{
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    if ((f = fopen(p, "w")))
        fputs("x", f), fclose(f);
}
static void dir(const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    mkdir(p, 0755);
}
static void root(const char *name)
{
    const char *base = getenv("TAKES_TEST_DIR");
    char cmd[1200];
    snprintf(ROOT, sizeof ROOT, "%s/%s-%ld", base ? base : "/tmp", name, (long)getpid());
    snprintf(cmd, sizeof cmd, "%s", ROOT);
    mkdir(cmd, 0755);
}
static uint32_t id_of(tl_t *l, const char *rel)
{
    tl_row_t r[8];
    int t, n, i, d, nd;
    uint32_t f;
    tl_row_t dv[8];
    nd = tl_rows(l, 0, 0, 8, dv, &t, &f);
    for (d = 0; d < nd; d++) {
        int from;
        for (from = 0; from < dv[d].takes; from += 8) {
            n = tl_rows(l, dv[d].id, from, 8, r, &t, &f);
            for (i = 0; i < n; i++)
                if (!strcmp(r[i].rel, rel))
                    return r[i].id;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ the tests --- */
#define D24 "2026-11-24"
#define D23 "2026-11-23"
#define TM D24 "/1124-153000 Master 120bpm 4b.wav"
#define TS2 D24 "/1124-150000 Track 2 Felucca 120bpm 2b.wav"
#define TIN D24 "/1124-140000 Input 1.5s.wav"
#define TOLD D24 "/1124-130000 Felucca 3s.wav"
#define TST D24 "/1124-120000 Stems 120bpm 1b"
#define T23 D23 "/1123-090000 Master 2.25s (2).wav"

static void fixture(void)
{
    wav(TM, 352800, 2, 20000);
    wav(TS2, 176400, 2, 12000);
    wav(TIN, 66150, 1, 8000);
    wav(TOLD, 132300, 2, 5000);
    wav(TST "/Master.wav", 88200, 2, 9000);
    wav(TST "/Track 1 ChoralRoot.wav", 88200, 2, 3000);
    wav(D24 "/1124-160000 Master.part", 1000, 2, 1000);          /* (the recorder writes it: not listed) */
    wav(D24 "/1124-170000 Stems.part/Master.wav", 1000, 2, 1000);
    wav(D24 "/.1124-150000 Track 2 Felucca 120bpm 2b.wav.tmp", 10, 2, 1000);
    touch_file(D24 "/notes.txt");
    wav(T23, 99225, 2, 7000);
    dir("2026-11-22");                                            /* (an empty day: not listed) */
    dir(".trash");
    wav(".trash/2026-11-20_1120-100000 Master 1s.wav", 100, 2, 100);
    wav("2026-11-21/notatake.wav", 100, 2, 100);                 /* (no head: not a take, the day not listed) */
}

static void t_list(void)
{
    tl_row_t r[8];
    int total, n, ok = 1;
    uint32_t f;
    tl_t *l;
    static const char *const LAB[3] = { "ChoralRoot", "Felucca", "" };
    root("list");
    fixture();
    l = tl_new(ROOT, fixed_now);
    tl_set_labels(l, LAB);
    n = tl_rows(l, 0, 0, 8, r, &total, &f);
    check(n == 2 && total == 2 && !f && r[0].kind == TL_DAY && r[0].today && r[0].day == 24 && r[0].month == 11 &&
          r[0].takes == 5 && r[1].day == 23 && !r[1].today && r[1].takes == 1 && r[0].id && r[0].id != r[1].id,
          "List: two days, newest first: today (5 takes), 23 Nov (1); the empty day, a day of no takes not listed (%d)", n);
    {
        const uint32_t today_id = r[0].id, d23 = r[1].id;
        n = tl_rows(l, TL_NEWEST, 0, 8, r, &total, &f);
        check(n == 5 && total == 5 && f == today_id && r[0].folder == today_id,
              "List: the newest day's takes (5): no .part, no dot file, no other file");
        n = tl_rows(l, d23, 0, 8, r, &total, &f);
        check(n == 1 && f == d23 && !strcmp(r[0].rel, T23) && r[0].src == TL_SRC_MASTER && r[0].frames == 99225,
              "List: a day by its id ('(2)' after the name parsed)");
    }
    n = tl_rows(l, TL_NEWEST, 0, 8, r, &total, &f);
    ok &= !strcmp(r[0].rel, TM) && r[0].src == TL_SRC_MASTER && r[0].bars == 4 && r[0].bpm == 120 && r[0].frames == 352800;
    ok &= !strcmp(r[1].rel, TS2) && r[1].src == TL_SRC_SLOT2 && r[1].bars == 2 && r[1].frames == 176400;
    ok &= !strcmp(r[2].rel, TIN) && r[2].src == TL_SRC_INPUT && r[2].bars == 0 && r[2].frames == 66150 && r[2].rate == 44100;
    ok &= !strcmp(r[3].rel, TOLD) && r[3].src == TL_SRC_SLOT2 && r[3].frames == 132300;   /* (an older name: its firmware) */
    ok &= !strcmp(r[4].rel, TST) && r[4].kind == TL_STEMS && r[4].src == TL_SRC_STEMS && r[4].bars == 1 &&
          r[4].frames == 88200;
    check(ok && r[0].kind == TL_TAKE, "List: newest first, sources (M, Track 2, the input, a firmware's name, stems), "
                                      "bars, tempo, frames from the header (a stems take: its Master)");
    n = tl_rows(l, TL_NEWEST, 3, 3, r, &total, &f);
    check(n == 2 && total == 5 && !strcmp(r[0].rel, TOLD), "List: a window (from 3: two left)");
    n = tl_rows(l, 12345, 0, 8, r, &total, &f);
    check(n == 0 && total == 0 && f == 0, "List: no such day: nothing");
    {
        tl_row_t a, b;
        const uint32_t id = id_of(l, TS2);
        check(id && tl_find(l, id, &a) && !strcmp(a.rel, TS2) && a.frames == 176400, "Find: a take by its id");
        check(tl_row_of(l, "/nowhere/x.wav", &b) == 0, "Row of a path outside the folder: none");
    }
    {
        char p[1100];
        tl_row_t a;
        snprintf(p, sizeof p, "%s/%s/Master.wav", ROOT, TST);
        check(tl_row_of(l, p, &a) && a.kind == TL_STEMS && !strcmp(a.rel, TST),
              "Row of a stems take's Master.wav (the recorder's result): the stems take");
        snprintf(p, sizeof p, "%s/%s", ROOT, TM);
        check(tl_row_of(l, p, &a) && a.kind == TL_TAKE && a.id == id_of(l, TM), "Row of a saved take's path");
    }
    tl_free(l);
}

/* a peak as the library gives it: |sample| as 0..255 (not 0: at least 1), scaled to the take's peak (32000 here) */
static int P8(int v) { return v ? (v * 255 + 32767) / 32768 : 0; }
static int SC(int v)
{
    const int m = P8(32000), s = (P8(v) * 255 + m - 1) / m;
    return s > 255 ? 255 : s;
}
static void t_peaks(void)
{
    static const int AMP[14] = { 1000, 30000, 500, 8000, 16000, 0, 32000, 2000, 4000, 12000, 24000, 6000, 300, 20000 };
    tl_row_t r[8];
    int total, k, ok = 1;
    uint32_t f, id;
    uint8_t pk[64];
    tl_t *l;
    root("peaks");
    dir(D24);
    wav_env(D24 "/1124-100000 Master 10s.wav", 14 * 10240, AMP, 14);   /* (parts of 40 table steps) */
    l = tl_new(ROOT, fixed_now);
    tl_rows(l, TL_NEWEST, 0, 1, r, &total, &f);
    for (k = 0; k < 14; k++)
        ok &= r[0].thumb[k] == SC(AMP[k]);
    check(ok, "Thumbs: 14 peaks over the take, each its part's peak (linear, scaled to the take's peak: 32000 255; "
              "silence 0; %d %d)", r[0].thumb[6], r[0].thumb[0]);
    id = r[0].id;
    ok = tl_peaks(l, id, 0, 0, 14, pk);
    for (k = 0; k < 14; k++)
        ok &= pk[k] == r[0].thumb[k];
    check(ok, "Peaks: the whole take in 14 columns (count 0: to its end) as the picture");
    ok = tl_peaks(l, id, 3 * 10240, 2 * 10240, 2, pk) && pk[0] == SC(8000) && pk[1] == SC(16000);
    check(ok, "Peaks: a window (parts 3 and 4), from the table (%d %d)", pk[0], pk[1]);
    ok = tl_peaks(l, id, 61540, 64, 64, pk);            /* (narrow: the frames themselves; frame 61550 the peak) */
    for (k = 0; k < 64; k++)
        ok &= pk[k] == (k == 10 ? SC(32000) : SC(32000 / 3));
    check(ok, "Peaks: a narrow window (64 frames, a column a frame) read from the file, exact (%d %d)", pk[10], pk[0]);
    ok = tl_peaks(l, id, 139990, 1000, 4, pk) && pk[0] > 0;
    check(ok, "Peaks: a window past the end: what is there");
    check(!tl_peaks(l, 999, 0, 0, 4, pk), "Peaks: no such take: none");
    tl_free(l);
}

/* the edits against the signal */
static void t_edits(void)
{
    const uint64_t N = 44100;
    int16_t *a, *b;
    uint64_t n, i;
    uint32_t id;
    tl_row_t after;
    int ok;
    tl_t *l;
    root("edits");
    wav(D24 "/1124-100000 Master 1s.wav", N, 2, 10000);
    l = tl_new(ROOT, fixed_now);
    id = id_of(l, D24 "/1124-100000 Master 1s.wav");
    a = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    /* trim: [1000, 6000), the cut ends faded TL_CUT_FADE frames */
    ok = tl_edit(l, id, TL_OP_TRIM, 1000, 5000, 0, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    check(ok && n == 5000 && after.id == id && after.frames == 5000 && tl_has_undo(l),
          "Trim: the file 5000 frames, the same take (its id), an undo");
    ok = 1;
    for (i = 0; i < 5000; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            int64_t s = a[2 * (1000 + i) + c];
            if (i < TL_CUT_FADE)
                s = s * (int64_t)i / TL_CUT_FADE;
            if (i >= 5000 - TL_CUT_FADE)
                s = s * (int64_t)(5000 - 1 - i) / TL_CUT_FADE;
            ok &= b[2 * i + c] == (int16_t)s;
        }
    }
    check(ok, "Trim: sample-exact: frames 1000..5999 of it, %d frames faded at each cut end (linear)", TL_CUT_FADE);
    free(b);
    check(tl_undo(l, &after) && after.frames == N && !tl_has_undo(l), "Undo: the take as it was (%llu frames), no undo left",
          (unsigned long long)after.frames);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    check(n == N && !memcmp(a, b, N * 4), "Undo: the file exactly the one before the trim");
    free(b);
    /* trim from the start to before the end: no fade at the start */
    ok = tl_edit(l, id, TL_OP_TRIM, 0, 3000, 0, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    ok &= n == 3000 && !memcmp(a, b, (3000 - TL_CUT_FADE) * 4) && b[2 * 2999] == 0;
    check(ok, "Trim from 0: no fade at the start (frames 0.. as they were), faded at the cut end");
    free(b);
    tl_undo(l, &after);
    /* normalize: the peak (10000) to TL_NORM_PEAK */
    ok = tl_edit(l, id, TL_OP_NORMALIZE, 0, 0, 0, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    {
        int pk = 0;
        for (i = 0; i < 2 * N; i++) {
            const int64_t v = (int64_t)a[i] * TL_NORM_PEAK;
            const int64_t w = v >= 0 ? (v + 5000) / 10000 : -((-v + 5000) / 10000);
            ok &= b[i] == (int16_t)w;
            pk = abs(b[i]) > pk ? abs(b[i]) : pk;
        }
        check(ok && n == N && pk == TL_NORM_PEAK, "Normalize: each sample x %d / 10000 (rounded), the peak %d (-0.3 dBFS)",
              TL_NORM_PEAK, pk);
    }
    free(b);
    tl_undo(l, &after);
    /* reverse */
    ok = tl_edit(l, id, TL_OP_REVERSE, 0, 0, 0, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    for (i = 0; ok && i < N; i++)
        ok &= b[2 * i] == a[2 * (N - 1 - i)] && b[2 * i + 1] == a[2 * (N - 1 - i) + 1];
    check(ok && n == N, "Reverse: frame i is frame N-1-i, both channels (over chunk ends)");
    free(b);
    tl_undo(l, &after);
    /* fade in / out, 10 ms: 441 frames */
    ok = tl_edit(l, id, TL_OP_FADE_IN, 0, 0, 10, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    for (i = 0; ok && i < N; i++)
        ok &= b[2 * i] == (int16_t)(i < 441 ? (int64_t)a[2 * i] * (int64_t)i / 441 : a[2 * i]);
    check(ok, "Fade in, 10 ms: frames 0..440 x i / 441, the rest as they were");
    free(b);
    ok = tl_edit(l, id, TL_OP_FADE_OUT, 0, 0, 10, &after);   /* (over the faded in: the undo is this one's) */
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    for (i = N - 441; ok && i < N; i++)
        ok &= b[2 * i + 1] == (int16_t)((int64_t)a[2 * i + 1] * (int64_t)(N - 1 - i) / 441);
    check(ok, "Fade out, 10 ms: the last 441 frames x (N-1-i) / 441");
    free(b);
    tl_undo(l, &after);
    b = frames_of(D24 "/1124-100000 Master 1s.wav", &n);
    ok = n == N;
    for (i = 0; ok && i < 441; i++)                     /* (one step: the fade out undone, the fade in stays) */
        ok &= b[2 * i] == (int16_t)((int64_t)a[2 * i] * (int64_t)i / 441);
    check(ok && !tl_has_undo(l) && !tl_undo(l, &after), "Undo is one step: the last edit undone, then nothing");
    free(b);
    check(!tl_edit(l, id, TL_OP_TRIM, N + 5, 10, 0, &after) && !tl_edit(l, 777, TL_OP_REVERSE, 0, 0, 0, &after),
          "Edits refused: a trim past the end, no such take");
    {   /* a mono WAV: listed, played, not edited */
        uint32_t mid;
        wav(D24 "/1124-090000 Input 1s.wav", 1000, 1, 100);
        mid = id_of(l, D24 "/1124-090000 Input 1s.wav");
        check(mid && !tl_edit(l, mid, TL_OP_REVERSE, 0, 0, 0, &after) && exists(D24 "/1124-090000 Input 1s.wav"),
              "A mono WAV: not edited (the recorder's are stereo), left as it is");
    }
    {   /* no .tmp left beside */
        char p[1100];
        snprintf(p, sizeof p, "%s/%s/.1124-100000 Master 1s.wav.tmp", ROOT, D24);
        check(access(p, F_OK) != 0, "No temporary file left beside the take");
    }
    free(a);
    tl_free(l);
}

static void t_delete(void)
{
    tl_row_t r[8], after;
    int total, n;
    uint32_t f, id, sid;
    tl_t *l;
    char p[1100];
    root("delete");
    fixture();
    l = tl_new(ROOT, fixed_now);
    id = id_of(l, TS2);
    check(tl_edit(l, id, TL_OP_DELETE, 0, 0, 0, &after) && !exists(TS2) && tl_has_undo(l) && !after.id,
          "Delete: the take gone from its day, an undo");
    snprintf(p, sizeof p, ".trash/%s", "2026-11-24_1124-150000 Track 2 Felucca 120bpm 2b.wav");
    n = tl_rows(l, TL_NEWEST, 0, 8, r, &total, &f);
    check(exists(p) && n == 4 && total == 4 && strcmp(r[1].rel, TS2), "Delete: into the hidden trash (never listed): 4 takes");
    check(tl_undo(l, &after) && exists(TS2) && !exists(p) && after.id == id && !strcmp(after.rel, TS2) &&
          after.frames == 176400, "Undo: the take back where it was, its row");
    sid = id_of(l, TST);
    check(sid && !tl_edit(l, sid, TL_OP_REVERSE, 0, 0, 0, &after) && !tl_edit(l, sid, TL_OP_TRIM, 0, 100, 0, &after) &&
          exists(TST), "A stems take: no edits but delete in this stage");
    check(tl_edit(l, sid, TL_OP_DELETE, 0, 0, 0, &after) && !exists(TST) && tl_has_undo(l),
          "Delete a stems take: its whole folder");
    snprintf(p, sizeof p, ".trash/2026-11-24_1124-120000 Stems 120bpm 1b");
    check(exists(p) && exists(".trash/2026-11-24_1124-120000 Stems 120bpm 1b/Master.wav"), "...its folder in the trash");
    check(tl_edit(l, id_of(l, TM), TL_OP_REVERSE, 0, 0, 0, &after) && !exists(p),
          "An edit after the delete: its undo replaces the delete's (the stems take's trash gone for good: one step)");
    tl_undo(l, &after);
    id = id_of(l, TM);
    check(tl_edit(l, id, TL_OP_DELETE, 0, 0, 0, &after), "Delete again (for the restart)");
    tl_free(l);
    l = tl_new(ROOT, fixed_now);                        /* the undo kept over a restart */
    check(tl_has_undo(l) && tl_undo(l, &after) && exists(TM) && after.id == id,
          "Undo after a restart (.trash/undo.txt): the take back");
    {   /* undelete never over a take that came there meanwhile */
        id = id_of(l, TIN);
        tl_edit(l, id, TL_OP_DELETE, 0, 0, 0, &after);
        wav(TIN, 10, 2, 10);
        check(!tl_undo(l, &after) && tl_has_undo(l), "Undo never over a take of the same name made meanwhile");
    }
    tl_free(l);
    {   /* a hostile undo.txt: never outside the trash */
        FILE *u;
        snprintf(p, sizeof p, "%s/.trash/undo.txt", ROOT);
        u = fopen(p, "w");
        fputs("1\n..\n", u);
        fclose(u);
        l = tl_new(ROOT, fixed_now);
        check(!tl_has_undo(l) && exists(TM), "A bad undo.txt ('..'): no undo, nothing removed");
        tl_free(l);
    }
    {   /* an undo.txt naming a path outside a day, its trash file there: still no undo */
        FILE *u;
        snprintf(p, sizeof p, "%s/.trash/undo.txt", ROOT);
        u = fopen(p, "w");
        fputs("1\n../escaped.wav\n", u);
        fclose(u);
        wav(".trash/_._escaped.wav", 10, 2, 10);
        l = tl_new(ROOT, fixed_now);
        check(!tl_has_undo(l) && !tl_undo(l, &after) && exists(".trash/_._escaped.wav"),
              "An undo.txt with a path outside the days: no undo, nothing moved");
        n = tl_rows(l, 0, 0, 8, r, &total, &f);
        check(n >= 1 && r[n - 1].kind == TL_DAY && !tl_edit(l, r[n - 1].id, TL_OP_DELETE, 0, 0, 0, &after) &&
              exists(T23) && !tl_has_undo(l), "Delete asked for a day: nothing (only a take is deleted)");
        tl_free(l);
    }
}

/* the worker: requests posted, answers polled; a newer PEAKS replaces one not started */
static int poll_for(tl_t *l, tl_ans_t *a, int ms)
{
    while (ms-- > 0) {
        if (tl_poll(l, a))
            return 1;
        usleep(1000);
    }
    return 0;
}
static void t_worker(void)
{
    tl_req_t q;
    tl_ans_t *a = malloc(sizeof *a);
    tl_t *l;
    char p[1100];
    int got = 0, n, i;
    root("worker");
    fixture();
    l = tl_new(ROOT, fixed_now);
    memset(&q, 0, sizeof q);
    q.kind = TL_Q_ROWS, q.take = TL_NEWEST, q.from = 0, q.n = 3;
    tl_post(l, &q);
    check(poll_for(l, a, 3000) && a->kind == TL_Q_ROWS && a->asked == TL_NEWEST && a->total == 5 && a->n == 3 &&
          a->folder == a->rows[0].folder && !strcmp(a->rows[0].rel, TM), "Worker: ROWS answered (the newest day's three)");
    q.kind = TL_Q_ROWS, q.take = 0, q.from = 0, q.n = 3;
    tl_post(l, &q);
    check(poll_for(l, a, 3000) && a->asked == 0 && a->folder == 0 && a->total == 2 && a->rows[0].kind == TL_DAY,
          "Worker: the days");
    q.kind = TL_Q_ROWS, q.take = 4242, q.n = 3;
    tl_post(l, &q);
    check(poll_for(l, a, 3000) && a->asked == 4242 && a->folder == 4242 && a->total == 0,
          "Worker: a day not there: answered empty, as asked");
    for (i = 0; i < 6; i++) {                           /* (six views: the last one answered, some dropped) */
        q.kind = TL_Q_PEAKS, q.take = id_of(l, TM), q.from = (uint64_t)i * 1000, q.count = 64000, q.n = 64;
        tl_post(l, &q);
    }
    n = 0;
    while (poll_for(l, a, 2000)) {
        got = a->kind == TL_Q_PEAKS && a->pfrom == 5000 && a->n == 64 && a->pk[0] > 0;
        n++;
    }
    check(got && n >= 1 && n <= 6, "Worker: PEAKS, the latest view answered last (%d answers for 6 asks)", n);
    q.kind = TL_Q_EDIT, q.take = id_of(l, TOLD), q.op = TL_OP_REVERSE;
    tl_post(l, &q);
    check(poll_for(l, a, 5000) && a->kind == TL_Q_EDIT && a->ok && a->has_row && a->row.frames == 132300 && tl_has_undo(l),
          "Worker: an EDIT, its row after it");
    q.kind = TL_Q_UNDO;
    tl_post(l, &q);
    check(poll_for(l, a, 5000) && a->kind == TL_Q_UNDO && a->op == TL_OP_UNDO && a->ok && a->has_row &&
          !strcmp(a->row.rel, TOLD), "Worker: UNDO, the take restored");
    q.kind = TL_Q_SAVED, q.mode = 3;
    snprintf(q.path, sizeof q.path, "%s/%s", ROOT, TIN);
    tl_post(l, &q);
    check(poll_for(l, a, 5000) && a->kind == TL_Q_SAVED && a->has_row && a->mode == 3 && !strcmp(a->row.rel, TIN) &&
          a->row.src == TL_SRC_INPUT, "Worker: SAVED (a file the recorder saved): its row, the mode back");
    (void)p;
    tl_free(l);
    free(a);
}

/* playback: the engine's side here; the frames out exactly the file's, the position, the end, a stop */
static void t_play(void)
{
    tl_t *l;
    int16_t blk[256], *all, *a;
    uint64_t n, pos, k = 0;
    uint32_t id, pid;
    int ok = 1, tries = 0, under = 0;
    root("play");
    fixture();
    l = tl_new(ROOT, fixed_now);
    id = id_of(l, TS2);
    a = frames_of(TS2, &n);
    all = calloc(176400 * 2, 2);
    tl_play(l, id, 1000, 100000);
    while (k < 100000 && tries++ < 200000) {
        memset(blk, 0, sizeof blk);
        tl_mix(l, blk, 128);
        tl_playing(l, &pid, &pos);
        if (pos >= 1000 + k && (pid == id || pos > 1000 + k)) {   /* (its last block: it ends with it) */
            const uint64_t got = pos - 1000 - k;
            if (got < 128)
                under++;
            memcpy(all + 2 * k, blk, (size_t)got * 4);
            k += got;
            ok &= pos == 1000 + k;
        }
        usleep(50);
    }
    ok &= k == 100000 && !memcmp(all, a + 2 * 1000, 100000 * 4);
    check(ok, "Play [1000, 101000): the frames out exactly the file's (%llu frames, %d blocks short while it read)",
          (unsigned long long)k, under);
    tries = 0;
    do {
        memset(blk, 0, sizeof blk);
        tl_mix(l, blk, 128);
        tl_playing(l, &pid, &pos);
        usleep(100);
    } while (pid && tries++ < 1000);
    check(!pid && pos == 101000, "Play: at its end, nothing plays (the last position %llu)", (unsigned long long)pos);
    memset(blk, 0, sizeof blk);
    tl_mix(l, blk, 128);
    check(!blk[0] && !blk[255], "Play: after it, the block untouched");
    tl_play(l, id_of(l, TST), 0, 0);                    /* a stems take: its Master; a stop at once */
    tries = 0;
    do {
        tl_mix(l, blk, 128);
        tl_playing(l, &pid, &pos);
        usleep(500);
    } while ((!pid || pos < 2000) && tries++ < 4000);
    check(pid == id_of(l, TST) && pos >= 2000, "Play a stems take: its Master plays (at %llu)", (unsigned long long)pos);
    tl_stop(l);
    tries = 0;
    do {
        tl_mix(l, blk, 128);
        tl_playing(l, &pid, &pos);
        usleep(200);
    } while (pid && tries++ < 4000);
    check(!pid, "Stop: nothing plays");
    tl_play(l, 9999, 0, 0);                             /* (no such take: nothing) */
    tries = 0;
    do {
        memset(blk, 0, sizeof blk);
        tl_mix(l, blk, 128);
        usleep(200);
    } while (tries++ < 50);
    tl_playing(l, &pid, &pos);
    check(!pid && !blk[0], "Play a take not there: nothing");
    free(all);
    free(a);
    tl_free(l);
}

/* (the page's Sampler window) rename: the rules, the head and .wav kept, the undo follows; the list as JSON; a take's
 * files; the page's tagged requests on the worker (never dropped for a newer view) */
static void t_page(void)
{
    tl_t *l;
    tl_row_t after;
    tl_req_t q;
    tl_ans_t *a = malloc(sizeof *a);
    char *j, p[1400], w[1400];
    uint32_t id, nid;
    int i, n, tags;
    root("page");
    fixture();
    l = tl_new(ROOT, fixed_now);
    id = id_of(l, TOLD);
    check(tl_rename(l, id, "a/b", &after) == -1 && tl_rename(l, id, ".hidden", &after) == -1 &&
          tl_rename(l, id, "", &after) == -1 && tl_rename(l, id, "   ", &after) == -1 &&
          tl_rename(l, id, "x\ny", &after) == -1 && tl_rename(l, id, "back\\slash", &after) == -1 && exists(TOLD),
          "Rename: \"/\", a leading \".\", empty, control characters refused; the file as it was");
    check(tl_rename(l, id, "Felucca 120bpm 2b", &after) == 1 && !exists(TOLD) &&
          exists(D24 "/1124-130000 Felucca 120bpm 2b.wav") && after.id == id_of(l, D24 "/1124-130000 Felucca 120bpm 2b.wav") &&
          after.id != id && after.bars == 2 && after.bpm == 120 && after.frames == 132300,
          "Rename: the head and .wav kept, the list parses it (bars, tempo), a new id");
    nid = after.id;
    check(tl_rename(l, nid, "1124-130000 Old one.wav", &after) == 1 && exists(D24 "/1124-130000 Old one.wav"),
          "Rename: a full name with its own head taken as it is");
    nid = after.id;
    wav(D24 "/1124-130000 Taken.wav", 100, 2, 100);
    check(tl_rename(l, nid, "Taken", &after) == -2 && exists(D24 "/1124-130000 Old one.wav"),
          "Rename: an existing name refused (-2)") ;
    check(tl_rename(l, nid, "1124-153000 Master 120bpm 4b.wav", &after) == 1 &&
          exists(D24 "/1124-130000 1124-153000 Master 120bpm 4b.wav") && exists(TM),
          "Rename: another take's head is a name after ours (never over that take)");
    nid = after.id;
    check(tl_rename(l, id_of(l, TST), "Mine", &after) == 0 && exists(TST "/Master.wav"), "Rename: a stems take keeps its name");
    check(tl_rename(l, 4242, "x", &after) == 0, "Rename: no such take");
    check(tl_edit(l, nid, TL_OP_REVERSE, 0, 0, 0, &after) && tl_has_undo(l) &&
          tl_rename(l, nid, "Reversed", &after) == 1 && exists(D24 "/1124-130000 Reversed.wav") &&
          tl_undo(l, &after) && !strcmp(after.rel, D24 "/1124-130000 Reversed.wav") &&
          !exists(D24 "/1124-130000 1124-153000 Master 120bpm 4b.wav"),
          "Rename: the undo follows the take (the edit undone under its new name)");
    j = tl_list_json(l, TL_NEWEST);
    check(j && strstr(j, "\"days\":[{\"id\":") && strstr(j, "\"date\":\"2026-11-24\",\"today\":1,\"takes\":6") &&
          strstr(j, "\"date\":\"2026-11-23\",\"today\":0,\"takes\":1") &&
          strstr(j, "\"name\":\"1124-153000 Master 120bpm 4b.wav\",\"date\":\"2026-11-24\",\"bars\":4,\"bpm\":120.00,"
                    "\"frames\":352800,\"rate\":44100") &&
          strstr(j, "\"kind\":\"stems\",\"src\":5") && strstr(j, "\"name\":\"1124-130000 Reversed.wav\"") &&
          strstr(j, "\"undo\":0") && !strstr(j, ".part") && !strstr(j, "notes"),
          "List JSON: the days, the newest day's takes (names, bars, tempo, frames; stems), no .part");
    free(j);
    {
        tl_row_t dv[4];
        int t;
        uint32_t f;
        n = tl_rows(l, 0, 0, 4, dv, &t, &f);
        j = n == 2 ? tl_list_json(l, dv[1].id) : NULL;
        check(j && strstr(j, "1123-090000 Master 2.25s (2).wav") && !strstr(j, "Reversed"), "List JSON: an older day by its id");
        free(j);
        j = tl_list_json(l, 4242);
        check(j && strstr(j, "Reversed"), "List JSON: a day not there: the newest");
        free(j);
    }
    check(tl_file(l, id_of(l, TST), p, sizeof p, w, sizeof w, &after) && after.kind == TL_STEMS &&
          strstr(p, "1124-120000 Stems 120bpm 1b") && !strstr(p, "Master.wav") && strstr(w, "Stems 120bpm 1b/Master.wav") &&
          tl_file(l, id_of(l, TM), p, sizeof p, w, sizeof w, NULL) && !strcmp(p, w) && strstr(p, TM) &&
          !tl_file(l, 4242, p, sizeof p, w, sizeof w, NULL),
          "Files: a take's WAV; a stems take's folder and its Master.wav");
    /* the worker: the page's requests answered with their tags, every PEAKS answered */
    memset(&q, 0, sizeof q);
    for (i = 0; i < 4; i++) {
        q.kind = TL_Q_PEAKS, q.take = id_of(l, TM), q.from = 0, q.count = 0, q.n = 32, q.tag = 100 + (uint32_t)i;
        tl_post(l, &q);
    }
    q.kind = TL_Q_LIST, q.take = 0, q.tag = 200;
    tl_post(l, &q);
    q.kind = TL_Q_FILE, q.take = id_of(l, TM), q.tag = 201;
    tl_post(l, &q);
    q.kind = TL_Q_RENAME, q.take = id_of(l, TIN), q.tag = 202;
    snprintf(q.path, sizeof q.path, "%s", "Voice 1.5s");
    tl_post(l, &q);
    q.kind = TL_Q_RENAME, q.take = id_of(l, TM), q.tag = 203;
    snprintf(q.path, sizeof q.path, "%s", ".x");
    tl_post(l, &q);
    tags = 0;
    while (poll_for(l, a, 3000)) {
        if (a->kind == TL_Q_PEAKS && a->tag >= 100 && a->tag < 104 && a->n == 32 && a->pk[0] > 0)
            tags |= 1 << (a->tag - 100);
        if (a->kind == TL_Q_LIST && a->tag == 200 && a->ok && a->text && strstr(a->text, "\"takes\":["))
            tags |= 1 << 4;
        if (a->kind == TL_Q_FILE && a->tag == 201 && a->ok && a->text && strchr(a->text, '\n') && strstr(a->text, TM))
            tags |= 1 << 5;
        if (a->kind == TL_Q_RENAME && a->tag == 202 && a->ok == 1 && a->has_row &&
            !strcmp(a->row.rel, D24 "/1124-140000 Voice 1.5s.wav"))
            tags |= 1 << 6;
        if (a->kind == TL_Q_RENAME && a->tag == 203 && a->ok == -1 && !a->has_row)
            tags |= 1 << 7;
        free(a->text);
    }
    check(tags == 0xFF, "Worker: the page's PEAKS (each answered), LIST, FILE, RENAME (done, refused) with their tags "
                        "(%02x)", tags);
    for (i = 0; i < 40; i++) {                          /* (answers never polled: their texts freed, ASan sees) */
        q.kind = TL_Q_LIST, q.take = 0, q.tag = 300 + (uint32_t)i;
        tl_post(l, &q);
    }
    usleep(300000);
    tl_free(l);
    free(a);
}

int main(void)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2026 - 1900, tm.tm_mon = 10, tm.tm_mday = 24, tm.tm_hour = 15, tm.tm_min = 30, tm.tm_isdst = -1;
    T0 = mktime(&tm);
    printf("takes_test\n");
    t_list();
    t_peaks();
    t_edits();
    t_delete();
    t_worker();
    t_play();
    t_page();
    printf("takes_test: %d checks, %d failed\n", checks, fails);
    if (!fails)
        printf("ALL TAKES TESTS PASSED\n");
    return fails ? 1 : 0;
}
