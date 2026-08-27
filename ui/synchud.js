/* ============================================================
   MONATOMIC — SYNC ACTIVITY HUD + FILE TRANSFER   synchud.js  v1.0.0
   ------------------------------------------------------------
   Three surfaces on top of the phone-sync bridge:

   1. A non-modal activity HUD (bottom-right) that appears whenever
      sync OR transfer activity happens: binds the existing
      {"type":"sync",...} status events AND the new transfer events
        {"type":"xfer","state":"start|progress|done|error",
         "file":"…","index":N,"total":M,"sentBytes":X,
         "totalBytes":Y,"skipped":bool,"error":"…"}
      Auto-dismisses a few seconds after "done"; stays until
      dismissed on "error".

   2. Drag & drop of audio files anywhere in the window → wireless
      upload to the phone ({"cmd":"sendfiles","paths":[…]}). CEF
      desktop drops expose real paths via file.path; when this
      build doesn't, the drop falls back to the native picker
      ({"cmd":"sendpick"} — also wired to the Settings → Sync
      "Send files to phone…" button, #sync-sendfiles).

   3. "On phone?" presence badges: {"cmd":"syncpresence"} asks the
      phone which of our fingerprinted tracks it has; the reply
      {"type":"presence","ok":bool,"have":[hashes],"ids":[ids]}
      is cached as a track-id set and rendered as a tiny phone
      glyph on track rows (app.js consults window.__mnOnPhone /
      window.__mnPhoneBadge at row build; existing rows are swept
      here). ok:false (phone unreachable) degrades silently.

   Talks to the bus only through the core module api; "xfer" and
   "presence" are NEW message types owned by this module.
   ============================================================ */
(function () {
  "use strict";
  if (!window.MN) return;

  MN.define("synchud", "1.0.0", ["core"], function (deps) {
    const core = deps.core;
    if (!core || !core.send || !core.on) return {};
    const send = core.send;
    const on = core.on;                 /* NEW types this module owns      */
    const tap = core.tap || core.on;    /* shared types sync.js/app.js own */
    const $ = (s, r) => (r || document).querySelector(s);
    const toast = (m) => { if (typeof window.__mnToast === "function") window.__mnToast(m); };

    const SYNC_ACTIVE = { connecting: 1, pulling: 1, merging: 1, pushing: 1 };
    const SYNC_LABEL = {
      connecting: "Connecting to phone…", pulling: "Pulling changes…",
      merging: "Merging…", pushing: "Pushing changes…",
    };

    let host = "";        /* "ip:port" from sync status events */
    let phoneName = "";   /* pretty name from discovery, persisted */
    try { phoneName = localStorage.getItem("mn.phonename") || ""; } catch (_) {}
    const phoneLabel = () => phoneName || (host ? "phone at " + host : "your phone");

    /* ---------------- HUD ---------------- */
    let hud = null, hideTimer = 0;
    function ensureHud() {
      if (hud) return hud;
      hud = document.createElement("div");
      hud.id = "sync-hud";
      hud.innerHTML =
        '<div class="sh-head"><span class="sh-title">Phone sync</span>' +
        '<button class="sh-close" title="Dismiss">✕</button></div>' +
        '<div class="sh-stage">—</div>' +
        '<div class="sh-sub" hidden></div>' +
        '<div class="sh-bar"><div class="sh-fill"></div></div>';
      document.body.appendChild(hud);
      $(".sh-close", hud).addEventListener("click", hide);
      return hud;
    }
    function show() {
      ensureHud();
      clearTimeout(hideTimer); hideTimer = 0;   /* activity cancels a pending hide */
      hud.classList.add("in");
    }
    function hide() {
      clearTimeout(hideTimer); hideTimer = 0;
      if (hud) hud.classList.remove("in");
    }
    function hideSoon(ms) {
      clearTimeout(hideTimer);
      hideTimer = setTimeout(hide, ms || 3500);
    }
    function paint(stage, sub, pct, err) {
      ensureHud();
      const st = $(".sh-stage", hud), sb = $(".sh-sub", hud), fl = $(".sh-fill", hud);
      st.textContent = stage;
      st.classList.toggle("err", !!err);
      if (sub) { sb.hidden = false; sb.textContent = sub; } else { sb.hidden = true; }
      hud.classList.toggle("busy", pct != null);
      if (pct != null) fl.style.width = Math.max(0, Math.min(100, pct)) + "%";
    }

    function fmtBytes(n) {
      n = +n || 0;
      if (n >= 1e9) return (n / 1e9).toFixed(2) + " GB";
      if (n >= 1e6) return (n / 1e6).toFixed(1) + " MB";
      if (n >= 1e3) return (n / 1e3).toFixed(0) + " KB";
      return n + " B";
    }

    /* ---- sync status events → stage line (no bar). tap(), NOT on():
       sync.js owns the on("sync") slot (single-handler bus). ---- */
    tap("sync", (m) => {
      if (!m) return;
      host = m.host || host;
      const st = m.state || "idle";
      if (SYNC_ACTIVE[st]) {
        show();
        paint(SYNC_LABEL[st] || st, host ? "Library sync · " + host : "Library sync", null, false);
      } else if (st === "done") {
        /* only announce a sync that this HUD actually watched */
        if (hud && hud.classList.contains("in")) {
          paint("Sync complete", (m.applied || 0) + " applied · " + (m.skipped || 0)
                + " skipped · " + (m.pushed || 0) + " pushed", null, false);
          hideSoon(3500);
        }
        setTimeout(requestPresence, 1500);   /* library metrics moved — refresh badges */
      } else if (st === "error") {
        if (hud && hud.classList.contains("in")) {
          paint("Sync failed", m.error || "unknown error", null, true);
          /* sticky: stays until dismissed */
        }
      }
    });

    /* remember the discovered phone's pretty name for the drop overlay
       (tap — sync.js owns on("syncdiscover")) */
    tap("syncdiscover", (m) => {
      if (m && m.ok && m.name) {
        phoneName = m.name;
        try { localStorage.setItem("mn.phonename", phoneName); } catch (_) {}
      }
    });

    /* ---- transfer events → stage + progress bar ---- */
    let batch = { sent: 0, skipped: 0, failed: 0 };
    on("xfer", (m) => {
      if (!m) return;
      const idx = m.index || 0, tot = m.total || 0;
      const pos = tot ? "File " + idx + " of " + tot : "";
      if (m.state === "start") {
        if (idx <= 1) batch = { sent: 0, skipped: 0, failed: 0 };
        show();
        paint("Sending " + (m.file || "file"),
              pos + " · " + fmtBytes(m.totalBytes), 0, false);
      } else if (m.state === "progress") {
        show();
        const pct = m.totalBytes > 0 ? (100 * m.sentBytes / m.totalBytes) : 0;
        paint("Sending " + (m.file || "file"),
              pos + " · " + fmtBytes(m.sentBytes) + " / " + fmtBytes(m.totalBytes),
              pct, false);
      } else if (m.state === "done") {
        if (m.skipped) batch.skipped++; else batch.sent++;
        if (idx >= tot) {
          const bits = [];
          if (batch.sent) bits.push(batch.sent + " sent");
          if (batch.skipped) bits.push(batch.skipped + " already on the phone");
          if (batch.failed) bits.push(batch.failed + " failed");
          show();
          paint("Transfer complete", bits.join(" · ") || "nothing to do",
                batch.failed ? null : 100, batch.failed > 0);
          if (!batch.failed) hideSoon(4000);
          setTimeout(requestPresence, 1200);   /* new files → new badges */
        } else {
          paint((m.skipped ? "Already on phone — " : "Sent ") + (m.file || ""),
                pos, 100, false);
        }
      } else if (m.state === "error") {
        batch.failed++;
        show();
        paint((m.file ? m.file + " — " : "") + "transfer failed",
              m.error || "unknown error", null, true);
        /* sticky on a terminal error; a following file's "start" repaints */
        if (idx >= tot) setTimeout(requestPresence, 1200);
      }
    });

    /* ---------------- drag & drop → send to phone ---------------- */
    const AUDIO_RE = /\.(mp3|flac|m4a|aac|ogg|oga|opus|wav|wma|aif|aiff|ape|mka|dsf|dff|mp2|mp4|alac|wv)$/i;
    let overlay = null, dragDepth = 0;
    function ensureOverlay() {
      if (overlay) return overlay;
      overlay = document.createElement("div");
      overlay.id = "drop-overlay";
      overlay.innerHTML =
        '<div class="drop-card">Drop to send to <span class="drop-who"></span>' +
        '<div class="drop-sub">Audio files are copied to the phone over Wi-Fi</div></div>';
      document.body.appendChild(overlay);
      return overlay;
    }
    function overlayShow(onoff) {
      ensureOverlay();
      if (onoff) $(".drop-who", overlay).textContent = phoneLabel();
      overlay.classList.toggle("on", !!onoff);
    }
    function dragHasFiles(e) {
      const t = e.dataTransfer && e.dataTransfer.types;
      if (!t) return false;
      for (let i = 0; i < t.length; i++) if (t[i] === "Files") return true;
      return false;
    }
    window.addEventListener("dragenter", (e) => {
      if (!dragHasFiles(e)) return;
      e.preventDefault();
      dragDepth++;
      overlayShow(true);
    });
    window.addEventListener("dragover", (e) => {
      if (!dragHasFiles(e)) return;
      e.preventDefault();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
    });
    window.addEventListener("dragleave", (e) => {
      if (!dragHasFiles(e)) return;
      if (--dragDepth <= 0) { dragDepth = 0; overlayShow(false); }
    });
    window.addEventListener("drop", (e) => {
      if (!dragHasFiles(e)) return;
      e.preventDefault();
      dragDepth = 0;
      overlayShow(false);
      const files = (e.dataTransfer && e.dataTransfer.files) ? [...e.dataTransfer.files] : [];
      if (!files.length) return;
      const audio = files.filter((f) => AUDIO_RE.test(f.name || ""));
      if (!audio.length) { toast("Only audio files can be sent to the phone"); return; }
      /* CEF desktop drops carry the real filesystem path on file.path */
      const paths = audio.map((f) => f.path).filter((p) => typeof p === "string" && p.length > 1);
      if (paths.length) {
        send({ cmd: "sendfiles", paths });
        toast("Sending " + paths.length + " file" + (paths.length === 1 ? "" : "s")
              + " to " + phoneLabel() + "…");
      } else {
        /* this CEF build hides drop paths from JS — route through the
           native picker instead so the gesture still ends in a transfer */
        toast("Couldn't read the dropped paths — pick the files instead");
        send({ cmd: "sendpick" });
      }
    });

    /* Settings → Sync fallback button */
    const btnSend = $("#sync-sendfiles");
    if (btnSend) btnSend.addEventListener("click", () => send({ cmd: "sendpick" }));

    /* ---------------- "on phone?" presence badges ---------------- */
    const phoneIds = new Set();
    window.__mnOnPhone = (id) => phoneIds.has(Number(id));
    window.__mnPhoneBadge = function () {
      const b = document.createElement("span");
      b.className = "row-onphone";
      b.title = "Also on " + phoneLabel();
      b.innerHTML = '<svg viewBox="0 0 10 16" width="7" height="11" aria-hidden="true">'
        + '<rect x="0.75" y="0.75" width="8.5" height="14.5" rx="2" fill="none" '
        + 'stroke="currentColor" stroke-width="1.5"/>'
        + '<circle cx="5" cy="12.4" r="1.1" fill="currentColor"/></svg>';
      return b;
    };
    function sweepBadges() {
      document.querySelectorAll(".track-row[data-id]").forEach((r) => {
        const onPhone = phoneIds.has(parseInt(r.dataset.id, 10));
        const cur = r.querySelector(".row-onphone");
        if (onPhone && !cur) {
          const title = r.querySelector(".c-title");
          if (title) title.appendChild(window.__mnPhoneBadge());
        } else if (!onPhone && cur) {
          cur.remove();
        }
      });
    }
    on("presence", (m) => {
      /* ok:false = phone unreachable → keep the last known set, no errors,
         no badge churn (degrade silently) */
      if (!m || !m.ok) return;
      phoneIds.clear();
      (m.ids || []).forEach((id) => phoneIds.add(Number(id)));
      sweepBadges();
    });
    let presenceTimer = 0;
    function requestPresence() {
      clearTimeout(presenceTimer);
      presenceTimer = setTimeout(() => send({ cmd: "syncpresence" }), 300);
    }
    /* boot probe once the library + sync state have settled. No polling
       beyond this: fresh rows consult the cached set at build time
       (app.js hook), and sync/transfer completions re-probe above. */
    setTimeout(requestPresence, 4000);

    return { onPhone: window.__mnOnPhone, refresh: requestPresence };
  });

  /* nothing else depends on this module, so self-boot after defining */
  MN.get("synchud");
})();
