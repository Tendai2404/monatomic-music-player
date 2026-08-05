/*
 * tags_write.c — Monatomic Audio Player
 *
 * Implementation of the metadata writer declared in tags_write.h. Mirrors
 * the reader's (tags.c) hand-rolled container parsing conventions:
 * dependency-light (C stdlib + Win32 for wide paths / atomic replace),
 * whole-file-in-memory parsing, bounds-checked walks.
 *
 * Rewrite strategy (MP3/FLAC): parse the existing metadata, rebuild the
 * metadata section (preserving everything we don't manage), stream a NEW
 * temp file (new metadata + audio bytes verbatim) and atomically replace
 * the original (MoveFileEx MOVEFILE_REPLACE_EXISTING / rename). The audio
 * payload is never re-encoded or shifted relative to itself.
 *
 * M4A: SAFE in-place subset — the new ilst (+ 'free' filler) must fit the
 * byte span of the old ilst + adjacent free atom, so no chunk-offset
 * (stco/co64) fixups are ever needed. Otherwise "m4a-needs-repack".
 */

#include "tags_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ==========================================================================
 * Small helpers (byte IO, growable buffer, wide-path file ops)
 * ======================================================================== */

static void tw_set_err(char *err, size_t errn, const char *msg)
{
    if (err && errn) {
        snprintf(err, errn, "%s", msg ? msg : "");
    }
}

static uint32_t tw_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t tw_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t tw_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Growable byte buffer. */
typedef struct {
    uint8_t *d;
    size_t   len;
    size_t   cap;
    bool     oom;
} tw_buf;

static void tw_buf_init(tw_buf *b)
{
    b->d = NULL; b->len = 0; b->cap = 0; b->oom = false;
}

static void tw_buf_free(tw_buf *b)
{
    free(b->d);
    b->d = NULL; b->len = 0; b->cap = 0;
}

static bool tw_buf_reserve(tw_buf *b, size_t extra)
{
    size_t need;
    if (b->oom) return false;
    need = b->len + extra;
    if (need < b->len) { b->oom = true; return false; } /* overflow */
    if (need <= b->cap) return true;
    {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < need) {
            if (ncap > (SIZE_MAX / 2)) { b->oom = true; return false; }
            ncap *= 2;
        }
        {
            uint8_t *nd = (uint8_t *)realloc(b->d, ncap);
            if (!nd) { b->oom = true; return false; }
            b->d = nd; b->cap = ncap;
        }
    }
    return true;
}

static void tw_put(tw_buf *b, const void *p, size_t n)
{
    if (!n || !tw_buf_reserve(b, n)) return;
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

static void tw_put_byte(tw_buf *b, uint8_t v) { tw_put(b, &v, 1); }

static void tw_put_be16(tw_buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    tw_put(b, t, 2);
}
static void tw_put_be24(tw_buf *b, uint32_t v)
{
    uint8_t t[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    tw_put(b, t, 3);
}
static void tw_put_be32(tw_buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    tw_put(b, t, 4);
}
static void tw_put_le32(tw_buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    tw_put(b, t, 4);
}

static void tw_put_zeros(tw_buf *b, size_t n)
{
    if (!tw_buf_reserve(b, n)) return;
    memset(b->d + b->len, 0, n);
    b->len += n;
}

/* Open a file with a UTF-8 path (wide on Windows). */
static FILE *tw_fopen(const char *path, const char *mode)
{
    if (!path) return NULL;
#ifdef _WIN32
    {
        wchar_t wmode[8];
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        wchar_t *wpath;
        FILE *f;
        size_t mi;
        if (wlen <= 0) return NULL;
        wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (!wpath) return NULL;
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) <= 0) {
            free(wpath);
            return NULL;
        }
        for (mi = 0; mode[mi] && mi < 6; mi++) wmode[mi] = (wchar_t)mode[mi];
        wmode[mi] = 0;
        f = _wfopen(wpath, wmode);
        free(wpath);
        return f;
    }
#else
    return fopen(path, mode);
#endif
}

/* Read the whole file (same 512 MiB safety cap as tags.c). */
#define TW_MAX_FILE_BYTES ((size_t)512u * 1024u * 1024u)

static uint8_t *tw_read_whole_file(const char *path, size_t *out_len)
{
    FILE *f;
    long sz;
    size_t len, got;
    uint8_t *buf;

    if (out_len) *out_len = 0;
    f = tw_fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if ((size_t)sz > TW_MAX_FILE_BYTES) { fclose(f); return NULL; }
    len = (size_t)sz;
    buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) { fclose(f); return NULL; }
    got = len ? fread(buf, 1, len, f) : 0;
    fclose(f);
    if (got != len) { free(buf); return NULL; }
    if (out_len) *out_len = len;
    return buf;
}

/* Delete a file by UTF-8 path. */
static void tw_delete_file(const char *path)
{
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath;
    if (wlen <= 0) return;
    wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) > 0) {
        DeleteFileW(wpath);
    }
    free(wpath);
#else
    remove(path);
#endif
}

/*
 * Write `parts` into "<path>.mnwtmp" then atomically replace `path`.
 * Same directory => same volume => the replace is atomic.
 */
typedef struct {
    const uint8_t *p;
    size_t         n;
} tw_part;

static bool tw_replace_file(const char *path, const tw_part *parts, int nparts,
                            char *err, size_t errn)
{
    char tmp[1200];
    FILE *f;
    int i;

    if (snprintf(tmp, sizeof(tmp), "%s.mnwtmp", path) >= (int)sizeof(tmp)) {
        tw_set_err(err, errn, "io-error");
        return false;
    }
    f = tw_fopen(tmp, "wb");
    if (!f) {
        tw_set_err(err, errn, "io-error");
        return false;
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].n &&
            fwrite(parts[i].p, 1, parts[i].n, f) != parts[i].n) {
            fclose(f);
            tw_delete_file(tmp);
            tw_set_err(err, errn, "io-error");
            return false;
        }
    }
    if (fclose(f) != 0) {
        tw_delete_file(tmp);
        tw_set_err(err, errn, "io-error");
        return false;
    }

#ifdef _WIN32
    {
        int wl1 = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0);
        int wl2 = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        wchar_t *w1, *w2;
        BOOL ok = FALSE;
        if (wl1 <= 0 || wl2 <= 0) { tw_delete_file(tmp); tw_set_err(err, errn, "io-error"); return false; }
        w1 = (wchar_t *)malloc((size_t)wl1 * sizeof(wchar_t));
        w2 = (wchar_t *)malloc((size_t)wl2 * sizeof(wchar_t));
        if (w1 && w2 &&
            MultiByteToWideChar(CP_UTF8, 0, tmp, -1, w1, wl1) > 0 &&
            MultiByteToWideChar(CP_UTF8, 0, path, -1, w2, wl2) > 0) {
            ok = MoveFileExW(w1, w2,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        free(w1);
        free(w2);
        if (!ok) {
            tw_delete_file(tmp);
            tw_set_err(err, errn, "replace-failed");
            return false;
        }
    }
#else
    if (rename(tmp, path) != 0) {
        tw_delete_file(tmp);
        tw_set_err(err, errn, "replace-failed");
        return false;
    }
#endif
    return true;
}

/* ==========================================================================
 * Text encoding helpers (UTF-8 <-> UTF-16LE, latin1 -> UTF-8)
 * ======================================================================== */

/* Decode one UTF-8 code point at s[0..n); returns bytes consumed (>=1). */
static size_t tw_utf8_cp(const char *s, size_t n, uint32_t *out_cp)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *out_cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && n >= 2 && ((unsigned char)s[1] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && n >= 3 &&
        ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c & 0x0F) << 12) |
                  (((uint32_t)(unsigned char)s[1] & 0x3F) << 6) |
                  ((uint32_t)(unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && n >= 4 &&
        ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80 &&
        ((unsigned char)s[3] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c & 0x07) << 18) |
                  (((uint32_t)(unsigned char)s[1] & 0x3F) << 12) |
                  (((uint32_t)(unsigned char)s[2] & 0x3F) << 6) |
                  ((uint32_t)(unsigned char)s[3] & 0x3F);
        return 4;
    }
    *out_cp = 0xFFFD; /* replacement */
    return 1;
}

/* Append a UTF-8 string as UTF-16LE code units (no BOM, no terminator). */
static void tw_put_utf16le(tw_buf *b, const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t used = tw_utf8_cp(utf8 + i, n - i, &cp);
        i += used;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xD800 | (v >> 10));
            uint16_t lo = (uint16_t)(0xDC00 | (v & 0x3FF));
            uint8_t t[4] = { (uint8_t)hi, (uint8_t)(hi >> 8),
                             (uint8_t)lo, (uint8_t)(lo >> 8) };
            tw_put(b, t, 4);
        } else {
            uint16_t u = (cp <= 0xFFFF) ? (uint16_t)cp : 0xFFFDu;
            uint8_t t[2] = { (uint8_t)u, (uint8_t)(u >> 8) };
            tw_put(b, t, 2);
        }
    }
}

/* Append one Unicode code point as UTF-8 to a tw_buf. */
static void tw_put_utf8_cp(tw_buf *b, uint32_t u)
{
    uint8_t t[4];
    if (u < 0x80) { t[0] = (uint8_t)u; tw_put(b, t, 1); }
    else if (u < 0x800) {
        t[0] = (uint8_t)(0xC0 | (u >> 6));
        t[1] = (uint8_t)(0x80 | (u & 0x3F));
        tw_put(b, t, 2);
    } else if (u < 0x10000) {
        t[0] = (uint8_t)(0xE0 | (u >> 12));
        t[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
        t[2] = (uint8_t)(0x80 | (u & 0x3F));
        tw_put(b, t, 3);
    } else {
        t[0] = (uint8_t)(0xF0 | (u >> 18));
        t[1] = (uint8_t)(0x80 | ((u >> 12) & 0x3F));
        t[2] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
        t[3] = (uint8_t)(0x80 | (u & 0x3F));
        tw_put(b, t, 4);
    }
}

/*
 * Decode an ID3 text payload (enc byte semantics: 0 latin1, 1 UTF-16 BOM,
 * 2 UTF-16BE, 3 UTF-8) of n bytes into UTF-8 appended to `out`. Stops at
 * an embedded terminator only when `stop_at_nul` is set (lyrics keep
 * everything up to the first terminator anyway per spec).
 */
static void tw_id3_text_to_utf8(tw_buf *out, uint8_t enc,
                                const uint8_t *p, size_t n)
{
    if (enc == 0x03) { /* UTF-8 */
        size_t m = n;
        while (m > 0 && p[m - 1] == 0) m--;
        tw_put(out, p, m);
        return;
    }
    if (enc == 0x00) { /* latin1 */
        size_t i;
        for (i = 0; i < n; i++) {
            if (p[i] == 0) break;
            tw_put_utf8_cp(out, p[i]);
        }
        return;
    }
    /* UTF-16 */
    {
        bool be = (enc == 0x02);
        size_t i = 0;
        if (enc == 0x01 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = be ? (uint32_t)((p[i] << 8) | p[i + 1])
                            : (uint32_t)((p[i + 1] << 8) | p[i]);
            if (u == 0) break;
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                uint32_t lo = be ? (uint32_t)((p[i + 2] << 8) | p[i + 3])
                                 : (uint32_t)((p[i + 3] << 8) | p[i + 2]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            tw_put_utf8_cp(out, u);
        }
    }
}

/* ==========================================================================
 * Format detection
 * ======================================================================== */

typedef enum {
    TW_FMT_MP3 = 0,
    TW_FMT_FLAC,
    TW_FMT_M4A,
    TW_FMT_OTHER
} tw_fmt;

static uint32_t tw_synchsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/* Total byte size of a leading ID3v2 tag (0 if none). */
static size_t tw_id3v2_total(const uint8_t *buf, size_t len)
{
    size_t total;
    if (len < 10 || memcmp(buf, "ID3", 3) != 0) return 0;
    total = 10 + (size_t)tw_synchsafe(buf + 6);
    if (buf[5] & 0x10) total += 10; /* footer present (v2.4) */
    if (total > len) total = len;
    return total;
}

static bool tw_ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    dot++;
    while (*dot && *ext) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext)) return false;
        dot++; ext++;
    }
    return *dot == 0 && *ext == 0;
}

/* Detect the container. For FLAC files carrying a nonstandard leading
 * ID3v2 tag, *flac_off receives the "fLaC" offset (else 0). */
static tw_fmt tw_detect(const uint8_t *buf, size_t len, const char *path,
                        size_t *flac_off)
{
    if (flac_off) *flac_off = 0;
    if (len >= 4 && memcmp(buf, "fLaC", 4) == 0) return TW_FMT_FLAC;
    if (len >= 12 && memcmp(buf + 4, "ftyp", 4) == 0) return TW_FMT_M4A;
    if (len >= 10 && memcmp(buf, "ID3", 3) == 0) {
        size_t t = tw_id3v2_total(buf, len);
        if (t + 4 <= len && memcmp(buf + t, "fLaC", 4) == 0) {
            if (flac_off) *flac_off = t;
            return TW_FMT_FLAC;
        }
        return TW_FMT_MP3;
    }
    if (tw_ext_is(path, "mp3")) return TW_FMT_MP3;
    if (tw_ext_is(path, "flac")) return TW_FMT_FLAC;
    if (tw_ext_is(path, "m4a") || tw_ext_is(path, "m4b") ||
        tw_ext_is(path, "mp4")) return TW_FMT_M4A;
    return TW_FMT_OTHER;
}

/* ==========================================================================
 * ID3v2.3 writer
 * ======================================================================== */

/* Append a v2.3 text frame (UTF-16LE with BOM). Skipped when value empty. */
static void id3w_text_frame(tw_buf *tag, const char *id, const char *utf8)
{
    tw_buf body;
    if (!utf8 || !utf8[0]) return;
    tw_buf_init(&body);
    tw_put_byte(&body, 0x01);           /* encoding: UTF-16 with BOM */
    tw_put_byte(&body, 0xFF);
    tw_put_byte(&body, 0xFE);
    tw_put_utf16le(&body, utf8);
    if (!body.oom && body.len <= 0x00FFFFFFu) {
        tw_put(tag, id, 4);
        tw_put_be32(tag, (uint32_t)body.len);
        tw_put_be16(tag, 0);            /* flags */
        tw_put(tag, body.d, body.len);
    }
    tw_buf_free(&body);
}

/* Append a COMM/USLT frame: enc(1) lang(3)="eng" desc(term) text. */
static void id3w_lang_text_frame(tw_buf *tag, const char *id, const char *utf8)
{
    tw_buf body;
    if (!utf8 || !utf8[0]) return;
    tw_buf_init(&body);
    tw_put_byte(&body, 0x01);
    tw_put(&body, "eng", 3);
    /* empty description: BOM + UTF-16 terminator */
    tw_put_byte(&body, 0xFF); tw_put_byte(&body, 0xFE);
    tw_put_byte(&body, 0x00); tw_put_byte(&body, 0x00);
    tw_put_byte(&body, 0xFF); tw_put_byte(&body, 0xFE);
    tw_put_utf16le(&body, utf8);
    if (!body.oom && body.len <= 0x00FFFFFFu) {
        tw_put(tag, id, 4);
        tw_put_be32(tag, (uint32_t)body.len);
        tw_put_be16(tag, 0);
        tw_put(tag, body.d, body.len);
    }
    tw_buf_free(&body);
}

/* Append an APIC frame: enc(0) mime NUL type(3) desc NUL data. */
static void id3w_apic_frame(tw_buf *tag, const uint8_t *img, size_t len,
                            const char *mime)
{
    size_t mlen = mime ? strlen(mime) : 0;
    size_t body_len = 1 + mlen + 1 + 1 + 1 + len;
    if (!img || !len || body_len > 0x00FFFFFFu) return;
    tw_put(tag, "APIC", 4);
    tw_put_be32(tag, (uint32_t)body_len);
    tw_put_be16(tag, 0);
    tw_put_byte(tag, 0x00);             /* latin1 text encoding */
    if (mlen) tw_put(tag, mime, mlen);
    tw_put_byte(tag, 0x00);             /* mime terminator */
    tw_put_byte(tag, 0x03);             /* picture type: front cover */
    tw_put_byte(tag, 0x00);             /* empty description */
    tw_put(tag, img, len);
}

/* Is `id` (4 chars) in the managed list? */
static bool id3w_managed(const char id[4], const char *const *managed, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (memcmp(id, managed[i], 4) == 0) return true;
    }
    return false;
}

/* True when a COMM frame body carries an EMPTY description (the generic
 * comment we manage). Non-empty descriptions (iTunNORM, replaygain tools,
 * ...) are app-specific data we must preserve. */
static bool id3w_comm_desc_empty(const uint8_t *body, size_t n)
{
    uint8_t enc;
    if (n < 5) return true;
    enc = body[0];
    body += 4; n -= 4;                       /* skip enc + lang */
    if (enc == 0x01 || enc == 0x02) {
        if (n >= 2 && ((body[0] == 0xFF && body[1] == 0xFE) ||
                       (body[0] == 0xFE && body[1] == 0xFF))) {
            body += 2; n -= 2;
        }
        return n < 2 || (body[0] == 0 && body[1] == 0);
    }
    return n < 1 || body[0] == 0;
}

/*
 * Rewrite the file's ID3v2 tag: preserved (unmanaged) frames + the frames
 * already serialized into `new_frames`. Managed ids in the source tag are
 * dropped ("COMM" drops only empty-description comments).
 */
static bool id3_rewrite(const char *path, const uint8_t *buf, size_t len,
                        const tw_buf *new_frames,
                        const char *const *managed, size_t nmanaged,
                        char *err, size_t errn)
{
    tw_buf tag;      /* full new ID3v2.3 tag incl. header + padding */
    tw_buf keep;     /* preserved frames (v2.3-encoded)             */
    size_t audio_start = 0;
    bool ok;

    tw_buf_init(&tag);
    tw_buf_init(&keep);

    if (len >= 10 && memcmp(buf, "ID3", 3) == 0) {
        uint8_t major = buf[3];
        uint8_t flags = buf[5];
        size_t total = tw_id3v2_total(buf, len);
        audio_start = total;

        if ((major == 3 || major == 4) && !(flags & 0x80)) {
            /* Walk frames; preserve everything unmanaged. */
            const uint8_t *p = buf + 10;
            const uint8_t *end = buf + ((total <= len) ? total : len);
            if (flags & 0x10 && end - buf >= 10) end -= 10; /* footer */
            /* extended header */
            if (flags & 0x40 && p + 4 <= end) {
                uint32_t exlen = (major == 4) ? tw_synchsafe(p) : tw_be32(p);
                const uint8_t *np = p + ((major == 4) ? exlen : exlen + 4);
                if (np > p && np <= end) p = np;
            }
            while (p + 10 <= end) {
                char id[4];
                uint32_t fsize;
                const uint8_t *fp;
                memcpy(id, p, 4);
                if (id[0] == 0) break; /* padding */
                fsize = (major == 4) ? tw_synchsafe(p + 4) : tw_be32(p + 4);
                fp = p + 10;
                if (fp + fsize > end || fsize == 0) break;

                {
                    bool drop = false;
                    if (id3w_managed(id, managed, nmanaged)) {
                        if (memcmp(id, "COMM", 4) == 0) {
                            drop = id3w_comm_desc_empty(fp, fsize);
                        } else {
                            drop = true;
                        }
                    }
                    /* v2.4 frames with format flags (compressed/encrypted/
                     * unsync/DLI/grouped) can't be copied verbatim into a
                     * v2.3 tag — drop them (rare). */
                    if (!drop && major == 4 && (p[9] & 0x4F) != 0) {
                        drop = true;
                    }
                    if (!drop) {
                        if (major == 3) {
                            tw_put(&keep, p, 10 + (size_t)fsize); /* verbatim */
                        } else {
                            tw_put(&keep, id, 4);
                            tw_put_be32(&keep, fsize);
                            tw_put_be16(&keep, 0);
                            tw_put(&keep, fp, fsize);
                        }
                    }
                }
                p = fp + fsize;
            }
        }
        /* v2.2 / unsync / unknown version: preserve nothing (spec fallback:
         * write only our frames + keep the audio). */
    }

    /* Assemble the new tag: header + managed + preserved + padding. */
    {
        size_t frames_len = new_frames->len + keep.len;
        size_t padding = 1024;
        size_t body = frames_len + padding;
        if (body >= (1u << 28)) {
            tw_buf_free(&tag); tw_buf_free(&keep);
            tw_set_err(err, errn, "corrupt");
            return false;
        }
        tw_put(&tag, "ID3", 3);
        tw_put_byte(&tag, 0x03);  /* v2.3.0 */
        tw_put_byte(&tag, 0x00);
        tw_put_byte(&tag, 0x00);  /* flags */
        tw_put_byte(&tag, (uint8_t)((body >> 21) & 0x7F));
        tw_put_byte(&tag, (uint8_t)((body >> 14) & 0x7F));
        tw_put_byte(&tag, (uint8_t)((body >> 7) & 0x7F));
        tw_put_byte(&tag, (uint8_t)(body & 0x7F));
        tw_put(&tag, new_frames->d, new_frames->len);
        tw_put(&tag, keep.d, keep.len);
        tw_put_zeros(&tag, padding);
    }

    if (tag.oom || keep.oom) {
        tw_buf_free(&tag); tw_buf_free(&keep);
        tw_set_err(err, errn, "io-error");
        return false;
    }

    {
        tw_part parts[2];
        parts[0].p = tag.d;             parts[0].n = tag.len;
        parts[1].p = buf + audio_start; parts[1].n = len - audio_start;
        ok = tw_replace_file(path, parts, 2, err, errn);
    }
    tw_buf_free(&tag);
    tw_buf_free(&keep);
    return ok;
}

/* Serialize the managed frame set for a tag edit. */
static void id3w_build_edit_frames(tw_buf *frames, const mn_tag_edit *e)
{
    char num[16];
    id3w_text_frame(frames, "TIT2", e->title);
    id3w_text_frame(frames, "TPE1", e->artist);
    id3w_text_frame(frames, "TALB", e->album);
    id3w_text_frame(frames, "TPE2", e->album_artist);
    id3w_text_frame(frames, "TCON", e->genre);
    if (e->composer[0]) {
        id3w_text_frame(frames, "TCOM", e->composer);
    }
    if (e->track_no > 0) {
        snprintf(num, sizeof(num), "%d", e->track_no);
        id3w_text_frame(frames, "TRCK", num);
    }
    if (e->year > 0) {
        snprintf(num, sizeof(num), "%04d", e->year);
        id3w_text_frame(frames, "TYER", num);
        id3w_text_frame(frames, "TDRC", num);
    }
    id3w_lang_text_frame(frames, "COMM", e->comment);
}

/* ==========================================================================
 * FLAC writer
 * ======================================================================== */

/* Case-insensitive "does this vorbis comment entry start with one of the
 * managed KEY= prefixes?". `entry` is not NUL-terminated. */
static bool vc_managed(const char *entry, size_t elen,
                       const char *const *managed, size_t nmanaged)
{
    size_t k;
    size_t eq = 0;
    while (eq < elen && entry[eq] != '=') eq++;
    for (k = 0; k < nmanaged; k++) {
        size_t mlen = strlen(managed[k]);
        if (eq == mlen) {
            size_t i;
            bool same = true;
            for (i = 0; i < mlen; i++) {
                if (toupper((unsigned char)entry[i]) !=
                    toupper((unsigned char)managed[k][i])) { same = false; break; }
            }
            if (same) return true;
        }
    }
    return false;
}

static void vc_add(tw_buf *cbuf, uint32_t *count, const char *key, const char *val)
{
    size_t klen, vlen;
    if (!val || !val[0]) return;
    klen = strlen(key);
    vlen = strlen(val);
    tw_put_le32(cbuf, (uint32_t)(klen + 1 + vlen));
    tw_put(cbuf, key, klen);
    tw_put_byte(cbuf, '=');
    tw_put(cbuf, val, vlen);
    (*count)++;
}

/*
 * Build a new VORBIS_COMMENT block body: preserved vendor string,
 * unmanaged comments verbatim, then the `add` callback's new comments.
 * `old` may be NULL (no existing block).
 */
typedef void (*vc_add_fn)(tw_buf *cbuf, uint32_t *count, void *user);

static void vc_rebuild(tw_buf *out, const uint8_t *old, size_t oldn,
                       const char *const *managed, size_t nmanaged,
                       vc_add_fn add, void *user)
{
    tw_buf cbuf;
    uint32_t count = 0;
    const char *vendor = "Monatomic";
    size_t vlen = strlen(vendor);
    const uint8_t *vptr = (const uint8_t *)vendor;

    tw_buf_init(&cbuf);

    if (old && oldn >= 8) {
        size_t i = 0;
        uint32_t v = tw_le32(old); i = 4;
        if (i + v + 4 <= oldn) {
            vptr = old + i;
            vlen = v;
            i += v;
            {
                uint32_t n = tw_le32(old + i); i += 4;
                uint32_t c;
                for (c = 0; c < n && i + 4 <= oldn; c++) {
                    uint32_t clen = tw_le32(old + i); i += 4;
                    if (i + clen > oldn) break;
                    if (!vc_managed((const char *)(old + i), clen,
                                    managed, nmanaged)) {
                        tw_put_le32(&cbuf, clen);
                        tw_put(&cbuf, old + i, clen);
                        count++;
                    }
                    i += clen;
                }
            }
        }
    }

    if (add) add(&cbuf, &count, user);

    tw_put_le32(out, (uint32_t)vlen);
    tw_put(out, vptr, vlen);
    tw_put_le32(out, count);
    tw_put(out, cbuf.d, cbuf.len);
    if (cbuf.oom) out->oom = true;
    tw_buf_free(&cbuf);
}

/* Build a PICTURE block body (type 3 front cover). */
static void flac_picture_body(tw_buf *out, const uint8_t *img, size_t len,
                              const char *mime)
{
    size_t mlen = mime ? strlen(mime) : 0;
    tw_put_be32(out, 3);                 /* picture type: front cover */
    tw_put_be32(out, (uint32_t)mlen);
    if (mlen) tw_put(out, mime, mlen);
    tw_put_be32(out, 0);                 /* description length */
    tw_put_be32(out, 0);                 /* width   (unknown) */
    tw_put_be32(out, 0);                 /* height  (unknown) */
    tw_put_be32(out, 0);                 /* depth   (unknown) */
    tw_put_be32(out, 0);                 /* colors  (unknown) */
    tw_put_be32(out, (uint32_t)len);
    tw_put(out, img, len);
}

/* Picture type of a PICTURE block body (front cover == 3). */
static uint32_t flac_picture_type(const uint8_t *body, size_t n)
{
    return (n >= 4) ? tw_be32(body) : 0;
}

typedef struct {
    int op;                      /* 0 = tags, 1 = art, 2 = lyrics */
    const mn_tag_edit *edit;
    const uint8_t *img;
    size_t img_len;
    const char *mime;
    const char *lyrics;
} flac_job;

static void flac_add_comments(tw_buf *cbuf, uint32_t *count, void *user)
{
    const flac_job *job = (const flac_job *)user;
    if (job->op == 0) {
        char num[16];
        const mn_tag_edit *e = job->edit;
        vc_add(cbuf, count, "TITLE", e->title);
        vc_add(cbuf, count, "ARTIST", e->artist);
        vc_add(cbuf, count, "ALBUM", e->album);
        vc_add(cbuf, count, "ALBUMARTIST", e->album_artist);
        vc_add(cbuf, count, "GENRE", e->genre);
        if (e->composer[0]) vc_add(cbuf, count, "COMPOSER", e->composer);
        if (e->year > 0) {
            snprintf(num, sizeof(num), "%04d", e->year);
            vc_add(cbuf, count, "DATE", num);
        }
        if (e->track_no > 0) {
            snprintf(num, sizeof(num), "%d", e->track_no);
            vc_add(cbuf, count, "TRACKNUMBER", num);
        }
        vc_add(cbuf, count, "COMMENT", e->comment);
    } else if (job->op == 2) {
        vc_add(cbuf, count, "LYRICS", job->lyrics);
    }
}

/*
 * Rewrite the FLAC metadata block chain. `flac_off` is the offset of
 * "fLaC" (nonzero when a nonstandard leading ID3v2 blob is preserved).
 */
static bool flac_rewrite(const char *path, const uint8_t *buf, size_t len,
                         size_t flac_off, const flac_job *job,
                         char *err, size_t errn)
{
    static const char *lyr_keys[] = { "LYRICS", "UNSYNCEDLYRICS", "UNSYNCED LYRICS" };

    /* dynamic managed set (see mn_tagw_write_tags: keep_missing makes an
     * empty field PRESERVE the file's existing comment instead of remove) */
    const char *tag_keys[14];
    const char *const *managed = NULL;
    size_t nmanaged = 0;

    tw_buf meta;                 /* new metadata section incl. "fLaC" */
    size_t audio_start = 0;
    const uint8_t *old_vc = NULL;
    size_t old_vc_len = 0;
    bool ok;

    if (flac_off + 8 > len || memcmp(buf + flac_off, "fLaC", 4) != 0) {
        tw_set_err(err, errn, "corrupt");
        return false;
    }

    if (job->op == 0) {
        const mn_tag_edit *e = job->edit;
        bool km = e->keep_missing;
        if (!km || e->title[0])        tag_keys[nmanaged++] = "TITLE";
        if (!km || e->artist[0])       tag_keys[nmanaged++] = "ARTIST";
        if (!km || e->album[0])        tag_keys[nmanaged++] = "ALBUM";
        if (!km || e->album_artist[0]) {
            tag_keys[nmanaged++] = "ALBUMARTIST";
            tag_keys[nmanaged++] = "ALBUM ARTIST";
            tag_keys[nmanaged++] = "ALBUM_ARTIST";
        }
        if (!km || e->genre[0])        tag_keys[nmanaged++] = "GENRE";
        if (!km || e->year > 0) {
            tag_keys[nmanaged++] = "DATE";
            tag_keys[nmanaged++] = "YEAR";
        }
        if (!km || e->track_no > 0) {
            tag_keys[nmanaged++] = "TRACKNUMBER";
            tag_keys[nmanaged++] = "TRACK";
        }
        if (!km || e->comment[0])      tag_keys[nmanaged++] = "COMMENT";
        if (e->composer[0])            tag_keys[nmanaged++] = "COMPOSER";
        managed = tag_keys;
    } else if (job->op == 2) {
        managed = lyr_keys;
        nmanaged = sizeof(lyr_keys) / sizeof(lyr_keys[0]);
    }

    tw_buf_init(&meta);
    tw_put(&meta, buf + flac_off, 4); /* "fLaC" */

    /* Pass 1: locate the existing VORBIS_COMMENT + the audio start. */
    {
        size_t i = flac_off + 4;
        bool last = false;
        while (!last && i + 4 <= len) {
            uint8_t hdr = buf[i];
            uint8_t type = hdr & 0x7F;
            uint32_t blen = ((uint32_t)buf[i + 1] << 16) |
                            ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
            last = (hdr & 0x80) != 0;
            i += 4;
            if (i + blen > len) { tw_buf_free(&meta); tw_set_err(err, errn, "corrupt"); return false; }
            if (type == 4) { old_vc = buf + i; old_vc_len = blen; }
            i += blen;
        }
        audio_start = i;
        if (audio_start >= len) { tw_buf_free(&meta); tw_set_err(err, errn, "corrupt"); return false; }
    }

    /* Pass 2: emit the new chain. Order: STREAMINFO first (required),
     * then preserved blocks, replaced VORBIS_COMMENT / PICTURE, and a
     * fresh PADDING block last. */
    {
        tw_buf vc;      /* new vorbis comment body */
        tw_buf pic;     /* new picture body (art op only) */
        size_t i = flac_off + 4;
        bool last = false;
        bool vc_emitted = false;

        tw_buf_init(&vc);
        tw_buf_init(&pic);
        if (job->op == 0 || job->op == 2 || old_vc) {
            vc_rebuild(&vc, old_vc, old_vc_len, managed, nmanaged,
                       flac_add_comments, (void *)job);
        }
        if (job->op == 1) {
            flac_picture_body(&pic, job->img, job->img_len, job->mime);
        }

        while (!last && i + 4 <= len) {
            uint8_t hdr = buf[i];
            uint8_t type = hdr & 0x7F;
            uint32_t blen = ((uint32_t)buf[i + 1] << 16) |
                            ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
            const uint8_t *body = buf + i + 4;
            last = (hdr & 0x80) != 0;
            i += 4 + blen;

            if (type == 1) {
                continue;                       /* drop PADDING (re-added) */
            }
            if (type == 4) {
                if (vc.len > 0 && vc.len <= 0x00FFFFFFu) {
                    tw_put_byte(&meta, 4);
                    tw_put_be24(&meta, (uint32_t)vc.len);
                    tw_put(&meta, vc.d, vc.len);
                    vc_emitted = true;
                }
                continue;
            }
            if (type == 6 && job->op == 1 &&
                flac_picture_type(body, blen) == 3) {
                continue;                       /* replaced front cover */
            }
            /* STREAMINFO + everything else: verbatim. */
            tw_put_byte(&meta, type);           /* is-last cleared */
            tw_put_be24(&meta, blen);
            tw_put(&meta, body, blen);
        }

        /* VORBIS_COMMENT did not exist: append the fresh one. */
        if (!vc_emitted && vc.len > 0 && vc.len <= 0x00FFFFFFu &&
            (job->op == 0 || job->op == 2)) {
            tw_put_byte(&meta, 4);
            tw_put_be24(&meta, (uint32_t)vc.len);
            tw_put(&meta, vc.d, vc.len);
        }

        /* New front cover. */
        if (job->op == 1) {
            if (pic.len > 0x00FFFFFFu) {
                tw_buf_free(&vc); tw_buf_free(&pic); tw_buf_free(&meta);
                tw_set_err(err, errn, "io-error"); /* image too large */
                return false;
            }
            tw_put_byte(&meta, 6);
            tw_put_be24(&meta, (uint32_t)pic.len);
            tw_put(&meta, pic.d, pic.len);
        }

        /* Fresh PADDING, flagged as the last metadata block. */
        tw_put_byte(&meta, 0x80 | 1);
        tw_put_be24(&meta, 4096);
        tw_put_zeros(&meta, 4096);

        tw_buf_free(&vc);
        tw_buf_free(&pic);
    }

    if (meta.oom) {
        tw_buf_free(&meta);
        tw_set_err(err, errn, "io-error");
        return false;
    }

    {
        tw_part parts[3];
        parts[0].p = buf;               parts[0].n = flac_off;       /* id3 blob */
        parts[1].p = meta.d;            parts[1].n = meta.len;
        parts[2].p = buf + audio_start; parts[2].n = len - audio_start;
        ok = tw_replace_file(path, parts, 3, err, errn);
    }
    tw_buf_free(&meta);
    return ok;
}

/* ==========================================================================
 * M4A / MP4 safe in-place ilst patcher
 * ======================================================================== */

typedef struct {
    size_t off;    /* absolute offset of the atom header */
    size_t size;   /* full atom size including header    */
} mp4_span;

/* Find the first child atom named `type` in [off, end). Returns false when
 * absent or when an unparseable (64-bit / zero) size is encountered. */
static bool mp4_find_child(const uint8_t *buf, size_t len, size_t off,
                           size_t end, const char *type, mp4_span *out)
{
    if (end > len) end = len;
    while (off + 8 <= end) {
        uint32_t sz = tw_be32(buf + off);
        if (sz == 1 || sz == 0) return false;   /* 64-bit / to-end: bail */
        if (sz < 8 || off + sz > end) return false;
        if (memcmp(buf + off + 4, type, 4) == 0) {
            out->off = off;
            out->size = sz;
            return true;
        }
        off += sz;
    }
    return false;
}

/* Append an iTunes text item: <item>[<data> flags=1 utf8]. */
static void mp4w_item_text(tw_buf *b, const char *name, const char *utf8)
{
    size_t vlen;
    if (!utf8 || !utf8[0]) return;
    vlen = strlen(utf8);
    tw_put_be32(b, (uint32_t)(8 + 16 + vlen));  /* item size */
    tw_put(b, name, 4);
    tw_put_be32(b, (uint32_t)(16 + vlen));      /* data atom size */
    tw_put(b, "data", 4);
    tw_put_be32(b, 1);                          /* type: UTF-8 text */
    tw_put_be32(b, 0);                          /* locale */
    tw_put(b, utf8, vlen);
}

/* Append a trkn item (binary pair). */
static void mp4w_item_trkn(tw_buf *b, int track, int total)
{
    if (track <= 0) return;
    tw_put_be32(b, 8 + 16 + 8);
    tw_put(b, "trkn", 4);
    tw_put_be32(b, 16 + 8);
    tw_put(b, "data", 4);
    tw_put_be32(b, 0);                          /* type: binary */
    tw_put_be32(b, 0);
    tw_put_be16(b, 0);
    tw_put_be16(b, (uint16_t)track);
    tw_put_be16(b, (uint16_t)(total > 0 ? total : 0));
    tw_put_be16(b, 0);
}

/* Append a covr item. */
static void mp4w_item_covr(tw_buf *b, const uint8_t *img, size_t len,
                           const char *mime)
{
    uint32_t flags = 13;                        /* JPEG */
    if (mime && strstr(mime, "png")) flags = 14;
    else if (len > 2 && img[0] == 0x89 && img[1] == 'P') flags = 14;
    tw_put_be32(b, (uint32_t)(8 + 16 + len));
    tw_put(b, "covr", 4);
    tw_put_be32(b, (uint32_t)(16 + len));
    tw_put(b, "data", 4);
    tw_put_be32(b, flags);
    tw_put_be32(b, 0);
    tw_put(b, img, len);
}

typedef struct {
    int op;                      /* 0 = tags, 1 = art, 2 = lyrics */
    const mn_tag_edit *edit;
    const uint8_t *img;
    size_t img_len;
    const char *mime;
    const char *lyrics;
} mp4_job;

static bool mp4w_managed_name(const uint8_t *name, const mp4_job *job)
{
    if (job->op == 0) {
        const mn_tag_edit *e = job->edit;
        bool km = e->keep_missing;   /* empty field => PRESERVE the atom */
        if (!memcmp(name, "\xA9""nam", 4)) return !km || e->title[0];
        if (!memcmp(name, "\xA9""ART", 4)) return !km || e->artist[0];
        if (!memcmp(name, "\xA9""alb", 4)) return !km || e->album[0];
        if (!memcmp(name, "aART", 4))      return !km || e->album_artist[0];
        if (!memcmp(name, "\xA9""gen", 4) ||
            !memcmp(name, "gnre", 4))      return !km || e->genre[0];
        if (!memcmp(name, "\xA9""day", 4)) return !km || e->year > 0;
        if (!memcmp(name, "trkn", 4))      return !km || e->track_no > 0;
        if (!memcmp(name, "\xA9""cmt", 4)) return !km || e->comment[0];
        if (!memcmp(name, "\xA9""wrt", 4) && e->composer[0]) return true;
        return false;
    }
    if (job->op == 1) return !memcmp(name, "covr", 4);
    return !memcmp(name, "\xA9""lyr", 4);       /* op == 2 */
}

static bool mp4_patch(const char *path, const uint8_t *buf, size_t len,
                      const mp4_job *job, char *err, size_t errn)
{
    mp4_span moov, udta, meta, ilst;
    size_t meta_kids, meta_end;
    size_t avail;             /* old ilst + adjacent free span (bytes) */
    tw_buf items;             /* new ilst children */
    int old_trkn_total = 0;
    bool ok;

    if (!mp4_find_child(buf, len, 0, len, "moov", &moov)) {
        tw_set_err(err, errn, "m4a-needs-repack");
        return false;
    }
    if (mp4_find_child(buf, len, moov.off + 8, moov.off + moov.size, "udta", &udta)) {
        if (!mp4_find_child(buf, len, udta.off + 8, udta.off + udta.size, "meta", &meta)) {
            tw_set_err(err, errn, "m4a-needs-repack");
            return false;
        }
    } else if (!mp4_find_child(buf, len, moov.off + 8, moov.off + moov.size, "meta", &meta)) {
        tw_set_err(err, errn, "m4a-needs-repack");
        return false;
    }
    if (meta.size < 12) {
        tw_set_err(err, errn, "m4a-needs-repack");
        return false;
    }
    meta_kids = meta.off + 12;   /* 8-byte header + 4-byte version/flags */
    meta_end = meta.off + meta.size;
    if (!mp4_find_child(buf, len, meta_kids, meta_end, "ilst", &ilst)) {
        tw_set_err(err, errn, "m4a-needs-repack");
        return false;
    }

    /* Adjacent free space directly after ilst inside meta. */
    avail = ilst.size;
    {
        size_t fo = ilst.off + ilst.size;
        while (fo + 8 <= meta_end) {
            uint32_t fsz = tw_be32(buf + fo);
            if (fsz < 8 || fo + fsz > meta_end) break;
            if (memcmp(buf + fo + 4, "free", 4) != 0 &&
                memcmp(buf + fo + 4, "skip", 4) != 0) break;
            avail += fsz;
            fo += fsz;
        }
    }

    /* Build the new ilst children: preserved unmanaged items + new items. */
    tw_buf_init(&items);
    {
        size_t i = ilst.off + 8;
        size_t end = ilst.off + ilst.size;
        while (i + 8 <= end) {
            uint32_t sz = tw_be32(buf + i);
            if (sz < 8 || i + sz > end) break;
            {
                const uint8_t *name = buf + i + 4;
                if (mp4w_managed_name(name, job)) {
                    /* Capture the old trkn total so a track-number edit
                     * keeps the "of N" part. */
                    if (!memcmp(name, "trkn", 4) && sz >= 8 + 16 + 6) {
                        const uint8_t *d = buf + i + 8;
                        if (sz >= 8 + 8 && !memcmp(d + 4, "data", 4)) {
                            uint32_t dsz = tw_be32(d);
                            if (dsz >= 16 + 8 && 8 + dsz <= sz) {
                                old_trkn_total = tw_be16(d + 16 + 4);
                            }
                        }
                    }
                } else {
                    tw_put(&items, buf + i, sz);
                }
            }
            i += sz;
        }
    }

    if (job->op == 0) {
        const mn_tag_edit *e = job->edit;
        char num[16];
        mp4w_item_text(&items, "\xA9""nam", e->title);
        mp4w_item_text(&items, "\xA9""ART", e->artist);
        mp4w_item_text(&items, "\xA9""alb", e->album);
        mp4w_item_text(&items, "aART", e->album_artist);
        mp4w_item_text(&items, "\xA9""gen", e->genre);
        if (e->composer[0]) mp4w_item_text(&items, "\xA9""wrt", e->composer);
        if (e->year > 0) {
            snprintf(num, sizeof(num), "%04d", e->year);
            mp4w_item_text(&items, "\xA9""day", num);
        }
        mp4w_item_trkn(&items, e->track_no, old_trkn_total);
        mp4w_item_text(&items, "\xA9""cmt", e->comment);
    } else if (job->op == 1) {
        mp4w_item_covr(&items, job->img, job->img_len, job->mime);
    } else {
        mp4w_item_text(&items, "\xA9""lyr", job->lyrics);
    }

    if (items.oom) {
        tw_buf_free(&items);
        tw_set_err(err, errn, "io-error");
        return false;
    }

    /* Fit check: the new ilst (+ optional free filler) must exactly fill
     * the old ilst + free span so no other byte in the file moves. */
    {
        size_t new_ilst = 8 + items.len;
        size_t remain;
        tw_buf patch;

        if (new_ilst > avail || (avail - new_ilst > 0 && avail - new_ilst < 8)) {
            tw_buf_free(&items);
            tw_set_err(err, errn, "m4a-needs-repack");
            return false;
        }
        remain = avail - new_ilst;

        tw_buf_init(&patch);
        tw_put_be32(&patch, (uint32_t)new_ilst);
        tw_put(&patch, "ilst", 4);
        tw_put(&patch, items.d, items.len);
        if (remain >= 8) {
            tw_put_be32(&patch, (uint32_t)remain);
            tw_put(&patch, "free", 4);
            tw_put_zeros(&patch, remain - 8);
        }
        tw_buf_free(&items);
        if (patch.oom || patch.len != avail) {
            tw_buf_free(&patch);
            tw_set_err(err, errn, "io-error");
            return false;
        }

        {
            tw_part parts[3];
            parts[0].p = buf;                    parts[0].n = ilst.off;
            parts[1].p = patch.d;                parts[1].n = patch.len;
            parts[2].p = buf + ilst.off + avail; parts[2].n = len - (ilst.off + avail);
            ok = tw_replace_file(path, parts, 3, err, errn);
        }
        tw_buf_free(&patch);
    }
    return ok;
}

/* ==========================================================================
 * Public writers
 * ======================================================================== */

bool mn_tagw_write_tags(const char *path, const mn_tag_edit *edit,
                        char *err, size_t errn)
{
    /* The managed list is built DYNAMICALLY: a frame is only "managed"
     * (original dropped, replaced by ours) when we are actually writing a
     * value for it — or when the edit is authoritative (keep_missing off),
     * where an empty value means REMOVE. With keep_missing, empty fields
     * leave the file's existing frames untouched. */
    const char *managed[12];
    size_t nmanaged = 0;
    uint8_t *buf;
    size_t len = 0, flac_off = 0;
    bool ok = false;
    bool km;

    tw_set_err(err, errn, "");
    if (!path || !edit) { tw_set_err(err, errn, "bad-args"); return false; }
    km = edit->keep_missing;
    if (!km || edit->title[0])        managed[nmanaged++] = "TIT2";
    if (!km || edit->artist[0])       managed[nmanaged++] = "TPE1";
    if (!km || edit->album[0])        managed[nmanaged++] = "TALB";
    if (!km || edit->album_artist[0]) managed[nmanaged++] = "TPE2";
    if (!km || edit->genre[0])        managed[nmanaged++] = "TCON";
    if (!km || edit->track_no > 0)    managed[nmanaged++] = "TRCK";
    if (!km || edit->year > 0) {
        managed[nmanaged++] = "TYER";
        managed[nmanaged++] = "TDRC";
        managed[nmanaged++] = "TDAT";
    }
    if (!km || edit->comment[0])      managed[nmanaged++] = "COMM";
    if (edit->composer[0])            managed[nmanaged++] = "TCOM";

    buf = tw_read_whole_file(path, &len);
    if (!buf) { tw_set_err(err, errn, "io-error"); return false; }

    switch (tw_detect(buf, len, path, &flac_off)) {
        case TW_FMT_MP3: {
            tw_buf frames;
            tw_buf_init(&frames);
            id3w_build_edit_frames(&frames, edit);
            ok = !frames.oom &&
                 id3_rewrite(path, buf, len, &frames, managed, nmanaged,
                             err, errn);
            tw_buf_free(&frames);
            break;
        }
        case TW_FMT_FLAC: {
            flac_job job;
            memset(&job, 0, sizeof(job));
            job.op = 0;
            job.edit = edit;
            ok = flac_rewrite(path, buf, len, flac_off, &job, err, errn);
            break;
        }
        case TW_FMT_M4A: {
            mp4_job job;
            memset(&job, 0, sizeof(job));
            job.op = 0;
            job.edit = edit;
            ok = mp4_patch(path, buf, len, &job, err, errn);
            break;
        }
        default:
            tw_set_err(err, errn, "unsupported-format");
            break;
    }
    free(buf);
    return ok;
}

bool mn_tagw_write_art(const char *path, const uint8_t *img, size_t len,
                       const char *mime, char *err, size_t errn)
{
    uint8_t *buf;
    size_t flen = 0, flac_off = 0;
    bool ok = false;

    tw_set_err(err, errn, "");
    if (!path || !img || !len) { tw_set_err(err, errn, "bad-args"); return false; }
    if (!mime || !mime[0]) mime = "image/jpeg";
    buf = tw_read_whole_file(path, &flen);
    if (!buf) { tw_set_err(err, errn, "io-error"); return false; }

    switch (tw_detect(buf, flen, path, &flac_off)) {
        case TW_FMT_MP3: {
            static const char *managed[] = { "APIC" };
            tw_buf frames;
            tw_buf_init(&frames);
            id3w_apic_frame(&frames, img, len, mime);
            ok = !frames.oom && frames.len > 0 &&
                 id3_rewrite(path, buf, flen, &frames, managed, 1, err, errn);
            if (frames.len == 0) tw_set_err(err, errn, "io-error");
            tw_buf_free(&frames);
            break;
        }
        case TW_FMT_FLAC: {
            flac_job job;
            memset(&job, 0, sizeof(job));
            job.op = 1;
            job.img = img;
            job.img_len = len;
            job.mime = mime;
            ok = flac_rewrite(path, buf, flen, flac_off, &job, err, errn);
            break;
        }
        case TW_FMT_M4A: {
            mp4_job job;
            memset(&job, 0, sizeof(job));
            job.op = 1;
            job.img = img;
            job.img_len = len;
            job.mime = mime;
            ok = mp4_patch(path, buf, flen, &job, err, errn);
            break;
        }
        default:
            tw_set_err(err, errn, "unsupported-format");
            break;
    }
    free(buf);
    return ok;
}

bool mn_tagw_write_lyrics(const char *path, const char *text,
                          char *err, size_t errn)
{
    uint8_t *buf;
    size_t len = 0, flac_off = 0;
    bool ok = false;

    tw_set_err(err, errn, "");
    if (!path) { tw_set_err(err, errn, "bad-args"); return false; }
    if (!text) text = "";
    buf = tw_read_whole_file(path, &len);
    if (!buf) { tw_set_err(err, errn, "io-error"); return false; }

    switch (tw_detect(buf, len, path, &flac_off)) {
        case TW_FMT_MP3: {
            static const char *managed[] = { "USLT" };
            tw_buf frames;
            tw_buf_init(&frames);
            id3w_lang_text_frame(&frames, "USLT", text);
            ok = !frames.oom &&
                 id3_rewrite(path, buf, len, &frames, managed, 1, err, errn);
            tw_buf_free(&frames);
            break;
        }
        case TW_FMT_FLAC: {
            flac_job job;
            memset(&job, 0, sizeof(job));
            job.op = 2;
            job.lyrics = text;
            ok = flac_rewrite(path, buf, len, flac_off, &job, err, errn);
            break;
        }
        case TW_FMT_M4A: {
            mp4_job job;
            memset(&job, 0, sizeof(job));
            job.op = 2;
            job.lyrics = text;
            ok = mp4_patch(path, buf, len, &job, err, errn);
            break;
        }
        default:
            tw_set_err(err, errn, "unsupported-format");
            break;
    }
    free(buf);
    return ok;
}

/* ==========================================================================
 * Sidecar (.lrc / .txt)
 * ======================================================================== */

/* "<audio path minus extension>" + new extension into out. */
static bool tw_sidecar_path(const char *audio_path, const char *ext,
                            char *out, size_t n)
{
    const char *dot = strrchr(audio_path, '.');
    const char *s1 = strrchr(audio_path, '/');
    const char *s2 = strrchr(audio_path, '\\');
    const char *sep = (s1 > s2) ? s1 : s2;
    size_t stem;
    if (!dot || (sep && dot < sep)) {
        stem = strlen(audio_path);
    } else {
        stem = (size_t)(dot - audio_path);
    }
    if (stem + strlen(ext) + 1 >= n) return false;
    memcpy(out, audio_path, stem);
    out[stem] = '\0';
    strcat(out, ext);
    return true;
}

/* Write (or, when empty, delete) a sidecar of the given extension. */
static bool tw_write_sidecar_ext(const char *audio_path, const char *ext,
                                 const char *content)
{
    char side[1200];
    FILE *f;
    size_t n;
    if (!audio_path || !tw_sidecar_path(audio_path, ext, side, sizeof(side))) {
        return false;
    }
    if (!content || !content[0]) {
        tw_delete_file(side);
        return true;
    }
    f = tw_fopen(side, "wb");
    if (!f) return false;
    n = strlen(content);
    if (fwrite(content, 1, n, f) != n) { fclose(f); return false; }
    fclose(f);
    return true;
}

bool mn_tagw_write_sidecar_lrc(const char *audio_path, const char *lrc_text)
{
    return tw_write_sidecar_ext(audio_path, ".lrc", lrc_text);
}

bool mn_tagw_write_sidecar_txt(const char *audio_path, const char *text)
{
    return tw_write_sidecar_ext(audio_path, ".txt", text);
}

/* ==========================================================================
 * Lyrics reading: embedded first, then sidecar
 * ======================================================================== */

/* Copy a tw_buf's text into a bounded caller buffer (UTF-8, NUL-safe). */
static void tw_copy_out(const tw_buf *b, char *out, size_t n)
{
    size_t m = b->len;
    if (n == 0) return;
    if (m >= n) {
        m = n - 1;
        /* back off mid-UTF-8 truncation */
        while (m > 0 && (b->d[m] & 0xC0) == 0x80) m--;
    }
    memcpy(out, b->d, m);
    out[m] = '\0';
}

/* Extract USLT text from a leading ID3v2 tag. */
static bool tw_read_lyrics_id3(const uint8_t *buf, size_t len,
                               char *out, size_t n)
{
    uint8_t major, flags;
    size_t total;
    const uint8_t *p, *end;

    if (len < 10 || memcmp(buf, "ID3", 3) != 0) return false;
    major = buf[3];
    flags = buf[5];
    total = tw_id3v2_total(buf, len);
    if (major < 3 || major > 4 || (flags & 0x80)) return false;

    p = buf + 10;
    end = buf + ((total <= len) ? total : len);
    if ((flags & 0x10) && end - buf >= 10) end -= 10;
    if ((flags & 0x40) && p + 4 <= end) {
        uint32_t exlen = (major == 4) ? tw_synchsafe(p) : tw_be32(p);
        const uint8_t *np = p + ((major == 4) ? exlen : exlen + 4);
        if (np > p && np <= end) p = np;
    }
    while (p + 10 <= end) {
        uint32_t fsize;
        const uint8_t *fp;
        if (p[0] == 0) break;
        fsize = (major == 4) ? tw_synchsafe(p + 4) : tw_be32(p + 4);
        fp = p + 10;
        if (fp + fsize > end || fsize == 0) break;
        if (memcmp(p, "USLT", 4) == 0 && fsize > 4) {
            uint8_t enc = fp[0];
            size_t i = 4;                    /* skip enc + lang */
            /* skip the description (encoding-dependent terminator) */
            if (enc == 0x01 || enc == 0x02) {
                while (i + 1 < fsize && !(fp[i] == 0 && fp[i + 1] == 0)) i += 2;
                i += 2;
            } else {
                while (i < fsize && fp[i] != 0) i++;
                i += 1;
            }
            if (i < fsize) {
                tw_buf b;
                tw_buf_init(&b);
                tw_id3_text_to_utf8(&b, enc, fp + i, fsize - i);
                tw_copy_out(&b, out, n);
                tw_buf_free(&b);
                return out[0] != '\0';
            }
            return false;
        }
        p = fp + fsize;
    }
    return false;
}

/* Extract LYRICS=/UNSYNCEDLYRICS= from FLAC vorbis comments. */
static bool tw_read_lyrics_flac(const uint8_t *buf, size_t len, size_t flac_off,
                                char *out, size_t n)
{
    size_t i;
    bool last = false;
    if (flac_off + 8 > len || memcmp(buf + flac_off, "fLaC", 4) != 0) return false;
    i = flac_off + 4;
    while (!last && i + 4 <= len) {
        uint8_t hdr = buf[i];
        uint8_t type = hdr & 0x7F;
        uint32_t blen = ((uint32_t)buf[i + 1] << 16) |
                        ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
        last = (hdr & 0x80) != 0;
        i += 4;
        if (i + blen > len) break;
        if (type == 4) {
            const uint8_t *p = buf + i;
            size_t j = 0;
            uint32_t v, cnt, c;
            if (blen < 8) return false;
            v = tw_le32(p); j = 4;
            if (j + v + 4 > blen) return false;
            j += v;
            cnt = tw_le32(p + j); j += 4;
            for (c = 0; c < cnt && j + 4 <= blen; c++) {
                uint32_t clen = tw_le32(p + j); j += 4;
                if (j + clen > blen) break;
                {
                    const char *e = (const char *)(p + j);
                    size_t klen = 0;
                    while (klen < clen && e[klen] != '=') klen++;
                    if (klen < clen) {
                        static const char *keys[] = {
                            "LYRICS", "UNSYNCEDLYRICS", "UNSYNCED LYRICS"
                        };
                        size_t k;
                        for (k = 0; k < 3; k++) {
                            size_t ml = strlen(keys[k]);
                            if (klen == ml) {
                                size_t q;
                                bool same = true;
                                for (q = 0; q < ml; q++) {
                                    if (toupper((unsigned char)e[q]) != keys[k][q]) {
                                        same = false; break;
                                    }
                                }
                                if (same && clen > klen + 1) {
                                    size_t vlen = clen - klen - 1;
                                    if (vlen >= n) vlen = n - 1;
                                    memcpy(out, e + klen + 1, vlen);
                                    out[vlen] = '\0';
                                    return out[0] != '\0';
                                }
                            }
                        }
                    }
                }
                j += clen;
            }
            return false;
        }
        i += blen;
    }
    return false;
}

/* Extract the ©lyr atom text from an M4A. */
static bool tw_read_lyrics_mp4(const uint8_t *buf, size_t len,
                               char *out, size_t n)
{
    mp4_span moov, udta, meta, ilst;
    size_t i, end;
    if (!mp4_find_child(buf, len, 0, len, "moov", &moov)) return false;
    if (mp4_find_child(buf, len, moov.off + 8, moov.off + moov.size, "udta", &udta)) {
        if (!mp4_find_child(buf, len, udta.off + 8, udta.off + udta.size, "meta", &meta))
            return false;
    } else if (!mp4_find_child(buf, len, moov.off + 8, moov.off + moov.size, "meta", &meta)) {
        return false;
    }
    if (meta.size < 12 ||
        !mp4_find_child(buf, len, meta.off + 12, meta.off + meta.size, "ilst", &ilst)) {
        return false;
    }
    i = ilst.off + 8;
    end = ilst.off + ilst.size;
    while (i + 8 <= end) {
        uint32_t sz = tw_be32(buf + i);
        if (sz < 8 || i + sz > end) break;
        if (!memcmp(buf + i + 4, "\xA9""lyr", 4)) {
            size_t j = i + 8;
            size_t iend = i + sz;
            while (j + 8 <= iend) {
                uint32_t dsz = tw_be32(buf + j);
                if (dsz < 8 || j + dsz > iend) break;
                if (!memcmp(buf + j + 4, "data", 4) && dsz > 16) {
                    size_t vlen = dsz - 16;
                    if (vlen >= n) vlen = n - 1;
                    memcpy(out, buf + j + 16, vlen);
                    out[vlen] = '\0';
                    return out[0] != '\0';
                }
                j += dsz;
            }
            return false;
        }
        i += sz;
    }
    return false;
}

/* Read a sidecar text file into out (strips a UTF-8 BOM). */
static bool tw_read_sidecar(const char *audio_path, const char *ext,
                            char *out, size_t n)
{
    char side[1200];
    FILE *f;
    size_t got;
    if (!tw_sidecar_path(audio_path, ext, side, sizeof(side))) return false;
    f = tw_fopen(side, "rb");
    if (!f) return false;
    got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = '\0';
    if (got >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        memmove(out, out + 3, got - 3 + 1);
    }
    return out[0] != '\0';
}

bool mn_tagw_read_lyrics(const char *path, char *out, size_t n)
{
    uint8_t *buf;
    size_t len = 0, flac_off = 0;
    bool found = false;

    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!path || !path[0]) return false;

    buf = tw_read_whole_file(path, &len);
    if (buf) {
        switch (tw_detect(buf, len, path, &flac_off)) {
            case TW_FMT_MP3:  found = tw_read_lyrics_id3(buf, len, out, n); break;
            case TW_FMT_FLAC: found = tw_read_lyrics_flac(buf, len, flac_off, out, n); break;
            case TW_FMT_M4A:  found = tw_read_lyrics_mp4(buf, len, out, n); break;
            default: break;
        }
        free(buf);
    }
    if (!found) found = tw_read_sidecar(path, ".lrc", out, n);
    if (!found) found = tw_read_sidecar(path, ".txt", out, n);
    return found;
}

/* ==========================================================================
 * Base64 decode (data: URI tolerant)
 * ======================================================================== */

uint8_t *mn_tagw_b64_decode(const char *src, size_t *out_len)
{
    static const char *al =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    signed char tbl[256];
    size_t i, n, o = 0;
    uint32_t acc = 0;
    int bits = 0;
    uint8_t *out;

    if (out_len) *out_len = 0;
    if (!src) return NULL;

    /* Skip a "data:<mime>;base64," prefix if present. */
    if (strncmp(src, "data:", 5) == 0) {
        const char *comma = strchr(src, ',');
        if (comma) src = comma + 1;
    }
    n = strlen(src);
    if (n == 0) return NULL;

    memset(tbl, -1, sizeof(tbl));
    for (i = 0; i < 64; i++) tbl[(unsigned char)al[i]] = (signed char)i;
    /* url-safe variants */
    tbl[(unsigned char)'-'] = 62;
    tbl[(unsigned char)'_'] = 63;

    out = (uint8_t *)malloc(n / 4 * 3 + 4);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        signed char v = tbl[(unsigned char)src[i]];
        if (v < 0) continue;    /* '=', whitespace, junk */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    if (o == 0) { free(out); return NULL; }
    if (out_len) *out_len = o;
    return out;
}

void mn_tagw_b64_free(uint8_t *p)
{
    free(p);
}
