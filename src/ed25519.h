/* SPDX-License-Identifier: MPL-2.0 */
/* Ed25519 signature verification (RFC 8032, pure Ed25519) and SHA-512 (FIPS 180-4), with no library: the catalog's
 * index is checked with it (catsig.h) whether or not the Move has libcrypto. The field and group arithmetic is
 * TweetNaCl's (Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers; public domain), verification only; see
 * ed25519.c. Not constant-time where that does not matter: it handles only public data (keys, signatures, messages). */
#ifndef RFM14_ED25519_H
#define RFM14_ED25519_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t h[8];
    uint64_t bytes;
    uint8_t buf[128];
    unsigned fill;
} sha512_t;

void sha512_init(sha512_t *s);
void sha512_update(sha512_t *s, const void *data, size_t n);
void sha512_final(sha512_t *s, uint8_t out[64]);

/* 0: sig is pk's signature of msg; -1 not (a damaged key or signature, S >= L, or another message) */
int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]);

#endif
