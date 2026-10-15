/* ===== Cikon IoT Dashboard ===== */
(function () {
  "use strict";

  var POLL_MS = 5000;
  var PENDING_TTL = POLL_MS * 2; // must outlast one full poll cycle with margin

  // ---- state ----
  var state = {
    tele: {},
    online: true,
    live: false,
    tasksOpen: false,
    sortKey: "cpu",
    sortDir: "desc",
    resetPhase: "idle"
  };
  var confirmTimer = null, resetTimer = null;
  var prevTicks = null, prevTotal = null;  // for live (delta) CPU
  state.cpu = null;                         // { taskName: pct } or null

  var $ = function (id) { return document.getElementById(id); };

  // ---- helpers ----
  // Bytes → "84.9 kB" / "147 kB" / "1.87 MB" / "2 MB" (binary units, trailing zeros trimmed)
  function fmtMem(b) {
    if (b == null) return "—";
    function trim(v, d) { return String(parseFloat(v.toFixed(d))); }
    if (b < 1024) return b + " B";
    if (b < 1048576) { var k = b / 1024; return trim(k, k < 100 ? 1 : 0) + " kB"; }
    var m = b / 1048576; return trim(m, m < 10 ? 2 : 1) + " MB";
  }
  function pctOf(v, tot) { return Math.max(0, Math.min(100, (v / tot) * 100)); }
  // ---- memory tiles ----
  // To add a memory (e.g. FAT) append one entry: keys are tried in order; give either `free` or `used` keys.
  // Tile appears only when size + free/used exist; "min seen" line only if a min key exists.
  // level(free, min, pct) -> "ok" | "warn" | "crit"
  var MEMS = [
    { label: "DRAM", size: ["dram_size", "heap_total"], free: ["dram_free", "free_heap"], min: ["dram_min_free", "min_heap"],
      level: function (free, min) { return free < 40960 ? "crit" : (free < 81920 || (min != null && min < 40960)) ? "warn" : "ok"; } },
    { label: "PSRAM", size: ["psram_size"], free: ["psram_free"], min: ["psram_min_free"],
      level: function (f, m, pct) { return pct >= 95 ? "crit" : pct >= 85 ? "warn" : "ok"; } },
    { label: "FS", size: ["fs_size", "fs_total"], used: ["fs_used"],
      level: function (f, m, pct) { return pct >= 90 ? "crit" : pct >= 75 ? "warn" : "ok"; } }
  ];
  var MEM_COLORS = { ok: "var(--accent)", warn: "#d9b27a", crit: "#d98e84" };
  function renderMems(t) {
    function pick(keys) { if (!keys) return null; for (var i = 0; i < keys.length; i++) if (t[keys[i]] != null) return t[keys[i]]; return null; }
    var C = 2 * Math.PI * 19;
    function dash(p) { return (p / 100 * C).toFixed(1) + " " + C.toFixed(1); }
    var html = "";
    MEMS.forEach(function (d) {
      var size = pick(d.size);
      var used = d.used ? pick(d.used) : null;
      var free = d.free ? pick(d.free) : (used != null ? size - used : null);
      if (!size || free == null) return;
      var min = d.min ? pick(d.min) : null;
      var pct = pctOf(size - free, size);
      var peak = min != null ? pctOf(size - min, size) : 0;
      var col = MEM_COLORS[d.level(free, min, pct)];
      var title = d.label + " · used " + fmtMem(size - free) + " of " + fmtMem(size) + (min != null ? " · min seen " + fmtMem(min) : "");
      html += '<div class="mem-tile" title="' + esc(title) + '">' +
        '<div class="mem-row"><div class="mem-ring">' +
          '<svg viewBox="0 0 46 46"><circle cx="23" cy="23" r="19" class="mem-trk"/>' +
          '<circle cx="23" cy="23" r="19" class="mem-pk" style="stroke:' + col + '" stroke-dasharray="' + dash(Math.max(peak, pct)) + '"/>' +
          '<circle cx="23" cy="23" r="19" class="mem-us" style="stroke:' + col + '" stroke-dasharray="' + dash(pct) + '"/></svg>' +
          '<span>' + Math.round(pct) + '%</span></div>' +
          '<div class="mem-txt"><span class="mem-lbl">' + esc(d.label) + '</span>' +
          '<span class="mem-val"><b>' + fmtMem(free) + '</b><small>/' + fmtMem(size) + '</small></span>' +
          (min != null ? '<span class="mem-min"><i style="background:' + col + '"></i>min<span class="mem-seen"> seen</span> ' + fmtMem(min) + '</span>' : '') +
        '</div></div>' +
      '</div>';
    });
    var box = $("mems");
    box.innerHTML = html;
    box.hidden = !html;
  }
  function fmtUptime(s) {
    s = Math.floor(s);
    var d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
    if (d > 0) return d + "d " + h + "h";
    if (h > 0) return h + "h " + m + "m";
    if (s >= 60) return m + "m";
    return s + "s";
  }
  function fmtBoot(iso) {
    if (!iso) return "—";
    var d = new Date(iso);
    if (isNaN(d.getTime())) return "—";
    function pad(n) { return String(n).padStart(2, "0"); }
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
  }
  function fmtBootSec(iso) {
    if (!iso) return "—";
    var d = new Date(iso);
    if (isNaN(d.getTime())) return "—";
    function pad(n) { return String(n).padStart(2, "0"); }
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
  }
  var CAP_LABELS = { wifi: "Wi-Fi radio", ble: "Bluetooth LE", bt: "Bluetooth Classic", ieee802154: "802.15.4 radio", thread: "Thread", zigbee: "Zigbee" };
  var STATE_ORDER = { running: 0, ready: 1, blocked: 2, suspended: 3 };
  function stateStyle(st) {
    var dark = document.documentElement.getAttribute("data-theme") === "dark";
    var map = {
      running: [dark ? "#a9d8bd" : "#5fa67e", dark ? "#384a40" : "#e6f1ea"],
      ready: [dark ? "#c2d3a0" : "#8aa86a", dark ? "#414832" : "#eef2e2"],
      blocked: [dark ? "#dcc792" : "#c2a25e", dark ? "#4a4230" : "#f6efdd"],
      suspended: [dark ? "#b4afbe" : "#a89c91", dark ? "#454250" : "#efe9e3"]
    };
    var m = map[st] || map.suspended;
    return { color: m[0], bg: m[1] };
  }
  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }

  // ---- live (delta) CPU: share of CPU time used between two /tele samples ----
  function computeCpu(t) {
    var td = t && t.tasks_dict;
    if (!td) { state.cpu = null; prevTicks = null; prevTotal = null; return; }
    var cur = {}, curTotal = 0;
    Object.keys(td).forEach(function (n) {
      var v = td[n].runtime_ticks || 0;
      cur[n] = v; curTotal += v;
    });
    if (prevTicks && prevTotal != null) {
      var dTotal = curTotal - prevTotal;
      if (dTotal > 0) {
        var cpu = {};
        Object.keys(cur).forEach(function (n) {
          var d = cur[n] - (prevTicks[n] || 0);
          cpu[n] = Math.max(0, d / dTotal * 100);
        });
        state.cpu = cpu;
      }
    }
    prevTicks = cur; prevTotal = curTotal;
  }

  // ---- telemetry ----
  function loadTele() {
    var ctrl = new AbortController();
    var to = setTimeout(function () { ctrl.abort(); }, 3500);
    return fetch("/tele", { cache: "no-store", signal: ctrl.signal })
      .then(function (r) { if (!r.ok) throw new Error("bad"); return r.json(); })
      .then(function (t) {
        computeCpu(t);
        Object.keys(pendingSwitch).forEach(function (name) {
          var p = pendingSwitch[name];
          if (t[name] === p.value) { delete pendingSwitch[name]; }
          else if (Date.now() - p.ts < PENDING_TTL) { t[name] = p.value; }
          else { delete pendingSwitch[name]; }
        });
        state.tele = t; state.online = true; state.live = true; render();
      })
      .catch(function () {
        if (state.live) { state.online = false; render(); return; }
        // No real /tele (design preview) → feed the UI from a sample file.
        // Not needed on the device, where /tele always answers.
        return fetch("sample-tele.json", { cache: "no-store" })
          .then(function (r) { if (!r.ok) throw new Error("bad"); return r.json(); })
          .then(function (t) { state.tele = t; state.online = true; render(); })
          .catch(function () { state.online = false; render(); });
      })
      .finally(function () { clearTimeout(to); });
  }

  function pollLoop() {
    loadTele().finally(function () {
      setTimeout(pollLoop, state.online ? POLL_MS : 2000);
    });
  }

  // ---- switches: fully data-driven from t.switches (flat bool per name) ----
  var switchKeysBuilt = "";
  var pendingSwitch = {}; // name -> { value, ts }

  function switchPretty(k) {
    return k.replace(/_/g, " ").replace(/\b\w/g, function (c) { return c.toUpperCase(); });
  }
  function switchToggle(name) {
    var t = state.tele, next = !t[name];
    t[name] = next; render();
    pendingSwitch[name] = { value: next, ts: Date.now() };
    var body = {}; body[name] = next ? "on" : "off";
    fetch("/cmnd", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    }).catch(function () {});
  }
  function buildSwitchCards(keys) {
    var wrap = $("switch-cards");
    wrap.innerHTML = "";
    keys.forEach(function (name) {
      var card = document.createElement("div");
      card.className = "card";
      card.style.cssText = "display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px";
      card.innerHTML =
        '<div style="display:flex;align-items:center;gap:12px;min-width:0">' +
        '<button class="toggle sm" role="switch" aria-checked="false" style="flex:none"><span class="knob"></span></button>' +
        '<div class="row-title" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap"></div></div>' +
        '<div class="row-sub" style="font-size:13px;font-weight:600"></div>';
      card.querySelector(".row-title").textContent = switchPretty(name);
      card.querySelector(".row-title").style.marginBottom = "0";
      card._name = name;
      card.querySelector(".toggle").addEventListener("click", function () { switchToggle(name); });
      wrap.appendChild(card);
    });
  }

  // ---- Lights: fully data-driven from t.lights (names) + flat t[name] = {on,v,r?,g?,b?,c?,w?} ----
  var lightKeysBuilt = "";  // signature of the currently-built card set
  var pendingLights = {};   // name -> { ts, group?, key?, patch:{on?,v?} } — optimistic overrides for ~4s
  var lightLast = {}, lightTimer = {};
  var DOT_COLOR = { r: "#cf8f86", g: "#8fb88a", b: "#6f8fd6", c: "#f7f5f0", w: "#deb98c" };
  var DOT_LABEL = { r: "Red", g: "Green", b: "Blue", c: "Cold white", w: "Warm white" };

  function lightPretty(k) {
    return k.replace(/_/g, " ").replace(/\b\w/g, function (c) { return c.toUpperCase(); });
  }
  function activeInGroup(raw, keys) {
    var best = null, bestV = -1;
    keys.forEach(function (k) { if (raw[k] != null && raw[k] > bestV) { bestV = raw[k]; best = k; } });
    return bestV > 0 ? best : null;
  }
  function lightTone(allKeys, activeKey) {
    if (activeKey) return DOT_COLOR[activeKey];
    if (allKeys.length === 1) return DOT_COLOR[allKeys[0]];
    return "var(--accent)";
  }
  function lightPost(name, payload) {
    var body = {}; body[name] = payload;
    fetch("/cmnd", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }).catch(function () {});
  }
  function effectiveLightV(name) {
    var raw = (state.tele && state.tele[name]) || {};
    var pend = pendingLights[name];
    var fresh = pend && (Date.now() - pend.ts < PENDING_TTL);
    return fresh && pend.patch.v != null ? pend.patch.v : Math.max(1, Math.min(100, raw.v | 0));
  }
  function mergePendingLight(name, fields) {
    var prev = pendingLights[name];
    var prevFresh = prev && (Date.now() - prev.ts < PENDING_TTL);
    pendingLights[name] = {
      ts: Date.now(),
      group: fields.group !== undefined ? fields.group : (prevFresh ? prev.group : undefined),
      key: fields.key !== undefined ? fields.key : (prevFresh ? prev.key : undefined),
      patch: Object.assign({}, prevFresh ? prev.patch : {}, fields.patch)
    };
  }
  function toggleLight(name, nextOn) {
    mergePendingLight(name, { patch: { on: nextOn } });
    renderLightCards();
    lightPost(name, { on: nextOn });
  }
  function setLightColor(name, key, group) {
    var v = effectiveLightV(name) || 100;
    var payload = key === "r" ? { h: 0, s: 100, v: v, on: true }
      : key === "g" ? { h: 120, s: 100, v: v, on: true }
      : key === "b" ? { h: 240, s: 100, v: v, on: true }
      : key === "c" ? { cct: 100, v: v, on: true }
      : { cct: 0, v: v, on: true };
    mergePendingLight(name, { group: group, key: key, patch: { on: true } });
    renderLightCards();
    lightPost(name, payload);
  }
  function setLightValue(name, v) {
    v = Math.max(1, Math.min(100, v | 0));
    mergePendingLight(name, { patch: { on: true, v: v } });
    renderLightCards();
    var now = Date.now();
    var send = function () { lightLast[name] = Date.now(); lightPost(name, { v: v, on: true }); };
    if (now - (lightLast[name] || 0) > 120) send();
    else { clearTimeout(lightTimer[name]); lightTimer[name] = setTimeout(send, 130); }
  }

  function buildLightCards(names, t) {
    var wrap = $("light-cards");
    wrap.innerHTML = "";
    names.forEach(function (name) {
      var card = document.createElement("div");
      card.className = "card";
      card.style.cssText = "display:flex;flex-direction:column;gap:12px;padding:16px 18px";
      card.innerHTML =
        '<div style="display:flex;align-items:center;justify-content:space-between;gap:12px">' +
        '<div style="display:flex;align-items:center;gap:12px;min-width:0">' +
        '<button class="light-toggle" role="switch" aria-checked="false" style="background:none;border:none;cursor:pointer;padding:0;flex:none">' +
        '<span class="light-track" style="width:40px;height:24px;border-radius:12px;background:var(--track-off);position:relative;display:block;transition:background .25s">' +
        '<span class="knob" style="position:absolute;top:3px;left:3px;width:18px;height:18px;border-radius:50%;background:#fff;box-shadow:0 2px 4px rgba(0,0,0,.2);transition:transform .25s;display:block"></span>' +
        '</span></button>' +
        '<div class="row-title" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap"></div></div>' +
        '<div class="light-dim" style="display:flex;align-items:center;gap:10px;flex:none">' +
        '<span class="light-dots" style="display:flex;gap:8px"></span>' +
        '<span class="mono light-pct" style="font-size:13px;font-weight:600;color:var(--muted);min-width:34px;text-align:right">0%</span>' +
        '</div>' +
        '<span class="light-onoff" style="font-size:13px;font-weight:600;color:var(--muted)"></span>' +
        '</div>' +
        '<div class="light-dim" style="position:relative;height:24px;display:flex;align-items:center">' +
        '<div style="position:absolute;left:0;right:0;height:12px;border-radius:6px;background:var(--track-off)"></div>' +
        '<div class="light-fill" style="pointer-events:none;position:relative;height:12px;border-radius:6px;background:var(--accent);width:0%;transition:width .06s"></div>' +
        '<input type="range" min="1" max="100" class="slider">' +
        '</div>';
      card._name = name;
      card._hasBrightness = (t[name] || {}).v != null;
      [].forEach.call(card.querySelectorAll(".light-dim"), function (el) { el.hidden = !card._hasBrightness; });
      card.querySelector(".light-onoff").hidden = card._hasBrightness;
      card.querySelector(".row-title").textContent = lightPretty(name);
      var toggle = card.querySelector(".light-toggle");
      var inp = card.querySelector(".slider");
      toggle.addEventListener("click", function () {
        var on = toggle.getAttribute("aria-checked") === "true";
        toggleLight(name, !on);
      });
      inp.addEventListener("input", function () { setLightValue(name, parseInt(inp.value, 10) || 1); });
      inp.addEventListener("click", function (e) {
        var r = inp.getBoundingClientRect();
        setLightValue(name, Math.max(1, Math.round((e.clientX - r.left) / r.width * 100)));
      });
      wrap.appendChild(card);
    });
  }

  function renderLightCards() {
    var t = state.tele;
    var names = (Array.isArray(t.lights) ? t.lights : []).filter(function (n) { return n && t[n] != null; });
    var sig = names.join(",");
    $("lights-label").hidden = names.length === 0;
    $("lights-label").style.marginTop = $("controls-label").hidden ? "0" : "16px";
    $("controls-cell").hidden = names.length === 0 && $("controls-label").hidden;
    if (sig !== lightKeysBuilt) { buildLightCards(names, t); lightKeysBuilt = sig; }
    [].forEach.call($("light-cards").children, function (card) {
      var name = card._name, raw = t[name] || {};
      var hasBrightness = card._hasBrightness;
      var pend = pendingLights[name], fresh = pend && (Date.now() - pend.ts < PENDING_TTL);
      var on = fresh && pend.patch.on != null ? pend.patch.on : !!raw.on;
      var v = fresh && pend.patch.v != null ? pend.patch.v : Math.max(1, Math.min(100, raw.v | 0));
      var rgbKeys = ["r", "g", "b"].filter(function (k) { return raw[k] != null; });
      var cwKeys = ["c", "w"].filter(function (k) { return raw[k] != null; });
      var allKeys = rgbKeys.concat(cwKeys);
      // c/w first: they are either real scaled brightness (white mode) or exactly 0 (RGB mode).
      // r/g/b in white mode is a decorative cct-only approximation and inflated, so the two
      // groups must never be compared in one "take the largest" pass.
      var activeKey = fresh && pend.key
        ? pend.key
        : (activeInGroup(raw, cwKeys) || activeInGroup(raw, rgbKeys));
      var tone = hasBrightness ? lightTone(allKeys, activeKey) : "#deb98c";
      card.querySelector(".light-toggle").setAttribute("aria-checked", on ? "true" : "false");
      card.querySelector(".light-track").style.background = on ? tone : "var(--track-off)";
      card.querySelector(".knob").style.transform = "translateX(" + (on ? "16px" : "0px") + ")";
      card.querySelector(".light-pct").textContent = v + "%";
      card.querySelector(".light-onoff").textContent = on ? "On" : "Off";
      card.querySelector(".light-fill").style.background = tone;
      card.querySelector(".light-fill").style.width = v + "%";
      var slider = card.querySelector(".slider");
      if (document.activeElement !== slider) slider.value = v;
      [].forEach.call(card.querySelectorAll(".light-dim"), function (el) { el.style.opacity = on ? 1 : 0.5; });
      var dots = card.querySelector(".light-dots");
      if (!hasBrightness) return;
      var dotKeys = allKeys.length < 2 ? [] : allKeys;
      if (dots.childElementCount !== dotKeys.length || dots._sig !== dotKeys.join(",")) {
        dots.innerHTML = "";
        dots._sig = dotKeys.join(",");
        dotKeys.forEach(function (k) {
          var dot = document.createElement("button");
          dot.title = DOT_LABEL[k];
          dot.className = "light-dot";
          dot.style.cssText = "width:18px;height:18px;border-radius:50%;border:" + (k === "c" ? "1px solid var(--faint)" : "none") + ";cursor:pointer;padding:0;flex:none;background:" + DOT_COLOR[k] + ";--dot-c:" + DOT_COLOR[k] + ";transition:all .15s";
          dot._key = k;
          dot.addEventListener("click", function () {
            setLightColor(name, k, rgbKeys.indexOf(k) !== -1 ? "rgb" : "cw");
          });
          dots.appendChild(dot);
        });
      }
      [].forEach.call(dots.children, function (dot) {
        dot.classList.toggle("active", dot._key === activeKey);
      });
    });
  }

  function doReset() {
    if (state.resetPhase === "sending") return;
    if (state.resetPhase === "confirm") { runReset(); return; }
    state.resetPhase = "confirm"; render();
    clearTimeout(confirmTimer);
    confirmTimer = setTimeout(function () {
      if (state.resetPhase === "confirm") { state.resetPhase = "idle"; render(); }
    }, 4000);
  }
  function runReset() {
    state.resetPhase = "sending"; render();
    fetch("/cmnd", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ restart: null })
    })
      .catch(function () {})
      .then(function () {
        state.resetPhase = "done"; state.online = false; render();
        clearTimeout(resetTimer);
        resetTimer = setTimeout(function () { state.resetPhase = "idle"; render(); }, 6000);
      });
  }

  function toggleTheme() {
    var html = document.documentElement;
    var next = html.getAttribute("data-theme") === "dark" ? "light" : "dark";
    html.setAttribute("data-theme", next);
    try { localStorage.setItem("cikon-theme", next); } catch (e) {}
    render();
  }
  function setSort(key) {
    if (state.sortKey === key) { state.sortDir = state.sortDir === "asc" ? "desc" : "asc"; }
    else { state.sortKey = key; state.sortDir = (key === "name" || key === "state") ? "asc" : "desc"; }
    render();
  }

  // ---- icons ----
  var SUN = '<svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><circle cx="9" cy="9" r="3.4"></circle><path d="M9 1.5v2M9 14.5v2M1.5 9h2M14.5 9h2M3.8 3.8l1.4 1.4M12.8 12.8l1.4 1.4M3.8 14.2l1.4-1.4M12.8 5.2l1.4-1.4"></path></svg>';
  var MOON = '<svg width="18" height="18" viewBox="0 0 18 18"><path d="M14.5 11.2A6 6 0 1 1 7.3 3.6a4.8 4.8 0 0 0 7.2 7.6z" fill="currentColor"></path></svg>';

  var RESET_LABEL = { idle: "Restart", confirm: "Confirm?", sending: "Restarting…", done: "Sent" };

  // ---- render ----
  // iOS Safari yanks page scroll to the top when an innerHTML-heavy re-render (the 5s
  // /tele poll, or tapping a sort header) swaps DOM near the viewport. Snapshot the
  // scroll offset, run the real render, then restore it if WebKit moved it — synchronously
  // and again next frame, since the jump can land after layout settles.
  function render() {
    var doc = document.scrollingElement || document.documentElement;
    var y = window.pageYOffset || doc.scrollTop || 0;
    renderInner();
    var restore = function () {
      var cur = window.pageYOffset || doc.scrollTop || 0;
      if (Math.abs(cur - y) > 2) window.scrollTo(0, y);
    };
    restore();
    requestAnimationFrame(restore);
  }
  function renderInner() {
    var t = state.tele || {};
    var dark = document.documentElement.getAttribute("data-theme") === "dark";

    // header
    $("dev-name").textContent = t.name || "—";
    // IP under the device name (System list no longer repeats it); mDNS name as tooltip
    $("dev-sub").textContent = t.ip || t.mdns || "";
    $("dev-sub").title = t.ip && t.mdns ? t.mdns : "";

    var badge = $("status");
    badge.classList.toggle("online", state.online);
    badge.title = state.online
      ? "Receiving telemetry from /tele"
      : "No response from /tele";
    $("status-text").textContent = state.online ? "Live" : "Offline";

    var rb = $("btn-restart");
    rb.classList.toggle("confirm", state.resetPhase === "confirm");
    $("restart-label").textContent = RESET_LABEL[state.resetPhase];

    // offline → dim whole dashboard; controls also inert
    var off = !state.online;
    document.querySelector(".wrap").classList.toggle("offline", off);
    $("switch-cards").classList.toggle("ctl-off", off);
    $("light-cards").classList.toggle("ctl-off", off);
    rb.classList.toggle("ctl-off", off && state.resetPhase === "idle");
    $("reconnect").hidden = !off;

    $("btn-theme").innerHTML = dark ? SUN : MOON;

    // hero
    $("uptime").textContent = t.uptime != null ? fmtUptime(t.uptime) : "—";
    $("since").textContent = "since " + fmtBoot(t.boot_time);
    renderMems(t);

    // capabilities (esp_chip_info features + flash/psram size)
    var CAP_ICON = {
      wifi: '<svg width="17" height="17" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M1.8 5.8A9 9 0 0 1 14.2 5.8"></path><path d="M4.3 8.4A5.3 5.3 0 0 1 11.7 8.4"></path><circle cx="8" cy="11.5" r="1" fill="currentColor" stroke="none"></circle></svg>',
      bt: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 5l6 6-3 3V2l3 3-6 6"></path></svg>',
      net: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="3" r="1.6"></circle><circle cx="3.4" cy="12" r="1.6"></circle><circle cx="12.6" cy="12" r="1.6"></circle><path d="M8 4.6 4 10.6M8 4.6l4 6M5 12h6"></path></svg>'
    };
    var FLASH_ICON = '<svg width="13" height="14" viewBox="0 0 14 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M3 1.5h5.5l3 3v10h-8.5z"></path><path d="M5.5 4v2.5M8 4v2.5" stroke-linecap="round"></path></svg>';
    function capType(f) {
      if (f === "wifi") return "wifi";
      if (f === "ble" || f === "bt") return "bt";
      if (f === "ieee802154" || f === "thread" || f === "zigbee") return "net";
      return null;
    }
    var fmtMB = function (b) { return (b / 1048576).toFixed(0) + " MB"; };
    var CAP_COLOR = { wifi: "#5f9ea8", bt: "#6f8fd6", net: "#a98ad0" };
    var CAP_SHORT = { wifi: "Wi‑Fi", ble: "BLE", bt: "BT", ieee802154: "Thread", thread: "Thread", zigbee: "Zigbee" };
    function sized(svg, px) { return svg.replace(/width="\d+" height="\d+"/, 'width="' + px + '" height="' + px + '"'); }
    var LINK_ICON = {
      wifi: '<svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M1.8 5.8A9 9 0 0 1 14.2 5.8"></path><path d="M4.3 8.4A5.3 5.3 0 0 1 11.7 8.4"></path><circle cx="8" cy="11.5" r="1" fill="currentColor" stroke="none"></circle></svg>',
      eth: '<svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="2.2" y="4.5" width="11.6" height="7" rx="1.4"></rect><path d="M5 11.5v-2M8 11.5v-2M11 11.5v-2"></path></svg>'
    };
    var TEMP_ICON = '<svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M9 9.3V3.2a1.5 1.5 0 0 0-3 0v6.1a3 3 0 1 0 3 0z"></path></svg>';
    var CPU_ICON = '<svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11.5a5.5 5.5 0 1 1 10 0"></path><path d="M8 8.5 10.4 6"></path></svg>';

    // Radio row (System list): chip capabilities from esp_chip_info(), deduped by label
    var seen = {};
    var radioHTML = (Array.isArray(t.features) ? t.features : []).map(function (f) {
      var ty = capType(f), short = CAP_SHORT[f] || f;
      if (!ty || seen[short]) return "";
      seen[short] = true;
      return '<span class="tag" title="' + esc(CAP_LABELS[f] || f) + '" style="color:' + CAP_COLOR[ty] + '">' + sized(CAP_ICON[ty], ty === "bt" ? 14 : 15) + '<span>' + esc(short) + '</span></span>';
    }).join("");

    // Live strip (hero card): active link (+ RSSI only on Wi-Fi), chip temp, CPU, flash — each only if present
    var dim = dark ? "#4d4a57" : "#ddd6ca";
    var liveHTML = "";
    if (t.link) {
      var isEth = t.link === "ethernet" || t.link === "eth", isThread = t.link === "thread", isWifi = t.link === "wifi";
      var name = isEth ? "Ethernet" : isThread ? "Thread" : isWifi ? (t.ssid || "Wi‑Fi") : t.link;
      var lcol = isEth ? "#6fae84" : isThread ? "#a98ad0" : "#5f9ea8";
      var licon = isEth ? LINK_ICON.eth : isThread ? sized(CAP_ICON.net, 15) : LINK_ICON.wifi;
      var ltitle = isWifi ? (t.ssid ? "Wi‑Fi · " + t.ssid : "Wi‑Fi") : name;
      var body;
      if (isWifi && typeof t.rssi === "number") {
        var lvl = t.rssi >= -55 ? 4 : t.rssi >= -67 ? 3 : t.rssi >= -78 ? 2 : 1;
        var rc = lvl >= 3 ? "#6fae84" : lvl === 2 ? "#d9a866" : "#cf8f86";
        var bars = ["4px", "6.5px", "9px", "11px"].map(function (bh, i) {
          return '<span style="height:' + bh + ';background:' + (i < lvl ? rc : dim) + '"></span>';
        }).join("");
        body = '<span>' + esc(name) + '</span><span class="live-bars">' + bars + '</span><b>' + t.rssi + ' dBm</b>';
      } else {
        body = '<b>' + esc(name) + '</b>';
      }
      liveHTML += '<span class="live-i" title="' + esc(ltitle) + '"><span class="live-ic" style="color:' + lcol + '">' + licon + '</span>' + body + '</span>';
    }
    if (typeof t.chip_temp === "number") {
      liveHTML += '<span class="live-i" title="Chip temperature"><span class="live-ic" style="color:#d9a866">' + TEMP_ICON + '</span><b>' + t.chip_temp.toFixed(1) + ' °C</b></span>';
    }
    if (t.cpu_freq) {
      liveHTML += '<span class="live-i" title="CPU frequency"><span class="live-ic">' + CPU_ICON + '</span><b>' + t.cpu_freq + ' MHz</b></span>';
    }
    if (t.flash_size) {
      liveHTML += '<span class="live-i" title="Flash size"><span class="live-ic" style="color:#d9b27a">' + FLASH_ICON + '</span><b>' + fmtMB(t.flash_size) + '</b></span>';
    }
    $("live").innerHTML = liveHTML;
    $("live").hidden = liveHTML === "";

    // last reset reason → shown in System list
    var resetLabel = null, resetColor = null;
    if (t.reset_reason) {
      resetLabel = t.reset_reason;
      resetColor = dark ? "#b4afbe" : "#a89c91";
    }

    var otaLabel = t.ota_state || "—";
    var otaColor = dark ? "#b4afbe" : "#a89c91";

    // switch cards
    // only names that actually have a value in /tele count as controls
    var switchKeys = (Array.isArray(t.switches) ? t.switches : []).filter(function (n) { return n && t[n] != null; });
    var swSig = switchKeys.join(",");
    if (swSig !== switchKeysBuilt) { buildSwitchCards(switchKeys); switchKeysBuilt = swSig; }
    $("controls-label").hidden = switchKeys.length === 0;
    [].forEach.call($("switch-cards").children, function (card) {
      var on = !!t[card._name];
      card.querySelector(".toggle").setAttribute("aria-checked", on ? "true" : "false");
      card.querySelector(".row-sub").textContent = on ? "On" : "Off";
    });

    // Lights — fully data-driven from t.lights
    renderLightCards();

    // system
    var rows = [
      ["Chip", (t.chip || "—") + (t.chip_rev != null ? " rev" + t.chip_rev : "") + (t.cores ? " · " + t.cores + (t.cores == 1 ? " core" : " cores") : ""), false, false],
      ["Device ID", t.id || "—", true, false]
    ];
    if (resetLabel) rows.push(["Last reset reason", resetLabel, false, false, resetColor]);
    rows.push(["OTA state", otaLabel, false, false, otaColor]);
    var sysHTML = rows.map(function (r) {
      var col = r[4] ? ' style="color:' + r[4] + '"' : "";
      return '<div class="item"><span class="k">' + esc(r[0]) + '</span>' +
        '<span class="v' + (r[2] ? " mono" : "") + (r[3] ? " ok" : "") + '"' + col + '>' + esc(r[1]) + "</span></div>";
    });
    if (radioHTML) sysHTML.splice(1, 0, '<div class="item"><span class="k">Radio</span><span class="tags">' + radioHTML + '</span></div>');
    $("sys-list").innerHTML = sysHTML.join("");

    // firmware tile: IDF is the primary (bold) line; version moves into a "?" tooltip next to it
    $("fw-app-idf").textContent = "IDF " + (t.idf || "—");
    $("fw-app-info").title = t.version || "—";
    var appBuiltEl = $("fw-app-built");
    if (t.app_build_time) { appBuiltEl.hidden = false; appBuiltEl.textContent = "built " + fmtBootSec(t.app_build_time); }
    else { appBuiltEl.hidden = true; }

    var hasBl = t.bootloader_version != null || !!t.bootloader_idf || !!t.bootloader_build_time;
    $("fw-bl-block").hidden = !hasBl;
    if (hasBl) {
      $("fw-bl-idf").textContent = "IDF " + (t.bootloader_idf || "—");
      $("fw-bl-info").title = "v" + (t.bootloader_version != null ? t.bootloader_version : "—");
      var blBuiltEl = $("fw-bl-built");
      if (t.bootloader_build_time) { blBuiltEl.hidden = false; blBuiltEl.textContent = "built " + fmtBootSec(t.bootloader_build_time); }
      else { blBuiltEl.hidden = true; }
    }

    // tasks
    var td = t.tasks_dict;
    var hasTasks = td && Object.keys(td).length > 0;
    $("tasks-section").hidden = !hasTasks;
    if (hasTasks) {
      $("task-count").textContent = Object.keys(td).length;
      $("tasks-action").textContent = state.tasksOpen ? "Hide" : "Show";
      $("tasks-toggle").classList.toggle("open", state.tasksOpen);
      $("task-table").hidden = !state.tasksOpen;

      // sort arrows
      var ths = document.querySelectorAll(".th");
      for (var i = 0; i < ths.length; i++) {
        var k = ths[i].getAttribute("data-sort");
        var base = { name: "Task", state: "State", prio: "Prio", stack: "Stack", cpu: "CPU" }[k];
        ths[i].textContent = base + (state.sortKey === k ? (state.sortDir === "asc" ? " ↑" : " ↓") : "");
      }

      if (state.tasksOpen) {
        var entries = Object.keys(td).map(function (name) { return { name: name, v: td[name] }; });
        var hasLive = !!state.cpu;
        var total = entries.reduce(function (a, e) { return a + (e.v.runtime_ticks || 0); }, 0) || 1;
        entries.forEach(function (e) {
          e.share = hasLive ? (state.cpu[e.name] || 0) : ((e.v.runtime_ticks || 0) / total * 100);
        });
        var dir = state.sortDir === "asc" ? 1 : -1;
        var cmp = {
          name: function (a, b) { return a.name.toLowerCase().localeCompare(b.name.toLowerCase()); },
          state: function (a, b) { return (STATE_ORDER[a.v.state] == null ? 9 : STATE_ORDER[a.v.state]) - (STATE_ORDER[b.v.state] == null ? 9 : STATE_ORDER[b.v.state]); },
          prio: function (a, b) { return (a.v.prio || 0) - (b.v.prio || 0); },
          stack: function (a, b) { return (a.v.stack || 0) - (b.v.stack || 0); },
          cpu: function (a, b) { return a.share - b.share; }
        }[state.sortKey];
        entries.sort(function (a, b) { return dir * cmp(a, b) || a.name.localeCompare(b.name); });

        $("task-body").innerHTML = entries.map(function (e) {
          var ss = stateStyle(e.v.state);
          var share = e.share;
          var pctStr = share >= 0.1 ? share.toFixed(1) + "%" : "<0.1%";
          var barW = Math.max(2, Math.min(100, share));
          return '<div class="task-row">' +
            '<span class="task-name">' + esc(e.name) + "</span>" +
            '<span class="task-state"><span class="pill" style="background:' + ss.bg + ";color:" + ss.color + '">' + esc(e.v.state || "—") + "</span></span>" +
            '<span class="task-prio">' + (e.v.prio != null ? e.v.prio : "—") + "</span>" +
            '<span class="task-stack">' + (e.v.stack != null ? e.v.stack + "B" : "—") + "</span>" +
            '<span class="task-cpu"><span class="bar"><span style="width:' + barW + '%"></span></span><span class="pct">' + pctStr + "</span></span>" +
            "</div>";
        }).join("");
      }
    }

    // footer
    $("footer").textContent = "© 2025–" + new Date().getFullYear() + " Cikon IoT Solutions";
  }

  // ---- init ----
  function init() {
    try {
      var saved = localStorage.getItem("cikon-theme");
      if (saved) document.documentElement.setAttribute("data-theme", saved);
    } catch (e) {}

    $("btn-restart").addEventListener("click", doReset);
    $("btn-theme").addEventListener("click", toggleTheme);
    $("tasks-toggle").addEventListener("click", function () { state.tasksOpen = !state.tasksOpen; render(); });
    var ths = document.querySelectorAll(".th");
    for (var i = 0; i < ths.length; i++) {
      ths[i].addEventListener("click", function () { setSort(this.getAttribute("data-sort")); });
    }

    render();
    pollLoop();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
