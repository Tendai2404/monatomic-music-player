/*
 * Monatomic Music Player — entry point.
 *
 * Creates the application controller (library db, audio, stems, playback) and
 * hands it to the CEF (open Chromium) host, which renders the HTML/CSS/JS UI and
 * bridges JS<->C. Pass --headless <file> for a quick audio-core smoke test.
 */
#include "app.h"
#include "cef_host.h"
#include "audio_engine.h"
#include "stems.h"
#include "audio_write.h"
#include "stempack.h"
#include "tags.h"
#include "tags_write.h"
#include "artcache.h"
#include "modeldl.h"

/* Compiled into artcache.c's stb_image_write implementation (extern there),
 * but not part of the header's public declaration block — declare it here. */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            int stride_bytes, int x, int y,
                                            int n, int *out_len);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
static void default_data_dir(char *out, size_t n) {
    char *appdata = getenv("APPDATA");
    if (appdata) snprintf(out, n, "%s\\Monatomic", appdata);
    else snprintf(out, n, ".");
    CreateDirectoryA(out, NULL);
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned long long now_ms(void) { return (unsigned long long)GetTickCount64(); }
#else
#include <unistd.h>
#include <pthread.h>
static void default_data_dir(char *out, size_t n) {
    char *home = getenv("HOME");
    snprintf(out, n, "%s/.monatomic", home ? home : ".");
}
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
static unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
}
#endif

#ifdef _WIN32
#define MN_PATH_SEP '\\'
#else
#define MN_PATH_SEP '/'
#endif

static int run_headless(const char *file) {
    mn_engine *e = NULL;
    if (mn_engine_create(&e) != MN_OK || !e) { fprintf(stderr, "engine init failed\n"); return 1; }
    if (mn_engine_load(e, file) != MN_OK) { fprintf(stderr, "load failed: %s\n", file); mn_engine_destroy(e); return 1; }
    mn_audio_format fmt; mn_engine_get_format(e, &fmt);
    printf("format: %s  %u Hz / %u ch -> %u Hz / %u ch / %u-bit\n",
           fmt.format, fmt.src_sample_rate, fmt.src_channels,
           fmt.out_sample_rate, fmt.out_channels, fmt.out_bits);
    mn_engine_play(e);
    while (!mn_engine_finished(e)) {
        printf("\r%llu / %llu ms   ", (unsigned long long)mn_engine_position_ms(e),
               (unsigned long long)mn_engine_duration_ms(e));
        fflush(stdout);
        sleep_ms(200);
    }
    printf("\ndone.\n");
    mn_engine_destroy(e);
    return 0;
}

/* --------------------------------------------------------------------------
 * --stress: two-thread race harness.
 *
 * Reproduces the GUI's threading model deterministically: the MAIN thread runs
 * mn_app_tick() every 10 ms plus scan_status/row_count/window polls (like the
 * host window's WM_TIMER + UI queries), while a SECOND thread hammers the
 * CEF-UI-thread call pattern (play_row/next/prev/seek/volume/stems toggles)
 * every ~20 ms. add_folder() is kicked mid-run to overlap a live scan with
 * playback transitions. Survives 30 s => prints "[stress] OK" and exits 0.
 * -------------------------------------------------------------------------- */
static volatile int g_stress_stop = 0;

typedef struct { mn_app *app; int64_t ids[2]; } stress_arg;

static void stress_ui_body(stress_arg *sa) {
    mn_app *a = sa->app;
    int64_t ids[2] = { sa->ids[0], sa->ids[1] };

    /* Stems stay ENABLED: with the seeded caches every track start publishes
     * neural buffers instantly, so the audio callback's mn_stems_mix is live
     * while this thread's next play_row/next/prev frees + reallocates those
     * same buffers (the GUI track-switch crash). The seek-to-end also forces
     * EOS so the MAIN thread's tick auto-advances concurrently: two threads
     * driving mn_engine_load / device reinit at once. */
    mn_app_stems_enable(a, true);

    for (int i = 0; !g_stress_stop; ++i) {
        switch (i % 8) {
            case 0: mn_app_play_row(a, ids[0]);        break;
            case 1: mn_app_seek_ms(a, 10 * 60 * 1000); break; /* clamp -> EOS */
            case 2: mn_app_play_row(a, ids[1]);        break;
            case 3: mn_app_next(a);                    break;
            case 4: mn_app_play_row(a, ids[0]);        break;
            case 5: mn_app_prev(a);                    break;
            case 6: mn_app_stems_enable(a, true);      break; /* re-kick job */
            case 7: mn_app_set_volume(a, 0.35f);       break;
        }
        sleep_ms(2);
    }
    mn_app_stems_enable(a, false);
}

#ifdef _WIN32
static DWORD WINAPI stress_ui_thread(LPVOID p) {
    stress_ui_body((stress_arg *)p);
    return 0;
}
#else
static void *stress_ui_thread(void *p) {
    stress_ui_body((stress_arg *)p);
    return NULL;
}
#endif

/* Pick two track ids for the stress hammer (first two rows of the view). */
static void stress_pick_ids(mn_app *a, int64_t ids[2]) {
    mn_row rows[2];
    int n = (int)mn_app_window(a, 0, 2, rows);
    ids[0] = (n >= 1) ? rows[0].id : 0;
    ids[1] = (n >= 2) ? rows[1].id : ids[0];
}

/* Seed a small stem-cache entry (2 s of silent stems) for `id` so that
 * enabling stems publishes neural buffers INSTANTLY from the cache; the audio
 * callback's mn_stems_mix is then actively reading those buffers while the
 * hammer's track switches free/reallocate them — the exact GUI crash path.
 * Existing (real) cache files are never touched. Returns 1 if this call
 * created the file (so the caller can clean it up afterwards). */
static int stress_seed_cache(const char *data_dir, int64_t id, char *path_out, size_t path_n) {
    unsigned int magic = 0x31534E4Du, stems = 9u;
    unsigned long long frames = 88200ull; /* 2 s @ 44.1 kHz */
    snprintf(path_out, path_n, "%s%cstem-cache%c%lld.mnstems", data_dir,
#ifdef _WIN32
             '\\', '\\',
#else
             '/', '/',
#endif
             (long long)id);
    FILE *probe = fopen(path_out, "rb");
    if (probe) { fclose(probe); return 0; } /* real cache exists: keep it */
    FILE *f = fopen(path_out, "wb");
    if (!f) return 0;
    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&stems, sizeof stems, 1, f);
    fwrite(&frames, sizeof frames, 1, f);
    {
        static float zeros[4096];
        unsigned long long remain = frames * 2ull * 9ull;
        while (remain > 0) {
            size_t n = (remain > 4096ull) ? 4096u : (size_t)remain;
            fwrite(zeros, sizeof(float), n, f);
            remain -= n;
        }
    }
    fclose(f);
    return 1;
}

/* --------------------------------------------------------------------------
 * --stress2: three-thread API hammer over the FULL command surface added in
 * the feature waves (DSP/EQ, facets, playlists, queue mutation, spectrum,
 * album cache) while transport runs. Complements --stress (which focuses on
 * scan/play races). ~12 s wall-clock; exits 0 iff no crash/deadlock.
 * -------------------------------------------------------------------------- */
typedef struct { mn_app *app; volatile int stop; } stress2_arg;

static unsigned s2_rand(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static DWORD WINAPI s2_transport(LPVOID p) {
    stress2_arg *a = (stress2_arg *)p;
    unsigned seed = 0xBEEF;
    while (!a->stop) {
        switch (s2_rand(&seed) % 5) {
            case 0: mn_app_toggle_pause(a->app); break;
            case 1: mn_app_next(a->app); break;
            case 2: mn_app_prev(a->app); break;
            case 3: mn_app_seek_ms(a->app, (int64_t)(s2_rand(&seed) % 180000)); break;
            case 4: mn_app_set_volume(a->app, (float)(s2_rand(&seed) % 100) / 100.0f); break;
        }
        Sleep(s2_rand(&seed) % 20);
    }
    return 0;
}

static DWORD WINAPI s2_queries(LPVOID p) {
    stress2_arg *a = (stress2_arg *)p;
    unsigned seed = 0xF00D;
    mn_album *alb = (mn_album *)malloc(64 * sizeof(mn_album));
    mn_row   *rows = (mn_row *)malloc(64 * sizeof(mn_row));
    mn_facet_value fv[64];
    mn_playlist_item pl[32];
    float spec[64];
    mn_now now;
    if (!alb || !rows) { free(alb); free(rows); return 1; }
    while (!a->stop) {
        switch (s2_rand(&seed) % 7) {
            case 0: (void)mn_app_album_window(a->app, (int64_t)(s2_rand(&seed) % 900), 64, alb); break;
            case 1: (void)mn_app_window(a->app, (int64_t)(s2_rand(&seed) % 4000), 64, rows); break;
            case 2: (void)mn_app_facet_window(a->app, 1 + (int)(s2_rand(&seed) % 5), 0, 64, fv); break;
            case 3: (void)mn_app_playlist_list(a->app, pl, 32); break;
            case 4: (void)mn_app_get_spectrum(a->app, spec, 64); break;
            case 5: mn_app_now(a->app, &now); break;
            case 6: (void)mn_app_album_count(a->app); break;
        }
        Sleep(s2_rand(&seed) % 8);
    }
    free(alb); free(rows);
    return 0;
}

static DWORD WINAPI s2_mutations(LPVOID p) {
    stress2_arg *a = (stress2_arg *)p;
    unsigned seed = 0xCAFE;
    mn_row row;
    while (!a->stop) {
        switch (s2_rand(&seed) % 8) {
            case 0: mn_app_set_eq_band(a->app, (int)(s2_rand(&seed) % 10),
                                       (float)((int)(s2_rand(&seed) % 24) - 12)); break;
            case 1: mn_app_set_dsp_enabled(a->app, (int)(s2_rand(&seed) % 2)); break;
            case 2: mn_app_set_preamp(a->app, (float)((int)(s2_rand(&seed) % 12) - 6)); break;
            case 3: mn_app_queue_move(a->app, (int)(s2_rand(&seed) % 20),
                                      (int)(s2_rand(&seed) % 20)); break;
            case 4: mn_app_queue_remove(a->app, (int)(s2_rand(&seed) % 20)); break;
            case 5:
                if (mn_app_window(a->app, (int64_t)(s2_rand(&seed) % 1000), 1, &row) == 1) {
                    mn_app_set_liked(a->app, row.id, (int)(s2_rand(&seed) % 3) - 1);
                }
                break;
            case 6: mn_app_set_balance(a->app, (float)((int)(s2_rand(&seed) % 200) - 100) / 100.0f); break;
            case 7: mn_app_set_sleep_timer(a->app, 0); break;
        }
        Sleep(s2_rand(&seed) % 15);
    }
    return 0;
}

static int run_stress2(void) {
    char dd[1024];
    default_data_dir(dd, sizeof(dd));
    fprintf(stderr, "[stress2] creating app at %s\n", dd); fflush(stderr);

    mn_app *a = mn_app_create(dd);
    if (!a) { fprintf(stderr, "[stress2] app create failed\n"); return 1; }
    if (mn_app_row_count(a) == 0) {
        fprintf(stderr, "[stress2] library empty; scan a folder first\n");
        mn_app_destroy(a);
        return 1;
    }

    /* Start playback so transport/EQ/spectrum act on a live engine. */
    {
        mn_row r;
        if (mn_app_window(a, 0, 1, &r) == 1) mn_app_play_row(a, r.id);
    }

    stress2_arg sa; sa.app = a; sa.stop = 0;
    HANDLE th[3];
    th[0] = CreateThread(NULL, 0, s2_transport, &sa, 0, NULL);
    th[1] = CreateThread(NULL, 0, s2_queries,   &sa, 0, NULL);
    th[2] = CreateThread(NULL, 0, s2_mutations, &sa, 0, NULL);
    if (!th[0] || !th[1] || !th[2]) {
        fprintf(stderr, "[stress2] thread create failed\n");
        sa.stop = 1;
        mn_app_destroy(a);
        return 1;
    }

    /* Main thread ticks like the real host for 12 s wall-clock. */
    {
        DWORD t0 = GetTickCount();
        int   ticks = 0;
        while (GetTickCount() - t0 < 12000) {
            mn_app_tick(a);
            ticks++;
            Sleep(30);
        }
        fprintf(stderr, "[stress2] %d ticks done, joining\n", ticks); fflush(stderr);
    }

    sa.stop = 1;
    WaitForMultipleObjects(3, th, TRUE, 15000);
    CloseHandle(th[0]); CloseHandle(th[1]); CloseHandle(th[2]);

    fprintf(stderr, "[stress2] destroying app\n"); fflush(stderr);
    mn_app_destroy(a);
    fprintf(stderr, "[stress2] OK\n");
    return 0;
}

static int run_stress(const char *scan_folder) {
    char dd[1024];
    default_data_dir(dd, sizeof(dd));
    fprintf(stderr, "[stress] creating app at %s\n", dd); fflush(stderr);

    mn_app *a = mn_app_create(dd);
    if (!a) { fprintf(stderr, "[stress] app create failed\n"); return 1; }

    if (mn_app_row_count(a) == 0) {
        fprintf(stderr, "[stress] library empty; scan a folder first\n");
        mn_app_destroy(a);
        return 1;
    }

    stress_arg sa;
    sa.app = a;
    stress_pick_ids(a, sa.ids);
    fprintf(stderr, "[stress] hammer ids: %lld %lld\n",
            (long long)sa.ids[0], (long long)sa.ids[1]);

    /* Seed instant stem-cache entries for the hammer ids (see above). */
    char seeded_path[2][1200];
    int  seeded[2];
    seeded[0] = stress_seed_cache(dd, sa.ids[0], seeded_path[0], sizeof(seeded_path[0]));
    seeded[1] = (sa.ids[1] != sa.ids[0])
              ? stress_seed_cache(dd, sa.ids[1], seeded_path[1], sizeof(seeded_path[1]))
              : 0;

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, stress_ui_thread, &sa, 0, NULL);
    if (!th) { fprintf(stderr, "[stress] thread create failed\n"); mn_app_destroy(a); return 1; }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, stress_ui_thread, &sa) != 0) {
        fprintf(stderr, "[stress] thread create failed\n"); mn_app_destroy(a); return 1;
    }
#endif

    /* MAIN thread: 30 s wall-clock of tick + status/query hammering; kick
     * scans at ~5 s and ~15 s so scanner teardown/restart overlaps
     * everything else. Wall-clock so lock contention can't stretch the run. */
    {
        const unsigned long long t0 = now_ms();
        const unsigned long long dur = 30000ull;
        unsigned long long next_mark = 0, scan1 = 5000, scan2 = 15000;
        int i = 0;
        for (;;) {
            unsigned long long el = now_ms() - t0;
            if (el >= dur) break;

            mn_app_tick(a);
            if ((i % 5) == 0) {
                mn_scan sc;
                mn_app_scan_status(a, &sc);
                mn_row rows[16];
                (void)mn_app_row_count(a);
                (void)mn_app_window(a, 0, 16, rows);
            }
            if (scan_folder && scan1 && el >= scan1) {
                scan1 = 0;
                fprintf(stderr, "[stress] add_folder %s (t=%llums)\n",
                        scan_folder, el);
                fflush(stderr);
                (void)mn_app_add_folder(a, scan_folder);
            }
            if (scan_folder && scan2 && el >= scan2) {
                scan2 = 0;
                fprintf(stderr, "[stress] add_folder %s (t=%llums)\n",
                        scan_folder, el);
                fflush(stderr);
                (void)mn_app_add_folder(a, scan_folder);
            }
            if (el >= next_mark) {
                fprintf(stderr, "[stress] t=%llus alive\n", el / 1000);
                fflush(stderr);
                next_mark += 5000;
            }
            i++;
            sleep_ms(2);
        }
    }

    g_stress_stop = 1;
#ifdef _WIN32
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif

    mn_app_destroy(a);

    if (seeded[0]) remove(seeded_path[0]);
    if (seeded[1]) remove(seeded_path[1]);

    fprintf(stderr, "[stress] OK\n");
    return 0;
}

/* --------------------------------------------------------------------------
 * --sep <audiofile>: stem-separation benchmark harness.
 *
 * Creates a stems session on the real model (%APPDATA%\Monatomic\ai-models\
 * htdemucs_6s.onnx) with the disk cache DISABLED (NULL cache dir), so the run
 * always measures the full decode + inference pipeline and leaves no cache
 * artifacts behind. Runs separation to completion and prints elapsed seconds,
 * audio seconds and the realtime factor.
 * -------------------------------------------------------------------------- */
static int run_sep(const char *file) {
    char dd[1024], model[1200];
    default_data_dir(dd, sizeof(dd));
    snprintf(model, sizeof(model), "%s%cai-models%chtdemucs_6s.onnx", dd,
#ifdef _WIN32
             '\\', '\\'
#else
             '/', '/'
#endif
    );

    fprintf(stderr, "[sep] model: %s\n[sep] audio: %s\n", model, file);
    fflush(stderr);

    unsigned long long t0 = now_ms();
    mn_stems *s = mn_stems_create(model, NULL /* no disk cache */);
    if (!s) {
        fprintf(stderr, "[sep] FAILED: mn_stems_create (model missing/ORT init)\n");
        return 1;
    }
    fprintf(stderr, "[sep] session created in %.2f s\n", (now_ms() - t0) / 1000.0);

    t0 = now_ms();
    if (!mn_stems_start(s, /*track_id*/ 1, file)) {
        fprintf(stderr, "[sep] FAILED: mn_stems_start\n");
        mn_stems_destroy(s);
        return 1;
    }

    /* MN_SEP_SIM=1: simulate a realtime playhead that polls mn_stems_mix,
     * verifying that neural mixing ENGAGES mid-separation (progressive
     * playback) — and, as a side effect, exercising the producer's GPU
     * duty-cycle pacing exactly as live playback would. */
    const int sim = (getenv("MN_SEP_SIM") != NULL);
    static float sim_buf[4096 * 2];
    unsigned long long sim_engaged_at = 0;

    mn_stems_progress p;
    memset(&p, 0, sizeof(p));
    float last_frac = -1.0f;
    unsigned long long last_progress = now_ms();
    for (;;) {
        sleep_ms(200);
        mn_stems_get_progress(s, &p);
        if (sim) {
            int64_t playhead = (int64_t)((now_ms() - t0) * 44100ull / 1000ull);
            if (mn_stems_mix(s, playhead, 4096, sim_buf) &&
                sim_engaged_at == 0) {
                sim_engaged_at = now_ms() - t0;
                fprintf(stderr,
                        "\n[sep] SIM: neural mix ENGAGED at t=%.1f s "
                        "(playhead %.1f s, frontier %.1f s, %.1f%% separated)\n",
                        sim_engaged_at / 1000.0, playhead / 44100.0,
                        p.separated_ms / 1000.0, p.fraction * 100.0f);
            }
        }
        if (p.fraction != last_frac) {
            last_frac = p.fraction;
            last_progress = now_ms();
            fprintf(stderr, "\r[sep] %5.1f%%  rt=%.2fx   ",
                    p.fraction * 100.0f, p.rt_factor);
            fflush(stderr);
        }
        if (p.fraction >= 1.0f) break;
        if (now_ms() - last_progress > 180000ull) {
            fprintf(stderr, "\n[sep] FAILED: no progress for 180 s "
                            "(inference error or stall)\n");
            mn_stems_destroy(s);
            return 1;
        }
    }
    if (sim) {
        fprintf(stderr, "\n[sep] SIM: engaged mid-separation: %s\n",
                sim_engaged_at ? "YES" : "NO");
    }

    {
        double elapsed = (now_ms() - t0) / 1000.0;
        double audio   = p.total_ms / 1000.0;
        const char *prov = (p.provider == MN_STEMS_PROVIDER_CUDA) ? "CUDA"
                         : (p.provider == MN_STEMS_PROVIDER_CPU)  ? "CPU"
                         : (p.provider == MN_STEMS_PROVIDER_CACHE)? "CACHE"
                                                                  : "NONE";
        printf("\n[sep] provider=%s elapsed_sec=%.2f audio_sec=%.2f "
               "rt_factor=%.2f (inference-only rt=%.2f)\n",
               prov, elapsed, audio, elapsed > 0.0 ? audio / elapsed : 0.0,
               p.rt_factor);
    }

    /* MN_SEP_LINGER=<sec>: keep the (resident) session alive after completion
     * so idle VRAM can be sampled externally (nvidia-smi) — verifies the
     * post-job arena shrinkage without unloading the model. */
    {
        const char *lg = getenv("MN_SEP_LINGER");
        int lsec = lg ? atoi(lg) : 0;
        if (lsec > 0) {
            fprintf(stderr, "[sep] lingering %d s with session resident "
                            "(sample idle VRAM now)...\n", lsec);
            fflush(stderr);
            sleep_ms((unsigned)lsec * 1000u);
        }
    }

    mn_stems_destroy(s);
    return 0;
}

/* --------------------------------------------------------------------------
 * --stemexport <audiofile>: end-to-end stem-export verification harness.
 *
 * Runs the exact production export path — mn_stems_separate_sync (cache
 * fast-path if the track was separated before) → mn_stems_export_channel for
 * all 9 channels → mn_audio_write to WAV/FLAC/MP3 → mn_stempack_write into a
 * .mnstem container — and prints the byte sizes so the output can be validated
 * externally (ffprobe on the encoded files, unzip on the container). Writes to
 * the system temp dir. Exercises real code, not a reconstruction.
 * -------------------------------------------------------------------------- */
static int run_stemexport(const char *file) {
    char dd[1024], model[1200];
    default_data_dir(dd, sizeof(dd));
#ifdef _WIN32
    snprintf(model, sizeof(model), "%s\\ai-models\\htdemucs_6s.onnx", dd);
    const char *tmp = getenv("TEMP"); if (!tmp) tmp = ".";
#else
    snprintf(model, sizeof(model), "%s/ai-models/htdemucs_6s.onnx", dd);
    const char *tmp = "/tmp";
#endif
    /* use the app's real stem cache dir so the fast-path can hit */
    char cache[1200];
#ifdef _WIN32
    snprintf(cache, sizeof(cache), "%s\\stem-cache", dd);
#else
    snprintf(cache, sizeof(cache), "%s/stem-cache", dd);
#endif

    fprintf(stderr, "[stemexport] model=%s\n[stemexport] audio=%s\n", model, file);
    mn_stems *s = mn_stems_create(model, cache);
    if (!s) { fprintf(stderr, "[stemexport] FAILED: mn_stems_create\n"); return 1; }

    /* MN_SX_TRACKID lets the harness reuse an existing cache entry (fast path). */
    const char *tid_env = getenv("MN_SX_TRACKID");
    int64_t tid = tid_env ? (int64_t)atoll(tid_env) : 999999;
    unsigned long long t0 = now_ms();
    if (!mn_stems_separate_sync(s, tid, file, NULL, NULL)) {
        fprintf(stderr, "[stemexport] FAILED: separate_sync\n");
        mn_stems_destroy(s);
        return 1;
    }
    fprintf(stderr, "[stemexport] separated in %.2f s\n", (now_ms() - t0) / 1000.0);

    static const char *NAMES[MN_STEMS_CHANNEL_COUNT] = {
        "Sub Bass", "Bass", "Vocals", "Lead", "Instruments",
        "Wide", "Air", "Guitar", "Piano"
    };
    const mn_awfmt fmts[3] = { MN_AWFMT_WAV, MN_AWFMT_FLAC, MN_AWFMT_MP3 };
    int fails = 0;

    /* For each format, encode all 9 channels to temp files and pack a container. */
    for (int fi = 0; fi < 3; fi++) {
        mn_awfmt fmt = fmts[fi];
        const char *ext = mn_awfmt_ext(fmt);
        mn_stempack_file members[MN_STEMS_CHANNEL_COUNT];
        char paths[MN_STEMS_CHANNEL_COUNT][1300];
        char arcs [MN_STEMS_CHANNEL_COUNT][64];
        int wrote = 0;

        for (int c = 0; c < MN_STEMS_CHANNEL_COUNT; c++) {
            float *pcm = NULL; uint64_t frames = 0;
            if (!mn_stems_export_channel(s, c, &pcm, &frames) || !pcm || !frames) {
                fprintf(stderr, "[stemexport] %s ch%d export_channel FAILED\n", ext, c);
                fails++; continue;
            }
            snprintf(paths[wrote], sizeof(paths[0]), "%s%cmn_sx_%d_%02d.%s",
                     tmp, MN_PATH_SEP, fi, c, ext);
            bool wok = mn_audio_write(paths[wrote], fmt, pcm, frames, 2, 44100);
            free(pcm);
            if (!wok) {
                fprintf(stderr, "[stemexport] %s ch%d write FAILED\n", ext, c);
                fails++; continue;
            }
            snprintf(arcs[wrote], sizeof(arcs[0]), "%02d %s.%s", c + 1, NAMES[c], ext);
            members[wrote].arcname = arcs[wrote];
            members[wrote].srcpath = paths[wrote];
            wrote++;
        }

        /* minimal manifest */
        char manifest[512];
        snprintf(manifest, sizeof(manifest),
                 "{\"schema\":1,\"model\":\"htdemucs_6s.onnx\",\"format\":\"%s\","
                 "\"stem_count\":%d}", ext, wrote);
        char outpack[1300];
        snprintf(outpack, sizeof(outpack), "%s%cmn_stemexport_%s.mnstem",
                 tmp, MN_PATH_SEP, ext);
        bool pok = mn_stempack_write(outpack, manifest, members, wrote, NULL);
        long long sz = 0;
        FILE *pf = fopen(outpack, "rb");
        if (pf) { fseek(pf, 0, SEEK_END); sz = ftell(pf); fclose(pf); }
        printf("[stemexport] %-4s: %d/%d stems, container=%s (%lld bytes) -> %s\n",
               ext, wrote, MN_STEMS_CHANNEL_COUNT, pok ? "OK" : "FAIL", sz, outpack);
        if (!pok || wrote != MN_STEMS_CHANNEL_COUNT) fails++;
    }

    mn_stems_destroy(s);
    printf("[stemexport] %s (%d failure%s)\n", fails ? "FAILED" : "PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * --arttest <audiofile>: cover-art extraction harness. Calls mn_art_ensure on
 * a throwaway cache dir and reports whether a thumbnail was produced. Isolates
 * the extraction path (embedded + folder sidecar) from the album sweep.
 * -------------------------------------------------------------------------- */
static int run_arttest(const char *file) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP"); if (!tmp) tmp = ".";
    char cache[1024];
    snprintf(cache, sizeof(cache), "%s\\mn_arttest_cache", tmp);
    /* CLEAR the harness cache first: the key below is fixed, so any thumb
     * left by a previous run would be served by mn_art_ensure's fast path
     * and produce a spurious PASS for a file with no art at all (observed
     * live: an artless album PASSed with the preceding run's thumb). */
    {
        char pat[1100];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\*", cache);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    char full[1400];
                    snprintf(full, sizeof(full), "%s\\%s", cache, fd.cFileName);
                    DeleteFileA(full);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    char out[1200] = {0};
    uint8_t *emb = NULL; size_t emblen = 0;
    bool has_emb = mn_tags_read_cover(file, &emb, &emblen, NULL);
    fprintf(stderr, "[arttest] file: %s\n", file);
    fprintf(stderr, "[arttest] embedded cover: %s (%zu bytes)\n",
            has_emb ? "present" : "none", emblen);
    if (emb) mn_tags_free_cover(emb);
    bool ok = mn_art_ensure(cache, "arttest\x1fkey", file, out, sizeof(out));
    fprintf(stderr, "[arttest] mn_art_ensure -> %s\n", ok ? "OK" : "FAILED");
    if (ok) {
        FILE *pf = fopen(out, "rb");
        long sz = 0;
        if (pf) { fseek(pf, 0, SEEK_END); sz = ftell(pf); fclose(pf); }
        fprintf(stderr, "[arttest] thumb: %s (%ld bytes)\n", out, sz);
    }
    printf("[arttest] %s\n", ok ? "PASS (thumbnail produced)" : "FAIL (no thumbnail)");
    return ok ? 0 : 1;
#else
    (void)file; return 1;
#endif
}

#ifdef _WIN32
/* FNV-1a over "<aa>\x1f<album>" — MUST mirror art_key_hash in cef_host.c
 * (the persisted NONE-verdict ledger artnone.txt stores these hashes). */
static uint64_t arttest_key_hash(const char *aa, const char *album) {
    uint64_t h = 1469598103934665603ULL;
    const char *parts[3];
    parts[0] = aa ? aa : "";
    parts[1] = "\x1f";
    parts[2] = album ? album : "";
    for (int i = 0; i < 3; i++)
        for (const unsigned char *p = (const unsigned char *)parts[i]; *p; ++p) {
            h ^= (uint64_t)(*p);
            h *= 1099511628211ULL;
        }
    return h;
}

/* Load the persisted non-music roots (<data>\folder_kinds.txt, lines of
 * "kind|path[|epoch]") into the harness app, mirroring the host's
 * sync_audiobook_roots (skip music, normalize the legacy plural). Without
 * this the harness app has ZERO kind roots, every view is unscoped, and a
 * kind-scoped audit silently audits nothing — the exact hole that let
 * 'missing=0' coexist with 182 unverified audiobook-grid keys. */
static int arttest_load_kind_roots(mn_app *a, const char *dd) {
    static char kinds[32][32];
    static char paths[32][512];
    char  kfile[1200], line[1400];
    int   n = 0;
    FILE *f;
    snprintf(kfile, sizeof(kfile), "%s\\folder_kinds.txt", dd);
    f = fopen(kfile, "r");
    if (!f) { mn_app_set_kind_roots(a, kinds, paths, 0); return 0; }
    while (n < 32 && fgets(line, sizeof(line), f)) {
        char  *s1 = strchr(line, '|');
        char  *s2 = s1 ? strchr(s1 + 1, '|') : NULL;
        char  *p;
        size_t ln;
        if (!s1) continue;
        *s1 = 0;
        p = s1 + 1;
        if (s2) *s2 = 0;
        ln = strlen(p);
        while (ln && (p[ln - 1] == '\n' || p[ln - 1] == '\r')) p[--ln] = 0;
        if (!ln || !line[0]) continue;
        if (_stricmp(line, "music") == 0) continue;
        if (_stricmp(line, "audiobooks") == 0)
            snprintf(kinds[n], sizeof(kinds[n]), "audiobook");
        else
            snprintf(kinds[n], sizeof(kinds[n]), "%s", line);
        snprintf(paths[n], sizeof(paths[n]), "%s", p);
        n++;
    }
    fclose(f);
    mn_app_set_kind_roots(a, kinds, paths, n);
    return n;
}

/* --arttest ident: for EVERY kind a view can activate ("" music + each
 * registered non-music kind), compare the real grid album-cache derivation
 * (mn_app_set_category_kind + mn_app_album_window — exactly what the active
 * view emits) against the verifier's kind-scoped derivation
 * (mn_app_album_ident_kind) — any (artist,title) divergence means the
 * verifier heals/verdicts a key some grid never asks for. Also sanity-checks
 * the kind-agnostic enumeration under music (legacy behavior). Prints the
 * first 25 divergences. */
static int run_arttest_ident(void) {
    char dd[1024]; default_data_dir(dd, sizeof(dd));
    mn_app *a = mn_app_create(dd);
    if (!a) { fprintf(stderr, "[ident] app create failed\n"); return 1; }
    arttest_load_kind_roots(a, dd);
    static char kinds[32][32];
    int32_t nkinds = mn_app_kind_list(a, kinds, 32);
    static mn_album g[60], v[60];
    int64_t diverged = 0, checked = 0, emptyaa = 0;
    for (int32_t k = 0; k <= nkinds; k++) {
        const char *kind = (k == 0) ? "" : kinds[k - 1];
        mn_app_set_category_kind(a, kind);
        int64_t n1 = mn_app_album_count(a);
        int64_t n2 = mn_app_album_count_kind(a, kind);
        fprintf(stderr, "[ident] kind=\"%s\" grid_count=%lld ident_count=%lld\n",
                kind[0] ? kind : "music", (long long)n1, (long long)n2);
        if (n1 != n2) diverged++;
        for (int64_t off = 0; off < n1 && off < n2; off += 60) {
            int gn = mn_app_album_window(a, off, 60, g);
            int vn = mn_app_album_ident_kind(a, kind, off, 60, v);
            int n = gn < vn ? gn : vn;
            for (int i = 0; i < n; i++) {
                checked++;
                if (!v[i].artist[0]) emptyaa++;
                if (strcmp(g[i].title, v[i].title) != 0 ||
                    strcmp(g[i].artist, v[i].artist) != 0) {
                    diverged++;
                    if (diverged <= 25)
                        fprintf(stderr, "[ident]   kind=\"%s\" grid=(\"%s\",\"%s\")"
                                "  vs  ident=(\"%s\",\"%s\")\n",
                                kind, g[i].artist, g[i].title,
                                v[i].artist, v[i].title);
                }
            }
        }
    }
    printf("[ident] kinds=%d checked=%lld diverged=%lld ident-empty-artist=%lld\n",
           (int)(nkinds + 1), (long long)checked, (long long)diverged,
           (long long)emptyaa);
    mn_app_destroy(a);
    return diverged == 0 ? 0 : 1;
}

/* --arttest lib: ONE-STORE serving-contract audit over the real library —
 * for EVERY kind-scoped grid the UI can open ("" music + each registered
 * non-music kind; the kind filter changes each album's first track and
 * therefore its art key, so auditing only one derivation let 'missing=0'
 * coexist with a kind view minting brand-new keys on first open).
 * Asserts, for every album row each kind's GRID emits (mn_app_set_category_
 * kind + mn_app_album_window — the exact aa/album strings build_albums
 * hashes), that either
 *   (a) mn_app_art_path resolves to an EXISTING art-cache thumbnail
 *       (art_url_for would emit a URL whose file exists — the no-404
 *       contract), or
 *   (b) a persisted NONE verdict is recorded in <data>\artnone.txt
 *       (intentional placeholder), or the album title is empty (unknown-
 *       album bucket, NONE by design).
 * Prints per-kind + total covered/none/missing; PASS iff missing == 0. */
static int run_arttest_lib(void) {
    char dd[1024]; default_data_dir(dd, sizeof(dd));
    mn_app *a = mn_app_create(dd);
    if (!a) { fprintf(stderr, "[arttest] app create failed\n"); return 1; }
    arttest_load_kind_roots(a, dd);

    /* load the NONE ledger */
    uint64_t *none = NULL; int none_n = 0, none_cap = 0;
    {
        char np[1200]; snprintf(np, sizeof(np), "%s\\artnone.txt", dd);
        FILE *f = fopen(np, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                uint64_t h = strtoull(line, NULL, 16);
                if (!h) continue;
                if (none_n == none_cap) {
                    int nc = none_cap ? none_cap * 2 : 256;
                    uint64_t *nl = (uint64_t *)realloc(none, (size_t)nc * 8);
                    if (!nl) break;
                    none = nl; none_cap = nc;
                }
                none[none_n++] = h;
            }
            fclose(f);
        }
    }

    static char kinds[32][32];
    int32_t nkinds = mn_app_kind_list(a, kinds, 32);
    int64_t covered = 0, nonecnt = 0, missing = 0, unnamed = 0, atot_sum = 0;
    static mn_album aw[60];
    for (int32_t kk = 0; kk <= nkinds; kk++) {
        const char *kind = (kk == 0) ? "" : kinds[kk - 1];
        int64_t kcov = 0, knone = 0, kmiss = 0, kunn = 0;
        mn_app_set_category_kind(a, kind);
        int64_t atot = mn_app_album_count(a);
        atot_sum += atot;
        for (int64_t off = 0; off < atot; off += 60) {
            int n = mn_app_album_window(a, off, 60, aw);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (!aw[i].title[0]) { kunn++; continue; }
                const char *p = mn_app_art_path(a, aw[i].artist, aw[i].title);
                if (p && p[0]) { kcov++; continue; }
                {
                    uint64_t h = arttest_key_hash(aw[i].artist, aw[i].title);
                    int k; bool isnone = false;
                    for (k = 0; k < none_n; k++)
                        if (none[k] == h) { isnone = true; break; }
                    if (isnone) { knone++; continue; }
                }
                kmiss++;
                if (missing + kmiss <= 25)
                    fprintf(stderr, "[arttest]   MISSING (kind=\"%s\"): "
                            "\"%s\" — \"%s\"\n",
                            kind[0] ? kind : "music",
                            aw[i].artist, aw[i].title);
            }
        }
        printf("[arttest] kind=\"%s\": albums=%lld covered=%lld "
               "none-verdict=%lld unnamed=%lld missing=%lld\n",
               kind[0] ? kind : "music", (long long)atot, (long long)kcov,
               (long long)knone, (long long)kunn, (long long)kmiss);
        covered += kcov; nonecnt += knone; missing += kmiss; unnamed += kunn;
    }
    printf("[arttest] TOTAL (%d kind%s): albums=%lld covered=%lld "
           "none-verdict=%lld unnamed=%lld missing=%lld\n",
           (int)(nkinds + 1), nkinds ? "s" : "", (long long)atot_sum,
           (long long)covered, (long long)nonecnt,
           (long long)unnamed, (long long)missing);
    printf("[arttest] %s\n",
           missing == 0 ? "PASS (every kind-scoped grid album serves or is a recorded NONE)"
                        : "FAIL (grid albums without servable art and no verdict)");
    free(none);
    mn_app_destroy(a);
    return missing == 0 ? 0 : 1;
}
#endif

/* --------------------------------------------------------------------------
 * --tagcheck <file>: metadata-writer verification harness.
 *
 * Copies the given audio file to a temp sibling copy (same extension so
 * container detection is unchanged), writes tags (title="TagCheck",
 * artist="Verifier", year=2026), lyrics ("line1\nline2") and a generated
 * 4x4 PNG as front-cover art through tags_write.c, then RE-READS via the
 * production readers (mn_tags_read / mn_tagw_read_lyrics /
 * mn_tags_read_cover) and prints PASS/FAIL per field. The original file
 * is never touched; the temp copy is deleted afterwards.
 * -------------------------------------------------------------------------- */

/* ==========================================================================
 * SELF-TEST + BENCHMARK  (--selftest / --bench)
 * Hardware-diversity verification: exercises every subsystem the app
 * depends on (DB+FTS, decode, encode, audio device, art cache, content
 * fingerprint, neural stems with its CUDA->CPU fallback) on THIS machine
 * and reports PASS/FAIL plus throughput numbers. Run on any box — AMD,
 * Intel, no-GPU — to verify the build before/after install.
 * ========================================================================== */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            int stride_bytes, int x, int y,
                                            int n, int *out_len);

static double st_now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int st_check(const char *what, int pass, int *fails, const char *note) {
    printf("  %-28s %s%s%s\n", what, pass ? "PASS" : "FAIL",
           note && note[0] ? "   " : "", note ? note : "");
    fflush(stdout);
    if (!pass) (*fails)++;
    return pass;
}

/* content-hash recipe (must mirror cef_host.c / the documented sync recipe):
 * fnv1a-64 over (size as 8 LE bytes || first 64KiB || last 64KiB). */
static int st_content_fp(const char *path, char *out, size_t outn) {
    static unsigned char buf[65536];
    unsigned char szle[8];
    unsigned long long h = 14695981039346656037ULL;
    FILE *f = fopen(path, "rb");
    long long size;
    size_t head, tail, got, i;
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END); size = _ftelli64(f);
    if (size <= 0) { fclose(f); return 0; }
    for (i = 0; i < 8; i++) szle[i] = (unsigned char)((size >> (i * 8)) & 0xFF);
    for (i = 0; i < 8; i++) { h ^= szle[i]; h *= 1099511628211ULL; }
    head = size < 65536 ? (size_t)size : 65536;
    _fseeki64(f, 0, SEEK_SET);
    got = fread(buf, 1, head, f);
    for (i = 0; i < got; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    if (size > 65536) {
        long long rem = size - 65536;
        tail = rem < 65536 ? (size_t)rem : 65536;
        _fseeki64(f, size - (long long)tail, SEEK_SET);
        got = fread(buf, 1, tail, f);
        for (i = 0; i < got; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    }
    fclose(f);
    snprintf(out, outn, "%016llx", h);
    return 1;
}

/* Write a stereo test tone (440/554 Hz) as 24-bit WAV; returns frames. */
static uint64_t st_make_tone(const char *path, double seconds) {
    uint64_t frames = (uint64_t)(44100.0 * seconds);
    float *buf = (float *)malloc((size_t)frames * 2 * sizeof(float));
    uint64_t i;
    if (!buf) return 0;
    for (i = 0; i < frames; i++) {
        double t = (double)i / 44100.0;
        buf[i * 2 + 0] = (float)(0.5 * sin(2.0 * 3.14159265358979 * 440.0 * t));
        buf[i * 2 + 1] = (float)(0.5 * sin(2.0 * 3.14159265358979 * 554.37 * t));
    }
    if (!mn_audio_write(path, MN_AWFMT_WAV, buf, frames, 2, 44100)) frames = 0;
    free(buf);
    return frames;
}

static int run_selftest(int with_stems) {
    char dd[1024], tone[1200], fp1[24], fp2[24];
    int fails = 0;
    mn_app *a;
    uint64_t tone_frames;

    default_data_dir(dd, sizeof(dd));
    printf("== Monatomic self-test ==\ndata dir: %s\n\n", dd);

    /* 1. app + database (open, migrate, query) */
    a = mn_app_create(dd);
    if (!st_check("app + database open", a != NULL, &fails, NULL)) return 1;
    {
        char note[96];
        int64_t rows = mn_app_row_count(a);
        int64_t albs = mn_app_album_count(a);
        snprintf(note, sizeof(note), "(%lld tracks, %lld albums)",
                 (long long)rows, (long long)albs);
        st_check("library query", rows >= 0 && albs >= 0, &fails, note);
        /* 2. FTS search */
        mn_app_set_search(a, "a");
        st_check("full-text search", mn_app_row_count(a) >= 0, &fails, NULL);
        mn_app_set_search(a, "");
    }

    /* 3. audio device enumeration + caps */
    {
        mn_audio_device devs[32];
        char note[96];
        int nd = mn_app_list_devices(a, devs, 32);
        snprintf(note, sizeof(note), "(%d device%s)", nd, nd == 1 ? "" : "s");
        st_check("audio devices", nd > 0, &fails, note);
    }
    {
        mn_audio_caps caps;
        memset(&caps, 0, sizeof(caps));
        st_check("device capability probe", mn_app_audio_caps(a, &caps), &fails,
                 caps.device_name[0] ? caps.device_name : NULL);
    }

    /* 4. encode -> decode roundtrip (audio_write WAV -> universal decode) */
    snprintf(tone, sizeof(tone), "%s\\selftest_tone.wav", dd);
    tone_frames = st_make_tone(tone, 2.0);
    st_check("audio encode (WAV)", tone_frames > 0, &fails, NULL);
    {
        float *L = NULL, *R = NULL;
        uint64_t fr = 0;
        int ok = tone_frames > 0 &&
                 mn_decode_44100_stereo_ex(tone, &L, &R, &fr, NULL, NULL) == 0 &&
                 fr > tone_frames * 95 / 100 && fr < tone_frames * 105 / 100;
        int nonsilent = 0;
        if (ok && L) {
            uint64_t i;
            for (i = 0; i < fr; i += 997) if (L[i] > 0.05f || L[i] < -0.05f) { nonsilent = 1; break; }
        }
        st_check("audio decode roundtrip", ok && nonsilent, &fails, NULL);
        free(L); free(R);
    }

    /* 5. content fingerprint: deterministic + repeatable */
    {
        int ok = st_content_fp(tone, fp1, sizeof(fp1)) &&
                 st_content_fp(tone, fp2, sizeof(fp2)) &&
                 strcmp(fp1, fp2) == 0 && strlen(fp1) == 16;
        st_check("content fingerprint", ok, &fails, ok ? fp1 : NULL);
    }

    /* 6. PNG encode (art pipeline's encoder) */
    {
        unsigned char *px = (unsigned char *)malloc(64 * 64 * 4);
        int len = 0, ok = 0;
        if (px) {
            int i;
            for (i = 0; i < 64 * 64 * 4; i++) px[i] = (unsigned char)(i * 31);
            {
                unsigned char *png = stbi_write_png_to_mem(px, 64 * 4, 64, 64, 4, &len);
                ok = png && len > 8 && png[1] == 'P';
                free(png);
            }
            free(px);
        }
        st_check("art PNG encoder", ok, &fails, NULL);
    }

    /* 7. neural stems: session create + provider (the CUDA->CPU fallback) */
    if (with_stems) {
        char mp[1200];
        snprintf(mp, sizeof(mp), "%s\\%s", dd, "ai-models\\htdemucs_6s.onnx");
        {
            FILE *mf = fopen(mp, "rb");
            if (!mf) {
                st_check("stems model present", 0, &fails, "(download via AI Models)");
            } else {
                fclose(mf);
                {
                    char cache[1200];
                    mn_stems *s;
                    snprintf(cache, sizeof(cache), "%s\\stem-cache", dd);
                    s = mn_stems_create(mp, cache);
                    st_check("stems session (ORT)", s != NULL, &fails, NULL);
                    if (s) {
                        double t0;
                        {   /* force FRESH inference: drop any cached result
                               so the provider (CUDA vs CPU) is really used */
                            char cf[1300];
                            snprintf(cf, sizeof(cf), "%s\\987654321.mnstems", cache);
                            remove(cf);
                        }
                        t0 = st_now_ms();
                        int ok = mn_stems_separate_sync(s, 987654321, tone, NULL, NULL);
                        double ms = st_now_ms() - t0;
                        mn_stems_progress prog;
                        memset(&prog, 0, sizeof(prog));
                        mn_stems_get_progress(s, &prog);
                        {
                            char note[128];
                            snprintf(note, sizeof(note),
                                     "(provider %s, 2s tone in %.1fs, %.2fx rt)",
                                     prog.provider == MN_STEMS_PROVIDER_CUDA ? "CUDA" :
                                     prog.provider == MN_STEMS_PROVIDER_CPU ? "CPU" :
                                     prog.provider == MN_STEMS_PROVIDER_CACHE ? "CACHE" : "?",
                                     ms / 1000.0, ms > 0 ? 2000.0 / ms : 0.0);
                            st_check("stem separation", ok, &fails, note);
                        }
                        mn_stems_destroy(s);
                    }
                }
            }
        }
    }

    remove(tone);
    mn_app_destroy(a);
    printf("\n%s — %d failure%s\n", fails ? "SELF-TEST FAILED" : "SELF-TEST PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

static int run_bench(void) {
    char dd[1024], tone[1200];
    mn_app *a;
    uint64_t tone_frames;

    default_data_dir(dd, sizeof(dd));
    printf("== Monatomic benchmark ==\ndata dir: %s\n\n", dd);
    a = mn_app_create(dd);
    if (!a) { printf("app create failed\n"); return 1; }

    snprintf(tone, sizeof(tone), "%s\\bench_tone.wav", dd);
    tone_frames = st_make_tone(tone, 5.0);

    /* decode speed */
    if (tone_frames) {
        float *L = NULL, *R = NULL; uint64_t fr = 0;
        double t0 = st_now_ms(), dt;
        int i;
        for (i = 0; i < 4; i++) {
            free(L); free(R); L = R = NULL;
            mn_decode_44100_stereo_ex(tone, &L, &R, &fr, NULL, NULL);
        }
        dt = (st_now_ms() - t0) / 4.0;
        printf("  decode (WAV)            %8.1fx realtime\n", dt > 0 ? 5000.0 / dt : 0);
        free(L); free(R);
    }

    /* DB windowed query rate */
    {
        static mn_row rows[100];
        int64_t total = mn_app_row_count(a);
        double t0 = st_now_ms(), dt;
        int i, n = 0;
        for (i = 0; i < 200; i++)
            n += mn_app_window(a, (total > 100 ? (i * 37) % (total - 100) : 0), 100, rows);
        dt = st_now_ms() - t0;
        printf("  library windows         %8.0f windows/s  (100 rows each, %d rows read)\n",
               dt > 0 ? 200000.0 / dt : 0, n);
    }

    /* FTS search latency */
    {
        static const char *terms[] = { "the", "love", "night", "one", "man" };
        double t0 = st_now_ms(), dt;
        int i;
        int64_t hits = 0;
        for (i = 0; i < 5; i++) {
            mn_app_set_search(a, terms[i]);
            hits += mn_app_row_count(a);
        }
        mn_app_set_search(a, "");
        dt = (st_now_ms() - t0) / 5.0;
        printf("  full-text search        %8.2f ms/query  (%lld total hits)\n",
               dt, (long long)hits);
    }

    /* content-hash throughput */
    if (tone_frames) {
        char fp[24];
        double t0 = st_now_ms(), dt;
        int i;
        for (i = 0; i < 20; i++) st_content_fp(tone, fp, sizeof(fp));
        dt = (st_now_ms() - t0) / 20.0;
        printf("  content fingerprint     %8.2f ms/file\n", dt);
    }

    /* PNG encode rate (art pipeline) */
    {
        unsigned char *px = (unsigned char *)malloc(512 * 512 * 4);
        if (px) {
            int i, len = 0;
            double t0, dt;
            for (i = 0; i < 512 * 512 * 4; i++) px[i] = (unsigned char)(i * 2654435761u >> 24);
            t0 = st_now_ms();
            for (i = 0; i < 10; i++) {
                unsigned char *png = stbi_write_png_to_mem(px, 512 * 4, 512, 512, 4, &len);
                free(png);
            }
            dt = (st_now_ms() - t0) / 10.0;
            printf("  art PNG encode (512px)  %8.1f ms/image\n", dt);
            free(px);
        }
    }

    /* stems separation (the GPU/CPU workhorse) */
    {
        char mp[1200], cache[1200];
        snprintf(mp, sizeof(mp), "%s\\%s", dd, "ai-models\\htdemucs_6s.onnx");
        snprintf(cache, sizeof(cache), "%s\\stem-cache", dd);
        {
            FILE *mf = fopen(mp, "rb");
            if (!mf) {
                printf("  stem separation           (model not downloaded — skipped)\n");
            } else {
                mn_stems *s;
                fclose(mf);
                s = mn_stems_create(mp, cache);
                if (s && tone_frames) {
                    double t0, dt;
                    {   char cf[1300];
                        snprintf(cf, sizeof(cf), "%s\\987654322.mnstems", cache);
                        remove(cf);
                    }
                    t0 = st_now_ms();
                    int ok = mn_stems_separate_sync(s, 987654322, tone, NULL, NULL);
                    mn_stems_progress prog;
                    dt = st_now_ms() - t0;
                    memset(&prog, 0, sizeof(prog));
                    mn_stems_get_progress(s, &prog);
                    printf("  stem separation         %8.2fx realtime  (%s, %s)\n",
                           dt > 0 ? 5000.0 / dt : 0,
                           prog.provider == MN_STEMS_PROVIDER_CUDA ? "CUDA" :
                           prog.provider == MN_STEMS_PROVIDER_CPU ? "CPU" :
                           prog.provider == MN_STEMS_PROVIDER_CACHE ? "CACHE" : "?",
                           ok ? "ok" : "FAILED");
                }
                if (s) mn_stems_destroy(s);
            }
        }
    }

    remove(tone);
    mn_app_destroy(a);
    printf("\nbenchmark complete\n");
    return 0;
}

static int tc_check(const char *what, int pass, int *fails) {
    printf("[tagcheck]   %-10s %s\n", what, pass ? "PASS" : "FAIL");
    if (!pass) (*fails)++;
    return pass;
}

static int run_tagcheck(const char *file) {
    char tmp[1200];
    int  fails = 0;
    char err[MN_TAGW_ERR_CAP];

    /* temp copy path: "<stem>.tagcheck.<ext>" (extension preserved). */
    {
        const char *dot = strrchr(file, '.');
        const char *s1 = strrchr(file, '/');
        const char *s2 = strrchr(file, '\\');
        const char *sep = (s1 > s2) ? s1 : s2;
        if (dot && (!sep || dot > sep)) {
            snprintf(tmp, sizeof(tmp), "%.*s.tagcheck%s",
                     (int)(dot - file), file, dot);
        } else {
            snprintf(tmp, sizeof(tmp), "%s.tagcheck", file);
        }
    }

    /* byte-copy the source to the temp path. */
    {
        FILE *in = fopen(file, "rb");
        FILE *out = in ? fopen(tmp, "wb") : NULL;
        static char buf[1 << 16];
        size_t n;
        if (!in || !out) {
            fprintf(stderr, "[tagcheck] cannot copy %s -> %s\n", file, tmp);
            if (in) fclose(in);
            if (out) fclose(out);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
    }
    printf("[tagcheck] file: %s\n[tagcheck] temp: %s\n", file, tmp);

    /* 1. tags */
    {
        mn_tag_edit e;
        memset(&e, 0, sizeof(e));
        snprintf(e.title, sizeof(e.title), "TagCheck");
        snprintf(e.artist, sizeof(e.artist), "Verifier");
        e.year = 2026;
        err[0] = 0;
        if (!mn_tagw_write_tags(tmp, &e, err, sizeof(err))) {
            printf("[tagcheck] write tags FAILED (%s)\n", err);
            fails++;
        }
    }
    /* 2. lyrics */
    err[0] = 0;
    if (!mn_tagw_write_lyrics(tmp, "line1\nline2", err, sizeof(err))) {
        printf("[tagcheck] write lyrics FAILED (%s)\n", err);
        fails++;
    }
    /* 3. art: generated 4x4 PNG */
    {
        static unsigned char px[4 * 4 * 4];
        int plen = 0, i;
        unsigned char *png;
        for (i = 0; i < 16; i++) {
            px[i * 4 + 0] = 0xE0; px[i * 4 + 1] = 0x20;
            px[i * 4 + 2] = 0x40; px[i * 4 + 3] = 0xFF;
        }
        png = stbi_write_png_to_mem(px, 4 * 4, 4, 4, 4, &plen);
        if (!png || plen <= 0) {
            printf("[tagcheck] png generation FAILED\n");
            fails++;
        } else {
            err[0] = 0;
            if (!mn_tagw_write_art(tmp, png, (size_t)plen, "image/png",
                                   err, sizeof(err))) {
                printf("[tagcheck] write art FAILED (%s)\n", err);
                fails++;
            } else {
                /* re-read the cover and compare bytes. */
                uint8_t *cb = NULL; size_t cl = 0;
                char mime[MN_TAGS_MIME_CAP] = {0};
                bool got = mn_tags_read_cover(tmp, &cb, &cl, mime);
                tc_check("art-bytes",
                         got && cl == (size_t)plen && memcmp(cb, png, cl) == 0,
                         &fails);
                tc_check("art-mime", got && strcmp(mime, "image/png") == 0,
                         &fails);
                mn_tags_free_cover(cb);
            }
            free(png);   /* STBIW default allocator is malloc/free */
        }
    }

    /* re-read tags + lyrics through the production readers. */
    {
        mn_tags t;
        char lyr[4096] = {0};
        if (!mn_tags_read(tmp, &t)) {
            printf("[tagcheck] mn_tags_read FAILED\n");
            fails++;
        } else {
            tc_check("title",  strcmp(t.title, "TagCheck") == 0, &fails);
            tc_check("artist", strcmp(t.artist, "Verifier") == 0, &fails);
            tc_check("year",   t.year == 2026, &fails);
        }
        (void)mn_tagw_read_lyrics(tmp, lyr, sizeof(lyr));
        tc_check("lyrics", strcmp(lyr, "line1\nline2") == 0, &fails);
    }

    /* 4. keep_missing (partial album-batch edit): write ONLY album-level
     * fields with keep_missing on — Title/Track#/Comment must SURVIVE.
     * Regression guard for the album-batch tag-wipe bug. */
    {
        mn_tag_edit e;
        mn_tags     t;
        /* first set a full baseline incl. a title + track we can check */
        memset(&e, 0, sizeof(e));
        snprintf(e.title,  sizeof(e.title),  "KeepMe");
        snprintf(e.artist, sizeof(e.artist), "BaseArtist");
        snprintf(e.album,  sizeof(e.album),  "BaseAlbum");
        e.track_no = 7;
        err[0] = 0;
        (void)mn_tagw_write_tags(tmp, &e, err, sizeof(err));
        /* now a PARTIAL edit: change only the album, keep_missing on */
        memset(&e, 0, sizeof(e));
        snprintf(e.album, sizeof(e.album), "NewAlbum");
        e.keep_missing = true;
        err[0] = 0;
        if (!mn_tagw_write_tags(tmp, &e, err, sizeof(err))) {
            printf("[tagcheck] keep_missing write FAILED (%s)\n", err);
            fails++;
        } else if (mn_tags_read(tmp, &t)) {
            tc_check("km-title-kept",  strcmp(t.title, "KeepMe") == 0, &fails);
            tc_check("km-track-kept",  t.track_no == 7, &fails);
            tc_check("km-album-changed", strcmp(t.album, "NewAlbum") == 0, &fails);
        } else {
            printf("[tagcheck] keep_missing re-read FAILED\n");
            fails++;
        }
    }

    /* MN_TAGCHECK_KEEP=1 keeps the written temp copy so external tools
     * (ffmpeg -f null decode) can validate the rewritten container. */
    if (!getenv("MN_TAGCHECK_KEEP")) {
        remove(tmp);
    } else {
        printf("[tagcheck] kept: %s\n", tmp);
    }
    printf("[tagcheck] %s (%d failure%s)\n", fails ? "FAIL" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * --tagapp <audiofile>: app-level locked-file integration harness.
 *
 * Validates the CRITICAL currently-playing rewrite path end-to-end: copies
 * the given file into a scratch folder, indexes it (add_folder), STARTS
 * PLAYING it, then calls mn_app_write_tags on the PLAYING track — which
 * must detach the engine (the decoder holds the file open, denying the
 * atomic replace), rewrite, and resume playback at the remembered
 * position. Verifies the write succeeded, playback resumed, and the file
 * on disk carries the new title.
 * -------------------------------------------------------------------------- */
static int run_tagapp(const char *file) {
    char base[1024], dd[1200], dir[1400], copy[1600];
    int rc = 0;

    /* ISOLATED data dir (scratch library db) so the harness never touches
     * the user's real library. */
    default_data_dir(base, sizeof(base));
    snprintf(dd, sizeof(dd), "%s%cmn_tagapp_data", base,
#ifdef _WIN32
             '\\');
    CreateDirectoryA(dd, NULL);
#else
             '/');
    { char cmd[1300]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dd); (void)system(cmd); }
#endif
    snprintf(dir, sizeof(dir), "%s%cmn_tagapp", dd,
#ifdef _WIN32
             '\\');
    CreateDirectoryA(dir, NULL);
#else
             '/');
    { char cmd[1300]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir); (void)system(cmd); }
#endif
    {
        const char *dot = strrchr(file, '.');
        snprintf(copy, sizeof(copy), "%s%clockcheck%s", dir,
#ifdef _WIN32
                 '\\',
#else
                 '/',
#endif
                 dot ? dot : ".mp3");
    }
    {
        FILE *in = fopen(file, "rb");
        FILE *out = in ? fopen(copy, "wb") : NULL;
        static char buf[1 << 16];
        size_t n;
        if (!in || !out) {
            fprintf(stderr, "[tagapp] cannot copy %s -> %s\n", file, copy);
            if (in) fclose(in);
            if (out) fclose(out);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
    }

    mn_app *a = mn_app_create(dd);
    if (!a) { fprintf(stderr, "[tagapp] app create failed\n"); return 1; }

    /* Index the scratch folder and wait for the scan to finish. */
    if (!mn_app_add_folder(a, dir)) {
        fprintf(stderr, "[tagapp] add_folder failed\n");
        mn_app_destroy(a);
        return 1;
    }
    for (int i = 0; i < 300; i++) {
        mn_scan sc; mn_app_scan_status(a, &sc);
        mn_app_tick(a);
        if (!sc.active && i > 4) break;
        sleep_ms(100);
    }

    /* Locate the copy's track id by path. */
    int64_t id = 0;
    {
        mn_row rows[64];
        int64_t total = mn_app_row_count(a);
        for (int64_t off = 0; off < total && !id && off < 20000; off += 64) {
            int n = (int)mn_app_window(a, off, 64, rows);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
#ifdef _WIN32
                if (_stricmp(rows[i].path, copy) == 0) { id = rows[i].id; break; }
#else
                if (strcmp(rows[i].path, copy) == 0) { id = rows[i].id; break; }
#endif
            }
        }
    }
    if (!id) {
        fprintf(stderr, "[tagapp] FAILED: copy not indexed (%s)\n", copy);
        mn_app_destroy(a);
        remove(copy);
        return 1;
    }

    /* Play it and confirm the transport is live (file now LOCKED by the
     * engine's decoder). */
    mn_app_play_row(a, id);
    for (int i = 0; i < 10; i++) { mn_app_tick(a); sleep_ms(100); }
    {
        mn_now now; mn_app_now(a, &now);
        fprintf(stderr, "[tagapp] playing id=%lld pos=%lld ms\n",
                (long long)now.track_id, (long long)now.position_ms);
        if (!now.playing || now.track_id != id) {
            fprintf(stderr, "[tagapp] FAILED: track did not start\n");
            rc = 1;
        }
    }

    /* Tag-write the PLAYING file: must detach + rewrite + resume. */
    if (rc == 0) {
        mn_tag_edit e;
        char err[MN_TAGW_ERR_CAP] = {0};
        memset(&e, 0, sizeof(e));
        snprintf(e.title, sizeof(e.title), "LockCheck");
        snprintf(e.artist, sizeof(e.artist), "Verifier");
        e.year = 2026;
        if (!mn_app_write_tags(a, id, &e, err, sizeof(err))) {
            fprintf(stderr, "[tagapp] FAILED: write on playing file (%s)\n", err);
            rc = 1;
        } else {
            int64_t p0, p1;
            mn_now now;
            mn_app_tick(a); sleep_ms(300); mn_app_tick(a);
            mn_app_now(a, &now); p0 = now.position_ms;
            sleep_ms(500); mn_app_tick(a);
            mn_app_now(a, &now); p1 = now.position_ms;
            fprintf(stderr, "[tagapp] after write: playing=%d pos %lld -> %lld ms\n",
                    now.playing ? 1 : 0, (long long)p0, (long long)p1);
            if (!now.playing || p1 <= p0) {
                fprintf(stderr, "[tagapp] FAILED: playback did not resume\n");
                rc = 1;
            }
            /* db row must reflect the new title immediately. */
            {
                mn_row rows[4];
                bool found = false;
                mn_app_set_search(a, "LockCheck");
                int n = (int)mn_app_window(a, 0, 4, rows);
                for (int i = 0; i < n; i++) {
                    if (rows[i].id == id &&
                        strcmp(rows[i].title, "LockCheck") == 0) found = true;
                }
                mn_app_set_search(a, "");
                fprintf(stderr, "[tagapp] db refresh: %s\n",
                        found ? "OK" : "FAILED");
                if (!found) rc = 1;
            }
        }
    }

    mn_app_destroy(a);

    /* The file on disk must carry the new tag (read AFTER destroy so the
     * decoder is gone). */
    if (rc == 0) {
        mn_tags t;
        if (!mn_tags_read(copy, &t) || strcmp(t.title, "LockCheck") != 0) {
            fprintf(stderr, "[tagapp] FAILED: file tag not rewritten\n");
            rc = 1;
        }
    }
    remove(copy);
    fprintf(stderr, "[tagapp] %s\n", rc == 0 ? "OK" : "FAILED");
    return rc;
}

/* --------------------------------------------------------------------- */
/* --dltest <repo> <file>: download a file from Hugging Face to a temp    */
/* dir, print progress %, and print OK + byte count on completion. Proves */
/* the WinHTTP resolve->CDN pipeline end to end against real HF servers.  */
/* --------------------------------------------------------------------- */
typedef struct {
    volatile int      done;      /* set on the terminal callback */
    volatile int      failed;
    volatile int64_t  bytes;
    char              err[160];
} dltest_state;

static void dltest_cb(void *user, int64_t done, int64_t total,
                      bool finished, const char *err) {
    dltest_state *s = (dltest_state *)user;
    int pct = (total > 0) ? (int)((done * 100) / total) : 0;
    if (finished) {
        s->bytes = done;
        if (err && err[0]) { s->failed = 1; snprintf(s->err, sizeof(s->err), "%s", err); }
        s->done = 1;
        return;
    }
    fprintf(stderr, "\r[dltest] %3d%%  %lld / %lld bytes    ",
            pct, (long long)done, (long long)total);
    fflush(stderr);
}

static int run_dltest(const char *repo, const char *file) {
    dltest_state s;
    char dest[1024];

    memset(&s, 0, sizeof(s));
#ifdef _WIN32
    {
        char tmp[900];
        DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
        if (n == 0 || n > sizeof(tmp)) snprintf(tmp, sizeof(tmp), ".\\");
        snprintf(dest, sizeof(dest), "%smn-dltest", tmp);
    }
#else
    snprintf(dest, sizeof(dest), "/tmp/mn-dltest");
#endif

    fprintf(stderr, "[dltest] repo=%s file=%s -> %s\n", repo, file, dest);
    if (!mn_modeldl_start(repo, file, NULL, dest, dltest_cb, &s)) {
        fprintf(stderr, "[dltest] start failed (busy/invalid)\n");
        return 1;
    }
    while (!s.done) sleep_ms(50);
    fprintf(stderr, "\n");
    if (s.failed) {
        fprintf(stderr, "[dltest] FAILED: %s\n", s.err);
        return 1;
    }
    fprintf(stderr, "[dltest] OK  downloaded %lld bytes -> %s\\%s\n",
            (long long)s.bytes, dest, file);
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Built as /SUBSYSTEM:WINDOWS so normal launches show NO console window.
     * The -- diagnostic harnesses still print: re-attach to the parent
     * terminal's console when one exists — but ONLY for streams that have no
     * handle already (explicit 2>file redirects must keep working). */
    if (argc >= 2 && argv[1][0] == '-' && AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL ||
            GetStdHandle(STD_OUTPUT_HANDLE) == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stdout);
        if (GetStdHandle(STD_ERROR_HANDLE) == NULL ||
            GetStdHandle(STD_ERROR_HANDLE) == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stderr);
    }
#endif
    if (argc >= 3 && strcmp(argv[1], "--headless") == 0)
        return run_headless(argv[2]);

    /* --headlessu <utf8file>: like --headless but reads the track path (UTF-8)
     * from a file, so paths with emoji/CJK survive the ANSI argv mangling and
     * we can test the real UTF-8 decode path from the CLI. */
    if (argc >= 3 && strcmp(argv[1], "--headlessu") == 0) {
        FILE *pf = fopen(argv[2], "rb");
        if (!pf) { fprintf(stderr, "cannot open path file\n"); return 1; }
        char pbuf[4096]; size_t rn = fread(pbuf, 1, sizeof(pbuf) - 1, pf); fclose(pf);
        pbuf[rn] = 0;
        while (rn && (pbuf[rn-1] == '\n' || pbuf[rn-1] == '\r')) pbuf[--rn] = 0;
        return run_headless(pbuf);
    }

    /* --tagcheck <file>: metadata writer round-trip harness (see above). */
    if (argc >= 3 && strcmp(argv[1], "--tagcheck") == 0)
        return run_tagcheck(argv[2]);

    /* --tagapp <file>: app-level locked-file / playing-track write harness. */
    if (argc >= 3 && strcmp(argv[1], "--tagapp") == 0)
        return run_tagapp(argv[2]);

    /* --sep <audiofile>: stem-separation benchmark (see run_sep above). */
    if (argc >= 3 && strcmp(argv[1], "--sep") == 0)
        return run_sep(argv[2]);

    /* --stemexport <audiofile>: end-to-end export harness (see run_stemexport). */
    if (argc >= 3 && strcmp(argv[1], "--stemexport") == 0)
        return run_stemexport(argv[2]);

    /* --arttest lib: library-wide one-store serving-contract audit;
     * --arttest <audiofile>: single-file extraction harness. */
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return run_selftest(1);
    if (argc >= 2 && strcmp(argv[1], "--selftest-fast") == 0)
        return run_selftest(0);          /* skip the model load */
    if (argc >= 2 && strcmp(argv[1], "--bench") == 0)
        return run_bench();
    if (argc >= 3 && strcmp(argv[1], "--arttest") == 0) {
#ifdef _WIN32
        if (strcmp(argv[2], "lib") == 0)
            return run_arttest_lib();
        if (strcmp(argv[2], "ident") == 0)
            return run_arttest_ident();
#endif
        return run_arttest(argv[2]);
    }

    /* --devices [index] [audiofile]: enumerate playback devices; optionally
     * select one and (optionally) play 3 s of audio on it to prove the device
     * reinit path end-to-end. Diagnostic twin of the audiodevices/setdevice
     * bridge commands. */
    if (argc >= 2 && strcmp(argv[1], "--devices") == 0) {
        mn_engine *e = NULL;
        if (mn_engine_create(&e) != MN_OK || !e) {
            fprintf(stderr, "[devices] engine init failed\n");
            return 1;
        }
        mn_audio_device devs[32];
        int n = mn_engine_list_devices(e, devs, 32);
        printf("[devices] %d playback device(s):\n", n);
        for (int i = 0; i < n; i++)
            printf("  [%d]%s %s\n", i, devs[i].is_default ? " (default)" : "",
                   devs[i].name);
        if (argc >= 3) {
            int idx = atoi(argv[2]);
            bool ok = mn_engine_select_device(e, idx);
            printf("[devices] select %d -> %s (active index now %d)\n",
                   idx, ok ? "OK" : "FAILED", mn_engine_selected_device(e));
            if (ok && argc >= 4 && mn_engine_load(e, argv[3]) == MN_OK &&
                mn_engine_play(e) == MN_OK) {
                sleep_ms(3000);
                printf("[devices] played 3 s on the selected device "
                       "(pos=%llu ms)\n",
                       (unsigned long long)mn_engine_position_ms(e));
            }
        }
        mn_engine_destroy(e);
        return 0;
    }

    /* --albumcheck: exhaustive album-enumeration probe (BUG 3). Prints the
     * album_count, then walks the facet in 60-wide windows from offset 0 to
     * count, asserting every album id returned is DISTINCT across all pages and
     * that the number of distinct ids equals album_count. Catches any facet cap
     * or repeat past an offset boundary. Exit 0 iff distinct == count. */
    if (argc >= 2 && strcmp(argv[1], "--albumcheck") == 0) {
        char dd[1024]; default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[albumcheck] app create failed\n"); return 1; }

        int64_t atot = mn_app_album_count(a);
        fprintf(stderr, "[albumcheck] album_count=%lld\n", (long long)atot);

        /* Collect ids across windows; detect duplicates with a simple sorted
         * scan (album counts are small — hundreds to low thousands). */
        static mn_album aw[60];
        int64_t *ids = (int64_t *)malloc(sizeof(int64_t) * (size_t)(atot > 0 ? atot : 1));
        int64_t nids = 0, paged = 0;
        int rc = 0;
        for (int64_t off = 0; off < atot; off += 60) {
            int n = mn_app_album_window(a, off, 60, aw);
            if (n <= 0) {
                fprintf(stderr, "[albumcheck] FAILED: empty window at offset %lld\n",
                        (long long)off);
                rc = 1; break;
            }
            for (int i = 0; i < n && ids; i++) ids[nids++] = aw[i].id;
            paged += n;
            if (off < 360)
                fprintf(stderr, "[albumcheck]   offset %lld -> %d albums (first id=%lld)\n",
                        (long long)off, n, (long long)aw[0].id);
        }
        /* Count distinct ids. */
        int64_t distinct = 0;
        if (ids && nids > 0) {
            /* insertion into a set via qsort + unique */
            for (int64_t i = 0; i < nids; i++)
                for (int64_t j = i + 1; j < nids; j++)
                    if (ids[j] < ids[i]) { int64_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
            distinct = nids ? 1 : 0;
            for (int64_t i = 1; i < nids; i++) if (ids[i] != ids[i-1]) distinct++;
        }
        fprintf(stderr, "[albumcheck] paged=%lld distinct=%lld count=%lld\n",
                (long long)paged, (long long)distinct, (long long)atot);
        if (distinct != atot || paged != atot) {
            fprintf(stderr, "[albumcheck] FAILED: distinct/paged != count\n");
            rc = 1;
        } else {
            fprintf(stderr, "[albumcheck] OK: all %lld albums enumerated, no repeats\n",
                    (long long)atot);
        }
        free(ids);
        mn_app_destroy(a);
        return rc;
    }

    /* --refreshart: run the whole-library art refresh (embedded APIC first,
     * then folder sidecars), printing per-album misses and the gained count.
     * Regenerates art-cache thumbs; does NOT touch webroot (no CEF here). */
    if (argc >= 2 && strcmp(argv[1], "--refreshart") == 0) {
        char dd[1024]; default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[refreshart] app create failed\n"); return 1; }
        int64_t gained = mn_app_refresh_art(a, false, 0, NULL, NULL);
        fprintf(stderr, "[refreshart] gained=%lld new thumbnails\n",
                (long long)gained);
        mn_app_destroy(a);
        return 0;
    }

    /* --check: functional check of the folder-visibility filter + date-added
     * sort. Lists folders, hides the first populated one, verifies the row
     * count drops, unhides it, verifies the count restores. Always leaves
     * the persisted hidden set the way it found it. */
    if (argc >= 2 && strcmp(argv[1], "--check") == 0) {
        char dd[1024]; default_data_dir(dd, sizeof(dd));
        fprintf(stderr, "[check] creating app at %s\n", dd); fflush(stderr);
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[check] app create failed\n"); return 1; }

        int rc = 0;

        /* Stems pending-enable: request separation IMMEDIATELY, while the
         * async model loader is still in flight. The flag must be
         * remembered and survive publication (verified further below,
         * after the other checks have given the loader time to finish). */
        mn_app_stems_enable(a, true);
        mn_folder *fl = (mn_folder *)malloc(sizeof(mn_folder) * 512);
        int nf = fl ? (int)mn_app_folder_list(a, fl, 512) : 0;
        int64_t before = mn_app_row_count(a);
        int64_t albums_before = mn_app_album_count(a);
        fprintf(stderr, "[check] folders=%d rows=%lld albums=%lld\n",
                nf, (long long)before, (long long)albums_before);
        int pick = -1;
        for (int i = 0; i < nf; i++) {
            if (i < 8)
                fprintf(stderr, "[check]   id=%lld tracks=%lld hidden=%d %s\n",
                        (long long)fl[i].id, (long long)fl[i].track_count,
                        fl[i].hidden ? 1 : 0, fl[i].path);
            if (pick < 0 && !fl[i].hidden && fl[i].track_count > 0) pick = i;
        }
        if (nf > 8) fprintf(stderr, "[check]   ... (%d more)\n", nf - 8);

        if (pick >= 0 && before > 0) {
            if (!mn_app_folder_set_hidden(a, fl[pick].id, true))
                { fprintf(stderr, "[check] FAILED: hide rejected\n"); rc = 1; }
            int64_t hidden_rows   = mn_app_row_count(a);
            int64_t hidden_albums = mn_app_album_count(a);
            if (!mn_app_folder_set_hidden(a, fl[pick].id, false))
                { fprintf(stderr, "[check] FAILED: unhide rejected\n"); rc = 1; }
            int64_t restored = mn_app_row_count(a);
            fprintf(stderr, "[check] hide id=%lld (%lld tracks): rows %lld -> %lld "
                            "(albums %lld -> %lld), unhide -> %lld\n",
                    (long long)fl[pick].id, (long long)fl[pick].track_count,
                    (long long)before, (long long)hidden_rows,
                    (long long)albums_before, (long long)hidden_albums,
                    (long long)restored);
            if (hidden_rows != before - fl[pick].track_count)
                { fprintf(stderr, "[check] FAILED: hidden row count wrong\n"); rc = 1; }
            if (restored != before)
                { fprintf(stderr, "[check] FAILED: count not restored\n"); rc = 1; }
        } else {
            fprintf(stderr, "[check] library empty / no populated folder; "
                            "hide test skipped\n");
        }

        /* date-added sort: newest first must be monotonically non-increasing. */
        {
            mn_row rows[8];
            mn_app_set_sort(a, MN_SORT_DATE_ADDED, false);
            int n = (int)mn_app_window(a, 0, 8, rows);
            for (int i = 1; i < n; i++) {
                if (rows[i].date_added > rows[i - 1].date_added) {
                    fprintf(stderr, "[check] FAILED: date_added not descending\n");
                    rc = 1;
                    break;
                }
            }
            if (n > 0)
                fprintf(stderr, "[check] sort by added desc: top date_added=%lld "
                                "path=%s\n",
                        (long long)rows[0].date_added, rows[0].path);
        }

        /* Album pagination: every 60-wide window must advance (fresh album
         * ids, no repeats) and the windows must add up to album_count. */
        {
            static mn_album aw[60];
            int64_t atot = mn_app_album_count(a);
            int64_t seen = 0, prev_first = 0;
            for (int64_t off = 0; off < atot; off += 60) {
                int n = mn_app_album_window(a, off, 60, aw);
                if (n <= 0) {
                    fprintf(stderr, "[check] FAILED: empty album window at "
                                    "offset %lld (total %lld)\n",
                            (long long)off, (long long)atot);
                    rc = 1;
                    break;
                }
                if (off > 0 && aw[0].id == prev_first) {
                    fprintf(stderr, "[check] FAILED: album window at offset "
                                    "%lld repeats the previous page\n",
                            (long long)off);
                    rc = 1;
                    break;
                }
                prev_first = aw[0].id;
                seen += n;
            }
            fprintf(stderr, "[check] album pagination: total=%lld paged=%lld\n",
                    (long long)atot, (long long)seen);
            if (seen != atot) {
                fprintf(stderr, "[check] FAILED: album pages sum %lld != "
                                "count %lld\n",
                        (long long)seen, (long long)atot);
                rc = 1;
            }
        }

        /* Liked (thumbs) round-trip on the first row. */
        {
            mn_row rows[1];
            if ((int)mn_app_window(a, 0, 1, rows) == 1) {
                int64_t id = rows[0].id;
                int32_t orig = mn_app_get_liked(a, id);
                mn_app_set_liked(a, id, 1);
                int32_t up = mn_app_get_liked(a, id);
                mn_app_set_liked(a, id, -1);
                int32_t dn = mn_app_get_liked(a, id);
                mn_app_set_liked(a, id, orig);
                fprintf(stderr, "[check] liked roundtrip id=%lld: 1->%d -1->%d"
                                " (restored %d)\n",
                        (long long)id, up, dn, orig);
                if (up != 1 || dn != -1) {
                    fprintf(stderr, "[check] FAILED: liked roundtrip\n");
                    rc = 1;
                }
                /* liked must surface in the row projection too. */
                mn_app_set_liked(a, id, 1);
                if ((int)mn_app_window(a, 0, 1, rows) == 1 &&
                    rows[0].id == id && rows[0].liked != 1) {
                    fprintf(stderr, "[check] FAILED: liked missing from row\n");
                    rc = 1;
                }
                mn_app_set_liked(a, id, orig);
            }
        }

        /* Library stats. */
        {
            mn_app_stats st;
            if (!mn_app_get_stats(a, &st)) {
                fprintf(stderr, "[check] FAILED: stats\n");
                rc = 1;
            } else {
                fprintf(stderr, "[check] stats: tracks=%lld albums=%lld "
                                "artists=%lld hours=%.1f size=%lld hires=%.1f%% "
                                "lyrics=%.1f%% missing=%lld formats=%d",
                        (long long)st.tracks, (long long)st.albums,
                        (long long)st.artists,
                        (double)st.duration_ms / 3600000.0,
                        (long long)st.size_bytes, st.hires_pct, st.lyrics_pct,
                        (long long)st.missing, st.format_count);
                for (int i = 0; i < st.format_count && i < 4; i++)
                    fprintf(stderr, " %s:%lld", st.formats[i].fmt,
                            (long long)st.formats[i].n);
                fprintf(stderr, "\n");
                if (before > 0 && st.tracks <= 0) {
                    fprintf(stderr, "[check] FAILED: stats track count\n");
                    rc = 1;
                }
            }
        }

        /* Stems: wait for the async loader to publish, then confirm the
         * early enable request survived (pending-enable semantics). */
        {
            mn_now now;
            int waited = 0;
            for (;;) {
                mn_app_now(a, &now);
                if (now.stems_available || (!now.stems_loading && waited > 0))
                    break;
                if (waited >= 60000) break;
                sleep_ms(250);
                waited += 250;
            }
            fprintf(stderr, "[check] stems: available=%d loading=%d "
                            "enabled=%d (waited %d ms)\n",
                    now.stems_available ? 1 : 0, now.stems_loading ? 1 : 0,
                    now.stems_enabled ? 1 : 0, waited);
            if (!now.stems_available) {
                fprintf(stderr, "[check] stems model missing/failed — "
                                "pending-enable check skipped\n");
            } else if (!now.stems_enabled) {
                fprintf(stderr, "[check] FAILED: pre-load stems enable was "
                                "dropped on publish\n");
                rc = 1;
            }
            mn_app_stems_enable(a, false);
        }

        free(fl);
        mn_app_destroy(a);
        if (rc == 0) fprintf(stderr, "[check] OK\n");
        return rc;
    }

    /* --rmcheck <audiofile>: isolated removefolder harness. Copies the file
     * into a scratch library (same pattern as --tagapp), indexes it, removes
     * its folder and verifies rows + folder list drop to zero. */
    if (argc >= 3 && strcmp(argv[1], "--rmcheck") == 0) {
        char base[1024], dd[1200], dir[1400], copy[1600];
        int rc = 0;
        default_data_dir(base, sizeof(base));
        snprintf(dd, sizeof(dd), "%s%cmn_rmcheck_data", base,
#ifdef _WIN32
                 '\\');
        CreateDirectoryA(dd, NULL);
#else
                 '/');
        { char cmd[1300]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dd); (void)system(cmd); }
#endif
        snprintf(dir, sizeof(dir), "%s%cmn_rmcheck", dd,
#ifdef _WIN32
                 '\\');
        CreateDirectoryA(dir, NULL);
#else
                 '/');
        { char cmd[1300]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir); (void)system(cmd); }
#endif
        {
            const char *dot = strrchr(argv[2], '.');
            snprintf(copy, sizeof(copy), "%s%crmcheck%s", dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     dot ? dot : ".mp3");
        }
        {
            FILE *in = fopen(argv[2], "rb");
            FILE *out = in ? fopen(copy, "wb") : NULL;
            static char buf[1 << 16];
            size_t n;
            if (!in || !out) {
                fprintf(stderr, "[rmcheck] cannot copy %s\n", argv[2]);
                if (in) fclose(in);
                if (out) fclose(out);
                return 1;
            }
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
            fclose(in); fclose(out);
        }

        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[rmcheck] app create failed\n"); return 1; }
        if (!mn_app_add_folder(a, dir)) {
            fprintf(stderr, "[rmcheck] add_folder failed\n");
            mn_app_destroy(a);
            return 1;
        }
        for (int i = 0; i < 300; i++) {
            mn_scan sc; mn_app_scan_status(a, &sc);
            mn_app_tick(a);
            if (!sc.active && i > 4) break;
            sleep_ms(100);
        }
        {
            mn_folder fl[8];
            int nf = (int)mn_app_folder_list(a, fl, 8);
            int64_t rows_before = mn_app_row_count(a);
            fprintf(stderr, "[rmcheck] indexed: folders=%d rows=%lld\n",
                    nf, (long long)rows_before);
            if (nf < 1 || rows_before < 1) {
                fprintf(stderr, "[rmcheck] FAILED: nothing indexed\n");
                rc = 1;
            } else {
                /* hide it first so the hidden-set cleanup path runs too */
                int64_t fid = fl[0].id;
                (void)mn_app_folder_set_hidden(a, fid, true);
                int64_t deleted = mn_app_remove_folder(a, fid);
                int64_t rows_after = mn_app_row_count(a);
                int nf2 = (int)mn_app_folder_list(a, fl, 8);
                fprintf(stderr, "[rmcheck] removefolder id=%lld: deleted=%lld "
                                "rows %lld -> %lld folders %d -> %d hidden=%d\n",
                        (long long)fid, (long long)deleted,
                        (long long)rows_before, (long long)rows_after,
                        nf, nf2, mn_app_folder_hidden(a, fid) ? 1 : 0);
                if (deleted != rows_before || rows_after != 0 || nf2 != 0) {
                    fprintf(stderr, "[rmcheck] FAILED\n");
                    rc = 1;
                }
            }
        }
        mn_app_destroy(a);
        remove(copy);
        fprintf(stderr, "[rmcheck] %s\n", rc == 0 ? "OK" : "FAILED");
        return rc;
    }

    /* --dltest <repo> <file>: Hugging Face download smoke test. */
    if (argc >= 4 && strcmp(argv[1], "--dltest") == 0)
        return run_dltest(argv[2], argv[3]);

    /* --stress [folder]: two-thread race harness (see run_stress above). */
    if (argc >= 2 && strcmp(argv[1], "--stress") == 0) {
        static char stress_dir[MAX_PATH];
        const char *prof = getenv("USERPROFILE");
        snprintf(stress_dir, sizeof(stress_dir), "%s\\Music",
                 prof && prof[0] ? prof : ".");
        return run_stress(argc >= 3 ? argv[2] : stress_dir);
    }

    /* --stress2: three-thread full-API hammer (transport/queries/mutations). */
    if (argc >= 2 && strcmp(argv[1], "--stress2") == 0)
        return run_stress2();

    /* --scan <folder> [data_dir]: exercise add_folder + the scanner headlessly.
     * Optional data_dir runs against a THROWAWAY db instead of the live
     * library (safe while the app is open). Prints the skip counter so
     * incremental rescans are verifiable: a second run over the same folder
     * must show proc~0 / skip~found. */
    if (argc >= 3 && strcmp(argv[1], "--scan") == 0) {
        char dd[1024];
        if (argc >= 4) snprintf(dd, sizeof(dd), "%s", argv[3]);
        else           default_data_dir(dd, sizeof(dd));
        fprintf(stderr, "[scan] creating app at %s\n", dd); fflush(stderr);
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[scan] app create failed\n"); return 1; }
        fprintf(stderr, "[scan] add_folder %s\n", argv[2]); fflush(stderr);
        long long t0 = (long long)GetTickCount64();
        bool ok = mn_app_add_folder(a, argv[2]);
        fprintf(stderr, "[scan] add_folder -> %d\n", ok); fflush(stderr);
        for (int i = 0; i < 600; i++) {
            mn_scan sc; mn_app_scan_status(a, &sc);
            mn_app_tick(a);
            /* also hammer the query path to reproduce the UI-thread race */
            mn_row rows[32];
            int64_t n = mn_app_row_count(a);
            (void)mn_app_window(a, 0, 32, rows);
            fprintf(stderr, "\r[scan] found=%lld proc=%lld skip=%lld rows=%lld active=%d   ",
                    (long long)sc.found, (long long)sc.processed,
                    (long long)sc.skipped, (long long)n, sc.active);
            fflush(stderr);
            if (!sc.active && i > 4) break;
            sleep_ms(100);
        }
        fprintf(stderr, "\n[scan] elapsed=%lldms, destroying\n",
                (long long)GetTickCount64() - t0); fflush(stderr);
        mn_app_destroy(a);
        fprintf(stderr, "[scan] OK\n");
        return 0;
    }

    /* --sortcheck [data_dir]: verify every sort key × direction end-to-end
     * through the SAME code path the UI uses (set_sort -> window) for BOTH
     * tracks and albums. Ascending and descending must yield different
     * first rows on any real library (id tiebreak guarantees it). This is
     * the regression harness for the recurring "sorting does nothing". */
    if (argc >= 2 && strcmp(argv[1], "--sortcheck") == 0) {
        char dd[1024];
        if (argc >= 3) snprintf(dd, sizeof(dd), "%s", argv[2]);
        else           default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[sortcheck] app create failed\n"); return 1; }
        static const struct { mn_sort k; const char *name; } KS[] = {
            {MN_SORT_TITLE,"title"},   {MN_SORT_ARTIST,"artist"},
            {MN_SORT_ALBUM,"album"},   {MN_SORT_GENRE,"genre"},
            {MN_SORT_YEAR,"year"},     {MN_SORT_DURATION,"duration"},
            {MN_SORT_RATING,"rating"}, {MN_SORT_PLAY_COUNT,"plays"},
            {MN_SORT_DATE_ADDED,"added"},
        };
        int same = 0;
        for (size_t i = 0; i < sizeof(KS) / sizeof(KS[0]); ++i) {
            mn_row   ra[2], rd[2];
            mn_album aa[2], ad[2];
            int na, nd, ba, bd, tdiff, adiff;
            mn_app_set_sort(a, KS[i].k, true);
            na = (int)mn_app_window(a, 0, 2, ra);
            ba = (int)mn_app_album_window(a, 0, 2, aa);
            mn_app_set_sort(a, KS[i].k, false);
            nd = (int)mn_app_window(a, 0, 2, rd);
            bd = (int)mn_app_album_window(a, 0, 2, ad);
            tdiff = (na > 0 && nd > 0) && (ra[0].id != rd[0].id);
            adiff = (ba > 0 && bd > 0) && (aa[0].id != ad[0].id);
            fprintf(stderr,
                    "[sortcheck] %-8s tracks asc='%.28s' desc='%.28s' %s | "
                    "albums asc='%.24s' desc='%.24s' %s\n",
                    KS[i].name,
                    na ? ra[0].title : "-", nd ? rd[0].title : "-",
                    tdiff ? "OK" : "SAME",
                    ba ? aa[0].title : "-", bd ? ad[0].title : "-",
                    adiff ? "OK" : "SAME");
            if (!tdiff || !adiff) same++;
        }
        mn_app_destroy(a);
        fprintf(stderr, "[sortcheck] %s (%d/%d keys flagged SAME)\n",
                same ? "CHECK OUTPUT" : "ALL OK",
                same, (int)(sizeof(KS) / sizeof(KS[0])));
        return same ? 2 : 0;
    }

    /* --facetcheck [data_dir]: verify the Unknown facet buckets — the
     * last artist/genre facet row must be the vid=0 bucket, and drilling
     * into it (cascade value_id=0 -> id IS NULL) must return rows. */
    if (argc >= 2 && strcmp(argv[1], "--facetcheck") == 0) {
        char dd[1024];
        int fails = 0;
        if (argc >= 3) snprintf(dd, sizeof(dd), "%s", argv[2]);
        else           default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[facetcheck] app create failed\n"); return 1; }
        static const struct { int dim; const char *name; const char *unk; } DIMS[] = {
            {1, "artist", "Unknown artist"},   /* MN_FACET_ARTIST */
            {4, "genre",  "Unknown genre"},    /* MN_FACET_GENRE  */
        };
        for (size_t i = 0; i < sizeof(DIMS) / sizeof(DIMS[0]); ++i) {
            int32_t total = mn_app_facet_count(a, DIMS[i].dim);
            mn_facet_value v[2];
            int32_t got = total > 0
                ? mn_app_facet_window(a, DIMS[i].dim, total - 1, 1, v) : 0;
            bool has_unknown = got == 1 && v[0].id == 0
                && strcmp(v[0].label, DIMS[i].unk) == 0;
            fprintf(stderr, "[facetcheck] %-6s rows=%d last='%s' (id=%lld, %d trk) %s\n",
                    DIMS[i].name, (int)total, got ? v[0].label : "-",
                    got ? (long long)v[0].id : -1LL, got ? v[0].count : 0,
                    has_unknown ? "OK" : "no-unknown-bucket");
            if (has_unknown) {
                /* drill: cascade value_id=0 must produce that many rows
                 * (capped at the facet_tracks window size of 500). */
                static mn_row rows[500];
                int32_t n = mn_app_facet_tracks(a, DIMS[i].dim, 0, 500, rows);
                int32_t expect = v[0].count > 500 ? 500 : v[0].count;
                fprintf(stderr, "[facetcheck] %-6s drill vid=0 -> %d rows (expect %d) %s\n",
                        DIMS[i].name, (int)n, (int)expect,
                        n == expect ? "OK" : "FAIL");
                if (n != expect) fails++;
            }
            /* An untagged library w/o a bucket is legal only when NOTHING
             * is untagged — we can't see that here, so only the drill
             * mismatch is fatal. */
        }
        mn_app_destroy(a);
        fprintf(stderr, "[facetcheck] %s\n", fails ? "FAIL" : "ALL OK");
        return fails ? 2 : 0;
    }

    /* --purgemissing [data_dir]: drop every missing-flagged row (the same
     * operation as Settings -> Library -> Remove missing tracks). */
    if (argc >= 2 && strcmp(argv[1], "--purgemissing") == 0) {
        char dd[1024];
        if (argc >= 3) snprintf(dd, sizeof(dd), "%s", argv[2]);
        else           default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[purge] app create failed\n"); return 1; }
        long long n = (long long)mn_app_purge_missing(a);
        fprintf(stderr, "[purge] removed %lld missing row(s)\n", n);
        mn_app_destroy(a);
        return 0;
    }

    /* --reinfer [data_dir]: filename/folder tag-inference backfill (the
     * same operation as Settings -> Library -> Fix untagged tracks now). */
    if (argc >= 2 && strcmp(argv[1], "--reinfer") == 0) {
        char dd[1024];
        if (argc >= 3) snprintf(dd, sizeof(dd), "%s", argv[2]);
        else           default_data_dir(dd, sizeof(dd));
        mn_app *a = mn_app_create(dd);
        if (!a) { fprintf(stderr, "[reinfer] app create failed\n"); return 1; }
        long long n = (long long)mn_app_reinfer_untagged(a);
        fprintf(stderr, "[reinfer] filled tags on %lld row(s)\n", n);
        mn_app_destroy(a);
        return 0;
    }

    /* CEF sub-process (renderer / gpu / utility): the command line carries
     * --type=<...>. Route it straight into CEF via webview_run(NULL,...) —
     * do NOT create the app controller (db, audio device, ONNX model) in
     * sub-processes; only the browser process owns those. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--type=", 7) == 0) {
            return webview_run(NULL, "", "");
        }
    }

    char data_dir[1024];
    default_data_dir(data_dir, sizeof(data_dir));

    mn_app *app = mn_app_create(data_dir);
    if (!app) { fprintf(stderr, "failed to create app (db at %s)\n", data_dir); return 1; }

    /* No auto-scan — the library loads from the db; scanning is user-initiated. */
    (void)argc; (void)argv;

    /* Locate ui/ next to the exe (portable bundle) with a dev fallback. */
    char ui_dir[1200], art_dir[1200];
    snprintf(art_dir, sizeof(art_dir), "%s/art-cache", data_dir);
#ifdef _WIN32
    char exe[1024]; GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    snprintf(ui_dir, sizeof(ui_dir), "%s\\ui", exe);
    {
        char probe[1300]; snprintf(probe, sizeof(probe), "%s\\index.html", ui_dir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            char *s2 = strrchr(exe, '\\'); if (s2) { *s2 = 0;
                snprintf(ui_dir, sizeof(ui_dir), "%s\\ui", exe); }
        }
    }
#else
    snprintf(ui_dir, sizeof(ui_dir), "./ui");
#endif

    int rc = webview_run(app, ui_dir, art_dir);

    mn_app_destroy(app);
    return rc;
}
