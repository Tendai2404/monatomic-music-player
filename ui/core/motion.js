/* ============================================================
   MOTION SYSTEM — core/motion.js                       v2.0.0
   ------------------------------------------------------------
   Extensive, granular motion design. Every visual behaviour is a
   CHANNEL: a user preference (persisted under mn.motion.*) applied
   as a data-attribute on <html> so CSS variants switch with zero JS
   in the hot path. The REGISTRY below carries labels/hints/options,
   and the Settings → Animations panel auto-generates one control per
   channel from it — per-element granularity by construction.

   Design language: Claude-web-inspired — springy easings, staggered
   fade-up entrances, hover lifts and glows, shimmer placeholders,
   animated underlines, breathing accents while playing, count-ups,
   graceful exits. HARD RULES: transform/opacity only in hot paths,
   nothing animates at idle (continuous effects are gated to
   body.playing or :hover), and everything is subordinate to
   html.no-anim + prefers-reduced-motion.

   API (superset of v1.3 — nothing removed):
     get/set(key)                 — read/write a channel (applies live)
     registry                     — [{key,label,hint,group,options|slider}]
     onAlbumStyle(cb), attachRolodex(scroller)
     reduced(), stagger(nodes[,max]), pop(el), bump(el), close(el,done)
     countUp(el, to[, suffix])    — animated number roll-up (counters ch.)
     attachTilt(container, sel)   — pointer-tracking 3D tilt (tilt3d ch.)
     playing(on)                  — body.playing sync (gates live effects)
   ============================================================ */
MN.define("motion", "2.0.0", [], function () {
  "use strict";

  /* ---------------- channel registry (drives the settings UI) ------------- */
  const REG = [
    /* — Entrances — */
    { key: "listIn",   attr: "moList",   group: "Entrances", label: "List & grid entrances",
      hint: "How freshly loaded rows, cards and results appear",
      options: [["rise","Rise"],["fade","Fade"],["scale","Scale"],["slide","Slide"],["depth","3D depth"],["none","None"]],
      def: "rise" },
    { key: "staggerAmt", attr: "moStagger", group: "Entrances", label: "Entrance stagger",
      hint: "Per-item delay cascade when a batch appears",
      options: [["subtle","Subtle"],["normal","Normal"],["dramatic","Dramatic"],["off","Off"]],
      def: "normal" },
    { key: "artIn",    attr: "moArtin",  group: "Entrances", label: "Album art reveal",
      hint: "How covers appear once loaded",
      options: [["fade","Crossfade"],["rise","Fade up"],["zoom","Zoom in"],["instant","Instant"]],
      def: "fade" },
    { key: "reveal",   attr: "moReveal", group: "Entrances", label: "Scroll reveal",
      hint: "Sections fade up as they scroll into view (settings, panels)",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "shimmer",  attr: "moShimmer", group: "Entrances", label: "Loading shimmer",
      hint: "Soft sheen sweep across placeholders while content loads",
      options: [["on","On"],["off","Off"]], def: "on" },

    /* — Interaction — */
    { key: "hover",    attr: "moHover",  group: "Interaction", label: "Hover response",
      hint: "Cards and buttons under the pointer",
      options: [["subtle","Subtle"],["lift","Lift"],["glow","Glow"],["off","Off"]],
      def: "subtle" },
    { key: "tilt3d",   attr: "moTilt",   group: "Interaction", label: "3D cover tilt",
      hint: "Album covers tilt toward the cursor while hovered",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "spotlight", attr: "moSpotlight", group: "Interaction", label: "Cursor spotlight",
      hint: "A soft light follows the cursor across hovered cards",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "parallax", attr: "moParallax", group: "Interaction", label: "Cover parallax",
      hint: "The artwork drifts counter to the cursor for depth",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "magnetic", attr: "moMagnetic", group: "Interaction", label: "Magnetic buttons",
      hint: "Transport buttons lean toward the approaching cursor",
      options: [["on","On"],["off","Off"]], def: "off" },
    { key: "likeBurst", attr: "moLikeburst", group: "Interaction", label: "Like burst",
      hint: "A particle burst when you like a track",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "artZoom",  attr: "moArtzoom", group: "Interaction", label: "Cover zoom on hover",
      hint: "The artwork itself eases larger inside a hovered card",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "press",    attr: "moPress",  group: "Interaction", label: "Press feedback",
      hint: "Tactile push-down on buttons and rows",
      options: [["scale","Scale"],["spring","Spring"],["off","Off"]], def: "scale" },
    { key: "ripple",   attr: "moRipple", group: "Interaction", label: "Ripple on press",
      hint: "Emanating wave from the pointer on primary actions",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "underline", attr: "moUnder", group: "Interaction", label: "Tab underlines",
      hint: "Animated underline on tabs and section headers",
      options: [["slide","Slide in"],["grow","Grow"],["off","Off"]], def: "slide" },
    { key: "micro",    attr: "moMicro",  group: "Interaction", label: "Micro-motion",
      hint: "Toggle pops, value bumps, checkmark pops",
      options: [["on","On"],["off","Off"]], def: "on" },

    /* — Transitions — */
    { key: "view",     attr: "moView",   group: "Transitions", label: "View transitions",
      hint: "Switching library views and settings tabs",
      options: [["slide","Slide"],["fade","Fade"],["zoom","Zoom"],["cinematic","Cinematic"],["none","None"]],
      def: "slide" },
    { key: "overlay",  attr: "moOverlay", group: "Transitions", label: "Overlays & pickers",
      hint: "EQ, menus, dialogs entering and leaving",
      options: [["spring","Spring"],["zoom","Zoom"],["fade","Fade"]], def: "spring" },
    { key: "modalBlur", attr: "moBlur",  group: "Transitions", label: "Backdrop blur",
      hint: "Frosted-glass blur behind overlays (GPU permitting)",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "toastIn",  attr: "moToast",  group: "Transitions", label: "Toasts & notices",
      hint: "How toasts and status pills arrive",
      options: [["spring","Spring"],["slide","Slide"],["fade","Fade"]], def: "spring" },
    { key: "expand",   attr: "moExpand", group: "Transitions", label: "Panel expansion",
      hint: "Album expand panel and collapsible sections",
      options: [["spring","Spring"],["soft","Soft"],["off","Instant"]], def: "spring" },
    { key: "navGlide", attr: "moNavglide", group: "Transitions", label: "Sidebar indicator",
      hint: "The active-item marker glides between nav entries",
      options: [["on","On"],["off","Off"]], def: "on" },

    /* — While playing — */
    { key: "glowPulse", attr: "moGlow",  group: "While playing", label: "Playing glow",
      hint: "Breathing accent on the playing card and play button (only while audio plays)",
      options: [["on","On"],["off","Off"]], def: "off" },
    { key: "seekPulse", attr: "moSeekpulse", group: "While playing", label: "Seek knob pulse",
      hint: "Gentle pulse on the playhead while playing",
      options: [["on","On"],["off","Off"]], def: "off" },
    { key: "gradFlow", attr: "moGradflow", group: "While playing", label: "Accent gradient flow",
      hint: "Accent gradients slowly drift on active elements",
      options: [["playing","While playing"],["hover","On hover"],["off","Off"]], def: "playing" },
    { key: "playMorph", attr: "moPlaymorph", group: "While playing", label: "Play button morph",
      hint: "Play/pause glyph rotates and springs on toggle",
      options: [["on","On"],["off","Off"]], def: "off" },
    { key: "queueSlide", attr: "moQueueslide", group: "While playing", label: "Queue motion",
      hint: "Up-next entries slide as the queue changes",
      options: [["on","On"],["off","Off"]], def: "on" },

    /* — Text & numbers — */
    { key: "artFloat", attr: "moArtfloat", group: "While playing", label: "Floating cover",
      hint: "The now-playing artwork gently floats while audio plays",
      options: [["on","On"],["off","Off"]], def: "on" },

    { key: "counters", attr: "moCounters", group: "Text & numbers", label: "Count-up numbers",
      hint: "Library counts roll up to their value",
      options: [["on","On"],["off","Off"]], def: "on" },
    { key: "titleFx", attr: "moTitlefx", group: "Text & numbers", label: "Title changes",
      hint: "How the view title switches text",
      options: [["scramble","Scramble"],["slide","Slide"],["off","Instant"]], def: "slide" },
    { key: "sheen",    attr: "moSheen",  group: "Text & numbers", label: "Brand sheen",
      hint: "A light sweep across the wordmark on hover",
      options: [["on","On"],["off","Off"]], def: "on" },

    /* — Global feel — */
    { key: "springiness", attr: "moSpring", group: "Global feel", label: "Easing personality",
      hint: "The character of every curve — calm, balanced, or playful overshoot",
      options: [["soft","Soft"],["normal","Balanced"],["bouncy","Bouncy"]], def: "normal" },
    { key: "intensity", group: "Global feel", label: "Speed",
      hint: "Global duration scale — lower is snappier",
      slider: { min: 50, max: 150, step: 10 }, def: 1 },
  ];

  /* view mode lives with motion for legacy reasons; not in the settings grid */
  const EXTRA = [{ key: "albumStyle", attr: "moAlbum", def: "grid" }];

  const ALL = REG.concat(EXTRA);
  const state = {};
  const albumCbs = [];

  ALL.forEach((c) => {
    let v = null;
    try { v = localStorage.getItem("mn.motion." + c.key); } catch (_) {}
    state[c.key] = v == null ? c.def : (c.key === "intensity" ? parseFloat(v) : v);
    if (c.key === "intensity" && !(state[c.key] > 0)) state[c.key] = 1;
  });

  /* easing personalities (Claude-web feel: springy but composed) */
  const EASINGS = {
    soft:   { e: "cubic-bezier(.25,.1,.25,1)", o: "cubic-bezier(0,0,.2,1)",
              s: "cubic-bezier(.25,1.05,.35,1)" },
    normal: { e: "cubic-bezier(.4,0,.2,1)",    o: "cubic-bezier(0,0,.2,1)",
              s: "cubic-bezier(.34,1.3,.44,1)" },
    bouncy: { e: "cubic-bezier(.34,1.2,.5,1)", o: "cubic-bezier(.18,1.4,.4,1)",
              s: "cubic-bezier(.34,1.7,.44,1.1)" },
  };

  function apply() {
    const root = document.documentElement;
    ALL.forEach((c) => { if (c.attr) root.dataset[c.attr] = String(state[c.key]); });
    const m = state.intensity;
    root.style.setProperty("--t-fast", Math.round(110 * m) + "ms var(--ease)");
    root.style.setProperty("--t-med",  Math.round(220 * m) + "ms var(--ease)");
    root.style.setProperty("--t-slow", Math.round(360 * m) + "ms var(--ease)");
    const ez = EASINGS[state.springiness] || EASINGS.normal;
    root.style.setProperty("--ease", ez.e);
    root.style.setProperty("--ease-out", ez.o);
    root.style.setProperty("--ease-spring", ez.s);
    /* stagger step feeds the CSS delay calc */
    const st = { subtle: 14, normal: 26, dramatic: 46, off: 0 }[state.staggerAmt];
    root.style.setProperty("--stagger-ms", (st == null ? 26 : st) + "ms");
  }
  apply();

  /* ---- reduced-motion gate ---- */
  const rmq = window.matchMedia ? matchMedia("(prefers-reduced-motion: reduce)") : null;
  function reduced() {
    return (rmq && rmq.matches) ||
           document.documentElement.classList.contains("no-anim");
  }

  /* ---- entrance stagger (first bind only; never per scroll frame) ---- */
  function stagger(nodes, max) {
    if (reduced() || state.listIn === "none" || state.staggerAmt === "off" || !nodes) return;
    const n = Math.min(nodes.length, max || 14);
    for (let i = 0; i < n; i++) {
      nodes[i].classList.add("stagger");
      nodes[i].style.setProperty("--i", String(i));
    }
  }

  /* ---- one-shot micro feedback ---- */
  function oneShot(el, cls) {
    if (!el || reduced() || state.micro === "off") return;
    el.classList.remove(cls);
    void el.offsetWidth;
    el.classList.add(cls);
  }
  function pop(el)  { oneShot(el, "mo-pop"); }
  function bump(el) { oneShot(el, "mo-bump"); }

  /* ---- graceful overlay dismissal ---- */
  function close(el, done) {
    done = done || function () {};
    if (!el) { done(); return; }
    if (el.classList.contains("mo-out")) return;
    if (reduced()) { done(); return; }
    let fin = false, tm = 0;
    function end() {
      if (fin) return;
      fin = true;
      clearTimeout(tm);
      el.removeEventListener("animationend", onEnd);
      el.classList.remove("mo-out");
      done();
    }
    function onEnd(e) {
      if (e && String(e.animationName).indexOf("-out") < 0) return;
      end();
    }
    el.addEventListener("animationend", onEnd);
    tm = setTimeout(end, 320);
    el.classList.add("mo-out");
  }

  /* ---- ripple emanation (delegated, spawn-only, self-cleaning) ---- */
  const RIPPLE_SEL = ".btn,.tbtn,.mm-btn,.mm-tab,.nav-item,.chip:not(.ro)," +
                     ".btn-mini,.kind-card,.vm-btn,.aex-play,.facet-card," +
                     ".stab,.cs-card,.stem-preset-chip,.pl-picker-item";
  document.addEventListener("pointerdown", (ev) => {
    if (state.ripple !== "on" || reduced()) return;
    if (ev.button !== 0) return;
    const host = ev.target && ev.target.closest ? ev.target.closest(RIPPLE_SEL) : null;
    if (!host || host.disabled) return;
    const r = host.getBoundingClientRect();
    if (!r.width || r.width > 600) return;
    host.classList.add("mo-ripple-host");
    const clip = document.createElement("span");
    clip.className = "mo-ripple-clip";
    const wave = document.createElement("span");
    wave.className = "mo-ripple";
    const d = Math.ceil(Math.max(r.width, r.height) * 2.1);
    wave.style.width = wave.style.height = d + "px";
    wave.style.left = (ev.clientX - r.left - d / 2) + "px";
    wave.style.top = (ev.clientY - r.top - d / 2) + "px";
    clip.appendChild(wave);
    host.appendChild(clip);
    let done = false;
    const gone = () => { if (!done) { done = true; clip.remove(); } };
    wave.addEventListener("animationend", gone, { once: true });
    setTimeout(gone, 900);
  }, { passive: true });

  /* ---- count-up numbers (counters channel). One-shot rAF roll from the
     element's current shown value to `to`; ~500ms, eased. Event-driven —
     runs only when a count actually changes, never at idle. ---- */
  function countUp(el, to, suffix) {
    if (!el) return;
    to = +to || 0;
    suffix = suffix || "";
    if (reduced() || state.counters !== "on") { el.textContent = to.toLocaleString() + suffix; return; }
    const from = parseInt(String(el.textContent).replace(/[^0-9]/g, ""), 10) || 0;
    if (from === to) { el.textContent = to.toLocaleString() + suffix; return; }
    const t0 = performance.now(), dur = 500;
    if (el._moCntRaf) cancelAnimationFrame(el._moCntRaf);
    function tick(t) {
      const p = Math.min(1, (t - t0) / dur);
      const e = 1 - Math.pow(1 - p, 3);            /* easeOutCubic */
      el.textContent = Math.round(from + (to - from) * e).toLocaleString() + suffix;
      el._moCntRaf = p < 1 ? requestAnimationFrame(tick) : 0;
    }
    el._moCntRaf = requestAnimationFrame(tick);
  }

  /* ---- pointer-tracking 3D tilt on covers (tilt3d channel). Delegated:
     listeners attach once per container; rAF-throttled transform writes run
     ONLY while a pointer moves over a matching card, reset on leave. Zero
     idle cost; disabled under rolodex/coverflow (they own card transforms). */
  function attachTilt(container, sel) {
    if (!container) return;
    let raf = 0, target = null, lx = 0, ly = 0;
    function ok() {
      return (state.tilt3d === "on" || state.spotlight === "on" ||
              state.parallax === "on") && !reduced() &&
             state.albumStyle !== "rolodex" && state.albumStyle !== "coverflow";
    }
    function write() {
      raf = 0;
      if (!target) return;
      const r = target.getBoundingClientRect();
      if (!r.width) return;
      const px = (lx - r.left) / r.width - 0.5;    /* -0.5 .. 0.5 */
      const py = (ly - r.top) / r.height - 0.5;
      if (state.tilt3d === "on") {
        target.style.transform =
          "perspective(700px) rotateY(" + (px * 7).toFixed(2) + "deg)" +
          " rotateX(" + (-py * 7).toFixed(2) + "deg) translateZ(4px)";
      }
      /* spotlight + parallax read these in CSS (percent within the card) */
      target.style.setProperty("--mx", ((px + 0.5) * 100).toFixed(1) + "%");
      target.style.setProperty("--my", ((py + 0.5) * 100).toFixed(1) + "%");
      target.style.setProperty("--pxr", px.toFixed(3));
      target.style.setProperty("--pyr", py.toFixed(3));
    }
    container.addEventListener("pointermove", (ev) => {
      if (!ok()) return;
      const card = ev.target && ev.target.closest ? ev.target.closest(sel) : null;
      if (card !== target) {
        if (target) { target.style.transform = ""; target.classList.remove("mo-tilting"); }
        target = card;
        if (target) target.classList.add("mo-tilting");
      }
      if (!target) return;
      lx = ev.clientX; ly = ev.clientY;
      if (!raf) raf = requestAnimationFrame(write);
    }, { passive: true });
    container.addEventListener("pointerleave", () => {
      if (target) { target.style.transform = ""; target.classList.remove("mo-tilting"); }
      target = null;
    }, { passive: true });
  }

  /* ---- magnetic buttons (magnetic channel): controls lean toward the
     cursor within a small radius. Delegated pointermove on the container,
     rAF-throttled; every touched element resets on container leave. Zero
     idle cost — writes happen only while the pointer moves over it. ---- */
  function attachMagnetic(container, sel) {
    if (!container) return;
    let raf = 0, lx = 0, ly = 0;
    const touched = new Set();
    function write() {
      raf = 0;
      if (state.magnetic !== "on" || reduced()) return;
      const els = container.querySelectorAll(sel);
      for (const el of els) {
        const r = el.getBoundingClientRect();
        if (!r.width) continue;
        const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
        const dx = lx - cx, dy = ly - cy;
        const dist = Math.hypot(dx, dy);
        const R = 90;                       /* attraction radius (px) */
        if (dist < R) {
          const pull = (1 - dist / R) * 5;  /* max 5px lean */
          el.style.transform =
            "translate(" + (dx / dist * pull || 0).toFixed(1) + "px," +
                           (dy / dist * pull || 0).toFixed(1) + "px)";
          touched.add(el);
        } else if (touched.has(el)) {
          el.style.transform = "";
          touched.delete(el);
        }
      }
    }
    container.addEventListener("pointermove", (ev) => {
      if (state.magnetic !== "on" || reduced()) return;
      lx = ev.clientX; ly = ev.clientY;
      if (!raf) raf = requestAnimationFrame(write);
    }, { passive: true });
    container.addEventListener("pointerleave", () => {
      for (const el of touched) el.style.transform = "";
      touched.clear();
    }, { passive: true });
  }

  /* ---- like burst (likeBurst channel): one-shot particle emanation from
     an element. Spawns a clipped overlay of small accent particles, each
     flying out on its own --bx/--by vector; self-removes on animationend. */
  function burst(host) {
    if (!host || reduced() || state.likeBurst !== "on") return;
    const wrap = document.createElement("span");
    wrap.className = "mo-burst";
    for (let i = 0; i < 10; i++) {
      const p = document.createElement("span");
      p.className = "mo-burst-p";
      const a = (Math.PI * 2 * i) / 10 + (Math.random() - 0.5) * 0.5;
      const d = 22 + Math.random() * 16;
      p.style.setProperty("--bx", (Math.cos(a) * d).toFixed(1) + "px");
      p.style.setProperty("--by", (Math.sin(a) * d).toFixed(1) + "px");
      p.style.animationDelay = (Math.random() * 60) + "ms";
      wrap.appendChild(p);
    }
    host.appendChild(wrap);
    let done = false;
    const gone = () => { if (!done) { done = true; wrap.remove(); } };
    wrap.addEventListener("animationend", gone);
    setTimeout(gone, 900);
  }

  /* ---- title scramble (titleFx channel): one-shot decode-style text
     transition (~360ms rAF, event-driven only on title CHANGE). ---- */
  const SCRAMBLE_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789·";
  function scramble(el, text) {
    text = String(text == null ? "" : text);
    if (!el) return;
    if (reduced() || state.titleFx !== "scramble") { el.textContent = text; return; }
    if (el._moScrRaf) cancelAnimationFrame(el._moScrRaf);
    const t0 = performance.now(), dur = 360;
    function tick(t) {
      const p = Math.min(1, (t - t0) / dur);
      const keep = Math.floor(text.length * p);
      let out = text.slice(0, keep);
      for (let i = keep; i < text.length; i++) {
        const c = text[i];
        out += (c === " " || c === "·") ? c
             : SCRAMBLE_CHARS[(Math.random() * SCRAMBLE_CHARS.length) | 0];
      }
      el.textContent = out;
      el._moScrRaf = p < 1 ? requestAnimationFrame(tick) : 0;
    }
    el._moScrRaf = requestAnimationFrame(tick);
  }

  /* ---- playing-state gate: continuous effects (glow, gradient drift,
     seek pulse) run only under body.playing — audio is active, so nothing
     ever animates at true idle. app.js calls this from the now handler. */
  function playing(on) {
    document.body.classList.toggle("playing", !!on);
  }

  /* ---- scroll reveal (reveal channel): one-shot fade-up for NON-virtual
     content (settings sections, MM panes, stats). Elements register via
     class .mo-reveal; the observer unobserves after first reveal. ---- */
  let revealIO = null;
  function revealScan(rootEl) {
    if (reduced() || state.reveal !== "on") return;
    if (!("IntersectionObserver" in window)) return;
    if (!revealIO) {
      revealIO = new IntersectionObserver((ents) => {
        for (const en of ents) {
          if (en.isIntersecting) {
            en.target.classList.add("mo-revealed");
            revealIO.unobserve(en.target);
          }
        }
      }, { threshold: 0.06 });
    }
    (rootEl || document).querySelectorAll(".mo-reveal:not(.mo-revealed)")
      .forEach((el) => revealIO.observe(el));
  }

  function get(k) { return state[k]; }
  function set(k, v) {
    if (!(k in state)) return;
    state[k] = (k === "intensity") ? (parseFloat(v) || 1) : v;
    try { localStorage.setItem("mn.motion." + k, String(state[k])); } catch (_) {}
    apply();
    if (k === "albumStyle") albumCbs.forEach((cb) => { try { cb(state.albumStyle); } catch (_) {} });
  }
  function onAlbumStyle(cb) { albumCbs.push(cb); }

  /* ---- Rolodex (unchanged from v1.3) ---- */
  function attachRolodex(scroller) {
    let raf = 0;
    let touched = [];
    let cardList = null, cardH = 0, lastCount = -1;
    function snapshot() {
      cardList = scroller.querySelectorAll(".album-card");
      if (cardList.length && !cardH) {
        for (const c of cardList) { const h = c.offsetHeight; if (h > 0) { cardH = h; break; } }
      }
      lastCount = cardList.length;
    }
    function clearTouched() {
      for (const c of touched) { c.style.transform = ""; c.style.opacity = ""; }
      touched = [];
    }
    function pass() {
      raf = 0;
      if (state.albumStyle !== "rolodex") { clearTouched(); return; }
      const top = scroller.scrollTop, vh = scroller.clientHeight;
      const mid = top + vh / 2;
      const lo = top - vh, hi = top + vh * 2;
      if (!cardList || cardList.length !== lastCount) snapshot();
      const cards = cardList || scroller.querySelectorAll(".album-card");
      const h0 = cardH || 260;
      const inWin = [];
      for (let i = 0; i < cards.length; i++) {
        const c = cards[i], y = c.offsetTop;
        if (y > hi) break;
        const h = c.offsetHeight || h0;
        if (y + h < lo) continue;
        inWin.push([c, y + h / 2]);
      }
      const prev = touched; touched = [];
      const seen = new Set();
      for (const [c, cy] of inWin) {
        const d = (cy - mid) / (vh * 0.75);
        const rot = Math.max(-38, Math.min(38, d * 38));
        const z = -Math.abs(d) * 90;
        c.style.transform =
          "perspective(900px) rotateX(" + (-rot) + "deg) translateZ(" + z + "px)";
        c.style.opacity = String(Math.max(0.35, 1 - Math.abs(d) * 0.5));
        touched.push(c); seen.add(c);
      }
      for (const c of prev) {
        if (!seen.has(c)) { c.style.transform = ""; c.style.opacity = ""; }
      }
    }
    function kick() { if (!raf) raf = requestAnimationFrame(pass); }
    scroller.addEventListener("scroll", kick, { passive: true });
    if (window.ResizeObserver) {
      new ResizeObserver(() => { cardH = 0; cardList = null; kick(); }).observe(scroller);
    }
    albumCbs.push(kick);
    return kick;
  }

  return { get, set, onAlbumStyle, attachRolodex, reduced, stagger, pop, bump,
           close, countUp, attachTilt, attachMagnetic, burst, scramble,
           playing, revealScan,
           registry: REG, channels: ALL.map((c) => c.key), defaults: {} };
});
