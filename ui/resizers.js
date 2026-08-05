/* Resizable panels: drag the handles to resize the sidebar, now-playing panel,
 * and the player-bar height. Widths are stored on the #app element's CSS custom
 * properties (--side-w, --np-w, --player-h) and persisted to localStorage. */
(function () {
  const app = document.getElementById('app');
  if (!app) return;
  const root = document.documentElement;

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const px = (v) => v + 'px';

  // restore saved sizes
  try {
    const s = JSON.parse(localStorage.getItem('monatomic.panels') || '{}');
    if (s.side) root.style.setProperty('--side-w', px(s.side));
    if (s.np)   root.style.setProperty('--np-w',   px(s.np));
    if (s.ph)   root.style.setProperty('--player-h', px(s.ph));
  } catch (e) { /* ignore */ }

  function save() {
    const cs = getComputedStyle(root);
    localStorage.setItem('monatomic.panels', JSON.stringify({
      side: parseInt(cs.getPropertyValue('--side-w')) || 230,
      np:   parseInt(cs.getPropertyValue('--np-w'))   || 300,
      ph:   parseInt(cs.getPropertyValue('--player-h')) || 92,
    }));
  }

  /* rAF-coalesce the CSS-var write: writing a grid-column/row var relays the
     whole #app grid, and pointermove can fire faster than frames. Capture
     the latest value in the handler; commit at most one write per frame. */
  function makeDrag(bodyClass, compute) {
    return function (handle, varName, ...args) {
      handle.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        handle.setPointerCapture(e.pointerId);
        handle.classList.add('dragging');
        document.body.classList.add(bodyClass);
        let pending = 0, val = null;
        const flush = () => { pending = 0; if (val != null) root.style.setProperty(varName, px(val)); };
        const move = (ev) => {
          val = compute(ev, args);
          if (!pending) pending = requestAnimationFrame(flush);
        };
        const up = () => {
          if (pending) { cancelAnimationFrame(pending); flush(); }
          handle.releasePointerCapture(e.pointerId);
          handle.classList.remove('dragging');
          document.body.classList.remove(bodyClass);
          window.removeEventListener('pointermove', move);
          window.removeEventListener('pointerup', up);
          save();
        };
        window.addEventListener('pointermove', move, { passive: true });
        window.addEventListener('pointerup', up);
      });
    };
  }
  const dragCol = makeDrag('resizing', (ev, [fromLeft, min, max]) =>
    clamp(fromLeft ? ev.clientX : (window.innerWidth - ev.clientX), min, max));
  const dragRow = makeDrag('resizing-row', (ev, [min, max]) =>
    clamp(window.innerHeight - ev.clientY, min, max));

  const rSide = document.getElementById('resizer-side');
  const rNp   = document.getElementById('resizer-np');
  const rPlay = document.getElementById('resizer-player');
  if (rSide) dragCol(rSide, '--side-w', true,  170, 420);
  if (rNp)   dragCol(rNp,   '--np-w',   false, 220, 520);
  if (rPlay) dragRow(rPlay, '--player-h', 72, 220);
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("resizers", "1.0.0", [], function () { return window.MnResizers || {}; });
