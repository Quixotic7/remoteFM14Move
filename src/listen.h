/* SPDX-License-Identifier: MPL-2.0 */
/* Listen here (README "Listening on the page"): the Move's whole output streamed to its page.
 *
 * The audio thread (engine_audio) hands each block it gives the Move to listen_audio: while at least one page
 * listens, the block (the exact stereo int16 the Move would play: the mix after master level and FX, the screen
 * reader's words) is copied into a lock-free single-producer ring, whole blocks or none (the ring full: the block is
 * dropped and counted); then, while a listener asks for it, the block is silenced for the Move, ramped over
 * LISTEN_RAMP frames each way. Nothing else sees the silence: the recorder, the meters and mix.raw take the mix on
 * the engine's thread, before the audio thread. With no listener the tap costs a flag's load.
 *
 * The web thread (web.c, /ws/audio) is the ring's one consumer: listen_read takes whole frames of LISTEN_FRAME
 * frames; listen_set says how many pages listen and whether the Move is silent (logged when it changes).
 *
 * THREADS: listen_init on the main thread before the audio thread starts; listen_audio on the audio thread only;
 * listen_set and listen_read on the web thread only; listen_get from any thread (atomics). */
#ifndef RFM14_LISTEN_H
#define RFM14_LISTEN_H
#include <stdint.h>

#define LISTEN_RATE 44100
#define LISTEN_FRAME 882      /* frames per WebSocket frame (20 ms) */
#define LISTEN_RING 16384     /* frames in the ring (371 ms) */
#define LISTEN_RAMP 256       /* frames the mute takes in or out (5.8 ms) */

typedef struct {
    int listeners;            /* pages listening */
    int muted;                /* the Move silenced (a listener asks; the ramp may still be on its way) */
    int silent;               /* the Move's output at zero now (the ramp done) */
    int tap_peak, out_peak;   /* the last ~100 ms: the tap's peak, the Move's (while listening and 10 s after) */
    uint32_t tapped, dropped; /* blocks into the ring, blocks dropped (the ring full) */
} listen_info_t;

int listen_init(void);
void listen_audio(int16_t *out, int frames);
void listen_set(int listeners, int mute);
int listen_read(int16_t *dst);   /* LISTEN_FRAME frames into dst (stereo): 1, or 0 when fewer are queued */
void listen_get(listen_info_t *i);

#endif
