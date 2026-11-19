/* SPDX-License-Identifier: MPL-2.0 */
/* Ed25519 verification and SHA-512: see ed25519.h.
 *
 * The arithmetic below (gf, car25519 .. unpackneg, modL, reduce, and crypto_sign_open's steps in ed25519_verify) is
 * TweetNaCl 20140427's (https://tweetnacl.cr.yp.to/, public domain), with only these changes: verification only, the
 * message hashed as it streams (no copy of it), S < L required (RFC 8032 5.1.7; TweetNaCl does not check it), the
 * shifts of negative values written as multiplications, the curve constants checked against their definitions
 * (d = -121665/121666, the base point, sqrt(-1)). SHA-512 is ours (FIPS 180-4). Tested with RFC 8032's vectors
 * (tests/unit.c). */
#include "ed25519.h"

#include <string.h>

typedef int64_t i64;
typedef i64 gf[16];

static const gf gf0, gf1 = { 1 },
    D = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f,
          0x6cee, 0x5203 },
    D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df,
           0xd9dc, 0x2406 },
    X = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e,
          0x36d3, 0x2169 },
    Y = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
          0x6666, 0x6666 },
    I = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1,
          0x2480, 0x2b83 };

/* ------------------------------------------------------------------ SHA-512 --- */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define ROR(x, n) ((x) >> (n) | (x) << (64 - (n)))

static void sha512_block(uint64_t h[8], const uint8_t *p)
{
    uint64_t w[80], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; i++) {
        int k;
        w[i] = 0;
        for (k = 0; k < 8; k++)
            w[i] = w[i] << 8 | p[i * 8 + k];
    }
    for (i = 16; i < 80; i++) {
        const uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ w[i - 15] >> 7;
        const uint64_t s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ w[i - 2] >> 6;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (i = 0; i < 80; i++) {
        const uint64_t t1 = hh + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        const uint64_t t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void sha512_init(sha512_t *s)
{
    static const uint64_t H0[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    memcpy(s->h, H0, sizeof H0);
    s->bytes = 0;
    s->fill = 0;
}

void sha512_update(sha512_t *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->bytes += n;
    while (n) {
        if (s->fill == 0 && n >= 128) {
            sha512_block(s->h, p);
            p += 128, n -= 128;
            continue;
        }
        {
            size_t k = 128 - s->fill;
            if (k > n)
                k = n;
            memcpy(s->buf + s->fill, p, k);
            s->fill += (unsigned)k, p += k, n -= k;
            if (s->fill == 128) {
                sha512_block(s->h, s->buf);
                s->fill = 0;
            }
        }
    }
}

void sha512_final(sha512_t *s, uint8_t out[64])
{
    const uint64_t bits = s->bytes * 8;
    int i;
    s->buf[s->fill++] = 0x80;
    if (s->fill > 112) {
        memset(s->buf + s->fill, 0, 128 - s->fill);
        sha512_block(s->h, s->buf);
        s->fill = 0;
    }
    memset(s->buf + s->fill, 0, 128 - s->fill);
    for (i = 0; i < 8; i++)   /* (the length's high 64 bits: 0) */
        s->buf[127 - i] = (uint8_t)(bits >> (8 * i));
    sha512_block(s->h, s->buf);
    for (i = 0; i < 64; i++)
        out[i] = (uint8_t)(s->h[i / 8] >> (56 - 8 * (i % 8)));
}

/* ------------------------------------------------------------------ GF(2^255 - 19) (TweetNaCl) --- */
static int vn32(const uint8_t *x, const uint8_t *y)
{
    uint32_t i, d = 0;
    for (i = 0; i < 32; i++)
        d |= (uint32_t)(x[i] ^ y[i]);
    return (1 & ((d - 1) >> 8)) - 1;
}

static void set25519(gf r, const gf a)
{
    int i;
    for (i = 0; i < 16; i++)
        r[i] = a[i];
}

static void car25519(gf o)
{
    int i;
    i64 c;
    for (i = 0; i < 16; i++) {
        o[i] += (i64)1 << 16;
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b)
{
    i64 t, c = ~((i64)b - 1);
    int i;
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++)
        t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return vn32(c, d);
}

static uint8_t par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    int i, j;
    for (i = 0; i < 31; i++)
        t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++)
        o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a)
{
    M(o, a, a);
}

static void inv25519(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++)
        c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4)
            M(c, c, i);
    }
    for (a = 0; a < 16; a++)
        o[a] = c[a];
}

static void pow2523(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++)
        c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1)
            M(c, c, i);
    }
    for (a = 0; a < 16; a++)
        o[a] = c[a];
}

/* ------------------------------------------------------------------ the group (TweetNaCl) --- */
static void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], uint8_t b)
{
    int i;
    for (i = 0; i < 4; i++)
        sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
    int i;
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (i = 255; i >= 0; --i) {
        const uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t *s)
{
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static const i64 L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static void modL(uint8_t *r, i64 x[64])
{
    i64 carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++)
        x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t *r)
{
    i64 x[64];
    int i;
    for (i = 0; i < 64; i++)
        x[i] = (i64)r[i];
    for (i = 0; i < 64; i++)
        r[i] = 0;
    modL(r, x);
}

static int unpackneg(gf r[4], const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num))
        M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num))
        return -1;
    if (par25519(r[0]) == (p[31] >> 7))
        Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

/* S < L, little-endian */
static int s_canonical(const uint8_t *s)
{
    int i;
    for (i = 31; i >= 0; i--) {
        if (s[i] < L[i])
            return 1;
        if (s[i] > L[i])
            return 0;
    }
    return 0;   /* (S == L) */
}

int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32])
{
    uint8_t h[64], t[32];
    gf p[4], q[4];
    sha512_t s;
    if (!s_canonical(sig + 32) || unpackneg(q, pk))
        return -1;
    sha512_init(&s);   /* h = SHA-512(R || A || M) mod L */
    sha512_update(&s, sig, 32);
    sha512_update(&s, pk, 32);
    sha512_update(&s, msg, n);
    sha512_final(&s, h);
    reduce(h);
    scalarmult(p, q, h);   /* [h](-A) + [S]B, packed, is R */
    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);
    return vn32(sig, t) ? -1 : 0;
}
