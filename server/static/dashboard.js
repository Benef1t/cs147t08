// RiseQuest dashboard: polls the server and draws live data, history and reliability.
"use strict";

const CSRF = document.querySelector('meta[name="csrf-token"]').content;
const $ = (id) => document.getElementById(id);
const tooltip = $("tooltip");
const SVGNS = "http://www.w3.org/2000/svg";

async function api(path, opts = {}) {
  const res = await fetch(path, {
    ...opts,
    headers: { "Content-Type": "application/json", "X-CSRF-Token": CSRF, ...(opts.headers || {}) },
  });
  if (res.status === 401) { location.href = "/login"; throw new Error("signed out"); }
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw Object.assign(new Error("HTTP " + res.status), { body });
  return body;
}

const fmtTime = (t) => new Date(t * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
const fmtDur = (s) => {
  if (s == null) return "–";
  s = Math.round(s);
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`;
};
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

function showTip(html, x, y) {
  tooltip.innerHTML = html;
  tooltip.hidden = false;
  const r = tooltip.getBoundingClientRect();
  let left = x + 14, top = y + 14;
  if (left + r.width > innerWidth - 8) left = x - r.width - 14;
  if (top + r.height > innerHeight - 8) top = y - r.height - 14;
  tooltip.style.left = left + "px";
  tooltip.style.top = top + "px";
}
const hideTip = () => { tooltip.hidden = true; };

function el(tag, attrs = {}, parent) {
  const n = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (parent) parent.appendChild(n);
  return n;
}

// ------------------------------------------------------------ live chart
const WINDOW_S = 60;
const samples = []; // {t, ax, ay, az}
const SERIES = [
  { key: "ax", name: "x", cls: "var(--series-1)" },
  { key: "ay", name: "y", cls: "var(--series-2)" },
  { key: "az", name: "z", cls: "var(--series-3)" },
];

function drawAccel() {
  const box = $("accel-chart");
  const W = box.clientWidth, H = box.clientHeight;
  const m = { l: 36, r: 24, t: 8, b: 22 };
  const now = Date.now() / 1000;
  const x = (t) => m.l + (W - m.l - m.r) * (1 - (now - t) / WINDOW_S);
  const yMax = Math.max(12, ...samples.flatMap((s) => [Math.abs(s.ax), Math.abs(s.ay), Math.abs(s.az)]));
  const lim = Math.ceil(yMax / 5) * 5;
  const y = (v) => m.t + (H - m.t - m.b) * (1 - (v + lim) / (2 * lim));

  const svg = el("svg", { viewBox: `0 0 ${W} ${H}` });
  const axis = el("g", { class: "axis" }, svg);
  for (let v = -lim; v <= lim; v += lim / 2) {
    el("line", { class: v === 0 ? "zero" : "gridline", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
    const t = el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, axis);
    t.textContent = v;
  }
  for (const s of [60, 45, 30, 15, 0]) {
    const t = el("text", { x: x(now - s), y: H - 4, "text-anchor": "middle" }, axis);
    t.textContent = s ? `-${s}s` : "now";
  }
  for (const s of SERIES) {
    const pts = samples.map((p) => `${x(p.t).toFixed(1)},${y(p[s.key]).toFixed(1)}`).join(" ");
    if (pts) el("polyline", { class: "line", points: pts, style: `stroke:${s.cls}` }, svg);
  }
  const cross = el("line", { class: "crosshair", y1: m.t, y2: H - m.b, visibility: "hidden" }, svg);
  const hit = el("rect", { x: m.l, y: 0, width: W - m.l - m.r, height: H, fill: "transparent" }, svg);
  hit.addEventListener("mousemove", (e) => {
    if (!samples.length) return;
    const px = e.offsetX * (W / box.clientWidth);
    let best = samples[0];
    for (const p of samples) if (Math.abs(x(p.t) - px) < Math.abs(x(best.t) - px)) best = p;
    cross.setAttribute("x1", x(best.t));
    cross.setAttribute("x2", x(best.t));
    cross.setAttribute("visibility", "visible");
    showTip(`<b>${(now - best.t).toFixed(1)} s ago</b><br>` +
      SERIES.map((s) => `<i class="sw s${SERIES.indexOf(s) + 1}"></i>${s.name} ${best[s.key].toFixed(2)} m/s²`).join("<br>"),
      e.clientX, e.clientY);
  });
  hit.addEventListener("mouseleave", () => { cross.setAttribute("visibility", "hidden"); hideTip(); });
  box.replaceChildren(svg);
}

// ------------------------------------------------------------ daily bars
function drawDaily(days) {
  const box = $("daily-chart");
  const W = box.clientWidth, H = box.clientHeight;
  const m = { l: 28, r: 8, t: 14, b: 22 };
  const max = Math.max(2, ...days.map((d) => d.completed));
  const step = (W - m.l - m.r) / days.length;
  const bw = Math.max(4, Math.min(28, step - 4));
  const y = (v) => m.t + (H - m.t - m.b) * (1 - v / max);
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}` });
  const axis = el("g", { class: "axis" }, svg);
  for (const v of [0, Math.ceil(max / 2), max]) {
    el("line", { class: v ? "gridline" : "zero", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
    el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, axis).textContent = v;
  }
  days.forEach((d, i) => {
    const cx = m.l + step * i + step / 2;
    const h = y(0) - y(d.completed);
    if (d.completed) {
      const r = Math.min(4, bw / 2, h);
      // rounded top, square baseline
      el("path", {
        class: "bar-rect",
        d: `M${cx - bw / 2},${y(0)} V${y(d.completed) + r} Q${cx - bw / 2},${y(d.completed)} ${cx - bw / 2 + r},${y(d.completed)} ` +
           `H${cx + bw / 2 - r} Q${cx + bw / 2},${y(d.completed)} ${cx + bw / 2},${y(d.completed) + r} V${y(0)} Z`,
      }, svg);
    }
    const every = Math.ceil(days.length / 7);
    const lastIdx = days.length - 1;
    // label every Nth day, always the last one, and drop a label that would crowd the last
    if ((i % every === 0 && lastIdx - i >= every) || i === lastIdx) {
      el("text", { x: cx, y: H - 4, "text-anchor": "middle" }, axis).textContent = d.day.slice(5);
    }
    const hit = el("rect", { x: cx - step / 2, y: m.t, width: step, height: H - m.t - m.b, fill: "transparent" }, svg);
    hit.addEventListener("mousemove", (e) => showTip(
      `<b>${d.day}</b><br>Completed ${d.completed}<br>Abandoned ${d.abandoned}<br>Locks ${d.locks}`, e.clientX, e.clientY));
    hit.addEventListener("mouseleave", hideTip);
  });
  box.replaceChildren(svg);
}

// ------------------------------------------------------------ live polling
async function pollLive() {
  let d;
  try { d = await api("/api/live"); } catch { return setStatus("offline", "server unreachable"); }
  const l = d.live;
  if (!l) return setStatus("offline", "no data from device yet");
  setStatus(d.online ? "online" : "offline", d.online ? `online · boot #${d.boot_id} · fw ${d.fw}` : `offline · last seen ${fmtDur(d.age_s)} ago`);

  if (d.online) {
    samples.push({ t: Date.now() / 1000, ax: +l.ax, ay: +l.ay, az: +l.az });
    while (samples.length && samples[0].t < Date.now() / 1000 - WINDOW_S) samples.shift();
  }
  drawAccel();

  const locked = l.latch === "locked";
  $("latch").innerHTML = `<span class="pill ${locked ? "bad" : "good"}">${locked ? "Locked" : "Unlocked"}</span>`;
  $("latch-sub").textContent = `${l.latch_cycles ?? 0} unlock cycles${l.alarm ? " · ALARM ringing" : ""}`;

  const bar = $("progress-bar");
  bar.style.width = (l.progress || 0) + "%";
  bar.classList.toggle("done", l.progress >= 100);
  $("progress-text").textContent = locked
    ? (l.attempt ? `${(l.active_ms / 1000).toFixed(1)} s · ${l.progress}%` : "waiting for flips")
    : "–";
  $("progress-sub").textContent = locked && l.attempt
    ? `${l.flips} flips${l.paused ? " · paused (keep flipping)" : ""}${l.shaking ? " · shaking ignored" : ""}`
    : " ";

  $("face").textContent = l.imu_ok ? ({ up: "Face up", down: "Face down", edge: "On edge" }[l.face] || "–") : "Sensor error";
  $("tilt").textContent = `tilt ${l.tilt}° · pitch ${l.pitch}° · roll ${l.roll}°`;
  $("ax").textContent = (+l.ax).toFixed(2);
  $("ay").textContent = (+l.ay).toFixed(2);
  $("az").textContent = (+l.az).toFixed(2);
  $("mag").textContent = `|a| ${Math.hypot(l.ax, l.ay, l.az).toFixed(2)} m/s²`;

  const hb = d.heartbeat || {};
  const rows = [
    ["Last heartbeat", hb.at ? fmtTime(hb.at) : "–"],
    ["Uptime", fmtDur(hb.up)],
    ["Wi-Fi RSSI", l.rssi != null ? `${l.rssi} dBm` : "–"],
    ["Queued records", l.queued ?? "–"],
    ["Queue overflow (this boot)", hb.ovf ?? "–"],
    ["Upload failures (this boot)", hb.fail ?? "–"],
    ["Free heap", hb.heap ? `${(hb.heap / 1024).toFixed(0)} KB` : "–"],
    ["Firmware / boot", `v${d.fw} / #${d.boot_id}`],
  ];
  $("health").innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("");
}

function setStatus(cls, text) {
  $("status").className = "status " + cls;
  $("status-text").textContent = text;
}

// ------------------------------------------------------------ slower panels
async function pollHistory() {
  try {
    const h = await api("/api/history?days=14");
    drawDaily(h.days);
    const tot = h.days.reduce((a, d) => ({ c: a.c + d.completed, x: a.x + d.abandoned }), { c: 0, x: 0 });
    $("daily-summary").textContent = `${tot.c} completed, ${tot.x} abandoned in 14 days`;
    $("attempts").innerHTML = h.attempts.length
      ? h.attempts.map((a) => `<tr><td>${fmtTime(a.at)}</td>
          <td><span class="pill ${a.outcome === "completed" ? "good" : "warn"}">${a.outcome}</span></td>
          <td>${a.active_s} s</td><td>${a.wall_s} s</td><td>${a.flips}</td><td>${a.rejected}</td></tr>`).join("")
      : `<tr><td colspan="6" class="muted">No attempts yet</td></tr>`;
  } catch (e) { console.warn(e); }

  try {
    const r = await api("/api/reliability");
    $("rel-note").textContent = `heartbeat every ${r.heartbeat_s}s; gaps > ${2.5 * r.heartbeat_s}s flagged`;
    const span = (list) => list.length
      ? list.map((g) => `${fmtDur(g.seconds)} @ ${fmtTime(g.from)}`).join("<br>") : "none";
    $("reliability").innerHTML = r.boots.length ? r.boots.map((b) => `<tr>
        <td>#${b.boot_id}</td><td>${esc(b.reset_reason)}</td><td>${b.fw ?? "–"}</td><td>${fmtDur(b.uptime_s)}</td>
        <td>${b.expected}</td><td>${b.received}</td>
        <td>${b.missing ? `<span class="pill warn">${b.missing}</span>` : "0"}</td>
        <td>${b.queue_overflow ? `<span class="pill bad">${b.queue_overflow}</span>` : "0"}</td>
        <td class="wrap">${span(b.upload_outages)}</td><td class="wrap">${span(b.heartbeat_gaps)}</td></tr>`).join("")
      : `<tr><td colspan="10" class="muted">No data yet</td></tr>`;
  } catch (e) { console.warn(e); }

  try {
    const ev = await api("/api/events?limit=40");
    $("events").innerHTML = ev.map((e) => `<li><time>${fmtTime(e.at)}</time><span><b>${esc(e.kind)}</b>
      <code>${esc(JSON.stringify(e.data))}</code></span></li>`).join("") || `<li class="muted">No events yet</li>`;
  } catch (e) { console.warn(e); }
}

// ------------------------------------------------------------ settings
const form = $("settings");
async function loadSettings() {
  const s = await api("/api/settings");
  form.alarm_enabled.checked = s.alarm_enabled;
  form.alarm_time.value = `${String(s.alarm_hour).padStart(2, "0")}:${String(s.alarm_minute).padStart(2, "0")}`;
  for (const k of ["timezone", "required_s", "pause_ms", "min_flips", "face_cos_pct", "heartbeat_s"]) form[k].value = s[k];
  $("settings-msg").textContent = `config version ${s.version}`;
}
form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const [h, m] = form.alarm_time.value.split(":").map(Number);
  const body = {
    alarm_enabled: form.alarm_enabled.checked, alarm_hour: h, alarm_minute: m, timezone: form.timezone.value,
  };
  for (const k of ["required_s", "pause_ms", "min_flips", "face_cos_pct", "heartbeat_s"]) body[k] = Number(form[k].value);
  try {
    const s = await api("/api/settings", { method: "POST", body: JSON.stringify(body) });
    $("settings-msg").textContent = `saved · version ${s.version} (device picks it up on next upload)`;
  } catch (err) {
    $("settings-msg").textContent = "error: " + JSON.stringify(err.body?.errors || err.message);
  }
});

$("release").addEventListener("click", async () => {
  if (!confirm("Release the latch without the flip task?")) return;
  try {
    await api("/api/commands/release", { method: "POST" });
    $("release-msg").textContent = "sent; the device will open on its next upload";
  } catch { $("release-msg").textContent = "failed"; }
});

$("logout").addEventListener("click", async () => {
  await api("/logout", { method: "POST" }).catch(() => {});
  location.href = "/login";
});

addEventListener("resize", () => { drawAccel(); pollHistory(); });

loadSettings().catch(console.warn);
pollLive();
pollHistory();
setInterval(pollLive, 1000);
setInterval(pollHistory, 10000);
