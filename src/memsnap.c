/* SPDX-License-Identifier: MPL-2.0 */
/* A firmware's working memory as a file: see memsnap.h. */
#include "memsnap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "RFM1MEM1"

ms_snap_t *ms_new(uint64_t mem_size)
{
    ms_snap_t *s;
    if (mem_size > ((uint64_t)1 << 32) || !(s = malloc(sizeof *s + (size_t)mem_size)))
        return NULL;
    memset(&s->h, 0, sizeof s->h);
    s->h.mem_size = mem_size;
    s->refs = 1;
    return s;
}

ms_snap_t *ms_ref(ms_snap_t *s)
{
    if (s)
        __atomic_add_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
    return s;
}

void ms_free(ms_snap_t *s)
{
    if (s && __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0)
        free(s);
}

void ms_path(char *out, size_t cap, const char *bin)
{
    const size_t n = strlen(bin);
    if (n > 4 && !strcmp(bin + n - 4, ".bin"))
        snprintf(out, cap, "%.*s.mem", (int)(n - 4), bin);
    else
        snprintf(out, cap, "%s.mem", bin);
}

/* ---- the output */
typedef struct {
    uint8_t *b;
    size_t n, cap;
    int bad;
} out_t;
static void put(out_t *o, const void *p, size_t n)
{
    if (o->bad)
        return;
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap : 65536;
        uint8_t *nb;
        while (c < o->n + n)
            c *= 2;
        if (!(nb = realloc(o->b, c))) {
            o->bad = 1;
            return;
        }
        o->b = nb, o->cap = c;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void put8(out_t *o, uint8_t v) { put(o, &v, 1); }
static void put16(out_t *o, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(o, b, 2); }
static void put32(out_t *o, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(o, b, 4);
}
static void put64(out_t *o, uint64_t v) { put32(o, (uint32_t)v); put32(o, (uint32_t)(v >> 32)); }

static uint32_t fnv(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* ---- LZ, one page (LZ4's block format, a greedy match finder) */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void lz_len(uint8_t *d, size_t *o, size_t v)   /* a length past 15 */
{
    while (v >= 255)
        d[(*o)++] = 255, v -= 255;
    d[(*o)++] = (uint8_t)v;
}
/* src (n <= MS_PAGE) into dst (room for n + n / 255 + 16): its size */
static size_t lz_page(const uint8_t *src, size_t n, uint8_t *dst)
{
    uint16_t ht[4096];   /* position + 1 of a 4-byte sequence's last place, 0 none */
    size_t ip = 0, anchor = 0, o = 0;
    memset(ht, 0, sizeof ht);
    while (ip + 4 <= n) {
        const uint32_t v = rd32(src + ip), h = (v * 2654435761u) >> 20;
        const size_t ref = ht[h];
        ht[h] = (uint16_t)(ip + 1);
        if (ref && rd32(src + ref - 1) == v) {
            const size_t r = ref - 1, lit = ip - anchor;
            size_t ml = 4;
            uint8_t *tok = &dst[o++];
            while (ip + ml < n && src[r + ml] == src[ip + ml])
                ml++;
            *tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4 | (ml - 4 >= 15 ? 15 : ml - 4));
            if (lit >= 15)
                lz_len(dst, &o, lit - 15);
            memcpy(dst + o, src + anchor, lit);
            o += lit;
            dst[o++] = (uint8_t)(ip - r), dst[o++] = (uint8_t)((ip - r) >> 8);
            if (ml - 4 >= 15)
                lz_len(dst, &o, ml - 4 - 15);
            ip += ml;
            anchor = ip;
        } else
            ip++;
    }
    {   /* the last literals */
        const size_t lit = n - anchor;
        dst[o++] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
        if (lit >= 15)
            lz_len(dst, &o, lit - 15);
        memcpy(dst + o, src + anchor, lit);
        o += lit;
    }
    return o;
}
/* 0, or -1 (not a page of exactly n bytes) */
static int unlz_page(const uint8_t *s, size_t sn, uint8_t *d, size_t n)
{
    size_t i = 0, o = 0;
    for (;;) {
        size_t lit, ml, off;
        uint8_t t;
        if (i >= sn)
            return -1;
        t = s[i++];
        lit = t >> 4;
        if (lit == 15) {
            uint8_t x;
            do {
                if (i >= sn)
                    return -1;
                lit += x = s[i++];
            } while (x == 255);
        }
        if (lit > sn - i || lit > n - o)
            return -1;
        memcpy(d + o, s + i, lit);
        i += lit, o += lit;
        if (i == sn)   /* (the last sequence: literals only) */
            return o == n && (t & 15) == 0 ? 0 : -1;
        if (sn - i < 2)
            return -1;
        off = (size_t)s[i] | (size_t)s[i + 1] << 8;
        i += 2;
        ml = (size_t)(t & 15) + 4;
        if ((t & 15) == 15) {
            uint8_t x;
            do {
                if (i >= sn)
                    return -1;
                ml += x = s[i++];
            } while (x == 255);
        }
        if (!off || off > o || ml > n - o)
            return -1;
        if (off >= ml)
            memcpy(d + o, d + o - off, ml);
        else {   /* (overlapping: a run) */
            size_t k;
            for (k = 0; k < ml; k++)
                d[o + k] = d[o - off + k];
        }
        o += ml;
    }
}

static int all_zero(const uint8_t *p, size_t n)
{
    static const uint8_t z[MS_PAGE];
    return !memcmp(p, z, n);
}

uint8_t *ms_encode(const ms_snap_t *s, const char *flash_hex, size_t *n)
{
    const fw_snap_t *h = &s->h;
    out_t o = { NULL, 0, 0, 0 };
    uint8_t tmp[MS_PAGE + MS_PAGE / 255 + 32];
    char hex[64];
    uint64_t at;
    uint32_t i;
    size_t hdr_at;
    put(&o, MAGIC, 8);
    hdr_at = o.n;
    put32(&o, 0);
    put32(&o, 0);
    memset(hex, '0', sizeof hex);
    memcpy(hex, h->sha256, strlen(h->sha256) < 64 ? strlen(h->sha256) : 64);
    put(&o, hex, 64);
    memset(hex, '0', sizeof hex);
    if (flash_hex)
        memcpy(hex, flash_hex, strlen(flash_hex) < 64 ? strlen(flash_hex) : 64);
    put(&o, hex, 64);
    put64(&o, h->mem_size);
    put32(&o, h->g_off), put32(&o, h->g_len), put32(&o, h->tg_len);
    put(&o, h->g, FW_SNAP_GMAX), put(&o, h->tg, FW_SNAP_GMAX);
    put32(&o, h->scratch), put32(&o, h->c1_stack), put32(&o, h->ms), put32(&o, h->last_frame), put32(&o, h->frame_ms);
    put64(&o, h->frames_done), put64(&o, h->due);
    put32(&o, h->tk_ptr), put32(&o, h->rd_ptr), put32(&o, h->rd_asked), put32(&o, h->ain_buf), put32(&o, h->ain_state);
    put32(&o, h->ain_lat), put32(&o, h->ain_said), put32(&o, h->keys_held), put32(&o, h->btns_held);
    put32(&o, h->qn);
    for (i = 0; i < h->qn && i < 256; i++) {
        put64(&o, h->q[i].due);
        put8(&o, h->q[i].op), put8(&o, h->q[i].a), put16(&o, (uint16_t)h->q[i].b), put32(&o, h->q[i].c);
    }
    if (!o.bad) {   /* (the header's size, now known) */
        const uint32_t hb = (uint32_t)o.n;
        o.b[hdr_at] = (uint8_t)hb, o.b[hdr_at + 1] = (uint8_t)(hb >> 8), o.b[hdr_at + 2] = (uint8_t)(hb >> 16),
        o.b[hdr_at + 3] = (uint8_t)(hb >> 24);
    }
    for (at = 0; at < h->mem_size && !o.bad; at += MS_PAGE) {
        const size_t pn = h->mem_size - at < MS_PAGE ? (size_t)(h->mem_size - at) : MS_PAGE;
        const uint8_t *p = s->mem + at;
        size_t z;
        if (all_zero(p, pn)) {
            put8(&o, 0);
            continue;
        }
        z = lz_page(p, pn, tmp);
        if (z + 3 < pn) {
            put8(&o, 2);
            put16(&o, (uint16_t)z);
            put(&o, tmp, z);
        } else {
            put8(&o, 1);
            put(&o, p, pn);
        }
    }
    if (!o.bad)
        put32(&o, fnv(o.b, o.n));
    if (o.bad) {
        free(o.b);
        return NULL;
    }
    *n = o.n;
    return o.b;
}

/* ---- the input */
typedef struct {
    const uint8_t *b;
    size_t n, i;
    int bad;
} in_t;
static const uint8_t *take(in_t *r, size_t n)
{
    const uint8_t *p;
    if (r->bad || n > r->n - r->i) {
        r->bad = 1;
        return NULL;
    }
    p = r->b + r->i;
    r->i += n;
    return p;
}
static uint8_t get8(in_t *r) { const uint8_t *p = take(r, 1); return p ? p[0] : 0; }
static uint16_t get16(in_t *r) { const uint8_t *p = take(r, 2); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
static uint32_t get32(in_t *r)
{
    const uint8_t *p = take(r, 4);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}
static uint64_t get64(in_t *r) { const uint64_t lo = get32(r); return lo | (uint64_t)get32(r) << 32; }
static void gethex(in_t *r, char out[65])
{
    const uint8_t *p = take(r, 64);
    if (p)
        memcpy(out, p, 64);
    out[p ? 64 : 0] = 0;
}

int ms_peek(const uint8_t *b, size_t n, char sha_hex[65], char flash_hex[65])
{
    in_t r = { b, n, 0, 0 };
    const uint8_t *m = take(&r, 8);
    if (!m || memcmp(m, MAGIC, 8))
        return -1;
    get32(&r), get32(&r);
    gethex(&r, sha_hex);
    gethex(&r, flash_hex);
    return r.bad ? -1 : 0;
}

ms_snap_t *ms_decode(const uint8_t *b, size_t n, char flash_hex[65])
{
    in_t r = { b, n, 0, 0 };
    const uint8_t *m;
    ms_snap_t *s;
    fw_snap_t h;
    uint32_t hb, i;
    uint64_t at;
    if (n < 12 || (m = take(&r, 8), memcmp(m, MAGIC, 8)) || fnv(b, n - 4) != (b[n - 4] | (uint32_t)b[n - 3] << 8 |
                                                                           (uint32_t)b[n - 2] << 16 |
                                                                           (uint32_t)b[n - 1] << 24))
        return NULL;
    r.n = n - 4;
    memset(&h, 0, sizeof h);
    hb = get32(&r);
    get32(&r);
    gethex(&r, h.sha256);
    gethex(&r, flash_hex);
    h.mem_size = get64(&r);
    h.g_off = get32(&r), h.g_len = get32(&r), h.tg_len = get32(&r);
    if ((m = take(&r, FW_SNAP_GMAX)))
        memcpy(h.g, m, FW_SNAP_GMAX);
    if ((m = take(&r, FW_SNAP_GMAX)))
        memcpy(h.tg, m, FW_SNAP_GMAX);
    h.scratch = get32(&r), h.c1_stack = get32(&r), h.ms = get32(&r), h.last_frame = get32(&r), h.frame_ms = get32(&r);
    h.frames_done = get64(&r), h.due = get64(&r);
    h.tk_ptr = get32(&r), h.rd_ptr = get32(&r), h.rd_asked = get32(&r), h.ain_buf = get32(&r), h.ain_state = get32(&r);
    h.ain_lat = get32(&r), h.ain_said = get32(&r), h.keys_held = get32(&r), h.btns_held = get32(&r);
    h.qn = get32(&r);
    if (r.bad || h.qn > 256 || h.g_len > FW_SNAP_GMAX || h.tg_len > FW_SNAP_GMAX || !h.mem_size ||
        h.mem_size > ((uint64_t)1 << 32))
        return NULL;
    for (i = 0; i < h.qn; i++) {
        h.q[i].due = get64(&r);
        h.q[i].op = get8(&r), h.q[i].a = get8(&r), h.q[i].b = (int16_t)get16(&r), h.q[i].c = get32(&r);
    }
    if (r.bad || r.i != hb || !(s = ms_new(h.mem_size)))
        return NULL;
    s->h = h;
    for (at = 0; at < h.mem_size; at += MS_PAGE) {
        const size_t pn = h.mem_size - at < MS_PAGE ? (size_t)(h.mem_size - at) : MS_PAGE;
        const uint8_t tag = get8(&r);
        if (tag == 0)
            memset(s->mem + at, 0, pn);
        else if (tag == 1 && (m = take(&r, pn)))
            memcpy(s->mem + at, m, pn);
        else if (tag == 2) {
            const uint16_t z = get16(&r);
            if (!(m = take(&r, z)) || unlz_page(m, z, s->mem + at, pn) != 0)
                r.bad = 1;
        } else
            r.bad = 1;
        if (r.bad)
            break;
    }
    if (r.bad || r.i != r.n) {
        ms_free(s);
        return NULL;
    }
    return s;
}
