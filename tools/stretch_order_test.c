/*
 * stretch_order_test.c — does the WSOLA stretcher ever emit audio out of
 * time order? (Chasing a "the audiobook started playing backwards" report.)
 *
 * Probe: a monotonically RISING ramp. Every output frame must be >= the
 * previous one, apart from the small dips overlap-add can create at grain
 * seams. A grain emitted from the past shows up as a LARGE backward jump,
 * which is what "playing backwards" is. We report the worst backward jump
 * and how far back in source time it implies.
 *
 * Also exercises the exact transition the user hit: changing settings
 * (speed re-sent / reset) MID-STREAM while audio is flowing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/stretch.h"

#define CH 2

static int check_monotone(const float *out, int n, float *worst_drop)
{
    int i, bad = 0;
    float prev = out[0];
    *worst_drop = 0.0f;
    for (i = 1; i < n; ++i) {
        float v = out[(size_t)i * CH];
        float d = prev - v;                 /* positive = went backwards */
        if (d > *worst_drop) *worst_drop = d;
        if (d > 0.02f) bad++;               /* >2% of full ramp = a real jump */
        prev = v;
    }
    return bad;
}

/* speed: source-time per output-time. mid_change != 0 re-sets the speed
 * halfway through, the way a settings change does. */
static int run(const char *name, float speed, int mid_change, float mid_speed)
{
    mn_stretch *s = mn_stretch_create();
    const int SRC = 200000;                  /* ~4.5 s of source at 44.1k */
    const int OUTCAP = 400000;
    float *src = (float *)malloc((size_t)SRC * CH * sizeof(float));
    float *out = (float *)malloc((size_t)OUTCAP * CH * sizeof(float));
    int i, pushed = 0, got = 0, bad, halfway;
    float worst = 0.0f;

    for (i = 0; i < SRC; ++i) {             /* rising ramp 0..1 */
        float v = (float)i / (float)SRC;
        src[(size_t)i * CH] = v;
        src[(size_t)i * CH + 1] = v;
    }
    mn_stretch_set_speed(s, speed);
    halfway = SRC / 2;

    while (pushed < SRC && got < OUTCAP - 4096) {
        int chunk = 2048;
        int acc;
        if (pushed + chunk > SRC) chunk = SRC - pushed;
        acc = mn_stretch_push(s, src + (size_t)pushed * CH, chunk);
        if (acc > 0) pushed += acc;
        if (mid_change && pushed >= halfway) {
            mn_stretch_set_speed(s, mid_speed);
            mid_change = 0;                 /* once */
        }
        for (;;) {
            int n = mn_stretch_pop(s, out + (size_t)got * CH, 2048);
            if (n <= 0) break;
            got += n;
            if (got >= OUTCAP - 4096) break;
        }
        if (acc == 0 && mn_stretch_need_input(s) == 0) break;
    }
    mn_stretch_flush(s);
    for (;;) {
        int n = mn_stretch_pop(s, out + (size_t)got * CH, 2048);
        if (n <= 0 || got >= OUTCAP - 4096) break;
        got += n;
    }

    bad = got > 1 ? check_monotone(out, got, &worst) : 0;
    printf("  %-40s out=%6d  backward-jumps=%-4d worst=%.4f  %s\n",
           name, got, bad, worst,
           (bad == 0) ? "OK forward" : "*** OUT OF ORDER ***");
    free(src); free(out);
    mn_stretch_destroy(s);
    return bad == 0;
}

int main(void)
{
    int fails = 0;
    printf("== WSOLA time-order harness (rising ramp) ==\n");
    if (!run("speed 1.5 (typical audiobook)", 1.5f, 0, 0)) fails++;
    if (!run("speed 2.0", 2.0f, 0, 0)) fails++;
    if (!run("speed 0.75", 0.75f, 0, 0)) fails++;
    if (!run("speed 3.0 (max)", 3.0f, 0, 0)) fails++;
    if (!run("speed 1.5 -> 2.0 mid-stream", 1.5f, 1, 2.0f)) fails++;
    if (!run("speed 1.5 -> 1.0 mid-stream", 1.5f, 1, 1.0f)) fails++;
    if (!run("speed 2.0 -> 0.75 mid-stream", 2.0f, 1, 0.75f)) fails++;
    printf("\n%s (%d failing case%s)\n",
           fails ? "OUT-OF-ORDER REPRODUCED" : "stretcher always forward",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
