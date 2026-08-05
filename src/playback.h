/*
 * playback.h -- Queue/playback controller for Monatomic Audio Player.
 *
 * A thin controller layered over the audio engine (mn_engine). It owns the
 * play queue, playback state machine, shuffle/repeat logic, crossfade and
 * ReplayGain settings, and drives gapless auto-advance via mn_playback_tick().
 *
 * Threading model: this API is NOT internally synchronized. All calls must be
 * made from a single owning thread (typically the UI/control thread). The
 * underlying mn_engine handles the realtime audio thread; the controller only
 * issues commands to it. mn_playback_tick() is expected to be called
 * periodically from the same owning thread (e.g. once per UI frame or timer).
 *
 * Ownership: track paths passed into set_queue/append/insert_next are copied
 * internally, so caller-owned buffers may be freed after the call returns.
 *
 * Scale: designed for very large queues. Queue mutations aim to avoid O(n)
 * work on the audio path; index-based addressing is used throughout so the UI
 * never needs to scan the full queue.
 */
#ifndef MN_PLAYBACK_H
#define MN_PLAYBACK_H

#include <stddef.h>   /* size_t   */
#include <stdint.h>   /* int64_t, uint64_t, uint32_t */
#include <stdbool.h>  /* bool     */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration: the audio engine is defined in audio_engine.h.
 * playback only holds a pointer to an engine it does not own. */
typedef struct mn_engine mn_engine;

/* Opaque playback controller handle. */
typedef struct mn_playback mn_playback;

/* ------------------------------------------------------------------ *
 *  Enums
 * ------------------------------------------------------------------ */

/* High-level playback state. */
typedef enum mn_playback_state {
    MN_PLAYBACK_STOPPED = 0, /* No active playback (idle or after stop/end). */
    MN_PLAYBACK_PLAYING = 1  /* A track is loaded and advancing (incl. paused;
                              * see mn_playback_is_paused for pause detail).  */
} mn_playback_state;

/* Repeat mode. */
typedef enum mn_repeat_mode {
    MN_REPEAT_OFF = 0, /* Stop after the last track in the queue.        */
    MN_REPEAT_ALL = 1, /* Wrap from last track back to the first.        */
    MN_REPEAT_ONE = 2  /* Repeat the current track indefinitely.         */
} mn_repeat_mode;

/* ReplayGain application mode. */
typedef enum mn_replaygain_mode {
    MN_REPLAYGAIN_OFF   = 0, /* No gain adjustment applied.               */
    MN_REPLAYGAIN_TRACK = 1, /* Use per-track gain (rg_track_db).         */
    MN_REPLAYGAIN_ALBUM = 2  /* Use per-album gain (rg_album_db).         */
} mn_replaygain_mode;

/* ------------------------------------------------------------------ *
 *  Track descriptor
 * ------------------------------------------------------------------ */

/* Sentinel used for a missing/unknown ReplayGain value. When a track's
 * rg_*_db equals this, ReplayGain falls back to no adjustment for that track
 * regardless of the active mode. */
#define MN_RG_UNKNOWN_DB (-1000.0)

/* A single queue entry supplied by the caller.
 *
 * `path`        UTF-8, null-terminated absolute path to the media file.
 *               Copied internally by the controller.
 * `id`          Caller-defined stable identifier (e.g. library track rowid).
 *               Opaque to playback; echoed back via current_track. Use 0 or
 *               any convention for "no id".
 * `rg_track_db` Per-track ReplayGain in dB, or MN_RG_UNKNOWN_DB if unknown.
 * `rg_album_db` Per-album ReplayGain in dB, or MN_RG_UNKNOWN_DB if unknown.
 */
typedef struct mn_track {
    const char *path;
    int64_t     id;
    double      rg_track_db;
    double      rg_album_db;
    int64_t     duration_ms; /* known track duration (DB tag), 0 = unknown.
                              * Passed to the engine as a length hint so
                              * loading skips the whole-file length scan —
                              * the stall that made switching into large
                              * single-file audiobooks non-instant. */
} mn_track;

/* Information about the currently active track, filled by current_track. */
typedef struct mn_track_info {
    char    path[1024]; /* Null-terminated UTF-8 path of the current track. */
    int64_t id;         /* Caller-defined id supplied when enqueued.        */
    size_t  index;      /* Position in the queue (0-based).                 */
} mn_track_info;

/* ------------------------------------------------------------------ *
 *  Lifecycle
 * ------------------------------------------------------------------ */

/* Create a playback controller bound to an existing, initialized `engine`.
 * The controller does not take ownership of the engine and does not free it.
 * `engine` must outlive the returned controller.
 * Returns NULL on allocation failure or if `engine` is NULL. */
mn_playback *mn_playback_create(mn_engine *engine);

/* Destroy a controller and release all internal queue storage. Stops playback
 * if active. Safe to call with NULL (no-op). The bound engine is untouched. */
void mn_playback_destroy(mn_playback *pb);

/* ------------------------------------------------------------------ *
 *  Queue management
 * ------------------------------------------------------------------ */

/* Replace the entire queue with `count` tracks from `tracks`. Any current
 * playback is stopped and the play position is reset. Passing count==0 clears
 * the queue (equivalent to mn_playback_clear). Track data is deep-copied.
 * Returns true on success, false on allocation failure (queue left unchanged
 * on failure). */
bool mn_playback_set_queue(mn_playback *pb, const mn_track *tracks, size_t count);

/* Append `count` tracks to the end of the queue. Does not change the current
 * play position or state. Track data is deep-copied.
 * Returns true on success, false on allocation failure. */
bool mn_playback_append(mn_playback *pb, const mn_track *tracks, size_t count);

/* Insert `count` tracks immediately after the current track ("play next").
 * If nothing is playing, inserts at the front of the queue. Track data is
 * deep-copied. Returns true on success, false on allocation failure. */
bool mn_playback_insert_next(mn_playback *pb, const mn_track *tracks, size_t count);

/* Remove the track at `index`. If it is the current track, playback advances
 * according to the current repeat mode (or stops if the queue empties).
 * Indices after `index` shift down by one.
 * Returns true on success, false if `index` is out of range. */
bool mn_playback_remove(mn_playback *pb, size_t index);

/* Move the track at `from` to `to`, shifting intervening entries. Both indices
 * must be in [0, count-1]. The current-track tracking follows the moved entry.
 * Returns true on success, false if either index is out of range. */
bool mn_playback_move(mn_playback *pb, size_t from, size_t to);

/* Remove all tracks and stop playback. */
void mn_playback_clear(mn_playback *pb);

/* Return the number of tracks currently in the queue. */
size_t mn_playback_count(const mn_playback *pb);

/* ------------------------------------------------------------------ *
 *  Transport
 * ------------------------------------------------------------------ */

/* Begin playing the track at `index`, loading it into the engine and entering
 * MN_PLAYBACK_PLAYING (unpaused). Returns true on success, false if `index` is
 * out of range or the track failed to load. */
bool mn_playback_play_index(mn_playback *pb, size_t index);

/* Advance to the next track per shuffle/repeat rules and begin playing it.
 * With MN_REPEAT_ONE this restarts the current track. At the end of the queue
 * with MN_REPEAT_OFF this stops playback and returns false.
 * Returns true if a track began playing, false otherwise. */
bool mn_playback_next(mn_playback *pb);

/* Go to the previous track and begin playing it. Behavior near the start of
 * the queue follows the current repeat mode (wraps under MN_REPEAT_ALL).
 * Returns true if a track began playing, false otherwise. */
bool mn_playback_prev(mn_playback *pb);

/* Toggle between paused and playing for the current track. No-op (returns the
 * unchanged pause state) when stopped. Returns the resulting paused state
 * (true == now paused). */
bool mn_playback_toggle_pause(mn_playback *pb);

/* Explicitly set the paused state of the current track. No-op when stopped. */
void mn_playback_set_paused(mn_playback *pb, bool paused);

/* Stop playback, unload the current track from the engine, and enter
 * MN_PLAYBACK_STOPPED. The queue is preserved; the play position is retained
 * so a subsequent mn_playback_next resumes from a sensible point. */
void mn_playback_stop(mn_playback *pb);

/* Seek within the current track to `position_ms` (milliseconds from start).
 * Clamped to the track's duration. No-op when stopped.
 * Returns true on success, false if stopped or the seek failed. */
bool mn_playback_seek_ms(mn_playback *pb, uint64_t position_ms);

/* ------------------------------------------------------------------ *
 *  Modes & settings
 * ------------------------------------------------------------------ */

/* Shuffle. When enabled, next/prev and auto-advance traverse a shuffled order
 * while the underlying queue indices are preserved. */
bool mn_playback_get_shuffle(const mn_playback *pb);
void mn_playback_set_shuffle(mn_playback *pb, bool enabled);

/* Repeat mode (see mn_repeat_mode). */
mn_repeat_mode mn_playback_get_repeat(const mn_playback *pb);
void           mn_playback_set_repeat(mn_playback *pb, mn_repeat_mode mode);

/* Crossfade duration in milliseconds applied between consecutive tracks on
 * auto-advance and manual next/prev. 0 disables crossfade (enabling true
 * gapless playback). */
uint32_t mn_playback_get_crossfade_ms(const mn_playback *pb);
void     mn_playback_set_crossfade_ms(mn_playback *pb, uint32_t crossfade_ms);

/* ReplayGain configuration. `mode` selects the source of gain (off/track/
 * album). `target_db` is the reference/pre-amp level in dB applied together
 * with the per-track or per-album gain (typical value 0.0). */
void mn_playback_set_replaygain(mn_playback *pb, mn_replaygain_mode mode, double target_db);
mn_replaygain_mode mn_playback_get_replaygain_mode(const mn_playback *pb);
double             mn_playback_get_replaygain_target_db(const mn_playback *pb);

/* Output volume as a linear scalar in [0.0, 1.0] (1.0 == unity). Values are
 * clamped to that range. This is independent of and multiplied with any
 * ReplayGain adjustment. */
float mn_playback_get_volume(const mn_playback *pb);
void  mn_playback_set_volume(mn_playback *pb, float volume);

/* ------------------------------------------------------------------ *
 *  Driver / state query
 * ------------------------------------------------------------------ */

/* Periodic pump that drives gapless/crossfaded auto-advance. Call regularly
 * from the owning thread. When the current track nears its end, this loads and
 * begins the next track per shuffle/repeat rules (respecting crossfade_ms), or
 * transitions to MN_PLAYBACK_STOPPED at the end of the queue with
 * MN_REPEAT_OFF. Cheap to call when nothing is due.
 * Returns true if a track transition occurred during this tick. */
bool mn_playback_tick(mn_playback *pb);

/* Current high-level state (see mn_playback_state). */
mn_playback_state mn_playback_state_get(const mn_playback *pb);

/* Whether the current track is paused. False when stopped. */
bool mn_playback_is_paused(const mn_playback *pb);

/* Fill `*out_info` with details of the currently active track.
 * Returns true if a track is active (playing or paused) and info was written;
 * false when stopped (in which case `*out_info` is left unmodified). */
bool mn_playback_current_track(const mn_playback *pb, mn_track_info *out_info);

/* Convenience: current queue index of the active track. Returns SIZE_MAX when
 * stopped / no active track. */
size_t mn_playback_current_index(const mn_playback *pb);

/* Fill out_info with the queued track at `index` (0-based) WITHOUT changing
 * playback. Returns false if index is out of range. Unlike current_track this
 * works regardless of play state. */
bool mn_playback_track_at(const mn_playback *pb, size_t index, mn_track_info *out_info);

/* Current index regardless of play/pause state (SIZE_MAX if none). */
size_t mn_playback_position_index(const mn_playback *pb);

/* Current playback position within the active track, in milliseconds.
 * Returns 0 when stopped. */
uint64_t mn_playback_position_ms(const mn_playback *pb);

/* Duration of the active track in milliseconds, or 0 when stopped or unknown. */
uint64_t mn_playback_duration_ms(const mn_playback *pb);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_PLAYBACK_H */
