/*
 * scanner.h -- Monatomic Music Player
 *
 * Multi-threaded, incremental filesystem scanner.
 *
 * The scanner recursively walks one or more root directories, discovers
 * candidate audio files, reads their metadata tags (see tags.h) on a pool
 * of worker threads, and reports each track back to the caller through a
 * user-supplied callback. It is designed for EXTREMELY LARGE libraries
 * (on the order of 1,000,000 tracks): directory walking and tag reading
 * run entirely on background threads, results are streamed as they are
 * produced, and an optional "is_known" predicate lets the caller skip
 * files that are already up to date so that re-scans are cheap.
 *
 * Threading model:
 *   - mn_scanner_start() returns immediately (non-blocking); all work
 *     happens on internal threads.
 *   - The on_track and is_known callbacks are invoked CONCURRENTLY from
 *     multiple worker threads. They MUST be thread-safe (guard shared
 *     state with your own locks, or hand work off to a single-consumer
 *     queue). They must not call back into the same mn_scanner instance.
 *   - Progress can be polled at any time from any thread.
 *
 * Ownership:
 *   - All pointers passed to callbacks (paths, tags) are owned by the
 *     scanner and are valid ONLY for the duration of the callback. Copy
 *     anything you need to retain.
 *
 * Platform:
 *   - Win32: FindFirstFileW / FindNextFileW (wide paths, long-path aware).
 *   - POSIX: opendir / readdir.
 *
 * Paths passed to callbacks are UTF-8 encoded on all platforms.
 */

#ifndef MN_SCANNER_H
#define MN_SCANNER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "tags.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque scanner instance. Create with mn_scanner_create(). */
typedef struct mn_scanner mn_scanner;

/*
 * Callback invoked once for every audio file whose tags were successfully
 * read. Invoked concurrently from worker threads.
 *
 *   user  - the user pointer from mn_scanner_config.
 *   path  - absolute path to the file, UTF-8. Valid only during the call.
 *   tags  - parsed metadata (see tags.h). Valid only during the call.
 *
 * The scanner does not interpret the return value.
 */
typedef void (*mn_scanner_on_track_fn)(void *user,
                                       const char *path,
                                       const mn_tags *tags);

/*
 * Optional predicate invoked (concurrently, from worker threads) before a
 * discovered file is opened and its tags are read. It lets the caller skip
 * files that are already recorded and unchanged, which is what makes
 * incremental re-scans cheap.
 *
 *   user  - the user pointer from mn_scanner_config.
 *   path  - absolute path to the file, UTF-8. Valid only during the call.
 *   mtime - file last-modified time, seconds since the Unix epoch (UTC).
 *   size  - file size in bytes.
 *
 * Return true if the file is already known and up to date; the scanner
 * will then skip it (counted in progress.files_skipped) and NOT invoke
 * on_track for it. Return false to have the file processed normally.
 *
 * If the config's is_known field is NULL, every discovered file is
 * processed.
 */
typedef bool (*mn_scanner_is_known_fn)(void *user,
                                       const char *path,
                                       int64_t mtime,
                                       uint64_t size);

/*
 * Scanner configuration. Zero-initialize this struct before filling it in
 * so that fields added in future revisions default sensibly.
 */
typedef struct mn_scanner_config {
    /*
     * Array of root directory paths to scan recursively, UTF-8 encoded.
     * The scanner copies the strings, so the array and its contents need
     * only remain valid for the duration of the mn_scanner_create() call.
     */
    const char *const *roots;

    /* Number of entries in the roots array. Must be >= 1. */
    size_t root_count;

    /*
     * Required. Invoked for each successfully processed track. See
     * mn_scanner_on_track_fn. Must be non-NULL.
     */
    mn_scanner_on_track_fn on_track;

    /*
     * Optional. If non-NULL, invoked to decide whether a discovered file
     * should be skipped. See mn_scanner_is_known_fn.
     */
    mn_scanner_is_known_fn is_known;

    /* Opaque pointer passed verbatim to on_track and is_known. */
    void *user;

    /*
     * Number of worker threads to use for directory walking and tag
     * reading. 0 means "choose automatically" (typically the number of
     * logical CPUs).
     */
    unsigned thread_count;
} mn_scanner_config;

/*
 * Snapshot of scanner progress. Counters are cumulative for the current
 * run and are monotonically non-decreasing while the scan is in flight.
 *
 * Populated by mn_scanner_progress(), which takes a consistent snapshot.
 */
/*
 * NOTE: This is declared as a bare `struct mn_scanner_progress` (a struct
 * tag) rather than a typedef. In C, typedef names and function names share
 * the ordinary-identifier namespace, so a typedef named mn_scanner_progress
 * would collide with the mn_scanner_progress() function declared below.
 * Using a struct tag (a separate namespace, as with `struct stat`/stat())
 * lets the type and the function share the name legally. Refer to the type
 * as `struct mn_scanner_progress`.
 */
struct mn_scanner_progress {
    /* Directories entered and enumerated. */
    uint64_t dirs_scanned;

    /* Candidate audio files discovered by the walker. */
    uint64_t files_found;

    /* Files whose tags were read and reported via on_track. */
    uint64_t files_processed;

    /* Files skipped because is_known returned true. */
    uint64_t files_skipped;

    /* Files that were opened but whose tags could not be parsed. */
    uint64_t tag_errors;

    /* Directory/file I/O errors encountered (open, read, enumerate). */
    uint64_t io_errors;

    /* True while directory walking is still in progress. */
    bool walking;

    /* True once all work has completed (naturally or after cancel). */
    bool finished;

    /* True if the run was cancelled via mn_scanner_cancel(). */
    bool cancelled;
};

/*
 * Create a scanner from the given configuration.
 *
 * The configuration is validated and its contents (including the roots
 * array and strings) are copied internally; the caller retains ownership
 * of everything it passed in.
 *
 * Returns a new scanner instance, or NULL on invalid configuration
 * (e.g. root_count == 0, roots == NULL, on_track == NULL) or allocation
 * failure. The returned scanner is idle; call mn_scanner_start() to begin.
 *
 * Destroy the returned instance with mn_scanner_destroy().
 */
mn_scanner *mn_scanner_create(const mn_scanner_config *config);

/*
 * Begin scanning. Non-blocking: spawns the worker threads and returns
 * immediately. Callbacks begin firing on worker threads shortly after.
 *
 * A given scanner instance may be started only once. Calling start on an
 * instance that has already been started (whether running, finished, or
 * cancelled) returns false.
 *
 * Returns true if the scan was started, false on error (NULL scanner,
 * already started, or thread creation failure).
 */
bool mn_scanner_start(mn_scanner *scanner);

/*
 * Block until the scan has fully finished -- either by processing all
 * discovered files or because it was cancelled and all worker threads
 * have drained. After this returns, no further callbacks will fire.
 *
 * Safe to call on a scanner that was never started or has already
 * finished; in those cases it returns immediately. Passing NULL is a
 * no-op.
 */
void mn_scanner_wait(mn_scanner *scanner);

/*
 * Request cancellation. Non-blocking: sets a flag that worker threads
 * observe at the next opportunity, then returns. In-flight callbacks may
 * still complete. Use mn_scanner_wait() to block until the threads have
 * actually stopped.
 *
 * Idempotent; safe to call multiple times and on a scanner that has not
 * been started. Passing NULL is a no-op.
 */
void mn_scanner_cancel(mn_scanner *scanner);

/*
 * Take a consistent snapshot of the scanner's progress counters and
 * state flags into *out.
 *
 * Safe to call at any time from any thread, before/during/after a scan.
 * If the scanner has not been started, all counters are zero and all
 * flags are false. Does nothing if scanner or out is NULL.
 */
void mn_scanner_progress(const mn_scanner *scanner,
                         struct mn_scanner_progress *out);

/*
 * Destroy a scanner and release all its resources.
 *
 * If a scan is still in flight, this implicitly cancels it and blocks
 * until all worker threads have stopped (equivalent to cancel + wait)
 * before freeing. After this call the pointer is invalid.
 *
 * Passing NULL is a no-op.
 */
void mn_scanner_destroy(mn_scanner *scanner);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_SCANNER_H */
