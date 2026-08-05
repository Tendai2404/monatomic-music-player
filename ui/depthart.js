/* ============================================================
   MONATOMIC — volumetric (depth-mapped) album art renderer.

   WebGL port of the Android app's VolumetricAlbumArt: the cover is
   rendered as a depth-displaced triangle mesh through a perspective
   camera, so the subject pops out and self-occludes as it tilts.

   Parameters mirror the Android implementation:
     grid 96x96, DEPTH_SCALE 0.45, EDGE_MARGIN 0.07, FOV 38 deg,
     camera distance 3.1, unlit texturing (original colors).

   PERFORMANCE ARCHITECTURE:
     The mesh renders on a WEB WORKER driving an OffscreenCanvas, so
     the GL loop never competes with the DOM main thread (scrolling
     stays free). The main thread only posts tilt/motion/size events —
     the idle "showcase" orbit is computed inside the worker, so zero
     per-frame messages cross threads. Falls back to a main-thread
     renderer when OffscreenCanvas is unavailable. Either path pauses
     completely when the panel is hidden / no cover is loaded.

   Usage (from app.js):
     MnDepthArt.mount(canvas)                    once
     MnDepthArt.setSources(artUrl, depthUrl, onReady(ok))
     MnDepthArt.setTilt(x, y, pointerInside)     event-driven (-1..1)
     MnDepthArt.setMotion("pointer"|"showcase"|"off")
     MnDepthArt.setActive(bool)                  panel visibility gate
   ============================================================ */
(function () {
  "use strict";

  /* ------------------------------------------------------------------
     RENDER CORE — self-contained factory (no outer-scope captures) so it
     can be stringified into the Worker source AND used directly on the
     main-thread fallback path. Compiles shaders, builds the 96x96 mesh,
     caches every uniform/attribute location once.
     ------------------------------------------------------------------ */
  function createCore(gl) {
    var GRID = 160, DEPTH_SCALE = 0.45, EDGE_MARGIN = 0.07;
    var FOV = 38 * Math.PI / 180, CAM_DIST = 3.1;

    var VS =
      "attribute vec2 a_pos;\n" +          /* -1..1 plane */
      "attribute vec2 a_uv;\n" +
      "uniform sampler2D u_depth;\n" +
      "uniform mat4 u_mvp;\n" +
      "uniform float u_base;\n" +          /* per-cover background depth (45th pctile) */
      "varying vec2 v_uv;\n" +
      "void main() {\n" +
      "  v_uv = a_uv;\n" +
      "  float d = texture2D(u_depth, a_uv).r;\n" +   /* 1 = near */
      /* Android-style: subtract the cover's own background level and
         re-normalize so the subject always gets the full pop range. */
      "  float z = max(d - u_base, 0.0) / max(1.0 - u_base, 0.05) * " + DEPTH_SCALE.toFixed(2) + ";\n" +
      /* ramp displacement to zero at the borders (rectilinear outline) */
      "  float ex = smoothstep(0.0, " + EDGE_MARGIN.toFixed(2) + ", min(a_uv.x, 1.0 - a_uv.x));\n" +
      "  float ey = smoothstep(0.0, " + EDGE_MARGIN.toFixed(2) + ", min(a_uv.y, 1.0 - a_uv.y));\n" +
      "  z *= ex * ey;\n" +
      /* covers are square (256x256 thumbs): plane maps 1:1 */
      "  gl_Position = u_mvp * vec4(a_pos.xy, z, 1.0);\n" +
      "}";
    /* NOTE: mesh uv v=0 is the TOP row and texture row 0 is the image top,
       so uv samples directly — no flip anywhere (a 1-v flip here showed
       covers upside down). */
    var FS =
      "precision mediump float;\n" +
      "varying vec2 v_uv;\n" +
      "uniform sampler2D u_art;\n" +
      "void main() { gl_FragColor = texture2D(u_art, v_uv); }";

    function compile(type, src) {
      var s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) return null;
      return s;
    }
    var vs = compile(gl.VERTEX_SHADER, VS);
    var fs = compile(gl.FRAGMENT_SHADER, FS);
    if (!vs || !fs) return { ok: false };
    var prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return { ok: false };

    /* cache locations ONCE (the old renderer looked them up every frame) */
    var uMvp = gl.getUniformLocation(prog, "u_mvp");
    var uBase = gl.getUniformLocation(prog, "u_base");
    var uArt = gl.getUniformLocation(prog, "u_art");
    var uDepth = gl.getUniformLocation(prog, "u_depth");
    var aPos = gl.getAttribLocation(prog, "a_pos");
    var aUv = gl.getAttribLocation(prog, "a_uv");

    /* mesh */
    var verts = [];
    for (var y = 0; y <= GRID; y++) {
      for (var x = 0; x <= GRID; x++) {
        var u = x / GRID, v = y / GRID;
        verts.push(u * 2 - 1, 1 - v * 2, u, v);
      }
    }
    var idx = [];
    var W = GRID + 1;
    for (var yy = 0; yy < GRID; yy++) {
      for (var xx = 0; xx < GRID; xx++) {
        var a = yy * W + xx, b = a + 1, c = a + W, d = c + 1;
        idx.push(a, c, b, b, c, d);
      }
    }
    var vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);
    var ibo = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(idx), gl.STATIC_DRAW);
    var indexCount = idx.length;

    /* static state set once: program, attribs, depth test */
    gl.useProgram(prog);
    gl.enable(gl.DEPTH_TEST);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(aUv);
    gl.vertexAttribPointer(aUv, 2, gl.FLOAT, false, 16, 8);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    gl.uniform1i(uArt, 0);
    gl.uniform1i(uDepth, 1);

    var texArt = null, texDepth = null;

    function makeTex(img) {
      var t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return t;
    }

    /* column-major 4x4 helpers (only what we need) */
    function perspective(fovy, aspect, near, far) {
      var f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
      return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
    }
    function mul(m1, m2) {
      var o = new Array(16).fill(0);
      for (var cc = 0; cc < 4; cc++)
        for (var rr = 0; rr < 4; rr++)
          for (var k = 0; k < 4; k++) o[cc * 4 + rr] += m1[k * 4 + rr] * m2[cc * 4 + k];
      return o;
    }
    function rotXY(rx, ry, tz, ex, ey2) {
      var cx = Math.cos(rx), sx = Math.sin(rx), cy = Math.cos(ry), sy = Math.sin(ry);
      /* Rx * Ry, then translate: camera back + lateral eye offset. */
      return [cy, sx * sy, -cx * sy, 0,
              0, cx, sx, 0,
              sy, -sx * cy, cx * cy, 0,
              -ex, -ey2, tz, 1];
    }

    return {
      ok: true,
      setTextures: function (art, depth) {
        if (texArt) gl.deleteTexture(texArt);
        if (texDepth) gl.deleteTexture(texDepth);
        gl.activeTexture(gl.TEXTURE0);
        texArt = makeTex(art);
        gl.activeTexture(gl.TEXTURE1);
        texDepth = makeTex(depth);
      },
      draw: function (w, h, rx, ry, base) {
        gl.viewport(0, 0, w, h);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        var proj = perspective(FOV, w / h, 0.5, 10);
        var mv = rotXY(rx, ry, -CAM_DIST, 0, 0);   /* eye fixed on center */
        gl.uniformMatrix4fv(uMvp, false, new Float32Array(mul(proj, mv)));
        gl.uniform1f(uBase, base);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, texArt);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, texDepth);
        gl.drawElements(gl.TRIANGLES, indexCount, gl.UNSIGNED_SHORT, 0);
      }
    };
  }

  /* ------------------------------------------------------------------
     MOTION DRIVER — glide easing + idle/showcase orbit (Android values:
     GLIDE=0.07, tilt 22/15 deg, orbit 0.45/0.28). Also stringified into
     the worker so the orbit never needs per-frame cross-thread messages.
     ------------------------------------------------------------------ */
  function createMotion() {
    var curX = 0, curY = 0;
    return {
      step: function (t, mode, inside, tx, ty) {
        var gx = 0, gy = 0;
        if (mode === "off") { gx = 0; gy = 0; }
        else if (mode === "pointer" && inside) { gx = tx; gy = ty; }
        else {
          /* slow autonomous orbit — mirrors the Android showcase mode */
          gx = Math.sin(t * 0.00016 * Math.PI * 2) * 0.45;
          gy = Math.sin(t * 0.00009 * Math.PI * 2) * 0.28;
        }
        curX += (gx - curX) * 0.07;
        curY += (gy - curY) * 0.07;
        var idle = Math.sin(t * 0.00035) * 0.021;
        return {
          ry: curX * 22 * Math.PI / 180 + idle,
          rx: -curY * 15 * Math.PI / 180 + Math.sin(t * 0.0002) * 0.01,
          /* settled = safe to stop rendering (mode "off" only) */
          settled: mode === "off" && Math.abs(curX) < 0.0004 && Math.abs(curY) < 0.0004
        };
      }
    };
  }

  /* ------------------------------------------------------------------
     WORKER SOURCE — built from the factories above via toString() and
     run from a Blob URL. Owns the OffscreenCanvas + its own rAF loop.
     ------------------------------------------------------------------ */
  var WORKER_SRC =
    '"use strict";\n' +
    "var createCore = " + createCore.toString() + ";\n" +
    "var createMotion = " + createMotion.toString() + ";\n" +
    "var core = null, motion = createMotion();\n" +
    "var cnv = null, W = 0, H = 0, dpr = 1, base = 0.45;\n" +
    "var mode = 'pointer', inside = false, tx = 0, ty = 0;\n" +
    "var active = false, ready = false, running = false, dead = false;\n" +
    "function frame(t) {\n" +
    "  if (!active || !ready || dead) { running = false; return; }\n" +
    "  var w = Math.round(W * dpr), h = Math.round(H * dpr);\n" +
    "  if (w > 0 && h > 0) {\n" +
    "    if (cnv.width !== w || cnv.height !== h) { cnv.width = w; cnv.height = h; }\n" +
    "    var m = motion.step(t, mode, inside, tx, ty);\n" +
    "    core.draw(w, h, m.rx, m.ry, base);\n" +
    "    if (m.settled) { running = false; return; }\n" +   /* motion off: park */
    "  }\n" +
    "  requestAnimationFrame(frame);\n" +
    "}\n" +
    "function wake() { if (!running && active && ready && !dead) { running = true; requestAnimationFrame(frame); } }\n" +
    "self.onmessage = function (e) {\n" +
    "  var m = e.data;\n" +
    "  if (m.t === 'init') {\n" +
    "    cnv = m.canvas;\n" +
    "    var gl = cnv.getContext('webgl', { alpha: true, antialias: true, depth: true, premultipliedAlpha: false });\n" +
    "    core = gl ? createCore(gl) : null;\n" +
    "    if (!core || !core.ok) { dead = true; self.postMessage({ t: 'fail' }); }\n" +
    "    return;\n" +
    "  }\n" +
    "  if (dead) return;\n" +
    "  if (m.t === 'size') { W = m.w; H = m.h; dpr = m.dpr; wake(); }\n" +
    "  else if (m.t === 'tex') { core.setTextures(m.art, m.depth); base = m.base; m.art.close(); m.depth.close(); ready = true; wake(); }\n" +
    "  else if (m.t === 'tilt') { tx = m.x; ty = m.y; inside = !!m.inside; wake(); }\n" +
    "  else if (m.t === 'motion') { mode = m.mode; wake(); }\n" +
    "  else if (m.t === 'active') { active = !!m.on; wake(); }\n" +
    "};\n";

  /* ------------------------------------------------------------------
     MAIN-THREAD state + public API — wrapped in a FACTORY so multiple
     independent renderers can coexist (the now-playing panel keeps the
     default singleton; Cover Flow spins up its own instance for the
     centered cover). WORKER_SRC + the two core factories above are
     stateless and shared across instances. createDepthArt() returns a
     fresh, fully-isolated renderer with its own canvas/worker/state.
     ------------------------------------------------------------------ */
  function createDepthArt() {
  var canvas = null;
  var worker = null, workerDead = false;
  /* fallback (main-thread) renderer state */
  var core = null, motion = null, mainGl = false;
  var running = false, ready = false, active = false;
  var mode = "pointer", inside = false, tiltX = 0, tiltY = 0;
  var cssW = 0, cssH = 0;
  var depthBase = 0.45;

  function clampB(v, a, b) { return Math.max(a, Math.min(b, v)); }

  /* 45th-percentile of the depth image = the cover's background level
     (Android's adaptive baseline), computed once per cover via canvas. */
  function computeBase(img) {
    try {
      var c = document.createElement("canvas");
      var s = 64;                       /* subsample is plenty for a percentile */
      c.width = s; c.height = s;
      var cx2 = c.getContext("2d");
      cx2.drawImage(img, 0, 0, s, s);
      var px = cx2.getImageData(0, 0, s, s).data;
      var vals = new Array(s * s);
      for (var i = 0; i < s * s; i++) vals[i] = px[i * 4];   /* grayscale: R */
      vals.sort(function (a, b) { return a - b; });
      depthBase = clampB(vals[Math.floor(vals.length * 0.45)] / 255, 0.05, 0.85);
    } catch (_) { depthBase = 0.45; }
  }

  function loadImg(url) {
    return new Promise(function (res, rej) {
      var i = new Image();
      i.decoding = "async";
      i.onload = function () { res(i); };
      i.onerror = rej;
      i.src = url;
    });
  }

  /* main-thread fallback rAF (same park/wake discipline as the worker) */
  function mainFrame(t) {
    if (!active || !ready || !core) { running = false; return; }
    var dpr = window.devicePixelRatio || 1;
    var w = Math.round(cssW * dpr), h = Math.round(cssH * dpr);
    if (w > 0 && h > 0) {
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      var m = motion.step(t, mode, inside, tiltX, tiltY);
      core.draw(w, h, m.rx, m.ry, depthBase);
      if (m.settled) { running = false; return; }
    }
    requestAnimationFrame(mainFrame);
  }
  function wake() {
    if (worker || running || !active || !ready) return;
    running = true;
    requestAnimationFrame(mainFrame);
  }

  return {
    mount: function (cnv) {
      canvas = cnv;

      /* CSS size tracked via ResizeObserver — zero layout reads per frame */
      var ro = new ResizeObserver(function (ents) {
        for (var i = 0; i < ents.length; i++) {
          cssW = ents[i].contentRect.width;
          cssH = ents[i].contentRect.height;
        }
        if (worker) worker.postMessage({ t: "size", w: cssW, h: cssH, dpr: window.devicePixelRatio || 1 });
        else wake();
      });
      ro.observe(cnv);

      /* Preferred: OffscreenCanvas on a dedicated worker thread. */
      if (typeof cnv.transferControlToOffscreen === "function" &&
          typeof Worker === "function" && typeof createImageBitmap === "function") {
        try {
          var w = new Worker(URL.createObjectURL(new Blob([WORKER_SRC], { type: "text/javascript" })));
          w.onmessage = function (e) { if (e.data && e.data.t === "fail") workerDead = true; };
          var off = cnv.transferControlToOffscreen();
          w.postMessage({ t: "init", canvas: off }, [off]);
          worker = w;
          return true;
        } catch (_) { worker = null; /* canvas may be unusable now; try main */ }
      }

      /* Fallback: main-thread WebGL (still pause-gated). */
      var gl = cnv.getContext("webgl", { alpha: true, antialias: true, depth: true, premultipliedAlpha: false });
      if (!gl) return false;
      core = createCore(gl);
      if (!core.ok) { core = null; return false; }
      motion = createMotion();
      mainGl = true;
      return true;
    },

    /* Try art+depth; onReady(true) => volumetric active, false => fallback.
       coverUrl (optional) is a HIGH-RES cover for the mesh TEXTURE so the 3D
       art is crisp; it falls back to artUrl (the 256 grid PNG) when absent or
       if the hi-res image fails to load. The depth map is always depthUrl. */
    setSources: function (artUrl, depthUrl, onReady, coverUrl) {
      if ((!worker && !mainGl) || workerDead || !artUrl || !depthUrl) {
        ready = false;
        if (onReady) onReady(false);
        return;
      }
      /* Prefer the hi-res cover for the texture; degrade to the 256 art if it
         is missing or 404s (depth worker may not have published it yet). */
      var texPromise = coverUrl
        ? loadImg(coverUrl).catch(function () { return loadImg(artUrl); })
        : loadImg(artUrl);
      Promise.all([texPromise, loadImg(depthUrl)]).then(function (imgs) {
        computeBase(imgs[1]);          /* per-cover adaptive background level */
        if (worker) {
          /* decode fully off-thread, transfer zero-copy to the worker */
          return Promise.all([createImageBitmap(imgs[0]), createImageBitmap(imgs[1])]).then(function (bmps) {
            if (workerDead) { ready = false; if (onReady) onReady(false); return; }
            worker.postMessage({ t: "tex", art: bmps[0], depth: bmps[1], base: depthBase }, [bmps[0], bmps[1]]);
            ready = true;
            if (onReady) onReady(true);
          });
        }
        core.setTextures(imgs[0], imgs[1]);
        ready = true;
        wake();
        if (onReady) onReady(true);
      }).catch(function () { ready = false; if (onReady) onReady(false); });
    },

    /* Event-driven (pointermove/leave) — NOT called per frame anymore. */
    setTilt: function (x, y, pointerInside) {
      tiltX = x; tiltY = y; inside = !!pointerInside;
      if (worker) worker.postMessage({ t: "tilt", x: x, y: y, inside: inside });
      else wake();
    },
    setMotion: function (m) {
      mode = m === "showcase" || m === "off" ? m : "pointer";
      if (worker) worker.postMessage({ t: "motion", mode: mode });
      else wake();
    },
    /* Gate: now-playing panel visible AND a volumetric cover is loaded. */
    setActive: function (on) {
      active = !!on;
      if (worker) worker.postMessage({ t: "active", on: active });
      else wake();
    },
    stop: function () { ready = false; },
    /* Fully release this instance (terminate its worker). Use when a
       transient renderer — e.g. a Cover Flow centered cover — is torn down.
       The singleton never calls this. */
    destroy: function () {
      ready = false; active = false;
      if (worker) { try { worker.terminate(); } catch (_) {} worker = null; }
      core = null; motion = null; mainGl = false; canvas = null;
    }
  };
  }

  /* Default singleton — the now-playing panel's renderer (unchanged API). */
  window.MnDepthArt = createDepthArt();
  /* Factory for additional independent renderers (Cover Flow). */
  window.MnDepthArt.create = createDepthArt;
})();

/* module registry: expose this file as an independent block */
if (window.MN) MN.define("depthart", "1.0.0", [], function () { return window.MnDepthArt || {}; });
