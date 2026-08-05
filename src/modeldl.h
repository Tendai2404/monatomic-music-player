/*
 * modeldl.h — Monatomic Music Player
 *
 * Hugging Face model downloader. Fetches a single file from a HF repo to a
 * destination directory over HTTPS (WinHTTP), following the resolve/main ->
 * CDN 302 redirect, streaming to a ".part" temp file that is atomically
 * renamed into place on completion.
 *
 * One global download slot with a dedicated worker thread. A second start
 * while one is in flight is rejected ("busy"). Progress is reported through a
 * caller-supplied callback invoked ON THE WORKER THREAD.
 */
#ifndef MN_MODELDL_H
#define MN_MODELDL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Progress callback. Invoked periodically (~every 256 KiB / final flush) on
 * the worker thread, and exactly once more at the very end with done=true
 * (success: err==NULL) or with a non-NULL err string (failure).
 *
 *   user       — opaque pointer passed to mn_modeldl_start.
 *   bytes_done — bytes written so far.
 *   bytes_total— Content-Length, or 0 if the server did not report one.
 *   done       — true on the terminal callback (success or failure).
 *   err        — NULL on success; a short human-readable string on failure.
 */
typedef void (*mn_modeldl_cb)(void *user,
                              int64_t bytes_done,
                              int64_t bytes_total,
                              bool done,
                              const char *err);

/*
 * Start a download of https://huggingface.co/<repo>/resolve/main/<file>
 * into <dest_dir>/<save_as> (or <dest_dir>/<basename(file)> when save_as is
 * NULL/empty). Spawns a worker thread and returns immediately.
 *
 * Returns true if the download was accepted (worker started), false if a
 * download is already in flight (busy) or on an argument/resource error — in
 * which case cb is NOT invoked.
 *
 * `file` may contain forward-slash subfolders (e.g. "onnx/model.onnx") — the
 * full path is used in the URL. `save_as` disambiguates repos that all store
 * their model at the same subpath (onnx-community repos use "onnx/model.onnx"
 * for every model, which would otherwise collide locally). save_as must be a
 * bare filename: separators and ".." are rejected for security.
 */
bool mn_modeldl_start(const char *repo, const char *file,
                      const char *save_as,
                      const char *dest_dir,
                      mn_modeldl_cb cb, void *user);

/* True while a download worker is active. */
bool mn_modeldl_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* MN_MODELDL_H */
