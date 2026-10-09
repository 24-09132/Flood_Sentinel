/* FLOOD-SENTINEL – Live Monitor page: reads the ESP32 data stored by the Vercel API */
(function () {
  const ONLINE_MAX_AGE_MS = 75000;   // ESP32 posts every 30 s
  const POLL_MS = 10000;
  const M = FS.monitor;
  let lastTs = 0;
  let latest = null;

  function ago(ms) {
    const s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return s + ' s ago';
    if (s < 3600) return Math.round(s / 60) + ' min ago';
    if (s < 86400) return Math.round(s / 3600) + ' h ago';
    return Math.round(s / 86400) + ' day(s) ago';
  }
  function uptime(sec) {
    if (sec === undefined || sec === null) return '--';
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
    return h ? `${h} h ${m} min` : `${m} min`;
  }
  function signal(rssi) {
    if (rssi === undefined || rssi === null) return '--';
    const q = rssi >= -60 ? 'Strong' : rssi >= -70 ? 'Good' : rssi >= -80 ? 'Fair' : 'Weak';
    return `${rssi} dBm (${q})`;
  }

  function setConn(kind, text) {
    const p = document.getElementById('conn-pill');
    p.textContent = text;
    const c = kind === 'on' ? '#22c55e' : kind === 'off' ? '#ef4444' : '#8d9ab5';
    p.style.cssText = `color:${c};border-color:${c}66;background:${c}1a`;
  }

  function showInfo() {
    if (!latest) {
      M.setInfo({ 'Connection': '<span class="bad">No data yet</span>', 'Data source': 'Vercel cloud API', 'Upload interval': '30 s' });
      return;
    }
    const age = Date.now() - latest.timestamp;
    const online = age <= ONLINE_MAX_AGE_MS;
    M.setInfo({
      'Connection': online ? '<span class="ok">Online</span>' : '<span class="bad">Offline</span>',
      'Last update': `${FS.timeStr(new Date(latest.timestamp), true)} (${ago(age)})`,
      'WiFi signal': signal(latest.rssi),
      'GSM / SMS module': latest.gsm_ready ? '<span class="ok">Ready</span>' : '<span class="warn-t">Connecting</span>',
      'SMS sent (device)': latest.sms_sent ?? 0,
      'Device uptime': uptime(latest.uptime),
      'Recipients': '10 numbers',
    });
    setConn(online ? 'on' : 'off', online ? 'SYSTEM ONLINE' : 'SYSTEM OFFLINE');
  }

  function toReading(d) {
    return {
      ts: new Date(d.timestamp),
      rain: d.rainfall_mm, rate: d.rain_rate,
      pook: d.pook_cm ?? (d.pook_mm !== undefined ? d.pook_mm / 10 : null),
      bay: d.bay_cm ?? (d.bay_mm !== undefined ? d.bay_mm / 10 : null),
      l4: d.level4_cm ?? (d.level4_mm !== undefined ? d.level4_mm / 10 : null),
      prob: d.flood_prob, rssi: d.rssi, gsm: d.gsm_ready, sms: d.sms_sent,
    };
  }

  async function loadHistory() {
    try {
      const res = await fetch('/api/history', { cache: 'no-store' });
      if (!res.ok) return;
      const h = await res.json();
      if (!Array.isArray(h) || !h.length) return;
      h.filter((p) => p.t && p.r !== undefined).forEach((p) => {
        M.update({ ts: new Date(p.t), rain: p.r, rate: p.q ?? 0, pook: p.k, bay: p.b, l4: p.l ?? 0, prob: p.p }, { record: true });
        lastTs = Math.max(lastTs, p.t);
      });
      M.log(`Loaded ${h.length} earlier reading(s) from the cloud.`, 'info');
    } catch (e) { /* optional */ }
  }

  async function poll() {
    try {
      const res = await fetch('/api/telemetry', { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const d = await res.json();
      if (d.rainfall_mm === undefined || !d.timestamp) {
        setConn('off', 'NO DATA YET');
        showInfo();
        return;
      }
      latest = d;
      if (d.timestamp > lastTs) {
        lastTs = d.timestamp;
        M.update(toReading(d), { record: true });
      } else if (!M.rows.length) {
        M.update(toReading(d), { record: false });
      }
      showInfo();
    } catch (e) {
      setConn('off', 'SERVER UNREACHABLE');
      M.log('Cannot reach the cloud API (' + e.message + ').', 'err');
    }
  }

  document.addEventListener('DOMContentLoaded', async () => {
    FS.renderNav('live');
    M.mount(document.getElementById('monitor'), { mode: 'live' });
    M.log('Live monitor started. Waiting for ESP32 data…', 'info');
    showInfo();
    await loadHistory();
    await poll();
    setInterval(poll, POLL_MS);
    setInterval(showInfo, 5000);
  });
})();
