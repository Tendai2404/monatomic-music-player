/*
 * Monatomic (C) — CEF (Chromium Embedded Framework) host implementation.
 *
 * Replaces the WebView2 host with a bundled, OPEN Chromium backend. Renders the
 * HTML/CSS/JS UI (ui/index.html) inside a CEF browser and bridges JS<->C over the
 * same JSON protocol the WebView2 host used (window.chrome.webview.postMessage /
 * addEventListener('message')). No chrome.webview exists in CEF, so a small JS
 * shim is injected on every page load that maps:
 *
 *     chrome.webview.postMessage(json)  ->  window.__mn_send(json)
 *     chrome.webview._emit(obj)         ->  dispatched to addEventListener('message')
 *
 * __mn_send is a native V8 function bound in the RENDER process. It packages
 * the JSON string into a cef_process_message and sends it to the BROWSER process.
 * The browser process (cef_client on_process_message_received) parses the command,
 * calls the matching app_* function, builds a JSON reply, and delivers it back to
 * JS by executing  window.chrome.webview._emit(<reply>)  in the frame.
 *
 * CEF C API: every framework struct begins with a cef_base_ref_counted_t and is
 * driven through function pointers (This->member(This, args...)). Client-side
 * handler structs (app / client / life-span / render-process / v8 handler) are
 * hand-rolled here with an atomic reference count. DLL-side objects (browser,
 * frame, list_value, ...) are used through their vtable and released when done.
 *
 * Multi-process: CEF re-launches THIS SAME exe for its sub-processes. webview_run
 * calls cef_execute_process first; for a sub-process that returns >= 0 and we
 * return it immediately. The browser process then runs cef_initialize +
 * cef_run_message_loop.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>   /* IFileDialog (modern folder picker) */
#include <shellapi.h>   /* ShellExecuteW (reveal in Explorer) */
#include <winhttp.h>    /* online cover-art fetch (iTunes/Deezer)  */
#include <dxgi.h>       /* GPU name + VRAM for the hwcaps probe    */
#include <intrin.h>     /* __cpuid for AVX-512 detection           */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CEF C API. Included as "include/capi/..." — build with /I vendor\cef so these
 * cross-references resolve. */
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_render_process_handler_capi.h"
#include "include/capi/cef_browser_process_handler_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_frame_capi.h"
#include "include/capi/cef_process_message_capi.h"
#include "include/capi/cef_values_capi.h"
#include "include/capi/cef_v8_capi.h"
#include "include/capi/cef_task_capi.h"   /* cef_post_task / cef_task_t (TID_UI) */
#include "include/capi/cef_display_handler_capi.h"
#include "include/internal/cef_string.h"
#include "include/cef_api_hash.h"   /* cef_api_hash() — configures the API version */

#include "app.h"
#include "audio_engine.h"   /* mn_engine_waveform() for the waveform command */
#include "dsp.h"            /* MN_DSP_EQ_BANDS, band freq + preset names (EQ) */
#include "artcache.h"
#include "tags_write.h"     /* mn_tag_edit + base64 for tagwrite/artwrite */
#include "depth.h"          /* cover-art depth maps for the volumetric art */
#include "stems.h"          /* offline stem separation for the export feature */
#include "audio_write.h"    /* WAV/FLAC/MP3 writers for stem export */
#include "stempack.h"       /* .mnstem ZIP container writer */

/* Implementation lives in artcache.c; here we only need the declarations. */
#include "stb_image_write.h"

#include "cef_host.h"

/* ------------------------------------------------------------------------- */
/* CEF string helpers (default string type is UTF-16 / char16_t).            */
/* ------------------------------------------------------------------------- */

/* Set a cef_string_t (UTF-16) from a UTF-8 C string (copying). */
static void cefstr_from_utf8(cef_string_t *out, const char *utf8) {
    memset(out, 0, sizeof(*out));
    if (!utf8) return;
    cef_string_utf8_to_utf16(utf8, strlen(utf8), out);
}

/* Set a cef_string_t from an ASCII constant. */
static void cefstr_from_ascii(cef_string_t *out, const char *ascii) {
    memset(out, 0, sizeof(*out));
    if (!ascii) return;
    cef_string_ascii_to_utf16(ascii, strlen(ascii), out);
}

/* Convert a cef_string_t (UTF-16) to a freshly malloc'd UTF-8 buffer. Caller
 * frees. Returns NULL on failure / empty. */
static char *utf8_from_cefstr16(const cef_string_t *s) {
    if (!s || !s->str || s->length == 0) return NULL;
    cef_string_utf8_t u8;
    memset(&u8, 0, sizeof(u8));
    if (!cef_string_utf16_to_utf8(s->str, s->length, &u8) || !u8.str) {
        cef_string_utf8_clear(&u8);
        return NULL;
    }
    char *out = (char *)malloc(u8.length + 1);
    if (out) {
        memcpy(out, u8.str, u8.length);
        out[u8.length] = 0;
    }
    cef_string_utf8_clear(&u8);
    return out;
}

/* Convert a cef_string_userfree_t (UTF-16, owned) to malloc'd UTF-8 and free the
 * userfree. Returns NULL on failure. */
static char *utf8_from_userfree(cef_string_userfree_t uf) {
    if (!uf) return NULL;
    char *out = utf8_from_cefstr16(uf);
    cef_string_userfree_free(uf);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Globals. The browser-process CEF callbacks all run on the main UI thread   */
/* (single_process-friendly: multi_threaded_message_loop=0 + run_message_loop),*/
/* so plain globals are fine for browser-side state.                          */
/* ------------------------------------------------------------------------- */

static mn_app          *g_app     = NULL;   /* app controller (browser process) */
static cef_browser_t   *g_browser = NULL;   /* owned ref, released on close      */
static HWND             g_host_hwnd = NULL; /* top-level host window            */
static HINSTANCE        g_host_hinst = NULL;/* module instance for the host win  */

/* System-integration state (media hotkeys + taskbar thumbnail controls). */
#define MN_HOTKEY_PLAYPAUSE 0xB301   /* RegisterHotKey ids                     */
#define MN_HOTKEY_NEXT      0xB302
#define MN_HOTKEY_PREV      0xB303
#define MN_HOTKEY_STOP      0xB304
/* Taskbar thumbnail-toolbar buttons (ITaskbarList3). NO tray icon: the app
 * minimizes to the TASKBAR like a normal window. (The old minimize-to-tray
 * hid the window entirely; users then clicked the pinned taskbar shortcut,
 * launching a SECOND instance that fought the first over the shared CEF
 * cache and fell into a bare chrome-bootstrap window. See the per-instance
 * cache slots in main for the other half of that fix.) */
#define MN_THB_PREV  0xB320
#define MN_THB_PLAY  0xB321
#define MN_THB_NEXT  0xB322
static ITaskbarList3 *g_taskbar   = NULL;
static UINT           g_tbc_msg   = 0;      /* "TaskbarButtonCreated"        */
static HICON          g_thb_ico[4] = {0};   /* prev, play, pause, next       */
static BOOL           g_thb_added = FALSE;
static const CLSID mn_CLSID_TaskbarList =
    {0x56FDF344,0xFD6D,0x11d0,{0x95,0x8A,0x00,0x60,0x97,0xC9,0xA0,0x90}};
static const IID mn_IID_ITaskbarList3 =
    {0xEA1AFB91,0x9E28,0x4B86,{0x90,0xE9,0x9E,0x9F,0x8A,0x5E,0xEF,0xAF}};

/* With multi_threaded_message_loop=1, CEF runs its own dedicated UI thread and
 * our Win32 loop runs on the main thread. g_browser is written on the UI thread
 * (life-span on_after_created) and read on the main thread (WM_SIZE/WM_CLOSE),
 * so guard it with a critical section. */
static CRITICAL_SECTION g_browser_lock;

static char  g_ui_dir[1200]  = {0};   /* folder with index.html            */
static char  g_art_dir[1200] = {0};   /* album-art cache (.rgba thumbs)     */
static char  g_webroot[1300] = {0};   /* mapped folder: fonts/ + art/       */
static char  g_webart[1400]  = {0};   /* LEGACY g_webroot\art (v1 mirror) —
                                       * kept only for cacheinfo display and
                                       * the one-time v2 reclamation delete */
static char  g_exe_path[1200] = {0};  /* absolute path to this exe          */
static char  g_exe_dir[1200]  = {0};  /* directory containing the exe       */

#define NE_TICK_TIMER_ID  1
#define NE_TICK_MS        100
#define NE_ARTHEAL_TIMER_ID 2      /* one-shot post-launch art self-heal      */
#define NE_ARTHEAL_MS       2000
#define NE_DEPTHHEAL_TIMER_ID 3    /* recurring depth-map self-heal sweep     */
#define NE_DEPTHHEAL_MS   (5 * 60 * 1000)
#define NE_SIZEMOVE_TICK_ID 4      /* keeps mn_app_tick alive during a drag   */

/* Process-message name for the JS->C command channel. */
#define NE_MSG_CMD  "mn_cmd"

/* ------------------------------------------------------------------------- */
/* Small string buffer for building JSON replies (ported from webview_host). */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} strbuf;

static void sb_init(strbuf *b) {
    b->cap = 4096;
    b->len = 0;
    b->oom = false;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = 0; else b->oom = true;
}

static void sb_free(strbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool sb_reserve(strbuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t ncap = b->cap;
    while (b->len + extra + 1 > ncap) {
        if (ncap > (SIZE_MAX / 2)) { b->oom = true; return false; }
        ncap *= 2;
    }
    char *nd = (char *)realloc(b->data, ncap);
    if (!nd) { b->oom = true; return false; }
    b->data = nd;
    b->cap  = ncap;
    return true;
}

static void sb_putn(strbuf *b, const char *s, size_t n) {
    if (!sb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sb_puts(strbuf *b, const char *s) {
    if (!s) return;
    sb_putn(b, s, strlen(s));
}

static void sb_putc(strbuf *b, char c) {
    if (!sb_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len]   = 0;
}

/* JSON string value INCLUDING surrounding quotes, escaped per RFC 8259. */
static void sb_json_str(strbuf *b, const char *s) {
    sb_putc(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            unsigned char c = *p;
            switch (c) {
                case '"':  sb_putn(b, "\\\"", 2); break;
                case '\\': sb_putn(b, "\\\\", 2); break;
                case '\b': sb_putn(b, "\\b", 2);  break;
                case '\f': sb_putn(b, "\\f", 2);  break;
                case '\n': sb_putn(b, "\\n", 2);  break;
                case '\r': sb_putn(b, "\\r", 2);  break;
                case '\t': sb_putn(b, "\\t", 2);  break;
                default:
                    if (c < 0x20) {
                        char u[8];
                        snprintf(u, sizeof(u), "\\u%04x", c);
                        sb_putn(b, u, 6);
                    } else {
                        sb_putc(b, (char)c);
                    }
            }
        }
    }
    sb_putc(b, '"');
}

static void sb_json_i64(strbuf *b, int64_t v) {
    char t[32]; snprintf(t, sizeof(t), "%lld", (long long)v); sb_puts(b, t);
}
static void sb_json_int(strbuf *b, int v) {
    char t[16]; snprintf(t, sizeof(t), "%d", v); sb_puts(b, t);
}
static void sb_json_u32(strbuf *b, uint32_t v) {
    char t[16]; snprintf(t, sizeof(t), "%u", v); sb_puts(b, t);
}
static void sb_json_bool(strbuf *b, bool v) {
    sb_puts(b, v ? "true" : "false");
}
static void sb_json_float(strbuf *b, float v) {
    if (v != v || v > 3.0e38f || v < -3.0e38f) { sb_puts(b, "0"); return; }
    char t[32]; snprintf(t, sizeof(t), "%.4f", (double)v); sb_puts(b, t);
}

/* ------------------------------------------------------------------------- */
/* Minimal hand-rolled JSON extractor for incoming {cmd:...} messages         */
/* (ported from webview_host).                                                */
/* ------------------------------------------------------------------------- */

static const char *json_find_value(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        if (p > json && p[-1] == '"' && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                return q;
            }
        }
        p += klen;
    }
    return NULL;
}

static int64_t json_get_i64(const char *json, const char *key, int64_t dflt) {
    const char *v = json_find_value(json, key);
    if (!v) return dflt;
    if (*v == '"') v++;
    char *end = NULL;
    long long r = strtoll(v, &end, 10);
    if (end == v) return dflt;
    return (int64_t)r;
}

static bool json_get_bool(const char *json, const char *key, bool dflt) {
    const char *v = json_find_value(json, key);
    if (!v) return dflt;
    if (*v == '"') v++;
    if (strncmp(v, "true", 4) == 0)  return true;
    if (strncmp(v, "false", 5) == 0) return false;
    if (*v == '1') return true;
    if (*v == '0') return false;
    return dflt;
}

static double json_get_double(const char *json, const char *key, double dflt) {
    const char *v = json_find_value(json, key);
    if (!v) return dflt;
    if (*v == '"') v++;
    char *end = NULL;
    double r = strtod(v, &end);
    if (end == v) return dflt;
    return r;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_n) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"' || out_n == 0) { if (out_n) out[0] = 0; return false; }
    v++;
    size_t o = 0;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char e = *v++;
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case '/':  c = '/';  break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    unsigned cp = 0; int ok = 1;
                    for (int i = 0; i < 4; i++) {
                        char h = *v;
                        if      (h >= '0' && h <= '9') cp = (cp << 4) + (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp = (cp << 4) + (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp = (cp << 4) + (unsigned)(h - 'A' + 10);
                        else { ok = 0; break; }
                        v++;
                    }
                    if (!ok) continue;
                    if (cp < 0x80) {
                        if (o + 1 < out_n) out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o + 2 < out_n) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        if (o + 3 < out_n) {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;
                }
                default:   c = e; break;
            }
        }
        if (o + 1 < out_n) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

/* Like json_get_str but heap-allocated, for values of unbounded length
 * (lyrics text, base64 cover art). Returns a malloc'd UTF-8 string (caller
 * frees) or NULL when the key is absent / not a string / OOM. */
static char *json_get_str_alloc(const char *json, const char *key) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') return NULL;
    v++;

    /* Measure the raw (escaped) span; unescaping never grows it. */
    const char *e = v;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    size_t raw = (size_t)(e - v);
    char *out = (char *)malloc(raw + 1);
    if (!out) return NULL;

    size_t o = 0;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char esc = *v++;
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case '/':  c = '/';  break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    unsigned cp = 0; int okhex = 1;
                    for (int i = 0; i < 4; i++) {
                        char h = *v;
                        if      (h >= '0' && h <= '9') cp = (cp << 4) + (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp = (cp << 4) + (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp = (cp << 4) + (unsigned)(h - 'A' + 10);
                        else { okhex = 0; break; }
                        v++;
                    }
                    if (!okhex) continue;
                    if (cp < 0x80) {
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    continue;
                }
                default:   c = esc; break;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------------------------- */
/* Depth-map worker: one LOW-priority background thread turns webroot art     */
/* PNGs into "<png minus .png>.depth.png" grayscale depth maps consumed by    */
/* the UI's volumetric cover mesh (ui/depthart.js). CPU-only inference        */
/* (~0.9 s/cover, serialized inside depth.c); NEVER runs on the CEF UI or     */
/* audio threads — art_url_for only ENQUEUES. Small fixed queue: drop-if-full */
/* (a later browse re-enqueues), skip-if-exists, model loaded lazily once.    */
/* ------------------------------------------------------------------------- */

#define NE_DEPTH_QUEUE_CAP 256

typedef struct {
    char png[1600];
    char depth[1620];
    char artist[256];   /* for hi-res cover resolution (may be empty)      */
    char album[256];
} ne_depth_job;

static ne_depth_job     g_depth_q[NE_DEPTH_QUEUE_CAP];
static int              g_depth_q_head = 0;
static int              g_depth_q_len  = 0;
static CRITICAL_SECTION g_depth_lock;
static HANDLE           g_depth_event  = NULL;  /* work queued / stop     */
static HANDLE           g_depth_thread = NULL;
static volatile LONG    g_depth_stop   = 0;
static mn_depth        *g_depth        = NULL;  /* lazy; worker-owned     */
static volatile LONG    g_depth_off    = 0;     /* model missing => off   */
/* When the depth session fails to build, retry after this tick instead of
 * latching off forever — a transient ORT/file hiccup at startup used to kill
 * depth generation for the whole run ("some albums never get 3D"). */
static volatile LONG64  g_depth_retry_at = 0;
/* Depth pipeline status for the UI pill: 0 standby, 1 model loading,
 * 2 ready, 3 failed (retry pending), 4 generating. busy/done are advisory
 * (torn reads are harmless for display). */
static volatile LONG    g_depth_state  = 0;
static volatile LONG    g_depth_done_n = 0;
static volatile LONG    g_depth_fail_n = 0;   /* consecutive generate fails */
static char             g_depth_busy[256];
/* Background pre-generation gate (Settings). OFF = depth maps are generated
 * ON DEMAND only: the now-playing album (priority) + explicit per-album
 * regenerate. Browsing and self-heal sweeps stop feeding the queue. */
static volatile LONG    g_depth_batch  = 0;
static volatile LONG    g_watch_folders = 1;   /* live folder monitoring */

/* Forward declarations for helpers defined below dispatch_command. */
static void depth_clear_maps(void);
static void depth_selfheal_sweep(void);
static int  mn_https_get(const wchar_t *host, const wchar_t *path,
                         char **out, int max_len);
static int  mn_https_download(const char *url, const char *file);
static void mn_url_encode(const char *s, char *out, size_t n);
static bool mn_json_find_str(const char *json, const char *key,
                             char *out, size_t n);
static void mn_dir_stats(const char *dir, int depth,
                         int64_t *bytes, int64_t *files);
static void mn_dir_delete_matching(const char *dir, const char *pattern);
static void mn_dir_trim(const char *dir, const char *pattern, int64_t cap_bytes);
static void mn_dir_pattern_stats(const char *dir, const char *pattern,
                                 int64_t *bytes, int64_t *files);
static void dispatch_command(cef_frame_t *frame, const char *json);
static void artscan_selfheal_start(void);
static void art_integrity_kick(int64_t limit);         /* art-integrity subsystem */
static volatile LONG g_artverify_missing;              /* last sweep: missing count */
static volatile LONG g_artverify_total;                /* last sweep: albums checked */
/* async bridge workers (definitions live below with post_emit) */
static void cacheop_start(cef_frame_t *frame_owned, const char *which_or_null);
static void artfetch_start(cef_frame_t *frame_owned, const char *artist,
                           const char *album, int res);
static void roots_start(cef_frame_t *frame_owned);
static void roots_file_touch(const char *path, const char *kind, bool remove);
static void sync_audiobook_roots(void);
static void kind_for_path(const char *path, char *out, size_t cap);
static int  kind_is_music(const char *k);   /* defined with the roots-file IO below */
static void build_kinds(strbuf *b);
/* per-book resume + per-kind listen stats (defined near the roots file IO) */
#define NE_MAX_BOOKS 512
typedef struct { long long album, track, pos, updated; } ne_book_line;
static int  books_read(ne_book_line *out, int max);
static void datafile_path(char *out, size_t n, const char *name);
static void book_progress_tick(void);
static void playlist_export_start(cef_frame_t *frame, int64_t id, const char *name);
static void stemexport_start(cef_frame_t *frame, const char *json);
static void post_emit_owned(cef_frame_t *frame_owned, char *json_heap);
static cef_frame_t *sync_grab_frame(void);   /* owned main-frame ref or NULL */
static void sync_start(cef_frame_t *frame_owned);
static void syncfile_start(cef_frame_t *frame_owned, bool import,
                           const char *path_or_null);
static void purgemissing_start(cef_frame_t *frame_owned);
static void deletetracks_start(cef_frame_t *frame_owned, const char *json);
static void backupnow_start(cef_frame_t *frame_owned);
static void reinfer_start(cef_frame_t *frame_owned);
static void sync_emit_status(cef_frame_t *frame_owned, const char *state,
                             int applied, int skipped, int pushed,
                             const char *error);
static bool db_backup_rotate(bool force);
static bool depth_generate_guarded(mn_depth *d, const char *src,
                                   const char *dst);
static volatile LONG    g_depth_reload = 0;     /* depth model changed     */
static char             g_data_dir[1300] = {0}; /* %APPDATA%\Monatomic    */

/* --------------------------------------------------------------------------
 * Worker lifecycle. Every fire-and-forget worker thread brackets its body
 * with worker_enter()/worker_leave() so shutdown can DRAIN them: quitting
 * mid-activity (the boot art self-heal, a sync, a cache clear) used to race
 * mn_app_destroy / DeleteCriticalSection / cef_shutdown -> crash on exit.
 * worker_leave also releases the thread's TLS SQLite reader connection —
 * without it every short-lived worker leaked one (each pinning up to a
 * 64 MiB page cache) into the readers list until process exit.
 * -------------------------------------------------------------------------- */
static volatile LONG g_shutting_down = 0;
static volatile LONG g_workers_live  = 0;

static void worker_enter(void) { InterlockedIncrement(&g_workers_live); }
static void worker_leave(void) {
    if (g_app) mn_app_thread_detach(g_app);
    InterlockedDecrement(&g_workers_live);
}

/* Bounded drain before teardown: give in-flight workers up to ~8 s. */
static void workers_drain(void) {
    int i;
    InterlockedExchange(&g_shutting_down, 1);
    Sleep(100);   /* grace: just-created threads reach worker_enter() */
    for (i = 0; i < 160; ++i) {
        if (InterlockedCompareExchange(&g_workers_live, 0, 0) == 0) return;
        Sleep(50);
    }
    fprintf(stderr, "[shutdown] %ld worker(s) still live after drain window\n",
            (long)InterlockedCompareExchange(&g_workers_live, 0, 0));
}

/* ------------------------------------------------------------------------- */
/* Library sync (phone <-> desktop, SYNC_PROTOCOL v1) — host-side state.      */
/* Persisted under %APPDATA%\Monatomic\sync\ (host.txt "host|port", auto.txt  */
/* "1"/"0", last.txt epoch-ms of the last success) so it survives restarts.   */
/* Every state change emits the STATUS EVENT CONTRACT the UI binds against:   */
/*   {"type":"sync","state":"idle|connecting|pulling|merging|pushing|done|    */
/*    error","host":"<host:port or empty>","auto":bool,"last_ms":N,           */
/*    "applied":N,"skipped":M,"pushed":K,"error":"<message or empty>"}        */
/* ------------------------------------------------------------------------- */

#define NE_SYNC_DEFAULT_PORT 8797            /* the phone server's default  */
#define NE_SYNC_AUTO_MIN_MS  (10 * 60 * 1000) /* auto: only when staler      */

static CRITICAL_SECTION g_sync_cs;           /* guards host/port/state      */
static char             g_sync_host[128] = {0};
static int              g_sync_port = NE_SYNC_DEFAULT_PORT;
static char             g_sync_state[16] = "idle";  /* last emitted state   */
static volatile LONG    g_sync_auto = 0;     /* auto-sync opt-in            */
static volatile LONG    g_sync_busy = 0;     /* single-flight guard         */
static volatile LONG64  g_sync_last_ms = 0;  /* epoch ms of last success    */

/* Wall-clock epoch milliseconds (GetSystemTimeAsFileTime-derived). */
static int64_t sync_now_ms(void) {
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100 ns ticks since 1601 -> ms since 1970. */
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

/* Build "<data_dir>\sync\<leaf>" (creating the sync dir, best-effort). */
static void sync_file_path(const char *leaf, char *out, size_t n) {
    char dir[1400];
    snprintf(dir, sizeof(dir), "%s\\sync", g_data_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\%s", dir, leaf);
}

static void sync_state_save_host(void) {
    char path[1500];
    FILE *f;
    sync_file_path("host.txt", path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        EnterCriticalSection(&g_sync_cs);
        fprintf(f, "%s|%d\n", g_sync_host, g_sync_port);
        LeaveCriticalSection(&g_sync_cs);
        fclose(f);
    }
}

static void sync_state_save_auto(void) {
    char path[1500];
    FILE *f;
    sync_file_path("auto.txt", path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", InterlockedCompareExchange(&g_sync_auto, 0, 0) ? 1 : 0);
        fclose(f);
    }
}

static void sync_state_save_last(void) {
    char path[1500];
    FILE *f;
    sync_file_path("last.txt", path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lld\n",
                (long long)InterlockedCompareExchange64(&g_sync_last_ms, 0, 0));
        fclose(f);
    }
}

/* Per-field participation bitmask (1=likes 2=ratings 4=plays; default all)
 * and the auto-sync interval in minutes. Persisted as fields.txt "mask|min". */
static volatile LONG g_sync_fields       = 7;
static volatile LONG g_sync_interval_min = 10;

static void sync_fields_apply_to_app(void) {
    LONG m = InterlockedCompareExchange(&g_sync_fields, 0, 0);
    if (g_app) mn_app_set_sync_fields(g_app, (m & 1) != 0, (m & 2) != 0,
                                      (m & 4) != 0);
}

static void sync_state_save_fields(void) {
    char path[1500];
    FILE *f;
    sync_file_path("fields.txt", path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld|%ld\n",
                (long)InterlockedCompareExchange(&g_sync_fields, 0, 0),
                (long)InterlockedCompareExchange(&g_sync_interval_min, 0, 0));
        fclose(f);
    }
}

/* Replay the persisted host/auto/last state. Called once at startup (after
 * setup_webroot resolves g_data_dir, before any worker can race it). */
static void sync_state_load(void) {
    char path[1500], line[256];
    FILE *f;

    sync_file_path("host.txt", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            char *bar = strchr(line, '|');
            if (bar) {
                int port = atoi(bar + 1);
                *bar = '\0';
                /* trim trailing whitespace off the host half */
                {
                    size_t n = strlen(line);
                    while (n > 0 && (unsigned char)line[n - 1] <= 0x20) line[--n] = '\0';
                }
                snprintf(g_sync_host, sizeof(g_sync_host), "%s", line);
                g_sync_port = (port > 0 && port <= 65535) ? port
                                                          : NE_SYNC_DEFAULT_PORT;
            }
        }
        fclose(f);
    }
    sync_file_path("auto.txt", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f) && atoi(line) != 0) {
            InterlockedExchange(&g_sync_auto, 1);
        }
        fclose(f);
    }
    sync_file_path("last.txt", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            long long ms = atoll(line);
            if (ms > 0) InterlockedExchange64(&g_sync_last_ms, ms);
        }
        fclose(f);
    }
    sync_file_path("fields.txt", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            char *bar = strchr(line, '|');
            long  m   = atol(line);
            if (m >= 0 && m <= 7) InterlockedExchange(&g_sync_fields, m);
            if (bar) {
                long mins = atol(bar + 1);
                if (mins >= 5 && mins <= 24 * 60)
                    InterlockedExchange(&g_sync_interval_min, mins);
            }
        }
        fclose(f);
    }
    sync_fields_apply_to_app();
}

/* Build the {"type":"sync",...} status event (the exact UI contract). */
static void sync_status_json(strbuf *b, const char *state,
                             int applied, int skipped, int pushed,
                             const char *error) {
    char hostport[160] = {0};
    EnterCriticalSection(&g_sync_cs);
    if (g_sync_host[0]) {
        snprintf(hostport, sizeof(hostport), "%s:%d", g_sync_host, g_sync_port);
    }
    LeaveCriticalSection(&g_sync_cs);
    sb_puts(b, "{\"type\":\"sync\",\"state\":");
    sb_json_str(b, state);
    sb_puts(b, ",\"host\":");
    sb_json_str(b, hostport);
    sb_puts(b, ",\"auto\":");
    sb_json_bool(b, InterlockedCompareExchange(&g_sync_auto, 0, 0) != 0);
    sb_puts(b, ",\"last_ms\":");
    sb_json_i64(b, (int64_t)InterlockedCompareExchange64(&g_sync_last_ms, 0, 0));
    sb_puts(b, ",\"applied\":");
    sb_json_int(b, applied);
    sb_puts(b, ",\"skipped\":");
    sb_json_int(b, skipped);
    sb_puts(b, ",\"pushed\":");
    sb_json_int(b, pushed);
    /* per-field toggles + auto interval so the settings UI can populate */
    {
        LONG m = InterlockedCompareExchange(&g_sync_fields, 0, 0);
        sb_puts(b, ",\"f_likes\":");   sb_json_bool(b, (m & 1) != 0);
        sb_puts(b, ",\"f_ratings\":"); sb_json_bool(b, (m & 2) != 0);
        sb_puts(b, ",\"f_plays\":");   sb_json_bool(b, (m & 4) != 0);
        sb_puts(b, ",\"interval\":");
        sb_json_int(b, (int)InterlockedCompareExchange(&g_sync_interval_min, 0, 0));
    }
    sb_puts(b, ",\"error\":");
    sb_json_str(b, error ? error : "");
    sb_putc(b, '}');
}

static bool file_present_nonempty(const char *path);   /* defined below */

/* Attribution ledger for depth_publish_hires (the second hires writer beside
 * artcache's mn_art_ensure_hires/ingest): count + bytes of "<hash>.hires.png"
 * copies published this session, read by the heal-tick summary line. */
static volatile LONG64 g_hirespub_files = 0;
static volatile LONG64 g_hirespub_bytes = 0;

/* Copy the art-cache hi-res cover to a webart sibling of the served PNG so the
 * UI can load a crisp mesh texture. For "…\<hash>.png" the sibling is
 * "…\<hash>.hires.png". No-op if a valid (non-empty) destination exists. */
static void depth_publish_hires(const char *png, const char *hires_src) {
    char dst[1620];
    size_t n;
    if (!png || !hires_src || !hires_src[0]) return;
    n = strlen(png);
    if (n < 5) return;   /* need "*.png" */
    snprintf(dst, sizeof(dst), "%.*s.hires.png", (int)(n - 4), png);
    if (file_present_nonempty(dst)) return;   /* a torn 0-byte copy is NOT valid */
    {
        char tmp[1700];
        snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", dst,
                 (unsigned long)GetCurrentThreadId());
        if (CopyFileA(hires_src, tmp, FALSE)) {
            if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileA(tmp);
            } else {
                WIN32_FILE_ATTRIBUTE_DATA fad;
                LONG64 sz = 0;
                if (GetFileAttributesExA(dst, GetFileExInfoStandard, &fad))
                    sz = ((LONG64)fad.nFileSizeHigh << 32) |
                         (LONG64)fad.nFileSizeLow;
                InterlockedIncrement64(&g_hirespub_files);
                InterlockedExchangeAdd64(&g_hirespub_bytes, sz);
            }
        }
    }
}

static DWORD WINAPI depth_worker(LPVOID param) {
    (void)param;
    for (;;) {
        WaitForSingleObject(g_depth_event, INFINITE);
        for (;;) {
            ne_depth_job job;
            bool         have = false;

            EnterCriticalSection(&g_depth_lock);
            if (g_depth_q_len > 0) {
                job = g_depth_q[g_depth_q_head];
                g_depth_q_head = (g_depth_q_head + 1) % NE_DEPTH_QUEUE_CAP;
                g_depth_q_len--;
                have = true;
            }
            LeaveCriticalSection(&g_depth_lock);
            if (!have) break;

            /* Stopping: drain without doing work. */
            if (InterlockedCompareExchange(&g_depth_stop, 0, 0)) continue;
            if (InterlockedCompareExchange(&g_depth_off, 0, 0)) {
                /* Off after a session-build failure: re-arm once the retry
                 * deadline passes (the self-heal sweep re-enqueues work). */
                if ((LONG64)GetTickCount64() <
                    InterlockedCompareExchange64(&g_depth_retry_at, 0, 0)) {
                    continue;
                }
                InterlockedExchange(&g_depth_off, 0);
            }

            /* Depth model was re-selected: drop the old session so the next
             * build below picks up the new file, and re-arm the feature (the
             * previous selection may have failed and set g_depth_off). */
            if (InterlockedExchange(&g_depth_reload, 0)) {
                if (g_depth) { mn_depth_destroy(g_depth); g_depth = NULL; }
                InterlockedExchange(&g_depth_off, 0);
            }

            /* Lazy one-time session build (the expensive part). The depth
             * model filename is resolved from the persisted selection
             * (selected.txt) so a downloaded Depth-Anything-Base/Large is used
             * for new depth generations; falls back to the bundled small. */
            if (!g_depth) {
                char sel[256] = {0};
                char model[1700];
                if (!g_app ||
                    !mn_app_get_selected_model(g_app, "depth",
                                               sel, sizeof(sel)) || !sel[0])
                    snprintf(sel, sizeof(sel), "depth_anything_v2_small.onnx");
                snprintf(model, sizeof(model),
                         "%s\\ai-models\\%s", g_data_dir, sel);
                g_depth = mn_depth_create(model);
                if (!g_depth) {
                    /* Model missing / ORT failure: back off for 60 s, then the
                     * worker re-arms and tries again (NOT a permanent latch). */
                    fprintf(stderr, "[depth] session build failed (%s); "
                            "retrying in 60s\n", model);
                    fflush(stderr);
                    InterlockedExchange64(&g_depth_retry_at,
                        (LONG64)GetTickCount64() + 60000);
                    InterlockedExchange(&g_depth_off, 1);
                    InterlockedExchange(&g_depth_state, 3);   /* failed */
                    continue;
                }
                InterlockedExchange(&g_depth_state, 2);       /* ready */
            }

            if (GetFileAttributesA(job.depth) != INVALID_FILE_ATTRIBUTES)
                continue;   /* another pass already produced it */

            /* HIGH-RES pipeline: extract a full-resolution cover (long edge
             * <= 1024, aspect preserved) for this album and feed THAT to the
             * model — the 256 webart PNG only exists as a grid thumb. Also
             * mirror the hi-res cover next to the webart PNG as
             * "<png-stem>.hires.png" so the volumetric mesh samples a crisp
             * texture. Falls back to the 256 PNG when no hi-res is available. */
            {
                const char *src = job.png;   /* default: the 256 thumb */
                char hires[1600] = {0};
                if (g_app && job.album[0] &&
                    mn_app_hires_cover(g_app, job.artist, job.album,
                                       true, hires, sizeof(hires)) &&
                    hires[0]) {
                    src = hires;
                    /* Publish a webart-side hi-res copy for the UI mesh. */
                    depth_publish_hires(job.png, hires);
                }
                InterlockedExchange(&g_depth_state, 4);       /* generating */
                snprintf(g_depth_busy, sizeof(g_depth_busy), "%s",
                         job.album[0] ? job.album : "cover");
                if (depth_generate_guarded(g_depth, src, job.depth)) {
                    InterlockedIncrement(&g_depth_done_n);
                    InterlockedExchange(&g_depth_fail_n, 0);
                } else if (InterlockedIncrement(&g_depth_fail_n) >= 3) {
                    /* AUTORECOVERY: repeated failures usually mean a corrupt
                     * session (driver reset, bad state after a caught crash)
                     * — destroy it; the next job rebuilds from scratch. */
                    fprintf(stderr, "[depth] 3 consecutive failures; "
                            "rebuilding the session\n");
                    fflush(stderr);
                    mn_depth_destroy(g_depth);
                    g_depth = NULL;
                    InterlockedExchange(&g_depth_fail_n, 0);
                    InterlockedExchange64(&g_depth_retry_at,
                        (LONG64)GetTickCount64() + 15000);
                    InterlockedExchange(&g_depth_off, 1);
                    InterlockedExchange(&g_depth_state, 3);
                }
                g_depth_busy[0] = 0;
                if (InterlockedCompareExchange(&g_depth_state, 0, 0) == 4)
                    InterlockedExchange(&g_depth_state, 2);   /* back to ready */
            }
        }
        if (InterlockedCompareExchange(&g_depth_stop, 0, 0)) break;
    }
    return 0;
}

/* Enqueue a depth job for an existing art PNG. Cheap; safe on the CEF UI
 * thread. No-ops when the map already exists, the feature is off, the queue
 * is full (drop — a later browse retries), or the job is already queued. */
static void depth_enqueue(const char *png, const char *artist,
                          const char *album) {
    char   depth[1620];
    size_t n;
    bool   queued = false;
    int    i;

    if (!png || !g_depth_event) return;
    /* While off, keep DROPPING only until the retry deadline; after it, accept
     * jobs again so the worker can re-arm and process them. */
    if (InterlockedCompareExchange(&g_depth_off, 0, 0) &&
        (LONG64)GetTickCount64() <
            InterlockedCompareExchange64(&g_depth_retry_at, 0, 0)) {
        return;
    }
    n = strlen(png);
    if (n < 5) return;
    snprintf(depth, sizeof(depth), "%.*s.depth.png", (int)(n - 4), png);
    if (GetFileAttributesA(depth) != INVALID_FILE_ATTRIBUTES) return;

    EnterCriticalSection(&g_depth_lock);
    for (i = 0; i < g_depth_q_len; i++) {
        int idx = (g_depth_q_head + i) % NE_DEPTH_QUEUE_CAP;
        if (strcmp(g_depth_q[idx].png, png) == 0) { i = -1; break; }
    }
    if (i >= 0 && g_depth_q_len < NE_DEPTH_QUEUE_CAP) {
        int tail = (g_depth_q_head + g_depth_q_len) % NE_DEPTH_QUEUE_CAP;
        snprintf(g_depth_q[tail].png,    sizeof(g_depth_q[tail].png),    "%s", png);
        snprintf(g_depth_q[tail].depth,  sizeof(g_depth_q[tail].depth),  "%s", depth);
        snprintf(g_depth_q[tail].artist, sizeof(g_depth_q[tail].artist), "%s",
                 artist ? artist : "");
        snprintf(g_depth_q[tail].album,  sizeof(g_depth_q[tail].album),  "%s",
                 album ? album : "");
        g_depth_q_len++;
        queued = true;
    }
    LeaveCriticalSection(&g_depth_lock);
    if (queued) SetEvent(g_depth_event);
}

/* SEH guard around one depth inference: an access violation inside ORT (GPU
 * driver reset, corrupted session) is caught here instead of crashing the
 * whole app; the caller counts it as a failure and rebuilds the session. */
static bool depth_generate_guarded(mn_depth *d, const char *src,
                                   const char *dst) {
    bool ok = false;
    __try {
        ok = mn_depth_generate(d, src, dst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[depth] native exception 0x%08lX during generate — "
                "caught, session will be rebuilt\n",
                (unsigned long)GetExceptionCode());
        fflush(stderr);
        ok = false;
    }
    return ok;
}

/* PRIORITY depth request: front-inserts so the NOW-PLAYING album's map is
 * generated next, not behind a 200-job self-heal sweep (each job can take
 * 10-60 s with the base/large models). Skips silently when the map exists,
 * the job is already queued at the head, or the feature is off. */
static char g_depth_prio_last[1600];   /* memo: skip repeat polls */
static void depth_enqueue_priority(const char *png, const char *artist,
                                   const char *album) {
    char   depth[1620];
    size_t n;
    int    i;
    bool   queued = false;

    if (!png || !png[0] || !g_depth_event) return;
    if (InterlockedCompareExchange(&g_depth_off, 0, 0) &&
        (LONG64)GetTickCount64() <
            InterlockedCompareExchange64(&g_depth_retry_at, 0, 0)) {
        return;
    }
    n = strlen(png);
    if (n < 5) return;
    snprintf(depth, sizeof(depth), "%.*s.depth.png", (int)(n - 4), png);
    if (GetFileAttributesA(depth) != INVALID_FILE_ATTRIBUTES) {
        /* map exists: nothing to do, and DON'T memoize — if it is later
         * cleared (Clear cache / regenerate) we must re-enqueue on the next
         * poll rather than being stuck thinking it is done. */
        return;
    }
    /* map missing: memo prevents re-enqueuing the SAME missing png every 250 ms
     * poll while it is generating; it is reset in build_now when the now-
     * playing png actually changes, so a re-selected album re-triggers. */
    if (strcmp(png, g_depth_prio_last) == 0) return;

    EnterCriticalSection(&g_depth_lock);
    /* remove an existing queued copy so the front-insert is THE copy */
    for (i = 0; i < g_depth_q_len; i++) {
        int idx = (g_depth_q_head + i) % NE_DEPTH_QUEUE_CAP;
        if (strcmp(g_depth_q[idx].png, png) == 0) {
            /* shift the earlier segment up over the duplicate */
            int j;
            for (j = i; j > 0; j--) {
                int dst = (g_depth_q_head + j) % NE_DEPTH_QUEUE_CAP;
                int src = (g_depth_q_head + j - 1) % NE_DEPTH_QUEUE_CAP;
                g_depth_q[dst] = g_depth_q[src];
            }
            g_depth_q_head = (g_depth_q_head + 1) % NE_DEPTH_QUEUE_CAP;
            g_depth_q_len--;
            break;
        }
    }
    if (g_depth_q_len < NE_DEPTH_QUEUE_CAP) {
        g_depth_q_head = (g_depth_q_head - 1 + NE_DEPTH_QUEUE_CAP)
                       % NE_DEPTH_QUEUE_CAP;
        snprintf(g_depth_q[g_depth_q_head].png,
                 sizeof(g_depth_q[g_depth_q_head].png), "%s", png);
        snprintf(g_depth_q[g_depth_q_head].depth,
                 sizeof(g_depth_q[g_depth_q_head].depth), "%s", depth);
        snprintf(g_depth_q[g_depth_q_head].artist,
                 sizeof(g_depth_q[g_depth_q_head].artist), "%s",
                 artist ? artist : "");
        snprintf(g_depth_q[g_depth_q_head].album,
                 sizeof(g_depth_q[g_depth_q_head].album), "%s",
                 album ? album : "");
        g_depth_q_len++;
        queued = true;
        snprintf(g_depth_prio_last, sizeof(g_depth_prio_last), "%s", png);
    }
    LeaveCriticalSection(&g_depth_lock);
    if (queued) SetEvent(g_depth_event);
}

/* Start the worker (browser process, after setup_webroot filled g_data_dir). */
static void depth_worker_start(void) {
    InitializeCriticalSection(&g_depth_lock);
    g_depth_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_depth_event) return;
    g_depth_thread = CreateThread(NULL, 0, depth_worker, NULL, 0, NULL);
    if (g_depth_thread) {
        SetThreadPriority(g_depth_thread, THREAD_PRIORITY_LOWEST);
    }
}

/* Join the worker + release the session at teardown. */
static void depth_worker_stop(void) {
    if (g_depth_thread) {
        InterlockedExchange(&g_depth_stop, 1);
        SetEvent(g_depth_event);
        WaitForSingleObject(g_depth_thread, INFINITE);
        CloseHandle(g_depth_thread);
        g_depth_thread = NULL;
    }
    if (g_depth_event) { CloseHandle(g_depth_event); g_depth_event = NULL; }
    if (g_depth) { mn_depth_destroy(g_depth); g_depth = NULL; }
}

/* ------------------------------------------------------------------------- */
/* ONE-STORE ART ARCHITECTURE (serve-what-exists).                            */
/*                                                                            */
/* art-cache\ is the single art store: <key-hash>.png grid thumbs (written    */
/* at scan time / by targeted extraction) plus their derived siblings         */
/* <key-hash>.hires.png and <key-hash>.depth.png. The key is the scan-time    */
/* "<album_artist-or-artist>\x1f<album>" FNV — the SAME key every surface     */
/* derives, so serving is one synchronous stat (art_url_for). There is no     */
/* second store, no mirror, no re-encode, and therefore no async gap between  */
/* "URL emitted" and "file exists": a URL is only ever emitted for a file     */
/* that exists AT EMIT TIME. Rows whose thumb is not yet materialized carry   */
/* art:"" (the UI paints its intentional placeholder, zero retries) and this  */
/* pool EXTRACTS the thumb in the background; each landing posts a throttled  */
/* {"type":"artready"} batch so the UI repaints exactly the affected tiles    */
/* (Winamp's targeted-redraw model — correctness never depends on a retry    */
/* budget outracing a queue).                                                 */
/* ------------------------------------------------------------------------- */

/* Sized so one full cold-cache grid flood (a maximized far-region jump is
 * ~150-200 visible tiles, plus the eager page chain behind it) can NEVER
 * overflow into drops of in-view work: ~1.2 MB static. When the queue does
 * fill, artenc_enqueue evicts the OLDEST job (fresh visible requests always
 * queue) and the worker fires one bounded integrity kick after the drain so
 * evicted albums still heal without user interaction. */
#define NE_ARTENC_QUEUE_CAP 2048
#define NE_ARTENC_THREADS   4

typedef struct {
    char aa[256];       /* album_artist-or-artist (the requesting surface's) */
    char album[256];
} ne_artenc_job;

static ne_artenc_job    g_artenc_q[NE_ARTENC_QUEUE_CAP];
static int              g_artenc_head = 0;
static int              g_artenc_len  = 0;
static CRITICAL_SECTION g_artenc_lock;
static HANDLE           g_artenc_sem  = NULL;   /* counts queued jobs        */
static HANDLE           g_artenc_threads[NE_ARTENC_THREADS];
static volatile LONG    g_artenc_stop = 0;
/* Set when a full queue forced an eviction; consumed by the worker when the
 * queue next drains — dropped jobs are then recovered by one bounded
 * art_integrity_kick instead of waiting for a scroll or the 5-min tick. */
static volatile LONG    g_artenc_dropped = 0;
static void art_integrity_kick(int64_t limit);  /* defined with the verifier */

/* True iff `path` names a non-empty file. A crash mid-encode can leave a
 * 0-byte PNG that GetFileAttributes reports as present; trusting it kept the
 * cover blank forever. Treat empty files as ABSENT so they get regenerated. */
static bool file_present_nonempty(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    return (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0);
}

/* FNV-1a over the album key "<aa>\x1f<album>" — indexes the session-local
 * failure ledgers (artdead ring, NONE set). This is NOT the on-disk filename
 * hash (artcache.c owns that); it only has to be stable within this app. */
static uint64_t art_key_hash(const char *aa, const char *album) {
    uint64_t h = 1469598103934665603ULL;
    const char *parts[3];
    parts[0] = aa ? aa : "";
    parts[1] = "\x1f";
    parts[2] = album ? album : "";
    for (int i = 0; i < 3; i++) {
        for (const unsigned char *p = (const unsigned char *)parts[i]; *p; ++p) {
            h ^= (uint64_t)(*p);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

/* Transient-failure belt: an extraction that failed this session isn't
 * re-enqueued on every browse. NOT permanent — the ring is CLEARED at the
 * start of every heal tick, so an AV lock / mid-delete can never mute a
 * cover for more than one heal period. Small ring, mutex-guarded. */
#define NE_ARTDEAD_CAP 512
static uint64_t         g_artdead[NE_ARTDEAD_CAP];      /* art_key_hash values */
static int              g_artdead_len = 0, g_artdead_head = 0;
static CRITICAL_SECTION g_artdead_lock;
static bool art_is_dead(const char *aa, const char *album) {
    uint64_t h = art_key_hash(aa, album);
    int i; bool hit = false;
    EnterCriticalSection(&g_artdead_lock);
    for (i = 0; i < g_artdead_len; i++)
        if (g_artdead[i] == h) { hit = true; break; }
    LeaveCriticalSection(&g_artdead_lock);
    return hit;
}
static void art_mark_dead(const char *aa, const char *album) {
    uint64_t h = art_key_hash(aa, album);
    EnterCriticalSection(&g_artdead_lock);
    if (g_artdead_len < NE_ARTDEAD_CAP) {
        g_artdead[g_artdead_len++] = h;
    } else {                              /* ring-evict oldest */
        g_artdead[g_artdead_head] = h;
        g_artdead_head = (g_artdead_head + 1) % NE_ARTDEAD_CAP;
    }
    LeaveCriticalSection(&g_artdead_lock);
}
/* Heal tick opener: every transient verdict expires each tick. */
static void art_clear_dead_all(void) {
    EnterCriticalSection(&g_artdead_lock);
    g_artdead_len = 0;
    g_artdead_head = 0;
    LeaveCriticalSection(&g_artdead_lock);
}

/* --------------------------------------------------------------------------
 * Persisted NONE verdicts (MM's "use default icon" negative cache): albums
 * where extraction found NO resolvable source are recorded in
 * <data_dir>\artnone.txt (one art_key_hash hex per line) and never re-probed
 * — they render the same intentional placeholder everywhere. A NONE verdict
 * only gates EXTRACTION ENQUEUES, never serving: if a thumb appears later
 * (folder-watch rescan after the user drops a folder.jpg, tag edit, online
 * fetch), art_url_for serves it regardless, and the explicit invalidation
 * hooks (tag/art writes, artfetch, user refresh) also clear the verdict.
 * -------------------------------------------------------------------------- */
static CRITICAL_SECTION g_artnone_lock;
static uint64_t        *g_artnone = NULL;
static int              g_artnone_n = 0, g_artnone_cap = 0;
static bool             g_artnone_loaded = false;

static void art_none_path(char *out, size_t n) {
    snprintf(out, n, "%s\\artnone.txt", g_data_dir);
}
/* caller holds g_artnone_lock */
static void art_none_load_locked(void) {
    char path[1400];
    FILE *f;
    if (g_artnone_loaded || !g_data_dir[0]) return;
    g_artnone_loaded = true;
    art_none_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return;
    {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            uint64_t h = strtoull(line, NULL, 16);
            if (!h) continue;
            if (g_artnone_n == g_artnone_cap) {
                int nc = g_artnone_cap ? g_artnone_cap * 2 : 256;
                uint64_t *nl = (uint64_t *)realloc(g_artnone,
                                                   (size_t)nc * sizeof(uint64_t));
                if (!nl) break;
                g_artnone = nl; g_artnone_cap = nc;
            }
            g_artnone[g_artnone_n++] = h;
        }
    }
    fclose(f);
}
static bool art_none_has(const char *aa, const char *album) {
    uint64_t h = art_key_hash(aa, album);
    int i; bool hit = false;
    EnterCriticalSection(&g_artnone_lock);
    art_none_load_locked();
    for (i = 0; i < g_artnone_n; i++)
        if (g_artnone[i] == h) { hit = true; break; }
    LeaveCriticalSection(&g_artnone_lock);
    return hit;
}
static void art_none_add(const char *aa, const char *album) {
    uint64_t h = art_key_hash(aa, album);
    bool fresh = false;
    EnterCriticalSection(&g_artnone_lock);
    art_none_load_locked();
    {
        int i;
        for (i = 0; i < g_artnone_n; i++)
            if (g_artnone[i] == h) break;
        if (i == g_artnone_n) {
            if (g_artnone_n == g_artnone_cap) {
                int nc = g_artnone_cap ? g_artnone_cap * 2 : 256;
                uint64_t *nl = (uint64_t *)realloc(g_artnone,
                                                   (size_t)nc * sizeof(uint64_t));
                if (nl) { g_artnone = nl; g_artnone_cap = nc; }
            }
            if (g_artnone_n < g_artnone_cap) {
                g_artnone[g_artnone_n++] = h;
                fresh = true;
            }
        }
    }
    if (fresh && g_data_dir[0]) {
        char path[1400];
        FILE *f;
        art_none_path(path, sizeof(path));
        f = fopen(path, "a");
        if (f) {
            fprintf(f, "%016llx\n", (unsigned long long)h);
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_artnone_lock);
}
/* Rewrite-on-remove keeps the file exact; removals are rare (art appeared
 * for a previously artless album via tag edit / online fetch). */
static void art_none_remove(const char *aa, const char *album) {
    uint64_t h = art_key_hash(aa, album);
    bool changed = false;
    EnterCriticalSection(&g_artnone_lock);
    art_none_load_locked();
    {
        int i;
        for (i = 0; i < g_artnone_n; i++) {
            if (g_artnone[i] == h) {
                g_artnone[i] = g_artnone[--g_artnone_n];
                changed = true;
                break;
            }
        }
    }
    if (changed && g_data_dir[0]) {
        char path[1400];
        FILE *f;
        art_none_path(path, sizeof(path));
        f = fopen(path, "w");
        if (f) {
            int i;
            for (i = 0; i < g_artnone_n; i++)
                fprintf(f, "%016llx\n", (unsigned long long)g_artnone[i]);
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_artnone_lock);
}
static void art_none_clear_all(void) {
    EnterCriticalSection(&g_artnone_lock);
    art_none_load_locked();
    g_artnone_n = 0;
    if (g_data_dir[0]) {
        char path[1400];
        art_none_path(path, sizeof(path));
        DeleteFileA(path);
    }
    LeaveCriticalSection(&g_artnone_lock);
}

/* Build the served URL for an existing art-cache thumb: file:///<path with
 * forward slashes>?g=<mtime> — the generation param busts CEF's per-URL
 * file:// cache after a tag-edit/art-replace rewrites the same path. */
static bool art_thumb_url(const char *thumb, char *url_out, size_t url_n) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char fwd[1200];
    size_t j = 0;
    if (url_out && url_n) url_out[0] = 0;
    if (!thumb || !thumb[0] || !url_out) return false;
    if (!GetFileAttributesExA(thumb, GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0))
        return false;
    for (size_t i = 0; thumb[i] && j + 1 < sizeof(fwd); ++i)
        fwd[j++] = (thumb[i] == '\\') ? '/' : thumb[i];
    fwd[j] = 0;
    {
        uint64_t gen = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32)
                     |  (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
        snprintf(url_out, url_n, "file:///%s?g=%llx",
                 fwd, (unsigned long long)gen);
    }
    return true;
}

/* --------------------------------------------------------------------------
 * artready batcher: workers that materialize a thumb queue (aa, album, url)
 * here; a short-lived flusher thread coalesces everything queued within a
 * ~250 ms window into ONE {"type":"artready","items":[...]} bridge emit so
 * the one-time backfill (hundreds of albums healing) cannot flood the bridge.
 * Fully idle when nothing is queued (no persistent timer — the flusher only
 * exists while items are pending; perf-hardening invariant intact).
 * -------------------------------------------------------------------------- */
#define NE_ARTREADY_CAP 192
typedef struct {
    char aa[256];
    char album[256];
    char url[1300];
} ne_artready_item;
static ne_artready_item g_artready_q[NE_ARTREADY_CAP];
static int              g_artready_n = 0;
static CRITICAL_SECTION g_artready_lock;
static volatile LONG    g_artready_flusher = 0;   /* single-flight flag */

static void artready_kick(void);

static DWORD WINAPI artready_flush_thread(LPVOID param) {
    (void)param;
    for (;;) {
        Sleep(250);                       /* coalesce window */
        {
            ne_artready_item *items = NULL;
            int n = 0;
            EnterCriticalSection(&g_artready_lock);
            n = g_artready_n;
            if (n > 0) {
                items = (ne_artready_item *)malloc((size_t)n * sizeof(*items));
                if (items)
                    memcpy(items, g_artready_q, (size_t)n * sizeof(*items));
                else
                    n = 0;
                g_artready_n = 0;
            }
            LeaveCriticalSection(&g_artready_lock);
            if (n > 0) {
                cef_frame_t *frame = sync_grab_frame();
                if (frame) {
                    strbuf b; sb_init(&b);
                    sb_puts(&b, "{\"type\":\"artready\",\"items\":[");
                    for (int i = 0; i < n; i++) {
                        if (i) sb_putc(&b, ',');
                        sb_puts(&b, "{\"aa\":");
                        sb_json_str(&b, items[i].aa);
                        sb_puts(&b, ",\"album\":");
                        sb_json_str(&b, items[i].album);
                        sb_puts(&b, ",\"url\":");
                        sb_json_str(&b, items[i].url);
                        sb_putc(&b, '}');
                    }
                    sb_puts(&b, "]}");
                    if (!b.oom) post_emit_owned(frame, b.data);
                    else {
                        sb_free(&b);
                        frame->base.release(&frame->base);
                    }
                }
            }
            free(items);
        }
        {
            int more;
            EnterCriticalSection(&g_artready_lock);
            more = g_artready_n;
            LeaveCriticalSection(&g_artready_lock);
            if (!more) break;             /* drained: park (thread exits) */
        }
    }
    InterlockedExchange(&g_artready_flusher, 0);
    /* close the queued-between-drain-and-clear window */
    {
        int more;
        EnterCriticalSection(&g_artready_lock);
        more = g_artready_n;
        LeaveCriticalSection(&g_artready_lock);
        if (more) artready_kick();
    }
    return 0;
}

static void artready_kick(void) {
    HANDLE h;
    if (InterlockedCompareExchange(&g_artready_flusher, 1, 0) != 0) return;
    h = CreateThread(NULL, 0, artready_flush_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else   InterlockedExchange(&g_artready_flusher, 0);
}

static void artready_queue(const char *aa, const char *album, const char *url) {
    if (!album || !album[0] || !url || !url[0]) return;
    EnterCriticalSection(&g_artready_lock);
    if (g_artready_n < NE_ARTREADY_CAP) {   /* drop-if-full: the tile heals on
                                             * the next fetch/scroll re-enqueue */
        ne_artready_item *it = &g_artready_q[g_artready_n++];
        snprintf(it->aa,    sizeof(it->aa),    "%s", aa ? aa : "");
        snprintf(it->album, sizeof(it->album), "%s", album);
        snprintf(it->url,   sizeof(it->url),   "%s", url);
    }
    LeaveCriticalSection(&g_artready_lock);
    artready_kick();
}

/* One extraction job: materialize the (aa, album) thumb into art-cache (the
 * only genuinely async art work left), then publish derived tiers off the
 * serving path. On a fresh landing, posts an artready batch entry. */
static void art_encode_one(const ne_artenc_job *job) {
    char thumb[MN_ART_PATH_MAX];
    bool newly = false, src_seen = false;

    if (!job->album[0] || !g_app) return;
    if (art_none_has(job->aa, job->album)) return;

    if (!mn_app_art_extract_one(g_app, job->aa, job->album, &newly,
                                thumb, sizeof(thumb), &src_seen)) {
        /* Session belt either way (stops re-enqueue loops until the next
         * heal tick clears the ring). The PERSISTED NONE verdict is only
         * for genuinely artless albums: when a source EXISTS but failed to
         * decode (corrupt sidecar, AV lock, exotic format) a NONE here
         * would permanently mute an album that has recoverable art — the
         * false-NONE bug (Camila: 3 valid covers beside 1 corrupt one). */
        art_mark_dead(job->aa, job->album);
        if (!src_seen)
            art_none_add(job->aa, job->album);
        return;
    }

    /* artready on BOTH paths — newly-extracted AND fast-path (the thumb
     * materialized between the pending row's emit and this job running,
     * e.g. the import scanner won the race). The pending tile that caused
     * this enqueue is waiting either way; the emit is coalesced and the JS
     * repaint only touches tiles whose data-art-key matches (idempotent). */
    {
        char url[1300];
        if (art_thumb_url(thumb, url, sizeof(url)))
            artready_queue(job->aa, job->album, url);
    }

    /* NO hires ensure here: hi-res is DEMAND-only (Cover Flow center, the
     * now-playing depth mesh and artfetch ingest all self-ensure it). The
     * unconditional ensure that used to sit here made every cold-backfill
     * extraction also perform a full-resolution decode nobody had asked for
     * (~doubling cold-rebuild CPU/disk and starving the thumb queue users
     * were actually waiting on — measured 259 undemanded hires on one cold
     * boot). Thumb healing always wins now. */
    if (InterlockedCompareExchange(&g_depth_batch, 0, 0))
        depth_enqueue(thumb, job->aa, job->album); /* volumetric companion */
}

static DWORD WINAPI artenc_worker(LPVOID param) {
    (void)param;
    for (;;) {
        WaitForSingleObject(g_artenc_sem, INFINITE);
        if (InterlockedCompareExchange(&g_artenc_stop, 0, 0)) break;
        ne_artenc_job job;
        bool have = false;
        EnterCriticalSection(&g_artenc_lock);
        if (g_artenc_len > 0) {
            job = g_artenc_q[g_artenc_head];
            g_artenc_head = (g_artenc_head + 1) % NE_ARTENC_QUEUE_CAP;
            g_artenc_len--;
            have = true;
        }
        LeaveCriticalSection(&g_artenc_lock);
        if (have) art_encode_one(&job);
        /* Drop recovery: if any eviction happened while the queue was full,
         * run one integrity sweep once the flood has fully drained. The
         * sweep re-derives every album key and heals whatever the evictions
         * lost — so a dropped job can never strand a placeholder until the
         * user interacts or the 5-min tick fires. Single-flight inside the
         * kick; lowest priority; no-op when nothing is actually missing. */
        {
            int idle;
            EnterCriticalSection(&g_artenc_lock);
            idle = (g_artenc_len == 0);
            LeaveCriticalSection(&g_artenc_lock);
            if (idle && InterlockedExchange(&g_artenc_dropped, 0))
                art_integrity_kick(0);
        }
    }
    return 0;
}

/* Called on the CEF UI thread: cheap — copy strings, dedup, signal. When
 * full, the OLDEST job is evicted (it was enqueued by a region the user has
 * scrolled away from; the NEW request is for a tile on screen RIGHT NOW), so
 * visible-tile heals are unloseable. Every eviction flags g_artenc_dropped;
 * the worker recovers the evicted albums with one bounded integrity kick
 * once the queue drains. */
static void artenc_enqueue(const char *aa, const char *album) {
    if (!g_artenc_sem || !album || !album[0]) return;
    if (art_is_dead(aa, album)) return;   /* failed this session — belt      */
    bool queued = false, evicted = false;
    int  i;
    EnterCriticalSection(&g_artenc_lock);
    for (i = 0; i < g_artenc_len; i++) {
        int idx = (g_artenc_head + i) % NE_ARTENC_QUEUE_CAP;
        if (strcmp(g_artenc_q[idx].album, album) == 0 &&
            strcmp(g_artenc_q[idx].aa, aa ? aa : "") == 0) { i = -1; break; }
    }
    if (i >= 0) {
        if (g_artenc_len >= NE_ARTENC_QUEUE_CAP) {
            /* drop-OLDEST: advance head. The evicted job's semaphore count
             * stays outstanding and pays for the new tail slot below, so no
             * extra ReleaseSemaphore (workers tolerate empty wakes anyway). */
            g_artenc_head = (g_artenc_head + 1) % NE_ARTENC_QUEUE_CAP;
            g_artenc_len--;
            evicted = true;
            InterlockedExchange(&g_artenc_dropped, 1);
        }
        {
            int tail = (g_artenc_head + g_artenc_len) % NE_ARTENC_QUEUE_CAP;
            snprintf(g_artenc_q[tail].aa,    sizeof(g_artenc_q[tail].aa),    "%s",
                     aa ? aa : "");
            snprintf(g_artenc_q[tail].album, sizeof(g_artenc_q[tail].album), "%s",
                     album);
            g_artenc_len++;
            queued = true;
        }
    }
    LeaveCriticalSection(&g_artenc_lock);
    if (queued && !evicted) ReleaseSemaphore(g_artenc_sem, 1, NULL);
}

static void artenc_pool_start(void) {
    InitializeCriticalSection(&g_artenc_lock);
    InitializeCriticalSection(&g_artdead_lock);
    InitializeCriticalSection(&g_artnone_lock);
    InitializeCriticalSection(&g_artready_lock);
    g_artenc_sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
    if (!g_artenc_sem) return;
    /* Throwaway UI thumbs: favor encode speed over size (stb reads the level
     * per call; set once before any worker runs). Extraction still encodes
     * fresh thumbs via artcache.c, so the knob remains load-bearing. */
    stbi_write_png_compression_level = 3;
    for (int i = 0; i < NE_ARTENC_THREADS; i++)
        g_artenc_threads[i] = CreateThread(NULL, 0, artenc_worker, NULL, 0, NULL);
}

static void artenc_pool_stop(void) {
    if (!g_artenc_sem) return;
    InterlockedExchange(&g_artenc_stop, 1);
    ReleaseSemaphore(g_artenc_sem, NE_ARTENC_THREADS, NULL);
    for (int i = 0; i < NE_ARTENC_THREADS; i++) {
        if (g_artenc_threads[i]) {
            WaitForSingleObject(g_artenc_threads[i], INFINITE);
            CloseHandle(g_artenc_threads[i]);
            g_artenc_threads[i] = NULL;
        }
    }
    CloseHandle(g_artenc_sem);
    g_artenc_sem = NULL;
    DeleteCriticalSection(&g_artenc_lock);
}

/* NO-404 SERVING CONTRACT: a URL is only ever emitted for a file that exists
 * AT EMIT TIME (one synchronous stat of a file the scan already wrote — no
 * decode, no encode on the render path). On a miss the row carries art:""
 * (the UI paints the placeholder synchronously, zero retries) and a
 * background EXTRACTION is enqueued; its landing posts artready and the UI
 * repaints exactly the affected tiles. The UI page is loaded from file://
 * with allow-file-access-from-files, so art-cache is served directly. */
static void art_url_for(char *url_out, size_t url_n,
                        const char *artist, const char *album) {
    char thumb[MN_ART_PATH_MAX];

    if (url_out && url_n) url_out[0] = 0;
    /* empty-album rows (unknown-album bucket) are NONE by design */
    if (!album || !album[0] || !g_app) return;

    if (mn_app_art_check(g_app, artist, album, thumb, sizeof(thumb)) &&
        art_thumb_url(thumb, url_out, url_n)) {
        /* Served — and NOTHING else on the thumb path. Hi-res is DEMAND-only
         * (Cover Flow center / now-playing depth mesh / artfetch ingest all
         * ensure it themselves): enqueueing a 10-100x hires extraction here
         * for every grid serve whose .hires sibling was missing re-created
         * the v1 non-convergence churn (cap < working set + regenerate-on-
         * serve = perpetual CPU/disk/SSD grind competing with scroll). */
        if (InterlockedCompareExchange(&g_depth_batch, 0, 0))
            depth_enqueue(thumb, artist, album);
        return;
    }

    /* Miss: art:"" now; extraction (unless a NONE verdict says the album
     * genuinely has no art — those tiles are the placeholder, terminally). */
    if (!art_none_has(artist, album))
        artenc_enqueue(artist, album);
}

/* ------------------------------------------------------------------------- */
/* JSON reply builders (ported from webview_host).                           */
/* ------------------------------------------------------------------------- */

static void append_row(strbuf *b, const mn_row *r) {
    char art[256];
    /* Art is cached under "<album_artist-or-artist>\x1f<album>" (the scan-time
     * key). mn_row now carries album_artist, so compilation/VA track rows
     * resolve the album cover exactly like the album grid / queue / now-playing
     * instead of hashing on the per-track artist (which was never cached and
     * showed the ♪ placeholder forever). */
    const char *aa = r->album_artist[0] ? r->album_artist : r->artist;
    art_url_for(art, sizeof(art), aa, r->album);
    sb_putc(b, '{');
    sb_puts(b, "\"id\":");          sb_json_i64(b, r->id);            sb_putc(b, ',');
    sb_puts(b, "\"title\":");       sb_json_str(b, r->title);        sb_putc(b, ',');
    sb_puts(b, "\"artist\":");      sb_json_str(b, r->artist);       sb_putc(b, ',');
    /* the art-key artist (album_artist-or-artist): lets the UI target
     * PENDING tiles when this album's artready lands */
    sb_puts(b, "\"album_artist\":"); sb_json_str(b, aa);             sb_putc(b, ',');
    sb_puts(b, "\"album\":");       sb_json_str(b, r->album);        sb_putc(b, ',');
    sb_puts(b, "\"genre\":");       sb_json_str(b, r->genre);        sb_putc(b, ',');
    sb_puts(b, "\"duration_ms\":"); sb_json_u32(b, r->duration_ms);  sb_putc(b, ',');
    sb_puts(b, "\"year\":");        sb_json_int(b, r->year);         sb_putc(b, ',');
    sb_puts(b, "\"track_no\":");    sb_json_int(b, r->track_no);     sb_putc(b, ',');
    sb_puts(b, "\"disc\":");        sb_json_int(b, r->disc_no);      sb_putc(b, ',');
    sb_puts(b, "\"rating\":");      sb_json_int(b, r->rating);       sb_putc(b, ',');
    sb_puts(b, "\"liked\":");       sb_json_int(b, r->liked);        sb_putc(b, ',');
    sb_puts(b, "\"play_count\":");  sb_json_i64(b, r->play_count);   sb_putc(b, ',');
    sb_puts(b, "\"bitrate\":");     sb_json_int(b, r->bitrate_kbps); sb_putc(b, ',');
    sb_puts(b, "\"size\":");        sb_json_i64(b, r->size);         sb_putc(b, ',');
    sb_puts(b, "\"date_added\":");  sb_json_i64(b, r->date_added);   sb_putc(b, ',');
    sb_puts(b, "\"missing\":");     sb_json_bool(b, r->missing);     sb_putc(b, ',');
    sb_puts(b, "\"path\":");        sb_json_str(b, r->path);         sb_putc(b, ',');
    sb_puts(b, "\"art\":");         sb_json_str(b, art);
    sb_putc(b, '}');
}

static void build_tracks(strbuf *b, int64_t offset, int count, int gen) {
    if (count < 0)   count = 0;
    if (count > 200) count = 200;

    mn_row rows[200];
    int n = (count > 0) ? mn_app_window(g_app, offset, count, rows) : 0;
    int64_t total = mn_app_row_count(g_app);

    sb_puts(b, "{\"type\":\"tracks\",\"gen\":");
    sb_json_int(b, gen);
    sb_puts(b, ",\"total\":");
    sb_json_i64(b, total);
    sb_puts(b, ",\"offset\":");
    sb_json_i64(b, offset);
    sb_puts(b, ",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        append_row(b, &rows[i]);
    }
    sb_puts(b, "]}");
}

static void build_albums(strbuf *b, int64_t offset, int count, int gen) {
    if (count < 0)   count = 0;
    if (count > 200) count = 200;

    mn_album albums[200];
    int n = (count > 0) ? mn_app_album_window(g_app, offset, count, albums) : 0;
    int64_t total = mn_app_album_count(g_app);

    sb_puts(b, "{\"type\":\"albums\",\"gen\":");
    sb_json_int(b, gen);
    sb_puts(b, ",\"total\":");
    sb_json_i64(b, total);
    sb_puts(b, ",\"offset\":");
    sb_json_i64(b, offset);
    sb_puts(b, ",\"albums\":[");
    for (int i = 0; i < n; i++) {
        const mn_album *a = &albums[i];
        char art[256];
        art_url_for(art, sizeof(art), a->artist, a->title);
        if (i) sb_putc(b, ',');
        sb_putc(b, '{');
        sb_puts(b, "\"id\":");          sb_json_i64(b, a->id);          sb_putc(b, ',');
        sb_puts(b, "\"title\":");       sb_json_str(b, a->title);       sb_putc(b, ',');
        sb_puts(b, "\"artist\":");      sb_json_str(b, a->artist);      sb_putc(b, ',');
        sb_puts(b, "\"track_count\":"); sb_json_int(b, a->track_count); sb_putc(b, ',');
        sb_puts(b, "\"year\":");        sb_json_int(b, a->year);        sb_putc(b, ',');
        sb_puts(b, "\"format\":");      sb_json_str(b, a->format);      sb_putc(b, ',');
        sb_puts(b, "\"sample_rate\":"); sb_json_int(b, a->sample_rate); sb_putc(b, ',');
        sb_puts(b, "\"bit_depth\":");   sb_json_int(b, a->bit_depth);   sb_putc(b, ',');
        sb_puts(b, "\"bitrate\":");     sb_json_int(b, a->bitrate_kbps);sb_putc(b, ',');
        sb_puts(b, "\"size\":");        sb_json_i64(b, a->size);        sb_putc(b, ',');
        sb_puts(b, "\"date_added\":");  sb_json_i64(b, a->date_added);  sb_putc(b, ',');
        sb_puts(b, "\"art\":");         sb_json_str(b, art);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

static void build_albumtracks(strbuf *b, int64_t album_id) {
    mn_row rows[200];
    int n = mn_app_album_tracks(g_app, album_id, 200, rows);

    sb_puts(b, "{\"type\":\"albumtracks\",\"id\":");
    sb_json_i64(b, album_id);
    sb_puts(b, ",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        append_row(b, &rows[i]);
    }
    sb_puts(b, "]}");
}

/* Facet browse (Artists / Genres / Years). `dim` = mn_facet_dim value. The
 * reply `type` mirrors the requesting command so the UI routes it correctly. */
static void build_facet(strbuf *b, const char *type, int dim, int64_t offset, int count) {
    enum { FCAP = 500 };
    static mn_facet_value vals[FCAP];   /* 500 * ~140B — keep off the stack   */
    int want = count > FCAP ? FCAP : count;
    int n = mn_app_facet_window(g_app, dim, offset, want, vals);
    int total = mn_app_facet_count(g_app, dim);
    sb_puts(b, "{\"type\":\"");
    sb_puts(b, type);
    sb_puts(b, "\",\"offset\":"); sb_json_i64(b, offset);
    sb_puts(b, ",\"total\":");    sb_json_int(b, total);
    sb_puts(b, ",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        sb_putc(b, '{');
        sb_puts(b, "\"id\":");    sb_json_i64(b, vals[i].id);    sb_putc(b, ',');
        sb_puts(b, "\"label\":"); sb_json_str(b, vals[i].label); sb_putc(b, ',');
        sb_puts(b, "\"count\":"); sb_json_int(b, vals[i].count);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

/* Playlists list. */
static void build_playlists(strbuf *b) {
    enum { PCAP = 400 };
    static mn_playlist_item items[PCAP];
    int n = mn_app_playlist_list(g_app, items, PCAP);
    sb_puts(b, "{\"type\":\"playlists\",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        sb_putc(b, '{');
        sb_puts(b, "\"id\":");    sb_json_i64(b, items[i].id);          sb_putc(b, ',');
        sb_puts(b, "\"name\":");  sb_json_str(b, items[i].name);        sb_putc(b, ',');
        sb_puts(b, "\"count\":"); sb_json_int(b, items[i].track_count);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

/* One playlist's tracks (in playlist order). */
static void build_playlisttracks(strbuf *b, int64_t id) {
    enum { PTCAP = 500 };
    static mn_row rows[PTCAP];          /* keep the big rows off the stack */
    int n = mn_app_playlist_tracks(g_app, id, PTCAP, rows);
    sb_puts(b, "{\"type\":\"playlisttracks\",\"id\":");
    sb_json_i64(b, id);
    sb_puts(b, ",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        append_row(b, &rows[i]);
    }
    sb_puts(b, "]}");
}

/* Tracks under one facet value (artist/genre/year drill-in). */
static void build_facettracks(strbuf *b, int dim, int64_t value_id) {
    enum { TCAP = 500 };
    static mn_row rows[TCAP];           /* mn_row is ~1.6 KB; keep off stack  */
    int n = mn_app_facet_tracks(g_app, dim, value_id, TCAP, rows);
    sb_puts(b, "{\"type\":\"facettracks\",\"dim\":");
    sb_json_int(b, dim);
    sb_puts(b, ",\"value_id\":"); sb_json_i64(b, value_id);
    sb_puts(b, ",\"rows\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        append_row(b, &rows[i]);
    }
    sb_puts(b, "]}");
}

static void build_now(strbuf *b) {
    mn_now now;
    memset(&now, 0, sizeof(now));
    mn_app_now(g_app, &now);

    char art[256];
    /* Art thumbnails are cached under "<album_artist>\x1f<album>" (see
     * mn_app_on_track); using the TRACK artist here missed the cache for
     * VA/feat. tracks, leaving the now-playing panel artless. */
    art_url_for(art, sizeof(art), now.track_album_artist, now.track_album);
    /* The PLAYING album's depth map wins the queue: front-insert so the 3D
     * cover appears in seconds, not behind a background self-heal sweep.
     * Keyed on the now-playing TRACK id so selecting a different track/album
     * from the queue always re-triggers (and clears the same-poll memo). */
    if (now.track_album[0]) {
        static int64_t s_last_now_id = -1;
        char ppng[MN_ART_PATH_MAX];
        if (now.track_id != s_last_now_id) {
            s_last_now_id = now.track_id;
            g_depth_prio_last[0] = '\0';   /* new selection: allow re-enqueue */
        }
        /* depth keys off the SERVED art-cache thumb; when it isn't
         * materialized yet the artenc pool is already extracting it and the
         * next poll re-tries the priority enqueue. */
        if (mn_app_art_check(g_app, now.track_album_artist, now.track_album,
                             ppng, sizeof(ppng)))
            depth_enqueue_priority(ppng, now.track_album_artist,
                                   now.track_album);
    }

    sb_puts(b, "{\"type\":\"now\",");
    sb_puts(b, "\"track_id\":");     sb_json_i64(b, now.track_id);          sb_putc(b, ',');
    sb_puts(b, "\"album_id\":");     sb_json_i64(b, now.album_id);          sb_putc(b, ',');
    sb_puts(b, "\"playing\":");      sb_json_bool(b, now.playing);          sb_putc(b, ',');
    sb_puts(b, "\"position_ms\":");  sb_json_i64(b, (int64_t)now.position_ms); sb_putc(b, ',');
    sb_puts(b, "\"duration_ms\":");  sb_json_i64(b, (int64_t)now.duration_ms); sb_putc(b, ',');
    sb_puts(b, "\"title\":");        sb_json_str(b, now.track_title);       sb_putc(b, ',');
    sb_puts(b, "\"artist\":");       sb_json_str(b, now.track_artist);      sb_putc(b, ',');
    sb_puts(b, "\"album\":");        sb_json_str(b, now.track_album);       sb_putc(b, ',');
    /* the ART-KEY artist (album_artist-or-artist — the exact aa art_url_for
     * hashed above): lets the UI stamp data-art-key on the now-playing tiles
     * so a later artready can target them even while the poll is memoized
     * (paused). Both bridge sides changed together (app.js reads it). */
    sb_puts(b, "\"track_album_artist\":"); sb_json_str(b, now.track_album_artist); sb_putc(b, ',');
    sb_puts(b, "\"track_path\":");   sb_json_str(b, now.track_path);        sb_putc(b, ',');
    sb_puts(b, "\"format\":");       sb_json_str(b, now.format);            sb_putc(b, ',');
    sb_puts(b, "\"sample_rate\":");  sb_json_u32(b, now.sample_rate);       sb_putc(b, ',');
    sb_puts(b, "\"bit_depth\":");    sb_json_int(b, now.bit_depth);         sb_putc(b, ',');
    sb_puts(b, "\"channels\":");     sb_json_int(b, now.channels);          sb_putc(b, ',');
    sb_puts(b, "\"bitrate_kbps\":"); sb_json_u32(b, now.bitrate_kbps);      sb_putc(b, ',');
    /* REAL device-side output format (SOURCE -> OUTPUT chain for the UI). */
    sb_puts(b, "\"out_rate\":");     sb_json_int(b, now.out_sample_rate);   sb_putc(b, ',');
    sb_puts(b, "\"out_bits\":");     sb_json_int(b, now.out_bit_depth);     sb_putc(b, ',');
    sb_puts(b, "\"out_ch\":");       sb_json_int(b, now.out_channels);      sb_putc(b, ',');
    sb_puts(b, "\"out_excl\":");     sb_json_bool(b, now.out_exclusive);    sb_putc(b, ',');
    sb_puts(b, "\"pipe_ch\":");      sb_json_int(b, now.pipe_channels);     sb_putc(b, ',');
    sb_puts(b, "\"downmixed\":");    sb_json_bool(b, now.downmixed);        sb_putc(b, ',');
    sb_puts(b, "\"rate_limited\":"); sb_json_bool(b, now.rate_limited);     sb_putc(b, ',');
    sb_puts(b, "\"out_pcm\":");      sb_json_str(b, now.out_pcm);           sb_putc(b, ',');
    /* depth pipeline status for the UI pill */
    {
        LONG dstate = InterlockedCompareExchange(&g_depth_state, 0, 0);
        int  retry_s = 0;
        if (dstate == 3) {
            LONG64 ra = InterlockedCompareExchange64(&g_depth_retry_at, 0, 0);
            LONG64 nowt = (LONG64)GetTickCount64();
            retry_s = (ra > nowt) ? (int)((ra - nowt) / 1000) : 0;
        }
        sb_puts(b, "\"depth_state\":");  sb_json_int(b, (int)dstate);        sb_putc(b, ',');
        sb_puts(b, "\"depth_queue\":");  sb_json_int(b, g_depth_q_len);      sb_putc(b, ',');
        sb_puts(b, "\"depth_done\":");   sb_json_int(b, (int)g_depth_done_n); sb_putc(b, ',');
        sb_puts(b, "\"depth_retry\":");  sb_json_int(b, retry_s);            sb_putc(b, ',');
        sb_puts(b, "\"depth_busy\":");   sb_json_str(b, g_depth_busy);       sb_putc(b, ',');
    }
    sb_puts(b, "\"volume\":");       sb_json_float(b, now.volume);          sb_putc(b, ',');
    sb_puts(b, "\"shuffle\":");      sb_json_bool(b, now.shuffle);          sb_putc(b, ',');
    sb_puts(b, "\"repeat\":");       sb_json_int(b, now.repeat);            sb_putc(b, ',');
    sb_puts(b, "\"liked\":");        sb_json_int(b, now.liked);             sb_putc(b, ',');
    sb_puts(b, "\"stems_available\":");   sb_json_bool(b, now.stems_available);   sb_putc(b, ',');
    sb_puts(b, "\"stems_loading\":");     sb_json_bool(b, now.stems_loading);     sb_putc(b, ',');
    sb_puts(b, "\"stems_enabled\":");     sb_json_bool(b, now.stems_enabled);     sb_putc(b, ',');
    sb_puts(b, "\"stems_passthrough\":"); sb_json_bool(b, now.stems_passthrough); sb_putc(b, ',');
    sb_puts(b, "\"neural_active\":");     sb_json_bool(b, now.neural_active);     sb_putc(b, ',');
    sb_puts(b, "\"stem_provider\":");     sb_json_str(b, now.stem_provider);      sb_putc(b, ',');
    sb_puts(b, "\"stem_rt_factor\":");    sb_json_float(b, now.stem_rt_factor);   sb_putc(b, ',');
    sb_puts(b, "\"stem_fraction\":");     sb_json_float(b, now.stem_fraction);    sb_putc(b, ',');
    sb_puts(b, "\"stem_meters\":[");
    for (int i = 0; i < 9; i++) {
        if (i) sb_putc(b, ',');
        sb_json_float(b, now.stem_meters[i]);
    }
    sb_puts(b, "],");
    sb_puts(b, "\"art\":");          sb_json_str(b, art);
    sb_putc(b, '}');
}

/* {"cmd":"audiocaps"} -> the default playback device's native capabilities,
 * probed live through the audio engine (WASAPI mix format + exclusive-mode
 * format search). "ok":false when no capability info could be obtained. */
static void build_audiocaps(strbuf *b) {
    mn_audio_caps c;
    memset(&c, 0, sizeof(c));
    bool ok = mn_app_audio_caps(g_app, &c);

    sb_puts(b, "{\"type\":\"audiocaps\",");
    sb_puts(b, "\"ok\":");                sb_json_bool(b, ok);              sb_putc(b, ',');
    sb_puts(b, "\"device\":");            sb_json_str(b, c.device_name);    sb_putc(b, ',');
    sb_puts(b, "\"max_bit_depth\":");     sb_json_int(b, c.max_bit_depth);  sb_putc(b, ',');
    sb_puts(b, "\"mix_rate\":");          sb_json_int(b, c.mix_sample_rate);sb_putc(b, ',');
    sb_puts(b, "\"max_rate\":");          sb_json_int(b, c.max_sample_rate);sb_putc(b, ',');
    sb_puts(b, "\"max_channels\":");      sb_json_int(b, c.max_channels);   sb_putc(b, ',');
    sb_puts(b, "\"rates\":[");
    for (int i = 0; i < c.native_rate_count && i < MN_CAPS_MAX_RATES; i++) {
        if (i) sb_putc(b, ',');
        sb_json_int(b, c.native_rates[i]);
    }
    sb_puts(b, "],");
    sb_puts(b, "\"exclusive_capable\":"); sb_json_bool(b, c.exclusive_capable);
    sb_putc(b, '}');
}

/* {"cmd":"audiodevices"} -> {"type":"audiodevices","devices":[{"name":"...",
 * "default":true,"active":false},...]}. "active" marks the endpoint audio is
 * currently routed to: the explicitly selected device if any, else the system
 * default. */
static void build_audiodevices(strbuf *b) {
    mn_audio_device devs[32];
    int n      = mn_app_list_devices(g_app, devs, 32);
    int active = mn_app_selected_device(g_app);

    sb_puts(b, "{\"type\":\"audiodevices\",\"devices\":[");
    for (int i = 0; i < n; i++) {
        bool is_active = (active >= 0) ? (i == active) : devs[i].is_default;
        if (i) sb_putc(b, ',');
        sb_putc(b, '{');
        sb_puts(b, "\"name\":");    sb_json_str(b, devs[i].name);        sb_putc(b, ',');
        sb_puts(b, "\"default\":"); sb_json_bool(b, devs[i].is_default); sb_putc(b, ',');
        sb_puts(b, "\"active\":");  sb_json_bool(b, is_active);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

/* {"cmd":"folders"} -> every library folder (INCLUDING hidden ones, so the
 * UI can offer the unhide checkbox) with its visible-track count. */
#define NE_MAX_FOLDERS 512

static void build_folders(strbuf *b) {
    mn_folder *fl = (mn_folder *)malloc(sizeof(mn_folder) * NE_MAX_FOLDERS);
    int n = fl ? (int)mn_app_folder_list(g_app, fl, NE_MAX_FOLDERS) : 0;

    sb_puts(b, "{\"type\":\"folders\",\"folders\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(b, ',');
        sb_putc(b, '{');
        char kind[32];
        kind_for_path(fl[i].path, kind, sizeof(kind));
        sb_puts(b, "\"id\":");          sb_json_i64(b, fl[i].id);          sb_putc(b, ',');
        sb_puts(b, "\"path\":");        sb_json_str(b, fl[i].path);        sb_putc(b, ',');
        sb_puts(b, "\"track_count\":"); sb_json_i64(b, fl[i].track_count); sb_putc(b, ',');
        sb_puts(b, "\"kind\":");        sb_json_str(b, kind);              sb_putc(b, ',');
        sb_puts(b, "\"hidden\":");      sb_json_bool(b, fl[i].hidden);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
    free(fl);
}

/* {"cmd":"stats"} -> aggregate library statistics for the stats view.
 * hours is fractional; lyrics_pct is -1 when the sidecar probe was skipped
 * (library larger than the bounded-cost threshold). */
static void build_stats(strbuf *b) {
    mn_app_stats s;
    bool ok = mn_app_get_stats(g_app, &s);
    if (!ok) memset(&s, 0, sizeof(s));

    sb_puts(b, "{\"type\":\"stats\",");
    sb_puts(b, "\"tracks\":");     sb_json_i64(b, s.tracks);      sb_putc(b, ',');
    sb_puts(b, "\"albums\":");     sb_json_i64(b, s.albums);      sb_putc(b, ',');
    sb_puts(b, "\"artists\":");    sb_json_i64(b, s.artists);     sb_putc(b, ',');
    sb_puts(b, "\"hours\":");      sb_json_float(b, (float)((double)s.duration_ms / 3600000.0)); sb_putc(b, ',');
    sb_puts(b, "\"size_bytes\":"); sb_json_i64(b, s.size_bytes);  sb_putc(b, ',');
    sb_puts(b, "\"formats\":[");
    for (int i = 0; i < s.format_count; i++) {
        if (i) sb_putc(b, ',');
        sb_puts(b, "{\"fmt\":"); sb_json_str(b, s.formats[i].fmt);
        sb_puts(b, ",\"n\":");   sb_json_i64(b, s.formats[i].n);
        sb_putc(b, '}');
    }
    sb_puts(b, "],");
    sb_puts(b, "\"hires_pct\":");  sb_json_float(b, s.hires_pct);  sb_putc(b, ',');
    sb_puts(b, "\"lyrics_pct\":"); sb_json_float(b, s.lyrics_pct); sb_putc(b, ',');
    sb_puts(b, "\"missing\":");    sb_json_i64(b, s.missing);
    sb_putc(b, '}');
}

static void build_scan(strbuf *b) {
    mn_scan sc;
    memset(&sc, 0, sizeof(sc));
    mn_app_scan_status(g_app, &sc);

    sb_puts(b, "{\"type\":\"scan\",");
    sb_puts(b, "\"active\":");       sb_json_bool(b, sc.active);        sb_putc(b, ',');
    sb_puts(b, "\"found\":");        sb_json_i64(b, sc.found);          sb_putc(b, ',');
    sb_puts(b, "\"processed\":");    sb_json_i64(b, sc.processed);      sb_putc(b, ',');
    sb_puts(b, "\"dirs_scanned\":"); sb_json_i64(b, sc.dirs_scanned);   sb_putc(b, ',');
    sb_puts(b, "\"skipped\":");      sb_json_i64(b, sc.skipped);        sb_putc(b, ',');
    sb_puts(b, "\"tag_errors\":");   sb_json_i64(b, sc.tag_errors);     sb_putc(b, ',');
    sb_puts(b, "\"io_errors\":");    sb_json_i64(b, sc.io_errors);      sb_putc(b, ',');
    sb_puts(b, "\"source\":");       sb_json_str(b, sc.source);
    sb_putc(b, '}');
}

/* ------------------------------------------------------------------------- */
/* Deliver a JSON reply to JS by running window.chrome.webview._emit(<json>). */
/* Runs in the browser process on the UI thread; targets the frame that sent  */
/* the command (or the main frame).                                           */
/* ------------------------------------------------------------------------- */

static void emit_to_frame(cef_frame_t *frame, const char *json) {
    if (!frame || !json) return;

    /* Build:  window.chrome.webview._emit(<json>);  as a UTF-8 string, then
     * hand it to CEF as a cef_string. The JSON is already a valid JS object
     * literal, so it can be embedded directly as the call argument. */
    strbuf js; sb_init(&js);
    sb_puts(&js, "try{window.chrome.webview._emit(");
    sb_puts(&js, json);
    sb_puts(&js, ");}catch(e){}");
    if (js.oom) { sb_free(&js); return; }

    cef_string_t code; cefstr_from_utf8(&code, js.data);
    cef_string_t url;  memset(&url, 0, sizeof(url));
    frame->execute_java_script(frame, &code, &url, 0);
    cef_string_clear(&code);
    sb_free(&js);
}

/* ------------------------------------------------------------------------- */
/* Native folder picker (reply {type:"picked",path:...}).                     */
/* Modern IFileDialog in folder-pick mode, run on its OWN thread so the CEF   */
/* UI thread (and the whole browser) never blocks while the dialog is open.   */
/* Implementation lives after the refbase/INIT_BASE section below.            */
/* ------------------------------------------------------------------------- */

static void pick_folder_and_reply(cef_frame_t *frame);
static void waveform_and_reply(cef_frame_t *frame, int64_t id);
static void tagwrite_and_reply(cef_frame_t *frame, const char *json);
static void artwrite_and_reply(cef_frame_t *frame, const char *json);
static void lyricswrite_and_reply(cef_frame_t *frame, const char *json);
static void resetlibrary_and_reply(cef_frame_t *frame, const char *json);
static void downloadmodel_and_reply(cef_frame_t *frame, const char *json);
static void selectmodel_and_reply(cef_frame_t *frame, const char *json);
static void selectedmodels_and_reply(cef_frame_t *frame, const char *json);
static void refreshart_and_reply(cef_frame_t *frame, const char *json);
static void register_persisted_roots(void);

/* ------------------------------------------------------------------------- */
/* Reveal a file in Explorer with it selected: `explorer /select,"<path>"`.   */
/* Unicode-safe (ShellExecuteW). A directory path (no file) just opens it.    */
/* ------------------------------------------------------------------------- */
static void reveal_in_explorer(const char *utf8_path) {
    wchar_t *wpath = NULL;
    wchar_t  params[1400];
    int      wlen;
    DWORD    attrs;

    if (!utf8_path || !utf8_path[0]) return;
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, NULL, 0);
    if (wlen <= 0) return;
    wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, wlen) <= 0) {
        free(wpath);
        return;
    }

    attrs = GetFileAttributesW(wpath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        /* A folder: open it — explicitly via explorer.exe. (Plain
         * ShellExecuteW("open", path) routes through the shell ASSOCIATION
         * table; a URL-shaped or association-hijacked "path" could launch
         * the default BROWSER. explorer.exe can only be Explorer.) */
        _snwprintf(params, sizeof(params) / sizeof(params[0]),
                   L"\"%s\"", wpath);
        ShellExecuteW(NULL, L"open", L"explorer.exe", params, NULL,
                      SW_SHOWNORMAL);
    } else {
        /* A file (or missing): ask Explorer to select it in its folder. */
        _snwprintf(params, sizeof(params) / sizeof(params[0]),
                   L"/select,\"%s\"", wpath);
        ShellExecuteW(NULL, L"open", L"explorer.exe", params, NULL,
                      SW_SHOWNORMAL);
    }
    free(wpath);
}

/* ------------------------------------------------------------------------- */
/* Command dispatch: parse {cmd:...} JSON, act, and reply for query commands. */
/* Runs in the BROWSER process; replies target `frame`. (ported)             */
/* ------------------------------------------------------------------------- */

/* ==========================================================================
 * Clipboard image -> base64 PNG ("Paste album art"). Reads the registered
 * "PNG" clipboard format when present (browsers put raw PNG there), else
 * converts CF_DIB (24/32bpp BI_RGB / BI_BITFIELDS, bottom-up or top-down)
 * to PNG via stb. Emits {"type":"clipart","ok",b64,mime} — the UI routes it
 * through the normal artwrite pipeline (whole album).
 * ========================================================================== */
/* stbi_write_png_to_mem is defined in artcache.c's stb implementation but
 * the vendored header only declares it inside the IMPLEMENTATION section —
 * without this explicit declaration C implicitly types it as returning int,
 * truncating the 64-bit pointer (instant segfault on x64). */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            int stride_bytes, int x, int y,
                                            int n, int *out_len);

static char *b64_enc(const unsigned char *src, size_t n, size_t *out_n) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((n + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = T[v & 63];
    }
    if (i < n) {
        unsigned v = src[i] << 16;
        int two = (i + 1 < n);
        if (two) v |= src[i + 1] << 8;
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = two ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_n) *out_n = o;
    return out;
}

/* Grab the clipboard image as PNG bytes (malloc'd; caller frees). */
static unsigned char *clip_image_png(size_t *out_n) {
    unsigned char *png = NULL;
    UINT fmt_png = RegisterClipboardFormatA("PNG");
    *out_n = 0;
    if (!OpenClipboard(NULL)) return NULL;
    /* 1) raw PNG straight off the clipboard (browsers, editors) */
    if (fmt_png && IsClipboardFormatAvailable(fmt_png)) {
        HGLOBAL h = GetClipboardData(fmt_png);
        if (h) {
            size_t n = GlobalSize(h);
            const unsigned char *p = (const unsigned char *)GlobalLock(h);
            if (p && n > 8 && p[0] == 0x89 && p[1] == 'P') {
                png = (unsigned char *)malloc(n);
                if (png) { memcpy(png, p, n); *out_n = n; }
            }
            if (p) GlobalUnlock(h);
        }
    }
    /* 2) CF_DIB fallback: convert to RGBA then encode PNG in memory */
    if (!png && IsClipboardFormatAvailable(CF_DIB)) {
        HGLOBAL h = GetClipboardData(CF_DIB);
        const BITMAPINFOHEADER *bi = h ? (const BITMAPINFOHEADER *)GlobalLock(h) : NULL;
        if (bi) fprintf(stderr, "[clipart] dib size=%lu bpp=%d comp=%lu w=%ld h=%ld gsz=%zu\n",
                        (unsigned long)bi->biSize, (int)bi->biBitCount,
                        (unsigned long)bi->biCompression, (long)bi->biWidth,
                        (long)bi->biHeight, (size_t)GlobalSize(h));
        if (bi && bi->biSize >= sizeof(BITMAPINFOHEADER) &&
            (bi->biBitCount == 24 || bi->biBitCount == 32) &&
            (bi->biCompression == BI_RGB || bi->biCompression == BI_BITFIELDS) &&
            bi->biWidth > 0 && bi->biWidth <= 8192 &&
            bi->biHeight != 0 && bi->biHeight <= 8192 && bi->biHeight >= -8192) {
            int w = bi->biWidth;
            int hgt = bi->biHeight > 0 ? bi->biHeight : -bi->biHeight;
            int bottom_up = bi->biHeight > 0;
            int bpp = bi->biBitCount / 8;
            size_t stride = ((size_t)w * bpp + 3) & ~(size_t)3;
            const unsigned char *bits = (const unsigned char *)bi + bi->biSize +
                (bi->biCompression == BI_BITFIELDS ? 12 : 0);
            unsigned char *rgba = (unsigned char *)malloc((size_t)w * hgt * 4);
            if (rgba && GlobalSize(h) >= (size_t)(bits - (const unsigned char *)bi) + stride * hgt) {
                int x, y;
                for (y = 0; y < hgt; y++) {
                    const unsigned char *row = bits + stride *
                        (size_t)(bottom_up ? (hgt - 1 - y) : y);
                    unsigned char *dst = rgba + (size_t)y * w * 4;
                    for (x = 0; x < w; x++) {
                        dst[x * 4 + 0] = row[x * bpp + 2];
                        dst[x * 4 + 1] = row[x * bpp + 1];
                        dst[x * 4 + 2] = row[x * bpp + 0];
                        dst[x * 4 + 3] = 255;   /* DIB alpha is unreliable */
                    }
                }
                {
                    int len = 0;
                    unsigned char *mem = stbi_write_png_to_mem(
                        rgba, w * 4, w, hgt, 4, &len);
                    if (mem && len > 0) {
                        png = (unsigned char *)malloc((size_t)len);
                        if (png) { memcpy(png, mem, (size_t)len); *out_n = (size_t)len; }
                    }
                    free(mem);   /* stb default allocator is malloc/free */
                }
            }
            free(rgba);
        }
        if (bi) GlobalUnlock(h);
    }
    CloseClipboard();
    return png;
}

/* Emit a book's bookmark list: {"type":"bookmarks","album",...,"items":[...]}.
 * Shared by bookmarkadd / bookmarkdel / bookmarklist (mutations reply with
 * the refreshed list so the UI needs no separate ack handling). */
static void emit_bookmarks_for(cef_frame_t *frame, int64_t album) {
    enum { BM_MAX = 64 };
    static int64_t ids[BM_MAX], tids[BM_MAX], pms[BM_MAX], crt[BM_MAX];
    static char    nts[BM_MAX][128];
    int n = mn_app_bookmark_list(g_app, album, ids, tids, pms, nts, crt,
                                 BM_MAX), i;
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"bookmarks\",\"album\":");
    sb_json_i64(&b, album);
    sb_puts(&b, ",\"items\":[");
    for (i = 0; i < n; i++) {
        if (i) sb_putc(&b, ',');
        sb_putc(&b, '{');
        sb_puts(&b, "\"id\":");      sb_json_i64(&b, ids[i]);  sb_putc(&b, ',');
        sb_puts(&b, "\"track\":");   sb_json_i64(&b, tids[i]); sb_putc(&b, ',');
        sb_puts(&b, "\"pos_ms\":");  sb_json_i64(&b, pms[i]);  sb_putc(&b, ',');
        sb_puts(&b, "\"note\":");    sb_json_str(&b, nts[i]);  sb_putc(&b, ',');
        sb_puts(&b, "\"created\":"); sb_json_i64(&b, crt[i]);
        sb_putc(&b, '}');
    }
    sb_puts(&b, "]}");
    if (!b.oom) emit_to_frame(frame, b.data);
    sb_free(&b);
}

static void dispatch_command(cef_frame_t *frame, const char *json) {
    if (!json) return;

    char cmd[64];
    if (!json_get_str(json, "cmd", cmd, sizeof(cmd))) return;

    if (strcmp(cmd, "searchsug") == 0) {
        /* Live search suggestions: independent prefix-match over tracks; the
         * UI derives albums + artists from the rows. Does not disturb the
         * current view. Echoes gen so the UI drops stale replies. */
        char q[256] = {0};
        int  gen = (int)json_get_i64(json, "gen", 0);
        (void)json_get_str(json, "q", q, sizeof(q));
        mn_row rows[24];
        int n = q[0] ? mn_app_search_tracks(g_app, q, 24, rows) : 0;
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"searchsug\",\"gen\":");
        sb_json_int(&b, gen);
        sb_puts(&b, ",\"rows\":[");
        for (int i = 0; i < n; i++) { if (i) sb_putc(&b, ','); append_row(&b, &rows[i]); }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "searchall") == 0) {
        /* Unified search results: one FTS pass, aggregated server-side into
         * three sections — tracks (best rows), albums and artists (deduped
         * from the matched rows, ranked by match count, with art URLs).
         * Read-only: does NOT touch the view filter or the album cache. */
        enum { SA_FETCH = 400, SA_TRACKS = 80, SA_ALBUMS = 40, SA_ARTISTS = 30 };
        char q[256] = {0};
        int  gen = (int)json_get_i64(json, "gen", 0);
        (void)json_get_str(json, "q", q, sizeof(q));
        mn_row *rows = (mn_row *)malloc(sizeof(mn_row) * SA_FETCH);
        int n = (rows && q[0]) ? mn_app_search_tracks(g_app, q, SA_FETCH, rows) : 0;
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"searchall\",\"gen\":");
        sb_json_int(&b, gen);
        sb_puts(&b, ",\"q\":"); sb_json_str(&b, q);
        sb_puts(&b, ",\"total\":"); sb_json_int(&b, n);
        sb_puts(&b, ",\"tracks\":[");
        for (int i = 0; i < n && i < SA_TRACKS; i++) {
            if (i) sb_putc(&b, ',');
            append_row(&b, &rows[i]);
        }
        sb_puts(&b, "],\"albums\":[");
        {
            /* dedup by (album_artist-or-artist, album) — track artist would
             * split a VA/compilation album into one entry per guest artist */
            struct { int first; int count; } alb[SA_ALBUMS];
            int na = 0;
            #define NE_ROW_AA(r) ((r).album_artist[0] ? (r).album_artist : (r).artist)
            for (int i = 0; i < n; i++) {
                if (!rows[i].album[0]) continue;
                int j;
                for (j = 0; j < na; j++) {
                    if (_stricmp(rows[alb[j].first].album, rows[i].album) == 0 &&
                        _stricmp(NE_ROW_AA(rows[alb[j].first]),
                                 NE_ROW_AA(rows[i])) == 0) {
                        alb[j].count++; break;
                    }
                }
                if (j == na && na < SA_ALBUMS) { alb[na].first = i; alb[na].count = 1; na++; }
            }
            #undef NE_ROW_AA
            for (int j = 0; j < na; j++) {
                const mn_row *r = &rows[alb[j].first];
                char art[256];
                /* Same album_artist-first key as append_row so VA albums in
                 * search results carry their cover too. */
                const char *aa = r->album_artist[0] ? r->album_artist
                                                    : r->artist;
                art_url_for(art, sizeof(art), aa, r->album);
                if (j) sb_putc(&b, ',');
                sb_putc(&b, '{');
                sb_puts(&b, "\"title\":");  sb_json_str(&b, r->album);   sb_putc(&b, ',');
                sb_puts(&b, "\"artist\":"); sb_json_str(&b, aa);         sb_putc(&b, ',');
                sb_puts(&b, "\"year\":");   sb_json_int(&b, r->year);    sb_putc(&b, ',');
                sb_puts(&b, "\"matches\":"); sb_json_int(&b, alb[j].count); sb_putc(&b, ',');
                sb_puts(&b, "\"art\":");    sb_json_str(&b, art);
                sb_putc(&b, '}');
            }
        }
        sb_puts(&b, "],\"artists\":[");
        {
            struct { int first; int count; } ar[SA_ARTISTS];
            int na = 0;
            for (int i = 0; i < n; i++) {
                if (!rows[i].artist[0]) continue;
                int j;
                for (j = 0; j < na; j++) {
                    if (_stricmp(rows[ar[j].first].artist, rows[i].artist) == 0) {
                        ar[j].count++; break;
                    }
                }
                if (j == na && na < SA_ARTISTS) { ar[na].first = i; ar[na].count = 1; na++; }
            }
            for (int j = 0; j < na; j++) {
                const mn_row *r = &rows[ar[j].first];
                if (j) sb_putc(&b, ',');
                sb_putc(&b, '{');
                sb_puts(&b, "\"name\":");    sb_json_str(&b, r->artist);   sb_putc(&b, ',');
                sb_puts(&b, "\"matches\":"); sb_json_int(&b, ar[j].count);
                sb_putc(&b, '}');
            }
        }
        sb_puts(&b, "]}");
        free(rows);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "tracks") == 0) {
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 100);
        int     gen    = (int)json_get_i64(json, "gen", 0);
        strbuf b; sb_init(&b);
        build_tracks(&b, offset, count, gen);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "albums") == 0) {
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 100);
        int     gen    = (int)json_get_i64(json, "gen", 0);
        strbuf b; sb_init(&b);
        build_albums(&b, offset, count, gen);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "albumtracks") == 0) {
        int64_t id = json_get_i64(json, "id", 0);
        strbuf b; sb_init(&b);
        build_albumtracks(&b, id);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "artists") == 0) {
        /* mn_facet_dim: MN_FACET_ARTIST == 1 */
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 500);
        strbuf b; sb_init(&b);
        build_facet(&b, "artists", 1, offset, count);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "genres") == 0) {
        /* mn_facet_dim: MN_FACET_GENRE == 4 */
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 500);
        strbuf b; sb_init(&b);
        build_facet(&b, "genres", 4, offset, count);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "years") == 0) {
        /* mn_facet_dim: MN_FACET_YEAR == 5 */
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 500);
        strbuf b; sb_init(&b);
        build_facet(&b, "years", 5, offset, count);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "facettracks") == 0) {
        int     dim = (int)json_get_i64(json, "dim", 1);
        int64_t vid = json_get_i64(json, "value_id", 0);
        strbuf b; sb_init(&b);
        build_facettracks(&b, dim, vid);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlists") == 0) {
        strbuf b; sb_init(&b);
        build_playlists(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlisttracks") == 0) {
        int64_t id = json_get_i64(json, "id", 0);
        strbuf b; sb_init(&b);
        build_playlisttracks(&b, id);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistcreate") == 0) {
        char name[256];
        if (!json_get_str(json, "name", name, sizeof(name)) || !name[0]) {
            snprintf(name, sizeof(name), "New Playlist");
        }
        (void)mn_app_playlist_create(g_app, name);
        strbuf b; sb_init(&b);
        build_playlists(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistrename") == 0) {
        char name[256];
        int64_t id = json_get_i64(json, "id", 0);
        if (json_get_str(json, "name", name, sizeof(name)) && name[0]) {
            mn_app_playlist_rename(g_app, id, name);
        }
        strbuf b; sb_init(&b);
        build_playlists(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistdelete") == 0) {
        int64_t id = json_get_i64(json, "id", 0);
        mn_app_playlist_delete(g_app, id);
        strbuf b; sb_init(&b);
        build_playlists(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistadd") == 0) {
        int64_t id  = json_get_i64(json, "id", 0);
        int64_t tid = json_get_i64(json, "track_id", 0);
        mn_app_playlist_add(g_app, id, tid);
    } else if (strcmp(cmd, "playlistmove") == 0) {
        int64_t id   = json_get_i64(json, "id", 0);
        int64_t from = json_get_i64(json, "from", -1);
        int64_t to   = json_get_i64(json, "to", -1);
        mn_app_playlist_move(g_app, id, from, to);
        strbuf b; sb_init(&b);
        build_playlisttracks(&b, id);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistremoveat") == 0) {
        int64_t id  = json_get_i64(json, "id", 0);
        int64_t pos = json_get_i64(json, "position", 0);
        mn_app_playlist_remove_at(g_app, id, pos);
        strbuf b; sb_init(&b);
        build_playlisttracks(&b, id);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "playlistexport") == 0) {
        /* Export a playlist to .m3u8 (native save dialog on a worker;
         * reply {"type":"plexport","ok":bool,"path":...}). */
        int64_t id = json_get_i64(json, "id", 0);
        char name[256] = {0};
        (void)json_get_str(json, "name", name, sizeof(name));
        frame->base.add_ref(&frame->base);
        playlist_export_start(frame, id, name);
    } else if (strcmp(cmd, "stemexport") == 0) {
        /* Separate the given tracks offline and write their stems (per-stem
         * files or one .mnstem container). Streams {type:"stemexport",...}. */
        frame->base.add_ref(&frame->base);
        stemexport_start(frame, json);
    } else if (strcmp(cmd, "savequeue") == 0) {
        /* Save the current live queue as a new static playlist. */
        char name[256];
        if (!json_get_str(json, "name", name, sizeof(name)) || !name[0]) {
            snprintf(name, sizeof(name), "Saved Queue");
        }
        {
            int64_t pid = mn_app_playlist_create(g_app, name);
            if (pid > 0) {
                enum { QCAP = 500 };
                static mn_queue_item items[QCAP];
                int qcur = -1;
                int qn = mn_app_queue(g_app, items, QCAP, &qcur);
                for (int i = 0; i < qn; i++) {
                    if (items[i].id > 0) mn_app_playlist_add(g_app, pid, items[i].id);
                }
            }
        }
        strbuf b; sb_init(&b);
        build_playlists(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "now") == 0) {
        /* IDLE-CPU: the UI polls "now" ~4 Hz. build_now still runs every poll
         * (its depth-priority enqueue is idempotent and must stay live), but we
         * skip the expensive emit_to_frame — UTF-8->UTF-16 marshal + a V8
         * execute_java_script round-trip — when the payload is byte-identical to
         * the last one we sent. This is the common case whenever nothing is
         * changing (paused, or a long unchanged idle), turning the poll into a
         * cheap string compare. Any real change (position_ms tick, depth state,
         * track switch) differs and emits as before. Static memo keyed on the
         * single now-frame; harmless if a second window ever polls (worst case
         * one redundant emit after a cross-window change). */
        static char *s_now_memo = NULL;
        static size_t s_now_memo_cap = 0;
        strbuf b; sb_init(&b);
        build_now(&b);
        if (!b.oom) {
            size_t len = strlen(b.data);
            if (!s_now_memo || strcmp(s_now_memo, b.data) != 0) {
                emit_to_frame(frame, b.data);
                if (s_now_memo_cap < len + 1) {
                    size_t ncap = len + 1;
                    char *nm = (char *)realloc(s_now_memo, ncap);
                    if (nm) { s_now_memo = nm; s_now_memo_cap = ncap; }
                }
                if (s_now_memo_cap >= len + 1) memcpy(s_now_memo, b.data, len + 1);
                else if (s_now_memo) s_now_memo[0] = '\0';  /* OOM: force next emit */
            }
        }
        sb_free(&b);
    } else if (strcmp(cmd, "playqueue") == 0) {
        int qi = (int)json_get_i64(json, "index", -1);
        mn_app_play_queue_index(g_app, qi);
    } else if (strcmp(cmd, "queuemove") == 0) {
        int from = (int)json_get_i64(json, "from", -1);
        int to   = (int)json_get_i64(json, "to", -1);
        mn_app_queue_move(g_app, from, to);
    } else if (strcmp(cmd, "queueremove") == 0) {
        int idx = (int)json_get_i64(json, "index", -1);
        mn_app_queue_remove(g_app, idx);
    } else if (strcmp(cmd, "queueclear") == 0) {
        mn_app_queue_clear(g_app);
    } else if (strcmp(cmd, "queue") == 0) {
        enum { QWIN = 512 };   /* was 100 — long queues showed a truncated panel */
        static mn_queue_item items[QWIN];   /* dispatch is single-threaded */
        int qcur = -1;
        int qn = mn_app_queue(g_app, items, QWIN, &qcur);
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"queue\",\"current\":");
        sb_json_int(&b, qcur);
        sb_puts(&b, ",\"items\":[");
        for (int i = 0; i < qn; i++) {
            char art[256];
            art_url_for(art, sizeof(art), items[i].album_artist, items[i].album);
            if (i) sb_putc(&b, ',');
            sb_putc(&b, '{');
            sb_puts(&b, "\"index\":");    sb_json_int(&b, i);                    sb_putc(&b, ',');
            sb_puts(&b, "\"id\":");       sb_json_i64(&b, items[i].id);          sb_putc(&b, ',');
            sb_puts(&b, "\"title\":");    sb_json_str(&b, items[i].title);       sb_putc(&b, ',');
            sb_puts(&b, "\"artist\":");   sb_json_str(&b, items[i].artist);      sb_putc(&b, ',');
            /* art-key parts so queue tiles can be artready-targeted */
            sb_puts(&b, "\"album_artist\":"); sb_json_str(&b, items[i].album_artist); sb_putc(&b, ',');
            sb_puts(&b, "\"album\":");    sb_json_str(&b, items[i].album);       sb_putc(&b, ',');
            sb_puts(&b, "\"format\":");   sb_json_str(&b, items[i].format);      sb_putc(&b, ',');
            sb_puts(&b, "\"duration_ms\":"); sb_json_int(&b, items[i].duration_ms); sb_putc(&b, ',');
            sb_puts(&b, "\"bitrate\":");  sb_json_int(&b, items[i].bitrate_kbps); sb_putc(&b, ',');
            sb_puts(&b, "\"sample_rate\":"); sb_json_int(&b, items[i].sample_rate); sb_putc(&b, ',');
            sb_puts(&b, "\"bit_depth\":"); sb_json_int(&b, items[i].bit_depth);  sb_putc(&b, ',');
            sb_puts(&b, "\"liked\":");    sb_json_int(&b, items[i].liked);       sb_putc(&b, ',');
            sb_puts(&b, "\"play_count\":"); sb_json_i64(&b, items[i].play_count); sb_putc(&b, ',');
            sb_puts(&b, "\"art\":");      sb_json_str(&b, art);
            sb_putc(&b, '}');
        }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "scan") == 0) {
        strbuf b; sb_init(&b);
        build_scan(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "audiocaps") == 0) {
        strbuf b; sb_init(&b);
        build_audiocaps(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "audiodevices") == 0) {
        strbuf b; sb_init(&b);
        build_audiodevices(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "setdevice") == 0) {
        int idx = (int)json_get_i64(json, "index", -1);
        if (idx >= 0) (void)mn_app_select_device(g_app, idx);
        /* Re-reply the device list so the UI reflects the new routing. */
        strbuf b; sb_init(&b);
        build_audiodevices(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "folders") == 0) {
        strbuf b; sb_init(&b);
        build_folders(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "folderhidden") == 0) {
        /* {"cmd":"folderhidden","id":N,"hidden":true} — toggle folder
         * visibility, persist, and re-reply the folder list (row/album
         * counts are already dirty by the time the UI re-queries them). */
        int64_t id     = json_get_i64(json, "id", 0);
        bool    hidden = json_get_bool(json, "hidden", true);
        if (id > 0) (void)mn_app_folder_set_hidden(g_app, id, hidden);
        strbuf b; sb_init(&b);
        build_folders(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "sort") == 0) {
        /* {"cmd":"sort","by":"title|artist|album|genre|year|duration|rating|
         *  plays|added|track"[,"asc":bool]} — recency/popularity keys default
         * to DESCENDING (newest/most first), the rest to ascending. */
        char by[32] = {0};
        json_get_str(json, "by", by, sizeof(by));
        mn_sort key = MN_SORT_TITLE;
        bool dflt_asc = true;
        if      (strcmp(by, "title") == 0)    { key = MN_SORT_TITLE; }
        else if (strcmp(by, "artist") == 0)   { key = MN_SORT_ARTIST; }
        else if (strcmp(by, "album") == 0)    { key = MN_SORT_ALBUM; }
        else if (strcmp(by, "genre") == 0)    { key = MN_SORT_GENRE; }
        else if (strcmp(by, "year") == 0)     { key = MN_SORT_YEAR; dflt_asc = false; }
        else if (strcmp(by, "duration") == 0) { key = MN_SORT_DURATION; }
        else if (strcmp(by, "rating") == 0)   { key = MN_SORT_RATING; dflt_asc = false; }
        else if (strcmp(by, "plays") == 0)    { key = MN_SORT_PLAY_COUNT; dflt_asc = false; }
        else if (strcmp(by, "played") == 0)   { key = MN_SORT_LAST_PLAYED; dflt_asc = false; }
        else if (strcmp(by, "added") == 0)    { key = MN_SORT_DATE_ADDED; dflt_asc = false; }
        else if (strcmp(by, "created") == 0)  { key = MN_SORT_DATE_CREATED; dflt_asc = false; }
        else if (strcmp(by, "track") == 0)    { key = MN_SORT_TRACK_NO; }
        mn_app_set_sort(g_app, key, json_get_bool(json, "asc", dflt_asc));
    } else if (strcmp(cmd, "view") == 0) {
        int v = (int)json_get_i64(json, "v", 0);
        if (v < 0) v = 0;
        if (v > MN_VIEW_PLAYLISTS) v = MN_VIEW_PLAYLISTS;
        mn_app_set_view(g_app, (mn_view)v);
    } else if (strcmp(cmd, "search") == 0) {
        char q[512];
        json_get_str(json, "q", q, sizeof(q));
        mn_app_set_search(g_app, q);
    } else if (strcmp(cmd, "play") == 0) {
        mn_app_play_row(g_app, json_get_i64(json, "id", 0));
    } else if (strcmp(cmd, "playalbum") == 0) {
        /* Optional "track": start the album queue at that track (queue
         * context = the album list the user played from). */
        mn_app_play_album_track(g_app, json_get_i64(json, "id", 0),
                                json_get_i64(json, "track", 0));
    } else if (strcmp(cmd, "queuenext") == 0) {
        /* {"cmd":"queuenext","id":<track>} or {"albumid":<album>} — insert
         * after the current track without interrupting playback. */
        mn_app_queue_next(g_app, json_get_i64(json, "id", 0),
                          json_get_i64(json, "albumid", 0));
    } else if (strcmp(cmd, "queuelast") == 0) {
        /* {"cmd":"queuelast","id":<track>} or {"albumid":<album>} — append. */
        mn_app_queue_last(g_app, json_get_i64(json, "id", 0),
                          json_get_i64(json, "albumid", 0));
    } else if (strcmp(cmd, "removetrack") == 0) {
        /* {"cmd":"removetrack","id":N} — drop the row (file untouched). */
        int64_t id = json_get_i64(json, "id", 0);
        bool ok = mn_app_remove_track(g_app, id);
        char m[96];
        snprintf(m, sizeof(m),
                 "{\"type\":\"removed\",\"id\":%lld,\"ok\":%s}",
                 (long long)id, ok ? "true" : "false");
        emit_to_frame(frame, m);
    } else if (strcmp(cmd, "deletetracks") == 0) {
        /* Media Manager duplicates: {"ids":[...],"tag":N} — move files to
         * the Windows RECYCLE BIN (never a hard delete) and drop the rows
         * of files that actually left the disk. Shell ops are slow —
         * WORKER thread (reply type "deletetracks", echoes "tag"). */
        frame->base.add_ref(&frame->base);
        deletetracks_start(frame, json);
    } else if (strcmp(cmd, "convert") == 0) {
        /* Format conversion stub — acknowledge so the UI can show "coming
         * soon" without a silent no-op. */
        emit_to_frame(frame,
            "{\"type\":\"convert\",\"ok\":false,\"msg\":\"coming soon\"}");
    } else if (strcmp(cmd, "toggle") == 0) {
        mn_app_toggle_pause(g_app);
    } else if (strcmp(cmd, "next") == 0) {
        mn_app_next(g_app);
    } else if (strcmp(cmd, "prev") == 0) {
        mn_app_prev(g_app);
    } else if (strcmp(cmd, "seek") == 0) {
        int64_t ms = json_get_i64(json, "ms", 0);
        if (ms < 0) ms = 0;
        mn_app_seek_ms(g_app, (uint64_t)ms);
    } else if (strcmp(cmd, "volume") == 0) {
        double v = json_get_double(json, "v", 1.0);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        mn_app_set_volume(g_app, (float)v);
    } else if (strcmp(cmd, "eq") == 0) {
        /* Unified DSP/EQ command. {"cmd":"eq","action":...}
         *   enable  {on}                 — master DSP chain on/off
         *   eqon    {on}                 — EQ stage on/off within the chain
         *   band    {band,gain}          — one 10-band gain (dB)
         *   preset  {preset}             — apply a built-in preset (index)
         *   preamp  {gain}               — pre-EQ gain (dB)
         *   balance {v}                  — stereo balance -1..+1
         *   limiter {on,threshold,ceiling}
         *   get                          — reply with current state + band meta */
        char act[32];
        if (!json_get_str(json, "action", act, sizeof(act))) {
            act[0] = 'g'; act[1] = 'e'; act[2] = 't'; act[3] = '\0';
        }
        if (strcmp(act, "enable") == 0) {
            mn_app_set_dsp_enabled(g_app, json_get_bool(json, "on", true) ? 1 : 0);
        } else if (strcmp(act, "eqon") == 0) {
            mn_app_set_eq_enabled(g_app, json_get_bool(json, "on", true) ? 1 : 0);
        } else if (strcmp(act, "band") == 0) {
            int   band = (int)json_get_i64(json, "band", 0);
            float gain = (float)json_get_double(json, "gain", 0.0);
            mn_app_set_eq_band(g_app, band, gain);
        } else if (strcmp(act, "preset") == 0) {
            int   preset = (int)json_get_i64(json, "preset", 0);
            float gains[MN_DSP_EQ_BANDS]; float preamp = 0.0f;
            mn_app_set_eq_preset(g_app, preset, gains, &preamp);
        } else if (strcmp(act, "preamp") == 0) {
            mn_app_set_preamp(g_app, (float)json_get_double(json, "gain", 0.0));
        } else if (strcmp(act, "balance") == 0) {
            mn_app_set_balance(g_app, (float)json_get_double(json, "v", 0.0));
        } else if (strcmp(act, "limiter") == 0) {
            mn_app_set_limiter(g_app, json_get_bool(json, "on", false) ? 1 : 0,
                               (float)json_get_double(json, "threshold", -1.0),
                               (float)json_get_double(json, "ceiling", -0.1));
        } else if (strcmp(act, "master") == 0) {
            mn_app_set_master(g_app, (float)json_get_double(json, "gain", 0.0));
        }
        /* Always reply with the resolved state so the UI reflects presets etc. */
        {
            float gains[MN_DSP_EQ_BANDS]; float preamp = 0.0f; int eqon = 0;
            float balance = 0.0f, lthr = -1.0f, lceil = -0.1f, master = 0.0f;
            int   limon = 0;
            uint32_t bi;
            strbuf b; sb_init(&b);
            mn_app_get_eq(g_app, gains, &preamp, &eqon);
            mn_app_get_dsp_extra(g_app, &balance, &limon, &lthr, &lceil, &master);
            sb_puts(&b, "{\"type\":\"eq\",\"enabled\":");
            sb_json_int(&b, mn_app_get_dsp_enabled(g_app));
            sb_puts(&b, ",\"eq_enabled\":"); sb_json_int(&b, eqon);
            sb_puts(&b, ",\"preamp\":");     sb_json_float(&b, preamp);
            sb_puts(&b, ",\"balance\":");    sb_json_float(&b, balance);
            sb_puts(&b, ",\"limiter\":");    sb_json_int(&b, limon);
            sb_puts(&b, ",\"lim_threshold\":"); sb_json_float(&b, lthr);
            sb_puts(&b, ",\"lim_ceiling\":");   sb_json_float(&b, lceil);
            sb_puts(&b, ",\"master\":");     sb_json_float(&b, master);
            sb_puts(&b, ",\"bands\":[");
            for (bi = 0; bi < MN_DSP_EQ_BANDS; bi++) {
                if (bi) sb_putc(&b, ',');
                sb_json_float(&b, gains[bi]);
            }
            sb_puts(&b, "],\"freqs\":[");
            for (bi = 0; bi < MN_DSP_EQ_BANDS; bi++) {
                if (bi) sb_putc(&b, ',');
                sb_json_float(&b, mn_dsp_band_frequency(bi));
            }
            sb_puts(&b, "],\"presets\":[");
            for (bi = 0; bi < (uint32_t)MN_DSP_EQ_PRESET_COUNT; bi++) {
                if (bi) sb_putc(&b, ',');
                sb_json_str(&b, mn_dsp_preset_name((mn_dsp_eq_preset)bi));
            }
            sb_puts(&b, "]}");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "resume") == 0) {
        /* Restore the last session's track PAUSED at its saved position —
         * atomic on the C side so startup can never blast audio. */
        int64_t id = json_get_i64(json, "id", 0);
        int64_t ms = json_get_i64(json, "ms", 0);
        mn_app_resume_row(g_app, id, ms);
    } else if (strcmp(cmd, "likedonly") == 0) {
        mn_app_set_liked_only(g_app, json_get_bool(json, "on", false));
    } else if (strcmp(cmd, "importplaylists") == 0) {
        int n = mn_app_import_playlists(g_app);
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"plimport\",\"n\":");
            sb_json_int(&b, n);
            sb_putc(&b, '}');
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
        {
            strbuf b; sb_init(&b);
            build_playlists(&b);
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "cacheinfo") == 0) {
        /* Storage panel: recursive walks over 5 cache dirs (incl. the CEF
         * browser cache — thousands of files). WORKER thread; walking them
         * here stalled every queued bridge command for the duration. */
        frame->base.add_ref(&frame->base);
        cacheop_start(frame, NULL);
    } else if (strcmp(cmd, "clearcache") == 0) {
        /* Functional per-cache Clear buttons (Settings -> Storage).
         * which = stems | art | webart | depth. Deletion of potentially
         * thousands of files -> WORKER thread (replies: cachecleared, then
         * fresh cacheinfo). */
        char which[24] = {0};
        (void)json_get_str(json, "which", which, sizeof(which));
        frame->base.add_ref(&frame->base);
        cacheop_start(frame, which);
    } else if (strcmp(cmd, "cleardepth") == 0) {
        /* Wipe every generated depth map (art changed / model changed), then
         * sweep so they regenerate in the background. */
        depth_clear_maps();
        depth_selfheal_sweep();
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"cleardepth\",\"ok\":true}");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "redepth") == 0) {
        /* Per-album: delete this album's depth map + re-enqueue generation. */
        char artist[256] = {0}, album[256] = {0};
        (void)json_get_str(json, "artist", artist, sizeof(artist));
        (void)json_get_str(json, "album",  album,  sizeof(album));
        if (album[0]) {
            char png[MN_ART_PATH_MAX];
            if (mn_app_art_check(g_app, artist, album, png, sizeof(png))) {
                size_t pn = strlen(png);
                if (pn > 4) {
                    char depth[MN_ART_PATH_MAX + 32], hires[MN_ART_PATH_MAX + 32];
                    snprintf(depth, sizeof(depth), "%.*s.depth.png",
                             (int)(pn - 4), png);
                    snprintf(hires, sizeof(hires), "%.*s.hires.png",
                             (int)(pn - 4), png);
                    DeleteFileA(depth);
                    DeleteFileA(hires);   /* re-extract hi-res too (art may be new) */
                }
                depth_enqueue(png, artist, album);
            }
        }
    } else if (strcmp(cmd, "artfetch") == 0) {
        /* Online cover-art fetch (iTunes -> Deezer, then ingest). Network +
         * download + decode is 1-30 s — WORKER thread; running it here froze
         * transport/seek/now polls for the duration (bridge replies queue
         * behind the running handler). */
        char artist[256] = {0}, album[256] = {0};
        int  res = (int)json_get_i64(json, "res", 1200);
        (void)json_get_str(json, "artist", artist, sizeof(artist));
        (void)json_get_str(json, "album",  album,  sizeof(album));
        frame->base.add_ref(&frame->base);
        artfetch_start(frame, artist, album, res);
    } else if (strcmp(cmd, "sleeptimer") == 0) {
        int minutes = (int)json_get_i64(json, "minutes", 0);
        mn_app_set_sleep_timer(g_app, minutes);
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"sleeptimer\",\"remaining\":");
        sb_json_int(&b, mn_app_get_sleep_remaining(g_app));
        sb_putc(&b, '}');
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "spectrum") == 0) {
        float bars[64];
        int n = mn_app_get_spectrum(g_app, bars, 64);
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"spectrum\",\"bars\":[");
        for (int i = 0; i < n; i++) { if (i) sb_putc(&b, ','); sb_json_float(&b, bars[i]); }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "shuffle") == 0) {
        mn_app_set_shuffle(g_app, json_get_bool(json, "on", false));
    } else if (strcmp(cmd, "repeat") == 0) {
        mn_app_cycle_repeat(g_app);
    } else if (strcmp(cmd, "rating") == 0) {
        int64_t id    = json_get_i64(json, "id", 0);
        int     stars = (int)json_get_i64(json, "stars", 0);
        mn_app_set_rating(g_app, id, stars);
    } else if (strcmp(cmd, "like") == 0) {
        /* {"cmd":"like","id":N,"v":1|-1|0} — absolute thumbs state (the UI
         * owns the toggle semantics). Ack with the applied value so the
         * clicked row can repaint without a full window re-query. */
        int64_t id = json_get_i64(json, "id", 0);
        int     v  = (int)json_get_i64(json, "v", 0);
        if (id > 0) {
            mn_app_set_liked(g_app, id, v);
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"liked\",\"id\":");
            sb_json_i64(&b, id);
            sb_puts(&b, ",\"v\":");
            sb_json_int(&b, mn_app_get_liked(g_app, id));
            sb_putc(&b, '}');
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "stats") == 0) {
        strbuf b; sb_init(&b);
        build_stats(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "removefolder") == 0) {
        /* {"cmd":"removefolder","id":N} — permanently drop the folder's
         * tracks + the folder itself, then re-reply the folder list. */
        int64_t id = json_get_i64(json, "id", 0);
        if (id > 0) (void)mn_app_remove_folder(g_app, id);
        strbuf b; sb_init(&b);
        build_folders(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "addfolder") == 0) {
        char path[1024], kind[32] = {0};
        if (json_get_str(json, "path", path, sizeof(path)) && path[0]) {
            /* Upsert into the persisted roots file (dedup + last-scan stamp;
             * the old append-only writer duplicated lines on re-add). */
            (void)json_get_str(json, "kind", kind, sizeof(kind));
            roots_file_touch(path, kind[0] ? kind : NULL, false);
            mn_app_add_folder(g_app, path);
            sync_audiobook_roots();   /* kind may partition the library */
        }
    } else if (strcmp(cmd, "category") == 0) {
        /* Per-kind library switch: {"kind":""|"audiobook"|"ost"|...}.
         * Back-compat: {"ab":true} means kind "audiobook". */
        char kind[32] = {0};
        if (!json_get_str(json, "kind", kind, sizeof(kind)) || !kind[0]) {
            if (json_get_bool(json, "ab", false))
                snprintf(kind, sizeof(kind), "audiobook");
        }
        mn_app_set_category_kind(g_app, kind_is_music(kind) ? "" : kind);
    } else if (strcmp(cmd, "kinds") == 0) {
        strbuf b; sb_init(&b);
        build_kinds(&b);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "mmtracks") == 0) {
        /* Media-tool scope window: {"prefix","offset","count"} — rows under
         * the prefix, independent of the current view/category. */
        char prefix[1024] = {0};
        int64_t offset = json_get_i64(json, "offset", 0);
        int     count  = (int)json_get_i64(json, "count", 200);
        int64_t total  = 0;
        (void)json_get_str(json, "prefix", prefix, sizeof(prefix));
        if (count > 200) count = 200;
        if (count < 1)   count = 1;
        {
            mn_row rows[200];
            int n = mn_app_tracks_under(g_app, prefix, offset, count, rows, &total);
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"mmtracks\",\"prefix\":"); sb_json_str(&b, prefix);
            sb_puts(&b, ",\"offset\":"); sb_json_i64(&b, offset);
            sb_puts(&b, ",\"total\":");  sb_json_i64(&b, total);
            sb_puts(&b, ",\"rows\":[");
            for (int i = 0; i < n; i++) {
                if (i) sb_putc(&b, ',');
                append_row(&b, &rows[i]);
            }
            sb_puts(&b, "]}");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "bookresume") == 0) {
        /* Last position within a book (book_progress DB): {"album":id} ->
         * {"type":"bookresume","album","track","pos_ms","percent",
         *  "finished"} (0s when none). */
        int64_t album = json_get_i64(json, "album", 0);
        int64_t track = 0, pos = 0;
        double  pct = 0; bool fin = false;
        (void)mn_app_book_get(g_app, album, &track, &pos, &pct, &fin);
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"bookresume\",\"album\":"); sb_json_i64(&b, album);
            sb_puts(&b, ",\"track\":");    sb_json_i64(&b, track);
            sb_puts(&b, ",\"pos_ms\":");   sb_json_i64(&b, pos);
            sb_puts(&b, ",\"percent\":");  sb_json_float(&b, (float)pct);
            sb_puts(&b, ",\"finished\":"); sb_json_bool(&b, fin);
            sb_putc(&b, '}');
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "continuebooks") == 0) {
        /* Continue-Listening shelf feed: most-recently-played books with
         * progress + art. {"max":N (default 12)} ->
         * {"type":"continuebooks","books":[{album_id,track_id,album,artist,
         *  chapter,pos_ms,duration_ms,percent,finished,updated,art},...]} */
        enum { CB_MAX = 24 };
        static mn_book books[CB_MAX];
        int maxn = (int)json_get_i64(json, "max", 12);
        int n, i;
        if (maxn < 1) maxn = 1;
        if (maxn > CB_MAX) maxn = CB_MAX;
        n = mn_app_recent_books(g_app, books, maxn);
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"continuebooks\",\"books\":[");
            for (i = 0; i < n; i++) {
                mn_book *bk = &books[i];
                char art[256];
                /* art key matches the album grid: ALBUM artist, album */
                art_url_for(art, sizeof(art),
                            bk->album_artist[0] ? bk->album_artist : bk->artist,
                            bk->album);
                if (i) sb_putc(&b, ',');
                sb_putc(&b, '{');
                sb_puts(&b, "\"album_id\":");    sb_json_i64(&b, bk->album_id);   sb_putc(&b, ',');
                sb_puts(&b, "\"track_id\":");    sb_json_i64(&b, bk->track_id);   sb_putc(&b, ',');
                sb_puts(&b, "\"album\":");       sb_json_str(&b, bk->album);      sb_putc(&b, ',');
                sb_puts(&b, "\"artist\":");      sb_json_str(&b, bk->album_artist[0] ? bk->album_artist : bk->artist); sb_putc(&b, ',');
                sb_puts(&b, "\"chapter\":");     sb_json_str(&b, bk->title);      sb_putc(&b, ',');
                sb_puts(&b, "\"pos_ms\":");      sb_json_i64(&b, bk->pos_ms);     sb_putc(&b, ',');
                sb_puts(&b, "\"duration_ms\":"); sb_json_i64(&b, bk->duration_ms);sb_putc(&b, ',');
                sb_puts(&b, "\"percent\":");     sb_json_float(&b, (float)bk->percent); sb_putc(&b, ',');
                sb_puts(&b, "\"finished\":");    sb_json_bool(&b, bk->finished);  sb_putc(&b, ',');
                sb_puts(&b, "\"updated\":");     sb_json_i64(&b, bk->updated);    sb_putc(&b, ',');
                sb_puts(&b, "\"art\":");         sb_json_str(&b, art);
                sb_putc(&b, '}');
            }
            sb_puts(&b, "]}");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "copytext") == 0) {
        /* Copy text to the system clipboard natively — reliable regardless
         * of web-permission state (used for the BTC donation address). */
        char txt[512];
        json_get_str(json, "text", txt, sizeof(txt));
        if (txt[0] && OpenClipboard(NULL)) {
            int wl = MultiByteToWideChar(CP_UTF8, 0, txt, -1, NULL, 0);
            HGLOBAL h = (wl > 0) ? GlobalAlloc(GMEM_MOVEABLE,
                                               (SIZE_T)wl * sizeof(wchar_t))
                                 : NULL;
            if (h) {
                wchar_t *w = (wchar_t *)GlobalLock(h);
                if (w) {
                    MultiByteToWideChar(CP_UTF8, 0, txt, -1, w, wl);
                    GlobalUnlock(h);
                    EmptyClipboard();
                    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                } else GlobalFree(h);
            }
            CloseClipboard();
        }
    } else if (strcmp(cmd, "openurl") == 0) {
        /* Open an EXTERNAL link in the system browser / mail client:
         * {"url":"https://…"}. Scheme-guarded (http/https/mailto only) so a
         * malformed value can never execute a local path. */
        char url[1024];
        json_get_str(json, "url", url, sizeof(url));
        if (strncmp(url, "https://", 8) == 0 ||
            strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "mailto:", 7) == 0) {
            wchar_t wurl[1024];
            if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024) > 0) {
                ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
            }
        }
    } else if (strcmp(cmd, "clipart") == 0) {
        /* Clipboard image for "Paste album art": {"type":"clipart","ok",
         * "b64","mime"}. The UI then writes it via the normal artwrite
         * pipeline (whole album). ~64MB PNG guard. */
        size_t png_n = 0;
        unsigned char *png = clip_image_png(&png_n);
        strbuf b; sb_init(&b);
        if (png && png_n > 0 && png_n < 64u * 1024 * 1024) {
            size_t b64n = 0;
            char *b64 = b64_enc(png, png_n, &b64n);
            if (b64) {
                sb_puts(&b, "{\"type\":\"clipart\",\"ok\":true,\"mime\":\"image/png\",\"b64\":\"");
                sb_puts(&b, b64);
                sb_puts(&b, "\"}");
                free(b64);
            } else {
                sb_puts(&b, "{\"type\":\"clipart\",\"ok\":false}");
            }
        } else {
            sb_puts(&b, "{\"type\":\"clipart\",\"ok\":false}");
        }
        free(png);
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "bookmarkadd") == 0) {
        /* {"album":id,"track":id,"pos_ms":N,"note":"..."} -> refreshed list */
        int64_t album = json_get_i64(json, "album", 0);
        char note[128];
        json_get_str(json, "note", note, sizeof(note));
        (void)mn_app_bookmark_add(g_app, album,
                                  json_get_i64(json, "track", 0),
                                  json_get_i64(json, "pos_ms", 0),
                                  note, (int64_t)time(NULL));
        emit_bookmarks_for(frame, album);
    } else if (strcmp(cmd, "bookmarkdel") == 0) {
        /* {"id":bookmarkId,"album":id} -> refreshed list */
        int64_t album = json_get_i64(json, "album", 0);
        mn_app_bookmark_del(g_app, json_get_i64(json, "id", 0));
        emit_bookmarks_for(frame, album);
    } else if (strcmp(cmd, "bookmarklist") == 0) {
        emit_bookmarks_for(frame, json_get_i64(json, "album", 0));
    } else if (strcmp(cmd, "speed") == 0) {
        /* Pitch-preserved playback speed: {"v":1.5} (0.5..3.0 clamped).
         * Reply echoes the applied value: {"type":"speed","v":1.5}. */
        double v = json_get_double(json, "v", 1.0);
        if (v < 0.5) v = 0.5;
        if (v > 3.0) v = 3.0;
        mn_app_set_speed(g_app, (float)v);
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"speed\",\"v\":");
            sb_json_float(&b, (float)mn_app_get_speed(g_app));
            sb_putc(&b, '}');
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "bookchapters") == 0) {
        /* Per-chapter remembered positions within one book: {"album":id} ->
         * {"type":"bookchapters","album":id,"chapters":[{"track":id,
         *  "pos_ms":N},...]} — the expand panel decorates chapter rows and
         * resumes a chapter at its own spot. */
        enum { BC_MAX = 512 };
        static int64_t bc_ids[BC_MAX], bc_pos[BC_MAX];
        int64_t album = json_get_i64(json, "album", 0);
        int n = mn_app_book_chapters(g_app, album, bc_ids, bc_pos, BC_MAX), i;
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"bookchapters\",\"album\":"); sb_json_i64(&b, album);
        sb_puts(&b, ",\"chapters\":[");
        for (i = 0; i < n; i++) {
            if (i) sb_putc(&b, ',');
            sb_putc(&b, '{');
            sb_puts(&b, "\"track\":");  sb_json_i64(&b, bc_ids[i]); sb_putc(&b, ',');
            sb_puts(&b, "\"pos_ms\":"); sb_json_i64(&b, bc_pos[i]);
            sb_putc(&b, '}');
        }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "bookbadges") == 0) {
        /* Per-book progress badges for the grid tiles: every touched book's
         * completion. {"type":"bookbadges","badges":[{"album_id":N,
         * "percent":0.42,"finished":false},...]}. Small (touched books only,
         * ~dozens) — the UI keeps a map and decorates tiles at bind time. */
        enum { BB_MAX = 1024 };
        static int64_t bb_ids[BB_MAX];
        static double  bb_pct[BB_MAX];
        static bool    bb_fin[BB_MAX];
        int n = mn_app_book_badges(g_app, bb_ids, bb_pct, bb_fin, BB_MAX), i;
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"bookbadges\",\"badges\":[");
        for (i = 0; i < n; i++) {
            if (i) sb_putc(&b, ',');
            sb_putc(&b, '{');
            sb_puts(&b, "\"album_id\":"); sb_json_i64(&b, bb_ids[i]); sb_putc(&b, ',');
            sb_puts(&b, "\"percent\":");  sb_json_float(&b, (float)bb_pct[i]); sb_putc(&b, ',');
            sb_puts(&b, "\"finished\":"); sb_json_bool(&b, bb_fin[i]);
            sb_putc(&b, '}');
        }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "kindstats") == 0) {
        /* Lifetime listening per kind (hours-listened for audiobooks etc.) */
        char f[1400], line[128];
        FILE *sf;
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"kindstats\",\"stats\":[");
        datafile_path(f, sizeof(f), "listen_stats.txt");
        sf = fopen(f, "r");
        if (sf) {
            int first = 1;
            while (fgets(line, sizeof(line), sf)) {
                char *bar = strchr(line, '|');
                long long msv;
                size_t ln;
                if (!bar) continue;
                *bar = 0;
                msv = _strtoi64(bar + 1, NULL, 10);
                ln = strlen(line);
                while (ln && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = 0;
                if (!ln) continue;
                if (!first) sb_putc(&b, ',');
                first = 0;
                sb_puts(&b, "{\"kind\":"); sb_json_str(&b, line);
                sb_puts(&b, ",\"ms\":");   sb_json_i64(&b, msv);
                sb_putc(&b, '}');
            }
            fclose(sf);
        }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "setrootkind") == 0) {
        /* Folders view: designate a folder's content kind (music/audiobook/
         * custom). Upserts the roots registry, re-syncs the per-kind split,
         * and replies with the fresh kinds list for the sidebar. */
        char path[1024], kind[32] = {0};
        if (json_get_str(json, "path", path, sizeof(path)) && path[0]) {
            (void)json_get_str(json, "kind", kind, sizeof(kind));
            roots_file_touch(path, kind[0] ? kind : "music", false);
            sync_audiobook_roots();
            {
                strbuf b; sb_init(&b);
                build_kinds(&b);
                if (!b.oom) emit_to_frame(frame, b.data);
                sb_free(&b);
            }
        }
    } else if (strcmp(cmd, "pickfolder") == 0) {
        pick_folder_and_reply(frame);
    } else if (strcmp(cmd, "rescan") == 0) {
        /* Replay persisted roots (folder_kinds.txt) so a fresh session can
         * rescan, then incremental-rescan everything registered. */
        register_persisted_roots();
        roots_file_touch(NULL, NULL, false);   /* stamp last-scanned = now */
        mn_app_rescan(g_app);
    } else if (strcmp(cmd, "roots") == 0) {
        /* Settings -> Library: the manually-added roots with per-root stats
         * (tracks/albums/bytes/newest + last-scan stamp). Worker (DB scans). */
        frame->base.add_ref(&frame->base);
        roots_start(frame);
    } else if (strcmp(cmd, "removetree") == 0) {
        /* Unified root removal: drop EVERY library folder under the root's
         * path prefix (tracks + folder rows), the registry entry, and the
         * per-kind split; reply with the fresh roots list. Files on disk
         * are untouched. */
        char path[1024];
        if (json_get_str(json, "path", path, sizeof(path)) && path[0]) {
            size_t plen = strlen(path);
            mn_folder *fl = (mn_folder *)malloc(sizeof(mn_folder) * NE_MAX_FOLDERS);
            int nf = fl ? (int)mn_app_folder_list(g_app, fl, NE_MAX_FOLDERS) : 0;
            int i;
            for (i = 0; i < nf; i++) {
                if (_strnicmp(fl[i].path, path, plen) == 0)
                    (void)mn_app_remove_folder(g_app, fl[i].id);
            }
            free(fl);
            roots_file_touch(path, NULL, true);
            sync_audiobook_roots();
            {
                strbuf b; sb_init(&b);
                build_kinds(&b);
                if (!b.oom) emit_to_frame(frame, b.data);
                sb_free(&b);
            }
            frame->base.add_ref(&frame->base);
            roots_start(frame);          /* fresh unified list */
        }
    } else if (strcmp(cmd, "removeroot") == 0) {
        /* Drop a root from the persisted list (the UI separately removes or
         * hides its content via removefolder/folderhidden). */
        char path[1024];
        if (json_get_str(json, "path", path, sizeof(path)) && path[0]) {
            roots_file_touch(path, NULL, true);
        }
        frame->base.add_ref(&frame->base);
        roots_start(frame);              /* reply with the fresh list */
    } else if (strcmp(cmd, "resetlibrary") == 0) {
        resetlibrary_and_reply(frame, json);
    } else if (strcmp(cmd, "purgemissing") == 0) {
        /* Walks + deletes potentially thousands of missing-flagged rows —
         * WORKER thread (reply: {"type":"purgemissing","purged":N}). */
        frame->base.add_ref(&frame->base);
        purgemissing_start(frame);
    } else if (strcmp(cmd, "backupnow") == 0) {
        /* Forced db backup rotation (bypasses the 6 h gate). The SQLite
         * Backup API can take seconds on a big library — WORKER thread
         * (reply: {"type":"backup","ok":bool}). */
        frame->base.add_ref(&frame->base);
        backupnow_start(frame);
    } else if (strcmp(cmd, "reinfer") == 0) {
        /* Backfill filename/folder-inferred tags onto untagged rows —
         * WORKER thread (reply: {"type":"reinfer","updated":N}). */
        frame->base.add_ref(&frame->base);
        reinfer_start(frame);
    } else if (strcmp(cmd, "syncstatus") == 0) {
        /* Current sync status snapshot (same shape as the live events). */
        char state[16];
        EnterCriticalSection(&g_sync_cs);
        snprintf(state, sizeof(state), "%s", g_sync_state);
        LeaveCriticalSection(&g_sync_cs);
        strbuf b; sb_init(&b);
        sync_status_json(&b, state, 0, 0, 0, "");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "syncsethost") == 0) {
        /* {"cmd":"syncsethost","host":"192.168.1.42","port":8797} —
         * persist to sync\host.txt ("host|port"), then re-emit status. */
        char raw[160] = {0}, host[128];
        int  port = (int)json_get_i64(json, "port", NE_SYNC_DEFAULT_PORT);
        size_t j = 0;
        (void)json_get_str(json, "host", raw, sizeof(raw));
        /* keep printable chars only; '|' is the persist separator */
        for (const char *p = raw; *p && j + 1 < sizeof(host); ++p) {
            if ((unsigned char)*p > 0x20 && *p != '|') host[j++] = *p;
        }
        host[j] = 0;
        EnterCriticalSection(&g_sync_cs);
        snprintf(g_sync_host, sizeof(g_sync_host), "%s", host);
        g_sync_port = (port > 0 && port <= 65535) ? port
                                                  : NE_SYNC_DEFAULT_PORT;
        LeaveCriticalSection(&g_sync_cs);
        sync_state_save_host();
        {
            char state[16];
            EnterCriticalSection(&g_sync_cs);
            snprintf(state, sizeof(state), "%s", g_sync_state);
            LeaveCriticalSection(&g_sync_cs);
            strbuf b; sb_init(&b);
            sync_status_json(&b, state, 0, 0, 0, "");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "syncauto") == 0) {
        /* {"cmd":"syncauto","on":bool[,"minutes":N]} — persisted; the
         * 5-minute heal tick runs the flow when on + host set + last
         * success older than the configured interval. */
        InterlockedExchange(&g_sync_auto,
                            json_get_bool(json, "on", false) ? 1 : 0);
        {
            long mins = (long)json_get_i64(json, "minutes", 0);
            if (mins >= 5 && mins <= 24 * 60)
                InterlockedExchange(&g_sync_interval_min, mins);
        }
        sync_state_save_auto();
        sync_state_save_fields();
        {
            char state[16];
            EnterCriticalSection(&g_sync_cs);
            snprintf(state, sizeof(state), "%s", g_sync_state);
            LeaveCriticalSection(&g_sync_cs);
            strbuf b; sb_init(&b);
            sync_status_json(&b, state, 0, 0, 0, "");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "syncfields") == 0) {
        /* {"cmd":"syncfields","likes":b,"ratings":b,"plays":b} — which data
         * groups participate in sync (snapshot AND merge). Persisted. */
        LONG m = 0;
        if (json_get_bool(json, "likes",   true)) m |= 1;
        if (json_get_bool(json, "ratings", true)) m |= 2;
        if (json_get_bool(json, "plays",   true)) m |= 4;
        InterlockedExchange(&g_sync_fields, m);
        sync_fields_apply_to_app();
        sync_state_save_fields();
        {
            char state[16];
            EnterCriticalSection(&g_sync_cs);
            snprintf(state, sizeof(state), "%s", g_sync_state);
            LeaveCriticalSection(&g_sync_cs);
            strbuf b; sb_init(&b);
            sync_status_json(&b, state, 0, 0, 0, "");
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "syncnow") == 0) {
        /* Full HTTP sync against the stored host — network + merge can take
         * seconds, WORKER thread; progress arrives as {"type":"sync",...}
         * events per state change (single-flight). */
        frame->base.add_ref(&frame->base);
        sync_start(frame);
    } else if (strcmp(cmd, "syncexport") == 0) {
        /* Snapshot -> sync\nexgen_library_sync.json (or a given path). */
        char path[1024] = {0};
        (void)json_get_str(json, "path", path, sizeof(path));
        frame->base.add_ref(&frame->base);
        syncfile_start(frame, false, path[0] ? path : NULL);
    } else if (strcmp(cmd, "syncimport") == 0) {
        /* Merge a snapshot file ({"path": optional} -> default sync dir). */
        char path[1024] = {0};
        (void)json_get_str(json, "path", path, sizeof(path));
        frame->base.add_ref(&frame->base);
        syncfile_start(frame, true, path[0] ? path : NULL);
    } else if (strcmp(cmd, "waveform") == 0) {
        waveform_and_reply(frame, json_get_i64(json, "id", 0));
    } else if (strcmp(cmd, "settings") == 0) {
        char action[16] = {0};
        json_get_str(json, "action", action, sizeof(action));
        mn_settings s;
        mn_app_get_settings(g_app, &s);
        if (strcmp(action, "set") == 0) {
            s.exclusive       = json_get_bool(json, "exclusive", s.exclusive);
            s.hifi_native_bits = json_get_bool(json, "hifi_native_bits", s.hifi_native_bits);
            s.ab_rate_cap_hz  = (int32_t)json_get_i64(json, "ab_rate_cap_hz", s.ab_rate_cap_hz);
            s.ab_bits_cap     = (int32_t)json_get_i64(json, "ab_bits_cap", s.ab_bits_cap);
            s.crossfade_ms    = (int32_t)json_get_i64(json, "crossfade_ms", s.crossfade_ms);
            s.replaygain      = json_get_bool(json, "replaygain", s.replaygain);
            s.replaygain_mode = (int32_t)json_get_i64(json, "replaygain_mode", s.replaygain_mode);
            s.rg_preamp_db    = (float)json_get_double(json, "rg_preamp_db", s.rg_preamp_db);
            s.album_art_size  = (int32_t)json_get_i64(json, "album_art_size", s.album_art_size);
            s.stem_cache_gb   = (int32_t)json_get_i64(json, "stem_cache_gb", s.stem_cache_gb);
            s.art_cache_mb    = (int32_t)json_get_i64(json, "art_cache_mb", s.art_cache_mb);
            {
                bool was = s.depth_batch;
                s.depth_batch = json_get_bool(json, "depth_batch", s.depth_batch);
                InterlockedExchange(&g_depth_batch, s.depth_batch ? 1 : 0);
                if (s.depth_batch && !was) depth_selfheal_sweep();
            }
            s.infer_tags = json_get_bool(json, "infer_tags", s.infer_tags);
            s.watch_folders = json_get_bool(json, "watch_folders", s.watch_folders);
            InterlockedExchange(&g_watch_folders, s.watch_folders ? 1 : 0);
            /* low_power persists here; the compositor/ONNX effects apply at
             * next launch (command-line + session options are start-time). */
            s.low_power = json_get_bool(json, "low_power", s.low_power);
            mn_app_set_settings(g_app, &s);
            mn_app_get_settings(g_app, &s);   /* re-read the applied values */
        }
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"settings\",\"exclusive\":");
        sb_puts(&b, s.exclusive ? "true" : "false");
        sb_puts(&b, ",\"hifi_native_bits\":");
        sb_json_bool(&b, s.hifi_native_bits);
        sb_puts(&b, ",\"ab_rate_cap_hz\":");
        sb_json_i64(&b, s.ab_rate_cap_hz);
        sb_puts(&b, ",\"ab_bits_cap\":");
        sb_json_i64(&b, s.ab_bits_cap);
        sb_puts(&b, ",\"crossfade_ms\":");
        sb_json_i64(&b, s.crossfade_ms);
        sb_puts(&b, ",\"replaygain\":");
        sb_puts(&b, s.replaygain ? "true" : "false");
        sb_puts(&b, ",\"replaygain_mode\":");
        sb_json_i64(&b, s.replaygain_mode);
        sb_puts(&b, ",\"rg_preamp_db\":");
        sb_json_float(&b, s.rg_preamp_db);
        sb_puts(&b, ",\"album_art_size\":");
        sb_json_i64(&b, s.album_art_size);
        sb_puts(&b, ",\"stem_cache_gb\":");
        sb_json_i64(&b, s.stem_cache_gb);
        sb_puts(&b, ",\"art_cache_mb\":");
        sb_json_i64(&b, s.art_cache_mb);
        sb_puts(&b, ",\"depth_batch\":");
        sb_json_bool(&b, s.depth_batch);
        sb_puts(&b, ",\"infer_tags\":");
        sb_json_bool(&b, s.infer_tags);
        sb_puts(&b, ",\"watch_folders\":");
        sb_json_bool(&b, s.watch_folders);
        sb_puts(&b, ",\"low_power\":");
        sb_json_bool(&b, s.low_power);
        sb_putc(&b, '}');
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "stems") == 0) {
        char action[32];
        json_get_str(json, "action", action, sizeof(action));
        if (strcmp(action, "enable") == 0) {
            mn_app_stems_enable(g_app, json_get_bool(json, "on", false));
        } else if (strcmp(action, "passthrough") == 0) {
            mn_app_stems_passthrough(g_app, json_get_bool(json, "on", false));
        } else if (strcmp(action, "gain") == 0) {
            int    i = (int)json_get_i64(json, "i", 0);
            double v = json_get_double(json, "v", 1.0);
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            mn_app_stem_gain(g_app, i, (float)v);
        } else if (strcmp(action, "mute") == 0) {
            int i = (int)json_get_i64(json, "i", 0);
            mn_app_stem_mute(g_app, i, json_get_bool(json, "on", false));
        } else if (strcmp(action, "solo") == 0) {
            int i = (int)json_get_i64(json, "i", 0);
            mn_app_stem_solo(g_app, i, json_get_bool(json, "on", false));
        }
    } else if (strcmp(cmd, "tagwrite") == 0) {
        tagwrite_and_reply(frame, json);
    } else if (strcmp(cmd, "artwrite") == 0) {
        artwrite_and_reply(frame, json);
    } else if (strcmp(cmd, "lyricswrite") == 0) {
        lyricswrite_and_reply(frame, json);
    } else if (strcmp(cmd, "downloadmodel") == 0) {
        downloadmodel_and_reply(frame, json);
    } else if (strcmp(cmd, "selectmodel") == 0) {
        selectmodel_and_reply(frame, json);
    } else if (strcmp(cmd, "selectedmodels") == 0) {
        selectedmodels_and_reply(frame, json);
    } else if (strcmp(cmd, "modelfiles") == 0) {
        /* List *.onnx files present in ai-models/ so the UI can mark models
         * installed across restarts (not just this-session downloads). */
        char dir[1024];
        mn_app_models_dir(g_app, dir, sizeof(dir));
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"modelfiles\",\"files\":[");
        if (dir[0]) {
            char pattern[1100];
            WIN32_FIND_DATAA fd;
            HANDLE h;
            int first = 1;
            snprintf(pattern, sizeof(pattern), "%s\\*.onnx", dir);
            h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (!first) sb_putc(&b, ',');
                    first = 0;
                    sb_json_str(&b, fd.cFileName);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        sb_puts(&b, "]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
    } else if (strcmp(cmd, "hwcaps") == 0) {
        /* Neural-hardware probe for the AI Models view (was orphan: the UI
         * sent it + listened but no C handler existed, so the page ran on a
         * WebGL guess). DXGI for GPU name + dedicated VRAM, GlobalMemoryStatus
         * for RAM, GetSystemInfo for cores, cpuid for AVX-512. Provider flags:
         * ORT was built with the CUDA EP (stems select it), and DirectML is
         * available on any DXGI-capable GPU. */
        char     gpu[256] = {0};
        double   vram_gb = 0.0;
        int      cores = 0;
        double   ram_gb = 0.0;
        int      cuda = 0, directml = 0, avx512 = 0;
        {
            IDXGIFactory *fac = NULL;
            if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) && fac) {
                IDXGIAdapter *ad = NULL;
                if (fac->lpVtbl->EnumAdapters(fac, 0, &ad) == S_OK && ad) {
                    DXGI_ADAPTER_DESC d;
                    if (SUCCEEDED(ad->lpVtbl->GetDesc(ad, &d))) {
                        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1,
                                            gpu, (int)sizeof(gpu), NULL, NULL);
                        vram_gb = (double)d.DedicatedVideoMemory / (1024.0*1024.0*1024.0);
                        directml = 1;   /* DXGI adapter => DirectML EP usable */
                        /* NVIDIA vendor id 0x10DE => CUDA EP (ORT gpu build) */
                        if (d.VendorId == 0x10DE) cuda = 1;
                    }
                    ad->lpVtbl->Release(ad);
                }
                fac->lpVtbl->Release(fac);
            }
        }
        {
            MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms))
                ram_gb = (double)ms.ullTotalPhys / (1024.0*1024.0*1024.0);
            SYSTEM_INFO si; GetSystemInfo(&si);
            cores = (int)si.dwNumberOfProcessors;
        }
        {
            int r[4] = {0,0,0,0};
            __cpuidex(r, 7, 0);
            avx512 = (r[1] & (1 << 16)) != 0;   /* EBX bit16 = AVX512F */
        }
        {
            strbuf b; sb_init(&b);
            sb_puts(&b, "{\"type\":\"hwcaps\",\"gpu_name\":");
            sb_json_str(&b, gpu);
            sb_puts(&b, ",\"vram_gb\":");   sb_json_float(&b, (float)vram_gb);
            sb_puts(&b, ",\"cores\":");     sb_json_int(&b, cores);
            sb_puts(&b, ",\"ram_gb\":");    sb_json_float(&b, (float)ram_gb);
            sb_puts(&b, ",\"cuda\":");      sb_json_bool(&b, cuda);
            sb_puts(&b, ",\"tensorrt\":");  sb_json_bool(&b, false);
            sb_puts(&b, ",\"directml\":");  sb_json_bool(&b, directml);
            sb_puts(&b, ",\"npu\":");       sb_json_bool(&b, false);
            sb_puts(&b, ",\"cpu_avx512\":");sb_json_bool(&b, avx512);
            sb_putc(&b, '}');
            if (!b.oom) emit_to_frame(frame, b.data);
            sb_free(&b);
        }
    } else if (strcmp(cmd, "refreshart") == 0) {
        refreshart_and_reply(frame, json);
    } else if (strcmp(cmd, "arthealth") == 0) {
        /* Report the last art-integrity sweep result, and kick a fresh verify+
         * heal pass in the background so a UI check also repairs any gap. */
        char msg[160];
        long miss = InterlockedCompareExchange(&g_artverify_missing, 0, 0);
        long tot  = InterlockedCompareExchange(&g_artverify_total, 0, 0);
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"arthealth\",\"missing\":%ld,\"total\":%ld,\"ok\":%s}",
                 miss, tot, (miss == 0 ? "true" : "false"));
        emit_to_frame(frame, msg);
        art_integrity_kick(0);  /* user-requested: full background verify+heal */
    } else if (strcmp(cmd, "reveal") == 0) {
        char rp[1200] = {0};
        if (json_get_str(json, "path", rp, sizeof(rp))) reveal_in_explorer(rp);
    } else if (strcmp(cmd, "lyricsread") == 0) {
        /* Synchronous: a whole-file metadata parse + sidecar probe is
         * cheap relative to the write paths. */
        int64_t id = json_get_i64(json, "id", 0);
        size_t  cap = 128 * 1024;
        char   *text = (char *)malloc(cap);
        if (text) {
            text[0] = 0;
            (void)mn_app_read_lyrics(g_app, id, text, cap);
        }
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"lyrics\",\"id\":");
        sb_json_i64(&b, id);
        sb_puts(&b, ",\"text\":");
        sb_json_str(&b, text ? text : "");
        sb_putc(&b, '}');
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
        free(text);
    }
    /* unknown commands are ignored */
}

/* ========================================================================= */
/* Reference-counted CEF handler base helper.                                 */
/*                                                                            */
/* All of our handler structs are static singletons that live for the whole   */
/* process, so add_ref / release just track a count (they never self-delete)  */
/* and has_one_ref / has_at_least_one_ref report from it. This satisfies the  */
/* cef_base_ref_counted_t contract without dynamic allocation.                */
/* ========================================================================= */

typedef struct {
    long count;   /* referenced via InterlockedIncrement/Decrement */
} refbase;

/* Locate the refbase for a handler singleton.
 *
 * Every wrapper struct is laid out as  { cef_xxx_t handler; refbase rb; }  so
 * the refbase sits immediately AFTER the CEF struct, i.e. at byte offset
 * sizeof(cef_xxx_t) from the start of the object. INIT_BASE stores exactly that
 * value in base.size, so base.size is the correct offset to the refbase.
 *
 * (The previous version used sizeof(cef_base_ref_counted_t) as the offset,
 * which pointed into the vtable function-pointer area instead of the real rb.
 * add_ref/release then corrupted the first handler function pointer(s), so CEF
 * later jumped through a clobbered pointer and crashed.) */
static refbase *rb_of(cef_base_ref_counted_t *self) {
    return (refbase *)((char *)self + self->size);
}

static void CEF_CALLBACK base_add_ref(cef_base_ref_counted_t *self) {
    InterlockedIncrement(&rb_of(self)->count);
}
static int CEF_CALLBACK base_release(cef_base_ref_counted_t *self) {
    long v = InterlockedDecrement(&rb_of(self)->count);
    return v == 0 ? 1 : 0;   /* never actually free: static singletons */
}
static int CEF_CALLBACK base_has_one_ref(cef_base_ref_counted_t *self) {
    return rb_of(self)->count == 1 ? 1 : 0;
}
static int CEF_CALLBACK base_has_at_least_one_ref(cef_base_ref_counted_t *self) {
    return rb_of(self)->count >= 1 ? 1 : 0;
}

/* Initialize the cef_base_ref_counted_t header + refbase for a singleton. */
#define INIT_BASE(structptr, refbaseptr, structtype)                    \
    do {                                                                \
        (structptr)->base.size = sizeof(structtype);                    \
        (structptr)->base.add_ref = base_add_ref;                       \
        (structptr)->base.release = base_release;                       \
        (structptr)->base.has_one_ref = base_has_one_ref;               \
        (structptr)->base.has_at_least_one_ref = base_has_at_least_one_ref; \
        (refbaseptr)->count = 1;                                        \
    } while (0)

/* Release for HEAP-ALLOCATED one-shot tasks: frees the wrapper when the last
 * reference drops. CEF calls release() AFTER execute() returns, so execute
 * callbacks must NEVER free() the wrapper themselves — doing so is a
 * use-after-free the moment CEF touches the refcount again (this was the
 * add-folder crash). The wrapper struct must start with the CEF struct and
 * be a single malloc/calloc block. */
static int CEF_CALLBACK heap_task_release(cef_base_ref_counted_t *self) {
    long v = InterlockedDecrement(&rb_of(self)->count);
    if (v == 0) { free(self); return 1; }
    return 0;
}

/* INIT_BASE for heap one-shot tasks: identical wiring, self-freeing release. */
#define INIT_HEAP_TASK(structptr, refbaseptr, structtype)              \
    do {                                                               \
        INIT_BASE(structptr, refbaseptr, structtype);                  \
        (structptr)->base.release = heap_task_release;                 \
    } while (0)

/* ========================================================================= */
/* RENDER PROCESS: V8 handler for window.__mn_send(str) + shim injection. */
/* ========================================================================= */

/* V8 handler: window.__mn_send(jsonStr) -> send process message to browser. */
typedef struct {
    cef_v8_handler_t handler;
    refbase          rb;
} ne_v8_handler;

static int CEF_CALLBACK v8h_execute(cef_v8_handler_t *self,
                                    const cef_string_t *name,
                                    cef_v8_value_t *object,
                                    size_t argc,
                                    cef_v8_value_t *const *argv,
                                    cef_v8_value_t **retval,
                                    cef_string_t *exception) {
    (void)self; (void)object; (void)name; (void)exception;
    /* Always give V8 a concrete return value. */
    if (retval) *retval = cef_v8_value_create_bool(1);
    if (argc < 1 || !argv[0]) return 1;

    if (!argv[0]->is_string(argv[0])) return 1;

    /* Read the JS string argument (UTF-16 userfree). */
    cef_string_userfree_t uf = argv[0]->get_string_value(argv[0]);
    if (!uf) return 1;

    /* Package into a process message and send to the browser process. */
    cef_v8_context_t *ctx = cef_v8_context_get_current_context();
    if (ctx) {
        cef_frame_t *frame = ctx->get_frame(ctx);
        if (frame) {
            cef_string_t mname; cefstr_from_ascii(&mname, NE_MSG_CMD);
            cef_process_message_t *msg = cef_process_message_create(&mname);
            cef_string_clear(&mname);
            if (msg) {
                cef_list_value_t *args = msg->get_argument_list(msg);
                if (args) {
                    args->set_size(args, 1);
                    args->set_string(args, 0, uf);   /* uf is a cef_string_t* */
                }
                /* send_process_message CONSUMES the message reference — do NOT
                 * release it afterward (double-release wedges the renderer). */
                frame->send_process_message(frame, PID_BROWSER, msg);
            }
            frame->base.release(&frame->base);
        }
        ctx->base.release(&ctx->base);
    }

    cef_string_userfree_free(uf);
    return 1;
}

static ne_v8_handler g_v8_handler;

/* --- native folder picker implementation (decl above dispatch_command) --- */

typedef struct {
    cef_task_t   task;      /* posted to TID_UI to emit the reply */
    refbase      rb;
    cef_frame_t *frame;     /* owned ref                          */
    char         path[1024];
} ne_pick_reply_task;

static void CEF_CALLBACK pick_reply_execute(cef_task_t *self) {
    ne_pick_reply_task *t = (ne_pick_reply_task *)self;
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"picked\",\"path\":");
    sb_json_str(&b, t->path);
    sb_putc(&b, '}');
    if (!b.oom && t->frame) emit_to_frame(t->frame, b.data);
    sb_free(&b);
    if (t->frame) t->frame->base.release(&t->frame->base);
    /* NO free(t) here: CEF releases the task AFTER execute returns; the
     * wrapper is freed by heap_task_release when the refcount drops. */
}

typedef struct { cef_frame_t *frame; } ne_pick_thread_arg;

static DWORD WINAPI pick_folder_thread(LPVOID param) {
    worker_enter();
    ne_pick_thread_arg *arg = (ne_pick_thread_arg *)param;
    char chosen[1024] = {0};

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IFileDialog *dlg = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileDialog, (void **)&dlg)) && dlg) {
        DWORD opts = 0;
        dlg->lpVtbl->GetOptions(dlg, &opts);
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->lpVtbl->SetTitle(dlg, L"Add a library folder");
        if (SUCCEEDED(dlg->lpVtbl->Show(dlg, g_host_hwnd))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                        chosen, (int)sizeof(chosen), NULL, NULL);
                    CoTaskMemFree(wpath);
                }
                item->lpVtbl->Release(item);
            }
        }
        dlg->lpVtbl->Release(dlg);
    }
    if (SUCCEEDED(hr)) CoUninitialize();

    /* Marshal the reply onto the CEF UI thread. */
    ne_pick_reply_task *t = (ne_pick_reply_task *)calloc(1, sizeof(*t));
    if (t) {
        INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
        t->task.execute = pick_reply_execute;
        t->frame = arg->frame;           /* transfer the owned ref */
        snprintf(t->path, sizeof(t->path), "%s", chosen);
        if (!cef_post_task(TID_UI, &t->task)) {
            /* Never posted: drop the frame ref and the task's own ref. */
            if (t->frame) t->frame->base.release(&t->frame->base);
            t->task.base.release(&t->task.base);
        }
    } else if (arg->frame) {
        arg->frame->base.release(&arg->frame->base);
    }
    free(arg);
    worker_leave();
    return 0;
}

/* ---- Playlist export (.m3u8): IFileSaveDialog on its own thread, gather
 * the playlist's track paths + durations, write UTF-8 M3U8 with #EXTINF. --- */
typedef struct {
    cef_frame_t *frame;    /* owned ref */
    int64_t      id;
    char         name[256];
} ne_plexport_arg;

typedef struct {
    cef_task_t   task;
    refbase      rb;
    cef_frame_t *frame;
    bool         ok;
    char         path[1400];
} ne_plexport_reply;

static void CEF_CALLBACK plexport_reply_execute(cef_task_t *self) {
    ne_plexport_reply *t = (ne_plexport_reply *)self;
    if (t->frame) {
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"plexport\",\"ok\":");
        sb_json_bool(&b, t->ok);
        sb_puts(&b, ",\"path\":");
        sb_json_str(&b, t->path);
        sb_putc(&b, '}');
        if (!b.oom) emit_to_frame(t->frame, b.data);
        sb_free(&b);
        t->frame->base.release(&t->frame->base);
    }
}

static DWORD WINAPI plexport_thread(LPVOID param) {
    worker_enter();
    ne_plexport_arg *arg = (ne_plexport_arg *)param;
    char  chosen[1400] = {0};
    bool  ok = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileDialog *dlg = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileDialog, (void **)&dlg)) && dlg) {
        COMDLG_FILTERSPEC filt[1] = { { L"Playlist (*.m3u8)", L"*.m3u8" } };
        wchar_t wdef[260];
        MultiByteToWideChar(CP_UTF8, 0, arg->name[0] ? arg->name : "playlist",
                            -1, wdef, 260);
        dlg->lpVtbl->SetFileTypes(dlg, 1, filt);
        dlg->lpVtbl->SetDefaultExtension(dlg, L"m3u8");
        dlg->lpVtbl->SetFileName(dlg, wdef);
        dlg->lpVtbl->SetTitle(dlg, L"Export playlist");
        if (SUCCEEDED(dlg->lpVtbl->Show(dlg, g_host_hwnd))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, chosen,
                                        (int)sizeof(chosen), NULL, NULL);
                    CoTaskMemFree(wpath);
                }
                item->lpVtbl->Release(item);
            }
        }
        dlg->lpVtbl->Release(dlg);
    }

    if (chosen[0]) {
        enum { PLX_MAX = 100000 };
        mn_row *rows = (mn_row *)malloc(sizeof(mn_row) * 4096);
        FILE   *f = fopen(chosen, "wb");
        if (rows && f) {
            int n = mn_app_playlist_tracks(g_app, arg->id, 4096, rows);
            int i;
            fputs("\xEF\xBB\xBF#EXTM3U\n", f);   /* UTF-8 BOM + header */
            for (i = 0; i < n; ++i) {
                int secs = (int)(rows[i].duration_ms / 1000);
                fprintf(f, "#EXTINF:%d,%s - %s\n", secs,
                        rows[i].artist[0] ? rows[i].artist : "Unknown",
                        rows[i].title[0]  ? rows[i].title  : "Unknown");
                fprintf(f, "%s\n", rows[i].path);
            }
            ok = true;
        }
        if (f) fclose(f);
        free(rows);
    }
    if (SUCCEEDED(hr)) CoUninitialize();

    ne_plexport_reply *t = (ne_plexport_reply *)calloc(1, sizeof(*t));
    if (t) {
        INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
        t->task.execute = plexport_reply_execute;
        t->frame = arg->frame;
        t->ok = ok;
        snprintf(t->path, sizeof(t->path), "%s", chosen);
        if (!cef_post_task(TID_UI, &t->task)) {
            if (t->frame) t->frame->base.release(&t->frame->base);
            t->task.base.release(&t->task.base);
        }
    } else if (arg->frame) {
        arg->frame->base.release(&arg->frame->base);
    }
    free(arg);
    worker_leave();
    return 0;
}

static void playlist_export_start(cef_frame_t *frame, int64_t id, const char *name) {
    ne_plexport_arg *arg = (ne_plexport_arg *)calloc(1, sizeof(*arg));
    if (!arg) { frame->base.release(&frame->base); return; }
    arg->frame = frame;
    arg->id = id;
    snprintf(arg->name, sizeof(arg->name), "%s", name ? name : "playlist");
    HANDLE h = CreateThread(NULL, 0, plexport_thread, arg, 0, NULL);
    if (h) CloseHandle(h);
    else { frame->base.release(&frame->base); free(arg); }
}

/* ==========================================================================
 * STEM EXPORT — separate each track offline and write its stems as an audio
 * file per stem, optionally bundled into one self-describing .mnstem ZIP.
 *
 * Payload: {cmd:"stemexport",
 *   tracks:[{id,path,artist,album,title,track_no,year,art}],   // UI supplies
 *   set:"individual"|"karaoke"|"acapella"|"drums"|"nodrums"|"bass",
 *   fmt:"wav"|"flac"|"mp3", container:bool, dest:"<dir>"|null }
 * Streams {type:"stemexport", index, total, pct, done, error, file} replies.
 * ========================================================================== */

/* Preset channel-sets (indices into the 9 band-split channels) mirroring the
 * UI's STEM_PRESETS. Each preset sums the listed channels into ONE stem. */
typedef struct { const char *name; const char *label; int ch[9]; int n; } ne_preset;
static const ne_preset NE_PRESETS[] = {
    /* karaoke = everything except vocals */
    { "karaoke",  "Karaoke",     {0,1,3,4,5,6,7,8}, 8 },
    /* acapella = vocals only */
    { "acapella", "A cappella",  {2}, 1 },
    /* drums only = lead + air (the drum bands) */
    { "drums",    "Drums",       {3,6}, 2 },
    /* no drums = everything except the drum bands */
    { "nodrums",  "No Drums",    {0,1,2,4,5,7,8}, 7 },
    /* bass only = sub-bass + bass */
    { "bass",     "Bass",        {0,1}, 2 },
};
static const char *NE_STEM_LABELS[9] = {
    "Sub Bass","Bass","Vocals","Lead","Instruments","Wide","Air","Guitar","Piano"
};

typedef struct {
    int64_t id;
    char path[1024], artist[256], album[256], title[256], art[1024];
    int track_no, year;
} ne_sx_track;

typedef struct {
    cef_frame_t *frame;           /* owned ref (released after the last emit) */
    ne_sx_track *tracks; int ntracks;
    char set[32], fmt[16], dest[1024];
    int  container;
} ne_sx_arg;

/* Extract one object's string/int fields from a JSON tracks[] array. Minimal
 * scanner: finds the i-th top-level object and pulls known keys from it. */
static bool sx_parse_track(const char *arr, int idx, ne_sx_track *out) {
    /* walk to the idx-th '{' at brace-depth 1 inside the array */
    int depth = 0, obj = -1; const char *p = arr, *start = NULL;
    for (; *p; ++p) {
        if (*p == '[') { depth++; continue; }
        if (*p == '{') {
            if (depth == 1) { obj++; if (obj == idx) { start = p; } }
            depth++;
        } else if (*p == '}') {
            depth--;
            if (start && depth == 1) {
                /* isolate this object into a temp buffer and parse it */
                size_t len = (size_t)(p - start) + 1;
                char *tmp = (char *)malloc(len + 1);
                if (!tmp) return false;
                memcpy(tmp, start, len); tmp[len] = 0;
                memset(out, 0, sizeof(*out));
                out->id       = json_get_i64(tmp, "id", 0);
                out->track_no = (int)json_get_i64(tmp, "track_no", 0);
                out->year     = (int)json_get_i64(tmp, "year", 0);
                json_get_str(tmp, "path",   out->path,   sizeof(out->path));
                json_get_str(tmp, "artist", out->artist, sizeof(out->artist));
                json_get_str(tmp, "album",  out->album,  sizeof(out->album));
                json_get_str(tmp, "title",  out->title,  sizeof(out->title));
                json_get_str(tmp, "art",    out->art,    sizeof(out->art));
                free(tmp);
                return out->path[0] != 0;
            }
        } else if (*p == ']' && depth == 1) break;
    }
    return false;
}

/* sanitize a component for a filename (strip path-hostile chars) */
static void sx_safe(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < n; ++p) {
        char c = *p;
        if (c=='\\'||c=='/'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
        out[j++] = c;
    }
    out[j] = 0;
    if (j == 0) snprintf(out, n, "Untitled");
}

/* stream a progress message; add_ref keeps the frame alive across emits. */
static void sx_progress(cef_frame_t *frame, int index, int total, int pct,
                        bool done, const char *err, const char *file) {
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"stemexport\",\"index\":"); sb_json_int(&b, index);
    sb_puts(&b, ",\"total\":"); sb_json_int(&b, total);
    sb_puts(&b, ",\"pct\":");   sb_json_int(&b, pct);
    sb_puts(&b, ",\"done\":");  sb_json_bool(&b, done);
    sb_puts(&b, ",\"error\":"); sb_json_str(&b, err ? err : "");
    sb_puts(&b, ",\"file\":");  sb_json_str(&b, file ? file : "");
    sb_putc(&b, '}');
    if (b.oom) { sb_free(&b); return; }
    if (!done) frame->base.add_ref(&frame->base);   /* keep alive for next emit */
    post_emit_owned(frame, b.data);                 /* hands off b.data + a ref */
}

static DWORD WINAPI stemexport_thread(LPVOID param) {
    worker_enter();
    ne_sx_arg *a = (ne_sx_arg *)param;

    mn_awfmt fmt = MN_AWFMT_WAV;
    if      (strcmp(a->fmt, "flac") == 0) fmt = MN_AWFMT_FLAC;
    else if (strcmp(a->fmt, "mp3")  == 0) fmt = MN_AWFMT_MP3;
    const char *ext = mn_awfmt_ext(fmt);

    /* resolve destination dir: given, else <Music>\Monatomic Stems. */
    char destdir[1024];
    if (a->dest[0]) {
        snprintf(destdir, sizeof(destdir), "%s", a->dest);
    } else {
        char music[512] = {0};
        const char *up = getenv("USERPROFILE");
        snprintf(music, sizeof(music), "%s\\Music", up ? up : ".");
        snprintf(destdir, sizeof(destdir), "%s\\Monatomic Stems", music);
    }
    CreateDirectoryA(destdir, NULL);

    /* build a dedicated stem session (model + cache dir) so we never contend
     * with live playback. */
    char model_sel[260] = {0}, models_dir[700] = {0}, model_path[1024] = {0};
    char stem_cache[1024] = {0};
    mn_app_models_dir(g_app, models_dir, sizeof(models_dir));
    (void)mn_app_get_selected_model(g_app, "stems", model_sel, sizeof(model_sel));
    if (!model_sel[0]) snprintf(model_sel, sizeof(model_sel), "htdemucs_6s.onnx");
    snprintf(model_path, sizeof(model_path), "%s\\%s", models_dir, model_sel);
    mn_app_cache_paths(g_app, NULL, stem_cache, NULL, sizeof(stem_cache));

    mn_stems *sx = mn_stems_create(model_path, stem_cache[0] ? stem_cache : NULL);
    int total = a->ntracks;
    char lasterr[256] = {0};
    int exported_ok = 0;

    for (int ti = 0; ti < total; ti++) {
        ne_sx_track *t = &a->tracks[ti];
        sx_progress(a->frame, ti, total, 0, false, "", "");

        if (!sx) { snprintf(lasterr, sizeof(lasterr), "stem model unavailable"); break; }
        if (!mn_stems_separate_sync(sx, t->id, t->path, NULL, NULL)) {
            snprintf(lasterr, sizeof(lasterr), "separation failed: %s", t->title);
            sx_progress(a->frame, ti, total, 0, false, lasterr, "");
            continue;
        }

        /* per-track working dir for temp encoded stems */
        char aS[256], tS[256];
        sx_safe(t->artist[0] ? t->artist : "Unknown", aS, sizeof(aS));
        sx_safe(t->title[0]  ? t->title  : "Untitled", tS, sizeof(tS));
        char base[900];
        snprintf(base, sizeof(base), "%s\\%s - %s", destdir, aS, tS);

        /* determine which stems/mixes to write */
        mn_stempack_file members[16];
        char memnames[16][300], mempaths[16][1024];
        int  nmembers = 0;

        /* helper lambda-ish: encode a summed channel-set to a temp file */
        #define SX_EMIT(setname, chans, nchans)                                  \
        do {                                                                      \
            uint64_t frames = 0; float *acc = NULL;                               \
            for (int _ci = 0; _ci < (nchans); _ci++) {                            \
                float *c = NULL; uint64_t cf = 0;                                 \
                if (!mn_stems_export_channel(sx, (chans)[_ci], &c, &cf)) continue;\
                if (!acc) { acc = (float *)calloc((size_t)cf*2, sizeof(float));    \
                            frames = cf; }                                        \
                uint64_t lim = cf < frames ? cf : frames;                         \
                for (uint64_t _s = 0; _s < lim*2; _s++) acc[_s] += c[_s];         \
                free(c);                                                          \
            }                                                                     \
            if (acc && frames) {                                                  \
                char nm[300], pth[1024];                                          \
                snprintf(nm, sizeof(nm), "%02d %s.%s", nmembers+1, (setname), ext);\
                if (a->container) snprintf(pth, sizeof(pth), "%s.%d.tmp", base, nmembers);\
                else { CreateDirectoryA(base, NULL);                              \
                       snprintf(pth, sizeof(pth), "%s\\%s", base, nm); }          \
                if (mn_audio_write(pth, fmt, acc, frames, 2, 44100)) {            \
                    snprintf(memnames[nmembers], 300, "%s", nm);                  \
                    snprintf(mempaths[nmembers], 1024, "%s", pth);                \
                    members[nmembers].arcname = memnames[nmembers];               \
                    members[nmembers].srcpath = mempaths[nmembers];               \
                    nmembers++;                                                   \
                }                                                                 \
            }                                                                     \
            free(acc);                                                            \
        } while (0)

        if (strcmp(a->set, "individual") == 0) {
            for (int ci = 0; ci < 9; ci++) {
                int one[1] = { ci };
                SX_EMIT(NE_STEM_LABELS[ci], one, 1);
            }
        } else {
            const ne_preset *pr = NULL;
            for (size_t pi = 0; pi < sizeof(NE_PRESETS)/sizeof(NE_PRESETS[0]); pi++)
                if (strcmp(NE_PRESETS[pi].name, a->set) == 0) { pr = &NE_PRESETS[pi]; break; }
            if (!pr) pr = &NE_PRESETS[0];
            SX_EMIT(pr->label, pr->ch, pr->n);
        }
        #undef SX_EMIT

        char outfile[1200] = {0};
        if (nmembers == 0) {
            snprintf(lasterr, sizeof(lasterr), "no stems written: %s", t->title);
            sx_progress(a->frame, ti, total, 50, false, lasterr, "");
        } else if (a->container) {
            /* manifest.json */
            strbuf mf; sb_init(&mf);
            sb_puts(&mf, "{\"schema\":1,\"app\":\"Monatomic\",\"model\":");
            sb_json_str(&mf, model_sel);
            sb_puts(&mf, ",\"sample_rate\":44100,\"source\":{\"artist\":");
            sb_json_str(&mf, t->artist); sb_puts(&mf, ",\"album\":"); sb_json_str(&mf, t->album);
            sb_puts(&mf, ",\"title\":"); sb_json_str(&mf, t->title);
            sb_puts(&mf, ",\"track_no\":"); sb_json_int(&mf, t->track_no);
            sb_puts(&mf, ",\"year\":"); sb_json_int(&mf, t->year);
            sb_puts(&mf, "},\"stems\":[");
            for (int mi = 0; mi < nmembers; mi++) {
                if (mi) sb_putc(&mf, ',');
                sb_puts(&mf, "{\"index\":"); sb_json_int(&mf, mi);
                sb_puts(&mf, ",\"file\":"); sb_json_str(&mf, memnames[mi]);
                sb_puts(&mf, ",\"format\":"); sb_json_str(&mf, ext); sb_putc(&mf, '}');
            }
            sb_puts(&mf, "]}");
            snprintf(outfile, sizeof(outfile), "%s.mnstem", base);
            /* cover: art field holds a file:// URL; convert to a local path */
            char cover[1024] = {0};
            if (t->art[0]) {
                const char *ap = t->art;
                if (strncmp(ap, "file:///", 8) == 0) ap += 8;
                size_t j = 0; for (; ap[j] && j+1 < sizeof(cover); j++)
                    cover[j] = (ap[j]=='/') ? '\\' : ap[j];
                cover[j] = 0;
            }
            if (!mf.oom)
                mn_stempack_write(outfile, mf.data, members, nmembers,
                                  cover[0] ? cover : NULL);
            sb_free(&mf);
            /* remove temp encoded files */
            for (int mi = 0; mi < nmembers; mi++) DeleteFileA(mempaths[mi]);
            exported_ok++;
        } else {
            snprintf(outfile, sizeof(outfile), "%s", base);   /* the folder */
            exported_ok++;
        }
        sx_progress(a->frame, ti, total,
                    (int)(((ti + 1) * 100) / (total ? total : 1)),
                    false, "", outfile);
    }

    if (sx) mn_stems_destroy(sx);

    /* terminal message (releases the frame ref) */
    {
        char summary[300];
        snprintf(summary, sizeof(summary), "%d/%d exported → %s",
                 exported_ok, total, destdir);
        sx_progress(a->frame, total, total, 100, true,
                    lasterr[0] ? lasterr : "", summary);
    }

    free(a->tracks);
    free(a);
    worker_leave();
    return 0;
}

static void stemexport_start(cef_frame_t *frame, const char *json) {
    ne_sx_arg *a = (ne_sx_arg *)calloc(1, sizeof(*a));
    if (!a) { frame->base.release(&frame->base); return; }
    a->frame = frame;
    json_get_str(json, "set", a->set, sizeof(a->set));
    json_get_str(json, "fmt", a->fmt, sizeof(a->fmt));
    json_get_str(json, "dest", a->dest, sizeof(a->dest));
    a->container = json_get_bool(json, "container", true);
    if (!a->set[0]) snprintf(a->set, sizeof(a->set), "individual");
    if (!a->fmt[0]) snprintf(a->fmt, sizeof(a->fmt), "flac");

    /* parse the tracks[] array. Find its bounds, then extract each object. */
    char *tarr = NULL;
    const char *k = strstr(json, "\"tracks\"");
    if (k) { const char *lb = strchr(k, '['); if (lb) tarr = (char *)lb; }
    if (tarr) {
        /* count objects at depth 1 */
        int depth = 0, cnt = 0;
        for (const char *p = tarr; *p; ++p) {
            if (*p == '[') depth++;
            else if (*p == ']') { if (--depth == 0) break; }
            else if (*p == '{' && depth == 1) cnt++;
        }
        if (cnt > 0) {
            a->tracks = (ne_sx_track *)calloc((size_t)cnt, sizeof(ne_sx_track));
            if (a->tracks) {
                for (int i = 0; i < cnt; i++)
                    if (sx_parse_track(tarr, i, &a->tracks[a->ntracks])) a->ntracks++;
            }
        }
    }
    if (a->ntracks == 0) {
        sx_progress(frame, 0, 0, 100, true, "no tracks", "");
        free(a->tracks); free(a);
        return;
    }
    HANDLE h = CreateThread(NULL, 0, stemexport_thread, a, 0, NULL);
    if (h) CloseHandle(h);
    else { frame->base.release(&frame->base); free(a->tracks); free(a); }
}

static void pick_folder_and_reply(cef_frame_t *frame) {
    ne_pick_thread_arg *arg = (ne_pick_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) return;
    frame->base.add_ref(&frame->base);   /* keep the frame alive for the reply */
    arg->frame = frame;
    HANDLE h = CreateThread(NULL, 0, pick_folder_thread, arg, 0, NULL);
    if (h) CloseHandle(h);
    else { frame->base.release(&frame->base); free(arg); }
}

/* ------------------------------------------------------------------------- */
/* Waveform peaks: {cmd:"waveform",id:<trackid>} -> {type:"waveform",...}.     */
/* Decoding the whole file is slow, so it runs on a worker thread; the reply   */
/* is marshaled back onto the CEF UI thread via cef_post_task, mirroring the   */
/* folder picker above.                                                        */
/* ------------------------------------------------------------------------- */

#define NE_WAVEFORM_BARS 200

typedef struct {
    cef_task_t   task;                    /* posted to TID_UI to emit the reply */
    refbase      rb;
    cef_frame_t *frame;                   /* owned ref                          */
    int64_t      id;                      /* track id echoed back               */
    int          nbars;                   /* number of valid bars (0 == failed) */
    float        bars[NE_WAVEFORM_BARS];  /* normalized peaks in [0,1]          */
} ne_waveform_reply_task;

static void CEF_CALLBACK waveform_reply_execute(cef_task_t *self) {
    ne_waveform_reply_task *t = (ne_waveform_reply_task *)self;
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"waveform\",\"id\":");
    sb_json_i64(&b, t->id);
    sb_puts(&b, ",\"bars\":[");
    for (int i = 0; i < t->nbars; i++) {
        if (i) sb_putc(&b, ',');
        sb_json_float(&b, t->bars[i]);
    }
    sb_puts(&b, "]}");
    if (!b.oom && t->frame) emit_to_frame(t->frame, b.data);
    sb_free(&b);
    if (t->frame) t->frame->base.release(&t->frame->base);
    /* freed by heap_task_release, not here */
}

typedef struct {
    cef_frame_t *frame;   /* owned ref */
    int64_t      id;
    char         path[1024];
} ne_waveform_thread_arg;

static DWORD WINAPI waveform_thread(LPVOID param) {
    worker_enter();
    ne_waveform_thread_arg *arg = (ne_waveform_thread_arg *)param;

    ne_waveform_reply_task *t =
        (ne_waveform_reply_task *)calloc(1, sizeof(*t));
    if (t) {
        INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
        t->task.execute = waveform_reply_execute;
        t->frame = arg->frame;            /* transfer the owned ref */
        t->id    = arg->id;
        t->nbars = mn_engine_waveform(arg->path, NE_WAVEFORM_BARS, t->bars);
        if (!cef_post_task(TID_UI, &t->task)) {
            if (t->frame) t->frame->base.release(&t->frame->base);
            t->task.base.release(&t->task.base);
        }
    } else if (arg->frame) {
        arg->frame->base.release(&arg->frame->base);
    }
    free(arg);
    worker_leave();
    return 0;
}

static void waveform_and_reply(cef_frame_t *frame, int64_t id) {
    ne_waveform_thread_arg *arg =
        (ne_waveform_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) return;

    /* Resolve the current track's path on the UI thread (cheap). If nothing is
     * playing, reply immediately with an empty bar list so the UI can clear. */
    if (!mn_app_current_path(g_app, arg->path, sizeof(arg->path))) {
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"waveform\",\"id\":");
        sb_json_i64(&b, id);
        sb_puts(&b, ",\"bars\":[]}");
        if (!b.oom) emit_to_frame(frame, b.data);
        sb_free(&b);
        free(arg);
        return;
    }

    frame->base.add_ref(&frame->base);    /* keep the frame alive for the reply */
    arg->frame = frame;
    arg->id    = id;
    HANDLE h = CreateThread(NULL, 0, waveform_thread, arg, 0, NULL);
    if (h) CloseHandle(h);
    else { frame->base.release(&frame->base); free(arg); }
}

/* ------------------------------------------------------------------------- */
/* Metadata writing: tagwrite / artwrite / lyricswrite.                        */
/* File rewrites can take a moment (whole-file copy + atomic replace, and for  */
/* whole-album art many files), so — mirroring the waveform/pickfolder pattern */
/* — the work runs on a worker thread and the JSON reply is marshaled back     */
/* onto the CEF UI thread via cef_post_task.                                   */
/* ------------------------------------------------------------------------- */

enum { NE_TAGW_TAGS = 0, NE_TAGW_ART = 1, NE_TAGW_LYRICS = 2 };

typedef struct {
    cef_task_t   task;                 /* posted to TID_UI to emit the reply */
    refbase      rb;
    cef_frame_t *frame;                /* owned ref                          */
    int          kind;                 /* NE_TAGW_*                          */
    int64_t      id;
    int          ok;
    char         err[MN_TAGW_ERR_CAP];
} ne_tagw_reply_task;

static void CEF_CALLBACK tagw_reply_execute(cef_task_t *self) {
    ne_tagw_reply_task *t = (ne_tagw_reply_task *)self;
    const char *type = (t->kind == NE_TAGW_TAGS) ? "tagwrote"
                     : (t->kind == NE_TAGW_ART)  ? "artwrote"
                                                 : "lyricswrote";
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":");
    sb_json_str(&b, type);
    sb_puts(&b, ",\"id\":");
    sb_json_i64(&b, t->id);
    sb_puts(&b, ",\"ok\":");
    sb_json_bool(&b, t->ok != 0);
    if (!t->ok && t->err[0] && t->kind != NE_TAGW_LYRICS) {
        sb_puts(&b, ",\"error\":");
        sb_json_str(&b, t->err);
    }
    sb_putc(&b, '}');
    if (!b.oom && t->frame) emit_to_frame(t->frame, b.data);
    sb_free(&b);
    if (t->frame) t->frame->base.release(&t->frame->base);
    /* freed by heap_task_release, not here */
}

typedef struct {
    cef_frame_t *frame;    /* owned ref */
    int          kind;     /* NE_TAGW_* */
    int64_t      id;
    mn_tag_edit  edit;     /* NE_TAGW_TAGS   */
    uint8_t     *img;      /* NE_TAGW_ART (owned) */
    size_t       img_len;
    char         mime[32];
    bool         whole_album;
    char        *text;     /* NE_TAGW_LYRICS (owned, may be NULL) */
    char        *lrc;      /* NE_TAGW_LYRICS (owned, may be NULL) */
} ne_tagw_thread_arg;

/* Invalidate ONE album's DERIVED art tiers (hires + depth) after an art
 * write. The base thumb itself is already regenerated by the write path
 * (mn_app_refresh_art_cache); its mtime bump changes the served ?g=
 * generation, so CEF re-fetches the fresh pixels at the same path. Also
 * clears any NONE verdict / session-dead entry (art just became resolvable)
 * and posts artready so visible tiles repaint without user interaction. */
static void tagw_clear_webart_one(const char *artist, const char *album) {
    char png[MN_ART_PATH_MAX];
    if (!album || !album[0]) return;
    art_none_remove(artist, album);
    art_clear_dead_all();   /* cheap; the ring is a per-session belt anyway */
    if (mn_app_art_check(g_app, artist, album, png, sizeof(png))) {
        size_t pn = strlen(png);
        if (pn > 4) {
            char depth[MN_ART_PATH_MAX + 32], hires[MN_ART_PATH_MAX + 32];
            snprintf(depth, sizeof(depth), "%.*s.depth.png", (int)(pn - 4), png);
            snprintf(hires, sizeof(hires), "%.*s.hires.png", (int)(pn - 4), png);
            DeleteFileA(depth);
            DeleteFileA(hires);
        }
        {
            char url[1300];
            if (art_thumb_url(png, url, sizeof(url)))
                artready_queue(artist, album, url);
        }
    } else {
        /* thumb not rebuilt yet (or art removed): let the pool extract and
         * artready the landing */
        artenc_enqueue(artist, album);
    }
}

/* Bulk-edit fallback (album unknown / library-wide op): drop every DERIVED
 * art tier so hires/depth regenerate from the refreshed thumbs. Base thumbs
 * are NEVER touched here — they are the coverage invariant; stale ones are
 * rebuilt per-track by the write path and served with a fresh ?g=. */
static void tagw_clear_webart(void) {
    if (!g_art_dir[0]) return;
    mn_dir_delete_matching(g_art_dir, "*.hires.png");
    mn_dir_delete_matching(g_art_dir, "*.depth.png");
    art_none_clear_all();   /* keys may have changed wholesale */
    art_clear_dead_all();
}

static DWORD WINAPI tagw_thread(LPVOID param) {
    worker_enter();
    ne_tagw_thread_arg *arg = (ne_tagw_thread_arg *)param;

    ne_tagw_reply_task *t = (ne_tagw_reply_task *)calloc(1, sizeof(*t));
    if (t) {
        INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
        t->task.execute = tagw_reply_execute;
        t->frame = arg->frame;            /* transfer the owned ref */
        t->kind  = arg->kind;
        t->id    = arg->id;

        switch (arg->kind) {
            case NE_TAGW_TAGS:
                t->ok = mn_app_write_tags(g_app, arg->id, &arg->edit,
                                          t->err, sizeof(t->err));
                break;
            case NE_TAGW_ART:
                t->ok = mn_app_write_art(g_app, arg->id, arg->img,
                                         arg->img_len, arg->mime,
                                         arg->whole_album,
                                         t->err, sizeof(t->err));
                if (t->ok) {
                    /* Invalidate ONLY the edited album's webart, not the whole
                     * library (whole_album writes one album's tracks, so the
                     * art key is the same for all of them). Fall back to the
                     * library-wide clear only if the album can't be resolved. */
                    char aArtist[256], aAlbum[256];
                    if (mn_app_track_art_key(g_app, arg->id, aArtist, sizeof(aArtist),
                                             aAlbum, sizeof(aAlbum)))
                        tagw_clear_webart_one(aArtist, aAlbum);
                    else
                        tagw_clear_webart();
                }
                break;
            case NE_TAGW_LYRICS:
            default:
                t->ok = mn_app_write_lyrics(g_app, arg->id,
                                            arg->text ? arg->text : "",
                                            arg->lrc ? arg->lrc : "");
                break;
        }
        if (!cef_post_task(TID_UI, &t->task)) {
            if (t->frame) t->frame->base.release(&t->frame->base);
            t->task.base.release(&t->task.base);
        }
    } else if (arg->frame) {
        arg->frame->base.release(&arg->frame->base);
    }
    mn_tagw_b64_free(arg->img);
    free(arg->text);
    free(arg->lrc);
    free(arg);
    worker_leave();
    return 0;
}

/* Kick a prepared thread arg onto the worker (shared by the 3 commands). */
static void tagw_start(cef_frame_t *frame, ne_tagw_thread_arg *arg) {
    frame->base.add_ref(&frame->base);    /* keep the frame alive for reply */
    arg->frame = frame;
    HANDLE h = CreateThread(NULL, 0, tagw_thread, arg, 0, NULL);
    if (h) CloseHandle(h);
    else {
        frame->base.release(&frame->base);
        mn_tagw_b64_free(arg->img);
        free(arg->text);
        free(arg->lrc);
        free(arg);
    }
}

/* Immediate {ok:false} reply for requests that fail before the worker. */
static void tagw_reply_fail(cef_frame_t *frame, const char *type, int64_t id,
                            const char *error) {
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":");
    sb_json_str(&b, type);
    sb_puts(&b, ",\"id\":");
    sb_json_i64(&b, id);
    sb_puts(&b, ",\"ok\":false");
    if (error && error[0]) {
        sb_puts(&b, ",\"error\":");
        sb_json_str(&b, error);
    }
    sb_putc(&b, '}');
    if (!b.oom) emit_to_frame(frame, b.data);
    sb_free(&b);
}

/* {"cmd":"tagwrite", id, fields:{title,artist,album,album_artist,genre,
 *  year,track_no,comment}} -> {"type":"tagwrote", id, ok[, error]}. */
static void tagwrite_and_reply(cef_frame_t *frame, const char *json) {
    int64_t id = json_get_i64(json, "id", 0);
    ne_tagw_thread_arg *arg =
        (ne_tagw_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) { tagw_reply_fail(frame, "tagwrote", id, "io-error"); return; }

    arg->kind = NE_TAGW_TAGS;
    arg->id   = id;
    /* The `fields` object's keys are unique in the message, so the flat
     * extractor finds them nested. Comment may be long: alloc variant. */
    json_get_str(json, "title",        arg->edit.title,        sizeof(arg->edit.title));
    json_get_str(json, "artist",       arg->edit.artist,       sizeof(arg->edit.artist));
    json_get_str(json, "album",        arg->edit.album,        sizeof(arg->edit.album));
    json_get_str(json, "album_artist", arg->edit.album_artist, sizeof(arg->edit.album_artist));
    json_get_str(json, "genre",        arg->edit.genre,        sizeof(arg->edit.genre));
    json_get_str(json, "comment",      arg->edit.comment,      sizeof(arg->edit.comment));
    arg->edit.year     = (int)json_get_i64(json, "year", 0);
    arg->edit.track_no = (int)json_get_i64(json, "track_no", 0);
    if (arg->edit.year < 0)     arg->edit.year = 0;
    if (arg->edit.year > 9999)  arg->edit.year = 9999;
    if (arg->edit.track_no < 0) arg->edit.track_no = 0;
    /* PARTIAL edit (album batch): empty fields PRESERVE the file's values
     * instead of removing the frames — without this the batch editor wiped
     * Title/Track#/Comment from every file in the album. */
    arg->edit.keep_missing = json_get_bool(json, "keep_missing", false);

    tagw_start(frame, arg);
}

/* {"cmd":"artwrite", id, image_b64, mime, whole_album}
 *  -> {"type":"artwrote", id, ok[, error]}. */
static void artwrite_and_reply(cef_frame_t *frame, const char *json) {
    int64_t id = json_get_i64(json, "id", 0);
    char *b64 = json_get_str_alloc(json, "image_b64");
    uint8_t *img = NULL;
    size_t img_len = 0;

    if (b64) {
        img = mn_tagw_b64_decode(b64, &img_len);
        free(b64);
    }
    if (!img || img_len == 0) {
        mn_tagw_b64_free(img);
        tagw_reply_fail(frame, "artwrote", id, "bad-image");
        return;
    }

    ne_tagw_thread_arg *arg =
        (ne_tagw_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) {
        mn_tagw_b64_free(img);
        tagw_reply_fail(frame, "artwrote", id, "io-error");
        return;
    }
    arg->kind    = NE_TAGW_ART;
    arg->id      = id;
    arg->img     = img;
    arg->img_len = img_len;
    json_get_str(json, "mime", arg->mime, sizeof(arg->mime));
    if (!arg->mime[0]) snprintf(arg->mime, sizeof(arg->mime), "image/jpeg");
    arg->whole_album = json_get_bool(json, "whole_album", false);

    tagw_start(frame, arg);
}

/* {"cmd":"lyricswrite", id, text, synced_lrc}
 *  -> {"type":"lyricswrote", id, ok}. */
static void lyricswrite_and_reply(cef_frame_t *frame, const char *json) {
    int64_t id = json_get_i64(json, "id", 0);
    ne_tagw_thread_arg *arg =
        (ne_tagw_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) { tagw_reply_fail(frame, "lyricswrote", id, NULL); return; }

    arg->kind = NE_TAGW_LYRICS;
    arg->id   = id;
    arg->text = json_get_str_alloc(json, "text");
    arg->lrc  = json_get_str_alloc(json, "synced_lrc");

    tagw_start(frame, arg);
}

/* ------------------------------------------------------------------------- */
/* Library reset: {"cmd":"resetlibrary","art":bool} -> {"type":"resetdone"}.  */
/* The wipe joins the scanner and rewrites the db — potentially seconds — so  */
/* it runs on a worker thread (same pattern as the tag writers) and the tiny  */
/* reply is marshaled back to TID_UI.                                         */
/* ------------------------------------------------------------------------- */

/* Small generic "emit JSON to a frame" UI task. Short payloads live in the
 * inline buffer; larger ones ride an owned heap string (json_big). */
typedef struct {
    cef_task_t   task;
    refbase      rb;
    cef_frame_t *frame;    /* owned ref */
    char        *json_big; /* owned heap payload, or NULL -> use json[] */
    char         json[128];
} ne_emit_task;

static void CEF_CALLBACK emit_task_execute(cef_task_t *self) {
    ne_emit_task *t = (ne_emit_task *)self;
    if (t->frame) {
        emit_to_frame(t->frame, t->json_big ? t->json_big : t->json);
        t->frame->base.release(&t->frame->base);
    }
    free(t->json_big);
    t->json_big = NULL;
    /* task struct freed by heap_task_release, not here */
}

/* Post `json` to the UI thread for `frame` (consumes the frame ref). */
static void post_emit(cef_frame_t *frame_owned, const char *json) {
    ne_emit_task *t = (ne_emit_task *)calloc(1, sizeof(*t));
    if (!t) {
        if (frame_owned) frame_owned->base.release(&frame_owned->base);
        return;
    }
    INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
    t->task.execute = emit_task_execute;
    t->frame = frame_owned;
    snprintf(t->json, sizeof(t->json), "%s", json);
    if (!cef_post_task(TID_UI, &t->task)) {
        if (t->frame) t->frame->base.release(&t->frame->base);
        t->task.base.release(&t->task.base);
    }
}

/* Like post_emit but takes OWNERSHIP of a malloc'd JSON string (any size).
 * Consumes both the frame ref and the string. */
static void post_emit_owned(cef_frame_t *frame_owned, char *json_heap) {
    ne_emit_task *t;
    if (!json_heap) {
        if (frame_owned) frame_owned->base.release(&frame_owned->base);
        return;
    }
    t = (ne_emit_task *)calloc(1, sizeof(*t));
    if (!t) {
        if (frame_owned) frame_owned->base.release(&frame_owned->base);
        free(json_heap);
        return;
    }
    INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
    t->task.execute = emit_task_execute;
    t->frame = frame_owned;
    t->json_big = json_heap;
    if (!cef_post_task(TID_UI, &t->task)) {
        if (t->frame) t->frame->base.release(&t->frame->base);
        free(t->json_big); t->json_big = NULL;
        t->task.base.release(&t->task.base);
    }
}

/* ------------------------------------------------------------------------- */
/* Cache-ops worker: cacheinfo (dir-stat walks) and clearcache (mass deletes) */
/* both touch thousands of files — they ran synchronously in dispatch and    */
/* stalled every queued bridge command. One worker handles both: `which` set  */
/* -> clear that cache, emit cachecleared, then always emit fresh cacheinfo.  */
/* ------------------------------------------------------------------------- */
typedef struct {
    cef_frame_t *frame;      /* owned ref */
    bool         do_clear;
    char         which[24];
} ne_cacheop_ctx;

/* Directory that holds the CEF cache slots. Portable (zip) builds keep it
 * beside the exe as always; INSTALLED builds live in Program Files where the
 * exe dir is not writable, so the cache moves to %LOCALAPPDATA%\Monatomic.
 * Decided once by write-probing the exe dir (DELETE_ON_CLOSE leaves nothing
 * behind). */
static void mn_cef_cache_base(char *out, size_t cap) {
    static int  decided = 0;
    static char base[1400];
    if (!decided) {
        char probe[1400];
        HANDLE hf;
        snprintf(probe, sizeof(probe), "%s\\.mn_writeprobe", g_exe_dir);
        hf = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            CloseHandle(hf);
            snprintf(base, sizeof(base), "%s", g_exe_dir);
        } else {
            const char *lad = getenv("LOCALAPPDATA");
            if (lad && lad[0]) {
                snprintf(base, sizeof(base), "%s\\Monatomic", lad);
                CreateDirectoryA(base, NULL);
            } else {
                snprintf(base, sizeof(base), "%s", g_exe_dir);
            }
        }
        decided = 1;
    }
    snprintf(out, cap, "%s", base);
}

/* Build the {"type":"cacheinfo",...} reply into `b` (walks 5 cache dirs). */
static void cacheinfo_build(strbuf *b) {
    struct { const char *name; char path[1400]; } caches[5];
    caches[0].path[0] = caches[1].path[0] = caches[2].path[0] = 0;
    mn_app_cache_paths(g_app, caches[0].path, caches[1].path,
                       caches[2].path, 1400);
    caches[0].name = "Album art (thumbs + hi-res)";
    caches[1].name = "Neural stems";
    caches[2].name = "AI models";
    caches[3].name = "Web art (legacy — being reclaimed)";
    snprintf(caches[3].path, sizeof(caches[3].path), "%s", g_webart);
    caches[4].name = "Browser (CEF)";
    {
        char cbase[1400];
        mn_cef_cache_base(cbase, sizeof(cbase));
        snprintf(caches[4].path, sizeof(caches[4].path), "%s\\cef_cache", cbase);
    }
    sb_puts(b, "{\"type\":\"cacheinfo\",\"caches\":[");
    for (int ci = 0; ci < 5; ci++) {
        int64_t bytes = 0, files = 0;
        if (caches[ci].path[0]) mn_dir_stats(caches[ci].path, 0, &bytes, &files);
        if (ci) sb_putc(b, ',');
        sb_putc(b, '{');
        sb_puts(b, "\"name\":");  sb_json_str(b, caches[ci].name);  sb_putc(b, ',');
        sb_puts(b, "\"path\":");  sb_json_str(b, caches[ci].path);  sb_putc(b, ',');
        sb_puts(b, "\"bytes\":"); sb_json_i64(b, bytes);            sb_putc(b, ',');
        sb_puts(b, "\"files\":"); sb_json_i64(b, files);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

static DWORD WINAPI cacheop_thread(LPVOID param) {
    worker_enter();
    ne_cacheop_ctx *ctx = (ne_cacheop_ctx *)param;

    if (ctx->do_clear) {
        const char *which = ctx->which;
        if (strcmp(which, "stems") == 0) {
            char dir[1400] = {0};
            mn_app_cache_paths(g_app, NULL, dir, NULL, sizeof(dir));
            if (dir[0]) mn_dir_delete_matching(dir, "*.mnstems");
        } else if (strcmp(which, "art") == 0) {
            char dir[1400] = {0};
            mn_app_cache_paths(g_app, dir, NULL, NULL, sizeof(dir));
            /* full art reset: thumbs + derived tiers; verdicts no longer
             * apply (the selfheal re-extracts everything from tags) */
            if (dir[0]) mn_dir_delete_matching(dir, "*.png");
            art_none_clear_all();
            art_clear_dead_all();
            artscan_selfheal_start();
        } else if (strcmp(which, "webart") == 0) {
            /* one-store era: "webart" now clears the DERIVED tiers only
             * (hires + depth regenerate lazily); base thumbs are the
             * coverage invariant and are never mass-deleted from here */
            if (g_art_dir[0]) {
                mn_dir_delete_matching(g_art_dir, "*.hires.png");
                mn_dir_delete_matching(g_art_dir, "*.depth.png");
            }
        } else if (strcmp(which, "depth") == 0) {
            depth_clear_maps();
            depth_selfheal_sweep();
        }
        {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "{\"type\":\"cachecleared\",\"which\":\"%s\"}", which);
            /* two posts total: add a ref for this one, the ctx ref feeds
             * the cacheinfo post below */
            ctx->frame->base.add_ref(&ctx->frame->base);
            post_emit(ctx->frame, msg);
        }
    }

    {
        strbuf b; sb_init(&b);
        cacheinfo_build(&b);
        if (!b.oom) {
            post_emit_owned(ctx->frame, b.data);   /* hands off b.data */
        } else {
            sb_free(&b);
            ctx->frame->base.release(&ctx->frame->base);
        }
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void cacheop_start(cef_frame_t *frame_owned, const char *which_or_null) {
    ne_cacheop_ctx *ctx = (ne_cacheop_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    if (which_or_null) {
        /* `which` is echoed raw inside a JSON literal — keep [a-z0-9_] only */
        size_t j = 0;
        ctx->do_clear = true;
        for (const char *p = which_or_null;
             *p && j + 1 < sizeof(ctx->which); ++p) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')
                ctx->which[j++] = *p;
        }
        ctx->which[j] = 0;
    }
    h = CreateThread(NULL, 0, cacheop_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* ------------------------------------------------------------------------- */
/* Online cover-art fetch worker (iTunes -> Deezer -> download -> ingest).    */
/* Was synchronous in dispatch: 1-30 s of WinHTTP froze transport + polls.    */
/* ------------------------------------------------------------------------- */
typedef struct {
    cef_frame_t *frame;      /* owned ref */
    char         artist[256];
    char         album[256];
    int          res;
} ne_artfetch_ctx;

static DWORD WINAPI artfetch_thread(LPVOID param) {
    worker_enter();
    ne_artfetch_ctx *ctx = (ne_artfetch_ctx *)param;
    const char *artist = ctx->artist, *album = ctx->album;
    int   res = ctx->res;
    char  imgurl[1600] = {0};
    bool  ok = false;
    const char *msg = "no art found";

    if (res < 300)  res = 300;
    if (res > 3000) res = 3000;
    if (album[0]) {
        char q[600], qa[256], qb[256];
        char *body = NULL;
        wchar_t wpath[1200];
        mn_url_encode(artist, qa, sizeof(qa));
        mn_url_encode(album,  qb, sizeof(qb));
        /* --- iTunes --- */
        snprintf(q, sizeof(q),
                 "/search?term=%s+%s&entity=album&limit=3", qa, qb);
        if (MultiByteToWideChar(CP_UTF8, 0, q, -1, wpath, 1200) > 0 &&
            mn_https_get(L"itunes.apple.com", wpath, &body, 512 * 1024) > 0) {
            char u[1600];
            if (mn_json_find_str(body, "artworkUrl100", u, sizeof(u))) {
                /* .../100x100bb.jpg -> .../<res>x<res>bb.jpg */
                char *sz = strstr(u, "100x100bb");
                if (sz) {
                    char tail[64];
                    snprintf(tail, sizeof(tail), "%dx%dbb%s", res, res, sz + 9);
                    *sz = '\0';
                    snprintf(imgurl, sizeof(imgurl), "%s%s", u, tail);
                } else {
                    snprintf(imgurl, sizeof(imgurl), "%s", u);
                }
            }
        }
        free(body); body = NULL;
        /* --- Deezer fallback --- */
        if (!imgurl[0]) {
            snprintf(q, sizeof(q), "/search/album?q=%s+%s&limit=3", qa, qb);
            if (MultiByteToWideChar(CP_UTF8, 0, q, -1, wpath, 1200) > 0 &&
                mn_https_get(L"api.deezer.com", wpath, &body, 512 * 1024) > 0) {
                (void)mn_json_find_str(body, "cover_xl", imgurl, sizeof(imgurl));
            }
            free(body); body = NULL;
        }
        if (imgurl[0]) {
            char tmpf[1024];
            char tmpdir[900];
            GetTempPathA(sizeof(tmpdir), tmpdir);
            /* per-thread temp name: two concurrent fetches must not collide */
            snprintf(tmpf, sizeof(tmpf), "%smn_artfetch_%lu.img",
                     tmpdir, (unsigned long)GetCurrentThreadId());
            if (mn_https_download(imgurl, tmpf) > 0) {
                if (mn_app_ingest_album_art(g_app, artist, album, tmpf)) {
                    /* Ingest rewrote the art-cache thumb + hires in place.
                     * Clear the NONE verdict (art just became resolvable),
                     * drop the stale depth map, and post artready with the
                     * fresh ?g= so every visible tile swaps immediately. */
                    char png[MN_ART_PATH_MAX];
                    art_none_remove(artist, album);
                    art_clear_dead_all();
                    if (mn_app_art_check(g_app, artist, album,
                                         png, sizeof(png))) {
                        size_t pn = strlen(png);
                        if (pn > 4) {
                            char depth[MN_ART_PATH_MAX + 32];
                            snprintf(depth, sizeof(depth), "%.*s.depth.png",
                                     (int)(pn - 4), png);
                            DeleteFileA(depth);
                        }
                        {
                            char url[1300];
                            if (art_thumb_url(png, url, sizeof(url)))
                                artready_queue(artist, album, url);
                        }
                    }
                    ok = true;
                    msg = "cover replaced";
                } else {
                    msg = "could not decode downloaded image";
                }
                DeleteFileA(tmpf);
            } else {
                msg = "download failed";
            }
        }
    }
    {
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"artfetch\",\"ok\":");
        sb_json_bool(&b, ok);
        sb_puts(&b, ",\"msg\":");
        sb_json_str(&b, msg);
        sb_puts(&b, ",\"album\":");
        sb_json_str(&b, album);
        sb_putc(&b, '}');
        if (!b.oom) {
            post_emit_owned(ctx->frame, b.data);
        } else {
            sb_free(&b);
            ctx->frame->base.release(&ctx->frame->base);
        }
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void artfetch_start(cef_frame_t *frame_owned, const char *artist,
                           const char *album, int res) {
    ne_artfetch_ctx *ctx = (ne_artfetch_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    snprintf(ctx->artist, sizeof(ctx->artist), "%s", artist ? artist : "");
    snprintf(ctx->album,  sizeof(ctx->album),  "%s", album  ? album  : "");
    ctx->res = res;
    h = CreateThread(NULL, 0, artfetch_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* ------------------------------------------------------------------------- */
/* Library-sync workers: syncnow (full HTTP flow against the phone),          */
/* syncexport/syncimport (snapshot files), purgemissing, backupnow. All ride  */
/* the cacheop/artfetch pattern: dispatch never blocks, replies are marshaled */
/* to TID_UI via post_emit/post_emit_owned (each consumes one frame ref).     */
/* ------------------------------------------------------------------------- */

/* Emit one sync status event (consumes the frame ref; NULL frame = no-op,
 * the auto tick runs headless when no browser frame is available). Also
 * records `state` so a later {"cmd":"syncstatus"} reflects it. */
static void sync_emit_status(cef_frame_t *frame_owned, const char *state,
                             int applied, int skipped, int pushed,
                             const char *error) {
    strbuf b;
    EnterCriticalSection(&g_sync_cs);
    snprintf(g_sync_state, sizeof(g_sync_state), "%s", state);
    LeaveCriticalSection(&g_sync_cs);
    if (!frame_owned) return;
    sb_init(&b);
    sync_status_json(&b, state, applied, skipped, pushed, error);
    if (!b.oom) {
        post_emit_owned(frame_owned, b.data);   /* hands off b.data */
    } else {
        sb_free(&b);
        frame_owned->base.release(&frame_owned->base);
    }
}

typedef struct {
    cef_frame_t *frame;      /* owned ref; may be NULL (headless auto sync) */
    char         host[128];
    int          port;
} ne_sync_ctx;

/* Progress relay from mn_app_sync_run (worker thread): every state change
 * emits the status contract; "done" stamps + persists last_ms first so the
 * event already carries it. */
static void sync_progress_cb(void *user, const char *state,
                             int applied, int skipped, int pushed,
                             const char *error) {
    ne_sync_ctx *ctx = (ne_sync_ctx *)user;
    if (strcmp(state, "done") == 0) {
        InterlockedExchange64(&g_sync_last_ms, sync_now_ms());
        sync_state_save_last();
    }
    if (ctx->frame) {
        /* sync_emit_status consumes a ref; keep the baseline one alive. */
        ctx->frame->base.add_ref(&ctx->frame->base);
        sync_emit_status(ctx->frame, state, applied, skipped, pushed, error);
    } else {
        sync_emit_status(NULL, state, applied, skipped, pushed, error);
    }
}

static DWORD WINAPI sync_thread(LPVOID param) {
    worker_enter();
    ne_sync_ctx *ctx = (ne_sync_ctx *)param;
    (void)mn_app_sync_run(g_app, ctx->host, ctx->port, sync_progress_cb, ctx);
    if (ctx->frame) ctx->frame->base.release(&ctx->frame->base);
    InterlockedExchange(&g_sync_busy, 0);
    free(ctx);
    worker_leave();
    return 0;
}

/* Launch the full sync flow against the stored host (single-flight). Takes
 * ownership of frame_owned (may be NULL for the headless auto tick). */
static void sync_start(cef_frame_t *frame_owned) {
    ne_sync_ctx *ctx;
    HANDLE h;
    char   host[128];
    int    port;

    EnterCriticalSection(&g_sync_cs);
    snprintf(host, sizeof(host), "%s", g_sync_host);
    port = g_sync_port;
    LeaveCriticalSection(&g_sync_cs);
    if (!host[0]) {
        sync_emit_status(frame_owned, "error", 0, 0, 0,
                         "no sync host configured");
        return;
    }
    if (InterlockedCompareExchange(&g_sync_busy, 1, 0) != 0) {
        sync_emit_status(frame_owned, "error", 0, 0, 0,
                         "a sync is already running");
        return;
    }
    ctx = (ne_sync_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        InterlockedExchange(&g_sync_busy, 0);
        if (frame_owned) frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    snprintf(ctx->host, sizeof(ctx->host), "%s", host);
    ctx->port = port;
    h = CreateThread(NULL, 0, sync_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else {
        InterlockedExchange(&g_sync_busy, 0);
        if (ctx->frame) ctx->frame->base.release(&ctx->frame->base);
        free(ctx);
    }
}

/* Grab an owned ref to the main frame (or NULL) — lets the timer-driven
 * auto sync emit progress without a bridge command in flight. */
static cef_frame_t *sync_grab_frame(void) {
    cef_frame_t   *frame = NULL;
    cef_browser_t *br = NULL;
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) {
        br = g_browser;
        br->base.add_ref(&br->base);
    }
    LeaveCriticalSection(&g_browser_lock);
    if (br) {
        frame = br->get_main_frame(br);   /* owned ref (or NULL) */
        br->base.release(&br->base);
    }
    return frame;
}

/* 5-minute maintenance hook: when auto sync is on, a host is stored and the
 * last success is stale (> 10 min, or never), run the flow. Single-flight
 * via g_sync_busy inside sync_start. Called from heal_tick_thread. */
static void sync_auto_tick(void) {
    char    host[128];
    int64_t last;

    if (!g_app) return;
    if (!InterlockedCompareExchange(&g_sync_auto, 0, 0)) return;
    EnterCriticalSection(&g_sync_cs);
    snprintf(host, sizeof(host), "%s", g_sync_host);
    LeaveCriticalSection(&g_sync_cs);
    if (!host[0]) return;
    last = (int64_t)InterlockedCompareExchange64(&g_sync_last_ms, 0, 0);
    {
        /* configurable interval (Settings -> Sync), floor 5 min */
        int64_t gate_ms =
            (int64_t)InterlockedCompareExchange(&g_sync_interval_min, 0, 0)
            * 60 * 1000;
        if (gate_ms < 5 * 60 * 1000) gate_ms = NE_SYNC_AUTO_MIN_MS;
        if (last > 0 && sync_now_ms() - last < gate_ms) return;
    }
    if (InterlockedCompareExchange(&g_sync_busy, 0, 0) != 0) return;
    sync_start(sync_grab_frame());
}

/* --- snapshot file export / import worker ------------------------------- */

typedef struct {
    cef_frame_t *frame;      /* owned ref */
    bool         import;
    char         path[1024]; /* "" = default sync-dir snapshot path */
} ne_syncfile_ctx;

static DWORD WINAPI syncfile_thread(LPVOID param) {
    worker_enter();
    ne_syncfile_ctx *ctx = (ne_syncfile_ctx *)param;
    strbuf b; sb_init(&b);

    if (ctx->import) {
        int  a = 0, s = 0;
        bool ok = mn_app_sync_import(g_app, ctx->path[0] ? ctx->path : NULL,
                                     &a, &s);
        sb_puts(&b, "{\"type\":\"syncimport\",\"ok\":");
        sb_json_bool(&b, ok);
        sb_puts(&b, ",\"applied\":");
        sb_json_int(&b, a);
        sb_puts(&b, ",\"skipped\":");
        sb_json_int(&b, s);
        sb_putc(&b, '}');
    } else {
        char outp[1024] = {0};
        bool ok = mn_app_sync_export(g_app, ctx->path[0] ? ctx->path : NULL,
                                     outp, sizeof(outp));
        sb_puts(&b, "{\"type\":\"syncexport\",\"ok\":");
        sb_json_bool(&b, ok);
        sb_puts(&b, ",\"path\":");
        sb_json_str(&b, outp);
        sb_putc(&b, '}');
    }
    if (!b.oom) {
        post_emit_owned(ctx->frame, b.data);
    } else {
        sb_free(&b);
        ctx->frame->base.release(&ctx->frame->base);
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void syncfile_start(cef_frame_t *frame_owned, bool import,
                           const char *path_or_null) {
    ne_syncfile_ctx *ctx = (ne_syncfile_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame  = frame_owned;
    ctx->import = import;
    if (path_or_null) {
        snprintf(ctx->path, sizeof(ctx->path), "%s", path_or_null);
    }
    h = CreateThread(NULL, 0, syncfile_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* --- purge-missing worker ------------------------------------------------ */

typedef struct { cef_frame_t *frame; } ne_purge_ctx;

static DWORD WINAPI purgemissing_thread(LPVOID param) {
    worker_enter();
    ne_purge_ctx *ctx = (ne_purge_ctx *)param;
    int64_t n = mn_app_purge_missing(g_app);
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"purgemissing\",\"purged\":");
    sb_json_i64(&b, n);
    sb_putc(&b, '}');
    if (!b.oom) {
        post_emit_owned(ctx->frame, b.data);
    } else {
        sb_free(&b);
        ctx->frame->base.release(&ctx->frame->base);
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void purgemissing_start(cef_frame_t *frame_owned) {
    ne_purge_ctx *ctx = (ne_purge_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    h = CreateThread(NULL, 0, purgemissing_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* --- delete-to-Recycle-Bin worker (Media Manager duplicates) ------------- */
/* {"cmd":"deletetracks","ids":[...],"tag":N} — SAFETY: files are moved to   */
/* the Windows Recycle Bin (FOF_ALLOWUNDO), NEVER hard-deleted; volumes      */
/* WITHOUT a Recycle Bin (UNC shares, DRIVE_REMOTE/REMOVABLE — where the    */
/* shell would silently fall back to a permanent delete) are SKIPPED and    */
/* reported instead. Only files that actually left the disk get their       */
/* library row removed. Shell file ops can take seconds — WORKER thread.    */
/* Reply: {"type":"deletetracks","tag","requested","recycled","removed",    */
/*  "failed":[ids],"failed_reason":["in-use"|"no-recycle",...]} (parallel;  */
/* failed = still on disk, e.g. locked by another process or unsafe drive). */

#define NE_DELTRK_MAX 256

typedef struct {
    cef_frame_t *frame;
    int64_t      ids[NE_DELTRK_MAX];
    int          n;
    int64_t      tag;
} ne_deltrk_ctx;

/* Move one file (UTF-8 path) to the Recycle Bin. Returns true only when the
 * file is no longer on disk afterwards. */
static bool recycle_file_utf8(const char *path) {
    /* SHFileOperationW requires a DOUBLE-null-terminated source list. */
    wchar_t w[1400 + 2];
    SHFILEOPSTRUCTW op;
    int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1400);
    if (len <= 0) return false;
    w[len] = 0;                     /* len includes the first terminator */
    memset(&op, 0, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = w;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/* Does the volume holding wpath have a Recycle Bin? UNC shares and remote /
 * removable drives do not — there SHFileOperation(FOF_ALLOWUNDO) silently
 * falls back to a PERMANENT delete, which the caller must never allow. */
static bool volume_can_recycle(const wchar_t *wpath) {
    wchar_t root[4];
    if (wpath[0] == L'\\' && wpath[1] == L'\\') return false;   /* UNC share  */
    if (!wpath[0] || wpath[1] != L':') return false;            /* no volume  */
    root[0] = wpath[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    return GetDriveTypeW(root) == DRIVE_FIXED;
}

static DWORD WINAPI deltrk_thread(LPVOID param) {
    worker_enter();
    ne_deltrk_ctx *ctx = (ne_deltrk_ctx *)param;
    int64_t failed_ids[NE_DELTRK_MAX];
    const char *failed_why[NE_DELTRK_MAX];   /* "in-use" | "no-recycle" */
    int nfail = 0, recycled = 0, removed = 0, i;
    /* Shell file operations want an STA thread by contract. */
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    for (i = 0; i < ctx->n; i++) {
        char path[1400];
        bool gone = false;
        const char *why = "in-use";
        if (mn_app_track_path(g_app, ctx->ids[i], path, sizeof(path))) {
            wchar_t wchk[1400];
            int wok = MultiByteToWideChar(CP_UTF8, 0, path, -1, wchk, 1400);
            if (wok > 0 && GetFileAttributesW(wchk) == INVALID_FILE_ATTRIBUTES) {
                /* File already vanished — the row still deserves cleanup. */
                gone = true;
            } else if (wok > 0 && !volume_can_recycle(wchk)) {
                /* SAFETY: this volume has no Recycle Bin — the shell would
                 * PERMANENTLY delete. Never do that: skip and report. */
                why = "no-recycle";
            } else {
                /* Release our own decoder handle first, so recycling the
                 * currently-loaded (even playing) track works instead of
                 * failing "in use" on our own open file. */
                mn_app_release_path(g_app, path);
                if (recycle_file_utf8(path)) {
                    gone = true;
                    recycled++;
                }
            }
        } else {
            gone = true;   /* unknown id: just drop whatever row remains */
        }
        if (gone) {
            if (mn_app_remove_track(g_app, ctx->ids[i])) removed++;
        } else if (nfail < NE_DELTRK_MAX) {
            failed_why[nfail] = why;
            failed_ids[nfail++] = ctx->ids[i];
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    {
        strbuf b; sb_init(&b);
        sb_puts(&b, "{\"type\":\"deletetracks\",\"tag\":");
        sb_json_i64(&b, ctx->tag);
        sb_puts(&b, ",\"requested\":"); sb_json_int(&b, ctx->n);
        sb_puts(&b, ",\"recycled\":");  sb_json_int(&b, recycled);
        sb_puts(&b, ",\"removed\":");   sb_json_int(&b, removed);
        sb_puts(&b, ",\"failed\":[");
        for (i = 0; i < nfail; i++) {
            if (i) sb_putc(&b, ',');
            sb_json_i64(&b, failed_ids[i]);
        }
        sb_puts(&b, "],\"failed_reason\":[");
        for (i = 0; i < nfail; i++) {
            if (i) sb_putc(&b, ',');
            sb_json_str(&b, failed_why[i]);
        }
        sb_puts(&b, "]}");
        if (!b.oom) {
            post_emit_owned(ctx->frame, b.data);
        } else {
            sb_free(&b);
            ctx->frame->base.release(&ctx->frame->base);
        }
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void deletetracks_start(cef_frame_t *frame_owned, const char *json) {
    ne_deltrk_ctx *ctx = (ne_deltrk_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    const char *v;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    ctx->tag   = json_get_i64(json, "tag", 0);
    /* Parse ids:[...] HERE — the json string dies with the caller. */
    v = json_find_value(json, "ids");
    if (v && *v == '[') {
        v++;
        while (*v && *v != ']' && ctx->n < NE_DELTRK_MAX) {
            char *end = NULL;
            long long id = strtoll(v, &end, 10);
            if (end == v) { v++; continue; }
            if (id > 0) ctx->ids[ctx->n++] = (int64_t)id;
            v = end;
        }
    }
    h = CreateThread(NULL, 0, deltrk_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* --- re-infer untagged worker -------------------------------------------- */

typedef struct { cef_frame_t *frame; } ne_reinfer_ctx;

static DWORD WINAPI reinfer_thread(LPVOID param) {
    worker_enter();
    ne_reinfer_ctx *ctx = (ne_reinfer_ctx *)param;
    int64_t n = mn_app_reinfer_untagged(g_app);
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"reinfer\",\"updated\":");
    sb_json_i64(&b, n);
    sb_putc(&b, '}');
    if (!b.oom) {
        post_emit_owned(ctx->frame, b.data);
    } else {
        sb_free(&b);
        ctx->frame->base.release(&ctx->frame->base);
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void reinfer_start(cef_frame_t *frame_owned) {
    ne_reinfer_ctx *ctx = (ne_reinfer_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    h = CreateThread(NULL, 0, reinfer_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* --- forced db backup worker --------------------------------------------- */

typedef struct { cef_frame_t *frame; } ne_backup_ctx;

static DWORD WINAPI backupnow_thread(LPVOID param) {
    worker_enter();
    ne_backup_ctx *ctx = (ne_backup_ctx *)param;
    bool ok = db_backup_rotate(true);   /* force: bypass the 6 h gate */
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"backup\",\"ok\":");
    sb_json_bool(&b, ok);
    sb_putc(&b, '}');
    if (!b.oom) {
        post_emit_owned(ctx->frame, b.data);
    } else {
        sb_free(&b);
        ctx->frame->base.release(&ctx->frame->base);
    }
    free(ctx);
    worker_leave();
    return 0;
}

static void backupnow_start(cef_frame_t *frame_owned) {
    ne_backup_ctx *ctx = (ne_backup_ctx *)calloc(1, sizeof(*ctx));
    HANDLE h;
    if (!ctx) {
        frame_owned->base.release(&frame_owned->base);
        return;
    }
    ctx->frame = frame_owned;
    h = CreateThread(NULL, 0, backupnow_thread, ctx, 0, NULL);
    if (h) CloseHandle(h);
    else { ctx->frame->base.release(&ctx->frame->base); free(ctx); }
}

/* --------------------------------------------------------------------------
 * Persisted scan roots (<exe_dir>\folder_kinds.txt).
 * Line format: kind|path[|last_scan_epoch_seconds]  (3rd field added later —
 * both forms parse). One line per MANUALLY added root; the settings Library
 * tab renders exactly this list with per-root stats.
 * -------------------------------------------------------------------------- */
#define NE_MAX_ROOTS 32
typedef struct {
    char      kind[32];
    char      path[1024];
    long long scanned;      /* epoch seconds of the last scan touch, 0 = n/a */
} ne_root_line;

static void roots_file_path(char *out, size_t n) {
    /* The roots list is USER DATA — it lives in the app data dir (beside
     * the library db), NOT beside the exe: a dist wipe/rebuild used to
     * silently destroy the user's scan roots. The data dir is derived
     * from g_art_dir ("<data>\art-cache"). One-time migration: an old
     * exe-dir copy is adopted if the data-dir file doesn't exist yet. */
    char data_dir[1024];
    snprintf(data_dir, sizeof(data_dir), "%s", g_art_dir);
    {
        char *slash = strrchr(data_dir, '\\');
        char *fslash = strrchr(data_dir, '/');
        if (fslash > slash) slash = fslash;
        if (slash) *slash = '\0';
    }
    snprintf(out, n, "%s\\folder_kinds.txt", data_dir);
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
        char legacy[1400];
        snprintf(legacy, sizeof(legacy), "%s\\folder_kinds.txt", g_exe_dir);
        if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES) {
            (void)CopyFileA(legacy, out, FALSE);
        }
    }
}

/* ==========================================================================
 * Per-book RESUME + per-kind LISTEN STATS (audiobooks and custom libraries).
 * book_resume.txt : album_id|track_id|pos_ms|updated   (one line per book)
 * listen_stats.txt: kind|total_ms                      (lifetime listening)
 * Recorded from the 100ms app tick every ~5s while a NON-MUSIC track plays;
 * resume answers {"cmd":"bookresume"} for the expand panel's Resume button.
 * ========================================================================== */
static void datafile_path(char *out, size_t n, const char *name) {
    char data_dir[1024];
    snprintf(data_dir, sizeof(data_dir), "%s", g_art_dir);
    {
        char *slash = strrchr(data_dir, '\\');
        char *fslash = strrchr(data_dir, '/');
        if (fslash > slash) slash = fslash;
        if (slash) *slash = '\0';
    }
    snprintf(out, n, "%s\\%s", data_dir, name);
}
static int books_read(ne_book_line *out, int max) {
    char  f[1400], line[256];
    FILE *bf;
    int   n = 0;
    datafile_path(f, sizeof(f), "book_resume.txt");
    bf = fopen(f, "r");
    if (!bf) return 0;
    while (n < max && fgets(line, sizeof(line), bf)) {
        ne_book_line b;
        if (sscanf(line, "%lld|%lld|%lld|%lld",
                   &b.album, &b.track, &b.pos, &b.updated) == 4 && b.album > 0)
            out[n++] = b;
    }
    fclose(bf);
    return n;
}
static void book_note_progress(long long album, long long track, long long pos) {
    static ne_book_line arr[NE_MAX_BOOKS];
    char  f[1400], tmp[1420];
    FILE *bf;
    int   n, i, hit = -1;
    if (album <= 0 || track <= 0) return;
    n = books_read(arr, NE_MAX_BOOKS);
    for (i = 0; i < n; i++) if (arr[i].album == album) { hit = i; break; }
    if (hit < 0) {
        if (n >= NE_MAX_BOOKS) {           /* evict the stalest entry */
            int old = 0;
            for (i = 1; i < n; i++) if (arr[i].updated < arr[old].updated) old = i;
            hit = old;
        } else hit = n++;
        arr[hit].album = album;
    }
    arr[hit].track   = track;
    arr[hit].pos     = pos;
    arr[hit].updated = (long long)time(NULL);
    datafile_path(f, sizeof(f), "book_resume.txt");
    snprintf(tmp, sizeof(tmp), "%s.tmp", f);
    bf = fopen(tmp, "w");
    if (!bf) return;
    for (i = 0; i < n; i++)
        fprintf(bf, "%lld|%lld|%lld|%lld\n",
                arr[i].album, arr[i].track, arr[i].pos, arr[i].updated);
    fclose(bf);
    MoveFileExA(tmp, f, MOVEFILE_REPLACE_EXISTING);
}
static void listen_note(const char *kind, long long add_ms) {
    char  f[1400], tmp[1420], line[128];
    char  kinds[32][32]; long long ms[32];
    int   n = 0, i, hit = -1;
    FILE *sf;
    if (!kind || !kind[0] || add_ms <= 0) return;
    datafile_path(f, sizeof(f), "listen_stats.txt");
    sf = fopen(f, "r");
    if (sf) {
        while (n < 32 && fgets(line, sizeof(line), sf)) {
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            snprintf(kinds[n], sizeof(kinds[n]), "%s", line);
            ms[n] = _strtoi64(bar + 1, NULL, 10);
            n++;
        }
        fclose(sf);
    }
    for (i = 0; i < n; i++) if (_stricmp(kinds[i], kind) == 0) { hit = i; break; }
    if (hit < 0 && n < 32) { hit = n++; snprintf(kinds[hit], sizeof(kinds[hit]), "%s", kind); ms[hit] = 0; }
    if (hit < 0) return;
    ms[hit] += add_ms;
    snprintf(tmp, sizeof(tmp), "%s.tmp", f);
    sf = fopen(tmp, "w");
    if (!sf) return;
    for (i = 0; i < n; i++) fprintf(sf, "%s|%lld\n", kinds[i], ms[i]);
    fclose(sf);
    MoveFileExA(tmp, f, MOVEFILE_REPLACE_EXISTING);
}
/* 100ms-tick hook: sample the transport ~every 5s of real playback. */
static void book_progress_tick(void) {
    static ULONGLONG last = 0;
    ULONGLONG t;
    mn_now now;
    char kind[32];
    if (!g_app) return;
    t = GetTickCount64();
    if (t - last < 5000) return;
    last = t;
    mn_app_now_lite(g_app, &now);   /* no art_path read here */
    if (!now.playing || now.track_id <= 0 || !now.track_path[0]) return;
    kind_for_path(now.track_path, kind, sizeof(kind));
    if (_stricmp(kind, "music") == 0) {
        listen_note("music", 5000);
        return;                     /* resume tracking is for books only */
    }
    listen_note(kind, 5000);
    /* book_progress DB (v7): per-chapter positions + whole-book percent +
     * content_hash snapshot — replaces the book_resume.txt flat file (the
     * legacy file is imported once at startup, see books_migrate_once). */
    mn_app_book_note(g_app, now.album_id, now.track_id,
                     (int64_t)now.position_ms, (int64_t)time(NULL));
}

/* One-time import of the legacy book_resume.txt flat file into the
 * book_progress table, then the file is renamed .migrated so this never
 * runs twice. Positions and updated timestamps are preserved (percent is
 * recomputed by the DB layer from the book's chapter durations). */
static void books_migrate_once(void) {
    static ne_book_line arr[NE_MAX_BOOKS];
    char f[1400], done[1420];
    int n, i;
    if (!g_app) return;
    n = books_read(arr, NE_MAX_BOOKS);
    if (n <= 0) return;
    for (i = 0; i < n; i++) {
        if (arr[i].album > 0 && arr[i].track > 0)
            mn_app_book_note(g_app, arr[i].album, arr[i].track,
                             arr[i].pos, arr[i].updated);
    }
    datafile_path(f, sizeof(f), "book_resume.txt");
    snprintf(done, sizeof(done), "%s.migrated", f);
    MoveFileExA(f, done, MOVEFILE_REPLACE_EXISTING);
    fprintf(stderr, "[books] migrated %d legacy resume line(s) into book_progress\n", n);
}

/* ==========================================================================
 * content_hash BACKFILL — the compute half of the immutable track
 * fingerprint (schema v7). RECIPE (documented for sync clients; any device
 * hashing the same file MUST produce the same id):
 *   h = FNV-1a 64 (offset 14695981039346656037, prime 1099511628211) over:
 *     1. the file SIZE as 8 little-endian bytes,
 *     2. the first min(64 KiB, size) bytes of the file,
 *     3. the last  min(64 KiB, max(0, size-64 KiB)) bytes (no overlap).
 *   rendered as 16 lowercase hex chars.
 * Pure content: moves/renames/retags never change it. The worker drains
 * mn_app_hashless_rows in small batches on a LOWEST-priority thread with
 * per-file sleeps, aborts on shutdown, single-flight. Kicked at startup and
 * from the 5-min heal tick so new files (scan) get fingerprinted too.
 * ========================================================================== */
#define NE_HASH_SAMPLE (64 * 1024)
static volatile LONG g_hashfill_busy = 0;

static uint64_t fp_fnv1a64_feed(uint64_t h, const unsigned char *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* Compute the content fingerprint of one file. Returns false if unreadable
 * (row is left NULL — never an error). */
static bool file_content_fp(const char *path, char *out, size_t outn) {
    static __declspec(thread) unsigned char buf[NE_HASH_SAMPLE];
    unsigned char szle[8];
    uint64_t h = 14695981039346656037ULL;
    FILE *f;
    long long size;
    size_t head, tail, got;
    int i;
    f = fopen(path, "rb");
    if (!f) return false;
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    size = _ftelli64(f);
    if (size <= 0) { fclose(f); return false; }
    for (i = 0; i < 8; i++) szle[i] = (unsigned char)((size >> (i * 8)) & 0xFF);
    h = fp_fnv1a64_feed(h, szle, 8);
    head = (size < NE_HASH_SAMPLE) ? (size_t)size : NE_HASH_SAMPLE;
    _fseeki64(f, 0, SEEK_SET);
    got = fread(buf, 1, head, f);
    if (got != head) { fclose(f); return false; }
    h = fp_fnv1a64_feed(h, buf, head);
    if (size > NE_HASH_SAMPLE) {
        long long rem = size - NE_HASH_SAMPLE;
        tail = (rem < NE_HASH_SAMPLE) ? (size_t)rem : NE_HASH_SAMPLE;
        _fseeki64(f, size - (long long)tail, SEEK_SET);
        got = fread(buf, 1, tail, f);
        if (got != tail) { fclose(f); return false; }
        h = fp_fnv1a64_feed(h, buf, tail);
    }
    fclose(f);
    snprintf(out, outn, "%016llx", (unsigned long long)h);
    return true;
}

static DWORD WINAPI hash_backfill_thread(LPVOID param) {
    enum { HB_BATCH = 64 };
    static int64_t ids[HB_BATCH];
    static char    paths[HB_BATCH][1024];
    static int64_t sizes[HB_BATCH];
    long done = 0, failed = 0;
    (void)param;
    worker_enter();
    for (;;) {
        int n, i;
        if (InterlockedCompareExchange(&g_shutting_down, 0, 0)) break;
        if (!g_app) break;
        n = mn_app_hashless_rows(g_app, ids, paths, sizes, HB_BATCH);
        if (n <= 0) break;                 /* fully fingerprinted */
        for (i = 0; i < n; i++) {
            char hex[24];
            if (InterlockedCompareExchange(&g_shutting_down, 0, 0)) break;
            if (paths[i][0] && file_content_fp(paths[i], hex, sizeof(hex))) {
                mn_app_set_content_hash(g_app, ids[i], hex, false);
                done++;
            } else {
                /* unreadable: leave NULL. Mark it skipped THIS sweep by
                 * writing nothing — the LIMIT query would loop on it, so
                 * count and bail if a whole batch failed (offline drive). */
                failed++;
            }
            Sleep(15);                     /* gentle: ~60 files/s upper bound */
        }
        if (failed >= n && n > 0) break;   /* nothing readable — stop sweep */
        failed = 0;
        Sleep(200);                        /* batch breather */
    }
    if (done > 0)
        fprintf(stderr, "[hash] fingerprinted %ld track(s) this sweep\n", done);
    worker_leave();
    InterlockedExchange(&g_hashfill_busy, 0);
    return 0;
}

static void hash_backfill_kick(void) {
    HANDLE h;
    if (InterlockedCompareExchange(&g_hashfill_busy, 1, 0) != 0) return;
    h = CreateThread(NULL, 0, hash_backfill_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_LOWEST); CloseHandle(h); }
    else InterlockedExchange(&g_hashfill_busy, 0);
}

static int roots_file_read(ne_root_line *out, int max) {
    char  kfile[1400], line[1400];
    FILE *kf;
    int   n = 0;
    roots_file_path(kfile, sizeof(kfile));
    kf = fopen(kfile, "r");
    if (!kf) return 0;
    while (n < max && fgets(line, sizeof(line), kf)) {
        char *s1 = strchr(line, '|');
        char *s2 = s1 ? strchr(s1 + 1, '|') : NULL;
        char *p, *end;
        size_t ln;
        if (!s1) continue;
        *s1 = 0;
        p = s1 + 1;
        if (s2) *s2 = 0;
        ln = strlen(p);
        while (ln && (p[ln - 1] == '\n' || p[ln - 1] == '\r')) p[--ln] = 0;
        if (!ln) continue;
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].kind, sizeof(out[n].kind), "%s", line);
        snprintf(out[n].path, sizeof(out[n].path), "%s", p);
        out[n].scanned = s2 ? _strtoi64(s2 + 1, &end, 10) : 0;
        /* dedupe (case-insensitive path) — historical files appended blindly */
        {
            int k, dup = 0;
            for (k = 0; k < n; ++k) {
                if (_stricmp(out[k].path, out[n].path) == 0) {
                    if (out[n].scanned > out[k].scanned) out[k].scanned = out[n].scanned;
                    snprintf(out[k].kind, sizeof(out[k].kind), "%s", out[n].kind);
                    dup = 1;
                    break;
                }
            }
            if (!dup) n++;
        }
    }
    fclose(kf);
    return n;
}

/* True when a kind string means the default music library. */
static int kind_is_music(const char *k) {
    return !k || !k[0] || _stricmp(k, "music") == 0;
}

/* Push every NON-MUSIC root (audiobook, podcast, custom designations) with
 * its kind label into the app so every browse/search surface can isolate
 * per-kind libraries (see mn_spec_apply_category). Call after anything that
 * changes the roots registry. */
static void sync_audiobook_roots(void) {
    static ne_root_line rf[NE_MAX_ROOTS];
    char kinds[NE_MAX_ROOTS][32];
    char paths[NE_MAX_ROOTS][512];     /* app clamps to MN_MAX_KIND_ROOTS */
    int  n, i, na = 0;
    if (!g_app) return;
    n = roots_file_read(rf, NE_MAX_ROOTS);
    for (i = 0; i < n && na < NE_MAX_ROOTS; i++) {
        if (kind_is_music(rf[i].kind)) continue;
        /* normalize legacy plural */
        if (_stricmp(rf[i].kind, "audiobooks") == 0)
            snprintf(kinds[na], sizeof(kinds[na]), "audiobook");
        else
            snprintf(kinds[na], sizeof(kinds[na]), "%s", rf[i].kind);
        snprintf(paths[na], sizeof(paths[na]), "%s", rf[i].path);
        na++;
    }
    mn_app_set_kind_roots(g_app, kinds, paths, na);
}

/* {"type":"kinds","kinds":[{"kind":"audiobook","roots":2},...]} — the
 * distinct non-music designations, for the sidebar's library sections. */
static void build_kinds(strbuf *b) {
    static ne_root_line rf[NE_MAX_ROOTS];
    char seen[NE_MAX_ROOTS][32];
    int  cnt[NE_MAX_ROOTS];
    int  n, i, k, ns = 0;
    n = roots_file_read(rf, NE_MAX_ROOTS);
    for (i = 0; i < n; i++) {
        const char *kd = rf[i].kind;
        if (kind_is_music(kd)) continue;
        if (_stricmp(kd, "audiobooks") == 0) kd = "audiobook";
        for (k = 0; k < ns; k++) if (_stricmp(seen[k], kd) == 0) break;
        if (k == ns && ns < NE_MAX_ROOTS) {
            snprintf(seen[ns], sizeof(seen[ns]), "%s", kd);
            cnt[ns] = 0;
            ns++;
        }
        if (k < ns) cnt[k]++;
    }
    sb_puts(b, "{\"type\":\"kinds\",\"kinds\":[");
    for (k = 0; k < ns; k++) {
        if (k) sb_putc(b, ',');
        sb_puts(b, "{\"kind\":"); sb_json_str(b, seen[k]);
        sb_puts(b, ",\"roots\":"); sb_json_int(b, cnt[k]);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

/* Longest-prefix kind lookup for an arbitrary library folder path. */
static void kind_for_path(const char *path, char *out, size_t cap) {
    static ne_root_line rf[NE_MAX_ROOTS];
    int    n, i;
    size_t best = 0;
    snprintf(out, cap, "music");
    n = roots_file_read(rf, NE_MAX_ROOTS);
    for (i = 0; i < n; i++) {
        size_t rl = strlen(rf[i].path);
        char  sep;
        if (rl <= best) continue;
        if (_strnicmp(path, rf[i].path, rl) != 0) continue;
        /* boundary check: the match must end at a path separator (or the root
         * already ends in one), else "D:\OST" wrongly claims "D:\OST Extra\..."
         * and miscounts a music track's listen time as ost/audiobook. */
        sep = path[rl];
        if (sep != '\0' && sep != '\\' && sep != '/'
            && rf[i].path[rl - 1] != '\\' && rf[i].path[rl - 1] != '/')
            continue;
        {
            const char *kd = kind_is_music(rf[i].kind) ? "music"
                : (_stricmp(rf[i].kind, "audiobooks") == 0 ? "audiobook"
                                                           : rf[i].kind);
            snprintf(out, cap, "%s", kd);
            best = rl;
        }
    }
}

static void roots_file_write(const ne_root_line *arr, int n) {
    char  kfile[1400], tmp[1420];
    FILE *kf;
    int   i;
    roots_file_path(kfile, sizeof(kfile));
    snprintf(tmp, sizeof(tmp), "%s.tmp", kfile);
    kf = fopen(tmp, "w");
    if (!kf) return;
    for (i = 0; i < n; ++i) {
        fprintf(kf, "%s|%s|%lld\n", arr[i].kind[0] ? arr[i].kind : "music",
                arr[i].path, arr[i].scanned);
    }
    fclose(kf);
    MoveFileExA(tmp, kfile, MOVEFILE_REPLACE_EXISTING);
}

/* Upsert a root (path NULL = stamp ALL roots with now). `kind` NULL keeps
 * the stored kind. `remove` deletes the entry instead. */
static void roots_file_touch(const char *path, const char *kind, bool remove) {
    ne_root_line roots[NE_MAX_ROOTS];
    int n = roots_file_read(roots, NE_MAX_ROOTS);
    long long now = (long long)time(NULL);
    int i, found = 0;
    if (!path) {
        for (i = 0; i < n; ++i) roots[i].scanned = now;
    } else {
        for (i = 0; i < n; ++i) {
            if (_stricmp(roots[i].path, path) == 0) {
                found = 1;
                if (remove) {
                    memmove(&roots[i], &roots[i + 1],
                            (size_t)(n - i - 1) * sizeof(roots[0]));
                    n--;
                } else {
                    roots[i].scanned = now;
                    if (kind && kind[0])
                        snprintf(roots[i].kind, sizeof(roots[i].kind), "%s", kind);
                }
                break;
            }
        }
        if (!found && !remove && n < NE_MAX_ROOTS) {
            memset(&roots[n], 0, sizeof(roots[n]));
            snprintf(roots[n].kind, sizeof(roots[n].kind), "%s",
                     (kind && kind[0]) ? kind : "music");
            snprintf(roots[n].path, sizeof(roots[n].path), "%s", path);
            roots[n].scanned = now;
            n++;
        }
    }
    roots_file_write(roots, n);
}

/* Replay the persisted roots into the app's rescan root set. */
static void register_persisted_roots(void) {
    ne_root_line roots[NE_MAX_ROOTS];
    int n = roots_file_read(roots, NE_MAX_ROOTS);
    int i;
    for (i = 0; i < n; ++i) (void)mn_app_register_root(g_app, roots[i].path);
}

/* --- library folder watcher ----------------------------------------------
 * MediaMonkey-style live monitoring: one long-lived worker holds a
 * FindFirstChangeNotification (whole subtree) per persisted root and,
 * after a change followed by a QUIET window (so a 200-file copy triggers
 * ONE rescan at the end, not 200), kicks the normal incremental rescan.
 * The handle set rebuilds whenever the roots file changes. All waits are
 * short so workers_drain()'s ~8 s shutdown window always succeeds.      */

#define NE_WATCH_QUIET_MS  4000   /* rescan after this much post-change calm */
#define NE_WATCH_POLL_MS   500
/* Idle wait timeout for the watch loop's roots-mtime re-poll + the
 * WaitForMultipleObjects fallback. FindFirstChangeNotification blocks properly
 * during the wait, so real FS changes still wake within OS notification
 * latency; this only paces the roots-file mtime re-check + the watch-disabled
 * spin. 3s is well inside the 8s workers_drain shutdown window. Kept SEPARATE
 * from NE_WATCH_POLL_MS so the active-scan drain loop below (bounded at
 * 1200 * NE_WATCH_POLL_MS = 10 min) keeps its 500ms cadence and its bound. */
#define NE_WATCH_IDLE_MS   3000

static DWORD WINAPI folder_watch_thread(LPVOID param) {
    HANDLE       h[NE_MAX_ROOTS];
    ne_root_line roots[NE_MAX_ROOTS];
    int          nh = 0;
    FILETIME     roots_mtime = {0, 0};
    ULONGLONG    pending_since = 0;  /* 0 = no change awaiting the quiet gap */
    (void)param;
    worker_enter();

    for (;;) {
        if (InterlockedCompareExchange(&g_shutting_down, 0, 0)) break;

        /* (Re)build the notification set when the roots file changes. */
        {
            char kfile[1400];
            WIN32_FILE_ATTRIBUTE_DATA fad;
            roots_file_path(kfile, sizeof(kfile));
            if (GetFileAttributesExA(kfile, GetFileExInfoStandard, &fad) &&
                CompareFileTime(&fad.ftLastWriteTime, &roots_mtime) != 0) {
                int i, n;
                for (i = 0; i < nh; ++i)
                    if (h[i] != INVALID_HANDLE_VALUE)
                        FindCloseChangeNotification(h[i]);
                nh = 0;
                roots_mtime = fad.ftLastWriteTime;
                n = roots_file_read(roots, NE_MAX_ROOTS);
                for (i = 0; i < n && nh < NE_MAX_ROOTS; ++i) {
                    wchar_t wpath[1024];
                    if (MultiByteToWideChar(CP_UTF8, 0, roots[i].path, -1, wpath,
                                            (int)(sizeof(wpath)/sizeof(wpath[0]))) <= 0)
                        continue;
                    h[nh] = FindFirstChangeNotificationW(
                        wpath, TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
                    if (h[nh] != INVALID_HANDLE_VALUE) nh++;
                    else fprintf(stderr, "[watch] cannot watch %s\n", roots[i].path);
                }
                fprintf(stderr, "[watch] watching %d root(s)\n", nh);
            }
        }

        if (!InterlockedCompareExchange(&g_watch_folders, 0, 0) || nh == 0) {
            Sleep(NE_WATCH_IDLE_MS);
            pending_since = 0;   /* disabled: drop any pending change */
            continue;
        }

        {
            DWORD w = WaitForMultipleObjects((DWORD)nh, h, FALSE, NE_WATCH_IDLE_MS);
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + (DWORD)nh) {
                pending_since = GetTickCount64();
                FindNextChangeNotification(h[w - WAIT_OBJECT_0]);
            }
        }

        if (pending_since &&
            GetTickCount64() - pending_since >= NE_WATCH_QUIET_MS && g_app) {
            pending_since = 0;
            fprintf(stderr, "[watch] change settled -> rescan\n");
            mn_app_rescan(g_app);   /* incremental; unchanged files skip fast */
            /* DRIVE the scan to completion: the final commit + missing-file
             * reconcile + tag backfill all run inside scan_status when it
             * observes the finish — and nothing else polls a scan the UI
             * didn't start. Bounded at 10 min; aborts on shutdown. */
            {
                int polls;
                for (polls = 0; polls < 1200; ++polls) {
                    mn_scan sc;
                    if (InterlockedCompareExchange(&g_shutting_down, 0, 0))
                        break;
                    Sleep(NE_WATCH_POLL_MS);
                    memset(&sc, 0, sizeof(sc));
                    mn_app_scan_status(g_app, &sc);
                    if (!sc.active) break;
                }
            }
        }
    }

    {
        int i;
        for (i = 0; i < nh; ++i)
            if (h[i] != INVALID_HANDLE_VALUE)
                FindCloseChangeNotification(h[i]);
    }
    worker_leave();
    return 0;
}

static void folder_watch_start(void) {
    HANDLE t = CreateThread(NULL, 0, folder_watch_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---- {"cmd":"roots"} — the settings Library tab's folder list -----------
 * Per manually-added root: kind, last-scan stamp, and live DB aggregates
 * (tracks / distinct albums / bytes / newest addition). Indexed range
 * scans, but still DB work — worker thread. */
static DWORD WINAPI roots_thread(LPVOID param) {
    worker_enter();
    cef_frame_t *frame = (cef_frame_t *)param;
    ne_root_line roots[NE_MAX_ROOTS];
    int n = roots_file_read(roots, NE_MAX_ROOTS);
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"roots\",\"roots\":[");
    for (int i = 0; i < n; ++i) {
        int64_t tracks = 0, albums = 0, bytes = 0, newest = 0;
        (void)mn_app_root_stats(g_app, roots[i].path,
                                &tracks, &albums, &bytes, &newest);
        if (i) sb_putc(&b, ',');
        sb_putc(&b, '{');
        sb_puts(&b, "\"path\":");    sb_json_str(&b, roots[i].path);    sb_putc(&b, ',');
        sb_puts(&b, "\"kind\":");    sb_json_str(&b, roots[i].kind);    sb_putc(&b, ',');
        sb_puts(&b, "\"scanned\":"); sb_json_i64(&b, roots[i].scanned); sb_putc(&b, ',');
        sb_puts(&b, "\"tracks\":");  sb_json_i64(&b, tracks);           sb_putc(&b, ',');
        sb_puts(&b, "\"albums\":");  sb_json_i64(&b, albums);           sb_putc(&b, ',');
        sb_puts(&b, "\"bytes\":");   sb_json_i64(&b, bytes);            sb_putc(&b, ',');
        sb_puts(&b, "\"newest\":");  sb_json_i64(&b, newest);
        sb_putc(&b, '}');
    }
    sb_puts(&b, "]}");
    if (!b.oom) post_emit_owned(frame, b.data);
    else { sb_free(&b); frame->base.release(&frame->base); }
    worker_leave();
    return 0;
}

static void roots_start(cef_frame_t *frame_owned) {
    HANDLE h = CreateThread(NULL, 0, roots_thread, frame_owned, 0, NULL);
    if (h) CloseHandle(h);
    else frame_owned->base.release(&frame_owned->base);
}

typedef struct { cef_frame_t *frame; bool art; } ne_reset_thread_arg;

static DWORD WINAPI reset_thread(LPVOID param) {
    worker_enter();
    ne_reset_thread_arg *arg = (ne_reset_thread_arg *)param;
    register_persisted_roots();
    (void)mn_app_reset_library(g_app, arg->art);
    if (arg->art) {
        /* Also drop the webroot PNG mirror; "*.png" covers the depth maps
         * ("<hash>.depth.png") as well. Regenerated on demand. */
        tagw_clear_webart();
    }
    post_emit(arg->frame, "{\"type\":\"resetdone\"}");
    free(arg);
    worker_leave();
    return 0;
}

static void resetlibrary_and_reply(cef_frame_t *frame, const char *json) {
    ne_reset_thread_arg *arg =
        (ne_reset_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) return;
    arg->art = json_get_bool(json, "art", false);
    frame->base.add_ref(&frame->base);   /* keep alive for the reply */
    arg->frame = frame;
    {
        HANDLE h = CreateThread(NULL, 0, reset_thread, arg, 0, NULL);
        if (h) CloseHandle(h);
        else { frame->base.release(&frame->base); free(arg); }
    }
}

/* ------------------------------------------------------------------------- */
/* Model download: {"cmd":"downloadmodel","id":"..","repo":"..","file":".."}  */
/* -> periodic {"type":"modeldl","id":"..","pct":N,"done":bool,"error":".."}. */
/*                                                                            */
/* mn_app_download_model runs the transfer on a worker thread and calls back  */
/* (on that worker thread) with byte progress. We hold one owned frame ref    */
/* for the life of the download and marshal each progress tick to TID_UI via  */
/* post_emit (which consumes a ref, so we add_ref per post). The retained     */
/* "keep-alive" ref is released on the terminal (finished) callback. Only one */
/* download runs at a time (mn_modeldl enforces the single slot).             */
/* ------------------------------------------------------------------------- */

typedef struct { cef_frame_t *frame; } ne_dl_reply_ctx;

/* JSON-escape a short id into out (defensive; UI ids are simple slugs). */
static void dl_json_escape(char *out, size_t n, const char *s) {
    strbuf b; sb_init(&b);
    sb_json_str(&b, s ? s : "");
    if (!b.oom) snprintf(out, n, "%s", b.data);
    else        snprintf(out, n, "\"\"");
    sb_free(&b);
}

static void dl_progress_cb(void *user, const char *id,
                           int64_t done, int64_t total,
                           bool finished, const char *err) {
    ne_dl_reply_ctx *ctx = (ne_dl_reply_ctx *)user;
    char  jid[96];
    char  jerr[160];
    char  msg[128];
    int   pct;

    if (!ctx || !ctx->frame) { if (ctx) free(ctx); return; }

    pct = (total > 0) ? (int)((done * 100) / total)
                      : (finished && !err ? 100 : 0);
    if (pct > 100) pct = 100;

    dl_json_escape(jid, sizeof(jid), id);

    if (err && err[0]) {
        char e[128];
        dl_json_escape(e, sizeof(e), err);
        snprintf(jerr, sizeof(jerr), ",\"error\":%s", e);
    } else {
        jerr[0] = '\0';
    }

    snprintf(msg, sizeof(msg),
             "{\"type\":\"modeldl\",\"id\":%s,\"pct\":%d,\"done\":%s%s}",
             jid, pct, (finished && !err) ? "true" : "false", jerr);

    /* post_emit consumes a frame ref — add one per post. */
    ctx->frame->base.add_ref(&ctx->frame->base);
    post_emit(ctx->frame, msg);

    if (finished) {
        ctx->frame->base.release(&ctx->frame->base);  /* drop keep-alive */
        free(ctx);
    }
}

/* Delete every "*.depth.png" under the art cache so depth maps regenerate
 * with the newly selected depth model. Keeps base covers + hires. */
static void depth_clear_maps(void) {
    char pattern[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!g_art_dir[0]) return;
    snprintf(pattern, sizeof(pattern), "%s\\*.depth.png", g_art_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[1700];
        snprintf(full, sizeof(full), "%s\\%s", g_art_dir, fd.cFileName);
        DeleteFileA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* {"cmd":"selectmodel","kind":"stems"|"depth","file":"<filename>"} — persist
 * the chosen model filename (see mn_app_set_selected_model). For depth the
 * change applies live: the session reloads on the next depth job and cached
 * depth maps are cleared so they regenerate with the new model. For stems the
 * path is updated for a future session build ("restart to apply"). Reply:
 * {"type":"modelselected","kind":..,"file":..,"ok":bool,"live":bool}. */
static void selectmodel_and_reply(cef_frame_t *frame, const char *json) {
    char kind[16] = {0};
    char file[256] = {0};
    bool ok;
    (void)json_get_str(json, "kind", kind, sizeof(kind));
    (void)json_get_str(json, "file", file, sizeof(file));

    ok = mn_app_set_selected_model(g_app, kind, file);

    bool live = false;
    if (ok && strcmp(kind, "depth") == 0) {
        /* Apply immediately: reload the session + regenerate depth maps. */
        depth_clear_maps();
        InterlockedExchange(&g_depth_reload, 1);
        if (g_depth_event) SetEvent(g_depth_event);
        live = true;
    }

    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"modelselected\",\"kind\":");
    sb_json_str(&b, kind);
    sb_puts(&b, ",\"file\":");
    sb_json_str(&b, file);
    sb_puts(&b, ",\"ok\":");   sb_json_bool(&b, ok);
    sb_puts(&b, ",\"live\":"); sb_json_bool(&b, live);
    sb_putc(&b, '}');
    if (!b.oom) emit_to_frame(frame, b.data);
    sb_free(&b);
}

/* {"cmd":"selectedmodels"} -> the currently active model filenames so the UI
 * can show the "Active" badge. Reply:
 * {"type":"selectedmodels","stems":"..","depth":".."}. */
static void selectedmodels_and_reply(cef_frame_t *frame, const char *json) {
    (void)json;
    char stems[256] = {0}, depth[256] = {0};
    (void)mn_app_get_selected_model(g_app, "stems", stems, sizeof(stems));
    (void)mn_app_get_selected_model(g_app, "depth", depth, sizeof(depth));
    strbuf b; sb_init(&b);
    sb_puts(&b, "{\"type\":\"selectedmodels\",\"stems\":");
    sb_json_str(&b, stems);
    sb_puts(&b, ",\"depth\":");
    sb_json_str(&b, depth);
    sb_putc(&b, '}');
    if (!b.oom) emit_to_frame(frame, b.data);
    sb_free(&b);
}

static void downloadmodel_and_reply(cef_frame_t *frame, const char *json) {
    char id[64]   = {0};
    char repo[256] = {0};
    char file[256] = {0};
    char save_as[256] = {0};
    ne_dl_reply_ctx *ctx;

    (void)json_get_str(json, "id",      id,      sizeof(id));
    (void)json_get_str(json, "repo",    repo,    sizeof(repo));
    (void)json_get_str(json, "file",    file,    sizeof(file));
    (void)json_get_str(json, "save_as", save_as, sizeof(save_as));

    /* Security: file must not escape the ai-models directory. Forward-slash
     * subfolders are allowed (HF ONNX exports commonly live at
     * "onnx/model.onnx"; the downloader uses the sub-path for the URL but
     * saves under save_as/basename); backslashes, ".." and leading "/" are
     * not. save_as names the LOCAL file, so it must be a bare filename. */
    if (!repo[0] || !file[0] || file[0] == '/' ||
        strchr(file, '\\') || strstr(file, "..") ||
        (save_as[0] && (strchr(save_as, '\\') || strchr(save_as, '/') ||
                        strstr(save_as, "..")))) {
        char jid[96]; dl_json_escape(jid, sizeof(jid), id);
        char m[160];
        snprintf(m, sizeof(m),
                 "{\"type\":\"modeldl\",\"id\":%s,\"pct\":0,\"done\":false,"
                 "\"error\":\"invalid request\"}", jid);
        emit_to_frame(frame, m);
        return;
    }

    ctx = (ne_dl_reply_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) return;
    frame->base.add_ref(&frame->base);   /* keep-alive for the whole download */
    ctx->frame = frame;

    if (!mn_app_download_model(g_app, id, repo, file,
                               save_as[0] ? save_as : NULL,
                               dl_progress_cb, ctx)) {
        /* Busy or invalid: report and clean up the keep-alive ref. */
        char jid[96]; dl_json_escape(jid, sizeof(jid), id);
        char m[160];
        snprintf(m, sizeof(m),
                 "{\"type\":\"modeldl\",\"id\":%s,\"pct\":0,\"done\":false,"
                 "\"error\":\"busy\"}", jid);
        emit_to_frame(frame, m);
        frame->base.release(&frame->base);
        free(ctx);
    }
}

/* ------------------------------------------------------------------------- */
/* Album-art refresh (maintenance): {"cmd":"refreshart"[,"limit":N]}          */
/*   -> periodic {"type":"artscan","done":D,"total":T,"gained":G}.            */
/*                                                                            */
/* Walks every album via mn_app_refresh_art (embedded APIC first, then folder */
/* sidecars), caching a thumbnail under the album key. ONE-STORE: the cached   */
/* thumbnail IS the served file, so there is no mirror step — a thumb that     */
/* lands is served on the very next stat. Fresh landings post artready so      */
/* visible tiles fill live. Runs on a worker thread (same pattern as reset).   */
/* ------------------------------------------------------------------------- */

typedef struct {
    cef_frame_t     *frame;   /* owned ref (NULL for the silent self-heal)  */
    volatile LONG    last_pct_bucket;
    int64_t          total;
} ne_artscan_ctx;

/* Per-album callback from mn_app_refresh_art (worker thread). Posts artready
 * for freshly-created thumbs and, when a frame is attached, throttles a
 * progress emit to the UI (~every 1% and on the final album). */
static void artscan_cb(void *user, const char *artist, const char *album,
                       const char *thumb, bool newly, bool src_seen,
                       int64_t done, int64_t total) {
    ne_artscan_ctx *ctx = (ne_artscan_ctx *)user;
    (void)src_seen;
    if (newly && thumb && thumb[0]) {
        char url[1300];
        if (art_thumb_url(thumb, url, sizeof(url)))
            artready_queue(artist, album, url);
    }

    if (!ctx || !ctx->frame) return;

    /* Throttle: emit on each 1% bucket change and always on the last album. */
    {
        LONG bucket = (total > 0) ? (LONG)((done * 100) / total) : 100;
        LONG prev   = InterlockedExchange(&ctx->last_pct_bucket, bucket);
        if (bucket == prev && done < total) return;
    }
    {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"artscan\",\"done\":%lld,\"total\":%lld}",
                 (long long)done, (long long)total);
        ctx->frame->base.add_ref(&ctx->frame->base);  /* post_emit consumes */
        post_emit(ctx->frame, msg);
    }
}

typedef struct {
    cef_frame_t *frame;   /* owned ref, or NULL */
    int64_t      limit;   /* <=0 == all albums  */
    bool         skip_existing;
} ne_artscan_thread_arg;

static DWORD WINAPI artscan_thread(LPVOID param) {
    worker_enter();
    ne_artscan_thread_arg *arg = (ne_artscan_thread_arg *)param;
    ne_artscan_ctx ctx;
    int64_t gained;

    ctx.frame = arg->frame;
    ctx.last_pct_bucket = -1;
    ctx.total = 0;

    gained = mn_app_refresh_art(g_app, arg->skip_existing, arg->limit,
                                artscan_cb, &ctx);

    if (arg->frame) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"artdone\",\"gained\":%lld}", (long long)gained);
        post_emit(arg->frame, msg);   /* consumes the keep-alive frame ref */
    }
    free(arg);
    worker_leave();
    return 0;
}

/* {"cmd":"refreshart"[,"limit":N]} — user-triggered full refresh (frame != NULL,
 * skip_existing=false so it regenerates everything). */
static void refreshart_and_reply(cef_frame_t *frame, const char *json) {
    ne_artscan_thread_arg *arg =
        (ne_artscan_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) return;
    arg->limit = json_get_i64(json, "limit", 0);
    arg->skip_existing = json_get_bool(json, "skip_existing", false);
    if (!arg->skip_existing) {
        /* explicit full refresh: every negative verdict is up for re-probe */
        art_none_clear_all();
        art_clear_dead_all();
    }
    frame->base.add_ref(&frame->base);   /* keep alive for progress + done */
    arg->frame = frame;
    {
        HANDLE h = CreateThread(NULL, 0, artscan_thread, arg, 0, NULL);
        if (h) CloseHandle(h);
        else { frame->base.release(&frame->base); free(arg); }
    }
}

/* Depth-map self-heal: sweep the ART CACHE for base cover thumbs whose
 * ".depth.png" companion is missing and (re)enqueue them. Fixes covers whose
 * depth job was dropped when the small queue overflowed during fast browsing
 * — the reason some albums render flat while their neighbors are 3D. Cheap
 * (directory listing + existence checks; depth_enqueue is skip-if-exists and
 * drop-if-full), so it runs at startup and then every NE_DEPTHHEAL_MS. */
static void depth_selfheal_sweep(void) {
    char pattern[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int enq = 0;

    if (!g_art_dir[0]) return;
    snprintf(pattern, sizeof(pattern), "%s\\*.png", g_art_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t n = strlen(fd.cFileName);
        char full[1700];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* only BASE covers: skip "<hash>.depth.png" / "<hash>.hires.png" */
        if (n > 10 && _stricmp(fd.cFileName + n - 10, ".depth.png") == 0) continue;
        if (n > 10 && _stricmp(fd.cFileName + n - 10, ".hires.png") == 0) continue;
        snprintf(full, sizeof(full), "%s\\%s", g_art_dir, fd.cFileName);
        depth_enqueue(full, "", "");   /* skip-if-exists inside */
        if (++enq >= 200) break;       /* bound one sweep; the timer repeats */
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* --------------------------------------------------------------------------
 * 5-minute maintenance tick body, on a BACKGROUND thread: the depth sweep +
 * art cap trims enumerate thousands of files, and the WM_TIMER thread also
 * drives mn_app_tick (gapless auto-advance) — walking there caused periodic
 * hiccups at track boundaries. Single-flight: overlapping ticks are skipped.
 * -------------------------------------------------------------------------- */
static volatile LONG g_healtick_busy = 0;

/* Rotated background DB backups: library.backup1.db is the newest of three
 * generations kept beside the live db. 6-hour cadence, first one on the
 * first maintenance tick (~5 min after boot); `force` (the "backupnow"
 * bridge command) bypasses the gate. The SQLite Backup API copies from this
 * thread's own reader connection, so playback/scans never block. Returns
 * true when a fresh backup was written. */
static bool db_backup_rotate(bool force) {
    static ULONGLONG     s_last_ms = 0;
    static volatile LONG s_busy = 0;
    ULONGLONG t = GetTickCount64();
    char b1[1400], b2[1400], b3[1400];
    bool ok = false;
    if (!g_app || !g_data_dir[0]) return false;
    if (!force && s_last_ms && t - s_last_ms < 6ULL * 60 * 60 * 1000) {
        return false;   /* gated; nothing written this tick */
    }
    /* single-flight: the heal tick and a "Back up now" click can overlap —
     * interleaved rotation renames would drop a backup generation */
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return false;
    snprintf(b1, sizeof(b1), "%s\\library.backup1.db", g_data_dir);
    snprintf(b2, sizeof(b2), "%s\\library.backup2.db", g_data_dir);
    snprintf(b3, sizeof(b3), "%s\\library.backup3.db", g_data_dir);
    DeleteFileA(b3);
    MoveFileExA(b2, b3, MOVEFILE_REPLACE_EXISTING);
    MoveFileExA(b1, b2, MOVEFILE_REPLACE_EXISTING);
    if (mn_app_backup_db(g_app, b1)) {
        s_last_ms = t;
        fprintf(stderr, "[backup] library.db -> library.backup1.db (3 kept)\n");
        ok = true;
    } else {
        fprintf(stderr, "[backup] FAILED\n");
    }
    InterlockedExchange(&s_busy, 0);
    return ok;
}

/* ==========================================================================
 * ART INTEGRITY SUBSYSTEM (one-store)
 *
 * Coverage is a protected invariant, not a cache policy:
 *   1. Base thumbs are EXEMPT from the size cap forever (the heal tick trims
 *      only the derived *.hires.png / *.depth.png tiers), so once this
 *      verify converges NOTHING can un-converge it.
 *   2. Phase A re-runs the library-wide scan-time pregeneration
 *      (mn_app_refresh_art, skip_existing — cheap check-only per cached
 *      album, extraction only for gaps, every kind included).
 *   3. Phase B verifies the keys the ALBUM GRID actually requests
 *      (mn_app_album_window rows — the same aa/album strings build_albums
 *      emits) and heals misses by TARGETED extraction under that exact key,
 *      so "healed" finally means "the grid will render it". Albums with no
 *      resolvable source get a persisted NONE verdict (never re-probed;
 *      honest health metric). Every heal posts artready.
 * Runs on the maintenance tick (low priority, single-flight) and once
 * shortly after launch.
 * ========================================================================== */
static volatile LONG g_artverify_busy;          /* single-flight guard (zero-init) */
/* g_artverify_missing / g_artverify_total forward-declared near the top */

/* Phase-A callback: count + artready for freshly-extracted thumbs; albums
 * with NO art anywhere get their (refresh-key) NONE verdict here. */
static void artverify_cb(void *user, const char *artist, const char *album,
                         const char *thumb, bool newly, bool src_seen,
                         int64_t done, int64_t total) {
    (void)user; (void)done; (void)total;
    InterlockedIncrement(&g_artverify_total);
    if (thumb && thumb[0]) {
        if (newly) {
            char url[1300];
            if (art_thumb_url(thumb, url, sizeof(url)))
                artready_queue(artist, album, url);
        }
    } else {
        /* No thumb could be produced. Counted as missing either way (honest
         * health metric), but the PERSISTED NONE verdict is reserved for
         * albums with no potential source at all: a source that exists but
         * failed to decode (corrupt sidecar / AV lock) gets only the session
         * artdead belt, which the next heal tick clears — so it is re-probed
         * instead of permanently muted (the false-NONE fix). */
        InterlockedIncrement(&g_artverify_missing);
        if (album && album[0]) {
            if (src_seen) art_mark_dead(artist, album);
            else          art_none_add(artist, album);
        }
    }
}

/* Run one verify+heal sweep (blocking; call on a worker). limit=0 sweeps the
 * whole library; limit>0 bounds the pass (post-launch kick) so a cold first
 * pass cannot contend with the initial scan's disk I/O. */
static void art_integrity_verify(int64_t limit) {
    if (!g_app) return;
    if (InterlockedCompareExchange(&g_artverify_busy, 1, 0) != 0) return;  /* single-flight */
    InterlockedExchange(&g_artverify_total, 0);
    InterlockedExchange(&g_artverify_missing, 0);

    /* Phase A: library-wide pregeneration completion (all kinds; the
     * per-track key aliases written at scan time are what make every
     * surface's derived key hit — keep them fed). */
    (void)mn_app_refresh_art(g_app, /*skip_existing=*/true, limit,
                             artverify_cb, NULL);

    /* Phase B: verify + heal the GRID's own keys, for EVERY derivation a
     * grid can actually request:
     *   pass 0            — the kind-agnostic enumeration (every album);
     *   pass 1            — the MUSIC-scoped derivation (kind "");
     *   pass 2..N         — each registered non-music kind's derivation.
     * The kind filter changes which track is FIRST for an album, so a
     * kind-scoped view derives (aa, album) variants the kind-agnostic sweep
     * never sees — measured live: first-open of the Audiobooks view minted
     * 182 brand-new thumb keys minutes after three full sweeps and
     * --arttest all said missing=0. Verifying every scoped derivation makes
     * "missing=0" mean "no view's first open can paint a coverable
     * placeholder". Extraction is skip-existing, so converged passes are
     * pure stats. */
    {
        enum { VPAGE = 128 };
        enum { VKINDS = 32 };             /* = MN_MAX_KIND_ROOTS (library_db.h) */
        static mn_album vw[VPAGE];        /* single-flight guarded; off-stack */
        static char     kinds[VKINDS][32];
        long    healed = 0, gridmiss = 0;
        int32_t nkinds = mn_app_kind_list(g_app, kinds, VKINDS);
        int     pass;
        /* An album appears in pass 0 AND its own kind's pass (same key when
         * the derivations agree) — dedup missing counts by key hash so the
         * health metric stays honest instead of double-counting every NONE. */
        uint64_t *misskeys = NULL;
        int       missn = 0, misscap = 0;
        for (pass = 0; pass < 2 + nkinds; pass++) {
            const char *kind  = (pass >= 2) ? kinds[pass - 2] : "";
            int64_t     total = (pass == 0)
                              ? mn_app_album_count_all(g_app)
                              : mn_app_album_count_kind(g_app, kind);
            int64_t     off;
            if (limit > 0 && limit < total) total = limit;
            for (off = 0; off < total; off += VPAGE) {
                int n = (pass == 0)
                      ? mn_app_album_ident_all(g_app, off, VPAGE, vw)
                      : mn_app_album_ident_kind(g_app, kind, off, VPAGE, vw);
                int i;
                if (n <= 0) break;
                for (i = 0; i < n; i++) {
                    char thumb[MN_ART_PATH_MAX];
                    bool newly = false, src_seen = false;
                    if (!vw[i].title[0]) continue;  /* unknown-album: NONE by design */
                    if (mn_app_art_check(g_app, vw[i].artist, vw[i].title,
                                         thumb, sizeof(thumb)))
                        continue;                    /* grid key servable */
                    if (art_none_has(vw[i].artist, vw[i].title)) {
                        goto count_miss;             /* recorded artless */
                    }
                    if (mn_app_art_extract_one(g_app, vw[i].artist, vw[i].title,
                                               &newly, thumb, sizeof(thumb),
                                               &src_seen)) {
                        char url[1300];
                        healed++;
                        if (art_thumb_url(thumb, url, sizeof(url)))
                            artready_queue(vw[i].artist, vw[i].title, url);
                        continue;
                    }
                    /* NONE only when NO source exists; an existing-but-
                     * undecodable source keeps only the session belt so
                     * the next tick re-probes it (false-NONE fix). */
                    if (src_seen) art_mark_dead(vw[i].artist, vw[i].title);
                    else          art_none_add(vw[i].artist, vw[i].title);
count_miss:
                    {
                        uint64_t h = art_key_hash(vw[i].artist, vw[i].title);
                        int      k;
                        bool     dup = false;
                        for (k = 0; k < missn; k++)
                            if (misskeys[k] == h) { dup = true; break; }
                        if (!dup) {
                            if (missn == misscap) {
                                int nc = misscap ? misscap * 2 : 128;
                                uint64_t *nl = (uint64_t *)
                                    realloc(misskeys, (size_t)nc * 8);
                                if (nl) { misskeys = nl; misscap = nc; }
                            }
                            if (missn < misscap) misskeys[missn++] = h;
                            gridmiss++;
                        }
                    }
                }
            }
        }
        free(misskeys);
        if (gridmiss > 0)
            InterlockedExchangeAdd(&g_artverify_missing, gridmiss);
        {
            long miss = InterlockedCompareExchange(&g_artverify_missing, 0, 0);
            long tot  = InterlockedCompareExchange(&g_artverify_total, 0, 0);
            fprintf(stderr, "[art-integrity] verify: %ld albums checked "
                    "(+%d kind-scoped passes), %ld grid-keys healed, "
                    "%ld without resolvable art\n",
                    tot, 1 + (int)nkinds, healed, miss);
        }
    }
    InterlockedExchange(&g_artverify_busy, 0);
}

/* Fire a verify+heal sweep on a low-priority background thread (single-flight
 * via g_artverify_busy). Used by the arthealth command + the post-launch kick. */
static DWORD WINAPI art_integrity_thread(LPVOID param) {
    worker_enter();
    art_integrity_verify((int64_t)(intptr_t)param);
    worker_leave();
    return 0;
}
static void art_integrity_kick(int64_t limit) {
    HANDLE h;
    if (InterlockedCompareExchange(&g_artverify_busy, 0, 0) != 0) return;  /* already running */
    h = CreateThread(NULL, 0, art_integrity_thread,
                     (LPVOID)(intptr_t)limit, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_LOWEST); CloseHandle(h); }
}

static DWORD WINAPI heal_tick_thread(LPVOID param) {
    worker_enter();
    (void)param;
    /* Transient failure verdicts expire every tick — an AV lock / mid-delete
     * can never permanently mute a cover for the session. (Persisted NONE
     * verdicts survive; they only gate re-probes of genuinely artless albums
     * and are cleared by the explicit tag-edit/artfetch/refresh hooks.) */
    art_clear_dead_all();
    /* ART INTEGRITY: verify+heal every album's cover (all kinds + the grid's
     * exact keys) each tick. Once converged this is ~one stat per album and
     * nothing can un-converge it (thumbs are trim-exempt below). */
    art_integrity_verify(0);
    /* content_hash: fingerprint any rows still NULL (new files from scans,
     * previously-offline drives). No-op once converged; single-flight. */
    hash_backfill_kick();
    /* HIRES WRITE ATTRIBUTION: the 2026-07 incident rewrote the entire hires
     * tier (1747 files / 1.49 GB) in one session with no log line naming the
     * writer. Print a per-session running summary whenever the counters have
     * moved since the previous tick so any mass regeneration is attributed
     * the moment it happens (generated = artcache decode/encode writes;
     * published = depth_publish_hires copies). */
    {
        static long long s_gen_seen = 0, s_pub_seen = 0;
        long long gf = 0, gb = 0, pf, pb;
        mn_art_hires_stats(&gf, &gb);
        pf = (long long)InterlockedCompareExchange64(&g_hirespub_files, 0, 0);
        pb = (long long)InterlockedCompareExchange64(&g_hirespub_bytes, 0, 0);
        if (gf != s_gen_seen || pf != s_pub_seen) {
            fprintf(stderr, "[art] hires writes this session: %lld generated "
                    "(%lld MB) + %lld published copies (%lld MB)\n",
                    gf, gb >> 20, pf, pb >> 20);
            s_gen_seen = gf;
            s_pub_seen = pf;
        }
    }
    if (InterlockedCompareExchange(&g_depth_batch, 0, 0))
        depth_selfheal_sweep();
    /* Cap budget applies to the DERIVED tiers only. Base thumbs are EXEMPT
     * forever — trimming them is what mathematically forced the old
     * non-convergence (4.8 GB working set vs a 2 GB cap, oldest-first) and
     * re-created the serving race every 5 minutes. Thumbs for a 3k-album
     * library are a few hundred MB; hires/depth are the bulky recyclables. */
    if (g_app) {
        mn_settings st;
        char artdir[1400] = {0};
        mn_app_get_settings(g_app, &st);
        mn_app_cache_paths(g_app, artdir, NULL, NULL, sizeof(artdir));
        if (st.art_cache_mb > 0 && artdir[0]) {
            int64_t total     = (int64_t)st.art_cache_mb << 20;
            int64_t hires_cap = total * 2 / 3;    /* hires: bigger + costlier */
            int64_t depth_cap = total - hires_cap;
            /* AUTO-SIZE the hires cap to the full-coverage working set
             * (albums x observed avg hires size, +20% headroom). Trimming
             * BELOW the working set is the v1 non-convergence math relocated:
             * each 5-min trim deletes what the next browse/Cover-Flow visit
             * regenerates, forever (measured: 1413 hires rewritten in 30 min,
             * ~800 MB per tick). The configured budget still applies to
             * anything ABOVE full coverage (stale keys from retagged albums),
             * so the tier stays bounded by library size, churn-free. */
            {
                int64_t hb = 0, hn = 0;
                mn_dir_pattern_stats(artdir, "*.hires.png", &hb, &hn);
                if (hn > 0) {
                    int64_t albums = mn_app_album_count_all(g_app);
                    int64_t need   = (albums > hn ? albums : hn) * (hb / hn);
                    need += need / 5;             /* 20% size-variance headroom */
                    if (hires_cap < need) {
                        static LONG s_warned;
                        if (!InterlockedExchange(&s_warned, 1))
                            fprintf(stderr, "[art-heal] hires working set "
                                    "needs ~%lld MB but art_cache_mb budgets "
                                    "%lld MB; auto-raising the hires cap "
                                    "(raise art_cache_mb to silence)\n",
                                    (long long)(need >> 20),
                                    (long long)(hires_cap >> 20));
                        hires_cap = need;
                    }
                }
            }
            mn_dir_trim(artdir, "*.hires.png", hires_cap);
            mn_dir_trim(artdir, "*.depth.png", depth_cap);
        }
    }
    /* versioned safety net for the library db */
    db_backup_rotate(false);
    /* phone library sync, when auto is on + host set + stale > 10 min */
    sync_auto_tick();
    InterlockedExchange(&g_healtick_busy, 0);
    worker_leave();
    return 0;
}

static void heal_tick_start(void) {
    HANDLE h;
    if (InterlockedCompareExchange(&g_healtick_busy, 1, 0) != 0) return;
    h = CreateThread(NULL, 0, heal_tick_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else   InterlockedExchange(&g_healtick_busy, 0);
}

/* --------------------------------------------------------------------------
 * Small WinHTTP helpers for the online cover-art fetch (iTunes/Deezer APIs).
 * Blocking; called from the artfetch WORKER thread (never the dispatch/UI
 * thread — a dead network would freeze every queued bridge command).
 * -------------------------------------------------------------------------- */

/* HTTPS GET `path` from `host`; malloc's the body into *out (NUL-terminated).
 * Caps at max_len. Returns body length or -1. */
static int mn_https_get(const wchar_t *host, const wchar_t *path,
                        char **out, int max_len) {
    HINTERNET s = NULL, c = NULL, r = NULL;
    char *buf = NULL;
    int   len = 0, ok = -1;

    *out = NULL;
    s = WinHttpOpen(L"Monatomic/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    WinHttpSetTimeouts(s, 8000, 8000, 8000, 15000);
    c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) goto done;
    r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) goto done;
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL)) goto done;
    {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) goto done;
    }
    buf = (char *)malloc((size_t)max_len + 1);
    if (!buf) goto done;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf + len, (DWORD)(max_len - len), &got)) break;
        if (got == 0) break;
        len += (int)got;
        if (len >= max_len) break;
    }
    buf[len] = '\0';
    *out = buf;
    buf = NULL;
    ok = len;
done:
    free(buf);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

/* Download an https URL ("https://host/path") to `file`. Returns bytes or -1. */
static int mn_https_download(const char *url, const char *file) {
    wchar_t whost[256], wpath[1600];
    char body_host[256];
    const char *p, *slash;
    char *body = NULL;
    int n = -1;

    if (strncmp(url, "https://", 8) != 0) return -1;
    p = url + 8;
    slash = strchr(p, '/');
    if (!slash || (size_t)(slash - p) >= sizeof(body_host)) return -1;
    memcpy(body_host, p, (size_t)(slash - p));
    body_host[slash - p] = '\0';
    if (MultiByteToWideChar(CP_UTF8, 0, body_host, -1, whost, 256) <= 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, slash, -1, wpath, 1600) <= 0) return -1;

    n = mn_https_get(whost, wpath, &body, 12 * 1024 * 1024);
    if (n > 0 && body) {
        FILE *fp = fopen(file, "wb");
        if (fp) {
            if ((int)fwrite(body, 1, (size_t)n, fp) != n) n = -1;
            fclose(fp);
        } else {
            n = -1;
        }
    }
    free(body);
    return n;
}

/* Percent-encode `s` for a URL query component. */
static void mn_url_encode(const char *s, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (; *s && j + 4 < n; ++s) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[j++] = (char)c;
        } else if (c == ' ') {
            out[j++] = '+';
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = '\0';
}

/* Extract the first "key":"value" string after `key` in a JSON body into out.
 * Naive but sufficient for the iTunes/Deezer search responses. */
static bool mn_json_find_str(const char *json, const char *key,
                             char *out, size_t n) {
    char pat[64];
    const char *p, *q;
    size_t j = 0;
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    q = p;
    while (*q && *q != '"' && j + 1 < n) {
        if (*q == '\\' && q[1] == '/') q++;   /* JSON-escaped slash */
        out[j++] = *q++;
    }
    out[j] = '\0';
    return j > 0;
}

/* Delete every file in `dir` matching `pattern` (non-recursive). */
static void mn_dir_delete_matching(const char *dir, const char *pattern) {
    char pat[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\%s", dir, pattern);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[1700];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        DeleteFileA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Evict oldest files in `dir` (matching `pattern`) until under cap_bytes. */
static void mn_dir_trim(const char *dir, const char *pattern, int64_t cap_bytes) {
    typedef struct { char name[512]; int64_t size; int64_t mtime; } tf;
    char pat[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    tf *list = NULL;
    int n = 0, capn = 0;
    int64_t total = 0;

    if (cap_bytes <= 0) return;
    snprintf(pat, sizeof(pat), "%s\\%s", dir, pattern);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n == capn) {
            int nc = capn ? capn * 2 : 512;
            tf *nl = (tf *)realloc(list, (size_t)nc * sizeof(tf));
            if (!nl) break;
            list = nl; capn = nc;
        }
        snprintf(list[n].name, sizeof(list[n].name), "%s", fd.cFileName);
        list[n].size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        list[n].mtime = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                        fd.ftLastWriteTime.dwLowDateTime;
        total += list[n].size;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (total > cap_bytes && n > 1) {
        int i, j;
        for (i = 1; i < n; ++i) {
            tf key = list[i];
            for (j = i - 1; j >= 0 && list[j].mtime > key.mtime; --j) list[j + 1] = list[j];
            list[j + 1] = key;
        }
        for (i = 0; i < n - 1 && total > cap_bytes; ++i) {
            char full[1700];
            snprintf(full, sizeof(full), "%s\\%s", dir, list[i].name);
            if (DeleteFileA(full)) total -= list[i].size;
        }
    }
    free(list);
}

/* Non-recursive per-pattern dir stats: total bytes + file count of the files
 * matching `pattern` directly inside `dir` (the shape of one art tier). */
static void mn_dir_pattern_stats(const char *dir, const char *pattern,
                                 int64_t *bytes, int64_t *files) {
    char pat[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\%s", dir, pattern);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        *bytes += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        *files += 1;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Recursive directory stats (bytes + file count), bounded depth. */
static void mn_dir_stats(const char *dir, int depth,
                         int64_t *bytes, int64_t *files) {
    char pattern[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (depth > 6) return;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[1700];
            snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
            mn_dir_stats(sub, depth + 1, bytes, files);
        } else {
            *bytes += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            *files += 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void artscan_selfheal_start(void) {
    ne_artscan_thread_arg *arg =
        (ne_artscan_thread_arg *)calloc(1, sizeof(*arg));
    if (!arg) return;
    arg->frame = NULL;
    arg->limit = 0;              /* all albums, but skip cached ones (cheap) */
    arg->skip_existing = true;
    {
        HANDLE h = CreateThread(NULL, 0, artscan_thread, arg, 0, NULL);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_LOWEST); CloseHandle(h); }
        else free(arg);
    }
}

/* The chrome.webview shim, registered as a CEF V8 EXTENSION in
 * on_web_kit_initialized. Extensions are auto-injected into every V8 context
 * before any page script runs, and `native function` declarations route calls
 * into our cef_v8_handler_t — the canonical CEF way to expose native bindings
 * (no manual eval / set_value_bykey needed). */
static const char *NE_SHIM_JS =
    "var chrome;"
    "if (!chrome) chrome = {};"
    "(function(){"
    "  var listeners = [];"
    "  chrome.webview = {"
    "    __monatomic: true,"
    "    postMessage: function(m){"
    "      native function MnSend();"
    "      try { var s = (typeof m === 'string') ? m : JSON.stringify(m);"
    "            MnSend(s); } catch(e){}"
    "    },"
    "    addEventListener: function(t, cb){ if (t === 'message' && typeof cb === 'function') listeners.push(cb); },"
    "    removeEventListener: function(t, cb){ if (t === 'message'){ var i = listeners.indexOf(cb); if (i>=0) listeners.splice(i,1); } },"
    "    _emit: function(obj){"
    "      var ev = { data: obj };"
    "      for (var i = 0; i < listeners.length; i++){ try { listeners[i](ev); } catch(e){} }"
    "    }"
    "  };"
    "  chrome.webview.postMessage('{\"cmd\":\"hello\"}');"
    "})();";

/* --- render process handler --- */
typedef struct {
    cef_render_process_handler_t handler;
    refbase                      rb;
} ne_render_handler;

static void CEF_CALLBACK rph_on_web_kit_initialized(
        cef_render_process_handler_t *self) {
    (void)self;
    /* Register the chrome.webview shim as a V8 extension. CEF injects it into
     * every context before page scripts; `native function MnSend()` routes to
     * g_v8_handler (v8h_execute). This MUST be called from this callback. */
    cef_string_t name; cefstr_from_ascii(&name, "v8/monatomic");
    cef_string_t code; cefstr_from_utf8(&code, NE_SHIM_JS);
    cef_register_extension(&name, &code, &g_v8_handler.handler);
    cef_string_clear(&name);
    cef_string_clear(&code);
}

static ne_render_handler g_render_handler;

/* ========================================================================= */
/* BROWSER PROCESS: life-span handler + client (on_process_message_received). */
/* ========================================================================= */

/* --- life-span handler --- */
typedef struct {
    cef_life_span_handler_t handler;
    refbase                 rb;
} ne_life_span_handler;

static void CEF_CALLBACK lsh_on_after_created(cef_life_span_handler_t *self,
                                              cef_browser_t *browser) {
    (void)self;
    if (!browser) return;
    EnterCriticalSection(&g_browser_lock);
    if (!g_browser) {
        g_browser = browser;
        browser->base.add_ref(&browser->base);  /* keep our own ref */
    }
    LeaveCriticalSection(&g_browser_lock);
}

/* BLOCK every popup, unconditionally. The app is single-window by design —
 * nothing legitimate opens a second browser window. Without this, any
 * window.open / target=_blank / Chromium-internal UI (first-run, downloads,
 * devtools prompts) can spawn a Chrome-looking window at runtime — users
 * report it as "the app opened Chrome". Return 1 = cancel the popup. */
static int CEF_CALLBACK lsh_on_before_popup(
        cef_life_span_handler_t *self, cef_browser_t *browser,
        cef_frame_t *frame, int popup_id, const cef_string_t *target_url,
        const cef_string_t *target_frame_name,
        cef_window_open_disposition_t target_disposition, int user_gesture,
        const cef_popup_features_t *popup_features,
        cef_window_info_t *window_info, cef_client_t **client,
        cef_browser_settings_t *browser_settings, cef_dictionary_value_t **extra_info,
        int *no_javascript_access) {
    (void)self; (void)browser; (void)frame; (void)popup_id; (void)target_url;
    (void)target_frame_name; (void)target_disposition; (void)user_gesture;
    (void)popup_features; (void)window_info; (void)client;
    (void)browser_settings; (void)extra_info; (void)no_javascript_access;
    return 1;   /* cancel */
}

/* Set once the browser has begun closing. CEF's default do_close handling for a
 * parented (WS_CHILD) browser sends WM_CLOSE back to the host window; without
 * this flag the host's WM_CLOSE would just post another close task forever and
 * the window could never actually close. */
static volatile LONG g_closing = 0;

static int CEF_CALLBACK lsh_do_close(cef_life_span_handler_t *self,
                                     cef_browser_t *browser) {
    (void)self; (void)browser;
    /* Mark the close as in progress, then allow the standard close path
     * (return false => CEF sends WM_CLOSE to the host window, which now
     * destroys itself instead of re-requesting a browser close). */
    InterlockedExchange(&g_closing, 1);
    return 0;
}

static void CEF_CALLBACK lsh_on_before_close(cef_life_span_handler_t *self,
                                             cef_browser_t *browser) {
    (void)self; (void)browser;
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) {
        g_browser->base.release(&g_browser->base);
        g_browser = NULL;
    }
    LeaveCriticalSection(&g_browser_lock);
    /* Last browser gone: post WM_CLOSE-equivalent to the host window so the
     * main-thread Win32 loop breaks (PostQuitMessage runs on that thread). With
     * multi_threaded_message_loop=1 there is no CEF message loop to quit; the
     * main thread owns the run loop. */
    if (g_host_hwnd) PostMessageW(g_host_hwnd, WM_APP + 1, 0, 0);
}

static ne_life_span_handler g_life_span_handler;

/* --- client --- */
typedef struct {
    cef_client_t client;
    refbase      rb;
} ne_client;

static cef_life_span_handler_t *CEF_CALLBACK client_get_life_span_handler(
        cef_client_t *self) {
    (void)self;
    g_life_span_handler.handler.base.add_ref(&g_life_span_handler.handler.base);
    return &g_life_span_handler.handler;
}

static int CEF_CALLBACK client_on_process_message_received(
        cef_client_t *self,
        cef_browser_t *browser,
        cef_frame_t *frame,
        cef_process_id_t source_process,
        cef_process_message_t *message) {
    (void)self; (void)browser; (void)source_process;
    if (!message) return 0;

    /* Match the command channel by name. */
    char *name = utf8_from_userfree(message->get_name(message));
    int handled = 0;
    if (name && strcmp(name, NE_MSG_CMD) == 0) {
        cef_list_value_t *args = message->get_argument_list(message);
        if (args && args->get_size(args) >= 1) {
            char *json = utf8_from_userfree(args->get_string(args, 0));
            if (json) {
                dispatch_command(frame, json);
                free(json);
            }
        }
        handled = 1;
    }
    free(name);
    return handled;
}

/* Display handler: sink the page's console messages quietly (returning 1
 * keeps them out of CEF's own log). */
typedef struct { cef_display_handler_t handler; refbase rb; } ne_display_handler;
static ne_display_handler g_display_handler;

static int CEF_CALLBACK dh_on_console_message(cef_display_handler_t *self,
                                              cef_browser_t *browser,
                                              cef_log_severity_t level,
                                              const cef_string_t *message,
                                              const cef_string_t *source,
                                              int line) {
    (void)self; (void)browser; (void)level; (void)message; (void)source; (void)line;
    return 0;
}

static cef_display_handler_t *CEF_CALLBACK client_get_display_handler(
        cef_client_t *self) {
    (void)self;
    g_display_handler.handler.base.add_ref(&g_display_handler.handler.base);
    return &g_display_handler.handler;
}

/* Load handler: kept wired for lifecycle completeness; intentionally quiet. */
typedef struct { cef_load_handler_t handler; refbase rb; } ne_load_handler;
static ne_load_handler g_load_handler;

static void CEF_CALLBACK loadh_on_load_start(cef_load_handler_t *self,
                                             cef_browser_t *browser,
                                             cef_frame_t *frame,
                                             cef_transition_type_t tt) {
    (void)self; (void)browser; (void)frame; (void)tt;
}

static void CEF_CALLBACK loadh_on_load_end(cef_load_handler_t *self,
                                           cef_browser_t *browser,
                                           cef_frame_t *frame,
                                           int http_status) {
    (void)self; (void)browser; (void)frame; (void)http_status;
}

static void CEF_CALLBACK loadh_on_load_error(cef_load_handler_t *self,
                                             cef_browser_t *browser,
                                             cef_frame_t *frame,
                                             cef_errorcode_t err,
                                             const cef_string_t *err_text,
                                             const cef_string_t *failed_url) {
    (void)self; (void)browser; (void)frame; (void)err; (void)err_text; (void)failed_url;
}

static cef_load_handler_t *CEF_CALLBACK client_get_load_handler(cef_client_t *self) {
    (void)self;
    g_load_handler.handler.base.add_ref(&g_load_handler.handler.base);
    return &g_load_handler.handler;
}

static ne_client g_client;

/* ========================================================================= */
/* APP: browser-process handler (on_context_initialized creates the browser)  */
/* + render-process handler accessor + command-line switches.                 */
/* ========================================================================= */

/* Build a file:/// URL to ui/index.html from an OS path (\ -> /, %-escape). */
static void build_file_url_utf8(char *url, size_t url_n, const char *ui_dir) {
    char full[1300];
    if (!_fullpath(full, ui_dir, sizeof(full))) {
        snprintf(full, sizeof(full), "%s", ui_dir);
    }
    size_t o = 0;
    const char *prefix = "file:///";
    for (const char *p = prefix; *p && o + 1 < url_n; ++p) url[o++] = *p;
    for (const unsigned char *p = (const unsigned char *)full; *p && o + 4 < url_n; ++p) {
        unsigned char c = *p;
        if (c == '\\') { url[o++] = '/'; }
        else if (c == ' ') { url[o++] = '%'; url[o++] = '2'; url[o++] = '0'; }
        else if (c == '#') { url[o++] = '%'; url[o++] = '2'; url[o++] = '3'; }
        else if (c == '?') { url[o++] = '%'; url[o++] = '3'; url[o++] = 'F'; }
        else if (c == '%') { url[o++] = '%'; url[o++] = '2'; url[o++] = '5'; }
        else { url[o++] = (char)c; }
    }
    const char *tail = "/index.html";
    for (const char *p = tail; *p && o + 1 < url_n; ++p) url[o++] = *p;
    url[o] = 0;
}

/* Create the CEF browser as a child of the host HWND, filling its client area. */
static void create_browser(void) {
    RECT rc;
    GetClientRect(g_host_hwnd, &rc);

    cef_window_info_t wi;
    memset(&wi, 0, sizeof(wi));
    wi.size = sizeof(wi);
    wi.parent_window = g_host_hwnd;
    wi.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE;
    wi.bounds.x = 0;
    wi.bounds.y = 0;
    wi.bounds.width  = rc.right - rc.left;
    wi.bounds.height = rc.bottom - rc.top;
    /* A client-provided WS_CHILD parent window requires Alloy runtime style;
     * Chrome style (the DEFAULT in CEF 144) manages its own top-level window and
     * does not support being hosted as a child of an arbitrary HWND. */
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

    char url_utf8[3072];
    build_file_url_utf8(url_utf8, sizeof(url_utf8), g_ui_dir);
    cef_string_t url; cefstr_from_utf8(&url, url_utf8);

    cef_browser_settings_t bs;
    memset(&bs, 0, sizeof(bs));
    bs.size = sizeof(bs);
    /* Opaque dark background (ARGB, alpha 0xFF): a 0 background is transparent,
     * so newly-exposed regions during a grow-resize composite against an
     * undefined/white backing => white flash. Match the DWM caption (6,6,8). */
    bs.background_color = 0xFF060608u;

    cef_browser_host_create_browser(&wi, &g_client.client, &url, &bs, NULL, NULL);
    cef_string_clear(&url);
}

/* --- browser process handler --- */
typedef struct {
    cef_browser_process_handler_t handler;
    refbase                       rb;
} ne_browser_process_handler;

/* A cef_task_t that runs create_browser() on the CEF UI thread. CEF 144 requires
 * cef_browser_host_create_browser to be called on TID_UI; with
 * multi_threaded_message_loop=1, on_context_initialized already fires on the real
 * UI thread, and cef_post_task(TID_UI,...) lands there reliably. The host window
 * is created earlier, on the MAIN thread, so create_browser has a valid, sized
 * parent HWND to fill. */
typedef struct { cef_task_t task; refbase rb; } ne_init_task;
static ne_init_task g_init_task;

static void CEF_CALLBACK init_task_execute(cef_task_t *self) {
    (void)self;
    if (g_host_hwnd) create_browser();
}

static void CEF_CALLBACK bph_on_context_initialized(
        cef_browser_process_handler_t *self) {
    (void)self;
    /* Post browser creation onto the UI thread (see ne_init_task above). */
    cef_post_task(TID_UI, &g_init_task.task);
}

static ne_browser_process_handler g_bph;

/* --- app --- */
typedef struct {
    cef_app_t app;
    refbase   rb;
} ne_app_impl;

static void append_cmd_switch(cef_command_line_t *cl, const char *name) {
    cef_string_t sw;
    cefstr_from_ascii(&sw, name);
    cl->append_switch(cl, &sw);
    cef_string_clear(&sw);
}

static void append_cmd_switch_value(cef_command_line_t *cl,
                                    const char *name, const char *value) {
    cef_string_t sw, v;
    cefstr_from_ascii(&sw, name);
    cefstr_from_ascii(&v, value);
    cl->append_switch_with_value(cl, &sw, &v);
    cef_string_clear(&sw);
    cef_string_clear(&v);
}

static void CEF_CALLBACK app_on_before_command_line_processing(
        cef_app_t *self,
        const cef_string_t *process_type,
        cef_command_line_t *command_line) {
    (void)self; (void)process_type;
    if (!command_line) return;

    /* Allow the file:// UI to fetch fonts/art from file:// (dev-style loading). */
    append_cmd_switch(command_line, "allow-file-access-from-files");
    append_cmd_switch(command_line, "disable-web-security");

    /* MN_CDP=<port>: enable Chrome DevTools Protocol for live diagnosis. */
    {
        const char *cdp = getenv("MN_CDP");
        if (cdp && cdp[0]) {
            append_cmd_switch_value(command_line, "remote-debugging-port", cdp);
            append_cmd_switch_value(command_line, "remote-allow-origins", "*");
        }
    }

    /* GPU compositing. History: the OUT-OF-PROCESS GPU process crash-looped
     * very early in development ("Failed to create shared context for
     * virtualization", 0xC0000005) and the app shipped first with disable-gpu
     * (software raster), then with --in-process-gpu. PROFILED 2026-07: with
     * --in-process-gpu the compositor still ran in a software (SwiftShader-
     * class) path — 16 fps idle / 7 fps scrolling at 4K, cost scaling with
     * pixel count. RE-TESTED 2026-07: the full OOP GPU process still
     * crash-loops on this vendored CEF/driver combo (STATUS_BREAKPOINT,
     * "Failed to create shared context for virtualization") and Chromium
     * falls back to SwiftShader — so OOP stays off. Instead: GPU work in the
     * browser process, pinned to the ANGLE D3D11 backend (the virtualization
     * crash is a native-GL-path symptom), with hardware rasterization of
     * page content:                                                        */
    append_cmd_switch_value(command_line, "use-angle", "d3d11");
    if (getenv("MN_LOWPOWER")) {
        /* Low-power mode: still use the GPU (software raster at any
         * resolution makes window RESIZING crawl) — just don't override
         * the driver blocklist or oversubscribe raster threads. */
        append_cmd_switch(command_line, "enable-gpu-rasterization");
        append_cmd_switch(command_line, "enable-zero-copy");
        append_cmd_switch_value(command_line, "num-raster-threads", "2");
        append_cmd_switch_value(command_line, "enable-features",
                                "NetworkServiceInProcess");
    } else {
        append_cmd_switch(command_line, "ignore-gpu-blocklist");
        append_cmd_switch(command_line, "enable-gpu-rasterization");
        append_cmd_switch(command_line, "enable-zero-copy");
        append_cmd_switch_value(command_line, "num-raster-threads", "4");
        /* NetworkServiceInProcess: BEST-EFFORT — asks for the network service
         * (used only for lyrics providers + model downloads) as a browser
         * thread instead of a utility process. Chromium 144 ignores it (the
         * upstream feature was retired; the utility process still spawns,
         * ~35 MB) but it is harmless and future CEF builds may honor it. */
        append_cmd_switch_value(command_line, "enable-features",
                                "CanvasOopRasterization,NetworkServiceInProcess");
    }

    /* De-bloat: this Chromium serves exactly ONE local single-page UI —
     * strip subsystems that only exist for the open web. Fewer renderer
     * processes (one site, one process), no extensions machinery, no
     * speech/translate/print surfaces. */
    append_cmd_switch(command_line, "disable-extensions");
    append_cmd_switch(command_line, "disable-component-extensions-with-background-pages");
    append_cmd_switch(command_line, "disable-speech-api");
    append_cmd_switch(command_line, "disable-print-preview");
    append_cmd_switch(command_line, "disable-site-isolation-trials");
    append_cmd_switch(command_line, "process-per-site");
    append_cmd_switch_value(command_line, "renderer-process-limit", "1");
    append_cmd_switch_value(command_line, "disable-features",
                            /* CEF's bootstrap injects its OWN disable-features
                             * (hang watcher, SideBySide, Glic, LensOverlay)
                             * BEFORE this callback, and last-switch-wins —
                             * carry its entries so ours doesn't erase them. */
                            "EnableHangWatcher,SideBySide,GlicActorUi,LensOverlay,"
                            "Translate,MediaRouter,OptimizationHints,"
                            "InterestFeedContentSuggestions,"
                            /* BackForwardCache: no navigation, so it only
                             * pins a second document tree in memory. */
                            "BackForwardCache,Prerender2,"
                            /* CalculateNativeWinOcclusion stays ENABLED (it
                             * was disabled for per-resize overhead, but it is
                             * what lets Chromium throttle the renderer when
                             * the window is fully covered by other windows —
                             * a real battery/CPU win that the explicit
                             * minimize-driven was_hidden path cannot see). */
                            /* Media keys are handled via RegisterHotKey. */
                            "HardwareMediaKeyHandling,"
                            /* No warm spare renderer: renderer-process-limit
                             * is 1 and there is exactly one page, ever — the
                             * spare just pins ~60 MB waiting for a second
                             * site that cannot exist. */
                            "SpareRendererForSitePerProcess,"
                            /* Storage service folds into the browser process
                             * (localStorage only; no service workers, no
                             * IndexedDB churn) — one fewer utility process. */
                            "StorageServiceOutOfProcess,"
                            /* Media-session plumbing + the Chrome media hub:
                             * transport is native (RegisterHotKey + taskbar
                             * thumb buttons), the page never plays audio. */
                            "MediaSessionService,GlobalMediaControls,"
                            /* No web forms — search box is plain text. */
                            "AutofillServerCommunication,"
                            /* WebGPU: unused by the UI (stems run in native
                             * ORT/CUDA); disabling also lets the build drop
                             * dxcompiler.dll + dxil.dll (~35 MB, Windows). */
                            "WebGPU");

    /* Render / frame-scheduling tuning for a single foreground local UI.
     *
     * POWER: background throttling is deliberately left ENABLED (the old
     * disable-background-timer-throttling / disable-renderer-backgrounding /
     * disable-backgrounding-occluded-windows trio kept the renderer at full
     * cadence even minimized). When the window is minimized or fully
     * occluded, Chromium now clamps JS timers to ~1 Hz, parks rAF entirely,
     * and stops presenting frames — near-zero CPU/GPU. Playback is untouched
     * (audio + stems run natively in this process, not in the renderer);
     * the UI catches up instantly on restore (visibilitychange wakes the
     * main loop in app.js). */
    append_cmd_switch(command_line, "disable-hang-monitor");
    append_cmd_switch(command_line, "disable-smooth-scrolling");
    /* Accessibility-tree rebuilds measured ~65ms PER MAIN FRAME at 4K with a
     * large album grid (Blink.Accessibility.UpdateTime in the trace) — a
     * third of the whole frame budget spent serializing UI nobody reads.
     * Disable unless a screen reader is actually running (in which case the
     * cost is the price of accessibility, gladly paid). */
    {
        BOOL sr = FALSE;
        SystemParametersInfoW(SPI_GETSCREENREADER, 0, &sr, 0);
        if (!sr)
            append_cmd_switch(command_line, "disable-renderer-accessibility");
    }
    /* NOTE: do NOT add a second enable-features here — append_switch_with_value
     * OVERWRITES the earlier enable-features (CanvasOopRasterization), it does
     * not merge. RawDraw was also breaking text/glyph rasterization on this
     * ANGLE/D3D11 config (solid fills rendered, all text vanished). Any extra
     * enable-features feature must be appended to the ONE list at line ~5305. */

    /* KILL every Chrome-bootstrap consumer behavior. CEF 144's chrome
     * bootstrap can run FIRST-RUN and DEFAULT-BROWSER flows on a fresh
     * profile (e.g. right after the cef_cache is cleared) — that pops a
     * Chrome-branded window / the OS default-browser flow at APP LAUNCH,
     * which users correctly report as "the app opens Chrome". The GCM
     * registration errors in the log proved these services were live. */
    append_cmd_switch(command_line, "no-first-run");
    append_cmd_switch(command_line, "no-default-browser-check");
    /* The page never plays audio (playback is native miniaudio) — mute the
     * whole browser so Chromium never spins up its audio stack. */
    append_cmd_switch(command_line, "mute-audio");
    /* HTTP disk cache: only lyrics JSON + model downloads flow through it
     * (UI + art are file://). Cap it so cef_cache can't balloon. */
    append_cmd_switch_value(command_line, "disk-cache-size", "16777216");
    append_cmd_switch(command_line, "disable-breakpad");   /* no crash uploader */
    append_cmd_switch(command_line, "no-pings");           /* no hyperlink audits */
    append_cmd_switch(command_line, "disable-spell-checking");
    append_cmd_switch(command_line, "disable-background-networking");
    append_cmd_switch(command_line, "disable-component-update");
    append_cmd_switch(command_line, "disable-sync");
    append_cmd_switch(command_line, "disable-client-side-phishing-detection");
    append_cmd_switch(command_line, "disable-domain-reliability");
    append_cmd_switch(command_line, "metrics-recording-only");
    append_cmd_switch(command_line, "disable-default-apps");
}

static cef_browser_process_handler_t *CEF_CALLBACK app_get_browser_process_handler(
        cef_app_t *self) {
    (void)self;
    g_bph.handler.base.add_ref(&g_bph.handler.base);
    return &g_bph.handler;
}

static cef_render_process_handler_t *CEF_CALLBACK app_get_render_process_handler(
        cef_app_t *self) {
    (void)self;
    g_render_handler.handler.base.add_ref(&g_render_handler.handler.base);
    return &g_render_handler.handler;
}

static ne_app_impl g_app_impl;

/* ------------------------------------------------------------------------- */
/* Wire up all singleton handler vtables.                                     */
/* ------------------------------------------------------------------------- */

static void init_handlers(void) {
    /* app */
    memset(&g_app_impl, 0, sizeof(g_app_impl));
    INIT_BASE(&g_app_impl.app, &g_app_impl.rb, cef_app_t);
    g_app_impl.app.on_before_command_line_processing =
        app_on_before_command_line_processing;
    g_app_impl.app.get_browser_process_handler = app_get_browser_process_handler;
    g_app_impl.app.get_render_process_handler  = app_get_render_process_handler;

    /* browser process handler */
    memset(&g_bph, 0, sizeof(g_bph));
    INIT_BASE(&g_bph.handler, &g_bph.rb, cef_browser_process_handler_t);
    g_bph.handler.on_context_initialized = bph_on_context_initialized;

    /* init task (posted to TID_UI to create the window + browser) */
    memset(&g_init_task, 0, sizeof(g_init_task));
    INIT_BASE(&g_init_task.task, &g_init_task.rb, cef_task_t);
    g_init_task.task.execute = init_task_execute;

    /* render process handler */
    memset(&g_render_handler, 0, sizeof(g_render_handler));
    INIT_BASE(&g_render_handler.handler, &g_render_handler.rb,
              cef_render_process_handler_t);
    g_render_handler.handler.on_web_kit_initialized = rph_on_web_kit_initialized;

    /* v8 handler */
    memset(&g_v8_handler, 0, sizeof(g_v8_handler));
    INIT_BASE(&g_v8_handler.handler, &g_v8_handler.rb, cef_v8_handler_t);
    g_v8_handler.handler.execute = v8h_execute;

    /* client */
    memset(&g_client, 0, sizeof(g_client));
    INIT_BASE(&g_client.client, &g_client.rb, cef_client_t);
    g_client.client.get_life_span_handler = client_get_life_span_handler;
    g_client.client.get_load_handler = client_get_load_handler;
    g_client.client.get_display_handler = client_get_display_handler;

    /* display handler (console piping) */
    memset(&g_display_handler, 0, sizeof(g_display_handler));
    INIT_BASE(&g_display_handler.handler, &g_display_handler.rb, cef_display_handler_t);
    g_display_handler.handler.on_console_message = dh_on_console_message;
    g_client.client.on_process_message_received =
        client_on_process_message_received;

    /* load handler singleton */
    memset(&g_load_handler, 0, sizeof(g_load_handler));
    INIT_BASE(&g_load_handler.handler, &g_load_handler.rb, cef_load_handler_t);
    g_load_handler.handler.on_load_start = loadh_on_load_start;
    g_load_handler.handler.on_load_end   = loadh_on_load_end;
    g_load_handler.handler.on_load_error = loadh_on_load_error;

    /* life-span handler */
    memset(&g_life_span_handler, 0, sizeof(g_life_span_handler));
    INIT_BASE(&g_life_span_handler.handler, &g_life_span_handler.rb,
              cef_life_span_handler_t);
    g_life_span_handler.handler.on_after_created = lsh_on_after_created;
    g_life_span_handler.handler.on_before_popup  = lsh_on_before_popup;
    g_life_span_handler.handler.do_close         = lsh_do_close;
    g_life_span_handler.handler.on_before_close  = lsh_on_before_close;
}

/* ------------------------------------------------------------------------- */
/* Host window: resizes the browser child, drives app_tick, closes cleanly.   */
/* ------------------------------------------------------------------------- */

/* A one-shot task that resizes the browser child window + notifies CEF, run on
 * the CEF UI thread (all cef_browser_host_t calls must happen there under
 * multi_threaded_message_loop=1). Allocated on demand; freed via
 * heap_task_release when CEF drops the last reference.
 *
 * COALESCED: WM_SIZE storms (interactive drag-resize at 4K) used to post one
 * task per message, flooding the CEF UI thread. g_resize_pending gates it to
 * at most one in-flight task; the flag clears at the START of execute so the
 * task always reads the freshest client rect and a late WM_SIZE re-posts. */
typedef struct { cef_task_t task; refbase rb; } ne_resize_task;

static volatile LONG g_resize_pending = 0;
static volatile LONG g_in_sizemove    = 0;   /* inside an interactive drag  */

static void CEF_CALLBACK resize_task_execute(cef_task_t *self) {
    (void)self;
    cef_browser_t *br = NULL;
    InterlockedExchange(&g_resize_pending, 0);   /* allow the next coalesce */
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) { br = g_browser; br->base.add_ref(&br->base); }
    LeaveCriticalSection(&g_browser_lock);
    if (br) {
        cef_browser_host_t *host = br->get_host(br);
        if (host) {
            HWND child = host->get_window_handle(host);
            if (child && g_host_hwnd) {
                RECT rc;
                GetClientRect(g_host_hwnd, &rc);
                /* SWP_NOREDRAW: don't force a synchronous GDI repaint of the
                 * child here — was_resized() below is the single authoritative
                 * repaint driver (CEF invalidates + re-rasterizes internally).
                 * The old MoveWindow(...,TRUE) triggered TWO repaints per
                 * coalesced frame, fighting the compositor. */
                SetWindowPos(child, NULL, 0, 0,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
            host->was_resized(host);
            host->base.release(&host->base);
        }
        br->base.release(&br->base);
    }
    /* freed by heap_task_release, not here */
}

static void resize_browser_to_client(void) {
    /* Skip until the browser exists: WM_SIZE fires during the host window's
     * creation on the main thread, before cef_initialize brings the UI task
     * runner up — posting then would just warn "No task runner for threadId 0". */
    bool have_browser;
    EnterCriticalSection(&g_browser_lock);
    have_browser = (g_browser != NULL);
    LeaveCriticalSection(&g_browser_lock);
    if (!have_browser) return;

    /* One in-flight resize task at a time (see g_resize_pending above). */
    if (InterlockedCompareExchange(&g_resize_pending, 1, 0) != 0) return;

    ne_resize_task *t = (ne_resize_task *)calloc(1, sizeof(*t));
    if (!t) { InterlockedExchange(&g_resize_pending, 0); return; }
    INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
    t->task.execute = resize_task_execute;
    if (!cef_post_task(TID_UI, &t->task)) {
        InterlockedExchange(&g_resize_pending, 0);
        t->task.base.release(&t->task.base);
    }
}

/* Visibility task: cef_browser_host_t::was_hidden must run on the UI thread.
 * Windowed CEF does NOT infer visibility from the native window state (and
 * this build disables Chromium's occlusion tracker), so the host tells the
 * browser explicitly on minimize/restore. That flips document.hidden in the
 * page, parks rAF, and lets the renderer drop to background-throttled timers
 * — the actual power win when minimized. */
typedef struct { cef_task_t task; refbase rb; int hidden; } ne_vis_task;

static void CEF_CALLBACK vis_task_execute(cef_task_t *self) {
    ne_vis_task *t = (ne_vis_task *)self;
    cef_browser_t *br = NULL;
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) { br = g_browser; br->base.add_ref(&br->base); }
    LeaveCriticalSection(&g_browser_lock);
    if (br) {
        cef_browser_host_t *host = br->get_host(br);
        if (host) {
            /* was_hidden alone does not flip page visibility for a WINDOWED
             * browser — Chromium tracks the child widget's WS_VISIBLE. Hide
             * the child HWND too: that drives document.hidden=true in the
             * page, parks rAF, and throttles timers for real. */
            HWND child = host->get_window_handle(host);
            if (child) ShowWindow(child, t->hidden ? SW_HIDE : SW_SHOW);
            host->was_hidden(host, t->hidden);
            host->base.release(&host->base);
        }
        /* The embedded chrome-runtime widget does NOT inherit the host's
         * minimize into page visibility (document.hidden stays false), so
         * push the state to the UI over the bus — app.js parks its rAF
         * loops and stretches the now-poll on {type:"vis",hidden:true}. */
        {
            cef_frame_t *frame = br->get_main_frame(br);
            if (frame) {
                emit_to_frame(frame,
                    t->hidden ? "{\"type\":\"vis\",\"hidden\":true}"
                              : "{\"type\":\"vis\",\"hidden\":false}");
                frame->base.release(&frame->base);
            }
        }
        br->base.release(&br->base);
    }
}
static void browser_set_hidden(int hidden) {
    static int last = -1;               /* main thread only (WM_SIZE)      */
    ne_vis_task *t;
    if (hidden == last) return;
    last = hidden;
    t = (ne_vis_task *)calloc(1, sizeof(*t));
    if (!t) return;
    INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
    t->task.execute = vis_task_execute;
    t->hidden = hidden;
    if (!cef_post_task(TID_UI, &t->task)) t->task.base.release(&t->task.base);
}

/* Close task: cef_browser_host_t::close_browser must run on the UI thread. */
typedef struct { cef_task_t task; refbase rb; } ne_close_task;

static void CEF_CALLBACK close_task_execute(cef_task_t *self) {
    cef_browser_t *br = NULL;
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) { br = g_browser; br->base.add_ref(&br->base); }
    LeaveCriticalSection(&g_browser_lock);
    if (br) {
        cef_browser_host_t *host = br->get_host(br);
        if (host) {
            host->close_browser(host, 0);
            host->base.release(&host->base);
        }
        br->base.release(&br->base);
    }
    (void)self;   /* freed by heap_task_release, not here */
}

/* ---- taskbar thumbnail toolbar (prev / play-pause / next) + progress ----
 * Media-player taskbar integration like WMP/MediaMonkey: hover the taskbar
 * button for transport controls; the button itself shows track progress
 * (green while playing, yellow while paused). Icons are drawn with GDI so
 * no .ico resources are needed. */
static HICON thb_make_icon(int kind) {  /* 0 prev, 1 play, 2 pause, 3 next */
    const int S = 24;
    HDC     sdc = GetDC(NULL);
    HDC     cdc = CreateCompatibleDC(sdc);
    HDC     mdc = CreateCompatibleDC(sdc);
    HBITMAP cbm = CreateCompatibleBitmap(sdc, S, S);
    HBITMAP mbm = CreateBitmap(S, S, 1, 1, NULL);
    HBITMAP oc, om;
    RECT    all = {0, 0, S, S};
    HICON   icon = NULL;
    if (!cdc || !mdc || !cbm || !mbm) goto done;
    oc = (HBITMAP)SelectObject(cdc, cbm);
    om = (HBITMAP)SelectObject(mdc, mbm);
    /* color: black bg, white glyph — mask: white(1)=transparent, black(0)=glyph */
    FillRect(cdc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    FillRect(mdc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    {
        HDC   dcs[2] = { cdc, mdc };
        int   d;
        for (d = 0; d < 2; d++) {
            HDC dc = dcs[d];
            HBRUSH br = (HBRUSH)GetStockObject(d == 0 ? WHITE_BRUSH : BLACK_BRUSH);
            HPEN   pn = (HPEN)GetStockObject(d == 0 ? WHITE_PEN : BLACK_PEN);
            HBRUSH ob = (HBRUSH)SelectObject(dc, br);
            HPEN   op = (HPEN)SelectObject(dc, pn);
            switch (kind) {
                case 1: {  /* play: right-pointing triangle */
                    POINT p[3] = { {7,4}, {7,20}, {20,12} };
                    Polygon(dc, p, 3);
                    break;
                }
                case 2: {  /* pause: two bars */
                    RECT a = {6,4,10,20}, b = {14,4,18,20};
                    FillRect(dc, &a, br); FillRect(dc, &b, br);
                    break;
                }
                case 0: {  /* prev: bar + left triangle */
                    RECT bar = {5,4,8,20};
                    POINT p[3] = { {19,4}, {19,20}, {9,12} };
                    FillRect(dc, &bar, br);
                    Polygon(dc, p, 3);
                    break;
                }
                default: { /* next: right triangle + bar */
                    RECT bar = {16,4,19,20};
                    POINT p[3] = { {5,4}, {5,20}, {15,12} };
                    FillRect(dc, &bar, br);
                    Polygon(dc, p, 3);
                    break;
                }
            }
            SelectObject(dc, ob); SelectObject(dc, op);
        }
    }
    SelectObject(cdc, oc); SelectObject(mdc, om);
    {
        ICONINFO ii;
        ii.fIcon = TRUE; ii.xHotspot = 0; ii.yHotspot = 0;
        ii.hbmMask = mbm; ii.hbmColor = cbm;
        icon = CreateIconIndirect(&ii);
    }
done:
    if (cbm) DeleteObject(cbm);
    if (mbm) DeleteObject(mbm);
    if (cdc) DeleteDC(cdc);
    if (mdc) DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    return icon;
}
static void thb_fill(THUMBBUTTON *b, UINT id, HICON ico, const wchar_t *tip) {
    memset(b, 0, sizeof(*b));
    b->dwMask  = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b->iId     = id;
    b->hIcon   = ico;
    b->dwFlags = THBF_ENABLED;
    wcscpy_s(b->szTip, 260, tip);
}
static void taskbar_buttons_init(HWND hwnd, BOOL playing) {
    THUMBBUTTON b[3];
    if (!g_taskbar) {
        if (FAILED(CoCreateInstance(&mn_CLSID_TaskbarList, NULL,
                                    CLSCTX_INPROC_SERVER,
                                    &mn_IID_ITaskbarList3,
                                    (void **)&g_taskbar)) || !g_taskbar)
            return;
        if (FAILED(g_taskbar->lpVtbl->HrInit(g_taskbar))) {
            g_taskbar->lpVtbl->Release(g_taskbar);
            g_taskbar = NULL;
            return;
        }
    }
    if (!g_thb_ico[0]) {
        g_thb_ico[0] = thb_make_icon(0);
        g_thb_ico[1] = thb_make_icon(1);
        g_thb_ico[2] = thb_make_icon(2);
        g_thb_ico[3] = thb_make_icon(3);
    }
    thb_fill(&b[0], MN_THB_PREV, g_thb_ico[0], L"Previous");
    thb_fill(&b[1], MN_THB_PLAY, g_thb_ico[playing ? 2 : 1],
             playing ? L"Pause" : L"Play");
    thb_fill(&b[2], MN_THB_NEXT, g_thb_ico[3], L"Next");
    if (!g_thb_added) {
        if (SUCCEEDED(g_taskbar->lpVtbl->ThumbBarAddButtons(g_taskbar, hwnd, 3, b)))
            g_thb_added = TRUE;
    } else {
        g_taskbar->lpVtbl->ThumbBarUpdateButtons(g_taskbar, hwnd, 3, b);
    }
}
/* Called from the 100ms app tick: keeps the play/pause glyph and the
 * taskbar-button progress bar in sync with playback. Cheap: COM calls only
 * fire on state change or every 500ms while playing. */
static void taskbar_progress_tick(HWND hwnd) {
    static int       last_playing = -1;
    static int       last_have    = -1;
    static ULONGLONG last_prog    = 0;
    mn_now now;
    int playing, have;
    if (!g_taskbar || !g_app) return;
    /* Lite snapshot: this tick reads only playing/track_id/duration/position,
     * never the art path — so skip the per-call art fopen/stat at 10 Hz. */
    mn_app_now_lite(g_app, &now);
    playing = now.playing ? 1 : 0;
    have    = (now.track_id > 0 && now.duration_ms > 0) ? 1 : 0;
    if (playing != last_playing || have != last_have) {
        last_playing = playing; last_have = have;
        taskbar_buttons_init(hwnd, playing ? TRUE : FALSE);
        g_taskbar->lpVtbl->SetProgressState(g_taskbar, hwnd,
            !have ? TBPF_NOPROGRESS : (playing ? TBPF_NORMAL : TBPF_PAUSED));
        last_prog = 0;   /* force an immediate value refresh below */
    }
    if (have) {
        ULONGLONG t = GetTickCount64();
        if (t - last_prog >= 500) {
            last_prog = t;
            g_taskbar->lpVtbl->SetProgressValue(g_taskbar, hwnd,
                (ULONGLONG)(now.position_ms > 0 ? now.position_ms : 0),
                (ULONGLONG)now.duration_ms);
        }
    }
}
/* Open a dropped / command-line path: a directory is added to the library; a
 * single file gets its parent directory added (so the scanner indexes it).
 * Best-effort, non-fatal. UTF-16 in so Unicode paths survive. */
static void mn_open_path_utf16(const wchar_t *wpath) {
    DWORD attr;
    wchar_t dir[1024];
    char u8[2048];
    int n;
    if (!g_app || !wpath || !wpath[0]) return;
    attr = GetFileAttributesW(wpath);
    wcsncpy_s(dir, 1024, wpath, _TRUNCATE);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        /* strip to parent directory for a single file */
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash) slash = wcsrchr(dir, L'/');
        if (slash) *slash = L'\0';
    }
    n = WideCharToMultiByte(CP_UTF8, 0, dir, -1, u8, sizeof(u8), NULL, NULL);
    if (n > 0) mn_app_add_folder(g_app, u8);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* Shell broadcast: the taskbar button exists (fires on window creation
     * AND whenever Explorer restarts) — (re)attach the thumbnail toolbar. */
    if (g_tbc_msg && msg == g_tbc_msg) {
        if (g_taskbar) {           /* Explorer restarted: stale proxy */
            g_taskbar->lpVtbl->Release(g_taskbar);
            g_taskbar = NULL;
            g_thb_added = FALSE;
        }
        taskbar_buttons_init(hwnd, FALSE);
        return 0;
    }
    switch (msg) {
        case WM_ERASEBKGND:
            /* The CEF child (WS_VISIBLE, clipped) fully covers the client area
             * and paints itself — the host must NEVER erase, or every resize
             * frame blits the background brush first (flash/tear band). */
            return 1;
        case WM_ENTERSIZEMOVE:
            /* Interactive drag-resize runs a MODAL loop inside DefWindowProc;
             * our main GetMessage loop (and thus WM_TIMER -> mn_app_tick) is
             * suspended for the whole drag. Keep playback ticking with a real
             * timer, and mark the drag so resize coalesces harder. */
            g_in_sizemove = 1;
            SetTimer(hwnd, NE_SIZEMOVE_TICK_ID, 16, NULL);
            return 0;
        case WM_EXITSIZEMOVE:
            g_in_sizemove = 0;
            KillTimer(hwnd, NE_SIZEMOVE_TICK_ID);
            resize_browser_to_client();   /* one authoritative final resize */
            return 0;
        case WM_SIZE:
            /* Minimized: the window stays a normal TASKBAR button (never a
             * tray icon — see the taskbar block above). Tell CEF the browser
             * is hidden so the renderer throttles to ~1 Hz timers + parked
             * rAF (playback continues natively in this process); restore
             * flips it back and app.js's visibilitychange resumes instantly. */
            if (wp == SIZE_MINIMIZED) { browser_set_hidden(1); return 0; }
            browser_set_hidden(0);
            resize_browser_to_client();
            return 0;
        case WM_HOTKEY:
            if (g_app) {
                switch (wp) {
                    case MN_HOTKEY_PLAYPAUSE: mn_app_toggle_pause(g_app); break;
                    case MN_HOTKEY_NEXT:      mn_app_next(g_app);         break;
                    case MN_HOTKEY_PREV:      mn_app_prev(g_app);         break;
                    case MN_HOTKEY_STOP:      mn_app_stop(g_app);         break;
                }
            }
            return 0;
        case WM_DROPFILES: {
            HDROP hd = (HDROP)wp;
            UINT count = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
            UINT i;
            for (i = 0; i < count; i++) {
                wchar_t path[1024];
                if (DragQueryFileW(hd, i, path, 1024)) mn_open_path_utf16(path);
            }
            DragFinish(hd);
            return 0;
        }
        case WM_COMMAND:
            /* Taskbar thumbnail-toolbar clicks arrive as THBN_CLICKED. */
            if (HIWORD(wp) == THBN_CLICKED && g_app) {
                switch (LOWORD(wp)) {
                    case MN_THB_PLAY: mn_app_toggle_pause(g_app); return 0;
                    case MN_THB_NEXT: mn_app_next(g_app);         return 0;
                    case MN_THB_PREV: mn_app_prev(g_app);         return 0;
                }
            }
            return 0;
        case WM_TIMER:
            if (wp == NE_TICK_TIMER_ID && g_app) {
                mn_app_tick(g_app);
                taskbar_progress_tick(hwnd);
                book_progress_tick();   /* ~5s cadence internally */
            }
            else if (wp == NE_SIZEMOVE_TICK_ID) {
                /* Fires from inside the modal resize loop (where the normal
                 * NE_TICK_TIMER_ID does not pump) so playback keeps advancing
                 * while the user drags the window edge. */
                if (g_app) mn_app_tick(g_app);
            }
            else if (wp == NE_DEPTHHEAL_TIMER_ID) {
                /* Directory sweeps + cap trims walk thousands of files —
                 * NOT on this thread (it drives mn_app_tick / gapless
                 * auto-advance; the walk stalled track boundaries). Single-
                 * flight background thread; skipped if one is still running. */
                heal_tick_start();
            }
            else if (wp == NE_ARTHEAL_TIMER_ID) {
                /* One-shot: low-priority post-launch art pass. Run the FULL
                 * integrity verify (mn_app_refresh_art skip_existing + webart
                 * mirror + served-file verification/miss count), not just the
                 * mirror-only selfheal — previously the true library-wide
                 * verify first ran on the 5-minute maintenance tick, so a gap
                 * (torn webart, cleared cache) survived the whole first view.
                 * art_integrity_kick is single-flight and supersedes
                 * artscan_selfheal_start (same refresh, richer callback).
                 * BOUNDED first pass: on a large COLD library an unbounded
                 * verify does real extraction work that contends with the
                 * initial scan's disk I/O (thread priority does not bound
                 * I/O). Cap it so first-view surfaces fill fast; the 5-min
                 * NE_DEPTHHEAL tick runs limit=0 and finishes the tail. */
                KillTimer(hwnd, NE_ARTHEAL_TIMER_ID);
                if (g_app) art_integrity_kick(/*limit=*/1024);
                /* depth maps: batch pre-generation only when opted in */
                {
                    mn_settings st;
                    mn_app_get_settings(g_app, &st);
                    InterlockedExchange(&g_depth_batch, st.depth_batch ? 1 : 0);
                    if (st.depth_batch) depth_selfheal_sweep();
                    /* live folder monitoring (persisted preference) */
                    InterlockedExchange(&g_watch_folders, st.watch_folders ? 1 : 0);
                }
                register_persisted_roots();  /* roots known before watching */
                sync_audiobook_roots();      /* category split active early */
                books_migrate_once();        /* legacy book_resume.txt -> DB */
                hash_backfill_kick();        /* content fingerprints (v7)   */
                folder_watch_start();
                SetTimer(hwnd, NE_DEPTHHEAL_TIMER_ID, NE_DEPTHHEAL_MS, NULL);
            }
            return 0;
        case WM_CLOSE: {
            /* Second WM_CLOSE (sent by CEF's default do_close handling after we
             * requested the browser close): the browser is tearing down, so let
             * the window die for real now. */
            if (InterlockedCompareExchange(&g_closing, 0, 0)) {
                DestroyWindow(hwnd);
                return 0;
            }
            /* First WM_CLOSE (user clicked X): ask CEF (on its UI thread) to
             * close the browser; CEF then calls do_close which sets g_closing
             * and bounces WM_CLOSE back here to actually destroy. */
            bool have_browser;
            EnterCriticalSection(&g_browser_lock);
            have_browser = (g_browser != NULL);
            LeaveCriticalSection(&g_browser_lock);
            if (have_browser) {
                ne_close_task *t = (ne_close_task *)calloc(1, sizeof(*t));
                if (t) {
                    INIT_HEAP_TASK(&t->task, &t->rb, cef_task_t);
                    t->task.execute = close_task_execute;
                    if (!cef_post_task(TID_UI, &t->task))
                        t->task.base.release(&t->task.base);
                }
                return 0;   /* do not destroy yet */
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_APP + 1:
            /* Browser fully closed (posted from on_before_close on the UI
             * thread). Tear the host window down and end the message loop. */
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, NE_TICK_TIMER_ID);
            KillTimer(hwnd, NE_ARTHEAL_TIMER_ID);
            KillTimer(hwnd, NE_DEPTHHEAL_TIMER_ID);
            UnregisterHotKey(hwnd, MN_HOTKEY_PLAYPAUSE);
            UnregisterHotKey(hwnd, MN_HOTKEY_NEXT);
            UnregisterHotKey(hwnd, MN_HOTKEY_PREV);
            UnregisterHotKey(hwnd, MN_HOTKEY_STOP);
            if (g_taskbar) {
                g_taskbar->lpVtbl->Release(g_taskbar);
                g_taskbar = NULL;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ------------------------------------------------------------------------- */
/* webroot setup: mkdir webroot, webroot\fonts; copy the 5 ttf. The legacy    */
/* webroot\art mirror is GONE (one-store: art-cache is served directly) —     */
/* its path is still computed so the one-time v2 migration can reclaim it.    */
/* ------------------------------------------------------------------------- */

static void path_dirname(char *p) {
    char *s1 = strrchr(p, '\\');
    char *s2 = strrchr(p, '/');
    char *s  = (s1 > s2) ? s1 : s2;
    if (s) *s = 0;
}

/* Legacy pre-v1 webroot-ROOT art file: "art" + 16 hex + ".hires.png" or
 * ".depth.png" (the era before the art/ subdir; ~51 strays observed). */
static bool webroot_legacy_art_name(const char *n) {
    int i;
    if (strlen(n) != 29) return false;   /* 3 + 16 + 10 */
    if (_strnicmp(n, "art", 3) != 0) return false;
    for (i = 3; i < 19; i++) {
        char c = n[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return _stricmp(n + 19, ".hires.png") == 0 ||
           _stricmp(n + 19, ".depth.png") == 0;
}

/* Sweep (delete_them=true) or just count the legacy art files sitting in the
 * webroot ROOT beside fonts/. Returns how many remain afterward — locked
 * files are skipped and retried next boot, same as the art-dir migration. */
static int webroot_root_legacy_sweep(bool delete_them) {
    char pattern[1700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int remain = 0;
    if (!g_webroot[0]) return 0;
    snprintf(pattern, sizeof(pattern), "%s\\art*.png", g_webroot);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!webroot_legacy_art_name(fd.cFileName)) continue;
        if (delete_them) {
            char full[1700];
            snprintf(full, sizeof(full), "%s\\%s", g_webroot, fd.cFileName);
            if (DeleteFileA(full)) continue;          /* reclaimed */
        }
        remain++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return remain;
}

/* One-time v2 migration (marker-gated, zero flag-day): delete the legacy
 * webroot\art mirror (~GBs of duplicate PNGs) on a lowest-priority thread,
 * plus the ~51 pre-v1 art*.hires/.depth strays in the webroot ROOT itself.
 * The marker (<data>\art.v2) is written ONLY after the art directory is fully
 * gone — sharing violations (a second running instance) leave files behind,
 * no marker is written, and the next boot retries. Root strays are retried
 * independently of the marker (the kick re-checks them each boot). Rollback
 * stays trivial until the delete succeeds: the old build still finds its
 * webart. */
static DWORD WINAPI webart_migrate_thread(LPVOID param) {
    (void)param;
    worker_enter();
    if (g_webart[0]) {
        char pattern[1700];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", g_webart);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char full[1700];
                if (fd.cFileName[0] == '.' &&
                    (fd.cFileName[1] == '\0' ||
                     (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;   /* flat dir; anything else is not ours */
                snprintf(full, sizeof(full), "%s\\%s", g_webart, fd.cFileName);
                DeleteFileA(full);   /* locked files: skipped, retried next boot */
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (GetFileAttributesA(g_webart) == INVALID_FILE_ATTRIBUTES ||
            RemoveDirectoryA(g_webart)) {
            char marker[1400];
            FILE *f;
            snprintf(marker, sizeof(marker), "%s\\art.v2", g_data_dir);
            f = fopen(marker, "w");
            if (f) { fputs("one-store art migration complete\n", f); fclose(f); }
            fprintf(stderr, "[art-migrate] legacy webroot\\art reclaimed\n");
        } else {
            fprintf(stderr, "[art-migrate] webroot\\art still has locked "
                            "files; will retry next boot\n");
        }
    }
    /* pre-v1 strays in the webroot ROOT (beside fonts/): dead bytes no code
     * path references anymore — reclaim them too. Locked files remain and
     * are retried next boot (the kick re-detects them, marker or not). */
    {
        int remain = webroot_root_legacy_sweep(true);
        if (remain > 0)
            fprintf(stderr, "[art-migrate] %d legacy webroot-root art files "
                            "still locked; will retry next boot\n", remain);
        else
            fprintf(stderr, "[art-migrate] webroot-root legacy art strays "
                            "reclaimed\n");
    }
    worker_leave();
    return 0;
}
static void webart_migrate_kick(void) {
    char marker[1400];
    HANDLE h;
    bool have_marker, dir_present, strays;
    if (!g_data_dir[0] || !g_webart[0]) return;
    /* One-time v2b: RESET the persisted NONE ledger. Verdicts recorded before
     * the src_seen distinction existed conflated "source exists but failed to
     * decode" with "genuinely artless" (the false-NONE bug: a corrupt
     * Cover.jpg permanently muted an album with three valid covers on disk).
     * The rewritten verify re-derives honest verdicts on the next heal pass. */
    {
        char m2[1400];
        snprintf(m2, sizeof(m2), "%s\\art.v2b", g_data_dir);
        if (GetFileAttributesA(m2) == INVALID_FILE_ATTRIBUTES) {
            FILE *f;
            art_none_clear_all();
            f = fopen(m2, "w");
            if (f) {
                fputs("NONE ledger reset: decode-fail is no longer a "
                      "terminal verdict\n", f);
                fclose(f);
            }
        }
    }
    snprintf(marker, sizeof(marker), "%s\\art.v2", g_data_dir);
    have_marker = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
    dir_present = GetFileAttributesA(g_webart) != INVALID_FILE_ATTRIBUTES;
    strays      = webroot_root_legacy_sweep(false) > 0;
    if (have_marker && !dir_present && !strays) return;   /* fully migrated */
    if (!dir_present && !strays) {
        /* nothing to reclaim (fresh install): just record v2 */
        FILE *f = fopen(marker, "w");
        if (f) { fputs("one-store art (fresh install)\n", f); fclose(f); }
        return;
    }
    h = CreateThread(NULL, 0, webart_migrate_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_LOWEST); CloseHandle(h); }
}

static void setup_webroot(void) {
    char parent[1300];
    snprintf(parent, sizeof(parent), "%s", g_art_dir);
    path_dirname(parent);
    if (parent[0] == 0) snprintf(parent, sizeof(parent), ".");

    snprintf(g_webroot, sizeof(g_webroot), "%s\\webroot", parent);
    snprintf(g_webart,  sizeof(g_webart),  "%s\\art",     g_webroot);
    /* Data dir (parent of the art cache, i.e. %APPDATA%\Monatomic) — the
     * depth worker resolves its model under <data_dir>\ai-models\. */
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", parent);

    char fonts_dst[1500];
    snprintf(fonts_dst, sizeof(fonts_dst), "%s\\fonts", g_webroot);

    CreateDirectoryA(g_webroot,  NULL);
    CreateDirectoryA(fonts_dst,  NULL);
    CreateDirectoryA(g_art_dir,  NULL);

    char fonts_src[1500];
    {
        char base[1300];
        snprintf(base, sizeof(base), "%s", g_ui_dir);
        path_dirname(base);
        snprintf(fonts_src, sizeof(fonts_src), "%s\\assets\\fonts", base);
    }

    static const char *ttf[] = {
        "manrope_regular.ttf",  "manrope_medium.ttf",   "manrope_semibold.ttf",
        "manrope_bold.ttf",     "manrope_extrabold.ttf",
    };
    for (int i = 0; i < (int)(sizeof(ttf) / sizeof(ttf[0])); i++) {
        char src[1700], dst[1700];
        snprintf(src, sizeof(src), "%s\\%s", fonts_src, ttf[i]);
        snprintf(dst, sizeof(dst), "%s\\%s", fonts_dst, ttf[i]);
        if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES)
            CopyFileA(src, dst, FALSE);
    }
}

/* ------------------------------------------------------------------------- */
/* Public entry point — same signature as the WebView2 host (drop-in).        */
/* ------------------------------------------------------------------------- */

int webview_run(mn_app *app, const char *ui_dir, const char *art_dir) {
    /* CEF 144 ships a versioned C API. Before ANY other CEF call (in EVERY
     * process, including the sub-processes routed through cef_execute_process),
     * configure the API version via cef_api_hash(). Without this first call,
     * cef_api_version() stays -1 and every C-to-C++ struct wrapper aborts with
     * "CefApp_0_CToCpp called with invalid version -1". CEF_API_VERSION is
     * pinned to the vendored DLL (14400) by the build's /DCEF_API_VERSION. */
    cef_api_hash(CEF_API_VERSION, 0);

    g_app = app;
    snprintf(g_ui_dir,  sizeof(g_ui_dir),  "%s", ui_dir  ? ui_dir  : "./ui");
    snprintf(g_art_dir, sizeof(g_art_dir), "%s", art_dir ? art_dir : "./art-cache");

    /* Low-power marker rides the ENVIRONMENT so every CEF sub-process and
     * the ONNX modules (depth/stems thread caps) see the same decision —
     * the command-line hook runs per-process where g_app may be NULL. */
    if (app) {
        mn_settings st0;
        mn_app_get_settings(app, &st0);
        if (st0.low_power) _putenv("MN_LOWPOWER=1");
    }

    HINSTANCE hinst = GetModuleHandleW(NULL);

    /* Resolve our own exe path (used as the browser-subprocess path so CEF
     * re-launches this same binary for its sub-processes — no helper exe). */
    GetModuleFileNameA(NULL, g_exe_path, sizeof(g_exe_path));
    snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", g_exe_path);
    path_dirname(g_exe_dir);

    /* Wire up all handler vtables before any CEF entry that may invoke them. */
    init_handlers();

    /* CEF main args (Windows: just the module instance). */
    cef_main_args_t main_args;
    memset(&main_args, 0, sizeof(main_args));
    main_args.instance = hinst;

    /* Sub-process routing: for the browser process this returns -1 and we
     * continue; for a recognized sub-process (renderer, gpu, utility, ...) it
     * blocks until exit and returns the exit code — return it immediately. */
    int exec_code = cef_execute_process(&main_args, &g_app_impl.app, NULL);
    if (exec_code >= 0) {
        return exec_code;
    }

    /* --- browser process only from here down --- */

    setup_webroot();
    InitializeCriticalSection(&g_sync_cs);
    sync_state_load();      /* persisted sync host/auto/last (needs g_data_dir) */
    depth_worker_start();   /* lazy model load happens on the worker itself */
    artenc_pool_start();    /* background art extraction (off the UI thread) */
    webart_migrate_kick();  /* one-time: reclaim the legacy webroot\art mirror */

    /* CEF settings. */
    cef_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.size = sizeof(settings);
    settings.no_sandbox = 1;
    /* CEF runs its own dedicated UI thread; the main thread runs a standard Win32
     * message loop for the host window. This is the most robust model for a
     * Win32-hosted (WS_CHILD) browser: on_context_initialized and all
     * cef_browser_host_t calls land on the real Chrome_UIThread, avoiding the
     * "Must be called on Chrome_UIThread" fatal. We must NOT call
     * cef_run_message_loop()/cef_quit_message_loop() in this mode. */
    settings.multi_threaded_message_loop = 1;
    settings.log_severity = LOGSEVERITY_WARNING;
    /* Opaque global default so no compositing surface starts transparent. */
    settings.background_color = 0xFF060608u;

    /* Same exe hosts the sub-processes. */
    cefstr_from_utf8(&settings.browser_subprocess_path, g_exe_path);

    /* Resources/locales sit under vendor\cef\Release + Resources at build time
     * and are copied beside the exe; leave the paths default so CEF finds them
     * next to the exe. A cache path keeps localStorage across runs.
     *
     * PER-INSTANCE CACHE SLOTS: CEF hard-locks its cache directory. When two
     * instances shared one cef_cache, the second lost the lock race and fell
     * into a bare chrome-bootstrap window (the "random Google page" bug).
     * Each instance now claims the lowest free slot via a named mutex held
     * for the process lifetime: instance 1 -> cef_cache (settings persist as
     * before), instance 2 -> cef_cache_2, ... Instances are fully
     * independent; a second launch is a real second player. */
    {
        char cbase[1400];
        char cache[1400];
        int  slot;
        /* Portable: beside the exe as always. Installed (read-only exe dir,
         * e.g. Program Files): %LOCALAPPDATA%\Monatomic. */
        mn_cef_cache_base(cbase, sizeof(cbase));
        for (slot = 1; slot <= 8; slot++) {
            char mname[64];
            HANDLE hm;
            snprintf(mname, sizeof(mname), "Local\\MonatomicCefSlot_%d", slot);
            hm = CreateMutexA(NULL, TRUE, mname);
            if (hm && GetLastError() != ERROR_ALREADY_EXISTS) break;  /* ours */
            if (hm) CloseHandle(hm);
            /* slot taken by a live instance — try the next one */
        }
        if (slot <= 1 || slot > 8)
            snprintf(cache, sizeof(cache), "%s\\cef_cache", cbase);
        else
            snprintf(cache, sizeof(cache), "%s\\cef_cache_%d", cbase, slot);
        CreateDirectoryA(cache, NULL);
        cefstr_from_utf8(&settings.cache_path, cache);
        cefstr_from_utf8(&settings.root_cache_path, cache);
    }

    /* Register the host window class + create the host window NOW, on the MAIN
     * thread, before cef_initialize. Its wnd_proc therefore runs on the main
     * thread, where our GetMessage loop lives. on_context_initialized (on CEF's
     * own UI thread) then posts create_browser to TID_UI, which parents the
     * browser into this already-created, already-shown, correctly-sized HWND —
     * so create_browser reads a NON-zero client rect. */
    InitializeCriticalSection(&g_browser_lock);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    /* NO CS_HREDRAW/CS_VREDRAW: those force a full host client-area invalidate
     * on every WM_SIZE during a drag — a per-frame erase+repaint storm behind
     * the CEF child that covers the whole area anyway. The CEF child paints
     * itself; the host has no own content to redraw on size change. */
    wc.style         = 0;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    /* NULL background brush: with a WM_ERASEBKGND handler returning 1 (see
     * wnd_proc) the host never erases, so the resize-time flash/tear band is
     * gone. The child fully covers the client area. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"MonatomicCefHost";
    RegisterClassExW(&wc);
    g_host_hinst = hinst;

    /* WS_CLIPCHILDREN on the PARENT: without it, the host may paint/erase into
     * the CEF child's rectangle during resize (compounds the erase storm).
     * The child already has WS_CLIPCHILDREN|WS_CLIPSIBLINGS. */
    g_host_hwnd = CreateWindowExW(
        0, L"MonatomicCefHost", L"Monatomic Audio Player",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900,
        NULL, NULL, hinst, NULL);
    if (g_host_hwnd) {
        /* Make the OS title bar match the app: DWM dark mode + a black caption
         * bar and matching border (Windows 10 2004+ / Windows 11). Loaded
         * dynamically so it degrades on older builds. */
        {
            HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            if (dwm) {
                typedef HRESULT (WINAPI *SetAttr)(HWND, DWORD, LPCVOID, DWORD);
                SetAttr set = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
                if (set) {
                    BOOL dark = TRUE;
                    /* DWMWA_USE_IMMERSIVE_DARK_MODE = 20 */
                    set(g_host_hwnd, 20, &dark, sizeof(dark));
                    /* DWMWA_CAPTION_COLOR = 35 ; DWMWA_BORDER_COLOR = 34
                     * COLORREF is 0x00BBGGRR — near-black to match --bg (#000). */
                    COLORREF cap = RGB(6, 6, 8);
                    set(g_host_hwnd, 35, &cap, sizeof(cap));
                    set(g_host_hwnd, 34, &cap, sizeof(cap));
                    /* DWMWA_TEXT_COLOR = 36 — light caption text */
                    COLORREF txt = RGB(210, 210, 214);
                    set(g_host_hwnd, 36, &txt, sizeof(txt));
                }
                /* keep dwmapi resident for the app lifetime */
            }
        }
        ShowWindow(g_host_hwnd, SW_SHOW);
        UpdateWindow(g_host_hwnd);

        /* System integration: global media-key hotkeys, drag-drop, tray. */
        RegisterHotKey(g_host_hwnd, MN_HOTKEY_PLAYPAUSE, 0, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(g_host_hwnd, MN_HOTKEY_NEXT,      0, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(g_host_hwnd, MN_HOTKEY_PREV,      0, VK_MEDIA_PREV_TRACK);
        RegisterHotKey(g_host_hwnd, MN_HOTKEY_STOP,      0, VK_MEDIA_STOP);
        DragAcceptFiles(g_host_hwnd, TRUE);
        /* taskbar thumbnail toolbar: COM + the shell's "button created"
         * broadcast; taskbar_buttons_init runs when that message arrives. */
        (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        g_tbc_msg = RegisterWindowMessageW(L"TaskbarButtonCreated");
        /* elevated processes: let the shell's broadcast through UIPI */
        ChangeWindowMessageFilterEx(g_host_hwnd, g_tbc_msg, MSGFLT_ALLOW, NULL);

        /* Command-line / "Open with" / double-click: import EVERY non-flag
         * path argument (was first-only, so multi-select Open-with dropped
         * all but one file). UTF-16 so Unicode filenames survive. */
        {
            int wargc = 0;
            LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
            if (wargv) {
                int ai;
                for (ai = 1; ai < wargc; ai++) {
                    if (wargv[ai][0] == L'-' || wargv[ai][0] == L'/') continue;
                    if (GetFileAttributesW(wargv[ai]) == INVALID_FILE_ATTRIBUTES) continue;
                    mn_open_path_utf16(wargv[ai]);
                }
                LocalFree(wargv);
            }
        }
    }

    if (!cef_initialize(&main_args, &settings, &g_app_impl.app, NULL)) {
        cef_string_clear(&settings.browser_subprocess_path);
        cef_string_clear(&settings.cache_path);
        cef_string_clear(&settings.root_cache_path);
        return cef_get_exit_code();
    }

    /* Load the persisted library ROOTS + per-kind partition SYNCHRONOUSLY here,
     * BEFORE the message loop starts and before the CEF UI thread can round-trip
     * its first {cmd:"albums"} request. Previously these ran only from the +2s
     * NE_ARTHEAL_TIMER, so the FIRST album query built its cache with kroot_len==0
     * → mn_spec_apply_category emitted NO exclusion → audiobook-root content leaked
     * into the MUSIC album grid until a later gesture. Both calls are cheap,
     * in-memory (folder_kinds.txt read → mn_app_set_kind_roots) and run on this
     * main thread pre-loop, so kroot is populated before any query can fire. */
    if (g_app) {
        register_persisted_roots();   /* rescan roots known before watching */
        sync_audiobook_roots();       /* per-kind partition active on first query */
    }

    /* Drive the app tick from the host window (main-thread timer). */
    if (g_host_hwnd) {
        SetTimer(g_host_hwnd, NE_TICK_TIMER_ID, NE_TICK_MS, NULL);
        SetTimer(g_host_hwnd, NE_ARTHEAL_TIMER_ID, NE_ARTHEAL_MS, NULL);
    }

    /* Run the standard Win32 message loop on the main thread. CEF drives the
     * browser on its own UI thread (multi_threaded_message_loop=1); we only pump
     * the host window here. The loop ends when WM_DESTROY posts WM_QUIT. */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* Teardown. The host window was already destroyed on WM_DESTROY (which posted
     * WM_QUIT to break the loop above); just drop the stale handle. */
    g_host_hwnd = NULL;
    EnterCriticalSection(&g_browser_lock);
    if (g_browser) {
        g_browser->base.release(&g_browser->base);
        g_browser = NULL;
    }
    LeaveCriticalSection(&g_browser_lock);

    /* DRAIN the fire-and-forget workers BEFORE tearing anything down: an
     * in-flight art self-heal / sync / cache op would otherwise race
     * cef_shutdown + DeleteCriticalSection here and mn_app_destroy in main
     * — the classic quit-during-background-activity crash. The app-side
     * loops poll app->shutting_down, so flag it first for a fast drain. */
    if (g_app) mn_app_request_shutdown(g_app);
    workers_drain();

    cef_shutdown();
    artenc_pool_stop();     /* join the art-encode pool                    */
    depth_worker_stop();    /* join the depth thread + release its session */
    DeleteCriticalSection(&g_browser_lock);
    DeleteCriticalSection(&g_sync_cs);

    cef_string_clear(&settings.browser_subprocess_path);
    cef_string_clear(&settings.cache_path);
    cef_string_clear(&settings.root_cache_path);

    return 0;
}
