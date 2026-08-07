/* ============================================================
   MONATOMIC — Streams view (saved internet streams)
   Desktop port of the Android StreamsPane + Streams store.
   Also the save-target for the Radio pane's favorites: the
   star calls MnStreams.add(name, url) and computes saved-state
   from MnStreams.urls().
   Persistence: ONE JSON doc {"items":[{id,name,url}]} stored
   backend-side via onlinesave/onlineload under the name
   "streams" (shared across app instances).
   app.js opens this view via MnStreams.open(modApi); call
   MnStreams.init(modApi) early at boot so the store is loaded
   before the Radio pane needs urls().
   ============================================================ */
window.MnStreams = (function () {
  "use strict";

  const STORE = "streams";

  /* ---------- helpers ---------- */
  const el = (tag, cls, txt) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt != null) e.textContent = txt;   /* remote/user strings only ever land here */
    return e;
  };
  function hostOf(url) {
    try { const h = new URL(url).host; if (h) return h; } catch (_) {}
    const m = String(url || "").match(/^[a-z][a-z0-9+.\-]*:\/\/([^\/?#]+)/i);
    return m ? m[1] : String(url || "");
  }
  /* broadcast glyph (static markup, no interpolation) */
  const BCAST_SVG =
    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="2" fill="currentColor" stroke="none"></circle>' +
    '<path d="M8.46 15.54a5 5 0 0 1 0-7.08"></path>' +
    '<path d="M15.54 8.46a5 5 0 0 1 0 7.08"></path>' +
    '<path d="M5.64 18.36a9 9 0 0 1 0-12.72"></path>' +
    '<path d="M18.36 5.64a9 9 0 0 1 0 12.72"></path>' +
    "</svg>";
  function bcastIcon(cls) {
    const s = el("span", cls);
    s.innerHTML = BCAST_SVG;
    return s;
  }

  /* ---------- state ---------- */
  let bridge = null;      /* modApi from app.js                          */
  let inited = false;     /* taps registered + first onlineload sent     */
  let loaded = false;     /* first onlinefile reply (or timeout) landed  */
  let root = null;        /* #view-streams                               */
  let listEl = null;      /* scrolling list container                    */
  let items = [];         /* [{id:number, name, url}] sorted by name     */
  let nowUrl = "";        /* lowercased online_url currently playing     */
  let pending = null;     /* {id,url} play awaiting its streamres        */
  let err = null;         /* {id,msg} inline play error for one row      */
  let ctxEl = null;       /* open context menu                           */
  let modalEl = null;     /* open add/rename dialog overlay              */

  /* ============================================================
     STORE — load / normalize / persist
     ============================================================ */
  function sortItems() {
    items.sort((a, b) => {
      const x = a.name.toLowerCase(), y = b.name.toLowerCase();
      return x < y ? -1 : x > y ? 1 : 0;
    });
  }
  function persist() {
    if (!bridge) return;
    bridge.send({ cmd: "onlinesave", name: STORE, text: JSON.stringify({ items }) });
  }
  function onOnlineFile(m) {
    if (!m || m.name !== STORE) return;   /* multi-consumer type: filter by name */
    let doc = null;
    try { doc = JSON.parse(m.text || "null"); } catch (_) {}
    const list = (doc && Array.isArray(doc.items)) ? doc.items : [];
    items = [];
    let hi = 0;
    list.forEach((it) => {
      if (!it || typeof it.url !== "string" || !it.url) return;
      const id = (typeof it.id === "number" && isFinite(it.id) && it.id > 0) ? Math.floor(it.id) : hi + 1;
      const name = (typeof it.name === "string" && it.name.trim()) ? it.name.trim() : hostOf(it.url);
      items.push({ id, name, url: it.url });
      if (id > hi) hi = id;
    });
    sortItems();
    loaded = true;
    renderList();
  }

  /* trim; prepend http:// to bare hosts; ONLY http/https survive */
  function normalizeUrl(raw) {
    let u = String(raw == null ? "" : raw).trim();
    if (!u) return null;
    if (/^https?:\/\//i.test(u)) return u;
    if (u.indexOf("://") !== -1) return null;             /* other :// scheme */
    if (/^[a-z][a-z0-9+.\-]*:/i.test(u)) return null;     /* mailto:, file:, … */
    return "http://" + u;                                 /* bare host        */
  }
  function nextId() {
    let n = 1;
    items.forEach((i) => { if (i.id >= n) n = i.id + 1; });
    return n;
  }

  /* ---------- public store API (used by the Radio pane's star) ---------- */
  function add(name, url) {
    const u = normalizeUrl(url);
    if (!u) return false;
    const key = u.toLowerCase();
    if (items.some((i) => i.url.toLowerCase() === key)) return false;  /* de-dupe */
    let n = String(name == null ? "" : name).trim();
    if (!n) n = hostOf(u);
    items.push({ id: nextId(), name: n, url: u });
    sortItems();
    persist();
    renderList();
    return true;
  }
  function urls() {
    return new Set(items.map((i) => i.url.toLowerCase()));
  }
  function remove(id) {
    const before = items.length;
    items = items.filter((i) => i.id !== id);
    if (items.length === before) return;
    if (err && err.id === id) err = null;
    persist();
    renderList();
  }
  function rename(id, name) {
    const it = items.find((i) => i.id === id);
    if (!it) return;
    const n = String(name == null ? "" : name).trim();
    it.name = n || hostOf(it.url);
    sortItems();
    persist();
    renderList();
  }

  /* ============================================================
     PLAYBACK (bridge: -> streamplay, <- streamres / now)
     ============================================================ */
  function playItem(it) {
    if (!bridge) return;
    err = null;
    pending = { id: it.id, url: it.url };
    bridge.send({
      cmd: "streamplay",
      url: it.url,
      title: it.name,
      artist: hostOf(it.url),
      kind: "stream",
    });
    renderList();
  }
  function onStreamRes(m) {
    if (!m || !pending) return;
    /* filter: only react to the reply for OUR request (url echo) */
    const mu = m.url ? String(m.url).toLowerCase() : "";
    if (mu && mu !== pending.url.toLowerCase()) return;
    const p = pending;
    pending = null;
    err = m.ok ? null : { id: p.id, msg: String(m.error || "stream failed to start") };
    renderList();
  }
  function onNow(n) {
    /* lite 'now' ticks omit the online fields — ignore those */
    if (!n || typeof n.online === "undefined") return;
    const u = (n.online && n.online_url) ? String(n.online_url).toLowerCase() : "";
    if (u === nowUrl) return;
    nowUrl = u;
    updatePlayingRows();
  }
  function updatePlayingRows() {
    if (!listEl) return;
    listEl.querySelectorAll(".strm-row").forEach((r) => {
      r.classList.toggle("playing", !!nowUrl && r.dataset.url === nowUrl);
    });
  }

  /* ============================================================
     CONTEXT MENU (desktop right-click = Android long-press)
     ============================================================ */
  function closeCtx() {
    if (!ctxEl) return;
    ctxEl.remove();
    ctxEl = null;
    document.removeEventListener("pointerdown", ctxAway, true);
    document.removeEventListener("keydown", ctxKey, true);
  }
  function ctxAway(e) { if (ctxEl && !ctxEl.contains(e.target)) closeCtx(); }
  function ctxKey(e) { if (e.key === "Escape") { e.stopPropagation(); closeCtx(); } }
  function openCtx(x, y, it) {
    closeCtx();
    const menu = el("div", "strm-ctx");
    menu.appendChild(el("div", "strm-ctx-head", it.name));
    const mk = (label, danger, fn) => {
      const b = el("button", "strm-ctx-item" + (danger ? " danger" : ""), label);
      b.addEventListener("click", () => { closeCtx(); fn(); });
      menu.appendChild(b);
    };
    mk("Play",   false, () => playItem(it));
    mk("Rename", false, () => openDialog("rename", it));
    mk("Delete", true,  () => remove(it.id));
    document.body.appendChild(menu);
    const r = menu.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(x, window.innerWidth  - r.width  - 8)) + "px";
    menu.style.top  = Math.max(8, Math.min(y, window.innerHeight - r.height - 8)) + "px";
    ctxEl = menu;
    document.addEventListener("pointerdown", ctxAway, true);
    document.addEventListener("keydown", ctxKey, true);
  }

  /* ============================================================
     ADD / RENAME DIALOG
     Add:    Save enabled when URL non-blank.
     Rename: URL locked, Save enabled when Name non-blank.
     ============================================================ */
  function closeModal() {
    if (!modalEl) return;
    modalEl.remove();
    modalEl = null;
  }
  function openDialog(mode, it) {
    closeCtx();
    closeModal();
    const isAdd = mode === "add";

    const ov = el("div", "strm-modal-ov");
    const card = el("div", "strm-modal");
    const head = el("div", "strm-modal-head");
    head.appendChild(el("h2", null, isAdd ? "Add stream" : "Rename stream"));
    card.appendChild(head);

    const body = el("div", "strm-modal-body");

    const f1 = el("label", "strm-field");
    f1.appendChild(el("span", "strm-field-k", "Name"));
    const inName = document.createElement("input");
    inName.type = "text";
    inName.placeholder = "My radio station";
    inName.value = it ? it.name : "";
    f1.appendChild(inName);
    body.appendChild(f1);

    const f2 = el("label", "strm-field");
    f2.appendChild(el("span", "strm-field-k", "Stream URL"));
    const inUrl = document.createElement("input");
    inUrl.type = "text";
    inUrl.placeholder = "https://…";
    inUrl.value = it ? it.url : "";
    if (!isAdd) inUrl.disabled = true;   /* rename locks the URL */
    f2.appendChild(inUrl);
    body.appendChild(f2);

    const errLine = el("div", "strm-modal-err");
    body.appendChild(errLine);

    const acts = el("div", "strm-modal-acts");
    const bCancel = el("button", "strm-btn ghost", "Cancel");
    const bSave = el("button", "strm-btn save", "Save");
    acts.appendChild(bCancel);
    acts.appendChild(bSave);
    body.appendChild(acts);
    card.appendChild(body);
    ov.appendChild(card);

    const syncSave = () => {
      bSave.disabled = isAdd ? !inUrl.value.trim() : !inName.value.trim();
    };
    syncSave();
    inName.addEventListener("input", () => { errLine.textContent = ""; syncSave(); });
    inUrl.addEventListener("input", () => { errLine.textContent = ""; syncSave(); });

    const submit = () => {
      if (bSave.disabled) return;
      if (isAdd) {
        const norm = normalizeUrl(inUrl.value);
        if (!norm) { errLine.textContent = "Enter a valid http(s) URL."; return; }
        if (urls().has(norm.toLowerCase())) { errLine.textContent = "That URL is already in your streams."; return; }
        add(inName.value, inUrl.value);
      } else {
        rename(it.id, inName.value);
      }
      closeModal();
    };
    bSave.addEventListener("click", submit);
    bCancel.addEventListener("click", closeModal);
    ov.addEventListener("pointerdown", (e) => { if (e.target === ov) closeModal(); });
    /* keep the app's global hotkeys away from the dialog's typing */
    card.addEventListener("keydown", (e) => {
      e.stopPropagation();
      if (e.key === "Enter") submit();
      else if (e.key === "Escape") closeModal();
    });

    document.body.appendChild(ov);
    modalEl = ov;
    inName.focus();
    if (!isAdd) inName.select();
  }

  /* ============================================================
     RENDER
     ============================================================ */
  function render() {
    if (!root) return;
    root.innerHTML = "";
    const wrap = el("div", "strm-wrap");
    listEl = el("div", "strm-scroll");
    wrap.appendChild(listEl);

    const fab = el("button", "strm-fab", "+");
    fab.title = "Add stream";
    fab.setAttribute("aria-label", "Add stream");
    fab.addEventListener("click", () => openDialog("add", null));
    wrap.appendChild(fab);

    root.appendChild(wrap);
    renderList();
  }

  function renderList() {
    if (!listEl) return;          /* pane not built yet — store API still works */
    listEl.innerHTML = "";
    if (!loaded) return;          /* waiting for the first onlineload reply */

    if (!items.length) {
      const em = el("div", "strm-empty");
      em.appendChild(bcastIcon("strm-empty-ico"));
      em.appendChild(el("div", "strm-empty-t", "No streams yet"));
      em.appendChild(el("div", "strm-empty-s", "Add an internet-radio or direct audio URL to play it here."));
      listEl.appendChild(em);
      return;
    }

    items.forEach((it) => {
      const row = el("div", "strm-row");
      row.dataset.url = it.url.toLowerCase();
      row.tabIndex = 0;
      if (nowUrl && row.dataset.url === nowUrl) row.classList.add("playing");

      row.appendChild(bcastIcon("strm-ico"));

      const txt = el("div", "strm-txt");
      txt.appendChild(el("div", "strm-name", it.name));
      txt.appendChild(el("div", "strm-url", it.url));
      if (err && err.id === it.id) {
        const e2 = el("div", "strm-err", "Can't play — " + err.msg);
        e2.title = err.msg;
        txt.appendChild(e2);
      }
      row.appendChild(txt);

      row.addEventListener("click", () => playItem(it));
      row.addEventListener("keydown", (e) => { if (e.key === "Enter") playItem(it); });
      row.addEventListener("contextmenu", (e) => {
        e.preventDefault();
        e.stopPropagation();
        openCtx(e.clientX, e.clientY, it);
      });
      listEl.appendChild(row);
    });
  }

  /* ============================================================
     INIT / OPEN
     init(modApi): call once early at boot — loads the store so
     the Radio pane's stars have urls() before this pane is ever
     opened. open() auto-inits if init was never called.
     ============================================================ */
  function init(b) {
    if (b) bridge = b;
    if (inited || !bridge) return;
    inited = true;
    bridge.tap("onlinefile", onOnlineFile);   /* NEVER on(): multi-consumer types */
    bridge.tap("now", onNow);
    bridge.tap("streamres", onStreamRes);
    bridge.send({ cmd: "onlineload", name: STORE });
    /* backend never answers (cmd unwired / no file): settle to empty */
    setTimeout(() => { if (!loaded) { loaded = true; renderList(); } }, 1500);
  }

  function open(b) {
    if (b) bridge = b;
    const wasInited = inited;
    init(bridge);
    root = document.getElementById("view-streams");
    render();
    /* refresh from disk on re-open — the store is shared across instances */
    if (wasInited && loaded && bridge) bridge.send({ cmd: "onlineload", name: STORE });
  }

  return { init, open, add, urls, remove, rename };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("streams", "1.0.0", [], function () { return window.MnStreams || {}; });
