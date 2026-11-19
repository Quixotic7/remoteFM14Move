/* SPDX-License-Identifier: MPL-2.0 */
/* The catalog index's signature: see catsig.h. */
#include "catsig.h"

#include <stdio.h>
#include <string.h>

#include "catalog_key.h"
#include "ed25519.h"
#ifdef RFM14_TEST_BUILD
#include "catalog_test_key.h"   /* build/testkey (the Makefile): CAT_TEST_KEY */
#endif

static const uint8_t *const KEYS[] = {
    CAT_SIGNING_KEY,
#ifdef RFM14_TEST_BUILD
    CAT_TEST_KEY,
#endif
};

int catsig_keys(const uint8_t *const **keys)
{
    *keys = KEYS;
    return (int)(sizeof KEYS / sizeof KEYS[0]);
}

/* the .sig file's base64 -> 64 bytes: 0, -1 not that */
static int sig_decode(const uint8_t *in, size_t n, uint8_t sig[64])
{
    uint32_t acc = 0;
    int bits = 0, k = 0, pad = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        const uint8_t c = in[i];
        int v;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad)
            return -1;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k >= 64)
                return -1;
            sig[k++] = (uint8_t)(acc >> bits);
        }
    }
    return k == 64 && pad <= 2 ? 0 : -1;
}

int catsig_check_key(const uint8_t pk[32], const uint8_t *index, size_t n, const uint8_t *sig_file, size_t sn,
                     char *why, int cap)
{
    uint8_t sig[64];
    if (!sig_file || !sn) {
        snprintf(why, (size_t)cap, "no signature (index.json.sig)");
        return -1;
    }
    if (sig_decode(sig_file, sn, sig) != 0) {
        snprintf(why, (size_t)cap, "a damaged signature (index.json.sig is not 64 bytes in base64)");
        return -1;
    }
    if (ed25519_verify(sig, index, n, pk) != 0) {
        snprintf(why, (size_t)cap, "a bad signature (not the catalog's signing key, or the index changed)");
        return -1;
    }
    return 0;
}

int catsig_check(const uint8_t *index, size_t n, const uint8_t *sig_file, size_t sn, char *why, int cap)
{
    const uint8_t *const *keys;
    const int nk = catsig_keys(&keys);
    int i;
    for (i = 0; i < nk; i++)
        if (catsig_check_key(keys[i], index, n, sig_file, sn, why, cap) == 0)
            return 0;
    return -1;
}
