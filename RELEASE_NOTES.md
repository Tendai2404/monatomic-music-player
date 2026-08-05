# Monatomic Music Player — v1.0.0

The first release. A native C audio player with a Chromium UI, built for
very large libraries (tested against 52,000+ tracks), neural stem
separation, audiobooks, and a native-smooth feel.

## Highlights

### Library & playback
- Windowed virtual library — smooth at 50k+ tracks, instant search (FTS5
  with prefix indexes), facet browsing (artists / genres / years / folders)
- Gapless playback, crossfade, ReplayGain, WASAPI exclusive mode,
  multichannel passthrough, Media Foundation universal decode
  (MP3/FLAC/M4A/ALAC/WMA/Opus/…)
- Album grid with resizable tiles (slider + Ctrl+scroll), Details,
  Carousel, Rolodex, and 3D Cover Flow views
- Playlists, liked/smart views, sleep timer, A‑B repeat, waveform seekbar

### Album art, done right
- Single-source-of-truth art cache served directly — art always shows,
  in every view, cold or warm (Winamp/WMP-grade consistency)
- Periodic self-healing integrity sweep; embedded → sidecar → parent-folder
  extraction (jpg/png/bmp/gif/webp), hi‑res covers on demand
- Tag editor cover search across **12 sources** (iTunes, Deezer,
  MusicBrainz/CAA, Last.fm, DuckDuckGo, Bing, Google, Yandex, Discogs,
  Bandcamp, Archive.org, Genius) with per-image source attribution
- Right-click any album → **Paste album art** straight from the clipboard
- Media Manager: batch art with review mode, film strip, perceptual
  duplicate matching, batch lyrics, tag consistency fixer, duplicate
  finder with recycle-bin delete, folder classifier

### Neural stems
- Live 9-channel neural stem separation (HTDemucs) with a modern mixer:
  per-channel faders/meters/mute/solo, master fader, honest presets,
  CUDA acceleration with CPU fallback, lazy model loading
- One-click stem export — per-track `.mnstem` containers (WAV/FLAC/MP3)
  or loose files
- Blue separation-progress banner behind the waveform

### Audiobooks
- Fully separated audiobook library (plus custom libraries: OST, game
  music, …) — never mixes with music
- **Continue Listening shelf** — recent books with progress bars,
  one-click resume to the exact chapter and position
- Per-chapter positions, whole-book percent, finished flags and progress
  badges on covers
- **Pitch-preserved playback speed 0.75×–3×** (WSOLA time-stretch)
- Bookmarks, ±30s/±5min skip, natural chapter ordering for untagged
  rips, book-aware launch resume
- Move-proof progress: an immutable per-file content fingerprint keys all
  progress, so it survives file moves/renames and is device-sync-ready

### Motion design
- 33 individually configurable animation channels (Settings → Animations):
  staggered entrances, cursor spotlight, 3D cover tilt + parallax,
  magnetic buttons, like bursts, cinematic view transitions, playing
  glow, count-ups, title scramble, easing personalities, and more
- Everything compositor-only and parked at idle — the motion system adds
  **zero** idle CPU

### Performance
- Idle browser-process CPU ~1%, boot memory halved via lazy model
  loading, LTCG whole-program optimized build
- Respects `prefers-reduced-motion` and a global animations-off switch;
  Low Power mode for weak machines

## Requirements
- Windows 10/11 x64. Fully portable — unzip and run `monatomic.exe`.
- NVIDIA GPU (CUDA) accelerates stem separation; CPU fallback included.

## Notes
- Library, art cache, and settings live in `%APPDATA%\Monatomic`.
- First launch scans your folders and builds art in the background.
