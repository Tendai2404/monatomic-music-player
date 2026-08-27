/*
 * sync.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Desktop side of the NEX-GEN library-sync protocol (SYNC_PROTOCOL.md v1):
 * per-track likes/dislikes, play counts, last-played times and audiobook
 * chapter positions (the additive "books" section) converge between this
 * library and the phone's, keyed by a portable TRACK IDENTITY derived from
 * tags (§1) — never by file path. Star ratings are no longer EXPORTED
 * (the desktop retired its star UI); incoming ratings are still applied to
 * the kept rating_x2 column. Votes carry their own LWW clock
 * (votesUpdatedAt, schema v8) so un-likes can propagate safely.
 *
 * This module is deliberately UI-free and app-free: it sees the library
 * only through an mn_sync_env (an mn_library* plus optional lock/unlock
 * hooks the caller supplies so library access serializes against the
 * scanner/UI), and reports progress through a plain callback. Transport is
 * plain-HTTP WinHTTP against the phone's LAN server (§4a) and/or snapshot
 * files on disk (§4b). All blocking — call from a worker thread.
 *
 * Naming: functions/types use the "mn_" prefix, macros use "MN_".
 */

#ifndef MN_SYNC_H
#define MN_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same opaque handle library_db.h declares (benign identical typedef, so
 * this header stays self-contained for callers that alias the db enums). */
typedef struct mn_library mn_library;

/* Protocol version implemented (snapshots with a higher one are refused).
 * NOTE: the optional per-item "hash" field (the schema-v7 content
 * fingerprint, fnv1a-64 hex) is an ADDITIVE v1 extension — both sides emit
 * it when they have it and ignore it when they don't, so the wire version
 * stays 1. Merge matches incoming records by "hash" FIRST (exact match
 * against tracks.content_hash), falling back to the tag identity below. */
#define MN_SYNC_PROTOCOL 1

/* Default port of the phone's sync server (§4a). */
#define MN_SYNC_DEFAULT_PORT 8797

/* Snapshot filename shared with the phone's file export/import (§4b). */
#define MN_SYNC_FILE_NAME "nexgen_library_sync.json"

/* ------------------------------------------------------------------ */
/* Environment: library handle + serialization hooks                   */
/* ------------------------------------------------------------------ */

/*
 * lock/unlock (both optional, both-or-neither) are taken around every
 * mn_library access this module makes — the app passes hooks that enter/
 * leave its lib_lock so sync writes serialize against the scanner and UI.
 * Lock scopes are kept short: never held across network I/O.
 */
/* Per-field participation (the Settings -> Sync "what gets synced"
 * toggles). A disabled group is neither emitted in snapshots nor applied
 * from remote ones. Callers must set these EXPLICITLY (a zeroed struct
 * syncs nothing — the app wrapper loads the persisted config, default all
 * on). */
typedef struct mn_sync_fields {
    bool likes;      /* liked / disliked                 */
    bool ratings;    /* 5-star ratings                   */
    bool plays;      /* play counts + last-played times  */
} mn_sync_fields;

/*
 * Per-category tallies of what a merge actually CHANGED locally — the
 * "what got synced" story the UI tells in plain words. A row can land in
 * several buckets at once (thumb + rating + plays all moved). Rows whose
 * thumb was CLEARED (like/dislike -> neutral) count in `cleared`; they
 * are still inside the aggregate `applied` row count.
 */
typedef struct mn_sync_counts {
    int likes;       /* rows switched to liked            */
    int dislikes;    /* rows switched to disliked         */
    int cleared;     /* rows whose thumb went back to neutral */
    int ratings;     /* star-rating changes               */
    int plays;       /* play-count / last-played advances */
    int books;       /* audiobook chapter positions moved */
} mn_sync_counts;

typedef struct mn_sync_env {
    mn_library    *lib;
    void         (*lock)(void *user);
    void         (*unlock)(void *user);
    void          *lock_user;
    mn_sync_fields fields;
    /* Optional: when non-NULL, mn_sync_merge_snapshot ADDS its per-
     * category tallies here (caller zeroes it; additive so the file-
     * import path could batch several merges into one summary). */
    mn_sync_counts *counts_out;
    /* Optional remote-control pairing info: when control_token is
     * non-NULL/non-empty, built snapshots carry a top-level
     *   "control":{"port":N,"token":"...","name":"..."}
     * block so the phone that receives our push learns where THIS
     * machine's /control/* listener lives (the sender's IP is the
     * address; it is never claimed in the payload). Snapshots travel
     * only to the user's REGISTERED device — the block rides the same
     * trust as the metrics themselves. */
    int            control_port;
    const char    *control_token;
    const char    *control_name;
} mn_sync_env;

/*
 * Progress callback for mn_sync_run: invoked on the calling (worker) thread
 * on every state change. `state` is one of
 *     "connecting" | "pulling" | "merging" | "pushing" | "done" | "error"
 * applied/skipped are the LOCAL merge counts (remote records applied to /
 * skipped by this library), pushed is the count the PHONE reported applying
 * from our snapshot. by_hash/by_id split the locally MATCHED remote records
 * by how they matched: content fingerprint vs tag identity (they cover every
 * matched record, changed or not — applied is a subset of their sum).
 * `error` is a short message on "error", "" otherwise.
 */
typedef void (*mn_sync_progress_cb)(void *user, const char *state,
                                    int applied, int skipped, int pushed,
                                    int by_hash, int by_id,
                                    const char *error);

/* ------------------------------------------------------------------ */
/* Identity (§1)                                                       */
/* ------------------------------------------------------------------ */

/*
 * Compute the portable track identity:
 *     norm(artist) + norm(title) + norm(album) + floor(durationMs/10000)
 * concatenated DIRECTLY, WITH NO SEPARATORS — this is the Android app's
 * wire format (SyncEngine.identity) and must never change unilaterally.
 * norm() trims, lowercases ASCII, collapses runs of [\s._-] to one space,
 * strips (...) and [...] groups non-greedily, and trims again —
 * byte-for-byte the §1 algorithm. `out` is always NUL-terminated
 * (truncated if out_n is too small).
 */
void mn_sync_identity(const char *artist, const char *title,
                      const char *album, int64_t duration_ms,
                      char *out, size_t out_n);

/* ------------------------------------------------------------------ */
/* Snapshot build / merge (§2, §3)                                     */
/* ------------------------------------------------------------------ */

/*
 * Build this library's snapshot JSON (§2: protocol 1, device "desktop",
 * only tracks with a non-default metric). Returns a malloc'd NUL-terminated
 * string the caller frees, or NULL on OOM/error.
 */
char *mn_sync_build_snapshot(const mn_sync_env *env);

/*
 * Merge a remote snapshot into the library (§3: max-merge play stats,
 * last-write-wins preference group keyed on updatedAt). Refuses snapshots
 * with protocol > MN_SYNC_PROTOCOL. Records carrying the optional "hash"
 * field are matched against tracks.content_hash FIRST; the tag-identity map
 * is the fallback. Rows that change are written through mn_library_sync_apply
 * inside ONE transaction. `out_applied` counts rows updated; `out_skipped`
 * counts remote records with no local match; `out_by_hash`/`out_by_id` count
 * matched records per match path (all optional). Returns false on
 * parse/protocol/db failure.
 */
bool mn_sync_merge_snapshot(const mn_sync_env *env, const char *json,
                            int *out_applied, int *out_skipped,
                            int *out_by_hash, int *out_by_id);

/* ------------------------------------------------------------------ */
/* Transport: LAN HTTP against the phone (§4a)                         */
/* ------------------------------------------------------------------ */

/*
 * Full sync flow against the phone's server at host:port —
 *   GET /sync/ping (verify protocol == 1)  ->  GET /sync/snapshot  ->
 *   merge into local  ->  build local snapshot  ->  POST /sync/merge  ->
 *   parse the phone's applied/skipped counts.
 * Progress lands on `cb` (may be NULL). Returns true when the whole flow
 * completed. Blocking (~5 s connect / ~10 s receive timeouts per request);
 * worker thread only.
 */
bool mn_sync_run(const mn_sync_env *env, const char *host, int port,
                 mn_sync_progress_cb cb, void *user);

/* ------------------------------------------------------------------ */
/* Transport: wireless file transfer to the phone                      */
/* ------------------------------------------------------------------ */

/*
 * Per-chunk progress callback for mn_sync_send_file: invoked on the calling
 * (worker) thread as body bytes go out. `sent`/`total` are byte counts;
 * the final call has sent == total.
 */
typedef void (*mn_sync_xfer_cb)(void *user, int64_t sent, int64_t total);

/*
 * Upload ONE file's raw bytes to the phone:
 *     POST /sync/file?name=<url-encoded name>&hash=<16-hex content hash>
 * The body is streamed straight from disk in fixed chunks (Content-Length
 * set up front; no multipart, no chunking), so a ~200 MB file never sits in
 * memory. `hash` is the schema-v7 content fingerprint the CALLER computed
 * (the compute half lives in the host layer beside the backfill worker).
 * The phone replies {"ok":true,"path":"...","skipped":bool} — skipped:true
 * means it already had that hash and wrote nothing; mirrored into
 * *out_skipped (optional). Returns true on HTTP 200 + ok:true. On failure
 * `err` (optional) gets a short user-facing message. Blocking (~5 s connect,
 * generous send window for big files); worker thread only.
 */
bool mn_sync_send_file(const char *host, int port, const char *file_path,
                       const char *name, const char *hash,
                       mn_sync_xfer_cb cb, void *user,
                       bool *out_skipped, char *err, size_t err_n);

/*
 * Ask the phone which of `hashes_csv` (comma-separated 16-hex fingerprints)
 * it already has: GET /sync/have?hashes=<csv>. Returns the malloc'd raw
 * reply body ({"have":["<hash>",...]}; caller frees and parses) or NULL on
 * any failure — unreachable phones must degrade soft. The URL is built
 * dynamically, so batches of ~1500 hashes are fine. Blocking; worker only.
 */
char *mn_sync_have(const char *host, int port, const char *hashes_csv);

/* ------------------------------------------------------------------ */
/* Transport: snapshot files (§4b)                                     */
/* ------------------------------------------------------------------ */

/*
 * The default snapshot path "<data_dir>/sync/MN_SYNC_FILE_NAME" (the sync/
 * subdirectory is created). Copies at most `n` bytes, NUL-terminated.
 */
void mn_sync_default_path(const char *data_dir, char *out, size_t n);

/* Write this library's snapshot to `path`. Returns true on success. */
bool mn_sync_export_file(const mn_sync_env *env, const char *path);

/* Read a snapshot file from `path` and merge it (§3). Counts as in
 * mn_sync_merge_snapshot. Returns false on read/parse/db failure. */
bool mn_sync_import_file(const mn_sync_env *env, const char *path,
                         int *out_applied, int *out_skipped);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_SYNC_H */
