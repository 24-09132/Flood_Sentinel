/*
 * FLOOD-SENTINEL shared page parts: navigation, monitor dashboard, telemetry stream,
 * event log, data recorder and Excel export. Used by index.html (Live) and simulation.html.
 */
(function () {
  const $ = (id) => document.getElementById(id);
  const fmt = (v, d = 1) => (v === null || v === undefined || isNaN(v) ? '--' : Number(v).toFixed(d));
  const pad = (n) => String(n).padStart(2, '0');
  const SENSOR_RANGE_CM = 4.0;     // water level sensor traces = 40 mm
  const MAX_STREAM = 120;
  // Same SMS rule as BRIDGE.ino: one SMS per change, worse conditions reported once settled
  const SMS_SETTLE_MS = 3 * 60000, SMS_MAX_WAIT_MS = 10 * 60000, SMS_DOWNGRADE_HOLD_MS = 5 * 60000;

  const LED_INFO = [
    { led: 'Green',  gpio: 25, road: 'Passable',        meaning: 'All vehicles may pass. Water levels and rainfall are within the safe range.' },
    { led: 'Blue',   gpio: 14, road: 'Restricted',      meaning: 'Two-wheel vehicles (motorcycles, bicycles) are not advised to pass.' },
    { led: 'Yellow', gpio: 26, road: 'More Restricted', meaning: 'Three- and four-wheel vehicles are not advised to pass. Avoid unnecessary travel.' },
    { led: 'Red',    gpio: 27, road: 'Impassable',      meaning: 'No vehicles allowed. Do not attempt to cross the road.' },
  ];

  // ------------------------------------------------------------ helpers
  function rainClass(rate) {
    if (!rate || rate <= 0) return 'No Rain';
    if (rate < 2.5) return 'Light Rain / Drizzle';
    if (rate < 10) return 'Moderate Rain';
    if (rate < 50) return 'Heavy Rain';
    return 'Torrential Rain';
  }
  // Marshall–Palmer drop-size estimates from rain rate R (mm/h)
  function dropMedian(rate) { return rate > 0 ? 0.89 * Math.pow(rate, 0.21) : 0; }
  function dropSpectrum(rate) {
    const bins = [[0, 1], [1, 2], [2, 3], [3, 4.5], [4.5, 8]];
    if (!rate || rate <= 0) return bins.map(() => 0);
    const lam = 4.1 * Math.pow(rate, -0.21);
    const mass = bins.map(([a, b]) => { let s = 0; for (let d = a; d < b; d += 0.02) s += Math.pow(d + 0.01, 3) * Math.exp(-lam * (d + 0.01)); return s; });
    const tot = mass.reduce((x, y) => x + y, 0);
    return mass.map((m) => (100 * m) / tot);
  }
  function dateStr(d) { return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`; }
  function timeStr(d, sec) { return `${pad(d.getHours())}:${pad(d.getMinutes())}${sec ? ':' + pad(d.getSeconds()) : ''}`; }

  // ------------------------------------------------------------ navigation
  function renderNav(active) {
    const links = [['index.html', 'live', 'Live Monitor'], ['simulation.html', 'sim', 'Simulation'], ['trial.html', 'trial', 'Trial Results']];
    const nav = document.createElement('header');
    nav.className = 'topbar';
    nav.innerHTML = `
      <a class="brand" href="index.html">
        <span class="logo" aria-hidden="true"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.7l5.7 5.6a8 8 0 1 1-11.4 0z"/></svg></span>
        <span><strong>FLOOD-SENTINEL</strong><small>Pook, Brgy. Simlong</small></span>
      </a>
      <nav class="nav">${links.map(([h, k, t]) => `<a href="${h}" class="${k === active ? 'active' : ''}">${t}</a>`).join('')}</nav>`;
    document.body.prepend(nav);
  }

  // ------------------------------------------------------------ monitor markup
  function monitorHtml(opts) {
    const tank = (id, name) => `
      <div class="tank-wrap">
        <div class="tank"><div class="water" id="${id}-fill"><span class="wave"></span></div>
          <div class="tick t75"></div><div class="tick t50"></div><div class="tick t25"></div></div>
        <div class="tank-read"><strong id="${id}-val">--</strong><span>cm</span></div>
        <div class="tank-name">${name}</div>
      </div>`;
    return `
    <section class="row r-top">
      <article class="card road">
        <div class="card-h"><h3>Road Condition &amp; 4-LED Indicator Panel</h3><span class="pill" id="road-pill">--</span></div>
        <div class="road-main">
          <div class="road-text"><div class="road-name" id="road-name">--</div><p id="road-desc" class="muted">Waiting for data</p></div>
          <div class="led-panel">
            ${LED_INFO.map((l) => `<div class="led led-${l.led.toLowerCase()}" id="led-${l.led.toLowerCase()}"><span class="lamp"></span><b>${l.led.toUpperCase()}</b></div>`).join('')}
          </div>
        </div>
        <div class="matrix">
          <h4>Smart Indicator Matrix Logic</h4>
          <table><thead><tr><th>LED</th><th>Road condition</th><th>What it means</th></tr></thead>
          <tbody>${LED_INFO.map((l, i) => `<tr id="mx-${i}"><td><span class="dot d-${l.led.toLowerCase()}"></span>${l.led}</td><td>${l.road}</td><td>${l.meaning}</td></tr>`).join('')}</tbody></table>
        </div>
      </article>
      <article class="card status-card">
        <div class="card-h"><h3>${opts.mode === 'sim' ? 'Simulation Status' : 'System Status'}</h3></div>
        <dl class="kv" id="info"></dl>
      </article>
    </section>

    <section class="row r-mid">
      <article class="card">
        <div class="card-h"><h3>Water Level Monitor</h3></div>
        <div class="tanks">
          ${tank('pook', 'Pook-side river')}
          ${tank('bay', 'Bay-connected river')}
          ${tank('l4', 'Sensor 4')}
        </div>
      </article>
      <article class="card rain-card">
        <div class="card-h"><h3>Rainfall &amp; Disdrometer</h3></div>
        <div class="rain-grid">
          <div class="rain-vis"><canvas id="rain-canvas" width="140" height="150"></canvas><div class="collector"><div class="collector-fill" id="collector-fill"></div></div></div>
          <div class="rain-stats">
            <div><span class="lbl">Rainfall collected</span><span class="big"><b id="rain-mm">--</b> mm</span></div>
            <div><span class="lbl">Rain rate</span><span class="big"><b id="rain-rate">--</b> mm/h</span></div>
            <div><span class="lbl">Intensity</span><span class="mid" id="rain-class">--</span></div>
            <div><span class="lbl">Average drop size</span><span class="mid"><b id="drop-d">--</b> mm</span></div>
          </div>
        </div>
        <div class="spectrum">
          <div class="spec-h"><span>Drop-size distribution</span></div>
          <div class="bars" id="spectrum">${['&lt;1 mm', '1–2', '2–3', '3–4.5', '&gt;4.5'].map((l, i) => `<div class="bar"><div class="track"><div class="fill" id="sp-${i}"></div></div><span class="pct" id="sp-${i}-v">0%</span><span class="bl">${l}</span></div>`).join('')}</div>
        </div>
      </article>
    </section>

    <section class="card stream">
      <div class="card-h"><h3>Telemetry Stream</h3><span class="muted small" id="stream-info"></span></div>
      <div class="chart-box"><canvas id="stream-chart"></canvas></div>
    </section>

    <section class="row r-bot">
      <article class="card">
        <div class="card-h"><h3>Event Log</h3><button class="btn ghost sm" id="log-clear">Clear</button></div>
        <div class="log" id="log"></div>
      </article>
      <article class="card recorder">
        <div class="card-h"><h3>Data Recorder</h3><span class="pill rec" id="rec-pill">REC</span></div>
        <div class="rec-count"><b id="rec-count">0</b> readings recorded</div>
        <div class="rec-last muted small" id="rec-last">--</div>
        <div class="rec-actions">
          <button class="btn" id="rec-download">Download Excel</button>
          <button class="btn ghost" id="rec-clear">Clear data</button>
        </div>
      </article>
    </section>`;
  }

  // ------------------------------------------------------------ monitor controller
  const M = {
    mode: 'live', rows: [], events: [], lastCls: null, chart: null, rainRate: 0, rainAnim: null,
    sms: { notified: 0, up: null, upSince: 0, upFirst: 0, down: null, downSince: 0 },

    mount(el, opts) {
      this.mode = opts.mode;
      this.filePrefix = opts.filePrefix || 'FLOOD-SENTINEL';
      el.innerHTML = monitorHtml(opts);
      $('log-clear').onclick = () => { $('log').innerHTML = ''; };
      $('rec-download').onclick = () => this.exportExcel();
      $('rec-clear').onclick = () => {
        if (!this.rows.length || confirm('Clear all recorded readings on this page?')) {
          this.rows = []; this.events = []; this.lastCls = null; this.resetSms(this.sms.notified); this.updateRecorder();
          if (this.chart) { this.chart.data.labels = []; this.chart.data.datasets.forEach((d) => (d.data = [])); this.chart.update(); }
          this.log('Recorded data cleared.', 'info');
        }
      };
      this.initChart();
      this.startRain();
      this.updateRecorder();
    },

    setInfo(items) {
      $('info').innerHTML = Object.entries(items).map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
    },

    log(msg, kind = '', ts = new Date()) {
      const div = document.createElement('div');
      div.className = 'entry ' + kind;
      div.innerHTML = `<span class="t">${timeStr(ts, true)}</span>${msg}`;
      const lg = $('log');
      lg.prepend(div);
      while (lg.children.length > 200) lg.lastChild.remove();
    },

    // reading: { ts: Date, rain, rate, pook, bay, l4, prob? }
    update(r, opts = { record: true }) {
      const prob = r.prob !== undefined && r.prob !== null ? r.prob : fsProbability(r.rain, r.pook, r.bay);
      const ci = fsClassIndex(prob);
      const li = LED_INFO[ci];
      const color = FS_CLASSES[ci].color;

      $('road-name').textContent = li.road.toUpperCase();
      $('road-name').style.color = color;
      $('road-desc').textContent = li.meaning;
      $('road-pill').textContent = li.led.toUpperCase() + ' LED';
      $('road-pill').style.cssText = `background:${color}22;color:${color};border-color:${color}66`;
      LED_INFO.forEach((l, i) => {
        $('led-' + l.led.toLowerCase()).classList.toggle('on', i === ci);
        $('mx-' + i).classList.toggle('on', i === ci);
      });

      [['pook', r.pook], ['bay', r.bay], ['l4', r.l4]].forEach(([id, v]) => {
        $(id + '-val').textContent = fmt(v, 2);
        $(id + '-fill').style.height = Math.min(100, Math.max(0, ((v || 0) / SENSOR_RANGE_CM) * 100)) + '%';
      });

      $('rain-mm').textContent = fmt(r.rain);
      $('rain-rate').textContent = fmt(r.rate);
      $('rain-class').textContent = rainClass(r.rate);
      $('drop-d').textContent = r.rate > 0 ? fmt(dropMedian(r.rate), 2) : '--';
      $('collector-fill').style.height = Math.min(100, ((r.rain || 0) / 40) * 100) + '%';
      dropSpectrum(r.rate).forEach((p, i) => { $('sp-' + i).style.height = p + '%'; $('sp-' + i + '-v').textContent = Math.round(p) + '%'; });
      this.rainRate = r.rate || 0;

      if (!opts.record) return;
      const ts = r.ts || new Date();
      const row = { ts, rain: r.rain, rate: r.rate, pook: r.pook, bay: r.bay, l4: r.l4, ci, prob,
                    rssi: r.rssi, gsm: r.gsm, sms: r.sms };
      this.rows.push(row);
      if (this.lastCls !== null && ci !== this.lastCls) {
        this.log(`Road condition: <b style="color:${color}">${li.road.toUpperCase()}</b> (${li.led} LED)`, 'warn', ts);
      }
      this.lastCls = ci;
      this.checkSms(ci, ts);
      this.log(`Rain ${fmt(r.rain)} mm (${fmt(r.rate)} mm/h) · Pook ${fmt(r.pook, 2)} cm · Bay ${fmt(r.bay, 2)} cm · S4 ${fmt(r.l4, 2)} cm → ${li.road}`, '', ts);
      this.pushChart(ts, r);
      this.updateRecorder();
    },

    resetSms(level = 0) { this.sms = { notified: level, up: null, upSince: 0, upFirst: 0, down: null, downSince: 0 }; },

    // One SMS per change (same rule as the ESP32): a worse condition is sent once it has stayed
    // the same for 3 min (IMPASSABLE at once, never later than 10 min); a better one after 5 min.
    checkSms(ci, ts) {
      const s = this.sms, t = ts.getTime();
      let send = null;
      if (ci > s.notified) {
        if (s.up === null) s.upFirst = t;
        if (s.up !== ci) { s.up = ci; s.upSince = t; }
        if (ci === 3 || t - s.upSince >= SMS_SETTLE_MS || t - s.upFirst >= SMS_MAX_WAIT_MS) send = ci;
        s.down = null;
      } else if (ci < s.notified) {
        s.up = null;
        if (s.down !== ci) { s.down = ci; s.downSince = t; }
        else if (t - s.downSince >= SMS_DOWNGRADE_HOLD_MS) send = ci;
      } else { s.up = null; s.down = null; }
      if (send !== null) {
        s.notified = send; s.up = null; s.down = null;
        this.events.push({ ts, ci: send });
        this.log(`SMS sent to 10 numbers: "${FS_CLASSES[send].sms}"`, 'info', ts);
        if (this.onSms) this.onSms();
      }
    },

    updateRecorder() {
      $('rec-count').textContent = this.rows.length;
      const last = this.rows[this.rows.length - 1];
      $('rec-last').textContent = last ? `Last: ${dateStr(last.ts)} ${timeStr(last.ts, true)} · ${LED_INFO[last.ci].road}` : 'No readings yet';
      $('stream-info').textContent = this.rows.length ? `${this.rows.length} reading(s)` : '';
      $('rec-download').disabled = !this.rows.length;
    },

    initChart() {
      if (typeof Chart === 'undefined') return;
      Chart.defaults.color = '#8d9ab5';
      Chart.defaults.borderColor = '#22304d';
      Chart.defaults.font.family = 'Inter, sans-serif';
      Chart.defaults.font.size = 11;
      this.chart = new Chart($('stream-chart'), {
        type: 'line',
        data: { labels: [], datasets: [
          { label: 'Pook-side (cm)', data: [], borderColor: '#38bdf8', yAxisID: 'y', tension: 0.25, pointRadius: 0, borderWidth: 2 },
          { label: 'Bay-connected (cm)', data: [], borderColor: '#a78bfa', yAxisID: 'y', tension: 0.25, pointRadius: 0, borderWidth: 2 },
          { label: 'Sensor 4 (cm)', data: [], borderColor: '#64748b', yAxisID: 'y', tension: 0.25, pointRadius: 0, borderWidth: 1.5, borderDash: [4, 3] },
          { label: 'Rainfall (mm)', data: [], borderColor: '#22c55e', yAxisID: 'y1', tension: 0.25, pointRadius: 0, borderWidth: 2 },
          { label: 'Rain rate (mm/h)', data: [], borderColor: '#eab308', yAxisID: 'y1', tension: 0.25, pointRadius: 0, borderWidth: 1.5 },
        ] },
        options: { responsive: true, maintainAspectRatio: false, animation: false, interaction: { mode: 'index', intersect: false },
          scales: {
            x: { ticks: { maxRotation: 0, autoSkip: true, maxTicksLimit: 8 } },
            y: { position: 'left', min: 0, suggestedMax: 4, title: { display: true, text: 'Water level (cm)' } },
            y1: { position: 'right', min: 0, suggestedMax: 20, grid: { drawOnChartArea: false }, title: { display: true, text: 'Rain (mm, mm/h)' } },
          },
          plugins: { legend: { labels: { boxWidth: 10, boxHeight: 2 } } } },
      });
    },

    pushChart(ts, r) {
      if (!this.chart) return;
      const c = this.chart;
      c.data.labels.push(timeStr(ts, this.mode === 'live'));
      [r.pook, r.bay, r.l4, r.rain, r.rate].forEach((v, i) => c.data.datasets[i].data.push(v === undefined || v === null ? null : +(+v).toFixed(2)));
      if (c.data.labels.length > MAX_STREAM) { c.data.labels.shift(); c.data.datasets.forEach((d) => d.data.shift()); }
      c.update();
    },

    startRain() {
      const cv = $('rain-canvas');
      const ctx = cv.getContext('2d');
      const drops = [];
      const step = () => {
        const want = Math.min(80, Math.round(this.rainRate * 2.5));
        while (drops.length < want) drops.push({ x: Math.random() * cv.width, y: Math.random() * -cv.height, v: 2 + Math.random() * 3 });
        while (drops.length > want) drops.pop();
        ctx.clearRect(0, 0, cv.width, cv.height);
        ctx.strokeStyle = 'rgba(125,200,255,0.75)';
        ctx.lineWidth = 1.2;
        ctx.beginPath();
        for (const d of drops) {
          d.y += d.v + this.rainRate * 0.05;
          if (d.y > cv.height) { d.y = -8; d.x = Math.random() * cv.width; }
          ctx.moveTo(d.x, d.y); ctx.lineTo(d.x - 1, d.y + 7);
        }
        ctx.stroke();
        this.rainAnim = requestAnimationFrame(step);
      };
      step();
    },

    // ---------------------------------------------------------- Excel export (paper format)
    exportExcel() {
      if (!this.rows.length) return;
      const kind = this.mode === 'sim' ? 'Simulation' : 'Live';
      const sec = this.mode === 'live';
      const first = this.rows[0].ts;
      const sheets = [
        {
          name: 'Monitoring Data',
          title: `FLOOD-SENTINEL ${kind} Monitoring Data`,
          subtitle: `Recorded ${dateStr(first)} ${timeStr(first, sec)} – ${timeStr(this.rows[this.rows.length - 1].ts, sec)}. Same columns as the paper's trial table (Figure 2). Fill in Observed Flood (0/1) from field observation.`,
          columns: [
            { header: 'Date', width: 12, type: 'text' }, { header: 'Time', width: 10, type: 'text' },
            { header: 'Rainfall (mm)', width: 13, type: 'num1' }, { header: 'Pook-side water level (cm)', width: 17, type: 'num2' },
            { header: 'Bay-connected river water level (cm)', width: 20, type: 'num2' }, { header: 'Observed Flood (0/1)', width: 13, type: 'int' },
            { header: 'Road Condition', width: 18, type: 'text' }, { header: 'LED Indicator', width: 13, type: 'led' },
            { header: 'Flood Probability (%)', width: 14, type: 'num1' },
          ],
          rows: this.rows.map((r) => [dateStr(r.ts), timeStr(r.ts, sec), r.rain, r.pook, r.bay, '', LED_INFO[r.ci].road, LED_INFO[r.ci].led, +r.prob.toFixed(1)]),
        },
        {
          name: 'Sensor Details',
          title: 'Additional readings',
          columns: [
            { header: 'Date', width: 12, type: 'text' }, { header: 'Time', width: 10, type: 'text' },
            { header: 'Rain rate (mm/h)', width: 13, type: 'num1' }, { header: 'Rain intensity', width: 20, type: 'text' },
            { header: 'Median drop size, est. (mm)', width: 16, type: 'num2' }, { header: 'Water level sensor 4 (cm)', width: 16, type: 'num2' },
            { header: 'WiFi RSSI (dBm)', width: 12, type: 'int' }, { header: 'GSM', width: 10, type: 'text' }, { header: 'SMS sent (device total)', width: 14, type: 'int' },
          ],
          rows: this.rows.map((r) => [dateStr(r.ts), timeStr(r.ts, sec), r.rate, rainClass(r.rate), r.rate > 0 ? +dropMedian(r.rate).toFixed(2) : '',
            r.l4, r.rssi ?? '', r.gsm === undefined ? '' : r.gsm ? 'Ready' : 'Not ready', r.sms ?? '']),
        },
        {
          name: 'SMS Events',
          title: 'Road-condition changes (one SMS to each of the 10 registered numbers)',
          columns: [
            { header: 'Date', width: 12, type: 'text' }, { header: 'Time', width: 10, type: 'text' },
            { header: 'Road Condition', width: 18, type: 'text' }, { header: 'LED Indicator', width: 13, type: 'led' },
            { header: 'SMS message', width: 90, type: 'text' }, { header: 'Recipients', width: 11, type: 'int' },
          ],
          rows: this.events.map((e) => [dateStr(e.ts), timeStr(e.ts, sec), LED_INFO[e.ci].road, LED_INFO[e.ci].led, FS_SMS_TEXTS[e.ci].replace('\n', ' '), 10]),
        },
      ];
      const stamp = `${dateStr(new Date())}_${timeStr(new Date()).replace(':', '')}`;
      FSXlsx.download(`${this.filePrefix}_${kind}_${stamp}.xlsx`, sheets);
      this.log(`Excel file downloaded (${this.rows.length} readings).`, 'info');
    },
  };

  window.FS = { renderNav, monitor: M, rainClass, dropMedian, dropSpectrum, dateStr, timeStr, fmt, LED_INFO };
})();
