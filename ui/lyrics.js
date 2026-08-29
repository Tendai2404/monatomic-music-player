/* ============================================================
   MONATOMIC — Lyrics Aggregator
   Multi-provider lyric fetching (LRCLIB → Genius → AZLyrics →
   SongLyrics → Lyrics.com → Letras) with adaptive per-host rate
   limiting, UA rotation, a worker-pool batch fetcher, synced
   .lrc support, and the LIVE karaoke section that sits inline in
   the now-playing panel below the album art + spectrum
   (#np-lyrics-live — see the "LIVE LYRICS" block below).

   app.js boots this via MnLyrics.init(api) and opens the view
   via MnLyrics.open(api) — the same pattern as models.js.
   The page runs in CEF with --disable-web-security, so the
   provider HTTP + DOMParser parsing happens directly here.
   ============================================================ */
window.MnLyrics = (function () {
  "use strict";

  /* ---------- tiny helpers ---------- */
  const $  = (s, r) => (r || document).querySelector(s);
  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  const OPTS_KEY = "mn.lyrics.opts.v1";
  const MIN_LEN = 50;          /* first provider hit > 50 chars wins   */
  const MAX_RENDER = 4000;     /* table render cap (batch covers all)  */

  /* ---------- state ---------- */
  let B = null;                /* bridge api handed over by app.js     */
  let inited = false, built = false;
  let running = false, stopFlag = false;
  let readsDown = false;       /* lyricsread unanswered -> stop probing*/
  let writeNoted = false;
  let selectedId = null;
  let renderedCount = -1;

  const store = {};            /* id -> {status,source,text,lrc,existing,wrote} */
  const rowEls = {};           /* id -> row element                    */
  const stats = {};            /* provider id -> {ok,tried}            */
  const prog = { done: 0, total: 0, found: 0 };

  /* ---------- persisted options ---------- */
  function loadOpts() {
    let o = {};
    try { o = JSON.parse(localStorage.getItem(OPTS_KEY) || "{}"); } catch (_) {}
    return {
      providers: o.providers || {},
      skip: o.skip !== false,
      overwrite: !!o.overwrite,
      workers: clamp(o.workers || 3, 1, 8),
    };
  }
  const opts = loadOpts();
  function saveOpts() { try { localStorage.setItem(OPTS_KEY, JSON.stringify(opts)); } catch (_) {} }

  /* ============================================================
     ADAPTIVE RATE LIMITER + UA ROTATION
     min delay 250 ms per host; ×2 on network failure / HTTP 429
     (capped at ×10); decays ×0.9 toward 1 on success.
     ============================================================ */
  const UAS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:127.0) Gecko/20100101 Firefox/127.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Edg/125.0.0.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
  ];
  const pickUA = () => UAS[(Math.random() * UAS.length) | 0];
  const rate = {};             /* host -> {mult, next} */

  async function politeFetch(url, init) {
    let host = "";
    try { host = new URL(url).host; } catch (_) {}
    const r = rate[host] || (rate[host] = { mult: 1, next: 0 });
    const wait = r.next - Date.now();
    if (wait > 0) await sleep(wait + Math.random() * 60);
    r.next = Date.now() + 250 * r.mult;
    try {
      const resp = await fetch(url, Object.assign({ headers: { "User-Agent": pickUA() } }, init || {}));
      if (resp.status === 429 || resp.status >= 500) r.mult = Math.min(10, r.mult * 2);
      else if (resp.ok) r.mult = Math.max(1, r.mult * 0.9);
      return resp;
    } catch (e) {
      r.mult = Math.min(10, r.mult * 2);
      throw e;
    }
  }

  /* ---------- slugs + html helpers ---------- */
  const deaccent = (s) => String(s == null ? "" : s).normalize("NFD").replace(/[\u0300-\u036f]/g, "");
  /* generic slug: lowercase, non-alnum -> '-' collapsed */
  const slug = (s) => deaccent(s).toLowerCase().replace(/&/g, "and").replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
  /* AZLyrics slug: lowercase, strip everything non [a-z0-9] */
  const azslug = (s) => deaccent(s).toLowerCase().replace(/[^a-z0-9]/g, "");

  function parseHtml(html) {
    const doc = new DOMParser().parseFromString(html, "text/html");
    doc.querySelectorAll("br").forEach((br) => br.replaceWith(doc.createTextNode("\n")));
    return doc;
  }
  const tidy = (s) => String(s || "").replace(/\r/g, "").replace(/[ \t]+\n/g, "\n").replace(/\n{3,}/g, "\n\n").trim();

  function stripLrc(lrc) {
    return tidy(String(lrc || "").split("\n").map((l) => l.replace(/\[[^\]]*\]/g, "")).join("\n"));
  }
  const looksLrc = (s) => /\[\d{1,3}:\d{2}(?:[.:]\d{1,3})?\]/.test(String(s || ""));

  function parseLrc(lrc) {
    const out = [];
    for (const line of String(lrc || "").split(/\r?\n/)) {
      const marks = Array.from(line.matchAll(/\[(\d{1,3}):(\d{1,2}(?:\.\d{1,3})?)\]/g));
      if (!marks.length) continue;
      const text = line.replace(/\[[^\]]*\]/g, "").trim();
      for (const m of marks) out.push({ t: (+m[1]) * 60000 + parseFloat(m[2]) * 1000, text });
    }
    out.sort((a, b) => a.t - b.t);
    return out;
  }

  /* ============================================================
     PROVIDERS — chain order matters: first hit > 50 chars wins.
     ============================================================ */

  /* 1. LRCLIB — free JSON API, plain + synced .lrc */
  async function fromLrclib(t) {
    let url = "https://lrclib.net/api/get?artist_name=" + encodeURIComponent(t.artist || "") +
              "&track_name=" + encodeURIComponent(t.title || "");
    if (t.duration_ms) url += "&duration=" + Math.round(t.duration_ms / 1000);
    const r = await politeFetch(url);
    if (!r.ok) return null;
    const j = await r.json();
    const lrc = j && j.syncedLyrics ? String(j.syncedLyrics) : "";
    const plain = j && j.plainLyrics ? String(j.plainLyrics) : "";
    if (lrc && stripLrc(lrc).length > MIN_LEN) return { text: plain ? tidy(plain) : stripLrc(lrc), lrc };
    if (plain && plain.trim().length > MIN_LEN) return { text: tidy(plain) };
    return null;
  }

  /* 2. Genius — search/multi JSON, then scrape the song page */
  async function fromGenius(t) {
    const q = encodeURIComponent((t.artist || "") + " " + (t.title || ""));
    const sr = await politeFetch("https://genius.com/api/search/multi?q=" + q);
    if (!sr.ok) return null;
    const j = await sr.json();
    let url = null;
    const sections = (j && j.response && j.response.sections) || [];
    outer:
    for (const sec of sections) {
      for (const hit of (sec.hits || [])) {
        if (hit.type === "song" && hit.result && hit.result.url) { url = hit.result.url; break outer; }
      }
    }
    if (!url) return null;
    const pr = await politeFetch(url);
    if (!pr.ok) return null;
    const doc = parseHtml(await pr.text());
    const parts = doc.querySelectorAll('div[data-lyrics-container="true"]');
    if (!parts.length) return null;
    const chunks = [];
    parts.forEach((p) => chunks.push(p.textContent));
    const text = tidy(chunks.join("\n"));
    return text.length > MIN_LEN ? { text } : null;
  }

  /* 3. AZLyrics — slugged URL; lyrics live in the first bare div */
  async function fromAz(t) {
    const a = azslug(String(t.artist || "").replace(/^\s*the\s+/i, ""));
    const s = azslug(t.title || "");
    if (!a || !s) return null;
    const r = await politeFetch("https://www.azlyrics.com/lyrics/" + a + "/" + s + ".html");
    if (!r.ok) return null;
    const doc = parseHtml(await r.text());
    for (const d of doc.querySelectorAll("div")) {
      if (d.className || d.id) continue;
      const text = tidy(d.textContent);
      if (text.length > 200) return { text };
    }
    return null;
  }

  /* 4. SongLyrics — p#songLyricsDiv */
  async function fromSongLyrics(t) {
    const a = slug(t.artist), s = slug(t.title);
    if (!a || !s) return null;
    const r = await politeFetch("https://www.songlyrics.com/" + a + "/" + s + "-lyrics/");
    if (!r.ok) return null;
    const doc = parseHtml(await r.text());
    const p = doc.querySelector("p#songLyricsDiv");
    if (!p) return null;
    const text = tidy(p.textContent);
    if (/we do not have the lyrics/i.test(text)) return null;
    return text.length > MIN_LEN ? { text } : null;
  }

  /* 5. Lyrics.com — serp search, then pre#lyric-body-text */
  async function fromLyricsCom(t) {
    const q = encodeURIComponent((t.artist || "") + " " + (t.title || ""));
    const sr = await politeFetch("https://www.lyrics.com/serp.php?st=" + q);
    if (!sr.ok) return null;
    const sdoc = parseHtml(await sr.text());
    const a = sdoc.querySelector('a[href^="/lyric/"]');
    if (!a) return null;
    const pr = await politeFetch("https://www.lyrics.com" + a.getAttribute("href"));
    if (!pr.ok) return null;
    const doc = parseHtml(await pr.text());
    const pre = doc.querySelector("pre#lyric-body-text");
    if (!pre) return null;
    const text = tidy(pre.textContent);
    return text.length > MIN_LEN ? { text } : null;
  }

  /* 6. Letras — div.lyric-original / div.cnt-letra */
  async function fromLetras(t) {
    const a = slug(t.artist), s = slug(t.title);
    if (!a || !s) return null;
    const r = await politeFetch("https://www.letras.mus.br/" + a + "/" + s + "/");
    if (!r.ok) return null;
    const doc = parseHtml(await r.text());
    const box = doc.querySelector("div.lyric-original") || doc.querySelector("div.cnt-letra");
    if (!box) return null;
    const parts = [];
    box.querySelectorAll("p").forEach((p) => parts.push(tidy(p.textContent)));
    const text = tidy(parts.length ? parts.join("\n\n") : box.textContent);
    return text.length > MIN_LEN ? { text } : null;
  }

  const PROVIDERS = [
    { id: "lrclib",     name: "LRCLIB",     fn: fromLrclib },
    { id: "genius",     name: "Genius",     fn: fromGenius },
    { id: "azlyrics",   name: "AZLyrics",   fn: fromAz },
    { id: "songlyrics", name: "SongLyrics", fn: fromSongLyrics },
    { id: "lyricscom",  name: "Lyrics.com", fn: fromLyricsCom },
    { id: "letras",     name: "Letras",     fn: fromLetras },
  ];

  async function runProviders(t) {
    for (const p of PROVIDERS) {
      if (stopFlag) return null;
      if (opts.providers[p.id] === false) continue;
      const st = stats[p.id] || (stats[p.id] = { ok: 0, tried: 0 });
      st.tried++;
      let res = null;
      try { res = await p.fn(t); } catch (_) {}
      if (res && res.text && res.text.length > MIN_LEN) {
        st.ok++;
        updateSrcChips();
        return { text: res.text, lrc: res.lrc || "", source: p.name };
      }
      updateSrcChips();
    }
    return null;
  }

  /* ============================================================
     BRIDGE round-trips with graceful no-reply degradation
     ============================================================ */
  const pend = {};             /* "type:id" -> {resolve, timer} */
  function bridgeRequest(obj, replyType, id, timeoutMs) {
    return new Promise((resolve) => {
      const key = replyType + ":" + id;
      const prev = pend[key];
      if (prev) { clearTimeout(prev.timer); prev.resolve(null); }
      pend[key] = { resolve, timer: setTimeout(() => { delete pend[key]; resolve(null); }, timeoutMs || 1500) };
      B.send(obj);
    });
  }
  function settle(replyType, m) {
    if (!m || m.id == null) return;
    const p = pend[replyType + ":" + m.id];
    if (p) { clearTimeout(p.timer); delete pend[replyType + ":" + m.id]; p.resolve(m); }
  }

  async function readLyrics(id) {
    if (readsDown || id == null) return null;
    const m = await bridgeRequest({ cmd: "lyricsread", id }, "lyrics", id, 1500);
    if (m == null) readsDown = true;   /* backend not answering — stop probing */
    return m;
  }

  function writeLyrics(id, rec, manual) {
    return bridgeRequest({ cmd: "lyricswrite", id, text: rec.text || "", synced_lrc: rec.lrc || "" },
                         "lyricswrote", id, 2500)
      .then((m) => {
        const ok = !!(m && m.ok);
        rec.wrote = ok;
        if (m == null && !writeNoted) { writeNoted = true; if (noteEl) noteEl.hidden = false; }
        if (manual) setPvStatus(ok ? "Saved ✓"
          : m ? "Write failed on the backend."
              : "Backend write support coming online — kept for this session.");
        updateRow(id);
        return ok;
      });
  }

  /* ============================================================
     BATCH FETCH — sequential queue drained by N promise workers
     ============================================================ */
  async function ensureAllTracks() {
    let guard = 0;
    while (!B.tracksDone() && !stopFlag && guard++ < 1000) {
      const before = B.getTracks().length;
      B.requestMoreTracks();
      let waited = 0;
      while (B.getTracks().length === before && waited < 3000 && !stopFlag) { await sleep(120); waited += 120; }
      if (B.getTracks().length === before) break;   /* bridge quiet — use what we have */
      if (viewVisible()) renderRowsIfGrown();
    }
  }

  async function processTrack(t) {
    const rec = store[t.id] || (store[t.id] = {});
    if (opts.skip && !rec.text && !readsDown) {
      const m = await readLyrics(t.id);
      if (m && m.text && String(m.text).trim().length > 10) {
        rec.text = String(m.text); rec.existing = true; rec.source = "file"; rec.status = "skip";
        if (looksLrc(rec.text)) rec.lrc = rec.text;
        updateRow(t.id);
        if (selectedId === t.id) renderPreview();
        return;
      }
    }
    if (rec.text && opts.skip) { rec.status = rec.status || "skip"; updateRow(t.id); return; }
    rec.status = "search"; updateRow(t.id);
    const found = await runProviders(t);
    if (found) {
      rec.text = found.text; rec.lrc = found.lrc || ""; rec.source = found.source; rec.status = "found";
      prog.found++;
      if (!rec.existing || opts.overwrite) writeLyrics(t.id, rec);
      if (selectedId === t.id) renderPreview();
    } else {
      rec.status = "fail";
    }
    updateRow(t.id);
  }

  async function fetchAll() {
    if (running) return;
    running = true; stopFlag = false;
    setRunUI(true);
    await ensureAllTracks();
    const queue = (B.getTracks() || []).filter((t) => {
      const s = store[t.id];
      return !(s && s.text && s.status !== "fail");
    });
    prog.done = 0; prog.total = queue.length; prog.found = 0;
    updateProgress();
    let idx = 0;
    const worker = async () => {
      while (!stopFlag) {
        const i = idx++;
        if (i >= queue.length) break;
        await processTrack(queue[i]);
        prog.done++;
        updateProgress();
      }
    };
    await Promise.all(Array.from({ length: clamp(opts.workers, 1, 8) }, worker));
    running = false;
    setRunUI(false);
    updateProgress();
  }

  function trackForNow(now) {
    const id = now.track_id;
    for (const r of (B.getTracks() || [])) if (r.id === id) return r;
    return {
      id: id != null ? id : -1,
      title: now.track_title || "", artist: now.track_artist || "",
      album: now.track_album || "", duration_ms: now.duration_ms || 0,
    };
  }

  /* fetch (read-cache -> backend file -> providers) for one track */
  async function getLyricsFor(t) {
    const have = store[t.id];
    if (have && have.text) return have;
    const m = await readLyrics(t.id);
    if (m && m.text && String(m.text).trim().length > 10) {
      const rec = store[t.id] = { text: String(m.text), source: "file", status: "skip", existing: true };
      if (looksLrc(rec.text)) rec.lrc = rec.text;
      updateRow(t.id);
      /* TIMESTAMP UPGRADE. Plenty of files ship EMBEDDED lyrics with no
         timestamps, and returning here meant those tracks could never
         follow playback — the now-playing panel would sit on a dead block
         of text forever even when LRCLIB has a synced copy. Try ONCE for a
         timed version, keeping the embedded text as the fallback so this
         can only ever improve on what the file already had. */
      if (rec.lrc || rec.syncTried) return rec;
      rec.syncTried = true;
      try {
        const timed = await runProviders(t);
        if (timed && timed.lrc) {
          rec.lrc = timed.lrc;
          rec.text = timed.text || rec.text;
          rec.source = timed.source;
          rec.status = "found";
          writeLyrics(t.id, rec);      /* persists the .lrc sidecar */
        }
      } catch (_) { /* keep the embedded copy */ }
      updateRow(t.id);
      return rec;
    }
    const rec = store[t.id] || (store[t.id] = {});
    rec.status = "search"; updateRow(t.id);
    const found = await runProviders(t);
    if (found) {
      rec.text = found.text; rec.lrc = found.lrc || ""; rec.source = found.source; rec.status = "found";
      writeLyrics(t.id, rec);
      updateRow(t.id);
      return rec;
    }
    rec.status = "fail"; updateRow(t.id);
    return null;
  }

  async function fetchCurrent() {
    const now = B.getNow();
    if (!now || !now.track_title) { flashProg("Nothing is playing."); return; }
    const t = trackForNow(now);
    selectedId = t.id;
    for (const k in rowEls) rowEls[k].classList.toggle("on", String(t.id) === k);
    renderPreview();
    await getLyricsFor(t);
    renderPreview();
  }

  /* ============================================================
     VIEW UI
     ============================================================ */
  let rootEl = null, rowsEl = null, noteEl = null, progEl = null;
  let btnCur = null, btnAll = null, btnStop = null;
  let pvTitle = null, pvPill = null, pvText = null, pvSave = null, pvStatus = null;
  const srcChipEls = {};

  function viewVisible() {
    const p = $("#view-lyrics");
    return !!(p && !p.hidden);
  }

  function mkSwitch(label, key) {
    const lab = el("label", "switch");
    const inp = document.createElement("input");
    inp.type = "checkbox"; inp.checked = !!opts[key];
    lab.appendChild(inp);
    lab.appendChild(el("span", "track"));
    lab.appendChild(el("span", "switch-label", label));
    inp.addEventListener("change", () => { opts[key] = inp.checked; saveOpts(); });
    return lab;
  }

  function buildUI() {
    rootEl = $("#lyrics-root");
    if (!rootEl || built) return;
    built = true;
    rootEl.innerHTML = "";

    /* toolbar row 1: actions + progress + workers */
    const tb = el("div", "lyr-toolbar");
    const r1 = el("div", "lyr-tb-row");
    btnCur = el("button", "btn btn-accent", "Fetch for current track");
    btnCur.addEventListener("click", fetchCurrent);
    btnAll = el("button", "btn btn-wide", "⭳  Fetch all missing");
    btnAll.addEventListener("click", fetchAll);
    btnStop = el("button", "btn btn-stop", "Stop");
    btnStop.disabled = true;
    btnStop.addEventListener("click", () => { stopFlag = true; flashProg("Stopping…"); });
    progEl = el("span", "lyr-prog", "");
    r1.appendChild(btnCur); r1.appendChild(btnAll); r1.appendChild(btnStop); r1.appendChild(progEl);
    r1.appendChild(el("span", "lyr-spacer"));
    const wl = el("label", "lyr-workers");
    wl.appendChild(el("span", "switch-label", "Workers"));
    const wi = document.createElement("input");
    wi.type = "number"; wi.min = "1"; wi.max = "8"; wi.value = String(opts.workers);
    wi.addEventListener("change", () => { opts.workers = clamp(+wi.value || 3, 1, 8); wi.value = String(opts.workers); saveOpts(); });
    wl.appendChild(wi);
    r1.appendChild(wl);
    tb.appendChild(r1);

    /* toolbar row 2: provider chips (with live success rate) + toggles */
    const r2 = el("div", "lyr-tb-row");
    PROVIDERS.forEach((p) => {
      const c = el("button", "chip lyr-src" + (opts.providers[p.id] === false ? "" : " on"));
      c.appendChild(el("span", null, p.name));
      const rateSpan = el("span", "lyr-src-rate", "");
      c.appendChild(rateSpan);
      c.title = "Toggle " + p.name + " in the provider chain";
      c.addEventListener("click", () => {
        const enable = opts.providers[p.id] === false;
        opts.providers[p.id] = enable;
        c.classList.toggle("on", enable);
        saveOpts();
      });
      srcChipEls[p.id] = rateSpan;
      r2.appendChild(c);
    });
    r2.appendChild(el("span", "lyr-spacer"));
    r2.appendChild(mkSwitch("Skip existing", "skip"));
    r2.appendChild(mkSwitch("Overwrite", "overwrite"));
    tb.appendChild(r2);
    rootEl.appendChild(tb);

    noteEl = el("div", "lyr-note",
      "Backend write support coming online — fetched lyrics are kept for this session but not written to files yet.");
    noteEl.hidden = true;
    rootEl.appendChild(noteEl);

    /* main split: table | preview */
    const main = el("div", "lyr-main");
    const tw = el("div", "lyr-tablewrap");
    const th = el("div", "lyr-thead");
    ["Track", "Artist", "Status", "Source"].forEach((h) => th.appendChild(el("div", "lyr-th", h)));
    tw.appendChild(th);
    rowsEl = el("div", "lyr-rows");
    rowsEl.addEventListener("click", (e) => {
      const rw = e.target.closest ? e.target.closest(".lyr-row") : null;
      if (rw) selectRow(+rw.dataset.id);
    });
    tw.appendChild(rowsEl);
    main.appendChild(tw);

    const pv = el("aside", "lyr-preview");
    const ph = el("div", "lyr-pv-head");
    pvTitle = el("div", "lyr-pv-title", "No track selected");
    pvPill = el("span", "pill synced", "SYNCED");
    pvPill.hidden = true;
    ph.appendChild(pvTitle); ph.appendChild(pvPill);
    pv.appendChild(ph);
    pvText = document.createElement("textarea");
    pvText.className = "lyr-pv-text";
    pvText.spellcheck = false;
    pvText.placeholder = "Select a track, then fetch or paste lyrics here.";
    pv.appendChild(pvText);
    const pf = el("div", "lyr-pv-foot");
    pvSave = el("button", "btn btn-wide lyr-save", "Save lyrics");
    pvSave.addEventListener("click", () => {
      if (selectedId == null) return;
      const rec = store[selectedId] || (store[selectedId] = {});
      rec.text = pvText.value;
      if (!rec.status || rec.status === "fail") rec.status = "found";
      if (!rec.source) rec.source = "manual";
      setPvStatus("Saving…");
      writeLyrics(selectedId, rec, true);
      updateRow(selectedId);
    });
    pvStatus = el("span", "lyr-pv-status", "");
    pf.appendChild(pvSave); pf.appendChild(pvStatus);
    pv.appendChild(pf);
    main.appendChild(pv);
    rootEl.appendChild(main);
  }

  function setPvStatus(s) { if (pvStatus) pvStatus.textContent = s; }

  function updateSrcChips() {
    PROVIDERS.forEach((p) => {
      const e = srcChipEls[p.id], st = stats[p.id];
      if (e) e.textContent = st && st.tried ? st.ok + "/" + st.tried : "";
    });
  }

  function updateProgress() {
    if (!progEl) return;
    progEl.textContent = running
      ? prog.done + " / " + prog.total + "  ·  " + prog.found + " found"
      : (prog.total ? "done — " + prog.found + " of " + prog.total + " fetched" : "");
  }
  function flashProg(msg) { if (progEl) progEl.textContent = msg; }

  function setRunUI(on) {
    if (btnStop) btnStop.disabled = !on;
    if (btnAll) btnAll.disabled = on;
    if (btnCur) btnCur.disabled = on;
  }

  /* ---------- table rows ---------- */
  function rowElFor(t) {
    const r = el("div", "lyr-row");
    r.dataset.id = t.id;
    if (t.id === selectedId) r.classList.add("on");
    r.appendChild(el("div", "lyr-t", t.title || "Unknown title"));
    r.appendChild(el("div", "lyr-a", t.artist || "—"));
    r.appendChild(el("div", "lyr-status", ""));
    r.appendChild(el("div", "lyr-source", ""));
    rowEls[t.id] = r;
    paintRow(t.id, r);
    return r;
  }

  function paintRow(id, r) {
    const rec = store[id];
    const st = $(".lyr-status", r), src = $(".lyr-source", r);
    if (!st || !src) return;
    const s = rec ? rec.status : null;
    st.className = "lyr-status " +
      (s === "found" ? "s-found" : s === "search" ? "s-search" : s === "fail" ? "s-fail" : s === "skip" ? "s-skip" : "s-idle");
    st.textContent =
      s === "found" ? (rec.wrote ? "✓ saved" : "✓ found") :
      s === "search" ? "searching…" :
      s === "fail" ? "no match" :
      s === "skip" ? "has lyrics" : "—";
    src.textContent = (rec && rec.source ? rec.source : "") + (rec && rec.lrc && s === "found" ? " · synced" : "");
  }
  function updateRow(id) { const r = rowEls[id]; if (r) paintRow(id, r); }

  function renderRows() {
    if (!rowsEl) return;
    const rows = B.getTracks() || [];
    rowsEl.innerHTML = "";
    Object.keys(rowEls).forEach((k) => delete rowEls[k]);
    const frag = document.createDocumentFragment();
    const n = Math.min(rows.length, MAX_RENDER);
    for (let i = 0; i < n; i++) frag.appendChild(rowElFor(rows[i]));
    rowsEl.appendChild(frag);
    if (rows.length > MAX_RENDER) {
      rowsEl.appendChild(el("div", "lyr-cap",
        "Showing the first " + MAX_RENDER.toLocaleString() + " of " + rows.length.toLocaleString() +
        " tracks — batch fetch still covers every track."));
    }
    if (!rows.length) {
      rowsEl.appendChild(el("div", "lyr-cap", "No tracks loaded yet — the table mirrors your library."));
    }
    renderedCount = rows.length;
  }
  function renderRowsIfGrown() {
    const n = (B.getTracks() || []).length;
    if (n !== renderedCount) renderRows();
  }

  async function selectRow(id) {
    selectedId = id;
    for (const k in rowEls) rowEls[k].classList.toggle("on", +k === id);
    renderPreview();
    const rec = store[id];
    if ((!rec || !rec.text) && !(rec && rec.status === "search")) {
      const m = await readLyrics(id);
      if (m && m.text && selectedId === id) {
        const r2 = store[id] || (store[id] = {});
        r2.text = String(m.text); r2.source = "file"; r2.existing = true;
        if (!r2.status) r2.status = "skip";
        if (looksLrc(r2.text)) r2.lrc = r2.text;
        updateRow(id);
        renderPreview();
      }
    }
  }

  function renderPreview() {
    if (!pvTitle) return;
    let t = null;
    for (const r of (B.getTracks() || [])) if (r.id === selectedId) { t = r; break; }
    const rec = selectedId != null ? store[selectedId] : null;
    pvTitle.textContent = t ? (t.title || "Unknown title") + " — " + (t.artist || "—")
                            : (selectedId != null ? "Track #" + selectedId : "No track selected");
    pvPill.hidden = !(rec && rec.lrc);
    pvText.value = rec && rec.text ? rec.text : "";
    pvText.placeholder = rec && rec.status === "search" ? "Searching…" : "Select a track, then fetch or paste lyrics here.";
    setPvStatus(rec && rec.source ? "Source: " + rec.source + (rec.wrote ? " · written" : "") : "");
  }

  /* ============================================================
     LIVE LYRICS — the permanent inline section of the now-playing
     panel (#np-lyrics-live in index.html), sitting directly BELOW
     the album art and the spectrum and ABOVE the online card and
     the queue. It populates itself on every track change; the old
     absolute .np-lyrics overlay it replaces is gone.

     IDLE-CPU CONTRACT (the app holds ~1% idle and must keep it):
     there is NO timer here. The karaoke follower rides app.js's
     playhead rAF loop through B.addTick / B.removeTick, and that
     loop already parks itself whenever nothing is playing. On top
     of that we only stay subscribed while there is genuinely
     something to advance — see npWantFollow(). The per-frame body
     (npTick) is one getPos() and two numeric compares until the
     ACTIVE LINE actually changes; the DOM is touched only then.
     ============================================================ */
  const NP_OPEN_KEY = "mn.lyrics.np.open";
  const NP_LEAD_MS  = 120;      /* fire a line slightly early — reads on-beat */
  const NP_MAX_LINES = 900;     /* node cap for pathological .lrc files       */
  const NP_FETCH_DELAY = 280;   /* debounce: skipping tracks must not spam
                                   the provider chain (LRCLIB → … → Letras)  */

  let npSec = null, npBtn = null, npPillEl = null, npBody = null;
  let npOpen = true;            /* persisted collapse/expand choice           */
  let npGen = 0;                /* fetch generation — kills stale renders     */
  let npKey = null;             /* identity of the track the section shows    */
  let npFetchTimer = 0;
  let npLines = null;           /* parsed [{t,text}] for synced lyrics, else null */
  let npLineEls = null;
  let npCur = -1;               /* active line index                          */
  let npLo = 0, npHi = 0;       /* [lo,hi) ms window the active line owns     */
  let npFollowing = false;      /* currently subscribed to the rAF loop       */

  function npReduced() {
    const mo = window.MN && MN.get && MN.get("motion");
    if (mo && mo.reduced) { try { return !!mo.reduced(); } catch (_) {} }
    return document.documentElement.classList.contains("no-anim") ||
           !!(window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches);
  }

  /* ---------- follower subscription (the whole idle story) ----------
     ONE geometric test covers every way the section can be out of sight, so
     no future CSS can leave the follower running over nothing:
       · display:none from the mini/micro tier rules
       · display:none from the @media (max-height:799px) short-window rule
       · the whole #nowplaying column sliding out under @media (max-width:1180px)
       · the user collapsing the panel (#app.np-collapsed -> zero-width track)
       · hidden="" for radio/podcast sessions
     It is a single getBoundingClientRect, and it runs only when something
     actually changed (see the triggers wired in buildNp) — never per frame. */
  function npOnScreen() {
    if (!npSec || npSec.hidden) return false;
    const r = npSec.getBoundingClientRect();
    return r.width > 8 && r.height > 0 &&
           r.right > 0 && r.left < (window.innerWidth || 0) &&
           r.bottom > 0 && r.top < (window.innerHeight || 0);
  }
  function npWantFollow() {
    return !!(npLines && npLines.length && npOpen &&
              typeof B.addTick === "function" && npOnScreen());
  }
  function npSyncFollow() {
    const want = npWantFollow();
    if (want === npFollowing) return;
    npFollowing = want;
    if (want) B.addTick(npTick); else if (B.removeTick) B.removeTick(npTick);
  }
  /* re-evaluate after something that can change visibility, and re-centre if
     the section just came back (its scroll box had no layout while hidden) */
  function npRevisit() {
    const was = npFollowing;
    npSyncFollow();
    if (npFollowing && !was && npLines) npPaint(npLocate(B.getPos().pos + NP_LEAD_MS), true);
  }

  /* last line whose timestamp is <= pos (binary search — a seek to the end
     of a 200-line .lrc costs 8 compares, not 200) */
  function npLocate(pos) {
    const L = npLines;
    if (!L || !L.length || pos < L[0].t) return -1;
    let lo = 0, hi = L.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (L[mid].t <= pos) lo = mid; else hi = mid - 1; }
    return lo;
  }

  /* THE per-frame body. Everything before the early-out is two numbers. */
  function npTick() {
    if (!npLines) return;
    const pos = B.getPos().pos + NP_LEAD_MS;
    if (pos >= npLo && pos < npHi) return;      /* still on the same line */
    npPaint(npLocate(pos), false);
  }

  function npPaint(idx, force) {
    const L = npLines;
    if (!L) return;
    npLo = idx >= 0 ? L[idx].t : -Infinity;
    /* skip over any lines sharing this timestamp so the window is never
       zero-width (that would re-search every single frame) */
    let j = idx + 1;
    while (j < L.length && L[j].t <= npLo) j++;
    npHi = j < L.length ? L[j].t : Infinity;
    if (idx === npCur && !force) return;
    /* a SEEK is anything that is not "the very next line": jump straight
       there instead of smooth-scrolling through everything in between */
    const jumped = !!force || npCur < 0 || idx < npCur || idx > npCur + 1;
    if (npCur >= 0 && npLineEls && npLineEls[npCur]) npLineEls[npCur].classList.remove("on");
    npCur = idx;
    if (idx < 0 || !npLineEls || !npLineEls[idx]) { npScrollTo(null, true); return; }
    npLineEls[idx].classList.add("on");
    npScrollTo(npLineEls[idx], jumped);
  }

  /* keep the active line centred INSIDE .npl-body only — computed directly
     rather than via scrollIntoView, which would also scroll ancestors */
  function npScrollTo(node, instant) {
    if (!npBody) return;
    if (!node) { npBody.scrollTop = 0; return; }
    const target = Math.max(0, node.offsetTop - (npBody.clientHeight - node.offsetHeight) / 2);
    if (instant || npReduced() || !npBody.scrollTo) npBody.scrollTop = target;
    else npBody.scrollTo({ top: target, behavior: "smooth" });
  }

  /* ---------- rendering ---------- */
  function npReset() {
    npLines = null; npLineEls = null; npCur = -1; npLo = 0; npHi = 0;
    if (npBody) npBody.textContent = "";
    if (npPillEl) npPillEl.hidden = true;
  }

  /* the "nothing to say" state: ONE dim line, no pill, no follower */
  function npQuiet(msg) {
    npReset();
    if (!npBody) return;
    npBody.classList.add("npl-hush");
    npBody.appendChild(el("div", "npl-quiet", msg));
    npSyncFollow();
  }

  function npRender(rec) {
    npReset();
    if (!npBody) return;
    if (!rec || !rec.text) { npQuiet("No lyrics for this track."); return; }
    npBody.classList.remove("npl-hush");
    const lrc = rec.lrc || (looksLrc(rec.text) ? rec.text : "");
    if (lrc) {
      const lines = parseLrc(lrc);
      if (lines.length) {
        npLines = lines.length > NP_MAX_LINES ? lines.slice(0, NP_MAX_LINES) : lines;
        const frag = document.createDocumentFragment();
        /* textContent only — provider text is remote data */
        npLineEls = npLines.map((L) => { const d = el("div", "lyr-line", L.text || "♪"); frag.appendChild(d); return d; });
        npBody.appendChild(frag);
        npPillEl.hidden = false;
        npBody.scrollTop = 0;
        npPaint(npLocate(B.getPos().pos + NP_LEAD_MS), true);
        npSyncFollow();
        return;
      }
    }
    /* plain lyrics: scrollable, no highlight, and the pill says so */
    npPillEl.hidden = false;
    npPillEl.classList.remove("synced");
    npPillEl.classList.add("plain");
    npPillEl.textContent = "UNSYNCED";
    npPillEl.title = "No timestamps for this track — the text does not follow playback";
    npBody.appendChild(el("div", "lyr-plain", stripLrc(rec.text)));
    npBody.scrollTop = 0;
    npSyncFollow();               /* -> unsubscribes: nothing to advance */
  }

  function npResetPill() {
    if (!npPillEl) return;
    npPillEl.classList.add("synced");
    npPillEl.classList.remove("plain");
    npPillEl.textContent = "SYNCED";
    npPillEl.title = "Timestamped lyrics — following playback live";
  }

  /* ---------- track changes ---------- */
  function npLoad(m) {
    const gen = ++npGen;
    if (npFetchTimer) { clearTimeout(npFetchTimer); npFetchTimer = 0; }
    npResetPill();

    /* internet radio / podcasts have no track to look up */
    if (!m || m.online || !m.track_title) {
      npReset();
      if (npBody) npBody.classList.add("npl-hush");
      if (npSec) npSec.hidden = true;
      npSyncFollow();
      return;
    }
    if (npSec) npSec.hidden = false;

    const t = trackForNow(m);
    const rec = store[t.id];
    if (rec && rec.text) { npRender(rec); return; }          /* already cached */
    if (rec && rec.status === "fail") { npQuiet("No lyrics for this track."); return; }

    npQuiet("Looking for lyrics…");
    npFetchTimer = setTimeout(() => {
      npFetchTimer = 0;
      if (gen !== npGen) return;
      getLyricsFor(t).then(
        (r) => { if (gen === npGen) npRender(r); },
        ()  => { if (gen === npGen) npQuiet("No lyrics for this track."); }
      );
    }, NP_FETCH_DELAY);
  }

  /* Identity of what is on screen. track_id alone is not enough: online
     sessions and the "nothing playing" state both carry no usable id. */
  function npIdentity(m) {
    if (!m || !m.track_title) return "none";
    if (m.online) return "online";
    return (m.track_id != null ? m.track_id : "?") + " " + m.track_title;
  }

  function npOnNow(m) {
    if (!npSec) return;
    const key = npIdentity(m);
    if (key !== npKey) { npKey = key; npLoad(m); return; }
    /* SAME track. While PAUSED the rAF loop is parked, so a seek made from
       the seekbar/keyboard would leave the highlight stale — resync off the
       existing now poll instead of keeping a timer alive. Costs the tick
       body (getPos + two compares) and only while a synced track is up. */
    if (npFollowing && m && !m.playing) npTick();
  }

  /* ---------- collapse / expand ---------- */
  function npSetOpen(open, persist) {
    npOpen = !!open;
    if (npSec) npSec.classList.toggle("collapsed", !npOpen);
    if (npBtn) {
      npBtn.setAttribute("aria-expanded", npOpen ? "true" : "false");
      npBtn.classList.toggle("on", npOpen);
    }
    if (persist) { try { localStorage.setItem(NP_OPEN_KEY, npOpen ? "1" : "0"); } catch (_) {} }
    npSyncFollow();
    /* re-centre on expand — the body had no layout while collapsed */
    if (npOpen && npLines) npPaint(npLocate(B.getPos().pos + NP_LEAD_MS), true);
  }

  function buildNp() {
    npSec    = $("#np-lyrics-live");
    npBtn    = $("#npl-toggle");
    npPillEl = $("#npl-pill");
    npBody   = $("#npl-body");
    if (!npSec || !npBody) return;

    try { npOpen = localStorage.getItem(NP_OPEN_KEY) !== "0"; } catch (_) { npOpen = true; }
    npSetOpen(npOpen, false);                  /* default EXPANDED */
    if (npBtn) npBtn.addEventListener("click", () => npSetOpen(!npOpen, true));

    /* VISIBILITY TRIGGERS. Each fires only on a real change — no polling,
       no timers, nothing at idle — and each just re-runs npOnScreen():
         · data-ui-tier on <html>   — the mini/micro widget tiers
         · class on #app            — the user collapsing the panel
         · one matchMedia listener  — the two breakpoints that hide it */
    if (window.MutationObserver) {
      const mo = new MutationObserver(npRevisit);
      mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-ui-tier"] });
      const app = $("#app");
      if (app) mo.observe(app, { attributes: true, attributeFilter: ["class"] });
    }
    if (window.matchMedia) {
      const mq = matchMedia("(max-height:799px),(max-width:1180px)");
      if (mq.addEventListener) mq.addEventListener("change", npRevisit);
      else if (mq.addListener) mq.addListener(npRevisit);
    }

    /* rAF is frozen while the document is hidden, so the follower simply
       does not run; re-centre once when we come back. */
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden && npLines && npOpen) {
        npPaint(npLocate(B.getPos().pos + NP_LEAD_MS), true);
      }
    });

    B.tap("now", npOnNow);
    npOnNow(B.getNow());
  }

  /* ============================================================
     PUBLIC — init(api) at boot, open(api) on nav click
     ============================================================ */
  function init(api) {
    B = api;
    if (inited) return;
    inited = true;
    B.on("lyrics", (m) => { readsDown = false; settle("lyrics", m); });
    B.on("lyricswrote", (m) => settle("lyricswrote", m));
    B.tap("tracks", () => { if (viewVisible()) setTimeout(renderRowsIfGrown, 40); });
    buildNp();
  }

  function open(api) {
    B = api;
    if (!inited) init(api);
    buildUI();
    renderRows();
    updateSrcChips();
    updateProgress();
    renderPreview();
  }

  /* Player-bar LYR pill: the live section is always present now, so the pill
     collapses/expands it (and scrolls it into view when it re-opens). */
  function toggleNp() {
    if (!npSec) return;
    npSetOpen(!npOpen, true);
    if (npOpen && npSec.scrollIntoView && !npSec.hidden) {
      npSec.scrollIntoView({ block: "nearest", behavior: npReduced() ? "auto" : "smooth" });
    }
  }

  return { init, open, toggleNp };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("lyrics", "1.2.0", [], function () { return window.MnLyrics || {}; });
