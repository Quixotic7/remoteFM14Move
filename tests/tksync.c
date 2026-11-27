/* SPDX-License-Identifier: MPL-2.0 */
/* tests/cr_tk_sync_e2e.sh's measure (rfm1cs 0.17.0, the owner's "ChoralRoot takeover should always match the RemoteFM1
 * tempo and sync the playhead"): where a takeover firmware's beats land on RemoteFM1's song position.
 *
 *   tksync <mix.raw> <pairs> <from-frame> <to-frame>
 *
 * mix.raw: the host's --capture mix (stereo int16 at FW_FS, frame 0 = block 0). pairs: lines "blocks pos bpm10" from
 * /api/status (blocks x FW_BLOCK is the frame where the song was at pos ticks, 96 a beat; pos -1 stopped; bpm10 the
 * tempo in use x 10). The onsets of the mix between the two frames (the firmware's metronome and its loop's chord,
 * every beat) are found (a sharp rise of the high-passed signal's energy), each one's song position read from the
 * pair before it (only where that pair and the next have the same tempo: a tempo change inside a pair's second is not
 * guessed at), and its offset from the nearest beat printed. Out: "tksync: N onsets (M beats), offsets lo..hi ticks
 * (spread S ticks, X ms), bpm A..B"; exit 0, or 2 when nothing could be measured. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FS 44100
#define BLOCK 128
#define TPB 96.0

typedef struct { double f, pos; int bpm10; } pair_t;

int main(int argc, char **argv)
{
    FILE *fp;
    long nfr, i, from, to, last = -100000;
    int16_t *pcm;
    double *y, es = 0, el = 0, lo = 1e9, hi = -1e9;
    pair_t P[4096];
    int np = 0, n = 0, bmin = 1 << 30, bmax = 0, k;
    const int S = FS / 1000, L = FS * 30 / 1000;       /* 1 ms against the 30 ms before it */
    if (argc != 5) {
        fprintf(stderr, "usage: tksync mix.raw pairs from to\n");
        return 2;
    }
    from = atol(argv[3]), to = atol(argv[4]);
    if (!(fp = fopen(argv[1], "rb")))
        return 2;
    fseek(fp, 0, SEEK_END);
    nfr = ftell(fp) / 4;
    fseek(fp, 0, SEEK_SET);
    pcm = malloc((size_t)nfr * 4), y = calloc((size_t)nfr, sizeof *y);
    if (!pcm || !y || fread(pcm, 4, (size_t)nfr, fp) != (size_t)nfr)
        return 2;
    fclose(fp);
    if (!(fp = fopen(argv[2], "r")))
        return 2;
    {
        long b;
        double pos;
        int bpm;
        while (np < 4096 && fscanf(fp, "%ld %lf %d", &b, &pos, &bpm) == 3)
            if (!np || b * BLOCK > P[np - 1].f)
                P[np].f = (double)b * BLOCK, P[np].pos = pos, P[np].bpm10 = bpm, np++;
    }
    fclose(fp);
    if (to > nfr)
        to = nfr;
    for (i = 1; i < nfr; i++)                          /* high-passed mono (the clicks' 1.5 / 2.5 kHz over the chord) */
        y[i] = ((double)pcm[2 * i] + pcm[2 * i + 1] - pcm[2 * i - 2] - pcm[2 * i - 1]) / 2.0;
    for (i = L + S; i < to; i++) {
        es += y[i] * y[i] - y[i - S] * y[i - S];
        el += y[i - S] * y[i - S] - y[i - S - L] * y[i - S - L];
        if (i < from || i - last < FS / 8)
            continue;
        if (es / S > 12.0 * (el / L) + 4000.0) {       /* a rise: its first sample past a third of the window's peak */
            double pk = 0, pos, per, off;
            long j, at = i - S;
            for (j = i - S; j <= i; j++)
                if (fabs(y[j]) > pk)
                    pk = fabs(y[j]);
            for (j = i - S; j <= i && fabs(y[j]) < pk / 3; j++)
                ;
            at = j, last = i;
            for (k = np - 1; k >= 0 && P[k].f > at; k--)
                ;
            if (k < 0 || k + 1 >= np || P[k].pos < 0 || P[k + 1].pos < 0 || P[k].bpm10 != P[k + 1].bpm10)
                continue;
            per = P[k].bpm10 / 10.0 / 60.0 * TPB / FS;   /* ticks a frame */
            pos = P[k].pos + (at - P[k].f) * per;
            off = fmod(pos, TPB);
            if (off > TPB / 2)
                off -= TPB;
            if (off < lo)
                lo = off;
            if (off > hi)
                hi = off;
            if (P[k].bpm10 < bmin)
                bmin = P[k].bpm10;
            if (P[k].bpm10 > bmax)
                bmax = P[k].bpm10;
            printf("  onset frame %ld: song %.2f ticks (beat %.0f), %+.2f ticks off it, %.1f BPM\n", at, pos,
                   floor((pos + TPB / 2) / TPB), off, P[k].bpm10 / 10.0);
            n++;
        }
    }
    if (!n) {
        printf("tksync: no onsets measured\n");
        return 2;
    }
    printf("tksync: %d onsets, offsets %+.2f..%+.2f ticks (spread %.2f ticks, %.1f ms at %.1f BPM), bpm %.1f..%.1f\n", n,
           lo, hi, hi - lo, (hi - lo) / (bmax / 10.0 / 60.0 * TPB) * 1000.0, bmax / 10.0, bmin / 10.0, bmax / 10.0);
    return 0;
}
