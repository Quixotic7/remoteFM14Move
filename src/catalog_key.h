/* SPDX-License-Identifier: MPL-2.0 */
/* The Move catalog's signing key: Ed25519, the public half (raw, 32 bytes). The index.json the host uses must carry
 * this key's signature (catsig.h). The private half is on the owner's Mac only, at
 * ~/.config/remotefm14move/catalog-signing-key.pem (never in git); `make catalog` signs with it
 * (tools/sign_index.py; CATALOG_SIGNING_KEY=path names another, e.g. for a CI). In hex:
 *   38561cc131df9c7875b9524229f5dfa446b86bd0cb57dfab9757a3875c8dfbb0
 * (base64 OFYcwTHfnHh1uVJCKfXfpEa4a9DLV9+rl1ejh1yN+7A=). A new key means a new host for every Move. */
#ifndef RFM14_CATALOG_KEY_H
#define RFM14_CATALOG_KEY_H
#include <stdint.h>

static const uint8_t CAT_SIGNING_KEY[32] = {
    0x38, 0x56, 0x1c, 0xc1, 0x31, 0xdf, 0x9c, 0x78, 0x75, 0xb9, 0x52, 0x42, 0x29, 0xf5, 0xdf, 0xa4,
    0x46, 0xb8, 0x6b, 0xd0, 0xcb, 0x57, 0xdf, 0xab, 0x97, 0x57, 0xa3, 0x87, 0x5c, 0x8d, 0xfb, 0xb0,
};

#endif
