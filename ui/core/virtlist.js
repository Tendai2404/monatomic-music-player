/* ============================================================
   VIRTUAL WINDOW ENGINE — core/virtlist.js            v1.0.0
   ------------------------------------------------------------
   Constant-DOM windowed rendering for arbitrarily large libraries:
   only the visible items (± buffer) exist as elements, absolutely
   positioned inside a spacer sized to the FULL count — the
   scrollbar is exact at any library size and memory stays flat.

   const v = MN.get("virtlist").create({
     scroller,            // the overflow:auto element
     content,             // positioned child that receives items
     itemH: 46,           // row height (px)
     itemW: 0,            // 0 = full-width rows; >0 = grid cards
     gap: 0,              // grid gap (px)
     axis: "y",           // "y" vertical | "x" horizontal strip
     buffer: 12,          // extra rows rendered either side
     total: () => N,      // item count (called every pass)
     get: (i) => obj|null,        // cached item or null (missing)
     fetch: (off, cnt) => {},     // request missing range (async)
     make: (obj, i) => Element,   // build one item element
   });
   v.refresh()   — recompute + re-render (data arrived / resize)
   v.reset()     — drop pooled DOM (sort/search changed)
   v.indexAtTop()— first visible index (scroll popup)
   v.cols()      — current column count
   ============================================================ */
MN.define("virtlist", "1.0.0", [], function () {
  "use strict";

  function create(cfg) {
    const scroller = cfg.scroller, content = cfg.content;
    const axis = cfg.axis || "y";
    let raf = 0;
    let lastFetchKey = "";

    content.style.position = "relative";

    function cols() {
      if (!cfg.itemW) return 1;
      const avail = (axis === "y" ? scroller.clientWidth : scroller.clientHeight)
                    - (cfg.pad || 0);
      return Math.max(1, Math.floor((avail + cfg.gap) / (cfg.itemW + cfg.gap)));
    }

    function pass() {
      raf = 0;
      const N = cfg.total();
      const C = cols();
      const rows = Math.ceil(N / C);
      const ih = (typeof cfg.itemH === "function") ? cfg.itemH() : cfg.itemH;
      const span = ih + (cfg.gap || 0);

      /* spacer: exact full extent */
      /* Optional inline inset (vertical grids only): an expanded panel below
         row `insetRow` reserves `insetH` px, pushing every later row down so
         nothing overlaps (album track-list expand). */
      const insetRow = (axis === "y" && cfg.insetRow != null) ? cfg.insetRow() : -1;
      const insetH = insetRow >= 0 ? (cfg.insetH ? cfg.insetH() : 0) : 0;
      const rowTop = (r) => r * span + (insetRow >= 0 && r > insetRow ? insetH : 0);

      /* READ all geometry BEFORE writing the spacer size — writing height
         then reading scrollTop/clientHeight forces a synchronous reflow
         every pass (per resize/scroll frame). Read first, write after. */
      const scrollPos = axis === "y" ? scroller.scrollTop : scroller.scrollLeft;
      const viewLen  = axis === "y" ? scroller.clientHeight : scroller.clientWidth;

      if (axis === "y") {
        content.style.height = (rows * span + (insetRow >= 0 ? insetH : 0)) + "px";
        content.style.width = "";
      } else {
        content.style.width = (rows * span) + "px";
        content.style.height = "";
      }
      /* account for the inset when mapping scroll position back to rows */
      const adjScroll = (axis === "y" && insetRow >= 0 &&
                         scrollPos > (insetRow + 1) * span)
                      ? scrollPos - insetH : scrollPos;
      const firstRow = Math.max(0, Math.floor(adjScroll / span) - cfg.buffer);
      const lastRow  = Math.min(rows - 1,
                        Math.ceil((adjScroll + viewLen) / span) + cfg.buffer);
      const first = firstRow * C;
      const last  = Math.min(N - 1, (lastRow + 1) * C - 1);

      /* fetch any missing stretch ONCE per distinct range */
      let missA = -1, missB = -1;
      for (let i = first; i <= last; i++) {
        if (!cfg.get(i)) { if (missA < 0) missA = i; missB = i; }
      }
      if (missA >= 0) {
        const key = missA + ":" + missB;
        if (key !== lastFetchKey) {
          lastFetchKey = key;
          cfg.fetch(missA, missB - missA + 1);
        }
      }

      /* render: reconcile by data-vidx (reuse untouched nodes) */
      const keep = {};
      for (let i = first; i <= last; i++) keep[i] = true;
      const stale = [];
      for (let el = content.firstElementChild; el; el = el.nextElementSibling) {
        const vi = el.dataset ? +el.dataset.vidx : NaN;
        if (!(vi >= first && vi <= last)) stale.push(el);
        else keep[vi] = el;   /* element already present */
      }
      stale.forEach((el) => el.remove());

      for (let i = first; i <= last; i++) {
        if (keep[i] !== true) continue;   /* node exists */
        const obj = cfg.get(i);
        if (!obj) continue;                /* not loaded yet */
        const el = cfg.make(obj, i);
        el.dataset.vidx = String(i);
        el.style.position = "absolute";
        const r = Math.floor(i / C), c = i % C;
        if (axis === "y") {
          el.style.top = rowTop(r) + "px";
          if (cfg.itemW) {
            el.style.left = (c * (cfg.itemW + cfg.gap)) + "px";
            el.style.width = cfg.itemW + "px";
          } else {
            el.style.left = "0"; el.style.right = "0";
          }
        } else {
          el.style.left = (r * span) + "px";
          el.style.top = (c * (cfg.itemW + cfg.gap)) + "px";
          if (cfg.itemW) el.style.height = cfg.itemW + "px";
        }
        content.appendChild(el);
      }
      if (cfg.after) cfg.after(first, last);
    }

    function schedule() { if (!raf) raf = requestAnimationFrame(pass); }

    scroller.addEventListener("scroll", schedule, { passive: true });
    let ro = null;
    if (window.ResizeObserver) {
      ro = new ResizeObserver(schedule);
      ro.observe(scroller);
    }

    return {
      refresh: schedule,
      reset() { content.innerHTML = ""; lastFetchKey = ""; schedule(); },
      indexAtTop() {
        const ih = (typeof cfg.itemH === "function") ? cfg.itemH() : cfg.itemH;
        const span = ih + (cfg.gap || 0);
        const pos = axis === "y" ? scroller.scrollTop : scroller.scrollLeft;
        return Math.floor(pos / span) * cols();
      },
      cols,
      /* row span (item height + gap) and the y of a given row's TOP, both
         honoring the current inline inset — used to place the expand panel. */
      span() {
        const ih = (typeof cfg.itemH === "function") ? cfg.itemH() : cfg.itemH;
        return ih + (cfg.gap || 0);
      },
      rowTopOf(row) {
        const ih = (typeof cfg.itemH === "function") ? cfg.itemH() : cfg.itemH;
        const sp = ih + (cfg.gap || 0);
        const ir = (axis === "y" && cfg.insetRow != null) ? cfg.insetRow() : -1;
        const ih2 = ir >= 0 ? (cfg.insetH ? cfg.insetH() : 0) : 0;
        return row * sp + (ir >= 0 && row > ir ? ih2 : 0);
      },
      destroy() {
        scroller.removeEventListener("scroll", schedule);
        if (ro) ro.disconnect();
      },
    };
  }

  return { create };
});
