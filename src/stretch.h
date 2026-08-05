/*
 * stretch.h — pitch-preserving time-stretch (WSOLA) for playback speed.
 * ---------------------------------------------------------------------
 * Stereo interleaved f32. Push source frames in, pop time-stretched frames
 * out; speed = source-time per output-time (2.0 = twice as fast, pitch
 * unchanged). Designed for the audio callback: no allocations after create,
 * O(frames) per call, deterministic.
 *
 * Contract:
 *   - speed 1.0 must be BYPASSED by the caller (this module always windows/
 *     overlap-adds, which is not bit-exact).
 *   - reset() after any seek/track change (drops all buffered audio).
 *   - flush() at source EOS lets the buffered tail drain; pop() then returns
 *     0 when fully drained.
 */
#ifndef MN_STRETCH_H
#define MN_STRETCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mn_stretch mn_stretch;

mn_stretch *mn_stretch_create(void);
void        mn_stretch_destroy(mn_stretch *s);

/* Drop all buffered input/output (seek / track change / speed jump). */
void mn_stretch_reset(mn_stretch *s);

/* 0.5 .. 3.0 (clamped). Takes effect on the next synthesis block. */
void  mn_stretch_set_speed(mn_stretch *s, float speed);
float mn_stretch_get_speed(const mn_stretch *s);

/* How many more SOURCE frames the stretcher wants before it can synthesize
 * its next block (0 = it can produce output now, or is flushed+drained). */
int mn_stretch_need_input(const mn_stretch *s);

/* Feed source frames (interleaved stereo f32). Returns frames accepted
 * (< frames only if the internal FIFO is full — feed again after pop). */
int mn_stretch_push(mn_stretch *s, const float *src, int frames);

/* Signal source EOS: remaining buffered input is synthesized without
 * requiring further pushes; pop() drains the tail then returns 0. */
void mn_stretch_flush(mn_stretch *s);

/* Pop up to max_frames stretched frames into out. Returns frames written
 * (0 = need more input, or fully drained after flush). */
int mn_stretch_pop(mn_stretch *s, float *out, int max_frames);

#ifdef __cplusplus
}
#endif
#endif /* MN_STRETCH_H */
