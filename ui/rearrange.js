/* ============================================================
   MONATOMIC — manual UI rearrangement.
   Generic HTML5 drag-to-reorder for item containers; the order
   persists in localStorage and is re-applied on every render.

   window.MnRearrange.attach(container, itemSelector, storageKey)
     - every matching child becomes draggable
     - saved order is applied immediately and after future
       re-renders via attach()'s returned reapply()
   Keys used: mn.order.<storageKey>
   ============================================================ */
(function () {
  "use strict";

  function idOf(el) {
    /* stable identity: explicit data-key, else data-view, else trimmed text */
    return el.dataset.key || el.dataset.view || (el.textContent || "").trim().slice(0, 40);
  }

  function saveOrder(container, itemSelector, storageKey) {
    const ids = Array.from(container.querySelectorAll(itemSelector)).map(idOf);
    try { localStorage.setItem("mn.order." + storageKey, JSON.stringify(ids)); } catch (_) {}
  }

  function applyOrder(container, itemSelector, storageKey) {
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem("mn.order." + storageKey) || "null"); } catch (_) {}
    if (!Array.isArray(saved) || !saved.length) return;
    const items = Array.from(container.querySelectorAll(itemSelector));
    const byId = new Map(items.map((el) => [idOf(el), el]));
    for (const id of saved) {
      const el = byId.get(id);
      if (el) container.appendChild(el);   /* move into saved order */
    }
    /* any new items not in the saved list keep their natural (appended) spot */
  }

  function attach(container, itemSelector, storageKey) {
    if (!container) return () => {};
    let dragEl = null;

    function wire() {
      Array.from(container.querySelectorAll(itemSelector)).forEach((el) => {
        if (el.dataset.mnDraggable) return;
        el.dataset.mnDraggable = "1";
        el.draggable = true;
        el.addEventListener("dragstart", (ev) => {
          /* UI lock (topbar 🔒): rearranging is disabled while locked */
          if (window.MnUILocked) { ev.preventDefault(); return; }
          dragEl = el;
          el.classList.add("dragging-item");
          ev.dataTransfer.effectAllowed = "move";
          try { ev.dataTransfer.setData("text/plain", idOf(el)); } catch (_) {}
        });
        el.addEventListener("dragend", () => {
          el.classList.remove("dragging-item");
          dragEl = null;
          saveOrder(container, itemSelector, storageKey);
        });
        el.addEventListener("dragover", (ev) => {
          if (!dragEl || dragEl === el) return;
          ev.preventDefault();
          const r = el.getBoundingClientRect();
          const horizontal = r.width > r.height * 1.4;
          const before = horizontal
            ? (ev.clientX - r.left) < r.width / 2
            : (ev.clientY - r.top) < r.height / 2;
          el.parentNode.insertBefore(dragEl, before ? el : el.nextSibling);
        });
      });
    }

    applyOrder(container, itemSelector, storageKey);
    wire();
    /* re-apply + re-wire after dynamic re-renders */
    return function reapply() { applyOrder(container, itemSelector, storageKey); wire(); };
  }

  window.MnRearrange = { attach };
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("rearrange", "1.0.0", [], function () { return window.MnRearrange || {}; });
