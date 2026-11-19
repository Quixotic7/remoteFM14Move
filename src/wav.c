/* SPDX-License-Identifier: MPL-2.0 */
/* wav.h */
#include "wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int16_t *wav_read(const char *path, uint32_t *frames, const char **why)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n = 0;
    uint32_t fmt = 0, ch = 0, bits = 0, at = 12, dlen = 0, bps, nf, i, k;
    const uint8_t *data = NULL;
    int16_t *out;
    *frames = 0;
    if (!f) {
        *why = "cannot open it";
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 12 || n > (1L << 30) || !(b = malloc((size_t)n)) || fread(b, 1, (size_t)n, f) != (size_t)n ||
        memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fclose(f);
        free(b);
        *why = "not a RIFF WAVE file";
        return NULL;
    }
    fclose(f);
    while ((long)at + 8 <= n) {
        const uint32_t len = le32(b + at + 4);
        const uint8_t *c = b + at + 8;
        if (!memcmp(b + at, "fmt ", 4) && len >= 16 && (long)at + 8 + 16 <= n) {
            fmt = le16(c);
            ch = le16(c + 2);
            bits = le16(c + 14);
            if (fmt == 0xFFFE && len >= 26 && (long)at + 8 + 26 <= n)
                fmt = le16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE: the sub-format's tag */
        } else if (!memcmp(b + at, "data", 4)) {
            data = c;
            dlen = (long)len <= n - (long)(at + 8) ? len : (uint32_t)(n - (long)(at + 8));
            break;
        }
        if ((uint64_t)at + 8 + len + (len & 1u) > (uint64_t)n)
            break;
        at += 8 + len + (len & 1u);
    }
    bps = bits / 8;
    if (!data || !ch || !bps || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) {
        free(b);
        *why = "no PCM 16 / 24 / 32-bit or float data";
        return NULL;
    }
    nf = dlen / (bps * ch);
    if (!(out = malloc((size_t)(nf ? nf : 1) * 4u))) {
        free(b);
        *why = "no memory";
        return NULL;
    }
    for (i = 0; i < nf; i++)
        for (k = 0; k < 2; k++) {
            const uint8_t *p = data + ((size_t)i * ch + (k < ch ? k : 0)) * bps;   /* (mono: L = R) */
            int32_t v;
            if (fmt == 3) {
                float x;
                memcpy(&x, p, 4);
                x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
                v = (int32_t)lrintf(x * 32767.0f);
            } else if (bits == 16)
                v = (int16_t)le16(p);
            else if (bits == 24)
                v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
            else
                v = (int32_t)le32(p) >> 16;
            out[2 * i + k] = (int16_t)v;
        }
    free(b);
    *frames = nf;
    *why = NULL;
    return out;
}
