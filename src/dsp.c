/*
 * dsp.c - Monatomic Music Player DSP chain implementation.
 *
 * Implements dsp.h exactly. The processing chain, applied in place to
 * interleaved 32-bit float PCM inside the audio callback, is:
 *
 *     preamp -> 10-band peaking-biquad graphic EQ -> stereo balance ->
 *     upmix / downmix to output layout -> master gain -> soft-clip limiter
 *
 * Real-time safety
 * ----------------
 * mn_dsp_process() performs no heap allocation, no locks and no syscalls. All
 * working buffers are sized and allocated at create()/configure() time. Control
 * threads publish new parameters through a double-buffered snapshot guarded by a
 * seqlock-style generation counter; the audio thread copies the snapshot once
 * per block. User-facing scalar parameters (preamp, balance, gains, master) are
 * ramped per-sample toward their targets to avoid zipper noise. Biquad
 * coefficients are recomputed on the audio thread only when a band's target gain
 * changes, which is bounded work (MN_DSP_EQ_BANDS * channels).
 *
 * Coefficient design
 * ------------------
 * Each EQ band is a second-order peaking (bell) filter using the RBJ audio-EQ
 * cookbook formulas, with a fixed Q per band chosen to give roughly one-octave
 * bandwidth so adjacent ISO bands overlap smoothly.
 *
 * All functions use the mn_ prefix; macros use MN_.
 */

#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Local constants and helpers                                                */
/* ------------------------------------------------------------------------- */

#ifndef MN_PI
#define MN_PI 3.14159265358979323846
#endif

/* Fixed Q for the peaking bands (~1 octave bandwidth). */
#define MN_DSP_EQ_Q 1.41f

/* Per-sample smoothing coefficient for scalar parameter ramps. Applied as a
 * one-pole glide: value += (target - value) * MN_DSP_RAMP. Small enough to be
 * click-free, large enough to settle within a few milliseconds. */
#define MN_DSP_RAMP 0.002f

/* Threshold (dB) below which a gain change is considered a no-op, to avoid
 * needless coefficient recomputation on the audio thread. */
#define MN_DSP_GAIN_EPS_DB 0.001f

/* ISO 1/1-octave center frequencies for the 10 graphic-EQ bands. */
const float MN_DSP_EQ_FREQUENCIES[MN_DSP_EQ_BANDS] = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

/* Built-in EQ presets: 10 band gains (dB) + preamp (dB), indexed by
 * mn_dsp_eq_preset. Order matches the enum in dsp.h. */
typedef struct mn_preset_def {
    const char *name;
    float       gains[MN_DSP_EQ_BANDS];
    float       preamp;
} mn_preset_def;

static const mn_preset_def MN_PRESETS[MN_DSP_EQ_PRESET_COUNT] = {
    /* FLAT */
    { "Flat",
      { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0.0f },
    /* ACOUSTIC */
    { "Acoustic",
      { 4.0f, 4.0f, 3.0f, 1.0f, 1.5f, 1.5f, 3.0f, 3.5f, 3.0f, 1.5f }, -1.5f },
    /* BASS_BOOST */
    { "Bass Boost",
      { 6.0f, 5.0f, 4.0f, 2.5f, 0.5f, 0, 0, 0, 0, 0 }, -3.0f },
    /* BASS_REDUCE */
    { "Bass Reduce",
      { -6.0f, -5.0f, -4.0f, -2.5f, -0.5f, 0, 0, 0, 0, 0 }, 0.0f },
    /* CLASSICAL */
    { "Classical",
      { 4.5f, 4.0f, 3.0f, 2.5f, -1.5f, -1.5f, 0, 2.0f, 3.0f, 3.5f }, -1.5f },
    /* DANCE */
    { "Dance",
      { 5.0f, 6.5f, 3.0f, 0, 1.5f, 3.0f, 4.0f, 3.5f, 2.5f, 0 }, -2.5f },
    /* ELECTRONIC */
    { "Electronic",
      { 5.0f, 4.0f, 1.0f, 0, -1.5f, 1.5f, 0.5f, 1.0f, 4.0f, 5.0f }, -2.0f },
    /* HIP_HOP */
    { "Hip-Hop",
      { 6.0f, 5.0f, 2.0f, 3.0f, -1.0f, -1.0f, 1.5f, -0.5f, 2.0f, 3.0f }, -2.5f },
    /* JAZZ */
    { "Jazz",
      { 4.0f, 3.0f, 1.5f, 2.5f, -1.5f, -1.5f, 0, 1.5f, 3.0f, 4.0f }, -1.5f },
    /* LOUDNESS */
    { "Loudness",
      { 6.0f, 4.5f, 0, 0, -2.0f, 0, -1.0f, -4.0f, 5.0f, 1.0f }, -3.0f },
    /* LOUNGE */
    { "Lounge",
      { -3.0f, -1.5f, -0.5f, 1.5f, 3.5f, 1.5f, 0, -1.5f, 2.0f, 1.0f }, 0.0f },
    /* POP */
    { "Pop",
      { -1.5f, -1.0f, 0, 2.0f, 4.0f, 4.0f, 2.0f, 0, -1.0f, -1.5f }, 0.0f },
    /* ROCK */
    { "Rock",
      { 5.0f, 4.0f, 3.0f, 1.5f, -0.5f, -1.0f, 0.5f, 3.0f, 4.0f, 4.5f }, -2.0f },
    /* TREBLE_BOOST */
    { "Treble Boost",
      { 0, 0, 0, 0, 0, 0.5f, 2.5f, 4.0f, 5.0f, 6.0f }, -3.0f },
    /* VOCAL */
    { "Vocal",
      { -1.5f, -3.0f, -3.0f, 1.5f, 4.0f, 4.0f, 3.5f, 2.0f, 0, -1.5f }, -1.5f }
};

/* Clamp a float to [lo, hi]. */
static float mn_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Convert decibels to a linear amplitude factor. */
static float mn_db_to_lin(float db)
{
    return (float)pow(10.0, (double)db / 20.0);
}

/* ------------------------------------------------------------------------- */
/* Biquad                                                                     */
/* ------------------------------------------------------------------------- */

/* Transposed Direct Form II peaking biquad. Coefficients are normalized so a0
 * is folded in. State s1/s2 are the two delay elements. */
typedef struct mn_biquad {
    float b0, b1, b2;   /* feed-forward */
    float a1, a2;       /* feedback (a0 normalized to 1) */
    float s1, s2;       /* delay state */
} mn_biquad;

/* Compute peaking-EQ coefficients (RBJ cookbook) for the given center
 * frequency, sample rate, Q and gain in dB, writing them into *bq without
 * touching the delay state. */
static void mn_biquad_set_unity(mn_biquad *bq);   /* defined below */

static void mn_biquad_set_peaking(mn_biquad *bq, float freq, float sample_rate,
                                  float q, float gain_db)
{
    double A, w0, cosw0, sinw0, alpha, a0, inv;

    /* NYQUIST GUARD. Above sample_rate/2 the bilinear form breaks down:
     * sin(w0) goes NEGATIVE, which flips alpha's sign and drives a0 toward
     * zero, so 1/a0 explodes and the filter oscillates instead of filtering.
     * Measured on the real chain: the 16 kHz band on a 22.05 kHz audiobook
     * produced +inf, and 11 kHz produced ~2.9e38 — heard as violently
     * warbled/garbled audio the moment that band was boosted.
     * A band that cannot exist at this rate is simply passed through; we
     * stop at 0.45*rate because peaking sections near Nyquist are already
     * numerically fragile in float even while nominally stable. */
    if (!(sample_rate > 0.0f) || freq >= sample_rate * 0.45f) {
        mn_biquad_set_unity(bq);
        return;
    }

    A     = pow(10.0, (double)gain_db / 40.0);
    w0    = 2.0 * MN_PI * (double)freq / (double)sample_rate;
    cosw0 = cos(w0);
    sinw0 = sin(w0);
    alpha = sinw0 / (2.0 * (double)q);

    a0  = 1.0 + alpha / A;
    inv = 1.0 / a0;

    bq->b0 = (float)((1.0 + alpha * A) * inv);
    bq->b1 = (float)((-2.0 * cosw0) * inv);
    bq->b2 = (float)((1.0 - alpha * A) * inv);
    bq->a1 = (float)((-2.0 * cosw0) * inv);
    bq->a2 = (float)((1.0 - alpha / A) * inv);
}

/* Set unity (pass-through) coefficients without clearing delay state. */
static void mn_biquad_set_unity(mn_biquad *bq)
{
    bq->b0 = 1.0f;
    bq->b1 = 0.0f;
    bq->b2 = 0.0f;
    bq->a1 = 0.0f;
    bq->a2 = 0.0f;
}

/* Process a single sample through the biquad (TDF-II). */
static float mn_biquad_tick(mn_biquad *bq, float x)
{
    float y = bq->b0 * x + bq->s1;
    bq->s1 = bq->b1 * x - bq->a1 * y + bq->s2;
    bq->s2 = bq->b2 * x - bq->a2 * y;
    return y;
}

/* Zero the delay state (clears history without changing coefficients). */
static void mn_biquad_reset(mn_biquad *bq)
{
    bq->s1 = 0.0f;
    bq->s2 = 0.0f;
}

/* ------------------------------------------------------------------------- */
/* First-order low-pass (for LFE derivation on upmix)                         */
/* ------------------------------------------------------------------------- */

typedef struct mn_onepole {
    float a;    /* smoothing coefficient */
    float z;    /* state */
} mn_onepole;

static void mn_onepole_set_lp(mn_onepole *lp, float cutoff, float sample_rate)
{
    /* One-pole low-pass: y += a*(x - y), a = 1 - exp(-2*pi*fc/fs). */
    double a = 1.0 - exp(-2.0 * MN_PI * (double)cutoff / (double)sample_rate);
    lp->a = (float)mn_clampf((float)a, 0.0f, 1.0f);
}

static float mn_onepole_tick(mn_onepole *lp, float x)
{
    lp->z += lp->a * (x - lp->z);
    return lp->z;
}

static void mn_onepole_reset(mn_onepole *lp)
{
    lp->z = 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Parameter snapshot (published from control thread)                         */
/* ------------------------------------------------------------------------- */

/* Internal, fully-clamped mirror of mn_dsp_params. */
typedef struct mn_dsp_snapshot {
    int   eq_enabled;
    float preamp_db;
    float eq_gains_db[MN_DSP_EQ_BANDS];
    float balance;
    int   limiter_enabled;
    float limiter_threshold_db;
    float limiter_ceiling_db;
    float master_gain_db;
} mn_dsp_snapshot;

/* ------------------------------------------------------------------------- */
/* Instance                                                                   */
/* ------------------------------------------------------------------------- */

struct mn_dsp {
    /* Immutable-at-runtime configuration. */
    uint32_t      sample_rate;
    uint32_t      in_channels;
    uint32_t      out_channels;
    mn_dsp_layout out_layout;
    uint32_t      max_frames;
    int           configured;

    /* Double-buffered parameter snapshot + seqlock generation counter.
     * Control threads write into params[write_idx], then flip published and
     * bump generation. The audio thread reads published once per block. */
    mn_dsp_snapshot params[2];
    volatile long   generation;   /* bumped on every publish */
    int             write_idx;    /* index the control side may write next */
    long            seen_gen;     /* generation last consumed by audio thread */

    /* Live (ramped) scalar parameters used by the audio thread. */
    float cur_preamp_lin;
    float tgt_preamp_lin;
    float cur_balance;
    float tgt_balance;
    float cur_master_lin;
    float tgt_master_lin;

    /* Live EQ state. */
    int      eq_enabled;
    float    band_gain_db[MN_DSP_EQ_BANDS];  /* current applied gains */
    /* One biquad per band per input channel. */
    mn_biquad eq[MN_DSP_MAX_CHANNELS][MN_DSP_EQ_BANDS];

    /* Limiter state. */
    int   limiter_enabled;
    float limiter_threshold_lin;
    float limiter_ceiling_lin;

    /* LFE low-pass filters (one per channel that feeds the LFE derivation). */
    mn_onepole lfe_lp[2];

    /* Decorrelation all-pass-ish delay lines for surround channels. A short
     * fixed delay per surround channel decorrelates the upmixed rear content.
     * Sized at configure() time to max_frames-independent small buffers. */
    float  *surround_delay[4];    /* up to 4 surround/rear channels */
    uint32_t surround_delay_len[4]; /* per-channel ring length (distinct delays) */
    uint32_t surround_delay_pos[4];
};

/* ------------------------------------------------------------------------- */
/* Layout helpers                                                             */
/* ------------------------------------------------------------------------- */

uint32_t mn_dsp_layout_channels(mn_dsp_layout layout)
{
    switch (layout) {
        case MN_DSP_LAYOUT_MONO:   return 1;
        case MN_DSP_LAYOUT_STEREO: return 2;
        case MN_DSP_LAYOUT_5_1:    return 6;
        case MN_DSP_LAYOUT_7_1:    return 8;
        default:                   return 0;
    }
}

uint32_t mn_dsp_out_channels(const mn_dsp *dsp)
{
    return dsp ? dsp->out_channels : 0;
}

mn_dsp_layout mn_dsp_get_layout(const mn_dsp *dsp)
{
    return dsp ? dsp->out_layout : MN_DSP_LAYOUT_STEREO;
}

/* ------------------------------------------------------------------------- */
/* Snapshot / parameter defaults                                              */
/* ------------------------------------------------------------------------- */

/* Fill a snapshot with sane, clamped defaults. */
static void mn_snapshot_defaults(mn_dsp_snapshot *s)
{
    int i;
    s->eq_enabled = 0;
    s->preamp_db = 0.0f;
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i) s->eq_gains_db[i] = 0.0f;
    s->balance = 0.0f;
    s->limiter_enabled = 1;
    s->limiter_threshold_db = -3.0f;
    s->limiter_ceiling_db = -0.1f;
    s->master_gain_db = 0.0f;
}

/* Clamp every field of a snapshot into its legal range in place. */
static void mn_snapshot_clamp(mn_dsp_snapshot *s)
{
    int i;
    s->preamp_db = mn_clampf(s->preamp_db, MN_DSP_GAIN_MIN_DB, MN_DSP_GAIN_MAX_DB);
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        s->eq_gains_db[i] = mn_clampf(s->eq_gains_db[i],
                                      MN_DSP_GAIN_MIN_DB, MN_DSP_GAIN_MAX_DB);
    s->balance = mn_clampf(s->balance, MN_DSP_BALANCE_MIN, MN_DSP_BALANCE_MAX);
    /* Threshold and ceiling must be <= 0 dB and ceiling >= threshold. */
    if (s->limiter_threshold_db > 0.0f) s->limiter_threshold_db = 0.0f;
    if (s->limiter_ceiling_db > 0.0f)   s->limiter_ceiling_db = 0.0f;
    if (s->limiter_ceiling_db < s->limiter_threshold_db)
        s->limiter_ceiling_db = s->limiter_threshold_db;
    /* Keep gains bounded so linear conversions stay finite. */
    s->limiter_threshold_db = mn_clampf(s->limiter_threshold_db, -60.0f, 0.0f);
    s->limiter_ceiling_db = mn_clampf(s->limiter_ceiling_db, -60.0f, 0.0f);
    s->master_gain_db = mn_clampf(s->master_gain_db,
                                  MN_DSP_GAIN_MIN_DB, MN_DSP_GAIN_MAX_DB);
    s->eq_enabled = s->eq_enabled ? 1 : 0;
    s->limiter_enabled = s->limiter_enabled ? 1 : 0;
}

/* Read the currently published snapshot into *out (control-thread safe copy). */
static void mn_read_published(const mn_dsp *dsp, mn_dsp_snapshot *out)
{
    /* The published index is the opposite of write_idx. Copy under the seqlock
     * generation so a concurrent flip is detected and retried. */
    long g0, g1;
    do {
        g0 = dsp->generation;
        *out = dsp->params[1 - dsp->write_idx];
        g1 = dsp->generation;
    } while (g0 != g1);
}

/* Publish a new snapshot from the control thread. Not called on audio thread. */
static void mn_publish(mn_dsp *dsp, const mn_dsp_snapshot *s)
{
    int wi = dsp->write_idx;
    dsp->params[wi] = *s;
    /* Flip: the just-written buffer becomes the published one. */
    dsp->write_idx = 1 - wi;
    /* Bump generation to signal a new snapshot; the increment also invalidates
     * any in-flight read in mn_read_published(). */
    dsp->generation++;
}

/* Convenience: read the published snapshot, mutate it via callback data, and
 * republish. Used by the individual setters. */

/* ------------------------------------------------------------------------- */
/* Coefficient / derived-state updates (audio thread)                         */
/* ------------------------------------------------------------------------- */

/* Recompute all biquad coefficients for a band across every input channel,
 * preserving delay state. */
static void mn_update_band(mn_dsp *dsp, int band, float gain_db)
{
    uint32_t ch;
    for (ch = 0; ch < dsp->in_channels; ++ch) {
        if (fabsf(gain_db) < MN_DSP_GAIN_EPS_DB) {
            mn_biquad_set_unity(&dsp->eq[ch][band]);
        } else {
            mn_biquad_set_peaking(&dsp->eq[ch][band],
                                  MN_DSP_EQ_FREQUENCIES[band],
                                  (float)dsp->sample_rate,
                                  MN_DSP_EQ_Q, gain_db);
        }
    }
    dsp->band_gain_db[band] = gain_db;
}

/* Apply a freshly-consumed snapshot to the live audio-thread state, updating
 * only what changed. Runs on the audio thread; bounded work. */
static void mn_apply_snapshot(mn_dsp *dsp, const mn_dsp_snapshot *s)
{
    int i;

    dsp->eq_enabled = s->eq_enabled;
    dsp->limiter_enabled = s->limiter_enabled;

    dsp->tgt_preamp_lin = mn_db_to_lin(s->preamp_db);
    dsp->tgt_balance = s->balance;
    dsp->tgt_master_lin = mn_db_to_lin(s->master_gain_db);

    dsp->limiter_threshold_lin = mn_db_to_lin(s->limiter_threshold_db);
    dsp->limiter_ceiling_lin = mn_db_to_lin(s->limiter_ceiling_db);

    for (i = 0; i < MN_DSP_EQ_BANDS; ++i) {
        if (fabsf(s->eq_gains_db[i] - dsp->band_gain_db[i]) >= MN_DSP_GAIN_EPS_DB)
            mn_update_band(dsp, i, s->eq_gains_db[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/* Validate a config struct. Returns MN_DSP_OK or an error code. */
static mn_dsp_result mn_validate_config(const mn_dsp_config *cfg)
{
    if (!cfg) return MN_DSP_ERR_INVALID_ARG;
    if (cfg->sample_rate == 0) return MN_DSP_ERR_INVALID_ARG;
    if (cfg->in_channels != 1 && cfg->in_channels != 2)
        return MN_DSP_ERR_UNSUPPORTED;
    if (mn_dsp_layout_channels(cfg->out_layout) == 0)
        return MN_DSP_ERR_UNSUPPORTED;
    if (cfg->max_frames == 0) return MN_DSP_ERR_INVALID_ARG;
    return MN_DSP_OK;
}

/* Free any per-configuration allocations (surround delay lines). */
static void mn_free_buffers(mn_dsp *dsp)
{
    int i;
    for (i = 0; i < 4; ++i) {
        free(dsp->surround_delay[i]);
        dsp->surround_delay[i] = NULL;
        dsp->surround_delay_pos[i] = 0;
        dsp->surround_delay_len[i] = 0;
    }
}

/* Allocate per-configuration buffers based on the current config. Returns
 * MN_DSP_OK or MN_DSP_ERR_OUT_OF_MEMORY. */
static mn_dsp_result mn_alloc_buffers(mn_dsp *dsp)
{
    int i;
    uint32_t n_surround = 0;
    /* Distinct decorrelation delays (samples) per surround channel; chosen as
     * mutually prime-ish small values for a diffuse rear field. Scaled by
     * sample rate relative to 48 kHz reference. */
    static const uint32_t ref_delays[4] = { 313, 419, 277, 521 };
    double sr_scale = (double)dsp->sample_rate / 48000.0;

    if (dsp->out_layout == MN_DSP_LAYOUT_5_1) n_surround = 2;
    else if (dsp->out_layout == MN_DSP_LAYOUT_7_1) n_surround = 4;

    for (i = 0; i < 4; ++i) {
        dsp->surround_delay[i] = NULL;
        dsp->surround_delay_pos[i] = 0;
        dsp->surround_delay_len[i] = 0;
    }

    for (i = 0; i < (int)n_surround; ++i) {
        uint32_t d = (uint32_t)((double)ref_delays[i] * sr_scale);
        if (d < 1) d = 1;
        dsp->surround_delay_len[i] = d;
        dsp->surround_delay[i] = (float *)calloc(d, sizeof(float));
        if (!dsp->surround_delay[i]) {
            mn_free_buffers(dsp);
            return MN_DSP_ERR_OUT_OF_MEMORY;
        }
    }
    return MN_DSP_OK;
}

/* Rebuild all live audio-thread state from the published snapshot and config.
 * Snaps ramps to targets and clears transient state. Used at create/configure/
 * reset. */
static void mn_rebuild_state(mn_dsp *dsp)
{
    mn_dsp_snapshot s;
    uint32_t ch;
    int band;

    mn_read_published(dsp, &s);

    /* Force all bands to recompute regardless of prior gains. */
    for (band = 0; band < MN_DSP_EQ_BANDS; ++band)
        dsp->band_gain_db[band] = 1e30f; /* sentinel != any legal value */

    mn_apply_snapshot(dsp, &s);

    /* Snap ramps to their targets (no glide across a rebuild). */
    dsp->cur_preamp_lin = dsp->tgt_preamp_lin;
    dsp->cur_balance = dsp->tgt_balance;
    dsp->cur_master_lin = dsp->tgt_master_lin;

    /* Clear biquad + filter + delay state. */
    for (ch = 0; ch < MN_DSP_MAX_CHANNELS; ++ch)
        for (band = 0; band < MN_DSP_EQ_BANDS; ++band)
            mn_biquad_reset(&dsp->eq[ch][band]);

    mn_onepole_set_lp(&dsp->lfe_lp[0], 120.0f, (float)dsp->sample_rate);
    mn_onepole_set_lp(&dsp->lfe_lp[1], 120.0f, (float)dsp->sample_rate);
    mn_onepole_reset(&dsp->lfe_lp[0]);
    mn_onepole_reset(&dsp->lfe_lp[1]);

    {
        int i;
        for (i = 0; i < 4; ++i) {
            if (dsp->surround_delay[i] && dsp->surround_delay_len[i] > 0)
                memset(dsp->surround_delay[i], 0,
                       dsp->surround_delay_len[i] * sizeof(float));
            dsp->surround_delay_pos[i] = 0;
        }
    }

    dsp->seen_gen = dsp->generation;
}

mn_dsp_result mn_dsp_create(const mn_dsp_config *cfg, mn_dsp **out_dsp)
{
    mn_dsp *dsp;
    mn_dsp_result r;

    if (!out_dsp) return MN_DSP_ERR_INVALID_ARG;
    *out_dsp = NULL;

    r = mn_validate_config(cfg);
    if (r != MN_DSP_OK) return r;

    dsp = (mn_dsp *)calloc(1, sizeof(*dsp));
    if (!dsp) return MN_DSP_ERR_OUT_OF_MEMORY;

    dsp->sample_rate = cfg->sample_rate;
    dsp->in_channels = cfg->in_channels;
    dsp->out_layout = cfg->out_layout;
    dsp->out_channels = mn_dsp_layout_channels(cfg->out_layout);
    dsp->max_frames = cfg->max_frames;

    /* Initialize both snapshot buffers with defaults and publish one. */
    mn_snapshot_defaults(&dsp->params[0]);
    mn_snapshot_clamp(&dsp->params[0]);
    dsp->params[1] = dsp->params[0];
    dsp->write_idx = 0;      /* published index = 1 - 0 = 1 (holds defaults) */
    dsp->generation = 1;
    dsp->seen_gen = 0;

    r = mn_alloc_buffers(dsp);
    if (r != MN_DSP_OK) {
        free(dsp);
        return r;
    }

    dsp->configured = 1;
    mn_rebuild_state(dsp);

    *out_dsp = dsp;
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_configure(mn_dsp *dsp, const mn_dsp_config *cfg)
{
    mn_dsp_result r;
    mn_dsp_snapshot preserved;

    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    r = mn_validate_config(cfg);
    if (r != MN_DSP_OK) return r;

    /* Preserve current parameter state across the reconfigure. */
    mn_read_published(dsp, &preserved);

    mn_free_buffers(dsp);

    dsp->sample_rate = cfg->sample_rate;
    dsp->in_channels = cfg->in_channels;
    dsp->out_layout = cfg->out_layout;
    dsp->out_channels = mn_dsp_layout_channels(cfg->out_layout);
    dsp->max_frames = cfg->max_frames;

    r = mn_alloc_buffers(dsp);
    if (r != MN_DSP_OK) {
        dsp->configured = 0;
        return r;
    }

    /* Re-establish the preserved parameters in both buffers. */
    mn_snapshot_clamp(&preserved);
    dsp->params[0] = preserved;
    dsp->params[1] = preserved;
    dsp->write_idx = 0;
    dsp->generation++;

    dsp->configured = 1;
    mn_rebuild_state(dsp);
    return MN_DSP_OK;
}

void mn_dsp_destroy(mn_dsp *dsp)
{
    if (!dsp) return;
    mn_free_buffers(dsp);
    free(dsp);
}

void mn_dsp_reset(mn_dsp *dsp)
{
    if (!dsp || !dsp->configured) return;
    mn_rebuild_state(dsp);
}

/* ------------------------------------------------------------------------- */
/* Processing (real-time safe)                                                */
/* ------------------------------------------------------------------------- */

/* Soft-clip a single sample using a tanh-based knee between threshold and
 * ceiling. Below threshold the signal is linear; above it, it saturates smoothly
 * toward the ceiling and never exceeds it. */
static float mn_soft_clip(float x, float thresh, float ceiling)
{
    float sign, mag, over, range, shaped;

    sign = (x < 0.0f) ? -1.0f : 1.0f;
    mag = fabsf(x);

    if (mag <= thresh)
        return x;

    range = ceiling - thresh;
    if (range <= 1e-9f) {
        /* Degenerate: hard clip at ceiling. */
        return sign * (mag > ceiling ? ceiling : mag);
    }

    /* Normalize the overshoot into the knee and map through tanh so the output
     * asymptotically approaches `ceiling` but never crosses it. */
    over = (mag - thresh) / range;             /* >= 0 */
    shaped = thresh + range * tanhf(over);     /* in (thresh, ceiling) */
    if (shaped > ceiling) shaped = ceiling;    /* numerical safety */
    return sign * shaped;
}

/* Per-sample one-pole glide of a scalar toward its target. */
static float mn_glide(float cur, float tgt)
{
    return cur + (tgt - cur) * MN_DSP_RAMP;
}

/* Apply master gain then (optionally) the soft-clip limiter to one sample. */
static float mn_soft_clip_master(mn_dsp *dsp, float x, float master)
{
    x *= master;
    if (dsp->limiter_enabled)
        x = mn_soft_clip(x, dsp->limiter_threshold_lin, dsp->limiter_ceiling_lin);
    return x;
}

/* Push a sample into surround channel `i`'s decorrelation delay line and return
 * the delayed output. `i` in [0,4). No-op passthrough if no delay configured. */
static float mn_surround_delay_tick(mn_dsp *dsp, int i, float x)
{
    float *line = dsp->surround_delay[i];
    uint32_t len = dsp->surround_delay_len[i];
    uint32_t pos;
    float out;
    if (!line || len == 0)
        return x;
    pos = dsp->surround_delay_pos[i];
    out = line[pos];
    line[pos] = x;
    pos++;
    if (pos >= len) pos = 0;
    dsp->surround_delay_pos[i] = pos;
    return out;
}

/* Read one input frame's L/R (mono is duplicated). */
static void mn_read_lr(const float *frame, uint32_t in_ch, float *l, float *r)
{
    if (in_ch == 1) {
        *l = frame[0];
        *r = frame[0];
    } else {
        *l = frame[0];
        *r = frame[1];
    }
}

/* Compute equal-power balance gains for the L/R pair. balance in [-1,1]. */
static void mn_balance_gains(float balance, float *gl, float *gr)
{
    /* Map balance to an angle in [0, pi/2]; equal-power pan keeps perceived
     * loudness roughly constant. balance = -1 -> full left, +1 -> full right. */
    float t = (balance + 1.0f) * 0.5f;         /* 0..1 */
    float ang = t * (float)(MN_PI * 0.5);
    *gl = cosf(ang);
    *gr = sinf(ang);
    /* Normalize so center (balance=0) is unity per channel. cos(pi/4)=0.707,
     * scale by sqrt(2) to keep center at 1.0. */
    *gl *= 1.41421356f;
    *gr *= 1.41421356f;
}

mn_dsp_result mn_dsp_process(mn_dsp *dsp, float *buffer, uint32_t frames,
                             uint32_t channels)
{
    uint32_t out_ch;
    uint32_t f;

    if (!dsp || !buffer) return MN_DSP_ERR_INVALID_ARG;
    if (!dsp->configured) return MN_DSP_ERR_NOT_CONFIGURED;
    if (channels != dsp->in_channels) return MN_DSP_ERR_INVALID_ARG;
    if (frames == 0) return MN_DSP_OK;
    if (frames > dsp->max_frames) return MN_DSP_ERR_INVALID_ARG;

    out_ch = dsp->out_channels;

    /* Consume a new parameter snapshot at block boundary, if published. */
    {
        long gen = dsp->generation;
        if (gen != dsp->seen_gen) {
            mn_dsp_snapshot s;
            mn_read_published(dsp, &s);
            mn_apply_snapshot(dsp, &s);
            dsp->seen_gen = gen;
        }
    }

    /* Process frame by frame. Because out_ch >= in_ch for upmix and we always
     * write from low to high channel index reading L/R first, in-place upmix is
     * safe as long as we compute all outputs from the saved L/R before writing.
     * We stride the buffer by the LARGER of in/out channel counts so successive
     * output frames do not overwrite yet-unread input frames. */
    {
        uint32_t in_ch = dsp->in_channels;
        uint32_t stride = (out_ch > in_ch) ? out_ch : in_ch;

        /* ALWAYS walk forward. This loop used to run last-to-first when
         * upmixing, on the theory that a wider output frame could clobber an
         * unread input frame — but both the read and the write use the SAME
         * `stride`, so frames tile without overlap and each frame's L/R is
         * read before that same frame is written. Nothing could clobber.
         * Meanwhile EVERY stage in here is STATEFUL — the EQ biquads, the
         * parameter glide, the LFE one-poles and the surround delay lines —
         * so running the block backwards fed them time-reversed audio. A
         * causal filter driven backwards smears its ringing BEFORE each
         * transient, which is exactly what "the audio plays backwards"
         * sounds like. Verified with tools/dsp_reverse_test.c, which caught
         * the 5.1/7.1 layouts filtering anticausally. */
        (void)in_ch;

        for (f = 0; f < frames; ++f) {
            float *frame = buffer + (size_t)f * stride;
            float l, r;
            float preamp, master, bal_l, bal_r;

            /* Glide scalar params once per frame. */
            dsp->cur_preamp_lin = mn_glide(dsp->cur_preamp_lin, dsp->tgt_preamp_lin);
            dsp->cur_balance = mn_glide(dsp->cur_balance, dsp->tgt_balance);
            dsp->cur_master_lin = mn_glide(dsp->cur_master_lin, dsp->tgt_master_lin);
            preamp = dsp->cur_preamp_lin;
            master = dsp->cur_master_lin;

            mn_read_lr(frame, in_ch, &l, &r);

            /* Preamp. */
            l *= preamp;
            r *= preamp;

            /* EQ (per input channel). Mono runs one channel; stereo runs two. */
            if (dsp->eq_enabled) {
                int b;
                if (in_ch == 1) {
                    for (b = 0; b < MN_DSP_EQ_BANDS; ++b)
                        l = mn_biquad_tick(&dsp->eq[0][b], l);
                    r = l;
                } else {
                    for (b = 0; b < MN_DSP_EQ_BANDS; ++b) {
                        l = mn_biquad_tick(&dsp->eq[0][b], l);
                        r = mn_biquad_tick(&dsp->eq[1][b], r);
                    }
                }
            }

            /* Balance (equal-power) on the L/R pair. */
            mn_balance_gains(dsp->cur_balance, &bal_l, &bal_r);
            l *= bal_l;
            r *= bal_r;

            /* --- Channel mapping / upmix into `frame` (up to out_ch). --- */
            switch (dsp->out_layout) {
                case MN_DSP_LAYOUT_MONO: {
                    float m = 0.5f * (l + r);
                    frame[0] = mn_soft_clip_master(dsp, m, master);
                    break;
                }
                case MN_DSP_LAYOUT_STEREO: {
                    frame[0] = mn_soft_clip_master(dsp, l, master);
                    frame[1] = mn_soft_clip_master(dsp, r, master);
                    break;
                }
                case MN_DSP_LAYOUT_5_1:
                case MN_DSP_LAYOUT_7_1: {
                    /* Standard interleave: [L, R, C, LFE, RL, RR, (SL, SR)] */
                    float c   = 0.5f * (l + r);
                    float lfe;
                    float rear_l, rear_r;
                    float side_l, side_r;

                    /* LFE: sum, low-passed. Use both one-pole filters averaged
                     * for a steeper-ish rolloff. */
                    {
                        float bass = 0.5f * (l + r);
                        bass = mn_onepole_tick(&dsp->lfe_lp[0], bass);
                        bass = mn_onepole_tick(&dsp->lfe_lp[1], bass);
                        lfe = bass;
                    }

                    /* Decorrelated surrounds: difference signal delayed per
                     * channel to widen the rear field. */
                    {
                        float diff = 0.5f * (l - r);
                        rear_l = mn_surround_delay_tick(dsp, 0, l * 0.6f + diff);
                        rear_r = mn_surround_delay_tick(dsp, 1, r * 0.6f - diff);
                    }

                    frame[0] = mn_soft_clip_master(dsp, l, master);
                    frame[1] = mn_soft_clip_master(dsp, r, master);
                    frame[2] = mn_soft_clip_master(dsp, c, master);
                    frame[3] = mn_soft_clip_master(dsp, lfe, master);
                    frame[4] = mn_soft_clip_master(dsp, rear_l, master);
                    frame[5] = mn_soft_clip_master(dsp, rear_r, master);

                    if (dsp->out_layout == MN_DSP_LAYOUT_7_1) {
                        float diff2 = 0.5f * (l - r);
                        side_l = mn_surround_delay_tick(dsp, 2, l * 0.5f + diff2 * 0.5f);
                        side_r = mn_surround_delay_tick(dsp, 3, r * 0.5f - diff2 * 0.5f);
                        frame[6] = mn_soft_clip_master(dsp, side_l, master);
                        frame[7] = mn_soft_clip_master(dsp, side_r, master);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    return MN_DSP_OK;
}

/* ------------------------------------------------------------------------- */
/* Parameter control API                                                      */
/* ------------------------------------------------------------------------- */

mn_dsp_result mn_dsp_set_params(mn_dsp *dsp, const mn_dsp_params *params)
{
    mn_dsp_snapshot s;
    int i;
    if (!dsp || !params) return MN_DSP_ERR_INVALID_ARG;

    s.eq_enabled = params->eq_enabled;
    s.preamp_db = params->preamp_db;
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        s.eq_gains_db[i] = params->eq_gains_db[i];
    s.balance = params->balance;
    s.limiter_enabled = params->limiter_enabled;
    s.limiter_threshold_db = params->limiter_threshold_db;
    s.limiter_ceiling_db = params->limiter_ceiling_db;
    s.master_gain_db = params->master_gain_db;

    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_get_params(const mn_dsp *dsp, mn_dsp_params *out_params)
{
    mn_dsp_snapshot s;
    int i;
    if (!dsp || !out_params) return MN_DSP_ERR_INVALID_ARG;

    mn_read_published(dsp, &s);

    out_params->eq_enabled = s.eq_enabled;
    out_params->preamp_db = s.preamp_db;
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        out_params->eq_gains_db[i] = s.eq_gains_db[i];
    out_params->balance = s.balance;
    out_params->limiter_enabled = s.limiter_enabled;
    out_params->limiter_threshold_db = s.limiter_threshold_db;
    out_params->limiter_ceiling_db = s.limiter_ceiling_db;
    out_params->master_gain_db = s.master_gain_db;
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_eq_enabled(mn_dsp *dsp, int enabled)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.eq_enabled = enabled ? 1 : 0;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_eq_band(mn_dsp *dsp, uint32_t band, float gain_db)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    if (band >= MN_DSP_EQ_BANDS) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.eq_gains_db[band] = gain_db;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_eq_gains(mn_dsp *dsp, const float gains_db[MN_DSP_EQ_BANDS])
{
    mn_dsp_snapshot s;
    int i;
    if (!dsp || !gains_db) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        s.eq_gains_db[i] = gains_db[i];
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_preamp(mn_dsp *dsp, float preamp_db)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.preamp_db = preamp_db;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_eq_preset(mn_dsp *dsp, mn_dsp_eq_preset preset)
{
    mn_dsp_snapshot s;
    int i;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    if ((int)preset < 0 || preset >= MN_DSP_EQ_PRESET_COUNT)
        return MN_DSP_ERR_INVALID_ARG;

    mn_read_published(dsp, &s);
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        s.eq_gains_db[i] = MN_PRESETS[preset].gains[i];
    s.preamp_db = MN_PRESETS[preset].preamp;
    s.eq_enabled = 1;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

float mn_dsp_band_frequency(uint32_t band)
{
    if (band >= MN_DSP_EQ_BANDS) return 0.0f;
    return MN_DSP_EQ_FREQUENCIES[band];
}

const char *mn_dsp_preset_name(mn_dsp_eq_preset preset)
{
    if ((int)preset < 0 || preset >= MN_DSP_EQ_PRESET_COUNT) return NULL;
    return MN_PRESETS[preset].name;
}

mn_dsp_result mn_dsp_set_balance(mn_dsp *dsp, float balance)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.balance = balance;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_limiter_enabled(mn_dsp *dsp, int enabled)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.limiter_enabled = enabled ? 1 : 0;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_limiter(mn_dsp *dsp, float threshold_db, float ceiling_db)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.limiter_threshold_db = threshold_db;
    s.limiter_ceiling_db = ceiling_db;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

mn_dsp_result mn_dsp_set_master_gain(mn_dsp *dsp, float master_gain_db)
{
    mn_dsp_snapshot s;
    if (!dsp) return MN_DSP_ERR_INVALID_ARG;
    mn_read_published(dsp, &s);
    s.master_gain_db = master_gain_db;
    mn_snapshot_clamp(&s);
    mn_publish(dsp, &s);
    return MN_DSP_OK;
}

/* ------------------------------------------------------------------------- */
/* Introspection strings                                                      */
/* ------------------------------------------------------------------------- */

const char *mn_dsp_result_string(mn_dsp_result r)
{
    switch (r) {
        case MN_DSP_OK:                 return "OK";
        case MN_DSP_ERR_INVALID_ARG:    return "invalid argument";
        case MN_DSP_ERR_UNSUPPORTED:    return "unsupported layout/channels";
        case MN_DSP_ERR_OUT_OF_MEMORY:  return "out of memory";
        case MN_DSP_ERR_NOT_CONFIGURED: return "not configured";
        default:                        return "unknown error";
    }
}
