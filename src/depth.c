/* ==========================================================================
 * depth.c -- Monatomic Audio Player
 *
 * Implementation of depth.h: cover-art depth-map generation with
 * Depth-Anything-V2-Small via the ONNX Runtime C API, CPU execution provider.
 *
 * Pipeline (mn_depth_generate)
 * ----------------------------
 *   1. Decode the source image with stb_image (forced to 3-channel RGB).
 *   2. Resize to the model's input resolution (518x518) with
 *      stb_image_resize2. Covers are square in practice, so a direct resize
 *      is used (no letterbox padding); for non-square inputs the depth field
 *      is stretched and un-stretched symmetrically by the final resize, which
 *      is visually indistinguishable for a parallax mesh.
 *   3. HWC uint8 -> CHW float32 with ImageNet normalization
 *      (mean .485/.456/.406, std .229/.224/.225).
 *   4. Run the session (serialized by a mutex; see depth.h thread-safety).
 *   5. The output is RELATIVE INVERSE DEPTH (one channel, larger = nearer).
 *      Min-max normalize to 0..1 so 1 = nearest.
 *   6. Bilinear-resize the 518x518 float field back to the source image's
 *      dimensions, quantize to 8-bit gray, write a PNG (white = near).
 *
 * stb usage: the single-TU IMPLEMENTATIONs of stb_image / stb_image_resize2 /
 * stb_image_write are compiled in artcache.c; this file includes the headers
 * as declarations only. (The standalone test harness, tools/depth_test.c,
 * carries its own implementation defines because it links without artcache.)
 *
 * ONNX Runtime patterns follow stems.c (OrtGetApiBase, status helper that
 * logs + releases, queried IO names, UTF-8 -> ORTCHAR_T path conversion).
 * Unlike stems.c there is NO CUDA provider here -- the GPU belongs to the
 * stems engine; this model is tiny and runs in ~1-2 s on a few CPU threads.
 * ========================================================================== */

#include "depth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "onnxruntime_c_api.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#endif

/* --------------------------------------------------------------------------
 * Constants.
 * -------------------------------------------------------------------------- */

/* Depth-Anything-V2 input resolution for DYNAMIC-shape exports (multiple of the
 * ViT patch size 14): 770 = 55*14, higher than the canonical 518 for finer
 * depth detail on the RTX-class hardware this targets. When the export carries
 * a STATIC shape we honor the model's own value instead. If a dynamic session
 * rejects 770 at create time we fall back to MN_DEPTH_NET_FALLBACK. */
#define MN_DEPTH_NET_DEFAULT  770
#define MN_DEPTH_NET_FALLBACK 518
/* Larger Depth-Anything tiers can afford (and benefit from) bigger inputs.
 * All sizes are ViT patch multiples (14): 770=55*14, 1036=74*14, 1288=92*14.
 * Depth maps are generated ONCE per album on the low-priority worker and
 * cached to disk, so the (much) slower per-image inference at these sizes is
 * a one-time cost, not a browsing cost. */
#define MN_DEPTH_NET_BASE     1036
#define MN_DEPTH_NET_LARGE    1288

/* Intra-op thread cap: enough to keep a cover under ~2 s without turning a
 * library scan into a CPU stampede (callers may queue many covers). */
#define MN_DEPTH_INTRA_THREADS 4

/* ImageNet normalization used by the DINOv2 backbone. */
static const float MN_DEPTH_MEAN[3] = { 0.485f, 0.456f, 0.406f };
static const float MN_DEPTH_STD[3]  = { 0.229f, 0.224f, 0.225f };

/* --------------------------------------------------------------------------
 * Minimal platform mutex (same shape as stems.c's shim, private to this TU).
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
typedef CRITICAL_SECTION mn_depth_mutex;
static void mn_depth_mutex_init(mn_depth_mutex *m)    { InitializeCriticalSection(m); }
static void mn_depth_mutex_destroy(mn_depth_mutex *m) { DeleteCriticalSection(m); }
static void mn_depth_mutex_lock(mn_depth_mutex *m)    { EnterCriticalSection(m); }
static void mn_depth_mutex_unlock(mn_depth_mutex *m)  { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t mn_depth_mutex;
static void mn_depth_mutex_init(mn_depth_mutex *m)    { pthread_mutex_init(m, NULL); }
static void mn_depth_mutex_destroy(mn_depth_mutex *m) { pthread_mutex_destroy(m); }
static void mn_depth_mutex_lock(mn_depth_mutex *m)    { pthread_mutex_lock(m); }
static void mn_depth_mutex_unlock(mn_depth_mutex *m)  { pthread_mutex_unlock(m); }
#endif

/* --------------------------------------------------------------------------
 * Handle.
 * -------------------------------------------------------------------------- */
struct mn_depth {
    const OrtApi      *ort;         /* API function table (never freed)      */
    OrtEnv            *env;
    OrtSessionOptions *sopts;
    OrtSession        *session;
    OrtMemoryInfo     *mem_info;    /* CPU memory info for input tensors     */
    OrtAllocator      *allocator;   /* default allocator (not owned)         */
    char              *input_name;  /* queried (allocator memory)            */
    char              *output_name; /* queried (allocator memory)            */
    int                net_w;       /* model input width  (518 or 770)       */
    int                net_h;       /* model input height (518 or 770)       */
    int                dynamic;     /* 1 if the export accepts arbitrary HxW  */
    mn_depth_mutex     run_mutex;   /* serializes Run() across worker threads */
};

/* --------------------------------------------------------------------------
 * ORT status helper: 1 on error (logged + released), 0 on OK. (stems.c style)
 * -------------------------------------------------------------------------- */
static int mn_depth_ort_failed(const OrtApi *ort, OrtStatus *status,
                               const char *what) {
    if (status == NULL) {
        return 0;
    }
    fprintf(stderr, "[depth] %s failed: %s\n",
            what ? what : "ORT call", ort->GetErrorMessage(status));
    fflush(stderr);
    ort->ReleaseStatus(status);
    return 1;
}

/* UTF-8 -> ORTCHAR_T for CreateSession (wchar_t on Windows). Caller frees. */
static ORTCHAR_T *mn_depth_to_ortchar(const char *utf8) {
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

/* Regular-file existence probe (model presence check before ORT bring-up). */
static int mn_depth_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Query the model's input spatial dims (NCHW). Any dynamic (-1) or missing
 * dimension falls back to MN_DEPTH_NET_DEFAULT. Also sanity-checks that the
 * input element type is float32 (the 27 MB export is weight-quantized only;
 * its IO stays float). Returns 0 on success, -1 if the input is not a float
 * tensor.
 * -------------------------------------------------------------------------- */
static int mn_depth_query_input_dims(mn_depth *d) {
    const OrtApi *ort = d->ort;
    OrtTypeInfo *tinfo = NULL;
    const OrtTensorTypeAndShapeInfo *shape = NULL; /* owned by tinfo */
    int rc = -1;

    d->net_w = MN_DEPTH_NET_DEFAULT;
    d->net_h = MN_DEPTH_NET_DEFAULT;
    d->dynamic = 1;   /* assume dynamic unless a static dim is reported below */

    if (mn_depth_ort_failed(ort,
            ort->SessionGetInputTypeInfo(d->session, 0, &tinfo),
            "SessionGetInputTypeInfo")) {
        return -1;
    }
    if (!mn_depth_ort_failed(ort,
            ort->CastTypeInfoToTensorInfo(tinfo, &shape),
            "CastTypeInfoToTensorInfo") && shape != NULL) {
        ONNXTensorElementDataType elem =
            ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        size_t ndims = 0;
        (void)mn_depth_ort_failed(ort, ort->GetTensorElementType(shape, &elem),
                                  "GetTensorElementType");
        if (elem == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            rc = 0;
        } else {
            fprintf(stderr,
                    "[depth] model input element type %d is not float32; "
                    "unsupported export\n", (int)elem);
            fflush(stderr);
        }
        if (rc == 0 &&
            !mn_depth_ort_failed(ort, ort->GetDimensionsCount(shape, &ndims),
                                 "GetDimensionsCount") && ndims == 4) {
            int64_t dims[4] = { 0 };
            if (!mn_depth_ort_failed(ort,
                    ort->GetDimensions(shape, dims, 4), "GetDimensions")) {
                if (dims[2] > 0) {
                    d->net_h = (int)dims[2];
                    d->dynamic = 0;   /* export pins the spatial size */
                }
                if (dims[3] > 0) {
                    d->net_w = (int)dims[3];
                    d->dynamic = 0;
                }
            }
        }
    }
    ort->ReleaseTypeInfo(tinfo);
    return rc;
}

/* Probe a dynamic-shape session at d->net_w x d->net_h with a throwaway input.
 * Returns 1 if the model accepts that resolution, 0 if Run() rejects it (some
 * exports are effectively pinned even when the graph dims read as dynamic).
 * Static-shape models are never probed (they already succeeded/matched). */
static int mn_depth_probe_resolution(mn_depth *d) {
    const OrtApi *ort = d->ort;
    const size_t npix = (size_t)d->net_w * (size_t)d->net_h;
    float *chw = (float *)calloc(npix * 3, sizeof(float));
    OrtValue *in_val = NULL, *out_val = NULL;
    int ok = 0;
    if (!chw) return 0;
    {
        int64_t in_shape[4] = { 1, 3, d->net_h, d->net_w };
        const char *in_names[1]  = { d->input_name };
        const char *out_names[1] = { d->output_name };
        if (!mn_depth_ort_failed(ort, ort->CreateTensorWithDataAsOrtValue(
                d->mem_info, chw, npix * 3 * sizeof(float),
                in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val),
                "probe CreateTensor") && in_val &&
            !mn_depth_ort_failed(ort, ort->Run(d->session, NULL,
                in_names, (const OrtValue *const *)&in_val, 1,
                out_names, 1, &out_val), "probe Run") && out_val) {
            ok = 1;
        }
    }
    if (out_val) ort->ReleaseValue(out_val);
    if (in_val)  ort->ReleaseValue(in_val);
    free(chw);
    return ok;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

mn_depth *mn_depth_create(const char *model_path) {
    if (model_path == NULL || model_path[0] == '\0' ||
        !mn_depth_file_exists(model_path)) {
        return NULL; /* model missing => feature disabled, not an error */
    }

    mn_depth *d = (mn_depth *)calloc(1, sizeof *d);
    if (d == NULL) {
        return NULL;
    }
    mn_depth_mutex_init(&d->run_mutex);

    d->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (d->ort == NULL) {
        goto fail;
    }
    const OrtApi *ort = d->ort;

    if (mn_depth_ort_failed(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                                "mn_depth", &d->env),
                            "CreateEnv")) {
        goto fail;
    }
    if (mn_depth_ort_failed(ort, ort->CreateSessionOptions(&d->sopts),
                            "CreateSessionOptions")) {
        goto fail;
    }
    ort->SetSessionGraphOptimizationLevel(d->sopts, ORT_ENABLE_ALL);
    /* CPU EP only, few threads: never contend with the stems engine's CUDA
     * session or hog cores during a scan. Scaled to the machine: half the
     * cores capped at MN_DEPTH_INTRA_THREADS (a fixed 4 oversubscribed
     * 2-core boxes); low-power mode pins it to 1. */
    {
        int threads = MN_DEPTH_INTRA_THREADS;
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si);
        threads = (int)si.dwNumberOfProcessors / 2;
#endif
        if (threads < 1) threads = 1;
        if (threads > MN_DEPTH_INTRA_THREADS) threads = MN_DEPTH_INTRA_THREADS;
        if (getenv("MN_LOWPOWER")) threads = 1;
        ort->SetIntraOpNumThreads(d->sopts, threads);
    }

    {
        ORTCHAR_T *wpath = mn_depth_to_ortchar(model_path);
        if (wpath == NULL) {
            goto fail;
        }
        int bad = mn_depth_ort_failed(ort,
                ort->CreateSession(d->env, wpath, d->sopts, &d->session),
                "CreateSession");
        free(wpath);
        if (bad || d->session == NULL) {
            goto fail;
        }
    }

    if (mn_depth_ort_failed(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator,
                                                          OrtMemTypeDefault,
                                                          &d->mem_info),
                            "CreateCpuMemoryInfo")) {
        goto fail;
    }
    if (mn_depth_ort_failed(ort,
            ort->GetAllocatorWithDefaultOptions(&d->allocator),
            "GetAllocatorWithDefaultOptions")) {
        goto fail;
    }

    /* Query IO names from the model rather than hardcoding export choices. */
    if (mn_depth_ort_failed(ort,
            ort->SessionGetInputName(d->session, 0, d->allocator,
                                     &d->input_name),
            "SessionGetInputName") ||
        mn_depth_ort_failed(ort,
            ort->SessionGetOutputName(d->session, 0, d->allocator,
                                      &d->output_name),
            "SessionGetOutputName") ||
        d->input_name == NULL || d->output_name == NULL) {
        goto fail;
    }

    if (mn_depth_query_input_dims(d) != 0) {
        goto fail;
    }

    /* Dynamic exports: pick the largest input the model tier can afford
     * (small→770, base→1036, large→1288 — tier read from the filename), then
     * verify it actually runs and walk down the ladder on rejection. The
     * probe is one throwaway inference, paid once at session create on the
     * background loader thread. Static exports are trusted as-is. */
    if (d->dynamic && d->net_w == MN_DEPTH_NET_DEFAULT) {
        int  ladder[3] = { MN_DEPTH_NET_DEFAULT,
                           MN_DEPTH_NET_DEFAULT,
                           MN_DEPTH_NET_FALLBACK };
        int  li;
        /* tier by filename (case-insensitive) */
        {
            char low[512];
            size_t k;
            for (k = 0; model_path[k] && k + 1 < sizeof(low); ++k) {
                char c = model_path[k];
                low[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            low[k] = '\0';
            if (strstr(low, "large"))      ladder[0] = MN_DEPTH_NET_LARGE;
            else if (strstr(low, "base"))  ladder[0] = MN_DEPTH_NET_BASE;
        }
        for (li = 0; li < 3; ++li) {
            if (li > 0 && ladder[li] == ladder[li - 1]) continue;
            d->net_w = ladder[li];
            d->net_h = ladder[li];
            if (mn_depth_probe_resolution(d)) break;
            fprintf(stderr, "[depth] model rejected %dx%d; stepping down\n",
                    d->net_w, d->net_h);
            fflush(stderr);
        }
        if (li == 3) {
            d->net_w = MN_DEPTH_NET_FALLBACK;
            d->net_h = MN_DEPTH_NET_FALLBACK;
        }
    }

    fprintf(stderr,
            "[depth] session ready (CPU, %d threads), input '%s' %dx%d%s, "
            "output '%s'\n",
            MN_DEPTH_INTRA_THREADS, d->input_name, d->net_w, d->net_h,
            d->dynamic ? " (dynamic)" : " (static)",
            d->output_name);
    fflush(stderr);
    return d;

fail:
    mn_depth_destroy(d);
    return NULL;
}

void mn_depth_destroy(mn_depth *d) {
    if (d == NULL) {
        return;
    }
    if (d->ort != NULL) {
        const OrtApi *ort = d->ort;
        if (d->input_name != NULL && d->allocator != NULL) {
            ort->AllocatorFree(d->allocator, d->input_name);
        }
        if (d->output_name != NULL && d->allocator != NULL) {
            ort->AllocatorFree(d->allocator, d->output_name);
        }
        if (d->mem_info != NULL) {
            ort->ReleaseMemoryInfo(d->mem_info);
        }
        if (d->session != NULL) {
            ort->ReleaseSession(d->session);
        }
        if (d->sopts != NULL) {
            ort->ReleaseSessionOptions(d->sopts);
        }
        if (d->env != NULL) {
            ort->ReleaseEnv(d->env);
        }
    }
    mn_depth_mutex_destroy(&d->run_mutex);
    free(d);
}

bool mn_depth_generate(mn_depth *d, const char *image_path,
                       const char *out_depth_png) {
    if (d == NULL || image_path == NULL || out_depth_png == NULL) {
        return false;
    }

    const OrtApi *ort = d->ort;
    const int nw = d->net_w, nh = d->net_h;
    const size_t npix = (size_t)nw * (size_t)nh;

    bool ok = false;
    unsigned char *rgb = NULL;      /* source, HWC RGB                       */
    unsigned char *net_rgb = NULL;  /* nw x nh, HWC RGB                      */
    float *chw = NULL;              /* 3 * npix, normalized model input      */
    float *dnet = NULL;             /* npix, normalized depth at net res     */
    float *dfull = NULL;            /* w*h, depth resized to source res      */
    unsigned char *gray = NULL;     /* w*h, 8-bit output                     */
    OrtValue *in_val = NULL, *out_val = NULL;

    /* 1) Decode source (force RGB). */
    int w = 0, h = 0, comp = 0;
    rgb = stbi_load(image_path, &w, &h, &comp, 3);
    if (rgb == NULL || w <= 0 || h <= 0) {
        fprintf(stderr, "[depth] decode failed: %s (%s)\n",
                image_path, stbi_failure_reason());
        fflush(stderr);
        goto done;
    }

    /* 2) Resize to network resolution. */
    net_rgb = (unsigned char *)malloc(npix * 3);
    chw     = (float *)malloc(npix * 3 * sizeof(float));
    dnet    = (float *)malloc(npix * sizeof(float));
    dfull   = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    gray    = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (net_rgb == NULL || chw == NULL || dnet == NULL ||
        dfull == NULL || gray == NULL) {
        goto done;
    }
    if (stbir_resize_uint8_linear(rgb, w, h, 0, net_rgb, nw, nh, 0,
                                  STBIR_RGB) == NULL) {
        fprintf(stderr, "[depth] input resize failed: %s\n", image_path);
        fflush(stderr);
        goto done;
    }

    /* 3) HWC uint8 -> CHW float32, ImageNet-normalized. */
    for (int c = 0; c < 3; ++c) {
        const float mean = MN_DEPTH_MEAN[c];
        const float inv_std = 1.0f / MN_DEPTH_STD[c];
        float *dst = chw + (size_t)c * npix;
        const unsigned char *src = net_rgb + c;
        for (size_t i = 0; i < npix; ++i) {
            dst[i] = ((float)src[i * 3] * (1.0f / 255.0f) - mean) * inv_std;
        }
    }

    /* 4) Inference (serialized: one session shared by concurrent callers). */
    {
        int64_t in_shape[4] = { 1, 3, nh, nw };
        const char *in_names[1]  = { d->input_name };
        const char *out_names[1] = { d->output_name };

        mn_depth_mutex_lock(&d->run_mutex);
        int run_bad =
            mn_depth_ort_failed(ort, ort->CreateTensorWithDataAsOrtValue(
                    d->mem_info, chw, npix * 3 * sizeof(float),
                    in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val),
                    "CreateTensor(input)") ||
            in_val == NULL ||
            mn_depth_ort_failed(ort, ort->Run(d->session, NULL,
                    in_names, (const OrtValue *const *)&in_val, 1,
                    out_names, 1, &out_val), "Run") ||
            out_val == NULL;
        mn_depth_mutex_unlock(&d->run_mutex);
        if (run_bad) {
            goto done;
        }
    }

    /* 5) Fetch the output ([1,H,W] or [1,1,H,W] -- accept any layout whose
     * element count is exactly npix), min-max normalize to 0..1 (1 = near:
     * the model emits relative INVERSE depth, larger = nearer). */
    {
        OrtTensorTypeAndShapeInfo *oinfo = NULL;
        size_t count = 0;
        float *odata = NULL;
        if (mn_depth_ort_failed(ort, ort->GetTensorTypeAndShape(out_val,
                                                                &oinfo),
                                "GetTensorTypeAndShape")) {
            goto done;
        }
        int bad = mn_depth_ort_failed(ort,
                ort->GetTensorShapeElementCount(oinfo, &count),
                "GetTensorShapeElementCount");
        ort->ReleaseTensorTypeAndShapeInfo(oinfo);
        if (bad ||
            mn_depth_ort_failed(ort, ort->GetTensorMutableData(out_val,
                                                               (void **)&odata),
                                "GetTensorMutableData") ||
            odata == NULL) {
            goto done;
        }
        if (count != npix) {
            fprintf(stderr,
                    "[depth] unexpected output element count %zu "
                    "(expected %zu)\n", count, npix);
            fflush(stderr);
            goto done;
        }

        float lo = odata[0], hi = odata[0];
        for (size_t i = 1; i < npix; ++i) {
            float v = odata[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        float range = hi - lo;
        if (range <= 0.0f) {
            /* Degenerate (flat) output: emit mid-gray rather than fail. */
            for (size_t i = 0; i < npix; ++i) {
                dnet[i] = 0.5f;
            }
        } else {
            float inv = 1.0f / range;
            for (size_t i = 0; i < npix; ++i) {
                dnet[i] = (odata[i] - lo) * inv;
            }
        }
    }

    /* 6) Resize depth back to the source dimensions and write the PNG. */
    if (stbir_resize_float_linear(dnet, nw, nh, 0, dfull, w, h, 0,
                                  STBIR_1CHANNEL) == NULL) {
        fprintf(stderr, "[depth] output resize failed: %s\n", out_depth_png);
        fflush(stderr);
        goto done;
    }
    {
        const size_t total = (size_t)w * (size_t)h;
        for (size_t i = 0; i < total; ++i) {
            float v = dfull[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            gray[i] = (unsigned char)(v * 255.0f + 0.5f);
        }
    }
    if (!stbi_write_png(out_depth_png, w, h, 1, gray, w)) {
        fprintf(stderr, "[depth] PNG write failed: %s\n", out_depth_png);
        fflush(stderr);
        goto done;
    }
    ok = true;

done:
    if (out_val != NULL) {
        ort->ReleaseValue(out_val);
    }
    if (in_val != NULL) {
        ort->ReleaseValue(in_val);
    }
    free(gray);
    free(dfull);
    free(dnet);
    free(chw);
    free(net_rgb);
    stbi_image_free(rgb);
    return ok;
}
