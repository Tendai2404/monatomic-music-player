/*
 * app.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * Implementation of the top-level application controller declared in
 * app.h. This module is the single seam between the UI/bridge layer and
 * the individual subsystems:
 *
 *     library_db  — SQLite index (windowed queries, facets, playlists)
 *     audio_engine— miniaudio playback engine
 *     playback    — queue/transport controller over the engine
 *     stems       — neural stem separation session (HTDemucs/ONNX)
 *     scanner     — multi-threaded incremental filesystem scanner
 *     tags        — metadata reader (used by the scan callback)
 *     artcache    — per-album cover-art thumbnail cache
 *
 * ------------------------------------------------------------------
 * Header-symbol isolation
 * ------------------------------------------------------------------
 * Several of these headers were written independently and reuse the same
 * MN_* enumerator names with *different* values:
 *
 *   - audio_engine.h  mn_result   : MN_OK/MN_ERR_INVALID/... (negative codes)
 *   - library_db.h    mn_status   : MN_OK/MN_ERR_INVALID/... (positive codes)
 *                     mn_sort_key : MN_SORT_TITLE/...
 *   - playback.h      mn_repeat_mode: MN_REPEAT_OFF/ALL/ONE
 *   - app.h (ours)    mn_sort / mn_repeat : MN_SORT_* / MN_REPEAT_*
 *
 * They cannot all be included verbatim in one translation unit (MSVC
 * C2365 redefinition). app.h is the compile-time contract we implement,
 * so its names are kept verbatim; the conflicting enumerators of the
 * *other* headers are renamed at include time with the preprocessor and
 * immediately #undef'd, so the rest of this file refers to them through
 * unambiguous prefixed spellings:
 *
 *     engine result codes  -> MNE_*
 *     db status codes      -> MNDB_*
 *     db sort keys         -> MNDB_SORT_*
 *     playback repeat mode -> MNP_REPEAT_*
 *
 * This is purely a compile-time aliasing of enumerator tokens; the ABI of
 * every subsystem is untouched.
 */

/* ------------------------------------------------------------------ */
/* audio_engine.h — alias mn_result codes to MNE_*                    */
/* ------------------------------------------------------------------ */
#define MN_OK              MNE_OK
#define MN_ERR_INVALID     MNE_ERR_INVALID
#define MN_ERR_NOMEM       MNE_ERR_NOMEM
#define MN_ERR_DEVICE      MNE_ERR_DEVICE
#define MN_ERR_OPEN        MNE_ERR_OPEN
#define MN_ERR_UNSUPPORTED MNE_ERR_UNSUPPORTED
#define MN_ERR_STATE       MNE_ERR_STATE
#define MN_ERR_SEEK        MNE_ERR_SEEK
#define MN_ERR_IO          MNE_ERR_IO
#include "audio_engine.h"
#undef MN_OK
#undef MN_ERR_INVALID
#undef MN_ERR_NOMEM
#undef MN_ERR_DEVICE
#undef MN_ERR_OPEN
#undef MN_ERR_UNSUPPORTED
#undef MN_ERR_STATE
#undef MN_ERR_SEEK
#undef MN_ERR_IO

/* ------------------------------------------------------------------ */
/* playback.h — alias repeat enumerators to MNP_*                     */
/* ------------------------------------------------------------------ */
#define MN_REPEAT_OFF MNP_REPEAT_OFF
#define MN_REPEAT_ALL MNP_REPEAT_ALL
#define MN_REPEAT_ONE MNP_REPEAT_ONE
#include "playback.h"
#undef MN_REPEAT_OFF
#undef MN_REPEAT_ALL
#undef MN_REPEAT_ONE

/* These carry no conflicting enumerators. */
#include "stems.h"
#include "scanner.h"
#include "tags.h"
#include "tags_write.h"
#include "artcache.h"
#include "modeldl.h"

/* ------------------------------------------------------------------ */
/* library_db.h — alias mn_status + mn_sort_key to MNDB_*             */
/* ------------------------------------------------------------------ */
#define MN_OK                MNDB_OK
#define MN_ERR_GENERIC       MNDB_ERR_GENERIC
#define MN_ERR_NOMEM         MNDB_ERR_NOMEM
#define MN_ERR_INVALID       MNDB_ERR_INVALID
#define MN_ERR_NOTFOUND      MNDB_ERR_NOTFOUND
#define MN_ERR_BUSY          MNDB_ERR_BUSY
#define MN_ERR_IO            MNDB_ERR_IO
#define MN_ERR_CORRUPT       MNDB_ERR_CORRUPT
#define MN_ERR_CONSTRAINT    MNDB_ERR_CONSTRAINT
#define MN_ERR_MIGRATE       MNDB_ERR_MIGRATE
#define MN_ERR_RANGE         MNDB_ERR_RANGE
#define MN_ERR_STATE         MNDB_ERR_STATE
#define MN_SORT_NONE         MNDB_SORT_NONE
#define MN_SORT_TITLE        MNDB_SORT_TITLE
#define MN_SORT_ARTIST       MNDB_SORT_ARTIST
#define MN_SORT_ALBUM        MNDB_SORT_ALBUM
#define MN_SORT_ALBUM_ARTIST MNDB_SORT_ALBUM_ARTIST
#define MN_SORT_GENRE        MNDB_SORT_GENRE
#define MN_SORT_YEAR         MNDB_SORT_YEAR
#define MN_SORT_TRACK        MNDB_SORT_TRACK
#define MN_SORT_DURATION     MNDB_SORT_DURATION
#define MN_SORT_DATE_ADDED   MNDB_SORT_DATE_ADDED
#define MN_SORT_DATE_CREATED MNDB_SORT_DATE_CREATED
#define MN_SORT_LAST_PLAYED  MNDB_SORT_LAST_PLAYED
#define MN_SORT_PLAY_COUNT   MNDB_SORT_PLAY_COUNT
#define MN_SORT_RATING       MNDB_SORT_RATING
#define MN_SORT_BITRATE      MNDB_SORT_BITRATE
#define MN_SORT_PATH         MNDB_SORT_PATH
#define MN_SORT_RELEVANCE    MNDB_SORT_RELEVANCE
#include "library_db.h"
#undef MN_OK
#undef MN_ERR_GENERIC
#undef MN_ERR_NOMEM
#undef MN_ERR_INVALID
#undef MN_ERR_NOTFOUND
#undef MN_ERR_BUSY
#undef MN_ERR_IO
#undef MN_ERR_CORRUPT
#undef MN_ERR_CONSTRAINT
#undef MN_ERR_MIGRATE
#undef MN_ERR_RANGE
#undef MN_ERR_STATE
#undef MN_SORT_NONE
#undef MN_SORT_TITLE
#undef MN_SORT_ARTIST
#undef MN_SORT_ALBUM
#undef MN_SORT_ALBUM_ARTIST
#undef MN_SORT_GENRE
#undef MN_SORT_YEAR
#undef MN_SORT_TRACK
#undef MN_SORT_DURATION
#undef MN_SORT_DATE_ADDED
#undef MN_SORT_DATE_CREATED
#undef MN_SORT_LAST_PLAYED
#undef MN_SORT_PLAY_COUNT
#undef MN_SORT_RATING
#undef MN_SORT_BITRATE
#undef MN_SORT_PATH
#undef MN_SORT_RELEVANCE

/* The contract we implement — original names intact. */
#include "app.h"

/* Library sync (SYNC_PROTOCOL v1). Self-contained header: it only forward-
 * declares mn_library, so it is safe amid the enum aliasing above. */
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Cross-thread serialization for the WHOLE controller. The app is driven from
 * several threads at once:
 *   - the MAIN thread: WM_TIMER -> mn_app_tick (playback auto-advance, which
 *     can LOAD the next track into the engine),
 *   - the CEF UI thread: every JS bridge command (play_row/next/prev/seek/
 *     stems toggles/queries/add_folder/...),
 *   - scanner worker threads: mn_app_on_track (library upserts).
 * None of the subsystems below (playback queue/engine device reinit/query
 * cursor/SQLite connection) tolerates concurrent control-side use, so ONE
 * recursive lock is taken at the top of every public mn_app_* entry point.
 * Recursive (Win32 CRITICAL_SECTION semantics) so nested internal calls are
 * fine. It is NEVER taken from the audio data callback, and it is NEVER held
 * while joining scanner workers (they need it inside on_track — deadlock). */
#ifdef _WIN32
#include <windows.h>
#include <direct.h>   /* _mkdir */
typedef CRITICAL_SECTION mn_lock;
#define MN_LOCK_INIT(l)  InitializeCriticalSection(l)
#define MN_LOCK_FREE(l)  DeleteCriticalSection(l)
#define MN_LOCK(l)       EnterCriticalSection(l)
#define MN_UNLOCK(l)     LeaveCriticalSection(l)
#else
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
typedef pthread_mutex_t mn_lock;
/* Recursive so nested library calls on the same thread (e.g. album_window ->
 * art_path) don't self-deadlock — matches Win32 CRITICAL_SECTION semantics. */
static void mn_lock_init_recursive(pthread_mutex_t *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
#define MN_LOCK_INIT(l)  mn_lock_init_recursive(l)
#define MN_LOCK_FREE(l)  pthread_mutex_destroy(l)
#define MN_LOCK(l)       pthread_mutex_lock(l)
#define MN_UNLOCK(l)     pthread_mutex_unlock(l)
#endif

/* ================================================================== */
/* Tunables / small helpers                                           */
/* ================================================================== */

/* Commit the write transaction every this many upserts during a scan so
 * the library populates live as the scan proceeds. */
#define MN_SCAN_COMMIT_EVERY 32

/* How many rows we are willing to build into a playback queue when the
 * user starts a track from the current window. Bounds the work done on a
 * single play command for a 1M-track library. */
#define MN_QUEUE_BUILD_MAX 5000

/* Maximum registered library roots. */
#define MN_MAX_ROOTS 64

/* Model / cache sub-paths under data_dir. */
#define MN_STEM_MODEL_SUBPATH "ai-models/htdemucs_6s.onnx"
#define MN_STEM_CACHE_SUBPATH "stem-cache"
#define MN_ART_CACHE_SUBPATH  "art-cache"
#define MN_DB_SUBPATH         "library.db"

/* Bundled default model filenames (returned when nothing is persisted). */
#define MN_STEM_MODEL_DEFAULT_FILE  "htdemucs_6s.onnx"
#define MN_DEPTH_MODEL_DEFAULT_FILE "depth_anything_v2_small.onnx"

/* Bounded copy into a fixed char buffer, always NUL-terminated. `src`
 * may be NULL (treated as ""). */
static void mn_copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    /* snprintf always NUL-terminates within cap. */
    snprintf(dst, cap, "%s", src);
}

/* ASCII case-insensitive compare (0 == equal). Used only to disambiguate an
 * album/artist match among FTS results; not a Unicode collation. */
static int mn_strcasecmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
}

/* Join a directory and a relative sub-path with a forward slash. */
static void mn_path_join(char *dst, size_t cap, const char *dir, const char *sub)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!dir) {
        dir = ".";
    }
    snprintf(dst, cap, "%s/%s", dir, sub);
}

/* ================================================================== */
/* Controller state                                                   */
/* ================================================================== */

struct mn_app {
    /* Subsystems (owned). */
    mn_library *lib;
    mn_engine  *engine;
    mn_playback*pb;
    mn_stems   *stems;
    mn_scanner *scanner;      /* current/last scan; NULL if none started */

    /* Scratch arena used to build query strings / small allocations. */
    mn_arena    arena;

    /* Data / cache directories. */
    char data_dir[MN_STR_PATH];
    char art_cache_dir[MN_STR_PATH];
    char stem_cache_dir[MN_STR_PATH];
    char model_path[MN_STR_PATH];

    /* Registered library roots (for rescan). */
    char roots[MN_MAX_ROOTS][MN_STR_PATH];
    int  root_count;

    /* ---- Browsing state ---- */
    mn_view view;
    mn_sort sort_key;
    bool    sort_asc;
    char    search[MN_STR_SHORT * 2];   /* raw user query text */

    /* Lazily (re)built query for the current view/search/sort. `dirty`
     * forces a rebuild + count refresh on the next access. */
    mn_query *query;
    bool      query_dirty;
    int64_t   row_count_cache;

    /* ---- Folder visibility (persisted in <data_dir>/hidden_folders.txt) --- */
    int64_t hidden_folders[MN_MAX_EXCLUDED_FOLDERS];
    int     hidden_folder_count;
    char    hidden_file[MN_STR_PATH];

    /* ---- Category scoping: per-kind libraries ----
     * kroot_* = every library root whose content-kind is NOT "music"
     * (audiobook, podcast, user-defined designations like "ost"), pushed by
     * the host from folder_kinds.txt. active_kind selects which library
     * every browse/search surface sees: "" = music (all kroots excluded);
     * "<kind>" = ONLY roots of that kind. */
    char    active_kind[32];
    int     kroot_len;
    char    kroot_kind[MN_MAX_KIND_ROOTS][32];
    char    kroot_path[MN_MAX_KIND_ROOTS][512];

    /* ---- Scan bookkeeping ---- */
    bool     scanning;
    int64_t  scan_pending;      /* upserts since last commit boundary   */
    int64_t  scan_committed;    /* total committed (for dirty trigger)  */
    char     scan_source[MN_STR_PATH];

    /* ---- Fast is_known index (built once per scan from one bulk query) ----
     * Open-addressing hash of every indexed path -> {mtime,size}, so the
     * scanner answers is_known() from RAM instead of a per-file locked DB
     * point query. Rebuilt at scan start; read lock-free by worker threads
     * (only mutated on the control thread before workers run). */
    struct mn_scan_ent { uint64_t hash; int64_t mtime; int64_t size;
                         char *path; } *scan_idx;
    size_t   scan_idx_cap;      /* power of two; 0 = not built            */
    size_t   scan_idx_len;

    /* ---- Now-playing cached display strings (of the active track) ---- */
    char now_title[MN_STR_SHORT];
    char now_artist[MN_STR_SHORT];
    char now_album[MN_STR_SHORT];
    char now_album_artist[MN_STR_SHORT]; /* album artist (art-cache key half) */
    int32_t now_src_rate;                /* tag-scanned SOURCE sample rate    */
    int32_t now_src_bits;                /* tag-scanned SOURCE bit depth      */
    int32_t now_src_kbps;                /* tag-scanned SOURCE bitrate (kbps) */
    char    now_src_format[MN_STR_SHORT];/* tag-scanned codec label           */
    int64_t now_track_id;
    int64_t now_album_id;                /* albums-dimension id (0 none)  */
    char    now_path[MN_STR_PATH];       /* playing track's file path     */
    int32_t now_liked;                   /* thumbs state of the active track */

    /* ---- Online session (internet radio / streamed podcast) ---- */
    /* When active, the ENGINE is driven directly (mn_playback is stopped
     * and its queue left untouched); the now-snapshot override below is
     * what the transport renders. */
    struct {
        bool     active;
        char     kind[12];               /* "radio" | "podcast" | "stream" */
        char     title[MN_STR_SHORT];    /* station / episode title        */
        char     artist[MN_STR_SHORT];   /* subtitle / feed title          */
        char     url[MN_STR_PATH];
        char     art[MN_STR_PATH];       /* favicon / show artwork URL     */
        char     icy[MN_STR_SHORT];      /* latest StreamTitle             */
        uint32_t icy_seq;
    } online;

    /* ---- Mixer / mode state we own on the app side ---- */
    float   volume;
    bool    stems_enabled;
    bool    stems_passthrough;

    /* ---- Settings ---- */
    mn_settings settings;

    /* ---- DSP shadow state (the engine has no getters for these; the EQ
     * modal restores from here so balance/limiter/master reflect reality) ---- */
    float dsp_balance;       /* -1..1, 0 = center                            */
    int   dsp_limiter_on;
    float dsp_limiter_thresh_db;
    float dsp_limiter_ceil_db;
    float dsp_master_db;     /* master gain in dB, 0 = unity                 */

    /* ---- Sleep timer ---- */
    uint64_t sleep_deadline_ms;          /* GetTickCount64 deadline; 0 = off */

    /* ---- Liked-only browse filter (the "Liked songs" smart list) ---- */
    bool liked_only;

    /* ---- Album browse cache ----
     * The albums grid pages over a FULL per-album detail array (artist, year,
     * size sum, art resolve) that is expensive to rebuild per page. Build it
     * once and serve every window as a memcpy slice; every site that marks
     * query_dirty also clears alb_cache_valid (library/search/sort changed). */
    mn_album *alb_cache;
    int32_t   alb_cache_n;      /* albums with FULL detail in the cache      */
    int32_t   alb_cache_total;  /* true album count (may exceed alb_cache_n) */
    bool      alb_cache_valid;
    /* The ORDER the cache is currently sorted in. album_window re-sorts the
     * cached array (qsort, ~µs at 2k) when the app sort key/direction moved
     * away from this — the dropdown + asc/desc finally apply to albums. */
    mn_sort   alb_sorted_key;
    bool      alb_sorted_asc;

    /* Scratch buffer returned by mn_app_art_path (valid until next tick). */
    char art_path_scratch[MN_STR_PATH];

    /* ---- Async stems-model loader (window shows instantly; the ~136 MB
     * ONNX session builds on this background thread and is published to
     * app->stems under the lock once ready). ---- */
#ifdef _WIN32
    HANDLE  stems_loader;        /* NULL when no loader thread            */
#else
    pthread_t stems_loader;
    bool     stems_loader_valid;
#endif
    bool     stems_loading;      /* loader still in flight                */
    bool     shutting_down;      /* destroy() in progress: abandon loads  */

    /* ---- Post-scan missing-file reconcile thread ---- */
#ifdef _WIN32
    HANDLE  reconcile_thread;    /* NULL when idle                        */
#else
    pthread_t reconcile_thread;
    bool     reconcile_valid;
#endif
    bool     reconciling;        /* a reconcile pass is in flight         */

    /* Serializes ALL mn_library access across the scanner worker threads and
     * the UI thread (see mn_lock above). */
    mn_lock lib_lock;
};

/* ================================================================== */
/* Enum mapping helpers                                               */
/* ================================================================== */

/* Map the app's sort key onto the library's sort key. Unsupported keys
 * fall back to something sensible per the app.h contract. */
static mn_sort_key mn_map_sort(mn_sort key)
{
    switch (key) {
        case MN_SORT_TITLE:      return MNDB_SORT_TITLE;
        case MN_SORT_ARTIST:     return MNDB_SORT_ARTIST;
        case MN_SORT_ALBUM:      return MNDB_SORT_ALBUM;
        case MN_SORT_GENRE:      return MNDB_SORT_GENRE;
        case MN_SORT_YEAR:       return MNDB_SORT_YEAR;
        case MN_SORT_DURATION:   return MNDB_SORT_DURATION;
        case MN_SORT_RATING:     return MNDB_SORT_RATING;
        case MN_SORT_PLAY_COUNT: return MNDB_SORT_PLAY_COUNT;
        case MN_SORT_DATE_ADDED: return MNDB_SORT_DATE_ADDED;
        case MN_SORT_TRACK_NO:   return MNDB_SORT_TRACK;
        case MN_SORT_LAST_PLAYED: return MNDB_SORT_LAST_PLAYED;
        case MN_SORT_DATE_CREATED: return MNDB_SORT_DATE_CREATED;
        default:                 return MNDB_SORT_TITLE;
    }
}

/* Map the app repeat enum to the playback controller's repeat mode. */
static mn_repeat_mode mn_map_repeat(mn_repeat r)
{
    switch (r) {
        case MN_REPEAT_ALL: return MNP_REPEAT_ALL;
        case MN_REPEAT_ONE: return MNP_REPEAT_ONE;
        case MN_REPEAT_OFF:
        default:            return MNP_REPEAT_OFF;
    }
}

/* Map the playback controller's repeat mode back to the app enum. */
static mn_repeat mn_unmap_repeat(mn_repeat_mode m)
{
    switch (m) {
        case MNP_REPEAT_ALL: return MN_REPEAT_ALL;
        case MNP_REPEAT_ONE: return MN_REPEAT_ONE;
        case MNP_REPEAT_OFF:
        default:             return MN_REPEAT_OFF;
    }
}

/* ================================================================== */
/* Folder-visibility persistence                                      */
/* ================================================================== */

/* One folder id per line in <data_dir>/hidden_folders.txt. Ids are stable
 * primary keys of the db's `folders` dimension table (same db file, same
 * data_dir), so persisting the raw ids is safe across restarts/rescans. */
static void mn_app_load_hidden(mn_app *app)
{
    FILE *f;
    char  line[64];

    app->hidden_folder_count = 0;
    f = fopen(app->hidden_file, "r");
    if (!f) {
        return;
    }
    while (app->hidden_folder_count < MN_MAX_EXCLUDED_FOLDERS
           && fgets(line, sizeof(line), f)) {
        long long id = atoll(line);
        if (id > 0) {
            app->hidden_folders[app->hidden_folder_count++] = (int64_t)id;
        }
    }
    fclose(f);
}

static void mn_app_save_hidden(mn_app *app)
{
    FILE *f = fopen(app->hidden_file, "w");
    int   i;

    if (!f) {
        return;
    }
    for (i = 0; i < app->hidden_folder_count; ++i) {
        fprintf(f, "%lld\n", (long long)app->hidden_folders[i]);
    }
    fclose(f);
}

/* True if folder_id is in the hidden set. Caller holds the app lock. */
static bool mn_app_hidden_contains(const mn_app *app, int64_t folder_id)
{
    int i;
    for (i = 0; i < app->hidden_folder_count; ++i) {
        if (app->hidden_folders[i] == folder_id) {
            return true;
        }
    }
    return false;
}

/* Stamp category scoping for an EXPLICIT kind onto a spec ("" = the default
 * music library). Factored out of mn_spec_apply_category so kind-scoped
 * maintenance surfaces (the art verifier / --arttest) can audit ANY kind's
 * derivation without touching the user's live active_kind. Caller holds the
 * app lock. */
static void mn_spec_apply_category_kind(const mn_app *app,
                                        mn_filter_spec *spec,
                                        const char *kind)
{
    int i, out = 0;
    int n = app->kroot_len;
    if (n > MN_MAX_KIND_ROOTS) n = MN_MAX_KIND_ROOTS;
    if (!kind) kind = "";

    if (kind[0]) {
        /* a named library: ONLY roots of that kind */
        for (i = 0; i < n && out < MN_MAX_KIND_ROOTS; i++) {
            if (_stricmp(app->kroot_kind[i], kind) != 0) continue;
            mn_copy_str(spec->kind_roots[out], sizeof(spec->kind_roots[out]),
                        app->kroot_path[i]);
            out++;
        }
        spec->kind_include = true;
        if (out == 0) {
            /* named library with no roots: empty view, never a leak of the
             * music library (builder emits AND 0 for this shape) */
            spec->kind_roots_len = 1;
            spec->kind_roots[0][0] = '\0';
            return;
        }
        spec->kind_roots_len = out;
    } else if (n > 0) {
        /* music (default): EXCLUDE every non-music root */
        for (i = 0; i < n && out < MN_MAX_KIND_ROOTS; i++) {
            mn_copy_str(spec->kind_roots[out], sizeof(spec->kind_roots[out]),
                        app->kroot_path[i]);
            out++;
        }
        spec->kind_include   = false;
        spec->kind_roots_len = out;
    }
}

/* Stamp category (music vs audiobooks) scoping onto a spec. Called from
 * mn_spec_apply_hidden so EVERY browse/search surface inherits it — this is
 * what keeps audiobook results out of music search and vice versa. Caller
 * holds the app lock. */
static void mn_spec_apply_category(const mn_app *app, mn_filter_spec *spec)
{
    mn_spec_apply_category_kind(app, spec, app->active_kind);
}

/* Hidden-folder exclusion WITHOUT the category stamp — for kind-agnostic
 * maintenance surfaces (art verify/extraction) that must see every library.
 * mn_spec_apply_hidden below re-applies the ACTIVE KIND scoping, which in a
 * live session silently emptied cascade/FTS queries for every non-active-kind
 * album (the run-2 regression: ~2400 bogus NONE verdicts under ""-artist
 * keys). Caller holds the app lock. */
static void mn_spec_apply_hidden_only(const mn_app *app, mn_filter_spec *spec)
{
    int n = app->hidden_folder_count;
    if (n <= 0) {
        spec->excluded_folders_len = 0;
        return;
    }
    if (n > MN_MAX_EXCLUDED_FOLDERS) {
        n = MN_MAX_EXCLUDED_FOLDERS;
    }
    memcpy(spec->excluded_folders, app->hidden_folders,
           (size_t)n * sizeof(spec->excluded_folders[0]));
    spec->excluded_folders_len = n;
}

/* Stamp the hidden-folder exclusion set onto a filter spec so hidden
 * folders vanish from whatever query/facet consumes it. */
static void mn_spec_apply_hidden(const mn_app *app, mn_filter_spec *spec)
{
    int n = app->hidden_folder_count;
    /* category scoping rides the same template so every surface (tracks,
     * albums, facets, search, suggestions) is isolated per kind. MUST run
     * BEFORE the no-hidden-folders early return — the common case has zero
     * hidden folders and the stamp was silently skipped. */
    mn_spec_apply_category(app, spec);
    if (n <= 0) {
        spec->excluded_folders_len = 0;
        return;
    }
    if (n > MN_MAX_EXCLUDED_FOLDERS) {
        n = MN_MAX_EXCLUDED_FOLDERS;
    }
    memcpy(spec->excluded_folders, app->hidden_folders,
           (size_t)n * sizeof(spec->excluded_folders[0]));
    spec->excluded_folders_len = n;
}

/* Push the audiophile profile to the engine: full quality for music, the
 * power-saving caps ONLY while the audiobook library is active. Caller holds
 * the app lock. */
void mn_app_apply_hifi_profile_locked(mn_app *app)
{
    int      native;
    uint32_t rate_cap = 0, bits_cap = 0;
    if (!app || !app->engine) return;
    native = app->settings.hifi_native_bits ? 1 : 0;
    if (_stricmp(app->active_kind, "audiobook") == 0) {
        rate_cap = app->settings.ab_rate_cap_hz > 0 ? (uint32_t)app->settings.ab_rate_cap_hz : 48000u;
        bits_cap = app->settings.ab_bits_cap    > 0 ? (uint32_t)app->settings.ab_bits_cap    : 16u;
    }
    mn_engine_set_hifi_profile(app->engine, native, rate_cap, bits_cap);
}

void mn_app_set_category_kind(mn_app *app, const char *kind)
{
    if (!app) return;
    if (!kind) kind = "";
    MN_LOCK(&app->lib_lock);
    if (_stricmp(app->active_kind, kind) != 0) {
        mn_copy_str(app->active_kind, sizeof(app->active_kind), kind);
        app->query_dirty = true; app->alb_cache_valid = false;
        mn_app_apply_hifi_profile_locked(app);   /* audiobook power profile */
    }
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_set_kind_roots(mn_app *app, const char kinds[][32],
                           const char paths[][512], int n)
{
    int i;
    if (!app) return;
    if (n > MN_MAX_KIND_ROOTS) n = MN_MAX_KIND_ROOTS;
    if (n < 0) n = 0;
    MN_LOCK(&app->lib_lock);
    app->kroot_len = n;
    for (i = 0; i < n; i++) {
        mn_copy_str(app->kroot_kind[i], sizeof(app->kroot_kind[i]), kinds[i]);
        mn_copy_str(app->kroot_path[i], sizeof(app->kroot_path[i]), paths[i]);
    }
    /* Guard against a dead-end: if we're viewing a NAMED kind whose last folder
     * was just removed, none of the new roots match it → the view would show
     * an empty phantom library forever. Fall back to music (active_kind="").  */
    if (app->active_kind[0]) {
        int have = 0;
        for (i = 0; i < n; i++) {
            if (_stricmp(app->kroot_kind[i], app->active_kind) == 0) { have = 1; break; }
        }
        if (!have) {
            app->active_kind[0] = '\0';
            mn_app_apply_hifi_profile_locked(app);   /* leave audiobook power caps */
        }
    }
    app->query_dirty = true; app->alb_cache_valid = false;
    MN_UNLOCK(&app->lib_lock);
}

/* ================================================================== */
/* Filter spec construction                                           */
/* ================================================================== */

/* Build an mn_filter_spec from the current browsing state. The fts_match
 * string, when present, points into `app->search` and is valid for the
 * lifetime of the call chain that consumes the spec (mn_query_open copies
 * it internally). */
static void mn_build_spec(mn_app *app, mn_filter_spec *spec)
{
    memset(spec, 0, sizeof(*spec));

    if (app->search[0] != '\0') {
        spec->fts_match = app->search;
    }

    spec->sort[0].key = mn_map_sort(app->sort_key);
    spec->sort[0].descending = !app->sort_asc;
    spec->sort_len = 1;

    /* MediaMonkey-style secondary keys so ties land in listening order
     * instead of scan order: Artist -> album -> disc/track, Album ->
     * disc/track, Year/Genre -> album -> disc/track, Title -> artist.
     * Secondaries always ascend; only the primary follows sort_asc. */
    {
        int n = 1;
        switch (app->sort_key) {
            case MN_SORT_ARTIST:
                spec->sort[n].key = MNDB_SORT_ALBUM; spec->sort[n++].descending = false;
                spec->sort[n].key = MNDB_SORT_TRACK; spec->sort[n++].descending = false;
                break;
            case MN_SORT_ALBUM:
                spec->sort[n].key = MNDB_SORT_TRACK; spec->sort[n++].descending = false;
                break;
            case MN_SORT_YEAR:
            case MN_SORT_GENRE:
                spec->sort[n].key = MNDB_SORT_ALBUM; spec->sort[n++].descending = false;
                spec->sort[n].key = MNDB_SORT_TRACK; spec->sort[n++].descending = false;
                break;
            case MN_SORT_TITLE:
                spec->sort[n].key = MNDB_SORT_ARTIST; spec->sort[n++].descending = false;
                break;
            default:
                break;
        }
        spec->sort_len = n;
    }

    /* Missing rows STAY VISIBLE (dimmed + MISSING pill in the UI) so the
     * user can see and purge them; row JSON carries the flag. */
    spec->include_missing = true;
    spec->min_rating_x2 = 0;
    spec->liked_only = app->liked_only;

    /* Hidden folders vanish from every browse/search surface. */
    mn_spec_apply_hidden(app, spec);
}

/* ==================================================================== */
/* Playlist-file import (M3U / M3U8 / PLS found in the library folders) */
/* ==================================================================== */

static void mn_app_seed_roots_locked(mn_app *app);   /* defined below */

/* True when `name` (UTF-8) ends with one of the playlist extensions. */
static bool mn_is_playlist_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return _stricmp(dot, ".m3u") == 0 || _stricmp(dot, ".m3u8") == 0 ||
           _stricmp(dot, ".pls") == 0;
}

/* Resolve one playlist entry line to an absolute, backslashed path. */
static void mn_playlist_entry_abspath(const char *base_dir, const char *entry,
                                      char *out, size_t n)
{
    char tmp[MN_STR_PATH];
    size_t i;
    /* absolute forms: "C:\...", "C:/...", "\\server\..." */
    if ((entry[0] && entry[1] == ':') ||
        (entry[0] == '\\' && entry[1] == '\\')) {
        snprintf(tmp, sizeof(tmp), "%s", entry);
    } else {
        snprintf(tmp, sizeof(tmp), "%s\\%s", base_dir, entry);
    }
    for (i = 0; tmp[i] && i + 1 < n; ++i) {
        out[i] = (tmp[i] == '/') ? '\\' : tmp[i];
    }
    out[i] = '\0';
}

/* Import one playlist file as a static playlist named after the file.
 * Returns tracks added (0 when nothing resolved), -1 when skipped (already
 * imported / unreadable). Caller holds the app lock. */
static int mn_app_import_one_playlist_locked(mn_app *app, const char *file)
{
    char     name[MN_STR_SHORT];
    char     dir[MN_STR_PATH];
    FILE    *fp = NULL;
    char     line[MN_STR_PATH + 64];
    int64_t  pl_id = 0;
    int      added = 0;
    bool     is_pls;

    /* name = filename sans extension; dir = containing folder */
    {
        const char *slash = strrchr(file, '\\');
        const char *fn = slash ? slash + 1 : file;
        const char *dot = strrchr(fn, '.');
        size_t len = dot ? (size_t)(dot - fn) : strlen(fn);
        if (len == 0 || len >= sizeof(name)) return -1;
        memcpy(name, fn, len);
        name[len] = '\0';
        if (slash) {
            size_t dl = (size_t)(slash - file);
            if (dl >= sizeof(dir)) return -1;
            memcpy(dir, file, dl);
            dir[dl] = '\0';
        } else {
            dir[0] = '.'; dir[1] = '\0';
        }
        is_pls = (_stricmp(dot ? dot : "", ".pls") == 0);
    }

    /* skip when a playlist of this name already exists (idempotent rescan) */
    {
        mn_arena arena;
        const mn_playlist_row *rows = NULL;
        int32_t n = 0, i;
        bool dup = false;
        if (mn_arena_init(&arena, 8 * 1024) == MNDB_OK) {
            if (mn_playlist_list(app->lib, &arena, &rows, &n) == MNDB_OK && rows) {
                for (i = 0; i < n; ++i) {
                    if (rows[i].name && _stricmp(rows[i].name, name) == 0) {
                        dup = true;
                        break;
                    }
                }
            }
            mn_arena_free(&arena);
        }
        if (dup) return -1;
    }

    /* open as bytes; entries are UTF-8 (m3u8) or ANSI-ish (m3u) — the NOCASE
     * path lookup absorbs most of the difference on Windows paths */
    {
        wchar_t wfile[MN_STR_PATH];
        if (MultiByteToWideChar(CP_UTF8, 0, file, -1, wfile,
                                (int)(sizeof(wfile) / sizeof(wfile[0]))) <= 0) {
            return -1;
        }
        fp = _wfopen(wfile, L"rb");
        if (!fp) return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *s = line;
        char  abspath[MN_STR_PATH];
        int64_t tid = 0;
        size_t  ln;

        /* trim leading whitespace + UTF-8 BOM, trailing CR/LF/space */
        while (*s == ' ' || *s == '\t') s++;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) {
            s += 3;
        }
        ln = strlen(s);
        while (ln > 0 && (s[ln-1] == '\n' || s[ln-1] == '\r' ||
                          s[ln-1] == ' '  || s[ln-1] == '\t')) {
            s[--ln] = '\0';
        }
        if (!s[0]) continue;

        if (is_pls) {
            /* PLS: only "FileN=<path>" lines carry entries. */
            if (_strnicmp(s, "File", 4) != 0) continue;
            {
                char *eq = strchr(s, '=');
                if (!eq) continue;
                s = eq + 1;
            }
            if (!s[0]) continue;
        } else {
            if (s[0] == '#') continue;   /* EXTM3U/EXTINF/comments */
        }

        mn_playlist_entry_abspath(dir, s, abspath, sizeof(abspath));
        if (mn_library_track_id_by_path(app->lib, abspath, &tid) != MNDB_OK ||
            tid <= 0) {
            continue;   /* not in the library (moved/deleted/streaming URL) */
        }
        if (pl_id == 0) {
            if (mn_playlist_create(app->lib, name, &pl_id) != MNDB_OK ||
                pl_id <= 0) {
                break;
            }
        }
        if (mn_playlist_add(app->lib, pl_id, tid, NULL) == MNDB_OK) {
            added++;
        }
    }
    fclose(fp);
    return added;
}

/* Recursive sweep of a directory for playlist files (bounded depth). */
static void mn_app_import_playlist_dir_locked(mn_app *app, const char *dir,
                                              int depth, int *playlists_added)
{
    wchar_t wpattern[MN_STR_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char pattern[MN_STR_PATH];

    if (depth > 6) return;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern,
                            (int)(sizeof(wpattern) / sizeof(wpattern[0]))) <= 0) {
        return;
    }
    h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[MN_STR_PATH];
        char full[MN_STR_PATH];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                                sizeof(name), NULL, NULL) <= 0) {
            continue;
        }
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        if ((int)snprintf(full, sizeof(full), "%s\\%s", dir, name)
                >= (int)sizeof(full)) {
            continue;   /* path too deep to represent — skip */
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            mn_app_import_playlist_dir_locked(app, full, depth + 1,
                                              playlists_added);
        } else if (mn_is_playlist_file(name)) {
            if (mn_app_import_one_playlist_locked(app, full) > 0) {
                (*playlists_added)++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Sweep every library root for .m3u/.m3u8/.pls files and import each as a
 * static playlist (named after the file, entries resolved against the DB by
 * path). Idempotent: a playlist whose name already exists is skipped, so
 * rescans don't duplicate. Returns the number of playlists created. */
int mn_app_import_playlists(mn_app *app)
{
    int made = 0;
    int i;

    if (!app || !app->lib) return 0;
    MN_LOCK(&app->lib_lock);
    mn_app_seed_roots_locked(app);
    for (i = 0; i < app->root_count; ++i) {
        mn_app_import_playlist_dir_locked(app, app->roots[i], 0, &made);
    }
    MN_UNLOCK(&app->lib_lock);
    return made;
}

/* Expose the cache directories for the settings "storage" panel. Any out
 * pointer may be NULL. */
void mn_app_cache_paths(mn_app *app, char *art, char *stems, char *models,
                        size_t cap)
{
    if (!app) return;
    if (art)    mn_copy_str(art, cap, app->art_cache_dir);
    if (stems)  mn_copy_str(stems, cap, app->stem_cache_dir);
    if (models) mn_app_models_dir(app, models, cap);
}

/* Replace an album's cached art from an arbitrary image file (online fetch).
 * Rebuilds the 256 thumb + hi-res companion under the album key; the caller
 * is responsible for invalidating webart PNGs + depth maps. */
bool mn_app_ingest_album_art(mn_app *app, const char *artist,
                             const char *album, const char *image_path)
{
    char key[MN_STR_SHORT * 2];
    char out[MN_ART_PATH_MAX];
    bool ok;
    if (!app || !album || !album[0] || !image_path || !image_path[0]) {
        return false;
    }
    snprintf(key, sizeof(key), "%s\x1f%s", artist ? artist : "", album);
    MN_LOCK(&app->lib_lock);
    ok = mn_art_ingest_image(app->art_cache_dir, key, image_path,
                             out, sizeof(out));
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

/* Liked-only browse filter ("Liked songs"): filters every track window at
 * the DB layer, so the list is complete regardless of paging. */
void mn_app_set_liked_only(mn_app *app, bool on)
{
    if (!app) return;
    MN_LOCK(&app->lib_lock);
    if (app->liked_only != on) {
        app->liked_only  = on;
        app->query_dirty = true; app->alb_cache_valid = false;
    }
    MN_UNLOCK(&app->lib_lock);
}

/* (Re)build the compiled query for the current view/search/sort if it has
 * been marked dirty, refreshing the cached row count. Safe to call every
 * access; cheap when clean. Returns MNDB_OK on success. */
static mn_status mn_refresh_query(mn_app *app)
{
    mn_filter_spec spec;
    mn_query      *q = NULL;
    mn_status      st;

    if (!app || !app->lib) {
        return MNDB_ERR_INVALID;
    }
    if (!app->query_dirty && app->query) {
        return MNDB_OK;
    }

    mn_build_spec(app, &spec);

    st = mn_query_open(app->lib, &spec, &q);
    if (st != MNDB_OK || !q) {
        return (st != MNDB_OK) ? st : MNDB_ERR_GENERIC;
    }

    /* Swap in the fresh query; discard the old one. */
    if (app->query) {
        mn_query_close(app->query);
    }
    app->query = q;
    app->query_dirty = false;

    app->row_count_cache = 0;
    (void)mn_query_count(app->query, &app->row_count_cache);

    return MNDB_OK;
}

/* Copy a db track row into an app-facing mn_row with bounded copies. */
static void mn_fill_row(mn_row *out, const mn_track_row *src)
{
    memset(out, 0, sizeof(*out));
    out->id = src->id;
    mn_copy_str(out->title,  sizeof(out->title),  src->title);
    mn_copy_str(out->artist, sizeof(out->artist), src->artist);
    /* album_artist is the art-cache key half (scan-time key is
     * "<album_artist-or-artist>\x1f<album>"); without it, VA/compilation
     * track rows hashed art on the per-track artist and always missed. */
    mn_copy_str(out->album_artist, sizeof(out->album_artist),
                src->album_artist);
    mn_copy_str(out->album,  sizeof(out->album),  src->album);
    mn_copy_str(out->genre,  sizeof(out->genre),  src->genre);
    mn_copy_str(out->path,   sizeof(out->path),   src->path);
    out->duration_ms = (int32_t)src->duration_ms;
    out->year        = src->year;
    out->track_no    = src->track;
    out->disc_no     = src->disc;
    out->rating      = src->rating_x2 / 2;   /* 0..10 half-stars -> 0..5 */
    out->liked       = src->liked;
    out->play_count  = src->play_count;
    out->bitrate_kbps = src->bitrate_kbps;
    out->size         = src->size;
    out->date_added   = src->date_added;
    out->missing      = src->missing;
}

/* ================================================================== */
/* File stat helpers (missing / modified detection)                   */
/* ================================================================== */

/* Fetch a file's mtime (unix seconds) + size. The Windows FILETIME->unix
 * conversion MUST match scanner.c's walker exactly, or the is_known
 * equality check below would never hold and rescans would re-read
 * everything: (100ns_ticks / 10^7) - 11644473600. */
static bool mn_app_stat_file2(const char *path, int64_t *mtime, int64_t *size,
                              int64_t *created)
{
    if (mtime)   *mtime   = 0;
    if (size)    *size    = 0;
    if (created) *created = 0;
#ifdef _WIN32
    {
        wchar_t wpath[MN_STR_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath,
                                (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0) {
            return false;
        }
        if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) {
            return false;
        }
        {
            ULARGE_INTEGER li;
            li.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
            li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            if (mtime) *mtime = (int64_t)(li.QuadPart / 10000000ULL) - 11644473600LL;
            li.LowPart  = fad.ftCreationTime.dwLowDateTime;
            li.HighPart = fad.ftCreationTime.dwHighDateTime;
            if (created) *created = (int64_t)(li.QuadPart / 10000000ULL) - 11644473600LL;
            li.LowPart  = fad.nFileSizeLow;
            li.HighPart = fad.nFileSizeHigh;
            if (size) *size = (int64_t)li.QuadPart;
        }
        return true;
    }
#else
    {
        struct stat sst;
        if (stat(path, &sst) != 0) return false;
        if (mtime) *mtime = (int64_t)sst.st_mtime;
        if (size)  *size  = (int64_t)sst.st_size;
        /* POSIX has no portable birth time; ctime is the honest floor. */
        if (created) *created = (int64_t)sst.st_ctime;
        return true;
    }
#endif
}

static bool mn_app_stat_file(const char *path, int64_t *mtime, int64_t *size)
{
    return mn_app_stat_file2(path, mtime, size, NULL);
}

/* Does the file exist (UTF-8 path)? */
static bool mn_app_file_exists(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[MN_STR_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0) {
        return false;
    }
    return GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

/* ================================================================== */
/* Filename/folder tag inference                                      */
/* ================================================================== */

/* MediaMonkey/Poweramp-style fallbacks for untagged files. NEVER
 * overwrites a real tag — each field is filled only when empty:
 *   title  <- filename stem, minus "NN[.- )]" prefix and "Artist - "
 *   artist <- the "Artist - Title" left side
 *   album  <- parent directory name (only for dirs >= 2 levels deep,
 *             so "D:\loose_file.mp3" doesn't invent album "D:")
 *   track  <- the leading numeric prefix
 * Underscores read as spaces (common in ripped files). */

typedef struct mn_inferred {
    char    title[256];
    char    artist[256];
    char    album[256];
    int32_t track;
} mn_inferred;

static void mn__infer_clean(char *s) {
    char *w = s, *r = s;
    bool sp = true;                  /* collapse + trim leading spaces */
    for (; *r; ++r) {
        char c = (*r == '_') ? ' ' : *r;
        if (c == ' ') {
            if (sp) continue;
            sp = true;
        } else {
            sp = false;
        }
        *w++ = c;
    }
    while (w > s && w[-1] == ' ') --w; /* trim trailing */
    *w = '\0';
}

static void mn_infer_from_path(const char *path, mn_inferred *out)
{
    char stem[512];
    const char *base = path, *p, *dot;
    size_t n;

    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return;

    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    n = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (n == 0) return;
    if (n >= sizeof(stem)) n = sizeof(stem) - 1;
    memcpy(stem, base, n); stem[n] = '\0';
    mn__infer_clean(stem);

    /* Leading track number: 1-3 digits + [.-)] or space separator. */
    {
        char *s = stem; int digits = 0, val = 0;
        while (s[digits] >= '0' && s[digits] <= '9' && digits < 3) {
            val = val * 10 + (s[digits] - '0'); digits++;
        }
        if (digits >= 1 && digits <= 3) {
            char *rest = s + digits;
            int sep = 0;
            while (*rest == '.' || *rest == '-' || *rest == ')' || *rest == ' ') {
                rest++; sep = 1;
            }
            if (sep && *rest) {
                out->track = val;
                memmove(stem, rest, strlen(rest) + 1);
            }
        }
    }

    /* "Artist - Title" split at the FIRST " - " (spaces required, so
     * hyphenated words like "Re-Up" survive). */
    {
        char *sep = strstr(stem, " - ");
        if (sep && sep != stem && sep[3]) {
            *sep = '\0';
            snprintf(out->artist, sizeof(out->artist), "%s", stem);
            snprintf(out->title,  sizeof(out->title),  "%s", sep + 3);
            mn__infer_clean(out->artist);
            mn__infer_clean(out->title);
        } else {
            snprintf(out->title, sizeof(out->title), "%s", stem);
        }
    }

    /* Album <- parent directory name, when the file is at least two
     * levels below the filesystem root (avoids "E:" / "MUSIC" albums
     * for loose root files; a real "...\Artist\Album\NN Title.mp3"
     * layout passes). Bracketed rip suffixes like "[FLAC]" drop. */
    {
        const char *dir_end = base > path ? base - 1 : NULL; /* at slash */
        const char *ds = NULL, *d;
        int seps = 0;
        for (d = path; d < (dir_end ? dir_end : path); ++d) {
            if (*d == '/' || *d == '\\') { ds = d; seps++; }
        }
        if (dir_end && ds && seps >= 2) {
            size_t an = (size_t)(dir_end - (ds + 1));
            if (an > 0 && an < sizeof(out->album)) {
                char *br;
                memcpy(out->album, ds + 1, an); out->album[an] = '\0';
                mn__infer_clean(out->album);
                br = strstr(out->album, " [");
                if (br && strchr(br, ']')) *br = '\0';
                /* Disc-folder names ("CD1", "Disc 2") are not albums. */
                if (!_strnicmp(out->album, "cd", 2) ||
                    !_strnicmp(out->album, "disc", 4) ||
                    !_strnicmp(out->album, "disk", 4)) {
                    const char *rest2 = out->album +
                        (!_strnicmp(out->album, "cd", 2) ? 2 : 4);
                    while (*rest2 == ' ') rest2++;
                    if (!*rest2 || (*rest2 >= '0' && *rest2 <= '9'))
                        out->album[0] = '\0';
                }
            }
        }
    }
}

/* Clear a folder-derived album when the file sits DIRECTLY inside a
 * registered scan root — "D:\Music\loose.mp3" must not invent the album
 * "Music". Caller holds the app lock (roots array). */
static void mn_infer_suppress_root_album(mn_app *app, const char *path,
                                         mn_inferred *inf)
{
    char dir[MN_STR_PATH];
    const char *p, *slash = NULL;
    size_t n;
    int i;
    if (!inf->album[0]) return;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) { inf->album[0] = '\0'; return; }
    n = (size_t)(slash - path);
    if (n >= sizeof(dir)) n = sizeof(dir) - 1;
    memcpy(dir, path, n); dir[n] = '\0';
    for (i = 0; i < app->root_count; ++i) {
        size_t rl = strlen(app->roots[i]);
        /* trim any trailing slash on the stored root for the compare */
        while (rl && (app->roots[i][rl-1] == '\\' || app->roots[i][rl-1] == '/'))
            rl--;
        if (rl == n && _strnicmp(app->roots[i], dir, n) == 0) {
            inf->album[0] = '\0';
            return;
        }
    }
}

/* ================================================================== */
/* Scan callback                                                      */
/* ================================================================== */

/*
 * on_track is invoked CONCURRENTLY from multiple scanner worker threads.
 * The library writer is internally serialized (mutex-guarded), so upserts
 * are safe; but the batch transaction and our scan counters are shared
 * mutable state. The library's begin/commit take the writer mutex for the
 * duration, so we let a single logical transaction span many upserts and
 * only cross a commit boundary based on a global counter. To keep this
 * correct under concurrency we serialize the callback body's bookkeeping
 * through the writer via begin/commit ordering; upsert itself is the
 * serialized primitive we rely on.
 *
 * Because begin()/commit() take the writer mutex and cannot be nested, we
 * guard the transaction lifecycle with the same discipline used by the
 * batch API: we hold one open transaction across the scan and commit at
 * count boundaries. The scanner may call this from several threads, so we
 * treat the transaction as best-effort batching — if a begin/commit races
 * it simply returns MNDB_ERR_STATE and the upsert still lands (autocommit).
 */
static void mn_app_on_track(void *user, const char *path, const mn_tags *tags)
{
    mn_app *app = (mn_app *)user;
    mn_track_in t;
    int64_t     id = 0;
    int64_t     pending;
    char        fmtbuf[12];

    if (!app || !path || !tags) {
        return;
    }

    memset(&t, 0, sizeof(t));
    t.path         = path;

    /* Format label: prefer the REAL codec the tag parser read from the
     * container magic bytes (.m4a -> ALAC/AAC, .ogg -> OPUS/VORBIS);
     * fall back to the uppercased extension for containers the parser
     * doesn't label (WMA/AIFF/APE/WV...). Powers the stats breakdown and
     * every format pill. */
    fmtbuf[0] = '\0';
    if (tags->codec[0]) {
        snprintf(fmtbuf, sizeof(fmtbuf), "%s", tags->codec);
    } else {
        const char *dot = strrchr(path, '.');
        if (dot && dot[1] && !strchr(dot, '\\') && !strchr(dot, '/')
            && strlen(dot + 1) < sizeof(fmtbuf)) {
            size_t k;
            for (k = 0; dot[1 + k]; ++k) {
                char c = dot[1 + k];
                fmtbuf[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            fmtbuf[k] = '\0';
        }
    }

    /* Real file mtime + size + creation time: powers the incremental
     * is_known skip on rescans, the modified-file re-read, and the
     * Date-created sort. Done before taking the lock (filesystem I/O). */
    (void)mn_app_stat_file2(path, &t.mtime, &t.size, &t.created);

    /* Detect cover-art availability (embedded OR folder sidecar) so the
     * has_art column is accurate. Previously hardcoded false, which left the
     * flag useless. Cheap probe (no decode/resize); done before the lock since
     * it is filesystem I/O. */
    bool track_has_art = mn_art_probe(path);

    /* Filename/folder inference for whatever the tags left empty (gated
     * by the Library setting; computed OUTSIDE the lock — pure string
     * work on `path`). */
    mn_inferred inf; bool use_inf;
    memset(&inf, 0, sizeof(inf));
    use_inf = app->settings.infer_tags &&
              (!tags->title[0] || !tags->artist[0] || !tags->album[0] ||
               tags->track_no == 0);
    if (use_inf) mn_infer_from_path(path, &inf);

    MN_LOCK(&app->lib_lock);
    if (use_inf) mn_infer_suppress_root_album(app, path, &inf);
    t.title        = tags->title[0]        ? tags->title
                   : (inf.title[0]  ? inf.title  : NULL);
    t.artist       = tags->artist[0]       ? tags->artist
                   : (inf.artist[0] ? inf.artist : NULL);
    t.album        = tags->album[0]        ? tags->album
                   : (inf.album[0]  ? inf.album  : NULL);
    t.album_artist = tags->album_artist[0] ? tags->album_artist : NULL;
    t.composer     = tags->composer[0]     ? tags->composer     : NULL;
    t.genre        = tags->genre[0]        ? tags->genre        : NULL;
    t.format       = fmtbuf[0] ? fmtbuf : NULL;
    t.year         = (int32_t)tags->year;
    t.track        = tags->track_no ? (int32_t)tags->track_no : inf.track;
    t.disc         = (int32_t)tags->disc_no;
    t.duration_ms  = (int64_t)tags->duration_ms;
    t.sample_rate  = (int32_t)tags->sample_rate;
    t.channels     = (int32_t)tags->channels;
    t.bit_depth    = (int32_t)tags->bit_depth;
    t.bitrate_kbps = (int32_t)tags->bitrate_kbps;
    /* t.size / t.mtime already stat'ed above. */
    t.has_art      = track_has_art;

    /* Moved/renamed detection: rows sharing this file's size+duration
     * whose OWN file has vanished from disk are the old location of a
     * move — repath the single unambiguous match so ratings/playcounts
     * survive; the upsert below then refreshes its tags in place. */
    if (t.size > 0 && t.duration_ms > 0) {
        mn_relink_cand cand[4];
        int nc = mn_library_relink_candidates(app->lib, path, t.size,
                                              t.duration_ms, cand, 4);
        if (nc > 0 && nc <= 4) {
            int i, gone_i = -1, gone_n = 0;
            for (i = 0; i < nc; ++i) {
                if (!mn_app_file_exists(cand[i].path)) {
                    gone_i = i; gone_n++;
                }
            }
            if (gone_n == 1) {
                (void)mn_library_repath_track(app->lib, cand[gone_i].id,
                                              path);
            }
        }
    }

    (void)mn_library_upsert_track(app->lib, &t, &id);

    /* Batch-commit bookkeeping: mark query dirty on commit boundaries so
     * the UI list populates live during a scan. Held under the same lock so
     * transaction state is consistent across worker threads + UI queries. */
    pending = ++app->scan_pending;
    if (pending >= MN_SCAN_COMMIT_EVERY) {
        app->scan_pending = 0;
        if (mn_library_in_transaction(app->lib)) {
            (void)mn_library_commit(app->lib);
        }
        (void)mn_library_begin(app->lib);
        app->scan_committed++;
        /* Live-view refresh, THROTTLED to ~1/s: unthrottled, every 32-track
         * commit re-ran the COUNT(*) + query rebuild (and worse, a full
         * album-cache rebuild if the user was in Albums) — the UI turned
         * sluggish for the whole scan. One refresh per second is visually
         * identical and keeps the lock contention flat. */
        {
            static ULONGLONG s_last_inval_ms = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_last_inval_ms >= 1000) {
                s_last_inval_ms = t;
                app->query_dirty = true; app->alb_cache_valid = false;
            }
        }
    }
    MN_UNLOCK(&app->lib_lock);

    /* Extract/cache cover art for this album (dedup by album key). Done
     * OUTSIDE the DB lock — it is heavy I/O and independently thread-safe. */
    {
        char key[MN_STR_SHORT * 2];
        char out[MN_ART_PATH_MAX];
        const char *aa = tags->album_artist[0] ? tags->album_artist
                       : (tags->artist[0]      ? tags->artist : "");
        const char *al = tags->album[0] ? tags->album : "";
        if (al[0] != '\0') {
            snprintf(key, sizeof(key), "%s\x1f%s", aa, al);
            (void)mn_art_ensure(app->art_cache_dir, key, path, out, sizeof(out));
        }
    }
}

/*
 * Incremental-scan predicate (scanner worker threads, concurrent): a file
 * is "known" — and its tag read skipped — when the db already has its path
 * with the SAME mtime and size and it is not flagged missing. A modified
 * file (mtime differs) therefore falls through to on_track, which re-reads
 * its tags and upserts the fresh values; a missing-flagged file that
 * reappeared also falls through so the upsert clears missing=0.
 */
/* FNV-1a over a NUL-terminated string. */
static uint64_t mn_scan_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h ? h : 1;   /* 0 reserved for empty slot */
}

/* Callback for mn_library_enumerate_paths: insert one row into the index. */
static void mn_scan_idx_put(void *user, const char *path,
                            int64_t mtime, int64_t size, bool missing) {
    mn_app *app = (mn_app *)user;
    if (missing || !path || !path[0] || !app->scan_idx || app->scan_idx_cap == 0)
        return;
    /* grow when load factor would exceed ~0.7 */
    if ((app->scan_idx_len + 1) * 10 >= app->scan_idx_cap * 7) {
        size_t ncap = app->scan_idx_cap * 2;
        struct mn_scan_ent *ni =
            (struct mn_scan_ent *)calloc(ncap, sizeof(*ni));
        if (ni) {
            size_t i;
            for (i = 0; i < app->scan_idx_cap; ++i) {
                if (app->scan_idx[i].hash) {
                    size_t m = ncap - 1, j = app->scan_idx[i].hash & m;
                    while (ni[j].hash) j = (j + 1) & m;
                    ni[j] = app->scan_idx[i];
                }
            }
            free(app->scan_idx);
            app->scan_idx = ni;
            app->scan_idx_cap = ncap;
        }
    }
    /* NEVER insert into a (nearly) full table: if the grow above failed
     * (OOM), an insert probe on a 100%-full table would spin forever. The
     * fallback point-query path in mn_app_is_known covers missing entries. */
    if ((app->scan_idx_len + 1) >= app->scan_idx_cap) return;
    {
        uint64_t hsh = mn_scan_hash(path);
        size_t m = app->scan_idx_cap - 1, j = hsh & m;
        while (app->scan_idx[j].hash) {
            if (app->scan_idx[j].hash == hsh && app->scan_idx[j].path &&
                strcmp(app->scan_idx[j].path, path) == 0) {
                app->scan_idx[j].mtime = mtime;   /* dup path: refresh */
                app->scan_idx[j].size  = size;
                return;
            }
            j = (j + 1) & m;
        }
        app->scan_idx[j].hash  = hsh;
        app->scan_idx[j].mtime = mtime;
        app->scan_idx[j].size  = size;
        app->scan_idx[j].path  = _strdup(path);
        app->scan_idx_len++;
    }
}

/* Build the in-memory is_known index from the DB (one bulk query). Control
 * thread only, called before the scan's worker threads start. */
static void mn_app_scan_index_build(mn_app *app) {
    size_t cap = 1024;
    int64_t rows;

    /* free any previous index */
    if (app->scan_idx) {
        size_t i;
        for (i = 0; i < app->scan_idx_cap; ++i) free(app->scan_idx[i].path);
        free(app->scan_idx);
        app->scan_idx = NULL;
    }
    app->scan_idx_cap = 0;
    app->scan_idx_len = 0;
    if (!app->lib) return;

    /* size the table to the row count (next pow2, min 1024) */
    rows = mn_app_row_count(app);
    while ((int64_t)cap < rows * 2 && cap < (1u << 24)) cap <<= 1;
    app->scan_idx = (struct mn_scan_ent *)calloc(cap, sizeof(struct mn_scan_ent));
    if (!app->scan_idx) return;
    app->scan_idx_cap = cap;

    MN_LOCK(&app->lib_lock);
    (void)mn_library_enumerate_paths(app->lib, mn_scan_idx_put, app);
    MN_UNLOCK(&app->lib_lock);
}

static void mn_app_scan_index_free(mn_app *app) {
    if (app->scan_idx) {
        size_t i;
        for (i = 0; i < app->scan_idx_cap; ++i) free(app->scan_idx[i].path);
        free(app->scan_idx);
        app->scan_idx = NULL;
    }
    app->scan_idx_cap = 0;
    app->scan_idx_len = 0;
}

static bool mn_app_is_known(void *user, const char *path,
                            int64_t mtime, uint64_t size)
{
    mn_app *app = (mn_app *)user;

    if (!app || !path) {
        return false;
    }
    /* Fast path: answer from the RAM index (lock-free — the index is fully
     * built on the control thread before workers start and never mutated
     * during the walk). One string hash + probe vs a locked SQLite query. */
    if (app->scan_idx && app->scan_idx_cap) {
        uint64_t hsh = mn_scan_hash(path);
        size_t m = app->scan_idx_cap - 1, j = hsh & m;
        size_t steps = 0;
        while (app->scan_idx[j].hash) {
            if (app->scan_idx[j].hash == hsh && app->scan_idx[j].path &&
                strcmp(app->scan_idx[j].path, path) == 0) {
                return app->scan_idx[j].mtime != 0
                    && app->scan_idx[j].mtime == mtime
                    && app->scan_idx[j].size  == (int64_t)size;
            }
            j = (j + 1) & m;
            if (++steps > app->scan_idx_cap) return false;  /* full-table guard */
        }
        return false;   /* not in the index => new file */
    }

    /* Fallback (index unavailable): the original locked point query. */
    {
    int64_t db_mtime = 0, db_size = 0;
    bool    missing = false;
    bool    known = false;
    MN_LOCK(&app->lib_lock);
    if (app->lib &&
        mn_library_lookup_path(app->lib, path, NULL, &db_mtime, &db_size,
                               &missing) == MNDB_OK) {
        known = !missing
             && db_mtime != 0
             && db_mtime == mtime
             && db_size == (int64_t)size;
    }
    MN_UNLOCK(&app->lib_lock);
    return known;
    }
}

/* ================================================================== */
/* Post-scan reconcile: mark rows whose files vanished as missing=1    */
/* (and clear the flag for files that reappeared). Runs on its own     */
/* background thread after each scan finishes; batches of paths are    */
/* snapshotted under the lock, the filesystem stats happen OUTSIDE it. */
/* ================================================================== */

#define MN_RECONCILE_BATCH 256
#define MN_RECONCILE_ROOTS 256

/* True when `path` is an existing, reachable directory. */
static bool mn_rec_root_online(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
/* Case-appropriate path-prefix test (Windows paths are case-insensitive). */
static bool mn_rec_path_under(const char *path, const char *root, size_t rlen)
{
#ifdef _WIN32
    return _strnicmp(path, root, rlen) == 0;
#else
    return strncmp(path, root, rlen) == 0;
#endif
}

static void mn_app_reconcile_body(mn_app *app)
{
    typedef struct {
        int64_t id;
        bool    missing;
        bool    need_created;   /* pre-v6 row: stat + backfill birth time */
        char    path[MN_STR_PATH];
    } mn_rec_row;
    typedef struct {
        char   path[MN_STR_PATH];
        size_t len;
        bool   online;
    } mn_rec_root;

    mn_filter_spec spec;
    mn_query      *q = NULL;
    int64_t        offset = 0;
    bool           dirty = false;
    mn_rec_row    *batch;
    mn_rec_root   *roots = NULL;
    int32_t        nroots = 0;

    batch = (mn_rec_row *)malloc(sizeof(mn_rec_row) * MN_RECONCILE_BATCH);
    if (!batch) {
        goto out;
    }

    /* ---- ROOT-AVAILABILITY GUARD -------------------------------------
     * This pass runs right after every scan, including the boot rescan.
     * If a library root (external drive, NAS) simply has not mounted yet,
     * every stat under it fails and the whole root used to be MASS-FLAGGED
     * missing — the "app launches with albums/content missing" bug. A row
     * may only be marked missing when its root is provably reachable; rows
     * under an offline root are skipped untouched this pass (the next scan
     * or watcher event reconciles them once the root is back). */
    {
        mn_folder *fl = (mn_folder *)malloc(sizeof(mn_folder) * MN_RECONCILE_ROOTS);
        if (fl) {
            int32_t nf = mn_app_folder_list(app, fl, MN_RECONCILE_ROOTS);
            if (nf > 0) {
                roots = (mn_rec_root *)calloc((size_t)nf, sizeof(mn_rec_root));
                if (roots) {
                    int32_t fi;
                    for (fi = 0; fi < nf; fi++) {
                        mn_copy_str(roots[nroots].path, MN_STR_PATH, fl[fi].path);
                        roots[nroots].len    = strlen(roots[nroots].path);
                        roots[nroots].online = mn_rec_root_online(fl[fi].path);
                        nroots++;
                    }
                }
            }
            free(fl);
        }
    }

    for (;;) {
        const mn_track_row *rows = NULL;
        int32_t             got = 0;
        int32_t             i, n;

        MN_LOCK(&app->lib_lock);
        if (app->shutting_down || !app->lib) {
            MN_UNLOCK(&app->lib_lock);
            break;
        }
        if (!q) {
            memset(&spec, 0, sizeof(spec));
            spec.include_missing = true;             /* visit flagged rows too */
            spec.sort[0].key = MNDB_SORT_PATH;       /* stable enumeration     */
            spec.sort[0].descending = false;
            spec.sort_len = 1;
            if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
                q = NULL;
                MN_UNLOCK(&app->lib_lock);
                break;
            }
        }
        if (mn_query_window(q, offset, MN_RECONCILE_BATCH, &rows, &got) != MNDB_OK
            || !rows || got <= 0) {
            MN_UNLOCK(&app->lib_lock);
            break;
        }
        for (i = 0; i < got; ++i) {
            batch[i].id           = rows[i].id;
            batch[i].missing      = rows[i].missing;
            batch[i].need_created = rows[i].created <= 0;
            mn_copy_str(batch[i].path, MN_STR_PATH, rows[i].path);
        }
        n = got;
        MN_UNLOCK(&app->lib_lock);

        /* Filesystem checks WITHOUT the lock (bounded by library size). */
        for (i = 0; i < n; ++i) {
            int64_t created = 0;
            bool gone;
            /* skip rows whose root is offline this pass (see guard above) */
            {
                bool under_any = false, skip = false;
                int32_t ri;
                for (ri = 0; ri < nroots; ri++) {
                    if (mn_rec_path_under(batch[i].path, roots[ri].path, roots[ri].len)) {
                        under_any = true;
                        if (!roots[ri].online) skip = true;
                        break;
                    }
                }
#ifdef _WIN32
                /* row under no registered root: at least require its VOLUME
                 * to be present before trusting a failed stat */
                if (!under_any && batch[i].path[0] && batch[i].path[1] == ':') {
                    char vol[4];
                    UINT dt;
                    vol[0] = batch[i].path[0]; vol[1] = ':'; vol[2] = '\\'; vol[3] = 0;
                    dt = GetDriveTypeA(vol);
                    if (dt == DRIVE_NO_ROOT_DIR || dt == DRIVE_UNKNOWN) skip = true;
                }
#endif
                if (skip) continue;
            }
            if (batch[i].need_created) {
                /* one stat serves both existence + birth-time backfill */
                gone = !mn_app_stat_file2(batch[i].path, NULL, NULL, &created);
            } else {
                gone = !mn_app_file_exists(batch[i].path);
            }
            if (gone != batch[i].missing ||
                (!gone && batch[i].need_created && created > 0)) {
                MN_LOCK(&app->lib_lock);
                if (!app->shutting_down && app->lib) {
                    if (gone != batch[i].missing) {
                        (void)mn_library_mark_missing(app->lib, batch[i].id, gone);
                        dirty = true;
                    }
                    if (!gone && batch[i].need_created && created > 0) {
                        (void)mn_library_set_created(app->lib, batch[i].id,
                                                     created);
                    }
                }
                MN_UNLOCK(&app->lib_lock);
            }
        }

        offset += n;
        if (n < MN_RECONCILE_BATCH) {
            break;
        }
    }

    if (q) {
        MN_LOCK(&app->lib_lock);
        mn_query_close(q);
        if (dirty) {
            app->query_dirty = true; app->alb_cache_valid = false;   /* rows (dis)appear from the view */
        }
        MN_UNLOCK(&app->lib_lock);
    }
    free(batch);
    free(roots);
out:
    /* Piggyback the untagged-row inference backfill on this thread: it
     * runs after every scan's reconcile, is idempotent (second pass finds
     * nothing empty), and shares the reconciling-flag lifecycle guards. */
    if (app->settings.infer_tags) {
        (void)mn_app_reinfer_untagged(app);
    }
    MN_LOCK(&app->lib_lock);
    app->reconciling = false;
    MN_UNLOCK(&app->lib_lock);
}

#ifdef _WIN32
static DWORD WINAPI mn_app_reconcile_thread_fn(LPVOID param)
{
    mn_app_reconcile_body((mn_app *)param);
    /* release this short-lived thread's TLS reader connection */
    mn_app_thread_detach((mn_app *)param);
    return 0;
}
#else
static void *mn_app_reconcile_thread_fn(void *param)
{
    mn_app_reconcile_body((mn_app *)param);
    return NULL;
}
#endif

/* ================================================================== */
/* Untagged-row re-inference (backfill)                               */
/* ================================================================== */

/*
 * Walk the library and apply filename/folder inference to rows whose
 * title/artist/album/track came up empty at scan time (legacy rows from
 * before inference existed, or scans run with the setting off). Pure
 * DB+string work — the files themselves are never touched. Idempotent:
 * a second pass finds no empty fields left to fill and writes nothing.
 * Runs on the caller's thread; callers wrap it in a worker.
 */
#define MN_REINF_BATCH 256

int64_t mn_app_reinfer_untagged(mn_app *app)
{
    typedef struct {
        char    path[MN_STR_PATH];
        char    title[256], artist[256], album[256], album_artist[256];
        char    composer[256], genre[128], format[12];
        int32_t year, track, disc, sample_rate, channels, bit_depth, bitrate_kbps;
        int64_t duration_ms, size, mtime;
        bool    has_art;
    } mn_reinf_row;

    mn_filter_spec spec;
    mn_query      *q = NULL;
    int64_t        offset = 0, updated = 0;
    mn_reinf_row  *cand;

    if (!app) return 0;
    cand = (mn_reinf_row *)malloc(sizeof(mn_reinf_row) * MN_REINF_BATCH);
    if (!cand) return 0;

    for (;;) {
        const mn_track_row *rows = NULL;
        int32_t             got = 0;
        int32_t             i, nc = 0;

        MN_LOCK(&app->lib_lock);
        if (app->shutting_down || !app->lib) {
            MN_UNLOCK(&app->lib_lock);
            break;
        }
        if (!q) {
            memset(&spec, 0, sizeof(spec));
            spec.include_missing = false;        /* absent files: skip     */
            spec.sort[0].key = MNDB_SORT_PATH;   /* stable enumeration     */
            spec.sort[0].descending = false;
            spec.sort_len = 1;
            if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
                q = NULL;
                MN_UNLOCK(&app->lib_lock);
                break;
            }
        }
        if (mn_query_window(q, offset, MN_REINF_BATCH, &rows, &got) != MNDB_OK
            || !rows || got <= 0) {
            MN_UNLOCK(&app->lib_lock);
            break;
        }
        for (i = 0; i < got; ++i) {
            const mn_track_row *r = &rows[i];
            mn_reinf_row *c;
            if (r->title[0] && r->artist[0] && r->album[0] && r->track != 0)
                continue;                        /* fully tagged           */
            c = &cand[nc++];
            mn_copy_str(c->path,         sizeof(c->path),         r->path);
            mn_copy_str(c->title,        sizeof(c->title),        r->title);
            mn_copy_str(c->artist,       sizeof(c->artist),       r->artist);
            mn_copy_str(c->album,        sizeof(c->album),        r->album);
            mn_copy_str(c->album_artist, sizeof(c->album_artist), r->album_artist);
            mn_copy_str(c->composer,     sizeof(c->composer),     r->composer);
            mn_copy_str(c->genre,        sizeof(c->genre),        r->genre);
            mn_copy_str(c->format,       sizeof(c->format),       r->format);
            c->year = r->year; c->track = r->track; c->disc = r->disc;
            c->sample_rate = r->sample_rate; c->channels = r->channels;
            c->bit_depth = r->bit_depth; c->bitrate_kbps = r->bitrate_kbps;
            c->duration_ms = r->duration_ms; c->size = r->size;
            c->mtime = r->mtime; c->has_art = r->has_art;
        }
        MN_UNLOCK(&app->lib_lock);

        /* Inference + one write transaction per window. Under the lock:
         * the merge consults app->roots (root-album suppression) and the
         * string work is microseconds per row. */
        if (nc > 0) {
            int wrote_any = 0;
            MN_LOCK(&app->lib_lock);
            if (!app->shutting_down && app->lib) {
                (void)mn_library_begin(app->lib);
                for (i = 0; i < nc; ++i) {
                    mn_track_in t;
                    mn_reinf_row *c = &cand[i];
                    mn_inferred   inf;
                    bool          changed = false;
                    mn_infer_from_path(c->path, &inf);
                    mn_infer_suppress_root_album(app, c->path, &inf);
                    if (!c->title[0] && inf.title[0]) {
                        mn_copy_str(c->title, sizeof(c->title), inf.title);
                        changed = true;
                    }
                    if (!c->artist[0] && inf.artist[0]) {
                        mn_copy_str(c->artist, sizeof(c->artist), inf.artist);
                        changed = true;
                    }
                    if (!c->album[0] && inf.album[0]) {
                        mn_copy_str(c->album, sizeof(c->album), inf.album);
                        changed = true;
                    }
                    if (c->track == 0 && inf.track != 0) {
                        c->track = inf.track;
                        changed = true;
                    }
                    if (!changed) continue;
                    memset(&t, 0, sizeof(t));
                    t.path         = c->path;
                    t.title        = c->title[0]        ? c->title        : NULL;
                    t.artist       = c->artist[0]       ? c->artist       : NULL;
                    t.album        = c->album[0]        ? c->album        : NULL;
                    t.album_artist = c->album_artist[0] ? c->album_artist : NULL;
                    t.composer     = c->composer[0]     ? c->composer     : NULL;
                    t.genre        = c->genre[0]        ? c->genre        : NULL;
                    t.format       = c->format[0]       ? c->format       : NULL;
                    t.year = c->year; t.track = c->track; t.disc = c->disc;
                    t.sample_rate = c->sample_rate; t.channels = c->channels;
                    t.bit_depth = c->bit_depth; t.bitrate_kbps = c->bitrate_kbps;
                    t.duration_ms = c->duration_ms; t.size = c->size;
                    t.mtime = c->mtime; t.has_art = c->has_art;
                    if (mn_library_upsert_track(app->lib, &t, NULL) == MNDB_OK) {
                        updated++; wrote_any = 1;
                    }
                }
                if (mn_library_in_transaction(app->lib))
                    (void)mn_library_commit(app->lib);
                if (wrote_any) {
                    app->query_dirty = true;
                    app->alb_cache_valid = false;
                }
            }
            MN_UNLOCK(&app->lib_lock);
        }

        offset += got;
        if (got < MN_REINF_BATCH) {
            break;
        }
    }

    if (q) {
        MN_LOCK(&app->lib_lock);
        mn_query_close(q);
        MN_UNLOCK(&app->lib_lock);
    }
    free(cand);
    return updated;
}

/* Kick a reconcile pass. Caller holds the app lock. No-op when one is
 * already in flight or the app is shutting down. */
static void mn_app_spawn_reconcile_locked(mn_app *app)
{
    if (app->reconciling || app->shutting_down) {
        return;
    }
#ifdef _WIN32
    /* A previous (finished) thread handle may still be open: recycle it.
     * reconciling==false guarantees the thread body is done with `app`. */
    if (app->reconcile_thread) {
        CloseHandle(app->reconcile_thread);
        app->reconcile_thread = NULL;
    }
    app->reconciling = true;
    app->reconcile_thread = CreateThread(NULL, 0, mn_app_reconcile_thread_fn,
                                         app, 0, NULL);
    if (!app->reconcile_thread) {
        app->reconciling = false;
    }
#else
    if (app->reconcile_valid) {
        pthread_join(app->reconcile_thread, NULL);   /* finished: cheap */
        app->reconcile_valid = false;
    }
    app->reconciling = true;
    app->reconcile_valid =
        (pthread_create(&app->reconcile_thread, NULL,
                        mn_app_reconcile_thread_fn, app) == 0);
    if (!app->reconcile_valid) {
        app->reconciling = false;
    }
#endif
}

/* ================================================================== */
/* Scan lifecycle                                                     */
/* ================================================================== */

/* Start a scan over `roots[0..count)`. The caller must have already detached
 * and destroyed any previous scanner (see mn_app_detach_scanner) and must hold
 * the app lock. Returns true if the scan was launched. */
static bool mn_app_start_scan(mn_app *app, const char *const *roots, size_t count)
{
    mn_scanner_config cfg;
    mn_scanner       *sc;

    if (!app || !roots || count == 0) {
        return false;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.roots        = roots;
    cfg.root_count   = count;
    cfg.on_track     = mn_app_on_track;
    cfg.is_known     = mn_app_is_known;   /* skip unchanged files (mtime+size) */
    cfg.user         = app;
    cfg.thread_count = 0;      /* auto: number of logical CPUs */

    sc = mn_scanner_create(&cfg);
    if (!sc) {
        return false;
    }

    /* Build the fast is_known index from ONE bulk query, before workers start
     * and before the write transaction opens. Turns a rescan's per-file
     * locked DB lookups into lock-free RAM probes. */
    mn_app_scan_index_build(app);

    /* Open a batch transaction for throughput; committed at boundaries. */
    app->scan_pending   = 0;
    app->scan_committed = 0;
    if (!mn_library_in_transaction(app->lib)) {
        (void)mn_library_begin(app->lib);
    }

    if (!mn_scanner_start(sc)) {
        mn_scanner_destroy(sc);
        if (mn_library_in_transaction(app->lib)) {
            (void)mn_library_rollback(app->lib);
        }
        return false;
    }

    app->scanner  = sc;
    app->scanning = true;
    return true;
}

/* ================================================================== */
/* Async stems-model loader                                           */
/* ================================================================== */

/*
 * Loading the stem-separation ONNX model (~136 MB, CUDA session build)
 * takes 10-30 s; doing it inside mn_app_create blocked the window from
 * appearing. This body runs on a background thread: the expensive
 * mn_stems_create happens WITHOUT the app lock, then the finished session
 * is published to app->stems under the lock and handed to the engine with
 * the CURRENT enabled/passthrough flags. Every mn_app_stems_* / now()
 * consumer already NULL-checks app->stems, so "not loaded yet" simply
 * reads as "stems unavailable" until publication.
 */
static void mn_app_stems_loader_body(mn_app *app)
{
    mn_stems *s = mn_stems_create(app->model_path, app->stem_cache_dir);

    MN_LOCK(&app->lib_lock);
    if (app->shutting_down) {
        /* mn_app_destroy started while we were loading: it will join us
         * and never sees this session — dispose of it ourselves. */
        MN_UNLOCK(&app->lib_lock);
        if (s) {
            mn_stems_destroy(s);
        }
        return;
    }
    app->stems         = s;   /* may be NULL: model missing => feature off */
    app->stems_loading = false;
    if (s) {
        /* Apply the persisted stems-cache cap now that the session exists
         * (set_settings may have run before the async model finished). */
        if (app->settings.stem_cache_gb > 0) {
            mn_stems_set_cache_cap_bytes(
                (int64_t)app->settings.stem_cache_gb * 1024 * 1024 * 1024);
        }
        /* Publish to the engine with whatever flags the user set while the
         * model was loading, and kick separation for the active track. */
        mn_engine_set_stem_source(app->engine, s, app->stems_enabled,
                                  app->stems_passthrough);
        if (app->stems_enabled && app->pb) {
            mn_track_info info;
            if (mn_playback_current_track(app->pb, &info)) {
                (void)mn_stems_start(s, info.id, info.path);
                mn_stems_set_passthrough(s, app->stems_passthrough);
            }
        }
    }
    MN_UNLOCK(&app->lib_lock);
}

#ifdef _WIN32
static DWORD WINAPI mn_app_stems_loader_thread(LPVOID param)
{
    mn_app_stems_loader_body((mn_app *)param);
    return 0;
}
#else
static void *mn_app_stems_loader_thread(void *param)
{
    mn_app_stems_loader_body((mn_app *)param);
    return NULL;
}
#endif

/* Spawn the loader; on thread-creation failure fall back to a synchronous
 * load so the app still gets its stems session. */
static void mn_app_spawn_stems_loader(mn_app *app)
{
    app->stems_loading = true;
#ifdef _WIN32
    app->stems_loader = CreateThread(NULL, 0, mn_app_stems_loader_thread,
                                     app, 0, NULL);
    if (!app->stems_loader) {
        mn_app_stems_loader_body(app);
    }
#else
    app->stems_loader_valid =
        (pthread_create(&app->stems_loader, NULL,
                        mn_app_stems_loader_thread, app) == 0);
    if (!app->stems_loader_valid) {
        mn_app_stems_loader_body(app);
    }
#endif
}

/* Join the loader thread if it is (or was) running. Called from destroy
 * AFTER shutting_down is set, WITHOUT the app lock held (the loader takes
 * the lock to publish). */
static void mn_app_join_stems_loader(mn_app *app)
{
#ifdef _WIN32
    if (app->stems_loader) {
        WaitForSingleObject(app->stems_loader, INFINITE);
        CloseHandle(app->stems_loader);
        app->stems_loader = NULL;
    }
#else
    if (app->stems_loader_valid) {
        pthread_join(app->stems_loader, NULL);
        app->stems_loader_valid = false;
    }
#endif
}

/* Join the post-scan missing-file reconcile thread (see change further
 * below); same discipline as the stems loader: never hold the app lock. */
static void mn_app_join_reconcile(mn_app *app)
{
#ifdef _WIN32
    if (app->reconcile_thread) {
        WaitForSingleObject(app->reconcile_thread, INFINITE);
        CloseHandle(app->reconcile_thread);
        app->reconcile_thread = NULL;
    }
#else
    if (app->reconcile_valid) {
        pthread_join(app->reconcile_thread, NULL);
        app->reconcile_valid = false;
    }
#endif
}

/* ================================================================== */
/* Lifecycle                                                          */
/* ================================================================== */

mn_app *mn_app_create(const char *data_dir)
{
    mn_app       *app;
    mn_open_opts  opts;
    char          db_path[MN_STR_PATH];
    mn_status     st;
    mn_result     er;

    if (!data_dir || data_dir[0] == '\0') {
        return NULL;
    }

    app = (mn_app *)calloc(1, sizeof(*app));
    if (!app) {
        return NULL;
    }

    MN_LOCK_INIT(&app->lib_lock);
    mn_copy_str(app->data_dir, sizeof(app->data_dir), data_dir);
    mn_path_join(app->art_cache_dir,  sizeof(app->art_cache_dir),  data_dir, MN_ART_CACHE_SUBPATH);
    mn_path_join(app->stem_cache_dir, sizeof(app->stem_cache_dir), data_dir, MN_STEM_CACHE_SUBPATH);
    mn_path_join(app->model_path,     sizeof(app->model_path),     data_dir, MN_STEM_MODEL_SUBPATH);
    mn_path_join(db_path,             sizeof(db_path),             data_dir, MN_DB_SUBPATH);
    mn_path_join(app->hidden_file,    sizeof(app->hidden_file),    data_dir, "hidden_folders.txt");

    /* Resolve the stems model from the persisted selection (selected.txt);
     * falls back to the bundled htdemucs_6s.onnx. If the selected file is
     * missing on disk, keep the bundled default so the app still loads. */
    {
        char sel[256] = {0};
        if (mn_app_get_selected_model(app, "stems", sel, sizeof(sel)) &&
            sel[0] && strcmp(sel, MN_STEM_MODEL_DEFAULT_FILE) != 0) {
            char sub[300], cand[MN_STR_PATH];
            snprintf(sub, sizeof(sub), "ai-models/%s", sel);
            mn_path_join(cand, sizeof(cand), data_dir, sub);
            FILE *cf = fopen(cand, "rb");
            if (cf) {
                fclose(cf);
                mn_copy_str(app->model_path, sizeof(app->model_path), cand);
                fprintf(stderr, "[app] stems model from selection: %s\n", sel);
            } else {
                fprintf(stderr, "[app] selected stems model '%s' missing; "
                                "using bundled default\n", sel);
            }
        }
    }

    /* Restore the persisted hidden-folder set (ids into the library's
     * `folders` table); applied to every query via mn_build_spec. */
    mn_app_load_hidden(app);

    /* Scratch arena for query-string building. */
    if (mn_arena_init(&app->arena, 64 * 1024) != MNDB_OK) {
        free(app);
        return NULL;
    }

    /* Open + migrate the library database. */
    memset(&opts, 0, sizeof(opts));
    opts.read_only         = false;
    opts.create_if_missing = true;
    opts.busy_timeout_ms   = 5000;
    st = mn_library_open(db_path, &opts, &app->lib);
    if (st != MNDB_OK || !app->lib) {
        mn_arena_free(&app->arena);
        free(app);
        return NULL;
    }

    /* Audio engine (out-param API). */
    er = mn_engine_create(&app->engine);
    if (er != MNE_OK || !app->engine) {
        mn_library_close(app->lib);
        mn_arena_free(&app->arena);
        free(app);
        return NULL;
    }

    /* Playback controller over the engine. */
    app->pb = mn_playback_create(app->engine);
    if (!app->pb) {
        mn_engine_destroy(app->engine);
        mn_library_close(app->lib);
        mn_arena_free(&app->arena);
        free(app);
        return NULL;
    }

    /* Neural stem session: loaded LAZILY, not at boot. The ~136 MB CUDA ONNX
     * session (htdemucs_6s) is only mapped the first time the user actually
     * enables stems (mn_app_stems_enable) or plays with stems already on —
     * stems_enabled defaults false, so an ordinary listening session never
     * pays the RSS/VRAM cost or the load thread. app->stems stays NULL until
     * then; every consumer already NULL-checks it, and the loader body honors
     * whatever stems_enabled/passthrough flags are set when it finishes (so a
     * user who flips stems on gets separation kicked automatically on publish).
     * The engine starts with no stem source (plain passthrough). */
    app->stems = NULL;
    mn_engine_set_stem_source(app->engine, NULL, false, false);

    /* Default browsing/mixer/settings state. Sort default MUST match the
     * UI's default ("Title") — it used to be ARTIST, so any boot where the
     * UI didn't push an explicit sort (e.g. fresh localStorage after a CEF
     * cache clear) served artist order under a dropdown that said Title. */
    app->view       = MN_VIEW_TRACKS;
    app->sort_key   = MN_SORT_TITLE;
    app->sort_asc   = true;
    app->search[0]  = '\0';
    app->query      = NULL;
    app->query_dirty= true;
    app->row_count_cache = 0;

    app->volume            = 1.0f;
    app->stems_enabled     = false;
    app->stems_passthrough = false;
    app->now_track_id      = 0;

    app->settings.exclusive      = false;
    app->settings.crossfade_ms   = 0;
    app->settings.replaygain     = false;
    app->settings.replaygain_mode = 0;
    app->settings.rg_preamp_db   = 0.0f;
    app->settings.stem_cache_gb  = 8;
    app->settings.art_cache_mb   = 2048;
    app->settings.depth_batch    = false;   /* depth maps ON DEMAND by default */
    app->settings.infer_tags     = true;    /* fill missing tags from names */
    app->settings.watch_folders  = true;    /* live folder monitoring       */
    app->settings.low_power      = false;
    app->settings.hifi_native_bits = true;   /* exclusive-mode bit-perfect on */
    app->settings.ab_rate_cap_hz = 48000;    /* audiobook power caps          */
    app->settings.ab_bits_cap    = 16;
    app->sleep_deadline_ms       = 0;
    app->settings.album_art_size = MN_ART_THUMB_SIZE;

    /* DSP shadow defaults mirror the dsp.c defaults (limiter is ON by
     * default there — the EQ modal must reflect that, not show it off). */
    app->dsp_balance          = 0.0f;
    app->dsp_limiter_on       = 1;
    app->dsp_limiter_thresh_db = -1.0f;
    app->dsp_limiter_ceil_db   = -0.1f;
    app->dsp_master_db        = 0.0f;

    /* Load persisted settings over the defaults (audio settings used to
     * reset every launch), then apply the art size + engine state. */
    mn_app_load_settings(app);
    mn_art_set_thumb_size(app->settings.album_art_size);
    if (app->pb) {
        mn_replaygain_mode rgm = MN_REPLAYGAIN_OFF;
        if (app->settings.replaygain_mode == 2)      rgm = MN_REPLAYGAIN_ALBUM;
        else if (app->settings.replaygain_mode == 1 ||
                 app->settings.replaygain)           rgm = MN_REPLAYGAIN_TRACK;
        mn_playback_set_crossfade_ms(app->pb, (uint32_t)app->settings.crossfade_ms);
        mn_playback_set_replaygain(app->pb, rgm, (double)app->settings.rg_preamp_db);
    }
    if (app->engine) {
        mn_engine_set_exclusive(app->engine, app->settings.exclusive ? 1 : 0);
    }

    mn_playback_set_volume(app->pb, app->volume);

    return app;
}

void mn_app_destroy(mn_app *app)
{
    if (!app) {
        return;
    }

    /* 0. Flag shutdown, then join the async model loader + reconcile pass
     *    WITHOUT the lock (both take it to publish). A mid-load create is
     *    abandoned: the loader sees shutting_down and destroys its own
     *    session instead of publishing. */
    MN_LOCK(&app->lib_lock);
    app->shutting_down = true;
    MN_UNLOCK(&app->lib_lock);
    mn_app_join_stems_loader(app);
    mn_app_join_reconcile(app);

    /* 1. Stop the scanner first (cancel + join worker threads). */
    if (app->scanner) {
        mn_scanner_destroy(app->scanner);
        app->scanner = NULL;
    }

    /* 2. Close any open scan transaction. */
    if (app->lib && mn_library_in_transaction(app->lib)) {
        (void)mn_library_commit(app->lib);
    }

    /* 3. Cancel stem inference (joins producer thread) before the engine
     *    that feeds it goes away. */
    if (app->stems) {
        mn_stems_cancel(app->stems);
        mn_stems_destroy(app->stems);
        app->stems = NULL;
    }

    /* 4. Stop + destroy the playback controller (stops the engine's
     *    transport) before the engine itself. */
    if (app->pb) {
        mn_playback_stop(app->pb);
        mn_playback_destroy(app->pb);
        app->pb = NULL;
    }

    /* 5. Engine (tears down device/decoder). */
    if (app->engine) {
        mn_engine_destroy(app->engine);
        app->engine = NULL;
    }

    /* 6. Query cursor + library. */
    if (app->query) {
        mn_query_close(app->query);
        app->query = NULL;
    }
    if (app->lib) {
        mn_library_close(app->lib);
        app->lib = NULL;
    }

    mn_arena_free(&app->arena);
    free(app->alb_cache);
    mn_app_scan_index_free(app);
    MN_LOCK_FREE(&app->lib_lock);
    free(app);
}

/* ================================================================== */
/* Library scanning                                                   */
/* ================================================================== */

/* Serializes the whole stop sequence: two threads can trigger scans at once
 * (CEF dispatch: addfolder/rescan · main thread: drag-drop / command-line
 * open). Without this, the second stopper skips the join (scanner already
 * detached by the first) and frees the is_known index while the FIRST
 * stopper's not-yet-joined workers still probe it. */
static CRITICAL_SECTION g_scan_ctl;
static LONG             g_scan_ctl_init = 0;
static void mn_scan_ctl_lock(void)
{
    if (InterlockedCompareExchange(&g_scan_ctl_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_scan_ctl);
        InterlockedExchange(&g_scan_ctl_init, 2);
    }
    while (InterlockedCompareExchange(&g_scan_ctl_init, 0, 0) != 2) Sleep(0);
    EnterCriticalSection(&g_scan_ctl);
}
static void mn_scan_ctl_unlock(void) { LeaveCriticalSection(&g_scan_ctl); }

/* Detach the current scanner (under the lock) and destroy it OUTSIDE the
 * lock. mn_scanner_destroy cancels and JOINS the worker threads, and those
 * workers take the app lock inside mn_app_on_track — joining while holding
 * the lock would deadlock. Safe to call with no scanner active. */
static void mn_app_stop_scanner(mn_app *app)
{
    mn_scanner *old;

    mn_scan_ctl_lock();

    MN_LOCK(&app->lib_lock);
    old = app->scanner;
    app->scanner  = NULL;
    app->scanning = false;
    MN_UNLOCK(&app->lib_lock);

    if (old) {
        mn_scanner_destroy(old);   /* cancel + join, lock NOT held */
    }
    /* Workers are joined — the is_known index is no longer read; free it. */
    mn_app_scan_index_free(app);

    /* Close out any batch transaction the dead scan left open so the next
     * scan (or plain queries) start from a clean slate. */
    MN_LOCK(&app->lib_lock);
    if (app->lib && mn_library_in_transaction(app->lib)) {
        (void)mn_library_commit(app->lib);
    }
    MN_UNLOCK(&app->lib_lock);

    mn_scan_ctl_unlock();
}

bool mn_app_add_folder(mn_app *app, const char *path)
{
    const char *roots[1];
    bool        ok;

    if (!app || !path || path[0] == '\0') {
        return false;
    }

    /* Tear down any in-flight scan first — WITHOUT holding the lock while
     * its workers are joined (they need the lock in on_track). */
    mn_app_stop_scanner(app);

    MN_LOCK(&app->lib_lock);

    /* Record the root for later rescans (dedup, bounded). */
    if (app->root_count < MN_MAX_ROOTS) {
        int i;
        bool exists = false;
        for (i = 0; i < app->root_count; ++i) {
            if (strcmp(app->roots[i], path) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            mn_copy_str(app->roots[app->root_count], sizeof(app->roots[0]), path);
            app->root_count++;
        }
    }

    mn_copy_str(app->scan_source, sizeof(app->scan_source), path);

    roots[0] = path;
    ok = mn_app_start_scan(app, roots, 1);
    if (ok) {
        /* New content en route — refresh the view as it lands. */
        app->query_dirty = true; app->alb_cache_valid = false;
    }
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

/* Is `path` equal to or inside `root`? (case-insensitive on Windows,
 * separator-aware). */
static bool mn_app_path_under(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (n == 0) return false;
#ifdef _WIN32
    if (_strnicmp(root, path, n) != 0) return false;
#else
    if (strncmp(root, path, n) != 0) return false;
#endif
    return path[n] == '\0' || path[n] == '\\' || path[n] == '/'
        || root[n - 1] == '\\' || root[n - 1] == '/';
}

/* Register a library root for later rescans WITHOUT starting a scan
 * (dedup + subsumption: a path already covered by a registered root is
 * ignored). Caller holds the app lock. */
static void mn_app_register_root_locked(mn_app *app, const char *path)
{
    int i;
    if (!path || !path[0] || app->root_count >= MN_MAX_ROOTS) {
        return;
    }
    for (i = 0; i < app->root_count; ++i) {
        if (mn_app_path_under(app->roots[i], path)) {
            return;   /* duplicate or already covered */
        }
    }
    mn_copy_str(app->roots[app->root_count], sizeof(app->roots[0]), path);
    app->root_count++;
}

bool mn_app_register_root(mn_app *app, const char *path)
{
    if (!app || !path || !path[0]) {
        return false;
    }
    MN_LOCK(&app->lib_lock);
    mn_app_register_root_locked(app, path);
    MN_UNLOCK(&app->lib_lock);
    return true;
}

/* When no roots were registered this session (fresh launch), derive an
 * effective root set from the db's folders dimension: enumerate folder
 * paths (sorted, so parents precede children) and keep the minimal
 * prefix-reduced set. Caller holds the app lock. */
static void mn_app_seed_roots_locked(mn_app *app)
{
    mn_facet           *f = NULL;
    const mn_facet_row *rows = NULL;
    int32_t             got = 0;
    int64_t             off = 0;
    int32_t             i;

    if (app->root_count > 0 || !app->lib) {
        return;
    }
    if (mn_facet_open(app->lib, MN_FACET_FOLDER, NULL, &f) != MNDB_OK || !f) {
        return;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    for (;;) {
        if (mn_facet_window(f, off, 128, &rows, &got) != MNDB_OK
            || !rows || got <= 0) {
            break;
        }
        for (i = 0; i < got; ++i) {
            if (rows[i].label && rows[i].label[0]) {
                mn_app_register_root_locked(app, rows[i].label);
            }
        }
        off += got;
        if (got < 128 || app->root_count >= MN_MAX_ROOTS) {
            break;
        }
    }
    mn_facet_close(f);
}

void mn_app_rescan(mn_app *app)
{
    const char *roots[MN_MAX_ROOTS];
    int i;

    if (!app) {
        return;
    }

    mn_app_stop_scanner(app);

    MN_LOCK(&app->lib_lock);
    /* Fresh launch: no add_folder happened this session, so derive the
     * roots from the library itself (prefix-reduced folder dimension). */
    mn_app_seed_roots_locked(app);
    if (app->root_count == 0) {
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    for (i = 0; i < app->root_count; ++i) {
        roots[i] = app->roots[i];
    }

    mn_copy_str(app->scan_source, sizeof(app->scan_source), app->roots[0]);
    (void)mn_app_start_scan(app, roots, (size_t)app->root_count);
    app->query_dirty = true; app->alb_cache_valid = false;
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_scan_status(mn_app *app, mn_scan *out)
{
    struct mn_scanner_progress pg;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!app) {
        return;
    }

    MN_LOCK(&app->lib_lock);
    if (!app->scanner) {
        MN_UNLOCK(&app->lib_lock);
        return;
    }

    memset(&pg, 0, sizeof(pg));
    mn_scanner_progress(app->scanner, &pg);

    out->active       = !pg.finished && !pg.cancelled;
    out->found        = (int64_t)pg.files_found;
    out->processed    = (int64_t)pg.files_processed;
    out->dirs_scanned = (int64_t)pg.dirs_scanned;
    out->skipped      = (int64_t)pg.files_skipped;
    out->tag_errors   = (int64_t)pg.tag_errors;
    out->io_errors    = (int64_t)pg.io_errors;
    mn_copy_str(out->source, sizeof(out->source), app->scan_source);

    /* On finish: perform the final commit, clear the scanning flag, and
     * force one last live refresh so the view reflects everything. */
    if (pg.finished && app->scanning) {
        if (mn_library_in_transaction(app->lib)) {
            (void)mn_library_commit(app->lib);
        }
        (void)mn_library_analyze(app->lib);
        app->scanning    = false;
        app->query_dirty = true; app->alb_cache_valid = false;
        /* Post-scan pass: flag db rows whose files vanished as missing
         * (background thread; see mn_app_reconcile_body). */
        mn_app_spawn_reconcile_locked(app);
    }
    MN_UNLOCK(&app->lib_lock);
}

/* ================================================================== */
/* Library reset + missing-row purge                                  */
/* ================================================================== */

/* Delete every regular file directly inside `dir` (non-recursive — the
 * art cache is flat). */
static void mn_app_wipe_dir_files(const char *dir)
{
#ifdef _WIN32
    char             pattern[MN_STR_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char full[MN_STR_PATH + MAX_PATH];
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            DeleteFileA(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR           *d = opendir(dir);
    struct dirent *e;
    if (!d) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char full[MN_STR_PATH * 2];
        struct stat sst;
        if (e->d_name[0] == '.') continue;
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (stat(full, &sst) == 0 && S_ISREG(sst.st_mode)) {
            unlink(full);
        }
    }
    closedir(d);
#endif
}

bool mn_app_reset_library(mn_app *app, bool clear_art)
{
    const char *roots[MN_MAX_ROOTS];
    int         i;
    bool        ok;

    if (!app || !app->lib) {
        return false;
    }

    /* Stop the scan machinery FIRST — the scanner join and the reconcile
     * join both must happen without the app lock held. */
    mn_app_stop_scanner(app);
    mn_app_join_reconcile(app);

    MN_LOCK(&app->lib_lock);

    /* Silence playback + drop the queue (its paths reference rows that are
     * about to vanish). */
    if (app->pb) {
        mn_playback_stop(app->pb);
        mn_playback_clear(app->pb);
    }
    if (app->stems) {
        mn_stems_cancel(app->stems);
    }
    if (app->engine) {
        (void)mn_engine_unload(app->engine);
    }
    app->now_track_id = 0;
    app->now_liked    = 0;
    app->now_title[0] = app->now_artist[0] = '\0';
    app->now_album[0] = app->now_album_artist[0] = '\0';

    /* Remember what to rescan BEFORE the folders table is wiped. */
    mn_app_seed_roots_locked(app);

    ok = (mn_library_reset(app->lib) == MNDB_OK);
    app->query_dirty = true; app->alb_cache_valid = false;

    if (clear_art) {
        /* The webroot PNG mirror (and its .depth.png companions) is owned
         * by the CEF host, which clears it alongside this call. */
        mn_app_wipe_dir_files(app->art_cache_dir);
    }

    /* Re-scan all registered roots (scan status flows to the UI as usual). */
    if (ok && app->root_count > 0) {
        for (i = 0; i < app->root_count; ++i) {
            roots[i] = app->roots[i];
        }
        mn_copy_str(app->scan_source, sizeof(app->scan_source), app->roots[0]);
        (void)mn_app_start_scan(app, roots, (size_t)app->root_count);
    }
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int64_t mn_app_purge_missing(mn_app *app)
{
    mn_filter_spec spec;
    mn_query      *q = NULL;
    int64_t       *ids = NULL;
    int64_t        n_ids = 0, cap = 0;
    int64_t        offset = 0;
    int64_t        purged = 0;
    int64_t        k;

    if (!app || !app->lib) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);

    /* Collect the ids of every missing-flagged row (windowed). */
    memset(&spec, 0, sizeof(spec));
    spec.include_missing = true;
    spec.sort[0].key = MNDB_SORT_PATH;
    spec.sort[0].descending = false;
    spec.sort_len = 1;

    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    for (;;) {
        const mn_track_row *rows = NULL;
        int32_t             got = 0, i;
        if (app->shutting_down) break;   /* teardown: bail promptly */
        if (mn_query_window(q, offset, 256, &rows, &got) != MNDB_OK
            || !rows || got <= 0) {
            break;
        }
        for (i = 0; i < got; ++i) {
            if (!rows[i].missing) continue;
            if (n_ids == cap) {
                int64_t  ncap = cap ? cap * 2 : 256;
                int64_t *nid = (int64_t *)realloc(ids, (size_t)ncap * sizeof(*ids));
                if (!nid) { got = 0; break; }
                ids = nid;
                cap = ncap;
            }
            ids[n_ids++] = rows[i].id;
        }
        offset += got;
        if (got < 256) break;
    }
    mn_query_close(q);

    /* Delete them in one batch transaction (delete_track keeps dimension
     * counts + FTS + playlist membership consistent per row). */
    if (n_ids > 0) {
        bool own_txn = !mn_library_in_transaction(app->lib);
        if (own_txn) {
            (void)mn_library_begin(app->lib);
        }
        for (k = 0; k < n_ids; ++k) {
            if (mn_library_delete_track(app->lib, ids[k]) == MNDB_OK) {
                purged++;
            }
        }
        if (own_txn && mn_library_in_transaction(app->lib)) {
            (void)mn_library_commit(app->lib);
        }
        app->query_dirty = true; app->alb_cache_valid = false;
    }
    free(ids);
    MN_UNLOCK(&app->lib_lock);
    return purged;
}

/* ================================================================== */
/* AI model downloads (Hugging Face)                                  */
/* ================================================================== */

void mn_app_models_dir(mn_app *app, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!app) return;
    mn_path_join(out, n, app->data_dir, "ai-models");
}

/* ================================================================== */
/* AI model selection (persisted in <data_dir>/ai-models/selected.txt) */
/* ================================================================== */

/* Reject filenames that could escape the ai-models directory. Selections are
 * bare filenames of already-downloaded models; no path separators allowed. */
static bool mn_model_file_safe(const char *f)
{
    if (!f || !f[0]) return false;
    if (strchr(f, '/') || strchr(f, '\\')) return false;
    if (strstr(f, "..")) return false;
    return true;
}

static const char *mn_model_default_file(const char *kind)
{
    if (kind && strcmp(kind, "depth") == 0) return MN_DEPTH_MODEL_DEFAULT_FILE;
    return MN_STEM_MODEL_DEFAULT_FILE; /* "stems" (and any unknown) -> stems */
}

/* Path of the selection file: <data_dir>/ai-models/selected.txt */
static void mn_app_selected_file(mn_app *app, char *out, size_t n)
{
    char dir[MN_STR_PATH];
    mn_app_models_dir(app, dir, sizeof(dir));
    mn_path_join(out, n, dir, "selected.txt");
}

bool mn_app_get_selected_model(mn_app *app, const char *kind,
                               char *out, size_t n)
{
    char  path[MN_STR_PATH];
    FILE *f;
    char  line[512];
    const char *def;

    if (!app || !out || n == 0 || !kind) return false;
    def = mn_model_default_file(kind);
    mn_copy_str(out, n, def);   /* default unless overridden below */

    mn_app_selected_file(app, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return true;        /* no file yet == bundled default */

    while (fgets(line, sizeof(line), f)) {
        /* Format "kind=filename". Trim trailing CR/LF. */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == '\n' || val[vl - 1] == '\r' ||
                          val[vl - 1] == ' '  || val[vl - 1] == '\t'))
            val[--vl] = '\0';
        if (strcmp(line, kind) == 0 && vl > 0 && mn_model_file_safe(val)) {
            mn_copy_str(out, n, val);
            break;
        }
    }
    fclose(f);
    return true;
}

bool mn_app_set_selected_model(mn_app *app, const char *kind,
                               const char *filename)
{
    char  path[MN_STR_PATH];
    char  cur_stems[256];
    char  cur_depth[256];

    if (!app || !kind) return false;
    if (strcmp(kind, "stems") != 0 && strcmp(kind, "depth") != 0)
        return false;
    if (!mn_model_file_safe(filename)) return false;

    /* Read the current pair so we rewrite both lines (simple, atomic-ish). */
    (void)mn_app_get_selected_model(app, "stems", cur_stems, sizeof(cur_stems));
    (void)mn_app_get_selected_model(app, "depth", cur_depth, sizeof(cur_depth));
    if (strcmp(kind, "stems") == 0)
        mn_copy_str(cur_stems, sizeof(cur_stems), filename);
    else
        mn_copy_str(cur_depth, sizeof(cur_depth), filename);

    /* Make sure the ai-models dir exists before writing. */
    {
        char dir[MN_STR_PATH];
        mn_app_models_dir(app, dir, sizeof(dir));
#ifdef _WIN32
        (void)_mkdir(dir);
#else
        (void)mkdir(dir, 0755);
#endif
    }

    mn_app_selected_file(app, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "stems=%s\n", cur_stems);
    fprintf(f, "depth=%s\n", cur_depth);
    fclose(f);

    /* Keep the live stems path in sync for a *future* create; the running
     * session is not swapped (documented "restart to apply"). */
    if (strcmp(kind, "stems") == 0) {
        char sub[300];
        snprintf(sub, sizeof(sub), "ai-models/%s", filename);
        MN_LOCK(&app->lib_lock);
        mn_path_join(app->model_path, sizeof(app->model_path),
                     app->data_dir, sub);
        MN_UNLOCK(&app->lib_lock);
    }
    return true;
}

/*
 * Trampoline: mn_modeldl carries a single opaque user pointer and no id, so we
 * heap-allocate a context that remembers the UI id + the caller's callback,
 * and translate the module's raw progress into the app-level signature. The
 * context is freed on the terminal (finished) callback.
 */
typedef struct {
    char         id[64];
    mn_app_dl_cb cb;
    void        *user;
} mn_app_dl_ctx;

static void mn_app_dl_trampoline(void *user, int64_t done, int64_t total,
                                 bool finished, const char *err)
{
    mn_app_dl_ctx *ctx = (mn_app_dl_ctx *)user;
    if (!ctx) return;
    if (ctx->cb) ctx->cb(ctx->user, ctx->id, done, total, finished, err);
    if (finished) free(ctx);   /* module guarantees exactly one finished cb */
}

bool mn_app_download_model(mn_app *app, const char *id,
                           const char *repo, const char *file,
                           const char *save_as,
                           mn_app_dl_cb cb, void *user)
{
    char           models_dir[MN_STR_PATH];
    mn_app_dl_ctx *ctx;

    if (!app || !repo || !file || !repo[0] || !file[0]) return false;

    mn_app_models_dir(app, models_dir, sizeof(models_dir));
    if (!models_dir[0]) return false;

    ctx = (mn_app_dl_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    mn_copy_str(ctx->id, sizeof(ctx->id), id ? id : "");
    ctx->cb   = cb;
    ctx->user = user;

    if (!mn_modeldl_start(repo, file, save_as, models_dir,
                          mn_app_dl_trampoline, ctx)) {
        free(ctx);
        return false;   /* busy or invalid */
    }
    return true;
}

/* ================================================================== */
/* Browsing: view / search / sort                                    */
/* ================================================================== */

void mn_app_set_view(mn_app *app, mn_view view)
{
    if (!app || view < 0 || view >= MN_VIEW_COUNT) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->view != view) {
        app->view = view;
        /* The album cache does NOT depend on the view (mn_build_spec only
         * carries search/liked/hidden) — invalidating it here forced a
         * multi-second synchronous rebuild on every Tracks<->Albums switch. */
        app->query_dirty = true;
    }
    MN_UNLOCK(&app->lib_lock);
}

mn_view mn_app_get_view(mn_app *app)
{
    mn_view v;
    if (!app) {
        return MN_VIEW_TRACKS;
    }
    MN_LOCK(&app->lib_lock);
    v = app->view;
    MN_UNLOCK(&app->lib_lock);
    return v;
}

void mn_app_set_search(mn_app *app, const char *query)
{
    if (!app) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    mn_copy_str(app->search, sizeof(app->search), query);
    app->query_dirty = true; app->alb_cache_valid = false;
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_set_sort(mn_app *app, mn_sort key, bool ascending)
{
    if (!app || key < 0 || key >= MN_SORT_COUNT) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->sort_key != key || app->sort_asc != ascending) {
        app->sort_key = key;
        app->sort_asc = ascending;
        /* Sort feeds the TRACK query only — the album cache is built with
         * sort_len=0 (label order), so it stays valid across sort changes. */
        app->query_dirty = true;
    }
    MN_UNLOCK(&app->lib_lock);
}

/* ================================================================== */
/* Browsing: windowed row access                                     */
/* ================================================================== */

int64_t mn_app_row_count(mn_app *app)
{
    int64_t n;
    if (!app) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    n = (mn_refresh_query(app) == MNDB_OK) ? app->row_count_cache : 0;
    MN_UNLOCK(&app->lib_lock);
    return n;
}

int32_t mn_app_window(mn_app *app, int64_t offset, int32_t count, mn_row *rows)
{
    const mn_track_row *db_rows = NULL;
    int32_t             got = 0;
    int32_t             i;

    if (!app || !rows || count <= 0 || offset < 0) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    if (mn_refresh_query(app) != MNDB_OK || !app->query) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (mn_query_window(app->query, offset, count, &db_rows, &got) != MNDB_OK || !db_rows) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (got > count) {
        got = count;
    }
    for (i = 0; i < got; ++i) {
        mn_fill_row(&rows[i], &db_rows[i]);
    }
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* Independent search over tracks for the live suggestions dropdown: builds a
 * throwaway spec with just the FTS query (does NOT touch the current view's
 * persistent query/sort/search). Sorted by title so results are stable. */
/* Release the CALLING thread's TLS SQLite reader connection. Worker threads
 * call this before exiting — without it each short-lived worker leaks one
 * connection (with its page cache) into the library's readers list. */
void mn_app_thread_detach(mn_app *app)
{
    if (app && app->lib) mn_library_thread_detach(app->lib);
}

/* Signal long-running background loops (art self-heal, purge, reconcile)
 * to bail out promptly. Called at UI teardown BEFORE mn_app_destroy so the
 * worker drain completes fast. Safe to call repeatedly. */
void mn_app_request_shutdown(mn_app *app)
{
    if (app) app->shutting_down = true;
}

/* Aggregate stats for one scan root (tracks / albums / bytes / newest
 * date_added under the directory). Reader connection; no app lock. */
/* Media-tool scoped window: rows under a path prefix, INDEPENDENT of the
 * current view/search/category state (its own local spec — reuses the
 * kind-roots prefix machinery as a plain include filter). Sorted
 * album -> disc/track so the tool can group rows into albums linearly. */
int32_t mn_app_tracks_under(mn_app *app, const char *prefix, int64_t offset,
                            int32_t count, mn_row *rows, int64_t *out_total)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *db_rows = NULL;
    int32_t              got = 0, i;

    if (out_total) *out_total = 0;
    if (!app || !app->lib || !rows || count <= 0 || !prefix || !prefix[0]) {
        return 0;
    }
    memset(&spec, 0, sizeof(spec));
    spec.include_missing = false;
    spec.kind_include    = true;
    spec.kind_roots_len  = 1;
    mn_copy_str(spec.kind_roots[0], sizeof(spec.kind_roots[0]), prefix);
    spec.sort[0].key = MNDB_SORT_ALBUM; spec.sort[0].descending = false;
    spec.sort[1].key = MNDB_SORT_TRACK; spec.sort[1].descending = false;
    spec.sort_len = 2;

    MN_LOCK(&app->lib_lock);
    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (out_total) (void)mn_query_count(q, out_total);
    if (mn_query_window(q, offset, count, &db_rows, &got) == MNDB_OK && db_rows) {
        for (i = 0; i < got && i < count; i++) mn_fill_row(&rows[i], &db_rows[i]);
    } else {
        got = 0;
    }
    mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

bool mn_app_root_stats(mn_app *app, const char *root,
                       int64_t *tracks, int64_t *albums,
                       int64_t *bytes, int64_t *newest_added)
{
    if (!app || !app->lib || !root || !root[0]) return false;
    return mn_library_prefix_stats(app->lib, root, tracks, albums,
                                   bytes, newest_added) == MNDB_OK;
}

/* Online backup of the library db (SQLite Backup API on the calling
 * thread's own reader connection — no app lock; safe during scans). */
bool mn_app_backup_db(mn_app *app, const char *dest_path)
{
    if (!app || !app->lib || !dest_path || !dest_path[0]) return false;
    return mn_library_backup(app->lib, dest_path) == MNDB_OK;
}

int32_t mn_app_search_tracks(mn_app *app, const char *query,
                             int32_t count, mn_row *rows)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *db_rows = NULL;
    int32_t              got = 0, i;

    if (!app || !app->lib || !rows || count <= 0 || !query || !query[0]) {
        return 0;
    }
    memset(&spec, 0, sizeof(spec));
    spec.fts_match = query;
    spec.include_missing = false;
    spec.sort[0].key = MNDB_SORT_TITLE;
    spec.sort[0].descending = false;
    spec.sort_len = 1;

    MN_LOCK(&app->lib_lock);
    mn_spec_apply_hidden(app, &spec);
    if (mn_query_open(app->lib, &spec, &q) == MNDB_OK && q) {
        if (mn_query_window(q, 0, count, &db_rows, &got) == MNDB_OK && db_rows) {
            if (got > count) got = count;
            for (i = 0; i < got; ++i) mn_fill_row(&rows[i], &db_rows[i]);
        } else {
            got = 0;
        }
        mn_query_close(q);
    }
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* ================================================================== */
/* Browsing: albums (via the ALBUM facet)                            */
/* ================================================================== */

int64_t mn_app_album_count(mn_app *app)
{
    mn_filter_spec spec;
    mn_facet      *f = NULL;
    int64_t        n = 0;

    if (!app || !app->lib) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);
    if (app->alb_cache_valid) {
        n = app->alb_cache_total ? app->alb_cache_total : app->alb_cache_n;
        MN_UNLOCK(&app->lib_lock);
        return n;
    }
    mn_build_spec(app, &spec);
    /* Facets don't sort by track columns; only fts_match matters here. */
    spec.sort_len = 0;

    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) != MNDB_OK || !f) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    (void)mn_facet_count(f, &n);
    mn_facet_close(f);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* ---- album cache ordering ------------------------------------------- */
/* qsort comparator config — set under lib_lock immediately before qsort,
 * so the plain-qsort globals are race-free. */
static mn_sort g_alb_cmp_key = MN_SORT_TITLE;
static bool    g_alb_cmp_asc = true;

static int mn__alb_cmp(const void *pa, const void *pb) {
    const mn_album *a = (const mn_album *)pa, *b = (const mn_album *)pb;
    int r, ra, rb;

    /* Untagged/implausible key values group at the BOTTOM in BOTH sort
     * directions (MediaMonkey behavior) — so the rank comparison happens
     * before the direction flip at the bottom of this function. */
    switch (g_alb_cmp_key) {
        case MN_SORT_ARTIST:
            ra = a->artist[0] == '\0'; rb = b->artist[0] == '\0'; break;
        case MN_SORT_YEAR:
            ra = a->year < 1000 || a->year > 2100;
            rb = b->year < 1000 || b->year > 2100; break;
        case MN_SORT_DATE_CREATED:
            ra = a->created <= 0; rb = b->created <= 0; break;
        default:
            ra = a->title[0] == '\0'; rb = b->title[0] == '\0'; break;
    }
    if (ra != rb) return ra - rb;

    switch (g_alb_cmp_key) {
        case MN_SORT_ARTIST:
            r = _stricmp(a->artist, b->artist);
            if (r == 0) r = (a->year > b->year) - (a->year < b->year);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_YEAR:
            r = (a->year > b->year) - (a->year < b->year);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_DATE_ADDED:
            r = (a->date_added > b->date_added) - (a->date_added < b->date_added);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_DATE_CREATED:
            r = (a->created > b->created) - (a->created < b->created);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_DURATION:
            /* no per-album duration in the cache; total byte size is the
             * closest meaningful proxy for "longest albums" */
            r = (a->size > b->size) - (a->size < b->size);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_TRACK_NO:
        case MN_SORT_PLAY_COUNT:
            /* albums carry no play count; track count is the nearest
             * useful ordering for both */
            r = (a->track_count > b->track_count) - (a->track_count < b->track_count);
            if (r == 0) r = _stricmp(a->title, b->title);
            break;
        case MN_SORT_TITLE:
        case MN_SORT_ALBUM:
        case MN_SORT_GENRE:   /* no album-level genre — stable title order */
        case MN_SORT_RATING:  /* no album-level rating — stable title order */
        default:
            r = _stricmp(a->title, b->title);
            break;
    }
    return g_alb_cmp_asc ? r : -r;
}

/* Bring the cached album array into the app's CURRENT sort key/direction.
 * No-op when already ordered that way; otherwise one qsort (~µs at 2k
 * albums). Caller holds lib_lock. */
static void mn_app_album_cache_sort_locked(mn_app *app) {
    if (app->alb_sorted_key == app->sort_key &&
        app->alb_sorted_asc == app->sort_asc) {
        return;
    }
    if (app->alb_cache_valid && app->alb_cache && app->alb_cache_n > 1) {
        g_alb_cmp_key = app->sort_key;
        g_alb_cmp_asc = app->sort_asc;
        qsort(app->alb_cache, (size_t)app->alb_cache_n, sizeof(mn_album),
              mn__alb_cmp);
    }
    app->alb_sorted_key = app->sort_key;
    app->alb_sorted_asc = app->sort_asc;
}

/* Rebuild the full album-detail cache (facet label order). Caller holds the
 * app lock. Bounded: one facet pass + one small track window per album; the
 * heavy art extraction is a one-time cost per album (thumbnails persist). */
/* ---- parallel album-detail fill -------------------------------------
 * The build's cost is one small track query per album — 875 ms SERIAL at
 * 1,933 albums (measured). The queries are pure reads on per-thread WAL
 * reader connections, so a small worker pool fills the array in parallel
 * while the caller keeps holding lib_lock. Workers touch ONLY their own
 * seed/slot and their own reader connection — never mn_app state (the
 * hidden-folder config they read through the spec template cannot mutate
 * because every mutator needs the lock the caller is holding). */
typedef struct mn_alb_seed {
    int64_t id;
    int64_t count;
    char    label[MN_STR_SHORT];
} mn_alb_seed;

typedef struct mn_alb_fill_ctx {
    mn_app            *app;
    const mn_alb_seed *seeds;
    mn_album          *out;
    LONG               n;
    volatile LONG      next;          /* atomic index dispenser            */
} mn_alb_fill_ctx;

/* Art-stat phase worker body: a cheap check-only cover lookup per album.
 * Pure filesystem stats — no DB access, so workers scale cleanly. Uses
 * mn_art_ensure directly, NOT mn_app_art_path: that wrapper takes the app
 * lock (which the build's caller holds — a worker would deadlock) and
 * returns a SHARED scratch buffer. Never extracts art here; the artscan
 * worker heals misses in the background. */
static void mn_app_album_art_one(mn_alb_fill_ctx *cx, int32_t i)
{
    mn_album *a = &cx->out[i];
    char      key[MN_STR_SHORT * 2];
    char      art[MN_ART_PATH_MAX];

    if (!a->title[0]) return;
    snprintf(key, sizeof(key), "%s\x1f%s", a->artist, a->title);
    if (mn_art_ensure(cx->app->art_cache_dir, key, NULL, art, sizeof(art))) {
        mn_copy_str(a->art_path, sizeof(a->art_path), art);
    }
}

#ifdef _WIN32
static DWORD WINAPI mn_alb_fill_worker(LPVOID param)
{
    mn_alb_fill_ctx *cx = (mn_alb_fill_ctx *)param;
    for (;;) {
        LONG i = InterlockedIncrement(&cx->next) - 1;
        if (i >= cx->n) break;
        mn_app_album_art_one(cx, (int32_t)i);
    }
    return 0;
}
#endif

static void mn_app_album_cache_build_locked(mn_app *app)
{
    mn_filter_spec       spec;
    mn_facet            *f = NULL;
    const mn_facet_row  *frows = NULL;
    mn_alb_seed         *seeds = NULL;
    int64_t              total = 0;
    int64_t              off;
    int32_t              got = 0;
    int32_t              i;
    int32_t              w = 0;

    /* Scalability guard: the full-detail build costs one small track query
     * per album, so cap it. Beyond the cap (absurdly large libraries) the
     * window path serves basic facet rows (title/count) directly — browsing
     * never breaks, detail pills just degrade past the cap. */
    enum { MN_ALB_CACHE_MAX = 8000 };

    free(app->alb_cache);
    app->alb_cache = NULL;
    app->alb_cache_n = 0;
    app->alb_cache_total = 0;
    app->alb_cache_valid = false;

    ULONGLONG tb0 = GetTickCount64(), tb1, tb2, tb3;

    mn_build_spec(app, &spec);
    spec.sort_len = 0;

    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) != MNDB_OK || !f) {
        return;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    (void)mn_facet_count(f, &total);
    if (total <= 0) {
        mn_facet_close(f);
        app->alb_cache_valid = true;   /* empty library: valid empty cache */
        app->alb_sorted_key = MN_SORT_TITLE;
        app->alb_sorted_asc = true;
        return;
    }
    app->alb_cache_total = (int32_t)total;
    if (total > MN_ALB_CACHE_MAX) total = MN_ALB_CACHE_MAX;

    /* Phase 1: materialize the facet rows into an owned seed array (facet
     * row pointers only live until the next window call). */
    seeds = (mn_alb_seed *)calloc((size_t)total, sizeof(mn_alb_seed));
    app->alb_cache = (mn_album *)calloc((size_t)total, sizeof(mn_album));
    if (!seeds || !app->alb_cache) {
        free(seeds);
        free(app->alb_cache);
        app->alb_cache = NULL;
        mn_facet_close(f);
        return;                        /* stays invalid; retried next call */
    }
    for (off = 0; off < total && w < (int32_t)total; off += got) {
        if (mn_facet_window(f, off, 256, &frows, &got) != MNDB_OK
            || !frows || got <= 0) {
            break;
        }
        for (i = 0; i < got && w < (int32_t)total; ++i, ++w) {
            seeds[w].id    = frows[i].value_id;
            seeds[w].count = frows[i].count;
            mn_copy_str(seeds[w].label, sizeof(seeds[w].label), frows[i].label);
        }
    }
    mn_facet_close(f);
    tb1 = GetTickCount64();

    /* Seed the result skeletons + an ALBUM-ID -> slot hash map. IDENTITY,
     * not title: album names are NOT unique (the live library has 21
     * duplicate-name albums plus case variants like "Donda 2"/"DONDA 2");
     * a title-keyed map funneled every duplicate's rows into ONE slot and
     * left its twin with no artist, no art and zero size — the artless
     * ghost albums that clustered at the top of the grid. The facet's
     * value_id IS tracks.album_id, so the id joins exactly. */
    {
        size_t    hcap = 64;
        struct { int64_t id; int32_t slot; bool used; } *hmap;
        while (hcap < (size_t)w * 2) hcap <<= 1;
        hmap = calloc(hcap, sizeof(*hmap));
        if (!hmap) {
            free(seeds);
            free(app->alb_cache); app->alb_cache = NULL;
            return;
        }
        for (i = 0; i < w; ++i) {
            mn_album *a = &app->alb_cache[i];
            uint64_t  h;
            a->id          = seeds[i].id;
            a->track_count = (int32_t)seeds[i].count;
            mn_copy_str(a->title, sizeof(a->title), seeds[i].label);
            h = (uint64_t)seeds[i].id * 1099511628211ULL + 1469598103934665603ULL;
            {
                size_t m = hcap - 1, j = (size_t)h & m;
                while (hmap[j].used) j = (j + 1) & m;
                hmap[j].id = seeds[i].id; hmap[j].slot = i; hmap[j].used = true;
            }
        }

        /* Phase 2: ONE streaming pass over all tracks sorted by album
         * (covering index idx_tracks_sort_album), aggregating per album:
         * first row seen = lead track (disc/track order within the album)
         * -> artist/format/year/rates/date_added; size sums every row.
         * This replaced 1,933 per-album queries (875 ms serial; a
         * parallel-query variant was MEASURED SLOWER at 1,234 ms due to
         * WAL read-slot contention + cold per-connection page caches). */
        {
            mn_filter_spec qspec;
            mn_query      *q = NULL;
            memset(&qspec, 0, sizeof(qspec));
            qspec.sort[0].key = MNDB_SORT_ALBUM;
            qspec.sort[0].descending = false;
            qspec.sort_len = 1;
            mn_spec_apply_hidden(app, &qspec);

            if (mn_query_open(app->lib, &qspec, &q) == MNDB_OK && q) {
                const mn_track_row *tr = NULL;
                int32_t tn = 0;
                /* ONE window for the whole table: OFFSET-paged windows
                 * re-walk the index from the start per page (O(n²) — the
                 * 1024-row paged variant measured 891 ms; this is ~90 ms).
                 * The arena holds every row transiently (~10-30 MB), freed
                 * at query close. */
                if (mn_query_window(q, 0, 10 * 1000 * 1000, &tr, &tn) == MNDB_OK
                    && tr && tn > 0) {
                    for (i = 0; i < tn; ++i) {
                        const mn_track_row *r = &tr[i];
                        int32_t slot = -1;
                        if (r->album_id <= 0) continue;
                        {
                            uint64_t h = (uint64_t)r->album_id * 1099511628211ULL
                                       + 1469598103934665603ULL;
                            size_t m = hcap - 1, j = (size_t)h & m;
                            while (hmap[j].used) {
                                if (hmap[j].id == r->album_id) { slot = hmap[j].slot; break; }
                                j = (j + 1) & m;
                            }
                        }
                        if (slot < 0) continue;      /* not in the (capped) cache */
                        {
                            mn_album *a = &app->alb_cache[slot];
                            if (!a->artist[0] && !a->date_added) {
                                /* first row of this album (disc/track order) */
                                const char *aa =
                                    (r->album_artist && r->album_artist[0])
                                    ? r->album_artist : r->artist;
                                mn_copy_str(a->artist, sizeof(a->artist), aa);
                                mn_copy_str(a->format, sizeof(a->format), r->format);
                                a->year         = r->year;
                                a->sample_rate  = r->sample_rate;
                                a->bit_depth    = r->bit_depth;
                                a->bitrate_kbps = r->bitrate_kbps;
                                a->date_added   = r->date_added;
                            }
                            if (r->created > a->created)
                                a->created = r->created;   /* newest file */
                            a->size += r->size;
                        }
                    }
                }
                mn_query_close(q);
            }
        }
        free(hmap);
    }
    free(seeds);
    tb2 = GetTickCount64();

    /* Phase 3: cover checks (one filesystem stat per album) — parallel on
     * Windows; pure FS work, no DB handles, scales linearly. */
    {
        mn_alb_fill_ctx cx;
        cx.app   = app;
        cx.seeds = NULL;
        cx.out   = app->alb_cache;
        cx.n     = (LONG)w;
        cx.next  = 0;
#ifdef _WIN32
        {
            SYSTEM_INFO si;
            HANDLE th[7];
            int    nt, t;
            GetSystemInfo(&si);
            nt = (int)si.dwNumberOfProcessors - 1;
            if (nt > 7) nt = 7;
            if (nt > w - 1) nt = w - 1;
            if (w < 64) nt = 0;             /* tiny builds: threads not worth it */
            if (nt < 0) nt = 0;
            for (t = 0; t < nt; ++t) {
                th[t] = CreateThread(NULL, 0, mn_alb_fill_worker, &cx, 0, NULL);
                if (!th[t]) { nt = t; break; }
            }
            for (;;) {
                LONG j = InterlockedIncrement(&cx.next) - 1;
                if (j >= cx.n) break;
                mn_app_album_art_one(&cx, (int32_t)j);
            }
            if (nt > 0) {
                WaitForMultipleObjects((DWORD)nt, th, TRUE, INFINITE);
                for (t = 0; t < nt; ++t) CloseHandle(th[t]);
            }
        }
#else
        for (i = 0; i < w; ++i) mn_app_album_art_one(&cx, i);
#endif
    }

    tb3 = GetTickCount64();
    fprintf(stderr, "[albcache] phases: facet=%llums agg=%llums art=%llums\n",
            (unsigned long long)(tb1 - tb0),
            (unsigned long long)(tb2 - tb1),
            (unsigned long long)(tb3 - tb2));

    app->alb_cache_n = w;
    app->alb_cache_valid = true;
    /* the facet pass delivered label (title) ascending order */
    app->alb_sorted_key = MN_SORT_TITLE;
    app->alb_sorted_asc = true;
}

int32_t mn_app_album_window(mn_app *app, int64_t offset, int32_t count, mn_album *albums)
{
    int32_t got = 0;

    if (!app || !app->lib || !albums || count <= 0 || offset < 0) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);
    if (!app->alb_cache_valid) {
        /* benchmark instrumentation: the build is the album view's one
         * serial cost — log it so regressions (and the win from a future
         * parallel build) are measurable in the console. */
        ULONGLONG t0 = GetTickCount64();
        mn_app_album_cache_build_locked(app);
        fprintf(stderr, "[albcache] rebuilt %d albums in %llums\n",
                app->alb_cache_n,
                (unsigned long long)(GetTickCount64() - t0));
    }
    /* serve in the CURRENT sort order (dropdown + asc/desc apply to albums) */
    mn_app_album_cache_sort_locked(app);
    if (app->alb_cache_valid && app->alb_cache && offset < app->alb_cache_n) {
        got = (int32_t)(app->alb_cache_n - offset);
        if (got > count) got = count;
        memcpy(albums, app->alb_cache + offset, (size_t)got * sizeof(mn_album));
    } else if (app->alb_cache_valid && offset < app->alb_cache_total) {
        /* Past the full-detail cache cap (very large libraries): serve basic
         * facet rows directly so the grid keeps scrolling forever. */
        mn_filter_spec       spec;
        mn_facet            *f = NULL;
        const mn_facet_row  *frows = NULL;
        int32_t              n = 0, i;
        mn_build_spec(app, &spec);
        spec.sort_len = 0;
        if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) == MNDB_OK && f) {
            (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
            if (mn_facet_window(f, offset, count, &frows, &n) == MNDB_OK && frows) {
                if (n > count) n = count;
                for (i = 0; i < n; ++i) {
                    mn_album *a = &albums[i];
                    memset(a, 0, sizeof(*a));
                    a->id = frows[i].value_id;
                    mn_copy_str(a->title, sizeof(a->title),
                                frows[i].label ? frows[i].label : "");
                    a->track_count = (int32_t)frows[i].count;
                }
                got = n;
            }
            mn_facet_close(f);
        }
    }
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* Generic facet-value window (Artists / Genres / Composers / Years browse).
 * `dim` is an mn_facet_dim; fills out[i] = {id, label, count}. Returns the
 * number of rows written. */
int32_t mn_app_facet_window(mn_app *app, int dim, int64_t offset, int32_t count,
                            mn_facet_value *out)
{
    mn_facet            *f = NULL;
    const mn_facet_row  *frows = NULL;
    int32_t              got = 0, i;

    mn_filter_spec spec;

    if (!app || !app->lib || !out || count <= 0 || offset < 0) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    /* Apply the hidden-folder filter so Artists/Genres/Years agree with the
     * rest of the UI (they used to count + list hidden-folder tracks, and
     * disagree with the drill-in which DID filter). */
    memset(&spec, 0, sizeof(spec));
    mn_spec_apply_hidden(app, &spec);
    if (mn_facet_open(app->lib, (mn_facet_dim)dim, &spec, &f) != MNDB_OK || !f) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    if (mn_facet_window(f, offset, count, &frows, &got) != MNDB_OK || !frows) {
        mn_facet_close(f);
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (got > count) got = count;
    for (i = 0; i < got; ++i) {
        out[i].id = frows[i].value_id;
        mn_copy_str(out[i].label, sizeof(out[i].label),
                    frows[i].label ? frows[i].label : "");
        out[i].count = (int32_t)frows[i].count;
    }
    mn_facet_close(f);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* Total number of values in a facet dimension (for scrollbar sizing). */
int32_t mn_app_facet_count(mn_app *app, int dim)
{
    mn_facet      *f = NULL;
    int64_t        n = 0;
    mn_filter_spec spec;
    if (!app || !app->lib) return 0;
    MN_LOCK(&app->lib_lock);
    memset(&spec, 0, sizeof(spec));
    mn_spec_apply_hidden(app, &spec);   /* match the windowed list's filter */
    if (mn_facet_open(app->lib, (mn_facet_dim)dim, &spec, &f) == MNDB_OK && f) {
        (void)mn_facet_count(f, &n);
        mn_facet_close(f);
    }
    MN_UNLOCK(&app->lib_lock);
    return (int32_t)n;
}

/* Tracks under one facet value (drill-in): cascade on `dim`=value_id. */
int32_t mn_app_facet_tracks(mn_app *app, int dim, int64_t value_id,
                            int32_t count, mn_row *rows)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *db_rows = NULL;
    int32_t              got = 0, i;

    if (!app || !app->lib || !rows || count <= 0) {
        return 0;
    }
    memset(&spec, 0, sizeof(spec));
    spec.cascade[0].dim = (mn_facet_dim)dim;
    spec.cascade[0].value_id = value_id;
    spec.cascade_len = 1;
    spec.sort[0].key = MNDB_SORT_ARTIST;
    spec.sort[0].descending = false;
    spec.sort[1].key = MNDB_SORT_ALBUM;
    spec.sort[1].descending = false;
    spec.sort[2].key = MNDB_SORT_TRACK;
    spec.sort[2].descending = false;
    spec.sort_len = 3;

    MN_LOCK(&app->lib_lock);
    mn_spec_apply_hidden(app, &spec);
    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (mn_query_window(q, 0, count, &db_rows, &got) == MNDB_OK && db_rows) {
        if (got > count) got = count;
        for (i = 0; i < got; ++i) {
            mn_fill_row(&rows[i], &db_rows[i]);
        }
    }
    mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* ---- Playlists (thin wrappers over the mn_playlist_* API) --------------- */

int32_t mn_app_playlist_list(mn_app *app, mn_playlist_item *out, int32_t max)
{
    mn_arena arena;
    const mn_playlist_row *rows = NULL;
    int32_t n = 0, i, w = 0;

    if (!app || !app->lib || !out || max <= 0) return 0;
    if (mn_arena_init(&arena, 16 * 1024) != MNDB_OK) return 0;
    MN_LOCK(&app->lib_lock);
    if (mn_playlist_list(app->lib, &arena, &rows, &n) == MNDB_OK && rows) {
        for (i = 0; i < n && w < max; ++i) {
            out[w].id = rows[i].id;
            mn_copy_str(out[w].name, sizeof(out[w].name),
                        rows[i].name ? rows[i].name : "");
            out[w].track_count = (int32_t)rows[i].track_count;
            w++;
        }
    }
    MN_UNLOCK(&app->lib_lock);
    mn_arena_free(&arena);
    return w;
}

int64_t mn_app_playlist_create(mn_app *app, const char *name)
{
    int64_t id = 0;
    if (!app || !app->lib || !name || !name[0]) return 0;
    MN_LOCK(&app->lib_lock);
    if (mn_playlist_create(app->lib, name, &id) != MNDB_OK) id = 0;
    MN_UNLOCK(&app->lib_lock);
    return id;
}

int mn_app_playlist_rename(mn_app *app, int64_t id, const char *name)
{
    int ok = 0;
    if (!app || !app->lib || id <= 0 || !name || !name[0]) return 0;
    MN_LOCK(&app->lib_lock);
    ok = (mn_playlist_rename(app->lib, id, name) == MNDB_OK);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_playlist_delete(mn_app *app, int64_t id)
{
    int ok = 0;
    if (!app || !app->lib || id <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    ok = (mn_playlist_delete(app->lib, id) == MNDB_OK);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_playlist_add(mn_app *app, int64_t id, int64_t track_id)
{
    int ok = 0;
    if (!app || !app->lib || id <= 0 || track_id <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    ok = (mn_playlist_add(app->lib, id, track_id, NULL) == MNDB_OK);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_playlist_remove_at(mn_app *app, int64_t id, int64_t position)
{
    int ok = 0;
    if (!app || !app->lib || id <= 0 || position < 0) return 0;
    MN_LOCK(&app->lib_lock);
    ok = (mn_playlist_remove_at(app->lib, id, position) == MNDB_OK);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_playlist_move(mn_app *app, int64_t id, int64_t from, int64_t to)
{
    int ok = 0;
    if (!app || !app->lib || id <= 0 || from < 0 || to < 0) return 0;
    MN_LOCK(&app->lib_lock);
    ok = (mn_playlist_move(app->lib, id, from, to) == MNDB_OK);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

/* Copy a playlist's tracks (in playlist order) into rows[]. */
int32_t mn_app_playlist_tracks(mn_app *app, int64_t id, int32_t count, mn_row *rows)
{
    mn_query           *q = NULL;
    const mn_track_row *db_rows = NULL;
    int32_t             got = 0, i;

    if (!app || !app->lib || !rows || count <= 0 || id <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    if (mn_playlist_query(app->lib, id, NULL, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (mn_query_window(q, 0, count, &db_rows, &got) == MNDB_OK && db_rows) {
        if (got > count) got = count;
        for (i = 0; i < got; ++i) mn_fill_row(&rows[i], &db_rows[i]);
    }
    mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

int32_t mn_app_album_tracks(mn_app *app, int64_t album_id, int32_t count, mn_row *rows)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *db_rows = NULL;
    int32_t              got = 0;
    int32_t              i;

    if (!app || !app->lib || !rows || count <= 0 || album_id == MN_INVALID_ID) {
        return 0;
    }

    memset(&spec, 0, sizeof(spec));
    spec.cascade[0].dim = MN_FACET_ALBUM;
    spec.cascade[0].value_id = album_id;
    spec.cascade_len = 1;
    spec.sort[0].key = MNDB_SORT_TRACK;   /* disc then track number */
    spec.sort[0].descending = false;
    spec.sort_len = 1;

    MN_LOCK(&app->lib_lock);
    mn_spec_apply_hidden(app, &spec);
    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (mn_query_window(q, 0, count, &db_rows, &got) != MNDB_OK || !db_rows) {
        mn_query_close(q);
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (got > count) {
        got = count;
    }
    for (i = 0; i < got; ++i) {
        mn_fill_row(&rows[i], &db_rows[i]);
    }
    mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* ================================================================== */
/* Browsing: folders + selective visibility                           */
/* ================================================================== */

int32_t mn_app_folder_list(mn_app *app, mn_folder *out, int32_t max)
{
    mn_facet           *f = NULL;
    const mn_facet_row *frows = NULL;
    int32_t             got = 0;
    int32_t             i;

    if (!app || !app->lib || !out || max <= 0) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);
    /* NULL spec: unfiltered facet, so HIDDEN folders still list (the UI
     * needs them to offer the unhide checkbox). Counts are non-missing
     * track counts computed live by the facet engine. */
    if (mn_facet_open(app->lib, MN_FACET_FOLDER, NULL, &f) != MNDB_OK || !f) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    if (mn_facet_window(f, 0, max, &frows, &got) != MNDB_OK || !frows) {
        mn_facet_close(f);
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    if (got > max) {
        got = max;
    }
    /* Only list TOP-LEVEL library folders — the roots the user actually added —
     * not every subfolder the scan recursed into. A facet row is a "root" iff
     * its path is not contained within any OTHER facet row's path. This keeps
     * the folder-management UI to the handful of added masters, and hiding a
     * root still hides everything beneath it (the exclusion filter matches by
     * folder id, but the browse query also excludes descendants via path). */
    {
        int32_t w = 0;
        int32_t j;
        for (i = 0; i < got; ++i) {
            bool is_child = false;
            for (j = 0; j < got; ++j) {
                if (j == i) continue;
                /* frows[i] is a child if some other row is a path-prefix of it */
                if (mn_app_path_under(frows[j].label, frows[i].label)) {
                    is_child = true;
                    break;
                }
            }
            if (is_child) continue;                 /* skip subfolders */
            out[w].id          = frows[i].value_id;
            out[w].track_count = frows[i].count;
            out[w].hidden      = mn_app_hidden_contains(app, frows[i].value_id);
            mn_copy_str(out[w].path, sizeof(out[w].path), frows[i].label);
            w++;
        }
        got = w;
    }
    mn_facet_close(f);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

bool mn_app_folder_set_hidden(mn_app *app, int64_t folder_id, bool hidden)
{
    bool     ok = true;
    bool     changed = false;
    int      i;
    int64_t  ids[MN_MAX_EXCLUDED_FOLDERS];
    int32_t  nids;
    int32_t  k;

    if (!app || folder_id <= 0) {
        return false;
    }

    /* Expand to the folder's whole SUBTREE: `folders` rows exist per
     * DIRECT parent dir, and the browse filter excludes by folder_id —
     * hiding only the clicked id left tracks in nested album dirs visible
     * everywhere ("hidden folder still shows its albums" bug). */
    nids = mn_library_folder_subtree(app->lib, folder_id, ids,
                                     MN_MAX_EXCLUDED_FOLDERS);
    if (nids <= 0) { ids[0] = folder_id; nids = 1; }
    if (nids > MN_MAX_EXCLUDED_FOLDERS) {
        nids = MN_MAX_EXCLUDED_FOLDERS;   /* truncated: partial but honest */
        ok = false;
    }

    MN_LOCK(&app->lib_lock);
    for (k = 0; k < nids; ++k) {
        int64_t fid = ids[k];
        if (hidden) {
            if (!mn_app_hidden_contains(app, fid)) {
                if (app->hidden_folder_count < MN_MAX_EXCLUDED_FOLDERS) {
                    app->hidden_folders[app->hidden_folder_count++] = fid;
                    changed = true;
                } else {
                    ok = false;   /* set full — cannot hide more folders */
                    break;
                }
            }
        } else {
            for (i = 0; i < app->hidden_folder_count; ++i) {
                if (app->hidden_folders[i] == fid) {
                    /* Order is irrelevant: swap-remove with the last entry. */
                    app->hidden_folders[i] =
                        app->hidden_folders[app->hidden_folder_count - 1];
                    app->hidden_folder_count--;
                    changed = true;
                    break;
                }
            }
        }
    }
    if (changed) {
        mn_app_save_hidden(app);
        app->query_dirty = true; app->alb_cache_valid = false;   /* recompile the browse query/counts */
    }
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

bool mn_app_folder_hidden(mn_app *app, int64_t folder_id)
{
    bool h;
    if (!app || folder_id <= 0) {
        return false;
    }
    MN_LOCK(&app->lib_lock);
    h = mn_app_hidden_contains(app, folder_id);
    MN_UNLOCK(&app->lib_lock);
    return h;
}

int64_t mn_app_remove_folder(mn_app *app, int64_t folder_id)
{
    char    folder_path[MN_STR_PATH];
    int64_t deleted = 0;
    int     i;

    if (!app || !app->lib || folder_id <= 0) {
        return -1;
    }

    MN_LOCK(&app->lib_lock);

    /* Resolve the folder's path FIRST (needed to drop matching rescan
     * roots) — the db row is gone after the delete below. */
    folder_path[0] = '\0';
    {
        mn_facet           *f = NULL;
        const mn_facet_row *frows = NULL;
        int32_t             got = 0;
        int64_t             off = 0;

        if (mn_facet_open(app->lib, MN_FACET_FOLDER, NULL, &f) == MNDB_OK && f) {
            while (mn_facet_window(f, off, 128, &frows, &got) == MNDB_OK
                   && frows && got > 0) {
                for (i = 0; i < got; ++i) {
                    if (frows[i].value_id == folder_id) {
                        mn_copy_str(folder_path, sizeof(folder_path),
                                    frows[i].label);
                        break;
                    }
                }
                if (folder_path[0] || got < 128) {
                    break;
                }
                off += got;
            }
            mn_facet_close(f);
        }
    }

    /* Delete the folder's WHOLE SUBTREE: folders rows are per direct parent
     * dir, so deleting only the clicked id left tracks in nested album dirs
     * in the library ("removed folder still shows its albums" bug). */
    {
        int64_t subids[MN_MAX_EXCLUDED_FOLDERS];
        int32_t nsub = mn_library_folder_subtree(app->lib, folder_id, subids,
                                                 MN_MAX_EXCLUDED_FOLDERS);
        int32_t k;
        if (nsub <= 0) { subids[0] = folder_id; nsub = 1; }
        if (nsub > MN_MAX_EXCLUDED_FOLDERS) nsub = MN_MAX_EXCLUDED_FOLDERS;
        for (k = 0; k < nsub; ++k) {
            int64_t one = 0;
            if (mn_library_delete_folder(app->lib, subids[k], &one) == MNDB_OK) {
                deleted += one;
            }
            /* drop each from the hidden set (persisted below if changed) */
            for (i = 0; i < app->hidden_folder_count; ++i) {
                if (app->hidden_folders[i] == subids[k]) {
                    app->hidden_folders[i] =
                        app->hidden_folders[app->hidden_folder_count - 1];
                    app->hidden_folder_count--;
                    break;
                }
            }
        }
        mn_app_save_hidden(app);
    }

    /* Drop rescan roots that live at or under the removed folder, so a
     * later rescan does not resurrect it. Roots ABOVE the folder stay (they
     * cover siblings); their next scan may re-index the files if they still
     * exist on disk — removal of on-disk content is the user's call. */
    if (folder_path[0]) {
        for (i = 0; i < app->root_count; /* in-body */) {
            if (mn_app_path_under(folder_path, app->roots[i])) {
                mn_copy_str(app->roots[i], sizeof(app->roots[0]),
                            app->roots[app->root_count - 1]);
                app->root_count--;
            } else {
                ++i;
            }
        }
    }

    /* If the active track lived in that folder its row is gone; the cached
     * now-playing strings stay valid for the remainder of playback. */
    app->query_dirty = true; app->alb_cache_valid = false;   /* row/album counts + windows are stale */

    MN_UNLOCK(&app->lib_lock);
    return deleted;
}

/* ================================================================== */
/* Library statistics                                                 */
/* ================================================================== */

/* Threshold above which the per-track lyrics sidecar probe is skipped
 * (filesystem stat per track; bounded cost only for smaller libraries). */
#define MN_STATS_LYRICS_MAX 5000

/* Does a .lrc or .txt sidecar exist beside `path`? */
static bool mn_app_lyrics_sidecar_exists(const char *path)
{
    char    side[MN_STR_PATH + 8];
    size_t  n, stem;
    int64_t mt, sz;
    static const char *const exts[2] = { ".lrc", ".txt" };
    int i;

    n = strlen(path);
    if (n == 0 || n >= sizeof(side) - 8) {
        return false;
    }
    /* Find the extension dot (after the last path separator). */
    stem = n;
    for (size_t k = n; k > 0; --k) {
        char c = path[k - 1];
        if (c == '\\' || c == '/') {
            break;
        }
        if (c == '.') {
            stem = k - 1;
            break;
        }
    }
    for (i = 0; i < 2; ++i) {
        memcpy(side, path, stem);
        side[stem] = '\0';
        strcat(side, exts[i]);
        if (mn_app_stat_file(side, &mt, &sz)) {
            return true;
        }
    }
    return false;
}

bool mn_app_get_stats(mn_app *app, mn_app_stats *out)
{
    mn_stats_ext ext;
    int i;

    if (!app || !app->lib || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    MN_LOCK(&app->lib_lock);
    if (mn_library_stats_ext(app->lib, &ext) != MNDB_OK) {
        MN_UNLOCK(&app->lib_lock);
        return false;
    }

    out->tracks      = ext.track_count;
    out->albums      = ext.album_count;
    out->artists     = ext.artist_count;
    out->missing     = ext.missing_count;
    out->duration_ms = ext.total_duration_ms;
    out->size_bytes  = ext.total_size_bytes;
    out->hires_pct   = (ext.track_count > 0)
                     ? (float)(100.0 * (double)ext.hires_count
                                     / (double)ext.track_count)
                     : 0.0f;
    out->format_count = ext.format_count;
    if (out->format_count > MN_APP_STATS_MAX_FORMATS) {
        out->format_count = MN_APP_STATS_MAX_FORMATS;
    }
    for (i = 0; i < out->format_count; ++i) {
        mn_copy_str(out->formats[i].fmt, sizeof(out->formats[i].fmt),
                    ext.formats[i].fmt);
        out->formats[i].n = ext.formats[i].n;
    }

    /* Everything above was a cheap aggregate query — release the lock now.
     * The lyrics-coverage probe below stat()s thousands of files on disk;
     * holding lib_lock across it stalled the scanner + every other DB op. */
    MN_UNLOCK(&app->lib_lock);

    /* Lyrics coverage: sidecar existence probe, bounded to small/medium
     * libraries. Paths are gathered UNDER the lock (fast), then stat'd
     * WITHOUT it. -1 = "not computed". */
    out->lyrics_pct = -1.0f;
    if (ext.track_count > 0 && ext.track_count <= MN_STATS_LYRICS_MAX) {
        char  (*paths)[MN_STR_PATH] = NULL;
        int64_t npaths = 0, cap = 0, with = 0;
        mn_filter_spec      spec;
        mn_query           *q = NULL;
        const mn_track_row *rows = NULL;
        int32_t             got = 0;
        int64_t             off = 0, k;

        memset(&spec, 0, sizeof(spec));
        spec.include_missing = false;

        MN_LOCK(&app->lib_lock);
        if (mn_query_open(app->lib, &spec, &q) == MNDB_OK && q) {
            cap = ext.track_count + 8;
            paths = (char (*)[MN_STR_PATH])malloc((size_t)cap * MN_STR_PATH);
            while (paths && mn_query_window(q, off, 512, &rows, &got) == MNDB_OK
                   && rows && got > 0) {
                for (i = 0; i < got && npaths < cap; ++i) {
                    mn_copy_str(paths[npaths], MN_STR_PATH, rows[i].path);
                    npaths++;
                }
                if (got < 512) break;
                off += got;
            }
            mn_query_close(q);
        }
        MN_UNLOCK(&app->lib_lock);

        /* stat the sidecars with the lock released */
        for (k = 0; k < npaths; ++k) {
            if (mn_app_lyrics_sidecar_exists(paths[k])) with++;
        }
        if (npaths > 0) {
            out->lyrics_pct = (float)(100.0 * (double)with / (double)npaths);
        }
        free(paths);
    }
    return true;
}

/* ================================================================== */
/* Playback internals                                                 */
/* ================================================================== */

/* Cache the display strings + id for the now-playing track. */
static void mn_app_cache_now(mn_app *app, const mn_track_row *r)
{
    if (!app || !r) {
        return;
    }
    app->now_track_id = r->id;
    app->now_album_id = r->album_id;
    app->now_liked    = r->liked;
    mn_copy_str(app->now_path,   sizeof(app->now_path),   r->path);
    mn_copy_str(app->now_title,  sizeof(app->now_title),  r->title);
    mn_copy_str(app->now_artist, sizeof(app->now_artist), r->artist);
    mn_copy_str(app->now_album,  sizeof(app->now_album),  r->album);
    /* Album artist with track-artist fallback: MUST mirror the art-cache key
     * built at scan time in mn_app_on_track ("<album_artist-or-artist>"), or
     * now-playing art lookups miss for VA / feat. tracks. */
    mn_copy_str(app->now_album_artist, sizeof(app->now_album_artist),
                (r->album_artist && r->album_artist[0]) ? r->album_artist
                                                        : r->artist);
    /* Authoritative SOURCE format facts from the tag scan. The engine's
     * decoder reports its own decode format (f32 -> "32 bit") for most
     * codecs, which is the pipeline format, NOT the file's. */
    app->now_src_rate    = r->sample_rate;
    app->now_src_bits    = r->bit_depth;
    app->now_src_kbps    = r->bitrate_kbps;
    mn_copy_str(app->now_src_format, sizeof(app->now_src_format),
                r->format ? r->format : "");
}

/* Start stem separation for the given track+path if stems are enabled. */
static void mn_app_kick_stems(mn_app *app, int64_t track_id, const char *path)
{
    if (!app || !app->stems || !app->stems_enabled || !path || path[0] == '\0') {
        return;
    }
    (void)mn_stems_start(app->stems, track_id, path);
    mn_stems_set_passthrough(app->stems, app->stems_passthrough);
}

/*
 * Build a playback queue from the current query window and start playing
 * at the entry whose id == start_id. THE QUEUE IS ALWAYS THE VISIBLE LIST:
 * the compiled view/search/sort/folder-filter query, in displayed order —
 * "queue = the list you played from" (play_album stays the explicit
 * album-queue path). For lists larger than MN_QUEUE_BUILD_MAX the window
 * is centered on the clicked row (located via mn_query_index_of) so the
 * clicked track is ALWAYS in the queue — previously a row outside the
 * first window silently played the list's first track instead. Returns
 * true if playback started.
 */
static bool mn_app_play_from_query(mn_app *app, int64_t start_id)
{
    const mn_track_row *db_rows = NULL;
    int32_t             got = 0;
    int64_t             total;
    int32_t             build;
    int64_t             win_off = 0;
    mn_track           *queue = NULL;
    size_t              qn = 0;
    size_t              start_index = 0;
    bool                found = false;
    int32_t             i;

    if (mn_refresh_query(app) != MNDB_OK || !app->query) {
        return false;
    }

    total = app->row_count_cache;
    if (total <= 0) {
        return false;
    }
    build = (total > MN_QUEUE_BUILD_MAX) ? MN_QUEUE_BUILD_MAX : (int32_t)total;

    /* Locate the clicked row within the current ordering so the built
     * window is guaranteed to contain it (centered when possible). */
    if (total > build && start_id > 0) {
        int64_t idx = -1;
        if (mn_query_index_of(app->query, start_id, &idx) == MNDB_OK
            && idx >= build) {
            win_off = idx - build / 2;
            if (win_off + build > total) win_off = total - build;
            if (win_off < 0)             win_off = 0;
        }
    }

    if (mn_query_window(app->query, win_off, build, &db_rows, &got) != MNDB_OK || !db_rows || got <= 0) {
        return false;
    }

    queue = (mn_track *)calloc((size_t)got, sizeof(mn_track));
    if (!queue) {
        return false;
    }

    /* The db rows live in the query arena and are invalidated by the next
     * window() call, so we snapshot the id + a stable path pointer into
     * the queue. mn_playback_set_queue deep-copies the paths immediately,
     * so it is safe to pass arena-owned strings here. */
    for (i = 0; i < got; ++i) {
        queue[qn].path        = db_rows[i].path;
        queue[qn].id          = db_rows[i].id;
        queue[qn].rg_track_db = MN_RG_UNKNOWN_DB;
        queue[qn].rg_album_db = MN_RG_UNKNOWN_DB;
        queue[qn].duration_ms = db_rows[i].duration_ms;
        if (db_rows[i].id == start_id) {
            start_index = qn;
            found = true;
            /* Cache now-playing strings from the matching row. */
            mn_app_cache_now(app, &db_rows[i]);
        }
        qn++;
    }

    if (!found) {
        /* The requested row is NOT in the current view's query (search hit
         * from another kind, filtered-out row). The old "play index 0
         * instead" behaviour swallowed the click — the user kept hearing
         * the current track ("switching does nothing" bug) or got a wrong
         * track on resume. Fail honestly; mn_app_play_row's fallback then
         * plays the track in its album context / as a single-track queue. */
        free(queue);
        return false;
    }

    if (!mn_playback_set_queue(app->pb, queue, qn)) {
        free(queue);
        return false;
    }

    {
        bool ok = mn_playback_play_index(app->pb, start_index);
        /* start_id path may differ from cache if !found; use the queue. */
        const char *play_path = queue[start_index].path;
        int64_t     play_id   = queue[start_index].id;
        free(queue);
        if (!ok) {
            return false;
        }
        (void)mn_library_bump_play(app->lib, play_id, (int64_t)time(NULL));
        mn_app_kick_stems(app, play_id, play_path);
        return true;
    }
}

/* ================================================================== */
/* Playback / transport                                              */
/* ================================================================== */

static void mn_app_sync_current(mn_app *app);   /* defined below */

void mn_app_play_row(mn_app *app, int64_t row_id)
{
    bool ok;
    int64_t alb = 0;
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    /* Playing a library row always ENDS an online session — otherwise the
     * now-snapshot override would keep masking the real queue. */
    app->online.active = false;
    ok = mn_app_play_from_query(app, row_id);
    if (!ok) {
        alb = mn_library_track_album_id(app->lib, row_id);
    }
    MN_UNLOCK(&app->lib_lock);
    if (ok) return;
    /* FALLBACK: the row is not in the current view's query (a search hit
     * from another kind, a filtered-out row). The old behaviour SILENTLY
     * kept the previous track playing — the "clicked a track but nothing
     * switched" bug. Play it in its album context, or as a single-track
     * queue when it has no album (untagged loose files). */
    if (alb > 0) {
        mn_app_play_album_track(app, alb, row_id);
        return;
    }
    {
        static char path[MN_STR_PATH];
        int64_t dur = 0;
        mn_track one;
        MN_LOCK(&app->lib_lock);
        ok = mn_library_track_path_duration(app->lib, row_id, path,
                                            sizeof(path), &dur);
        MN_UNLOCK(&app->lib_lock);
        if (!ok) return;
        memset(&one, 0, sizeof(one));
        one.path        = path;
        one.id          = row_id;
        one.rg_track_db = MN_RG_UNKNOWN_DB;
        one.rg_album_db = MN_RG_UNKNOWN_DB;
        one.duration_ms = dur;
        MN_LOCK(&app->lib_lock);
        if (mn_playback_set_queue(app->pb, &one, 1) &&
            mn_playback_play_index(app->pb, 0)) {
            mn_app_sync_current(app);
        }
        MN_UNLOCK(&app->lib_lock);
    }
}

/* Resume-on-launch: load `row_id` PAUSED at `position_ms`, atomically. This
 * exists because the UI's old play-then-toggle dance raced the state poll and
 * could leave the app blasting audio on startup. Nothing is audible here:
 * play_from_query starts the track, then the engine is paused and repositioned
 * before the lock is released and before a single tick advances. */
void mn_app_resume_row(mn_app *app, int64_t row_id, int64_t position_ms)
{
    if (!app || !app->pb || row_id <= 0) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (mn_app_play_from_query(app, row_id)) {
        mn_playback_set_paused(app->pb, true);
        if (position_ms > 0) {
            (void)mn_playback_seek_ms(app->pb, (uint64_t)position_ms);
        }
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    MN_UNLOCK(&app->lib_lock);
    /* Out-of-view resume (e.g. last track was an audiobook, boot restored
     * the music view): play through the id-direct fallback, then pause. */
    mn_app_play_row(app, row_id);
    MN_LOCK(&app->lib_lock);
    if (app->pb && app->now_track_id == row_id) {
        mn_playback_set_paused(app->pb, true);
        if (position_ms > 0) {
            (void)mn_playback_seek_ms(app->pb, (uint64_t)position_ms);
        }
    }
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_play_album(mn_app *app, int64_t album_id)
{
    mn_app_play_album_track(app, album_id, 0);
}

/* Queue the whole album (disc/track order) and start at `track_id` when it
 * is one of the album's tracks, else at the first track. This is the
 * "played from an album's track list" queue context. */
void mn_app_play_album_track(mn_app *app, int64_t album_id, int64_t track_id)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *db_rows = NULL;
    int32_t              got = 0;
    mn_track            *queue = NULL;
    int32_t              start = 0;
    int32_t              i;

    if (!app || !app->pb || !app->lib || album_id == MN_INVALID_ID) {
        return;
    }

    MN_LOCK(&app->lib_lock);
    app->online.active = false;   /* library play ends an online session */
    memset(&spec, 0, sizeof(spec));
    spec.cascade[0].dim = MN_FACET_ALBUM;
    spec.cascade[0].value_id = album_id;
    spec.cascade_len = 1;
    spec.sort[0].key = MNDB_SORT_TRACK;
    spec.sort[0].descending = false;
    spec.sort_len = 1;
    /* Hidden-only, NOT the kind-scoped stamp: an EXPLICIT album id must play
     * regardless of the active kind. With the category filter here, playing
     * a VGM/OST track while the music kind is active built a query the kind
     * scope emptied -> silent no-op (the exact cross-kind case the play_row
     * fallback exists to handle). The album cascade already scopes rows. */
    mn_spec_apply_hidden_only(app, &spec);

    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    if (mn_query_window(q, 0, MN_QUEUE_BUILD_MAX, &db_rows, &got) != MNDB_OK
        || !db_rows || got <= 0) {
        mn_query_close(q);
        MN_UNLOCK(&app->lib_lock);
        return;
    }

    queue = (mn_track *)calloc((size_t)got, sizeof(mn_track));
    if (!queue) {
        mn_query_close(q);
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    for (i = 0; i < got; ++i) {
        queue[i].path        = db_rows[i].path;
        queue[i].id          = db_rows[i].id;
        queue[i].rg_track_db = MN_RG_UNKNOWN_DB;
        queue[i].rg_album_db = MN_RG_UNKNOWN_DB;
        queue[i].duration_ms = db_rows[i].duration_ms;
        if (track_id > 0 && db_rows[i].id == track_id) {
            start = i;
        }
    }
    mn_app_cache_now(app, &db_rows[start]);

    if (mn_playback_set_queue(app->pb, queue, (size_t)got)
        && mn_playback_play_index(app->pb, (size_t)start)) {
        (void)mn_library_bump_play(app->lib, queue[start].id, (int64_t)time(NULL));
        mn_app_kick_stems(app, queue[start].id, queue[start].path);
    }

    free(queue);
    mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
}

/* Build a heap array of mn_track for a whole album (by id) or a single track
 * (by id). Caller frees *out_tracks. Returns the count (0 on miss/empty).
 * Must be called with the lib_lock held. */
static size_t mn_app_gather_tracks_locked(mn_app *app, int64_t track_id,
                                          int64_t album_id,
                                          mn_track **out_tracks)
{
    mn_filter_spec      spec;
    mn_query           *q = NULL;
    const mn_track_row *db_rows = NULL;
    int32_t             got = 0;
    mn_track           *arr = NULL;
    size_t              n = 0;

    *out_tracks = NULL;

    memset(&spec, 0, sizeof(spec));
    if (album_id > 0) {
        spec.cascade[0].dim = MN_FACET_ALBUM;
        spec.cascade[0].value_id = album_id;
        spec.cascade_len = 1;
        spec.sort[0].key = MNDB_SORT_TRACK;
        spec.sort[0].descending = false;
        spec.sort_len = 1;
    }
    mn_spec_apply_hidden(app, &spec);

    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q)
        return 0;
    if (mn_query_window(q, 0, MN_QUEUE_BUILD_MAX, &db_rows, &got) != MNDB_OK
        || !db_rows || got <= 0) {
        mn_query_close(q);
        return 0;
    }

    arr = (mn_track *)calloc((size_t)got, sizeof(mn_track));
    if (!arr) { mn_query_close(q); return 0; }

    for (int32_t i = 0; i < got; ++i) {
        if (album_id > 0 || db_rows[i].id == track_id) {
            arr[n].path        = db_rows[i].path;   /* deep-copied by playback */
            arr[n].id          = db_rows[i].id;
            arr[n].rg_track_db = MN_RG_UNKNOWN_DB;
            arr[n].rg_album_db = MN_RG_UNKNOWN_DB;
            arr[n].duration_ms = db_rows[i].duration_ms;
            n++;
            if (album_id <= 0) break;   /* single-track match found */
        }
    }
    mn_query_close(q);
    if (n == 0) { free(arr); return 0; }
    *out_tracks = arr;
    return n;
}

void mn_app_queue_last(mn_app *app, int64_t track_id, int64_t album_id)
{
    mn_track *arr = NULL;
    size_t    n;
    if (!app || !app->pb || !app->lib) return;
    MN_LOCK(&app->lib_lock);
    n = mn_app_gather_tracks_locked(app, track_id, album_id, &arr);
    if (n > 0) (void)mn_playback_append(app->pb, arr, n);
    free(arr);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_queue_next(mn_app *app, int64_t track_id, int64_t album_id)
{
    mn_track *arr = NULL;
    size_t    n;
    if (!app || !app->pb || !app->lib) return;
    MN_LOCK(&app->lib_lock);
    n = mn_app_gather_tracks_locked(app, track_id, album_id, &arr);
    if (n > 0) (void)mn_playback_insert_next(app->pb, arr, n);
    free(arr);
    MN_UNLOCK(&app->lib_lock);
}

bool mn_app_remove_track(mn_app *app, int64_t track_id)
{
    bool ok = false;
    if (!app || !app->lib || track_id <= 0) return false;
    MN_LOCK(&app->lib_lock);
    if (mn_library_delete_track(app->lib, track_id) == MNDB_OK) {
        ok = true;
        app->query_dirty = true; app->alb_cache_valid = false;   /* the visible list must repopulate */
    }
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

void mn_app_toggle_pause(mn_app *app)
{
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->online.active && app->engine) {
        /* Online session: pause/resume the engine directly — the parked
         * library queue must NOT restart from a transport toggle. */
        if (mn_engine_state(app->engine) == MN_STATE_PLAYING) {
            (void)mn_engine_pause(app->engine);
        } else {
            (void)mn_engine_play(app->engine);
        }
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    (void)mn_playback_toggle_pause(app->pb);
    MN_UNLOCK(&app->lib_lock);
}

/* True STOP: pause + rewind to 0 (the media-key Stop used to just toggle
 * pause, so pressing Stop while paused started playback). */
void mn_app_stop(mn_app *app)
{
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    mn_playback_set_paused(app->pb, true);
    mn_playback_seek_ms(app->pb, 0);
    MN_UNLOCK(&app->lib_lock);
}

/* After a next/prev/auto-advance, refresh the cached now-playing strings
 * and (re)kick stems for the newly active track. */
static void mn_app_sync_current(mn_app *app)
{
    mn_track_info info;

    if (!app || !app->pb) {
        return;
    }
    if (!mn_playback_current_track(app->pb, &info)) {
        return;
    }
    if (info.id == app->now_track_id) {
        return; /* unchanged */
    }

    /* Fetch metadata for the new track via a scratch query. */
    MN_LOCK(&app->lib_lock);
    if (app->query) {
        const mn_track_row *r = NULL;
        if (mn_query_fetch_id(app->query, info.id, &r) == MNDB_OK && r) {
            mn_app_cache_now(app, r);
        } else {
            app->now_track_id = info.id;
        }
    } else {
        app->now_track_id = info.id;
    }

    (void)mn_library_bump_play(app->lib, info.id, (int64_t)time(NULL));
    MN_UNLOCK(&app->lib_lock);
    mn_app_kick_stems(app, info.id, info.path);
}

void mn_app_next(mn_app *app)
{
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->online.active) {
        /* Station/episode next is the UI module's job (its list, its
         * order); advancing the parked library queue would hijack it. */
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    if (mn_playback_next(app->pb)) {
        mn_app_sync_current(app);
    }
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_prev(mn_app *app)
{
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->online.active) {
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    if (mn_playback_prev(app->pb)) {
        mn_app_sync_current(app);
    }
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_seek_ms(mn_app *app, int64_t position_ms)
{
    if (!app || !app->pb || position_ms < 0) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->online.active && app->engine) {
        (void)mn_engine_seek_ms(app->engine, (uint64_t)position_ms);
    } else {
        (void)mn_playback_seek_ms(app->pb, (uint64_t)position_ms);
    }
    MN_UNLOCK(&app->lib_lock);
}

/* ------------------------------------------------------------------ */
/* Online session (internet radio / podcasts)                          */
/* ------------------------------------------------------------------ */

bool mn_app_online_play(mn_app *app, const char *src, const char *title,
                        const char *artist, const char *kind,
                        const char *art_url, int64_t duration_ms, bool local,
                        char *err, size_t err_cap)
{
    mn_result r;
    bool want_icy;

    if (err && err_cap) err[0] = 0;
    if (!app || !app->engine || !src || !src[0]) {
        return false;
    }
    if (!kind || !kind[0]) kind = "stream";
    want_icy = (strcmp(kind, "radio") == 0 || strcmp(kind, "stream") == 0);

    /* Park the library queue: tick() then never touches the engine. The
     * queue itself is left intact so ending the online session and playing
     * a library row behaves exactly like any other new play. */
    MN_LOCK(&app->lib_lock);
    if (app->pb) {
        mn_playback_stop(app->pb);
    }
    MN_UNLOCK(&app->lib_lock);

    /* Connect + decode OUTSIDE the lock — this blocks for seconds and the
     * UI thread must stay free to poll `now` (which shows the old state
     * until we flip online.active below). */
    if (duration_ms > 0) {
        mn_engine_set_length_hint_ms(app->engine, duration_ms);
    }
    /* NOTE: engine MN_OK is masked here by the app.h/library_db.h alias
     * dance — engine success is 0, every error negative. */
    if (local) {
        r = mn_engine_load(app->engine, src);
        if (r != 0 && err && err_cap) {
            snprintf(err, err_cap, "couldn't open the episode file");
        }
    } else {
        r = mn_engine_load_url(app->engine, src, want_icy ? 1 : 0,
                               err, err_cap);
    }
    if (r != 0) {
        MN_LOCK(&app->lib_lock);
        app->online.active = false;
        MN_UNLOCK(&app->lib_lock);
        return false;
    }
    (void)mn_engine_play(app->engine);

    MN_LOCK(&app->lib_lock);
    app->online.active  = true;
    app->online.icy_seq = 0;
    app->online.icy[0]  = '\0';
    mn_copy_str(app->online.kind,   sizeof(app->online.kind),   kind);
    mn_copy_str(app->online.title,  sizeof(app->online.title),
                title && title[0] ? title : "Stream");
    mn_copy_str(app->online.artist, sizeof(app->online.artist),
                artist ? artist : "");
    mn_copy_str(app->online.url,    sizeof(app->online.url),    src);
    mn_copy_str(app->online.art,    sizeof(app->online.art),
                art_url ? art_url : "");
    /* If the server sent a station name and the caller had none, use it. */
    if ((!title || !title[0]) && app->engine) {
        const char *sn = mn_engine_stream_station(app->engine);
        if (sn && sn[0]) {
            mn_copy_str(app->online.title, sizeof(app->online.title), sn);
        }
    }
    MN_UNLOCK(&app->lib_lock);
    return true;
}

void mn_app_online_stop(mn_app *app)
{
    if (!app) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    if (app->online.active) {
        app->online.active = false;
        if (app->engine) {
            (void)mn_engine_stop(app->engine);
        }
    }
    MN_UNLOCK(&app->lib_lock);
}

bool mn_app_online_active(mn_app *app)
{
    bool a;
    if (!app) return false;
    MN_LOCK(&app->lib_lock);
    a = app->online.active;
    MN_UNLOCK(&app->lib_lock);
    return a;
}

void mn_app_set_volume(mn_app *app, float volume)
{
    if (!app || !app->pb) {
        return;
    }
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    MN_LOCK(&app->lib_lock);
    app->volume = volume;
    mn_playback_set_volume(app->pb, volume);
    MN_UNLOCK(&app->lib_lock);
}

/* ---- DSP / EQ passthroughs to the engine -------------------------------- */

void mn_app_set_dsp_enabled(mn_app *app, int enabled)
{
    if (!app || !app->engine) return;
    mn_engine_set_dsp_enabled(app->engine, enabled);
}

int mn_app_get_dsp_enabled(mn_app *app)
{
    if (!app || !app->engine) return 0;
    return mn_engine_get_dsp_enabled(app->engine);
}

void mn_app_set_eq_enabled(mn_app *app, int enabled)
{
    if (!app || !app->engine) return;
    mn_engine_set_eq_enabled(app->engine, enabled);
}

void mn_app_set_eq_band(mn_app *app, int band, float gain_db)
{
    if (!app || !app->engine || band < 0) return;
    mn_engine_set_eq_band(app->engine, (uint32_t)band, gain_db);
}

void mn_app_set_eq_preset(mn_app *app, int preset, float out_gains[10], float *out_preamp)
{
    if (!app || !app->engine) return;
    mn_engine_set_eq_preset(app->engine, preset, out_gains, out_preamp);
}

void mn_app_set_preamp(mn_app *app, float preamp_db)
{
    if (!app || !app->engine) return;
    mn_engine_set_preamp(app->engine, preamp_db);
}

void mn_app_set_balance(mn_app *app, float balance)
{
    if (!app || !app->engine) return;
    app->dsp_balance = balance;
    mn_engine_set_balance(app->engine, balance);
}

void mn_app_set_limiter(mn_app *app, int enabled, float threshold_db, float ceiling_db)
{
    if (!app || !app->engine) return;
    app->dsp_limiter_on        = enabled ? 1 : 0;
    app->dsp_limiter_thresh_db = threshold_db;
    app->dsp_limiter_ceil_db   = ceiling_db;
    mn_engine_set_limiter(app->engine, enabled, threshold_db, ceiling_db);
}

void mn_app_set_master(mn_app *app, float gain_db)
{
    if (!app || !app->engine) return;
    app->dsp_master_db = gain_db;
    mn_engine_set_master_gain(app->engine, gain_db);
}

/* Report the DSP shadow state for the EQ modal restore. */
void mn_app_get_dsp_extra(mn_app *app, float *balance, int *limiter_on,
                          float *lim_thresh, float *lim_ceil, float *master_db)
{
    if (!app) return;
    if (balance)    *balance    = app->dsp_balance;
    if (limiter_on) *limiter_on = app->dsp_limiter_on;
    if (lim_thresh) *lim_thresh = app->dsp_limiter_thresh_db;
    if (lim_ceil)   *lim_ceil   = app->dsp_limiter_ceil_db;
    if (master_db)  *master_db  = app->dsp_master_db;
}

void mn_app_get_eq(mn_app *app, float out_gains[10], float *out_preamp, int *out_enabled)
{
    if (!app || !app->engine) return;
    mn_engine_get_eq(app->engine, out_gains, out_preamp, out_enabled);
}

int mn_app_get_spectrum(mn_app *app, float *out, int max)
{
    if (!app || !app->engine || !out || max <= 0) return 0;
    return (int)mn_engine_get_spectrum(app->engine, out, (uint32_t)max);
}

void mn_app_set_shuffle(mn_app *app, bool enabled)
{
    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    mn_playback_set_shuffle(app->pb, enabled);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_cycle_repeat(mn_app *app)
{
    mn_repeat_mode cur;
    mn_repeat_mode next;

    if (!app || !app->pb) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    cur = mn_playback_get_repeat(app->pb);
    /* OFF -> ALL -> ONE -> OFF */
    switch (cur) {
        case MNP_REPEAT_OFF: next = MNP_REPEAT_ALL; break;
        case MNP_REPEAT_ALL: next = MNP_REPEAT_ONE; break;
        case MNP_REPEAT_ONE:
        default:             next = MNP_REPEAT_OFF; break;
    }
    mn_playback_set_repeat(app->pb, next);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_set_rating(mn_app *app, int64_t row_id, int32_t rating)
{
    if (!app || !app->lib || row_id == MN_INVALID_ID) {
        return;
    }
    if (rating < 0) rating = 0;
    if (rating > 5) rating = 5;
    /* app rating is 0..5 stars; db stores half-stars 0..10. */
    MN_LOCK(&app->lib_lock);
    (void)mn_library_set_rating(app->lib, row_id, rating * 2);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_set_liked(mn_app *app, int64_t row_id, int32_t v)
{
    if (!app || row_id <= 0) {
        return;
    }
    if (v < -1) v = -1;
    if (v >  1) v = 1;
    MN_LOCK(&app->lib_lock);
    (void)mn_library_set_liked(app->lib, row_id, v);
    if (row_id == app->now_track_id) {
        app->now_liked = v;   /* keep the now-playing snapshot in sync */
    }
    MN_UNLOCK(&app->lib_lock);
}

/* ================================================================== */
/* Audiobook progress (book_progress table)                            */
/* ================================================================== */

/* Note the current playback position of (book, chapter). Percent +
 * content_hash snapshotting happen inside the DB layer. */
void mn_app_book_note(mn_app *app, int64_t album_id, int64_t track_id,
                      int64_t pos_ms, int64_t updated)
{
    if (!app || album_id <= 0 || track_id <= 0) return;
    MN_LOCK(&app->lib_lock);
    (void)mn_library_book_note(app->lib, album_id, track_id, pos_ms, updated);
    MN_UNLOCK(&app->lib_lock);
}

/* Where was I in this book? (current chapter + position + completion). */
bool mn_app_book_get(mn_app *app, int64_t album_id, int64_t *out_track,
                     int64_t *out_pos, double *out_percent,
                     bool *out_finished)
{
    bool got;
    if (!app || album_id <= 0) return false;
    MN_LOCK(&app->lib_lock);
    got = mn_library_book_get(app->lib, album_id, out_track, out_pos,
                              out_percent, out_finished);
    MN_UNLOCK(&app->lib_lock);
    return got;
}

/* The Continue-Listening shelf feed. Translates the DB layer's rows into
 * the app.h POD contract (mn_book). Returns entries filled. */
int mn_app_recent_books(mn_app *app, mn_book *out, int max)
{
    enum { RB_CAP = 24 };
    static mn_book_recent rows[RB_CAP];   /* serialized under lib_lock */
    int n, i;
    if (!app || !out || max <= 0) return 0;
    if (max > RB_CAP) max = RB_CAP;
    MN_LOCK(&app->lib_lock);
    n = mn_library_recent_books(app->lib, rows, max);
    for (i = 0; i < n; i++) {
        mn_book *b = &out[i];
        memset(b, 0, sizeof(*b));
        b->album_id    = rows[i].album_id;
        b->track_id    = rows[i].track_id;
        b->pos_ms      = rows[i].pos_ms;
        b->duration_ms = rows[i].duration_ms;
        b->percent     = rows[i].percent;
        b->finished    = rows[i].finished;
        b->updated     = rows[i].updated;
        mn_copy_str(b->album,        sizeof(b->album),        rows[i].album);
        mn_copy_str(b->album_artist, sizeof(b->album_artist), rows[i].album_artist);
        mn_copy_str(b->artist,       sizeof(b->artist),       rows[i].artist);
        mn_copy_str(b->title,        sizeof(b->title),        rows[i].title);
    }
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* Bookmarks (named positions within a book). */
int64_t mn_app_bookmark_add(mn_app *app, int64_t album_id, int64_t track_id,
                            int64_t pos_ms, const char *note, int64_t created)
{
    int64_t id;
    if (!app) return 0;
    MN_LOCK(&app->lib_lock);
    id = mn_library_bookmark_add(app->lib, album_id, track_id, pos_ms,
                                 note, created);
    MN_UNLOCK(&app->lib_lock);
    return id;
}

void mn_app_bookmark_del(mn_app *app, int64_t bookmark_id)
{
    if (!app) return;
    MN_LOCK(&app->lib_lock);
    (void)mn_library_bookmark_del(app->lib, bookmark_id);
    MN_UNLOCK(&app->lib_lock);
}

int mn_app_bookmark_list(mn_app *app, int64_t album_id, int64_t *ids,
                         int64_t *track_ids, int64_t *pos_ms,
                         char (*notes)[128], int64_t *created, int max)
{
    int n;
    if (!app) return 0;
    MN_LOCK(&app->lib_lock);
    n = mn_library_bookmark_list(app->lib, album_id, ids, track_ids, pos_ms,
                                 notes, created, max);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* Pitch-preserved playback speed (audiobooks). 0.5..3.0; 1.0 = bypass. */
void mn_app_set_speed(mn_app *app, float speed)
{
    if (!app || !app->engine) return;
    (void)mn_engine_set_speed(app->engine, speed);
}

float mn_app_get_speed(mn_app *app)
{
    if (!app || !app->engine) return 1.0f;
    return mn_engine_get_speed(app->engine);
}

/* content_hash backfill plumbing (the worker lives in the host layer). */
int mn_app_hashless_rows(mn_app *app, int64_t *ids, char (*paths)[1024],
                         int64_t *sizes, int max)
{
    int n;
    if (!app) return 0;
    MN_LOCK(&app->lib_lock);
    n = mn_library_hashless_rows(app->lib, ids, paths, sizes, max);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

void mn_app_set_content_hash(mn_app *app, int64_t track_id, const char *hash,
                             bool force)
{
    if (!app || track_id <= 0 || !hash || !hash[0]) return;
    MN_LOCK(&app->lib_lock);
    (void)mn_library_set_content_hash(app->lib, track_id, hash, force);
    MN_UNLOCK(&app->lib_lock);
}

/* All remembered chapter positions within one book. */
int mn_app_book_chapters(mn_app *app, int64_t album_id, int64_t *track_ids,
                         int64_t *pos_ms, int max)
{
    int n;
    if (!app || album_id <= 0 || !track_ids || !pos_ms || max <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    n = mn_library_book_chapters(app->lib, album_id, track_ids, pos_ms, max);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* Grid-tile badge feed: parallel arrays of every touched book's completion. */
int mn_app_book_badges(mn_app *app, int64_t *album_ids, double *percents,
                       bool *finisheds, int max)
{
    int n;
    if (!app || !album_ids || !percents || !finisheds || max <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    n = mn_library_book_badges(app->lib, album_ids, percents, finisheds, max);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

int32_t mn_app_get_liked(mn_app *app, int64_t row_id)
{
    int32_t v = 0;
    if (!app || row_id <= 0) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    (void)mn_library_get_liked(app->lib, row_id, &v);
    MN_UNLOCK(&app->lib_lock);
    return v;
}

/* ================================================================== */
/* Neural stem separation                                            */
/* ================================================================== */

void mn_app_stems_enable(mn_app *app, bool enabled)
{
    if (!app) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    app->stems_enabled = enabled;
    if (!app->stems) {
        /* Lazy model load: the ~136 MB session isn't mapped at boot. The first
         * time stems are turned ON, spawn the loader now (once — guarded by
         * stems_loading). When it finishes it publishes app->stems and, seeing
         * stems_enabled==true, auto-kicks separation for the active track (see
         * mn_app_stems_loader_body). Spawning under the lock is safe: the
         * loader body takes the lock on a separate thread and simply waits for
         * this unlock. Turning stems OFF while unloaded is a no-op. */
        if (enabled && !app->stems_loading) {
            mn_app_spawn_stems_loader(app);
        }
        MN_UNLOCK(&app->lib_lock);
        return;
    }
    if (enabled) {
        /* Kick separation for whatever is currently playing. */
        mn_track_info info;
        if (mn_playback_current_track(app->pb, &info)) {
            (void)mn_stems_start(app->stems, info.id, info.path);
            mn_stems_set_passthrough(app->stems, app->stems_passthrough);
        }
    } else {
        mn_stems_cancel(app->stems);
    }
    /* Publish the new routing to the engine's audio callback. */
    mn_engine_set_stem_source(app->engine, app->stems, enabled,
                              app->stems_passthrough);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_stems_passthrough(mn_app *app, bool enabled)
{
    if (!app) {
        return;
    }
    MN_LOCK(&app->lib_lock);
    app->stems_passthrough = enabled;
    if (app->stems) {
        mn_stems_set_passthrough(app->stems, enabled);
    }
    /* Republish routing so the engine callback honors the passthrough flip. */
    mn_engine_set_stem_source(app->engine, app->stems, app->stems_enabled,
                              enabled);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_stem_gain(mn_app *app, int32_t stem, float gain)
{
    if (!app || !app->stems) {
        return;
    }
    if (stem < 0 || stem >= MN_STEM_COUNT) {
        return;
    }
    /* mn_stems_set_gain is atomic-publish; no app lock needed. */
    mn_stems_set_gain(app->stems, (int)stem, gain);
}

void mn_app_stem_mute(mn_app *app, int32_t stem, bool muted)
{
    if (!app || !app->stems) {
        return;
    }
    if (stem < 0 || stem >= MN_STEM_COUNT) {
        return;
    }
    mn_stems_set_mute(app->stems, (int)stem, muted);
}

void mn_app_stem_solo(mn_app *app, int32_t stem, bool soloed)
{
    if (!app || !app->stems) {
        return;
    }
    if (stem < 0 || stem >= MN_STEM_COUNT) {
        return;
    }
    /* Per-channel independent solo: soloing/un-soloing one stem never touches
     * the others, so you can solo several stems at once and un-solo one without
     * disabling the whole solo set. */
    mn_stems_set_solo_state(app->stems, (int)stem, soloed);
}

/* ================================================================== */
/* Per-frame polling                                                 */
/* ================================================================== */

/* Shared implementation. want_art=false skips the on-disk art-path resolution
 * (mn_app_art_path -> mn_art_ensure does a real fopen/stat every call). Callers
 * that never read out->art_path — e.g. the 10 Hz taskbar progress tick — pass
 * false to drop that syscall from the idle hot path. */
static void mn_app_now_impl(mn_app *app, mn_now *out, bool want_art)
{
    mn_audio_format fmt;
    mn_stems_progress prog;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!app) {
        return;
    }

    MN_LOCK(&app->lib_lock);

    /* Transport. */
    if (app->engine) {
        out->position_ms = (int64_t)mn_engine_position_ms(app->engine);
        out->duration_ms = (int64_t)mn_engine_duration_ms(app->engine);
    }
    if (app->pb) {
        out->playing = (mn_playback_state_get(app->pb) == MN_PLAYBACK_PLAYING)
                     && !mn_playback_is_paused(app->pb);
        out->shuffle = mn_playback_get_shuffle(app->pb);
        out->repeat  = mn_unmap_repeat(mn_playback_get_repeat(app->pb));
    }

    /* Cached track metadata. */
    out->track_id = app->now_track_id;
    out->album_id = app->now_album_id;
    out->liked    = app->now_liked;
    mn_copy_str(out->track_path,   sizeof(out->track_path),   app->now_path);
    mn_copy_str(out->track_title,  sizeof(out->track_title),  app->now_title);
    mn_copy_str(out->track_artist, sizeof(out->track_artist), app->now_artist);
    mn_copy_str(out->track_album,  sizeof(out->track_album),  app->now_album);
    mn_copy_str(out->track_album_artist, sizeof(out->track_album_artist),
                app->now_album_artist);

    /* Album art for the current track — keyed by ALBUM artist (the key the
     * cache was populated with at scan time), not the track artist. Skipped
     * for lite callers (want_art=false) so the fopen stays out of their hot
     * path; out->art_path is left empty (memset above). */
    if (want_art) {
        const char *ap = mn_app_art_path(app, app->now_album_artist,
                                         app->now_album);
        if (ap) {
            mn_copy_str(out->art_path, sizeof(out->art_path), ap);
        }
    }

    /* Stream format (source side + REAL device-side output). SOURCE facts
     * prefer the tag-scanned library values: the engine's decoder reports its
     * decode/pipeline format (f32 -> "32 bit", resampled rate) rather than
     * the file's true depth/rate/bitrate. Engine values remain the fallback
     * for files that carried no tags. */
    if (app->engine && mn_engine_get_format(app->engine, &fmt) == MNE_OK) {
        mn_copy_str(out->format, sizeof(out->format), fmt.format);
        out->sample_rate     = (int32_t)fmt.src_sample_rate;
        out->bit_depth       = (int32_t)fmt.src_bits;
        out->channels        = (int32_t)fmt.src_channels;
        out->bitrate_kbps    = (int32_t)(fmt.bitrate / 1000u);
        out->out_sample_rate = (int32_t)fmt.out_sample_rate;
        out->out_bit_depth   = (int32_t)fmt.out_bits;
        out->out_channels    = (int32_t)fmt.out_channels;
        out->out_exclusive   = fmt.out_exclusive;
        out->pipe_channels   = (int32_t)fmt.pipe_channels;
        out->downmixed       = fmt.downmixed;
        out->rate_limited    = fmt.rate_limited;
        mn_copy_str(out->out_pcm, sizeof(out->out_pcm), fmt.out_pcm);
    }
    if (app->now_track_id > 0) {
        if (app->now_src_rate > 0)      out->sample_rate  = app->now_src_rate;
        if (app->now_src_bits > 0)      out->bit_depth    = app->now_src_bits;
        else                            out->bit_depth    = 0;  /* lossy: no fixed depth */
        if (app->now_src_kbps > 0)      out->bitrate_kbps = app->now_src_kbps;
        if (app->now_src_format[0])
            mn_copy_str(out->format, sizeof(out->format), app->now_src_format);
    }

    /* Mixer. */
    out->volume = app->volume;

    /* Neural stems. */
    out->stems_available   = (app->stems != NULL);
    out->stems_loading     = (!app->stems && app->stems_loading);
    out->stems_enabled     = app->stems_enabled;
    out->stems_passthrough = app->stems_passthrough;
    if (!app->stems && app->stems_loading) {
        /* Async model load in flight: surface it in the provider slot so
         * the stem dock can show progress instead of a silent "idle". */
        mn_copy_str(out->stem_provider, sizeof(out->stem_provider),
                    "loading model…");
    }
    if (app->stems) {
        out->neural_active = mn_stems_neural_active(app->stems);

        memset(&prog, 0, sizeof(prog));
        mn_stems_get_progress(app->stems, &prog);
        out->stem_rt_factor = prog.rt_factor;
        out->stem_fraction  = prog.fraction;
        switch (prog.provider) {
            case MN_STEMS_PROVIDER_CACHE: mn_copy_str(out->stem_provider, sizeof(out->stem_provider), "CACHE"); break;
            case MN_STEMS_PROVIDER_CUDA:  mn_copy_str(out->stem_provider, sizeof(out->stem_provider), "CUDA");  break;
            case MN_STEMS_PROVIDER_CPU:   mn_copy_str(out->stem_provider, sizeof(out->stem_provider), "CPU");   break;
            case MN_STEMS_PROVIDER_NONE:
            default:                      mn_copy_str(out->stem_provider, sizeof(out->stem_provider), "");      break;
        }

        /* Meters: stems exposes MN_STEMS_CHANNEL_COUNT (9) == MN_STEM_COUNT. */
        {
            float meters[MN_STEMS_CHANNEL_COUNT];
            int   k;
            int   n = (MN_STEM_COUNT < MN_STEMS_CHANNEL_COUNT)
                    ? MN_STEM_COUNT : MN_STEMS_CHANNEL_COUNT;
            memset(meters, 0, sizeof(meters));
            mn_stems_get_meters(app->stems, meters);
            for (k = 0; k < n; ++k) {
                out->stem_meters[k] = meters[k];
            }
        }
    }

    /* ---- Online session override -------------------------------------
     * The playback controller is stopped during an online session, so the
     * standard fill above reported the OLD queue track + playing=false.
     * Overwrite with the live stream's reality. */
    if (app->online.active && app->engine) {
        char icy[MN_STR_SHORT];
        out->online = true;
        mn_copy_str(out->online_kind, sizeof(out->online_kind),
                    app->online.kind);
        mn_copy_str(out->online_url, sizeof(out->online_url),
                    app->online.url);
        mn_copy_str(out->online_art, sizeof(out->online_art),
                    app->online.art);
        out->online_live = mn_engine_is_stream(app->engine) &&
                           !mn_engine_stream_seekable(app->engine);
        out->playing  = (mn_engine_state(app->engine) == MN_STATE_PLAYING);
        out->track_id = 0;
        out->album_id = 0;
        out->liked    = 0;
        out->track_path[0] = '\0';
        out->art_path[0]   = '\0';
        /* Latest ICY song title (radio): diffed by sequence so we only
         * copy on change; cached for polls in between. */
        if (mn_engine_stream_title(app->engine, icy, sizeof(icy),
                                   &app->online.icy_seq)) {
            mn_copy_str(app->online.icy, sizeof(app->online.icy), icy);
        }
        mn_copy_str(out->stream_title, sizeof(out->stream_title),
                    app->online.icy);
        if (strcmp(app->online.kind, "radio") == 0 && app->online.icy[0]) {
            /* Song as the title, station as the artist line. */
            mn_copy_str(out->track_title,  sizeof(out->track_title),
                        app->online.icy);
            mn_copy_str(out->track_artist, sizeof(out->track_artist),
                        app->online.title);
        } else {
            mn_copy_str(out->track_title,  sizeof(out->track_title),
                        app->online.title);
            mn_copy_str(out->track_artist, sizeof(out->track_artist),
                        app->online.artist);
        }
        mn_copy_str(out->track_album, sizeof(out->track_album),
                    strcmp(app->online.kind, "podcast") == 0 ? "Podcasts" :
                    strcmp(app->online.kind, "stream")  == 0 ? "Streams"
                                                             : "Radio");
        out->track_album_artist[0] = '\0';
        /* Library-tag overrides above don't apply to a stream. */
        out->stems_available = false;
        out->stems_enabled   = false;
    }
    MN_UNLOCK(&app->lib_lock);
}

/* Full snapshot including the resolved on-disk art path. */
void mn_app_now(mn_app *app, mn_now *out)
{
    mn_app_now_impl(app, out, true);
}

/* Lite snapshot: everything EXCEPT out->art_path (left empty). Skips the
 * per-call fopen/stat in mn_app_art_path. For the 10 Hz taskbar tick and any
 * caller that only needs transport/format state, not the art thumbnail. */
void mn_app_now_lite(mn_app *app, mn_now *out)
{
    mn_app_now_impl(app, out, false);
}

bool mn_app_current_path(mn_app *app, char *out, size_t n)
{
    mn_track_info info;
    bool          ok;

    if (!out || n == 0) {
        return false;
    }
    out[0] = '\0';
    if (!app || !app->pb) {
        return false;
    }
    MN_LOCK(&app->lib_lock);
    ok = mn_playback_current_track(app->pb, &info) && info.path[0] != '\0';
    if (ok) {
        mn_copy_str(out, n, info.path);
    }
    MN_UNLOCK(&app->lib_lock);
    return ok && out[0] != '\0';
}

void mn_app_tick(mn_app *app)
{
    if (!app) {
        return;
    }

    MN_LOCK(&app->lib_lock);

    /* Drive gapless/crossfaded auto-advance; sync now-playing on change. */
    if (app->pb) {
        if (mn_playback_tick(app->pb)) {
            mn_app_sync_current(app);
        } else {
            /* Even without a transition, the engine may have hit EOS with
             * REPEAT_OFF at the tail; keep cached state coherent. */
            mn_app_sync_current(app);
        }
    }

    /* Sleep timer: pause playback once the deadline passes. */
    if (app->sleep_deadline_ms != 0 && app->pb) {
        uint64_t nowt = (uint64_t)GetTickCount64();
        if (nowt >= app->sleep_deadline_ms) {
            app->sleep_deadline_ms = 0;
            mn_playback_set_paused(app->pb, true);
        }
    }

    /* If a scan crossed a commit boundary, the on_track callback marked
     * the query dirty; the next row_count/window rebuilds it. Refresh the
     * count opportunistically so the UI sees live growth. */
    if (app->scanning && app->query_dirty) {
        (void)mn_refresh_query(app);
    }

    MN_UNLOCK(&app->lib_lock);
}

/* Sleep timer: pause playback after `minutes` (0 cancels). */
void mn_app_set_sleep_timer(mn_app *app, int minutes)
{
    if (!app) return;
    MN_LOCK(&app->lib_lock);
    if (minutes <= 0) {
        app->sleep_deadline_ms = 0;
    } else {
        app->sleep_deadline_ms =
            (uint64_t)GetTickCount64() + (uint64_t)minutes * 60000ull;
    }
    MN_UNLOCK(&app->lib_lock);
}

/* Remaining sleep-timer seconds (0 = off). */
int mn_app_get_sleep_remaining(mn_app *app)
{
    int rem = 0;
    if (!app) return 0;
    MN_LOCK(&app->lib_lock);
    if (app->sleep_deadline_ms != 0) {
        uint64_t nowt = (uint64_t)GetTickCount64();
        rem = (app->sleep_deadline_ms > nowt)
            ? (int)((app->sleep_deadline_ms - nowt) / 1000ull) : 0;
    }
    MN_UNLOCK(&app->lib_lock);
    return rem;
}

/* ================================================================== */
/* Album art                                                         */
/* ================================================================== */

const char *mn_app_art_path(mn_app *app, const char *artist, const char *album)
{
    char        key[MN_STR_SHORT * 2];
    const char *ret = NULL;

    if (!app) {
        return NULL;
    }
    if (!album || album[0] == '\0') {
        return NULL;
    }

    /* Album key must match the one used at scan time (mn_app_on_track):
     * "<album_artist-or-artist>\x1f<album>". */
    snprintf(key, sizeof(key), "%s\x1f%s", artist ? artist : "", album);

    /* Check-only lookup (audio_path == NULL): returns the cached path if a
     * thumbnail already exists on disk. */
    MN_LOCK(&app->lib_lock);
    if (mn_art_ensure(app->art_cache_dir, key, NULL,
                      app->art_path_scratch, sizeof(app->art_path_scratch))) {
        ret = app->art_path_scratch;
    }
    MN_UNLOCK(&app->lib_lock);
    return ret;
}

bool mn_app_hires_cover(mn_app *app, const char *artist, const char *album,
                        bool ensure, char *out, size_t n)
{
    char key[MN_STR_SHORT * 2];
    char track_path[MN_STR_PATH];
    bool have_path = false;

    if (!app || !out || n == 0 || !album || album[0] == '\0')
        return false;

    snprintf(key, sizeof(key), "%s\x1f%s", artist ? artist : "", album);
    track_path[0] = '\0';

    MN_LOCK(&app->lib_lock);

    /* Check-only fast path: if a hi-res cover is already cached, no DB work. */
    if (mn_art_ensure_hires(app->art_cache_dir, key, NULL, out, n)) {
        MN_UNLOCK(&app->lib_lock);
        return true;
    }

    if (ensure) {
        /* Resolve one representative track path for this album via an FTS
         * search on the album title, then match on album (+ artist when
         * available) to avoid a same-title collision across artists. */
        mn_filter_spec spec;
        mn_query      *q = NULL;
        memset(&spec, 0, sizeof(spec));
        spec.fts_match = album;
        spec.sort[0].key = MNDB_SORT_TRACK;
        spec.sort[0].descending = false;
        spec.sort_len = 1;
        /* HIDDEN-ONLY: hires publish runs from kind-agnostic maintenance
         * paths; the category stamp found zero tracks for any album outside
         * the active kind (hires never appeared for those surfaces). */
        mn_spec_apply_hidden_only(app, &spec);

        if (mn_query_open(app->lib, &spec, &q) == MNDB_OK && q) {
            const mn_track_row *tr = NULL;
            int32_t tn = 0;
            if (mn_query_window(q, 0, 64, &tr, &tn) == MNDB_OK && tr) {
                for (int32_t i = 0; i < tn && !have_path; ++i) {
                    if (!tr[i].album || !tr[i].path || !tr[i].path[0])
                        continue;
                    if (mn_strcasecmp(tr[i].album, album) != 0)
                        continue;
                    if (artist && artist[0]) {
                        const char *aa = (tr[i].album_artist &&
                                          tr[i].album_artist[0])
                                       ? tr[i].album_artist : tr[i].artist;
                        if (!aa || mn_strcasecmp(aa, artist) != 0)
                            continue;
                    }
                    mn_copy_str(track_path, sizeof(track_path), tr[i].path);
                    have_path = true;
                }
                /* Fall back to the first result if strict matching found none
                 * (e.g. album-artist blank while artist filled the key). */
                if (!have_path && tn > 0 && tr[0].path && tr[0].path[0]) {
                    mn_copy_str(track_path, sizeof(track_path), tr[0].path);
                    have_path = true;
                }
            }
            mn_query_close(q);
        }
    }
    MN_UNLOCK(&app->lib_lock);

    if (!ensure || !have_path)
        return false;

    /* Heavy extract/decode/scale/encode runs WITHOUT the lock (matches the
     * refresh_art discipline). */
    return mn_art_ensure_hires(app->art_cache_dir, key, track_path, out, n);
}

/* Kind-agnostic album-identity window for the art-integrity verifier: walks
 * the ALBUM facet with kind/search/hidden scoping stripped (every album of
 * every kind, label order) and derives each album's (artist, title) exactly
 * like the grid does — first track of the album in album/disc/track order,
 * album_artist-or-artist. Only id/title/artist/track_count are filled.
 * Returns rows written. Runs under the app lock (DB work only). */
int32_t mn_app_album_ident_all(mn_app *app, int64_t offset, int32_t count,
                               mn_album *out)
{
    mn_filter_spec       spec;
    mn_facet            *f = NULL;
    const mn_facet_row  *frows = NULL;
    int32_t              got = 0, w = 0;

    if (!app || !app->lib || !out || count <= 0 || offset < 0) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);
    mn_build_spec(app, &spec);
    spec.sort_len = 0;
    /* library-wide: strip kind scoping + search + liked/rating filters (same
     * de-scoping as mn_app_refresh_art) so audiobooks/ost/vgm are verified
     * even while the music view is active */
    spec.kind_roots_len = 0;
    spec.kind_include = false;
    spec.excluded_folders_len = 0;
    spec.fts_match = NULL;
    spec.liked_only = false;
    spec.min_rating_x2 = 0;

    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) != MNDB_OK || !f) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    if (mn_facet_window(f, offset, count, &frows, &got) == MNDB_OK &&
        frows && got > 0) {
        int32_t i;
        if (got > count) got = count;
        for (i = 0; i < got; ++i) {
            mn_album           *a = &out[w];
            mn_filter_spec      tspec;
            mn_query           *tq = NULL;
            const mn_track_row *tr = NULL;
            int32_t             tn = 0;

            memset(a, 0, sizeof(*a));
            a->id          = frows[i].value_id;
            a->track_count = (int32_t)frows[i].count;
            mn_copy_str(a->title, sizeof(a->title),
                        frows[i].label ? frows[i].label : "");

            memset(&tspec, 0, sizeof(tspec));
            tspec.cascade[0].dim = MN_FACET_ALBUM;
            tspec.cascade[0].value_id = frows[i].value_id;
            tspec.cascade_len = 1;
            /* GRID-order first row: the album cache streams tracks sorted by
             * MNDB_SORT_ALBUM (with the hidden-folder filter) and takes the
             * first row it sees per album — mirror both so the derived aa is
             * byte-identical to the grid's aa. HIDDEN-ONLY: the category
             * stamp would empty this query for every non-active-kind album. */
            tspec.sort[0].key = MNDB_SORT_ALBUM;
            tspec.sort[0].descending = false;
            tspec.sort_len = 1;
            mn_spec_apply_hidden_only(app, &tspec);

            if (mn_query_open(app->lib, &tspec, &tq) == MNDB_OK && tq) {
                if (mn_query_window(tq, 0, 1, &tr, &tn) == MNDB_OK &&
                    tr && tn > 0) {
                    const char *aa = (tr[0].album_artist && tr[0].album_artist[0])
                                   ? tr[0].album_artist : tr[0].artist;
                    mn_copy_str(a->artist, sizeof(a->artist), aa ? aa : "");
                }
                mn_query_close(tq);
            }
            w++;
        }
    }
    mn_facet_close(f);
    MN_UNLOCK(&app->lib_lock);
    return w;
}

/* Total albums library-wide (all kinds) — pairs with mn_app_album_ident_all. */
int64_t mn_app_album_count_all(mn_app *app)
{
    mn_filter_spec spec;
    mn_facet      *f = NULL;
    int64_t        total = 0;

    if (!app || !app->lib) return 0;
    MN_LOCK(&app->lib_lock);
    mn_build_spec(app, &spec);
    spec.sort_len = 0;
    spec.kind_roots_len = 0;
    spec.kind_include = false;
    spec.excluded_folders_len = 0;
    spec.fts_match = NULL;
    spec.liked_only = false;
    spec.min_rating_x2 = 0;
    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) == MNDB_OK && f) {
        (void)mn_facet_count(f, &total);
        mn_facet_close(f);
    }
    MN_UNLOCK(&app->lib_lock);
    return total;
}

/* Distinct NON-MUSIC kinds currently registered (audiobook, podcast, custom
 * designations), case-insensitively deduped from the kind-roots registry.
 * Returns the number written. The music library is kind "" and is NOT
 * emitted (callers iterate "" + this list). */
int32_t mn_app_kind_list(mn_app *app, char out[][32], int32_t max)
{
    int32_t i, j, n = 0;
    if (!app || !out || max <= 0) return 0;
    MN_LOCK(&app->lib_lock);
    for (i = 0; i < app->kroot_len && n < max; i++) {
        bool dup = false;
        if (!app->kroot_kind[i][0]) continue;
        for (j = 0; j < n; j++) {
            if (_stricmp(out[j], app->kroot_kind[i]) == 0) { dup = true; break; }
        }
        if (dup) continue;
        mn_copy_str(out[n], 32, app->kroot_kind[i]);
        n++;
    }
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* Album count under an EXPLICIT kind's scoping ("" = music), independent of
 * the live active_kind. Pairs with mn_app_album_ident_kind. */
int64_t mn_app_album_count_kind(mn_app *app, const char *kind)
{
    mn_filter_spec spec;
    mn_facet      *f = NULL;
    int64_t        total = 0;

    if (!app || !app->lib) return 0;
    MN_LOCK(&app->lib_lock);
    memset(&spec, 0, sizeof(spec));
    mn_spec_apply_hidden_only(app, &spec);
    mn_spec_apply_category_kind(app, &spec, kind);
    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) == MNDB_OK && f) {
        (void)mn_facet_count(f, &total);
        mn_facet_close(f);
    }
    MN_UNLOCK(&app->lib_lock);
    return total;
}

/* KIND-SCOPED album-identity window: derives each album's (artist, title)
 * exactly like the ALBUM GRID does when `kind` is the active library —
 * album facet + first track in album/disc/track order, BOTH under that
 * kind's category scoping. This is what closes the audit hole the
 * kind-agnostic mn_app_album_ident_all left open: a kind-filtered album's
 * FIRST TRACK (hence its album_artist-or-artist, hence its art key) can
 * differ from the unscoped derivation, so a kind view's first open used to
 * request key variants no verifier had ever checked (measured: opening the
 * Audiobooks view minted 182 brand-new thumb keys minutes after three full
 * sweeps + --arttest all reported missing=0). Does NOT touch the live
 * active_kind. Only id/title/artist/track_count are filled. */
int32_t mn_app_album_ident_kind(mn_app *app, const char *kind,
                                int64_t offset, int32_t count, mn_album *out)
{
    mn_filter_spec       spec;
    mn_facet            *f = NULL;
    const mn_facet_row  *frows = NULL;
    int32_t              got = 0, w = 0;

    if (!app || !app->lib || !out || count <= 0 || offset < 0) {
        return 0;
    }

    MN_LOCK(&app->lib_lock);
    memset(&spec, 0, sizeof(spec));
    mn_spec_apply_hidden_only(app, &spec);
    mn_spec_apply_category_kind(app, &spec, kind);

    if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) != MNDB_OK || !f) {
        MN_UNLOCK(&app->lib_lock);
        return 0;
    }
    (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
    if (mn_facet_window(f, offset, count, &frows, &got) == MNDB_OK &&
        frows && got > 0) {
        int32_t i;
        if (got > count) got = count;
        for (i = 0; i < got; ++i) {
            mn_album           *a = &out[w];
            mn_filter_spec      tspec;
            mn_query           *tq = NULL;
            const mn_track_row *tr = NULL;
            int32_t             tn = 0;

            memset(a, 0, sizeof(*a));
            a->id          = frows[i].value_id;
            a->track_count = (int32_t)frows[i].count;
            mn_copy_str(a->title, sizeof(a->title),
                        frows[i].label ? frows[i].label : "");

            memset(&tspec, 0, sizeof(tspec));
            tspec.cascade[0].dim = MN_FACET_ALBUM;
            tspec.cascade[0].value_id = frows[i].value_id;
            tspec.cascade_len = 1;
            /* Mirror the kind-scoped album cache byte-for-byte: same sort,
             * same hidden filter, same category stamp — so the derived aa is
             * exactly what THIS kind's grid emits for the album. */
            tspec.sort[0].key = MNDB_SORT_ALBUM;
            tspec.sort[0].descending = false;
            tspec.sort_len = 1;
            mn_spec_apply_hidden_only(app, &tspec);
            mn_spec_apply_category_kind(app, &tspec, kind);

            if (mn_query_open(app->lib, &tspec, &tq) == MNDB_OK && tq) {
                if (mn_query_window(tq, 0, 1, &tr, &tn) == MNDB_OK &&
                    tr && tn > 0) {
                    const char *aa = (tr[0].album_artist && tr[0].album_artist[0])
                                   ? tr[0].album_artist : tr[0].artist;
                    mn_copy_str(a->artist, sizeof(a->artist), aa ? aa : "");
                }
                mn_query_close(tq);
            }
            w++;
        }
    }
    mn_facet_close(f);
    MN_UNLOCK(&app->lib_lock);
    return w;
}

/* Thread-safe check-only lookup into a caller-owned buffer (no shared scratch,
 * no app lock — mn_art_ensure with audio_path==NULL is a pure stat). */
bool mn_app_art_check(mn_app *app, const char *artist, const char *album,
                      char *out, size_t n)
{
    char key[MN_STR_SHORT * 2];

    if (!app || !out || n == 0 || !album || album[0] == '\0') {
        return false;
    }
    snprintf(key, sizeof(key), "%s\x1f%s", artist ? artist : "", album);
    return mn_art_ensure(app->art_cache_dir, key, NULL, out, n);
}

/* Targeted single-album extraction (see app.h). Resolves representative track
 * paths with the same FTS + exact-match logic as mn_app_hires_cover, then
 * ensures the grid thumb under the caller's exact "<artist>\x1f<album>" key —
 * so whatever aa variant a surface derives becomes servable for that surface. */
bool mn_app_art_extract_one(mn_app *app, const char *artist, const char *album,
                            bool *newly, char *out, size_t n, bool *src_seen)
{
    char key[MN_STR_SHORT * 2];
    char paths[8][MN_STR_PATH];
    int  npaths = 0;

    if (newly) *newly = false;
    if (src_seen) *src_seen = false;
    if (!app || !out || n == 0 || !album || album[0] == '\0') {
        return false;
    }
    snprintf(key, sizeof(key), "%s\x1f%s", artist ? artist : "", album);

    /* Fast path: already cached (another worker / the scanner won the race). */
    if (mn_art_ensure(app->art_cache_dir, key, NULL, out, n)) {
        return true;
    }

    /* Resolve up to 8 candidate track paths for this album under the lock. */
    MN_LOCK(&app->lib_lock);
    {
        mn_filter_spec spec;
        mn_query      *q = NULL;
        memset(&spec, 0, sizeof(spec));
        spec.fts_match = album;
        spec.sort[0].key = MNDB_SORT_TRACK;
        spec.sort[0].descending = false;
        spec.sort_len = 1;
        /* HIDDEN-ONLY, no category stamp: extraction must resolve tracks for
         * every kind (an audiobook/ost album healing while the music view is
         * active found ZERO tracks under the kind-scoped spec and was falsely
         * recorded as artless). */
        mn_spec_apply_hidden_only(app, &spec);

        if (mn_query_open(app->lib, &spec, &q) == MNDB_OK && q) {
            const mn_track_row *tr = NULL;
            int32_t tn = 0;
            if (mn_query_window(q, 0, 64, &tr, &tn) == MNDB_OK && tr) {
                int32_t i;
                for (i = 0; i < tn && npaths < 8; ++i) {
                    if (!tr[i].album || !tr[i].path || !tr[i].path[0])
                        continue;
                    if (mn_strcasecmp(tr[i].album, album) != 0)
                        continue;
                    if (artist && artist[0]) {
                        const char *aa = (tr[i].album_artist &&
                                          tr[i].album_artist[0])
                                       ? tr[i].album_artist : tr[i].artist;
                        if (!aa || mn_strcasecmp(aa, artist) != 0)
                            continue;
                    }
                    mn_copy_str(paths[npaths], sizeof(paths[npaths]),
                                tr[i].path);
                    npaths++;
                }
                /* Strict matching found none (album-artist / artist skew):
                 * fall back to the first FTS hit so a lone mistagged track
                 * still yields a cover for this key. */
                if (npaths == 0 && tn > 0 && tr[0].path && tr[0].path[0]) {
                    mn_copy_str(paths[0], sizeof(paths[0]), tr[0].path);
                    npaths = 1;
                }
            }
            mn_query_close(q);
        }
    }
    MN_UNLOCK(&app->lib_lock);

    /* Heavy extraction runs lock-free; first track with usable art wins.
     * (mn_art_ensure itself falls through ranked sidecar candidates, so a
     * failure here means NO candidate of NO track decoded.) */
    {
        int i;
        for (i = 0; i < npaths; ++i) {
            if (mn_art_ensure(app->art_cache_dir, key, paths[i], out, n)) {
                if (src_seen) *src_seen = true;
                if (newly) *newly = true;
                return true;
            }
        }
        /* Extraction failed everywhere. Distinguish "a source exists but is
         * undecodable" (must NOT be recorded as a terminal NONE verdict) from
         * "genuinely artless" — probe is cheap relative to the failed decode
         * work above and only runs on this failure path. */
        if (src_seen) {
            for (i = 0; i < npaths && !*src_seen; ++i)
                if (mn_art_probe(paths[i]))
                    *src_seen = true;
        }
    }
    return false;
}

/* One album's identity + a bounded list of track (id,path) pairs, snapshotted
 * under the lock so the heavy art extraction runs lock-free. */
typedef struct {
    int64_t value_id;
    char    artist[MN_STR_SHORT];
    char    album[MN_STR_SHORT];
    int64_t ids[8];
    char    paths[8][MN_STR_PATH];
    int     n;
} mn_art_album_snap;

/* Parallel art-extraction pool context: workers steal album indices from a
 * shared atomic counter and each ensures one album's thumbnail (the slow FS +
 * decode + encode work), mirrors it via the callback, and updates has_art.
 * `gained`/`done` are accumulated atomically. */
typedef struct {
    mn_app             *app;
    mn_art_album_snap  *batch;
    int                 batch_n;
    bool                skip_existing;
    mn_app_art_cb       cb;
    void               *user;
    int64_t             total_albums;
    int64_t             processed_base;   /* processed count before this batch */
    volatile LONG       next;             /* work-stealing index                */
    volatile LONG       gained;           /* newly-created thumbs this batch     */
    volatile LONG       done;             /* albums finished this batch          */
} mn_art_par_ctx;

/* Process ONE album (index bi) of the batch — the parallelizable unit. */
static void mn_art_process_one(mn_art_par_ctx *pcx, int bi)
{
    mn_app            *app = pcx->app;
    mn_art_album_snap *s   = &pcx->batch[bi];
    char key[MN_STR_SHORT * 2];
    char out[MN_ART_PATH_MAX];
    bool have = false, newly = false;

    if (!s->album[0]) return;
    snprintf(key, sizeof(key), "%s\x1f%s", s->artist, s->album);

    if (pcx->skip_existing &&
        mn_art_ensure(app->art_cache_dir, key, NULL, out, sizeof(out))) {
        have = true;
    } else {
        bool pre = mn_art_ensure(app->art_cache_dir, key, NULL, out, sizeof(out));
        for (int ti = 0; ti < s->n; ++ti) {
            if (mn_art_ensure(app->art_cache_dir, key, s->paths[ti],
                              out, sizeof(out))) {
                have = true; newly = !pre; break;
            }
        }
    }

    if (have) {
        MN_LOCK(&app->lib_lock);
        for (int ti = 0; ti < s->n; ++ti)
            (void)mn_library_set_has_art(app->lib, s->ids[ti], true);
        MN_UNLOCK(&app->lib_lock);
    }
    if (newly) InterlockedIncrement(&pcx->gained);

    /* progress/result callback (thread-safe; invoked concurrently). `newly`
     * lets the host emit targeted artready repaints only for thumbs THIS
     * pass created (pre-existing ones must not spam the bridge). When no
     * thumb could be produced, `src_seen` tells the host whether a potential
     * source existed but failed to decode — those must NOT get a persisted
     * NONE verdict (only genuinely artless albums do). */
    if (pcx->cb) {
        bool src_seen = have;
        LONG p = InterlockedIncrement(&pcx->done);
        if (!have) {
            for (int ti = 0; ti < s->n && !src_seen; ++ti)
                if (mn_art_probe(s->paths[ti]))
                    src_seen = true;
        }
        pcx->cb(pcx->user, s->artist, s->album, have ? out : "", newly,
                src_seen, pcx->processed_base + p, pcx->total_albums);
    } else {
        InterlockedIncrement(&pcx->done);
    }
}

/* Worker: steal album indices until the batch is exhausted (or shutdown). */
static DWORD WINAPI mn_art_par_worker(LPVOID param)
{
    mn_art_par_ctx *pcx = (mn_art_par_ctx *)param;
    for (;;) {
        LONG j;
        if (pcx->app->shutting_down) break;
        j = InterlockedIncrement(&pcx->next) - 1;
        if (j >= pcx->batch_n) break;
        mn_art_process_one(pcx, (int)j);
    }
    return 0;
}

int64_t mn_app_refresh_art(mn_app *app, bool skip_existing, int64_t limit,
                           mn_app_art_cb cb, void *user)
{
    mn_filter_spec       spec;
    mn_facet            *f = NULL;
    const mn_facet_row  *frows = NULL;
    int32_t              got = 0;
    int64_t              total_albums = 0;
    int64_t              processed = 0;
    int64_t              gained = 0;
    int64_t              offset = 0;
    enum { PAGE = 64 };

    if (!app || !app->lib) return 0;

    total_albums = mn_app_album_count(app);
    if (limit > 0 && limit < total_albums) total_albums = limit;
    if (total_albums <= 0) return 0;

    /* HEAP, not stack: the batch is ~550 KB (64 snaps x 8 paths x 1 KB) —
     * on a default 1 MB worker stack that left almost no headroom above
     * the decode frames below it. */
    mn_art_album_snap *batch =
        (mn_art_album_snap *)malloc(sizeof(mn_art_album_snap) * PAGE);
    if (!batch) return 0;

    for (;;) {
        int batch_n = 0;

        /* the app is tearing down — abandon the sweep promptly so destroy()
         * never frees the library out from under this thread */
        if (app->shutting_down) break;
        if (limit > 0 && processed >= limit) break;

        /* --- Snapshot one page of albums + their first few tracks under the
         * lock (DB access), then release before touching the filesystem. --- */
        MN_LOCK(&app->lib_lock);
        mn_build_spec(app, &spec);
        spec.sort_len = 0;
        /* Art must be built for the WHOLE library, not just the active kind.
         * mn_build_spec applies the active_kind category filter, so without
         * this a refresh while viewing "music" never builds audiobook / ost /
         * vgm art (they stay blank forever). Strip the kind scoping + search +
         * hidden-folder + liked filters so the sweep covers every album. */
        spec.kind_roots_len = 0;
        spec.kind_include = false;
        spec.excluded_folders_len = 0;
        spec.fts_match = NULL;
        spec.liked_only = false;
        spec.min_rating_x2 = 0;
        if (mn_facet_open(app->lib, MN_FACET_ALBUM, &spec, &f) != MNDB_OK || !f) {
            MN_UNLOCK(&app->lib_lock);
            break;
        }
        (void)mn_facet_sort(f, MN_FACET_ORDER_LABEL);
        if (mn_facet_window(f, offset, PAGE, &frows, &got) != MNDB_OK ||
            !frows || got <= 0) {
            mn_facet_close(f);
            MN_UNLOCK(&app->lib_lock);
            break;
        }

        for (int32_t i = 0; i < got && batch_n < PAGE; ++i) {
            mn_art_album_snap *s = &batch[batch_n];
            mn_filter_spec      tspec;
            mn_query           *tq = NULL;
            const mn_track_row *tr = NULL;
            int32_t             tn = 0;

            memset(s, 0, sizeof(*s));
            s->value_id = frows[i].value_id;
            mn_copy_str(s->album, sizeof(s->album), frows[i].label);

            memset(&tspec, 0, sizeof(tspec));
            tspec.cascade[0].dim = MN_FACET_ALBUM;
            tspec.cascade[0].value_id = frows[i].value_id;
            tspec.cascade_len = 1;
            tspec.sort[0].key = MNDB_SORT_TRACK;
            tspec.sort[0].descending = false;
            tspec.sort_len = 1;
            /* NO kind/hidden filter here: the album value_id already pins this
             * exact album, and the sweep is library-wide. Applying the active-
             * kind category would drop every non-active-kind album's tracks →
             * no art extracted for audiobooks/ost/vgm while viewing music. */

            if (mn_query_open(app->lib, &tspec, &tq) == MNDB_OK && tq) {
                if (mn_query_window(tq, 0, 8, &tr, &tn) == MNDB_OK && tr && tn > 0) {
                    const char *aa = (tr[0].album_artist && tr[0].album_artist[0])
                                   ? tr[0].album_artist : tr[0].artist;
                    mn_copy_str(s->artist, sizeof(s->artist), aa ? aa : "");
                    for (int32_t ti = 0; ti < tn && s->n < 8; ++ti) {
                        if (tr[ti].path && tr[ti].path[0]) {
                            s->ids[s->n] = tr[ti].id;
                            mn_copy_str(s->paths[s->n], sizeof(s->paths[s->n]),
                                        tr[ti].path);
                            s->n++;
                        }
                    }
                }
                mn_query_close(tq);
            }
            batch_n++;
        }
        mn_facet_close(f);
        f = NULL;
        MN_UNLOCK(&app->lib_lock);

        if (batch_n == 0) break;

        /* --- Lock-free heavy work: ensure a thumbnail per album. PARALLEL:
         * mn_art_ensure does per-album decode/extract/resize/encode (the slow
         * part) on independent files, so it fans out across a worker pool
         * (work-stealing via an atomic index; main thread joins in). The DB
         * has_art write + the progress callback happen INSIDE the per-album
         * worker but are each self-synchronizing (lib_lock / the callback's own
         * webart temp-file names are per-thread). `gained`/`processed` are
         * bumped atomically so counts stay correct across threads. */
        {
            mn_art_par_ctx pcx;
            pcx.app = app; pcx.batch = batch; pcx.batch_n = batch_n;
            pcx.skip_existing = skip_existing;
            pcx.cb = cb; pcx.user = user;
            pcx.total_albums = total_albums;
            pcx.processed_base = processed;
            pcx.next = 0; pcx.gained = 0; pcx.done = 0;
#ifdef _WIN32
            {
                SYSTEM_INFO si;
                HANDLE th[15];
                int nt = 0, t;
                GetSystemInfo(&si);
                nt = (int)si.dwNumberOfProcessors - 1;
                if (nt > 15) nt = 15;
                if (nt > batch_n - 1) nt = batch_n - 1;
                if (batch_n < 4) nt = 0;
                if (nt < 0) nt = 0;
                for (t = 0; t < nt; ++t) {
                    th[t] = CreateThread(NULL, 0, mn_art_par_worker, &pcx, 0, NULL);
                    if (!th[t]) { nt = t; break; }
                }
                mn_art_par_worker(&pcx);          /* main thread joins the pool */
                if (nt > 0) {
                    WaitForMultipleObjects((DWORD)nt, th, TRUE, INFINITE);
                    for (t = 0; t < nt; ++t) CloseHandle(th[t]);
                }
            }
#else
            mn_art_par_worker(&pcx);
#endif
            gained    += pcx.gained;
            processed += pcx.done;
        }
        if (limit > 0 && processed >= limit) break;

        offset += got;
        if (got < PAGE) break;   /* last page */
        if (app->shutting_down) break;
    }

    fprintf(stderr, "[art-refresh] swept %lld albums, %lld new thumbs\n",
            (long long)processed, (long long)gained);
    free(batch);
    return gained;
}

/* ================================================================== */
/* Settings                                                          */
/* ================================================================== */

/* --------------------------------------------------------------------------
 * Settings persistence. mn_settings lived only in RAM with hardcoded defaults,
 * so exclusive mode / crossfade / ReplayGain / preamp / art size / cache caps
 * silently reset on every launch. Persisted as a small "key=value" text file
 * (<data_dir>\settings.txt) — same flat-file pattern as folder_kinds/sync.
 * Called under the app lock. Best-effort (a missing/corrupt file just leaves
 * the defaults). EQ/DSP state persists separately (see mn_app_save_dsp).
 * -------------------------------------------------------------------------- */
static void mn_app_settings_path(mn_app *app, char *out, size_t n)
{
    snprintf(out, n, "%s%csettings.txt", app->data_dir,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
}

static void mn_app_save_settings_locked(mn_app *app)
{
    char  path[MN_STR_PATH + 16], tmp[MN_STR_PATH + 24];
    FILE *f;
    mn_app_settings_path(app, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "exclusive=%d\n",      app->settings.exclusive ? 1 : 0);
    fprintf(f, "hifi_native_bits=%d\n", app->settings.hifi_native_bits ? 1 : 0);
    fprintf(f, "ab_rate_cap_hz=%d\n", app->settings.ab_rate_cap_hz);
    fprintf(f, "ab_bits_cap=%d\n",    app->settings.ab_bits_cap);
    fprintf(f, "crossfade_ms=%d\n",   app->settings.crossfade_ms);
    fprintf(f, "replaygain=%d\n",     app->settings.replaygain ? 1 : 0);
    fprintf(f, "replaygain_mode=%d\n",app->settings.replaygain_mode);
    fprintf(f, "rg_preamp_db=%.3f\n", (double)app->settings.rg_preamp_db);
    fprintf(f, "album_art_size=%d\n", app->settings.album_art_size);
    fprintf(f, "stem_cache_gb=%d\n",  app->settings.stem_cache_gb);
    fprintf(f, "art_cache_mb=%d\n",   app->settings.art_cache_mb);
    fprintf(f, "depth_batch=%d\n",    app->settings.depth_batch ? 1 : 0);
    fprintf(f, "infer_tags=%d\n",     app->settings.infer_tags ? 1 : 0);
    fprintf(f, "watch_folders=%d\n",  app->settings.watch_folders ? 1 : 0);
    fprintf(f, "low_power=%d\n",      app->settings.low_power ? 1 : 0);
    fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp, path);
#endif
}

/* Load persisted settings over the current defaults. Public: mn_app_create
 * calls it once after the defaults are set, before the engine is wired. */
void mn_app_load_settings(mn_app *app)
{
    char  path[MN_STR_PATH + 16], line[256];
    FILE *f;
    if (!app) return;
    mn_app_settings_path(app, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return;
    MN_LOCK(&app->lib_lock);
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *k, *v;
        if (!eq) continue;
        *eq = 0; k = line; v = eq + 1;
        if      (!strcmp(k, "exclusive"))       app->settings.exclusive = atoi(v) != 0;
        else if (!strcmp(k, "hifi_native_bits")) app->settings.hifi_native_bits = atoi(v) != 0;
        else if (!strcmp(k, "ab_rate_cap_hz"))  app->settings.ab_rate_cap_hz = atoi(v);
        else if (!strcmp(k, "ab_bits_cap"))     app->settings.ab_bits_cap = atoi(v);
        else if (!strcmp(k, "crossfade_ms"))    app->settings.crossfade_ms = atoi(v);
        else if (!strcmp(k, "replaygain"))      app->settings.replaygain = atoi(v) != 0;
        else if (!strcmp(k, "replaygain_mode")) app->settings.replaygain_mode = atoi(v);
        else if (!strcmp(k, "rg_preamp_db"))    app->settings.rg_preamp_db = (float)atof(v);
        else if (!strcmp(k, "album_art_size"))  app->settings.album_art_size = atoi(v);
        else if (!strcmp(k, "stem_cache_gb"))   app->settings.stem_cache_gb = atoi(v);
        else if (!strcmp(k, "art_cache_mb"))    app->settings.art_cache_mb = atoi(v);
        else if (!strcmp(k, "depth_batch"))     app->settings.depth_batch = atoi(v) != 0;
        else if (!strcmp(k, "infer_tags"))      app->settings.infer_tags = atoi(v) != 0;
        else if (!strcmp(k, "watch_folders"))   app->settings.watch_folders = atoi(v) != 0;
        else if (!strcmp(k, "low_power"))       app->settings.low_power = atoi(v) != 0;
    }
    MN_UNLOCK(&app->lib_lock);
    fclose(f);
}

void mn_app_get_settings(mn_app *app, mn_settings *out)
{
    if (!out) {
        return;
    }
    if (!app) {
        memset(out, 0, sizeof(*out));
        return;
    }
    MN_LOCK(&app->lib_lock);
    *out = app->settings;
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_set_settings(mn_app *app, const mn_settings *settings)
{
    if (!app || !settings) {
        return;
    }

    MN_LOCK(&app->lib_lock);
    app->settings = *settings;
    if (app->settings.crossfade_ms < 0) {
        app->settings.crossfade_ms = 0;
    }
    if (app->settings.album_art_size <= 0) {
        app->settings.album_art_size = MN_ART_THUMB_SIZE;
    }
    if (app->settings.album_art_size < MN_ART_THUMB_MIN) {
        app->settings.album_art_size = MN_ART_THUMB_MIN;
    }
    if (app->settings.album_art_size > MN_ART_THUMB_MAX) {
        app->settings.album_art_size = MN_ART_THUMB_MAX;
    }

    /* Wire the art size into the artcache layer: NEW thumbnails are rendered
     * at this size, and the size participates in the cache filename for
     * non-default values, so a changed size regenerates on demand. */
    mn_art_set_thumb_size(app->settings.album_art_size);

    /* Apply to the playback controller / engine immediately. */
    if (app->pb) {
        mn_replaygain_mode rgm = MN_REPLAYGAIN_OFF;
        /* Prefer the explicit mode; the legacy bool maps to TRACK. */
        if (app->settings.replaygain_mode == 2)      rgm = MN_REPLAYGAIN_ALBUM;
        else if (app->settings.replaygain_mode == 1 ||
                 app->settings.replaygain)           rgm = MN_REPLAYGAIN_TRACK;
        mn_playback_set_crossfade_ms(app->pb, (uint32_t)app->settings.crossfade_ms);
        mn_playback_set_replaygain(app->pb, rgm,
                                   (double)app->settings.rg_preamp_db);
    }
    /* Exclusive/bit-perfect WASAPI output: publish to the engine, which
     * restarts the device in the new share mode when the flag changes. */
    if (app->engine) {
        mn_engine_set_exclusive(app->engine, app->settings.exclusive ? 1 : 0);
        mn_app_apply_hifi_profile_locked(app);
    }
    /* Stems disk-cache cap: the setter had ZERO callers, so the Storage
     * "Stems cache limit" slider was a no-op (cache stayed at the compile
     * default). Wire it here (GB -> bytes; 0/neg keeps the built-in cap). */
    if (app->stems && app->settings.stem_cache_gb > 0) {
        mn_stems_set_cache_cap_bytes(
            (int64_t)app->settings.stem_cache_gb * 1024 * 1024 * 1024);
    }
    /* Persist so audio settings survive restarts (they used to reset). */
    mn_app_save_settings_locked(app);
    MN_UNLOCK(&app->lib_lock);
}

/* ================================================================== */
/* Audio hardware capabilities                                       */
/* ================================================================== */

bool mn_app_audio_caps(mn_app *app, struct mn_audio_caps *out)
{
    bool ok;
    if (!app || !out || !app->engine) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    MN_LOCK(&app->lib_lock);
    ok = mn_engine_get_caps(app->engine, out);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_list_devices(mn_app *app, struct mn_audio_device *out, int max)
{
    int n;
    if (!app || !out || max <= 0 || !app->engine) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    n = mn_engine_list_devices(app->engine, out, max);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

bool mn_app_select_device(mn_app *app, int index)
{
    bool ok;
    if (!app || !app->engine || index < 0) {
        return false;
    }
    /* Device reinit must be serialized against every other engine mutation
     * (load/seek/stems retune) — the app lock is that serialization. */
    MN_LOCK(&app->lib_lock);
    ok = mn_engine_select_device(app->engine, index);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

int mn_app_selected_device(mn_app *app)
{
    int idx;
    if (!app || !app->engine) {
        return -1;
    }
    MN_LOCK(&app->lib_lock);
    idx = mn_engine_selected_device(app->engine);
    MN_UNLOCK(&app->lib_lock);
    return idx;
}

bool mn_app_play_queue_index(mn_app *app, int index)
{
    bool ok = false;
    if (!app || !app->pb || index < 0) return false;
    MN_LOCK(&app->lib_lock);
    if ((size_t)index < mn_playback_count(app->pb)) {
        ok = mn_playback_play_index(app->pb, (size_t)index);
        if (ok) mn_app_sync_current(app);      /* refresh now-playing + stems */
    }
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

/* Queue mutation wrappers (reorder / remove / clear from the Up-Next UI). */
void mn_app_queue_move(mn_app *app, int from, int to)
{
    if (!app || !app->pb || from < 0 || to < 0) return;
    MN_LOCK(&app->lib_lock);
    (void)mn_playback_move(app->pb, (size_t)from, (size_t)to);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_queue_remove(mn_app *app, int index)
{
    if (!app || !app->pb || index < 0) return;
    MN_LOCK(&app->lib_lock);
    (void)mn_playback_remove(app->pb, (size_t)index);
    MN_UNLOCK(&app->lib_lock);
}

void mn_app_queue_clear(mn_app *app)
{
    if (!app || !app->pb) return;
    MN_LOCK(&app->lib_lock);
    mn_playback_clear(app->pb);
    MN_UNLOCK(&app->lib_lock);
}

int mn_app_queue(mn_app *app, mn_queue_item *out, int max, int *out_current)
{
    int n = 0;
    size_t total, cur, i;
    mn_track_info info;
    mn_filter_spec spec;
    mn_query *q = NULL;   /* dedicated UNFILTERED track query: resolves any id
                             (the app-view query may be an album facet). */

    if (out_current) *out_current = -1;
    if (!app || !app->pb || !app->lib || !out || max <= 0) {
        return 0;
    }
    MN_LOCK(&app->lib_lock);
    total = mn_playback_count(app->pb);
    cur   = mn_playback_position_index(app->pb);
    /* Playlist behavior: list the WHOLE queue (played + upcoming); the UI
     * highlights the current index and jumping does not drop tracks. */
    if (out_current) *out_current = (cur == (size_t)-1) ? -1 : (int)cur;

    memset(&spec, 0, sizeof(spec));            /* no facet cascade = all tracks */
    (void)mn_query_open(app->lib, &spec, &q);

    for (i = 0; i < total && n < max; ++i) {
        if (!mn_playback_track_at(app->pb, i, &info)) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = info.id;
        if (q) {
            const mn_track_row *r = NULL;
            if (mn_query_fetch_id(q, info.id, &r) == MNDB_OK && r) {
                mn_copy_str(out[n].title,  sizeof(out[n].title),  r->title);
                mn_copy_str(out[n].artist, sizeof(out[n].artist), r->artist);
                mn_copy_str(out[n].album,  sizeof(out[n].album),  r->album);
                mn_copy_str(out[n].album_artist, sizeof(out[n].album_artist), (r->album_artist && r->album_artist[0]) ? r->album_artist : r->artist);
                mn_copy_str(out[n].format, sizeof(out[n].format), r->format);
                out[n].duration_ms  = (int32_t)r->duration_ms;
                out[n].bitrate_kbps = r->bitrate_kbps;
                out[n].sample_rate  = r->sample_rate;
                out[n].bit_depth    = r->bit_depth;
                out[n].play_count   = (int64_t)r->play_count;
                out[n].liked        = r->liked;
            }
        }
        n++;
    }
    if (q) mn_query_close(q);
    MN_UNLOCK(&app->lib_lock);
    return n;
}

/* ================================================================== */
/* Metadata writing (tags / cover art / lyrics)                       */
/* ================================================================== */

/* Longest lived error token from the tag writer. */
#define MN_APP_ERR(e, n, msg)                       \
    do { if ((e) && (n)) snprintf((e), (n), "%s", (msg)); } while (0)

/* Upper bound of files touched by a whole-album art write. */
#define MN_ALBUM_ART_MAX_TRACKS 256

/* Case-insensitive path equality on Windows (case-preserving FS). */
static bool mn_path_eq(const char *a, const char *b)
{
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Delete a file by UTF-8 path (wide conversion on Windows). */
static void mn_app_delete_file(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[MN_STR_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) > 0) {
        DeleteFileW(wpath);
    }
#else
    remove(path);
#endif
}

/*
 * Owned snapshot of one track's db row. mn_track_row strings live in the
 * scratch query's arena (dead once the query closes), so every consumer
 * of a fetched row outside the query's lifetime goes through this copy.
 */
typedef struct {
    char path[MN_STR_PATH];
    char title[MN_STR_SHORT * 2];
    char artist[MN_STR_SHORT * 2];
    char album[MN_STR_SHORT * 2];
    char album_artist[MN_STR_SHORT * 2];
    char genre[MN_STR_SHORT];
    char composer[MN_STR_SHORT * 2];
    char format[MN_STR_SHORT];
    int32_t year, track, disc;
    int32_t sample_rate, channels, bit_depth, bitrate_kbps;
    int64_t duration_ms, size, mtime;
    bool    has_art;
} mn_trackmeta;

/* Fetch a track row by id into an owned snapshot. Caller holds the lock. */
static bool mn_app_fetch_meta_locked(mn_app *app, int64_t id, mn_trackmeta *m)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *r = NULL;
    bool                 ok = false;

    memset(m, 0, sizeof(*m));
    memset(&spec, 0, sizeof(spec));
    spec.include_missing = true;   /* editing a missing-flagged file is fine */

    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        return false;
    }
    if (mn_query_fetch_id(q, id, &r) == MNDB_OK && r) {
        mn_copy_str(m->path,         sizeof(m->path),         r->path);
        mn_copy_str(m->title,        sizeof(m->title),        r->title);
        mn_copy_str(m->artist,       sizeof(m->artist),       r->artist);
        mn_copy_str(m->album,        sizeof(m->album),        r->album);
        mn_copy_str(m->album_artist, sizeof(m->album_artist), r->album_artist);
        mn_copy_str(m->genre,        sizeof(m->genre),        r->genre);
        mn_copy_str(m->composer,     sizeof(m->composer),     r->composer);
        mn_copy_str(m->format,       sizeof(m->format),       r->format);
        m->year         = r->year;
        m->track        = r->track;
        m->disc         = r->disc;
        m->sample_rate  = r->sample_rate;
        m->channels     = r->channels;
        m->bit_depth    = r->bit_depth;
        m->bitrate_kbps = r->bitrate_kbps;
        m->duration_ms  = r->duration_ms;
        m->size         = r->size;
        m->mtime        = r->mtime;
        m->has_art      = r->has_art;
        ok = m->path[0] != '\0';
    }
    mn_query_close(q);
    return ok;
}

/* Resolve a track's ALBUM-ART key — the (album_artist-or-artist, album) pair
 * used to hash its cover — so a caller can invalidate exactly that album's
 * cached art (not the whole library). False if the id is unknown or has no
 * album. Thread-safe (takes the lib lock). */
bool mn_app_track_art_key(mn_app *app, int64_t id,
                          char *artist_out, size_t artist_n,
                          char *album_out, size_t album_n)
{
    mn_trackmeta m;
    bool ok;
    if (!app || !artist_out || !album_out) return false;
    MN_LOCK(&app->lib_lock);
    ok = mn_app_fetch_meta_locked(app, id, &m);
    MN_UNLOCK(&app->lib_lock);
    if (!ok || m.album[0] == '\0') return false;
    /* album_artist preferred (matches the scan-time art key); fall back to the
     * track artist when the album-artist tag is absent. */
    mn_copy_str(artist_out, artist_n,
                m.album_artist[0] ? m.album_artist : m.artist);
    mn_copy_str(album_out, album_n, m.album);
    return true;
}

/* Resolve a track's file path by id (Media Manager recycle/delete needs the
 * on-disk location before dropping the row). Thread-safe. */
bool mn_app_track_path(mn_app *app, int64_t id, char *out, size_t out_n)
{
    mn_trackmeta m;
    bool ok;
    if (!out || out_n == 0) return false;
    out[0] = '\0';
    if (!app || !app->lib || id <= 0) return false;
    MN_LOCK(&app->lib_lock);
    ok = mn_app_fetch_meta_locked(app, id, &m);
    MN_UNLOCK(&app->lib_lock);
    if (!ok) return false;
    mn_copy_str(out, out_n, m.path);
    return out[0] != '\0';
}

/*
 * CRITICAL — locked files. The engine's ma_decoder keeps the loaded file
 * OPEN (even when merely stopped), which denies the delete access the
 * atomic replace (MoveFileEx MOVEFILE_REPLACE_EXISTING) needs. Before a
 * rewrite we must fully unload the engine when it holds the target path,
 * remembering the transport state so playback resumes afterwards.
 */
typedef struct {
    bool     restore;    /* playback was ACTIVE on the target file      */
    bool     paused;
    uint64_t pos_ms;
    size_t   index;      /* queue index to re-play                      */
} mn_pb_resume;

/* Release the file at `path` if the engine currently holds it open.
 * Caller holds the app lock. */
static void mn_app_release_if_loaded(mn_app *app, const char *path,
                                     mn_pb_resume *rz)
{
    char          loaded[MN_STR_PATH];
    mn_track_info info;

    if (!app->engine ||
        !mn_engine_loaded_path(app->engine, loaded, sizeof(loaded)) ||
        !mn_path_eq(loaded, path)) {
        return;
    }

    if (app->pb &&
        mn_playback_state_get(app->pb) == MN_PLAYBACK_PLAYING &&
        mn_playback_current_track(app->pb, &info) &&
        mn_path_eq(info.path, path)) {
        rz->restore = true;
        rz->paused  = mn_playback_is_paused(app->pb);
        rz->pos_ms  = mn_engine_position_ms(app->engine);
        rz->index   = info.index;
        mn_playback_stop(app->pb);
    }
    (void)mn_engine_unload(app->engine);   /* closes the decoder's handle */
}

/* Reload + seek back + restore play/pause state after a rewrite. Skipped
 * when the user started other playback meanwhile. Caller holds the lock. */
static void mn_app_reattach(mn_app *app, const mn_pb_resume *rz)
{
    if (!rz->restore || !app->pb) {
        return;
    }
    if (mn_playback_state_get(app->pb) == MN_PLAYBACK_PLAYING) {
        return;   /* something else is already playing — leave it alone */
    }
    if (mn_playback_play_index(app->pb, rz->index)) {
        if (rz->paused) {
            mn_playback_set_paused(app->pb, true);
        }
        (void)mn_playback_seek_ms(app->pb, rz->pos_ms);
    }
}

/* PUBLIC wrapper (Recycle-Bin delete path, cef_host.c): release the decoder
 * handle when the engine holds `path`, WITHOUT keeping resume state — the
 * caller is about to delete the file, so playback of it simply ends. */
void mn_app_release_path(mn_app *app, const char *path)
{
    mn_pb_resume rz;
    if (!app || !path || !path[0]) {
        return;
    }
    memset(&rz, 0, sizeof(rz));
    MN_LOCK(&app->lib_lock);
    mn_app_release_if_loaded(app, path, &rz);
    MN_UNLOCK(&app->lib_lock);
}

/* Bust + regenerate the album's cached thumbnail after an art write. The
 * cache key must mirror mn_app_on_track: "<album_artist-or-artist>\x1f
 * <album>". Deletes every cached variant the cache would serve (sized +
 * legacy default-size fallback), then regenerates from the file's freshly
 * embedded art. Caller holds the app lock. */
static void mn_app_refresh_art_cache(mn_app *app, const mn_trackmeta *m)
{
    char key[MN_STR_SHORT * 2];
    char out[MN_ART_PATH_MAX];
    const char *aa = m->album_artist[0] ? m->album_artist
                   : (m->artist[0]      ? m->artist : "");
    int  i;

    if (!m->album[0]) {
        return;
    }
    snprintf(key, sizeof(key), "%s\x1f%s", aa, m->album);
    for (i = 0; i < 4; i++) {   /* bounded: sized + legacy fallback */
        if (!mn_art_ensure(app->art_cache_dir, key, NULL, out, sizeof(out))) {
            break;
        }
        mn_app_delete_file(out);
    }
    (void)mn_art_ensure(app->art_cache_dir, key, m->path, out, sizeof(out));
}

/* Resolve the ALBUM facet value id for an exact album label (windowed
 * scan; the facet enumerates distinct labels, typically a few 10k rows at
 * most). Caller holds the lock. Returns 0 when not found. */
static int64_t mn_app_album_value_id_locked(mn_app *app, const char *album_label)
{
    mn_facet           *f = NULL;
    const mn_facet_row *rows = NULL;
    int32_t             got = 0;
    int64_t             off = 0;
    int64_t             found = 0;

    if (!album_label || !album_label[0]) {
        return 0;
    }
    if (mn_facet_open(app->lib, MN_FACET_ALBUM, NULL, &f) != MNDB_OK || !f) {
        return 0;
    }
    for (;;) {
        int32_t i;
        if (mn_facet_window(f, off, 256, &rows, &got) != MNDB_OK ||
            !rows || got <= 0) {
            break;
        }
        for (i = 0; i < got; i++) {
            if (rows[i].label && strcmp(rows[i].label, album_label) == 0) {
                found = rows[i].value_id;
                break;
            }
        }
        if (found || got < 256) {
            break;
        }
        off += got;
    }
    mn_facet_close(f);
    return found;
}

/* Collect the file paths of every track in `album_label` (bounded by max).
 * Caller holds the lock. Returns the number written. */
static int mn_app_collect_album_paths_locked(mn_app *app,
                                             const char *album_label,
                                             char (*out)[MN_STR_PATH], int max)
{
    mn_filter_spec       spec;
    mn_query            *q = NULL;
    const mn_track_row  *rows = NULL;
    int32_t              got = 0;
    int                  n = 0;
    int64_t              album_id = mn_app_album_value_id_locked(app, album_label);

    if (album_id == MN_INVALID_ID) {
        return 0;
    }
    memset(&spec, 0, sizeof(spec));
    spec.include_missing = false;
    spec.cascade[0].dim = MN_FACET_ALBUM;
    spec.cascade[0].value_id = album_id;
    spec.cascade_len = 1;

    if (mn_query_open(app->lib, &spec, &q) != MNDB_OK || !q) {
        return 0;
    }
    if (mn_query_window(q, 0, max, &rows, &got) == MNDB_OK && rows) {
        int32_t i;
        for (i = 0; i < got && n < max; i++) {
            if (rows[i].path && rows[i].path[0]) {
                mn_copy_str(out[n], MN_STR_PATH, rows[i].path);
                n++;
            }
        }
    }
    mn_query_close(q);
    return n;
}

bool mn_app_write_tags(mn_app *app, int64_t id, const struct mn_tag_edit *edit,
                       char *err, size_t errn)
{
    mn_trackmeta m;
    mn_pb_resume rz;
    bool         ok;

    MN_APP_ERR(err, errn, "");
    if (!app || !app->lib || !edit || id == MN_INVALID_ID) {
        MN_APP_ERR(err, errn, "bad-args");
        return false;
    }
    memset(&rz, 0, sizeof(rz));

    MN_LOCK(&app->lib_lock);
    if (!mn_app_fetch_meta_locked(app, id, &m)) {
        MN_UNLOCK(&app->lib_lock);
        MN_APP_ERR(err, errn, "not-found");
        return false;
    }
    mn_app_release_if_loaded(app, m.path, &rz);
    MN_UNLOCK(&app->lib_lock);

    /* The heavy file rewrite runs OUTSIDE the app lock so the tick timer
     * and UI polls stay responsive. */
    ok = mn_tagw_write_tags(m.path, edit, err, errn);

    MN_LOCK(&app->lib_lock);
    if (ok) {
        /* Upsert the new tags (keyed by path); user columns (rating, play
         * counts, date_added) are preserved by the upsert contract. */
        mn_track_in t;
        /* keep_missing (partial edits, e.g. the album batch editor): an
         * empty field PRESERVED the file's value — mirror that in the db
         * from the pre-edit snapshot instead of nulling the column. */
        bool km = edit->keep_missing;
        memset(&t, 0, sizeof(t));
        t.path         = m.path;
        t.title        = edit->title[0]        ? edit->title
                       : (km && m.title[0]        ? m.title        : NULL);
        t.artist       = edit->artist[0]       ? edit->artist
                       : (km && m.artist[0]       ? m.artist       : NULL);
        t.album        = edit->album[0]        ? edit->album
                       : (km && m.album[0]        ? m.album        : NULL);
        t.album_artist = edit->album_artist[0] ? edit->album_artist
                       : (km && m.album_artist[0] ? m.album_artist : NULL);
        t.genre        = edit->genre[0]        ? edit->genre
                       : (km && m.genre[0]        ? m.genre        : NULL);
        /* composer empty = preserved in the FILE, so preserve it in the
         * db as well. */
        t.composer     = edit->composer[0] ? edit->composer
                       : (m.composer[0]    ? m.composer : NULL);
        t.format       = m.format[0] ? m.format : NULL;
        t.year         = edit->year     ? edit->year     : (km ? m.year  : 0);
        t.track        = edit->track_no ? edit->track_no : (km ? m.track : 0);
        t.disc         = m.disc;
        t.duration_ms  = m.duration_ms;
        t.sample_rate  = m.sample_rate;
        t.channels     = m.channels;
        t.bit_depth    = m.bit_depth;
        t.bitrate_kbps = m.bitrate_kbps;
        t.size         = m.size;
        t.mtime        = m.mtime;
        t.has_art      = m.has_art;
        (void)mn_library_upsert_track(app->lib, &t, NULL);
        app->query_dirty = true; app->alb_cache_valid = false;   /* the UI re-windows with fresh values */

        /* Keep the now-playing strip coherent when the active track was
         * the one edited (keep_missing: empty fields didn't change). */
        if (app->now_track_id == id) {
            if (!km || edit->title[0])
                mn_copy_str(app->now_title,  sizeof(app->now_title),  edit->title);
            if (!km || edit->artist[0])
                mn_copy_str(app->now_artist, sizeof(app->now_artist), edit->artist);
            if (!km || edit->album[0])
                mn_copy_str(app->now_album,  sizeof(app->now_album),  edit->album);
            if (!km || edit->album_artist[0] || edit->artist[0])
                mn_copy_str(app->now_album_artist, sizeof(app->now_album_artist),
                            edit->album_artist[0] ? edit->album_artist
                                                  : edit->artist);
        }
    }
    mn_app_reattach(app, &rz);
    MN_UNLOCK(&app->lib_lock);
    return ok;
}

bool mn_app_write_art(mn_app *app, int64_t id, const uint8_t *bytes,
                      size_t len, const char *mime, bool whole_album,
                      char *err, size_t errn)
{
    mn_trackmeta m;
    char (*paths)[MN_STR_PATH] = NULL;
    int   npaths = 0;
    int   i;
    bool  target_seen = false;
    bool  ok = false;

    MN_APP_ERR(err, errn, "");
    if (!app || !app->lib || id == MN_INVALID_ID || !bytes || len == 0) {
        MN_APP_ERR(err, errn, "bad-args");
        return false;
    }
    paths = (char (*)[MN_STR_PATH])calloc(MN_ALBUM_ART_MAX_TRACKS,
                                          MN_STR_PATH);
    if (!paths) {
        MN_APP_ERR(err, errn, "io-error");
        return false;
    }

    MN_LOCK(&app->lib_lock);
    if (!mn_app_fetch_meta_locked(app, id, &m)) {
        MN_UNLOCK(&app->lib_lock);
        free(paths);
        MN_APP_ERR(err, errn, "not-found");
        return false;
    }
    if (whole_album && m.album[0]) {
        npaths = mn_app_collect_album_paths_locked(app, m.album, paths,
                                                   MN_ALBUM_ART_MAX_TRACKS);
    }
    MN_UNLOCK(&app->lib_lock);

    if (npaths <= 0) {   /* single file (or the album lookup failed) */
        mn_copy_str(paths[0], MN_STR_PATH, m.path);
        npaths = 1;
    }

    /* Write per file: detach if it is the currently playing file, rewrite
     * outside the lock, reattach. The command's ok/error reflect the file
     * of the requested track id; sibling failures are non-fatal. */
    for (i = 0; i < npaths; i++) {
        mn_pb_resume rz;
        char         ferr[MN_TAGW_ERR_CAP];
        bool         wok;

        memset(&rz, 0, sizeof(rz));
        ferr[0] = '\0';

        MN_LOCK(&app->lib_lock);
        mn_app_release_if_loaded(app, paths[i], &rz);
        MN_UNLOCK(&app->lib_lock);

        wok = mn_tagw_write_art(paths[i], bytes, len, mime,
                                ferr, sizeof(ferr));

        MN_LOCK(&app->lib_lock);
        mn_app_reattach(app, &rz);
        MN_UNLOCK(&app->lib_lock);

        if (mn_path_eq(paths[i], m.path)) {
            target_seen = true;
            ok = wok;
            if (!wok) {
                MN_APP_ERR(err, errn, ferr);
            }
        }
    }

    /* The album query can miss the id'd file (e.g. hidden folder filter):
     * write it explicitly so the requested track always gets the art. */
    if (!target_seen) {
        mn_pb_resume rz;
        memset(&rz, 0, sizeof(rz));
        MN_LOCK(&app->lib_lock);
        mn_app_release_if_loaded(app, m.path, &rz);
        MN_UNLOCK(&app->lib_lock);
        ok = mn_tagw_write_art(m.path, bytes, len, mime, err, errn);
        MN_LOCK(&app->lib_lock);
        mn_app_reattach(app, &rz);
        MN_UNLOCK(&app->lib_lock);
    }

    free(paths);

    if (ok) {
        MN_LOCK(&app->lib_lock);
        mn_app_refresh_art_cache(app, &m);
        MN_UNLOCK(&app->lib_lock);
    }
    return ok;
}

bool mn_app_write_lyrics(mn_app *app, int64_t id, const char *text,
                         const char *synced_lrc)
{
    mn_trackmeta m;
    mn_pb_resume rz;
    bool         embedded = false;
    bool         sidecar = false;

    if (!app || !app->lib || id == MN_INVALID_ID) {
        return false;
    }
    memset(&rz, 0, sizeof(rz));

    MN_LOCK(&app->lib_lock);
    if (!mn_app_fetch_meta_locked(app, id, &m)) {
        MN_UNLOCK(&app->lib_lock);
        return false;
    }
    mn_app_release_if_loaded(app, m.path, &rz);
    MN_UNLOCK(&app->lib_lock);

    embedded = mn_tagw_write_lyrics(m.path, text ? text : "", NULL, 0);
    if (synced_lrc && synced_lrc[0]) {
        sidecar = mn_tagw_write_sidecar_lrc(m.path, synced_lrc);
    } else if (!embedded && text && text[0]) {
        /* Embedding failed (read-only format / locked file) and there's no
         * synced LRC — persist the plain text as a .txt sidecar so these
         * tracks don't silently refetch their lyrics every session. */
        sidecar = mn_tagw_write_sidecar_txt(m.path, text);
    } else if (text && !text[0]) {
        /* cleared: also drop any stale .txt sidecar */
        (void)mn_tagw_write_sidecar_txt(m.path, "");
    }

    MN_LOCK(&app->lib_lock);
    mn_app_reattach(app, &rz);
    MN_UNLOCK(&app->lib_lock);
    return embedded || sidecar;
}

bool mn_app_read_lyrics(mn_app *app, int64_t id, char *out, size_t n)
{
    mn_trackmeta m;
    bool         ok;

    if (!out || n == 0) {
        return false;
    }
    out[0] = '\0';
    if (!app || !app->lib || id == MN_INVALID_ID) {
        return false;
    }
    MN_LOCK(&app->lib_lock);
    ok = mn_app_fetch_meta_locked(app, id, &m);
    MN_UNLOCK(&app->lib_lock);
    if (!ok) {
        return false;
    }
    /* Whole-file parse happens OUTSIDE the app lock. */
    return mn_tagw_read_lyrics(m.path, out, n);
}

/* ================================================================== */
/* Library sync (phone <-> desktop, SYNC_PROTOCOL v1)                 */
/* ================================================================== */

/* Lock hooks handed to sync.c so its library access serializes against the
 * scanner and UI (sync.c never holds them across network I/O). */
static void mn_app_sync_lock(void *user)
{
    MN_LOCK(&((mn_app *)user)->lib_lock);
}

static void mn_app_sync_unlock(void *user)
{
    MN_UNLOCK(&((mn_app *)user)->lib_lock);
}

/* Per-field sync participation. Stored INVERTED (skip flags) so the
 * calloc'd mn_app defaults to everything enabled. */
static bool g_sync_skip_likes   = false;
static bool g_sync_skip_ratings = false;
static bool g_sync_skip_plays   = false;

void mn_app_set_sync_fields(mn_app *app, bool likes, bool ratings, bool plays)
{
    (void)app;
    g_sync_skip_likes   = !likes;
    g_sync_skip_ratings = !ratings;
    g_sync_skip_plays   = !plays;
}

static void mn_app_sync_env(mn_app *app, mn_sync_env *env)
{
    env->lib            = app->lib;
    env->lock           = mn_app_sync_lock;
    env->unlock         = mn_app_sync_unlock;
    env->lock_user      = app;
    env->fields.likes   = !g_sync_skip_likes;
    env->fields.ratings = !g_sync_skip_ratings;
    env->fields.plays   = !g_sync_skip_plays;
}

/* A merge changed rows: the browse query + album cache are stale. */
static void mn_app_sync_mark_dirty(mn_app *app)
{
    MN_LOCK(&app->lib_lock);
    app->query_dirty     = true;
    app->alb_cache_valid = false;
    MN_UNLOCK(&app->lib_lock);
}

/* Forwarding shim: capture the merge counts while relaying progress to the
 * bridge's callback, so the wrapper knows whether to invalidate caches. */
struct mn_app_sync_relay {
    mn_app_sync_cb cb;
    void          *user;
    int            applied;
};

static void mn_app_sync_progress(void *user, const char *state,
                                 int applied, int skipped, int pushed,
                                 const char *error)
{
    struct mn_app_sync_relay *r = (struct mn_app_sync_relay *)user;
    if (applied > r->applied) {
        r->applied = applied;
    }
    if (r->cb) {
        r->cb(r->user, state, applied, skipped, pushed, error);
    }
}

bool mn_app_sync_run(mn_app *app, const char *host, int port,
                     mn_app_sync_cb cb, void *user)
{
    mn_sync_env              env;
    struct mn_app_sync_relay relay;
    bool                     ok;

    if (!app || !app->lib) {
        return false;
    }
    mn_app_sync_env(app, &env);
    relay.cb      = cb;
    relay.user    = user;
    relay.applied = 0;
    ok = mn_sync_run(&env, host, port, mn_app_sync_progress, &relay);
    if (relay.applied > 0) {
        mn_app_sync_mark_dirty(app);
    }
    return ok;
}

bool mn_app_sync_export(mn_app *app, const char *path_or_null,
                        char *out_path, size_t out_n)
{
    mn_sync_env env;
    char        path[MN_STR_PATH];

    if (!app || !app->lib) {
        return false;
    }
    if (path_or_null && path_or_null[0]) {
        mn_copy_str(path, sizeof(path), path_or_null);
    } else {
        mn_sync_default_path(app->data_dir, path, sizeof(path));
    }
    if (out_path && out_n > 0) {
        mn_copy_str(out_path, out_n, path);
    }
    mn_app_sync_env(app, &env);
    return mn_sync_export_file(&env, path);
}

bool mn_app_sync_import(mn_app *app, const char *path_or_null,
                        int *applied, int *skipped)
{
    mn_sync_env env;
    char        path[MN_STR_PATH];
    int         a = 0, s = 0;
    bool        ok;

    if (applied) *applied = 0;
    if (skipped) *skipped = 0;
    if (!app || !app->lib) {
        return false;
    }
    if (path_or_null && path_or_null[0]) {
        mn_copy_str(path, sizeof(path), path_or_null);
    } else {
        mn_sync_default_path(app->data_dir, path, sizeof(path));
    }
    mn_app_sync_env(app, &env);
    ok = mn_sync_import_file(&env, path, &a, &s);
    if (applied) *applied = a;
    if (skipped) *skipped = s;
    if (ok && a > 0) {
        mn_app_sync_mark_dirty(app);
    }
    return ok;
}
