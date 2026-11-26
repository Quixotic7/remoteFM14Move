/* SPDX-License-Identifier: MPL-2.0 */
/* The kit (src/kit.c) natively: the sound files (WAV 16 / 24-bit and float, mono and stereo, AIFF and AIFC sowt, the
 * rate kept), a sound past the kit's memory refused (the pad keeps the one it had), a missing file, the voices (a one
 * shot sample for sample, a note's frame, reverse, an octave up, start / end, a loop while held, momentary's release,
 * the attack, the one shot's tail ending at its end, the hold's gate, a choke group, a pad again, mute, level and pan,
 * velocity, 24 voices and the oldest stolen), Samples' rows (folders first, by name, the pictures, a folder entered),
 * Takes' rows from the take library and a take loaded, the preview (after the tap: kit_preview_mix), the file work on
 * the take library's worker (TL_Q_JOB) and on the kit's own thread; (stage 3b) Stretch, the sends and the kit's
 * strip, kits saved / listed / opened, a whole folder onto the pads; (stage 3c) mix and join, a pad's own edits (the
 * source untouched), a take's slices (hits, equal, steps), a slice heard, a sound shared by pads. Ends with ALL KIT
 * TESTS PASSED.
 *   tests/kit_test.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/kit.h"
#include "../src/takes.h"

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
static char DIRP[512];

/* ---- files */
static void w16le(FILE *f, unsigned v) { fputc(v & 255, f), fputc(v >> 8 & 255, f); }
static void w32le(FILE *f, uint32_t v) { w16le(f, v & 0xFFFF), w16le(f, v >> 16); }
static void w16be(FILE *f, unsigned v) { fputc(v >> 8 & 255, f), fputc(v & 255, f); }
static void w32be(FILE *f, uint32_t v) { w16be(f, v >> 16), w16be(f, v & 0xFFFF); }
/* a WAV: frames of a ramp (frame i: i * 7 - 3000 on L, -(that) on R; mono: L), bits 16 / 24 / 32 (float: fmt 3) */
static int16_t ramp(uint32_t i) { return (int16_t)((int)(i * 7u % 6000u) - 3000); }
static void wav(const char *path, uint32_t frames, int ch, int bits, int flt, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    const uint32_t bps = (uint32_t)bits / 8, data = frames * (uint32_t)ch * bps;
    uint32_t i;
    int c;
    fwrite("RIFF", 1, 4, f), w32le(f, 36 + data), fwrite("WAVEfmt ", 1, 8, f), w32le(f, 16);
    w16le(f, flt ? 3 : 1), w16le(f, (unsigned)ch), w32le(f, rate), w32le(f, rate * (uint32_t)ch * bps);
    w16le(f, (unsigned)ch * bps), w16le(f, (unsigned)bits), fwrite("data", 1, 4, f), w32le(f, data);
    for (i = 0; i < frames; i++)
        for (c = 0; c < ch; c++) {
            const int16_t v = (int16_t)(c ? -ramp(i) : ramp(i));
            if (flt) {
                const float x = v / 32767.0f;
                fwrite(&x, 4, 1, f);
            } else if (bits == 16)
                w16le(f, (uint16_t)v);
            else
                fputc(0x55, f), w16le(f, (uint16_t)v);  /* (24-bit: a low byte, then the 16 bits) */
        }
    fclose(f);
}
static void aiff(const char *path, uint32_t frames, int ch, int sowt, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    const uint32_t data = frames * (uint32_t)ch * 2u, comm = sowt ? 18 + 4 + 2 : 18;
    uint32_t i;
    int c, e = 16383 + 15;                              /* (rate as an 80-bit extended: 2^15 <= 48000 < 2^16) */
    uint64_t m = (uint64_t)rate << (63 - 15);
    fwrite("FORM", 1, 4, f), w32be(f, 4 + 8 + comm + 8 + 8 + data), fwrite(sowt ? "AIFC" : "AIFF", 1, 4, f);
    fwrite("COMM", 1, 4, f), w32be(f, comm), w16be(f, (unsigned)ch), w32be(f, frames), w16be(f, 16);
    w16be(f, (unsigned)e), w32be(f, (uint32_t)(m >> 32)), w32be(f, (uint32_t)m);
    if (sowt)
        fwrite("sowt", 1, 4, f), w16be(f, 0);
    fwrite("SSND", 1, 4, f), w32be(f, 8 + data), w32be(f, 0), w32be(f, 0);
    for (i = 0; i < frames; i++)
        for (c = 0; c < ch; c++) {
            const int16_t v = (int16_t)(c ? -ramp(i) : ramp(i));
            if (sowt)
                w16le(f, (uint16_t)v);
            else
                w16be(f, (uint16_t)v);
        }
    fclose(f);
}

/* ---- the kit, its file work by hand */
static kit_t *mk(uint32_t max)
{
    kit_cfg_t c;
    memset(&c, 0, sizeof c);
    c.fs = 48000, c.max_bytes = max;
    c.samples_dir = DIRP;
    return kit_new(&c);
}
static int load(kit_t *k, int pad, const char *path, kit_ans_t *a)   /* the answer's status (-1: none) */
{
    kit_load(k, pad, path);
    kit_work(k);
    return kit_poll(k, a) ? a->status : -1;
}
/* n frames of the bus */
static float B[2 * 48000];
static void run(kit_t *k, uint32_t n)
{
    uint32_t at = 0;
    memset(B, 0, sizeof(float) * 2 * n);
    while (at < n) {
        const uint32_t m = n - at < 128 ? n - at : 128;
        kit_render(k, B + 2 * at, m);
        at += m;
    }
}
static int nonzero_until(uint32_t n)   /* the last frame with sound + 1 */
{
    int i, last = 0;
    for (i = 0; i < (int)n; i++)
        if (fabsf(B[2 * i]) > 0.5f || fabsf(B[2 * i + 1]) > 0.5f)
            last = i + 1;
    return last;
}

static void t_files(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int ok;
    snprintf(p, sizeof p, "%s/m16.wav", DIRP), wav(p, 1000, 1, 16, 0, 44100);
    CHECK(load(k, 0, p, &a) == KIT_OK && a.kind == KIT_A_LOADED && a.pad == 0 && a.frames == 1000 && a.rate == 44100 &&
          !strcmp(a.name, "m16") && !strcmp(a.path, p) && a.npk == KIT_PEAKS && kit_frames(k, 0) == 1000);
    snprintf(p, sizeof p, "%s/s24.wav", DIRP), wav(p, 500, 2, 24, 0, 48000);
    CHECK(load(k, 1, p, &a) == KIT_OK && a.frames == 500 && a.rate == 48000);
    snprintf(p, sizeof p, "%s/f32.wav", DIRP), wav(p, 300, 2, 32, 1, 96000);
    CHECK(load(k, 2, p, &a) == KIT_OK && a.frames == 300 && a.rate == 96000);
    snprintf(p, sizeof p, "%s/a.aif", DIRP), aiff(p, 400, 2, 0, 48000);
    CHECK(load(k, 3, p, &a) == KIT_OK && a.frames == 400 && a.rate == 48000);
    snprintf(p, sizeof p, "%s/c.aifc", DIRP), aiff(p, 200, 1, 1, 22050);
    CHECK(load(k, 4, p, &a) == KIT_OK && a.frames == 200 && a.rate == 22050);
    /* the samples as written: each format's frame 123 played at its own pitch (an AIFF at 48 kHz: sample for sample) */
    kit_set(k, 3, KIT_P_MODE, KIT_M_ONESHOT);
    kit_note(k, 3, 0, 127, 0);
    run(k, 400);
    ok = fabsf(B[2 * 123] - ramp(123)) < 1.0f && fabsf(B[2 * 123 + 1] + ramp(123)) < 1.0f;
    CHECK(ok);
    CHECK(kit_bytes(k) == (1000 + 500 + 300 + 400 + 200) * 4u);
    /* missing, not a sound */
    snprintf(p, sizeof p, "%s/none.wav", DIRP);
    CHECK(load(k, 5, p, &a) == KIT_MISSING);
    snprintf(p, sizeof p, "%s/junk.wav", DIRP);
    {
        FILE *f = fopen(p, "wb");
        fputs("RIFF....WAVEjunk", f), fclose(f);
    }
    CHECK(load(k, 5, p, &a) == KIT_FAILED && !kit_frames(k, 5));
    /* emptied */
    CHECK(load(k, 0, "", &a) == KIT_OK && !a.path[0] && !kit_frames(k, 0) && kit_bytes(k) == (500 + 300 + 400 + 200) * 4u);
    kit_free(k);
}
static void t_too_long(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(8000);                                /* (2000 frames a kit) */
    snprintf(p, sizeof p, "%s/one.wav", DIRP), wav(p, 1500, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    snprintf(p, sizeof p, "%s/two.wav", DIRP), wav(p, 600, 2, 16, 0, 48000);
    CHECK(load(k, 1, p, &a) == KIT_TOO_LONG && !kit_frames(k, 1));
    snprintf(p, sizeof p, "%s/three.wav", DIRP), wav(p, 400, 2, 16, 0, 48000);
    CHECK(load(k, 1, p, &a) == KIT_OK);
    snprintf(p, sizeof p, "%s/four.wav", DIRP), wav(p, 1800, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_TOO_LONG && kit_frames(k, 0) == 1500);   /* (the pad keeps the one it had) */
    snprintf(p, sizeof p, "%s/five.wav", DIRP), wav(p, 1600, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK && kit_frames(k, 0) == 1600);   /* (its own old size not counted) */
    kit_free(k);
}
static void t_voices(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int i, ok, n;
    snprintf(p, sizeof p, "%s/v.wav", DIRP), wav(p, 4800, 2, 16, 0, 48000);
    for (i = 0; i < 4; i++)
        load(k, i, p, &a);
    /* a one shot: the sound sample for sample from the note's frame, to its end */
    kit_note(k, 0, 0, 127, 50);
    run(k, 6000);
    for (ok = 1, i = 0; i < 4800; i++)
        ok &= fabsf(B[2 * (50 + i)] - ramp((uint32_t)i)) < 0.01f && fabsf(B[2 * (50 + i) + 1] + ramp((uint32_t)i)) < 0.01f;
    CHECK(ok && fabsf(B[2 * 49]) < 0.01f && nonzero_until(6000) <= 50 + 4800 && !kit_voices(k));
    /* reverse: the last frame first */
    kit_set(k, 0, KIT_P_REVERSE, 1);
    kit_note(k, 0, 0, 127, 0);
    run(k, 5000);
    CHECK(fabsf(B[0] - ramp(4799)) < 0.01f && fabsf(B[2 * 10] - ramp(4789)) < 0.01f && nonzero_until(5000) <= 4801);
    kit_set(k, 0, KIT_P_REVERSE, 0);
    /* an octave up (a pitch, or the right half's 12 semitones): half as long, every other frame */
    kit_set(k, 0, KIT_P_PITCH, 1200);
    kit_note(k, 0, 0, 127, 0);
    run(k, 5000);
    n = nonzero_until(5000);
    CHECK(n >= 2395 && n <= 2401 && fabsf(B[2 * 100] - ramp(200)) < 0.01f);
    kit_set(k, 0, KIT_P_PITCH, 0);
    kit_note(k, 0, 12, 127, 0);
    run(k, 5000);
    CHECK(nonzero_until(5000) <= 2401 && fabsf(B[2 * 100] - ramp(200)) < 0.01f);
    /* start and end: the region alone */
    kit_set(k, 1, KIT_P_START, KIT_FRAC / 4), kit_set(k, 1, KIT_P_END, KIT_FRAC / 2);
    kit_note(k, 1, 0, 127, 0);
    run(k, 5000);
    n = nonzero_until(5000);
    CHECK(n >= 1196 && n <= 1202 && B[0] >= ramp(1199) - 0.5f && B[0] <= ramp(1200) + 0.5f);
    /* a loop: round the region while held, then its release */
    kit_set(k, 1, KIT_P_MODE, KIT_M_LOOP), kit_set(k, 1, KIT_P_RELEASE, 10);
    kit_note(k, 1, 0, 127, 0);
    run(k, 4000);
    CHECK(kit_voices(k) == 1 && nonzero_until(4000) > 3990);
    kit_note(k, 1, 0, 0, 0);
    run(k, 2000);
    n = nonzero_until(2000);
    CHECK(!kit_voices(k) && n >= 470 && n <= 482);      /* (10 ms at 48 kHz) */
    /* momentary: while held; the sound's end ends it too */
    kit_set(k, 2, KIT_P_MODE, KIT_M_MOMENTARY);
    kit_note(k, 2, 0, 127, 0);
    run(k, 1000);
    kit_note(k, 2, 0, 0, 0);
    run(k, 1000);
    CHECK(!kit_voices(k) && nonzero_until(1000) <= 2);
    /* the attack: a ramp; a one shot's release: the tail ending at its end; the hold: the one shot's gate */
    kit_set(k, 3, KIT_P_ATTACK, 10), kit_set(k, 3, KIT_P_RELEASE, 20);
    kit_note(k, 3, 0, 127, 0);
    run(k, 6000);
    CHECK(fabsf(B[2 * 240]) < fabsf((float)ramp(240)) * 0.6f && fabsf(B[2 * 240]) > fabsf((float)ramp(240)) * 0.4f);
    CHECK(fabsf(B[2 * 2000] - ramp(2000)) < 0.01f && fabsf(B[2 * 4320]) < fabsf((float)ramp(4320)) * 0.55f &&
          nonzero_until(6000) <= 4800);
    kit_set(k, 3, KIT_P_HOLD, 20), kit_set(k, 3, KIT_P_ATTACK, 0), kit_set(k, 3, KIT_P_RELEASE, 10);
    kit_note(k, 3, 0, 127, 0);
    run(k, 6000);
    n = nonzero_until(6000);
    CHECK(n >= 960 + 470 && n <= 960 + 485);            /* (20 ms held, 10 ms let go) */
    kit_set(k, 3, KIT_P_HOLD, 0), kit_set(k, 3, KIT_P_RELEASE, 0);
    /* a choke group: the other pad of the group stops (a short fade); another group plays on */
    kit_set(k, 0, KIT_P_CHOKE, 2), kit_set(k, 1, KIT_P_CHOKE, 2), kit_set(k, 1, KIT_P_MODE, KIT_M_ONESHOT);
    kit_set(k, 1, KIT_P_START, 0), kit_set(k, 1, KIT_P_END, KIT_FRAC);
    kit_note(k, 0, 0, 127, 0), kit_note(k, 2, 0, 127, 0);
    run(k, 1000);
    kit_note(k, 1, 0, 127, 0);
    run(k, 500);
    CHECK(kit_voices(k) == 2);                          /* (pad 1 faded out: pads 2 and 3) */
    /* a pad again: its own voice back */
    kit_note(k, 1, 0, 127, 0);
    run(k, 500);
    CHECK(kit_voices(k) == 2);
    kit_note(k, 2, 0, 0, 0);
    run(k, 5000);
    /* mute: nothing; level -6 dB and pan left; velocity */
    kit_set(k, 0, KIT_P_MUTE, 1);
    kit_note(k, 0, 0, 127, 0);
    run(k, 200);
    CHECK(!kit_voices(k) && nonzero_until(200) == 0);
    kit_set(k, 0, KIT_P_MUTE, 0), kit_set(k, 0, KIT_P_LEVEL, -6), kit_set(k, 0, KIT_P_PAN, -50), kit_set(k, 0, KIT_P_CHOKE, 0);
    kit_note(k, 0, 0, 127, 0);
    run(k, 200);
    CHECK(fabsf(B[2 * 100] - ramp(100) * 0.501f) < 2.0f && fabsf(B[2 * 100 + 1]) < 0.01f);
    run(k, 5000);
    kit_set(k, 0, KIT_P_LEVEL, 0), kit_set(k, 0, KIT_P_PAN, 0);
    kit_note(k, 0, 0, 64, 0);
    run(k, 200);
    CHECK(fabsf(B[2 * 100] - ramp(100) * 64.0f / 127.0f) < 1.0f);
    run(k, 5000);
    /* 24 voices at once: the oldest stolen */
    for (i = 0; i < 4; i++)
        kit_set(k, i, KIT_P_MODE, KIT_M_LOOP);
    for (i = 0; i < 30; i++)
        kit_note(k, i % 4, i / 4, 127, 0);
    run(k, 1000);
    CHECK(kit_voices(k) == KIT_VOICES);
    kit_free(k);
}
static void t_samples(void)
{
    char d[600], p[700];
    kit_ans_t a;
    kit_t *k;
    int i;
    snprintf(d, sizeof d, "%s/Samples", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/Drums", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/bass", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.hidden", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/zap.WAV", d), wav(p, 4800, 1, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/Clap.aif", d), aiff(p, 2400, 2, 0, 48000);
    snprintf(p, sizeof p, "%s/notes.txt", d);
    fclose(fopen(p, "w"));
    snprintf(p, sizeof p, "%s/Drums/Kick 808.wav", d), wav(p, 1200, 2, 16, 0, 48000);
    {
        kit_cfg_t c;
        memset(&c, 0, sizeof c);
        c.samples_dir = d;
        k = kit_new(&c);
    }
    kit_rows(k, KIT_PL_SAMPLES, 0, 0, 3);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_ROWS && a.place == KIT_PL_SAMPLES && a.folder == 0 && a.total == 4 && a.n == 3);
    CHECK(a.rows[0].kind == KIT_ROW_DIR && !strcmp(a.rows[0].name, "bass") && a.rows[1].kind == KIT_ROW_DIR &&
          !strcmp(a.rows[1].name, "Drums") && a.rows[2].kind == KIT_ROW_FILE && !strcmp(a.rows[2].name, "Clap") &&
          a.rows[2].frames == 2400 && a.rows[0].id && a.rows[0].id != a.rows[1].id);
    {
        int any = 0;
        for (i = 0; i < KIT_THUMB; i++)
            any |= a.rows[2].thumb[i];
        CHECK(any);
    }
    {
        const uint32_t drums = a.rows[1].id, clap = a.rows[2].id;
        kit_rows(k, KIT_PL_SAMPLES, 0, 3, 3);
        kit_work(k);
        CHECK(kit_poll(k, &a) && a.n == 1 && !strcmp(a.rows[0].name, "zap"));
        kit_rows(k, KIT_PL_SAMPLES, drums, 0, 3);
        kit_work(k);
        CHECK(kit_poll(k, &a) && a.folder == drums && a.total == 1 && !strcmp(a.rows[0].name, "Kick 808"));
        kit_load_row(k, 5, KIT_PL_SAMPLES, a.rows[0].id);
        kit_work(k);
        CHECK(kit_poll(k, &a) && a.kind == KIT_A_LOADED && a.pad == 5 && a.status == KIT_OK && a.frames == 1200 &&
              strstr(a.path, "/Drums/Kick 808.wav") && !strcmp(a.name, "Kick 808"));
        /* (2026-11-26, the owner's rule) the preview retired: nothing plays, nothing read */
        kit_preview(k, KIT_PL_SAMPLES, clap, 1);
        kit_work(k);
        CHECK(!kit_poll(k, &a) && !kit_previewing(k));
        kit_preview(k, KIT_PL_SAMPLES, clap, 0);
        kit_work(k);
        kit_poll(k, &a);
        CHECK(!kit_previewing(k));
    }
    kit_rows(k, KIT_PL_SAMPLES, 12345, 0, 3);           /* (a folder never listed: none) */
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.total == 0 && a.n == 0);
    kit_free(k);
}
/* Takes from the take library; the kit's file work on its worker (TL_Q_JOB) */
static void t_takes(void)
{
    char d[600], p[800];
    tl_t *l;
    kit_t *k;
    kit_cfg_t c;
    kit_ans_t a;
    int i, got = 0;
    snprintf(d, sizeof d, "%s/takes", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/2026-11-25", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/2026-11-25/1125-101500 Master 120bpm 2b.wav", d), wav(p, 96000, 2, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/2026-11-25/1125-101600 Input 1.5s.wav", d), wav(p, 72000, 1, 16, 0, 48000);
    l = tl_new(d, NULL);
    memset(&c, 0, sizeof c);
    c.tl = l;
    k = kit_new(&c);
    kit_rows(k, KIT_PL_TAKES, 0, 0, 3);
    for (i = 0; i < 300 && !got; i++)
        got = kit_poll(k, &a), usleep(10000);
    CHECK(got && a.kind == KIT_A_ROWS && a.place == KIT_PL_TAKES && a.total == 2 && a.n == 2);
    CHECK(!strcmp(a.rows[0].name, "I 1.5 s") && !strcmp(a.rows[1].name, "M 2 Bars") && a.rows[1].frames == 96000);
    kit_load_row(k, 3, KIT_PL_TAKES, a.rows[1].id);
    for (got = 0, i = 0; i < 300 && !got; i++)
        got = kit_poll(k, &a), usleep(10000);
    CHECK(got && a.kind == KIT_A_LOADED && a.pad == 3 && a.status == KIT_OK && a.frames == 96000 && strstr(a.path, "2b.wav") &&
          !strcmp(a.name, "M 2 Bars") && kit_frames(k, 3) == 96000);
    kit_free(k);
    tl_free(l);
}
/* the kit's own thread */
static void t_thread(void)
{
    char p[600];
    kit_cfg_t c;
    kit_ans_t a;
    kit_t *k;
    int i, got = 0, n = 0;
    memset(&c, 0, sizeof c);
    c.own_thread = 1;
    k = kit_new(&c);
    snprintf(p, sizeof p, "%s/t.wav", DIRP), wav(p, 2000, 2, 16, 0, 48000);
    for (i = 0; i < KIT_PADS; i++)
        kit_load(k, i, p);
    for (i = 0; i < 500 && got < KIT_PADS; i++) {
        while (kit_poll(k, &a))
            got++, n += a.status == KIT_OK;
        if (got < KIT_PADS)
            usleep(5000);
    }
    CHECK(got == KIT_PADS && n == KIT_PADS && kit_bytes(k) == 2000u * 4u);   /* (3c: one file, in memory once) */
    for (i = 0; i < KIT_PADS; i++)                      /* (replaced: the old sounds freed on that thread) */
        kit_load(k, i, p);
    for (got = 0, i = 0; i < 500 && got < KIT_PADS; i++) {
        while (kit_poll(k, &a))
            got++;
        if (got < KIT_PADS)
            usleep(5000);
    }
    CHECK(got == KIT_PADS);
    kit_note(k, 7, 0, 100, 0);
    run(k, 100);
    CHECK(kit_voices(k) == 1);
    kit_free(k);
}

/* (stage 3b) Stretch: the region lasts its microseconds (a voice sounding re-fitted), the right half's semitones over it */
static void t_stretch(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int len;
    snprintf(p, sizeof p, "%s/st.wav", DIRP), wav(p, 48000, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    CHECK(fabs(kit_rate_of(k, 0, 0) - 1.0) < 1e-9);
    kit_set(k, 0, KIT_P_WARP, KIT_W_REPITCH);           /* (pages v2: Sync stretches only with Warp on, Repitch) */
    kit_set(k, 0, KIT_P_STRETCH, 4), kit_stretch(k, 0, 500000);   /* (4 steps of 125 ms: half a second) */
    /* (pages v2, KIT-SPEC "Warp": Repitch with Sync on, the fit sets the rate; the right half's pitches ignored, the
     * owner's rule; they still apply with Sync off) */
    CHECK(fabs(kit_rate_of(k, 0, 0) - 2.0) < 1e-9 && fabs(kit_rate_of(k, 0, 12) - 2.0) < 1e-9);
    kit_set(k, 0, KIT_P_PITCH, 700);                    /* (Pitch ignored) */
    CHECK(fabs(kit_rate_of(k, 0, 0) - 2.0) < 1e-9);
    kit_note(k, 0, 0, 127, 0);
    run(k, 48000);
    len = nonzero_until(48000);
    CHECK(len >= 23990 && len <= 24010);
    kit_set(k, 0, KIT_P_END, KIT_FRAC / 2);             /* (half the region in the same time) */
    CHECK(fabs(kit_rate_of(k, 0, 0) - 1.0) < 1e-3);
    kit_set(k, 0, KIT_P_END, KIT_FRAC);
    kit_note(k, 0, 0, 127, 0);
    run(k, 128);
    kit_stretch(k, 0, 2000000);                         /* (the tempo halved: the voice sounding follows) */
    run(k, 48000 - 128);
    CHECK(nonzero_until(48000 - 128) >= 48000 - 129);   /* (a quarter of the sound a second: still on) */
    kit_set(k, 0, KIT_P_STRETCH, 0), kit_stretch(k, 0, 0), kit_set(k, 0, KIT_P_WARP, KIT_W_OFF);
    CHECK(fabs(kit_rate_of(k, 0, 0) - pow(2.0, 7.0 / 12.0)) < 1e-9 && fabs(kit_rate_of(k, 0, 12) - pow(2.0, 19.0 / 12.0)) < 1e-9);
    kit_free(k);
}
/* (stage 3b) the sends: a pad's own after its level and pan, the kit's strip (level, pan, sends) over all; its own
 * output (the sampler's Kit source) */
static float S1[2 * 512], S2[2 * 512], S3[2 * 512];
static int16_t OWN[2 * 512];
static void bus_run(kit_t *k, uint32_t n, int with)
{
    float *const s[3] = { S1, S2, with ? S3 : NULL };
    memset(B, 0, sizeof(float) * 2 * n), memset(S1, 0, sizeof S1), memset(S2, 0, sizeof S2), memset(S3, 0, sizeof S3);
    kit_render_bus(k, B, s, OWN, n);
}
static void t_sends(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    const int s0[3] = { 0, 0, 0 }, sk[3] = { 0, 127, 127 };
    snprintf(p, sizeof p, "%s/se.wav", DIRP), wav(p, 4800, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_set(k, 0, KIT_P_SEND1, 50);
    kit_note(k, 0, 0, 127, 0);
    bus_run(k, 512, 1);
    CHECK(fabsf(B[2 * 300] - ramp(300)) < 0.01f && fabsf(S1[2 * 300] - ramp(300) * 0.5f) < 0.01f && S2[2 * 300] == 0.0f &&
          OWN[2 * 300] == ramp(300) && OWN[2 * 300 + 1] == -ramp(300));   /* (the strip at unity at first) */
    kit_mix(k, 127, 64, sk);                            /* (the strip's sends 2 and 3: the whole kit) */
    bus_run(k, 512, 0);
    CHECK(fabsf(S2[2 * 100] - B[2 * 100]) < 0.01f && fabsf(S1[2 * 100] - B[2 * 100] * 0.5f) < 0.01f && S3[2 * 100] == 0.0f &&
          fabsf(B[2 * 100]) > 100.0f);                  /* (send 3's FX not running: nothing there) */
    kit_mix(k, 0, 64, s0);                              /* (level 0: nothing anywhere) */
    bus_run(k, 512, 1);
    CHECK(B[2 * 50] == 0.0f && S1[2 * 50] == 0.0f && OWN[2 * 50] == 0);
    kit_mix(k, 127, 0, s0);                             /* (panned hard left) */
    bus_run(k, 512, 1);
    CHECK(fabsf(B[2 * 50]) > 100.0f && B[2 * 50 + 1] == 0.0f);
    kit_free(k);
}
/* (stage 3b) kits: saved (Kit 1, Kit 2: the sounds copied, a name taken twice numbered, kit.json), listed (Files'
 * Kits), opened; one save at a time; a whole folder onto the pads (in name order, the rest emptied) */
static void t_kits(void)
{
    char d[600], kd[700], p[800], q[800];
    kit_cfg_t c;
    kit_ans_t a;
    kit_t *k;
    static kit_kit_t x;
    struct stat st;
    int i, kit1 = 0;
    snprintf(d, sizeof d, "%s/KS", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/F", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/G", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/F/sub", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/F/b 10.wav", d), wav(p, 300, 1, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/F/b 9.wav", d), wav(p, 200, 1, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/F/a.wav", d), wav(p, 100, 1, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/G/a.wav", d), wav(p, 400, 1, 16, 0, 48000);
    snprintf(kd, sizeof kd, "%s/RemoteFM1/Kits", d);
    memset(&c, 0, sizeof c);
    c.samples_dir = d, c.kits_dir = kd;
    k = kit_new(&c);
    /* saved */
    memset(&x, 0, sizeof x);
    x.layout = 1, x.mix[0] = 90, x.mix[1] = 40, x.mix[4] = 7;
    for (i = 0; i < KIT_PADS; i++)
        x.v[i][KIT_P_END] = KIT_FRAC, x.v[i][KIT_P_PITCH] = (int16_t)(i * 10 - 50);
    x.v[3][KIT_P_STRETCH] = 16, x.v[3][KIT_P_SEND2] = 60;
    snprintf(x.path[0], sizeof x.path[0], "%s/F/a.wav", d);
    snprintf(x.path[1], sizeof x.path[1], "%s/F/a.wav", d);   /* (the same file: one copy) */
    snprintf(x.path[3], sizeof x.path[3], "%s/G/a.wav", d);   /* (another a.wav: "04 a.wav") */
    snprintf(x.path[5], sizeof x.path[5], "%s/gone.wav", d);  /* (not there: the pad saved empty) */
    kit_save(k, &x);
    kit_save(k, &x);                                    /* (one at a time: refused) */
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_SAVED && a.status == KIT_FAILED);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_SAVED && a.status == KIT_OK && !strcmp(a.name, "Kit 1"));
    snprintf(p, sizeof p, "%s/Kit 1/a.wav", kd), snprintf(q, sizeof q, "%s/Kit 1/04 a.wav", kd);
    CHECK(!stat(p, &st) && st.st_size == 44 + 200 && !stat(q, &st) && st.st_size == 44 + 800);
    snprintf(p, sizeof p, "%s/Kit 1/kit.json", kd);
    CHECK(!stat(p, &st) && st.st_size > 100);
    kit_save(k, &x);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.status == KIT_OK && !strcmp(a.name, "Kit 2"));
    /* listed: Files' Kits */
    kit_rows(k, KIT_PL_KITS, 0, 0, 3);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_ROWS && a.place == KIT_PL_KITS && a.total == 2 && a.n == 2 &&
          a.rows[0].kind == KIT_ROW_KIT && !strcmp(a.rows[0].name, "Kit 1") && !strcmp(a.rows[1].name, "Kit 2"));
    kit1 = (int)a.rows[0].id;
    /* opened */
    kit_open(k, (uint32_t)kit1);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_KIT && a.status == KIT_OK && !strcmp(a.kit.name, "Kit 1") && a.kit.layout == 1 &&
          a.kit.mix[0] == 90 && a.kit.mix[1] == 40 && a.kit.mix[4] == 7);
    snprintf(p, sizeof p, "%s/Kit 1/a.wav", kd), snprintf(q, sizeof q, "%s/Kit 1/04 a.wav", kd);
    CHECK(!strcmp(a.kit.path[0], p) && !strcmp(a.kit.path[1], p) && !strcmp(a.kit.path[3], q) && !a.kit.path[5][0] &&
          !a.kit.path[2][0] && a.kit.v[3][KIT_P_STRETCH] == 16 && a.kit.v[3][KIT_P_SEND2] == 60 &&
          a.kit.v[15][KIT_P_PITCH] == 100 && a.kit.v[7][KIT_P_END] == KIT_FRAC);
    kit_open(k, 4242);                                  /* (no such kit) */
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_KIT && a.status == KIT_FAILED);
    /* a folder onto the pads: a, b 9, b 10 (names in order, numbers as numbers), the rest emptied */
    for (i = 0; i < 6; i++) {
        snprintf(p, sizeof p, "%s/G/a.wav", d);
        kit_load(k, i, p);
    }
    kit_work(k);
    while (kit_poll(k, &a)) {}
    kit_rows(k, KIT_PL_SAMPLES, 0, 0, 3);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.rows[0].kind == KIT_ROW_DIR && !strcmp(a.rows[0].name, "F"));
    kit_folder(k, KIT_PL_SAMPLES, a.rows[0].id);
    kit_work(k);
    {
        int got = 0, empty = 0, order = 1;
        while (kit_poll(k, &a)) {
            got++;
            if (a.status == KIT_EMPTY)
                empty++;
            else
                order &= (a.pad == 0 && strstr(a.path, "/F/a.wav")) || (a.pad == 1 && strstr(a.path, "/F/b 9.wav")) ||
                         (a.pad == 2 && strstr(a.path, "/F/b 10.wav"));
        }
        CHECK(got == KIT_PADS && empty == 13 && order);
    }
    CHECK(kit_frames(k, 0) == 100 && kit_frames(k, 2) == 300 && kit_frames(k, 3) == 0 && kit_frames(k, 5) == 0);
    kit_free(k);
}


/* ================================================================ (stage 3c) === */
/* a 16-bit stereo WAV of a function of the frame */
static void wavf(const char *path, uint32_t frames, uint32_t rate, int16_t (*fn)(uint32_t, int))
{
    FILE *f = fopen(path, "wb");
    const uint32_t data = frames * 4u;
    uint32_t i;
    fwrite("RIFF", 1, 4, f), w32le(f, 36 + data), fwrite("WAVEfmt ", 1, 8, f), w32le(f, 16);
    w16le(f, 1), w16le(f, 2), w32le(f, rate), w32le(f, rate * 4u), w16le(f, 4), w16le(f, 16);
    fwrite("data", 1, 4, f), w32le(f, data);
    for (i = 0; i < frames; i++)
        w16le(f, (uint16_t)fn(i, 0)), w16le(f, (uint16_t)fn(i, 1));
    fclose(f);
}
static int16_t loud(uint32_t i, int c) { (void)i; return (int16_t)(c ? -30000 : 30000); }
static int16_t quiet(uint32_t i, int c) { return (int16_t)((int)(i % 200u) * 10 - 1000) * (c ? -1 : 1); }
static int16_t ramp2(uint32_t i, int c) { return (int16_t)(c ? -(int)(i % 30000u) : (int)(i % 30000u)); }
/* 4 hits a second (at 0.1, 0.35, 0.6, 0.85 s): a decaying burst each, a quiet hum between */
static int16_t hits(uint32_t i, int c)
{
    static const uint32_t AT[4] = { 4800, 16800, 28800, 40800 };
    double v = 30.0 * sin(i * 0.01);
    int h;
    (void)c;
    for (h = 0; h < 4; h++)
        if (i >= AT[h] && i < AT[h] + 6000)
            v += 20000.0 * exp(-(double)(i - AT[h]) / 900.0) * sin((double)(i - AT[h]) * 0.3);
    return (int16_t)v;
}
static uint64_t file_sum(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t h = 1469598103934665603ull;
    int ch;
    if (!f)
        return 0;
    while ((ch = fgetc(f)) != EOF)
        h = (h ^ (uint64_t)ch) * 1099511628211ull;
    fclose(f);
    return h;
}
static int wait_ans(kit_t *k, kit_ans_t *a)   /* the next answer (the file work done here) */
{
    kit_work(k);
    return kit_poll(k, a);
}
/* the new file on disk: its frames and peak (-1: unreadable) */
static int disk(const char *path, uint32_t *frames, int *peak)
{
    kit_t *k = mk(0);
    kit_ans_t a;
    int st = load(k, 0, path, &a);
    *frames = kit_frames(k, 0), *peak = 0;
    if (st == KIT_OK) {
        uint32_t i;
        kit_set(k, 0, KIT_P_MODE, KIT_M_ONESHOT);
        kit_note(k, 0, 0, 127, 0);
        run(k, *frames < 48000 ? *frames : 48000);
        for (i = 0; i < 2 * (*frames < 48000 ? *frames : 48000); i++)
            *peak = (int)fabsf(B[i]) > *peak ? (int)fabsf(B[i]) : *peak;
    }
    kit_free(k);
    return st == KIT_OK ? 0 : -1;
}
/* 3c: mix (summed, under full scale), join (length a + b; regions; another rate), a pad's own edits (trim, normalize,
 * reverse: new files in Kits/Edits, the source untouched), a sound several pads hold in memory once */
static void t_make(void)
{
    char d[600], kd[700], pa[800], pb[800], pc[800], pq[800];
    kit_cfg_t c;
    kit_ans_t a;
    kit_t *k;
    uint32_t fr;
    int pk;
    uint64_t sum;
    snprintf(d, sizeof d, "%s/MK", DIRP), mkdir(d, 0755);
    snprintf(kd, sizeof kd, "%s/Kits", d);
    snprintf(pa, sizeof pa, "%s/a.wav", d), wavf(pa, 1000, 48000, loud);
    snprintf(pb, sizeof pb, "%s/b.wav", d), wavf(pb, 600, 48000, loud);
    snprintf(pc, sizeof pc, "%s/c.wav", d), wavf(pc, 600, 24000, ramp2);
    snprintf(pq, sizeof pq, "%s/q.wav", d), wavf(pq, 2000, 48000, quiet);
    memset(&c, 0, sizeof c);
    c.fs = 48000, c.kits_dir = kd;
    k = kit_new(&c);
    CHECK(load(k, 0, pa, &a) == KIT_OK && load(k, 1, pb, &a) == KIT_OK && load(k, 2, pc, &a) == KIT_OK &&
          load(k, 3, pq, &a) == KIT_OK);
    /* Mix: pad 1 onto pad 2, two full-scale sounds summed: under full scale, never clipped */
    kit_merge(k, 1, 0, KIT_MG_MIX);
    CHECK(wait_ans(k, &a) && a.kind == KIT_A_LOADED && a.pad == 1 && a.status == KIT_OK && a.frames == 1000 &&
          !strcmp(a.name, "Mix 1+2") && strstr(a.path, "/Kits/Edits/Mix 1+2.wav") && kit_frames(k, 1) == 1000);
    CHECK(!strcmp(kit_pad_path(k, 1), a.path));
    CHECK(!disk(a.path, &fr, &pk) && fr == 1000 && pk == KIT_NORM_PEAK && pk <= 32767);
    printf("  ok    Mix: 30000 + 30000 summed to a peak of %d (never clipped), 1000 frames, %s\n", pk, strrchr(a.path, '/') + 1);
    /* Join: pad 1 then pad 3 (24 kHz: at pad 1's rate), length a + b */
    CHECK(load(k, 1, pb, &a) == KIT_OK);
    kit_merge(k, 1, 0, KIT_MG_JOIN);
    CHECK(wait_ans(k, &a) && a.status == KIT_OK && a.frames == 1000 + 600 && !strcmp(a.name, "Join 1+2"));
    CHECK(!disk(a.path, &fr, &pk) && fr == 1600);
    kit_merge(k, 2, 0, KIT_MG_JOIN);                    /* (24 kHz: 600 frames are 1200 at 48 kHz) */
    CHECK(wait_ans(k, &a) && a.status == KIT_OK && a.frames == 1000 + 1200 && a.rate == 48000);
    kit_set(k, 0, KIT_P_START, KIT_FRAC / 2), kit_set(k, 0, KIT_P_END, KIT_FRAC);   /* (regions: 500 of pad 1) */
    CHECK(load(k, 1, pb, &a) == KIT_OK);
    kit_merge(k, 1, 0, KIT_MG_JOIN);
    CHECK(wait_ans(k, &a) && a.status == KIT_OK && a.frames == 1000u - 1000u * (KIT_FRAC / 2) / KIT_FRAC + 600u &&
          strstr(a.path, "Join 1+2 2.wav"));
    printf("  ok    Join: 1000 + 600 = 1600 frames, 1000 + 600 at 24 kHz = 2200, the second half's 501 + 600 = %u\n",
           a.frames);
    kit_set(k, 0, KIT_P_START, 0);
    /* a pad's own edits: new files, the source untouched */
    sum = file_sum(pq);
    kit_set(k, 3, KIT_P_START, KIT_FRAC / 4), kit_set(k, 3, KIT_P_END, KIT_FRAC / 2);
    kit_edit(k, 3, KIT_ED_TRIM);
    CHECK(wait_ans(k, &a) && a.pad == 3 && a.status == KIT_OK && a.frames == 1000 - 500 && !strcmp(a.name, "q trim") &&
          strstr(a.path, "/Kits/Edits/q trim.wav"));
    CHECK(file_sum(pq) == sum && !strcmp(kit_pad_path(k, 3), a.path));
    CHECK(load(k, 3, pq, &a) == KIT_OK);
    kit_set(k, 3, KIT_P_START, 0), kit_set(k, 3, KIT_P_END, KIT_FRAC);
    kit_edit(k, 3, KIT_ED_NORMALIZE);
    CHECK(wait_ans(k, &a) && a.status == KIT_OK && a.frames == 2000 && !strcmp(a.name, "q norm"));
    CHECK(!disk(a.path, &fr, &pk) && pk == KIT_NORM_PEAK && file_sum(pq) == sum);
    CHECK(load(k, 3, pc, &a) == KIT_OK);
    kit_edit(k, 3, KIT_ED_REVERSE);
    CHECK(wait_ans(k, &a) && a.status == KIT_OK && a.frames == 600 && !strcmp(a.name, "c rev"));
    {   /* reversed: its first frame the source's last */
        kit_t *z = mk(0);
        kit_ans_t b;
        CHECK(load(z, 0, a.path, &b) == KIT_OK);
        kit_set(z, 0, KIT_P_MODE, KIT_M_ONESHOT);
        kit_note(z, 0, 0, 127, 0);
        run(z, 4);
        CHECK(fabsf(B[0]) < 1.0f || fabsf(B[0] - 599.0f) < 1.5f);
        kit_free(z);
    }
    printf("  ok    Trim (500 frames), Normalize (peak %d), Reverse: new files in Kits/Edits, the source's bytes unchanged\n",
           KIT_NORM_PEAK);
    /* refused at once: an empty pad */
    CHECK(load(k, 5, "", &a) == KIT_OK);
    kit_edit(k, 5, KIT_ED_TRIM);
    CHECK(kit_poll(k, &a) && a.pad == 5 && a.status == KIT_FAILED);
    kit_merge(k, 5, 0, KIT_MG_MIX);
    CHECK(kit_poll(k, &a) && a.pad == 5 && a.status == KIT_FAILED);
    /* one file on four pads: in memory once */
    {
        int i;
        uint32_t before;
        for (i = 4; i < 8; i++)
            CHECK(load(k, i, pa, &a) == KIT_OK);
        before = kit_bytes(k);
        CHECK(load(k, 8, pa, &a) == KIT_OK && kit_bytes(k) == before);
    }
    kit_free(k);
}
/* 3c: a take's slices: the cuts by its hits, equal parts, steps; KIT_SLICE through the take library (the take's file,
 * its wave), a slice heard on the preview path (its frames only), done; the slices on pads, each its own region */
static void t_slice(void)
{
    static const uint32_t AT[4] = { 4800, 16800, 28800, 40800 };
    char d[600], p[800];
    int16_t *pcm = malloc(48000u * 4u);
    uint32_t cuts[KIT_SLICES + 1], i;
    int n, ok = 1;
    tl_t *l;
    kit_t *k;
    kit_cfg_t c;
    kit_ans_t a;
    int16_t out[2 * 1024];
    for (i = 0; i < 48000; i++)
        pcm[2 * i] = hits(i, 0), pcm[2 * i + 1] = hits(i, 1);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_HITS, 0, cuts);
    for (i = 0; n == 4 && i < 4; i++)
        ok &= (cuts[i] > AT[i] ? cuts[i] - AT[i] : AT[i] - cuts[i]) <= 240;
    CHECK(n == 4 && ok && cuts[4] == 48000);
    printf("  ok    Hits: 4 slices at %u %u %u %u (the hits at 4800 16800 28800 40800)\n", cuts[0], cuts[1], cuts[2], cuts[3]);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_EQ8, 0, cuts);
    CHECK(n == 8 && cuts[1] == 6000 && cuts[8] == 48000);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_EQ16, 0, cuts);
    CHECK(n == 16 && cuts[1] == 3000);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_EQ4, 0, cuts);
    CHECK(n == 4 && cuts[2] == 24000);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_STEPS, 125000, cuts);   /* (a 1/16 at 120 BPM: 6000 frames) */
    CHECK(n == 8 && cuts[1] == 6000 && cuts[7] == 42000 && cuts[8] == 48000);
    n = kit_cuts(pcm, 48000, 48000, KIT_SM_STEPS, 62500, cuts);    /* (16 at most) */
    CHECK(n == 16 && cuts[16] == 48000);
    printf("  ok    Equal 16 / 8 / 4 and Steps (125 ms: 8 slices of 6000 frames; 62.5 ms: 16)\n");
    memset(pcm, 0, 48000u * 4u);
    CHECK(kit_cuts(pcm, 48000, 48000, KIT_SM_HITS, 0, cuts) == 1 && cuts[0] == 0 && cuts[1] == 48000);
    free(pcm);
    /* through the take library */
    snprintf(d, sizeof d, "%s/sltakes", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/2026-11-25", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/2026-11-25/1125-120000 Master 120bpm 2b.wav", d), wavf(p, 48000, 48000, hits);
    l = tl_new(d, NULL);
    memset(&c, 0, sizeof c);
    c.fs = 48000, c.tl = l;
    k = kit_new(&c);
    {
        tl_row_t r[2];
        int tot = 0;
        uint32_t f;
        CHECK(tl_rows(l, TL_ALL, 0, 1, r, &tot, &f) == 1);
        kit_slice(k, r[0].id, KIT_SM_HITS, 0);
        for (i = 0, n = 0; i < 300 && !n; i++)
            n = kit_poll(k, &a), usleep(10000);
        CHECK(n && a.kind == KIT_A_SLICES && a.take == r[0].id && a.n == 4 && a.frames == 48000 && a.npk == KIT_PEAKS &&
              !strcmp(a.path, p));
        memcpy(cuts, a.cuts, sizeof cuts);
        kit_slice_play(k, cuts[1], 1000);               /* (2026-11-26: slice 2 a kit voice, from its frame) */
        CHECK(kit_previewing(k) && kit_voices(k) == 1 && kit_slice_pos(k) == cuts[1] && !kit_sounding(k));
        kit_slice_done(k);
        CHECK(!kit_previewing(k) && !kit_voices(k));
        kit_slice_play(k, 0, 100);
        CHECK(!kit_previewing(k));
        kit_slice(k, 0x12345, KIT_SM_HITS, 0);          /* (no such take: no slices) */
        for (i = 0, n = 0; i < 300 && !n; i++)
            n = kit_poll(k, &a), usleep(10000);
        CHECK(n && a.kind == KIT_A_SLICES && a.n == 0);
    }
    /* the slices on pads 1..4: the one file in memory once, each pad its own region */
    {
        int j;
        for (j = 0; j < 4; j++) {
            kit_load(k, j, p);
            for (i = 0, n = 0; i < 300 && !n; i++)
                n = kit_poll(k, &a), usleep(5000);
            kit_set(k, j, KIT_P_START, (int)((uint64_t)cuts[j] * KIT_FRAC / 48000));
            kit_set(k, j, KIT_P_END, (int)((uint64_t)cuts[j + 1] * KIT_FRAC / 48000));
            kit_set(k, j, KIT_P_MODE, KIT_M_ONESHOT);
        }
        CHECK(kit_bytes(k) == 48000u * 4u);
        kit_note(k, 2, 0, 127, 0);
        run(k, 64);
        {   /* (pad 3 plays from its own start: slice 3's, between two source frames as the region says) */
            const double s0 = 48000.0 * (int)((uint64_t)cuts[2] * KIT_FRAC / 48000) / KIT_FRAC + 10.0;
            const uint32_t f = (uint32_t)s0;
            const float want = (float)(hits(f, 0) + (hits(f + 1, 0) - hits(f, 0)) * (s0 - f));
            CHECK(fabsf(B[2 * 10] - want) < 1.5f);
        }
    }
    printf("  ok    KIT_SLICE: the take's 4 hits, a slice heard (its 1000 frames), the slices on pads 1..4 (in memory once)\n");
    kit_free(k);
    tl_free(l);
}

/* (round A, 2026-11-25) the Hits' sensitivity and spacing, Trim silence's lead, the close-up's peaks, a pad's audition,
 * Toggle voices, where a sound sits (KIT_FIND: Samples' folders, a take's row) */
static int16_t hits2(uint32_t i, int c)                 /* hits at 4800, 16800 (loud), 28800 (soft), 40800 + 42720 */
{
    static const uint32_t AT[5] = { 4800, 16800, 28800, 40800, 42720 };
    static const double AMP[5] = { 20000, 20000, 1500, 20000, 20000 };
    double v = 30.0 * sin(i * 0.01);
    int h;
    (void)c;
    for (h = 0; h < 5; h++)
        if (i >= AT[h] && i < AT[h] + 6000)
            v += AMP[h] * exp(-(double)(i - AT[h]) / 900.0) * sin((double)(i - AT[h]) * 0.3);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
static int16_t late(uint32_t i, int c) { return (int16_t)(i < 2400 ? (c ? 20 : -20) : (i % 2 ? 5000 : -5000)); }
static int wait_kind(kit_t *k, kit_ans_t *a, int kind)
{
    int i;
    for (i = 0; i < 400; i++) {
        if (kit_poll(k, a) && a->kind == kind)
            return 1;
        usleep(5000);
    }
    return 0;
}
static void t_rounda(void)
{
    char p[800], d[600];
    int16_t *pcm = malloc(48000u * 4u), out[2 * 1024];
    uint32_t cuts[KIT_SLICES + 1], i;
    int n, n50, n100, ok;
    kit_ans_t a;
    kit_t *k;
    for (i = 0; i < 48000; i++)
        pcm[2 * i] = hits2(i, 0), pcm[2 * i + 1] = hits2(i, 1);
    n50 = kit_cuts_ex(pcm, 48000, 48000, KIT_SM_HITS, 0, 50, KIT_HIT_GAP_MS, cuts);
    CHECK(n50 == 3 && kit_cuts(pcm, 48000, 48000, KIT_SM_HITS, 0, cuts) == n50);   /* (the soft one and the close one out) */
    n100 = kit_cuts_ex(pcm, 48000, 48000, KIT_SM_HITS, 0, 100, KIT_HIT_GAP_MS, cuts);
    for (ok = 0, i = 0; i < (uint32_t)n100; i++)
        ok |= cuts[i] >= 28800 - 240 && cuts[i] <= 28800 + 240;
    CHECK(n100 == 4 && ok);                             /* (sensitivity 100: the soft hit too) */
    n = kit_cuts_ex(pcm, 48000, 48000, KIT_SM_HITS, 0, 80, 20, cuts);
    for (ok = 0, i = 0; i < (uint32_t)n; i++)
        ok |= cuts[i] >= 42720 - 240 && cuts[i] <= 42720 + 240;
    CHECK(n == 5 && ok);                                /* (20 ms apart at least: the hit 40 ms after another) */
    n = kit_cuts_ex(pcm, 48000, 48000, KIT_SM_HITS, 0, 80, KIT_HIT_GAP_MS, cuts);
    for (ok = 0, i = 0; i < (uint32_t)n; i++)
        ok |= cuts[i] >= 42720 - 240 && cuts[i] <= 42720 + 240;
    CHECK(n == 4 && !ok);                               /* (60 ms: not) */
    n = kit_cuts_ex(pcm, 48000, 48000, KIT_SM_HITS, 0, 1, KIT_HIT_GAP_MS, cuts);
    CHECK(n >= 1 && n <= n50);
    printf("  ok    Hits: sensitivity 50 %d hits, 100 %d (the soft one), sensitivity 80 with spacing 20 ms the close one (60 ms: not)\n", n50, n100);
    /* Trim silence: the lead */
    for (i = 0; i < 48000; i++)
        pcm[2 * i] = late(i, 0), pcm[2 * i + 1] = late(i, 1);
    CHECK(kit_lead(pcm, 48000, 48000) == 2400 - 240 && kit_lead(pcm + 2 * 2300, 1000, 48000) == 0);
    memset(pcm, 0, 48000u * 4u);
    CHECK(kit_lead(pcm, 48000, 48000) == 0);
    free(pcm);
    k = mk(0);
    snprintf(p, sizeof p, "%s/late.wav", DIRP), wavf(p, 9600, 48000, late);
    kit_load_rec(k, 2, p, 1);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_LOADED && a.status == KIT_OK && a.lead == 2160 && kit_frames(k, 2) == 9600);
    kit_load(k, 2, p);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.lead == -1);             /* (not a take recorded into it: not asked) */
    printf("  ok    Trim silence: a take recorded into a pad, its first sound at 2400: the lead 2160 (5 ms before)\n");
    /* the close-up's peaks: a window of the pad's sound, scaled as its whole */
    snprintf(p, sizeof p, "%s/zoom.wav", DIRP), wav(p, 4800, 2, 16, 0, 48000);
    load(k, 0, p, &a);
    kit_zoom(k, 0, 1000, 640, 64);
    kit_zoom(k, 0, 1000, 640, 64);                      /* (the newer replaces one not started) */
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_PEAKS && a.pad == 0 && a.zfrom == 1000 && a.zcount == 640 && a.npk == 64);
    for (ok = 1, n = 0; n < 64; n++) {
        int pk = 0, f;
        for (f = 1000 + n * 10; f < 1010 + n * 10; f++)
            pk = abs(ramp((uint32_t)f)) > pk ? abs(ramp((uint32_t)f)) : pk;
        ok &= a.pk[n] == pk * 255 / 3000;
    }
    CHECK(ok && !kit_poll(k, &a));
    kit_zoom(k, 5, 0, 100, 64);                         /* (no sound on pad 6: nothing asked) */
    kit_work(k);
    CHECK(!kit_poll(k, &a));
    kit_audition(k, 0, 100, 500);                       /* (2026-11-26: retired) */
    CHECK(!kit_auditioning(k) && !kit_voices(k));
    printf("  ok    the close-up's peaks (a window, scaled as the whole), no audition\n");
    /* (pages v2: Toggle is gone; its test with it) */
    /* where a sound sits: Samples' folders and rows */
    snprintf(d, sizeof d, "%s/fd", DIRP), mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/fd/sub", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/fd/sub/a.wav", DIRP), wav(p, 100, 1, 16, 0, 48000);
    snprintf(p, sizeof p, "%s/fd/sub/b.wav", DIRP), wav(p, 100, 1, 16, 0, 48000);
    kit_find(k, 1, p);
    kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_FOUND && a.pad == 1 && a.place == KIT_PL_SAMPLES && a.depth == 2 &&
          a.sels[2] == 1 && a.take);
    {
        kit_ans_t r;
        const uint32_t f1 = a.folders[1], f2 = a.folders[2], id = a.take;
        const int s0 = a.sels[0], s1 = a.sels[1];
        kit_rows(k, KIT_PL_SAMPLES, 0, s0, 1), kit_work(k);
        CHECK(kit_poll(k, &r) && r.n == 1 && r.rows[0].kind == KIT_ROW_DIR && !strcmp(r.rows[0].name, "fd") && r.rows[0].id == f1);
        kit_rows(k, KIT_PL_SAMPLES, f1, s1, 1), kit_work(k);
        CHECK(kit_poll(k, &r) && r.n == 1 && !strcmp(r.rows[0].name, "sub") && r.rows[0].id == f2);
        kit_rows(k, KIT_PL_SAMPLES, f2, 1, 1), kit_work(k);
        CHECK(kit_poll(k, &r) && r.n == 1 && !strcmp(r.rows[0].name, "b") && r.rows[0].id == id);
    }
    kit_find(k, 1, "/nowhere/x.wav"), kit_work(k);
    CHECK(kit_poll(k, &a) && a.kind == KIT_A_FOUND && a.place == 0);
    kit_free(k);
    /* .. a take: its row in Takes (the newest first) */
    {
        tl_t *l;
        kit_cfg_t c;
        snprintf(d, sizeof d, "%s/ftakes", DIRP), mkdir(d, 0755);
        snprintf(p, sizeof p, "%s/2026-11-25", d), mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/2026-11-25/1125-101700 Master 120bpm 1b.wav", d), wav(p, 4800, 2, 16, 0, 48000);
        snprintf(p, sizeof p, "%s/2026-11-25/1125-101500 Master 120bpm 2b.wav", d), wav(p, 4800, 2, 16, 0, 48000);
        l = tl_new(d, NULL);
        memset(&c, 0, sizeof c);
        c.fs = 48000, c.tl = l;
        k = kit_new(&c);
        kit_find(k, 4, p);
        CHECK(wait_kind(k, &a, KIT_A_FOUND) && a.pad == 4 && a.place == 0);   /* (2026-11-26: no Takes place; a take is
                                                         * found in My Samples, here none) */
        kit_free(k);
        tl_free(l);
    }
    printf("  ok    KIT_FIND: Samples/fd/sub/b.wav (its folders, their rows), a take outside My Samples (nowhere), nowhere\n");
}

/* (2026-11-26) the volume knob's MASTER on the kit's whole output (an audio taper: the square), 0 silent; and a step
 * lock on a setting outside the nine LK locks (KIT_NOTE's kp_mask / kp[]: here End) on that hit's voice only */
static float peak_of(uint32_t n)
{
    float pk = 0;
    uint32_t i;
    for (i = 0; i < 2 * n; i++)
        pk = fabsf(B[i]) > pk ? fabsf(B[i]) : pk;
    return pk;
}
static void t_master_plock(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int16_t pv[KIT_P_N];
    float full, half;
    snprintf(p, sizeof p, "%s/mp.wav", DIRP), wav(p, 4800, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_note(k, 0, 0, 127, 0), run(k, 5000), full = peak_of(5000);
    kit_master(k, 511);
    kit_note(k, 0, 0, 127, 0), run(k, 5000), half = peak_of(5000);
    CHECK(full > 2900 && fabsf(half / full - (511.0f / 1023.0f) * (511.0f / 1023.0f)) < 0.01f);
    kit_master(k, 0);
    kit_note(k, 0, 0, 127, 0), run(k, 5000);
    CHECK(peak_of(5000) == 0.0f);
    kit_master(k, 1023);
    kit_note(k, 0, 0, 127, 0), run(k, 5000);
    CHECK(fabsf(peak_of(5000) - full) < 0.5f);
    memset(pv, 0, sizeof pv);
    pv[KIT_P_END] = KIT_FRAC / 4;                       /* (a lock on End: a quarter of the sound, this hit only) */
    kit_note_pl(k, 0, 0, 127, 0, 0, NULL, 1ull << KIT_P_END, pv, 0);
    run(k, 5000);
    CHECK(nonzero_until(5000) >= 1150 && nonzero_until(5000) <= 1250);
    kit_note(k, 0, 0, 127, 0), run(k, 5000);
    CHECK(nonzero_until(5000) > 4700);                  /* (the next hit: the pad's own End, untouched) */
    kit_free(k);
}
/* (2026-11-26) a ghost trigger (rfm1cs KIT_GHOST): its locks onto the pad's sounding voice (Level -inf: silent; -6 dB:
 * half), a note-on-only setting ignored, the next hit its own */
static void t_ghost(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int16_t pv[KIT_P_N];
    float full, pk;
    snprintf(p, sizeof p, "%s/gh.wav", DIRP), wav(p, 40000, 2, 16, 0, 48000);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_note(k, 0, 0, 127, 0), run(k, 4000), full = peak_of(4000);
    memset(pv, 0, sizeof pv);
    pv[KIT_P_LEVEL] = -6;
    kit_ghost(k, 0, 0, 1ull << KIT_P_LEVEL, pv), run(k, 4000), pk = peak_of(4000);
    CHECK(full > 2900 && fabsf(pk / full - 0.501f) < 0.02f && kit_voices(k) == 1);
    pv[KIT_P_START] = KIT_FRAC / 2;
    kit_ghost(k, 0, 0, 1ull << KIT_P_START, pv), run(k, 4000);   /* (Start: a note-on's, not the voice's) */
    CHECK(fabsf(peak_of(4000) - pk) < 1.0f);
    pv[KIT_P_LEVEL] = -60;
    kit_ghost(k, 1, 0, 1ull << KIT_P_LEVEL, pv), run(k, 4000);   /* (another pad's: not this voice) */
    CHECK(fabsf(peak_of(4000) - pk) < 1.0f);
    kit_ghost(k, 0, 0, 1ull << KIT_P_LEVEL, pv), run(k, 4000);
    CHECK(peak_of(4000) == 0.0f);
    kit_note(k, 0, 0, 127, 0), run(k, 4000);           /* (the next hit: the pad's own Level) */
    CHECK(fabsf(peak_of(4000) - full) < 1.0f);
    kit_free(k);
    printf("  ok    a ghost trigger: Level -6 dB halves the sounding voice, -inf silences it, Start ignored, the next hit its own\n");
}
/* (2026-11-26) FLAC (tests/flac/, made by `flac` from ramp WAVs): loaded sample for sample (16-bit stereo, 24-bit mono
 * without a seektable), a row's picture, too long refused; the Move Library's index: nested categories, its root
 * (Samples/ without sounds: the whole; a lone wrapper skipped), "Reading..." until done, the unsupported rows, the
 * status */
static void cp_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[4096];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    if (a)
        fclose(a);
    if (b)
        fclose(b);
}
static void touch(const char *p) { FILE *f = fopen(p, "w"); if (f) fputs("x", f), fclose(f); }
static int core_rows(kit_t *k, uint32_t folder, kit_ans_t *a, int *reading)   /* the rows, past "Reading..." */
{
    int i;
    kit_rows(k, KIT_PL_CORE, folder, 0, 8);
    for (i = 0; i < 400; i++) {
        kit_work(k);
        if (kit_poll(k, a) && a->kind == KIT_A_ROWS) {
            if (!(a->n == 1 && a->rows[0].kind == KIT_ROW_NOTE && !strcmp(a->rows[0].name, "Reading...")))
                return 1;
            ++*reading;                                 /* (asked again by the index when done: not by us) */
        }
        usleep(5000);
    }
    return 0;
}
static int row_named(const kit_ans_t *a, const char *name, int kind)
{
    int i;
    for (i = 0; i < a->n; i++)
        if (!strcmp(a->rows[i].name, name) && a->rows[i].kind == kind)
            return i;
    return -1;
}
/* (2026-11-26) LoFi's Bits in tenths: 24.0 transparent, 4.0 and 4.5 bits quantised to 2^12 and 2^11.5 of the int16
 * scale */
static int on_grid(uint32_t n, double q)
{
    uint32_t i;
    for (i = 0; i < 2 * n; i++) {
        const double x = B[i] / q;
        if (fabs(x - floor(x + 0.5)) > 1e-3)
            return 0;
    }
    return 1;
}
static void t_lofi(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int i, same = 1, any = 0;
    snprintf(p, sizeof p, "%s/lofi.wav", DIRP), wavf(p, 4000, 48000, ramp2);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_set(k, 0, KIT_P_MODE, KIT_M_LOFI), kit_set(k, 0, KIT_P_BITS, 240);
    kit_note(k, 0, 0, 127, 0), run(k, 2000);
    for (i = 0; i < 2000; i++)
        same &= fabsf(B[2 * i] - ramp2((uint32_t)i, 0)) < 1.0f;
    CHECK(same);
    run(k, 4000);
    kit_set(k, 0, KIT_P_BITS, 40), kit_note(k, 0, 0, 127, 0), run(k, 2000);
    CHECK(on_grid(2000, 4096.0));
    run(k, 4000);
    kit_set(k, 0, KIT_P_BITS, 45), kit_note(k, 0, 0, 127, 0), run(k, 2000);
    CHECK(on_grid(2000, exp2(11.5)) && !on_grid(2000, 4096.0));
    for (i = 0; i < 2000; i++)
        any |= B[2 * i] != 0.0f;
    CHECK(any);
    kit_free(k);
    printf("  ok    LoFi Bits in tenths: 24.0 transparent, 4.0 bits on a 4096 grid, 4.5 bits on 2^11.5\n");
}
/* (2026-11-26, the owner's sustain loop) Loop: from Start to Loop End, round Loop End -> Loop Start while held (two
 * rounds), let go: on past Loop End to the region's End (a long release), then silent; reversed: the mirror */
static int16_t idx(uint32_t i, int c) { (void)c; return (int16_t)(i + 1); }
static void t_sustain_loop(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int i, top = 0, past = 0, lo = 99999, ok = 1;
    snprintf(p, sizeof p, "%s/sl.wav", DIRP), wavf(p, 4000, 48000, idx);   /* (frame i: i + 1) */
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_set(k, 0, KIT_P_MODE, KIT_M_LOOP), kit_set(k, 0, KIT_P_RELEASE, 2000);
    kit_set(k, 0, KIT_P_LOOPS, KIT_FRAC / 4), kit_set(k, 0, KIT_P_LOOPE, KIT_FRAC / 2);   /* (about 1000 .. 2000) */
    kit_note(k, 0, 0, 127, 0), run(k, 4000);            /* (held: 2000 to the loop's end, then two rounds) */
    for (i = 0; i < 4000; i++)
        top = B[2 * i] > top ? (int)B[2 * i] : top;
    for (i = 2100; i < 4000; i++)
        lo = B[2 * i] < lo ? (int)B[2 * i] : lo;
    CHECKF(top <= 2002 && lo >= 990 && lo <= 1010 && kit_voices(k) == 1, "held: top %d, low after the first round %d", top, lo);
    kit_note(k, 0, 0, 0, 0), run(k, 4000);              /* (let go: on to the end, 4000) */
    for (i = 0; i < 4000; i++)
        past = B[2 * i] > 2100 ? i : past;
    for (i = 1; i < 4000 && ok; i++)                    /* (no jump back: rising until it ends) */
        ok = B[2 * i] == 0.0f || B[2 * i] >= B[2 * (i - 1)] - 2.0f || B[2 * (i - 1)] == 0.0f;
    CHECKF(past > 1000 && ok && !kit_voices(k) && nonzero_until(4000) < 3100, "let go: past the loop until %d, rising %d, voices %d",
           past, ok, kit_voices(k));
    kit_set(k, 0, KIT_P_REVERSE, 1);                    /* (reversed: from the end down to Loop Start, round, then on
                                                         * down to the start) */
    kit_note(k, 0, 0, 127, 0), run(k, 4000);
    lo = 99999, top = 0;
    for (i = 0; i < 4000; i++)
        lo = B[2 * i] < lo && B[2 * i] > 0 ? (int)B[2 * i] : lo;
    for (i = 2100; i < 4000; i++)
        top = B[2 * i] > top ? (int)B[2 * i] : top;
    CHECKF(lo >= 995 && top >= 1990 && top <= 2010, "reversed held: low %d, top after a round %d", lo, top);
    kit_note(k, 0, 0, 0, 0), run(k, 4000);
    for (i = 0, lo = 99999; i < 4000; i++)
        lo = B[2 * i] < lo && B[2 * i] > 0 ? (int)B[2 * i] : lo;
    CHECKF(lo < 900, "reversed let go: down to %d", lo);
    kit_free(k);
    printf("  ok    Loop: Start to Loop End, round while held, let go: on through Loop End to End (and reversed)\n");
}
/* (2026-11-26, the owner's) Granular's Scan (Position 20 %: its first 100 ms read 20..30 %): +100 moves the grains' position forward through the region (one pass a
 * second) and wraps to its start; -100 backwards, wrapping to its end. The sound's frame i is i / 2: the loudest of
 * each 100 ms is where the grains read */
static int16_t half(uint32_t i, int c) { (void)c; return (int16_t)(i / 2); }
static void scan_peaks(kit_t *k, int scan, int *pk)
{
    int b, i;
    kit_set(k, 0, KIT_P_GSCAN, scan);
    kit_note(k, 0, 0, 127, 0);
    for (b = 0; b < 15; b++) {                          /* (1.5 s in 100 ms blocks) */
        run(k, 4800);
        pk[b] = 0;
        for (i = 0; i < 4800; i++)
            pk[b] = B[2 * i] > pk[b] ? (int)B[2 * i] : pk[b];
    }
    kit_note(k, 0, 0, 0, 0), run(k, 48000);
}
static void t_scan(void)
{
    char p[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int f[15], r[15], b, up = 0, down = 0, wrapf = -1, wrapr = -1;
    snprintf(p, sizeof p, "%s/scan.wav", DIRP), wavf(p, 48000, 48000, half);
    CHECK(load(k, 0, p, &a) == KIT_OK);
    kit_set(k, 0, KIT_P_MODE, KIT_M_GRANULAR), kit_set(k, 0, KIT_P_GSIZE, 10), kit_set(k, 0, KIT_P_GDENS, 1);
    kit_set(k, 0, KIT_P_GSPRAY, 0), kit_set(k, 0, KIT_P_GSPREAD, 0), kit_set(k, 0, KIT_P_GPOS, 20);
    kit_set(k, 0, KIT_P_RELEASE, 0);
    scan_peaks(k, 100, f);
    scan_peaks(k, -100, r);
    for (b = 1; b < 15; b++) {
        up += f[b] > f[b - 1], down += r[b] < r[b - 1];
        if (f[b] < f[b - 1] - 10000 && wrapf < 0)
            wrapf = b;
        if (r[b] > r[b - 1] + 10000 && wrapr < 0)
            wrapr = b;
    }
    CHECKF(up >= 12 && wrapf >= 7 && wrapf <= 9 && f[0] > 4800 && f[0] < 7400, "forward: %d rising, wrap at block %d, first %d",
           up, wrapf, f[0]);
    CHECKF(down >= 12 && wrapr >= 1 && wrapr <= 3, "backward: %d falling, wrap at block %d", down, wrapr);
    kit_free(k);
    printf("  ok    Granular Scan: +100 forward a pass a second, wrapping at the end; -100 backward, wrapping at the start\n");
}
static void t_flac(void)
{
    char p[700], d[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int ok, i, rd = 0;
    CHECK(load(k, 0, "tests/flac/ramp-s16.flac", &a) == KIT_OK && a.frames == 1000 && a.rate == 48000 &&
          !strcmp(a.name, "ramp-s16") && a.npk == KIT_PEAKS);
    CHECK(load(k, 1, "tests/flac/ramp-m24.flac", &a) == KIT_OK && a.frames == 500 && a.rate == 48000);
    kit_set(k, 0, KIT_P_MODE, KIT_M_ONESHOT), kit_set(k, 1, KIT_P_MODE, KIT_M_ONESHOT);
    kit_note(k, 0, 0, 127, 0), run(k, 400);
    for (i = 0, ok = 1; i < 300; i++)                   /* (lossless: every frame as written) */
        ok &= fabsf(B[2 * i] - ramp((uint32_t)i)) < 1.0f && fabsf(B[2 * i + 1] + ramp((uint32_t)i)) < 1.0f;
    CHECK(ok);
    run(k, 2000);
    kit_note(k, 1, 0, 127, 0), run(k, 400);
    for (i = 0, ok = 1; i < 300; i++)
        ok &= fabsf(B[2 * i] - ramp((uint32_t)i)) < 1.0f && fabsf(B[2 * i + 1] - ramp((uint32_t)i)) < 1.0f;
    CHECK(ok);
    kit_free(k);
    k = mk(999 * 4);                                    /* (1000 frames past it: Too Long, from the header) */
    CHECK(load(k, 0, "tests/flac/ramp-s16.flac", &a) == KIT_TOO_LONG && !kit_frames(k, 0));
    kit_free(k);
    /* My Samples: a FLAC listed, its picture */
    snprintf(d, sizeof d, "%s/FlacSamples", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/Ramp.flac", d), cp_file("tests/flac/ramp-s16.flac", p);
    {
        kit_cfg_t c;
        int any = 0;
        memset(&c, 0, sizeof c);
        c.samples_dir = d;
        k = kit_new(&c);
        kit_rows(k, KIT_PL_SAMPLES, 0, 0, 8);
        kit_work(k);
        CHECK(kit_poll(k, &a) && a.total == 1 && a.rows[0].kind == KIT_ROW_FILE && !strcmp(a.rows[0].name, "Ramp") &&
              a.rows[0].frames == 1000);
        for (i = 0; i < KIT_THUMB; i++)
            any |= a.rows[0].thumb[i];
        CHECK(any);
        kit_free(k);
    }
    /* the Move Library: Samples/ has an .ogg only, the sounds in Packs/<category>/<kind>/ */
    snprintf(d, sizeof d, "%s/Core", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/Samples", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Samples/Pad.ogg", d), touch(p);
    snprintf(p, sizeof p, "%s/Presets", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Presets/Kit.ablpreset", d), touch(p);
    snprintf(p, sizeof p, "%s/Packs", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Packs/Keys", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Packs/Keys/Bells", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Packs/Keys/Bells/Glass.flac", d), cp_file("tests/flac/ramp-s16.flac", p);
    snprintf(p, sizeof p, "%s/Packs/Keys/Bells/Tin.mp3", d), touch(p);
    snprintf(p, sizeof p, "%s/Packs/Drums", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Packs/Drums/Kicks", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Packs/Drums/Kicks/Deep.wav", d), wav(p, 800, 2, 16, 0, 48000);
    {
        kit_cfg_t c;
        char js[1024];
        uint32_t keys, bells, samples;
        memset(&c, 0, sizeof c);
        c.core_dir = d;
        k = kit_new(&c);
        CHECK(kit_has_core(k));
        CHECK(core_rows(k, 0, &a, &rd) && a.total == 2 && row_named(&a, "Packs", KIT_ROW_DIR) == 0 &&
              row_named(&a, "Samples", KIT_ROW_DIR) == 1);   /* (Samples/ has no sound read: the whole, Presets hidden) */
        samples = a.rows[1].id;
        CHECK(core_rows(k, a.rows[0].id, &a, &rd) && a.total == 2 && row_named(&a, "Drums", KIT_ROW_DIR) == 0 &&
              row_named(&a, "Keys", KIT_ROW_DIR) == 1);
        keys = a.rows[1].id;
        CHECK(core_rows(k, keys, &a, &rd) && a.total == 1 && row_named(&a, "Bells", KIT_ROW_DIR) == 0);
        bells = a.rows[0].id;
        CHECK(core_rows(k, bells, &a, &rd) && a.total == 2 && row_named(&a, "Glass", KIT_ROW_FILE) == 0 &&
              row_named(&a, "1 .mp3 (unsupported)", KIT_ROW_NOTE) == 1 && !a.rows[1].id && a.rows[0].frames == 1000);
        kit_load_row(k, 2, KIT_PL_CORE, a.rows[0].id);
        kit_work(k);
        CHECK(kit_poll(k, &a) && a.kind == KIT_A_LOADED && a.status == KIT_OK && a.frames == 1000 &&
              strstr(a.path, "/Packs/Keys/Bells/Glass.flac"));
        CHECK(core_rows(k, samples, &a, &rd) && a.total == 1 && row_named(&a, "1 .ogg (unsupported)", KIT_ROW_NOTE) == 0);
        kit_corelib_json(k, js, sizeof js);
        CHECK(strstr(js, "\"cl_state\":\"ready\"") && strstr(js, "\"cl_sounds\":2") && strstr(js, "\"cl_unsupported\":2") &&
              strstr(js, "\"flac\":1") && strstr(js, "\"wav\":1") && strstr(js, "\"ablpreset\":1") &&
              strstr(js, "\"ogg\":1") && strstr(js, "\"cl_skipped\":3"));
        if (!strstr(js, "\"cl_sounds\":2"))
            fprintf(stderr, "corelib: %s\n", js);
        kit_free(k);
    }
    /* a lone wrapper folder (Library/) skipped: the categories at the top; nothing at all: "No sounds found" */
    snprintf(d, sizeof d, "%s/Core2", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/Library", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Keys", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Keys/Bells", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Keys/Bells/Glass.flac", d), cp_file("tests/flac/ramp-m24.flac", p);
    snprintf(p, sizeof p, "%s/Library/Drums", d), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Library/Drums/Hit.aif", d), aiff(p, 300, 1, 0, 48000);
    {
        kit_cfg_t c;
        memset(&c, 0, sizeof c);
        c.core_dir = d;
        k = kit_new(&c);
        CHECK(core_rows(k, 0, &a, &rd) && a.total == 2 && row_named(&a, "Drums", KIT_ROW_DIR) == 0 &&
              row_named(&a, "Keys", KIT_ROW_DIR) == 1);
        kit_free(k);
    }
    snprintf(d, sizeof d, "%s/Core3", DIRP), mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/notes.txt", d), touch(p);
    {
        kit_cfg_t c;
        memset(&c, 0, sizeof c);
        c.core_dir = d;
        k = kit_new(&c);
        CHECK(core_rows(k, 0, &a, &rd) && a.total == 1 && row_named(&a, "No sounds found", KIT_ROW_NOTE) == 0);
        kit_free(k);
    }
    printf("  ok    FLAC: 16-bit stereo and 24-bit mono sample for sample, a row's picture, Too Long; the Move Library's "
           "categories nested, its root, the unsupported rows, the status (\"Reading...\" first %d times)\n", rd);
}
/* (the owner's 2026-11-26: "The kit should be polyphonic between pads, unless a pad is set to a choke group") two
 * pads' notes in the same block, at the same frame and apart, on a track's pads (1, 2) and across tracks (1, 5): every
 * pad sounds at once; only pads sharing a non-zero choke group cut each other */
static int live(const kit_t *k, int pad)
{
    int c, f, fs, a, as;
    return kit_live(k, pad, &c, &f, &fs, &a, &as);
}
static void t_poly(void)
{
    char p[600], q[600];
    kit_ans_t a;
    kit_t *k = mk(0);
    int16_t lk[KIT_LK_N], pv[KIT_P_N];
    int i;
    snprintf(p, sizeof p, "%s/pa.wav", DIRP), wav(p, 24000, 2, 16, 0, 48000);
    snprintf(q, sizeof q, "%s/pb.wav", DIRP), wav(q, 24000, 1, 16, 0, 44100);
    CHECK(load(k, 0, p, &a) == KIT_OK && load(k, 1, q, &a) == KIT_OK && load(k, 4, p, &a) == KIT_OK);
    memset(lk, 0, sizeof lk), memset(pv, 0, sizeof pv);
    for (i = 0; i < 3; i++) {                           /* (as the core sends a sequencer's hit: locks given, none set) */
        kit_note_pl(k, 0, 0, 100, 40, 0, lk, 0, pv, 0);
        kit_note_pl(k, 1, 0, 100, 40, 0, lk, 0, pv, 0);
        kit_note_pl(k, 4, 0, 100, i == 2 ? 10 : 40, 0, lk, 0, pv, 0);
        run(k, 600);                                    /* (a pad again: its own voice's fade, 5 ms) */
        CHECKF(live(k, 0) && live(k, 1) && live(k, 4) && kit_voices(k) == 3, "round %d: %d %d %d, %d voices", i,
               live(k, 0), live(k, 1), live(k, 4), kit_voices(k));
        kit_note(k, 0, 0, 0, 0), kit_note(k, 1, 0, 0, 0), kit_note(k, 4, 0, 0, 0);   /* (the steps' ends: one shots
                                                         * play on) */
        run(k, 1200);
        CHECK(live(k, 0) && live(k, 1) && live(k, 4));
    }
    run(k, 24000);
    {                                                   /* the held engines (Loop, Momentary, Granular), a hit's locks
                                                         * and a ghost beside another pad's hit: each pad its own */
        static const int M3[3] = { KIT_M_LOOP, KIT_M_MOMENTARY, KIT_M_GRANULAR };
        int j;
        for (j = 0; j < 3; j++) {
            kit_set(k, 0, KIT_P_MODE, M3[j]), kit_set(k, 1, KIT_P_MODE, M3[(j + 1) % 3]);
            pv[KIT_P_LEVEL] = -12, pv[KIT_P_SEND1] = 50;
            kit_note_pl(k, 0, 0, 100, 40, 0, lk, 1ull << KIT_P_LEVEL | 1ull << KIT_P_SEND1, pv, 0);
            kit_note_pl(k, 1, 0, 100, 40, 0, lk, 0, pv, 0);
            kit_ghost(k, 4, 40, 1ull << KIT_P_LEVEL, pv);
            run(k, 2400);
            CHECKF(live(k, 0) && live(k, 1) && kit_voices(k) == 2, "engines %d: %d %d, %d voices", j, live(k, 0),
                   live(k, 1), kit_voices(k));
            kit_note(k, 0, 0, 0, 0), kit_note(k, 1, 0, 0, 0);
            run(k, 24000);
        }
        kit_set(k, 0, KIT_P_MODE, KIT_M_ONESHOT), kit_set(k, 1, KIT_P_MODE, KIT_M_ONESHOT);
        pv[KIT_P_CHOKE] = 0;
    }
    kit_set(k, 0, KIT_P_CHOKE, 1), kit_set(k, 1, KIT_P_CHOKE, 1);   /* (a group: the later of the two cuts the other) */
    kit_note(k, 0, 0, 100, 10), kit_note(k, 1, 0, 100, 20), kit_note(k, 4, 0, 100, 30);
    run(k, 1000);
    CHECK(!live(k, 0) && live(k, 1) && live(k, 4));
    kit_free(k);
}
int main(void)
{
    const char *d = getenv("KIT_TEST_DIR");
    snprintf(DIRP, sizeof DIRP, "%s/kit-%d", d && *d ? d : "/tmp", (int)getpid());
    mkdir(DIRP, 0755);
    t_files();
    t_too_long();
    t_voices();
    t_samples();
    t_takes();
    t_thread();
    t_stretch();
    t_sends();
    t_kits();
    t_make();
    t_slice();
    t_rounda();
    t_master_plock();
    t_ghost();
    t_flac();
    t_lofi();
    t_sustain_loop();
    t_scan();
    t_poly();
    printf("kit_test: %d checks, %d failed\n", checks, fails);
    if (fails)
        return 1;
    printf("ALL KIT TESTS PASSED\n");
    return 0;
}
