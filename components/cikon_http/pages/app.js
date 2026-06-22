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
  var confirmTimer = null, resetTimer = null;
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
    fetch("/tele", { cache: "no-store" })
      .then(function (r) { if (!r.ok) throw new Error("bad"); return r.json(); })
      .then(function (t) { computeCpu(t); state.tele = t; state.online = true; state.live = true; render(); })
      .catch(function () { if (state.live) { state.online = false; render(); } });
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
    $("dev-sub").textContent = t.ip || "";

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

    $("btn-theme").innerHTML = dark ? SUN : MOON;

    // hero
    $("uptime").textContent = t.uptime != null ? fmtUptime(t.uptime) : "—";
    $("since").textContent = "since " + fmtBoot(t.startup);
    $("free-heap").textContent = t.free_heap != null ? fmtHeap(t.free_heap) : "—";
    $("min-heap").textContent = "min seen " + (t.min_heap != null ? fmtHeap(t.min_heap) : "—");
    var pct = (t.free_heap && t.min_heap) ? Math.max(6, Math.min(100, (t.min_heap / t.free_heap) * 100)) : 60;
    $("heap-fill").style.width = pct + "%";

    // controls
    var led = !!t.onboard_led;
    var lt = $("led-toggle");
    lt.setAttribute("aria-checked", led ? "true" : "false");
    $("led-sub").textContent = led ? "On" : "Off";

    // system
    var rows = [
      ["Name", t.name || "—", false, false],
      ["App", t.app || "—", false, false],
      ["Chip", (t.chip || "—") + (t.chip_rev != null ? " rev" + t.chip_rev : "") + (t.cores ? " · " + t.cores + " cores" : ""), false, false],
      ["Device ID", t.id || "—", true, false],
      ["IP address", t.ip || "—", true, false],
      ["Firmware", (t.version || "—") + " · IDF " + (t.idf || "—"), true, false],
      ["OTA rollback", t.rollback || "—", false, !t.rollback || t.rollback === "n/a"]
    ];
    $("sys-list").innerHTML = rows.map(function (r) {
      return '<div class="item"><span class="k">' + esc(r[0]) + '</span>' +
        '<span class="v' + (r[2] ? " mono" : "") + (r[3] ? " ok" : "") + '">' + esc(r[1]) + "</span></div>";
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
    loadTele();
    setInterval(loadTele, POLL_MS);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
