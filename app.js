/*
 * FLOOD-SENTINEL dashboard
 * Live System: reads the ESP32 data stored by /api/telemetry (Vercel)
 * Simulation : runs the same model (model.js) on values you enter or on the paper's trial data
 */
(function () {
  const $ = (id) => document.getElementById(id);
  const fmt = (v, d = 1) => (v === null || v === undefined || isNaN(v) ? '--' : Number(v).toFixed(d));
  const ONLINE_MAX_AGE_MS = 75000;   // ESP32 posts every 30 s
  const LIVE_POLL_MS = 10000;
  const HISTORY_POLL_MS = 60000;

  let mode = 'live';
  let liveTimer = null, historyTimer = null, playTimer = null;
  let simHistory = [];          // {label, prob}
  let simEvents = [];           // {label, cls}
  let simLastCls = 0;
  let currentTrialRow = -1;
  let chartTime = null, chartRain = null;

  // ------------------------------------------------------------ static parts
  function buildRules() {
    const m = FS_MODEL;
    const ranges = [`below ${m.tRestricted}%`, `${m.tRestricted}% to below ${m.tMoreRestricted}%`,
                    `${m.tMoreRestricted}% to below ${m.tImpassable}%`, `${m.tImpassable}% and above`];
    $('rules').innerHTML = FS_CLASSES.map((c, i) =>
      `<tr><td>${ranges[i]}</td><td>${c.name}</td><td><span class="chip" style="--c:${c.color}">${c.led[0].toUpperCase() + c.led.slice(1)}</span></td></tr>`).join('');
    const marks = [0, m.tRestricted, m.tMoreRestricted, m.tImpassable, 100];
    const zones = document.querySelectorAll('.zone');
    for (let i = 0; i < 4; i++) zones[i].style.width = (marks[i + 1] - marks[i]) + '%';
    $('scale-labels').innerHTML = marks.map((v) => `<span style="left:${v}%">${v}%</span>`).join('');
  }

  function buildTrialTable() {
    $('trial-body').innerHTML = FS_TRIAL.map((r, i) => {
      const p = fsProbability(r.rain, r.pook, r.bay);
      const c = FS_CLASSES[fsClassIndex(p)];
      const same = Math.abs(Math.round(p * 10) / 10 - r.paperProb) < 0.001;
      return `<tr data-row="${i}"><td>${r.time}</td><td>${fmt(r.rain)}</td><td>${fmt(r.pook)}</td><td>${fmt(r.bay)}</td>
        <td>${r.obs}</td><td class="${same ? 'match' : ''}">${fmt(p)}</td><td>${fmt(r.paperProb)}</td>
        <td><span class="chip" style="--c:${c.color}">${c.name}</span></td></tr>`;
    }).join('');
    document.querySelectorAll('#trial-body tr').forEach((tr) =>
      tr.addEventListener('click', () => { setMode('sim'); stopPlay(); loadTrialRow(+tr.dataset.row); }));
  }

  // ------------------------------------------------------------ rendering
  function render(rain, pook, bay, probFromDevice) {
    const hasInputs = [rain, pook, bay].every((v) => v !== null && v !== undefined && !isNaN(v));
    const prob = probFromDevice !== undefined && probFromDevice !== null ? probFromDevice
               : hasInputs ? fsProbability(rain, pook, bay) : null;
    $('v-rain').textContent = fmt(rain);
    $('v-pook').textContent = fmt(pook);
    $('v-bay').textContent = fmt(bay);

    if (prob === null) {
      $('prob').textContent = '--';
      $('road-name').textContent = '--';
      $('road-name').style.color = '';
      $('road-desc').textContent = 'Waiting for data';
      document.querySelectorAll('.led').forEach((l) => l.classList.remove('on'));
      $('calc').textContent = 'Waiting for sensor readings…';
      $('sms-preview').textContent = '--';
      return null;
    }
    const ci = fsClassIndex(prob);
    const c = FS_CLASSES[ci];
    $('prob').textContent = fmt(prob);
    $('needle').style.left = Math.min(100, Math.max(0, prob)) + '%';
    $('road-name').textContent = c.name.toUpperCase();
    $('road-name').style.color = c.color;
    $('road-desc').textContent = ['Safe for all vehicles', 'Two-wheel vehicles not advised to pass',
      'Three- and four-wheel vehicles not advised to pass', 'No vehicles allowed to pass'][ci];
    document.querySelectorAll('.led').forEach((l) => l.classList.toggle('on', l.dataset.led === c.led));
    $('sms-preview').textContent = FS_SMS_TEXTS[ci];

    if (hasInputs) {
      const m = FS_MODEL;
      const z = fsLogit(rain, pook, bay);
      const s = (v) => (v < 0 ? '−' + Math.abs(v).toFixed(4) : v.toFixed(4));
      const rule = ci === 3 ? `≥ ${m.tImpassable}` : ci === 2 ? `≥ ${m.tMoreRestricted}` : ci === 1 ? `≥ ${m.tRestricted}` : `< ${m.tRestricted}`;
      $('calc').textContent =
        `z = b0 + b1·Rainfall + b2·Pook + b3·Bay\n` +
        `  = ${s(m.b0)} + ${m.bRain}×${fmt(rain)} + ${m.bPook}×${fmt(pook)} + ${m.bBay}×${fmt(bay)}\n` +
        `  = ${z.toFixed(4)}\n` +
        `P = 100 / (1 + e^(−z)) = ${fmt(fsProbability(rain, pook, bay), 1)} %\n` +
        `${fmt(prob)} % ${rule} %  →  ${c.name.toUpperCase()} (${c.led.toUpperCase()} LED)`;
    }
    updateRainChart(pook, bay, rain, prob);
    return { prob, ci };
  }

  // ------------------------------------------------------------ charts
  function initCharts() {
    if (typeof Chart === 'undefined') return;
    Chart.defaults.color = '#8d9ab5';
    Chart.defaults.borderColor = '#24314f';
    Chart.defaults.font.family = 'Inter, sans-serif';
    chartTime = new Chart($('chart-time'), {
      type: 'line',
      data: { labels: [], datasets: [{ label: 'Flood probability (%)', data: [], borderColor: '#38bdf8',
        backgroundColor: 'rgba(56,189,248,0.15)', fill: true, tension: 0.25, pointRadius: 3 }] },
      options: { responsive: true, maintainAspectRatio: false, animation: false,
        scales: { y: { min: 0, max: 100, title: { display: true, text: 'Flood probability (%)' } },
                  x: { title: { display: true, text: 'Time' }, ticks: { maxRotation: 0, autoSkip: true, maxTicksLimit: 8 } } },
        plugins: { legend: { display: false } } },
    });
    chartRain = new Chart($('chart-rain'), {
      type: 'scatter',
      data: { datasets: [
        { label: 'Model', data: [], showLine: true, borderColor: '#38bdf8', pointRadius: 0, borderWidth: 2 },
        { label: 'Trial readings', data: FS_TRIAL.map((r) => ({ x: r.rain, y: fsProbability(r.rain, r.pook, r.bay) })),
          backgroundColor: '#eab308', pointRadius: 4 },
        { label: 'Current', data: [], backgroundColor: '#ef4444', pointRadius: 7 },
      ] },
      options: { responsive: true, maintainAspectRatio: false, animation: false,
        scales: { x: { min: 0, title: { display: true, text: 'Rainfall (mm)' } },
                  y: { min: 0, max: 100, title: { display: true, text: 'Flood probability (%)' } } },
        plugins: { legend: { labels: { boxWidth: 10 } } } },
    });
  }

  function updateRainChart(pook, bay, rain, prob) {
    if (!chartRain) return;
    const p = isNaN(pook) || pook === null ? 0 : pook;
    const b = isNaN(bay) || bay === null ? 0 : bay;
    const xmax = Math.max(20, Math.ceil(((rain || 0) + 5) / 5) * 5);
    const pts = [];
    for (let x = 0; x <= xmax; x += xmax / 80) pts.push({ x: +x.toFixed(2), y: fsProbability(x, p, b) });
    chartRain.data.datasets[0].data = pts;
    chartRain.data.datasets[0].label = `Model (Pook ${fmt(p)} cm, Bay ${fmt(b)} cm)`;
    chartRain.data.datasets[2].data = rain !== null && !isNaN(rain) ? [{ x: rain, y: prob }] : [];
    chartRain.options.scales.x.max = xmax;
    chartRain.update();
  }

  function setTimeChart(labels, values) {
    if (!chartTime) return;
    chartTime.data.labels = labels;
    chartTime.data.datasets[0].data = values;
    chartTime.update();
  }

  // ------------------------------------------------------------ SMS log
  function renderSmsLog(events, note) {
    $('sms-log').innerHTML = events.length
      ? events.slice().reverse().map((e) => {
          const c = FS_CLASSES[e.cls];
          return `<li><span class="t">${e.label}</span><span><span class="chip" style="--c:${c.color}">${c.sms}</span> &rarr; sent to 10 numbers</span></li>`;
        }).join('')
      : `<li class="muted">${note}</li>`;
    $('sms-count').textContent = events.length ? `${events.length} event(s) × 10 = ${events.length * 10} SMS` : '';
  }

  // ------------------------------------------------------------ simulation
  function simValues() {
    return { rain: +$('in-rain').value, pook: +$('in-pook').value, bay: +$('in-bay').value };
  }

  function setInputs(rain, pook, bay) {
    [['rain', rain], ['pook', pook], ['bay', bay]].forEach(([k, v]) => {
      $('in-' + k).value = v; $('num-' + k).value = v;
    });
  }

  function simStep(label) {
    const v = simValues();
    const r = render(v.rain, v.pook, v.bay);
    if (!r) return;
    const lbl = label || new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    simHistory.push({ label: lbl, prob: +r.prob.toFixed(1) });
    if (simHistory.length > 60) simHistory.shift();
    if (r.ci !== simLastCls) {
      simEvents.push({ label: lbl, cls: r.ci });
      simLastCls = r.ci;
    }
    setTimeChart(simHistory.map((h) => h.label), simHistory.map((h) => h.prob));
    renderSmsLog(simEvents, 'No change yet – the road starts as Passable.');
  }

  function loadTrialRow(i) {
    const r = FS_TRIAL[i];
    currentTrialRow = i;
    document.querySelectorAll('#trial-body tr').forEach((tr) => tr.classList.toggle('current', +tr.dataset.row === i));
    setInputs(r.rain, r.pook, r.bay);
    simStep('Sep 1 ' + r.time);
  }

  function resetSim() {
    stopPlay();
    simHistory = []; simEvents = []; simLastCls = 0; currentTrialRow = -1;
    document.querySelectorAll('#trial-body tr').forEach((tr) => tr.classList.remove('current'));
    setInputs(0, 0, 0);
    simStep('start');
  }

  function playTrial() {
    if (playTimer) { stopPlay(); return; }
    setMode('sim');
    simHistory = []; simEvents = []; simLastCls = 0;
    let i = 0;
    $('btn-play').innerHTML = '&#10074;&#10074; Pause';
    loadTrialRow(i);
    playTimer = setInterval(() => {
      i++;
      if (i >= FS_TRIAL.length) { stopPlay(); return; }
      loadTrialRow(i);
    }, 1500);
  }

  function stopPlay() {
    if (playTimer) clearInterval(playTimer);
    playTimer = null;
    $('btn-play').innerHTML = '&#9654; Play trial';
  }

  function bindSimInputs() {
    ['rain', 'pook', 'bay'].forEach((k) => {
      $('in-' + k).addEventListener('input', (e) => { $('num-' + k).value = e.target.value; stopPlay(); simStep(); });
      $('num-' + k).addEventListener('change', (e) => {
        const el = $('in-' + k);
        const v = Math.min(+el.max, Math.max(0, +e.target.value || 0));
        el.value = v; e.target.value = v; stopPlay(); simStep();
      });
    });
    $('btn-play').addEventListener('click', playTrial);
    $('btn-reset').addEventListener('click', resetSim);
  }

  // ------------------------------------------------------------ live system
  function setStatus(kind, text) {
    $('status').className = 'status ' + kind;
    $('status-text').textContent = text;
  }

  function ago(ms) {
    const s = Math.round(ms / 1000);
    if (s < 60) return s + ' s ago';
    if (s < 3600) return Math.round(s / 60) + ' min ago';
    if (s < 86400) return Math.round(s / 3600) + ' h ago';
    return Math.round(s / 86400) + ' day(s) ago';
  }

  async function pollLive() {
    try {
      const res = await fetch('/api/telemetry', { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const d = await res.json();
      if (mode !== 'live') return;
      const hasData = d.rainfall_mm !== undefined && d.timestamp;
      if (!hasData) {
        setStatus('offline', 'No data from the ESP32 yet');
        render(null, null, null);
        return;
      }
      const age = Date.now() - d.timestamp;
      const pook = d.pook_cm ?? (d.pook_mm !== undefined ? d.pook_mm / 10 : null);
      const bay = d.bay_cm ?? (d.bay_mm !== undefined ? d.bay_mm / 10 : null);
      render(d.rainfall_mm, pook, bay, d.flood_prob);
      if (age <= ONLINE_MAX_AGE_MS) setStatus('online', 'System online · updated ' + ago(age));
      else setStatus('offline', 'System offline · last data ' + ago(age));
      $('sms-info').textContent = `SMS sent by the device since it started: ${d.sms_sent ?? 0}. ` +
        'One SMS goes to all 10 registered numbers each time the road condition changes.';
    } catch (e) {
      if (mode === 'live') setStatus('offline', 'Cannot reach the server');
    }
  }

  async function pollHistory() {
    try {
      const res = await fetch('/api/history', { cache: 'no-store' });
      if (!res.ok) return;
      const h = await res.json();
      if (mode !== 'live' || !Array.isArray(h)) return;
      const pts = h.filter((p) => p.p !== undefined);
      const label = (t) => new Date(t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      setTimeChart(pts.map((p) => label(p.t)), pts.map((p) => +(+p.p).toFixed(1)));
      const events = [];
      let last = null;
      pts.forEach((p) => {
        const ci = p.c !== undefined ? p.c : fsClassIndex(p.p);
        if (last !== null && ci !== last) events.push({ label: label(p.t), cls: ci });
        last = ci;
      });
      renderSmsLog(events, 'No road-condition change in the recent history.');
    } catch (e) { /* history is optional */ }
  }

  // ------------------------------------------------------------ mode switching
  function setMode(m) {
    if (m === mode) return;
    mode = m;
    $('tab-live').classList.toggle('active', m === 'live');
    $('tab-sim').classList.toggle('active', m === 'sim');
    $('tab-live').setAttribute('aria-selected', m === 'live');
    $('tab-sim').setAttribute('aria-selected', m === 'sim');
    document.body.classList.toggle('sim-mode', m === 'sim');
    clearInterval(liveTimer); clearInterval(historyTimer);
    if (m === 'live') {
      stopPlay();
      $('chart1-title').textContent = 'Flood probability over time (device)';
      setTimeChart([], []);
      renderSmsLog([], 'Loading…');
      setStatus('', 'Connecting…');
      pollLive(); pollHistory();
      liveTimer = setInterval(pollLive, LIVE_POLL_MS);
      historyTimer = setInterval(pollHistory, HISTORY_POLL_MS);
    } else {
      setStatus('sim', 'Simulation – not live data');
      $('chart1-title').textContent = 'Flood probability over time (simulation)';
      $('sms-info').textContent = 'Simulated: one SMS goes to all 10 registered numbers each time the road condition changes. ' +
        'On the device, a drop to a lower level is confirmed for 5 minutes before its SMS is sent.';
      if (!simHistory.length) { simStep('start'); return; }
      const v = simValues();
      render(v.rain, v.pook, v.bay);
      setTimeChart(simHistory.map((h) => h.label), simHistory.map((h) => h.prob));
      renderSmsLog(simEvents, 'No change yet – the road starts as Passable.');
    }
  }

  // ------------------------------------------------------------ start
  document.addEventListener('DOMContentLoaded', () => {
    buildRules();
    buildTrialTable();
    initCharts();
    bindSimInputs();
    $('tab-live').addEventListener('click', () => setMode('live'));
    $('tab-sim').addEventListener('click', () => setMode('sim'));
    mode = '';
    const start = new URLSearchParams(location.search).get('mode') === 'sim' ? 'sim' : 'live';
    setMode(start);
  });
})();
