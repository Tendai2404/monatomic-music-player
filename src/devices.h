/*
 * devices.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * The SYNC DEVICE REGISTRY: every phone/player the user has paired
 * for library sync (sync.h §4a), persisted across runs. Many devices
 * may be known; exactly ONE is "active" — the target every sync
 * action (sync now, auto-sync, file transfer, presence probes) uses.
 *
 * A device is identified to the USER by its editable display name and
 * to the SYSTEM by a small integer id (never reused within one
 * registry file). The self-reported model string from UDP discovery
 * ("SM-F936B", "RMX5108", …) is what lets background presence scans
 * follow a known device across DHCP address changes: the address is
 * transport detail, the model+registration is the identity the user
 * consented to. Devices are ONLY ever added by an explicit user
 * action — discovery never writes to the registry by itself.
 *
 * This module is pure data + file persistence: no sockets, no
 * threads, no locks. The caller (cef_host) owns one instance and
 * serializes access with its own critical section.
 *
 * Naming: functions/types use the "mn_" prefix, macros use "MN_".
 */

#ifndef MN_DEVICES_H
#define MN_DEVICES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap on registered devices. Generous: this is a personal LAN
 * pairing list, not an inventory system. */
#define MN_DEV_MAX 32

typedef struct mn_device {
    int     id;              /* registry id, > 0, never reused        */
    char    name[64];        /* user-editable display name            */
    char    model[64];       /* self-reported Build.MODEL (may be "") */
    char    host[64];        /* current IPv4 as dotted string         */
    int     port;            /* TCP sync port (default 8797)          */
    int64_t last_seen_ms;    /* epoch ms of last discovery sighting   */
    int64_t last_sync_ms;    /* epoch ms of last SUCCESSFUL sync      */
    char    last_result[96]; /* short outcome of the last sync try    */
    int     was_online;      /* RUNTIME ONLY (never persisted): the
                                presence scanner's previous online
                                verdict, for edge-triggered seen/lost
                                activity-log entries */
} mn_device;

typedef struct mn_devreg {
    mn_device dev[MN_DEV_MAX];
    int       count;
    int       active_id;     /* 0 = no device selected                */
    int       next_id;       /* next id to hand out (starts at 1)     */
} mn_devreg;

/* Reset to the empty registry (no devices, nothing active). */
void mn_devreg_init(mn_devreg *r);

/*
 * Load/save the registry file. The format is the sync-dir line style
 * (host.txt's "a|b" convention, extended):
 *     A|<active_id>|<next_id>
 *     D|id|port|last_seen_ms|last_sync_ms|host|name|model|last_result
 * One D line per device; last_result is the REST of the line (it may
 * contain spaces and punctuation; '|' is stripped from all free-text
 * fields on write). Load tolerates a missing file (returns false,
 * registry left empty) and skips malformed lines. Save is
 * write-whole-file (small: <= MN_DEV_MAX short lines).
 */
bool mn_devreg_load(mn_devreg *r, const char *path);
bool mn_devreg_save(const mn_devreg *r, const char *path);

/* Lookups. All return NULL / -1 style "not found" rather than assert. */
mn_device *mn_devreg_find(mn_devreg *r, int id);
mn_device *mn_devreg_find_host(mn_devreg *r, const char *host, int port);
mn_device *mn_devreg_active(mn_devreg *r);

/*
 * Add a device (user action only — see the header comment). name may
 * be "" (falls back to model, then host). Free-text fields are
 * sanitized ('|' and control chars dropped). Refuses duplicates by
 * host:port (returns the existing entry instead — adding the same
 * phone twice is always a misclick) and refuses a full registry
 * (returns NULL). The FIRST device added to an empty registry
 * becomes active automatically; later adds never steal selection.
 */
mn_device *mn_devreg_add(mn_devreg *r, const char *name, const char *model,
                         const char *host, int port);

/* Remove by id. Clears active_id if it pointed at the removed row.
 * Returns false when the id is unknown. */
bool mn_devreg_remove(mn_devreg *r, int id);

/* Copy a sanitized string into a device free-text/host field. */
void mn_devreg_set_text(char *dst, size_t dst_n, const char *src);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MN_DEVICES_H */
