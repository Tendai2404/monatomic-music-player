/* artcache.h -- Album-art thumbnail cache for Monatomic.
 *
 * Extracts embedded cover art (or sidecar image) from an audio file, decodes
 * and resizes it to a fixed 256x256 RGBA thumbnail, and caches the result on
 * disk keyed by album. Thumbnails are deduplicated per album: every track that
 * shares an album key resolves to a single cached thumbnail file, so a library
 * of 1M tracks across ~50k albums stores ~50k thumbnails, not 1M.
 *
 * Decoding/resizing is backed by stb_image.h + stb_image_resize2.h and cover
 * extraction by tags.h (see mn_tags_*). This header is pure declarations; all
 * heavy dependencies live in artcache.c.
 *
 * Thumbnail format on disk: PNG, 256x256, 8-bit RGBA (written via
 * stb_image_write.h). The on-disk filename is derived deterministically from
 * album_key so that a check-only lookup never needs to touch the audio file.
 *
 * Threading: mn_art_ensure() is safe to call concurrently from multiple
 * background worker threads *provided* concurrent calls use distinct album_key
 * values. Concurrent calls that target the same album_key race on the same
 * cache file; the cache tolerates this (last writer wins, readers see either
 * the old or a complete new file) but callers that care should serialize per
 * album. All functions are otherwise reentrant and hold no global state.
 *
 * Ownership: pixel buffers returned by mn_art_load_rgba() must be released with
 * mn_art_free(). Do not free them with the C library free() directly, as the
 * allocator is an implementation detail of the backing image library.
 */

#ifndef MN_ARTCACHE_H
#define MN_ARTCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DEFAULT edge length (pixels) of cached thumbnails. Both width and height.
 * RGBA, 8 bits per channel. The effective size is runtime-configurable via
 * mn_art_set_thumb_size(); this constant is the default and the value used
 * for legacy (unsuffixed) cache filenames. */
#define MN_ART_THUMB_SIZE 256

/* Runtime bounds for mn_art_set_thumb_size(). */
#define MN_ART_THUMB_MIN 128
#define MN_ART_THUMB_MAX 512

/* Bytes per pixel in the RGBA thumbnails this module produces and consumes. */
#define MN_ART_BPP 4

/* Recommended minimum size for the out_path buffer passed to mn_art_ensure().
 * A generous bound covering cache_dir plus the derived thumbnail filename. */
#define MN_ART_PATH_MAX 1024

/* Ensure a square RGBA thumbnail (edge = mn_art_get_thumb_size() px, default
 * MN_ART_THUMB_SIZE) exists on disk for the given album, and return its path.
 *
 * The cached thumbnail file lives under cache_dir and its name is derived
 * deterministically from album_key (a stable per-album identifier chosen by the
 * caller, e.g. a hash of "albumartist\0album"). Because the name depends only
 * on album_key, all tracks of one album map to one thumbnail file (per-album
 * dedup).
 *
 * Behavior:
 *   - If a valid cached thumbnail for album_key already exists, its path is
 *     written to out_path and the function returns true without touching
 *     audio_path.
 *   - Otherwise, if audio_path is non-NULL, cover art is extracted from that
 *     audio file (embedded picture via tags.h, or a conventional sidecar image
 *     such as cover.jpg/folder.jpg next to it), decoded, resized to
 *     MN_ART_THUMB_SIZE square, written to the cache, and its path returned via
 *     out_path (returns true).
 *   - If audio_path is NULL, the call is check-only: it returns true and fills
 *     out_path iff a cached thumbnail already exists, and returns false
 *     otherwise (no extraction attempted, out_path left unspecified).
 *
 * Parameters:
 *   cache_dir  Directory in which thumbnails are stored. Created on demand if
 *              it does not exist. Must be non-NULL.
 *   album_key  Stable, non-empty album identifier. Must be non-NULL. Used only
 *              to derive the cache filename; never dereferenced as a path.
 *   audio_path Source audio file to extract cover art from, or NULL for a
 *              check-only lookup (see above).
 *   out_path   Caller-owned buffer receiving the NUL-terminated thumbnail path
 *              on success. Must be non-NULL with capacity >= n. On failure its
 *              contents are unspecified. MN_ART_PATH_MAX is a safe size.
 *   n          Capacity of out_path in bytes, including the NUL terminator.
 *
 * Returns true if out_path now names an existing, valid cached thumbnail;
 * false on any failure (no/unreadable cover art, decode/resize/write error,
 * check-only miss, or invalid arguments). This function never blocks on user
 * input and performs no network I/O.
 */
bool mn_art_ensure(const char *cache_dir,
                   const char *album_key,
                   const char *audio_path,
                   char *out_path,
                   size_t n);

/* Long-edge cap (pixels) for the high-resolution cover variant produced by
 * mn_art_ensure_hires(). Aspect-preserved (NOT square-cropped) so the depth
 * model and the volumetric mesh see the real cover geometry. */
#define MN_ART_HIRES_MAX 1024

/* Ensure a HIGH-RESOLUTION cover PNG exists on disk for the given album and
 * return its path. Unlike mn_art_ensure (which produces a small square thumb
 * for the grid), this extracts the FULL-resolution embedded/sidecar cover,
 * scales its long edge down to at most MN_ART_HIRES_MAX (never up), PRESERVES
 * aspect ratio (no crop), and writes an RGB PNG named "<hash>.hires.png" under
 * cache_dir. Feeds the depth-map generator and the now-playing volumetric mesh
 * so 3D art is crisp instead of a blurry 256 upscale.
 *
 * Behavior mirrors mn_art_ensure:
 *   - Cached hi-res present  -> return its path (true), audio_path untouched.
 *   - audio_path non-NULL    -> extract/decode/scale/write, then return path.
 *   - audio_path == NULL     -> check-only: true iff a cached hi-res exists.
 * Returns false on any failure. Reentrant; safe to call concurrently across
 * distinct album_key values (same-album calls race last-writer-wins). */
bool mn_art_ensure_hires(const char *cache_dir,
                         const char *album_key,
                         const char *audio_path,
                         char *out_path,
                         size_t n);

/* Session hires-write attribution (the 2026-07 incident: the entire hires
 * tier — 1747 files, 1.49 GB — was rewritten in one warm session with no log
 * line naming the emitter). Every successful "<hash>.hires.png" WRITE by
 * mn_art_ensure_hires / mn_art_ingest_image bumps these process-lifetime
 * totals; the host prints a summary when they move (heal tick) so any
 * recurrence of a mass regeneration is attributed, never silent. Thread-safe. */
void mn_art_hires_stats(long long *out_files, long long *out_bytes);

/* Replace an album's cached art from an image FILE (e.g. a cover downloaded
 * from an online source). Decodes image_path (stb, with the platform fallback
 * for webp/heic/avif), writes BOTH the square grid thumb (current thumb size)
 * and the aspect-preserving "<hash>.hires.png" companion, atomically. On
 * success copies the thumb path into out_path and returns true. Was previously
 * only implicitly declared at its call site (C4013). */
bool mn_art_ingest_image(const char *cache_dir,
                         const char *album_key,
                         const char *image_path,
                         char *out_path,
                         size_t n);

/* Cheap presence check: does cover art exist for this audio file, either as an
 * embedded picture OR as a conventional sidecar image (cover.jpg / folder.jpg /
 * front.jpg / album.* ) in the same directory? Does NOT decode or resize — it
 * only detects availability, so it is cheap enough to call once per track at
 * scan time to populate the tracks.has_art column. Returns false on any error
 * or when no art is present. audio_path must be non-NULL. */
bool mn_art_probe(const char *audio_path);

/* Set the edge length (pixels) used for NEWLY generated thumbnails, clamped to
 * [MN_ART_THUMB_MIN, MN_ART_THUMB_MAX]. The size participates in the cache
 * filename (cache-buster) for non-default sizes, so changing it makes
 * mn_art_ensure() regenerate thumbnails on demand instead of serving stale
 * ones; check-only lookups fall back to the default-size thumbnail when the
 * sized one does not exist yet (so the UI never goes blank after a size
 * change). Thread-safe in practice: a single aligned int store, read by the
 * scanner workers; changes apply to subsequent calls. */
void mn_art_set_thumb_size(int px);

/* Current effective thumbnail edge length in pixels. */
int mn_art_get_thumb_size(void);

/* Load a cached thumbnail from disk into a freshly allocated RGBA pixel buffer.
 *
 * The file at thumb_path is decoded to 8-bit RGBA. On success the pixel buffer
 * (w * h * MN_ART_BPP bytes, top-left origin, row-major, no padding) is
 * returned and *out_w / *out_h receive its dimensions in pixels. For thumbnails
 * produced by mn_art_ensure() these are MN_ART_THUMB_SIZE, but callers should
 * honor the reported dimensions rather than assume.
 *
 * Parameters:
 *   thumb_path Path to a thumbnail image file. Must be non-NULL.
 *   out_w      Receives the decoded width in pixels. Must be non-NULL.
 *   out_h      Receives the decoded height in pixels. Must be non-NULL.
 *
 * Returns a pointer to the pixel buffer on success, which the caller must
 * release with mn_art_free(). Returns NULL on failure (missing/unreadable file,
 * decode error, or invalid arguments); *out_w and *out_h are left unspecified
 * in that case.
 */
uint8_t *mn_art_load_rgba(const char *thumb_path, int *out_w, int *out_h);

/* Release a pixel buffer returned by mn_art_load_rgba().
 *
 * Passing NULL is a no-op. Passing any pointer not obtained from
 * mn_art_load_rgba() is undefined behavior. */
void mn_art_free(uint8_t *px);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_ARTCACHE_H */
