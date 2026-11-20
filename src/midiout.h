/* SPDX-License-Identifier: MPL-2.0 */
/* "Out": the firmwares' MIDI out leaving the Move, to two places at once:
 *
 *   - the Move's USB-A port (Move Remote's mr_usba_send: the surface controller's bridge, cable 2 on the SPI side).
 *     The bridge has one cable for USB-A: whatever is plugged in (an FM-1, or a hub and what is on it) gets every
 *     message, and nothing picks a device or a cable behind it. So while an FM-1 running RemoteFM1 is on USB-A (a
 *     HELLO_R from it lately, or the session linked over USB-A: cfg.usba_busy) Out skips USB-A: its link's SysEx and
 *     ~10 packets a transfer are the panel's, and a note for the firmware would only land on the unit it drives.
 *     Any other device (a synth, a stock FM-1, an interface) gets Out. --no-fm1: USB-A is Move Remote's, Out skips it.
 *   - network MIDI: an RTP-MIDI session (RFC 6295 with Apple's session protocol: control port P, data port P + 1;
 *     IN/OK, CK clock sync at 10 kHz, RS, BY) a Mac (Audio MIDI Setup > Network) or any AppleMIDI initiator opens,
 *     found by mDNS as "RemoteFM1 Move" (_apple-midi._udp: mdns.h mdns_service). Up to MIDIOUT_PEERS at once, each
 *     sent the same packets; a peer silent for MIDIOUT_PEER_MS is dropped. No journal (J = 0). The peers' own MIDI is
 *     counted and ignored (midiout.c rtp_rx: the stub where an input would go).
 *
 * Real-time bytes (F8 clock, FA/FB/FC, FE, FF) are dropped unless cfg.realtime.
 *
 * THREADS: midiout_send from one producer thread (the engine's: its block), lock-free and allocation-free (a
 * single-producer ring of whole messages, stamped); midiout_poll, midiout_json and midiout_stop on the thread that
 * calls fm1link_tick (the engine's: mr_usba_send has one embedder thread); midiout_start before both. */
#ifndef RFM14_MIDIOUT_H
#define RFM14_MIDIOUT_H
#include <stdint.h>

#define MIDIOUT_RTP_PORT 5004        /* control; data on the next (no root: both above 1024) */
#define MIDIOUT_NAME "RemoteFM1 Move"
#define MIDIOUT_PEERS 4
#define MIDIOUT_PEER_MS 90000u       /* a peer with nothing (no CK, no packet) this long: gone */
#define MIDIOUT_RING (64 * 1024)     /* bytes of messages waiting (each + 6 bytes) */
#define MIDIOUT_MSG_MAX 16384        /* the longest message (SysEx) taken */

typedef struct {
    int rtp_port;                    /* 0: MIDIOUT_RTP_PORT; < 0: no network MIDI */
    const char *name;                /* the session's name (NULL: MIDIOUT_NAME) */
    int usba;                        /* 1: Out to USB-A (fm1link's hooks are in: not --no-fm1) */
    int (*usba_busy)(uint32_t now);  /* 1 while an FM-1 running RemoteFM1 is on USB-A (fm1link_usba_busy); NULL never */
    int realtime;                    /* 1: real-time bytes go out too */
} midiout_cfg_t;

/* the sockets (said and carried on without when a port is taken) and the ring: 0, -1 (no memory) */
int midiout_start(const midiout_cfg_t *cfg);
/* whole messages, concatenated (channel, system common, SysEx F0..F7; running status not taken), queued whole, stamped
 * now: the messages queued; the rest (no room, damaged, real time) counted in dropped / filtered */
int midiout_send(const uint8_t *msg, int n);
/* USB-MIDI packets (a firmware's rfm1_midi_out, fwrt.h fw_midi_out: SysEx in pieces) as messages: a's SysEx carries
 * over between calls (one assembler a stream) */
typedef struct { uint8_t sx[MIDIOUT_MSG_MAX]; int n, on; } midiout_asm_t;
int midiout_send_pkts(midiout_asm_t *a, const uint32_t *pkts, int n);
/* the sockets (invitations, CK, RS, BY) and the queue out: every loop */
void midiout_poll(uint32_t now_ms);
/* ,"midiout":{"rtp_peers":..,"rtp_sent":..,"usba_out_sent":..,"dropped":..,...} (with its leading comma): its length */
int midiout_json(char *out, int cap);
void midiout_stop(void);             /* BY to each peer, the sockets closed */

#endif
