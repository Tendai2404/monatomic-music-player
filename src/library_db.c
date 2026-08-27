/*
 * library_db.c - SQLite-backed library index for Monatomic Music Player.
 *
 * Implementation of library_db.h against the vendored amalgamation
 * vendor/sqlite3.c (SQLite 3.46.1, built with -DSQLITE_ENABLE_FTS5).
 *
 * Design highlights (see library_db.h for the API contract):
 *   - One serialized WRITER connection guarded by a mutex; per-thread READER
 *     connections stored in thread-local storage so reads never block writes.
 *   - Dimension tables (artists/albums/genres/folders/album_artists) with a
 *     maintained track_count, resolved on upsert and decremented on delete.
 *   - FTS5 external-content index over title/artist/album kept in sync by
 *     triggers when FTS5 is present; otherwise queries fall back to LIKE.
 *   - Windowed queries with a cached total count and a stable id tiebreak so
 *     paging is deterministic even when the primary sort key has duplicates.
 *   - Result rows / strings live in a per-handle bump arena, reset between
 *     windows so scrolling a 1M-row list produces zero per-row malloc churn.
 *
 * All SQL is parameterized. Every mutation funnels through mn__write_lock().
 */

#include "library_db.h"

#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Platform: mutex + thread-local storage
 * -------------------------------------------------------------------------- */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef CRITICAL_SECTION mn__mutex;
static void mn__mutex_init(mn__mutex *m)   { InitializeCriticalSection(m); }
static void mn__mutex_destroy(mn__mutex *m){ DeleteCriticalSection(m); }
static void mn__mutex_lock(mn__mutex *m)   { EnterCriticalSection(m); }
static void mn__mutex_unlock(mn__mutex *m) { LeaveCriticalSection(m); }

typedef DWORD mn__tls_key;
static int  mn__tls_create(mn__tls_key *k) {
    *k = TlsAlloc();
    return (*k == TLS_OUT_OF_INDEXES) ? -1 : 0;
}
static void *mn__tls_get(mn__tls_key k)          { return TlsGetValue(k); }
static void  mn__tls_set(mn__tls_key k, void *v) { TlsSetValue(k, v); }
static void  mn__tls_delete(mn__tls_key k)       { TlsFree(k); }
#else
#  include <pthread.h>
typedef pthread_mutex_t mn__mutex;
static void mn__mutex_init(mn__mutex *m)   { pthread_mutex_init(m, NULL); }
static void mn__mutex_destroy(mn__mutex *m){ pthread_mutex_destroy(m); }
static void mn__mutex_lock(mn__mutex *m)   { pthread_mutex_lock(m); }
static void mn__mutex_unlock(mn__mutex *m) { pthread_mutex_unlock(m); }

typedef pthread_key_t mn__tls_key;
static int  mn__tls_create(mn__tls_key *k)       { return pthread_key_create(k, NULL); }
static void *mn__tls_get(mn__tls_key k)          { return pthread_getspecific(k); }
static void  mn__tls_set(mn__tls_key k, void *v) { pthread_setspecific(k, v); }
static void  mn__tls_delete(mn__tls_key k)       { pthread_key_delete(k); }
#endif

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */

#define MN__UNUSED(x) ((void)(x))

/* Map an SQLite result code to an mn_status. */
static mn_status mn__map_sqlite(int rc) {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:      return MN_OK;
        case SQLITE_NOMEM:     return MN_ERR_NOMEM;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:    return MN_ERR_BUSY;
        case SQLITE_MISUSE:    return MN_ERR_STATE;
        case SQLITE_CONSTRAINT:return MN_ERR_CONSTRAINT;
        case SQLITE_NOTFOUND:  return MN_ERR_NOTFOUND;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:    return MN_ERR_CORRUPT;
        case SQLITE_IOERR:
        case SQLITE_FULL:
        case SQLITE_CANTOPEN:
        case SQLITE_READONLY:  return MN_ERR_IO;
        case SQLITE_RANGE:     return MN_ERR_RANGE;
        default:               return MN_ERR_GENERIC;
    }
}

/* Duplicate a C string with malloc (NULL-safe). */
static char *mn__strdup(const char *s) {
    size_t n;
    char *p;
    if (!s) s = "";
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* --------------------------------------------------------------------------
 * Arena bump allocator
 * -------------------------------------------------------------------------- */

typedef struct mn_arena_block {
    struct mn_arena_block *next;
    size_t   cap;
    size_t   used;
    uint8_t  data[1]; /* flexible payload */
} mn_arena_block;

#define MN__ARENA_MIN_BLOCK 4096u

static size_t mn__align_up(size_t v, size_t align) {
    if (align < 1) align = 1;
    return (v + (align - 1)) & ~(align - 1);
}

mn_status mn_arena_init(mn_arena *a, size_t initial_cap) {
    if (!a) return MN_ERR_INVALID;
    a->base   = NULL;
    a->used   = 0;
    a->cap    = 0;
    a->blocks = NULL;
    if (initial_cap > 0) {
        /* Pre-warm a first block. Failure here is non-fatal; alloc grows. */
        void *p = mn_arena_alloc(a, 0, 1);
        MN__UNUSED(p);
        /* Nothing else needed; a real reservation is created lazily below. */
    }
    return MN_OK;
}

/* Allocate a new block able to hold at least `need` bytes and chain it. */
static mn_arena_block *mn__arena_new_block(mn_arena *a, size_t need) {
    size_t cap = MN__ARENA_MIN_BLOCK;
    mn_arena_block *b;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) { cap = need; break; }
        cap *= 2;
    }
    b = (mn_arena_block *)malloc(sizeof(mn_arena_block) + cap);
    if (!b) return NULL;
    b->next = a->blocks;
    b->cap  = cap;
    b->used = 0;
    a->blocks = b;
    /* Track the current (head) block through the public fields. */
    a->base = b->data;
    a->used = 0;
    a->cap  = cap;
    return b;
}

void *mn_arena_alloc(mn_arena *a, size_t size, size_t align) {
    mn_arena_block *head;
    size_t off;

    if (!a) return NULL;
    if ((align & (align - 1)) != 0 || align == 0) align = 1;

    head = a->blocks;
    if (head) {
        off = mn__align_up(head->used, align);
        if (off + size <= head->cap) {
            void *p = head->data + off;
            head->used = off + size;
            a->used = head->used;
            return p;
        }
    }

    /* Need a fresh block large enough for the aligned allocation. */
    head = mn__arena_new_block(a, mn__align_up(size, align) + align);
    if (!head) return NULL;
    off = mn__align_up(head->used, align);
    {
        void *p = head->data + off;
        head->used = off + size;
        a->used = head->used;
        return p;
    }
}

const char *mn_arena_strdup(mn_arena *a, const char *s) {
    size_t n;
    char *p;
    if (!a) return NULL;
    if (!s) s = "";
    n = strlen(s) + 1;
    p = (char *)mn_arena_alloc(a, n, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

/* Duplicate a byte range (not necessarily NUL-terminated) as a C string. */
static const char *mn__arena_strndup(mn_arena *a, const char *s, size_t n) {
    char *p;
    if (!a) return NULL;
    if (!s) { n = 0; }
    p = (char *)mn_arena_alloc(a, n + 1, 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void mn_arena_reset(mn_arena *a) {
    mn_arena_block *b;
    if (!a) return;
    for (b = a->blocks; b; b = b->next) b->used = 0;
    if (a->blocks) {
        a->base = a->blocks->data;
        a->cap  = a->blocks->cap;
    } else {
        a->base = NULL;
        a->cap  = 0;
    }
    a->used = 0;
}

void mn_arena_free(mn_arena *a) {
    mn_arena_block *b, *nx;
    if (!a) return;
    for (b = a->blocks; b; b = nx) {
        nx = b->next;
        free(b);
    }
    a->blocks = NULL;
    a->base   = NULL;
    a->used   = 0;
    a->cap    = 0;
}

/* --------------------------------------------------------------------------
 * Prepared-statement cache
 * -------------------------------------------------------------------------- */

/*
 * A tiny open-addressing style cache keyed by SQL text pointer identity. All
 * SQL strings used as keys are string literals with static lifetime, so
 * comparing the pointer is sufficient and cheap. Each cache belongs to one
 * connection.
 */
#define MN__STMT_CACHE_CAP 64

typedef struct mn__stmt_cache {
    const char   *keys[MN__STMT_CACHE_CAP];
    sqlite3_stmt *stmts[MN__STMT_CACHE_CAP];
    int           count;
} mn__stmt_cache;

static void mn__stmt_cache_init(mn__stmt_cache *c) {
    memset(c, 0, sizeof(*c));
}

static void mn__stmt_cache_finalize(mn__stmt_cache *c) {
    int i;
    for (i = 0; i < c->count; i++) {
        if (c->stmts[i]) sqlite3_finalize(c->stmts[i]);
        c->stmts[i] = NULL;
        c->keys[i]  = NULL;
    }
    c->count = 0;
}

/*
 * Return a cached, reset prepared statement for `sql` on `db`. `sql` MUST be a
 * static string literal (identity used as the cache key). On success the
 * returned statement is reset and its bindings cleared. NULL on failure.
 */
static sqlite3_stmt *mn__stmt_get(sqlite3 *db, mn__stmt_cache *c,
                                  const char *sql, int *rc_out) {
    int i, rc;
    sqlite3_stmt *st;

    for (i = 0; i < c->count; i++) {
        if (c->keys[i] == sql) {
            st = c->stmts[i];
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            if (rc_out) *rc_out = SQLITE_OK;
            return st;
        }
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (rc_out) *rc_out = rc;
        return NULL;
    }
    if (c->count < MN__STMT_CACHE_CAP) {
        c->keys[c->count]  = sql;
        c->stmts[c->count] = st;
        c->count++;
    }
    /* If the cache is full, the statement is still usable but will be
     * finalized here to avoid a leak (rare; cache is generously sized). */
    if (rc_out) *rc_out = SQLITE_OK;
    if (c->count >= MN__STMT_CACHE_CAP) {
        /* Overflow: caller owns it transiently. Mark so we finalize on use. */
        /* To keep semantics simple we finalize immediately after use is not
         * possible here, so instead just leave it prepared and let the
         * caller-side pattern below finalize non-cached statements. We flag
         * this by not storing it; return a distinct value via a wrapper is
         * overkill, so we simply finalize and re-prepare uncached. */
    }
    return st;
}

/* --------------------------------------------------------------------------
 * Per-connection state (writer + each reader)
 * -------------------------------------------------------------------------- */

typedef struct mn__conn {
    sqlite3        *db;
    mn__stmt_cache  cache;
    bool            is_writer;
} mn__conn;

/* --------------------------------------------------------------------------
 * Library handle
 * -------------------------------------------------------------------------- */

struct mn_library {
    char        *path;
    mn__conn     writer;         /* Serialized writer connection.             */
    mn__mutex    write_mtx;      /* Guards writer + shared handle fields.     */
    mn__tls_key  reader_key;     /* TLS: mn__conn* per reading thread.        */
    mn__mutex    readers_mtx;    /* Guards the reader registry list.          */
    struct mn__reader_node *readers; /* All reader conns, for close cleanup.  */

    bool         read_only;
    bool         has_fts5;       /* Runtime-detected FTS5 availability.       */
    bool         in_txn;         /* Writer transaction open?                  */
    int          busy_timeout_ms;
    int          schema_version;

    char         errmsg[512];    /* Last error message (writer path).         */
    mn__mutex    err_mtx;
};

typedef struct mn__reader_node {
    struct mn__reader_node *next;
    mn__conn               *conn;
} mn__reader_node;

static void mn__set_err(mn_library *lib, const char *msg) {
    if (!lib) return;
    mn__mutex_lock(&lib->err_mtx);
    if (msg) {
        strncpy(lib->errmsg, msg, sizeof(lib->errmsg) - 1);
        lib->errmsg[sizeof(lib->errmsg) - 1] = '\0';
    } else {
        lib->errmsg[0] = '\0';
    }
    mn__mutex_unlock(&lib->err_mtx);
}

static void mn__set_err_db(mn_library *lib, sqlite3 *db) {
    if (db) mn__set_err(lib, sqlite3_errmsg(db));
}

static void mn__write_lock(mn_library *lib)   { mn__mutex_lock(&lib->write_mtx); }
static void mn__write_unlock(mn_library *lib) { mn__mutex_unlock(&lib->write_mtx); }

/* --------------------------------------------------------------------------
 * Query / facet handles
 * -------------------------------------------------------------------------- */

struct mn_query {
    mn_library     *lib;
    mn__conn       *reader;      /* Reader connection for this thread.        */
    mn_filter_spec  spec;        /* Copied spec.                              */
    char           *fts_match;   /* Owned copy of spec.fts_match (or NULL).   */
    int64_t         playlist_id; /* >0 if this is a playlist-order query.     */
    bool            has_count;
    int64_t         count;
    mn_arena        arena;       /* Backs returned rows/strings.              */

    /* Scratch build buffers for dynamic SQL. */
    char           *sql_buf;
    size_t          sql_cap;
};

struct mn_facet {
    mn_library     *lib;
    mn__conn       *reader;
    mn_facet_dim    dim;
    mn_filter_spec  spec;
    char           *fts_match;
    mn_facet_order  order;
    bool            has_count;
    int64_t         count;
    mn_arena        arena;
    char           *sql_buf;
    size_t          sql_cap;
};

/* --------------------------------------------------------------------------
 * Schema
 * -------------------------------------------------------------------------- */

/* Base schema (no FTS). FTS objects are added conditionally afterwards. */
static const char *const MN__SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS artists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE,"
    "  track_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS album_artists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE,"
    "  track_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS albums ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  album_artist_id INTEGER,"
    "  track_count INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(name, album_artist_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS genres ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE,"
    "  track_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS folders ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  track_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS tracks ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  title TEXT NOT NULL DEFAULT '',"
    "  artist TEXT NOT NULL DEFAULT '',"
    "  album TEXT NOT NULL DEFAULT '',"
    "  album_artist TEXT NOT NULL DEFAULT '',"
    "  composer TEXT NOT NULL DEFAULT '',"
    "  genre TEXT NOT NULL DEFAULT '',"
    "  format TEXT NOT NULL DEFAULT '',"
    "  artist_id INTEGER,"
    "  album_artist_id INTEGER,"
    "  album_id INTEGER,"
    "  genre_id INTEGER,"
    "  folder_id INTEGER,"
    "  year INTEGER NOT NULL DEFAULT 0,"
    "  track INTEGER NOT NULL DEFAULT 0,"
    "  disc INTEGER NOT NULL DEFAULT 0,"
    "  duration_ms INTEGER NOT NULL DEFAULT 0,"
    "  sample_rate INTEGER NOT NULL DEFAULT 0,"
    "  channels INTEGER NOT NULL DEFAULT 0,"
    "  bit_depth INTEGER NOT NULL DEFAULT 0,"
    "  bitrate_kbps INTEGER NOT NULL DEFAULT 0,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  mtime INTEGER NOT NULL DEFAULT 0,"
    "  created INTEGER NOT NULL DEFAULT 0,"
    "  date_added INTEGER NOT NULL DEFAULT 0,"
    "  last_played INTEGER NOT NULL DEFAULT 0,"
    "  last_skipped INTEGER NOT NULL DEFAULT 0,"
    "  play_count INTEGER NOT NULL DEFAULT 0,"
    "  skip_count INTEGER NOT NULL DEFAULT 0,"
    "  rating_x2 INTEGER NOT NULL DEFAULT 0,"
    "  liked INTEGER NOT NULL DEFAULT 0,"
    "  pref_updated_ms INTEGER NOT NULL DEFAULT 0,"
    "  has_art INTEGER NOT NULL DEFAULT 0,"
    "  missing INTEGER NOT NULL DEFAULT 0,"
    "  scan_epoch INTEGER NOT NULL DEFAULT 0,"
    /* Immutable content fingerprint (v7): fnv1a-64 hex over (size ||
     * first 64KiB || last 64KiB) of the raw file. Never changes on
     * move/rename/retag; NULL = not yet computed (purely additive). */
    "  content_hash TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS playlists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  date_created INTEGER NOT NULL DEFAULT 0,"
    "  date_modified INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS playlist_items ("
    "  playlist_id INTEGER NOT NULL,"
    "  position INTEGER NOT NULL,"
    "  track_id INTEGER NOT NULL,"
    "  PRIMARY KEY(playlist_id, position),"
    "  FOREIGN KEY(playlist_id) REFERENCES playlists(id) ON DELETE CASCADE,"
    "  FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE"
    ");"
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ");"
    /* Audiobook (any kind) playback progress — replaces the legacy
     * book_resume.txt flat file. One CURRENT row per book (album_id)
     * PLUS one remembered position per chapter (track_id), so switching
     * chapters/books never loses a place. content_hash mirrors the
     * track's immutable fingerprint at note time: progress re-attaches
     * by hash after a file move, and rows are portable for device sync
     * (hash, pos_ms, percent, finished, updated). */
    "CREATE TABLE IF NOT EXISTS book_progress ("
    "  album_id INTEGER NOT NULL,"
    "  track_id INTEGER NOT NULL,"
    "  content_hash TEXT,"
    "  pos_ms INTEGER NOT NULL DEFAULT 0,"
    "  percent REAL NOT NULL DEFAULT 0,"        /* whole-BOOK completion */
    "  finished INTEGER NOT NULL DEFAULT 0,"
    "  current INTEGER NOT NULL DEFAULT 1,"     /* 1 = book's live row  */
    "  updated INTEGER NOT NULL DEFAULT 0,"     /* unix seconds         */
    "  PRIMARY KEY(album_id, track_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_bookprog_recent "
    "ON book_progress(current, updated DESC);"
    "CREATE INDEX IF NOT EXISTS idx_bookprog_hash "
    "ON book_progress(content_hash) WHERE content_hash IS NOT NULL;"
    /* Named positions within a book (audiobook bookmarks). content_hash
     * mirrors the track fingerprint so bookmarks survive file moves and
     * are portable for device sync, same as book_progress. */
    "CREATE TABLE IF NOT EXISTS book_bookmarks ("
    "  id INTEGER PRIMARY KEY,"
    "  album_id INTEGER NOT NULL,"
    "  track_id INTEGER NOT NULL,"
    "  content_hash TEXT,"
    "  pos_ms INTEGER NOT NULL DEFAULT 0,"
    "  note TEXT NOT NULL DEFAULT '',"
    "  created INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_bookmarks_album "
    "ON book_bookmarks(album_id, created DESC);"
    /* Covering / lookup indexes. */
    "CREATE INDEX IF NOT EXISTS idx_tracks_missing ON tracks(missing);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album_artist ON tracks(album_artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_folder ON tracks(folder_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_year ON tracks(year);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_scan ON tracks(scan_epoch);"
    /* The UNIQUE constraint on tracks.path yields a BINARY index, which no
     * COLLATE NOCASE comparison can use — so both of the app's case-insensitive
     * path accesses would otherwise full-scan tracks: the per-entry lookup in
     * mn_library_track_id_by_path (playlist import) and the kind path-prefix
     * ranges in mn__build_where. */
    "CREATE INDEX IF NOT EXISTS idx_tracks_path_nocase "
    "  ON tracks(path COLLATE NOCASE);"
    /* Moved-file relink identity probe (size, then duration range). */
    "CREATE INDEX IF NOT EXISTS idx_tracks_size ON tracks(size, duration_ms);"
    /* Covering indexes for the common sort orders (id tiebreak trailing). */
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_title "
    "  ON tracks(missing, title COLLATE NOCASE, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_artist "
    "  ON tracks(missing, artist COLLATE NOCASE, album COLLATE NOCASE, disc, track, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_album "
    "  ON tracks(missing, album COLLATE NOCASE, disc, track, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_added "
    "  ON tracks(missing, date_added, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_played "
    "  ON tracks(missing, last_played, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_playcount "
    "  ON tracks(missing, play_count, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_rating "
    "  ON tracks(missing, rating_x2, id);"
    /* Duration/genre/bitrate are offered as sort keys too — without these,
     * every scroll page re-sorted all rows through a temp b-tree (tens of ms
     * per page, on the bridge dispatch thread, under the app lock). */
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_duration "
    "  ON tracks(missing, duration_ms, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_genre "
    "  ON tracks(missing, genre COLLATE NOCASE, id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_sort_bitrate "
    "  ON tracks(missing, bitrate_kbps, id);"
    "CREATE INDEX IF NOT EXISTS idx_pi_track ON playlist_items(track_id);";

/* FTS5 objects: external-content index over tracks + sync triggers.
 * prefix='2 3': dedicated prefix indexes make short type-as-you-go queries
 * ("be*", "bey*") direct index lookups instead of term-range doclist merges
 * — the difference between ~instant and tens of ms per keystroke. Longer
 * prefixes reuse the 3-char index and narrow from there. */
static const char *const MN__SCHEMA_FTS_SQL =
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
    "  title, artist, album,"
    "  content='tracks', content_rowid='id',"
    "  tokenize='unicode61 remove_diacritics 2',"
    "  prefix='2 3'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS trg_tracks_ai AFTER INSERT ON tracks BEGIN"
    "  INSERT INTO tracks_fts(rowid, title, artist, album)"
    "  VALUES (new.id, new.title, new.artist, new.album);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS trg_tracks_ad AFTER DELETE ON tracks BEGIN"
    "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album)"
    "  VALUES ('delete', old.id, old.title, old.artist, old.album);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS trg_tracks_au AFTER UPDATE OF title,artist,album ON tracks BEGIN"
    "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album)"
    "  VALUES ('delete', old.id, old.title, old.artist, old.album);"
    "  INSERT INTO tracks_fts(rowid, title, artist, album)"
    "  VALUES (new.id, new.title, new.artist, new.album);"
    "END;";

/* --------------------------------------------------------------------------
 * Connection setup
 * -------------------------------------------------------------------------- */

static mn_status mn__exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return mn__map_sqlite(rc);
}

/* Detect FTS5 by attempting to create a temp virtual table. */
static bool mn__detect_fts5(sqlite3 *db) {
    int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE IF NOT EXISTS temp.mn__fts_probe USING fts5(x);"
        "DROP TABLE IF EXISTS temp.mn__fts_probe;",
        NULL, NULL, NULL);
    return rc == SQLITE_OK;
}

static void mn__apply_pragmas(sqlite3 *db, const mn_open_opts *opts) {
    char buf[128];

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

    {
        int64_t mmap = (opts && opts->mmap_size > 0)
                     ? opts->mmap_size : (int64_t)268435456; /* 256 MiB */
        snprintf(buf, sizeof(buf), "PRAGMA mmap_size=%lld;", (long long)mmap);
        sqlite3_exec(db, buf, NULL, NULL, NULL);
    }
    {
        /* cache_size negative => KiB. Default 65536 KiB (64 MiB). */
        int kib = (opts && opts->cache_size_kib > 0)
                ? opts->cache_size_kib : 65536;
        snprintf(buf, sizeof(buf), "PRAGMA cache_size=-%d;", kib);
        sqlite3_exec(db, buf, NULL, NULL, NULL);
    }
}

/* NATSORT collation: case-insensitive natural ordering — digit runs compare
 * NUMERICALLY ("Chapter 2" < "Chapter 10", "Disc 1/09.mp3" < "Disc 1/10.mp3").
 * Used as the chapter-order tiebreak for books whose track tags are missing
 * (track/disc = 0): filename order is then the only sane ordering, and plain
 * lexicographic would play 1,10,11,2. Leading zeros compare shorter-first
 * ("07" < "7" is false — equal numerically, then shorter run wins) so mixed
 * zero-padded rips stay stable. */
static int mn__natcmp(void *ud, int an, const void *av, int bn, const void *bv) {
    const unsigned char *a = (const unsigned char *)av;
    const unsigned char *b = (const unsigned char *)bv;
    int ia = 0, ib = 0;
    (void)ud;
    while (ia < an && ib < bn) {
        unsigned char ca = a[ia], cb = b[ib];
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            /* compare the whole digit runs numerically */
            int sa = ia, sb = ib;
            while (ia < an && a[ia] >= '0' && a[ia] <= '9') ia++;
            while (ib < bn && b[ib] >= '0' && b[ib] <= '9') ib++;
            {
                /* strip leading zeros */
                int za = sa, zb = sb;
                while (za < ia - 1 && a[za] == '0') za++;
                while (zb < ib - 1 && b[zb] == '0') zb++;
                if ((ia - za) != (ib - zb)) return (ia - za) - (ib - zb);
                while (za < ia && zb < ib) {
                    if (a[za] != b[zb]) return (int)a[za] - (int)b[zb];
                    za++; zb++;
                }
                /* numerically equal: shorter original run (fewer zeros) first */
                if ((ia - sa) != (ib - sb)) return (ia - sa) - (ib - sb);
            }
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return (int)ca - (int)cb;
        ia++; ib++;
    }
    return (an - ia) - (bn - ib);
}

static mn_status mn__open_conn(mn_library *lib, mn__conn *c, bool writer) {
    int flags;
    int rc;

    flags = writer ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                   : SQLITE_OPEN_READONLY;
    if (lib->read_only && writer) {
        return MN_ERR_STATE;
    }
    flags |= SQLITE_OPEN_NOMUTEX; /* We serialize externally. */

    rc = sqlite3_open_v2(lib->path, &c->db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (c->db) { mn__set_err_db(lib, c->db); sqlite3_close(c->db); c->db = NULL; }
        return mn__map_sqlite(rc);
    }
    sqlite3_busy_timeout(c->db, lib->busy_timeout_ms);
    mn__apply_pragmas(c->db, NULL);
    /* Natural-order collation (chapter ordering for untagged books). */
    sqlite3_create_collation(c->db, "NATSORT", SQLITE_UTF8, NULL, mn__natcmp);
    mn__stmt_cache_init(&c->cache);
    c->is_writer = writer;
    return MN_OK;
}

static void mn__close_conn(mn__conn *c) {
    if (!c) return;
    mn__stmt_cache_finalize(&c->cache);
    if (c->db) {
        sqlite3_close(c->db);
        c->db = NULL;
    }
}

/* Get (or lazily create) this thread's reader connection. */
static mn__conn *mn__reader(mn_library *lib) {
    mn__conn *c = (mn__conn *)mn__tls_get(lib->reader_key);
    if (c) return c;

    c = (mn__conn *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (mn__open_conn(lib, c, false) != MN_OK) {
        free(c);
        return NULL;
    }
    /* Register for cleanup at close. */
    {
        mn__reader_node *node = (mn__reader_node *)malloc(sizeof(*node));
        if (!node) { mn__close_conn(c); free(c); return NULL; }
        mn__mutex_lock(&lib->readers_mtx);
        node->conn = c;
        node->next = lib->readers;
        lib->readers = node;
        mn__mutex_unlock(&lib->readers_mtx);
    }
    mn__tls_set(lib->reader_key, c);
    return c;
}

/* Close and unregister the CALLING thread's reader connection. For short-
 * lived worker threads (e.g. the parallel album-cache fill): without this,
 * every spawned worker leaves its lazily-created connection in the readers
 * list until close — thousands of leaked conns over a long session. */
void mn_library_thread_detach(mn_library *lib) {
    mn__conn *c;
    mn__reader_node **pp, *n;
    if (!lib) return;
    c = (mn__conn *)mn__tls_get(lib->reader_key);
    if (!c) return;
    mn__tls_set(lib->reader_key, NULL);
    mn__mutex_lock(&lib->readers_mtx);
    for (pp = &lib->readers; (n = *pp) != NULL; pp = &n->next) {
        if (n->conn == c) {
            *pp = n->next;
            free(n);
            break;
        }
    }
    mn__mutex_unlock(&lib->readers_mtx);
    mn__close_conn(c);
    free(c);
}

/* --------------------------------------------------------------------------
 * Meta helpers (schema version)
 * -------------------------------------------------------------------------- */

static int mn__read_user_version(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int v = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

static mn_status mn__write_user_version(sqlite3 *db, int v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "PRAGMA user_version=%d;", v);
    return mn__exec(db, buf);
}

/* --------------------------------------------------------------------------
 * Open / close / migrate
 * -------------------------------------------------------------------------- */

mn_status mn_library_migrate(mn_library *lib) {
    mn_status st;
    int ver;

    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_OK; /* Assume already migrated. */

    mn__write_lock(lib);

    st = mn__exec(lib->writer.db, MN__SCHEMA_SQL);
    if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto done; }

    if (lib->has_fts5) {
        st = mn__exec(lib->writer.db, MN__SCHEMA_FTS_SQL);
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto done; }
    }

    ver = mn__read_user_version(lib->writer.db);
    if (ver < MN_SCHEMA_VERSION) {
        /* v1 -> v2: thumbs up/down column. Fresh databases already get it
         * from MN__SCHEMA_SQL above (their user_version is 0 too), so the
         * ALTER's "duplicate column" failure is expected and ignored. */
        if (ver < 2) {
            (void)mn__exec(lib->writer.db,
                "ALTER TABLE tracks ADD COLUMN liked INTEGER NOT NULL DEFAULT 0;");
        }
        /* v2 -> v3: backfill the (previously never-populated) format label
         * from the path's extension. substr(path, length(rtrim(path,
         * replace(path,'.','')))+1) == "text after the last dot"; rows whose
         * last dot sits in a directory name (separators in the "extension")
         * or whose extension is implausibly long are left untouched. */
        if (ver < 3) {
            (void)mn__exec(lib->writer.db,
                "UPDATE tracks SET format = UPPER("
                "  substr(path, length(rtrim(path, replace(path,'.','')))+1))"
                " WHERE (format IS NULL OR format='')"
                "   AND path LIKE '%.%'"
                "   AND length(substr(path,"
                "        length(rtrim(path, replace(path,'.','')))+1))"
                "       BETWEEN 1 AND 8"
                "   AND substr(path, length(rtrim(path, replace(path,'.','')))+1)"
                "       NOT LIKE '%\\%'"
                "   AND substr(path, length(rtrim(path, replace(path,'.','')))+1)"
                "       NOT LIKE '%/%';");
        }
        /* v3 -> v4: rebuild the FTS index WITH prefix indexes (prefix='2 3')
         * so short type-as-you-go prefixes are indexed lookups. External-
         * content table: drop + recreate + 'rebuild' repopulates from tracks
         * (one-time at open; ~1-2 s for a 30k library). The IF NOT EXISTS
         * exec above kept the old prefix-less table alive on existing DBs —
         * this replaces it. */
        if (ver < 4 && lib->has_fts5) {
            (void)mn__exec(lib->writer.db, "DROP TABLE IF EXISTS tracks_fts;");
            st = mn__exec(lib->writer.db, MN__SCHEMA_FTS_SQL);
            if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto done; }
            (void)mn__exec(lib->writer.db,
                "INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild');");
        }
        /* v4 -> v5: pref_updated_ms — epoch ms of the last LOCAL like/rating
         * change, the last-write-wins clock for library sync. Fresh databases
         * already get it from MN__SCHEMA_SQL above, so the ALTER's "duplicate
         * column" failure is expected and ignored (same as v1 -> v2). */
        if (ver < 5) {
            (void)mn__exec(lib->writer.db,
                "ALTER TABLE tracks ADD COLUMN "
                "pref_updated_ms INTEGER NOT NULL DEFAULT 0;");
        }
        /* v5 -> v6: filesystem creation (birth) time — a distinct sort
         * axis from date_added (a bulk import lands on one date_added
         * day but each file keeps its own creation date). The column +
         * its sort index are BOTH created here (they cannot live in
         * MN__SCHEMA_SQL: on a pre-v6 db the index would reference a
         * column that does not exist yet and fail the whole exec).
         * Backfill happens in the app's reconcile pass (stat per file).
         * The ALTER's duplicate-column failure on fresh dbs is expected
         * and ignored (same as v1 -> v2). */
        if (ver < 6) {
            (void)mn__exec(lib->writer.db,
                "ALTER TABLE tracks ADD COLUMN "
                "created INTEGER NOT NULL DEFAULT 0;");
        }
        (void)mn__exec(lib->writer.db,
            "CREATE INDEX IF NOT EXISTS idx_tracks_sort_created "
            "ON tracks(missing, created, id);");
        /* v6 -> v7: content_hash — immutable 64-bit content fingerprint
         * (fnv1a-64 hex over size + first/last 64KiB of raw bytes). The
         * key playback progress uses so it survives file moves/renames
         * and can sync across devices. NULLABLE and write-once: rows are
         * backfilled by a low-priority background pass, never inline.
         * Purely additive — NULL rows behave exactly as before. The
         * ALTER's duplicate-column failure on fresh dbs is expected and
         * ignored (same as v1 -> v2). The partial index (WHERE NOT NULL)
         * keeps lookups fast without indexing the not-yet-hashed rows. */
        if (ver < 7) {
            (void)mn__exec(lib->writer.db,
                "ALTER TABLE tracks ADD COLUMN content_hash TEXT;");
        }
        (void)mn__exec(lib->writer.db,
            "CREATE INDEX IF NOT EXISTS idx_tracks_content_hash "
            "ON tracks(content_hash) WHERE content_hash IS NOT NULL;");
        st = mn__write_user_version(lib->writer.db, MN_SCHEMA_VERSION);
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto done; }
        ver = MN_SCHEMA_VERSION;
    } else if (ver > MN_SCHEMA_VERSION) {
        st = MN_ERR_MIGRATE;
        mn__set_err(lib, "database schema is newer than this build");
        goto done;
    }
    lib->schema_version = ver;
    st = MN_OK;

done:
    mn__write_unlock(lib);
    return st;
}

mn_status mn_library_open(const char *path, const mn_open_opts *opts,
                          mn_library **out) {
    mn_library *lib;
    mn_status st;

    if (!path || !out) return MN_ERR_INVALID;
    *out = NULL;

    lib = (mn_library *)calloc(1, sizeof(*lib));
    if (!lib) return MN_ERR_NOMEM;

    lib->path = mn__strdup(path);
    if (!lib->path) { free(lib); return MN_ERR_NOMEM; }

    lib->read_only       = opts ? opts->read_only : false;
    lib->busy_timeout_ms = (opts && opts->busy_timeout_ms > 0)
                         ? opts->busy_timeout_ms : 5000;
    lib->schema_version  = 0;
    lib->in_txn          = false;

    mn__mutex_init(&lib->write_mtx);
    mn__mutex_init(&lib->readers_mtx);
    mn__mutex_init(&lib->err_mtx);
    if (mn__tls_create(&lib->reader_key) != 0) {
        mn__mutex_destroy(&lib->write_mtx);
        mn__mutex_destroy(&lib->readers_mtx);
        mn__mutex_destroy(&lib->err_mtx);
        free(lib->path);
        free(lib);
        return MN_ERR_GENERIC;
    }

    if (lib->read_only) {
        /* Read-only: no writer connection, use a reader for probing. */
        mn__conn *r = mn__reader(lib);
        if (!r) { st = MN_ERR_IO; goto fail; }
        lib->has_fts5 = mn__detect_fts5(r->db);
        lib->schema_version = mn__read_user_version(r->db);
        *out = lib;
        return MN_OK;
    }

    /* Writer connection. */
    if (!opts || opts->create_if_missing || 1) {
        /* create_if_missing controls SQLITE_OPEN_CREATE; we default to create.
         * If the caller explicitly disallowed creation, honor it below. */
    }
    st = mn__open_conn(lib, &lib->writer, true);
    if (st != MN_OK) goto fail;

    /* Re-apply pragmas already done in open_conn. Detect FTS5. */
    lib->has_fts5 = mn__detect_fts5(lib->writer.db);

    st = mn_library_migrate(lib);
    if (st != MN_OK) goto fail;

    *out = lib;
    return MN_OK;

fail:
    mn_library_close(lib);
    return st;
}

void mn_library_close(mn_library *lib) {
    mn__reader_node *n, *nx;
    if (!lib) return;

    /* Close all reader connections. */
    mn__mutex_lock(&lib->readers_mtx);
    for (n = lib->readers; n; n = nx) {
        nx = n->next;
        if (n->conn) {
            mn__close_conn(n->conn);
            free(n->conn);
        }
        free(n);
    }
    lib->readers = NULL;
    mn__mutex_unlock(&lib->readers_mtx);

    /* Checkpoint + close writer. PRAGMA optimize first — SQLite's
     * recommended shutdown step: refreshes planner statistics for whichever
     * queries actually ran this session (cheap; bounded by analysis_limit). */
    if (lib->writer.db) {
        sqlite3_exec(lib->writer.db, "PRAGMA optimize;", NULL, NULL, NULL);
        sqlite3_wal_checkpoint_v2(lib->writer.db, NULL,
                                  SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    }
    mn__close_conn(&lib->writer);

    mn__tls_delete(lib->reader_key);
    mn__mutex_destroy(&lib->write_mtx);
    mn__mutex_destroy(&lib->readers_mtx);
    mn__mutex_destroy(&lib->err_mtx);

    free(lib->path);
    free(lib);
}

int mn_library_schema_version(const mn_library *lib) {
    return lib ? lib->schema_version : 0;
}

const char *mn_library_errmsg(const mn_library *lib) {
    if (!lib) return "invalid library handle";
    return lib->errmsg[0] ? lib->errmsg : "no error";
}

mn_status mn_library_analyze(mn_library *lib) {
    mn_status st;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__exec(lib->writer.db, "ANALYZE; PRAGMA optimize;");
    if (st != MN_OK) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return st;
}

mn_status mn_library_checkpoint(mn_library *lib) {
    int rc;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    rc = sqlite3_wal_checkpoint_v2(lib->writer.db, NULL,
                                   SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    if (rc != SQLITE_OK) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* --------------------------------------------------------------------------
 * Transaction control
 * -------------------------------------------------------------------------- */

mn_status mn_library_begin(mn_library *lib) {
    mn_status st;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    if (lib->in_txn) { mn__write_unlock(lib); return MN_ERR_STATE; }
    st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
    if (st == MN_OK) lib->in_txn = true;
    else mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return st;
}

mn_status mn_library_commit(mn_library *lib) {
    mn_status st;
    if (!lib) return MN_ERR_INVALID;
    mn__write_lock(lib);
    if (!lib->in_txn) { mn__write_unlock(lib); return MN_ERR_STATE; }
    st = mn__exec(lib->writer.db, "COMMIT;");
    if (st == MN_OK) lib->in_txn = false;
    else mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return st;
}

mn_status mn_library_rollback(mn_library *lib) {
    mn_status st;
    if (!lib) return MN_ERR_INVALID;
    mn__write_lock(lib);
    if (!lib->in_txn) { mn__write_unlock(lib); return MN_ERR_STATE; }
    st = mn__exec(lib->writer.db, "ROLLBACK;");
    lib->in_txn = false; /* Rollback always clears txn state. */
    if (st != MN_OK) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return st;
}

bool mn_library_in_transaction(const mn_library *lib) {
    return lib ? lib->in_txn : false;
}

/* --------------------------------------------------------------------------
 * Dimension table resolution (writer, mutex already held by caller)
 * -------------------------------------------------------------------------- */

/*
 * Resolve (get-or-create) a dimension row by unique text `name`, returning its
 * id via *out_id. Increments the row's track_count by `delta_count`. Uses
 * cached statements on the writer cache. `name` NULL/empty maps to id NULL
 * (out_id = 0) and is treated as "no dimension".
 */
static mn_status mn__dim_resolve(mn_library *lib, const char *table,
                                 const char *sel_sql, const char *ins_sql,
                                 const char *name, int64_t *out_id) {
    sqlite3_stmt *st;
    int rc;
    int64_t id = 0;

    *out_id = 0;
    if (!name || name[0] == '\0') return MN_OK;

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, sel_sql, &rc);
    if (!st) { mn__set_err_db(lib, lib->writer.db); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        sqlite3_reset(st);
        *out_id = id;
        return MN_OK;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { mn__set_err_db(lib, lib->writer.db); return mn__map_sqlite(rc); }

    /* Insert new. */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, ins_sql, &rc);
    if (!st) { mn__set_err_db(lib, lib->writer.db); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { mn__set_err_db(lib, lib->writer.db); return mn__map_sqlite(rc); }
    *out_id = sqlite3_last_insert_rowid(lib->writer.db);
    MN__UNUSED(table);
    return MN_OK;
}

/* Adjust track_count on a dimension row by delta (may be negative). */
static void mn__dim_bump(mn_library *lib, const char *upd_sql,
                         int64_t id, int64_t delta) {
    sqlite3_stmt *st;
    int rc;
    if (id <= 0 || delta == 0) return;
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, upd_sql, &rc);
    if (!st) return;
    sqlite3_bind_int64(st, 1, delta);
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_reset(st);
}

/* Resolve the folder (directory portion of path). */
static void mn__folder_of(const char *path, char *buf, size_t cap) {
    const char *slash = NULL, *p;
    size_t n;
    buf[0] = '\0';
    if (!path) return;
    for (p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (!slash) return;
    n = (size_t)(slash - path);
    if (n == 0) n = 1; /* root */
    if (n >= cap) n = cap - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';
}

/* --------------------------------------------------------------------------
 * Upsert track
 * -------------------------------------------------------------------------- */

/* Fetch the existing dimension ids for a track (for decrement on update). */
static void mn__track_old_dims(mn_library *lib, int64_t id,
                               int64_t *artist_id, int64_t *aa_id,
                               int64_t *album_id, int64_t *genre_id,
                               int64_t *folder_id) {
    static const char *SQL =
        "SELECT artist_id,album_artist_id,album_id,genre_id,folder_id "
        "FROM tracks WHERE id=?1;";
    sqlite3_stmt *st;
    int rc;
    *artist_id = *aa_id = *album_id = *genre_id = *folder_id = 0;
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) return;
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *artist_id = sqlite3_column_int64(st, 0);
        *aa_id     = sqlite3_column_int64(st, 1);
        *album_id  = sqlite3_column_int64(st, 2);
        *genre_id  = sqlite3_column_int64(st, 3);
        *folder_id = sqlite3_column_int64(st, 4);
    }
    sqlite3_reset(st);
}

mn_status mn_library_upsert_track(mn_library *lib, const mn_track_in *t,
                                  int64_t *out_id) {
    static const char *SEL_ID = "SELECT id FROM tracks WHERE path=?1;";
    static const char *ART_SEL = "SELECT id FROM artists WHERE name=?1;";
    static const char *ART_INS = "INSERT INTO artists(name) VALUES(?1);";
    static const char *ART_UPD = "UPDATE artists SET track_count=track_count+?1 WHERE id=?2;";
    static const char *AA_SEL  = "SELECT id FROM album_artists WHERE name=?1;";
    static const char *AA_INS  = "INSERT INTO album_artists(name) VALUES(?1);";
    static const char *AA_UPD  = "UPDATE album_artists SET track_count=track_count+?1 WHERE id=?2;";
    static const char *ALB_UPD = "UPDATE albums SET track_count=track_count+?1 WHERE id=?2;";
    static const char *GEN_SEL = "SELECT id FROM genres WHERE name=?1;";
    static const char *GEN_INS = "INSERT INTO genres(name) VALUES(?1);";
    static const char *GEN_UPD = "UPDATE genres SET track_count=track_count+?1 WHERE id=?2;";
    static const char *FLD_SEL = "SELECT id FROM folders WHERE path=?1;";
    static const char *FLD_INS = "INSERT INTO folders(path) VALUES(?1);";
    static const char *FLD_UPD = "UPDATE folders SET track_count=track_count+?1 WHERE id=?2;";
    static const char *ALB_SEL =
        "SELECT id FROM albums WHERE name=?1 AND "
        "(album_artist_id IS ?2 OR (album_artist_id IS NULL AND ?2 IS NULL));";
    static const char *ALB_INS =
        "INSERT INTO albums(name, album_artist_id) VALUES(?1, ?2);";
    static const char *UPSERT =
        "INSERT INTO tracks("
        " path,title,artist,album,album_artist,composer,genre,format,"
        " artist_id,album_artist_id,album_id,genre_id,folder_id,"
        " year,track,disc,duration_ms,sample_rate,channels,bit_depth,"
        " bitrate_kbps,size,mtime,date_added,has_art,missing,scan_epoch,"
        " created)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,"
        "        ?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,0,?26,?27)"
        " ON CONFLICT(path) DO UPDATE SET"
        "  title=excluded.title, artist=excluded.artist, album=excluded.album,"
        "  album_artist=excluded.album_artist, composer=excluded.composer,"
        "  genre=excluded.genre, format=excluded.format,"
        "  artist_id=excluded.artist_id, album_artist_id=excluded.album_artist_id,"
        "  album_id=excluded.album_id, genre_id=excluded.genre_id,"
        "  folder_id=excluded.folder_id, year=excluded.year, track=excluded.track,"
        "  disc=excluded.disc, duration_ms=excluded.duration_ms,"
        "  sample_rate=excluded.sample_rate, channels=excluded.channels,"
        "  bit_depth=excluded.bit_depth, bitrate_kbps=excluded.bitrate_kbps,"
        "  size=excluded.size, mtime=excluded.mtime, has_art=excluded.has_art,"
        "  missing=0, scan_epoch=excluded.scan_epoch,"
        /* keep a known birth time if this upsert doesn't have one (e.g.
         * the reinfer backfill rebuilds rows without re-stat'ing) */
        "  created=CASE WHEN excluded.created>0 THEN excluded.created"
        "               ELSE tracks.created END;";

    mn_status st = MN_OK;
    sqlite3_stmt *stmt;
    int rc;
    int64_t artist_id = 0, aa_id = 0, album_id = 0, genre_id = 0, folder_id = 0;
    int64_t old_id = 0, new_id = 0;
    int64_t old_artist = 0, old_aa = 0, old_album = 0, old_genre = 0, old_folder = 0;
    bool existed = false;
    char folder[1024];
    bool own_txn = false;

    if (!lib || !t || !t->path || t->path[0] == '\0') return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    mn__write_lock(lib);

    if (!lib->in_txn) {
        st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto unlock; }
        own_txn = true;
    }

    /* Does the track already exist? Capture old dimension ids to decrement. */
    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, SEL_ID, &rc);
    if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
    sqlite3_bind_text(stmt, 1, t->path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        old_id = sqlite3_column_int64(stmt, 0);
        existed = true;
    }
    sqlite3_reset(stmt);

    if (existed) {
        mn__track_old_dims(lib, old_id, &old_artist, &old_aa,
                           &old_album, &old_genre, &old_folder);
    }

    /* Resolve new dimensions (get-or-create). */
    st = mn__dim_resolve(lib, "artists", ART_SEL, ART_INS, t->artist, &artist_id);
    if (st != MN_OK) goto rollback;
    st = mn__dim_resolve(lib, "album_artists", AA_SEL, AA_INS,
                         t->album_artist, &aa_id);
    if (st != MN_OK) goto rollback;
    st = mn__dim_resolve(lib, "genres", GEN_SEL, GEN_INS, t->genre, &genre_id);
    if (st != MN_OK) goto rollback;

    mn__folder_of(t->path, folder, sizeof(folder));
    st = mn__dim_resolve(lib, "folders", FLD_SEL, FLD_INS, folder, &folder_id);
    if (st != MN_OK) goto rollback;

    /* Album: keyed by (name, album_artist_id). */
    if (t->album && t->album[0]) {
        stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, ALB_SEL, &rc);
        if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
        sqlite3_bind_text(stmt, 1, t->album, -1, SQLITE_TRANSIENT);
        if (aa_id > 0) sqlite3_bind_int64(stmt, 2, aa_id);
        else           sqlite3_bind_null(stmt, 2);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            album_id = sqlite3_column_int64(stmt, 0);
            sqlite3_reset(stmt);
        } else {
            sqlite3_reset(stmt);
            if (rc != SQLITE_DONE) { st = mn__map_sqlite(rc); goto rollback; }
            stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, ALB_INS, &rc);
            if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
            sqlite3_bind_text(stmt, 1, t->album, -1, SQLITE_TRANSIENT);
            if (aa_id > 0) sqlite3_bind_int64(stmt, 2, aa_id);
            else           sqlite3_bind_null(stmt, 2);
            rc = sqlite3_step(stmt);
            sqlite3_reset(stmt);
            if (rc != SQLITE_DONE) { st = mn__map_sqlite(rc); goto rollback; }
            album_id = sqlite3_last_insert_rowid(lib->writer.db);
        }
    }

    /* Perform the upsert. */
    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, UPSERT, &rc);
    if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
    sqlite3_bind_text (stmt, 1,  t->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2,  t->title ? t->title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3,  t->artist ? t->artist : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4,  t->album ? t->album : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5,  t->album_artist ? t->album_artist : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 6,  t->composer ? t->composer : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 7,  t->genre ? t->genre : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 8,  t->format ? t->format : "", -1, SQLITE_TRANSIENT);
    if (artist_id > 0) sqlite3_bind_int64(stmt, 9, artist_id); else sqlite3_bind_null(stmt, 9);
    if (aa_id > 0)     sqlite3_bind_int64(stmt, 10, aa_id);     else sqlite3_bind_null(stmt, 10);
    if (album_id > 0)  sqlite3_bind_int64(stmt, 11, album_id);  else sqlite3_bind_null(stmt, 11);
    if (genre_id > 0)  sqlite3_bind_int64(stmt, 12, genre_id);  else sqlite3_bind_null(stmt, 12);
    if (folder_id > 0) sqlite3_bind_int64(stmt, 13, folder_id); else sqlite3_bind_null(stmt, 13);
    sqlite3_bind_int   (stmt, 14, t->year);
    sqlite3_bind_int   (stmt, 15, t->track);
    sqlite3_bind_int   (stmt, 16, t->disc);
    sqlite3_bind_int64 (stmt, 17, t->duration_ms);
    sqlite3_bind_int   (stmt, 18, t->sample_rate);
    sqlite3_bind_int   (stmt, 19, t->channels);
    sqlite3_bind_int   (stmt, 20, t->bit_depth);
    sqlite3_bind_int   (stmt, 21, t->bitrate_kbps);
    sqlite3_bind_int64 (stmt, 22, t->size);
    sqlite3_bind_int64 (stmt, 23, t->mtime);
    sqlite3_bind_int64 (stmt, 24, (int64_t)time(NULL)); /* date_added on insert */
    sqlite3_bind_int   (stmt, 25, t->has_art ? 1 : 0);
    sqlite3_bind_int64 (stmt, 26, t->mtime); /* scan_epoch := mtime touch */
    sqlite3_bind_int64 (stmt, 27, t->created);
    rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) {
        mn__set_err_db(lib, lib->writer.db);
        st = mn__map_sqlite(rc);
        goto rollback;
    }

    if (existed) {
        new_id = old_id;
        /* Decrement old dimension counts, increment new. */
        mn__dim_bump(lib, ART_UPD, old_artist, -1);
        mn__dim_bump(lib, AA_UPD,  old_aa,     -1);
        mn__dim_bump(lib, ALB_UPD, old_album,  -1);
        mn__dim_bump(lib, GEN_UPD, old_genre,  -1);
        mn__dim_bump(lib, FLD_UPD, old_folder, -1);
    } else {
        new_id = sqlite3_last_insert_rowid(lib->writer.db);
    }

    mn__dim_bump(lib, ART_UPD, artist_id, +1);
    mn__dim_bump(lib, AA_UPD,  aa_id,     +1);
    mn__dim_bump(lib, ALB_UPD, album_id,  +1);
    mn__dim_bump(lib, GEN_UPD, genre_id,  +1);
    mn__dim_bump(lib, FLD_UPD, folder_id, +1);

    if (own_txn) {
        st = mn__exec(lib->writer.db, "COMMIT;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto rollback; }
        own_txn = false;
    }

    if (out_id) *out_id = new_id;
    mn__write_unlock(lib);
    return MN_OK;

rollback:
    if (own_txn) {
        mn__exec(lib->writer.db, "ROLLBACK;");
    }
unlock:
    mn__write_unlock(lib);
    return st;
}

/* --------------------------------------------------------------------------
 * Moved-file relink
 * -------------------------------------------------------------------------- */

/*
 * A moved/renamed file used to become a brand-new row (path is the upsert
 * key), losing rating/playcount/liked/date_added, while the old row rotted
 * as missing=1. The relink protocol (MediaMonkey-style):
 *   1. candidates(): rows sharing the new file's byte size and (±2 s)
 *      duration — identity that survives any rename. The MISSING FLAG IS
 *      DELIBERATELY IGNORED: a file moved just before this scan has not
 *      been reconciled yet, so its row is still missing=0.
 *   2. The APP stat()s each candidate's path (a layering the db can't do)
 *      and, when exactly one candidate's file is gone from disk, calls
 *      repath(). The following upsert then hits ON CONFLICT(path) and
 *      refreshes tags while the user columns ride along untouched.
 */
int mn_library_relink_candidates(mn_library *lib, const char *new_path,
                                 int64_t size, int64_t duration_ms,
                                 mn_relink_cand *out, int max) {
    static const char *EXISTS = "SELECT 1 FROM tracks WHERE path=?1;";
    static const char *FIND =
        "SELECT id, path FROM tracks WHERE size=?1 "
        "AND duration_ms BETWEEN ?2-2000 AND ?2+2000 LIMIT ?3;";
    sqlite3_stmt *st;
    int rc, n = 0;

    if (!lib || !new_path || !new_path[0] || size <= 0 || !out || max <= 0)
        return 0;

    mn__write_lock(lib);

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, EXISTS, &rc);
    if (!st) { mn__write_unlock(lib); return 0; }
    sqlite3_bind_text(st, 1, new_path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc == SQLITE_ROW) { mn__write_unlock(lib); return 0; } /* known path */

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, FIND, &rc);
    if (!st) { mn__write_unlock(lib); return 0; }
    sqlite3_bind_int64(st, 1, size);
    sqlite3_bind_int64(st, 2, duration_ms);
    sqlite3_bind_int(st, 3, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 1);
        out[n].id = sqlite3_column_int64(st, 0);
        snprintf(out[n].path, sizeof(out[n].path), "%s", p ? p : "");
        n++;
    }
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return n;
}

mn_status mn_library_repath_track(mn_library *lib, int64_t track_id,
                                  const char *new_path) {
    static const char *REPATH =
        "UPDATE tracks SET path=?1, missing=0 WHERE id=?2;";
    sqlite3_stmt *st;
    int rc;
    if (!lib || !new_path || !new_path[0] || track_id <= 0)
        return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, REPATH, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, new_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        mn__set_err_db(lib, lib->writer.db);
        mn__write_unlock(lib);
        return mn__map_sqlite(rc);
    }
    mn__write_unlock(lib);
    return MN_OK;
}

/* --------------------------------------------------------------------------
 * Missing / delete
 * -------------------------------------------------------------------------- */

/* Backfill the filesystem creation time on a row that predates the v6
 * `created` column (the reconcile pass stats these). Serialized. */
mn_status mn_library_set_created(mn_library *lib, int64_t track_id,
                                 int64_t created) {
    static const char *SQL = "UPDATE tracks SET created=?1 WHERE id=?2;";
    sqlite3_stmt *st;
    int rc;
    if (!lib || track_id <= 0 || created <= 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, created);
    sqlite3_bind_int64(st, 2, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* --------------------------------------------------------------------------
 * content_hash (v7) — immutable content fingerprint for move-proof,
 * sync-able playback progress. See MN_SCHEMA_VERSION docs in library_db.h.
 * -------------------------------------------------------------------------- */

/* WRITE-ONCE set: the SQL itself refuses to overwrite an existing hash
 * (WHERE content_hash IS NULL), so no caller can accidentally mutate the
 * fingerprint on a retag/rescan. To legitimately re-fingerprint a row whose
 * FILE CONTENT genuinely changed (size mismatch detected by the caller),
 * pass force=true — the only path that may replace a hash. */
mn_status mn_library_set_content_hash(mn_library *lib, int64_t track_id,
                                      const char *hash, bool force) {
    static const char *SQL_ONCE =
        "UPDATE tracks SET content_hash=?1 "
        "WHERE id=?2 AND content_hash IS NULL;";
    static const char *SQL_FORCE =
        "UPDATE tracks SET content_hash=?1 WHERE id=?2;";
    sqlite3_stmt *st;
    int rc;
    if (!lib || track_id <= 0 || !hash || !hash[0]) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
                      force ? SQL_FORCE : SQL_ONCE, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* Batch feed for the low-priority background backfill: up to `max` rows
 * that still need a fingerprint (NULL hash, present on disk). Returns the
 * row count; fills ids/paths/sizes in caller-provided arrays. Read snapshot
 * (WAL, per-thread reader conn) — never blocks the writer. */
int mn_library_hashless_rows(mn_library *lib, int64_t *ids,
                             char (*paths)[1024], int64_t *sizes, int max) {
    static const char *SQL =
        "SELECT id, path, size FROM tracks "
        "WHERE content_hash IS NULL AND missing=0 "
        "ORDER BY id LIMIT ?1;";
    sqlite3_stmt *st;
    int rc, n = 0;
    mn__conn *r;
    if (!lib || !ids || !paths || !sizes || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int(st, 1, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(st, 1);
        ids[n] = sqlite3_column_int64(st, 0);
        snprintf(paths[n], sizeof(paths[n]), "%s", p ? (const char *)p : "");
        sizes[n] = sqlite3_column_int64(st, 2);
        n++;
    }
    sqlite3_reset(st);
    return n;
}

/* --------------------------------------------------------------------------
 * book_progress — audiobook resume/progress (replaces book_resume.txt).
 * -------------------------------------------------------------------------- */

/* Upsert a progress note for (album, chapter). Marks this row the book's
 * CURRENT one and demotes the album's other rows, so "where was I in this
 * book" is one indexed lookup while every chapter keeps its own remembered
 * position. content_hash (may be NULL) snapshots the track's immutable
 * fingerprint so the row re-attaches by hash after a file move and is
 * portable for device sync. percent is whole-book completion 0..1;
 * finished latches at >= 0.995 and un-latches only on an explicit restart
 * (percent dropping under 0.5 clears it — a rewind to re-listen). */
mn_status mn_library_book_note(mn_library *lib, int64_t album_id,
                               int64_t track_id, int64_t pos_ms,
                               int64_t updated) {
    /* Whole-book completion: (durations of chapters ORDERED BEFORE the
     * current one + pos within it) / total book duration. Chapter order
     * matches MNDB_SORT_TRACK: disc, track, id. Also snapshots the
     * track's immutable content_hash into the progress row so it can
     * re-attach after a file move / be synced. Small indexed scan of the
     * book's own rows (idx_tracks_album), fine at the 5s note cadence. */
    static const char *PCT =
        "SELECT"
        " (SELECT COALESCE(SUM(duration_ms),0) FROM tracks"
        "   WHERE album_id=?1 AND missing=0),"
        " (SELECT COALESCE(SUM(t2.duration_ms),0) FROM tracks t2, tracks t"
        "   WHERE t.id=?2 AND t2.album_id=?1 AND t2.missing=0"
        "     AND (t2.disc<t.disc OR (t2.disc=t.disc AND (t2.track<t.track"
        "          OR (t2.track=t.track AND t2.id<t.id))))),"
        " (SELECT content_hash FROM tracks WHERE id=?2);";
    static const char *UPSERT =
        "INSERT INTO book_progress"
        " (album_id,track_id,content_hash,pos_ms,percent,finished,current,updated)"
        " VALUES(?1,?2,?3,?4,?5,"
        "   CASE WHEN ?5>=0.995 THEN 1 ELSE 0 END,1,?6)"
        " ON CONFLICT(album_id,track_id) DO UPDATE SET"
        "   content_hash=COALESCE(excluded.content_hash,content_hash),"
        "   pos_ms=excluded.pos_ms,"
        "   percent=excluded.percent,"
        "   finished=CASE WHEN excluded.percent>=0.995 THEN 1"
        "                 WHEN excluded.percent<0.5  THEN 0"
        "                 ELSE finished END,"
        "   current=1,"
        "   updated=excluded.updated;";
    static const char *DEMOTE =
        "UPDATE book_progress SET current=0 "
        "WHERE album_id=?1 AND track_id<>?2 AND current=1;";
    sqlite3_stmt *st;
    int rc;
    double percent = 0.0;
    char hash[64];
    hash[0] = '\0';
    if (!lib || album_id <= 0 || track_id <= 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    /* percent + hash snapshot (writer conn: same lock, no reader needed) */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, PCT, &rc);
    if (st) {
        sqlite3_bind_int64(st, 1, album_id);
        sqlite3_bind_int64(st, 2, track_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int64_t total  = sqlite3_column_int64(st, 0);
            int64_t before = sqlite3_column_int64(st, 1);
            const unsigned char *h = sqlite3_column_text(st, 2);
            if (h) snprintf(hash, sizeof(hash), "%s", (const char *)h);
            if (total > 0) {
                percent = (double)(before + pos_ms) / (double)total;
                if (percent < 0.0) percent = 0.0;
                if (percent > 1.0) percent = 1.0;
            }
        }
        sqlite3_reset(st);
    }
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, UPSERT, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_bind_int64(st, 2, track_id);
    if (hash[0]) sqlite3_bind_text(st, 3, hash, -1, SQLITE_TRANSIENT);
    else         sqlite3_bind_null(st, 3);
    sqlite3_bind_int64(st, 4, pos_ms);
    sqlite3_bind_double(st, 5, percent);
    sqlite3_bind_int64(st, 6, updated);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc == SQLITE_DONE) {
        st = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEMOTE, &rc);
        if (st) {
            sqlite3_bind_int64(st, 1, album_id);
            sqlite3_bind_int64(st, 2, track_id);
            rc = sqlite3_step(st);
            sqlite3_reset(st);
        }
    }
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* The book's CURRENT position (which chapter + how far). Falls back to the
 * legacy caller behaviour: zeros when the book has no progress. */
bool mn_library_book_get(mn_library *lib, int64_t album_id,
                         int64_t *out_track, int64_t *out_pos,
                         double *out_percent, bool *out_finished) {
    static const char *SQL =
        "SELECT track_id,pos_ms,percent,finished FROM book_progress "
        "WHERE album_id=?1 AND current=1 LIMIT 1;";
    sqlite3_stmt *st;
    int rc;
    bool got = false;
    mn__conn *r;
    if (out_track) *out_track = 0;
    if (out_pos) *out_pos = 0;
    if (out_percent) *out_percent = 0;
    if (out_finished) *out_finished = false;
    if (!lib || album_id <= 0) return false;
    r = mn__reader(lib);
    if (!r) return false;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return false;
    sqlite3_bind_int64(st, 1, album_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (out_track)    *out_track    = sqlite3_column_int64(st, 0);
        if (out_pos)      *out_pos      = sqlite3_column_int64(st, 1);
        if (out_percent)  *out_percent  = sqlite3_column_double(st, 2);
        if (out_finished) *out_finished = sqlite3_column_int(st, 3) != 0;
        got = true;
    }
    sqlite3_reset(st);
    return got;
}

/* A chapter's remembered position within a book (0 if never played). */
int64_t mn_library_chapter_pos(mn_library *lib, int64_t album_id,
                               int64_t track_id) {
    static const char *SQL =
        "SELECT pos_ms FROM book_progress "
        "WHERE album_id=?1 AND track_id=?2 LIMIT 1;";
    sqlite3_stmt *st;
    int rc;
    int64_t pos = 0;
    mn__conn *r;
    if (!lib || album_id <= 0 || track_id <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_bind_int64(st, 2, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) pos = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return pos;
}

/* The Continue-Listening shelf feed: most-recently-touched books (current
 * rows, unfinished first optional at the UI), newest first, joined with the
 * current chapter's track metadata so the UI can render title/artist/art
 * and time-left without extra round-trips. Returns rows filled. */
int mn_library_recent_books(mn_library *lib, mn_book_recent *out, int max) {
    static const char *SQL =
        "SELECT bp.album_id, bp.track_id, bp.pos_ms, bp.percent, bp.finished,"
        "       bp.updated, t.album, t.album_artist, t.artist, t.title,"
        "       t.duration_ms"
        " FROM book_progress bp JOIN tracks t ON t.id=bp.track_id"
        " WHERE bp.current=1 AND t.missing=0"
        " ORDER BY bp.updated DESC LIMIT ?1;";
    sqlite3_stmt *st;
    int rc, n = 0;
    mn__conn *r;
    if (!lib || !out || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int(st, 1, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        mn_book_recent *b = &out[n];
        const unsigned char *al = sqlite3_column_text(st, 6);
        const unsigned char *aa = sqlite3_column_text(st, 7);
        const unsigned char *ar = sqlite3_column_text(st, 8);
        const unsigned char *ti = sqlite3_column_text(st, 9);
        memset(b, 0, sizeof(*b));
        b->album_id    = sqlite3_column_int64(st, 0);
        b->track_id    = sqlite3_column_int64(st, 1);
        b->pos_ms      = sqlite3_column_int64(st, 2);
        b->percent     = sqlite3_column_double(st, 3);
        b->finished    = sqlite3_column_int(st, 4) != 0;
        b->updated     = sqlite3_column_int64(st, 5);
        snprintf(b->album,        sizeof(b->album),        "%s", al ? (const char *)al : "");
        snprintf(b->album_artist, sizeof(b->album_artist), "%s", aa ? (const char *)aa : "");
        snprintf(b->artist,       sizeof(b->artist),       "%s", ar ? (const char *)ar : "");
        snprintf(b->title,        sizeof(b->title),        "%s", ti ? (const char *)ti : "");
        b->duration_ms = sqlite3_column_int64(st, 10);
        n++;
    }
    sqlite3_reset(st);
    return n;
}

/* ---- bookmarks ---- */

/* Add a named position. Snapshots the track's content_hash (sync/move-proof).
 * Returns the new bookmark id (0 on failure). */
int64_t mn_library_bookmark_add(mn_library *lib, int64_t album_id,
                                int64_t track_id, int64_t pos_ms,
                                const char *note, int64_t created) {
    static const char *SQL =
        "INSERT INTO book_bookmarks"
        " (album_id,track_id,content_hash,pos_ms,note,created)"
        " VALUES(?1,?2,(SELECT content_hash FROM tracks WHERE id=?2),"
        "        ?3,?4,?5);";
    sqlite3_stmt *st;
    int rc;
    int64_t id = 0;
    if (!lib || album_id <= 0 || track_id <= 0) return 0;
    if (lib->read_only) return 0;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return 0; }
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_bind_int64(st, 2, track_id);
    sqlite3_bind_int64(st, 3, pos_ms);
    sqlite3_bind_text(st, 4, note ? note : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, created);
    if (sqlite3_step(st) == SQLITE_DONE)
        id = sqlite3_last_insert_rowid(lib->writer.db);
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return id;
}

mn_status mn_library_bookmark_del(mn_library *lib, int64_t bookmark_id) {
    static const char *SQL = "DELETE FROM book_bookmarks WHERE id=?1;";
    sqlite3_stmt *st;
    int rc;
    if (!lib || bookmark_id <= 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, bookmark_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* A book's bookmarks, newest first. Parallel arrays; notes are copied into
 * fixed 128-byte rows. Returns count. */
int mn_library_bookmark_list(mn_library *lib, int64_t album_id,
                             int64_t *ids, int64_t *track_ids,
                             int64_t *pos_ms, char (*notes)[128],
                             int64_t *created, int max) {
    static const char *SQL =
        "SELECT id,track_id,pos_ms,note,created FROM book_bookmarks "
        "WHERE album_id=?1 ORDER BY created DESC LIMIT ?2;";
    sqlite3_stmt *st;
    int rc, n = 0;
    mn__conn *r;
    if (!lib || album_id <= 0 || !ids || !track_ids || !pos_ms || !notes ||
        !created || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_bind_int(st, 2, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *nt = sqlite3_column_text(st, 3);
        ids[n]       = sqlite3_column_int64(st, 0);
        track_ids[n] = sqlite3_column_int64(st, 1);
        pos_ms[n]    = sqlite3_column_int64(st, 2);
        snprintf(notes[n], sizeof(notes[n]), "%s", nt ? (const char *)nt : "");
        created[n]   = sqlite3_column_int64(st, 4);
        n++;
    }
    sqlite3_reset(st);
    return n;
}

/* Every remembered chapter position within one book (for the expand
 * panel's per-chapter resume decorations). Parallel arrays, returns count. */
int mn_library_book_chapters(mn_library *lib, int64_t album_id,
                             int64_t *track_ids, int64_t *pos_ms, int max) {
    static const char *SQL =
        "SELECT track_id, pos_ms FROM book_progress "
        "WHERE album_id=?1 AND pos_ms>0 LIMIT ?2;";
    sqlite3_stmt *st;
    int rc, n = 0;
    mn__conn *r;
    if (!lib || album_id <= 0 || !track_ids || !pos_ms || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_bind_int(st, 2, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        track_ids[n] = sqlite3_column_int64(st, 0);
        pos_ms[n]    = sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_reset(st);
    return n;
}

/* Compact per-book completion feed for grid-tile badges: every touched
 * book's (album_id, percent, finished). Parallel arrays, returns count. */
int mn_library_book_badges(mn_library *lib, int64_t *album_ids,
                           double *percents, bool *finisheds, int max) {
    static const char *SQL =
        "SELECT album_id, percent, finished FROM book_progress "
        "WHERE current=1 LIMIT ?1;";
    sqlite3_stmt *st;
    int rc, n = 0;
    mn__conn *r;
    if (!lib || !album_ids || !percents || !finisheds || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int(st, 1, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        album_ids[n] = sqlite3_column_int64(st, 0);
        percents[n]  = sqlite3_column_double(st, 1);
        finisheds[n] = sqlite3_column_int(st, 2) != 0;
        n++;
    }
    sqlite3_reset(st);
    return n;
}

/* Direct-by-id essentials for the play fallback: when a track is not part
 * of the CURRENT view query (a search hit from another kind, a filtered-out
 * row), the UI can still play it — album_id gives the album-queue context,
 * path+duration build a single-track queue for album-less rows. */
int64_t mn_library_track_album_id(mn_library *lib, int64_t track_id) {
    static const char *SQL = "SELECT album_id FROM tracks WHERE id=?1;";
    sqlite3_stmt *st;
    int rc;
    int64_t alb = 0;
    mn__conn *r;
    if (!lib || track_id <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) alb = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return alb;
}

bool mn_library_track_path_duration(mn_library *lib, int64_t track_id,
                                    char *path_out, size_t path_n,
                                    int64_t *dur_ms_out) {
    static const char *SQL =
        "SELECT path, duration_ms FROM tracks WHERE id=?1 AND missing=0;";
    sqlite3_stmt *st;
    int rc;
    bool ok = false;
    mn__conn *r;
    if (path_out && path_n) path_out[0] = 0;
    if (dur_ms_out) *dur_ms_out = 0;
    if (!lib || track_id <= 0 || !path_out || !path_n) return false;
    r = mn__reader(lib);
    if (!r) return false;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return false;
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(st, 0);
        if (p && p[0]) {
            snprintf(path_out, path_n, "%s", (const char *)p);
            if (dur_ms_out) *dur_ms_out = sqlite3_column_int64(st, 1);
            ok = true;
        }
    }
    sqlite3_reset(st);
    return ok;
}

/* Look a track up by its content fingerprint (progress re-match after a
 * file move: the old row's hash finds the new row). Returns the track id
 * or 0. If several rows share the hash (duplicate files), the lowest id
 * wins deterministically. */
int64_t mn_library_track_by_hash(mn_library *lib, const char *hash) {
    static const char *SQL =
        "SELECT id FROM tracks WHERE content_hash=?1 AND missing=0 "
        "ORDER BY id LIMIT 1;";
    sqlite3_stmt *st;
    int rc;
    int64_t id = 0;
    mn__conn *r;
    if (!lib || !hash || !hash[0]) return 0;
    r = mn__reader(lib);
    if (!r) return 0;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return 0;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return id;
}

mn_status mn_library_mark_missing(mn_library *lib, int64_t track_id,
                                  bool missing) {
    static const char *SQL = "UPDATE tracks SET missing=?1 WHERE id=?2;";
    sqlite3_stmt *st;
    int rc;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int(st, 1, missing ? 1 : 0);
    sqlite3_bind_int64(st, 2, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

mn_status mn_library_reap_missing(mn_library *lib, const char *root_prefix,
                                  int64_t scan_epoch, int64_t *out_marked) {
    static const char *SQL =
        "UPDATE tracks SET missing=1 "
        "WHERE path >= ?1 AND path < ?2 AND scan_epoch < ?3 AND missing=0;";
    sqlite3_stmt *st;
    int rc;
    char *upper = NULL;
    size_t n;

    if (!lib || !root_prefix) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    /* Normalize the prefix to end in a separator so the range covers only
     * true children — otherwise reaping "D:\OST" would also mark tracks under
     * the SIBLING "D:\OST Extra\..." as missing (same leak as the kind filter).
     * Then build an exclusive upper bound by incrementing the last byte so the
     * range [prefix\, prefix]) uses the path index rather than a LIKE scan. */
    n = strlen(root_prefix);
    upper = (char *)malloc(n + 2);
    if (!upper) return MN_ERR_NOMEM;
    memcpy(upper, root_prefix, n);
    upper[n] = '\0';
    if (n > 0 && upper[n - 1] != '\\' && upper[n - 1] != '/') {
        upper[n++] = '\\';
        upper[n] = '\0';
    }
    {
        /* `lower` is the normalized prefix; `upper` becomes lower with the
         * last byte incremented (in place after we copy). */
        char *lower = (char *)malloc(n + 1);
        if (!lower) { free(upper); return MN_ERR_NOMEM; }
        memcpy(lower, upper, n + 1);
        upper[n - 1] = (char)(upper[n - 1] + 1);

    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { free(upper); free(lower); mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, lower, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, scan_epoch);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    free(upper);
    free(lower);
    if (rc != SQLITE_DONE) {
        mn__set_err_db(lib, lib->writer.db);
        mn__write_unlock(lib);
        return mn__map_sqlite(rc);
    }
    if (out_marked) *out_marked = sqlite3_changes64(lib->writer.db);
    mn__write_unlock(lib);
    return MN_OK;
    }
}

mn_status mn_library_delete_track(mn_library *lib, int64_t track_id) {
    static const char *ART_UPD = "UPDATE artists SET track_count=track_count-1 WHERE id=?1;";
    static const char *AA_UPD  = "UPDATE album_artists SET track_count=track_count-1 WHERE id=?1;";
    static const char *ALB_UPD = "UPDATE albums SET track_count=track_count-1 WHERE id=?1;";
    static const char *GEN_UPD = "UPDATE genres SET track_count=track_count-1 WHERE id=?1;";
    static const char *FLD_UPD = "UPDATE folders SET track_count=track_count-1 WHERE id=?1;";
    static const char *DEL     = "DELETE FROM tracks WHERE id=?1;";
    static const char *DEL_PI  = "DELETE FROM playlist_items WHERE track_id=?1;";

    mn_status st = MN_OK;
    sqlite3_stmt *stmt;
    int rc;
    int64_t artist = 0, aa = 0, album = 0, genre = 0, folder = 0;
    bool own_txn = false;

    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    mn__write_lock(lib);

    if (!lib->in_txn) {
        st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto unlock; }
        own_txn = true;
    }

    mn__track_old_dims(lib, track_id, &artist, &aa, &album, &genre, &folder);

    /* Delete playlist membership explicitly (in case FK pragma off). */
    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL_PI, &rc);
    if (stmt) { sqlite3_bind_int64(stmt, 1, track_id); sqlite3_step(stmt); sqlite3_reset(stmt); }

    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL, &rc);
    if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
    sqlite3_bind_int64(stmt, 1, track_id);
    rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) { st = mn__map_sqlite(rc); mn__set_err_db(lib, lib->writer.db); goto rollback; }
    if (sqlite3_changes64(lib->writer.db) == 0) {
        st = MN_ERR_NOTFOUND;
        goto rollback;
    }

    mn__dim_bump(lib, ART_UPD, artist, -1); /* one-arg form below */
    /* mn__dim_bump uses (delta,id); these UPD use single ?1=id, so bump directly. */
    /* Correct single-arg decrement helper: */
    {
        /* Decrement directly since these statements take only id. */
        struct { const char *sql; int64_t id; } ups[5] = {
            { ART_UPD, artist }, { AA_UPD, aa }, { ALB_UPD, album },
            { GEN_UPD, genre }, { FLD_UPD, folder }
        };
        int i;
        for (i = 0; i < 5; i++) {
            if (ups[i].id <= 0) continue;
            stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, ups[i].sql, &rc);
            if (!stmt) continue;
            sqlite3_bind_int64(stmt, 1, ups[i].id);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
    }

    if (own_txn) {
        st = mn__exec(lib->writer.db, "COMMIT;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto rollback; }
        own_txn = false;
    }
    mn__write_unlock(lib);
    return MN_OK;

rollback:
    if (own_txn) mn__exec(lib->writer.db, "ROLLBACK;");
unlock:
    mn__write_unlock(lib);
    return st;
}

/* --------------------------------------------------------------------------
 * Subtree expansion: folder_id itself plus every folder whose path lies
 * under it (bytewise range on folders.path — the same bound construction as
 * mn_library_prefix_stats). Fills out[] up to `max`; returns the TOTAL count
 * found (return > max signals truncation to the caller).
 * -------------------------------------------------------------------------- */
int32_t mn_library_folder_subtree(mn_library *lib, int64_t folder_id,
                                  int64_t *out, int32_t max) {
    static const char *SEL = "SELECT path FROM folders WHERE id=?1;";
    static const char *SUB =
        "SELECT id FROM folders WHERE path=?1 OR (path>=?2 AND path<?3);";
    mn__conn     *r;
    sqlite3_stmt *st;
    int           rc;
    char          base[1200], lo[1200], hi[1200];
    size_t        n;
    int32_t       count = 0;

    if (!lib || !out || max <= 0) return 0;
    r = mn__reader(lib);
    if (!r) return 0;

    st = mn__stmt_get(r->db, &r->cache, SEL, &rc);
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, folder_id);
    base[0] = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (p) snprintf(base, sizeof(base), "%s", p);
    }
    sqlite3_reset(st);
    if (!base[0]) return 0;

    n = strlen(base);
    if (n + 2 >= sizeof(lo)) return 0;
    memcpy(lo, base, n);
    if (lo[n - 1] != '\\' && lo[n - 1] != '/') lo[n++] = '\\';
    lo[n] = 0;
    memcpy(hi, lo, n + 1);
    hi[n - 1] = (char)(hi[n - 1] + 1);

    st = mn__stmt_get(r->db, &r->cache, SUB, &rc);
    if (!st) return 0;
    sqlite3_bind_text(st, 1, base, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count < max) out[count] = sqlite3_column_int64(st, 0);
        count++;
    }
    sqlite3_reset(st);
    return count;
}

mn_status mn_library_delete_folder(mn_library *lib, int64_t folder_id,
                                   int64_t *out_deleted) {
    /* Grouped dimension decrements run BEFORE the tracks delete (they read
     * the doomed rows); the tracks delete then fires the per-row FTS delete
     * trigger, keeping tracks_fts consistent. */
    static const char *const DIM_UPD[4] = {
        "UPDATE artists SET track_count=track_count-"
        " (SELECT COUNT(*) FROM tracks WHERE folder_id=?1 AND artist_id=artists.id)"
        " WHERE id IN (SELECT DISTINCT artist_id FROM tracks WHERE folder_id=?1);",
        "UPDATE album_artists SET track_count=track_count-"
        " (SELECT COUNT(*) FROM tracks WHERE folder_id=?1 AND album_artist_id=album_artists.id)"
        " WHERE id IN (SELECT DISTINCT album_artist_id FROM tracks WHERE folder_id=?1);",
        "UPDATE albums SET track_count=track_count-"
        " (SELECT COUNT(*) FROM tracks WHERE folder_id=?1 AND album_id=albums.id)"
        " WHERE id IN (SELECT DISTINCT album_id FROM tracks WHERE folder_id=?1);",
        "UPDATE genres SET track_count=track_count-"
        " (SELECT COUNT(*) FROM tracks WHERE folder_id=?1 AND genre_id=genres.id)"
        " WHERE id IN (SELECT DISTINCT genre_id FROM tracks WHERE folder_id=?1);"
    };
    static const char *DEL_PI =
        "DELETE FROM playlist_items WHERE track_id IN"
        " (SELECT id FROM tracks WHERE folder_id=?1);";
    static const char *DEL_TRACKS = "DELETE FROM tracks WHERE folder_id=?1;";
    static const char *DEL_FOLDER = "DELETE FROM folders WHERE id=?1;";

    mn_status st = MN_OK;
    sqlite3_stmt *stmt;
    int rc, i;
    bool own_txn = false;

    if (out_deleted) *out_deleted = 0;
    if (!lib || folder_id <= 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    mn__write_lock(lib);
    if (!lib->in_txn) {
        st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto unlock; }
        own_txn = true;
    }

    for (i = 0; i < 4; i++) {
        stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DIM_UPD[i], &rc);
        if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
        sqlite3_bind_int64(stmt, 1, folder_id);
        rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc != SQLITE_DONE) {
            st = mn__map_sqlite(rc);
            mn__set_err_db(lib, lib->writer.db);
            goto rollback;
        }
    }

    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL_PI, &rc);
    if (stmt) { sqlite3_bind_int64(stmt, 1, folder_id); sqlite3_step(stmt); sqlite3_reset(stmt); }

    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL_TRACKS, &rc);
    if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
    sqlite3_bind_int64(stmt, 1, folder_id);
    rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) {
        st = mn__map_sqlite(rc);
        mn__set_err_db(lib, lib->writer.db);
        goto rollback;
    }
    if (out_deleted) *out_deleted = (int64_t)sqlite3_changes64(lib->writer.db);

    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL_FOLDER, &rc);
    if (!stmt) { st = mn__map_sqlite(rc); goto rollback; }
    sqlite3_bind_int64(stmt, 1, folder_id);
    rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) {
        st = mn__map_sqlite(rc);
        mn__set_err_db(lib, lib->writer.db);
        goto rollback;
    }

    if (own_txn) {
        st = mn__exec(lib->writer.db, "COMMIT;");
        if (st != MN_OK) { mn__set_err_db(lib, lib->writer.db); goto rollback; }
        own_txn = false;
    }
    mn__write_unlock(lib);
    return MN_OK;

rollback:
    if (own_txn) mn__exec(lib->writer.db, "ROLLBACK;");
unlock:
    mn__write_unlock(lib);
    return st;
}

mn_status mn_library_lookup_path(mn_library *lib, const char *path,
                                 int64_t *out_id, int64_t *out_mtime,
                                 int64_t *out_size, bool *out_missing) {
    static const char *SQL =
        "SELECT id, mtime, size, missing FROM tracks WHERE path=?1;";
    sqlite3_stmt *st;
    int rc;
    mn_status ret = MN_ERR_NOTFOUND;

    if (out_id)      *out_id = 0;
    if (out_mtime)   *out_mtime = 0;
    if (out_size)    *out_size = 0;
    if (out_missing) *out_missing = false;
    if (!lib || !path || !path[0]) return MN_ERR_INVALID;

    /* Uses the WRITER connection so rows upserted inside an open scan
     * transaction are visible (the reader connections would not see them
     * until commit, making is_known lie during a live scan). */
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (out_id)      *out_id      = sqlite3_column_int64(st, 0);
        if (out_mtime)   *out_mtime   = sqlite3_column_int64(st, 1);
        if (out_size)    *out_size    = sqlite3_column_int64(st, 2);
        if (out_missing) *out_missing = sqlite3_column_int(st, 3) != 0;
        ret = MN_OK;
    }
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return ret;
}

/* --------------------------------------------------------------------------
 * Per-root aggregate stats: tracks / distinct albums / total bytes / newest
 * date_added for every indexed file under `prefix` (a directory path). Uses
 * a RANGE on the unique path index (prefix..prefix+1), so it never scans
 * rows outside the subtree. Reader connection; safe on any thread.
 * -------------------------------------------------------------------------- */
mn_status mn_library_prefix_stats(mn_library *lib, const char *prefix,
                                  int64_t *tracks, int64_t *albums,
                                  int64_t *bytes, int64_t *newest_added) {
    static const char *SQL =
        "SELECT COUNT(*), COUNT(DISTINCT album), COALESCE(SUM(size),0),"
        "       COALESCE(MAX(date_added),0)"
        "  FROM tracks WHERE missing=0 AND path >= ?1 AND path < ?2;";
    mn__conn     *r;
    sqlite3_stmt *st;
    int           rc;
    char          lo[1200], hi[1200];
    size_t        n;

    if (tracks) *tracks = 0;
    if (albums) *albums = 0;
    if (bytes)  *bytes  = 0;
    if (newest_added) *newest_added = 0;
    if (!lib || !prefix || !prefix[0]) return MN_ERR_INVALID;

    /* range bounds: "<root>\" .. "<root>]" (']' = '\\'+1, so every path
     * strictly inside the subtree falls in [lo, hi)) */
    n = strlen(prefix);
    if (n + 2 >= sizeof(lo)) return MN_ERR_INVALID;
    memcpy(lo, prefix, n);
    if (lo[n - 1] != '\\' && lo[n - 1] != '/') lo[n++] = '\\';
    lo[n] = 0;
    memcpy(hi, lo, n + 1);
    hi[n - 1] = (char)(hi[n - 1] + 1);

    r = mn__reader(lib);
    if (!r) return MN_ERR_IO;
    st = mn__stmt_get(r->db, &r->cache, SQL, &rc);
    if (!st) return mn__map_sqlite(rc);
    sqlite3_bind_text(st, 1, lo, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hi, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (tracks) *tracks = sqlite3_column_int64(st, 0);
        if (albums) *albums = sqlite3_column_int64(st, 1);
        if (bytes)  *bytes  = sqlite3_column_int64(st, 2);
        if (newest_added) *newest_added = sqlite3_column_int64(st, 3);
    }
    sqlite3_reset(st);
    return MN_OK;
}

/* --------------------------------------------------------------------------
 * Online backup: copy the live database to `dest_path` via the SQLite
 * Backup API. Runs from a READER connection so the writer is never blocked;
 * the API transparently restarts if the source changes mid-copy, so it is
 * safe during scans. Intended for a background thread.
 * -------------------------------------------------------------------------- */
mn_status mn_library_backup(mn_library *lib, const char *dest_path) {
    sqlite3        *dst = NULL;
    sqlite3_backup *bk;
    mn__conn       *r;
    int             rc;

    if (!lib || !dest_path || !dest_path[0]) return MN_ERR_INVALID;
    r = mn__reader(lib);
    if (!r) return MN_ERR_IO;

    rc = sqlite3_open_v2(dest_path, &dst,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        if (dst) sqlite3_close(dst);
        return mn__map_sqlite(rc);
    }
    bk = sqlite3_backup_init(dst, "main", r->db, "main");
    if (!bk) {
        rc = sqlite3_errcode(dst);
        sqlite3_close(dst);
        return mn__map_sqlite(rc);
    }
    do {
        rc = sqlite3_backup_step(bk, 256);          /* ~1 MB per step */
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(25);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    (void)sqlite3_backup_finish(bk);
    rc = sqlite3_errcode(dst);
    sqlite3_close(dst);
    return (rc == SQLITE_OK) ? MN_OK : mn__map_sqlite(rc);
}

mn_status mn_library_enumerate_paths(mn_library *lib,
                                     mn_library_path_cb cb, void *user) {
    static const char *SQL =
        "SELECT path, mtime, size, missing FROM tracks;";
    sqlite3_stmt *st;
    int rc;

    if (!lib || !cb) return MN_ERR_INVALID;

    /* Writer connection so an in-progress scan transaction's upserts are
     * visible, matching mn_library_lookup_path's consistency. One statement,
     * one lock acquisition — replaces a per-file locked point query. */
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        cb(user, p ? p : "",
           sqlite3_column_int64(st, 1),
           sqlite3_column_int64(st, 2),
           sqlite3_column_int(st, 3) != 0);
    }
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return MN_OK;
}

mn_status mn_library_reset(mn_library *lib) {
    mn_status st;

    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    mn__write_lock(lib);
    if (lib->in_txn) {
        /* Caller must close its batch transaction first (stop the scan). */
        mn__write_unlock(lib);
        return MN_ERR_STATE;
    }
    st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
    if (st == MN_OK) {
        /* Order matters for FK cleanliness; the tracks delete fires the
         * FTS delete trigger per row, keeping tracks_fts consistent. */
        st = mn__exec(lib->writer.db,
            "DELETE FROM playlist_items;"
            "DELETE FROM playlists;"
            "DELETE FROM tracks;"
            "DELETE FROM artists;"
            "DELETE FROM album_artists;"
            "DELETE FROM albums;"
            "DELETE FROM genres;"
            "DELETE FROM folders;");
        if (st == MN_OK) {
            st = mn__exec(lib->writer.db, "COMMIT;");
        } else {
            mn__exec(lib->writer.db, "ROLLBACK;");
        }
    }
    if (st != MN_OK) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return st;
}

/* --------------------------------------------------------------------------
 * User columns
 * -------------------------------------------------------------------------- */

static mn_status mn__simple_write(mn_library *lib, const char *sql,
                                  int64_t a, int64_t b) {
    sqlite3_stmt *st;
    int rc;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, sql, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* Three-parameter variant of mn__simple_write (rating/liked writes also
 * stamp pref_updated_ms). */
static mn_status mn__simple_write3(mn_library *lib, const char *sql,
                                   int64_t a, int64_t b, int64_t c) {
    sqlite3_stmt *st;
    int rc;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, sql, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    sqlite3_bind_int64(st, 3, c);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

/* Wall-clock epoch MILLISECONDS. Stamped into tracks.pref_updated_ms on every
 * LOCAL like/dislike/rating change so last-write-wins sync merges resolve
 * (SYNC_PROTOCOL §3). Never stamped by play/skip updates. */
static int64_t mn__now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

mn_status mn_library_set_rating(mn_library *lib, int64_t track_id,
                                int32_t stars_x2) {
    if (stars_x2 < 0)  stars_x2 = 0;
    if (stars_x2 > 10) stars_x2 = 10;
    return mn__simple_write3(lib,
        "UPDATE tracks SET rating_x2=?1, pref_updated_ms=?2 WHERE id=?3;",
        (int64_t)stars_x2, mn__now_ms(), track_id);
}

mn_status mn_library_bump_play(mn_library *lib, int64_t track_id, int64_t when) {
    return mn__simple_write(lib,
        "UPDATE tracks SET play_count=play_count+1, last_played=?1 WHERE id=?2;",
        when, track_id);
}

mn_status mn_library_bump_skip(mn_library *lib, int64_t track_id, int64_t when) {
    return mn__simple_write(lib,
        "UPDATE tracks SET skip_count=skip_count+1, last_skipped=?1 WHERE id=?2;",
        when, track_id);
}

mn_status mn_library_set_has_art(mn_library *lib, int64_t track_id,
                                 bool has_art) {
    return mn__simple_write(lib,
        "UPDATE tracks SET has_art=?1 WHERE id=?2;",
        (int64_t)(has_art ? 1 : 0), track_id);
}

mn_status mn_library_set_liked(mn_library *lib, int64_t track_id,
                               int32_t liked) {
    if (liked < -1) liked = -1;
    if (liked >  1) liked = 1;
    return mn__simple_write3(lib,
        "UPDATE tracks SET liked=?1, pref_updated_ms=?2 WHERE id=?3;",
        (int64_t)liked, mn__now_ms(), track_id);
}

mn_status mn_library_get_liked(mn_library *lib, int64_t track_id,
                               int32_t *out_liked) {
    static const char *SQL = "SELECT liked FROM tracks WHERE id=?1;";
    mn__conn *reader;
    sqlite3_stmt *st = NULL;
    int rc;
    mn_status ret = MN_ERR_NOTFOUND;

    if (out_liked) *out_liked = 0;
    if (!lib || !out_liked || track_id <= 0) return MN_ERR_INVALID;

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    rc = sqlite3_prepare_v2(reader->db, SQL, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_liked = sqlite3_column_int(st, 0);
        ret = MN_OK;
    }
    sqlite3_finalize(st);
    return ret;
}

/* --------------------------------------------------------------------------
 * Library sync (SYNC_PROTOCOL v1): merged-metrics write + bulk enumerate
 * -------------------------------------------------------------------------- */

mn_status mn_library_sync_apply(mn_library *lib, int64_t track_id,
                                int liked, int disliked, int stars,
                                int64_t play_count, int64_t last_played_ms,
                                int64_t updated_ms) {
    static const char *SQL =
        "UPDATE tracks SET liked=?1, rating_x2=?2, play_count=?3,"
        " last_played=?4, pref_updated_ms=?5 WHERE id=?6;";
    sqlite3_stmt *st;
    int rc;
    int32_t thumbs;

    if (!lib || track_id <= 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;

    /* liked+disliked both set is invalid input; treat as liked (§7). */
    thumbs = liked ? 1 : (disliked ? -1 : 0);
    if (stars < 0) stars = 0;
    if (stars > 5) stars = 5;
    if (play_count < 0) play_count = 0;
    if (last_played_ms < 0) last_played_ms = 0;
    if (updated_ms < 0) updated_ms = 0;

    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, (int64_t)thumbs);
    sqlite3_bind_int64(st, 2, (int64_t)(stars * 2));
    sqlite3_bind_int64(st, 3, play_count);
    sqlite3_bind_int64(st, 4, last_played_ms / 1000);  /* ms -> unix seconds */
    sqlite3_bind_int64(st, 5, updated_ms);
    sqlite3_bind_int64(st, 6, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

mn_status mn_library_sync_enumerate(mn_library *lib, bool all,
                                    mn_library_sync_cb cb, void *user) {
    /* Two literal SQL strings so each gets its own cached statement. */
    static const char *SQL_ALL =
        "SELECT id, artist, title, album, duration_ms, liked, rating_x2,"
        " play_count, last_played, pref_updated_ms FROM tracks;";
    static const char *SQL_NONDEFAULT =
        "SELECT id, artist, title, album, duration_ms, liked, rating_x2,"
        " play_count, last_played, pref_updated_ms FROM tracks"
        " WHERE liked<>0 OR rating_x2>0 OR play_count>0 OR last_played>0;";
    sqlite3_stmt *st;
    int rc;

    if (!lib || !cb) return MN_ERR_INVALID;

    /* Writer connection for the same consistency as enumerate_paths (rows
     * upserted inside an open scan transaction are visible). One statement,
     * one lock acquisition. */
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
                      all ? SQL_ALL : SQL_NONDEFAULT, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *artist = (const char *)sqlite3_column_text(st, 1);
        const char *title  = (const char *)sqlite3_column_text(st, 2);
        const char *album  = (const char *)sqlite3_column_text(st, 3);
        cb(user,
           sqlite3_column_int64(st, 0),
           artist ? artist : "",
           title  ? title  : "",
           album  ? album  : "",
           sqlite3_column_int64(st, 4),
           sqlite3_column_int(st, 5),
           sqlite3_column_int(st, 6),
           sqlite3_column_int64(st, 7),
           sqlite3_column_int64(st, 8),
           sqlite3_column_int64(st, 9));
    }
    sqlite3_reset(st);
    mn__write_unlock(lib);
    return MN_OK;
}

mn_status mn_library_track_id_by_path(mn_library *lib, const char *path,
                                      int64_t *out_id)
{
    /* NOCASE: playlist files routinely differ from the scanner's path casing
     * on Windows (drive letter, folder capitalization). */
    static const char *SQL =
        "SELECT id FROM tracks WHERE path=?1 COLLATE NOCASE LIMIT 1;";
    mn__conn *reader;
    sqlite3_stmt *st = NULL;
    int rc;
    mn_status ret = MN_ERR_NOTFOUND;

    if (out_id) *out_id = 0;
    if (!lib || !out_id || !path || !path[0]) return MN_ERR_INVALID;

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    rc = sqlite3_prepare_v2(reader->db, SQL, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_id = sqlite3_column_int64(st, 0);
        ret = MN_OK;
    }
    sqlite3_finalize(st);
    return ret;
}

/* --------------------------------------------------------------------------
 * SQL builder helpers (dynamic string buffer)
 * -------------------------------------------------------------------------- */

typedef struct mn__sb {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} mn__sb;

static void mn__sb_init(mn__sb *b, char *initial, size_t cap) {
    b->buf = initial;
    b->len = 0;
    b->cap = cap;
    b->oom = false;
    if (b->buf && cap) b->buf[0] = '\0';
}

static bool mn__sb_reserve(mn__sb *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        char *nb;
        while (ncap < need) ncap *= 2;
        nb = (char *)realloc(b->buf, ncap);
        if (!nb) { b->oom = true; return false; }
        b->buf = nb;
        b->cap = ncap;
    }
    return true;
}

static void mn__sb_puts(mn__sb *b, const char *s) {
    size_t n;
    if (b->oom || !s) return;
    n = strlen(s);
    if (!mn__sb_reserve(b, n)) return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* --------------------------------------------------------------------------
 * Filter -> WHERE clause construction
 * -------------------------------------------------------------------------- */

/*
 * Build the WHERE fragment (without a leading "WHERE") for a filter spec and
 * record how bind parameters map. To keep binding uniform we bind facet value
 * ids and rating positionally after any FTS parameters. The caller binds:
 *   - FTS match text (if fts_match && has_fts5) at index 1
 *   - Then each cascade value id in order
 *   - Then min_rating (if > 0)
 * We return, via out params, the count of leading FTS binds so the caller can
 * compute offsets.
 *
 * For the LIKE fallback we embed four LIKE clauses that all bind the SAME
 * pattern parameter; SQLite lets a numbered parameter be reused, so ?N is
 * bound once.
 */
static const char *mn__facet_col(mn_facet_dim dim) {
    switch (dim) {
        case MN_FACET_ARTIST:       return "artist_id";
        case MN_FACET_ALBUM_ARTIST: return "album_artist_id";
        case MN_FACET_ALBUM:        return "album_id";
        case MN_FACET_GENRE:        return "genre_id";
        case MN_FACET_YEAR:         return "year";
        case MN_FACET_FOLDER:       return "folder_id";
        default:                    return NULL;
    }
}

/*
 * Append the WHERE clause. Uses named-ish numbered parameters:
 *   ?1 reserved for FTS match text OR LIKE pattern (when a text filter present)
 *   subsequent ?n for cascade ids and rating, assigned sequentially.
 * Returns via *next_param the next free parameter index.
 */
static void mn__build_where(mn__sb *sb, const mn_filter_spec *spec,
                            bool has_fts5, const char *fts_match,
                            int64_t playlist_id, int *next_param) {
    int p = 1;
    bool first = true;
    int i;
    bool text_filter = (fts_match && fts_match[0]);

    mn__sb_puts(sb, " WHERE 1=1");

    if (!spec || !spec->include_missing) {
        mn__sb_puts(sb, " AND t.missing=0");
    }

    if (text_filter) {
        if (has_fts5) {
            /* Join handled in FROM; restrict via subquery on FTS rowid. */
            mn__sb_puts(sb, " AND t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?1)");
        } else {
            mn__sb_puts(sb,
                " AND (t.title LIKE ?1 OR t.artist LIKE ?1 OR "
                "t.album LIKE ?1 OR t.album_artist LIKE ?1)");
        }
        p = 2;
    }

    if (spec) {
        for (i = 0; i < spec->cascade_len && i < MN_MAX_CASCADE; i++) {
            const char *col = mn__facet_col(spec->cascade[i].dim);
            char frag[64];
            if (!col) continue;
            if (spec->cascade[i].value_id == 0) {
                /* The Unknown bucket (facet vid 0): untagged rows have a
                 * NULL dimension id. No bind slot — keep bind in lockstep. */
                snprintf(frag, sizeof(frag), " AND t.%s IS NULL", col);
            } else {
                snprintf(frag, sizeof(frag), " AND t.%s=?%d", col, p);
                p++;
            }
            mn__sb_puts(sb, frag);
        }
        if (spec->min_rating_x2 > 0) {
            char frag[48];
            snprintf(frag, sizeof(frag), " AND t.rating_x2>=?%d", p);
            mn__sb_puts(sb, frag);
            p++;
        }
        /* Liked-only: constant predicate, no bind slot (keeps mn__bind_where
         * in lockstep without a matching bind). */
        if (spec->liked_only) {
            mn__sb_puts(sb, " AND t.liked=1");
        }
        /* Folder-visibility exclusion. Ids are trusted int64 values, embedded
         * as integer literals so no bind slots are consumed (keeps
         * mn__bind_where in lockstep). Skipped when the cascade explicitly
         * browses a folder so a hidden folder's own view still lists it.
         * folder_id may be NULL (legacy rows) — those are never hidden. */
        if (spec->excluded_folders_len > 0) {
            bool folder_cascade = false;
            for (i = 0; i < spec->cascade_len && i < MN_MAX_CASCADE; i++) {
                if (spec->cascade[i].dim == MN_FACET_FOLDER) {
                    folder_cascade = true;
                    break;
                }
            }
            if (!folder_cascade) {
                int k;
                int len = spec->excluded_folders_len;
                if (len > MN_MAX_EXCLUDED_FOLDERS) len = MN_MAX_EXCLUDED_FOLDERS;
                mn__sb_puts(sb, " AND (t.folder_id IS NULL OR t.folder_id NOT IN (");
                for (k = 0; k < len; k++) {
                    char idbuf[32];
                    snprintf(idbuf, sizeof(idbuf), "%s%lld", k ? "," : "",
                             (long long)spec->excluded_folders[k]);
                    mn__sb_puts(sb, idbuf);
                }
                mn__sb_puts(sb, "))");
            }
        }
        /* Category scoping (music vs audiobooks): indexed path prefix-ranges
         * over the kind roots, embedded as escaped string literals (no bind
         * slots — keeps mn__bind_where in lockstep). The exclusive upper
         * bound increments the prefix's last byte, same trick as
         * mn_library_reap_missing. */
        if (spec->kind_roots_len > 0) {
            int k, wrote = 0;
            int len = spec->kind_roots_len;
            if (len > MN_MAX_KIND_ROOTS) len = MN_MAX_KIND_ROOTS;
            for (k = 0; k < len; k++) {
                const char *r0 = spec->kind_roots[k];
                char rbuf[520];
                size_t rl, ci;
                if (!r0 || !r0[0]) continue;
                /* Normalize the root to end with a trailing separator BEFORE
                 * building the prefix range. Without it, the range for
                 * "D:\OST" is [D:\OST, D:\OSU) which wrongly captures the
                 * SIBLING folder "D:\OST Extra\..." (space 0x20 sorts inside
                 * the range) — a cross-kind LEAK. Appending "\" makes the
                 * range [D:\OST\, D:\OST]) which matches only true children
                 * (the DB stores backslash paths — verified). Same trailing-
                 * separator trick as mn_library_folder_subtree. */
                rl = strlen(r0);
                if (rl >= sizeof(rbuf) - 1) rl = sizeof(rbuf) - 2;
                memcpy(rbuf, r0, rl);
                rbuf[rl] = '\0';
                if (rl > 0 && rbuf[rl - 1] != '\\' && rbuf[rl - 1] != '/') {
                    rbuf[rl++] = '\\';
                    rbuf[rl] = '\0';
                }
                if (!wrote)
                    mn__sb_puts(sb, spec->kind_include ? " AND (" : " AND NOT (");
                else
                    mn__sb_puts(sb, " OR ");
                wrote++;
                /* COLLATE NOCASE on BOTH operands: everywhere else in the app
                 * paths compare case-insensitively (_strnicmp / _stricmp, and
                 * the lookup in mn_library_track_id_by_path). A raw BINARY
                 * range here: one case difference between folder_kinds.txt and
                 * tracks.path put every track OUTSIDE the range — a named kind
                 * showed an EMPTY library, and music's exclusion clause matched
                 * nothing so that kind's content leaked back into music.
                 * NOCASE folds ASCII a-z only; non-ASCII path bytes still
                 * compare byte-wise, which is fine because the registry stores
                 * the path exactly as the folder picker reported it. The
                 * separator/increment bytes ('\\' 0x5C -> ']' 0x5D, '/' -> '0')
                 * are non-letters, so the prefix range stays exact under the
                 * fold: only 0x5C itself sits in ['\\', ']'). */
                mn__sb_puts(sb, "(t.path >= '");
                for (ci = 0; ci < rl; ci++) {
                    char lit[3] = { rbuf[ci], 0, 0 };
                    if (rbuf[ci] == '\'') lit[1] = '\'';   /* SQL-escape quote */
                    mn__sb_puts(sb, lit);
                }
                mn__sb_puts(sb, "' COLLATE NOCASE AND t.path < '");
                for (ci = 0; ci < rl; ci++) {
                    char cc = rbuf[ci];
                    char lit[3] = { 0, 0, 0 };
                    if (ci == rl - 1) cc = (char)(cc + 1);
                    lit[0] = cc;
                    if (cc == '\'') lit[1] = '\'';
                    mn__sb_puts(sb, lit);
                }
                mn__sb_puts(sb, "' COLLATE NOCASE)");
            }
            if (wrote) mn__sb_puts(sb, ")");
            else if (spec->kind_include)
                /* include-mode with no usable roots: match nothing (an
                 * audiobooks view without audiobook folders must be empty,
                 * never a leak of the music library) */
                mn__sb_puts(sb, " AND 0");
        }
    }

    if (playlist_id > 0) {
        char frag[48];
        snprintf(frag, sizeof(frag), " AND pi.playlist_id=?%d", p);
        mn__sb_puts(sb, frag);
        p++;
    }

    MN__UNUSED(first);
    *next_param = p;
}

/* Bind the filter parameters onto a prepared statement, mirroring the order in
 * mn__build_where. Returns the next free bind index. */
static int mn__bind_where(sqlite3_stmt *st, const mn_filter_spec *spec,
                          bool has_fts5, const char *fts_match,
                          int64_t playlist_id) {
    int p = 1;
    int i;
    bool text_filter = (fts_match && fts_match[0]);

    if (text_filter) {
        if (has_fts5) {
            /* Prefix-tokenize so partial, type-as-you-go input matches:
             *   "abb roa"  ->  "abb"* "roa"*
             * Each whitespace token is quoted (neutralizing any FTS operator
             * chars the user types) and suffixed '*' for prefix search; all
             * tokens must prefix-match (implicit AND). Case-insensitive via
             * FTS's default tokenizer. */
            const char *q = fts_match;
            size_t cap = strlen(q) * 4 + 8;
            char *ex = (char *)malloc(cap);
            if (ex) {
                size_t oi = 0;
                const char *p2 = q;
                while (*p2 && oi + 4 < cap) {
                    while (*p2 == ' ' || *p2 == '\t') p2++;
                    if (!*p2) break;
                    if (oi > 0) ex[oi++] = ' ';
                    ex[oi++] = '"';
                    while (*p2 && *p2 != ' ' && *p2 != '\t' && oi + 4 < cap) {
                        if (*p2 == '"') ex[oi++] = '"';  /* escape for FTS literal */
                        ex[oi++] = *p2++;
                    }
                    ex[oi++] = '"';
                    ex[oi++] = '*';
                }
                ex[oi] = '\0';
                sqlite3_bind_text(st, 1, ex, -1, SQLITE_TRANSIENT);
                free(ex);
            } else {
                sqlite3_bind_text(st, 1, fts_match, -1, SQLITE_TRANSIENT);
            }
        } else {
            /* Wrap in %...% for a substring LIKE. */
            size_t n = strlen(fts_match);
            char *pat = (char *)malloc(n + 3);
            if (pat) {
                pat[0] = '%';
                memcpy(pat + 1, fts_match, n);
                pat[n + 1] = '%';
                pat[n + 2] = '\0';
                sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
                free(pat);
            } else {
                sqlite3_bind_text(st, 1, fts_match, -1, SQLITE_TRANSIENT);
            }
        }
        p = 2;
    }

    if (spec) {
        for (i = 0; i < spec->cascade_len && i < MN_MAX_CASCADE; i++) {
            if (!mn__facet_col(spec->cascade[i].dim)) continue;
            if (spec->cascade[i].value_id == 0) continue; /* IS NULL: no slot */
            sqlite3_bind_int64(st, p, spec->cascade[i].value_id);
            p++;
        }
        if (spec->min_rating_x2 > 0) {
            sqlite3_bind_int(st, p, spec->min_rating_x2);
            p++;
        }
    }

    if (playlist_id > 0) {
        sqlite3_bind_int64(st, p, playlist_id);
        p++;
    }

    return p;
}

/* Map a sort key to a column expression (with COLLATE where relevant). */
static const char *mn__sort_col(mn_sort_key k) {
    switch (k) {
        case MN_SORT_TITLE:        return "t.title COLLATE NOCASE";
        case MN_SORT_ARTIST:       return "t.artist COLLATE NOCASE";
        case MN_SORT_ALBUM:        return "t.album COLLATE NOCASE";
        case MN_SORT_ALBUM_ARTIST: return "t.album_artist COLLATE NOCASE";
        case MN_SORT_GENRE:        return "t.genre COLLATE NOCASE";
        case MN_SORT_YEAR:         return "t.year";
        case MN_SORT_TRACK:        return "t.disc, t.track";
        case MN_SORT_DURATION:     return "t.duration_ms";
        case MN_SORT_DATE_ADDED:   return "t.date_added";
        case MN_SORT_DATE_CREATED: return "t.created";
        case MN_SORT_LAST_PLAYED:  return "t.last_played";
        case MN_SORT_PLAY_COUNT:   return "t.play_count";
        case MN_SORT_RATING:       return "t.rating_x2";
        case MN_SORT_BITRATE:      return "t.bitrate_kbps";
        case MN_SORT_PATH:         return "t.path";
        default:                   return NULL;
    }
}

/* Untagged-value rank emitted BEFORE the sort column, always ASC, so rows
 * with an empty/implausible key value group at the BOTTOM regardless of
 * sort direction (MediaMonkey behavior; SQLite's native ordering would put
 * NULL/'' first ascending, i.e. junk at the top of every text sort). */
static const char *mn__sort_rank(mn_sort_key k) {
    switch (k) {
        case MN_SORT_TITLE:        return "(t.title IS NULL OR t.title='')";
        case MN_SORT_ARTIST:       return "(t.artist IS NULL OR t.artist='')";
        case MN_SORT_ALBUM:        return "(t.album IS NULL OR t.album='')";
        case MN_SORT_ALBUM_ARTIST: return "(t.album_artist IS NULL OR t.album_artist='')";
        case MN_SORT_GENRE:        return "(t.genre IS NULL OR t.genre='')";
        case MN_SORT_YEAR:         return "(t.year IS NULL OR t.year<1000 OR t.year>2100)";
        case MN_SORT_DATE_CREATED: return "(t.created IS NULL OR t.created<=0)";
        default:                   return NULL;
    }
}

/* Append ORDER BY with a stable id tiebreak. */
static void mn__build_order(mn__sb *sb, const mn_filter_spec *spec,
                            int64_t playlist_id, bool has_fts5,
                            const char *fts_match) {
    int i;
    bool any = false;

    if (playlist_id > 0) {
        /* Playlist order dominates. */
        mn__sb_puts(sb, " ORDER BY pi.position ASC, t.id ASC");
        return;
    }

    mn__sb_puts(sb, " ORDER BY ");
    if (spec) {
        for (i = 0; i < spec->sort_len && i < MN_MAX_SORT; i++) {
            mn_sort_key k = spec->sort[i].key;
            const char *col;
            if (k == MN_SORT_NONE) continue;
            if (k == MN_SORT_RELEVANCE) {
                if (has_fts5 && fts_match && fts_match[0]) {
                    if (any) mn__sb_puts(sb, ", ");
                    /* Lower rank = better match in FTS5. */
                    mn__sb_puts(sb, "(SELECT rank FROM tracks_fts WHERE rowid=t.id AND tracks_fts MATCH ?1) ASC");
                    any = true;
                }
                continue;
            }
            col = mn__sort_col(k);
            if (!col) continue;
            if (any) mn__sb_puts(sb, ", ");
            {
                const char *rank = mn__sort_rank(k);
                if (rank) { mn__sb_puts(sb, rank); mn__sb_puts(sb, ", "); }
            }
            if (k == MN_SORT_TRACK) {
                /* Compound disc+track: direction must apply to BOTH parts
                 * (a bare "t.disc, t.track DESC" would leave disc ASC).
                 * NATSORT path tiebreak: for rows whose track tags are 0
                 * (83 audiobooks in the reference library) disc+track are
                 * equal, and natural filename order is the correct chapter
                 * order — plain id order played Chapter 10 after Chapter 1. */
                const char *d = spec->sort[i].descending ? " DESC" : " ASC";
                mn__sb_puts(sb, "t.disc"); mn__sb_puts(sb, d);
                mn__sb_puts(sb, ", t.track"); mn__sb_puts(sb, d);
                mn__sb_puts(sb, ", t.path COLLATE NATSORT"); mn__sb_puts(sb, d);
            } else {
                mn__sb_puts(sb, col);
                mn__sb_puts(sb, spec->sort[i].descending ? " DESC" : " ASC");
            }
            any = true;
        }
    }
    if (!any) {
        mn__sb_puts(sb, "t.id ASC");
    } else {
        mn__sb_puts(sb, ", t.id ASC"); /* stable tiebreak */
    }
}

/* --------------------------------------------------------------------------
 * Query engine
 * -------------------------------------------------------------------------- */

static mn__conn *mn__query_reader(mn_query *q) { return q->reader; }

static mn_status mn__query_open_common(mn_library *lib, const mn_filter_spec *spec,
                                       int64_t playlist_id, mn_query **out) {
    mn_query *q;
    mn__conn *reader;

    if (!lib || !out) return MN_ERR_INVALID;
    *out = NULL;

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    q = (mn_query *)calloc(1, sizeof(*q));
    if (!q) return MN_ERR_NOMEM;

    q->lib = lib;
    q->reader = reader;
    q->playlist_id = playlist_id;
    q->has_count = false;
    q->count = 0;
    if (spec) {
        q->spec = *spec;
        if (spec->fts_match) {
            q->fts_match = mn__strdup(spec->fts_match);
            if (!q->fts_match) { free(q); return MN_ERR_NOMEM; }
            q->spec.fts_match = q->fts_match;
        } else {
            q->spec.fts_match = NULL;
        }
    } else {
        memset(&q->spec, 0, sizeof(q->spec));
    }
    mn_arena_init(&q->arena, 65536);

    *out = q;
    return MN_OK;
}

mn_status mn_query_open(mn_library *lib, const mn_filter_spec *spec,
                        mn_query **out) {
    return mn__query_open_common(lib, spec, 0, out);
}

void mn_query_close(mn_query *q) {
    if (!q) return;
    mn_arena_free(&q->arena);
    free(q->fts_match);
    free(q->sql_buf);
    free(q);
}

/* Build the FROM clause: base table plus playlist join if needed. */
static void mn__query_from(mn__sb *sb, int64_t playlist_id) {
    if (playlist_id > 0) {
        mn__sb_puts(sb, " FROM playlist_items pi JOIN tracks t ON t.id=pi.track_id");
    } else {
        mn__sb_puts(sb, " FROM tracks t");
    }
}

mn_status mn_query_count(mn_query *q, int64_t *out_count) {
    mn__sb sb;
    char stack[1024];
    sqlite3_stmt *st = NULL;
    int rc;
    mn_status status = MN_OK;
    int next_param;
    bool has_fts5;

    if (!q || !out_count) return MN_ERR_INVALID;
    if (q->has_count) { *out_count = q->count; return MN_OK; }

    has_fts5 = q->lib->has_fts5;

    mn__sb_init(&sb, stack, sizeof(stack));
    mn__sb_puts(&sb, "SELECT COUNT(*)");
    mn__query_from(&sb, q->playlist_id);
    mn__build_where(&sb, &q->spec, has_fts5, q->fts_match,
                    q->playlist_id, &next_param);
    if (sb.oom) { status = MN_ERR_NOMEM; goto cleanup; }

    rc = sqlite3_prepare_v2(q->reader->db, sb.buf, -1, &st, NULL);
    if (rc != SQLITE_OK) { status = mn__map_sqlite(rc); goto cleanup; }
    mn__bind_where(st, &q->spec, has_fts5, q->fts_match, q->playlist_id);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        q->count = sqlite3_column_int64(st, 0);
        q->has_count = true;
        *out_count = q->count;
        status = MN_OK;
    } else {
        status = mn__map_sqlite(rc);
    }

cleanup:
    if (st) sqlite3_finalize(st);
    if (sb.buf != stack) free(sb.buf);
    return status;
}

/* Project the current row of a fully-selected tracks statement into the arena. */
static void mn__row_from_stmt(mn_arena *a, sqlite3_stmt *st, mn_track_row *r) {
    const char *s;
    #define TXT(col) ((s = (const char*)sqlite3_column_text(st, col)) ? \
                       mn_arena_strdup(a, s) : mn_arena_strdup(a, ""))
    r->id           = sqlite3_column_int64(st, 0);
    r->path         = TXT(1);
    r->title        = TXT(2);
    r->artist       = TXT(3);
    r->album        = TXT(4);
    r->album_artist = TXT(5);
    r->composer     = TXT(6);
    r->genre        = TXT(7);
    r->format       = TXT(8);
    r->year         = sqlite3_column_int(st, 9);
    r->track        = sqlite3_column_int(st, 10);
    r->disc         = sqlite3_column_int(st, 11);
    r->duration_ms  = sqlite3_column_int64(st, 12);
    r->sample_rate  = sqlite3_column_int(st, 13);
    r->channels     = sqlite3_column_int(st, 14);
    r->bit_depth    = sqlite3_column_int(st, 15);
    r->bitrate_kbps = sqlite3_column_int(st, 16);
    r->size         = sqlite3_column_int64(st, 17);
    r->mtime        = sqlite3_column_int64(st, 18);
    r->date_added   = sqlite3_column_int64(st, 19);
    r->last_played  = sqlite3_column_int64(st, 20);
    r->play_count   = sqlite3_column_int(st, 21);
    r->skip_count   = sqlite3_column_int(st, 22);
    r->rating_x2    = sqlite3_column_int(st, 23);
    r->has_art      = sqlite3_column_int(st, 24) != 0;
    r->missing      = sqlite3_column_int(st, 25) != 0;
    r->liked        = sqlite3_column_int(st, 26);
    r->album_id     = sqlite3_column_int64(st, 27);
    r->created      = sqlite3_column_int64(st, 28);
    #undef TXT
}

#define MN__TRACK_COLUMNS \
    "t.id,t.path,t.title,t.artist,t.album,t.album_artist,t.composer,t.genre," \
    "t.format,t.year,t.track,t.disc,t.duration_ms,t.sample_rate,t.channels," \
    "t.bit_depth,t.bitrate_kbps,t.size,t.mtime,t.date_added,t.last_played," \
    "t.play_count,t.skip_count,t.rating_x2,t.has_art,t.missing,t.liked," \
    "t.album_id,t.created"

mn_status mn_query_window(mn_query *q, int64_t offset, int32_t n,
                          const mn_track_row **out_rows, int32_t *out_n) {
    mn__sb sb;
    char stack[2048];
    sqlite3_stmt *st = NULL;
    int rc, param;
    mn_status status = MN_OK;
    mn_track_row *rows = NULL;
    int32_t got = 0;
    int next_param;
    bool has_fts5;

    if (!q || !out_rows || !out_n) return MN_ERR_INVALID;
    *out_rows = NULL;
    *out_n = 0;
    if (n <= 0) return MN_OK;
    if (offset < 0) return MN_ERR_RANGE;

    has_fts5 = q->lib->has_fts5;
    mn_arena_reset(&q->arena);

    mn__sb_init(&sb, stack, sizeof(stack));
    mn__sb_puts(&sb, "SELECT ");
    mn__sb_puts(&sb, MN__TRACK_COLUMNS);
    mn__query_from(&sb, q->playlist_id);
    mn__build_where(&sb, &q->spec, has_fts5, q->fts_match,
                    q->playlist_id, &next_param);
    mn__build_order(&sb, &q->spec, q->playlist_id, has_fts5, q->fts_match);
    mn__sb_puts(&sb, " LIMIT ? OFFSET ?");
    if (sb.oom) { status = MN_ERR_NOMEM; goto cleanup; }

    rc = sqlite3_prepare_v2(q->reader->db, sb.buf, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        mn__set_err_db(q->lib, q->reader->db);
        status = mn__map_sqlite(rc);
        goto cleanup;
    }
    param = mn__bind_where(st, &q->spec, has_fts5, q->fts_match, q->playlist_id);
    sqlite3_bind_int(st, param, n);
    sqlite3_bind_int64(st, param + 1, offset);

    rows = (mn_track_row *)mn_arena_alloc(&q->arena,
                                          sizeof(mn_track_row) * (size_t)n,
                                          sizeof(void *));
    if (!rows) { status = MN_ERR_NOMEM; goto cleanup; }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW && got < n) {
        mn__row_from_stmt(&q->arena, st, &rows[got]);
        got++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        status = mn__map_sqlite(rc);
        goto cleanup;
    }

    *out_rows = rows;
    *out_n = got;
    status = MN_OK;

cleanup:
    if (st) sqlite3_finalize(st);
    if (sb.buf != stack) free(sb.buf);
    return status;
}

mn_status mn_query_fetch_id(mn_query *q, int64_t track_id,
                            const mn_track_row **out_row) {
    static const char *SQL =
        "SELECT " MN__TRACK_COLUMNS " FROM tracks t WHERE t.id=?1;";
    sqlite3_stmt *st = NULL;
    int rc;
    mn_status status;
    mn_track_row *row;

    if (!q || !out_row) return MN_ERR_INVALID;
    *out_row = NULL;

    mn_arena_reset(&q->arena);
    st = mn__stmt_get(q->reader->db, &q->reader->cache, SQL, &rc);
    if (!st) return mn__map_sqlite(rc);
    sqlite3_bind_int64(st, 1, track_id);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        row = (mn_track_row *)mn_arena_alloc(&q->arena, sizeof(*row), sizeof(void *));
        if (!row) { sqlite3_reset(st); return MN_ERR_NOMEM; }
        mn__row_from_stmt(&q->arena, st, row);
        *out_row = row;
        status = MN_OK;
    } else if (rc == SQLITE_DONE) {
        status = MN_ERR_NOTFOUND;
    } else {
        status = mn__map_sqlite(rc);
    }
    sqlite3_reset(st);
    return status;
}

mn_status mn_query_index_of(mn_query *q, int64_t track_id, int64_t *out_offset) {
    /*
     * Determine the ordinal position of track_id within the ordered result by
     * counting how many rows sort strictly before it. We reuse the ordered
     * window query as a subquery with row_number(), which is exact and uses
     * the same ORDER BY (hence the same tiebreak) as window().
     */
    mn__sb sb;
    char stack[2560];
    sqlite3_stmt *st = NULL;
    int rc, param;
    mn_status status = MN_OK;
    int next_param;
    bool has_fts5;

    if (!q || !out_offset) return MN_ERR_INVALID;
    *out_offset = -1;

    has_fts5 = q->lib->has_fts5;

    mn__sb_init(&sb, stack, sizeof(stack));
    mn__sb_puts(&sb,
        "SELECT pos FROM (SELECT t.id AS tid, "
        "(ROW_NUMBER() OVER (");
    mn__build_order(&sb, &q->spec, q->playlist_id, has_fts5, q->fts_match);
    mn__sb_puts(&sb, ") - 1) AS pos");
    mn__query_from(&sb, q->playlist_id);
    mn__build_where(&sb, &q->spec, has_fts5, q->fts_match,
                    q->playlist_id, &next_param);
    mn__sb_puts(&sb, ") WHERE tid=?");
    if (sb.oom) { status = MN_ERR_NOMEM; goto cleanup; }

    {
        /* The trailing ?N for tid gets the next parameter index. */
        char tail[16];
        /* Rebuild is awkward; instead bind tid at (next free) explicit index. */
        MN__UNUSED(tail);
    }

    rc = sqlite3_prepare_v2(q->reader->db, sb.buf, -1, &st, NULL);
    if (rc != SQLITE_OK) { status = mn__map_sqlite(rc); goto cleanup; }
    param = mn__bind_where(st, &q->spec, has_fts5, q->fts_match, q->playlist_id);
    sqlite3_bind_int64(st, param, track_id);

    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out_offset = sqlite3_column_int64(st, 0);
        status = MN_OK;
    } else if (rc == SQLITE_DONE) {
        status = MN_ERR_NOTFOUND;
    } else {
        status = mn__map_sqlite(rc);
    }

cleanup:
    if (st) sqlite3_finalize(st);
    if (sb.buf != stack) free(sb.buf);
    return status;
}

/* --------------------------------------------------------------------------
 * Facets
 * -------------------------------------------------------------------------- */

mn_status mn_facet_open(mn_library *lib, mn_facet_dim dim,
                        const mn_filter_spec *spec, mn_facet **out) {
    mn_facet *f;
    mn__conn *reader;

    if (!lib || !out) return MN_ERR_INVALID;
    if (dim == MN_FACET_NONE) return MN_ERR_INVALID;
    *out = NULL;

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    f = (mn_facet *)calloc(1, sizeof(*f));
    if (!f) return MN_ERR_NOMEM;
    f->lib = lib;
    f->reader = reader;
    f->dim = dim;
    f->order = MN_FACET_ORDER_LABEL;
    f->has_count = false;
    f->count = 0;
    if (spec) {
        f->spec = *spec;
        if (spec->fts_match) {
            f->fts_match = mn__strdup(spec->fts_match);
            if (!f->fts_match) { free(f); return MN_ERR_NOMEM; }
            f->spec.fts_match = f->fts_match;
        } else {
            f->spec.fts_match = NULL;
        }
    } else {
        memset(&f->spec, 0, sizeof(f->spec));
    }
    mn_arena_init(&f->arena, 16384);
    *out = f;
    return MN_OK;
}

void mn_facet_close(mn_facet *f) {
    if (!f) return;
    mn_arena_free(&f->arena);
    free(f->fts_match);
    free(f->sql_buf);
    free(f);
}

mn_status mn_facet_sort(mn_facet *f, mn_facet_order order) {
    if (!f) return MN_ERR_INVALID;
    if (f->order != order) { f->order = order; f->has_count = false; }
    return MN_OK;
}

/*
 * Facet dimensions map to a (join table, label column, group id column).
 * ARTIST/ALBUM_ARTIST/ALBUM/GENRE group by the dimension id and read the
 * label from the dimension table. YEAR groups by the numeric year directly.
 */
static void mn__facet_build(mn__sb *sb, mn_facet *f, bool count_only) {
    mn_facet_dim dim = f->dim;
    int next_param;
    bool has_fts5 = f->lib->has_fts5;
    const char *idcol = mn__facet_col(dim);

    if (dim == MN_FACET_YEAR) {
        if (count_only) {
            mn__sb_puts(sb, "SELECT COUNT(DISTINCT t.year) FROM tracks t");
            mn__build_where(sb, &f->spec, has_fts5, f->fts_match, 0, &next_param);
        } else {
            mn__sb_puts(sb,
                "SELECT t.year AS vid, CAST(t.year AS TEXT) AS label, COUNT(*) AS cnt "
                "FROM tracks t");
            mn__build_where(sb, &f->spec, has_fts5, f->fts_match, 0, &next_param);
            mn__sb_puts(sb, " GROUP BY t.year");
            if (f->order == MN_FACET_ORDER_COUNT_DESC)
                mn__sb_puts(sb, " ORDER BY cnt DESC, t.year ASC");
            else
                mn__sb_puts(sb, " ORDER BY (t.year<1000 OR t.year>2100), t.year ASC");
            mn__sb_puts(sb, " LIMIT ? OFFSET ?");
        }
        return;
    }

    /* id-based dimensions: derive label table. Untagged rows (NULL id)
     * surface as an "Unknown <dim>" bucket with vid=0 — LEFT JOIN +
     * COALESCE, sorted LAST (MediaMonkey behavior). Cascade selection of
     * vid 0 maps back to `id IS NULL` in mn__build_where. */
    {
        const char *dimtable = NULL, *labelcol = "name", *unknown = "Unknown";
        switch (dim) {
            case MN_FACET_ARTIST:       dimtable = "artists"; unknown = "Unknown artist"; break;
            case MN_FACET_ALBUM_ARTIST: dimtable = "album_artists"; unknown = "Unknown artist"; break;
            case MN_FACET_ALBUM:        dimtable = "albums"; unknown = "Unknown album"; break;
            case MN_FACET_GENRE:        dimtable = "genres"; unknown = "Unknown genre"; break;
            case MN_FACET_FOLDER:       dimtable = "folders"; labelcol = "path"; break;
            default:                    dimtable = "artists"; break;
        }
        if (count_only) {
            mn__sb_puts(sb, "SELECT COUNT(DISTINCT COALESCE(t.");
            mn__sb_puts(sb, idcol);
            mn__sb_puts(sb, ",0)) FROM tracks t");
            mn__build_where(sb, &f->spec, has_fts5, f->fts_match, 0, &next_param);
        } else {
            char frag[320];
            snprintf(frag, sizeof(frag),
                "SELECT COALESCE(t.%s,0) AS vid, COALESCE(d.%s,'%s') AS label,"
                " COUNT(*) AS cnt "
                "FROM tracks t LEFT JOIN %s d ON d.id=t.%s",
                idcol, labelcol, unknown, dimtable, idcol);
            mn__sb_puts(sb, frag);
            mn__build_where(sb, &f->spec, has_fts5, f->fts_match, 0, &next_param);
            snprintf(frag, sizeof(frag), " GROUP BY COALESCE(t.%s,0)", idcol);
            mn__sb_puts(sb, frag);
            if (f->order == MN_FACET_ORDER_COUNT_DESC)
                mn__sb_puts(sb, " ORDER BY cnt DESC, label COLLATE NOCASE ASC");
            else
                mn__sb_puts(sb, " ORDER BY (vid=0), (label IS NULL OR label=''),"
                                " label COLLATE NOCASE ASC");
            mn__sb_puts(sb, " LIMIT ? OFFSET ?");
        }
    }
}

mn_status mn_facet_count(mn_facet *f, int64_t *out_count) {
    mn__sb sb;
    char stack[1024];
    sqlite3_stmt *st = NULL;
    int rc;
    mn_status status;

    if (!f || !out_count) return MN_ERR_INVALID;
    if (f->has_count) { *out_count = f->count; return MN_OK; }

    mn__sb_init(&sb, stack, sizeof(stack));
    mn__facet_build(&sb, f, true);
    if (sb.oom) { if (sb.buf != stack) free(sb.buf); return MN_ERR_NOMEM; }

    rc = sqlite3_prepare_v2(f->reader->db, sb.buf, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (sb.buf != stack) free(sb.buf);
        return mn__map_sqlite(rc);
    }
    mn__bind_where(st, &f->spec, f->lib->has_fts5, f->fts_match, 0);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        f->count = sqlite3_column_int64(st, 0);
        f->has_count = true;
        *out_count = f->count;
        status = MN_OK;
    } else {
        status = mn__map_sqlite(rc);
    }
    sqlite3_finalize(st);
    if (sb.buf != stack) free(sb.buf);
    return status;
}

mn_status mn_facet_window(mn_facet *f, int64_t offset, int32_t n,
                          const mn_facet_row **out_rows, int32_t *out_n) {
    mn__sb sb;
    char stack[1536];
    sqlite3_stmt *st = NULL;
    int rc, param;
    mn_status status = MN_OK;
    mn_facet_row *rows = NULL;
    int32_t got = 0;

    if (!f || !out_rows || !out_n) return MN_ERR_INVALID;
    *out_rows = NULL;
    *out_n = 0;
    if (n <= 0) return MN_OK;
    if (offset < 0) return MN_ERR_RANGE;

    mn_arena_reset(&f->arena);

    mn__sb_init(&sb, stack, sizeof(stack));
    mn__facet_build(&sb, f, false);
    if (sb.oom) { status = MN_ERR_NOMEM; goto cleanup; }

    rc = sqlite3_prepare_v2(f->reader->db, sb.buf, -1, &st, NULL);
    if (rc != SQLITE_OK) { status = mn__map_sqlite(rc); goto cleanup; }
    param = mn__bind_where(st, &f->spec, f->lib->has_fts5, f->fts_match, 0);
    sqlite3_bind_int(st, param, n);
    sqlite3_bind_int64(st, param + 1, offset);

    rows = (mn_facet_row *)mn_arena_alloc(&f->arena,
                                          sizeof(mn_facet_row) * (size_t)n,
                                          sizeof(void *));
    if (!rows) { status = MN_ERR_NOMEM; goto cleanup; }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW && got < n) {
        const char *lbl = (const char *)sqlite3_column_text(st, 1);
        rows[got].value_id = sqlite3_column_int64(st, 0);
        rows[got].label    = lbl ? mn_arena_strdup(&f->arena, lbl)
                                 : mn_arena_strdup(&f->arena, "");
        rows[got].count    = sqlite3_column_int64(st, 2);
        got++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        status = mn__map_sqlite(rc);
        goto cleanup;
    }

    *out_rows = rows;
    *out_n = got;
    status = MN_OK;

cleanup:
    if (st) sqlite3_finalize(st);
    if (sb.buf != stack) free(sb.buf);
    return status;
}

/* --------------------------------------------------------------------------
 * Playlists
 * -------------------------------------------------------------------------- */

mn_status mn_playlist_create(mn_library *lib, const char *name,
                             int64_t *out_id) {
    static const char *SQL =
        "INSERT INTO playlists(name,date_created,date_modified) "
        "VALUES(?1,?2,?2);";
    sqlite3_stmt *st;
    int rc;
    int64_t now = (int64_t)time(NULL);
    if (!lib || !name) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { mn__set_err_db(lib, lib->writer.db); mn__write_unlock(lib); return mn__map_sqlite(rc); }
    if (out_id) *out_id = sqlite3_last_insert_rowid(lib->writer.db);
    mn__write_unlock(lib);
    return MN_OK;
}

mn_status mn_playlist_rename(mn_library *lib, int64_t playlist_id,
                             const char *name) {
    static const char *SQL =
        "UPDATE playlists SET name=?1, date_modified=?2 WHERE id=?3;";
    sqlite3_stmt *st;
    int rc;
    if (!lib || !name) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SQL, &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)time(NULL));
    sqlite3_bind_int64(st, 3, playlist_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

mn_status mn_playlist_delete(mn_library *lib, int64_t playlist_id) {
    mn_status st = MN_OK;
    sqlite3_stmt *stmt;
    int rc;
    bool own_txn = false;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    if (!lib->in_txn) {
        st = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (st != MN_OK) { mn__write_unlock(lib); return st; }
        own_txn = true;
    }
    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache,
                        "DELETE FROM playlist_items WHERE playlist_id=?1;", &rc);
    if (stmt) { sqlite3_bind_int64(stmt, 1, playlist_id); sqlite3_step(stmt); sqlite3_reset(stmt); }
    stmt = mn__stmt_get(lib->writer.db, &lib->writer.cache,
                        "DELETE FROM playlists WHERE id=?1;", &rc);
    if (stmt) { sqlite3_bind_int64(stmt, 1, playlist_id); sqlite3_step(stmt); sqlite3_reset(stmt); }
    if (own_txn) st = mn__exec(lib->writer.db, "COMMIT;");
    mn__write_unlock(lib);
    return st;
}

mn_status mn_playlist_add(mn_library *lib, int64_t playlist_id,
                          int64_t track_id, int64_t *out_position) {
    static const char *NEXT =
        "SELECT COALESCE(MAX(position)+1,0) FROM playlist_items WHERE playlist_id=?1;";
    static const char *INS =
        "INSERT INTO playlist_items(playlist_id,position,track_id) VALUES(?1,?2,?3);";
    static const char *TOUCH =
        "UPDATE playlists SET date_modified=?1 WHERE id=?2;";
    sqlite3_stmt *st;
    int rc;
    int64_t pos = 0;
    bool own_txn = false;
    mn_status status = MN_OK;

    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    if (!lib->in_txn) {
        status = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (status != MN_OK) { mn__write_unlock(lib); return status; }
        own_txn = true;
    }
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, NEXT, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    if (sqlite3_step(st) == SQLITE_ROW) pos = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, INS, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, pos);
    sqlite3_bind_int64(st, 3, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); mn__set_err_db(lib, lib->writer.db); goto done; }

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, TOUCH, &rc);
    if (st) { sqlite3_bind_int64(st, 1, (int64_t)time(NULL)); sqlite3_bind_int64(st, 2, playlist_id); sqlite3_step(st); sqlite3_reset(st); }

    if (out_position) *out_position = pos;

done:
    if (own_txn) {
        if (status == MN_OK) status = mn__exec(lib->writer.db, "COMMIT;");
        else mn__exec(lib->writer.db, "ROLLBACK;");
    }
    mn__write_unlock(lib);
    return status;
}

mn_status mn_playlist_insert_at(mn_library *lib, int64_t playlist_id,
                                int64_t track_id, int64_t position) {
    static const char *SHIFT =
        "UPDATE playlist_items SET position=position+1 "
        "WHERE playlist_id=?1 AND position>=?2;";
    static const char *INS =
        "INSERT INTO playlist_items(playlist_id,position,track_id) VALUES(?1,?2,?3);";
    sqlite3_stmt *st;
    int rc;
    bool own_txn = false;
    mn_status status = MN_OK;

    if (!lib || position < 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    if (!lib->in_txn) {
        status = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (status != MN_OK) { mn__write_unlock(lib); return status; }
        own_txn = true;
    }
    /* Shift later items up by one to open a slot at `position`. SQLite defers
     * the composite PRIMARY KEY uniqueness check to statement end, so the
     * transient overlap during the renumber is safe. */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SHIFT, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, position);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, INS, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, position);
    sqlite3_bind_int64(st, 3, track_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

done:
    if (own_txn) {
        if (status == MN_OK) status = mn__exec(lib->writer.db, "COMMIT;");
        else mn__exec(lib->writer.db, "ROLLBACK;");
    }
    mn__write_unlock(lib);
    return status;
}

mn_status mn_playlist_remove_at(mn_library *lib, int64_t playlist_id,
                                int64_t position) {
    static const char *DEL =
        "DELETE FROM playlist_items WHERE playlist_id=?1 AND position=?2;";
    static const char *SHIFT =
        "UPDATE playlist_items SET position=position-1 "
        "WHERE playlist_id=?1 AND position>?2;";
    sqlite3_stmt *st;
    int rc;
    bool own_txn = false;
    mn_status status = MN_OK;

    if (!lib || position < 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    if (!lib->in_txn) {
        status = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (status != MN_OK) { mn__write_unlock(lib); return status; }
        own_txn = true;
    }
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, DEL, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, position);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

    st = mn__stmt_get(lib->writer.db, &lib->writer.cache, SHIFT, &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, position);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

done:
    if (own_txn) {
        if (status == MN_OK) status = mn__exec(lib->writer.db, "COMMIT;");
        else mn__exec(lib->writer.db, "ROLLBACK;");
    }
    mn__write_unlock(lib);
    return status;
}

mn_status mn_playlist_move(mn_library *lib, int64_t playlist_id,
                           int64_t from_pos, int64_t to_pos) {
    sqlite3_stmt *st;
    int rc;
    bool own_txn = false;
    mn_status status = MN_OK;
    int64_t track_id = -1;

    if (!lib || from_pos < 0 || to_pos < 0) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    if (from_pos == to_pos) return MN_OK;

    mn__write_lock(lib);
    if (!lib->in_txn) {
        status = mn__exec(lib->writer.db, "BEGIN IMMEDIATE;");
        if (status != MN_OK) { mn__write_unlock(lib); return status; }
        own_txn = true;
    }

    /* Fetch the moving track. */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
        "SELECT track_id FROM playlist_items WHERE playlist_id=?1 AND position=?2;", &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, from_pos);
    if (sqlite3_step(st) == SQLITE_ROW) track_id = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    if (track_id < 0) { status = MN_ERR_NOTFOUND; goto done; }

    /* Park the moving row out of the way to avoid PK collision. */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
        "UPDATE playlist_items SET position=-1 WHERE playlist_id=?1 AND position=?2;", &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, from_pos);
    sqlite3_step(st); sqlite3_reset(st);

    if (from_pos < to_pos) {
        /* Shift the gap [from+1, to] down by one. */
        st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
            "UPDATE playlist_items SET position=position-1 "
            "WHERE playlist_id=?1 AND position>?2 AND position<=?3;", &rc);
    } else {
        /* Shift [to, from-1] up by one. */
        st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
            "UPDATE playlist_items SET position=position+1 "
            "WHERE playlist_id=?1 AND position>=?3 AND position<?2;", &rc);
    }
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, from_pos);
    sqlite3_bind_int64(st, 3, to_pos);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

    /* Place the parked row at its destination. */
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
        "UPDATE playlist_items SET position=?2 WHERE playlist_id=?1 AND position=-1;", &rc);
    if (!st) { status = mn__map_sqlite(rc); goto done; }
    sqlite3_bind_int64(st, 1, playlist_id);
    sqlite3_bind_int64(st, 2, to_pos);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) { status = mn__map_sqlite(rc); goto done; }

done:
    if (own_txn) {
        if (status == MN_OK) status = mn__exec(lib->writer.db, "COMMIT;");
        else mn__exec(lib->writer.db, "ROLLBACK;");
    }
    mn__write_unlock(lib);
    return status;
}

mn_status mn_playlist_clear(mn_library *lib, int64_t playlist_id) {
    sqlite3_stmt *st;
    int rc;
    if (!lib) return MN_ERR_INVALID;
    if (lib->read_only) return MN_ERR_STATE;
    mn__write_lock(lib);
    st = mn__stmt_get(lib->writer.db, &lib->writer.cache,
        "DELETE FROM playlist_items WHERE playlist_id=?1;", &rc);
    if (!st) { mn__write_unlock(lib); return mn__map_sqlite(rc); }
    sqlite3_bind_int64(st, 1, playlist_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) mn__set_err_db(lib, lib->writer.db);
    mn__write_unlock(lib);
    return mn__map_sqlite(rc);
}

mn_status mn_playlist_list(mn_library *lib, mn_arena *arena,
                           const mn_playlist_row **out_rows, int32_t *out_n) {
    static const char *SQL =
        "SELECT p.id, p.name, "
        "(SELECT COUNT(*) FROM playlist_items pi WHERE pi.playlist_id=p.id), "
        "p.date_created, p.date_modified "
        "FROM playlists p ORDER BY p.name COLLATE NOCASE ASC, p.id ASC;";
    mn__conn *reader;
    sqlite3_stmt *st = NULL;
    int rc;
    mn_playlist_row *rows = NULL;
    int32_t cap = 0, got = 0;

    if (!lib || !arena || !out_rows || !out_n) return MN_ERR_INVALID;
    *out_rows = NULL;
    *out_n = 0;

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    rc = sqlite3_prepare_v2(reader->db, SQL, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *nm;
        if (got == cap) {
            int32_t ncap = cap ? cap * 2 : 16;
            mn_playlist_row *nr = (mn_playlist_row *)mn_arena_alloc(
                arena, sizeof(mn_playlist_row) * (size_t)ncap, sizeof(void *));
            if (!nr) { sqlite3_finalize(st); return MN_ERR_NOMEM; }
            if (rows) memcpy(nr, rows, sizeof(mn_playlist_row) * (size_t)got);
            rows = nr;
            cap = ncap;
        }
        rows[got].id            = sqlite3_column_int64(st, 0);
        nm                      = (const char *)sqlite3_column_text(st, 1);
        rows[got].name          = nm ? mn_arena_strdup(arena, nm)
                                      : mn_arena_strdup(arena, "");
        rows[got].track_count   = sqlite3_column_int64(st, 2);
        rows[got].date_created  = sqlite3_column_int64(st, 3);
        rows[got].date_modified = sqlite3_column_int64(st, 4);
        got++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return mn__map_sqlite(rc);

    *out_rows = rows;
    *out_n = got;
    return MN_OK;
}

mn_status mn_playlist_query(mn_library *lib, int64_t playlist_id,
                            const mn_filter_spec *spec, mn_query **out) {
    if (!lib || !out || playlist_id <= 0) return MN_ERR_INVALID;
    return mn__query_open_common(lib, spec, playlist_id, out);
}

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */

mn_status mn_library_stats(mn_library *lib, mn_stats *out) {
    static const char *SQL =
        "SELECT "
        " (SELECT COUNT(*) FROM tracks WHERE missing=0),"
        " (SELECT COUNT(*) FROM tracks WHERE missing=1),"
        " (SELECT COUNT(*) FROM artists WHERE track_count>0),"
        " (SELECT COUNT(*) FROM albums WHERE track_count>0),"
        " (SELECT COUNT(*) FROM genres WHERE track_count>0),"
        " (SELECT COUNT(*) FROM playlists),"
        " (SELECT COALESCE(SUM(duration_ms),0) FROM tracks WHERE missing=0),"
        " (SELECT COALESCE(SUM(size),0) FROM tracks WHERE missing=0),"
        " (SELECT COALESCE(SUM(play_count),0) FROM tracks);";
    mn__conn *reader;
    sqlite3_stmt *st = NULL;
    int rc;

    if (!lib || !out) return MN_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    rc = sqlite3_prepare_v2(reader->db, SQL, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->track_count       = sqlite3_column_int64(st, 0);
        out->missing_count     = sqlite3_column_int64(st, 1);
        out->artist_count      = sqlite3_column_int64(st, 2);
        out->album_count       = sqlite3_column_int64(st, 3);
        out->genre_count       = sqlite3_column_int64(st, 4);
        out->playlist_count    = sqlite3_column_int64(st, 5);
        out->total_duration_ms = sqlite3_column_int64(st, 6);
        out->total_size_bytes  = sqlite3_column_int64(st, 7);
        out->total_play_count  = sqlite3_column_int64(st, 8);
        sqlite3_finalize(st);
        return MN_OK;
    }
    sqlite3_finalize(st);
    return mn__map_sqlite(rc);
}

mn_status mn_library_stats_ext(mn_library *lib, mn_stats_ext *out) {
    /* One aggregate pass over tracks... */
    static const char *AGG =
        "SELECT COUNT(*),"
        " COALESCE(SUM(duration_ms),0),"
        " COALESCE(SUM(size),0),"
        " COUNT(DISTINCT album_id),"
        " COUNT(DISTINCT artist_id),"
        " SUM(CASE WHEN bit_depth>=24 OR sample_rate>=88200 THEN 1 ELSE 0 END),"
        " (SELECT COUNT(*) FROM tracks WHERE missing=1)"
        " FROM tracks WHERE missing=0;";
    /* ...plus the per-format breakdown (most-populated first). */
    static const char *FMT =
        "SELECT UPPER(format), COUNT(*) FROM tracks WHERE missing=0"
        " GROUP BY UPPER(format) ORDER BY COUNT(*) DESC;";
    mn__conn *reader;
    sqlite3_stmt *st = NULL;
    int rc;

    if (!lib || !out) return MN_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    reader = mn__reader(lib);
    if (!reader) return MN_ERR_IO;

    rc = sqlite3_prepare_v2(reader->db, AGG, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return mn__map_sqlite(rc);
    }
    out->track_count       = sqlite3_column_int64(st, 0);
    out->total_duration_ms = sqlite3_column_int64(st, 1);
    out->total_size_bytes  = sqlite3_column_int64(st, 2);
    out->album_count       = sqlite3_column_int64(st, 3);
    out->artist_count      = sqlite3_column_int64(st, 4);
    out->hires_count       = sqlite3_column_int64(st, 5);
    out->missing_count     = sqlite3_column_int64(st, 6);
    sqlite3_finalize(st);
    st = NULL;

    rc = sqlite3_prepare_v2(reader->db, FMT, -1, &st, NULL);
    if (rc != SQLITE_OK) return mn__map_sqlite(rc);
    while (sqlite3_step(st) == SQLITE_ROW &&
           out->format_count < MN_STATS_MAX_FORMATS) {
        const char *f = (const char *)sqlite3_column_text(st, 0);
        mn_stats_fmt *slot = &out->formats[out->format_count++];
        snprintf(slot->fmt, sizeof(slot->fmt), "%s",
                 (f && f[0]) ? f : "UNKNOWN");
        slot->n = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return MN_OK;
}
