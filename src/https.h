/* SPDX-License-Identifier: MPL-2.0 */
/* The catalog's HTTP(S) client (the storage module's network thread only): one GET into a file.
 *
 * TLS is the system's OpenSSL 3 (the Move has libssl.so.3 / libcrypto.so.3, 3.0.x), opened with dlopen at the first
 * https:// fetch: the host does not link it, so it starts (and runs everything else) without it; https:// then fails
 * with a clear error. Certificate chain and host name are verified (SSL_VERIFY_PEER, SSL_set1_host, SNI) against
 * /etc/ssl/certs (the hashed CA directory, and its ca-certificates.crt when there); TLS 1.2 at least.
 *
 * HTTP/1.1 GET, Connection: close; Content-Length, chunked, or up to the close; redirects (301 302 303 307 308) up
 * to 5, to https:// only (plain http:// is followed only from a plain http:// URL, never from https://); a body over
 * max is refused; no byte for 30 s (or a connection not made in 15 s) is an error; cancel (any thread) stops it.
 *
 * Plain http:// is fetched only when allow_http is set (catalog.c: a base URL the owner gave, or the tests): what it
 * fetches is checked by the index's signature and SHA-256s, as everything is. */
#ifndef RFM14_HTTPS_H
#define RFM14_HTTPS_H
#include <stdint.h>

/* got so far, the body's length (0 unknown) */
typedef void (*https_progress_fn)(void *ctx, uint64_t got, uint64_t len);

/* url -> out (written from scratch). 0 fetched; -1 not (err; out removed) */
int https_get(const char *url, const char *out, uint64_t max, int allow_http, https_progress_fn progress, void *ctx,
              volatile int *cancel, char *err, int cap);

/* the CA directory instead of /etc/ssl/certs (RFM14_TEST_BUILD's --ca-dir; before the first fetch) */
void https_ca_dir(const char *dir);

#endif
