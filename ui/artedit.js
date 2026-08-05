/* ============================================================
   MONATOMIC — Album Art Editor
   Mounted inside the tag-edit modal by tagedit.js.
   Keyless cover search (iTunes Search API + Deezer), film-strip
   candidate picker (click to select, ← → to move, Enter to
   accept), plus a direct "From URL" pipeline. Accepted covers
   are decoded through a canvas and handed back as base64 for
   the artwrite bridge command.
   ============================================================ */
window.MnArtEdit = (function () {
  "use strict";

  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  let o = null;               /* mount options: {artist, album, art, onApply, allowWholeAlbum} */
  let cands = [], sel = -1, busy = false;
  let curBox = null, stripEl = null, statusEl = null, urlWrap = null, urlInput = null, applyBtn = null, wholeChk = null;

  /* whether the fetched cover should be written to every track in the album.
     Only meaningful when opts.allowWholeAlbum rendered the checkbox. */
  function wholeAlbum() { return !!(wholeChk && wholeChk.checked); }

  /* ---------- provider engine ----------
     Every provider returns [{thumb, full, label, src}] — label is the TITLE
     the source itself attributes to the image (shown in small print under
     each candidate). Stores/APIs first, then search engines, then the
     obscure crate-digging sources (Discogs / Bandcamp / Archive.org /
     Genius). All keyless; scrapers are best-effort and fail silent. */
  const Q = encodeURIComponent;
  const jget = (u) => fetch(u).then((r) => (r.ok ? r.json() : null)).catch(() => null);
  const tget = (u) => fetch(u).then((r) => (r.ok ? r.text() : null)).catch(() => null);
  const host = (u) => { try { return new URL(u).hostname.replace(/^www\./, ""); } catch (_) { return ""; } };

  function itunes(q) {
    return jget("https://itunes.apple.com/search?term=" + Q(q) + "&entity=album&limit=6")
      .then((j) => ((j && j.results) || []).map((x) => ({
        thumb: x.artworkUrl100 || "",
        full: String(x.artworkUrl100 || "").replace("100x100", "1200x1200"),
        label: (x.collectionName || "") + (x.artistName ? " — " + x.artistName : ""),
        src: "iTunes",
      })).filter((c) => c.thumb));
  }
  function deezer(q) {
    return jget("https://api.deezer.com/search/album?q=" + Q(q))
      .then((j) => (((j && j.data) || []).slice(0, 6)).map((x) => ({
        thumb: x.cover_medium || x.cover || "",
        full: x.cover_xl || x.cover_big || x.cover || "",
        label: (x.title || "") + (x.artist && x.artist.name ? " — " + x.artist.name : ""),
        src: "Deezer",
      })).filter((c) => c.full));
  }
  function caa(artist, album) {
    return jget("https://musicbrainz.org/ws/2/release/?query=" +
        Q('artist:"' + artist + '" AND release:"' + album + '"') + "&fmt=json&limit=3")
      .then((j) => (((j && j.releases) || []).slice(0, 3)).map((r) => ({
        thumb: "https://coverartarchive.org/release/" + r.id + "/front-250",
        full: "https://coverartarchive.org/release/" + r.id + "/front-1200",
        label: (r.title || "") + (r.date ? " (" + String(r.date).slice(0, 4) + ")" : ""),
        src: "MusicBrainz",
      })));
  }
  function lastfm(artist, album) {
    return jget("https://ws.audioscrobbler.com/2.0/?method=album.getinfo&api_key=b25b959554ed76058ac220b7b2e0a026&artist=" +
        Q(artist) + "&album=" + Q(album) + "&format=json")
      .then((j) => {
        const a = j && j.album;
        const imgs = (a && a.image) || [];
        for (let i = imgs.length - 1; i >= 0; i--) {
          const u = imgs[i]["#text"];
          if (u && u.indexOf("noimage") < 0) {
            return [{ thumb: u, full: u.replace(/\/i\/u\/[^/]+\//, "/i/u/"),
                      label: (a.name || "") + (a.artist ? " — " + a.artist : ""),
                      src: "Last.fm" }];
          }
        }
        return [];
      });
  }
  async function ddg(q) {
    const seed = await tget("https://duckduckgo.com/?q=" + Q(q + " album cover") + "&iax=images&ia=images");
    const vqd = seed && (seed.match(/vqd=["']?([\d-]+)/) || [])[1];
    if (!vqd) return [];
    const j = await jget("https://duckduckgo.com/i.js?l=us-en&o=json&q=" + Q(q + " album cover") +
                         "&vqd=" + vqd + "&f=,,,layout:Square");
    return (((j && j.results) || []).slice(0, 6))
      .sort((a, b) => (b.width * b.height) - (a.width * a.height))
      .map((r) => ({ thumb: r.thumbnail || r.image, full: r.image,
                     label: r.title || host(r.image), src: "DuckDuckGo" }));
  }
  async function bing(q) {
    const html = await tget("https://www.bing.com/images/search?q=" +
      Q(q + " album cover") + "&qft=+filterui:aspect-square+filterui:imagesize-large");
    if (!html) return [];
    const out = [];
    /* each grid tile is a JSON blob carrying murl (full), turl (thumb), t (title) */
    const re = /"murl":"(https?:[^"]+?)"[^}]*?(?:"turl":"(https?:[^"]+?)")?[^}]*?"t":"([^"]*?)"/g;
    let m;
    while ((m = re.exec(html)) && out.length < 6) {
      const fix = (s) => String(s || "").replace(/\\u002f/gi, "/").replace(/\\\//g, "/");
      out.push({ full: fix(m[1]), thumb: fix(m[2]) || fix(m[1]),
                 label: (m[3] || host(m[1])).replace(/\\u0026/g, "&"), src: "Bing" });
    }
    return out;
  }
  async function google(q) {
    const html = await tget("https://www.google.com/search?q=" +
      Q(q + " album cover") + "&tbm=isch&tbs=iar:s,isz:l");
    if (!html) return [];
    const cands = [];
    const re = /\["(https?:\/\/[^"]+?\.(?:jpg|jpeg|png|webp)[^"]*?)",(\d{3,5}),(\d{3,5})\]/g;
    let m;
    while ((m = re.exec(html)) && cands.length < 40) {
      const u = m[1].replace(/\\u003d/gi, "=").replace(/\\u0026/gi, "&");
      if (/gstatic\.com|googleusercontent/.test(u)) continue;
      cands.push({ u, s: Math.min(+m[2], +m[3]) });
    }
    return cands.sort((a, b) => b.s - a.s).slice(0, 6)
      .map((c) => ({ thumb: c.u, full: c.u, label: host(c.u), src: "Google" }));
  }
  async function yandex(q) {
    const html = await tget("https://yandex.com/images/search?text=" +
      Q(q + " album cover") + "&iorient=square&isize=large");
    if (!html) return [];
    const out = [];
    const re = /"origUrl":"(https?:[^"]+?)"/g;
    let m;
    while ((m = re.exec(html)) && out.length < 5) {
      const u = m[1].replace(/\\u002F/gi, "/").replace(/\\\//g, "/");
      out.push({ thumb: u, full: u, label: host(u), src: "Yandex" });
    }
    return out;
  }
  /* — the obscure crate: Discogs / Bandcamp / Archive.org / Genius — */
  async function discogs(q) {
    const html = await tget("https://www.discogs.com/search/?q=" + Q(q) + "&type=release");
    if (!html) return [];
    const out = [];
    const re = /<img[^>]+data-src="(https:\/\/i\.discogs\.com\/[^"]+)"[^>]*alt="([^"]*)"|<img[^>]+src="(https:\/\/i\.discogs\.com\/[^"]+)"[^>]*alt="([^"]*)"/g;
    let m;
    while ((m = re.exec(html)) && out.length < 5) {
      const u = m[1] || m[3], alt = (m[2] || m[4] || "").trim();
      if (!u || /spacer|blank/.test(u)) continue;
      out.push({ thumb: u, full: u, label: alt || "Discogs release", src: "Discogs" });
    }
    return out;
  }
  async function bandcamp(q) {
    const html = await tget("https://bandcamp.com/search?q=" + Q(q) + "&item_type=a");
    if (!html) return [];
    const arts = [];
    const reA = /<img[^>]+src="(https:\/\/f\d\.bcbits\.com\/img\/a\d+_\d+\.jpg)"/g;
    let m;
    while ((m = reA.exec(html)) && arts.length < 5) arts.push(m[1]);
    const titles = [];
    const reT = /class="heading">\s*<a[^>]*>\s*([^<]+?)\s*<\/a>/g;
    while ((m = reT.exec(html)) && titles.length < 8) titles.push(m[1].trim());
    return arts.map((u, i) => ({
      thumb: u,
      full: u.replace(/_\d+\.jpg$/, "_10.jpg"),   /* _10 = original size */
      label: titles[i] || "Bandcamp album", src: "Bandcamp" }));
  }
  function archiveOrg(q) {
    return jget("https://archive.org/advancedsearch.php?q=" + Q(q) +
                "&fl%5B%5D=identifier&fl%5B%5D=title&rows=4&page=1&output=json")
      .then((j) => ((((j || {}).response || {}).docs) || []).map((d) => ({
        thumb: "https://archive.org/services/img/" + d.identifier,
        full: "https://archive.org/services/img/" + d.identifier,
        label: d.title || d.identifier, src: "Archive.org" })));
  }
  function genius(q) {
    return jget("https://genius.com/api/search/multi?q=" + Q(q))
      .then((j) => {
        const out = [];
        const secs = (j && j.response && j.response.sections) || [];
        for (const s of secs) {
          for (const h of s.hits || []) {
            const r = h.result || {};
            const u = r.cover_art_url || r.song_art_image_url || r.header_image_url;
            if (u && !/default_cover/.test(u)) {
              out.push({ thumb: u, full: u,
                         label: (r.full_title || r.title || r.name || "Genius"),
                         src: "Genius" });
            }
            if (out.length >= 4) return out;
          }
        }
        return out;
      });
  }

  /* ---------- image -> base64 pipeline (mime-preserving) ---------- */
  async function toB64(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error("HTTP " + r.status);
    const srcMime = String(r.headers.get("content-type") || "").split(";")[0].trim().toLowerCase();
    const blob = await r.blob();
    const bmp = await createImageBitmap(blob);
    const canvas = document.createElement("canvas");
    canvas.width = bmp.width; canvas.height = bmp.height;
    canvas.getContext("2d").drawImage(bmp, 0, 0);
    if (bmp.close) bmp.close();
    /* keep png as png; everything else becomes high-quality jpeg */
    const mime = srcMime === "image/png" ? "image/png" : "image/jpeg";
    const dataUrl = canvas.toDataURL(mime, 0.92);
    return { b64: dataUrl.slice(dataUrl.indexOf(",") + 1), mime, dataUrl };
  }

  /* ---------- UI ---------- */
  function setStatus(s) { if (statusEl) statusEl.textContent = s || ""; }

  function setCur(url) {
    if (!curBox) return;
    curBox.innerHTML = "";
    if (url) {
      const img = el("img");
      img.alt = "";
      img.src = url;
      curBox.appendChild(img);
    } else {
      curBox.appendChild(el("span", "art-glyph", "♪"));
    }
  }

  function renderStrip() {
    stripEl.innerHTML = "";
    stripEl.hidden = !cands.length;
    cands.forEach((c, i) => {
      const t = el("div", "art-thumb" + (i === sel ? " sel" : ""));
      t.title = (c.label ? c.label + "\n" : "") + "Source: " + c.src;
      const img = el("img");
      img.loading = "lazy"; img.alt = c.label || "";
      img.src = c.thumb;
      img.onerror = () => { t.classList.add("dead"); };
      t.appendChild(img);
      t.appendChild(el("span", "art-src", c.src));
      /* small-print attribution: the title the SOURCE gives this image */
      if (c.label) t.appendChild(el("span", "art-cap", c.label));
      t.addEventListener("click", () => select(i));
      t.addEventListener("dblclick", () => { select(i); accept(); });
      stripEl.appendChild(t);
    });
  }

  function select(i) {
    if (!cands.length) return;
    sel = clamp(i, 0, cands.length - 1);
    Array.from(stripEl.children).forEach((c, j) => c.classList.toggle("sel", j === sel));
    const t = stripEl.children[sel];
    if (t && t.scrollIntoView) t.scrollIntoView({ inline: "center", block: "nearest", behavior: "smooth" });
    applyBtn.disabled = false;
    const c = cands[sel];
    setStatus(c.src + (c.label ? "  ·  " + c.label : "") + "   —   Enter to apply");
  }

  async function findOnline() {
    if (busy) return;
    const artist = (o.artist || "").trim(), album = (o.album || "").trim();
    const q = (artist + " " + album).trim();
    if (!q) { setStatus("No artist/album metadata to search with."); return; }
    busy = true;
    cands = []; sel = -1; renderStrip();
    /* every provider streams in as it returns — stores first (fast APIs),
       search engines and the obscure crate trail in behind */
    const provs = [
      itunes(q), deezer(q), caa(artist, album), lastfm(artist, album),
      ddg(q), bing(q), google(q), yandex(q),
      discogs(q), bandcamp(q), archiveOrg(q), genius(q),
    ];
    let doneN = 0;
    const seen = new Set();
    setStatus("Searching 12 sources…");
    provs.forEach((p) => p.catch(() => []).then((list) => {
      doneN++;
      for (const c of list || []) {
        if (!c || !c.full || seen.has(c.full)) continue;
        seen.add(c.full);
        if (cands.length < 36) cands.push(c);
      }
      renderStrip();
      if (sel < 0 && cands.length) { select(0); stripEl.focus(); }
      setStatus(cands.length + " candidates from " + doneN + "/12 sources" +
                (doneN < 12 ? "…" : " — click or ← → to browse, Enter to apply."));
      if (doneN >= 12) {
        busy = false;
        if (!cands.length) { setStatus("No covers found for “" + q + "”."); applyBtn.disabled = true; }
      }
    }));
  }

  async function accept() {
    if (busy || sel < 0 || !cands[sel]) return;
    const c = cands[sel];
    busy = true;
    setStatus("Downloading " + c.src + " cover…");
    try {
      const out = await toB64(c.full);
      setCur(out.dataUrl);
      setStatus(wholeAlbum() ? "Writing cover to whole album…" : "Writing cover…");
      o.onApply(out.b64, out.mime, (ok) => {
        busy = false;
        setStatus(ok === true ? (wholeAlbum() ? "Cover saved to album ✓" : "Cover saved ✓")
          : ok === false ? "Backend rejected the cover write."
          : "Backend write support coming online — cover shown here won't persist yet.");
      }, wholeAlbum());
    } catch (e) {
      busy = false;
      setStatus("Cover download failed (" + (e && e.message ? e.message : "error") + ").");
    }
  }

  async function applyUrl(u) {
    if (busy) return;
    busy = true;
    setStatus("Downloading image…");
    try {
      const out = await toB64(u);
      setCur(out.dataUrl);
      setStatus(wholeAlbum() ? "Writing cover to whole album…" : "Writing cover…");
      o.onApply(out.b64, out.mime, (ok) => {
        busy = false;
        setStatus(ok === true ? (wholeAlbum() ? "Cover saved to album ✓" : "Cover saved ✓")
          : ok === false ? "Backend rejected the cover write."
          : "Backend write support coming online — cover shown here won't persist yet.");
      }, wholeAlbum());
    } catch (e) {
      busy = false;
      setStatus("Could not load that URL (" + (e && e.message ? e.message : "error") + ").");
    }
  }

  /* ============================================================
     MOUNT — called by tagedit.js each time the modal opens.
     opts: { artist, album, art, onApply(b64, mime, done) }
     ============================================================ */
  function mount(container, opts) {
    o = opts || {};
    cands = []; sel = -1; busy = false; wholeChk = null;
    container.innerHTML = "";

    const row = el("div", "art-row");
    curBox = el("div", "art-cur");
    setCur(o.art || "");
    row.appendChild(curBox);

    const right = el("div", "art-ctl");
    const btns = el("div", "art-btns");
    const bFind = el("button", "btn-mini", "⌕  Find online");
    bFind.addEventListener("click", findOnline);
    const bUrl = el("button", "btn-mini", "From URL…");
    bUrl.addEventListener("click", () => { urlWrap.hidden = !urlWrap.hidden; if (!urlWrap.hidden) urlInput.focus(); });
    applyBtn = el("button", "btn-mini art-apply", "Use selected cover");
    applyBtn.disabled = true;
    applyBtn.addEventListener("click", accept);
    btns.appendChild(bFind); btns.appendChild(bUrl); btns.appendChild(applyBtn);
    right.appendChild(btns);

    /* "Apply to whole album" — only when opened from a single track that has an
       album, so a fetched cover can be written to every track at once. */
    if (o.allowWholeAlbum) {
      const wrap = el("label", "art-whole");
      wholeChk = document.createElement("input");
      wholeChk.type = "checkbox";
      wholeChk.checked = !!o.wholeAlbumDefault;
      wrap.appendChild(wholeChk);
      wrap.appendChild(document.createTextNode(" Apply to whole album"));
      right.appendChild(wrap);
    }

    urlWrap = el("div", "art-url-row");
    urlWrap.hidden = true;
    urlInput = document.createElement("input");
    urlInput.type = "text";
    urlInput.placeholder = "https://…  image URL — Enter to apply";
    urlInput.spellcheck = false;
    urlInput.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && urlInput.value.trim()) applyUrl(urlInput.value.trim());
    });
    const bGo = el("button", "btn-mini", "Apply");
    bGo.addEventListener("click", () => { if (urlInput.value.trim()) applyUrl(urlInput.value.trim()); });
    urlWrap.appendChild(urlInput); urlWrap.appendChild(bGo);
    right.appendChild(urlWrap);

    statusEl = el("div", "art-status", "");
    right.appendChild(statusEl);
    row.appendChild(right);
    container.appendChild(row);

    stripEl = el("div", "art-strip");
    stripEl.tabIndex = 0;
    stripEl.hidden = true;
    stripEl.addEventListener("keydown", (e) => {
      if (e.key === "ArrowRight")     { e.preventDefault(); select(sel + 1); }
      else if (e.key === "ArrowLeft") { e.preventDefault(); select(sel - 1); }
      else if (e.key === "Enter")     { e.preventDefault(); accept(); }
    });
    container.appendChild(stripEl);
  }

  return { mount };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("artedit", "1.0.0", [], function () { return window.MnArtEdit || {}; });
