/* ==========================================================================
 * stempack.c — .mnstem ZIP container writer. See header.
 * ========================================================================== */
#include "stempack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* miniz single-file ZIP (compiled in vendor/miniz.c). */
#include "../vendor/miniz.h"

/* Read a whole file into memory. Caller frees. Returns NULL on failure. */
static void *read_all(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *out_len = rd;
    return buf;
}

bool mn_stempack_write(const char *out_path,
                       const char *manifest_json,
                       const mn_stempack_file *files, int file_count,
                       const char *cover_jpg_path) {
    if (!out_path || !manifest_json) return false;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    /* overwrite any existing pack */
    remove(out_path);
    if (!mz_zip_writer_init_file(&zip, out_path, 0)) return false;

    bool ok = true;

    /* manifest.json — DEFLATE-compressed */
    if (!mz_zip_writer_add_mem(&zip, "manifest.json",
                               manifest_json, strlen(manifest_json),
                               MZ_BEST_COMPRESSION)) {
        ok = false;
    }

    /* audio members — STORED (they're already compressed); read+add each */
    for (int i = 0; ok && i < file_count; i++) {
        size_t len = 0;
        void *data = read_all(files[i].srcpath, &len);
        if (!data) { ok = false; break; }
        if (!mz_zip_writer_add_mem(&zip, files[i].arcname, data, len,
                                   MZ_NO_COMPRESSION)) {
            ok = false;
        }
        free(data);
    }

    /* optional cover.jpg — STORED */
    if (ok && cover_jpg_path && cover_jpg_path[0]) {
        size_t len = 0;
        void *data = read_all(cover_jpg_path, &len);
        if (data) {
            (void)mz_zip_writer_add_mem(&zip, "cover.jpg", data, len,
                                        MZ_NO_COMPRESSION);   /* best-effort */
            free(data);
        }
    }

    if (ok) ok = mz_zip_writer_finalize_archive(&zip) ? true : false;
    mz_zip_writer_end(&zip);
    if (!ok) remove(out_path);
    return ok;
}
