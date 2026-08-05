/* ==========================================================================
 * stems.c — Monatomic Music Player
 *
 * Implementation of stems.h: real-time neural stem separation (HTDemucs, 6
 * stems) via ONNX Runtime (CUDA if available, else CPU), band-split into 9
 * user-facing control channels, with a resident mixing session for the audio
 * path and a disk cache for instant replay.
 *
 * Architecture
 * ------------
 *   - One resident ONNX session (env + session + memory info) lives for the
 *     lifetime of the mn_stems handle. Loading the model is expensive, so it is
 *     done once in mn_stems_create().
 *   - A single producer thread, created lazily on the first mn_stems_start(),
 *     runs the separation job: decode -> segment -> infer -> band-split ->
 *     publish. Restarting cancels the in-flight job before beginning a new one.
 *   - The audio thread calls mn_stems_mix() only. It reads published, immutable
 *     PCM channel buffers (guarded by an atomic "published frame count") and
 *     applies per-channel smoothed gain plus mute/solo masking with no locks,
 *     no allocation and no blocking.
 *
 * Band-split (6 neural stems -> 9 control channels)
 * -------------------------------------------------
 *   bass   -> MN_STEM_SUB_BASS (90 Hz low-pass) + MN_STEM_BASS (remainder)
 *   drums  -> MN_STEM_LEAD (body, < 9 kHz) + MN_STEM_AIR (high shimmer)
 *   other  -> MN_STEM_INSTRUMENTS (mid) + MN_STEM_WIDE (stereo side)
 *   vocals -> MN_STEM_VOCALS (direct)
 *   guitar -> MN_STEM_GUITAR (direct)
 *   piano  -> MN_STEM_PIANO  (direct)
 *
 * Disk cache format (little-endian, native float32)
 * -------------------------------------------------
 *   [uint32 magic]         MN_STEMS_CACHE_MAGIC
 *   [uint32 stem_count]    == MN_STEMS_CHANNEL_COUNT
 *   [uint64 frames]        number of interleaved-stereo frames per channel
 *   then stem_count blocks, each: frames * MN_STEMS_CHANNELS float32,
 *   interleaved L/R.
 * ========================================================================== */

#include "stems.h"
#include "audio_engine.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "onnxruntime_c_api.h"
#include "onnxruntime_run_options_config_keys.h"

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <sys/stat.h>
#endif

/* --------------------------------------------------------------------------
 * Local constants.
 * -------------------------------------------------------------------------- */

/* Disk-cache header magic ("MNS1" little-endian). */
#define MN_STEMS_CACHE_MAGIC  0x31534E4Du

/* Gain smoothing coefficient per mix block (one-pole). Closer to 1 == slower. */
#define MN_STEMS_GAIN_SMOOTH  0.90f

/* Meter ballistics (per mix block). */
#define MN_STEMS_METER_ATTACK 0.30f
#define MN_STEMS_METER_DECAY  0.05f

/* Clamp for user gains. */
#define MN_STEMS_GAIN_MIN     0.0f
#define MN_STEMS_GAIN_MAX     4.0f

/* Crossover / filter cutoffs (Hz) used by the band-split filterbank. */
#define MN_STEMS_SUB_HZ       90.0f    /* bass sub/upper split                */
#define MN_STEMS_AIR_HZ       9000.0f  /* drums body/air split                */

/* --------------------------------------------------------------------------
 * Progressive-playback engagement (Android-model parity).
 *
 * mix() serves any fully-published PREFIX of the track while the producer
 * keeps separating ahead. To avoid an audible renderer swap right after it
 * commits, it only ENGAGES once the separation frontier leads the playhead by
 * a safety margin (10 s normally, 1 s when the user force-enables); once
 * engaged it stays engaged for the rest of the track (blocks beyond the
 * frontier still fall back to the decoded source).
 * -------------------------------------------------------------------------- */
#define MN_STEMS_ENGAGE_MARGIN       (10 * MN_STEMS_MODEL_SR) /* frames */
#define MN_STEMS_ENGAGE_MARGIN_FORCE (1 * MN_STEMS_MODEL_SR)  /* frames */

/* --------------------------------------------------------------------------
 * GPU duty-cycle cap.
 *
 * Once the separation frontier comfortably leads the playhead
 * (MN_STEMS_THROTTLE_LEAD), the producer sleeps between segments so inference
 * occupies only ~MN_STEMS_GPU_DUTY of wall time, leaving GPU headroom for
 * other applications. While the frontier is close to the playhead (or nothing
 * is consuming the mix, e.g. the --sep benchmark) it runs at full speed so
 * realtime playback never starves. Constant for now; a settings knob later.
 * -------------------------------------------------------------------------- */
#define MN_STEMS_GPU_DUTY       0.65
#define MN_STEMS_THROTTLE_LEAD  (30 * MN_STEMS_MODEL_SR)      /* frames */

/* CUDA arena cap (bytes). Balances speed vs resident VRAM: the default
 * (uncapped, kNextPowerOfTwo) ballooned to ~7.3 GB resident for a 136 MB fp16
 * model. 4 GiB is the measured floor for HTDemucs 6s: 3 GiB starves the
 * iSTFT ConvTranspose workspace mid-track (BFCArena alloc failure), 4 GiB
 * runs full speed; kSameAsRequested + post-job arena shrinkage then returns
 * idle VRAM while the session stays resident. */
#define MN_STEMS_CUDA_MEM_LIMIT "4294967296"

/* --------------------------------------------------------------------------
 * Cross-platform atomics / threading shims.
 *
 * We keep the surface tiny: a 64-bit and a 32-bit atomic load/store with
 * acquire/release ordering, plus a thread handle and a mutex for control-side
 * serialization. On MSVC we use the Interlocked/volatile intrinsics; on POSIX
 * we use C11 <stdatomic.h> and pthreads.
 * -------------------------------------------------------------------------- */

#ifdef _WIN32

typedef volatile LONG64 mn_atomic64;
typedef volatile LONG   mn_atomic32;

static inline int64_t mn_atomic64_load(const mn_atomic64 *p) {
    /* Aligned 64-bit reads are atomic on x64; MemoryBarrier gives ordering. */
    int64_t v = (int64_t)*p;
    MemoryBarrier();
    return v;
}
static inline void mn_atomic64_store(mn_atomic64 *p, int64_t v) {
    MemoryBarrier();
    *p = (LONG64)v;
    MemoryBarrier();
}
static inline int32_t mn_atomic32_load(const mn_atomic32 *p) {
    int32_t v = (int32_t)*p;
    MemoryBarrier();
    return v;
}
static inline void mn_atomic32_store(mn_atomic32 *p, int32_t v) {
    MemoryBarrier();
    *p = (LONG)v;
    MemoryBarrier();
}

typedef CRITICAL_SECTION mn_mutex;
static inline void mn_mutex_init(mn_mutex *m)    { InitializeCriticalSection(m); }
static inline void mn_mutex_destroy(mn_mutex *m) { DeleteCriticalSection(m); }
static inline void mn_mutex_lock(mn_mutex *m)    { EnterCriticalSection(m); }
static inline void mn_mutex_unlock(mn_mutex *m)  { LeaveCriticalSection(m); }
static inline int  mn_mutex_trylock(mn_mutex *m) { return TryEnterCriticalSection(m) ? 1 : 0; }

typedef HANDLE mn_thread;

#else /* POSIX */

#include <stdatomic.h>

typedef _Atomic int64_t mn_atomic64;
typedef _Atomic int32_t mn_atomic32;

static inline int64_t mn_atomic64_load(const mn_atomic64 *p) {
    return atomic_load_explicit((mn_atomic64 *)p, memory_order_acquire);
}
static inline void mn_atomic64_store(mn_atomic64 *p, int64_t v) {
    atomic_store_explicit(p, v, memory_order_release);
}
static inline int32_t mn_atomic32_load(const mn_atomic32 *p) {
    return atomic_load_explicit((mn_atomic32 *)p, memory_order_acquire);
}
static inline void mn_atomic32_store(mn_atomic32 *p, int32_t v) {
    atomic_store_explicit(p, v, memory_order_release);
}

typedef pthread_mutex_t mn_mutex;
static inline void mn_mutex_init(mn_mutex *m)    { pthread_mutex_init(m, NULL); }
static inline void mn_mutex_destroy(mn_mutex *m) { pthread_mutex_destroy(m); }
static inline void mn_mutex_lock(mn_mutex *m)    { pthread_mutex_lock(m); }
static inline void mn_mutex_unlock(mn_mutex *m)  { pthread_mutex_unlock(m); }
static inline int  mn_mutex_trylock(mn_mutex *m) { return pthread_mutex_trylock(m) == 0; }

typedef pthread_t mn_thread;

#endif

/* --------------------------------------------------------------------------
 * Session state.
 * -------------------------------------------------------------------------- */

/* Per-channel live control + smoothing state (audio-thread owned smoothing). */
typedef struct mn_channel_ctl {
    mn_atomic32 target_gain_bits; /* user target gain, float bit-cast        */
    mn_atomic32 muted;            /* 0/1                                      */
    mn_atomic32 soloed;           /* 0/1                                      */
    float       cur_gain;         /* smoothed gain (audio thread only)        */
    mn_atomic32 meter_bits;       /* smoothed meter magnitude, float bit-cast */
} mn_channel_ctl;

struct mn_stems {
    /* ---- ONNX Runtime resident state ---- */
    const OrtApi   *ort;          /* API function table (never freed)         */
    OrtEnv         *env;          /* runtime environment                      */
    OrtSessionOptions *sopts;     /* session options (kept for reference)     */
    OrtSession     *session;      /* loaded HTDemucs session                  */
    OrtMemoryInfo  *mem_info;     /* CPU memory info for input/output tensors */
    OrtAllocator   *allocator;    /* default allocator (not owned)            */
    char           *input_name;   /* queried model input name (allocator mem) */
    char           *output_name;  /* queried model output name(allocator mem) */
    OrtRunOptions  *run_shrink;   /* run opts: shrink device arena (last seg) */
    mn_stems_provider active_provider; /* provider selected at create time    */

    /* ---- Configuration ---- */
    char *cache_dir;              /* disk-cache directory, or NULL            */

    /* ---- Published neural results (immutable once published) ---- */
    /* Nine planar interleaved-stereo channel buffers, each channel_cap frames
     * of MN_STEMS_CHANNELS floats. Allocated per job, owned by the session,
     * written only by the producer thread before it advances published_frames.*/
    float   *chan[MN_STEMS_CHANNEL_COUNT];
    uint64_t channel_cap;         /* frames allocated per channel buffer      */
    mn_atomic64 published_frames; /* frames safe for the audio thread to read */
    mn_atomic64 total_frames;     /* total frames in the current track        */
    mn_atomic32 neural_ready;     /* 0/1: any neural data available           */
    mn_atomic32 engaged;          /* 0/1: mix() committed to neural (track)   */

    /* ---- Playhead hint (written by the audio thread in mix()) ---- */
    mn_atomic64 last_mix_frame;   /* most recent block start handed to mix()  */
    mn_atomic32 mix_seen;         /* 1 once mix() was called for this job     */

    /* ---- Live controls ---- */
    mn_channel_ctl ctl[MN_STEMS_CHANNEL_COUNT];
    mn_atomic32 force_neural;     /* 0/1                                       */
    mn_atomic32 passthrough;      /* 0/1                                       */

    /* ---- Progress snapshot (published field-by-field, read via getter) ---- */
    mn_atomic32 prog_provider;    /* mn_stems_provider                        */
    mn_atomic32 prog_rt_bits;     /* rt_factor, float bit-cast                */
    mn_atomic32 prog_frac_bits;   /* fraction, float bit-cast                 */
    mn_atomic64 prog_sep_ms;      /* separated_ms                             */
    mn_atomic64 prog_total_ms;    /* total_ms                                 */

    /* ---- Producer thread + job control ---- */
    /* Guards the LIFETIME of the chan[] buffers against the audio thread.
     * Held (blocking) around every free/realloc of the channel buffers;
     * TRY-acquired by mn_stems_mix — if the control side is mid-swap the
     * mixer simply reports "no neural output" for that block and the engine
     * plays the decoded source instead. This makes the free-while-mixing
     * use-after-free (the track-switch crash) structurally impossible while
     * never blocking the audio callback. */
    mn_mutex   buf_mutex;
    mn_mutex   job_mutex;         /* serializes start/cancel on control thread */
    mn_thread  thread;            /* producer thread handle                   */
    int        thread_valid;      /* 1 if `thread` holds a live/joinable thr  */
    mn_atomic32 cancel_flag;      /* request producer to stop early           */
    mn_atomic32 shutting_down;    /* session is being destroyed               */

    /* Job parameters handed to the producer thread. */
    int64_t  job_track_id;
    char    *job_audio_path;      /* owned copy for the producer thread       */
};

/* --------------------------------------------------------------------------
 * float <-> int32 bit-cast helpers for lock-free float publishing.
 * -------------------------------------------------------------------------- */
static inline int32_t mn_f2i(float f) {
    int32_t i;
    memcpy(&i, &f, sizeof i);
    return i;
}
static inline float mn_i2f(int32_t i) {
    float f;
    memcpy(&f, &i, sizeof f);
    return f;
}

static inline float mn_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --------------------------------------------------------------------------
 * ONNX status helper: returns 1 on error (and releases the status), 0 on OK.
 * -------------------------------------------------------------------------- */
static int mn_ort_failed(const OrtApi *ort, OrtStatus *status) {
    if (status == NULL) {
        return 0;
    }
    ort->ReleaseStatus(status);
    return 1;
}

/* Like mn_ort_failed but logs the ORT error message to stderr with a context
 * label, so provider/session failures are visible instead of silent. */
static int mn_ort_failed_log(const OrtApi *ort, OrtStatus *status,
                             const char *what) {
    if (status == NULL) {
        return 0;
    }
    fprintf(stderr, "[stems] %s failed: %s\n",
            what ? what : "ORT call", ort->GetErrorMessage(status));
    fflush(stderr);
    ort->ReleaseStatus(status);
    return 1;
}

/* Portable millisecond sleep (producer-thread pacing only). */
static void mn_sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Monotonic wall clock in milliseconds (for real rt-factor reporting). */
static int64_t mn_mono_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

/* --------------------------------------------------------------------------
 * Path -> ORTCHAR_T conversion for CreateSession.
 *
 * On Windows ORTCHAR_T is wchar_t; we convert UTF-8 to UTF-16. On POSIX it is
 * char and we pass the path straight through. The returned buffer must be freed
 * by the caller with free().
 * -------------------------------------------------------------------------- */
static ORTCHAR_T *mn_to_ortchar(const char *utf8) {
    if (utf8 == NULL) {
        return NULL;
    }
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (w == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    return w;
#else
    size_t n = strlen(utf8) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, utf8, n);
    }
    return p;
#endif
}

/* --------------------------------------------------------------------------
 * Create the directory (best-effort, non-recursive). Safe if it exists.
 * -------------------------------------------------------------------------- */
static void mn_mkdir(const char *dir) {
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0777);
#endif
}

/* --------------------------------------------------------------------------
 * Build the disk-cache file path for a track id into `out` (size bytes).
 * Returns 0 on success, -1 if caching is disabled or the buffer is too small.
 * -------------------------------------------------------------------------- */
static int mn_cache_path(const mn_stems *s, int64_t track_id,
                         char *out, size_t out_sz) {
    if (s == NULL || s->cache_dir == NULL || out == NULL) {
        return -1;
    }
    int n = snprintf(out, out_sz, "%s%c%lld.mnstems",
                     s->cache_dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     (long long)track_id);
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Simple one-pole low-pass IIR applied in place to an interleaved-stereo
 * planar buffer. `dst` receives the low-passed signal; if `hi` is non-NULL it
 * receives (src - low) so the caller gets a complementary high band.
 *
 *   coeff = dt / (rc + dt), with rc = 1/(2*pi*fc).
 * Channels are filtered independently to preserve the stereo field.
 * -------------------------------------------------------------------------- */
static void mn_lowpass_split(const float *src, float *dst, float *hi,
                             uint64_t frames, float fc, float sr) {
    if (src == NULL || dst == NULL || frames == 0) {
        return;
    }
    float dt = 1.0f / sr;
    float rc = 1.0f / (2.0f * 3.14159265358979323846f * fc);
    float a  = dt / (rc + dt);
    float lp_l = 0.0f, lp_r = 0.0f;
    for (uint64_t i = 0; i < frames; ++i) {
        float xl = src[i * 2 + 0];
        float xr = src[i * 2 + 1];
        lp_l += a * (xl - lp_l);
        lp_r += a * (xr - lp_r);
        dst[i * 2 + 0] = lp_l;
        dst[i * 2 + 1] = lp_r;
        if (hi != NULL) {
            hi[i * 2 + 0] = xl - lp_l;
            hi[i * 2 + 1] = xr - lp_r;
        }
    }
}

/* --------------------------------------------------------------------------
 * Split an interleaved-stereo buffer into a mid (mono-summed, equal in both
 * channels) part and a side (stereo-difference) part.
 *   mid[L]=mid[R]=(L+R)/2 ; side[L]=(L-R)/2, side[R]=-(L-R)/2.
 * -------------------------------------------------------------------------- */
static void mn_midside_split(const float *src, float *mid, float *side,
                             uint64_t frames) {
    if (src == NULL || frames == 0) {
        return;
    }
    for (uint64_t i = 0; i < frames; ++i) {
        float l = src[i * 2 + 0];
        float r = src[i * 2 + 1];
        float m = 0.5f * (l + r);
        float sd = 0.5f * (l - r);
        if (mid != NULL) {
            mid[i * 2 + 0] = m;
            mid[i * 2 + 1] = m;
        }
        if (side != NULL) {
            side[i * 2 + 0] =  sd;
            side[i * 2 + 1] = -sd;
        }
    }
}

/* Copy an interleaved-stereo region straight through. */
static void mn_copy_stereo(const float *src, float *dst, uint64_t frames) {
    if (src != NULL && dst != NULL && frames != 0) {
        memcpy(dst, src, (size_t)frames * MN_STEMS_CHANNELS * sizeof(float));
    }
}

/* --------------------------------------------------------------------------
 * Free all published channel buffers and reset published state. Called with no
 * producer running (create failure, cancel completion, or destroy). The
 * audio thread may be INSIDE mn_stems_mix reading these buffers, so:
 *   1. retract the publication first (published/ready -> 0) so no NEW mix
 *      block starts consuming them, then
 *   2. take buf_mutex — waits out any mix block already in flight — and only
 *      then free the memory.
 * -------------------------------------------------------------------------- */
static void mn_free_channels(mn_stems *s) {
    mn_atomic32_store(&s->neural_ready, 0);
    mn_atomic32_store(&s->engaged, 0);
    mn_atomic64_store(&s->published_frames, 0);
    mn_atomic64_store(&s->total_frames, 0);

    mn_mutex_lock(&s->buf_mutex);
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        free(s->chan[c]);
        s->chan[c] = NULL;
    }
    s->channel_cap = 0;
    mn_mutex_unlock(&s->buf_mutex);
}

/* Allocate the nine channel buffers for `frames` frames. Returns 0 on success. */
static int mn_alloc_channels(mn_stems *s, uint64_t frames) {
    float *fresh[MN_STEMS_CHANNEL_COUNT] = { 0 };

    mn_free_channels(s);
    if (frames == 0) {
        return -1;
    }
    size_t bytes = (size_t)frames * MN_STEMS_CHANNELS * sizeof(float);
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        fresh[c] = (float *)calloc(1, bytes);
        if (fresh[c] == NULL) {
            for (int k = 0; k < c; ++k) {
                free(fresh[k]);
            }
            return -1;
        }
    }
    /* Install the new buffers under the buffer lock (pointer swap only). */
    mn_mutex_lock(&s->buf_mutex);
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        s->chan[c] = fresh[c];
    }
    s->channel_cap = frames;
    mn_mutex_unlock(&s->buf_mutex);
    mn_atomic64_store(&s->total_frames, (int64_t)frames);
    return 0;
}

/* --------------------------------------------------------------------------
 * Band-split one segment's worth of the six neural stems into the nine channel
 * buffers, writing into [dst_off, dst_off+frames). `neural` points to six
 * planar interleaved-stereo stems, each `frames` long, laid out as
 * neural[stem][frame*2 + ch]. Stem order matches the HTDemucs canonical order:
 *   0=drums 1=bass 2=other 3=vocals 4=guitar 5=piano.
 *
 * `scratch` is a caller-provided workspace of at least frames*2 floats used for
 * the low/high split of the drums stem.
 * -------------------------------------------------------------------------- */
static void mn_bandsplit_segment(mn_stems *s,
                                 float *const neural[MN_STEMS_NEURAL_COUNT],
                                 uint64_t dst_off, uint64_t frames,
                                 float *scratch) {
    if (frames == 0) {
        return;
    }
    const float sr = (float)MN_STEMS_MODEL_SR;
    size_t stride = (size_t)dst_off * MN_STEMS_CHANNELS;

    const float *drums  = neural[0];
    const float *bass   = neural[1];
    const float *other  = neural[2];
    const float *vocals = neural[3];
    const float *guitar = neural[4];
    const float *piano  = neural[5];

    float *sub  = s->chan[MN_STEM_SUB_BASS]    + stride;
    float *bas  = s->chan[MN_STEM_BASS]        + stride;
    float *voc  = s->chan[MN_STEM_VOCALS]      + stride;
    float *lead = s->chan[MN_STEM_LEAD]        + stride;
    float *inst = s->chan[MN_STEM_INSTRUMENTS] + stride;
    float *wide = s->chan[MN_STEM_WIDE]        + stride;
    float *air  = s->chan[MN_STEM_AIR]         + stride;
    float *gtr  = s->chan[MN_STEM_GUITAR]      + stride;
    float *pno  = s->chan[MN_STEM_PIANO]       + stride;

    /* bass -> sub (90 Hz LP) + bass (remainder). */
    mn_lowpass_split(bass, sub, bas, frames, MN_STEMS_SUB_HZ, sr);

    /* drums -> lead (body < 9 kHz) + air (high). Use scratch for the low band,
     * then move it into lead; air receives the complementary high band. */
    mn_lowpass_split(drums, scratch, air, frames, MN_STEMS_AIR_HZ, sr);
    mn_copy_stereo(scratch, lead, frames);

    /* other -> instruments (mid) + wide (side). */
    mn_midside_split(other, inst, wide, frames);

    /* Direct-mapped stems. */
    mn_copy_stereo(vocals, voc, frames);
    mn_copy_stereo(guitar, gtr, frames);
    mn_copy_stereo(piano,  pno, frames);
}

/* --------------------------------------------------------------------------
 * Publish progress fields as a group (best-effort, individually atomic).
 * -------------------------------------------------------------------------- */
static void mn_publish_progress(mn_stems *s, mn_stems_provider provider,
                                float rt_factor, float fraction,
                                int64_t sep_ms, int64_t total_ms) {
    mn_atomic32_store(&s->prog_provider, (int32_t)provider);
    mn_atomic32_store(&s->prog_rt_bits, mn_f2i(rt_factor));
    mn_atomic32_store(&s->prog_frac_bits, mn_f2i(mn_clampf(fraction, 0.0f, 1.0f)));
    mn_atomic64_store(&s->prog_sep_ms, sep_ms);
    mn_atomic64_store(&s->prog_total_ms, total_ms);
}

/* --------------------------------------------------------------------------
 * Disk cache: try to load the nine channels for `track_id`. On success, fills
 * the channel buffers, sets published/total frames and returns 0. On any
 * mismatch or I/O error returns -1 and leaves published state cleared.
 * -------------------------------------------------------------------------- */
static int mn_cache_load(mn_stems *s, int64_t track_id) {
    char path[1024];
    if (mn_cache_path(s, track_id, path, sizeof path) != 0) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    uint32_t magic = 0, stem_count = 0;
    uint64_t frames = 0;
    int ok = 1;
    if (fread(&magic, sizeof magic, 1, f) != 1 ||
        fread(&stem_count, sizeof stem_count, 1, f) != 1 ||
        fread(&frames, sizeof frames, 1, f) != 1) {
        ok = 0;
    }
    if (ok && (magic != MN_STEMS_CACHE_MAGIC ||
               stem_count != MN_STEMS_CHANNEL_COUNT ||
               frames == 0)) {
        ok = 0;
    }
    if (ok && mn_alloc_channels(s, frames) != 0) {
        ok = 0;
    }
    if (ok) {
        size_t count = (size_t)frames * MN_STEMS_CHANNELS;
        for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
            /* Large caches take a while to read; honor cancellation between
             * channels so a track switch never stalls behind this load. */
            if (mn_atomic32_load(&s->cancel_flag) ||
                mn_atomic32_load(&s->shutting_down)) {
                ok = 0;
                break;
            }
            if (fread(s->chan[c], sizeof(float), count, f) != count) {
                ok = 0;
                break;
            }
        }
    }
    fclose(f);
    if (!ok) {
        mn_free_channels(s);
        return -1;
    }
    mn_atomic64_store(&s->published_frames, (int64_t)frames);
    mn_atomic32_store(&s->neural_ready, 1);
    return 0;
}

/* --------------------------------------------------------------------------
 * Disk-cache size cap. Uncompressed 9-channel float stems run ~1.6 MB per
 * track-second, so an uncapped cache reaches tens of GB after a few dozen
 * tracks and eventually FILLS THE DRIVE (observed: 27 GB). After every store
 * (and once at create) evict oldest-modified files until under the cap.
 * -------------------------------------------------------------------------- */
#define MN_STEMS_CACHE_CAP_BYTES (8ull << 30)   /* 8 GB default */

/* Runtime-adjustable cap (Settings -> Storage). Global rather than
 * per-session so it can be set before/without a live stems session. */
static volatile LONG64 g_stems_cache_cap = (LONG64)MN_STEMS_CACHE_CAP_BYTES;

void mn_stems_set_cache_cap_bytes(int64_t bytes) {
    if (bytes < (int64_t)1 << 28) bytes = (int64_t)1 << 28;   /* >= 256 MB */
    InterlockedExchange64(&g_stems_cache_cap, (LONG64)bytes);
}

#ifdef _WIN32
static void mn_cache_trim(mn_stems *s) {
    typedef struct { char name[512]; int64_t size; int64_t mtime; } mn_cf;
    char pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    mn_cf *list = NULL;
    int n = 0, capn = 0;
    int64_t total = 0;

    if (s == NULL || s->cache_dir == NULL) return;
    snprintf(pattern, sizeof pattern, "%s\\*.mnstems", s->cache_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n == capn) {
            int   nc = capn ? capn * 2 : 256;
            mn_cf *nl = (mn_cf *)realloc(list, (size_t)nc * sizeof(mn_cf));
            if (!nl) break;
            list = nl; capn = nc;
        }
        snprintf(list[n].name, sizeof list[n].name, "%s", fd.cFileName);
        list[n].size  = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        list[n].mtime = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                        fd.ftLastWriteTime.dwLowDateTime;
        total += list[n].size;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    const int64_t cap = (int64_t)InterlockedCompareExchange64(&g_stems_cache_cap, 0, 0);
    if (total > cap && n > 1) {
        /* oldest first */
        for (int i = 1; i < n; ++i) {          /* insertion sort: n is small */
            mn_cf key = list[i];
            int j = i - 1;
            while (j >= 0 && list[j].mtime > key.mtime) {
                list[j + 1] = list[j];
                j--;
            }
            list[j + 1] = key;
        }
        for (int i = 0; i < n - 1 && total > cap; ++i) {
            char full[1200];
            snprintf(full, sizeof full, "%s\\%s", s->cache_dir, list[i].name);
            if (remove(full) == 0) {
                total -= list[i].size;
            }
        }
    }
    free(list);
}
#else
static void mn_cache_trim(mn_stems *s) { (void)s; }
#endif

/* --------------------------------------------------------------------------
 * Disk cache: write the nine channels for `track_id`. Best-effort; a failure is
 * silently ignored (the session still works, just without a cached replay).
 * -------------------------------------------------------------------------- */
static void mn_cache_store(mn_stems *s, int64_t track_id, uint64_t frames) {
    char path[1024];
    if (frames == 0 || mn_cache_path(s, track_id, path, sizeof path) != 0) {
        return;
    }
    mn_mkdir(s->cache_dir);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    uint32_t magic = MN_STEMS_CACHE_MAGIC;
    uint32_t stem_count = MN_STEMS_CHANNEL_COUNT;
    int ok = 1;
    if (fwrite(&magic, sizeof magic, 1, f) != 1 ||
        fwrite(&stem_count, sizeof stem_count, 1, f) != 1 ||
        fwrite(&frames, sizeof frames, 1, f) != 1) {
        ok = 0;
    }
    if (ok) {
        size_t count = (size_t)frames * MN_STEMS_CHANNELS;
        for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
            if (fwrite(s->chan[c], sizeof(float), count, f) != count) {
                ok = 0;
                break;
            }
        }
    }
    fclose(f);
    if (!ok) {
        remove(path);
    }
    /* Keep the cache bounded (evicts oldest tracks past the cap). */
    mn_cache_trim(s);
}

/* --------------------------------------------------------------------------
 * Run HTDemucs on one interleaved-stereo segment of `seg_frames` frames.
 *
 * Input tensor shape: [1, MN_STEMS_CHANNELS, seg_frames] (batch, channels,
 * samples) with planar (deinterleaved) layout, matching typical HTDemucs export.
 * Output tensor shape: [1, MN_STEMS_NEURAL_COUNT, MN_STEMS_CHANNELS, seg_frames].
 *
 * The caller owns two REUSABLE OrtValues created once per job: `input` wraps
 * the deinterleaved segment buffer ([L samples..., R samples...]) and
 * `*output` wraps `out_planar` (stem-major planar output). On success writes
 * six planar interleaved-stereo stems into out_stems[k] (each seg_frames*2
 * floats) and returns 0; returns -1 on any ORT error (logged to stderr).
 * -------------------------------------------------------------------------- */
static int mn_infer_segment(mn_stems *s, OrtRunOptions *ropts,
                            OrtValue *input, OrtValue **output,
                            const float *out_planar, uint64_t seg_frames,
                            float *out_stems[MN_STEMS_NEURAL_COUNT]) {
    const OrtApi *ort = s->ort;

    const char *in_names[1]  = { s->input_name };
    const char *out_names[1] = { s->output_name };

    /* `input` wraps the job's in_planar buffer (rewritten between calls) and
     * `*output` wraps out_planar; both are created ONCE per job and reused for
     * every segment, so no OrtValue churn or output allocation happens per
     * 7.8 s chunk. ORT fills the pre-allocated output in place. `ropts` is
     * NULL for all but a job's final segment, where it enables device-arena
     * shrinkage so idle VRAM is returned once separation completes. */
    if (mn_ort_failed_log(ort, ort->Run(s->session, ropts,
            in_names, (const OrtValue *const *)&input, 1,
            out_names, 1, output), "Run")) {
        return -1;
    }

    /* out_planar layout: [stem][channel][sample], planar. Re-interleave into
     * each stem's stereo buffer. */
    for (int k = 0; k < MN_STEMS_NEURAL_COUNT; ++k) {
        const float *base = out_planar +
            (size_t)k * MN_STEMS_CHANNELS * seg_frames;
        const float *pl = base;
        const float *pr = base + seg_frames;
        float *dst = out_stems[k];
        for (uint64_t i = 0; i < seg_frames; ++i) {
            dst[i * 2 + 0] = pl[i];
            dst[i * 2 + 1] = pr[i];
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Producer job body: decode, segment, infer, band-split, publish, cache.
 * -------------------------------------------------------------------------- */

/* Abort predicate handed to the cancellable decode: a track switch (cancel)
 * or session teardown stops the whole-file decode within ~1 s of audio, so
 * mn_stems_start's cancel+join never stalls behind a long decode. */
static bool mn_job_should_abort(void *user) {
    mn_stems *s = (mn_stems *)user;
    return mn_atomic32_load(&s->cancel_flag) != 0 ||
           mn_atomic32_load(&s->shutting_down) != 0;
}

static void mn_run_job(mn_stems *s) {
    int64_t track_id = s->job_track_id;
    char   *audio_path = s->job_audio_path;

    /* 1) Cache fast-path. */
    if (mn_cache_load(s, track_id) == 0) {
        int64_t frames = mn_atomic64_load(&s->total_frames);
        int64_t total_ms = frames * 1000 / MN_STEMS_MODEL_SR;
        mn_publish_progress(s, MN_STEMS_PROVIDER_CACHE, 0.0f, 1.0f,
                            total_ms, total_ms);
        return;
    }

    if (audio_path == NULL) {
        return;
    }

    /* 2) Decode to 44.1 kHz deinterleaved stereo (cancellable: a new
     * mn_stems_start/cancel aborts it between ~1 s chunks). */
    float *L = NULL, *R = NULL;
    uint64_t frames = 0;
    if (mn_decode_44100_stereo_ex(audio_path, &L, &R, &frames,
                                  mn_job_should_abort, s) != MN_OK ||
        frames == 0) {
        mn_free_samples(L);
        mn_free_samples(R);
        return;
    }

    int64_t total_ms = (int64_t)(frames * 1000 / MN_STEMS_MODEL_SR);
    mn_stems_provider provider = s->active_provider;

    if (mn_alloc_channels(s, frames) != 0) {
        mn_free_samples(L);
        mn_free_samples(R);
        return;
    }

    /* 3) Per-segment scratch buffers + REUSABLE ORT tensors (created once per
     * job, reused across every segment — no per-chunk OrtValue create/release
     * and no per-chunk output allocation). */
    const uint64_t seg = MN_STEMS_SEGMENT;
    const uint64_t hop = (seg > MN_STEMS_OVERLAP) ? (seg - MN_STEMS_OVERLAP)
                                                  : seg;
    float *in_planar = (float *)malloc((size_t)seg * MN_STEMS_CHANNELS *
                                       sizeof(float));
    float *out_planar = (float *)malloc((size_t)seg * MN_STEMS_CHANNELS *
                                        MN_STEMS_NEURAL_COUNT * sizeof(float));
    float *stems_buf[MN_STEMS_NEURAL_COUNT] = { 0 };
    float *bs_scratch = (float *)malloc((size_t)seg * MN_STEMS_CHANNELS *
                                        sizeof(float));
    OrtValue *in_val = NULL, *out_val = NULL;
    int alloc_ok = (in_planar != NULL && out_planar != NULL &&
                    bs_scratch != NULL);
    for (int k = 0; k < MN_STEMS_NEURAL_COUNT && alloc_ok; ++k) {
        stems_buf[k] = (float *)malloc((size_t)seg * MN_STEMS_CHANNELS *
                                       sizeof(float));
        if (stems_buf[k] == NULL) {
            alloc_ok = 0;
        }
    }
    if (alloc_ok) {
        const OrtApi *ort = s->ort;
        int64_t in_shape[3]  = { 1, MN_STEMS_CHANNELS, (int64_t)seg };
        int64_t out_shape[4] = { 1, MN_STEMS_NEURAL_COUNT, MN_STEMS_CHANNELS,
                                 (int64_t)seg };
        if (mn_ort_failed_log(ort, ort->CreateTensorWithDataAsOrtValue(
                s->mem_info, in_planar,
                (size_t)seg * MN_STEMS_CHANNELS * sizeof(float),
                in_shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val),
                "CreateTensor(input)") ||
            mn_ort_failed_log(ort, ort->CreateTensorWithDataAsOrtValue(
                s->mem_info, out_planar,
                (size_t)seg * MN_STEMS_CHANNELS * MN_STEMS_NEURAL_COUNT *
                    sizeof(float),
                out_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out_val),
                "CreateTensor(output)") ||
            in_val == NULL || out_val == NULL) {
            alloc_ok = 0;
        }
    }

    if (alloc_ok) {
        const int   dbg      = (getenv("MN_SEP_DEBUG") != NULL);
        const int64_t t_loop0 = mn_mono_ms();
        float       rt       = 0.0f;

        for (uint64_t pos = 0; pos < frames; pos += hop) {
            if (mn_atomic32_load(&s->cancel_flag) ||
                mn_atomic32_load(&s->shutting_down)) {
                break;
            }
            uint64_t seg_frames = frames - pos;
            if (seg_frames > seg) {
                seg_frames = seg;
            }

            /* Fill the planar input: [L..., R...]; zero-pad the tail. */
            memset(in_planar, 0,
                   (size_t)seg * MN_STEMS_CHANNELS * sizeof(float));
            for (uint64_t i = 0; i < seg_frames; ++i) {
                in_planar[i] = L[pos + i];
                in_planar[seg + i] = R[pos + i];
            }

            /* The final segment's Run carries the arena-shrink options so
             * device memory is returned once the job's inference is done. */
            int is_last = (pos + seg >= frames);

            int64_t t_seg0 = mn_mono_ms();
            if (mn_infer_segment(s, is_last ? s->run_shrink : NULL,
                                 in_val, &out_val, out_planar, seg,
                                 stems_buf) != 0) {
                break;
            }
            int64_t seg_ms = mn_mono_ms() - t_seg0;
            if (dbg) {
                fprintf(stderr,
                        "[stems] segment @%llu: %lld ms for %.2f s audio "
                        "(%.2fx rt)\n",
                        (unsigned long long)pos, (long long)seg_ms,
                        (double)seg_frames / MN_STEMS_MODEL_SR,
                        seg_ms > 0 ? ((double)seg_frames * 1000.0 /
                                      MN_STEMS_MODEL_SR / (double)seg_ms)
                                   : 0.0);
                fflush(stderr);
            }

            /* Band-split the valid portion and publish it. On overlapping
             * segments we simply overwrite the overlap region with the newest
             * (fully-resolved) inference, which is monotonic and race-free
             * because published_frames only advances after the write. */
            mn_bandsplit_segment(s, stems_buf, pos, seg_frames, bs_scratch);

            uint64_t done_frames = pos + seg_frames;
            mn_atomic64_store(&s->published_frames, (int64_t)done_frames);
            mn_atomic32_store(&s->neural_ready, 1);

            float frac = (float)done_frames / (float)frames;
            int64_t sep_ms = (int64_t)(done_frames * 1000 / MN_STEMS_MODEL_SR);
            /* REAL rt factor: audio-time separated / wall-time spent in the
             * inference loop (previously hardcoded to 1.0, which the UI then
             * dutifully displayed as "1.00x" no matter the actual speed). */
            {
                int64_t wall_ms = mn_mono_ms() - t_loop0;
                rt = (wall_ms > 0) ? (float)((double)sep_ms /
                                             (double)wall_ms) : 0.0f;
            }
            mn_publish_progress(s, provider, rt, frac, sep_ms, total_ms);

            if (seg_frames < seg) {
                break; /* consumed the tail */
            }

            /* GPU duty-cycle cap (~MN_STEMS_GPU_DUTY): once the frontier
             * leads the live playhead by MN_STEMS_THROTTLE_LEAD, pause
             * between segments so inference occupies only ~65% of wall time
             * and other applications keep GPU headroom. Full speed while the
             * playhead is close (realtime never starves) or when nothing is
             * consuming the mix (no playhead => e.g. the --sep benchmark).
             * Sleeps in small slices so cancellation stays bounded and the
             * pause ends early if the playhead catches up. */
            if (!is_last) {
                int64_t pause_ms = (int64_t)((double)seg_ms *
                        (1.0 - MN_STEMS_GPU_DUTY) / MN_STEMS_GPU_DUTY);
                while (pause_ms > 0 &&
                       !mn_atomic32_load(&s->cancel_flag) &&
                       !mn_atomic32_load(&s->shutting_down)) {
                    if (!mn_atomic32_load(&s->mix_seen)) {
                        break; /* no live consumer: run at full speed */
                    }
                    int64_t playhead = mn_atomic64_load(&s->last_mix_frame);
                    if ((int64_t)done_frames <
                        playhead + MN_STEMS_THROTTLE_LEAD) {
                        break; /* frontier no longer comfortably ahead */
                    }
                    mn_sleep_ms(pause_ms > 50 ? 50u : (unsigned)pause_ms);
                    pause_ms -= 50;
                }
            }
        }

        /* 4) If we finished cleanly, cache the result for instant replay. */
        if (!mn_atomic32_load(&s->cancel_flag) &&
            !mn_atomic32_load(&s->shutting_down) &&
            mn_atomic64_load(&s->published_frames) >= (int64_t)frames) {
            mn_cache_store(s, track_id, frames);
            mn_publish_progress(s, provider, rt, 1.0f, total_ms, total_ms);
        }
    }

    /* 5) Cleanup scratch + decoded PCM + reusable ORT tensors. The OrtValues
     * only WRAP in_planar/out_planar (no copy), so release them before the
     * underlying buffers are freed. */
    if (in_val != NULL) {
        s->ort->ReleaseValue(in_val);
    }
    if (out_val != NULL) {
        s->ort->ReleaseValue(out_val);
    }
    for (int k = 0; k < MN_STEMS_NEURAL_COUNT; ++k) {
        free(stems_buf[k]);
    }
    free(in_planar);
    free(out_planar);
    free(bs_scratch);
    mn_free_samples(L);
    mn_free_samples(R);
}

/* --------------------------------------------------------------------------
 * Producer thread entry-point (platform trampoline).
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
static unsigned __stdcall mn_thread_entry(void *arg) {
    mn_run_job((mn_stems *)arg);
    return 0;
}
#else
static void *mn_thread_entry(void *arg) {
    mn_run_job((mn_stems *)arg);
    return NULL;
}
#endif

/* Join the producer thread if one is running. Assumes cancel/shutdown flags are
 * already set by the caller as desired. */
static void mn_join_thread(mn_stems *s) {
    if (!s->thread_valid) {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    s->thread = NULL;
#else
    pthread_join(s->thread, NULL);
#endif
    s->thread_valid = 0;
}

/* Spawn the producer thread running mn_run_job. Returns 0 on success. */
static int mn_spawn_thread(mn_stems *s) {
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, mn_thread_entry, s, 0, NULL);
    if (h == 0) {
        return -1;
    }
    s->thread = (HANDLE)h;
    s->thread_valid = 1;
    return 0;
#else
    if (pthread_create(&s->thread, NULL, mn_thread_entry, s) != 0) {
        return -1;
    }
    s->thread_valid = 1;
    return 0;
#endif
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

mn_stems *mn_stems_create(const char *model_path, const char *cache_dir) {
    if (model_path == NULL || model_path[0] == '\0') {
        return NULL;
    }

    mn_stems *s = (mn_stems *)calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }

    mn_mutex_init(&s->job_mutex);
    mn_mutex_init(&s->buf_mutex);

    /* Default live-control state: unity gain, unmuted, unsoloed. */
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        mn_atomic32_store(&s->ctl[c].target_gain_bits, mn_f2i(1.0f));
        s->ctl[c].cur_gain = 1.0f;
        mn_atomic32_store(&s->ctl[c].muted, 0);
        mn_atomic32_store(&s->ctl[c].soloed, 0);
        mn_atomic32_store(&s->ctl[c].meter_bits, mn_f2i(0.0f));
    }
    mn_atomic32_store(&s->prog_provider, MN_STEMS_PROVIDER_NONE);

    /* Copy the cache dir (optional). */
    if (cache_dir != NULL && cache_dir[0] != '\0') {
        size_t n = strlen(cache_dir) + 1;
        s->cache_dir = (char *)malloc(n);
        if (s->cache_dir == NULL) {
            goto fail;
        }
        memcpy(s->cache_dir, cache_dir, n);
        mn_mkdir(s->cache_dir);
        /* One-time startup trim: brings an over-cap cache (from older builds
         * without the cap) back under MN_STEMS_CACHE_CAP_BYTES. */
        mn_cache_trim(s);
    }

    /* ---- ONNX Runtime bring-up. ---- */
    s->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (s->ort == NULL) {
        goto fail;
    }
    const OrtApi *ort = s->ort;

    if (mn_ort_failed(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                          "mn_stems", &s->env))) {
        goto fail;
    }
    if (mn_ort_failed(ort, ort->CreateSessionOptions(&s->sopts))) {
        goto fail;
    }
    ort->SetSessionGraphOptimizationLevel(s->sopts, ORT_ENABLE_ALL);
    /* 0 = ORT's default = ~all logical cores. Fine behind CUDA (the CPU
     * only feeds the GPU), catastrophic for a CPU-EP fallback on a small
     * box — low-power mode caps it to half the cores so a separation
     * can't freeze the whole machine. */
    if (getenv("MN_LOWPOWER")) {
        int threads = 2;
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si);
        threads = (int)si.dwNumberOfProcessors / 2;
#endif
        if (threads < 1) threads = 1;
        ort->SetIntraOpNumThreads(s->sopts, threads);
    } else {
        ort->SetIntraOpNumThreads(s->sopts, 0); /* ORT default */
    }

    /* Try CUDA first via the V2 provider options, fall back to CPU. Failures
     * are LOGGED (stderr) so a silent CPU fallback is impossible to miss:
     * active_provider — and therefore the "CUDA" string surfaced to the UI —
     * is only set when the provider actually registered successfully. */
    s->active_provider = MN_STEMS_PROVIDER_CPU;
    {
        OrtCUDAProviderOptionsV2 *cuda_opts = NULL;
        if (!mn_ort_failed_log(ort, ort->CreateCUDAProviderOptions(&cuda_opts),
                               "CreateCUDAProviderOptions") &&
            cuda_opts != NULL) {
            /* Speed/VRAM balance (measured on the RTX 4060 Ti 16GB):
             *   - do_copy_in_default_stream=1: the proven-fast prototype
             *     setting (no cross-stream sync overhead).
             *   - cudnn_conv_algo_search=HEURISTIC: EXHAUSTIVE's search
             *     allocates huge trial workspaces (part of the ~7 GB VRAM
             *     balloon) for a one-time gain that HEURISTIC matches here.
             *   - arena_extend_strategy=kSameAsRequested + gpu_mem_limit=3GiB:
             *     the default (uncapped kNextPowerOfTwo) arena never returns
             *     memory and doubled its way to ~7 GB resident; capping +
             *     exact-growth keeps throughput (measured) while the post-job
             *     arena shrinkage (run_shrink) returns idle VRAM. */
            const char *ck[] = { "do_copy_in_default_stream",
                                 "cudnn_conv_algo_search",
                                 "arena_extend_strategy",
                                 "gpu_mem_limit" };
            const char *cv[] = { "1", "HEURISTIC", "kSameAsRequested",
                                 MN_STEMS_CUDA_MEM_LIMIT };
            (void)mn_ort_failed_log(ort,
                    ort->UpdateCUDAProviderOptions(cuda_opts, ck, cv, 4),
                    "UpdateCUDAProviderOptions");
            if (!mn_ort_failed_log(ort,
                    ort->SessionOptionsAppendExecutionProvider_CUDA_V2(
                        s->sopts, cuda_opts),
                    "SessionOptionsAppendExecutionProvider_CUDA_V2")) {
                s->active_provider = MN_STEMS_PROVIDER_CUDA;
            }
            ort->ReleaseCUDAProviderOptions(cuda_opts);
        }
    }

    /* Load the model (resident session). */
    {
        ORTCHAR_T *wpath = mn_to_ortchar(model_path);
        if (wpath == NULL) {
            goto fail;
        }
        int bad = mn_ort_failed_log(ort,
                                    ort->CreateSession(s->env, wpath,
                                                       s->sopts, &s->session),
                                    "CreateSession");
        free(wpath);
        if (bad || s->session == NULL) {
            goto fail;
        }
    }

    /* One-time truth log of the provider actually driving inference. */
    fprintf(stderr, "[stems] session ready, execution provider: %s\n",
            (s->active_provider == MN_STEMS_PROVIDER_CUDA) ? "CUDA" : "CPU");
    fflush(stderr);

    /* Run options used ONLY for the final segment of a job: shrink the device
     * memory arena when that run completes, returning idle VRAM to the system
     * while the session (weights, kernels) stays fully resident. */
    if (!mn_ort_failed_log(ort, ort->CreateRunOptions(&s->run_shrink),
                           "CreateRunOptions") && s->run_shrink != NULL) {
        const char *arenas = (s->active_provider == MN_STEMS_PROVIDER_CUDA)
                                 ? "gpu:0" : "cpu:0";
        if (mn_ort_failed_log(ort, ort->AddRunConfigEntry(
                s->run_shrink, kOrtRunOptionsConfigEnableMemoryArenaShrinkage,
                arenas), "AddRunConfigEntry(arena shrink)")) {
            ort->ReleaseRunOptions(s->run_shrink);
            s->run_shrink = NULL;
        }
    }

    if (mn_ort_failed(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator,
                                                    OrtMemTypeDefault,
                                                    &s->mem_info))) {
        goto fail;
    }
    if (mn_ort_failed(ort, ort->GetAllocatorWithDefaultOptions(&s->allocator))) {
        goto fail;
    }

    /* Query the model IO names. */
    if (mn_ort_failed(ort, ort->SessionGetInputName(s->session, 0,
                                                    s->allocator,
                                                    &s->input_name)) ||
        mn_ort_failed(ort, ort->SessionGetOutputName(s->session, 0,
                                                     s->allocator,
                                                     &s->output_name)) ||
        s->input_name == NULL || s->output_name == NULL) {
        goto fail;
    }

    mn_publish_progress(s, MN_STEMS_PROVIDER_NONE, 0.0f, 0.0f, 0, 0);
    return s;

fail:
    mn_stems_destroy(s);
    return NULL;
}

void mn_stems_destroy(mn_stems *s) {
    if (s == NULL) {
        return;
    }

    /* Signal and join any in-flight producer. */
    mn_atomic32_store(&s->shutting_down, 1);
    mn_atomic32_store(&s->cancel_flag, 1);
    mn_join_thread(s);

    /* Release ONNX Runtime state (order matters: session before env). */
    if (s->ort != NULL) {
        const OrtApi *ort = s->ort;
        if (s->input_name != NULL && s->allocator != NULL) {
            ort->AllocatorFree(s->allocator, s->input_name);
        }
        if (s->output_name != NULL && s->allocator != NULL) {
            ort->AllocatorFree(s->allocator, s->output_name);
        }
        if (s->run_shrink != NULL) {
            ort->ReleaseRunOptions(s->run_shrink);
        }
        if (s->mem_info != NULL) {
            ort->ReleaseMemoryInfo(s->mem_info);
        }
        if (s->session != NULL) {
            ort->ReleaseSession(s->session);
        }
        if (s->sopts != NULL) {
            ort->ReleaseSessionOptions(s->sopts);
        }
        if (s->env != NULL) {
            ort->ReleaseEnv(s->env);
        }
    }

    mn_free_channels(s);
    free(s->job_audio_path);
    free(s->cache_dir);
    mn_mutex_destroy(&s->buf_mutex);
    mn_mutex_destroy(&s->job_mutex);
    free(s);
}

bool mn_stems_start(mn_stems *s, int64_t track_id, const char *audio_path) {
    if (s == NULL || audio_path == NULL || audio_path[0] == '\0') {
        return false;
    }
    if (mn_atomic32_load(&s->shutting_down)) {
        return false;
    }

    mn_mutex_lock(&s->job_mutex);

    /* Cancel + join any prior job so we can safely reset published buffers. */
    mn_atomic32_store(&s->cancel_flag, 1);
    mn_join_thread(s);
    mn_atomic32_store(&s->cancel_flag, 0);

    /* Reset published neural state to passthrough for the new track. */
    mn_free_channels(s);

    /* Fresh playhead-hint state for the new job (duty-cycle pacing). */
    mn_atomic32_store(&s->mix_seen, 0);
    mn_atomic64_store(&s->last_mix_frame, 0);

    /* Stage job parameters. */
    free(s->job_audio_path);
    size_t n = strlen(audio_path) + 1;
    s->job_audio_path = (char *)malloc(n);
    if (s->job_audio_path == NULL) {
        mn_mutex_unlock(&s->job_mutex);
        return false;
    }
    memcpy(s->job_audio_path, audio_path, n);
    s->job_track_id = track_id;

    mn_publish_progress(s, MN_STEMS_PROVIDER_NONE, 0.0f, 0.0f, 0, 0);

    if (mn_spawn_thread(s) != 0) {
        free(s->job_audio_path);
        s->job_audio_path = NULL;
        mn_mutex_unlock(&s->job_mutex);
        return false;
    }

    mn_mutex_unlock(&s->job_mutex);
    return true;
}

void mn_stems_cancel(mn_stems *s) {
    if (s == NULL) {
        return;
    }
    mn_mutex_lock(&s->job_mutex);
    mn_atomic32_store(&s->cancel_flag, 1);
    mn_join_thread(s);
    mn_atomic32_store(&s->cancel_flag, 0);
    mn_free_channels(s);
    mn_publish_progress(s, MN_STEMS_PROVIDER_NONE, 0.0f, 0.0f, 0, 0);
    mn_mutex_unlock(&s->job_mutex);
}

/* --------------------------------------------------------------------------
 * Offline export: separate to completion (blocking) + read channels.
 * -------------------------------------------------------------------------- */
bool mn_stems_separate_sync(mn_stems *s, int64_t track_id,
                            const char *audio_path,
                            int (*abort)(void *ctx), void *abort_ctx) {
    if (s == NULL || audio_path == NULL || audio_path[0] == '\0') return false;
    if (mn_atomic32_load(&s->shutting_down)) return false;

    mn_mutex_lock(&s->job_mutex);

    /* Cancel/join any prior job, reset published buffers, stage this job —
     * mirrors mn_stems_start's setup. */
    mn_atomic32_store(&s->cancel_flag, 1);
    mn_join_thread(s);
    mn_atomic32_store(&s->cancel_flag, 0);
    mn_free_channels(s);
    mn_atomic32_store(&s->mix_seen, 0);
    mn_atomic64_store(&s->last_mix_frame, 0);

    free(s->job_audio_path);
    size_t n = strlen(audio_path) + 1;
    s->job_audio_path = (char *)malloc(n);
    if (s->job_audio_path == NULL) { mn_mutex_unlock(&s->job_mutex); return false; }
    memcpy(s->job_audio_path, audio_path, n);
    s->job_track_id = track_id;
    mn_publish_progress(s, MN_STEMS_PROVIDER_NONE, 0.0f, 0.0f, 0, 0);

    if (mn_spawn_thread(s) != 0) {
        free(s->job_audio_path); s->job_audio_path = NULL;
        mn_mutex_unlock(&s->job_mutex);
        return false;
    }

    /* With no live playhead the producer runs full-speed (no duty throttle)
     * and separates the whole track, then exits. Poll the abort predicate
     * while it works; requesting cancel makes the producer stop early. On
     * Windows we timed-wait on the thread; elsewhere we poll the completion
     * fraction. Either way mn_join_thread() below reaps it definitively. */
#ifdef _WIN32
    for (;;) {
        DWORD w = WaitForSingleObject(s->thread, 100);
        if (w == WAIT_OBJECT_0) break;
        if (abort && abort(abort_ctx)) mn_atomic32_store(&s->cancel_flag, 1);
    }
#else
    for (;;) {
        struct timespec ts = { 0, 100L * 1000000L };
        nanosleep(&ts, NULL);
        if (abort && abort(abort_ctx)) mn_atomic32_store(&s->cancel_flag, 1);
        float frac = mn_i2f(mn_atomic32_load(&s->prog_frac_bits));
        if (frac >= 0.999f) break;
    }
#endif
    /* ensure the thread is fully reaped */
    mn_join_thread(s);

    bool full = mn_atomic32_load(&s->neural_ready) &&
                mn_atomic64_load(&s->total_frames) > 0 &&
                mn_atomic64_load(&s->published_frames) >=
                    mn_atomic64_load(&s->total_frames);
    mn_mutex_unlock(&s->job_mutex);
    return full;
}

bool mn_stems_export_channel(mn_stems *s, int idx,
                             float **out, uint64_t *out_frames) {
    if (s == NULL || out == NULL || out_frames == NULL) return false;
    if (idx < 0 || idx >= MN_STEMS_CHANNEL_COUNT) return false;
    *out = NULL; *out_frames = 0;

    mn_mutex_lock(&s->buf_mutex);
    uint64_t frames = mn_atomic64_load(&s->published_frames);
    if (!mn_atomic32_load(&s->neural_ready) || frames == 0 || s->chan[idx] == NULL) {
        mn_mutex_unlock(&s->buf_mutex);
        return false;
    }
    size_t n = (size_t)frames * MN_STEMS_CHANNELS;
    float *buf = (float *)malloc(n * sizeof(float));
    if (buf == NULL) { mn_mutex_unlock(&s->buf_mutex); return false; }
    memcpy(buf, s->chan[idx], n * sizeof(float));
    mn_mutex_unlock(&s->buf_mutex);

    *out = buf;
    *out_frames = frames;
    return true;
}

void mn_stems_set_gain(mn_stems *s, int channel, float gain) {
    if (s == NULL || channel < 0 || channel >= MN_STEMS_CHANNEL_COUNT) {
        return;
    }
    gain = mn_clampf(gain, MN_STEMS_GAIN_MIN, MN_STEMS_GAIN_MAX);
    mn_atomic32_store(&s->ctl[channel].target_gain_bits, mn_f2i(gain));
}

void mn_stems_set_mute(mn_stems *s, int channel, bool on) {
    if (s == NULL || channel < 0 || channel >= MN_STEMS_CHANNEL_COUNT) {
        return;
    }
    mn_atomic32_store(&s->ctl[channel].muted, on ? 1 : 0);
}

void mn_stems_set_solo(mn_stems *s, int channel) {
    if (s == NULL) {
        return;
    }
    if (channel < 0) {
        /* Clear the entire solo set. */
        for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
            mn_atomic32_store(&s->ctl[c].soloed, 0);
        }
        return;
    }
    if (channel < MN_STEMS_CHANNEL_COUNT) {
        mn_atomic32_store(&s->ctl[channel].soloed, 1);
    }
}

/* Set the solo state of ONE channel independently (multi-solo support): several
 * channels can be soloed at once, and un-soloing one does not clear the others.
 * This is what the UI's per-stem S button should drive. */
void mn_stems_set_solo_state(mn_stems *s, int channel, bool soloed) {
    if (s == NULL || channel < 0 || channel >= MN_STEMS_CHANNEL_COUNT) {
        return;
    }
    mn_atomic32_store(&s->ctl[channel].soloed, soloed ? 1 : 0);
}

void mn_stems_set_force(mn_stems *s, bool on) {
    if (s != NULL) {
        mn_atomic32_store(&s->force_neural, on ? 1 : 0);
    }
}

void mn_stems_set_passthrough(mn_stems *s, bool on) {
    if (s != NULL) {
        mn_atomic32_store(&s->passthrough, on ? 1 : 0);
    }
}

bool mn_stems_neural_active(const mn_stems *s) {
    if (s == NULL) {
        return false;
    }
    if (mn_atomic32_load(&s->passthrough)) {
        return false;
    }
    return mn_atomic32_load(&s->neural_ready) != 0 &&
           mn_atomic64_load(&s->published_frames) > 0;
}

void mn_stems_get_meters(const mn_stems *s, float out[MN_STEMS_CHANNEL_COUNT]) {
    if (out == NULL) {
        return;
    }
    if (s == NULL) {
        for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
            out[c] = 0.0f;
        }
        return;
    }
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        out[c] = mn_i2f(mn_atomic32_load(&s->ctl[c].meter_bits));
    }
}

void mn_stems_get_progress(const mn_stems *s, mn_stems_progress *out) {
    if (out == NULL) {
        return;
    }
    if (s == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->provider     = (mn_stems_provider)mn_atomic32_load(&s->prog_provider);
    out->rt_factor    = mn_i2f(mn_atomic32_load(&s->prog_rt_bits));
    out->fraction     = mn_i2f(mn_atomic32_load(&s->prog_frac_bits));
    out->separated_ms = mn_atomic64_load(&s->prog_sep_ms);
    out->total_ms     = mn_atomic64_load(&s->prog_total_ms);
}

bool mn_stems_mix(mn_stems *s, int64_t frame, uint32_t frames, float *dest) {
    if (s == NULL || dest == NULL || frames == 0 || frame < 0) {
        return false;
    }

    /* Publish the playhead position for the producer's duty-cycle pacing
     * (single atomic stores; done even when this call declines to mix). */
    mn_atomic64_store(&s->last_mix_frame, frame);
    mn_atomic32_store(&s->mix_seen, 1);

    /* Passthrough forced -> let the caller supply source audio. */
    if (mn_atomic32_load(&s->passthrough)) {
        return false;
    }

    /* AUDIO THREAD: try-acquire the buffer-lifetime lock. If the control side
     * is freeing/swapping the channel buffers right now, fall back to source
     * audio for this block instead of blocking (or reading freed memory). */
    if (!mn_mutex_trylock(&s->buf_mutex)) {
        return false;
    }

    int64_t published = mn_atomic64_load(&s->published_frames);
    if (published <= 0 || !mn_atomic32_load(&s->neural_ready)) {
        mn_mutex_unlock(&s->buf_mutex);
        return false;
    }

    int force = mn_atomic32_load(&s->force_neural);
    int64_t total = mn_atomic64_load(&s->total_frames);

    /* PROGRESSIVE PLAYBACK: serve any fully-published PREFIX of the track
     * while the producer keeps separating ahead — no waiting for the whole
     * track. Region [frame, frame+frames) must be covered by published data
     * unless we are in force mode (then not-yet-separated frames are rendered
     * as silence). */
    int64_t end = frame + (int64_t)frames;
    if (!force) {
        if (end > published) {
            /* Block reaches past the separation frontier: fall back to the
             * decoded source for this block. */
            mn_mutex_unlock(&s->buf_mutex);
            return false;
        }
    } else {
        /* In force mode, if the block starts entirely beyond published data and
         * beyond the track, there is nothing to render. */
        if (frame >= total) {
            mn_mutex_unlock(&s->buf_mutex);
            return false;
        }
    }

    /* Engagement gate: commit to the neural renderer only once the frontier
     * comfortably leads this block (10 s margin; 1 s when force-enabled) or
     * the track is fully separated. Once engaged, stay engaged for the whole
     * track so the renderer never audibly swaps back and forth mid-song. */
    if (!mn_atomic32_load(&s->engaged)) {
        int64_t margin = force ? MN_STEMS_ENGAGE_MARGIN_FORCE
                               : MN_STEMS_ENGAGE_MARGIN;
        if (published >= total || published >= end + margin) {
            mn_atomic32_store(&s->engaged, 1);
        } else {
            mn_mutex_unlock(&s->buf_mutex);
            return false;
        }
    }

    /* Determine whether any channel is soloed (solo overrides mute). */
    int any_solo = 0;
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        if (mn_atomic32_load(&s->ctl[c].soloed)) {
            any_solo = 1;
            break;
        }
    }

    /* Zero the destination; we accumulate channels into it. */
    memset(dest, 0, (size_t)frames * MN_STEMS_CHANNELS * sizeof(float));

    /* Per-channel accumulation with smoothed gain and mute/solo masking. */
    float energy[MN_STEMS_CHANNEL_COUNT];
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        energy[c] = 0.0f;

        float target = mn_i2f(mn_atomic32_load(&s->ctl[c].target_gain_bits));
        int   muted  = mn_atomic32_load(&s->ctl[c].muted);
        int   soloed = mn_atomic32_load(&s->ctl[c].soloed);

        /* Audible mask: solo wins; otherwise mute silences. */
        int audible = any_solo ? soloed : !muted;

        const float *src = s->chan[c];
        if (src == NULL) {
            /* No buffer -> gain still slews toward target for glitch-free
             * resumption, but nothing is mixed. */
            s->ctl[c].cur_gain += (1.0f - MN_STEMS_GAIN_SMOOTH) *
                                  (target - s->ctl[c].cur_gain);
            continue;
        }

        float g = s->ctl[c].cur_gain;
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            int64_t sf = frame + (int64_t)i;

            /* Slew the per-sample gain toward target (block-rate smoothing via
             * a light one-pole; applied once per frame). */
            g += (1.0f - MN_STEMS_GAIN_SMOOTH) * (target - g);

            float l = 0.0f, r = 0.0f;
            if (sf < published) {
                l = src[sf * 2 + 0];
                r = src[sf * 2 + 1];
            }
            /* Meter reflects content energy (post-gain, pre-mask). */
            float gl = l * g;
            float gr = r * g;
            float m = fabsf(gl) > fabsf(gr) ? fabsf(gl) : fabsf(gr);
            if (m > peak) {
                peak = m;
            }
            if (audible) {
                dest[i * 2 + 0] += gl;
                dest[i * 2 + 1] += gr;
            }
        }
        s->ctl[c].cur_gain = g;
        energy[c] = peak;
    }

    /* Update meters with attack/decay ballistics. */
    for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; ++c) {
        float prev = mn_i2f(mn_atomic32_load(&s->ctl[c].meter_bits));
        float target = energy[c];
        float coeff = (target > prev) ? MN_STEMS_METER_ATTACK
                                      : MN_STEMS_METER_DECAY;
        float next = prev + coeff * (target - prev);
        next = mn_clampf(next, 0.0f, 1.0f);
        mn_atomic32_store(&s->ctl[c].meter_bits, mn_f2i(next));
    }

    mn_mutex_unlock(&s->buf_mutex);
    return true;
}
