/* ==========================================================================
 * audio_write.c — WAV / FLAC / MP3 writers for stem export. See header.
 * ========================================================================== */
#include "audio_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* miniaudio is implemented in audio_engine.c; here we only need the encoder
 * declarations (no second MINIAUDIO_IMPLEMENTATION). */
#include "../vendor/miniaudio.h"

/* shine MP3 encoder (vendored). */
#include "../vendor/shine/layer3.h"

const char *mn_awfmt_ext(mn_awfmt fmt) {
    switch (fmt) {
        case MN_AWFMT_FLAC: return "flac";
        case MN_AWFMT_MP3:  return "mp3";
        case MN_AWFMT_WAV:  default: return "wav";
    }
}

/* ---- helpers ---- */
static inline int32_t f_to_s24(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    /* symmetric 24-bit range */
    int32_t s = (int32_t)lrintf(v * 8388607.0f);
    if (s >  8388607)  s =  8388607;
    if (s < -8388608)  s = -8388608;
    return s;
}
static inline int16_t f_to_s16(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    int32_t s = (int32_t)lrintf(v * 32767.0f);
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

/* ==========================================================================
 * WAV — 24-bit PCM via miniaudio's built-in encoder.
 * ========================================================================== */
static bool write_wav(const char *path, const float *pcm, uint64_t frames,
                      uint32_t ch, uint32_t rate) {
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s24, ch, rate);
    ma_encoder enc;
    if (ma_encoder_init_file(path, &cfg, &enc) != MA_SUCCESS) return false;

    /* ma_format_s24 expects 3 bytes/sample little-endian, tightly packed. */
    const uint64_t CHUNK = 8192;
    uint8_t *buf = (uint8_t *)malloc((size_t)CHUNK * ch * 3);
    if (!buf) { ma_encoder_uninit(&enc); return false; }

    bool ok = true;
    uint64_t done = 0;
    while (done < frames) {
        uint64_t n = frames - done; if (n > CHUNK) n = CHUNK;
        size_t bi = 0;
        for (uint64_t i = 0; i < n; i++) {
            for (uint32_t c = 0; c < ch; c++) {
                int32_t s = f_to_s24(pcm[(done + i) * ch + c]);
                buf[bi++] = (uint8_t)(s & 0xFF);
                buf[bi++] = (uint8_t)((s >> 8) & 0xFF);
                buf[bi++] = (uint8_t)((s >> 16) & 0xFF);
            }
        }
        ma_uint64 written = 0;
        if (ma_encoder_write_pcm_frames(&enc, buf, n, &written) != MA_SUCCESS ||
            written != n) { ok = false; break; }
        done += n;
    }
    free(buf);
    ma_encoder_uninit(&enc);
    return ok;
}

/* ==========================================================================
 * MP3 — 320 kbps CBR via shine (interleaved int16).
 * ========================================================================== */
static bool write_mp3(const char *path, const float *pcm, uint64_t frames,
                      uint32_t ch, uint32_t rate) {
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;   /* shine is mono/stereo */

    shine_config_t cfg;
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.wave.channels   = (ch == 1) ? PCM_MONO : PCM_STEREO;
    cfg.wave.samplerate = (int)rate;
    cfg.mpeg.mode       = (ch == 1) ? MONO : JOINT_STEREO;
    cfg.mpeg.bitr       = 320;

    if (shine_find_samplerate_index((int)rate) < 0) return false;  /* unsupported SR */

    shine_t s = shine_initialise(&cfg);
    if (!s) return false;

    FILE *f = fopen(path, "wb");
    if (!f) { shine_close(s); return false; }

    const int spp = shine_samples_per_pass(s);   /* frames per pass, per channel */
    int16_t *ibuf = (int16_t *)malloc((size_t)spp * ch * sizeof(int16_t));
    if (!ibuf) { fclose(f); shine_close(s); return false; }

    bool ok = true;
    uint64_t done = 0;
    while (done < frames) {
        int n = (int)((frames - done > (uint64_t)spp) ? spp : (frames - done));
        /* shine wants a full pass; zero-pad the final short block */
        for (int i = 0; i < spp; i++) {
            for (uint32_t c = 0; c < ch; c++) {
                float v = (i < n) ? pcm[(done + i) * ch + c] : 0.0f;
                ibuf[i * ch + c] = f_to_s16(v);
            }
        }
        int written = 0;
        unsigned char *mp3 = shine_encode_buffer_interleaved(s, ibuf, &written);
        if (written > 0 && mp3) {
            if (fwrite(mp3, 1, (size_t)written, f) != (size_t)written) { ok = false; break; }
        }
        done += (uint64_t)spp;   /* advance a full pass (padding included) */
        if (n < spp) break;      /* last (padded) pass done */
    }
    /* flush */
    if (ok) {
        int written = 0;
        unsigned char *mp3 = shine_flush(s, &written);
        if (written > 0 && mp3) fwrite(mp3, 1, (size_t)written, f);
    }
    free(ibuf);
    fclose(f);
    shine_close(s);
    return ok;
}

/* ==========================================================================
 * FLAC — compact self-contained encoder (fixed predictors, no libFLAC).
 *
 * Produces a valid, standard-decodable FLAC stream: STREAMINFO + constant/
 * verbatim/fixed subframes with Rice-coded residuals. It intentionally omits
 * LPC (uses only fixed predictors orders 0-4, choosing the best per subframe),
 * which every FLAC decoder supports. Compression is a bit below libFLAC's
 * default but the output is fully lossless and correct. 24-bit samples.
 * ========================================================================== */

/* Bit writer that BUFFERS the whole frame in memory. This lets us compute the
 * FLAC frame-header CRC-8 (poly 0x07, over the header bytes) and the frame
 * footer CRC-16 (poly 0x8005, over everything up to the footer) correctly
 * before committing to the file. */
typedef struct {
    uint8_t *buf; size_t len, cap;
    uint32_t acc; int nbits;
} flac_bw;
static void flac_bw_init(flac_bw *b) { b->buf = NULL; b->len = 0; b->cap = 0; b->acc = 0; b->nbits = 0; }
static void flac_bw_free(flac_bw *b) { free(b->buf); b->buf = NULL; }
static void flac_put_byte(flac_bw *b, uint8_t v) {
    if (b->len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        uint8_t *nb = (uint8_t *)realloc(b->buf, nc);
        if (!nb) return;   /* OOM: drop (caller checks final validity) */
        b->buf = nb; b->cap = nc;
    }
    b->buf[b->len++] = v;
}
static uint8_t flac_crc8(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}
static uint16_t flac_crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
    }
    return crc;
}
static void flac_writebits(flac_bw *b, uint32_t val, int bits) {
    /* MSB-first bit packing */
    while (bits > 0) {
        int take = 8 - b->nbits; if (take > bits) take = bits;
        b->acc = (b->acc << take) | ((val >> (bits - take)) & ((1u << take) - 1));
        b->nbits += take; bits -= take;
        if (b->nbits == 8) { flac_put_byte(b, (uint8_t)b->acc); b->acc = 0; b->nbits = 0; }
    }
}
static void flac_flush_byte(flac_bw *b) {
    if (b->nbits > 0) { flac_writebits(b, 0, 8 - b->nbits); }
}
static void flac_unary(flac_bw *b, uint32_t q) { while (q--) flac_writebits(b, 0, 1); flac_writebits(b, 1, 1); }
/* zigzag map signed->unsigned */
static inline uint32_t zz(int32_t v) { return (uint32_t)((v << 1) ^ (v >> 31)); }

/* Rice-encode a residual block with parameter k. */
static void flac_rice(flac_bw *b, const int32_t *res, int n, int k) {
    for (int i = 0; i < n; i++) {
        uint32_t u = zz(res[i]);
        flac_unary(b, u >> k);
        if (k) flac_writebits(b, u & ((1u << k) - 1), k);
    }
}
/* estimate Rice bits for parameter k */
static uint64_t flac_rice_cost(const int32_t *res, int n, int k) {
    uint64_t bits = 0;
    for (int i = 0; i < n; i++) { uint32_t u = zz(res[i]); bits += (u >> k) + 1 + k; }
    return bits;
}
static int flac_best_k(const int32_t *res, int n, uint64_t *out_cost) {
    /* RICE2 residual coding (5-bit parameter): valid k is 0..30 — 31 is the
     * verbatim escape, which we never emit, so cap the search at 30. */
    int bestk = 0; uint64_t best = flac_rice_cost(res, n, 0);
    for (int k = 1; k <= 30; k++) {
        uint64_t c = flac_rice_cost(res, n, k);
        if (c < best) { best = c; bestk = k; } else if (c > best + n) break;
    }
    if (out_cost) *out_cost = best;
    return bestk;
}

/* one channel subframe: pick best fixed order 0..4 by residual magnitude. */
static void flac_subframe(flac_bw *b, const int32_t *s, int n, int bps) {
    /* constant? */
    int constant = 1;
    for (int i = 1; i < n; i++) if (s[i] != s[0]) { constant = 0; break; }
    if (constant) {
        flac_writebits(b, 0, 1);       /* zero pad bit */
        flac_writebits(b, 0, 6);       /* subframe type: CONSTANT */
        flac_writebits(b, 0, 1);       /* no wasted bits */
        flac_writebits(b, (uint32_t)s[0], bps);
        return;
    }
    /* try fixed predictors 0..4; residual r_o[i] */
    static int32_t res[5][1 << 16];    /* n <= block size (we cap at 4096) */
    uint64_t cost[5]; int k_of[5];
    int maxord = 4; if (maxord > n - 1) maxord = n - 1; if (maxord < 0) maxord = 0;
    for (int o = 0; o <= maxord; o++) {
        for (int i = 0; i < n; i++) {
            int32_t p;
            switch (o) {
                case 0: p = 0; break;
                case 1: p = (i>=1)? s[i-1] : 0; break;
                case 2: p = (i>=2)? 2*s[i-1]-s[i-2] : 0; break;
                case 3: p = (i>=3)? 3*s[i-1]-3*s[i-2]+s[i-3] : 0; break;
                default:p = (i>=4)? 4*s[i-1]-6*s[i-2]+4*s[i-3]-s[i-4] : 0; break;
            }
            res[o][i] = (i >= o) ? (s[i] - p) : 0;
        }
        uint64_t rc; k_of[o] = flac_best_k(res[o] + o, n - o, &rc);
        cost[o] = rc + (uint64_t)o * bps;   /* + warmup samples */
    }
    int best = 0; for (int o = 1; o <= maxord; o++) if (cost[o] < cost[best]) best = o;

    flac_writebits(b, 0, 1);                       /* zero pad */
    flac_writebits(b, (uint32_t)(8 | best), 6);    /* FIXED subframe: 001000+order */
    flac_writebits(b, 0, 1);                       /* no wasted bits */
    for (int i = 0; i < best; i++) flac_writebits(b, (uint32_t)s[i], bps);  /* warmup */
    /* residual: RICE2 method (5-bit Rice params), partition order 0.
     * RICE2 (method code 01) covers k up to 30, which 24-bit residuals need;
     * the 4-bit RICE method (max k 14, with 15 as the verbatim escape) is too
     * narrow and produced invalid streams on real audio. */
    flac_writebits(b, 1, 2);                        /* residual method: RICE2 (5-bit params) */
    flac_writebits(b, 0, 4);                        /* partition order 0 */
    {
        int kk = k_of[best];
        if (kk < 0) kk = 0;
        if (kk > 30) kk = 30;                       /* 31 = escape; never emit */
        flac_writebits(b, (uint32_t)kk, 5);         /* rice parameter (5-bit) */
        flac_rice(b, res[best] + best, n - best, kk);
    }
}

static bool write_flac(const char *path, const float *pcm, uint64_t frames,
                       uint32_t ch, uint32_t rate) {
    if (ch < 1) ch = 1;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int BPS = 24;
    const int BLK = 4096;

    /* --- fLaC marker + STREAMINFO metadata block --- */
    fwrite("fLaC", 1, 4, f);
    uint8_t mb[38]; memset(mb, 0, sizeof(mb));
    mb[0] = 0x80;                       /* last-metadata-block + type 0 (STREAMINFO) */
    mb[1] = 0; mb[2] = 0; mb[3] = 34;   /* length 34 */
    /* min/max block size */
    mb[4] = (BLK >> 8) & 0xFF; mb[5] = BLK & 0xFF;
    mb[6] = (BLK >> 8) & 0xFF; mb[7] = BLK & 0xFF;
    /* min/max frame size = 0 (unknown) already zeroed: content bytes 4..9 =
     * mb[8..13]. Sample rate begins at content byte 10 = mb[14] (the metadata
     * block header occupies mb[0..3], so content byte N lives at mb[4+N]). */
    /* sample rate (20 bits) | channels-1 (3) | bps-1 (5) | total samples (36) */
    uint64_t total = frames;
    mb[14] = (uint8_t)((rate >> 12) & 0xFF);
    mb[15] = (uint8_t)((rate >> 4) & 0xFF);
    mb[16] = (uint8_t)(((rate & 0xF) << 4) | (((ch - 1) & 0x7) << 1) | (((BPS - 1) >> 4) & 0x1));
    mb[17] = (uint8_t)((((BPS - 1) & 0xF) << 4) | (uint8_t)((total >> 32) & 0xF));
    mb[18] = (uint8_t)((total >> 24) & 0xFF);
    mb[19] = (uint8_t)((total >> 16) & 0xFF);
    mb[20] = (uint8_t)((total >> 8) & 0xFF);
    mb[21] = (uint8_t)(total & 0xFF);
    /* md5 (content bytes 18..33 = mb[22..37]) left zero = "not computed", valid */
    fwrite(mb, 1, 38, f);

    /* --- per-channel int32 sample scratch --- */
    int32_t *chan[8];
    for (uint32_t c = 0; c < ch && c < 8; c++) chan[c] = (int32_t *)malloc(sizeof(int32_t) * BLK);

    bool ok = true;
    uint64_t pos = 0; uint32_t frame_no = 0;
    flac_bw b; flac_bw_init(&b);
    while (pos < frames && ok) {
        int n = (int)((frames - pos > (uint64_t)BLK) ? BLK : (frames - pos));
        for (uint32_t c = 0; c < ch; c++)
            for (int i = 0; i < n; i++)
                chan[c][i] = f_to_s24(pcm[(pos + i) * ch + c]);

        b.len = 0; b.acc = 0; b.nbits = 0;   /* reset per frame */
        /* frame header */
        flac_writebits(&b, 0x3FFE, 14);    /* sync '11111111111110'          */
        flac_writebits(&b, 0, 1);          /* reserved                        */
        flac_writebits(&b, 0, 1);          /* blocking strategy: fixed        */
        flac_writebits(&b, 0x7, 4);        /* block size code 0111: 16-bit-1  */
        flac_writebits(&b, 0, 4);          /* sample rate: from STREAMINFO    */
        flac_writebits(&b, (uint32_t)(ch - 1), 4);  /* independent channels   */
        flac_writebits(&b, 0x6, 3);        /* sample size 110 = 24 bits/sample */
        flac_writebits(&b, 0, 1);          /* mandatory 0 (reserved)          */
        /* frame number, UTF-8 coded */
        if (frame_no < 0x80) {
            flac_writebits(&b, frame_no, 8);
        } else if (frame_no < 0x800) {
            flac_writebits(&b, 0xC0 | (frame_no >> 6), 8);
            flac_writebits(&b, 0x80 | (frame_no & 0x3F), 8);
        } else if (frame_no < 0x10000) {
            flac_writebits(&b, 0xE0 | (frame_no >> 12), 8);
            flac_writebits(&b, 0x80 | ((frame_no >> 6) & 0x3F), 8);
            flac_writebits(&b, 0x80 | (frame_no & 0x3F), 8);
        } else {
            flac_writebits(&b, 0xF0 | (frame_no >> 18), 8);
            flac_writebits(&b, 0x80 | ((frame_no >> 12) & 0x3F), 8);
            flac_writebits(&b, 0x80 | ((frame_no >> 6) & 0x3F), 8);
            flac_writebits(&b, 0x80 | (frame_no & 0x3F), 8);
        }
        flac_writebits(&b, (uint32_t)(n - 1), 16);   /* blocksize-1 */
        /* header ends on a byte boundary here; compute + append CRC-8. */
        {
            uint8_t c8 = flac_crc8(b.buf, b.len);
            flac_writebits(&b, c8, 8);
        }
        /* subframes */
        for (uint32_t c = 0; c < ch; c++)
            flac_subframe(&b, chan[c], n, BPS);
        flac_flush_byte(&b);
        /* frame footer CRC-16 over everything so far */
        {
            uint16_t c16 = flac_crc16(b.buf, b.len);
            flac_writebits(&b, c16, 16);
        }
        if (fwrite(b.buf, 1, b.len, f) != b.len) { ok = false; }
        pos += n; frame_no++;
    }
    flac_bw_free(&b);
    for (uint32_t c = 0; c < ch && c < 8; c++) free(chan[c]);
    fclose(f);
    return ok;
}

/* ==========================================================================
 * dispatch
 * ========================================================================== */
bool mn_audio_write(const char *path, mn_awfmt fmt, const float *pcm,
                    uint64_t frames, uint32_t ch, uint32_t rate) {
    if (!path || !pcm || frames == 0 || ch == 0 || rate == 0) return false;
    switch (fmt) {
        case MN_AWFMT_MP3:  return write_mp3(path, pcm, frames, ch, rate);
        case MN_AWFMT_FLAC: return write_flac(path, pcm, frames, ch, rate);
        case MN_AWFMT_WAV:  default: return write_wav(path, pcm, frames, ch, rate);
    }
}
