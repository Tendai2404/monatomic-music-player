/*
 * audio_engine.c — Monatomic hi-fi audio engine implementation.
 *
 * Layered over miniaudio's LOW-LEVEL device API (ma_device + ma_decoder). This
 * is the single translation unit that compiles the miniaudio implementation
 * (MINIAUDIO_IMPLEMENTATION is defined here and nowhere else). It also provides
 * a standalone decode shim (mn_decode_44100_stereo) and a waveform-peak
 * generator (mn_engine_waveform) built on ma_decoder.
 *
 * Design notes:
 *   - The engine owns one ma_device opened in playback mode at the canonical
 *     44100 Hz / 2ch / f32 output format, plus, when a track is loaded, one
 *     ma_decoder configured to resample that track to the same format.
 *   - We deliberately use the low-level device + data-callback path (rather than
 *     the high-level ma_engine/ma_sound) so the stem mixer has an injection
 *     point: on each block the callback can hand the absolute frame range to
 *     mn_stems_mix() and, when neural stems are ready, output the band-split mix
 *     instead of the decoded source.
 *   - The device callback runs on miniaudio's audio thread. The decoder, the
 *     absolute frame cursor and the "at end" flag are shared with the control
 *     thread (which issues load/seek/play) and are guarded by a mutex held only
 *     briefly. Volume/gain and the stem-routing pointers are published as plain
 *     scalars/pointers read locklessly in the callback.
 *   - Transport state (STOPPED/PLAYING/PAUSED) is tracked explicitly. Pause is
 *     implemented by stopping the device (halting the callback) without moving
 *     the cursor; play/resume restarts it.
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_engine.h"
#include "stems.h"
#include "mf_decode.h"
#include "dsp.h"
#include "stretch.h"
#include "netstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
/* Open a decoder from a UTF-8 path. On Windows, ma_decoder_init_file() uses the
 * ANSI file API and CANNOT open paths with characters outside the system code
 * page (emoji, CJK, ¥, ⭐, etc.) — such tracks fail with "load failed" even
 * though the file exists. Convert UTF-8 -> UTF-16 and use the wide variant so
 * EVERY path opens regardless of its characters. */
static ma_result mn_decoder_init_path(const char *utf8, const ma_decoder_config *cfg,
                                      ma_decoder *out) {
    wchar_t wbuf[4096];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf,
                                (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (n <= 0) {
        /* fall back to the narrow path if conversion somehow fails */
        return ma_decoder_init_file(utf8, cfg, out);
    }
    return ma_decoder_init_file_w(wbuf, cfg, out);
}
#else
#define mn_decoder_init_path(utf8, cfg, out) ma_decoder_init_file((utf8), (cfg), (out))
#endif

/* ------------------------------------------------------------------------- */
/* Engine object                                                              */
/* ------------------------------------------------------------------------- */

/* Frames pulled from the decoder per callback chunk when servicing a request
 * larger than this (also the stem-mix block granularity). */
#define MN_ENGINE_CHUNK_FRAMES 2048u

struct mn_engine {
    ma_context    context;      /* owned backend context (device info + device)*/
    ma_bool32     context_ready;
    ma_device     device;       /* miniaudio output device (playback)          */
    ma_decoder    decoder;      /* current track decoder (f32/pipe_rate/2ch)   */
    ma_bool32     device_ready; /* device successfully initialized             */
    ma_bool32     has_track;    /* `decoder` holds a live, loaded track        */

    mn_play_state state;        /* explicit transport state                    */

    /* Shared with the audio callback; guarded by `lock` for the decoder + the
     * cursor + at_end. */
    ma_mutex      lock;
    ma_bool32     lock_ready;
    uint64_t      cursor;       /* absolute output-frame cursor (next to read) */
    uint64_t      length;       /* total output frames in the track, 0=unknown */
    ma_bool32     at_end;       /* decoder reached EOS and drained             */

    /* Output level, published locklessly (aligned scalar writes). */
    float         volume;       /* linear volume scalar, [0,1]                 */
    float         gain_linear;  /* dB gain converted to linear (>= 0)          */

    /* Stem routing, published locklessly. The callback snapshots these once per
     * block. `stems` is a borrowed pointer (not owned). */
    mn_stems     *stems;
    ma_bool32     stems_enabled;
    ma_bool32     stems_passthrough;

    /* Scratch buffer for the stem mix (interleaved stereo f32), sized to
     * MN_ENGINE_CHUNK_FRAMES. */
    float        *mix_buf;

    /* Pitch-preserving playback speed (audiobooks). speed==1.0 bypasses the
     * stretcher entirely (bit-perfect path untouched). Stereo-only: surround
     * passthrough always plays at 1x. stretch_src is the source scratch the
     * stretcher is fed from (chunk-sized). Cursor stays in SOURCE frames, so
     * position/duration/seek are speed-agnostic automatically. */
    mn_stretch   *stretch;
    float        *stretch_src;
    float         speed;            /* 0.5 .. 3.0, default 1.0 */

    /* One-shot length hint for the NEXT load (ms; 0 = none). When set, the
     * load computes length from it and SKIPS ma_decoder_get_length —
     * a whole-file scan on big VBR files that stalled track switches. */
    int64_t       length_hint_ms;

    /* Non-NULL while the loaded "track" is an HTTP stream (internet radio /
     * streamed podcast episode). Owned; closed by mn_unload. While set, the
     * decoder reads through netstream callbacks, stems/pipeline retunes are
     * refused (there is no file to re-open), and seeking is gated on the
     * stream being Range-seekable. */
    mn_netstream *net;
    /* Set for EVERY URL load — including the AAC/M4A path where Media
     * Foundation streams the URL itself and `net` stays NULL. Guards the
     * same invariants (no pipeline retune, no file re-open). */
    ma_bool32     is_url;

    /* Post-mix DSP chain (EQ / balance / limiter / master gain). Owned.
     * Created lazily on first device open, reconfigured on rate changes.
     * `dsp_enabled` gates the process() call locklessly; params are published
     * through the mn_dsp_* setters which are safe wrt process(). */
    mn_dsp       *dsp;
    ma_bool32     dsp_enabled;
    uint32_t      dsp_rate;         /* rate the dsp was configured for */
    mn_dsp_params dsp_params;       /* last-published snapshot (for reconfigure) */
    ma_bool32     dsp_params_valid;

    /* Exclusive (bit-perfect) WASAPI output. When set, the device opens in
     * ma_share_mode_exclusive; falls back to shared automatically if the
     * device refuses exclusive mode. */
    ma_bool32     want_exclusive;

    /* Audiophile output profile.
     *   hifi_native_bits: in exclusive mode, open the device at the source's
     *     NATIVE integer bit depth (16/24/32) instead of always f32, so a
     *     16-bit source stays a true 16-bit hardware stream (bit-perfect).
     *   rate_cap_hz: 0 = no cap; otherwise the pipeline/device rate is
     *     clamped to this — power-saving profile for audiobooks (e.g. 48000).
     *   bits_cap: 0 = no cap; otherwise exclusive-mode depth is clamped
     *     (e.g. 16) for spoken-word content. Both caps let the app run
     *     max-quality for music and scale down for audiobooks. */
    ma_bool32     hifi_native_bits;
    uint32_t      rate_cap_hz;
    uint32_t      bits_cap;
    ma_format     want_device_format;   /* resolved per load; f32 default */

    /* Spectrum analyzer: a mono capture window filled from the render path and
     * reduced to MN_SPECTRUM_BARS log-spaced magnitude bars on demand,
     * published locklessly for the UI (aligned float writes). */
    float         spec_capture[512];   /* latest mono window (power of two)   */
    uint32_t      spec_write;          /* running write cursor                */
    float         spec_bars[MN_SPECTRUM_BARS]; /* published magnitudes [0,1]   */

    /* Adaptive pipeline rate: the rate the decoder outputs at and the device
     * is opened at (its data callback therefore runs at this rate). Chosen
     * per track: the source's native rate (clamped to the hardware max) when
     * stems are OFF, pinned to MN_ENGINE_SAMPLE_RATE when stems are ON. The
     * cursor/length frame counters are in units of this rate. */
    uint32_t      pipe_rate;

    /* Pipeline channel count. Normally MN_ENGINE_CHANNELS (2). In the
     * MULTICHANNEL PASSTHROUGH path — a >2ch source with stems+DSP OFF and a
     * device that supports the channel count — this becomes the source's
     * channel count so surround content is delivered intact instead of
     * force-downmixed to stereo. Falls back to 2 for stereo sources, when
     * stems/DSP are active (both are stereo processors), or when the device
     * can't do the channel count. */
    uint32_t      pipe_channels;
    ma_bool32     downmixed;        /* true when a >2ch source was folded to 2 */

    /* REAL hardware-side output format, captured from the device's internal
     * (backend-native) descriptors after each (re)init. */
    uint32_t      hw_rate;
    uint32_t      hw_channels;
    uint32_t      hw_bits;

    /* Native capabilities of the default playback device (last good probe). */
    mn_audio_caps caps;
    ma_bool32     caps_valid;

    /* Explicit output-device selection. When use_selected is set, every
     * (re)init of the ma_device targets selected_id instead of the system
     * default; selected_index mirrors the enumeration index for the UI. */
    ma_device_id  selected_id;
    ma_bool32     use_selected;
    int           selected_index;   /* -1 == system default */

    /* Absolute path of the loaded track (needed to re-open the decoder when
     * the pipeline rate is retuned, e.g. stems toggled on mid-track). */
    char          track_path[1024];

    /* Cached source-format info captured at load time. */
    uint32_t      src_sample_rate;
    uint32_t      src_channels;
    uint32_t      src_bits;
    uint32_t      bitrate;
    char          format[MN_FORMAT_STR_CAP];
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

/* Clamp a float into [lo, hi]. */
static float mn_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*
 * Return a short uppercase codec/container label derived from the file
 * extension. Copies at most MN_FORMAT_STR_CAP-1 chars into `out`. `out` is
 * always NUL-terminated. Unknown extensions yield an empty string.
 */
static void mn_label_from_path(const char *path, char *out, size_t cap)
{
    const char *dot = NULL;
    const char *p;
    size_t i;

    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (path == NULL) {
        return;
    }

    /* Find the last '.' that is not part of a directory component. */
    for (p = path; *p != '\0'; ++p) {
        if (*p == '.') {
            dot = p;
        } else if (*p == '/' || *p == '\\') {
            dot = NULL; /* reset: separators after a dot invalidate it */
        }
    }
    if (dot == NULL || dot[1] == '\0') {
        return;
    }

    /* Copy the extension, uppercased, bounded by capacity. */
    for (i = 0, p = dot + 1; *p != '\0' && i + 1 < cap; ++p, ++i) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

/*
 * Map a ma_format to a nominal bit depth. Float formats report 32; the pipeline
 * output is float32 regardless, but for the *source* we report the container's
 * native depth where miniaudio exposes it.
 */
static uint32_t mn_bits_from_format(ma_format fmt)
{
    switch (fmt) {
        case ma_format_u8:  return 8u;
        case ma_format_s16: return 16u;
        case ma_format_s24: return 24u;
        case ma_format_s32: return 32u;
        case ma_format_f32: return 32u;
        default:            return 0u; /* unknown / lossy */
    }
}

/* Convert a frame count at the engine sample rate to milliseconds. */
static uint64_t mn_frames_to_ms(uint64_t frames, uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return 0;
    }
    return (frames * 1000ull) / (uint64_t)sample_rate;
}

/* Convert milliseconds to a frame index at the given sample rate. */
static uint64_t mn_ms_to_frames(uint64_t ms, uint32_t sample_rate)
{
    return (ms * (uint64_t)sample_rate) / 1000ull;
}

/*
 * Tear down the currently loaded track (if any) and reset per-track state.
 * The device remains intact. Must be called with the device stopped (so the
 * callback is not touching the decoder) or under `lock`.
 */
static void mn_unload(mn_engine *engine)
{
    if (engine->has_track) {
        ma_decoder_uninit(&engine->decoder);
        engine->has_track = MA_FALSE;
    }
    if (engine->net != NULL) {
        mn_netstream_close(engine->net);
        engine->net = NULL;
    }
    engine->is_url = MA_FALSE;
    engine->cursor          = 0;
    engine->length          = 0;
    engine->at_end          = MA_FALSE;
    engine->state           = MN_STATE_STOPPED;
    engine->src_sample_rate = 0;
    engine->src_channels    = 0;
    engine->src_bits        = 0;
    engine->bitrate         = 0;
    engine->format[0]       = '\0';
    engine->track_path[0]   = '\0';
}

/* ------------------------------------------------------------------------- */
/* Hardware capability detection + adaptive device format                     */
/* ------------------------------------------------------------------------- */

/* Standard rates advertised when a backend reports "all rates supported"
 * (nativeDataFormats[].sampleRate == 0). */
static const uint32_t mn_standard_rates[] = {
    44100u, 48000u, 88200u, 96000u, 176400u, 192000u
};

/* Append `rate` to caps->native_rates keeping the list unique + ascending. */
static void mn_caps_add_rate(mn_audio_caps *caps, uint32_t rate)
{
    int32_t i, j;

    if (rate == 0 || caps->native_rate_count >= MN_CAPS_MAX_RATES) {
        return;
    }
    for (i = 0; i < caps->native_rate_count; ++i) {
        if (caps->native_rates[i] == (int32_t)rate) {
            return; /* already listed */
        }
        if (caps->native_rates[i] > (int32_t)rate) {
            break; /* insertion point (list kept ascending) */
        }
    }
    for (j = caps->native_rate_count; j > i; --j) {
        caps->native_rates[j] = caps->native_rates[j - 1];
    }
    caps->native_rates[i] = (int32_t)rate;
    caps->native_rate_count++;
}

/*
 * Probe the DEFAULT playback device's native capabilities through the backend
 * (WASAPI: the shared-mode mix format plus exclusive-mode IsFormatSupported
 * probing) and refresh engine->caps. On success engine->caps_valid is set and
 * true is returned; on failure the previous snapshot (if any) is preserved.
 * Control-thread only.
 */
static bool mn_query_caps(mn_engine *engine)
{
    ma_device_info info;
    mn_audio_caps  caps;
    uint32_t       i;
    int32_t        shared_bits = 0;   /* deepest shared-mode (mix) depth      */
    int32_t        excl_bits   = 0;   /* deepest exclusive-mode (hw) depth    */
    bool           any_rate_wildcard = false;

    if (!engine->context_ready) {
        return false;
    }

    memset(&info, 0, sizeof(info));
    if (ma_context_get_device_info(&engine->context, ma_device_type_playback,
                                   NULL /* default device */, &info) != MA_SUCCESS) {
        return false;
    }

    memset(&caps, 0, sizeof(caps));

    /* Endpoint name, truncated + NUL-terminated. */
    strncpy(caps.device_name, info.name, sizeof(caps.device_name) - 1);
    caps.device_name[sizeof(caps.device_name) - 1] = '\0';

    for (i = 0; i < info.nativeDataFormatCount; ++i) {
        ma_format fmt      = info.nativeDataFormats[i].format;
        uint32_t  channels = info.nativeDataFormats[i].channels;
        uint32_t  rate     = info.nativeDataFormats[i].sampleRate;
        bool      excl     = (info.nativeDataFormats[i].flags &
                              MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE) != 0;
        /* ma_format_unknown == "all formats supported" -> treat as 32. */
        int32_t   bits     = (fmt == ma_format_unknown)
                                 ? 32 : (int32_t)mn_bits_from_format(fmt);

        if (excl) {
            caps.exclusive_capable = true;
            if (bits > excl_bits) excl_bits = bits;
        } else {
            if (bits > shared_bits) shared_bits = bits;
            /* The first shared-mode entry is the OS mixer format (WASAPI:
             * GetMixFormat). */
            if (caps.mix_sample_rate == 0 && rate != 0) {
                caps.mix_sample_rate = (int32_t)rate;
            }
        }

        if (rate == 0) {
            /* "All rates supported": advertise the standard ladder. */
            size_t r;
            any_rate_wildcard = true;
            for (r = 0; r < sizeof(mn_standard_rates) / sizeof(mn_standard_rates[0]); ++r) {
                mn_caps_add_rate(&caps, mn_standard_rates[r]);
            }
        } else {
            mn_caps_add_rate(&caps, rate);
            if ((int32_t)rate > caps.max_sample_rate) {
                caps.max_sample_rate = (int32_t)rate;
            }
        }

        if (channels == 0) {
            channels = 8u; /* "all channel counts" -> assume up to 7.1 */
        }
        if ((int32_t)channels > caps.max_channels) {
            caps.max_channels = (int32_t)channels;
        }
    }

    /* Hardware truth beats the (float32) shared mix format for bit depth. */
    caps.max_bit_depth = (excl_bits > 0) ? excl_bits
                       : (shared_bits > 0) ? shared_bits : 32;

    if (any_rate_wildcard && caps.max_sample_rate < 192000) {
        caps.max_sample_rate = 192000;
    }

    /* Conservative fallbacks for backends exposing nothing useful. */
    if (caps.mix_sample_rate == 0) {
        caps.mix_sample_rate = (engine->hw_rate != 0)
                                   ? (int32_t)engine->hw_rate
                                   : (int32_t)MN_ENGINE_SAMPLE_RATE;
    }
    if (caps.max_sample_rate < caps.mix_sample_rate) {
        caps.max_sample_rate = caps.mix_sample_rate;
    }
    if (caps.max_channels == 0) {
        caps.max_channels = (int32_t)MN_ENGINE_CHANNELS;
    }
    mn_caps_add_rate(&caps, (uint32_t)caps.mix_sample_rate);

    engine->caps       = caps;
    engine->caps_valid = MA_TRUE;
    return true;
}

/* Forward declaration (defined with the other callbacks below). */
static void mn_data_callback(ma_device *pDevice, void *pOutput,
                             const void *pInput, ma_uint32 frameCount);

/*
 * (Re)initialize the output device at `rate` (f32 / stereo on the callback
 * side; miniaudio converts to the device's native format on output). Captures
 * the REAL hardware-side format into hw_rate/hw_channels/hw_bits. The device
 * must currently be uninitialized (device_ready == false). Control-thread only.
 */
/*
 * Ensure the post-mix DSP chain exists and is configured for `rate` (stereo in,
 * stereo out). Lazily creates the instance on first call. NOT real-time safe;
 * call only with the device stopped (device open/reopen paths). Preserves the
 * last-published parameter snapshot across reconfigure. Best-effort: on failure
 * the engine simply runs without DSP (the render path guards on NULL + the
 * enabled flag).
 */
static void mn_engine_sync_dsp(mn_engine *engine, uint32_t rate)
{
    mn_dsp_config cfg;

    if (rate == 0) {
        return;
    }
    if (engine->dsp != NULL && engine->dsp_rate == rate) {
        return; /* already configured for this rate */
    }

    cfg.sample_rate = rate;
    cfg.in_channels = MN_ENGINE_CHANNELS;
    cfg.out_layout  = MN_DSP_LAYOUT_STEREO;
    cfg.max_frames  = MN_ENGINE_CHUNK_FRAMES;

    if (engine->dsp == NULL) {
        if (mn_dsp_create(&cfg, &engine->dsp) != MN_DSP_OK) {
            engine->dsp = NULL;
            return;
        }
    } else {
        if (mn_dsp_configure(engine->dsp, &cfg) != MN_DSP_OK) {
            return; /* keep old config; process() still valid at old rate */
        }
    }
    engine->dsp_rate = rate;
    /* Re-apply the published parameter snapshot (configure preserves params,
     * but a freshly created instance needs them). */
    if (engine->dsp_params_valid) {
        mn_dsp_set_params(engine->dsp, &engine->dsp_params);
    }
}

static mn_result mn_device_open(mn_engine *engine, uint32_t rate)
{
    ma_device_config cfg;

    cfg                   = ma_device_config_init(ma_device_type_playback);
    /* The data callback ALWAYS produces f32 (mn_data_callback), so the device
     * "playback.format" MUST stay f32 — miniaudio's internal converter turns
     * that into whatever the hardware runs (in exclusive mode it negotiates
     * the endpoint's native integer format, which we snapshot as hw_bits and
     * report). Setting a non-f32 callback format here would hand the callback
     * an integer buffer while it writes floats → memory corruption. The
     * bit-perfect path is: exclusive mode + matched sample rate + no DSP. */
    cfg.playback.format   = ma_format_f32;
    /* Open at the pipeline channel count (2 for the normal path, the source's
     * channel count for multichannel passthrough). Falls back to stereo if
     * the device rejects it below. */
    cfg.playback.channels = engine->pipe_channels ? engine->pipe_channels
                                                  : MN_ENGINE_CHANNELS;
    cfg.sampleRate        = rate;
    cfg.dataCallback      = mn_data_callback;
    cfg.pUserData         = engine;
    /* Highest-quality resampler for any device-side rate conversion. */
    cfg.resampling.algorithm = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = 8;   /* miniaudio max — steep anti-alias */
    /* Honor an explicit device selection across every reinit (rate retunes,
     * stems toggles); NULL keeps the system default endpoint. */
    if (engine->use_selected) {
        cfg.playback.pDeviceID = &engine->selected_id;
    }
    /* Bit-perfect exclusive output when requested. */
    if (engine->want_exclusive) {
        cfg.playback.shareMode = ma_share_mode_exclusive;
    }

    if (ma_device_init(engine->context_ready ? &engine->context : NULL,
                       &cfg, &engine->device) != MA_SUCCESS) {
        /* Exclusive mode can be refused by the endpoint (in use, unsupported
         * format). Retry in shared mode so playback still works. */
        if (engine->want_exclusive) {
            cfg.playback.shareMode = ma_share_mode_shared;
            if (ma_device_init(engine->context_ready ? &engine->context : NULL,
                               &cfg, &engine->device) != MA_SUCCESS) {
                return MN_ERR_DEVICE;
            }
        } else {
            return MN_ERR_DEVICE;
        }
    }
    engine->device_ready = MA_TRUE;

    /* Snapshot what the hardware is ACTUALLY running at. */
    engine->hw_rate     = (uint32_t)engine->device.playback.internalSampleRate;
    engine->hw_channels = (uint32_t)engine->device.playback.internalChannels;
    engine->hw_bits     = mn_bits_from_format(engine->device.playback.internalFormat);
    if (engine->hw_rate == 0)     engine->hw_rate     = rate;
    if (engine->hw_channels == 0) engine->hw_channels = MN_ENGINE_CHANNELS;
    if (engine->hw_bits == 0)     engine->hw_bits     = 32u;

    /* Keep the DSP chain matched to the callback-side rate. */
    mn_engine_sync_dsp(engine, rate);
    return MN_OK;
}

/*
 * Re-open the decoder for the currently loaded track at `rate`, restoring the
 * playback position `pos_ms` (clamped to the track length) and updating
 * pipe_rate/length/cursor. Must be called with the device STOPPED (the audio
 * callback must not be running). Takes the lock itself. On decoder failure the
 * track is unloaded and MN_ERR_OPEN returned. Control-thread only.
 */
static mn_result mn_reopen_decoder(mn_engine *engine, uint32_t rate, uint64_t pos_ms)
{
    ma_decoder_config dcfg;
    ma_uint64 length = 0;
    uint64_t  frame;

    ma_mutex_lock(&engine->lock);
    if (!engine->has_track || engine->track_path[0] == '\0') {
        ma_mutex_unlock(&engine->lock);
        return MN_ERR_STATE;
    }
    if (engine->is_url) {
        /* HTTP stream (either path): there is no file to re-open at another
         * rate, and the live connection would be lost. Leave the pipeline
         * exactly as it is (streams run stems-less at a fixed rate) and
         * report success so the caller never unloads a playing stream. */
        ma_mutex_unlock(&engine->lock);
        return MN_OK;
    }

    ma_decoder_uninit(&engine->decoder);
    engine->has_track = MA_FALSE;

    dcfg = ma_decoder_config_init(ma_format_f32,
               engine->pipe_channels ? engine->pipe_channels : MN_ENGINE_CHANNELS,
               rate);
    mn_decode_config_apply_backends(&dcfg);
    if (mn_decoder_init_path(engine->track_path, &dcfg, &engine->decoder) != MA_SUCCESS) {
        engine->cursor = 0;
        engine->length = 0;
        engine->at_end = MA_FALSE;
        engine->state  = MN_STATE_STOPPED;
        ma_mutex_unlock(&engine->lock);
        return MN_ERR_OPEN;
    }
    engine->has_track = MA_TRUE;
    engine->pipe_rate = rate;

    if (ma_decoder_get_length_in_pcm_frames(&engine->decoder, &length) == MA_SUCCESS) {
        engine->length = (uint64_t)length;
    } else {
        engine->length = 0;
    }

    frame = mn_ms_to_frames(pos_ms, rate);
    if (engine->length != 0 && frame > engine->length) {
        frame = engine->length;
    }
    if (ma_decoder_seek_to_pcm_frame(&engine->decoder, frame) != MA_SUCCESS) {
        (void)ma_decoder_seek_to_pcm_frame(&engine->decoder, 0);
        frame = 0;
    }
    engine->cursor = frame;
    engine->at_end = MA_FALSE;
    if (engine->stretch != NULL) mn_stretch_reset(engine->stretch);
    ma_mutex_unlock(&engine->lock);
    return MN_OK;
}

/*
 * Make the output device run at engine->pipe_rate, reinitializing it if it is
 * currently at a different rate. Must be called with the device STOPPED. If
 * the device refuses the requested rate, falls back to the canonical
 * MN_ENGINE_SAMPLE_RATE and retunes the decoder to match so the pipeline stays
 * consistent. Control-thread only.
 */
static mn_result mn_sync_device_rate(mn_engine *engine)
{
    uint32_t want = engine->pipe_rate;
    uint32_t wch  = engine->pipe_channels ? engine->pipe_channels : MN_ENGINE_CHANNELS;
    /* Reopen when rate OR channel count no longer matches the device (a
     * stereo→surround crossing needs a fresh device at the new layout). */
    if (engine->device_ready &&
        (uint32_t)engine->device.sampleRate == want &&
        (uint32_t)engine->device.playback.channels == wch) {
        return MN_OK;
    }
    if (engine->device_ready) {
        ma_device_uninit(&engine->device);
        engine->device_ready = MA_FALSE;
    }
    if (mn_device_open(engine, want) == MN_OK) {
        return MN_OK;
    }
    /* Fallback 1: the device rejected the multichannel layout — fold to
     * stereo (re-decode to 2ch) and retry at the same rate. */
    if (wch > MN_ENGINE_CHANNELS) {
        engine->pipe_channels = MN_ENGINE_CHANNELS;
        engine->downmixed     = MA_TRUE;
        if (engine->has_track) {
            uint64_t pos_ms = mn_frames_to_ms(engine->cursor, engine->pipe_rate);
            (void)mn_reopen_decoder(engine, want, pos_ms);
        }
        if (mn_device_open(engine, want) == MN_OK) {
            return MN_OK;
        }
    }
    /* Fallback 2: canonical rate; keep decoder and device in lockstep. */
    if (mn_device_open(engine, MN_ENGINE_SAMPLE_RATE) == MN_OK) {
        if (want != MN_ENGINE_SAMPLE_RATE && engine->has_track) {
            uint64_t pos_ms = mn_frames_to_ms(engine->cursor, engine->pipe_rate);
            (void)mn_reopen_decoder(engine, MN_ENGINE_SAMPLE_RATE, pos_ms);
        }
        engine->pipe_rate = MN_ENGINE_SAMPLE_RATE;
        return MN_OK;
    }
    return MN_ERR_DEVICE;
}

/* ------------------------------------------------------------------------- */
/* Audio callback                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Render one chunk (<= MN_ENGINE_CHUNK_FRAMES frames) starting at the current
 * cursor into `dst` (interleaved stereo f32). Reads from the decoder, optionally
 * routes through the stem mixer for the absolute frame range, applies the output
 * level, advances the cursor and updates at_end. Returns the number of frames
 * actually produced (0 at EOS). Called with `lock` held.
 */
static ma_uint32 mn_render_chunk(mn_engine *engine, float *dst, ma_uint32 want)
{
    ma_uint64 read = 0;
    ma_uint64 start_frame;
    float     level;
    ma_uint32 i;
    const ma_uint32 ch = engine->pipe_channels ? engine->pipe_channels
                                               : MN_ENGINE_CHANNELS;
    const ma_bool32 stereo = (ch == 2u);

    if (want > MN_ENGINE_CHUNK_FRAMES) {
        want = MN_ENGINE_CHUNK_FRAMES;
    }

    start_frame = engine->cursor;

    /* Pull decoded source into dst. A short read means we hit EOS. */
    if (ma_decoder_read_pcm_frames(&engine->decoder, dst, want, &read) != MA_SUCCESS) {
        read = 0;
    }
    if (read == 0) {
        engine->at_end = MA_TRUE;
        return 0;
    }
    /* Zero any tail we could not fill (partial final block). */
    if (read < want) {
        memset(dst + (size_t)read * ch, 0,
               (size_t)(want - read) * ch * sizeof(float));
    }

    /* Stem mixer + DSP are STEREO processors; they run only on the 2ch path.
     * Multichannel passthrough (ch>2) deliberately skips both so surround
     * content is delivered intact (stems/DSP are auto-disabled for it at
     * load time — see the pipe_channels decision in mn_engine_load). */
    if (stereo) {
        if (engine->stems != NULL && engine->stems_enabled &&
            !engine->stems_passthrough && engine->mix_buf != NULL) {
            if (mn_stems_mix(engine->stems, (int64_t)start_frame,
                             (uint32_t)read, engine->mix_buf)) {
                memcpy(dst, engine->mix_buf,
                       (size_t)read * MN_ENGINE_CHANNELS * sizeof(float));
            }
        }
    }

    /* Apply combined output level (volume * gain) across ALL channels.
     * When level==1.0 the samples are not touched at all — bit-perfect. */
    level = engine->volume * engine->gain_linear;
    if (level != 1.0f) {
        for (i = 0; i < (ma_uint32)read * ch; ++i) {
            dst[i] *= level;
        }
    }

    if (stereo && engine->dsp != NULL && engine->dsp_enabled) {
        mn_dsp_process(engine->dsp, dst, (uint32_t)read, MN_ENGINE_CHANNELS);
    }

    /* Spectrum capture: fold the block into the mono capture ring. Averages
     * the front L/R for stereo, or the first two channels for surround. */
    {
        uint32_t w = engine->spec_write;
        ma_uint32 fi;
        for (fi = 0; fi < (ma_uint32)read; fi++) {
            engine->spec_capture[w & 511u] =
                0.5f * (dst[(size_t)fi * ch] + dst[(size_t)fi * ch + 1]);
            w++;
        }
        engine->spec_write = w;
    }

    engine->cursor += read;
    if (read < want) {
        /* Reached the end of the stream within this block. */
        engine->at_end = MA_TRUE;
    }
    return (ma_uint32)read;
}

/*
 * Device data callback (audio thread). Fills `pOutput` with frameCount frames of
 * interleaved stereo f32. When no track is loaded, or after EOS, the remainder
 * is left silent (the device buffer is pre-silenced by miniaudio).
 */
static void mn_data_callback(ma_device *pDevice, void *pOutput,
                             const void *pInput, ma_uint32 frameCount)
{
    mn_engine *engine = (mn_engine *)pDevice->pUserData;
    float     *out    = (float *)pOutput;
    ma_uint32  done   = 0;

    (void)pInput;
    if (engine == NULL) {
        return;
    }

    ma_mutex_lock(&engine->lock);
    {
    const ma_uint32 ch = engine->pipe_channels ? engine->pipe_channels
                                                : MN_ENGINE_CHANNELS;
    if (engine->has_track && !engine->at_end &&
        engine->speed != 1.0f && ch == 2u && engine->stretch != NULL) {
        /* Pitch-preserved speed path (stereo only): feed source chunks into
         * the WSOLA stretcher, pop stretched frames to the device. The
         * cursor advances by SOURCE frames inside mn_render_chunk, so
         * position/seek stay in source time. At source EOS the stretcher is
         * flushed and its tail drains before at_end playback stops. */
        mn_stretch_set_speed(engine->stretch, engine->speed);
        while (done < frameCount) {
            int got = mn_stretch_pop(engine->stretch,
                                     out + (size_t)done * 2,
                                     (int)(frameCount - done));
            done += (ma_uint32)got;
            if (done >= frameCount) break;
            if (engine->at_end) break;               /* flushed + drained */
            {
                int need = mn_stretch_need_input(engine->stretch);
                if (need <= 0) {
                    if (got == 0) break;             /* safety: no progress */
                    continue;
                }
                while (need > 0) {
                    ma_uint32 want = (ma_uint32)need;
                    ma_uint32 gotc;
                    if (want > MN_ENGINE_CHUNK_FRAMES)
                        want = MN_ENGINE_CHUNK_FRAMES;
                    gotc = mn_render_chunk(engine, engine->stretch_src, want);
                    if (gotc == 0) {                 /* source EOS */
                        mn_stretch_flush(engine->stretch);
                        break;
                    }
                    mn_stretch_push(engine->stretch, engine->stretch_src,
                                    (int)gotc);
                    need -= (int)gotc;
                }
            }
        }
    } else if (engine->has_track && !engine->at_end) {
        while (done < frameCount) {
            ma_uint32 want = frameCount - done;
            ma_uint32 got;
            if (want > MN_ENGINE_CHUNK_FRAMES) {
                want = MN_ENGINE_CHUNK_FRAMES;
            }
            got = mn_render_chunk(engine, out + (size_t)done * ch, want);
            done += got;
            if (got < want) {
                break; /* EOS */
            }
        }
    }
    /* Silence any unfilled tail (pre-silenced buffer covers this, but be
     * explicit for stopped/EOS blocks). */
    if (done < frameCount) {
        memset(out + (size_t)done * ch, 0,
               (size_t)(frameCount - done) * ch * sizeof(float));
    }
    }
    ma_mutex_unlock(&engine->lock);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

mn_result mn_engine_create(mn_engine **out_engine)
{
    mn_engine *engine;
    uint32_t   initial_rate;

    if (out_engine == NULL) {
        return MN_ERR_INVALID;
    }
    *out_engine = NULL;

    engine = (mn_engine *)calloc(1, sizeof(*engine));
    if (engine == NULL) {
        return MN_ERR_NOMEM;
    }

    engine->state             = MN_STATE_STOPPED;
    engine->volume            = 1.0f;
    engine->gain_linear       = 1.0f;
    engine->device_ready      = MA_FALSE;
    engine->has_track         = MA_FALSE;
    engine->stems             = NULL;
    engine->stems_enabled     = MA_FALSE;
    engine->stems_passthrough = MA_FALSE;
    engine->format[0]         = '\0';
    engine->pipe_rate         = MN_ENGINE_SAMPLE_RATE;
    engine->pipe_channels     = MN_ENGINE_CHANNELS;
    engine->downmixed         = MA_FALSE;
    engine->use_selected      = MA_FALSE;
    engine->selected_index    = -1;
    /* Audiophile defaults: native bit-depth in exclusive mode ON, no caps. */
    engine->hifi_native_bits  = MA_TRUE;
    engine->rate_cap_hz       = 0;
    engine->bits_cap          = 0;
    engine->want_device_format = ma_format_f32;

    /* Scratch buffer for the stem mix (interleaved stereo). */
    engine->mix_buf = (float *)malloc((size_t)MN_ENGINE_CHUNK_FRAMES *
                                      MN_ENGINE_CHANNELS * sizeof(float));
    if (engine->mix_buf == NULL) {
        free(engine);
        return MN_ERR_NOMEM;
    }

    /* Playback-speed stretcher (lazy on first use would race the audio
     * thread — create it up front; it is tiny). speed defaults to 1.0 =
     * fully bypassed. */
    engine->speed       = 1.0f;
    engine->stretch     = mn_stretch_create();
    engine->stretch_src = (float *)malloc((size_t)MN_ENGINE_CHUNK_FRAMES *
                                          MN_ENGINE_CHANNELS * sizeof(float));
    if (engine->stretch == NULL || engine->stretch_src == NULL) {
        mn_stretch_destroy(engine->stretch);
        free(engine->stretch_src);
        free(engine->mix_buf);
        free(engine);
        return MN_ERR_NOMEM;
    }

    if (ma_mutex_init(&engine->lock) != MA_SUCCESS) {
        free(engine->mix_buf);
        free(engine);
        return MN_ERR_DEVICE;
    }
    engine->lock_ready = MA_TRUE;

    /* Backend context: owned so the engine can query device capabilities and
     * reinitialize the device across rate changes. Non-fatal if unavailable
     * (device falls back to an internally managed context, caps unavailable). */
    if (ma_context_init(NULL, 0, NULL, &engine->context) == MA_SUCCESS) {
        engine->context_ready = MA_TRUE;
    }

    /* Discover the default device's native capabilities up front, then open
     * the device at the hardware's preferred (OS mix) rate rather than a
     * hardcoded one, so idle/first playback already matches the hardware. */
    (void)mn_query_caps(engine);
    initial_rate = (engine->caps_valid && engine->caps.mix_sample_rate > 0)
                       ? (uint32_t)engine->caps.mix_sample_rate
                       : MN_ENGINE_SAMPLE_RATE;

    engine->pipe_rate = initial_rate;
    if (mn_device_open(engine, initial_rate) != MN_OK) {
        /* Retry at the canonical rate before giving up. */
        engine->pipe_rate = MN_ENGINE_SAMPLE_RATE;
        if (initial_rate == MN_ENGINE_SAMPLE_RATE ||
            mn_device_open(engine, MN_ENGINE_SAMPLE_RATE) != MN_OK) {
            if (engine->context_ready) {
                ma_context_uninit(&engine->context);
            }
            ma_mutex_uninit(&engine->lock);
            free(engine->mix_buf);
            free(engine);
            return MN_ERR_DEVICE;
        }
    }
    /* Caps may have lacked a mix rate before the device existed; refresh the
     * fallback fields now that hw_rate is known. */
    if (!engine->caps_valid) {
        (void)mn_query_caps(engine);
    }

    /*
     * Start the device now and leave it running for the engine's lifetime. The
     * callback outputs silence whenever no track is loaded or playback is
     * paused/stopped, so a persistently-running device is simplest and lets
     * play/pause be pure state flips plus start/stop of the decoder cursor.
     * (We still stop the device on pause to avoid burning a wakeup; see below.)
     */
    *out_engine = engine;
    return MN_OK;
}

void mn_engine_destroy(mn_engine *engine)
{
    if (engine == NULL) {
        return;
    }
    if (engine->device_ready) {
        /* Stop the device first so the callback can no longer touch the
         * decoder, then tear the decoder down. */
        ma_device_uninit(&engine->device);
        engine->device_ready = MA_FALSE;
    }
    mn_unload(engine);
    if (engine->context_ready) {
        ma_context_uninit(&engine->context);
        engine->context_ready = MA_FALSE;
    }
    if (engine->lock_ready) {
        ma_mutex_uninit(&engine->lock);
        engine->lock_ready = MA_FALSE;
    }
    if (engine->dsp != NULL) {
        mn_dsp_destroy(engine->dsp);
        engine->dsp = NULL;
    }
    mn_stretch_destroy(engine->stretch);
    free(engine->stretch_src);
    free(engine->mix_buf);
    free(engine);
}

/* ------------------------------------------------------------------------- */
/* Playback speed (pitch-preserved; audiobooks)                               */
/* ------------------------------------------------------------------------- */

void mn_engine_set_length_hint_ms(mn_engine *engine, int64_t ms)
{
    if (engine == NULL) return;
    engine->length_hint_ms = (ms > 0) ? ms : 0;
}

mn_result mn_engine_set_speed(mn_engine *engine, float speed)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 3.0f) speed = 3.0f;
    ma_mutex_lock(&engine->lock);
    if (engine->speed != speed) {
        /* Returning to 1.0 bypasses the stretcher — drop its buffered audio
         * so no stale stretched tail plays; entering/changing a stretched
         * speed keeps continuity (WSOLA adapts on the next grain). */
        if (speed == 1.0f && engine->stretch != NULL) {
            mn_stretch_reset(engine->stretch);
        }
        engine->speed = speed;
    }
    ma_mutex_unlock(&engine->lock);
    return MN_OK;
}

float mn_engine_get_speed(const mn_engine *engine)
{
    return engine ? engine->speed : 1.0f;
}

/* ------------------------------------------------------------------------- */
/* DSP chain control (control thread; publishes to the audio thread)          */
/* ------------------------------------------------------------------------- */

/* Master enable for the whole DSP chain. When off the render path skips
 * mn_dsp_process entirely (pure passthrough). Lockless flag write. */
void mn_engine_set_dsp_enabled(mn_engine *engine, int enabled)
{
    if (engine == NULL) return;
    engine->dsp_enabled = enabled ? MA_TRUE : MA_FALSE;
}

int mn_engine_get_dsp_enabled(const mn_engine *engine)
{
    return (engine != NULL && engine->dsp_enabled) ? 1 : 0;
}

/* Toggle the EQ stage within the chain. */
void mn_engine_set_eq_enabled(mn_engine *engine, int enabled)
{
    if (engine == NULL) return;
    engine->dsp_params.eq_enabled = enabled ? 1 : 0;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_eq_enabled(engine->dsp, enabled);
}

/* Set one 10-band EQ band gain in dB (band 0..9). */
void mn_engine_set_eq_band(mn_engine *engine, uint32_t band, float gain_db)
{
    if (engine == NULL || band >= MN_DSP_EQ_BANDS) return;
    engine->dsp_params.eq_gains_db[band] = gain_db;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_eq_band(engine->dsp, band, gain_db);
}

/* Set all 10 EQ band gains at once. */
void mn_engine_set_eq_gains(mn_engine *engine, const float gains_db[10])
{
    uint32_t b;
    if (engine == NULL || gains_db == NULL) return;
    for (b = 0; b < MN_DSP_EQ_BANDS; ++b) {
        engine->dsp_params.eq_gains_db[b] = gains_db[b];
    }
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_eq_gains(engine->dsp, gains_db);
}

/* Apply a built-in preset (overwrites all bands + preamp). Returns the
 * resolved gains into out_gains[10] and out_preamp for UI reflection (both
 * may be NULL). */
void mn_engine_set_eq_preset(mn_engine *engine, int preset,
                             float out_gains[10], float *out_preamp)
{
    mn_dsp_params p;
    uint32_t b;
    if (engine == NULL) return;
    if (preset < 0 || preset >= MN_DSP_EQ_PRESET_COUNT) preset = 0;
    if (engine->dsp) {
        mn_dsp_set_eq_preset(engine->dsp, (mn_dsp_eq_preset)preset);
        if (mn_dsp_get_params(engine->dsp, &p) == MN_DSP_OK) {
            for (b = 0; b < MN_DSP_EQ_BANDS; ++b)
                engine->dsp_params.eq_gains_db[b] = p.eq_gains_db[b];
            engine->dsp_params.preamp_db  = p.preamp_db;
            engine->dsp_params.eq_enabled = p.eq_enabled;
            engine->dsp_params_valid = MA_TRUE;
        }
    }
    if (out_gains) {
        for (b = 0; b < MN_DSP_EQ_BANDS; ++b)
            out_gains[b] = engine->dsp_params.eq_gains_db[b];
    }
    if (out_preamp) *out_preamp = engine->dsp_params.preamp_db;
}

void mn_engine_set_preamp(mn_engine *engine, float preamp_db)
{
    if (engine == NULL) return;
    engine->dsp_params.preamp_db = preamp_db;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_preamp(engine->dsp, preamp_db);
}

void mn_engine_set_balance(mn_engine *engine, float balance)
{
    if (engine == NULL) return;
    engine->dsp_params.balance = balance;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_balance(engine->dsp, balance);
}

void mn_engine_set_limiter(mn_engine *engine, int enabled,
                           float threshold_db, float ceiling_db)
{
    if (engine == NULL) return;
    engine->dsp_params.limiter_enabled      = enabled ? 1 : 0;
    engine->dsp_params.limiter_threshold_db = threshold_db;
    engine->dsp_params.limiter_ceiling_db   = ceiling_db;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) {
        mn_dsp_set_limiter_enabled(engine->dsp, enabled);
        mn_dsp_set_limiter(engine->dsp, threshold_db, ceiling_db);
    }
}

void mn_engine_set_master_gain(mn_engine *engine, float master_gain_db)
{
    if (engine == NULL) return;
    engine->dsp_params.master_gain_db = master_gain_db;
    engine->dsp_params_valid = MA_TRUE;
    if (engine->dsp) mn_dsp_set_master_gain(engine->dsp, master_gain_db);
}

/* Copy the current EQ band gains + preamp into out (for UI hydration). */
void mn_engine_get_eq(const mn_engine *engine, float out_gains[10],
                      float *out_preamp, int *out_enabled)
{
    uint32_t b;
    if (engine == NULL) return;
    if (out_gains) {
        for (b = 0; b < MN_DSP_EQ_BANDS; ++b)
            out_gains[b] = engine->dsp_params.eq_gains_db[b];
    }
    if (out_preamp)  *out_preamp  = engine->dsp_params.preamp_db;
    if (out_enabled) *out_enabled = engine->dsp_params.eq_enabled;
}

/* Request exclusive (bit-perfect) WASAPI output. Restarts the device in the
 * new share mode when the flag actually changes and a device is open. Control
 * thread only (stops/reopens the device). Falls back to shared if exclusive is
 * refused (see mn_device_open). */
void mn_engine_set_exclusive(mn_engine *engine, int enabled)
{
    ma_bool32 want;
    if (engine == NULL) return;
    want = enabled ? MA_TRUE : MA_FALSE;
    if (engine->want_exclusive == want) return;
    engine->want_exclusive = want;
    if (engine->device_ready) {
        ma_bool32 was_playing = (engine->state == MN_STATE_PLAYING);
        ma_device_uninit(&engine->device);
        engine->device_ready = MA_FALSE;
        if (mn_device_open(engine, engine->pipe_rate) != MN_OK) {
            /* last-ditch: reopen at canonical rate */
            (void)mn_device_open(engine, MN_ENGINE_SAMPLE_RATE);
        }
        if (was_playing && engine->device_ready) {
            (void)ma_device_start(&engine->device);
        }
    }
}

/* Audiophile output profile. hifi_native_bits: exclusive-mode device opens at
 * the source's native integer depth. rate_cap_hz / bits_cap: power-saving
 * downscale (0 = uncapped). Reloads the CURRENT track so the change takes
 * effect immediately (control thread only). */
void mn_engine_set_hifi_profile(mn_engine *engine, int native_bits,
                                uint32_t rate_cap_hz, uint32_t bits_cap)
{
    ma_bool32 nb;
    if (engine == NULL) return;
    nb = native_bits ? MA_TRUE : MA_FALSE;
    if (engine->hifi_native_bits == nb && engine->rate_cap_hz == rate_cap_hz &&
        engine->bits_cap == bits_cap) {
        return;
    }
    engine->hifi_native_bits = nb;
    engine->rate_cap_hz      = rate_cap_hz;
    engine->bits_cap         = bits_cap;
    if (engine->device_ready && engine->has_track && engine->track_path[0]) {
        char path[sizeof(engine->track_path)];
        uint64_t pos_ms = mn_frames_to_ms(engine->cursor, engine->pipe_rate);
        ma_bool32 was_playing = (engine->state == MN_STATE_PLAYING);
        strncpy(path, engine->track_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        if (mn_engine_load(engine, path) == MN_OK) {
            mn_engine_seek_ms(engine, pos_ms);
            if (was_playing) (void)mn_engine_play(engine);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Spectrum analyzer                                                          */
/* ------------------------------------------------------------------------- */

/* In-place iterative radix-2 FFT (n a power of two). re/im length n. */
static void mn_fft(float *re, float *im, int n)
{
    int i, j, k, m, step;
    /* bit-reversal permutation */
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (step = 2; step <= n; step <<= 1) {
        float ang = -6.28318530718f / (float)step;
        float wr = cosf(ang), wi = sinf(ang);
        for (m = 0; m < n; m += step) {
            float cr = 1.0f, ci = 0.0f;
            for (k = 0; k < step / 2; k++) {
                int a = m + k, b = m + k + step / 2;
                float tr = cr * re[b] - ci * im[b];
                float ti = cr * im[b] + ci * re[b];
                float ncr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                ncr = cr * wr - ci * wi;
                ci  = cr * wi + ci * wr;
                cr  = ncr;
            }
        }
    }
}

/* Compute + copy the latest spectrum bars into out[]. Runs the FFT reduction
 * on the calling (UI) thread from a lockless snapshot of the capture ring.
 * Returns the number of bars written. */
uint32_t mn_engine_get_spectrum(mn_engine *engine, float *out, uint32_t max)
{
    enum { N = 512 };
    float re[N], im[N];
    uint32_t i, b, n;

    if (engine == NULL || out == NULL || max == 0) return 0;

    /* copy the capture (already mono) applying a Hann window */
    for (i = 0; i < N; i++) {
        float w = 0.5f - 0.5f * cosf(6.28318530718f * (float)i / (float)(N - 1));
        re[i] = engine->spec_capture[i] * w;
        im[i] = 0.0f;
    }
    mn_fft(re, im, N);

    /* group the first N/2 bins into log-spaced bars */
    for (b = 0; b < MN_SPECTRUM_BARS; b++) {
        float f0 = powf((float)(N / 2), (float)b / (float)MN_SPECTRUM_BARS);
        float f1 = powf((float)(N / 2), (float)(b + 1) / (float)MN_SPECTRUM_BARS);
        int lo = (int)f0, hi = (int)f1;
        float sum = 0.0f; int cnt = 0; float v;
        if (hi <= lo) hi = lo + 1;
        if (hi > N / 2) hi = N / 2;
        for (i = (uint32_t)lo; i < (uint32_t)hi; i++) {
            sum += sqrtf(re[i] * re[i] + im[i] * im[i]);
            cnt++;
        }
        v = cnt ? (sum / (float)cnt) : 0.0f;
        /* log compression + normalize to a pleasant range */
        v = log10f(1.0f + 9.0f * v * 0.15f);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        engine->spec_bars[b] = v;
    }

    n = (max < MN_SPECTRUM_BARS) ? max : MN_SPECTRUM_BARS;
    for (i = 0; i < n; i++) out[i] = engine->spec_bars[i];
    return n;
}

/* ------------------------------------------------------------------------- */
/* Loading / transport                                                        */
/* ------------------------------------------------------------------------- */

mn_result mn_engine_load(mn_engine *engine, const char *path)
{
    ma_decoder_config dcfg;
    ma_result mr;
    ma_uint64 length = 0;
    uint32_t  native_rate;
    uint32_t  target_rate;

    if (engine == NULL || path == NULL || path[0] == '\0') {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready) {
        return MN_ERR_STATE;
    }

    /* Stop the device so the callback is not touching the decoder while we swap
     * the loaded track. */
    ma_device_stop(&engine->device);

    ma_mutex_lock(&engine->lock);

    /* Replace any previously loaded track. */
    mn_unload(engine);

    /* First open at the SOURCE's native rate (sampleRate 0 = no resampling)
     * so we can decide the adaptive pipeline rate for this track. */
    dcfg = ma_decoder_config_init(ma_format_f32, MN_ENGINE_CHANNELS, 0);
    mn_decode_config_apply_backends(&dcfg);
    mr = mn_decoder_init_path(path, &dcfg, &engine->decoder);
    if (mr != MA_SUCCESS) {
        ma_mutex_unlock(&engine->lock);
        if (mr == MA_NOT_IMPLEMENTED || mr == MA_INVALID_FILE) {
            return MN_ERR_UNSUPPORTED;
        }
        return MN_ERR_OPEN;
    }

    /* Capture the source format for introspection from the decoder's converter
     * input side (i.e. the file's native format before any conversion). */
    engine->src_sample_rate = (uint32_t)engine->decoder.converter.sampleRateIn;
    engine->src_channels    = (uint32_t)engine->decoder.converter.channelsIn;
    engine->src_bits        = mn_bits_from_format(engine->decoder.converter.formatIn);
    /* Bitrate is not exposed by the decoder; leave at 0. */
    engine->bitrate = 0;

    native_rate = (uint32_t)engine->decoder.outputSampleRate;
    if (native_rate == 0) {
        native_rate = MN_ENGINE_SAMPLE_RATE;
    }

    /*
     * Choose the pipeline (decoder output == device) rate:
     *   - stems ON : pinned to 44100 so mn_stems_mix always sees its canonical
     *                rate; the device-side converter handles 44100 -> hardware.
     *   - stems OFF: the source's native rate, clamped to the hardware max, so
     *                matching content plays without any decoder resampling
     *                (bit-perfect-ish path).
     */
    if (engine->stems_enabled && engine->stems != NULL) {
        target_rate = MN_ENGINE_SAMPLE_RATE;
    } else {
        target_rate = native_rate;
        if (engine->caps_valid && engine->caps.max_sample_rate > 0 &&
            target_rate > (uint32_t)engine->caps.max_sample_rate) {
            target_rate = (uint32_t)engine->caps.max_sample_rate;
        }
    }
    /* Power-saving rate cap (audiobook profile): never UP-sample past it, and
     * only cap DOWN when the source is genuinely higher — spoken word gains
     * nothing from 96 kHz but the SoC clocks up for it. */
    if (engine->rate_cap_hz > 0 && target_rate > engine->rate_cap_hz) {
        target_rate = engine->rate_cap_hz;
    }

    /*
     * MULTICHANNEL PASSTHROUGH decision. A >2ch source is delivered intact —
     * never force-downmixed to stereo — when: stems + DSP are both OFF (they
     * are stereo processors), the hardware supports the channel count, and
     * we are not in the audiobook power profile. Otherwise fold to stereo
     * (and flag it so the UI can report the quality event honestly).
     */
    {
        uint32_t sch = engine->src_channels ? engine->src_channels : 2u;
        uint32_t hw_max = (engine->caps_valid && engine->caps.max_channels > 0)
                        ? (uint32_t)engine->caps.max_channels : 2u;
        ma_bool32 stems_on = (engine->stems_enabled && engine->stems != NULL);
        ma_bool32 dsp_on   = (engine->dsp != NULL && engine->dsp_enabled);
        if (sch > 2u && !stems_on && !dsp_on && sch <= hw_max &&
            engine->rate_cap_hz == 0) {
            engine->pipe_channels = sch;   /* passthrough */
            engine->downmixed     = MA_FALSE;
        } else {
            engine->pipe_channels = MN_ENGINE_CHANNELS;
            engine->downmixed     = (sch > 2u) ? MA_TRUE : MA_FALSE;
        }
    }

    /* Re-open at the target rate/channels when they differ from the native
     * probe. (channelsIn is the source; the decoder converts to
     * pipe_channels — for passthrough that equals the source so no mix.) */
    if (target_rate != (uint32_t)engine->decoder.outputSampleRate ||
        engine->pipe_channels != (uint32_t)engine->decoder.outputChannels) {
        ma_decoder_uninit(&engine->decoder);
        dcfg = ma_decoder_config_init(ma_format_f32, engine->pipe_channels, target_rate);
        mn_decode_config_apply_backends(&dcfg);
        mr = mn_decoder_init_path(path, &dcfg, &engine->decoder);
        if (mr != MA_SUCCESS) {
            ma_mutex_unlock(&engine->lock);
            return MN_ERR_OPEN;
        }
    }

    engine->has_track = MA_TRUE;
    engine->cursor    = 0;
    engine->at_end    = MA_FALSE;
    engine->pipe_rate = target_rate;
    /* Fresh track: never let the previous track's stretched tail bleed in. */
    if (engine->stretch != NULL) mn_stretch_reset(engine->stretch);

    /* Remember the path so the pipeline can be retuned later (stems toggle). */
    strncpy(engine->track_path, path, sizeof(engine->track_path) - 1);
    engine->track_path[sizeof(engine->track_path) - 1] = '\0';

    /* Total length in output-rate frames. A DB duration hint (set by the
     * playback controller from the track's tag) lets us SKIP the decoder
     * length query — for large VBR files that query scans the ENTIRE file
     * and made switching tracks feel non-instant. */
    if (engine->length_hint_ms > 0) {
        engine->length = mn_ms_to_frames((uint64_t)engine->length_hint_ms,
                                         target_rate);
    } else {
        /* No hint (untagged row / direct load). The accurate query scans the
         * WHOLE file for VBR formats — fine for songs, but a multi-GB
         * single-file audiobook stalled the switch for seconds. Above 64 MB
         * play immediately with unknown length instead (the UI showed 0 for
         * these rows anyway; EOS still comes from decode EOF). */
        struct _stati64 stt;
        if (_stati64(path, &stt) == 0 && stt.st_size > 64LL * 1024 * 1024) {
            engine->length = 0;
        } else if (ma_decoder_get_length_in_pcm_frames(&engine->decoder,
                                                       &length) == MA_SUCCESS) {
            engine->length = (uint64_t)length;
        } else {
            engine->length = 0;
        }
    }
    engine->length_hint_ms = 0;   /* one-shot */

    mn_label_from_path(path, engine->format, sizeof(engine->format));

    engine->state = MN_STATE_STOPPED; /* loaded but not yet playing */
    ma_mutex_unlock(&engine->lock);

    /* Retune the device to the pipeline rate if needed (device is stopped). */
    if (mn_sync_device_rate(engine) != MN_OK) {
        ma_mutex_lock(&engine->lock);
        mn_unload(engine);
        ma_mutex_unlock(&engine->lock);
        return MN_ERR_DEVICE;
    }
    return MN_OK;
}

/* ---- HTTP stream loading (internet radio / streamed podcasts) --------- */

static int g_net_trace = 0;   /* --nettest2 verbosity */

static ma_result mn_net_on_read(ma_decoder *dec, void *out, size_t bytes,
                                size_t *bytes_read)
{
    mn_netstream *ns = (mn_netstream *)dec->pUserData;
    size_t got = mn_netstream_read(ns, out, bytes);
    if (g_net_trace) printf("    on_read(%zu) -> %zu\n", bytes, got);
    if (bytes_read) *bytes_read = got;
    if (got == 0) return MA_AT_END;
    return MA_SUCCESS;
}

static ma_result mn_net_on_seek(ma_decoder *dec, ma_int64 offset,
                                ma_seek_origin origin)
{
    mn_netstream *ns = (mn_netstream *)dec->pUserData;
    int64_t target;
    int64_t cur = mn_netstream_tell(ns);
    int64_t len = mn_netstream_length(ns);

    switch (origin) {
        case ma_seek_origin_start:   target = offset;       break;
        case ma_seek_origin_current: target = cur + offset; break;
        case ma_seek_origin_end:
            if (len < 0) {
                if (g_net_trace) printf("    on_seek(end%+lld) -> NOT_IMPL (live)\n",
                                        (long long)offset);
                return MA_NOT_IMPLEMENTED;
            }
            target = len + offset;
            break;
        default: return MA_INVALID_ARGS;
    }
    if (g_net_trace) printf("    on_seek(origin=%d off=%lld cur=%lld -> %lld)\n",
                            (int)origin, (long long)offset, (long long)cur,
                            (long long)target);
    if (target == cur) return MA_SUCCESS;       /* no-op probes are fine */
    /* netstream resolves: buffered-ahead skip, history rewind (live-safe),
     * or an HTTP Range re-request (seekable only). */
    if (mn_netstream_seek(ns, target)) return MA_SUCCESS;
    if (!mn_netstream_seekable(ns) && target > cur &&
        target - cur <= 256 * 1024) {
        /* Live mount, small FORWARD skip beyond the buffer: consume. */
        char scratch[4096];
        int64_t left = target - cur;
        while (left > 0) {
            size_t got = mn_netstream_read(ns, scratch,
                    left < (int64_t)sizeof(scratch) ? (size_t)left
                                                    : sizeof(scratch));
            if (got == 0) return MA_AT_END;
            left -= (int64_t)got;
        }
        return MA_SUCCESS;
    }
    return MA_NOT_IMPLEMENTED;
}

/* Map a Content-Type (already lowercased) / URL to a miniaudio encoding.
 * Returns ma_encoding_format_unknown for containers we cannot decode over
 * a socket yet (AAC/MP4 need the MF byte-stream bridge). */
static ma_encoding_format mn_net_encoding(const char *ctype, const char *url)
{
    if (strstr(ctype, "mpeg") || strstr(ctype, "mp3") || strstr(ctype, "mpg"))
        return ma_encoding_format_mp3;
    if (strstr(ctype, "flac"))
        return ma_encoding_format_flac;
    if (strstr(ctype, "wav"))
        return ma_encoding_format_wav;
    if (strstr(ctype, "aac") || strstr(ctype, "mp4") || strstr(ctype, "m4a"))
        return ma_encoding_format_unknown;   /* MF bridge territory */
    /* No usable type (octet-stream / missing): guess from the URL. */
    {
        const char *q = strchr(url, '?');
        size_t n = q ? (size_t)(q - url) : strlen(url);
        if (n >= 4 && _strnicmp(url + n - 4, ".mp3", 4) == 0)
            return ma_encoding_format_mp3;
        if (n >= 5 && _strnicmp(url + n - 5, ".flac", 5) == 0)
            return ma_encoding_format_flac;
        if (n >= 4 && (_strnicmp(url + n - 4, ".aac", 4) == 0 ||
                       _strnicmp(url + n - 4, ".m4a", 4) == 0 ||
                       _strnicmp(url + n - 4, ".mp4", 4) == 0))
            return ma_encoding_format_unknown;
    }
    /* Ambiguous (octet-stream / missing / unrecognised): route to the
     * MF-URL path — Media Foundation sniffs the container itself and
     * handles MP3 as well as AAC/MP4. Guessing mp3 here once sent a live
     * AAC stream into dr_mp3's sync scan, which never terminates on an
     * endless source. Only the ICY song-title nicety is lost. */
    return ma_encoding_format_unknown;
}

mn_result mn_engine_load_url(mn_engine *engine, const char *url, int want_icy,
                             char *err, size_t err_cap)
{
    ma_decoder_config dcfg;
    ma_result mr;
    mn_netstream *ns;
    ma_encoding_format enc;

    if (err && err_cap) err[0] = 0;
    if (engine == NULL || url == NULL || url[0] == '\0') {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready) {
        return MN_ERR_STATE;
    }

    /* Connect FIRST (blocking, seconds) so a failed connect never tears the
     * current track down. Caller must be a worker thread. */
    ns = mn_netstream_open(url, want_icy ? true : false, err, err_cap);
    if (ns == NULL) {
        return MN_ERR_OPEN;
    }
    enc = mn_net_encoding(mn_netstream_content_type(ns), url);
    if (enc == ma_encoding_format_unknown) {
        /* AAC/MP4: Media Foundation streams the URL itself (progressive
         * download, ADTS/MP4 demux, Range seek). The netstream probe is no
         * longer needed — MF makes its own clean connection (no ICY). */
        mn_netstream_close(ns);
        ns = NULL;
    } else {
        /* Pre-roll so the decoder header probe + first render never starve:
         * ~6 s of a 128 kbps stream, or the whole file if smaller. */
        mn_netstream_wait_buffered(ns, 96 * 1024, 10000);
    }

    ma_device_stop(&engine->device);
    ma_mutex_lock(&engine->lock);
    mn_unload(engine);

    /* Streams decode at a FIXED 44100/stereo pipeline — predictable device
     * behaviour, no native-rate probe (that would need a second connect). */
    dcfg = ma_decoder_config_init(ma_format_f32, MN_ENGINE_CHANNELS,
                                  MN_ENGINE_SAMPLE_RATE);
    if (ns != NULL) {
        dcfg.encodingFormat = enc;
        mr = ma_decoder_init(mn_net_on_read, mn_net_on_seek, ns, &dcfg,
                             &engine->decoder);
    } else {
        mn_decode_config_apply_mf_url(&dcfg, url);
        mr = ma_decoder_init(mn_net_on_read, mn_net_on_seek, NULL, &dcfg,
                             &engine->decoder);
    }
    if (mr != MA_SUCCESS) {
        ma_mutex_unlock(&engine->lock);
        if (ns) mn_netstream_close(ns);
        if (err && !err[0]) snprintf(err, err_cap, "stream decode failed");
        return (mr == MA_NOT_IMPLEMENTED || mr == MA_INVALID_FILE)
                   ? MN_ERR_UNSUPPORTED : MN_ERR_OPEN;
    }

    engine->is_url    = MA_TRUE;
    engine->net       = ns;
    engine->has_track = MA_TRUE;
    engine->cursor    = 0;
    engine->at_end    = MA_FALSE;
    engine->pipe_rate = MN_ENGINE_SAMPLE_RATE;
    engine->pipe_channels = MN_ENGINE_CHANNELS;
    engine->downmixed = MA_FALSE;
    engine->src_sample_rate = (uint32_t)engine->decoder.converter.sampleRateIn;
    engine->src_channels    = (uint32_t)engine->decoder.converter.channelsIn;
    engine->src_bits        = mn_bits_from_format(engine->decoder.converter.formatIn);
    engine->bitrate = 0;
    if (engine->stretch != NULL) mn_stretch_reset(engine->stretch);

    strncpy(engine->track_path, url, sizeof(engine->track_path) - 1);
    engine->track_path[sizeof(engine->track_path) - 1] = '\0';

    /* Duration: the RSS hint for podcasts, unknown (0) for live radio.
     * netstream path: NEVER ma_decoder_get_length — for MP3 that scans the
     * whole "file", i.e. downloads the entire episode before playing.
     * MF path: the reader knows the duration from container metadata, so
     * the query is cheap and correct (M4A podcast seek bars work). */
    if (engine->length_hint_ms > 0) {
        engine->length = mn_ms_to_frames((uint64_t)engine->length_hint_ms,
                                         MN_ENGINE_SAMPLE_RATE);
    } else if (ns == NULL) {
        ma_uint64 mlen = 0;
        if (ma_decoder_get_length_in_pcm_frames(&engine->decoder,
                                                &mlen) == MA_SUCCESS) {
            engine->length = (uint64_t)mlen;
        } else {
            engine->length = 0;
        }
    } else {
        engine->length = 0;
    }
    engine->length_hint_ms = 0;

    snprintf(engine->format, sizeof(engine->format), "%s",
             ns == NULL ? "AAC" :
             enc == ma_encoding_format_mp3 ? "MP3" :
             enc == ma_encoding_format_flac ? "FLAC" : "STREAM");

    engine->state = MN_STATE_STOPPED;
    ma_mutex_unlock(&engine->lock);

    if (mn_sync_device_rate(engine) != MN_OK) {
        ma_mutex_lock(&engine->lock);
        mn_unload(engine);
        ma_mutex_unlock(&engine->lock);
        return MN_ERR_DEVICE;
    }
    return MN_OK;
}

/* Diagnostic harness for --nettest2: the exact callback-decoder path
 * mn_engine_load_url takes, minus the device. Prints the raw ma_result. */
int mn_engine_nettest_decode_one(const char *url, int want_icy);
int mn_engine_nettest_decode(const char *url)
{
    int a, b;
    printf("== with ICY metadata ==\n");
    a = mn_engine_nettest_decode_one(url, 1);
    printf("== without ICY metadata ==\n");
    b = mn_engine_nettest_decode_one(url, 0);
    return (a == 0 && b == 0) ? 0 : 1;
}

int mn_engine_nettest_decode_one(const char *url, int want_icy)
{
    char err[256] = {0};
    mn_netstream *ns = mn_netstream_open(url, want_icy ? true : false,
                                         err, sizeof(err));
    ma_decoder dec;
    ma_decoder_config cfg;
    ma_result mr;
    ma_encoding_format enc;

    if (!ns) { printf("OPEN FAILED: %s\n", err); return 1; }
    printf("content-type: %s\n", mn_netstream_content_type(ns));
    enc = mn_net_encoding(mn_netstream_content_type(ns), url);
    printf("encoding pick: %d (0=MF-url 2=flac 3=mp3)\n", (int)enc);

    cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
    if (enc == ma_encoding_format_unknown) {
        /* Mirror the engine: MF streams the URL itself. */
        mn_netstream_close(ns);
        ns = NULL;
        mn_decode_config_apply_mf_url(&cfg, url);
        g_net_trace = 1;
        mr = ma_decoder_init(mn_net_on_read, mn_net_on_seek, NULL, &cfg, &dec);
        g_net_trace = 0;
        printf("decoder init (MF-url): %d (%s)\n", (int)mr,
               mr == MA_SUCCESS ? "OK" : ma_result_description(mr));
        if (mr == MA_SUCCESS) {
            float *buf = (float *)malloc(sizeof(float) * 4096 * 2);
            ma_uint64 frames = 0, total = 0;
            int i;
            for (i = 0; i < 10 && buf; i++) {
                if (ma_decoder_read_pcm_frames(&dec, buf, 4096, &frames)
                        != MA_SUCCESS) break;
                total += frames;
            }
            printf("decoded %llu frames (%.1f s of audio)\n",
                   (unsigned long long)total, (double)total / 44100.0);
            free(buf);
            ma_decoder_uninit(&dec);
        }
        return (mr == MA_SUCCESS) ? 0 : 1;
    }
    printf("buffered: %zu\n", mn_netstream_wait_buffered(ns, 96 * 1024, 10000));
    cfg.encodingFormat = enc;
    g_net_trace = 1;
    mr = ma_decoder_init(mn_net_on_read, mn_net_on_seek, ns, &cfg, &dec);
    g_net_trace = 0;
    printf("decoder init: %d (%s)\n", (int)mr,
           mr == MA_SUCCESS ? "OK" : ma_result_description(mr));
    if (mr == MA_SUCCESS) {
        float *buf = (float *)malloc(sizeof(float) * 4096 * 2);
        ma_uint64 frames = 0, total = 0;
        int i;
        for (i = 0; i < 10 && buf; i++) {
            if (ma_decoder_read_pcm_frames(&dec, buf, 4096, &frames)
                    != MA_SUCCESS) break;
            total += frames;
        }
        printf("decoded %llu frames (%.1f s of audio)\n",
               (unsigned long long)total, (double)total / 44100.0);
        free(buf);
        ma_decoder_uninit(&dec);
    }
    mn_netstream_close(ns);
    return (mr == MA_SUCCESS) ? 0 : 1;
}

int mn_engine_is_stream(mn_engine *engine)
{
    return (engine != NULL && engine->is_url) ? 1 : 0;
}

int mn_engine_stream_seekable(mn_engine *engine)
{
    if (engine == NULL || !engine->is_url) return 0;
    if (engine->net != NULL) return mn_netstream_seekable(engine->net) ? 1 : 0;
    /* MF-URL path: seekable when the container reported a duration. */
    return engine->length > 0 ? 1 : 0;
}

int mn_engine_stream_title(mn_engine *engine, char *out, size_t cap,
                           uint32_t *seq)
{
    if (engine == NULL || engine->net == NULL) return 0;
    return mn_netstream_title(engine->net, out, cap, seq) ? 1 : 0;
}

const char *mn_engine_stream_station(mn_engine *engine)
{
    if (engine == NULL || engine->net == NULL) return "";
    return mn_netstream_station_name(engine->net);
}

mn_result mn_engine_play(mn_engine *engine)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready || !engine->has_track) {
        return MN_ERR_STATE;
    }
    if (engine->state == MN_STATE_PLAYING) {
        return MN_OK; /* idempotent */
    }

    /* If we had reached EOS and the user hits play again, rewind to the start
     * so playback restarts rather than instantly re-ending. */
    ma_mutex_lock(&engine->lock);
    if (engine->at_end) {
        ma_decoder_seek_to_pcm_frame(&engine->decoder, 0);
        engine->cursor = 0;
        engine->at_end = MA_FALSE;
        if (engine->stretch != NULL) mn_stretch_reset(engine->stretch);
    }
    engine->state = MN_STATE_PLAYING;
    ma_mutex_unlock(&engine->lock);

    if (ma_device_start(&engine->device) != MA_SUCCESS) {
        engine->state = MN_STATE_PAUSED;
        return MN_ERR_DEVICE;
    }
    return MN_OK;
}

mn_result mn_engine_pause(mn_engine *engine)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready || !engine->has_track) {
        return MN_ERR_STATE;
    }
    if (engine->state != MN_STATE_PLAYING) {
        return MN_OK; /* already paused or stopped */
    }

    /* Stopping the device halts the callback without moving the cursor, which
     * is exactly pause semantics. */
    engine->state = MN_STATE_PAUSED;
    ma_device_stop(&engine->device);
    return MN_OK;
}

mn_result mn_engine_stop(mn_engine *engine)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready || !engine->has_track) {
        return MN_ERR_STATE;
    }

    engine->state = MN_STATE_STOPPED;
    ma_device_stop(&engine->device);

    /* Rewind to the beginning; the track stays loaded and replayable.
     * LIVE mounts have no beginning — skip the decoder seek (it would
     * fail) and let a resume continue at the live edge. */
    ma_mutex_lock(&engine->lock);
    if (!(engine->is_url && engine->net != NULL &&
          !mn_netstream_seekable(engine->net))) {
        ma_decoder_seek_to_pcm_frame(&engine->decoder, 0);
    }
    engine->cursor = 0;
    engine->at_end = MA_FALSE;
    if (engine->stretch != NULL) mn_stretch_reset(engine->stretch);
    ma_mutex_unlock(&engine->lock);
    return MN_OK;
}

mn_result mn_engine_unload(mn_engine *engine)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (!engine->has_track) {
        return MN_OK; /* nothing loaded — nothing holding a file open */
    }
    /* Halt the callback before tearing the decoder down (mn_unload closes
     * the decoder's underlying file handle, releasing the OS file lock so
     * the tag writer can atomically replace the file). */
    if (engine->device_ready) {
        ma_device_stop(&engine->device);
    }
    ma_mutex_lock(&engine->lock);
    mn_unload(engine);
    ma_mutex_unlock(&engine->lock);
    return MN_OK;
}

bool mn_engine_loaded_path(mn_engine *engine, char *out, size_t n)
{
    bool ok = false;
    if (out == NULL || n == 0) {
        return false;
    }
    out[0] = '\0';
    if (engine == NULL) {
        return false;
    }
    ma_mutex_lock(&engine->lock);
    if (engine->has_track && engine->track_path[0] != '\0') {
        snprintf(out, n, "%s", engine->track_path);
        ok = true;
    }
    ma_mutex_unlock(&engine->lock);
    return ok && out[0] != '\0';
}

mn_result mn_engine_seek_ms(mn_engine *engine, uint64_t position_ms)
{
    uint64_t duration_ms;
    uint64_t frame;

    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    if (!engine->device_ready || !engine->has_track) {
        return MN_ERR_STATE;
    }

    /* Live streams have no timeline — quietly ignore the seek instead of
     * ending the stream via a failed decoder seek. Covers both the
     * netstream path (unseekable mount) and the MF-URL path (length 0). */
    if (engine->is_url && !mn_engine_stream_seekable(engine)) {
        return MN_OK;
    }

    /* Clamp to [0, duration]. */
    duration_ms = mn_engine_duration_ms(engine);
    if (duration_ms != 0 && position_ms > duration_ms) {
        position_ms = duration_ms;
    }
    frame = mn_ms_to_frames(position_ms, engine->pipe_rate);

    ma_mutex_lock(&engine->lock);
    if (ma_decoder_seek_to_pcm_frame(&engine->decoder, frame) != MA_SUCCESS) {
        ma_mutex_unlock(&engine->lock);
        return MN_ERR_SEEK;
    }
    engine->cursor = frame;
    engine->at_end = MA_FALSE;
    /* Drop any stretched-but-unplayed audio from before the seek. */
    if (engine->stretch != NULL) {
        mn_stretch_reset(engine->stretch);
    }
    ma_mutex_unlock(&engine->lock);
    return MN_OK;
}

/* ------------------------------------------------------------------------- */
/* Stem-mixer injection                                                       */
/* ------------------------------------------------------------------------- */

void mn_engine_set_stem_source(mn_engine *engine, mn_stems *stems,
                               bool enabled, bool passthrough)
{
    bool need_retune;

    if (engine == NULL) {
        return;
    }
    /* Publish under the lock so the callback sees a consistent triple. */
    ma_mutex_lock(&engine->lock);
    engine->stems             = stems;
    engine->stems_enabled     = enabled ? MA_TRUE : MA_FALSE;
    engine->stems_passthrough = passthrough ? MA_TRUE : MA_FALSE;
    /* mn_stems_mix operates at the canonical 44100/2ch; if the adaptive
     * pipeline is currently at a different rate OR in multichannel
     * passthrough (>2ch), the track must be retuned to 44100/stereo before
     * the mixer may be consulted. */
    need_retune = (stems != NULL && enabled &&
                   engine->has_track &&
                   (engine->pipe_rate != MN_ENGINE_SAMPLE_RATE ||
                    engine->pipe_channels != MN_ENGINE_CHANNELS));
    ma_mutex_unlock(&engine->lock);

    if (need_retune && engine->device_ready) {
        bool     was_playing = (engine->state == MN_STATE_PLAYING);
        uint64_t pos_ms;

        /* Halt the callback so the decoder/device can be swapped safely. */
        ma_device_stop(&engine->device);
        pos_ms = mn_frames_to_ms(engine->cursor, engine->pipe_rate);

        engine->pipe_channels = MN_ENGINE_CHANNELS;   /* stems are stereo */
        engine->downmixed     = MA_FALSE;
        if (mn_reopen_decoder(engine, MN_ENGINE_SAMPLE_RATE, pos_ms) == MN_OK) {
            (void)mn_sync_device_rate(engine);
        }
        if (was_playing && engine->device_ready && engine->has_track) {
            if (ma_device_start(&engine->device) != MA_SUCCESS) {
                engine->state = MN_STATE_PAUSED;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Volume / gain                                                              */
/* ------------------------------------------------------------------------- */

mn_result mn_engine_set_volume(mn_engine *engine, float volume)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    /* Single aligned float store; read locklessly in the callback. */
    engine->volume = mn_clampf(volume, 0.0f, 1.0f);
    return MN_OK;
}

mn_result mn_engine_set_gain_db(mn_engine *engine, float gain_db)
{
    if (engine == NULL) {
        return MN_ERR_INVALID;
    }
    /* Convert dB to a linear multiplier applied on top of the volume scalar. */
    engine->gain_linear = ma_volume_db_to_linear(gain_db);
    return MN_OK;
}

/* ------------------------------------------------------------------------- */
/* Introspection                                                              */
/* ------------------------------------------------------------------------- */

uint64_t mn_engine_position_ms(const mn_engine *engine)
{
    uint64_t cursor;

    if (engine == NULL || !engine->device_ready || !engine->has_track) {
        return 0;
    }
    /* Plain aligned 64-bit read; the callback advances this on the audio
     * thread. A slightly stale value is fine for a UI position clock. */
    cursor = engine->cursor;
    return mn_frames_to_ms(cursor, engine->pipe_rate);
}

uint64_t mn_engine_duration_ms(const mn_engine *engine)
{
    if (engine == NULL || !engine->device_ready || !engine->has_track) {
        return 0;
    }
    if (engine->length == 0) {
        return 0;
    }
    return mn_frames_to_ms(engine->length, engine->pipe_rate);
}

mn_result mn_engine_get_format(const mn_engine *engine, mn_audio_format *out_format)
{
    if (engine == NULL || out_format == NULL) {
        return MN_ERR_INVALID;
    }

    memset(out_format, 0, sizeof(*out_format));

    if (!engine->device_ready || !engine->has_track) {
        /* No track loaded: zero-filled struct, still success. */
        return MN_OK;
    }

    out_format->src_sample_rate = engine->src_sample_rate;
    out_format->src_channels    = engine->src_channels;
    out_format->src_bits        = engine->src_bits;

    /* Report the REAL hardware-side output format (captured from the device's
     * internal descriptors at init), not the internal float mix format. */
    out_format->out_sample_rate = engine->hw_rate     ? engine->hw_rate
                                                      : engine->pipe_rate;
    out_format->out_channels    = engine->hw_channels ? engine->hw_channels
                                                      : MN_ENGINE_CHANNELS;
    out_format->out_bits        = engine->hw_bits     ? engine->hw_bits : 32u;

    out_format->bitrate         = engine->bitrate;

    /* Actual device state for the output pills: share mode really in use and
     * the device's internal sample format (int vs float PCM). */
    out_format->out_exclusive =
        (engine->device.playback.shareMode == ma_share_mode_exclusive);
    {
        const char *pf = "PCM";
        switch (engine->device.playback.internalFormat) {
            case ma_format_f32: pf = "float"; break;
            case ma_format_s32: pf = "PCM 32"; break;
            case ma_format_s24: pf = "PCM 24"; break;
            case ma_format_s16: pf = "PCM 16"; break;
            case ma_format_u8:  pf = "PCM 8";  break;
            default: break;
        }
        snprintf(out_format->out_pcm, sizeof(out_format->out_pcm), "%s", pf);
    }

    /* Quality-event transparency. */
    out_format->pipe_channels = engine->pipe_channels ? engine->pipe_channels
                                                      : MN_ENGINE_CHANNELS;
    out_format->downmixed     = engine->downmixed ? true : false;
    out_format->rate_limited  =
        (engine->src_sample_rate > 0 && engine->pipe_rate > 0 &&
         engine->pipe_rate < engine->src_sample_rate) ? true : false;

    /* Copy the cached label, guaranteed NUL-terminated and bounded. */
    memcpy(out_format->format, engine->format, sizeof(out_format->format));
    out_format->format[MN_FORMAT_STR_CAP - 1] = '\0';

    return MN_OK;
}

bool mn_engine_get_caps(mn_engine *engine, mn_audio_caps *out)
{
    if (engine == NULL || out == NULL) {
        return false;
    }
    /* Re-probe so a changed default endpoint is picked up; a failed probe
     * (device busy, backend hiccup) keeps the last good snapshot. */
    (void)mn_query_caps(engine);
    if (!engine->caps_valid) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = engine->caps;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Output device enumeration / selection                                      */
/* ------------------------------------------------------------------------- */

int mn_engine_list_devices(mn_engine *engine, mn_audio_device *out, int max)
{
    ma_device_info *infos = NULL;
    ma_uint32       count = 0;
    int             n     = 0;

    if (engine == NULL || out == NULL || max <= 0 || !engine->context_ready) {
        return 0;
    }
    /* The returned array is owned by the context and stays valid until the
     * next enumeration; we copy what we need immediately. Control-thread
     * only, serialized by the app lock like every other engine call. */
    if (ma_context_get_devices(&engine->context, &infos, &count, NULL, NULL)
            != MA_SUCCESS || infos == NULL) {
        return 0;
    }
    for (ma_uint32 i = 0; i < count && n < max; ++i) {
        strncpy(out[n].name, infos[i].name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].is_default = infos[i].isDefault ? true : false;
        n++;
    }
    return n;
}

bool mn_engine_select_device(mn_engine *engine, int index)
{
    ma_device_info *infos = NULL;
    ma_uint32       count = 0;
    bool            was_playing;

    if (engine == NULL || !engine->context_ready || index < 0) {
        return false;
    }
    if (ma_context_get_devices(&engine->context, &infos, &count, NULL, NULL)
            != MA_SUCCESS || infos == NULL || (ma_uint32)index >= count) {
        return false;
    }

    /* Snapshot the target id BEFORE touching the device (the info array is
     * context-owned and independent of the device, but be conservative). */
    ma_device_id target = infos[index].id;

    was_playing = (engine->state == MN_STATE_PLAYING);

    /* Tear down the current device. The decoder, cursor and transport state
     * are untouched, so the playback position is preserved across the swap;
     * uninit stops the device first, halting the audio callback. */
    if (engine->device_ready) {
        ma_device_uninit(&engine->device);
        engine->device_ready = MA_FALSE;
    }

    engine->selected_id    = target;
    engine->use_selected   = MA_TRUE;
    engine->selected_index = index;

    if (mn_device_open(engine, engine->pipe_rate) != MN_OK) {
        /* Chosen endpoint refused: fall back to the system default so audio
         * keeps working, and report failure. */
        engine->use_selected   = MA_FALSE;
        engine->selected_index = -1;
        if (mn_device_open(engine, engine->pipe_rate) == MN_OK &&
            was_playing && engine->has_track) {
            if (ma_device_start(&engine->device) != MA_SUCCESS) {
                engine->state = MN_STATE_PAUSED;
            }
        }
        return false;
    }

    if (was_playing && engine->has_track) {
        if (ma_device_start(&engine->device) != MA_SUCCESS) {
            engine->state = MN_STATE_PAUSED;
            return false;
        }
    }
    return true;
}

int mn_engine_selected_device(const mn_engine *engine)
{
    if (engine == NULL) {
        return -1;
    }
    return engine->use_selected ? engine->selected_index : -1;
}

mn_play_state mn_engine_state(const mn_engine *engine)
{
    if (engine == NULL) {
        return MN_STATE_STOPPED;
    }
    return engine->state;
}

bool mn_engine_finished(const mn_engine *engine)
{
    if (engine == NULL || !engine->device_ready || !engine->has_track) {
        return false;
    }
    return engine->at_end ? true : false;
}

/* ------------------------------------------------------------------------- */
/* Decode shim (standalone; no engine/device)                                 */
/* ------------------------------------------------------------------------- */

mn_result mn_decode_44100_stereo_ex(const char *path,
                                    float **out_L,
                                    float **out_R,
                                    uint64_t *out_frames,
                                    mn_decode_abort_fn should_abort,
                                    void *abort_user)
{
    ma_decoder_config cfg;
    ma_decoder        decoder;
    ma_result         mr;
    ma_uint64         total_frames = 0;
    float            *interleaved  = NULL;
    float            *left         = NULL;
    float            *right        = NULL;
    ma_uint64         frames_read  = 0;
    mn_result         rc;

    /* Chunk granularity for the abort poll: 1 second of output audio. */
    const ma_uint64 chunk_frames = MN_ENGINE_SAMPLE_RATE;

    /* Validate outputs and pre-clear them so failure paths are clean. */
    if (out_L == NULL || out_R == NULL || out_frames == NULL) {
        return MN_ERR_INVALID;
    }
    *out_L      = NULL;
    *out_R      = NULL;
    *out_frames = 0;

    if (path == NULL || path[0] == '\0') {
        return MN_ERR_INVALID;
    }

    /* Force decode + resample to canonical 44100 Hz stereo float32. */
    cfg = ma_decoder_config_init(ma_format_f32,
                                 MN_ENGINE_CHANNELS,
                                 MN_ENGINE_SAMPLE_RATE);
    mn_decode_config_apply_backends(&cfg);

    mr = mn_decoder_init_path(path, &cfg, &decoder);
    if (mr != MA_SUCCESS) {
        if (mr == MA_NOT_IMPLEMENTED) {
            return MN_ERR_UNSUPPORTED;
        }
        return MN_ERR_OPEN;
    }

    /*
     * Determine the total length in (output-rate) frames. Some sources are not
     * length-queryable; in that case we cannot preallocate, so treat it as an
     * I/O error rather than looping unbounded.
     */
    mr = ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
    if (mr != MA_SUCCESS || total_frames == 0) {
        ma_decoder_uninit(&decoder);
        return MN_ERR_IO;
    }

    /* Guard against overflow in the interleaved allocation size. */
    if (total_frames > (SIZE_MAX / (sizeof(float) * MN_ENGINE_CHANNELS))) {
        ma_decoder_uninit(&decoder);
        return MN_ERR_NOMEM;
    }

    interleaved = (float *)malloc((size_t)total_frames *
                                  MN_ENGINE_CHANNELS * sizeof(float));
    left  = (float *)malloc((size_t)total_frames * sizeof(float));
    right = (float *)malloc((size_t)total_frames * sizeof(float));
    if (interleaved == NULL || left == NULL || right == NULL) {
        rc = MN_ERR_NOMEM;
        goto fail;
    }

    /*
     * Read the file in ~1 s chunks, polling the abort predicate between
     * chunks so a cancelled stem job stops decoding within one chunk. A short
     * chunk read signals EOF; the frames actually read (<= total_frames)
     * become the true frame count.
     */
    while (frames_read < total_frames) {
        ma_uint64 want = total_frames - frames_read;
        ma_uint64 got  = 0;

        if (should_abort != NULL && should_abort(abort_user)) {
            rc = MN_ERR_STATE;
            goto fail;
        }
        if (want > chunk_frames) {
            want = chunk_frames;
        }
        mr = ma_decoder_read_pcm_frames(
                 &decoder,
                 interleaved + (size_t)frames_read * MN_ENGINE_CHANNELS,
                 want, &got);
        frames_read += got;
        if (mr != MA_SUCCESS || got < want) {
            break; /* EOF, or decode error after a partial read */
        }
    }
    if (frames_read == 0) {
        rc = MN_ERR_IO;
        goto fail;
    }

    /* Deinterleave L/R planar. */
    {
        ma_uint64 i;
        for (i = 0; i < frames_read; ++i) {
            left[i]  = interleaved[i * 2u + 0u];
            right[i] = interleaved[i * 2u + 1u];
        }
    }

    free(interleaved);
    ma_decoder_uninit(&decoder);

    *out_L      = left;
    *out_R      = right;
    *out_frames = (uint64_t)frames_read;
    return MN_OK;

fail:
    free(interleaved);
    free(left);
    free(right);
    ma_decoder_uninit(&decoder);
    return rc;
}

mn_result mn_decode_44100_stereo(const char *path,
                                 float **out_L,
                                 float **out_R,
                                 uint64_t *out_frames)
{
    return mn_decode_44100_stereo_ex(path, out_L, out_R, out_frames,
                                     NULL, NULL);
}

void mn_free_samples(float *samples)
{
    /* free(NULL) is a no-op, but be explicit for the documented contract. */
    if (samples != NULL) {
        free(samples);
    }
}

/* ------------------------------------------------------------------------- */
/* Waveform peaks (standalone; no engine/device)                              */
/* ------------------------------------------------------------------------- */

int mn_engine_waveform(const char *path, int bars, float *out_peaks)
{
    ma_decoder_config cfg;
    ma_decoder        decoder;
    ma_result         mr;
    ma_uint64         total_frames = 0;
    float             chunk[MN_ENGINE_CHUNK_FRAMES * MN_ENGINE_CHANNELS];
    ma_uint64         frame_index = 0;
    double            frames_per_bar;
    int               i;

    if (path == NULL || path[0] == '\0' || out_peaks == NULL || bars <= 0) {
        return 0;
    }
    for (i = 0; i < bars; ++i) {
        out_peaks[i] = 0.0f;
    }

    /* Decode + resample to canonical 44100 Hz stereo float32. */
    cfg = ma_decoder_config_init(ma_format_f32, MN_ENGINE_CHANNELS,
                                 MN_ENGINE_SAMPLE_RATE);
    mn_decode_config_apply_backends(&cfg);
    if (mn_decoder_init_path(path, &cfg, &decoder) != MA_SUCCESS) {
        return 0;
    }

    if (ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames) != MA_SUCCESS ||
        total_frames == 0) {
        ma_decoder_uninit(&decoder);
        return 0;
    }

    frames_per_bar = (double)total_frames / (double)bars;
    if (frames_per_bar < 1.0) {
        frames_per_bar = 1.0;
    }

    /* Stream the file in chunks, accumulating the per-bucket peak. */
    for (;;) {
        ma_uint64 read = 0;
        ma_uint64 f;

        mr = ma_decoder_read_pcm_frames(&decoder, chunk,
                                        MN_ENGINE_CHUNK_FRAMES, &read);
        if (read == 0) {
            break;
        }
        for (f = 0; f < read; ++f) {
            float l = chunk[f * 2u + 0u];
            float r = chunk[f * 2u + 1u];
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            int   b = (int)((double)(frame_index + f) / frames_per_bar);
            if (b < 0) {
                b = 0;
            }
            if (b >= bars) {
                b = bars - 1;
            }
            if (a > out_peaks[b]) {
                out_peaks[b] = a;
            }
        }
        frame_index += read;
        if (mr != MA_SUCCESS) {
            break; /* EOS or error after a short read */
        }
    }

    ma_decoder_uninit(&decoder);

    /* Clamp to [0,1] (float32 sources can exceed unity). */
    for (i = 0; i < bars; ++i) {
        if (out_peaks[i] > 1.0f) {
            out_peaks[i] = 1.0f;
        }
    }
    return bars;
}
