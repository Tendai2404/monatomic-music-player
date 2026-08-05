/* ==========================================================================
 * stempack.h — build a self-describing ".mnstem" stem container.
 *
 * A .mnstem is a ZIP archive so any tool can open it, but the extension marks
 * it as a Monatomic stem pack that can be re-imported. Contents:
 *   manifest.json  — source track tags + separation model + per-stem list
 *   <NN Name>.<ext> — one encoded audio file per exported stem
 *   cover.jpg      — album art (optional)
 * ========================================================================== */
#ifndef MN_STEMPACK_H
#define MN_STEMPACK_H

#include <stdint.h>
#include <stdbool.h>

/* One member file to bundle: its name inside the archive + a source path on
 * disk to read from (the already-encoded stem temp files). */
typedef struct mn_stempack_file {
    const char *arcname;    /* e.g. "01 Sub Bass.flac" */
    const char *srcpath;    /* absolute path to the encoded file on disk */
} mn_stempack_file;

/* Write `out_path` (.mnstem ZIP) containing manifest_json (as "manifest.json"),
 * each file in `files`, and optionally cover_jpg_path as "cover.jpg" (may be
 * NULL). Audio members are STORED (already compressed); the manifest is
 * DEFLATE-compressed. Returns true on success. */
bool mn_stempack_write(const char *out_path,
                       const char *manifest_json,
                       const mn_stempack_file *files, int file_count,
                       const char *cover_jpg_path);

#endif /* MN_STEMPACK_H */
