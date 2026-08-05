/* ==========================================================================
 * stems.h — Monatomic Audio Player
 *
 * Real-time neural stem separation (HTDemucs, 6 stems) via ONNX Runtime CUDA,
 * band-split into 9 user-facing control channels, plus a resident mixing
 * session for the audio path and a disk cache for instant replay.
 *
 * Pipeline:
 *   audio_path --(producer thread)--> HTDemucs (6 neural stems)
 *              --(band-split filterbank)--> 9 control channels
 *              --(published to audio thread)--> mn_stems_mix()
 *
 * The 6 raw HTDemucs stems (drums, bass, other, vocals, guitar, piano) are
 * band-split by a linear-phase filterbank into 9 control channels that the
 * mixer, meters and UI operate on. See mn_stem_channel below.
 *
 * Threading contract:
 *   - create/destroy/start/cancel are called from the control (UI) thread.
 *   - set_gain/set_mute/set_solo/set_force/set_passthrough are lock-free
 *     publishers callable from the control thread; they take effect on the
 *     audio thread within one mix block (values are smoothed).
 *   - neural_active/meters/progress are lock-free readers, safe to poll from
 *     any thread (typically the UI thread once per frame).
 *   - mix() is called ONLY from the audio (miniaudio data) thread.
 *   - A single producer thread owned by the session runs separation; calling
 *     start() again cancels any in-flight job before beginning the new one.
 *
 * All functions are non-blocking except create/destroy and cancel (which
 * joins the producer thread). No allocation or blocking occurs in mix().
 * ========================================================================== */

#ifndef MN_STEMS_H
#define MN_STEMS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Model / segmentation constants (HTDemucs @ 44.1 kHz).
 *
 * MODEL_SR      : sample rate the network operates at.
 * SEGMENT       : frames per inference window (~7.8 s @ 44.1 kHz).
 * OVERLAP       : frames of overlap between consecutive windows; the producer
 *                 cross-fades overlapping regions to hide window boundaries.
 * -------------------------------------------------------------------------- */
#define MN_STEMS_MODEL_SR   44100
#define MN_STEMS_SEGMENT    343980
#define MN_STEMS_OVERLAP    22050

/* Number of raw stems emitted by the HTDemucs network. */
#define MN_STEMS_NEURAL_COUNT   6

/* Number of user-facing control channels after band-splitting. */
#define MN_STEMS_CHANNEL_COUNT  9

/* Audio is processed and mixed as interleaved stereo. */
#define MN_STEMS_CHANNELS       2

/* --------------------------------------------------------------------------
 * Control channels.
 *
 * The 9 band-split control channels, in canonical index order. Indices passed
 * to set_gain/set_mute/set_solo and slots in meters[]/gains map to these.
 * -------------------------------------------------------------------------- */
typedef enum mn_stem_channel {
    MN_STEM_SUB_BASS    = 0,  /* sub-bass band of the bass stem              */
    MN_STEM_BASS        = 1,  /* upper bass band of the bass stem            */
    MN_STEM_VOCALS      = 2,  /* vocals stem, full band                      */
    MN_STEM_LEAD        = 3,  /* lead / melodic mid band of the other stem   */
    MN_STEM_INSTRUMENTS = 4,  /* body mid band of the other stem             */
    MN_STEM_WIDE        = 5,  /* wide stereo-side content of the other stem  */
    MN_STEM_AIR         = 6,  /* high-air band (cymbals/shimmer)             */
    MN_STEM_GUITAR      = 7,  /* guitar stem                                 */
    MN_STEM_PIANO       = 8   /* piano stem                                  */
} mn_stem_channel;

/* --------------------------------------------------------------------------
 * Progress reporting.
 * -------------------------------------------------------------------------- */

/* Which compute backend actually serviced the running / last job. */
typedef enum mn_stems_provider {
    MN_STEMS_PROVIDER_NONE = 0, /* idle / no job has run yet                 */
    MN_STEMS_PROVIDER_CACHE,    /* served from disk cache, no inference      */
    MN_STEMS_PROVIDER_CUDA,     /* ONNX Runtime CUDA execution provider      */
    MN_STEMS_PROVIDER_CPU       /* ONNX Runtime CPU fallback                 */
} mn_stems_provider;

/* Snapshot of separation progress for the current/last track.
 * Read atomically via mn_stems_get_progress(); fields are internally
 * consistent within one snapshot. */
typedef struct mn_stems_progress {
    mn_stems_provider provider;     /* backend servicing the job            */
    float             rt_factor;    /* processing speed vs realtime         */
                                    /*   (>1 == faster than realtime)       */
    float             fraction;     /* completion in [0,1]                  */
    int64_t           separated_ms; /* audio separated so far, milliseconds */
    int64_t           total_ms;     /* total track duration, milliseconds   */
} mn_stems_progress;

/* --------------------------------------------------------------------------
 * Opaque session handle.
 * -------------------------------------------------------------------------- */
typedef struct mn_stems mn_stems;

/* --------------------------------------------------------------------------
 * Lifecycle.
 * -------------------------------------------------------------------------- */

/* Create a resident stem-separation session.
 *   model_path : path to the HTDemucs .onnx model file.
 *   cache_dir  : directory for the disk cache of separated stems; created if
 *                absent. Pass NULL to disable disk caching.
 * Loads the model and initializes the ONNX Runtime environment (CUDA if
 * available, else CPU). Returns NULL on failure (bad path, model load error).
 * The returned session owns one producer thread, created lazily on first
 * start(). Blocking; call off the audio thread. */
mn_stems *mn_stems_create(const char *model_path, const char *cache_dir);

/* Destroy a session: cancels any in-flight job, joins the producer thread,
 * releases the model and all buffers. Safe to pass NULL. Must NOT be called
 * concurrently with mix() — quiesce the audio path first. */
void mn_stems_destroy(mn_stems *s);

/* --------------------------------------------------------------------------
 * Job control (control thread).
 * -------------------------------------------------------------------------- */

/* Begin separating a track. Cancels any prior in-flight job first, then:
 *   - if the disk cache holds stems for track_id, loads them (provider=CACHE),
 *   - otherwise decodes audio_path and runs HTDemucs on the producer thread,
 *     streaming results segment-by-segment into the resident session.
 *
 *   track_id   : stable library id, used as the disk-cache key.
 *   audio_path : source audio file to decode and separate.
 * Returns true if the job was accepted and started, false on bad arguments
 * or if the session is shutting down. Non-blocking: separation proceeds on
 * the producer thread; poll mn_stems_get_progress()/mn_stems_neural_active().
 * Until the new job publishes results the session behaves as passthrough. */
bool mn_stems_start(mn_stems *s, int64_t track_id, const char *audio_path);

/* Cancel the in-flight job (if any) and return the session to passthrough.
 * Blocks until the producer thread has stopped touching published buffers.
 * No-op if idle. Safe to pass NULL. */
void mn_stems_cancel(mn_stems *s);

/* --------------------------------------------------------------------------
 * Live controls (control thread; lock-free, smoothed on the audio thread).
 * Channel indices are mn_stem_channel values in [0, MN_STEMS_CHANNEL_COUNT).
 * Out-of-range indices are ignored.
 * -------------------------------------------------------------------------- */

/* Set the linear gain of a channel (1.0 == unity). Values are clamped to a
 * sane range and smoothed to avoid zipper noise. */
void mn_stems_set_gain(mn_stems *s, int channel, float gain);

/* Mute (on=true) or unmute a channel. */
void mn_stems_set_mute(mn_stems *s, int channel, bool on);

/* Solo a channel: silences all other non-soloed channels. Passing a valid
 * channel adds it to the solo set; the mixer plays only soloed channels while
 * any solo is active. Pass a negative channel to clear the entire solo set. */
void mn_stems_set_solo(mn_stems *s, int channel);

/* Set one channel's solo independently (multi-solo: several may be soloed at
 * once; un-soloing one leaves the rest). channel<0 or out-of-range is ignored. */
void mn_stems_set_solo_state(mn_stems *s, int channel, bool soloed);

/* Force eager neural mixing: engage as soon as the separation frontier leads
 * the playhead by ~1 s (instead of the default ~10 s safety margin) and render
 * silence for blocks past the frontier instead of falling back to the source.
 * For A/B and debugging; normal playback should leave this off. */
void mn_stems_set_force(mn_stems *s, bool on);

/* Force passthrough: when on, mix() copies source frames through unchanged and
 * ignores neural results regardless of availability. Overrides set_force. */
void mn_stems_set_passthrough(mn_stems *s, bool on);

/* --------------------------------------------------------------------------
 * Published state (any thread; lock-free readers).
 * -------------------------------------------------------------------------- */

/* True when neural stems for the current track are available and the mixer is
 * (or would be) band-split mixing rather than passing audio through. */
/* Runtime cap for the on-disk stems cache (Settings -> Storage). Oldest
 * cached tracks are evicted past the cap. Clamped to >= 256 MB. */
void mn_stems_set_cache_cap_bytes(int64_t bytes);

bool mn_stems_neural_active(const mn_stems *s);

/* Copy the current per-channel meter levels into out[MN_STEMS_CHANNEL_COUNT].
 * Levels are smoothed linear magnitudes in [0,1] (post-gain, pre-solo/mute
 * masking is NOT applied — meters reflect content energy). out must be
 * non-NULL and hold at least MN_STEMS_CHANNEL_COUNT floats. */
void mn_stems_get_meters(const mn_stems *s, float out[MN_STEMS_CHANNEL_COUNT]);

/* Copy the current progress snapshot. out must be non-NULL. */
void mn_stems_get_progress(const mn_stems *s, mn_stems_progress *out);

/* --------------------------------------------------------------------------
 * Offline separation for EXPORT (control thread; blocking).
 * -------------------------------------------------------------------------- */

/* Separate `audio_path` to completion (cache fast-path if the track was
 * already fully separated, else full inference) and BLOCK until done. With no
 * live playhead the GPU duty-cycle throttle self-disables, so it runs at full
 * speed. `abort` (may be NULL) is polled periodically; return non-zero to
 * cancel. Returns true when the 9 channels are fully populated and readable
 * via mn_stems_export_channel(). The session must not be used for live
 * playback while this runs. */
bool mn_stems_separate_sync(mn_stems *s, int64_t track_id,
                            const char *audio_path,
                            int (*abort)(void *ctx), void *abort_ctx);

/* After a successful mn_stems_separate_sync, copy channel `idx`
 * (0..MN_STEMS_CHANNEL_COUNT-1) into a freshly malloc'd interleaved-stereo
 * float32 buffer. *out_frames receives the frame count (each frame is
 * MN_STEMS_CHANNELS floats). Caller frees *out with free(). Returns false if
 * no separation result is available. Safe to call from the control thread. */
bool mn_stems_export_channel(mn_stems *s, int idx,
                             float **out, uint64_t *out_frames);

/* --------------------------------------------------------------------------
 * Audio path (audio thread ONLY).
 * -------------------------------------------------------------------------- */

/* Produce the mixed output for the block of audio starting at absolute source
 * frame `frame` (in MODEL_SR frames from the start of the track).
 *
 *   frame  : absolute start frame of this block within the track.
 *   frames : number of interleaved-stereo frames to produce.
 *   dest   : output buffer of frames * MN_STEMS_CHANNELS floats (interleaved
 *            L/R), written by this call.
 *
 * Behavior (PROGRESSIVE): separation results are served as soon as they are
 * published — mix() renders any fully-published prefix of the track while the
 * producer keeps separating ahead:
 *   - Returns true and writes the band-split neural mix when the block lies
 *     within the published prefix, passthrough is not forced, and the session
 *     has ENGAGED for this track. Engagement happens once the separation
 *     frontier leads the block by ~10 s (~1 s in force mode) or the track is
 *     fully separated; once engaged the session stays engaged for the whole
 *     track (no audible renderer flip-flop).
 *   - Returns false and leaves dest untouched when the caller should use its
 *     own passthrough source (block beyond the separation frontier, engage
 *     margin not yet reached, passthrough forced, or session idle).
 *
 * Never allocates or blocks. All live-control changes are applied here with
 * per-block smoothing. */
bool mn_stems_mix(mn_stems *s, int64_t frame, uint32_t frames, float *dest);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_STEMS_H */
