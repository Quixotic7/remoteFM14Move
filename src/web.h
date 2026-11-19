/* SPDX-License-Identifier: MPL-2.0 */
/* The page on :7730 (Ableton's MoveWebService has :80): RemoteFM1's own site, built for the Move (scripts/build_site.sh,
 * web/move/), its engine the firmware on the Move over /ws (web/move/js/movehost.js).
 *
 *   GET /...                the site's files, from its folder (web_start's site), by their extension's content type
 *   GET /api/status         the engine's status (JSON: blocks, late blocks, the slots' CPU)
 *   GET /api/info           the slots and the active firmware's panel, its live labels (JSON; as /ws sends it)
 *   GET /api/firmwares      the installed firmwares ({"firmwares":[...]}: engine_firmwares)
 *   GET /api/fw/<base><suf> an installed firmware's .licenses.txt, .rfm1skin (JSON), .rfm1skin.bg, .rfm1skin.bgl
 *   POST /api/load          {"slot": 0..2, "id": "...", "activate": 0|1}: that firmware into that slot (rfm1cs's FW_LOAD),
 *                           then (activate) that slot the active one; 202, or 4xx
 *   GET /api/mix            the slots, the FX setup and the mix ({"type":"mix",..}: as /ws sends it)
 *   GET /api/fx             the installed Schwung FX modules and their parameters (fx.h fx_lib_json)
 *   POST /api/edit          {"op", "a", "b"}: a Mix & FX edit or a slot switch (engine.h ENG_OP_*), as /ws's ops
 *   POST /api/test/midi     {"track", "b0", "b1", "b2"}: sequencer MIDI from outside (--test-api only; else 403)
 *   GET /ws                 a WebSocket. Out (binary): 1 y0 n rows   the screen's rows y0..y0+n-1 that changed (RGB565 BE)
 *                                                      2 leds[44] play_green
 *                                                      3 meters[7] (slots, returns, master; 0..255)
 *                                                      4 slot 48 rows   a slot's mini screen (48 x 48 RGB565 BE)
 *                                            (text): the /api/info JSON when it changes, the /api/mix JSON
 *                                                    ("type":"mix") when it changes
 *                           In (binary, 4 bytes each): op a b_lo b_hi (fwrt.h FWI_*: 1 KEY a vel|0, 2 LEAN a 0..100,
 *                                            3 BTN a 1|0, 4 ENC a detents, 5 MASTER b 0..1023; 16..24 engine.h
 *                                            ENG_OP_*: slots, mix, routing, FX; other ops are dropped)
 * The server runs on its own thread; one request per connection. */
#ifndef RFM14_WEB_H
#define RFM14_WEB_H

int web_start(int port, const char *site);   /* 0, or -1 (the port is taken); site: the site's folder */
void web_stop(void);

#endif
