/* SPDX-License-Identifier: MPL-2.0 */
/* RemoteFM1's words for Schwung's screen reader (README "Screen reader").
 *
 * Schwung speaks through `schwung-tts`, a program of its own (eSpeak NG or Flite) that the shim starts and that dies
 * with its parent. A standalone tool has no shim: Move's software is stopped while it runs, so this module does the
 * shim's part itself, through the same contract (Schwung src/host/tts_shm.h, version 1):
 *
 *   - the shared memory /schwung-tts: a request block (the settings and the text to speak, a seqlock), the settings
 *     the sidecar loaded from disk, and an audio ring (44.1 kHz stereo int16) the sidecar fills;
 *   - the sidecar /data/UserData/schwung/schwung-tts, started here as our child (it leaves when we do); it loads the
 *     user's settings (config/screen_reader_state.txt: on or off; config/tts.json: engine, speed, pitch, volume) and
 *     publishes them. We only ever restate those settings, so nothing the user chose is changed or saved;
 *   - the ring mixed into the Move's output (engine_audio, the real-time thread), over the firmwares.
 *
 * The sidecar runs whenever it is there (not --screen-reader off); RemoteFM1's own Speech mode (rfm1cs 0.6.12, Global
 * Settings: Off / On / Jog) says when words come (ann_set_open). Schwung's setting (Global Settings > Screen Reader:
 * shadow_config.json, else screen_reader_state.txt, read at the start) only picks that mode's default (ann_user_on).
 * Every request says "enabled", even when the sidecar's own setting is off; the sidecar then saves its
 * screen_reader_state.txt on, so the file is put back as it was once it has acted (and again at ann_stop).
 *
 * Words are debounced as the shim does (tts.json's debounce_ms, 300 ms by default): a value (a knob's readout, a
 * list's selection) replaces the value before it; an event (a mode, a view, a slot, a firmware loaded) is kept and
 * joined with the events before it. A new utterance cuts the one still playing (a firmware's queued words and a KEEP
 * wait for it instead: ann_fw).
 *
 * THREADS: ann_start / ann_stop on the main thread (before the engine / after it); ann_say, ann_now and ann_tick on
 * the engine's thread; ann_mix on the audio thread (lock-free: the ring's counters). */
#ifndef RFM14_ANNOUNCE_H
#define RFM14_ANNOUNCE_H
#include <stdint.h>

typedef struct {
    int mode;                 /* -1 off (no sidecar), 0 the sidecar, Schwung's setting the default mode, 1 the sidecar,
                               * the default mode On (tests) */
    const char *sidecar;      /* NULL: /data/UserData/schwung/schwung-tts */
    const char *shm;          /* NULL: "/schwung-tts" (the sidecar's own name for it) */
    const char *config_dir;   /* NULL: /data/UserData/schwung/config (screen_reader_state.txt, tts.json) */
} ann_cfg_t;

enum { ANN_VALUE = 0, ANN_EVENT = 1 };

int ann_start(const ann_cfg_t *cfg);   /* 1: speaking (the sidecar started), 0 off */
void ann_stop(void);                   /* the last request delivered (up to 0.4 s), the sidecar stopped */
int ann_on(void);                      /* 1 while the sidecar runs (words can be spoken when open) */
int ann_user_on(void);                 /* Schwung's own screen reader setting on (or --screen-reader on) */
/* words may be said now (rfm_move_speech_open). Closing drops what is pending and cuts off what is playing */
void ann_set_open(int open);
/* the host's own words, only while open: ANN_VALUE replaces the pending value, ANN_EVENT is joined to the pending events */
void ann_say(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* the core's words (RFM_MOVE_ACT_SAY): the core gates them on its mode itself, and says its mode's change in any mode */
void ann_core(int kind, const char *text, int len);
void ann_now(const char *text);        /* now, with what is pending, no debounce (leaving); only while open */
/* a firmware's own words (the screen reader protocol's SAY, fmreader.h), flags ANN_FW_*: NOW cuts off what is being said
 * (said at once, what of the firmware's was pending dropped); without it, queued: said once the speech playing has
 * ended (the sidecar cuts an utterance with the next one). KEEP: heard whole, nothing said after it until it has played.
 * A burst (a new page's lines, within ANN_FW_GAP_MS) is one utterance, after the host's own words pending */
enum { ANN_FW_NOW = 1, ANN_FW_KEEP = 2 };
#define ANN_FW_GAP_MS 30
void ann_fw(int flags, const char *text);   /* (only while open) */
void ann_tick(uint32_t now_ms);        /* the debounce: speaks what has been quiet long enough */
/* the audio thread: the sidecar's audio mixed into out (frames stereo int16). have: frames the engine gave (0: none,
 * out is not written yet). The frames out holds now */
int ann_mix(int16_t *out, int frames, int have);
/* the status: on, utterances sent, the sidecar's answers, frames mixed */
void ann_stats(int *on, uint32_t *said, uint32_t *acked, uint32_t *mixed);

#endif
