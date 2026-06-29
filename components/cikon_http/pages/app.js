/* ===== Cikon IoT Dashboard ===== */
(function () {
  "use strict";

  var POLL_MS = 5000;

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
  var confirmTimer = null, resetTimer = null, pollTimer = null;
  var prevTicks = null, prevTotal = null;  // for live (delta) CPU
  state.cpu = null;                         // { taskName: pct } or null

  var $ = function (id) { return document.getElementById(id); };

  // ---- helpers ----
  function fmtHeap(b) { return (b / 1024).toFixed(0) + " kB"; }
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
    return String(iso).replace("T", " ").replace("Z", "").slice(0, 16) + " UTC";
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
      .then(function (t) { computeCpu(t); state.tele = t; state.online = true; state.live = true; render(); })
      .catch(function () { if (state.live) { state.online = false; render(); } })
      .finally(function () { clearTimeout(to); });
  }

  function pollLoop() {
    loadTele().finally(function () {
      pollTimer = setTimeout(pollLoop, state.online ? POLL_MS : 2000);
    });
  }

  // ---- actions ----
  function toggleLed() {
    var t = state.tele, next = !t.onboard_led;
    t.onboard_led = next; render();
    fetch("/cmnd", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ onboard_led: next ? "on" : "off" })
    }).catch(function () {});
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
  function render() {
    var t = state.tele || {};
    var dark = document.documentElement.getAttribute("data-theme") === "dark";

    // header
    $("dev-name").textContent = t.name || "—";
    $("dev-sub").textContent = t.mdns || (t.name ? t.name + ".local" : "");

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
    $("led-card").classList.toggle("ctl-off", off);
    rb.classList.toggle("ctl-off", off && state.resetPhase === "idle");
    $("reconnect").hidden = !off;

    $("btn-theme").innerHTML = dark ? SUN : MOON;

    // hero
    $("uptime").textContent = t.uptime != null ? fmtUptime(t.uptime) : "—";
    $("since").textContent = "since " + fmtBoot(t.startup);
    $("free-heap").textContent = t.free_heap != null ? fmtHeap(t.free_heap) : "—";
    $("min-heap").textContent = "min seen " + (t.min_heap != null ? fmtHeap(t.min_heap) : "—");
    var pct = (t.free_heap && t.min_heap) ? Math.max(6, Math.min(100, (t.min_heap / t.free_heap) * 100)) : 60;
    $("heap-fill").style.width = pct + "%";

    // capabilities (esp_chip_info features + flash/psram size)
    var CAP_ICON = {
      wifi: '<svg width="17" height="17" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M1.8 5.8A9 9 0 0 1 14.2 5.8"></path><path d="M4.3 8.4A5.3 5.3 0 0 1 11.7 8.4"></path><circle cx="8" cy="11.5" r="1" fill="currentColor" stroke="none"></circle></svg>',
      bt: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 5l6 6-3 3V2l3 3-6 6"></path></svg>',
      net: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="3" r="1.6"></circle><circle cx="3.4" cy="12" r="1.6"></circle><circle cx="12.6" cy="12" r="1.6"></circle><path d="M8 4.6 4 10.6M8 4.6l4 6M5 12h6"></path></svg>'
    };
    var FLASH_ICON = '<svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="4.5" y="4.5" width="7" height="7" rx="1.2"></rect><path d="M6.5 1.8v2M9.5 1.8v2M6.5 12.2v2M9.5 12.2v2M1.8 6.5h2M1.8 9.5h2M12.2 6.5h2M12.2 9.5h2"></path></svg>';
    var PSRAM_ICON = '<svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="5" width="12" height="6" rx="1"></rect><path d="M5 5v6M8 5v6M11 5v6"></path></svg>';
    function capType(f) {
      if (f === "wifi") return "wifi";
      if (f === "ble" || f === "bt") return "bt";
      if (f === "ieee802154" || f === "thread" || f === "zigbee") return "net";
      return null;
    }
    var fmtMB = function (b) { return (b / 1048576).toFixed(0) + " MB"; };
    var fmtFS = function (b) { return b < 1048576 ? Math.round(b / 1024) + " kB" : (b / 1048576).toFixed(1) + " MB"; };
    var CAP_COLOR = { wifi: "#5f9ea8", bt: "#6f8fd6", net: "#a98ad0" };
    var capsHTML = (Array.isArray(t.features) ? t.features : []).map(function (f) {
      var ty = capType(f);
      if (!ty) return "";
      return '<span class="cap" title="' + esc(CAP_LABELS[f] || f) + '" style="color:' + CAP_COLOR[ty] + '">' + CAP_ICON[ty] + '</span>';
    }).join("");
    if (t.flash_size) {
      capsHTML += '<span class="cap cap-mem" title="Flash size"><span class="cap-i" style="color:#d9a866">' + FLASH_ICON + '</span><b>' + fmtMB(t.flash_size) + '</b></span>';
    }
    if (t.psram_size) {
      capsHTML += '<span class="cap cap-mem" title="Pseudo-static RAM"><span class="cap-i" style="color:#cf8f86">' + PSRAM_ICON + '</span><b>' + fmtMB(t.psram_size) + '</b><span class="cap-tag">PSRAM</span></span>';
    }
    if (t.cpu_freq) {
      capsHTML += '<span class="cap cap-mem" title="CPU frequency"><span class="cap-i" style="color:#6f9ec4"><svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11.5a5.5 5.5 0 1 1 10 0"></path><path d="M8 8.5 10.4 6"></path></svg></span><b>' + t.cpu_freq + ' MHz</b></span>';
    }
    var capsEl = $("caps");
    capsEl.hidden = capsHTML === "";
    capsEl.innerHTML = capsHTML;
    $("hw-cell").hidden = capsHTML === "";

    // live status chips (all optional)
    var dim = dark ? "#4d4a57" : "#ddd6ca";
    var statusHTML = "";

    // connection link
    if (t.link) {
      var linkIsEth = t.link === "ethernet" || t.link === "eth";
      var linkLabel = linkIsEth ? "Ethernet" : (t.link === "wifi" ? (t.ssid || "Wi-Fi") : t.link);
      var linkColor = linkIsEth ? "#6fae84" : "#5f9ea8";
      var linkIcon = linkIsEth
        ? '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="2.2" y="4.5" width="11.6" height="7" rx="1.4"></rect><path d="M5 11.5v-2M8 11.5v-2M11 11.5v-2"></path></svg>'
        : '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M1.8 5.8A9 9 0 0 1 14.2 5.8"></path><path d="M4.3 8.4A5.3 5.3 0 0 1 11.7 8.4"></path><circle cx="8" cy="11.5" r="1" fill="currentColor" stroke="none"></circle></svg>';
      statusHTML += '<span class="cap cap-mem" title="Uplink"><span class="cap-i" style="color:' + linkColor + '">' + linkIcon + '</span><b>' + esc(linkLabel) + '</b></span>';
    }

    // wifi rssi
    if (typeof t.rssi === "number") {
      var lvl = t.rssi >= -55 ? 4 : t.rssi >= -67 ? 3 : t.rssi >= -78 ? 2 : 1;
      var rc = lvl >= 3 ? "#6fae84" : lvl === 2 ? "#d9a866" : "#cf8f86";
      var heights = ["6px", "9px", "12px", "15px"];
      var bars = heights.map(function (h, i) {
        return '<span style="width:3px;height:' + h + ';background:' + (i < lvl ? rc : dim) + ';border-radius:1px;display:block"></span>';
      }).join("");
      var rssiTitle = "Wi-Fi signal strength";
      statusHTML += '<span class="cap cap-mem" title="' + esc(rssiTitle) + '"><span style="display:inline-flex;align-items:flex-end;gap:2px;height:15px;flex:none">' + bars + '</span><b>' + t.rssi + ' dBm</b></span>';
    }

    // chip temperature
    if (typeof t.chip_temp === "number") {
      statusHTML += '<span class="cap cap-mem" title="Chip temperature"><span class="cap-i" style="color:#d9a866"><svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M9 9.3V3.2a1.5 1.5 0 0 0-3 0v6.1a3 3 0 1 0 3 0z"></path></svg></span><b>' + Math.round(t.chip_temp) + ' °C</b></span>';
    }

    // cpu frequency moved to capabilities row (first row)

    // filesystem usage
    if (t.fs_total && t.fs_used != null) {
      var fsPct = Math.round(t.fs_used / t.fs_total * 100);
      var fsTitle = "Filesystem · " + fmtFS(t.fs_used) + " / " + fmtFS(t.fs_total);
      statusHTML += '<span class="cap cap-mem" title="' + esc(fsTitle) + '"><span class="cap-i" style="color:#5f9ea8"><svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><ellipse cx="8" cy="4" rx="5" ry="2"></ellipse><path d="M3 4v8c0 1.1 2.2 2 5 2s5-.9 5-2V4"></path></svg></span><span class="fsbar"><span style="width:' + fsPct + '%"></span></span><b>FS ' + fsPct + '%</b></span>';
    }

    // last reset reason → shown in System list (below Firmware), not as a chip
    var resetLabel = null, resetColor = null;
    if (t.reset_reason) {
      var RESET_MAP = {
        ESP_RST_UNKNOWN: ["Unknown", false],
        ESP_RST_POWERON: ["Power-on", false],
        ESP_RST_EXT: ["External pin", false],
        ESP_RST_SW: ["Software", false],
        ESP_RST_PANIC: ["Panic", true],
        ESP_RST_INT_WDT: ["Int watchdog", true],
        ESP_RST_TASK_WDT: ["Task watchdog", true],
        ESP_RST_WDT: ["Watchdog", true],
        ESP_RST_DEEPSLEEP: ["Deep sleep", false],
        ESP_RST_BROWNOUT: ["Brownout", true],
        ESP_RST_SDIO: ["SDIO", false],
        ESP_RST_USB: ["USB", false],
        ESP_RST_JTAG: ["JTAG", false],
        ESP_RST_EFUSE: ["eFuse error", true],
        ESP_RST_PWR_GLITCH: ["Power glitch", true],
        ESP_RST_CPU_LOCKUP: ["CPU lockup", true]
      };
      var ri = RESET_MAP[String(t.reset_reason).toUpperCase()] || [t.reset_reason, false];
      resetLabel = ri[0];
      resetColor = ri[1] ? "#cf8f86" : (dark ? "#b4afbe" : "#a89c91");
    }

    // OTA image state → ["Label", severity]  (ok | bad | muted)
    function parseOtaState(s) { return String(s || "").split(":").pop().trim().toUpperCase(); }
    var OTA_MAP = {
      ESP_OTA_IMG_NEW: ["New", "muted"],
      ESP_OTA_IMG_PENDING_VERIFY: ["Pending verify", "muted"],
      ESP_OTA_IMG_VALID: ["Valid", "ok"],
      ESP_OTA_IMG_INVALID: ["Invalid", "bad"],
      ESP_OTA_IMG_ABORTED: ["Aborted", "bad"],
      ESP_OTA_IMG_UNDEFINED: ["Undefined", "muted"],
      UNKNOWN: ["Unknown", "muted"]
    };
    var oi = OTA_MAP[parseOtaState(t.ota_state)] || [t.ota_state || "—", "muted"];
    var otaLabel = oi[0];
    var otaColor = oi[1] === "ok" ? "var(--accent,#5aa06f)" : oi[1] === "bad" ? "#cf8f86" : (dark ? "#b4afbe" : "#a89c91");
    var hasOtaFail = !!t.ota_last_failed_partition;
    var otaFailVal = "";
    if (hasOtaFail) {
      var ofm = OTA_MAP[parseOtaState(t.ota_last_failed_partition)];
      var ofPart = String(t.ota_last_failed_partition).split(":")[0].trim();
      var ofState = ofm ? ofm[0] : t.ota_last_failed_partition;
      otaFailVal = ofPart ? ofPart + " · " + ofState : ofState;
    }

    var statusEl = $("status-chips");
    statusEl.hidden = statusHTML === "";
    statusEl.innerHTML = statusHTML;
    $("status-cell").hidden = statusHTML === "";

    // controls
    var led = !!t.onboard_led;
    var lt = $("led-toggle");
    lt.setAttribute("aria-checked", led ? "true" : "false");
    $("led-sub").textContent = led ? "On" : "Off";

    // system
    var rows = [
      ["Chip", (t.chip || "—") + (t.chip_rev != null ? " rev" + t.chip_rev : "") + (t.cores ? " · " + t.cores + (t.cores == 1 ? " core" : " cores") : ""), false, false],
      ["Device ID", t.id || "—", true, false],
      ["IP address", t.ip || "—", true, false],
      ["Firmware", (t.version || "—") + " · IDF " + (t.idf || "—"), true, false]
    ];
    if (resetLabel) rows.push(["Last reset reason", resetLabel, false, false, resetColor]);
    rows.push(["OTA state", otaLabel, false, false, otaColor]);
    if (hasOtaFail) rows.push(["OTA last failed", otaFailVal, false, false, "#cf8f86"]);
    $("sys-list").innerHTML = rows.map(function (r) {
      var col = r[4] ? ' style="color:' + r[4] + '"' : "";
      return '<div class="item"><span class="k">' + esc(r[0]) + '</span>' +
        '<span class="v' + (r[2] ? " mono" : "") + (r[3] ? " ok" : "") + '"' + col + '>' + esc(r[1]) + "</span></div>";
    }).join("");

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

    $("led-toggle").addEventListener("click", toggleLed);
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
