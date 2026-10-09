/* FLOOD-SENTINEL – Simulation page: weather presets drive the same model the ESP32 uses */
(function () {
  const $ = (id) => document.getElementById(id);
  const M = FS.monitor;
  const TICK_MS = 1000;            // one simulation step per second
  const RIVER_TAU_MIN = 40;        // river levels approach the preset target with a 40-minute time constant

  const PRESETS = {
    clear:    { name: 'Clear & Dry',      rate: 0,   pook: 0.2, bay: 0.1, l4: 0.1 },
    drizzle:  { name: 'Gentle Drizzle',   rate: 1.5, pook: 0.6, bay: 0.4, l4: 0.3 },
    moderate: { name: 'Moderate Rain',    rate: 6,   pook: 1.0, bay: 0.7, l4: 0.6 },
    downpour: { name: 'Heavy Downpour',   rate: 20,  pook: 1.6, bay: 1.2, l4: 1.0 },
    flood:    { name: 'Flood Emergency',  rate: 40,  pook: 3.0, bay: 2.6, l4: 2.2 },
  };

  const s = { rain: 0, rate: 0, pook: 0, bay: 0, l4: 0, target: { pook: 0, bay: 0, l4: 0 }, preset: 'Manual',
              clock: new Date(), running: false, timer: null, steps: 0 };

  function resetClock() { const d = new Date(); d.setSeconds(0, 0); s.clock = d; }

  function syncSliders() {
    const set = (id, v, d, unit) => { $('s-' + id).value = v; $('s-' + id + '-v').textContent = `${Number(v).toFixed(d)} ${unit}`; };
    set('rate', s.rate, 1, 'mm/h'); set('rain', s.rain, 1, 'mm');
    set('pook', s.pook, 2, 'cm'); set('bay', s.bay, 2, 'cm'); set('l4', s.l4, 2, 'cm');
  }

  function info() {
    M.setInfo({
      'Mode': s.running ? '<span class="ok">Running</span>' : '<span class="warn-t">Paused</span>',
      'Weather preset': s.preset,
      'Simulated time': `${FS.dateStr(s.clock)} ${FS.timeStr(s.clock)}`,
      'Step size': `${$('b-speed').selectedOptions[0].text}`,
      'Steps recorded': s.steps,
      'SMS events': M.events.length + ' × 10 numbers',
    });
    $('sim-pill').textContent = s.running ? 'RUNNING' : 'PAUSED';
    $('sim-pill').style.cssText = s.running ? 'color:#22c55e;border-color:#22c55e66;background:#22c55e1a' : '';
    $('b-run').textContent = s.running ? '❚❚ Pause' : '▶ Start';
  }

  function reading() {
    const r2 = (v) => Math.round(v * 100) / 100;
    return { ts: new Date(s.clock), rain: Math.round(s.rain * 10) / 10, rate: s.rate, pook: r2(s.pook), bay: r2(s.bay), l4: r2(s.l4) };
  }
  function show() { syncSliders(); M.update(reading(), { record: false }); info(); }

  function step() {
    const dtMin = +$('b-speed').value;
    s.clock = new Date(s.clock.getTime() + dtMin * 60000);
    const wasFull = s.rain >= 40;
    s.rain = Math.min(40, s.rain + (s.rate * dtMin) / 60);
    const k = 1 - Math.exp(-dtMin / RIVER_TAU_MIN);
    ['pook', 'bay', 'l4'].forEach((x) => { s[x] = +(s[x] + (s.target[x] - s[x]) * k).toFixed(3); });
    s.steps++;
    syncSliders();
    M.update(reading(), { record: true });
    if (s.rain >= 40 && !wasFull) M.log('Rain container is full (40 mm) – empty it to keep measuring.', 'warn', s.clock);
    info();
  }

  function setRunning(on) {
    s.running = on;
    clearInterval(s.timer);
    if (on) { step(); s.timer = setInterval(step, TICK_MS); M.log(`Simulation started (${s.preset}).`, 'info', s.clock); }
    info();
  }

  function applyPreset(key) {
    const p = PRESETS[key];
    s.preset = p.name; s.rate = p.rate;
    s.target = { pook: p.pook, bay: p.bay, l4: p.l4 };
    document.querySelectorAll('.preset').forEach((b) => b.classList.toggle('active', b.dataset.p === key));
    M.log(`Preset "${p.name}": rain ${p.rate} mm/h, river levels heading to Pook ${p.pook} cm / Bay ${p.bay} cm.`, 'info', s.clock);
    show();
    if (!s.running) setRunning(true);
  }

  function bind() {
    document.querySelectorAll('.preset').forEach((b) => b.addEventListener('click', () => applyPreset(b.dataset.p)));
    ['rate', 'rain', 'pook', 'bay', 'l4'].forEach((k) => $('s-' + k).addEventListener('input', (e) => {
      s[k] = +e.target.value;
      if (k in s.target) s.target[k] = s[k];
      s.preset = 'Manual';
      document.querySelectorAll('.preset').forEach((b) => b.classList.remove('active'));
      show();
    }));
    $('b-run').onclick = () => setRunning(!s.running);
    $('b-step').onclick = () => step();
    $('b-empty').onclick = () => { s.rain = 0; M.log('Rain container emptied.', 'info', s.clock); show(); };
    $('b-speed').onchange = info;
    $('b-reset').onclick = () => {
      setRunning(false);
      Object.assign(s, { rain: 0, rate: 0, pook: 0, bay: 0, l4: 0, target: { pook: 0, bay: 0, l4: 0 }, preset: 'Manual', steps: 0 });
      resetClock();
      document.querySelectorAll('.preset').forEach((b) => b.classList.remove('active'));
      show();
      M.log('Simulation reset.', 'info');
    };
  }

  document.addEventListener('DOMContentLoaded', () => {
    FS.renderNav('sim');
    M.mount($('monitor'), { mode: 'sim' });
    resetClock();
    bind();
    show();
    M.log('Simulation ready. Pick a weather preset or move the sliders, then press Start.', 'info');
  });
})();
