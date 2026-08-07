/*
 * mf_decode.c — Universal audio decode backends for miniaudio (Windows).
 *
 * See mf_decode.h for the design overview. This file compiles the two custom
 * ma_decoding_backend_vtable implementations:
 *
 *   - Media Foundation (IMFSourceReader) catch-all → PCM float32.
 *   - ffmpeg-subprocess fallback (transcode to a temp WAV, decode with the
 *     miniaudio WAV backend).
 *
 * Both are wrapped as ma_data_source objects. miniaudio pulls PCM frames from
 * our onRead; we translate to MF ReadSample calls (or, for ffmpeg, to a nested
 * ma_decoder over the transcoded WAV).
 *
 * IMPORTANT: neither backend claims an extension miniaudio already decodes
 * natively (flac/mp3/wav/ogg/oga). Since miniaudio tries custom backends before
 * its own extension dispatch, declining those keeps the known-good path intact.
 */

#include "mf_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfobjects.h>
#include <shlwapi.h>
#include <propvarutil.h>

/* ------------------------------------------------------------------------- */
/* Extension policy                                                           */
/* ------------------------------------------------------------------------- */

/* Lower-cased file extension (without the dot) into buf; returns buf. */
static const char *mn_ext_of(const char *path, char *buf, size_t cap)
{
    const char *dot = NULL;
    const char *p;
    size_t i;
    buf[0] = '\0';
    if (path == NULL) return buf;
    for (p = path; *p; ++p) {
        if (*p == '.') dot = p;
        else if (*p == '/' || *p == '\\') dot = NULL;
    }
    if (dot == NULL) return buf;
    dot++;
    for (i = 0; dot[i] && i + 1 < cap; ++i) {
        buf[i] = (char)tolower((unsigned char)dot[i]);
    }
    buf[i] = '\0';
    return buf;
}

/* Extensions the MF backend must decline so miniaudio's own path takes them.
 * This includes .ogg/.oga: MF's Vorbis support is unreliable, and if miniaudio
 * was built WITH stb_vorbis it decodes them; if not, the ffmpeg fallback picks
 * them up (see mn_ext_ffmpeg_decline). Either way MF should keep its hands off. */
static int mn_ext_is_native_miniaudio(const char *ext)
{
    return (strcmp(ext, "flac") == 0 ||
            strcmp(ext, "mp3")  == 0 ||
            strcmp(ext, "wav")  == 0 ||
            strcmp(ext, "wave") == 0 ||
            strcmp(ext, "ogg")  == 0 ||   /* stb_vorbis (if built) / ffmpeg */
            strcmp(ext, "oga")  == 0);
}

/* Extensions the ffmpeg fallback must decline: only the formats miniaudio
 * GENUINELY decodes on its own in this build (FLAC/MP3/WAV). Everything else —
 * including Ogg-Vorbis (this build ships without stb_vorbis) and Ogg-Opus — is
 * fair game for the ffmpeg transcode fallback when the MF backend also passed.
 * Keeping this list narrow (not the full native list) is what makes .ogg
 * playable here without vendoring an extra codec. */
static int mn_ext_ffmpeg_decline(const char *ext)
{
    return (strcmp(ext, "flac") == 0 ||
            strcmp(ext, "mp3")  == 0 ||
            strcmp(ext, "wav")  == 0 ||
            strcmp(ext, "wave") == 0);
}

/* Extensions the MF backend is willing to try. MF on Win10/11 handles these
 * (subject to installed codecs). We keep an explicit allow-list rather than
 * "everything non-native" so a bogus file with an audio-ish extension gets a
 * clean decline instead of a slow MF probe on, say, a .txt. Ogg-Opus (.opus)
 * is listed: MF handles it on 1607+, and if it fails the ffmpeg backend covers
 * it. */
static int mn_ext_mf_candidate(const char *ext)
{
    static const char *ok[] = {
        "m4a", "m4b", "m4r", "aac", "adts", "mp4", "m4p",
        "alac", "wma", "asf", "ac3", "eac3", "amr", "3gp", "3g2",
        "aif", "aiff", "aifc", "caf", "opus", "mka", "mov",
        NULL
    };
    int i;
    for (i = 0; ok[i]; ++i) if (strcmp(ext, ok[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Media Foundation lifecycle                                                 */
/* ------------------------------------------------------------------------- */

static LONG  g_mf_started = 0;   /* MFStartup done (ref-ish; simple flag)     */
static CRITICAL_SECTION g_mf_lock;
static LONG  g_mf_lock_init = 0;

static void mn_mf_ensure_lock(void)
{
    if (InterlockedCompareExchange(&g_mf_lock_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_mf_lock);
    }
}

static HRESULT mn_mf_ensure_started(void)
{
    HRESULT hr = S_OK;
    mn_mf_ensure_lock();
    EnterCriticalSection(&g_mf_lock);
    if (g_mf_started == 0) {
        /* COM may already be initialized by the host (CEF/ole32). MFStartup
         * does not require a specific apartment for the source reader. */
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(hr)) g_mf_started = 1;
    }
    LeaveCriticalSection(&g_mf_lock);
    return hr;
}

void mn_decode_backends_init(void)
{
    (void)mn_mf_ensure_started();
}

void mn_decode_backends_shutdown(void)
{
    mn_mf_ensure_lock();
    EnterCriticalSection(&g_mf_lock);
    if (g_mf_started) {
        MFShutdown();
        g_mf_started = 0;
    }
    LeaveCriticalSection(&g_mf_lock);
}

/* ------------------------------------------------------------------------- */
/* Media Foundation data source                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    ma_data_source_base ds;      /* MUST be first                             */
    IMFSourceReader    *reader;
    ma_format           format;  /* always ma_format_f32                      */
    ma_uint32           channels;
    ma_uint32           sampleRate;
    ma_uint64           cursor;      /* output frames delivered so far        */
    ma_uint64           length;      /* total output frames (0 if unknown)    */

    /* Leftover PCM from the last IMFSample that didn't fit the caller's read
     * request; f32 interleaved. */
    ma_uint8           *residual;    /* malloc'd buffer                       */
    size_t              residual_cap;
    size_t              residual_len;   /* valid bytes                        */
    size_t              residual_pos;   /* consumed bytes                     */

    ma_bool32           at_end;
    ma_allocation_callbacks alloc;
} mn_mf_ds;

static ma_uint32 mn_mf_bytes_per_frame(mn_mf_ds *d)
{
    return d->channels * (ma_uint32)sizeof(float);
}

/* Configure the reader to deliver 32-bit float PCM. Returns the negotiated
 * channel count / sample rate. */
static HRESULT mn_mf_configure(IMFSourceReader *reader,
                               ma_uint32 *outChannels,
                               ma_uint32 *outRate)
{
    HRESULT hr;
    IMFMediaType *want = NULL;
    IMFMediaType *actual = NULL;
    UINT32 ch = 0, sr = 0;

    /* Deselect all streams, then select only the first audio stream. */
    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    hr = MFCreateMediaType(&want);
    if (FAILED(hr)) return hr;

    /* Ask MF to convert to uncompressed float PCM. We do NOT pin channels or
     * sample rate — the AudioResampler/decoder picks the source-native layout,
     * which miniaudio then resamples to the pipeline rate. This preserves
     * quality and avoids an MF resample we don't need. */
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float);

    hr = IMFSourceReader_SetCurrentMediaType(reader,
             (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
    if (FAILED(hr)) {
        /* Some decoders won't emit float directly; fall back to PCM 16-bit is
         * NOT attempted here because miniaudio expects a single fixed output
         * format from us. Float is supported by the MF AudioResampler for all
         * mainstream codecs, so a failure here means truly unsupported. */
        IMFMediaType_Release(want);
        return hr;
    }

    /* Read back the actual negotiated type to learn ch / sr. */
    hr = IMFSourceReader_GetCurrentMediaType(reader,
             (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    if (SUCCEEDED(hr)) {
        IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        IMFMediaType_Release(actual);
    }
    IMFMediaType_Release(want);

    if (ch == 0) ch = 2;
    if (sr == 0) sr = 44100;
    *outChannels = ch;
    *outRate = sr;

    /* Ensure the stream is selected for reading. */
    IMFSourceReader_SetStreamSelection(reader,
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    return S_OK;
}

/* Query total duration → output frames. Best-effort (0 if unknown). */
static ma_uint64 mn_mf_query_length(IMFSourceReader *reader, ma_uint32 rate)
{
    PROPVARIANT var;
    ma_uint64 frames = 0;
    HRESULT hr;
    PropVariantInit(&var);
    hr = IMFSourceReader_GetPresentationAttribute(reader,
            (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
        /* Duration is in 100-ns units. */
        double seconds = (double)var.uhVal.QuadPart / 1.0e7;
        frames = (ma_uint64)(seconds * (double)rate + 0.5);
    }
    PropVariantClear(&var);
    return frames;
}

/* Pull the next decoded IMFSample into the residual buffer. Sets at_end on EOS.
 * Returns MA_SUCCESS if at least some data was buffered OR at_end was reached
 * cleanly. */
static ma_result mn_mf_fill_residual(mn_mf_ds *d)
{
    HRESULT hr;
    DWORD flags = 0;
    IMFSample *sample = NULL;
    LONGLONG ts = 0;

    d->residual_pos = 0;
    d->residual_len = 0;

    for (;;) {
        hr = IMFSourceReader_ReadSample(d->reader,
                (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0, NULL, &flags, &ts, &sample);
        if (FAILED(hr)) {
            d->at_end = MA_TRUE;
            return MA_ERROR;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            d->at_end = MA_TRUE;
            if (sample) IMFSample_Release(sample);
            return MA_SUCCESS; /* no data, clean EOS */
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            /* Renegotiate ch/sr; keep going. */
            ma_uint32 ch = d->channels, sr = d->sampleRate;
            IMFMediaType *actual = NULL;
            if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(d->reader,
                    (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) && actual) {
                UINT32 v = 0;
                if (SUCCEEDED(IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_NUM_CHANNELS, &v)) && v) ch = v;
                v = 0;
                if (SUCCEEDED(IMFMediaType_GetUINT32(actual, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) && v) sr = v;
                IMFMediaType_Release(actual);
            }
            d->channels = ch;
            d->sampleRate = sr;
        }
        if (sample == NULL) {
            /* Gap / no sample this call; try again. */
            continue;
        }

        /* Copy the contiguous buffer out. */
        {
            IMFMediaBuffer *mbuf = NULL;
            BYTE *pdata = NULL;
            DWORD curLen = 0;
            hr = IMFSample_ConvertToContiguousBuffer(sample, &mbuf);
            if (SUCCEEDED(hr) && mbuf) {
                hr = IMFMediaBuffer_Lock(mbuf, &pdata, NULL, &curLen);
                if (SUCCEEDED(hr)) {
                    if (curLen > 0) {
                        if (curLen > d->residual_cap) {
                            void *nb = ma_realloc(d->residual, curLen, &d->alloc);
                            if (nb == NULL) {
                                IMFMediaBuffer_Unlock(mbuf);
                                IMFMediaBuffer_Release(mbuf);
                                IMFSample_Release(sample);
                                return MA_OUT_OF_MEMORY;
                            }
                            d->residual = (ma_uint8 *)nb;
                            d->residual_cap = curLen;
                        }
                        memcpy(d->residual, pdata, curLen);
                        d->residual_len = curLen;
                    }
                    IMFMediaBuffer_Unlock(mbuf);
                }
                IMFMediaBuffer_Release(mbuf);
            }
            IMFSample_Release(sample);
        }

        if (d->residual_len > 0) {
            return MA_SUCCESS;
        }
        /* Zero-length sample: loop and read the next one. */
    }
}

static ma_result mn_mf_ds_read(ma_data_source *pDataSource, void *pFramesOut,
                               ma_uint64 frameCount, ma_uint64 *pFramesRead)
{
    mn_mf_ds *d = (mn_mf_ds *)pDataSource;
    ma_uint32 bpf = mn_mf_bytes_per_frame(d);
    ma_uint8 *out = (ma_uint8 *)pFramesOut;
    ma_uint64 framesDone = 0;

    if (pFramesRead) *pFramesRead = 0;
    if (bpf == 0) return MA_ERROR;

    while (framesDone < frameCount) {
        size_t avail = d->residual_len - d->residual_pos;
        if (avail == 0) {
            if (d->at_end) break;
            {
                ma_result r = mn_mf_fill_residual(d);
                if (r != MA_SUCCESS) {
                    if (framesDone > 0) break;
                    return r;
                }
                avail = d->residual_len - d->residual_pos;
                if (avail == 0) {
                    /* EOS with no data. */
                    break;
                }
            }
        }
        {
            ma_uint64 framesWanted = frameCount - framesDone;
            ma_uint64 framesAvail = avail / bpf;
            ma_uint64 n = (framesWanted < framesAvail) ? framesWanted : framesAvail;
            size_t bytes = (size_t)(n * bpf);
            if (out) {
                memcpy(out + (size_t)(framesDone * bpf),
                       d->residual + d->residual_pos, bytes);
            }
            d->residual_pos += bytes;
            framesDone += n;
        }
    }

    d->cursor += framesDone;
    if (pFramesRead) *pFramesRead = framesDone;
    if (framesDone == 0 && d->at_end) return MA_AT_END;
    return MA_SUCCESS;
}

static ma_result mn_mf_ds_seek(ma_data_source *pDataSource, ma_uint64 frameIndex)
{
    mn_mf_ds *d = (mn_mf_ds *)pDataSource;
    PROPVARIANT var;
    HRESULT hr;
    LONGLONG hns;

    /* Convert output-frame index to 100-ns units. */
    hns = (LONGLONG)(((double)frameIndex / (double)d->sampleRate) * 1.0e7 + 0.5);

    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = hns;
    hr = IMFSourceReader_SetCurrentPosition(d->reader, &GUID_NULL, &var);
    PropVariantClear(&var);
    if (FAILED(hr)) return MA_ERROR;

    d->residual_pos = 0;
    d->residual_len = 0;
    d->at_end = MA_FALSE;
    d->cursor = frameIndex;
    return MA_SUCCESS;
}

static ma_result mn_mf_ds_get_data_format(ma_data_source *pDataSource,
        ma_format *pFormat, ma_uint32 *pChannels, ma_uint32 *pSampleRate,
        ma_channel *pChannelMap, size_t channelMapCap)
{
    mn_mf_ds *d = (mn_mf_ds *)pDataSource;
    if (pFormat)     *pFormat = d->format;
    if (pChannels)   *pChannels = d->channels;
    if (pSampleRate) *pSampleRate = d->sampleRate;
    if (pChannelMap) {
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     pChannelMap, channelMapCap, d->channels);
    }
    return MA_SUCCESS;
}

static ma_result mn_mf_ds_get_cursor(ma_data_source *pDataSource, ma_uint64 *pCursor)
{
    mn_mf_ds *d = (mn_mf_ds *)pDataSource;
    if (pCursor) *pCursor = d->cursor;
    return MA_SUCCESS;
}

static ma_result mn_mf_ds_get_length(ma_data_source *pDataSource, ma_uint64 *pLength)
{
    mn_mf_ds *d = (mn_mf_ds *)pDataSource;
    if (pLength) *pLength = d->length;
    return (d->length > 0) ? MA_SUCCESS : MA_NOT_IMPLEMENTED;
}

static ma_data_source_vtable g_mn_mf_ds_vtable = {
    mn_mf_ds_read,
    mn_mf_ds_seek,
    mn_mf_ds_get_data_format,
    mn_mf_ds_get_cursor,
    mn_mf_ds_get_length,
    NULL,   /* onSetLooping */
    0
};

/* Convert a UTF-8 path to a freshly-allocated wide string. Caller frees. */
static wchar_t *mn_utf8_to_wide(const char *s)
{
    int n;
    wchar_t *w;
    if (s == NULL) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

static ma_result mn_mf_init_common(const wchar_t *wpath,
                                   const ma_decoding_backend_config *pConfig,
                                   const ma_allocation_callbacks *pAlloc,
                                   ma_data_source **ppBackend)
{
    HRESULT hr;
    IMFSourceReader *reader = NULL;
    IMFAttributes *attrs = NULL;
    mn_mf_ds *d = NULL;
    ma_data_source_config dsc;
    ma_uint32 ch = 0, sr = 0;
    ma_result rc;

    (void)pConfig;

    if (FAILED(mn_mf_ensure_started())) return MA_NO_BACKEND;

    /* Ask the reader to use hardware/robust decoding where possible. */
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1))) {
        IMFAttributes_SetUINT32(attrs, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
    }

    hr = MFCreateSourceReaderFromURL(wpath, attrs, &reader);
    if (attrs) IMFAttributes_Release(attrs);
    if (FAILED(hr) || reader == NULL) {
        return MA_NO_BACKEND;
    }

    if (FAILED(mn_mf_configure(reader, &ch, &sr))) {
        IMFSourceReader_Release(reader);
        return MA_NO_BACKEND;
    }

    d = (mn_mf_ds *)ma_malloc(sizeof(*d), pAlloc);
    if (d == NULL) {
        IMFSourceReader_Release(reader);
        return MA_OUT_OF_MEMORY;
    }
    memset(d, 0, sizeof(*d));
    if (pAlloc) d->alloc = *pAlloc;

    dsc = ma_data_source_config_init();
    dsc.vtable = &g_mn_mf_ds_vtable;
    rc = ma_data_source_init(&dsc, &d->ds);
    if (rc != MA_SUCCESS) {
        ma_free(d, pAlloc);
        IMFSourceReader_Release(reader);
        return rc;
    }

    d->reader     = reader;
    d->format     = ma_format_f32;
    d->channels   = ch;
    d->sampleRate = sr;
    d->cursor     = 0;
    d->length     = mn_mf_query_length(reader, sr);
    d->at_end     = MA_FALSE;

    *ppBackend = (ma_data_source *)d;
    return MA_SUCCESS;
}

static ma_result mn_mf_onInitFile(void *pUserData, const char *pFilePath,
        const ma_decoding_backend_config *pConfig,
        const ma_allocation_callbacks *pAlloc, ma_data_source **ppBackend)
{
    char ext[32];
    wchar_t *wpath;
    ma_result rc;
    (void)pUserData;

    mn_ext_of(pFilePath, ext, sizeof(ext));
    if (mn_ext_is_native_miniaudio(ext)) return MA_NO_BACKEND;
    if (!mn_ext_mf_candidate(ext))       return MA_NO_BACKEND;

    wpath = mn_utf8_to_wide(pFilePath);
    if (wpath == NULL) return MA_NO_BACKEND;
    rc = mn_mf_init_common(wpath, pConfig, pAlloc, ppBackend);
    free(wpath);
    return rc;
}

static ma_result mn_mf_onInitFileW(void *pUserData, const wchar_t *pFilePath,
        const ma_decoding_backend_config *pConfig,
        const ma_allocation_callbacks *pAlloc, ma_data_source **ppBackend)
{
    char ext[32];
    char utf8[8]; /* only need the extension; convert a bounded tail */
    (void)pUserData;
    (void)utf8;

    /* Derive extension from the wide path directly. */
    {
        const wchar_t *dot = NULL, *p;
        size_t i;
        for (p = pFilePath; *p; ++p) {
            if (*p == L'.') dot = p;
            else if (*p == L'/' || *p == L'\\') dot = NULL;
        }
        ext[0] = '\0';
        if (dot) {
            dot++;
            for (i = 0; dot[i] && i + 1 < sizeof(ext); ++i)
                ext[i] = (char)tolower((int)dot[i]);
            ext[i] = '\0';
        }
    }
    if (mn_ext_is_native_miniaudio(ext)) return MA_NO_BACKEND;
    if (!mn_ext_mf_candidate(ext))       return MA_NO_BACKEND;

    return mn_mf_init_common(pFilePath, pConfig, pAlloc, ppBackend);
}

static void mn_mf_onUninit(void *pUserData, ma_data_source *pBackend,
                           const ma_allocation_callbacks *pAlloc)
{
    mn_mf_ds *d = (mn_mf_ds *)pBackend;
    (void)pUserData;
    if (d == NULL) return;
    if (d->reader) IMFSourceReader_Release(d->reader);
    if (d->residual) ma_free(d->residual, &d->alloc);
    ma_data_source_uninit(&d->ds);
    ma_free(d, pAlloc);
}

ma_decoding_backend_vtable g_mn_decoding_backend_mf = {
    NULL,               /* onInit (stream) — not supported; file only        */
    mn_mf_onInitFile,
    mn_mf_onInitFileW,
    NULL,               /* onInitMemory                                       */
    mn_mf_onUninit
};

/* ---- MF-over-URL backend (AAC/M4A internet streams + episodes) ---------
 * Media Foundation's source resolver speaks http(s) natively (progressive
 * download, byte-range seek, ADTS/MP4 demux) — everything the callback
 * decoder path cannot do for AAC. ma_decoder has no URL init, so the URL
 * rides a one-shot side channel: mn_decode_config_apply_mf_url() stashes
 * it and installs this vtable; its onInit IGNORES the read callbacks and
 * opens the stashed URL directly. Engine loads are serialized on the
 * control thread, so the single pending slot cannot race. */
static char g_mf_pending_url[2048];

static ma_result mn_mf_onInitStreamURL(void *pUserData,
        ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
        void *pReadSeekTellUserData,
        const ma_decoding_backend_config *pConfig,
        const ma_allocation_callbacks *pAlloc, ma_data_source **ppBackend)
{
    wchar_t *w;
    ma_result rc;
    (void)pUserData; (void)onRead; (void)onSeek; (void)onTell;
    (void)pReadSeekTellUserData;
    if (g_mf_pending_url[0] == '\0') return MA_NO_BACKEND;
    w = mn_utf8_to_wide(g_mf_pending_url);
    if (w == NULL) return MA_NO_BACKEND;
    rc = mn_mf_init_common(w, pConfig, pAlloc, ppBackend);
    free(w);
    return rc;
}

static ma_decoding_backend_vtable g_mn_backend_mf_url = {
    mn_mf_onInitStreamURL,
    NULL, NULL, NULL,
    mn_mf_onUninit
};
static ma_decoding_backend_vtable *g_mf_url_chain[] = { &g_mn_backend_mf_url };

void mn_decode_config_apply_mf_url(ma_decoder_config *cfg, const char *url)
{
    snprintf(g_mf_pending_url, sizeof(g_mf_pending_url), "%s", url);
    cfg->ppCustomBackendVTables = g_mf_url_chain;
    cfg->customBackendCount     = 1;
    cfg->pCustomBackendUserData = NULL;
}

/* ------------------------------------------------------------------------- */
/* ffmpeg fallback backend                                                    */
/*                                                                            */
/* For formats neither miniaudio nor MF handles. Transcodes the source to a   */
/* temp WAV via a bundled/PATH ffmpeg.exe, then decodes that WAV with a       */
/* nested ma_decoder (WAV backend). The nested decoder IS the data source we  */
/* hand back — we just extend it with a temp-file path we delete on uninit.   */
/* ------------------------------------------------------------------------- */

typedef struct {
    ma_data_source_base ds;   /* first; but we proxy to inner decoder         */
    ma_decoder inner;         /* decodes the transcoded WAV                   */
    wchar_t   *tmpfile;       /* wide path to delete on uninit                */
    ma_allocation_callbacks alloc;
} mn_ff_ds;

/* Proxy data-source ops to the inner WAV decoder. */
static ma_result mn_ff_read(ma_data_source *p, void *out, ma_uint64 n, ma_uint64 *rd)
{ mn_ff_ds *d = (mn_ff_ds *)p; return ma_data_source_read_pcm_frames(&d->inner, out, n, rd); }
static ma_result mn_ff_seek(ma_data_source *p, ma_uint64 i)
{ mn_ff_ds *d = (mn_ff_ds *)p; return ma_data_source_seek_to_pcm_frame(&d->inner, i); }
static ma_result mn_ff_fmt(ma_data_source *p, ma_format *f, ma_uint32 *c, ma_uint32 *s, ma_channel *m, size_t mc)
{ mn_ff_ds *d = (mn_ff_ds *)p; return ma_data_source_get_data_format(&d->inner, f, c, s, m, mc); }
static ma_result mn_ff_cur(ma_data_source *p, ma_uint64 *c)
{ mn_ff_ds *d = (mn_ff_ds *)p; return ma_data_source_get_cursor_in_pcm_frames(&d->inner, c); }
static ma_result mn_ff_len(ma_data_source *p, ma_uint64 *l)
{ mn_ff_ds *d = (mn_ff_ds *)p; return ma_data_source_get_length_in_pcm_frames(&d->inner, l); }

static ma_data_source_vtable g_mn_ff_ds_vtable = {
    mn_ff_read, mn_ff_seek, mn_ff_fmt, mn_ff_cur, mn_ff_len, NULL, 0
};

/* Locate an ffmpeg executable: prefer one bundled beside our exe, else PATH.
 * Returns a wide command-usable path in `out` (quoted-safe caller). */
static int mn_find_ffmpeg(wchar_t *out, size_t cap)
{
    wchar_t exedir[MAX_PATH];
    DWORD n;

    /* 1) beside the running exe: <exedir>\ffmpeg.exe */
    n = GetModuleFileNameW(NULL, exedir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(exedir, L'\\');
        if (slash) {
            *slash = 0;
            _snwprintf(out, cap, L"%s\\ffmpeg.exe", exedir);
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }

    /* 2) PATH lookup via SearchPath. */
    n = SearchPathW(NULL, L"ffmpeg.exe", NULL, (DWORD)cap, out, NULL);
    if (n > 0 && n < cap) return 1;

    /* 3) common WinGet Links shim. */
    {
        wchar_t up[MAX_PATH];
        DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", up, MAX_PATH);
        if (m > 0 && m < MAX_PATH) {
            _snwprintf(out, cap, L"%s\\Microsoft\\WinGet\\Links\\ffmpeg.exe", up);
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    return 0;
}

static ma_result mn_ff_onInitFile(void *pUserData, const char *pFilePath,
        const ma_decoding_backend_config *pConfig,
        const ma_allocation_callbacks *pAlloc, ma_data_source **ppBackend)
{
    char ext[32];
    wchar_t ffmpeg[MAX_PATH];
    wchar_t *wsrc = NULL;
    wchar_t tmpdir[MAX_PATH];
    wchar_t tmpfile[MAX_PATH];
    wchar_t *cmd = NULL;
    size_t cmdcap;
    mn_ff_ds *d = NULL;
    ma_decoder_config dcfg;
    ma_data_source_config dsc;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD wait;
    ma_result rc;
    (void)pUserData;

    mn_ext_of(pFilePath, ext, sizeof(ext));
    /* Decline only what miniaudio GENUINELY decodes on its own (FLAC/MP3/WAV).
     * MF-candidate extensions were already tried by the MF backend (registered
     * first); if MF failed for those we still allow ffmpeg as a last resort.
     * Ogg-Vorbis/Opus reach here because this build has no stb_vorbis — ffmpeg
     * makes them play. */
    if (mn_ext_ffmpeg_decline(ext)) return MA_NO_BACKEND;

    if (!mn_find_ffmpeg(ffmpeg, MAX_PATH)) {
        if (getenv("MN_DECODE_DEBUG"))
            fprintf(stderr, "[mf_decode] ffmpeg NOT FOUND for .%s\n", ext);
        return MA_NO_BACKEND;
    }
    if (getenv("MN_DECODE_DEBUG"))
        fwprintf(stderr, L"[mf_decode] ffmpeg=%s for %hs\n", ffmpeg, pFilePath);

    wsrc = mn_utf8_to_wide(pFilePath);
    if (wsrc == NULL) return MA_NO_BACKEND;

    if (GetTempPathW(MAX_PATH, tmpdir) == 0 ||
        GetTempFileNameW(tmpdir, L"mnf", 0, tmpfile) == 0) {
        free(wsrc);
        return MA_NO_BACKEND;
    }
    /* GetTempFileName made a .tmp file; give it a .wav name so the WAV backend
     * is happy and there's no stale empty file. */
    {
        wchar_t wavname[MAX_PATH];
        _snwprintf(wavname, MAX_PATH, L"%s.wav", tmpfile);
        DeleteFileW(tmpfile);
        wcsncpy(tmpfile, wavname, MAX_PATH - 1);
    }

    /* Build: ffmpeg -v error -y -i "src" -vn -acodec pcm_f32le "tmp.wav"
     * f32le keeps full precision; miniaudio decodes it trivially. */
    cmdcap = wcslen(ffmpeg) + wcslen(wsrc) + wcslen(tmpfile) + 96;
    cmd = (wchar_t *)malloc(cmdcap * sizeof(wchar_t));
    if (cmd == NULL) { free(wsrc); return MA_OUT_OF_MEMORY; }
    _snwprintf(cmd, cmdcap,
        L"\"%s\" -v error -nostdin -y -i \"%s\" -vn -map a:0 -acodec pcm_f32le \"%s\"",
        ffmpeg, wsrc, tmpfile);
    free(wsrc);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (getenv("MN_DECODE_DEBUG"))
            fprintf(stderr, "[mf_decode] CreateProcess failed err=%lu\n",
                    (unsigned long)GetLastError());
        free(cmd);
        DeleteFileW(tmpfile);
        return MA_NO_BACKEND;
    }
    free(cmd);
    /* Bound the wait so a hung ffmpeg can't wedge a track load. 60s is ample
     * for transcoding a single track to WAV. */
    wait = WaitForSingleObject(pi.hProcess, 60000);
    {
        DWORD ec = 1;
        GetExitCodeProcess(pi.hProcess, &ec);
        if (getenv("MN_DECODE_DEBUG"))
            fwprintf(stderr, L"[mf_decode] ffmpeg wait=%lu exit=%lu tmp=%s\n",
                     (unsigned long)wait, (unsigned long)ec, tmpfile);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (wait != WAIT_OBJECT_0 || ec != 0) {
            DeleteFileW(tmpfile);
            return MA_NO_BACKEND;
        }
    }

    /* Decode the transcoded WAV with miniaudio's WAV backend. */
    d = (mn_ff_ds *)ma_malloc(sizeof(*d), pAlloc);
    if (d == NULL) { DeleteFileW(tmpfile); return MA_OUT_OF_MEMORY; }
    memset(d, 0, sizeof(*d));
    if (pAlloc) d->alloc = *pAlloc;

    dcfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    dcfg.encodingFormat = ma_encoding_format_wav;
    if (pConfig && pConfig->preferredFormat != ma_format_unknown)
        dcfg.format = pConfig->preferredFormat;
    rc = ma_decoder_init_file_w(tmpfile, &dcfg, &d->inner);
    if (rc != MA_SUCCESS) {
        if (getenv("MN_DECODE_DEBUG"))
            fwprintf(stderr, L"[mf_decode] WAV re-decode failed rc=%d tmp=%s\n",
                     (int)rc, tmpfile);
        ma_free(d, pAlloc);
        DeleteFileW(tmpfile);
        return MA_NO_BACKEND;
    }

    dsc = ma_data_source_config_init();
    dsc.vtable = &g_mn_ff_ds_vtable;
    rc = ma_data_source_init(&dsc, &d->ds);
    if (rc != MA_SUCCESS) {
        ma_decoder_uninit(&d->inner);
        ma_free(d, pAlloc);
        DeleteFileW(tmpfile);
        return rc;
    }

    d->tmpfile = _wcsdup(tmpfile);
    *ppBackend = (ma_data_source *)d;
    return MA_SUCCESS;
}

static void mn_ff_onUninit(void *pUserData, ma_data_source *pBackend,
                           const ma_allocation_callbacks *pAlloc)
{
    mn_ff_ds *d = (mn_ff_ds *)pBackend;
    (void)pUserData;
    if (d == NULL) return;
    ma_decoder_uninit(&d->inner);
    ma_data_source_uninit(&d->ds);
    if (d->tmpfile) { DeleteFileW(d->tmpfile); free(d->tmpfile); }
    ma_free(d, pAlloc);
}

ma_decoding_backend_vtable g_mn_decoding_backend_ffmpeg = {
    NULL,
    mn_ff_onInitFile,
    NULL,           /* onInitFileW — UTF-8 path is sufficient                 */
    NULL,
    mn_ff_onUninit
};

/* ------------------------------------------------------------------------- */
/* Registration                                                               */
/* ------------------------------------------------------------------------- */

static ma_decoding_backend_vtable *g_mn_backends[] = {
    &g_mn_decoding_backend_mf,       /* try native OS decode first            */
    &g_mn_decoding_backend_ffmpeg    /* heavy fallback                        */
};

void mn_decode_config_apply_backends(ma_decoder_config *cfg)
{
    if (cfg == NULL) return;
    cfg->ppCustomBackendVTables = g_mn_backends;
    cfg->customBackendCount     = (ma_uint32)(sizeof(g_mn_backends) / sizeof(g_mn_backends[0]));
    cfg->pCustomBackendUserData = NULL;
}

#else /* !_WIN32 — non-Windows declining stubs */

void mn_decode_backends_init(void)     {}
void mn_decode_backends_shutdown(void) {}
void mn_decode_config_apply_backends(ma_decoder_config *cfg) { (void)cfg; }
void mn_decode_config_apply_mf_url(ma_decoder_config *cfg, const char *url) {
    (void)cfg; (void)url;
}

ma_decoding_backend_vtable g_mn_decoding_backend_mf     = {0};
ma_decoding_backend_vtable g_mn_decoding_backend_ffmpeg = {0};

#endif /* _WIN32 */
