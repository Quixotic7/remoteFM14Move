/* SPDX-License-Identifier: MPL-2.0 */
/* The page on :7730 (Ableton's MoveWebService has :80): the firmware's screen and LEDs live, its panel back.
 *
 *   GET /             the page (web/index.html)
 *   GET /api/status   the engine's status (JSON: blocks, late blocks, the slots' CPU)
 *   GET /api/info     the slots and the active firmware's panel (JSON)
 *   GET /ws           a WebSocket. Out (binary): 1 y0 n rows   the screen's rows y0..y0+n-1 that changed (RGB565 BE)
 *                                               2 leds[44] play_green
 *                                     (text): the /api/info JSON when it changes
 *                     In (binary, 4 bytes): op a b_lo b_hi (fwrt.h FWI_*: 1 KEY a vel|0, 3 BTN a 1|0, 4 ENC a detents,
 *                                     5 MASTER b 0..1023)
 * The server runs on its own thread. */
#ifndef RFM14_WEB_H
#define RFM14_WEB_H

int web_start(int port);   /* 0, or -1 (the port is taken) */
void web_stop(void);

#endif
