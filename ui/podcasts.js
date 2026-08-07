/* ============================================================
   MONATOMIC — Podcasts view
   Full podcast system: subscriptions (RSS via the C httpfetch
   bridge), an iTunes-directory Discover mode, per-episode
   downloads with progress (poddownload/poddl), storage
   management (podusage/podwipe/poddelete) and playback through
   the streaming engine (streamplay kind:'podcast').
   Desktop equivalent of the Android PodcastsPane — exact
   structure, labels and copy, Monatomic visual language.
   app.js opens this view via MnPodcasts.open(modApi).
   ============================================================ */
window.MnPodcasts = (function () {
  let lastPlayMeta = null;   /* right-panel Episode Notes (app.js nowMeta) */
  "use strict";

  /* ---------- helpers ---------- */
  const $  = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const el = (tag, cls, txt) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt != null) e.textContent = txt;
    return e;
  };

  /* Static SVG icons (never carry remote data — innerHTML is safe here). */
  const SVG = {
    refresh: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/></svg>',
    download: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v10"/><path d="M8 10l4 4 4-4"/><path d="M5 19h14"/></svg>',
    check: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M8.5 12.4l2.5 2.6 4.7-5.4"/></svg>',
    close: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M6 6l12 12"/><path d="M18 6L6 18"/></svg>',
    pod: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="2.4"/><path d="M8.2 15.8a5.4 5.4 0 0 1 0-7.6"/><path d="M15.8 8.2a5.4 5.4 0 0 1 0 7.6"/><path d="M5.6 18.4a9 9 0 0 1 0-12.8"/><path d="M18.4 5.6a9 9 0 0 1 0 12.8"/></svg>',
  };
  function icon(name, cls) {
    const s = el("span", "pod-ico" + (cls ? " " + cls : ""));
    s.innerHTML = SVG[name] || "";
    return s;
  }
  function spinner(px) {
    const s = el("span", "pod-spin");
    s.style.width = s.style.height = (px || 16) + "px";
    return s;
  }

  /* ---------- byte / date / duration formatting (EXACT Android rules) ---------- */
  /* decimal units: <1000 "N B"; <1e6 round kB; <1e8 "%.1f MB"; <1e9 round MB; else "%.2f GB" */
  function fmtBytes(b) {
    b = Math.max(0, Math.floor(+b || 0));
    if (b < 1000) return b + " B";
    if (b < 1000000) return Math.round(b / 1000) + " kB";
    if (b < 100000000) return (b / 1e6).toFixed(1) + " MB";
    if (b < 1000000000) return Math.round(b / 1e6) + " MB";
    return (b / 1e9).toFixed(2) + " GB";
  }
  const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  function fmtDate(ms) {
    const d = new Date(ms);
    if (isNaN(d)) return "";
    return d.getDate() + " " + MONTHS[d.getMonth()] + " " + d.getFullYear();
  }
  /* "<h>h <m>m" when >= 1h, else "<m> min" */
  function fmtDur(sec) {
    sec = Math.max(0, Math.floor(+sec || 0));
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    if (h > 0) return h + "h " + m + "m";
    return m + " min";
  }

  /* 32-bit Java-style string hash (for filenames + tint picking) */
  function strHash(s) {
    s = String(s || "");
    let h = 0;
    for (let i = 0; i < s.length; i++) h = (Math.imul(31, h) + s.charCodeAt(i)) | 0;
    return h;
  }
  function firstLetter(name) {
    const t = String(name || "").trim();
    return t ? t.charAt(0).toUpperCase() : "?";
  }
  const TINTS = ["#e57373", "#f0a05a", "#ffd166", "#81c784", "#4dd0e1", "#64b5f6", "#b39ddb", "#f06292"];

  /* ---------- download filename (EXACT rule) ----------
     guid filtered to [A-Za-z0-9_-] take 60, + "_" + hex of the 32-bit hash of
     guid ("ep_<hex>" when the filtered part is empty), ext from the enclosure
     URL path (strip query/fragment, 2-4 alnum chars, lowercased, else mp3). */
  function extFrom(url) {
    let p = String(url || "");
    const q = p.indexOf("?"); if (q >= 0) p = p.slice(0, q);
    const f = p.indexOf("#"); if (f >= 0) p = p.slice(0, f);
    const dot = p.lastIndexOf(".");
    let ext = dot >= 0 ? p.slice(dot + 1).toLowerCase() : "";
    if (!(ext.length >= 2 && ext.length <= 4 && /^[a-z0-9]+$/.test(ext))) ext = "mp3";
    return ext;
  }
  function fnameFor(ep) {
    const guid = String(ep.guid || "");
    const base = guid.replace(/[^A-Za-z0-9_-]/g, "").slice(0, 60);
    const hex = (strHash(guid) >>> 0).toString(16);
    const name = base ? (base + "_" + hex) : ("ep_" + hex);
    return name + "." + extFrom(ep.audioUrl);
  }

  /* ---------- HTML strip (EXACT: tags -> space, 6 entities, collapse, cap 500) ---------- */
  function decodeEntities(s) {
    return String(s || "")
      .replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
      .replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&nbsp;/g, " ");
  }
  function stripHtml(s) {
    s = String(s || "").replace(/<[^>]*>/g, " ");
    s = decodeEntities(s);
    s = s.replace(/\s+/g, " ").trim();
    return s.length > 500 ? s.slice(0, 500) : s;
  }

  /* ---------- RFC-822 pubDate (Date parsing fallbacks) ---------- */
  function parseRfc822(s) {
    s = String(s || "").trim();
    if (!s) return 0;
    let t = Date.parse(s);
    if (!isNaN(t)) return t;
    /* "dd MMM yyyy HH:mm:ss Z" (no day-of-week) */
    t = Date.parse(s.replace(/^[A-Za-z]{3,},\s*/, ""));
    if (!isNaN(t)) return t;
    /* unknown trailing zone token -> drop it (parsed as local, like SimpleDateFormat) */
    t = Date.parse(s.replace(/\s+[A-Za-z]{1,5}$/, ""));
    if (!isNaN(t)) return t;
    /* no-seconds variant: normalize "HH:mm " + zone already covered by Date.parse */
    return 0;
  }

  /* ---------- itunes:duration (hh:mm:ss | mm:ss | secs folding) ---------- */
  function parseDuration(s) {
    s = String(s || "").trim();
    if (!s) return 0;
    if (s.indexOf(":") >= 0) {
      let acc = 0;
      const parts = s.split(":");
      for (let i = 0; i < parts.length; i++) {
        const v = parseInt(parts[i], 10);
        if (isNaN(v)) return 0;
        acc = acc * 60 + v;
      }
      return Math.max(0, Math.min(acc, 0x7fffffff));
    }
    const v = Math.floor(parseFloat(s));
    return isNaN(v) ? 0 : Math.max(0, v);
  }

  /* ============================================================
     RSS PARSING — DOMParser first, regex tag-scanning fallback.
     Returns { title, artworkUrl, description, episodes[] } or
     null when the payload is not XML/RSS shaped at all.
     ============================================================ */
  function dedupeSort(eps) {
    const seen = new Set(), out = [];
    for (const e of eps) {
      if (seen.has(e.guid)) continue;
      seen.add(e.guid);
      out.push(e);
    }
    out.sort((a, b) => b.pubDateMs - a.pubDateMs);
    return out;
  }

  function parseRssDom(doc) {
    const channel = doc.getElementsByTagName("channel")[0];
    if (!channel) return null;
    let title = "", itArt = "", imgArt = "", desc = "";
    for (let n = channel.firstElementChild; n; n = n.nextElementSibling) {
      const tag = String(n.tagName || "").toLowerCase();
      if (tag === "item") continue;
      if (tag === "title") { if (!title) title = String(n.textContent || "").trim(); }
      else if (tag === "itunes:image") { if (!itArt) itArt = String(n.getAttribute("href") || "").trim(); }
      else if (tag === "image") {
        if (!imgArt) {
          const u = n.getElementsByTagName("url")[0];
          if (u) imgArt = String(u.textContent || "").trim();
        }
      }
      else if (tag === "description") { if (!desc) desc = stripHtml(n.textContent); }
    }
    const eps = [];
    const items = doc.getElementsByTagName("item");
    for (let i = 0; i < items.length && eps.length < 300; i++) {
      const it = items[i];
      let t = "", audioUrl = "", size = 0, encType = "", pub = 0, dur = 0, guid = "", d = "";
      for (let n = it.firstElementChild; n; n = n.nextElementSibling) {
        const tag = String(n.tagName || "").toLowerCase();
        if (tag === "title") { const v = String(n.textContent || "").trim(); if (v) t = v; }
        else if (tag === "enclosure") {
          const u = String(n.getAttribute("url") || "").trim();
          const ty = String(n.getAttribute("type") || "").trim().toLowerCase();
          const ln = Math.max(0, parseInt(String(n.getAttribute("length") || "").trim(), 10) || 0);
          if (!audioUrl) { audioUrl = u; size = ln; encType = ty; }
          else if (ty.indexOf("audio") === 0 && encType.indexOf("audio") !== 0 && u) {
            audioUrl = u; size = ln; encType = ty; /* later audio/* replaces a non-audio one */
          }
        }
        else if (tag === "pubdate") { const v = parseRfc822(n.textContent); if (v) pub = v; }
        else if (tag === "itunes:duration") { const v = parseDuration(n.textContent); if (v) dur = v; }
        else if (tag === "guid") { const v = String(n.textContent || "").trim(); if (v) guid = v; }
        else if (tag === "description") { const v = stripHtml(n.textContent); if (v) d = v; }
      }
      if (!audioUrl) continue; /* blank enclosure url -> dropped */
      eps.push({
        guid: guid || audioUrl, title: t || "Untitled episode", audioUrl,
        pubDateMs: pub, durationSec: dur, description: d, sizeBytes: size,
      });
    }
    return {
      title, artworkUrl: itArt || imgArt, description: desc, episodes: dedupeSort(eps),
    };
  }

  /* fallback tag scanning for feeds DOMParser rejects (undeclared namespaces …) */
  function unCdata(s) { return String(s || "").replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1"); }
  function matchTag(block, tag) {
    const re = new RegExp("<" + tag + "(?:\\s[^>]*)?>([\\s\\S]*?)</" + tag + "\\s*>", "i");
    const m = String(block).match(re);
    return m ? m[1] : "";
  }
  function attrOf(attrs, name) {
    const m = String(attrs).match(new RegExp("\\b" + name + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')", "i"));
    return m ? (m[1] != null ? m[1] : m[2]) : "";
  }
  function parseRssScan(text) {
    const s = String(text || "");
    if (!/<(rss|channel)[\s>]/i.test(s) && !/<item[\s>]/i.test(s)) return null;
    const firstItem = s.search(/<item[\s>]/i);
    const head = firstItem >= 0 ? s.slice(0, firstItem) : s;
    const title = decodeEntities(unCdata(matchTag(head, "title"))).trim();
    let art = decodeEntities((head.match(/<itunes:image\b[^>]*\bhref\s*=\s*["']([^"']*)["']/i) || [])[1] || "").trim();
    if (!art) {
      const im = head.match(/<image[\s>][\s\S]*?<url[^>]*>([\s\S]*?)<\/url>/i);
      if (im) art = decodeEntities(unCdata(im[1])).trim();
    }
    const desc = stripHtml(unCdata(matchTag(head, "description")));
    const eps = [];
    const itemRe = /<item[\s>][\s\S]*?<\/item\s*>/gi;
    let m;
    while ((m = itemRe.exec(s)) && eps.length < 300) {
      const b = m[0];
      let audioUrl = "", size = 0, encType = "";
      const encRe = /<enclosure\b([^>]*)>/gi;
      let em;
      while ((em = encRe.exec(b))) {
        const u = decodeEntities(attrOf(em[1], "url")).trim();
        const ty = String(attrOf(em[1], "type") || "").trim().toLowerCase();
        const ln = Math.max(0, parseInt(String(attrOf(em[1], "length") || "").trim(), 10) || 0);
        if (!audioUrl) { audioUrl = u; size = ln; encType = ty; }
        else if (ty.indexOf("audio") === 0 && encType.indexOf("audio") !== 0 && u) {
          audioUrl = u; size = ln; encType = ty;
        }
      }
      if (!audioUrl) continue;
      const t = decodeEntities(unCdata(matchTag(b, "title"))).trim();
      const guid = decodeEntities(unCdata(matchTag(b, "guid"))).trim();
      eps.push({
        guid: guid || audioUrl,
        title: t || "Untitled episode",
        audioUrl,
        pubDateMs: parseRfc822(unCdata(matchTag(b, "pubdate")).trim()),
        durationSec: parseDuration(unCdata(matchTag(b, "itunes:duration")).trim()),
        description: stripHtml(unCdata(matchTag(b, "description"))),
        sizeBytes: size,
      });
    }
    return { title, artworkUrl: art, description: desc, episodes: dedupeSort(eps) };
  }

  function parseRss(text) {
    let doc = null;
    try { doc = new DOMParser().parseFromString(String(text || ""), "text/xml"); } catch (_) { doc = null; }
    if (doc && !doc.getElementsByTagName("parsererror").length) {
      const out = parseRssDom(doc);
      if (out) return out;
    }
    return parseRssScan(text);
  }

  /* ============================================================
     STATE
     ============================================================ */
  let bridge = null;
  let inited = false;
  let rootPanel = null;   /* #view-podcasts                       */
  let podRootEl = null;   /* rebuilt by renderAll                 */
  let layerEl = null;     /* persistent dialog / menu layer       */

  /* store — persisted via onlineload/onlinesave name 'podcasts'.
     Shape (spec): { feeds:[{id,title,rssUrl,artworkUrl,description}],
                     episodes:{ "<feedId>":[{guid,title,audioUrl,pubDateMs,
                     durationSec,description,sizeBytes}] } }
     Desktop extension: episodes remember localPath/localBytes once a
     download completes so downloaded state survives sessions. */
  const store = { feeds: [], episodes: {} };
  let storeLoaded = false;

  let mode = "feeds";          /* 'feeds' | 'discover' (feed level only) */
  let openFeedId = null;       /* internal nav: episode list when set    */
  let paneError = "";          /* dismissable refresh-error line          */
  const refreshing = new Set();/* feed ids with an in-flight refresh      */

  let autoDefaulted = false;   /* one-time Discover auto-default latch    */
  let autoTimerDone = false;

  /* downloads */
  const dlProg = {};           /* guid -> { pct: 0..1 }                   */
  const dlErr = {};            /* guid -> raw error message               */
  let usage = { feeds: [], total: 0, count: 0 };  /* from podusage        */

  /* playback */
  let nowKey = "";             /* online_url of the playing podcast, or "" */
  let lastPlay = null;         /* { url, guid, ts } for streamres matching */
  let playErr = null;          /* { url, msg } inline playback error       */

  /* httpfetch correlation */
  const httpPending = {};      /* id -> resolve(m)                         */
  let httpSeq = 0;

  /* discover */
  let discQuery = "";
  let discActive = "";
  let disc = { status: "idle", rows: [], error: "" };
  let discGen = 0;
  const discCache = {};        /* "top" | "search:<q>" -> { ts, rows }     */
  const discRowState = {};     /* feedUrl+"|"+name -> 'busy'|'done'|error  */
  const lookupCache = {};      /* itunes id -> feedUrl | null (permanent)  */

  /* ---------- small store helpers ---------- */
  function sortFeeds() {
    store.feeds.sort((a, b) => {
      const x = String(a.title || "").toLowerCase(), y = String(b.title || "").toLowerCase();
      return x < y ? -1 : x > y ? 1 : 0;
    });
  }
  function feedById(id) {
    const k = String(id);
    return store.feeds.find((f) => String(f.id) === k) || null;
  }
  function epsOf(id) { return store.episodes[String(id)] || []; }
  function findEp(guid) {
    for (const f of store.feeds) {
      const eps = epsOf(f.id);
      for (const e of eps) if (e.guid === guid) return { feed: f, ep: e };
    }
    return null;
  }
  function subscribedUrls() {
    const set = new Set();
    store.feeds.forEach((f) => set.add(String(f.rssUrl || "").trim().toLowerCase()));
    return set;
  }
  function usageFor(id) {
    const k = String(id);
    const u = (usage.feeds || []).find((x) => String(x.feed) === k);
    return u ? u.bytes : 0;
  }
  function localSize(ep) { return ep.localBytes || ep.sizeBytes || 0; }
  function hostOf(url) {
    try { return new URL(url).hostname || ""; } catch (_) { return ""; }
  }
  function persist() {
    if (!bridge) return;
    try { bridge.send({ cmd: "onlinesave", name: "podcasts", text: JSON.stringify(store) }); } catch (_) {}
  }
  function requestUsage() { if (bridge) bridge.send({ cmd: "podusage" }); }

  /* ============================================================
     HTTP (RSS) via the C bridge — httpfetch/httpbody matched by id
     ============================================================ */
  function httpFetch(url) {
    return new Promise((resolve) => {
      const id = "pod" + Date.now().toString(36) + "-" + (++httpSeq);
      let done = false;
      httpPending[id] = (m) => { if (!done) { done = true; resolve(m); } };
      setTimeout(() => {
        if (httpPending[id]) {
          delete httpPending[id];
          if (!done) { done = true; resolve({ ok: false, status: 0, body: "", timeout: true }); }
        }
      }, 25000);
      bridge.send({ cmd: "httpfetch", url, id });
    });
  }
  function onHttpBody(m) {
    if (!m || !httpPending[m.id]) return; /* multi-consumer type: ours only */
    const fn = httpPending[m.id];
    delete httpPending[m.id];
    fn(m);
  }

  /* fetch + parse one feed URL -> { parsed } | { error } (friendly text) */
  async function fetchFeed(url) {
    const m = await httpFetch(url);
    if (m.timeout) return { error: "Connection timed out" };
    if (!m.ok) {
      if (m.status && (m.status < 200 || m.status >= 300)) return { error: "Server returned HTTP " + m.status };
      return { error: "Couldn't load feed" };
    }
    const parsed = parseRss(m.body || "");
    if (!parsed) return { error: "Not a valid RSS feed" };
    return { parsed };
  }

  /* ============================================================
     SUBSCRIBE / REFRESH / UNSUBSCRIBE
     ============================================================ */
  /* URL normalization (spec): trim; keep http(s)://; reject other schemes;
     bare host gets https:// prepended; must end up > 9 chars. */
  function normalizeFeedUrl(raw) {
    let url = String(raw || "").trim();
    if (!url) return null;
    if (url.indexOf("://") >= 0) {
      if (!/^https?:\/\//i.test(url)) return null;
    } else {
      url = "https://" + url;
    }
    if (url.length <= 9) return null;
    return url;
  }

  async function subscribeUrl(raw) {
    const url = normalizeFeedUrl(raw);
    if (!url) return { ok: false, error: "Enter a valid http(s) feed URL" };

    const existing = store.feeds.find((f) => String(f.rssUrl || "").toLowerCase() === url.toLowerCase());
    if (existing) {
      /* re-subscribing a known URL refreshes it instead of duplicating */
      const r = await refreshFeed(existing.id, { fromSubscribe: true });
      if (r && r.error) return { ok: false, error: r.error };
      return { ok: true, feed: existing };
    }

    const got = await fetchFeed(url);
    if (got.error) return { ok: false, error: got.error };
    const p = got.parsed;
    if (!p.episodes.length && !p.title) return { ok: false, error: "Not a podcast RSS feed" };

    const id = store.feeds.reduce((mx, f) => Math.max(mx, +f.id || 0), 0) + 1;
    const feed = {
      id,
      title: p.title || hostOf(url) || "Podcast",
      rssUrl: url,
      artworkUrl: p.artworkUrl || "",
      description: p.description || "",
    };
    store.feeds.push(feed);
    sortFeeds();
    store.episodes[String(id)] = p.episodes.slice(0, 300);
    persist();
    renderAll();
    return { ok: true, feed };
  }

  async function refreshFeed(id, opts) {
    opts = opts || {};
    const feed = feedById(id);
    if (!feed) return { ok: false, error: "Feed not found" };
    if (refreshing.has(feed.id)) return { ok: false, error: "" }; /* concurrent refresh ignored */
    refreshing.add(feed.id);
    renderAll();

    const got = await fetchFeed(feed.rssUrl);
    refreshing.delete(feed.id);

    let err = "";
    if (got.error) err = got.error;
    else if (!got.parsed.episodes.length && !got.parsed.title) err = "Not a podcast RSS feed";

    if (err) {
      if (!opts.fromSubscribe) paneError = err;
      renderAll();
      return { ok: false, error: err };
    }

    const p = got.parsed;
    /* keep old values when new ones are blank */
    if (p.title) feed.title = p.title;
    if (p.artworkUrl) feed.artworkUrl = p.artworkUrl;
    if (p.description) feed.description = p.description;

    /* REPLACE the episode list, carrying local download state over by guid
       (the file on disk is unaffected by a refresh) */
    const localBy = {};
    epsOf(feed.id).forEach((e) => { if (e.localPath) localBy[e.guid] = e; });
    store.episodes[String(feed.id)] = p.episodes.slice(0, 300).map((e) => {
      const l = localBy[e.guid];
      return l ? Object.assign({}, e, { localPath: l.localPath, localBytes: l.localBytes || 0 }) : e;
    });
    sortFeeds();
    persist();
    renderAll();
    return { ok: true };
  }

  function confirmUnsubscribe(feed) {
    const ub = usageFor(feed.id);
    const body = ub > 0
      ? "Its " + fmtBytes(ub) + " of downloads will also be deleted."
      : "The feed and its episode list will be removed.";
    confirmDlg('Unsubscribe from "' + feed.title + '"?', body, "Unsubscribe", true, () => {
      /* wipe downloads FIRST, then drop from the store */
      bridge.send({ cmd: "podwipe", feed: String(feed.id) });
      store.feeds = store.feeds.filter((f) => f !== feed);
      delete store.episodes[String(feed.id)];
      if (openFeedId != null && String(openFeedId) === String(feed.id)) openFeedId = null;
      persist();
      requestUsage();
      renderAll();
    });
  }

  function wipeFeedDownloads(feedIdStr, title, bytes) {
    confirmDlg(
      "Delete all downloads?",
      'Frees ' + fmtBytes(bytes) + ' used by "' + title + '". Episodes stay streamable.',
      "Delete all", true,
      () => {
        bridge.send({ cmd: "podwipe", feed: String(feedIdStr) });
        epsOf(feedIdStr).forEach((e) => { e.localPath = ""; e.localBytes = 0; });
        persist();
        requestUsage();
        renderAll();
      }
    );
  }

  /* ============================================================
     DOWNLOADS — poddownload / poddl / poddlcancel / poddelete
     ============================================================ */
  function startDownload(feed, ep) {
    if (dlProg[ep.guid] || ep.localPath) return; /* idempotent */
    delete dlErr[ep.guid];
    dlProg[ep.guid] = { pct: 0 };
    bridge.send({
      cmd: "poddownload",
      url: ep.audioUrl,
      feed: String(feed.id),
      guid: ep.guid,
      fname: fnameFor(ep),
    });
    updateEpRow(ep.guid);
  }
  function cancelDownload(ep) {
    bridge.send({ cmd: "poddlcancel", guid: ep.guid });
    delete dlProg[ep.guid];
    updateEpRow(ep.guid);
  }
  function deleteDownload(feed, ep) {
    bridge.send({ cmd: "poddelete", feed: String(feed.id), fname: fnameFor(ep) });
    ep.localPath = "";
    ep.localBytes = 0;
    persist();
    requestUsage();
    updateEpRow(ep.guid);
  }

  function onPodDl(m) {
    if (!m || m.guid == null) return;
    const guid = String(m.guid);
    const known = dlProg[guid] || dlErr[guid] || findEp(guid);
    if (!known) return;

    if (m.error) {
      delete dlProg[guid];
      const raw = String(m.error);
      if (!/cancel/i.test(raw)) dlErr[guid] = raw; /* silent on user cancel */
      updateEpRow(guid);
      return;
    }
    if (m.done) {
      delete dlProg[guid];
      delete dlErr[guid];
      const loc = findEp(guid);
      if (loc) {
        const path = String(m.path || m.file || m.dest || "");
        if (path) loc.ep.localPath = path;
        const b = (+m.total > 0) ? +m.total : ((+m.bytes > 0) ? +m.bytes : 0);
        if (b) loc.ep.localBytes = b;
        persist();
      }
      requestUsage();      /* refreshes header stats + storage card */
      renderAll();
      return;
    }
    /* progress tick — accept 0..1 fraction or 0..100 percent */
    let frac = +m.pct || 0;
    if (frac > 1) frac = frac / 100;
    dlProg[guid] = { pct: Math.max(0, Math.min(frac, 1)) };
    updateEpRow(guid);
  }

  function onPodUsage(m) {
    if (!m || !Array.isArray(m.feeds)) return;
    usage = {
      feeds: m.feeds.map((u) => ({ feed: String(u.feed), bytes: +u.bytes || 0, files: +u.files || 0 })),
      total: +m.total || 0,
      count: +m.count || 0,
    };
    renderAll();
  }

  /* ============================================================
     PLAYBACK — streamplay kind:'podcast'; local file when downloaded
     ============================================================ */
  function playUrlOf(ep) { return ep.localPath || ep.audioUrl; }

  function playEp(feed, ep) {
    playErr = null;
    const msg = {
      cmd: "streamplay",
      kind: "podcast",
      title: ep.title,
      artist: feed.title,
      duration_ms: (ep.durationSec || 0) * 1000,
      art: (feed.artworkUrl || ""),
    };
    if (ep.localPath) { msg.url = ep.localPath; msg.local = true; }
    else msg.url = ep.audioUrl;
    lastPlay = { url: msg.url, guid: ep.guid, ts: Date.now() };
    lastPlayMeta = {
      url: msg.url,
      description: ep.description || "",
      date: ep.pubDateMs ? new Date(ep.pubDateMs).toLocaleDateString(
        undefined, { day: "numeric", month: "short", year: "numeric" }) : "",
      duration: ep.durationSec
        ? (ep.durationSec >= 3600
            ? Math.floor(ep.durationSec / 3600) + "h " +
              Math.round((ep.durationSec % 3600) / 60) + "m"
            : Math.max(1, Math.round(ep.durationSec / 60)) + " min")
        : "",
    };
    bridge.send(msg);
    updateEpRow(ep.guid);
  }

  function onStreamRes(m) {
    if (!m) return;
    /* streamres is shared with the radio/streams modules — only react to OUR
       last play (matched by url when the reply carries one). */
    if (!lastPlay) return;
    if (m.url && m.url !== lastPlay.url) return;
    if (!m.url && Date.now() - lastPlay.ts > 30000) return;
    if (m.ok) {
      if (playErr) { playErr = null; updateEpRow(lastPlay.guid); }
      return;
    }
    playErr = { url: lastPlay.url, msg: String(m.error || "Playback failed") };
    updateEpRow(lastPlay.guid);
  }

  function onNow(m) {
    if (!m || m.online === undefined) return; /* lite ticks without stream info */
    const key = (m.online && m.online_kind === "podcast") ? String(m.online_url || "") : "";
    if (key === nowKey) return;
    nowKey = key;
    if (!podRootEl) return;
    $$(".pod-erow", podRootEl).forEach((r) => {
      r.classList.toggle("playing", !!nowKey && r.dataset.purl === nowKey);
    });
  }

  /* ============================================================
     DISCOVER — iTunes directory via plain fetch() (CORS-friendly)
     ============================================================ */
  function fetchJson(url) {
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), 15000);
    return fetch(url, { signal: ctl.signal })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      })
      .finally(() => clearTimeout(t));
  }

  async function itunesSearch(q) {
    const j = await fetchJson("https://itunes.apple.com/search?media=podcast&limit=50&term=" + encodeURIComponent(q));
    const out = [];
    ((j && j.results) || []).forEach((x) => {
      const feedUrl = String(x.feedUrl || "").trim();
      const name = String(x.collectionName || "").trim();
      if (!feedUrl || !name) return; /* skip empty feedUrl / collectionName */
      out.push({
        name,
        author: String(x.artistName || ""),
        feedUrl,
        artworkUrl: String(x.artworkUrl600 || x.artworkUrl100 || ""),
        genre: String(x.primaryGenreName || ""),
      });
    });
    return out;
  }

  async function itunesTop() {
    const j = await fetchJson("https://itunes.apple.com/us/rss/toppodcasts/limit=50/json");
    let entries = j && j.feed && j.feed.entry;
    if (!entries) entries = [];
    if (!Array.isArray(entries)) entries = [entries]; /* bare object when 1 item */
    const out = [];
    entries.forEach((e) => {
      const name = e && e["im:name"] && e["im:name"].label ? String(e["im:name"].label) : "";
      const id = e && e.id && e.id.attributes && e.id.attributes["im:id"] ? String(e.id.attributes["im:id"]) : "";
      if (!name || !id) return;
      const author = e["im:artist"] && e["im:artist"].label ? String(e["im:artist"].label) : "";
      let art = "", best = -1;
      const imgs = Array.isArray(e["im:image"]) ? e["im:image"] : (e["im:image"] ? [e["im:image"]] : []);
      imgs.forEach((im, i) => {
        const h = im && im.attributes && im.attributes.height ? parseInt(im.attributes.height, 10) : NaN;
        const score = isNaN(h) ? i : h; /* missing height -> index, later wins */
        if (score >= best) { best = score; art = String((im && im.label) || ""); }
      });
      let genre = "";
      const cat = Array.isArray(e.category) ? e.category[0] : e.category;
      if (cat && cat.attributes && cat.attributes.label) genre = String(cat.attributes.label);
      out.push({ name, author, feedUrl: "itunes-id:" + id, artworkUrl: art, genre });
    });
    return out;
  }

  async function lookupFeed(id) {
    if (Object.prototype.hasOwnProperty.call(lookupCache, id)) return lookupCache[id];
    let j = null;
    try { j = await fetchJson("https://itunes.apple.com/lookup?id=" + encodeURIComponent(id)); }
    catch (_) { return null; } /* network failure: not cached */
    const fu = j && j.results && j.results[0] ? String(j.results[0].feedUrl || "").trim() : "";
    const ok = fu && /^http/i.test(fu) ? fu : null;
    lookupCache[id] = ok; /* cached forever, even when null */
    return ok;
  }

  function ensureDiscover() { if (disc.status === "idle") loadDiscover(false); }

  async function loadDiscover(force) {
    const q = discActive;
    const key = q ? "search:" + q.toLowerCase() : "top";
    const c = discCache[key];
    if (!force && c && Date.now() - c.ts < 600000) { /* 10-min cache */
      disc = { status: "done", rows: c.rows, error: "" };
      renderAll();
      return;
    }
    const gen = ++discGen;
    disc = { status: "loading", rows: [], error: "" };
    renderAll();
    try {
      const rows = q ? await itunesSearch(q) : await itunesTop();
      if (gen !== discGen) return;
      discCache[key] = { ts: Date.now(), rows };
      disc = { status: "done", rows, error: "" };
    } catch (_) {
      if (gen !== discGen) return;
      disc = { status: "error", rows: [], error: "Couldn't reach the podcast directory" };
    }
    renderAll();
  }

  function submitSearch() {
    discActive = discQuery.trim();
    loadDiscover(false);
  }

  async function discSubscribe(entry, key) {
    discRowState[key] = "busy";
    renderAll();
    let url = entry.feedUrl;
    try {
      if (/^itunes-id:/i.test(url)) {
        const resolved = await lookupFeed(url.slice(10));
        if (!resolved) { discRowState[key] = "No feed URL for this show"; renderAll(); return; }
        url = resolved;
      }
      const res = await subscribeUrl(url);
      discRowState[key] = res.ok ? "done" : (res.error || "Couldn't load feed");
    } catch (_) {
      discRowState[key] = "Couldn't load feed";
    }
    renderAll();
  }

  /* ============================================================
     STORE LOAD (onlineload/onlinesave name 'podcasts')
     ============================================================ */
  function onOnlineFile(m) {
    if (!m || m.name !== "podcasts") return; /* filter multi-consumer type */
    try {
      const j = JSON.parse(m.text || "null");
      if (j && typeof j === "object") {
        store.feeds = Array.isArray(j.feeds)
          ? j.feeds.filter((f) => f && f.rssUrl).map((f) => ({
              id: +f.id || 0,
              title: String(f.title || ""),
              rssUrl: String(f.rssUrl || ""),
              artworkUrl: String(f.artworkUrl || ""),
              description: String(f.description || ""),
            }))
          : [];
        store.episodes = {};
        if (j.episodes && typeof j.episodes === "object") {
          Object.keys(j.episodes).forEach((k) => {
            const arr = Array.isArray(j.episodes[k]) ? j.episodes[k] : [];
            const eps = arr.slice(0, 300).map((e) => ({
              guid: String((e && (e.guid || e.audioUrl)) || ""),
              title: String((e && e.title) || "Untitled episode"),
              audioUrl: String((e && e.audioUrl) || ""),
              pubDateMs: +(e && e.pubDateMs) || 0,
              durationSec: +(e && e.durationSec) || 0,
              description: String((e && e.description) || ""),
              sizeBytes: +(e && e.sizeBytes) || 0,
              localPath: String((e && e.localPath) || ""),
              localBytes: +(e && e.localBytes) || 0,
            })).filter((e) => e.audioUrl);
            eps.sort((a, b) => b.pubDateMs - a.pubDateMs);
            store.episodes[k] = eps;
          });
        }
        sortFeeds();
      }
    } catch (_) { /* corrupt blob -> start empty */ }
    storeLoaded = true;
    maybeAutoDiscover();
    renderAll();
  }

  /* Discover auto-default: 600ms after first open, zero subscriptions,
     one-time latch. */
  function maybeAutoDiscover() {
    if (autoDefaulted || !autoTimerDone || !storeLoaded) return;
    autoDefaulted = true;
    if (!store.feeds.length && mode === "feeds" && openFeedId == null) {
      mode = "discover";
      ensureDiscover();
      renderAll();
    }
  }

  /* ============================================================
     DIALOGS + CONTEXT MENU (live in the persistent layer)
     ============================================================ */
  let dlgEl = null, menuEl = null;

  function closeDialog() { if (dlgEl) { dlgEl.remove(); dlgEl = null; } }
  function openDialog(build) {
    closeDialog(); closeMenu();
    dlgEl = el("div", "pod-dim");
    const card = el("div", "pod-dlg");
    dlgEl.appendChild(card);
    dlgEl.addEventListener("mousedown", (e) => { if (e.target === dlgEl) closeDialog(); });
    build(card, closeDialog);
    layerEl.appendChild(dlgEl);
  }
  function confirmDlg(title, body, confirmLabel, danger, onConfirm) {
    openDialog((card, close) => {
      card.appendChild(el("div", "pod-dlg-title", title));
      card.appendChild(el("div", "pod-dlg-body", body));
      const row = el("div", "pod-dlg-btns");
      const c = el("button", "pod-btn ghost", "Cancel");
      c.addEventListener("click", close);
      const ok = el("button", "pod-btn " + (danger ? "danger" : "accent"), confirmLabel);
      ok.addEventListener("click", () => { close(); onConfirm(); });
      row.appendChild(c); row.appendChild(ok);
      card.appendChild(row);
    });
  }

  function closeMenu() {
    if (menuEl) {
      menuEl.remove();
      menuEl = null;
      document.removeEventListener("click", closeMenuCapture, true);
    }
  }
  function closeMenuCapture() { closeMenu(); }
  function openMenu(ev, items, headerText) {
    ev.preventDefault();
    ev.stopPropagation();
    closeMenu();
    const m = el("div", "pod-menu");
    if (headerText) m.appendChild(el("div", "pod-menu-head", headerText));
    items.forEach((it) => {
      if (!it) return;
      const b = el("button", "pod-menu-item" + (it.danger ? " danger" : ""), it.label);
      b.addEventListener("click", () => { closeMenu(); it.fn(); });
      m.appendChild(b);
    });
    layerEl.appendChild(m);
    menuEl = m;
    /* NOTE: .view-panel has contain:layout, which makes the panel the
       containing block even for fixed descendants — position relative to
       the layer, converting from viewport coords. */
    const base = layerEl.getBoundingClientRect();
    const r = m.getBoundingClientRect();
    let x = ev.clientX - base.left, y = ev.clientY - base.top;
    if (x + r.width > base.width - 8) x = Math.max(8, base.width - r.width - 8);
    if (y + r.height > base.height - 8) y = Math.max(8, base.height - r.height - 8);
    m.style.left = x + "px";
    m.style.top = y + "px";
    setTimeout(() => document.addEventListener("click", closeMenuCapture, true), 0);
  }

  function openAddDialog() {
    openDialog((card, close) => {
      card.appendChild(el("div", "pod-dlg-title", "Add podcast"));
      card.appendChild(el("label", "pod-dlg-label", "RSS feed URL"));
      const inp = el("input", "pod-dlg-input");
      inp.type = "text";
      inp.placeholder = "https://example.com/feed.xml";
      inp.spellcheck = false;
      card.appendChild(inp);
      const busy = el("div", "pod-dlg-busy");
      busy.hidden = true;
      busy.appendChild(spinner(16));
      busy.appendChild(el("span", null, "Fetching feed…"));
      card.appendChild(busy);
      const err = el("div", "pod-dlg-err");
      err.hidden = true;
      card.appendChild(err);
      const btns = el("div", "pod-dlg-btns");
      const cancel = el("button", "pod-btn ghost", "Cancel");
      cancel.addEventListener("click", close);
      const sub = el("button", "pod-btn accent", "Subscribe");
      let working = false;
      const go = async () => {
        if (working) return;
        working = true;
        sub.disabled = true;
        busy.hidden = false;
        err.hidden = true;
        const res = await subscribeUrl(inp.value);
        if (res.ok) { close(); return; }
        err.textContent = res.error || "Couldn't load feed";
        err.hidden = false;
        busy.hidden = true;
        sub.disabled = false;
        working = false;
      };
      sub.addEventListener("click", go);
      inp.addEventListener("keydown", (e) => { if (e.key === "Enter") go(); });
      btns.appendChild(cancel);
      btns.appendChild(sub);
      card.appendChild(btns);
      setTimeout(() => inp.focus(), 30);
    });
  }

  /* ============================================================
     RENDER
     ============================================================ */
  function renderAll() {
    if (!podRootEl) return;
    const oldScroll = $(".pod-scroll", podRootEl);
    const st = oldScroll ? oldScroll.scrollTop : 0;
    podRootEl.innerHTML = "";

    podRootEl.appendChild(renderHeader());

    const scroll = el("div", "pod-scroll");
    scroll.addEventListener("scroll", closeMenu, { passive: true });
    if (openFeedId != null) renderEpisodes(scroll);
    else if (mode === "discover") renderDiscover(scroll);
    else renderFeeds(scroll);
    podRootEl.appendChild(scroll);
    scroll.scrollTop = st;

    if (openFeedId == null) {
      const fab = el("button", "pod-fab", "+");
      fab.title = "Add podcast";
      fab.setAttribute("aria-label", "Add podcast");
      fab.addEventListener("click", openAddDialog);
      podRootEl.appendChild(fab);
    }
  }

  function renderHeader() {
    const h = el("div", "pod-head");
    const row = el("div", "pod-head-row");
    const tw = el("div", "pod-head-text");
    tw.appendChild(el("div", "pod-title", "Podcasts"));
    const n = store.feeds.length;
    tw.appendChild(el("div", "pod-stats",
      n + " subscription" + (n === 1 ? "" : "s") + " · " +
      usage.count + " downloaded · " + fmtBytes(usage.total) + " used"));
    row.appendChild(tw);
    h.appendChild(row);

    if (paneError) {
      const e = el("div", "pod-panerr", paneError);
      e.title = "Dismiss";
      e.addEventListener("click", () => { paneError = ""; renderAll(); });
      h.appendChild(e);
    }

    if (openFeedId == null) {
      const chips = el("div", "pod-chips");
      [["feeds", "My podcasts"], ["discover", "Discover"]].forEach(([id, label]) => {
        const c = el("button", "pod-chip" + (mode === id ? " on" : ""), label);
        c.addEventListener("click", () => {
          if (mode === id) return;
          mode = id;
          if (id === "discover") ensureDiscover();
          renderAll();
        });
        chips.appendChild(c);
      });
      h.appendChild(chips);
    }
    return h;
  }

  /* ---------- feed list ---------- */
  function renderFeeds(scroll) {
    if (!store.feeds.length) {
      const em = el("div", "pod-empty");
      em.appendChild(icon("pod", "pod-empty-ico"));
      em.appendChild(el("div", "pod-empty-title", "No podcasts yet"));
      em.appendChild(el("div", "pod-empty-sub", "Tap + and paste an RSS feed URL to subscribe."));
      scroll.appendChild(em);
    } else {
      const list = el("div", "pod-list");
      store.feeds.forEach((f) => list.appendChild(feedRow(f)));
      scroll.appendChild(list);
    }
    scroll.appendChild(storageCard());
  }

  function refreshBtn(fid) {
    const b = el("button", "pod-refresh");
    if (refreshing.has(fid)) {
      b.classList.add("busy");
      b.appendChild(spinner(18));
    } else {
      b.innerHTML = SVG.refresh;
      b.title = "Refresh";
      b.addEventListener("click", (ev) => { ev.stopPropagation(); refreshFeed(fid); });
    }
    return b;
  }

  function feedRow(f) {
    const r = el("div", "pod-frow");
    r.dataset.fid = String(f.id);
    r.appendChild(el("div", "pod-avatar", firstLetter(f.title)));
    const mid = el("div", "pod-fmid");
    mid.appendChild(el("div", "pod-ftitle", f.title));
    const count = epsOf(f.id).length;
    const ub = usageFor(f.id);
    let sub = count + " episode" + (count === 1 ? "" : "s");
    if (ub > 0) sub += " · " + fmtBytes(ub);
    mid.appendChild(el("div", "pod-fsub", sub));
    r.appendChild(mid);
    r.appendChild(refreshBtn(f.id));
    r.addEventListener("click", (ev) => {
      if (ev.target.closest(".pod-refresh")) return;
      openFeedId = f.id;
      renderAll();
    });
    r.addEventListener("contextmenu", (ev) => openMenu(ev, [
      { label: "Refresh", fn: () => refreshFeed(f.id) },
      { label: "Unsubscribe…", danger: true, fn: () => confirmUnsubscribe(f) },
    ], f.title));
    return r;
  }

  function storageCard() {
    const c = el("div", "pod-storage");
    c.appendChild(el("div", "pod-storage-title", "Manage storage"));
    const rows = (usage.feeds || []).filter((u) => u.bytes > 0);
    if (!rows.length) {
      c.appendChild(el("div", "pod-storage-empty",
        "No downloads yet — episodes stream until you download them."));
      return c;
    }
    rows.forEach((u) => {
      const f = feedById(u.feed);
      const title = f ? f.title : ("Feed " + u.feed);
      const r = el("div", "pod-storage-row");
      r.appendChild(el("div", "pod-storage-name", title));
      r.appendChild(el("div", "pod-storage-size", fmtBytes(u.bytes)));
      const d = el("button", "pod-storage-del", "Delete all");
      d.addEventListener("click", () => wipeFeedDownloads(u.feed, title, u.bytes));
      r.appendChild(d);
      c.appendChild(r);
    });
    return c;
  }

  /* ---------- episode list ---------- */
  function renderEpisodes(scroll) {
    const f = feedById(openFeedId);
    if (!f) { openFeedId = null; renderFeeds(scroll); return; }

    const back = el("div", "pod-back");
    const bb = el("button", "pod-back-btn");
    bb.appendChild(el("span", "pod-back-arrow", "←"));
    bb.appendChild(el("span", "pod-back-title", f.title));
    bb.addEventListener("click", () => { openFeedId = null; renderAll(); });
    back.appendChild(bb);
    back.appendChild(refreshBtn(f.id));
    scroll.appendChild(back);

    const eps = epsOf(f.id);
    if (!eps.length) {
      const em = el("div", "pod-empty");
      em.appendChild(el("div", "pod-empty-title", "No episodes"));
      em.appendChild(el("div", "pod-empty-sub", "Pull the refresh icon to re-fetch this feed."));
      scroll.appendChild(em);
      return;
    }
    const list = el("div", "pod-list");
    eps.forEach((ep) => list.appendChild(epRow(f, ep)));
    scroll.appendChild(list);
  }

  function epSubtitle(ep) {
    const parts = [];
    if (ep.pubDateMs > 0) parts.push(fmtDate(ep.pubDateMs));
    if (ep.durationSec > 0) parts.push(fmtDur(ep.durationSec));
    if (ep.localPath) {
      const b = localSize(ep);
      parts.push(b > 0 ? fmtBytes(b) : "size unknown");
    } else {
      parts.push(ep.sizeBytes > 0 ? fmtBytes(ep.sizeBytes) : "size unknown");
    }
    return parts.join(" · ");
  }

  function ringBtn(ep) {
    const b = el("button", "pod-ring");
    b.title = "Cancel download";
    const R = 10.5, C = 2 * Math.PI * R;
    b.innerHTML =
      '<svg viewBox="0 0 26 26">' +
      '<circle class="pod-ring-track" cx="13" cy="13" r="' + R + '"/>' +
      '<circle class="pod-ring-fill" cx="13" cy="13" r="' + R + '"/>' +
      "</svg>";
    const fill = $(".pod-ring-fill", b);
    fill.style.strokeDasharray = C.toFixed(2);
    const frac = (dlProg[ep.guid] && dlProg[ep.guid].pct) || 0;
    if (frac <= 0) b.classList.add("ind"); /* indeterminate until pct>0 */
    else fill.style.strokeDashoffset = (C * (1 - Math.min(frac, 0.99))).toFixed(2);
    const x = el("span", "pod-ring-x");
    x.innerHTML = SVG.close;
    b.appendChild(x);
    b.addEventListener("click", (ev) => { ev.stopPropagation(); cancelDownload(ep); });
    return b;
  }

  function epControl(f, ep) {
    const box = el("div", "pod-ectrl");
    if (dlProg[ep.guid]) {
      box.appendChild(ringBtn(ep));
    } else if (ep.localPath) {
      const w = el("div", "pod-done");
      w.appendChild(icon("check", "pod-check"));
      const b = localSize(ep);
      if (b > 0) w.appendChild(el("span", "pod-done-size", fmtBytes(b)));
      box.appendChild(w);
    } else {
      const b = el("button", "pod-dlbtn");
      b.title = "Download";
      b.innerHTML = SVG.download;
      b.addEventListener("click", (ev) => { ev.stopPropagation(); startDownload(f, ep); });
      box.appendChild(b);
    }
    return box;
  }

  function epMenuItems(f, ep) {
    const items = [{ label: "Play", fn: () => playEp(f, ep) }];
    if (ep.localPath) {
      items.push({
        label: "Delete download (" + fmtBytes(localSize(ep)) + ")",
        danger: true,
        fn: () => deleteDownload(f, ep),
      });
    } else if (!dlProg[ep.guid]) {
      items.push({ label: "Download", fn: () => startDownload(f, ep) });
    }
    return items;
  }

  function epRow(f, ep) {
    const r = el("div", "pod-erow");
    r.dataset.guid = ep.guid;
    const purl = playUrlOf(ep);
    r.dataset.purl = purl;
    if (nowKey && purl === nowKey) r.classList.add("playing");

    const mid = el("div", "pod-emid");
    mid.appendChild(el("div", "pod-etitle", ep.title));
    mid.appendChild(el("div", "pod-esub", epSubtitle(ep)));
    if (dlErr[ep.guid]) mid.appendChild(el("div", "pod-eperr", "Download failed: " + dlErr[ep.guid]));
    if (playErr && playErr.url === purl) mid.appendChild(el("div", "pod-eperr", playErr.msg));
    r.appendChild(mid);

    r.appendChild(epControl(f, ep));
    r.addEventListener("click", (ev) => {
      if (ev.target.closest(".pod-ectrl")) return;
      playEp(f, ep);
    });
    r.addEventListener("contextmenu", (ev) => openMenu(ev, epMenuItems(f, ep), ep.title));
    return r;
  }

  /* Targeted row refresh so download progress never resets scroll. */
  function updateEpRow(guid) {
    if (!podRootEl) return;
    const rows = $$(".pod-erow", podRootEl);
    for (const row of rows) {
      if (row.dataset.guid !== guid) continue;
      const loc = findEp(guid);
      if (loc) row.replaceWith(epRow(loc.feed, loc.ep));
      return;
    }
  }

  /* ---------- discover ---------- */
  function artEl(url, name) {
    const wrap = el("div", "pod-dart");
    const tint = TINTS[Math.abs(strHash(name)) % TINTS.length];
    wrap.style.background = tint + "2e"; /* 18% alpha */
    const letter = el("span", "pod-dletter", firstLetter(name));
    letter.style.color = tint;
    wrap.appendChild(letter);
    if (url && /^https?:\/\//i.test(url)) {
      const img = document.createElement("img");
      img.loading = "lazy";
      img.decoding = "async";
      img.alt = "";
      img.addEventListener("error", () => img.remove());
      img.src = url;
      wrap.appendChild(img);
    }
    return wrap;
  }

  function renderDiscover(scroll) {
    const bar = el("div", "pod-search");
    const ic = el("button", "pod-search-ico" + (discQuery.trim() ? " hot" : ""));
    ic.innerHTML = SVG.pod;
    ic.title = "Search";
    const inp = el("input", "pod-search-input");
    inp.type = "text";
    inp.placeholder = "Search podcasts worldwide…";
    inp.value = discQuery;
    inp.spellcheck = false;
    inp.addEventListener("input", () => {
      discQuery = inp.value;
      ic.classList.toggle("hot", !!inp.value.trim());
    });
    inp.addEventListener("keydown", (e) => { if (e.key === "Enter") submitSearch(); });
    ic.addEventListener("click", submitSearch);
    bar.appendChild(ic);
    bar.appendChild(inp);
    scroll.appendChild(bar);

    scroll.appendChild(el("div", "pod-caption",
      discActive ? 'Results for "' + discActive + '"' : "Top podcasts · iTunes directory"));

    if (disc.status === "loading" || disc.status === "idle") {
      const c = el("div", "pod-cstate");
      c.appendChild(spinner(32));
      scroll.appendChild(c);
      return;
    }
    if (disc.status === "error") {
      const c = el("div", "pod-cstate");
      c.appendChild(icon("pod", "pod-empty-ico"));
      c.appendChild(el("div", "pod-empty-sub", disc.error || "Couldn't reach the podcast directory"));
      const retry = el("button", "pod-retry", "Retry");
      retry.addEventListener("click", () => loadDiscover(true));
      c.appendChild(retry);
      scroll.appendChild(c);
      return;
    }
    if (!disc.rows.length) {
      const c = el("div", "pod-cstate");
      c.appendChild(el("div", "pod-empty-sub", "No podcasts found"));
      scroll.appendChild(c);
      return;
    }
    const list = el("div", "pod-list");
    const subs = subscribedUrls();
    disc.rows.forEach((e) => list.appendChild(discRow(e, subs)));
    scroll.appendChild(list);
  }

  function discRow(e, subs) {
    const key = e.feedUrl + "|" + e.name;
    const r = el("div", "pod-drow");
    r.appendChild(artEl(e.artworkUrl, e.name));
    const mid = el("div", "pod-dmid");
    mid.appendChild(el("div", "pod-dname", e.name));
    const sub = [e.author, e.genre].filter(Boolean).join(" · ") || "Podcast";
    mid.appendChild(el("div", "pod-dsub", sub));
    const st = discRowState[key];
    if (st && st !== "busy" && st !== "done") mid.appendChild(el("div", "pod-eperr", st));
    r.appendChild(mid);

    const right = el("div", "pod-dright");
    const isSub = st === "done" || subs.has(String(e.feedUrl || "").trim().toLowerCase());
    if (isSub) {
      right.appendChild(icon("check", "pod-dcheck"));
    } else if (st === "busy") {
      right.appendChild(spinner(20));
    } else {
      const b = el("button", "pod-addpill", "+ Add");
      b.addEventListener("click", () => discSubscribe(e, key));
      right.appendChild(b);
    }
    r.appendChild(right);
    return r;
  }

  /* ============================================================
     OPEN (called by app.js each time the nav entry is clicked)
     ============================================================ */
  function open(b) {
    bridge = b;
    rootPanel = document.getElementById("view-podcasts");
    if (!rootPanel) return;
    if (!inited) {
      inited = true;
      rootPanel.innerHTML = "";
      podRootEl = el("div", "pod-root");
      layerEl = el("div", "pod-layer");
      rootPanel.appendChild(podRootEl);
      rootPanel.appendChild(layerEl);

      /* NEVER on() — these types are shared with app.js / other modules. */
      bridge.tap("onlinefile", onOnlineFile);
      bridge.tap("httpbody", onHttpBody);
      bridge.tap("poddl", onPodDl);
      bridge.tap("podusage", onPodUsage);
      bridge.tap("streamres", onStreamRes);
      bridge.tap("now", onNow);

      document.addEventListener("keydown", (e) => {
        if (e.key === "Escape") { closeMenu(); closeDialog(); }
      });

      bridge.send({ cmd: "onlineload", name: "podcasts" });
      setTimeout(() => { autoTimerDone = true; maybeAutoDiscover(); }, 600);
      renderAll();
    } else {
      renderAll();
    }
    requestUsage();
  }

  return { open,
    nowMeta: (url) => (lastPlayMeta && lastPlayMeta.url === url) ? lastPlayMeta : lastPlayMeta,
  };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("podcasts", "1.0.0", [], function () { return window.MnPodcasts || {}; });
