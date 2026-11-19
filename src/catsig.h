/* SPDX-License-Identifier: MPL-2.0 */
/* The catalog index's signature: index.json.sig beside index.json holds the base64 (88 characters, then optional
 * white space) of the 64-byte Ed25519 signature (RFC 8032) over the exact bytes of index.json, made with the catalog's
 * signing key (catalog_key.h). An index is used (downloaded, cached, or bundled) only when its signature is good.
 *
 * Built with RFM14_TEST_BUILD (the simulator's host and the unit tests only), the test key of
 * build/testkey/catalog_test_key.h (made under build/ by the Makefile, never in git) is trusted as well. The Move's
 * host is built without it: it trusts the one key (tests/in_image.sh checks its bytes). */
#ifndef RFM14_CATSIG_H
#define RFM14_CATSIG_H
#include <stddef.h>
#include <stdint.h>

/* index (n bytes) against its .sig file's bytes: 0 good (by a trusted key); -1 not (why) */
int catsig_check(const uint8_t *index, size_t n, const uint8_t *sig_file, size_t sn, char *why, int cap);
/* the same with one key */
int catsig_check_key(const uint8_t pk[32], const uint8_t *index, size_t n, const uint8_t *sig_file, size_t sn,
                     char *why, int cap);
/* the trusted keys (the release key first): how many, *keys pointing at them */
int catsig_keys(const uint8_t *const **keys);

#endif
