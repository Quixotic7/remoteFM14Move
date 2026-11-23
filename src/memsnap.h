/* SPDX-License-Identifier: MPL-2.0 */
/* A firmware's working memory as a file (README "The working memory"): a snapshot (fwrt.h fw_snap_t, the instance's
 * memory and globals and the host's view of it) beside the flash image it goes with, state-NN.mem beside state-NN.bin
 * and move-project-NN.fwK.mem beside .fwK.bin. Pure work, no threads (the storage thread, fwrun and the unit tests).
 *
 * The file, little-endian:
 *   "RFM1MEM1", u32 the header's bytes (to the pages), u32 0
 *   char[64] the .rfm1's SHA-256 (hex), char[64] the SHA-256 of the flash image it goes with (hex: a .mem whose
 *            image is not the .bin beside it is not used)
 *   u64 the memory's size; u32 g_off, g_len, tg_len, u8[64] g, u8[64] tg; u32 scratch, c1_stack, ms, last_frame,
 *   frame_ms; u64 frames_done, due; u32 tk_ptr, rd_ptr, rd_asked, ain_buf, ain_state, ain_lat, ain_said, keys_held,
 *   btns_held; u32 qn, then qn x (u64 due, u8 op, u8 a, s16 b, u32 c)
 *   the pages (MS_PAGE bytes each, the memory's size / MS_PAGE of them), each: u8 0 (all zero), 1 + the page as it
 *   is, or 2 + u16 n + n bytes (the page as LZ4's block format has it: a token (literals << 4 | match - 4), its
 *   literals, a u16 offset back; the last sequence literals only; lengths of 15 go on in bytes of 255)
 *   u32 FNV-1a of every byte before it
 * Most of a firmware's memory is zeros or repeats: ChoralRoot's 8 MB is a few hundred KB. */
#ifndef RFM14_MEMSNAP_H
#define RFM14_MEMSNAP_H
#include <stddef.h>
#include <stdint.h>

#include "fwrt.h"

#define MS_PAGE 4096u

/* a snapshot in one block: the header, then its memory (h.mem_size bytes); ms_new makes it with one reference, ms_ref
 * adds one, ms_free gives one back (the last frees it; any thread; NULL: nothing) */
typedef struct {
    int refs;
    fw_snap_t h;
    uint8_t mem[];
} ms_snap_t;
ms_snap_t *ms_new(uint64_t mem_size);   /* NULL: no memory */
ms_snap_t *ms_ref(ms_snap_t *s);
void ms_free(ms_snap_t *s);
/* the file for s (flash_hex: the image's SHA-256 it goes with, 64 hex digits): malloc'd, its size in *n; NULL: no
 * memory */
uint8_t *ms_encode(const ms_snap_t *s, const char *flash_hex, size_t *n);
/* a file back: the snapshot (malloc'd) and the image's hash it names; NULL: not such a file (damaged, another
 * version) */
ms_snap_t *ms_decode(const uint8_t *b, size_t n, char flash_hex[65]);
/* the module's SHA-256 a file names, without decoding it (0, -1 not such a file) */
int ms_peek(const uint8_t *b, size_t n, char sha_hex[65], char flash_hex[65]);
/* a .bin path's .mem beside it */
void ms_path(char *out, size_t cap, const char *bin);

#endif
