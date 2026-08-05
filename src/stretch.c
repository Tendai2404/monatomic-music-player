/*
 * stretch.c — WSOLA (Waveform-Similarity Overlap-Add) time-stretch.
 * -----------------------------------------------------------------
 * Classic speech-friendly time-scale modification: Hann-windowed grains of
 * N frames are overlap-added at a fixed synthesis hop (N/2); the ANALYSIS
 * hop through the source is speed * N/2, and each grain's exact source
 * position is refined within ±SEEK by maximising cross-correlation (on the
 * mono sum) against the previously laid tail — so grain joins land on
 * similar waveform phase and speech stays natural, pitch unchanged.
 *
 * Sizing (@44.1/48 kHz): N=2048 (~46/43 ms grain), hop 1024, seek ±512.
 * Correlation is evaluated every 4th sample over the first half of the
 * grain — ~0.5 M mac per synthesis block, ~22 M mac/s at 44.1 kHz: cheap
 * enough for the audio thread.
 *
 * The input FIFO holds source frames; `anchor` tracks the (fractional)
 * analysis position of the NEXT grain relative to the FIFO head. Consumed
 * frames are discarded from the head so the FIFO stays small (~16 K frames).
 */
#include "stretch.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ST_N        2048            /* grain length (frames)               */
#define ST_HOP      (ST_N / 2)      /* synthesis hop = 50% overlap         */
#define ST_SEEK     512             /* ± waveform-similarity search        */
#define ST_STEP     4               /* correlation decimation              */
#define ST_FIFO_CAP 16384           /* input FIFO frames (power of two)    */
#define ST_MIN_SPD  0.5f
#define ST_MAX_SPD  3.0f

struct mn_stretch {
    /* input FIFO (interleaved stereo) */
    float  *fifo;                   /* ST_FIFO_CAP * 2 floats              */
    int     head;                   /* first valid frame                   */
    int     count;                  /* valid frames                        */
    double  anchor;                 /* next grain's analysis pos (frames
                                     * past `head`), fractional            */
    /* synthesis state */
    float  *ola;                    /* overlap accumulator, ST_N * 2       */
    float  *win;                    /* Hann window, ST_N                   */
    int     ola_fill;               /* frames of ola[] still to emit ahead
                                     * of the next overlap (== ST_HOP once
                                     * running)                            */
    float  *tail;                   /* previous grain's overlap region
                                     * (mono), ST_HOP samples, for the
                                     * similarity search                   */
    bool    primed;                 /* first grain laid                    */
    bool    flushed;                /* source EOS signalled                */
    float   speed;
    /* emit buffer: frames of finished output not yet popped */
    float  *emit;                   /* ST_HOP * 2                          */
    int     emit_n, emit_off;
};

mn_stretch *mn_stretch_create(void)
{
    mn_stretch *s = (mn_stretch *)calloc(1, sizeof(*s));
    int i;
    if (!s) return NULL;
    s->fifo = (float *)malloc(sizeof(float) * ST_FIFO_CAP * 2);
    s->ola  = (float *)calloc((size_t)ST_N * 2, sizeof(float));
    s->win  = (float *)malloc(sizeof(float) * ST_N);
    s->tail = (float *)calloc((size_t)ST_HOP, sizeof(float));
    s->emit = (float *)malloc(sizeof(float) * ST_HOP * 2);
    if (!s->fifo || !s->ola || !s->win || !s->tail || !s->emit) {
        mn_stretch_destroy(s);
        return NULL;
    }
    for (i = 0; i < ST_N; i++) {
        s->win[i] = 0.5f - 0.5f * (float)cos(2.0 * 3.14159265358979323846 *
                                             (double)i / (double)ST_N);
    }
    s->speed = 1.0f;
    mn_stretch_reset(s);
    return s;
}

void mn_stretch_destroy(mn_stretch *s)
{
    if (!s) return;
    free(s->fifo); free(s->ola); free(s->win); free(s->tail); free(s->emit);
    free(s);
}

void mn_stretch_reset(mn_stretch *s)
{
    if (!s) return;
    s->head = 0; s->count = 0;
    s->anchor = 0.0;
    memset(s->ola, 0, sizeof(float) * ST_N * 2);
    memset(s->tail, 0, sizeof(float) * ST_HOP);
    s->ola_fill = 0;
    s->primed = false;
    s->flushed = false;
    s->emit_n = s->emit_off = 0;
}

void mn_stretch_set_speed(mn_stretch *s, float speed)
{
    if (!s) return;
    if (speed < ST_MIN_SPD) speed = ST_MIN_SPD;
    if (speed > ST_MAX_SPD) speed = ST_MAX_SPD;
    s->speed = speed;
}

float mn_stretch_get_speed(const mn_stretch *s)
{
    return s ? s->speed : 1.0f;
}

/* Source frames required in the FIFO to synthesize the next grain: the
 * grain itself plus the similarity search span past the anchor. */
static int st_required(const mn_stretch *s)
{
    return (int)(s->anchor + 0.5) + ST_N + ST_SEEK;
}

int mn_stretch_need_input(const mn_stretch *s)
{
    int req;
    if (!s || s->flushed) return 0;
    if (s->emit_n > s->emit_off) return 0;          /* output pending */
    req = st_required(s);
    return (req > s->count) ? (req - s->count) : 0;
}

int mn_stretch_push(mn_stretch *s, const float *src, int frames)
{
    int room, take;
    if (!s || !src || frames <= 0 || s->flushed) return 0;
    /* compact: discard frames before the earliest one still needed
     * (anchor - SEEK, floored at 0) so head stays near zero */
    {
        int keep_from = (int)s->anchor - ST_SEEK;
        if (keep_from > 0) {
            if (keep_from > s->count) keep_from = s->count;
            memmove(s->fifo, s->fifo + (size_t)keep_from * 2,
                    (size_t)(s->count - keep_from) * 2 * sizeof(float));
            s->count  -= keep_from;
            s->anchor -= keep_from;
        }
    }
    room = ST_FIFO_CAP - s->count;
    take = (frames < room) ? frames : room;
    if (take > 0) {
        memcpy(s->fifo + (size_t)s->count * 2, src,
               (size_t)take * 2 * sizeof(float));
        s->count += take;
    }
    return take;
}

void mn_stretch_flush(mn_stretch *s)
{
    if (s) s->flushed = true;
}

/* Lay the next grain into the OLA accumulator and stage ST_HOP frames for
 * emission. Returns false if there is not enough input (and not flushed). */
static bool st_synthesize(mn_stretch *s)
{
    int    base = (int)(s->anchor + 0.5);
    int    best = 0;
    int    avail = s->count - base;
    int    n, i;
    const float *g;

    if (!s->primed) {
        /* first grain: no similarity target yet — take it at the anchor */
        if (avail < ST_N) {
            if (!s->flushed) return false;
            if (avail <= 0) return false;
        }
    } else if (avail < ST_N + ST_SEEK) {
        if (!s->flushed) return false;
        if (avail <= ST_HOP) return false;    /* nothing meaningful left */
    }

    /* waveform-similarity search: best offset in [-SEEK, +SEEK] whose grain
     * head best matches the previous tail (mono, decimated). Clamped to the
     * available input range. */
    if (s->primed) {
        int lo = -ST_SEEK, hi = ST_SEEK;
        float best_score = -1e30f;
        int off;
        if (base + lo < 0) lo = -base;
        if (base + hi + ST_N > s->count) hi = s->count - ST_N - base;
        if (hi < lo) { lo = 0; hi = 0; }
        for (off = lo; off <= hi; off += ST_STEP) {
            const float *cand = s->fifo + (size_t)(base + off) * 2;
            float score = 0.0f;
            for (i = 0; i < ST_HOP; i += ST_STEP) {
                float mono = cand[(size_t)i * 2] + cand[(size_t)i * 2 + 1];
                score += mono * s->tail[i];
            }
            if (score > best_score) { best_score = score; best = off; }
        }
    }

    g = s->fifo + (size_t)(base + best) * 2;
    n = s->count - (base + best);
    if (n > ST_N) n = ST_N;
    if (n <= 0) return false;

    /* overlap-add the windowed grain */
    for (i = 0; i < n; i++) {
        float w = s->win[i];
        s->ola[(size_t)i * 2]     += g[(size_t)i * 2] * w;
        s->ola[(size_t)i * 2 + 1] += g[(size_t)i * 2 + 1] * w;
    }

    /* stage the first ST_HOP frames of the accumulator for emission */
    memcpy(s->emit, s->ola, sizeof(float) * ST_HOP * 2);
    s->emit_n = ST_HOP; s->emit_off = 0;

    /* slide the accumulator left by one hop */
    memmove(s->ola, s->ola + (size_t)ST_HOP * 2,
            sizeof(float) * ST_HOP * 2);
    memset(s->ola + (size_t)ST_HOP * 2, 0, sizeof(float) * ST_HOP * 2);

    /* remember this grain's upcoming overlap region (mono) as the next
     * similarity target: samples [HOP, 2*HOP) of the grain we just laid */
    for (i = 0; i < ST_HOP; i++) {
        int gi = i + ST_HOP;
        if (gi < n) {
            s->tail[i] = g[(size_t)gi * 2] + g[(size_t)gi * 2 + 1];
        } else {
            s->tail[i] = 0.0f;
        }
    }

    /* advance the analysis anchor by speed * hop (from the UNSHIFTED base,
     * so the similarity offset never accumulates drift) */
    s->anchor += (double)s->speed * (double)ST_HOP;
    s->primed = true;
    return true;
}

int mn_stretch_pop(mn_stretch *s, float *out, int max_frames)
{
    int produced = 0;
    if (!s || !out || max_frames <= 0) return 0;
    while (produced < max_frames) {
        int have = s->emit_n - s->emit_off;
        if (have > 0) {
            int take = (have < max_frames - produced) ? have
                                                      : max_frames - produced;
            memcpy(out + (size_t)produced * 2,
                   s->emit + (size_t)s->emit_off * 2,
                   (size_t)take * 2 * sizeof(float));
            s->emit_off += take;
            produced    += take;
            continue;
        }
        if (!st_synthesize(s)) break;    /* needs input, or drained */
    }
    return produced;
}
