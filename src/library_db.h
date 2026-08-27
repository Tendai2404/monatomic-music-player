/*
 * library_db.h - SQLite-backed library index for Monatomic Music Player.
 *
 * A clean C API contract for indexing and querying EXTREMELY LARGE music
 * libraries (target: 1,000,000+ tracks). Everything here is designed to avoid
 * O(n) scans on the UI thread: queries are windowed (offset/limit), counts are
 * cheap, facets are computed by the database, and all string results are handed
 * back as plain `const char*` backed by a per-result arena so the caller never
 * frees individual fields.
 *
 * Threading model:
 *   - A single mn_library handle wraps one SQLite database.
 *   - The WRITER is serialized: all mutations funnel through a mutex-guarded
 *     write connection. Use begin/commit/rollback to batch large imports.
 *   - READERS are per-thread: each calling thread lazily gets its own read-only
 *     SQLite connection (WAL mode) so reads never block writes and vice versa.
 *   - Query/facet handles are NOT themselves thread-safe; do not share a single
 *     mn_query across threads. Each thread opens its own.
 *
 * Ownership:
 *   - Row structs returned from window() point into an arena owned by the
 *     mn_query (or mn_facet) that produced them. They remain valid until the
 *     next window() call on that same handle, or until the handle is closed.
 *   - The caller never frees individual const char* fields.
 *
 * All functions return mn_status (MN_OK == 0) unless otherwise noted.
 */

#ifndef MN_LIBRARY_DB_H
#define MN_LIBRARY_DB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

typedef enum mn_status {
    MN_OK = 0,            /* Success.                                        */
    MN_ERR_GENERIC = 1,   /* Unspecified failure.                           */
    MN_ERR_NOMEM = 2,     /* Allocation failure.                            */
    MN_ERR_INVALID = 3,   /* Invalid argument / misuse.                     */
    MN_ERR_NOTFOUND = 4,  /* Requested row/entity does not exist.           */
    MN_ERR_BUSY = 5,      /* Database locked/busy after retries.            */
    MN_ERR_IO = 6,        /* Disk / filesystem error.                       */
    MN_ERR_CORRUPT = 7,   /* Database file is malformed.                    */
    MN_ERR_CONSTRAINT = 8,/* Constraint violation (e.g. UNIQUE).            */
    MN_ERR_MIGRATE = 9,   /* Schema migration failed / unsupported version. */
    MN_ERR_RANGE = 10,    /* Window / offset out of range.                  */
    MN_ERR_STATE = 11     /* Operation not valid in current state.          */
} mn_status;

/* Current on-disk schema version written by mn_library_open() after migrate.
 * v2: tracks.liked (thumbs up/down: -1 dislike, 0 none, 1 like).
 * v3: backfill tracks.format from the path extension (rows indexed before
 *     the scanner learned to derive the label).
 * v4: FTS index rebuilt with prefix indexes (prefix='2 3').
 * v5: tracks.pref_updated_ms — epoch MILLISECONDS of the last LOCAL
 *     like/dislike/rating change; drives last-write-wins library sync
 *     (SYNC_PROTOCOL v1 §3). Never bumped by play/skip updates.
 * v6: tracks.created — filesystem creation (birth) time sort axis.
 * v7: tracks.content_hash — IMMUTABLE 64-bit content fingerprint (hex
 *     string) of the file's raw bytes: fnv1a-64 over (file size ||
 *     first 64 KiB || last 64 KiB). Location- and metadata-independent:
 *     a move/rename/retag NEVER changes it; identical for the same file
 *     copied to another device, so playback progress keyed on it
 *     survives moves and is portable for device sync. WRITE-ONCE:
 *     computed when NULL (background backfill / scan), recomputed ONLY
 *     if the file SIZE changes (a genuinely different file). NULL =
 *     not yet computed; every consumer must treat NULL as "fall back
 *     to id/path" so the column is purely additive.
 * v8: tracks.votes_updated_ms — epoch MILLISECONDS of the last LOCAL
 *     like/dislike CHANGE (not ratings, not plays). 0 = never voted
 *     locally. The vote-specific LWW clock the phone's merge requires
 *     before it lets a peer CLEAR a vote: `updatedAt` moves on any
 *     field change, so it cannot prove "this peer un-liked the track"
 *     — votes_updated_ms can (HANDOFF-MONATOMIC.md in the Android
 *     repo). Migration backfills every already-voted row so no vote
 *     sits at stamp 0 (a 0 stamp would lose to ANY peer stamp). */
#define MN_SCHEMA_VERSION 8

/* Sentinel row id meaning "no row". */
#define MN_INVALID_ID ((int64_t)0)

/* ------------------------------------------------------------------------- */
/* Opaque handles                                                            */
/* ------------------------------------------------------------------------- */

typedef struct mn_library mn_library;  /* Mutex-guarded DB handle.          */
typedef struct mn_query   mn_query;    /* One windowed query cursor.        */
typedef struct mn_facet   mn_facet;    /* One windowed facet cursor.        */

/* ------------------------------------------------------------------------- */
/* Arena bump allocator                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A trivial bump allocator. Result strings/structs from queries live in an
 * arena that is reset (not freed) between windows, so a large scroll produces
 * zero per-row malloc churn. Exposed here so callers may allocate scratch
 * from the same arena a query hands back, and so tests can drive it directly.
 */
typedef struct mn_arena {
    uint8_t *base;    /* Start of the current block.                        */
    size_t   used;    /* Bytes consumed in the current block.               */
    size_t   cap;     /* Capacity of the current block.                     */
    struct mn_arena_block *blocks; /* Linked list of spilled blocks (impl). */
} mn_arena;

/* Initialize an arena with an initial reservation (may be 0). */
mn_status mn_arena_init(mn_arena *a, size_t initial_cap);

/* Allocate `size` bytes aligned to `align` (power of two). NULL on OOM. */
void *mn_arena_alloc(mn_arena *a, size_t size, size_t align);

/* Duplicate a NUL-terminated string into the arena. NULL only on OOM.
 * A NULL input yields a pointer to an empty string ("") in the arena. */
const char *mn_arena_strdup(mn_arena *a, const char *s);

/* Reset the arena to empty, retaining allocated blocks for reuse. */
void mn_arena_reset(mn_arena *a);

/* Release all memory owned by the arena. */
void mn_arena_free(mn_arena *a);

/* ------------------------------------------------------------------------- */
/* Open / close / migrate                                                    */
/* ------------------------------------------------------------------------- */

typedef struct mn_open_opts {
    bool     read_only;        /* Open without a writer (queries only).      */
    bool     create_if_missing;/* Create the DB file/schema if absent.       */
    int      busy_timeout_ms;  /* SQLite busy timeout (default 5000).        */
    int64_t  mmap_size;        /* PRAGMA mmap_size (0 = SQLite default).     */
    int      cache_size_kib;   /* PRAGMA cache_size in KiB (0 = default).    */
} mn_open_opts;

/*
 * Open (and, if needed, create + migrate) the library database at `path`.
 * On success `*out` receives a new handle. Configures WAL journaling,
 * foreign keys, and the FTS5 index. `opts` may be NULL for defaults.
 */
mn_status mn_library_open(const char *path, const mn_open_opts *opts,
                          mn_library **out);

/* Flush, checkpoint WAL, close all reader/writer connections, free handle. */
void mn_library_close(mn_library *lib);

/* Online backup of the live database to dest_path (SQLite Backup API from a
 * reader connection — safe concurrently with scans/writes; run it on a
 * background thread). Overwrites dest_path. */
mn_status mn_library_backup(mn_library *lib, const char *dest_path);

/* Aggregate stats for every indexed file under directory `prefix`:
 * track count, distinct albums, total bytes, newest date_added. Range scan
 * on the unique path index; reader connection; any thread. */
mn_status mn_library_prefix_stats(mn_library *lib, const char *prefix,
                                  int64_t *tracks, int64_t *albums,
                                  int64_t *bytes, int64_t *newest_added);

/* Close + unregister the calling thread's lazily-created reader connection.
 * Call before a short-lived worker thread exits; otherwise its connection
 * lives until mn_library_close. Safe to call with no connection open. */
void mn_library_thread_detach(mn_library *lib);

/* Return the on-disk schema version currently present (post-migration). */
int mn_library_schema_version(const mn_library *lib);

/*
 * Run any pending schema migrations up to MN_SCHEMA_VERSION. Called
 * automatically by open(); exposed for explicit control. Serialized.
 */
mn_status mn_library_migrate(mn_library *lib);

/* Human-readable message for the last error on this handle (never NULL). */
const char *mn_library_errmsg(const mn_library *lib);

/* Run PRAGMA optimize / ANALYZE to refresh planner stats. Serialized. */
mn_status mn_library_analyze(mn_library *lib);

/* Force a WAL checkpoint (TRUNCATE). Serialized. */
mn_status mn_library_checkpoint(mn_library *lib);

/* ------------------------------------------------------------------------- */
/* Batch transaction control (serialized writer)                             */
/* ------------------------------------------------------------------------- */

/*
 * Begin/commit/rollback an explicit write transaction on the serialized
 * writer connection. Wrap bulk imports (e.g. a folder scan) in a single
 * transaction for orders-of-magnitude throughput. These take/release the
 * writer mutex for the duration; keep transactions bounded. Nesting is not
 * supported (returns MN_ERR_STATE if a transaction is already open).
 */
mn_status mn_library_begin(mn_library *lib);
mn_status mn_library_commit(mn_library *lib);
mn_status mn_library_rollback(mn_library *lib);

/* True if a write transaction is currently open. */
bool mn_library_in_transaction(const mn_library *lib);

/* ------------------------------------------------------------------------- */
/* Track upsert / maintenance (serialized writer)                            */
/* ------------------------------------------------------------------------- */

/*
 * Denormalized track record for upsert. String fields are borrowed for the
 * duration of the call only (copied into the DB); the caller retains
 * ownership. NULL strings are stored as NULL/empty. `path` is the unique key.
 */
typedef struct mn_track_in {
    /* Text fields (const char*, borrowed; NULL allowed). */
    const char *path;          /* Absolute path. UNIQUE, required.           */
    const char *title;
    const char *artist;
    const char *album;
    const char *album_artist;
    const char *composer;
    const char *genre;
    const char *format;        /* e.g. "flac", "mp3", "wav", "ogg".          */

    /* Integer fields (0 = unknown/unset unless noted). */
    int32_t  year;
    int32_t  track;            /* Track number within disc.                  */
    int32_t  disc;             /* Disc number.                               */
    int64_t  duration_ms;
    int32_t  sample_rate;      /* Hz.                                         */
    int32_t  channels;
    int32_t  bit_depth;        /* Bits per sample (0 for lossy).             */
    int32_t  bitrate_kbps;
    int64_t  size;             /* File size in bytes.                        */
    int64_t  mtime;            /* File modification time (unix seconds).     */
    int64_t  created;          /* File creation/birth time (unix seconds;
                                  0 when the filesystem can't provide it).   */

    bool     has_art;          /* Embedded/available cover art present.      */
} mn_track_in;

/*
 * Insert or update a track keyed by `path`. On success `*out_id` (if non-NULL)
 * receives the track's row id. Preserves user-owned columns (rating, play/skip
 * counts, date_added) across updates; refreshes the FTS index. Serialized.
 */
mn_status mn_library_upsert_track(mn_library *lib, const mn_track_in *t,
                                  int64_t *out_id);

/* Moved-file relink, part 1: rows whose (size, duration ±2 s) identity
 * matches a NEW path's file — the app stat()s each candidate's path and
 * repaths the single one whose file is gone. Returns candidate count
 * (0 when `new_path` is already known). Serialized. */
typedef struct mn_relink_cand {
    int64_t id;
    char    path[1024];
} mn_relink_cand;
int mn_library_relink_candidates(mn_library *lib, const char *new_path,
                                 int64_t size, int64_t duration_ms,
                                 mn_relink_cand *out, int max);

/* Moved-file relink, part 2: point an existing row at its file's new
 * location (missing cleared; rating/playcount/liked/date_added keep).
 * The caller's next upsert on `new_path` refreshes the tag columns. */
mn_status mn_library_repath_track(mn_library *lib, int64_t track_id,
                                  const char *new_path);

/* Backfill the filesystem creation time (rows from before schema v6). */
mn_status mn_library_set_created(mn_library *lib, int64_t track_id,
                                 int64_t created);

/* ------------------------------------------------------------------ */
/* content_hash (schema v7): immutable content fingerprint.           */
/* ------------------------------------------------------------------ */

/* Set a row's content fingerprint. WRITE-ONCE by default: the UPDATE
 * refuses to touch a row whose hash is already set (immutability is
 * enforced in the SQL itself). force=true is the ONLY way to replace a
 * hash and is reserved for the file-content-actually-changed case (the
 * caller detected a SIZE change at the same path). */
mn_status mn_library_set_content_hash(mn_library *lib, int64_t track_id,
                                      const char *hash, bool force);

/* Fill up to `max` (id, path, size) rows still needing a fingerprint
 * (content_hash NULL, not missing). Feed for the low-priority background
 * backfill worker. Returns the row count. */
int mn_library_hashless_rows(mn_library *lib, int64_t *ids,
                             char (*paths)[1024], int64_t *sizes, int max);

/* Find the (lowest-id, non-missing) track carrying this fingerprint —
 * how playback progress re-attaches after a file move/rename. 0 = none. */
int64_t mn_library_track_by_hash(mn_library *lib, const char *hash);

/* Direct-by-id essentials for the out-of-view play fallback. */
int64_t mn_library_track_album_id(mn_library *lib, int64_t track_id);
bool mn_library_track_path_duration(mn_library *lib, int64_t track_id,
                                    char *path_out, size_t path_n,
                                    int64_t *dur_ms_out);

/* ------------------------------------------------------------------ */
/* book_progress — audiobook resume/progress (replaces book_resume.txt)*/
/* ------------------------------------------------------------------ */

/* One Continue-Listening shelf entry: a recently-touched book's current
 * chapter + progress, with enough metadata to render without another
 * round-trip. Owned buffers (snapshot; no borrowed statement memory). */
typedef struct mn_book_recent {
    int64_t album_id;
    int64_t track_id;      /* the CURRENT chapter                      */
    int64_t pos_ms;        /* position within that chapter             */
    double  percent;       /* whole-book completion 0..1               */
    bool    finished;
    int64_t updated;       /* unix seconds of the last progress note   */
    int64_t duration_ms;   /* current chapter's duration               */
    char    album[256];    /* book title (album tag)                   */
    char    album_artist[256];
    char    artist[256];
    char    title[256];    /* current chapter's title                  */
} mn_book_recent;

/* Upsert a progress note for (album, chapter); marks it the book's current
 * row (demoting others) so every chapter keeps its own position. Computes
 * whole-book percent internally (chapter durations before + pos / total)
 * and snapshots the track's content_hash. updated = unix seconds. */
mn_status mn_library_book_note(mn_library *lib, int64_t album_id,
                               int64_t track_id, int64_t pos_ms,
                               int64_t updated);

/* The book's current chapter + position (false/zeros when untouched). */
bool mn_library_book_get(mn_library *lib, int64_t album_id,
                         int64_t *out_track, int64_t *out_pos,
                         double *out_percent, bool *out_finished);

/* A specific chapter's remembered position (0 = never played). */
int64_t mn_library_chapter_pos(mn_library *lib, int64_t album_id,
                               int64_t track_id);

/* Most-recently-touched books, newest first — the Continue shelf feed. */
int mn_library_recent_books(mn_library *lib, mn_book_recent *out, int max);

/* Compact (album_id, percent, finished) feed for grid-tile badges. */
int mn_library_book_badges(mn_library *lib, int64_t *album_ids,
                           double *percents, bool *finisheds, int max);

/* All remembered chapter positions within one book. */
int mn_library_book_chapters(mn_library *lib, int64_t album_id,
                             int64_t *track_ids, int64_t *pos_ms, int max);

/* Bookmarks: named positions within a book (sync/move-proof via the
 * content_hash snapshot). add returns the new id (0 on failure). */
int64_t mn_library_bookmark_add(mn_library *lib, int64_t album_id,
                                int64_t track_id, int64_t pos_ms,
                                const char *note, int64_t created);
mn_status mn_library_bookmark_del(mn_library *lib, int64_t bookmark_id);
int mn_library_bookmark_list(mn_library *lib, int64_t album_id,
                             int64_t *ids, int64_t *track_ids,
                             int64_t *pos_ms, char (*notes)[128],
                             int64_t *created, int max);

/* Mark a single track as missing (file no longer on disk). Serialized. */
mn_status mn_library_mark_missing(mn_library *lib, int64_t track_id,
                                  bool missing);

/*
 * Bulk-mark tracks missing: any track whose path begins with `root_prefix`
 * and whose `mtime` was not touched since `scan_epoch` is flagged missing.
 * Use after a full rescan to reap deletions. Returns count via `out_marked`.
 * Serialized.
 */
mn_status mn_library_reap_missing(mn_library *lib, const char *root_prefix,
                                  int64_t scan_epoch, int64_t *out_marked);

/* Permanently delete a track and its FTS/playlist references. Serialized. */
mn_status mn_library_delete_track(mn_library *lib, int64_t track_id);

/*
 * Look up a track by exact path on the writer connection (sees rows upserted
 * inside an open scan transaction). Fills any non-NULL out params. Returns
 * MN_OK when found, MN_ERR_NOTFOUND otherwise. Serialized; cheap (indexed).
 */
mn_status mn_library_lookup_path(mn_library *lib, const char *path,
                                 int64_t *out_id, int64_t *out_mtime,
                                 int64_t *out_size, bool *out_missing);

/*
 * Bulk-enumerate every indexed track's (path, mtime, size, missing) via one
 * streaming query under a single lock. The scanner builds an in-memory index
 * from this ONCE per scan, then answers is_known() from RAM instead of a
 * per-file locked point query — the difference between one lookup and 100k
 * on a large-library rescan. cb is invoked for each row; do not call other
 * library functions from inside it.
 */
typedef void (*mn_library_path_cb)(void *user, const char *path,
                                   int64_t mtime, int64_t size, bool missing);
mn_status mn_library_enumerate_paths(mn_library *lib,
                                     mn_library_path_cb cb, void *user);

/*
 * Wipe the ENTIRE library: all tracks (FTS kept consistent via triggers),
 * playlists + items, and every dimension table (artists/album_artists/
 * albums/genres/folders). User must not have a batch transaction open
 * (returns MN_ERR_STATE). One transaction; serialized.
 */
mn_status mn_library_reset(mn_library *lib);

/* ------------------------------------------------------------------------- */
/* User columns: rating / play / skip                                        */
/* ------------------------------------------------------------------------- */

/*
 * Set rating in half-star units: 0..10 (stars_x2), where 10 == 5 stars,
 * 0 == unrated. Values are clamped to [0,10]. Serialized.
 */
mn_status mn_library_set_rating(mn_library *lib, int64_t track_id,
                                int32_t stars_x2);

/* Increment play count and set last_played to `when` (unix s). Serialized. */
mn_status mn_library_bump_play(mn_library *lib, int64_t track_id, int64_t when);

/* Increment skip count and set last_skipped to `when`. Serialized. */
mn_status mn_library_bump_skip(mn_library *lib, int64_t track_id, int64_t when);

/* Set the has_art flag (0/1) for one track. Used by the art-refresh pass so the
 * DB reflects freshly-discovered sidecar/embedded covers. Serialized. */
mn_status mn_library_set_has_art(mn_library *lib, int64_t track_id, bool has_art);

/* Set the thumbs state: -1 dislike, 0 neutral, 1 like (clamped). Serialized. */
mn_status mn_library_set_liked(mn_library *lib, int64_t track_id, int32_t liked);

/* Resolve a track id by absolute path (case-insensitive). MN_OK + *out_id on
 * hit; MN_ERR_NOTFOUND on miss. Read-only. */
mn_status mn_library_track_id_by_path(mn_library *lib, const char *path,
                                      int64_t *out_id);

/* Read the thumbs state of one track into *out_liked (0 when not found). */
mn_status mn_library_get_liked(mn_library *lib, int64_t track_id,
                               int32_t *out_liked);

/*
 * Permanently delete EVERY track of one folder (playlist references and the
 * FTS index stay consistent; dimension track_counts are decremented) plus the
 * folder row itself. `out_deleted` (optional) receives the number of tracks
 * removed. One transaction; serialized.
 */
mn_status mn_library_delete_folder(mn_library *lib, int64_t folder_id,
                                   int64_t *out_deleted);

/* Fill out[] with folder_id plus every folder under its path (subtree).
 * Returns the TOTAL found (return > max signals truncation). */
int32_t mn_library_folder_subtree(mn_library *lib, int64_t folder_id,
                                  int64_t *out, int32_t max);

/* ------------------------------------------------------------------------- */
/* Library sync (SYNC_PROTOCOL v1: likes / ratings / play stats)             */
/* ------------------------------------------------------------------------- */

/*
 * Apply an already-MERGED set of per-track metrics in one UPDATE (used by the
 * sync merge in sync.c — the last-write-wins decision happens there, this is
 * the write). Mapping onto the schema:
 *   liked/disliked -> tracks.liked (1 like, -1 dislike, 0 none; liked wins
 *                     when both are set, per SYNC_PROTOCOL §7),
 *   stars (0..5)   -> tracks.rating_x2 (= stars*2),
 *   play_count     -> tracks.play_count (set directly; caller max-merged),
 *   last_played_ms -> tracks.last_played (converted ms -> unix SECONDS),
 *   updated_ms     -> tracks.pref_updated_ms,
 *   votes_updated_ms -> tracks.votes_updated_ms (v8: the merged votes
 *                     clock — the caller resolves it, this just writes).
 * Serialized.
 */
mn_status mn_library_sync_apply(mn_library *lib, int64_t track_id,
                                int liked, int disliked, int stars,
                                int64_t play_count, int64_t last_played_ms,
                                int64_t updated_ms, int64_t votes_updated_ms);

/*
 * Bulk-enumerate the sync-relevant columns of tracks via one streaming query
 * under a single lock (modeled on mn_library_enumerate_paths). `all` = true
 * streams EVERY track (the merge builds its identity map from this); false
 * streams only tracks carrying at least one NON-DEFAULT metric (liked != 0 OR
 * rating_x2 > 0 OR play_count > 0 OR last_played > 0 — the snapshot rule of
 * SYNC_PROTOCOL §2). `last_played` is in unix SECONDS (the stored unit);
 * `pref_updated_ms` / `votes_updated_ms` in epoch milliseconds. `content_hash`
 * is the row's immutable content fingerprint (schema v7) or NULL when not yet
 * computed — the NULL/non-NULL distinction is meaningful, do not map NULL
 * to "". cb is invoked per row; do not call other library functions from
 * inside it, and copy any strings you keep.
 */
typedef void (*mn_library_sync_cb)(void *user, int64_t track_id,
                                   const char *artist, const char *title,
                                   const char *album, int64_t duration_ms,
                                   int32_t liked, int32_t rating_x2,
                                   int64_t play_count, int64_t last_played,
                                   int64_t pref_updated_ms,
                                   int64_t votes_updated_ms,
                                   const char *content_hash);
mn_status mn_library_sync_enumerate(mn_library *lib, bool all,
                                    mn_library_sync_cb cb, void *user);

/* ------------------------------------------------------------------------- */
/* Audiobook progress sync (additive v1 "books" snapshot section)            */
/* ------------------------------------------------------------------------- */

/*
 * Bulk-enumerate every book_progress row joined with its chapter's tags via
 * one streaming query under a single lock — the feed for the snapshot's
 * "books" section. `content_hash` is the TRACK's current fingerprint,
 * falling back to the hash snapshotted into the progress row (NULL when
 * neither exists). `updated` is unix SECONDS (the stored unit — the sync
 * layer converts to protocol milliseconds at the wire). Missing tracks are
 * still enumerated: progress on a detached drive is progress worth syncing.
 * cb rules are the same as mn_library_sync_cb's.
 */
typedef void (*mn_library_book_sync_cb)(void *user, int64_t album_id,
                                        int64_t track_id,
                                        const char *artist, const char *title,
                                        const char *album, int64_t duration_ms,
                                        const char *content_hash,
                                        int64_t pos_ms, double percent,
                                        bool finished, bool current,
                                        int64_t updated);
mn_status mn_library_book_sync_enumerate(mn_library *lib,
                                         mn_library_book_sync_cb cb,
                                         void *user);

/*
 * Apply ONE remote book-progress record to (album_id, track_id) with
 * last-write-wins resolved HERE (unlike mn_library_book_note, which
 * overwrites unconditionally): the write happens only when the row does not
 * exist yet or `updated_s` (unix seconds) is strictly newer than the stored
 * row's `updated`. `percent` is NOT taken from the wire — it is recomputed
 * from the LOCAL album's chapter durations (the peer's chapter split may
 * differ), exactly like mn_library_book_note. `finished` IS taken from the
 * wire when `has_finished` (the peer's latch decision: a book finished
 * there is finished here, and a rewind-cleared latch clears here too);
 * otherwise the local latch rule runs on the recomputed percent.
 * `want_current` marks the record as the peer's live resume chapter: the
 * row is promoted to current=1 (demoting the album's other rows) only when
 * `updated_s` also beats the album's newest current row — an older peer
 * note never steals the resume target. *out_changed (optional) reports
 * whether anything was written. Serialized.
 */
mn_status mn_library_book_sync_apply(mn_library *lib, int64_t album_id,
                                     int64_t track_id, int64_t pos_ms,
                                     bool has_finished, bool finished,
                                     bool want_current, int64_t updated_s,
                                     bool *out_changed);

/* ------------------------------------------------------------------------- */
/* Windowed query engine                                                     */
/* ------------------------------------------------------------------------- */

/* Sortable columns. */
typedef enum mn_sort_key {
    MN_SORT_NONE = 0,
    MN_SORT_TITLE,
    MN_SORT_ARTIST,
    MN_SORT_ALBUM,
    MN_SORT_ALBUM_ARTIST,
    MN_SORT_GENRE,
    MN_SORT_YEAR,
    MN_SORT_TRACK,          /* disc then track number.                       */
    MN_SORT_DURATION,
    MN_SORT_DATE_ADDED,
    MN_SORT_DATE_CREATED,   /* filesystem creation (birth) time.             */
    MN_SORT_LAST_PLAYED,
    MN_SORT_PLAY_COUNT,
    MN_SORT_RATING,
    MN_SORT_BITRATE,
    MN_SORT_PATH,
    MN_SORT_RELEVANCE       /* FTS rank; valid only when fts_match set.       */
} mn_sort_key;

typedef struct mn_sort_term {
    mn_sort_key key;
    bool        descending;
} mn_sort_term;

/* Maximum sort terms in a filter spec. */
#define MN_MAX_SORT 3

/*
 * Cascade filter: an ordered chain of facet selections (e.g. artist -> album).
 * Each entry restricts by a facet dimension and a chosen value id (as returned
 * by the facet engine). An empty chain means "no facet restriction".
 */
typedef enum mn_facet_dim {
    MN_FACET_NONE = 0,
    MN_FACET_ARTIST,
    MN_FACET_ALBUM_ARTIST,
    MN_FACET_ALBUM,
    MN_FACET_GENRE,
    MN_FACET_YEAR,
    MN_FACET_FOLDER          /* Filesystem folder (dirname of track path).    */
} mn_facet_dim;

typedef struct mn_facet_sel {
    mn_facet_dim dim;
    int64_t      value_id;   /* Facet value id (see mn_facet_row.value_id).   */
} mn_facet_sel;

/* Maximum cascade depth. */
#define MN_MAX_CASCADE 4

/* Maximum folder ids a filter spec can exclude (folder-visibility toggles). */
#define MN_MAX_EXCLUDED_FOLDERS 512

/* Maximum category-scoping roots (music vs audiobooks isolation). */
#define MN_MAX_KIND_ROOTS 32

/*
 * Filter specification for a windowed query. All fields optional.
 *   - fts_match: FTS5 MATCH expression (NULL = no full-text filter). If FTS5
 *     is unavailable the engine transparently falls back to LIKE across
 *     title/artist/album/album_artist.
 *   - cascade: ordered facet selections, terminated by a MN_FACET_NONE entry
 *     or by cascade_len.
 *   - sort: up to MN_MAX_SORT ordering terms, applied in order.
 *   - include_missing: if false (default), rows flagged missing are excluded.
 *   - excluded_folders: tracks whose folder_id is in this set are filtered
 *     out (folder-visibility toggles). Applied as an inline integer NOT IN
 *     list in the WHERE clause, so it composes with search/cascade/sort at
 *     zero extra bind cost. SKIPPED when the cascade explicitly selects a
 *     folder (MN_FACET_FOLDER), so browsing INTO a hidden folder still shows
 *     its contents.
 */
typedef struct mn_filter_spec {
    const char    *fts_match;
    mn_facet_sel   cascade[MN_MAX_CASCADE];
    int            cascade_len;
    mn_sort_term   sort[MN_MAX_SORT];
    int            sort_len;
    bool           include_missing;
    bool           liked_only;     /* true = only tracks with liked == 1.     */
    int32_t        min_rating_x2;  /* 0 = no minimum.                         */
    int64_t        excluded_folders[MN_MAX_EXCLUDED_FOLDERS];
    int            excluded_folders_len; /* 0 = no folder exclusion.          */
    /* Category scoping (music vs audiobooks). When kind_roots_len > 0:
     *   kind_include=true  -> ONLY tracks under one of these path roots;
     *   kind_include=false -> tracks under these roots are EXCLUDED.
     * Applied as indexed path prefix-ranges in the WHERE clause, so it
     * composes with search/cascade/facets on every browse surface. */
    bool           kind_include;
    int            kind_roots_len;       /* 0 = no category scoping.          */
    char           kind_roots[MN_MAX_KIND_ROOTS][512];
} mn_filter_spec;

/*
 * One track row projected for the UI. All const char* point into the parent
 * query's arena and are valid until the next window() on that query, or until
 * the query is closed. Strings are never NULL (empty string for unset).
 */
typedef struct mn_track_row {
    int64_t     id;
    int64_t     album_id;    /* albums-dimension id (0 if none) — the album
                              * facet's value_id, so consumers can group rows
                              * by ALBUM IDENTITY (names are NOT unique).    */
    const char *path;
    const char *title;
    const char *artist;
    const char *album;
    const char *album_artist;
    const char *composer;
    const char *genre;
    const char *format;
    int32_t     year;
    int32_t     track;
    int32_t     disc;
    int64_t     duration_ms;
    int32_t     sample_rate;
    int32_t     channels;
    int32_t     bit_depth;
    int32_t     bitrate_kbps;
    int64_t     size;
    int64_t     mtime;
    int64_t     created;     /* file creation/birth time (0 = unknown)       */
    int64_t     date_added;
    int64_t     last_played;
    int32_t     play_count;
    int32_t     skip_count;
    int32_t     rating_x2;
    int32_t     liked;       /* -1 dislike, 0 neutral, 1 like.               */
    bool        has_art;
    bool        missing;
} mn_track_row;

/*
 * Open a query cursor for `spec` on the calling thread's reader connection.
 * `*out` receives a new mn_query. The spec is copied; the caller may reuse it.
 * O(1) — no scan happens until count()/window() is called.
 */
mn_status mn_query_open(mn_library *lib, const mn_filter_spec *spec,
                        mn_query **out);

/* Close a query cursor and free its arena. Safe on NULL. */
void mn_query_close(mn_query *q);

/*
 * Total number of rows matching the query's filter, ignoring the window.
 * Cached after first call until the query is closed. `*out_count` receives it.
 */
mn_status mn_query_count(mn_query *q, int64_t *out_count);

/*
 * Fetch up to `n` rows starting at `offset`. On success `*out_rows` points to
 * an array of `*out_n` mn_track_row (out_n <= n), owned by the query's arena
 * and invalidated by the next window() call. `out_n` may be 0 at the tail.
 */
mn_status mn_query_window(mn_query *q, int64_t offset, int32_t n,
                          const mn_track_row **out_rows, int32_t *out_n);

/*
 * Fetch a single track by id into the query's arena (convenience). Returns
 * MN_ERR_NOTFOUND if absent. The row is valid until the next window()/fetch.
 */
mn_status mn_query_fetch_id(mn_query *q, int64_t track_id,
                            const mn_track_row **out_row);

/*
 * Locate the window offset of a given track id within the current filter
 * ordering (for "scroll to now-playing"). MN_ERR_NOTFOUND if not in results.
 */
mn_status mn_query_index_of(mn_query *q, int64_t track_id, int64_t *out_offset);

/* ------------------------------------------------------------------------- */
/* Facets (artist / album / genre / year) with counts + windowing            */
/* ------------------------------------------------------------------------- */

/*
 * One facet value row. `label` points into the facet's arena (valid until the
 * next facet window() or close). `value_id` feeds mn_facet_sel.value_id in a
 * cascade filter. `count` is the number of tracks under that value given the
 * facet's own filter/cascade context.
 */
typedef struct mn_facet_row {
    int64_t     value_id;   /* Stable id usable in a cascade selection.       */
    const char *label;      /* Display text (e.g. artist name, "2019").       */
    int64_t     count;      /* Track count under this value.                  */
} mn_facet_row;

/*
 * Open a facet cursor over dimension `dim`, restricted by `spec` (which may
 * carry its own fts_match + upstream cascade for cascading facets). Pass NULL
 * spec for an unfiltered facet over the whole library. Facet rows are sorted
 * by label ascending by default (see mn_facet_sort). O(1) until count/window.
 */
mn_status mn_facet_open(mn_library *lib, mn_facet_dim dim,
                        const mn_filter_spec *spec, mn_facet **out);

/* Close a facet cursor and free its arena. Safe on NULL. */
void mn_facet_close(mn_facet *f);

/* Sort ordering for facet values. */
typedef enum mn_facet_order {
    MN_FACET_ORDER_LABEL = 0,   /* Alphabetical / numeric by label.           */
    MN_FACET_ORDER_COUNT_DESC   /* Most-populated first.                      */
} mn_facet_order;

/* Set facet ordering (call before window()). */
mn_status mn_facet_sort(mn_facet *f, mn_facet_order order);

/* Total distinct facet values under the current filter. */
mn_status mn_facet_count(mn_facet *f, int64_t *out_count);

/*
 * Fetch up to `n` facet rows starting at `offset`. `*out_rows`/`*out_n` are
 * owned by the facet arena and invalidated by the next window() call.
 */
mn_status mn_facet_window(mn_facet *f, int64_t offset, int32_t n,
                          const mn_facet_row **out_rows, int32_t *out_n);

/* ------------------------------------------------------------------------- */
/* Playlists (CRUD)                                                          */
/* ------------------------------------------------------------------------- */

typedef struct mn_playlist_row {
    int64_t     id;
    const char *name;        /* Arena-owned.                                  */
    int64_t     track_count;
    int64_t     date_created;
    int64_t     date_modified;
} mn_playlist_row;

/* Create a playlist; `*out_id` receives its id. Serialized. */
mn_status mn_playlist_create(mn_library *lib, const char *name,
                             int64_t *out_id);

/* Rename a playlist. Serialized. */
mn_status mn_playlist_rename(mn_library *lib, int64_t playlist_id,
                             const char *name);

/* Delete a playlist and its membership rows. Serialized. */
mn_status mn_playlist_delete(mn_library *lib, int64_t playlist_id);

/*
 * Append a track at the end of a playlist. If `out_position` is non-NULL it
 * receives the assigned 0-based position. Serialized.
 */
mn_status mn_playlist_add(mn_library *lib, int64_t playlist_id,
                          int64_t track_id, int64_t *out_position);

/* Insert a track at an explicit position, shifting later items. Serialized. */
mn_status mn_playlist_insert_at(mn_library *lib, int64_t playlist_id,
                                int64_t track_id, int64_t position);

/* Remove the item at `position`, compacting the tail. Serialized. */
mn_status mn_playlist_remove_at(mn_library *lib, int64_t playlist_id,
                                int64_t position);

/* Move an item from `from_pos` to `to_pos` (reorder). Serialized. */
mn_status mn_playlist_move(mn_library *lib, int64_t playlist_id,
                           int64_t from_pos, int64_t to_pos);

/* Remove every item from a playlist. Serialized. */
mn_status mn_playlist_clear(mn_library *lib, int64_t playlist_id);

/*
 * List playlists into a caller-provided arena. `*out_rows`/`*out_n` are
 * valid until `arena` is reset/freed by the caller. Read-only.
 */
mn_status mn_playlist_list(mn_library *lib, mn_arena *arena,
                           const mn_playlist_row **out_rows, int32_t *out_n);

/*
 * Open a windowed query over a playlist's tracks IN PLAYLIST ORDER, further
 * constrained by `spec` (may be NULL). Rows come back as mn_track_row via the
 * normal mn_query_window path. `*out` receives a query cursor.
 */
mn_status mn_playlist_query(mn_library *lib, int64_t playlist_id,
                            const mn_filter_spec *spec, mn_query **out);

/* ------------------------------------------------------------------------- */
/* Library statistics                                                        */
/* ------------------------------------------------------------------------- */

typedef struct mn_stats {
    int64_t track_count;        /* Non-missing tracks.                        */
    int64_t missing_count;
    int64_t artist_count;       /* Distinct artists.                          */
    int64_t album_count;        /* Distinct albums.                           */
    int64_t genre_count;        /* Distinct genres.                           */
    int64_t playlist_count;
    int64_t total_duration_ms;  /* Sum over non-missing tracks.               */
    int64_t total_size_bytes;   /* Sum over non-missing tracks.               */
    int64_t total_play_count;
} mn_stats;

/* Compute aggregate library statistics. Read-only; may be O(n) — call off-UI. */
mn_status mn_library_stats(mn_library *lib, mn_stats *out);

/* Extended stats: per-format breakdown + hi-res count, one pass over tracks. */
#define MN_STATS_MAX_FORMATS 16

typedef struct mn_stats_fmt {
    char    fmt[24];            /* Upper-cased format label, e.g. "MP3".      */
    int64_t n;                  /* Non-missing tracks in that format.         */
} mn_stats_fmt;

typedef struct mn_stats_ext {
    int64_t track_count;        /* Non-missing tracks.                        */
    int64_t missing_count;
    int64_t artist_count;       /* Distinct artists (non-missing tracks).     */
    int64_t album_count;        /* Distinct albums (non-missing tracks).      */
    int64_t total_duration_ms;  /* Sum over non-missing tracks.               */
    int64_t total_size_bytes;   /* Sum over non-missing tracks.               */
    int64_t hires_count;        /* bit_depth>=24 OR sample_rate>=88200.       */
    mn_stats_fmt formats[MN_STATS_MAX_FORMATS]; /* Sorted by count desc.      */
    int     format_count;
} mn_stats_ext;

/* Aggregate + GROUP BY format over tracks. Read-only; call off the UI path. */
mn_status mn_library_stats_ext(mn_library *lib, mn_stats_ext *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_LIBRARY_DB_H */
