/*
 * discover.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * Zero-config LAN discovery of the phone (see discover.h).
 *
 * Winsock UDP broadcast: the ASCII probe MN_DISCOVER_PROBE goes to
 * UDP MN_DISCOVER_PORT at 255.255.255.255 AND at every interface's
 * subnet broadcast address (GetAdaptersInfo — the limited broadcast
 * is dropped by some Wi-Fi drivers/APs, the directed one usually
 * survives). Two probe rounds (~0 ms and ~500 ms) cover single-packet
 * loss; replies are collected in a ~1500 ms select() window, deduped
 * by sender address, and the first one whose JSON says "app":"nexgen"
 * wins. The phone's address is taken from the UDP sender, never from
 * the payload.
 *
 * JSON parsing reuses the sync.c idiom (tolerant key-scan extractors,
 * no JSON library). Blocking — worker thread only. Windows-only; the
 * portable stub reports "not found".
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#    define _WINSOCK_DEPRECATED_NO_WARNINGS   /* inet_addr on adapter strings */
#  endif
#  include <winsock2.h>      /* MUST precede windows.h */
#  include <ws2tcpip.h>
#  include <iphlpapi.h>      /* GetAdaptersInfo (link iphlpapi.lib) */
#  include <windows.h>
#endif

#include "discover.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32

/* ------------------------------------------------------------------ */
/* Tolerant JSON extractors (the sync.c sy_json_* pattern, replicated  */
/* so this module stays dependency-free — no JSON library)             */
/* ------------------------------------------------------------------ */

static const char *dv_json_find_value(const char *json, const char *key) {
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

static int64_t dv_json_get_i64(const char *json, const char *key, int64_t dflt) {
    const char *v = dv_json_find_value(json, key);
    char *end = NULL;
    long long r;
    if (!v) return dflt;
    if (*v == '"') v++;
    r = strtoll(v, &end, 10);
    if (end == v) return dflt;
    return (int64_t)r;
}

/* Bounded string extraction with escape decoding (device names can carry
 * escaped characters; unicode escapes re-encode as UTF-8). */
static bool dv_json_get_str(const char *json, const char *key,
                            char *out, size_t out_n) {
    const char *v = dv_json_find_value(json, key);
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
/* Broadcast target collection                                         */
/* ------------------------------------------------------------------ */

/*
 * Fill `out` with the limited broadcast (255.255.255.255) plus every
 * up-interface's directed subnet broadcast ((ip & mask) | ~mask), deduped.
 * Addresses are in network byte order. Returns the count (>= 1: the
 * limited broadcast always goes in, even if adapter enumeration fails).
 */
static size_t dv_broadcast_targets(struct in_addr *out, size_t max_out) {
    size_t n = 0;
    ULONG  len = 0;

    if (max_out == 0) return 0;
    out[n++].s_addr = INADDR_BROADCAST;

    if (GetAdaptersInfo(NULL, &len) == ERROR_BUFFER_OVERFLOW && len > 0) {
        IP_ADAPTER_INFO *ai = (IP_ADAPTER_INFO *)malloc(len);
        if (ai && GetAdaptersInfo(ai, &len) == NO_ERROR) {
            const IP_ADAPTER_INFO *a;
            for (a = ai; a; a = a->Next) {
                const IP_ADDR_STRING *ip;
                for (ip = &a->IpAddressList; ip; ip = ip->Next) {
                    unsigned long addr = inet_addr(ip->IpAddress.String);
                    unsigned long mask = inet_addr(ip->IpMask.String);
                    unsigned long bcast;
                    size_t k;
                    /* skip unconfigured (0.0.0.0), unparsable, loopback */
                    if (addr == INADDR_NONE || addr == 0) continue;
                    if ((ntohl(addr) >> 24) == 127) continue;
                    if (mask == INADDR_NONE || mask == 0) continue;
                    bcast = (addr & mask) | ~mask;
                    if (bcast == INADDR_BROADCAST) continue;   /* already sent */
                    for (k = 0; k < n; k++) {
                        if (out[k].s_addr == bcast) break;
                    }
                    if (k < n) continue;                       /* duplicate */
                    if (n < max_out) out[n++].s_addr = bcast;
                }
            }
        }
        free(ai);
    }
    return n;
}

/* One probe round: MN_DISCOVER_PROBE to every target (best-effort). */
static void dv_send_probes(SOCKET s, const struct in_addr *targets, size_t n) {
    struct sockaddr_in dest;
    size_t i;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((u_short)MN_DISCOVER_PORT);
    for (i = 0; i < n; i++) {
        dest.sin_addr = targets[i];
        (void)sendto(s, MN_DISCOVER_PROBE, (int)strlen(MN_DISCOVER_PROBE), 0,
                     (const struct sockaddr *)&dest, (int)sizeof(dest));
    }
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

int mn_discover_scan(mn_found_device *out, int max_out, int window_ms) {
#ifndef _WIN32
    (void)out; (void)max_out; (void)window_ms;
    return 0;
#else
    WSADATA   wsa;
    SOCKET    s;
    struct sockaddr_in local;
    struct in_addr     targets[16];
    size_t    ntargets;
    BOOL      yes = TRUE;
    ULONGLONG start, deadline, next_round;
    int       rounds_sent, found = 0;
    /* dedupe: sender addresses already examined this run */
    unsigned long seen[64];
    size_t        nseen = 0;

    if (!out || max_out <= 0) return 0;
    if (window_ms < 500)   window_ms = 500;
    if (window_ms > 10000) window_ms = 10000;

    /* No app-wide Winsock init exists (WinHTTP does its own); a local
     * refcounted WSAStartup/WSACleanup pair keeps this self-contained. */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                     (const char *)&yes, sizeof(yes));
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = 0;   /* ephemeral */
    if (bind(s, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        closesocket(s);
        WSACleanup();
        return 0;
    }

    ntargets = dv_broadcast_targets(targets, 16);

    /* three probe rounds spread over the front 2/3 of the window (a lone
     * broadcast datagram over Wi-Fi is too easy to lose), one receive
     * window for the whole thing */
    start      = GetTickCount64();
    deadline   = start + (ULONGLONG)window_ms;
    next_round = start + (ULONGLONG)window_ms / 3;
    dv_send_probes(s, targets, ntargets);
    rounds_sent = 1;

    while (found < max_out) {
        ULONGLONG now = GetTickCount64(), until;
        fd_set         rf;
        struct timeval tv;
        int            r;

        if (now >= deadline) break;
        if (rounds_sent < 3 && now >= next_round) {
            dv_send_probes(s, targets, ntargets);
            rounds_sent++;
            next_round = start
                       + (ULONGLONG)window_ms * (unsigned)(rounds_sent) / 3;
        }
        /* wake for the next probe round even if nothing arrives */
        until = (rounds_sent < 3 && next_round < deadline) ? next_round
                                                           : deadline;
        if (until <= now) continue;
        tv.tv_sec  = (long)((until - now) / 1000);
        tv.tv_usec = (long)(((until - now) % 1000) * 1000);
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        r = select(0 /* ignored on Winsock */, &rf, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;

        {
            char buf[1500];
            char app[32];
            struct sockaddr_in from;
            int    flen = (int)sizeof(from);
            int    got  = recvfrom(s, buf, (int)sizeof(buf) - 1, 0,
                                   (struct sockaddr *)&from, &flen);
            size_t k;
            mn_found_device *d;
            if (got <= 0) continue;
            buf[got] = 0;

            /* dedupe: several broadcast targets (and probe rounds) can
             * elicit replies from the same device — record each sender
             * address once */
            for (k = 0; k < nseen; k++) {
                if (seen[k] == from.sin_addr.s_addr) break;
            }
            if (k < nseen) continue;
            if (nseen < sizeof(seen) / sizeof(seen[0])) {
                seen[nseen++] = from.sin_addr.s_addr;
            }

            /* only our ecosystem's reply counts: {"app":"nexgen",...} */
            if (!dv_json_get_str(buf, "app", app, sizeof(app)) ||
                strcmp(app, "nexgen") != 0) {
                continue;
            }
            d = &out[found++];
            memset(d, 0, sizeof(*d));
            {
                const unsigned char *b =
                    (const unsigned char *)&from.sin_addr.s_addr;
                snprintf(d->host, sizeof(d->host), "%u.%u.%u.%u",
                         b[0], b[1], b[2], b[3]);
            }
            d->port = (int)dv_json_get_i64(buf, "port", MN_DISCOVER_PORT);
            if (d->port <= 0 || d->port > 65535) d->port = MN_DISCOVER_PORT;
            (void)dv_json_get_str(buf, "name", d->model, sizeof(d->model));
            d->protocol = (int)dv_json_get_i64(buf, "protocol", 0);
        }
    }

    closesocket(s);
    WSACleanup();
    return found;
#endif /* _WIN32 */
}

int mn_discover_phone(char *out_host, size_t host_n, int *out_port,
                      char *out_name, size_t name_n) {
    mn_found_device d;
    int n;
    /* zero the outputs up front so failure paths leave them clean */
    if (out_host && host_n) out_host[0] = 0;
    if (out_name && name_n) out_name[0] = 0;
    if (out_port) *out_port = 0;
    /* max_out=1 keeps the historical early-exit: first reply wins */
    n = mn_discover_scan(&d, 1, 1500);
    if (n <= 0) return 0;
    if (out_host && host_n) snprintf(out_host, host_n, "%s", d.host);
    if (out_port) *out_port = d.port;
    if (out_name && name_n) snprintf(out_name, name_n, "%s", d.model);
    return 1;
}
