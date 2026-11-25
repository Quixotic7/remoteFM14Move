/* SPDX-License-Identifier: MPL-2.0 */
/* gzip with no library: see gz.h. The inflater follows Mark Adler's puff.c (zlib's contrib; the canonical Huffman
 * decode and its checks), streaming through a 32 KB window; the deflater is fixed Huffman with one hash candidate. */
#define _GNU_SOURCE
#include "gz.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------------------- CRC-32 --- */
static uint32_t g_crc[256];
static void crc_init(void)
{
    uint32_t i, k, c;
    if (g_crc[1])
        return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        g_crc[i] = c;
    }
}
uint32_t gz_crc32(uint32_t crc, const void *p, size_t n)
{
    const uint8_t *b = p;
    crc_init();
    crc = ~crc;
    while (n--)
        crc = g_crc[(crc ^ *b++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                    115, 131, 163, 195, 227, 258 };
static const uint8_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                    1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                  12, 13, 13 };

/* ---------------------------------------------------------------------------------------------------- inflate --- */
#define WIN 32768u
typedef struct {
    uint16_t count[16], sym[288];
} huff_t;
typedef struct {
    int fd, err;
    uint64_t left;            /* compressed bytes still to read from fd */
    uint8_t in[65536];
    size_t in_n, in_p;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t win[WIN];
    uint64_t out;
    uint32_t crc;
    gz_sink_fn sink;
    void *ctx;
    int stopped;
} inf_t;

static int getbyte(inf_t *s)
{
    if (s->in_p == s->in_n) {
        ssize_t r;
        size_t want = sizeof s->in;
        if (s->err)
            return 0;
        if ((uint64_t)want > s->left)
            want = (size_t)s->left;
        r = want ? read(s->fd, s->in, want) : 0;
        if (r < 0 && errno == EINTR)
            return getbyte(s);
        if (r <= 0) {
            s->err = 1;
            return 0;
        }
        s->left -= (uint64_t)r;
        s->in_n = (size_t)r, s->in_p = 0;
    }
    return s->in[s->in_p++];
}
static uint32_t bits(inf_t *s, int n)
{
    uint32_t v;
    while (s->bitcnt < n) {
        s->bitbuf |= (uint32_t)getbyte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}
static void flush(inf_t *s, size_t n)
{
    if (!n || s->stopped)
        return;
    s->crc = gz_crc32(s->crc, s->win, n);
    if (s->sink(s->ctx, s->win, n) != 0)
        s->stopped = 1;
}
static inline void put(inf_t *s, uint8_t b)
{
    s->win[s->out & (WIN - 1)] = b;
    if ((++s->out & (WIN - 1)) == 0)
        flush(s, WIN);
}
static int decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        const int count = h->count[len];
        code |= (int)bits(s, 1);
        if (code - count < first)
            return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
/* 0 complete, > 0 incomplete, < 0 over-subscribed */
static int construct(huff_t *h, const uint16_t *length, int n)
{
    int sym, len, left;
    uint16_t offs[16];
    for (len = 0; len < 16; len++)
        h->count[len] = 0;
    for (sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym])
            h->sym[offs[length[sym]]++] = (uint16_t)sym;
    return left;
}
static const char *codes(inf_t *s, const huff_t *lc, const huff_t *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (s->err)
            return "the payload ends early";
        if (s->stopped)
            return "stopped";
        if (sym < 0)
            return "a bad code in the payload";
        if (sym < 256)
            put(s, (uint8_t)sym);
        else if (sym == 256)
            return NULL;
        else {
            unsigned len, dist;
            sym -= 257;
            if (sym >= 29)
                return "a bad length in the payload";
            len = LBASE[sym] + bits(s, LEXT[sym]);
            sym = decode(s, dc);
            if (sym < 0 || sym >= 30)
                return "a bad distance in the payload";
            dist = DBASE[sym] + bits(s, DEXT[sym]);
            if (dist > s->out)
                return "a distance too far back in the payload";
            while (len--)
                put(s, s->win[(s->out - dist) & (WIN - 1)]);
        }
    }
}
static const char *dynamic(inf_t *s)
{
    static const uint8_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint16_t lengths[320];
    huff_t lc, dc;
    int nlen, ndist, ncode, index, err;
    nlen = (int)bits(s, 5) + 257;
    ndist = (int)bits(s, 5) + 1;
    ncode = (int)bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return "a bad block in the payload";
    for (index = 0; index < 19; index++)
        lengths[ORDER[index]] = index < ncode ? (uint16_t)bits(s, 3) : 0;
    if (construct(&lc, lengths, 19) != 0)
        return "a bad code table in the payload";
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lc), len = 0;
        if (s->err)
            return "the payload ends early";
        if (sym < 0)
            return "a bad code table in the payload";
        if (sym < 16)
            lengths[index++] = (uint16_t)sym;
        else {
            if (sym == 16) {
                if (index == 0)
                    return "a bad code table in the payload";
                len = lengths[index - 1];
                sym = 3 + (int)bits(s, 2);
            } else if (sym == 17)
                sym = 3 + (int)bits(s, 3);
            else
                sym = 11 + (int)bits(s, 7);
            if (index + sym > nlen + ndist)
                return "a bad code table in the payload";
            while (sym--)
                lengths[index++] = (uint16_t)len;
        }
    }
    if (lengths[256] == 0)
        return "a bad code table in the payload";
    err = construct(&lc, lengths, nlen);
    if (err && (err < 0 || nlen != lc.count[0] + lc.count[1]))
        return "a bad code table in the payload";
    err = construct(&dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dc.count[0] + dc.count[1]))
        return "a bad code table in the payload";
    return codes(s, &lc, &dc);
}
static const char *fixed(inf_t *s)
{
    static huff_t lc, dc;
    static int made;
    if (!made) {   /* (the same tables every time: made by the first caller, update.c's one worker) */
        uint16_t l[288];
        int i;
        for (i = 0; i < 144; i++)
            l[i] = 8;
        for (; i < 256; i++)
            l[i] = 9;
        for (; i < 280; i++)
            l[i] = 7;
        for (; i < 288; i++)
            l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++)
            l[i] = 5;
        construct(&dc, l, 30);
        made = 1;
    }
    return codes(s, &lc, &dc);
}
static const char *stored(inf_t *s)
{
    unsigned len, nlen;
    s->bitbuf = 0, s->bitcnt = 0;   /* (the rest of the byte) */
    len = (unsigned)getbyte(s);
    len |= (unsigned)getbyte(s) << 8;
    nlen = (unsigned)getbyte(s);
    nlen |= (unsigned)getbyte(s) << 8;
    if (s->err)
        return "the payload ends early";
    if (len != (~nlen & 0xffffu))
        return "a bad stored block in the payload";
    while (len--) {
        const int b = getbyte(s);
        if (s->err)
            return "the payload ends early";
        put(s, (uint8_t)b);
    }
    return s->stopped ? "stopped" : NULL;
}

int gz_inflate_fd(int fd, uint64_t len, gz_sink_fn sink, void *ctx, char *why, size_t cap)
{
    inf_t *s = calloc(1, sizeof *s);
    const char *e = NULL;
    int flg, last, i;
    uint32_t crc, isize;
    if (!s) {
        snprintf(why, cap, "no memory");
        return -1;
    }
    s->fd = fd, s->left = len, s->sink = sink, s->ctx = ctx;
    if (getbyte(s) != 0x1f || getbyte(s) != 0x8b || getbyte(s) != 8) {
        e = "the payload is not gzip";
        goto out;
    }
    flg = getbyte(s);
    for (i = 0; i < 6; i++)
        getbyte(s);
    if (flg & 0xe0) {
        e = "the payload is not gzip";
        goto out;
    }
    if (flg & 4) {   /* FEXTRA */
        int xl = getbyte(s);
        xl |= getbyte(s) << 8;
        while (xl-- && !s->err)
            getbyte(s);
    }
    if (flg & 8)     /* FNAME */
        while (getbyte(s) && !s->err) {}
    if (flg & 16)    /* FCOMMENT */
        while (getbyte(s) && !s->err) {}
    if (flg & 2)     /* FHCRC */
        getbyte(s), getbyte(s);
    if (s->err) {
        e = "the payload ends early";
        goto out;
    }
    do {
        int type;
        last = (int)bits(s, 1);
        type = (int)bits(s, 2);
        e = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : "a bad block in the payload";
        if (!e && s->err)
            e = "the payload ends early";
    } while (!e && !last);
    if (e)
        goto out;
    flush(s, (size_t)(s->out & (WIN - 1)));
    if (s->stopped) {
        e = "stopped";
        goto out;
    }
    s->bitbuf = 0, s->bitcnt = 0;
    crc = (uint32_t)getbyte(s);
    crc |= (uint32_t)getbyte(s) << 8;
    crc |= (uint32_t)getbyte(s) << 16;
    crc |= (uint32_t)getbyte(s) << 24;
    isize = (uint32_t)getbyte(s);
    isize |= (uint32_t)getbyte(s) << 8;
    isize |= (uint32_t)getbyte(s) << 16;
    isize |= (uint32_t)getbyte(s) << 24;
    if (s->err)
        e = "the payload ends early";
    else if (crc != s->crc || isize != (uint32_t)s->out)
        e = "the payload's CRC does not match";
out:
    if (e)
        snprintf(why, cap, "%s", e);
    free(s);
    return e ? -1 : 0;
}

/* ---------------------------------------------------------------------------------------------------- deflate --- */
#define HBITS 15
#define MAXM 258
struct gz_writer {
    FILE *f;
    uint32_t crc;
    uint64_t total;
    uint8_t buf[2 * WIN];
    size_t fill, pos;
    int32_t head[1 << HBITS];
    uint64_t acc;
    int nacc, err;
    uint8_t ob[16384];
    size_t on;
};

static void ob_flush(gz_writer_t *w)
{
    if (w->on && fwrite(w->ob, 1, w->on, w->f) != w->on)
        w->err = 1;
    w->on = 0;
}
static void putbits(gz_writer_t *w, uint32_t v, int n)
{
    w->acc |= (uint64_t)v << w->nacc;
    w->nacc += n;
    while (w->nacc >= 8) {
        w->ob[w->on++] = (uint8_t)w->acc;
        w->acc >>= 8;
        w->nacc -= 8;
        if (w->on == sizeof w->ob)
            ob_flush(w);
    }
}
static void putcode(gz_writer_t *w, uint32_t code, int len)   /* a Huffman code: its bits from the top */
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < len; i++)
        r |= ((code >> i) & 1u) << (len - 1 - i);
    putbits(w, r, len);
}
static void putsym(gz_writer_t *w, unsigned sym)
{
    if (sym < 144)
        putcode(w, 0x30 + sym, 8);
    else if (sym < 256)
        putcode(w, 0x190 + sym - 144, 9);
    else if (sym < 280)
        putcode(w, sym - 256, 7);
    else
        putcode(w, 0xc0 + sym - 280, 8);
}
static void putmatch(gz_writer_t *w, unsigned len, unsigned dist)
{
    int i = 28;
    while (len < LBASE[i])
        i--;
    putsym(w, 257u + (unsigned)i);
    putbits(w, len - LBASE[i], LEXT[i]);
    i = 29;
    while (dist < DBASE[i])
        i--;
    putcode(w, (uint32_t)i, 5);
    putbits(w, dist - DBASE[i], DEXT[i]);
}
static inline unsigned hash3(const uint8_t *p)
{
    return ((unsigned)p[0] << 10 ^ (unsigned)p[1] << 5 ^ p[2]) & ((1u << HBITS) - 1u);
}
static void encode(gz_writer_t *w, int final)
{
    const size_t limit = final ? w->fill : w->fill > MAXM ? w->fill - MAXM : 0;
    while (w->pos < limit) {
        const size_t pos = w->pos;
        unsigned len = 0;
        int32_t cand = -1;
        size_t h = 0;
        if (pos + 3 <= w->fill) {
            h = hash3(w->buf + pos);
            cand = w->head[h];
            w->head[h] = (int32_t)pos;
        }
        if (cand >= 0 && pos - (size_t)cand <= WIN) {
            size_t max = w->fill - pos;
            if (max > MAXM)
                max = MAXM;
            while (len < max && w->buf[(size_t)cand + len] == w->buf[pos + len])
                len++;
        }
        if (len >= 3) {
            size_t k;
            putmatch(w, len, (unsigned)(pos - (size_t)cand));
            for (k = 1; k < len && pos + k + 3 <= w->fill; k++)
                w->head[hash3(w->buf + pos + k)] = (int32_t)(pos + k);
            w->pos += len;
        } else {
            putsym(w, w->buf[pos]);
            w->pos++;
        }
    }
}
static void slide(gz_writer_t *w)
{
    size_t i;
    memmove(w->buf, w->buf + WIN, w->fill - WIN);
    w->fill -= WIN, w->pos -= WIN;
    for (i = 0; i < (1u << HBITS); i++)
        w->head[i] = w->head[i] >= (int32_t)WIN ? w->head[i] - (int32_t)WIN : -1;
}

gz_writer_t *gzw_open(FILE *f)
{
    static const uint8_t H[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
    gz_writer_t *w = calloc(1, sizeof *w);
    size_t i;
    if (!w)
        return NULL;
    w->f = f;
    for (i = 0; i < (1u << HBITS); i++)
        w->head[i] = -1;
    if (fwrite(H, 1, sizeof H, f) != sizeof H)
        w->err = 1;
    putbits(w, 0, 1);   /* (one fixed block holds it all; gzw_close ends it and adds an empty last one) */
    putbits(w, 1, 2);
    return w;
}
int gzw_write(gz_writer_t *w, const void *p, size_t n)
{
    const uint8_t *b = p;
    w->crc = gz_crc32(w->crc, p, n);
    w->total += n;
    while (n) {
        size_t k = sizeof w->buf - w->fill;
        if (k > n)
            k = n;
        memcpy(w->buf + w->fill, b, k);
        w->fill += k, b += k, n -= k;
        encode(w, 0);
        if (w->fill == sizeof w->buf)
            slide(w);
    }
    return w->err ? -1 : 0;
}
int gzw_close(gz_writer_t *w)
{
    uint8_t t[8];
    int err, i;
    encode(w, 1);
    putsym(w, 256);
    putbits(w, 1, 1);   /* the last block: fixed, empty */
    putbits(w, 1, 2);
    putsym(w, 256);
    if (w->nacc)
        putbits(w, 0, 8 - w->nacc);
    for (i = 0; i < 4; i++) {
        t[i] = (uint8_t)(w->crc >> (8 * i));
        t[4 + i] = (uint8_t)(w->total >> (8 * i));
    }
    for (i = 0; i < 8; i++)
        putbits(w, t[i], 8);
    ob_flush(w);
    err = w->err;
    free(w);
    return err ? -1 : 0;
}
