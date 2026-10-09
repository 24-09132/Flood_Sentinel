# FLOOD-SENTINEL

Rainfall and River Water Level Based Road Impassability Probability Model with Automated SMS Notification – Pook, Barangay Simlong.

| Part | File(s) |
|---|---|
| ESP32 firmware (sensors, model, LEDs, SIM800L SMS, cloud upload) | `BRIDGE.ino` |
| Website (live view + simulation) | `index.html`, `app.js`, `model.js`, `style.css` |
| Cloud API (Vercel serverless + Upstash Redis) | `api/telemetry.js`, `api/history.js`, `api/_db.js` |
| Local test server (no Vercel needed) | `web_dashboard/server.js` (`npm start`) |

## Flood probability model

```
z = −3.9822 + 0.2208·Rainfall(mm) + 0.7994·Pook-side level(cm) + 1.2461·Bay-connected level(cm)
P = 100 / (1 + e^(−z))   (%)
```

| Flood probability | Road condition | LED | SMS |
|---|---|---|---|
| below 10 % | Passable | Green | ROAD PASSABLE |
| 10 % to below 50 % | Restricted | Blue | ROAD RESTRICTED |
| 50 % to below 75 % | More Restricted | Yellow | ROAD HIGHLY RESTRICTED |
| 75 % and above | Impassable | Red | ROAD IMPASSABLE |

The same coefficients and thresholds are in `BRIDGE.ino`, `model.js` and the Excel workbook (`Model` sheet).
They reproduce every flood probability in the paper's Figure 2. If you change one, change all three.

## Deploy the website to Vercel

1. **GitHub** – put this folder in a GitHub repository on your account.
2. **Vercel** – sign in at vercel.com with GitHub → **Add New → Project** → import the repository → **Deploy**.
   No build settings are needed.
3. **Database** – in the project: **Storage → Create Database → Upstash (Redis)** → connect it to the project.
   This adds `KV_REST_API_URL` and `KV_REST_API_TOKEN` automatically.
4. **API key** – **Settings → Environment Variables** → add `bridge_key` = `bridgingthegap`
   (or your own key; then use the same key in `BRIDGE.ino`, `VERCEL_API_KEY`).
5. **Redeploy** – **Deployments → ⋯ → Redeploy** so the database and key are picked up.
6. Your site is at `https://<project-name>.vercel.app`.

## Connect the ESP32

In `BRIDGE.ino` set:

```cpp
const char* WIFI_SSID     = "your Wi-Fi name";
const char* WIFI_PASSWORD = "your Wi-Fi password";
#define VERCEL_HOST       "<project-name>.vercel.app"   // no https://, no slash
#define VERCEL_API_KEY    "bridgingthegap"
```

Upload. The Serial Monitor shows `[VERCEL] Telemetry pushed ... HTTP 200` every 30 s and the website's
**Live System** tab shows "System online". If no data arrives for 75 s it shows "System offline".

The ESP32 uploads every 30 s, and immediately when the road condition changes. This keeps usage inside
the free Vercel Hobby (1,000,000 function invocations/month) and Upstash (500,000 commands/month) limits.

## Test without hardware

- Website: open the **Simulation** tab (or add `?mode=sim` to the URL). **Play trial** replays Figure 2.
- Local server: `npm start`, open `http://localhost:3000`, then post a reading:

```bash
curl -X POST http://localhost:3000/api/telemetry -H "x-api-key: bridgingthegap" \
  -d '{"rainfall_mm":11.6,"pook_cm":1.8,"bay_cm":1.3,"flood_prob":83.7,"road_class":3,"road_condition":"IMPASSABLE"}'
```

## Serial Monitor commands (115200 baud, Newline)

`MON` status on/off · `SMSTEST` test SMS · `SMSRESET` reset saved road state ·
`CAL S1..S4`, `CAL <mm>`, `CAL LIST`, `CAL UNDO`, `CAL CLEAR`, `CAL DEFAULT`, `CAL DONE` (calibration; SMS paused while calibrating)

FLOOD-SENTINEL is a research and decision-support prototype. It does not replace official flood warnings,
road closures, evacuation orders or instructions from local authorities.
