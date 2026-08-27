/*
 * devices.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * Sync device registry (see devices.h). Pure data + a tiny line-file
 * persistence format; the caller owns locking and all I/O policy.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mn_devreg_init(mn_devreg *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->next_id = 1;
}

/* '|' is the field separator and \r\n end lines, so both are dropped
 * from free-text on the way in; other control chars go with them. */
void mn_devreg_set_text(char *dst, size_t dst_n, const char *src) {
    size_t o = 0;
    if (!dst || dst_n == 0) return;
    for (; src && *src && o + 1 < dst_n; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '|' || c < 0x20) continue;
        dst[o++] = (char)c;
    }
    dst[o] = 0;
    /* trim trailing spaces so "name |" round-trips stably */
    while (o > 0 && dst[o - 1] == ' ') dst[--o] = 0;
}

mn_device *mn_devreg_find(mn_devreg *r, int id) {
    int i;
    if (!r || id <= 0) return NULL;
    for (i = 0; i < r->count; i++) {
        if (r->dev[i].id == id) return &r->dev[i];
    }
    return NULL;
}

mn_device *mn_devreg_find_host(mn_devreg *r, const char *host, int port) {
    int i;
    if (!r || !host || !host[0]) return NULL;
    for (i = 0; i < r->count; i++) {
        if (r->dev[i].port == port && strcmp(r->dev[i].host, host) == 0)
            return &r->dev[i];
    }
    return NULL;
}

mn_device *mn_devreg_active(mn_devreg *r) {
    return r ? mn_devreg_find(r, r->active_id) : NULL;
}

mn_device *mn_devreg_add(mn_devreg *r, const char *name, const char *model,
                         const char *host, int port) {
    mn_device *d;
    char       h[64] = {0};
    if (!r || !host || !host[0]) return NULL;
    if (port <= 0 || port > 65535) port = 8797;
    mn_devreg_set_text(h, sizeof(h), host);
    if (!h[0]) return NULL;

    /* same endpoint twice = the same phone; hand back the existing row
     * (refreshing its model if discovery has since told us one) */
    d = mn_devreg_find_host(r, h, port);
    if (d) {
        if (model && model[0] && !d->model[0])
            mn_devreg_set_text(d->model, sizeof(d->model), model);
        return d;
    }
    if (r->count >= MN_DEV_MAX) return NULL;

    d = &r->dev[r->count++];
    memset(d, 0, sizeof(*d));
    d->id   = r->next_id++;
    d->port = port;
    snprintf(d->host, sizeof(d->host), "%s", h);
    mn_devreg_set_text(d->model, sizeof(d->model), model ? model : "");
    mn_devreg_set_text(d->name, sizeof(d->name), name ? name : "");
    if (!d->name[0]) {
        /* something human-readable, always: model beats a bare IP */
        snprintf(d->name, sizeof(d->name), "%s",
                 d->model[0] ? d->model : d->host);
    }
    /* first pairing selects itself — the common case is exactly one
     * phone, and "added but still no active device" is a trap state */
    if (r->count == 1 && r->active_id == 0) r->active_id = d->id;
    return d;
}

bool mn_devreg_remove(mn_devreg *r, int id) {
    int i;
    if (!r) return false;
    for (i = 0; i < r->count; i++) {
        if (r->dev[i].id != id) continue;
        memmove(&r->dev[i], &r->dev[i + 1],
                (size_t)(r->count - i - 1) * sizeof(mn_device));
        r->count--;
        if (r->active_id == id) r->active_id = 0;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

bool mn_devreg_save(const mn_devreg *r, const char *path) {
    FILE *f;
    int   i;
    if (!r || !path || !path[0]) return false;
    f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "A|%d|%d\n", r->active_id, r->next_id);
    for (i = 0; i < r->count; i++) {
        const mn_device *d = &r->dev[i];
        fprintf(f, "D|%d|%d|%lld|%lld|%s|%s|%s|%s\n",
                d->id, d->port,
                (long long)d->last_seen_ms, (long long)d->last_sync_ms,
                d->host, d->name, d->model, d->last_result);
    }
    fclose(f);
    return true;
}

/* Split `line` on '|' into at most `max` fields IN PLACE; the LAST
 * field keeps the rest of the line verbatim (free-text tail). */
static int dev_split(char *line, char **out, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        out[n++] = p;
        if (n == max) break;
        p = strchr(p, '|');
        if (!p) break;
        *p++ = 0;
    }
    return n;
}

bool mn_devreg_load(mn_devreg *r, const char *path) {
    FILE *f;
    char  line[512];
    if (!r) return false;
    mn_devreg_init(r);
    if (!path || !path[0]) return false;
    f = fopen(path, "r");
    if (!f) return false;

    while (fgets(line, sizeof(line), f)) {
        char *fld[9];
        int   n;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (line[0] == 'A') {
            n = dev_split(line, fld, 3);
            if (n >= 3) {
                r->active_id = atoi(fld[1]);
                r->next_id   = atoi(fld[2]);
            }
        } else if (line[0] == 'D' && r->count < MN_DEV_MAX) {
            mn_device *d;
            n = dev_split(line, fld, 9);
            if (n < 8) continue;                     /* malformed: skip */
            d = &r->dev[r->count];
            memset(d, 0, sizeof(*d));
            d->id           = atoi(fld[1]);
            d->port         = atoi(fld[2]);
            d->last_seen_ms = atoll(fld[3]);
            d->last_sync_ms = atoll(fld[4]);
            snprintf(d->host,  sizeof(d->host),  "%s", fld[5]);
            snprintf(d->name,  sizeof(d->name),  "%s", fld[6]);
            snprintf(d->model, sizeof(d->model), "%s", fld[7]);
            if (n >= 9)
                snprintf(d->last_result, sizeof(d->last_result), "%s", fld[8]);
            if (d->id <= 0 || !d->host[0]) continue; /* malformed: skip */
            if (d->port <= 0 || d->port > 65535) d->port = 8797;
            if (!d->name[0])
                snprintf(d->name, sizeof(d->name), "%s",
                         d->model[0] ? d->model : d->host);
            r->count++;
            if (d->id >= r->next_id) r->next_id = d->id + 1;
        }
    }
    fclose(f);
    if (r->next_id < 1) r->next_id = 1;
    /* a dangling active id (removed row persisted mid-crash) resolves
     * to "none selected", never to a wrong device */
    if (r->active_id && !mn_devreg_find(r, r->active_id)) r->active_id = 0;
    return true;
}
