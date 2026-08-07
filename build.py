#!/usr/bin/env python3
"""
Monatomic Music Player — portable multi-platform build.

Pick a target and it compiles the C backend + CEF frontend, then stages EVERY
runtime dependency (CEF Chromium, ONNX Runtime, fonts, UI) beside the exe so the
output folder is fully self-contained and portable.

    python build.py                     # auto-detect host target
    python build.py --target win-x64    # explicit
    python build.py --target linux-x64
    python build.py --target mac-arm64
    python build.py --list              # show supported targets
    python build.py --debug

Targets: win-x64, win-arm64, linux-x64, linux-arm64, mac-x64, mac-arm64.
Each target links the matching vendored libraries; only the host OS's target can
actually be *built* on that host (cross-OS compiling C+CEF isn't practical), but
the same source + this script build a self-contained bundle on each platform.
"""
import argparse
import faulthandler
import os
import platform
import shutil
import subprocess
import sys

# This machine intermittently wedges builds in opaque ways (WMI service hangs
# leak into platform.system() and vcvars64.bat). If the WHOLE build exceeds
# 10 min, dump every thread's stack to stderr and hard-exit so any hang is
# diagnosable instead of silent.
faulthandler.dump_traceback_later(600, exit=True)

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
VENDOR = os.path.join(ROOT, "vendor")
UI = os.path.join(ROOT, "ui")
FONTS = os.path.join(ROOT, "assets", "fonts")

# Source files (order irrelevant to cl/clang; each TU compiles independently).
SOURCES = [
    os.path.join(VENDOR, "sqlite3.c"),
    os.path.join(SRC, "audio_engine.c"),
    # Pitch-preserved playback speed (WSOLA time-stretch; audiobooks).
    os.path.join(SRC, "stretch.c"),
    # HTTP streaming source (internet radio / podcasts): WinHTTP ring buffer,
    # ICY metadata, Range seek.
    os.path.join(SRC, "netstream.c"),
    os.path.join(SRC, "mf_decode.c"),
    os.path.join(SRC, "tags.c"),
    os.path.join(SRC, "tags_write.c"),
    os.path.join(SRC, "artcache.c"),
    os.path.join(SRC, "dsp.c"),
    os.path.join(SRC, "library_db.c"),
    os.path.join(SRC, "scanner.c"),
    os.path.join(SRC, "playback.c"),
    # playlists live in library_db.c (mn_playlist_*); playlists.c is a redundant
    # standalone duplicate that nothing includes — excluded to avoid LNK2005.
    os.path.join(SRC, "stems.c"),
    os.path.join(SRC, "depth.c"),
    os.path.join(SRC, "modeldl.c"),
    os.path.join(SRC, "sync.c"),
    # Stem export: audio-file writers (WAV/FLAC/MP3) + .mnstem ZIP container.
    os.path.join(SRC, "audio_write.c"),
    os.path.join(SRC, "stempack.c"),
    os.path.join(VENDOR, "miniz.c"),                     # ZIP container writer
    os.path.join(VENDOR, "shine", "layer3.c"),           # MP3 encoder (shine)
    os.path.join(VENDOR, "shine", "l3subband.c"),
    os.path.join(VENDOR, "shine", "l3mdct.c"),
    os.path.join(VENDOR, "shine", "l3loop.c"),
    os.path.join(VENDOR, "shine", "l3bitstream.c"),
    os.path.join(VENDOR, "shine", "huffman.c"),
    os.path.join(VENDOR, "shine", "bitstream.c"),
    os.path.join(VENDOR, "shine", "reservoir.c"),
    os.path.join(VENDOR, "shine", "tables.c"),
    os.path.join(SRC, "app.c"),
    os.path.join(SRC, "cef_host.c"),
    os.path.join(SRC, "main.c"),
]

TARGETS = ["win-x64", "win-arm64", "linux-x64", "linux-arm64", "mac-x64", "mac-arm64"]

VCVARS = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"


def host_target():
    # NOTE: platform.system()/machine() on Python 3.12 issue a WMI query on
    # Windows; a wedged WMI service makes them block FOREVER (observed on this
    # machine). sys.platform + PROCESSOR_ARCHITECTURE are plain constants.
    if sys.platform == "win32":
        mach = os.environ.get("PROCESSOR_ARCHITECTURE", "AMD64").lower()
        return "win-arm64" if "arm" in mach else "win-x64"
    mach = platform.machine().lower()
    arch = "arm64" if ("arm" in mach or "aarch64" in mach) else "x64"
    if sys.platform.startswith("linux"):
        return f"linux-{arch}"
    if sys.platform == "darwin":
        return f"mac-{arch}"
    return "win-x64"


def includes():
    return ["/I" + VENDOR, "/I" + os.path.join(VENDOR, "ort"),
            "/I" + os.path.join(VENDOR, "cef"), "/I" + SRC]


def unix_includes():
    return ["-I" + VENDOR, "-I" + os.path.join(VENDOR, "ort"),
            "-I" + os.path.join(VENDOR, "cef"), "-I" + SRC]


def load_vc_env():
    """Return the MSVC toolchain environment as a dict — WITHOUT vcvars64.bat.

    vcvars64.bat internally shells out to WMI-touching helpers, and a wedged
    WMI service makes it hang forever (observed repeatedly on this machine).
    The toolchain layout is static, so synthesize INCLUDE/LIB/PATH directly
    from the newest installed MSVC toolset + Windows 10/11 SDK.
    """
    vsroot = r"C:\Program Files\Microsoft Visual Studio\2022\Community"
    msvc_root = os.path.join(vsroot, "VC", "Tools", "MSVC")
    sdk_root = r"C:\Program Files (x86)\Windows Kits\10"

    def newest(d):
        try:
            vs = [v for v in os.listdir(d)
                  if os.path.isdir(os.path.join(d, v)) and v[0].isdigit()]
        except OSError:
            return None
        if not vs:
            return None
        return max(vs, key=lambda s: [int(x) for x in s.split(".") if x.isdigit()])

    msvc_ver = newest(msvc_root)
    sdk_ver = newest(os.path.join(sdk_root, "Include"))
    if not msvc_ver or not sdk_ver:
        print(f"toolchain not found (msvc={msvc_ver} sdk={sdk_ver})")
        return None

    msvc = os.path.join(msvc_root, msvc_ver)
    sdk_inc = os.path.join(sdk_root, "Include", sdk_ver)
    sdk_lib = os.path.join(sdk_root, "Lib", sdk_ver)
    sdk_bin = os.path.join(sdk_root, "bin", sdk_ver, "x64")

    env = dict(os.environ)
    env["INCLUDE"] = os.pathsep.join([
        os.path.join(msvc, "include"),
        os.path.join(sdk_inc, "ucrt"),
        os.path.join(sdk_inc, "um"),
        os.path.join(sdk_inc, "shared"),
        os.path.join(sdk_inc, "winrt"),
    ])
    env["LIB"] = os.pathsep.join([
        os.path.join(msvc, "lib", "x64"),
        os.path.join(sdk_lib, "ucrt", "x64"),
        os.path.join(sdk_lib, "um", "x64"),
    ])
    env["PATH"] = os.pathsep.join([
        os.path.join(msvc, "bin", "Hostx64", "x64"),   # cl, link
        sdk_bin,                                        # rc
        env.get("PATH", ""),
    ])
    # CreateProcess resolves the EXECUTABLE from the parent's PATH, not the
    # child env's — hand back absolute tool paths so callers don't rely on it.
    env["_MN_CL"] = os.path.join(msvc, "bin", "Hostx64", "x64", "cl.exe")
    env["_MN_RC"] = os.path.join(sdk_bin, "rc.exe")
    print(f"toolchain: MSVC {msvc_ver}, SDK {sdk_ver} (vcvars bypassed)")
    return env


def build_windows(target, debug):
    outdir = os.path.join(ROOT, "dist", target)
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "monatomic.exe")

    # CEF 144 ships a versioned C API. Left undefined, the headers select the
    # EXPERIMENTAL (unversioned) struct layout, which a distributed release
    # libcef.dll rejects at runtime ("CefApp_0_CToCpp called with invalid
    # version -1"). Pin the explicit API version to match the vendored DLL.
    cflags = ["/nologo", "/W3", "/D_CRT_SECURE_NO_WARNINGS", "/DSQLITE_ENABLE_FTS5",
              "/DWIN32_LEAN_AND_MEAN", "/DCEF_API_VERSION=14400"]
    if debug:
        cflags += ["/Zi", "/Od", "/MDd", "/D_DEBUG"]
    else:
        # Release: native-speed codegen.
        #   /O2  — full speed optimization
        #   /GL  — WHOLE-PROGRAM optimization: the compiler defers codegen so the
        #          LINKER (/LTCG) can inline + optimize ACROSS translation units.
        #          The single biggest MSVC native-speed win; requires /LTCG to link.
        #   /Oi  — emit intrinsics for memcpy/memset/etc. instead of lib calls.
        #   /Gy  — function-level linking so /OPT:REF can dead-strip unused funcs.
        #   /Gw  — same, for global data.
        # (No /arch:AVX2 — the exe also hosts CEF's processes and must run on any
        #  x64 CPU the user has; a non-AVX2 machine would #UD-crash. Left to /O2's
        #  baseline SSE2, matching a portable native build.)
        cflags += ["/O2", "/Oi", "/Gy", "/Gw", "/GL", "/MD", "/DNDEBUG"]

    libs = ["ole32.lib", "oleaut32.lib", "user32.lib", "advapi32.lib", "gdi32.lib",
            "shell32.lib", "shlwapi.lib", "uuid.lib", "winhttp.lib", "dxgi.lib", "dxguid.lib",
            # Media Foundation universal-decode backend (mf_decode.c): AAC/M4A/
            # ALAC/WMA/AC3/AIFF/Opus via IMFSourceReader. Built into Windows.
            "mfplat.lib", "mf.lib", "mfreadwrite.lib", "mfuuid.lib",
            # WIC (artcache.c): fallback decode for WEBP/HEIC/AVIF cover art
            # that stb_image has no codec for. Built into Windows.
            "windowscodecs.lib",
            os.path.join(VENDOR, "cef", "Release", "libcef.lib"),
            os.path.join(VENDOR, "ort", "onnxruntime.lib"),
            # Windows compatibility manifest (supportedOS + PerMonitorV2 DPI).
            # REQUIRED: this exe also hosts CEF's GPU process; without the
            # supportedOS GUIDs Chromium sees an ancient Windows version and
            # its DirectComposition path crash-loops the GPU process, dropping
            # the app to SwiftShader software compositing (16 fps at 4K).
            "/MANIFEST:EMBED",
            "/MANIFESTINPUT:" + os.path.join(SRC, "monatomic.manifest"),
            # GUI app: no console window on launch. main() stays the entry
            # point via mainCRTStartup; the diagnostic harnesses re-attach to
            # the parent console when run with -- flags (see main.c).
            "/SUBSYSTEM:WINDOWS",
            "/ENTRY:mainCRTStartup"]

    # Toolchain environment synthesized directly (vcvars64.bat is bypassed —
    # it hangs whenever this machine's WMI service wedges).
    env = load_vc_env()
    if env is None:
        print("BUILD FAILED (could not initialize VC environment)"); return 1

    # App icon + version info: compile src/monatomic.rc -> .res and hand it to
    # the linker so the exe carries the icon (Explorer, taskbar, Alt-Tab).
    # Absolute tool paths: CreateProcess resolves the exe from the PARENT's
    # PATH, not the child env's.
    res = os.path.join(outdir, "monatomic.res")
    rc_cmd = [env["_MN_RC"], "/nologo", "/fo", res,
              os.path.join(SRC, "monatomic.rc")]

    # Release linker: /LTCG pairs with /GL (link-time codegen across all TUs);
    # /OPT:REF dead-strips unreferenced code+data; /OPT:ICF folds identical
    # functions. These shrink the exe and speed hot paths. (Debug: none, to keep
    # link fast + debuggable.)
    link_opts = [] if debug else ["/LTCG", "/OPT:REF", "/OPT:ICF"]
    cl = [env["_MN_CL"]] + cflags + includes() + SOURCES + \
         ["/Fe:" + exe, "/Fo:" + os.path.join(outdir, "")] + ["/link"] + link_opts + libs + [res]
    print("Compiling (MSVC)…")
    r = subprocess.run(rc_cmd, env=env)
    if r.returncode != 0:
        print("BUILD FAILED (rc)"); return 1
    r = subprocess.run(cl, env=env)
    if r.returncode != 0:
        print("BUILD FAILED"); return 1

    stage_windows(outdir)
    print(f"BUILD OK -> {exe}")
    print(f"Portable bundle staged in dist/{target}/ (self-contained).")
    return 0


def stage_windows(outdir):
    """Copy every runtime dep beside the exe so the folder is self-contained."""
    cefrel = os.path.join(VENDOR, "cef", "Release")
    cefres = os.path.join(VENDOR, "cef", "Resources")
    # CEF runtime DLLs + support files — ESSENTIALS ONLY (the strip list is
    # mirrored for the mac/linux targets in build_unix):
    #   dropped dxcompiler.dll + dxil.dll (~26 MB): WebGPU shader compilers;
    #     the UI never uses WebGPU and cef_host disables the feature.
    #   dropped vulkan-1.dll: rendering is pinned to ANGLE-D3D11; the Vulkan
    #     backend is never selected.
    #   KEPT vk_swiftshader (5 MB): the software-GL fallback — without it a
    #     machine with a broken GPU driver renders a blank window.
    for f in ["libcef.dll", "chrome_elf.dll", "libEGL.dll", "libGLESv2.dll",
              "d3dcompiler_47.dll",
              "vk_swiftshader.dll", "v8_context_snapshot.bin",
              "vk_swiftshader_icd.json"]:
        copy_if(os.path.join(cefrel, f), outdir)
    # prune previously-staged non-essentials from dist (stage is additive)
    for f in ["dxcompiler.dll", "dxil.dll", "vulkan-1.dll"]:
        p = os.path.join(outdir, f)
        if os.path.isfile(p):
            os.remove(p)
    # CEF resources (paks + icu)
    for f in ["icudtl.dat", "resources.pak", "chrome_100_percent.pak",
              "chrome_200_percent.pak"]:
        copy_if(os.path.join(cefres, f), outdir)
    # locales: the app is en-US-only UI chrome (its own strings live in ui/);
    # CEF only fatal-requires the pak matching settings.locale. Shipping all
    # ~220 locale paks cost 47 MB for strings nothing ever reads.
    locales_src = os.path.join(cefres, "locales")
    locales_dst = os.path.join(outdir, "locales")
    if os.path.isdir(locales_src):
        os.makedirs(locales_dst, exist_ok=True)
        copy_if(os.path.join(locales_src, "en-US.pak"), locales_dst)
        for f in os.listdir(locales_dst):          # prune stale locales
            if f != "en-US.pak":
                os.remove(os.path.join(locales_dst, f))
    # ONNX Runtime (CUDA stems)
    ort = os.path.join(VENDOR, "ort")
    for f in ["onnxruntime.dll", "onnxruntime_providers_shared.dll",
              "onnxruntime_providers_cuda.dll"]:
        copy_if(os.path.join(ort, f), outdir)
    # cuDNN/cuBLAS/cuFFT for the CUDA execution provider. Vendored under
    # vendor/cuda/win-x64 so the repo is fully self-contained (they used to
    # live only in dist, with the originals in a since-deleted folder).
    # copy_if skips bytes when dst is already identical, so incremental
    # builds don't re-copy ~1.8 GB.
    cuda = os.path.join(VENDOR, "cuda", "win-x64")
    if os.path.isdir(cuda):
        for f in os.listdir(cuda):
            if f.lower().endswith(".dll"):
                copy_if(os.path.join(cuda, f), outdir)
    # UI (HTML/CSS/JS) + fonts
    shutil.copytree(UI, os.path.join(outdir, "ui"), dirs_exist_ok=True)
    if os.path.isdir(FONTS):
        os.makedirs(os.path.join(outdir, "assets", "fonts"), exist_ok=True)
        for f in os.listdir(FONTS):
            copy_if(os.path.join(FONTS, f), os.path.join(outdir, "assets", "fonts"))
    print("Staged: CEF runtime + resources + locales, ONNX Runtime, CUDA DLLs, ui/, fonts.")


def build_unix(target, debug):
    """macOS (clang, CoreAudio) / Linux (cc, ALSA/Pulse). Stages CEF+ORT+ui."""
    outdir = os.path.join(ROOT, "dist", target)
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "monatomic")
    cc = os.environ.get("CC", "clang" if target.startswith("mac") else "cc")
    cflags = ["-std=c11", "-DSQLITE_ENABLE_FTS5", "-Wall", "-Wno-unused-parameter"]
    cflags += (["-g", "-O0"] if debug else ["-O2", "-DNDEBUG"])
    ldflags = []
    if target.startswith("mac"):
        ldflags += ["-framework", "CoreFoundation", "-framework", "CoreAudio",
                    "-framework", "AudioToolbox", "-framework", "AppKit",
                    "-L" + os.path.join(VENDOR, "cef", "Release"), "-lcef",
                    "-lpthread", "-lm"]
    else:
        ldflags += ["-L" + os.path.join(VENDOR, "cef", "Release"), "-lcef",
                    "-lpthread", "-lm", "-ldl"]
    cmd = [cc] + cflags + unix_includes() + SOURCES + ["-o", exe] + ldflags
    print(f"Compiling ({cc})…")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("BUILD FAILED"); return 1
    shutil.copytree(UI, os.path.join(outdir, "ui"), dirs_exist_ok=True)
    print(f"BUILD OK -> {exe}  (stage CEF runtime + fonts beside it for a portable bundle)")
    return 0


def copy_if(src, dstdir):
    if not os.path.isfile(src):
        return
    dst = os.path.join(dstdir, os.path.basename(src))
    if os.path.isfile(dst):
        ss, ds = os.stat(src), os.stat(dst)
        # same size + mtime -> already staged; skip (matters for ~1.8 GB CUDA)
        if ss.st_size == ds.st_size and int(ss.st_mtime) == int(ds.st_mtime):
            return
    shutil.copy2(src, dstdir)


def main():
    ap = argparse.ArgumentParser(description="Monatomic portable multi-platform build")
    ap.add_argument("--target", choices=TARGETS, default=None)
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        print("Targets:", ", ".join(TARGETS))
        print("Host detected:", host_target())
        return 0

    target = args.target or host_target()
    print(f"== Monatomic build :: target {target} ==")

    if target.startswith("win"):
        if sys.platform != "win32":   # NOT platform.system(): that hits WMI
            print("Windows targets must be built on Windows."); return 1
        return build_windows(target, args.debug)
    else:
        return build_unix(target, args.debug)


if __name__ == "__main__":
    sys.exit(main())
