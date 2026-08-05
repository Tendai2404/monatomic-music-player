/* artcache.c -- Album-art thumbnail cache for Monatomic.
 *
 * Implements artcache.h: extract embedded cover art (or a conventional sidecar
 * image) from an audio file, decode it, center-crop to a square, resize to a
 * fixed 256x256 RGBA thumbnail, and cache the result on disk as a PNG keyed by
 * an FNV-1a hash of the caller-supplied album_key.
 *
 * Heavy image dependencies are compiled directly into this translation unit:
 *   - stb_image.h            (decode JPEG/PNG/... to RGBA)
 *   - stb_image_resize2.h    (high-quality sRGB-aware resize)
 *   - stb_image_write.h      (write 8-bit RGBA PNG)
 * Cover-art extraction is delegated to tags.h (mn_tags_read_cover).
 *
 * No global mutable state; every function is reentrant. See artcache.h for the
 * full contract, including the concurrency notes for same-album races.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

/* Trim the decoder surface to the formats cover art actually ships in; this
 * also shrinks the object and avoids pulling in codecs we never feed. */
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include "artcache.h"
#include "tags.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>   /* _mkdir */
#  define COBJMACROS    /* C-style COM macros for the WIC decode fallback */
#  include <windows.h>  /* FindFirstFileW / _wfopen (Unicode-safe I/O) */
#  include <objbase.h>  /* CoInitializeEx / CoCreateInstance            */
#  include <wincodec.h> /* WIC: WEBP/HEIC/AVIF/TIFF fallback decoder    */
#  define MN_PATH_SEP '\\'
#else
#  include <sys/stat.h> /* mkdir */
#  include <sys/types.h>
#  include <dirent.h>
#  include <ctype.h>
#  include <unistd.h>    /* getpid */
#  define MN_PATH_SEP '/'
#endif

#include <ctype.h>

/* --------------------------------------------------------------------------
 * Hires-write attribution (see artcache.h). One process-lifetime counter per
 * metric; bumped ONLY on a successful "<hash>.hires.png" rename-into-place.
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
static volatile LONG64 g_hires_write_files = 0;
static volatile LONG64 g_hires_write_bytes = 0;
static void mn_art_note_hires_write(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    LONG64 sz = 0;
    if (path && GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        sz = ((LONG64)fad.nFileSizeHigh << 32) | (LONG64)fad.nFileSizeLow;
    InterlockedIncrement64(&g_hires_write_files);
    InterlockedExchangeAdd64(&g_hires_write_bytes, sz);
}
void mn_art_hires_stats(long long *out_files, long long *out_bytes)
{
    if (out_files) *out_files = (long long)
        InterlockedCompareExchange64(&g_hires_write_files, 0, 0);
    if (out_bytes) *out_bytes = (long long)
        InterlockedCompareExchange64(&g_hires_write_bytes, 0, 0);
}
#else
static long long g_hires_write_files = 0;
static long long g_hires_write_bytes = 0;
static void mn_art_note_hires_write(const char *path)
{
    struct stat st;
    __sync_fetch_and_add(&g_hires_write_files, 1);
    if (path && stat(path, &st) == 0)
        __sync_fetch_and_add(&g_hires_write_bytes, (long long)st.st_size);
}
void mn_art_hires_stats(long long *out_files, long long *out_bytes)
{
    if (out_files) *out_files = __sync_fetch_and_add(&g_hires_write_files, 0);
    if (out_bytes) *out_bytes = __sync_fetch_and_add(&g_hires_write_bytes, 0);
}
#endif

/* ASCII lowercase copy of s into out (bounded). Cover-art filenames are ASCII
 * in practice; this is only used for case-insensitive scoring, never for I/O. */
static void mn_art_ascii_lower(char *out, size_t n, const char *s)
{
    size_t i = 0;
    if (!out || n == 0) return;
    for (; s && s[i] && i + 1 < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[i] = '\0';
}

/* True if `hay` contains `needle` (both already lowercased). */
static bool mn_art_has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* UTF-8-aware fopen so cover files under non-ASCII directories open. Mirrors
 * mn_fopen_utf8 in tags.c. Falls back to plain fopen off Windows. */
static FILE *mn_art_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wp = NULL, *wm = NULL;
    FILE *f = NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    int mlen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (wlen <= 0 || mlen <= 0) return NULL;
    wp = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    wm = (wchar_t *)malloc((size_t)mlen * sizeof(wchar_t));
    if (wp && wm &&
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, wlen) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, mlen) > 0) {
        f = _wfopen(wp, wm);
    }
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

/* Score a candidate cover filename (already lowercased) by how likely it is to
 * be the album's front cover. Higher wins. Returns <0 to reject the file
 * outright (thumbnails, back covers, WMP small art, non-image, ...). Only the
 * base filename (no directory) is passed. */
static int mn_art_score_name(const char *lname, const char *ext_lower)
{
    /* Must be a supported raster image extension. webp/heic/heif/avif are
     * accepted here (increasingly common sidecars); they decode through the
     * WIC fallback on Windows (stb has no codec for them), and are logged
     * rather than silently dropped when no decoder is available. */
    bool is_img = (strcmp(ext_lower, "jpg")  == 0 ||
                   strcmp(ext_lower, "jpeg") == 0 ||
                   strcmp(ext_lower, "png")  == 0 ||
                   strcmp(ext_lower, "bmp")  == 0 ||
                   strcmp(ext_lower, "gif")  == 0 ||
                   strcmp(ext_lower, "webp") == 0 ||
                   strcmp(ext_lower, "heic") == 0 ||
                   strcmp(ext_lower, "heif") == 0 ||
                   strcmp(ext_lower, "avif") == 0);
    if (!is_img) return -1;

    /* Hard rejects: clearly-not-front-cover images. "albumartsmall" must be
     * rejected here so it never beats a real cover; it is only used as an
     * absolute last resort via the low positive score below when nothing
     * else matched (handled by the caller keeping the best non-negative). */
    if (mn_art_has(lname, "__ia_thumb") ||
        mn_art_has(lname, "thumb")      ||
        mn_art_has(lname, "-back")      ||
        mn_art_has(lname, "_back")      ||
        mn_art_has(lname, "back-")      ||
        mn_art_has(lname, "spine")      ||
        mn_art_has(lname, "tray")       ||
        mn_art_has(lname, "inlay")      ||
        mn_art_has(lname, "booklet")    ||
        mn_art_has(lname, "disc")       ||
        mn_art_has(lname, "cd1")        ||
        mn_art_has(lname, "cd2"))
        return -1;

    /* Exact conventional names (strongest signal), compared on the STEM so
     * every accepted extension ranks the same (cover.webp == cover.jpg). */
    {
        char stem[256];
        const char *dot = strrchr(lname, '.');
        size_t sl = dot ? (size_t)(dot - lname) : strlen(lname);
        if (sl >= sizeof(stem)) sl = sizeof(stem) - 1;
        memcpy(stem, lname, sl);
        stem[sl] = '\0';
        if (strcmp(stem, "cover") == 0)
            return 100;
        if (strcmp(stem, "folder") == 0)
            return 95;
        if (strcmp(stem, "front") == 0)
            return 90;
        if (strcmp(stem, "albumart") == 0 || strcmp(stem, "album") == 0)
            return 85;
    }

    /* "00-cover.jpg", "00-cover-front.jpg", "front cover.jpg", ... : contains
     * "cover" or "front" as a front-art hint (already excluded -back above). */
    if (mn_art_has(lname, "cover") || mn_art_has(lname, "front"))
        return 70;

    /* Windows Media Player: "AlbumArt_{GUID}_Large.jpg" (full-res). */
    if (mn_art_has(lname, "albumart_") && mn_art_has(lname, "_large"))
        return 60;

    /* WMP small art — last resort. */
    if (mn_art_has(lname, "albumartsmall") ||
        (mn_art_has(lname, "albumart_") && mn_art_has(lname, "_small")))
        return 5;

    /* Any other image in the folder: weak fallback (better than blank). */
    return 10;
}

/* --------------------------------------------------------------------------
 * Runtime thumbnail size (see mn_art_set_thumb_size in artcache.h).
 *
 * Read by scanner worker threads / written from the settings path; a single
 * aligned int, so torn reads are not possible on any supported platform, and
 * a slightly stale value only means one thumbnail renders at the prior size.
 * ------------------------------------------------------------------------ */
static int g_thumb_size = MN_ART_THUMB_SIZE;

void mn_art_set_thumb_size(int px)
{
    if (px < MN_ART_THUMB_MIN)
        px = MN_ART_THUMB_MIN;
    if (px > MN_ART_THUMB_MAX)
        px = MN_ART_THUMB_MAX;
    g_thumb_size = px;
}

int mn_art_get_thumb_size(void)
{
    return g_thumb_size;
}

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

/* FNV-1a 64-bit hash of a NUL-terminated string. Deterministic across runs and
 * platforms, which is what makes the cache filename stable per album_key. */
static uint64_t mn_art_fnv1a(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV offset basis */
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        h ^= (uint64_t)(*p++);
        h *= 0x100000001b3ULL; /* FNV prime */
    }
    return h;
}

/* Create a single directory, tolerating "already exists". Returns true if the
 * directory exists (or was created) afterward, false on a hard error. */
static bool mn_art_mkdir(const char *dir)
{
    if (!dir || !dir[0])
        return false;
#ifdef _WIN32
    if (_mkdir(dir) == 0)
        return true;
#else
    if (mkdir(dir, 0755) == 0)
        return true;
#endif
    /* errno == EEXIST (or a pre-existing dir) is success for our purposes. */
    return errno == EEXIST;
}

/* Return true iff a regular file exists AND is non-empty at path. A killed
 * process can leave a 0-byte (torn) thumbnail behind; treating those as
 * "present" is exactly what left albums permanently blank, so a zero-length
 * file is reported as ABSENT here and gets regenerated by mn_art_ensure.
 *
 * This is the check-only HOT PATH (called per album per render via
 * mn_app_art_path), so it must be a single metadata stat — the previous
 * fopen+fseek+ftell+fclose opened a kernel handle and touched file data
 * caches on every call. GetFileAttributesExW (Unicode-safe) / stat() yield
 * the size directly, preserving the 0-byte-as-absent rule. */
static bool mn_art_file_exists(const char *path)
{
#ifdef _WIN32
    wchar_t wp[MN_ART_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!path || !path[0])
        return false;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp,
                            (int)(sizeof(wp) / sizeof(wp[0]))) <= 0)
        return false;
    if (!GetFileAttributesExW(wp, GetFileExInfoStandard, &fad))
        return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    return (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0);
#else
    struct stat st;
    if (!path || !path[0])
        return false;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    return st.st_size > 0;
#endif
}

/* Delete a (possibly stale/empty) file, Unicode-safe. Best-effort. */
static void mn_art_unlink(const char *path)
{
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t *wp = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wp && MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, wlen) > 0)
            _wremove(wp);
        free(wp);
    }
#else
    remove(path);
#endif
}

/* Build the deterministic thumbnail path for album_key under cache_dir into
 * out_path. The requested pixel size participates in the filename for
 * non-default sizes (cache-buster: "<hash>_<size>.png"); the default size
 * keeps the legacy "<hash>.png" name so existing caches remain valid.
 * Returns false if the composed path would not fit in n bytes. */
static bool mn_art_build_path(const char *cache_dir,
                              const char *album_key,
                              int size_px,
                              char *out_path,
                              size_t n)
{
    uint64_t h = mn_art_fnv1a(album_key);

    /* Strip any trailing separator on cache_dir so we do not emit "a//b". */
    size_t dlen = strlen(cache_dir);
    while (dlen > 0 &&
           (cache_dir[dlen - 1] == '/' || cache_dir[dlen - 1] == '\\'))
        dlen--;

    int written;
    if (size_px == MN_ART_THUMB_SIZE) {
        /* "<cache_dir><sep><16 hex>.png" (legacy-compatible) */
        written = snprintf(out_path, n, "%.*s%c%016llx.png",
                           (int)dlen, cache_dir, MN_PATH_SEP,
                           (unsigned long long)h);
    } else {
        /* "<cache_dir><sep><16 hex>_<size>.png" */
        written = snprintf(out_path, n, "%.*s%c%016llx_%d.png",
                           (int)dlen, cache_dir, MN_PATH_SEP,
                           (unsigned long long)h, size_px);
    }
    if (written < 0 || (size_t)written >= n)
        return false;
    return true;
}

/* Length of the directory prefix of audio_path (index just past the last
 * separator; 0 if none), so "<prefix><sidecar>" names a sibling file. */
static size_t mn_art_dir_prefix_len(const char *audio_path)
{
    size_t dir_len = strlen(audio_path);
    while (dir_len > 0 &&
           audio_path[dir_len - 1] != '/' &&
           audio_path[dir_len - 1] != '\\')
        dir_len--;
    return dir_len;
}

/* Extract the lowercased extension (without dot) of a filename into ext. */
static void mn_art_ext_lower(char *ext, size_t n, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (ext && n) ext[0] = '\0';
    if (dot && dot[1]) mn_art_ascii_lower(ext, n, dot + 1);
}

/* A ranked sidecar candidate: full path + its mn_art_score_name score. The
 * scan keeps the TOP-N candidates in score order (not just the winner) so a
 * corrupt/undecodable best-scoring file (e.g. an all-zero Cover.jpg) can no
 * longer fail the whole album when a valid Folder.jpg sits right beside it —
 * the decode path falls through to the next candidate. */
#define MN_ART_SIDECAR_CANDS 6
typedef struct {
    int  score;
    char name[MN_ART_PATH_MAX];  /* bare filename during the scan; the
                                  * sidecar-cands wrapper joins it to a
                                  * full path in place */
} mn_art_scand;

/* Scan ONE directory (`dir` ends with a separator, length dir_len) for cover
 * images, collecting up to `cap` positive-scoring candidates in DESCENDING
 * score order (stable: earlier-enumerated wins ties). Returns the count. */
static int mn_art_scan_one_dir_cands(const char *dir, size_t dir_len,
                                     mn_art_scand *cands, int cap)
{
    int n = 0;

    if (dir_len == 0 || !cands || cap <= 0)
        return 0;

#define MN_ART_CAND_INSERT(score_, name_)                                  \
    do {                                                                   \
        int ins = n;                                                       \
        while (ins > 0 && cands[ins - 1].score < (score_)) ins--;          \
        if (ins < cap) {                                                   \
            int mv = (n < cap) ? n : cap - 1;                              \
            for (; mv > ins; mv--) cands[mv] = cands[mv - 1];              \
            cands[ins].score = (score_);                                   \
            snprintf(cands[ins].name, sizeof(cands[ins].name), "%s",       \
                     (name_));                                             \
            if (n < cap) n++;                                              \
        }                                                                  \
    } while (0)

#ifdef _WIN32
    {
        char     pattern[MN_ART_PATH_MAX];
        wchar_t *wpat = NULL;
        WIN32_FIND_DATAW fd;
        HANDLE   h;
        int      wlen;

        if (dir_len + 2 >= sizeof(pattern)) return 0;
        snprintf(pattern, sizeof(pattern), "%s*", dir);
        wlen = MultiByteToWideChar(CP_UTF8, 0, pattern, -1, NULL, 0);
        if (wlen <= 0) return 0;
        wpat = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (!wpat) return 0;
        if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpat, wlen) <= 0) {
            free(wpat);
            return 0;
        }
        h = FindFirstFileW(wpat, &fd);
        free(wpat);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            char  name[512];
            char  lname[512];
            char  ext[16];
            int   score;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                    name, sizeof(name), NULL, NULL) <= 0)
                continue;
            mn_art_ascii_lower(lname, sizeof(lname), name);
            mn_art_ext_lower(ext, sizeof(ext), lname);
            score = mn_art_score_name(lname, ext);
            if (score > 0)
                MN_ART_CAND_INSERT(score, name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    {
        char dscan[MN_ART_PATH_MAX];
        DIR *d;
        struct dirent *de;
        /* opendir wants the dir without a trailing slash (tolerated, but be
         * tidy); keep dir with sep for path assembly below. */
        snprintf(dscan, sizeof(dscan), "%.*s", (int)dir_len, dir);
        d = opendir(dscan);
        if (!d) return 0;
        while ((de = readdir(d)) != NULL) {
            char lname[512];
            char ext[16];
            int  score;
            mn_art_ascii_lower(lname, sizeof(lname), de->d_name);
            mn_art_ext_lower(ext, sizeof(ext), lname);
            score = mn_art_score_name(lname, ext);
            if (score > 0)
                MN_ART_CAND_INSERT(score, de->d_name);
        }
        closedir(d);
    }
#endif
#undef MN_ART_CAND_INSERT

    return n;
}

/* Collect ranked sidecar candidates for audio_path as FULL paths: the track's
 * own folder first; if it has none, the PARENT folder (multi-disc rips keep
 * the cover at the album root). Returns the number of candidates written. */
static int mn_art_sidecar_cands(const char *audio_path,
                                mn_art_scand *cands, int cap)
{
    size_t dir_len = mn_art_dir_prefix_len(audio_path);
    char   dir[MN_ART_PATH_MAX];
    int    n = 0, i;

    if (dir_len == 0 || dir_len + 1 >= sizeof(dir) || !cands || cap <= 0)
        return 0;
    memcpy(dir, audio_path, dir_len);
    dir[dir_len] = '\0';        /* keeps the trailing separator */

    n = mn_art_scan_one_dir_cands(dir, dir_len, cands, cap);
    if (n == 0) {
        /* PARENT folder: drop the trailing separator, then the last component. */
        size_t p = dir_len;
        if (p > 0 && (dir[p - 1] == '\\' || dir[p - 1] == '/')) p--;
        while (p > 0 && dir[p - 1] != '\\' && dir[p - 1] != '/') p--;
        if (p > 3) {
            dir[p] = '\0';
            dir_len = p;
            n = mn_art_scan_one_dir_cands(dir, dir_len, cands, cap);
        }
    }
    /* names -> full paths, in place (name buffer is large enough to hold the
     * joined path check; overlong joins are dropped). */
    {
        int w = 0;
        for (i = 0; i < n; ++i) {
            char full[MN_ART_PATH_MAX];
            if (dir_len + strlen(cands[i].name) + 1 > sizeof(full))
                continue;
            snprintf(full, sizeof(full), "%s%s", dir, cands[i].name);
            snprintf(cands[w].name, sizeof(cands[w].name), "%s", full);
            cands[w].score = cands[i].score;
            w++;
        }
        n = w;
    }
    return n;
}

/* Find the best cover image next to the audio file (single-winner wrapper
 * over mn_art_sidecar_cands — own folder first, then the album-root parent
 * for multi-disc rips). Used by the cheap mn_art_probe check. */
static bool mn_art_scan_sidecar(const char *audio_path,
                                char *out_path, size_t out_n)
{
    mn_art_scand c[1];
    if (mn_art_sidecar_cands(audio_path, c, 1) <= 0)
        return false;
    if (strlen(c[0].name) + 1 > out_n)
        return false;
    snprintf(out_path, out_n, "%s", c[0].name);
    return true;
}

/* Cheap availability check — embedded picture OR sidecar file present. No
 * decode. See artcache.h. */
bool mn_art_probe(const char *audio_path)
{
    uint8_t *bytes = NULL;
    size_t   len = 0;
    char     sidecar[MN_ART_PATH_MAX];

    if (!audio_path || !audio_path[0])
        return false;

    /* 1) Embedded picture. mn_tags_read_cover DOES allocate; free immediately —
     * we only care that it exists. It short-circuits as soon as the first
     * picture atom/frame is found, so this is bounded. */
    if (mn_tags_read_cover(audio_path, &bytes, &len, NULL) && bytes && len > 0) {
        mn_tags_free_cover(bytes);
        return true;
    }
    if (bytes) {
        mn_tags_free_cover(bytes);
        bytes = NULL;
    }

    /* 2) Any scoreable sidecar image in the same directory (cover/folder/front,
     * WMP AlbumArt_*_Large, 00-cover*, or a lone image fallback). */
    return mn_art_scan_sidecar(audio_path, sidecar, sizeof(sidecar));
}

/* Try to load raw cover-art bytes for audio_path. First asks tags.h for an
 * embedded picture; if none is present, probes for a conventional sidecar
 * image (cover.jpg, folder.jpg, ...) in the same directory.
 *
 * On success sets *out_bytes (heap owned) / *out_len and returns one of two
 * ownership modes via *out_from_tags:
 *   true  -> release with mn_tags_free_cover()
 *   false -> release with free()
 * Returns false and leaves outputs untouched when no cover could be found. */
/* Cheap sniff: do these bytes begin with a known raster-image magic that stb
 * can decode? Guards against a truncated / mis-offset embedded picture (a real
 * bug: a mis-aligned ID3 APIC description skip can hand back bytes that start a
 * few bytes BEFORE the JPEG SOI). Rejecting those here lets the caller fall
 * through to a folder sidecar instead of caching nothing. */
static bool mn_art_looks_like_image(const uint8_t *b, size_t n)
{
    if (!b || n < 4) return false;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;            /* JPEG   */
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return true; /* PNG  */
    if (b[0] == 'B' && b[1] == 'M') return true;                             /* BMP    */
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return true;              /* GIF    */
    if (n >= 12 && b[0]=='R'&&b[1]=='I'&&b[2]=='F'&&b[3]=='F'
        && b[8]=='W'&&b[9]=='E'&&b[10]=='B'&&b[11]=='P') return true;        /* WEBP   */
    if (n >= 12 && b[4]=='f'&&b[5]=='t'&&b[6]=='y'&&b[7]=='p') return true;  /* HEIF/AVIF (stb may not decode, but let it try) */
    return false;
}

#ifdef _WIN32
/* WIC (Windows Imaging Component) fallback decoder for formats stb has no
 * codec for — WEBP, HEIC/HEIF, AVIF, TIFF. WIC ships the codecs system-wide
 * (WebP/HEIF arrive via the OS "Image Extensions"; JPEG/PNG/TIFF are always
 * present), so covers in modern formats decode without vendoring a library.
 * Returns a malloc'd tightly-packed pixel buffer (req_comp = 3 RGB or 4 RGBA)
 * compatible with stbi_image_free (which is free()), or NULL. Reentrant:
 * per-call factory, per-call CoInitializeEx (balanced). */
static uint8_t *mn_art_wic_decode(const uint8_t *bytes, size_t len,
                                  int *out_w, int *out_h, int req_comp)
{
    IWICImagingFactory    *fac    = NULL;
    IWICStream            *stream = NULL;
    IWICBitmapDecoder     *dec    = NULL;
    IWICBitmapFrameDecode *frame  = NULL;
    IWICFormatConverter   *conv   = NULL;
    uint8_t               *px     = NULL;
    UINT                   w = 0, h = 0;
    HRESULT                hrco, hr;
    const GUID *fmt = (req_comp == 4) ? &GUID_WICPixelFormat32bppRGBA
                                      : &GUID_WICPixelFormat24bppRGB;

    if (!bytes || len == 0 || len > 0x7fffffffu ||
        (req_comp != 3 && req_comp != 4))
        return NULL;

    hrco = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&fac);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(fac, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(stream,
                                (BYTE *)bytes, (DWORD)len);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream(fac,
                                (IStream *)stream, NULL,
                                WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(fac, &conv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(conv,
                                (IWICBitmapSource *)frame, fmt,
                                WICBitmapDitherTypeNone, NULL, 0.0,
                                WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize(conv, &w, &h);
    if (SUCCEEDED(hr) && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
        UINT stride = w * (UINT)req_comp;
        UINT size   = stride * h;
        px = (uint8_t *)malloc(size);
        if (px &&
            FAILED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)conv,
                                               NULL, stride, size, px))) {
            free(px);
            px = NULL;
        }
    }
    if (px) {
        if (out_w) *out_w = (int)w;
        if (out_h) *out_h = (int)h;
    }
    if (conv)   IWICFormatConverter_Release(conv);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    if (fac)    IWICImagingFactory_Release(fac);
    if (hrco == S_OK || hrco == S_FALSE)
        CoUninitialize();
    return px;
}
#endif /* _WIN32 */

/* Decode raw image bytes to a tightly-packed req_comp buffer: stb first
 * (JPEG/PNG/BMP/GIF, the overwhelming majority), then the platform fallback
 * (WIC on Windows) for WEBP/HEIC/AVIF/TIFF covers stb can't read. The result
 * is always releasable with stbi_image_free() (free()). Logs an undecodable
 * cover instead of silently dropping it — so "why is this album blank" is
 * answerable from the console. */
static uint8_t *mn_art_decode_mem(const uint8_t *bytes, size_t len,
                                  int *out_w, int *out_h, int req_comp,
                                  const char *origin_hint)
{
    int comp = 0;
    uint8_t *px = stbi_load_from_memory(bytes, (int)len,
                                        out_w, out_h, &comp, req_comp);
    if (px)
        return px;
#ifdef _WIN32
    px = mn_art_wic_decode(bytes, len, out_w, out_h, req_comp);
    if (px)
        return px;
#endif
    fprintf(stderr, "[artcache] undecodable cover (%zu bytes)%s%s\n",
            len, origin_hint ? " for " : "", origin_hint ? origin_hint : "");
    return NULL;
}

/* Slurp a whole file into a malloc'd buffer. Returns NULL on any error. */
static uint8_t *mn_art_read_file(const char *path, size_t *out_len)
{
    FILE *f = mn_art_fopen(path, "rb");
    long sz;
    uint8_t *buf;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* Decode the best AVAILABLE cover for audio_path into a tightly-packed
 * req_comp buffer (free with stbi_image_free): the embedded picture first,
 * then every sidecar candidate in SCORE ORDER until one decodes. This is the
 * fix for the false-NONE class: a corrupt top-ranked Cover.jpg (or a format
 * neither stb nor WIC can read) falls through to the next candidate instead
 * of failing the album outright.
 *
 * `out_src_seen` (optional) reports whether ANY potential source existed —
 * embedded bytes or at least one sidecar candidate — even if none decoded.
 * Callers use it to distinguish "source exists but undecodable" (transient
 * verdict only; re-probed next heal tick) from "genuinely artless" (persisted
 * NONE verdict). */
static uint8_t *mn_art_decode_cover(const char *audio_path,
                                    int *out_w, int *out_h, int req_comp,
                                    bool *out_src_seen)
{
    uint8_t *px = NULL;

    if (out_src_seen) *out_src_seen = false;

    /* 1) Embedded picture via tags.h (magic-sniffed: a corrupt / mis-offset
     * APIC must not short-circuit the sidecar fallback). */
    {
        uint8_t *bytes = NULL;
        size_t   len = 0;
        if (mn_tags_read_cover(audio_path, &bytes, &len, NULL) &&
            bytes && len > 0) {
            if (out_src_seen) *out_src_seen = true;
            if (mn_art_looks_like_image(bytes, len))
                px = mn_art_decode_mem(bytes, len, out_w, out_h, req_comp,
                                       audio_path);
        }
        if (bytes)
            mn_tags_free_cover(bytes);
        if (px)
            return px;
    }

    /* 2) Sidecar candidates, best score first — first one that decodes wins. */
    {
        mn_art_scand cands[MN_ART_SIDECAR_CANDS];
        int n = mn_art_sidecar_cands(audio_path, cands,
                                     MN_ART_SIDECAR_CANDS);
        int i;
        if (n > 0 && out_src_seen) *out_src_seen = true;
        for (i = 0; i < n && !px; ++i) {
            size_t   len = 0;
            uint8_t *bytes = mn_art_read_file(cands[i].name, &len);
            if (!bytes)
                continue;
            px = mn_art_decode_mem(bytes, len, out_w, out_h, req_comp,
                                   cands[i].name);
            free(bytes);
        }
    }
    return px;
}

/* Center-crop an RGBA image (w x h) to a square of side min(w,h), writing the
 * cropped pixels into a freshly allocated tightly-packed buffer. On success
 * returns the buffer and sets *out_side; caller frees with free(). Returns NULL
 * on allocation failure or invalid dimensions. */
static uint8_t *mn_art_center_crop_square(const uint8_t *src,
                                          int w, int h,
                                          int *out_side)
{
    if (!src || w <= 0 || h <= 0)
        return NULL;

    int side = (w < h) ? w : h;
    int off_x = (w - side) / 2;
    int off_y = (h - side) / 2;

    uint8_t *dst = (uint8_t *)malloc((size_t)side * (size_t)side * MN_ART_BPP);
    if (!dst)
        return NULL;

    for (int y = 0; y < side; ++y) {
        const uint8_t *srow =
            src + ((size_t)(y + off_y) * (size_t)w + (size_t)off_x) * MN_ART_BPP;
        uint8_t *drow = dst + (size_t)y * (size_t)side * MN_ART_BPP;
        memcpy(drow, srow, (size_t)side * MN_ART_BPP);
    }

    *out_side = side;
    return dst;
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

bool mn_art_ensure(const char *cache_dir,
                   const char *album_key,
                   const char *audio_path,
                   char *out_path,
                   size_t n)
{
    if (!cache_dir || !cache_dir[0] || !album_key || !album_key[0] ||
        !out_path || n == 0)
        return false;

    /* Snapshot the configured size once so path + resize agree even if the
     * setting changes concurrently. */
    const int size_px = mn_art_get_thumb_size();

    /* Compose the deterministic destination path first; every branch needs it. */
    char thumb_path[MN_ART_PATH_MAX];
    if (!mn_art_build_path(cache_dir, album_key, size_px, thumb_path,
                           sizeof(thumb_path)))
        return false;

    /* Fast path / check-only path: an existing cached thumbnail wins. */
    if (mn_art_file_exists(thumb_path)) {
        size_t need = strlen(thumb_path) + 1;
        if (need > n)
            return false;
        memcpy(out_path, thumb_path, need);
        return true;
    }

    /* No cached thumbnail at the configured size. Check-only callers fall
     * back to the default-size thumbnail if one exists (so a size change
     * never blanks the UI before a rescan regenerates sized thumbs). */
    if (!audio_path || !audio_path[0]) {
        if (size_px != MN_ART_THUMB_SIZE &&
            mn_art_build_path(cache_dir, album_key, MN_ART_THUMB_SIZE,
                              thumb_path, sizeof(thumb_path)) &&
            mn_art_file_exists(thumb_path)) {
            size_t need = strlen(thumb_path) + 1;
            if (need > n)
                return false;
            memcpy(out_path, thumb_path, need);
            return true;
        }
        return false;
    }

    /* Decode the best available cover: embedded first, then EVERY sidecar
     * candidate in score order (a corrupt/undecodable top candidate falls
     * through instead of failing the album — the false-NONE fix). */
    bool ok = false;
    uint8_t *decoded = NULL;
    uint8_t *square = NULL;
    uint8_t *thumb = NULL;

    int dw = 0, dh = 0;
    decoded = mn_art_decode_cover(audio_path, &dw, &dh, MN_ART_BPP, NULL);
    if (!decoded)
        return false;

    /* Center-crop to a square. */
    int side = 0;
    square = mn_art_center_crop_square(decoded, dw, dh, &side);
    if (!square)
        goto cleanup;

    /* Resize the square down (or up) to the configured thumbnail size. */
    thumb = (uint8_t *)malloc((size_t)size_px * (size_t)size_px * MN_ART_BPP);
    if (!thumb)
        goto cleanup;

    if (!stbir_resize_uint8_srgb(square, side, side,
                                 side * MN_ART_BPP,
                                 thumb, size_px, size_px,
                                 size_px * MN_ART_BPP,
                                 STBIR_RGBA))
        goto cleanup;

    /* Ensure the cache directory exists, then write the PNG ATOMICALLY: encode
     * to a per-process temp sibling, then rename over the final name. A crash /
     * taskkill mid-encode then leaves at most a stray .tmp, never a torn 0-byte
     * <hash>.png that would be trusted forever (the bug that blanked albums). */
    if (!mn_art_mkdir(cache_dir))
        goto cleanup;

    {
        char tmp_path[MN_ART_PATH_MAX + 32];
#ifdef _WIN32
        unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        int w = snprintf(tmp_path, sizeof(tmp_path), "%s.%lu.tmp",
                         thumb_path, pid);
        if (w < 0 || (size_t)w >= sizeof(tmp_path))
            goto cleanup;

        if (!stbi_write_png(tmp_path, size_px, size_px,
                            MN_ART_BPP, thumb,
                            size_px * MN_ART_BPP))
            goto cleanup;

#ifdef _WIN32
        if (!MoveFileExA(tmp_path, thumb_path, MOVEFILE_REPLACE_EXISTING)) {
            mn_art_unlink(tmp_path);
            goto cleanup;
        }
#else
        if (rename(tmp_path, thumb_path) != 0) {
            mn_art_unlink(tmp_path);
            goto cleanup;
        }
#endif
    }

    /* Hand the path back to the caller. */
    {
        size_t need = strlen(thumb_path) + 1;
        if (need > n)
            goto cleanup;
        memcpy(out_path, thumb_path, need);
    }
    ok = true;

cleanup:
    if (thumb)
        free(thumb);
    if (square)
        free(square);
    if (decoded)
        stbi_image_free(decoded);
    return ok;
}

/* Build the "<hash>.hires.png" path for album_key under cache_dir. Uses the
 * SAME FNV-1a hash as mn_art_build_path so a thumb and its hi-res sibling share
 * the hash stem. Returns false if the path would not fit in n bytes. */
static bool mn_art_build_hires_path(const char *cache_dir,
                                    const char *album_key,
                                    char *out_path,
                                    size_t n)
{
    uint64_t h = mn_art_fnv1a(album_key);
    size_t dlen = strlen(cache_dir);
    while (dlen > 0 &&
           (cache_dir[dlen - 1] == '/' || cache_dir[dlen - 1] == '\\'))
        dlen--;
    int written = snprintf(out_path, n, "%.*s%c%016llx.hires.png",
                           (int)dlen, cache_dir, MN_PATH_SEP,
                           (unsigned long long)h);
    if (written < 0 || (size_t)written >= n)
        return false;
    return true;
}

bool mn_art_ensure_hires(const char *cache_dir,
                         const char *album_key,
                         const char *audio_path,
                         char *out_path,
                         size_t n)
{
    if (!cache_dir || !cache_dir[0] || !album_key || !album_key[0] ||
        !out_path || n == 0)
        return false;

    char hires_path[MN_ART_PATH_MAX];
    if (!mn_art_build_hires_path(cache_dir, album_key, hires_path,
                                 sizeof(hires_path)))
        return false;

    /* Cached hi-res wins (and the check-only path stops here). */
    if (mn_art_file_exists(hires_path)) {
        size_t need = strlen(hires_path) + 1;
        if (need > n) return false;
        memcpy(out_path, hires_path, need);
        return true;
    }
    if (!audio_path || !audio_path[0])
        return false;

    bool     ok = false;
    uint8_t *decoded = NULL;   /* RGB (3ch)               */
    uint8_t *scaled  = NULL;   /* long-edge-capped RGB    */

    /* Decode to RGB (depth wants 3-channel; the mesh texture too). Same
     * embedded-then-ranked-sidecar fallback routing as the thumb path, so
     * the hires and thumb always agree on source + decodability. */
    int dw = 0, dh = 0;
    decoded = mn_art_decode_cover(audio_path, &dw, &dh, 3, NULL);
    if (!decoded || dw <= 0 || dh <= 0)
        goto cleanup;

    /* Scale the long edge down to at most MN_ART_HIRES_MAX, preserve aspect,
     * NEVER upscale (a small embedded cover stays its native size). */
    int ow = dw, oh = dh;
    int longest = (dw > dh) ? dw : dh;
    if (longest > MN_ART_HIRES_MAX) {
        double scale = (double)MN_ART_HIRES_MAX / (double)longest;
        ow = (int)(dw * scale + 0.5);
        oh = (int)(dh * scale + 0.5);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
    }

    const uint8_t *out_pixels;
    if (ow == dw && oh == dh) {
        out_pixels = decoded;                      /* no resize needed */
    } else {
        scaled = (uint8_t *)malloc((size_t)ow * (size_t)oh * 3);
        if (!scaled) goto cleanup;
        if (!stbir_resize_uint8_srgb(decoded, dw, dh, dw * 3,
                                     scaled, ow, oh, ow * 3, STBIR_RGB))
            goto cleanup;
        out_pixels = scaled;
    }

    if (!mn_art_mkdir(cache_dir))
        goto cleanup;

    /* Atomic write: temp sibling + rename (same discipline as the thumb path). */
    {
        char tmp_path[MN_ART_PATH_MAX + 32];
#ifdef _WIN32
        unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        int w = snprintf(tmp_path, sizeof(tmp_path), "%s.%lu.tmp",
                         hires_path, pid);
        if (w < 0 || (size_t)w >= sizeof(tmp_path))
            goto cleanup;
        if (!stbi_write_png(tmp_path, ow, oh, 3, out_pixels, ow * 3))
            goto cleanup;
#ifdef _WIN32
        if (!MoveFileExA(tmp_path, hires_path, MOVEFILE_REPLACE_EXISTING)) {
            mn_art_unlink(tmp_path);
            goto cleanup;
        }
#else
        if (rename(tmp_path, hires_path) != 0) {
            mn_art_unlink(tmp_path);
            goto cleanup;
        }
#endif
        mn_art_note_hires_write(hires_path);   /* session attribution ledger */
    }

    {
        size_t need = strlen(hires_path) + 1;
        if (need > n) goto cleanup;
        memcpy(out_path, hires_path, need);
    }
    ok = true;

cleanup:
    if (scaled)  free(scaled);
    if (decoded) stbi_image_free(decoded);
    return ok;
}

uint8_t *mn_art_load_rgba(const char *thumb_path, int *out_w, int *out_h)
{
    if (!thumb_path || !out_w || !out_h)
        return NULL;

    int w = 0, h = 0, comp = 0;
    uint8_t *px = stbi_load(thumb_path, &w, &h, &comp, MN_ART_BPP);
    if (!px)
        return NULL;

    *out_w = w;
    *out_h = h;
    return px;
}

void mn_art_free(uint8_t *px)
{
    if (px)
        stbi_image_free(px);
}

/* --------------------------------------------------------------------------
 * mn_art_ingest_image — replace an album's cached art from an image FILE
 * (e.g. a cover downloaded from an online source). Writes both the square
 * grid thumb (g_thumb_size) and the aspect-preserving hi-res companion the
 * depth/3D pipeline samples. Atomic tmp+rename writes, like mn_art_ensure.
 * -------------------------------------------------------------------------- */
bool mn_art_ingest_image(const char *cache_dir,
                         const char *album_key,
                         const char *image_path,
                         char *out_path,
                         size_t n)
{
    enum { MN_ART_INGEST_HIRES_MAX = 1200 };
    const int size_px = mn_art_get_thumb_size();
    char thumb_path[MN_ART_PATH_MAX];
    char hires_path[MN_ART_PATH_MAX];
    uint8_t *decoded = NULL, *square = NULL, *thumb = NULL, *hires = NULL;
    int dw = 0, dh = 0, dcomp = 0;
    bool ok = false;

    if (!cache_dir || !album_key || !image_path || !out_path || n == 0)
        return false;
    if (!mn_art_build_path(cache_dir, album_key, size_px,
                           thumb_path, sizeof(thumb_path)))
        return false;
    if (!mn_art_build_hires_path(cache_dir, album_key,
                                 hires_path, sizeof(hires_path)))
        return false;

    decoded = stbi_load(image_path, &dw, &dh, &dcomp, MN_ART_BPP);
    if (!decoded) {
        /* Downloaded covers are increasingly webp/avif — read the file and
         * route through the same stb→WIC fallback the extract path uses. */
        FILE *f = mn_art_fopen(image_path, "rb");
        if (f) {
            long sz = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
                uint8_t *buf = (uint8_t *)malloc((size_t)sz);
                if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                    decoded = mn_art_decode_mem(buf, (size_t)sz, &dw, &dh,
                                                MN_ART_BPP, image_path);
                free(buf);
            }
            fclose(f);
        }
    }
    if (!decoded || dw <= 0 || dh <= 0)
        goto cleanup;
    if (!mn_art_mkdir(cache_dir))
        goto cleanup;

    /* ---- square grid thumb ---- */
    {
        int side = 0;
        square = mn_art_center_crop_square(decoded, dw, dh, &side);
        if (!square)
            goto cleanup;
        thumb = (uint8_t *)malloc((size_t)size_px * (size_t)size_px * MN_ART_BPP);
        if (!thumb)
            goto cleanup;
        if (!stbir_resize_uint8_srgb(square, side, side, side * MN_ART_BPP,
                                     thumb, size_px, size_px,
                                     size_px * MN_ART_BPP, STBIR_RGBA))
            goto cleanup;
        {
            char tmp_path[MN_ART_PATH_MAX + 32];
#ifdef _WIN32
            unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
            unsigned long pid = (unsigned long)getpid();
#endif
            int w = snprintf(tmp_path, sizeof(tmp_path), "%s.%lu.tmp",
                             thumb_path, pid);
            if (w < 0 || (size_t)w >= sizeof(tmp_path))
                goto cleanup;
            if (!stbi_write_png(tmp_path, size_px, size_px, MN_ART_BPP,
                                thumb, size_px * MN_ART_BPP))
                goto cleanup;
#ifdef _WIN32
            if (!MoveFileExA(tmp_path, thumb_path, MOVEFILE_REPLACE_EXISTING)) {
                mn_art_unlink(tmp_path);
                goto cleanup;
            }
#else
            if (rename(tmp_path, thumb_path) != 0) {
                mn_art_unlink(tmp_path);
                goto cleanup;
            }
#endif
        }
    }

    /* ---- hi-res companion (aspect preserved, long edge capped) ---- */
    {
        int ow = dw, oh = dh;
        const uint8_t *src_rgba = decoded;
        int long_edge = (dw > dh) ? dw : dh;
        if (long_edge > MN_ART_INGEST_HIRES_MAX) {
            double s = (double)MN_ART_INGEST_HIRES_MAX / (double)long_edge;
            ow = (int)(dw * s + 0.5);
            oh = (int)(dh * s + 0.5);
            if (ow < 1) ow = 1;
            if (oh < 1) oh = 1;
        }
        hires = (uint8_t *)malloc((size_t)ow * (size_t)oh * MN_ART_BPP);
        if (!hires)
            goto cleanup;
        if (ow == dw && oh == dh) {
            memcpy(hires, src_rgba, (size_t)ow * (size_t)oh * MN_ART_BPP);
        } else if (!stbir_resize_uint8_srgb(src_rgba, dw, dh, dw * MN_ART_BPP,
                                            hires, ow, oh, ow * MN_ART_BPP,
                                            STBIR_RGBA)) {
            goto cleanup;
        }
        {
            char tmp_path[MN_ART_PATH_MAX + 32];
#ifdef _WIN32
            unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
            unsigned long pid = (unsigned long)getpid();
#endif
            int w = snprintf(tmp_path, sizeof(tmp_path), "%s.%lu.tmp",
                             hires_path, pid);
            if (w < 0 || (size_t)w >= sizeof(tmp_path))
                goto cleanup;
            if (!stbi_write_png(tmp_path, ow, oh, MN_ART_BPP,
                                hires, ow * MN_ART_BPP))
                goto cleanup;
#ifdef _WIN32
            if (!MoveFileExA(tmp_path, hires_path, MOVEFILE_REPLACE_EXISTING)) {
                mn_art_unlink(tmp_path);
                goto cleanup;
            }
#else
            if (rename(tmp_path, hires_path) != 0) {
                mn_art_unlink(tmp_path);
                goto cleanup;
            }
#endif
            mn_art_note_hires_write(hires_path);   /* attribution ledger */
        }
    }

    {
        size_t need = strlen(thumb_path) + 1;
        if (need > n)
            goto cleanup;
        memcpy(out_path, thumb_path, need);
    }
    ok = true;

cleanup:
    if (thumb) free(thumb);
    if (square) free(square);
    if (hires) free(hires);
    if (decoded) stbi_image_free(decoded);
    return ok;
}
