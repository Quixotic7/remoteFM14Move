/* SPDX-License-Identifier: MPL-2.0 */
/* The engine: one thread (cores 0..2, never real time) that runs everything between the Move and the firmwares:
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
    const char *default_fw;       /* the firmware for slot 1 when nothing was loaded before (an id), or NULL: the
                                   * first listed */
    int lead_blocks;              /* the ring's fill the engine keeps (blocks of 128), 4..8 */
    uint32_t splash_ms;           /* the session (the Move's controls and screen) starts this long after the engine: the
                                   * status screen with the page's address shows meanwhile */
    int cpu_mask;                 /* the engine thread's cores (bits; 0 any) */
    /* tests (the simulator): every output block from the audio thread / the engine, the first firmware's own blocks
     * and its input log (tools/fwrun --replay) */
    const char *capture_out, *capture_mix, *capture_raw, *log_inputs;
    uint32_t capture_seconds;
} engine_cfg_t;

int engine_start(const engine_cfg_t *cfg);
void engine_stop(void);           /* the firmwares' flash saved, the files written, the threads joined */

/* Move Remote's audio source (the rt thread) */
int engine_audio(void *ctx, int16_t *out, int frames);

/* ---- for the web page */
#define ENG_LCD_BYTES (240 * 240 * 2)
typedef struct {
    uint32_t seq;                 /* moves with every change of anything below */
    uint32_t lcd_seq;             /* moves with the screen */
    uint8_t lcd[ENG_LCD_BYTES];   /* the active slot's screen, RGB565 big-endian */
    uint8_t leds[44], play_green;
    char info[4096];              /* JSON: the slots, the active one, its panel's names and wiring, its live labels,
                                   * the last firmware the page asked for that failed to load */
    uint32_t info_seq;
} engine_view_t;
/* copies the view when its seq is not *seq (then *seq = the new one): 1 copied, 0 unchanged */
int engine_view(engine_view_t *v, uint32_t *seq);
/* the page's panel input for the active firmware (fwrt.h FWI_*: KEY a vel|0, BTN a 1|0, ENC a detents, MASTER b) */
void engine_panel(int op, int a, int b);
/* a JSON status line (the slots, CPU, late blocks, the ring) into out */
int engine_status(char *out, int cap);
/* the installed firmwares, {"firmwares":[...]} (each: id, name, version, license, source_url, sha256, file, base, authors,
 * built_by, categories, skin: the skin's files ([] none; web.c /api/fw/), licenses), made at start: a malloc'd copy (the caller frees it), or NULL */
char *engine_firmwares(void);
/* the page's firmware pick: firmware id into slot 0..2, done on the engine's thread as rfm1cs's FW_LOAD (the storage
 * worker opens it, the slot boots it, the Move is told). 0 queued; -1 an unknown id or slot, or the queue is full */
int engine_load(int slot, const char *id);

#endif
