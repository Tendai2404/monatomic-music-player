/*
 * scanner.c -- Monatomic Audio Player
 *
 * Implementation of the multi-threaded, incremental filesystem scanner
 * declared in scanner.h.
 *
 * Architecture
 * ------------
 * The scan is split into two roles that run on a single shared pool of
 * worker threads:
 *
 *   1. Directory walking. A work queue of pending directories is seeded
 *      with the configured roots. Any idle worker pops a directory,
 *      enumerates it, pushes discovered subdirectories back onto the
 *      queue, and hands each candidate audio file to the tag stage.
 *
 *   2. Tag reading. For each candidate file the worker (optionally)
 *      consults is_known, then reads its tags via mn_tags_read() and
 *      reports the result through on_track.
 *
 * Because directories are processed on the same pool that reads tags, a
 * single N-thread pool saturates both I/O-bound walking and CPU-bound tag
 * parsing without a separate producer/consumer split. Threads block on a
 * condition variable when the queue is momentarily empty and wake when new
 * directories are pushed; the run ends when the queue is empty AND no
 * worker is still busy (so no worker can produce more work).
 *
 * All progress counters are updated atomically. Cancellation is a single
 * atomic flag observed at every loop boundary.
 *
 * Platform abstraction
 * --------------------
 * Threads, mutexes and condition variables are wrapped in a tiny internal
 * layer (mn_thread / mn_mutex / mn_cond) implemented with Win32 primitives
 * on Windows and pthreads elsewhere. Directory enumeration uses
 * FindFirstFileW/FindNextFileW on Windows (wide, long-path aware) and
 * opendir/readdir on POSIX. All paths handed to callbacks are UTF-8.
 */

#include "scanner.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Platform primitives
 * ---------------------------------------------------------------------- */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef CRITICAL_SECTION       mn_mutex;
typedef CONDITION_VARIABLE     mn_cond;
typedef HANDLE                 mn_thread;

static void mn_mutex_init(mn_mutex *m)    { InitializeCriticalSection(m); }
static void mn_mutex_destroy(mn_mutex *m) { DeleteCriticalSection(m); }
static void mn_mutex_lock(mn_mutex *m)    { EnterCriticalSection(m); }
static void mn_mutex_unlock(mn_mutex *m)  { LeaveCriticalSection(m); }

static void mn_cond_init(mn_cond *c)      { InitializeConditionVariable(c); }
static void mn_cond_destroy(mn_cond *c)   { (void)c; /* nothing to do */ }
static void mn_cond_wait(mn_cond *c, mn_mutex *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
static void mn_cond_signal(mn_cond *c)    { WakeConditionVariable(c); }
static void mn_cond_broadcast(mn_cond *c) { WakeAllConditionVariable(c); }

#else /* POSIX */

#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

typedef pthread_mutex_t mn_mutex;
typedef pthread_cond_t  mn_cond;
typedef pthread_t       mn_thread;

static void mn_mutex_init(mn_mutex *m)    { pthread_mutex_init(m, NULL); }
static void mn_mutex_destroy(mn_mutex *m) { pthread_mutex_destroy(m); }
static void mn_mutex_lock(mn_mutex *m)    { pthread_mutex_lock(m); }
static void mn_mutex_unlock(mn_mutex *m)  { pthread_mutex_unlock(m); }

static void mn_cond_init(mn_cond *c)      { pthread_cond_init(c, NULL); }
static void mn_cond_destroy(mn_cond *c)   { pthread_cond_destroy(c); }
static void mn_cond_wait(mn_cond *c, mn_mutex *m) { pthread_cond_wait(c, m); }
static void mn_cond_signal(mn_cond *c)    { pthread_cond_signal(c); }
static void mn_cond_broadcast(mn_cond *c) { pthread_cond_broadcast(c); }

#endif /* _WIN32 */

/* -------------------------------------------------------------------------
 * Atomic counters
 *
 * All counters live behind the queue mutex except the progress totals,
 * which are hit from many threads without holding that lock. We use
 * lightweight platform atomics (interlocked / __atomic) for those so that
 * mn_scanner_progress() can snapshot without contending on the work lock.
 * ---------------------------------------------------------------------- */

#ifdef _WIN32
typedef volatile LONG64 mn_atomic_u64;
typedef volatile LONG   mn_atomic_flag;

static void mn_atomic_add_u64(mn_atomic_u64 *v, uint64_t n) {
    InterlockedAdd64(v, (LONG64)n);
}
static uint64_t mn_atomic_load_u64(const mn_atomic_u64 *v) {
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)v, 0, 0);
}
static void mn_atomic_store_flag(mn_atomic_flag *v, int n) {
    InterlockedExchange(v, (LONG)n);
}
static int mn_atomic_load_flag(const mn_atomic_flag *v) {
    return (int)InterlockedCompareExchange((volatile LONG *)v, 0, 0);
}
#else
typedef _Atomic uint64_t mn_atomic_u64;
typedef _Atomic int      mn_atomic_flag;

static void mn_atomic_add_u64(mn_atomic_u64 *v, uint64_t n) {
    __atomic_fetch_add(v, n, __ATOMIC_RELAXED);
}
static uint64_t mn_atomic_load_u64(const mn_atomic_u64 *v) {
    return __atomic_load_n(v, __ATOMIC_RELAXED);
}
static void mn_atomic_store_flag(mn_atomic_flag *v, int n) {
    __atomic_store_n(v, n, __ATOMIC_RELAXED);
}
static int mn_atomic_load_flag(const mn_atomic_flag *v) {
    return __atomic_load_n(v, __ATOMIC_RELAXED);
}
#endif

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* Case-insensitive ASCII compare of a NUL-terminated extension (without the
 * leading dot) against a lowercase literal. Returns nonzero on match. */
static int mn_ext_eq(const char *ext, const char *lower_lit) {
    while (*ext && *lower_lit) {
        char c = *ext;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *lower_lit) return 0;
        ++ext;
        ++lower_lit;
    }
    return *ext == '\0' && *lower_lit == '\0';
}

/*
 * Recognized audio extensions. Kept as a static table so the set is easy to
 * audit and extend. Matched case-insensitively against the file's extension.
 */
static const char *const MN_AUDIO_EXTS[] = {
    "mp3", "flac", "wav", "m4a", "aac", "ogg",
    "opus", "wma", "aiff", "aif", "ape", "wv",
};
static const size_t MN_AUDIO_EXT_COUNT =
    sizeof(MN_AUDIO_EXTS) / sizeof(MN_AUDIO_EXTS[0]);

/* Return the extension (past the last '.') of a UTF-8 path, or NULL if the
 * final path component has no dot. Does not include the dot. */
static const char *mn_path_ext(const char *path) {
    const char *dot = NULL;
    const char *p;
    for (p = path; *p; ++p) {
        if (*p == '.') {
            dot = p;
        } else if (*p == '/' || *p == '\\') {
            dot = NULL; /* dots in parent dirs don't count */
        }
    }
    return dot ? dot + 1 : NULL;
}

/* True if the path's extension is a recognized audio type. */
static int mn_is_audio_path(const char *path) {
    const char *ext = mn_path_ext(path);
    size_t i;
    if (!ext || *ext == '\0') return 0;
    for (i = 0; i < MN_AUDIO_EXT_COUNT; ++i) {
        if (mn_ext_eq(ext, MN_AUDIO_EXTS[i])) return 1;
    }
    return 0;
}

/* Duplicate a NUL-terminated string; returns NULL on OOM or NULL input. */
static char *mn_strdup(const char *s) {
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Number of logical CPUs, clamped to at least 1. */
static unsigned mn_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (unsigned)n : 1u;
#endif
}

/* -------------------------------------------------------------------------
 * UTF-8 / UTF-16 conversion (Windows only)
 * ---------------------------------------------------------------------- */

#ifdef _WIN32
/* Convert UTF-8 -> newly-allocated wide string. Returns NULL on error. */
static wchar_t *mn_utf8_to_wide(const char *s) {
    int wlen;
    wchar_t *w;
    if (!s) return NULL;
    wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* Convert wide string -> newly-allocated UTF-8. Returns NULL on error. */
static char *mn_wide_to_utf8(const wchar_t *w) {
    int len;
    char *s;
    if (!w) return NULL;
    len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    s = (char *)malloc((size_t)len);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}
#endif /* _WIN32 */

/* -------------------------------------------------------------------------
 * Directory work queue
 *
 * A simple singly-linked LIFO stack of directory paths (UTF-8). Access is
 * guarded by the scanner's mutex. LIFO gives depth-first traversal, which
 * keeps the queue small even for deep trees.
 * ---------------------------------------------------------------------- */

typedef struct mn_dir_node {
    struct mn_dir_node *next;
    char *path; /* owned, UTF-8, no trailing separator */
} mn_dir_node;

/* -------------------------------------------------------------------------
 * Scanner instance
 * ---------------------------------------------------------------------- */

struct mn_scanner {
    /* Immutable configuration snapshot (deep-copied from the caller). */
    char **roots;
    size_t root_count;
    mn_scanner_on_track_fn on_track;
    mn_scanner_is_known_fn is_known;
    void *user;
    unsigned thread_count;

    /* Worker threads. */
    mn_thread *threads;
    unsigned threads_started;

    /* Synchronization. */
    mn_mutex lock;   /* guards the queue + busy_count + queue-empty waits */
    mn_cond  cv;     /* signalled when work appears or the run should end */

    /* Directory work queue (LIFO). Guarded by lock. */
    mn_dir_node *queue_head;

    /* Number of workers currently processing an item (guarded by lock).
     * The scan is complete when the queue is empty and busy_count == 0. */
    unsigned busy_count;

    /* Lifecycle flags. */
    int started;   /* set once; start() is single-shot */
    int joined;    /* set once threads have been joined */

    /* Atomic control + progress. */
    mn_atomic_flag cancel_flag;
    mn_atomic_flag walking;   /* 1 while any directory work remains */
    mn_atomic_flag finished;  /* 1 once all workers have exited */

    mn_atomic_u64 dirs_scanned;
    mn_atomic_u64 files_found;
    mn_atomic_u64 files_processed;
    mn_atomic_u64 files_skipped;
    mn_atomic_u64 tag_errors;
    mn_atomic_u64 io_errors;
};

/* -------------------------------------------------------------------------
 * Queue operations (caller must hold scanner->lock)
 * ---------------------------------------------------------------------- */

/* Push a directory path (takes ownership of `path`) onto the LIFO queue.
 * On allocation failure the path is freed and 0 is returned. */
static int mn_queue_push_locked(mn_scanner *s, char *path) {
    mn_dir_node *n = (mn_dir_node *)malloc(sizeof(*n));
    if (!n) {
        free(path);
        return 0;
    }
    n->path = path;
    n->next = s->queue_head;
    s->queue_head = n;
    return 1;
}

/* Pop the top directory path; returns an owned string or NULL if empty. */
static char *mn_queue_pop_locked(mn_scanner *s) {
    mn_dir_node *n = s->queue_head;
    char *path;
    if (!n) return NULL;
    s->queue_head = n->next;
    path = n->path;
    free(n);
    return path;
}

/* -------------------------------------------------------------------------
 * File processing: is_known -> read tags -> on_track
 * ---------------------------------------------------------------------- */

static void mn_process_file(mn_scanner *s,
                            const char *path,
                            int64_t mtime,
                            uint64_t size) {
    mn_tags tags;

    if (mn_atomic_load_flag(&s->cancel_flag)) return;

    mn_atomic_add_u64(&s->files_found, 1);

    /* Incremental skip. */
    if (s->is_known && s->is_known(s->user, path, mtime, size)) {
        mn_atomic_add_u64(&s->files_skipped, 1);
        return;
    }

    memset(&tags, 0, sizeof(tags));
    if (!mn_tags_read(path, &tags)) {
        /* Container we can't parse tags from (WMA/AIFF/APE/WV) or a
         * malformed file: STILL index it — playback goes through Media
         * Foundation which decodes more than the tag layer parses, and
         * the app's filename inference recovers title/artist/album.
         * (Dropping these made whole formats silently vanish.) */
        mn_atomic_add_u64(&s->tag_errors, 1);
        memset(&tags, 0, sizeof(tags));
    }

    if (mn_atomic_load_flag(&s->cancel_flag)) return;

    s->on_track(s->user, path, &tags);
    mn_atomic_add_u64(&s->files_processed, 1);
}

/* -------------------------------------------------------------------------
 * Directory enumeration (platform-specific)
 *
 * Enumerates `dir`, calling mn_process_file() for each audio file and
 * pushing each subdirectory back onto the work queue. Increments
 * dirs_scanned; increments io_errors on enumeration failure.
 * ---------------------------------------------------------------------- */

#ifdef _WIN32

static void mn_scan_dir(mn_scanner *s, const char *dir) {
    /* Build "<dir>\*" as a wide search pattern with the long-path prefix so
     * that paths longer than MAX_PATH are handled. */
    wchar_t *wdir = mn_utf8_to_wide(dir);
    wchar_t *pattern = NULL;
    size_t wlen, plen;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    static const wchar_t kPrefix[] = L"\\\\?\\";

    if (!wdir) {
        mn_atomic_add_u64(&s->io_errors, 1);
        return;
    }

    wlen = wcslen(wdir);
    /* prefix + dir + "\*" + NUL. Only add the \\?\ prefix for absolute,
     * non-UNC paths that are not already prefixed. */
    {
        int add_prefix = 0;
        if (wlen >= 2 && wdir[1] == L':' && wcsncmp(wdir, kPrefix, 4) != 0) {
            add_prefix = 1;
        }
        plen = (add_prefix ? 4 : 0) + wlen + 3;
        pattern = (wchar_t *)malloc(plen * sizeof(wchar_t));
        if (!pattern) {
            free(wdir);
            mn_atomic_add_u64(&s->io_errors, 1);
            return;
        }
        pattern[0] = L'\0';
        if (add_prefix) wcscpy(pattern, kPrefix);
        wcscat(pattern, wdir);
        wcscat(pattern, L"\\*");
    }

    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        free(wdir);
        mn_atomic_add_u64(&s->io_errors, 1);
        return;
    }

    mn_atomic_add_u64(&s->dirs_scanned, 1);

    /* RECENCY-FIRST: gather this directory's entries, then process files and
     * enqueue subdirs newest-modified first. So a rescan surfaces the folders
     * and files you touched most recently before churning through the archive.
     * (Collected as UTF-8 child path + mtime + size; small per-directory
     * allocation, freed before we return.) */
    {
        struct ent { char *path; int64_t mtime; uint64_t size; };
        struct ent *files = NULL, *dirs = NULL;
        size_t nf = 0, cf = 0, nd = 0, cd = 0, i;
        size_t dlen = wcslen(wdir);

        do {
            const wchar_t *name = fd.cFileName;
            wchar_t *wchild;
            char *child;
            size_t nlen;
            int64_t mtime;
            uint64_t size;
            ULARGE_INTEGER li;

            if (mn_atomic_load_flag(&s->cancel_flag)) break;
            if (name[0] == L'.' &&
                (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
                continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue;

            nlen = wcslen(name);
            wchild = (wchar_t *)malloc((dlen + 1 + nlen + 1) * sizeof(wchar_t));
            if (!wchild) { mn_atomic_add_u64(&s->io_errors, 1); continue; }
            wcscpy(wchild, wdir);
            wchild[dlen] = L'\\';
            wcscpy(wchild + dlen + 1, name);
            child = mn_wide_to_utf8(wchild);
            free(wchild);
            if (!child) { mn_atomic_add_u64(&s->io_errors, 1); continue; }

            li.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            li.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            mtime = (int64_t)(li.QuadPart / 10000000ULL) - 11644473600LL;
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            size = (uint64_t)li.QuadPart;

            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (nd == cd) {
                    size_t ncd = cd ? cd * 2 : 16;
                    struct ent *n = (struct ent *)realloc(dirs, ncd * sizeof(*n));
                    if (!n) { free(child); mn_atomic_add_u64(&s->io_errors, 1); continue; }
                    dirs = n; cd = ncd;
                }
                dirs[nd].path = child; dirs[nd].mtime = mtime; dirs[nd].size = 0; nd++;
            } else if (mn_is_audio_path(child)) {
                if (nf == cf) {
                    size_t ncf = cf ? cf * 2 : 32;
                    struct ent *n = (struct ent *)realloc(files, ncf * sizeof(*n));
                    if (!n) { free(child); mn_atomic_add_u64(&s->io_errors, 1); continue; }
                    files = n; cf = ncf;
                }
                files[nf].path = child; files[nf].mtime = mtime; files[nf].size = size; nf++;
            } else {
                free(child);
            }
        } while (FindNextFileW(h, &fd) != 0);
        FindClose(h);
        free(wdir);

        /* newest-first sort (insertion sort — directory fan-outs are small) */
        for (i = 1; i < nf; ++i) {
            struct ent k = files[i]; size_t j = i;
            while (j > 0 && files[j-1].mtime < k.mtime) { files[j] = files[j-1]; j--; }
            files[j] = k;
        }
        for (i = 1; i < nd; ++i) {
            struct ent k = dirs[i]; size_t j = i;
            while (j > 0 && dirs[j-1].mtime < k.mtime) { dirs[j] = dirs[j-1]; j--; }
            dirs[j] = k;
        }

        /* process files newest-first */
        for (i = 0; i < nf && !mn_atomic_load_flag(&s->cancel_flag); ++i) {
            mn_process_file(s, files[i].path, files[i].mtime, files[i].size);
            free(files[i].path);
        }
        for (; i < nf; ++i) free(files[i].path);   /* cancelled: free rest */
        free(files);

        /* push subdirs OLDEST-first so the LIFO stack pops NEWEST first */
        for (i = nd; i > 0; --i) {
            char *dp = dirs[i-1].path;
            mn_mutex_lock(&s->lock);
            if (!mn_queue_push_locked(s, dp)) {
                mn_mutex_unlock(&s->lock);
                mn_atomic_add_u64(&s->io_errors, 1);
            } else {
                mn_cond_signal(&s->cv);
                mn_mutex_unlock(&s->lock);
            }
        }
        free(dirs);
    }
}

#else /* POSIX */

static void mn_scan_dir(mn_scanner *s, const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    size_t dlen;

    if (!d) {
        mn_atomic_add_u64(&s->io_errors, 1);
        return;
    }

    mn_atomic_add_u64(&s->dirs_scanned, 1);
    dlen = strlen(dir);

    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        char *child;
        size_t nlen;
        struct stat st;
        int need_sep;

        if (mn_atomic_load_flag(&s->cancel_flag)) break;

        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }

        nlen = strlen(name);
        need_sep = (dlen > 0 && dir[dlen - 1] != '/') ? 1 : 0;
        child = (char *)malloc(dlen + (size_t)need_sep + nlen + 1);
        if (!child) {
            mn_atomic_add_u64(&s->io_errors, 1);
            continue;
        }
        memcpy(child, dir, dlen);
        if (need_sep) child[dlen] = '/';
        memcpy(child + dlen + (size_t)need_sep, name, nlen + 1);

        /* lstat so we do not follow symlinks (avoids cycles). */
        if (lstat(child, &st) != 0) {
            mn_atomic_add_u64(&s->io_errors, 1);
            free(child);
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            free(child);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            mn_mutex_lock(&s->lock);
            if (!mn_queue_push_locked(s, child)) {
                mn_mutex_unlock(&s->lock);
                mn_atomic_add_u64(&s->io_errors, 1);
            } else {
                mn_cond_signal(&s->cv);
                mn_mutex_unlock(&s->lock);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (mn_is_audio_path(child)) {
                mn_process_file(s, child,
                                (int64_t)st.st_mtime,
                                (uint64_t)st.st_size);
            }
            free(child);
        } else {
            free(child);
        }
    }

    closedir(d);
}

#endif /* _WIN32 */

/* -------------------------------------------------------------------------
 * Worker thread
 *
 * Loop: wait for a directory, mark ourselves busy, scan it, mark idle. The
 * run ends when the queue is empty and no worker is busy, or on cancel.
 * ---------------------------------------------------------------------- */

#ifdef _WIN32
static DWORD WINAPI mn_worker_main(LPVOID arg)
#else
static void *mn_worker_main(void *arg)
#endif
{
    mn_scanner *s = (mn_scanner *)arg;

    for (;;) {
        char *dir = NULL;

        mn_mutex_lock(&s->lock);

        /* Wait until there is work, or the run is over. */
        while (s->queue_head == NULL &&
               s->busy_count > 0 &&
               !mn_atomic_load_flag(&s->cancel_flag)) {
            mn_cond_wait(&s->cv, &s->lock);
        }

        if (mn_atomic_load_flag(&s->cancel_flag)) {
            mn_mutex_unlock(&s->lock);
            break;
        }

        dir = mn_queue_pop_locked(s);
        if (!dir) {
            /* Queue empty and no worker busy => all work is done. Wake the
             * other idle workers so they can observe termination too, and mark
             * the run finished so pollers (mn_scanner_progress) see completion
             * without needing an explicit mn_scanner_wait() join. */
            if (s->busy_count == 0) {
                mn_atomic_store_flag(&s->walking, 0);
                mn_atomic_store_flag(&s->finished, 1);
                mn_cond_broadcast(&s->cv);
                mn_mutex_unlock(&s->lock);
                break;
            }
            /* Spurious wake or transient emptiness: loop and wait again. */
            mn_mutex_unlock(&s->lock);
            continue;
        }

        s->busy_count++;
        mn_mutex_unlock(&s->lock);

        mn_scan_dir(s, dir);
        free(dir);

        mn_mutex_lock(&s->lock);
        s->busy_count--;
        /* If we just became idle and drained the queue, wake everyone so
         * they can re-evaluate the termination condition. */
        if (s->busy_count == 0 && s->queue_head == NULL) {
            mn_cond_broadcast(&s->cv);
        }
        mn_mutex_unlock(&s->lock);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* -------------------------------------------------------------------------
 * Thread create / join wrappers
 * ---------------------------------------------------------------------- */

static int mn_thread_create(mn_thread *t, mn_scanner *s) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, mn_worker_main, s, 0, NULL);
    if (!h) return 0;
    *t = h;
    return 1;
#else
    return pthread_create(t, NULL, mn_worker_main, s) == 0;
#endif
}

static void mn_thread_join(mn_thread t) {
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

mn_scanner *mn_scanner_create(const mn_scanner_config *config) {
    mn_scanner *s;
    size_t i;

    /* Validate configuration. */
    if (!config) return NULL;
    if (!config->roots || config->root_count == 0) return NULL;
    if (!config->on_track) return NULL;
    for (i = 0; i < config->root_count; ++i) {
        if (!config->roots[i]) return NULL;
    }

    s = (mn_scanner *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* Deep-copy the roots array and its strings. */
    s->roots = (char **)calloc(config->root_count, sizeof(char *));
    if (!s->roots) {
        free(s);
        return NULL;
    }
    s->root_count = config->root_count;
    for (i = 0; i < config->root_count; ++i) {
        s->roots[i] = mn_strdup(config->roots[i]);
        if (!s->roots[i]) {
            size_t j;
            for (j = 0; j < i; ++j) free(s->roots[j]);
            free(s->roots);
            free(s);
            return NULL;
        }
    }

    s->on_track = config->on_track;
    s->is_known = config->is_known;
    s->user = config->user;
    s->thread_count =
        config->thread_count ? config->thread_count : mn_cpu_count();
    if (s->thread_count == 0) s->thread_count = 1;

    mn_mutex_init(&s->lock);
    mn_cond_init(&s->cv);

    s->queue_head = NULL;
    s->busy_count = 0;
    s->started = 0;
    s->joined = 0;

    mn_atomic_store_flag(&s->cancel_flag, 0);
    mn_atomic_store_flag(&s->walking, 0);
    mn_atomic_store_flag(&s->finished, 0);

    return s;
}

bool mn_scanner_start(mn_scanner *scanner) {
    unsigned i;
    mn_scanner *s = scanner;

    if (!s) return false;

    mn_mutex_lock(&s->lock);
    if (s->started) {
        mn_mutex_unlock(&s->lock);
        return false;
    }
    s->started = 1;

    /* Seed the queue with the root directories. We treat the roots as the
     * initial "busy" work so that workers do not immediately conclude the
     * queue is permanently empty before enumeration begins: busy_count is
     * bumped by one placeholder that we drop right after seeding. */
    s->busy_count = 1; /* placeholder keeps workers alive during seeding */
    for (i = 0; i < (unsigned)s->root_count; ++i) {
        char *copy = mn_strdup(s->roots[i]);
        if (copy) {
            if (!mn_queue_push_locked(s, copy)) {
                mn_atomic_add_u64(&s->io_errors, 1);
            }
        } else {
            mn_atomic_add_u64(&s->io_errors, 1);
        }
    }
    mn_atomic_store_flag(&s->walking, 1);
    mn_mutex_unlock(&s->lock);

    /* Allocate the thread handle array. */
    s->threads = (mn_thread *)calloc(s->thread_count, sizeof(mn_thread));
    if (!s->threads) {
        /* Undo the started state so destroy() cleans up gracefully. */
        mn_mutex_lock(&s->lock);
        s->busy_count = 0;
        mn_mutex_unlock(&s->lock);
        mn_atomic_store_flag(&s->walking, 0);
        mn_atomic_store_flag(&s->finished, 1);
        return false;
    }

    /* Spawn workers. */
    s->threads_started = 0;
    for (i = 0; i < s->thread_count; ++i) {
        if (!mn_thread_create(&s->threads[i], s)) {
            break;
        }
        s->threads_started++;
    }

    if (s->threads_started == 0) {
        /* Could not create any worker; nothing will drain the queue. */
        mn_mutex_lock(&s->lock);
        s->busy_count = 0;
        mn_mutex_unlock(&s->lock);
        mn_atomic_store_flag(&s->walking, 0);
        mn_atomic_store_flag(&s->finished, 1);
        return false;
    }

    /* Drop the seeding placeholder now that real work is queued and workers
     * are live, then wake them to begin draining. */
    mn_mutex_lock(&s->lock);
    if (s->busy_count > 0) s->busy_count--;
    mn_cond_broadcast(&s->cv);
    mn_mutex_unlock(&s->lock);

    return true;
}

void mn_scanner_wait(mn_scanner *scanner) {
    mn_scanner *s = scanner;
    unsigned i;

    if (!s) return;

    mn_mutex_lock(&s->lock);
    if (!s->started || s->joined) {
        mn_mutex_unlock(&s->lock);
        return;
    }
    mn_mutex_unlock(&s->lock);

    /* Join every worker that was actually created. */
    for (i = 0; i < s->threads_started; ++i) {
        mn_thread_join(s->threads[i]);
    }

    mn_mutex_lock(&s->lock);
    s->joined = 1;
    mn_mutex_unlock(&s->lock);

    mn_atomic_store_flag(&s->walking, 0);
    mn_atomic_store_flag(&s->finished, 1);
}

void mn_scanner_cancel(mn_scanner *scanner) {
    mn_scanner *s = scanner;
    if (!s) return;

    mn_atomic_store_flag(&s->cancel_flag, 1);

    /* Wake any workers blocked on the condition variable so they observe
     * the cancel flag and exit promptly. */
    mn_mutex_lock(&s->lock);
    mn_cond_broadcast(&s->cv);
    mn_mutex_unlock(&s->lock);
}

void mn_scanner_progress(const mn_scanner *scanner,
                         struct mn_scanner_progress *out) {
    const mn_scanner *s = scanner;

    if (!s || !out) return;

    out->dirs_scanned    = mn_atomic_load_u64(&s->dirs_scanned);
    out->files_found     = mn_atomic_load_u64(&s->files_found);
    out->files_processed = mn_atomic_load_u64(&s->files_processed);
    out->files_skipped   = mn_atomic_load_u64(&s->files_skipped);
    out->tag_errors      = mn_atomic_load_u64(&s->tag_errors);
    out->io_errors       = mn_atomic_load_u64(&s->io_errors);

    out->walking   = mn_atomic_load_flag(&s->walking) != 0;
    out->finished  = mn_atomic_load_flag(&s->finished) != 0;
    out->cancelled = mn_atomic_load_flag(&s->cancel_flag) != 0;
}

void mn_scanner_destroy(mn_scanner *scanner) {
    mn_scanner *s = scanner;
    size_t i;
    mn_dir_node *n;

    if (!s) return;

    /* Implicitly cancel and wait for the workers, if any were started. */
    mn_scanner_cancel(s);
    mn_scanner_wait(s);

    /* Drain any directories left in the queue (e.g. after a cancel). */
    n = s->queue_head;
    while (n) {
        mn_dir_node *next = n->next;
        free(n->path);
        free(n);
        n = next;
    }
    s->queue_head = NULL;

    free(s->threads);

    for (i = 0; i < s->root_count; ++i) {
        free(s->roots[i]);
    }
    free(s->roots);

    mn_cond_destroy(&s->cv);
    mn_mutex_destroy(&s->lock);

    free(s);
}
