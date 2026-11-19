/* SPDX-License-Identifier: MPL-2.0 */
/* ainchk: the audio input tests' helper (ain1: tests/ain_e2e.sh, tests/fantasy_e2e.sh).
 *   ainchk make OUT.wav        a 2 s 44.1 kHz stereo input: 330 Hz on the left, 550 Hz on the right (-12 dB), a click
 *                              (both, near full scale) every 0.5 s
 *   ainchk find TAKE.wav IN.wav   TAKE is a stretch of IN played looped, sample for sample: prints where it starts in
 *                              IN and how long it is; exit 0, else 1 (with the first frame that differs)
 *   ainchk same A.wav B.wav    the same frames (a WAV's data against another's); exit 0, else 1 */
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
    fprintf(stderr, "usage: ainchk make OUT.wav | find TAKE.wav IN.wav | same A.wav B.wav\n");
    return 2;
}
