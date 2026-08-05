/*
 * dsp.h - Monatomic Music Player DSP chain public API.
 *
 * Real-time-safe digital signal processing chain intended to run inside the
 * audio callback. The chain processes interleaved 32-bit float PCM in place:
 *
 *     preamp -> 10-band graphic EQ -> channel mapping
 *     (upmix / downmix / balance) -> soft-clip limiter
 *
 * Design contract:
 *   - mn_dsp_process() is real-time safe: no heap allocation, no locks, no
 *     syscalls, no unbounded loops. All buffers are preallocated at
 *     mn_dsp_create()/mn_dsp_configure() time.
 *   - Configuration changes (EQ gains, presets, layout, balance, limiter)
 *     from a control thread are applied via the mn_dsp_set_* functions, which
 *     are safe to call concurrently with mn_dsp_process() (they publish new
 *     coefficients atomically; parameters ramp to avoid zipper noise).
 *   - Sample format is always MN_DSP_FORMAT: interleaved float, range nominally
 *     [-1, 1] (values outside are handled by the limiter).
 *
 * All functions with an mn_ prefix; macros with MN_ prefix.
 */
#ifndef MN_DSP_H
#define MN_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* ------------------------------------------------------------------------- */

/** Number of graphic EQ bands. */
#define MN_DSP_EQ_BANDS 10

/** Maximum number of channels the chain will ever process (7.1). */
#define MN_DSP_MAX_CHANNELS 8

/** Minimum / maximum per-band and preamp gain, in decibels. */
#define MN_DSP_GAIN_MIN_DB (-24.0f)
#define MN_DSP_GAIN_MAX_DB (24.0f)

/** Minimum / maximum balance value (left = -1, center = 0, right = +1). */
#define MN_DSP_BALANCE_MIN (-1.0f)
#define MN_DSP_BALANCE_MAX (1.0f)

/* ------------------------------------------------------------------------- */
/* Enumerations                                                               */
/* ------------------------------------------------------------------------- */

/** Result / error codes returned by DSP API functions. */
typedef enum mn_dsp_result {
    MN_DSP_OK = 0,             /**< Success. */
    MN_DSP_ERR_INVALID_ARG,    /**< NULL pointer or out-of-range argument. */
    MN_DSP_ERR_UNSUPPORTED,    /**< Unsupported channel count / layout combo. */
    MN_DSP_ERR_OUT_OF_MEMORY,  /**< Allocation failed at create/configure. */
    MN_DSP_ERR_NOT_CONFIGURED  /**< process() called before configure(). */
} mn_dsp_result;

/**
 * Speaker channel layouts the chain can target on output. Input is expected to
 * be mono or stereo; the chain upmixes/downmixes to the configured output
 * layout. Channel ordering follows the standard interleave order used by
 * miniaudio / WAV:
 *   MONO   : [C]
 *   STEREO : [L, R]
 *   5.1    : [L, R, C, LFE, RL, RR]
 *   7.1    : [L, R, C, LFE, RL, RR, SL, SR]
 */
typedef enum mn_dsp_layout {
    MN_DSP_LAYOUT_MONO = 0,
    MN_DSP_LAYOUT_STEREO,
    MN_DSP_LAYOUT_5_1,
    MN_DSP_LAYOUT_7_1
} mn_dsp_layout;

/** Built-in EQ presets. Selecting one overwrites all 10 band gains + preamp. */
typedef enum mn_dsp_eq_preset {
    MN_DSP_EQ_PRESET_FLAT = 0, /**< All bands 0 dB, preamp 0 dB. */
    MN_DSP_EQ_PRESET_ACOUSTIC,
    MN_DSP_EQ_PRESET_BASS_BOOST,
    MN_DSP_EQ_PRESET_BASS_REDUCE,
    MN_DSP_EQ_PRESET_CLASSICAL,
    MN_DSP_EQ_PRESET_DANCE,
    MN_DSP_EQ_PRESET_ELECTRONIC,
    MN_DSP_EQ_PRESET_HIP_HOP,
    MN_DSP_EQ_PRESET_JAZZ,
    MN_DSP_EQ_PRESET_LOUDNESS,
    MN_DSP_EQ_PRESET_LOUNGE,
    MN_DSP_EQ_PRESET_POP,
    MN_DSP_EQ_PRESET_ROCK,
    MN_DSP_EQ_PRESET_TREBLE_BOOST,
    MN_DSP_EQ_PRESET_VOCAL,
    MN_DSP_EQ_PRESET_COUNT      /**< Sentinel: number of presets. */
} mn_dsp_eq_preset;

/* ------------------------------------------------------------------------- */
/* Public constant tables                                                     */
/* ------------------------------------------------------------------------- */

/**
 * ISO center frequencies (Hz) for the 10 EQ bands, index 0..MN_DSP_EQ_BANDS-1:
 * { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 }.
 * Defined in dsp.c; use mn_dsp_band_frequency() to read at runtime.
 */
extern const float MN_DSP_EQ_FREQUENCIES[MN_DSP_EQ_BANDS];

/* ------------------------------------------------------------------------- */
/* Configuration and parameter structs                                        */
/* ------------------------------------------------------------------------- */

/**
 * Immutable-at-runtime stream configuration. Passed to mn_dsp_create() or
 * mn_dsp_configure(). Changing sample rate or channel counts requires a
 * reconfigure (which is NOT real-time safe and must not run in the callback).
 */
typedef struct mn_dsp_config {
    uint32_t      sample_rate;    /**< e.g. 44100, 48000. Must be > 0. */
    uint32_t      in_channels;    /**< 1 or 2 (mono/stereo source). */
    mn_dsp_layout out_layout;     /**< Desired output speaker layout. */
    uint32_t      max_frames;     /**< Max frames per process() call (buffer sizing). */
} mn_dsp_config;

/**
 * Full snapshot of user-facing DSP parameters. Can be read/written as a unit
 * for save/restore of state. Individual setters exist for low-overhead UI use.
 */
typedef struct mn_dsp_params {
    /* Equalizer. */
    int   eq_enabled;                    /**< Non-zero to run the EQ stage. */
    float preamp_db;                     /**< Global pre-EQ gain, clamped to range. */
    float eq_gains_db[MN_DSP_EQ_BANDS];  /**< Per-band peaking gain in dB. */

    /* Stereo balance (applies to L/R pair before upmix). */
    float balance;                       /**< -1 left .. 0 center .. +1 right. */

    /* Limiter. */
    int   limiter_enabled;               /**< Non-zero to run the soft-clip limiter. */
    float limiter_threshold_db;          /**< Onset of soft clipping, <= 0 dB. */
    float limiter_ceiling_db;            /**< Hard output ceiling, <= 0 dB. */

    /* Master. */
    float master_gain_db;                /**< Post-limiter makeup/attenuation. */
} mn_dsp_params;

/* Opaque DSP instance. Allocated by mn_dsp_create(). */
typedef struct mn_dsp mn_dsp;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Create and configure a DSP instance. Allocates all internal buffers based on
 * cfg->max_frames and channel counts. NOT real-time safe.
 *
 * @param cfg      Stream configuration (must be non-NULL, validated).
 * @param out_dsp  Receives the new instance on success.
 * @return MN_DSP_OK, or an error code; *out_dsp is NULL on failure.
 */
mn_dsp_result mn_dsp_create(const mn_dsp_config *cfg, mn_dsp **out_dsp);

/**
 * Reconfigure an existing instance for a new stream (sample rate / channels /
 * layout / max_frames). May reallocate internal buffers. NOT real-time safe;
 * the caller must ensure mn_dsp_process() is not running concurrently.
 * Parameter state (EQ, balance, limiter) is preserved across reconfigure.
 */
mn_dsp_result mn_dsp_configure(mn_dsp *dsp, const mn_dsp_config *cfg);

/** Destroy an instance and free all resources. Safe on NULL. */
void mn_dsp_destroy(mn_dsp *dsp);

/**
 * Reset all transient/internal state (biquad delay lines, limiter envelope,
 * parameter ramps snap to target). Does not change parameters or config.
 * Real-time safe. Call on seek / stream discontinuity to avoid clicks.
 */
void mn_dsp_reset(mn_dsp *dsp);

/* ------------------------------------------------------------------------- */
/* Processing (real-time safe)                                                */
/* ------------------------------------------------------------------------- */

/**
 * Process one block of audio in place. Real-time safe.
 *
 * The buffer holds `frames` frames of interleaved float samples. On input the
 * layout matches cfg->in_channels; on output it matches the configured output
 * layout's channel count (see mn_dsp_out_channels()). The buffer must be large
 * enough to hold max(in_channels, out_channels) * frames samples.
 *
 * @param dsp       Instance (must be configured).
 * @param buffer    Interleaved float PCM, modified in place.
 * @param frames    Number of frames in this block (<= cfg->max_frames).
 * @param channels  Channel count of `buffer` as passed in (must equal the
 *                  configured in_channels; used for validation).
 * @return MN_DSP_OK, or MN_DSP_ERR_INVALID_ARG on bad args, or
 *         MN_DSP_ERR_NOT_CONFIGURED. On error the buffer is left unchanged.
 */
mn_dsp_result mn_dsp_process(mn_dsp *dsp, float *buffer, uint32_t frames,
                             uint32_t channels);

/* ------------------------------------------------------------------------- */
/* Parameter control (thread-safe wrt process())                             */
/* ------------------------------------------------------------------------- */

/** Atomically apply a full parameter snapshot. Values are clamped to range. */
mn_dsp_result mn_dsp_set_params(mn_dsp *dsp, const mn_dsp_params *params);

/** Read the current parameter snapshot into *out_params. */
mn_dsp_result mn_dsp_get_params(const mn_dsp *dsp, mn_dsp_params *out_params);

/* --- Equalizer --- */

/** Enable/disable the EQ stage without changing band gains. */
mn_dsp_result mn_dsp_set_eq_enabled(mn_dsp *dsp, int enabled);

/**
 * Set a single band's gain (dB), clamped to [MN_DSP_GAIN_MIN_DB,
 * MN_DSP_GAIN_MAX_DB]. band in [0, MN_DSP_EQ_BANDS).
 */
mn_dsp_result mn_dsp_set_eq_band(mn_dsp *dsp, uint32_t band, float gain_db);

/** Set all 10 band gains at once from an array of length MN_DSP_EQ_BANDS. */
mn_dsp_result mn_dsp_set_eq_gains(mn_dsp *dsp, const float gains_db[MN_DSP_EQ_BANDS]);

/** Set the global preamp gain (dB), clamped to range. */
mn_dsp_result mn_dsp_set_preamp(mn_dsp *dsp, float preamp_db);

/**
 * Apply a built-in preset: overwrites all band gains and preamp, and enables
 * the EQ. preset in [0, MN_DSP_EQ_PRESET_COUNT).
 */
mn_dsp_result mn_dsp_set_eq_preset(mn_dsp *dsp, mn_dsp_eq_preset preset);

/** Center frequency (Hz) of a band, or 0 on invalid index. */
float mn_dsp_band_frequency(uint32_t band);

/** Human-readable preset name (static string), or NULL on invalid index. */
const char *mn_dsp_preset_name(mn_dsp_eq_preset preset);

/* --- Balance --- */

/** Set stereo balance, clamped to [MN_DSP_BALANCE_MIN, MN_DSP_BALANCE_MAX]. */
mn_dsp_result mn_dsp_set_balance(mn_dsp *dsp, float balance);

/* --- Limiter / master --- */

/** Enable/disable the soft-clip limiter stage. */
mn_dsp_result mn_dsp_set_limiter_enabled(mn_dsp *dsp, int enabled);

/**
 * Configure the soft-clip limiter. threshold_db is the soft-knee onset and
 * ceiling_db is the absolute output ceiling; both <= 0 dB and ceiling >=
 * threshold. Out-of-range values are clamped.
 */
mn_dsp_result mn_dsp_set_limiter(mn_dsp *dsp, float threshold_db, float ceiling_db);

/** Set post-limiter master gain (dB). */
mn_dsp_result mn_dsp_set_master_gain(mn_dsp *dsp, float master_gain_db);

/* ------------------------------------------------------------------------- */
/* Introspection                                                              */
/* ------------------------------------------------------------------------- */

/** Number of channels the configured output layout produces (1,2,6,8). */
uint32_t mn_dsp_out_channels(const mn_dsp *dsp);

/** Number of channels for a given layout, or 0 if unknown. */
uint32_t mn_dsp_layout_channels(mn_dsp_layout layout);

/** Configured output layout of the instance. */
mn_dsp_layout mn_dsp_get_layout(const mn_dsp *dsp);

/** Human-readable string for a result code (static, never NULL). */
const char *mn_dsp_result_string(mn_dsp_result r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_DSP_H */
