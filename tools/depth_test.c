/* depth_test.c -- standalone test harness for src/depth.c (Monatomic).
 *
 *   depth_test <model.onnx> <image_in> <depth_out.png>
 *
 * Runs the module end-to-end, then re-decodes the written depth PNG and
 * verifies: it exists, matches the source image's WxH, is 8-bit grayscale,
 * and has a non-degenerate value spread (prints min/max/mean).
 *
 * Built as its OWN exe (never part of the app), so THIS translation unit
 * carries the stb IMPLEMENTATIONs that the app normally gets from artcache.c.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include "depth.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
static double now_ms(void) { return (double)GetTickCount64(); }
#else
#  include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: depth_test <model.onnx> <image_in> "
                        "<depth_out.png>\n");
        return 2;
    }
    const char *model = argv[1], *img = argv[2], *out = argv[3];

    /* Source dimensions (for the size check). */
    int sw = 0, sh = 0, sc = 0;
    if (!stbi_info(img, &sw, &sh, &sc)) {
        printf("FAIL: cannot read input image %s\n", img);
        return 1;
    }

    double t0 = now_ms();
    mn_depth *d = mn_depth_create(model);
    double t_create = now_ms() - t0;
    if (d == NULL) {
        printf("FAIL: mn_depth_create returned NULL (model missing or "
               "session error)\n");
        return 1;
    }

    t0 = now_ms();
    bool ok = mn_depth_generate(d, img, out);
    double t_gen = now_ms() - t0;
    mn_depth_destroy(d);
    if (!ok) {
        printf("FAIL: mn_depth_generate returned false\n");
        return 1;
    }

    /* Verify the artifact. */
    int dw = 0, dh = 0, dc = 0;
    unsigned char *depth = stbi_load(out, &dw, &dh, &dc, 1);
    if (depth == NULL) {
        printf("FAIL: output %s missing or not decodable\n", out);
        return 1;
    }
    int native_c = 0;
    (void)stbi_info(out, &dw, &dh, &native_c);

    unsigned mn = 255, mx = 0;
    unsigned long long sum = 0;
    size_t n = (size_t)dw * (size_t)dh;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = depth[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    stbi_image_free(depth);

    int size_ok   = (dw == sw && dh == sh);
    int gray_ok   = (native_c == 1);
    int spread_ok = (mx > mn) && (mx - mn >= 32); /* non-degenerate field */

    printf("input : %s (%dx%d, %d ch)\n", img, sw, sh, sc);
    printf("output: %s (%dx%d, %d ch)\n", out, dw, dh, native_c);
    printf("depth : min=%u max=%u mean=%.1f\n", mn, mx,
           n ? (double)sum / (double)n : 0.0);
    printf("timing: create %.0f ms, generate %.0f ms\n", t_create, t_gen);

    if (!size_ok)   printf("FAIL: size mismatch (expected %dx%d)\n", sw, sh);
    if (!gray_ok)   printf("FAIL: output is not single-channel grayscale\n");
    if (!spread_ok) printf("FAIL: degenerate depth spread (min=%u max=%u)\n",
                           mn, mx);
    if (size_ok && gray_ok && spread_ok) {
        printf("OK (%.0f ms total inference path)\n", t_gen);
        return 0;
    }
    return 1;
}
