/* ============================================================
   MONATOMIC — Tag Editor
   Right-click a track row -> "Edit tags…" modal with a live
   Before → After diff strip (red current values, green edited
   values). Right-click an album card -> "Edit album tags…"
   applies artist/album/album-artist/genre/year to every track
   of the album (one tagwrite per track id from albumtracks).
   Also hosts the cover-art editor (artedit.js) and the
   "Fill from filename" helper.

   app.js boots this via MnTagEdit.init(api); track rows call
   MnTagEdit.trackMenu(ev, row, rowEl) and album cards call
   MnTagEdit.albumMenu(ev, album, cardEl) from contextmenu.
   ============================================================ */
window.MnTagEdit = (function () {
  "use strict";

  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };

  /* motion bridge (plain IIFE, MN is global): graceful dismissal for the
     modals + context menu. Falls back to instant close if motion is absent. */
  const moClose = (node, done) => {
    const mo = window.MN && MN.get("motion");
    if (mo && mo.close) mo.close(node, done); else done();
  };

  const NOTE = "Backend write support coming online — changes shown in the UI won't persist to files yet.";
  const FIELDS = [
    ["title", "Title"], ["artist", "Artist"], ["album", "Album"], ["album_artist", "Album Artist"],
    ["genre", "Genre"], ["year", "Year"], ["track_no", "Track #"], ["comment", "Comment"],
  ];
  const ALBUM_KEYS = { artist: 1, album: 1, album_artist: 1, genre: 1, year: 1 };
  const ALBUM_FIELDS = FIELDS.filter((f) => ALBUM_KEYS[f[0]]);
  const FIELD_LABEL = {};
  FIELDS.forEach((f) => { FIELD_LABEL[f[0]] = f[1]; });

  let B = null, inited = false;
  let cur = null;              /* open-modal context */
  const pendTag = {}, pendArt = {};   /* id -> {cb, timer} awaiting acks */

  /* ============================================================
     CONTEXT MENU  (rich: submenus, separators, icons, danger)
       item shapes:
         { label, fn }                 -> action row
         { label, sub: [items] }       -> hover submenu
         { sep: true }                 -> separator line
       optional per item: icon (string), danger (bool)
     ============================================================ */
  let ctxEl = null;
  let openSubs = [];   /* stack of open submenu panels for cleanup */

  function hideMenu() {
    /* submenu panels drop instantly (they sit on top of the root — fading
       the whole stack reads as flicker); the ROOT panel exits gracefully.
       ctxEl is nulled BEFORE the async close so menu()'s rebuild path can
       immediately create a fresh panel while the old one fades out
       (pointer-events:none under .mo-out keeps it click-transparent). */
    openSubs.forEach((s) => s.remove());
    openSubs = [];
    if (ctxEl) {
      const old = ctxEl;
      ctxEl = null;
      moClose(old, () => old.remove());
    }
  }

  /* Position a panel within the viewport near (x,y); prefer right of anchorW. */
  function placePanel(panel, x, y) {
    const w = panel.offsetWidth || 200, h = panel.offsetHeight || 40;
    panel.style.left = Math.max(4, Math.min(x, window.innerWidth - w - 8)) + "px";
    panel.style.top = Math.max(4, Math.min(y, window.innerHeight - h - 8)) + "px";
  }

  function buildPanel(items, depth) {
    /* submenus (depth>0) carry .mn-ctx-sub: they open instantly — an
       entrance on a panel stacked over the root reads as flicker */
    const panel = el("div", depth ? "mn-ctx mn-ctx-sub" : "mn-ctx");
    let subTimer = 0, curSub = null;

    const closeSub = () => {
      if (subTimer) { clearTimeout(subTimer); subTimer = 0; }
      if (curSub) {
        const i = openSubs.indexOf(curSub);
        if (i >= 0) openSubs.splice(i, 1);
        curSub.remove(); curSub = null;
      }
    };

    items.forEach((it) => {
      if (it.sep) { panel.appendChild(el("div", "mn-ctx-sep")); return; }

      const b = el("button", "mn-ctx-item" + (it.danger ? " danger" : "") + (it.sub ? " has-sub" : ""));
      if (it.icon) b.appendChild(el("span", "mn-ctx-ico", it.icon));
      b.appendChild(el("span", "mn-ctx-txt", it.label));
      if (it.sub) b.appendChild(el("span", "mn-ctx-arrow", "›"));

      if (it.sub) {
        const openThisSub = () => {
          closeSub();
          const sp = buildPanel(it.sub, depth + 1);
          document.body.appendChild(sp);
          openSubs.push(sp);
          curSub = sp;
          const r = b.getBoundingClientRect();
          placePanel(sp, r.right - 4, r.top - 4);
        };
        b.addEventListener("mouseenter", () => {
          if (subTimer) clearTimeout(subTimer);
          subTimer = setTimeout(openThisSub, 90);
        });
        b.addEventListener("mouseleave", () => {
          if (subTimer) { clearTimeout(subTimer); subTimer = 0; }
        });
        b.addEventListener("click", (e) => { e.stopPropagation(); openThisSub(); });
      } else {
        b.addEventListener("mouseenter", closeSub);
        b.addEventListener("click", (e) => { e.stopPropagation(); hideMenu(); if (it.fn) it.fn(); });
      }
      panel.appendChild(b);
    });
    return panel;
  }

  function menu(ev, items) {
    ev.preventDefault();
    ev.stopPropagation();
    hideMenu();
    ctxEl = buildPanel(items, 0);
    document.body.appendChild(ctxEl);
    placePanel(ctxEl, ev.clientX, ev.clientY);
  }

  /* ============================================================
     MODAL
     ============================================================ */
  let ov = null, headTitle = null, artBox = null, gridEl = null, diffEl = null;
  let applyBtn = null, statusEl = null, fillRow = null, fillBtn = null, fillPath = null;
  const inputs = {};           /* field key -> input element */

  function buildModal() {
    if (ov) return;
    ov = el("div", "modal-overlay");
    ov.hidden = true;
    const box = el("div", "modal modal-tag");

    const head = el("div", "modal-head");
    headTitle = el("h2", null, "Edit tags");
    const close = el("button", "btn btn-ghost", "✕");
    close.addEventListener("click", closeModal);
    head.appendChild(headTitle); head.appendChild(close);
    box.appendChild(head);

    const body = el("div", "modal-body");

    /* cover art (artedit.js mounts here) */
    const artSec = el("div", "set-section");
    artSec.appendChild(el("div", "set-label", "Cover Art"));
    artBox = el("div", "art-sec");
    artSec.appendChild(artBox);
    body.appendChild(artSec);

    /* tag fields */
    const tagSec = el("div", "set-section");
    tagSec.appendChild(el("div", "set-label", "Tags"));
    gridEl = el("div", "tag-grid");
    tagSec.appendChild(gridEl);
    fillRow = el("div", "tag-tools");
    fillBtn = el("button", "btn-mini", "Fill from filename");
    fillBtn.addEventListener("click", fillFromFilename);
    fillPath = el("span", "tag-path", "");
    fillRow.appendChild(fillBtn); fillRow.appendChild(fillPath);
    tagSec.appendChild(fillRow);
    body.appendChild(tagSec);

    /* live diff strip */
    const diffSec = el("div", "set-section");
    diffSec.appendChild(el("div", "set-label", "Before → After"));
    diffEl = el("div", "diff-strip");
    diffSec.appendChild(diffEl);
    body.appendChild(diffSec);

    /* actions */
    const act = el("div", "tag-actions");
    applyBtn = el("button", "btn btn-accent tag-apply", "Apply");
    applyBtn.addEventListener("click", apply);
    statusEl = el("span", "tag-status", "");
    act.appendChild(applyBtn); act.appendChild(statusEl);
    body.appendChild(act);

    box.appendChild(body);
    ov.appendChild(box);
    ov.addEventListener("click", (e) => { if (e.target === ov) closeModal(); });
    document.body.appendChild(ov);
  }

  function closeModal() {
    if (ov && !ov.hidden) moClose(ov, () => { ov.hidden = true; });
    cur = null;
  }

  function buildGrid(list) {
    gridEl.innerHTML = "";
    Object.keys(inputs).forEach((k) => delete inputs[k]);
    list.forEach(([key, label]) => {
      const f = el("div", "tag-field" + (key === "comment" ? " wide" : ""));
      f.appendChild(el("label", null, label));
      const inp = document.createElement("input");
      inp.type = (key === "year" || key === "track_no") ? "number" : "text";
      inp.spellcheck = false;
      inp.addEventListener("input", renderDiff);
      f.appendChild(inp);
      gridEl.appendChild(f);
      inputs[key] = inp;
    });
  }

  function setStatus(txt, cls) {
    statusEl.textContent = txt || "";
    statusEl.className = "tag-status" + (cls ? " " + cls : "");
  }

  /* ---------- diff strip (the signature Before → After UX) ---------- */
  function currentVals() {
    const v = {};
    for (const k in inputs) v[k] = inputs[k].value.trim();
    return v;
  }
  function changedList() {
    const v = currentVals(), out = [];
    for (const k in v) {
      const o = String(cur.orig[k] == null ? "" : cur.orig[k]).trim();
      if (v[k] !== o) out.push([k, o, v[k]]);
    }
    return out;
  }
  function renderDiff() {
    if (!cur || !diffEl) return;
    /* a read-only track keeps Apply disabled regardless of edits */
    const roLock = cur.mode === "track" && cur.row && tagReadOnly(cur.row.path);
    const ch = changedList();
    diffEl.innerHTML = "";
    if (!ch.length) {
      diffEl.appendChild(el("div", "diff-empty", "No changes yet — edits preview here before they touch the files."));
      applyBtn.disabled = true;
      return;
    }
    applyBtn.disabled = roLock;
    ch.forEach(([k, o, n]) => {
      const row = el("div", "diff-row");
      row.appendChild(el("span", "d-name", FIELD_LABEL[k] || k));
      row.appendChild(el("span", "d-old", o || "∅"));
      row.appendChild(el("span", "d-arrow", "→"));
      row.appendChild(el("span", "d-new", n || "∅"));
      diffEl.appendChild(row);
    });
  }

  /* ---------- filename parsing helper ---------- */
  const FN_RES = [
    /^(\d{1,3})\s*[-_.]\s*(.+)$/,
    /^(\d{1,3})\s+(.+)$/,
    /^\[(\d{1,3})\]\s*(.+)$/,
    /^Track\s*(\d{1,3})\s*[-_.]\s*(.+)$/i,
  ];
  function fileStem(p) {
    return String(p || "").split(/[\\/]/).pop().replace(/\.[^.]+$/, "");
  }
  function parseFilename(path) {
    const s = fileStem(path).trim();
    for (const re of FN_RES) {
      const m = s.match(re);
      if (m) return { track_no: +m[1], title: m[2].replace(/_+/g, " ").trim() };
    }
    return { title: s.replace(/_+/g, " ").trim() };
  }
  function fillFromFilename() {
    if (!cur || cur.mode !== "track" || !cur.row.path) return;
    const p = parseFilename(cur.row.path);
    if (p.title && inputs.title) inputs.title.value = p.title;
    if (p.track_no && inputs.track_no) inputs.track_no.value = String(p.track_no);
    renderDiff();
  }

  /* ============================================================
     BRIDGE writes (ack-tracked; degrade gracefully with a note)
     ============================================================ */
  /* Translate the backend's machine error tokens into something a human can
     act on (they used to collapse into one generic "rejected" line). */
  function tagErrText(err) {
    switch (err || "") {
      case "unsupported-format":
        return "This file format is read-only (tags can be written to MP3, FLAC and M4A).";
      case "m4a-needs-repack":
        return "The new tags don't fit this M4A's header — an external repack is needed.";
      case "replace-failed":
        return "The file is locked by another program — close it and try again.";
      case "io-error":
        return "Could not read or write the file (disk error or permissions).";
      case "corrupt":
        return "The file's tag structure looks corrupt — no changes were made.";
      default:
        return "Backend rejected the tag write" + (err ? " (" + err + ")." : ".");
    }
  }
  /* Read-only formats: readable in the editor, not writable by the backend. */
  function tagReadOnly(path) {
    return /\.(ogg|opus|wav|aiff?|ape|wv|wma)$/i.test(path || "");
  }
  function sendTag(id, fields, cb) {
    B.send({ cmd: "tagwrite", id, fields });
    if (pendTag[id]) { clearTimeout(pendTag[id].timer); pendTag[id].cb(null); }
    pendTag[id] = { cb, timer: setTimeout(() => { delete pendTag[id]; cb(null); }, 2500) };
  }
  function sendArt(id, b64, mime, whole, done) {
    B.send({ cmd: "artwrite", id, image_b64: b64, mime, whole_album: !!whole });
    if (pendArt[id]) { clearTimeout(pendArt[id].timer); pendArt[id].cb(null); }
    pendArt[id] = {
      cb: (m) => done(m ? !!m.ok : null),
      timer: setTimeout(() => { delete pendArt[id]; done(null); }, 3500),
    };
  }

  /* ============================================================
     OPENERS
     ============================================================ */
  function openTrack(row, rowEl) {
    buildModal();
    cur = { mode: "track", row, rowEl, ids: [row.id] };
    headTitle.textContent = "Edit tags";
    buildGrid(FIELDS);
    cur.orig = {
      title: row.title || "", artist: row.artist || "", album: row.album || "",
      album_artist: row.album_artist || "", genre: row.genre || "",
      year: row.year || "", track_no: row.track_no || "", comment: row.comment || "",
    };
    for (const k in inputs) inputs[k].value = cur.orig[k] ? String(cur.orig[k]) : "";
    fillRow.hidden = false;
    fillBtn.disabled = !row.path;
    fillPath.textContent = row.path ? fileStem(row.path) : "file path not exposed by the backend yet";
    applyBtn.textContent = "Apply";
    /* read-only formats: editor opens (tags are readable) but writing them
       back isn't supported — say so up front instead of failing on Apply */
    const ro = tagReadOnly(row.path);
    applyBtn.disabled = ro;
    setStatus(ro ? "Read-only format — tags can't be saved to " +
              (row.path.split(".").pop().toUpperCase()) + " files." : "", ro ? "warn" : "");
    renderDiff();
    if (window.MnArtEdit) {
      MnArtEdit.mount(artBox, {
        artist: row.artist || "", album: row.album || "", art: row.art || "",
        /* offer whole-album apply when this track belongs to an album */
        allowWholeAlbum: !!(row.album && row.album.length),
        onApply: (b64, mime, done, whole) => sendArt(row.id, b64, mime, !!whole, done),
      });
    }
    ov.hidden = false;
  }

  function openAlbum(a, cardEl) {
    buildModal();
    cur = { mode: "album", album: a, cardEl, ids: [] };
    headTitle.textContent = "Edit album tags";
    buildGrid(ALBUM_FIELDS);
    cur.orig = {
      artist: a.artist || "", album: a.title || "", album_artist: a.artist || "",
      genre: a.genre || "", year: a.year || "",
    };
    for (const k in inputs) inputs[k].value = cur.orig[k] ? String(cur.orig[k]) : "";
    fillRow.hidden = true;
    applyBtn.textContent = "Apply to album";
    setStatus("");
    renderDiff();
    B.send({ cmd: "albumtracks", id: a.id });   /* collect the track ids to batch-write */
    if (window.MnArtEdit) {
      MnArtEdit.mount(artBox, {
        artist: a.artist || "", album: a.title || "", art: a.art || "",
        onApply: (b64, mime, done) => {
          const ctx = cur;
          const tid = ctx && ctx.ids && ctx.ids.length ? ctx.ids[0] : a.id;
          sendArt(tid, b64, mime, true, done);
          /* optimistic: refresh the album card cover */
          if (cardEl) {
            const artEl = cardEl.querySelector(".album-art");
            if (artEl) {
              let img = artEl.querySelector("img");
              if (!img) {
                img = document.createElement("img");
                artEl.classList.remove("art-ph");
                const g = artEl.querySelector(".art-glyph");
                if (g) g.remove();
                artEl.insertBefore(img, artEl.firstChild);
              }
              img.src = "data:" + mime + ";base64," + b64;
              img.classList.add("loaded");
            }
          }
        },
      });
    }
    ov.hidden = false;
  }

  /* ============================================================
     APPLY
     ============================================================ */
  function updateRowDom(rowEl, row) {
    if (!rowEl) return;
    const set = (sel, val) => { const e = rowEl.querySelector(sel); if (e) e.textContent = val; };
    set(".c-title", row.title || "Unknown title");
    set(".c-artist", row.artist || "—");
    set(".c-album", row.album || "—");
    set(".c-year", row.year ? String(row.year) : "");
    set(".c-genre", row.genre || "");
  }

  function apply() {
    if (!cur) return;
    const v = currentVals();
    setStatus("Saving…");

    if (cur.mode === "track") {
      const fields = {
        title: v.title, artist: v.artist, album: v.album, album_artist: v.album_artist,
        genre: v.genre, year: +v.year || 0, track_no: +v.track_no || 0, comment: v.comment,
      };
      sendTag(cur.row.id, fields, (m) => {
        if (m && m.ok) setStatus("Saved ✓", "ok");
        else if (m) setStatus(tagErrText(m.error), "warn");
        else setStatus(NOTE, "warn");
      });
      /* optimistic row update */
      Object.assign(cur.row, {
        title: v.title, artist: v.artist, album: v.album, album_artist: v.album_artist,
        genre: v.genre, year: +v.year || 0, track_no: +v.track_no || 0, comment: v.comment,
      });
      updateRowDom(cur.rowEl, cur.row);
      cur.orig = Object.assign({}, cur.orig, v);
      renderDiff();
      return;
    }

    /* album batch: one tagwrite per track id from the albumtracks reply.
       keep_missing is CRITICAL: these are album-level fields only — without
       it the authoritative-remove contract stripped Title/Track#/Comment
       from every file in the album. */
    const fields = { artist: v.artist, album: v.album, album_artist: v.album_artist, genre: v.genre, year: +v.year || 0, keep_missing: true };
    const ids = cur.ids && cur.ids.length ? cur.ids.slice() : [cur.album.id];
    let acked = 0, okc = 0, silent = false;
    ids.forEach((id) => sendTag(id, fields, (m) => {
      acked++;
      if (m && m.ok) okc++;
      if (!m) silent = true;
      if (acked === ids.length) {
        if (silent) setStatus(NOTE, "warn");
        else setStatus("Saved ✓  ·  " + okc + "/" + ids.length + " tracks", "ok");
      }
    }));

    /* optimistic: album card + any loaded library rows */
    if (cur.cardEl) {
      const tEl = cur.cardEl.querySelector(".album-title");
      if (tEl) tEl.textContent = v.album || "Unknown album";
      const aEl = cur.cardEl.querySelector(".album-artist");
      if (aEl) aEl.textContent = v.artist || "—";
    }
    const oldAlbum = String(cur.orig.album || ""), oldArtist = String(cur.orig.artist || "");
    (B.getTracks() || []).forEach((r) => {
      if ((r.album || "") === oldAlbum && (!oldArtist || (r.artist || "") === oldArtist)) {
        if (v.album) r.album = v.album;
        if (v.artist) r.artist = v.artist;
        if (v.genre) r.genre = v.genre;
        if (+v.year) r.year = +v.year;
      }
    });
    Object.assign(cur.album, { artist: v.artist || cur.album.artist, title: v.album || cur.album.title });
    if (+v.year) cur.album.year = +v.year;
    cur.orig = Object.assign({}, cur.orig, v);
    renderDiff();
  }

  /* ============================================================
     ACTIONS shared by the rich context menu
     ============================================================ */
  function setRating(id, stars) { B.send({ cmd: "rating", id, stars }); }
  function setLiked(id, v)       { B.send({ cmd: "like", id, v }); }
  function reveal(path)          { if (path) B.send({ cmd: "reveal", path }); }

  /* "Find more from same": drive the app search box. Genre/year search on the
     literal term; the C-side FTS matches across fields. */
  function findMore(term) {
    if (B.setSearch) B.setSearch(term == null ? "" : String(term));
  }

  /* ---------- Properties modal (read-only metadata) ---------- */
  let propOv = null, propBody = null, propTitle = null;
  function fmtBytes(n) {
    n = +n || 0;
    if (n <= 0) return "—";
    const u = ["B", "KB", "MB", "GB"]; let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i ? n.toFixed(1) : n) + " " + u[i];
  }
  function fmtDur(ms) {
    ms = +ms || 0; if (ms <= 0) return "—";
    const s = Math.round(ms / 1000), m = Math.floor(s / 60), h = Math.floor(m / 60);
    const mm = String(m % 60).padStart(2, "0"), ss = String(s % 60).padStart(2, "0");
    return h ? h + ":" + mm + ":" + ss : m + ":" + ss;
  }
  function fmtDate(unix) {
    const t = +unix || 0; if (t <= 0) return "—";
    try { return new Date(t * 1000).toLocaleString(); } catch (e) { return "—"; }
  }
  function buildPropModal() {
    if (propOv) return;
    propOv = el("div", "modal-overlay"); propOv.hidden = true;
    const box = el("div", "modal modal-props");
    const head = el("div", "modal-head");
    propTitle = el("h2", null, "Properties");
    const closeProps = () => { if (!propOv.hidden) moClose(propOv, () => { propOv.hidden = true; }); };
    const close = el("button", "btn btn-ghost", "✕");
    close.addEventListener("click", closeProps);
    head.appendChild(propTitle); head.appendChild(close);
    box.appendChild(head);
    propBody = el("div", "modal-body props-body");
    box.appendChild(propBody);
    propOv.appendChild(box);
    propOv.addEventListener("click", (e) => { if (e.target === propOv) closeProps(); });
    document.body.appendChild(propOv);
  }
  function propRow(k, v) {
    const r = el("div", "prop-row");
    r.appendChild(el("span", "prop-k", k));
    r.appendChild(el("span", "prop-v", v == null || v === "" ? "—" : String(v)));
    return r;
  }
  function showProps(row, isAlbum) {
    buildPropModal();
    propTitle.textContent = isAlbum ? "Album properties" : "Track properties";
    propBody.innerHTML = "";
    const add = (k, v) => propBody.appendChild(propRow(k, v));
    if (isAlbum) {
      add("Album", row.title || row.album);
      add("Album artist", row.artist);
      add("Year", row.year || "");
      add("Tracks", row.track_count != null ? row.track_count : "");
      add("Format", row.format);
      add("Sample rate", row.sample_rate ? (row.sample_rate / 1000) + " kHz" : "");
      add("Bit depth", row.bit_depth ? row.bit_depth + " bit" : "");
      add("Bitrate", row.bitrate || row.bitrate_kbps ? (row.bitrate || row.bitrate_kbps) + " kbps" : "");
      add("Total size", fmtBytes(row.size));
      add("Date added", fmtDate(row.date_added));
    } else {
      add("Title", row.title);
      add("Artist", row.artist);
      add("Album", row.album);
      add("Album artist", row.album_artist);
      add("Genre", row.genre);
      add("Year", row.year || "");
      add("Track #", row.track_no || "");
      add("Format", row.format);
      add("Bitrate", (row.bitrate || row.bitrate_kbps) ? ((row.bitrate || row.bitrate_kbps) + " kbps") : "");
      add("Sample rate", row.sample_rate ? (row.sample_rate / 1000) + " kHz" : "");
      add("Channels", row.channels || "");
      add("Duration", fmtDur(row.duration_ms));
      add("Size", fmtBytes(row.size));
      add("Play count", row.play_count || 0);
      add("Rating", (row.rating || 0) + " / 5");
      add("Liked", row.liked === 1 ? "👍" : row.liked === -1 ? "👎" : "—");
      add("Date added", fmtDate(row.date_added));
      const p = el("div", "prop-row prop-path");
      p.appendChild(el("span", "prop-k", "Path"));
      p.appendChild(el("span", "prop-v", row.path || "—"));
      propBody.appendChild(p);
    }
    propOv.hidden = false;
  }

  /* Folder path of a track (for album reveal / open-location fallback). */
  function folderOf(path) {
    const s = String(path || "");
    const i = Math.max(s.lastIndexOf("\\"), s.lastIndexOf("/"));
    return i >= 0 ? s.slice(0, i) : s;
  }

  /* ---------- shared menu fragments ---------- */
  function ratingSub(id) {
    const items = [];
    for (let n = 5; n >= 1; n--)
      items.push({ label: "★".repeat(n) + "☆".repeat(5 - n), fn: () => setRating(id, n) });
    items.push({ label: "No rating", fn: () => setRating(id, 0) });
    items.push({ sep: true });
    items.push({ label: "👍 Thumbs up", fn: () => setLiked(id, 1) });
    items.push({ label: "👎 Thumbs down", fn: () => setLiked(id, -1) });
    items.push({ label: "Clear thumbs", fn: () => setLiked(id, 0) });
    return items;
  }
  function findMoreSub(row) {
    const sub = [];
    if (row.artist)        sub.push({ label: "Same artist",  fn: () => findMore(row.artist) });
    if (row.album || row.title) sub.push({ label: "Same album",   fn: () => findMore(row.album || row.title) });
    if (row.genre)         sub.push({ label: "Same genre",   fn: () => findMore(row.genre) });
    if (row.year)          sub.push({ label: "Same year",    fn: () => findMore(String(row.year)) });
    if (!sub.length) sub.push({ label: "(no metadata)", fn: () => {} });
    return sub;
  }

  /* ============================================================
     PUBLIC
     ============================================================ */
  function init(api) {
    B = api;
    if (inited) return;
    inited = true;
    B.on("tagwrote", (m) => {
      if (!m || m.id == null) return;
      const p = pendTag[m.id];
      if (p) { clearTimeout(p.timer); delete pendTag[m.id]; p.cb(m); }
    });
    B.on("artwrote", (m) => {
      if (!m || m.id == null) return;
      const p = pendArt[m.id];
      if (p) { clearTimeout(p.timer); delete pendArt[m.id]; p.cb(m); }
    });
    /* the albumtracks reply also feeds app.js's expanded card — we just tap it */
    /* clipboard image reply for "Paste album art": stash the PNG, then ask
       for the album's tracks (the albumtracks tap below finishes the write) */
    B.tap("clipart", (m) => {
      if (pendingPasteAlbumId == null) return;
      if (!m || !m.ok || !m.b64) {
        pendingPasteAlbumId = null;
        if (window.__mnToast) window.__mnToast("No image on the clipboard");
        return;
      }
      pendingPasteB64 = m.b64;
      pendingPasteMime = m.mime || "image/png";
      B.send({ cmd: "albumtracks", id: pendingPasteAlbumId });
    });
    B.tap("albumtracks", (m) => {
      if (cur && cur.mode === "album" && cur.album && m.id === cur.album.id) {
        cur.ids = (m.rows || []).map((r) => r.id);
        if (ov && !ov.hidden && cur.ids.length) applyBtn.textContent = "Apply to " + cur.ids.length + " tracks";
      }
      /* "Open in Explorer" for an album: reveal its first track's file. */
      if (m && pendingRevealAlbumId != null && m.id === pendingRevealAlbumId) {
        pendingRevealAlbumId = null;
        const rows = m.rows || [];
        if (rows.length && rows[0].path) reveal(rows[0].path);
      }
      /* paste-art: clipboard PNG arrived first; now we have the album's
         tracks — write the cover to the whole album via artwrite */
      if (m && pendingPasteAlbumId != null && m.id === pendingPasteAlbumId &&
          pendingPasteB64) {
        const rows = m.rows || [];
        pendingPasteAlbumId = null;
        if (rows.length && rows[0].id) {
          B.send({ cmd: "artwrite", id: rows[0].id, image_b64: pendingPasteB64,
                   mime: pendingPasteMime || "image/png", whole_album: true });
          if (window.__mnToast) window.__mnToast("Pasting cover to whole album…");
        } else if (window.__mnToast) window.__mnToast("Album has no tracks to write to");
        pendingPasteB64 = pendingPasteMime = null;
      }
      /* batch stem export for a whole album */
      if (m && pendingStemAlbumId != null && m.id === pendingStemAlbumId) {
        pendingStemAlbumId = null;
        const sx = window.__mnStemExport;
        const rows = m.rows || [];
        const tracks = rows.map((r) => sx.fromRow(r)).filter(Boolean);
        if (!tracks.length) { if (window.__mnToast) window.__mnToast("Album has no exportable tracks"); return; }
        sx.run(tracks, tracks.length + " tracks");
      }
    });
    document.addEventListener("mousedown", (e) => {
      if (ctxEl && !ctxEl.contains(e.target)) hideMenu();
    }, true);
    window.addEventListener("blur", hideMenu);
    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape") return;
      if (ctxEl) { hideMenu(); return; }
      if (ov && !ov.hidden) closeModal();
    });
  }

  function trackMenu(ev, row, rowEl) {
    const id = row.id;
    menu(ev, [
      { icon: "▶", label: "Play now",        fn: () => B.send({ cmd: "play", id }) },
      { icon: "⏭", label: "Queue next",      fn: () => B.send({ cmd: "queuenext", id }) },
      { icon: "＋", label: "Queue last",      fn: () => B.send({ cmd: "queuelast", id }) },
      { icon: "🎵", label: "Add to playlist…", fn: () => { if (typeof window.__mnAddToPlaylist === "function") window.__mnAddToPlaylist(id, ev); } },
      { sep: true },
      { icon: "🔀", label: "Play shuffled",   fn: () => { B.send({ cmd: "shuffle", on: true }); B.send({ cmd: "play", id }); } },
      { icon: "🔎", label: "Find more from same", sub: findMoreSub(row) },
      { icon: "★", label: "My Rating",       sub: ratingSub(id) },
      { sep: true },
      { icon: "✎", label: "Edit tags…",       fn: () => openTrack(row, rowEl) },
      { icon: "🅰", label: "Tag from filename", fn: () => { openTrack(row, rowEl); setTimeout(fillFromFilename, 60); } },
      { icon: "ℹ", label: "Properties",       fn: () => showProps(row, false) },
      { sep: true },
      { icon: "⇄", label: "Convert format…",  fn: () => B.send({ cmd: "convert", id }) },
      { icon: "⑂", label: "Export stems…",    fn: () => {
          const sx = window.__mnStemExport;
          if (!sx) { if (window.__mnToast) window.__mnToast("Stem export unavailable"); return; }
          sx.run([sx.fromRow(row)], row.title || row.path);
        } },
      { icon: "📂", label: "Open file location", fn: () => reveal(row.path) },
      { sep: true },
      { icon: "🗑", label: "Remove from library", danger: true,
        fn: () => B.send({ cmd: "removetrack", id }) },
    ]);
  }

  function albumMenu(ev, album, cardEl) {
    const aid = album.id;
    menu(ev, [
      { icon: "▶", label: "Play now",        fn: () => B.send({ cmd: "playalbum", id: aid }) },
      { icon: "⏭", label: "Queue next",      fn: () => B.send({ cmd: "queuenext", albumid: aid }) },
      { icon: "＋", label: "Queue last",      fn: () => B.send({ cmd: "queuelast", albumid: aid }) },
      { sep: true },
      { icon: "🔀", label: "Play shuffled",   fn: () => { B.send({ cmd: "shuffle", on: true }); B.send({ cmd: "playalbum", id: aid }); } },
      { icon: "🔀", label: "Play shuffled (by Album)", fn: () => { B.send({ cmd: "shuffle", on: true }); B.send({ cmd: "playalbum", id: aid }); } },
      { icon: "🔎", label: "Find more from same", sub: findMoreSub({ artist: album.artist, album: album.title, genre: album.genre, year: album.year }) },
      { sep: true },
      { icon: "✎", label: "Edit tags…",       fn: () => openAlbum(album, cardEl) },
      { icon: "ℹ", label: "Properties",       fn: () => showProps(album, true) },
      { sep: true },
      { icon: "🌐", label: "Find better art online…",
        fn: () => {
          if (typeof window.__mnToast === "function") window.__mnToast("Searching cover art…");
          B.send({ cmd: "artfetch", artist: album.artist || "", album: album.title || "",
                   res: +(localStorage.getItem("mn.artfetchres") || 1200) });
        } },
      { icon: "📋", label: "Paste album art",
        fn: () => {
          pendingPasteAlbumId = aid;
          B.send({ cmd: "clipart" });
        } },
      { icon: "🗺", label: "Regenerate depth map",
        fn: () => {
          B.send({ cmd: "redepth", artist: album.artist || "", album: album.title || "" });
          if (typeof window.__mnToast === "function") window.__mnToast("Depth map queued for regeneration");
        } },
      { sep: true },
      { icon: "⇄", label: "Convert format…",  fn: () => B.send({ cmd: "convert", albumid: aid }) },
      { icon: "⑂", label: "Export stems…",    fn: () => stemExportAlbum(album) },
      { icon: "📂", label: "Open in Explorer", fn: () => revealAlbum(album) },
    ]);
  }

  /* Batch stem export for a whole album: fetch its tracks, then hand the rows
     to the shared exporter (one .mnstem per track). Consumed by the albumtracks
     tap in init() via pendingStemAlbumId. */
  let pendingStemAlbumId = null;
  /* "Paste album art" flow: album awaiting a clipboard image + the image */
  let pendingPasteAlbumId = null, pendingPasteB64 = null, pendingPasteMime = null;
  function stemExportAlbum(album) {
    if (!window.__mnStemExport) {
      if (window.__mnToast) window.__mnToast("Stem export unavailable");
      return;
    }
    pendingStemAlbumId = album.id;
    if (window.__mnToast) window.__mnToast("Preparing stem export…");
    B.send({ cmd: "albumtracks", id: album.id });
  }

  /* Reveal an album's folder: ask the app for the album's tracks, then reveal
     the first track's path (selects the file in its folder). A single tap
     registered in init() consumes the reply for the pending album id. */
  let pendingRevealAlbumId = null;
  function revealAlbum(album) {
    pendingRevealAlbumId = album.id;
    B.send({ cmd: "albumtracks", id: album.id });
  }

  return { init, trackMenu, albumMenu };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("tagedit", "1.0.0", [], function () { return window.MnTagEdit || {}; });
