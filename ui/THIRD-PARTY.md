# Third-party components

Monatomic bundles the following third-party software. Each component is
governed by its own license, which travels with this distribution; the
Monatomic Noncommercial License applies only to the Monatomic code.

| Component | Purpose | License |
|---|---|---|
| Chromium Embedded Framework (CEF) + Chromium | UI runtime | BSD 3-Clause (Chromium: BSD and other permissive; see cef/ LICENSE files) |
| miniaudio | audio device I/O, decoding, WAV encode | MIT-0 / Public Domain (dual) |
| SQLite (+ FTS5) | library database | Public Domain |
| ONNX Runtime (+ optional CUDA/cuDNN EPs) | neural stem separation, depth | MIT (CUDA/cuDNN redistributables under NVIDIA license terms) |
| HTDemucs model (htdemucs_6s) | stem separation weights | Demucs — MIT (Meta AI research release) |
| stb_image / stb_image_write / stb_image_resize | image decode/encode/resize | MIT / Public Domain (dual) |
| dr_libs (dr_mp3, dr_flac, dr_wav) | audio decoding | MIT-0 / Public Domain (dual) |
| shine | MP3 encoding (stem export) | LGPL 2.1 — source for the vendored copy ships in `vendor/shine/`; the full application source is distributed, satisfying relink/source obligations |
| miniz | ZIP container (.mnstem export) | MIT |
| Manrope | UI typeface | SIL Open Font License 1.1 |

Where a component offers a choice of license (dual-licensed), Monatomic
elects the most permissive option unless noted.

If you redistribute Monatomic (as the license permits, noncommercially),
keep this file and the bundled license texts intact.
