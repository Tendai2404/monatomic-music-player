/* depth.h -- Monatomic Audio Player
 *
 * Cover-art depth-map generation via Depth-Anything-V2-Small (ONNX Runtime,
 * CPU execution provider).
 *
 * Given a cover-art image on disk (PNG/JPEG), produces a grayscale depth-map
 * PNG beside it where white = near, black = far, min-max normalized to the
 * full 0..255 range. The UI's WebGL mesh (ui/depthart.js) consumes
 * "<hash>.depth.png" files produced by this module.
 *
 * Design notes
 * ------------
 *   - One resident ONNX session per mn_depth handle, created once (model load
 *     is the expensive part). The model is small (~24 M params, 27 MB
 *     uint8-quantized); a single CPU inference takes ~1-2 s per cover.
 *   - CPU ONLY, deliberately: the CUDA device (and its VRAM arena) belongs to
 *     the stems engine (stems.c). Depth generation is a background nicety and
 *     must never compete for GPU memory. Intra-op threads are capped low so
 *     scanning stays polite.
 *   - Thread-safety: mn_depth_generate() may be called from concurrent
 *     scanner/worker threads on the SAME handle; inference is serialized
 *     internally with a mutex. create/destroy are NOT safe to race against
 *     in-flight generate calls -- destroy only after workers are quiesced.
 *   - No global state; multiple handles are independent (but each holds its
 *     own copy of the session, so one shared handle is the intended use).
 */

#ifndef MN_DEPTH_H
#define MN_DEPTH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mn_depth mn_depth;

/* Load the Depth-Anything-V2 ONNX model at model_path and build a resident
 * CPU inference session. Returns NULL if the model file is missing or the
 * session cannot be created -- callers should treat NULL as "depth maps
 * disabled" and carry on. */
mn_depth *mn_depth_create(const char *model_path);

/* Generate a depth map for the image at image_path (PNG/JPEG/BMP/GIF -- any
 * format artcache's stb build decodes) and write an 8-bit grayscale PNG to
 * out_depth_png with the SAME pixel dimensions as the source image.
 * White (255) = nearest, black (0) = farthest, min-max normalized.
 *
 * Synchronous; ~1-2 s of CPU per call. Safe to call from multiple threads on
 * one handle (runs are serialized internally). Returns true on success. */
bool mn_depth_generate(mn_depth *d, const char *image_path,
                       const char *out_depth_png);

/* Release the session and the handle. Must not race in-flight generate()
 * calls. NULL is a no-op. */
void mn_depth_destroy(mn_depth *d);

#ifdef __cplusplus
}
#endif

#endif /* MN_DEPTH_H */
