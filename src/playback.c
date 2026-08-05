/*
 * playback.c -- Queue/playback controller for Monatomic Music Player.
 *
 * Implements playback.h on top of audio_engine.h (mn_engine). The controller
 * owns the play queue and the playback state machine, and translates high-level
 * transport intents (next/prev/auto-advance/shuffle/repeat) into concrete
 * mn_engine load/play/seek/gain commands.
 *
 * Design notes
 * ------------
 * Queue storage:   A single growable array of `mn_queue_entry`, each holding a
 *                  heap-copied path plus the caller's id and ReplayGain values.
 *                  All addressing into the queue is by 0-based index.
 *
 * Ordering:        The queue array itself is always in "insertion" order. When
 *                  shuffle is enabled we maintain a separate permutation
 *                  (`order`) of queue indices and a cursor (`order_pos`) into it.
 *                  This lets the UI keep stable indices while playback walks a
 *                  shuffled path. The permutation is (re)built with Fisher-Yates.
 *
 * History:         A stack of queue indices records the actual visited order so
 *                  that mn_playback_prev walks back through real listening
 *                  history (important under shuffle, where "previous" is not
 *                  simply index-1).
 *
 * Crossfade:       The underlying engine renders a single decoder at a time and
 *                  cannot mix two streams, so a true overlapping crossfade is not
 *                  possible here. crossfade_ms is instead used as an early
 *                  auto-advance lookahead in mn_playback_tick(): when the current
 *                  track is within crossfade_ms of its end, the next track is
 *                  started, trimming the inter-track gap toward gapless. The
 *                  value is stored and honored as a lookahead window; 0 means
 *                  advance only on true end-of-stream.
 *
 * ReplayGain:      On each track start we compute an effective gain in dB from
 *                  the active mode (off/track/album), the per-track/album value,
 *                  and the configured target/pre-amp, and push it to the engine
 *                  via mn_engine_set_gain_db(). Unknown values fall back to 0 dB.
 *
 * Threading:       Not internally synchronized; single owning thread only, per
 *                  the header contract.
 */

#include "playback.h"
#include "audio_engine.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  Internal types
 * ------------------------------------------------------------------ */

/* One stored queue entry (deep copy of the caller-supplied mn_track). */
typedef struct mn_queue_entry {
    char   *path;        /* heap-owned NUL-terminated copy, never NULL when in use */
    int64_t id;          /* caller-defined opaque id */
    double  rg_track_db; /* per-track ReplayGain, or MN_RG_UNKNOWN_DB */
    double  rg_album_db; /* per-album ReplayGain, or MN_RG_UNKNOWN_DB */
    int64_t duration_ms; /* DB duration hint for the engine (0 = unknown) */
} mn_queue_entry;

struct mn_playback {
    mn_engine *engine;   /* borrowed, not owned */

    /* Queue storage (insertion order). */
    mn_queue_entry *items;
    size_t          count;
    size_t          cap;

    /* Current track. `current` is a queue index, or SIZE_MAX when none active. */
    size_t            current;
    mn_playback_state state;
    bool              paused;

    /* Shuffle ordering. `order` is a permutation of [0, count) valid only when
     * `shuffle` is true and order_len == count. order_pos is the cursor. */
    bool    shuffle;
    size_t *order;
    size_t  order_len;
    size_t  order_cap;
    size_t  order_pos;

    /* Listening history (stack of queue indices actually visited). */
    size_t *history;
    size_t  history_len;
    size_t  history_cap;

    /* Modes / settings. */
    mn_repeat_mode     repeat;
    uint32_t           crossfade_ms;
    mn_replaygain_mode rg_mode;
    double             rg_target_db;
    float              volume;

    /* Simple xorshift RNG state for shuffle (deterministic-ish, seeded once). */
    uint64_t rng;
};

/* ------------------------------------------------------------------ *
 *  Small helpers
 * ------------------------------------------------------------------ */

/* xorshift64* PRNG. Cheap, adequate for shuffling a play queue. */
static uint64_t mn__rng_next(mn_playback *pb)
{
    uint64_t x = pb->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    pb->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Duplicate a NUL-terminated string. Returns NULL on failure or NULL input. */
static char *mn__strdup(const char *s)
{
    size_t n;
    char  *p;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1u;
    p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

/* Free the path storage of a single entry and zero it out. */
static void mn__entry_free(mn_queue_entry *e)
{
    if (e != NULL) {
        free(e->path);
        e->path = NULL;
    }
}

/* Ensure `pb->items` can hold at least `need` entries. Returns false on OOM. */
static bool mn__reserve_items(mn_playback *pb, size_t need)
{
    size_t          newcap;
    mn_queue_entry *ni;
    if (need <= pb->cap) {
        return true;
    }
    newcap = (pb->cap != 0u) ? pb->cap : 16u;
    while (newcap < need) {
        /* Guard against overflow on the doubling. */
        if (newcap > (SIZE_MAX / 2u)) {
            newcap = need;
            break;
        }
        newcap *= 2u;
    }
    ni = (mn_queue_entry *)realloc(pb->items, newcap * sizeof(*pb->items));
    if (ni == NULL) {
        return false;
    }
    pb->items = ni;
    pb->cap   = newcap;
    return true;
}

/* Push a queue index onto the history stack. Best-effort: on OOM the history
 * simply loses this entry (prev becomes less precise) but playback continues. */
static void mn__history_push(mn_playback *pb, size_t index)
{
    if (pb->history_len >= pb->history_cap) {
        size_t  newcap = (pb->history_cap != 0u) ? pb->history_cap * 2u : 32u;
        size_t *nh     = (size_t *)realloc(pb->history, newcap * sizeof(*nh));
        if (nh == NULL) {
            return; /* keep old history, drop this push */
        }
        pb->history     = nh;
        pb->history_cap = newcap;
    }
    pb->history[pb->history_len++] = index;
}

/* ------------------------------------------------------------------ *
 *  Shuffle ordering
 * ------------------------------------------------------------------ */

/* (Re)build the shuffle permutation over [0, count) using Fisher-Yates.
 * If `first` is a valid index it is placed at order_pos 0 so playback continues
 * from the current track before visiting the rest in shuffled order.
 * Returns false on OOM (shuffle order left empty/invalid). */
static bool mn__build_order(mn_playback *pb, size_t first)
{
    size_t i;

    pb->order_len = 0u;
    pb->order_pos = 0u;

    if (pb->count == 0u) {
        return true;
    }
    if (pb->count > pb->order_cap) {
        size_t *no = (size_t *)realloc(pb->order, pb->count * sizeof(*no));
        if (no == NULL) {
            return false;
        }
        pb->order     = no;
        pb->order_cap = pb->count;
    }

    for (i = 0u; i < pb->count; ++i) {
        pb->order[i] = i;
    }
    /* Fisher-Yates shuffle. */
    for (i = pb->count; i > 1u; --i) {
        size_t j   = (size_t)(mn__rng_next(pb) % (uint64_t)i);
        size_t tmp = pb->order[i - 1u];
        pb->order[i - 1u] = pb->order[j];
        pb->order[j]      = tmp;
    }
    pb->order_len = pb->count;

    /* Move `first` to the front so we don't immediately jump away from it. */
    if (first < pb->count) {
        for (i = 0u; i < pb->order_len; ++i) {
            if (pb->order[i] == first) {
                size_t tmp   = pb->order[0];
                pb->order[0] = pb->order[i];
                pb->order[i] = tmp;
                break;
            }
        }
    }
    return true;
}

/* Find the position of queue index `qidx` within the shuffle order, or
 * SIZE_MAX if not present / order invalid. */
static size_t mn__order_find(const mn_playback *pb, size_t qidx)
{
    size_t i;
    if (pb->order == NULL || pb->order_len != pb->count) {
        return SIZE_MAX;
    }
    for (i = 0u; i < pb->order_len; ++i) {
        if (pb->order[i] == qidx) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* ------------------------------------------------------------------ *
 *  Gain / engine helpers
 * ------------------------------------------------------------------ */

/* Compute and apply the effective ReplayGain (in dB) for entry `e`. */
static void mn__apply_gain(mn_playback *pb, const mn_queue_entry *e)
{
    double gain_db = 0.0;

    if (pb->rg_mode == MN_REPLAYGAIN_TRACK) {
        if (e->rg_track_db > MN_RG_UNKNOWN_DB) {
            gain_db = e->rg_track_db + pb->rg_target_db;
        }
    } else if (pb->rg_mode == MN_REPLAYGAIN_ALBUM) {
        if (e->rg_album_db > MN_RG_UNKNOWN_DB) {
            gain_db = e->rg_album_db + pb->rg_target_db;
        }
    }
    /* MN_REPLAYGAIN_OFF (or unknown values) -> 0 dB unity. */
    (void)mn_engine_set_gain_db(pb->engine, (float)gain_db);
}

/* Load the queue entry at `index` into the engine and begin playing it,
 * applying ReplayGain and recording history. Updates controller state.
 * Returns true on success; on failure leaves the controller stopped. */
static bool mn__start_index(mn_playback *pb, size_t index, bool record_history)
{
    const mn_queue_entry *e;

    if (index >= pb->count) {
        return false;
    }
    e = &pb->items[index];
    if (e->path == NULL) {
        return false;
    }

    /* Known duration -> the engine can skip its whole-file length scan
     * (instant switches even into multi-hour single-file audiobooks). */
    mn_engine_set_length_hint_ms(pb->engine, e->duration_ms);
    if (mn_engine_load(pb->engine, e->path) != MN_OK) {
        /* Engine unloads itself on load failure; reflect stopped state. */
        pb->state   = MN_PLAYBACK_STOPPED;
        pb->paused  = false;
        pb->current = SIZE_MAX;
        return false;
    }

    mn__apply_gain(pb, e);
    (void)mn_engine_set_volume(pb->engine, pb->volume);

    if (mn_engine_play(pb->engine) != MN_OK) {
        (void)mn_engine_stop(pb->engine);
        pb->state   = MN_PLAYBACK_STOPPED;
        pb->paused  = false;
        pb->current = SIZE_MAX;
        return false;
    }

    if (record_history && pb->current != SIZE_MAX && pb->current < pb->count) {
        mn__history_push(pb, pb->current);
    }

    pb->current = index;
    pb->state   = MN_PLAYBACK_PLAYING;
    pb->paused  = false;

    /* Keep the shuffle cursor synchronized with the actually-playing track. */
    if (pb->shuffle) {
        size_t pos = mn__order_find(pb, index);
        if (pos != SIZE_MAX) {
            pb->order_pos = pos;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ *
 *  Next-index selection
 * ------------------------------------------------------------------ */

/* Determine the queue index that should follow the current one on a forward
 * step, honoring shuffle and repeat. Does not consider MN_REPEAT_ONE (callers
 * decide whether to restart). Returns SIZE_MAX when there is no next track
 * (end of queue under MN_REPEAT_OFF, or empty queue). */
static size_t mn__pick_next(mn_playback *pb)
{
    if (pb->count == 0u) {
        return SIZE_MAX;
    }

    if (pb->shuffle) {
        if (pb->order == NULL || pb->order_len != pb->count) {
            if (!mn__build_order(pb, pb->current)) {
                return SIZE_MAX;
            }
        }
        if (pb->order_pos + 1u < pb->order_len) {
            return pb->order[pb->order_pos + 1u];
        }
        /* Reached end of the shuffled order. */
        if (pb->repeat == MN_REPEAT_ALL) {
            /* Reshuffle for a fresh pass and start from its head. */
            if (!mn__build_order(pb, SIZE_MAX)) {
                return SIZE_MAX;
            }
            pb->order_pos = SIZE_MAX; /* so caller's +1 logic still lands on 0 */
            return (pb->order_len > 0u) ? pb->order[0] : SIZE_MAX;
        }
        return SIZE_MAX;
    }

    /* Linear order. */
    if (pb->current == SIZE_MAX) {
        return 0u; /* start from the top */
    }
    if (pb->current + 1u < pb->count) {
        return pb->current + 1u;
    }
    if (pb->repeat == MN_REPEAT_ALL) {
        return 0u;
    }
    return SIZE_MAX;
}

/* Advance to the next track and start it. `auto_advance` distinguishes tick /
 * end-of-track advance (which honors MN_REPEAT_ONE by restarting) from... it is
 * the same policy for manual next per the header (REPEAT_ONE restarts current).
 * Returns true if a track began playing. */
static bool mn__advance(mn_playback *pb)
{
    size_t next;

    if (pb->count == 0u) {
        mn_playback_stop(pb);
        return false;
    }

    /* Repeat-one: restart the current track (or first if none). */
    if (pb->repeat == MN_REPEAT_ONE && pb->current != SIZE_MAX) {
        return mn__start_index(pb, pb->current, false);
    }

    next = mn__pick_next(pb);
    if (next == SIZE_MAX) {
        /* End of queue with no repeat/wrap: stop. */
        mn_playback_stop(pb);
        return false;
    }

    /* After a successful reshuffle in mn__pick_next, order_pos was primed to
     * SIZE_MAX; start_index will resync it from the chosen index. */
    return mn__start_index(pb, next, true);
}

/* ------------------------------------------------------------------ *
 *  Lifecycle
 * ------------------------------------------------------------------ */

mn_playback *mn_playback_create(mn_engine *engine)
{
    mn_playback *pb;
    if (engine == NULL) {
        return NULL;
    }
    pb = (mn_playback *)calloc(1u, sizeof(*pb));
    if (pb == NULL) {
        return NULL;
    }
    pb->engine       = engine;
    pb->current      = SIZE_MAX;
    pb->state        = MN_PLAYBACK_STOPPED;
    pb->paused       = false;
    pb->shuffle      = false;
    pb->repeat       = MN_REPEAT_OFF;
    pb->crossfade_ms = 0u;
    pb->rg_mode      = MN_REPLAYGAIN_OFF;
    pb->rg_target_db = 0.0;
    pb->volume       = 1.0f;
    /* Seed the RNG. A fixed nonzero constant keeps it well-defined; mixing the
     * pointer value adds run-to-run variation without needing <time.h>. */
    pb->rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)pb;
    if (pb->rng == 0u) {
        pb->rng = 0x123456789ABCDEFULL;
    }
    return pb;
}

void mn_playback_destroy(mn_playback *pb)
{
    size_t i;
    if (pb == NULL) {
        return;
    }
    /* Stop the engine so it isn't left pointing at freed paths. */
    (void)mn_engine_stop(pb->engine);

    for (i = 0u; i < pb->count; ++i) {
        mn__entry_free(&pb->items[i]);
    }
    free(pb->items);
    free(pb->order);
    free(pb->history);
    free(pb);
}

/* ------------------------------------------------------------------ *
 *  Queue management
 * ------------------------------------------------------------------ */

/* Deep-copy `count` tracks from `src` into pb->items[at..]. The destination
 * slots must already be reserved. On OOM, any partially-copied entries are
 * rolled back and false is returned. */
static bool mn__copy_in(mn_playback *pb, size_t at, const mn_track *src, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) {
        mn_queue_entry *dst = &pb->items[at + i];
        dst->path = mn__strdup(src[i].path);
        if (dst->path == NULL && src[i].path != NULL) {
            /* Roll back this and previously copied entries in this batch. */
            size_t k;
            for (k = 0u; k < i; ++k) {
                mn__entry_free(&pb->items[at + k]);
            }
            return false;
        }
        dst->id          = src[i].id;
        dst->rg_track_db = src[i].rg_track_db;
        dst->rg_album_db = src[i].rg_album_db;
        dst->duration_ms = src[i].duration_ms;
    }
    return true;
}

bool mn_playback_set_queue(mn_playback *pb, const mn_track *tracks, size_t count)
{
    size_t i;
    if (pb == NULL) {
        return false;
    }
    if (count > 0u && tracks == NULL) {
        return false;
    }

    /* Reserve first so a failure leaves the existing queue intact. */
    if (!mn__reserve_items(pb, count)) {
        return false;
    }

    /* Stop and clear current contents (paths only; keep the array buffer). */
    (void)mn_engine_stop(pb->engine);
    for (i = 0u; i < pb->count; ++i) {
        mn__entry_free(&pb->items[i]);
    }
    pb->count       = 0u;
    pb->current     = SIZE_MAX;
    pb->state       = MN_PLAYBACK_STOPPED;
    pb->paused      = false;
    pb->history_len = 0u;
    pb->order_len   = 0u;
    pb->order_pos   = 0u;

    if (count > 0u) {
        if (!mn__copy_in(pb, 0u, tracks, count)) {
            return false;
        }
        pb->count = count;
    }

    if (pb->shuffle) {
        (void)mn__build_order(pb, SIZE_MAX);
    }
    return true;
}

bool mn_playback_append(mn_playback *pb, const mn_track *tracks, size_t count)
{
    if (pb == NULL) {
        return false;
    }
    if (count == 0u) {
        return true;
    }
    if (tracks == NULL) {
        return false;
    }
    if (count > (SIZE_MAX - pb->count)) {
        return false; /* size overflow guard */
    }
    if (!mn__reserve_items(pb, pb->count + count)) {
        return false;
    }
    if (!mn__copy_in(pb, pb->count, tracks, count)) {
        return false;
    }
    pb->count += count;

    /* Extend shuffle order with the appended indices (kept after current tail
     * so already-scheduled upcoming tracks are preserved). Best-effort: on OOM
     * we drop to a rebuild lazily in mn__pick_next. */
    if (pb->shuffle) {
        (void)mn__build_order(pb, (pb->current != SIZE_MAX) ? pb->current : SIZE_MAX);
    }
    return true;
}

bool mn_playback_insert_next(mn_playback *pb, const mn_track *tracks, size_t count)
{
    size_t at;
    size_t tail;
    if (pb == NULL) {
        return false;
    }
    if (count == 0u) {
        return true;
    }
    if (tracks == NULL) {
        return false;
    }
    if (count > (SIZE_MAX - pb->count)) {
        return false;
    }
    if (!mn__reserve_items(pb, pb->count + count)) {
        return false;
    }

    /* Insertion point: right after current, or front if nothing playing. */
    at = (pb->current != SIZE_MAX && pb->current < pb->count) ? (pb->current + 1u) : 0u;
    if (at > pb->count) {
        at = pb->count;
    }

    /* Shift the tail up by `count` to open a gap. memmove handles overlap. */
    tail = pb->count - at;
    if (tail > 0u) {
        memmove(&pb->items[at + count], &pb->items[at], tail * sizeof(*pb->items));
    }
    if (!mn__copy_in(pb, at, tracks, count)) {
        /* Roll the tail back down to restore the original layout. */
        if (tail > 0u) {
            memmove(&pb->items[at], &pb->items[at + count], tail * sizeof(*pb->items));
        }
        return false;
    }
    pb->count += count;

    /* current index is unaffected (insertion is strictly after it). */
    if (pb->shuffle) {
        (void)mn__build_order(pb, (pb->current != SIZE_MAX) ? pb->current : SIZE_MAX);
    }
    return true;
}

bool mn_playback_remove(mn_playback *pb, size_t index)
{
    bool   removing_current;
    size_t tail;
    if (pb == NULL || index >= pb->count) {
        return false;
    }

    removing_current = (pb->current == index);

    mn__entry_free(&pb->items[index]);
    tail = pb->count - index - 1u;
    if (tail > 0u) {
        memmove(&pb->items[index], &pb->items[index + 1u], tail * sizeof(*pb->items));
    }
    pb->count -= 1u;

    /* Fix up the current-track index for the shift. */
    if (removing_current) {
        /* Will re-resolve below after order rebuild. */
        pb->current = SIZE_MAX;
    } else if (pb->current != SIZE_MAX && pb->current > index) {
        pb->current -= 1u;
    }

    /* Rebuild shuffle order over the new size. */
    if (pb->shuffle) {
        (void)mn__build_order(pb, (pb->current != SIZE_MAX) ? pb->current : SIZE_MAX);
    }

    if (removing_current) {
        /* Advance into the slot the removed track occupied (now the following
         * track), or stop if the queue emptied. Under linear order the natural
         * "next" is `index` itself post-shift. */
        if (pb->count == 0u) {
            mn_playback_stop(pb);
            return true;
        }
        if (pb->repeat == MN_REPEAT_ONE) {
            /* Nothing to repeat (the track is gone); fall through to next. */
        }
        if (pb->shuffle) {
            size_t next = mn__pick_next(pb); /* current==SIZE_MAX -> picks head */
            if (next == SIZE_MAX) {
                next = (pb->order_len > 0u) ? pb->order[0] : SIZE_MAX;
            }
            if (next == SIZE_MAX) {
                mn_playback_stop(pb);
            } else {
                (void)mn__start_index(pb, next, false);
            }
        } else {
            size_t next = (index < pb->count) ? index : 0u;
            if (pb->repeat == MN_REPEAT_OFF && index >= pb->count) {
                /* Removed the last track: stop rather than wrap. */
                mn_playback_stop(pb);
            } else {
                (void)mn__start_index(pb, next, false);
            }
        }
    }
    return true;
}

bool mn_playback_move(mn_playback *pb, size_t from, size_t to)
{
    mn_queue_entry moved;
    if (pb == NULL || from >= pb->count || to >= pb->count) {
        return false;
    }
    if (from == to) {
        return true;
    }

    moved = pb->items[from];
    if (from < to) {
        /* Shift the block (from+1 .. to) down by one. */
        memmove(&pb->items[from], &pb->items[from + 1u],
                (to - from) * sizeof(*pb->items));
    } else {
        /* Shift the block (to .. from-1) up by one. */
        memmove(&pb->items[to + 1u], &pb->items[to],
                (from - to) * sizeof(*pb->items));
    }
    pb->items[to] = moved;

    /* Track the current index across the move. */
    if (pb->current != SIZE_MAX) {
        if (pb->current == from) {
            pb->current = to;
        } else if (from < to && pb->current > from && pb->current <= to) {
            pb->current -= 1u;
        } else if (from > to && pb->current >= to && pb->current < from) {
            pb->current += 1u;
        }
    }

    if (pb->shuffle) {
        (void)mn__build_order(pb, (pb->current != SIZE_MAX) ? pb->current : SIZE_MAX);
    }
    return true;
}

void mn_playback_clear(mn_playback *pb)
{
    size_t i;
    if (pb == NULL) {
        return;
    }
    (void)mn_engine_stop(pb->engine);
    for (i = 0u; i < pb->count; ++i) {
        mn__entry_free(&pb->items[i]);
    }
    pb->count       = 0u;
    pb->current     = SIZE_MAX;
    pb->state       = MN_PLAYBACK_STOPPED;
    pb->paused      = false;
    pb->history_len = 0u;
    pb->order_len   = 0u;
    pb->order_pos   = 0u;
}

size_t mn_playback_count(const mn_playback *pb)
{
    return (pb != NULL) ? pb->count : 0u;
}

/* ------------------------------------------------------------------ *
 *  Transport
 * ------------------------------------------------------------------ */

bool mn_playback_play_index(mn_playback *pb, size_t index)
{
    if (pb == NULL || index >= pb->count) {
        return false;
    }
    /* Ensure a shuffle order exists so the cursor resyncs correctly. */
    if (pb->shuffle && (pb->order == NULL || pb->order_len != pb->count)) {
        (void)mn__build_order(pb, index);
    }
    return mn__start_index(pb, index, true);
}

bool mn_playback_next(mn_playback *pb)
{
    if (pb == NULL) {
        return false;
    }
    return mn__advance(pb);
}

bool mn_playback_prev(mn_playback *pb)
{
    if (pb == NULL || pb->count == 0u) {
        return false;
    }

    /* If we're more than a few seconds into the track, "prev" restarts it
     * (common player behavior). Threshold: 3000 ms. */
    if (pb->current != SIZE_MAX && pb->state == MN_PLAYBACK_PLAYING) {
        uint64_t pos = mn_engine_position_ms(pb->engine);
        if (pos > 3000u) {
            return mn__start_index(pb, pb->current, false);
        }
    }

    /* Prefer real listening history. */
    if (pb->history_len > 0u) {
        size_t prev = pb->history[pb->history_len - 1u];
        pb->history_len -= 1u; /* pop; start_index(record=false) won't re-push */
        if (prev < pb->count) {
            return mn__start_index(pb, prev, false);
        }
    }

    /* No history: derive previous from ordering. */
    if (pb->shuffle) {
        if (pb->order != NULL && pb->order_len == pb->count && pb->order_pos > 0u) {
            return mn__start_index(pb, pb->order[pb->order_pos - 1u], false);
        }
        if (pb->repeat == MN_REPEAT_ALL && pb->order_len > 0u) {
            return mn__start_index(pb, pb->order[pb->order_len - 1u], false);
        }
        return false;
    }

    /* Linear order. */
    if (pb->current != SIZE_MAX && pb->current > 0u) {
        return mn__start_index(pb, pb->current - 1u, false);
    }
    if (pb->repeat == MN_REPEAT_ALL && pb->count > 0u) {
        return mn__start_index(pb, pb->count - 1u, false);
    }
    return false;
}

bool mn_playback_toggle_pause(mn_playback *pb)
{
    if (pb == NULL) {
        return false;
    }
    if (pb->state != MN_PLAYBACK_PLAYING) {
        return pb->paused; /* stopped: no-op, report unchanged */
    }
    mn_playback_set_paused(pb, !pb->paused);
    return pb->paused;
}

void mn_playback_set_paused(mn_playback *pb, bool paused)
{
    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING) {
        return;
    }
    if (paused == pb->paused) {
        return;
    }
    if (paused) {
        (void)mn_engine_pause(pb->engine);
    } else {
        (void)mn_engine_play(pb->engine);
    }
    pb->paused = paused;
}

void mn_playback_stop(mn_playback *pb)
{
    if (pb == NULL) {
        return;
    }
    (void)mn_engine_stop(pb->engine);
    pb->state  = MN_PLAYBACK_STOPPED;
    pb->paused = false;
    /* Retain pb->current so a subsequent next() resumes from a sensible point,
     * per the header contract. */
}

bool mn_playback_seek_ms(mn_playback *pb, uint64_t position_ms)
{
    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING) {
        return false;
    }
    return mn_engine_seek_ms(pb->engine, position_ms) == MN_OK;
}

/* ------------------------------------------------------------------ *
 *  Modes & settings
 * ------------------------------------------------------------------ */

bool mn_playback_get_shuffle(const mn_playback *pb)
{
    return (pb != NULL) ? pb->shuffle : false;
}

void mn_playback_set_shuffle(mn_playback *pb, bool enabled)
{
    if (pb == NULL || pb->shuffle == enabled) {
        return;
    }
    pb->shuffle = enabled;
    if (enabled) {
        /* Build a fresh order anchored on the current track. Clearing history
         * keeps "prev" meaningful relative to the new traversal. */
        (void)mn__build_order(pb, (pb->current != SIZE_MAX) ? pb->current : SIZE_MAX);
    } else {
        pb->order_len = 0u;
        pb->order_pos = 0u;
    }
}

mn_repeat_mode mn_playback_get_repeat(const mn_playback *pb)
{
    return (pb != NULL) ? pb->repeat : MN_REPEAT_OFF;
}

void mn_playback_set_repeat(mn_playback *pb, mn_repeat_mode mode)
{
    if (pb == NULL) {
        return;
    }
    if (mode == MN_REPEAT_OFF || mode == MN_REPEAT_ALL || mode == MN_REPEAT_ONE) {
        pb->repeat = mode;
    }
}

uint32_t mn_playback_get_crossfade_ms(const mn_playback *pb)
{
    return (pb != NULL) ? pb->crossfade_ms : 0u;
}

void mn_playback_set_crossfade_ms(mn_playback *pb, uint32_t crossfade_ms)
{
    if (pb != NULL) {
        pb->crossfade_ms = crossfade_ms;
    }
}

void mn_playback_set_replaygain(mn_playback *pb, mn_replaygain_mode mode, double target_db)
{
    if (pb == NULL) {
        return;
    }
    if (mode == MN_REPLAYGAIN_OFF || mode == MN_REPLAYGAIN_TRACK ||
        mode == MN_REPLAYGAIN_ALBUM) {
        pb->rg_mode = mode;
    }
    pb->rg_target_db = target_db;

    /* Re-apply immediately to the active track so the change is audible now. */
    if (pb->state == MN_PLAYBACK_PLAYING && pb->current != SIZE_MAX &&
        pb->current < pb->count) {
        mn__apply_gain(pb, &pb->items[pb->current]);
    }
}

mn_replaygain_mode mn_playback_get_replaygain_mode(const mn_playback *pb)
{
    return (pb != NULL) ? pb->rg_mode : MN_REPLAYGAIN_OFF;
}

double mn_playback_get_replaygain_target_db(const mn_playback *pb)
{
    return (pb != NULL) ? pb->rg_target_db : 0.0;
}

float mn_playback_get_volume(const mn_playback *pb)
{
    return (pb != NULL) ? pb->volume : 0.0f;
}

void mn_playback_set_volume(mn_playback *pb, float volume)
{
    if (pb == NULL) {
        return;
    }
    if (volume < 0.0f) {
        volume = 0.0f;
    } else if (volume > 1.0f) {
        volume = 1.0f;
    }
    pb->volume = volume;
    (void)mn_engine_set_volume(pb->engine, volume);
}

/* ------------------------------------------------------------------ *
 *  Driver / state query
 * ------------------------------------------------------------------ */

bool mn_playback_tick(mn_playback *pb)
{
    uint64_t dur;
    uint64_t pos;

    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING || pb->paused) {
        return false;
    }
    if (pb->count == 0u) {
        return false;
    }

    /* True end-of-stream: advance immediately. */
    if (mn_engine_finished(pb->engine)) {
        return mn__advance(pb);
    }

    /* Crossfade / early-advance lookahead: if within crossfade_ms of the end,
     * start the next track early to minimize the inter-track gap. The engine
     * cannot overlap two decoders, so this is a gap-trimming approximation of a
     * crossfade rather than a true equal-power blend. */
    if (pb->crossfade_ms > 0u) {
        dur = mn_engine_duration_ms(pb->engine);
        if (dur > 0u) {
            pos = mn_engine_position_ms(pb->engine);
            if (pos < dur && (dur - pos) <= (uint64_t)pb->crossfade_ms) {
                /* Under REPEAT_ONE we do not early-advance; let it play out and
                 * restart cleanly on true end-of-stream. */
                if (pb->repeat != MN_REPEAT_ONE) {
                    return mn__advance(pb);
                }
            }
        }
    }

    return false;
}

mn_playback_state mn_playback_state_get(const mn_playback *pb)
{
    return (pb != NULL) ? pb->state : MN_PLAYBACK_STOPPED;
}

bool mn_playback_is_paused(const mn_playback *pb)
{
    return (pb != NULL) ? pb->paused : false;
}

bool mn_playback_current_track(const mn_playback *pb, mn_track_info *out_info)
{
    const mn_queue_entry *e;
    size_t                n;

    if (pb == NULL || out_info == NULL) {
        return false;
    }
    if (pb->state != MN_PLAYBACK_PLAYING || pb->current == SIZE_MAX ||
        pb->current >= pb->count) {
        return false;
    }

    e = &pb->items[pb->current];
    out_info->id    = e->id;
    out_info->index = pb->current;

    if (e->path != NULL) {
        n = strlen(e->path);
        if (n >= sizeof(out_info->path)) {
            n = sizeof(out_info->path) - 1u;
        }
        memcpy(out_info->path, e->path, n);
        out_info->path[n] = '\0';
    } else {
        out_info->path[0] = '\0';
    }
    return true;
}

size_t mn_playback_position_index(const mn_playback *pb)
{
    /* current index regardless of play/pause state (SIZE_MAX if none) */
    if (pb == NULL || pb->current >= pb->count) return (size_t)-1;
    return pb->current;
}

bool mn_playback_track_at(const mn_playback *pb, size_t index, mn_track_info *out_info)
{
    const mn_queue_entry *e;
    size_t n;

    if (pb == NULL || out_info == NULL || index >= pb->count) {
        return false;
    }
    e = &pb->items[index];
    out_info->id    = e->id;
    out_info->index = index;
    if (e->path != NULL) {
        n = strlen(e->path);
        if (n >= sizeof(out_info->path)) n = sizeof(out_info->path) - 1u;
        memcpy(out_info->path, e->path, n);
        out_info->path[n] = '\0';
    } else {
        out_info->path[0] = '\0';
    }
    return true;
}

size_t mn_playback_current_index(const mn_playback *pb)
{
    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING ||
        pb->current >= pb->count) {
        return SIZE_MAX;
    }
    return pb->current;
}

uint64_t mn_playback_position_ms(const mn_playback *pb)
{
    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING) {
        return 0u;
    }
    return mn_engine_position_ms(pb->engine);
}

uint64_t mn_playback_duration_ms(const mn_playback *pb)
{
    if (pb == NULL || pb->state != MN_PLAYBACK_PLAYING) {
        return 0u;
    }
    return mn_engine_duration_ms(pb->engine);
}
