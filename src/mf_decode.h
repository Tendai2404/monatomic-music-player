/*
 * mf_decode.h — Universal audio decode backends for miniaudio (Windows).
 *
 * Provides two custom ma_decoding_backend_vtable implementations that extend
 * miniaudio's built-in FLAC/MP3/WAV/Vorbis decoders so Monatomic can play
 * essentially ANY audio file the OS (or a bundled ffmpeg) can decode:
 *
 *   1. Media Foundation backend (g_mn_decoding_backend_mf) — a CATCH-ALL that
 *      uses the Windows Media Foundation IMFSourceReader to decode to PCM
 *      float32. On Win10/11 this covers AAC/M4A/ALAC/WMA/AC3/AMR/AIFF and,
 *      on 1607+, Ogg-Opus. Zero external dependencies (built into Windows).
 *
 *   2. ffmpeg fallback backend (g_mn_decoding_backend_ffmpeg) — for formats
 *      that NEITHER miniaudio NOR Media Foundation handle (WavPack, Monkey's
 *      Audio/APE, TAK, TTA, Musepack, DSD/DSF, older Opus, …). If an ffmpeg
 *      executable is discoverable (bundled beside the exe or on PATH), it is
 *      spawned to transcode the source to a canonical WAV which miniaudio then
 *      decodes. If ffmpeg is not present this backend simply declines.
 *
 * Both backends DECLINE (return MA_NO_BACKEND) for extensions that miniaudio's
 * own decoders already handle (flac/mp3/wav/ogg/oga), so the existing,
 * known-good decode path for those formats is never perturbed. Because
 * miniaudio tries custom backends before its extension dispatch, this ordering
 * matters: the MF backend is registered first (fast, native), the ffmpeg
 * backend second (heavier, spawns a process).
 *
 * Register both at every ma_decoder_config_init() site via
 * mn_decode_config_apply_backends().
 */
#ifndef MN_MF_DECODE_H
#define MN_MF_DECODE_H

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The custom decoding backends. Defined in mf_decode.c (Windows) or as
 * declining stubs elsewhere. */
extern ma_decoding_backend_vtable g_mn_decoding_backend_mf;
extern ma_decoding_backend_vtable g_mn_decoding_backend_ffmpeg;

/*
 * Apply Monatomic's custom decoding backends to a decoder config. Call this on
 * every ma_decoder_config you pass to ma_decoder_init_file(). Safe to call with
 * a freshly ma_decoder_config_init()'d config; it only fills the custom-backend
 * fields and leaves everything else untouched.
 */
void mn_decode_config_apply_backends(ma_decoder_config *cfg);

/*
 * AAC/M4A over HTTP: install the MF-over-URL backend on `cfg` and stash
 * `url` for its onInit (one-shot; engine loads are serialized). The
 * subsequent ma_decoder_init()'s read callbacks are ignored — Media
 * Foundation streams the URL itself (progressive download, Range seek).
 */
void mn_decode_config_apply_mf_url(ma_decoder_config *cfg, const char *url);

/*
 * One-time process init/teardown for the decode backends (e.g. MFStartup).
 * mn_decode_backends_init() is idempotent and thread-safe-ish (call from the
 * main thread before first decode). Not strictly required — the MF backend
 * lazily starts MF if needed — but calling it once at startup avoids repeated
 * startup/shutdown churn.
 */
void mn_decode_backends_init(void);
void mn_decode_backends_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MN_MF_DECODE_H */
