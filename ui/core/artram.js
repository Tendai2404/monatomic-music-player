/* ============================================================
   RAM ART CACHE — core/artram.js                       v1.0.1
   ------------------------------------------------------------
   Keeps recently-seen album covers DECODED in memory so fast
   scrolling repaints instantly instead of re-reading/decoding
   from disk. Holds an LRU pool of Image objects (decoded via
   img.decode()); pinning them keeps Chromium's decoded-image
   cache warm for those URLs. Capacity is a user setting
   (mn.artram, default 500 covers; 0 disables).

   API: warm(url) · warmMany(urls) · cap()/setCap(n) · stats()
   ============================================================ */
MN.define("artram", "1.0.1", [], function () {
  "use strict";

  const pool = new Map();   /* url -> Image (Map preserves insertion = LRU) */
  /* CORRECTION: these Image objects have their src set, so Chromium keeps
     their DECODED bitmaps resident (not "encoded bytes" as previously
     claimed). At 256px RGBA that's ~256 KB decoded EACH, so a 2500 cap held
     ~650 MB of pixels and, paired with artwarm force-decoding the whole
     library, competed with scroll. artwarm v1.1.0 now warms only a bounded
     window, so a much smaller LRU covers it comfortably. User-overridable
     via mn.artram. */
  let capN = 400;
  try {
    const v = parseInt(localStorage.getItem("mn.artram"), 10);
    if (!isNaN(v) && v >= 0) capN = v;
  } catch (_) {}

  function evict() {
    while (pool.size > capN) {
      const oldest = pool.keys().next().value;
      /* drop the src so Chromium can release the DECODED bitmap now —
         a bare Map delete left it pinned until GC (up to ~GBs of pixels
         riding on GC timing at cap 500 with large covers) */
      const img = pool.get(oldest);
      if (img) img.src = "";
      pool.delete(oldest);
    }
  }

  function warm(url) {
    if (!url || capN === 0) return;
    if (pool.has(url)) {
      /* refresh LRU position */
      const img = pool.get(url);
      pool.delete(url);
      pool.set(url, img);
      return;
    }
    const img = new Image();
    img.decoding = "async";
    img.src = url;
    if (img.decode) img.decode().catch(() => {});
    pool.set(url, img);
    evict();
  }

  function warmMany(urls) {
    if (capN === 0 || !urls) return;
    /* stagger decodes so a fast scroll doesn't spike the main thread */
    let i = 0;
    (function step() {
      const end = Math.min(i + 8, urls.length);
      for (; i < end; i++) warm(urls[i]);
      if (i < urls.length) setTimeout(step, 16);
    })();
  }

  function cap() { return capN; }
  function setCap(n) {
    capN = Math.max(0, Math.min(5000, parseInt(n, 10) || 0));
    try { localStorage.setItem("mn.artram", String(capN)); } catch (_) {}
    evict();
  }

  function stats() { return { held: pool.size, cap: capN }; }

  return { warm, warmMany, cap, setCap, stats };
});
