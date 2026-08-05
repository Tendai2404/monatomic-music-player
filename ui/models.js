/* ============================================================
   MONATOMIC — AI Models view
   Curated ONNX model registry (models.json) merged with live
   Hugging Face listings, hardware capability detection, and
   per-model performance inference (current + projected after
   upcoming kernel/quantization optimizations).
   app.js opens this view via MnModels.open({ send, on }).
   ============================================================ */
window.MnModels = (function () {
  "use strict";

  /* ---------- helpers ---------- */
  const $  = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  const HF_CACHE_KEY = "mn.hf.models.v1";
  const HF_CACHE_MS  = 60 * 60 * 1000; /* 1 h */
  const KINDS = [
    { id: "stems", label: "Stem Separation", sub: "Real-time neural source separation for the stem mixer" },
    { id: "depth", label: "Depth Estimation", sub: "Monocular depth for volumetric 3D album art" },
  ];
  const TIER_LABEL = { medium: "Medium", high: "High", higher: "Higher", highest: "Highest", sota: "SOTA" };

  /* ---------- state ---------- */
  let bridge = null;        /* { send, on } from app.js            */
  let inited = false;
  let root = null;          /* #models-scroll                      */
  let registry = null;      /* parsed models.json                  */
  let registryErr = "";
  let hw = null;            /* detected hardware                   */
  let hf = { status: "idle", note: "", stems: [], depth: [] };
  const hfDetail = {};      /* repo id -> {loading,loaded,bytes,count,error} */
  const dl = {};            /* model id -> {pct,started,got,done,error,note} */
  const modelById = {};
  /* Active (persisted) model FILENAME per kind, from the backend. Empty until
     the first selectedmodels reply lands. */
  const activeFile = { stems: "", depth: "" };
  const pendingSel = { stems: "", depth: "" }; /* optimistic UI while awaiting reply */

  /* ============================================================
     HARDWARE DETECTION
     GPU name -> effective FP16 TFLOPS + typical VRAM. Coarse on
     purpose: it drives the perf inference, not scheduling.
     Longest names first so "4070 Ti Super" wins over "4070".
     ============================================================ */
  const GPU_DB = [
    ["RTX 5090", 105, 32], ["RTX 5080", 57, 16], ["RTX 5070 TI", 44, 16], ["RTX 5070", 31, 12],
    ["RTX 5060 TI", 24, 16], ["RTX 5060", 19, 8],
    ["RTX 4090", 82, 24], ["RTX 4080 SUPER", 52, 16], ["RTX 4080", 49, 16],
    ["RTX 4070 TI SUPER", 44, 16], ["RTX 4070 TI", 40, 12], ["RTX 4070 SUPER", 35, 12], ["RTX 4070", 29, 12],
    ["RTX 4060 TI", 22, 8], ["RTX 4060", 15, 8], ["RTX 4050", 9, 6],
    ["RTX 3090 TI", 40, 24], ["RTX 3090", 36, 24], ["RTX 3080 TI", 34, 12], ["RTX 3080", 30, 10],
    ["RTX 3070 TI", 22, 8], ["RTX 3070", 20, 8], ["RTX 3060 TI", 16, 8], ["RTX 3060", 13, 12], ["RTX 3050", 9, 8],
    ["RTX 2080 TI", 27, 11], ["RTX 2080 SUPER", 22, 8], ["RTX 2080", 20, 8],
    ["RTX 2070 SUPER", 18, 8], ["RTX 2070", 15, 8], ["RTX 2060 SUPER", 14, 8], ["RTX 2060", 13, 6],
    ["GTX 1660 TI", 11, 6], ["GTX 1660 SUPER", 10, 6], ["GTX 1660", 10, 6], ["GTX 1650", 6, 4],
    ["GTX 1080 TI", 11, 11], ["GTX 1080", 9, 8], ["GTX 1070", 6.5, 8], ["GTX 1060", 4.4, 6],
    ["RX 7900 XTX", 61, 24], ["RX 7900 XT", 52, 20], ["RX 7800 XT", 37, 16], ["RX 7700 XT", 35, 12], ["RX 7600", 21, 8],
    ["RX 6950 XT", 47, 16], ["RX 6900 XT", 46, 16], ["RX 6800 XT", 42, 16], ["RX 6700 XT", 26, 12], ["RX 6600", 18, 8],
    ["ARC B580", 27, 12], ["ARC A770", 39, 16], ["ARC A750", 34, 8], ["ARC A380", 8, 6],
  ];

  function lookupGpu(name) {
    const u = (name || "").toUpperCase();
    for (const row of GPU_DB) {
      if (u.indexOf(row[0]) !== -1) return { tflops: row[1], vram_gb: row[2], matched: row[0] };
    }
    if (/RTX/.test(u))            return { tflops: 15,  vram_gb: 8,    matched: "RTX-class (generic)" };
    if (/GTX/.test(u))            return { tflops: 6,   vram_gb: 6,    matched: "GTX-class (generic)" };
    if (/RADEON|\bRX\b/.test(u))  return { tflops: 14,  vram_gb: 8,    matched: "Radeon-class (generic)" };
    if (/\bARC\b/.test(u))        return { tflops: 15,  vram_gb: 8,    matched: "Arc-class (generic)" };
    if (/IRIS|UHD|INTEL\(R\) HD|VEGA|RADEON\(TM\)|780M|760M|INTEGRATED|APU/.test(u))
                                  return { tflops: 2.5, vram_gb: null, matched: "integrated graphics" };
    return { tflops: 4, vram_gb: null, matched: "unknown GPU (conservative estimate)" };
  }

  function detectBrowserHw() {
    let raw = "";
    try {
      const c = document.createElement("canvas");
      const gl = c.getContext("webgl") || c.getContext("experimental-webgl");
      if (gl) {
        const dbg = gl.getExtension("WEBGL_debug_renderer_info");
        raw = String(gl.getParameter(dbg ? dbg.UNMASKED_RENDERER_WEBGL : gl.RENDERER) || "");
      }
    } catch (_) {}
    /* "ANGLE (NVIDIA, NVIDIA GeForce RTX 4060 Ti (0x2803) Direct3D11 ...)" -> clean name */
    const m = raw.match(/ANGLE \([^,]+,\s*([^(]+?)(?:\s*\(0x[0-9A-Fa-f]+\))?\s*(?:Direct3D|D3D|Vulkan|OpenGL|$)/i);
    const clean = ((m && m[1]) || raw).replace(/\s*\(0x[0-9A-Fa-f]+\)\s*/g, " ").replace(/Direct3D.*$/i, "").trim();
    const look = lookupGpu(clean || raw);
    return {
      source: "browser",
      gpu_name: clean || raw || "Unknown GPU",
      tflops: look.tflops,
      vram_gb: look.vram_gb,
      matched: look.matched,
      cores: navigator.hardwareConcurrency || null,
      mem_gb: navigator.deviceMemory || null,
      cuda: null, tensorrt: null, directml: null, npu: null, cpu_avx512: null,
    };
  }

  /* Authoritative C-side capabilities override the browser guess. */
  function applyHwcaps(m) {
    if (!m || (!m.gpu_name && m.vram_gb == null)) return;
    const name = m.gpu_name || (hw && hw.gpu_name) || "";
    const look = lookupGpu(name);
    hw = {
      source: "backend",
      gpu_name: name || "Unknown GPU",
      tflops: (typeof m.tflops === "number" && m.tflops > 0) ? m.tflops : look.tflops,
      vram_gb: (m.vram_gb != null ? m.vram_gb : look.vram_gb),
      matched: look.matched,
      cores: (m.cores != null ? m.cores : (hw && hw.cores)) || navigator.hardwareConcurrency || null,
      mem_gb: (m.ram_gb != null ? m.ram_gb : (hw && hw.mem_gb)) || navigator.deviceMemory || null,
      cuda: !!m.cuda, tensorrt: !!m.tensorrt, directml: !!m.directml,
      npu: !!m.npu, cpu_avx512: !!m.cpu_avx512,
    };
    renderAll();
  }

  /* ============================================================
     PERFORMANCE INFERENCE
     est_rt  = tflops / (params_m * arch_cost * 0.55)       (stems)
     est_ms  = 2 * params_m * arch_cost / tflops             (depth)
     future  = current * arch_future_gain (upcoming kernels,
               int8/int4 quantization, sparsity — from registry)
     ============================================================ */
  function perfFor(model) {
    if (!hw || !model.params_m) return null;
    const cost = model.arch_cost || 1;
    const rt = hw.tflops / (model.params_m * cost * 0.55);
    const ms = (2 * model.params_m * cost) / hw.tflops;
    const gain = model.arch_future_gain || 1.4;
    const minv = model.requirements ? model.requirements.min_vram_gb : null;
    const vramOk = hw.vram_gb == null || minv == null || hw.vram_gb >= minv;
    let verdict, vcls;
    if (!vramOk)                                            { verdict = "Needs more VRAM"; vcls = "v-vram"; }
    else if (rt >= 2 || (model.kind === "depth" && ms < 200)) { verdict = "Runs great";     vcls = "v-great"; }
    else if (rt >= 1)                                       { verdict = "Realtime";        vcls = "v-rt"; }
    else                                                    { verdict = "Below realtime";  vcls = "v-below"; }
    return { rt, ms, gain, verdict, vcls, vramOk };
  }
  const fmtRt = (rt) => clamp(rt, 0.1, 99).toFixed(1) + "×";
  const fmtMs = (ms) => (ms < 1 ? "<1" : ms >= 100 ? String(Math.round(ms)) : ms.toFixed(1)) + " ms";

  /* ---------- misc formatting ---------- */
  function fmtMB(mb) {
    if (mb == null) return "—";
    return mb >= 1024 ? (mb / 1024).toFixed(2).replace(/\.?0+$/, "") + " GB" : Math.round(mb) + " MB";
  }
  const fmtBytes = (b) => fmtMB(b / 1048576);
  function fmtCount(n) {
    n = n || 0;
    if (n >= 1e6) return (n / 1e6).toFixed(1) + "M";
    if (n >= 1e3) return (n / 1e3).toFixed(1) + "k";
    return String(n);
  }
  function fmtDate(s) { try { const d = new Date(s); return isNaN(d) ? "" : d.toISOString().slice(0, 10); } catch (_) { return ""; } }
  function fmtAgo(ts) {
    const min = Math.max(0, Math.round((Date.now() - ts) / 60000));
    return min < 1 ? "just now" : min < 60 ? min + " min ago" : Math.round(min / 60) + " h ago";
  }

  /* ============================================================
     CURATED REGISTRY (models.json, staged next to the UI)
     ============================================================ */
  async function loadRegistry() {
    try {
      const r = await fetch("models.json", { cache: "no-cache" });
      registry = await r.json();
      registryErr = "";
    } catch (_) {
      registry = registry || { models: [], hf_search: {} };
      registryErr = "could not load models.json";
    }
    ((registry && registry.models) || []).forEach((m) => { modelById[m.id] = m; });
  }

  function curatedRepoIds() {
    const set = new Set();
    ((registry && registry.models) || []).forEach((m) => {
      if (m.hf_repo && !/^search:/.test(m.hf_repo)) set.add(m.hf_repo.toLowerCase());
    });
    return set;
  }

  /* ============================================================
     HUGGING FACE SYNC (direct fetch; page runs with
     --disable-web-security so cross-origin is allowed)
     ============================================================ */
  function hfFetchList(q) {
    let url = "https://huggingface.co/api/models?search=" + encodeURIComponent(q.query) +
              "&sort=downloads&direction=-1&limit=10";
    if (q.pipeline) url += "&pipeline_tag=" + encodeURIComponent(q.pipeline);
    return fetch(url).then((r) => { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); });
  }

  async function syncHF(force) {
    if (hf.status === "syncing") return;
    if (!force) {
      try {
        const c = JSON.parse(localStorage.getItem(HF_CACHE_KEY) || "null");
        if (c && Date.now() - c.ts < HF_CACHE_MS) {
          hf = { status: "done", note: "synced " + fmtAgo(c.ts), stems: c.stems || [], depth: c.depth || [] };
          renderAll();
          return;
        }
      } catch (_) {}
    }
    hf.status = "syncing"; hf.note = "querying huggingface.co…";
    renderAll();

    const searches = (registry && registry.hf_search) || {};
    const skip = curatedRepoIds();
    let anyOk = false;
    const out = { stems: [], depth: [] };

    for (const kind of ["stems", "depth"]) {
      const seen = new Set();
      const lists = await Promise.all(
        (searches[kind] || []).map((q) => hfFetchList(q).then((r) => { anyOk = true; return r; }).catch(() => []))
      );
      lists.forEach((list) => (list || []).forEach((m) => {
        const id = String(m.id || m.modelId || "");
        const key = id.toLowerCase();
        if (!id || seen.has(key) || skip.has(key)) return;
        seen.add(key);
        out[kind].push({
          id,
          downloads: m.downloads || 0,
          likes: m.likes || 0,
          lastModified: m.lastModified || "",
          pipeline: m.pipeline_tag || "",
        });
      }));
      out[kind].sort((a, b) => b.downloads - a.downloads);
      out[kind] = out[kind].slice(0, 12);
    }

    if (anyOk) {
      hf = { status: "done", note: "synced just now", stems: out.stems, depth: out.depth };
      try { localStorage.setItem(HF_CACHE_KEY, JSON.stringify({ ts: Date.now(), stems: out.stems, depth: out.depth })); } catch (_) {}
    } else {
      hf.status = "offline";
      hf.note = "offline — showing curated registry";
    }
    renderAll();
  }

  /* Lazy per-repo detail: sum .onnx sibling sizes, estimate params. */
  function loadHfDetail(id) {
    const d = hfDetail[id];
    if (d && (d.loading || d.loaded)) return;
    hfDetail[id] = { loading: true };
    updateHfDetailUI(id);
    fetch("https://huggingface.co/api/models/" + id + "?blobs=true")
      .then((r) => { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
      .then((m) => {
        let bytes = 0, count = 0;
        (m.siblings || []).forEach((s) => {
          if (/\.onnx$/i.test(s.rfilename || "")) { count++; bytes += s.size || 0; }
        });
        hfDetail[id] = { loaded: true, bytes, count };
        updateHfDetailUI(id);
      })
      .catch(() => { hfDetail[id] = { loaded: true, error: true }; updateHfDetailUI(id); });
  }

  function hfDetailContent(box, id, kind) {
    box.innerHTML = "";
    const d = hfDetail[id];
    if (!d) {
      const b = el("button", "btn-mini", "Inspect files");
      b.addEventListener("click", () => loadHfDetail(id));
      box.appendChild(b);
      return;
    }
    if (d.loading) { box.appendChild(el("span", "hf-dim", "reading repo metadata…")); return; }
    if (d.error)   { box.appendChild(el("span", "hf-dim", "details unavailable")); return; }
    if (!d.count)  { box.appendChild(el("span", "hf-dim", "no .onnx files in repo — may need conversion")); return; }
    const paramsM = d.bytes / 4 / 1e6; /* fp32 weight bytes -> params */
    box.appendChild(el("span", "mstat", d.count + (d.count === 1 ? " ONNX file" : " ONNX files")));
    box.appendChild(el("span", "mstat", fmtBytes(d.bytes)));
    box.appendChild(el("span", "mstat", "≈" + Math.round(paramsM) + " M params (fp32 est.)"));
    if (hw && paramsM > 0) {
      const fake = { params_m: paramsM, arch_cost: 1.2, arch_future_gain: 1.6, kind };
      const p = perfFor(fake);
      if (p) {
        box.appendChild(el("span", "hf-dim",
          kind === "depth" ? "≈" + fmtMs(p.ms) + " per cover on " + hw.gpu_name
                           : "≈" + fmtRt(p.rt) + " realtime on " + hw.gpu_name));
      }
    }
  }
  function updateHfDetailUI(id) {
    $$('.hf-card[data-repo]', root).forEach((card) => {
      if (card.dataset.repo !== id) return;
      const box = $(".hf-detail", card);
      if (box) hfDetailContent(box, id, card.dataset.kind);
    });
  }

  /* ============================================================
     DOWNLOADS (bridge: -> downloadmodel, <- modeldl)
     ============================================================ */
  /* A model is downloadable only when it has a real HF repo (not a "search:"
     placeholder) AND a real file path, and is not explicitly marked otherwise. */
  function isDownloadable(model) {
    if (model.downloadable === false) return false;
    return !!(model.hf_repo && !/^search:/.test(model.hf_repo) && model.file);
  }

  function startDownload(model) {
    if (!isDownloadable(model)) {
      dl[model.id] = { pct: 0, started: 0, got: false, done: false,
                       error: "no verified download for this model", note: "" };
      updateDlUI(model.id);
      return;
    }
    dl[model.id] = { pct: 0, started: Date.now(), got: false, done: false, error: "", note: "" };
    bridge.send({
      cmd: "downloadmodel",
      id: model.id,
      repo: model.hf_repo,
      file: model.file,
      save_as: model.save_as || "",
    });
    /* If the backend never answers at all (host not wired), surface that rather
       than spin forever. Real progress/error replies clear this. */
    setTimeout(() => {
      const s = dl[model.id];
      if (s && !s.got && !s.done && !s.error) {
        s.error = "no response from downloader";
        s.started = 0;
        updateDlUI(model.id);
      }
    }, 20000);
    updateDlUI(model.id);
  }

  /* Map the terse backend error strings to something a user can act on. */
  function friendlyDlError(raw) {
    const e = String(raw || "").toLowerCase();
    if (/http\s*404/.test(e)) return "Not available at this URL (404)";
    if (/http\s*40[13]/.test(e)) return "Access denied (gated or private repo)";
    if (/http\s*5\d\d/.test(e)) return "Hugging Face server error — try again later";
    if (/http\s*\d+/.test(e)) return "Download rejected (" + raw + ")";
    if (/connect|send|no response|stalled/.test(e)) return "Network error — check your connection";
    if (/disk|temp file|rename/.test(e)) return "Could not write the file to disk";
    if (/busy/.test(e)) return "Another download is in progress";
    if (/invalid/.test(e)) return "Invalid download request";
    return String(raw || "download failed");
  }

  function onModelDl(m) {
    if (!m || m.id == null) return;
    const s = dl[m.id] || (dl[m.id] = { started: Date.now() });
    s.got = true; s.note = "";
    if (m.pct != null) s.pct = clamp(+m.pct, 0, 100);
    if (m.error) { s.error = friendlyDlError(m.error); s.started = 0; }
    if (m.done) {
      s.done = true; s.pct = 100;
      bridge.send({ cmd: "modelfiles" });   /* refresh the on-disk list */
    }
    updateDlUI(m.id);
  }

  /* Radio-like "Use this model" control shown on every INSTALLED card. */
  function renderSelectControl(box, model) {
    const active = isActive(model);
    const row = el("div", "mc-select");
    if (active) {
      row.appendChild(el("span", "sel-active", "✓ Active"));
      if (model.kind === "stems" && pendingSel.stems === modelFile(model))
        row.appendChild(el("span", "sel-note", "restart to apply"));
    } else {
      const btn = el("button", "btn-mini use-model", "Use this model");
      btn.addEventListener("click", () => selectModel(model));
      row.appendChild(btn);
      if (model.kind === "stems")
        row.appendChild(el("span", "sel-note", "needs restart"));
    }
    box.appendChild(row);
  }

  function renderActions(box, model) {
    box.innerHTML = "";
    const s = dl[model.id];
    const installed = model.bundled || (s && s.done);
    if (installed) {
      if (model.bundled) {
        box.appendChild(el("span", "dl-ok", "Ships with the app"));
      } else {
        /* Freshly downloaded models land in %APPDATA%\Monatomic\ai-models\. */
        box.appendChild(el("span", "dl-ok", "Downloaded — ready"));
      }
      renderSelectControl(box, model);
      return;
    }

    /* Models with no verified HF file: no download button, just an honest note. */
    if (!isDownloadable(model)) {
      const note = model.note ||
        "No verified ONNX download — needs manual conversion/import.";
      box.appendChild(el("span", "dl-note", note));
      return;
    }

    if (s && s.started && !s.error) {
      const wrap = el("div", "dl-wrap");
      const track = el("div", "dl-track");
      const fill = el("div", "dl-fill");
      fill.style.width = clamp(s.pct || 0, 0, 100) + "%";
      track.appendChild(fill);
      wrap.appendChild(track);
      wrap.appendChild(el("span", "dl-pct", (s.got ? Math.round(s.pct || 0) + "%" : "requesting…")));
      box.appendChild(wrap);
      return;
    }

    const label = (s && s.error) ? "↻  Retry download " + fmtMB(model.size_mb)
                                 : "⭳  Download " + fmtMB(model.size_mb);
    const btn = el("button", "btn-dl", label);
    btn.addEventListener("click", () => startDownload(model));
    box.appendChild(btn);
    if (s && s.error) box.appendChild(el("span", "dl-err", s.error));
    else if (s && s.note) box.appendChild(el("span", "dl-note", s.note));
  }
  function updateDlUI(id) {
    const model = modelById[id];
    if (!model || !root) return;
    $$('.model-card[data-mid]', root).forEach((card) => {
      if (card.dataset.mid !== String(id)) return;
      const box = $(".mc-actions", card);
      if (box) renderActions(box, model);
      const badges = $(".mc-badges", card);
      const s = dl[id];
      if (badges && s && s.done && !$(".badge.inst", badges)) {
        badges.insertBefore(el("span", "badge inst", "INSTALLED"), badges.firstChild);
      }
    });
  }

  /* ============================================================
     MODEL SELECTION (bridge: -> selectmodel / selectedmodels,
     <- modelselected / selectedmodels). The engine loads whichever
     downloaded model is "active" per kind; the depth change applies
     live, stems needs a restart.
     ============================================================ */
  /* The LOCAL filename the engine resolves for a model: the registry's
     save_as when present (repos like onnx-community store every model at
     "onnx/model.onnx", so save_as disambiguates them locally), otherwise the
     basename of model.file. */
  function modelFile(model) {
    if (model.save_as) return String(model.save_as);
    const f = String(model.file || "");
    const slash = Math.max(f.lastIndexOf("/"), f.lastIndexOf("\\"));
    return slash >= 0 ? f.slice(slash + 1) : f;
  }
  /* Files actually present in ai-models/ (from the modelfiles reply), so a
     model downloaded in a PREVIOUS session still shows as installed. */
  let diskFiles = {};
  function onModelFiles(m) {
    diskFiles = {};
    (m && m.files || []).forEach((f) => { diskFiles[String(f).toLowerCase()] = true; });
    renderAll();
  }
  /* A model is selectable once it is available on disk: bundled, on-disk from
     any session, or a completed download this session. */
  function isInstalled(model) {
    if (model.bundled) return true;
    if (dl[model.id] && dl[model.id].done) return true;
    const f = modelFile(model);
    return !!(f && diskFiles[f.toLowerCase()]);
  }
  function isActive(model) {
    const f = modelFile(model);
    if (!f) return false;
    const kind = model.kind;
    if (pendingSel[kind]) return pendingSel[kind] === f;
    return activeFile[kind] === f;
  }
  function selectModel(model) {
    const f = modelFile(model);
    if (!f) return;
    pendingSel[model.kind] = f;
    bridge.send({ cmd: "selectmodel", kind: model.kind, file: f });
    /* Refresh badges/buttons across this kind immediately (optimistic). */
    renderAll();
  }
  function onModelSelected(m) {
    if (!m || !m.kind) return;
    if (m.ok && m.file) activeFile[m.kind] = m.file;
    pendingSel[m.kind] = "";
    renderAll();
  }
  function onSelectedModels(m) {
    if (!m) return;
    if (typeof m.stems === "string") activeFile.stems = m.stems;
    if (typeof m.depth === "string") activeFile.depth = m.depth;
    renderAll();
  }

  /* ============================================================
     RENDER
     ============================================================ */
  function hwStat(label, value, title) {
    const s = el("div", "hw-stat");
    s.appendChild(el("div", "hw-k", label));
    const v = el("div", "hw-v", value == null ? "—" : String(value));
    if (title) v.title = title;
    s.appendChild(v);
    return s;
  }

  function renderHwCard() {
    const card = el("div", "hw-card");
    const head = el("div", "hw-head");
    head.appendChild(el("h3", "hw-title", "Neural Hardware"));
    head.appendChild(el("span", "chip ro hw-src",
      hw && hw.source === "backend" ? "engine-verified" : "browser estimate"));
    const spacer = el("div", "hw-spacer");
    head.appendChild(spacer);

    const sync = el("button", "btn btn-sync", hf.status === "syncing" ? "Syncing…" : "⇅  Sync with Hugging Face");
    sync.disabled = hf.status === "syncing";
    sync.addEventListener("click", () => syncHF(true));
    head.appendChild(sync);
    card.appendChild(head);

    const grid = el("div", "hw-grid");
    if (hw) {
      grid.appendChild(hwStat("GPU", hw.gpu_name, "matched profile: " + hw.matched));
      grid.appendChild(hwStat("FP16 throughput", "≈" + hw.tflops + " TFLOPS", "effective fp16 tensor throughput used for performance inference"));
      grid.appendChild(hwStat("VRAM", hw.vram_gb != null ? hw.vram_gb + " GB" : "unknown"));
      grid.appendChild(hwStat("CPU threads", hw.cores));
      grid.appendChild(hwStat("System RAM", hw.mem_gb != null ? "≈" + hw.mem_gb + " GB" : "unknown"));
    }
    card.appendChild(grid);

    /* acceleration chips — authoritative only once the backend reports */
    const accel = el("div", "hw-accel");
    if (hw && hw.source === "backend") {
      [["CUDA", hw.cuda], ["TensorRT", hw.tensorrt], ["DirectML", hw.directml], ["NPU", hw.npu], ["AVX-512", hw.cpu_avx512]]
        .forEach(([name, on]) => accel.appendChild(el("span", "chip ro accel" + (on ? " on" : " off"), name)));
    } else {
      accel.appendChild(el("span", "hf-dim", "Execution providers (CUDA / TensorRT / DirectML / NPU) will be verified by the audio engine."));
    }
    card.appendChild(accel);

    /* capability summary vs curated registry */
    if (hw && registry && registry.models && registry.models.length) {
      let rtOk = 0, total = 0;
      registry.models.forEach((m) => {
        const p = perfFor(m);
        if (!p) return;
        total++;
        if (p.vcls === "v-great" || p.vcls === "v-rt") rtOk++;
      });
      card.appendChild(el("div", "hw-note",
        rtOk + " of " + total + " curated models run realtime or better on this hardware."));
    }

    const status = el("div", "hw-sync-note");
    if (hf.note) status.textContent = hf.note;
    if (hf.status === "offline") status.classList.add("warn");
    card.appendChild(status);
    return card;
  }

  function tierChip(model) {
    const t = (model.quality && model.quality.tier) || "medium";
    return el("span", "tierchip tier-" + t, TIER_LABEL[t] || t);
  }

  function curatedCard(model) {
    const card = el("div", "model-card");
    card.dataset.mid = model.id;

    const head = el("div", "mc-head");
    const title = el("div", "mc-title");
    title.appendChild(el("div", "mc-name", model.name));
    title.appendChild(el("div", "mc-vendor", model.vendor || ""));
    head.appendChild(title);
    const badges = el("div", "mc-badges");
    if (model.bundled) {
      badges.appendChild(el("span", "badge inst", "INSTALLED"));
      badges.appendChild(el("span", "badge bund", "BUNDLED"));
    } else if (dl[model.id] && dl[model.id].done) {
      badges.appendChild(el("span", "badge inst", "INSTALLED"));
    }
    head.appendChild(badges);
    card.appendChild(head);

    /* quality row */
    const chips = el("div", "mc-chips");
    chips.appendChild(tierChip(model));
    if (model.quality && model.quality.sdr_db != null)
      chips.appendChild(el("span", "chip ro sdr", model.quality.sdr_db.toFixed(1) + " dB SDR"));
    if (model.requirements && model.requirements.cpu_realtime)
      chips.appendChild(el("span", "chip ro cpu-ok", "CPU realtime capable"));
    chips.appendChild(el("span", "chip ro", model.arch || "onnx"));
    card.appendChild(chips);

    /* size / params */
    const stats = el("div", "mc-stats");
    stats.appendChild(el("span", "mstat", fmtMB(model.size_mb) + " on disk"));
    stats.appendChild(el("span", "mstat", model.params_m + " M params"));
    card.appendChild(stats);

    /* stems list */
    if (model.stems && model.stems.length) {
      const st = el("div", "mc-stems");
      model.stems.forEach((s) => st.appendChild(el("span", "stem-tag", s)));
      card.appendChild(st);
    }

    /* hardware requirements */
    const req = model.requirements || {};
    const reqBits = [];
    if (req.min_vram_gb != null) reqBits.push((req.min_vram_gb === 0 ? "runs without dedicated VRAM" : req.min_vram_gb + " GB VRAM min"));
    if (req.recommended_vram_gb != null) reqBits.push(req.recommended_vram_gb + " GB recommended");
    if (req.fp16) reqBits.push("fp16");
    card.appendChild(el("div", "mc-req", "Requires: " + (reqBits.join(" · ") || "—")));

    if (model.quality && model.quality.notes) card.appendChild(el("div", "mc-notes", model.quality.notes));
    if (model.io) card.appendChild(el("div", "mc-io", "IO: " + model.io.input + "  →  " + model.io.output));

    /* performance inference */
    const p = perfFor(model);
    if (p && hw) {
      const perf = el("div", "mc-perf");
      const row = el("div", "perf-row");
      row.appendChild(el("span", "verdict " + p.vcls, p.verdict));
      row.appendChild(el("span", "perf-line",
        model.kind === "depth"
          ? "≈" + fmtMs(p.ms) + " per cover on " + hw.gpu_name
          : "≈" + fmtRt(p.rt) + " realtime on " + hw.gpu_name));
      perf.appendChild(row);
      const fut = el("div", "perf-future");
      fut.appendChild(el("span", "zap", "⚡"));
      fut.appendChild(el("span", null,
        "with upcoming kernel optimizations: " +
        (model.kind === "depth" ? "≈" + fmtMs(p.ms / p.gain) + " per cover" : "≈" + fmtRt(p.rt * p.gain) + " realtime")));
      fut.title = "Projected from architecture headroom (fused attention kernels, int8/int4 quantization, structured sparsity) not yet applied to the shipped file.";
      perf.appendChild(fut);
      card.appendChild(perf);
    }

    /* actions */
    const actions = el("div", "mc-actions");
    renderActions(actions, model);
    card.appendChild(actions);
    return card;
  }

  function hfCard(entry, kind) {
    const card = el("div", "model-card hf-card");
    card.dataset.repo = entry.id;
    card.dataset.kind = kind;

    const head = el("div", "mc-head");
    const title = el("div", "mc-title");
    title.appendChild(el("div", "mc-name hf-id", entry.id));
    title.appendChild(el("div", "mc-vendor", entry.pipeline || "community model"));
    head.appendChild(title);
    const badges = el("div", "mc-badges");
    badges.appendChild(el("span", "badge hfb", "HUGGING FACE"));
    head.appendChild(badges);
    card.appendChild(head);

    const stats = el("div", "mc-stats");
    stats.appendChild(el("span", "mstat", "⭳ " + fmtCount(entry.downloads) + " downloads"));
    stats.appendChild(el("span", "mstat", "♥ " + fmtCount(entry.likes)));
    if (entry.lastModified) stats.appendChild(el("span", "mstat", "updated " + fmtDate(entry.lastModified)));
    card.appendChild(stats);

    const detail = el("div", "hf-detail");
    hfDetailContent(detail, entry.id, kind);
    card.appendChild(detail);

    const link = el("div", "hf-url");
    link.textContent = "https://huggingface.co/" + entry.id;
    link.title = "Open on Hugging Face — text is selectable, copy it into your browser";
    card.appendChild(link);
    card.appendChild(el("div", "hf-dim", "Community repo — review the license and IO shape before importing."));
    return card;
  }

  function renderAll() {
    if (!root) return;
    root.innerHTML = "";
    root.appendChild(renderHwCard());

    if (registryErr) {
      const w = el("div", "hw-sync-note warn", registryErr);
      root.appendChild(w);
    }

    const models = (registry && registry.models) || [];
    KINDS.forEach((kind) => {
      const sec = el("section", "model-sec");
      const sh = el("div", "sec-head");
      sh.appendChild(el("h3", "sec-title", kind.label));
      sh.appendChild(el("span", "sec-sub", kind.sub));
      sec.appendChild(sh);

      const grid = el("div", "model-grid");
      models.filter((m) => m.kind === kind.id).forEach((m) => grid.appendChild(curatedCard(m)));
      sec.appendChild(grid);

      /* community subsection */
      const community = hf[kind.id] || [];
      const ch = el("div", "sec-head sec-head-sub");
      ch.appendChild(el("h4", "sec-title-sub", "Community (Hugging Face)"));
      ch.appendChild(el("span", "sec-sub",
        hf.status === "syncing" ? "syncing…" :
        hf.status === "offline" ? "offline — showing curated registry" :
        community.length ? community.length + " repos by downloads" : "no community results yet — press Sync"));
      sec.appendChild(ch);
      if (community.length) {
        const cgrid = el("div", "model-grid");
        community.forEach((e) => cgrid.appendChild(hfCard(e, kind.id)));
        sec.appendChild(cgrid);
      }
      root.appendChild(sec);
    });
  }

  /* ============================================================
     OPEN (called by app.js each time the nav entry is clicked)
     ============================================================ */
  function open(b) {
    bridge = b;
    root = $("#models-scroll");
    if (!inited) {
      inited = true;
      hw = detectBrowserHw();
      bridge.on("hwcaps", applyHwcaps);
      bridge.on("modeldl", onModelDl);
      bridge.on("modelselected", onModelSelected);
      bridge.on("selectedmodels", onSelectedModels);
      bridge.on("modelfiles", onModelFiles);
      loadRegistry().then(() => { renderAll(); syncHF(false); });
    } else {
      renderAll();
      syncHF(false); /* respects the 1 h cache */
    }
    bridge.send({ cmd: "hwcaps" }); /* backend override, when implemented */
    bridge.send({ cmd: "selectedmodels" }); /* which models are active */
    bridge.send({ cmd: "modelfiles" });     /* which files are on disk */
  }

  return { open };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("models", "1.0.0", [], function () { return window.MnModels || {}; });
