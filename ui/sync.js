/* ============================================================
   MONATOMIC — LIBRARY SYNC UI            ui/sync.js    v2.0.0
   ------------------------------------------------------------
   Likes / ratings / play-counts sync with the Android app over
   LAN HTTP (the PHONE runs the server) or via snapshot files —
   now against a DEVICE REGISTRY instead of one hand-typed host.

   Backend contract (C side):
     status events  {"type":"sync","state":"idle|connecting|pulling|
                     merging|pushing|done|error","host":"h:p"|"",
                     "device":"<active device name>|","auto":bool,
                     "last_ms":<epoch ms|0>,"applied":N,"skipped":M,
                     "pushed":K,"byHash":H,"byId":I,"error":"…"}
     device events  {"type":"syncdevices","active":id,
                     "devices":[{id,name,model,host,port,online,
                                 lastSeen,lastSync,lastResult}],
                     "found":[{host,port,model,lastSeen}],
                     "scan":bool}
                    Pushed by the C side's AMBIENT presence scanner
                    (~30 s cadence + immediately after mutations and
                    forced scans); scan:true concludes a discovery
                    pass ("Find devices" spinner cue).
     commands       {"cmd":"syncstatus"} · {"cmd":"syncdevices"}
                    {"cmd":"syncdiscover"}          (force a scan now)
                    {"cmd":"syncdevadd",host,port[,name,model]}
                    {"cmd":"syncdevselect",id} · {"cmd":"syncdevrename",id,name}
                    {"cmd":"syncdevremove",id} · {"cmd":"syncdevupdate",id,host,port}
                    {"cmd":"syncnow"} · {"cmd":"syncauto",on[,minutes]}
                    {"cmd":"syncfields",likes,plays}
                    {"cmd":"syncexport"} · {"cmd":"syncimport"}
                    {"cmd":"synclog"} → {"type":"synclog","lines":[entries]}
                    (tail of sync\activity.jsonl, ≤300, oldest first)
     Status events also carry c_likes/c_dislikes/c_cleared/c_plays/
     c_books — the last merge's per-category tallies ("what got synced"),
     surfaced as the done-toast and mirrored in the activity log. (Star
     ratings were retired 2026-08-24: no toggle, no tally, no export.)

   RULE (user constraint, load-bearing): discovery NEVER adds a
   device by itself. The found list only OFFERS; every registry
   write is an explicit click in this panel.

   Surfaces: the PHONE CHIP in the player bar (#sync-pill — live
   online/offline/syncing/error state of the active device; click
   opens this panel) and the Settings → Sync tab. Talks to the bus
   ONLY through the core module api; "sync" + "syncdevices" are
   owned here (grep before adding more handlers).
   ============================================================ */
(function () {
  "use strict";
  if (!window.MN) return;

  MN.define("sync", "2.0.0", ["core"], function (deps) {
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

    let last = { state: "idle", host: "", device: "", auto: false, last_ms: 0,
                 applied: 0, skipped: 0, pushed: 0,
                 byHash: 0, byId: 0, error: "" };
    let devices = [];        /* registry rows from the last syncdevices event */
    let found = [];          /* discovered-but-unregistered responders */
    let activeId = 0;
    let devSeen = false;     /* first syncdevices event arrived */
    let prevState = "";
    let flashing = false;    /* brief green "done" flash before settling */
    let flashTimer = 0;
    let renamingId = 0;      /* device id with the rename editor open */
    let confirmId = 0;       /* device id with the two-step remove armed */
    let confirmTimer = 0;

    const pill = $("#sync-pill");
    const elDevList = $("#sync-devlist"), elFoundList = $("#sync-foundlist");
    const btnFind = $("#sync-find"), btnManual = $("#sync-manual-btn");
    const rowManual = $("#sync-manual");
    const elHost = $("#sync-host"), elPort = $("#sync-port");
    const btnAddManual = $("#sync-add-manual");
    const btnNow = $("#sync-now");
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

    const activeDev = () => devices.find((d) => d.id === activeId) || null;

    /* ---------- player-bar phone chip -------------------------------- */
    /* State ladder (first hit wins): no device -> dimmed · syncing ->
       pulse · last sync failed -> red · online -> green dot · offline ->
       gray dot. Driven purely by the ~30 s presence events + sync status
       events — no timers, no per-frame work. */
    function paintPill() {
      if (!pill) return;
      const a = activeDev();
      const st = flashing ? "done" : (last.state || "idle");
      const busy = !!ACTIVE[st];
      const failed = !busy && st === "error";
      pill.classList.toggle("nodev", !a);
      pill.classList.toggle("busy", busy);
      pill.classList.toggle("err", !!a && failed);
      pill.classList.toggle("ok", st === "done");
      pill.classList.toggle("online", !!(a && a.online) && !busy && !failed);
      pill.classList.toggle("offline", !!a && !a.online && !busy && !failed);
      let tip;
      if (!a) {
        tip = "Phone sync — no device set up yet. Click to add your phone (Settings → Sync).";
      } else if (busy) {
        tip = (STATE_LABEL[st] || st) + "  ·  " + a.name;
      } else if (failed) {
        tip = a.name + " — last sync failed: " + (last.error || a.lastResult || "unknown error");
      } else if (a.online) {
        tip = a.name + " — online at " + a.host + "  ·  last synced " + relTime(a.lastSync)
            + "  ·  click for sync settings";
      } else {
        tip = a.name + " — not seen on this network (sync server off, or different Wi-Fi)"
            + "  ·  last synced " + relTime(a.lastSync);
      }
      pill.title = tip;
    }

    function paintStatus() {
      if (elStatus) {
        const st = last.state || "idle";
        let txt;
        if (ACTIVE[st]) txt = STATE_LABEL[st] || st;
        else if (st === "error") txt = "Failed — " + (last.error || "unknown error");
        else txt = (activeDev() ? "Idle" : "No device selected")
                 + "  ·  last successful sync: " + relTime(last.last_ms);
        if (elStatus.textContent !== txt) elStatus.textContent = txt;
        elStatus.classList.toggle("err", st === "error");
        elStatus.classList.toggle("busy", !!ACTIVE[st]);
      }
      if (btnNow) btnNow.disabled = !!ACTIVE[last.state] || !activeDev();
      if (elAuto && document.activeElement !== elAuto) elAuto.checked = !!last.auto;
    }

    /* ---------- device registry list --------------------------------- */
    function devMeta(d) {
      const bits = [];
      if (d.model && d.model !== d.name) bits.push(d.model);
      bits.push(d.host + ":" + d.port);
      bits.push(d.online ? "online" : (d.lastSeen ? "last seen " + relTime(d.lastSeen) : "not seen yet"));
      return bits.join("  ·  ");
    }
    function devSub(d) {
      if (!d.lastSync && !d.lastResult) return "never synced";
      const when = d.lastSync ? relTime(d.lastSync) : "";
      return ("last sync: " + (d.lastResult || "—") + (when ? "  ·  " + when : ""));
    }

    function renderDevices() {
      if (!elDevList) return;
      elDevList.textContent = "";
      if (!devices.length) {
        const e = document.createElement("div");
        e.className = "sync-empty";
        e.textContent = devSeen
          ? "No devices added yet. When your phone appears under Nearby devices, click Add — or use Add by address."
          : "Loading…";
        elDevList.appendChild(e);
        return;
      }
      for (const d of devices) {
        const row = document.createElement("div");
        row.className = "sync-dev" + (d.id === activeId ? " is-active" : "");

        const dot = document.createElement("span");
        dot.className = "sync-dot " + (d.online ? "on" : "off");
        dot.title = d.online ? "Answering discovery on this network"
                             : "Not currently seen on this network";
        row.appendChild(dot);

        const main = document.createElement("div");
        main.className = "sync-dev-main";
        if (renamingId === d.id) {
          const inp = document.createElement("input");
          inp.className = "set-input sync-rename";
          inp.value = d.name;
          inp.maxLength = 60;
          const commit = () => {
            renamingId = 0;
            const v = inp.value.trim();
            if (v && v !== d.name) send({ cmd: "syncdevrename", id: d.id, name: v });
            else renderDevices();
          };
          inp.addEventListener("keydown", (ev) => {
            if (ev.key === "Enter") commit();
            if (ev.key === "Escape") { renamingId = 0; renderDevices(); }
          });
          inp.addEventListener("blur", commit);
          main.appendChild(inp);
          setTimeout(() => { inp.focus(); inp.select(); }, 0);
        } else {
          const nm = document.createElement("span");
          nm.className = "sync-dev-name";
          nm.textContent = d.name;
          nm.title = "Double-click to rename";
          nm.addEventListener("dblclick", () => { renamingId = d.id; renderDevices(); });
          main.appendChild(nm);
        }
        const meta = document.createElement("span");
        meta.className = "sync-dev-meta";
        meta.textContent = devMeta(d);
        main.appendChild(meta);
        const sub = document.createElement("span");
        sub.className = "sync-dev-meta sub";
        sub.textContent = devSub(d);
        main.appendChild(sub);
        row.appendChild(main);

        const ctl = document.createElement("div");
        ctl.className = "sync-dev-ctl";
        if (d.id === activeId) {
          const b = document.createElement("span");
          b.className = "sync-badge";
          b.textContent = "ACTIVE";
          b.title = "This device is the sync target";
          ctl.appendChild(b);
        } else {
          const b = document.createElement("button");
          b.className = "btn btn-ghost sync-mini";
          b.textContent = "Use";
          b.title = "Make this the sync target";
          b.addEventListener("click", () => send({ cmd: "syncdevselect", id: d.id }));
          ctl.appendChild(b);
        }
        const rn = document.createElement("button");
        rn.className = "btn btn-ghost sync-mini";
        rn.textContent = "✎";
        rn.title = "Rename";
        rn.addEventListener("click", () => { renamingId = d.id; renderDevices(); });
        ctl.appendChild(rn);
        const rm = document.createElement("button");
        rm.className = "btn btn-ghost sync-mini" + (confirmId === d.id ? " danger" : "");
        rm.textContent = confirmId === d.id ? "Sure?" : "✕";
        rm.title = confirmId === d.id ? "Click again to remove this device"
                                      : "Remove this device";
        rm.addEventListener("click", () => {
          if (confirmId === d.id) {
            clearTimeout(confirmTimer);
            confirmId = 0;
            send({ cmd: "syncdevremove", id: d.id });
            toast("Removed " + d.name);
          } else {
            confirmId = d.id;
            clearTimeout(confirmTimer);
            confirmTimer = setTimeout(() => { confirmId = 0; renderDevices(); }, 3500);
            renderDevices();
          }
        });
        ctl.appendChild(rm);
        row.appendChild(ctl);
        elDevList.appendChild(row);
      }
    }

    /* ---------- nearby (discovered, unregistered) list ---------------- */
    let scanning = false;
    let scanTimer = 0;
    function scanDone() {
      scanning = false;
      clearTimeout(scanTimer);
      scanTimer = 0;
      if (btnFind) { btnFind.disabled = false; btnFind.textContent = "Find devices"; }
      renderFound();
    }

    function renderFound() {
      if (!elFoundList) return;
      elFoundList.textContent = "";
      /* a just-added device stays in the C side's found cache until the
         next ambient pass — filter it here so Add feels instantaneous */
      const fresh = found.filter((f) =>
        !devices.some((d) => d.host === f.host && d.port === f.port));
      if (!fresh.length) {
        const e = document.createElement("div");
        e.className = "sync-empty";
        e.textContent = scanning
          ? "Scanning your network…"
          : "Nothing new found nearby. The two usual reasons: the phone's sync " +
            "server is off (Android app → Settings → Library Sync → Sync server), " +
            "or the phone and this PC are on different Wi-Fi networks. Devices " +
            "already in your list above don't show here again.";
        elFoundList.appendChild(e);
        return;
      }
      for (const f of fresh) {
        const row = document.createElement("div");
        row.className = "sync-dev found";
        const dot = document.createElement("span");
        dot.className = "sync-dot on";
        row.appendChild(dot);
        const main = document.createElement("div");
        main.className = "sync-dev-main";
        const nm = document.createElement("span");
        nm.className = "sync-dev-name";
        nm.textContent = f.model || "Unknown device";
        main.appendChild(nm);
        const meta = document.createElement("span");
        meta.className = "sync-dev-meta";
        meta.textContent = f.host + ":" + f.port + "  ·  seen " + relTime(f.lastSeen);
        main.appendChild(meta);
        row.appendChild(main);
        const ctl = document.createElement("div");
        ctl.className = "sync-dev-ctl";
        const add = document.createElement("button");
        add.className = "btn btn-ghost sync-mini";
        add.textContent = "Add";
        add.title = "Add " + (f.model || f.host) + " to your devices";
        add.addEventListener("click", () => {
          send({ cmd: "syncdevadd", host: f.host, port: f.port,
                 name: f.model || "", model: f.model || "" });
          toast("Added " + (f.model || f.host));
        });
        ctl.appendChild(add);
        row.appendChild(ctl);
        elFoundList.appendChild(row);
      }
    }

    /* ---------- events ------------------------------------------------ */
    on("syncdevices", (m) => {
      if (!m) return;
      devSeen = true;
      devices = Array.isArray(m.devices) ? m.devices : [];
      found = Array.isArray(m.found) ? m.found : [];
      activeId = m.active || 0;
      if (m.scan) scanDone();
      renderDevices();
      renderFound();
      paintPill();
      paintStatus();
    });

    on("sync", (m) => {
      if (!m) return;
      const was = prevState;
      last = {
        state: m.state || "idle",
        host: m.host || "",
        device: m.device || "",
        auto: !!m.auto,
        last_ms: m.last_ms || 0,
        applied: m.applied || 0,
        skipped: m.skipped || 0,
        pushed: m.pushed || 0,
        byHash: m.byHash || 0,
        byId: m.byId || 0,
        error: m.error || "",
      };
      /* reflect the persisted field toggles + interval (not while editing) */
      if (elFLikes   && m.f_likes   != null && document.activeElement !== elFLikes)   elFLikes.checked   = !!m.f_likes;
      if (elFPlays   && m.f_plays   != null && document.activeElement !== elFPlays)   elFPlays.checked   = !!m.f_plays;
      if (elInterval && m.interval  > 0     && document.activeElement !== elInterval) elInterval.value   = String(m.interval);
      prevState = last.state;
      if (last.state === "done" && was !== "done") {
        /* WHAT got synced, in plain words — the per-category tallies the
           merge counted (c_*), plus what the phone reported applying from
           our snapshot ("pushed"). Mirrors the activity-log entry. */
        const n = (v, one, many) => v + " " + (v === 1 ? one : many);
        const bits = [];
        if (m.c_likes)    bits.push(n(m.c_likes, "like", "likes"));
        if (m.c_dislikes) bits.push(n(m.c_dislikes, "dislike", "dislikes"));
        if (m.c_cleared)  bits.push(n(m.c_cleared, "cleared thumb", "cleared thumbs"));
        if (m.c_plays)    bits.push(n(m.c_plays, "play count", "play counts") + " updated");
        if (m.c_books)    bits.push(n(m.c_books, "book position", "book positions"));
        let msg = "Synced with " + (last.device || "your phone") + ": ";
        msg += bits.length ? bits.join(", ") : "no local changes";
        if (last.pushed)  msg += " · " + last.pushed + " pushed to phone";
        if (last.skipped) msg += " · " + last.skipped + " skipped";
        toast(msg);
        if (logVisible) send({ cmd: "synclog" });
        flashing = true;
        clearTimeout(flashTimer);
        flashTimer = setTimeout(() => { flashing = false; paintPill(); }, 2400);
      } else if (last.state === "error" && was !== "error") {
        /* specific failures: an unreachable phone is a normal condition
           (left the house, server off) — say what happens next */
        const who = last.device || "The phone";
        if (/unreachable/i.test(last.error || "")) {
          toast(who + " is unreachable — will sync when it reappears on your network");
        } else {
          toast("Sync with " + who + " failed — " + (last.error || "unknown error"));
        }
        if (logVisible) send({ cmd: "synclog" });
      }
      paintPill();
      paintStatus();
    });

    /* ---------- activity log view ------------------------------------- */
    const elLogBox = $("#sync-logbox");
    const btnLogToggle = $("#sync-log-toggle");
    const btnLogRefresh = $("#sync-log-refresh");
    let logVisible = false;

    function logWhen(ts) {
      if (!ts) return "";
      const d = new Date(ts);
      const today = new Date();
      const hm = String(d.getHours()).padStart(2, "0") + ":" +
                 String(d.getMinutes()).padStart(2, "0");
      if (d.toDateString() === today.toDateString()) return hm;
      return (d.getMonth() + 1) + "/" + d.getDate() + " " + hm;
    }

    /* one plain-words line per entry (same story the toasts tell) */
    function logText(e) {
      const n = (v, one, many) => v + " " + (v === 1 ? one : many);
      switch (e.ev) {
        case "sync": {
          if (!e.ok) return "Sync with " + e.dev + " failed — " + (e.error || "unknown error")
                          + (e.mode === "auto" ? " (auto)" : "");
          const bits = [];
          if (e.likes)    bits.push(n(e.likes, "like", "likes"));
          if (e.dislikes) bits.push(n(e.dislikes, "dislike", "dislikes"));
          if (e.cleared)  bits.push(n(e.cleared, "cleared thumb", "cleared thumbs"));
          if (e.plays)    bits.push(n(e.plays, "play count", "play counts"));
          if (e.books)    bits.push(n(e.books, "book position", "book positions"));
          let t = "Synced with " + e.dev + " — "
                + (bits.length ? bits.join(", ") : "no local changes");
          if (e.pushed)  t += " · pushed " + e.pushed;
          if (e.skipped) t += " · skipped " + e.skipped;
          t += " (" + (e.mode === "auto" ? "auto, " : "") + ((e.dur_ms || 0) / 1000).toFixed(1) + " s)";
          return t;
        }
        case "seen":     return e.dev + " came online" + (e.host ? " at " + e.host : "");
        case "lost":     return e.dev + " went offline";
        case "moved":    return e.dev + " address " + (e.from || "?") + " → " + (e.to || "?")
                              + (e.why === "dhcp" ? " (followed automatically)" :
                                 e.why === "manual" ? " (edited)" : "");
        case "found":    return "Discovered " + e.dev + (e.host ? " at " + e.host : "") + " — not added";
        case "added":    return "Added " + e.dev + (e.host ? " (" + e.host + ")" : "");
        case "removed":  return "Removed " + e.dev;
        case "renamed":  return "Renamed " + (e.from || "?") + " → " + e.dev;
        case "selected": return e.dev + " is now the sync target";
        case "migrated": return "Migrated saved host into the device list" + (e.host ? " (" + e.host + ")" : "");
        case "remote":   return "Remote-control session from " + (e.host || "?");
        default:         return e.ev + " " + (e.dev || "");
      }
    }

    on("synclog", (m) => {
      if (!elLogBox || !m || !Array.isArray(m.lines)) return;
      elLogBox.textContent = "";
      if (!m.lines.length) {
        const d = document.createElement("div");
        d.className = "sync-empty";
        d.textContent = "Nothing logged yet — sync events will appear here.";
        elLogBox.appendChild(d);
        return;
      }
      /* newest first; server caps at 300 so this render stays bounded */
      const frag = document.createDocumentFragment();
      for (let i = m.lines.length - 1; i >= 0; i--) {
        const e = m.lines[i];
        const row = document.createElement("div");
        row.className = "sync-log-row" +
          (e.ev === "sync" ? (e.ok ? " ok" : " bad") : "");
        const when = document.createElement("span");
        when.className = "sync-log-when";
        when.textContent = logWhen(e.ts);
        const txt = document.createElement("span");
        txt.className = "sync-log-text";
        txt.textContent = logText(e);
        row.appendChild(when);
        row.appendChild(txt);
        frag.appendChild(row);
      }
      elLogBox.appendChild(frag);
    });

    if (btnLogToggle) btnLogToggle.addEventListener("click", () => {
      logVisible = !logVisible;
      if (elLogBox) elLogBox.hidden = !logVisible;
      if (btnLogRefresh) btnLogRefresh.hidden = !logVisible;
      btnLogToggle.textContent = logVisible ? "Hide log" : "Show log";
      if (logVisible) send({ cmd: "synclog" });
    });
    if (btnLogRefresh) btnLogRefresh.addEventListener("click", () => {
      send({ cmd: "synclog" });
    });

    /* ---------- controls ---------------------------------------------- */
    /* the phone chip: always visible; opens the panel (state lives HERE,
       "Sync now" is one more click away — deliberate, so a mis-click on
       the player bar never fires a network flow) */
    if (pill) pill.addEventListener("click", () => {
      if (typeof window.__mnOpenSettings === "function") window.__mnOpenSettings("sync");
    });

    if (btnFind) btnFind.addEventListener("click", () => {
      if (btnFind.disabled) return;
      scanning = true;
      btnFind.disabled = true;
      btnFind.textContent = "Scanning…";
      clearTimeout(scanTimer);
      scanTimer = setTimeout(scanDone, 9000);   /* lost-reply safety net */
      renderFound();
      send({ cmd: "syncdiscover" });
    });

    if (btnManual) btnManual.addEventListener("click", () => {
      if (!rowManual) return;
      rowManual.hidden = !rowManual.hidden;
      if (!rowManual.hidden && elHost) elHost.focus();
    });
    function manualAdd() {
      const host = elHost ? elHost.value.trim() : "";
      let port = elPort ? parseInt(elPort.value, 10) : 8797;
      if (!port || port < 1 || port > 65535) port = 8797;
      if (!host) { toast("Enter the phone's address first"); return; }
      send({ cmd: "syncdevadd", host: host, port: port });
      toast("Added " + host + ":" + port);
      if (elHost) elHost.value = "";
      if (rowManual) rowManual.hidden = true;
    }
    if (btnAddManual) btnAddManual.addEventListener("click", manualAdd);
    if (elHost) elHost.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter") manualAdd();
    });

    if (btnNow) btnNow.addEventListener("click", () => {
      if (ACTIVE[last.state]) return;
      send({ cmd: "syncnow" });
    });
    if (elAuto) elAuto.addEventListener("change", () => {
      send({ cmd: "syncauto", on: elAuto.checked });
    });

    /* per-field toggles + auto interval (populated from status events) */
    const elFLikes   = $("#sync-f-likes");
    const elFPlays   = $("#sync-f-plays");
    const elInterval = $("#sync-interval");
    function sendFields() {
      send({
        cmd:     "syncfields",
        likes:   elFLikes ? elFLikes.checked : true,
        plays:   elFPlays ? elFPlays.checked : true,
      });
    }
    [elFLikes, elFPlays].forEach((c) => {
      if (c) c.addEventListener("change", sendFields);
    });
    if (elInterval) elInterval.addEventListener("change", () => {
      send({ cmd: "syncauto",
             on: elAuto ? elAuto.checked : false,
             minutes: parseInt(elInterval.value, 10) || 10 });
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

    /* keep the relative "last synced / last seen" strings fresh — and
       WATCHDOG a stuck busy state: if the terminal done/error event was
       lost (worker died, message dropped), the chip + Sync-now button
       would be disabled forever. After 2 min of the same busy state,
       re-ask the backend; after 3 min, fall back to idle. */
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
      if (renamingId === 0 && confirmId === 0) { renderDevices(); renderFound(); }
    }, 30000);

    /* populate from the backend once at boot */
    send({ cmd: "syncstatus" });
    send({ cmd: "syncdevices" });
    renderDevices();
    renderFound();
    paintStatus();
    paintPill();

    return { status: () => last, devices: () => devices };
  });

  /* nothing else depends on this module, so self-boot after defining */
  MN.get("sync");
})();
