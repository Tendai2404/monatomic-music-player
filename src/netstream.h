/*
 * netstream.h — Monatomic Music Player
 * ------------------------------------------------------------------
 * Progressive HTTP(S) audio reader for internet radio and podcast
 * streaming: WinHTTP fill thread -> ring buffer -> blocking reads that
 * plug straight into ma_decoder's onRead/onSeek callbacks.
 *
 *  - ICY (Icecast/SHOUTcast) metadata: sends "Icy-MetaData: 1" when
 *    asked, strips the interleaved metadata blocks out of the audio
 *    byte stream and exposes the latest StreamTitle with a change
 *    sequence number the poller can diff against.
 *  - Seek: HTTP Range re-request — only when the server advertised a
 *    Content-Length (podcast enclosures). Live mounts report
 *    unseekable and length -1.
 *  - Reconnect: a dropped live connection retries 3x with 1s/2s/4s
 *    backoff (seekable streams resume with Range at the byte where
 *    the connection died).
 *
 * Thread contract: one reader (the engine/device thread inside the
 * decoder callbacks) + one internal fill thread. mn_netstream_title /
 * _station_name / _content_type may be called from any thread.
 */
#ifndef MN_NETSTREAM_H
#define MN_NETSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mn_netstream mn_netstream;

/* Open `url` (http/https). `want_icy` requests ICY metadata (radio).
 * Blocks for connect + response headers (~8s timeout). On failure
 * returns NULL and, when `err` is given, a short reason in it. */
mn_netstream *mn_netstream_open(const char *url, bool want_icy,
                                char *err, size_t err_cap);

/* Blocking read of up to `n` bytes. Returns bytes read; 0 = end of
 * stream / unrecoverable network failure (after internal retries). */
size_t mn_netstream_read(mn_netstream *ns, void *dst, size_t n);

/* Absolute seek (SEEK_SET only). Returns false when the stream is not
 * seekable (live mount). On success subsequent reads continue at
 * `off` via an HTTP Range re-request. */
bool mn_netstream_seek(mn_netstream *ns, int64_t off);

/* Absolute byte position of the NEXT read. */
int64_t mn_netstream_tell(mn_netstream *ns);

/* Total byte length from Content-Length, or -1 (live / unknown). */
int64_t mn_netstream_length(mn_netstream *ns);

bool mn_netstream_seekable(mn_netstream *ns);

/* Latest ICY StreamTitle. Returns true and bumps *seq (in/out: pass
 * the last seq you saw) only when the title changed since. */
bool mn_netstream_title(mn_netstream *ns, char *out, size_t cap,
                        uint32_t *seq);

/* icy-name response header (station name), "" when absent. */
const char *mn_netstream_station_name(mn_netstream *ns);

/* Lowercased Content-Type response header, "" when absent. */
const char *mn_netstream_content_type(mn_netstream *ns);

/* Block until `bytes` are buffered (pre-roll) or `timeout_ms` passes
 * or the stream ends. Returns buffered byte count. */
size_t mn_netstream_wait_buffered(mn_netstream *ns, size_t bytes,
                                  uint32_t timeout_ms);

void mn_netstream_close(mn_netstream *ns);

#ifdef __cplusplus
}
#endif

#endif /* MN_NETSTREAM_H */
