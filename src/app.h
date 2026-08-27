/*
 * app.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Top-level, UI-facing application controller.
 *
 * mn_app is the single opaque handle the UI layer (CEF host + the HTML
 * bridge) talks to. It owns the audio engine, the library database, the
 * background scanner, the stem-separation pipeline and all playback
 * state. Every function here is designed to be called from the CEF/JS
 * bridge and is safe to call from the UI thread; long-running work
 * (scanning, decoding, stem inference) happens on background threads and
 * is surfaced through the polling structs (mn_scan / mn_now) plus
 * mn_app_tick().
 *
 * Designed for EXTREMELY LARGE libraries (~1M tracks): all list access is
 * windowed (offset/count), row/album counts are cached, and no API here
 * performs an O(n) scan of the library.
 *
 * This header is the compile-time contract for app.c and every other
 * module that targets the controller. It intentionally exposes only
 * plain-old-data structs and opaque handles so the HTML bridge and the
 * CEF host can bind against stable field/function names.
 *
 * Naming: functions/types use the "mn_" prefix, macros use "MN_".
 */

#ifndef MN_APP_H
#define MN_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time limits                                                */
/* ------------------------------------------------------------------ */

/* Number of separable stems the neural pipeline can surface. The stem
 * meter/gain/mute/solo arrays are sized to this. */
#define MN_STEM_COUNT 9

/* Fixed capacity of short inline text fields inside the POD structs so
 * the bridge can treat them as value types (no ownership questions).
 * Longer values (e.g. filesystem paths) get their own wider buffers. */
#define MN_STR_SHORT 128
#define MN_STR_PATH  1024

/* ------------------------------------------------------------------ */
/* Opaque handle                                                      */
/* ------------------------------------------------------------------ */

/* The application controller. Created with mn_app_create(), released
 * with mn_app_destroy(). All other calls take this handle. */
typedef struct mn_app mn_app;

/* ------------------------------------------------------------------ */
/* Enumerations                                                       */
/* ------------------------------------------------------------------ */

/* The active browsing view. Governs what row_count()/window() and the
 * album_* calls enumerate, and how sorting/search are interpreted. */
typedef enum mn_view {
    MN_VIEW_TRACKS = 0,
    MN_VIEW_ALBUMS,
    MN_VIEW_ARTISTS,
    MN_VIEW_GENRES,
    MN_VIEW_FOLDERS,
    MN_VIEW_PLAYLISTS,
    MN_VIEW_COUNT       /* sentinel: number of views, not a valid view */
} mn_view;

/* Sort key for the current view. Not every key is meaningful in every
 * view; the controller maps unsupported keys to a sensible default. */
typedef enum mn_sort {
    MN_SORT_TITLE = 0,
    MN_SORT_ARTIST,
    MN_SORT_ALBUM,
    MN_SORT_GENRE,
    MN_SORT_YEAR,
    MN_SORT_DURATION,
    MN_SORT_RATING,
    MN_SORT_PLAY_COUNT,
    MN_SORT_DATE_ADDED,
    MN_SORT_TRACK_NO,
    MN_SORT_LAST_PLAYED,  /* Recently Played */
    MN_SORT_DATE_CREATED, /* filesystem creation (birth) time */
    MN_SORT_COUNT       /* sentinel */
} mn_sort;

/* Repeat mode, advanced by mn_app_cycle_repeat(). */
typedef enum mn_repeat {
    MN_REPEAT_OFF = 0,  /* stop at end of queue                       */
    MN_REPEAT_ALL,      /* loop the whole queue                       */
    MN_REPEAT_ONE,      /* loop the current track                     */
    MN_REPEAT_COUNT     /* sentinel                                   */
} mn_repeat;

/* ------------------------------------------------------------------ */
/* Data rows                                                          */
/* ------------------------------------------------------------------ */

/* A single track row as surfaced to the list UI. Populated by
 * mn_app_window(); every field is a copy the caller may hold for the
 * lifetime of the returned window (not longer). String fields are
 * NUL-terminated fixed buffers so the bridge can copy them by value. */
typedef struct mn_row {
    int64_t id;                         /* stable track id (DB pk)      */
    char    title[MN_STR_SHORT];
    char    artist[MN_STR_SHORT];
    char    album_artist[MN_STR_SHORT]; /* "" when untagged (fall back to artist) */
    char    album[MN_STR_SHORT];
    char    genre[MN_STR_SHORT];
    int32_t duration_ms;                /* track length in ms           */
    int32_t year;                       /* 0 if unknown                 */
    int32_t track_no;                   /* 0 if unknown                 */
    int32_t disc_no;                    /* 0 if unknown / single-disc   */
    int32_t rating;                     /* 0..5 (0 = unrated)           */
    int32_t liked;                      /* -1 dislike, 0 none, 1 like   */
    int64_t play_count;
    int32_t bitrate_kbps;               /* 0 if unknown                 */
    int64_t size;                       /* file size in bytes, 0 unknown */
    int64_t date_added;                 /* unix seconds first indexed    */
    bool    missing;                    /* file vanished from disk       */
    char    path[MN_STR_PATH];          /* absolute filesystem path     */
} mn_row;

/* An album row for the ALBUMS grid. Populated by mn_app_album_window().
 * format/sample_rate/bit_depth describe the album's dominant/first
 * track and are advisory (mixed-format albums report the primary). */
typedef struct mn_album {
    int64_t id;                         /* stable album id              */
    char    title[MN_STR_SHORT];
    char    artist[MN_STR_SHORT];       /* album artist                 */
    char    art_path[MN_STR_PATH];      /* cached cover art file, or ""  */
    int32_t year;                       /* 0 if unknown                 */
    int32_t track_count;
    char    format[MN_STR_SHORT];       /* e.g. "FLAC", "MP3"           */
    int32_t sample_rate;                /* Hz                           */
    int32_t bit_depth;                  /* bits per sample, 0 if lossy   */
    int32_t bitrate_kbps;               /* album's first-track bitrate   */
    int64_t size;                       /* total bytes across the album  */
    int64_t date_added;                 /* first track's date_added (unix
                                         * seconds; proxy for the album's
                                         * add date — see album_window)  */
    int64_t created;                    /* newest track FILE creation time
                                         * (a bulk import shares one
                                         * date_added day; created keeps
                                         * each album's real file date)  */
} mn_album;

/* A single facet value (artist / genre / composer / year) for browse views. */
typedef struct mn_facet_value {
    int64_t id;                         /* stable facet value id (cascade key) */
    char    label[MN_STR_SHORT];        /* display text                        */
    int32_t count;                      /* track count under this value        */
} mn_facet_value;

/* A playlist as surfaced to the UI (name + track count). */
typedef struct mn_playlist_item {
    int64_t id;
    char    name[MN_STR_SHORT];
    int32_t track_count;
} mn_playlist_item;

/* A library folder (directory that contains indexed tracks) as surfaced to
 * the folder-visibility UI. `hidden` folders are excluded from every list,
 * album grid and search result until unhidden (persisted across restarts). */
typedef struct mn_folder {
    int64_t id;                         /* stable folder id (DB pk)      */
    char    path[MN_STR_PATH];          /* absolute directory path       */
    int64_t track_count;                /* visible (non-missing) tracks  */
    bool    hidden;                     /* excluded from browsing/search */
} mn_folder;

/* ------------------------------------------------------------------ */
/* Now-playing snapshot                                               */
/* ------------------------------------------------------------------ */

/* Immutable snapshot of engine + playback state, filled by mn_app_now().
 * The UI polls this each frame (after mn_app_tick()) to render the
 * transport, format badge, volume/shuffle/repeat controls and the stem
 * mixer. All fields are values; nothing is owned by the caller. */
typedef struct mn_now {
    /* transport ---------------------------------------------------- */
    bool    playing;                    /* true while audio is running   */
    int64_t position_ms;                /* playhead in current track     */
    int64_t duration_ms;                /* current track length          */

    /* current track metadata -------------------------------------- */
    int64_t track_id;                   /* stable id of the playing track */
    int64_t album_id;                   /* albums-dimension id (0 none)   */
    char    track_path[MN_STR_PATH];    /* file path (host-side kind check)*/
    char    track_title[MN_STR_SHORT];
    char    track_artist[MN_STR_SHORT];
    char    track_album[MN_STR_SHORT];
    /* Album artist (falls back to the track artist when untagged). This is
     * the string the album-art cache is KEYED by — art lookups must use it,
     * not track_artist, or VA/feat. tracks miss the cache. */
    char    track_album_artist[MN_STR_SHORT];
    char    art_path[MN_STR_PATH];      /* cover art file, or ""         */
    int32_t liked;                      /* current track: -1/0/1         */

    /* stream format ----------------------------------------------- */
    char    format[MN_STR_SHORT];       /* e.g. "FLAC", "MP3", "WAV"     */
    int32_t sample_rate;                /* Hz (source)                   */
    int32_t bit_depth;                  /* bits/sample, 0 if lossy       */
    int32_t channels;
    int32_t bitrate_kbps;               /* 0 if not applicable            */

    /* device-side output format ------------------------------------ */
    /* The REAL hardware format the audio device is running at, so the
     * UI can show the full SOURCE -> OUTPUT chain (e.g. "MP3 44.1kHz
     * 16-bit -> 48kHz 24-bit 2ch"). 0 when no device/track is active. */
    int32_t out_sample_rate;            /* Hz                            */
    int32_t out_bit_depth;              /* bits/sample                   */
    int32_t out_channels;
    bool    out_exclusive;              /* device really in exclusive mode */
    char    out_pcm[12];                /* device sample format "PCM 24"… */
    /* quality-event transparency (the app being honest about any lossy step)*/
    int32_t pipe_channels;              /* channels actually delivered      */
    bool    downmixed;                  /* a >2ch source folded to stereo   */
    bool    rate_limited;               /* pipeline rate clamped below source*/

    /* mixer / modes ----------------------------------------------- */
    float   volume;                     /* 0.0 .. 1.0                    */
    bool    shuffle;
    mn_repeat repeat;

    /* neural stem separation -------------------------------------- */
    bool    stems_available;            /* model session loaded + usable */
    bool    stems_loading;              /* async model load in flight     */
    bool    stems_enabled;              /* separation requested          */
    bool    stems_passthrough;          /* bypass mixing, play original   */
    bool    neural_active;              /* inference currently running    */
    char    stem_provider[MN_STR_SHORT];/* e.g. "CUDA", "CPU"            */
    float   stem_rt_factor;             /* realtime factor (x); >1 = faster*/
    float   stem_fraction;              /* 0..1 buffered/ready fraction   */
    float   stem_meters[MN_STEM_COUNT]; /* per-stem output level 0..1     */

    /* online session (internet radio / streamed podcast) ----------- */
    bool    online;                     /* engine is playing an HTTP source */
    bool    online_live;                /* unseekable live mount (radio)    */
    char    online_kind[12];            /* "radio" | "podcast" | "stream"   */
    char    online_url[MN_STR_PATH];    /* source URL (or local file path)  */
    char    online_art[MN_STR_PATH];    /* station favicon / show artwork    */
    char    stream_title[MN_STR_SHORT]; /* latest ICY StreamTitle, "" none  */
} mn_now;

/* ------------------------------------------------------------------ */
/* Scan progress snapshot                                             */
/* ------------------------------------------------------------------ */

/* Immutable snapshot of the background library scan, filled by
 * mn_app_scan_status(). Counts are cumulative for the in-flight (or
 * most recent) scan. */
typedef struct mn_scan {
    bool    active;                     /* a scan is running             */
    int64_t found;                      /* candidate files discovered    */
    int64_t processed;                  /* files fully tagged + stored   */
    int64_t dirs_scanned;
    int64_t skipped;                    /* unchanged / non-audio          */
    int64_t tag_errors;                 /* metadata parse failures        */
    int64_t io_errors;                  /* read/access failures           */
    char    source[MN_STR_PATH];        /* dir currently being scanned    */
} mn_scan;

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */

/* Persistent user settings. Read/written as a value struct so the
 * bridge can round-trip the whole panel in one call. */
typedef struct mn_settings {
    bool    exclusive;                  /* exclusive/WASAPI-exclusive out */
    int32_t crossfade_ms;               /* 0 = off                       */
    bool    replaygain;                 /* apply ReplayGain if present    */
    int32_t album_art_size;             /* cached art edge length, px     */
    int32_t replaygain_mode;            /* 0=off 1=track 2=album          */
    float   rg_preamp_db;               /* ReplayGain preamp/target (dB)  */
    int32_t stem_cache_gb;              /* stems disk-cache cap (GB)      */
    int32_t art_cache_mb;               /* art caches cap (MB, 0=default) */
    bool    depth_batch;                /* background depth pre-generation */
    bool    infer_tags;                 /* fill missing tags from file/
                                           folder names at scan (default on) */
    bool    watch_folders;              /* live-monitor library roots and
                                           rescan on change (default on)    */
    bool    low_power;                  /* low-power mode: flat 2D art, no
                                           animations, de-tuned compositor,
                                           capped ONNX threads, stretched
                                           polling (default off)            */
    /* Audiophile output */
    bool    hifi_native_bits;           /* exclusive-mode native bit depth  */
    int32_t ab_rate_cap_hz;             /* audiobook power cap: 0=off        */
    int32_t ab_bits_cap;                /* audiobook depth cap: 0=off        */
} mn_settings;

/* ================================================================== */
/* Lifecycle                                                          */
/* ================================================================== */

/* Create the controller. data_dir is a writable directory used for the
 * SQLite library database, the album-art cache and settings. Returns
 * NULL on failure. */
mn_app *mn_app_create(const char *data_dir);

/* Destroy the controller, stopping playback and joining all background
 * threads. Safe to call with NULL. */
void mn_app_destroy(mn_app *app);

/* ================================================================== */
/* Library scanning                                                   */
/* ================================================================== */

/* Register a folder as a watched library root and kick off a scan of it
 * on a background thread. Returns true if the folder was accepted. */
bool mn_app_add_folder(mn_app *app, const char *path);

/* Rescan all registered folders on a background thread. Incremental:
 * unchanged files are skipped. */
void mn_app_rescan(mn_app *app);

/* Fill *out with the current scan progress snapshot. */
void mn_app_scan_status(mn_app *app, mn_scan *out);

/* Backfill filename/folder-inferred tags onto rows whose title/artist/
 * album/track are empty (legacy rows scanned before inference existed).
 * Blocking — run it on a worker thread. Idempotent. Returns the number
 * of rows updated. */
int64_t mn_app_reinfer_untagged(mn_app *app);

/* Online backup of the library database to dest_path (safe while playing /
 * scanning; call from a background thread). */
bool mn_app_backup_db(mn_app *app, const char *dest_path);

/* Aggregate stats for one scan root: indexed tracks, distinct albums, total
 * bytes and newest date_added under the directory. Any thread. */
bool mn_app_root_stats(mn_app *app, const char *root,
                       int64_t *tracks, int64_t *albums,
                       int64_t *bytes, int64_t *newest_added);

/* Per-field library-sync participation (Settings -> Sync toggles): a
 * disabled group is neither exported in snapshots nor applied from remote
 * ones. Defaults: all enabled. */
void mn_app_set_sync_fields(mn_app *app, bool likes, bool ratings, bool plays);

/* Remote-control pairing info carried inside pushed sync snapshots (the
 * "control" block): the phone learns where the desktop's control listener
 * lives and its shared token. Empty/NULL token disables emission. */
void mn_app_set_control_info(mn_app *app, int port, const char *token,
                             const char *name);

/* Load persisted settings (<data_dir>\settings.txt) over the current values.
 * Called once by mn_app_create; exposed for tests. */
void mn_app_load_settings(mn_app *app);

/* Release the calling thread's lazily-created SQLite reader connection.
 * Worker threads call this before exiting (otherwise the connection lives
 * until the app closes). Safe with none open. */
void mn_app_thread_detach(mn_app *app);

/* Signal long-running background loops to bail out promptly — call at UI
 * teardown before mn_app_destroy. Safe to call repeatedly. */
void mn_app_request_shutdown(mn_app *app);

/* Register a folder as a rescan root WITHOUT starting a scan (dedup +
 * subsumption). Used by the host to replay persisted roots. */
bool mn_app_register_root(mn_app *app, const char *path);

/*
 * Wipe the whole library (tracks, playlists, dimensions), stop playback
 * and the scanner, optionally clear the album-art thumbnail cache, then
 * rescan every registered root (falling back to roots derived from the
 * db's folder dimension when none were registered this session). Blocking
 * for the wipe itself; the rescan runs in the background as usual.
 */
bool mn_app_reset_library(mn_app *app, bool clear_art);

/* Permanently delete every missing-flagged row. Returns the count. */
int64_t mn_app_purge_missing(mn_app *app);

/* ================================================================== */
/* AI model downloads (Hugging Face)                                  */
/* ================================================================== */

/*
 * The AI-models directory (<data_dir>/ai-models) where downloaded stem/depth
 * ONNX models are stored and picked up on next load. Copies at most n bytes
 * (NUL-terminated) into out.
 */
void mn_app_models_dir(mn_app *app, char *out, size_t n);

/*
 * Kick off a Hugging Face download of file `file` from repo `repo` into the
 * app's ai-models directory. `id` is the UI-side model id, echoed back so the
 * host can key progress replies to the right card. Returns true if the
 * download was accepted (worker started); false if one is already in flight
 * (busy) or the arguments are invalid.
 *
 * Progress is delivered through the caller-supplied callback, which runs on
 * the downloader worker thread; the host marshals it back to the UI thread.
 */
typedef void (*mn_app_dl_cb)(void *user, const char *id,
                             int64_t done, int64_t total,
                             bool finished, const char *err);
bool mn_app_download_model(mn_app *app, const char *id,
                           const char *repo, const char *file,
                           const char *save_as,
                           mn_app_dl_cb cb, void *user);

/* ================================================================== */
/* Browsing: view / search / sort                                    */
/* ================================================================== */

/* Set/get the active browsing view. Changing the view resets the
 * windowed enumeration but preserves the search string and sort key. */
void    mn_app_set_view(mn_app *app, mn_view view);
mn_view mn_app_get_view(mn_app *app);

/* Set the full-text search filter for the current view. Pass NULL or ""
 * to clear. Backed by FTS5; safe to call on every keystroke. */
void mn_app_set_search(mn_app *app, const char *query);

/* Set the sort key (and direction) for the current view. */
void mn_app_set_sort(mn_app *app, mn_sort key, bool ascending);

/* ================================================================== */
/* Browsing: windowed row access (TRACKS-style views)                */
/* ================================================================== */

/* Total number of rows matching the current view + search. Cached;
 * O(1) amortized. */
int64_t mn_app_row_count(mn_app *app);

/* Copy up to `count` rows starting at `offset` into the caller-provided
 * `rows` array (which must hold at least `count` mn_row). Returns the
 * number actually written (may be < count near the end). */
int32_t mn_app_window(mn_app *app, int64_t offset, int32_t count, mn_row *rows);

/* Independent track search for the live suggestions dropdown (does not affect
 * the current view's query/sort/search). Prefix-matches via FTS. */
int32_t mn_app_search_tracks(mn_app *app, const char *query,
                             int32_t count, mn_row *rows);

/* ================================================================== */
/* Browsing: albums                                                   */
/* ================================================================== */

/* Total number of albums matching the current search. */
int64_t mn_app_album_count(mn_app *app);

/* Copy up to `count` albums starting at `offset` into `albums` (which
 * must hold at least `count`). Returns the number written. */
int32_t mn_app_album_window(mn_app *app, int64_t offset, int32_t count, mn_album *albums);

/* Copy the tracks of one album (ordered by disc/track number) into
 * `rows` (capacity `count`). Returns the number written. */
int32_t mn_app_album_tracks(mn_app *app, int64_t album_id, int32_t count, mn_row *rows);

/* Facet browse (Artists / Genres / Composers / Years). `dim` is an
 * mn_facet_dim value from library_db.h. */
int32_t mn_app_facet_window(mn_app *app, int dim, int64_t offset, int32_t count,
                            mn_facet_value *out);
int32_t mn_app_facet_count(mn_app *app, int dim);
/* Tracks under one facet value (drill-in). */
int32_t mn_app_facet_tracks(mn_app *app, int dim, int64_t value_id,
                            int32_t count, mn_row *rows);

/* Playlists (static). Thin wrappers over the mn_playlist_* DB API. */
int32_t mn_app_playlist_list(mn_app *app, mn_playlist_item *out, int32_t max);
int64_t mn_app_playlist_create(mn_app *app, const char *name);
int     mn_app_playlist_rename(mn_app *app, int64_t id, const char *name);
int     mn_app_playlist_delete(mn_app *app, int64_t id);
int     mn_app_playlist_add(mn_app *app, int64_t id, int64_t track_id);
int     mn_app_playlist_remove_at(mn_app *app, int64_t id, int64_t position);
int     mn_app_playlist_move(mn_app *app, int64_t id, int64_t from, int64_t to);
int32_t mn_app_playlist_tracks(mn_app *app, int64_t id, int32_t count, mn_row *rows);

/* ================================================================== */
/* Browsing: folders + selective visibility                           */
/* ================================================================== */

/* Copy up to `max` library folders (sorted by path) into `out`. Includes
 * HIDDEN folders (so they can be unhidden). Returns the number written. */
int32_t mn_app_folder_list(mn_app *app, mn_folder *out, int32_t max);

/* Media-tool scoped window: rows under `prefix`, independent of the view /
 * search / category state. Sorted album -> disc/track. Fills *out_total
 * with the full match count when non-NULL. */
int32_t mn_app_tracks_under(mn_app *app, const char *prefix, int64_t offset,
                            int32_t count, mn_row *rows, int64_t *out_total);

/* Category scoping: per-kind libraries. kind "" (or NULL) = the default
 * music library (every non-music root excluded); any other kind (audiobook,
 * podcast, custom designations like "ost") switches every browse/search
 * surface to ONLY roots of that kind. */
void mn_app_set_category_kind(mn_app *app, const char *kind);

/* Read back the kind that is ACTUALLY active. C is the authority: a request
 * can be coerced (an alias folded to ""), and mn_app_set_kind_roots below can
 * drop the active kind on its own when the kind's last root disappears — so
 * the host must never assume the kind it last asked for is the kind in force.
 * Writes at most `n` bytes (always NUL-terminated when n > 0). Returns false
 * only on a bad argument. */
bool mn_app_get_category_kind(mn_app *app, char *out, size_t n);

/* Push every non-music root with its kind label (from the host's
 * folder_kinds registry). Roots are path prefixes; max MN_MAX_KIND_ROOTS. */
void mn_app_set_kind_roots(mn_app *app, const char kinds[][32],
                           const char paths[][512], int n);

/* Hide/unhide one folder. Hidden folders vanish from the track list, the
 * album grid and search results (they still list when explicitly browsed
 * via a folder cascade). Persisted under data_dir; survives restarts.
 * Returns true if the folder now has the requested visibility. */
bool mn_app_folder_set_hidden(mn_app *app, int64_t folder_id, bool hidden);

/* True if `folder_id` is currently hidden. */
bool mn_app_folder_hidden(mn_app *app, int64_t folder_id);

/* Permanently remove one folder: delete its tracks (and the folder row)
 * from the library, drop matching rescan roots and any hidden-set entry,
 * and mark the browse query dirty. Returns the number of tracks removed
 * (-1 on failure). */
int64_t mn_app_remove_folder(mn_app *app, int64_t folder_id);

/* ================================================================== */
/* Library statistics                                                 */
/* ================================================================== */

#define MN_APP_STATS_MAX_FORMATS 16

typedef struct mn_app_stats {
    int64_t tracks;                     /* non-missing tracks            */
    int64_t albums;                     /* distinct albums               */
    int64_t artists;                    /* distinct artists              */
    int64_t missing;                    /* missing-flagged rows          */
    int64_t duration_ms;                /* summed play time              */
    int64_t size_bytes;                 /* summed file size              */
    float   hires_pct;                  /* % tracks >=24bit or >=88.2kHz */
    float   lyrics_pct;                 /* % tracks with lyrics; -1 when
                                         * skipped (library too large)   */
    struct { char fmt[24]; int64_t n; } formats[MN_APP_STATS_MAX_FORMATS];
    int32_t format_count;
} mn_app_stats;

/* Aggregate library statistics (single SQL pass + bounded sidecar probe
 * for lyrics). Returns true on success. */
bool mn_app_get_stats(mn_app *app, mn_app_stats *out);

/* ================================================================== */
/* Playback / transport                                              */
/* ================================================================== */

/* Start playback from a row. The playback queue is built from the
 * current view/search/sort with `row_id` as the starting track. */
void mn_app_play_row(mn_app *app, int64_t row_id);

/* Start playback of an entire album from its first track. */
void mn_app_play_album(mn_app *app, int64_t album_id);

/* Queue an entire album (disc/track order) and start at `track_id` when it
 * belongs to the album, else at the first track. This is the queue context
 * for "played from an album's track list". */
void mn_app_play_album_track(mn_app *app, int64_t album_id, int64_t track_id);

/* Append a single track (by id) or a whole album (by album_id) to the END of
 * the current playback queue without interrupting what is playing ("Queue
 * last"). Exactly one of track_id / album_id should be > 0. No-op on miss. */
void mn_app_queue_last(mn_app *app, int64_t track_id, int64_t album_id);

/* Insert a single track (by id) or a whole album (by album_id) immediately
 * AFTER the current track ("Queue next"), without interrupting playback.
 * Exactly one of track_id / album_id should be > 0. No-op on miss. */
void mn_app_queue_next(mn_app *app, int64_t track_id, int64_t album_id);

/* Permanently remove a track (by id) from the library. Returns true if a row
 * was deleted. The file on disk is NOT touched. */
bool mn_app_remove_track(mn_app *app, int64_t track_id);

/* Resolve a track's file path by id (for host-side shell operations such as
 * moving the file to the Recycle Bin). Thread-safe (takes the lib lock).
 * Returns false when the id is unknown. */
bool mn_app_track_path(mn_app *app, int64_t id, char *out, size_t out_n);

/* Close the engine's open decoder if it currently holds `path` (the loaded —
 * even playing — track), so a host-side shell delete is not denied by our own
 * open handle. Playback of that file simply ends; no resume state is kept
 * (the file is going away). Thread-safe (takes the lib lock). */
void mn_app_release_path(mn_app *app, const char *path);

/* Toggle between play and pause. */
void mn_app_toggle_pause(mn_app *app);

/* Stop: pause and rewind to the start (distinct from toggle-pause). */
void mn_app_stop(mn_app *app);

/* Advance to the next / previous track in the queue (honoring shuffle
 * and repeat). */
void mn_app_next(mn_app *app);
void mn_app_prev(mn_app *app);

/* Seek within the current track. */
void mn_app_seek_ms(mn_app *app, int64_t position_ms);

/* Set output volume, 0.0 .. 1.0 (clamped). */
void mn_app_set_volume(mn_app *app, float volume);

/* DSP / 10-band EQ control (delegates to the engine's post-mix chain). */
void mn_app_set_dsp_enabled(mn_app *app, int enabled);
int  mn_app_get_dsp_enabled(mn_app *app);
void mn_app_set_eq_enabled(mn_app *app, int enabled);
void mn_app_set_eq_band(mn_app *app, int band, float gain_db);
void mn_app_set_eq_preset(mn_app *app, int preset, float out_gains[10], float *out_preamp);
void mn_app_set_preamp(mn_app *app, float preamp_db);
void mn_app_set_balance(mn_app *app, float balance);
void mn_app_set_limiter(mn_app *app, int enabled, float threshold_db, float ceiling_db);
void mn_app_set_master(mn_app *app, float gain_db);
void mn_app_get_eq(mn_app *app, float out_gains[10], float *out_preamp, int *out_enabled);
/* Report DSP state the engine has no getters for (EQ modal restore). */
void mn_app_get_dsp_extra(mn_app *app, float *balance, int *limiter_on,
                          float *lim_thresh, float *lim_ceil, float *master_db);

/* Spectrum analyzer bars for the visualizer (returns count written). */
int  mn_app_get_spectrum(mn_app *app, float *out, int max);

/* Sleep timer: pause playback after `minutes` (0 cancels). */
void mn_app_set_sleep_timer(mn_app *app, int minutes);
int  mn_app_get_sleep_remaining(mn_app *app);

/* Resume-on-launch: load a track PAUSED at position_ms (never audible). */
void mn_app_resume_row(mn_app *app, int64_t row_id, int64_t position_ms);

/* Liked-only browse filter for the "Liked songs" smart list. */
void mn_app_set_liked_only(mn_app *app, bool on);

/* Cache directories for the settings storage panel (any out may be NULL). */
void mn_app_cache_paths(mn_app *app, char *art, char *stems, char *models,
                        size_t cap);

/* Replace an album's cached art from an image file (online fetch). */
bool mn_app_ingest_album_art(mn_app *app, const char *artist,
                             const char *album, const char *image_path);

/* Import .m3u/.m3u8/.pls files found under the library roots as static
 * playlists (idempotent by name). Returns playlists created. */
int  mn_app_import_playlists(mn_app *app);

/* Enable/disable shuffle for the current queue. */
void mn_app_set_shuffle(mn_app *app, bool enabled);

/* Advance repeat mode OFF -> ALL -> ONE -> OFF. */
void mn_app_cycle_repeat(mn_app *app);

/* Set a track's rating, 0..5 (0 = unrated). Persisted to the library. */
void mn_app_set_rating(mn_app *app, int64_t row_id, int32_t rating);

/* Thumbs up/down: v = 1 like, -1 dislike, 0 clear. Persisted. */
void mn_app_set_liked(mn_app *app, int64_t row_id, int32_t v);

/* Current thumbs state of a track (-1/0/1; 0 when unknown). */
int32_t mn_app_get_liked(mn_app *app, int64_t row_id);

/* ------------------------------------------------------------------ */
/* Audiobook progress (book_progress table)                           */
/* ------------------------------------------------------------------ */

/* One Continue-Listening shelf entry: a recently-touched book's current
 * chapter + progress + display metadata. Plain-old-data (owned fixed
 * buffers) per this header's contract — app.c translates from the DB
 * layer's own row type. */
typedef struct mn_book {
    int64_t album_id;
    int64_t track_id;              /* the CURRENT chapter               */
    int64_t pos_ms;                /* position within that chapter      */
    int64_t duration_ms;           /* that chapter's duration           */
    double  percent;               /* whole-book completion 0..1        */
    bool    finished;
    int64_t updated;               /* unix seconds of last progress     */
    char    album[MN_STR_SHORT];   /* book title (album tag)            */
    char    album_artist[MN_STR_SHORT];
    char    artist[MN_STR_SHORT];
    char    title[MN_STR_SHORT];   /* current chapter's title           */
} mn_book;

/* Note the current position of (book, chapter); the DB layer computes
 * whole-book percent and snapshots the track's content_hash. */
void mn_app_book_note(mn_app *app, int64_t album_id, int64_t track_id,
                      int64_t pos_ms, int64_t updated);

/* The book's current chapter + position + completion (false = untouched). */
bool mn_app_book_get(mn_app *app, int64_t album_id, int64_t *out_track,
                     int64_t *out_pos, double *out_percent,
                     bool *out_finished);

/* Most-recently-touched books, newest first — the Continue shelf feed.
 * Returns entries filled (<= max). */
int mn_app_recent_books(mn_app *app, mn_book *out, int max);

/* Grid-tile badge feed: every touched book's (album_id, percent, finished)
 * in parallel arrays. Returns entries filled (<= max). */
int mn_app_book_badges(mn_app *app, int64_t *album_ids, double *percents,
                       bool *finisheds, int max);

/* All remembered chapter positions within one book (parallel arrays). */
int mn_app_book_chapters(mn_app *app, int64_t album_id, int64_t *track_ids,
                         int64_t *pos_ms, int max);

/* Pitch-preserved playback speed (audiobooks). 0.5..3.0 clamped; 1.0 =
 * stretcher fully bypassed (bit-perfect). Position stays in source time. */
void  mn_app_set_speed(mn_app *app, float speed);
float mn_app_get_speed(mn_app *app);

/* Bookmarks: named positions within a book (move-proof via content_hash). */
int64_t mn_app_bookmark_add(mn_app *app, int64_t album_id, int64_t track_id,
                            int64_t pos_ms, const char *note, int64_t created);
void    mn_app_bookmark_del(mn_app *app, int64_t bookmark_id);
int     mn_app_bookmark_list(mn_app *app, int64_t album_id, int64_t *ids,
                             int64_t *track_ids, int64_t *pos_ms,
                             char (*notes)[128], int64_t *created, int max);

/* content_hash backfill plumbing: rows still needing a fingerprint, and the
 * write-once setter (force only when the file's SIZE changed). The compute
 * + worker live in the host layer (cef_host.c hash_backfill). */
int mn_app_hashless_rows(mn_app *app, int64_t *ids, char (*paths)[1024],
                         int64_t *sizes, int max);
void mn_app_set_content_hash(mn_app *app, int64_t track_id, const char *hash,
                             bool force);

/* The opposite feed: rows that ALREADY carry a fingerprint, as parallel
 * (track id, 16-hex hash) arrays — the "on phone?" presence probe asks the
 * phone which of these it has. Returns entries filled (<= max). */
int mn_app_hashed_rows(mn_app *app, int64_t *ids, char (*hashes)[24],
                       int max);

/* ================================================================== */
/* Neural stem separation                                            */
/* ================================================================== */

/* Enable/disable real-time stem separation for the current track. When
 * enabled the engine routes decoded audio through the ONNX model and
 * mixes the per-stem outputs (subject to gain/mute/solo). */
void mn_app_stems_enable(mn_app *app, bool enabled);

/* When true, bypass the stem mixer and play the original stream while
 * inference keeps running (for A/B comparison). */
void mn_app_stems_passthrough(mn_app *app, bool enabled);

/* Set a stem's mix gain, 0.0 .. 1.0+ (index 0..MN_STEM_COUNT-1). */
void mn_app_stem_gain(mn_app *app, int32_t stem, float gain);

/* Mute/unmute a single stem. */
void mn_app_stem_mute(mn_app *app, int32_t stem, bool muted);

/* Solo/unsolo a single stem. Soloing any stem silences the others until
 * all solos are cleared. */
void mn_app_stem_solo(mn_app *app, int32_t stem, bool soloed);

/* ================================================================== */
/* Per-frame polling                                                 */
/* ================================================================== */

/* Fill *out with the current now-playing snapshot. */
void mn_app_now(mn_app *app, mn_now *out);

/* ---- Online session (internet radio / podcasts) ------------------- */
/* Start playing an HTTP(S) stream or a downloaded episode file OUTSIDE
 * the library queue. `kind` = "radio" | "podcast" | "stream"; radio gets
 * ICY metadata. `local` plays `src` as a file path. Blocks on connect —
 * worker thread only. On failure `err` (optional) has a short reason. */
bool mn_app_online_play(mn_app *app, const char *src, const char *title,
                        const char *artist, const char *kind,
                        const char *art_url, int64_t duration_ms, bool local,
                        char *err, size_t err_cap);
void mn_app_online_stop(mn_app *app);
bool mn_app_online_active(mn_app *app);

/* Like mn_app_now, but skips resolving out->art_path (left empty) and the
 * per-call fopen/stat it costs. For hot-path callers that never read the art
 * thumbnail (e.g. the 10 Hz taskbar progress tick). */
void mn_app_now_lite(mn_app *app, mn_now *out);

/* Copy the absolute filesystem path of the currently active track into `out`
 * (NUL-terminated, bounded by `n`). Returns true if a track is active and a
 * non-empty path was written; false otherwise (out[0] set to '\0' when n>0). */
bool mn_app_current_path(mn_app *app, char *out, size_t n);

/* Pump the controller once per UI frame: advances the queue on track
 * end, drains cross-thread events (scan/inference results), updates
 * meters and refreshes cached counts. Cheap and non-blocking. */
void mn_app_tick(mn_app *app);

/* ================================================================== */
/* Album art                                                         */
/* ================================================================== */

/* Return the cached album-art file path for (artist, album), or NULL if
 * none is available. The returned pointer is owned by the controller and
 * remains valid until the next mn_app_tick(); copy it if you need to
 * hold it. Missing art may be fetched/cached asynchronously, so a later
 * call for the same key may succeed. */
const char *mn_app_art_path(mn_app *app, const char *artist, const char *album);

/* Kind-agnostic album-identity window (art-integrity verifier): every album
 * of every kind in label order, each with the GRID's own (artist, title)
 * derivation (first track in album order, album_artist-or-artist). Only
 * id/title/artist/track_count are filled. Returns rows written. */
int32_t mn_app_album_ident_all(mn_app *app, int64_t offset, int32_t count,
                               mn_album *out);
/* Total albums library-wide (all kinds); pairs with the window above. */
int64_t mn_app_album_count_all(mn_app *app);

/* KIND-SCOPED album-identity window ("" = the music library): derives each
 * album's (artist, title) exactly like the album grid does when `kind` is
 * the active library — the kind filter changes which track is FIRST, so a
 * kind view can request art-key variants the kind-agnostic enumeration
 * above never sees. Never touches the live active_kind. */
int32_t mn_app_album_ident_kind(mn_app *app, const char *kind,
                                int64_t offset, int32_t count, mn_album *out);
/* Album count under `kind`'s scoping; pairs with the window above. */
int64_t mn_app_album_count_kind(mn_app *app, const char *kind);
/* Distinct non-music kinds registered (deduped, case-insensitive). Music is
 * kind "" and is not emitted. Returns kinds written into out[][32]. */
int32_t mn_app_kind_list(mn_app *app, char out[][32], int32_t max);

/* Thread-safe CHECK-ONLY art lookup into a CALLER-OWNED buffer: true iff a
 * cached thumbnail exists for "<artist>\x1f<album>" right now, copying its
 * path into out. Unlike mn_app_art_path (which returns a shared scratch
 * pointer under the app lock), this touches no shared state, so it is safe
 * from any thread concurrently — the serving path of the one-store art
 * architecture (art_url_for stats this file and emits its URL, or nothing). */
bool mn_app_art_check(mn_app *app, const char *artist, const char *album,
                      char *out, size_t n);

/* Targeted single-album EXTRACTION: resolve a representative audio-file path
 * for (artist, album) from the library (FTS on the album title + exact
 * album/artist match, same logic as mn_app_hires_cover) and ensure the grid
 * thumbnail exists under the "<artist>\x1f<album>" key. The DB lookup runs
 * under the app lock; the heavy decode/extract/encode runs lock-free. On
 * success out receives the thumbnail path and *newly (when non-NULL) reports
 * whether the thumb was created by THIS call (false = it already existed).
 * Returns false when no track resolves or no cover art is extractable; in
 * that case *src_seen (when non-NULL) reports whether a potential art source
 * (embedded picture or sidecar image) EXISTED but failed to decode — callers
 * must NOT persist a NONE verdict for those (transient/decoder gap, re-probed
 * by the heal tick), only for genuinely artless albums (src_seen=false).
 * This is the one backend primitive of the async art-heal path. */
bool mn_app_art_extract_one(mn_app *app, const char *artist, const char *album,
                            bool *newly, char *out, size_t n, bool *src_seen);

/* Ensure a HIGH-RESOLUTION cover PNG (long edge <= MN_ART_HIRES_MAX, aspect
 * preserved, "<hash>.hires.png" in the art cache) exists for (artist, album)
 * and copy its path into `out` (capacity `n`). Resolves the album's first
 * track path from the library to extract the full-resolution cover, then
 * delegates to mn_art_ensure_hires. Returns true on success (out holds a valid
 * path), false if no track/cover is available or on any error. Thread-safe:
 * the DB lookup runs under the app lock, the heavy extract/encode lock-free.
 * Used by the depth-map worker (crisp displacement) and the volumetric mesh
 * texture. If `ensure` is false the call is check-only (no extraction). */
bool mn_app_hires_cover(mn_app *app, const char *artist, const char *album,
                        bool ensure, char *out, size_t n);

/* Resolve a track's album-art key (album_artist-or-artist, album) so a caller
 * can invalidate exactly that album's cached webart. False if id unknown / no
 * album. Thread-safe. */
bool mn_app_track_art_key(mn_app *app, int64_t id,
                          char *artist_out, size_t artist_n,
                          char *album_out, size_t album_n);

/* Persisted AI-model selection ("<data_dir>/ai-models/selected.txt").
 * mn_app_get_selected_model copies the chosen filename for `kind`
 * ("stems"|"depth") into out (bundled default if none persisted);
 * mn_app_set_selected_model persists a new choice. Both return false on
 * invalid args. The stems selection is read at startup to build model_path;
 * changing it needs a restart. The depth selection is read live by the depth
 * worker. */
bool mn_app_get_selected_model(mn_app *app, const char *kind,
                               char *out, size_t n);
bool mn_app_set_selected_model(mn_app *app, const char *kind,
                               const char *filename);

/* Progress/result callback for mn_app_refresh_art(). Invoked once per album on
 * the calling (worker) thread. `artist`/`album` identify the album; `thumb`
 * is the freshly-ensured art-cache thumbnail path (non-empty when art was
 * found/generated, empty when the album has no embedded or sidecar art);
 * `newly` is true when THIS pass created the thumbnail (drives the host's
 * targeted {"type":"artready"} repaint emits — pre-existing thumbs must not
 * spam the bridge); `src_seen` is meaningful when `thumb` is empty: true
 * means a potential art source EXISTED but failed to decode (do NOT persist
 * a NONE verdict — transient/decoder gap), false means genuinely artless;
 * `done`/`total` drive a progress bar. */
typedef void (*mn_app_art_cb)(void *user,
                              const char *artist, const char *album,
                              const char *thumb, bool newly, bool src_seen,
                              int64_t done, int64_t total);

/*
 * Walk every album in the library and force-generate any missing album-art
 * thumbnail (embedded picture OR folder sidecar via the improved matcher),
 * caching it under the album key and updating the tracks.has_art flag so the
 * DB self-heals. For each album the callback (if non-NULL) receives the
 * ensured thumbnail path and running progress, letting the host mirror the
 * thumbnail into its webroot and emit {"type":"artscan",...}.
 *
 * When `skip_existing` is true, albums that already have a cached thumbnail are
 * reported (with their existing path) but not re-decoded — a cheap self-heal
 * pass. When false, every album is (re)generated.
 *
 * `limit` caps the number of albums processed (<=0 == all); the lightweight
 * post-launch self-heal passes a bound. Returns the number of albums that
 * gained a NEW thumbnail this pass. Blocking; intended to run on a worker
 * thread. Thread-safe against concurrent UI queries (per-op locking).
 */
int64_t mn_app_refresh_art(mn_app *app, bool skip_existing, int64_t limit,
                           mn_app_art_cb cb, void *user);

/* ================================================================== */
/* Metadata writing (tags / cover art / lyrics)                       */
/* ================================================================== */

/* Editable field set; defined in tags_write.h (no enum conflicts). */
struct mn_tag_edit;

/*
 * Write the textual tags of `edit` into the file of track `id`, then
 * upsert the new values into the library (keyed by path) and mark the
 * browse query dirty so the UI refreshes. If the target file is the
 * currently playing track, playback is detached (position remembered),
 * the file rewritten, and playback restored seamlessly. On failure a
 * short error token is written to `err` ("m4a-needs-repack", ...).
 */
bool mn_app_write_tags(mn_app *app, int64_t id, const struct mn_tag_edit *edit,
                       char *err, size_t errn);

/*
 * Embed `bytes[0..len)` (mime "image/jpeg" or "image/png") as the front
 * cover of track `id`'s file — or of EVERY file of its album when
 * `whole_album` is set. Afterwards the album's cached thumbnail is
 * regenerated from the new embedded art.
 */
bool mn_app_write_art(mn_app *app, int64_t id, const uint8_t *bytes,
                      size_t len, const char *mime, bool whole_album,
                      char *err, size_t errn);

/*
 * Embed `text` as the track's unsynchronized lyrics; when `synced_lrc` is
 * non-empty ALSO writes the "<path minus ext>.lrc" sidecar (UTF-8).
 * Returns true when at least one of the two destinations was written.
 */
bool mn_app_write_lyrics(mn_app *app, int64_t id, const char *text,
                         const char *synced_lrc);

/*
 * Read the track's lyrics into `out` (UTF-8, NUL-terminated, "" if none):
 * embedded (USLT / LYRICS / ©lyr) first, then the .lrc/.txt sidecar.
 */
bool mn_app_read_lyrics(mn_app *app, int64_t id, char *out, size_t n);

/* ================================================================== */
/* Settings                                                          */
/* ================================================================== */

/* Read the current settings into *out. */
void mn_app_get_settings(mn_app *app, mn_settings *out);

/* Apply and persist settings. Changes take effect immediately (e.g.
 * switching exclusive mode restarts the output device). */
void mn_app_set_settings(mn_app *app, const mn_settings *settings);

/* ================================================================== */
/* Audio hardware capabilities                                        */
/* ================================================================== */

/* Defined in audio_engine.h; forward-declared here so this header stays
 * self-contained (callers that use the caps include audio_engine.h). */
struct mn_audio_caps;
struct mn_audio_device;

/* Fill *out with the default playback device's native capabilities
 * (name, max bit depth, mix/max sample rates, channels, native-rate
 * list, exclusive capability) by querying the audio engine. Returns
 * true when *out holds valid data. Pass-through to mn_engine_get_caps. */
bool mn_app_audio_caps(mn_app *app, struct mn_audio_caps *out);

/* Enumerate playback devices into out[0..max) and return the count.
 * Pass-through to mn_engine_list_devices under the app lock. */
int mn_app_list_devices(mn_app *app, struct mn_audio_device *out, int max);

/* Switch playback output to device `index` (per the enumeration order of
 * mn_app_list_devices), preserving the loaded track and position. Returns
 * true on success. Pass-through to mn_engine_select_device under the app
 * lock. */
bool mn_app_select_device(mn_app *app, int index);

/* Enumeration index of the explicitly selected output device, or -1 when on
 * the system default endpoint. */
int mn_app_selected_device(mn_app *app);

/* A queued track's display fields for the "Up Next" list. */
typedef struct mn_queue_item {
    int64_t id;
    char    title[MN_STR_SHORT];
    char    artist[MN_STR_SHORT];
    char    album[MN_STR_SHORT];
    char    album_artist[MN_STR_SHORT];
    char    format[MN_STR_SHORT];
    int32_t duration_ms;
    int32_t bitrate_kbps;
    int32_t sample_rate;
    int32_t bit_depth;
    int32_t liked;              /* -1 / 0 / 1 */
    int64_t play_count;
} mn_queue_item;

/* Fill out with the UPCOMING queued tracks (those after the current one), up to
 * `max`. Returns the count written. 0 if nothing queued. Also reports the
 * current playing index via *out_current (SIZE-safe int; -1 if not playing). */
int mn_app_queue(mn_app *app, mn_queue_item *out, int max, int *out_current);

/* ================================================================== */
/* Library sync (phone <-> desktop, SYNC_PROTOCOL v1)                 */
/* ================================================================== */

/*
 * Progress callback for mn_app_sync_run, invoked on the calling (worker)
 * thread per state change. `state` is one of
 *     "connecting" | "pulling" | "merging" | "pushing" | "done" | "error"
 * applied/skipped are the LOCAL merge counts, pushed is what the phone
 * reported applying from our snapshot; by_hash/by_id split the matched
 * remote records by match path (content fingerprint vs tag identity);
 * `error` is "" except on "error".
 */
typedef void (*mn_app_sync_cb)(void *user, const char *state,
                               int applied, int skipped, int pushed,
                               int by_hash, int by_id,
                               const char *error);

/* Same struct sync.h defines (benign identical forward declaration — this
 * header stays free of the sync module's full contract). */
typedef struct mn_sync_counts mn_sync_counts;

/* Full HTTP sync against the phone's server at host:port (ping -> pull ->
 * merge -> push). BLOCKING — worker thread only. Library access serializes
 * against the scanner/UI via the app lock; the browse query + album cache
 * are invalidated when the merge changed rows. `counts_out` (optional) is
 * zeroed, then receives the per-category tallies of what the local merge
 * changed (the "what got synced" summary); final once the flow reaches
 * "pushing"/"done". Returns true on a completed flow. */
bool mn_app_sync_run(mn_app *app, const char *host, int port,
                     mn_sync_counts *counts_out,
                     mn_app_sync_cb cb, void *user);

/* Export this library's snapshot JSON to `path_or_null` (NULL -> the default
 * <data_dir>/sync/nexgen_library_sync.json; the sync dir is created). The
 * path written is copied into `out_path` (optional, capacity out_n). */
bool mn_app_sync_export(mn_app *app, const char *path_or_null,
                        char *out_path, size_t out_n);

/* Read + merge a snapshot file from `path_or_null` (NULL -> the default
 * path above). `applied`/`skipped` (optional) receive the merge counts. */
bool mn_app_sync_import(mn_app *app, const char *path_or_null,
                        int *applied, int *skipped);

/* Queue mutation (Up-Next reorder / remove / clear). */
void mn_app_queue_move(mn_app *app, int from, int to);
void mn_app_queue_remove(mn_app *app, int index);
void mn_app_queue_clear(mn_app *app);

/* Jump playback to an absolute queue index (does NOT remove tracks — the queue
 * stays intact, like a playlist). Returns false on out-of-range/no queue. */
bool mn_app_play_queue_index(mn_app *app, int index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_APP_H */
