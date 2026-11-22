/* SPDX-License-Identifier: MPL-2.0 */
/* (rfm1cs-abletonmove 0.13.0, RFM_MOVE_CAP_PROJFW) A project's own copy of each slot's firmware memory, and the
 * firmwares' states: the file names and the pure work on them (no threads; the storage thread and the unit tests call
 * these).
 *
 *   move/projects/move-project-NN.fwK.bin   project NN's copy of slot K's firmware memory (K 1..3; the flash image the
 *                                           host keeps, 1 MB), and beside it .fwK.json (id, version, sha256 of the
 *                                           .rfm1, saved, flash, hash: the SHA-256 of the image)
 *   move/projects/.clip.fwK.bin / .json     the project buffer's copies (a project cut or copied; hidden: not listed,
 *                                           not exported)
 *   states/<id>/state-KK.json               a state's json now has "hash" too (written by this host; an older one
 *                                           without it is compared byte for byte) */
#ifndef RFM14_PROJMEM_H
#define RFM14_PROJMEM_H
#include <stddef.h>
#include <stdint.h>

#include "rfm1cs/move.h"

#define PM_SLOTS 3

/* root/move/projects/move-project-NN.fwK.bin for project slot 0..31, firmware slot 0..2 */
void pm_copy_path(char *out, size_t cap, const char *root, int proj, int slot);
/* root/move/projects/.clip.fwK.bin: the project buffer's copy of firmware slot 0..2 */
void pm_clip_path(char *out, size_t cap, const char *root, int slot);
/* a .bin path's .json beside it */
void pm_json_path(char *out, size_t cap, const char *bin);
/* the SHA-256 of an image, 64 hex digits */
void pm_hash_hex(const uint8_t *b, uint32_t n, char hex[65]);
/* text (a JSON object, "{...}\n") with "hash" (the image's SHA-256) added before its last '}': malloc'd; NULL text:
 * NULL */
char *pm_json_hash(const char *text, const uint8_t *b, uint32_t n);
/* the "id" of a .json file into id: 0, -1 none */
int pm_json_id(const char *json_path, char *id, size_t cap);
/* the stored state of firmware id that holds exactly these bytes: 0..31, -1 none (n 0: none). A state's json "hash"
 * ruled out first when it has one; a candidate is always compared byte for byte */
int pm_state_match(const char *root, const char *id, const uint8_t *b, uint32_t n);
/* the firmware ids a project blob holds (its version 9 part; rfm1cs projfw.c's layout): 1 and ids filled; 0 a blob
 * of version 1..8 (or too short: no firmwares) */
int pm_blob_ids(const uint8_t *b, uint32_t n, char ids[PM_SLOTS][RFM_MOVE_FW_ID_MAX]);

#endif
