/* ============================================================
   MEDIA MANAGER — mediatool.js                          v2.0.0
   ------------------------------------------------------------
   Faithful port of Media Manager Pro v2.20 ("Film Strip
   Edition") into a self-contained Monatomic module, upgraded
   with real image-search engines. Five tabs:

     🖼 Album Art   — sortable album table, batch fetch with a
                      live FILM STRIP, SMART MATCH (perceptual
                      aHash+dHash), REVIEW MODE (↑ accept /
                      ↓ deny / ←→ alternatives / Esc), apply
                      through artwrite (whole album).
     🎤 Lyrics      — Genius + AZLyrics (ported parsers) +
                      LRCLIB synced; skip-existing via
                      lyricsread; embed via lyricswrite.
     📝 Consistency — per-FOLDER tag-value maps (album/artist/
                      album artist/genre/year); fix dialog picks
                      the value (majority default); auto-fix.
     🔄 Duplicates  — artist+title groups ranked by quality
                      (bitrate → size), auto select-lower,
                      export list.
     📂 Classifier  — media-type detection (Film/Game/Anime OST,
                      Audiobook, Podcast) by keyword scoring.

   Art sources: iTunes (3000×3000 CDN trick), Deezer XL,
   MusicBrainz→CoverArtArchive originals, Last.fm — plus REAL
   image engines extracting the ORIGINAL image (never the
   thumbnail): DuckDuckGo, Bing, Google, Yandex.

   Backend isolation: reads rows via {"cmd":"mmtracks"} (its own
   filter spec — the tool never disturbs the app's view state).
   ============================================================ */
window.MnMediaTool = (function () {
  "use strict";

  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const Q = encodeURIComponent;
  const dirOf = (p) => { const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/")); return i > 0 ? p.slice(0, i) : p; };
  const fmtQ = (t) => (t.bitrate ? t.bitrate + "k" : "?") + " · " + (t.size ? (t.size / 1048576).toFixed(1) + "MB" : "?");
  /* motion system bridge (this file is a plain window IIFE, but MN is
     global) — graceful overlay dismissal + one-shot micro feedback.
     Falls back to instant behavior if motion isn't up (never blocks). */
  const moClose = (node, done) => {
    const mo = window.MN && MN.get("motion");
    if (mo && mo.close) mo.close(node, done); else done();
  };
  const moPop = (node) => {
    const mo = window.MN && MN.get("motion");
    if (mo && mo.pop) mo.pop(node);
  };

  let A = null, host = null, built = false;

  /* ---------------- persisted options ---------------- */
  const OPTS_KEY = "mn.mmtool2";
  const S = Object.assign({
    workers: 12, minPx: 500, square: true,
    smartMatch: true, matchThreshold: 0.75,
    onlyMissing: false, autoApply: false,
    sources: { itunes: true, deezer: true, caa: true, lastfm: true,
               ddg: true, bing: true, google: true, yandex: false },
    lyrProviders: { lrclib: true, genius: true, azlyrics: true },
    lyrSkip: true,
    consTags: { album: true, artist: false, album_artist: true, genre: true, year: true },
  }, (() => { try { return JSON.parse(localStorage.getItem(OPTS_KEY) || "{}"); } catch (_) { return {}; } })());
  const saveOpts = () => { try { localStorage.setItem(OPTS_KEY, JSON.stringify(S)); } catch (_) {} };

  /* ---------------- run state ---------------- */
  const RUN = { stop: false, active: false };
  const DATA = {
    tracks: [], albums: [],     /* scope rows + AlbumArtInfo-alikes */
    dupGroups: [], consFolders: [], classFolders: [],
    scanned: false,
  };

  /* ---------------- bridge req/settle + rate gates ---------------- */
  const pend = {};
  function req(obj, replyType, key, timeoutMs) {
    return new Promise((resolve) => {
      const k = replyType + ":" + key;
      if (pend[k]) { clearTimeout(pend[k].t); pend[k].r(null); }
      pend[k] = { r: resolve, t: setTimeout(() => { delete pend[k]; resolve(null); }, timeoutMs || 8000) };
      A.send(obj);
    });
  }
  function settle(replyType, key, m) {
    const k = replyType + ":" + key;
    const p = pend[k];
    if (p) { clearTimeout(p.t); delete pend[k]; p.r(m); }
  }
  const hostGate = {};
  function gate(hostKey, minInterval) {
    const now = Date.now();
    const ready = Math.max(now, (hostGate[hostKey] || 0) + minInterval);
    hostGate[hostKey] = ready;
    return sleep(ready - now);
  }

  /* ---------------- fetch helpers ---------------- */
  async function jget(url, ms) {
    const c = new AbortController(); const t = setTimeout(() => c.abort(), ms || 9000);
    try { const r = await fetch(url, { signal: c.signal, credentials: "omit" }); return r.ok ? await r.json() : null; }
    catch (_) { return null; } finally { clearTimeout(t); }
  }
  async function tget(url, ms) {
    const c = new AbortController(); const t = setTimeout(() => c.abort(), ms || 9000);
    try { const r = await fetch(url, { signal: c.signal, credentials: "omit" }); return r.ok ? await r.text() : null; }
    catch (_) { return null; } finally { clearTimeout(t); }
  }
  async function iget(url, ms) {
    const c = new AbortController(); const t = setTimeout(() => c.abort(), ms || 12000);
    try {
      const r = await fetch(url, { signal: c.signal, credentials: "omit" });
      if (!r.ok) return null;
      const blob = await r.blob();
      if (!/image\//.test(blob.type) && blob.size < 4096) return null;
      const bmp = await createImageBitmap(blob).catch(() => null);
      if (!bmp) return null;
      const out = { blob, w: bmp.width, h: bmp.height, url };
      bmp.close();
      return out;
    } catch (_) { return null; } finally { clearTimeout(t); }
  }
  const blobB64 = (blob) => new Promise((res) => {
    const fr = new FileReader();
    fr.onload = () => { const s = String(fr.result); res(s.slice(s.indexOf(",") + 1)); };
    fr.onerror = () => res(null);
    fr.readAsDataURL(blob);
  });

  /* ============================================================
     SMART MATCH — perceptual hashing (port of ImageMatcher):
     aHash (8×8 mean) + dHash (9×8 gradient); similarity is the
     mean of the two hamming similarities.
     ============================================================ */
  async function hashImage(src) {
    /* src: Blob | url string */
    const img = await (async () => {
      if (typeof src === "string") { const g = await iget(src, 8000); return g ? g.blob : null; }
      return src;
    })();
    if (!img) return null;
    const bmp = await createImageBitmap(img).catch(() => null);
    if (!bmp) return null;
    const cv = document.createElement("canvas");
    cv.width = 9; cv.height = 8;
    const cx = cv.getContext("2d", { willReadFrequently: true });
    cx.drawImage(bmp, 0, 0, 9, 8);
    bmp.close();
    const d = cx.getImageData(0, 0, 9, 8).data;
    const g = [];
    for (let i = 0; i < 72; i++) g.push(0.299 * d[i * 4] + 0.587 * d[i * 4 + 1] + 0.114 * d[i * 4 + 2]);
    /* aHash over the left 8×8 */
    const cells = [];
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) cells.push(g[y * 9 + x]);
    const mean = cells.reduce((a, b) => a + b, 0) / 64;
    const aH = cells.map((v) => (v > mean ? 1 : 0));
    /* dHash: horizontal gradient across the 9×8 */
    const dH = [];
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) dH.push(g[y * 9 + x] < g[y * 9 + x + 1] ? 1 : 0);
    return { aH, dH };
  }
  function hashSim(h1, h2) {
    if (!h1 || !h2) return 0;
    const ham = (a, b) => { let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++; return 1 - n / a.length; };
    return (ham(h1.aH, h2.aH) + ham(h1.dH, h2.dH)) / 2;
  }

  /* ============================================================
     ART SOURCES (candidate URL producers, original resolution)
     ============================================================ */
  const SRC = {
    async itunes(artist, album) {
      await gate("itunes", 350);
      const j = await jget("https://itunes.apple.com/search?term=" + Q(artist + " " + album) + "&entity=album&limit=4");
      const out = [];
      (j && j.results || []).forEach((r) => {
        const a = r.artworkUrl100; if (!a) return;
        out.push(a.replace("100x100bb", "3000x3000bb"));
        out.push(a.replace("100x100bb", "1200x1200bb"));
      });
      return out;
    },
    async deezer(artist, album) {
      await gate("deezer", 350);
      const j = await jget("https://api.deezer.com/search/album?q=" + Q(artist + " " + album) + "&limit=4");
      const out = [];
      (j && j.data || []).forEach((r) => {
        if (r.cover_xl) { out.push(r.cover_xl.replace("1000x1000", "1400x1400")); out.push(r.cover_xl); }
        else if (r.cover_big) out.push(r.cover_big);
      });
      return out;
    },
    async caa(artist, album) {
      await gate("musicbrainz", 1100);
      const j = await jget("https://musicbrainz.org/ws/2/release/?query=" +
        Q('artist:"' + artist + '" AND release:"' + album + '"') + "&fmt=json&limit=3");
      const out = [];
      (j && j.releases || []).slice(0, 2).forEach((r) => {
        out.push("https://coverartarchive.org/release/" + r.id + "/front");
        out.push("https://coverartarchive.org/release/" + r.id + "/front-1200");
      });
      return out;
    },
    async lastfm(artist, album) {
      await gate("lastfm", 400);
      const j = await jget("https://ws.audioscrobbler.com/2.0/?method=album.getinfo&api_key=b25b959554ed76058ac220b7b2e0a026&artist=" +
        Q(artist) + "&album=" + Q(album) + "&format=json");
      const imgs = (j && j.album && j.album.image) || [];
      const out = [];
      for (let i = imgs.length - 1; i >= 0; i--) {
        const u = imgs[i]["#text"];
        if (u && u.indexOf("noimage") < 0) { out.push(u.replace(/\/i\/u\/[^/]+\//, "/i/u/")); out.push(u); break; }
      }
      return out;
    },
    async ddg(artist, album) {
      await gate("ddg", 900);
      const q = artist + " " + album + " album cover";
      const seed = await tget("https://duckduckgo.com/?q=" + Q(q) + "&iax=images&ia=images");
      const vqd = seed && (seed.match(/vqd=["']?([\d-]+)/) || [])[1];
      if (!vqd) return [];
      const j = await jget("https://duckduckgo.com/i.js?l=us-en&o=json&q=" + Q(q) + "&vqd=" + vqd + "&f=,,,layout:Square");
      return (j && j.results || []).slice(0, 6)
        .sort((a, b) => (b.width * b.height) - (a.width * a.height))
        .map((r) => r.image);
    },
    async bing(artist, album) {
      await gate("bing", 900);
      const html = await tget("https://www.bing.com/images/search?q=" +
        Q(artist + " " + album + " album cover") + "&qft=+filterui:aspect-square+filterui:imagesize-large");
      if (!html) return [];
      const out = []; const re = /"murl":"(https?:[^"]+?)"/g; let m;
      while ((m = re.exec(html)) && out.length < 6)
        out.push(m[1].replace(/\\u002f/g, "/").replace(/\\\//g, "/"));
      return out;
    },
    async google(artist, album) {
      await gate("google", 1200);
      const html = await tget("https://www.google.com/search?q=" +
        Q(artist + " " + album + " album cover") + "&tbm=isch&tbs=iar:s,isz:l");
      if (!html) return [];
      const cands = []; const re = /\["(https?:\/\/[^"]+?\.(?:jpg|jpeg|png|webp)[^"]*?)",(\d{3,5}),(\d{3,5})\]/g; let m;
      while ((m = re.exec(html)) && cands.length < 40) {
        const u = m[1].replace(/\\u003d/g, "=").replace(/\\u0026/g, "&");
        if (/gstatic\.com|googleusercontent/.test(u)) continue;
        cands.push({ u, s: Math.min(+m[2], +m[3]) });
      }
      return cands.sort((a, b) => b.s - a.s).slice(0, 6).map((c) => c.u);
    },
    async yandex(artist, album) {
      await gate("yandex", 1500);
      const html = await tget("https://yandex.com/images/search?text=" +
        Q(artist + " " + album + " album cover") + "&iorient=square&isize=large");
      if (!html) return [];
      const out = []; const re = /"origUrl":"(https?:[^"]+?)"/g; let m;
      while ((m = re.exec(html)) && out.length < 5)
        out.push(m[1].replace(/\\u002F/gi, "/").replace(/\\\//g, "/"));
      return out;
    },
  };
  const SRC_LABEL = { itunes: "iTunes", deezer: "Deezer", caa: "MB/CAA", lastfm: "Last.fm",
                      ddg: "DuckDuckGo", bing: "Bing", google: "Google", yandex: "Yandex" };

  /* normalize search terms: strip "(OST)"-style prefixes and bracketed
     qualifiers that defeat store/API searches */
  function cleanQ(s) {
    return String(s || "")
      .replace(/^\s*[\(\[]\s*(ost|o\.s\.t\.?|soundtrack|score)\s*[\)\]]\s*/i, "")
      .replace(/\s*[\(\[][^)\]]*(remaster|deluxe|edition|bonus|expanded|explicit)[^)\]]*[\)\]]/ig, "")
      .replace(/\s{2,}/g, " ").trim();
  }
  async function collectArtUrls(rawArtist, rawAlbum, cap) {
    const artist = cleanQ(rawArtist), album = cleanQ(rawAlbum) || rawAlbum;
    const order = ["itunes", "deezer", "caa", "lastfm", "ddg", "bing", "google", "yandex"];
    let urls = [];
    for (const s of order) {
      if (!S.sources[s] || RUN.stop) continue;
      try { (await SRC[s](artist, album)).forEach((u) => urls.push({ url: u, src: SRC_LABEL[s] })); } catch (_) {}
      if (urls.length >= (cap || 12) && s === "lastfm") break;
    }
    const seen = new Set();
    return urls.filter((c) => { const k = c.url.split("?")[0]; if (seen.has(k)) return false; seen.add(k); return true; })
               .slice(0, cap || 12);
  }
  /* best single candidate (used by batch fetch) */
  async function findBestArt(al) {
    const urls = await collectArtUrls(al.artist, al.album, 12);
    let best = null;
    for (const c of urls) {
      if (RUN.stop) break;
      const img = await iget(c.url);
      if (!img) continue;
      const mn = Math.min(img.w, img.h), ratio = img.w / img.h;
      if (mn < S.minPx) continue;
      if (S.square && (ratio < 0.9 || ratio > 1.12)) continue;
      if (!best || mn > Math.min(best.w, best.h)) best = Object.assign(img, { src: c.src });
      if (best && Math.min(best.w, best.h) >= 1400) break;
    }
    return best;
  }
  /* several distinct candidates (Review Mode ←→ alternatives) */
  async function findAlternatives(al, max) {
    const urls = await collectArtUrls(al.artist, al.album, 16);
    const out = [];
    for (const c of urls) {
      if (out.length >= (max || 5) || RUN.stop) break;
      const img = await iget(c.url);
      if (!img) continue;
      if (Math.min(img.w, img.h) < 300) continue;
      out.push(Object.assign(img, { src: c.src }));
    }
    return out;
  }
  /* Write art and WAIT for the per-file "artwrote" ack — the old path marked
     'applied' before C ever touched the file, so locked/deleted files showed
     as applied forever. Failures now surface as status "write failed". */
  async function applyArt(al, img) {
    const b64 = await blobB64(img.blob);
    if (!b64) { al.status = "write failed"; al._applyErr = "encode failed"; return false; }
    al.status = "applying…";
    const m = await req({ cmd: "artwrite", id: al.firstId, image_b64: b64,
                          mime: img.blob.type || "image/jpeg", whole_album: true },
                        "artwrote", al.firstId, 90000);   /* whole-album = many file rewrites */
    if (m && m.ok !== false) {
      al.status = "applied";
      al.artApplied = true;
      al._applyErr = null;
      return true;
    }
    al.status = "write failed";
    al._applyErr = (m && m.error) || (m ? "write error" : "no reply (timeout)");
    return false;
  }
  /* one object URL per fetched image, created lazily and reused — repaints
     used to mint a fresh blob URL every render and leak them for the session */
  function objUrl(img) {
    return img._objUrl || (img._objUrl = URL.createObjectURL(img.blob));
  }
  /* revoke the outgoing best's URL when it is being replaced — unless the
     image also lives in the alternatives strip, which still shows it */
  function dropBest(al) {
    const old = al.best;
    if (old && old._objUrl && !(al._alts || []).includes(old)) {
      URL.revokeObjectURL(old._objUrl);
      old._objUrl = null;
    }
  }

  /* ============================================================
     LYRICS PROVIDERS (Genius + AZLyrics ports, LRCLIB native)
     ============================================================ */
  const stripHtml = (s) => { const d = document.createElement("div"); d.innerHTML = s; return d.textContent || ""; };
  const LYR = {
    async lrclib(t) {
      await gate("lrclib", 350);
      const j = await jget("https://lrclib.net/api/get?artist_name=" + Q(t.artist || "") +
        "&track_name=" + Q(t.title || "") + "&album_name=" + Q(t.album || "") +
        "&duration=" + Math.round((t.duration_ms || 0) / 1000));
      if (!j || (!j.plainLyrics && !j.syncedLyrics)) return null;
      return { text: j.plainLyrics || "", lrc: j.syncedLyrics || "", src: "LRCLIB" };
    },
    async genius(t) {
      await gate("genius", 1000);
      const j = await jget("https://genius.com/api/search/multi?q=" + Q((t.artist || "") + " " + (t.title || "")));
      let songUrl = null;
      ((j && j.response && j.response.sections) || []).some((sec) =>
        (sec.hits || []).some((h) => {
          if (h.type === "song") { songUrl = h.result && h.result.url; return true; }
          return false;
        }));
      if (!songUrl) return null;
      await gate("genius", 1000);
      const page = await tget(songUrl);
      if (!page) return null;
      const doc = new DOMParser().parseFromString(page, "text/html");
      const divs = doc.querySelectorAll('[data-lyrics-container="true"]');
      if (!divs.length) return null;
      const parts = [];
      divs.forEach((d) => {
        d.querySelectorAll("br").forEach((br) => br.replaceWith("\n"));
        parts.push(d.textContent);
      });
      const text = parts.join("\n").trim();
      return text.length > 40 ? { text, lrc: "", src: "Genius" } : null;
    },
    async azlyrics(t) {
      await gate("azlyrics", 2500);   /* azlyrics bans aggressively */
      let a = (t.artist || "").toLowerCase().replace(/[^a-z0-9]/g, "");
      const ti = (t.title || "").toLowerCase().replace(/[^a-z0-9]/g, "");
      if (a.startsWith("the")) a = a.slice(3);
      if (!a || !ti) return null;
      const page = await tget("https://www.azlyrics.com/lyrics/" + a + "/" + ti + ".html");
      if (!page) return null;
      const doc = new DOMParser().parseFromString(page, "text/html");
      for (const div of doc.querySelectorAll("div:not([class]):not([id])")) {
        const text = div.textContent.trim();
        if (text.length > 200 && text.indexOf("\n") >= 0) return { text, lrc: "", src: "AZLyrics" };
      }
      return null;
    },
  };
  async function fetchLyricsFor(t) {
    const order = ["lrclib", "genius", "azlyrics"];
    for (const p of order) {
      if (!S.lyrProviders[p] || RUN.stop) continue;
      try { const r = await LYR[p](t); if (r) return r; } catch (_) {}
    }
    return null;
  }

  /* ============================================================
     SCOPE (shared) — roots + folders checkbox tree
     ============================================================ */
  let scopeCountEl = null;
  function scopePrefixes() {
    const boxes = [...host.querySelectorAll(".mm-scope input:checked")].map((b) => b.dataset.path).filter(Boolean);
    return boxes.filter((p) => !boxes.some((q) => q !== p && p.toLowerCase().indexOf(q.toLowerCase() + "\\") === 0));
  }
  function scopeSet(state) {
    /* true=all, false=none, null=invert */
    host.querySelectorAll(".mm-scope input[type=checkbox]").forEach((b) => {
      b.checked = state === null ? !b.checked : state;
    });
    updateScopeCount();
  }
  function updateScopeCount() {
    if (!scopeCountEl) return;
    let n = 0;
    const roots = (A.getRoots ? A.getRoots() : []) || [];
    host.querySelectorAll(".mm-scope-row").forEach((r) => {
      const b = r.querySelector("input");
      const isRoot = r.style.paddingLeft === "10px";
      if (b && b.checked && isRoot) {
        const c = r.querySelector(".mm-scope-count");
        if (c) n += parseInt(c.textContent.replace(/[^0-9]/g, "")) || 0;
      }
    });
    scopeCountEl.textContent = n ? "≈" + n.toLocaleString() + " ♪" : "";
  }
  function buildScope() {
    const wrap = host.querySelector(".mm-scope");
    if (!wrap) return;
    if (RUN.active) return;             /* never rebuild mid-run */
    /* PRESERVE the user's selection across rebuilds (the roots reply used
       to race manual scoping and silently re-check everything) */
    const prev = new Map();
    wrap.querySelectorAll("input[type=checkbox]").forEach((b) => prev.set(b.dataset.path, b.checked));
    wrap.innerHTML = "";
    const roots = (A.getRoots ? A.getRoots() : []) || [];
    const folders = (A.getFolders ? A.getFolders() : []) || [];
    if (!roots.length && !folders.length) {
      wrap.appendChild(el("div", "mm-empty", "No library folders — add folders first."));
      return;
    }
    const mkRow = (path, label, depth, count) => {
      const r = el("label", "mm-scope-row");
      r.style.paddingLeft = (10 + depth * 20) + "px";
      const c = el("input"); c.type = "checkbox"; c.dataset.path = path;
      c.checked = prev.has(path) ? prev.get(path) : (depth === 0);
      r.appendChild(c);
      r.appendChild(el("span", "mm-scope-name", label));
      if (count) r.appendChild(el("span", "mm-scope-count", count.toLocaleString() + " ♪"));
      wrap.appendChild(r);
      return c;
    };
    roots.forEach((rt) => {
      const rc = mkRow(rt.path, rt.path + (rt.kind && rt.kind !== "music" ? "  [" + rt.kind + "]" : ""), 0, rt.tracks);
      const kids = [];
      folders.filter((f) => f.path.toLowerCase() !== rt.path.toLowerCase() &&
                            f.path.toLowerCase().indexOf(rt.path.toLowerCase() + "\\") === 0)
        .slice(0, 400).forEach((f) => {
          const kid = mkRow(f.path, f.path.slice(rt.path.length + 1), 1, f.track_count);
          if (!prev.has(f.path)) kid.checked = prev.has(rt.path) ? prev.get(rt.path) : true;
          kids.push(kid);
        });
      rc.addEventListener("change", () => { kids.forEach((k) => { k.checked = rc.checked; }); updateScopeCount(); });
    });
    /* the .mm-scope element persists across rebuilds — attach ONCE or every
       "roots" emit stacks another listener for the session */
    if (!wrap._mnChg) { wrap._mnChg = true; wrap.addEventListener("change", updateScopeCount); }
    updateScopeCount();
  }
  /* Cross-tab scan sharing: the scoped rows are cached under a signature of
     the selected prefixes. Any tab that scans the SAME scope reuses them
     instead of re-hitting the backend — scan once, use in all five tabs. */
  async function loadScopeTracks(onProgress, force) {
    const prefixes = scopePrefixes();
    const sig = prefixes.slice().sort().join("|");
    if (!force && DATA.scanSig === sig && DATA.tracks.length) {
      if (onProgress) onProgress(DATA.tracks.length, 1);
      return DATA.tracks;
    }
    const rows = [];
    let overallTotal = 0, totals = {};
    /* prime totals for a real progress fraction. The reply settles under
       prefix+":"+offset (echoed by C), so the probe MUST use the real key
       p+":0" — a suffixed key never settles and stalls 10 s per root. Safe:
       the paging loop registers p+":0" only after this resolved. */
    for (const p of prefixes) {
      const m0 = await req({ cmd: "mmtracks", prefix: p, offset: 0, count: 1 }, "mmtracks", p + ":0", 10000);
      totals[p] = m0 ? (m0.total || 0) : 0;
      overallTotal += totals[p];
    }
    let complete = true;                /* becomes false on Stop or a lost page */
    for (const p of prefixes) {
      let off = 0, total = totals[p] || 1;
      while (off < total && !RUN.stop) {
        const m = await req({ cmd: "mmtracks", prefix: p, offset: off, count: 200 },
                            "mmtracks", p + ":" + off, 10000);
        if (!m) { complete = false; break; }
        total = m.total || 0;
        (m.rows || []).forEach((r) => rows.push(r));
        off += (m.rows || []).length || 200;
        if (!(m.rows || []).length) { if (off < total) complete = false; break; }
        if (onProgress) onProgress(rows.length, overallTotal ? rows.length / overallTotal : null);
      }
    }
    if (RUN.stop) complete = false;
    DATA.tracks = rows;
    /* Only a COMPLETE scan may become the cross-tab cache. A stopped or
       gappy scan still returns its partial rows for THIS run, but clears the
       signature so every later scan re-fetches instead of silently reusing
       an incomplete library subset (wrong dup groups / cons folders). */
    if (complete) { DATA.scanSig = sig; DATA.scanned = true; }
    else DATA.scanSig = null;
    return rows;
  }
  function groupAlbums(rows) {
    const map = new Map();
    rows.forEach((r) => {
      if (!r.album) return;
      const k = ((r.artist || "") + "" + r.album).toLowerCase();
      let g = map.get(k);
      if (!g) {
        g = { artist: r.artist || "", album: r.album, art: r.art, firstId: r.id,
              tracks: [], curW: 0, curH: 0, status: "", match: -1,
              best: null, checked: true };
        map.set(k, g);
      }
      g.tracks.push(r);
    });
    return [...map.values()];
  }

  /* ============================================================
     UI SCAFFOLD — tab bar + shared scope rail
     ============================================================ */
  /* monochrome glyphs only — color emoji fight the token-driven dark theme
     (the playful large emoji stay in the empty-state illustrations) */
  const TABS = [
    { id: "art",  label: "▣ Album Art" },
    { id: "lyr",  label: "♪ Lyrics" },
    { id: "cons", label: "✎ Consistency" },
    { id: "dup",  label: "⧉ Duplicates" },
    { id: "cls",  label: "◈ Classifier" },
  ];
  let activeTab = "art";
  const tabHosts = {};

  function build() {
    if (built) { buildScope(); return; }
    built = true;
    host.innerHTML = "";

    const tabs = el("div", "mm-tabs");
    TABS.forEach((t) => {
      const b = el("button", "mm-tab" + (t.id === activeTab ? " on" : ""), t.label);
      b._tabId = t.id;
      b.addEventListener("click", () => {
        activeTab = t.id;
        host.querySelectorAll(".mm-tab").forEach((x) => x.classList.toggle("on", x === b));
        Object.keys(tabHosts).forEach((k) => { tabHosts[k].hidden = k !== t.id; });
      });
      tabs.appendChild(b);
    });
    host.appendChild(tabs);

    const cols = el("div", "mm-cols");
    const rail = el("div", "mm-rail");
    const shead = el("div", "mm-scope-head");
    shead.appendChild(el("span", "mm-lbl", "Scope"));
    shead.appendChild(btn("All", "mm-mini", () => scopeSet(true)));
    shead.appendChild(btn("None", "mm-mini", () => scopeSet(false)));
    shead.appendChild(btn("Invert", "mm-mini", () => scopeSet(null)));
    const scount = el("span", "mm-scope-total");
    shead.appendChild(scount);
    rail.appendChild(shead);
    rail.appendChild(el("div", "mm-scope"));
    cols.appendChild(rail);
    scopeCountEl = scount;
    const main = el("div", "mm-main");
    TABS.forEach((t) => {
      const p = el("div", "mm-pane");
      p.hidden = t.id !== activeTab;
      tabHosts[t.id] = p;
      main.appendChild(p);
    });
    cols.appendChild(main);
    host.appendChild(cols);

    buildScope();
    buildArtTab(tabHosts.art);
    buildLyricsTab(tabHosts.lyr);
    buildConsTab(tabHosts.cons);
    buildDupTab(tabHosts.dup);
    buildClsTab(tabHosts.cls);
  }

  /* small widget helpers */
  function btn(label, cls, fn) { const b = el("button", "mm-btn" + (cls ? " " + cls : ""), label); b.addEventListener("click", fn); return b; }
  function chk(label, get, set) {
    const w = el("label", "mm-opt");
    const c = el("input"); c.type = "checkbox"; c.checked = !!get();
    /* pop fires HERE (a real user toggle-on) — a CSS :checked animation
       replayed on every already-checked box each time an MM tab's [hidden]
       flip restarted descendant animations */
    c.addEventListener("change", () => { set(c.checked); saveOpts(); if (c.checked) moPop(c); });
    w.appendChild(c); w.appendChild(el("span", null, label));
    return w;
  }
  /* status line + inline progress bar. Returns log(text[, frac]) — pass a
     0..1 fraction to drive the bar (with an ETA once it has moved). */
  function statusLine(pane) {
    const wrap = el("div", "mm-statuswrap");
    const s = el("div", "mm-log", "Ready.");
    const bar = el("div", "mm-progress");
    const fill = el("div", "mm-progress-fill");
    bar.appendChild(fill);
    bar.hidden = true;
    wrap.appendChild(s); wrap.appendChild(bar);
    pane.appendChild(wrap);
    let t0 = 0;
    return (text, frac) => {
      s.textContent = text;
      if (frac == null) { bar.hidden = true; t0 = 0; return; }
      bar.hidden = false;
      if (!t0) t0 = Date.now();
      const pct = clamp(frac, 0, 1);
      fill.style.transform = "scaleX(" + pct.toFixed(4) + ")";   /* compositor-only */
      if (pct > 0.02 && pct < 1) {
        const elapsed = (Date.now() - t0) / 1000;
        const eta = elapsed / pct - elapsed;
        s.textContent = text + "  ·  ~" + (eta > 90 ? Math.round(eta / 60) + "m" : Math.round(eta) + "s") + " left";
      }
      if (pct >= 1) setTimeout(() => { bar.hidden = true; t0 = 0; }, 800);
    };
  }
  /* mark a tab busy so cross-tab runs are visible */
  function setTabBusy(tabId, busy) {
    const b = [...host.querySelectorAll(".mm-tab")].find((x) => x._tabId === tabId);
    if (b) b.classList.toggle("busy", !!busy);
  }

  /* sortable table factory (port of the reference's column sorting) */
  function mkTable(pane, columns) {
    const wrap = el("div", "mm-tablewrap");
    const table = el("table", "mm-table");
    const thead = el("thead"); const trh = el("tr");
    columns.forEach((c, i) => {
      const th = el("th", c.cls || "", c.label);
      if (c.sort) {
        th.classList.add("sortable");
        th.addEventListener("click", () => {
          table._sortKey = (table._sortKey === i) ? -i - 1 : i;   /* toggle dir */
          table._resort && table._resort();
        });
      }
      trh.appendChild(th);
    });
    thead.appendChild(trh);
    table.appendChild(thead);
    const tbody = el("tbody");
    table.appendChild(tbody);
    wrap.appendChild(table);
    pane.appendChild(wrap);
    return { table, tbody, columns };
  }

  /* ---- shared confirm dialog (destructive actions get an explicit, calm
     summary: title, detail lines, optional path list, danger styling) ---- */
  function mmConfirm(o) {
    return new Promise((res) => {
      const ov = el("div", "mm-review");
      const card = el("div", "mm-review-card mm-confirm");
      card.appendChild(el("div", "mm-review-title", o.title));
      (o.lines || []).forEach((ln) => {
        const isWarn = typeof ln === "string" && ln.charAt(0) === "⚠";
        card.appendChild(el("div", "mm-confirm-line" + (isWarn ? " warn" : ""), ln));
      });
      if (o.list && o.list.length) {
        const box = el("div", "mm-confirm-list");
        o.list.slice(0, 10).forEach((p) => box.appendChild(el("div", "mm-confirm-item", p)));
        if (o.list.length > 10)
          box.appendChild(el("div", "mm-confirm-item dim", "… and " + (o.list.length - 10) + " more"));
        card.appendChild(box);
      }
      const acts = el("div", "mm-bar mm-confirm-acts");
      const done = (v) => { document.removeEventListener("keydown", key, true); moClose(ov, () => ov.remove()); res(v); };
      const okB = btn(o.confirm || "Confirm", o.danger ? "mm-dangerfill" : "mm-primary", () => done(true));
      const noB = btn("Cancel", "", () => done(false));
      const key = (ev) => {
        if (ev.key === "Escape") { ev.preventDefault(); ev.stopPropagation(); done(false); }
        else if (ev.key === "Enter") {
          ev.preventDefault(); ev.stopPropagation();
          /* danger dialogs never confirm on a stray Enter — destruction takes
             a deliberate click (or Space on the focused button) */
          if (!o.danger) done(true);
        } else if (ev.key === "Tab") {
          /* minimal focus trap: cycle the two buttons, never the page behind */
          ev.preventDefault(); ev.stopPropagation();
          (document.activeElement === noB ? okB : noB).focus();
        }
      };
      if (o.danger) okB.title = "Enter is disabled here — click (or press Space) to confirm";
      acts.appendChild(okB);
      acts.appendChild(noB);
      card.appendChild(acts);
      ov.appendChild(card);
      ov.addEventListener("click", (e) => { if (e.target === ov) done(false); });
      document.addEventListener("keydown", key, true);
      document.body.appendChild(ov);
      noB.focus();   /* safe default focused on open */
    });
  }

  /* ---- shared empty state (icon + headline + hint) ---- */
  function mmEmpty(box, icon, title, sub) {
    const w = el("div", "mm-emptystate");
    w.appendChild(el("div", "mm-emptystate-ico", icon));
    w.appendChild(el("div", "mm-emptystate-t", title));
    if (sub) w.appendChild(el("div", "mm-emptystate-s", sub));
    box.appendChild(w);
    return w;
  }
  const fmtMB = (bytes) => (bytes / 1048576).toFixed(1) + " MB";

  /* ============================================================
     TAB 1 — ALBUM ART
     ============================================================ */
  let artUI = null;
  function buildArtTab(pane) {
    const bar1 = el("div", "mm-bar");
    bar1.appendChild(btn("⟳ Scan Albums", "mm-primary", artScan));
    bar1.appendChild(btn("⬇ Fetch Checked", "mm-primary", artFetchBatch));
    bar1.appendChild(btn("■ Stop", "", () => { RUN.stop = true; }));
    bar1.appendChild(btn("✔ Apply Matched", "", artApplyMatched));
    bar1.appendChild(btn("◉ Review Mode", "", artReview));
    const wl = el("label", "mm-opt", "workers ");
    const wi = el("input"); wi.type = "range"; wi.min = 1; wi.max = 32; wi.value = S.workers;
    const wv = el("span", "mm-wv", String(S.workers));
    wi.addEventListener("input", () => { S.workers = +wi.value; wv.textContent = wi.value; saveOpts(); });
    wl.appendChild(wi); wl.appendChild(wv);
    bar1.appendChild(wl);
    const ml = el("label", "mm-opt", "min px ");
    const mi = el("input"); mi.type = "number"; mi.min = 200; mi.max = 3000; mi.step = 100; mi.value = S.minPx; mi.className = "mm-num";
    mi.addEventListener("change", () => { S.minPx = clamp(+mi.value || 500, 200, 3000); saveOpts(); });
    ml.appendChild(mi);
    bar1.appendChild(ml);
    bar1.appendChild(chk("smart match", () => S.smartMatch, (v) => { S.smartMatch = v; }));
    bar1.appendChild(chk("square only", () => S.square, (v) => { S.square = v; }));
    bar1.appendChild(chk("only missing", () => S.onlyMissing, (v) => { S.onlyMissing = v; }));
    bar1.appendChild(chk("auto-apply", () => S.autoApply, (v) => { S.autoApply = v; }));
    pane.appendChild(bar1);

    const bar2 = el("div", "mm-bar");
    bar2.appendChild(el("span", "mm-lbl", "Sources"));
    Object.keys(SRC_LABEL).forEach((k) => {
      bar2.appendChild(chk(SRC_LABEL[k], () => S.sources[k], (v) => { S.sources[k] = v; }));
    });
    bar2.appendChild(el("span", "mm-lbl", "Select"));
    bar2.appendChild(btn("All", "mm-mini", () => artSelect("all")));
    bar2.appendChild(btn("None", "mm-mini", () => artSelect("none")));
    bar2.appendChild(btn("Missing", "mm-mini", () => artSelect("missing")));
    bar2.appendChild(btn("Small", "mm-mini", () => artSelect("small")));
    const filt = el("input", "mm-filter");
    filt.placeholder = "filter…";
    filt.addEventListener("input", () => artRender(filt.value.trim().toLowerCase()));
    bar2.appendChild(filt);
    pane.appendChild(bar2);

    const log = statusLine(pane);
    const T = mkTable(pane, [
      { label: "✓" }, { label: "Artist", sort: true }, { label: "Album", sort: true },
      { label: "♪", sort: true }, { label: "Art", sort: true }, { label: "New", sort: true },
      { label: "Match", sort: true }, { label: "Source" }, { label: "Status", sort: true },
    ]);
    /* ---- RESIZABLE bottom dock: per-album PREVIEW + film strip ---- */
    const dock = el("div", "mm-dock");
    const handle = el("div", "mm-dock-handle");
    handle.title = "Drag to resize the preview";
    dock.appendChild(handle);
    const dockBody = el("div", "mm-dock-body");
    const preview = el("div", "mm-preview");
    preview.appendChild(el("div", "mm-empty", "Select an album row to preview and pick art."));
    dockBody.appendChild(preview);
    const strip = el("div", "mm-strip");
    dockBody.appendChild(strip);
    dock.appendChild(dockBody);
    pane.appendChild(dock);
    const setDockH = (h) => {
      S.dockH = clamp(h | 0, 150, 560);
      dock.style.height = S.dockH + "px";
      dock.style.setProperty("--mm-thumb", Math.max(46, S.dockH - 96) + "px");
      dock.style.setProperty("--mm-big", Math.max(90, S.dockH - 116) + "px");
    };
    setDockH(S.dockH || 260);
    handle.addEventListener("pointerdown", (ev) => {
      ev.preventDefault();
      handle.setPointerCapture(ev.pointerId);
      const y0 = ev.clientY, h0 = S.dockH;
      const move = (e) => setDockH(h0 + (y0 - e.clientY));
      const up = () => {
        handle.removeEventListener("pointermove", move);
        handle.removeEventListener("pointerup", up);
        saveOpts();
      };
      handle.addEventListener("pointermove", move);
      handle.addEventListener("pointerup", up);
    });
    artUI = { log, T, strip, preview, dock, filter: "", sel: null };
    T.table._resort = () => artRender(artUI.filter);
  }

  /* ---- per-album preview + candidate picker (the ergonomic core) ---- */
  function selectAlbum(al) {
    artUI.sel = al;
    artRender(artUI.filter);
    renderPreview(al);
  }
  function renderPreview(al) {
    const p = artUI.preview;
    p.innerHTML = "";
    if (!al) { p.appendChild(el("div", "mm-empty", "Select an album row.")); return; }
    const head = el("div", "mm-prev-head");
    head.appendChild(el("div", "mm-prev-title", (al.artist ? al.artist + " — " : "") + al.album));
    const acts = el("div", "mm-prev-acts");
    acts.appendChild(btn("⬇ Fetch", "mm-mini", async () => {
      al.status = "fetching…"; renderPreview(al); artRender(artUI.filter);
      RUN.stop = false;
      const best = await findBestArt(al);
      if (best) {
        dropBest(al);
        al.best = best;
        if (S.smartMatch && al.art) {
          const [h1, h2] = await Promise.all([hashImage(al.art), hashImage(best.blob)]);
          al.match = hashSim(h1, h2);
          al.status = al.match >= S.matchThreshold ? "ready" : "mismatch";
        } else { al.match = -1; al.status = "ready"; }
      } else al.status = "no art";
      renderPreview(al); artRender(artUI.filter);
    }));
    acts.appendChild(btn("⊞ Alternatives", "mm-mini", () => loadAlternatives(al)));
    const applyB = btn("✔ Apply", "mm-mini mm-primary", async () => {
      if (!al.best) return;
      await applyArt(al, al.best);
      renderPreview(al); artRender(artUI.filter);
    });
    applyB.disabled = !al.best || !!al.artApplied;
    acts.appendChild(applyB);
    head.appendChild(acts);
    p.appendChild(head);

    const pair = el("div", "mm-prev-pair");
    const side = (label, url, cap, accent) => {
      const s = el("div", "mm-prev-side");
      const img = el("img", "mm-prev-img" + (accent ? " new" : ""));
      if (url) img.src = url; else img.classList.add("none");
      s.appendChild(img);
      s.appendChild(el("div", "mm-prev-cap" + (accent ? " accent" : ""), cap));
      return s;
    };
    pair.appendChild(side("orig", al.art,
      al.curW ? "ORIGINAL · " + al.curW + "×" + al.curH : (al.art ? "ORIGINAL" : "no art")));
    pair.appendChild(side("new", al.best ? objUrl(al.best) : null,
      al.best ? (al.artApplied ? "APPLIED · " : "NEW · ") + al.best.w + "×" + al.best.h + " · " + al.best.src
              : "nothing fetched yet", true));
    if (al.match >= 0)
      pair.appendChild(el("div", "mm-prev-match" + (al.match >= S.matchThreshold ? " good" : " warn"),
        Math.round(al.match * 100) + "%"));
    p.appendChild(pair);

    /* candidate picker strip (filled by Alternatives / previous loads) */
    const cands = el("div", "mm-cands");
    if (al._alts && al._alts.length) {
      al._alts.forEach((c) => {
        const t = el("img", "mm-cand" + (al.best === c ? " on" : ""));
        t.src = objUrl(c);
        t.title = c.w + "×" + c.h + " · " + c.src + " — click to select, double-click to apply";
        t.addEventListener("click", () => { dropBest(al); al.best = c; al.status = "ready"; renderPreview(al); artRender(artUI.filter); });
        t.addEventListener("dblclick", async () => { dropBest(al); al.best = c; await applyArt(al, c); renderPreview(al); artRender(artUI.filter); });
        cands.appendChild(t);
      });
    } else if (al._altsLoading) {
      cands.appendChild(el("div", "mm-empty", "searching all sources…"));
    }
    p.appendChild(cands);
  }
  async function loadAlternatives(al) {
    if (al._altsLoading) return;
    al._altsLoading = true;
    renderPreview(al);
    RUN.stop = false;
    const alts = await findAlternatives(al, 8);
    if (al.best && !alts.includes(al.best)) alts.unshift(al.best);
    /* revoke object URLs (and drop blob refs) of OUTGOING alternatives that
       are not carried into the new strip and are not the album's best — the
       replaced array used to keep its URLs+blobs alive for the session */
    (al._alts || []).forEach((c) => {
      if (c !== al.best && !alts.includes(c) && c._objUrl) {
        URL.revokeObjectURL(c._objUrl); c._objUrl = null; c.blob = null;
      }
    });
    al._alts = alts;
    al._altsLoading = false;
    if (artUI.sel === al) renderPreview(al);
  }
  function artSortVal(al, key) {
    switch (Math.abs(key < 0 ? -key - 1 : key)) {
      case 1: return (al.artist || "").toLowerCase();
      case 2: return (al.album || "").toLowerCase();
      case 3: return al.tracks.length;
      case 4: return Math.min(al.curW || 0, al.curH || 0);
      case 5: return al.best ? Math.min(al.best.w, al.best.h) : -1;
      case 6: return al.match;
      case 8: return al.status || "";
      default: return 0;
    }
  }
  function artRender(filter) {
    artUI.filter = filter || "";
    const key = artUI.T.table._sortKey;
    let list = DATA.albums.slice();
    if (artUI.filter)
      list = list.filter((al) => (al.artist + " " + al.album).toLowerCase().includes(artUI.filter));
    if (key != null) {
      const dir = key < 0 ? -1 : 1;
      list.sort((a, b) => { const va = artSortVal(a, key), vb = artSortVal(b, key);
        return va < vb ? -dir : va > vb ? dir : 0; });
    }
    const tb = artUI.T.tbody;
    tb.innerHTML = "";
    if (!list.length) {
      const tr = el("tr");
      const td = el("td", "mm-emptycell");
      td.colSpan = 9;
      mmEmpty(td, "🖼", DATA.scanned ? (artUI.filter ? "No albums match the filter" : "No albums in this scope")
                                     : "Scan to list your albums",
        DATA.scanned ? "" : "Pick a scope on the left, then Scan Albums — current covers get measured, and batch fetch pulls originals from every enabled source.");
      tr.appendChild(td);
      tb.appendChild(tr);
    }
    list.forEach((al) => {
      const tr = el("tr", "mm-tr " + (al.status === "applied" ? "ok" : al.status === "ready" ? "ready" :
        al.status === "no art" || al.status === "write failed" ? "bad" : al.status === "mismatch" ? "warn" : "") +
        (artUI.sel === al ? " sel" : ""));
      const c0 = el("td");
      const cb = el("input"); cb.type = "checkbox"; cb.checked = al.checked;
      cb.addEventListener("change", () => { al.checked = cb.checked; });
      c0.appendChild(cb); tr.appendChild(c0);
      tr.appendChild(el("td", "", al.artist || "—"));
      tr.appendChild(el("td", "", al.album));
      tr.appendChild(el("td", "num", String(al.tracks.length)));
      tr.appendChild(el("td", "num", al.curW ? al.curW + "×" + al.curH : (al.art ? "…" : "none")));
      tr.appendChild(el("td", "num accent", al.best ? al.best.w + "×" + al.best.h : ""));
      tr.appendChild(el("td", "num", al.match >= 0 ? Math.round(al.match * 100) + "%" : ""));
      tr.appendChild(el("td", "", al.best ? al.best.src : ""));
      const st = el("td", "", al.status || "");
      if (al._applyErr) st.title = al._applyErr;
      tr.appendChild(st);
      tr.addEventListener("click", (ev) => {
        if (ev.target.tagName === "INPUT") return;
        selectAlbum(al);
      });
      tr.addEventListener("dblclick", () => reviewOne(al));
      tb.appendChild(tr);
      al._tr = tr;
    });
    /* during a batch run the worker owns the status line — don't race it */
    if (!RUN.active) artUI.log(list.length + " albums" + (artUI.filter ? " (filtered)" : ""));
  }
  function artSelect(mode) {
    DATA.albums.forEach((al) => {
      if (mode === "all") al.checked = true;
      else if (mode === "none") al.checked = false;
      else if (mode === "missing") al.checked = !al.art;
      else if (mode === "small") al.checked = (al.curW || 0) > 0 && Math.min(al.curW, al.curH) < S.minPx;
    });
    artRender(artUI.filter);
  }
  async function artScan() {
    if (RUN.active) return;
    RUN.stop = false; RUN.active = true;
    setTabBusy("art", true);
    artUI.log("scanning scope…", 0);
    await loadScopeTracks((n, f) => artUI.log("scope: " + n.toLocaleString() + " tracks…", f == null ? 0.02 : f * 0.5));
    DATA.albums = groupAlbums(DATA.tracks);
    artRender("");
    /* measure current art sizes (bounded parallel), driving the progress bar */
    let i = 0, mdone = 0, lastLog = 0;
    const withArt = DATA.albums.filter((a) => a.art);
    const meas = async () => {
      for (;;) {
        const k = i++;
        if (k >= withArt.length || RUN.stop) return;
        const al = withArt[k];
        const img = await iget(al.art, 6000);
        if (img) { al.curW = img.w; al.curH = img.h; }
        /* time-throttled (not modulo-20): small libraries used to sit at a
           frozen 50% bar until the whole measure pass ended */
        mdone++;
        const now = Date.now();
        if (now - lastLog > 150 || mdone === withArt.length) {
          lastLog = now;
          artUI.log("measuring art… " + mdone + "/" + withArt.length,
                    0.5 + 0.5 * (mdone / (withArt.length || 1)));
        }
      }
    };
    await Promise.all(Array.from({ length: 8 }, meas));
    RUN.active = false;
    setTabBusy("art", false);
    artRender(artUI.filter);
    const missing = DATA.albums.filter((a) => !a.art).length;
    const small = DATA.albums.filter((a) => a.curW && Math.min(a.curW, a.curH) < S.minPx).length;
    artUI.log(DATA.albums.length + " albums · " + missing + " missing art · " + small + " below " + S.minPx + "px");
  }
  /* film strip frame */
  function stripFrame(al) {
    const f = el("div", "mm-frame");
    f.addEventListener("click", () => selectAlbum(al));
    f.appendChild(el("div", "mm-frame-t", (al.artist ? al.artist + " — " : "") + al.album));
    const pair = el("div", "mm-frame-pair");
    const o = el("img", "mm-frame-img");
    if (al.art) o.src = al.art; else o.classList.add("none");
    pair.appendChild(o);
    const n = el("img", "mm-frame-img new");
    if (al.best) n.src = objUrl(al.best);
    pair.appendChild(n);
    f.appendChild(pair);
    f.appendChild(el("div", "mm-frame-s",
      al.status === "applied" ? "✓ applied"
      : al.best ? (al.match >= 0 ? Math.round(al.match * 100) + "% · " : "") + al.best.w + "×" + al.best.h
      : al.status || "…"));
    return f;
  }
  async function artFetchBatch() {
    if (RUN.active) return;
    if (!DATA.albums.length) await artScan();
    RUN.stop = false; RUN.active = true;
    artUI.strip.innerHTML = "";
    const todo = DATA.albums.filter((al) => al.checked &&
      !(S.onlyMissing && al.curW > 0));
    let idx = 0, done = 0, found = 0, mismatched = 0;
    const worker = async () => {
      for (;;) {
        if (RUN.stop) return;
        const i = idx++;
        if (i >= todo.length) return;
        const al = todo[i];
        al.status = "fetching…"; artPaint(al);
        const best = await findBestArt(al);
        if (RUN.stop && !best) { al.status = "stopped"; artPaint(al); continue; }
        if (!best) { al.status = "no art"; artPaint(al); }
        else {
          dropBest(al);
          al.best = best;
          /* smart match vs current art */
          if (S.smartMatch && al.art) {
            const [h1, h2] = await Promise.all([hashImage(al.art), hashImage(best.blob)]);
            al.match = hashSim(h1, h2);
            al.status = al.match >= S.matchThreshold ? "ready" : "mismatch";
            if (al.status === "mismatch") mismatched++;
          } else { al.match = -1; al.status = "ready"; }
          found++;
          if (S.autoApply && al.status === "ready") await applyArt(al, best);
        }
        artPaint(al);
        artUI.strip.appendChild(stripFrame(al));
        artUI.strip.scrollLeft = artUI.strip.scrollWidth;
        if (artUI.sel === al) renderPreview(al);
        done++;
        artUI.log(done + "/" + todo.length + " · " + found + " found · " + mismatched + " mismatched", done / (todo.length || 1));
      }
    };
    setTabBusy("art", true);
    await Promise.all(Array.from({ length: clamp(S.workers, 1, 32) }, worker));
    RUN.active = false;
    setTabBusy("art", false);
    artUI.log(done + "/" + todo.length + " done · " + found + " found · " + mismatched + " need review" + (RUN.stop ? " (stopped)" : " ✓"));
  }
  /* Patch the ONE row in place — a full artRender per status flip was O(n²)
     DOM work over a batch run (innerHTML wipe × workers × albums). */
  function artPaint(al) {
    const tr = al._tr;
    if (!tr || !tr.isConnected) return;   /* filtered out / not rendered yet */
    tr.className = "mm-tr " + (al.status === "applied" ? "ok" : al.status === "ready" ? "ready" :
      al.status === "no art" || al.status === "write failed" ? "bad" : al.status === "mismatch" ? "warn" : "") +
      (artUI.sel === al ? " sel" : "");
    const td = tr.children;
    td[4].textContent = al.curW ? al.curW + "×" + al.curH : (al.art ? "…" : "none");
    td[5].textContent = al.best ? al.best.w + "×" + al.best.h : "";
    td[6].textContent = al.match >= 0 ? Math.round(al.match * 100) + "%" : "";
    td[7].textContent = al.best ? al.best.src : "";
    td[8].textContent = al.status || "";
    td[8].title = al._applyErr || "";
  }
  /* Bulk apply, batched like tagWriteBatch: confirm first, bounded workers,
     per-album "artwrote" ack, progress fraction and a failed tally — the old
     loop fire-and-forgot one multi-MB artwrite per album with no guard. */
  async function artApplyMatched() {
    if (RUN.active) return;
    const list = DATA.albums.filter((al) => al.best && !al.artApplied &&
      (al.status === "ready"));
    if (!list.length) { artUI.log("nothing matched to apply — fetch first (only “ready” rows are applied)"); return; }
    const ok = await mmConfirm({
      title: "Apply " + list.length + " cover" + (list.length === 1 ? "" : "s") + "?",
      lines: [
        "Each fetched cover is embedded into every track of its album (whole-album write).",
        "Files are modified in place; rows that fail (locked or missing files) are flagged “write failed” and can be retried.",
      ],
      list: list.map((al) => (al.artist ? al.artist + " — " : "") + al.album),
      confirm: "✔ Apply " + list.length,
    });
    if (!ok) return;
    RUN.stop = false; RUN.active = true;
    setTabBusy("art", true);
    let i = 0, done = 0, applied = 0, failed = 0;
    const worker = async () => {
      for (;;) {
        if (RUN.stop) return;
        const k = i++;
        if (k >= list.length) return;
        const al = list[k];
        if (await applyArt(al, al.best)) applied++; else failed++;
        artPaint(al);
        if (artUI.sel === al) renderPreview(al);
        done++;
        artUI.log("applying " + done + "/" + list.length + " · ✓ " + applied +
                  (failed ? " · ✗ " + failed : ""), done / list.length);
      }
    };
    await Promise.all(Array.from({ length: 3 }, worker));
    RUN.active = false;
    setTabBusy("art", false);
    artRender(artUI.filter);
    artUI.log("applied " + applied + " cover" + (applied === 1 ? "" : "s") +
              (failed ? " · ⚠ " + failed + " write failed (see Status column)" : "") +
              (RUN.stop ? " (stopped)" : " ✓"));
  }

  /* ---- Review Mode (keyboard: ↑ accept · ↓ deny · ←→ alternatives · Esc) ---- */
  function artReview() {
    const queue = DATA.albums.filter((al) => al.best && !al.artApplied);
    if (!queue.length) { artUI.log("nothing to review — fetch first"); return; }
    let i = 0, accepted = 0, denied = 0;
    let alts = null, altIdx = 0;
    const ov = el("div", "mm-review");
    const card = el("div", "mm-review-card");
    ov.appendChild(card);
    document.body.appendChild(ov);
    const render = () => {
      const al = queue[i];
      card.innerHTML = "";
      card.appendChild(el("div", "mm-review-keys", "⬆ Accept   ⬇ Deny   ← → Alternatives   Esc Close"));
      card.appendChild(el("div", "mm-review-title", (al.artist ? al.artist + " — " : "") + al.album));
      const pair = el("div", "mm-review-pair");
      const o = el("div", "mm-review-side");
      const oi = el("img"); if (al.art) oi.src = al.art;
      o.appendChild(oi);
      o.appendChild(el("div", "mm-review-cap", al.curW ? "ORIGINAL · " + al.curW + "×" + al.curH : "no art"));
      pair.appendChild(o);
      const cand = (alts && alts[altIdx]) || al.best;
      const n = el("div", "mm-review-side");
      const ni = el("img"); ni.src = objUrl(cand);
      n.appendChild(ni);
      n.appendChild(el("div", "mm-review-cap accent",
        "NEW · " + cand.w + "×" + cand.h + " · " + cand.src +
        (alts ? "  (" + (altIdx + 1) + "/" + alts.length + ")" : "")));
      pair.appendChild(n);
      card.appendChild(pair);
      if (al.match >= 0)
        card.appendChild(el("div", "mm-review-match" + (al.match >= S.matchThreshold ? " good" : " warn"),
          (al.match >= S.matchThreshold ? "✓ MATCH " : "⚠ DIFFERENT ") + Math.round(al.match * 100) + "%"));
      card.appendChild(el("div", "mm-review-tally", "✓ " + accepted + "   ✗ " + denied + "   " + (i + 1) + "/" + queue.length));
    };
    /* Revoke object URLs minted for VIEWED review alternatives that are not
       kept anywhere (not the album's best, not in its picker strip) — the
       per-candidate URLs used to leak for the session. */
    const dropAlts = () => {
      const al = queue[i] || {};
      (alts || []).forEach((c) => {
        if (c !== al.best && !(al._alts || []).includes(c) && c._objUrl) {
          URL.revokeObjectURL(c._objUrl); c._objUrl = null; c.blob = null;
        }
      });
      alts = null; altIdx = 0;
    };
    const next = () => {
      dropAlts();
      if (++i >= queue.length) { close(); artUI.log("review done — ✓ " + accepted + " · ✗ " + denied); }
      else render();
    };
    /* busy = an apply (or alt search) is awaiting its ack. Without it a
       second ArrowUp re-fired applyArt on the SAME album (req() cancels the
       first pend → miscounted as failed) and next() ran twice, silently
       skipping an album. All action keys are ignored while busy; Esc stays
       live so the user is never trapped. */
    let busy = false;
    const key = async (ev) => {
      const al = queue[i];
      /* This overlay owns the arrow/Escape keys while open. It's a CAPTURE
         listener, so stopPropagation here fully shields the app's global
         keydown (which otherwise ALSO seeks/volumes on the same arrows). */
      const handled = ev.key === "Escape" || ev.key === "ArrowUp" ||
        ev.key === "ArrowDown" || ev.key === "ArrowRight" || ev.key === "ArrowLeft";
      if (handled) { ev.preventDefault(); ev.stopPropagation(); }
      if (ev.key === "Escape") { close(); return; }
      if (busy || !handled) return;
      if (ev.key === "ArrowUp") {
        const cand = (alts && alts[altIdx]) || al.best;
        /* the accepted candidate becomes the album's best so the table/strip
           show what was actually applied (and dropAlts never revokes it) */
        if (cand !== al.best) { dropBest(al); al.best = cand; }
        busy = true;
        card.appendChild(el("div", "mm-review-cap accent", "⏳ applying…"));
        const okA = await applyArt(al, cand);
        busy = false;
        if (okA) accepted++;
        artRender(artUI.filter); next();
      }
      else if (ev.key === "ArrowDown") { denied++; next(); }
      else if (ev.key === "ArrowRight" || ev.key === "ArrowLeft") {
        if (!alts) {
          busy = true;
          card.appendChild(el("div", "mm-review-cap", "searching alternatives…"));
          const found = await findAlternatives(al, 5);
          busy = false;
          alts = found;
          if (al.best) alts.unshift(al.best);
          altIdx = 0;
        } else altIdx = (altIdx + (ev.key === "ArrowRight" ? 1 : alts.length - 1)) % alts.length;
        render();
      }
    };
    const close = () => { dropAlts(); document.removeEventListener("keydown", key, true); moClose(ov, () => ov.remove()); };
    document.addEventListener("keydown", key, true);
    ov.addEventListener("click", (e) => { if (e.target === ov) close(); });
    render();
  }
  function reviewOne(al) {
    if (!al.best) return;
    const keep = DATA.albums;
    DATA.albums = [al];
    try { artReview(); } finally { DATA.albums = keep; }
  }

  /* ============================================================
     TAB 2 — LYRICS
     ============================================================ */
  let lyrUI = null;
  function buildLyricsTab(pane) {
    const bar = el("div", "mm-bar");
    bar.appendChild(btn("⟳ Scan Tracks", "mm-primary", lyrScan));
    bar.appendChild(btn("⬇ Fetch Checked", "mm-primary", lyrFetch));
    bar.appendChild(btn("■ Stop", "", () => { RUN.stop = true; }));
    bar.appendChild(chk("skip existing", () => S.lyrSkip, (v) => { S.lyrSkip = v; }));
    bar.appendChild(el("span", "mm-lbl", "Providers"));
    [["lrclib", "LRCLIB (synced)"], ["genius", "Genius"], ["azlyrics", "AZLyrics"]].forEach(([k, lbl]) =>
      bar.appendChild(chk(lbl, () => S.lyrProviders[k], (v) => { S.lyrProviders[k] = v; })));
    pane.appendChild(bar);
    const log = statusLine(pane);
    const T = mkTable(pane, [
      { label: "✓" }, { label: "Artist", sort: true }, { label: "Title", sort: true },
      { label: "Album" }, { label: "Lyrics", sort: true }, { label: "Source" },
    ]);
    lyrUI = { log, T, rows: [] };
    T.table._resort = lyrRender;
  }
  function lyrRender() {
    const tb = lyrUI.T.tbody; tb.innerHTML = "";
    if (!lyrUI.rows.length) {
      const tr = el("tr");
      const td = el("td", "mm-emptycell");
      td.colSpan = 6;
      mmEmpty(td, "🎤", "Scan to list tracks",
        "Pick a scope on the left, then Scan Tracks — existing embedded lyrics are probed so “skip existing” only fetches what's missing.");
      tr.appendChild(td);
      tb.appendChild(tr);
    }
    lyrUI.rows.forEach((t) => {
      const tr = el("tr", "mm-tr " + (t._lyrGot ? "ok" : t._lyrFail ? "bad" : ""));
      const c0 = el("td");
      const cb = el("input"); cb.type = "checkbox"; cb.checked = t._chk !== false;
      cb.addEventListener("change", () => { t._chk = cb.checked; });
      c0.appendChild(cb); tr.appendChild(c0);
      tr.appendChild(el("td", "", t.artist || "—"));
      tr.appendChild(el("td", "", t.title || "—"));
      tr.appendChild(el("td", "", t.album || ""));
      tr.appendChild(el("td", "", t._hasLyr === true ? "✓ has" : t._hasLyr === false ? "—" : "?"));
      tr.appendChild(el("td", "", t._lyrSrc || ""));
      tb.appendChild(tr);
    });
    /* during a run the worker owns the status line + progress bar — a bare
       count here would hide the bar every re-render (same guard as art) */
    if (!RUN.active) lyrUI.log(lyrUI.rows.length + " tracks");
  }
  async function lyrScan() {
    if (RUN.active) return;
    RUN.stop = false; RUN.active = true;
    lyrUI.log("scanning scope…");
    await loadScopeTracks((n) => lyrUI.log("scope: " + n + " tracks…"));
    lyrUI.rows = DATA.tracks.slice();
    lyrRender();
    /* probe existing lyrics (bounded) */
    let i = 0, probed = 0;
    const probe = async () => {
      for (;;) {
        const k = i++;
        if (k >= lyrUI.rows.length || RUN.stop) return;
        const t = lyrUI.rows[k];
        const m = await req({ cmd: "lyricsread", id: t.id }, "lyrics", t.id, 1500);
        t._hasLyr = !!(m && ((m.text && m.text.length > 40) || (m.synced_lrc && m.synced_lrc.length > 40)));
        if (++probed % 25 === 0) {
          lyrUI.log("probing lyrics " + probed + "/" + lyrUI.rows.length, probed / (lyrUI.rows.length || 1));
          lyrRender();
        }
      }
    };
    await Promise.all(Array.from({ length: 6 }, probe));
    RUN.active = false;
    lyrRender();
    lyrUI.log(lyrUI.rows.length + " tracks · " + lyrUI.rows.filter((t) => t._hasLyr).length + " already have lyrics");
  }
  async function lyrFetch() {
    if (RUN.active) return;
    if (!lyrUI.rows.length) await lyrScan();
    RUN.stop = false; RUN.active = true;
    const todo = lyrUI.rows.filter((t) => t._chk !== false && !(S.lyrSkip && t._hasLyr));
    let idx = 0, ok = 0, fail = 0;
    const t0 = Date.now();
    const worker = async () => {
      for (;;) {
        if (RUN.stop) return;
        const i = idx++;
        if (i >= todo.length) return;
        const t = todo[i];
        const rec = await fetchLyricsFor(t);
        if (rec) {
          /* wait for the per-file "lyricswrote" ack — the old fire-and-forget
             marked ✓ before C touched the file, so a locked/missing file
             showed as embedded forever (art + consistency already ack) */
          const m = await req({ cmd: "lyricswrite", id: t.id, text: rec.text || "", synced_lrc: rec.lrc || "" },
                              "lyricswrote", t.id, 30000);
          if (m && m.ok !== false) { t._lyrGot = true; t._lyrSrc = rec.src; t._hasLyr = true; ok++; }
          else { t._lyrFail = true; t._lyrSrc = rec.src + " · write failed"; fail++; }
        } else { t._lyrFail = true; fail++; }
        lyrUI.log((ok + fail) + "/" + todo.length + " · ✓ " + ok + " · ✗ " + fail, (ok + fail) / (todo.length || 1));
        if ((ok + fail) % 10 === 0) lyrRender();
      }
    };
    setTabBusy("lyr", true);
    await Promise.all(Array.from({ length: clamp(Math.min(S.workers, 6), 1, 6) }, worker));
    RUN.active = false;
    setTabBusy("lyr", false);
    lyrRender();
    lyrUI.log("done — ✓ " + ok + " · ✗ " + fail + " · " + ((Date.now() - t0) / 1000).toFixed(0) + "s" + (RUN.stop ? " (stopped)" : ""));
  }

  /* ============================================================
     TAB 3 — CONSISTENCY (per-folder tag-value maps + fix)
     ============================================================ */
  let consUI = null;
  const CONS_TAGS = [["album", "Album"], ["artist", "Artist"], ["album_artist", "Album artist"],
                     ["genre", "Genre"], ["year", "Year"]];
  function buildConsTab(pane) {
    const bar = el("div", "mm-bar");
    bar.appendChild(btn("⟳ Scan", "mm-primary", consScan));
    const fixAll = btn("⇉ Auto-fix All (majority value)", "", consAutoFixAll);
    fixAll.title = "Set every inconsistent tag to its folder's majority value — asks for confirmation first";
    bar.appendChild(fixAll);
    bar.appendChild(btn("■ Stop", "", () => { RUN.stop = true; }));
    bar.appendChild(el("span", "mm-lbl", "Tags"));
    CONS_TAGS.forEach(([k, lbl]) =>
      bar.appendChild(chk(lbl, () => S.consTags[k], (v) => { S.consTags[k] = v; })));
    pane.appendChild(bar);
    const log = statusLine(pane);
    const list = el("div", "mm-conslist");
    pane.appendChild(list);
    consUI = { log, list };
  }
  async function consScan() {
    if (RUN.active) return;
    RUN.stop = false; RUN.active = true;
    consUI.log("scanning scope…");
    await loadScopeTracks((n) => consUI.log("scope: " + n + " tracks…"));
    const tags = CONS_TAGS.map(([k]) => k).filter((k) => S.consTags[k]);
    const byFolder = new Map();
    DATA.tracks.forEach((t) => {
      const f = dirOf(t.path || "");
      if (!byFolder.has(f)) byFolder.set(f, []);
      byFolder.get(f).push(t);
    });
    /* exact port of ConsScanner.run(): per folder, per tag -> value -> files;
       folders where any chosen tag has >1 distinct value are issues */
    DATA.consFolders = [];
    for (const [folder, files] of byFolder) {
      const issues = {};
      for (const tag of tags) {
        const vals = new Map();
        files.forEach((t) => {
          const v = String(tag === "album_artist" ? (t.album_artist || "") : (t[tag] || "")).trim();
          if (!v || v === "0") return;
          if (!vals.has(v)) vals.set(v, []);
          vals.get(v).push(t);
        });
        if (vals.size > 1) issues[tag] = vals;
      }
      if (Object.keys(issues).length) DATA.consFolders.push({ folder, files, issues });
    }
    RUN.active = false;
    consRender();
    consUI.log(byFolder.size + " folders scanned · " + DATA.consFolders.length + " with inconsistencies");
  }
  function consRender() {
    consUI.list.innerHTML = "";
    if (!DATA.consFolders.length) {
      if (DATA.scanned)
        mmEmpty(consUI.list, "✅", "All folders are consistent",
          "Every checked tag agrees within each folder. Enable more tags above or widen the scope to keep auditing.");
      else
        mmEmpty(consUI.list, "📝", "Audit tag consistency",
          "Pick a scope on the left, then Scan. Folders whose files disagree on album, artist, genre or year show up here with one-click fixes.");
      return;
    }
    DATA.consFolders.forEach((cf) => {
      const row = el("div", "mm-cons-row");
      /* COLLAPSED by default: the header is a highlighted SUMMARY of what
         is inconsistent (per-tag warning badges); expand for the values. */
      const head = el("div", "mm-cons-head mm-clickable");
      const caret = el("span", "mm-caret", "▸");
      head.appendChild(caret);
      head.appendChild(el("div", "mm-cons-folder", cf.folder));
      const badges = el("div", "mm-cons-badges");
      Object.entries(cf.issues).forEach(([tag, vals]) =>
        badges.appendChild(el("span", "mm-badge warn", tag + " ×" + vals.size)));
      head.appendChild(badges);
      head.appendChild(el("span", "mm-cons-nfiles", cf.files.length + " ♪"));
      const fix = btn("Fix…", "mm-mini", () => consFixDialog(cf));
      fix.addEventListener("click", (ev) => ev.stopPropagation());
      head.appendChild(fix);
      row.appendChild(head);
      const body = el("div", "mm-cons-body");
      body.hidden = true;
      Object.entries(cf.issues).forEach(([tag, vals]) => {
        const line = el("div", "mm-cons-line");
        line.appendChild(el("span", "mm-cons-tag", tag));
        const sorted = [...vals.entries()].sort((a, b) => b[1].length - a[1].length);
        sorted.forEach(([v, files], i) =>
          /* majority value dim; every DEVIANT value highlighted */
          line.appendChild(el("span", "mm-cons-val" + (i === 0 ? " majority" : " deviant"),
            files.length + "× “" + v + "”")));
        body.appendChild(line);
      });
      row.appendChild(body);
      head.addEventListener("click", () => {
        body.hidden = !body.hidden;
        caret.textContent = body.hidden ? "▸" : "▾";
        row.classList.toggle("open", !body.hidden);
      });
      consUI.list.appendChild(row);
    });
  }
  /* Collect the (id, tag, value) rewrites one chosen value implies —
     nothing is sent here; senders batch through tagWriteBatch below. */
  function consJobs(cf, tag, value) {
    const out = [];
    const vals = cf.issues[tag];
    for (const [v, files] of vals) {
      if (v === value) continue;
      files.forEach((t) => out.push({ id: t.id, tag, value }));
    }
    return out;
  }
  /* Merge raw (id, tag, value) triples into one tagwrite per file, so a track
     deviant in several tags gets a single rewrite instead of N. */
  function consMergeJobs(raw) {
    const m = new Map();
    raw.forEach((j) => {
      if (!m.has(j.id)) m.set(j.id, {});
      m.get(j.id)[j.tag] = j.value;
    });
    return [...m.entries()].map(([id, fields]) => ({ id, fields }));
  }
  /* Bounded-parallel tag writing with per-file acks ("tagwrote") — the old
     path fired one unacked tagwrite per deviant track and flooded the bridge
     on big folders. 4 in flight, progress bar + error tally. */
  async function tagWriteBatch(jobs, log, label) {
    RUN.stop = false; RUN.active = true;
    setTabBusy("cons", true);
    let i = 0, ok = 0, fail = 0;
    /* id -> cached row: acked fixes are folded back into DATA.tracks so a
       re-Scan (which reuses the cross-tab cache) reflects reality instead of
       resurrecting the just-fixed inconsistencies from stale rows. */
    const byId = new Map();
    DATA.tracks.forEach((t) => byId.set(t.id, t));
    const worker = async () => {
      for (;;) {
        if (RUN.stop) return;
        const k = i++;
        if (k >= jobs.length) return;
        const j = jobs[k];
        /* keep_missing:true is LOAD-BEARING — without it every field NOT in
           j.fields is authoritative-empty and the writer STRIPS it from the
           file (Title/Artist/Comment gone, Year/Track# deleted). Consistency
           jobs only carry the deviant tags, so preserve-on-empty is exactly
           the intended semantics (all sent values are non-empty by scan). */
        const m = await req({ cmd: "tagwrite", id: j.id, fields: j.fields, keep_missing: true }, "tagwrote", j.id, 30000);
        if (m && m.ok !== false) {
          ok++;
          const t = byId.get(j.id);
          if (t) Object.assign(t, j.fields);
        } else { fail++; j.failed = true; }
        log(label + " " + (ok + fail) + "/" + jobs.length + " · ✓ " + ok + (fail ? " · ✗ " + fail : ""),
            (ok + fail) / (jobs.length || 1));
      }
    };
    await Promise.all(Array.from({ length: 4 }, worker));
    RUN.active = false;
    setTabBusy("cons", false);
    return { ok, fail, stopped: RUN.stop };
  }
  function consFixDialog(cf) {
    /* port of ConsistencyFixDialog: pick the value per tag (majority default) */
    const ov = el("div", "mm-review");
    const card = el("div", "mm-review-card mm-fix");
    card.appendChild(el("div", "mm-review-title", cf.folder));
    const sels = {};
    Object.entries(cf.issues).forEach(([tag, vals]) => {
      const line = el("div", "mm-fix-line");
      line.appendChild(el("span", "mm-cons-tag", tag));
      const sel = el("select", "mm-fix-sel");
      [...vals.entries()].sort((a, b) => b[1].length - a[1].length).forEach(([v, files], i) => {
        const o = el("option", null, v + "  (" + files.length + "×)");
        o.value = v;
        if (i === 0) o.selected = true;
        sel.appendChild(o);
      });
      sels[tag] = sel;
      line.appendChild(sel);
      card.appendChild(line);
    });
    const acts = el("div", "mm-bar");
    const close = () => { document.removeEventListener("keydown", key, true); moClose(ov, () => ov.remove()); };
    const key = (ev) => {
      if (ev.key === "Escape") { ev.preventDefault(); ev.stopPropagation(); close(); }
    };
    const applyB = btn("Apply", "mm-primary", async () => {
      if (RUN.active) return;
      const raw = [];
      Object.keys(sels).forEach((tag) => raw.push(...consJobs(cf, tag, sels[tag].value)));
      const jobs = consMergeJobs(raw);
      close();
      if (!jobs.length) return;
      const r = await tagWriteBatch(jobs, consUI.log, "fixing");
      consUI.log("fixed " + r.ok + " tracks in " + cf.folder +
                 (r.fail ? " · " + r.fail + " failed (locked file?)" : "") +
                 (r.stopped ? " (stopped)" : ""));
      if (!r.fail && !r.stopped) {
        DATA.consFolders = DATA.consFolders.filter((x) => x !== cf);
        consRender();
      }
    });
    if (RUN.active) {
      applyB.disabled = true;
      applyB.title = "Another job is running — wait for it to finish (or press ■ Stop) before applying fixes";
    }
    acts.appendChild(applyB);
    acts.appendChild(btn("Cancel", "", close));
    card.appendChild(acts);
    ov.appendChild(card);
    ov.addEventListener("click", (e) => { if (e.target === ov) close(); });
    document.addEventListener("keydown", key, true);
    document.body.appendChild(ov);
  }
  async function consAutoFixAll() {
    if (RUN.active) return;
    if (!DATA.consFolders.length) { consUI.log("nothing to fix — scan first"); return; }
    const raw = [];
    DATA.consFolders.forEach((cf) => {
      Object.entries(cf.issues).forEach(([tag, vals]) => {
        const top = [...vals.entries()].sort((a, b) => b[1].length - a[1].length)[0][0];
        raw.push(...consJobs(cf, tag, top));
      });
    });
    const jobs = consMergeJobs(raw);
    const nFolders = DATA.consFolders.length;
    const ok = await mmConfirm({
      title: "Auto-fix " + nFolders + " folder" + (nFolders === 1 ? "" : "s") + "?",
      lines: [
        "Rewrites tags on " + jobs.length + " files, setting each inconsistent tag to its folder's majority value.",
        "⚠ Files are modified in place — there is no bulk undo. Fix folders one by one (Fix…) if you want to review the values first.",
      ],
      list: DATA.consFolders.map((cf) => cf.folder),
      confirm: "⇉ Fix " + jobs.length + " files",
      danger: true,
    });
    if (!ok) return;
    const r = await tagWriteBatch(jobs, consUI.log, "auto-fixing");
    consUI.log("auto-fixed " + r.ok + " tracks across " + nFolders + " folders" +
               (r.fail ? " · " + r.fail + " failed — rescan to see what's left" : "") +
               (r.stopped ? " (stopped — rescan to refresh)" : ""));
    if (!r.fail && !r.stopped) {
      DATA.consFolders = [];
      consRender();
    }
  }

  /* ============================================================
     TAB 4 — DUPLICATES (artist+title groups, quality ranked)
     ============================================================ */
  let dupUI = null;
  function buildDupTab(pane) {
    const bar = el("div", "mm-bar");
    bar.appendChild(btn("⟳ Scan", "mm-primary", dupScan));
    bar.appendChild(btn("■ Stop", "", () => { RUN.stop = true; }));
    bar.appendChild(el("span", "mm-lbl", "Select"));
    const selLower = btn("Lower quality", "mm-mini", dupSelectLower);
    selLower.title = "Tick every copy except the best one (highest bitrate, then size) in each group";
    bar.appendChild(selLower);
    const selNone = btn("None", "mm-mini", () => dupSelect(false));
    bar.appendChild(selNone);
    const selInfo = el("span", "mm-dup-selinfo", "");
    bar.appendChild(selInfo);
    const exp = btn("⇩ Export list", "", dupExport);
    exp.title = "Save the ticked file paths as duplicates.txt";
    bar.appendChild(exp);
    const del = btn("♻︎ Recycle Selected", "mm-dangerfill", dupDelete);
    bar.appendChild(del);
    pane.appendChild(bar);
    const log = statusLine(pane);
    const list = el("div", "mm-conslist");
    pane.appendChild(list);
    dupUI = { log, list, del, selInfo };
    dupUpdateSel();
  }
  function dupSelected() {
    const out = [];
    DATA.dupGroups.forEach((g) => g.forEach((t) => { if (t._dupSel) out.push(t); }));
    return out;
  }
  function dupUpdateSel() {
    if (!dupUI) return;
    const sel = dupSelected();
    const bytes = sel.reduce((n, t) => n + (t.size || 0), 0);
    dupUI.selInfo.textContent = sel.length
      ? sel.length + " selected · " + fmtMB(bytes)
      : "";
    dupUI.del.disabled = !sel.length || RUN.active;
    dupUI.del.title = sel.length
      ? "Move the " + sel.length + " ticked files to the Windows Recycle Bin and remove their library rows — drives without a Recycle Bin are skipped, never hard-deleted"
      : "Nothing ticked — use “Lower quality” to select every non-best copy";
  }
  function dupSelect(state) {
    DATA.dupGroups.forEach((g) => g.forEach((t) => {
      t._dupSel = !!state;
      if (t._dupCb) t._dupCb.checked = !!state;
    }));
    dupUpdateSel();
  }
  const dupKey = (t) => ((t.artist || "") + "" + (t.title || ""))
    .toLowerCase().replace(/\(.*?\)|\[.*?\]/g, "").replace(/[^a-z0-9]+/g, " ").trim();
  async function dupScan() {
    if (RUN.active) return;
    RUN.stop = false; RUN.active = true;
    dupUI.log("scanning scope…");
    await loadScopeTracks((n) => dupUI.log("scope: " + n + " tracks…"));
    const map = new Map();
    DATA.tracks.forEach((t) => {
      if (!t.title) return;
      const k = dupKey(t);
      if (!map.has(k)) map.set(k, []);
      map.get(k).push(t);
    });
    DATA.dupGroups = [...map.values()].filter((g) => g.length > 1)
      .map((g) => g.sort((a, b) => (b.bitrate || 0) - (a.bitrate || 0) || (b.size || 0) - (a.size || 0)));
    RUN.active = false;
    dupRender();
    const extra = DATA.dupGroups.reduce((n, g) => n + g.length - 1, 0);
    dupUI.log(DATA.dupGroups.length + " duplicate groups · " + extra + " redundant files");
  }
  function dupRender() {
    dupUI.list.innerHTML = "";
    if (!DATA.dupGroups.length) {
      if (DATA.scanned)
        mmEmpty(dupUI.list, "✨", "No duplicates in this scope",
          "Every artist + title pair is unique. Widen the scope on the left and rescan to check more folders.");
      else
        mmEmpty(dupUI.list, "🔄", "Find duplicate tracks",
          "Pick a scope on the left, then Scan. Copies of the same song are grouped and ranked by quality (bitrate, then size) so you can safely recycle the lesser ones.");
      dupUpdateSel();
      return;
    }
    DATA.dupGroups.forEach((g) => {
      const row = el("div", "mm-cons-row");
      const head = el("div", "mm-cons-head mm-clickable");
      const caret = el("span", "mm-caret", "▸");
      head.appendChild(caret);
      head.appendChild(el("div", "mm-cons-folder", (g[0].artist || "—") + " — " + g[0].title));
      head.appendChild(el("span", "mm-badge warn", g.length + " copies"));
      head.appendChild(el("span", "mm-cons-nfiles", "best: " + fmtQ(g[0])));
      row.appendChild(head);
      const body = el("div", "mm-cons-body");
      body.hidden = true;
      head.addEventListener("click", () => {
        body.hidden = !body.hidden;
        caret.textContent = body.hidden ? "▸" : "▾";
        row.classList.toggle("open", !body.hidden);
      });
      g.forEach((t, i) => {
        const line = el("label", "mm-dup-line" + (i === 0 ? " best" : ""));
        const cb = el("input"); cb.type = "checkbox"; cb.checked = !!t._dupSel;
        cb.addEventListener("change", () => { t._dupSel = cb.checked; dupUpdateSel(); });
        line.appendChild(cb);
        line.appendChild(el("span", "mm-dup-q", fmtQ(t)));
        line.appendChild(el("span", "mm-dup-p", t.path));
        if (i === 0) line.appendChild(el("span", "mm-dup-best", "BEST"));
        if (t._delFail) {
          const noBin = t._delFail === "no-recycle";
          const fb = el("span", "mm-badge warn", noBin ? "no recycle bin"
                        : t._delFail === "timeout" ? "not attempted" : "in use");
          fb.title = noBin
            ? "This drive has no Recycle Bin (network or removable) — recycling would permanently delete, so the file was left untouched"
            : t._delFail === "timeout"
              ? "No reply from the delete worker — the file was left in the library; rescan to reconcile"
              : "Could not be moved to the Recycle Bin — the file is probably playing or open elsewhere";
          line.appendChild(fb);
        }
        body.appendChild(line);
        t._dupCb = cb;
      });
      row.appendChild(body);
      dupUI.list.appendChild(row);
    });
    dupUpdateSel();
  }
  function dupSelectLower() {
    DATA.dupGroups.forEach((g) => g.forEach((t, i) => { t._dupSel = i > 0; if (t._dupCb) t._dupCb.checked = i > 0; }));
    dupUpdateSel();
    dupUI.log("selected every non-best copy");
  }
  function dupExport() {
    const lines = dupSelected().map((t) => t.path);
    if (!lines.length) { dupUI.log("nothing selected — tick the copies to export"); return; }
    const blob = new Blob([lines.join("\r\n")], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "duplicates.txt";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    dupUI.log("exported " + lines.length + " paths");
  }
  /* ---- DELETE selected → Windows Recycle Bin (backed by cmd:"deletetracks";
     recycle-only, never a hard delete; chunked so progress stays live) ---- */
  async function dupDelete() {
    if (RUN.active) return;
    const sel = dupSelected();
    if (!sel.length) { dupUI.log("nothing selected — tick the copies to remove"); return; }
    const bytes = sel.reduce((n, t) => n + (t.size || 0), 0);
    /* safety callout: groups where EVERY copy is ticked lose the song entirely */
    const wholeGroups = DATA.dupGroups.filter((g) => g.every((t) => t._dupSel)).length;
    const lines = [
      "Files go to the Windows Recycle Bin and their rows leave the library.",
      "≈ " + fmtMB(bytes) + " will be reclaimed.",
      "Files on drives without a Recycle Bin (network / removable) are SKIPPED, never hard-deleted — they stay on disk and get flagged here.",
    ];
    if (wholeGroups)
      lines.push("⚠ " + wholeGroups + " group" + (wholeGroups === 1 ? " has" : "s have") +
                 " EVERY copy ticked — those songs will be removed entirely, best copy included.");
    const ok = await mmConfirm({
      title: "Move " + sel.length + " file" + (sel.length === 1 ? "" : "s") + " to the Recycle Bin?",
      lines,
      list: sel.map((t) => t.path),
      confirm: "♻︎ Recycle " + sel.length + " file" + (sel.length === 1 ? "" : "s"),
      danger: true,
    });
    if (!ok) return;
    RUN.stop = false; RUN.active = true;
    setTabBusy("dup", true);
    dupUpdateSel();
    let done = 0, recycled = 0, removed = 0;
    const failedIds = [], failReason = {}, sentIds = [];
    const CHUNK = 40;
    for (let i = 0; i < sel.length && !RUN.stop; i += CHUNK) {
      const chunk = sel.slice(i, i + CHUNK);
      chunk.forEach((t) => sentIds.push(t.id));   /* only SUBMITTED ids may leave the UI */
      const m = await req({ cmd: "deletetracks", ids: chunk.map((t) => t.id), tag: i },
                          "deletetracks", i, 120000);
      if (!m) {
        chunk.forEach((t) => { failedIds.push(t.id); failReason[t.id] = "timeout"; });   /* keep rows visible */
      } else {
        recycled += m.recycled || 0;
        removed  += m.removed  || 0;
        (m.failed || []).forEach((id, ix) => {
          failedIds.push(id);
          failReason[id] = (m.failed_reason || [])[ix] || "in-use";
        });
      }
      done += chunk.length;
      dupUI.log("recycling… " + done + "/" + sel.length, done / sel.length);
    }
    const failSet = new Set(failedIds);
    /* Rows leave the UI only when their chunk was actually SENT and the id did
       not come back failed — a mid-run Stop must not vanish untouched files. */
    const delSet = new Set(sentIds.filter((id) => !failSet.has(id)));
    sel.forEach((t) => { t._delFail = failSet.has(t.id) ? (failReason[t.id] || "in-use") : null; if (t._delFail) t._dupSel = false; });
    DATA.tracks = DATA.tracks.filter((t) => !delSet.has(t.id));
    DATA.dupGroups = DATA.dupGroups
      .map((g) => g.filter((t) => !delSet.has(t.id)))
      .filter((g) => g.length > 1);
    /* keep the Art tab coherent: drop recycled tracks from the album groups,
       remap firstId to a surviving track (artwrite targets firstId!) and drop
       albums that lost every track — otherwise Apply hits recycled files. */
    if (delSet.size && DATA.albums.length) {
      DATA.albums = DATA.albums.filter((al) => {
        al.tracks = al.tracks.filter((t) => !delSet.has(t.id));
        if (!al.tracks.length) return false;
        if (delSet.has(al.firstId)) al.firstId = al.tracks[0].id;
        return true;
      });
      if (artUI) {
        if (artUI.sel && DATA.albums.indexOf(artUI.sel) < 0) { artUI.sel = null; renderPreview(null); }
        artRender(artUI.filter);
      }
    }
    if (lyrUI && lyrUI.rows.length)
      lyrUI.rows = lyrUI.rows.filter((t) => !delSet.has(t.id));
    /* keep the Consistency tab coherent: recycled tracks must leave the
       per-folder issue maps, or a later Fix run sends tagwrite for removed
       ids and mis-reports them as "locked file". Folders whose issues vanish
       with the deleted copies drop off the list entirely. */
    if (delSet.size && DATA.consFolders.length) {
      DATA.consFolders = DATA.consFolders.filter((cf) => {
        cf.files = cf.files.filter((t) => !delSet.has(t.id));
        let any = false;
        Object.keys(cf.issues).forEach((tag) => {
          const vals = cf.issues[tag];
          [...vals.entries()].forEach(([v, files]) => {
            const kept = files.filter((t) => !delSet.has(t.id));
            if (kept.length) vals.set(v, kept); else vals.delete(v);
          });
          if (vals.size <= 1) delete cf.issues[tag]; else any = true;
        });
        return any && cf.files.length > 0;
      });
      if (consUI) consRender();
    }
    /* and the Classifier tab: refresh per-folder track counts from the
       pruned cache and drop folders that lost every track (applied badges
       on surviving folders are preserved). */
    if (delSet.size && DATA.classFolders.length) {
      const cnt = new Map();
      DATA.tracks.forEach((t) => { const f = dirOf(t.path || ""); cnt.set(f, (cnt.get(f) || 0) + 1); });
      DATA.classFolders = DATA.classFolders.filter((cf) => {
        cf.n = cnt.get(cf.folder) || 0;
        return cf.n > 0;
      });
      if (clsUI) clsRender();
    }
    RUN.active = false;
    setTabBusy("dup", false);
    dupRender();
    dupUpdateSel();
    dupUI.log("♻ " + recycled + " file" + (recycled === 1 ? "" : "s") + " moved to the Recycle Bin · " +
              removed + " rows removed" +
              (failSet.size ? " · ⚠ " + failSet.size + " skipped (see badges)" : "") +
              (RUN.stop ? " (stopped — unsent files untouched)" : ""));
  }

  /* ============================================================
     TAB 5 — CLASSIFIER (port of MediaTypeClassifier.classify)
     ============================================================ */
  let clsUI = null;
  const CLS_RULES = [
    ["Film OST",  2, ["soundtrack", "ost", "motion picture", "film score"]],
    ["Game OST",  2, ["game", "video game", "nintendo", "playstation", "final fantasy"]],
    ["Anime OST", 2, ["anime", "opening theme", "ending theme"]],
    ["Audiobook", 3, ["audiobook", "narrated by", "unabridged", "chapter"]],
    ["Podcast",   3, ["podcast", "episode"]],
  ];
  /* classifier category → the app's folder-kind taxonomy (each non-music
     kind becomes its own isolated sidebar library; see setrootkind) */
  const CLS_KIND = { "Film OST": "film ost", "Game OST": "game ost", "Anime OST": "anime ost",
                     "Audiobook": "audiobook", "Podcast": "podcast" };
  function buildClsTab(pane) {
    const bar = el("div", "mm-bar");
    bar.appendChild(btn("⟳ Classify Scope", "mm-primary", clsScan));
    bar.appendChild(btn("■ Stop", "", () => { RUN.stop = true; }));
    pane.appendChild(bar);
    pane.appendChild(el("div", "mm-log",
      "Detects Film/Game/Anime OSTs, audiobooks and podcasts by folder + tag keywords. " +
      "“Set kind” designates the folder right here — it becomes its own isolated library in the sidebar."));
    const log = statusLine(pane);
    const list = el("div", "mm-conslist");
    pane.appendChild(list);
    clsUI = { log, list };
  }
  async function clsApply(cf) {
    const kind = CLS_KIND[cf.type] || cf.type.toLowerCase();
    const ok = await mmConfirm({
      title: "Designate folder as “" + kind + "”?",
      lines: [
        cf.folder,
        cf.n + " tracks · detected " + cf.type + " at " + Math.round(cf.conf * 100) +
          "% confidence (keywords: " + cf.kw.join(", ") + ")",
        "The folder leaves the music library and becomes its own “" + kind +
          "” library under Libraries in the sidebar. Reversible any time: set it back to Music in the Folders view.",
      ],
      confirm: "Set kind: " + kind,
    });
    if (!ok) return;
    A.send({ cmd: "setrootkind", path: cf.folder, kind });
    cf.applied = kind;
    clsRender();
    clsUI.log("designated " + cf.folder + " as “" + kind + "”");
    /* setrootkind replies with "kinds" (sidebar), not "roots" — request fresh
       roots so the scope rail shows the new [kind] suffix immediately (the
       "roots" reply refreshes A.getRoots() and re-triggers buildScope). */
    setTimeout(() => A.send({ cmd: "roots" }), 350);
  }
  /* Per-kind bulk apply. Detection is per LEAF folder, so an audiobook or
     OST collection with per-title subfolders yields dozens of detections —
     applied one by one they each became their own sidebar root. This
     confirms ONCE and, where every scanned leaf under a common parent is a
     detected sibling of the SAME kind, collapses them into one parent root. */
  function clsBulkTargets(list) {
    const byParent = new Map();
    list.forEach((cf) => {
      const p = dirOf(cf.folder);
      if (!byParent.has(p)) byParent.set(p, []);
      byParent.get(p).push(cf);
    });
    const targets = [];
    for (const [parent, kids] of byParent) {
      let collapse = kids.length >= 2 && parent && parent !== kids[0].folder;
      if (collapse) {
        /* safe only if the scan saw NO other track folder under this parent
           (incl. loose tracks in the parent itself) — otherwise the parent
           kind would hijack unrelated music */
        const kidSet = new Set(kids.map((c) => c.folder.toLowerCase()));
        const pfx = parent.toLowerCase() + "\\";
        for (const t of DATA.tracks) {
          const f = dirOf(t.path || "").toLowerCase();
          if ((f === parent.toLowerCase() || f.indexOf(pfx) === 0) && !kidSet.has(f)) { collapse = false; break; }
        }
      }
      if (collapse) targets.push({ path: parent, covers: kids });
      else kids.forEach((cf) => targets.push({ path: cf.folder, covers: [cf] }));
    }
    return targets;
  }
  async function clsBulkApply(kind) {
    const list = DATA.classFolders.filter((cf) => !cf.applied &&
      (CLS_KIND[cf.type] || cf.type.toLowerCase()) === kind);
    if (!list.length) return;
    const targets = clsBulkTargets(list);
    const s = targets.length === 1 ? "" : "s";
    const ok = await mmConfirm({
      title: "Set kind “" + kind + "” on " + targets.length + " folder" + s + "?",
      lines: [
        list.length + " detected folder" + (list.length === 1 ? "" : "s") + " → " +
          targets.length + " sidebar root" + s +
          (targets.length < list.length ? " (sibling folders collapsed into their common parent)" : "") + ".",
        "Each root leaves the music library and becomes its own “" + kind +
          "” library under Libraries in the sidebar. Reversible any time in the Folders view.",
      ],
      list: targets.map((t) => t.path + (t.covers.length > 1 ? "   (covers " + t.covers.length + " folders)" : "")),
      confirm: "Set kind on " + targets.length,
    });
    if (!ok) return;
    targets.forEach((tg) => {
      A.send({ cmd: "setrootkind", path: tg.path, kind });
      tg.covers.forEach((cf) => { cf.applied = kind; });
    });
    clsRender();
    clsUI.log("designated " + targets.length + " folder" + s + " as “" + kind + "”" +
              (targets.length < list.length ? " (covering " + list.length + " detections)" : ""));
    setTimeout(() => A.send({ cmd: "roots" }), 350);
  }
  async function clsScan() {
    if (RUN.active) return;
    RUN.stop = false; RUN.active = true;
    clsUI.log("scanning scope…");
    await loadScopeTracks((n) => clsUI.log("scope: " + n + " tracks…"));
    const byFolder = new Map();
    DATA.tracks.forEach((t) => {
      const f = dirOf(t.path || "");
      if (!byFolder.has(f)) byFolder.set(f, []);
      byFolder.get(f).push(t);
    });
    DATA.classFolders = [];
    for (const [folder, files] of byFolder) {
      const name = folder.split(/[\\/]/).pop().toLowerCase();
      const text = name + " " + files.map((t) =>
        [(t.artist || ""), (t.album || ""), (t.title || ""), (t.genre || "")].join(" ").toLowerCase()).join(" ");
      const scores = {}; const kw = [];
      CLS_RULES.forEach(([cat, pts, words]) => {
        scores[cat] = 0;
        words.forEach((w) => { if (text.includes(w)) { scores[cat] += pts; kw.push(w); } });
      });
      const best = Object.keys(scores).reduce((a, b) => (scores[a] >= scores[b] ? a : b));
      const total = Object.values(scores).reduce((a, b) => a + b, 0);
      if (scores[best] > 0)
        DATA.classFolders.push({ folder, n: files.length, type: best,
          conf: total ? scores[best] / total : 0.5, kw: [...new Set(kw)].slice(0, 5) });
    }
    DATA.classFolders.sort((a, b) => b.conf - a.conf);
    RUN.active = false;
    clsRender();
    clsUI.log(byFolder.size + " folders · " + DATA.classFolders.length + " classified as non-music");
  }
  function clsRender() {
    clsUI.list.innerHTML = "";
    if (!DATA.classFolders.length) {
      if (DATA.scanned)
        mmEmpty(clsUI.list, "🎵", "Everything looks like music",
          "No folder in this scope matched the OST / audiobook / podcast keyword rules.");
      else
        mmEmpty(clsUI.list, "📂", "Classify your folders",
          "Pick a scope on the left, then Classify. Detected soundtracks, audiobooks and podcasts can be split into their own libraries with one click.");
      return;
    }
    /* bulk bar: one click applies a kind to every unapplied detection of
       that kind (sibling leaves collapse into their parent — see clsBulkApply) */
    const kindCount = new Map();
    DATA.classFolders.forEach((cf) => {
      if (cf.applied) return;
      const k = CLS_KIND[cf.type] || cf.type.toLowerCase();
      kindCount.set(k, (kindCount.get(k) || 0) + 1);
    });
    if ([...kindCount.values()].some((n) => n >= 2)) {
      const bar = el("div", "mm-bar");
      bar.appendChild(el("span", "mm-lbl", "Bulk"));
      for (const [k, n] of kindCount) {
        if (n < 2) continue;
        const b = btn("Set “" + k + "” on all " + n + "…", "mm-mini", () => clsBulkApply(k));
        b.title = "Apply the “" + k + "” kind to all " + n + " detected folders in one confirmed step — " +
                  "sibling folders that fully cover their parent become a single parent root";
        bar.appendChild(b);
      }
      clsUI.list.appendChild(bar);
    }
    DATA.classFolders.forEach((cf) => {
      const row = el("div", "mm-cons-row");
      const head = el("div", "mm-cons-head");
      head.appendChild(el("div", "mm-cons-folder", cf.folder));
      head.appendChild(el("span", "mm-cls-type", cf.type + " · " + Math.round(cf.conf * 100) + "%"));
      if (cf.applied) {
        const b = el("span", "mm-badge good", "✓ " + cf.applied);
        b.title = "Kind applied — this folder is now its own library (undo in the Folders view)";
        head.appendChild(b);
      } else {
        const b = btn("Set kind…", "mm-mini", () => clsApply(cf));
        b.title = "Designate this folder as “" + (CLS_KIND[cf.type] || cf.type.toLowerCase()) +
                  "” — it becomes its own isolated library";
        head.appendChild(b);
      }
      row.appendChild(head);
      row.appendChild(el("div", "mm-cons-line", cf.n + " tracks · keywords: " + cf.kw.join(", ")));
      clsUI.list.appendChild(row);
    });
  }

  /* ============================================================ */
  function open(api) {
    A = api;
    host = document.getElementById("view-mediatool");
    if (!host) return;
    if (!open._tapped) {
      open._tapped = true;
      A.tap("mmtracks", (m) => settle("mmtracks", (m.prefix || "") + ":" + (m.offset || 0), m));
      A.tap("lyrics",   (m) => settle("lyrics", m.id, m));
      A.tap("roots",    () => { if (built) buildScope(); });
      /* recycle-bin delete results (duplicates tab) + per-file tag-write
         acks (consistency batching) — tap(), NEVER on(): see bus contract */
      A.tap("deletetracks", (m) => settle("deletetracks", m.tag, m));
      A.tap("tagwrote",     (m) => settle("tagwrote", m.id, m));
      A.tap("artwrote",     (m) => settle("artwrote", m.id, m));
      A.tap("lyricswrote",  (m) => settle("lyricswrote", m.id, m));
    }
    build();
  }
  return { open };
})();
