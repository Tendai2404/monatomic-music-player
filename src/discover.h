/*
 * discover.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Zero-config LAN discovery of the phone (the NEX-GEN Android app).
 *
 * The phone's sync server listens on TCP 8797 for the HTTP sync flow
 * (sync.h §4a) and ALSO runs a UDP responder on the same port number
 * (UDP and TCP port spaces are independent). We broadcast a small
 * ASCII probe ("MONATOMIC_DISCOVER") to the subnet; the phone answers
 * with a one-line JSON telling us who it is and where the TCP sync
 * endpoint lives:
 *
 *     {"app":"nexgen","name":"<device model>","port":8797,
 *      "protocol":1,"hash":"fnv1a64"}
 *
 * so the user never has to type an IP address again.
 *
 * Blocking (~1.5 s worst case) — call from a worker thread only.
 * Windows-only for now (Winsock + iphlpapi); the non-Windows stub
 * always reports "not found".
 *
 * Naming: functions/types use the "mn_" prefix, macros use "MN_".
 */

#ifndef MN_DISCOVER_H
#define MN_DISCOVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port the phone's discovery responder listens on (same number as
 * MN_SYNC_DEFAULT_PORT in sync.h — kept in lockstep with the phone). */
#define MN_DISCOVER_PORT 8797

/* The ASCII probe payload the phone's responder matches on. */
#define MN_DISCOVER_PROBE "MONATOMIC_DISCOVER"

/* One discovery responder, as heard on the wire. */
typedef struct mn_found_device {
    char host[64];   /* IPv4 dotted string — the UDP SENDER address   */
    int  port;       /* TCP sync port from the reply (default 8797)   */
    char model[64];  /* the reply's "name" (Build.MODEL; may be "")   */
    int  protocol;   /* the reply's "protocol" (0 when absent)        */
} mn_found_device;

/*
 * Broadcast-probe the local network and collect EVERY responder.
 *
 * Sends MN_DISCOVER_PROBE to UDP MN_DISCOVER_PORT at 255.255.255.255
 * and at every interface's subnet broadcast address, in probe rounds
 * spread across the window (a lone datagram is too easy to lose), then
 * listens `window_ms` total (clamped to 500..10000, ~2500 is a good
 * ambient default, ~3000 for a user-facing scan). Replies are deduped
 * by sender address and filtered to "app":"nexgen". Returns the number
 * of devices written to `out` (0 on silence or any socket failure) and
 * stops early once `max_out` responders have answered.
 *
 * This is the engine behind BOTH the user-facing "Find devices" scan
 * and the ambient presence tick that keeps registry devices' online
 * state and DHCP-drifted addresses fresh.
 */
int mn_discover_scan(mn_found_device *out, int max_out, int window_ms);

/*
 * Broadcast-probe the local network for ONE phone (first reply wins).
 * Kept for callers that only ever want a single answer fast; today it
 * is a thin wrapper over mn_discover_scan(out, 1, ~1500).
 *
 * On success returns 1 and fills:
 *   out_host  — the phone's IPv4 address as a dotted string (the UDP
 *               sender address, NOT anything claimed in the payload)
 *   out_port  — the TCP sync port from the reply's "port" (default
 *               MN_DISCOVER_PORT when absent)
 *   out_name  — the reply's "name" (device model; may be "")
 * Returns 0 when nothing answered / on any socket failure; the outputs
 * are then zeroed/empty. Any output pointer may be NULL if unwanted.
 *
 * Winsock is initialized/torn down locally (WSAStartup is refcounted
 * by Windows, so this is safe alongside any other user).
 */
int mn_discover_phone(char *out_host, size_t host_n, int *out_port,
                      char *out_name, size_t name_n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_DISCOVER_H */
