/* SPDX-License-Identifier: MPL-2.0 */
/* The update signing key (update.h, README "Updating from the page"): Ed25519, the public half (raw, 32 bytes). A
 * .rfm1u the page sends is installed only with this key's signature, over "RFM1-UPDATE-1\0" + the SHA-256 of the file's
 * header and payload: a different key from the catalog's (catalog_key.h), and a different message, so a catalog
 * signature can never pass as an update. The private half is on the owner's Mac only, at
 * ~/.config/remotefm14move/update-signing-key.pem (mode 600, never in git); tools/make_update.py signs with it. In hex:
 *   47faa6c4ec840d1cd9e43ba9a3b7be19de59bd40d3d708d7539b68a804ee24e7
 * (base64 R/qmxOyEDRzZ5Dupo7e+Gd5ZvUDT1wjXU5toqATuJOc=). A new key means a new host for every Move. */
#ifndef RFM14_UPDATE_KEY_H
#define RFM14_UPDATE_KEY_H
#include <stdint.h>

static const uint8_t UPD_SIGNING_KEY[32] = {
    0x47, 0xfa, 0xa6, 0xc4, 0xec, 0x84, 0x0d, 0x1c, 0xd9, 0xe4, 0x3b, 0xa9, 0xa3, 0xb7, 0xbe, 0x19,
    0xde, 0x59, 0xbd, 0x40, 0xd3, 0xd7, 0x08, 0xd7, 0x53, 0x9b, 0x68, 0xa8, 0x04, 0xee, 0x24, 0xe7,
};

#endif
