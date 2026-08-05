/* ==========================================================================
 * audio_write.h — encode interleaved float32 PCM to an audio file.
 *
 * Used by the stem-export feature (a separated stem is 44100 Hz interleaved
 * stereo float32). Three formats:
 *   MN_AWFMT_WAV  — 24-bit PCM WAV via miniaudio's built-in encoder.
 *   MN_AWFMT_FLAC — lossless, via a compact self-contained fixed-predictor
 *                   FLAC encoder (no libFLAC dependency).
 *   MN_AWFMT_MP3  — 320 kbps CBR via the vendored `shine` encoder.
 *
 * All writers take the SAME input: interleaved f32 in [-1, 1], `frames`
 * frames of `channels` channels at `rate` Hz. Peak samples are clamped.
 * ========================================================================== */
#ifndef MN_AUDIO_WRITE_H
#define MN_AUDIO_WRITE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum mn_awfmt {
    MN_AWFMT_WAV = 0,
    MN_AWFMT_FLAC = 1,
    MN_AWFMT_MP3 = 2,
} mn_awfmt;

/* Lowercase extension (no dot) for a format, e.g. "wav". Never NULL. */
const char *mn_awfmt_ext(mn_awfmt fmt);

/* Write `frames` interleaved-float32 frames (`channels` ch @ `rate` Hz) to
 * `path` in `fmt`. Returns true on success. The parent directory must exist. */
bool mn_audio_write(const char *path, mn_awfmt fmt,
                    const float *interleaved, uint64_t frames,
                    uint32_t channels, uint32_t rate);

#endif /* MN_AUDIO_WRITE_H */
