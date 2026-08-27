/*
 * control.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Minimal LAN remote-control listener: the desktop mirror of the phone's
 * /control/* surface (NEX-GEN ControlApi), so the phone can drive THIS
 * player. Deliberately scoped to transport only:
 *
 *   GET  /control/ping                → {"app":"monatomic","control":1}
 *                                       (unauthenticated reachability probe)
 *   GET  /control/status              → now-playing + transport state
 *   POST /control/play|pause|stop|toggle
 *   POST /control/next|prev|previous
 *   POST /control/seek?ms=<abs>       → absolute position
 *   POST /control/seekby?ms=<delta>
 *   POST /control/volume?v=<0..1>
 *
 * Security model copied from the phone's ControlApi: every request except
 * /control/ping must carry `X-Auth-Token`, compared in constant time
 * against the token the host minted at first start. The token reaches the
 * phone over the existing sync channel (the snapshot the desktop pushes to
 * its REGISTERED device carries a "control" block) — pairing rides the
 * trust the user already established by adding the device.
 *
 * The module is UI-free and app-free: the host supplies callbacks. All
 * socket work happens on one internal listener thread; callbacks are
 * invoked ON that thread (mn_app_* transport calls are thread-safe — they
 * take the app's lib_lock internally).
 */

#ifndef MN_CONTROL_H
#define MN_CONTROL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default TCP port of the desktop control listener — same number the
 * ecosystem already uses for the phone's sync server (different machine,
 * no clash). The phone learns the real port from the sync "control"
 * block, so this is a default, not a contract. */
#define MN_CONTROL_DEFAULT_PORT 8797

typedef struct mn_control_env {
    void *user;
    /* Build a malloc'd JSON object string of the now-playing status
     * (caller of the callback frees it). Return NULL on failure. */
    char *(*status)(void *user);
    /* Execute one transport command ("play","pause","stop","toggle",
     * "next","prev","seek","seekby","volume"). `arg` is the parsed
     * numeric query value when `has_arg`. Return false for unknown
     * commands or bad arguments. */
    bool (*command)(void *user, const char *name, double arg, bool has_arg);
    /* Optional: called once per remote session (first authenticated
     * command from a client address in a while) — activity-log hook. */
    void (*session)(void *user, const char *client_ip);
    /* Optional thread lifecycle hooks, invoked on the listener thread at
     * its start and just before it exits — the host maps these onto its
     * worker_enter/worker_leave bracket so the thread's per-thread library
     * reader connection is released on shutdown like every other worker. */
    void (*thread_begin)(void *user);
    void (*thread_end)(void *user);
} mn_control_env;

/* Start the listener on `port` (0 -> MN_CONTROL_DEFAULT_PORT). `token` is
 * the shared secret (required, non-empty). Returns false when the socket
 * can't be bound. Idempotent: a second start is a no-op returning true. */
bool mn_control_start(const mn_control_env *env, int port, const char *token);

/* Stop the listener and join its thread (bounded wait). Safe to call
 * without a prior start. */
void mn_control_stop(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_CONTROL_H */
