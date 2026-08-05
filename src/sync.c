/*
 * sync.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * Desktop side of the NEX-GEN library-sync protocol (see sync.h; the
 * companion Android app ships SYNC_PROTOCOL.md). Everything here is blocking
 * worker-thread code, UI-free by contract:
 *
 *   identity (§1)  — hand-rolled byte scanners, no regex: trim, lowercase
 *                    ASCII, collapse [\s._-]+ runs to one space, strip
 *                    ( .. ) / [ .. ] groups non-greedily, join with 0x01
 *                    separators + a 10-second duration bucket.
 *   snapshot (§2)  — walks the library (non-default metrics only) into a
 *                    malloc'd JSON string via a local strbuf.
 *   merge (§3)     — tolerant hand-rolled snapshot parse; identity -> local
 *                    track resolved through an FNV-1a open-addressing map
 *                    built from ONE bulk enumerate; max-merge play stats +
 *                    last-write-wins preference group; changed rows written
 *                    through mn_library_sync_apply in ONE transaction.
 *   HTTP (§4a)     — WinHTTP, plain HTTP (the phone's LAN server), ~5 s
 *                    connect / ~10 s receive timeouts, 32 MB body cap.
 *   files (§4b)    — snapshot export/import in the shared JSON format.
 *
 * Library access goes through the caller-supplied mn_sync_env lock hooks;
 * locks are never held across network I/O.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "sync.h"
#include "library_db.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winhttp.h>
#  include <direct.h>   /* _mkdir */
#  define sy_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define sy_mkdir(p) mkdir((p), 0755)
#endif

/* Response body cap for the phone's HTTP replies (a 1M-track snapshot is
 * far below this; the cap only bounds a misbehaving peer). */
#define SY_HTTP_CAP (32 * 1024 * 1024)

/* Longest identity/tag scratch we normalize into (tags beyond this are
 * truncated identically on every pass, so matching stays stable). */
#define SY_ID_MAX 4096

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *sy_strdup(const char *s) {
    size_t n;
    char  *d;
    if (!s) s = "";
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Wall-clock epoch milliseconds (the protocol's timestamp unit). */
static int64_t sy_now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

static void sy_env_lock(const mn_sync_env *env) {
    if (env->lock) env->lock(env->lock_user);
}

static void sy_env_unlock(const mn_sync_env *env) {
    if (env->unlock) env->unlock(env->lock_user);
}

/* FNV-1a 64 over a NUL-terminated string (same constants as app.c's
 * mn_scan_hash). */
static uint64_t sy_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Local strbuf (the cef_host idiom, replicated so sync.c stays        */
/* dependency-free)                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} sy_strbuf;

static void sy_sb_init(sy_strbuf *b) {
    b->cap = 4096;
    b->len = 0;
    b->oom = false;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = 0; else b->oom = true;
}

static void sy_sb_free(sy_strbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool sy_sb_reserve(sy_strbuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    {
        size_t ncap = b->cap ? b->cap : 4096;
        char  *nd;
        while (b->len + extra + 1 > ncap) ncap *= 2;
        nd = (char *)realloc(b->data, ncap);
        if (!nd) { b->oom = true; return false; }
        b->data = nd;
        b->cap  = ncap;
    }
    return true;
}

static void sy_sb_putn(sy_strbuf *b, const char *s, size_t n) {
    if (!sy_sb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sy_sb_puts(sy_strbuf *b, const char *s) {
    sy_sb_putn(b, s, strlen(s));
}

static void sy_sb_putc(sy_strbuf *b, char c) {
    sy_sb_putn(b, &c, 1);
}

/* JSON string literal with control chars escaped — identities carry raw
 * 0x01 separators, which MUST round-trip as \u0001. */
static void sy_sb_json_str(sy_strbuf *b, const char *s) {
    sy_sb_putc(b, '"');
    if (s) {
        const unsigned char *p;
        for (p = (const unsigned char *)s; *p; ++p) {
            unsigned char c = *p;
            switch (c) {
                case '"':  sy_sb_putn(b, "\\\"", 2); break;
                case '\\': sy_sb_putn(b, "\\\\", 2); break;
                case '\b': sy_sb_putn(b, "\\b", 2);  break;
                case '\f': sy_sb_putn(b, "\\f", 2);  break;
                case '\n': sy_sb_putn(b, "\\n", 2);  break;
                case '\r': sy_sb_putn(b, "\\r", 2);  break;
                case '\t': sy_sb_putn(b, "\\t", 2);  break;
                default:
                    if (c < 0x20) {
                        char u[8];
                        snprintf(u, sizeof(u), "\\u%04x", c);
                        sy_sb_putn(b, u, 6);
                    } else {
                        sy_sb_putc(b, (char)c);
                    }
            }
        }
    }
    sy_sb_putc(b, '"');
}

static void sy_sb_json_i64(sy_strbuf *b, int64_t v) {
    char t[32];
    snprintf(t, sizeof(t), "%lld", (long long)v);
    sy_sb_puts(b, t);
}

static void sy_sb_json_bool(sy_strbuf *b, bool v) {
    sy_sb_puts(b, v ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* Tolerant JSON extractors (adapted from cef_host's json_get_*)       */
/* ------------------------------------------------------------------ */

static const char *sy_json_find_value(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        if (p > json && p[-1] == '"' && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                return q;
            }
        }
        p += klen;
    }
    return NULL;
}

static int64_t sy_json_get_i64(const char *json, const char *key, int64_t dflt) {
    const char *v = sy_json_find_value(json, key);
    char *end = NULL;
    long long r;
    if (!v) return dflt;
    if (*v == '"') v++;
    r = strtoll(v, &end, 10);
    if (end == v) return dflt;
    return (int64_t)r;
}

static bool sy_json_get_bool(const char *json, const char *key, bool dflt) {
    const char *v = sy_json_find_value(json, key);
    if (!v) return dflt;
    if (*v == '"') v++;
    if (strncmp(v, "true", 4) == 0)  return true;
    if (strncmp(v, "false", 5) == 0) return false;
    if (*v == '1') return true;
    if (*v == '0') return false;
    return dflt;
}

/* Bounded string extraction with escape decoding (incl. \u0001 -> 0x01,
 * which every identity in a snapshot carries). */
static bool sy_json_get_str(const char *json, const char *key,
                            char *out, size_t out_n) {
    const char *v = sy_json_find_value(json, key);
    size_t o = 0;
    if (!v || *v != '"' || out_n == 0) { if (out_n) out[0] = 0; return false; }
    v++;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char e = *v++;
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case '/':  c = '/';  break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    unsigned cp = 0;
                    int ok = 1, i;
                    for (i = 0; i < 4; i++) {
                        char h = *v;
                        if      (h >= '0' && h <= '9') cp = (cp << 4) + (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp = (cp << 4) + (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp = (cp << 4) + (unsigned)(h - 'A' + 10);
                        else { ok = 0; break; }
                        v++;
                    }
                    if (!ok) continue;
                    if (cp < 0x80) {
                        if (o + 1 < out_n) out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o + 2 < out_n) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        if (o + 3 < out_n) {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;
                }
                default:   c = e; break;
            }
        }
        if (o + 1 < out_n) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Identity (§1)                                                       */
/* ------------------------------------------------------------------ */

/*
 * norm(s): the §1 pipeline as byte scanners over UTF-8. Only ASCII A-Z is
 * lowercased (non-ASCII passes through untouched); "trim" strips anything
 * <= U+0020 at the ends (Java String.trim semantics); the collapse set is
 * exactly the regex class [\s._-] with Java \s = [ \t\n\x0B\f\r]; group
 * stripping mirrors the non-greedy "\(.*?\)|\[.*?\]" replaceAll: on '('
 * skip to the NEXT ')' (same for '['), an opener with no closer stays
 * literal. Always NUL-terminates `out`.
 */
static void sy_norm(const char *s, char *out, size_t out_n) {
    size_t n, i, o = 0, w;

    if (!out || out_n == 0) return;
    out[0] = 0;
    if (!s) return;

    /* trim */
    while (*s && (unsigned char)*s <= 0x20) s++;
    n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= 0x20) n--;

    /* lowercase ASCII + collapse [\s._-]+ runs to one space */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' ||
            c == '\r' || c == '.' || c == '_' || c == '-') {
            if ((o == 0 || out[o - 1] != ' ') && o + 1 < out_n) out[o++] = ' ';
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (o + 1 < out_n) out[o++] = (char)c;
    }

    /* strip ( .. ) and [ .. ] groups, non-greedily, in place */
    w = 0;
    for (i = 0; i < o; i++) {
        char c = out[i];
        if (c == '(' || c == '[') {
            const char *e = (const char *)memchr(out + i + 1,
                                                 (c == '(') ? ')' : ']',
                                                 o - i - 1);
            if (e) { i = (size_t)(e - out); continue; }
        }
        out[w++] = c;
    }
    o = w;

    /* trim ends again (the collapse/strip can leave spaces) */
    i = 0;
    while (i < o && (unsigned char)out[i] <= 0x20) i++;
    while (o > i && (unsigned char)out[o - 1] <= 0x20) o--;
    if (i > 0) memmove(out, out + i, o - i);
    out[o - i] = 0;
}

void mn_sync_identity(const char *artist, const char *title,
                      const char *album, int64_t duration_ms,
                      char *out, size_t out_n) {
    char na[SY_ID_MAX / 4], nt[SY_ID_MAX / 4], nal[SY_ID_MAX / 4];
    long long bucket;

    if (!out || out_n == 0) return;
    sy_norm(artist, na,  sizeof(na));
    sy_norm(title,  nt,  sizeof(nt));
    sy_norm(album,  nal, sizeof(nal));
    bucket = (duration_ms > 0) ? (long long)(duration_ms / 10000) : 0;
    snprintf(out, out_n, "%s\x01%s\x01%s\x01%lld", na, nt, nal, bucket);
}

/* ------------------------------------------------------------------ */
/* Local-track collection (one bulk enumerate) + identity map          */
/* ------------------------------------------------------------------ */

typedef struct sy_local {
    int64_t id;
    char   *identity;        /* malloc'd */
    int32_t liked;           /* thumbs: -1 dislike, 0 none, 1 like       */
    int32_t rating_x2;
    int64_t play_count;
    int64_t last_played_s;   /* stored unit: unix SECONDS                */
    int64_t pref_updated_ms;
    /* snapshot-only originals (NULL when collected for a merge map) */
    char   *artist;
    char   *title;
    char   *album;
} sy_local;

typedef struct sy_collect {
    sy_local *v;
    size_t    n, cap;
    bool      want_tags;
    bool      oom;
} sy_collect;

static void sy_collect_cb(void *user, int64_t track_id,
                          const char *artist, const char *title,
                          const char *album, int64_t duration_ms,
                          int32_t liked, int32_t rating_x2,
                          int64_t play_count, int64_t last_played,
                          int64_t pref_updated_ms) {
    sy_collect *c = (sy_collect *)user;
    char idbuf[SY_ID_MAX];
    sy_local *t;

    if (c->oom) return;
    if (c->n == c->cap) {
        size_t    ncap = c->cap ? c->cap * 2 : 1024;
        sy_local *nv = (sy_local *)realloc(c->v, ncap * sizeof(*nv));
        if (!nv) { c->oom = true; return; }
        c->v = nv;
        c->cap = ncap;
    }
    mn_sync_identity(artist, title, album, duration_ms, idbuf, sizeof(idbuf));
    t = &c->v[c->n];
    memset(t, 0, sizeof(*t));
    t->id              = track_id;
    t->identity        = sy_strdup(idbuf);
    t->liked           = liked;
    t->rating_x2       = rating_x2;
    t->play_count      = play_count;
    t->last_played_s   = last_played;
    t->pref_updated_ms = pref_updated_ms;
    if (!t->identity) { c->oom = true; return; }
    if (c->want_tags) {
        t->artist = sy_strdup(artist);
        t->title  = sy_strdup(title);
        t->album  = sy_strdup(album);
        if (!t->artist || !t->title || !t->album) {
            free(t->artist); free(t->title); free(t->album); free(t->identity);
            c->oom = true;
            return;
        }
    }
    c->n++;
}

static void sy_collect_free(sy_collect *c) {
    size_t i;
    for (i = 0; i < c->n; i++) {
        free(c->v[i].identity);
        free(c->v[i].artist);
        free(c->v[i].title);
        free(c->v[i].album);
    }
    free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

/* Open-addressing identity map over a collected array (indices + 1 so 0
 * means empty; first collector wins on duplicate identities — duplicate
 * tag+duration files legitimately share one record per §7). */
typedef struct sy_map {
    uint64_t *hash;
    size_t   *slot;   /* index into sy_collect.v, +1 (0 = empty)         */
    size_t    cap;    /* power of two                                    */
} sy_map;

static bool sy_map_build(sy_map *m, const sy_collect *c) {
    size_t cap = 64, i;
    while (cap < c->n * 2 + 1) cap *= 2;
    m->hash = (uint64_t *)calloc(cap, sizeof(uint64_t));
    m->slot = (size_t *)calloc(cap, sizeof(size_t));
    m->cap  = cap;
    if (!m->hash || !m->slot) return false;
    for (i = 0; i < c->n; i++) {
        uint64_t h = sy_hash(c->v[i].identity);
        size_t   b = (size_t)h & (cap - 1);
        for (;;) {
            if (m->slot[b] == 0) {
                m->hash[b] = h;
                m->slot[b] = i + 1;
                break;
            }
            if (m->hash[b] == h &&
                strcmp(c->v[m->slot[b] - 1].identity, c->v[i].identity) == 0) {
                break;   /* duplicate identity: keep the first row */
            }
            b = (b + 1) & (cap - 1);
        }
    }
    return true;
}

static sy_local *sy_map_find(const sy_map *m, sy_collect *c, const char *identity) {
    uint64_t h = sy_hash(identity);
    size_t   b = (size_t)h & (m->cap - 1);
    for (;;) {
        if (m->slot[b] == 0) return NULL;
        if (m->hash[b] == h &&
            strcmp(c->v[m->slot[b] - 1].identity, identity) == 0) {
            return &c->v[m->slot[b] - 1];
        }
        b = (b + 1) & (m->cap - 1);
    }
}

static void sy_map_free(sy_map *m) {
    free(m->hash);
    free(m->slot);
    m->hash = NULL;
    m->slot = NULL;
    m->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Snapshot builder (§2)                                               */
/* ------------------------------------------------------------------ */

char *mn_sync_build_snapshot(const mn_sync_env *env) {
    sy_collect c;
    sy_strbuf  b;
    size_t     i;
    mn_status  st;

    if (!env || !env->lib) return NULL;

    memset(&c, 0, sizeof(c));
    c.want_tags = true;
    sy_env_lock(env);
    st = mn_library_sync_enumerate(env->lib, false, sy_collect_cb, &c);
    sy_env_unlock(env);
    if (st != MN_OK || c.oom) {
        sy_collect_free(&c);
        return NULL;
    }

    sy_sb_init(&b);
    sy_sb_puts(&b, "{\"protocol\":1,\"device\":\"desktop\",\"exportedAt\":");
    sy_sb_json_i64(&b, sy_now_ms());
    sy_sb_puts(&b, ",\"items\":[");
    {
        size_t emitted = 0;
        for (i = 0; i < c.n; i++) {
            const sy_local *t = &c.v[i];
            /* Field toggles: disabled groups emit as defaults, and a record
             * with NOTHING enabled left is skipped entirely (§2 says emit
             * only non-default records). */
            int32_t s_liked = env->fields.likes   ? t->liked          : 0;
            int32_t s_stars = env->fields.ratings ? t->rating_x2 / 2  : 0;
            int64_t s_plays = env->fields.plays   ? t->play_count     : 0;
            int64_t s_last  = env->fields.plays && t->last_played_s > 0
                            ? t->last_played_s * 1000 : 0;
            if (!s_liked && !s_stars && !s_plays && !s_last) continue;
            if (emitted++) sy_sb_putc(&b, ',');
            sy_sb_puts(&b, "{\"id\":");
            sy_sb_json_str(&b, t->identity);
            sy_sb_puts(&b, ",\"artist\":");
            sy_sb_json_str(&b, t->artist);
            sy_sb_puts(&b, ",\"title\":");
            sy_sb_json_str(&b, t->title);
            sy_sb_puts(&b, ",\"album\":");
            sy_sb_json_str(&b, t->album);
            sy_sb_puts(&b, ",\"liked\":");
            sy_sb_json_bool(&b, s_liked == 1);
            sy_sb_puts(&b, ",\"disliked\":");
            sy_sb_json_bool(&b, s_liked == -1);
            sy_sb_puts(&b, ",\"stars\":");
            sy_sb_json_i64(&b, s_stars);
            sy_sb_puts(&b, ",\"playCount\":");
            sy_sb_json_i64(&b, s_plays);
            sy_sb_puts(&b, ",\"lastPlayedAt\":");
            sy_sb_json_i64(&b, s_last);
            sy_sb_puts(&b, ",\"updatedAt\":");
            sy_sb_json_i64(&b, t->pref_updated_ms);
            sy_sb_putc(&b, '}');
        }
    }
    sy_sb_puts(&b, "]}");
    sy_collect_free(&c);

    if (b.oom) {
        sy_sb_free(&b);
        return NULL;
    }
    return b.data;   /* ownership handed to the caller */
}

/* ------------------------------------------------------------------ */
/* Merge (§3)                                                          */
/* ------------------------------------------------------------------ */

/* One parsed remote record. */
typedef struct sy_remote {
    char   *id;              /* malloc'd identity                       */
    bool    liked;
    bool    disliked;
    int     stars;
    int64_t play_count;
    int64_t last_played_ms;
    int64_t updated_ms;
} sy_remote;

/*
 * Extract the next {...} object of the items array. `*pp` advances past the
 * returned object. Returns a malloc'd NUL-terminated copy, or NULL at the
 * array end / on malformed input. String-aware brace matching so titles
 * containing braces don't derail it.
 */
static char *sy_next_item(const char **pp) {
    const char *p = *pp, *q;
    int   depth = 0;
    bool  instr = false;
    char *item;
    size_t len;

    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    for (q = p; *q; q++) {
        char ch = *q;
        if (instr) {
            if (ch == '\\' && q[1]) q++;
            else if (ch == '"') instr = false;
        } else if (ch == '"') {
            instr = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) { q++; break; }
        }
    }
    if (depth != 0) return NULL;
    len = (size_t)(q - p);
    item = (char *)malloc(len + 1);
    if (!item) return NULL;
    memcpy(item, p, len);
    item[len] = 0;
    *pp = q;
    return item;
}

/* Parse every record of a snapshot's items array into a malloc'd sy_remote
 * array (caller frees ids + array). Returns false on malformed input. */
static bool sy_parse_items(const char *json, sy_remote **out_v, size_t *out_n) {
    const char *p;
    sy_remote  *v = NULL;
    size_t      n = 0, cap = 0;
    char        idbuf[SY_ID_MAX];

    *out_v = NULL;
    *out_n = 0;
    p = sy_json_find_value(json, "items");
    if (!p || *p != '[') return false;
    p++;
    for (;;) {
        char *item = sy_next_item(&p);
        sy_remote r;
        if (!item) break;
        memset(&r, 0, sizeof(r));
        if (sy_json_get_str(item, "id", idbuf, sizeof(idbuf)) && idbuf[0]) {
            r.liked          = sy_json_get_bool(item, "liked", false);
            r.disliked       = sy_json_get_bool(item, "disliked", false);
            if (r.liked) r.disliked = false;   /* both set: liked wins (§7) */
            r.stars          = (int)sy_json_get_i64(item, "stars", 0);
            r.play_count     = sy_json_get_i64(item, "playCount", 0);
            r.last_played_ms = sy_json_get_i64(item, "lastPlayedAt", 0);
            r.updated_ms     = sy_json_get_i64(item, "updatedAt", 0);
            if (r.stars < 0) r.stars = 0;
            if (r.stars > 5) r.stars = 5;
            if (r.play_count < 0) r.play_count = 0;
            if (r.last_played_ms < 0) r.last_played_ms = 0;
            if (r.updated_ms < 0) r.updated_ms = 0;
            r.id = sy_strdup(idbuf);
            if (r.id) {
                if (n == cap) {
                    size_t     ncap = cap ? cap * 2 : 256;
                    sy_remote *nv = (sy_remote *)realloc(v, ncap * sizeof(*nv));
                    if (!nv) { free(r.id); free(item); break; }
                    v = nv;
                    cap = ncap;
                }
                v[n++] = r;
            }
        }
        free(item);
    }
    *out_v = v;
    *out_n = n;
    return true;
}

bool mn_sync_merge_snapshot(const mn_sync_env *env, const char *json,
                            int *out_applied, int *out_skipped) {
    sy_remote *rv = NULL;
    size_t     rn = 0, i;
    sy_collect c;
    sy_map     map;
    int        applied = 0, skipped = 0;
    bool       ok = false, own_txn = false;
    int64_t    proto;

    if (out_applied) *out_applied = 0;
    if (out_skipped) *out_skipped = 0;
    if (!env || !env->lib || !json) return false;

    /* §6: refuse snapshots from a newer protocol. */
    proto = sy_json_get_i64(json, "protocol", -1);
    if (proto != MN_SYNC_PROTOCOL) return false;

    if (!sy_parse_items(json, &rv, &rn)) return false;

    memset(&c, 0, sizeof(c));
    memset(&map, 0, sizeof(map));
    c.want_tags = false;

    sy_env_lock(env);
    if (mn_library_sync_enumerate(env->lib, true, sy_collect_cb, &c) != MN_OK
        || c.oom || !sy_map_build(&map, &c)) {
        sy_env_unlock(env);
        goto done;
    }

    /* One transaction around the whole apply loop (skipped when a scan's
     * batch transaction is already open — same pattern as purge_missing). */
    own_txn = !mn_library_in_transaction(env->lib);
    if (own_txn && mn_library_begin(env->lib) != MN_OK) own_txn = false;

    for (i = 0; i < rn; i++) {
        const sy_remote *r = &rv[i];
        sy_local *loc = sy_map_find(&map, &c, r->id);
        int64_t   new_play, new_last_ms, fin_upd;
        int       fin_liked, fin_disliked, fin_stars, fin_thumbs;

        if (!loc) { skipped++; continue; }

        /* Play stats are monotonic: take the MAX (never lose plays) —
         * unless the plays group is disabled in the sync-fields settings,
         * in which case local values stand untouched. */
        if (env->fields.plays) {
            new_play    = (r->play_count > loc->play_count)
                        ? r->play_count : loc->play_count;
            new_last_ms = (r->last_played_ms > loc->last_played_s * 1000)
                        ? r->last_played_ms : loc->last_played_s * 1000;
        } else {
            new_play    = loc->play_count;
            new_last_ms = loc->last_played_s * 1000;
        }

        /* Preference fields move as a GROUP: newest updatedAt wins — but
         * each sub-group only applies when its toggle is on (a disabled
         * group keeps the LOCAL value even when remote is newer). */
        if (r->updated_ms > loc->pref_updated_ms
            && (env->fields.likes || env->fields.ratings)) {
            if (env->fields.likes) {
                fin_liked    = r->liked ? 1 : 0;
                fin_disliked = (!r->liked && r->disliked) ? 1 : 0;
            } else {
                fin_liked    = (loc->liked == 1)  ? 1 : 0;
                fin_disliked = (loc->liked == -1) ? 1 : 0;
            }
            fin_stars = env->fields.ratings ? r->stars : loc->rating_x2 / 2;
            fin_upd   = r->updated_ms;
        } else {
            fin_liked    = (loc->liked == 1)  ? 1 : 0;
            fin_disliked = (loc->liked == -1) ? 1 : 0;
            fin_stars    = loc->rating_x2 / 2;
            fin_upd      = loc->pref_updated_ms;
        }
        fin_thumbs = fin_liked ? 1 : (fin_disliked ? -1 : 0);

        /* Only write rows that actually change (idempotent re-merges are
         * no-ops; last_played compares in the stored unit, seconds). */
        if (fin_thumbs      == loc->liked &&
            fin_stars * 2   == loc->rating_x2 &&
            new_play        == loc->play_count &&
            new_last_ms / 1000 == loc->last_played_s &&
            fin_upd         == loc->pref_updated_ms) {
            continue;
        }
        if (mn_library_sync_apply(env->lib, loc->id, fin_liked, fin_disliked,
                                  fin_stars, new_play, new_last_ms,
                                  fin_upd) == MN_OK) {
            applied++;
            /* Keep the in-memory row current so duplicate remote ids merge
             * against the merged values, not the stale ones. */
            loc->liked           = (int32_t)fin_thumbs;
            loc->rating_x2       = (int32_t)(fin_stars * 2);
            loc->play_count      = new_play;
            loc->last_played_s   = new_last_ms / 1000;
            loc->pref_updated_ms = fin_upd;
        }
    }

    if (own_txn && mn_library_in_transaction(env->lib)) {
        (void)mn_library_commit(env->lib);
    }
    sy_env_unlock(env);
    ok = true;

done:
    sy_map_free(&map);
    sy_collect_free(&c);
    for (i = 0; i < rn; i++) free(rv[i].id);
    free(rv);
    if (out_applied) *out_applied = applied;
    if (out_skipped) *out_skipped = skipped;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Plain-HTTP client (WinHTTP; the phone's server is HTTP, not HTTPS)  */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/*
 * One request against the phone's LAN sync server. GET when `body` is NULL,
 * POST (application/json) otherwise. The response is read chunk-by-chunk
 * into a growable buffer capped at SY_HTTP_CAP. Returns the malloc'd
 * NUL-terminated body (caller frees) or NULL on any failure / non-200.
 */
static char *sy_http_req(const char *host, int port, const char *path,
                         const char *body) {
    wchar_t   whost[256], wpath[512];
    HINTERNET s = NULL, c = NULL, r = NULL;
    char     *buf = NULL;
    size_t    len = 0, cap = 0;
    bool      ok = false;

    if (!host || !host[0] || !path) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256) <= 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512) <= 0) return NULL;

    s = WinHttpOpen(L"Monatomic/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    /* resolve/connect ~5 s, send/receive ~10 s */
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 10000);
    c = WinHttpConnect(s, whost, (INTERNET_PORT)port, 0);
    if (!c) goto done;
    /* No WINHTTP_FLAG_SECURE: the phone's server is plain HTTP (§4a). */
    r = WinHttpOpenRequest(c, body ? L"POST" : L"GET", wpath, NULL,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!r) goto done;
    {
        const wchar_t *hdrs = body ? L"Content-Type: application/json\r\n"
                                   : WINHTTP_NO_ADDITIONAL_HEADERS;
        DWORD hlen = body ? (DWORD)-1 : 0;
        DWORD blen = body ? (DWORD)strlen(body) : 0;
        if (!WinHttpSendRequest(r, hdrs, hlen,
                                body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                                blen, blen, 0) ||
            !WinHttpReceiveResponse(r, NULL)) goto done;
    }
    {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) goto done;
    }
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(r, &avail)) goto done;
        if (avail == 0) break;
        if (len + avail + 1 > SY_HTTP_CAP) goto done;
        if (len + avail + 1 > cap) {
            size_t ncap = cap ? cap : 16384;
            char  *nb;
            while (len + avail + 1 > ncap) ncap *= 2;
            nb = (char *)realloc(buf, ncap);
            if (!nb) goto done;
            buf = nb;
            cap = ncap;
        }
        if (!WinHttpReadData(r, buf + len, avail, &got)) goto done;
        if (got == 0) break;
        len += got;
    }
    if (!buf) {
        buf = (char *)malloc(1);
        if (!buf) goto done;
    }
    buf[len] = '\0';
    ok = true;
done:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok) { free(buf); buf = NULL; }
    return buf;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Full sync flow (§4a)                                                */
/* ------------------------------------------------------------------ */

bool mn_sync_run(const mn_sync_env *env, const char *host, int port,
                 mn_sync_progress_cb cb, void *user) {
#ifndef _WIN32
    (void)env; (void)host; (void)port;
    if (cb) cb(user, "error", 0, 0, 0, "HTTP sync is Windows-only for now");
    return false;
#else
    char *body = NULL;
    char *mine = NULL;
    int   applied = 0, skipped = 0, pushed = 0;

    if (!env || !env->lib || !host || !host[0]) {
        if (cb) cb(user, "error", 0, 0, 0, "no sync host configured");
        return false;
    }
    if (port <= 0 || port > 65535) port = MN_SYNC_DEFAULT_PORT;

    /* 1. ping: reachable + protocol == 1. */
    if (cb) cb(user, "connecting", 0, 0, 0, "");
    body = sy_http_req(host, port, "/sync/ping", NULL);
    if (!body) {
        if (cb) cb(user, "error", 0, 0, 0, "phone unreachable (is the sync server on?)");
        return false;
    }
    if (sy_json_get_i64(body, "protocol", -1) != MN_SYNC_PROTOCOL) {
        free(body);
        if (cb) cb(user, "error", 0, 0, 0, "protocol mismatch — update the app(s)");
        return false;
    }
    free(body);

    /* 2. pull the phone's snapshot and merge it into this library. */
    if (cb) cb(user, "pulling", 0, 0, 0, "");
    body = sy_http_req(host, port, "/sync/snapshot", NULL);
    if (!body) {
        if (cb) cb(user, "error", 0, 0, 0, "snapshot fetch failed");
        return false;
    }
    if (cb) cb(user, "merging", 0, 0, 0, "");
    if (!mn_sync_merge_snapshot(env, body, &applied, &skipped)) {
        free(body);
        if (cb) cb(user, "error", 0, 0, 0, "merge failed (bad snapshot?)");
        return false;
    }
    free(body);

    /* 3. push our snapshot; the phone merges and reports its counts. */
    if (cb) cb(user, "pushing", applied, skipped, 0, "");
    mine = mn_sync_build_snapshot(env);
    if (!mine) {
        if (cb) cb(user, "error", applied, skipped, 0, "local snapshot build failed");
        return false;
    }
    body = sy_http_req(host, port, "/sync/merge", mine);
    free(mine);
    if (!body) {
        if (cb) cb(user, "error", applied, skipped, 0, "push failed");
        return false;
    }
    /* Top-level "applied" precedes the nested merged snapshot (§4a); the
     * returned snapshot itself is skipped — re-merging it is an idempotent
     * no-op by construction. */
    pushed = (int)sy_json_get_i64(body, "applied", 0);
    free(body);

    if (cb) cb(user, "done", applied, skipped, pushed, "");
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/* File export / import (§4b)                                          */
/* ------------------------------------------------------------------ */

void mn_sync_default_path(const char *data_dir, char *out, size_t n) {
    char dir[1200];
    if (!out || n == 0) return;
    out[0] = 0;
    if (!data_dir || !data_dir[0]) data_dir = ".";
    snprintf(dir, sizeof(dir), "%s/sync", data_dir);
    (void)sy_mkdir(dir);   /* best-effort; fails harmlessly when present */
    snprintf(out, n, "%s/%s", dir, MN_SYNC_FILE_NAME);
}

bool mn_sync_export_file(const mn_sync_env *env, const char *path) {
    char  *json;
    FILE  *f;
    size_t len;
    bool   ok;

    if (!env || !env->lib || !path || !path[0]) return false;
    json = mn_sync_build_snapshot(env);
    if (!json) return false;
    len = strlen(json);
    f = fopen(path, "wb");
    if (!f) {
        free(json);
        return false;
    }
    ok = (fwrite(json, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    free(json);
    return ok;
}

bool mn_sync_import_file(const mn_sync_env *env, const char *path,
                         int *out_applied, int *out_skipped) {
    FILE  *f;
    char  *json = NULL;
    long   sz;
    bool   ok = false;

    if (out_applied) *out_applied = 0;
    if (out_skipped) *out_skipped = 0;
    if (!env || !env->lib || !path || !path[0]) return false;

    f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 &&
        sz <= (long)SY_HTTP_CAP && fseek(f, 0, SEEK_SET) == 0) {
        json = (char *)malloc((size_t)sz + 1);
        if (json) {
            size_t got = fread(json, 1, (size_t)sz, f);
            json[got] = '\0';
            ok = true;
        }
    }
    fclose(f);
    if (!ok || !json) {
        free(json);
        return false;
    }
    ok = mn_sync_merge_snapshot(env, json, out_applied, out_skipped);
    free(json);
    return ok;
}
