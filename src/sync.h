/*
 * sync.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Desktop side of the NEX-GEN library-sync protocol (SYNC_PROTOCOL.md v1):
 * per-track likes/dislikes, 5-star ratings, play counts and last-played
 * times converge between this library and the phone's, keyed by a portable
 * TRACK IDENTITY derived from tags (§1) — never by file path.
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

/* Protocol version implemented (snapshots with a higher one are refused). */
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

typedef struct mn_sync_env {
    mn_library    *lib;
    void         (*lock)(void *user);
    void         (*unlock)(void *user);
    void          *lock_user;
    mn_sync_fields fields;
} mn_sync_env;

/*
 * Progress callback for mn_sync_run: invoked on the calling (worker) thread
 * on every state change. `state` is one of
 *     "connecting" | "pulling" | "merging" | "pushing" | "done" | "error"
 * applied/skipped are the LOCAL merge counts (remote records applied to /
 * skipped by this library), pushed is the count the PHONE reported applying
 * from our snapshot. `error` is a short message on "error", "" otherwise.
 */
typedef void (*mn_sync_progress_cb)(void *user, const char *state,
                                    int applied, int skipped, int pushed,
                                    const char *error);

/* ------------------------------------------------------------------ */
/* Identity (§1)                                                       */
/* ------------------------------------------------------------------ */

/*
 * Compute the portable track identity: norm(artist) 0x01 norm(title) 0x01
 * norm(album) 0x01 floor(durationMs/10000). norm() trims, lowercases ASCII,
 * collapses runs of [\s._-] to one space, strips (...) and [...] groups
 * non-greedily, and trims again — byte-for-byte the §1 algorithm. `out` is
 * always NUL-terminated (truncated if out_n is too small).
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
 * with protocol > MN_SYNC_PROTOCOL. Rows that change are written through
 * mn_library_sync_apply inside ONE transaction. `out_applied` counts rows
 * updated; `out_skipped` counts remote records with no local identity match
 * (both optional). Returns false on parse/protocol/db failure.
 */
bool mn_sync_merge_snapshot(const mn_sync_env *env, const char *json,
                            int *out_applied, int *out_skipped);

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
