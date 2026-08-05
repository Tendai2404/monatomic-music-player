/*
 * playlists.c — Implementation of playlists.h for Monatomic Audio Player.
 *
 * Static playlists, smart-playlist rule compilation, JSON persistence of rule
 * trees, and M3U/M3U8/PLS import/export, all against a caller-owned sqlite3*.
 *
 * Schema (created by mn_playlists_init_schema):
 *
 *   playlists(
 *       id          INTEGER PRIMARY KEY,
 *       name        TEXT NOT NULL,
 *       kind        INTEGER NOT NULL,      -- mn_playlist_kind
 *       rules_json  TEXT,                  -- NULL for static
 *       created_at  INTEGER NOT NULL,
 *       updated_at  INTEGER NOT NULL)
 *
 *   playlist_members(
 *       playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
 *       position    INTEGER NOT NULL,      -- dense 0-based
 *       track_id    INTEGER NOT NULL,
 *       PRIMARY KEY(playlist_id, position))
 *
 * Smart playlists persist their rule tree as JSON in playlists.rules_json and
 * compile it, on demand, to a single parameterized SELECT over the `tracks`
 * table. Only windowed rows are ever materialized so a 1,000,000-track library
 * never blows up.
 *
 * This module owns no sqlite3 connection; it neither opens nor closes `db`.
 */

#include "playlists.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "sqlite3.h"

/* ------------------------------------------------------------------------- *
 * Small utilities
 * ------------------------------------------------------------------------- */

#define MN_UNUSED(x) ((void)(x))

/* A growable byte buffer used for building SQL text, JSON, and export output. */
typedef struct mn_buf {
    char  *data;   /* NUL-terminated once mn_buf_cstr() has been reached      */
    size_t len;    /* bytes used, excluding the NUL                           */
    size_t cap;    /* allocated capacity                                      */
    bool   oom;    /* sticky allocation-failure flag                          */
} mn_buf;

static void mn_buf_init(mn_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

static void mn_buf_free(mn_buf *b) {
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* Ensure at least `extra` more bytes (plus room for a trailing NUL) fit. */
static bool mn_buf_reserve(mn_buf *b, size_t extra) {
    if (b->oom) {
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) {
        return true;
    }
    size_t ncap = (b->cap != 0) ? b->cap : 64;
    while (ncap < need) {
        if (ncap > (size_t)-1 / 2) {   /* overflow guard */
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    char *nd = (char *)realloc(b->data, ncap);
    if (nd == NULL) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void mn_buf_append(mn_buf *b, const char *s, size_t n) {
    if (n == 0) {
        return;
    }
    if (!mn_buf_reserve(b, n)) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void mn_buf_puts(mn_buf *b, const char *s) {
    if (s != NULL) {
        mn_buf_append(b, s, strlen(s));
    }
}

static void mn_buf_putc(mn_buf *b, char c) {
    mn_buf_append(b, &c, 1);
}

static void mn_buf_printf(mn_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        b->oom = true;
        return;
    }
    if (!mn_buf_reserve(b, (size_t)need)) {
        va_end(ap2);
        return;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
}

/* Return the NUL-terminated string. On a fresh empty buffer allocate a "". */
static const char *mn_buf_cstr(mn_buf *b) {
    if (b->data == NULL) {
        if (!mn_buf_reserve(b, 0)) {
            return NULL;
        }
        b->data[0] = '\0';
    }
    return b->data;
}

/* strdup that reports OOM cleanly. */
static char *mn_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static char *mn_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p != NULL) {
        if (n != 0 && s != NULL) {
            memcpy(p, s, n);
        }
        p[n] = '\0';
    }
    return p;
}

/* ------------------------------------------------------------------------- *
 * Status strings
 * ------------------------------------------------------------------------- */

const char *mn_playlist_status_str(mn_playlist_status status) {
    switch (status) {
        case MN_PLAYLIST_OK:              return "ok";
        case MN_PLAYLIST_ERR_INVALID_ARG: return "invalid argument";
        case MN_PLAYLIST_ERR_NOT_FOUND:   return "not found";
        case MN_PLAYLIST_ERR_WRONG_KIND:  return "wrong playlist kind";
        case MN_PLAYLIST_ERR_DUPLICATE:   return "duplicate name";
        case MN_PLAYLIST_ERR_SQL:         return "sql error";
        case MN_PLAYLIST_ERR_NOMEM:       return "out of memory";
        case MN_PLAYLIST_ERR_RULE:        return "malformed rule tree";
        case MN_PLAYLIST_ERR_IO:          return "i/o error";
        case MN_PLAYLIST_ERR_PARSE:       return "parse error";
        case MN_PLAYLIST_ERR_RANGE:       return "index out of range";
        case MN_PLAYLIST_ERR_UNSUPPORTED: return "unsupported";
        case MN_PLAYLIST_ERR_SCHEMA:      return "schema error";
        default:                          return "unknown error";
    }
}

/* Translate a raw sqlite result code to an mn_playlist_status. */
static mn_playlist_status mn_from_sqlite(int rc) {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:        return MN_PLAYLIST_OK;
        case SQLITE_CONSTRAINT: return MN_PLAYLIST_ERR_DUPLICATE;
        case SQLITE_NOMEM:      return MN_PLAYLIST_ERR_NOMEM;
        default:                return MN_PLAYLIST_ERR_SQL;
    }
}

/* ------------------------------------------------------------------------- *
 * Field / operator model
 * ------------------------------------------------------------------------- */

/* The column category a field lives in, which constrains legal operators. */
typedef enum mn_field_class {
    MN_FC_TEXT = 0,
    MN_FC_INT,
    MN_FC_DATE,
    MN_FC_BOOL,
    MN_FC_PLAYLIST
} mn_field_class;

/*
 * Map a field enum to (a) its SQL expression over the `tracks` table `t` and
 * (b) its class. Fields with no backing column resolve to a constant so the
 * generated SQL always compiles and evaluates deterministically.
 *
 * Returns false for an unknown field.
 */
static bool mn_field_sql(mn_pl_field field, const char **out_expr,
                         mn_field_class *out_class) {
    const char *expr = NULL;
    mn_field_class cls = MN_FC_TEXT;
    switch (field) {
        case MN_PL_FIELD_TITLE:         expr = "t.title";        cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_ARTIST:        expr = "t.artist";       cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_ALBUM_ARTIST:  expr = "t.album_artist"; cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_ALBUM:         expr = "t.album";        cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_GENRE:         expr = "t.genre";        cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_COMPOSER:      expr = "t.composer";     cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_COMMENT:       expr = "''";             cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_GROUPING:      expr = "''";             cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_FILE_PATH:     expr = "t.path";         cls = MN_FC_TEXT; break;
        case MN_PL_FIELD_FILE_KIND:     expr = "t.format";       cls = MN_FC_TEXT; break;

        case MN_PL_FIELD_YEAR:          expr = "t.year";           cls = MN_FC_INT; break;
        case MN_PL_FIELD_TRACK_NO:      expr = "t.track";          cls = MN_FC_INT; break;
        case MN_PL_FIELD_DISC_NO:       expr = "t.disc";           cls = MN_FC_INT; break;
        case MN_PL_FIELD_DURATION_MS:   expr = "t.duration_ms";    cls = MN_FC_INT; break;
        case MN_PL_FIELD_BITRATE:       expr = "(t.bitrate_kbps*1000)"; cls = MN_FC_INT; break;
        case MN_PL_FIELD_SAMPLE_RATE:   expr = "t.sample_rate";    cls = MN_FC_INT; break;
        case MN_PL_FIELD_CHANNELS:      expr = "t.channels";       cls = MN_FC_INT; break;
        case MN_PL_FIELD_FILE_SIZE:     expr = "t.size";           cls = MN_FC_INT; break;
        case MN_PL_FIELD_BPM:           expr = "0";                cls = MN_FC_INT; break;
        case MN_PL_FIELD_PLAY_COUNT:    expr = "t.play_count";     cls = MN_FC_INT; break;
        case MN_PL_FIELD_SKIP_COUNT:    expr = "t.skip_count";     cls = MN_FC_INT; break;
        case MN_PL_FIELD_RATING:        expr = "t.rating_x2";      cls = MN_FC_INT; break;

        case MN_PL_FIELD_DATE_ADDED:    expr = "t.date_added";  cls = MN_FC_DATE; break;
        case MN_PL_FIELD_LAST_PLAYED:   expr = "t.last_played"; cls = MN_FC_DATE; break;
        case MN_PL_FIELD_DATE_MODIFIED: expr = "t.mtime";       cls = MN_FC_DATE; break;

        case MN_PL_FIELD_LOVED:         expr = "0";           cls = MN_FC_BOOL; break;
        case MN_PL_FIELD_HAS_LYRICS:    expr = "0";           cls = MN_FC_BOOL; break;
        case MN_PL_FIELD_HAS_ARTWORK:   expr = "t.has_art";   cls = MN_FC_BOOL; break;

        case MN_PL_FIELD_PLAYLIST_REF:  expr = "t.id";        cls = MN_FC_PLAYLIST; break;
        default:
            return false;
    }
    if (out_expr) {
        *out_expr = expr;
    }
    if (out_class) {
        *out_class = cls;
    }
    return true;
}

/* Map an mn_pl_sort_key to a sortable SQL expression over `tracks` t. */
static const char *mn_sort_sql(mn_pl_sort_key key) {
    switch (key) {
        case MN_PL_SORT_NONE:         return "t.id";
        case MN_PL_SORT_TITLE:        return "t.title COLLATE NOCASE";
        case MN_PL_SORT_ARTIST:       return "t.artist COLLATE NOCASE";
        case MN_PL_SORT_ALBUM:        return "t.album COLLATE NOCASE";
        case MN_PL_SORT_ALBUM_ARTIST: return "t.album_artist COLLATE NOCASE";
        case MN_PL_SORT_YEAR:         return "t.year";
        case MN_PL_SORT_GENRE:        return "t.genre COLLATE NOCASE";
        case MN_PL_SORT_DURATION:     return "t.duration_ms";
        case MN_PL_SORT_DATE_ADDED:   return "t.date_added";
        case MN_PL_SORT_LAST_PLAYED:  return "t.last_played";
        case MN_PL_SORT_PLAY_COUNT:   return "t.play_count";
        case MN_PL_SORT_RATING:       return "t.rating_x2";
        case MN_PL_SORT_TRACK_NO:     return "t.disc, t.track";
        case MN_PL_SORT_BPM:          return "t.id"; /* no BPM column */
        case MN_PL_SORT_RANDOM:       return "random()";
        default:                      return NULL;
    }
}

/* ------------------------------------------------------------------------- *
 * SQL compilation of a rule tree
 *
 * The compiler walks the tree emitting a WHERE clause with `?` placeholders,
 * and records the ordered list of bind values so they can be bound to a
 * prepared statement in one pass. Text bind values are copied into the bind
 * table so the tree's borrowed strings need not outlive the compile call.
 * ------------------------------------------------------------------------- */

typedef enum mn_bind_kind {
    MN_BIND_INT = 0,
    MN_BIND_TEXT
} mn_bind_kind;

typedef struct mn_bind {
    mn_bind_kind kind;
    int64_t      i;      /* MN_BIND_INT                                      */
    char        *text;   /* MN_BIND_TEXT, heap owned                        */
} mn_bind;

typedef struct mn_compiler {
    mn_buf   where;      /* WHERE expression text (no leading "WHERE ")     */
    mn_bind *binds;
    size_t   bind_count;
    size_t   bind_cap;
    mn_playlist_status err;
    /* "now" captured once so relative-date rules are stable per compile. */
    int64_t  now;
} mn_compiler;

static void mn_compiler_init(mn_compiler *c) {
    mn_buf_init(&c->where);
    c->binds = NULL;
    c->bind_count = 0;
    c->bind_cap = 0;
    c->err = MN_PLAYLIST_OK;
    c->now = (int64_t)time(NULL);
}

static void mn_compiler_free(mn_compiler *c) {
    mn_buf_free(&c->where);
    if (c->binds != NULL) {
        for (size_t i = 0; i < c->bind_count; ++i) {
            if (c->binds[i].kind == MN_BIND_TEXT) {
                free(c->binds[i].text);
            }
        }
        free(c->binds);
    }
    c->binds = NULL;
    c->bind_count = 0;
    c->bind_cap = 0;
}

static bool mn_compiler_grow_binds(mn_compiler *c) {
    if (c->bind_count < c->bind_cap) {
        return true;
    }
    size_t ncap = (c->bind_cap != 0) ? c->bind_cap * 2 : 8;
    mn_bind *nb = (mn_bind *)realloc(c->binds, ncap * sizeof(*nb));
    if (nb == NULL) {
        c->err = MN_PLAYLIST_ERR_NOMEM;
        return false;
    }
    c->binds = nb;
    c->bind_cap = ncap;
    return true;
}

static void mn_bind_int(mn_compiler *c, int64_t v) {
    if (c->err != MN_PLAYLIST_OK || !mn_compiler_grow_binds(c)) {
        return;
    }
    c->binds[c->bind_count].kind = MN_BIND_INT;
    c->binds[c->bind_count].i = v;
    c->binds[c->bind_count].text = NULL;
    c->bind_count++;
}

/* Add a text bind, copying `n` bytes (or strlen if n==0 and ptr!=NULL). */
static void mn_bind_text(mn_compiler *c, const char *ptr, size_t n) {
    if (c->err != MN_PLAYLIST_OK || !mn_compiler_grow_binds(c)) {
        return;
    }
    if (ptr == NULL) {
        ptr = "";
        n = 0;
    } else if (n == 0) {
        n = strlen(ptr);
    }
    char *copy = mn_strndup(ptr, n);
    if (copy == NULL) {
        c->err = MN_PLAYLIST_ERR_NOMEM;
        return;
    }
    c->binds[c->bind_count].kind = MN_BIND_TEXT;
    c->binds[c->bind_count].i = 0;
    c->binds[c->bind_count].text = copy;
    c->bind_count++;
}

/* Read the text pointer/length out of a value, honoring the len==0 rule. */
static bool mn_value_text(const mn_pl_value *v, const char **out_ptr,
                          size_t *out_len) {
    if (v->type != MN_PL_VT_TEXT) {
        return false;
    }
    const char *p = v->v.text.ptr;
    size_t n = v->v.text.len;
    if (p == NULL) {
        p = "";
        n = 0;
    } else if (n == 0) {
        n = strlen(p);
    }
    *out_ptr = p;
    *out_len = n;
    return true;
}

/* Escape a LIKE pattern literal so %, _ and the escape char are neutralized,
 * then wrap it with the requested leading/trailing wildcards. Appends the
 * finished pattern to a fresh heap string via mn_buf; caller binds it as text.
 * ESCAPE '\' is emitted alongside by the caller. */
static char *mn_like_pattern(const char *s, size_t n, bool lead, bool trail) {
    mn_buf b;
    mn_buf_init(&b);
    if (lead) {
        mn_buf_putc(&b, '%');
    }
    for (size_t i = 0; i < n; ++i) {
        char ch = s[i];
        if (ch == '%' || ch == '_' || ch == '\\') {
            mn_buf_putc(&b, '\\');
        }
        mn_buf_putc(&b, ch);
    }
    if (trail) {
        mn_buf_putc(&b, '%');
    }
    if (b.oom) {
        mn_buf_free(&b);
        return NULL;
    }
    const char *cs = mn_buf_cstr(&b);
    if (cs == NULL) {
        mn_buf_free(&b);
        return NULL;
    }
    return b.data;   /* transfer ownership */
}

static void mn_emit_node(mn_compiler *c, const mn_pl_node *node);

/* Emit a single condition leaf. */
static void mn_emit_condition(mn_compiler *c, const mn_pl_node *node) {
    const char *expr = NULL;
    mn_field_class cls = MN_FC_TEXT;
    if (!mn_field_sql(node->field, &expr, &cls)) {
        c->err = MN_PLAYLIST_ERR_RULE;
        return;
    }

    mn_buf *w = &c->where;

    switch (node->op) {
        /* ---- Universal equality ---- */
        case MN_PL_OP_EQ:
        case MN_PL_OP_NE: {
            const char *cmp = (node->op == MN_PL_OP_EQ) ? "=" : "<>";
            if (cls == MN_FC_TEXT) {
                const char *p; size_t n;
                if (!mn_value_text(&node->value, &p, &n)) {
                    c->err = MN_PLAYLIST_ERR_RULE; return;
                }
                mn_buf_printf(w, "(%s COLLATE NOCASE %s ?)", expr, cmp);
                mn_bind_text(c, p, n);
            } else {
                int64_t v;
                if (node->value.type == MN_PL_VT_TEXT) {
                    c->err = MN_PLAYLIST_ERR_RULE; return;
                }
                v = (node->value.type == MN_PL_VT_PLAYLIST)
                        ? (int64_t)node->value.v.playlist
                        : node->value.v.i;
                mn_buf_printf(w, "(%s %s ?)", expr, cmp);
                mn_bind_int(c, v);
            }
            break;
        }

        /* ---- Numeric / date ordering ---- */
        case MN_PL_OP_GT:
        case MN_PL_OP_GE:
        case MN_PL_OP_LT:
        case MN_PL_OP_LE: {
            if (cls != MN_FC_INT && cls != MN_FC_DATE) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            if (node->value.type == MN_PL_VT_TEXT) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            const char *cmp = (node->op == MN_PL_OP_GT) ? ">"
                            : (node->op == MN_PL_OP_GE) ? ">="
                            : (node->op == MN_PL_OP_LT) ? "<" : "<=";
            mn_buf_printf(w, "(%s %s ?)", expr, cmp);
            mn_bind_int(c, node->value.v.i);
            break;
        }

        case MN_PL_OP_BETWEEN: {
            if (cls != MN_FC_INT && cls != MN_FC_DATE) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            if (node->value.type == MN_PL_VT_TEXT) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            mn_buf_printf(w, "(%s BETWEEN ? AND ?)", expr);
            mn_bind_int(c, node->value.v.i);
            mn_bind_int(c, node->value.v.i2);
            break;
        }

        /* ---- Text operators ---- */
        case MN_PL_OP_CONTAINS:
        case MN_PL_OP_NOT_CONTAINS:
        case MN_PL_OP_STARTS_WITH:
        case MN_PL_OP_ENDS_WITH: {
            if (cls != MN_FC_TEXT) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            const char *p; size_t n;
            if (!mn_value_text(&node->value, &p, &n)) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            bool lead  = (node->op == MN_PL_OP_CONTAINS ||
                          node->op == MN_PL_OP_NOT_CONTAINS ||
                          node->op == MN_PL_OP_ENDS_WITH);
            bool trail = (node->op == MN_PL_OP_CONTAINS ||
                          node->op == MN_PL_OP_NOT_CONTAINS ||
                          node->op == MN_PL_OP_STARTS_WITH);
            char *pat = mn_like_pattern(p, n, lead, trail);
            if (pat == NULL) {
                c->err = MN_PLAYLIST_ERR_NOMEM; return;
            }
            const char *neg = (node->op == MN_PL_OP_NOT_CONTAINS) ? "NOT " : "";
            /* IFNULL so NULL columns don't swallow NOT LIKE unexpectedly. */
            mn_buf_printf(w, "(IFNULL(%s,'') %sLIKE ? ESCAPE '\\')", expr, neg);
            mn_bind_text(c, pat, strlen(pat));
            free(pat);
            break;
        }

        case MN_PL_OP_MATCHES: {
            /* FTS5 MATCH against a companion tracks_fts table indexed by
             * rowid == tracks.id. Falls back gracefully if that table is
             * absent (the subquery simply yields no rows). */
            if (cls != MN_FC_TEXT) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            const char *p; size_t n;
            if (!mn_value_text(&node->value, &p, &n)) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            mn_buf_puts(w, "(t.id IN (SELECT rowid FROM tracks_fts "
                           "WHERE tracks_fts MATCH ?))");
            mn_bind_text(c, p, n);
            break;
        }

        /* ---- Emptiness (value ignored) ---- */
        case MN_PL_OP_IS_EMPTY:
        case MN_PL_OP_IS_NOT_EMPTY: {
            bool empty = (node->op == MN_PL_OP_IS_EMPTY);
            if (cls == MN_FC_TEXT) {
                if (empty) {
                    mn_buf_printf(w, "(%s IS NULL OR %s = '')", expr, expr);
                } else {
                    mn_buf_printf(w, "(%s IS NOT NULL AND %s <> '')", expr, expr);
                }
            } else {
                /* For non-text, "empty" means NULL or zero. */
                if (empty) {
                    mn_buf_printf(w, "(%s IS NULL OR %s = 0)", expr, expr);
                } else {
                    mn_buf_printf(w, "(%s IS NOT NULL AND %s <> 0)", expr, expr);
                }
            }
            break;
        }

        /* ---- Relative-date windows ---- */
        case MN_PL_OP_IN_LAST_DAYS:
        case MN_PL_OP_NOT_IN_LAST_DAYS: {
            if (cls != MN_FC_DATE) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            if (node->value.type == MN_PL_VT_TEXT) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            int64_t days = node->value.v.i;
            int64_t cutoff = c->now - days * 86400;
            if (node->op == MN_PL_OP_IN_LAST_DAYS) {
                mn_buf_printf(w, "(%s >= ? AND %s <= ?)", expr, expr);
                mn_bind_int(c, cutoff);
                mn_bind_int(c, c->now);
            } else {
                mn_buf_printf(w, "(%s IS NULL OR %s < ?)", expr, expr);
                mn_bind_int(c, cutoff);
            }
            break;
        }

        /* ---- Playlist membership ---- */
        case MN_PL_OP_IN_PLAYLIST:
        case MN_PL_OP_NOT_IN_PLAYLIST: {
            if (node->field != MN_PL_FIELD_PLAYLIST_REF) {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            int64_t pid;
            if (node->value.type == MN_PL_VT_PLAYLIST) {
                pid = (int64_t)node->value.v.playlist;
            } else if (node->value.type == MN_PL_VT_INT) {
                pid = node->value.v.i;
            } else {
                c->err = MN_PLAYLIST_ERR_RULE; return;
            }
            const char *neg = (node->op == MN_PL_OP_NOT_IN_PLAYLIST) ? "NOT " : "";
            mn_buf_printf(w, "(t.id %sIN (SELECT track_id FROM playlist_members "
                             "WHERE playlist_id = ?))", neg);
            mn_bind_int(c, pid);
            break;
        }

        default:
            c->err = MN_PLAYLIST_ERR_RULE;
            return;
    }
}

/* Emit a group node: (child0 CONJ child1 CONJ ...). Empty groups emit a
 * constant so they are harmless. */
static void mn_emit_group(mn_compiler *c, const mn_pl_node *node) {
    if (node->child_count == 0) {
        /* An empty AND matches everything; an empty OR matches nothing. */
        mn_buf_puts(&c->where, (node->conj == MN_PL_CONJ_OR) ? "(0)" : "(1)");
        return;
    }
    if (node->children == NULL) {
        c->err = MN_PLAYLIST_ERR_RULE;
        return;
    }
    const char *conj = (node->conj == MN_PL_CONJ_OR) ? " OR " : " AND ";
    mn_buf_putc(&c->where, '(');
    for (size_t i = 0; i < node->child_count; ++i) {
        if (i != 0) {
            mn_buf_puts(&c->where, conj);
        }
        if (node->children[i] == NULL) {
            c->err = MN_PLAYLIST_ERR_RULE;
            return;
        }
        mn_emit_node(c, node->children[i]);
        if (c->err != MN_PLAYLIST_OK) {
            return;
        }
    }
    mn_buf_putc(&c->where, ')');
}

static void mn_emit_node(mn_compiler *c, const mn_pl_node *node) {
    if (c->err != MN_PLAYLIST_OK) {
        return;
    }
    if (node == NULL) {
        c->err = MN_PLAYLIST_ERR_RULE;
        return;
    }
    if (node->negate) {
        mn_buf_puts(&c->where, "(NOT ");
    }
    if (node->kind == MN_PL_NODE_CONDITION) {
        mn_emit_condition(c, node);
    } else if (node->kind == MN_PL_NODE_GROUP) {
        mn_emit_group(c, node);
    } else {
        c->err = MN_PLAYLIST_ERR_RULE;
    }
    if (node->negate) {
        mn_buf_putc(&c->where, ')');
    }
}

/*
 * Compile `rules` into a full SELECT statement.
 *
 *   count_only : emit "SELECT COUNT(*) ..." with no ORDER/LIMIT/window.
 *   otherwise  : emit "SELECT t.id ..." with ORDER BY + LIMIT/OFFSET.
 *
 * The rule-defined limit and the caller window (offset/limit) are combined:
 * the effective row cap is min(rule_limit_remaining_after_offset, window).
 * Window offset/limit are emitted as literals (they are integers we control),
 * while all rule values are parameterized.
 */
static mn_playlist_status mn_compile_select(const mn_pl_rules *rules,
                                            bool count_only,
                                            int64_t offset,
                                            int64_t limit,
                                            mn_compiler *c,
                                            char **out_sql) {
    mn_compiler_init(c);

    if (rules != NULL && rules->root != NULL) {
        mn_emit_node(c, rules->root);
        if (c->err != MN_PLAYLIST_OK) {
            return c->err;
        }
    }

    int64_t rule_limit = (rules != NULL) ? rules->limit : MN_PL_LIMIT_NONE;

    mn_buf sql;
    mn_buf_init(&sql);

    if (count_only) {
        /* Count is bounded by the rule limit if any. */
        if (rule_limit >= 0) {
            mn_buf_puts(&sql, "SELECT COUNT(*) FROM (SELECT t.id FROM tracks t");
            if (c->where.len > 0) {
                mn_buf_puts(&sql, " WHERE ");
                mn_buf_puts(&sql, mn_buf_cstr(&c->where));
            }
            mn_buf_printf(&sql, " LIMIT %lld)", (long long)rule_limit);
        } else {
            mn_buf_puts(&sql, "SELECT COUNT(*) FROM tracks t");
            if (c->where.len > 0) {
                mn_buf_puts(&sql, " WHERE ");
                mn_buf_puts(&sql, mn_buf_cstr(&c->where));
            }
        }
    } else {
        mn_buf_puts(&sql, "SELECT t.id FROM tracks t");
        if (c->where.len > 0) {
            mn_buf_puts(&sql, " WHERE ");
            mn_buf_puts(&sql, mn_buf_cstr(&c->where));
        }

        /* ORDER BY */
        mn_pl_sort_key key = (rules != NULL) ? rules->sort_key : MN_PL_SORT_NONE;
        const char *sort_expr = mn_sort_sql(key);
        if (sort_expr == NULL) {
            mn_buf_free(&sql);
            return MN_PLAYLIST_ERR_RULE;
        }
        if (key != MN_PL_SORT_NONE) {
            const char *dir = (rules != NULL && rules->sort_dir == MN_PL_SORT_DESC)
                                  ? " DESC" : " ASC";
            if (key == MN_PL_SORT_RANDOM) {
                mn_buf_printf(&sql, " ORDER BY %s", sort_expr);
            } else if (key == MN_PL_SORT_TRACK_NO) {
                /* Multi-column key: apply direction to both, then id tiebreak. */
                mn_buf_printf(&sql, " ORDER BY t.disc%s, t.track%s, t.id",
                              dir, dir);
            } else {
                mn_buf_printf(&sql, " ORDER BY %s%s, t.id", sort_expr, dir);
            }
        } else {
            mn_buf_puts(&sql, " ORDER BY t.id");
        }

        /* Combine window with rule limit. Compute effective LIMIT/OFFSET. */
        if (offset < 0) {
            offset = 0;
        }
        int64_t eff_limit; /* -1 means "no cap" */
        if (limit < 0) {
            eff_limit = -1;
        } else {
            eff_limit = limit;
        }
        if (rule_limit >= 0) {
            /* Rows available from the rule after skipping `offset`. */
            int64_t avail = rule_limit - offset;
            if (avail < 0) {
                avail = 0;
            }
            if (eff_limit < 0 || eff_limit > avail) {
                eff_limit = avail;
            }
        }
        if (eff_limit < 0) {
            /* No cap: SQLite requires LIMIT for OFFSET, use -1 sentinel. */
            mn_buf_printf(&sql, " LIMIT -1 OFFSET %lld", (long long)offset);
        } else {
            mn_buf_printf(&sql, " LIMIT %lld OFFSET %lld",
                          (long long)eff_limit, (long long)offset);
        }
    }

    if (sql.oom || c->where.oom) {
        mn_buf_free(&sql);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    const char *cs = mn_buf_cstr(&sql);
    if (cs == NULL) {
        mn_buf_free(&sql);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    *out_sql = sql.data;   /* transfer ownership */
    return MN_PLAYLIST_OK;
}

/* Bind the compiler's recorded values to a prepared statement, starting at
 * bind index 1. */
static mn_playlist_status mn_bind_all(sqlite3_stmt *st, const mn_compiler *c) {
    for (size_t i = 0; i < c->bind_count; ++i) {
        int idx = (int)(i + 1);
        int rc;
        if (c->binds[i].kind == MN_BIND_INT) {
            rc = sqlite3_bind_int64(st, idx, c->binds[i].i);
        } else {
            rc = sqlite3_bind_text(st, idx, c->binds[i].text, -1,
                                   SQLITE_TRANSIENT);
        }
        if (rc != SQLITE_OK) {
            return mn_from_sqlite(rc);
        }
    }
    return MN_PLAYLIST_OK;
}

/* ------------------------------------------------------------------------- *
 * JSON serialization of a rule tree
 *
 * A compact, self-contained JSON schema mirroring the rule structs:
 *   { "sort_key":N, "sort_dir":N, "limit":N, "root": <node|null> }
 *   node    := { "t":"cond", "neg":bool, "field":N, "op":N, "vt":N, "v":... }
 *            | { "t":"group","neg":bool, "conj":N, "children":[node,...] }
 * ------------------------------------------------------------------------- */

static void mn_json_escape(mn_buf *b, const char *s, size_t n) {
    mn_buf_putc(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"':  mn_buf_puts(b, "\\\""); break;
            case '\\': mn_buf_puts(b, "\\\\"); break;
            case '\b': mn_buf_puts(b, "\\b");  break;
            case '\f': mn_buf_puts(b, "\\f");  break;
            case '\n': mn_buf_puts(b, "\\n");  break;
            case '\r': mn_buf_puts(b, "\\r");  break;
            case '\t': mn_buf_puts(b, "\\t");  break;
            default:
                if (ch < 0x20) {
                    mn_buf_printf(b, "\\u%04x", (unsigned)ch);
                } else {
                    mn_buf_putc(b, (char)ch);
                }
        }
    }
    mn_buf_putc(b, '"');
}

static void mn_json_value(mn_buf *b, const mn_pl_value *v) {
    mn_buf_printf(b, "\"vt\":%d,", (int)v->type);
    switch (v->type) {
        case MN_PL_VT_TEXT: {
            const char *p = v->v.text.ptr;
            size_t n = v->v.text.len;
            if (p == NULL) { p = ""; n = 0; }
            else if (n == 0) { n = strlen(p); }
            mn_buf_puts(b, "\"v\":");
            mn_json_escape(b, p, n);
            break;
        }
        case MN_PL_VT_INT:
        case MN_PL_VT_BOOL:
            mn_buf_printf(b, "\"v\":%lld,\"v2\":%lld",
                          (long long)v->v.i, (long long)v->v.i2);
            break;
        case MN_PL_VT_PLAYLIST:
            mn_buf_printf(b, "\"v\":%lld", (long long)v->v.playlist);
            break;
        default:
            mn_buf_puts(b, "\"v\":0");
            break;
    }
}

static void mn_json_node(mn_buf *b, const mn_pl_node *node) {
    if (node == NULL) {
        mn_buf_puts(b, "null");
        return;
    }
    mn_buf_putc(b, '{');
    if (node->kind == MN_PL_NODE_CONDITION) {
        mn_buf_printf(b, "\"t\":\"cond\",\"neg\":%s,\"field\":%d,\"op\":%d,",
                      node->negate ? "true" : "false",
                      (int)node->field, (int)node->op);
        mn_json_value(b, &node->value);
    } else {
        mn_buf_printf(b, "\"t\":\"group\",\"neg\":%s,\"conj\":%d,\"children\":[",
                      node->negate ? "true" : "false", (int)node->conj);
        for (size_t i = 0; i < node->child_count; ++i) {
            if (i != 0) {
                mn_buf_putc(b, ',');
            }
            mn_json_node(b, node->children ? node->children[i] : NULL);
        }
        mn_buf_putc(b, ']');
    }
    mn_buf_putc(b, '}');
}

/* Serialize rules to a heap JSON string. Caller frees. */
static mn_playlist_status mn_rules_to_json(const mn_pl_rules *rules,
                                           char **out_json) {
    mn_buf b;
    mn_buf_init(&b);
    int64_t limit = (rules != NULL) ? rules->limit : MN_PL_LIMIT_NONE;
    int sk = (rules != NULL) ? (int)rules->sort_key : MN_PL_SORT_NONE;
    int sd = (rules != NULL) ? (int)rules->sort_dir : MN_PL_SORT_ASC;
    mn_buf_printf(&b, "{\"sort_key\":%d,\"sort_dir\":%d,\"limit\":%lld,\"root\":",
                  sk, sd, (long long)limit);
    mn_json_node(&b, (rules != NULL) ? rules->root : NULL);
    mn_buf_putc(&b, '}');
    if (b.oom) {
        mn_buf_free(&b);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    if (mn_buf_cstr(&b) == NULL) {
        mn_buf_free(&b);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    *out_json = b.data;
    return MN_PLAYLIST_OK;
}

/* ------------------------------------------------------------------------- *
 * JSON parsing (a minimal recursive-descent parser for our own schema)
 * ------------------------------------------------------------------------- */

typedef struct mn_json_parser {
    const char *p;
    const char *end;
    mn_playlist_status err;
} mn_json_parser;

static void mn_jp_skip_ws(mn_json_parser *jp) {
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            jp->p++;
        } else {
            break;
        }
    }
}

static bool mn_jp_eat(mn_json_parser *jp, char c) {
    mn_jp_skip_ws(jp);
    if (jp->p < jp->end && *jp->p == c) {
        jp->p++;
        return true;
    }
    return false;
}

static bool mn_jp_peek(mn_json_parser *jp, char c) {
    mn_jp_skip_ws(jp);
    return (jp->p < jp->end && *jp->p == c);
}

/* Match a bare literal like true/false/null. */
static bool mn_jp_lit(mn_json_parser *jp, const char *lit) {
    mn_jp_skip_ws(jp);
    size_t n = strlen(lit);
    if ((size_t)(jp->end - jp->p) >= n && memcmp(jp->p, lit, n) == 0) {
        jp->p += n;
        return true;
    }
    return false;
}

/* Parse a JSON string into a freshly allocated heap buffer (unescaped, UTF-8).
 * On success *out is heap-owned; caller frees. */
static bool mn_jp_string(mn_json_parser *jp, char **out) {
    mn_jp_skip_ws(jp);
    if (jp->p >= jp->end || *jp->p != '"') {
        jp->err = MN_PLAYLIST_ERR_PARSE;
        return false;
    }
    jp->p++;
    mn_buf b;
    mn_buf_init(&b);
    while (jp->p < jp->end) {
        char c = *jp->p++;
        if (c == '"') {
            if (b.oom) {
                mn_buf_free(&b);
                jp->err = MN_PLAYLIST_ERR_NOMEM;
                return false;
            }
            if (mn_buf_cstr(&b) == NULL) {
                mn_buf_free(&b);
                jp->err = MN_PLAYLIST_ERR_NOMEM;
                return false;
            }
            *out = b.data;
            return true;
        }
        if (c == '\\') {
            if (jp->p >= jp->end) {
                break;
            }
            char e = *jp->p++;
            switch (e) {
                case '"':  mn_buf_putc(&b, '"');  break;
                case '\\': mn_buf_putc(&b, '\\'); break;
                case '/':  mn_buf_putc(&b, '/');  break;
                case 'b':  mn_buf_putc(&b, '\b'); break;
                case 'f':  mn_buf_putc(&b, '\f'); break;
                case 'n':  mn_buf_putc(&b, '\n'); break;
                case 'r':  mn_buf_putc(&b, '\r'); break;
                case 't':  mn_buf_putc(&b, '\t'); break;
                case 'u': {
                    if (jp->end - jp->p < 4) {
                        mn_buf_free(&b);
                        jp->err = MN_PLAYLIST_ERR_PARSE;
                        return false;
                    }
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = *jp->p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { mn_buf_free(&b); jp->err = MN_PLAYLIST_ERR_PARSE; return false; }
                    }
                    /* Encode the BMP code point as UTF-8 (no surrogate pairs;
                     * our serializer never emits them). */
                    if (cp < 0x80) {
                        mn_buf_putc(&b, (char)cp);
                    } else if (cp < 0x800) {
                        mn_buf_putc(&b, (char)(0xC0 | (cp >> 6)));
                        mn_buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        mn_buf_putc(&b, (char)(0xE0 | (cp >> 12)));
                        mn_buf_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        mn_buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    mn_buf_free(&b);
                    jp->err = MN_PLAYLIST_ERR_PARSE;
                    return false;
            }
        } else {
            mn_buf_putc(&b, c);
        }
    }
    mn_buf_free(&b);
    jp->err = MN_PLAYLIST_ERR_PARSE;
    return false;
}

/* Parse a JSON integer (optionally negative). */
static bool mn_jp_int(mn_json_parser *jp, int64_t *out) {
    mn_jp_skip_ws(jp);
    const char *start = jp->p;
    bool neg = false;
    if (jp->p < jp->end && (*jp->p == '-' || *jp->p == '+')) {
        neg = (*jp->p == '-');
        jp->p++;
    }
    int64_t val = 0;
    bool any = false;
    while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') {
        val = val * 10 + (*jp->p - '0');
        jp->p++;
        any = true;
    }
    /* Skip any fractional/exponent portion we don't use. */
    while (jp->p < jp->end &&
           (*jp->p == '.' || *jp->p == 'e' || *jp->p == 'E' ||
            *jp->p == '+' || *jp->p == '-' ||
            (*jp->p >= '0' && *jp->p <= '9'))) {
        jp->p++;
    }
    if (!any) {
        jp->p = start;
        jp->err = MN_PLAYLIST_ERR_PARSE;
        return false;
    }
    *out = neg ? -val : val;
    return true;
}

/* Skip an arbitrary JSON value we don't recognize (for forward-compat). */
static void mn_jp_skip_value(mn_json_parser *jp);

static void mn_jp_skip_value(mn_json_parser *jp) {
    mn_jp_skip_ws(jp);
    if (jp->p >= jp->end) {
        jp->err = MN_PLAYLIST_ERR_PARSE;
        return;
    }
    char c = *jp->p;
    if (c == '"') {
        char *s = NULL;
        if (mn_jp_string(jp, &s)) {
            free(s);
        }
    } else if (c == '{') {
        jp->p++;
        if (mn_jp_peek(jp, '}')) { jp->p++; return; }
        do {
            char *k = NULL;
            if (!mn_jp_string(jp, &k)) { return; }
            free(k);
            if (!mn_jp_eat(jp, ':')) { jp->err = MN_PLAYLIST_ERR_PARSE; return; }
            mn_jp_skip_value(jp);
            if (jp->err != MN_PLAYLIST_OK) { return; }
        } while (mn_jp_eat(jp, ','));
        if (!mn_jp_eat(jp, '}')) { jp->err = MN_PLAYLIST_ERR_PARSE; }
    } else if (c == '[') {
        jp->p++;
        if (mn_jp_peek(jp, ']')) { jp->p++; return; }
        do {
            mn_jp_skip_value(jp);
            if (jp->err != MN_PLAYLIST_OK) { return; }
        } while (mn_jp_eat(jp, ','));
        if (!mn_jp_eat(jp, ']')) { jp->err = MN_PLAYLIST_ERR_PARSE; }
    } else if (mn_jp_lit(jp, "true") || mn_jp_lit(jp, "false") ||
               mn_jp_lit(jp, "null")) {
        /* consumed */
    } else {
        int64_t tmp;
        if (!mn_jp_int(jp, &tmp)) {
            jp->err = MN_PLAYLIST_ERR_PARSE;
        }
    }
}

static mn_pl_node *mn_jp_node(mn_json_parser *jp);

/* Allocate a zeroed node. */
static mn_pl_node *mn_node_alloc(void) {
    return (mn_pl_node *)calloc(1, sizeof(mn_pl_node));
}

/* Free a node subtree we allocated during parsing. Text values are heap
 * copies owned by the node (see note in mn_jp_node). */
static void mn_node_free(mn_pl_node *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == MN_PL_NODE_GROUP && node->children != NULL) {
        for (size_t i = 0; i < node->child_count; ++i) {
            mn_node_free(node->children[i]);
        }
        free(node->children);
    }
    if (node->kind == MN_PL_NODE_CONDITION &&
        node->value.type == MN_PL_VT_TEXT) {
        /* We stored a heap copy in .ptr during parse. */
        free((void *)node->value.v.text.ptr);
    }
    free(node);
}

/* Parse one node object (or the literal null → NULL node). */
static mn_pl_node *mn_jp_node(mn_json_parser *jp) {
    mn_jp_skip_ws(jp);
    if (mn_jp_lit(jp, "null")) {
        return NULL;
    }
    if (!mn_jp_eat(jp, '{')) {
        jp->err = MN_PLAYLIST_ERR_PARSE;
        return NULL;
    }
    mn_pl_node *node = mn_node_alloc();
    if (node == NULL) {
        jp->err = MN_PLAYLIST_ERR_NOMEM;
        return NULL;
    }

    /* Defaults. */
    node->kind = MN_PL_NODE_CONDITION;
    bool have_vt = false;

    if (mn_jp_peek(jp, '}')) {
        jp->p++;
        return node;
    }
    do {
        char *key = NULL;
        if (!mn_jp_string(jp, &key)) {
            mn_node_free(node);
            return NULL;
        }
        if (!mn_jp_eat(jp, ':')) {
            free(key);
            jp->err = MN_PLAYLIST_ERR_PARSE;
            mn_node_free(node);
            return NULL;
        }

        if (strcmp(key, "t") == 0) {
            char *tv = NULL;
            if (!mn_jp_string(jp, &tv)) { free(key); mn_node_free(node); return NULL; }
            node->kind = (strcmp(tv, "group") == 0) ? MN_PL_NODE_GROUP
                                                    : MN_PL_NODE_CONDITION;
            free(tv);
        } else if (strcmp(key, "neg") == 0) {
            if (mn_jp_lit(jp, "true")) node->negate = true;
            else if (mn_jp_lit(jp, "false")) node->negate = false;
            else { free(key); jp->err = MN_PLAYLIST_ERR_PARSE; mn_node_free(node); return NULL; }
        } else if (strcmp(key, "field") == 0) {
            int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
            node->field = (mn_pl_field)v;
        } else if (strcmp(key, "op") == 0) {
            int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
            node->op = (mn_pl_op)v;
        } else if (strcmp(key, "conj") == 0) {
            int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
            node->conj = (mn_pl_conj)v;
        } else if (strcmp(key, "vt") == 0) {
            int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
            node->value.type = (mn_pl_value_type)v;
            have_vt = true;
        } else if (strcmp(key, "v") == 0) {
            /* Interpretation depends on vt, which precedes v in our schema. */
            if (have_vt && node->value.type == MN_PL_VT_TEXT) {
                char *sv = NULL;
                if (!mn_jp_string(jp, &sv)) { free(key); mn_node_free(node); return NULL; }
                /* Store heap copy; len=strlen so compiler treats it correctly. */
                node->value.v.text.ptr = sv;
                node->value.v.text.len = strlen(sv);
            } else if (have_vt && node->value.type == MN_PL_VT_PLAYLIST) {
                int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
                node->value.v.playlist = (mn_playlist_id)v;
            } else {
                int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
                node->value.v.i = v;
            }
        } else if (strcmp(key, "v2") == 0) {
            int64_t v; if (!mn_jp_int(jp, &v)) { free(key); mn_node_free(node); return NULL; }
            node->value.v.i2 = v;
        } else if (strcmp(key, "children") == 0) {
            if (!mn_jp_eat(jp, '[')) { free(key); jp->err = MN_PLAYLIST_ERR_PARSE; mn_node_free(node); return NULL; }
            if (!mn_jp_peek(jp, ']')) {
                do {
                    mn_pl_node *child = mn_jp_node(jp);
                    if (jp->err != MN_PLAYLIST_OK) {
                        mn_node_free(child);
                        free(key);
                        mn_node_free(node);
                        return NULL;
                    }
                    mn_pl_node **nc = (mn_pl_node **)realloc(
                        node->children,
                        (node->child_count + 1) * sizeof(*nc));
                    if (nc == NULL) {
                        mn_node_free(child);
                        free(key);
                        jp->err = MN_PLAYLIST_ERR_NOMEM;
                        mn_node_free(node);
                        return NULL;
                    }
                    node->children = nc;
                    node->children[node->child_count++] = child;
                } while (mn_jp_eat(jp, ','));
            }
            if (!mn_jp_eat(jp, ']')) { free(key); jp->err = MN_PLAYLIST_ERR_PARSE; mn_node_free(node); return NULL; }
        } else {
            /* Unknown key: skip its value for forward compatibility. */
            mn_jp_skip_value(jp);
            if (jp->err != MN_PLAYLIST_OK) { free(key); mn_node_free(node); return NULL; }
        }
        free(key);
    } while (mn_jp_eat(jp, ','));

    if (!mn_jp_eat(jp, '}')) {
        jp->err = MN_PLAYLIST_ERR_PARSE;
        mn_node_free(node);
        return NULL;
    }
    return node;
}

/* Parse a full rules JSON document into a heap mn_pl_rules. Caller frees with
 * mn_pl_rules_free(). */
static mn_playlist_status mn_json_to_rules(const char *json, size_t len,
                                           mn_pl_rules **out) {
    if (json == NULL || out == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_json_parser jp;
    jp.p = json;
    jp.end = json + len;
    jp.err = MN_PLAYLIST_OK;

    mn_pl_rules *rules = (mn_pl_rules *)calloc(1, sizeof(*rules));
    if (rules == NULL) {
        return MN_PLAYLIST_ERR_NOMEM;
    }
    rules->limit = MN_PL_LIMIT_NONE;
    rules->sort_key = MN_PL_SORT_NONE;
    rules->sort_dir = MN_PL_SORT_ASC;
    rules->root = NULL;

    if (!mn_jp_eat(&jp, '{')) {
        free(rules);
        return MN_PLAYLIST_ERR_PARSE;
    }
    if (!mn_jp_peek(&jp, '}')) {
        do {
            char *key = NULL;
            if (!mn_jp_string(&jp, &key)) {
                mn_pl_rules_free(rules);
                return jp.err ? jp.err : MN_PLAYLIST_ERR_PARSE;
            }
            if (!mn_jp_eat(&jp, ':')) {
                free(key);
                mn_pl_rules_free(rules);
                return MN_PLAYLIST_ERR_PARSE;
            }
            if (strcmp(key, "sort_key") == 0) {
                int64_t v; if (!mn_jp_int(&jp, &v)) { free(key); mn_pl_rules_free(rules); return MN_PLAYLIST_ERR_PARSE; }
                rules->sort_key = (mn_pl_sort_key)v;
            } else if (strcmp(key, "sort_dir") == 0) {
                int64_t v; if (!mn_jp_int(&jp, &v)) { free(key); mn_pl_rules_free(rules); return MN_PLAYLIST_ERR_PARSE; }
                rules->sort_dir = (mn_pl_sort_dir)v;
            } else if (strcmp(key, "limit") == 0) {
                int64_t v; if (!mn_jp_int(&jp, &v)) { free(key); mn_pl_rules_free(rules); return MN_PLAYLIST_ERR_PARSE; }
                rules->limit = v;
            } else if (strcmp(key, "root") == 0) {
                rules->root = mn_jp_node(&jp);
                if (jp.err != MN_PLAYLIST_OK) {
                    free(key);
                    mn_pl_rules_free(rules);
                    return jp.err;
                }
            } else {
                mn_jp_skip_value(&jp);
                if (jp.err != MN_PLAYLIST_OK) { free(key); mn_pl_rules_free(rules); return jp.err; }
            }
            free(key);
        } while (mn_jp_eat(&jp, ','));
    }
    if (!mn_jp_eat(&jp, '}')) {
        mn_pl_rules_free(rules);
        return MN_PLAYLIST_ERR_PARSE;
    }
    *out = rules;
    return MN_PLAYLIST_OK;
}

/* Public: free a rules tree (from get_smart_rules or our JSON parser). */
void mn_pl_rules_free(mn_pl_rules *rules) {
    if (rules == NULL) {
        return;
    }
    mn_node_free(rules->root);
    free(rules);
}

/* ------------------------------------------------------------------------- *
 * Info record helpers
 * ------------------------------------------------------------------------- */

void mn_playlist_info_dispose(mn_playlist_info *info) {
    if (info == NULL) {
        return;
    }
    free(info->name);
    info->name = NULL;
}

void mn_playlist_info_free_array(mn_playlist_info *arr, size_t count) {
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(arr[i].name);
    }
    free(arr);
}

void mn_playlists_free_ids(mn_track_id_list *list) {
    if (list == NULL) {
        return;
    }
    free(list->ids);
    list->ids = NULL;
    list->count = 0;
    list->total = -1;
}

void mn_playlists_free_sql(char *sql) {
    free(sql);
}

void mn_playlists_free_buffer(char *data) {
    free(data);
}

/* ------------------------------------------------------------------------- *
 * Internal DB helpers
 * ------------------------------------------------------------------------- */

/* Run a statement with no result rows (DDL/DML). */
static mn_playlist_status mn_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err != NULL) {
        sqlite3_free(err);
    }
    return mn_from_sqlite(rc);
}

/* Look up a playlist's kind. Returns NOT_FOUND if absent. */
static mn_playlist_status mn_lookup_kind(sqlite3 *db, mn_playlist_id id,
                                         mn_playlist_kind *out_kind) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT kind FROM playlists WHERE id=?;",
                                -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    mn_playlist_status status;
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (out_kind) {
            *out_kind = (mn_playlist_kind)sqlite3_column_int(st, 0);
        }
        status = MN_PLAYLIST_OK;
    } else if (rc == SQLITE_DONE) {
        status = MN_PLAYLIST_ERR_NOT_FOUND;
    } else {
        status = mn_from_sqlite(rc);
    }
    sqlite3_finalize(st);
    return status;
}

/* Assert a playlist exists and has the expected kind. */
static mn_playlist_status mn_require_kind(sqlite3 *db, mn_playlist_id id,
                                          mn_playlist_kind want) {
    mn_playlist_kind kind;
    mn_playlist_status s = mn_lookup_kind(db, id, &kind);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    return (kind == want) ? MN_PLAYLIST_OK : MN_PLAYLIST_ERR_WRONG_KIND;
}

/* Touch updated_at on a playlist. */
static void mn_touch(sqlite3 *db, mn_playlist_id id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE playlists SET updated_at=? WHERE id=?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)time(NULL));
        sqlite3_bind_int64(st, 2, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

/* Current dense member count of a static playlist. */
static mn_playlist_status mn_member_count(sqlite3 *db, mn_playlist_id id,
                                          int64_t *out) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM playlist_members WHERE playlist_id=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    mn_playlist_status status = MN_PLAYLIST_ERR_SQL;
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
        status = MN_PLAYLIST_OK;
    }
    sqlite3_finalize(st);
    return status;
}

/* ------------------------------------------------------------------------- *
 * Schema
 * ------------------------------------------------------------------------- */

mn_playlist_status mn_playlists_init_schema(sqlite3 *db) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }

    /* Require the tracks table to exist. */
    {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='tracks';",
            -1, &st, NULL);
        if (rc != SQLITE_OK) {
            return mn_from_sqlite(rc);
        }
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_ROW) {
            return MN_PLAYLIST_ERR_SCHEMA;
        }
    }

    static const char *ddl =
        "CREATE TABLE IF NOT EXISTS playlists("
        "  id         INTEGER PRIMARY KEY,"
        "  name       TEXT NOT NULL,"
        "  kind       INTEGER NOT NULL DEFAULT 0,"
        "  rules_json TEXT,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS playlist_members("
        "  playlist_id INTEGER NOT NULL,"
        "  position    INTEGER NOT NULL,"
        "  track_id    INTEGER NOT NULL,"
        "  PRIMARY KEY(playlist_id, position)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_pl_members_track "
        "  ON playlist_members(track_id);"
        "CREATE INDEX IF NOT EXISTS idx_playlists_updated "
        "  ON playlists(updated_at DESC);";

    return mn_exec(db, ddl);
}

/* ------------------------------------------------------------------------- *
 * Creation / deletion / rename
 * ------------------------------------------------------------------------- */

/* Shared insert of a playlist row. rules_json may be NULL (static). */
static mn_playlist_status mn_insert_playlist(sqlite3 *db, const char *name,
                                             mn_playlist_kind kind,
                                             const char *rules_json,
                                             mn_playlist_id *out_id) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO playlists(name,kind,rules_json,created_at,updated_at) "
        "VALUES(?,?,?,?,?);",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, (int)kind);
    if (rules_json != NULL) {
        sqlite3_bind_text(st, 3, rules_json, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 3);
    }
    sqlite3_bind_int64(st, 4, now);
    sqlite3_bind_int64(st, 5, now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return mn_from_sqlite(rc);
    }
    if (out_id != NULL) {
        *out_id = (mn_playlist_id)sqlite3_last_insert_rowid(db);
    }
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_create_static(sqlite3 *db, const char *name,
                                             mn_playlist_id *out_id) {
    if (db == NULL || name == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    return mn_insert_playlist(db, name, MN_PLAYLIST_KIND_STATIC, NULL, out_id);
}

/* Validate a rule tree by compiling it once (no execution). */
static mn_playlist_status mn_validate_rules(const mn_pl_rules *rules) {
    mn_compiler c;
    char *sql = NULL;
    mn_playlist_status s = mn_compile_select(rules, false, 0, 1, &c, &sql);
    mn_compiler_free(&c);
    free(sql);
    return s;
}

mn_playlist_status mn_playlist_create_smart(sqlite3 *db, const char *name,
                                            const mn_pl_rules *rules,
                                            mn_playlist_id *out_id) {
    if (db == NULL || name == NULL || rules == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_validate_rules(rules);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    char *json = NULL;
    s = mn_rules_to_json(rules, &json);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    s = mn_insert_playlist(db, name, MN_PLAYLIST_KIND_SMART, json, out_id);
    free(json);
    return s;
}

mn_playlist_status mn_playlist_set_smart_rules(sqlite3 *db, mn_playlist_id id,
                                               const mn_pl_rules *rules) {
    if (db == NULL || rules == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_SMART);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    s = mn_validate_rules(rules);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    char *json = NULL;
    s = mn_rules_to_json(rules, &json);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "UPDATE playlists SET rules_json=?, updated_at=? WHERE id=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        free(json);
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_text(st, 1, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)time(NULL));
    sqlite3_bind_int64(st, 3, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    free(json);
    return (rc == SQLITE_DONE) ? MN_PLAYLIST_OK : mn_from_sqlite(rc);
}

mn_playlist_status mn_playlist_get_smart_rules(sqlite3 *db, mn_playlist_id id,
                                               mn_pl_rules **out_rules) {
    if (db == NULL || out_rules == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT kind, rules_json FROM playlists WHERE id=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return MN_PLAYLIST_ERR_NOT_FOUND;
    }
    if (rc != SQLITE_ROW) {
        mn_playlist_status s = mn_from_sqlite(rc);
        sqlite3_finalize(st);
        return s;
    }
    mn_playlist_kind kind = (mn_playlist_kind)sqlite3_column_int(st, 0);
    if (kind != MN_PLAYLIST_KIND_SMART) {
        sqlite3_finalize(st);
        return MN_PLAYLIST_ERR_WRONG_KIND;
    }
    const unsigned char *txt = sqlite3_column_text(st, 1);
    int n = sqlite3_column_bytes(st, 1);
    mn_playlist_status s;
    if (txt == NULL) {
        /* Smart playlist with no rules => match-all default. */
        mn_pl_rules *r = (mn_pl_rules *)calloc(1, sizeof(*r));
        if (r == NULL) {
            s = MN_PLAYLIST_ERR_NOMEM;
        } else {
            r->limit = MN_PL_LIMIT_NONE;
            r->sort_key = MN_PL_SORT_NONE;
            r->sort_dir = MN_PL_SORT_ASC;
            *out_rules = r;
            s = MN_PLAYLIST_OK;
        }
    } else {
        s = mn_json_to_rules((const char *)txt, (size_t)n, out_rules);
    }
    sqlite3_finalize(st);
    return s;
}

mn_playlist_status mn_playlist_delete(sqlite3 *db, mn_playlist_id id) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    /* Confirm existence for accurate NOT_FOUND semantics. */
    mn_playlist_status s = mn_lookup_kind(db, id, NULL);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "DELETE FROM playlist_members WHERE playlist_id=?;", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    st = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM playlists WHERE id=?;",
                            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? MN_PLAYLIST_OK : mn_from_sqlite(rc);
}

mn_playlist_status mn_playlist_rename(sqlite3 *db, mn_playlist_id id,
                                      const char *new_name) {
    if (db == NULL || new_name == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_lookup_kind(db, id, NULL);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "UPDATE playlists SET name=?, updated_at=? WHERE id=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_text(st, 1, new_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)time(NULL));
    sqlite3_bind_int64(st, 3, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? MN_PLAYLIST_OK : mn_from_sqlite(rc);
}

/* ------------------------------------------------------------------------- *
 * Introspection
 * ------------------------------------------------------------------------- */

mn_playlist_status mn_playlist_get_info(sqlite3 *db, mn_playlist_id id,
                                        mn_playlist_info *out_info) {
    if (db == NULL || out_info == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT id,kind,name,created_at,updated_at FROM playlists WHERE id=?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return MN_PLAYLIST_ERR_NOT_FOUND;
    }
    if (rc != SQLITE_ROW) {
        mn_playlist_status s = mn_from_sqlite(rc);
        sqlite3_finalize(st);
        return s;
    }
    mn_playlist_info info;
    memset(&info, 0, sizeof(info));
    info.id = sqlite3_column_int64(st, 0);
    info.kind = (mn_playlist_kind)sqlite3_column_int(st, 1);
    const unsigned char *name = sqlite3_column_text(st, 2);
    info.name = mn_strdup(name ? (const char *)name : "");
    info.created_at = sqlite3_column_int64(st, 3);
    info.updated_at = sqlite3_column_int64(st, 4);
    sqlite3_finalize(st);

    if (info.name == NULL) {
        return MN_PLAYLIST_ERR_NOMEM;
    }

    if (info.kind == MN_PLAYLIST_KIND_STATIC) {
        int64_t cnt = 0;
        if (mn_member_count(db, id, &cnt) == MN_PLAYLIST_OK) {
            info.member_count = cnt;
        } else {
            info.member_count = 0;
        }
    } else {
        info.member_count = -1;   /* smart: unknown without evaluation */
    }

    *out_info = info;
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_list(sqlite3 *db, int64_t offset, int64_t limit,
                                    mn_playlist_info **out_arr, size_t *out_count,
                                    int64_t *out_total) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }

    if (out_total != NULL) {
        sqlite3_stmt *cst = NULL;
        int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM playlists;",
                                    -1, &cst, NULL);
        if (rc != SQLITE_OK) {
            return mn_from_sqlite(rc);
        }
        if (sqlite3_step(cst) == SQLITE_ROW) {
            *out_total = sqlite3_column_int64(cst, 0);
        } else {
            *out_total = 0;
        }
        sqlite3_finalize(cst);
    }

    /* limit == 0: count-only mode, no rows materialized. */
    if (limit == 0) {
        if (out_arr) {
            *out_arr = NULL;
        }
        if (out_count) {
            *out_count = 0;
        }
        return MN_PLAYLIST_OK;
    }
    if (out_arr == NULL || out_count == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }

    if (offset < 0) {
        offset = 0;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT id,kind,name,created_at,updated_at FROM playlists "
        "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, (limit < 0) ? -1 : limit);
    sqlite3_bind_int64(st, 2, offset);

    size_t cap = 0, n = 0;
    mn_playlist_info *arr = NULL;
    mn_playlist_status status = MN_PLAYLIST_OK;

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = (cap != 0) ? cap * 2 : 16;
            mn_playlist_info *na =
                (mn_playlist_info *)realloc(arr, ncap * sizeof(*na));
            if (na == NULL) {
                status = MN_PLAYLIST_ERR_NOMEM;
                break;
            }
            arr = na;
            cap = ncap;
        }
        mn_playlist_info *info = &arr[n];
        memset(info, 0, sizeof(*info));
        info->id = sqlite3_column_int64(st, 0);
        info->kind = (mn_playlist_kind)sqlite3_column_int(st, 1);
        const unsigned char *name = sqlite3_column_text(st, 2);
        info->name = mn_strdup(name ? (const char *)name : "");
        if (info->name == NULL) {
            status = MN_PLAYLIST_ERR_NOMEM;
            break;
        }
        info->created_at = sqlite3_column_int64(st, 3);
        info->updated_at = sqlite3_column_int64(st, 4);
        if (info->kind == MN_PLAYLIST_KIND_STATIC) {
            int64_t cnt = 0;
            (void)mn_member_count(db, info->id, &cnt);
            info->member_count = cnt;
        } else {
            info->member_count = -1;
        }
        n++;
    }
    sqlite3_finalize(st);

    if (status != MN_PLAYLIST_OK) {
        mn_playlist_info_free_array(arr, n);
        return status;
    }

    *out_arr = arr;
    *out_count = n;
    return MN_PLAYLIST_OK;
}

/* ------------------------------------------------------------------------- *
 * Static-playlist membership mutation
 *
 * All mutators run inside a SAVEPOINT so a partial failure rolls back and the
 * playlist is left unchanged, honoring the "atomic" contract.
 * ------------------------------------------------------------------------- */

static mn_playlist_status mn_savepoint(sqlite3 *db, const char *name) {
    char buf[64];
    snprintf(buf, sizeof(buf), "SAVEPOINT %s;", name);
    return mn_exec(db, buf);
}
static void mn_release(sqlite3 *db, const char *name) {
    char buf[64];
    snprintf(buf, sizeof(buf), "RELEASE %s;", name);
    (void)mn_exec(db, buf);
}
static void mn_rollback_to(sqlite3 *db, const char *name) {
    char buf[80];
    snprintf(buf, sizeof(buf), "ROLLBACK TO %s;", name);
    (void)mn_exec(db, buf);
    mn_release(db, name);
}

mn_playlist_status mn_playlist_add(sqlite3 *db, mn_playlist_id id,
                                   const mn_track_id *tracks, size_t count) {
    if (db == NULL || (tracks == NULL && count > 0)) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (count == 0) {
        return MN_PLAYLIST_OK;
    }
    int64_t base = 0;
    s = mn_member_count(db, id, &base);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    s = mn_savepoint(db, "mn_add");
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO playlist_members(playlist_id,position,track_id) "
        "VALUES(?,?,?);",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_add");
        return mn_from_sqlite(rc);
    }
    for (size_t i = 0; i < count; ++i) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, base + (int64_t)i);
        sqlite3_bind_int64(st, 3, tracks[i]);
        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            mn_rollback_to(db, "mn_add");
            return mn_from_sqlite(rc);
        }
    }
    sqlite3_finalize(st);
    mn_touch(db, id);
    mn_release(db, "mn_add");
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_insert(sqlite3 *db, mn_playlist_id id, int64_t at,
                                      const mn_track_id *tracks, size_t count) {
    if (db == NULL || (tracks == NULL && count > 0)) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    int64_t total = 0;
    s = mn_member_count(db, id, &total);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (at < 0 || at > total) {
        return MN_PLAYLIST_ERR_RANGE;
    }
    if (count == 0) {
        return MN_PLAYLIST_OK;
    }

    s = mn_savepoint(db, "mn_ins");
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    /* Shift existing members at >= at to the right by `count`. Update in
     * descending position order to avoid PK collisions. */
    sqlite3_stmt *sh = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE playlist_members SET position = position + ? "
        "WHERE playlist_id = ? AND position >= ?;",
        -1, &sh, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_ins");
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(sh, 1, (int64_t)count);
    sqlite3_bind_int64(sh, 2, id);
    sqlite3_bind_int64(sh, 3, at);
    rc = sqlite3_step(sh);
    sqlite3_finalize(sh);
    if (rc != SQLITE_DONE) {
        mn_rollback_to(db, "mn_ins");
        return mn_from_sqlite(rc);
    }

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO playlist_members(playlist_id,position,track_id) "
        "VALUES(?,?,?);",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_ins");
        return mn_from_sqlite(rc);
    }
    for (size_t i = 0; i < count; ++i) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, at + (int64_t)i);
        sqlite3_bind_int64(st, 3, tracks[i]);
        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            mn_rollback_to(db, "mn_ins");
            return mn_from_sqlite(rc);
        }
    }
    sqlite3_finalize(st);
    mn_touch(db, id);
    mn_release(db, "mn_ins");
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_remove_at(sqlite3 *db, mn_playlist_id id,
                                         const int64_t *positions, size_t count) {
    if (db == NULL || (positions == NULL && count > 0)) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (count == 0) {
        return MN_PLAYLIST_OK;
    }
    int64_t total = 0;
    s = mn_member_count(db, id, &total);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    /* Validate all positions first (all-or-nothing). */
    for (size_t i = 0; i < count; ++i) {
        if (positions[i] < 0 || positions[i] >= total) {
            return MN_PLAYLIST_ERR_RANGE;
        }
    }

    s = mn_savepoint(db, "mn_rm");
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    sqlite3_stmt *del = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "DELETE FROM playlist_members WHERE playlist_id=? AND position=?;",
        -1, &del, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_rm");
        return mn_from_sqlite(rc);
    }
    for (size_t i = 0; i < count; ++i) {
        sqlite3_reset(del);
        sqlite3_bind_int64(del, 1, id);
        sqlite3_bind_int64(del, 2, positions[i]);
        rc = sqlite3_step(del);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(del);
            mn_rollback_to(db, "mn_rm");
            return mn_from_sqlite(rc);
        }
    }
    sqlite3_finalize(del);

    /* Renumber remaining rows to stay contiguous. */
    sqlite3_stmt *rn = NULL;
    rc = sqlite3_prepare_v2(
        db,
        "UPDATE playlist_members SET position = ("
        "  SELECT COUNT(*) FROM playlist_members m2 "
        "  WHERE m2.playlist_id = playlist_members.playlist_id "
        "    AND m2.position < playlist_members.position"
        ") WHERE playlist_id = ?;",
        -1, &rn, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_rm");
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(rn, 1, id);
    rc = sqlite3_step(rn);
    sqlite3_finalize(rn);
    if (rc != SQLITE_DONE) {
        mn_rollback_to(db, "mn_rm");
        return mn_from_sqlite(rc);
    }

    mn_touch(db, id);
    mn_release(db, "mn_rm");
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_remove_track(sqlite3 *db, mn_playlist_id id,
                                            mn_track_id track) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    s = mn_savepoint(db, "mn_rmt");
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *del = NULL;
    int rc = sqlite3_prepare_v2(
        db, "DELETE FROM playlist_members WHERE playlist_id=? AND track_id=?;",
        -1, &del, NULL);
    if (rc != SQLITE_OK) {
        mn_rollback_to(db, "mn_rmt");
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(del, 1, id);
    sqlite3_bind_int64(del, 2, track);
    rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) {
        mn_rollback_to(db, "mn_rmt");
        return mn_from_sqlite(rc);
    }
    int changed = sqlite3_changes(db);

    if (changed > 0) {
        sqlite3_stmt *rn = NULL;
        rc = sqlite3_prepare_v2(
            db,
            "UPDATE playlist_members SET position = ("
            "  SELECT COUNT(*) FROM playlist_members m2 "
            "  WHERE m2.playlist_id = playlist_members.playlist_id "
            "    AND m2.position < playlist_members.position"
            ") WHERE playlist_id = ?;",
            -1, &rn, NULL);
        if (rc != SQLITE_OK) {
            mn_rollback_to(db, "mn_rmt");
            return mn_from_sqlite(rc);
        }
        sqlite3_bind_int64(rn, 1, id);
        rc = sqlite3_step(rn);
        sqlite3_finalize(rn);
        if (rc != SQLITE_DONE) {
            mn_rollback_to(db, "mn_rmt");
            return mn_from_sqlite(rc);
        }
        mn_touch(db, id);
    }
    mn_release(db, "mn_rmt");
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_move(sqlite3 *db, mn_playlist_id id,
                                    int64_t from, int64_t count, int64_t to) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (count <= 0) {
        return MN_PLAYLIST_OK;
    }
    int64_t total = 0;
    s = mn_member_count(db, id, &total);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (from < 0 || from + count > total || to < 0 || to > total) {
        return MN_PLAYLIST_ERR_RANGE;
    }
    /* A no-op move. */
    if (to >= from && to <= from + count) {
        return MN_PLAYLIST_OK;
    }

    /* Read the whole ordered list of track ids (positions 0..total-1). For a
     * 1M-track *library* this is fine: a single static playlist is bounded and
     * reorder is an explicit user gesture, not a hot query. */
    mn_track_id *ids = (mn_track_id *)malloc((size_t)total * sizeof(*ids));
    if (ids == NULL && total > 0) {
        return MN_PLAYLIST_ERR_NOMEM;
    }
    sqlite3_stmt *rd = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT track_id FROM playlist_members WHERE playlist_id=? "
        "ORDER BY position;",
        -1, &rd, NULL);
    if (rc != SQLITE_OK) {
        free(ids);
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(rd, 1, id);
    int64_t idx = 0;
    while ((rc = sqlite3_step(rd)) == SQLITE_ROW && idx < total) {
        ids[idx++] = sqlite3_column_int64(rd, 0);
    }
    sqlite3_finalize(rd);
    if (idx != total) {
        free(ids);
        return MN_PLAYLIST_ERR_SQL;
    }

    /* Build the reordered array in memory. Extract the run, then reinsert. */
    mn_track_id *out = (mn_track_id *)malloc((size_t)total * sizeof(*out));
    if (out == NULL && total > 0) {
        free(ids);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    /* Compute destination index in the post-removal numbering. */
    int64_t dst = to;
    if (to > from) {
        dst = to - count;   /* removal of the run shifts later indices left */
    }
    int64_t w = 0;
    /* Copy the non-run elements into a temporary, tracking where to splice. */
    mn_track_id *rest = (mn_track_id *)malloc((size_t)total * sizeof(*rest));
    if (rest == NULL && total > 0) {
        free(ids);
        free(out);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    int64_t rcount = 0;
    for (int64_t i = 0; i < total; ++i) {
        if (i >= from && i < from + count) {
            continue;
        }
        rest[rcount++] = ids[i];
    }
    /* Splice the run [from, from+count) at position dst in `rest`. */
    for (int64_t i = 0; i < dst; ++i) {
        out[w++] = rest[i];
    }
    for (int64_t i = 0; i < count; ++i) {
        out[w++] = ids[from + i];
    }
    for (int64_t i = dst; i < rcount; ++i) {
        out[w++] = rest[i];
    }
    free(rest);
    free(ids);

    /* Rewrite the membership table transactionally. */
    s = mn_savepoint(db, "mn_mv");
    if (s != MN_PLAYLIST_OK) {
        free(out);
        return s;
    }
    sqlite3_stmt *clr = NULL;
    rc = sqlite3_prepare_v2(
        db, "DELETE FROM playlist_members WHERE playlist_id=?;", -1, &clr, NULL);
    if (rc != SQLITE_OK) {
        free(out);
        mn_rollback_to(db, "mn_mv");
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(clr, 1, id);
    rc = sqlite3_step(clr);
    sqlite3_finalize(clr);
    if (rc != SQLITE_DONE) {
        free(out);
        mn_rollback_to(db, "mn_mv");
        return mn_from_sqlite(rc);
    }
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO playlist_members(playlist_id,position,track_id) "
        "VALUES(?,?,?);",
        -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        free(out);
        mn_rollback_to(db, "mn_mv");
        return mn_from_sqlite(rc);
    }
    for (int64_t i = 0; i < total; ++i) {
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, id);
        sqlite3_bind_int64(ins, 2, i);
        sqlite3_bind_int64(ins, 3, out[i]);
        rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(ins);
            free(out);
            mn_rollback_to(db, "mn_mv");
            return mn_from_sqlite(rc);
        }
    }
    sqlite3_finalize(ins);
    free(out);
    mn_touch(db, id);
    mn_release(db, "mn_mv");
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_clear(sqlite3 *db, mn_playlist_id id) {
    if (db == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "DELETE FROM playlist_members WHERE playlist_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return mn_from_sqlite(rc);
    }
    mn_touch(db, id);
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_count(sqlite3 *db, mn_playlist_id id,
                                     int64_t *out_count) {
    if (db == NULL || out_count == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    return mn_member_count(db, id, out_count);
}

/* ------------------------------------------------------------------------- *
 * Reading members / evaluating smart playlists
 * ------------------------------------------------------------------------- */

mn_playlist_status mn_playlist_get_members(sqlite3 *db, mn_playlist_id id,
                                           int64_t offset, int64_t limit,
                                           bool want_total,
                                           mn_track_id_list *out) {
    if (db == NULL || out == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_status s = mn_require_kind(db, id, MN_PLAYLIST_KIND_STATIC);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    if (offset < 0) {
        offset = 0;
    }

    out->ids = NULL;
    out->count = 0;
    out->total = -1;

    if (want_total) {
        int64_t cnt = 0;
        s = mn_member_count(db, id, &cnt);
        if (s != MN_PLAYLIST_OK) {
            return s;
        }
        out->total = cnt;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT track_id FROM playlist_members WHERE playlist_id=? "
        "ORDER BY position LIMIT ? OFFSET ?;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return mn_from_sqlite(rc);
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, (limit < 0) ? -1 : limit);
    sqlite3_bind_int64(st, 3, offset);

    size_t cap = 0, n = 0;
    mn_track_id *ids = NULL;
    mn_playlist_status status = MN_PLAYLIST_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = (cap != 0) ? cap * 2 : 64;
            mn_track_id *na = (mn_track_id *)realloc(ids, ncap * sizeof(*na));
            if (na == NULL) {
                status = MN_PLAYLIST_ERR_NOMEM;
                break;
            }
            ids = na;
            cap = ncap;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    if (status != MN_PLAYLIST_OK) {
        free(ids);
        return status;
    }
    out->ids = ids;
    out->count = n;
    return MN_PLAYLIST_OK;
}

/* Core rule evaluation shared by evaluate_smart and evaluate_rules. */
static mn_playlist_status mn_eval_rules(sqlite3 *db, const mn_pl_rules *rules,
                                        int64_t offset, int64_t limit,
                                        bool want_total, mn_track_id_list *out) {
    out->ids = NULL;
    out->count = 0;
    out->total = -1;

    /* Total (bounded by rule limit) when requested. */
    if (want_total) {
        mn_compiler cc;
        char *csql = NULL;
        mn_playlist_status s = mn_compile_select(rules, true, 0, -1, &cc, &csql);
        if (s != MN_PLAYLIST_OK) {
            mn_compiler_free(&cc);
            free(csql);
            return s;
        }
        sqlite3_stmt *cst = NULL;
        int rc = sqlite3_prepare_v2(db, csql, -1, &cst, NULL);
        free(csql);
        if (rc != SQLITE_OK) {
            mn_compiler_free(&cc);
            return mn_from_sqlite(rc);
        }
        s = mn_bind_all(cst, &cc);
        if (s != MN_PLAYLIST_OK) {
            sqlite3_finalize(cst);
            mn_compiler_free(&cc);
            return s;
        }
        if (sqlite3_step(cst) == SQLITE_ROW) {
            out->total = sqlite3_column_int64(cst, 0);
        }
        sqlite3_finalize(cst);
        mn_compiler_free(&cc);
    }

    mn_compiler c;
    char *sql = NULL;
    mn_playlist_status s = mn_compile_select(rules, false, offset, limit, &c,
                                             &sql);
    if (s != MN_PLAYLIST_OK) {
        mn_compiler_free(&c);
        free(sql);
        return s;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    free(sql);
    if (rc != SQLITE_OK) {
        mn_compiler_free(&c);
        return mn_from_sqlite(rc);
    }
    s = mn_bind_all(st, &c);
    if (s != MN_PLAYLIST_OK) {
        sqlite3_finalize(st);
        mn_compiler_free(&c);
        return s;
    }

    size_t cap = 0, n = 0;
    mn_track_id *ids = NULL;
    mn_playlist_status status = MN_PLAYLIST_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = (cap != 0) ? cap * 2 : 64;
            mn_track_id *na = (mn_track_id *)realloc(ids, ncap * sizeof(*na));
            if (na == NULL) {
                status = MN_PLAYLIST_ERR_NOMEM;
                break;
            }
            ids = na;
            cap = ncap;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW && status == MN_PLAYLIST_OK) {
        status = mn_from_sqlite(rc);
    }
    sqlite3_finalize(st);
    mn_compiler_free(&c);

    if (status != MN_PLAYLIST_OK) {
        free(ids);
        return status;
    }
    out->ids = ids;
    out->count = n;
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_evaluate_smart(sqlite3 *db, mn_playlist_id id,
                                              int64_t offset, int64_t limit,
                                              bool want_total,
                                              mn_track_id_list *out) {
    if (db == NULL || out == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_pl_rules *rules = NULL;
    mn_playlist_status s = mn_playlist_get_smart_rules(db, id, &rules);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    s = mn_eval_rules(db, rules, offset, limit, want_total, out);
    mn_pl_rules_free(rules);
    return s;
}

mn_playlist_status mn_playlist_evaluate_rules(sqlite3 *db,
                                              const mn_pl_rules *rules,
                                              int64_t offset, int64_t limit,
                                              bool want_total,
                                              mn_track_id_list *out) {
    if (db == NULL || out == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    return mn_eval_rules(db, rules, offset, limit, want_total, out);
}

mn_playlist_status mn_playlist_compile_rules_sql(const mn_pl_rules *rules,
                                                 char **out_sql) {
    if (out_sql == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_compiler c;
    char *sql = NULL;
    mn_playlist_status s = mn_compile_select(rules, false, 0, -1, &c, &sql);
    mn_compiler_free(&c);
    if (s != MN_PLAYLIST_OK) {
        free(sql);
        return s;
    }
    *out_sql = sql;
    return MN_PLAYLIST_OK;
}

/* ------------------------------------------------------------------------- *
 * Path helpers for import/export
 * ------------------------------------------------------------------------- */

/* Return a pointer to the basename within `path` (after the last / or \\). */
static const char *mn_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* Directory portion of a path into `out` (without trailing separator). If the
 * path has no directory component, `out` is set to "". */
static void mn_dirname(const char *path, char *out, size_t out_sz) {
    const char *base = mn_basename(path);
    size_t n = (size_t)(base - path);
    if (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        n--;   /* drop the trailing separator */
    }
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    if (out_sz > 0) {
        memcpy(out, path, n);
        out[n] = '\0';
    }
}

static bool mn_path_is_absolute(const char *p) {
    if (p == NULL || p[0] == '\0') {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    /* Windows drive letter, e.g. C:\ or C:/ */
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
        return true;
    }
    /* UNC path. */
    if (p[0] == '\\' && p[1] == '\\') {
        return true;
    }
    return false;
}

static bool mn_has_ext_ci(const char *path, const char *ext) {
    size_t pl = strlen(path);
    size_t el = strlen(ext);
    if (pl < el) {
        return false;
    }
    const char *tail = path + (pl - el);
    for (size_t i = 0; i < el; ++i) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

/* Join base_dir + entry into a heap absolute path. If entry is already
 * absolute, it is returned as-is (copied). Caller frees. */
static char *mn_join_path(const char *base_dir, const char *entry) {
    if (mn_path_is_absolute(entry) || base_dir == NULL || base_dir[0] == '\0') {
        return mn_strdup(entry);
    }
    size_t bl = strlen(base_dir);
    size_t el = strlen(entry);
    bool need_sep = (bl > 0 && base_dir[bl - 1] != '/' && base_dir[bl - 1] != '\\');
    char *out = (char *)malloc(bl + (need_sep ? 1 : 0) + el + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, base_dir, bl);
    size_t w = bl;
    if (need_sep) {
        out[w++] = '/';
    }
    memcpy(out + w, entry, el);
    w += el;
    out[w] = '\0';
    return out;
}

/* Format detection from a file extension. */
static mn_playlist_format mn_infer_format(const char *path) {
    if (path == NULL) {
        return MN_PLAYLIST_FMT_M3U8;
    }
    if (mn_has_ext_ci(path, ".m3u8")) return MN_PLAYLIST_FMT_M3U8;
    if (mn_has_ext_ci(path, ".m3u"))  return MN_PLAYLIST_FMT_M3U;
    if (mn_has_ext_ci(path, ".pls"))  return MN_PLAYLIST_FMT_PLS;
    return MN_PLAYLIST_FMT_M3U8;
}

/* Infer format from content when no extension is available. */
static mn_playlist_format mn_infer_format_content(const char *data, size_t len) {
    /* PLS files start with "[playlist]" (possibly after BOM/whitespace). */
    size_t i = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        i = 3;
    }
    while (i < len && (data[i] == ' ' || data[i] == '\t' ||
                       data[i] == '\r' || data[i] == '\n')) {
        i++;
    }
    if (len - i >= 10 && strncmp(data + i, "[playlist]", 10) == 0) {
        return MN_PLAYLIST_FMT_PLS;
    }
    return MN_PLAYLIST_FMT_M3U8;
}

/* ------------------------------------------------------------------------- *
 * Import
 * ------------------------------------------------------------------------- */

/* Resolve a filesystem path to a track id via the tracks table. Matches on the
 * exact stored path. Returns MN_TRACK_ID_NONE if not found. */
static mn_track_id mn_resolve_path(sqlite3 *db, const char *path) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM tracks WHERE path=? LIMIT 1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return MN_TRACK_ID_NONE;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    mn_track_id id = MN_TRACK_ID_NONE;
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

/* Trim leading/trailing ASCII whitespace in place; returns pointer to start. */
static char *mn_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* Parse the body of a playlist and collect ordered entry file references into
 * a heap array of heap strings. Caller frees each entry and the array. */
static mn_playlist_status mn_parse_entries(const char *data, size_t len,
                                           mn_playlist_format fmt,
                                           char ***out_entries,
                                           size_t *out_count) {
    /* Work on a mutable NUL-terminated copy. */
    char *buf = mn_strndup(data, len);
    if (buf == NULL) {
        return MN_PLAYLIST_ERR_NOMEM;
    }

    char **entries = NULL;
    size_t cap = 0, n = 0;
    mn_playlist_status status = MN_PLAYLIST_OK;

#define MN_PUSH_ENTRY(str)                                                    \
    do {                                                                      \
        if (n == cap) {                                                       \
            size_t ncap = (cap != 0) ? cap * 2 : 16;                          \
            char **ne = (char **)realloc(entries, ncap * sizeof(*ne));        \
            if (ne == NULL) { status = MN_PLAYLIST_ERR_NOMEM; goto done; }    \
            entries = ne; cap = ncap;                                         \
        }                                                                     \
        entries[n] = mn_strdup(str);                                          \
        if (entries[n] == NULL) { status = MN_PLAYLIST_ERR_NOMEM; goto done; }\
        n++;                                                                  \
    } while (0)

    /* Iterate lines. */
    char *save = buf;
    char *line;
    while ((line = save) != NULL && *save != '\0') {
        char *nl = strchr(save, '\n');
        if (nl != NULL) {
            *nl = '\0';
            save = nl + 1;
        } else {
            save = NULL;
        }
        char *t = mn_trim(line);
        if (*t == '\0') {
            if (save == NULL) break; else continue;
        }
        if (fmt == MN_PLAYLIST_FMT_PLS) {
            /* Lines of form FileN=path (case-insensitive key). */
            if ((t[0] == 'F' || t[0] == 'f') &&
                (t[1] == 'I' || t[1] == 'i') &&
                (t[2] == 'L' || t[2] == 'l') &&
                (t[3] == 'E' || t[3] == 'e')) {
                char *eq = strchr(t, '=');
                if (eq != NULL) {
                    char *val = mn_trim(eq + 1);
                    if (*val != '\0') {
                        MN_PUSH_ENTRY(val);
                    }
                }
            }
            /* Ignore [playlist], NumberOfEntries, TitleN, LengthN, Version. */
        } else {
            /* M3U / M3U8: skip comment/directive lines starting with '#'. */
            if (t[0] == '#') {
                if (save == NULL) break; else continue;
            }
            MN_PUSH_ENTRY(t);
        }
        if (save == NULL) {
            break;
        }
    }

#undef MN_PUSH_ENTRY

done:
    free(buf);
    if (status != MN_PLAYLIST_OK) {
        for (size_t i = 0; i < n; ++i) {
            free(entries[i]);
        }
        free(entries);
        return status;
    }
    *out_entries = entries;
    *out_count = n;
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_import_buffer(sqlite3 *db, const char *data,
                                             size_t len, const char *name,
                                             const mn_playlist_import_opts *opts,
                                             mn_playlist_import_result *out_result) {
    if (db == NULL || data == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    mn_playlist_format fmt =
        (opts != NULL) ? opts->format : MN_PLAYLIST_FMT_AUTO;
    if (fmt == MN_PLAYLIST_FMT_AUTO) {
        fmt = mn_infer_format_content(data, len);
    }
    const char *base_dir = (opts != NULL) ? opts->base_dir : NULL;

    char **entries = NULL;
    size_t ecount = 0;
    mn_playlist_status s = mn_parse_entries(data, len, fmt, &entries, &ecount);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    const char *pl_name = (name != NULL && name[0] != '\0') ? name : "Imported";
    mn_playlist_id pid = MN_PLAYLIST_ID_NONE;
    s = mn_playlist_create_static(db, pl_name, &pid);
    if (s != MN_PLAYLIST_OK) {
        goto cleanup;
    }

    int64_t total = 0, resolved = 0, unresolved = 0;

    s = mn_savepoint(db, "mn_imp");
    if (s != MN_PLAYLIST_OK) {
        goto cleanup;
    }
    {
        sqlite3_stmt *ins = NULL;
        int rc = sqlite3_prepare_v2(
            db,
            "INSERT INTO playlist_members(playlist_id,position,track_id) "
            "VALUES(?,?,?);",
            -1, &ins, NULL);
        if (rc != SQLITE_OK) {
            mn_rollback_to(db, "mn_imp");
            s = mn_from_sqlite(rc);
            goto cleanup;
        }
        int64_t pos = 0;
        for (size_t i = 0; i < ecount; ++i) {
            total++;
            char *abs = mn_join_path(base_dir, entries[i]);
            mn_track_id tid = MN_TRACK_ID_NONE;
            if (abs != NULL) {
                tid = mn_resolve_path(db, abs);
                if (tid == MN_TRACK_ID_NONE) {
                    /* Try the raw entry too (already-absolute or DB stores
                     * platform-native separators). */
                    tid = mn_resolve_path(db, entries[i]);
                }
                free(abs);
            }
            if (tid == MN_TRACK_ID_NONE) {
                unresolved++;
                continue;   /* skip unresolved; not fatal */
            }
            resolved++;
            sqlite3_reset(ins);
            sqlite3_bind_int64(ins, 1, pid);
            sqlite3_bind_int64(ins, 2, pos++);
            sqlite3_bind_int64(ins, 3, tid);
            rc = sqlite3_step(ins);
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(ins);
                mn_rollback_to(db, "mn_imp");
                s = mn_from_sqlite(rc);
                goto cleanup;
            }
        }
        sqlite3_finalize(ins);
    }
    mn_touch(db, pid);
    mn_release(db, "mn_imp");

    if (out_result != NULL) {
        out_result->id = pid;
        out_result->total = total;
        out_result->resolved = resolved;
        out_result->unresolved = unresolved;
    }
    s = MN_PLAYLIST_OK;

cleanup:
    if (entries != NULL) {
        for (size_t i = 0; i < ecount; ++i) {
            free(entries[i]);
        }
        free(entries);
    }
    /* If we created a playlist but failed after, best-effort delete it. */
    if (s != MN_PLAYLIST_OK && pid != MN_PLAYLIST_ID_NONE) {
        (void)mn_playlist_delete(db, pid);
    }
    return s;
}

/* Read an entire file into a heap buffer. Uses a wide path on Windows so
 * UTF-8 input paths address non-ASCII filenames correctly. */
static mn_playlist_status mn_read_file(const char *path, char **out_data,
                                       size_t *out_len) {
    FILE *f = NULL;
#ifdef _WIN32
    /* Convert UTF-8 -> UTF-16 and open with _wfopen. */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wpath == NULL) {
            return MN_PLAYLIST_ERR_NOMEM;
        }
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
        f = _wfopen(wpath, L"rb");
        free(wpath);
    }
#else
    f = fopen(path, "rb");
#endif
    if (f == NULL) {
        return MN_PLAYLIST_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return MN_PLAYLIST_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return MN_PLAYLIST_ERR_IO;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_data = buf;
    *out_len = got;
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_import_file(sqlite3 *db, const char *path,
                                           const char *name,
                                           const mn_playlist_import_opts *opts,
                                           mn_playlist_import_result *out_result) {
    if (db == NULL || path == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    char *data = NULL;
    size_t len = 0;
    mn_playlist_status s = mn_read_file(path, &data, &len);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    /* Resolve options, defaulting base_dir to the source file's directory and
     * format to inference from the file extension. */
    mn_playlist_import_opts local;
    if (opts != NULL) {
        local = *opts;
    } else {
        memset(&local, 0, sizeof(local));
        local.format = MN_PLAYLIST_FMT_AUTO;
    }
    char dirbuf[4096];
    if (local.base_dir == NULL) {
        mn_dirname(path, dirbuf, sizeof(dirbuf));
        local.base_dir = dirbuf;
    }
    if (local.format == MN_PLAYLIST_FMT_AUTO) {
        local.format = mn_infer_format(path);
    }

    /* Derive a name from the filename if not given. */
    char *derived = NULL;
    const char *use_name = name;
    if (use_name == NULL || use_name[0] == '\0') {
        const char *base = mn_basename(path);
        derived = mn_strdup(base);
        if (derived != NULL) {
            /* Strip a trailing .m3u/.m3u8/.pls extension. */
            char *dot = strrchr(derived, '.');
            if (dot != NULL) {
                *dot = '\0';
            }
            use_name = derived;
        }
    }

    s = mn_playlist_import_buffer(db, data, len, use_name, &local, out_result);
    free(derived);
    free(data);
    return s;
}

/* ------------------------------------------------------------------------- *
 * Export
 * ------------------------------------------------------------------------- */

/* Compute a path relative to `base_dir` if requested and possible; otherwise
 * returns a copy of `abs`. Simplified: emits a relative path only when `abs`
 * begins with base_dir + separator, stripping that prefix. Caller frees. */
static char *mn_relativize(const char *abs, const char *base_dir,
                           bool relative) {
    if (!relative || base_dir == NULL || base_dir[0] == '\0') {
        return mn_strdup(abs);
    }
    size_t bl = strlen(base_dir);
    if (bl > 0 && (base_dir[bl - 1] == '/' || base_dir[bl - 1] == '\\')) {
        bl--;
    }
    if (strncmp(abs, base_dir, bl) == 0 &&
        (abs[bl] == '/' || abs[bl] == '\\')) {
        return mn_strdup(abs + bl + 1);
    }
    return mn_strdup(abs);
}

/* Fetch (path,title,artist,duration_ms) for a track. Strings into heap via out
 * params (caller frees). Returns false if the track is missing. */
static bool mn_fetch_track_export(sqlite3 *db, mn_track_id id, char **out_path,
                                  char **out_title, char **out_artist,
                                  int64_t *out_dur_ms) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT path,title,artist,duration_ms FROM tracks WHERE id=?;",
            -1, &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(st, 0);
        const unsigned char *ti = sqlite3_column_text(st, 1);
        const unsigned char *ar = sqlite3_column_text(st, 2);
        *out_path = mn_strdup(p ? (const char *)p : "");
        *out_title = mn_strdup(ti ? (const char *)ti : "");
        *out_artist = mn_strdup(ar ? (const char *)ar : "");
        *out_dur_ms = sqlite3_column_int64(st, 3);
        ok = (*out_path != NULL && *out_title != NULL && *out_artist != NULL);
        if (!ok) {
            free(*out_path); free(*out_title); free(*out_artist);
            *out_path = *out_title = *out_artist = NULL;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

/* Build the export text for the given ordered track ids into `b`. */
static mn_playlist_status mn_build_export(sqlite3 *db, const mn_track_id *ids,
                                          size_t count, mn_playlist_format fmt,
                                          const char *base_dir, bool relative,
                                          bool extended, mn_buf *b) {
    if (fmt == MN_PLAYLIST_FMT_PLS) {
        mn_buf_puts(b, "[playlist]\r\n");
        size_t written = 0;
        for (size_t i = 0; i < count; ++i) {
            char *path = NULL, *title = NULL, *artist = NULL;
            int64_t dur = 0;
            if (!mn_fetch_track_export(db, ids[i], &path, &title, &artist,
                                       &dur)) {
                continue;
            }
            char *rel = mn_relativize(path, base_dir, relative);
            written++;
            mn_buf_printf(b, "File%zu=%s\r\n", written,
                          rel ? rel : path);
            /* Title line. */
            if (title[0] != '\0' || artist[0] != '\0') {
                if (artist[0] != '\0' && title[0] != '\0') {
                    mn_buf_printf(b, "Title%zu=%s - %s\r\n", written, artist,
                                  title);
                } else {
                    mn_buf_printf(b, "Title%zu=%s\r\n", written,
                                  title[0] ? title : artist);
                }
            }
            mn_buf_printf(b, "Length%zu=%lld\r\n", written,
                          (long long)(dur > 0 ? dur / 1000 : -1));
            free(rel); free(path); free(title); free(artist);
        }
        mn_buf_printf(b, "NumberOfEntries=%zu\r\n", written);
        mn_buf_puts(b, "Version=2\r\n");
    } else {
        /* M3U / M3U8 */
        if (extended) {
            mn_buf_puts(b, "#EXTM3U\r\n");
        }
        for (size_t i = 0; i < count; ++i) {
            char *path = NULL, *title = NULL, *artist = NULL;
            int64_t dur = 0;
            if (!mn_fetch_track_export(db, ids[i], &path, &title, &artist,
                                       &dur)) {
                continue;
            }
            char *rel = mn_relativize(path, base_dir, relative);
            if (extended) {
                int64_t secs = (dur > 0) ? (dur + 500) / 1000 : -1;
                if (artist[0] != '\0' && title[0] != '\0') {
                    mn_buf_printf(b, "#EXTINF:%lld,%s - %s\r\n",
                                  (long long)secs, artist, title);
                } else {
                    mn_buf_printf(b, "#EXTINF:%lld,%s\r\n", (long long)secs,
                                  title[0] ? title : mn_basename(path));
                }
            }
            mn_buf_printf(b, "%s\r\n", rel ? rel : path);
            free(rel); free(path); free(title); free(artist);
        }
    }
    if (b->oom) {
        return MN_PLAYLIST_ERR_NOMEM;
    }
    return MN_PLAYLIST_OK;
}

/* Gather the ordered track ids for a playlist for export. */
static mn_playlist_status mn_gather_export_ids(sqlite3 *db, mn_playlist_id id,
                                               const mn_playlist_export_opts *opts,
                                               mn_track_id **out_ids,
                                               size_t *out_count) {
    mn_playlist_kind kind;
    mn_playlist_status s = mn_lookup_kind(db, id, &kind);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    mn_track_id_list list;
    memset(&list, 0, sizeof(list));

    if (kind == MN_PLAYLIST_KIND_STATIC) {
        s = mn_playlist_get_members(db, id, 0, -1, false, &list);
    } else {
        if (opts == NULL || !opts->evaluate_smart) {
            return MN_PLAYLIST_ERR_WRONG_KIND;
        }
        s = mn_playlist_evaluate_smart(db, id, 0, -1, false, &list);
    }
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    *out_ids = list.ids;    /* transfer ownership */
    *out_count = list.count;
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_export_buffer(sqlite3 *db, mn_playlist_id id,
                                             const char *path_hint,
                                             const mn_playlist_export_opts *opts,
                                             char **out_data, size_t *out_len) {
    if (db == NULL || out_data == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }

    mn_playlist_format fmt =
        (opts != NULL) ? opts->format : MN_PLAYLIST_FMT_AUTO;
    if (fmt == MN_PLAYLIST_FMT_AUTO) {
        fmt = mn_infer_format(path_hint);
    }
    bool relative = (opts != NULL) ? opts->relative_paths : false;
    bool extended = (opts != NULL) ? opts->extended : true;

    char dirbuf[4096];
    dirbuf[0] = '\0';
    if (path_hint != NULL) {
        mn_dirname(path_hint, dirbuf, sizeof(dirbuf));
    }

    mn_track_id *ids = NULL;
    size_t count = 0;
    mn_playlist_status s = mn_gather_export_ids(db, id, opts, &ids, &count);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }

    mn_buf b;
    mn_buf_init(&b);
    s = mn_build_export(db, ids, count, fmt, dirbuf, relative, extended, &b);
    free(ids);
    if (s != MN_PLAYLIST_OK) {
        mn_buf_free(&b);
        return s;
    }
    if (mn_buf_cstr(&b) == NULL) {
        mn_buf_free(&b);
        return MN_PLAYLIST_ERR_NOMEM;
    }
    *out_data = b.data;
    if (out_len != NULL) {
        *out_len = b.len;
    }
    return MN_PLAYLIST_OK;
}

/* Write a buffer to a file, honoring wide paths on Windows. */
static mn_playlist_status mn_write_file(const char *path, const char *data,
                                        size_t len) {
    FILE *f = NULL;
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wpath == NULL) {
            return MN_PLAYLIST_ERR_NOMEM;
        }
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
        f = _wfopen(wpath, L"wb");
        free(wpath);
    }
#else
    f = fopen(path, "wb");
#endif
    if (f == NULL) {
        return MN_PLAYLIST_ERR_IO;
    }
    size_t wrote = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int ferr = ferror(f);
    if (fclose(f) != 0 || ferr != 0 || wrote != len) {
        return MN_PLAYLIST_ERR_IO;
    }
    return MN_PLAYLIST_OK;
}

mn_playlist_status mn_playlist_export_file(sqlite3 *db, mn_playlist_id id,
                                           const char *path,
                                           const mn_playlist_export_opts *opts) {
    if (db == NULL || path == NULL) {
        return MN_PLAYLIST_ERR_INVALID_ARG;
    }
    char *data = NULL;
    size_t len = 0;
    mn_playlist_status s = mn_playlist_export_buffer(db, id, path, opts, &data,
                                                     &len);
    if (s != MN_PLAYLIST_OK) {
        return s;
    }
    s = mn_write_file(path, data, len);
    free(data);
    return s;
}
