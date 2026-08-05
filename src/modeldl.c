/*
 * modeldl.c — Monatomic Audio Player
 *
 * Hugging Face single-file downloader over WinHTTP.
 *
 * URL scheme: https://huggingface.co/<repo>/resolve/main/<file>. The resolve
 * endpoint answers with a 302 to the LFS CDN (cdn-lfs*.huggingface.co) for
 * large weights, or streams the file directly for small ones. We let WinHTTP
 * follow redirects automatically (WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS),
 * which keeps us on HTTPS across the hop and re-issues the request to the CDN
 * host transparently — no manual Location parsing needed.
 *
 * The <file> may contain forward-slash subfolders (e.g. "onnx/model.onnx"),
 * which many HF ONNX exports use. The full sub-path is sent to the resolve
 * endpoint, but the file is saved LOCALLY under its basename only ("model.onnx")
 * so the flat ai-models/ load path finds it. Backslashes and ".." are rejected.
 *
 * Streaming: the body is read in chunks into a "<dest>/<base>.part" temp
 * file, then MoveFileEx-renamed onto the final path when the transfer
 * completes. Progress is pushed to the caller's callback on the worker thread.
 *
 * Concurrency: a single global slot guarded by a mutex. A start while busy is
 * rejected.
 */
#include "modeldl.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Global single-slot state.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char          repo[256];
    char          file[256];
    char          save_as[256];   /* local basename override; "" = basename(file) */
    char          dest_dir[1024];
    mn_modeldl_cb cb;
    void         *user;
} mn_dl_job;

static CRITICAL_SECTION g_dl_lock;
static volatile LONG    g_dl_lock_init = 0;
static volatile LONG    g_dl_active    = 0;   /* 0/1 busy flag */

static void ensure_lock(void) {
    /* One-time init; benign double-init avoided via interlocked guard. */
    if (InterlockedCompareExchange(&g_dl_lock_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_dl_lock);
        InterlockedExchange(&g_dl_lock_init, 2);
    }
    while (InterlockedCompareExchange(&g_dl_lock_init, 2, 2) != 2)
        Sleep(0);
}

/* ------------------------------------------------------------------ */
/* UTF-8 -> UTF-16 for WinHTTP wide APIs.                             */
/* ------------------------------------------------------------------ */

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* ------------------------------------------------------------------ */
/* Worker.                                                            */
/* ------------------------------------------------------------------ */

static void fail(mn_dl_job *j, const char *msg) {
    if (j->cb) j->cb(j->user, 0, 0, true, msg);
}

static DWORD WINAPI dl_worker(LPVOID param) {
    mn_dl_job *j = (mn_dl_job *)param;

    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    wchar_t   wpath[1200];
    wchar_t  *whost = NULL;
    FILE     *fp = NULL;
    char      part_path[1200];
    char      final_path[1100];
    char     *buf = NULL;
    int64_t   total = 0, got = 0, last_report = 0;
    bool      ok = false;
    const char *errmsg = "download failed";

    /* URL-path: /<repo>/resolve/main/<file>. Both repo and file may contain
     * '/', a valid URL path separator (file was validated: no '\\', no ".."). */
    {
        char apath[900];
        snprintf(apath, sizeof(apath), "/%s/resolve/main/%s", j->repo, j->file);
        int n = MultiByteToWideChar(CP_UTF8, 0, apath, -1, wpath,
                                    (int)(sizeof(wpath) / sizeof(wpath[0])));
        if (n <= 0) { errmsg = "bad url"; goto done; }
    }

    /* Local file: the caller's save_as override when given (disambiguates
     * repos that all store their model at "onnx/model.onnx"), otherwise the
     * basename of <file> so subpaths land flat in ai-models/. */
    {
        const char *base;
        if (j->save_as[0]) {
            base = j->save_as;
        } else {
            base = strrchr(j->file, '/');
            base = base ? base + 1 : j->file;
        }
        snprintf(final_path, sizeof(final_path), "%s\\%s", j->dest_dir, base);
        snprintf(part_path,  sizeof(part_path),  "%s\\%s.part", j->dest_dir, base);
    }

    /* Ensure the destination directory exists (best-effort, one level). */
    {
        wchar_t *wdir = utf8_to_wide(j->dest_dir);
        if (wdir) { CreateDirectoryW(wdir, NULL); free(wdir); }
    }

    hSession = WinHttpOpen(L"Monatomic/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { errmsg = "winhttp open failed"; goto done; }

    /* Follow all redirects, including the 302 to the LFS CDN, and permit the
     * https->https cross-host hop that HF uses. */
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY,
                         &policy, sizeof(policy));
    }

    whost = utf8_to_wide("huggingface.co");
    if (!whost) { errmsg = "oom"; goto done; }

    hConnect = WinHttpConnect(hSession, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { errmsg = "connect failed"; goto done; }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath, NULL,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE);
    if (!hRequest) { errmsg = "open request failed"; goto done; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        errmsg = "send failed"; goto done;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        errmsg = "no response"; goto done;
    }

    /* HTTP status. */
    {
        DWORD status = 0, sz = sizeof(status);
        if (WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                WINHTTP_NO_HEADER_INDEX)) {
            if (status != 200) {
                static char sbuf[64];
                snprintf(sbuf, sizeof(sbuf), "http %lu", (unsigned long)status);
                errmsg = sbuf;
                goto done;
            }
        }
    }

    /* Content-Length (best effort; 0 => unknown, progress still streams). */
    {
        DWORD len = 0, sz = sizeof(len);
        if (WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz,
                WINHTTP_NO_HEADER_INDEX)) {
            total = (int64_t)len;
        }
    }

    fp = fopen(part_path, "wb");
    if (!fp) { errmsg = "cannot open temp file"; goto done; }

    buf = (char *)malloc(64 * 1024);
    if (!buf) { errmsg = "oom"; goto done; }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            errmsg = "read stalled"; goto done;
        }
        if (avail == 0) break;               /* end of body */
        while (avail > 0) {
            DWORD want = avail > (64 * 1024) ? (64 * 1024) : avail;
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf, want, &read)) {
                errmsg = "read failed"; goto done;
            }
            if (read == 0) { avail = 0; break; }
            if (fwrite(buf, 1, read, fp) != read) {
                errmsg = "disk write failed"; goto done;
            }
            got   += (int64_t)read;
            avail -= read;
        }
        /* Report progress at most every 256 KiB to avoid callback spam. */
        if (got - last_report >= 256 * 1024) {
            last_report = got;
            if (j->cb) j->cb(j->user, got, total, false, NULL);
        }
    }

    fclose(fp); fp = NULL;

    /* Atomic-ish rename into place (overwrite any stale copy). */
    {
        remove(final_path);   /* MoveFileEx REPLACE_EXISTING handles this too */
        if (MoveFileExA(part_path, final_path, MOVEFILE_REPLACE_EXISTING)) {
            ok = true;
        } else {
            errmsg = "rename failed"; goto done;
        }
    }

done:
    if (fp)       fclose(fp);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    free(buf);
    free(whost);

    if (ok) {
        if (j->cb) j->cb(j->user, got, total > 0 ? total : got, true, NULL);
    } else {
        remove(part_path);   /* clean up the partial */
        fail(j, errmsg);
    }

    free(j);
    InterlockedExchange(&g_dl_active, 0);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

bool mn_modeldl_busy(void) {
    return InterlockedCompareExchange(&g_dl_active, 0, 0) != 0;
}

bool mn_modeldl_start(const char *repo, const char *file,
                      const char *save_as,
                      const char *dest_dir,
                      mn_modeldl_cb cb, void *user) {
    if (!repo || !file || !dest_dir || !repo[0] || !file[0] || !dest_dir[0])
        return false;
    /* Path-traversal guard. Forward-slash subfolders are allowed (HF ONNX
     * exports commonly live at "onnx/model.onnx"); backslashes and ".."
     * segments are not — they could escape the destination directory. The
     * URL uses the full sub-path; the local file is saved by basename only. */
    if (strchr(file, '\\') || strstr(file, ".."))
        return false;
    if (file[0] == '/')
        return false;   /* must be repo-relative, not absolute */
    /* save_as must be a bare filename (it names the LOCAL file). */
    if (save_as && save_as[0] &&
        (strchr(save_as, '\\') || strchr(save_as, '/') || strstr(save_as, "..")))
        return false;

    ensure_lock();
    EnterCriticalSection(&g_dl_lock);
    if (InterlockedCompareExchange(&g_dl_active, 1, 0) != 0) {
        LeaveCriticalSection(&g_dl_lock);
        return false;   /* busy */
    }
    LeaveCriticalSection(&g_dl_lock);

    mn_dl_job *j = (mn_dl_job *)calloc(1, sizeof(*j));
    if (!j) { InterlockedExchange(&g_dl_active, 0); return false; }
    snprintf(j->repo,     sizeof(j->repo),     "%s", repo);
    snprintf(j->file,     sizeof(j->file),     "%s", file);
    if (save_as && save_as[0])
        snprintf(j->save_as, sizeof(j->save_as), "%s", save_as);
    snprintf(j->dest_dir, sizeof(j->dest_dir), "%s", dest_dir);
    j->cb   = cb;
    j->user = user;

    HANDLE h = CreateThread(NULL, 0, dl_worker, j, 0, NULL);
    if (!h) { free(j); InterlockedExchange(&g_dl_active, 0); return false; }
    CloseHandle(h);
    return true;
}

#else /* !_WIN32 — WinHTTP unavailable; downloader is a no-op stub for now. */

bool mn_modeldl_busy(void) { return false; }

bool mn_modeldl_start(const char *repo, const char *file,
                      const char *save_as,
                      const char *dest_dir,
                      mn_modeldl_cb cb, void *user) {
    (void)repo; (void)file; (void)save_as; (void)dest_dir; (void)cb; (void)user;
    return false;   /* not implemented off-Windows yet */
}

#endif /* _WIN32 */
