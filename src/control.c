/*
 * control.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * Desktop remote-control listener (see control.h for the contract and the
 * security model). One long-lived thread: bind + listen, then handle one
 * short HTTP exchange per accepted connection, sequentially — the caller
 * is a single phone tapping transport buttons and polling status at ~1 Hz,
 * not a web farm. Requests are size-capped, time-capped, and parsed with
 * the same hand-rolled minimalism as the rest of the C side.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define CT_REQ_MAX   8192      /* whole request (line + headers) cap      */
#define CT_RECV_MS   5000      /* per-connection receive window           */
#define CT_SESSION_MS (10 * 60 * 1000)  /* session-log rearm per client   */

static mn_control_env g_env;
static char           g_token[128];
static SOCKET         g_listen = INVALID_SOCKET;
static HANDLE         g_thread = NULL;
static volatile LONG  g_running = 0;

/* Last session-logged client, so the activity log gets one line per
 * controlling phone per stretch, not one per button press. */
static char           g_last_ip[64];
static ULONGLONG      g_last_ip_ms = 0;

/* Constant-time token compare (length leak is fine, content leak is not). */
static bool ct_token_ok(const char *given) {
    size_t n = strlen(g_token), i;
    unsigned diff;
    if (!given || strlen(given) != n || n == 0) return false;
    diff = 0;
    for (i = 0; i < n; i++) diff |= (unsigned)(g_token[i] ^ given[i]);
    return diff == 0;
}

static void ct_send(SOCKET c, int code, const char *reason,
                    const char *json) {
    char hdr[256];
    int  blen = (int)strlen(json);
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Type: application/json\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n"
                         "Content-Length: %d\r\n\r\n",
                         code, reason, blen);
    if (hlen > 0) send(c, hdr, hlen, 0);
    if (blen > 0) send(c, json, blen, 0);
}

static void ct_send_err(SOCKET c, int code, const char *reason,
                        const char *msg) {
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    ct_send(c, code, reason, body);
}

/* Parse "?key=<num>" out of a path's query string. */
static bool ct_query_num(const char *path, const char *key, double *out) {
    const char *q = strchr(path, '?');
    size_t klen = strlen(key);
    if (!q) return false;
    q++;
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            char *end = NULL;
            double v = strtod(q + klen + 1, &end);
            if (end == q + klen + 1) return false;
            *out = v;
            return true;
        }
        q = strchr(q, '&');
        if (!q) break;
        q++;
    }
    return false;
}

static void ct_handle(SOCKET c, const char *client_ip) {
    char   req[CT_REQ_MAX + 1];
    int    got = 0, n;
    char   method[8], path[512], token[160];
    char  *hdrs, *route;
    bool   authed;
    DWORD  tmo = CT_RECV_MS;

    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));

    /* Read until the header terminator (no request bodies are used by the
     * control surface; a stray body is simply left unread — we close). */
    for (;;) {
        if (got >= CT_REQ_MAX) break;
        n = recv(c, req + got, CT_REQ_MAX - got, 0);
        if (n <= 0) break;
        got += n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (got <= 0) return;
    req[got] = 0;

    method[0] = path[0] = token[0] = 0;
    if (sscanf(req, "%7s %511s", method, path) != 2) {
        ct_send_err(c, 400, "Bad Request", "malformed request");
        return;
    }

    /* X-Auth-Token header (case-insensitive scan of the header block). */
    hdrs = strstr(req, "\r\n");
    if (hdrs) {
        const char *p = hdrs;
        while (p && *p) {
            const char *line = p + 2;
            const char *eol = strstr(line, "\r\n");
            if (!eol || eol == line) break;
            if (_strnicmp(line, "X-Auth-Token:", 13) == 0) {
                const char *v = line + 13;
                size_t o = 0;
                while (*v == ' ' || *v == '\t') v++;
                while (v < eol && o + 1 < sizeof(token)) token[o++] = *v++;
                token[o] = 0;
                while (o > 0 && (token[o - 1] == ' ' || token[o - 1] == '\r'))
                    token[--o] = 0;
            }
            p = eol;
        }
    }

    if (strncmp(path, "/control/", 9) != 0) {
        ct_send_err(c, 404, "Not Found", "not found");
        return;
    }
    route = path + 9;

    /* Unauthenticated reachability probe — the ONLY tokenless endpoint.
     * Reveals nothing but "a Monatomic control listener lives here". */
    if (strncmp(route, "ping", 4) == 0) {
        ct_send(c, 200, "OK", "{\"app\":\"monatomic\",\"control\":1}");
        return;
    }

    authed = ct_token_ok(token[0] ? token : NULL);
    if (!authed) {
        ct_send_err(c, 401, "Unauthorized", "bad or missing X-Auth-Token");
        return;
    }

    /* One session line per client per stretch (activity-log hook). */
    if (g_env.session) {
        ULONGLONG now = GetTickCount64();
        if (strcmp(g_last_ip, client_ip) != 0 ||
            now - g_last_ip_ms > CT_SESSION_MS) {
            snprintf(g_last_ip, sizeof(g_last_ip), "%s", client_ip);
            g_env.session(g_env.user, client_ip);
        }
        g_last_ip_ms = now;
    }

    if (strncmp(route, "status", 6) == 0) {
        char *json = g_env.status ? g_env.status(g_env.user) : NULL;
        if (json) {
            ct_send(c, 200, "OK", json);
            free(json);
        } else {
            ct_send_err(c, 500, "Internal Server Error", "status unavailable");
        }
        return;
    }

    {
        /* Transport commands — POST by contract, but the method is not
         * load-bearing (the token is the gate; GET from a browser address
         * bar is a convenience, exactly like the phone's surface). */
        static const char *plain[] = {
            "play", "pause", "stop", "toggle", "next", "previous", "prev"
        };
        size_t i;
        for (i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
            size_t rl = strlen(plain[i]);
            if (strncmp(route, plain[i], rl) == 0 &&
                (route[rl] == 0 || route[rl] == '?')) {
                const char *name = (strcmp(plain[i], "previous") == 0)
                                 ? "prev" : plain[i];
                if (g_env.command &&
                    g_env.command(g_env.user, name, 0, false)) {
                    ct_send(c, 200, "OK", "{\"ok\":true}");
                } else {
                    ct_send_err(c, 500, "Internal Server Error",
                                "command failed");
                }
                return;
            }
        }
    }

    if (strncmp(route, "seekby", 6) == 0) {
        double ms;
        if (!ct_query_num(path, "ms", &ms)) {
            ct_send_err(c, 400, "Bad Request", "seekby needs ?ms=<delta>");
            return;
        }
        if (g_env.command && g_env.command(g_env.user, "seekby", ms, true))
            ct_send(c, 200, "OK", "{\"ok\":true}");
        else
            ct_send_err(c, 500, "Internal Server Error", "command failed");
        return;
    }
    if (strncmp(route, "seek", 4) == 0) {
        double ms;
        if (!ct_query_num(path, "ms", &ms)) {
            ct_send_err(c, 400, "Bad Request", "seek needs ?ms=<position>");
            return;
        }
        if (g_env.command && g_env.command(g_env.user, "seek", ms, true))
            ct_send(c, 200, "OK", "{\"ok\":true}");
        else
            ct_send_err(c, 500, "Internal Server Error", "command failed");
        return;
    }
    if (strncmp(route, "volume", 6) == 0) {
        double v;
        if (!ct_query_num(path, "v", &v)) {
            ct_send_err(c, 400, "Bad Request", "volume needs ?v=<0..1>");
            return;
        }
        if (g_env.command && g_env.command(g_env.user, "volume", v, true))
            ct_send(c, 200, "OK", "{\"ok\":true}");
        else
            ct_send_err(c, 500, "Internal Server Error", "command failed");
        return;
    }

    ct_send_err(c, 404, "Not Found", "no such control endpoint");
}

static DWORD WINAPI ct_thread(LPVOID param) {
    (void)param;
    if (g_env.thread_begin) g_env.thread_begin(g_env.user);
    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        struct sockaddr_in peer;
        int    plen = sizeof(peer);
        char   ip[64];
        SOCKET c = accept(g_listen, (struct sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) {
            if (!InterlockedCompareExchange(&g_running, 0, 0)) break;
            Sleep(50);   /* transient accept failure; don't spin */
            continue;
        }
        ip[0] = 0;
        if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)))
            snprintf(ip, sizeof(ip), "?");
        ct_handle(c, ip);
        closesocket(c);
    }
    if (g_env.thread_end) g_env.thread_end(g_env.user);
    return 0;
}

bool mn_control_start(const mn_control_env *env, int port, const char *token) {
    WSADATA wsa;
    struct sockaddr_in addr;
    BOOL reuse = TRUE;

    if (g_thread) return true;   /* already running */
    if (!env || !token || !token[0]) return false;
    if (port <= 0 || port > 65535) port = MN_CONTROL_DEFAULT_PORT;

    g_env = *env;
    snprintf(g_token, sizeof(g_token), "%s", token);
    g_last_ip[0] = 0;
    g_last_ip_ms = 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* LAN reach; token gates */
    addr.sin_port        = htons((unsigned short)port);
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(g_listen, 4) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }

    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, ct_thread, NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    return true;
}

void mn_control_stop(void) {
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);   /* breaks the blocking accept() */
        g_listen = INVALID_SOCKET;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

#else  /* !_WIN32 */

bool mn_control_start(const mn_control_env *env, int port, const char *token) {
    (void)env; (void)port; (void)token;
    return false;
}
void mn_control_stop(void) {}

#endif /* _WIN32 */
