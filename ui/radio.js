/* ============================================================
   MONATOMIC — Internet Radio view
   Global station directory via radio-browser.info (free, no
   key). Desktop equivalent of the Android RadioPane: preset
   chip row + countries browser + worldwide name search, rows
   grouped by country, star-to-Streams saving, and a radio-
   exclusive queue so transport next/prev surf the current
   query result instead of the parked library queue.
   app.js opens this view via MnRadio.open(modApi).

   Bridge surface used:
     send({cmd:'streamplay', url, title, artist, kind:'radio'})
     tap('now')       -> highlight the live row (online_url)
     tap('streamres') -> inline per-row playback errors
   Directory calls use plain fetch() (radio-browser serves
   CORS); hosts are tried in a fixed failover order with an
   8 s abort per host and a 10-minute in-memory cache.

   Desktop limitation honored here: HLS (.m3u8) station urls
   are FILTERED OUT at parse (no HLS demux yet). AAC stations
   stay listed — a play failure surfaces the streamres error
   inline under the row.
   ============================================================ */
window.MnRadio = (function () {
  "use strict";

  /* ---------- helpers ---------- */
  const el = (tag, cls, txt) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt != null) e.textContent = txt;
    return e;
  };

  /* ---------- directory client ---------- */
  const HOSTS = [
    "https://de1.api.radio-browser.info",
    "https://nl1.api.radio-browser.info",
    "https://all.api.radio-browser.info",
  ];
  const FETCH_TIMEOUT_MS = 8000;
  const TTL_MS = 10 * 60 * 1000;              /* 10-minute cache      */

  const cache = new Map();                    /* key -> {ts, data}    */
  let countriesCache = null;                  /* {ts, data} — own slot */

  function cacheGet(key) {
    const c = cache.get(key);
    return c && Date.now() - c.ts < TTL_MS ? c.data : null;
  }
  function cachePut(key, data) { cache.set(key, { ts: Date.now(), data }); }

  /* GET {host}{path} with per-host timeout + de1 -> nl1 -> all failover. */
  async function apiGet(path) {
    let lastErr = null;
    for (const host of HOSTS) {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
      try {
        const r = await fetch(host + path, {
          signal: ctl.signal,
          headers: { "Accept": "application/json" },
        });
        clearTimeout(timer);
        if (!r.ok) throw new Error("HTTP " + r.status + " from " + host);
        return await r.json();
      } catch (e) {
        clearTimeout(timer);
        lastErr = e;                          /* try the next mirror   */
      }
    }
    throw (lastErr || new Error("No radio-browser hosts reachable"));
  }

  /* Raw station list -> clean rows. Drops blank url_resolved and HLS
     (.m3u8) entries — the desktop engine can't demux HLS yet — and
     de-dupes by stationuuid (fallback: the url itself). */
  function parseStations(raw) {
    const out = [];
    const seen = new Set();
    (Array.isArray(raw) ? raw : []).forEach((s) => {
      const url = String(s.url_resolved || "").trim();
      if (!url) return;                                        /* unplayable  */
      if (/\.m3u8([?#]|$)/i.test(url)) return;                 /* HLS: no demux yet */
      if (s.hls === 1 || s.hls === "1") return;                /* flagged HLS */
      const uuid = String(s.stationuuid || "").trim() || url;
      const key = uuid.toLowerCase();
      if (seen.has(key)) return;
      seen.add(key);
      out.push({
        uuid,
        url,
        name: String(s.name || "").trim() || "Unknown station",
        favicon: String(s.favicon || "").trim(),
        tags: String(s.tags || ""),
        country: String(s.country || "").trim(),
        codec: String(s.codec || "").trim(),
        bitrate: parseInt(s.bitrate, 10) || 0,
        votes: parseInt(s.votes, 10) || 0,
      });
    });
    return out;
  }

  function parseCountries(raw) {
    const out = [];
    const seen = new Set();
    (Array.isArray(raw) ? raw : []).forEach((c) => {
      const name = String(c.name || "").trim();
      const count = parseInt(c.stationcount, 10) || 0;
      if (!name || count <= 0) return;
      const key = name.toLowerCase();
      if (seen.has(key)) return;
      seen.add(key);
      out.push({ name, code: String(c.iso_3166_1 || "").trim(), count });
    });
    out.sort((a, b) => b.count - a.count);
    return out;
  }

  async function fetchStations(key, path) {
    const hit = cacheGet(key);
    if (hit) return hit;
    const list = parseStations(await apiGet(path));
    cachePut(key, list);
    return list;
  }
  async function fetchCountriesList() {
    if (countriesCache && Date.now() - countriesCache.ts < TTL_MS) return countriesCache.data;
    const list = parseCountries(await apiGet("/json/countries"));
    countriesCache = { ts: Date.now(), data: list };
    return list;
  }
  function stationsByCountry(c) {
    if (c.code) {
      return fetchStations("cc:" + c.code.toLowerCase(),
        "/json/stations/bycountrycodeexact/" + encodeURIComponent(c.code) +
        "?order=votes&reverse=true&limit=200");
    }
    return fetchStations("country:" + c.name.toLowerCase(),
      "/json/stations/bycountryexact/" + encodeURIComponent(c.name) +
      "?order=votes&reverse=true&limit=200");
  }

  /* ---------- presets ---------- */
  /* Exact chip order; every chip after the first two IS the tag. */
  const CHIPS = [
    "Popular", "Countries",
    "pop", "rock", "jazz", "classical", "electronic", "hip hop",
    "country", "metal", "ambient", "news", "talk", "dance",
    "oldies", "blues", "reggae", "latin",
  ];

  /* ---------- inline SVG (static strings only — never remote data) ---------- */
  const SVG_SEARCH =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg>';
  const SVG_CHEV =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 9l6 6 6-6"/></svg>';
  const SVG_STAR_OUTLINE =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M12 3l2.7 5.6 6.1.8-4.5 4.3 1.1 6-5.4-2.9-5.4 2.9 1.1-6L3.2 9.4l6.1-.8z"/></svg>';
  const SVG_STAR_FILL =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path d="M12 3l2.7 5.6 6.1.8-4.5 4.3 1.1 6-5.4-2.9-5.4 2.9 1.1-6L3.2 9.4l6.1-.8z"/></svg>';
  const SVG_RADIO =
    '<svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="9" width="18" height="11" rx="2"/><path d="M7 9L17.5 4.5"/><circle cx="8.5" cy="14.5" r="2.5"/><path d="M15 13h3M15 16.5h3"/></svg>';

  /* ---------- favicon fallback: hash-tinted lettered circle ---------- */
  const PALETTE = [
    "#FB8C00", "#00E5FF", "#8B5CF6", "#1DB954",
    "#E05656", "#FFD166", "#4D9FFF", "#FF6FB5",
  ];
  function nameHash(s) {
    let h = 0;
    for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
    return h;
  }
  function letterEl(st) {
    const col = PALETTE[Math.abs(nameHash(st.name)) % PALETTE.length];
    const first = (st.name.trim().charAt(0) || "?").toUpperCase();
    const d = el("div", "rad-letter", first);
    d.style.background = col + "2E";          /* 18% alpha tint        */
    d.style.color = col;
    return d;
  }
  /* In-session negative cache so dead favicon urls are not re-tried on
     every re-render / scroll. */
  const deadFavs = new Set();

  /* ---------- state ---------- */
  let bridge = null;                /* modApi from app.js                     */
  let inited = false;
  let panel = null;                 /* #view-radio                            */
  let chipRowEl = null, searchInput = null, searchBtn = null, contentEl = null;

  let chip = "Popular";             /* selected preset chip                   */
  let activeSearch = null;          /* non-null = search overrides the chip   */
  let loading = false;
  let loadErr = "";
  let gen = 0;                      /* fetch generation — drops stale replies */

  let stations = [];                /* current grouped-mode query result      */
  let expanded = new Set();         /* open country sections (grouped modes)  */

  let countries = [];               /* Countries mode directory               */
  const countryOpen = new Set();    /* open rows (key = name lowercased)      */
  const countryStations = new Map();/* key -> station list (memoized once)    */
  const countryLoading = new Set(); /* keys with an in-flight fetch           */

  /* Radio-exclusive queue: the whole current query result, in list order
     across groups (Countries mode: that single country's list). */
  let queue = [];
  let lastPlayedUrl = "";           /* lowercased url of our last streamplay  */
  let pendingPlayUrl = "";          /* optimistic highlight until 'now' lands */
  let nowUrl = "";                  /* live radio session url (from 'now')    */
  const inlineErr = {};             /* url (lower) -> playback error string   */

  /* ---------- saved-to-Streams state (MnStreams owns persistence) ---------- */
  function savedUrlSet() {
    try {
      if (window.MnStreams && typeof MnStreams.urls === "function") {
        return new Set((MnStreams.urls() || []).map((u) => String(u).toLowerCase()));
      }
    } catch (_) {}
    return new Set();
  }

  /* ---------- derived ---------- */
  const inCountriesMode = () => !activeSearch && chip === "Countries";

  /* Subtitle: "country · CODEC nnn kbps", blank/zero parts skipped,
     fallback "Internet radio". Doubles as the stream 'artist' string. */
  function subtitleOf(st) {
    const parts = [];
    if (st.country) parts.push(st.country);
    const tech = [];
    if (st.codec) tech.push(st.codec.toUpperCase());
    if (st.bitrate > 0) tech.push(st.bitrate + " kbps");
    if (tech.length) parts.push(tech.join(" "));
    return parts.length ? parts.join(" · ") : "Internet radio";
  }

  function groupByCountry(list) {
    const m = new Map();
    list.forEach((st) => {
      const c = st.country || "Unknown";
      let arr = m.get(c);
      if (!arr) { arr = []; m.set(c, arr); }
      arr.push(st);
    });
    const groups = [];
    m.forEach((sts, name) => groups.push({ name, sts }));
    groups.sort((a, b) => b.sts.length - a.sts.length);
    return groups;
  }

  /* ============================================================
     PLAYBACK — radio-exclusive queue + transport surfing
     ============================================================ */
  function playStation(st, queueList) {
    queue = queueList.slice();
    const u = st.url.toLowerCase();
    lastPlayedUrl = u;
    pendingPlayUrl = u;
    if (inlineErr[u]) { delete inlineErr[u]; updateRowError(u); }
    bridge.send({
      cmd: "streamplay",
      url: st.url,
      title: st.name,
      artist: subtitleOf(st),
      kind: "radio",
    });
    updateLiveRows();
  }

  /* next()/prev(): cycle the module queue with wrap-around. app.js routes
     the transport buttons here while a radio session is online. */
  function step(dir) {
    if (!queue.length) return false;
    const cur = nowUrl || pendingPlayUrl || lastPlayedUrl;
    let i = queue.findIndex((s) => s.url.toLowerCase() === cur);
    i = i < 0 ? 0 : (i + dir + queue.length) % queue.length;
    playStation(queue[i], queue);
    return true;
  }

  /* True when the current online session is a station from this module. */
  function isActive() {
    const u = nowUrl || pendingPlayUrl;
    if (!u) return false;
    if (u === lastPlayedUrl) return true;
    return queue.some((s) => s.url.toLowerCase() === u);
  }

  /* ---------- bridge taps (registered once; multi-consumer safe) ---------- */
  /* 'now' is high-frequency and multi-consumer: filter by field (lite now
     messages without online info are ignored) and only touch classes —
     never re-render — so this stays idle-cheap. */
  function onNow(m) {
    if (!m || m.online === undefined) return;
    const u = (m.online && m.online_kind === "radio" && m.online_url)
      ? String(m.online_url).toLowerCase() : "";
    const before = nowUrl || pendingPlayUrl;
    if (u) pendingPlayUrl = "";     /* engine confirmed an online session */
    nowUrl = u;
    if ((nowUrl || pendingPlayUrl) !== before) updateLiveRows();
  }

  /* streamres is shared with every stream-playing module: only the reply
     for the url WE last played is ours. Failures show inline (this app
     uses inline error text, not toasts). */
  function onStreamRes(m) {
    if (!m || !m.url) return;
    const u = String(m.url).toLowerCase();
    if (u !== lastPlayedUrl) return;
    if (m.ok) {
      if (inlineErr[u]) { delete inlineErr[u]; updateRowError(u); }
      return;
    }
    inlineErr[u] = String(m.error || "playback failed");
    if (pendingPlayUrl === u) pendingPlayUrl = "";
    updateRowError(u);
    updateLiveRows();
  }

  function updateLiveRows() {
    if (!contentEl) return;
    const act = nowUrl || pendingPlayUrl;
    contentEl.querySelectorAll(".rad-row").forEach((r) => {
      r.classList.toggle("rad-live", !!act && r.dataset.url === act);
    });
  }
  function updateRowError(u) {
    if (!contentEl) return;
    contentEl.querySelectorAll(".rad-row").forEach((r) => {
      if (r.dataset.url !== u) return;
      const e = r.querySelector(".rad-rowerr");
      if (!e) return;
      const msg = inlineErr[u] || "";
      e.textContent = msg;
      e.hidden = !msg;
    });
  }

  /* ============================================================
     DATA LOADING
     ============================================================ */
  async function load() {
    const g = ++gen;
    loading = true;
    loadErr = "";
    renderContent();
    try {
      if (inCountriesMode()) {
        const list = await fetchCountriesList();
        if (g !== gen) return;
        countries = list;
      } else {
        let list;
        if (activeSearch) {
          list = await fetchStations(
            "search:" + activeSearch.toLowerCase(),
            "/json/stations/search?name=" + encodeURIComponent(activeSearch) +
            "&order=votes&reverse=true&limit=100");
        } else if (chip === "Popular") {
          list = await fetchStations("top", "/json/stations/topvote/100");
        } else {
          /* the chip label IS the tag (lowercase, spaces included) */
          list = await fetchStations(
            "tag:" + chip.toLowerCase(),
            "/json/stations/bytagexact/" + encodeURIComponent(chip) +
            "?order=votes&reverse=true&limit=100");
        }
        if (g !== gen) return;
        stations = list;
        /* the 3 largest country sections start expanded */
        expanded = new Set(groupByCountry(stations).slice(0, 3).map((x) => x.name));
      }
    } catch (_) {
      if (g !== gen) return;
      stations = [];
      countries = [];
      loadErr = "Couldn't reach the radio directory";
    }
    if (g !== gen) return;
    loading = false;
    renderContent();
  }

  function toggleCountry(c) {
    const key = c.name.toLowerCase();
    if (countryOpen.has(key)) {
      countryOpen.delete(key);
    } else {
      countryOpen.add(key);
      if (!countryStations.has(key) && !countryLoading.has(key)) fetchCountry(c);
    }
    renderContent();
  }
  async function fetchCountry(c) {
    const key = c.name.toLowerCase();
    countryLoading.add(key);
    let list = [];
    try { list = await stationsByCountry(c); }
    catch (_) { list = []; }         /* failure shows "No playable stations" */
    countryLoading.delete(key);
    countryStations.set(key, list);  /* memoized: fetched once per country   */
    if (inCountriesMode()) renderContent();
  }

  /* ============================================================
     SHELL (built once): chips, caption, search, content scroller
     ============================================================ */
  function buildShell() {
    panel.innerHTML = "";
    const rootEl = el("div", "rad-root");

    /* (1) preset chip row */
    chipRowEl = el("div", "rad-chips");
    CHIPS.forEach((label) => {
      const c = el("button", "rad-chip", label);
      c.type = "button";
      c.dataset.chip = label;
      c.addEventListener("click", () => {
        activeSearch = null;         /* a chip always cancels the search */
        chip = label;
        syncChips();
        syncSearchTint();
        load();
      });
      chipRowEl.appendChild(c);
    });
    rootEl.appendChild(chipRowEl);

    /* (2) attribution caption */
    rootEl.appendChild(el("div", "rad-caption", "Global directory · radio-browser.info"));

    /* (3) search row — submits ONLY on Enter or the icon */
    const sr = el("div", "rad-search");
    searchInput = document.createElement("input");
    searchInput.type = "text";
    searchInput.placeholder = "Search stations worldwide…";
    searchInput.spellcheck = false;
    searchInput.setAttribute("aria-label", "Search stations worldwide");
    searchInput.addEventListener("keydown", (e) => {
      if (e.key === "Enter") submitSearch();
    });
    searchInput.addEventListener("input", syncSearchTint);
    sr.appendChild(searchInput);
    searchBtn = el("button", "rad-search-btn");
    searchBtn.type = "button";
    searchBtn.title = "Search";
    searchBtn.innerHTML = SVG_SEARCH;
    searchBtn.addEventListener("click", submitSearch);
    sr.appendChild(searchBtn);
    rootEl.appendChild(sr);

    /* (4) content */
    contentEl = el("div", "rad-content");
    rootEl.appendChild(contentEl);

    panel.appendChild(rootEl);
    syncChips();
  }

  function submitSearch() {
    const q = (searchInput.value || "").trim();
    if (!q) return;
    activeSearch = q;               /* overrides the chip until one is tapped */
    syncChips();
    searchInput.blur();
    load();
  }

  function syncChips() {
    if (!chipRowEl) return;
    Array.from(chipRowEl.children).forEach((c) => {
      c.classList.toggle("rad-sel", !activeSearch && c.dataset.chip === chip);
    });
  }
  function syncSearchTint() {
    if (searchBtn) searchBtn.classList.toggle("rad-hot", !!(searchInput && searchInput.value.trim()));
  }

  /* ============================================================
     CONTENT RENDER
     ============================================================ */
  function centerState(nodes) {
    const c = el("div", "rad-center");
    nodes.forEach((n) => c.appendChild(n));
    contentEl.appendChild(c);
  }

  function sectionHeader(name, count, open, onToggle) {
    const h = el("div", "rad-cshead" + (open ? " rad-openh" : ""));
    h.appendChild(el("div", "rad-csname", name));
    h.appendChild(el("div", "rad-cscount", String(count)));
    const ch = el("div", "rad-chev");
    ch.innerHTML = SVG_CHEV;
    h.appendChild(ch);
    h.title = open ? "Collapse" : "Expand";
    h.addEventListener("click", onToggle);
    return h;
  }

  function stationRow(st, queueList, savedSet) {
    const row = el("div", "rad-row");
    const u = st.url.toLowerCase();
    row.dataset.url = u;
    if (u === (nowUrl || pendingPlayUrl)) row.classList.add("rad-live");
    row.addEventListener("click", () => playStation(st, queueList));

    /* 42px favicon with lettered-circle fallback (negative-cached) */
    const ico = el("div", "rad-ico");
    const fav = st.favicon;
    if (/^https?:\/\//i.test(fav) && !deadFavs.has(fav)) {
      const img = document.createElement("img");
      img.className = "rad-fav";
      img.alt = "";
      img.loading = "lazy";
      img.decoding = "async";
      img.addEventListener("error", () => {
        deadFavs.add(fav);
        ico.replaceChildren(letterEl(st));
      });
      img.src = fav;
      ico.appendChild(img);
    } else {
      ico.appendChild(letterEl(st));
    }
    row.appendChild(ico);

    /* name / subtitle / inline playback error */
    const mid = el("div", "rad-mid");
    mid.appendChild(el("div", "rad-name", st.name));
    mid.appendChild(el("div", "rad-sub", subtitleOf(st)));
    const errEl = el("div", "rad-rowerr", inlineErr[u] || "");
    errEl.hidden = !inlineErr[u];
    mid.appendChild(errEl);
    row.appendChild(mid);

    /* star: save to Streams (MnStreams owns the saved list) */
    const saved = savedSet.has(u);
    const star = el("button", "rad-star" + (saved ? " rad-saved" : ""));
    star.type = "button";
    star.innerHTML = saved ? SVG_STAR_FILL : SVG_STAR_OUTLINE;
    star.title = saved ? "Saved" : "Save to Streams";
    star.disabled = saved;
    star.addEventListener("click", (e) => {
      e.stopPropagation();
      if (star.disabled) return;
      try {
        if (window.MnStreams && typeof MnStreams.add === "function") {
          MnStreams.add(st.name, st.url);
        }
      } catch (_) {}
      star.innerHTML = SVG_STAR_FILL;
      star.classList.add("rad-saved");
      star.title = "Saved";
      star.disabled = true;
    });
    row.appendChild(star);
    return row;
  }

  /* Grouped list (Popular / tag / search): sections by country, largest
     first; queue = the ENTIRE result flattened in list order across groups. */
  function renderGrouped() {
    const groups = groupByCountry(stations);
    const flat = [];
    groups.forEach((g) => { g.sts.forEach((s) => flat.push(s)); });
    const saved = savedUrlSet();
    const frag = document.createDocumentFragment();
    groups.forEach((g) => {
      const open = expanded.has(g.name);
      frag.appendChild(sectionHeader(g.name, g.sts.length, open, () => {
        if (expanded.has(g.name)) expanded.delete(g.name);
        else expanded.add(g.name);
        renderContent();
      }));
      if (open) g.sts.forEach((st) => frag.appendChild(stationRow(st, flat, saved)));
    });
    contentEl.appendChild(frag);
  }

  /* Countries browser: summary line + collapsible per-country rows with
     lazy top-200 fetch; queue = that single country's list. */
  function renderCountries() {
    if (!countries.length) {
      centerState([el("div", "rad-empty", "No stations")]);
      return;
    }
    const total = countries.reduce((a, c) => a + c.count, 0);
    contentEl.appendChild(el("div", "rad-summary",
      countries.length + " countries · " + total + " stations"));
    const saved = savedUrlSet();
    const frag = document.createDocumentFragment();
    countries.forEach((c) => {
      const key = c.name.toLowerCase();
      const open = countryOpen.has(key);
      frag.appendChild(sectionHeader(c.name, c.count, open, () => toggleCountry(c)));
      if (!open) return;
      if (countryLoading.has(key)) {
        const w = el("div", "rad-cload");
        w.appendChild(el("div", "rad-spin rad-spin-sm"));
        frag.appendChild(w);
      } else {
        const list = countryStations.get(key) || [];
        if (!list.length) frag.appendChild(el("div", "rad-none", "No playable stations"));
        else list.forEach((st) => frag.appendChild(stationRow(st, list, saved)));
      }
    });
    contentEl.appendChild(frag);
  }

  function renderContent() {
    if (!contentEl) return;
    contentEl.innerHTML = "";

    if (loading) {
      centerState([el("div", "rad-spin")]);
      return;
    }
    if (loadErr) {
      const ico = el("div", "rad-errico");
      ico.innerHTML = SVG_RADIO;
      const retry = el("button", "rad-retry", "Retry");
      retry.type = "button";
      retry.addEventListener("click", () => load());
      centerState([ico, el("div", "rad-errtext", loadErr), retry]);
      return;
    }
    if (inCountriesMode()) { renderCountries(); return; }
    if (!stations.length) {
      centerState([el("div", "rad-empty", "No stations")]);
      return;
    }
    renderGrouped();
  }

  /* ============================================================
     OPEN (called by app.js each time the nav entry is clicked)
     ============================================================ */
  function open(b) {
    bridge = b;
    panel = document.getElementById("view-radio");
    if (!panel) return;
    if (!inited) {
      inited = true;
      buildShell();
      /* tap(), never on(): 'now' and 'streamres' are multi-consumer. */
      bridge.tap("now", onNow);
      bridge.tap("streamres", onStreamRes);
      load();
    } else {
      /* re-render so saved-star state (Streams changes) and the live
         highlight are fresh; data itself rides the 10-min cache */
      renderContent();
    }
  }

  return {
    open,
    next: () => step(1),
    prev: () => step(-1),
    isActive,
  };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("radio", "1.0.0", [], function () { return window.MnRadio || {}; });
