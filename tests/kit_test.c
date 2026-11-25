/* SPDX-License-Identifier: MPL-2.0 */
/* The kit (src/kit.c) natively: the sound files (WAV 16 / 24-bit and float, mono and stereo, AIFF and AIFC sowt, the
 * rate kept), a sound past the kit's memory refused (the pad keeps the one it had), a missing file, the voices (a one
 * shot sample for sample, a note's frame, reverse, an octave up, start / end, a loop while held, momentary's release,
 * the attack, the one shot's tail ending at its end, the hold's gate, a choke group, a pad again, mute, level and pan,
 * velocity, 24 voices and the oldest stolen), Samples' rows (folders first, by name, the pictures, a folder entered),
 * Takes' rows from the take library and a take loaded, the preview (after the tap: kit_preview_mix), the file work on
 * the take library's worker (TL_Q_JOB) and on the kit's own thread. Ends with ALL KIT TESTS PASSED.
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
        /* the preview: heard after the tap, once; a newer one replaces it; off stops it */
        kit_preview(k, KIT_PL_SAMPLES, clap, 1);
        kit_work(k);
        CHECK(!kit_poll(k, &a) && kit_previewing(k));
        {
            int16_t out[2 * 3000];
            memset(out, 0, sizeof out);
            kit_preview_mix(k, out, 3000);
            CHECK(out[2 * 100] == ramp(100) && out[2 * 100 + 1] == -ramp(100) && !kit_previewing(k) && out[2 * 2500] == 0);
        }
        kit_preview(k, KIT_PL_SAMPLES, clap, 1);
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
    CHECK(got == KIT_PADS && n == KIT_PADS && kit_bytes(k) == KIT_PADS * 2000u * 4u);
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
    printf("kit_test: %d checks, %d failed\n", checks, fails);
    if (fails)
        return 1;
    printf("ALL KIT TESTS PASSED\n");
    return 0;
}
