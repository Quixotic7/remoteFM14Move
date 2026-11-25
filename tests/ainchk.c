/* SPDX-License-Identifier: MPL-2.0 */
/* ainchk: the audio input tests' helper (ain1: tests/ain_e2e.sh, tests/fantasy_e2e.sh).
 *   ainchk make OUT.wav        a 2 s 44.1 kHz stereo input: 330 Hz on the left, 550 Hz on the right (-12 dB), a click
 *                              (both, near full scale) every 0.5 s
 *   ainchk find TAKE.wav IN.wav   TAKE is a stretch of IN played looped, sample for sample: prints where it starts in
 *                              IN and how long it is; exit 0, else 1 (with the first frame that differs)
 *   ainchk same A.wav B.wav    the same frames (a WAV's data against another's); exit 0, else 1
 *   ainchk gaps TAKE.wav IN.wav   as find, but frames of silence in TAKE where IN has sound are allowed (the input
 *                              path's under-runs: a block of silence in place of one): prints where it starts and the
 *                              silent frames; exit 0, else 1
 *   ainchk info F.wav          "frames N peak P" (the recorder's takes: tests/rec_e2e.sh); exit 1 when unreadable
 *   ainchk trimmed TAKE.wav ORIG.wav FROM   TAKE is ORIG's frames from FROM on, sample for sample, the take library's
 *                              132 frames faded at a cut end (tests/takes_e2e.sh); exit 0, else 1
 *   ainchk reversed A.wav B.wav   A is B's frames in the reverse order; exit 0, else 1
 *   ainchk rawpeak F.raw       a capture's (--capture: stereo int16, no header) "frames N peak P loud L"
 *   ainchk listen OUT.raw MIX.raw   Listen here (tests/listen_e2e.sh): what went to the Move against the mix, frame by
 *                              frame (the same frames, in order): "frames N same S silenced Z ramp R other O run F
 *                              after A": the Move's frame the mix's, zero where the mix sounds, between (the ramps), none
 *                              of these; the longest silenced run (frames, quiet mix frames inside it allowed), the
 *                              frames the same and sounding after that run */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/wav.h"

static void le(FILE *f, uint32_t v, int n)
{
    while (n--) {
        fputc((int)(v & 255), f);
        v >>= 8;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "make")) {
        const uint32_t n = 88200;
        FILE *f = fopen(argv[2], "wb");
        uint32_t i;
        if (!f)
            return 2;
        fwrite("RIFF", 1, 4, f), le(f, 36 + n * 4, 4), fwrite("WAVEfmt ", 1, 8, f), le(f, 16, 4), le(f, 1, 2);
        le(f, 2, 2), le(f, 44100, 4), le(f, 176400, 4), le(f, 4, 2), le(f, 16, 2), fwrite("data", 1, 4, f), le(f, n * 4, 4);
        for (i = 0; i < n; i++) {
            int l = (int)lrint(8000.0 * sin(2 * M_PI * 330.0 * i / 44100.0));
            int r = (int)lrint(6000.0 * sin(2 * M_PI * 550.0 * i / 44100.0));
            if (i % 22050 < 32)
                l = r = 20000;
            le(f, (uint32_t)(uint16_t)(int16_t)l, 2), le(f, (uint32_t)(uint16_t)(int16_t)r, 2);
        }
        return fclose(f) ? 2 : 0;
    }
    if (argc == 4 && (!strcmp(argv[1], "find") || !strcmp(argv[1], "same"))) {
        const char *why = NULL;
        uint32_t nt = 0, ni = 0, o, i;
        int16_t *t = wav_read(argv[2], &nt, &why), *in;
        if (!t)
            return printf("ainchk: %s: %s\n", argv[2], why), 1;
        if (!(in = wav_read(argv[3], &ni, &why)))
            return printf("ainchk: %s: %s\n", argv[3], why), 1;
        if (!strcmp(argv[1], "same")) {
            for (i = 0; i < nt && i < ni && !memcmp(t + 2 * i, in + 2 * i, 4); i++) {}
            printf("ainchk: %u and %u frames, %s\n", nt, ni, nt == ni && i == nt ? "the same" : "different");
            return nt == ni && i == nt ? 0 : 1;
        }
        if (!nt || !ni)
            return printf("ainchk: no frames\n"), 1;
        for (o = 0; o < ni; o++) {
            for (i = 0; i < nt && !memcmp(t + 2 * i, in + 2 * ((o + i) % ni), 4); i++) {}
            if (i == nt) {
                printf("ainchk: %u frames (%.2f s), the input's from frame %u on, sample for sample\n", nt, nt / 44100.0, o);
                return 0;
            }
        }
        printf("ainchk: %u frames, not a stretch of the input\n", nt);
        return 1;
    }
    if (argc == 4 && !strcmp(argv[1], "gaps")) {
        const char *why = NULL;
        uint32_t nt = 0, ni = 0, o, i, best = ~0u, best_o = 0;
        int16_t *t = wav_read(argv[2], &nt, &why), *in;
        if (!t || !nt)
            return printf("ainchk: %s: %s\n", argv[2], t ? "no frames" : why), 1;
        if (!(in = wav_read(argv[3], &ni, &why)) || !ni)
            return printf("ainchk: %s: %s\n", argv[3], in ? "no frames" : why), 1;
        for (o = 0; o < ni; o++) {
            uint32_t z = 0;
            for (i = 0; i < nt; i++) {
                const int16_t *a = t + 2 * i, *b = in + 2 * ((o + i) % ni);
                if (a[0] == b[0] && a[1] == b[1])
                    continue;
                if (a[0] || a[1])
                    break;
                z++;
            }
            if (i == nt && z < best && z < nt / 2)
                best = z, best_o = o;
        }
        if (best == ~0u)
            return printf("ainchk: %u frames, not a stretch of the input (silent gaps allowed)\n", nt), 1;
        printf("ainchk: %u frames (%.2f s), the input's from frame %u on, %u silent frames\n", nt, nt / 44100.0, best_o, best);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "info")) {
        const char *why = NULL;
        uint32_t n = 0, i;
        int pk = 0;
        int16_t *t = wav_read(argv[2], &n, &why);
        if (!t)
            return printf("ainchk: %s: %s\n", argv[2], why), 1;
        for (i = 0; i < 2 * n; i++)
            pk = abs(t[i]) > pk ? abs(t[i]) : pk;
        printf("frames %u peak %d\n", n, pk);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "trimmed")) {   /* (the take library's Trim: tests/takes_e2e.sh) */
        const char *why = NULL;
        const uint32_t from = (uint32_t)strtoul(argv[4], NULL, 10), F0 = 132;   /* (takes.h TL_CUT_FADE) */
        uint32_t nt = 0, no = 0, i, F;
        int16_t *t = wav_read(argv[2], &nt, &why), *o;
        if (!t || !(o = wav_read(argv[3], &no, &why)))
            return printf("ainchk: %s\n", why), 1;
        if (!nt || from + nt > no)
            return printf("ainchk: %u frames from %u: past the original's %u\n", nt, from, no), 1;
        F = F0 < nt / 2 ? F0 : nt / 2;
        for (i = 0; i < 2 * nt; i++) {
            const uint32_t f = i / 2;
            int64_t s = o[2 * from + i];
            if (from > 0 && f < F)
                s = s * (int64_t)f / (int64_t)F;
            if (from + nt < no && f >= nt - F)
                s = s * (int64_t)(nt - 1 - f) / (int64_t)F;
            if (t[i] != (int16_t)s)
                return printf("ainchk: frame %u differs (%d, want %d)\n", f, t[i], (int)s), 1;
        }
        printf("ainchk: %u frames, the original's from %u on, exact (%u frames faded at a cut end)\n", nt, from, F);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "reversed")) {
        const char *why = NULL;
        uint32_t na = 0, nb = 0, i;
        int16_t *a = wav_read(argv[2], &na, &why), *b;
        if (!a || !(b = wav_read(argv[3], &nb, &why)))
            return printf("ainchk: %s\n", why), 1;
        for (i = 0; i < na && na == nb && !memcmp(a + 2 * i, b + 2 * (nb - 1 - i), 4); i++) {}
        printf("ainchk: %u and %u frames, %s\n", na, nb, na == nb && i == na ? "the one reversed" : "not reversed");
        return na == nb && i == na ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "rawpeak")) {   /* a capture (stereo int16, no header): its peak, its loud frames */
        FILE *f = fopen(argv[2], "rb");
        int16_t s[2];
        long n = 0, loud = 0;
        int pk = 0;
        if (!f)
            return printf("ainchk: %s: no file\n", argv[2]), 1;
        while (fread(s, 2, 2, f) == 2) {
            const int a = abs(s[0]) > abs(s[1]) ? abs(s[0]) : abs(s[1]);
            pk = a > pk ? a : pk;
            loud += a > 1000;
            n++;
        }
        fclose(f);
        printf("frames %ld peak %d loud %ld\n", n, pk, loud);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "listen")) {
        FILE *o = fopen(argv[2], "rb"), *m = fopen(argv[3], "rb");
        int16_t a[2], b[2];
        long n = 0, same = 0, sil = 0, ramp = 0, other = 0, run = 0, best = 0, best_end = -1, after = 0, silenced_in = 0;
        if (!o || !m)
            return printf("ainchk: no file\n"), 1;
        while (fread(a, 2, 2, o) == 2 && fread(b, 2, 2, m) == 2) {
            const int quiet = abs(b[0]) < 64 && abs(b[1]) < 64;
            if (a[0] == b[0] && a[1] == b[1]) {
                same++;
                if (!quiet) {
                    if (run > best)
                        best = run, best_end = n, after = 0;
                    run = 0, silenced_in = 0;
                    if (best_end >= 0)
                        after++;
                } else if (run)
                    run++;   /* (a quiet stretch of the mix inside a silenced run) */
            } else if (!a[0] && !a[1]) {
                sil++, run++, silenced_in++;
            } else if (abs(a[0]) <= abs(b[0]) && abs(a[1]) <= abs(b[1]) && (a[0] == 0 || (a[0] < 0) == (b[0] < 0))
                       && (a[1] == 0 || (a[1] < 0) == (b[1] < 0))) {
                ramp++;
            } else
                other++;
            n++;
        }
        if (run > best)
            best = run, best_end = n;
        (void)silenced_in;
        fclose(o), fclose(m);
        printf("frames %ld same %ld silenced %ld ramp %ld other %ld run %ld after %ld\n", n, same, sil, ramp, other, best,
               after);
        return 0;
    }
    fprintf(stderr, "usage: ainchk make OUT.wav | find TAKE.wav IN.wav | same A.wav B.wav | gaps TAKE.wav IN.wav | info F.wav"
                    " | trimmed TAKE.wav ORIG.wav FROM | reversed A.wav B.wav | rawpeak F.raw\n");
    return 2;
}
