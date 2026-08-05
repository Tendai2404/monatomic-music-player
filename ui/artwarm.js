/* ============================================================
   ART PRE-WARMER — artwarm.js                          v1.1.0
   ------------------------------------------------------------
   Warms covers in a bounded WINDOW around the scroll position,
   not the whole library. The <img loading=lazy decoding=async>
   with intrinsic width/height already lets Chromium decode
   near-viewport covers off-thread and evict under memory
   pressure; force-decoding all ~2000 covers into a 2500-image
   pool (~650 MB of DECODED pixels) via main-thread img.decode()
   competed with scroll and pinned huge memory. v1.1.0 only warms
   WARM_AHEAD covers ahead of the topmost visible card, so cost
   is O(window) no matter how large the library.

   Self-healing: a library reset / search / filter that rebuilds
   state.albCards restarts the sweep automatically.
   ============================================================ */
MN.define("artwarm", "1.1.0", ["core", "artram"], function () {
  "use strict";

  const core = MN.get("core");
  const ram  = MN.get("artram");

  const WARM_AHEAD = 60;   /* covers to keep warm ahead of the viewport */
  let warmedTo = 0;        /* highest album index warmed so far          */
  let lastLen = 0;         /* detects list rebuilds                      */

  /* first visible album index — estimated from the grid scroll position;
     falls back to 0 (top) when the grid isn't scrollable/available */
  function firstVisible(cards) {
    const grid = document.getElementById("alb-cards") ||
                 document.querySelector(".album-grid");
    if (!grid || !cards.length) return 0;
    const card = grid.querySelector(".album-card");
    if (!card) return 0;
    /* rows = ceil(count / cols); index ≈ (scrollTop / rowHeight) * cols */
    const cols = Math.max(1, Math.round(grid.clientWidth / (card.offsetWidth + 22)));
    const rowH = card.offsetHeight + 22;
    if (rowH <= 0) return 0;
    const row = Math.floor(grid.scrollTop / rowH);
    return Math.max(0, Math.min(cards.length - 1, row * cols));
  }

  function tick() {
    /* Low-power mode: no pre-warm at all — the browser's own lazy decode
       handles it. Keep the cheap watch alive so toggling off resumes. */
    if (window.__mnLowPower && window.__mnLowPower()) {
      setTimeout(tick, 5000);
      return;
    }
    const cards = (core.state && core.state.albCards) || [];
    if (cards.length < lastLen) warmedTo = 0;   /* rebuilt: restart */
    lastLen = cards.length;

    if (cards.length) {
      const target = Math.min(cards.length, firstVisible(cards) + WARM_AHEAD);
      if (warmedTo < target) {
        const batch = [];
        for (; warmedTo < target && batch.length < 12; warmedTo++) {
          const a = cards[warmedTo];
          if (a && a.art) batch.push(a.art);
        }
        if (batch.length) ram.warmMany(batch);
        setTimeout(tick, 120);      /* still catching up to the window */
        return;
      }
    }
    /* window satisfied — poll cheaply for scroll movement / new pages */
    setTimeout(tick, 600);
  }

  /* start a few seconds after boot so first paint + eager load win the disk */
  setTimeout(tick, 3500);

  return { stats: () => ({ warmedTo, of: lastLen }) };
});
/* self-boot */
if (window.MN) { try { MN.get("artwarm"); } catch (_) {} }
