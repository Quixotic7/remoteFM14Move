/* SPDX-License-Identifier: MPL-2.0 */
/* The FM-1 as the Move's panel: an FM-1 running the RemoteFM1 firmware becomes a panel for the active slot, as
 * "Use my FM-1" does on remotefm1.com, on the Move's USB-A port or over Wi-Fi. libremotefm1's session
 * (third_party/RemoteFM1 host/src) with two links: Move Remote's USB-A hooks (mr_core.h mr_set_usba_in, mr_usba_send,
 * mr_set_usba_cap) and a TCP connection to an FM-1 running RemoteFM1 Wi-Fi (RemoteFM1 docs/PROTOCOL.md section 10).
 *
 *   - Wi-Fi: "RFM1?" broadcast on UDP 7741 every 2 s (and to cfg.wifi_addr and the address remembered in
 *     ROOT/fm1-wifi.txt) while no FM-1 is connected; the first that answers and is not busy with another host (or the
 *     one asked for or remembered, busy or not) is connected to over TCP (the port its answer gives, 7740). One FM-1 at
 *     a time; Wi-Fi is preferred to USB-A: a Wi-Fi FM-1 found while one on USB-A is linked takes over (the session
 *     restarts on it), and the USB-A one is the panel again when Wi-Fi goes. Over TCP nothing is paced (a reliable
 *     link at hundreds of KB/s: 20 frames a second, exact tiles) and the panel's events come as PANEL_EV. A connection
 *     that fails, closes or answers nothing (no session within 6 s) is dropped and the search goes on; an address that
 *     linked is remembered.
 *
 *   - Discovery: HELLO every 2 s while nothing answers; a "RemoteFM1" proto-2 HELLO_R starts a session (keepalive
 *     3000 ms). The link is slow (the Move's surface controller bridges USB-A MIDI into its SPI transfers: ~3 bytes a
 *     packet, cfg.usba_cap packets a transfer one way, and a burst of more than ~200 bytes from the unit lost the
 *     other), so the session is libremotefm1's slow link (cfg.slow_link: Bluetooth's pacing at cfg.bps, lossy tiles,
 *     whole screen changes). Lost (unplugged, silent): discovery again, every key and button the FM-1 held let go.
 *   - The FM-1 -> the active slot: keys (velocity; pressure as LEAN where the unit has TOUCH), buttons, encoders and
 *     MASTER, as the page's panel input; its MIDI_IN (the unit's 5-pin jack) as MIDI played live.
 *   - The active slot -> the FM-1: its screen (240 x 240) and LEDs (44 levels + PLAY green); a slot switch switches
 *     what the FM-1 shows and plays (what the FM-1 held on the old slot is let go there).
 *
 * THREADS: fm1link_init before Move Remote's mr_init (engine_start); everything else on the engine's thread. */
#ifndef RFM14_FM1LINK_H
#define RFM14_FM1LINK_H
#include <stdint.h>

#include "fwrt.h"

#define FM1LINK_BPS_DEFAULT 8000u    /* TILES body bytes a second (libremotefm1's slow_bps) */
#define FM1LINK_CAP_DEFAULT 10       /* USB-A packets per SPI transfer: ~10.3 KB/s on the wire, the LEDs keep 10 */
#define FM1LINK_WIRE_CAP 2048u       /* framed TILES bytes unacknowledged */

#define FM1LINK_UDP_PORT 7741        /* RemoteFM1 Wi-Fi's discovery (RFM_NET_UDP_PORT) */
#define FM1LINK_TCP_PORT 7740        /* .. and its session (RFM_NET_TCP_PORT) */

typedef struct {
    uint32_t bps;                    /* 0: FM1LINK_BPS_DEFAULT */
    int usba_cap;                    /* 0: FM1LINK_CAP_DEFAULT */
    int off;                         /* 1: no link at all (USB-A stays Move Remote's) */
    int wifi_off;                    /* 1: no Wi-Fi (USB-A only) */
    const char *wifi_addr;           /* an FM-1 to ask and connect to, HOST[:PORT] (besides the broadcast), or NULL */
    const char *root;                /* the RemoteFM1 folder: fm1-wifi.txt keeps the last address linked; NULL none */
    int udp_port;                    /* 0: FM1LINK_UDP_PORT (the tests') */
    int usba_screen;                 /* 1: the firmware's screen over USB-A too (default: a plug on black) */
} fm1link_cfg_t;

int fm1link_init(const fm1link_cfg_t *cfg);
/* the screen the FM-1 shows while linked over USB-A (240 x 240, emu_hal_t.lcd's layout) */
void fm1link_plug_screen(uint16_t *lcd);
/* once a loop: USB-A in, the session, the active slot's screen and LEDs out. fw: the active slot's firmware (NULL:
 * none, a dark panel) */
void fm1link_tick(uint32_t now, int slot, fw_t *fw);
/* 1 once after the link came or went (the engine tells the page and the Move) */
int fm1link_changed(void);
/* {"linked":..,"link":"wifi"|"usb"|"","addr":..,"wifi":"off"|"searching"|"connecting"|"connected","state":..,
 * "name":..,"fw":..,"fps":..,"bps":..,"lost":..,...} into out (one flat object): its length */
int fm1link_json(char *out, int cap);
int fm1link_linked(void);
void fm1link_stop(uint32_t now);     /* SESSION 0 (the unit back to its own screen) */

/* 44 levels led[col][line] + PLAY green as the unit's planes (remotefm1.com's mapping: lit >= 176, mid >= 96, dim >= 40,
 * faint > 0; PLAY green on when lit). out: an rfm_leds_t */
void fm1link_leds(const uint8_t levels[FW_NLEVELS], uint8_t play_green, void *out);

/* (engine.c) the FM-1's panel into slot (-1: the active one), FWI_* as the page's (MASTER as the page's), and its MIDI
 * in (whole messages, concatenated) to the active slot as MIDI played live */
void engine_fm1_in(int slot, int op, int a, int b);
void engine_fm1_midi(const uint8_t *msgs, int n);

#endif
