/* ============================================================
   MONATOMIC — Music Player UI logic
   Talks to the C core over the CEF message bridge.
   ============================================================ */
(function () {
  "use strict";
  window.addEventListener("error", (e) => {
    try { const b=document.getElementById("bridge-error"); if(b){b.hidden=false; b.textContent="JS ERROR: "+(e.message||e.error)+" @"+(e.filename||"").split("/").pop()+":"+e.lineno;} } catch(_){}
  });

  /* ---------- tiny helpers ---------- */
  const $  = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const esc = (s) => (s == null ? "" : String(s));

  /* A clickable metadata value (artist/album/genre): clicking it searches the
     library for that term so you can see all of that artist's/album's work.
     `cls` is the base class; the element gets `meta-link` added for styling. */
  /* Meta links are ENTITY-AWARE (kind: "artist" | "album" | null):
     an artist link opens the Albums view scoped to that artist; an album
     link opens THE album (expanded, in Albums view). Only kind-less links
     fall back to a plain text search. This is what keeps click context —
     "artist" clicks never dump you into a fuzzy everything-matches list. */
  function metaLinkWire(e, q, kind, artist) {
    if (!q) return e;
    e.title = kind === "artist" ? "Show albums by " + q
            : kind === "album"  ? "Open album: " + q
            : "Show all: " + q;
    e.addEventListener("click", (ev) => {
      ev.stopPropagation();
      if (kind === "artist")      navGoAlbums(String(q));
      else if (kind === "album")  navGoAlbum(String(q), artist || "");
      else                        runSearch(String(q));
    });
    return e;
  }
  function metaLink(cls, value, term, kind, artist) {
    const e = el("div", cls + " meta-link", esc(value) || "—");
    return metaLinkWire(e, term != null ? term : value, kind, artist);
  }
  /* inline (span) variant for text that sits next to other text */
  function metaLinkSpan(value, term, kind, artist) {
    const e = el("span", "meta-link", esc(value));
    return metaLinkWire(e, term != null ? term : value, kind, artist);
  }

  function fmtTime(ms) {
    if (!ms || ms < 0 || !isFinite(ms)) return "0:00";
    const t = Math.floor(ms / 1000), m = Math.floor(t / 60), s = t % 60;
    return m + ":" + String(s).padStart(2, "0");
  }

  const STEM_NAMES = ["Sub Bass", "Bass", "Vocals", "Lead", "Instruments", "Wide", "Air", "Guitar", "Piano"];
  const VIEW_TITLES = ["Tracks", "Albums", "Artists", "Genres", "Folders", "Playlists"];

  /* ============================================================
     BRIDGE — postMessage to C, listen for replies. No mock:
     if the bridge is missing, surface a loud error banner.
     ============================================================ */
  const hasBridge = !!(window.chrome && window.chrome.webview);
  const handlers = {}; // type -> fn(msg)  (primary, one per type)
  const taps = {};     // type -> [fns]    (secondary observers for UI modules)

  if (!hasBridge) { const b = $("#bridge-error"); if (b) b.hidden = false; }

  function send(obj) {
    if (hasBridge) window.chrome.webview.postMessage(JSON.stringify(obj));
  }
  function on(type, fn) { handlers[type] = fn; }
  function tap(type, fn) { (taps[type] || (taps[type] = [])).push(fn); }
  function dispatch(msg) {
    if (!msg || !msg.type) return;
    if (msg.type === "now") {
      if (msg.track_title == null && msg.title != null) msg.track_title = msg.title;
      if (msg.track_artist == null && msg.artist != null) msg.track_artist = msg.artist;
      if (msg.track_album == null && msg.album != null) msg.track_album = msg.album;
    }
    /* CONFINE handler failures: an exception in one handler must not kill
       the taps behind it (a repeatable throw in on("now") would otherwise
       take down the whole playback UI at 4 Hz). */
    const h = handlers[msg.type];
    if (h) { try { h(msg); } catch (err) { console.error("handler(" + msg.type + ")", err); } }
    const t = taps[msg.type];
    if (t) for (const fn of t) {
      try { fn(msg); } catch (err) { console.error("tap(" + msg.type + ")", err); }
    }
  }
  if (hasBridge) {
    window.chrome.webview.addEventListener("message", (e) => {
      let msg = e.data;
      if (typeof msg === "string") { try { msg = JSON.parse(msg); } catch (_) { return; } }
      dispatch(msg);
    });
  }

  /* ============================================================
     THEME (manual, persisted in localStorage)
     ============================================================ */
  const THEME_KEY = "mn.theme";
  function loadTheme() {
    let t = {};
    try { t = JSON.parse(localStorage.getItem(THEME_KEY) || "{}"); } catch (_) {}
    return { bg: t.bg || "black", accent: t.accent || "duo", art3d: t.art3d !== false, motion: t.motion || "pointer" };
  }
  function applyTheme(t) {
    document.documentElement.dataset.theme  = t.bg;
    document.documentElement.dataset.accent = t.accent;
    localStorage.setItem(THEME_KEY, JSON.stringify(t));
    $$("#theme-bg .chip").forEach((c) => c.classList.toggle("on", c.dataset.bg === t.bg));
    $$("#theme-accent .chip").forEach((c) => c.classList.toggle("on", c.dataset.accent === t.accent));
    $$("#art-motion .chip").forEach((c) => c.classList.toggle("on", c.dataset.motion === t.motion));
    const a3 = $("#set-art3d"); if (a3) a3.checked = t.art3d;
  }
  let theme = loadTheme();
  applyTheme(theme);

  /* ============================================================
     LOCAL UI PREFS (persisted under mn.* keys)
     ============================================================ */
  const PREFS = {
    volstep:  clamp(+(localStorage.getItem("mn.volstep") || 5) || 5, 1, 5),
    remaining: localStorage.getItem("mn.remaining") === "1",
    newdays:  clamp(+(localStorage.getItem("mn.newdays") || 7) || 7, 1, 60),
    anim:     localStorage.getItem("mn.anim") !== "0",
    uiscale:  clamp(+(localStorage.getItem("mn.uiscale") || 100) || 100, 85, 125),
    dockidle: localStorage.getItem("mn.dockidle") === "1",
  };
  function savePref(k, v) { localStorage.setItem("mn." + k, String(v)); }
  function applyPrefs() {
    document.documentElement.classList.toggle("no-anim", !PREFS.anim);
    document.documentElement.style.fontSize = PREFS.uiscale === 100 ? "" : PREFS.uiscale + "%";
  }
  applyPrefs();

  /* Lightweight toast (bottom-center), used for one-shot action feedback. */
  let _toastTimer = null, _toastRmTimer = null;
  window.__mnToast = function (msg) {
    let t = document.getElementById("mn-toast");
    if (!t) { t = document.createElement("div"); t.id = "mn-toast"; t.className = "mn-toast"; document.body.appendChild(t); }
    t.textContent = msg;
    t.classList.remove("out"); void t.offsetWidth; t.classList.add("in");
    if (_toastTimer) clearTimeout(_toastTimer);
    if (_toastRmTimer) clearTimeout(_toastRmTimer);   /* a fading toast's removal
       must not delete the element out from under this new one */
    _toastTimer = setTimeout(() => {
      t.classList.remove("in"); t.classList.add("out");
      _toastRmTimer = setTimeout(() => { const x = document.getElementById("mn-toast"); if (x) x.remove(); }, 320);
    }, 2600);
  };

  /* "recently added" window for the NEW badges (date_added = unix seconds) */
  function isNew(ts) {
    return !!ts && (Date.now() / 1000 - ts) < PREFS.newdays * 86400;
  }

  /* ============================================================
     STATE
     ============================================================ */
  const state = {
    view: 0,
    query: "",
    searchOpen: false,   /* unified results panel visible */
    sort: "title",
    trkTotal: 0, trkRows: [], trkLoading: false, trkDone: false, trkOffset: 0,
    albTotal: 0, albCards: [], albLoading: false, albDone: false, albOffset: 0,
    expandedAlbum: null, _expandIdx: -1, _expandH: 0,
    now: null,
    seekDrag: false, volDrag: false,
    scanActive: false,
    pickedPath: "",
    category: null,        /* "liked" | "added" | "played" | "recent" | null */
    categoryFilter: null,  /* client-side row filter (e.g. liked===1) */
    sortAsc: true,         /* column-header sort direction */
  };
  const PAGE = 100, APAGE = 60;

  /* ============================================================
     ELEMENTS
     ============================================================ */
  const E = {
    app: $("#app"),
    trackRows: $("#track-rows"),
    trackScroll: $("#track-scroll"),
    trackSentinel: $("#track-sentinel"),
    albumGrid: $("#album-grid"),
    albumSentinel: $("#album-sentinel"),
    viewTitle: $("#view-title"),
    search: $("#search"),
    sort: $("#sort"),
    plArt: $("#pl-art"), plTitle: $("#pl-title"), plArtist: $("#pl-artist"), plFormat: $("#pl-format"),
    btnPlay: $("#btn-play"), playGlyph: $("#play-glyph"),
    btnPrev: $("#btn-prev"), btnNext: $("#btn-next"),
    btnShuffle: $("#btn-shuffle"), btnRepeat: $("#btn-repeat"),
    seekTrack: $("#seek-track"), seekKnob: $("#seek-knob"),
    tElapsed: $("#t-elapsed"), tTotal: $("#t-total"),
    volTrack: $("#vol-track"), volFill: $("#vol-fill"), volKnob: $("#vol-knob"),
    npArt: $("#np-art"), npArt3d: $("#np-art-3d"), npArtStage: $("#np-art-stage"), npGlare: $("#np-art-glare"),
    npTitle: $("#np-title"), npArtist: $("#np-artist"), npAlbum: $("#np-album"), npQueue: $("#np-queue"),
    scanCard: $("#scan-card"), scanCount: $("#scan-count"), scanBar: $("#scan-bar"),
    scanSource: $("#scan-source"), scanDirs: $("#scan-dirs"), scanSkipped: $("#scan-skipped"), scanErrors: $("#scan-errors"),
    plStatus: $("#pl-status"), plStatusText: $("#pl-status-text"), plStatusFill: $("#pl-status-fill"),
    stemDock: $("#stem-dock"), stemFaders: $("#stem-faders"),
    stemEnable: $("#stem-enable"), stemPass: $("#stem-passthrough"), stemStatus: $("#stem-status"),
    btnStems: $("#btn-stems"),
  };

  /* ============================================================
     ALBUM ART helper
     ============================================================ */
  /* Thumbnails are square, right-sized PNG/JPEGs (album_art_size, default
     256). Giving the <img> intrinsic width/height lets the browser reserve
     the box WITHOUT decoding — so a card scrolling in from the
     content-visibility skipped state doesn't force a layout + re-decode
     per card (the main scroll-crawl cost). Updated from the settings emit. */
  let artThumbPx = 256;
  /* URLs that exhausted their retries this session. A missing-art album that
     scrolls in and out of the virtual grid used to re-fire the full 6-retry
     404 cycle on every re-render (wasted requests + placeholder flicker);
     once an URL is known-dead we skip straight to the placeholder. Bounded
     so a giant library can't grow it without limit. */
  /* Art that failed to load is remembered with a TIMESTAMP, not permanently.
     Covers are encoded async on a C worker pool with a bounded (drop-if-full)
     queue — a fast first-browse of a large library can outrun the encoder, so
     a cover that 404s NOW is very often ready seconds later. A permanent
     "dead" set left those covers blank until app restart. Instead we cool down
     for ART_DEAD_MS and then allow a re-probe on the next render. */
  const ART_DEAD_MS = 20000;
  const artDead = new Map();          /* url -> epoch ms it was marked dead */
  function markArtDead(url) {
    if (!url) return;
    if (artDead.size > 6000) artDead.clear();   /* cap; re-probe after churn */
    artDead.set(url, performance.now());
  }
  function artIsDead(url) {
    const t = artDead.get(url);
    if (t == null) return false;
    if (performance.now() - t > ART_DEAD_MS) { artDead.delete(url); return false; }
    return true;                       /* still cooling down */
  }
  /* Remove only the nodes setArt itself manages (the cover <img> / the ♪
     glyph). Surface-added overlays (format/count chips, NEW badge, the play
     button) survive a repaint — an artready swap must not strip a live grid
     card down to bare art (innerHTML="" did exactly that). */
  function artStrip(container) {
    for (const n of Array.from(container.children)) {
      if (n.tagName === "IMG" || n.classList.contains("art-glyph")) n.remove();
    }
  }
  function artPlaceholder(container, glyphSize) {
    container.classList.add("art-ph");
    const g = el("span", "art-glyph", "♪");
    if (glyphSize) g.style.fontSize = glyphSize;
    artStrip(container);
    container.prepend(g);
  }
  /* 3-STATE TILE (one-store serving contract):
       CACHED  — url present. The C side only emits URLs for files that
                 existed at emit time, so the <img> load is expected to
                 succeed; the 2 remaining retries are a belt for AV-lock
                 transients only, NOT a race-recovery mechanism.
       PENDING — url == "" with an artKey: the intentional ♪ placeholder,
                 painted synchronously with ZERO retries. data-art-key marks
                 the tile so a later {"type":"artready"} batch can re-run
                 setArt on exactly this tile (targeted repaint, no global
                 refresh, no timers).
       NONE    — url == "" and no artready will ever come (recorded artless
                 album): the same placeholder, terminal. */
  function setArt(container, url, glyphSize, artKey) {
    /* "painted" distinguishes a container setArt has actually touched from a
       FRESH one: dataset.artUrl is only ever assigned below, so undefined ⇒
       never painted. Without this, a fresh tile with url=="" matched the
       no-churn memo (""==="") and early-returned BEFORE artPlaceholder ran —
       every PENDING/NONE tile rendered as an empty transparent box (the v1
       blank-black-tile bug, resurfaced). */
    const painted = container.dataset.artUrl !== undefined;
    const cur = container.dataset.artUrl || "";
    if (glyphSize) container.dataset.artGlyph = glyphSize;
    if (painted && cur === (url || "")) { // no churn if unchanged…
      if (artKey) container.dataset.artKey = artKey;  // …but keep the tile
      return;                          // targetable by artready after rebinds
    }
    container.dataset.artUrl = url || "";
    artStrip(container);
    if (url && artIsDead(url)) {       /* recently-failed: cooling down, no re-fire */
      if (artKey) container.dataset.artKey = artKey; /* artready can still heal it */
      artPlaceholder(container, glyphSize);
      return;
    }
    if (url) {
      /* CACHED tiles KEEP their data-art-key (when the caller supplies one):
         an artready carrying a fresh ?g= generation (art replaced via
         tag-edit/artfetch) must be able to target and swap them too, not
         only pending placeholders. */
      if (artKey) container.dataset.artKey = artKey;
      else delete container.dataset.artKey;
      container.classList.remove("art-ph");
      const img = el("img");
      /* EAGER, not lazy: every surface that uses setArt (the virtual album
         grid, virtual queue, windowed track list, now-playing) only keeps
         VISIBLE items in the DOM — so there are never off-screen <img>s to
         defer. Native loading="lazy" on the grid's absolutely-positioned pool
         cards (over a ~340k-px spacer) mis-judged intersection at first paint
         and left in-view covers UNFETCHED until a re-layout (tab switch) — the
         "blank art at launch" bug. Eager loads them immediately. */
      img.loading = "eager";
      img.fetchPriority = "high";
      img.decoding = "async";   /* decode off the main thread */
      img.width = artThumbPx;   /* intrinsic size: no layout shift / re-decode */
      img.height = artThumbPx;
      img.alt = "";
      /* Start FADED (opacity:0) only for a fresh fetch; onload flips it to
         .loaded → visible. But the base img (without .fade) is visible, so a
         cache-hit or a deferred-paint card is NEVER trapped invisible. */
      img.classList.add("fade");
      let tries = 0;
      img.onload = () => { img.classList.add("loaded"); artDead.delete(url); };
      img.onerror = () => {
        /* URL ⇒ file existed at emit time; a failure here is a transient
           (AV lock, mid-replace). Two short retries, then the placeholder —
           an artready (or the next fetch) heals it, never a retry loop. */
        if (tries < 2 && container.dataset.artUrl === url) {
          tries++;
          setTimeout(() => {
            if (container.dataset.artUrl === url) img.src = url + "&r=" + tries;
          }, 250 * tries);
        } else {
          markArtDead(url);   /* stop re-probing on future renders */
          if (artKey) container.dataset.artKey = artKey;
          /* same helper as the no-url path so the surface-specific glyph
             size applies on the error path too (small tiles clip the
             default 2.4em badge otherwise) */
          artPlaceholder(container, glyphSize);
        }
      };
      img.src = url;
      /* SYNCHRONOUS cache-hit fallback: a file:// cover that's already decoded
         (warmed by artram, or a re-render of a previously-loaded card) may have
         complete=true before onload can fire — mark it loaded now so it shows
         this frame instead of waiting for an onload that already passed. */
      if (img.complete && img.naturalWidth > 0) { img.classList.add("loaded"); artDead.delete(url); }
      /* prepend: overlays (chips/badges/play) stay AFTER the cover in DOM
         order, matching the original build order of every surface */
      container.prepend(img);
    } else {
      if (artKey) container.dataset.artKey = artKey;
      else delete container.dataset.artKey;
      artPlaceholder(container, glyphSize);
    }
  }

  /* Art-key string for a row/album object — MUST mirror the C side's
     album_artist-or-artist fallback (append_row / build_albums). */
  function artKeyOf(aa, album) { return (aa || "") + "\x1f" + (album || ""); }

  /* Derive a sibling URL (.hires.png / .depth.png) from a served cover URL,
     preserving the ?g=<mtime> generation param (the URL no longer ends in
     ".png", so a bare /\.png$/ replace would silently no-op). */
  function artSibling(url, ext) {
    return (url || "").replace(/\.png(\?[^#]*)?$/i, ext + "$1");
  }

  /* ============================================================
     3D DEPTH ART — perspective tilt on the now-playing cover.
     pointer mode: tilts toward the cursor over the panel;
     showcase mode: slow autonomous orbit (like the Android app);
     off: static. Values echo the Android app's volumetric feel.
     ============================================================ */
  const tilt = { x: 0, y: 0, tx: 0, ty: 0 };
  let pointerInside = false;

  E.npArtStage.addEventListener("pointermove", (ev) => {
    if (theme.motion !== "pointer" || !theme.art3d) return;
    const r = E.npArtStage.getBoundingClientRect();
    pointerInside = true;
    tilt.tx = clamp(((ev.clientX - r.left) / r.width) * 2 - 1, -1, 1);
    tilt.ty = clamp(((ev.clientY - r.top) / r.height) * 2 - 1, -1, 1);
    /* volumetric mesh gets tilt EVENT-driven (worker owns its own motion) */
    if (volumetricActive) MnDepthArt.setTilt(tilt.tx, tilt.ty, true);
    wakeLoop();
  });
  E.npArtStage.addEventListener("pointerleave", () => {
    pointerInside = false; tilt.tx = 0; tilt.ty = 0;
    if (volumetricActive) MnDepthArt.setTilt(0, 0, false);
  });

  /* Low-power mode (persisted C-side; mirrored here for boot-time reads):
     flat 2D art, no animations, no spectrum, slower polling, no art
     pre-warm. The mirror updates on every settings round-trip. */
  function lowPower() {
    try { return localStorage.getItem("mn.lowpower") === "1"; } catch (_) { return false; }
  }
  window.__mnLowPower = lowPower;   /* artwarm.js reads this at sweep time */

  /* Volumetric (depth-mapped) art: WebGL mesh when a <hash>.depth.png exists
     beside the cover PNG; CSS tilt otherwise. The mesh renders on a worker
     thread (OffscreenCanvas) and pauses when the panel is hidden. In low-
     power mode the worker + GL context are never even created — the flat
     cover is the whole story. */
  const depthCanvas = $("#np-depth-canvas");
  const depthOk = !lowPower() &&
      !!(window.MnDepthArt && depthCanvas && MnDepthArt.mount(depthCanvas));
  let volumetricActive = false;
  if (depthOk) MnDepthArt.setMotion(theme.motion);

  /* the mesh should only render while it is actually on screen */
  let artFullscreen = false;
  function syncDepthActive() {
    if (!depthOk) return;
    /* visible if: fullscreen, OR the now-playing panel is open */
    const onScreen = artFullscreen || !E.app.classList.contains("np-collapsed");
    MnDepthArt.setActive(volumetricActive && theme.art3d && onScreen);
  }

  /* ---- Fullscreen 3D cover ---------------------------------------------------
     Move the whole art stage (#np-art-stage, carrying its already-mounted WebGL
     canvas + worker) into a full-window overlay, then move it back on exit — so
     the same GL context keeps rendering, just larger. */
  const artStage = $("#np-art-stage");
  const artStageHome = artStage ? artStage.parentNode : null;
  const artFsOverlay = $("#np-art-fs");
  const artFsHost = $("#np-art-fs-host");
  function enterArtFullscreen() {
    if (artFullscreen || !artStage || !artFsHost) return;
    artFullscreen = true;
    artFsHost.appendChild(artStage);        /* re-parent the live canvas */
    artStage.classList.add("in-fs");
    artFsOverlay.hidden = false;
    /* mirror the current track meta into the fullscreen caption */
    const t = $("#np-fs-title"), a = $("#np-fs-artist");
    if (t) t.textContent = E.npTitle.textContent;
    if (a) a.textContent = E.npArtist.textContent;
    document.body.classList.add("art-fs-open");
    syncDepthActive();
    if (depthOk) MnDepthArt.resize && MnDepthArt.resize();
  }
  function exitArtFullscreen() {
    if (!artFullscreen || !artStage || !artStageHome) return;
    /* graceful out: fade the fullscreen stage (motion.close no-ops if a
       close is already in flight), THEN re-parent the live canvas home */
    motion.close(artFsOverlay, () => {
      artFullscreen = false;
      artStage.classList.remove("in-fs");
      artFsOverlay.hidden = true;
      document.body.classList.remove("art-fs-open");
      /* put the stage back as the FIRST child of its home (#nowplaying) */
      artStageHome.insertBefore(artStage, artStageHome.firstChild);
      syncDepthActive();
      if (depthOk) MnDepthArt.resize && MnDepthArt.resize();
    });
  }
  { const mx = $("#np-art-max"); if (mx) mx.addEventListener("click", (e) => { e.stopPropagation(); enterArtFullscreen(); }); }
  { const cl = $("#np-art-fs-close"); if (cl) cl.addEventListener("click", exitArtFullscreen); }
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && artFullscreen) exitArtFullscreen(); });

  /* Depth maps generate ASYNCHRONOUSLY (seconds to a minute with the larger
     models) — when a track starts before its map exists, RETRY with backoff
     until the worker publishes it, instead of silently staying flat. */
  let volRetryT = 0, volRetryN = 0, volRetryUrl = "";
  function updateVolumetricArt(artUrl, isRetry) {
    if (!isRetry) {                       /* fresh art: reset the retry state */
      clearTimeout(volRetryT);
      volRetryN = 0;
      volRetryUrl = artUrl || "";
    }
    if (!depthOk || !artUrl || !theme.art3d) {
      volumetricActive = false;
      if (depthCanvas) depthCanvas.hidden = true;
      E.npArt.style.visibility = "";
      syncDepthActive();
      return;
    }
    /* cache-bust retries so a previously-missing file:// load isn't reused
       (& not ? — served art URLs already carry a ?g=<mtime> generation) */
    const bust = isRetry ? ((artUrl.indexOf("?") >= 0 ? "&r=" : "?r=") + volRetryN) : "";
    const depthUrl = artSibling(artUrl, ".depth.png") + bust;
    const coverUrl = artSibling(artUrl, ".hires.png") + bust;
    MnDepthArt.setSources(artUrl, depthUrl, (ok) => {
      if (volRetryUrl !== artUrl) return;   /* track changed while loading */
      volumetricActive = ok;
      depthCanvas.hidden = !ok;
      /* keep the flat cover as backdrop when volumetric is off */
      E.npArt.style.visibility = ok ? "hidden" : "";
      if (ok) {
        /* card stays flat; the mesh does the motion */
        E.npArt3d.style.transform = "";
        E.npGlare.style.opacity = "0";
        artStyleReset = true;
        clearTimeout(volRetryT);
      } else if (volRetryN < 12) {
        /* map not published yet: 5s, 10s, 15s … (~2.5 min total) */
        volRetryN++;
        clearTimeout(volRetryT);
        volRetryT = setTimeout(() => {
          if (volRetryUrl === artUrl) updateVolumetricArt(artUrl, true);
        }, Math.min(5000 * volRetryN, 20000));
      }
      syncDepthActive();
      wakeLoop();
    }, coverUrl);
  }

  /* CSS-tilt fallback animation. Volumetric covers are fully handled by the
     depth worker, so this does NOTHING per frame in that case. All writes are
     compositor-only (transform / opacity) and skipped when unchanged.

     PARK WHEN IDLE: this used to return true whenever art3d + motion were on,
     with NO regard for playback or visibility — so the autonomous idle orbit
     drove the main rAF loop at 60fps FOREVER (measured ~80% of a core at
     idle), starving scroll. Now it only animates when the panel is on-screen
     AND the user is actually interacting (pointer inside) OR a track is
     playing. Paused + cursor-elsewhere (i.e. while browsing/scrolling the
     library) => the loop parks. The showcase idle orbit still runs while
     playing, which is when the cover is the focus. */
  let artStyleReset = true, glareQx = -1, glareQy = -1;
  function cssArtAnimating() {
    if (volumetricActive || !theme.art3d || theme.motion === "off") return false;
    const onScreen = artFullscreen || !E.app.classList.contains("np-collapsed");
    if (!onScreen) return false;
    const playing = !!(state.now && state.now.playing);
    return pointerInside || playing;
  }
  function tickArt(t) {
    if (!cssArtAnimating()) {
      if (!artStyleReset) {
        artStyleReset = true;
        E.npArt3d.style.transform = "";
        E.npGlare.style.opacity = "0";
        glareQx = glareQy = -1;
      }
      return;
    }
    artStyleReset = false;
    let gx = 0, gy = 0;
    if (theme.motion === "showcase" || (theme.motion === "pointer" && !pointerInside)) {
      /* slow idle orbit — mirrors the Android showcase mode */
      gx = Math.sin(t * 0.00016 * Math.PI * 2) * 0.45;
      gy = Math.sin(t * 0.00009 * Math.PI * 2) * 0.28;
    } else {
      gx = tilt.tx; gy = tilt.ty;
    }
    /* glide easing (Android GLIDE=0.07) */
    tilt.x += (gx - tilt.x) * 0.07;
    tilt.y += (gy - tilt.y) * 0.07;
    const ry = tilt.x * 9;       /* deg */
    const rx = -tilt.y * 7;
    E.npArt3d.style.transform =
      "rotateY(" + ry.toFixed(2) + "deg) rotateX(" + rx.toFixed(2) + "deg) translateZ(12px)";
    /* moving glare highlight sells the depth — QUANTIZED to 0.5% steps so
       slow-orbit frames skip the (paint-triggering) background rewrite */
    const qx = Math.round((tilt.x * 0.5 + 0.5) * 200);
    const qy = Math.round((tilt.y * 0.5 + 0.5) * 200);
    if (qx !== glareQx || qy !== glareQy) {
      glareQx = qx; glareQy = qy;
      E.npGlare.style.opacity = "1";
      E.npGlare.style.background =
        "radial-gradient(420px 300px at " + (qx / 2) + "% " +
        (qy / 2) + "%, rgba(255,255,255,.10), transparent 60%)";
    }
  }

  /* ============================================================
     TRACKS VIEW
     ============================================================ */
  /* Compact hover-revealed thumbs for ANY track row (t needs .id + .liked).
     Used by the details-mode inline lists and the search-result rows so the
     like/dislike system is reachable from EVERY track surface. */
  function rowThumbs(t) {
    const th = el("div", "row-thumbs");
    if (!t || !(t.id > 0)) return th;
    const up = el("span", "thumb up" + (t.liked === 1 ? " on" : ""), "👍");
    up.title = "Like";
    const dn = el("span", "thumb down" + (t.liked === -1 ? " on" : ""), "👎");
    dn.title = "Dislike";
    const rate = (v) => {
      const next = (t.liked === v) ? 0 : v;
      send({ cmd: "like", id: t.id, v: next });
      t.liked = next;
      up.classList.toggle("on", next === 1);
      dn.classList.toggle("on", next === -1);
      if (state.now && state.now.track_id === t.id) updateThumbsUI(next);
    };
    up.addEventListener("click", (e) => { e.stopPropagation(); rate(1); });
    dn.addEventListener("click", (e) => { e.stopPropagation(); rate(-1); });
    th.appendChild(up); th.appendChild(dn);
    return th;
  }

  function ratingCell(row) {
    const wrap = el("div", "c-rating");
    /* Android-style thumbs (liked: 1 / -1 / 0) — coexists with the stars,
       like MediaMonkey. Degrades gracefully if the backend lacks "like". */
    const up = el("span", "thumb up" + (row.liked === 1 ? " on" : ""), "👍");
    const dn = el("span", "thumb down" + (row.liked === -1 ? " on" : ""), "👎");
    const setLiked = (v) => {
      row.liked = v;
      send({ cmd: "like", id: row.id, v });
      up.classList.toggle("on", v === 1);
      dn.classList.toggle("on", v === -1);
      if (state.now && state.now.track_id === row.id) { state.now.liked = v; updateThumbsUI(v); }
    };
    up.addEventListener("click", (ev) => { ev.stopPropagation(); setLiked(row.liked === 1 ? 0 : 1); });
    dn.addEventListener("click", (ev) => { ev.stopPropagation(); setLiked(row.liked === -1 ? 0 : -1); });
    wrap.appendChild(up); wrap.appendChild(dn);
    for (let i = 1; i <= 5; i++) {
      const st = el("span", "star" + (i <= (row.rating || 0) ? " on" : ""), "★");
      st.addEventListener("click", (ev) => {
        ev.stopPropagation();
        const stars = (row.rating === i) ? 0 : i;
        row.rating = stars;
        send({ cmd: "rating", id: row.id, stars });
        renderRatingInRow(wrap, stars);
      });
      wrap.appendChild(st);
    }
    return wrap;
  }
  function renderRatingInRow(wrap, stars) {
    $$(".star", wrap).forEach((s, i) => s.classList.toggle("on", i < stars));
  }

  function trackRowEl(row, index) {
    const r = el("div", "track-row");
    r.dataset.id = row.id;
    if (state.now && state.now.playing && sameTrack(row)) r.classList.add("playing");

    const num = el("div", "c-num");
    num.appendChild(el("span", "num-txt", String(index + 1)));
    const play = el("div", "c-play", "▶");
    play.addEventListener("click", (ev) => { ev.stopPropagation(); playTrack(row.id); });
    num.appendChild(play);

    r.appendChild(num);
    const title = el("div", "c-title", esc(row.title) || "Unknown title");
    if (row.missing) {
      r.classList.add("missing");
      const mb = el("span", "row-missing", "MISSING");
      mb.title = "The file for this track no longer exists on disk";
      title.appendChild(mb);
    } else if (isNew(row.date_added)) {
      const nb = el("span", "row-new", "NEW");
      nb.title = "Recently added to the library (within " + PREFS.newdays + " days)";
      title.appendChild(nb);
    }
    r.appendChild(title);
    r.appendChild(metaLink("c-artist", row.artist, null, "artist"));
    r.appendChild(metaLink("c-album", row.album, null, "album", row.artist));
    r.appendChild(el("div", "c-year", row.year ? String(row.year) : ""));
    r.appendChild(metaLink("c-genre", row.genre));
    r.appendChild(el("div", "c-dur", fmtTime(row.duration_ms)));
    r.appendChild(ratingCell(row));

    r.addEventListener("click", (ev) => {
      const scope = r.parentNode || E.trackRows;
      if (ev.ctrlKey || ev.metaKey) {
        r.classList.toggle("selected");
        selAnchor = r;
      } else if (ev.shiftKey && selAnchor && selAnchor.parentNode === scope) {
        /* clear only the CURRENTLY selected rows, then walk siblings from
           the anchor to this row — O(selection + range), not O(all 30k
           loaded rows) like the old build-array-and-sweep version */
        $$(".track-row.selected", scope).forEach((x) => x.classList.remove("selected"));
        const fwd = !!(selAnchor.compareDocumentPosition(r) & Node.DOCUMENT_POSITION_FOLLOWING);
        let n = selAnchor;
        while (n) {
          if (n.classList && n.classList.contains("track-row")) n.classList.add("selected");
          if (n === r) break;
          n = fwd ? n.nextElementSibling : n.previousElementSibling;
        }
      } else {
        $$(".track-row.selected").forEach((x) => x.classList.remove("selected"));
        r.classList.add("selected");
        selAnchor = r;
      }
      updateSelBar();
    });
    r.addEventListener("dblclick", () => playTrack(row.id));
    r.addEventListener("contextmenu", (ev) => {
      if (window.MnTagEdit) MnTagEdit.trackMenu(ev, row, r);
    });
    return r;
  }

  /* ---- multi-select bulk-action bar (Ctrl/Shift-click rows) ---- */
  let selAnchor = null;
  function selectedIds() {
    return $$(".track-row.selected").map((x) => parseInt(x.dataset.id, 10)).filter((n) => !isNaN(n));
  }
  function clearSel() {
    $$(".track-row.selected").forEach((x) => x.classList.remove("selected"));
    const b = $("#sel-bar"); if (b) b.remove();
  }
  function updateSelBar() {
    let bar = $("#sel-bar");
    const ids = selectedIds();
    if (ids.length < 2) { if (bar) bar.remove(); return; }
    if (!bar) {
      bar = el("div", "sel-bar"); bar.id = "sel-bar";
      document.body.appendChild(bar);
    }
    bar.innerHTML = "";
    bar.appendChild(el("span", "sel-count", ids.length + " selected"));
    const mkBtn = (label, fn) => { const b = el("button", "sel-act", label); b.addEventListener("click", fn); bar.appendChild(b); };
    mkBtn("Queue next", () => { ids.forEach((id) => send({ cmd: "queuenext", id })); clearSel(); });
    mkBtn("Queue last", () => { ids.forEach((id) => send({ cmd: "queuelast", id })); clearSel(); });
    mkBtn("Add to playlist…", (ev) => { if (typeof window.__mnAddToPlaylist === "function") window.__mnAddToPlaylist(ids, ev); clearSel(); });
    mkBtn("Clear", clearSel);
  }

  function sameTrack(row) {
    const n = state.now;
    return n && row.title === n.track_title && row.artist === n.track_artist;
  }

  function playTrack(id) { send({ cmd: "play", id }); }

  /* ============================================================
     TRACK LIST — windowed paging (append rows, infinite-scroll to
     fetch the next page). Simple flow rendering: sort/search always
     works, no virtual-geometry races. A `gen` on each request lets
     us drop replies from a superseded sort. Large libraries stay
     responsive via paging + content-visibility on off-screen rows.
     ============================================================ */
  let rowCacheGen = 0;                 /* bumped on reset: drops stale replies */
  let albGen = 0;                      /* same for album pages                 */

  function resetTracks() {
    rowCacheGen++;
    state.trkRows = []; state.trkOffset = 0; state.trkTotal = 0; state.trkDone = false;
    /* trkLoading MUST clear: a reset mid-flight gen-drops the in-flight
       reply (which is what normally clears it) and a stuck true would
       block loadTracks FOREVER — every track surface would wedge empty.
       (The albums twin had this exact fix; the tracks side was missed.) */
    state.trkLoading = false;
    E.trackRows.innerHTML = "";
    if (E.trackScroll) E.trackScroll.scrollTop = 0;   /* sort/search jumps to top */
  }
  function loadTracks() {
    if (state.trkLoading || state.trkDone) return;
    state.trkLoading = true;
    send({ cmd: "tracks", offset: state.trkOffset, count: PAGE, gen: rowCacheGen });
  }

  on("tracks", (m) => {
    /* Drop replies from a superseded sort/search (stale ordering). */
    if (m.gen != null && m.gen !== rowCacheGen) return;
    state.trkLoading = false;
    state.trkTotal = m.total || 0;
    const rows = m.rows || [];
    const base = state.trkRows.length;
    const frag = document.createDocumentFragment();
    const fresh = base === 0 ? [] : null;   /* stagger ONLY the first page */
    rows.forEach((row, i) => {
      state.trkRows.push(row);
      const r = trackRowEl(row, base + i);
      if (fresh && i < 18) fresh.push(r);
      frag.appendChild(r);
    });
    /* entrance stagger for a fresh list (sort/search/view change) — never on
       infinite-scroll appends, never re-triggered by scrolling */
    if (fresh) MN.get("motion").stagger(fresh, 18);
    E.trackRows.appendChild(frag);   /* one DOM insertion per page */
    state.trkOffset += rows.length;
    if (rows.length < PAGE || state.trkOffset >= state.trkTotal) state.trkDone = true;
    if (state.trkTotal === 0 && state.view === 0) showLibraryEmpty(true);
    else if (state.view === 0) showLibraryEmpty(false);
    if (state.now && state.now.playing && state.view === 0) refreshPlayingRow();
    updateViewTitle();
  });

  MN.get("custom").onRowHeight(() => {});
  MN.get("custom").onLayout(() => {});

  function showLibraryEmpty(on) {
    let ph = $("#lib-empty", E.trackRows.parentElement);
    if (ph) ph.remove();                 /* rebuild — the text is contextual */
    if (!on) return;
    /* a FILTER with zero matches is not an empty library — say so instead
       of telling the user to add folders */
    const filtered = !!(state.query || state.categoryFilter || state.category);
    ph = el("div", "empty-state"); ph.id = "lib-empty";
    ph.appendChild(el("div", "empty-glyph", "♪"));
    ph.appendChild(el("div", "empty-title",
      filtered ? "No matching tracks" : "Your library is empty"));
    ph.appendChild(el("div", "empty-sub",
      filtered ? "Try a different search or clear the filter."
               : "Click “＋ Add folder” to pick a music folder on your PC."));
    E.trackRows.parentElement.insertBefore(ph, E.trackRows);
  }

  /* ============================================================
     ALBUMS VIEW
     ============================================================ */
  function fmtSize(bytes) {
    if (!bytes || bytes <= 0) return "";
    const mb = bytes / (1024 * 1024);
    if (mb >= 1024) return (mb / 1024).toFixed(1) + " GB";
    return (mb >= 10 ? mb.toFixed(0) : mb.toFixed(1)) + " MB";
  }
  /* ---- shared quality predicates -----------------------------------
     ONE source of truth for every pill family (player bar, album cards,
     queue) — these used to be three diverging copies. The format string
     is now the REAL codec from the container magic bytes (ALAC/AAC/
     OPUS...), so lossless classing is finally correct for .m4a. */
  function isLossless(fmt) {
    return /flac|alac|wav|aif|ape|wv|dsd|dsf|tak/i.test(String(fmt || ""));
  }
  function isHires(bitDepth, sampleRate) {
    return (bitDepth | 0) >= 24 || (sampleRate | 0) >= 88200;
  }
  /* Overflow guard: a pill shows IN FULL or not at all. Hides whole
     pills from the END (least important last) until the row fits;
     pills marked .keep survive longest.

     ZERO forced layout on resize: each pill's width is measured ONCE right
     after its row is (re)built (measurePills, from armPillFit) and cached on
     the element as ._pw. The resize path then reads only host.clientWidth —
     BEFORE touching any class — and does pure arithmetic against the cached
     widths. Un-hiding a pill can't change host.clientWidth, so no
     read-after-write reflow occurs. */
  function measurePills(host) {
    if (!host) return;
    /* measure with everything visible so cached widths are the true full size */
    const pills = Array.from(host.children);
    pills.forEach((p) => p.classList.remove("ovf-hide"));
    pills.forEach((p) => { p._pw = p.offsetWidth; });
  }
  function fitPills(host) {
    if (!host) return;
    const pills = Array.from(host.children);
    if (!pills.length) return;
    const avail = host.clientWidth;              /* READ first, before writes */
    const gap = 5;                               /* .pl-format/.pl-out flex gap */
    let total = 0, i;
    for (i = 0; i < pills.length; i++) {
      if (pills[i]._pw == null) pills[i]._pw = pills[i].offsetWidth; /* lazy first-time */
      total += pills[i]._pw;
    }
    total += gap * (pills.length - 1);
    const hidden = new Array(pills.length).fill(false);
    let total2 = total, visCount = pills.length;
    while (total2 > avail + 1 && visCount > 0) {
      let victim = -1;
      for (i = pills.length - 1; i >= 0; i--) {
        if (!hidden[i] && !pills[i].classList.contains("keep")) { victim = i; break; }
      }
      if (victim < 0) {
        for (i = pills.length - 1; i >= 0; i--) {
          if (!hidden[i]) { victim = i; break; }
        }
      }
      if (victim < 0) break;
      hidden[victim] = true; visCount--;
      total2 -= pills[victim]._pw + gap;
    }
    /* single write pass */
    for (i = 0; i < pills.length; i++) {
      pills[i].classList.toggle("ovf-hide", hidden[i]);
    }
  }
  function armPillFit() {
    const hosts = [$("#pl-format"), $("#pl-out")].filter(Boolean);
    /* measure once (widths are stable until the next rebuild), then fit */
    hosts.forEach((h) => { measurePills(h); fitPills(h); });
    if (!armPillFit._ro && window.ResizeObserver) {
      /* coalesce resize storms to one re-fit per frame; the callback does
         NOT re-measure (cached ._pw) — only reads clientWidth + toggles */
      armPillFit._ro = new ResizeObserver(() => {
        if (armPillFit._raf) return;
        armPillFit._raf = requestAnimationFrame(() => {
          armPillFit._raf = 0;
          hosts.forEach((h) => fitPills(h));
        });
      });
      hosts.forEach((h) => armPillFit._ro.observe(h));
    }
  }

  /* ---- resize-active marker -------------------------------------------
     Adds body.resizing WHILE the window is actively resizing and removes it
     ~140ms after motion stops. The CSS uses it to drop the topbar backdrop
     blur and the decorative body gradient during the drag (both re-rasterize
     per frame otherwise) and restore them the instant the drag ends. A
     ResizeObserver on the root fires for every size change; that's exactly
     the "is resizing right now" signal, at zero polling cost when idle. */
  (function initResizeMarker() {
    if (!window.ResizeObserver) return;
    const body = document.body;
    let offTimer = 0, on = false;
    const stop = () => {
      on = false; body.classList.remove("resizing");
      /* column count / card width changed with the window */
      if (VG.on) { vgRefresh(); reflowExpand(); }
      else remeasureAlbCard();
    };
    const ro = new ResizeObserver(() => {
      if (!on) { on = true; body.classList.add("resizing"); }
      clearTimeout(offTimer);
      offTimer = setTimeout(stop, 140);
    });
    ro.observe(document.documentElement);
    /* the first observe fires immediately at boot — clear that spurious mark */
    setTimeout(stop, 200);
  })();

  function albumPills(a) {
    const wrap = el("div", "album-pills");
    const add = (txt, cls, tip) => {
      if (!txt) return;
      const p = el("span", "apill" + (cls ? " " + cls : ""), txt);
      if (tip) p.title = tip;
      wrap.appendChild(p);
    };
    if (isNew(a.date_added)) add("NEW", "new", "Recently added to the library (within " + PREFS.newdays + " days)");
    if (a.format) add(String(a.format).toUpperCase(),
      "fmt " + (isLossless(a.format) ? "lossless" : "lossy"),
      isLossless(a.format) ? "Audio format — lossless: bit-perfect copy of the original"
                           : "Audio format — lossy compressed audio");
    add((a.track_count || 0) + (a.track_count === 1 ? " track" : " tracks"), null, "Number of tracks in this album");
    if (a.year) add(String(a.year), null, "Release year");
    if (a.sample_rate) add((a.sample_rate / 1000).toFixed(a.sample_rate % 1000 ? 1 : 0) + " kHz", null,
      "Sample rate — how many times per second the audio was sampled (CD = 44.1 kHz; higher = hi-res)");
    if (a.bit_depth) add(a.bit_depth + "-bit", null,
      "Bit depth — precision of each sample (CD = 16-bit; 24-bit = hi-res / more dynamic range)");
    if (a.bitrate) add(a.bitrate + " kbps", null,
      "Bitrate — data per second of audio; higher generally means better quality for lossy formats");
    const sz = fmtSize(a.size); if (sz) add(sz, null, "Total disk size of all tracks in this album");
    return wrap;
  }

  /* Build the HEAVY content (art + info + track host) of an album card.
     Split out from the shell so the recycler can strip it off off-screen
     cards and rebuild it on demand — see fillAlbumCard / stripAlbumCard. */
  function buildAlbumContent(a) {
    const art = el("div", "album-art");
    setArt(art, a.art, "2.6em", artKeyOf(a.artist, a.title));
    /* Android-style overlay chips directly on the artwork (always visible) */
    const chips = el("div", "art-chips");
    const hiresA = isHires(a.bit_depth, a.sample_rate);
    if (a.format) {
      const fc = el("span", "achip fmt" + (hiresA ? " hires" : ""), String(a.format).toUpperCase());
      fc.title = hiresA ? "Audio format — hi-res (24-bit or ≥88.2 kHz)" : "Audio format / codec";
      chips.appendChild(fc);
    }
    if (a.track_count) {
      const cc = el("span", "achip cnt", String(a.track_count) + " ♪");
      cc.title = a.track_count + (a.track_count === 1 ? " track" : " tracks") + " in this album";
      chips.appendChild(cc);
    }
    if (chips.childElementCount) art.appendChild(chips);
    if (isNew(a.date_added)) {
      const nc = el("span", "art-chip-new", "NEW");
      nc.title = "Recently added to the library (within " + PREFS.newdays + " days)";
      art.appendChild(nc);
    }
    /* book progress badge (kind views): %-ring while in progress, ✓ when
       finished. Data from the bookbadges map; static CSS, no animation. */
    if (state.activeKind && state.bookBadges) {
      const bp = state.bookBadges[String(a.id)];
      if (bp) {
        const pctInt = Math.round((bp.percent || 0) * 100);
        const bg = el("span", "book-badge" + (bp.finished ? " done" : ""),
                      bp.finished ? "✓" : pctInt + "%");
        if (!bp.finished)
          bg.style.background =
            "conic-gradient(var(--accent) " + pctInt + "%, var(--raised) 0)";
        bg.title = bp.finished ? "Finished" : pctInt + "% listened";
        art.appendChild(bg);
      }
    }
    const play = el("button", "album-play", "▶");
    play.title = "Play album";
    play.addEventListener("click", (ev) => { ev.stopPropagation(); send({ cmd: "playalbum", id: a.id }); });
    art.appendChild(play);

    const info = el("div", "album-info");
    info.appendChild(el("div", "album-title", esc(a.title) || "Unknown album"));
    info.appendChild(metaLink("album-artist", a.artist, null, "artist"));
    info.appendChild(albumPills(a));

    const frag = document.createDocumentFragment();
    frag.appendChild(art);
    frag.appendChild(info);
    /* inline track-list host — hidden except in DETAILS browse mode */
    frag.appendChild(el("div", "card-tracks"));
    return frag;
  }

  /* All album cards share one height (square art + fixed info block). We
     measure it ONCE from a filled card and reuse it as the stripped-shell
     height, so the recycler never reads per-card layout during a scroll. */
  let albCardH = 0;
  /* Publish the measured height as --alb-ci so the CSS contain-intrinsic-size
     placeholder is EXACT (an off-by-anything estimate makes every c-v:auto
     activation shift the whole grid — see styles.css .album-card). */
  function setAlbCardH(h) {
    albCardH = h;
    if (E.albumGrid) E.albumGrid.style.setProperty("--alb-ci", h + "px");
  }
  /* Card height tracks column width (square art) — re-measure after resizes. */
  function remeasureAlbCard() {
    requestAnimationFrame(() => {
      const c = E.albumGrid && E.albumGrid.querySelector(".album-card");
      if (!c) return;
      const h = c.offsetHeight;
      if (h > 0 && Math.abs(h - albCardH) > 1) setAlbCardH(h);
    });
  }
  /* Fill a stripped card shell with its content (idempotent). */
  function fillAlbumCard(card) {
    if (card._filled) return;
    const a = card._album;
    if (!a) return;
    card.style.minHeight = "";
    card.style.contentVisibility = "";
    card.appendChild(buildAlbumContent(a));
    card._filled = true;
    if (!albCardH) {
      /* first fill of the session — cache the natural height next frame */
      requestAnimationFrame(() => {
        const h = card.offsetHeight;
        if (h > 0) setAlbCardH(h);
      });
    }
  }
  /* Strip a card down to an empty, height-preserving shell (recycler).
     NOTE: no layout reads here — this runs in the recycler's write phase;
     albCardH is cached during the read phase (or the first fill's rAF). */
  function stripAlbumCard(card) {
    if (!card._filled) return;
    const h = albCardH || 260;
    card.textContent = "";
    card.style.minHeight = h + "px";
    /* content-visibility:hidden keeps the box (height reserved) but skips
       ALL rendering of the empty shell — cheaper than the default auto */
    card.style.contentVisibility = "hidden";
    card._filled = false;
  }

  function albumCardEl(a) {
    const card = el("div", "album-card");
    card.dataset.id = a.id;
    card._album = a;
    fillAlbumCard(card);
    card.addEventListener("click", () => {
      /* details mode shows the track list inline — no expand panel */
      if (E.albumGrid.classList.contains("mode-details")) return;
      toggleExpand(card, a);
    });
    card.addEventListener("contextmenu", (ev) => {
      if (window.MnTagEdit) MnTagEdit.albumMenu(ev, a, card);
    });
    return card;
  }

  /* ================= VIRTUAL ALBUM GRID =================
     True windowing (UICollectionView-style). History: the grid used to hold
     EVERY album as a live element with a JS strip/fill recycler + CSS
     content-visibility on top. Traced at 4K that architecture cost ~277ms of
     main-thread work PER SCROLL STEP with ~2,000 cards — compositing
     bookkeeping (Layerize ~106ms + Commit ~75ms, both O(total cards)),
     accessibility-tree rebuilds (~65ms), style/layout ~30ms — i.e. ~3 fps.
     Now the DOM holds only a POOL of (visible rows + 2×buffer) × cols card
     elements, absolutely positioned over a fixed-height spacer that gives
     the scrollbar its exact range. Global row R binds to pool row R %
     poolRows, so scrolling within a row touches NOTHING (pure compositor
     scroll) and crossing a row rebinds exactly one row of cards.
     Virtual mode covers the grid + rolodex album styles; details (variable
     row heights) and carousel (horizontal) keep the legacy flow rendering. */
  const VG = {
    on: false,
    spacer: null, pool: [], cols: 0, poolRows: 0,
    colW: 178, rowH: 252, gap: 22, buf: 2,
    firstRow: -1, total: 0,
    panelRow: -1, panelH: 0,   /* open expand panel: row + height shift */
    clientH: 0,
  };
  function vgEnsureSpacer() {
    if (!VG.spacer || !VG.spacer.isConnected) {
      VG.spacer = el("div", "vg-spacer");
      E.albumGrid.appendChild(VG.spacer);
    }
    return VG.spacer;
  }
  function vgRows() { return VG.cols ? Math.ceil(state.albCards.length / VG.cols) : 0; }
  function vgYOf(R) {
    return R * VG.rowH + (VG.panelRow >= 0 && R > VG.panelRow ? VG.panelH : 0);
  }
  function vgSetSpacerH() {
    const rows = vgRows();
    let h = (rows ? rows * VG.rowH - VG.gap : 0) + (VG.panelRow >= 0 ? VG.panelH : 0);
    /* in-place refresh in flight (scan/heal/artfetch triggered a reload with
       scroll preservation): never let the spacer shrink below the preserved
       position while pages stream back in, or the browser clamps scrollTop
       to 0 and teleports the user mid-browse. Cleared once the reload's data
       reaches the preserved offset (see the "albums" handler). */
    if (state._albKeepTop != null)
      h = Math.max(h, state._albKeepTop +
                      (VG.clientH || E.albumGrid.clientHeight || 800));
    vgEnsureSpacer().style.height = Math.max(0, h) + "px";
  }
  function vgMetrics() {
    const g = E.albumGrid;
    const cs = getComputedStyle(g);
    const padL = parseFloat(cs.paddingLeft) || 26,
          padR = parseFloat(cs.paddingRight) || 26;
    const availW = g.clientWidth - padL - padR;
    const min = parseFloat(cs.getPropertyValue("--card-min")) || 178;
    /* If the grid isn't laid out yet (availW <= 0) DON'T collapse to a single
       column — keep the previous good column count so the caller's guard can
       retry rather than committing a 1-wide build. */
    if (availW < min) return VG.cols || 0;
    const cols = Math.max(1, Math.floor((availW + VG.gap) / (min + VG.gap)));
    VG.colW = Math.floor((availW - VG.gap * (cols - 1)) / cols);
    VG.clientH = g.clientHeight;
    const c0 = VG.pool.find((c) => c._aid != null && c.style.display !== "none");
    const cardH = c0 ? c0.offsetHeight : 0;
    VG.rowH = (cardH > 0 ? cardH : VG.colW + 74) + VG.gap;
    return cols;
  }
  function vgBuildPool() {
    const cols = vgMetrics();
    if (cols < 1) return false;   /* metrics not ready — caller retries */
    const poolRows = Math.ceil(VG.clientH / VG.rowH) + 1 + VG.buf * 2;
    const sp = vgEnsureSpacer();
    if (cols !== VG.cols || poolRows !== VG.poolRows || !VG.pool.length) {
      VG.cols = cols; VG.poolRows = poolRows;
      const need = cols * poolRows;
      while (VG.pool.length < need) {
        const card = el("div", "album-card");
        card._aid = null; card._idx = -1; card._R = -1; card._col = -1; card._y = -1;
        card.style.display = "none";
        card.addEventListener("click", () => {
          if (!card._album) return;
          if (E.albumGrid.classList.contains("mode-details")) return;
          toggleExpand(card, card._album);
        });
        card.addEventListener("contextmenu", (ev) => {
          if (window.MnTagEdit && card._album) MnTagEdit.albumMenu(ev, card._album, card);
        });
        sp.appendChild(card);
        VG.pool.push(card);
      }
      while (VG.pool.length > cols * poolRows) VG.pool.pop().remove();
    }
    for (const c of VG.pool) {
      c.style.width = VG.colW + "px";
      c._R = -1; c._col = -1; c._y = -1;   /* force reposition */
    }
    VG.firstRow = -1;                      /* force rebind */
  }
  function vgBind(card, idx, R, col) {
    const a = state.albCards[idx];
    if (!a) {
      if (card.style.display !== "none") card.style.display = "none";
      card._idx = -1;
      return;
    }
    if (card.style.display) card.style.display = "";
    if (card._aid !== a.id) {
      card._aid = a.id; card._album = a;
      card.dataset.id = a.id;
      card.textContent = "";
      card.appendChild(buildAlbumContent(a));
    } else {
      card._album = a;                     /* refreshed data object */
    }
    card._idx = idx;
    const isExp = state.expandedAlbum != null &&
                  String(a.id) === String(state.expandedAlbum);
    if (card.classList.contains("expanded") !== isExp)
      card.classList.toggle("expanded", isExp);
    if (card._col !== col) {
      card._col = col;
      card.style.left = (col * (VG.colW + VG.gap)) + "px";
    }
    const y = vgYOf(R);
    if (card._y !== y) { card._y = y; card._R = R; card.style.top = y + "px"; }
  }
  function vgUpdate(force) {
    if (!VG.on || !VG.cols) return;
    const top = E.albumGrid.scrollTop;
    const firstRow = Math.max(0, Math.floor(top / VG.rowH) - VG.buf);
    if (!force && firstRow === VG.firstRow) return;   /* zero-write scroll frame */
    VG.firstRow = firstRow;
    const n = state.albCards.length;
    for (let R = firstRow; R < firstRow + VG.poolRows; R++) {
      const pr = R % VG.poolRows;
      for (let c = 0; c < VG.cols; c++)
        vgBind(VG.pool[pr * VG.cols + c], R * VG.cols + c, R, c);
    }
    /* near the end of loaded data → request the next page (replaces the
       flow-sentinel IntersectionObserver while virtual mode is active) */
    if (!state.albDone && (firstRow + VG.poolRows) * VG.cols >= n - VG.cols * 4)
      loadAlbums();
  }
  /* full re-init: metrics + pool + spacer + bind; corrects the estimated
     row height from a real card one frame later (once per call) */
  function vgRefresh() {
    if (!VG.on) return;
    /* LAYOUT-NOT-READY GUARD: at first paint (panel just un-hidden, fonts
       still loading) the grid can report clientWidth 0, so vgMetrics would
       compute cols=1 and build the whole library as ONE overlapping column
       — the "compacted albums until I resize" bug. Defer until the grid has
       a real width, retrying a few frames, and let the ResizeObserver below
       self-heal if the width only appears later. */
    if (E.albumGrid.clientWidth < 80) {
      if ((vgRefresh._tries = (vgRefresh._tries || 0) + 1) <= 30) {
        requestAnimationFrame(vgRefresh);
      }
      return;
    }
    vgRefresh._tries = 0;
    vgBuildPool();
    vgSetSpacerH();
    vgUpdate(true);
    requestAnimationFrame(() => {
      const c0 = VG.pool.find((c) => c._aid != null && c.style.display !== "none");
      if (!c0) return;
      const h = c0.offsetHeight;
      if (h > 0 && Math.abs(h + VG.gap - VG.rowH) > 1) {
        VG.rowH = h + VG.gap;
        vgSetSpacerH();
        VG.firstRow = -1;
        vgUpdate(true);
      }
    });
  }
  /* Force the compositor to COMMIT the first paint of freshly-bound cards.
     The album cards use `content-visibility:auto` + `contain:layout`, whose
     subtree paint Chromium DEFERS until an external layout/scroll/resize
     invalidation — which is exactly why covers stayed blank at launch until
     the window was resized/scrolled/tab-switched. After the first bind we
     synthesize that invalidation ourselves: a forced layout read + a 1px
     scroll bounce over two frames nudges the paint without any user gesture.
     Cheap and idempotent; only runs on the first page + view (re)entry. */
  function vgNudgePaint() {
    if (!VG.on || !E.albumGrid) return;
    requestAnimationFrame(() => {
      const g = E.albumGrid;
      /* forced reflow: reading a layout prop after the DOM writes above makes
         the engine flush layout for the now-visible cards */
      void g.offsetHeight;
      const st = g.scrollTop;
      /* a sub-pixel scroll toggle invalidates the content-visibility paint
         containers (same trigger a real scroll uses) without moving the view */
      g.scrollTop = st + 1;
      requestAnimationFrame(() => { g.scrollTop = st; });
    });
  }
  /* Self-heal: if the grid's usable width changes AFTER a build (first real
     layout, DPI/zoom change, panel reflow) and the column count no longer
     matches, rebuild the pool. Covers the case where the initial width was
     wrong and no user resize follows. */
  (function initVgResizeObserver() {
    if (!window.ResizeObserver || !E.albumGrid) return;
    let raf = 0, lastW = 0;
    const ro = new ResizeObserver((ents) => {
      const w = (ents[0] && ents[0].contentRect && ents[0].contentRect.width) || 0;
      if (w < 80 || Math.abs(w - lastW) < 4) return;
      lastW = w;
      if (VG.on && !raf) raf = requestAnimationFrame(() => { raf = 0; vgRefresh(); });
    });
    ro.observe(E.albumGrid);
  })();
  /* ---------- DETAILS-MODE WINDOWED LIST (variable heights) ----------
     Same two-spacer pattern as the queue, but rows vary in height (inline
     track lists). Heights: ESTIMATED from track_count until a row renders,
     then MEASURED via ResizeObserver (also catches the async track-list
     fill growing a row). Bounds the DOM to ~a screen of full-width rows —
     the scroll fix the virtual grid brought to grid mode, for details. */
  const DV = {
    on: false, padTop: null, padBot: null, first: -1,
    hMap: new Map(),          /* album id -> measured row height (+gap)  */
    rowCache: new Map(),      /* album id -> live card element. REUSED on
                                 every window move — rebuilding rows from
                                 scratch made the whole list FLASH per
                                 scroll step and refetched the inline
                                 track lists ("Loading…" flicker). */
    ro: null,
    artPx: +(localStorage.getItem("mn.dtart") || 150),
  };
  function dvDropCache() {
    for (const c of DV.rowCache.values()) {
      if (DV.ro) DV.ro.unobserve(c);
      adObs.unobserve(c);
    }
    DV.rowCache.clear();
  }
  function dvEstimate(a) {
    /* rows start WITHOUT their inline track list (it lazy-loads near the
       viewport), so unmeasured rows are uniform: art + padding. Estimating
       the loaded height here misplaced the window by hundreds of px per
       row; loaded rows get their real height from the ResizeObserver. */
    return Math.max(DV.artPx + 28, 120) + 14;            /* +14 row gap */
  }
  function dvH(a) { return DV.hMap.get(a.id) || dvEstimate(a); }
  function dvSetOn(on) {
    if (DV.on === on) return;
    DV.on = on;
    if (on) {
      for (const c of [...E.albumGrid.children])
        if (c !== E.albumSentinel) c.remove();
      DV.padTop = el("div", "dv-pad");
      DV.padBot = el("div", "dv-pad");
      if (E.albumSentinel) {
        E.albumGrid.insertBefore(DV.padTop, E.albumSentinel);
        E.albumGrid.insertBefore(DV.padBot, E.albumSentinel);
      } else {
        E.albumGrid.appendChild(DV.padTop);
        E.albumGrid.appendChild(DV.padBot);
      }
      if (!DV.ro) DV.ro = new ResizeObserver((es) => {
        let dirty = false;
        for (const e of es) {
          const id = e.target._dvId;
          if (id == null || !e.target.isConnected) continue;
          const h = e.target.offsetHeight + 14;
          if (Math.abs((DV.hMap.get(id) || 0) - h) > 1) { DV.hMap.set(id, h); dirty = true; }
        }
        if (dirty) dvUpdate(true);
      });
      E.albumGrid.style.setProperty("--dt-art", DV.artPx + "px");
      DV.first = -1;
      dvUpdate(true);
    } else {
      if (DV.padTop) { DV.padTop.remove(); DV.padBot.remove(); }
      DV.padTop = DV.padBot = null;
      DV.first = -1;
      dvDropCache();
    }
  }
  function dvUpdate(force) {
    if (!DV.on || !DV.padTop || !DV.padTop.isConnected) return;
    const g = E.albumGrid;
    const top = g.scrollTop, vh = g.clientHeight || 800, buf = vh;
    const cards = state.albCards || [];
    let y = 0, first = 0, firstY = 0, i;
    for (i = 0; i < cards.length; i++) {
      const h = dvH(cards[i]);
      if (y + h >= top - buf) break;
      y += h;
    }
    first = i; firstY = y;
    if (!force && first === DV.first) return;
    DV.first = first;
    let last = first, endY = firstY;
    for (i = first; i < cards.length; i++) {
      if (endY > top + vh + buf) break;
      endY += dvH(cards[i]);
      last = i + 1;
    }
    let totalH = endY;
    for (i = last; i < cards.length; i++) totalH += dvH(cards[i]);
    /* move the window between the pads — REUSING cached row elements so
       unchanged rows (and their loaded track lists) never rebuild */
    for (let n = DV.padTop.nextSibling; n && n !== DV.padBot;) {
      const nx = n.nextSibling; n.remove(); n = nx;   /* node lives on in rowCache */
    }
    const frag = document.createDocumentFragment();
    for (i = first; i < last; i++) {
      const a = cards[i];
      let card = DV.rowCache.get(a.id);
      if (!card) {
        card = albumCardEl(a);
        card._dvId = a.id;
        adObs.observe(card);    /* lazy inline track list */
        DV.ro.observe(card);    /* height corrections     */
        DV.rowCache.set(a.id, card);
      }
      frag.appendChild(card);
    }
    E.albumGrid.insertBefore(frag, DV.padBot);
    /* bound the cache: evict rows far outside the window */
    if (DV.rowCache.size > 150) {
      const keep = new Set();
      for (i = Math.max(0, first - 40); i < Math.min(cards.length, last + 40); i++)
        keep.add(cards[i].id);
      for (const [aid, c] of DV.rowCache) {
        if (keep.has(aid) || c.isConnected) continue;
        DV.ro.unobserve(c); adObs.unobserve(c);
        DV.rowCache.delete(aid);
      }
    }
    DV.padTop.style.height = Math.round(firstY) + "px";
    DV.padBot.style.height = Math.max(0, Math.round(totalH - endY)) + "px";
    if (!state.albDone && last >= cards.length - 10) loadAlbums();
  }
  function dvSetArt(px) {
    DV.artPx = Math.max(90, Math.min(220, px | 0));
    try { localStorage.setItem("mn.dtart", String(DV.artPx)); } catch (_) {}
    E.albumGrid.style.setProperty("--dt-art", DV.artPx + "px");
    DV.hMap.clear();            /* art size changes every row height */
    dvUpdate(true);
  }

  function vgSetOn(on) {
    if (VG.on === on) return;
    VG.on = on;
    E.albumGrid.classList.toggle("virtual", on);
    if (on) {
      for (const c of [...E.albumGrid.children])
        if (c !== VG.spacer && c !== E.albumSentinel) c.remove();
      vgRefresh();
    } else {
      if (VG.spacer) { VG.spacer.remove(); VG.spacer = null; }
      VG.pool = []; VG.cols = 0; VG.firstRow = -1;
      VG.panelRow = -1; VG.panelH = 0;
    }
  }

  /* The expand panel is a SEPARATE full-width grid item inserted into the album
     grid right after the clicked album's ROW (not anchored to the card), so it
     spans the window width, never overflows the margins, flows tracks in
     multiple columns, and pushes the rows below down cleanly. A pointer nub
     under the clicked album ties the bubble to its source. */

  /* Find the last card in the same visual grid row as `card`, so we insert the
     panel after that row and every album keeps its place while the rows below
     flow down to make room. */
  function lastCardInRow(card) {
    const cards = $$(".album-card", E.albumGrid);
    const i = cards.indexOf(card);
    if (i < 0) return card;
    const top = card.offsetTop;
    let last = card;
    for (let j = i + 1; j < cards.length; j++) {
      if (Math.abs(cards[j].offsetTop - top) < 4) last = cards[j];
      else break;
    }
    return last;
  }

  function positionNub(panel, card) {
    /* Align the ▲ nub horizontally under the clicked album cover, measured
       against the panel's own rect so it survives reflow/resize. */
    const panelR = panel.getBoundingClientRect();
    const cardR = card.getBoundingClientRect();
    const x = cardR.left - panelR.left + cardR.width / 2;
    panel.style.setProperty("--nub-x", x + "px");
  }

  function toggleExpand(card, a) {
    if (state.expandedAlbum === a.id) { collapseAlbum(); return; }
    collapseAlbum();                    /* close any other open panel first */

    card.classList.add("expanded");

    const panel = el("div", "album-expand");
    panel.dataset.id = a.id;
    panel.addEventListener("click", (ev) => ev.stopPropagation());

    /* header: cover thumb + title/meta + play/close */
    const head = el("div", "aex-head");
    const thumb = el("div", "aex-thumb");
    setArt(thumb, a.art, "1.4em", artKeyOf(a.artist, a.title));
    head.appendChild(thumb);
    const htext = el("div", "aex-htext");
    htext.appendChild(el("div", "aex-title", esc(a.title) || "Unknown album"));
    {
      const sub = el("div", "aex-sub");
      sub.appendChild(metaLinkSpan(a.artist || "—", a.artist, "artist"));
      if (a.year) sub.appendChild(el("span", null, "  ·  " + a.year));
      htext.appendChild(sub);
    }
    htext.appendChild(albumPills(a));
    head.appendChild(htext);
    const acts = el("div", "aex-acts");
    const playAll = el("button", "aex-play", "▶");
    playAll.title = "Play album";
    playAll.addEventListener("click", (ev) => { ev.stopPropagation(); send({ cmd: "playalbum", id: a.id }); });
    const closeBtn = el("button", "aex-close", "✕");
    closeBtn.addEventListener("click", (ev) => { ev.stopPropagation(); collapseAlbum(); });
    acts.appendChild(playAll); acts.appendChild(closeBtn);
    head.appendChild(acts);
    panel.appendChild(head);

    /* multi-column track list (auto-fills columns like the reference) */
    const list = el("div", "aex-tracks");
    list.appendChild(el("div", "aex-loading", "Loading tracks…"));
    panel.appendChild(list);

    if (VG.on) {
      /* VIRTUAL: the panel is an absolutely-positioned row inserted into the
         spacer at the clicked row's boundary; rows below shift down by the
         panel's height via vgYOf. Height lands async (track list), so a
         ResizeObserver propagates it into the spacer + card positions. */
      const idx = card._idx != null && card._idx >= 0
        ? card._idx
        : Math.max(0, state.albCards.indexOf(a));
      VG.panelRow = Math.floor(idx / VG.cols);
      VG.panelH = 0;
      panel.style.top = ((VG.panelRow + 1) * VG.rowH - VG.gap + 11) + "px";
      vgEnsureSpacer().appendChild(panel);
      const ro = new ResizeObserver(() => {
        const h = panel.offsetHeight + 22;
        if (Math.abs(h - VG.panelH) > 1) {
          VG.panelH = h;
          vgSetSpacerH();
          vgUpdate(true);
        }
      });
      ro.observe(panel);
      panel._ro = ro;
    } else {
      /* FLOW (details/carousel): insert right after the last card in the
         clicked card's visual row — a full-width flow item. */
      const anchor = lastCardInRow(card);
      anchor.after(panel);
    }

    state.expandedAlbum = a.id;
    state._expandPanel = panel;
    state._expandList = list;
    state._expandCard = card;

    requestAnimationFrame(() => {
      positionNub(panel, card);
      panel.classList.add("open");
      panel.scrollIntoView({ block: "nearest", behavior: "smooth" });
    });
    send({ cmd: "albumtracks", id: a.id });
    /* per-book resume + per-chapter positions + bookmarks (kind views) */
    if (state.activeKind) {
      send({ cmd: "bookresume", album: a.id });
      send({ cmd: "bookchapters", album: a.id });
      send({ cmd: "bookmarklist", album: a.id });
    }
  }

  /* long-form time for audiobooks: H:MM:SS above an hour */
  function fmtLong(ms) {
    if (!ms || ms < 0) return "0:00";
    const t = Math.floor(ms / 1000), h = Math.floor(t / 3600),
          m = Math.floor((t % 3600) / 60), s = t % 60;
    return h ? h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
             : m + ":" + String(s).padStart(2, "0");
  }
  on("bookresume", (m) => {
    if (!state._expandPanel) return;
    if (String(state.expandedAlbum) !== String(m.album)) return;
    if (!(m.track > 0) || !(m.pos_ms > 1000)) return;
    const acts = state._expandPanel.querySelector(".aex-acts");
    if (!acts || acts.querySelector(".aex-resume")) return;
    const btn = el("button", "aex-play aex-resume", "▶ Resume · " + fmtLong(m.pos_ms));
    btn.title = "Continue where you left off";
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      send({ cmd: "playalbum", id: m.album, track: m.track });
      /* seek once the track is up (playalbum is async through the engine) */
      setTimeout(() => send({ cmd: "seek", ms: m.pos_ms }), 800);
    });
    acts.insertBefore(btn, acts.firstChild);
  });

  /* per-chapter remembered positions: decorate the open expand panel's
     chapter rows with a resume chip; dblclick resumes at that spot. */
  state._chapterPos = null;   /* { album, map: {trackId: pos_ms} } */
  on("bookchapters", (m) => {
    if (!m || String(state.expandedAlbum) !== String(m.album)) return;
    const map = {};
    for (const c of m.chapters || []) if (c.pos_ms > 1000) map[String(c.track)] = c.pos_ms;
    state._chapterPos = { album: m.album, map };
    const panel = state._expandPanel;
    if (!panel) return;
    panel.querySelectorAll(".aex-track").forEach((row) => {
      const pos = map[String(row.dataset.id)];
      const old = row.querySelector(".aex-chpos");
      if (old) old.remove();
      if (pos) {
        const chip = el("span", "aex-chpos", "⏱ " + fmtLong(pos));
        chip.title = "Double-click the chapter to resume here";
        const mid = row.querySelector(".aex-tmid");
        if (mid) mid.appendChild(chip);
      }
    });
  });

  /* hours-listened per kind (shown in the library title) */
  state.kindStats = {};
  on("kindstats", (m) => {
    state.kindStats = {};
    (m.stats || []).forEach((s) => { state.kindStats[s.kind] = s.ms || 0; });
    updateViewTitle();
  });

  /* ============================================================
     CONTINUE LISTENING SHELF — recently-played books pinned above
     the grid in book/kind views. Fed by cmd:"continuebooks" (the
     book_progress DB). One click resumes the exact chapter+position
     (same playalbum→seek pattern as the expand-panel Resume button).
     Renders only when a kind view is active; hidden in music.
     ============================================================ */
  function continueShelfEl() {
    let sh = $("#continue-shelf");
    if (!sh) {
      const grid = $("#album-grid");
      if (!grid || !grid.parentNode) return null;
      sh = el("div", null, "");
      sh.id = "continue-shelf";
      sh.hidden = true;
      grid.parentNode.insertBefore(sh, grid);
    }
    return sh;
  }
  function hideContinueShelf() {
    const sh = $("#continue-shelf");
    if (sh) { sh.hidden = true; sh.innerHTML = ""; }
  }
  function fmtAgo(unixSec) {
    if (!unixSec) return "";
    const d = Math.max(0, Date.now() / 1000 - unixSec);
    if (d < 90)       return "just now";
    if (d < 3600)     return Math.round(d / 60) + "m ago";
    if (d < 86400)    return Math.round(d / 3600) + "h ago";
    return Math.round(d / 86400) + "d ago";
  }
  on("continuebooks", (m) => {
    const sh = continueShelfEl();
    if (!sh) return;
    const books = (m && m.books) || [];
    /* only meaningful in a kind view with actual history */
    if (!state.activeKind || !books.length) { hideContinueShelf(); return; }
    sh.innerHTML = "";
    const head = el("div", "cs-head", "");
    head.appendChild(el("span", "cs-title", "Continue listening"));
    const cnt = el("span", "cs-count", "");
    motion.countUp(cnt, books.length, books.length === 1 ? " book" : " books");
    head.appendChild(cnt);
    sh.appendChild(head);
    const row = el("div", "cs-row", "");
    for (const b of books) {
      const card = el("button", "cs-card" + (b.finished ? " done" : ""), "");
      card.title = (b.finished ? "Finished — play again" : "Resume") +
                   " · " + (b.album || "?") +
                   (b.chapter && b.chapter !== b.album ? " · " + b.chapter : "");
      /* cover */
      const artBox = el("div", "cs-art", "");
      if (typeof setArt === "function") setArt(artBox, b.art || "", "1.1em");
      card.appendChild(artBox);
      /* text column */
      const txt = el("div", "cs-txt", "");
      txt.appendChild(el("div", "cs-book", b.album || "(unknown book)"));
      const remain = Math.max(0, (b.duration_ms || 0) - (b.pos_ms || 0));
      const sub = b.finished
        ? "Finished · " + fmtAgo(b.updated)
        : Math.round((b.percent || 0) * 100) + "% · " +
          (remain > 1000 ? fmtLong(remain) + " left · " : "") + fmtAgo(b.updated);
      txt.appendChild(el("div", "cs-sub", sub));
      /* progress bar (static CSS width — no animation at idle) */
      const bar = el("div", "cs-bar", "");
      const fill = el("div", "cs-fill", "");
      fill.style.width = Math.round((b.percent || 0) * 100) + "%";
      bar.appendChild(fill);
      txt.appendChild(bar);
      card.appendChild(txt);
      card.addEventListener("click", () => {
        send({ cmd: "playalbum", id: b.album_id, track: b.track_id });
        if (!b.finished && b.pos_ms > 1000)
          setTimeout(() => send({ cmd: "seek", ms: b.pos_ms }), 800);
      });
      row.appendChild(card);
    }
    sh.appendChild(row);
    sh.hidden = false;
  });
  function refreshContinueShelf() {
    if (state.activeKind) {
      send({ cmd: "continuebooks", max: 12 });
      send({ cmd: "bookbadges" });
    } else hideContinueShelf();
  }

  /* per-book completion badges for the grid tiles (kind views) */
  state.bookBadges = null;
  on("bookbadges", (m) => {
    const map = {};
    for (const b of (m && m.badges) || [])
      map[String(b.album_id)] = { percent: b.percent || 0, finished: !!b.finished };
    /* UNCHANGED badges must not rebuild the pool — a full vgRefresh re-creates
       every visible card (art re-fades = the whole grid "blinks") each time a
       kind view opens. Only refresh when the data actually changed. */
    const sig = JSON.stringify(map);
    if (sig === state._bookBadgeSig) { state.bookBadges = map; return; }
    state._bookBadgeSig = sig;
    state.bookBadges = map;
    if (state.activeKind && typeof vgRefresh === "function") vgRefresh();
  });

  function collapseAlbum() {
    if (state._expandPanel) {
      if (state._expandPanel._ro) state._expandPanel._ro.disconnect();
      state._expandPanel.remove();
    }
    if (state._expandCard) state._expandCard.classList.remove("expanded");
    state._expandPanel = state._expandList = state._expandCard = null;
    state.expandedAlbum = null;
    if (VG.on && VG.panelRow >= 0) {
      VG.panelRow = -1; VG.panelH = 0;
      vgSetSpacerH();
      vgUpdate(true);
    }
  }

  /* Re-anchor the open panel after cards are appended or the grid resizes. */
  function reflowExpand() {
    if (!state._expandPanel) return;
    if (VG.on) {
      /* re-derive the row from the album's index (cols may have changed) */
      const idx = state.albCards.findIndex(
        (x) => String(x.id) === String(state.expandedAlbum));
      if (idx < 0) { collapseAlbum(); return; }
      VG.panelRow = Math.floor(idx / VG.cols);
      state._expandPanel.style.top =
        ((VG.panelRow + 1) * VG.rowH - VG.gap + 11) + "px";
      vgSetSpacerH();
      vgUpdate(true);
      const card = VG.pool.find((c) => c._idx === idx);
      if (card) positionNub(state._expandPanel, card);
      return;
    }
    if (!state._expandCard) return;
    if (!state._expandCard.isConnected) { collapseAlbum(); return; }
    const anchor = lastCardInRow(state._expandCard);
    if (anchor.nextElementSibling !== state._expandPanel) anchor.after(state._expandPanel);
    positionNub(state._expandPanel, state._expandCard);
  }

  on("albumtracks", (m) => {
    /* coerce ids to compare (bridge may send number or string) */
    if (String(state.expandedAlbum) !== String(m.id) || !state._expandList) return;
    const list = state._expandList; list.innerHTML = "";
    const rows = m.rows || [];
    /* multi-disc albums: section the list per disc (rows arrive ordered
       disc → track from the backend) */
    const discSet = new Set(rows.map((t) => t.disc || 0).filter((d) => d > 0));
    const multiDisc = discSet.size > 1;
    let lastDisc = -1;
    rows.forEach((t, i) => {
      if (multiDisc && (t.disc || 0) !== lastDisc) {
        lastDisc = t.disc || 0;
        list.appendChild(el("div", "aex-disc",
          lastDisc > 0 ? "Disc " + lastDisc : "Disc —"));
      }
      const isNow = state.now && state.now.playing && state.now.track_id === t.id;
      const row = el("div", "aex-track" + (isNow ? " playing" : ""));
      row.dataset.id = t.id;
      row.appendChild(el("div", "aex-tno", String(t.track_no || i + 1).padStart(2, "0")));
      const mid = el("div", "aex-tmid");
      mid.appendChild(el("span", "aex-ttitle", esc(t.title) || "Unknown title"));
      row.appendChild(mid);
      /* like/dislike on every album track */
      {
        const th = el("div", "aex-thumbs");
        const up = el("span", "thumb up" + (t.liked === 1 ? " on" : ""), "👍");
        up.title = "Like";
        const dn = el("span", "thumb down" + (t.liked === -1 ? " on" : ""), "👎");
        dn.title = "Dislike";
        const rate = (v) => {
          const next = (t.liked === v) ? 0 : v;
          send({ cmd: "like", id: t.id, v: next });
          t.liked = next;
          up.classList.toggle("on", next === 1);
          dn.classList.toggle("on", next === -1);
          if (state.now && state.now.track_id === t.id) updateThumbsUI(next);
        };
        up.addEventListener("click", (e) => { e.stopPropagation(); rate(1); });
        dn.addEventListener("click", (e) => { e.stopPropagation(); rate(-1); });
        th.appendChild(up); th.appendChild(dn);
        row.appendChild(th);
      }
      row.appendChild(el("div", "aex-tdur", fmtTime(t.duration_ms)));
      row.addEventListener("click", () => {
        $$(".aex-track.selected", list).forEach((x) => x.classList.remove("selected"));
        row.classList.add("selected");
      });
      row.addEventListener("dblclick", () => {
        send({ cmd: "playalbum", id: m.id, track: t.id });
        /* chapter-level resume: land at this chapter's remembered spot */
        const cp = state._chapterPos;
        const pos = cp && String(cp.album) === String(m.id) ? cp.map[String(t.id)] : 0;
        if (pos > 1000) setTimeout(() => send({ cmd: "seek", ms: pos }), 800);
      });
      list.appendChild(row);
    });
    if (!rows.length) list.appendChild(el("div", "aex-loading", "No tracks."));
    /* the track list changed the panel height — just re-anchor the nub */
    requestAnimationFrame(() => {
      if (!state._expandPanel || !state._expandCard) return;
      positionNub(state._expandPanel, state._expandCard);
    });
  });

  /* ============================================================
     ALBUM GRID — flow-rendered, windowed paging. Cards are normal
     grid children, so clicking one to expand inserts its track-list
     panel INLINE and the albums below flow down to make room (no
     virtual-geometry math). Infinite-scroll pages the next batch;
     off-screen cards are cheap via content-visibility.
     ============================================================ */
  const motion = MN.get("motion");
  const rolodexKick = motion.attachRolodex(E.albumGrid);

  /* ---- DETAILS mode: albums as full-width rows with INLINE track lists
     (WMP11 / MediaMonkey browse style). Track lists load lazily — an
     observer requests {cmd:"albumtracks"} only when a row nears the
     viewport, so the mode costs nothing until rows are actually seen. */
  const adPending = new Map();          /* album id -> its .card-tracks host */
  const adObs = new IntersectionObserver((ents) => {
    if (!E.albumGrid.classList.contains("mode-details")) return;
    ents.forEach((e) => {
      if (!e.isIntersecting) return;
      const card = e.target;
      const host = card.querySelector(".card-tracks");
      if (!host || host.dataset.loaded) return;
      host.dataset.loaded = "1";
      host.appendChild(el("div", "ct-loading", "Loading…"));
      adPending.set(String(card.dataset.id), host);
      send({ cmd: "albumtracks", id: +card.dataset.id });
    });
  }, { root: E.albumGrid, rootMargin: "500px" });

  /* tap(), NOT on() — the album-expand panel owns on("albumtracks") and a
     second on() would replace it (single-handler bus). */
  tap("albumtracks", (m) => {
    const host = adPending.get(String(m.id));
    if (!host) return;
    adPending.delete(String(m.id));
    host.innerHTML = "";
    const rows = m.tracks || m.rows || [];
    const ctDiscs = new Set(rows.map((t) => t.disc || 0).filter((d) => d > 0));
    const ctMulti = ctDiscs.size > 1;
    let ctLastDisc = -1;
    rows.forEach((t, i) => {
      if (ctMulti && (t.disc || 0) !== ctLastDisc) {
        ctLastDisc = t.disc || 0;
        host.appendChild(el("div", "ct-disc",
          ctLastDisc > 0 ? "Disc " + ctLastDisc : "Disc —"));
      }
      const r = el("div", "ct-row");
      r.appendChild(el("span", "ct-no", String(t.track_no || i + 1)));
      r.appendChild(el("span", "ct-title", esc(t.title) || "Unknown"));
      r.appendChild(rowThumbs(t));
      r.appendChild(el("span", "ct-dur", fmtTime(t.duration_ms)));
      r.addEventListener("dblclick", (ev) => {
        ev.stopPropagation();
        send({ cmd: "playalbum", id: m.id, track: t.id });
      });
      host.appendChild(r);
    });
    if (!rows.length) host.appendChild(el("div", "ct-loading", "No tracks."));
  });

  /* legacy flow render (details/carousel): every album as a grid child */
  function legacyRenderAlbums() {
    const frag = document.createDocumentFragment();
    state.albCards.forEach((a) => {
      const card = albumCardEl(a);
      adObs.observe(card);
      frag.appendChild(card);
    });
    if (E.albumSentinel) E.albumGrid.insertBefore(frag, E.albumSentinel);
    else E.albumGrid.appendChild(frag);
  }
  function applyAlbumMode() {
    const style = motion.get("albumStyle");
    E.albumGrid.classList.toggle("mode-carousel", style === "carousel");
    E.albumGrid.classList.toggle("mode-rolodex", style === "rolodex");
    E.albumGrid.classList.toggle("mode-details", style === "details");
    E.albumGrid.classList.toggle("mode-coverflow", style === "coverflow");
    collapseAlbum();
    const wantVirtual = style === "grid" || style === "rolodex";
    const wantDetails = style === "details";
    /* coverflow, like carousel, is a horizontal flow of real card children */
    const wantFlow = style === "carousel" || style === "coverflow";
    /* Deactivate coverflow FIRST when leaving it — it tears down the mesh
       canvas (a child of #album-grid); doing it before vgSetOn/dvSetOn purge
       the grid's children avoids orphaning the canvas while CF.mesh holds it. */
    if (style !== "coverflow") coverflowSetActive(false);
    if (!wantDetails && DV.on) dvSetOn(false);
    if (wantVirtual !== VG.on) vgSetOn(wantVirtual);
    if (wantDetails && !DV.on) { vgSetOn(false); dvSetOn(true); }
    if (wantFlow && !E.albumGrid.querySelector(".album-card"))
      legacyRenderAlbums();                     /* carousel/coverflow: flow children */
    /* the size slider only makes sense in details mode */
    { const s = $("#dt-size"); if (s) { s.hidden = !wantDetails; s.value = DV.artPx; } }
    rolodexKick();
    if (style === "coverflow") coverflowSetActive(true);
    if (typeof syncViewModeSeg === "function") syncViewModeSeg();
  }
  { const s = $("#dt-size");
    if (s) s.addEventListener("input", () => dvSetArt(+s.value)); }
  /* Cover Flow state — declared BEFORE applyAlbumMode()'s first call below,
     which reaches coverflowSetActive(); a `const` in the block further down
     would sit in the temporal dead zone here and throw during init, aborting
     the whole library load. (Assigned in the COVER FLOW section.) */
  let CF = null;
  applyAlbumMode();
  motion.onAlbumStyle(applyAlbumMode);
  if (E.albumSentinel) {
    const albObs = new IntersectionObserver((ents) => {
      if (ents.some((e) => e.isIntersecting) && state.view === 1) loadAlbums();
    }, { root: E.albumGrid, rootMargin: "900px" });
    albObs.observe(E.albumSentinel);
  }

  /* ============================================================
     COVER FLOW — an iPad/iTunes horizontal 3D carousel of album
     covers. The grid becomes a center-anchored horizontal strip:
     the card nearest the viewport center faces flat, the ones to
     each side rotate away (rotateY) and recede (translateZ), with
     depth-ordered stacking. The CENTERED cover upgrades to a live
     WebGL depth-mesh (its own MnDepthArt instance, sourced with
     the album's 1024px .hires.png + .depth.png) so the front cover
     is genuinely volumetric hi-def; side covers are flat hi-res
     <img> faces with a CSS tilt. Clicking a side cover brings it
     to center; clicking the centered cover expands its track list.

     PERFORMANCE: the tilt pass is scroll-driven, ONE rAF per burst
     (the rolodex model) and idles to zero — it never unparks the
     main app loop. The only continuous animator is the centered
     depth mesh, which self-parks on its worker and is torn down
     when the mode is left. This preserves the idle-CPU discipline
     documented at the app rAF loop (no 60fps-forever regression).
     ============================================================ */
  CF = {
    on: false, mesh: null, meshCanvas: null, meshCard: null,
    centerId: null, raf: 0, touched: [],
    /* physics: `pos` is the animated scrollLeft we glide toward `target`.
       vel carries flick momentum. animRaf drives the glide loop; it PARKS
       when settled (|pos-target| and |vel| both tiny) so idle CPU stays 0. */
    pos: 0, target: 0, vel: 0, animRaf: 0, animating: false,
    dragging: false, dragMoved: false, lastX: 0, lastT: 0, downX: 0, downScroll: 0,
  };
  function coverflowScroller() { return E.albumGrid; }
  const CF_STEP = () => {                    /* px between adjacent card centers */
    const first = E.albumGrid.querySelector(".album-card");
    if (!first) return 300;
    const w = first.offsetWidth || 300;
    /* effective pitch = card width + (two negative side margins) */
    return w * (1 + 2 * (-0.22)); // matches CSS margin:0 calc(cf-w * -0.22)
  };

  /* Lazily create the dedicated depth-mesh renderer for the front cover. */
  function cfEnsureMesh() {
    if (CF.mesh || !window.MnDepthArt || !MnDepthArt.create) return CF.mesh;
    const cnv = el("canvas", "cf-mesh-canvas");
    cnv.hidden = true;
    E.albumGrid.appendChild(cnv);          /* absolutely positioned overlay */
    CF.meshCanvas = cnv;
    const inst = MnDepthArt.create();
    if (!inst.mount(cnv)) { cnv.remove(); CF.meshCanvas = null; return null; }
    inst.setMotion("showcase");            /* gentle idle orbit on the front */
    CF.mesh = inst;
    return inst;
  }

  /* Point the front-cover mesh at whichever album is centered, and park it on
     the correct card. Called only when the centered album actually changes. */
  function cfMountMeshOn(card, a) {
    const mesh = cfEnsureMesh();
    if (!mesh || !a || !a.art) { cfHideMesh(); return; }
    cfShowFlat(true);                       /* un-hide the PREVIOUS card's cover */
    CF.meshCanvas.hidden = true;            /* hide stale mesh during re-source */
    CF.meshCard = card;
    const artUrl = a.art;
    const depthUrl = artSibling(artUrl, ".depth.png");
    const coverUrl = artSibling(artUrl, ".hires.png");
    mesh.setSources(artUrl, depthUrl, (ok) => {
      if (CF.centerId !== a.id) return;    /* centered card changed mid-load */
      if (ok && CF.on) {
        CF.meshCanvas.hidden = false;
        mesh.setActive(true);
        cfPositionMesh();
        /* the mesh IS the cover now — hide the flat <img> underneath so they
           don't double up (restored by cfHideMesh). */
        cfShowFlat(false);
      } else {
        cfHideMesh();                      /* depth map not ready — flat cover */
      }
    }, coverUrl);
  }
  /* toggle the centered card's flat cover image visibility */
  function cfShowFlat(show) {
    if (!CF.meshCard) return;
    const img = CF.meshCard.querySelector(".album-art img");
    if (img) img.style.visibility = show ? "" : "hidden";
  }
  function cfHideMesh() {
    if (CF.mesh) CF.mesh.setActive(false);
    if (CF.meshCanvas) CF.meshCanvas.hidden = true;
    cfShowFlat(true);                       /* bring the flat cover back */
    CF.meshCard = null;
  }
  /* Overlay the mesh canvas exactly onto the centered card's art box. */
  function cfPositionMesh() {
    if (!CF.meshCanvas || CF.meshCanvas.hidden || !CF.meshCard) return;
    const art = CF.meshCard.querySelector(".album-art");
    if (!art) return;
    const g = E.albumGrid.getBoundingClientRect();
    const r = art.getBoundingClientRect();
    const s = CF.meshCanvas.style;
    s.left = (r.left - g.left + E.albumGrid.scrollLeft) + "px";
    s.top = (r.top - g.top + E.albumGrid.scrollTop) + "px";
    s.width = r.width + "px";
    s.height = r.height + "px";
  }

  /* The tilt + center-detection pass. O(visible): read all card offsets in one
     batch, then write transforms only to the ~window of cards near center. */
  function cfPass() {
    CF.raf = 0;
    const grid = coverflowScroller();
    if (!CF.on || !grid.classList.contains("mode-coverflow")) { cfClearTouched(); return; }
    const cards = grid.querySelectorAll(".album-card");
    if (!cards.length) { cfHideMesh(); return; }
    const sl = grid.scrollLeft, vw = grid.clientWidth;
    const mid = sl + vw / 2;
    const lo = sl - vw, hi = sl + vw * 2;      /* ±1 viewport window */
    /* phase 1: pure reads */
    const inWin = [];
    let bestCard = null, bestA = null, bestDist = Infinity;
    for (let i = 0; i < cards.length; i++) {
      const c = cards[i];
      const cw = c.offsetWidth || 300;
      const cx = c.offsetLeft + cw / 2;
      if (cx + cw < lo) continue;
      if (cx - cw > hi) break;                 /* offsets are monotonic in x */
      inWin.push([c, cx, cw]);
      const dist = Math.abs(cx - mid);
      if (dist < bestDist) { bestDist = dist; bestCard = c; bestA = c._album; }
    }
    /* phase 2: pure writes.
       Cover Flow transform per card by its signed distance `d` from center
       (in card-widths). Center card faces flat & forward; neighbours rotate
       ~55°, recede modestly (so they stay bright, not lost to the dark), and
       tuck toward center. Tuned so ~4 covers are clearly visible each side. */
    const prev = CF.touched; CF.touched = [];
    const seen = new Set();
    for (const [c, cx, cw] of inWin) {
      const d = (cx - mid) / cw;               /* signed distance in card-widths */
      const cl = Math.max(-6, Math.min(6, d));
      const isCenter = c === bestCard;
      const sgn = cl < 0 ? -1 : 1;
      const ad = Math.abs(cl);
      /* iTunes Cover Flow: the FIRST side card does most of the turn; further
         cards share ~one steep angle and pack into a tight receding deck.
         Use a fast-saturating rotation so the neighbour is already ~68°. */
      const turn = isCenter ? 0 : sgn * (55 + 13 * Math.min(1, ad));   /* 68° by 1 card */
      const rot = turn;
      const z = isCenter ? 70 : -150 - Math.min(ad, 1) * 40 - Math.max(0, ad - 1) * 26;
      /* tuck neighbours toward center so the deck is tight (covers overlap,
         only a sliver of each far cover shows) */
      const tx = isCenter ? 0 : -sgn * (cw * 0.36) - (sgn * Math.max(0, ad - 1) * cw * 0.16);
      const sc = isCenter ? 1.0 : Math.max(0.76, 0.9 - ad * 0.025);
      c.style.transform =
        "translateX(" + tx.toFixed(1) + "px) translateZ(" + z.toFixed(1) + "px) " +
        "rotateY(" + rot.toFixed(2) + "deg) scale(" + sc.toFixed(3) + ")";
      c.style.zIndex = String(1000 - Math.round(Math.abs(cl) * 10));
      c.style.opacity = String(Math.max(0.62, 1 - Math.abs(cl) * 0.12));
      const wasCenter = c.classList.contains("cf-center");
      c.classList.toggle("cf-center", isCenter);
      if (isCenter && !wasCenter) {
        c.setAttribute("aria-current", "true");
        const a = c._album;
        if (a) c.setAttribute("aria-label",
          (a.title || "Album") + (a.artist ? " by " + a.artist : ""));
      } else if (!isCenter && wasCenter) {
        c.removeAttribute("aria-current");
      }
      CF.touched.push(c); seen.add(c);
    }
    for (const c of prev) {
      if (!seen.has(c)) {
        c.style.transform = ""; c.style.opacity = ""; c.style.zIndex = "";
        c.classList.remove("cf-center");
      }
    }
    /* centered album changed → re-source the front-cover depth mesh */
    const newId = bestA ? bestA.id : null;
    if (newId !== CF.centerId) {
      CF.centerId = newId;
      if (state.expandedAlbum != null && state.expandedAlbum !== newId) collapseAlbum();
      if (bestCard && bestA) cfMountMeshOn(bestCard, bestA);
      else cfHideMesh();
    } else {
      cfPositionMesh();                        /* same album, keep canvas glued */
    }
  }
  function cfClearTouched() {
    for (const c of CF.touched) {
      c.style.transform = ""; c.style.opacity = ""; c.style.zIndex = "";
      c.classList.remove("cf-center");
    }
    CF.touched = [];
  }
  function coverflowKick() { if (CF.on && !CF.raf) CF.raf = requestAnimationFrame(cfPass); }

  /* ---- Physics glide: the fluid, iTunes-style motion core -----------------
     Rather than lean on native smooth-scroll + CSS snap (steppy, fights
     momentum), we drive scrollLeft ourselves toward CF.target with critically-
     damped easing, plus flick inertia (CF.vel). The loop PARKS the instant it
     settles, so idle CPU is zero — same discipline as the app rAF loop. */
  function cfMaxScroll() {
    const g = E.albumGrid;
    return Math.max(0, g.scrollWidth - g.clientWidth);
  }
  function cfScrollForCard(card) {
    const g = E.albumGrid;
    return Math.max(0, Math.min(cfMaxScroll(),
      card.offsetLeft + card.offsetWidth / 2 - g.clientWidth / 2));
  }
  /* scrollLeft that centers the card nearest a given scrollLeft (snap target) */
  function cfSnapScroll(sl) {
    const cards = E.albumGrid.querySelectorAll(".album-card");
    if (!cards.length) return sl;
    const g = E.albumGrid, mid = sl + g.clientWidth / 2;
    let best = cards[0], bd = Infinity;
    for (const c of cards) {
      const cx = c.offsetLeft + c.offsetWidth / 2;
      const d = Math.abs(cx - mid);
      if (d < bd) { bd = d; best = c; }
    }
    return cfScrollForCard(best);
  }
  const cfReduced = () =>
    document.documentElement.classList.contains("no-anim") ||
    (window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches);

  function cfAnimTick() {
    CF.animRaf = 0;
    if (!CF.on) { CF.animating = false; return; }
    const g = E.albumGrid;
    /* inertia phase: if a flick left residual velocity and we're not chasing a
       fixed target yet, coast and decay, then snap when slow. */
    if (CF.vel !== 0 && CF.target == null) {
      CF.pos = Math.max(0, Math.min(cfMaxScroll(), CF.pos + CF.vel));
      CF.vel *= 0.95;                                  /* friction (longer glide) */
      if (CF.pos <= 0 || CF.pos >= cfMaxScroll()) CF.vel = 0;
      /* once slow, hand off to the eased snap so it settles ON a cover */
      if (Math.abs(CF.vel) < 1.0) { CF.vel = 0; CF.target = cfSnapScroll(CF.pos); }
    } else if (CF.target != null) {
      /* ease toward the snap/selected target — snappy but smooth */
      const d = CF.target - CF.pos;
      CF.pos += d * 0.22;
      if (Math.abs(d) < 0.5) { CF.pos = CF.target; CF.target = null; }
    }
    if (Math.abs(g.scrollLeft - CF.pos) > 0.5) g.scrollLeft = CF.pos;  /* drives cfPass via scroll */
    cfPass();
    /* continue while there is motion left */
    if (CF.vel !== 0 || CF.target != null) {
      CF.animRaf = requestAnimationFrame(cfAnimTick);
    } else { CF.animating = false; }
  }
  function cfStartAnim() {
    if (!CF.animating && CF.on) { CF.animating = true; CF.animRaf = requestAnimationFrame(cfAnimTick); }
  }
  function cfGlideTo(scrollLeft) {
    CF.vel = 0;
    CF.pos = E.albumGrid.scrollLeft;
    CF.target = Math.max(0, Math.min(cfMaxScroll(), scrollLeft));
    cfStartAnim();
  }
  function cfFlick(velPxPerFrame) {
    CF.target = null;
    CF.pos = E.albumGrid.scrollLeft;
    /* cap so a hard swipe glides a sane distance (~a dozen covers) instead of
       rocketing across the whole library */
    const cap = (CF_STEP() || 200) * 3.2;
    CF.vel = Math.max(-cap, Math.min(cap, velPxPerFrame));
    cfStartAnim();
  }
  /* move selection by ±N cards from the current center */
  function cfStep(dir) {
    const cards = Array.from(E.albumGrid.querySelectorAll(".album-card"));
    if (!cards.length) return;
    const g = E.albumGrid, mid = g.scrollLeft + g.clientWidth / 2;
    let idx = 0, bd = Infinity;
    cards.forEach((c, i) => {
      const cx = c.offsetLeft + c.offsetWidth / 2, d = Math.abs(cx - mid);
      if (d < bd) { bd = d; idx = i; }
    });
    const ni = Math.max(0, Math.min(cards.length - 1, idx + dir));
    cfCenterCard(cards[ni]);
  }

  /* Center a card. instant = jump (initial placement); else glide. */
  function cfCenterCard(card, instant) {
    if (!card) return;
    const sl = cfScrollForCard(card);
    if (instant || cfReduced()) {
      CF.vel = 0; CF.target = null; CF.pos = sl;
      E.albumGrid.scrollLeft = sl;
      coverflowKick();
    } else {
      cfGlideTo(sl);
    }
  }
  /* Coverflow click routing: a side cover centers; the centered cover expands
     its track list (below the strip). Installed once, delegated on the grid. */
  function cfOnClick(ev) {
    if (!CF.on || !E.albumGrid.classList.contains("mode-coverflow")) return;
    /* a drag that moved is a flick, not a click — swallow it */
    if (CF.dragMoved) { CF.dragMoved = false; ev.stopPropagation(); ev.preventDefault(); return; }
    const card = ev.target.closest(".album-card");
    if (!card || !E.albumGrid.contains(card)) return;
    /* let the ▶ play button work as-is */
    if (ev.target.closest(".album-play")) return;
    ev.stopPropagation();
    if (!card.classList.contains("cf-center")) { cfCenterCard(card); return; }
    /* centered: toggle its track-list expand */
    const a = card._album;
    if (a) cfToggleExpand(card, a);
  }
  /* Expand/collapse the centered album's track list as a full-width panel
     docked BELOW the horizontal strip (reuses the albumtracks bus + the
     shared expand state so on("albumtracks") fills it). */
  function cfToggleExpand(card, a) {
    if (state.expandedAlbum === a.id) { collapseAlbum(); return; }
    collapseAlbum();
    const panel = el("div", "album-expand cf-expand");
    panel.dataset.id = a.id;
    panel.addEventListener("click", (e) => e.stopPropagation());
    const head = el("div", "aex-head");
    const thumb = el("div", "aex-thumb");
    setArt(thumb, a.art, "1.4em", artKeyOf(a.artist, a.title));
    head.appendChild(thumb);
    const htext = el("div", "aex-htext");
    htext.appendChild(el("div", "aex-title", esc(a.title) || "Unknown album"));
    { const sub = el("div", "aex-sub");
      sub.appendChild(metaLinkSpan(a.artist || "—", a.artist, "artist"));
      if (a.year) sub.appendChild(el("span", null, "  ·  " + a.year));
      htext.appendChild(sub); }
    htext.appendChild(albumPills(a));
    head.appendChild(htext);
    const acts = el("div", "aex-acts");
    const playAll = el("button", "aex-play", "▶");
    playAll.title = "Play album";
    playAll.addEventListener("click", (e) => { e.stopPropagation(); send({ cmd: "playalbum", id: a.id }); });
    const closeBtn = el("button", "aex-close", "✕");
    closeBtn.addEventListener("click", (e) => { e.stopPropagation(); collapseAlbum(); });
    acts.appendChild(playAll); acts.appendChild(closeBtn);
    head.appendChild(acts);
    panel.appendChild(head);
    const list = el("div", "aex-tracks");
    list.appendChild(el("div", "aex-loading", "Loading tracks…"));
    panel.appendChild(list);
    /* Dock it below the strip. It must live in the NON-scrolling view panel,
       NOT inside #album-grid — the grid scrolls horizontally and is many
       screens wide, so an absolutely-positioned child there anchors to the
       scroll content (spilling far off-screen). #view-albums is fixed-width. */
    const host = E.albumGrid.parentNode || E.albumGrid;
    host.appendChild(panel);
    state.expandedAlbum = a.id;
    state._expandPanel = panel;
    state._expandList = list;
    state._expandCard = card;
    send({ cmd: "albumtracks", id: a.id });
  }

  function coverflowSetActive(on) {
    if (!CF) return;                 /* early init: CF not assigned yet */
    if (on === CF.on) { if (on) coverflowKick(); return; }
    CF.on = on;
    /* accessibility: make the strip a focusable listbox so keyboard users can
       arrow through covers; drop the affordances when leaving the mode. */
    if (E.albumGrid) {
      if (on) {
        E.albumGrid.setAttribute("role", "listbox");
        E.albumGrid.setAttribute("aria-label", "Albums — Cover Flow");
        E.albumGrid.setAttribute("aria-orientation", "horizontal");
        E.albumGrid.tabIndex = 0;
      } else {
        E.albumGrid.removeAttribute("role");
        E.albumGrid.removeAttribute("aria-label");
        E.albumGrid.removeAttribute("aria-orientation");
        E.albumGrid.removeAttribute("tabindex");
      }
    }
    if (on) {
      coverflowKick();
      /* Center the now-playing album (or the first) once cards exist. The
         eager album load streams pages in, so retry briefly until cards are
         present, then jump (no smooth-scroll on the initial placement so it
         lands instantly at center rather than animating from scrollLeft 0). */
      let tries = 0;
      const place = () => {
        if (!CF.on) return;
        const cards = E.albumGrid.querySelectorAll(".album-card");
        if (!cards.length) { if (tries++ < 30) setTimeout(place, 100); return; }
        let target = null;
        if (state.now && state.now.track_album) {
          for (const c of cards) {
            const a = c._album;
            if (a && a.title === state.now.track_album &&
                (!state.now.track_album_artist || a.artist === state.now.track_album_artist)) { target = c; break; }
          }
        }
        /* fall back to a card a little into the list so covers fan out on BOTH
           sides (a strip anchored at card[0] leaves a big empty left half) */
        if (!target) target = cards[Math.min(cards.length - 1, 8)];
        cfCenterCard(target, true);          /* instant, not smooth */
        coverflowKick();
      };
      requestAnimationFrame(place);
    } else {
      cfClearTouched();
      cfHideMesh();
      CF.centerId = null;
      if (CF.raf) { cancelAnimationFrame(CF.raf); CF.raf = 0; }
    }
  }
  /* scroll-driven tilt (one rAF/burst) + resize re-glue */
  if (E.albumGrid) {
    /* the DOM class is the ground truth for "coverflow is showing"; gating on
       it (not just CF.on) means a desynced CF.on can never hijack a normal
       grid scroll or preventDefault a normal wheel. */
    const cfLive = () => CF.on && E.albumGrid.classList.contains("mode-coverflow");
    /* passive scroll → repaint the tilt (covers native wheel/scrollbar/a11y
       scrolling; our own glide sets scrollLeft which also fires this) */
    let cfScrollEndT = 0;
    E.albumGrid.addEventListener("scroll", () => {
      if (!cfLive()) return;
      if (!CF.dragging && !CF.animating) {
        coverflowKick();                        /* external scroll: repaint */
        /* settle ON a cover after native momentum-scroll stops (trackpad) */
        clearTimeout(cfScrollEndT);
        cfScrollEndT = setTimeout(() => {
          if (cfLive() && !CF.dragging && !CF.animating) {
            const snap = cfSnapScroll(E.albumGrid.scrollLeft);
            if (Math.abs(snap - E.albumGrid.scrollLeft) > 1) cfGlideTo(snap);
          }
        }, 90);
      }
    }, { passive: true });
    E.albumGrid.addEventListener("click", cfOnClick, true);
    if (window.ResizeObserver)
      new ResizeObserver(() => { if (CF.on) coverflowKick(); }).observe(E.albumGrid);

    /* ---- WHEEL → horizontal flick. Vertical wheels (deltaY only, most mice)
       must still move the strip; translate any wheel delta into horizontal
       glide toward the neighbouring cover, iTunes-fashion. */
    /* the DOM class is the ground truth for "is coverflow showing"; gating on
       it (not just CF.on) guarantees we NEVER preventDefault a normal grid/
       tracks wheel, even if CF.on ever desyncs from the visible mode. */
    E.albumGrid.addEventListener("wheel", (e) => {
      if (!cfLive()) return;
      const dom = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
      if (!dom) return;
      e.preventDefault();
      cfStep(dom > 0 ? 1 : -1);
    }, { passive: false });

    /* ---- POINTER DRAG → flick with inertia (the signature interaction). */
    E.albumGrid.addEventListener("pointerdown", (e) => {
      if (!cfLive() || e.button !== 0) return;
      if (e.target.closest(".album-play, .cf-expand")) return;
      CF.dragging = true; CF.dragMoved = false;
      CF.downX = CF.lastX = e.clientX;
      CF.downScroll = E.albumGrid.scrollLeft;
      CF.vel = 0; CF.target = null; CF.pos = E.albumGrid.scrollLeft;
      CF.lastT = e.timeStamp;
      try { E.albumGrid.setPointerCapture(e.pointerId); } catch (_) {}
    });
    E.albumGrid.addEventListener("pointermove", (e) => {
      if (!CF.dragging) return;
      const dx = e.clientX - CF.lastX;
      if (Math.abs(e.clientX - CF.downX) > 4) CF.dragMoved = true;
      const sl = Math.max(0, Math.min(cfMaxScroll(), E.albumGrid.scrollLeft - dx));
      E.albumGrid.scrollLeft = sl; CF.pos = sl;
      /* velocity in px/frame (~16ms) from the last move */
      const dt = Math.max(1, e.timeStamp - CF.lastT);
      CF.vel = -dx * (16 / dt);
      CF.lastX = e.clientX; CF.lastT = e.timeStamp;
      coverflowKick();
    });
    const endDrag = (e) => {
      if (!CF.dragging) return;
      CF.dragging = false;
      try { E.albumGrid.releasePointerCapture(e.pointerId); } catch (_) {}
      if (cfReduced()) { cfGlideTo(cfSnapScroll(E.albumGrid.scrollLeft)); return; }
      if (Math.abs(CF.vel) > 1.2) cfFlick(CF.vel);           /* fling */
      else cfGlideTo(cfSnapScroll(E.albumGrid.scrollLeft));  /* just snap */
    };
    E.albumGrid.addEventListener("pointerup", endDrag);
    E.albumGrid.addEventListener("pointercancel", endDrag);

    /* ---- KEYBOARD (accessibility): arrows move one cover, Home/End jump,
       Enter/Space opens the centered album. Grid is focusable in cf mode. */
    E.albumGrid.addEventListener("keydown", (e) => {
      if (!cfLive()) return;
      const cards = E.albumGrid.querySelectorAll(".album-card");
      if (!cards.length) return;
      switch (e.key) {
        case "ArrowRight": e.preventDefault(); cfStep(1); break;
        case "ArrowLeft":  e.preventDefault(); cfStep(-1); break;
        case "Home":       e.preventDefault(); cfCenterCard(cards[0]); break;
        case "End":        e.preventDefault(); cfCenterCard(cards[cards.length - 1]); break;
        case "Enter":
        case " ": {
          e.preventDefault();
          const c = E.albumGrid.querySelector(".album-card.cf-center");
          if (c && c._album) cfToggleExpand(c, c._album);
          break;
        }
      }
    });
  }

  /* preserveScroll=true → an IN-PLACE refresh (scan finished, art cache
     cleared, artfetch landed): the album set's identity is unchanged, so the
     user's scroll position is kept through the reload instead of teleporting
     to the top (measured on cold boot: a stationary grid at scrollTop 230k
     was dumped to 7100 with zero input when the backfill's scan-state change
     triggered a refetch). Full resets (kind switch, search, sort) omit it. */
  function resetAlbums(preserveScroll) {
    /* capture BEFORE collapseAlbum — closing an expand panel shifts layout */
    const keepTop = (preserveScroll && VG.on) ? E.albumGrid.scrollTop : 0;
    state._albKeepTop = keepTop > 0 ? keepTop : null;
    collapseAlbum();
    albGen++;              /* drop in-flight replies from the old sequence */
    adPending.clear();     /* details-mode track lists headed for dead nodes */
    /* albLoading must clear too: a reset mid-eager-chain drops the in-flight
       reply (offset guard), and a stuck true here would block loadAlbums */
    state.albCards = []; state.albOffset = 0; state.albTotal = 0;
    state.albDone = false; state.albLoading = false;
    rolodexKick();         /* release the tilt pass's refs to the old cards */
    /* coverflow: fully release the front-cover mesh — replaceChildren() below
       detaches its canvas, so tear the instance down; cfEnsureMesh rebuilds it
       lazily when the new sequence's centered album loads. */
    if (CF.on || CF.mesh) {
      CF.centerId = null; CF.touched = [];
      if (CF.mesh) { CF.mesh.destroy(); CF.mesh = null; }
      CF.meshCanvas = null; CF.meshCard = null;
    }
    if (VG.on) {
      /* keep the pool; blank it and zero the spacer. When preserving scroll,
         the spacer KEEPS its height — collapsing it would make the browser
         clamp scrollTop to 0 before the refreshed pages stream back in. */
      for (const c of VG.pool) { c._aid = null; c._idx = -1; c.style.display = "none"; }
      VG.firstRow = -1;
      if (state._albKeepTop == null) vgSetSpacerH();
    } else if (DV.on) {
      DV.hMap.clear(); DV.first = -1;
      dvDropCache();
      for (let n = DV.padTop && DV.padTop.nextSibling; n && n !== DV.padBot;) {
        const nx = n.nextSibling; n.remove(); n = nx;
      }
      if (DV.padTop) { DV.padTop.style.height = "0px"; DV.padBot.style.height = "0px"; }
    } else if (E.albumSentinel) E.albumGrid.replaceChildren(E.albumSentinel);
    else E.albumGrid.replaceChildren();
    if (state._albKeepTop == null) E.albumGrid.scrollTop = 0;
  }
  function loadAlbums() {
    if (state.albLoading || state.albDone) return;
    state.albLoading = true;
    /* stamp the kind this request is being made under, so a reply that arrives
       after a kind switch (boot race, fast toggling) can be dropped even if it
       shares a gen — defense in depth against cross-library content. */
    state._albReqKind = state.activeKind || "";
    send({ cmd: "albums", offset: state.albOffset, count: APAGE, gen: albGen });
  }

  /* Above this size, fall back to sentinel paging (a pathological "albums"
     grouping); below it the WHOLE set is loaded eagerly so the scrollbar
     represents the full library from the start. */
  const ALB_EAGER_MAX = 5000;

  on("albums", (m) => {
    /* stale-reply guards: gen catches a superseded sort/search sequence
       (two sequences can both start at offset 0 — the offset check alone
       cannot tell them apart and mixed-ordering pages corrupted the grid);
       the offset check catches in-sequence duplicates. */
    if (m.gen != null && m.gen !== albGen) return;
    if ((m.offset || 0) !== state.albOffset) return;
    /* kind guard: the active library changed since this request went out (the
       reply is for a different kind) → drop it so no cross-library content
       paints. C scopes the query by active_kind at build time, so the reply's
       CONTENT already matches whatever kind was active when C processed it;
       comparing the request-time kind to the current kind rejects a reply that
       raced across a switch. */
    if ((state._albReqKind || "") !== (state.activeKind || "")) { state.albLoading = false; return; }
    state.albLoading = false;
    state.albTotal = m.total || 0;
    const albums = m.albums || [];
    const base = state.albCards.length;
    const frag = document.createDocumentFragment();
    state.albOffset += albums.length;
    if (!albums.length || state.albOffset >= state.albTotal) state.albDone = true;
    if (VG.on) {
      /* VIRTUAL: albums are pure DATA — no elements are built. Grow the
         spacer (exact scrollbar) and rebind only if the new page intersects
         the on-screen window (blank slots at the tail fill in). */
      albums.forEach((a) => state.albCards.push(a));
      if (base === 0) { vgRefresh(); vgNudgePaint(); }  /* first page: metrics + pool + paint */
      else { vgSetSpacerH(); vgUpdate(true); }
      /* scroll-preserving in-place refresh: once the reloaded data reaches
         the preserved offset (or the load finished), drop the spacer floor,
         settle the exact height and rebind at the kept position. The user
         never moved — this is Winamp's "repaint in place", not a jump. */
      if (state._albKeepTop != null) {
        const need = state._albKeepTop + (VG.clientH || E.albumGrid.clientHeight || 800);
        if (state.albDone || vgRows() * VG.rowH >= need) {
          const keep = state._albKeepTop;
          state._albKeepTop = null;
          vgSetSpacerH();                       /* exact height (floor removed) */
          if (Math.abs(E.albumGrid.scrollTop - keep) > 1)
            E.albumGrid.scrollTop = keep;       /* clamps if the set shrank */
          vgUpdate(true);
        }
      }
    } else if (DV.on) {
      /* DETAILS window: also pure data */
      albums.forEach((a) => state.albCards.push(a));
      dvUpdate(true);
    } else {
      albums.forEach((a, i) => {
        state.albCards.push(a);
        const card = albumCardEl(a);
        if (base === 0 && i < 14) { card.classList.add("stagger"); card.style.setProperty("--i", String(i)); }
        adObs.observe(card);   /* lazy inline track lists (details mode only) */
        frag.appendChild(card);
      });
      /* one DOM insertion per page, ahead of the scroll sentinel */
      if (E.albumSentinel) E.albumGrid.insertBefore(frag, E.albumSentinel);
      else E.albumGrid.appendChild(frag);
    }
    /* EAGER FULL LOAD: chain the next page immediately (200/page) instead of
       waiting for the scroll sentinel. In virtual mode a page is just 200
       array pushes + a spacer-height write, so "everything present" is
       near-free — and the scrollbar means something: grab it and jump
       ANYWHERE. */
    if (!state.albDone && state.albTotal <= ALB_EAGER_MAX) {
      state.albLoading = true;
      send({ cmd: "albums", offset: state.albOffset, count: 200, gen: albGen });
    }
    /* keep an open panel anchored after new cards land */
    reflowExpand();
    /* rolodex tilt for the newly-added cards */
    if (motion.get("albumStyle") === "rolodex") rolodexKick();
    /* coverflow: re-run the tilt/center pass over the enlarged strip */
    if (motion.get("albumStyle") === "coverflow") coverflowKick();
    /* warm decoded covers for smoother scroll */
    const urls = albums.map((a) => a.art).filter(Boolean);
    MN.get("artram").warmMany(urls);
    updateViewTitle();
  });

  /* ============================================================
     SYSTEM-WIDE VIEW MODE — topbar segment: List (every track,
     WMP-style) / Grid (album covers) / Details (albums + inline
     track lists). One control, applies to the library content.
     ============================================================ */
  function syncViewModeSeg() {
    const style = motion.get("albumStyle");
    [["vm-list",      state.view === 0],
     ["vm-grid",      state.view === 1 && style !== "details" && style !== "coverflow"],
     ["vm-coverflow", state.view === 1 && style === "coverflow"],
     ["vm-details",   state.view === 1 && style === "details"]]
      .forEach(([id, on2]) => {
        const b = $("#" + id);
        if (b) b.classList.toggle("active", !!on2);
      });
  }
  (function initViewModeSeg() {
    const L = $("#vm-list"), G = $("#vm-grid"), C = $("#vm-coverflow"), D = $("#vm-details");
    if (L) L.addEventListener("click", () => { switchView(0); });
    if (G) G.addEventListener("click", () => {
      /* leave the special full-cover styles for the plain grid */
      const st = motion.get("albumStyle");
      if (st === "details" || st === "coverflow") motion.set("albumStyle", "grid");
      if (state.view !== 1) switchView(1);
      syncViewModeSeg();
    });
    if (C) C.addEventListener("click", () => {
      if (motion.get("albumStyle") !== "coverflow") motion.set("albumStyle", "coverflow");
      if (state.view !== 1) switchView(1);
      syncViewModeSeg();
    });
    if (D) D.addEventListener("click", () => {
      if (motion.get("albumStyle") !== "details") motion.set("albumStyle", "details");
      if (state.view !== 1) switchView(1);
      syncViewModeSeg();
    });
    syncViewModeSeg();
  })();

  /* ============================================================
     SCROLL POSITION POPUP — a pill that rides the scrollbar and
     shows WHERE you are in terms of the active sort key (letter,
     year, rating, …), MediaMonkey/phone-contacts style.
     ============================================================ */
  (function initScrollPop() {
    let pill = null, hideT = 0;
    function ensurePill(panel) {
      if (pill && pill.parentNode === panel) return pill;
      if (pill) pill.remove();
      pill = el("div", "scroll-pop");
      pill.appendChild(el("div", "scroll-pop-main", ""));
      pill.appendChild(el("div", "scroll-pop-sub", ""));
      panel.appendChild(pill);
      return pill;
    }
    function letterOf(s) {
      const c = String(s || "").trim().charAt(0).toUpperCase();
      if (!c) return "·";
      if (/[0-9]/.test(c)) return "#";
      return c;
    }
    /* label for a track row under the current sort key */
    function trackLabel(row) {
      if (!row) return null;
      switch (state.sort) {
        case "artist":   return letterOf(row.artist);
        case "album":    return letterOf(row.album);
        case "genre":    return row.genre ? letterOf(row.genre) : "·";
        case "year":     return row.year ? String(row.year) : "—";
        case "duration": return fmtTime(row.duration_ms);
        case "rating":   return row.rating ? "★" + row.rating : "unrated";
        case "plays":    return (row.play_count || 0) + "×";
        case "added": {
          if (!row.date_added) return "—";
          const d = new Date(row.date_added * 1000);
          return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0");
        }
        case "track":    return "#" + (row.track_no || 0);
        default:         return letterOf(row.title);   /* title sort */
      }
    }
    function show(panel, scroller, main, sub) {
      const p = ensurePill(panel);
      const mainEl = p.querySelector(".scroll-pop-main");
      const subEl = p.querySelector(".scroll-pop-sub");
      if (mainEl.textContent !== main) mainEl.textContent = main;
      if (subEl.textContent !== sub) subEl.textContent = sub;
      /* ride alongside the scrollbar thumb — positioned with `translate`
         (compositor-only, independent of the .show transform) */
      const range = scroller.scrollHeight - scroller.clientHeight;
      const frac = range > 0 ? scroller.scrollTop / range : 0;
      const top = scroller.offsetTop + 14 + frac * Math.max(0, scroller.clientHeight - 74);
      p.style.translate = "0 " + Math.round(top) + "px";
      p.classList.add("show");
      clearTimeout(hideT);
      hideT = setTimeout(() => { if (pill) pill.classList.remove("show"); }, 650);
    }
    /* Scroll events can outpace frames — coalesce each scroller's pill
       update into one rAF so a flick costs one read+write batch per frame. */
    /* body.scrolling marker: added while any registered scroller is moving,
       removed ~160ms after it stops. CSS uses it to suppress the per-cover
       opacity fade-in (compositor-layer churn) during a fast scroll. */
    let scrollOffTimer = 0;
    function markScrolling() {
      if (!document.body.classList.contains("scrolling"))
        document.body.classList.add("scrolling");
      clearTimeout(scrollOffTimer);
      scrollOffTimer = setTimeout(
        () => document.body.classList.remove("scrolling"), 160);
    }
    function rafScroll(elm, fn) {
      let queued = 0;
      elm.addEventListener("scroll", () => {
        markScrolling();
        if (!queued) queued = requestAnimationFrame(() => { queued = 0; fn(); });
      }, { passive: true });
    }
    /* row height is read from a cached var, refreshed by the custom hook
       (was a localStorage read per scroll event) */
    let rhCache = MN.get("custom").rowHeight() || 46;
    MN.get("custom").onRowHeight((v) => { rhCache = v || 46; });
    /* tracks: rows are flow-rendered; index by measured row height */
    if (E.trackScroll) rafScroll(E.trackScroll, () => {
      const total = state.trkTotal || 0;
      if (!total) return;
      const idx = clamp(Math.floor(E.trackScroll.scrollTop / rhCache), 0, total - 1);
      const row = state.trkRows[idx];
      const lbl = trackLabel(row);
      show(E.trackScroll.parentNode, E.trackScroll,
           lbl != null ? lbl : "…",
           (idx + 1).toLocaleString() + " / " + total.toLocaleString());
    });
    /* FAR-JUMP GUARD: on a jump larger than one viewport (scrollbar grab,
       Home/End, letter-rail) the rAF-coalesced rebind below lands a frame
       late — recycled pool cards were briefly visible at the new offset
       still carrying the PREVIOUS album's cover (measured: a NONE album's
       tile showed a loaded wrong cover for ~1 frame). Rebind SYNCHRONOUSLY
       in the scroll event for jumps only: vgBind rebuilds each recycled
       card's content in the same task, so no stale-cover frame can ever be
       composited. Row-by-row scrolling keeps the cheap rAF path. */
    if (E.albumGrid) {
      let lastJumpTop = 0;
      E.albumGrid.addEventListener("scroll", () => {
        const t = E.albumGrid.scrollTop;
        const far = Math.abs(t - lastJumpTop) > (E.albumGrid.clientHeight || 800);
        lastJumpTop = t;
        if (far && VG.on && VG.cols) vgUpdate(true);
      }, { passive: true });
    }
    /* albums: virtual grid — rebind the pool when the scroll crosses a row
       (vgUpdate early-exits with ZERO writes while inside the same row, so
       most frames are pure compositor scroll) */
    if (E.albumGrid) rafScroll(E.albumGrid, () => {
      vgUpdate();
      dvUpdate();
      const total = state.albTotal || 0;
      if (!total || !state.albCards.length) return;
      const range = E.albumGrid.scrollHeight - E.albumGrid.clientHeight;
      const frac = range > 0 ? E.albumGrid.scrollTop / range : 0;
      const loaded = state.albCards.length;
      const idx = clamp(Math.round(frac * (loaded - 1)), 0, loaded - 1);
      const a = state.albCards[idx];
      show(E.albumGrid.parentNode, E.albumGrid,
           a ? letterOf(a.title) : "…",
           (idx + 1).toLocaleString() + " / " + total.toLocaleString());
    });
  })();

  /* Give the album grid a bottom spacer sized to the yet-unloaded albums so the
     scrollbar reflects the WHOLE library, not just what's loaded. Cheap: one
     element, height = (remaining rows) * (row height). */
  function updateAlbGridPad() { /* superseded by virtual grid */ }

  /* ============================================================
     FOLDERS VIEW — library sources with hide checkboxes.
     Checked = visible; unchecking sends folderhidden:true so the
     whole library can ignore e.g. a prep folder. Degrades to a
     "waiting" note until the backend answers {"cmd":"folders"}.
     ============================================================ */
  const foldersList = $("#folders-list");
  state.folders = [];
  let folderAckPending = false;

  function loadFolders() {
    if (!state.roots.length && !state.folders.length) {
      foldersList.innerHTML = "";
      foldersList.appendChild(el("div", "folders-empty", "Waiting for the audio engine…"));
    } else renderFolders();
    send({ cmd: "folders" });   /* hidden-banner + legacy-derivation data */
    send({ cmd: "roots" });     /* the authoritative top-level list        */
  }
  function setFolderHidden(id, hidden) {
    folderAckPending = true;
    send({ cmd: "folderhidden", id, hidden });
  }
  function folderRowEl(f, i) {
    const r = el("div", "folder-row stagger");
    r.style.setProperty("--i", String(Math.min(i, 12)));
    if (f.hidden) r.classList.add("is-hidden");
    const chk = el("label", "fcheck");
    const input = el("input");
    input.type = "checkbox";
    input.checked = !f.hidden;
    input.title = f.hidden ? "Show this folder's content" : "Hide this folder's content";
    input.addEventListener("change", () => setFolderHidden(f.id, !input.checked));
    chk.appendChild(input);
    chk.appendChild(el("span", "box"));
    r.appendChild(chk);
    const p = el("div", "folder-path", esc(f.path) || "—");
    p.title = esc(f.path);
    r.appendChild(p);
    /* content-kind designation: music / audiobook / podcast / any custom
       label (e.g. "ost"). Each non-music kind is its OWN isolated library
       with its own sidebar entry; changing it re-partitions instantly. */
    {
      const sel = el("select", "folder-kind");
      sel.title = "Content type — non-music kinds become their own isolated library";
      const opts = ["music", "audiobook", "podcast"];
      (state.kinds || []).forEach((k) => { if (!opts.includes(k)) opts.push(k); });
      const cur = f.kind || "music";
      if (!opts.includes(cur)) opts.push(cur);
      opts.forEach((k) => {
        const o = el("option", null, kindLabel(k));
        o.value = k;
        if (k === cur) o.selected = true;
        sel.appendChild(o);
      });
      const custom = el("option", null, "Custom…");
      custom.value = "__custom__";
      sel.appendChild(custom);
      sel.addEventListener("click", (ev) => ev.stopPropagation());
      sel.addEventListener("change", () => {
        let k = sel.value;
        if (k === "__custom__") {
          k = String(prompt("Custom content type (e.g. OST):", "") || "").trim().toLowerCase();
          if (!k) { sel.value = cur; return; }
        }
        send({ cmd: "setrootkind", path: f.path, kind: k });
        f.kind = k;
        state.albDirty = true;          /* partitions changed */
        setTimeout(() => send({ cmd: "folders" }), 150);
      });
      r.appendChild(sel);
    }
    if (f.hidden) {
      const hb = el("span", "folder-hidden-badge", "HIDDEN");
      hb.title = "This folder's tracks are excluded from all views and search until you re-check it";
      r.appendChild(hb);
    }
    const n = f.track_count || 0;
    r.appendChild(el("div", "folder-count", n.toLocaleString() + (n === 1 ? " track" : " tracks")));
    /* PERMANENT remove (not hide): drops the folder's tracks from the
       library and the roots registry. Files on disk are untouched. */
    {
      const rm = el("button", "folder-remove", "✕");
      rm.title = "Remove this folder from the library (files on disk are NOT deleted)";
      rm.addEventListener("click", (ev) => {
        ev.stopPropagation();
        if (!confirm("Remove \"" + f.path + "\" and its " + n.toLocaleString() +
                     " tracks from the library?\n\nFiles on disk are NOT deleted.")) return;
        send({ cmd: "removefolder", id: f.id });
        send({ cmd: "removeroot", path: f.path });   /* registry entry too */
        state.albDirty = true;
        setTimeout(() => { send({ cmd: "folders" }); send({ cmd: "kinds" }); }, 250);
      });
      r.appendChild(rm);
    }
    return r;
  }
  /* ---------- UNIFIED ROOTS (single source of truth) ----------
     Both the FOLDERS VIEW and the ADD-FOLDER dialog list the SAME set: the
     manually-added top-level roots from the C registry ({"cmd":"roots"},
     folder_kinds.txt + per-root stats). The old split — view showing every
     track-containing SUBfolder, dialog showing a localStorage mirror — gave
     two different lists (user-reported as an obvious error). Legacy
     libraries whose roots predate the registry fall back to deriving top
     folders from the DB folder dimension. */
  state.roots = [];
  state.rootsLoaded = false;
  /* tap(), NOT on(): the Settings -> Library panel already owns on("roots")
     and the bus is single-handler-per-type — an on() here silently replaced
     (or was replaced by) that handler, so state.roots never populated and
     the Folders view fell back to a music-only derived list forever (the
     "only music folders are listed" bug). */
  tap("roots", (m) => {
    state.roots = m.roots || [];
    state.rootsLoaded = true;
    renderFolders();
    if (typeof renderKindFolders === "function") renderKindFolders();
    updateViewTitle();
  });
  function unifiedRoots() {
    if (state.roots.length) {
      return state.roots.map((r) => ({
        path: r.path,
        kind: (!r.kind || r.kind === "music") ? "music"
              : (r.kind === "audiobooks" ? "audiobook" : r.kind),
        tracks: r.tracks || 0, albums: r.albums || 0,
      }));
    }
    if (!state.rootsLoaded) return null;   /* still loading — don't guess */
    /* registry genuinely empty: collapse the DB folder dimension */
    const derived = deriveLegacyRoots(state.folders.map((f) => f.path));
    return derived.map((p) => {
      let tracks = 0;
      state.folders.forEach((f) => { if (pathUnder(p, f.path)) tracks += f.track_count || 0; });
      return { path: p, kind: "music", tracks: tracks, albums: 0 };
    });
  }
  function rootRowEl(root) {
    const r = el("div", "folder-row");
    const p = el("div", "folder-path", esc(root.path) || "—");
    p.title = esc(root.path);
    r.appendChild(p);
    /* content-kind designation (music/audiobook/custom) — see fffb3f5 */
    {
      const sel = el("select", "folder-kind");
      sel.title = "Content type — non-music kinds become their own isolated library";
      const opts = ["music", "audiobook", "podcast"];
      (state.kinds || []).forEach((k) => { if (!opts.includes(k)) opts.push(k); });
      const cur = root.kind || "music";
      if (!opts.includes(cur)) opts.push(cur);
      opts.forEach((k) => {
        const o = el("option", null, kindLabel(k));
        o.value = k;
        if (k === cur) o.selected = true;
        sel.appendChild(o);
      });
      const custom = el("option", null, "Custom…");
      custom.value = "__custom__";
      sel.appendChild(custom);
      sel.addEventListener("click", (ev) => ev.stopPropagation());
      sel.addEventListener("change", () => {
        let k = sel.value;
        if (k === "__custom__") {
          k = String(prompt("Custom content type (e.g. OST):", "") || "").trim().toLowerCase();
          if (!k) { sel.value = cur; return; }
        }
        send({ cmd: "setrootkind", path: root.path, kind: k });
        state.albDirty = true;
        setTimeout(() => send({ cmd: "roots" }), 250);
      });
      r.appendChild(sel);
    }
    const n = root.tracks || 0;
    r.appendChild(el("div", "folder-count",
      n.toLocaleString() + (n === 1 ? " track" : " tracks") +
      (root.albums ? " · " + root.albums.toLocaleString() + " albums" : "")));
    {
      const rm = el("button", "folder-remove", "✕");
      rm.title = "Remove this folder from the library (files on disk are NOT deleted)";
      rm.addEventListener("click", (ev) => {
        ev.stopPropagation();
        if (!confirm("Remove \"" + root.path + "\" and its " + n.toLocaleString() +
                     " tracks from the library?\n\nFiles on disk are NOT deleted.")) return;
        send({ cmd: "removetree", path: root.path });
        state.albDirty = true;
        setTimeout(() => { send({ cmd: "folders" }); }, 400);
      });
      r.appendChild(rm);
    }
    return r;
  }
  function renderFolders() {
    foldersList.innerHTML = "";
    const roots = unifiedRoots();
    if (roots === null) {
      foldersList.appendChild(el("div", "folders-empty", "Loading folders…"));
      return;
    }
    if (!roots.length) {
      foldersList.appendChild(el("div", "folders-empty", "No folders yet — add one with “＋ Add folder”."));
      return;
    }
    roots.forEach((root) => foldersList.appendChild(rootRowEl(root)));
  }
  /* ---- Manually-added library ROOTS (the add-folder dialog's list) --------
     ROOT-CAUSE of the "popup lists dozens of subfolders" bug: the dialog fed
     straight off {"cmd":"folders"} = the DB's folders dimension table, i.e.
     every directory that DIRECTLY contains tracks. The C side collapses that
     list to entries with no listed ancestor — but a picked root like
     D:\Music holds no loose audio files itself, so it never gets a row and
     every album subfolder survives the collapse. The real roots live in
     <exe>\folder_kinds.txt, which no bridge command exposes. Fix: mirror the
     roots in localStorage (mn.libroots) at addfolder time; for roots added
     before this existed, derive them by collapsing sibling directories into
     their common parent (never into a bare drive/share root). */
  const LIBROOTS_KEY = "mn.libroots";
  function normPath(p) { return String(p || "").replace(/[\\/]+$/, ""); }
  function pathKey(p) { return normPath(p).toLowerCase(); }
  /* is `p` at-or-under `root`? */
  function pathUnder(root, p) {
    const r = pathKey(root), c = pathKey(p);
    if (!r || !c) return false;
    return c === r || c.indexOf(r + "\\") === 0 || c.indexOf(r + "/") === 0;
  }
  function loadLibRoots() {
    try {
      const a = JSON.parse(localStorage.getItem(LIBROOTS_KEY) || "[]");
      return Array.isArray(a) ? a.filter((r) => r && r.path) : [];
    } catch (_) { return []; }
  }
  function saveLibRoots(a) { try { localStorage.setItem(LIBROOTS_KEY, JSON.stringify(a)); } catch (_) {} }
  function addLibRoot(path, kind) {
    const roots = loadLibRoots();
    /* skip exact dupes AND paths already covered by an existing root */
    if (!roots.some((r) => pathUnder(r.path, path))) {
      roots.push({ path: normPath(path), kind: kind || "music" });
      saveLibRoots(roots);
    }
  }
  function removeLibRoot(path) {
    saveLibRoots(loadLibRoots().filter((r) => !pathUnder(path, r.path)));
  }
  function parentDir(p) {
    const n = normPath(p);
    const i = Math.max(n.lastIndexOf("\\"), n.lastIndexOf("/"));
    return i > 0 ? n.slice(0, i) : "";
  }
  /* bare drive ("F:") or UNC share ("\\server\share") — never a library root */
  function isFsRoot(p) {
    const n = normPath(p);
    return !n || /^[a-z]:$/i.test(n) || /^\\\\[^\\/]+([\\/][^\\/]+)?$/.test(n);
  }
  /* Legacy fallback for roots that predate mn.libroots: start from the
     track-containing dirs and repeatedly merge >=2 siblings into their
     parent. One multi-album root collapses to the root itself; deliberate
     sibling roots survive because a bare drive never absorbs them. */
  function deriveLegacyRoots(paths) {
    const minimal = (list) => list.filter((p, i) =>
      !list.some((q, j) => j !== i && pathKey(q) !== pathKey(p) && pathUnder(q, p)));
    let cur = paths.map(normPath).filter(Boolean);
    const seen0 = {};
    cur = cur.filter((p) => { const k = pathKey(p); if (seen0[k]) return false; seen0[k] = true; return true; });
    cur = minimal(cur);
    for (let pass = 0; pass < 24; pass++) {
      const byParent = {};
      cur.forEach((p) => {
        const pa = parentDir(p);
        if (!pa || isFsRoot(pa)) return;
        const k = pathKey(pa);
        (byParent[k] || (byParent[k] = { parent: pa, kids: [] })).kids.push(p);
      });
      let merged = false;
      const consumed = {};
      const next = [];
      Object.keys(byParent).forEach((k) => {
        const g = byParent[k];
        if (g.kids.length >= 2) {
          merged = true;
          g.kids.forEach((p) => { consumed[pathKey(p)] = true; });
          next.push(g.parent);
        }
      });
      cur.forEach((p) => { if (!consumed[pathKey(p)]) next.push(p); });
      const seen = {};
      cur = minimal(next.filter((p) => { const k = pathKey(p); if (seen[k]) return false; seen[k] = true; return true; }));
      if (!merged) break;
    }
    return cur;
  }

  /* The add-folder dialog's list: ONLY the manually-added roots — never the
     auto-discovered subfolders. Counts aggregate everything under each root;
     remove drops every known folder row under the root. */
  const kindFoldersEl = $("#kind-libfolders");
  function renderKindFolders() {
    if (!kindFoldersEl) return;
    kindFoldersEl.innerHTML = "";
    /* SAME unified list as the Folders view — one source of truth */
    const roots = unifiedRoots() || [];
    if (!roots.length) {
      kindFoldersEl.appendChild(el("div", "kind-lib-empty", "No library folders yet."));
      return;
    }
    roots.forEach((root) => {
      const r = el("div", "kind-lib-row");
      const p = el("div", "kind-lib-path", esc(root.path) || "—");
      p.title = esc(root.path);
      r.appendChild(p);
      if (root.kind && root.kind !== "music")
        r.appendChild(el("span", "kind-lib-kind", root.kind));
      r.appendChild(el("span", "kind-lib-count", (root.tracks || 0).toLocaleString() + " ♪"));
      const x = el("button", "kind-lib-remove", "✕");
      x.title = "Remove this folder (and everything under it) from the library";
      x.addEventListener("click", () => {
        if (!confirm("Remove “" + root.path + "” from the library?\nYour audio files on disk are not touched.")) return;
        x.disabled = true;
        removeLibRoot(root.path);                  /* legacy mirror stays clean */
        send({ cmd: "removetree", path: root.path });
        state.albDirty = true;
        setTimeout(() => send({ cmd: "folders" }), 400);
      });
      r.appendChild(x);
      kindFoldersEl.appendChild(r);
    });
  }

  function updateHiddenBanner() {
    const n = state.folders.filter((f) => f.hidden).length;
    $$(".hidden-banner").forEach((b) => {
      b.hidden = !n;
      $(".hb-txt", b).textContent =
        n + (n === 1 ? " folder hidden" : " folders hidden") + " — showing partial library";
    });
  }
  $$(".hidden-banner .hb-showall").forEach((btn) => btn.addEventListener("click", () => {
    state.folders.filter((f) => f.hidden).forEach((f) => setFolderHidden(f.id, false));
  }));
  on("folders", (m) => {
    state.folders = Array.isArray(m.folders) ? m.folders : [];
    if (state.view === 4) { renderFolders(); updateViewTitle(); }
    updateHiddenBanner();
    if (!$("#kind-overlay").hidden) renderKindFolders();
    if (folderAckPending) {
      folderAckPending = false;
      /* visibility changed → counts/pages changed; reload the library view */
      if (state.view === 1) { resetAlbums(); loadAlbums(); }
      else if (typeof state.view === "number" && state.view !== 4) { resetTracks(); loadTracks(); }
    }
  });

  /* ============================================================
     VIEW SWITCHING
     ============================================================ */
  function showPanel(id) {
    ["view-tracks", "view-albums", "view-facet", "view-playlists", "view-folders", "view-generic", "view-models", "view-lyrics", "view-stats", "view-search", "view-mediatool"].forEach((p) => { const e = $("#" + p); if (e) e.hidden = (p !== id); });
    if (id !== "view-search") state.searchOpen = false;
  }
  /* titleFx channel: every title change funnels through here — scramble
     decodes into the new text; slide replays a rise-in; off is instant. */
  function applyTitle(t) {
    if (E.viewTitle.textContent === t) return;
    const fx = motion.get("titleFx");
    if (fx === "scramble") { motion.scramble(E.viewTitle, t); return; }
    E.viewTitle.textContent = t;
    if (fx === "slide" && !motion.reduced()) {
      E.viewTitle.classList.remove("mo-title-new");
      void E.viewTitle.offsetWidth;
      E.viewTitle.classList.add("mo-title-new");
    }
  }
  function updateViewTitle() {
    /* the unified search panel owns the title while it is open (page
       replies for background views would overwrite "Results for …") */
    if (state.searchOpen) return;
    if (state.view === "models") { applyTitle("AI Models"); return; }
    if (state.view === "lyrics") { applyTitle("Lyrics"); return; }
    if (state.view === "stats")  { applyTitle("Statistics"); return; }
    if (state.view === "mediatool") { applyTitle("Media Manager"); return; }
    if (state.activeKind && state.view === 1) {
      const c = state.albTotal || 0;
      const lbl = kindLabel(state.activeKind);
      let t2 = c ? lbl + "  ·  " + c.toLocaleString() : lbl;
      const ms = (state.kindStats || {})[state.activeKind] || 0;
      if (ms >= 60000)
        t2 += "  ·  " + (ms / 3600000).toFixed(ms >= 3600000 ? 1 : 2) + " h listened";
      applyTitle(t2);
      return;
    }
    let t = VIEW_TITLES[state.view] || "Library";
    if (state.query) t = 'Results for "' + state.query + '"';
    let count = 0;
    if (state.view === 0) count = state.trkTotal;
    else if (state.view === 1) count = state.albTotal;
    else if (state.view === 4) count = (state.roots.length || state.folders.length);
    applyTitle(count ? t + "  ·  " + count.toLocaleString() : t);
  }

  function switchView(v) {
    /* a pending search debounce would yank the user BACK into the results
       panel ~100ms after they deliberately navigated away — cancel it */
    clearTimeout(searchTimer);
    clearTimeout(sugTimer);
    /* Cover Flow's live depth mesh must not keep rendering when the Albums
       view is hidden — park it on the way out, re-arm on the way in. */
    if (state.view === 1 && v !== 1) coverflowSetActive(false);
    state.view = v;
    if (v === 1 && motion.get("albumStyle") === "coverflow") coverflowSetActive(true);
    if (state.category === "liked") send({ cmd: "likedonly", on: false });
    state.category = null; state.categoryFilter = null;
    /* Navigating to a normal LIBRARY view (Tracks/Albums/…) leaves any per-kind
       library (audiobook/ost/…) → return to music scope + forget the last kind. */
    if (state.activeKind) { setCategoryMode(""); try { localStorage.removeItem("mn.lastkind"); } catch (_) {} }
    if (typeof updateCatHeader === "function") updateCatHeader();
    try { localStorage.setItem("mn.lastview", String(v)); } catch (_) {}
    if (typeof window.__mnNavPush === "function") window.__mnNavPush();
    $$(".nav-item").forEach((n) => n.classList.toggle("active", +n.dataset.view === v));
    if (typeof syncViewModeSeg === "function") syncViewModeSeg();

    /* Folders gets a dedicated panel driven by {"cmd":"folders"} — we do NOT
       send the numeric view so the C-side track filter stays untouched. */
    if (v === 4) { showPanel("view-folders"); loadFolders(); updateViewTitle(); return; }
    send({ cmd: "view", v });

    if (v === 0) { showPanel("view-tracks"); resetTracks(); loadTracks(); }
    else if (v === 1) {
      showPanel("view-albums");
      /* Boot with a pending kind restore (Audiobooks/OST/…): do NOT load the
         MUSIC grid here — openKindView (fired from the async `kinds` reply)
         will send `category` + load the correct kind. Loading music first is
         exactly the "wrong library flashes at launch" bug. */
      if (state._pendingBootKind) {
        /* show the empty albums panel; the kind load repaints it shortly */
      } else if (state.albDirty || !state.albCards.length) {
        /* Keep the loaded grid (cards + decoded art + scroll position) when
           nothing changed — re-entering Albums no longer starts over from
           page 0. A search, scan or library edit sets albDirty. */
        state.albDirty = false;
        resetAlbums(); loadAlbums();
      } else {
        updateViewTitle();
      }
    }
    else if (v === 2) { showPanel("view-facet"); openFacet("artists", 1); }
    else if (v === 3) { showPanel("view-facet"); openFacet("genres", 4); }
    else if (v === 5) { showPanel("view-playlists"); loadPlaylists(); }
    else {
      showPanel("view-tracks"); resetTracks(); loadTracks();
    }
    updateViewTitle();
  }

  /* ============================================================
     ARTISTS / GENRES / YEARS browse — a facet grid that drills
     into a track list for the selected value (cmd:facettracks).
     ============================================================ */
  const facetState = { kind: null, dim: 1 };
  function openFacet(kind, dim) {
    facetState.kind = kind; facetState.dim = dim;
    const grid = $("#facet-grid"), tracks = $("#facet-tracks"), back = $("#facet-back");
    if (tracks) { tracks.hidden = true; tracks.innerHTML = ""; }
    if (back) back.hidden = true;
    if (grid) { grid.hidden = false; grid.innerHTML = '<div class="aex-loading">Loading…</div>'; }
    send({ cmd: kind, offset: 0, count: 1000 });
  }
  let facetRows = [];   /* backs the delegated click (dataset index -> row) */
  function renderFacet(m, kind) {
    if (facetState.kind !== kind) return;
    const grid = $("#facet-grid");
    if (!grid) return;
    grid.innerHTML = "";
    const rows = m.rows || [];
    facetRows = rows;
    /* Build into a fragment and insert ONCE (was appending per card to the
       live grid -> an incremental reflow per card, up to 1000 = a visible
       hitch on the Artists/Genres view). One delegated click listener on
       the grid instead of one per card (was up to 1000 listeners). */
    const frag = document.createDocumentFragment();
    rows.forEach((r, i) => {
      const card = el("div", "facet-card");
      card.dataset.fi = String(i);
      card.appendChild(el("div", "facet-name", esc(r.label) || "Unknown"));
      card.appendChild(el("div", "facet-count", r.count + (r.count === 1 ? " track" : " tracks")));
      frag.appendChild(card);
    });
    grid.appendChild(frag);
    if (!rows.length) grid.appendChild(el("div", "aex-loading", "Nothing here yet."));
  }
  { const grid = $("#facet-grid");
    if (grid) grid.addEventListener("click", (ev) => {
      const card = ev.target.closest(".facet-card");
      if (!card || card.dataset.fi == null) return;
      const r = facetRows[+card.dataset.fi];
      if (r) drillFacet(r);
    }); }
  function drillFacet(r) {
    const grid = $("#facet-grid"), tracks = $("#facet-tracks"), back = $("#facet-back");
    if (grid) grid.hidden = true;
    if (back) { back.hidden = false; back.textContent = "← " + (r.label || "Back"); }
    if (tracks) { tracks.hidden = false; tracks.innerHTML = '<div class="aex-loading">Loading…</div>'; }
    send({ cmd: "facettracks", dim: facetState.dim, value_id: r.id });
  }
  { const fb = $("#facet-back"); if (fb) fb.addEventListener("click", () => openFacet(facetState.kind, facetState.dim)); }
  on("artists", (m) => renderFacet(m, "artists"));
  on("genres",  (m) => renderFacet(m, "genres"));
  on("years",   (m) => renderFacet(m, "years"));
  on("facettracks", (m) => {
    const tracks = $("#facet-tracks");
    if (!tracks || tracks.hidden) return;
    tracks.innerHTML = "";
    const rows = m.rows || [];
    state.trkRows = rows.slice();   /* so like/rating/context menus work here */
    rows.forEach((row, i) => tracks.appendChild(trackRowEl(row, i)));
    if (!rows.length) tracks.appendChild(el("div", "aex-loading", "No tracks."));
  });

  /* ============================================================
     PLAYLISTS — grid of playlist cards, drill-in to member tracks,
     create / rename / delete, and "Add to playlist" from rows.
     ============================================================ */
  let playlistsCache = [];
  let curPlaylist = null;
  function loadPlaylists() {
    const grid = $("#pl-grid"), tracks = $("#pl-tracks"), back = $("#pl-back");
    curPlaylist = null;
    if (tracks) { tracks.hidden = true; tracks.innerHTML = ""; }
    if (back) back.hidden = true;
    if (grid) { grid.hidden = false; grid.innerHTML = '<div class="aex-loading">Loading…</div>'; }
    send({ cmd: "playlists" });
  }
  on("playlists", (m) => {
    playlistsCache = m.rows || [];
    const grid = $("#pl-grid");
    const panel = $("#view-playlists");
    /* keep the cache fresh for the picker even when the view is hidden */
    if (!grid || grid.hidden || !panel || panel.hidden) return;
    grid.innerHTML = "";
    const create = el("div", "facet-card pl-create");
    create.appendChild(el("div", "facet-name", "＋ New playlist"));
    create.addEventListener("click", () => {
      const name = prompt("Playlist name:", "New Playlist");
      if (name && name.trim()) send({ cmd: "playlistcreate", name: name.trim() });
    });
    grid.appendChild(create);
    playlistsCache.forEach((p) => {
      const card = el("div", "facet-card");
      card.appendChild(el("div", "facet-name", esc(p.name) || "Untitled"));
      card.appendChild(el("div", "facet-count", (p.count || 0) + ((p.count === 1) ? " track" : " tracks")));
      const acts = el("div", "pl-card-acts");
      const ren = el("span", "pl-act", "✎"); ren.title = "Rename";
      ren.addEventListener("click", (e) => { e.stopPropagation(); const n = prompt("Rename playlist:", p.name); if (n && n.trim()) send({ cmd: "playlistrename", id: p.id, name: n.trim() }); });
      const exp = el("span", "pl-act", "⭳"); exp.title = "Export as .m3u8";
      exp.addEventListener("click", (e) => { e.stopPropagation(); send({ cmd: "playlistexport", id: p.id, name: p.name || "playlist" }); });
      const del = el("span", "pl-act", "🗑"); del.title = "Delete";
      del.addEventListener("click", (e) => { e.stopPropagation(); if (confirm('Delete playlist "' + p.name + '"?')) send({ cmd: "playlistdelete", id: p.id }); });
      acts.appendChild(ren); acts.appendChild(exp); acts.appendChild(del);
      card.appendChild(acts);
      card.addEventListener("click", () => drillPlaylist(p));
      grid.appendChild(card);
    });
    if (!playlistsCache.length) grid.appendChild(el("div", "aex-loading", "No playlists yet — create one."));
  });
  function drillPlaylist(p) {
    curPlaylist = p;
    const grid = $("#pl-grid"), tracks = $("#pl-tracks"), back = $("#pl-back");
    if (grid) grid.hidden = true;
    if (back) { back.hidden = false; back.textContent = "← " + (p.name || "Playlists"); }
    if (tracks) { tracks.hidden = false; tracks.innerHTML = '<div class="aex-loading">Loading…</div>'; }
    send({ cmd: "playlisttracks", id: p.id });
  }
  { const pb = $("#pl-back"); if (pb) pb.addEventListener("click", loadPlaylists); }
  on("playlisttracks", (m) => {
    const tracks = $("#pl-tracks");
    if (!tracks || tracks.hidden) return;
    if (!curPlaylist || String(curPlaylist.id) !== String(m.id)) return;
    tracks.innerHTML = "";
    const rows = m.rows || [];
    state.trkRows = rows.slice();
    rows.forEach((row, i) => {
      const rEl = trackRowEl(row, i);
      const rm = el("span", "pl-remove", "✕"); rm.title = "Remove from playlist";
      rm.addEventListener("click", (e) => { e.stopPropagation(); send({ cmd: "playlistremoveat", id: curPlaylist.id, position: i }); });
      rEl.appendChild(rm);
      tracks.appendChild(rEl);
    });
    if (!rows.length) tracks.appendChild(el("div", "aex-loading", "Empty playlist."));
  });

  /* "Add to playlist" picker — a small popover listing playlists.
     `trackId` may be a single id or an array of ids (bulk add). */
  function showAddToPlaylist(trackId, anchorEv) {
    const ids = Array.isArray(trackId) ? trackId : [trackId];
    if (!playlistsCache.length) send({ cmd: "playlists" });
    const menu = el("div", "pl-picker");
    menu.appendChild(el("div", "pl-picker-head",
      ids.length > 1 ? ("Add " + ids.length + " tracks to playlist") : "Add to playlist"));
    const list = el("div", "pl-picker-list");
    playlistsCache.forEach((p) => {
      const it = el("div", "pl-picker-item", esc(p.name));
      it.addEventListener("click", () => {
        ids.forEach((id) => send({ cmd: "playlistadd", id: p.id, track_id: id }));
        close();
      });
      list.appendChild(it);
    });
    menu.appendChild(list);
    const mk = el("div", "pl-picker-item pl-picker-new", "＋ New playlist…");
    mk.addEventListener("click", () => {
      const name = prompt("New playlist name:", "New Playlist");
      if (name && name.trim()) send({ cmd: "playlistcreate", name: name.trim() });
      close();
    });
    menu.appendChild(mk);
    document.body.appendChild(menu);
    const x = anchorEv ? anchorEv.clientX : 200, y = anchorEv ? anchorEv.clientY : 200;
    menu.style.left = Math.min(x, window.innerWidth - 240) + "px";
    menu.style.top = Math.min(y, window.innerHeight - 300) + "px";
    function close() {
      document.removeEventListener("click", onDoc, true);
      motion.close(menu, () => menu.remove());   /* graceful out */
    }
    function onDoc(e) { if (!menu.contains(e.target)) close(); }
    setTimeout(() => document.addEventListener("click", onDoc, true), 0);
  }
  window.__mnAddToPlaylist = showAddToPlaylist;
  /* warm the playlist cache once at boot so the picker has entries */
  send({ cmd: "playlists" });

  /* ============================================================
     LEFT-PANE CATEGORIES ("Smart" group) — TRACKS-view lists with
     a preset sort + (where the backend lacks one) a client filter.
     ============================================================ */
  const CATS = {
    liked:  { name: "Liked songs",    sort: "added",  asc: false },  /* DB-filtered via likedonly */
    added:  { name: "Recently Added",  sort: "added",  asc: false },
    played: { name: "Most Played",     sort: "plays",  asc: false },
    recent: { name: "Recently Played", sort: "played", asc: false },
  };
  const catHeader = $("#cat-header");
  function updateCatHeader() {
    const c = state.category ? CATS[state.category] : null;
    catHeader.hidden = !c;
    if (!c) return;
    $(".cat-name", catHeader).textContent = c.name;
    $(".cat-note", catHeader).textContent = c.note || "";
  }
  function openCategory(key) {
    const c = CATS[key];
    if (!c) return;
    state.category = key;
    state.categoryFilter = c.filter || null;
    state.sort = c.sort;
    state.view = 0;
    try { localStorage.setItem("mn.lastview", "0"); } catch (_) {}
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "cat:" + key));
    if (E.sort) { E.sort.value = /^(title|artist|album|year|rating|added)$/.test(c.sort) ? c.sort : ""; }
    clearColSortInd();
    updateCatHeader();
    showPanel("view-tracks");
    send({ cmd: "view", v: 0 });
    /* "Liked songs" filters at the DB layer so EVERY liked track appears —
       the old client-side filter only saw the loaded pages. */
    send({ cmd: "likedonly", on: key === "liked" });
    send({ cmd: "sort", by: c.sort, asc: c.asc });
    resetTracks(); loadTracks();
    updateViewTitle();
  }
  /* noRefresh: the caller (runSearch) does its own reset+fetch right after —
     without it a category-exit ran resetTracks/loadTracks TWICE. */
  function clearCategory(noRefresh) {
    if (!state.category) return;
    state.category = null; state.categoryFilter = null;
    send({ cmd: "likedonly", on: false });
    updateCatHeader();
    if (!noRefresh) switchView(0);
  }
  $(".cat-clear", catHeader).addEventListener("click", () => clearCategory());

  /* infinite scroll: fetch the next page as the sentinel nears the viewport */
  if (E.trackSentinel) {
    E.trackSentinel.style.display = "";
    const trkObs = new IntersectionObserver((ents) => {
      if (ents.some((e) => e.isIntersecting) && state.view === 0) loadTracks();
    }, { root: E.trackScroll, rootMargin: "800px" });
    trkObs.observe(E.trackSentinel);
  }

  /* (album paging handled by the flow-render infinite-scroll observer above) */

  /* ============================================================
     SEARCH + SORT
     ============================================================ */
  let searchTimer = 0;

  /* ============================================================
     UNIFIED SEARCH — one FTS pass, three sections (Artists /
     Albums / Tracks) rendered in a dedicated results panel. The
     query is READ-ONLY on the C side: typing never touches the
     view filter or the album cache, so it stays instant at any
     library size. Applying a FILTER (Show-all / artist / album
     click) uses applyTrackFilter below — the old filtering path.
     ============================================================ */

  /* Filter the Tracks view to `q` (the pre-unified behavior; used by
     "Show all", artist/album clicks, and external callers that want a
     filtered browse list rather than the results panel). */
  function applyTrackFilter(q) {
    const query = String(q == null ? "" : q);
    if (E.search) E.search.value = query;
    if (typeof clearCategory === "function") clearCategory(true);
    setLibraryFilter(query);   /* backend filter + album-cache invalidation */
    state.view = 0;
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "0"));
    send({ cmd: "view", v: 0 });
    showPanel("view-tracks");
    resetTracks(); loadTracks();
    updateViewTitle();
    if (typeof window.__mnNavPush === "function") window.__mnNavPush();
  }

  /* Open the unified results panel for `q` (hoisted; used by metaLink,
     the suggestion dropdown and the search box). */
  let saGen = 0, saLastQ = "";
  function runSearch(q) {
    const query = String(q == null ? "" : q).trim();
    if (E.search && E.search.value.trim() !== query) E.search.value = query;
    if (!query) { closeSearch(); return; }
    saGen++; saLastQ = query;
    send({ cmd: "searchall", q: query, gen: saGen });
    if (!state.searchOpen) {
      showPanel("view-search");            /* leaves searchOpen alone for this id */
      state.searchOpen = true;
    }
    E.viewTitle.textContent = 'Results for "' + query + '"';
    /* the results panel is a real history entry (typing evolves the top
       entry in place — see navPush's replace-top rule) */
    if (typeof window.__mnNavPush === "function") window.__mnNavPush();
  }

  /* Sidebar navigation means "take me to the FULL view": any active search
     filter or open results panel is left behind. (Without this, a search's
     server-side filter stuck forever — a 2,000-album library stayed at the
     26 filtered albums until app restart.) */
  function navClearSearchContext() {
    clearTimeout(searchTimer); clearTimeout(sugTimer);
    if (typeof hideSug === "function") hideSug();
    state.searchOpen = false;
    if (E.search) E.search.value = "";
    setLibraryFilter("");
  }

  /* SINGLE CHOKE-POINT for the server-side library filter. Every change
     goes through here so the album grid cache is ALWAYS invalidated with
     it — call sites that sent cmd:"search" without setting albDirty left
     the view showing the stale filtered grid ("stuck at 26 albums"). */
  function setLibraryFilter(q) {
    q = String(q == null ? "" : q).trim();
    if (q === (state.query || "")) return;
    state.query = q;
    send({ cmd: "search", q: q });
    state.albDirty = true;
  }

  /* ---------- ENTITY NAVIGATION (context-aware search) ----------
     navGoAlbums(q): Albums view with the library filter applied — the
     "albums by this artist" context an artist click means.
     navGoAlbum(title, artist): THE album — Albums view, unfiltered,
     scrolled to the album and expanded. Resolution happens client-side
     against the eagerly-loaded album cache (search results carry no album
     id — they are deduped from track rows server-side). */
  function navGoAlbums(query) {
    const q = String(query == null ? "" : query).trim();
    state.searchOpen = false;
    if (typeof hideSug === "function") hideSug();
    if (E.search) E.search.value = q;
    setLibraryFilter(q);
    switchView(1);
  }
  let albNavPending = null;   /* { title, artist, tries } */
  function navGoAlbum(title, artist) {
    state.searchOpen = false;
    if (typeof hideSug === "function") hideSug();
    if (E.search) E.search.value = "";
    setLibraryFilter("");
    switchView(1);
    albNavPending = {
      title: String(title == null ? "" : title),
      artist: String(artist == null ? "" : artist),
      tries: 60,                      /* eager load fills ~200 albums/page */
    };
    albNavTick();
  }
  function albNavTick() {
    if (!albNavPending) return;
    const p = albNavPending;
    const cards = state.albCards || [];
    const tl = p.title.toLowerCase(), al = p.artist.toLowerCase();
    let idx = cards.findIndex((a) =>
      (a.title || "").toLowerCase() === tl &&
      (!al || (a.artist || "").toLowerCase() === al));
    if (idx < 0 && state.albDone)     /* artist mismatch (compilations) — title only */
      idx = cards.findIndex((a) => (a.title || "").toLowerCase() === tl);
    if (idx >= 0) {
      const a = cards[idx];
      /* COVER FLOW: an album/artist "tag" click must center that cover in the
         3D strip (and open it) — the grid-scroll paths below don't apply to a
         horizontal carousel, so a tag click used to silently no-op here. */
      if (CF.on && E.albumGrid.classList.contains("mode-coverflow")) {
        const cardEl = E.albumGrid.querySelector('.album-card[data-id="' + a.id + '"]');
        if (cardEl) {
          albNavPending = null;
          cfCenterCard(cardEl);
          if (String(state.expandedAlbum) !== String(a.id))
            cfToggleExpand(cardEl, a);
          return;
        }
        /* card not built yet — keep retrying until the eager load reaches it */
      } else if (VG.on && VG.cols > 0) {
        albNavPending = null;
        const row = Math.floor(idx / VG.cols);
        E.albumGrid.scrollTop = Math.max(0, row * VG.rowH - Math.floor(VG.rowH / 2));
        vgUpdate(true);
        const card = VG.pool.find((c) => c._idx === idx);
        if (card && a && String(state.expandedAlbum) !== String(a.id))
          toggleExpand(card, a);
        return;
      } else {
        /* details / carousel (legacy flow): scroll the album's card into view
           and pulse it — its track list is already inline in details mode */
        const cardEl = E.albumGrid.querySelector('.album-card[data-id="' + a.id + '"]');
        if (cardEl) {
          albNavPending = null;
          cardEl.scrollIntoView({ block: "center" });
          cardEl.classList.add("nav-flash");
          setTimeout(() => cardEl.classList.remove("nav-flash"), 1600);
          return;
        }
      }
    }
    if (--p.tries <= 0) { albNavPending = null; return; }
    setTimeout(albNavTick, 150);
  }

  /* Leave the results panel: restore the underlying view and drop any
     applied filter so the library is whole again. */
  function closeSearch() {
    const hadFilter = !!state.query;
    if (E.search) E.search.value = "";
    setLibraryFilter("");   /* clears backend + dirties the album cache */
    if (state.searchOpen || hadFilter) {
      state.searchOpen = false;
      switchView(typeof state.view === "number" ? state.view : 0);
    }
  }

  on("searchall", (m) => {
    if (m.gen != null && m.gen !== saGen) return;   /* stale reply */
    const secA = $("#search-sec-artists"), secB = $("#search-sec-albums"),
          secT = $("#search-sec-tracks"), empty = $("#search-empty"),
          sum = $("#search-summary");
    const hostA = $("#search-artists"), hostB = $("#search-albums"),
          hostT = $("#search-tracks");
    if (!hostT) return;
    const artists = m.artists || [], albums = m.albums || [], tracks = m.tracks || [];
    /* RANKING: strongest matches lead — exact name > prefix > substring,
       then by match count. The server ranks by count alone, which buried
       the album you literally typed under bigger fuzzy matches. */
    const ql = String(m.q || "").toLowerCase();
    const rank = (s) => {
      s = String(s || "").toLowerCase();
      return s === ql ? 0 : s.startsWith(ql) ? 1 : 2;
    };
    artists.sort((a, b) => rank(a.name) - rank(b.name) || b.matches - a.matches);
    albums.sort((a, b) => rank(a.title) - rank(b.title) || b.matches - a.matches);
    sum.textContent = tracks.length
      ? (m.total >= 400 ? "400+" : m.total) + " matching tracks · "
        + albums.length + " albums · " + artists.length + " artists"
      : "";
    empty.hidden = !!tracks.length;
    /* artists — chips; click opens the ALBUMS VIEW scoped to the artist
       (context preserved — not a fuzzy everything-with-these-letters list) */
    secA.hidden = !artists.length;
    hostA.innerHTML = "";
    /* cap the chip strip: an 'essentials'-style query matched 30 artists and
       the chip WALL pushed the album cards below the fold — the user read it
       as "search only shows tracks". 12 strongest + a "+N more" expander. */
    const A_CAP = 12;
    const renderArtists = (list) => {
      list.forEach((a) => {
        const c = el("button", "search-artist-chip");
        c.appendChild(el("span", "sac-name", esc(a.name)));
        c.appendChild(el("span", "sac-count", a.matches + " ♪"));
        c.title = "Show albums by " + a.name;
        c.addEventListener("click", () => navGoAlbums(a.name));
        hostA.appendChild(c);
      });
    };
    renderArtists(artists.slice(0, A_CAP));
    if (artists.length > A_CAP) {
      const more = el("button", "search-artist-chip sac-more",
                      "+" + (artists.length - A_CAP) + " more");
      more.addEventListener("click", () => {
        more.remove();
        renderArtists(artists.slice(A_CAP));
      });
      hostA.appendChild(more);
    }
    /* albums — mini cards with art; click opens THE album (expanded, in
       Albums view) — never a track list of similarly-named tracks */
    secB.hidden = !albums.length;
    hostB.innerHTML = "";
    albums.forEach((a) => {
      const card = el("div", "search-album-card");
      const art = el("div", "sab-art");
      setArt(art, a.art, "1.2em", artKeyOf(a.artist, a.title));
      card.appendChild(art);
      const tx = el("div", "sab-text");
      tx.appendChild(el("div", "sab-title", esc(a.title)));
      tx.appendChild(el("div", "sab-sub",
        (a.artist || "—") + (a.year ? " · " + a.year : "")));
      card.appendChild(tx);
      card.title = "Open album: " + a.title;
      card.addEventListener("click", () => navGoAlbum(a.title, a.artist));
      hostB.appendChild(card);
    });
    /* tracks — compact rows; double-click / play button plays */
    secT.hidden = !tracks.length;
    hostT.innerHTML = "";
    const frag = document.createDocumentFragment();
    tracks.forEach((t) => {
      const r = el("div", "search-track-row");
      r.dataset.id = t.id;
      const play = el("button", "str-play", "▶");
      play.addEventListener("click", (ev) => { ev.stopPropagation(); send({ cmd: "play", id: t.id }); });
      r.appendChild(play);
      const mid = el("div", "str-mid");
      mid.appendChild(el("div", "str-title", esc(t.title) || "Unknown"));
      mid.appendChild(el("div", "str-sub",
        (t.artist || "—") + (t.album ? "  ·  " + t.album : "")));
      r.appendChild(mid);
      r.appendChild(rowThumbs(t));
      r.appendChild(el("div", "str-dur", fmtTime(t.duration_ms)));
      r.addEventListener("dblclick", () => send({ cmd: "play", id: t.id }));
      frag.appendChild(r);
    });
    hostT.appendChild(frag);
  });

  (function initSearchView() {
    const all = $("#search-alltracks");
    if (all) all.addEventListener("click", () => applyTrackFilter(saLastQ));
  })();

  /* ============================================================
     LIVE SEARCH SUGGESTIONS — a dropdown under the search box that
     populates as you type, grouped into Tracks / Albums / Artists.
     Backed by the (now prefix-matching) FTS tracks query; albums and
     artists are derived from the matched rows. Enter / full search
     still opens the filtered Tracks view.
     ============================================================ */
  let sugPanel = null, sugTimer = 0, sugGen = 0, sugRows = null;
  function ensureSugPanel() {
    if (sugPanel) return sugPanel;
    sugPanel = el("div", "search-sug");
    sugPanel.hidden = true;
    const wrap = E.search.closest(".search-wrap") || E.search.parentNode;
    wrap.appendChild(sugPanel);
    /* dismiss on outside click / Esc */
    document.addEventListener("click", (e) => {
      if (sugPanel && !sugPanel.hidden && !wrap.contains(e.target)) hideSug();
    }, true);
    return sugPanel;
  }
  /* Enter/Escape live directly on the box (NOT inside ensureSugPanel — that
     is created lazily, so a fast Enter before the first suggestion request
     used to do nothing). */
  E.search.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { hideSug(); closeSearch(); }
    else if (e.key === "Enter") {
      hideSug();
      /* cancel pending debounces so runSearch doesn't fire twice */
      clearTimeout(searchTimer); clearTimeout(sugTimer);
      runSearch(E.search.value);
    }
  });
  function hideSug() { if (sugPanel) sugPanel.hidden = true; }
  function requestSug(q) {
    sugGen++;
    sugRows = { gen: sugGen, q };
    /* pull a generous window of matches; we group them client-side */
    send({ cmd: "searchsug", q, gen: sugGen });
  }
  function renderSug(rows, q) {
    const panel = ensureSugPanel();
    panel.innerHTML = "";
    const ql = q.toLowerCase();
    /* dedup albums + artists from the matched track rows */
    const albums = new Map(), artists = new Map();
    (rows || []).forEach((r) => {
      if (r.album) { const k = (r.artist || "") + "" + r.album;
        if (!albums.has(k)) albums.set(k, r); }
      if (r.artist && !artists.has(r.artist.toLowerCase())) artists.set(r.artist.toLowerCase(), r.artist);
    });
    let any = false;
    const group = (label) => { const g = el("div", "sug-group", label); panel.appendChild(g); };
    const item = (cls, primary, secondary, onClick) => {
      const it = el("div", "sug-item " + cls);
      const t = el("div", "sug-txt");
      t.appendChild(el("div", "sug-primary", primary));
      if (secondary) t.appendChild(el("div", "sug-secondary", secondary));
      it.appendChild(t);
      it.addEventListener("click", () => { hideSug(); onClick(); });
      panel.appendChild(it); any = true;
    };
    /* ENTITY-FIRST ordering + exact/prefix ranking: if what you typed IS
       an artist or album, that entity leads the dropdown; tracks follow. */
    const srank = (s) => {
      s = String(s || "").toLowerCase();
      return s === ql ? 0 : s.startsWith(ql) ? 1 : 2;
    };
    /* Artists */
    const rlist = Array.from(artists.values())
      .sort((a, b) => srank(a) - srank(b)).slice(0, 5);
    if (rlist.length) {
      group("Artists");
      rlist.forEach((name) => item("sug-artist", "◍  " + name, "Albums by " + name,
        () => navGoAlbums(name)));
    }
    /* Albums */
    const alist = Array.from(albums.values())
      .sort((a, b) => srank(a.album) - srank(b.album)).slice(0, 5);
    if (alist.length) {
      group("Albums");
      alist.forEach((r) => item("sug-album", "▣  " + r.album, r.artist || "",
        () => navGoAlbum(r.album, r.artist)));
    }
    /* Tracks */
    const tks = (rows || []).slice(0, 6);
    if (tks.length) {
      group("Tracks");
      tks.forEach((r) => item("sug-track", "♪  " + (r.title || "Unknown"),
        (r.artist || "") + (r.album ? "  ·  " + r.album : ""),
        () => { send({ cmd: "play", id: r.id }); }));
    }
    if (!any) { panel.appendChild(el("div", "sug-empty", "No matches for “" + q + "”")); }
    panel.hidden = false;
  }
  on("searchsug", (m) => {
    if (!sugRows || m.gen !== sugRows.gen) return;   /* stale reply */
    renderSug(m.rows || [], sugRows.q);
  });

  E.search.addEventListener("input", () => {
    clearTimeout(searchTimer);
    clearTimeout(sugTimer);
    hideSug();   /* WMP11/MediaMonkey style: no dropdown — the CONTENT AREA is the result */
    const q = E.search.value.trim();
    if (q.length < 1) { closeSearch(); return; }
    /* The results panel opens on the FIRST keystroke and live-updates as you
       type (grouped Artists / Albums / Tracks). The C side is an indexed FTS
       prefix lookup (<4 ms at 33k tracks), so the short debounce just
       coalesces fast typing. */
    searchTimer = setTimeout(() => runSearch(q), state.searchOpen ? 90 : 130);
  });
  /* Natural default direction per key: recency/popularity descend, text asc. */
  const SORT_DEFAULT_ASC = { added: false, created: false, plays: false, year: false, rating: false, duration: false };
  function defaultAsc(key) { return SORT_DEFAULT_ASC[key] !== false; }
  const sortDirBtn = $("#sort-dir");
  function updateSortDirUI() {
    if (sortDirBtn) {
      sortDirBtn.textContent = state.sortAsc ? "↑" : "↓";
      sortDirBtn.title = "Sort " + (state.sortAsc ? "ascending" : "descending") + " — click to flip";
    }
  }
  function applySort(refresh) {
    clearColSortInd();
    try { localStorage.setItem("mn.sort", state.sort); localStorage.setItem("mn.sortasc", state.sortAsc ? "1" : "0"); } catch (_) {}
    updateSortDirUI();
    send({ cmd: "sort", by: state.sort, asc: state.sortAsc });
    if (refresh !== false) {
      if (state.view === 1) { resetAlbums(); loadAlbums(); }
      else { resetTracks(); loadTracks(); }
    }
  }
  E.sort.addEventListener("change", () => {
    state.sort = E.sort.value;
    state.sortAsc = defaultAsc(state.sort);   /* each key gets its natural default */
    applySort();
  });
  if (sortDirBtn) sortDirBtn.addEventListener("click", () => {
    state.sortAsc = !state.sortAsc;           /* flip works for ANY sort key */
    applySort();
  });

  /* ---------- system-wide column-header sorting (click .th) ---------- */
  function clearColSortInd() {
    $$(".table-head .th.sortable").forEach((th) => { th.classList.remove("sort-asc", "sort-desc"); });
  }
  $$(".table-head .th.sortable").forEach((th) => th.addEventListener("click", () => {
    const by = th.dataset.sort;
    if (!by) return;
    /* re-clicking the active column flips; a new column uses its natural default */
    if (state.sort === by) state.sortAsc = !state.sortAsc;
    else { state.sort = by; state.sortAsc = defaultAsc(by); }
    if (E.sort && /^(title|artist|album|genre|year|duration|rating|plays|added|created)$/.test(by)) E.sort.value = by;
    applySort(false);
    th.classList.add(state.sortAsc ? "sort-asc" : "sort-desc");
    resetTracks(); loadTracks();
  }));
  /* restore persisted sort at boot — and ALWAYS push it to the backend so
     C-side and UI sort state can never disagree (a fresh localStorage used
     to skip the push and the C default leaked through: artist-ordered
     lists under a dropdown that said "Title"). */
  {
    const ss = localStorage.getItem("mn.sort");
    if (ss) { state.sort = ss; if (E.sort) E.sort.value = ss; }
    const sa = localStorage.getItem("mn.sortasc");
    if (sa != null) state.sortAsc = sa === "1";
    updateSortDirUI();
    send({ cmd: "sort", by: state.sort, asc: state.sortAsc });
  }

  /* ============================================================
     COLUMN SHOW/HIDE — right-click the track-table header.
     Persists to mn.hiddencols; rebuilds the shared grid template
     (--track-cols) so header + rows stay aligned.
     ============================================================ */
  const COLS = [
    { key: "artist", label: "Artist", w: "minmax(120px,1.5fr)" },
    { key: "album",  label: "Album",  w: "minmax(120px,1.6fr)" },
    { key: "year",   label: "Year",   w: "62px" },
    { key: "genre",  label: "Genre",  w: "minmax(90px,1fr)" },
    { key: "dur",    label: "Time",   w: "68px" },
    { key: "rating", label: "Rating", w: "152px" },
  ];
  let hiddenCols = [];
  try { hiddenCols = JSON.parse(localStorage.getItem("mn.hiddencols") || "[]"); } catch (_) {}
  if (!Array.isArray(hiddenCols)) hiddenCols = [];
  function applyCols() {
    const root = document.documentElement;
    const parts = ["52px", "minmax(180px,2.4fr)"];   /* # + Title always shown */
    COLS.forEach((c) => {
      const hid = hiddenCols.indexOf(c.key) >= 0;
      root.classList.toggle("hide-col-" + c.key, hid);
      if (!hid) parts.push(c.w);
    });
    root.style.setProperty("--track-cols", parts.join(" "));
    try { localStorage.setItem("mn.hiddencols", JSON.stringify(hiddenCols)); } catch (_) {}
  }
  applyCols();
  $$(".table-head").forEach((thead) => thead.addEventListener("contextmenu", (ev) => {
    ev.preventDefault();
    const menu = el("div", "pl-picker");
    const dismiss = () => {
      document.removeEventListener("click", onDoc, true);
      motion.close(menu, () => menu.remove());   /* graceful out */
    };
    menu.appendChild(el("div", "pl-picker-head", "Show columns"));
    COLS.forEach((c) => {
      const hid = hiddenCols.indexOf(c.key) >= 0;
      const it = el("div", "pl-picker-item", (hid ? "☐ " : "☑ ") + c.label);
      it.addEventListener("click", () => {
        const i = hiddenCols.indexOf(c.key);
        if (i >= 0) hiddenCols.splice(i, 1); else hiddenCols.push(c.key);
        applyCols();
        dismiss();
      });
      menu.appendChild(it);
    });
    document.body.appendChild(menu);
    menu.style.left = Math.min(ev.clientX, window.innerWidth - 240) + "px";
    menu.style.top = Math.min(ev.clientY, window.innerHeight - 300) + "px";
    const onDoc = (e) => { if (!menu.contains(e.target)) dismiss(); };
    setTimeout(() => document.addEventListener("click", onDoc, true), 0);
  }));

  /* ============================================================
     PLAYER / NOW
     ============================================================ */
  let smoothPos = 0, smoothDur = 0, lastNowTs = 0;

  /* lyrics presence per track id (drives the LYR pill): ask the backend
     once per track via lyricsread; lyrics.js owns the primary "lyrics"
     handler, so we observe replies through a tap. */
  const lyrHas = {};    /* id -> bool  */
  const lyrAsked = {};  /* id -> true  */
  tap("lyrics", (m) => {
    if (m && m.id != null) lyrHas[m.id] = !!(m.text && String(m.text).trim().length > 10);
  });

  /* DSP-in-path signal for the bit-perfect verdict (cached from the eq reply
     in openEq's handler; conservative default = unknown-but-off) */
  function eqIsActive() { return !!state.dspActive; }

  /* Poweramp-style HI-FI ANALYSIS: a full source→output chain breakdown with
     a definitive bit-perfect verdict and the reason when it isn't. */
  function showHifiPanel(m) {
    const kHz = (r) => (r / 1000).toFixed(r % 1000 ? 1 : 0) + " kHz";
    const oR = m.out_rate | 0, oB = m.out_bits | 0;
    const rateMatch = !m.sample_rate || oR === m.sample_rate;
    const depthOk = !m.bit_depth || !oB || oB >= m.bit_depth;
    const dsp = eqIsActive();
    const bp = !!m.out_excl && rateMatch && depthOk && !dsp && !m.downmixed;
    const ov = el("div", "hifi-overlay");
    const card = el("div", "hifi-card");
    card.appendChild(el("div", "hifi-verdict " + (bp ? "bp" : "conv"),
      bp ? "✓ BIT-PERFECT OUTPUT" : "~ AUDIO IS BEING CONVERTED"));
    const chain = el("div", "hifi-chain");
    const stage = (title, lines, cls) => {
      const s = el("div", "hifi-stage" + (cls ? " " + cls : ""));
      s.appendChild(el("div", "hifi-stage-t", title));
      lines.forEach((l) => s.appendChild(el("div", "hifi-stage-l", l)));
      return s;
    };
    chain.appendChild(stage("SOURCE FILE", [
      (m.format || "—"),
      (m.sample_rate ? kHz(m.sample_rate) : "?") + (m.bit_depth ? " · " + m.bit_depth + "-bit" : ""),
      (m.bitrate_kbps ? m.bitrate_kbps + " kbps" : "lossless"),
    ]));
    chain.appendChild(el("div", "hifi-arrow", dsp ? "→ DSP →" : "→"));
    chain.appendChild(stage("DECODER", [
      "32-bit float pipeline",
      dsp ? "EQ / DSP active" : "no DSP (transparent)",
    ], dsp ? "warn" : "ok"));
    chain.appendChild(el("div", "hifi-arrow", "→"));
    chain.appendChild(stage("AUDIO DEVICE", [
      (m.out_pcm || "?"),
      (oR ? kHz(oR) : "?") + (oB ? " · " + oB + "-bit" : ""),
      m.out_excl ? "WASAPI Exclusive" : "WASAPI Shared",
    ], m.out_excl ? "ok" : "warn"));
    card.appendChild(chain);
    /* reasons list */
    const why = el("ul", "hifi-why");
    const line = (ok, txt) => { const li = el("li", ok ? "ok" : "no");
      li.textContent = (ok ? "✓ " : "✗ ") + txt; why.appendChild(li); };
    line(!!m.out_excl, "Exclusive device access (the app owns the DAC)");
    line(rateMatch, rateMatch ? "Sample rate unchanged (" + (oR ? kHz(oR) : "?") + ")"
      : "Sample rate converted " + kHz(m.sample_rate) + " → " + kHz(oR));
    line(depthOk, depthOk ? "Bit depth preserved or expanded"
      : "Bit depth reduced " + m.bit_depth + " → " + oB);
    /* channel integrity — surround content delivered intact vs folded */
    const srcCh = m.channels | 0, pipeCh = m.pipe_ch | 0;
    if (srcCh > 2 || pipeCh > 2) {
      line(!m.downmixed, m.downmixed
        ? "Downmixed " + srcCh + "ch → stereo"
        : "All " + srcCh + " channels delivered intact");
    }
    line(!dsp, dsp ? "EQ/DSP is modifying the signal" : "No EQ/DSP in the path");
    card.appendChild(why);
    if (!bp) {
      const tip = el("div", "hifi-tip");
      if (!m.out_excl) tip.textContent = "Enable Exclusive output in Settings → Audio for bit-perfect playback.";
      else if (dsp) tip.textContent = "Turn off the equalizer for bit-perfect playback.";
      else tip.textContent = "Your device can't run this source's exact format; the app picks the closest it supports.";
      card.appendChild(tip);
    }
    card.appendChild(btn2("Close", () => ov.remove()));
    ov.appendChild(card);
    ov.addEventListener("click", (e) => { if (e.target === ov) ov.remove(); });
    document.body.appendChild(ov);
  }
  function btn2(label, fn) { const b = el("button", "hifi-close", label); b.addEventListener("click", fn); return b; }

  on("now", (m) => {
    const prevPlaying = state.now ? !!state.now.playing : false;
    state.now = m;
    /* IDLE-CPU re-arm: pollInterval() stretches to 1.5 s while paused. When
       `playing` flips (either direction — resume from any source, or pause),
       re-arm the poll timer immediately so a resume snaps back to fast cadence
       instead of waiting out the slow interval, and a fresh pause takes the
       slow one right away. Cheap: only fires on the transition, not per poll. */
    if (!!m.playing !== prevPlaying && window.__mnRearmPoll) window.__mnRearmPoll();
    /* body.playing gates the continuous motion effects (glow/pulse/gradient
       drift) so they run ONLY while audio is actually playing — zero idle */
    if (!!m.playing !== prevPlaying) motion.playing(!!m.playing);
    lastNowTs = performance.now();
    smoothPos = m.position_ms || 0;
    smoothDur = m.duration_ms || 0;

    /* Resume-on-launch: remember the last track + position so a restart can
       restore it. Persist at most ~1/s and only with a real track loaded. */
    if (m.track_id != null && m.track_id >= 0 && m.duration_ms) {
      const nowT = performance.now();
      if (nowT - (state._resumeSaveTs || 0) > 1000) {
        state._resumeSaveTs = nowT;
        try { localStorage.setItem("mn.resume", JSON.stringify({ id: m.track_id, pos: Math.floor(m.position_ms || 0) })); } catch (_) {}
      }
    }

    /* always-cheap per-poll work */
    updateStemMeters(m.stem_meters || []);
    updateDepthPill(m);
    wakeLoop();

    /* Fetch a real waveform whenever the playing track changes. Retry a few
       times if bars don't arrive — but CAP it: a file whose length can't be
       decoded returns 0 bars forever, and the old unbounded 3s retry
       re-decoded the WHOLE file every 3 seconds for the life of the track. */
    const tkey = (m.track_title || "") + "|" + (m.track_artist || "");
    if (m.track_title && tkey !== state._waveKey) {
      state._waveKey = tkey;
      waveBars = null; waveMax = 1; waveForId = m.track_id || -1;
      lastWaveDrawKey = "";
      resetStemDetection();
      abClear();   /* A-B loop is per-track */
      send({ cmd: "waveform", id: m.track_id || 0 });
      state._waveRetryAt = performance.now() + 3000;
      state._waveRetries = 0;
    } else if (m.playing && !waveBars && (state._waveRetries || 0) < 3
               && performance.now() > (state._waveRetryAt || 0)) {
      send({ cmd: "waveform", id: m.track_id || 0 });
      state._waveRetryAt = performance.now() + 3000;
      state._waveRetries = (state._waveRetries || 0) + 1;
    }

    /* VOLATILE fields first — stem rt-factor jitters on nearly every poll
       while stems process, and volume churns during wheel/drag. They are
       1-2 element writes each, so they run unconditionally and are KEPT OUT
       of nowKey below (they used to bust the gate 4×/s and re-run the whole
       static rebuild). */
    if (!state.volDrag) setVolUI(m.volume != null ? m.volume : 1);
    updateStemStatus(m);
    {
      const sm = $("#set-stemmargin");
      if (sm) {
        const smT = m.stem_rt_factor != null ? m.stem_rt_factor.toFixed(2) + "× realtime" : "—";
        if (sm.textContent !== smT) sm.textContent = smT;
      }
    }

    /* Everything below rewrites static DOM (titles, pills, buttons). The
       poll arrives at 4 Hz — skip the whole block unless something that
       feeds it actually changed, so idle polls cost ~zero style/layout.
       (stem_fraction keyed at 5% steps: the STEMS pill still tracks
       progress without a rebuild per poll.) */
    const nowKey = [
      m.track_title, m.track_artist, m.track_album, m.art, m.track_id,
      m.playing, m.shuffle, m.repeat, m.liked,
      m.format, m.sample_rate, m.bit_depth, m.channels, m.bitrate_kbps,
      m.out_rate, m.out_bits, m.out_ch, m.out_pcm, m.out_excl, m.duration_ms,
      m.stems_enabled, m.stems_passthrough, m.neural_active,
      m.stem_provider, Math.round((m.stem_fraction || 0) * 20),
      m.play_count, (m.track_id != null && lyrHas[m.track_id]) ? 1 : 0,
    ].join("");
    if (nowKey === state._nowKey) return;
    state._nowKey = nowKey;

    E.plTitle.textContent = esc(m.track_title) || "Nothing playing";
    E.plArtist.textContent = esc(m.track_artist) || "—";
    E.npTitle.textContent = esc(m.track_title) || "Nothing playing";
    /* now-playing artist + album are clickable → search that artist/album */
    E.npArtist.innerHTML = "";
    E.npArtist.appendChild(metaLinkSpan(m.track_artist || "—", m.track_artist, "artist"));
    E.npAlbum.innerHTML = "";
    E.npAlbum.appendChild(metaLinkSpan(m.track_album || "—", m.track_album, "album", m.track_artist));
    /* artKey (from the C-side track_album_artist — the exact aa the art key
       hashed): while PAUSED the memoized now-payload suppresses re-emits, so
       a thumb that was still extracting at pause time can ONLY reach these
       tiles via a targeted artready repaint. */
    {
      const nk = m.track_album ?
        artKeyOf(m.track_album_artist || m.track_artist, m.track_album) : "";
      setArt(E.plArt, m.art, null, nk);
      setArt(E.npArt, m.art, null, nk);
    }
    if (m.art !== state._npArtUrl) {
      state._npArtUrl = m.art;
      updateVolumetricArt(m.art);
    }

    E.plFormat.innerHTML = "";
    const kHz = (r) => (r / 1000).toFixed(r % 1000 ? 1 : 0) + " kHz";
    /* small helper: pill with a hover tooltip explaining what it means */
    const fpill = (cls, txt, tip) => {
      const p = el("span", cls, txt);
      if (tip) p.title = tip;
      E.plFormat.appendChild(p);
    };
    /* adaptive coloring: lossless = cyan, lossy = orange, hi-res = duo
       gradient, surround = cyan block. `keep` = survives pill-fit longest. */
    const lossless = isLossless(m.format);
    const hires = isHires(m.bit_depth, m.sample_rate);
    if (m.format) fpill(
      "pill fmt keep " + (hires ? "hires" : lossless ? "lossless" : "lossy"),
      String(m.format).toUpperCase() + (hires ? " HI-RES" : ""),
      hires ? "Audio format — HI-RES: better than CD quality (24-bit or ≥88.2 kHz)"
            : lossless ? "Audio format — lossless: bit-perfect copy of the original recording"
                       : "Audio format — lossy: compressed audio (some data discarded to save space)");
    if (m.sample_rate) fpill("pill keep" + (m.sample_rate >= 88200 ? " hires" : ""), kHz(m.sample_rate),
      "Sample rate — samples per second (CD = 44.1 kHz; ≥88.2 kHz = hi-res)");
    if (m.bit_depth) fpill("pill" + (m.bit_depth >= 24 ? " hires" : ""), m.bit_depth + " bit",
      "Bit depth — precision per sample (CD = 16-bit; 24-bit = hi-res, more dynamic range)");
    if (m.channels) fpill("pill" + (m.channels > 2 ? " surround" : ""),
      m.channels === 2 ? "Stereo" : m.channels === 1 ? "Mono" : m.channels + "ch",
      m.channels > 2 ? "Multichannel/surround source (" + m.channels + " channels)"
                     : "Channel layout of the source file");
    if (m.bitrate_kbps) fpill(
      "pill br-" + (m.bitrate_kbps >= 900 ? "high" : m.bitrate_kbps >= 256 ? "mid" : "low"),
      m.bitrate_kbps + " kbps",
      "Bitrate — audio data per second (green ≥900 = lossless-class, yellow ≥256 = good lossy, red = low)");
    /* SOURCE -> OUTPUT chain: show what the hardware is actually running at
       when it differs from the source (rate or bit depth). */
    const oR = m.out_rate | 0, oB = m.out_bits | 0, oC = m.out_ch | 0;
    if (oR && m.sample_rate && (oR !== m.sample_rate || (m.bit_depth && oB && oB !== m.bit_depth))) {
      fpill("pill arrow", "→", "The source is being converted: source format → what your audio device actually outputs");
      fpill("pill out", kHz(oR), "Output sample rate — what the hardware/DAC is really running at");
      if (oB) fpill("pill out", oB + " bit", "Output bit depth — what the hardware/DAC is really running at");
      if (oC) fpill("pill out", oC + "ch", "Output channel count");
    }
    /* ALWAYS-visible exact output metrics, right side of the playback bar:
       device sample format, bit depth, rate, channels, share mode. */
    {
      const plOut = $("#pl-out");
      if (plOut) {
        plOut.innerHTML = "";
        if (oR) {
          const opill = (txt, tip) => {
            const p = el("span", "pill out", txt);
            p.title = tip;
            plOut.appendChild(p);
          };
          if (m.out_pcm) opill(m.out_pcm,
            "Device sample format — float = the shared-mode mix pipeline; PCM 16/24/32 = integer output (exclusive/bit-perfect)");
          if (oB) opill(oB + " bit", "Bit depth the audio device is actually running at");
          opill(kHz(oR), "Sample rate the audio device is actually running at");
          if (oC) opill(oC === 2 ? "Stereo" : oC === 1 ? "Mono" : oC + "ch", "Output channel layout");
          opill(m.out_excl ? "Exclusive" : "Shared",
            m.out_excl ? "WASAPI exclusive — the app owns the device; bit-perfect output"
                       : "WASAPI shared — Windows mixes all apps at the system mixer format");
          /* Poweramp-style BIT-PERFECT VERDICT: green when the hardware runs
             at the source's exact rate+depth in exclusive mode with no DSP;
             amber when converted; click for the full chain breakdown. */
          const rateMatch = !m.sample_rate || oR === m.sample_rate;
          const depthMatch = !m.bit_depth || !oB || oB >= m.bit_depth;
          const noDsp = !eqIsActive();
          const bitPerfect = !!m.out_excl && rateMatch && depthMatch && noDsp && !m.downmixed;
          const verdict = el("span", "pill hifi-verdict " + (bitPerfect ? "bp" : "conv"),
            bitPerfect ? "✓ BIT-PERFECT" : "~ CONVERTED");
          verdict.title = "Click for the full output-chain analysis";
          verdict.style.cursor = "pointer";
          verdict.addEventListener("click", () => showHifiPanel(m));
          plOut.appendChild(verdict);
        }
      }
    }
    /* Android-style status pills: lyrics / neural stems / play count */
    const tid = m.track_id;
    if (m.track_title && tid != null && tid >= 0 && !lyrAsked[tid]) {
      lyrAsked[tid] = true;
      send({ cmd: "lyricsread", id: tid });
    }
    if (tid != null && lyrHas[tid]) {
      const lp = el("span", "pill lyr clickable", "LYR");
      lp.title = "Lyrics available — click for live synced lyrics";
      lp.addEventListener("click", () => {
        if (window.MnLyrics && MnLyrics.toggleNp) MnLyrics.toggleNp();
      });
      E.plFormat.appendChild(lp);
    }
    if (m.stems_enabled && !m.stems_passthrough && m.stem_fraction != null && m.stem_fraction > 0) {
      fpill("pill stems",
        m.stem_fraction >= 1 ? "STEMS" : "STEMS " + Math.round(m.stem_fraction * 100) + "%",
        m.stem_fraction >= 1
          ? "AI stem separation complete — the mixer's 9 channels control this track live"
          : "AI stem separation in progress — % of the track separated so far");
    }
    const nrow = (tid != null) ? state.trkRows.find((r) => r.id === tid) : null;
    const plays = (nrow && (nrow.play_count || nrow.plays)) || m.play_count || 0;
    if (plays > 0) fpill("pill plays", plays + (plays === 1 ? " play" : " plays"),
      "How many times you've played this track");
    armPillFit();   /* whole pills only — hide (never clip) what won't fit */

    E.btnPlay.classList.toggle("playing", !!m.playing);
    /* root marker for CSS that must PARK while paused (the queue's dancing
       eq-bars). Same-value toggle is a no-op, so frequent now msgs are free. */
    document.documentElement.classList.toggle("mn-playing", !!m.playing);
    { /* morph the glyph only when it actually flips (now msgs are frequent) */
      const pg = m.playing ? "⏸" : "▶";
      if (E.playGlyph.textContent !== pg) {
        E.playGlyph.textContent = pg;
        motion.pop(E.playGlyph);
        /* playMorph channel: rotate-in the new glyph (CSS-gated) */
        E.playGlyph.classList.remove("mo-flip");
        void E.playGlyph.offsetWidth;
        E.playGlyph.classList.add("mo-flip");
      }
    }
    E.btnShuffle.classList.toggle("active", !!m.shuffle);
    /* repeat: 0=off 1=all 2=one. Show a "1" badge for repeat-one so it's
       distinguishable from repeat-all (they used to look identical). */
    E.btnRepeat.classList.toggle("active", !!m.repeat);
    E.btnRepeat.classList.toggle("repeat-one", m.repeat === 2);
    E.btnRepeat.title = m.repeat === 2 ? "Repeat one" : m.repeat === 1 ? "Repeat all" : "Repeat off";
    updateThumbsUI(m.liked || 0);

    E.tTotal.textContent = fmtTime(smoothDur);

    E.stemEnable.checked = !!m.stems_enabled;
    E.stemPass.checked = !!m.stems_passthrough;
    E.btnStems.classList.toggle("active", !!m.stems_enabled);
    updateDetailsRow(m);

    if (state.view === 0) refreshPlayingRow();
  });

  /* Cheap: rows carry data-id, `now` carries track_id — one attribute query,
     and a memo skips even that when nothing changed (this runs on every
     250ms poll reply, so it must be near-free at any row count). */
  let playingRowMark = null;   /* id currently marked, or null */
  /* Keep the expanded-album track list's now-playing highlight in sync. Cheap
     (the list is one screen of rows) and NOT gated by the main-list memo, so
     re-expanding the playing album re-marks its row even when the track id
     hasn't changed. */
  function refreshExpandPlaying(want) {
    const list = state._expandList;
    if (!list) return;
    const prev = list.querySelector(".aex-track.playing");
    if (prev && prev.dataset.id !== want) prev.classList.remove("playing");
    if (!want) return;
    const row = list.querySelector('.aex-track[data-id="' + want + '"]');
    if (row) row.classList.add("playing");
  }
  function refreshPlayingRow() {
    const want = (state.now && state.now.playing && state.now.track_id != null)
      ? String(state.now.track_id) : null;
    /* expanded album list is always reconciled (see refreshExpandPlaying) */
    refreshExpandPlaying(want);
    if (want === playingRowMark) {
      /* same target — only re-verify that the marked row still exists
         (a reset clears the DOM without telling us) */
      if (!want || E.trackRows.querySelector(".track-row.playing")) return;
    }
    const prev = E.trackRows.querySelector(".track-row.playing");
    if (prev) prev.classList.remove("playing");
    playingRowMark = want;
    if (!want) return;
    const row = E.trackRows.querySelector('.track-row[data-id="' + want + '"]');
    if (row) row.classList.add("playing");
  }

  /* Jump to the currently-playing track: switch to Tracks, locate the row,
     scroll it into view and flash it. If it isn't in the loaded window yet,
     the list is at least focused (deep paging can bring it in on scroll). */
  function jumpToCurrent() {
    if (!state.now || state.now.track_id == null) return;
    const id = String(state.now.track_id);
    if (state.view !== 0) switchView(0);
    const find = () => {
      const row = E.trackRows.querySelector('.track-row[data-id="' + id + '"]');
      if (row) {
        row.scrollIntoView({ block: "center", behavior: "smooth" });
        row.classList.remove("flash"); void row.offsetWidth; row.classList.add("flash");
        setTimeout(() => row.classList.remove("flash"), 1400);
      }
    };
    /* give a fresh switchView its first page a moment to render */
    setTimeout(find, state.view === 0 ? 0 : 260);
  }
  if (E.plTitle) {
    E.plTitle.style.cursor = "pointer";
    E.plTitle.title = "Jump to this track in the library";
    E.plTitle.addEventListener("click", jumpToCurrent);
  }

  /* ---------- THE app rAF loop ------------------------------------------
     ONE loop drives seek interpolation, the waveform playhead, stem meters
     and the CSS art tilt (the volumetric mesh runs on its own WORKER — see
     depthart.js). It performs ZERO layout reads (geometry is cached via
     ResizeObserver) and only compositor-friendly / skip-if-unchanged writes.
     It PARKS itself when nothing is playing, animating or being dragged;
     poll() and input events wake it back up.                             */
  let rafActive = false, lastFrac = 0, lastElapsedTxt = "", lastKnobPx = -1;
  let seekW = 0;   /* .seek-track CSS width (knob travel), cached */

  function setKnob(frac, dragging) {
    const px = seekW ? Math.round(frac * seekW * 2) / 2 : 0;  /* 0.5px steps */
    if (px === lastKnobPx && !dragging) return;
    lastKnobPx = px;
    /* transform, not `left` — compositor-only, no per-frame layout */
    E.seekKnob.style.transform = "translate(" + px + "px,-50%) translateX(-50%)" +
                                 (dragging ? " scale(1.25)" : "");
  }

  function metersAnimating() {
    /* peakDisp: 0.3/s dt-scaled decay — bounded, parks within ~3s of silence */
    for (const f of faderEls) {
      if (f.meterVal > 0.4 || f.meterDisp > 0.4 || f.peakDisp > 0.01) return true;
    }
    return false;
  }
  function loopNeeded() {
    /* minimized window (C pushes {type:"vis"}): nothing is watchable — park
       the whole loop even mid-playback; restore re-wakes via wakeLoop() */
    if (window.__mnWinHidden) return false;
    const n = state.now;
    /* keep the loop awake while stems are actively separating so the blue
       loading banner behind the waveform advances even if playback is paused */
    const separating = !!(n && n.stems_enabled && !n.stems_passthrough &&
                          n.stem_fraction != null && n.stem_fraction > 0 &&
                          n.stem_fraction < 0.999);
    return !!(n && n.playing) || state.seekDrag || separating ||
           cssArtAnimating() || metersAnimating();
  }
  function wakeLoop() {
    if (!rafActive) { rafActive = true; requestAnimationFrame(tick); }
  }

  function tick(t) {
    if (!loopNeeded()) {
      rafActive = false;      /* park — poll() / pointer re-arms */
      tickArt(t || performance.now());   /* one final frame resets art tilt */
      return;
    }
    if (!state.seekDrag && smoothDur > 0) {
      let pos = smoothPos;
      if (state.now && state.now.playing) {
        pos = clamp(smoothPos + (performance.now() - lastNowTs), 0, smoothDur);
      }
      const frac = pos / smoothDur;
      lastFrac = frac;
      setKnob(frac, false);
      const txt = PREFS.remaining
        ? "-" + fmtTime(Math.max(0, smoothDur - pos))
        : fmtTime(pos);
      if (txt !== lastElapsedTxt) { lastElapsedTxt = txt; E.tElapsed.textContent = txt; }
      drawWave(frac);
      /* A-B repeat: loop back to A when playback passes B */
      if (ab.a != null && ab.b != null && state.now && state.now.playing && pos >= ab.b) {
        smoothPos = ab.a; lastNowTs = performance.now();
        send({ cmd: "seek", ms: Math.round(ab.a) });
      }
    }
    smoothMeters(t || performance.now());
    tickArt(t || performance.now());
    requestAnimationFrame(tick);
  }

  /* ---------- waveform seekbar ---------- */
  let waveBars = null;          /* Float32-ish array of 0..1 peaks       */
  let waveMax = 1;              /* loudest bar — adaptive normalization  */
  let waveForId = -1;           /* track id the current bars belong to   */
  const waveCanvas = $("#wave-canvas");
  const waveCtx = waveCanvas ? waveCanvas.getContext("2d") : null;

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }
  /* --- waveform is rendered CHEAP per frame ---------------------------------
     The bar shape is static, so it's painted ONCE into two offscreen layers
     (dim = unplayed, accent = played) whenever the bars/size/theme change.
     Each animation frame only composites: blit dim, then blit the accent layer
     clipped to the played width — no per-bar work, no getComputedStyle().     */
  let waveDim = null, waveAcc = null, waveKey = "", waveW = 0, waveH = 0, waveDpr = 1;
  let waveCW = 0, waveCH = 0, lastWaveDrawKey = "";
  let accentCache = "#FB8C00", accent2Cache = "#00E5FF";
  /* Fixed blue for the stem-loading progress banner behind the waveform —
     theme-independent so the "separating" state always reads as blue. */
  const STEM_LOAD_BLUE = "#3B82F6", STEM_LOAD_BLUE_HOT = "#7DB0FF";
  function refreshAccentCache() {
    accentCache = cssVar("--accent") || "#FB8C00";
    accent2Cache = cssVar("--accent-2") || "#00E5FF";
    waveKey = "";   /* force layer rebuild with new colors */
    lastWaveDrawKey = "";
  }
  refreshAccentCache();

  function buildWaveLayers(w, h, dpr) {
    const bars = waveBars && waveBars.length ? waveBars : null;
    const N = bars ? bars.length : Math.max(48, Math.floor(w / 3));
    const gap = 1, bw = (w - (N - 1) * gap) / N, mid = h / 2;
    const mk = (color) => {
      const c = document.createElement("canvas");
      c.width = Math.round(w * dpr); c.height = Math.round(h * dpr);
      const x = c.getContext("2d"); x.setTransform(dpr, 0, 0, dpr, 0, 0);
      x.fillStyle = color;
      for (let i = 0; i < N; i++) {
        const p = bars ? clamp(bars[i] / (waveMax || 1), 0, 1) : 0.08;
        const bh = Math.max(2, p * (h - 2));
        x.fillRect(i * (bw + gap), mid - bh / 2, bw, bh);
      }
      return c;
    };
    waveDim = mk("rgba(255,255,255,.14)");
    waveAcc = mk(accentCache);
  }

  /* canvas CSS size cached via ResizeObserver — NO clientWidth/Height reads
     in the frame loop (those force layout) */
  function drawWave(frac) {
    if (!waveCtx || !waveCanvas) return;
    const dpr = window.devicePixelRatio || 1;
    const w = waveCW, h = waveCH;
    if (!w || !h) return;
    /* rebuild the static layers only when bars/size/dpr change */
    const key = (waveBars ? waveBars.length + ":" + waveMax : "flat") + ":" + w + "x" + h + ":" + dpr;
    const px = Math.round(frac * w * dpr);
    const n = state.now;
    /* Stem-loading progress banner: a BLUE fill behind the waveform showing how
     * much of the track has been neural-separated so far. Intentionally NOT gated
     * on waveBars — it must be visible during the load even before the real
     * waveform decodes (that is exactly when separation is running), so it draws
     * over the flat baseline too. Blue is fixed (not the theme accent) so it
     * always reads as the stem-buffer channel regardless of accent theme. */
    const stemPx = (n && n.stems_enabled && !n.stems_passthrough && n.stem_fraction > 0)
      ? Math.round(clamp(n.stem_fraction, 0, 1) * w * dpr) : 0;
    /* A-B loop markers: pixel columns for the set points (either may be null) */
    const dur = smoothDur || 0;
    const aPx = (ab.a != null && dur > 0) ? Math.round((ab.a / dur) * w * dpr) : -1;
    const bPx = (ab.b != null && dur > 0) ? Math.round((ab.b / dur) * w * dpr) : -1;
    /* frame-skip: nothing visible changed → zero canvas work this frame */
    const dk = key + "|" + px + "|" + stemPx + "|" + aPx + "|" + bPx;
    if (dk === lastWaveDrawKey) return;
    lastWaveDrawKey = dk;

    if (waveCanvas.width !== Math.round(w * dpr) || waveCanvas.height !== Math.round(h * dpr)) {
      waveCanvas.width = Math.round(w * dpr); waveCanvas.height = Math.round(h * dpr);
    }
    if (key !== waveKey) { buildWaveLayers(w, h, dpr); waveKey = key; waveW = w; waveH = h; waveDpr = dpr; }

    const ctx = waveCtx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, waveCanvas.width, waveCanvas.height);
    /* dim base */
    ctx.drawImage(waveDim, 0, 0);
    /* played portion: clip to fraction, blit accent layer */
    if (px > 0) {
      ctx.save();
      ctx.beginPath(); ctx.rect(0, 0, px, waveCanvas.height); ctx.clip();
      ctx.drawImage(waveAcc, 0, 0);
      ctx.restore();
    }
    /* stem-loading BLUE banner (only when active; cheap two rects). A solid-ish
     * blue band across the separated fraction with a brighter leading edge so it
     * reads as a progress banner behind the wave. Fixed blue = STEM_LOAD_BLUE. */
    if (stemPx > 0) {
      ctx.fillStyle = STEM_LOAD_BLUE;
      ctx.globalAlpha = 0.34;                 /* banner body — clearly visible */
      ctx.fillRect(0, 0, stemPx, waveCanvas.height);
      /* brighter leading edge marks the separation playhead */
      const edge = Math.max(2, Math.round(3 * dpr));
      ctx.globalAlpha = 0.85;
      ctx.fillStyle = STEM_LOAD_BLUE_HOT;
      ctx.fillRect(stemPx - edge, 0, edge, waveCanvas.height);
      ctx.globalAlpha = 1;
    }
    /* A-B loop markers: two bright vertical ticks + a faint band between */
    if (aPx >= 0 || bPx >= 0) {
      if (aPx >= 0 && bPx >= 0) {
        ctx.globalAlpha = 0.12;
        ctx.fillStyle = accentCache;
        ctx.fillRect(aPx, 0, bPx - aPx, waveCanvas.height);
        ctx.globalAlpha = 1;
      }
      ctx.fillStyle = accentCache;
      const tw = Math.max(2, Math.round(2 * dpr));
      if (aPx >= 0) ctx.fillRect(aPx, 0, tw, waveCanvas.height);
      if (bPx >= 0) ctx.fillRect(bPx - tw, 0, tw, waveCanvas.height);
    }
  }

  /* keep the cached geometry fresh; repaint once per resize */
  new ResizeObserver((ents) => {
    for (const en of ents) { waveCW = en.contentRect.width; waveCH = en.contentRect.height; }
    seekW = waveCW;                 /* the canvas fills .seek-track */
    lastWaveDrawKey = ""; lastKnobPx = -1;
    drawWave(lastFrac);
    setKnob(lastFrac, false);
  }).observe(waveCanvas);

  on("waveform", (m) => {
    if (!m || !Array.isArray(m.bars)) return;
    waveBars = m.bars;
    waveForId = m.id;
    waveMax = 0;
    for (let i = 0; i < waveBars.length; i++) if (waveBars[i] > waveMax) waveMax = waveBars[i];
    if (!(waveMax > 0)) waveMax = 1;
    drawWave(lastFrac);   /* repaint even if the loop is parked (paused) */
    wakeLoop();
  });
  function requestWaveform(id) {
    if (id && id !== waveForId) { waveBars = null; waveForId = id; send({ cmd: "waveform", id }); }
  }

  E.btnPlay.addEventListener("click", () => send({ cmd: "toggle" }));
  E.btnPrev.addEventListener("click", () => send({ cmd: "prev" }));
  E.btnNext.addEventListener("click", () => send({ cmd: "next" }));
  E.btnShuffle.addEventListener("click", () => {
    const on = !(state.now && state.now.shuffle);
    send({ cmd: "shuffle", on });
    E.btnShuffle.classList.toggle("active", on);
  });
  E.btnRepeat.addEventListener("click", () => send({ cmd: "repeat" }));

  /* ---------- Equalizer / DSP overlay ---------- */
  (function initEq() {
    const overlay = $("#eq-overlay");
    const btnEq = $("#btn-eq");
    if (!overlay || !btnEq) return;
    const bandsHost = $("#eq-bands");
    const selPreset = $("#eq-preset");
    const enDsp = $("#eq-enable"), enEq = $("#eq-eqon"), enLim = $("#eq-limiter");
    const preamp = $("#eq-preamp"), preampVal = $("#eq-preamp-val");
    const bal = $("#eq-balance"), balVal = $("#eq-balance-val");
    let built = false, sliders = [];

    function fmtHz(hz) { return hz >= 1000 ? (hz / 1000).toFixed(hz % 1000 ? 1 : 0) + "k" : String(Math.round(hz)); }

    function buildBands(freqs) {
      bandsHost.innerHTML = ""; sliders = [];
      freqs.forEach((hz, i) => {
        const col = el("div", "eq-band");
        const val = el("div", "eq-band-val", "0");
        const sl = document.createElement("input");
        sl.type = "range"; sl.min = "-12"; sl.max = "12"; sl.step = "0.5"; sl.value = "0";
        sl.className = "eq-slider";
        sl.addEventListener("input", () => {
          const g = parseFloat(sl.value);
          val.textContent = (g > 0 ? "+" : "") + g;
          send({ cmd: "eq", action: "band", band: i, gain: g });
        });
        const lbl = el("div", "eq-band-hz", fmtHz(hz));
        col.appendChild(val); col.appendChild(sl); col.appendChild(lbl);
        bandsHost.appendChild(col);
        sliders.push({ sl, val });
      });
    }

    on("eq", (m) => {
      if (Array.isArray(m.freqs) && (!built || sliders.length !== m.freqs.length)) {
        buildBands(m.freqs); built = true;
        if (selPreset && !selPreset.children.length && Array.isArray(m.presets)) {
          m.presets.forEach((name, i) => {
            const o = document.createElement("option"); o.value = i; o.textContent = name;
            selPreset.appendChild(o);
          });
        }
      }
      if (Array.isArray(m.bands)) m.bands.forEach((g, i) => {
        if (sliders[i]) { sliders[i].sl.value = g; sliders[i].val.textContent = (g > 0 ? "+" : "") + (Math.round(g * 10) / 10); }
      });
      if (enDsp) enDsp.checked = !!m.enabled;
      if (enEq) enEq.checked = !!m.eq_enabled;
      /* cache for the bit-perfect verdict (DSP in the path = not bit-perfect) */
      state.dspActive = !!(m.enabled && (m.eq_enabled ||
        (Array.isArray(m.bands) && m.bands.some((g) => Math.abs(g) > 0.05)) ||
        (m.preamp && Math.abs(m.preamp) > 0.05)));
      if (preamp && m.preamp != null) { preamp.value = m.preamp; preampVal.textContent = (m.preamp > 0 ? "+" : "") + m.preamp + " dB"; }
      /* balance + limiter now come back in the reply — restore them so the
         modal reflects the real engine state (they used to always show
         centered / off, and the limiter is actually ON by default). */
      if (bal && m.balance != null) {
        const pct = Math.round(m.balance * 100);
        bal.value = pct;
        if (balVal) balVal.textContent = pct === 0 ? "C" : (pct < 0 ? "L" + (-pct) : "R" + pct);
      }
      if (enLim && m.limiter != null) enLim.checked = !!m.limiter;
    });

    function openEq() { overlay.hidden = false; send({ cmd: "eq", action: "get" }); }
    /* graceful out — the panel exits per the overlay channel, then hides */
    function closeEq() { motion.close(overlay, () => { overlay.hidden = true; }); }
    btnEq.addEventListener("click", openEq);
    $("#eq-close").addEventListener("click", closeEq);
    overlay.addEventListener("click", (e) => { if (e.target === overlay) closeEq(); });
    document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !overlay.hidden) closeEq(); });

    if (enDsp) enDsp.addEventListener("change", () => send({ cmd: "eq", action: "enable", on: enDsp.checked }));
    if (enEq) enEq.addEventListener("change", () => send({ cmd: "eq", action: "eqon", on: enEq.checked }));
    if (enLim) enLim.addEventListener("change", () => send({ cmd: "eq", action: "limiter", on: enLim.checked, threshold: -1, ceiling: -0.1 }));
    if (selPreset) selPreset.addEventListener("change", () => send({ cmd: "eq", action: "preset", preset: parseInt(selPreset.value, 10) || 0 }));
    if (preamp) preamp.addEventListener("input", () => { const g = parseFloat(preamp.value); preampVal.textContent = (g > 0 ? "+" : "") + g + " dB"; send({ cmd: "eq", action: "preamp", gain: g }); });
    if (bal) bal.addEventListener("input", () => { const v = parseInt(bal.value, 10) / 100; balVal.textContent = v === 0 ? "C" : (v < 0 ? "L" + Math.round(-v * 100) : "R" + Math.round(v * 100)); send({ cmd: "eq", action: "balance", v }); });
    const btnReset = $("#eq-reset");
    if (btnReset) btnReset.addEventListener("click", () => { send({ cmd: "eq", action: "preset", preset: 0 }); });
    /* auto-enable DSP the first time the user opens the EQ so changes are audible */
    btnEq.addEventListener("click", () => { if (enDsp && !enDsp.checked) { enDsp.checked = true; send({ cmd: "eq", action: "enable", on: true }); } }, { once: true });
  })();

  /* ---------- spectrum visualizer (32-bar FFT from the C engine) ---------- */
  (function initSpectrum() {
    const cv = $("#np-spectrum");
    if (!cv) return;
    const ctx = cv.getContext("2d");
    const bars = new Float32Array(32);
    const disp = new Float32Array(32);
    let raf = 0, pollAt = 0;
    on("spectrum", (m) => {
      if (Array.isArray(m.bars)) {
        for (let i = 0; i < m.bars.length && i < bars.length; i++) bars[i] = m.bars[i];
      }
    });
    /* the canvas lives in the now-playing panel — when that is collapsed the
       bars are invisible, so the loop + its 20 Hz bridge polling must PARK
       (they used to run forever while "listening + browsing the library") */
    const visible = () => !lowPower() && !window.__mnWinHidden &&
                          !E.app.classList.contains("np-collapsed");
    function frame(ts) {
      raf = 0;
      if (!visible()) return;   /* parked; the toggle handler re-wakes us */
      const playing = state.now && state.now.playing;
      /* poll the backend for fresh bars ~20/s while playing */
      if (playing && ts - pollAt > 50) { pollAt = ts; send({ cmd: "spectrum" }); }
      const W = cv.width, H = cv.height;
      ctx.clearRect(0, 0, W, H);
      const n = bars.length, gap = 2, bw = (W - gap * (n - 1)) / n;
      /* cached accent (refreshAccentCache on theme change) — the old
         getComputedStyle() here forced a style recalc EVERY frame */
      ctx.fillStyle = accentCache;
      let decaying = false;
      for (let i = 0; i < n; i++) {
        const target = playing ? bars[i] : 0;
        disp[i] += (target - disp[i]) * (target > disp[i] ? 0.5 : 0.14);
        if (disp[i] > 0.01) decaying = true;
        const h = Math.max(1, disp[i] * H);
        ctx.globalAlpha = 0.35 + 0.65 * disp[i];
        ctx.fillRect(i * (bw + gap), H - h, bw, h);
      }
      ctx.globalAlpha = 1;
      if (playing || decaying) raf = requestAnimationFrame(frame);
    }
    function wake() { if (!raf) raf = requestAnimationFrame(frame); }
    /* NOTE: tap(), not on() — on() is single-handler-per-type and a second
       on("now") here would REPLACE the main now-handler, killing all playback
       UI updates (this exact mistake broke the app in a previous session). */
    tap("now", () => { if (state.now && state.now.playing && visible()) wake(); });
    /* re-wake when the panel is expanded again (deferred: the class is
       toggled by ANOTHER click handler registered after this one) */
    const npBtn = $("#btn-toggle-np");
    if (npBtn) npBtn.addEventListener("click", () => {
      setTimeout(() => { if (visible()) wake(); }, 0);
    });
  })();

  /* ---------- ±30s / ±5min skip (long single-file books, podcasts) ---------- */
  (function initSkip() {
    const back = $("#btn-back30"), fwd = $("#btn-fwd30");
    if (!back || !fwd) return;
    function skip(deltaMs) {
      if (!(smoothDur > 0)) return;
      const target = clamp(smoothPos + deltaMs, 0, Math.max(0, smoothDur - 500));
      smoothPos = target; lastNowTs = performance.now();  /* snappy pill */
      send({ cmd: "seek", ms: Math.round(target) });
      wakeLoop();
    }
    back.addEventListener("click", (ev) => skip(ev.shiftKey ? -300000 : -30000));
    fwd.addEventListener("click",  (ev) => skip(ev.shiftKey ?  300000 :  30000));
  })();

  /* ---------- bookmarks (named positions within a book) ---------- */
  (function initBookmark() {
    const btn = $("#btn-bookmark");
    if (!btn) return;
    btn.addEventListener("click", () => {
      const n = state.now;
      if (!n || !(n.track_id > 0) || !(n.album_id > 0)) return;
      send({ cmd: "bookmarkadd", album: n.album_id, track: n.track_id,
             pos_ms: Math.round(smoothPos), note: "" });
      /* quick visual confirm */
      btn.classList.add("active");
      setTimeout(() => btn.classList.remove("active"), 900);
    });
  })();
  /* bookmark list for the OPEN expand panel (kind views request it) */
  on("bookmarks", (m) => {
    if (!m || String(state.expandedAlbum) !== String(m.album)) return;
    const panel = state._expandPanel;
    if (!panel) return;
    let sec = panel.querySelector(".aex-bookmarks");
    if (sec) sec.remove();
    const items = m.items || [];
    if (!items.length) return;
    sec = el("div", "aex-bookmarks", "");
    sec.appendChild(el("div", "aex-bm-head", "Bookmarks"));
    for (const bm of items) {
      const row = el("div", "aex-bm-row", "");
      const jump = el("span", "aex-bm-jump",
                      "🔖 " + fmtLong(bm.pos_ms) + (bm.note ? " · " + esc(bm.note) : ""));
      jump.title = "Play from this bookmark";
      jump.addEventListener("click", () => {
        send({ cmd: "playalbum", id: m.album, track: bm.track });
        setTimeout(() => send({ cmd: "seek", ms: bm.pos_ms }), 800);
      });
      const del = el("span", "aex-bm-del", "✕");
      del.title = "Delete bookmark";
      del.addEventListener("click", () =>
        send({ cmd: "bookmarkdel", id: bm.id, album: m.album }));
      row.appendChild(jump); row.appendChild(del);
      sec.appendChild(row);
    }
    const acts = panel.querySelector(".aex-acts");
    if (acts && acts.parentNode) acts.parentNode.insertBefore(sec, acts.nextSibling);
    else panel.appendChild(sec);
  });

  /* ---------- playback speed (pitch-preserved, audiobooks) ---------- */
  (function initSpeed() {
    const btn = $("#btn-speed");
    if (!btn) return;
    const SPEEDS = [0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3];
    function label(v) { return (v % 1 === 0 ? v : v.toFixed(2).replace(/0$/, "")) + "×"; }
    function applyUI(v) {
      btn.textContent = label(v);
      btn.classList.toggle("active", v !== 1);
      btn.title = v === 1 ? "Playback speed (pitch-preserved)"
                          : "Playing at " + label(v) + " — pitch preserved";
    }
    let cur = 1;
    try { cur = parseFloat(localStorage.getItem("mn.speed")) || 1; } catch (_) {}
    if (cur < 0.5 || cur > 3) cur = 1;
    applyUI(cur);
    /* re-apply the persisted speed once the engine is up (boot) */
    if (cur !== 1) setTimeout(() => send({ cmd: "speed", v: cur }), 1500);
    on("speed", (m) => {
      if (m && typeof m.v === "number") { cur = m.v; applyUI(cur); }
    });
    btn.addEventListener("click", (ev) => {
      const menu = el("div", "pl-picker");
      const dismiss = () => {
        document.removeEventListener("click", onDoc, true);
        motion.close(menu, () => menu.remove());
      };
      menu.appendChild(el("div", "pl-picker-head", "Playback speed"));
      SPEEDS.forEach((v) => {
        const it = el("div", "pl-picker-item" + (v === cur ? " on" : ""), label(v));
        it.addEventListener("click", () => {
          try { localStorage.setItem("mn.speed", String(v)); } catch (_) {}
          send({ cmd: "speed", v: v });
          dismiss();
        });
        menu.appendChild(it);
      });
      document.body.appendChild(menu);
      menu.style.left = Math.min(ev.clientX, window.innerWidth - 200) + "px";
      menu.style.top = Math.max(10, ev.clientY - 300) + "px";
      const onDoc = (e) => { if (!menu.contains(e.target)) dismiss(); };
      setTimeout(() => document.addEventListener("click", onDoc, true), 0);
    });
  })();

  /* ---------- sleep timer ---------- */
  (function initSleep() {
    const btn = $("#btn-sleep");
    if (!btn) return;
    on("sleeptimer", (m) => {
      const remaining = m.remaining || 0;
      btn.classList.toggle("active", remaining > 0);
      btn.title = remaining > 0 ? ("Sleep in ~" + Math.ceil(remaining / 60) + " min") : "Sleep timer";
    });
    btn.addEventListener("click", (ev) => {
      const menu = el("div", "pl-picker");
      const dismiss = () => {
        document.removeEventListener("click", onDoc, true);
        motion.close(menu, () => menu.remove());   /* graceful out */
      };
      menu.appendChild(el("div", "pl-picker-head", "Sleep timer"));
      [["Off", 0], ["15 min", 15], ["30 min", 30], ["45 min", 45], ["60 min", 60], ["90 min", 90]].forEach(([label, min]) => {
        const it = el("div", "pl-picker-item", label);
        it.addEventListener("click", () => { send({ cmd: "sleeptimer", minutes: min }); dismiss(); });
        menu.appendChild(it);
      });
      document.body.appendChild(menu);
      menu.style.left = Math.min(ev.clientX, window.innerWidth - 200) + "px";
      menu.style.top = Math.max(10, ev.clientY - 260) + "px";
      const onDoc = (e) => { if (!menu.contains(e.target)) dismiss(); };
      setTimeout(() => document.addEventListener("click", onDoc, true), 0);
    });
  })();

  /* ---------- extra transport controls ---------- */
  /* stop = rewind to 0 and pause if playing */
  $("#btn-stop").addEventListener("click", () => {
    send({ cmd: "seek", ms: 0 });
    smoothPos = 0; lastNowTs = performance.now();
    if (state.now && state.now.playing) send({ cmd: "toggle" });
  });

  /* A-B repeat — pure UI: press once = set A, again = set B (loops via seek
     in tickSeek), third = clear. Cleared automatically on track change. */
  const ab = { a: null, b: null };
  const btnAB = $("#btn-ab");
  function abClear() {
    ab.a = null; ab.b = null;
    btnAB.classList.remove("a-set", "active");
    btnAB.title = "A-B repeat — set point A";
  }
  btnAB.addEventListener("click", () => {
    const p = currentPos().pos;
    if (ab.a == null) {
      ab.a = p;
      btnAB.classList.add("a-set");
      btnAB.title = "A set at " + fmtTime(p) + " — click to set B";
    } else if (ab.b == null) {
      ab.b = Math.max(p, ab.a + 1000);   /* B always after A */
      btnAB.classList.remove("a-set");
      btnAB.classList.add("active");
      btnAB.title = "Looping " + fmtTime(ab.a) + " – " + fmtTime(ab.b) + " — click to clear";
    } else abClear();
  });

  /* thumbs for the CURRENT track (now.liked + now.track_id) */
  const btnLike = $("#btn-like"), btnDislike = $("#btn-dislike");
  function updateThumbsUI(v) {
    btnLike.classList.toggle("on", v === 1);
    btnDislike.classList.toggle("on", v === -1);
  }
  function sendNowLike(v) {
    const n = state.now;
    if (!n || n.track_id == null || n.track_id < 0) return;
    const next = ((n.liked || 0) === v) ? 0 : v;
    n.liked = next;
    send({ cmd: "like", id: n.track_id, v: next });
    updateThumbsUI(next);
    const row = state.trkRows.find((r) => r.id === n.track_id);
    if (row) row.liked = next;
  }
  btnLike.addEventListener("click", () => sendNowLike(1));
  btnDislike.addEventListener("click", () => sendNowLike(-1));

  /* clicking the elapsed time toggles remaining-time display */
  E.tElapsed.addEventListener("click", () => {
    PREFS.remaining = !PREFS.remaining;
    savePref("remaining", PREFS.remaining ? 1 : 0);
  });

  function seekFromEvent(ev) {
    const r = E.seekTrack.getBoundingClientRect();   /* event-driven, not per-frame */
    const pct = clamp((ev.clientX - r.left) / r.width, 0, 1);
    setKnob(pct, true);
    lastFrac = pct;
    drawWave(pct);
    if (smoothDur > 0) E.tElapsed.textContent = fmtTime(pct * smoothDur);
    return pct;
  }
  E.seekTrack.addEventListener("pointerdown", (ev) => {
    if (!smoothDur) return;
    state.seekDrag = true; E.seekTrack.setPointerCapture(ev.pointerId); seekFromEvent(ev);
    wakeLoop();
  });
  E.seekTrack.addEventListener("pointermove", (ev) => { if (state.seekDrag) seekFromEvent(ev); });
  E.seekTrack.addEventListener("pointerup", (ev) => {
    if (!state.seekDrag) return;
    const pct = seekFromEvent(ev);
    state.seekDrag = false;
    setKnob(pct, false);   /* drop the drag scale */
    const ms = Math.round(pct * smoothDur);
    smoothPos = ms; lastNowTs = performance.now();
    send({ cmd: "seek", ms });
  });

  /* Volume snaps to whole-number percent (OCD-friendly stepped knob). */
  function setVolUI(v) {
    const pct = Math.round(clamp(v, 0, 1) * 100);
    E.volFill.style.width = pct + "%";
    E.volKnob.style.left = pct + "%";
    const vv = $("#vol-val"); if (vv) vv.textContent = String(pct);
    /* vertical popup mirrors the same level */
    const pf = $("#vol-pop-fill"), pk = $("#vol-pop-knob"), pv = $("#vol-pop-val");
    if (pf) pf.style.height = pct + "%";
    if (pk) pk.style.bottom = pct + "%";
    if (pv) pv.textContent = String(pct);
    /* icon reflects the level: slash when silent, one wave when quiet */
    const mb = $("#btn-mute");
    if (mb) {
      mb.classList.toggle("muted", pct === 0);
      mb.classList.toggle("vol-low", pct > 0 && pct < 40);
    }
  }
  function volFromEvent(ev) {
    const r = E.volTrack.getBoundingClientRect();
    const raw = clamp((ev.clientX - r.left) / r.width, 0, 1);
    const snapped = Math.round(raw * 100) / 100;   /* whole-percent steps */
    setVolUI(snapped);
    return snapped;
  }
  let lastVolSent = -1;
  function sendVol(v) {
    if (v !== lastVolSent) {
      lastVolSent = v;
      send({ cmd: "volume", v });
      try { localStorage.setItem("mn.volume", String(v)); } catch (_) {}
    }
  }
  E.volTrack.addEventListener("pointerdown", (ev) => {
    state.volDrag = true; E.volTrack.setPointerCapture(ev.pointerId);
    sendVol(volFromEvent(ev));
  });
  E.volTrack.addEventListener("pointermove", (ev) => { if (state.volDrag) sendVol(volFromEvent(ev)); });
  E.volTrack.addEventListener("pointerup", (ev) => {
    if (state.volDrag) {
      sendVol(volFromEvent(ev));
      state.volDrag = false;
      motion.bump($("#vol-val"));   /* value-bump on drag END only, never per-frame */
    }
  });
  /* mouse wheel over the volume area OR the playback transport nudges the
     volume by the configured step (Settings → Playback; default 5%).
     Deliberately scoped to those two areas only — not the whole player bar. */
  let volWheelBumpT = 0;   /* wheel events burst — bump once, at burst END */
  function volWheel(ev) {
    ev.preventDefault();
    const cur = state.now && state.now.volume != null ? state.now.volume : 1;
    const step = PREFS.volstep || 5;
    const next = clamp(Math.round(cur * 100) + (ev.deltaY < 0 ? step : -step), 0, 100) / 100;
    setVolUI(next); sendVol(next);
    if (state.now) state.now.volume = next;
    showVolPop();   /* scrolling reveals the vertical level readout */
    clearTimeout(volWheelBumpT);
    volWheelBumpT = setTimeout(() => {
      motion.bump($("#vol-pop-val"));
      motion.bump($("#vol-val"));
    }, 150);
  }
  /* Scroll anywhere on the player bar to change volume — EXCEPT over the
     waveform seek track (where scroll could be confused with scrubbing). */
  $("#player").addEventListener("wheel", (ev) => {
    if (ev.target.closest("#seek-track")) return;
    volWheel(ev);
  }, { passive: false });

  /* ---- vertical volume popup --------------------------------------
     The horizontal slider needs ~150px the player doesn't always have.
     When the right cell gets narrow (body.vol-compact), the slider + %
     collapse to just the (never-hidden) speaker icon, and volume moves
     into a vertical popup: it appears while scrolling, on hovering the
     icon, and can be dragged like any slider.
     It is a TOP-LAYER POPOVER (popover="manual"): the old absolute
     version was clipped by .pl-right{overflow:hidden} and its CSS
     display overrode [hidden], so it showed permanently and cut off. */
  const volPop = $("#vol-pop");
  let volPopHide = 0;
  function volPopOpen() {
    return !!(volPop && volPop.matches && volPop.matches(":popover-open"));
  }
  function positionVolPop() {
    /* anchor: centered above the speaker icon, clamped to the viewport */
    const mb = $("#btn-mute");
    if (!mb || !volPop) return;
    const r = mb.getBoundingClientRect();
    const pw = volPop.offsetWidth || 40, ph = volPop.offsetHeight || 170;
    const left = clamp(r.left + r.width / 2 - pw / 2, 8, innerWidth - pw - 8);
    const top = Math.max(8, r.top - ph - 12);
    volPop.style.left = left + "px";
    volPop.style.top = top + "px";
  }
  function hideVolPop() {
    if (volPopOpen()) volPop.hidePopover();
  }
  function showVolPop(sticky) {
    if (!volPop) return;
    /* the popup exists FOR compact mode — with the horizontal slider
       visible it would just duplicate it */
    if (!document.body.classList.contains("vol-compact")) { hideVolPop(); return; }
    if (!volPopOpen() && volPop.showPopover) volPop.showPopover();
    positionVolPop();
    clearTimeout(volPopHide);
    if (!sticky) volPopHide = setTimeout(hideVolPop, 1500);
  }
  function hideVolPopSoon() {
    clearTimeout(volPopHide);
    volPopHide = setTimeout(hideVolPop, 350);
  }
  {
    const root = $("#vol-root"), mb = $("#btn-mute");
    if (root && volPop && mb) {
      mb.addEventListener("mouseenter", () => {
        if (document.body.classList.contains("vol-compact")) showVolPop(true);
      });
      root.addEventListener("mouseleave", hideVolPopSoon);
      volPop.addEventListener("mouseenter", () => showVolPop(true));
      /* drag / click the vertical track */
      const pt = $("#vol-pop-track");
      const volFromY = (ev) => {
        const r = pt.getBoundingClientRect();
        return clamp(1 - (ev.clientY - r.top) / r.height, 0, 1);
      };
      let popDrag = false;
      if (pt) {
        pt.addEventListener("pointerdown", (ev) => {
          popDrag = true; pt.setPointerCapture(ev.pointerId);
          /* state.volDrag guards the 4 Hz now-poll from stomping the UI
             mid-drag (it only checked the horizontal slider's drag) */
          state.volDrag = true;
          const v = Math.round(volFromY(ev) * 100) / 100;
          setVolUI(v); sendVol(v); if (state.now) state.now.volume = v;
          showVolPop(true);
        });
        pt.addEventListener("pointermove", (ev) => {
          if (!popDrag) return;
          const v = Math.round(volFromY(ev) * 100) / 100;
          setVolUI(v); sendVol(v); if (state.now) state.now.volume = v;
        });
        pt.addEventListener("pointerup", () => {
          popDrag = false; state.volDrag = false; showVolPop();
        });
      }
    }
  }
  /* compact-mode switch: driven by the real width of the player's right
     cell. rAF-coalesced, reads the observed size from the RO entry's
     contentRect (never a forced clientWidth read), and uses HYSTERESIS
     (compact below 290, full above 320) so it can't oscillate at the
     boundary and ping-pong the fitPills observer during a drag. */
  {
    const rt = document.querySelector(".pl-right-top");
    if (rt && window.ResizeObserver) {
      let raf = 0, lastW = rt.clientWidth || 400;
      const apply = () => {
        raf = 0;
        const compact = document.body.classList.contains("vol-compact");
        if (!compact && lastW < 290) document.body.classList.add("vol-compact");
        else if (compact && lastW > 320) {
          document.body.classList.remove("vol-compact");
          hideVolPop();          /* horizontal slider is back — popup done */
        }
        if (volPopOpen()) positionVolPop();   /* keep anchored on resize */
      };
      const ro = new ResizeObserver((entries) => {
        const cr = entries[0] && entries[0].contentRect;
        if (cr) lastW = cr.width;
        if (!raf) raf = requestAnimationFrame(apply);
      });
      ro.observe(rt);
      apply();
    }
  }

  /* ============================================================
     GLOBAL KEYBOARD SHORTCUTS + help overlay (press ? or F1)
     ============================================================ */
  const SHORTCUTS = [
    ["Space", "Play / pause"],
    ["→ / ←", "Seek +/- 5 s"],
    ["Ctrl+→ / Ctrl+←", "Next / previous track"],
    ["↑ / ↓", "Volume up / down"],
    ["S", "Toggle shuffle"],
    ["R", "Cycle repeat"],
    ["L", "Like current track"],
    ["E", "Equalizer"],
    ["/", "Focus search"],
    ["Alt+← / Alt+→", "Navigate back / forward"],
    ["1–5", "Switch view (Tracks / Albums / Artists / Genres / Playlists)"],
    ["? or F1", "This help"],
    ["Esc", "Close overlay"],
  ];
  function toggleHelp() {
    let ov = $("#help-overlay");
    if (ov) { motion.close(ov, () => ov.remove()); return; }
    ov = el("div", "help-overlay"); ov.id = "help-overlay";
    const dismiss = () => motion.close(ov, () => ov.remove());
    const panel = el("div", "help-panel");
    const head = el("div", "help-head");
    head.appendChild(el("h2", "", "Keyboard shortcuts"));
    const x = el("button", "eq-close", "✕");
    x.addEventListener("click", dismiss);
    head.appendChild(x);
    panel.appendChild(head);
    const grid = el("div", "help-grid");
    SHORTCUTS.forEach(([k, d]) => {
      grid.appendChild(el("kbd", "help-key", k));
      grid.appendChild(el("div", "help-desc", d));
    });
    panel.appendChild(grid);
    ov.appendChild(panel);
    ov.addEventListener("click", (e) => { if (e.target === ov) dismiss(); });
    document.body.appendChild(ov);
  }
  function volNudge(delta) {
    const cur = (state.now && state.now.volume != null) ? state.now.volume : 1;
    const nv = clamp(cur + delta, 0, 1);
    send({ cmd: "volume", v: nv });
    try { localStorage.setItem("mn.volume", String(nv)); } catch (_) {}
    if (typeof setVolUI === "function") setVolUI(nv);
  }
  document.addEventListener("keydown", (e) => {
    const t = e.target.tagName;
    const typing = t === "INPUT" || t === "SELECT" || t === "TEXTAREA" || e.target.isContentEditable;
    if ((e.key === "?" || e.key === "F1") && !typing) { e.preventDefault(); toggleHelp(); return; }
    if (typing) return;
    if (e.key === "/") { const s = $("#search"); if (s) { e.preventDefault(); s.focus(); } return; }
    if (e.code === "Space") { e.preventDefault(); send({ cmd: "toggle" }); return; }
    if (e.key === "ArrowRight" && !e.altKey) {
      e.preventDefault();
      if (e.ctrlKey) send({ cmd: "next" });
      else send({ cmd: "seek", ms: Math.max(0, (smoothPos || 0) + 5000) });
      return;
    }
    if (e.key === "ArrowLeft" && !e.altKey) {
      e.preventDefault();
      if (e.ctrlKey) send({ cmd: "prev" });
      else send({ cmd: "seek", ms: Math.max(0, (smoothPos || 0) - 5000) });
      return;
    }
    if (e.key === "ArrowUp") { e.preventDefault(); volNudge(0.05); return; }
    if (e.key === "ArrowDown") { e.preventDefault(); volNudge(-0.05); return; }
    if (e.key === "s" || e.key === "S") { const on = !(state.now && state.now.shuffle); send({ cmd: "shuffle", on }); return; }
    if (e.key === "r" || e.key === "R") { send({ cmd: "repeat" }); return; }
    if (e.key === "l" || e.key === "L") { if (state.now && state.now.track_id >= 0) sendNowLike(1); return; }
    if (e.key === "e" || e.key === "E") { const b = $("#btn-eq"); if (b) b.click(); return; }
    if (/^[1-6]$/.test(e.key)) {
      /* number keys map 1:1 to the six views (Tracks/Albums/Artists/Genres/
         Folders/Playlists) — 4=Folders was previously skipped */
      switchView(parseInt(e.key, 10) - 1);
      return;
    }
  });

  $("#btn-toggle-np").addEventListener("click", () => {
    E.app.classList.toggle("np-collapsed");
    syncDepthActive();   /* pause the mesh worker while the panel is hidden */
  });

  /* ============================================================
     UI LOCK — default LOCKED: disables drag-rearrange (checked in
     rearrange.js's dragstart) and the panel resizers (CSS).
     ============================================================ */
  window.MnUILocked = localStorage.getItem("mn.uilock") !== "0";
  const btnLock = $("#btn-uilock");
  function applyUILock() {
    E.app.classList.toggle("ui-locked", window.MnUILocked);
    btnLock.textContent = window.MnUILocked ? "🔒" : "🔓";
    btnLock.classList.toggle("unlocked", !window.MnUILocked);
    btnLock.title = window.MnUILocked
      ? "UI locked — click to enable rearranging and panel resizing"
      : "UI unlocked — drag to rearrange, resize panels; click to lock";
  }
  btnLock.addEventListener("click", () => {
    window.MnUILocked = !window.MnUILocked;
    savePref("uilock", window.MnUILocked ? 1 : 0);
    applyUILock();
  });
  applyUILock();

  /* ============================================================
     ACTIVITY STATUS — a general-purpose indicator pinned to the
     bottom of the playback bar. Any background subsystem (art
     scan, folder scan, stem separation, sync, …) reports its
     activity here; the highest-priority active source shows, and
     the strip hides when everything is idle. Public via
     window.__mnStatus so other UI modules can push too.
     ============================================================ */
  const ACT = {
    sources: new Map(),   /* key -> {text, pct(0..1|null), prio, ts} */
  };
  function activityRender() {
    if (!E.plStatus) return;
    /* pick the highest-priority active source (ties: most recent) */
    let best = null;
    ACT.sources.forEach((v) => {
      if (!best || v.prio > best.prio || (v.prio === best.prio && v.ts > best.ts)) best = v;
    });
    if (!best) { E.plStatus.hidden = true; return; }
    E.plStatus.hidden = false;
    E.plStatusText.textContent = best.text;
    const indet = best.pct == null || !(best.pct >= 0);
    E.plStatus.classList.toggle("indeterminate", indet);
    if (!indet && E.plStatusFill)
      E.plStatusFill.style.width = clamp(best.pct * 100, 0, 100) + "%";
  }
  /* set(key, text, pct?) — pct in [0,1] for a bar, null/omit for indeterminate.
     clear(key) removes it. prio: higher wins when several are active. */
  function activitySet(key, text, pct, prio) {
    if (!text) { ACT.sources.delete(key); activityRender(); return; }
    ACT.sources.set(key, {
      text: String(text), pct: (typeof pct === "number" ? pct : null),
      prio: (prio || 0), ts: performance.now(),
    });
    activityRender();
  }
  function activityClear(key) { ACT.sources.delete(key); activityRender(); }
  window.__mnStatus = { set: activitySet, clear: activityClear };

  /* Art-cache scan progress (backend {type:"artscan",done,total} +
     {type:"artdone"}). Priority 2 (below folder scan). */
  on("artscan", (m) => {
    const done = m.done || 0, total = m.total || 0;
    if (total > 0 && done < total)
      activitySet("artscan", "Scanning album art · " + done.toLocaleString() +
        " / " + total.toLocaleString(), done / total, 2);
    else activityClear("artscan");
  });
  on("artdone", () => activityClear("artscan"));
  /* Periodic art-integrity health (backend {type:"arthealth",missing,total}). */
  on("arthealth", (m) => {
    if (m && m.ok === false && m.total)
      activitySet("arthealth", "Healing album art · " + (m.total - m.missing) +
        " / " + m.total, (m.total - m.missing) / m.total, 1);
    else activityClear("arthealth");
  });

  /* ============================================================
     SCAN CARD
     ============================================================ */
  on("scan", (m) => {
    /* general-purpose status strip (priority 3 — folder scan is the loudest) */
    if (m.active) {
      const found = m.found || 0, done = (m.processed || 0) + (m.skipped || 0);
      activitySet("folderscan",
        "Scanning library" + (m.source ? " · " + m.source : ""),
        found ? done / found : null, 3);
    } else activityClear("folderscan");
    const wasActive = state.scanActive;
    state.scanActive = !!m.active;
    E.scanCard.hidden = !m.active;
    /* scan finished: the album set may have changed — regrid on next visit
       (or immediately when the Albums view is open right now) */
    if (wasActive && !m.active) {
      state.albDirty = true;
      /* in-place: a finished scan refreshes CONTENT, not identity — keep the
         user's scroll position (a cold-boot backfill used to teleport a
         stationary grid from 230k to 7.1k with zero input) */
      if (state.view === 1) { state.albDirty = false; resetAlbums(true); loadAlbums(); }
      /* new folders may carry .m3u/.pls playlists — import (idempotent) */
      send({ cmd: "importplaylists" });
    }
    if (!m.active) return;
    const found = m.found || 0, done = m.processed || 0, skip = m.skipped || 0;
    /* NEW files are the real work; unchanged files are skipped in microseconds
       (the incremental index) — say so, or a rescan of a big library reads as
       "scanning everything again". The bar tracks (done+skip)/found so it
       reflects true progress instead of crawling while skips fly by. */
    E.scanCount.textContent = skip
      ? done.toLocaleString() + " new · " + skip.toLocaleString() + " unchanged"
      : done.toLocaleString() + " / " + found.toLocaleString();
    E.scanBar.style.width = (found ? clamp(((done + skip) / found) * 100, 0, 100) : 3) + "%";
    E.scanSource.textContent = m.source || "—";
    E.scanDirs.textContent = (m.dirs_scanned || 0).toLocaleString();
    E.scanSkipped.textContent = skip.toLocaleString();
    E.scanErrors.textContent = ((m.tag_errors || 0) + (m.io_errors || 0)).toLocaleString();
  });

  /* ---------- Add folder flow: native picker -> content-type ---------- */
  $("#btn-addfolder").addEventListener("click", () => send({ cmd: "pickfolder" }));
  $("#btn-rescan").addEventListener("click", () => { send({ cmd: "rescan" }); send({ cmd: "scan" }); });

  on("picked", (m) => {
    if (!m.path) return;
    state.pickedPath = m.path;
    $("#kind-path").textContent = m.path;
    renderKindFolders();
    send({ cmd: "folders" });   /* hidden-banner + legacy-derivation data */
    send({ cmd: "roots" });     /* the SAME authoritative list the Folders
                                   view shows — keeps both in lockstep */
    $("#kind-overlay").hidden = false;
  });
  const closeKindOv = () => { const ko = $("#kind-overlay"); motion.close(ko, () => { ko.hidden = true; }); };
  $("#kind-close").addEventListener("click", closeKindOv);
  $("#kind-overlay").addEventListener("click", (e) => { if (e.target === $("#kind-overlay")) closeKindOv(); });
  $$(".kind-card").forEach((k) => k.addEventListener("click", () => {
    closeKindOv();
    if (!state.pickedPath) return;
    addLibRoot(state.pickedPath, k.dataset.kind);   /* mirror the root locally */
    send({ cmd: "addfolder", path: state.pickedPath, kind: k.dataset.kind });
    send({ cmd: "scan" });
    state.pickedPath = "";
  }));

  /* ============================================================
     SETTINGS MODAL
     ============================================================ */
  const setOv = $("#settings-overlay");
  function openSettings(tab) {
    setOv.hidden = false;
    send({ cmd: "settings" });
    send({ cmd: "audiocaps" });
    send({ cmd: "audiodevices" });
    send({ cmd: "cacheinfo" });
    send({ cmd: "syncstatus" });
    send({ cmd: "roots" });     /* Library tab: per-root stats list */
    send({ cmd: "folders" });   /* …and the hide-state for its switches */
    if (tab && selectSettingsTab) selectSettingsTab(tab);
  }
  $("#btn-settings").addEventListener("click", () => openSettings());
  /* sync.js (and future modules) can deep-link a tab: __mnOpenSettings("sync") */
  window.__mnOpenSettings = openSettings;

  /* ---------- settings tabs (left rail; last-open tab persists) ---------- */
  const APP_VERSION = "2.8.0";
  let selectSettingsTab = null;
  /* ---------- Support / donate / report (topbar + About tab) ---------- */
  const SUPPORT_BTC_ADDR = "1KWgwdNdSir2jeLTAcJzsbw96BCrWczjHD";
  function supportCopyBtc(btn, restore) {
    /* native clipboard via the C host — works regardless of the web
       clipboard permission state (a donation address must never silently
       fail to copy) */
    send({ cmd: "copytext", text: SUPPORT_BTC_ADDR });
    if (btn) {
      btn.textContent = "Copied ✓";
      motion.pop(btn);
      setTimeout(() => { btn.textContent = restore; }, 1800);
    }
  }
  function supportPaypal() {
    send({ cmd: "openurl",
           url: "https://www.paypal.com/donate/?business=stinger2404%40gmail.com&currency_code=USD" });
  }
  function supportReport() {
    /* GitHub Issues, prefilled — public tracker beats a raw mailto (spam,
       no history, no dedup). The version + selftest tip ride the template. */
    const v = ($("#about-version") || {}).textContent || "";
    send({ cmd: "openurl",
           url: "https://github.com/Tendai2404/monatomic-music-player/issues/new?title=" +
                encodeURIComponent("[bug] ") +
                "&body=" + encodeURIComponent(
                  "**Version:** " + v + "\n\n**What happened:**\n\n\n" +
                  "**What I expected:**\n\n\n" +
                  "<sub>Tip: run `monatomic.exe --selftest` and paste the output here.</sub>\n") });
  }
  (function initSupport() {
    /* About tab */
    const btc = $("#donate-btc"), pp = $("#donate-paypal"), ri = $("#report-issue");
    const panel = $("#btc-panel"), copy = $("#btc-copy");
    if (btc && panel) btc.addEventListener("click", () => {
      panel.hidden = !panel.hidden;
      if (!panel.hidden) motion.pop(panel);
    });
    if (copy) copy.addEventListener("click", () => supportCopyBtc(copy, "Copy address"));
    if (pp) pp.addEventListener("click", supportPaypal);
    if (ri) ri.addEventListener("click", supportReport);
    /* license viewer: fetch the shipped markdown, show in a simple modal */
    function showDoc(file, title) {
      fetch(file).then((r) => r.ok ? r.text() : Promise.reject())
        .then((txt) => {
          const ov = el("div", "modal-overlay doc-overlay");
          const box = el("div", "doc-box");
          const head = el("div", "doc-head", "");
          head.appendChild(el("span", "doc-title", title));
          const x = el("button", "btn btn-ghost doc-close", "✕");
          x.addEventListener("click", () => motion.close(ov, () => ov.remove()));
          head.appendChild(x);
          box.appendChild(head);
          const pre = el("pre", "doc-body", txt);
          box.appendChild(pre);
          ov.appendChild(box);
          ov.addEventListener("click", (e) => {
            if (e.target === ov) motion.close(ov, () => ov.remove());
          });
          document.body.appendChild(ov);
        })
        .catch(() => { if (window.__mnToast) window.__mnToast("Document not found"); });
    }
    const vl = $("#view-license"), vt = $("#view-thirdparty");
    if (vl) vl.addEventListener("click", () => showDoc("LICENSE.md", "Monatomic Noncommercial License"));
    if (vt) vt.addEventListener("click", () => showDoc("THIRD-PARTY.md", "Third-party notices"));
    /* topbar cluster (before the view selector) */
    const tpp = $("#tb-paypal"), tri = $("#tb-issue"), tbtc = $("#tb-btc");
    if (tpp) tpp.addEventListener("click", supportPaypal);
    if (tri) tri.addEventListener("click", supportReport);
    if (tbtc) tbtc.addEventListener("click", (ev) => {
      /* anchored QR popover, same pattern as the sleep/speed pickers */
      const pop = el("div", "pl-picker btc-pop");
      const dismiss = () => {
        document.removeEventListener("click", onDoc, true);
        motion.close(pop, () => pop.remove());
      };
      pop.appendChild(el("div", "pl-picker-head", "Donate Bitcoin"));
      const img = el("img");
      img.src = "btc-qr.png"; img.alt = "Bitcoin donation QR";
      img.width = 164; img.height = 164; img.className = "btc-pop-qr";
      pop.appendChild(img);
      const ad = el("code", "btc-addr", SUPPORT_BTC_ADDR);
      pop.appendChild(ad);
      const cp = el("button", "btn btn-mini", "Copy address");
      cp.addEventListener("click", () => supportCopyBtc(cp, "Copy address"));
      pop.appendChild(cp);
      document.body.appendChild(pop);
      const r = ev.currentTarget.getBoundingClientRect();
      pop.style.left = Math.min(r.left, window.innerWidth - 230) + "px";
      pop.style.top = (r.bottom + 8) + "px";
      const onDoc = (e) => { if (!pop.contains(e.target)) dismiss(); };
      setTimeout(() => document.addEventListener("click", onDoc, true), 0);
    });
  })();

  function renderAboutTab() {
    const v = $("#about-version");
    if (v) v.textContent = "Version " + APP_VERSION;
    const host = $("#about-modules");
    if (!host || host.dataset.filled) return;
    host.dataset.filled = "1";
    try {
      const t = (window.MN && MN.topology) ? MN.topology() : null;
      (t && t.nodes ? t.nodes : []).forEach((n) => {
        const row = el("div", "about-mod");
        row.appendChild(el("span", "about-mod-name", n.name));
        row.appendChild(el("span", "about-mod-ver", "v" + n.version));
        host.appendChild(row);
      });
    } catch (_) {}
    if (!host.childElementCount) host.appendChild(el("div", "set-val", "—"));
  }
  (function initSettingsTabs() {
    const SETTAB_KEY = "mn.settab";
    const tabs = $$("#settings-tabs .stab");
    const panels = $$("#settings-panels .set-tab");
    if (!tabs.length || !panels.length) return;
    function show(name) {
      if (!tabs.some((t) => t.dataset.tab === name)) name = "playback";
      tabs.forEach((t) => t.classList.toggle("on", t.dataset.tab === name));
      panels.forEach((p) => { p.hidden = p.dataset.tab !== name; });
      try { localStorage.setItem(SETTAB_KEY, name); } catch (_) {}
      if (name === "about") renderAboutTab();
      /* scroll-reveal channel: sections of the shown tab fade up once */
      const pane = panels.find((p) => p.dataset.tab === name);
      if (pane && motion.get("reveal") === "on" && !motion.reduced()) {
        pane.querySelectorAll(".set-section:not(.mo-revealed)")
            .forEach((s) => s.classList.add("mo-reveal"));
        motion.revealScan(pane);
      }
    }
    tabs.forEach((t) => t.addEventListener("click", () => show(t.dataset.tab)));
    selectSettingsTab = show;
    show(localStorage.getItem(SETTAB_KEY) || "playback");
  })();
  { const om = $("#ai-openmodels");
    if (om) om.addEventListener("click", () => { setOv.hidden = true; openModelsView(); });
  }

  /* ---------- Layout & type: zones + row height + per-element ---------- */
  (function initCustomize() {
    const cz = MN.get("custom");
    /* row height */
    const rh = $("#cz-rowh"), rhv = $("#cz-rowh-val");
    if (rh) {
      rh.value = String(cz.rowHeight());
      if (rhv) rhv.textContent = cz.rowHeight() + " px";
      rh.addEventListener("input", () => {
        cz.setRowHeight(+rh.value);
        if (rhv) rhv.textContent = rh.value + " px";
      });
    }
    /* zone scale sliders (generated) */
    const zhost = $("#cz-zones");
    if (zhost) {
      cz.zones.forEach((z) => {
        const row = el("div", "set-row");
        row.appendChild(el("span", "set-name", z.label + " scale"));
        const ctl = el("div", "set-ctl");
        const sl = document.createElement("input");
        sl.type = "range"; sl.min = "70"; sl.max = "150"; sl.step = "5";
        sl.value = String(Math.round(cz.getZone(z.key) * 100));
        const val = el("span", "set-val", (cz.getZone(z.key)).toFixed(2) + "×");
        sl.addEventListener("input", () => {
          cz.setZone(z.key, (+sl.value) / 100);
          val.textContent = ((+sl.value) / 100).toFixed(2) + "×";
        });
        ctl.appendChild(sl); ctl.appendChild(val);
        row.appendChild(ctl);
        zhost.appendChild(row);
      });
    }
    /* per-element editor */
    const sel = $("#cz-el");
    const sScale = $("#cz-scale"), vScale = $("#cz-scale-val");
    const sFont = $("#cz-font"), vFont = $("#cz-font-val");
    const sX = $("#cz-x"), sY = $("#cz-y"), btnReset = $("#cz-reset");
    if (sel) {
      cz.elements.forEach((e) => {
        const o = document.createElement("option");
        o.value = e.key; o.textContent = e.label;
        sel.appendChild(o);
      });
      const loadEl = () => {
        const c = cz.getEl(sel.value);
        if (sScale) { sScale.value = String(Math.round(c.scale * 100)); if (vScale) vScale.textContent = c.scale.toFixed(2) + "×"; }
        if (sFont) { sFont.value = String(Math.round(c.font * 100)); if (vFont) vFont.textContent = c.font.toFixed(2) + "×"; }
        if (sX) sX.value = String(c.x || 0);
        if (sY) sY.value = String(c.y || 0);
      };
      sel.addEventListener("change", loadEl);
      loadEl();
      if (sScale) sScale.addEventListener("input", () => {
        cz.setEl(sel.value, { scale: (+sScale.value) / 100 });
        if (vScale) vScale.textContent = ((+sScale.value) / 100).toFixed(2) + "×";
      });
      if (sFont) sFont.addEventListener("input", () => {
        cz.setEl(sel.value, { font: (+sFont.value) / 100 });
        if (vFont) vFont.textContent = ((+sFont.value) / 100).toFixed(2) + "×";
      });
      if (sX) sX.addEventListener("input", () => cz.setEl(sel.value, { x: +sX.value }));
      if (sY) sY.addEventListener("input", () => cz.setEl(sel.value, { y: +sY.value }));
      if (btnReset) btnReset.addEventListener("click", () => { cz.resetEl(sel.value); loadEl(); });
    }
  })();

  /* ---------- Cache limits + RAM art + sidebar width + reset-all + GPU ---------- */
  (function initCacheControls() {
    /* disk cache caps: persisted locally + pushed to C via settings */
    function sendCaps() {
      send({ cmd: "settings", action: "set",
             stem_cache_gb: +(localStorage.getItem("mn.stemcapgb") || 8),
             art_cache_mb:  +(localStorage.getItem("mn.artcapmb") || 2048),
             depth_batch: localStorage.getItem("mn.depthbatch") === "1" });
    }
    const cs = $("#cap-stems"), csv = $("#cap-stems-val");
    if (cs) {
      cs.value = localStorage.getItem("mn.stemcapgb") || "8";
      if (csv) csv.textContent = cs.value + " GB";
      cs.addEventListener("input", () => {
        try { localStorage.setItem("mn.stemcapgb", cs.value); } catch (_) {}
        if (csv) csv.textContent = cs.value + " GB";
        sendCaps();
      });
    }
    const ca = $("#cap-art"), cav = $("#cap-art-val");
    if (ca) {
      ca.value = localStorage.getItem("mn.artcapmb") || "2048";
      if (cav) cav.textContent = (ca.value / 1024).toFixed(1) + " GB";
      ca.addEventListener("input", () => {
        try { localStorage.setItem("mn.artcapmb", ca.value); } catch (_) {}
        if (cav) cav.textContent = (ca.value / 1024).toFixed(1) + " GB";
        sendCaps();
      });
    }
    /* RAM art cache size */
    const cr = $("#cap-ram"), crv = $("#cap-ram-val");
    if (cr) {
      cr.value = String(MN.get("artram").cap());
      if (crv) crv.textContent = cr.value === "0" ? "off" : cr.value + " covers";
      cr.addEventListener("input", () => {
        MN.get("artram").setCap(+cr.value);
        if (crv) crv.textContent = cr.value === "0" ? "off" : cr.value + " covers";
      });
    }
    /* push persisted caps to C once at boot */
    setTimeout(sendCaps, 1500);

    /* sidebar width (shares the resizer's CSS var + storage) */
    const sw = $("#cz-sidew"), swv = $("#cz-sidew-val");
    if (sw) {
      const cur = parseInt(getComputedStyle(document.documentElement)
                    .getPropertyValue("--side-w")) || 230;
      sw.value = String(cur);
      if (swv) swv.textContent = cur + " px";
      sw.addEventListener("input", () => {
        document.documentElement.style.setProperty("--side-w", sw.value + "px");
        if (swv) swv.textContent = sw.value + " px";
        try {
          const st = JSON.parse(localStorage.getItem("mn.panes") || "{}");
          st.side = +sw.value;
          localStorage.setItem("mn.panes", JSON.stringify(st));
        } catch (_) {}
      });
    }
    /* reset ALL customizations */
    const ra = $("#cz-resetall");
    if (ra) ra.addEventListener("click", () => {
      MN.get("custom").resetAll();
      if (typeof window.__mnToast === "function") window.__mnToast("Layout customizations reset");
    });
    /* GPU-accelerated interface */
    const gpu = $("#set-gpuui");
    if (gpu) {
      const on = localStorage.getItem("mn.gpuui") === "1";
      gpu.checked = on;
      document.documentElement.classList.toggle("gpu-ui", on);
      gpu.addEventListener("change", () => {
        try { localStorage.setItem("mn.gpuui", gpu.checked ? "1" : "0"); } catch (_) {}
        document.documentElement.classList.toggle("gpu-ui", gpu.checked);
      });
    }
  })();

  /* ---------- Resizable album tiles ----------
     --card-min drives BOTH the CSS grid template and the virtual grid's
     column math (vgRefresh reads it), so one variable resizes everything.
     Persisted; slider in the toolbar + Ctrl+scroll over the grid. */
  (function initTileSize() {
    const MIN = 120, MAX = 320;
    const slider = $("#tile-size");
    let raf = 0;
    function apply(px, fromSlider) {
      px = clamp(Math.round(px), MIN, MAX);
      document.documentElement.style.setProperty("--card-min", px + "px");
      try { localStorage.setItem("mn.cardmin", String(px)); } catch (_) {}
      if (slider && !fromSlider) slider.value = String(px);
      /* re-lay the virtual grid once per frame at most */
      if (!raf) raf = requestAnimationFrame(() => {
        raf = 0;
        if (typeof vgRefresh === "function") { VG.firstRow = -1; vgRefresh(); }
      });
    }
    window.__mnSetTileSize = apply;
    /* boot restore (before the first vgRefresh runs) */
    let saved = 0;
    try { saved = parseInt(localStorage.getItem("mn.cardmin"), 10) || 0; } catch (_) {}
    if (saved) apply(saved);
    else if (slider) slider.value = "178";
    if (slider) {
      slider.addEventListener("input", () => apply(+slider.value, true));
      slider.addEventListener("change", () => motion.bump(slider.parentElement));
    }
    /* Ctrl+scroll over the album grid = zoom tiles (native-feeling) */
    E.albumGrid.addEventListener("wheel", (ev) => {
      if (!ev.ctrlKey) return;
      ev.preventDefault();
      const cur = parseFloat(getComputedStyle(document.documentElement)
                    .getPropertyValue("--card-min")) || 178;
      apply(cur + (ev.deltaY < 0 ? 12 : -12));
    }, { passive: false });
  })();

  /* ---------- Animation channel controls (AUTO-GENERATED from the motion
     registry: one granular control per channel, grouped, with hints). The
     hand-written selects were replaced by #mo-granular — adding a channel to
     core/motion.js REG makes it appear here with zero extra UI code. ---------- */
  (function initMotionSettings() {
    const mo = MN.get("motion");
    /* legacy album-style select stays hand-written (it is a view mode) */
    const alb = $("#mo-album");
    if (alb) {
      alb.value = mo.get("albumStyle");
      alb.addEventListener("change", () => mo.set("albumStyle", alb.value));
    }
    const host = $("#mo-granular");
    if (!host || !mo.registry) return;
    let lastGroup = "";
    for (const ch of mo.registry) {
      if (ch.group !== lastGroup) {
        lastGroup = ch.group;
        host.appendChild(el("div", "mo-group", ch.group));
      }
      const row = el("div", "set-row");
      row.appendChild(el("span", "set-name", ch.label));
      if (ch.slider) {
        const wrap = el("div", "set-ctl");
        const inp = document.createElement("input");
        inp.type = "range";
        inp.min = ch.slider.min; inp.max = ch.slider.max; inp.step = ch.slider.step;
        inp.value = String(Math.round(mo.get(ch.key) * 100));
        const val = el("span", "set-val", mo.get(ch.key).toFixed(1) + "×");
        inp.addEventListener("input", () => {
          const v = (+inp.value || 100) / 100;
          mo.set(ch.key, v);
          val.textContent = v.toFixed(1) + "×";
        });
        inp.addEventListener("change", () => mo.bump(val));
        wrap.appendChild(inp); wrap.appendChild(val);
        row.appendChild(wrap);
      } else {
        const sel = document.createElement("select");
        sel.className = "set-select";
        for (const [v, label] of ch.options) {
          const o = document.createElement("option");
          o.value = v; o.textContent = label;
          sel.appendChild(o);
        }
        sel.value = String(mo.get(ch.key));
        sel.addEventListener("change", () => { mo.set(ch.key, sel.value); mo.pop(sel); });
        row.appendChild(sel);
      }
      host.appendChild(row);
      if (ch.hint) host.appendChild(el("div", "mo-hint", ch.hint));
    }
  })();

  /* ---------- batch depth-mapping toggle ---------- */
  { const db = $("#set-depthbatch");
    if (db) {
      db.addEventListener("change", () => {
        send({ cmd: "settings", action: "set", depth_batch: db.checked });
        try { localStorage.setItem("mn.depthbatch", db.checked ? "1" : "0"); } catch (_) {}
      });
    }
  }

  /* ---------- Depth pipeline status pill (above the volume) ---------- */
  let _depthPillKey = "";
  /* the pill looks tappable — make it open the AI Models settings so the
     user can pick/verify the depth model (was a dead element) */
  (function () {
    const pill = $("#depth-pill");
    if (pill) {
      pill.style.cursor = "pointer";
      pill.title = "Depth-mapping model status — click for AI Models settings";
      pill.addEventListener("click", () => {
        if (typeof window.__mnOpenSettings === "function") __mnOpenSettings("ai");
      });
    }
  })();
  function updateDepthPill(m) {
    const pill = $("#depth-pill");
    if (!pill) return;
    if (!theme.art3d) { pill.hidden = true; return; }
    const st = m.depth_state | 0, q = m.depth_queue | 0;
    const busy = m.depth_busy || "", retry = m.depth_retry | 0;
    const done = m.depth_done | 0;
    const key = st + ":" + q + ":" + busy + ":" + retry;
    if (key === _depthPillKey) return;      /* no DOM churn on idle polls */
    _depthPillKey = key;
    pill.hidden = false;
    pill.classList.remove("ok", "warn", "err", "work");
    let txt = "", tip = "3D depth pipeline";
    if (st === 4 && busy) {
      const short = busy.length > 22 ? busy.slice(0, 21) + "…" : busy;
      txt = "3D · mapping “" + esc(short) + "”" + (q > 0 ? " · " + q + " queued" : "");
      pill.classList.add("work");
      tip = "Generating the depth map for “" + busy + "”" +
            (q ? " — " + q + " more queued" : "") +
            " · " + done + " generated this session";
    } else if (st === 3) {
      txt = "3D · failed" + (retry > 0 ? " · retry in " + retry + "s" : " · retrying…");
      pill.classList.add("err");
      tip = "The depth model failed to load or crashed — it recovers automatically (session rebuild + retry)";
    } else if (st === 1) {
      txt = "3D · loading model…";
      pill.classList.add("warn");
      tip = "Building the depth-model session (one-time per launch; larger models take longer)";
    } else if (st === 2) {
      txt = q > 0 ? "3D · ready · " + q + " queued" : "3D · ready";
      pill.classList.add("ok");
      tip = "Depth pipeline healthy · " + done + " maps generated this session";
    } else {
      txt = "3D · idle";
      tip = "Depth model loads on the first cover that needs a map";
    }
    pill.textContent = txt;
    pill.title = tip;
  }

  /* clear-cache acknowledgment: refresh anything showing the cleared data */
  on("cachecleared", (m) => {
    const which = m.which || "";
    if (typeof window.__mnToast === "function") window.__mnToast("Cache cleared — regenerating in background");
    if (which === "art" || which === "webart") {
      state.albDirty = true;
      if (state.view === 1) { state.albDirty = false; resetAlbums(true); loadAlbums(); }
      /* now-playing art + 3D will re-request via the retry loop */
      state._npArtUrl = null;
    }
  });

  /* ---------- Storage & caches panel ---------- */
  on("cacheinfo", (m) => {
    const host = $("#cache-list");
    if (!host) return;
    host.innerHTML = "";
    (m.caches || []).forEach((c) => {
      const row = el("div", "set-row cache-row");
      const name = el("span", "set-name", c.name);
      name.title = c.path || "";
      row.appendChild(name);
      const mb = (c.bytes || 0) / (1024 * 1024);
      const sz = mb >= 1024 ? (mb / 1024).toFixed(2) + " GB" : mb.toFixed(1) + " MB";
      const ctl = el("div", "set-ctl");
      ctl.appendChild(el("span", "set-val",
        sz + " · " + (c.files || 0).toLocaleString() + " files"));
      /* functional Clear for the clearable caches */
      const WHICH = {
        "Neural stems": "stems",
        "Album art (thumbs + hi-res)": "art",
        "Web art (covers + depth maps)": "webart",
      };
      const which = WHICH[c.name];
      if (which) {
        const btn = el("button", "btn btn-ghost cache-clear", "Clear");
        btn.title = "Delete this cache (contents regenerate on demand)";
        btn.addEventListener("click", () => {
          if (confirm("Clear “" + c.name + "”? Contents regenerate on demand.")) {
            send({ cmd: "clearcache", which });
            if (typeof window.__mnToast === "function") window.__mnToast("Clearing " + c.name + "…");
          }
        });
        ctl.appendChild(btn);
      }
      row.appendChild(ctl);
      host.appendChild(row);
    });
    if (!(m.caches || []).length) host.appendChild(el("div", "set-val", "—"));
  });
  { const cd = $("#set-cleardepth");
    if (cd) cd.addEventListener("click", () => {
      if (confirm("Delete ALL generated depth maps? They regenerate in the background over the next minutes.")) {
        send({ cmd: "cleardepth" });
      }
    });
  }
  on("cleardepth", () => { if (typeof window.__mnToast === "function") window.__mnToast("Depth maps cleared — regenerating in background"); });
  /* online-art resolution preference */
  { const sel = $("#set-artfetchres");
    if (sel) {
      sel.value = localStorage.getItem("mn.artfetchres") || "1200";
      sel.addEventListener("change", () => { try { localStorage.setItem("mn.artfetchres", sel.value); } catch (_) {} });
    }
  }
  /* online art fetch result: refresh the grid so the new cover shows */
  on("artfetch", (m) => {
    if (typeof window.__mnToast === "function") {
      window.__mnToast(m.ok ? ("New cover for “" + (m.album || "album") + "”")
                            : ("Art search: " + (m.msg || "failed")));
    }
    if (m.ok) {
      state.albDirty = true;
      if (state.view === 1) { state.albDirty = false; resetAlbums(true); loadAlbums(); }
    }
  });

  /* output-device picker: populated by the C side's audiodevices reply */
  on("audiodevices", (m) => {
    const sel = $("#set-device");
    if (!sel || !Array.isArray(m.devices)) return;
    sel.innerHTML = "";
    m.devices.forEach((d, i) => {
      const o = el("option", null, d.name + (d.default ? "  (default)" : ""));
      o.value = String(i);
      if (d.active) o.selected = true;
      sel.appendChild(o);
    });
  });
  $("#set-device").addEventListener("change", () => {
    send({ cmd: "setdevice", index: +$("#set-device").value });
  });
  const closeSettings = () => motion.close(setOv, () => { setOv.hidden = true; });
  $("#settings-close").addEventListener("click", closeSettings);
  setOv.addEventListener("click", (e) => { if (e.target === setOv) closeSettings(); });

  $$("#theme-bg .chip").forEach((c) => c.addEventListener("click", () => { theme.bg = c.dataset.bg; applyTheme(theme); refreshAccentCache(); }));
  $$("#theme-accent .chip").forEach((c) => c.addEventListener("click", () => { theme.accent = c.dataset.accent; applyTheme(theme); refreshAccentCache(); }));
  $$("#art-motion .chip").forEach((c) => c.addEventListener("click", () => {
    theme.motion = c.dataset.motion; applyTheme(theme);
    if (depthOk) MnDepthArt.setMotion(theme.motion);
    wakeLoop();
  }));
  /* ---------- 3D album art: one switch, three surfaces ----------
     Settings checkbox + playback-bar button + a button on the art itself
     all drive the same theme.art3d flag — the quick buttons exist so a
     low-power machine can kill the volumetric renderer in one click. */
  function setArt3D(on) {
    theme.art3d = !!on; applyTheme(theme);
    updateVolumetricArt(state._npArtUrl);   /* re-evaluate mesh vs CSS vs off */
    syncArt3DButtons();
    wakeLoop();
  }
  function syncArt3DButtons() {
    const a3 = $("#set-art3d"); if (a3) a3.checked = theme.art3d;
    const b1 = $("#btn-art3d"); if (b1) b1.classList.toggle("active", theme.art3d);
    const b2 = $("#np-art-3dtoggle"); if (b2) b2.classList.toggle("active", theme.art3d);
  }
  $("#set-art3d").addEventListener("change", () => setArt3D($("#set-art3d").checked));
  { const b = $("#btn-art3d");
    if (b) b.addEventListener("click", () => setArt3D(!theme.art3d)); }
  { const b = $("#np-art-3dtoggle");
    if (b) b.addEventListener("click", (e) => { e.stopPropagation(); setArt3D(!theme.art3d); }); }
  syncArt3DButtons();

  /* ---------- custom accent color (Settings → Theme) ---------- */
  function hexRgb(h) {
    h = String(h || "").replace("#", "");
    if (h.length === 3) h = h.split("").map((c) => c + c).join("");
    const n = parseInt(h, 16) || 0;
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  function shadeHex(h, f) {  /* f > 0 lighten toward #fff, f < 0 darken toward #000 */
    const t = f > 0 ? 255 : 0, a = Math.abs(f);
    return "#" + hexRgb(h).map((v) => Math.round(v + (t - v) * a).toString(16).padStart(2, "0")).join("");
  }
  function applyCustomAccent(hex) {
    let st = document.getElementById("mn-custom-accent");
    if (!st) { st = document.createElement("style"); st.id = "mn-custom-accent"; document.head.appendChild(st); }
    const rgb = hexRgb(hex);
    st.textContent = ':root[data-accent="custom"]{--accent:' + hex +
      ";--accent-hot:" + shadeHex(hex, 0.15) +
      ";--accent-deep:" + shadeHex(hex, -0.2) +
      ";--accent-glow:rgba(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + ",.35)" +
      ";--accent-2:#00E5FF;--accent-2-hot:#7DF9FF;--accent-2-glow:rgba(0,229,255,.35)}";
  }
  {
    const customInput = $("#accent-custom");
    const customDot = $("#custom-dot");
    const savedHex = localStorage.getItem("mn.customaccent") || "#FB8C00";
    applyCustomAccent(savedHex);          /* restore at boot (data-accent may be "custom") */
    if (customInput) {
      customInput.value = savedHex;
      if (customDot) customDot.style.background = savedHex;
      customInput.addEventListener("input", () => {
        const hex = customInput.value;
        localStorage.setItem("mn.customaccent", hex);
        applyCustomAccent(hex);
        if (customDot) customDot.style.background = hex;
        theme.accent = "custom"; applyTheme(theme);
      });
    }
  }

  function sendSettings() {
    const rgSel = $("#set-rgmode");
    const rgPre = $("#set-rgpreamp");
    send({
      cmd: "settings", action: "set",
      exclusive: $("#set-exclusive").checked,
      replaygain: $("#set-replaygain").checked,
      replaygain_mode: rgSel ? (+rgSel.value || 0) : ($("#set-replaygain").checked ? 1 : 0),
      rg_preamp_db: rgPre ? (+rgPre.value || 0) : 0,
      crossfade_ms: +$("#set-crossfade").value,
      album_art_size: +$("#set-artsize").value,
      hifi_native_bits: $("#set-hifibits") ? $("#set-hifibits").checked : true,
      ab_rate_cap_hz: abCap().rate,
      ab_bits_cap: abCap().bits,
    });
  }
  function abCap() {
    const v = ($("#set-abcap") && $("#set-abcap").value) || "48000-16";
    if (v === "0") return { rate: 0, bits: 0 };
    const [r, b] = v.split("-");
    return { rate: +r || 0, bits: +b || 0 };
  }
  on("settings", (m) => {
    $("#set-exclusive").checked = !!m.exclusive;
    if ($("#set-hifibits") && m.hifi_native_bits != null) $("#set-hifibits").checked = !!m.hifi_native_bits;
    if ($("#set-abcap") && m.ab_rate_cap_hz != null) {
      const key = m.ab_rate_cap_hz > 0 ? m.ab_rate_cap_hz + "-" + (m.ab_bits_cap || 16) : "0";
      if ([...$("#set-abcap").options].some((o) => o.value === key)) $("#set-abcap").value = key;
    }
    $("#set-replaygain").checked = !!m.replaygain;
    if ($("#set-rgmode") && m.replaygain_mode != null) $("#set-rgmode").value = String(m.replaygain_mode);
    if ($("#set-depthbatch") && m.depth_batch != null) $("#set-depthbatch").checked = !!m.depth_batch;
    if ($("#set-infertags") && m.infer_tags != null) $("#set-infertags").checked = !!m.infer_tags;
    if ($("#set-watchfolders") && m.watch_folders != null) $("#set-watchfolders").checked = !!m.watch_folders;
    if (m.low_power != null) {
      try { localStorage.setItem("mn.lowpower", m.low_power ? "1" : "0"); } catch (_) {}
      const lp = $("#set-lowpower"); if (lp) lp.checked = !!m.low_power;
      document.documentElement.classList.toggle("no-anim",
        !!m.low_power || !PREFS.anim);
    }
    if ($("#set-rgpreamp") && m.rg_preamp_db != null) {
      $("#set-rgpreamp").value = m.rg_preamp_db;
      const v = $("#set-rgpreamp-val");
      if (v) v.textContent = (m.rg_preamp_db > 0 ? "+" : "") + m.rg_preamp_db + " dB";
    }
    $("#set-crossfade").value = m.crossfade_ms || 0;
    $("#set-crossfade-val").textContent = m.crossfade_ms ? (m.crossfade_ms / 1000).toFixed(2).replace(/\.?0+$/, "") + " s" : "off";
    $("#set-artsize").value = m.album_art_size || 256;
    $("#set-artsize-val").textContent = (m.album_art_size || 256) + " px";
    if (m.album_art_size > 0) artThumbPx = m.album_art_size;
  });
  { const rm = $("#set-rgmode"); if (rm) rm.addEventListener("change", sendSettings); }
  { const rp = $("#set-rgpreamp"); if (rp) rp.addEventListener("input", () => { const v = $("#set-rgpreamp-val"); if (v) v.textContent = (+rp.value > 0 ? "+" : "") + rp.value + " dB"; sendSettings(); }); }
  /* ---------- Audio Output (hardware capabilities) ---------- */
  on("audiocaps", (m) => {
    const rate = (r) => (r ? (r / 1000).toFixed(r % 1000 ? 1 : 0) + " kHz" : "—");
    $("#ac-device").textContent = m.ok && m.device ? m.device : "unavailable";
    $("#ac-bits").textContent = m.ok && m.max_bit_depth ? m.max_bit_depth + " bit" : "—";
    $("#ac-maxrate").textContent = m.ok ? rate(m.max_rate) : "—";
    $("#ac-channels").textContent = m.ok && m.max_channels
      ? (m.max_channels >= 8 ? "7.1 (" + m.max_channels + "ch)"
        : m.max_channels >= 6 ? "5.1 (" + m.max_channels + "ch)"
        : m.max_channels === 2 ? "Stereo" : m.max_channels + "ch") : "—";
    $("#ac-exclusive").textContent = m.ok ? (m.exclusive_capable ? "Yes" : "No") : "—";
    const row = $("#ac-rates");
    row.innerHTML = "";
    (m.ok && m.rates ? m.rates : []).forEach((r) => row.appendChild(el("span", "chip ro", rate(r))));
    if (!row.childElementCount) row.appendChild(el("span", "chip ro", "—"));
  });

  $("#set-crossfade").addEventListener("input", () => {
    const v = +$("#set-crossfade").value;
    $("#set-crossfade-val").textContent = v ? (v / 1000).toFixed(2).replace(/\.?0+$/, "") + " s" : "off";
  });
  $("#set-crossfade").addEventListener("change", sendSettings);
  $("#set-artsize").addEventListener("input", () => {
    $("#set-artsize-val").textContent = $("#set-artsize").value + " px";
  });
  $("#set-artsize").addEventListener("change", sendSettings);
  $("#set-replaygain").addEventListener("change", sendSettings);
  $("#set-exclusive").addEventListener("change", sendSettings);
  { const h = $("#set-hifibits"); if (h) h.addEventListener("change", sendSettings); }
  { const a = $("#set-abcap"); if (a) a.addEventListener("change", sendSettings); }

  /* ---------- local UI prefs (Playback / Library / Interface) ---------- */
  function initPrefsUI() {
    $("#set-volstep").value = String(PREFS.volstep);
    $("#set-newdays").value = String(PREFS.newdays);
    $("#set-anim").checked = PREFS.anim;
    $("#set-uiscale").value = String(PREFS.uiscale);
    $("#set-uiscale-val").textContent = PREFS.uiscale + "%";
    $("#set-dockidle").checked = PREFS.dockidle;
  }
  $("#set-volstep").addEventListener("change", () => {
    PREFS.volstep = clamp(+$("#set-volstep").value || 5, 1, 5); savePref("volstep", PREFS.volstep);
  });
  $("#set-newdays").addEventListener("change", () => {
    PREFS.newdays = clamp(+$("#set-newdays").value || 7, 1, 60); savePref("newdays", PREFS.newdays);
    /* the NEW window changed — re-render whichever library view is up */
    if (state.view === 1) { resetAlbums(); loadAlbums(); }
    else if (state.view === 0) { resetTracks(); loadTracks(); }
  });
  $("#set-anim").addEventListener("change", () => {
    PREFS.anim = $("#set-anim").checked; savePref("anim", PREFS.anim ? 1 : 0); applyPrefs();
  });
  $("#set-uiscale").addEventListener("input", () => {
    PREFS.uiscale = clamp(+$("#set-uiscale").value || 100, 85, 125);
    $("#set-uiscale-val").textContent = PREFS.uiscale + "%";
    savePref("uiscale", PREFS.uiscale); applyPrefs();
  });
  $("#set-dockidle").addEventListener("change", () => {
    PREFS.dockidle = $("#set-dockidle").checked; savePref("dockidle", PREFS.dockidle ? 1 : 0);
    armDockIdle();
  });
  initPrefsUI();

  /* ---------- Library maintenance: purge missing + reset library ---------- */
  function reloadLibraryViews() {
    if (state.view === 1) { resetAlbums(); loadAlbums(); }
    else if (typeof state.view === "number" && state.view !== 4) { resetTracks(); loadTracks(); }
    send({ cmd: "folders" });
  }

  /* ---- previously-orphan reply types (emitted by C, consumed by nobody) ---- */
  /* "Convert format…" replies with a coming-soon message (was a silent no-op) */
  on("convert", (m) => {
    if (window.__mnToast) __mnToast("Format conversion — " + ((m && m.msg) || "not available yet"));
  });
  /* "Remove from library" — pull the row from the current view immediately */
  on("removed", (m) => {
    if (!m || !m.ok) { if (window.__mnToast) __mnToast("Could not remove the track"); return; }
    if (window.__mnToast) __mnToast("Removed from library");
    state.albDirty = true;
    reloadLibraryViews();
  });
  /* Playlist import count (the list itself refreshes separately) */
  on("plimport", (m) => {
    if (m && m.n > 0 && window.__mnToast) __mnToast("Imported " + m.n + " playlist" + (m.n === 1 ? "" : "s"));
  });
  /* Playlist export result */
  on("plexport", (m) => {
    if (window.__mnToast) __mnToast(m && m.ok
      ? ("Playlist exported" + (m.path ? " → " + m.path : ""))
      : "Playlist export cancelled or failed");
  });

  $("#set-purgemissing").addEventListener("click", () => {
    if (!confirm("Remove every track whose file is missing on disk from the library?\nThe files themselves are not touched.")) return;
    $("#purge-result").textContent = "…";
    send({ cmd: "purgemissing" });
  });
  /* both reply shapes funnel here: current builds answer {type:"purged",n:N},
     the new contract answers {type:"purgemissing",purged:N} */
  function purgeDone(n) {
    state.albDirty = true;   /* albums may have vanished with their tracks */
    $("#purge-result").textContent =
      n ? n.toLocaleString() + " removed" : "none missing";
    if (typeof window.__mnToast === "function") {
      window.__mnToast(n ? "Removed " + n.toLocaleString() + " missing tracks"
                         : "No missing tracks found");
    }
    if (n) reloadLibraryViews();
  }
  on("purged", (m) => purgeDone(m.n || 0));
  on("purgemissing", (m) => purgeDone((m.purged != null ? m.purged : m.n) || 0));

  /* Tag inference: the scan-time toggle plus the "fix untagged now"
     backfill (worker on the C side; reply carries the updated count). */
  { const it = $("#set-infertags");
    if (it) it.addEventListener("change", () =>
      send({ cmd: "settings", action: "set", infer_tags: it.checked })); }
  { const wf = $("#set-watchfolders");
    if (wf) wf.addEventListener("change", () =>
      send({ cmd: "settings", action: "set", watch_folders: wf.checked })); }
  { const lp = $("#set-lowpower");
    if (lp) lp.addEventListener("change", () => {
      try { localStorage.setItem("mn.lowpower", lp.checked ? "1" : "0"); } catch (_) {}
      send({ cmd: "settings", action: "set", low_power: lp.checked });
      /* live effects: flat art, no anim, slower polling; compositor/AI
         thread caps apply on next launch (start-time options) */
      if (lp.checked && typeof setArt3D === "function") setArt3D(false);
      document.documentElement.classList.toggle("no-anim", lp.checked || !PREFS.anim);
      if (window.__mnRearmPoll) window.__mnRearmPoll();
      if (typeof window.__mnToast === "function") {
        window.__mnToast(lp.checked
          ? "Low power mode on — restart to also de-tune the compositor and AI threads"
          : "Low power mode off — restart to restore full compositor tuning");
      }
    }); }
  { const rb = $("#set-reinfer");
    if (rb) rb.addEventListener("click", () => {
      $("#reinfer-result").textContent = "…";
      send({ cmd: "reinfer" });
    }); }
  on("reinfer", (m) => {
    const n = m.updated || 0;
    $("#reinfer-result").textContent =
      n ? n.toLocaleString() + " fixed" : "nothing to fix";
    if (typeof window.__mnToast === "function") {
      window.__mnToast(n ? "Filled tags on " + n.toLocaleString() + " tracks"
                         : "No untagged tracks needed fixing");
    }
    if (n) reloadLibraryViews();
  });

  /* ============================================================
     SETTINGS → LIBRARY: SCAN-ROOTS LIST — one row per MANUALLY
     added folder (never the auto-discovered subfolders), with
     per-root stats from {"cmd":"roots"}: tracks · albums · size ·
     last scanned · newest addition. The visibility switch hides/
     shows the root's content everywhere (folderhidden across its
     subfolders); Remove deletes its tracks from the library.
     ============================================================ */
  (function initRootsList() {
    const host = $("#roots-list");
    if (!host) return;
    const addBtn = $("#lib-addroot");
    if (addBtn) addBtn.addEventListener("click", () => send({ cmd: "pickfolder" }));

    function fmtBytes(b) {
      if (!b || b <= 0) return "0 B";
      const u = ["B", "KB", "MB", "GB", "TB"];
      let i = 0, v = b;
      while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
      return (v >= 10 ? Math.round(v) : v.toFixed(1)) + " " + u[i];
    }
    function rel(sec) {
      if (!sec) return "never";
      const d = Date.now() / 1000 - sec;
      if (d < 90) return "just now";
      if (d < 3600) return Math.round(d / 60) + " min ago";
      if (d < 172800) return Math.round(d / 3600) + " h ago";
      return Math.round(d / 86400) + " days ago";
    }
    function underRoot(root, p) {
      const r = String(root).replace(/[\\\/]+$/, "").toLowerCase();
      const q = String(p || "").toLowerCase();
      return q === r || q.startsWith(r + "\\") || q.startsWith(r + "/");
    }
    function subFolders(root) {
      return (state.folders || []).filter((f) => underRoot(root, f.path));
    }
    function rootHidden(root) {
      const subs = subFolders(root);
      return subs.length > 0 && subs.every((f) => f.hidden);
    }

    on("roots", (m) => {
      host.innerHTML = "";
      const roots = m.roots || [];
      if (!roots.length) {
        host.appendChild(el("div", "set-hint", "No folders added yet — use “Add folder” below."));
        return;
      }
      roots.forEach((r) => {
        const row = el("div", "root-row");
        row.dataset.path = r.path;
        row.appendChild(el("span", "root-kind", esc(r.kind) || "music"));
        const info = el("div", "root-info");
        info.appendChild(el("div", "root-path", r.path));
        info.appendChild(el("div", "root-stats",
          (r.tracks || 0).toLocaleString() + " tracks · " +
          (r.albums || 0).toLocaleString() + " albums · " + fmtBytes(r.bytes) +
          "   ·   scanned " + rel(r.scanned) +
          (r.newest ? "   ·   newest add " + rel(r.newest) : "")));
        row.appendChild(info);
        const vis = el("label", "switch root-vis");
        vis.title = "Show / hide this folder's content everywhere (files stay indexed)";
        const chk = el("input");
        chk.type = "checkbox";
        chk.checked = !rootHidden(r.path);
        chk.addEventListener("change", () => {
          const hide = !chk.checked;
          const subs = subFolders(r.path);
          if (!subs.length) {
            /* folder data not loaded yet — acting now would be a silent
               no-op that still toasts; re-request and restore the switch */
            send({ cmd: "folders" });
            chk.checked = !chk.checked;
            return;
          }
          subs.forEach((f) => send({ cmd: "folderhidden", id: f.id, hidden: hide }));
          setTimeout(() => send({ cmd: "folders" }), 300);
          if (window.__mnToast) __mnToast(hide ? "Folder content hidden" : "Folder content visible");
          /* the whole browse surface changes */
          state.albDirty = true;
          if (state.view === 0) { resetTracks(); loadTracks(); }
        });
        vis.appendChild(chk);
        vis.appendChild(el("span", "track"));
        row.appendChild(vis);
        const rm = el("button", "btn btn-ghost root-rm", "Remove");
        rm.addEventListener("click", () => {
          if (!confirm('Remove "' + r.path + '" and all its tracks from the library?\n(Files on disk are not touched.)')) return;
          subFolders(r.path).forEach((f) => send({ cmd: "removefolder", id: f.id }));
          send({ cmd: "removeroot", path: r.path });   /* replies with the fresh list */
          if (window.__mnToast) __mnToast("Removing folder from library…");
          setTimeout(() => {
            send({ cmd: "folders" });
            state.albDirty = true;
            if (state.view === 0) { resetTracks(); loadTracks(); }
          }, 600);
        });
        row.appendChild(rm);
        host.appendChild(row);
      });
    });

    /* keep the visibility switches truthful when folder data refreshes
       (tap — the folders view owns on("folders")) */
    tap("folders", () => {
      $$(".root-row", host).forEach((row) => {
        const chk = row.querySelector("input[type=checkbox]");
        if (chk) chk.checked = !rootHidden(row.dataset.path);
      });
    });
  })();

  /* ---------- new wired preferences: startup view / toasts / auto-rescan --- */
  (function initExtraPrefs() {
    const sv = $("#set-startview");
    if (sv) {
      sv.value = localStorage.getItem("mn.startview") || "last";
      sv.addEventListener("change", () => {
        try { localStorage.setItem("mn.startview", sv.value); } catch (_) {}
      });
    }
    const tg = $("#set-toasts");
    if (tg) {
      tg.checked = localStorage.getItem("mn.toasts") !== "0";
      tg.addEventListener("change", () => {
        try { localStorage.setItem("mn.toasts", tg.checked ? "1" : "0"); } catch (_) {}
      });
      /* gate the global toast once */
      const orig = window.__mnToast;
      window.__mnToast = function (msg) {
        if (localStorage.getItem("mn.toasts") === "0") return;
        orig(msg);
      };
    }
    const ar = $("#set-autorescan");
    if (ar) {
      ar.checked = localStorage.getItem("mn.autorescan") === "1";
      ar.addEventListener("change", () => {
        try { localStorage.setItem("mn.autorescan", ar.checked ? "1" : "0"); } catch (_) {}
      });
    }
  })();

  /* ---------- Library maintenance tools (Settings → Library) ---------- */
  { const rs = $("#lib-rescan");
    if (rs) rs.addEventListener("click", () => {
      send({ cmd: "rescan" });
      send({ cmd: "scan" });
      state.scanActive = true;   /* poll() keeps the scan card fed */
      if (typeof window.__mnToast === "function") window.__mnToast("Rescanning library folders…");
    });
  }
  { const ip = $("#lib-importpl");
    if (ip) ip.addEventListener("click", () => {
      send({ cmd: "importplaylists" });
      if (typeof window.__mnToast === "function") window.__mnToast("Importing playlists from library folders…");
    });
  }
  { const bk = $("#lib-backup");
    if (bk) bk.addEventListener("click", () => {
      bk.disabled = true;
      send({ cmd: "backupnow" });
      if (typeof window.__mnToast === "function") window.__mnToast("Backing up library database…");
      setTimeout(() => { bk.disabled = false; }, 8000);   /* re-arm if no reply lands */
    });
  }
  on("backup", (m) => {
    const bk = $("#lib-backup");
    if (bk) bk.disabled = false;
    if (typeof window.__mnToast === "function") {
      window.__mnToast(m && m.ok
        ? "Database backed up — 3 rotated copies kept beside the library db"
        : "Database backup failed");
    }
  });

  /* Refresh album art: force-regenerate every missing cover (embedded art or
     folder sidecar), reporting progress from the C-side {type:"artscan"} +
     terminal {type:"artdone"} messages. */
  const refreshArtBtn = $("#set-refreshart");
  if (refreshArtBtn) {
    refreshArtBtn.addEventListener("click", () => {
      refreshArtBtn.disabled = true;
      $("#artscan-result").textContent = "starting…";
      send({ cmd: "refreshart" });
    });
  }
  /* tap(), NOT on() — the activity strip owns on("artscan")/on("artdone")
     (single-handler bus); a second on() here silently replaced those handlers
     and killed the art-scan progress pill. These are secondary observers. */
  tap("artscan", (m) => {
    const done = m.done || 0, total = m.total || 0;
    $("#artscan-result").textContent =
      total ? done.toLocaleString() + " / " + total.toLocaleString() : "…";
  });
  tap("artdone", (m) => {
    const g = m.gained || 0;
    $("#artscan-result").textContent =
      g ? g.toLocaleString() + " covers added" : "up to date";
    if (refreshArtBtn) refreshArtBtn.disabled = false;
    reloadLibraryViews();   /* re-request albums so new covers load */
  });

  /* ------------------------------------------------------------
     ARTREADY — the one-store architecture's targeted repaint.
     The C extractor batches freshly-landed thumbs into
     {"type":"artready","items":[{aa,album,url},…]} (coalesced ~250ms).
     We patch the cached rows (so future re-binds/scrolls serve the URL
     without a refetch) and re-run setArt on EXACTLY the visible tiles
     whose data-art-key matches — Winamp's per-item redraw analog: no
     global re-render, no timers, no rAF loops (VG/paint invariants
     stay intact). tap(), NOT on() — single-handler bus.
     ------------------------------------------------------------ */
  tap("artready", (m) => {
    const items = m.items || [];
    if (!items.length) return;
    const byKey = new Map();
    for (const it of items) {
      const url = it.url || "";
      if (!url) continue;
      byKey.set(artKeyOf(it.aa, it.album), url);
      artDead.delete(url);           /* event-invalidated failure ledger */
    }
    if (!byKey.size) return;
    /* patch cached rows so re-binds keep the cover without a refetch —
       including rows that ALREADY had art: an artready after artfetch/ingest
       carries a fresh ?g= generation for the same path, and skipping those
       rows kept every non-refetched surface serving stale pixels (CEF caches
       file:// by full URL). */
    for (const a of state.albCards) {
      if (a) {
        const u = byKey.get(artKeyOf(a.artist, a.title));
        if (u && a.art !== u) a.art = u;
      }
    }
    for (const r of state.trkRows) {
      if (r) {
        const u = byKey.get(artKeyOf(r.album_artist || r.artist, r.album));
        if (u && r.art !== u) r.art = u;
      }
    }
    try {
      for (const q of qItems) {
        if (q) {
          const u = byKey.get(artKeyOf(q.album_artist || q.artist, q.album));
          if (u && q.art !== u) q.art = u;
        }
      }
    } catch (_) {}                   /* queue list not built yet */
    /* Repaint exactly the visible tiles whose key matches: PENDING
       placeholders swap in their cover, CACHED tiles swap generations.
       Clearing the artUrl memo forces the repaint even when the healed URL
       string equals the one whose load transiently failed. */
    document.querySelectorAll("[data-art-key]").forEach((c) => {
      const u = byKey.get(c.dataset.artKey);
      if (!u) return;
      const stale = (c.dataset.artUrl || "") !== u;
      const ph = c.classList.contains("art-ph");
      if (!stale && !ph) return;     /* already showing this exact cover */
      delete c.dataset.artUrl;
      setArt(c, u, c.dataset.artGlyph || "", c.dataset.artKey);
    });
  });

  /* Reset library: inline confirm (danger) with an "also clear art" option. */
  $("#set-resetlib").addEventListener("click", () => {
    const rc = $("#reset-confirm");
    rc.hidden = !rc.hidden;
  });
  $("#reset-cancel").addEventListener("click", () => { $("#reset-confirm").hidden = true; });
  $("#reset-go").addEventListener("click", () => {
    $("#reset-confirm").hidden = true;
    $("#reset-go").disabled = true;
    send({ cmd: "resetlibrary", art: $("#reset-art").checked });
  });
  on("resetdone", () => {
    state.albDirty = true;
    $("#reset-go").disabled = false;
    setOv.hidden = true;          /* close settings so the scan card shows */
    state.scanActive = true;      /* poll() keeps the scan card fed */
    send({ cmd: "scan" });
    reloadLibraryViews();
  });

  /* ============================================================
     STEM DOCK (always visible; collapsible to a slim bar)
     ============================================================ */
  const faderEls = [];
  /* channel-family tint for the name dot (see .fader[data-group=…] in CSS).
     Channel order: 0 Sub, 1 Bass, 2 Vocals, 3 Lead, 4 Instruments, 5 Wide,
     6 Air, 7 Guitar, 8 Piano. */
  const STEM_GROUPS = ["low", "low", "voice", "voice", "body", "space", "space", "body", "body"];
  let stemMaster = 1;   /* client-side master scale over all 9 channel gains */
  /* Throttle gain sends to ~30/s per channel with a guaranteed trailing send
     (the C side atomically publishes the last value, so only rate matters). */
  function mkGainThrottle(fn) {
    let last = 0, t = 0, pend = null;
    return (v) => {
      pend = v;
      const now = performance.now();
      if (now - last >= 33) { last = now; fn(pend); pend = null; }
      else if (!t) {
        t = setTimeout(() => {
          t = 0;
          if (pend != null) { last = performance.now(); fn(pend); pend = null; }
        }, 34);
      }
    };
  }
  /* effective gain the engine hears = channel gain × master (C caps at 1.0) */
  function pushGain(i) {
    const rec = faderEls[i];
    if (rec) rec.sendG(clamp(rec.gain * stemMaster, 0, 1));
  }
  function setFaderGain(rec, i, pctVal, sendIt) {
    const p = clamp(Math.round(pctVal), 0, 100);
    rec.gain = p / 100;
    rec.track.style.setProperty("--p", (p / 100).toFixed(3));
    rec.track.setAttribute("aria-valuenow", String(p));
    rec.pct.textContent = p + "%";
    if (sendIt !== false) pushGain(i);
  }
  function buildFaders() {
    E.stemFaders.innerHTML = ""; faderEls.length = 0;
    for (let i = 0; i < 9; i++) {
      const root = el("div", "fader");
      root.dataset.group = STEM_GROUPS[i];
      const name = el("div", "fader-name");
      name.appendChild(el("span", "fader-dot"));
      name.appendChild(el("span", "", STEM_NAMES[i]));
      name.title = STEM_NAMES[i];
      root.appendChild(name);

      /* vertical channel strip: readable meter + a REAL pointer-driven
         vertical fader (track + thumb) — no rotated <input type=range>. */
      const body = el("div", "fader-body");
      const meter = el("div", "meter");
      const mfill = el("div", "meter-fill");
      const mpeak = el("div", "meter-peak");
      meter.appendChild(mfill); meter.appendChild(mpeak);
      const track = el("div", "vfader");
      track.tabIndex = 0;
      track.setAttribute("role", "slider");
      track.setAttribute("aria-orientation", "vertical");
      track.setAttribute("aria-label", STEM_NAMES[i] + " level");
      track.setAttribute("aria-valuemin", "0");
      track.setAttribute("aria-valuemax", "100");
      track.title = STEM_NAMES[i] + " — drag or scroll; arrows when focused; double-click resets";
      track.appendChild(el("div", "vfader-fill"));
      /* thumb rides a compositor-translated rail (see .vfader-thumbrail) */
      const thumbRail = el("div", "vfader-thumbrail");
      thumbRail.appendChild(el("div", "vfader-thumb"));
      track.appendChild(thumbRail);
      body.appendChild(meter);
      body.appendChild(track);
      root.appendChild(body);

      const foot = el("div", "fader-foot");
      const pct = el("div", "fader-pct", "100%");
      pct.title = "Channel level — double-click to reset to 100%";
      const btns = el("div", "fader-btns");
      const mute = el("button", "fader-mute", "M");
      mute.title = "Mute " + STEM_NAMES[i];
      mute.setAttribute("aria-label", "Mute " + STEM_NAMES[i]);
      mute.setAttribute("aria-pressed", "false");
      const solo = el("button", "fader-solo", "S");
      solo.title = "Solo " + STEM_NAMES[i];
      solo.setAttribute("aria-label", "Solo " + STEM_NAMES[i]);
      solo.setAttribute("aria-pressed", "false");
      btns.appendChild(mute); btns.appendChild(solo);
      foot.appendChild(pct); foot.appendChild(btns);
      root.appendChild(foot);

      const rec = { root, mfill, mpeak, track, pct, mute, solo, gain: 1, on: true,
        soloed: false, meterVal: 0, meterDisp: 0, peakDisp: 0,
        sendG: mkGainThrottle((v) => send({ cmd: "stems", action: "gain", i, v })) };

      const valFromEvent = (ev) => {
        const r = track.getBoundingClientRect();
        return (1 - clamp((ev.clientY - r.top) / Math.max(1, r.height), 0, 1)) * 100;
      };
      /* bare-click engine sends are DEFERRED ~250ms so the FIRST press of a
         double-click reset never audibly passes through the clicked value;
         drags commit live from the first pointermove. */
      let dragMoved = false, clickSendT = 0;
      track.addEventListener("pointerdown", (ev) => {
        if (ev.button !== 0) return;
        ev.stopPropagation();               /* keep MnRearrange off the track */
        if (ev.detail > 1) return;          /* 2nd click of a dblclick: let the
                                               reset fire without a gain jump */
        if (clickSendT) { clearTimeout(clickSendT); clickSendT = 0; }
        track.setPointerCapture(ev.pointerId);
        track.classList.add("dragging");
        dragMoved = false;
        setFaderGain(rec, i, valFromEvent(ev), false);   /* visual only */
      });
      track.addEventListener("pointermove", (ev) => {
        if (!track.hasPointerCapture(ev.pointerId)) return;
        dragMoved = true;
        setFaderGain(rec, i, valFromEvent(ev));
      });
      track.addEventListener("pointerup", (ev) => {
        if (!track.hasPointerCapture(ev.pointerId)) return;
        track.releasePointerCapture(ev.pointerId);
        track.classList.remove("dragging");
        if (dragMoved) { pushGain(i); return; }  /* authoritative trailing send */
        /* bare click: hold the send briefly — a dblclick reset cancels it so
           the engine only ever hears the final value */
        clickSendT = setTimeout(() => { clickSendT = 0; pushGain(i); }, 250);
      });
      /* pointercancel / capture loss skips pointerup entirely — drop the
         scaled-thumb state here (also fires on normal release; harmless) */
      track.addEventListener("lostpointercapture", () => track.classList.remove("dragging"));
      track.addEventListener("dblclick", () => {
        if (clickSendT) { clearTimeout(clickSendT); clickSendT = 0; }
        setFaderGain(rec, i, 100);
      });
      pct.addEventListener("dblclick", () => setFaderGain(rec, i, 100));
      track.addEventListener("wheel", (ev) => {
        ev.preventDefault();
        setFaderGain(rec, i, rec.gain * 100 + (ev.deltaY < 0 ? 3 : -3));
      }, { passive: false });
      track.addEventListener("keydown", (ev) => {
        /* pointer-events:none on the dimmed overlay doesn't clear focus — a
           focused fader must go inert with the dock, like the master */
        if (stemDockState !== "live") return;
        const cur = rec.gain * 100;
        const k = ev.key;
        let v = null;
        if (k === "ArrowUp" || k === "ArrowRight") v = cur + (ev.shiftKey ? 10 : 2);
        else if (k === "ArrowDown" || k === "ArrowLeft") v = cur - (ev.shiftKey ? 10 : 2);
        else if (k === "PageUp") v = cur + 10;
        else if (k === "PageDown") v = cur - 10;
        else if (k === "Home") v = 0;
        else if (k === "End") v = 100;
        else if (k === "m" || k === "M") { ev.preventDefault(); mute.click(); return; }
        else if (k === "s" || k === "S") { ev.preventDefault(); solo.click(); return; }
        if (v == null) return;
        ev.preventDefault();
        setFaderGain(rec, i, v);
      });

      mute.addEventListener("click", () => {
        if (stemDockState !== "live") return;   /* inert with the dock overlay */
        rec.on = !rec.on;
        root.classList.toggle("muted", !rec.on);
        mute.classList.toggle("active", !rec.on);
        mute.setAttribute("aria-pressed", String(!rec.on));
        send({ cmd: "stems", action: "mute", i, on: !rec.on });
        syncStemPresetChips();
      });
      solo.addEventListener("click", () => {
        if (stemDockState !== "live") return;   /* inert with the dock overlay */
        rec.soloed = !rec.soloed;
        solo.classList.toggle("active", rec.soloed);
        solo.setAttribute("aria-pressed", String(rec.soloed));
        send({ cmd: "stems", action: "solo", i, on: rec.soloed });
        syncStemPresetChips();
      });
      faderEls.push(rec);
      E.stemFaders.appendChild(root);
      setFaderGain(rec, i, 100, false);   /* engine default gain is 1.0 */
    }
    buildStemPresets();
    syncStemPresetChips();
  }

  /* ---- master mini-fader (header): scales every channel's sent gain ---- */
  const stemMasterEls = {
    root: $("#stem-master"),
    track: $("#stem-master-track"), fill: $("#stem-master-fill"),
    val: $("#stem-master-val"),
  };
  /* while the dock overlay shows (off / loading / unavailable) the engine
     drops gain sends on the floor — the master must be inert too, or its
     value silently desyncs from what the engine will actually play */
  const stemMasterInert = () =>
    !!(stemMasterEls.root && stemMasterEls.root.classList.contains("inert"));
  /* a master drag fans out to all nine channels — throttle the fan-out as
     ONE logical control (~30/s, one shared timer, guaranteed trailing send)
     instead of letting nine per-channel throttles multiply bridge traffic */
  const pushAllGains = mkGainThrottle(() => {
    for (let i = 0; i < faderEls.length; i++) pushGain(i);
  });
  function setStemMaster(pctVal, sendIt) {
    const p = clamp(Math.round(pctVal), 0, 100);
    stemMaster = p / 100;
    const m = stemMasterEls;
    if (m.fill) m.fill.style.transform = "scaleX(" + (p / 100).toFixed(3) + ")";
    if (m.val) m.val.textContent = String(p);
    if (m.track) {
      /* knob rail reads --p and moves via transform (compositor-only) */
      m.track.style.setProperty("--p", (p / 100).toFixed(3));
      m.track.setAttribute("aria-valuenow", String(p));
    }
    if (sendIt !== false) pushAllGains(p);
  }
  if (stemMasterEls.track) {
    const mt = stemMasterEls.track;
    let mRect = null;   /* track rect cached at pointerdown — no layout read per move */
    const mv = (ev) => {
      const r = mRect || (mRect = mt.getBoundingClientRect());
      return clamp((ev.clientX - r.left) / Math.max(1, r.width), 0, 1) * 100;
    };
    /* like the channel faders: a bare click's engine send is deferred so the
       first press of a double-click reset never audibly passes through */
    let mDragMoved = false, mClickSendT = 0;
    mt.addEventListener("pointerdown", (ev) => {
      if (ev.button !== 0 || stemMasterInert()) return;
      if (ev.detail > 1) return;   /* 2nd click of a dblclick: reset only */
      if (mClickSendT) { clearTimeout(mClickSendT); mClickSendT = 0; }
      mt.setPointerCapture(ev.pointerId);
      mDragMoved = false;
      mRect = mt.getBoundingClientRect();    /* one read; reused all drag */
      setStemMaster(mv(ev), false);          /* visual only until move/up */
    });
    mt.addEventListener("pointermove", (ev) => {
      if (!mt.hasPointerCapture(ev.pointerId)) return;
      mDragMoved = true;
      setStemMaster(mv(ev));
    });
    mt.addEventListener("pointerup", (ev) => {
      if (!mt.hasPointerCapture(ev.pointerId)) return;
      mt.releasePointerCapture(ev.pointerId);
      if (mDragMoved) { setStemMaster(stemMaster * 100); return; }
      mClickSendT = setTimeout(() => { mClickSendT = 0; setStemMaster(stemMaster * 100); }, 250);
    });
    /* capture loss (incl. pointercancel) may skip pointerup — drop the cache */
    mt.addEventListener("lostpointercapture", () => { mRect = null; });
    mt.addEventListener("dblclick", () => {
      if (mClickSendT) { clearTimeout(mClickSendT); mClickSendT = 0; }
      if (!stemMasterInert()) setStemMaster(100);
    });
    mt.addEventListener("wheel", (ev) => {
      ev.preventDefault();
      if (stemMasterInert()) return;
      setStemMaster(stemMaster * 100 + (ev.deltaY < 0 ? 3 : -3));
    }, { passive: false });
    mt.addEventListener("keydown", (ev) => {
      if (stemMasterInert()) return;
      const cur = stemMaster * 100;
      let v = null;
      if (ev.key === "ArrowUp" || ev.key === "ArrowRight") v = cur + (ev.shiftKey ? 10 : 2);
      else if (ev.key === "ArrowDown" || ev.key === "ArrowLeft") v = cur - (ev.shiftKey ? 10 : 2);
      else if (ev.key === "Home") v = 0;
      else if (ev.key === "End") v = 100;
      if (v == null) return;
      ev.preventDefault();
      setStemMaster(v);
    });
  }

  /* One-tap stem macros (like the Android preset chip row). A preset defines
     the MUTE MASK only — the set of channels that stay AUDIBLE — and clears
     any solos; channel gains are left as you set them (deterministic and
     non-destructive). NOTE: this model has no discrete drum stem, so the
     percussion presets act on the Air band and are labeled as approximations.
     Channel order: 0 Sub, 1 Bass, 2 Vocals, 3 Lead, 4 Instruments, 5 Wide,
     6 Air, 7 Guitar, 8 Piano. */
  const STEM_PRESETS = {
    "Full mix":         { keep: [0, 1, 2, 3, 4, 5, 6, 7, 8], tip: "Unmute all nine channels" },
    "Karaoke":          { keep: [0, 1, 3, 4, 5, 6, 7, 8],    tip: "Mute the Vocals channel" },
    "A cappella":       { keep: [2],                          tip: "Vocals only" },
    "Percussion (Air)": { keep: [6],                          tip: "Air band only — closest to cymbals/drums; this model has no discrete drum stem" },
    "Bass only":        { keep: [0, 1],                       tip: "Sub Bass + Bass only" },
    "No percussion":    { keep: [0, 1, 2, 3, 4, 5, 7, 8],     tip: "Mute the Air band — approximates removing cymbals/drums" },
  };
  function applyStemPreset(name) {
    if (stemDockState !== "live") return;   /* inert with the dock overlay */
    const p = STEM_PRESETS[name];
    if (!p) return;
    const set = new Set(p.keep);
    faderEls.forEach((rec, i) => {
      const shouldBeOn = set.has(i);
      if (rec.on !== shouldBeOn) {
        rec.on = shouldBeOn;
        rec.root.classList.toggle("muted", !rec.on);
        rec.mute.classList.toggle("active", !rec.on);
        rec.mute.setAttribute("aria-pressed", String(!rec.on));
        send({ cmd: "stems", action: "mute", i, on: !rec.on });
      }
      /* clear any solos so the preset's mute mask is what plays */
      if (rec.soloed) {
        rec.soloed = false;
        rec.solo.classList.remove("active");
        rec.solo.setAttribute("aria-pressed", "false");
        send({ cmd: "stems", action: "solo", i, on: false });
      }
    });
    syncStemPresetChips();
  }
  /* Everything back to a clean slate: gains 100%, master 100%, no mutes, no
     solos. (Presets deliberately don't touch gains; this does.) */
  function resetStemMixer() {
    if (stemDockState !== "live") return;   /* inert with the dock overlay */
    setStemMaster(100, false);
    faderEls.forEach((rec, i) => {
      setFaderGain(rec, i, 100);
      if (!rec.on) {
        rec.on = true;
        rec.root.classList.remove("muted");
        rec.mute.classList.remove("active");
        rec.mute.setAttribute("aria-pressed", "false");
        send({ cmd: "stems", action: "mute", i, on: false });
      }
      if (rec.soloed) {
        rec.soloed = false;
        rec.solo.classList.remove("active");
        rec.solo.setAttribute("aria-pressed", "false");
        send({ cmd: "stems", action: "solo", i, on: false });
      }
    });
    syncStemPresetChips();
  }
  /* LIVE preset detection: the highlighted chip is DERIVED from the current
     audible set (solo overrides mute, matching engine routing) after every
     change, so the highlight can never go stale; no match = no highlight. */
  function syncStemPresetChips() {
    const row = $("#stem-presets");
    if (!row) return;
    const anySolo = faderEls.some((r) => r.soloed);
    const audible = [];
    faderEls.forEach((r, i) => { if (anySolo ? r.soloed : r.on) audible.push(i); });
    const key = audible.join(",");
    let match = "";
    for (const name of Object.keys(STEM_PRESETS)) {
      if (STEM_PRESETS[name].keep.join(",") === key) { match = name; break; }
    }
    row.querySelectorAll(".stem-preset-chip").forEach((c) => {
      c.classList.toggle("applied", !!match && c.dataset.preset === match);
    });
  }
  function buildStemPresets() {
    let row = $("#stem-presets");
    if (!row) {
      row = el("div", "stem-presets"); row.id = "stem-presets";
      if (E.stemFaders && E.stemFaders.parentNode) {
        E.stemFaders.parentNode.insertBefore(row, E.stemFaders);
      }
    }
    row.innerHTML = "";
    Object.keys(STEM_PRESETS).forEach((name) => {
      const chip = el("button", "stem-preset-chip", name);
      chip.dataset.preset = name;
      chip.title = STEM_PRESETS[name].tip;
      chip.addEventListener("click", () => applyStemPreset(name));
      row.appendChild(chip);
    });
    const rst = el("button", "stem-preset-chip stem-reset-chip", "Reset all");
    rst.title = "All channels to 100%, unmuted, no solos; master back to 100%";
    rst.addEventListener("click", resetStemMixer);
    row.appendChild(rst);
  }

  /* Latching instrument detection (like the Android app, DETECT_THRESHOLD~0.07):
     a stem that ever shows real energy in the current track is "present"; stems
     that stay silent are dimmed as "no signal" so the mixer only presents the
     instruments actually in the track. Reset on track change. */
  let stemGraceFrames = 0;      /* PLAYED meter frames seen this track   */
  let stemEnergySeen = false;   /* any channel over threshold this track */
  function resetStemDetection() {
    stemGraceFrames = 0; stemEnergySeen = false;
    for (const f of faderEls) {
      f.detected = false; f.root.classList.remove("present", "absent");
      /* drop the previous track's peak-hold cue too */
      f.peakDisp = 0; f._lastPk = 100;
      f.mpeak.style.transform = "translateY(100%)";
    }
  }
  function updateStemMeters(meters) {
    const active = state.now && state.now.stems_enabled && !state.now.stems_passthrough && state.now.neural_active;
    /* pause/stop halts the miniaudio device, so the C-side ballistics freeze
       at their last values — target zero instead so the meters decay via
       smoothMeters and the shared rAF loop can park (idle-CPU rule). */
    const playing = !!(state.now && state.now.playing);
    /* "absent" only after playback energy has actually been sampled: either
       some channel already crossed the threshold, or ~1s of real play went by
       with everything silent. Prevents all nine strips flashing "no signal"
       at track start, and never dims while paused at the top of a track. */
    if (active && playing) {
      stemGraceFrames++;
      if (!stemEnergySeen) {
        for (let i = 0; i < 9; i++) if ((meters[i] || 0) > 0.07) { stemEnergySeen = true; break; }
      }
    }
    const judge = active && playing && (stemEnergySeen || stemGraceFrames >= 5);
    for (let i = 0; i < faderEls.length; i++) {
      const f = faderEls[i];
      const v = playing ? clamp((meters[i] || 0), 0, 1) : 0;
      f.meterVal = v * 100;
      if (active) {
        if (!f.detected && v > 0.07) { f.detected = true; f.root.classList.add("present"); f.root.classList.remove("absent"); }
        else if (!f.detected && judge) { f.root.classList.add("absent"); }
      } else {
        f.root.classList.remove("present", "absent");   /* no neural = show all normally */
      }
    }
  }
  let smLastTs = 0;
  function smoothMeters(ts) {
    /* frame-delta so peak decay is refresh-rate independent (0.3/s = the old
       0.005/frame at 60 Hz); clamp covers rAF parking/resume gaps */
    const dt = smLastTs ? Math.min(0.1, Math.max(0, (ts - smLastTs) / 1000)) : 1 / 60;
    smLastTs = ts;
    for (const f of faderEls) {
      const t = f.meterVal;
      f.meterDisp += (t - f.meterDisp) * (t > f.meterDisp ? 0.5 : 0.12);
      const s = Math.max(0, Math.min(1, f.meterDisp / 100));
      /* transform (compositor-only, no layout) instead of height %; skip
         near-identical frames to avoid needless style writes */
      if (Math.abs(s - (f._lastS || -1)) > 0.004) {
        f._lastS = s;
        f.mfill.style.transform = "scaleY(" + s.toFixed(3) + ")";
      }
      /* peak-hold cue: jumps up with the signal, decays slowly. The line is
         the top edge of a full-height wrapper translated on the compositor;
         writes stop entirely once the peak settles (idle-safe). */
      f.peakDisp = Math.max(s, f.peakDisp - 0.3 * dt);
      const py = (1 - f.peakDisp) * 100;
      if (Math.abs(py - (f._lastPk == null ? -1 : f._lastPk)) > 0.45) {
        f._lastPk = py;
        f.mpeak.style.transform = "translateY(" + py.toFixed(1) + "%)";
      }
    }
  }

  /* Dock state machine driven by the C-emitted now fields (stems_loading /
     stems_available were previously unused). Memoized so the 4 Hz poll costs
     zero DOM work unless the state actually flips. */
  const stemOv = {
    root: $("#stem-overlay"), spin: $("#stem-ov-spin"),
    title: $("#stem-ov-title"), sub: $("#stem-ov-sub"), btn: $("#stem-ov-btn"),
    body: $("#stem-body"),
  };
  if (stemOv.btn) stemOv.btn.addEventListener("click", () => send({ cmd: "stems", action: "enable", on: true }));
  let stemDockState = "";
  /* Push the dock's complete mixer state to the engine. Needed whenever the
     dock transitions into "live": while stems were off/loading, C's
     mn_app_stem_* calls no-op (app->stems==NULL), so anything the UI shows
     (gains × master, mutes, solos) must be re-sent or engine and dock desync. */
  function resendStemMixer() {
    for (let i = 0; i < faderEls.length; i++) {
      const rec = faderEls[i];
      pushGain(i);
      if (!rec.on) send({ cmd: "stems", action: "mute", i, on: true });
      if (rec.soloed) send({ cmd: "stems", action: "solo", i, on: true });
    }
  }
  function updateStemDockState(m) {
    const s = !m.stems_enabled ? "off"
      : m.stems_loading ? "loading"
      : !m.stems_available ? "none"
      : "live";
    if (s === stemDockState || !stemOv.root) return;
    stemDockState = s;
    const showOv = s !== "live";
    stemOv.root.hidden = !showOv;
    if (stemOv.body) stemOv.body.classList.toggle("inert", showOv);
    if (stemMasterEls.root) stemMasterEls.root.classList.toggle("inert", showOv);
    if (!showOv) { resendStemMixer(); return; }
    if (s === "off") {
      stemOv.spin.hidden = true; stemOv.btn.hidden = false;
      stemOv.title.textContent = "AI Stems are off";
      stemOv.sub.textContent = "Enable to split playback into nine live channels you can mix, mute and solo.";
    } else if (s === "loading") {
      stemOv.spin.hidden = false; stemOv.btn.hidden = true;
      stemOv.title.textContent = "Loading neural model…";
      stemOv.sub.textContent = "First enable maps the ~136 MB stem model — the mixer goes live in a moment.";
    } else { /* enabled, not loading, engine still unavailable */
      stemOv.spin.hidden = true; stemOv.btn.hidden = true;
      stemOv.title.textContent = "Stems unavailable";
      stemOv.sub.textContent = "The stem engine could not start — the original audio keeps playing untouched.";
    }
  }

  function updateStemStatus(m) {
    const active = !!m.neural_active;
    const loading = !!(m.stems_enabled && m.stems_loading);
    let txt;
    if (!m.stems_enabled) txt = "idle";
    else if (loading) txt = "loading model…";
    else if (m.stems_passthrough) txt = "passthrough";
    else {
      const prov = m.stem_provider || "neural";
      const rt = m.stem_rt_factor != null ? m.stem_rt_factor.toFixed(2) + "× rt" : "";
      const frac = m.stem_fraction != null ? Math.round(m.stem_fraction * 100) + "%" : "";
      txt = [prov, rt, frac].filter(Boolean).join("  ·  ");
    }
    if (E.stemStatus.textContent !== txt) E.stemStatus.textContent = txt;
    E.stemStatus.classList.toggle("live", active && m.stems_enabled && !m.stems_passthrough);
    E.stemStatus.classList.toggle("loading", loading);
    updateStemDockState(m);
    /* general-purpose activity strip: show live stem separation progress
       (priority 4 — it's the foreground thing the user just asked for).
       Model load is surfaced on the dock itself, not here. */
    if (m.stems_enabled && !loading && !m.stems_passthrough && m.stem_fraction != null &&
        m.stem_fraction < 0.999) {
      activitySet("stemsep", "Separating stems", m.stem_fraction, 4);
    } else {
      activityClear("stemsep");
    }
  }

  /* Stems button = master enable toggle; dock is always on screen. */
  E.btnStems.addEventListener("click", () => {
    const on = !(state.now && state.now.stems_enabled);
    send({ cmd: "stems", action: "enable", on });
  });
  function syncStemCollapseBtn(collapsed) {
    const b = $("#stem-collapse");
    b.textContent = collapsed ? "▴" : "▾";
    b.title = collapsed ? "Expand stem dock" : "Collapse stem dock";
    b.setAttribute("aria-label", b.title);
    b.setAttribute("aria-expanded", String(!collapsed));
  }
  $("#stem-collapse").addEventListener("click", () => {
    const collapsed = E.stemDock.classList.toggle("collapsed");
    syncStemCollapseBtn(collapsed);
    localStorage.setItem("mn.stemdock", collapsed ? "min" : "open");
  });
  if (localStorage.getItem("mn.stemdock") === "min") {
    E.stemDock.classList.add("collapsed");
    syncStemCollapseBtn(true);
  }
  E.stemEnable.addEventListener("change", () => send({ cmd: "stems", action: "enable", on: E.stemEnable.checked }));
  E.stemPass.addEventListener("change", () => send({ cmd: "stems", action: "passthrough", on: E.stemPass.checked }));

  /* ============================================================
     STEM EXPORT — one-click transport button + batch (context menu),
     progress toast. Defaults persist in mn.stemexport; the C worker
     separates each track offline and writes a .mnstem container (or
     loose files) via cmd:"stemexport".
     ============================================================ */
  const SX_DEFAULTS = (() => {
    try { return JSON.parse(localStorage.getItem("mn.stemexport") || "{}"); } catch (_) { return {}; }
  })();
  const sxOpt = Object.assign({ set: "individual", fmt: "flac", container: true }, SX_DEFAULTS);
  function sxSave() { try { localStorage.setItem("mn.stemexport", JSON.stringify(sxOpt)); } catch (_) {} }
  /* build the tracks[] payload entry the C worker needs */
  function sxTrackFromNow() {
    const n = state.now;
    if (!n || !(n.track_id > 0) || !n.track_path) return null;
    return { id: n.track_id, path: n.track_path, artist: n.track_artist || n.artist || "",
      album: n.track_album || n.album || "", title: n.track_title || n.title || "",
      track_no: n.track_no || 0, year: n.year || 0, art: n.art || "" };
  }
  function sxTrackFromRow(r) {
    if (!r || !r.path) return null;
    return { id: r.id, path: r.path, artist: r.artist || "", album: r.album || "",
      title: r.title || "", track_no: r.track_no || 0, year: r.year || 0, art: r.art || "" };
  }
  function runStemExport(tracks, label) {
    const t = (tracks || []).filter(Boolean);
    if (!t.length) { window.__mnToast("Nothing to export"); return; }
    window.__mnToast("Exporting stems" + (label ? " — " + label : "") + "…");
    send({ cmd: "stemexport", tracks: t, set: sxOpt.set, fmt: sxOpt.fmt, container: !!sxOpt.container });
  }
  window.__mnStemExport = { run: runStemExport, fromRow: sxTrackFromRow, opt: sxOpt, save: sxSave };

  /* one-click transport button: export the CURRENT track with saved defaults */
  { const b = $("#btn-stemexport");
    if (b) b.addEventListener("click", () => {
      const tk = sxTrackFromNow();
      if (!tk) { window.__mnToast("Play a track first to export its stems"); return; }
      runStemExport([tk], tk.title);
    });
  }
  /* progress toast — repeated {index,total,pct,done,error,file} */
  on("stemexport", (m) => {
    if (m.error && m.error.length) { window.__mnToast("Stem export: " + m.error); return; }
    if (m.done) {
      window.__mnToast("✓ Stems exported (" + m.file + ")");
      return;
    }
    if (m.total > 1) window.__mnToast("Exporting stems… " + (m.pct | 0) + "%  (" + (m.index + 1) + "/" + m.total + ")");
  });

  /* opt-in: auto-collapse the dock after 30s without interaction while
     stems are not actively processing (Settings → Interface) */
  let dockIdleTimer = 0;
  /* Idle auto-collapse of the stem dock. Was re-armed on every pointermove
     over the dock (a clearTimeout+setTimeout allocation per move event).
     Now driven by pointerENTER/LEAVE + a "last activity" timestamp the
     single interval checks — no per-move churn. */
  let dockActive = false, dockLastTs = performance.now();
  function armDockIdle() {
    clearInterval(dockIdleTimer);
    if (!PREFS.dockidle) return;
    dockIdleTimer = setInterval(() => {
      if (dockActive) { dockLastTs = performance.now(); return; }
      if (performance.now() - dockLastTs < 30000) return;
      const busy = state.now && state.now.stems_enabled && !state.now.stems_passthrough;
      if (!busy && !E.stemDock.classList.contains("collapsed")) {
        E.stemDock.classList.add("collapsed");
        syncStemCollapseBtn(true);   /* keep title/aria in step, like manual */
      }
    }, 5000);
  }
  E.stemDock.addEventListener("pointerenter", () => { dockActive = true; dockLastTs = performance.now(); });
  E.stemDock.addEventListener("pointerleave", () => { dockActive = false; dockLastTs = performance.now(); });
  E.stemDock.addEventListener("pointerdown", () => { dockLastTs = performance.now(); });
  armDockIdle();

  /* ============================================================
     MODULE API — the shared bridge surface handed to the UI
     modules (models.js / lyrics.js / tagedit.js). `on` stays a
     single primary handler per type; modules observing types
     app.js already owns (tracks, albumtracks, now) use `tap`.
     ============================================================ */
  function currentPos() {
    const playing = !!(state.now && state.now.playing);
    let pos = smoothPos;
    if (playing && smoothDur > 0) pos = clamp(smoothPos + (performance.now() - lastNowTs), 0, smoothDur);
    return { pos, dur: smoothDur, playing };
  }
  const modApi = {
    send, on, tap,
    getTracks: () => state.trkRows,
    tracksDone: () => state.trkDone,
    tracksTotal: () => state.trkTotal,
    requestMoreTracks: loadTracks,
    getNow: () => state.now,
    getPos: currentPos,
    /* Drive the search box + view from code ("Find more from same…"). */
    setSearch: (q) => runSearch(q),
    /* Media-tool scope data (unified roots + DB folder dimension). */
    getRoots: () => (state.roots || []),
    getFolders: () => (state.folders || []),
  };

  /* ============================================================
     NAV wiring
     "models" and "lyrics" are UI-only views handled outside the
     numeric data-view system so the C-side view enum stays
     untouched.
     ============================================================ */
  function openModelsView() {
    state.view = "models";
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "models"));
    showPanel("view-models");
    updateViewTitle();
    if (window.MnModels) window.MnModels.open(modApi);
  }
  function openLyricsView() {
    state.view = "lyrics";
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "lyrics"));
    showPanel("view-lyrics");
    updateViewTitle();
    if (window.MnLyrics) window.MnLyrics.open(modApi);
  }
  /* ---------- AUDIOBOOKS (permanent sidebar category) ----------
     Flips the C-side category filter: EVERY surface — tracks, albums,
     facets, search, suggestions — shows ONLY audiobook-root content while
     in this mode, and never shows it outside (full isolation both ways).
     Books render through the (virtual) albums grid. */
  function kindLabel(kind) {
    if (kind === "audiobook") return "Audiobooks";
    return kind ? kind.charAt(0).toUpperCase() + kind.slice(1) : "Library";
  }
  function setCategoryMode(kind) {
    kind = String(kind || "");
    /* persist so launch can return you to the SAME library (e.g. Audiobooks)
       instead of always dropping into music */
    try { localStorage.setItem("mn.lastkind", kind); } catch (_) {}
    /* book-mode class FIRST (even when kind unchanged): every audiobook-only
       control — Continue shelf, ±30s skip, bookmark, speed — is CSS-gated on
       body.kind-mode so none of it can appear in the music library */
    document.body.classList.toggle("kind-mode", !!kind);
    if ((state.activeKind || "") === kind) return;
    state.activeKind = kind;
    state.abMode = kind === "audiobook";        /* legacy readers */
    send({ cmd: "category", kind: kind });
    state.albDirty = true;     /* every library re-queries on next view */
    /* the Continue shelf belongs to kind views only — hide on music */
    if (!kind && typeof hideContinueShelf === "function") hideContinueShelf();
  }
  /* Open any per-kind library (audiobook or a custom designation like
     "ost") — books/albums render through the virtual albums grid. */
  function openKindView(kind, navKey) {
    navClearSearchContext();
    setCategoryMode(kind);
    state.view = 1;
    state.category = null; state.categoryFilter = null;
    if (typeof updateCatHeader === "function") updateCatHeader();
    try { localStorage.setItem("mn.lastview", "1"); } catch (_) {}
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === navKey));
    if (typeof syncViewModeSeg === "function") syncViewModeSeg();
    showPanel("view-albums");
    send({ cmd: "view", v: 1 });
    resetAlbums(); loadAlbums();
    send({ cmd: "kindstats" });    /* hours-listened for the title */
    refreshContinueShelf();        /* recently-played books pinned on top */
    updateViewTitle();
    if (typeof window.__mnNavPush === "function") window.__mnNavPush();
  }
  function openAudiobooksView() { openKindView("audiobook", "audiobooks"); }
  window.__mnOpenKind = openKindView;               /* history restore */

  /* dynamic sidebar section for CUSTOM designations (ost, podcast, …);
     audiobooks keeps its permanent entry */
  state.kinds = [];
  on("kinds", (m) => {
    state.kinds = (m.kinds || []).map((k) => k.kind);
    const host = $("#nav-kind-items"), grp = $("#nav-custom-kinds");
    if (!host || !grp) return;
    const customs = state.kinds.filter((k) => k !== "audiobook");
    grp.hidden = !customs.length;
    host.innerHTML = "";
    customs.forEach((k) => {
      const b = el("button", "nav-item");
      b.dataset.view = "kind:" + k;
      const ico = el("span", "nav-ico", "◈");
      b.appendChild(ico);
      b.appendChild(document.createTextNode(kindLabel(k)));
      b.addEventListener("click", () => openKindView(k, "kind:" + k));
      host.appendChild(b);
    });
    /* refresh any open folders view so its selectors show fresh options */
    if (typeof renderFolders === "function" && state.folders.length) renderFolders();

    /* One-shot boot restore: return to the per-kind library the user was in
       last session (e.g. Audiobooks), now that we know which kinds exist.
       "audiobook" is always valid (built-in section); customs must be present. */
    if (state._pendingBootKind) {
      const k = state._pendingBootKind;
      state._pendingBootKind = null;
      const valid = k === "audiobook" || state.kinds.indexOf(k) >= 0;
      if (valid) openKindView(k, k === "audiobook" ? "audiobooks" : "kind:" + k);
    }
  });

  $$(".nav-item").forEach((n) => n.addEventListener("click", () => {
    const dv = n.dataset.view;
    if (dv === "audiobooks") { openAudiobooksView(); return; }
    navClearSearchContext();   /* sidebar = fresh, unfiltered view */
    setCategoryMode("");       /* any other destination is the MUSIC library */
    if (dv === "models") { openModelsView(); return; }
    if (dv === "lyrics") { openLyricsView(); return; }
    if (dv === "mediatool") { openMediaToolView(); return; }
    if (dv === "stats")  { openStatsView(); return; }
    if (dv === "years")  { openYearsView(); return; }
    if (dv && dv.indexOf("cat:") === 0) { openCategory(dv.slice(4)); return; }
    switchView(+dv);
  }));

  /* Media Manager: separate power-tool module (mediatool.js). */
  function openMediaToolView() {
    state.view = "mediatool";
    state.category = null; state.categoryFilter = null;
    if (typeof updateCatHeader === "function") updateCatHeader();
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "mediatool"));
    showPanel("view-mediatool");
    E.viewTitle.textContent = "Media Manager";
    send({ cmd: "roots" });     /* fresh scope data for the tool */
    send({ cmd: "folders" });
    if (window.MnMediaTool) MnMediaTool.open(modApi);
    if (typeof window.__mnNavPush === "function") window.__mnNavPush();
  }

  /* Years browse: string-keyed view (like models/lyrics) over the facet grid.
     mn_facet_dim: MN_FACET_YEAR == 5. */
  function openYearsView() {
    state.view = "years";
    state.category = null; state.categoryFilter = null;
    if (typeof updateCatHeader === "function") updateCatHeader();
    try { localStorage.setItem("mn.lastview", "years"); } catch (_) {}
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "years"));
    showPanel("view-facet");
    openFacet("years", 5);
    E.viewTitle.textContent = "Years";
  }

  /* ============================================================
     LIBRARY STATISTICS view — {"cmd":"stats"} → {"type":"stats",...}
     ============================================================ */
  const statsRoot = $("#stats-root");
  function openStatsView() {
    state.view = "stats";
    $$(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.view === "stats"));
    showPanel("view-stats");
    updateViewTitle();
    statsRoot.innerHTML = "";
    statsRoot.appendChild(el("div", "stats-loading", "Crunching library statistics…"));
    send({ cmd: "stats" });
  }
  const FMT_COLORS = ["#FB8C00", "#00E5FF", "#8B5CF6", "#1DB954", "#FFD166", "#FF6B6B", "#4D9FFF", "#9c9c9c"];
  function statMeter(label, pct, bg) {
    const sec = el("div", "stats-meter");
    const head = el("div", "sm-head");
    head.appendChild(el("span", "sm-label", label));
    head.appendChild(el("span", "sm-pct", Math.round(pct) + "%"));
    sec.appendChild(head);
    const track = el("div", "sm-track");
    const fill = el("div", "sm-fill");
    fill.style.width = clamp(pct, 0, 100) + "%";
    if (bg) fill.style.background = bg;
    track.appendChild(fill);
    sec.appendChild(track);
    return sec;
  }
  on("stats", (m) => {
    if (!statsRoot) return;
    statsRoot.innerHTML = "";

    /* big numbers */
    const gb = (m.size_bytes || 0) / (1024 * 1024 * 1024);
    const grid = el("div", "stats-grid");
    const card = (num, label, cls) => {
      const c = el("div", "stat-card" + (cls ? " " + cls : ""));
      c.appendChild(el("div", "stat-num", num));
      c.appendChild(el("div", "stat-label", label));
      return c;
    };
    grid.appendChild(card((m.tracks || 0).toLocaleString(), "Songs"));
    grid.appendChild(card((m.albums || 0).toLocaleString(), "Albums"));
    grid.appendChild(card((m.artists || 0).toLocaleString(), "Artists"));
    grid.appendChild(card((m.hours || 0).toLocaleString(undefined, { maximumFractionDigits: 1 }) + " h", "Total playtime"));
    grid.appendChild(card((gb >= 100 ? gb.toFixed(0) : gb.toFixed(1)) + " GB", "Library size"));
    grid.appendChild(card((m.missing || 0).toLocaleString(), "Missing files", (m.missing || 0) > 0 ? "warn" : ""));
    statsRoot.appendChild(grid);

    /* format breakdown: proportional colored segments + legend */
    const fmts = (Array.isArray(m.formats) ? m.formats : []).filter((f) => f && f.n > 0);
    const total = fmts.reduce((s, f) => s + f.n, 0);
    if (total > 0) {
      const sec = el("div", "stats-sec");
      sec.appendChild(el("div", "stats-sec-title", "Formats"));
      const bar = el("div", "fmt-bar");
      const legend = el("div", "fmt-legend");
      fmts.forEach((f, i) => {
        const color = FMT_COLORS[i % FMT_COLORS.length];
        const seg = el("div", "fmt-seg");
        seg.style.width = (f.n / total * 100) + "%";
        seg.style.background = color;
        seg.title = String(f.fmt).toUpperCase() + " — " + f.n.toLocaleString();
        bar.appendChild(seg);
        const li = el("span", "fmt-li");
        const dot = el("i", "dot"); dot.style.background = color;
        li.appendChild(dot);
        li.appendChild(document.createTextNode(
          String(f.fmt).toUpperCase() + " " + f.n.toLocaleString() + " (" + Math.round(f.n / total * 100) + "%)"));
        legend.appendChild(li);
      });
      sec.appendChild(bar);
      sec.appendChild(legend);
      statsRoot.appendChild(sec);
    }

    /* quality meters */
    const qsec = el("div", "stats-sec");
    qsec.appendChild(el("div", "stats-sec-title", "Quality"));
    qsec.appendChild(statMeter("Hi-res audio", m.hires_pct || 0, "linear-gradient(90deg,var(--accent),var(--accent-2))"));
    qsec.appendChild(statMeter("Tracks with lyrics", m.lyrics_pct || 0, "var(--accent-2)"));
    statsRoot.appendChild(qsec);
  });

  /* ============================================================
     POLL loop (250ms) + rAF loop
     ============================================================ */

  /* ---------- playback details row + mute ---------- */
  let lastVolBeforeMute = 1;
  const btnMute = $("#btn-mute");
  if (btnMute) btnMute.addEventListener("click", () => {
    /* icon state (slash / waves) is CSS-class-driven from setVolUI — the
       old emoji swap replaced the button's SVG with a font glyph that
       didn't always render (the "missing volume icon"). */
    const cur = state.now && state.now.volume != null ? state.now.volume : 1;
    if (cur > 0) { lastVolBeforeMute = cur; setVolUI(0); sendVol(0); }
    else { const v = lastVolBeforeMute || 1; setVolUI(v); sendVol(v); }
    if (state.now) state.now.volume = cur > 0 ? 0 : (lastVolBeforeMute || 1);
  });
  function updateDetailsRow(m) {
    const dr = $("#pl-details");
    if (!dr) return;
    const parts = [];
    if (m.format) parts.push("<b>" + esc(String(m.format).toUpperCase()) + "</b>");
    if (m.bitrate_kbps) parts.push(m.bitrate_kbps + " kbps");
    if (m.sample_rate) parts.push((m.sample_rate / 1000).toFixed(m.sample_rate % 1000 ? 1 : 0) + " kHz");
    if (m.out_rate && m.out_rate !== m.sample_rate) parts.push("→ <b>" + (m.out_rate / 1000).toFixed(m.out_rate % 1000 ? 1 : 0) + " kHz</b>");
    if (m.out_bits) parts.push(m.out_bits + "-bit out");
    if (m.stems_enabled && m.stem_fraction != null) parts.push("stems <b>" + Math.round(m.stem_fraction * 100) + "%</b>");
    if (m.play_count) parts.push(m.play_count + " plays");
    dr.innerHTML = parts.join('<span style="opacity:.35">·</span>');
  }

  function poll() {
    send({ cmd: "now" });
    if (state.scanActive) send({ cmd: "scan" });
    if (loopNeeded()) wakeLoop();   /* re-arm the parked rAF loop (≤250ms) */
  }
  /* 250 ms keeps seek/pill state snappy; low-power stretches to 800 ms;
     hidden (minimized/occluded) drops to 2 s — Chromium throttles background
     timers to ~1 Hz anyway, so asking for less just avoids queued backlog.
     Re-armable so the settings toggle takes effect without a restart. */
  function pollInterval() {
    if (document.hidden || window.__mnWinHidden) return 2000;
    /* IDLE-CPU: when nothing is actually moving — paused (or stopped) and the
       user isn't dragging the seek pill and no library scan is streaming — the
       "now" payload is static, so a 4 Hz poll just burns a bridge round-trip +
       C-side build_now every 250 ms for no visible change. Stretch to 1.5 s.
       The seek pill is rAF-interpolated (not poll-driven), so playback
       smoothness is unaffected; the on("now") handler re-arms to fast cadence
       the instant `playing` flips true (incl. external media-key/taskbar
       resume), so the worst case is a ~1.5 s UI catch-up, never a playback or
       gapless-timing bug. */
    const n = state.now;
    if (n && !n.playing && !state.seekDrag && !state.scanActive) return 1500;
    return lowPower() ? 800 : 250;
  }
  let pollTimer = setInterval(poll, pollInterval());
  window.__mnRearmPoll = () => {
    clearInterval(pollTimer);
    pollTimer = setInterval(poll, pollInterval());
  };
  /* restore from minimized: poll + rAF loop resume INSTANTLY instead of
     waiting out the throttled interval */
  document.addEventListener("visibilitychange", () => {
    window.__mnRearmPoll();
    if (!document.hidden) { poll(); wakeLoop(); }
  });
  /* C pushes minimize/restore (the embedded widget never flips
     document.hidden — see cef_host.c vis task) */
  on("vis", (m) => {
    window.__mnWinHidden = !!m.hidden;
    window.__mnRearmPoll();
    /* park the Cover Flow front-cover mesh while minimized (its idle orbit
       never settles on its own) and re-arm it on restore */
    if (CF.on && CF.mesh) CF.mesh.setActive(!m.hidden && !CF.meshCanvas.hidden);
    if (!m.hidden) { poll(); wakeLoop(); if (CF.on) coverflowKick(); }
  });

  /* ---------- Up Next queue (refreshed on track change + every 2s) ---------- */
  /* drag-reorder state for the Up-Next list (declared WITH its users) */
  let qDragFrom = null;
  function refreshQueueSoon() { setTimeout(() => send({ cmd: "queue" }), 60); }
  let qFirstRender = true;   /* slide-in entrance plays ONCE, not per refresh */
  /* Structural signature of a queue item list (ids + order). When it's
     unchanged between renders, only the current-track marker moved, so we can
     update in place instead of wiping innerHTML — which would reset scrollTop
     and jump the queue to the top. */
  function qSig(items) {
    if (!items || !items.length) return "";
    let s = items.length + "|";
    for (let i = 0; i < items.length; i++) s += (items[i].id || items[i].index) + ",";
    return s;
  }
  let qLastSig = "";
  let qAnimateOnce = false;   /* q-new entrances armed by a content change */
  function qUpdatePlayingClass() {
    if (!E.npQueue) return;
    const rows = $$(".q-item", E.npQueue);
    for (let i = 0; i < rows.length; i++) {
      const idx = +rows[i].dataset.qindex;
      rows[i].classList.toggle("playing", idx === state._queueCurrent);
    }
  }

  function renderQueue(items) {
    if (!E.npQueue) return;
    /* Same tracks in the same order → the backend just told us the current
       track changed. Re-mark the playing row in place, leave scroll alone. */
    const sig = qSig(items);
    if (sig && sig === qLastSig && E.npQueue.querySelector(".q-item, .q-pad")) {
      qItems = items;
      qUpdatePlayingClass();
      return;
    }
    qLastSig = sig;
    qAnimateOnce = true;   /* entrance animation for THIS content change only —
                              scroll-driven window re-renders must not replay it
                              (rows blinked and you lost your place) */
    const prevScroll = E.npQueue.scrollTop;
    if (qFirstRender) {
      qFirstRender = false;
      E.npQueue.classList.add("q-anim");
      setTimeout(() => E.npQueue.classList.remove("q-anim"), 600);
    }
    E.npQueue.innerHTML = "";
    if (!items || !items.length) {
      E.npQueue.appendChild(el("div", "np-empty", "Nothing queued"));
      return;
    }
    /* header: count + Clear + Save-as-playlist */
    const head = el("div", "q-head");
    head.appendChild(el("span", "q-head-lbl", "Up Next · " + items.length));
    const svq = el("button", "q-clear", "Save");
    svq.title = "Save queue as a playlist";
    svq.addEventListener("click", () => {
      const n = prompt("Save queue as playlist:", "Saved Queue");
      if (n && n.trim()) send({ cmd: "savequeue", name: n.trim() });
    });
    head.appendChild(svq);
    const clr = el("button", "q-clear", "Clear");
    clr.title = "Clear the queue";
    clr.addEventListener("click", () => {
      if (confirm("Clear the play queue?")) { send({ cmd: "queueclear" }); refreshQueueSoon(); }
    });
    head.appendChild(clr);
    E.npQueue.appendChild(head);

    qItems = items;
    /* two flow spacers + only a window of real rows — see qRenderWindow */
    qPadTop = el("div", "q-pad");
    qPadBot = el("div", "q-pad");
    E.npQueue.appendChild(qPadTop);
    E.npQueue.appendChild(qPadBot);
    qWinFirst = -1;
    /* restore the scroll position from before the innerHTML wipe, then draw
       the window for that position (a real rebuild = queue edited, not just a
       current-track change). */
    E.npQueue.scrollTop = prevScroll;
    qRenderWindow(true);
    /* measure the exact row pitch (row height + flex gap) once per render */
    requestAnimationFrame(() => {
      const rows = $$(".q-item", E.npQueue);
      if (rows.length >= 2) {
        const pitch = rows[1].offsetTop - rows[0].offsetTop;
        if (pitch > 0 && Math.abs(pitch - qRowH) > 0.5) { qRowH = pitch; qRenderWindow(true); }
      } else if (rows.length === 1 && rows[0].offsetHeight > 0) {
        qRowH = rows[0].offsetHeight + 2;
      }
    });
  }

  /* Build one queue row (extracted from the old render-everything loop). */
  function qRowEl(it) {
      const row = el("div", "q-item" + (qAnimateOnce ? " q-new" : "") +
                     (it.index === state._queueCurrent ? " playing" : ""));
      /* queueSlide channel: staggered slide-in ONLY when the queue content
         changed (qAnimateOnce) — never on scroll-window re-renders */
      if (qAnimateOnce) row.style.setProperty("--i", String(Math.abs(it.index || 0) % 10));
      if (it.index != null) row.dataset.qindex = it.index;
      row.draggable = true;
      /* double-click a queue row jumps to it WITHOUT dropping other tracks */
      if (it.index != null) row.addEventListener("dblclick", () => send({ cmd: "playqueue", index: it.index }));
      /* drag to reorder */
      row.addEventListener("dragstart", (e) => {
        qDragFrom = it.index;
        row.classList.add("dragging");
        if (e.dataTransfer) e.dataTransfer.effectAllowed = "move";
      });
      row.addEventListener("dragend", () => {
        qDragFrom = null;
        row.classList.remove("dragging");
        $$(".q-item.drop-target", E.npQueue).forEach((x) => x.classList.remove("drop-target"));
      });
      row.addEventListener("dragover", (e) => { e.preventDefault(); row.classList.add("drop-target"); });
      row.addEventListener("dragleave", () => row.classList.remove("drop-target"));
      row.addEventListener("drop", (e) => {
        e.preventDefault();
        row.classList.remove("drop-target");
        if (qDragFrom != null && qDragFrom !== it.index) {
          send({ cmd: "queuemove", from: qDragFrom, to: it.index });
          refreshQueueSoon();
        }
        qDragFrom = null;
      });
      const art = el("div", "q-art");
      setArt(art, it.art, "1em", artKeyOf(it.album_artist || it.artist, it.album));
      row.appendChild(art);
      const txt = el("div", "q-txt");
      txt.appendChild(el("div", "q-title", esc(it.title) || "Unknown"));
      txt.appendChild(el("div", "q-artist", esc(it.artist) || "—"));
      /* detail pills: format, bitrate, duration, plays, like state */
      const meta = el("div", "q-meta");
      const qpill = (cls, txt, tip) => {
        const p = el("span", cls, txt);
        if (tip) p.title = tip;
        meta.appendChild(p);
      };
      if (it.liked === 1) qpill("q-pill liked", "👍", "You liked this track");
      else if (it.liked === -1) qpill("q-pill disliked", "👎", "You disliked this track");
      if (it.format) qpill("q-pill fmt " + (isLossless(it.format) ? "lossless" : "lossy"),
        String(it.format).toUpperCase(),
        isLossless(it.format) ? "Audio format — lossless" : "Audio format — lossy");
      if (it.bitrate) qpill("q-pill", it.bitrate + "k", "Bitrate: " + it.bitrate + " kbps (audio data per second)");
      if (isHires(it.bit_depth, it.sample_rate)) qpill("q-pill hires", "HR",
        "Hi-res audio — better than CD quality (24-bit or ≥88.2 kHz)");
      if (it.duration_ms) qpill("q-pill", fmtTime(it.duration_ms), "Track length");
      if (it.play_count) qpill("q-pill", "▶" + it.play_count,
        "Played " + it.play_count + (it.play_count === 1 ? " time" : " times"));
      txt.appendChild(meta);
      row.appendChild(txt);
      /* like/dislike directly on the queue row */
      if (it.id > 0) {
        const th = el("div", "q-thumbs");
        const up = el("span", "thumb up" + (it.liked === 1 ? " on" : ""), "👍");
        up.title = "Like";
        const dn = el("span", "thumb down" + (it.liked === -1 ? " on" : ""), "👎");
        dn.title = "Dislike";
        const rate = (v) => {
          const next = (it.liked === v) ? 0 : v;
          send({ cmd: "like", id: it.id, v: next });
          it.liked = next;
          up.classList.toggle("on", next === 1);
          dn.classList.toggle("on", next === -1);
          if (state.now && state.now.track_id === it.id) updateThumbsUI(next);
        };
        up.addEventListener("click", (e) => { e.stopPropagation(); rate(1); });
        dn.addEventListener("click", (e) => { e.stopPropagation(); rate(-1); });
        th.appendChild(up); th.appendChild(dn);
        row.appendChild(th);
      }
      /* remove-from-queue button */
      const rm = el("button", "q-remove", "✕");
      rm.title = "Remove from queue";
      rm.addEventListener("click", (e) => {
        e.stopPropagation();
        send({ cmd: "queueremove", index: it.index });
        refreshQueueSoon();
      });
      row.appendChild(rm);
      return row;
  }

  /* ---- VIRTUAL QUEUE WINDOW ----
     512 always-rendered contained rows measured ~56ms of compositor
     Layerize PER FRAME (each row is its own paint chunk — the single
     biggest jank source found in the 4K scroll trace). Only ~a screen of
     rows exists in the DOM; two flow spacers keep the scrollbar exact. */
  let qItems = [], qRowH = 56, qWinFirst = -1, qPadTop = null, qPadBot = null, qScrollRaf = 0;
  const Q_BUF = 8;
  function qRenderWindow(force) {
    const q = E.npQueue;
    if (!q || !qPadTop || !qPadTop.isConnected) return;
    /* consume the one-shot entrance flag on the first window after a content
       change; every later (scroll) window renders without animation */
    const animThis = qAnimateOnce;
    setTimeout(() => { qAnimateOnce = false; }, 0);
    void animThis;
    /* Never re-window mid drag-reorder: removing the dragged row from the DOM
       cancels the native HTML5 drag and orphans .dragging/.drop-target. */
    if (qDragFrom != null) return;
    const vh = q.clientHeight || 400;
    const first = Math.max(0, Math.floor(q.scrollTop / qRowH) - Q_BUF);
    if (!force && first === qWinFirst) return;
    qWinFirst = first;
    const last = Math.min(qItems.length, first + Math.ceil(vh / qRowH) + Q_BUF * 2);
    for (let n = qPadTop.nextSibling; n && n !== qPadBot;) {
      const nx = n.nextSibling; n.remove(); n = nx;
    }
    const frag = document.createDocumentFragment();
    for (let i = first; i < last; i++) frag.appendChild(qRowEl(qItems[i]));
    q.insertBefore(frag, qPadBot);
    qPadTop.style.height = (first * qRowH) + "px";
    qPadBot.style.height = (Math.max(0, qItems.length - last) * qRowH) + "px";
  }
  if (E.npQueue) E.npQueue.addEventListener("scroll", () => {
    if (!qScrollRaf) qScrollRaf = requestAnimationFrame(() => { qScrollRaf = 0; qRenderWindow(); });
  }, { passive: true });
  on("queue", (m) => { state._queueCurrent = m.current; renderQueue(m.items || []); });

  /* Authoritative like/dislike ack from the backend ({type:"liked",id,v}).
     Repaint every affected surface with the value the backend actually stored,
     independent of the 4 Hz now-poll. */
  on("liked", (m) => {
    const id = m.id, v = m.v || 0;
    if (id == null) return;
    if (state.now && state.now.track_id === id) {
      state.now.liked = v;
      updateThumbsUI(v);
    }
    const row = (state.trkRows || []).find((r) => r.id === id);
    if (row) row.liked = v;
  });
  let lastQueueKey = "";
  setInterval(() => {
    /* refresh when a track is playing (cheap; the reply is small) */
    if (state.now && state.now.track_id != null) {
      const k = state.now.track_id + ":" + (state.now.playing ? 1 : 0);
      if (k !== lastQueueKey) { lastQueueKey = k; send({ cmd: "queue" }); }
    }
  }, 900);

  /* ============================================================
     FPS OVERLAY (debug) — Ctrl+Shift+F toggles; persists via
     localStorage("mn.fps"). Runs its OWN rAF loop so it keeps
     measuring even when the app's main loop idles.
     ============================================================ */
  const fpsDbg = { el: null, raf: 0, frames: [], last: 0, acc: 0 };
  function fpsLoop(t) {
    if (!fpsDbg.el) return;
    if (fpsDbg.last) {
      const dt = t - fpsDbg.last;
      fpsDbg.frames.push(dt);
      if (fpsDbg.frames.length > 180) fpsDbg.frames.shift();
      fpsDbg.acc += dt;
      if (fpsDbg.acc > 250) {
        fpsDbg.acc = 0;
        const fs = fpsDbg.frames;
        let sum = 0, worst = 0;
        for (const d of fs) { sum += d; if (d > worst) worst = d; }
        const avg = sum / fs.length;
        fpsDbg.el.textContent =
          (1000 / avg).toFixed(0) + " fps  ·  worst " + worst.toFixed(0) + " ms";
      }
    }
    fpsDbg.last = t;
    fpsDbg.raf = requestAnimationFrame(fpsLoop);
  }
  function fpsShow(on) {
    if (on && !fpsDbg.el) {
      const b = el("div");
      b.style.cssText = "position:fixed;top:8px;right:12px;z-index:999;padding:6px 12px;" +
        "background:rgba(0,0,0,.78);color:#0f0;font:700 16px ui-monospace,Consolas,monospace;" +
        "border-radius:8px;pointer-events:none;border:1px solid rgba(0,255,0,.35)";
      b.textContent = "-- fps";
      document.body.appendChild(b);
      fpsDbg.el = b; fpsDbg.last = 0; fpsDbg.frames.length = 0;
      fpsDbg.raf = requestAnimationFrame(fpsLoop);
    } else if (!on && fpsDbg.el) {
      cancelAnimationFrame(fpsDbg.raf);
      fpsDbg.el.remove(); fpsDbg.el = null;
    }
  }
  document.addEventListener("keydown", (e) => {
    if (e.ctrlKey && e.shiftKey && e.code === "KeyF") {
      const on = !fpsDbg.el;
      localStorage.setItem("mn.fps", on ? "1" : "0");
      fpsShow(on);
    }
  });
  if (localStorage.getItem("mn.fps") === "1") fpsShow(true);

  /* ============================================================
     NAVIGATION HISTORY — browser-style back/forward with scroll restore.
     Mouse buttons 3 (back) / 4 (forward) and Alt+Left / Alt+Right move
     through a stack of {view, query, sort, expandedAlbum, scroll positions}.
     ============================================================ */
  const nav = { stack: [], idx: -1, restoring: false };

  function activeScroller() {
    if (state.view === 0 || (typeof state.view === "number" && state.view > 1)) return E.trackScroll;
    if (state.view === 1) return E.albumGrid;
    return null;
  }
  function snapshotNav() {
    const sc = activeScroller();
    return {
      view: state.view,
      kind: state.activeKind || "",   /* per-kind library is part of the location */
      /* `query` is the SERVER-SIDE library filter (cmd:"search") — the thing
         that used to get stuck: history restored the search box text but
         never re-sent the filter, so the backend kept serving the narrowed
         library ("26 albums no matter what") until restart. */
      query: state.query || "",
      searchOpen: !!state.searchOpen,
      searchQ: state.searchOpen ? (saLastQ || "") : "",
      sort: state.sort,
      category: state.category || null,
      expandedAlbum: state.expandedAlbum || null,
      scroll: sc ? sc.scrollTop : 0,
    };
  }
  function updateNavBtns() {
    const b = $("#nav-back"), f = $("#nav-fwd");
    if (b) b.disabled = nav.idx <= 0;
    if (f) f.disabled = nav.idx >= nav.stack.length - 1;
  }
  /* Append the just-entered location as a new history entry. Called AFTER a
     navigation lands (view/search change). Not called while restoring. */
  function navPush() {
    if (nav.restoring) return;
    const snap = snapshotNav();   /* scroll starts at 0 for a fresh destination */
    /* drop any forward history when navigating anew */
    nav.stack = nav.stack.slice(0, nav.idx + 1);
    const top = nav.stack[nav.idx];
    /* typing in the search box evolves the SAME results entry — replace the
       top instead of pushing one entry per keystroke */
    if (top && top.searchOpen && snap.searchOpen) {
      nav.stack[nav.idx] = snap;
      updateNavBtns();
      return;
    }
    /* skip a duplicate of the current top (e.g. re-clicking the same view) */
    if (top && top.view === snap.view && top.query === snap.query &&
        top.category === snap.category && top.searchOpen === snap.searchOpen) return;
    nav.stack.push(snap);
    nav.idx = nav.stack.length - 1;
    if (nav.stack.length > 100) { nav.stack.shift(); nav.idx--; }
    updateNavBtns();
  }
  function restoreNav(snap) {
    nav.restoring = true;
    clearTimeout(searchTimer); clearTimeout(sugTimer);
    state.sort = snap.sort || state.sort;
    if (E.sort) E.sort.value = state.sort;
    /* REAPPLY the server-side filter — it lives in the C backend and does
       not change unless told to. This is the fix for the stuck library. */
    const wantFilter = snap.query || "";
    setLibraryFilter(wantFilter);
    if (snap.searchOpen) {
      /* the RESULTS PANEL is a real history entry — restore it live */
      if (E.search) E.search.value = snap.searchQ || "";
      state.searchOpen = true;
      saGen++; saLastQ = snap.searchQ || "";
      send({ cmd: "searchall", q: saLastQ, gen: saGen });
      showPanel("view-search");
      E.viewTitle.textContent = 'Results for "' + saLastQ + '"';
    } else {
      if (E.search) E.search.value = wantFilter;
      state.searchOpen = false;
      const wantKind = snap.kind || "";
      if (wantKind !== (state.activeKind || "")) setCategoryMode(wantKind);
      if (wantKind && typeof window.__mnOpenKind === "function")
        window.__mnOpenKind(wantKind, wantKind === "audiobook" ? "audiobooks" : "kind:" + wantKind);
      else if (snap.category) { if (typeof openCategory === "function") openCategory(snap.category); }
      else switchView(snap.view);
    }
    /* content loads async (bridge round-trip); restore scroll after it settles */
    const restoreScroll = () => {
      const sc = activeScroller();
      if (sc) sc.scrollTop = snap.scroll || 0;
      nav.restoring = false;
      updateNavBtns();
    };
    setTimeout(restoreScroll, 60);
    setTimeout(() => { const sc = activeScroller(); if (sc) sc.scrollTop = snap.scroll || 0; }, 250);
    setTimeout(() => { const sc = activeScroller(); if (sc) sc.scrollTop = snap.scroll || 0; }, 600);
  }
  function navBack() {
    if (nav.idx <= 0) return;
    /* save live scroll of current before leaving */
    const sc = activeScroller(); if (sc && nav.stack[nav.idx]) nav.stack[nav.idx].scroll = sc.scrollTop;
    nav.idx--;
    restoreNav(nav.stack[nav.idx]);
  }
  function navForward() {
    if (nav.idx >= nav.stack.length - 1) return;
    const sc = activeScroller(); if (sc && nav.stack[nav.idx]) nav.stack[nav.idx].scroll = sc.scrollTop;
    nav.idx++;
    restoreNav(nav.stack[nav.idx]);
  }
  /* visible browser-style buttons in the topbar */
  { const b = $("#nav-back"), f = $("#nav-fwd");
    if (b) b.addEventListener("click", navBack);
    if (f) f.addEventListener("click", navForward); }
  /* refresh: re-pull the current view's data from the backend (browser-style) */
  function refreshCurrentView() {
    send({ cmd: "kinds" });
    send({ cmd: "roots" });
    send({ cmd: "folders" });
    state.albDirty = true;
    if (state.searchOpen) { runSearch(saLastQ || (E.search && E.search.value) || ""); return; }
    if (state.view === 4) { loadFolders(); return; }
    if (state.view === 1) { resetAlbums(); loadAlbums(); updateViewTitle(); return; }
    if (typeof state.view === "number") { resetTracks(); loadTracks(); updateViewTitle(); return; }
    updateViewTitle();   /* string views (models/stats/…) refresh on re-entry */
  }
  { const r = $("#nav-refresh");
    if (r) r.addEventListener("click", refreshCurrentView); }
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "F5") { ev.preventDefault(); refreshCurrentView(); }
  });
  /* Mouse back/forward (buttons 3 & 4) anywhere in the app. */
  window.addEventListener("mouseup", (ev) => {
    if (ev.button === 3) { ev.preventDefault(); navBack(); }
    else if (ev.button === 4) { ev.preventDefault(); navForward(); }
  });
  /* some mice send these via the 'auxclick'/pointer path — also catch them */
  window.addEventListener("mousedown", (ev) => {
    if (ev.button === 3 || ev.button === 4) ev.preventDefault();
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.altKey && ev.key === "ArrowLeft") { ev.preventDefault(); navBack(); }
    else if (ev.altKey && ev.key === "ArrowRight") { ev.preventDefault(); navForward(); }
  });
  /* expose so nav-triggering actions can record a history entry */
  window.__mnNavPush = navPush;

  /* ============================================================
     BOOT
     ============================================================ */
  buildFaders();
  /* tilt3d/spotlight/parallax channels: pointer-tracking card effects
     (event-driven, rAF only while a pointer moves over a card) */
  motion.attachTilt($("#album-grid"), ".album-card");
  /* magnetic channel: transport buttons lean toward the cursor */
  motion.attachMagnetic($(".transport"), ".tbtn");
  /* likeBurst channel: particle burst on any like press (delegated) */
  document.addEventListener("click", (ev) => {
    const t = ev.target && ev.target.closest
      ? ev.target.closest(".thumb.up, #btn-like") : null;
    if (t) motion.burst(t);
  }, { passive: true });
  if (window.MnTagEdit) MnTagEdit.init(modApi);   /* context menus + tag/art modal  */
  if (window.MnLyrics) MnLyrics.init(modApi);     /* now-playing lyrics button      */
  /* Manual rearrangement: drag nav items within their group; drag stem faders. */
  if (window.MnRearrange) {
    $$("#nav .nav-group").forEach((g, i) => MnRearrange.attach(g, ".nav-item", "nav" + i));
    MnRearrange.attach(E.stemFaders, ".fader", "faders");
  }
  wakeLoop();   /* the rAF loop parks itself whenever it has nothing to do */
  /* Start on the configured startup view (Settings → Interface): an explicit
     Tracks/Albums choice wins; "last used" restores mn.lastview; first launch
     defaults to ALBUMS. */
  {
    let bootView = 1;
    const saved = localStorage.getItem("mn.lastview");
    if (saved !== null && saved !== "" && !isNaN(+saved)) bootView = +saved;
    const pref = localStorage.getItem("mn.startview");
    if (pref === "0" || pref === "1") bootView = +pref;
    /* Restore the last per-kind library (e.g. Audiobooks) so launch returns you
       where you were — but only when the user didn't force a fixed startview,
       and the kind still exists as a sidebar section (checked once kinds
       arrive). Falls back to the numeric view otherwise. */
    const lastKind = localStorage.getItem("mn.lastkind");
    if (lastKind && !(pref === "0" || pref === "1")) {
      state._pendingBootKind = lastKind;   /* openKindView once "kinds" confirms it */
      switchView(bootView === 1 ? 1 : bootView);   /* provisional; kind view applied below */
    } else {
      switchView(bootView);
    }
  }
  send({ cmd: "scan" });
  send({ cmd: "settings" });
  send({ cmd: "folders" });   /* hidden-folder banner state (graceful if unanswered) */
  send({ cmd: "kinds" });     /* sidebar sections for custom folder designations */
  send({ cmd: "eq", action: "get" });   /* seed the bit-perfect verdict's DSP state */

  /* Optional startup rescan (Settings → Interface): incremental, a few
     seconds after launch so boot stays snappy — unchanged files skip in
     microseconds. */
  if (localStorage.getItem("mn.autorescan") === "1") {
    setTimeout(() => {
      send({ cmd: "rescan" });
      send({ cmd: "scan" });
      state.scanActive = true;
    }, 4000);
  }

  /* Restore the saved volume BEFORE anything can play — the C engine defaults
     to 1.0 (max), which is exactly what you don't want blasted at startup. */
  {
    const sv = parseFloat(localStorage.getItem("mn.volume"));
    if (!isNaN(sv)) {
      const v = clamp(sv, 0, 1);
      send({ cmd: "volume", v });
      if (typeof setVolUI === "function") setVolUI(v);
    }
  }

  /* Resume-on-launch: reload the last track at its saved position, PAUSED.
     One atomic C-side command (play+pause+seek under the app lock) — the old
     play-then-toggle dance raced the state poll and could leave the app
     playing out loud on startup. */
  {
    let resume = null;
    try { resume = JSON.parse(localStorage.getItem("mn.resume") || "null"); } catch (_) {}
    if (resume && resume.id != null && resume.id >= 0) {
      setTimeout(() => {
        send({ cmd: "resume", id: resume.id, ms: Math.max(0, resume.pos || 0) });
      }, 1200);
    } else {
      /* Book-aware fallback: no local resume (fresh install / cleared
         profile) — land in the most recent BOOK from the book_progress DB,
         paused at its exact chapter+position. tap(), not on(): the shelf
         owns the on("continuebooks") slot. One-shot. */
      let did = false;
      tap("continuebooks", (m) => {
        if (did) return;
        const b = m && m.books && m.books[0];
        if (b && b.track_id > 0 && !b.finished) {
          did = true;
          send({ cmd: "resume", id: b.track_id, ms: Math.max(0, b.pos_ms || 0) });
        }
      });
      setTimeout(() => send({ cmd: "continuebooks", max: 1 }), 1400);
    }
  }

  /* Import .m3u/.m3u8/.pls playlists found in the library folders (idempotent
     — already-imported names are skipped, so this is cheap on every boot). */
  setTimeout(() => send({ cmd: "importplaylists" }), 2500);

  poll();

  /* Register the core as a module block: other modules reach the bus and
     shared state ONLY through this api (see MODULES.md for the contract). */
  if (window.MN) MN.define("core", "2.8.0", ["motion", "custom", "artram"], function () {
    return {
      send, on,
      state,
      refreshTracks: () => { resetTracks(); loadTracks(); },
      refreshAlbums: () => { resetAlbums(); loadAlbums(); },
      toast: (m) => { if (typeof window.__mnToast === "function") window.__mnToast(m); },
    };
  });

})();
