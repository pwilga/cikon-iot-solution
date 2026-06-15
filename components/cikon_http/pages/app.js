/* ── Theme ── */
const html = document.documentElement;
const themeIcon = document.getElementById('theme-icon');
function applyTheme(t) {
  html.setAttribute('data-theme', t);
  themeIcon.textContent = t === 'dark' ? '☀️' : '🌙';
  localStorage.setItem('theme', t);
}
applyTheme(localStorage.getItem('theme') || 'dark');
document.getElementById('theme-btn').addEventListener('click', () =>
  applyTheme(html.getAttribute('data-theme') === 'dark' ? 'light' : 'dark'));

/* ── Sort state (preserved across refreshes) ── */
const sortStates = {};

function applySort(table, key) {
  const s = sortStates[key];
  if (!s) return;
  const tbody = table.querySelector('tbody');
  const rows = [...tbody.querySelectorAll('tr')];
  rows.sort((a, b) => {
    const av = a.children[s.col].textContent.trim();
    const bv = b.children[s.col].textContent.trim();
    const an = parseFloat(av), bn = parseFloat(bv);
    const cmp = isNaN(an) || isNaN(bn) ? av.localeCompare(bv) : an - bn;
    return s.asc ? cmp : -cmp;
  });
  rows.forEach(r => tbody.appendChild(r));
  table.querySelectorAll('th').forEach((th, i) => {
    delete th.dataset.sort;
    if (i === s.col) th.dataset.sort = s.asc ? 'asc' : 'desc';
  });
}

function sortTable(th) {
  const table = th.closest('table');
  const key = table.closest('details')?.dataset.key || '_';
  const idx = [...th.parentNode.children].indexOf(th);
  const cur = sortStates[key];
  sortStates[key] = { col: idx, asc: !(cur?.col === idx && cur?.asc) };
  applySort(table, key);
}

/* ── Tele metadata ── */
const SKIP   = new Set(['uptime']);
const LABELS = { startup: 'Last Boot', onboard_led: 'Onboard LED', temperature: 'Temperature', tasks_dict: 'Tasks', rollback: 'OTA Rollback', ip: 'IP Address', pwm_led: 'PWM LED', neopixel: 'Neopixel' };
const ICONS  = { startup: '🕐', onboard_led: '💡', temperature: '🌡', tasks_dict: '📋', rollback: '🔄', ip: '🌐', pwm_led: '💡', neopixel: '🌈', ds18b20: '🌡' };
const UNITS  = { temperature: '°C' };
const SPANS  = { tasks_dict: 6, startup: 2 };

function label(k) { return LABELS[k] || k.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase()); }
function icon(k)  { return ICONS[k] || '📊'; }

/* ── Formatters ── */
function fmtHeap(b) { return (b / 1024).toFixed(0) + ' kB'; }

function fmtUptime(s) {
  s = Math.floor(s);
  if (s < 60) return s + 's';
  const m = Math.floor(s / 60) % 60, h = Math.floor(s / 3600) % 24, d = Math.floor(s / 86400);
  if (d > 0) return d + 'd ' + h + 'h';
  if (h > 0) return h + 'h ' + m + 'm';
  return m + 'm';
}

/* ── Card builders ── */
function cardNum(k, v) {
  const u = UNITS[k] || '';
  const disp = Number.isInteger(v) ? v : v.toFixed(1);
  return `<span class="card-icon">${icon(k)}</span><div class="card-val">${disp}${u}</div><div class="card-lbl">${label(k)}</div>`;
}

function cardBool(k, v) {
  return `<span class="card-icon">${icon(k)}</span><div class="card-val ${v ? 'g' : ''}">${v ? 'ON' : 'OFF'}</div><div class="card-lbl">${label(k)}</div>`;
}

function cardStr(k, v) {
  if (k === 'rollback') {
    const ok = v === 'n/a';
    return `<span class="card-icon">${icon(k)}</span><div class="card-val ${ok ? 'g' : 'r'}">${ok ? 'OK' : v}</div><div class="card-lbl">${label(k)}</div>`;
  }
  return `<span class="card-icon">${icon(k)}</span><div class="card-val mono">${v || '—'}</div><div class="card-lbl">${label(k)}</div>`;
}

function cardTasks(k, v) {
  const n = Object.keys(v).length;
  const rows = Object.entries(v).map(([name, t]) =>
    `<tr><td>${name}</td><td>${t.state || '—'}</td><td>${t.prio ?? '—'}</td><td>${t.stack ?? '—'}</td></tr>`
  ).join('');
  return `<details data-key="${k}"><summary>
    <span class="card-icon" style="margin:0">${icon(k)}</span>
    <div class="card-val" style="font-size:15px">${n}</div>
    <div class="card-lbl">${label(k)}</div>
  </summary>
  <div class="task-wrap"><table class="task-table">
    <thead><tr><th onclick="sortTable(this)">Task</th><th onclick="sortTable(this)">State</th><th onclick="sortTable(this)">Prio</th><th onclick="sortTable(this)">Stack</th></tr></thead>
    <tbody>${rows}</tbody>
  </table></div></details>`;
}

function cardObj(k, v) {
  const rows = Object.entries(v).map(([ek, ev]) =>
    `<div class="obj-row"><span class="obj-key">${ek}</span><span class="obj-val">${ev}</span></div>`
  ).join('');
  return `<span class="card-icon">${icon(k)}</span><div class="card-lbl">${label(k)}</div><div class="obj-list">${rows}</div>`;
}

function buildCard(k, v) {
  const el = document.createElement('div');
  let defSpan = 1, inner = '';

  if (typeof v === 'number') {
    inner = cardNum(k, v);
  } else if (typeof v === 'boolean') {
    inner = cardBool(k, v);
  } else if (typeof v === 'string') {
    inner = cardStr(k, v);
  } else if (v !== null && typeof v === 'object') {
    inner = k === 'tasks_dict' ? cardTasks(k, v) : cardObj(k, v);
  }

  const span = SPANS[k] ?? defSpan;
  el.className = `card s${span}`;
  el.innerHTML = inner;
  return el;
}

/* ── Render ── */
function renderTele(tele) {
  const sec = document.getElementById('tele-section');
  const open = new Set([...sec.querySelectorAll('details[open]')].map(d => d.dataset.key));
  sec.innerHTML = '';
  const TAIL = new Set(['tasks_dict']);
  const entries = Object.entries(tele).filter(([k]) => !SKIP.has(k));
  entries.sort(([a], [b]) => (TAIL.has(a) ? 1 : 0) - (TAIL.has(b) ? 1 : 0));
  for (const [k, v] of entries) {
    sec.appendChild(buildCard(k, v));
  }
  sec.querySelectorAll('details').forEach(d => { if (open.has(d.dataset.key)) d.open = true; });
  sec.querySelectorAll('.task-table').forEach(t => applySort(t, t.closest('details')?.dataset.key));
}

function setOnline(ok) {
  const p = document.getElementById('status-pill');
  p.className = 'status-pill' + (ok ? '' : ' offline');
  document.getElementById('status-text').textContent = ok ? 'Online' : 'Offline';
}

/* ── Load ── */
async function load() {
  try {
    const [info, tele] = await Promise.all([
      fetch('/info').then(r => r.json()),
      fetch('/tele').then(r => r.json()),
    ]);

    document.getElementById('device-name').textContent = info.app || '—';
    document.getElementById('device-chip').textContent = (info.chip || '—') + ' r' + (info.chip_rev || 0);
    document.getElementById('ms-heap').textContent = fmtHeap(info.free_heap);
    document.getElementById('ms-uptime').textContent = fmtUptime(info.uptime_s);
    document.getElementById('ms-chip').textContent = info.chip || '—';
    document.getElementById('fw-info').textContent = 'v' + info.version + ' · IDF ' + info.idf;

    setOnline(true);
    renderTele(tele);
  } catch (_) {
    setOnline(false);
  }
}

setInterval(load, 5000);
load();
