/* ============================================================
   ELEMENT CUSTOMIZATION — core/custom.js               v1.0.0
   ------------------------------------------------------------
   Granular, persisted user control over individual UI elements:

   ZONES  (mn.zone.<name>): whole-region scale via CSS zoom —
     sidebar, tracks, player, nowplaying, topbar
   ROW HEIGHT (mn.rowh): track-list row height; the virtual list
     reads it through rowHeight(), so density is a live control.
   ELEMENTS (mn.custom.<key>): per-element scale / x / y offset /
     font multiplier, applied as transform + font-size so nothing
     else reflows. Reset restores defaults.

   API: zones, elements (catalogs) · getZone/setZone ·
        rowHeight/setRowHeight · getEl/setEl/resetEl ·
        onRowHeight(cb)
   ============================================================ */
MN.define("custom", "1.0.0", [], function () {
  "use strict";

  const ZONES = [
    { key: "sidebar",    label: "Sidebar",           sel: "#sidebar" },
    { key: "tracks",     label: "Library lists",     sel: "#view" },
    { key: "player",     label: "Player bar",        sel: "#player" },
    { key: "nowplaying", label: "Now-playing panel", sel: "#nowplaying" },
    { key: "topbar",     label: "Top bar",           sel: "#topbar" },
  ];
  const ELEMENTS = [
    { key: "brand",     label: "Logo / brand",       sel: ".brand" },
    { key: "search",    label: "Search box",         sel: ".search-wrap" },
    { key: "viewtitle", label: "View title",         sel: "#view-title" },
    { key: "transport", label: "Transport buttons",  sel: ".transport" },
    { key: "seek",      label: "Seek bar",           sel: ".seek" },
    { key: "vol",       label: "Volume control",     sel: ".pl-right-top" },
    { key: "outpills",  label: "Output pills",       sel: "#pl-out" },
    { key: "npart",     label: "Now-playing cover",  sel: "#np-art-stage" },
    { key: "npmeta",    label: "Now-playing titles", sel: ".np-meta" },
    { key: "npqueue",   label: "Up-next queue",      sel: "#np-queue" },
    { key: "stemdock",  label: "Stem mixer dock",    sel: "#stem-dock" },
    { key: "plinfo",    label: "Track info (player)",sel: ".pl-left" },
  ];
  const DEF_EL = { scale: 1, x: 0, y: 0, font: 1 };
  const rowCbs = [];
  const layoutCbs = [];   /* fired on ANY zone/element change (live refresh) */

  function lsGet(k, d) {
    try { const v = localStorage.getItem(k); return v == null ? d : v; }
    catch (_) { return d; }
  }
  function lsSet(k, v) { try { localStorage.setItem(k, String(v)); } catch (_) {} }

  /* ---- zones ---- */
  function getZone(key) { return parseFloat(lsGet("mn.zone." + key, "1")) || 1; }
  function setZone(key, v) {
    v = Math.min(1.5, Math.max(0.7, parseFloat(v) || 1));
    lsSet("mn.zone." + key, v);
    applyZone(key);
    layoutCbs.forEach((cb) => { try { cb(); } catch (_) {} });
  }
  function applyZone(key) {
    const z = ZONES.find((x) => x.key === key);
    if (!z) return;
    const n = document.querySelector(z.sel);
    if (!n) return;
    const v = getZone(key);
    /* identity => leave the element completely untouched (zoom:1 on the
       now-playing panel disturbed the WebGL canvas sizing) */
    n.style.zoom = (v === 1) ? "" : String(v);
  }

  /* ---- row height ---- */
  function rowHeight() {
    const v = parseInt(lsGet("mn.rowh", "46"), 10);
    return (v >= 34 && v <= 72) ? v : 46;
  }
  function setRowHeight(v) {
    v = Math.min(72, Math.max(34, parseInt(v, 10) || 46));
    lsSet("mn.rowh", v);
    document.documentElement.style.setProperty("--row-h", v + "px");
    rowCbs.forEach((cb) => { try { cb(v); } catch (_) {} });
  }
  function onRowHeight(cb) { rowCbs.push(cb); }

  /* ---- per-element overrides ---- */
  function getEl(key) {
    try {
      const raw = localStorage.getItem("mn.custom." + key);
      return raw ? Object.assign({}, DEF_EL, JSON.parse(raw)) : Object.assign({}, DEF_EL);
    } catch (_) { return Object.assign({}, DEF_EL); }
  }
  function setEl(key, cfg) {
    const c = Object.assign(getEl(key), cfg);
    lsSet("mn.custom." + key, JSON.stringify(c));
    applyEl(key);
    layoutCbs.forEach((cb) => { try { cb(); } catch (_) {} });
  }
  function resetEl(key) {
    try { localStorage.removeItem("mn.custom." + key); } catch (_) {}
    applyEl(key);
  }
  function applyEl(key) {
    const e = ELEMENTS.find((x) => x.key === key);
    if (!e) return;
    const n = document.querySelector(e.sel);
    if (!n) return;
    const c = getEl(key);
    const t = [];
    if (c.x || c.y) t.push("translate(" + (c.x || 0) + "px," + (c.y || 0) + "px)");
    if (c.scale !== 1) t.push("scale(" + c.scale + ")");
    const tv = t.join(" ");
    if (tv) n.style.transform = tv; else n.style.removeProperty("transform");
    if (c.font !== 1) n.style.fontSize = (c.font * 100) + "%";
    else n.style.removeProperty("font-size");
  }

  /* apply everything persisted at boot */
  function applyAll() {
    ZONES.forEach((z) => applyZone(z.key));
    ELEMENTS.forEach((e) => applyEl(e.key));
    document.documentElement.style.setProperty("--row-h", rowHeight() + "px");
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", applyAll);
  } else {
    applyAll();
  }

  function resetAll() {
    try {
      ZONES.forEach((z) => localStorage.removeItem("mn.zone." + z.key));
      ELEMENTS.forEach((e) => localStorage.removeItem("mn.custom." + e.key));
      localStorage.removeItem("mn.rowh");
    } catch (_) {}
    /* clear inline styles then re-apply (now all identity = untouched) */
    ZONES.concat(ELEMENTS).forEach((x) => {
      const n = document.querySelector(x.sel);
      if (n) { n.style.removeProperty("zoom"); n.style.removeProperty("transform"); n.style.removeProperty("font-size"); }
    });
    applyAll();
    rowCbs.forEach((cb) => { try { cb(rowHeight()); } catch (_) {} });
  }

  function onLayout(cb) { layoutCbs.push(cb); }

  return { zones: ZONES, elements: ELEMENTS, resetAll, onLayout,
           getZone, setZone, rowHeight, setRowHeight, onRowHeight,
           getEl, setEl, resetEl, applyAll };
});
