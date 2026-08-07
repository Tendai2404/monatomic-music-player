/*
 * audio_engine.h — Monatomic hi-fi audio engine (public C API).
 *
 * A thin, hi-fidelity playback engine layered over miniaudio. It owns a single
 * device + decoder graph and exposes transport controls (load/play/pause/stop/
 * seek), gain/volume, and format/position introspection.
 *
 * All functions are safe to call from the UI thread. Playback is driven on
 * miniaudio's internal audio callback thread; the engine handles the necessary
 * synchronization internally. Unless noted, functions are non-blocking.
 *
 * Ownership: the caller creates an mn_engine via mn_engine_create() and must
 * release it with mn_engine_destroy(). The engine copies any paths it needs;
 * the caller retains ownership of strings passed in.
 *
 * Threading: a single mn_engine handle is NOT safe for concurrent mutation from
 * multiple caller threads. Serialize transport calls from one control thread.
 */
#ifndef MN_AUDIO_ENGINE_H
#define MN_AUDIO_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Constants                                                                  */
/* ------------------------------------------------------------------------- */

/* Canonical stem-pipeline format (and the fallback device rate). The engine
 * adapts its decode/device rate to the source + hardware at track load; when
 * neural stems are enabled the pipeline is pinned to this rate so mn_stems_mix
 * always sees 44100 Hz stereo. */
#define MN_ENGINE_SAMPLE_RATE   44100u
#define MN_ENGINE_CHANNELS      2u

/* Number of log-spaced spectrum-analyzer bars published to the UI. */
#define MN_SPECTRUM_BARS        32u

/* Length of the human-readable format string, including NUL. */
#define MN_FORMAT_STR_CAP       32

/* Capacity of the native-sample-rate list in mn_audio_caps. */
#define MN_CAPS_MAX_RATES       16

/* ------------------------------------------------------------------------- */
/* Result codes                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Return codes for engine operations. MN_OK == 0; all errors are negative so
 * callers can test `if (rc < 0)`.
 */
typedef enum mn_result {
    MN_OK              =  0,  /* success */
    MN_ERR_INVALID     = -1,  /* NULL / bad argument */
    MN_ERR_NOMEM       = -2,  /* allocation failed */
    MN_ERR_DEVICE      = -3,  /* audio device init/start failed */
    MN_ERR_OPEN        = -4,  /* could not open/decode the file */
    MN_ERR_UNSUPPORTED = -5,  /* codec/format not supported */
    MN_ERR_STATE       = -6,  /* operation invalid in current state */
    MN_ERR_SEEK        = -7,  /* seek failed */
    MN_ERR_IO          = -8   /* underlying I/O error */
} mn_result;

/* Playback transport state. */
typedef enum mn_play_state {
    MN_STATE_STOPPED = 0,  /* no track, or stopped (position reset to 0) */
    MN_STATE_PLAYING = 1,  /* actively rendering audio */
    MN_STATE_PAUSED  = 2   /* loaded, position held, device idle */
} mn_play_state;

/* ------------------------------------------------------------------------- */
/* Format description                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Describes the currently loaded track's source format and the engine's output
 * format. Populated by mn_engine_get_format(). When no track is loaded all
 * numeric fields are 0 and `format` is an empty string.
 */
typedef struct mn_audio_format {
    /* Source (decoded-from-file) properties. */
    uint32_t src_sample_rate;   /* Hz, e.g. 44100, 96000 */
    uint32_t src_channels;      /* channel count in the source stream */
    uint32_t src_bits;          /* bits per sample of the source (0 if lossy/unknown) */

    /* Engine output properties: the REAL hardware-side format the device is
     * running at (ma_device playback.internalSampleRate/-Channels/-Format),
     * i.e. what actually leaves the DAC path, not the float mix format. */
    uint32_t out_sample_rate;   /* Hz, device internal rate                  */
    uint32_t out_channels;      /* device internal channel count             */
    uint32_t out_bits;          /* device internal bit depth (16/24/32)      */

    /* Nominal source bitrate in bits/sec, 0 if unknown (e.g. lossless PCM). */
    uint32_t bitrate;

    /* Output device truth for the UI's output pills. */
    bool out_exclusive;         /* device really opened in exclusive mode    */
    char out_pcm[12];           /* device internal sample format, "PCM 24"…  */

    /* Quality-event transparency (the app being honest about any lossy step):
     *   pipe_channels : channel count actually delivered (== src unless folded)
     *   downmixed     : true when a >2ch source was folded to stereo
     *   rate_limited  : true when the pipeline rate was clamped below source
     *                   (hardware ceiling or audiobook power cap)             */
    uint32_t pipe_channels;
    bool     downmixed;
    bool     rate_limited;

    /* Short codec/container label, NUL-terminated, e.g. "FLAC", "MP3", "WAV". */
    char format[MN_FORMAT_STR_CAP];
} mn_audio_format;

/* ------------------------------------------------------------------------- */
/* Hardware capability description                                            */
/* ------------------------------------------------------------------------- */

/*
 * Native capabilities of the default playback device, discovered by probing
 * the backend (WASAPI on Windows) via miniaudio. Populated by
 * mn_engine_get_caps(). All fields are best-effort: backends/devices that do
 * not expose a value report a conservative default rather than 0 where noted.
 */
typedef struct mn_audio_caps {
    char    device_name[128];   /* human-readable endpoint name              */
    int32_t max_bit_depth;      /* deepest native PCM depth (hardware truth
                                 * from exclusive-mode probing when available,
                                 * else the shared-mode mix format's depth)   */
    int32_t mix_sample_rate;    /* the OS mixer / shared-mode rate, Hz        */
    int32_t max_sample_rate;    /* highest supported rate, Hz                 */
    int32_t max_channels;       /* widest supported channel layout (6 = 5.1)  */
    int32_t native_rate_count;  /* entries used in native_rates[]             */
    int32_t native_rates[MN_CAPS_MAX_RATES]; /* distinct native rates, ascending */
    bool    exclusive_capable;  /* device accepts exclusive-mode (bit-exact)  */
} mn_audio_caps;

/* ------------------------------------------------------------------------- */
/* Engine lifecycle                                                           */
/* ------------------------------------------------------------------------- */

/* Opaque engine handle. */
typedef struct mn_engine mn_engine;

/*
 * Forward declaration of the neural stem session (defined in stems.h). The
 * engine only stores a borrowed pointer and calls mn_stems_mix() from its audio
 * callback; it never owns or frees the session.
 */
typedef struct mn_stems mn_stems;

/*
 * Create an audio engine and initialize (but do not start) the output device.
 * On success writes a non-NULL handle to *out_engine and returns MN_OK.
 * On failure *out_engine is set to NULL and a negative mn_result is returned.
 */
mn_result mn_engine_create(mn_engine **out_engine);

/*
 * Stop playback, tear down the device/decoder, and free the engine.
 * Safe to call with NULL (no-op). After return the handle is invalid.
 */
void mn_engine_destroy(mn_engine *engine);

/* ------------------------------------------------------------------------- */
/* Loading / transport                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Load a track from disk, replacing any currently loaded track. Decoding is
 * initialized but playback does NOT start automatically; call mn_engine_play().
 * `path` is a UTF-8 filesystem path. Returns MN_OK, or MN_ERR_OPEN /
 * MN_ERR_UNSUPPORTED on decode-init failure. On failure the previously loaded
 * track (if any) is unloaded and state becomes MN_STATE_STOPPED.
 */
mn_result mn_engine_load(mn_engine *engine, const char *path);

/*
 * Load an HTTP(S) audio stream (internet radio / streamed podcast episode).
 * Blocks for connect + pre-roll — call from a worker thread, never the CEF
 * UI thread. `want_icy` requests ICY (Icecast) song-title metadata (radio).
 * Duration comes from a prior mn_engine_set_length_hint_ms (podcasts) or is
 * unknown/0 (live). On failure `err` (optional) carries a short reason and
 * the previous track is unloaded. MP3/FLAC/WAV payloads; AAC/MP4 returns
 * MN_ERR_UNSUPPORTED until the MF byte-stream bridge lands.
 */
mn_result mn_engine_load_url(mn_engine *engine, const char *url, int want_icy,
                             char *err, size_t err_cap);

/* Stream introspection: is the loaded track an HTTP stream / is it
 * Range-seekable / latest ICY StreamTitle (seq-diffed, see netstream.h) /
 * icy-name station header. */
int  mn_engine_is_stream(mn_engine *engine);
int  mn_engine_nettest_decode(const char *url);   /* --nettest2 harness */
int  mn_engine_stream_seekable(mn_engine *engine);
int  mn_engine_stream_title(mn_engine *engine, char *out, size_t cap,
                            uint32_t *seq);
const char *mn_engine_stream_station(mn_engine *engine);

/*
 * Start (or resume) playback of the loaded track. If already playing this is a
 * no-op returning MN_OK. Returns MN_ERR_STATE if no track is loaded.
 */
mn_result mn_engine_play(mn_engine *engine);

/*
 * Pause playback, holding the current position. No-op (MN_OK) if already paused
 * or stopped.
 */
mn_result mn_engine_pause(mn_engine *engine);

/*
 * Stop playback and reset the play position to 0. The track remains loaded and
 * can be replayed with mn_engine_play().
 */
mn_result mn_engine_stop(mn_engine *engine);

/*
 * Unload the current track entirely: stop the device, tear down the decoder
 * and CLOSE its underlying file handle. Unlike mn_engine_stop() (which keeps
 * the decoder open for replay), this releases the OS-level file lock so the
 * file can be rewritten/replaced (tag writing). No-op (MN_OK) when nothing
 * is loaded. Reload with mn_engine_load(). Control-thread only.
 */
mn_result mn_engine_unload(mn_engine *engine);

/*
 * Copy the UTF-8 path of the currently LOADED track (playing, paused or
 * stopped-but-loaded) into `out` (bounded by n, always NUL-terminated).
 * Returns true when a track is loaded and a non-empty path was written.
 * Control-thread only.
 */
bool mn_engine_loaded_path(mn_engine *engine, char *out, size_t n);

/*
 * Seek to an absolute position, in milliseconds from the start of the track.
 * Clamped to [0, duration]. Valid while playing or paused. Returns MN_ERR_STATE
 * if no track is loaded, MN_ERR_SEEK if the decoder rejected the seek.
 */
mn_result mn_engine_seek_ms(mn_engine *engine, uint64_t position_ms);

/* ------------------------------------------------------------------------- */
/* Volume / gain                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Set linear output volume in [0.0, 1.0]. Values outside the range are clamped.
 * This is a simple post-gain scalar applied to the master mix.
 */
mn_result mn_engine_set_volume(mn_engine *engine, float volume);

/*
 * Pitch-preserved playback speed (WSOLA time-stretch), 0.5..3.0 (clamped).
 * 1.0 fully bypasses the stretcher (bit-perfect path). Stereo only —
 * multichannel passthrough always plays at 1.0. Position/duration/seek are
 * expressed in SOURCE time and are unaffected by the speed.
 */
mn_result mn_engine_set_speed(mn_engine *engine, float speed);

/*
 * One-shot duration hint (ms) for the NEXT mn_engine_load: length is computed
 * from it instead of the decoder's whole-file scan (instant switches into
 * large files). Cleared by the load. 0 disables.
 */
void mn_engine_set_length_hint_ms(mn_engine *engine, int64_t ms);
float     mn_engine_get_speed(const mn_engine *engine);

/*
 * Set output gain in decibels (e.g. for ReplayGain / preamp). Applied in
 * addition to the linear volume. 0.0 dB is unity. Typical range [-60, +12];
 * extreme values are accepted but may clip.
 */
mn_result mn_engine_set_gain_db(mn_engine *engine, float gain_db);

/* ------------------------------------------------------------------------- */
/* Introspection                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Current playback position in milliseconds from the start of the track.
 * Returns 0 if no track is loaded.
 */
uint64_t mn_engine_position_ms(const mn_engine *engine);

/*
 * Total duration of the loaded track in milliseconds. Returns 0 if no track is
 * loaded or the duration is unknown (e.g. non-seekable stream).
 */
uint64_t mn_engine_duration_ms(const mn_engine *engine);

/*
 * Fill *out_format with the current source/output format description.
 * Returns MN_ERR_INVALID on NULL args. If no track is loaded, zero-fills the
 * struct and still returns MN_OK.
 */
mn_result mn_engine_get_format(const mn_engine *engine, mn_audio_format *out_format);

/*
 * Fill *out with the default playback device's native capabilities (name, max
 * bit depth, mix/max sample rates, channel count, native-rate list, exclusive
 * capability). Re-probes the device on each call so a changed default device
 * is picked up; on probe failure the last successful snapshot is returned.
 * Returns true when *out holds valid data, false otherwise (NULL args or no
 * capability information has ever been obtainable).
 */
bool mn_engine_get_caps(mn_engine *engine, mn_audio_caps *out);

/* ------------------------------------------------------------------------- */
/* Output device enumeration / selection                                      */
/* ------------------------------------------------------------------------- */

/* One enumerable playback endpoint. */
typedef struct mn_audio_device {
    char name[128];             /* human-readable endpoint name              */
    bool is_default;            /* true for the system default endpoint      */
} mn_audio_device;

/*
 * Enumerate the available playback devices into out[0..max). Returns the
 * number of devices written (0 on failure/NULL args). The enumeration order is
 * stable between consecutive calls (backend order) and is the index space that
 * mn_engine_select_device() consumes. Control-thread only (serialize with all
 * other engine calls, as usual).
 */
int mn_engine_list_devices(mn_engine *engine, mn_audio_device *out, int max);

/*
 * Switch playback output to the device at `index` (per the most recent
 * enumeration order). Reinitializes the output device on the chosen endpoint
 * at the current pipeline rate, preserving the loaded track and playback
 * position; playback resumes automatically if it was running. On failure the
 * engine falls back to the system default device so audio keeps working, and
 * false is returned. Control-thread only.
 */
bool mn_engine_select_device(mn_engine *engine, int index);

/*
 * Index (in the enumeration order) of the explicitly selected device, or -1
 * when the engine is on the system default endpoint.
 */
int mn_engine_selected_device(const mn_engine *engine);

/* Current transport state. Returns MN_STATE_STOPPED for a NULL engine. */
mn_play_state mn_engine_state(const mn_engine *engine);

/*
 * Returns true once the loaded track has played to its end (end-of-stream) and
 * has not been restarted/seeked away. Cleared by load/play-from-start/seek/stop.
 * Returns false for a NULL engine or when no track is loaded.
 */
bool mn_engine_finished(const mn_engine *engine);

/* ------------------------------------------------------------------------- */
/* Stem-mixer injection                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Attach (or detach) a neural stem session to the playback path. When set and
 * enabled and NOT in passthrough, the engine's audio callback calls
 * mn_stems_mix() for the block currently being rendered and, if it returns a
 * mixed buffer, outputs that instead of the decoded source frames. Otherwise
 * (disabled, passthrough, stems == NULL, or mn_stems_mix() declining) the raw
 * decoded source is played.
 *
 *   stems        borrowed session pointer, or NULL to detach. Not owned.
 *   enabled      master switch for stem routing.
 *   passthrough  when true, always play the source even if stems are ready
 *                (for A/B comparison while inference keeps running).
 *
 * Safe to call from the control thread at any time; the change is picked up by
 * the audio callback on the next block. Passing a NULL engine is a no-op.
 */
void mn_engine_set_stem_source(mn_engine *engine, mn_stems *stems,
                               bool enabled, bool passthrough);

/* ------------------------------------------------------------------------- */
/* DSP chain (10-band EQ / preamp / balance / limiter / master gain)          */
/* ------------------------------------------------------------------------- */
/* All control-thread safe; parameter writes are published to the audio thread
 * without locks and take effect within a block. The chain is a pure passthrough
 * until mn_engine_set_dsp_enabled(engine, 1). */
void mn_engine_set_dsp_enabled(mn_engine *engine, int enabled);
int  mn_engine_get_dsp_enabled(const mn_engine *engine);
void mn_engine_set_eq_enabled(mn_engine *engine, int enabled);
void mn_engine_set_eq_band(mn_engine *engine, uint32_t band, float gain_db);
void mn_engine_set_eq_gains(mn_engine *engine, const float gains_db[10]);
void mn_engine_set_eq_preset(mn_engine *engine, int preset,
                             float out_gains[10], float *out_preamp);
void mn_engine_set_preamp(mn_engine *engine, float preamp_db);
void mn_engine_set_balance(mn_engine *engine, float balance);
void mn_engine_set_limiter(mn_engine *engine, int enabled,
                           float threshold_db, float ceiling_db);
void mn_engine_set_master_gain(mn_engine *engine, float master_gain_db);
void mn_engine_get_eq(const mn_engine *engine, float out_gains[10],
                      float *out_preamp, int *out_enabled);

/* Exclusive (bit-perfect) WASAPI output; restarts the device on change. */
void mn_engine_set_exclusive(mn_engine *engine, int enabled);

/* Audiophile output profile:
 *   native_bits : in exclusive mode, open the device at the source's native
 *                 integer depth (16/24) instead of f32 — true bit-perfect.
 *   rate_cap_hz : 0 = uncapped; else clamp pipeline/device rate (audiobook
 *                 power-saving, e.g. 48000).
 *   bits_cap    : 0 = uncapped; else clamp exclusive depth (e.g. 16).
 * Reloads the current track so it takes effect immediately. */
void mn_engine_set_hifi_profile(mn_engine *engine, int native_bits,
                                uint32_t rate_cap_hz, uint32_t bits_cap);

/* Real-time spectrum analyzer: fills out[] with up to MN_SPECTRUM_BARS
 * log-spaced magnitude bars in [0,1]; returns count written. */
uint32_t mn_engine_get_spectrum(mn_engine *engine, float *out, uint32_t max);

/* ------------------------------------------------------------------------- */
/* Waveform peaks (for an accurate seekbar)                                   */
/* ------------------------------------------------------------------------- */

/*
 * Decode `path` in full and reduce it to `bars` normalized peak magnitudes,
 * one per equal-width time bucket across the whole track. Each out_peaks[i] is
 * the maximum |sample| (across both channels) within bucket i, in [0, 1].
 *
 * Writes min(bars, ...) == bars values into out_peaks[0..bars-1] and returns
 * the number of bars written, or 0 on failure (bad args, decode error). This
 * decodes the entire file once (at 44100 Hz stereo) so it is intended to run
 * on track load / a worker thread, not in a hot loop. Independent of any engine
 * instance and does not touch the output device.
 */
int mn_engine_waveform(const char *path, int bars, float *out_peaks);

/* ------------------------------------------------------------------------- */
/* Decode shim (for the stem-separation engine)                               */
/* ------------------------------------------------------------------------- */

/*
 * Decode an entire audio file to 44100 Hz, deinterleaved stereo float32.
 *
 * Fully decodes and resamples `path` into two newly-allocated planar buffers of
 * `*out_frames` samples each: left channel in *out_L, right channel in *out_R,
 * each sample in the range [-1.0, 1.0]. Mono sources are duplicated to both
 * channels; multi-channel sources are downmixed to stereo.
 *
 * On success returns MN_OK; the caller owns both buffers and must free each with
 * mn_free_samples(). On failure returns a negative mn_result and sets *out_L,
 * *out_R to NULL and *out_frames to 0.
 *
 * This is independent of any engine instance and does not touch the output
 * device — it is a pure file->PCM decode used to feed the ONNX stem model.
 */
mn_result mn_decode_44100_stereo(const char *path,
                                 float **out_L,
                                 float **out_R,
                                 uint64_t *out_frames);

/*
 * Abort predicate for long-running decodes: return true to stop the decode.
 * Polled between decode chunks (roughly every second of audio).
 */
typedef bool (*mn_decode_abort_fn)(void *user);

/*
 * Same as mn_decode_44100_stereo(), but polls `should_abort(user)` between
 * decode chunks so a whole-file decode can be cancelled promptly (used by the
 * stem separator: a track switch cancels the in-flight job without waiting
 * for the full decode). Returns MN_ERR_STATE if aborted. `should_abort` may
 * be NULL (never aborts — identical to mn_decode_44100_stereo).
 */
mn_result mn_decode_44100_stereo_ex(const char *path,
                                    float **out_L,
                                    float **out_R,
                                    uint64_t *out_frames,
                                    mn_decode_abort_fn should_abort,
                                    void *abort_user);

/*
 * Free a sample buffer returned by mn_decode_44100_stereo(). Safe on NULL.
 * Must be used instead of raw free() so allocation stays matched across the
 * module boundary.
 */
void mn_free_samples(float *samples);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_AUDIO_ENGINE_H */
