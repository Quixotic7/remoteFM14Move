/* SPDX-License-Identifier: MPL-2.0 */
/* gzip (RFC 1952) with no library, for the page's updates (update.h): a streaming reader (inflate, RFC 1951: stored,
 * fixed and dynamic blocks; one member; its CRC-32 and length checked) and a streaming writer (deflate with fixed
 * Huffman codes and a greedy LZ77 over a 32 KB window: runs and repeats shrink well, which is what the data backups
 * hold). The host links no zlib, and the Move's own may not be there: this is small and is tested both ways
 * (tests/update_test.c, and Python's / BusyBox's gzip in tests/update_e2e.sh). Memory: ~40 KB a reader, ~400 KB a
 * writer; no threads of its own. */
#ifndef RFM14_GZ_H
#define RFM14_GZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

uint32_t gz_crc32(uint32_t crc, const void *p, size_t n);   /* (start with 0) */

/* the gzip member that is the next `len` bytes of fd, inflated: its bytes to sink in pieces (sink returns 0, or -1 to
 * stop: then this returns -1 with why "stopped"). 0: whole, its CRC and length good; -1: why */
typedef int (*gz_sink_fn)(void *ctx, const uint8_t *p, size_t n);
int gz_inflate_fd(int fd, uint64_t len, gz_sink_fn sink, void *ctx, char *why, size_t cap);

/* a .gz being written to f */
typedef struct gz_writer gz_writer_t;
gz_writer_t *gzw_open(FILE *f);                                /* NULL: no memory */
int gzw_write(gz_writer_t *w, const void *p, size_t n);        /* 0, -1 a write failed */
int gzw_close(gz_writer_t *w);                                 /* the end, the trailer; frees w: 0, -1 a write failed */

#endif
