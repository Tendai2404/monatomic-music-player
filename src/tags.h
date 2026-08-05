/*
 * tags.h — Monatomic Audio Player
 *
 * Metadata and embedded-cover-art reader.
 *
 * Reads container/tag metadata (title, artist, album, etc.) and technical
 * audio properties (sample rate, channels, bit depth, bitrate, duration) from
 * a media file, and optionally extracts a single embedded cover-art image.
 *
 * Supported containers / tag formats:
 *   - MP3   (ID3v2 / ID3v1, APIC cover frames)
 *   - FLAC  (Vorbis comments, METADATA_BLOCK_PICTURE)
 *   - MP4   (.m4a / .m4b / .mp4, iTunes-style 'ilst' atoms, 'covr')
 *   - OGG   (Vorbis comments, base64 METADATA_BLOCK_PICTURE)
 *   - OPUS  (OpusTags / Vorbis comments)
 *   - WAV   (RIFF INFO / optional embedded ID3 chunk)
 *
 * Design notes:
 *   - The mn_tags struct uses fixed-size inline char buffers only. It performs
 *     NO heap allocation and is safe to place on the stack or embed in another
 *     struct. Text fields are always NUL-terminated UTF-8; strings longer than
 *     their buffer are truncated on a UTF-8 boundary.
 *   - Cover-art extraction DOES allocate; the returned buffer must be released
 *     with mn_tags_free_cover().
 *   - All functions are thread-safe with respect to distinct arguments; they
 *     keep no shared mutable state.
 */

#ifndef MN_TAGS_H
#define MN_TAGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Field capacities (including the terminating NUL byte).
 * Chosen generously; oversized source values are truncated to fit.
 * ------------------------------------------------------------------------ */
#define MN_TAGS_TITLE_CAP        256
#define MN_TAGS_ARTIST_CAP       256
#define MN_TAGS_ALBUM_CAP        256
#define MN_TAGS_ALBUM_ARTIST_CAP 256
#define MN_TAGS_GENRE_CAP        128
#define MN_TAGS_COMPOSER_CAP     256
#define MN_TAGS_MIME_CAP          32   /* e.g. "image/jpeg", "image/png" */

/* --------------------------------------------------------------------------
 * mn_tags — decoded metadata + technical properties for one media file.
 *
 * All text fields are NUL-terminated UTF-8. An absent/empty tag yields an
 * empty string (buffer[0] == '\0'). Numeric fields are 0 when unknown.
 * ------------------------------------------------------------------------ */
typedef struct mn_tags {
    /* Textual metadata (UTF-8, NUL-terminated). */
    char title[MN_TAGS_TITLE_CAP];
    char artist[MN_TAGS_ARTIST_CAP];
    char album[MN_TAGS_ALBUM_CAP];
    char album_artist[MN_TAGS_ALBUM_ARTIST_CAP];
    char genre[MN_TAGS_GENRE_CAP];
    char composer[MN_TAGS_COMPOSER_CAP];

    /* Release year (0 if unknown), e.g. 1997. */
    uint16_t year;

    /* Track/disc position within a set (0 if unknown). */
    uint16_t track_no;      /* this track's number */
    uint16_t track_total;   /* total tracks in the set (0 if unknown) */
    uint16_t disc_no;       /* this disc's number */
    uint16_t disc_total;    /* total discs in the set (0 if unknown) */

    /* Technical audio properties (0 if unknown / not applicable). */
    uint32_t sample_rate;   /* Hz, e.g. 44100 */
    uint16_t channels;      /* channel count, e.g. 2 */
    uint16_t bit_depth;     /* bits per sample for PCM formats; 0 for lossy */
    uint32_t bitrate_kbps;  /* average bitrate in kilobits per second */
    uint64_t duration_ms;   /* total playback duration in milliseconds */

    /* Real codec label from the container's magic bytes — NOT the file
     * extension (an .m4a is "ALAC" or "AAC"; an .ogg may be "OPUS").
     * Empty when the container is unrecognized; callers fall back to the
     * extension. */
    char codec[12];
} mn_tags;

/* --------------------------------------------------------------------------
 * mn_tags_read — read metadata and technical properties for a media file.
 *
 * The container format is detected from file contents (and extension as a
 * hint). On success, every field of *out is initialized: recognized tags are
 * filled, unknown text fields are set to "" and unknown numbers to 0.
 *
 *   path : UTF-8 path to the media file (on Windows, converted internally to
 *          the appropriate wide path for opening).
 *   out  : caller-provided struct to populate. Fully overwritten on success;
 *          left unspecified on failure. Must not be NULL.
 *
 * Returns true if the file was opened and parsed as a supported format
 * (even if it carried no tags), false on I/O error or unsupported/corrupt
 * container.
 * ------------------------------------------------------------------------ */
bool mn_tags_read(const char *path, mn_tags *out);

/* --------------------------------------------------------------------------
 * mn_tags_read_cover — extract the first embedded cover-art image.
 *
 * Allocates a buffer holding the raw, undecoded image bytes exactly as stored
 * in the file (typically JPEG or PNG). The caller owns the buffer and MUST
 * release it with mn_tags_free_cover().
 *
 *   path  : UTF-8 path to the media file.
 *   bytes : receives a pointer to the newly allocated image data. Set to NULL
 *           on failure. Must not be NULL.
 *   len   : receives the length of *bytes in bytes. Set to 0 on failure.
 *           Must not be NULL.
 *   mime  : caller-provided buffer of at least MN_TAGS_MIME_CAP bytes; receives
 *           the NUL-terminated MIME type of the image (e.g. "image/jpeg").
 *           Empty string if the format did not declare one. May be NULL if the
 *           caller does not need the MIME type.
 *
 * Returns true and sets *bytes/*len if cover art was found and extracted,
 * false otherwise (no embedded art, unsupported format, I/O error, or
 * allocation failure). On false, *bytes is NULL and *len is 0.
 * ------------------------------------------------------------------------ */
bool mn_tags_read_cover(const char *path,
                        uint8_t **bytes,
                        size_t *len,
                        char *mime);

/* --------------------------------------------------------------------------
 * mn_tags_free_cover — release a buffer returned by mn_tags_read_cover().
 *
 *   bytes : pointer previously returned via mn_tags_read_cover()'s *bytes.
 *           Passing NULL is a safe no-op.
 * ------------------------------------------------------------------------ */
void mn_tags_free_cover(uint8_t *bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_TAGS_H */
