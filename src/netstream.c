/*
 * netstream.c — Monatomic Music Player
 * ------------------------------------------------------------------
 * See netstream.h. WinHTTP progressive reader with a 4 MB ring buffer
 * (~4 minutes of 128 kbps radio), ICY metadata stripping, Range seek,
 * and live-mount reconnect. The reader side blocks inside the decoder
 * callbacks; an underrun therefore stalls the audio callback until
 * bytes arrive — correct behaviour for a live stream (silence gap),
 * and rare with this much buffer.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "netstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define NS_RING_CAP     (4u * 1024u * 1024u)
#define NS_HIST_MAX     (256u * 1024u)   /* consumed bytes kept for rewinds:
                                            decoder init probes (ID3 header,
                                            frame-sync scans) seek BACKWARD a
                                            little — impossible on a live
                                            mount unless we remember what we
                                            just handed out. */
#define NS_CHUNK        (64u * 1024u)
#define NS_META_MAX     (255u * 16u)
#define NS_CONNECT_MS   8000
#define NS_RECV_MS      12000
#define NS_READ_WAIT_MS 15000
#define NS_UA           L"Monatomic/1.0"

struct mn_netstream {
    /* immutable after open */
    char     url[2048];
    bool     want_icy;

    /* WinHTTP handles — owned by the fill thread after open; the
     * session handle outlives per-connection request handles. */
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;      /* current request; swapped on reconnect/seek */
    wchar_t   whost[512];
    wchar_t   wpath[1536];
    INTERNET_PORT port;
    bool      secure;

    /* response facts (first connection wins for name/type) */
    char      content_type[128];
    char      station_name[256];
    int64_t   content_length;   /* -1 unknown */
    bool      seekable;         /* content_length > 0 (Range resume works
                                   on every server that reports a length;
                                   Accept-Ranges is often omitted) */
    uint32_t  icy_metaint;      /* 0 = no interleaved metadata */

    /* ICY title */
    CRITICAL_SECTION title_cs;
    char      title[512];
    uint32_t  title_seq;

    /* ring buffer */
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv_data;   /* signalled when bytes arrive / EOF */
    CONDITION_VARIABLE cv_space;  /* signalled when the reader consumes */
    unsigned char *ring;
    size_t    head, tail, count;  /* tail=write, head=read */
    size_t    hist;               /* consumed bytes retained behind head */
    int64_t   read_pos;           /* absolute pos of next reader byte */
    int64_t   fill_pos;           /* absolute pos the fill thread is at */
    bool      eof;                /* fill side finished (or gave up) */
    bool      closing;
    int64_t   seek_to;            /* >=0: reader requested a seek */
    uint32_t  generation;         /* bumped per seek to flush in-flight */

    /* ICY strip state (fill thread only) */
    uint32_t  meta_countdown;     /* audio bytes until next meta block */
    uint32_t  meta_need;          /* remaining meta bytes to consume */
    uint32_t  meta_have;
    unsigned char meta_buf[NS_META_MAX + 1];

    HANDLE    fill_thread;
};

/* Atomically take ownership of the current request handle (reader-side
 * abort vs fill-side lifecycle both race on it). */
static HINTERNET ns_take_request(mn_netstream *ns) {
    return (HINTERNET)InterlockedExchangePointer((PVOID volatile *)&ns->request,
                                                 NULL);
}

/* ------------------------------------------------------------------ */

static void ns_utf8_to_wide(const char *s, wchar_t *out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    out[cap - 1] = 0;
}

static void ns_wide_to_utf8(const wchar_t *s, char *out, int cap) {
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    out[cap - 1] = 0;
}

/* Query a response header into UTF-8 (custom name or known index). */
static bool ns_header(HINTERNET req, DWORD info, const wchar_t *name,
                      char *out, size_t cap) {
    wchar_t buf[1024];
    DWORD   len = sizeof(buf) - sizeof(wchar_t);
    out[0] = 0;
    if (!WinHttpQueryHeaders(req, info,
                             name ? name : WINHTTP_HEADER_NAME_BY_INDEX,
                             buf, &len, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    buf[len / sizeof(wchar_t)] = 0;
    ns_wide_to_utf8(buf, out, (int)cap);
    return out[0] != 0;
}

/* Open one HTTP request at absolute byte offset `off` (0 = start).
 * Fills response facts on the FIRST connection only. Returns the
 * request handle or NULL. */
static HINTERNET ns_connect_at(mn_netstream *ns, int64_t off, bool first) {
    HINTERNET req;
    wchar_t   extra[256];
    DWORD     status = 0, sl = sizeof(status);
    DWORD     opt;

    req = WinHttpOpenRequest(ns->connect, L"GET", ns->wpath, NULL,
                             WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             ns->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return NULL;

    opt = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &opt, sizeof(opt));
    WinHttpSetTimeouts(req, NS_CONNECT_MS, NS_CONNECT_MS, NS_CONNECT_MS,
                       NS_RECV_MS);

    if (ns->want_icy) {
        WinHttpAddRequestHeaders(req, L"Icy-MetaData: 1\r\n", (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (off > 0) {
        swprintf(extra, 256, L"Range: bytes=%lld-", (long long)off);
        WinHttpAddRequestHeaders(req, extra, (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        return NULL;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                             WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(req);
        return NULL;
    }

    if (first) {
        char tmp[128];
        ns_header(req, WINHTTP_QUERY_CONTENT_TYPE, NULL,
                  ns->content_type, sizeof(ns->content_type));
        /* lowercase in place */
        { char *p; for (p = ns->content_type; *p; ++p)
              if (*p >= 'A' && *p <= 'Z') *p += 32; }
        ns->content_length = -1;
        if (ns_header(req, WINHTTP_QUERY_CUSTOM, L"Content-Length",
                      tmp, sizeof(tmp))) {
            long long v = _atoi64(tmp);
            if (v > 0) ns->content_length = v;
        }
        ns->seekable = (ns->content_length > 0);
        ns->icy_metaint = 0;
        if (ns_header(req, WINHTTP_QUERY_CUSTOM, L"icy-metaint",
                      tmp, sizeof(tmp))) {
            long v = atol(tmp);
            if (v > 0 && v <= 16 * 1024 * 1024) ns->icy_metaint = (uint32_t)v;
        }
        ns_header(req, WINHTTP_QUERY_CUSTOM, L"icy-name",
                  ns->station_name, sizeof(ns->station_name));
    }
    return req;
}

/* Parse StreamTitle='...' out of an ICY metadata block. */
static void ns_meta_title(mn_netstream *ns) {
    const char *k = "StreamTitle='";
    char *s, *e;
    ns->meta_buf[ns->meta_have] = 0;
    s = strstr((char *)ns->meta_buf, k);
    if (!s) return;
    s += strlen(k);
    e = strstr(s, "';");
    if (!e) e = s + strlen(s);
    EnterCriticalSection(&ns->title_cs);
    {
        size_t n = (size_t)(e - s);
        if (n >= sizeof(ns->title)) n = sizeof(ns->title) - 1;
        if (n != strlen(ns->title) || memcmp(ns->title, s, n) != 0) {
            memcpy(ns->title, s, n);
            ns->title[n] = 0;
            ns->title_seq++;
        }
    }
    LeaveCriticalSection(&ns->title_cs);
}

/* Push audio bytes into the ring; blocks while full (backpressure —
 * a paused player simply stops the download). Returns false when the
 * stream is closing or a seek flushed this generation. */
static bool ns_ring_push(mn_netstream *ns, const unsigned char *p, size_t n,
                         uint32_t gen) {
    EnterCriticalSection(&ns->cs);
    while (n > 0) {
        size_t space, chunk;
        /* A pending SEEK must abort the push: the reader thread is inside
         * mn_netstream_seek waiting for OUR ack — if we sat here waiting
         * for ring space that only that same reader could free, the two
         * threads deadlocked (engine stop's rewind-to-0 while playing hit
         * exactly this). */
        if (ns->closing || ns->generation != gen || ns->seek_to >= 0) {
            LeaveCriticalSection(&ns->cs);
            return false;
        }
        space = NS_RING_CAP - ns->count - ns->hist;
        if (space == 0 && ns->hist > 0) {
            /* Reclaim the OLDEST history first — live data always wins. */
            size_t drop = ns->hist < n ? ns->hist : n;
            ns->hist -= drop;
            space = drop;
        }
        if (space == 0) {
            SleepConditionVariableCS(&ns->cv_space, &ns->cs, 500);
            continue;
        }
        chunk = n < space ? n : space;
        {
            size_t first = NS_RING_CAP - ns->tail;
            if (first > chunk) first = chunk;
            memcpy(ns->ring + ns->tail, p, first);
            memcpy(ns->ring, p + first, chunk - first);
            ns->tail = (ns->tail + chunk) % NS_RING_CAP;
            ns->count += chunk;
        }
        p += chunk;
        n -= chunk;
        ns->fill_pos += (int64_t)chunk;
        WakeAllConditionVariable(&ns->cv_data);
    }
    LeaveCriticalSection(&ns->cs);
    return true;
}

/* Feed raw wire bytes through the ICY stripper into the ring. */
static bool ns_feed(mn_netstream *ns, const unsigned char *p, size_t n,
                    uint32_t gen) {
    if (ns->icy_metaint == 0) {
        return ns_ring_push(ns, p, n, gen);
    }
    while (n > 0) {
        if (ns->meta_need > 0) {
            /* consuming a metadata block */
            size_t take = n < ns->meta_need ? n : ns->meta_need;
            if (ns->meta_have + take <= NS_META_MAX) {
                memcpy(ns->meta_buf + ns->meta_have, p, take);
                ns->meta_have += (uint32_t)take;
            }
            ns->meta_need -= (uint32_t)take;
            p += take; n -= take;
            if (ns->meta_need == 0 && ns->meta_have > 0) {
                ns_meta_title(ns);
                ns->meta_have = 0;
            }
            continue;
        }
        if (ns->meta_countdown == 0) {
            /* length byte: meta block size = byte * 16 */
            ns->meta_need = (uint32_t)p[0] * 16u;
            ns->meta_have = 0;
            ns->meta_countdown = ns->icy_metaint;
            p += 1; n -= 1;
            continue;
        }
        {
            size_t take = n < ns->meta_countdown ? n : ns->meta_countdown;
            if (!ns_ring_push(ns, p, take, gen)) return false;
            ns->meta_countdown -= (uint32_t)take;
            p += take; n -= take;
        }
    }
    return true;
}

/* Park: no connection and nothing to do — announce EOF to readers and wait
 * for a seek (which revives the connection) or close. The fill thread must
 * NEVER exit while the stream is open: a decoder seek always has to find a
 * live servant, or the seeking thread would wait forever (that deadlock
 * held the engine lock and froze every transport poll). */
static void ns_park(mn_netstream *ns) {
    EnterCriticalSection(&ns->cs);
    ns->eof = true;
    WakeAllConditionVariable(&ns->cv_data);
    while (!ns->closing && ns->seek_to < 0) {
        SleepConditionVariableCS(&ns->cv_space, &ns->cs, 250);
    }
    LeaveCriticalSection(&ns->cs);
}

static DWORD WINAPI ns_fill_thread(LPVOID arg) {
    mn_netstream *ns = (mn_netstream *)arg;
    unsigned char *buf = (unsigned char *)malloc(NS_CHUNK);
    int retries = 0;

    for (;;) {
        DWORD    got = 0;
        BOOL     ok;
        uint32_t gen;
        int64_t  want_seek = -1;
        bool     closing;

        EnterCriticalSection(&ns->cs);
        closing   = ns->closing;
        want_seek = ns->seek_to;
        gen       = ns->generation;
        LeaveCriticalSection(&ns->cs);
        if (closing) break;

        if (want_seek >= 0) {
            /* Reader asked for a byte position: drop the connection and
             * come back with a Range request. A seek AT/PAST the end
             * (decoder size probes do this) is satisfied logically —
             * reads report EOF, no 416 Range dance with the server. */
            HINTERNET nreq = NULL;
            bool at_eof = ns->seekable && want_seek >= ns->content_length;
            { HINTERNET h = ns_take_request(ns); if (h) WinHttpCloseHandle(h); }
            if (!at_eof) nreq = ns_connect_at(ns, want_seek, false);
            EnterCriticalSection(&ns->cs);
            ns->seek_to = -1;
            ns->head = ns->tail = ns->count = 0;
            ns->hist = 0;               /* history is pre-seek data */
            ns->generation++;           /* flush anything mid-push */
            ns->fill_pos = want_seek;
            ns->read_pos = want_seek;
            ns->eof = (nreq == NULL);
            WakeAllConditionVariable(&ns->cv_data);
            LeaveCriticalSection(&ns->cs);
            ns->request = nreq;
            ns->meta_countdown = ns->icy_metaint;
            ns->meta_need = ns->meta_have = 0;
            retries = 0;
            if (!nreq) { ns_park(ns); }
            continue;
        }

        if (ns->request == NULL || !buf) { ns_park(ns); continue; }

        ok = WinHttpReadData(ns->request, buf, NS_CHUNK, &got);
        if (ok && got > 0) {
            retries = 0;
            if (!ns_feed(ns, buf, got, gen)) {
                continue;   /* seek flushed us; loop to handle it */
            }
            continue;
        }

        /* End of body or network failure. Seekable + not at the end, or
         * a live mount: try to resume/reconnect with backoff. */
        {
            bool at_real_end = ns->seekable &&
                               ns->fill_pos >= ns->content_length;
            if (!at_real_end && retries < 3) {
                DWORD backoff = 1000u << retries;   /* 1s / 2s / 4s */
                retries++;
                { HINTERNET h = ns_take_request(ns); if (h) WinHttpCloseHandle(h); }
                {   /* interruptible sleep */
                    DWORD waited = 0;
                    while (waited < backoff) {
                        bool stop;
                        EnterCriticalSection(&ns->cs);
                        stop = ns->closing || ns->seek_to >= 0;
                        LeaveCriticalSection(&ns->cs);
                        if (stop) break;
                        Sleep(100); waited += 100;
                    }
                }
                /* NEVER open a fresh connection while closing (or while a
                 * seek is pending — its handler reconnects itself): a
                 * multi-second connect here outlived the 5s join in
                 * netstream_close, which then freed this struct under us. */
                {
                    bool stop;
                    EnterCriticalSection(&ns->cs);
                    stop = ns->closing;
                    LeaveCriticalSection(&ns->cs);
                    if (stop) break;
                    EnterCriticalSection(&ns->cs);
                    stop = ns->seek_to >= 0;
                    LeaveCriticalSection(&ns->cs);
                    if (stop) continue;
                }
                ns->request = ns_connect_at(
                    ns, ns->seekable ? ns->fill_pos : 0, false);
                if (ns->request) {
                    ns->meta_countdown = ns->icy_metaint;
                    ns->meta_need = ns->meta_have = 0;
                    continue;
                }
            }
        }
        { HINTERNET h = ns_take_request(ns); if (h) WinHttpCloseHandle(h); }
        ns_park(ns);   /* true end / gave up — wait for a reviving seek */
    }

    free(buf);
    EnterCriticalSection(&ns->cs);
    ns->eof = true;
    WakeAllConditionVariable(&ns->cv_data);
    LeaveCriticalSection(&ns->cs);
    return 0;
}

/* ------------------------------------------------------------------ */

mn_netstream *mn_netstream_open(const char *url, bool want_icy,
                                char *err, size_t err_cap) {
    mn_netstream *ns;
    URL_COMPONENTS uc;
    wchar_t wurl[2048];

    if (err && err_cap) err[0] = 0;
    if (!url || strlen(url) >= sizeof(((mn_netstream *)0)->url)) {
        if (err) snprintf(err, err_cap, "bad URL");
        return NULL;
    }

    ns = (mn_netstream *)calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->ring = (unsigned char *)malloc(NS_RING_CAP);
    if (!ns->ring) { free(ns); return NULL; }

    strcpy(ns->url, url);
    ns->want_icy = want_icy;
    ns->content_length = -1;
    ns->seek_to = -1;
    InitializeCriticalSection(&ns->cs);
    InitializeCriticalSection(&ns->title_cs);
    InitializeConditionVariable(&ns->cv_data);
    InitializeConditionVariable(&ns->cv_space);

    ns_utf8_to_wide(url, wurl, 2048);
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = ns->whost; uc.dwHostNameLength = 512;
    uc.lpszUrlPath  = ns->wpath; uc.dwUrlPathLength  = 1536;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) ||
        (uc.nScheme != INTERNET_SCHEME_HTTP &&
         uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        if (err) snprintf(err, err_cap, "bad URL");
        goto fail;
    }
    ns->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    ns->port   = uc.nPort;

    ns->session = WinHttpOpen(NS_UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                              0);
    if (!ns->session) { if (err) snprintf(err, err_cap, "winhttp"); goto fail; }
    ns->connect = WinHttpConnect(ns->session, ns->whost, ns->port, 0);
    if (!ns->connect) { if (err) snprintf(err, err_cap, "connect"); goto fail; }

    ns->request = ns_connect_at(ns, 0, true);
    if (!ns->request) {
        if (err) snprintf(err, err_cap, "couldn't reach the stream");
        goto fail;
    }
    ns->meta_countdown = ns->icy_metaint;

    ns->fill_thread = CreateThread(NULL, 0, ns_fill_thread, ns, 0, NULL);
    if (!ns->fill_thread) { if (err) snprintf(err, err_cap, "thread"); goto fail; }
    return ns;

fail:
    if (ns->request) WinHttpCloseHandle(ns->request);
    if (ns->connect) WinHttpCloseHandle(ns->connect);
    if (ns->session) WinHttpCloseHandle(ns->session);
    DeleteCriticalSection(&ns->cs);
    DeleteCriticalSection(&ns->title_cs);
    free(ns->ring);
    free(ns);
    return NULL;
}

size_t mn_netstream_read(mn_netstream *ns, void *dst, size_t n) {
    size_t out = 0;
    if (!ns || !dst || n == 0) return 0;
    EnterCriticalSection(&ns->cs);
    for (;;) {
        if (ns->count > 0) {
            size_t take = n < ns->count ? n : ns->count;
            size_t first = NS_RING_CAP - ns->head;
            if (first > take) first = take;
            memcpy(dst, ns->ring + ns->head, first);
            memcpy((unsigned char *)dst + first, ns->ring, take - first);
            ns->head = (ns->head + take) % NS_RING_CAP;
            ns->count -= take;
            ns->read_pos += (int64_t)take;
            /* What was just consumed becomes rewindable history. */
            ns->hist += take;
            if (ns->hist > NS_HIST_MAX) ns->hist = NS_HIST_MAX;
            out = take;
            WakeAllConditionVariable(&ns->cv_space);
            break;
        }
        if (ns->eof || ns->closing) break;
        if (!SleepConditionVariableCS(&ns->cv_data, &ns->cs,
                                      NS_READ_WAIT_MS)) {
            break;   /* starved too long — treat as end */
        }
    }
    LeaveCriticalSection(&ns->cs);
    return out;
}

bool mn_netstream_seek(mn_netstream *ns, int64_t off) {
    if (!ns || off < 0) return false;
    if (ns->seekable && off > ns->content_length) off = ns->content_length;
    EnterCriticalSection(&ns->cs);
    /* Fast path: the target is already buffered ahead of the reader. */
    if (off >= ns->read_pos && off < ns->read_pos + (int64_t)ns->count) {
        size_t skip = (size_t)(off - ns->read_pos);
        ns->head = (ns->head + skip) % NS_RING_CAP;
        ns->count -= skip;
        ns->hist += skip;
        if (ns->hist > NS_HIST_MAX) ns->hist = NS_HIST_MAX;
        ns->read_pos = off;
        WakeAllConditionVariable(&ns->cv_space);
        LeaveCriticalSection(&ns->cs);
        return true;
    }
    /* Rewind path: the target is inside retained history — hand the bytes
     * back without touching the network. This is what makes decoder init
     * probes (read ID3 header, seek back to 0) work on LIVE mounts. */
    if (off < ns->read_pos && ns->read_pos - off <= (int64_t)ns->hist) {
        size_t back = (size_t)(ns->read_pos - off);
        ns->head = (ns->head + NS_RING_CAP - back) % NS_RING_CAP;
        ns->count += back;
        ns->hist -= back;
        ns->read_pos = off;
        LeaveCriticalSection(&ns->cs);
        return true;
    }
    if (!ns->seekable) {
        LeaveCriticalSection(&ns->cs);
        return false;
    }
    ns->seek_to = off;
    ns->eof = false;
    WakeAllConditionVariable(&ns->cv_space);   /* unblock a full push */
    LeaveCriticalSection(&ns->cs);
    /* Abort a blocking WinHttpReadData so the fill thread notices. */
    { HINTERNET h = ns_take_request(ns); if (h) WinHttpCloseHandle(h); }
    /* Wait until the fill thread acknowledged (seek_to reset). */
    for (;;) {
        bool pending;
        EnterCriticalSection(&ns->cs);
        pending = ns->seek_to >= 0 && !ns->closing;
        LeaveCriticalSection(&ns->cs);
        if (!pending) break;
        Sleep(10);
    }
    return true;
}

int64_t mn_netstream_tell(mn_netstream *ns) {
    int64_t p;
    if (!ns) return 0;
    EnterCriticalSection(&ns->cs);
    p = ns->read_pos;
    LeaveCriticalSection(&ns->cs);
    return p;
}

int64_t mn_netstream_length(mn_netstream *ns) {
    return ns ? ns->content_length : -1;
}

bool mn_netstream_seekable(mn_netstream *ns) {
    return ns ? ns->seekable : false;
}

bool mn_netstream_title(mn_netstream *ns, char *out, size_t cap,
                        uint32_t *seq) {
    bool changed = false;
    if (!ns || !out || cap == 0) return false;
    EnterCriticalSection(&ns->title_cs);
    if (seq && *seq != ns->title_seq && ns->title[0]) {
        snprintf(out, cap, "%s", ns->title);
        *seq = ns->title_seq;
        changed = true;
    }
    LeaveCriticalSection(&ns->title_cs);
    return changed;
}

const char *mn_netstream_station_name(mn_netstream *ns) {
    return ns ? ns->station_name : "";
}

const char *mn_netstream_content_type(mn_netstream *ns) {
    return ns ? ns->content_type : "";
}

size_t mn_netstream_wait_buffered(mn_netstream *ns, size_t bytes,
                                  uint32_t timeout_ms) {
    DWORD start = GetTickCount();
    size_t have = 0;
    if (!ns) return 0;
    for (;;) {
        EnterCriticalSection(&ns->cs);
        have = ns->count;
        if (have >= bytes || ns->eof || ns->closing) {
            LeaveCriticalSection(&ns->cs);
            break;
        }
        SleepConditionVariableCS(&ns->cv_data, &ns->cs, 250);
        LeaveCriticalSection(&ns->cs);
        if (GetTickCount() - start >= timeout_ms) break;
    }
    return have;
}

void mn_netstream_close(mn_netstream *ns) {
    if (!ns) return;
    EnterCriticalSection(&ns->cs);
    ns->closing = true;
    WakeAllConditionVariable(&ns->cv_data);
    WakeAllConditionVariable(&ns->cv_space);
    LeaveCriticalSection(&ns->cs);
    { HINTERNET h = ns_take_request(ns); if (h) WinHttpCloseHandle(h); }
    if (ns->fill_thread) {
        /* The fill thread is guaranteed to exit promptly now (aborted read,
         * interruptible backoff, closing-checked reconnect, 250ms park
         * poll). If it somehow doesn't, LEAK this struct rather than free
         * a critical section under a live thread — that use-after-free
         * froze the whole app. */
        if (WaitForSingleObject(ns->fill_thread, 15000) != WAIT_OBJECT_0) {
            CloseHandle(ns->fill_thread);
            return;   /* intentional leak; unreachable in practice */
        }
        CloseHandle(ns->fill_thread);
    }
    if (ns->connect) WinHttpCloseHandle(ns->connect);
    if (ns->session) WinHttpCloseHandle(ns->session);
    DeleteCriticalSection(&ns->cs);
    DeleteCriticalSection(&ns->title_cs);
    free(ns->ring);
    free(ns);
}
