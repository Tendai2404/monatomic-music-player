/*
 * playlists.h — Static and smart playlists for Monatomic Music Player.
 *
 * Public C API contract for managing playlists persisted in an sqlite3
 * database. Two playlist kinds are supported:
 *
 *   - STATIC playlists: an explicitly ordered list of track ids. Members are
 *     stored, ordered, and mutated by hand (add/insert/remove/move/clear).
 *
 *   - SMART playlists: a saved query described by a rule tree. The rule tree
 *     (fields, operators, AND/OR groups, sort, limit) is compiled into a
 *     single parameterized SQL SELECT and evaluated on demand. Results are
 *     returned windowed (offset + limit) so that a smart playlist backed by a
 *     1,000,000-track library never materializes more rows than requested.
 *
 * Design constraints:
 *   - All persistence is against a caller-owned sqlite3* handle. This module
 *     never opens, closes, or owns the connection.
 *   - No O(n) scans over the whole library in the UI thread. Every query is
 *     windowed and parameterized; rule trees compile to prepared statements.
 *   - Track ids are stable 64-bit row ids from the tracks table (mn_track_id).
 *   - The caller owns all returned id buffers and frees them with
 *     mn_playlists_free_ids().
 *   - Thread-affinity: a single sqlite3* handle must not be used concurrently
 *     from multiple threads. Callers running background evaluation should use
 *     a dedicated connection. Functions here perform no internal locking.
 *
 * All functions returning mn_playlist_status return MN_PLAYLIST_OK (0) on
 * success and a negative-free positive error code otherwise. Functions that
 * produce output write it through out-parameters and leave them untouched on
 * failure unless documented otherwise.
 */

#ifndef MN_PLAYLISTS_H
#define MN_PLAYLISTS_H

#include <stddef.h>   /* size_t                */
#include <stdint.h>   /* int64_t, uint64_t     */
#include <stdbool.h>  /* bool                  */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of the sqlite3 handle so callers need not include
 * sqlite3.h to see this API. The real type comes from vendor/sqlite3.h. */
typedef struct sqlite3 sqlite3;

/* ------------------------------------------------------------------------- *
 * Core scalar types
 * ------------------------------------------------------------------------- */

/* Stable database row id for a track (tracks.id). */
typedef int64_t mn_track_id;

/* Stable database row id for a playlist (playlists.id). */
typedef int64_t mn_playlist_id;

/* Sentinel meaning "no such id" / "not found". */
#define MN_PLAYLIST_ID_NONE ((mn_playlist_id)0)
#define MN_TRACK_ID_NONE    ((mn_track_id)0)

/* ------------------------------------------------------------------------- *
 * Status codes
 * ------------------------------------------------------------------------- */

typedef enum mn_playlist_status {
    MN_PLAYLIST_OK               = 0,  /* success                              */
    MN_PLAYLIST_ERR_INVALID_ARG  = 1,  /* NULL/out-of-range argument           */
    MN_PLAYLIST_ERR_NOT_FOUND    = 2,  /* playlist/track/member does not exist */
    MN_PLAYLIST_ERR_WRONG_KIND   = 3,  /* static op on smart list or vice versa*/
    MN_PLAYLIST_ERR_DUPLICATE    = 4,  /* name collision where unique required */
    MN_PLAYLIST_ERR_SQL          = 5,  /* underlying sqlite3 error             */
    MN_PLAYLIST_ERR_NOMEM        = 6,  /* allocation failure                   */
    MN_PLAYLIST_ERR_RULE         = 7,  /* malformed / uncompilable rule tree   */
    MN_PLAYLIST_ERR_IO           = 8,  /* file read/write error (import/export)*/
    MN_PLAYLIST_ERR_PARSE        = 9,  /* malformed playlist file on import    */
    MN_PLAYLIST_ERR_RANGE        = 10, /* index/window out of range            */
    MN_PLAYLIST_ERR_UNSUPPORTED  = 11, /* operator/field/format not supported  */
    MN_PLAYLIST_ERR_SCHEMA       = 12  /* required tables/columns missing      */
} mn_playlist_status;

/* Human-readable, static string for a status code. Never NULL. */
const char *mn_playlist_status_str(mn_playlist_status status);

/* ------------------------------------------------------------------------- *
 * Playlist kind
 * ------------------------------------------------------------------------- */

typedef enum mn_playlist_kind {
    MN_PLAYLIST_KIND_STATIC = 0,
    MN_PLAYLIST_KIND_SMART  = 1
} mn_playlist_kind;

/* ------------------------------------------------------------------------- *
 * Smart-playlist rule model
 *
 * A smart playlist is defined by a rule tree. Leaves are conditions
 * (field OP value); interior nodes are groups combining children with a
 * boolean conjunction (AND/OR). A single top-level group plus optional sort
 * and limit is compiled into one parameterized SQL SELECT of track ids.
 * ------------------------------------------------------------------------- */

/* Fields addressable by a rule condition. These map to columns (or derived
 * expressions) on the tracks table and its satellites. The compiler is
 * responsible for the concrete SQL mapping; callers only reference the enum. */
typedef enum mn_pl_field {
    MN_PL_FIELD_TITLE        = 0,  /* text    */
    MN_PL_FIELD_ARTIST       = 1,  /* text    */
    MN_PL_FIELD_ALBUM_ARTIST = 2,  /* text    */
    MN_PL_FIELD_ALBUM        = 3,  /* text    */
    MN_PL_FIELD_GENRE        = 4,  /* text    */
    MN_PL_FIELD_COMPOSER     = 5,  /* text    */
    MN_PL_FIELD_COMMENT      = 6,  /* text    */
    MN_PL_FIELD_GROUPING     = 7,  /* text    */
    MN_PL_FIELD_FILE_PATH    = 8,  /* text    */
    MN_PL_FIELD_FILE_KIND    = 9,  /* text (extension/codec, e.g. "flac")     */

    MN_PL_FIELD_YEAR         = 20, /* integer */
    MN_PL_FIELD_TRACK_NO     = 21, /* integer */
    MN_PL_FIELD_DISC_NO      = 22, /* integer */
    MN_PL_FIELD_DURATION_MS  = 23, /* integer, milliseconds                   */
    MN_PL_FIELD_BITRATE      = 24, /* integer, bits/sec                       */
    MN_PL_FIELD_SAMPLE_RATE  = 25, /* integer, Hz                             */
    MN_PL_FIELD_CHANNELS     = 26, /* integer                                 */
    MN_PL_FIELD_FILE_SIZE    = 27, /* integer, bytes                          */
    MN_PL_FIELD_BPM          = 28, /* integer                                 */
    MN_PL_FIELD_PLAY_COUNT   = 29, /* integer                                 */
    MN_PL_FIELD_SKIP_COUNT   = 30, /* integer                                 */
    MN_PL_FIELD_RATING       = 31, /* integer 0..100 (or 0..5 x20)            */

    MN_PL_FIELD_DATE_ADDED   = 40, /* integer, unix seconds                   */
    MN_PL_FIELD_LAST_PLAYED  = 41, /* integer, unix seconds                   */
    MN_PL_FIELD_DATE_MODIFIED= 42, /* integer, unix seconds                   */

    MN_PL_FIELD_LOVED        = 60, /* boolean (stored as 0/1)                 */
    MN_PL_FIELD_HAS_LYRICS   = 61, /* boolean                                 */
    MN_PL_FIELD_HAS_ARTWORK  = 62, /* boolean                                 */

    /* Reference to another playlist's membership; value is an mn_playlist_id.
     * Used with MN_PL_OP_IN_PLAYLIST / MN_PL_OP_NOT_IN_PLAYLIST. */
    MN_PL_FIELD_PLAYLIST_REF = 80
} mn_pl_field;

/* The value domain a field carries. Determines which member of
 * mn_pl_value.v is read and which operators are legal. */
typedef enum mn_pl_value_type {
    MN_PL_VT_TEXT    = 0,  /* UTF-8 string          */
    MN_PL_VT_INT     = 1,  /* 64-bit signed integer */
    MN_PL_VT_BOOL    = 2,  /* 0/1                   */
    MN_PL_VT_PLAYLIST= 3   /* mn_playlist_id        */
} mn_pl_value_type;

/* Operators applied by a condition. Legality depends on the field's value
 * type; the compiler validates the pairing and returns MN_PLAYLIST_ERR_RULE
 * on a mismatch. */
typedef enum mn_pl_op {
    /* Universal */
    MN_PL_OP_EQ            = 0,  /* ==                                        */
    MN_PL_OP_NE           = 1,  /* !=                                        */

    /* Numeric / date ordering */
    MN_PL_OP_GT           = 10, /* >                                         */
    MN_PL_OP_GE           = 11, /* >=                                        */
    MN_PL_OP_LT           = 12, /* <                                         */
    MN_PL_OP_LE           = 13, /* <=                                        */
    MN_PL_OP_BETWEEN      = 14, /* v.i2 range inclusive [i, i2]              */

    /* Text (case-insensitive; NFC-normalized comparison expected) */
    MN_PL_OP_CONTAINS     = 20,
    MN_PL_OP_NOT_CONTAINS = 21,
    MN_PL_OP_STARTS_WITH  = 22,
    MN_PL_OP_ENDS_WITH    = 23,
    MN_PL_OP_MATCHES      = 24, /* FTS5 MATCH against the search index        */

    /* Emptiness */
    MN_PL_OP_IS_EMPTY     = 30, /* value ignored                             */
    MN_PL_OP_IS_NOT_EMPTY = 31, /* value ignored                             */

    /* Relative-date windows for date fields; v.i = N units before "now". */
    MN_PL_OP_IN_LAST_DAYS = 40,
    MN_PL_OP_NOT_IN_LAST_DAYS = 41,

    /* Playlist membership (field == MN_PL_FIELD_PLAYLIST_REF) */
    MN_PL_OP_IN_PLAYLIST     = 50,
    MN_PL_OP_NOT_IN_PLAYLIST = 51
} mn_pl_op;

/* A concrete comparison value. Only the member indicated by `type` is read.
 * For text values the string is borrowed for the duration of the compile call
 * only; the compiler copies what it needs. */
typedef struct mn_pl_value {
    mn_pl_value_type type;
    union {
        struct {
            const char *ptr;   /* UTF-8, not required NUL-terminated if len>0 */
            size_t      len;    /* byte length; if 0 and ptr!=NULL, use strlen*/
        } text;
        struct {
            int64_t i;          /* primary integer / boolean / lower bound     */
            int64_t i2;         /* upper bound for MN_PL_OP_BETWEEN            */
        };
        mn_playlist_id playlist; /* for MN_PL_VT_PLAYLIST                      */
    } v;
} mn_pl_value;

/* Kind of a rule-tree node. */
typedef enum mn_pl_node_kind {
    MN_PL_NODE_CONDITION = 0, /* leaf: field OP value            */
    MN_PL_NODE_GROUP     = 1  /* interior: children joined by conj*/
} mn_pl_node_kind;

/* Boolean conjunction for a group node. */
typedef enum mn_pl_conj {
    MN_PL_CONJ_AND = 0,
    MN_PL_CONJ_OR  = 1
} mn_pl_conj;

/* A node in the rule tree. Conditions and groups share this struct;
 * `kind` selects which members are meaningful. Groups own their children
 * array but do not free it — the whole tree is caller-owned (typically a
 * contiguous arena the caller manages). `negate` inverts the node's result. */
typedef struct mn_pl_node mn_pl_node;
struct mn_pl_node {
    mn_pl_node_kind kind;
    bool            negate;   /* NOT applied to this node's result           */

    /* Valid when kind == MN_PL_NODE_CONDITION */
    mn_pl_field  field;
    mn_pl_op     op;
    mn_pl_value  value;

    /* Valid when kind == MN_PL_NODE_GROUP */
    mn_pl_conj    conj;
    mn_pl_node  **children;   /* array of child pointers                     */
    size_t        child_count;
};

/* Sortable ordering keys for smart-playlist results. */
typedef enum mn_pl_sort_key {
    MN_PL_SORT_NONE        = 0,  /* database order (rowid)                    */
    MN_PL_SORT_TITLE       = 1,
    MN_PL_SORT_ARTIST      = 2,
    MN_PL_SORT_ALBUM       = 3,
    MN_PL_SORT_ALBUM_ARTIST= 4,
    MN_PL_SORT_YEAR        = 5,
    MN_PL_SORT_GENRE       = 6,
    MN_PL_SORT_DURATION    = 7,
    MN_PL_SORT_DATE_ADDED  = 8,
    MN_PL_SORT_LAST_PLAYED = 9,
    MN_PL_SORT_PLAY_COUNT  = 10,
    MN_PL_SORT_RATING      = 11,
    MN_PL_SORT_TRACK_NO    = 12,
    MN_PL_SORT_BPM         = 13,
    MN_PL_SORT_RANDOM      = 14  /* random per evaluation                    */
} mn_pl_sort_key;

typedef enum mn_pl_sort_dir {
    MN_PL_SORT_ASC  = 0,
    MN_PL_SORT_DESC = 1
} mn_pl_sort_dir;

/* Sentinel meaning "no cap" for smart-playlist limit. */
#define MN_PL_LIMIT_NONE ((int64_t)-1)

/* Complete definition of a smart playlist. `root` may be NULL, which matches
 * every track (subject to sort/limit). */
typedef struct mn_pl_rules {
    mn_pl_node    *root;       /* top-level node; NULL => match all           */
    mn_pl_sort_key sort_key;
    mn_pl_sort_dir sort_dir;
    int64_t        limit;      /* max rows the smart list yields; MN_PL_LIMIT_NONE*/
} mn_pl_rules;

/* ------------------------------------------------------------------------- *
 * Metadata record returned by info/list queries
 * ------------------------------------------------------------------------- */

typedef struct mn_playlist_info {
    mn_playlist_id   id;
    mn_playlist_kind kind;
    char            *name;         /* heap UTF-8, NUL-terminated; caller frees */
    int64_t          created_at;   /* unix seconds                            */
    int64_t          updated_at;   /* unix seconds                            */
    int64_t          member_count; /* static: stored count; smart: -1 (unknown
                                    * without evaluation)                      */
} mn_playlist_info;

/* Free the heap members of a single info record (does not free the struct). */
void mn_playlist_info_dispose(mn_playlist_info *info);

/* Free an array of info records (frees members and the array itself). */
void mn_playlist_info_free_array(mn_playlist_info *arr, size_t count);

/* ------------------------------------------------------------------------- *
 * Schema management
 * ------------------------------------------------------------------------- */

/* Create the playlists/playlist_members tables and indexes if they do not
 * already exist. Idempotent. Must be called once per database before any
 * other function here. Returns MN_PLAYLIST_ERR_SCHEMA if the required tracks
 * table is absent. */
mn_playlist_status mn_playlists_init_schema(sqlite3 *db);

/* ------------------------------------------------------------------------- *
 * Creation / deletion / rename
 * ------------------------------------------------------------------------- */

/* Create an empty static playlist named `name`. On success writes the new id
 * to *out_id. Names need not be unique unless the database enforces it; a
 * unique-index violation surfaces as MN_PLAYLIST_ERR_DUPLICATE. */
mn_playlist_status mn_playlist_create_static(sqlite3        *db,
                                             const char     *name,
                                             mn_playlist_id *out_id);

/* Create a smart playlist named `name` defined by `rules`. The rule tree is
 * compiled and validated immediately (bad trees fail with
 * MN_PLAYLIST_ERR_RULE before anything is written) and serialized for later
 * evaluation. On success writes the new id to *out_id. `rules` is borrowed;
 * the caller retains ownership of the tree. */
mn_playlist_status mn_playlist_create_smart(sqlite3            *db,
                                            const char         *name,
                                            const mn_pl_rules  *rules,
                                            mn_playlist_id     *out_id);

/* Replace the rule tree of an existing smart playlist. Fails with
 * MN_PLAYLIST_ERR_WRONG_KIND if `id` is a static playlist. */
mn_playlist_status mn_playlist_set_smart_rules(sqlite3           *db,
                                               mn_playlist_id     id,
                                               const mn_pl_rules *rules);

/* Read the rule tree of a smart playlist into a freshly allocated *out_rules
 * (deep copy, caller owns). Free with mn_pl_rules_free(). Fails with
 * MN_PLAYLIST_ERR_WRONG_KIND for static playlists. */
mn_playlist_status mn_playlist_get_smart_rules(sqlite3        *db,
                                               mn_playlist_id  id,
                                               mn_pl_rules   **out_rules);

/* Free a rule tree previously returned by mn_playlist_get_smart_rules(). */
void mn_pl_rules_free(mn_pl_rules *rules);

/* Permanently delete a playlist and (for static lists) all its membership
 * rows. Deleting a nonexistent id returns MN_PLAYLIST_ERR_NOT_FOUND. */
mn_playlist_status mn_playlist_delete(sqlite3 *db, mn_playlist_id id);

/* Rename a playlist. Returns MN_PLAYLIST_ERR_DUPLICATE on a unique collision. */
mn_playlist_status mn_playlist_rename(sqlite3        *db,
                                      mn_playlist_id  id,
                                      const char     *new_name);

/* ------------------------------------------------------------------------- *
 * Introspection
 * ------------------------------------------------------------------------- */

/* Fetch metadata for one playlist. Writes into *out_info (caller frees its
 * heap members with mn_playlist_info_dispose). */
mn_playlist_status mn_playlist_get_info(sqlite3          *db,
                                        mn_playlist_id    id,
                                        mn_playlist_info *out_info);

/* List playlists, newest-updated first, windowed by [offset, offset+limit).
 * Allocates *out_arr (caller frees with mn_playlist_info_free_array) and sets
 * *out_count to the number of records returned. Pass limit == 0 to fetch the
 * total count only (via out_total) without materializing rows.
 * If out_total is non-NULL it receives the total number of playlists. */
mn_playlist_status mn_playlist_list(sqlite3           *db,
                                    int64_t            offset,
                                    int64_t            limit,
                                    mn_playlist_info **out_arr,
                                    size_t            *out_count,
                                    int64_t           *out_total);

/* ------------------------------------------------------------------------- *
 * Static-playlist membership mutation
 *
 * Members are kept in an explicit order (0-based positions). All mutators
 * fail with MN_PLAYLIST_ERR_WRONG_KIND when applied to a smart playlist.
 * ------------------------------------------------------------------------- */

/* Append `count` tracks (in order) to the end of a static playlist.
 * Duplicates are permitted. */
mn_playlist_status mn_playlist_add(sqlite3           *db,
                                   mn_playlist_id     id,
                                   const mn_track_id *tracks,
                                   size_t             count);

/* Insert `count` tracks at position `at` (0-based). Existing members at and
 * after `at` shift right. `at` may equal the current member count (append).
 * Out-of-range `at` returns MN_PLAYLIST_ERR_RANGE. */
mn_playlist_status mn_playlist_insert(sqlite3           *db,
                                      mn_playlist_id     id,
                                      int64_t            at,
                                      const mn_track_id *tracks,
                                      size_t             count);

/* Remove the members at the given positions (0-based). Positions are
 * deduplicated and applied atomically; remaining members are renumbered to
 * stay contiguous. Any out-of-range position returns MN_PLAYLIST_ERR_RANGE
 * and leaves the playlist unchanged. */
mn_playlist_status mn_playlist_remove_at(sqlite3       *db,
                                         mn_playlist_id id,
                                         const int64_t *positions,
                                         size_t         count);

/* Remove every occurrence of `track` from a static playlist and renumber.
 * Removing a track that is not present is not an error (0 rows affected). */
mn_playlist_status mn_playlist_remove_track(sqlite3       *db,
                                            mn_playlist_id id,
                                            mn_track_id    track);

/* Move a contiguous run of `count` members starting at `from` so that it
 * begins at `to` (positions interpreted in the pre-move numbering). Members
 * are renumbered to remain contiguous. Out-of-range indices return
 * MN_PLAYLIST_ERR_RANGE. */
mn_playlist_status mn_playlist_move(sqlite3       *db,
                                    mn_playlist_id id,
                                    int64_t        from,
                                    int64_t        count,
                                    int64_t        to);

/* Remove all members, leaving an empty static playlist. */
mn_playlist_status mn_playlist_clear(sqlite3 *db, mn_playlist_id id);

/* Number of members in a static playlist. Writes to *out_count. */
mn_playlist_status mn_playlist_count(sqlite3       *db,
                                     mn_playlist_id id,
                                     int64_t       *out_count);

/* ------------------------------------------------------------------------- *
 * Track-id result buffers
 * ------------------------------------------------------------------------- */

/* An owned, heap-allocated array of track ids returned by windowed queries.
 * Free with mn_playlists_free_ids(). */
typedef struct mn_track_id_list {
    mn_track_id *ids;    /* heap array of `count` ids (NULL iff count == 0)   */
    size_t       count;  /* number of ids in this window                     */
    int64_t      total;  /* total rows the query would yield ignoring window;
                          * -1 if the total was not computed                  */
} mn_track_id_list;

/* Free an id list produced by this module. Safe on a zeroed struct. */
void mn_playlists_free_ids(mn_track_id_list *list);

/* ------------------------------------------------------------------------- *
 * Reading members / evaluating smart playlists
 * ------------------------------------------------------------------------- */

/* Read a window of a STATIC playlist's members in stored order, ordered by
 * position. Fills *out (caller frees with mn_playlists_free_ids). Pass
 * limit < 0 for "all remaining from offset". If want_total is true, *out.total
 * is filled with the full member count. Smart playlists return
 * MN_PLAYLIST_ERR_WRONG_KIND (use mn_playlist_evaluate_smart). */
mn_playlist_status mn_playlist_get_members(sqlite3          *db,
                                           mn_playlist_id    id,
                                           int64_t           offset,
                                           int64_t           limit,
                                           bool              want_total,
                                           mn_track_id_list *out);

/* Evaluate a SMART playlist and return a window of matching track ids in the
 * playlist's configured sort order. The stored rule tree is compiled to a
 * parameterized SQL SELECT and executed; only [offset, offset+limit) rows are
 * materialized. `limit` here bounds the returned window and is further capped
 * by the playlist's own rule limit. Pass limit < 0 for "all remaining"
 * (still bounded by the rule limit). If want_total is true, *out.total is the
 * full match count (subject to the rule limit), computed with a COUNT query.
 * Static playlists return MN_PLAYLIST_ERR_WRONG_KIND. */
mn_playlist_status mn_playlist_evaluate_smart(sqlite3          *db,
                                              mn_playlist_id    id,
                                              int64_t           offset,
                                              int64_t           limit,
                                              bool              want_total,
                                              mn_track_id_list *out);

/* Evaluate an ad-hoc rule tree without persisting a playlist. Same windowing
 * semantics as mn_playlist_evaluate_smart. Useful for live rule previews in
 * the editor UI. `rules` is borrowed. */
mn_playlist_status mn_playlist_evaluate_rules(sqlite3           *db,
                                              const mn_pl_rules *rules,
                                              int64_t            offset,
                                              int64_t            limit,
                                              bool               want_total,
                                              mn_track_id_list  *out);

/* Compile a rule tree to its parameterized SQL text without executing it,
 * for debugging/inspection. Writes a heap NUL-terminated string to *out_sql
 * (caller frees with mn_playlists_free_sql). Bind-parameter values are not
 * returned; this is for diagnostics only. */
mn_playlist_status mn_playlist_compile_rules_sql(const mn_pl_rules *rules,
                                                 char             **out_sql);

/* Free a string returned by mn_playlist_compile_rules_sql. */
void mn_playlists_free_sql(char *sql);

/* ------------------------------------------------------------------------- *
 * Import / export (M3U, M3U8, PLS)
 * ------------------------------------------------------------------------- */

typedef enum mn_playlist_format {
    MN_PLAYLIST_FMT_AUTO = 0, /* infer from file extension / content         */
    MN_PLAYLIST_FMT_M3U  = 1, /* legacy M3U (local encoding, #EXTM3U extras)  */
    MN_PLAYLIST_FMT_M3U8 = 2, /* UTF-8 M3U                                    */
    MN_PLAYLIST_FMT_PLS  = 3  /* INI-style PLS                                */
} mn_playlist_format;

/* Options controlling how imported file entries are resolved to track ids. */
typedef struct mn_playlist_import_opts {
    mn_playlist_format format;        /* MN_PLAYLIST_FMT_AUTO to infer        */

    /* Directory used to resolve relative paths in the file. If NULL, the
     * directory containing the source file is used. */
    const char *base_dir;

    /* If true, entries whose file path is not found in the tracks table are
     * silently skipped; if false, an unresolved entry is not fatal but is
     * counted in *out_unresolved. Import never fails solely due to unresolved
     * entries. */
    bool skip_unresolved;

    /* If true and an entry's absolute path is not yet in the library, the
     * importer may insert a stub track row (resolved by a caller-provided
     * hook is out of scope here; without library support this is ignored). */
    bool insert_missing;
} mn_playlist_import_opts;

/* Result counts from an import operation. */
typedef struct mn_playlist_import_result {
    mn_playlist_id id;          /* id of the created/updated static playlist  */
    int64_t        total;       /* entries parsed from the file               */
    int64_t        resolved;    /* entries matched to a track id              */
    int64_t        unresolved;  /* entries with no matching track             */
} mn_playlist_import_result;

/* Import a playlist file into a NEW static playlist named `name` (or, if
 * `name` is NULL, a name derived from the file). File entries are matched to
 * existing tracks by path. Writes counts to *out_result (may be NULL).
 * Reads from a filesystem path (UTF-8 on all platforms; converted to the
 * native wide path on Windows internally). */
mn_playlist_status mn_playlist_import_file(sqlite3                        *db,
                                           const char                     *path,
                                           const char                     *name,
                                           const mn_playlist_import_opts  *opts,
                                           mn_playlist_import_result      *out_result);

/* Import from an in-memory buffer instead of a file. `format` must be explicit
 * (MN_PLAYLIST_FMT_AUTO infers from content only). Otherwise identical to
 * mn_playlist_import_file. */
mn_playlist_status mn_playlist_import_buffer(sqlite3                       *db,
                                             const char                    *data,
                                             size_t                         len,
                                             const char                    *name,
                                             const mn_playlist_import_opts *opts,
                                             mn_playlist_import_result     *out_result);

/* Options controlling export output. */
typedef struct mn_playlist_export_opts {
    mn_playlist_format format;      /* AUTO infers from `path` extension       */

    /* Write file paths relative to the directory of the output file when true;
     * otherwise write absolute paths. */
    bool relative_paths;

    /* Emit extended directives (#EXTM3U/#EXTINF for M3U/M3U8) when true. */
    bool extended;

    /* For smart playlists: evaluate and export the resulting tracks. If false,
     * exporting a smart playlist returns MN_PLAYLIST_ERR_WRONG_KIND. */
    bool evaluate_smart;
} mn_playlist_export_opts;

/* Export a playlist (static, or smart when opts->evaluate_smart) to a file at
 * `path` in the requested format. Track ids are resolved to file paths and
 * durations from the tracks table. Overwrites an existing file. */
mn_playlist_status mn_playlist_export_file(sqlite3                       *db,
                                           mn_playlist_id                 id,
                                           const char                    *path,
                                           const mn_playlist_export_opts *opts);

/* Export to a caller-provided growable buffer. On success *out_data points to
 * a heap NUL-terminated UTF-8 buffer of *out_len bytes (excluding the NUL);
 * free with mn_playlists_free_buffer(). `path_hint` (may be NULL) is used only
 * to compute relative paths and infer format when opts->format is AUTO. */
mn_playlist_status mn_playlist_export_buffer(sqlite3                       *db,
                                             mn_playlist_id                 id,
                                             const char                    *path_hint,
                                             const mn_playlist_export_opts *opts,
                                             char                         **out_data,
                                             size_t                        *out_len);

/* Free a buffer returned by mn_playlist_export_buffer. */
void mn_playlists_free_buffer(char *data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_PLAYLISTS_H */
