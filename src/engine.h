/* SPDX-License-Identifier: MPL-2.0 */
/* The engine: one thread (cores 0..2) that runs everything between the Move and the firmwares, and a render thread
 * per slot (README "Threads": fork-join per block; a dual-core firmware's core 1 on a thread of its own too):
 *
 *   - up to 3 firmware slots (fwrt.c instances) on their device clocks, mixed with each slot's level, through a soft
 *     limiter, rendered 4..8 blocks ahead into a lock-free ring that the Move's audio thread drains (engine_audio,
 *     Move Remote's audio source: mr_set_audio_source);
 *   - rfm1cs-abletonmove's session and core, in-process: the session's protocol bytes go through a socketpair whose
 *     other end Move Remote's device core has adopted (mr_adopt) as its host connection. The core's actions drive the
 *     firmware in the active slot; its FW_* / STATE_* / PROJECT_* / SETTINGS go to the storage thread;
 *   - the FM-1's screen and LEDs: to the session (the Move's screen and LEDs, as rfm1cs draws them) and to the web
 *     page (engine_view); the page's panel input comes back through engine_panel.
 *
 * THREADS: engine_audio on the audio thread (lock-free: a ring pop and a few counters); engine_view / engine_panel /
 * engine_status from any thread (a mutex the engine holds only to copy). */
#ifndef RFM14_ENGINE_H
#define RFM14_ENGINE_H
#include <stdint.h>

typedef struct {
    const char *root;             /* the RemoteFM1 root folder */
    const char *bundled;          /* the module's own firmwares/ (copied into the root when missing), or NULL */
    const char *dev_bundled;      /* the module's firmwares-dev/ (make package DEV_FIRMWARES=..: builds to try, put in
                                   * place of the installed object of the same <id>-<version> when theirs differ), or
                                   * NULL */
    const char *default_fw;       /* the firmware for slot 1 when nothing was loaded before (an id), or NULL: the
                                   * first listed */
    int lead_blocks;              /* the ring's fill the engine keeps (blocks of 128), 4..8 */
    uint32_t splash_ms;           /* the session (the Move's controls and screen) starts this long after the engine: the
                                   * status screen with the page's address shows meanwhile */
    int cpu_mask;                 /* the engine thread's cores (bits; 0 any) */
    int serial;                   /* 1: every slot rendered on the engine's thread (no render threads: README "Threads") */
    const char *heal;             /* the module's bin/heal (Move Remote's setuid helper: SCHED_FIFO for a named thread of
                                   * this process), asked for the engine's, the slots' and core 1's threads; NULL none */
    const char *bundled_index;    /* the module's catalog snapshot (catalog/index.json), or NULL */
    const char *catalog_url;      /* the catalog's base URL when the root's catalog-url names none (NULL: the default) */
    long log_cap;                 /* stderr's file is moved to <log>.1 past this many bytes (0: 1 MB, < 0: never) */
    /* tests (the simulator): every output block from the audio thread / the engine, the first firmware's own blocks
     * and its input log (tools/fwrun --replay) */
    const char *capture_out, *capture_mix, *capture_raw, *log_inputs;
    uint32_t capture_seconds;
    const char *fx_dir;           /* the Schwung modules (midi_fx/, audio_fx/): /data/UserData/schwung/modules; NULL none */
    int fx_budget;                /* an FX module's CPU budget, permille of real time (0: 200); past it: "Too busy" */
    int frame_ms_active, frame_ms_bg;   /* frame() every this many device ms for the slot on screen and for the
                                         * others (README "Frames"; 0: 33 and 200; 15: the ABI's own) */
    int test_api;                 /* tests: engine_test_midi (sequencer MIDI from outside) */
    /* the FM-1 on USB-A (fm1link.h): off, the screen's bytes a second, USB-A packets a transfer (0: the defaults) */
    int fm1_off;
    uint32_t fm1_bps;
    int fm1_cap;
    /* the FM-1 over Wi-Fi (fm1link.h): off; an address to ask (HOST[:PORT]) besides the broadcast, or NULL */
    int fm1_wifi_off;
    int fm1_usba_screen;             /* --fm1-usba-screen: the firmware's screen over USB-A too */
    const char *fm1_addr;
    int midi_out;                    /* --midi-out: the active slot's MIDI out to Out (midiout.h), the real-time bytes
                                      * as midiout's cfg says */
} engine_cfg_t;

int engine_start(const engine_cfg_t *cfg);
void engine_stop(void);           /* the firmwares' flash saved, the files written, the threads joined */

/* Move Remote's audio source (the rt thread) */
int engine_audio(void *ctx, int16_t *out, int frames);
/* Move Remote's audio input sink (the rt thread: mr_set_audio_in_sink): each block's input, for the carts with ain1 */
void engine_audio_in(void *ctx, const int16_t *in, int frames);
/* the round trip (frames) the carts are told: the input queued plus the output queued, and the SPI's block each way */
uint32_t engine_ain_latency(void);

/* ---- for the web page */
#define ENG_LCD_BYTES (240 * 240 * 2)
#define ENG_MINI 48
typedef struct {
    uint32_t seq;                 /* moves with every change of anything below */
    uint32_t lcd_seq;             /* moves with the screen */
    uint8_t lcd[ENG_LCD_BYTES];   /* the active slot's screen, RGB565 big-endian */
    uint8_t leds[44], play_green;
    char info[4096];              /* JSON: the slots, the active one, its panel's names and wiring, its live labels,
                                   * the last firmware the page asked for that failed to load */
    uint32_t info_seq;
    /* (Mix & FX) the slots, the FX setup and the mix as the core has them, JSON ({"type":"mix",..}: engine.c mix_json) */
    char mix[16384];
    uint32_t mix_seq;
    /* meters, 0..255 (peak, square root scale): slots 1..3, returns 1..3, the master; at most 10 a second */
    uint8_t meters[7];
    uint32_t meters_seq;
    /* each slot's screen, 48 x 48 RGB565 big-endian (a box average of the 240 x 240), at most 4 a second when it changed */
    uint8_t mini[3][ENG_MINI * ENG_MINI * 2];
    uint32_t mini_seq[3];
} engine_view_t;
/* copies the view when its seq is not *seq (then *seq = the new one): 1 copied, 0 unchanged */
int engine_view(engine_view_t *v, uint32_t *seq);
/* the page's panel input for the active firmware (fwrt.h FWI_*: KEY a vel|0, BTN a 1|0, ENC a detents, MASTER b) */
void engine_panel(int op, int a, int b);
/* the page's MIDI for the active firmware (its editors' SysEx; any thread): one whole message (SysEx F0..F7 too),
 * fed to its rfm1_midi_in as USB-MIDI packets, as many a device ms as it takes. 0 queued, -1 no room */
int engine_midi(const uint8_t *msg, int n);
/* the active firmware's MIDI out for the page (SysEx and channel messages; the real-time bytes go to the sequencer
 * only), from *pos on (bytes ever written; a reader that fell more than 64 KB behind skips ahead): bytes copied */
int engine_midi_out(uint8_t *buf, int cap, uint32_t *pos);
uint32_t engine_midi_out_pos(void);   /* (where a new page starts reading) */
/* the firmware ids in the slots now ("" none; any thread): the page's file browser leaves their files alone */
void engine_running_ids(char ids[3][32]);
/* a JSON status line (the slots, CPU, late blocks, the ring) into out */
int engine_status(char *out, int cap);
/* the installed firmwares, {"firmwares":[...]} (each: id, name, version, license, source_url, sha256, file, base, authors,
 * built_by, description and categories (the catalog's), skin: the skin's files ([] none; web.c /api/fw/), licenses),
 * made whenever they change: a malloc'd copy (the caller frees it), or NULL */
char *engine_firmwares(void);
/* the page's firmware browser: the whole catalog, {"generated", "from", "firmwares":[...], "favorites":[ids]} (each:
 * id, name, version, installed ("" none), description, virtual, indexed (downloadable), bytes, thumb (64 x 64, 1 bit,
 * base64, "" none), categories; favorites: the Move's library's, in order), a malloc'd copy, or NULL */
char *engine_catalog(void);
/* the page's download of a catalog firmware: rfm1cs's FW_DOWNLOAD on the engine's thread, as a jog press in the
 * Move's catalog (its progress: the status's dl_*). 0 queued; -1 a bad id or one already queued */
int engine_download(const char *id);
/* the page's firmware pick: firmware id into slot 0..2, done on the engine's thread as rfm1cs's FW_LOAD (the storage
 * worker opens it, the slot boots it, the Move is told). 0 queued; -1 an unknown id or slot, or the queue is full */
int engine_load(int slot, const char *id, int activate);   /* activate: then make that slot the active one */

/* the page's Mix & FX edits and slot switching: engine_panel ops, done on the engine's thread into rfm1cs's core state
 * (so the Move's own screens and the project agree), then the host's (engine.c page_edit):
 *   ENG_OP_ACTIVE a slot | ENG_OP_UNLOAD a slot | ENG_OP_MIX a slot*8+field (0 level, 1 pan, 2..4 sends) b 0..127
 *   ENG_OP_ROUTE a 0..2 firmware, 3..5 MIDI FX, b the 16-track filter | ENG_OP_FXLOAD a kind*4+slot, b module (-1 empty)
 *   ENG_OP_FXPARAM a kind<<5 | slot<<3 | param 0..7, b 0..127 (the 0.4 form) | ENG_OP_PAGE a 0..3 (FX setup's column,
 *   3 the Mixer) | (row + 1) << 2 (0: the slot selected stays)
 *   (rfm1cs 0.5.0) ENG_OP_FXPAR a param 0..127, b kind<<9 | slot<<7 | value 0..127 | ENG_OP_FXPG a kind*4+slot, b the
 *   page its knobs are on (the Move's fw.fx_pg)
 *   (rfm1cs 0.6.6) ENG_OP_MROUTE a slot 0..2, b its MIDI routes (bits 0..2 the slots, 8 Out; refused: a cycle, its own) */
enum { ENG_OP_ACTIVE = 16, ENG_OP_UNLOAD, ENG_OP_MIX, ENG_OP_ROUTE, ENG_OP_FXLOAD, ENG_OP_FXPARAM, ENG_OP_PAGE,
       ENG_OP_FXPAR, ENG_OP_FXPG, ENG_OP_MROUTE, ENG_OP_LAST = ENG_OP_MROUTE };
/* the installed FX modules, fx.h fx_lib_json (malloc'd copy), or NULL */
char *engine_fx_modules(void);

/* the page's States and projects (web.c /api/store; README "The page's States and projects"): each request done on the
 * engine's thread as the Move's pads ask for it (the same actions through on_action and the deferred work, the same
 * storage jobs), the core told what changed (rfm_move_fw_states / _fw_state_cur, the project's directory and info) so
 * the Move's screen and pads show it. A request is answered once the storage thread has done its part:
 *   ENG_ST_SAVE    slot, k: the firmware's sound now into state k (a stored one: ENG_STF_REPLACE); then the active one
 *   ENG_ST_LOAD    slot, k: state k loaded (the active state's memory written first, as from its pad)
 *   ENG_ST_DELETE  slot, k: state k gone (not into the Move's state buffer: a Copy + pad there pastes what it holds)
 *   ENG_ST_IMPORT  slot, k, dir: a state file (dir: s.bin, s.json, s.mem, web.c checked it is this firmware's) as state
 *                  k (a stored one: ENG_STF_REPLACE; the active one only with ENG_STF_LOAD, which also loads it)
 *   ENG_PJ_OPEN    k: that stored project opened (the open one saved first, as from its pad in the project grid)
 *   ENG_PJ_SAVE    k: the open project saved now, its firmware copies with it
 *   ENG_PJ_DUP     k, to: a stored project copied into the empty slot to, with its firmware copies
 *   ENG_PJ_DELETE  k: a project gone with its copies (the open one: an empty project there, as a cut of it)
 *   ENG_PJ_IMPORT  k, dir: a project file (dir: p.rfmmove, p.fx.json, p.fwK.*) as project k (a stored one:
 *                  ENG_STF_REPLACE; never the open one)
 *   ENG_ST_RESET   slot, k: state k the firmware as first installed (what step 14 New gives: a fresh, erased flash,
 *                  no working memory), stored or empty; the active one: the firmware restarts fresh too, still active
 * id: the firmware the page means (a state request for a slot running another one now is refused). */
enum { ENG_ST_SAVE = 1, ENG_ST_LOAD, ENG_ST_DELETE, ENG_ST_IMPORT, ENG_PJ_OPEN, ENG_PJ_SAVE, ENG_PJ_DUP, ENG_PJ_DELETE,
       ENG_PJ_IMPORT, ENG_ST_RESET };
#define ENG_ST_STATE(op) (((op) >= ENG_ST_SAVE && (op) <= ENG_ST_IMPORT) || (op) == ENG_ST_RESET)   /* a state's */
#define ENG_STF_REPLACE 1
#define ENG_STF_LOAD 2
typedef struct {
    int op, slot, k, to, flags;
    char id[32];
    char dir[512];
} eng_store_t;
/* queued: the request's number (> 0), or -1 (the queue is full) */
int engine_store(const eng_store_t *r);
/* a request's outcome: 0 under way, 1 done, -1 refused or failed (msg: why, the page's words), -2 unknown (too old) */
int engine_store_done(int req, char *msg, int cap);
/* the States and projects as the Move has them (GET /api/store; its "ver" also in the mix JSON's "store_ver" so a page
 * reads it again when it moved): {"ver", "open", "pending", "loading", "used", "dirty", "projects": [{"k", "color",
 * "bpm", "fw": [3 ids]}] (the stored ones), "slots": [{"id", "name", "version", "used", "cur", "color"} or null]}, a
 * malloc'd copy, or NULL */
char *engine_store_json(void);
/* tests (engine_cfg_t.test_api): a sequencer MIDI message on track 1..16, as rfm_move_seq_advance gives it. 0 queued */
int engine_test_midi(int track, int b0, int b1, int b2);
/* tests: whole messages for Out (midiout_send), sent from the engine's next block. 0 queued, -1 no room or no test API */
int engine_test_midiout(const uint8_t *msgs, int n);
/* tests: byte off of slot 0..2's firmware memory (its flash image) set to val on the engine's thread, the slot's memory
 * then taken as changed (its autosave). 0 queued, -1 no test API, a bad slot or no room */
int engine_test_poke(int slot, uint32_t off, int val);
/* tests (README "The working memory"): slot 0..2's firmware read on the engine's thread between two blocks, the caller
 * waiting (2 s at most): lcd 0: n (<= 64) bytes of its working memory at off into out; lcd 1: the FNV-1a of its screen
 * (240 x 240 RGB565) into *sum. 0, -1 (no test API, no firmware there, out of its memory, no answer) */
int engine_test_peek(int slot, int lcd, uint32_t off, uint32_t n, uint8_t *out, uint32_t *sum);

#endif
