/*
 * tags.c — Monatomic Music Player
 *
 * Implementation of the metadata + embedded-cover-art reader declared in
 * tags.h. Dependency-light: relies only on the C standard library. Every
 * container format (MP3/ID3, FLAC, MP4, OGG, Opus, WAV) is parsed by hand so
 * the reader stays small, allocation-frugal, and cross-platform.
 *
 * Overview:
 *   - mn_tags_read()       fills an mn_tags struct (text + audio properties).
 *   - mn_tags_read_cover() extracts the first embedded image (heap-allocated).
 *   - mn_tags_free_cover() releases a cover buffer.
 *
 * The file is loaded through a small "cover sink" abstraction so the tag
 * parsers can either capture cover art (for mn_tags_read_cover) or ignore it
 * (for mn_tags_read) using one shared code path.
 */

#include "tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ==========================================================================
 * Small helpers
 * ======================================================================== */

/* Byte reads with explicit endianness, bounds assumed checked by caller. */
static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd_be64(const uint8_t *p) {
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

/*
 * Open a file for binary reading. On Windows the UTF-8 path is converted to a
 * wide path so non-ASCII filenames work. Returns NULL on failure.
 */
static FILE *mn_fopen_utf8(const char *path) {
    if (!path) return NULL;
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) <= 0) {
        free(wpath);
        return NULL;
    }
    FILE *f = _wfopen(wpath, L"rb");
    free(wpath);
    return f;
#else
    return fopen(path, "rb");
#endif
}

/* Read the whole file into a heap buffer. Caps size to avoid huge reads for
 * pathological files while still covering normal music files with big art.
 * Returns NULL on failure; on success *out_len holds the byte count. */
#define MN_MAX_FILE_BYTES ((size_t)512u * 1024u * 1024u) /* 512 MiB safety cap */

static uint8_t *mn_read_whole_file(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    FILE *f = mn_fopen_utf8(path);
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if ((size_t)sz > MN_MAX_FILE_BYTES) { fclose(f); return NULL; }

    size_t len = (size_t)sz;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = len ? fread(buf, 1, len, f) : 0;
    fclose(f);
    if (got != len) { free(buf); return NULL; }
    if (out_len) *out_len = len;
    return buf;
}

/*
 * Copy up to cap-1 bytes of UTF-8 into dst, truncating on a UTF-8 continuation
 * boundary so a multibyte sequence is never split. dst is always terminated.
 */
static void mn_str_set(char *dst, size_t cap, const char *src, size_t src_len) {
    if (!dst || cap == 0) return;
    if (!src || src_len == 0) { dst[0] = '\0'; return; }

    size_t n = src_len < (cap - 1) ? src_len : (cap - 1);
    /* Back off if we truncated in the middle of a UTF-8 multibyte sequence. */
    if (n < src_len) {
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Trim leading/trailing ASCII whitespace and control chars in place-ish by
 * returning adjusted start/len. */
static void mn_trim(const char **s, size_t *len) {
    const char *p = *s;
    size_t n = *len;
    while (n > 0 && (unsigned char)*p <= ' ') { p++; n--; }
    while (n > 0 && (unsigned char)p[n - 1] <= ' ') n--;
    *s = p;
    *len = n;
}

/* Parse a leading unsigned integer from text (stops at first non-digit).
 * Returns the value clamped to uint16_t range. */
static uint16_t mn_parse_u16(const char *s, size_t len) {
    unsigned long v = 0;
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10u + (unsigned long)(s[i] - '0');
        if (v > 65535u) { v = 65535u; break; }
        i++;
    }
    return (uint16_t)v;
}

/* Parse "n/m" style position strings into number and total. Either may be 0. */
static void mn_parse_pos(const char *s, size_t len, uint16_t *num, uint16_t *total) {
    if (num) *num = 0;
    if (total) *total = 0;
    if (!s || len == 0) return;
    size_t i = 0;
    while (i < len && (s[i] < '0' || s[i] > '9')) i++;
    if (i < len && num) *num = mn_parse_u16(s + i, len - i);
    /* find slash */
    size_t j = 0;
    while (j < len && s[j] != '/') j++;
    if (j < len) {
        j++;
        while (j < len && (s[j] < '0' || s[j] > '9')) j++;
        if (j < len && total) *total = mn_parse_u16(s + j, len - j);
    }
}

/* Parse a year out of a date string like "1997", "1997-08-21", "1997/08". */
static uint16_t mn_parse_year(const char *s, size_t len) {
    /* find first 4 consecutive digits */
    for (size_t i = 0; i + 4 <= len; i++) {
        if (s[i] >= '0' && s[i] <= '9' && s[i+1] >= '0' && s[i+1] <= '9' &&
            s[i+2] >= '0' && s[i+2] <= '9' && s[i+3] >= '0' && s[i+3] <= '9') {
            return mn_parse_u16(s + i, 4);
        }
    }
    /* fallback: leading number */
    return mn_parse_u16(s, len);
}

/* ==========================================================================
 * Cover sink — lets tag parsers optionally capture the first cover image.
 * ======================================================================== */

typedef struct mn_cover_sink {
    bool     want;      /* true when the caller wants cover art */
    bool     have;      /* true once we captured one */
    int      ptype;     /* APIC picture-type of the captured image (-1 if
                         * unknown); 3 = front cover. Lets a later FRONT cover
                         * upgrade an earlier non-front pick. */
    uint8_t *bytes;     /* heap-allocated copy of image data */
    size_t   len;
    char     mime[MN_TAGS_MIME_CAP];
} mn_cover_sink;

/* Record a cover if wanted. `ptype` is the APIC picture-type (3 = front
 * cover, -1 = unknown/non-ID3 source). src is copied.
 *
 * PREFERENCE: the FRONT cover (type 3) wins. If we already captured a non-front
 * image and a front cover arrives, we UPGRADE to it. Once we hold a front cover
 * (or an unknown-type image from a non-ID3 source that we treat as final), we
 * keep it. This fixes files whose first embedded picture is a back cover /
 * artist photo / icon being cached as the album art. */
static void mn_cover_take(mn_cover_sink *sink, const uint8_t *src, size_t len,
                          const char *mime, size_t mime_len, int ptype) {
    if (!sink || !sink->want) return;
    if (!src || len == 0) return;
    if (sink->have) {
        /* already have one — only a FRONT cover may replace a non-front pick */
        if (sink->ptype == 3) return;            /* front already held */
        if (ptype != 3) return;                  /* incoming isn't front */
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return;
    memcpy(copy, src, len);
    free(sink->bytes);                           /* release any prior pick */
    sink->bytes = copy;
    sink->len = len;
    sink->have = true;
    sink->ptype = ptype;
    if (mime && mime_len) {
        mn_str_set(sink->mime, sizeof(sink->mime), mime, mime_len);
    } else {
        sink->mime[0] = '\0';
    }
}

/* ==========================================================================
 * Field dispatch — map a normalized tag key to an mn_tags field.
 * ======================================================================== */

/* Case-insensitive comparison of a bounded key against a NUL-terminated name. */
static bool key_eq(const char *key, size_t key_len, const char *name) {
    size_t n = strlen(name);
    if (key_len != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)key[i]) != tolower((unsigned char)name[i]))
            return false;
    }
    return true;
}

/*
 * Assign a Vorbis-comment / iTunes-style textual key+value to the tags struct.
 * Handles the common field name aliases across formats.
 */
static void mn_apply_field(mn_tags *t, const char *key, size_t key_len,
                           const char *val, size_t val_len) {
    if (!t || !key || key_len == 0) return;
    mn_trim(&val, &val_len);

    if (key_eq(key, key_len, "TITLE")) {
        mn_str_set(t->title, sizeof(t->title), val, val_len);
    } else if (key_eq(key, key_len, "ARTIST")) {
        mn_str_set(t->artist, sizeof(t->artist), val, val_len);
    } else if (key_eq(key, key_len, "ALBUM")) {
        mn_str_set(t->album, sizeof(t->album), val, val_len);
    } else if (key_eq(key, key_len, "ALBUMARTIST") ||
               key_eq(key, key_len, "ALBUM ARTIST") ||
               key_eq(key, key_len, "ALBUM_ARTIST")) {
        mn_str_set(t->album_artist, sizeof(t->album_artist), val, val_len);
    } else if (key_eq(key, key_len, "GENRE")) {
        mn_str_set(t->genre, sizeof(t->genre), val, val_len);
    } else if (key_eq(key, key_len, "COMPOSER")) {
        mn_str_set(t->composer, sizeof(t->composer), val, val_len);
    } else if (key_eq(key, key_len, "DATE") ||
               key_eq(key, key_len, "YEAR") ||
               key_eq(key, key_len, "ORIGINALDATE")) {
        if (t->year == 0) t->year = mn_parse_year(val, val_len);
    } else if (key_eq(key, key_len, "TRACKNUMBER") ||
               key_eq(key, key_len, "TRACK")) {
        uint16_t n = 0, tot = 0;
        mn_parse_pos(val, val_len, &n, &tot);
        if (n) t->track_no = n;
        if (tot) t->track_total = tot;
    } else if (key_eq(key, key_len, "TRACKTOTAL") ||
               key_eq(key, key_len, "TOTALTRACKS")) {
        t->track_total = mn_parse_u16(val, val_len);
    } else if (key_eq(key, key_len, "DISCNUMBER") ||
               key_eq(key, key_len, "DISC")) {
        uint16_t n = 0, tot = 0;
        mn_parse_pos(val, val_len, &n, &tot);
        if (n) t->disc_no = n;
        if (tot) t->disc_total = tot;
    } else if (key_eq(key, key_len, "DISCTOTAL") ||
               key_eq(key, key_len, "TOTALDISCS")) {
        t->disc_total = mn_parse_u16(val, val_len);
    }
}

/* ==========================================================================
 * ID3v1 (MP3 trailing 128-byte tag)
 * ======================================================================== */

/* Standard ID3v1 genre table (indices 0..191). */
static const char *const id3v1_genres[] = {
    "Blues","Classic Rock","Country","Dance","Disco","Funk","Grunge","Hip-Hop",
    "Jazz","Metal","New Age","Oldies","Other","Pop","R&B","Rap","Reggae","Rock",
    "Techno","Industrial","Alternative","Ska","Death Metal","Pranks","Soundtrack",
    "Euro-Techno","Ambient","Trip-Hop","Vocal","Jazz+Funk","Fusion","Trance",
    "Classical","Instrumental","Acid","House","Game","Sound Clip","Gospel","Noise",
    "AlternRock","Bass","Soul","Punk","Space","Meditative","Instrumental Pop",
    "Instrumental Rock","Ethnic","Gothic","Darkwave","Techno-Industrial","Electronic",
    "Pop-Folk","Eurodance","Dream","Southern Rock","Comedy","Cult","Gangsta","Top 40",
    "Christian Rap","Pop/Funk","Jungle","Native American","Cabaret","New Wave",
    "Psychadelic","Rave","Showtunes","Trailer","Lo-Fi","Tribal","Acid Punk",
    "Acid Jazz","Polka","Retro","Musical","Rock & Roll","Hard Rock","Folk",
    "Folk-Rock","National Folk","Swing","Fast Fusion","Bebob","Latin","Revival",
    "Celtic","Bluegrass","Avantgarde","Gothic Rock","Progressive Rock",
    "Psychedelic Rock","Symphonic Rock","Slow Rock","Big Band","Chorus",
    "Easy Listening","Acoustic","Humour","Speech","Chanson","Opera","Chamber Music",
    "Sonata","Symphony","Booty Bass","Primus","Porn Groove","Satire","Slow Jam",
    "Club","Tango","Samba","Folklore","Ballad","Power Ballad","Rhythmic Soul",
    "Freestyle","Duet","Punk Rock","Drum Solo","A capella","Euro-House","Dance Hall"
};

/* Copy a fixed-width ID3v1 field, trimming trailing spaces/NULs. */
static void id3v1_field(char *dst, size_t cap, const uint8_t *p, size_t n) {
    size_t len = n;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\0')) len--;
    mn_str_set(dst, cap, (const char *)p, len);
}

/* Parse a trailing ID3v1 tag. buf/len is the whole file. Only fills fields
 * that are still empty so ID3v2 (parsed first) takes priority. */
static void parse_id3v1(mn_tags *t, const uint8_t *buf, size_t len) {
    if (len < 128) return;
    const uint8_t *p = buf + (len - 128);
    if (memcmp(p, "TAG", 3) != 0) return;

    char tmp[64];
    if (t->title[0] == '\0')  { id3v1_field(tmp, sizeof(tmp), p + 3, 30);  mn_str_set(t->title, sizeof(t->title), tmp, strlen(tmp)); }
    if (t->artist[0] == '\0') { id3v1_field(tmp, sizeof(tmp), p + 33, 30); mn_str_set(t->artist, sizeof(t->artist), tmp, strlen(tmp)); }
    if (t->album[0] == '\0')  { id3v1_field(tmp, sizeof(tmp), p + 63, 30); mn_str_set(t->album, sizeof(t->album), tmp, strlen(tmp)); }
    if (t->year == 0) {
        char y[5]; id3v1_field(y, sizeof(y), p + 93, 4);
        t->year = mn_parse_u16(y, strlen(y));
    }
    /* Track number: ID3v1.1 stores it in the last comment byte when the
     * preceding byte is 0. */
    if (t->track_no == 0 && p[125] == 0 && p[126] != 0) {
        t->track_no = p[126];
    }
    if (t->genre[0] == '\0') {
        uint8_t g = p[127];
        if (g < (uint8_t)(sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))) {
            const char *gn = id3v1_genres[g];
            mn_str_set(t->genre, sizeof(t->genre), gn, strlen(gn));
        }
    }
}

/* ==========================================================================
 * ID3v2 (MP3 leading tag)
 * ======================================================================== */

/* Decode a 28-bit synchsafe integer (7 bits per byte). */
static uint32_t synchsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/*
 * Decode an ID3v2 text-frame payload (with its leading encoding byte) into a
 * UTF-8 string written to dst. Supports ISO-8859-1, UTF-16 (BOM), UTF-16BE,
 * and UTF-8 encodings.
 */
static void id3_decode_text(char *dst, size_t cap, const uint8_t *frame, size_t flen) {
    if (cap == 0) return;
    dst[0] = '\0';
    if (flen < 1) return;
    uint8_t enc = frame[0];
    const uint8_t *p = frame + 1;
    size_t n = flen - 1;

    if (enc == 0x00 || enc == 0x03) {
        /* ISO-8859-1 (latin1) or UTF-8. Treat UTF-8 directly. */
        if (enc == 0x03) {
            /* strip a trailing NUL if present */
            while (n > 0 && p[n - 1] == '\0') n--;
            mn_str_set(dst, cap, (const char *)p, n);
        } else {
            /* latin1 -> UTF-8 */
            size_t di = 0;
            for (size_t i = 0; i < n && di + 2 < cap; i++) {
                unsigned char c = p[i];
                if (c == 0) break;
                if (c < 0x80) {
                    dst[di++] = (char)c;
                } else {
                    dst[di++] = (char)(0xC0 | (c >> 6));
                    dst[di++] = (char)(0x80 | (c & 0x3F));
                }
            }
            dst[di] = '\0';
        }
        return;
    }

    /* UTF-16 (enc 0x01 with BOM, enc 0x02 = UTF-16BE without BOM) */
    bool big_endian = (enc == 0x02);
    if (enc == 0x01 && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) { big_endian = false; p += 2; n -= 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { big_endian = true; p += 2; n -= 2; }
    }
    size_t di = 0;
    for (size_t i = 0; i + 1 < n && di + 4 < cap; i += 2) {
        uint32_t u = big_endian ? (uint32_t)((p[i] << 8) | p[i + 1])
                                : (uint32_t)((p[i + 1] << 8) | p[i]);
        if (u == 0) break;
        /* surrogate pair */
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            uint32_t lo = big_endian ? (uint32_t)((p[i + 2] << 8) | p[i + 3])
                                     : (uint32_t)((p[i + 3] << 8) | p[i + 2]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (u < 0x80) {
            dst[di++] = (char)u;
        } else if (u < 0x800) {
            dst[di++] = (char)(0xC0 | (u >> 6));
            dst[di++] = (char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            dst[di++] = (char)(0xE0 | (u >> 12));
            dst[di++] = (char)(0x80 | ((u >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (u & 0x3F));
        } else {
            dst[di++] = (char)(0xF0 | (u >> 18));
            dst[di++] = (char)(0x80 | ((u >> 12) & 0x3F));
            dst[di++] = (char)(0x80 | ((u >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (u & 0x3F));
        }
    }
    dst[di] = '\0';
}

/* Map an ID3v2 4-char frame id to a tags field and store decoded text. */
static void id3_apply_text_frame(mn_tags *t, const char *id,
                                 const uint8_t *frame, size_t flen) {
    char buf[512];
    if (!strncmp(id, "TIT2", 4) || !strncmp(id, "TT2", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        mn_str_set(t->title, sizeof(t->title), buf, strlen(buf));
    } else if (!strncmp(id, "TPE1", 4) || !strncmp(id, "TP1", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        mn_str_set(t->artist, sizeof(t->artist), buf, strlen(buf));
    } else if (!strncmp(id, "TALB", 4) || !strncmp(id, "TAL", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        mn_str_set(t->album, sizeof(t->album), buf, strlen(buf));
    } else if (!strncmp(id, "TPE2", 4)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        mn_str_set(t->album_artist, sizeof(t->album_artist), buf, strlen(buf));
    } else if (!strncmp(id, "TCON", 4) || !strncmp(id, "TCO", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        /* Genre may be "(17)" numeric or "(17)Rock" — resolve leading refs. */
        const char *g = buf;
        if (g[0] == '(') {
            size_t idx = 0; const char *q = g + 1;
            while (*q >= '0' && *q <= '9') { idx = idx * 10 + (size_t)(*q - '0'); q++; }
            if (*q == ')') q++;
            if (*q) {
                mn_str_set(t->genre, sizeof(t->genre), q, strlen(q));
            } else if (idx < sizeof(id3v1_genres) / sizeof(id3v1_genres[0])) {
                mn_str_set(t->genre, sizeof(t->genre), id3v1_genres[idx], strlen(id3v1_genres[idx]));
            }
        } else {
            mn_str_set(t->genre, sizeof(t->genre), g, strlen(g));
        }
    } else if (!strncmp(id, "TCOM", 4) || !strncmp(id, "TCM", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        mn_str_set(t->composer, sizeof(t->composer), buf, strlen(buf));
    } else if (!strncmp(id, "TYER", 4) || !strncmp(id, "TYE", 3) ||
               !strncmp(id, "TDRC", 4) || !strncmp(id, "TDRL", 4)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        uint16_t y = mn_parse_year(buf, strlen(buf));
        if (y) t->year = y;
    } else if (!strncmp(id, "TRCK", 4) || !strncmp(id, "TRK", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        uint16_t n = 0, tot = 0; mn_parse_pos(buf, strlen(buf), &n, &tot);
        if (n) t->track_no = n;
        if (tot) t->track_total = tot;
    } else if (!strncmp(id, "TPOS", 4) || !strncmp(id, "TPA", 3)) {
        id3_decode_text(buf, sizeof(buf), frame, flen);
        uint16_t n = 0, tot = 0; mn_parse_pos(buf, strlen(buf), &n, &tot);
        if (n) t->disc_no = n;
        if (tot) t->disc_total = tot;
    }
}

/* True iff the bytes begin with a raster-image signature we can decode. Used to
 * anchor the embedded picture start when the ID3 description length is
 * ambiguous. */
static bool img_magic_at(const uint8_t *b, size_t n) {
    if (!b || n < 4) return false;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;            /* JPEG */
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return true; /* PNG */
    if (b[0] == 'B' && b[1] == 'M') return true;                             /* BMP  */
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return true; /* GIF */
    if (n >= 12 && b[0]=='R'&&b[1]=='I'&&b[2]=='F'&&b[3]=='F'
        && b[8]=='W'&&b[9]=='E'&&b[10]=='B'&&b[11]=='P') return true;        /* WEBP */
    return false;
}

/* Parse an APIC (v2.3/2.4) or PIC (v2.2) picture frame into the cover sink. */
static void id3_apply_apic(mn_cover_sink *sink, bool v22,
                           const uint8_t *frame, size_t flen) {
    /* NOTE: do NOT bail on sink->have here — a later FRONT-cover (type 3) frame
     * must be able to upgrade an earlier non-front pick (mn_cover_take gates). */
    if (!sink || !sink->want || (sink->have && sink->ptype == 3) || flen < 4) return;
    size_t i = 0;
    int ptype = -1;
    uint8_t enc = frame[i++];
    char mime[64]; size_t mlen = 0;

    if (v22) {
        /* PIC: 3-char image format code (e.g. "JPG","PNG") */
        if (i + 3 > flen) return;
        const char *fmt = (const char *)(frame + i);
        if (!strncmp(fmt, "JPG", 3)) { strcpy(mime, "image/jpeg"); }
        else if (!strncmp(fmt, "PNG", 3)) { strcpy(mime, "image/png"); }
        else { strcpy(mime, ""); }
        mlen = strlen(mime);
        i += 3;
    } else {
        /* APIC: NUL-terminated MIME string (latin1) */
        while (i < flen && frame[i] != '\0' && mlen < sizeof(mime) - 1) {
            mime[mlen++] = (char)frame[i++];
        }
        mime[mlen] = '\0';
        while (i < flen && frame[i] != '\0') i++; /* skip rest of mime if long */
        if (i < flen) i++; /* skip NUL */
    }

    if (i >= flen) return;
    ptype = frame[i];   /* picture type: 3 = front cover */
    i++;

    /* Description: text with encoding-dependent terminator. The description
     * STARTS at `i` now, so the UTF-16 terminator (00 00) is aligned to THIS
     * offset — step in code units from desc_start, not from an assumed even
     * frame offset (getting that wrong left `i` a few bytes off, so the image
     * bytes began mid-description and failed to decode). */
    {
        size_t desc_start = i;
        if (enc == 0x01 || enc == 0x02) {
            /* optional BOM is 2 bytes (keeps alignment); scan 2-byte units. */
            while (i + 1 < flen && !(frame[i] == 0 && frame[i + 1] == 0)) i += 2;
            i += 2;                                   /* skip the 00 00 */
        } else {
            while (i < flen && frame[i] != '\0') i++;
            i += 1;                                   /* skip the NUL */
        }
        if (i > flen) i = flen;

        /* ROBUSTNESS: after skipping the description, the remaining bytes must
         * begin with a real image magic. If they don't (a mis-encoded or
         * mis-terminated description threw the offset off), SEARCH FORWARD from
         * desc_start for the first JPEG/PNG/GIF/BMP signature and start there.
         * This recovers covers that would otherwise decode to garbage. */
        if (i >= flen || !img_magic_at(frame + i, flen - i)) {
            size_t j, lim = flen - 3;
            bool found = false;
            for (j = desc_start; j < lim; ++j) {
                if (img_magic_at(frame + j, flen - j)) { i = j; found = true; break; }
            }
            if (!found) return;
        }
    }

    size_t img_len = flen - i;
    if (img_len == 0) return;
    mn_cover_take(sink, frame + i, img_len, mime, strlen(mime), ptype);
}

/*
 * Parse an ID3v2 tag located at the start of buf. Returns the total tag size
 * (header + body, including any footer) so the caller can skip past it, or 0
 * if no valid tag is present.
 */
static size_t parse_id3v2(mn_tags *t, mn_cover_sink *sink,
                          const uint8_t *buf, size_t len) {
    if (len < 10 || memcmp(buf, "ID3", 3) != 0) return 0;
    uint8_t major = buf[3];
    uint8_t flags = buf[5];
    uint32_t body = synchsafe(buf + 6);
    size_t total = 10 + body;
    if (major < 2 || major > 4) return total; /* unknown version: just skip */
    if (total > len) total = len;

    bool unsync_all = (flags & 0x80) != 0; /* global unsync (rarely honored) */
    (void)unsync_all;
    const uint8_t *p = buf + 10;
    const uint8_t *end = buf + total;

    /* Skip extended header if present (v2.3/v2.4). */
    if (flags & 0x40) {
        if (p + 4 <= end) {
            uint32_t exlen = (major == 4) ? synchsafe(p) : rd_be32(p);
            const uint8_t *nextp = p + (major == 4 ? exlen : exlen + 4);
            if (nextp > p && nextp <= end) p = nextp;
        }
    }

    bool v22 = (major == 2);
    size_t id_len = v22 ? 3 : 4;
    size_t size_len = v22 ? 3 : 4;
    size_t hdr = v22 ? 6 : 10;

    while (p + hdr <= end) {
        char id[5] = {0};
        memcpy(id, p, id_len);
        if (id[0] == 0) break; /* padding */

        uint32_t fsize;
        if (v22) {
            fsize = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
        } else if (major == 4) {
            fsize = synchsafe(p + 4);
        } else {
            fsize = rd_be32(p + 4);
        }
        (void)size_len;
        const uint8_t *fp = p + hdr;
        if (fp + fsize > end) break;

        if (id[0] == 'T') {
            id3_apply_text_frame(t, id, fp, fsize);
        } else if ((!v22 && !strncmp(id, "APIC", 4)) ||
                   (v22 && !strncmp(id, "PIC", 3))) {
            id3_apply_apic(sink, v22, fp, fsize);
        }

        p = fp + fsize;
    }
    return total;
}

/* ==========================================================================
 * MP3 audio properties (first frame header + Xing/Info/VBRI)
 * ======================================================================== */

static const int mp3_bitrate_v1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const int mp3_bitrate_v2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
static const int mp3_srate[4][3] = {
    {11025,12000,8000},   /* MPEG 2.5 */
    {0,0,0},              /* reserved */
    {22050,24000,16000},  /* MPEG 2   */
    {44100,48000,32000}   /* MPEG 1   */
};

/* Locate the first valid MPEG audio frame and derive properties. Sets
 * sample_rate, channels, bitrate_kbps, duration_ms. audio_start = offset past
 * any ID3v2 tag. */
static void mp3_properties(mn_tags *t, const uint8_t *buf, size_t len,
                           size_t audio_start, size_t audio_len) {
    const uint8_t *base = buf + audio_start;
    size_t n = audio_len;
    size_t i = 0;
    /* scan for frame sync 0xFFEx */
    for (; i + 4 < n; i++) {
        if (base[i] == 0xFF && (base[i + 1] & 0xE0) == 0xE0) {
            uint8_t b1 = base[i + 1], b2 = base[i + 2];
            int version_id = (b1 >> 3) & 0x3;   /* 0=2.5 1=res 2=v2 3=v1 */
            int layer = (b1 >> 1) & 0x3;        /* 1=III 2=II 3=I */
            int br_idx = (b2 >> 4) & 0xF;
            int sr_idx = (b2 >> 2) & 0x3;
            if (version_id == 1 || layer == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3)
                continue;
            int sample_rate = mp3_srate[version_id][sr_idx];
            if (sample_rate == 0) continue;
            int bitrate = (version_id == 3) ? mp3_bitrate_v1[br_idx]
                                            : mp3_bitrate_v2[br_idx];
            if (bitrate == 0) continue;
            int chan_mode = (base[i + 3] >> 6) & 0x3;
            t->sample_rate = (uint32_t)sample_rate;
            t->channels = (chan_mode == 3) ? 1 : 2;
            t->bitrate_kbps = (uint32_t)bitrate;
            t->bit_depth = 0; /* lossy */

            /* Samples per frame */
            int spf;
            if (layer == 3) spf = 384;                 /* Layer I */
            else if (layer == 2) spf = 1152;           /* Layer II */
            else spf = (version_id == 3) ? 1152 : 576; /* Layer III */

            /* Look for Xing/Info/VBRI to get accurate frame count. */
            uint32_t frames = 0;
            /* Xing/Info offset depends on version + channel mode. */
            int side_info;
            if (version_id == 3) side_info = (chan_mode == 3) ? 17 : 32;
            else side_info = (chan_mode == 3) ? 9 : 17;
            size_t xoff = i + 4 + (size_t)side_info;
            if (xoff + 12 <= n &&
                (memcmp(base + xoff, "Xing", 4) == 0 ||
                 memcmp(base + xoff, "Info", 4) == 0)) {
                uint32_t xflags = rd_be32(base + xoff + 4);
                if (xflags & 0x1) frames = rd_be32(base + xoff + 8);
            } else if (i + 4 + 32 + 4 <= n &&
                       memcmp(base + i + 4 + 32, "VBRI", 4) == 0) {
                frames = rd_be32(base + i + 4 + 32 + 14);
            }

            if (frames > 0) {
                double dur = (double)frames * (double)spf / (double)sample_rate;
                t->duration_ms = (uint64_t)(dur * 1000.0 + 0.5);
                /* Recompute average bitrate from size for VBR. */
                if (dur > 0.0) {
                    double kbps = ((double)audio_len * 8.0) / dur / 1000.0;
                    t->bitrate_kbps = (uint32_t)(kbps + 0.5);
                }
            } else if (bitrate > 0) {
                /* CBR estimate from stream size. */
                double dur = ((double)audio_len * 8.0) / ((double)bitrate * 1000.0);
                t->duration_ms = (uint64_t)(dur * 1000.0 + 0.5);
            }
            return;
        }
    }
}

/* Full MP3 parse: ID3v2 (front), audio properties, ID3v1 (back). */
static bool parse_mp3(mn_tags *t, mn_cover_sink *sink,
                      const uint8_t *buf, size_t len) {
    size_t audio_start = 0;
    size_t tag = parse_id3v2(t, sink, buf, len);
    if (tag > 0 && tag <= len) audio_start = tag;

    size_t audio_len = len - audio_start;
    /* Trim a trailing ID3v1 tag from the audio length for accurate bitrate. */
    if (len >= 128 && memcmp(buf + len - 128, "TAG", 3) == 0 && audio_len >= 128)
        audio_len -= 128;

    mp3_properties(t, buf, len, audio_start, audio_len);
    parse_id3v1(t, buf, len);
    return true;
}

/* ==========================================================================
 * FLAC (native)
 * ======================================================================== */

/* Parse a Vorbis comment block body (vendor + user comments). */
static void parse_vorbis_comments(mn_tags *t, const uint8_t *p, size_t n) {
    size_t i = 0;
    if (i + 4 > n) return;
    uint32_t vlen = rd_le32(p + i); i += 4;
    if (i + vlen > n) return;
    i += vlen; /* skip vendor string */
    if (i + 4 > n) return;
    uint32_t count = rd_le32(p + i); i += 4;

    for (uint32_t c = 0; c < count; c++) {
        if (i + 4 > n) return;
        uint32_t clen = rd_le32(p + i); i += 4;
        if (i + clen > n) return;
        const char *entry = (const char *)(p + i);
        /* split on first '=' */
        size_t eq = 0;
        while (eq < clen && entry[eq] != '=') eq++;
        if (eq < clen) {
            mn_apply_field(t, entry, eq, entry + eq + 1, clen - eq - 1);
        }
        i += clen;
    }
}

/* Decode base64 (standard alphabet) into a freshly allocated buffer.
 * Returns NULL on failure; *out_len set to decoded length. */
static uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len) {
    static const signed char tbl_init = -1;
    signed char tbl[256];
    memset(tbl, tbl_init, sizeof(tbl));
    const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int k = 0; k < 64; k++) tbl[(unsigned char)al[k]] = (signed char)k;

    uint8_t *out = (uint8_t *)malloc(src_len / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        signed char v = tbl[(unsigned char)c];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    if (out_len) *out_len = o;
    return out;
}

/*
 * Parse a METADATA_BLOCK_PICTURE binary body (as used in FLAC PICTURE blocks
 * and, base64-decoded, in Vorbis comments). Layout is all big-endian:
 *   u32 type, u32 mime_len, mime, u32 desc_len, desc,
 *   u32 w, u32 h, u32 depth, u32 colors, u32 data_len, data.
 */
static void parse_metadata_picture(mn_cover_sink *sink,
                                   const uint8_t *p, size_t n) {
    if (!sink || !sink->want || (sink->have && sink->ptype == 3)) return;
    size_t i = 0;
    if (i + 8 > n) return;
    int ptype = (int)rd_be32(p + i); i += 4;   /* picture type: 3 = front */
    uint32_t mlen = rd_be32(p + i); i += 4;
    if (i + mlen + 4 > n) return;
    const char *mime = (const char *)(p + i);
    i += mlen;
    uint32_t dlen = rd_be32(p + i); i += 4;
    if (i + dlen + 20 > n) return;
    i += dlen;                 /* description */
    i += 16;                   /* w,h,depth,colors */
    uint32_t datalen = rd_be32(p + i); i += 4;
    if (i + datalen > n) return;
    mn_cover_take(sink, p + i, datalen, mime, mlen, ptype);
}

/* Parse native FLAC: metadata blocks + STREAMINFO for audio properties. */
static bool parse_flac(mn_tags *t, mn_cover_sink *sink,
                       const uint8_t *buf, size_t len) {
    if (len < 4 || memcmp(buf, "fLaC", 4) != 0) return false;
    size_t i = 4;
    bool last = false;
    while (!last && i + 4 <= len) {
        uint8_t hdr = buf[i];
        last = (hdr & 0x80) != 0;
        uint8_t type = hdr & 0x7F;
        uint32_t blen = ((uint32_t)buf[i + 1] << 16) |
                        ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
        i += 4;
        if (i + blen > len) break;
        const uint8_t *body = buf + i;

        if (type == 0 && blen >= 18) {
            /* STREAMINFO: bit-packed sample rate(20) / channels(3) / bps(5) /
             * total samples(36) starting at body[10]. */
            uint32_t sr = ((uint32_t)body[10] << 12) |
                          ((uint32_t)body[11] << 4) |
                          ((uint32_t)(body[12] >> 4));
            uint8_t chan = (uint8_t)(((body[12] >> 1) & 0x7) + 1);
            uint8_t bps = (uint8_t)((((body[12] & 0x1) << 4) | (body[13] >> 4)) + 1);
            uint64_t total = ((uint64_t)(body[13] & 0x0F) << 32) |
                             ((uint64_t)body[14] << 24) |
                             ((uint64_t)body[15] << 16) |
                             ((uint64_t)body[16] << 8) |
                             (uint64_t)body[17];
            t->sample_rate = sr;
            t->channels = chan;
            t->bit_depth = bps;
            if (sr > 0 && total > 0) {
                t->duration_ms = (uint64_t)((double)total * 1000.0 / (double)sr + 0.5);
                double bytes = (double)len;
                double secs = (double)total / (double)sr;
                if (secs > 0.0)
                    t->bitrate_kbps = (uint32_t)((bytes * 8.0) / secs / 1000.0 + 0.5);
            }
        } else if (type == 4) {
            parse_vorbis_comments(t, body, blen);
        } else if (type == 6) {
            parse_metadata_picture(sink, body, blen);
        }
        i += blen;
    }
    return true;
}

/* ==========================================================================
 * OGG / OPUS (Ogg container carrying Vorbis or Opus)
 * ======================================================================== */

/*
 * Gather the logical-bitstream packet payloads from the first pages of an Ogg
 * stream into a contiguous buffer, so the header packets (identification +
 * comment) can be parsed regardless of page boundaries. We only need the
 * beginning of the stream, so we cap how much we assemble.
 *
 * Returns a heap buffer (caller frees) and sets *out_len, plus the granule of
 * the last page seen in *last_granule and the first page's serial.
 */
static uint8_t *ogg_collect_headers(const uint8_t *buf, size_t len,
                                    size_t *out_len,
                                    uint64_t *last_granule,
                                    uint32_t *serial_out) {
    size_t cap = 1 << 16;
    uint8_t *acc = (uint8_t *)malloc(cap);
    if (!acc) return NULL;
    size_t acc_len = 0;
    size_t i = 0;
    uint32_t serial = 0;
    bool have_serial = false;
    int pages = 0;

    while (i + 27 <= len) {
        if (memcmp(buf + i, "OggS", 4) != 0) break;
        uint8_t nsegs = buf[i + 26];
        if (i + 27 + nsegs > len) break;
        uint32_t page_serial = rd_le32(buf + i + 14);
        uint64_t granule = rd_le64(buf + i + 6);
        if (last_granule && granule != (uint64_t)-1) *last_granule = granule;
        if (!have_serial) { serial = page_serial; have_serial = true; }

        size_t body = 0;
        for (uint8_t s = 0; s < nsegs; s++) body += buf[i + 27 + s];
        size_t body_off = i + 27 + nsegs;
        if (body_off + body > len) break;

        if (page_serial == serial && acc_len < (1 << 15)) {
            if (acc_len + body > cap) {
                size_t ncap = cap;
                while (ncap < acc_len + body) ncap *= 2;
                uint8_t *na = (uint8_t *)realloc(acc, ncap);
                if (!na) break;
                acc = na; cap = ncap;
            }
            memcpy(acc + acc_len, buf + body_off, body);
            acc_len += body;
        }
        i = body_off + body;
        if (++pages > 12 && acc_len > 0) {
            /* Continue scanning only to update last_granule cheaply: jump to
             * the final page by scanning backwards later instead. */
            break;
        }
    }

    /* Find last page granule by scanning from the end for the final "OggS". */
    if (last_granule) {
        for (size_t j = len >= 27 ? len - 27 : 0; ; j--) {
            if (memcmp(buf + j, "OggS", 4) == 0) {
                uint64_t g = rd_le64(buf + j + 6);
                if (g != (uint64_t)-1) { *last_granule = g; }
                break;
            }
            if (j == 0) break;
        }
    }

    if (serial_out) *serial_out = serial;
    if (out_len) *out_len = acc_len;
    return acc;
}

/* Parse an Opus stream's headers (OpusHead + OpusTags). */
static bool parse_opus(mn_tags *t, mn_cover_sink *sink,
                       const uint8_t *hdr, size_t hlen, uint64_t last_granule) {
    /* OpusHead: "OpusHead"(8) ver(1) chan(1) preskip(2) inputSR(4)... */
    size_t i = 0;
    if (hlen < 19 || memcmp(hdr, "OpusHead", 8) != 0) return false;
    uint8_t chan = hdr[9];
    uint32_t input_rate = rd_le32(hdr + 12);
    t->channels = chan;
    t->sample_rate = input_rate ? input_rate : 48000;
    t->bit_depth = 0;
    /* Opus granules count at 48kHz. */
    if (last_granule != (uint64_t)-1 && last_granule > 0)
        t->duration_ms = (uint64_t)((double)last_granule * 1000.0 / 48000.0 + 0.5);

    /* Find OpusTags packet within the assembled header buffer. */
    for (i = 8; i + 8 <= hlen; i++) {
        if (memcmp(hdr + i, "OpusTags", 8) == 0) {
            const uint8_t *p = hdr + i + 8;
            size_t n = hlen - i - 8;
            /* OpusTags layout mirrors a Vorbis comment body but with a
             * METADATA_BLOCK_PICTURE stored inside a comment value. Parse via
             * the same routine, plus explicit picture handling. */
            parse_vorbis_comments(t, p, n);
            /* Handle METADATA_BLOCK_PICTURE comments for cover art. */
            /* Re-walk comments to catch base64 picture blocks. */
            size_t j = 0;
            if (j + 4 <= n) {
                uint32_t vlen = rd_le32(p + j); j += 4;
                if (j + vlen <= n) {
                    j += vlen;
                    if (j + 4 <= n) {
                        uint32_t count = rd_le32(p + j); j += 4;
                        for (uint32_t c = 0; c < count && j + 4 <= n; c++) {
                            uint32_t clen = rd_le32(p + j); j += 4;
                            if (j + clen > n) break;
                            const char *entry = (const char *)(p + j);
                            if (clen > 23 &&
                                strncmp(entry, "METADATA_BLOCK_PICTURE=", 23) == 0) {
                                size_t declen = 0;
                                uint8_t *pic = base64_decode(entry + 23, clen - 23, &declen);
                                if (pic) {
                                    parse_metadata_picture(sink, pic, declen);
                                    free(pic);
                                }
                            }
                            j += clen;
                        }
                    }
                }
            }
            break;
        }
    }
    return true;
}

/* Parse a Vorbis stream's headers (identification + comment). */
static bool parse_ogg_vorbis(mn_tags *t, mn_cover_sink *sink,
                             const uint8_t *hdr, size_t hlen, uint64_t last_granule) {
    /* Identification header packet: 0x01 "vorbis" ver(4) chan(1) rate(4) ... */
    size_t idpos = SIZE_MAX;
    for (size_t i = 0; i + 7 <= hlen; i++) {
        if (hdr[i] == 0x01 && memcmp(hdr + i + 1, "vorbis", 6) == 0) { idpos = i; break; }
    }
    if (idpos == SIZE_MAX) return false;
    if (idpos + 16 <= hlen) {
        uint8_t chan = hdr[idpos + 11];
        uint32_t rate = rd_le32(hdr + idpos + 12);
        t->channels = chan;
        t->sample_rate = rate;
        t->bit_depth = 0;
        if (rate > 0 && last_granule != (uint64_t)-1 && last_granule > 0)
            t->duration_ms = (uint64_t)((double)last_granule * 1000.0 / (double)rate + 0.5);
    }
    /* Comment header packet: 0x03 "vorbis" then a Vorbis comment body. */
    for (size_t i = 0; i + 7 <= hlen; i++) {
        if (hdr[i] == 0x03 && memcmp(hdr + i + 1, "vorbis", 6) == 0) {
            const uint8_t *p = hdr + i + 7;
            size_t n = hlen - i - 7;
            parse_vorbis_comments(t, p, n);
            /* base64 picture blocks */
            size_t j = 0;
            if (j + 4 <= n) {
                uint32_t vlen = rd_le32(p + j); j += 4;
                if (j + vlen <= n) {
                    j += vlen;
                    if (j + 4 <= n) {
                        uint32_t count = rd_le32(p + j); j += 4;
                        for (uint32_t c = 0; c < count && j + 4 <= n; c++) {
                            uint32_t clen = rd_le32(p + j); j += 4;
                            if (j + clen > n) break;
                            const char *entry = (const char *)(p + j);
                            if (clen > 23 &&
                                strncmp(entry, "METADATA_BLOCK_PICTURE=", 23) == 0) {
                                size_t declen = 0;
                                uint8_t *pic = base64_decode(entry + 23, clen - 23, &declen);
                                if (pic) { parse_metadata_picture(sink, pic, declen); free(pic); }
                            }
                            j += clen;
                        }
                    }
                }
            }
            break;
        }
    }
    return true;
}

/* Dispatch an Ogg container to the Vorbis or Opus header parser. */
static bool parse_ogg(mn_tags *t, mn_cover_sink *sink,
                      const uint8_t *buf, size_t len) {
    if (len < 4 || memcmp(buf, "OggS", 4) != 0) return false;
    size_t hlen = 0; uint64_t granule = (uint64_t)-1; uint32_t serial = 0;
    uint8_t *hdr = ogg_collect_headers(buf, len, &hlen, &granule, &serial);
    if (!hdr) return false;

    bool ok = false;
    if (hlen >= 8 && memcmp(hdr, "OpusHead", 8) == 0) {
        ok = parse_opus(t, sink, hdr, hlen, granule);
    } else {
        ok = parse_ogg_vorbis(t, sink, hdr, hlen, granule);
    }
    free(hdr);
    return ok || true; /* recognized as Ogg even if header parse was partial */
}

/* ==========================================================================
 * MP4 / M4A (ISO base media, iTunes 'ilst' atoms)
 * ======================================================================== */

/* Map a 4-char iTunes atom name to an mn_tags text field. */
static void mp4_apply_text(mn_tags *t, const char *name,
                           const char *val, size_t vlen) {
    if (!memcmp(name, "\xA9""nam", 4)) mn_str_set(t->title, sizeof(t->title), val, vlen);
    else if (!memcmp(name, "\xA9""ART", 4)) mn_str_set(t->artist, sizeof(t->artist), val, vlen);
    else if (!memcmp(name, "\xA9""alb", 4)) mn_str_set(t->album, sizeof(t->album), val, vlen);
    else if (!memcmp(name, "aART", 4)) mn_str_set(t->album_artist, sizeof(t->album_artist), val, vlen);
    else if (!memcmp(name, "\xA9""gen", 4)) mn_str_set(t->genre, sizeof(t->genre), val, vlen);
    else if (!memcmp(name, "\xA9""wrt", 4)) mn_str_set(t->composer, sizeof(t->composer), val, vlen);
    else if (!memcmp(name, "\xA9""day", 4)) { uint16_t y = mn_parse_year(val, vlen); if (y) t->year = y; }
}

/*
 * Recursively walk MP4 atoms within [p, p+n). Container atoms (moov, udta,
 * meta, ilst, trak, mdia, minf, stbl) are descended into. Leaf metadata items
 * inside 'ilst' contain a 'data' child with a type/value payload.
 *
 * `depth` guards against pathological nesting. `in_ilst` tells us the current
 * children are iTunes metadata items.
 */
static void mp4_walk(mn_tags *t, mn_cover_sink *sink,
                     const uint8_t *p, size_t n, int depth, bool in_ilst) {
    if (depth > 12) return;
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t size = rd_be32(p + i);
        const char *type = (const char *)(p + i + 4);
        size_t hdr = 8;
        if (size == 1) { /* 64-bit extended size */
            if (i + 16 > n) break;
            size = rd_be64(p + i + 8);
            hdr = 16;
        } else if (size == 0) {
            size = n - i; /* extends to end */
        }
        if (size < hdr || i + size > n) break;
        const uint8_t *body = p + i + hdr;
        size_t body_len = (size_t)size - hdr;

        if (in_ilst) {
            /* This atom is a metadata item; its name is `type`. Find its
             * 'data' child. */
            size_t j = 0;
            while (j + 8 <= body_len) {
                uint32_t dsize = rd_be32(body + j);
                const char *dtype = (const char *)(body + j + 4);
                if (dsize < 8 || j + dsize > body_len) break;
                if (!memcmp(dtype, "data", 4) && dsize >= 16) {
                    uint32_t dflags = rd_be32(body + j + 8) & 0xFFFFFF;
                    const uint8_t *dval = body + j + 16;
                    size_t dlen = dsize - 16;
                    if (!memcmp(type, "covr", 4)) {
                        if (sink && sink->want && !sink->have && dlen > 0) {
                            /* flags: 13 = JPEG, 14 = PNG */
                            const char *mime = (dflags == 14) ? "image/png"
                                             : (dflags == 13) ? "image/jpeg" : "";
                            /* Sniff if flags absent. */
                            if (mime[0] == '\0' && dlen > 4) {
                                if (dval[0] == 0x89 && dval[1] == 'P') mime = "image/png";
                                else if (dval[0] == 0xFF && dval[1] == 0xD8) mime = "image/jpeg";
                            }
                            /* MP4 covr has no picture-type; treat as final (-1). */
                            mn_cover_take(sink, dval, dlen, mime, strlen(mime), -1);
                        }
                    } else if (!memcmp(type, "trkn", 4)) {
                        if (dlen >= 6) {
                            t->track_no = rd_be16(dval + 2);
                            if (dlen >= 8) t->track_total = rd_be16(dval + 4);
                        }
                    } else if (!memcmp(type, "disk", 4)) {
                        if (dlen >= 6) {
                            t->disc_no = rd_be16(dval + 2);
                            if (dlen >= 8) t->disc_total = rd_be16(dval + 4);
                        }
                    } else if (!memcmp(type, "gnre", 4)) {
                        /* numeric genre (1-based into id3v1 table) */
                        if (dlen >= 2 && t->genre[0] == '\0') {
                            uint16_t g = rd_be16(dval);
                            if (g >= 1 && g <= sizeof(id3v1_genres)/sizeof(id3v1_genres[0]))
                                mn_str_set(t->genre, sizeof(t->genre),
                                           id3v1_genres[g - 1], strlen(id3v1_genres[g - 1]));
                        }
                    } else {
                        mp4_apply_text(t, type, (const char *)dval, dlen);
                    }
                    break;
                }
                if (dsize == 0) break;
                j += dsize;
            }
        } else if (!memcmp(type, "moov", 4) || !memcmp(type, "udta", 4) ||
                   !memcmp(type, "trak", 4) || !memcmp(type, "mdia", 4) ||
                   !memcmp(type, "minf", 4) || !memcmp(type, "stbl", 4)) {
            mp4_walk(t, sink, body, body_len, depth + 1, false);
        } else if (!memcmp(type, "meta", 4)) {
            /* 'meta' has a 4-byte version/flags prefix before children. */
            if (body_len >= 4)
                mp4_walk(t, sink, body + 4, body_len - 4, depth + 1, false);
        } else if (!memcmp(type, "ilst", 4)) {
            mp4_walk(t, sink, body, body_len, depth + 1, true);
        } else if (!memcmp(type, "mvhd", 4)) {
            /* Movie header: timescale + duration -> total duration. */
            if (body_len >= 1) {
                uint8_t ver = body[0];
                if (ver == 1 && body_len >= 28) {
                    uint32_t ts = rd_be32(body + 20);
                    uint64_t dur = rd_be64(body + 24 - 4); /* offset care below */
                    /* v1: creation(8) mod(8) timescale(4) duration(8) starting
                     * after the 4-byte version/flags. */
                    ts = rd_be32(body + 4 + 8 + 8);
                    dur = rd_be64(body + 4 + 8 + 8 + 4);
                    if (ts > 0)
                        t->duration_ms = (uint64_t)((double)dur * 1000.0 / (double)ts + 0.5);
                } else if (body_len >= 20) {
                    uint32_t ts = rd_be32(body + 4 + 4 + 4);
                    uint32_t dur = rd_be32(body + 4 + 4 + 4 + 4);
                    if (ts > 0)
                        t->duration_ms = (uint64_t)((double)dur * 1000.0 / (double)ts + 0.5);
                }
            }
        }

        if (size == 0) break;
        i += (size_t)size;
    }
}

/*
 * Extract audio sample rate / channels from an 'mp4a' sample entry inside the
 * stsd box. We do a light scan for "mp4a" and read the AudioSampleEntry fields.
 */
static void mp4_audio_props(mn_tags *t, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i + 28 <= len; i++) {
        if (memcmp(buf + i, "mp4a", 4) == 0 || memcmp(buf + i, "alac", 4) == 0) {
            /* AudioSampleEntry: after 4-byte type, 6 reserved, 2 data-ref idx,
             * 8 reserved, 2 channelcount, 2 samplesize, 4 pre/reserved,
             * 4 samplerate(16.16). */
            const uint8_t *e = buf + i + 4;
            if (buf + i + 4 + 20 > buf + len) continue;
            uint16_t chans = rd_be16(e + 16);
            uint16_t ssize = rd_be16(e + 18);
            uint32_t sr = rd_be32(e + 22) >> 16;
            if (sr > 0 && chans > 0 && chans <= 8) {
                t->channels = chans;
                t->sample_rate = sr;
                if (memcmp(buf + i, "alac", 4) == 0) {
                    t->bit_depth = ssize;
                    snprintf(t->codec, sizeof(t->codec), "ALAC");
                } else {
                    t->bit_depth = 0;
                    snprintf(t->codec, sizeof(t->codec), "AAC");
                }
                return;
            }
        }
    }
}

/* Parse an MP4/M4A file. */
static bool parse_mp4(mn_tags *t, mn_cover_sink *sink,
                      const uint8_t *buf, size_t len) {
    /* Verify an 'ftyp' box near the start. */
    if (len < 12) return false;
    if (memcmp(buf + 4, "ftyp", 4) != 0) {
        /* some files start with other boxes; still accept if we see moov */
        bool seen = false;
        for (size_t i = 0; i + 8 <= len && i < 65536; i++) {
            if (memcmp(buf + i + 4, "moov", 4) == 0) { seen = true; break; }
        }
        if (!seen) return false;
    }
    mp4_walk(t, sink, buf, len, 0, false);
    mp4_audio_props(t, buf, len);
    if (t->bitrate_kbps == 0 && t->duration_ms > 0) {
        double secs = (double)t->duration_ms / 1000.0;
        if (secs > 0.0)
            t->bitrate_kbps = (uint32_t)(((double)len * 8.0) / secs / 1000.0 + 0.5);
    }
    return true;
}

/* ==========================================================================
 * WAV (RIFF) — 'fmt ', 'data', LIST/INFO, optional 'id3 ' chunk
 * ======================================================================== */

/* Map a RIFF INFO 4-char id to a tags field. */
static void wav_apply_info(mn_tags *t, const char *id,
                           const char *val, size_t vlen) {
    while (vlen > 0 && (val[vlen - 1] == '\0' || val[vlen - 1] == ' ')) vlen--;
    if (!memcmp(id, "INAM", 4)) mn_str_set(t->title, sizeof(t->title), val, vlen);
    else if (!memcmp(id, "IART", 4)) mn_str_set(t->artist, sizeof(t->artist), val, vlen);
    else if (!memcmp(id, "IPRD", 4)) mn_str_set(t->album, sizeof(t->album), val, vlen);
    else if (!memcmp(id, "IGNR", 4)) mn_str_set(t->genre, sizeof(t->genre), val, vlen);
    else if (!memcmp(id, "ICRD", 4)) { uint16_t y = mn_parse_year(val, vlen); if (y) t->year = y; }
    else if (!memcmp(id, "ITRK", 4) || !memcmp(id, "IPRT", 4)) {
        uint16_t nn = 0, tt = 0; mn_parse_pos(val, vlen, &nn, &tt);
        if (nn) t->track_no = nn; if (tt) t->track_total = tt;
    } else if (!memcmp(id, "ICMP", 4) || !memcmp(id, "IMUS", 4)) {
        mn_str_set(t->composer, sizeof(t->composer), val, vlen);
    }
}

/* Parse a WAV/RIFF file. */
static bool parse_wav(mn_tags *t, mn_cover_sink *sink,
                      const uint8_t *buf, size_t len) {
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;
    size_t i = 12;
    uint64_t data_bytes = 0;
    uint32_t byte_rate = 0;
    while (i + 8 <= len) {
        const char *cid = (const char *)(buf + i);
        uint32_t csize = rd_le32(buf + i + 4);
        size_t body = i + 8;
        if (body + csize > len) csize = (uint32_t)(len - body);

        if (!memcmp(cid, "fmt ", 4) && csize >= 16) {
            uint16_t channels = rd_le16(buf + body + 2);
            uint32_t sr = rd_le32(buf + body + 4);
            byte_rate = rd_le32(buf + body + 8);
            uint16_t bits = rd_le16(buf + body + 14);
            t->channels = channels;
            t->sample_rate = sr;
            t->bit_depth = bits;
        } else if (!memcmp(cid, "data", 4)) {
            data_bytes = csize;
        } else if (!memcmp(cid, "LIST", 4) && csize >= 4 &&
                   !memcmp(buf + body, "INFO", 4)) {
            size_t j = body + 4;
            size_t list_end = body + csize;
            while (j + 8 <= list_end) {
                const char *sid = (const char *)(buf + j);
                uint32_t ssize = rd_le32(buf + j + 4);
                size_t sbody = j + 8;
                if (sbody + ssize > list_end) break;
                wav_apply_info(t, sid, (const char *)(buf + sbody), ssize);
                j = sbody + ssize + (ssize & 1); /* word-align */
            }
        } else if ((!memcmp(cid, "id3 ", 4) || !memcmp(cid, "ID3 ", 4)) && csize >= 10) {
            parse_id3v2(t, sink, buf + body, csize);
        }

        i = body + csize + (csize & 1); /* chunks are word-aligned */
    }

    if (byte_rate > 0) {
        t->bitrate_kbps = (uint32_t)((byte_rate * 8) / 1000);
        if (data_bytes > 0)
            t->duration_ms = (uint64_t)((double)data_bytes * 1000.0 / (double)byte_rate + 0.5);
    }
    return true;
}

/* ==========================================================================
 * Format detection + top-level dispatch
 * ======================================================================== */

typedef enum {
    MN_FMT_UNKNOWN = 0,
    MN_FMT_MP3, MN_FMT_FLAC, MN_FMT_MP4, MN_FMT_OGG, MN_FMT_WAV
} mn_format;

/* Detect the container format from magic bytes (with an extension hint). */
static mn_format detect_format(const uint8_t *buf, size_t len, const char *path) {
    if (len >= 4 && memcmp(buf, "fLaC", 4) == 0) return MN_FMT_FLAC;
    if (len >= 4 && memcmp(buf, "OggS", 4) == 0) return MN_FMT_OGG;
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0)
        return MN_FMT_WAV;
    if (len >= 12 && memcmp(buf + 4, "ftyp", 4) == 0) return MN_FMT_MP4;
    if (len >= 3 && memcmp(buf, "ID3", 3) == 0) return MN_FMT_MP3;
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) return MN_FMT_MP3;

    /* Extension hint fallback. */
    if (path) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            char ext[8]; size_t k = 0;
            for (const char *e = dot + 1; *e && k < sizeof(ext) - 1; e++)
                ext[k++] = (char)tolower((unsigned char)*e);
            ext[k] = '\0';
            if (!strcmp(ext, "mp3")) return MN_FMT_MP3;
            if (!strcmp(ext, "flac")) return MN_FMT_FLAC;
            if (!strcmp(ext, "m4a") || !strcmp(ext, "m4b") ||
                !strcmp(ext, "mp4") || !strcmp(ext, "aac")) return MN_FMT_MP4;
            if (!strcmp(ext, "ogg") || !strcmp(ext, "oga") ||
                !strcmp(ext, "opus")) return MN_FMT_OGG;
            if (!strcmp(ext, "wav") || !strcmp(ext, "wave")) return MN_FMT_WAV;
        }
    }
    return MN_FMT_UNKNOWN;
}

/* Run the appropriate parser for the detected format, stamping the REAL
 * codec label (magic-byte truth, not the extension: .m4a is ALAC or AAC,
 * .ogg may be OPUS). MP4 stamps its own inside mp4_audio_props. */
static bool mn_parse_dispatch(mn_tags *t, mn_cover_sink *sink,
                              const uint8_t *buf, size_t len, const char *path) {
    bool ok;
    switch (detect_format(buf, len, path)) {
        case MN_FMT_MP3:
            ok = parse_mp3(t, sink, buf, len);
            if (ok) snprintf(t->codec, sizeof(t->codec), "MP3");
            return ok;
        case MN_FMT_FLAC:
            ok = parse_flac(t, sink, buf, len);
            if (ok) snprintf(t->codec, sizeof(t->codec), "FLAC");
            return ok;
        case MN_FMT_MP4:
            return parse_mp4(t, sink, buf, len);
        case MN_FMT_OGG:
            ok = parse_ogg(t, sink, buf, len);
            if (ok && !t->codec[0]) {
                /* Opus vs Vorbis: the id header sits in the first page. */
                size_t i, cap = len < 512 ? len : 512;
                const char *lbl = "VORBIS";
                for (i = 0; i + 8 <= cap; i++) {
                    if (memcmp(buf + i, "OpusHead", 8) == 0) { lbl = "OPUS"; break; }
                }
                snprintf(t->codec, sizeof(t->codec), "%s", lbl);
            }
            return ok;
        case MN_FMT_WAV:
            ok = parse_wav(t, sink, buf, len);
            if (ok) snprintf(t->codec, sizeof(t->codec), "WAV");
            return ok;
        default:
            return false;
    }
}

/* ==========================================================================
 * Public API
 * ======================================================================== */

bool mn_tags_read(const char *path, mn_tags *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    uint8_t *buf = mn_read_whole_file(path, &len);
    if (!buf) return false;

    mn_cover_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.want = false; /* metadata only, no cover capture */

    bool ok = mn_parse_dispatch(out, &sink, buf, len, path);

    free(buf);
    /* sink.bytes is never allocated when want == false, but be safe. */
    if (sink.bytes) free(sink.bytes);
    return ok;
}

bool mn_tags_read_cover(const char *path, uint8_t **bytes, size_t *len, char *mime) {
    if (!path || !bytes || !len) return false;
    *bytes = NULL;
    *len = 0;
    if (mime) mime[0] = '\0';

    size_t flen = 0;
    uint8_t *buf = mn_read_whole_file(path, &flen);
    if (!buf) return false;

    mn_tags scratch;
    memset(&scratch, 0, sizeof(scratch));

    mn_cover_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.want = true;

    (void)mn_parse_dispatch(&scratch, &sink, buf, flen, path);
    free(buf);

    if (sink.have && sink.bytes && sink.len > 0) {
        *bytes = sink.bytes;
        *len = sink.len;
        if (mime) {
            /* sink.mime is <= MN_TAGS_MIME_CAP, copy safely. */
            size_t m = strlen(sink.mime);
            if (m >= MN_TAGS_MIME_CAP) m = MN_TAGS_MIME_CAP - 1;
            memcpy(mime, sink.mime, m);
            mime[m] = '\0';
        }
        return true;
    }

    if (sink.bytes) free(sink.bytes);
    return false;
}

void mn_tags_free_cover(uint8_t *bytes) {
    free(bytes);
}
