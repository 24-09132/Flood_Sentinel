/**
 * FLOOD-SENTINEL local server (for testing on a laptop without Vercel)
 * Built with native Node.js modules - NO npm install required!
 *
 *  - Serves the same dashboard as Vercel (index.html, app.js, model.js, style.css in the repo root)
 *  - POST /api/telemetry  (ESP32 pushes readings, needs the API key)
 *  - GET  /api/telemetry  (latest reading)
 *  - GET  /api/history    (recent readings for the chart)
 * Run: npm start   then open http://localhost:3000
 * ESP32: set VERCEL_HOST to this computer's IP is NOT enough (ESP32 uses HTTPS) – use this for browser testing.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3000;
const PUBLIC_DIR = path.join(__dirname, '..');

let latestTelemetry = {};
const history = [];

const MIME_TYPES = {
  '.html': 'text/html',
  '.css': 'text/css',
  '.js': 'application/javascript',
  '.json': 'application/json',
  '.png': 'image/png',
  '.svg': 'image/svg+xml'
};

const server = http.createServer((req, res) => {
  // CORS Headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, x-api-key');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  const url = new URL(req.url, `http://${req.headers.host}`);

  // API Endpoints
  if (url.pathname === '/api/history') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(history.slice().reverse()));
    return;
  }

  if (url.pathname === '/api/telemetry') {
    if (req.method === 'GET') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(latestTelemetry));
      return;
    } else if (req.method === 'POST') {
      const apiKey = req.headers['x-api-key'] || req.headers['bridge_key'] || req.headers['bridge-key'] || url.searchParams.get('key') || url.searchParams.get('bridge_key');
      const expectedKey = process.env.API_KEY || process.env.bridge_key || 'bridgingthegap';

      if (apiKey !== expectedKey && apiKey !== 'bridgingthegap') {
        res.writeHead(401, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Unauthorized: Invalid API Key' }));
        return;
      }

      let body = '';
      req.on('data', chunk => { body += chunk; });
      req.on('end', () => {
        try {
          const parsed = JSON.parse(body);
          latestTelemetry = { ...latestTelemetry, ...parsed, timestamp: Date.now() };
          history.unshift({ t: latestTelemetry.timestamp, p: latestTelemetry.flood_prob, c: latestTelemetry.road_class,
                            r: latestTelemetry.rainfall_mm, k: latestTelemetry.pook_cm, b: latestTelemetry.bay_cm });
          if (history.length > 240) history.length = 240;
          console.log(`[INGEST] Rain=${latestTelemetry.rainfall_mm} mm, Pook=${latestTelemetry.pook_cm} cm, Bay=${latestTelemetry.bay_cm} cm, P=${latestTelemetry.flood_prob}% -> ${latestTelemetry.road_condition}`);
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ status: 'ok', received: true, timestamp: Date.now() }));
        } catch (err) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Invalid JSON payload' }));
        }
      });
      return;
    }
  }

  // Static File Serving
  let filePath = path.join(PUBLIC_DIR, url.pathname === '/' ? 'index.html' : url.pathname);
  if (!filePath.startsWith(PUBLIC_DIR)) { res.writeHead(403); res.end(); return; }
  const ext = path.extname(filePath).toLowerCase();
  const contentType = MIME_TYPES[ext] || 'application/octet-stream';

  fs.readFile(filePath, (err, content) => {
    if (err) {
      if (err.code === 'ENOENT') {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('404 Not Found');
      } else {
        res.writeHead(500, { 'Content-Type': 'text/plain' });
        res.end('500 Server Error');
      }
    } else {
      res.writeHead(200, { 'Content-Type': contentType });
      res.end(content);
    }
  });
});

server.listen(PORT, () => {
  console.log(`=======================================================`);
  console.log(` FLOOD-SENTINEL local server running                   `);
  console.log(` Web Dashboard: http://localhost:${PORT}                 `);
  console.log(` Ingest Endpoint: http://localhost:${PORT}/api/telemetry`);
  console.log(`=======================================================`);
});
