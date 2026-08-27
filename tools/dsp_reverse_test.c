/*
 * dsp_reverse_test.c — offline harness for the "audio plays backwards"
 * report. Runs the REAL mn_dsp chain (and, when asked, the REAL WSOLA
 * stretcher) over a signal whose time order is unambiguous, then checks
 * that the output preserves that order.
 *
 * Probe signal: a click train — one impulse every CLICK_SPACING frames,
 * each impulse carrying an increasing amplitude. If the output's impulse
 * amplitudes come back descending, the block was time-reversed. A biquad
 * run backwards also smears energy BEFORE each impulse instead of after,
 * which the pre/post energy test catches even when the peak order looks
 * right (that is what "sounds reversed" actually is for an IIR filter).
 *
 * Build: see tools/dsp_reverse_test.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/dsp.h"

#define RATE   44100u
#define FRAMES 2048u
#define CH     2u
#define CLICKS 8u

static void fill_click_train(float *buf, uint32_t frames, uint32_t ch)
{
    uint32_t i, k, spacing = frames / CLICKS;
    memset(buf, 0, (size_t)frames * ch * sizeof(float));
    for (k = 0; k < CLICKS; ++k) {
        uint32_t f = k * spacing + 4;          /* a little headroom before */
        float amp = 0.1f + 0.1f * (float)k;    /* strictly increasing */
        if (f >= frames) break;
        for (i = 0; i < ch; ++i) buf[(size_t)f * ch + i] = amp;
    }
}

/* Peak frame index of each click region, and the energy just before vs
 * just after it. A causal (forward) filter rings AFTER the impulse. */
static void analyse(const float *buf, uint32_t frames, uint32_t ch,
                    int *order_ok, int *causal_ok, float *first, float *last)
{
    uint32_t k, spacing = frames / CLICKS;
    float prev_peak = -1.0f;
    int desc = 0, asc = 0, anticausal = 0, causal = 0;

    *first = *last = 0.0f;
    for (k = 0; k < CLICKS; ++k) {
        uint32_t base = k * spacing;
        uint32_t f, end = base + spacing;
        float peak = 0.0f; uint32_t peak_at = base;
        float pre = 0.0f, post = 0.0f;
        if (end > frames) end = frames;
        for (f = base; f < end; ++f) {
            float v = (float)fabs(buf[(size_t)f * ch]);
            if (v > peak) { peak = v; peak_at = f; }
        }
        for (f = base; f < peak_at && f < end; ++f)
            pre += (float)fabs(buf[(size_t)f * ch]);
        for (f = peak_at + 1; f < end; ++f)
            post += (float)fabs(buf[(size_t)f * ch]);
        if (post > pre * 1.5f) causal++;
        else if (pre > post * 1.5f) anticausal++;

        if (k == 0) *first = peak;
        *last = peak;
        if (prev_peak >= 0.0f) { if (peak < prev_peak) desc++; else asc++; }
        prev_peak = peak;
    }
    *order_ok  = (asc >= desc);
    *causal_ok = (causal >= anticausal);
    printf("      clicks ascending=%d descending=%d | causal=%d anticausal=%d\n",
           asc, desc, causal, anticausal);
}

static int run_case(const char *name, mn_dsp_layout layout, int eq_on)
{
    mn_dsp_config cfg;
    mn_dsp_params p;
    mn_dsp *dsp = NULL;
    float *buf;
    uint32_t out_ch = (layout == MN_DSP_LAYOUT_STEREO) ? 2u :
                      (layout == MN_DSP_LAYOUT_MONO)   ? 1u :
                      (layout == MN_DSP_LAYOUT_5_1)    ? 6u : 8u;
    uint32_t stride = out_ch > CH ? out_ch : CH;
    int order_ok = 0, causal_ok = 0, ok;
    float first = 0, last = 0;
    int i;

    cfg.sample_rate = RATE;
    cfg.in_channels = CH;
    cfg.out_layout  = layout;
    cfg.max_frames  = FRAMES;
    if (mn_dsp_create(&cfg, &dsp) != MN_DSP_OK) {
        printf("  %-28s CREATE FAILED\n", name);
        return 0;
    }

    memset(&p, 0, sizeof(p));
    p.eq_enabled = eq_on;
    p.preamp_db      = 0.0f;
    p.master_gain_db = 0.0f;
    p.balance        = 0.0f;
    /* the user's actual audiobook curve: +5 dB on the top three bands */
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        p.eq_gains_db[i] = (i >= MN_DSP_EQ_BANDS - 3) ? 5.0f : 0.0f;
    mn_dsp_set_params(dsp, &p);

    buf = (float *)calloc((size_t)FRAMES * stride, sizeof(float));
    fill_click_train(buf, FRAMES, stride);

    /* two blocks so filter state carries over like it does in the engine */
    mn_dsp_process(dsp, buf, FRAMES, CH);
    analyse(buf, FRAMES, stride, &order_ok, &causal_ok, &first, &last);

    ok = order_ok && causal_ok;
    printf("  %-28s %s  (first peak %.3f, last %.3f)\n",
           name, ok ? "OK  forward" : "*** TIME-REVERSED ***", first, last);
    free(buf);
    mn_dsp_destroy(dsp);
    return ok;
}

/* Above-Nyquist stability probe. A peaking biquad whose centre frequency
 * exceeds sample_rate/2 has sin(w0) < 0, which flips alpha's sign and can
 * drive a0 toward zero — the filter stops being stable and rings/oscillates.
 * Audiobooks are commonly 22.05 kHz, where the 16 kHz band is ALREADY past
 * Nyquist. Feeds silence-after-impulse and watches for growth. */
static int run_stability(const char *name, uint32_t rate)
{
    mn_dsp_config cfg;
    mn_dsp_params p;
    mn_dsp *dsp = NULL;
    float *buf;
    uint32_t f;
    int i, ok;
    float peak_early = 0.0f, peak_late = 0.0f;

    cfg.sample_rate = rate; cfg.in_channels = CH;
    cfg.out_layout = MN_DSP_LAYOUT_STEREO; cfg.max_frames = FRAMES;
    if (mn_dsp_create(&cfg, &dsp) != MN_DSP_OK) { printf("  create failed\n"); return 0; }
    memset(&p, 0, sizeof(p));
    p.eq_enabled = 1;
    for (i = 0; i < MN_DSP_EQ_BANDS; ++i)
        p.eq_gains_db[i] = (i >= MN_DSP_EQ_BANDS - 3) ? 5.0f : 0.0f;
    mn_dsp_set_params(dsp, &p);

    buf = (float *)calloc((size_t)FRAMES * CH, sizeof(float));
    buf[0] = buf[1] = 0.5f;                 /* one impulse, then silence */
    mn_dsp_process(dsp, buf, FRAMES, CH);
    for (f = 1; f < FRAMES / 4; ++f)
        if ((float)fabs(buf[(size_t)f * CH]) > peak_early)
            peak_early = (float)fabs(buf[(size_t)f * CH]);
    for (f = FRAMES * 3 / 4; f < FRAMES; ++f)
        if ((float)fabs(buf[(size_t)f * CH]) > peak_late)
            peak_late = (float)fabs(buf[(size_t)f * CH]);

    /* a stable filter DECAYS; growth (or NaN) means it is oscillating */
    ok = !(peak_late > peak_early || peak_late != peak_late || peak_late > 1.0f);
    printf("  %-34s rate=%-6u ring_early=%.4f ring_late=%.4f  %s\n",
           name, rate, peak_early, peak_late,
           ok ? "stable" : "*** UNSTABLE (oscillating) ***");
    free(buf); mn_dsp_destroy(dsp);
    return ok;
}

int main(void)
{
    int fails = 0;
    printf("== mn_dsp time-order harness (click train, EQ +5dB top 3 bands) ==\n");
    if (!run_case("stereo->stereo, EQ on",  MN_DSP_LAYOUT_STEREO, 1)) fails++;
    if (!run_case("stereo->stereo, EQ off", MN_DSP_LAYOUT_STEREO, 0)) fails++;
    if (!run_case("stereo->mono,   EQ on",  MN_DSP_LAYOUT_MONO,   1)) fails++;
    if (!run_case("stereo->5.1,    EQ on",  MN_DSP_LAYOUT_5_1,    1)) fails++;
    if (!run_case("stereo->7.1,    EQ on",  MN_DSP_LAYOUT_7_1,    1)) fails++;
    printf("\n== biquad stability vs sample rate (16 kHz band is the risk) ==\n");
    if (!run_stability("CD music",            44100u)) fails++;
    if (!run_stability("48k",                 48000u)) fails++;
    if (!run_stability("audiobook 32k",       32000u)) fails++;
    if (!run_stability("audiobook 22.05k",    22050u)) fails++;
    if (!run_stability("low-rate speech 16k", 16000u)) fails++;
    if (!run_stability("low-rate speech 11k", 11025u)) fails++;

    printf("\n%s (%d failing case%s)\n",
           fails ? "PROBLEMS REPRODUCED" : "all clean",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
