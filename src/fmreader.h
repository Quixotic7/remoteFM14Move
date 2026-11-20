/* SPDX-License-Identifier: MPL-2.0 */
/* The FM-1 screen reader protocol (charlesvestal/fm1-reader, PROTOCOL.md version 1), RemoteFM1 as its reader: a
 * firmware that has it (fm1-base's announce.c) says what its screen shows, in its own words, through Schwung's
 * screen reader (announce.h).
 *
 *   - While speech is on and the active slot runs a firmware: HELLO (F0 7D 53 52 01 01 F7) into it every 2 s; BYE
 *     (.. 02 F7) to a slot that stops being the active one, or when speech goes off. A slot that answers (HELLO: its
 *     name, 0, its words file's link) supports it; one that has not after 3 s does not (asked three times, as the
 *     page in fm1-reader asks a port). Known per slot until another firmware boots there.
 *   - SAY (.. 10 flags text F7) from the active slot, once it has answered since it became active (what it queued
 *     before is stale): its text in words (fmwords.h) to ann_fw (NOW, KEEP).
 *   - Every frame of the protocol (F0 7D 53 52 ..), from any slot, is taken out of its MIDI out: the page, the MIDI
 *     routes and Out never see one.
 *   - The words file: fetched once (https.c) on a thread of its own, kept as <root>/words/<file name> (the name is a
 *     fingerprint of the content: a kept file is always right), read from there after. Without it: the common rules.
 *
 * THREADS: everything on the engine's thread, except the words file's fetch (its own thread, fmr_start .. fmr_stop). */
#ifndef RFM14_FMREADER_H
#define RFM14_FMREADER_H
#include <stdint.h>

#define FMR_SLOTS 3

/* send: one whole SysEx into slot's firmware (0 queued, -1 not: no firmware, no room) */
typedef int (*fmr_send_fn)(int slot, const uint8_t *msg, int n);

int fmr_start(const char *root, fmr_send_fn send);   /* the fetch thread (before the engine's real-time threads) */
void fmr_stop(void);
/* every engine loop: the active slot (-1 none), its firmware's boot serial (0: none running), speech on */
void fmr_tick(uint32_t now, int active, uint32_t serial, int speech);
/* slot's MIDI out packets (USB-MIDI, as fw_midi_out gives them): the protocol's frames taken out (and acted on), the
 * rest into out (room for n + 1: a packet held from the call before). The packets left */
int fmr_filter(int slot, const uint32_t *pk, int n, uint32_t *out);
/* the active firmware speaks for itself now (it answered, speech on): the core's words for its knobs step back */
int fmr_speaks(void);
/* the status: ,"fmr_slot":..,"fmr_name":..,"fmr_words":..,"fmr_said":.. (appended JSON fields); the length */
int fmr_json(char *out, int cap);

#endif
