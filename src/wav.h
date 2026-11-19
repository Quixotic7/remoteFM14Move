/* SPDX-License-Identifier: MPL-2.0 */
/* A RIFF WAVE file as 44.1 kHz stereo int16 frames (PCM 16 / 24 / 32-bit or float; mono as L = R; any rate played as
 * 44.1 kHz), as FM1VST rfm1_run's --audio-in reads one: tools/fwrun --audio-in, the simulator's --sim-audio-in. */
#ifndef RFM14_WAV_H
#define RFM14_WAV_H
#include <stdint.h>

/* malloc'd frames (*frames of them), or NULL with why (a static string) */
int16_t *wav_read(const char *path, uint32_t *frames, const char **why);

#endif
