/* SPDX-License-Identifier: MPL-2.0 */
/* The bar-cut maths the recorder and the looper share (header-only): where a grid line of the song's beats (a bar, 4
 * beats; a beat; 2 or 4 bars) falls in an engine block, from the beat at the block's first frame and the beats a frame
 * (the tempo). A line lands on the first frame at or after it, as the sequencer's tick does, so a take or a loop cut
 * here is a whole number of bars of beats however the tempo moved. */
#ifndef RFM14_BARCUT_H
#define RFM14_BARCUT_H
#include <math.h>

/* the frame (from the block's first, 0 ..) at which the beat reaches B: may be < 0 (passed) or past the block */
static inline double bc_offset(double beat0, double bpf, double B) { return ceil((B - beat0) / bpf - 1e-6); }
/* the first grid line (every beats apart, from beat 0) a block starting at beat0 can hold: a line exactly at its
 * first frame included */
static inline double bc_first_line(double beat0, double bpf, double every) { return every * ceil((beat0 - bpf) / every); }
/* the grid line at or after a beat (a launch's or a recording's time) */
static inline double bc_next_line(double beat, double every)
{
    return every > 0 ? every * ceil(beat / every - 1e-9) : beat;
}
#endif
