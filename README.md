# Monatomic

**A Windows music + audiobook player with a live neural stem mixer.**
Mute the singer of any song while it plays. Solo the drums. Export the stems.
Then browse 50,000 tracks with zero lag and pick your book back up mid-sentence.

Native C backend (audio, database, inference) + Chromium UI. No account, no
cloud, no telemetry. Free.

---

## Why it exists

Mainstream players treat a song as one opaque stream. Monatomic runs every
track through a neural network (HTDemucs, via ONNX Runtime) and gives you
**nine live faders** — sub bass, bass, vocals, lead, instruments, wide, air,
guitar, piano — over the music *as it plays*. GPU-accelerated on NVIDIA
(CUDA), automatic CPU fallback everywhere else, disk-cached so replays are
instant.

## Features

**Stems**
- Live 9-channel stem mixer: per-channel faders, meters, mute/solo, master fader, one-tap presets (Karaoke, A cappella, Drums only, …)
- Neural ↔ original A/B switch in real time
- One-click stem export: `.mnstem` containers (a self-describing ZIP) or loose files — WAV, FLAC, or MP3
- In-app AI model manager: curated registry + Hugging Face browser, downloads straight into the app

**Library**
- Built for huge collections: virtualized views stay smooth past 50,000 tracks
- Instant search (SQLite FTS5) across tracks, albums, artists; facet browsing by artist / genre / year / folder
- Per-kind isolation: music, audiobooks, and custom libraries (game music, scores, …) never bleed into each other
- Media Manager: batch art with review filmstrip, duplicate finder, tag fixer, batch lyrics, folder classifier
- Tag inference for untagged rips (filename/folder heuristics), unknown-value buckets so nothing hides

**Playback**
- Gapless, crossfade, ReplayGain, WASAPI exclusive (bit-perfect), per-device output selection
- Wide format support: MP3, FLAC, M4A/ALAC, WAV, OGG/Opus, WMA, AIFF, and more
- 10-band EQ with presets, limiter, balance, preamp — in the native audio path
- Waveform seekbar, A-B repeat, sleep timer, playback queue

**Album art**
- Art that actually shows up: single-source cache served directly, self-healing integrity sweep, embedded → sidecar → folder extraction, HEIC/AVIF fallback
- 12-source online cover search with per-image attribution; paste art from the clipboard
- Five browse modes, including iTunes-style **3D Cover Flow** and a **volumetric depth-mapped now-playing cover** (monocular depth estimation, live WebGL mesh)

**Audiobooks**
- Continue Listening shelf: one click resumes the exact chapter and position
- Pitch-preserved speed 0.75×–3× (WSOLA time-stretch — no chipmunk voices)
- Bookmarks, ±30s/±5min skips, progress badges, finished flags, natural chapter ordering for untagged rips
- Progress is keyed to a content fingerprint, so it survives file moves and renames

**Design**
- 33 individually configurable animation channels (Settings → Animations) — spotlight, tilt/parallax, magnetic buttons, cinematic transitions
- Everything compositor-only and parked at idle: ~1% idle CPU
- Respects `prefers-reduced-motion`; global off-switch and Low Power mode

**Private by construction**
- Portable or installed — either way your data stays in `%APPDATA%\Monatomic`
- No account, no cloud, no analytics. The network is touched only when you ask (model downloads, online art/lyrics search)

## Install

Grab the latest [release](https://github.com/Tendai2404/Monatomic/releases):

| Package | For |
|---|---|
| **MonatomicSetup-x.y.z.exe** | Normal install: Program Files, Start Menu, uninstaller |
| **Monatomic-x.y.z-win-x64.zip** | Portable: unzip anywhere and run `monatomic.exe` |

Windows 10/11 x64. An NVIDIA GPU accelerates stem separation; without one the
CPU path is used automatically (slower, same results).

## Build from source

```
python build.py --target win-x64
```

Requirements: Python 3, Visual Studio 2022 (MSVC + Windows 10/11 SDK). The
repo carries all vendored *source* dependencies (SQLite, miniaudio, dr_libs,
stb, miniz, shine). Three binary distributions are too large for git and go
under `vendor/` yourself:

| Tree | What | Where from |
|---|---|---|
| `vendor/cef/` | CEF 144 Windows x64 binary distribution (`Release/`, `Resources/`) | [cef-builds.spotifycdn.com](https://cef-builds.spotifycdn.com/index.html) |
| `vendor/ort/` | ONNX Runtime (headers, `onnxruntime.lib`, DLLs incl. CUDA EP) | [ONNX Runtime releases](https://github.com/microsoft/onnxruntime/releases) |
| `vendor/cuda/win-x64/` | cuBLAS 12 / cuDNN 9 / cuFFT 11 runtime DLLs (optional — CUDA acceleration only) | NVIDIA CUDA/cuDNN redistributables |

Output lands in `dist/win-x64/` as a self-contained portable bundle. The stem
model (`htdemucs_6s.onnx`) downloads in-app on first use, or ships with
releases.

Verify a build:

```
monatomic.exe --selftest     # 11 checks: DB/FTS5, decode, encode, device, art,
                             # fingerprint, neural stems (reports CUDA or CPU)
monatomic.exe --bench        # performance benchmark
```

The installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php):
`ISCC.exe installer\monatomic.iss`.

## License

[Monatomic Noncommercial License](LICENSE.md) — use it, modify it, share it,
don't sell it. Third-party components under their own licenses:
[THIRD-PARTY.md](THIRD-PARTY.md).

## Support the project

- **Report issues** → [GitHub Issues](https://github.com/Tendai2404/Monatomic/issues) (or the 🐞 button in the app)
- **Bitcoin** → `1KWgwdNdSir2jeLTAcJzsbw96BCrWczjHD`
- **PayPal** → [donate](https://www.paypal.com/donate/?business=stinger2404%40gmail.com&currency_code=USD)
