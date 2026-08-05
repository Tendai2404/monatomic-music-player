/*
 * tags_write.h — Monatomic Audio Player
 *
 * Metadata WRITER: tag fields, embedded cover art and embedded lyrics for
 * the three formats the editor UI supports:
 *
 *   - MP3  : ID3v2.3 tag rewrite (UTF-16 w/ BOM text frames — broadest
 *            reader compatibility). The whole leading tag is rebuilt from
 *            (existing frames we don't manage, preserved verbatim) +
 *            (replaced managed frames), then a NEW temp file (new tag +
 *            audio bytes copied verbatim) atomically replaces the original.
 *            A trailing ID3v1 tag is left untouched. Unparseable / v2.2 /
 *            unsynchronized source tags fall back to writing only our
 *            frames + keeping the audio.
 *   - FLAC : metadata-block-chain rewrite. STREAMINFO and unknown blocks
 *            are kept verbatim; VORBIS_COMMENT is merged (managed keys
 *            replaced, everything else preserved); PICTURE type 3 (front
 *            cover) is replaced for art writes. Audio frames are copied
 *            verbatim into a temp file + atomic replace.
 *   - M4A  : SAFE subset only. The moov>udta>meta>ilst payload is patched
 *            in place when the new ilst (+ trailing 'free' filler) fits in
 *            the byte span of the existing ilst + adjacent free space.
 *            No stco/co64 chunk-offset fixups are ever attempted; when the
 *            new metadata does not fit the call fails with the error
 *            string "m4a-needs-repack".
 *
 * Lyrics are written to USLT (MP3), LYRICS= (FLAC vorbis comment) or the
 * iTunes ©lyr atom (M4A). Reading checks the embedded field first, then
 * the "<audio path minus ext>.lrc" / ".txt" sidecar.
 *
 * All functions are thread-safe with respect to distinct paths; they keep
 * no shared mutable state. The caller must guarantee the target file is
 * not open elsewhere in this process with delete-sharing denied (i.e. the
 * playback engine must unload it first — see mn_engine_unload).
 */

#ifndef MN_TAGS_WRITE_H
#define MN_TAGS_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * mn_tag_edit — the full editable field set, mirroring the UI's tagwrite
 * command. Semantics: every field in this struct is AUTHORITATIVE — an
 * empty string (or 0 for the numeric fields) REMOVES the corresponding
 * frame/comment/atom from the file. The one exception is `composer`
 * (which the shipped editor does not surface): when empty, any existing
 * composer tag in the file is preserved.
 *
 * PARTIAL edits: set `keep_missing` and empty/0 fields PRESERVE the file's
 * existing values instead (the composer semantics applied to every field).
 * The album batch editor uses this — it sends only album-level fields, and
 * without the flag it stripped Title / Track # / Comment from every file.
 * ------------------------------------------------------------------------ */
typedef struct mn_tag_edit {
    char title[256];
    char artist[256];
    char album[256];
    char album_artist[256];
    char genre[128];
    char composer[256];   /* empty = PRESERVE existing (not managed)        */
    char comment[1024];
    int  year;            /* 0 = remove                                      */
    int  track_no;        /* 0 = remove                                      */
    bool keep_missing;    /* empty fields preserve instead of remove         */
} mn_tag_edit;

/* Longest error string written by the functions below (incl. NUL). */
#define MN_TAGW_ERR_CAP 128

/* --------------------------------------------------------------------------
 * Writers. Each returns true on success. On failure a short machine-usable
 * error token is written to `err` (may be NULL): "unsupported-format",
 * "io-error", "replace-failed", "m4a-needs-repack", "bad-args", "corrupt".
 * ------------------------------------------------------------------------ */

/* Write the textual tag fields of `edit` into the file at `path`. */
bool mn_tagw_write_tags(const char *path, const mn_tag_edit *edit,
                        char *err, size_t errn);

/* Embed `img[0..len)` as the front-cover (type 3) picture. `mime` must be
 * "image/jpeg" or "image/png" (anything else is stored as given for FLAC
 * and sniffed for M4A). Replaces any existing front cover. */
bool mn_tagw_write_art(const char *path, const uint8_t *img, size_t len,
                       const char *mime, char *err, size_t errn);

/* Embed unsynchronized lyrics `text` (UTF-8; empty removes the lyrics). */
bool mn_tagw_write_lyrics(const char *path, const char *text,
                          char *err, size_t errn);

/* Write "<path minus extension>.lrc" as UTF-8. Empty text deletes it. */
bool mn_tagw_write_sidecar_lrc(const char *audio_path, const char *lrc_text);
/* Plain-text ".txt" sidecar (fallback when embedding fails on formats the
 * writer can't touch — OGG/Opus/WAV — so plain lyrics still persist and
 * don't refetch every session). Empty text deletes the sidecar. */
bool mn_tagw_write_sidecar_txt(const char *audio_path, const char *text);

/* --------------------------------------------------------------------------
 * Lyrics reader: embedded (USLT / LYRICS= / ©lyr) first, then the .lrc and
 * .txt sidecars. `out` receives NUL-terminated UTF-8 ("" when none found).
 * Returns true when non-empty lyrics were found.
 * ------------------------------------------------------------------------ */
bool mn_tagw_read_lyrics(const char *path, char *out, size_t n);

/* --------------------------------------------------------------------------
 * Base64 decode helper (standard alphabet, whitespace/padding tolerant).
 * Accepts an optional "data:<mime>;base64," URI prefix and skips it.
 * Returns a malloc'd buffer (release with mn_tagw_b64_free) and sets
 * *out_len; NULL on OOM/empty input.
 * ------------------------------------------------------------------------ */
uint8_t *mn_tagw_b64_decode(const char *src, size_t *out_len);
void     mn_tagw_b64_free(uint8_t *p);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_TAGS_WRITE_H */
