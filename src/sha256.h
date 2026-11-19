/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RFM14_SHA256_H
#define RFM14_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t n);
void sha256_final(sha256_t *s, uint8_t out[32]);
/* a file's SHA-256 as 64 hex digits (and its size): 0, or -1 when it cannot be read */
int sha256_file(const char *path, char hex[65], uint64_t *size);

#endif
