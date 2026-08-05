/* ============================================================
   MONATOMIC — LIBRARY SYNC UI            ui/sync.js    v1.0.0
   ------------------------------------------------------------
   Likes / ratings / play-counts sync with the Android app over
   LAN HTTP (the PHONE runs the server) or via snapshot files.

   Backend contract (C side):
     status events  {"type":"sync","state":"idle|connecting|pulling|
                     merging|pushing|done|error","host":"h:p"|"",
                     "auto":bool,"last_ms":<epoch ms|0>,
                     "applied":N,"skipped":M,"pushed":K,"error":"…"}
     commands       {"cmd":"syncstatus"} · {"cmd":"syncsethost",host,port}
                    {"cmd":"syncnow"} · {"cmd":"syncauto",on}
                    {"cmd":"syncexport"} · {"cmd":"syncimport"}

   Surfaces: the SYNC pill in the player bar (#sync-pill) and the
   Settings → Sync tab. Talks to the bus ONLY through the core
   module api; "sync" is a NEW message type, so on() is safe here
   (no other handler owns it — grep before adding more).
   ============================================================ */
(function () {
  "use strict";
  if (!window.MN) return;

  MN.define("sync", "1.0.0", ["core"], function (deps) {
    const core = deps.core;
    if (!core || !core.send || !core.on) return {};
    const send = core.send;
    const on = core.on;
    const $ = (s, r) => (r || document).querySelector(s);
    const toast = (m) => { if (typeof window.__mnToast === "function") window.__mnToast(m); };

    const ACTIVE = { connecting: 1, pulling: 1, merging: 1, pushing: 1 };
    const STATE_LABEL = {
      idle: "Idle", connecting: "Connecting…", pulling: "Pulling changes…",
      merging: "Merging…", pushing: "Pushing changes…", done: "Done", error: "Failed",
    };

    let last = { state: "idle", host: "", auto: false, last_ms: 0,
                 applied: 0, skipped: 0, pushed: 0, error: "" };
    let seen = false;        /* first status arrived — only then show the pill */
    let prevState = "";
    let flashing = false;    /* brief green "done" flash before settling to idle */
    let flashTimer = 0;
    let hostTouched = false; /* user is editing host/port — stop auto-filling */

    const pill = $("#sync-pill");
    const elHost = $("#sync-host"), elPort = $("#sync-port");
    const btnSave = $("#sync-save"), btnNow = $("#sync-now");
    const elAuto = $("#sync-auto");
    const btnExp = $("#sync-export"), btnImp = $("#sync-import");
    const elStatus = $("#sync-status");

    function relTime(ms) {
      if (!ms) return "never";
      const d = Date.now() - ms;
      if (d < 90 * 1000) return "just now";
      const m = Math.floor(d / 60000);
      if (m < 60) return m + " min ago";
      const h = Math.floor(m / 60);
      if (h < 48) return h + " h ago";
      return Math.floor(h / 24) + " days ago";
    }

    function paintPill() {
      if (!pill) return;
      pill.hidden = !seen;
      if (!seen) return;
      const st = flashing ? "done" : (last.state || "idle");
      pill.classList.toggle("nohost", !last.host);
      pill.classList.toggle("busy", !!ACTIVE[st]);
      pill.classList.toggle("err", st === "error");
      pill.classList.toggle("ok", st === "done");
      let tip;
      if (!last.host) {
        tip = "Library sync — no phone configured yet. Click to set it up (Settings → Sync).";
      } else if (ACTIVE[st]) {
        tip = (STATE_LABEL[st] || st) + "  ·  " + last.host;
      } else if (st === "error") {
        tip = "Sync failed: " + (last.error || "unknown error") + "  ·  " + last.host;
      } else {
        tip = last.host + "  ·  last synced " + relTime(last.last_ms) + "  ·  click to sync now";
      }
      pill.title = tip;
    }

    function paintStatus() {
      if (elStatus) {
        const st = last.state || "idle";
        let txt;
        if (ACTIVE[st]) txt = STATE_LABEL[st] || st;
        else if (st === "error") txt = "Failed — " + (last.error || "unknown error");
        else txt = (last.host ? "Idle" : "No phone configured")
                 + "  ·  last successful sync: " + relTime(last.last_ms);
        if (elStatus.textContent !== txt) elStatus.textContent = txt;
        elStatus.classList.toggle("err", st === "error");
        elStatus.classList.toggle("busy", !!ACTIVE[st]);
      }
      if (btnNow) btnNow.disabled = !!ACTIVE[last.state];
      if (elAuto && document.activeElement !== elAuto) elAuto.checked = !!last.auto;
      /* mirror the configured host into the inputs unless the user is editing */
      if (last.host && !hostTouched) {
        const i = last.host.lastIndexOf(":");
        const h = i > 0 ? last.host.slice(0, i) : last.host;
        const p = i > 0 ? parseInt(last.host.slice(i + 1), 10) : 0;
        if (elHost && document.activeElement !== elHost) elHost.value = h;
        if (elPort && document.activeElement !== elPort && p) elPort.value = String(p);
      }
    }

    /* per-field toggles + auto interval (populated from status events) */
    const elFLikes   = $("#sync-f-likes");
    const elFRatings = $("#sync-f-ratings");
    const elFPlays   = $("#sync-f-plays");
    const elInterval = $("#sync-interval");
    function sendFields() {
      send({
        cmd:     "syncfields",
        likes:   elFLikes   ? elFLikes.checked   : true,
        ratings: elFRatings ? elFRatings.checked : true,
        plays:   elFPlays   ? elFPlays.checked   : true,
      });
    }
    [elFLikes, elFRatings, elFPlays].forEach((c) => {
      if (c) c.addEventListener("change", sendFields);
    });
    if (elInterval) elInterval.addEventListener("change", () => {
      send({ cmd: "syncauto",
             on: elAuto ? elAuto.checked : false,
             minutes: parseInt(elInterval.value, 10) || 10 });
    });

    on("sync", (m) => {
      if (!m) return;
      seen = true;
      const was = prevState;
      last = {
        state: m.state || "idle",
        host: m.host || "",
        auto: !!m.auto,
        last_ms: m.last_ms || 0,
        applied: m.applied || 0,
        skipped: m.skipped || 0,
        pushed: m.pushed || 0,
        error: m.error || "",
      };
      /* reflect the persisted field toggles + interval (not while editing) */
      if (elFLikes   && m.f_likes   != null && document.activeElement !== elFLikes)   elFLikes.checked   = !!m.f_likes;
      if (elFRatings && m.f_ratings != null && document.activeElement !== elFRatings) elFRatings.checked = !!m.f_ratings;
      if (elFPlays   && m.f_plays   != null && document.activeElement !== elFPlays)   elFPlays.checked   = !!m.f_plays;
      if (elInterval && m.interval  > 0     && document.activeElement !== elInterval) elInterval.value   = String(m.interval);
      prevState = last.state;
      if (last.state === "done" && was !== "done") {
        toast("Sync complete — " + last.applied + " applied, "
              + last.skipped + " skipped, " + last.pushed + " pushed");
        flashing = true;
        clearTimeout(flashTimer);
        flashTimer = setTimeout(() => { flashing = false; paintPill(); }, 2400);
      } else if (last.state === "error" && was !== "error") {
        toast("Sync failed — " + (last.error || "unknown error"));
      }
      paintPill();
      paintStatus();
    });

    if (pill) pill.addEventListener("click", () => {
      if (ACTIVE[last.state]) return;   /* a sync is already running */
      if (last.host) send({ cmd: "syncnow" });
      else if (typeof window.__mnOpenSettings === "function") window.__mnOpenSettings("sync");
    });

    if (elHost) elHost.addEventListener("input", () => { hostTouched = true; });
    if (elPort) elPort.addEventListener("input", () => { hostTouched = true; });
    if (btnSave) btnSave.addEventListener("click", () => {
      const host = elHost ? elHost.value.trim() : "";
      let port = elPort ? parseInt(elPort.value, 10) : 8797;
      if (!port || port < 1 || port > 65535) port = 8797;
      if (elPort) elPort.value = String(port);
      if (!host) { toast("Enter the phone's address first"); return; }
      hostTouched = false;
      send({ cmd: "syncsethost", host: host, port: port });
      toast("Sync host saved — " + host + ":" + port);
      send({ cmd: "syncstatus" });
    });
    if (btnNow) btnNow.addEventListener("click", () => {
      if (ACTIVE[last.state]) return;
      send({ cmd: "syncnow" });
    });
    if (elAuto) elAuto.addEventListener("change", () => {
      send({ cmd: "syncauto", on: elAuto.checked });
    });
    if (btnExp) btnExp.addEventListener("click", () => {
      send({ cmd: "syncexport" });
      toast("Exporting sync snapshot…");
    });
    if (btnImp) btnImp.addEventListener("click", () => {
      send({ cmd: "syncimport" });
      toast("Importing sync snapshot…");
    });
    /* completion feedback (these replies were previously unconsumed, so a
       failed export/import was silent and the written path never shown) */
    on("syncexport", (m) => {
      toast(m && m.ok ? ("Snapshot exported" + (m.path ? " → " + m.path : ""))
                      : "Export failed" + (m && m.error ? " (" + m.error + ")" : ""));
    });
    on("syncimport", (m) => {
      toast(m && m.ok ? ("Imported — " + (m.applied || 0) + " applied, " + (m.skipped || 0) + " skipped")
                      : "Import failed" + (m && m.error ? " (" + m.error + ")" : ""));
    });

    /* keep the relative "last synced" strings fresh — and WATCHDOG a stuck
       busy state: if the terminal done/error event was lost (worker died,
       message dropped), the pill + Sync-now button would be disabled
       forever. After 2 min of the same busy state, re-ask the backend;
       after 3 min, fall back to idle so the controls come back. */
    let busySince = 0;
    setInterval(() => {
      if (ACTIVE[last.state]) {
        if (!busySince) busySince = Date.now();
        const stuck = Date.now() - busySince;
        if (stuck > 120000) send({ cmd: "syncstatus" });
        if (stuck > 180000) { last.state = "idle"; busySince = 0; }
      } else {
        busySince = 0;
      }
      paintPill(); paintStatus();
    }, 30000);

    /* populate from the backend once at boot */
    send({ cmd: "syncstatus" });
    paintStatus();

    return { status: () => last };
  });

  /* nothing else depends on this module, so self-boot after defining */
  MN.get("sync");
})();
