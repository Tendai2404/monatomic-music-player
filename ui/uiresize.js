/* ============================================================
   UI RESIZE — uiresize.js                              v1.0.0
   ------------------------------------------------------------
   Direct-manipulation resizing for the content surfaces (the
   side/now-playing/player panels already have drag rails from
   resizers.js):

   1. TRACK-TABLE COLUMNS — drag the right edge of any header
      cell. Widths are written into the --track-cols template
      (all columns pinned to px on first drag) and persist as
      mn.trackcols. Double-click a handle to reset every column
      to the adaptive defaults.

   2. ALBUM CARD SIZE — Ctrl + mouse-wheel over the album grid
      zooms the card size (120–320 px), persisted as mn.cardmin.
      Works in grid AND details modes (details scales the cover
      column). Respects the UI-lock toggle.

   Standalone block: talks only to the DOM + localStorage — no
   core dependencies, safe to load before/after anything.
   ============================================================ */
MN.define("uiresize", "1.0.0", [], function () {
  "use strict";

  const root = document.documentElement;
  const uiLocked = () => root.classList.contains("ui-locked");

  /* ---------------- 1. track-table column resize ---------------- */
  (function initColResize() {
    const head = document.querySelector(".table-head");
    if (!head) return;
    const KEY = "mn.trackcols";

    /* restore persisted widths */
    try {
      const saved = localStorage.getItem(KEY);
      if (saved) root.style.setProperty("--track-cols", saved);
    } catch (_) {}

    const ths = Array.from(head.querySelectorAll(".th"));
    ths.forEach((th, idx) => {
      if (idx === ths.length - 1) return;      /* last column: no handle */
      const h = document.createElement("div");
      h.className = "col-grip";
      h.title = "Drag to resize column · double-click to reset all";
      th.style.position = "relative";
      th.appendChild(h);

      h.addEventListener("dblclick", (e) => {
        e.stopPropagation();
        root.style.removeProperty("--track-cols");
        try { localStorage.removeItem(KEY); } catch (_) {}
      });

      h.addEventListener("mousedown", (e) => {
        if (uiLocked()) return;
        e.preventDefault();
        e.stopPropagation();                    /* don't trigger header sort */
        const startX = e.clientX;
        /* pin EVERY column to its current pixel width so only the dragged
           one moves (fr units would rebalance all of them) */
        const widths = ths
          .filter((t) => t.offsetParent !== null)   /* skip hidden columns */
          .map((t) => Math.round(t.getBoundingClientRect().width));
        const visIdx = ths.filter((t, i) => i <= idx && t.offsetParent !== null).length - 1;
        const startW = widths[visIdx];

        let pending = 0;
        function apply() {
          pending = 0;
          root.style.setProperty("--track-cols", widths.map((w) => w + "px").join(" "));
        }
        /* rAF-coalesce: writing --track-cols relays the whole track grid;
           mousemove can outrun frames, so commit at most one write/frame */
        function move(ev) {
          widths[visIdx] = Math.max(36, startW + (ev.clientX - startX));
          if (!pending) pending = requestAnimationFrame(apply);
        }
        function up() {
          if (pending) { cancelAnimationFrame(pending); apply(); }
          document.removeEventListener("mousemove", move);
          document.removeEventListener("mouseup", up);
          document.body.classList.remove("col-resizing");
          try { localStorage.setItem(KEY, widths.map((w) => w + "px").join(" ")); } catch (_) {}
        }
        document.body.classList.add("col-resizing");
        document.addEventListener("mousemove", move);
        document.addEventListener("mouseup", up);
        apply();
      });
      /* keep clicks on the grip from sorting the column */
      h.addEventListener("click", (e) => e.stopPropagation());
    });
  })();

  /* ---------------- 2. album card size (Ctrl+wheel) ---------------- */
  (function initCardZoom() {
    const grid = document.getElementById("album-grid");
    if (!grid) return;
    const KEY = "mn.cardmin";
    let size = parseInt(localStorage.getItem(KEY), 10);
    if (!(size >= 120 && size <= 320)) size = 178;
    if (size !== 178) root.style.setProperty("--card-min", size + "px");

    let saveT = 0;
    grid.addEventListener("wheel", (e) => {
      if (!e.ctrlKey || uiLocked()) return;
      e.preventDefault();
      size = Math.max(120, Math.min(320, size + (e.deltaY < 0 ? 14 : -14)));
      root.style.setProperty("--card-min", size + "px");
      clearTimeout(saveT);
      saveT = setTimeout(() => {
        try { localStorage.setItem(KEY, String(size)); } catch (_) {}
      }, 300);
    }, { passive: false });
  })();

  return {};
});
/* self-boot */
if (window.MN) { try { MN.get("uiresize"); } catch (_) {} }
