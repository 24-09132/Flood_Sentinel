/*
 * =========================================================================================
 * FLOOD-SENTINEL (Project BRIDGE) – ESP32
 * =========================================================================================
 * GPIO 33  Rainfall collector water level sensor  -> rainfall (mm) and rate (mm/h)
 * GPIO 34  Pook-side river water level sensor     -> level (mm)
 * GPIO 35  Bay-side river water level sensor      -> level (mm)
 * GPIO 32  Water level sensor 4                   -> level (mm)
 * LEDs     Green 25, Yellow 26, Red 27, Blue 14
 * SIM800L  UART2 (RX 16, TX 17) – SMS on road-condition change
 * Wi-Fi web server (/ and /api/telemetry) + Vercel cloud push
 * =========================================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <math.h>

// -------------------------------------------------------------
// Pin Definitions (ADC1 only – Wi-Fi safe)
// -------------------------------------------------------------
const int PIN_RAIN_COLLECTOR = 33;
const int PIN_POOK_LEVEL     = 34;
const int PIN_BAY_LEVEL      = 35;
const int PIN_LEVEL4         = 32;

const int PIN_LED_GREEN   = 25;
const int PIN_LED_YELLOW  = 26;
const int PIN_LED_RED     = 27;
const int PIN_LED_BLUE    = 14;

// -------------------------------------------------------------
// Sensor Calibration (water depth in mm vs ADC reading)
// The defaults are used until you calibrate with the CAL commands in the
// Serial Monitor; your points are then saved in flash and loaded on boot.
// -------------------------------------------------------------
#define CAL_MAX_POINTS 12
#define SENSOR_RAIN    0
#define SENSOR_POOK    1
#define SENSOR_BAY     2
#define SENSOR_S4      3

struct CalTable { int n; float mm[CAL_MAX_POINTS]; int adc[CAL_MAX_POINTS]; };

const char* const SENSOR_NAMES[4] = { "Rainfall collector", "Pook-side river", "Bay-side river", "Water level sensor 4" };
const int SENSOR_PINS[4] = { PIN_RAIN_COLLECTOR, PIN_POOK_LEVEL, PIN_BAY_LEVEL, PIN_LEVEL4 };

const CalTable CAL_DEFAULTS[4] = {
  { 4, { 0.0, 5.0, 10.0, 40.0 }, { 0, 2081, 2161, 2608 } },
  { 3, { 0.0, 10.0, 40.0 },      { 0, 2275, 4095 } },
  { 3, { 0.0, 10.0, 40.0 },      { 0, 3327, 4095 } },
  { 3, { 0.0, 10.0, 40.0 },      { 0, 2646, 3489 } },
};

CalTable cal[4];
float    calOnset[4];
int      calSel = 0;
Preferences calPrefs;
bool     calMode = false;                          // SMS paused while calibrating
unsigned long calLastCmdT = 0;
const unsigned long CAL_MODE_TIMEOUT_MS = 15UL * 60UL * 1000UL;

const float RAIN_AREA_RATIO       = 1.0f;   // container floor area / opening area
const float SENSOR_RANGE_MM       = 40.0f;  // length of the sensor traces
const float RAIN_EMPTY_DROP_MM    = 3.0f;   // drop larger than this = container was emptied
const float RAIN_MIN_RISE_MM      = 0.5f;   // ignore smaller rises (sensor noise)
const int   RAIN_WINDOW_MIN       = 10;     // rain rate measured over the last 10 minutes

// -------------------------------------------------------------
// Flood Probability Model (same values as the Excel "Model" sheet and the website model.js)
//   z = B0 + B_RAIN*rainfall(mm) + B_POOK*Pook level(cm) + B_BAY*Bay level(cm)
//   P = 100 / (1 + e^-z)   (%)
// Road condition from P: Green = Passable, Blue = Restricted,
// Yellow = More (Highly) Restricted, Red = Impassable
// -------------------------------------------------------------
const float MODEL_B0     = -3.9822f;
const float MODEL_B_RAIN =  0.2208f;
const float MODEL_B_POOK =  0.7994f;
const float MODEL_B_BAY  =  1.2461f;

const float PROB_RESTRICTED      = 10.0f;   // %
const float PROB_MORE_RESTRICTED = 50.0f;   // %
const float PROB_IMPASSABLE      = 75.0f;   // %

// -------------------------------------------------------------
// WiFi Credentials & AP Fallback
// -------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* AP_SSID       = "HydroSense-AP";
const char* AP_PASSWORD   = "watermonitor";

// -------------------------------------------------------------
// Vercel Cloud & API Key Integration
// -------------------------------------------------------------
#define VERCEL_HOST       "your-project.vercel.app"   // <-- SET to your Vercel domain (without https://)
#define VERCEL_API_KEY    "bridgingthegap"
#define CLOUD_SYNC_MS     30000UL   // every 30 s (fits the free Vercel + Upstash limits); road changes are sent at once

WebServer server(80);
WiFiClientSecure httpsClient;
unsigned long lastCloudSync = 0;
int lastCloudRoadClass = -2;

// -------------------------------------------------------------
// SIM800L & SMS Configuration
// -------------------------------------------------------------
#define SIM800_RX_PIN   16
#define SIM800_TX_PIN   17
#define SIM800_BAUD     9600

HardwareSerial sim800(2);

const char* SMS_RECIPIENTS[] = {
  "+639569515275", "+639058794293", "+639674271179", "+639925823629", "+639263888362",
  "+639919413761", "+639266212688", "+639636932635", "+639150668815", "+639654024686"
};
const int SMS_RECIPIENT_COUNT = sizeof(SMS_RECIPIENTS) / sizeof(SMS_RECIPIENTS[0]);

#define ROAD_PASSABLE         0
#define ROAD_RESTRICTED       1
#define ROAD_MORE_RESTRICTED  2
#define ROAD_IMPASSABLE       3
#define ROAD_UNKNOWN         -1

const char* const ROAD_CLASS_NAMES[4] = { "PASSABLE", "RESTRICTED", "HIGHLY RESTRICTED", "IMPASSABLE" };

const char* const SMS_TEXTS[4] = {
  // PASSABLE (draft – replace with the client's text when provided)
  "FLOOD-SENTINEL UPDATE: ROAD PASSABLE.\n"
  "Water-level conditions have returned to normal. All vehicles may pass. Please continue to exercise caution and follow official instructions from local authorities.",

  "FLOOD-SENTINEL WARNING: ROAD RESTRICTED.\n"
  "Two-wheel vehicles are NOT ADVISED TO PASS due to monitored water-level conditions. Please exercise caution and follow official instructions from local authorities.",

  "FLOOD-SENTINEL WARNING: ROAD HIGHLY RESTRICTED.\n"
  "Three-wheel and four-wheel vehicles are NOT ADVISED TO PASS due to elevated or hazardous water-level conditions. Please avoid unnecessary travel, exercise extreme caution, and follow official instructions from local authorities.",

  "FLOOD-SENTINEL CRITICAL WARNING: ROAD IMPASSABLE.\n"
  "NO VEHICLES ARE ALLOWED TO PASS due to critical water-level conditions. DO NOT ATTEMPT TO CROSS. Follow official warnings and instructions from local authorities."
};

const int           SMS_CONFIRM_SAMPLES   = 3;
const unsigned long SMS_DOWNGRADE_HOLD_MS = 5UL * 60UL * 1000UL;
const int           SMS_MAX_RETRIES       = 2;

#define SMS_RAIN_UNIT   "mm"
#define SMS_LEVEL_UNIT  " cm"
#define SMS_MAX_CHARS   459
#define SMS_MAX_PARTS   3

const unsigned long GSM_BOOT_DELAY_MS     = 3000;
const unsigned long GSM_CMD_TIMEOUT_MS    = 2000;
const unsigned long GSM_PROMPT_TIMEOUT_MS = 5000;
const unsigned long GSM_SEND_TIMEOUT_MS   = 60000;
const unsigned long GSM_REG_POLL_MS       = 3000;
const unsigned long GSM_REG_GIVEUP_MS     = 90000;
const unsigned long GSM_HEALTH_CHECK_MS   = 60000;
const unsigned long GSM_RETRY_BACKOFF_MS  = 10000;

enum GsmState {
  GSM_BOOT_WAIT, GSM_INIT_STEP, GSM_REG_CHECK, GSM_REG_WAIT,
  GSM_READY, GSM_HEALTH, GSM_SMS_PROMPT, GSM_SMS_BODY, GSM_BACKOFF
};
GsmState gsmState          = GSM_BOOT_WAIT;
unsigned long gsmStateT    = 0;
unsigned long gsmCmdTO     = 0;
const char*   gsmExpect    = NULL;
int           gsmInitIdx   = 0;
unsigned long gsmRegStartT = 0;
bool          gsmRegistered = false;
int           gsmHealthFails = 0;

char   gsmRx[256];
size_t gsmRxLen = 0;

const char* const GSM_INIT_CMDS[] = { "AT", "ATE0", "AT+CMGF=0", "AT+CNMI=0,0,0,0,0" };
const int GSM_INIT_COUNT = sizeof(GSM_INIT_CMDS) / sizeof(GSM_INIT_CMDS[0]);

uint8_t smsSeptets[SMS_MAX_CHARS * 2];
int     smsSeptetCount = 0;
int     smsPartStart[SMS_MAX_PARTS + 1];
int     smsPartCount = 0;
int     smsPartIdx   = 0;
uint8_t smsConcatRef = 0;
char    smsPdu[340];

char smsText[SMS_MAX_CHARS + 1];        bool smsActive  = false;  int smsRecipientIdx = 0;  int smsRetry = 0;
char smsPendingText[SMS_MAX_CHARS + 1]; bool smsPending = false;

int  smsCandidateLevel  = ROAD_UNKNOWN;
int  smsCandidateCount  = 0;
int  smsConfirmedLevel  = ROAD_UNKNOWN;
int  smsNotifiedLevel   = ROAD_PASSABLE;
int  smsLowerLevel      = ROAD_UNKNOWN;
unsigned long smsLowerSinceT = 0;
unsigned long smsSentCount   = 0;
Preferences smsPrefs;

// -------------------------------------------------------------
// Telemetry State
// -------------------------------------------------------------
struct TelemetryData {
  int   rawPook;
  int   rawBay;
  int   rawLevel4;
  int   rawRain;

  float pookMm;
  float pookCm;
  float bayCm;
  float floodProb;          // % from the logistic model
  float bayMm;
  float level4Mm;
  float waterPercent;       // highest river level as % of the sensor range

  float rainfallMm;         // rain collected since the container was last emptied
  float rainRateMmH;        // rise over the last RAIN_WINDOW_MIN minutes, as mm/h
  String rainClass;

  int   roadClass;

  bool  ledGreen;
  bool  ledYellow;
  bool  ledRed;
  bool  ledBlue;

  int   rssi;
  unsigned long uptimeSeconds;
} telemetry;

unsigned long last1SecWindow  = 0;

bool serialMonitorOn = true;
unsigned long lastStatusPrint = 0;
const unsigned long SERIAL_STATUS_MS = 2000;

float rainHist[RAIN_WINDOW_MIN + 1];
int   rainHistCount = 0;
int   rainHistHead  = 0;
unsigned long lastRainHistT = 0;

// -------------------------------------------------------------
// Embedded Web Dashboard HTML (Served directly by ESP32)
// -------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FLOOD-SENTINEL Live Telemetry</title>
  <style>
    :root { --bg: #090d16; --card: rgba(18,26,47,0.85); --cyan: #06b6d4; --blue: #3b82f6; --green: #10b981; --yellow: #f59e0b; --red: #ef4444; }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: #f8fafc; padding: 20px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }
    .container { max-width: 900px; width: 100%; display: flex; flex-direction: column; gap: 16px; }
    header { background: var(--card); border: 1px solid rgba(255,255,255,0.1); border-radius: 14px; padding: 16px 20px; display: flex; justify-content: space-between; align-items: center; gap: 12px; flex-wrap: wrap; }
    h1 { font-size: 1.25rem; font-weight: 800; }
    .badge { padding: 4px 10px; border-radius: 99px; font-size: 0.75rem; font-weight: bold; background: rgba(16,185,129,0.2); color: var(--green); border: 1px solid var(--green); }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 16px; }
    .card { background: var(--card); border: 1px solid rgba(255,255,255,0.08); border-radius: 14px; padding: 18px; display: flex; flex-direction: column; gap: 12px; }
    .val-row { display: flex; align-items: baseline; gap: 4px; }
    .big-num { font-size: 2.2rem; font-weight: 800; color: var(--cyan); }
    .sub { font-size: 0.8rem; color: #94a3b8; }
    .lvl { display: flex; justify-content: space-between; font-size: 0.9rem; }
    .tank-bar { width: 100%; height: 12px; background: rgba(255,255,255,0.08); border-radius: 99px; overflow: hidden; }
    .tank-fill { height: 100%; background: linear-gradient(90deg, var(--cyan), var(--blue)); width: 0%; transition: width 0.4s ease; }
    .road { font-size: 1.4rem; font-weight: 800; }
    .leds-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
    .led-box { background: rgba(0,0,0,0.3); border-radius: 10px; padding: 10px; text-align: center; border: 1px solid rgba(255,255,255,0.06); font-size: 0.75rem; }
    .led-dot { width: 16px; height: 16px; border-radius: 50%; margin: 0 auto 6px auto; background: #334155; }
    .led-dot.on-green { background: var(--green); box-shadow: 0 0 12px var(--green); }
    .led-dot.on-yellow { background: var(--yellow); box-shadow: 0 0 12px var(--yellow); }
    .led-dot.on-red { background: var(--red); box-shadow: 0 0 12px var(--red); }
    .led-dot.on-blue { background: var(--blue); box-shadow: 0 0 12px var(--blue); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div>
        <h1>FLOOD-SENTINEL</h1>
        <p class="sub">Pook, Brgy. Simlong &bull; Rainfall &amp; River Water Level Monitor</p>
      </div>
      <span class="badge" id="net-badge">LIVE ON ESP32</span>
    </header>

    <div class="card">
      <p class="sub">Road condition</p>
      <span class="road" id="road">--</span>
      <p class="sub">Flood probability: <strong id="prob">--</strong> %</p>
      <p class="sub">SMS sent: <strong id="sms-count">--</strong></p>
    </div>

    <div class="grid">
      <div class="card">
        <h3>River Water Levels</h3>
        <div class="lvl"><span>Pook-side</span><strong id="pook">-- mm</strong></div>
        <div class="tank-bar"><div class="tank-fill" id="pook-bar"></div></div>
        <div class="lvl"><span>Bay-side</span><strong id="bay">-- mm</strong></div>
        <div class="tank-bar"><div class="tank-fill" id="bay-bar"></div></div>
        <div class="lvl"><span>Sensor 4</span><strong id="l4">-- mm</strong></div>
        <div class="tank-bar"><div class="tank-fill" id="l4-bar"></div></div>
        <p class="sub" id="raw-adc">ADC: --</p>
      </div>

      <div class="card">
        <h3>Rainfall</h3>
        <div class="val-row">
          <span class="big-num" id="rain-mm" style="color:var(--blue);">--</span><span style="font-size:1.2rem;color:#94a3b8;">mm collected</span>
        </div>
        <p class="sub">Rate: <strong id="rain-rate">--</strong> mm/h</p>
        <p class="sub" id="rain-class">Intensity: --</p>
      </div>

      <div class="card" style="grid-column: 1 / -1;">
        <h3>LED Indicators</h3>
        <div class="leds-row">
          <div class="led-box"><div class="led-dot" id="dot-green"></div>GREEN<br><span id="txt-green" class="sub">--</span></div>
          <div class="led-box"><div class="led-dot" id="dot-blue"></div>BLUE<br><span id="txt-blue" class="sub">--</span></div>
          <div class="led-box"><div class="led-dot" id="dot-yellow"></div>YELLOW<br><span id="txt-yellow" class="sub">--</span></div>
          <div class="led-box"><div class="led-dot" id="dot-red"></div>RED<br><span id="txt-red" class="sub">--</span></div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const ROAD_COLORS = { 'PASSABLE': '#10b981', 'RESTRICTED': '#3b82f6', 'HIGHLY RESTRICTED': '#f59e0b', 'IMPASSABLE': '#ef4444' };
    function bar(id, mm) { document.getElementById(id).style.width = Math.min(100, mm / 40 * 100) + '%'; }
    async function updateTelemetry() {
      try {
        const res = await fetch('/api/telemetry');
        const d = await res.json();
        document.getElementById('road').textContent = d.road_condition;
        document.getElementById('road').style.color = ROAD_COLORS[d.road_condition] || '#f8fafc';
        document.getElementById('sms-count').textContent = d.sms_sent;
        document.getElementById('prob').textContent = d.flood_prob.toFixed(1);
        document.getElementById('pook').textContent = d.pook_cm.toFixed(2) + ' cm'; bar('pook-bar', d.pook_mm);
        document.getElementById('bay').textContent = d.bay_cm.toFixed(2) + ' cm';   bar('bay-bar', d.bay_mm);
        document.getElementById('l4').textContent = d.level4_mm.toFixed(1) + ' mm'; bar('l4-bar', d.level4_mm);
        document.getElementById('raw-adc').textContent = `ADC  Pook ${d.s1_adc} | Bay ${d.s2_adc} | S4 ${d.s3_adc} | Rain ${d.rain_adc}`;
        document.getElementById('rain-mm').textContent = d.rainfall_mm.toFixed(1);
        document.getElementById('rain-rate').textContent = d.rain_rate.toFixed(1);
        document.getElementById('rain-class').textContent = 'Intensity: ' + d.rain_class;
        setLed('green', d.led_green, 'on-green');
        setLed('yellow', d.led_yellow, 'on-yellow');
        setLed('red', d.led_red, 'on-red');
        setLed('blue', d.led_blue, 'on-blue');
      } catch (e) {
        console.error('Fetch error:', e);
      }
    }
    function setLed(id, state, cls) {
      const dot = document.getElementById('dot-' + id);
      const txt = document.getElementById('txt-' + id);
      if (state) { dot.className = 'led-dot ' + cls; txt.textContent = 'ON'; }
      else { dot.className = 'led-dot'; txt.textContent = 'OFF'; }
    }
    setInterval(updateTelemetry, 1000);
    updateTelemetry();
  </script>
</body>
</html>
)rawliteral";

// =============================================================
// SENSOR READING & CALIBRATION
// =============================================================
int readAdcAvg(int pin) {
  int v[21];
  for (int i = 0; i < 21; i++) {
    int x = analogRead(pin), j = i;
    while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
    v[j] = x;
  }
  long sum = 0;
  for (int i = 5; i < 16; i++) sum += v[i];
  return (int)(sum / 11);
}

void calUpdateOnset(int s) {
  CalTable& t = cal[s];
  calOnset[s] = (t.n > 0) ? t.adc[0] : 0;
  if (t.n >= 3 && t.mm[0] == 0.0f && t.mm[2] > t.mm[1]) {
    float slope = (float)(t.adc[2] - t.adc[1]) / (t.mm[2] - t.mm[1]);
    float onset = (slope > 0) ? t.adc[1] - slope * t.mm[1] : t.adc[0];
    calOnset[s] = constrain(onset, (float)t.adc[0], (float)t.adc[1]);
  }
}

// Sensors jump up as soon as water touches them, then rise steadily.
// Below the jump ("onset") the reading counts as 0 mm.
float calAdcToMm(int s, int adc) {
  CalTable& t = cal[s];
  if (t.n < 2) return 0.0f;
  int start = 0;
  if (t.n >= 3 && t.mm[0] == 0.0f) {
    if (adc <= calOnset[s]) return 0.0f;
    if (adc <= t.adc[1]) {
      float span = t.adc[1] - calOnset[s];
      return (span > 0) ? t.mm[1] * (adc - calOnset[s]) / span : t.mm[1];
    }
    start = 1;
  } else if (adc <= t.adc[0]) {
    return t.mm[0];
  }
  if (adc >= t.adc[t.n - 1]) return t.mm[t.n - 1];
  int i = start + 1;
  while (i < t.n - 1 && adc > t.adc[i]) i++;
  float f = (float)(adc - t.adc[i - 1]) / (t.adc[i] - t.adc[i - 1]);
  return t.mm[i - 1] + f * (t.mm[i] - t.mm[i - 1]);
}

void calSave(int s) {
  char k[6]; snprintf(k, sizeof(k), "cal%d", s);
  calPrefs.putBytes(k, &cal[s], sizeof(CalTable));
  calUpdateOnset(s);
}

void calLoadAll() {
  calPrefs.begin("fscal2", false);
  for (int s = 0; s < 4; s++) {
    char k[6]; snprintf(k, sizeof(k), "cal%d", s);
    CalTable t;
    if (calPrefs.getBytes(k, &t, sizeof(CalTable)) == sizeof(CalTable) && t.n >= 0 && t.n <= CAL_MAX_POINTS) cal[s] = t;
    else cal[s] = CAL_DEFAULTS[s];
    calUpdateOnset(s);
  }
}

void calList(int s) {
  CalTable& t = cal[s];
  Serial.printf("[CAL] s%d %s (GPIO %d): %d point(s), wet onset ADC %.0f\n", s + 1, SENSOR_NAMES[s], SENSOR_PINS[s], t.n, calOnset[s]);
  for (int i = 0; i < t.n; i++) Serial.printf("        %5.1f mm -> ADC %4d\n", t.mm[i], t.adc[i]);
}

void calRecord(int s, float mm) {
  CalTable& t = cal[s];
  for (int i = 0; i < t.n; i++) if (fabsf(t.mm[i] - mm) < 0.01f) { Serial.println("[CAL] That depth is already recorded. Use CAL UNDO or CAL CLEAR first."); return; }
  if (t.n >= CAL_MAX_POINTS) { Serial.println("[CAL] Table full. Use CAL CLEAR."); return; }
  long sum = 0; int lo = 4095, hi = 0;
  for (int i = 0; i < 40; i++) {
    int v = readAdcAvg(SENSOR_PINS[s]);
    sum += v; if (v < lo) lo = v; if (v > hi) hi = v;
    delay(25);
  }
  int adc = (int)(sum / 40);
  int pos = t.n;
  while (pos > 0 && t.mm[pos - 1] > mm) { t.mm[pos] = t.mm[pos - 1]; t.adc[pos] = t.adc[pos - 1]; pos--; }
  t.mm[pos] = mm; t.adc[pos] = adc; t.n++;
  calSave(s);
  Serial.printf("[CAL] s%d recorded %.1f mm -> ADC %d (noise %d)\n", s + 1, mm, adc, hi - lo);
  if (hi - lo > 150) Serial.println("[CAL] WARNING: unstable reading – check the wire / let the water settle, then CAL UNDO and redo.");
  if (adc >= 4090)   Serial.println("[CAL] WARNING: reading is at the maximum (4095). Sensor is saturated at this depth.");
  for (int i = 1; i < t.n; i++)
    if (t.adc[i] <= t.adc[i - 1]) Serial.printf("[CAL] WARNING: ADC does not increase between %.1f and %.1f mm – redo.\n", t.mm[i - 1], t.mm[i]);
}

void calHandleCommand(const char* arg) {
  while (*arg == ' ') arg++;
  if (!strcmp(arg, "DONE")) {
    calMode = false;
    Serial.println("[CAL] Calibration mode OFF – SMS alerts active again.");
    return;
  }
  if (!calMode) Serial.println("[CAL] Calibration mode ON – SMS alerts PAUSED (type CAL DONE when finished, auto-resume after 15 min).");
  calMode = true;
  calLastCmdT = millis();
  if (arg[0] == 'S' && arg[1] >= '1' && arg[1] <= '4' && arg[2] == '\0') {
    calSel = arg[1] - '1';
    Serial.printf("[CAL] Selected s%d: %s\n", calSel + 1, SENSOR_NAMES[calSel]);
    calList(calSel);
  } else if (!strcmp(arg, "LIST")) {
    for (int s = 0; s < 4; s++) calList(s);
  } else if (!strcmp(arg, "CLEAR")) {
    cal[calSel].n = 0; calSave(calSel);
    Serial.printf("[CAL] s%d cleared. Record 0 (dry) first, then the wet depths.\n", calSel + 1);
  } else if (!strcmp(arg, "UNDO")) {
    if (cal[calSel].n > 0) { cal[calSel].n--; calSave(calSel); }
    calList(calSel);
  } else if (!strcmp(arg, "DEFAULT")) {
    cal[calSel] = CAL_DEFAULTS[calSel]; calSave(calSel);
    Serial.printf("[CAL] s%d restored to the default calibration.\n", calSel + 1);
  } else {
    char* end; float mm = strtof(arg, &end);
    if (end != arg && *end == '\0' && mm >= 0 && mm <= 1000) calRecord(calSel, mm);
    else Serial.println("[CAL] Commands: CAL S1..S4, CAL <mm>, CAL LIST, CAL UNDO, CAL CLEAR, CAL DEFAULT, CAL DONE");
  }
}

void processWaterLevelSensors() {
  telemetry.rawPook   = readAdcAvg(PIN_POOK_LEVEL);
  telemetry.rawBay    = readAdcAvg(PIN_BAY_LEVEL);
  telemetry.rawLevel4 = readAdcAvg(PIN_LEVEL4);

  telemetry.pookMm   = calAdcToMm(SENSOR_POOK, telemetry.rawPook);
  telemetry.bayMm    = calAdcToMm(SENSOR_BAY,  telemetry.rawBay);
  telemetry.level4Mm = calAdcToMm(SENSOR_S4,   telemetry.rawLevel4);

  float highest = max(telemetry.pookMm, max(telemetry.bayMm, telemetry.level4Mm));
  telemetry.waterPercent = constrain(highest / SENSOR_RANGE_MM * 100.0f, 0.0f, 100.0f);
}

void processRainfall() {
  telemetry.rawRain    = readAdcAvg(PIN_RAIN_COLLECTOR);
  telemetry.rainfallMm = calAdcToMm(SENSOR_RAIN, telemetry.rawRain) * RAIN_AREA_RATIO;

  unsigned long now = millis();
  if (rainHistCount > 0) {
    int lastIdx = (rainHistHead + RAIN_WINDOW_MIN) % (RAIN_WINDOW_MIN + 1);
    if (rainHist[lastIdx] - telemetry.rainfallMm > RAIN_EMPTY_DROP_MM) {
      rainHistCount = 0;
      Serial.println("[RAIN] Container emptied – rainfall history reset.");
    }
  }
  if (rainHistCount == 0 || now - lastRainHistT >= 60000UL) {
    lastRainHistT = now;
    rainHist[rainHistHead] = telemetry.rainfallMm;
    rainHistHead = (rainHistHead + 1) % (RAIN_WINDOW_MIN + 1);
    if (rainHistCount < RAIN_WINDOW_MIN + 1) rainHistCount++;
  }

  telemetry.rainRateMmH = 0.0f;
  if (rainHistCount >= 2) {
    int oldestIdx = (rainHistHead - rainHistCount + 2 * (RAIN_WINDOW_MIN + 1)) % (RAIN_WINDOW_MIN + 1);
    float rise = telemetry.rainfallMm - rainHist[oldestIdx];
    float minutes = (rainHistCount - 1) + (now - lastRainHistT) / 60000.0f;
    if (rise >= RAIN_MIN_RISE_MM && minutes > 0) telemetry.rainRateMmH = rise * 60.0f / minutes;
  }
}

// =============================================================
// RAIN CLASSIFICATION, ROAD CLASSIFICATION & LED INDICATORS
// =============================================================
void updateClassificationsAndLeds() {
  if (telemetry.rainRateMmH == 0.0f) {
    telemetry.rainClass = "Dry (No Rain)";
  } else if (telemetry.rainRateMmH < 2.5f) {
    telemetry.rainClass = "Light Rain / Drizzle";
  } else if (telemetry.rainRateMmH < 10.0f) {
    telemetry.rainClass = "Moderate Rain";
  } else if (telemetry.rainRateMmH < 50.0f) {
    telemetry.rainClass = "Heavy Rain";
  } else {
    telemetry.rainClass = "Torrential Storm";
  }

  telemetry.pookCm = telemetry.pookMm / 10.0f;
  telemetry.bayCm  = telemetry.bayMm / 10.0f;
  float z = MODEL_B0 + MODEL_B_RAIN * telemetry.rainfallMm + MODEL_B_POOK * telemetry.pookCm + MODEL_B_BAY * telemetry.bayCm;
  telemetry.floodProb = 100.0f / (1.0f + expf(-z));

  if      (telemetry.floodProb >= PROB_IMPASSABLE)      telemetry.roadClass = ROAD_IMPASSABLE;
  else if (telemetry.floodProb >= PROB_MORE_RESTRICTED) telemetry.roadClass = ROAD_MORE_RESTRICTED;
  else if (telemetry.floodProb >= PROB_RESTRICTED)      telemetry.roadClass = ROAD_RESTRICTED;
  else                                                  telemetry.roadClass = ROAD_PASSABLE;

  telemetry.ledGreen  = (telemetry.roadClass == ROAD_PASSABLE);
  telemetry.ledBlue   = (telemetry.roadClass == ROAD_RESTRICTED);
  telemetry.ledYellow = (telemetry.roadClass == ROAD_MORE_RESTRICTED);
  telemetry.ledRed    = (telemetry.roadClass == ROAD_IMPASSABLE);

  digitalWrite(PIN_LED_GREEN,  telemetry.ledGreen  ? HIGH : LOW);
  digitalWrite(PIN_LED_BLUE,   telemetry.ledBlue   ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, telemetry.ledYellow ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,    telemetry.ledRed    ? HIGH : LOW);
}

// =============================================================
// SIM800L – INITIALIZATION & NON-BLOCKING STATE MACHINE
// =============================================================
void simInit() {
  sim800.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX_PIN, SIM800_TX_PIN);
  gsmState  = GSM_BOOT_WAIT;
  gsmStateT = millis();
  smsPrefs.begin("fsms", false);
  smsNotifiedLevel = smsPrefs.getInt("lastLvl", ROAD_PASSABLE);
  if (smsNotifiedLevel < ROAD_PASSABLE || smsNotifiedLevel > ROAD_IMPASSABLE) smsNotifiedLevel = ROAD_PASSABLE;
  Serial.printf("[GSM] SIM800L on UART2 – initializing in background. Last notified: %s\n",
                ROAD_CLASS_NAMES[smsNotifiedLevel]);
}

static void gsmClearRx() { gsmRxLen = 0; gsmRx[0] = '\0'; }

static void gsmCommand(const char* cmd, const char* expect, unsigned long timeoutMs) {
  while (sim800.available()) sim800.read();
  gsmClearRx();
  sim800.print(cmd);
  sim800.print("\r");
  gsmExpect = expect;
  gsmCmdTO  = timeoutMs;
  gsmStateT = millis();
}

static void gsmReadIntoBuffer() {
  while (sim800.available()) {
    char c = (char)sim800.read();
    if (c == '\0') continue;
    if (gsmRxLen >= sizeof(gsmRx) - 1) {
      size_t half = gsmRxLen / 2;
      memmove(gsmRx, gsmRx + half, gsmRxLen - half);
      gsmRxLen -= half;
    }
    gsmRx[gsmRxLen++] = c;
    gsmRx[gsmRxLen] = '\0';
  }
}

static int gsmPoll() {
  gsmReadIntoBuffer();
  if (gsmExpect && strstr(gsmRx, gsmExpect)) return 1;
  if (strstr(gsmRx, "ERROR"))                 return -1;
  if (millis() - gsmStateT >= gsmCmdTO)       return -2;
  return 0;
}

static bool gsmParseRegistered() {
  const char* p = strstr(gsmRx, "+CREG:");
  if (!p) return false;
  p = strchr(p, ',');
  if (!p) return false;
  int stat = atoi(p + 1);
  return (stat == 1 || stat == 5);
}

static void gsmEnter(GsmState s) { gsmState = s; gsmStateT = millis(); }

static void gsmFail(const char* where) {
  Serial.printf("[GSM] %s failed -> re-init in %lus\n", where, GSM_RETRY_BACKOFF_MS / 1000);
  gsmRegistered = false;
  gsmEnter(GSM_BACKOFF);
}

// =============================================================
// SMS – PDU ENCODING (multi-part messages)
// =============================================================
static int smsGsmCode(char c, uint8_t* out) {
  switch (c) {
    case '@':  out[0] = 0x00; return 1;
    case '$':  out[0] = 0x02; return 1;
    case '_':  out[0] = 0x11; return 1;
    case '^':  out[0] = 0x1B; out[1] = 0x14; return 2;
    case '{':  out[0] = 0x1B; out[1] = 0x28; return 2;
    case '}':  out[0] = 0x1B; out[1] = 0x29; return 2;
    case '\\': out[0] = 0x1B; out[1] = 0x2F; return 2;
    case '[':  out[0] = 0x1B; out[1] = 0x3C; return 2;
    case '~':  out[0] = 0x1B; out[1] = 0x3D; return 2;
    case ']':  out[0] = 0x1B; out[1] = 0x3E; return 2;
    case '|':  out[0] = 0x1B; out[1] = 0x40; return 2;
    case '`':  out[0] = 0x27; return 1;
  }
  if (c == '\n' || c == '\r' || (c >= 0x20 && c <= 0x7E)) { out[0] = (uint8_t)c; return 1; }
  out[0] = '?'; return 1;
}

static void smsPrepareParts(const char* text) {
  smsSeptetCount = 0;
  for (const char* p = text; *p && smsSeptetCount < (int)sizeof(smsSeptets) - 2; p++) {
    smsSeptetCount += smsGsmCode(*p, &smsSeptets[smsSeptetCount]);
  }
  smsPartStart[0] = 0;
  if (smsSeptetCount <= 160) {
    smsPartCount = 1;
    smsPartStart[1] = smsSeptetCount;
    return;
  }
  smsPartCount = 0;
  int pos = 0;
  while (pos < smsSeptetCount && smsPartCount < SMS_MAX_PARTS) {
    int end = pos + 153;
    if (end >= smsSeptetCount) end = smsSeptetCount;
    else if (smsSeptets[end - 1] == 0x1B) end--;
    smsPartStart[smsPartCount++] = pos;
    pos = end;
  }
  smsPartStart[smsPartCount] = pos;
  if (pos < smsSeptetCount) Serial.println("[SMS] WARNING: message too long – extra text dropped.");
  smsConcatRef++;
}

static const char HEXD[] = "0123456789ABCDEF";
static int pduPutByte(char* out, int o, uint8_t b) { out[o] = HEXD[b >> 4]; out[o + 1] = HEXD[b & 0x0F]; return o + 2; }

static int smsBuildPdu(const char* number, int part) {
  int o = 0;
  bool multi = (smsPartCount > 1);
  o = pduPutByte(smsPdu, o, 0x00);
  o = pduPutByte(smsPdu, o, multi ? 0x41 : 0x01);
  o = pduPutByte(smsPdu, o, 0x00);

  const char* num = number;
  uint8_t toa = 0x81;
  if (*num == '+') { toa = 0x91; num++; }
  int nd = 0; char digits[20];
  for (const char* p = num; *p && nd < 20; p++) if (*p >= '0' && *p <= '9') digits[nd++] = *p;
  o = pduPutByte(smsPdu, o, (uint8_t)nd);
  o = pduPutByte(smsPdu, o, toa);
  for (int i = 0; i < nd; i += 2) {
    smsPdu[o++] = (i + 1 < nd) ? digits[i + 1] : 'F';
    smsPdu[o++] = digits[i];
  }
  o = pduPutByte(smsPdu, o, 0x00);
  o = pduPutByte(smsPdu, o, 0x00);

  int s0 = smsPartStart[part], s1 = smsPartStart[part + 1], n = s1 - s0;
  uint8_t ud[160];
  memset(ud, 0, sizeof(ud));
  int udhOctets = 0, fillBits = 0;
  if (multi) {
    ud[0] = 0x05; ud[1] = 0x00; ud[2] = 0x03;
    ud[3] = smsConcatRef; ud[4] = (uint8_t)smsPartCount; ud[5] = (uint8_t)(part + 1);
    udhOctets = 6; fillBits = 1;
  }
  int bitPos = udhOctets * 8 + fillBits;
  for (int i = 0; i < n; i++) {
    uint8_t s = smsSeptets[s0 + i] & 0x7F;
    int byteI = bitPos / 8, shift = bitPos % 8;
    ud[byteI] |= (uint8_t)(s << shift);
    if (shift > 1) ud[byteI + 1] |= (uint8_t)(s >> (8 - shift));
    bitPos += 7;
  }
  int udOctets = (bitPos + 7) / 8;
  int udl = multi ? (7 + n) : n;
  o = pduPutByte(smsPdu, o, (uint8_t)udl);
  for (int i = 0; i < udOctets; i++) o = pduPutByte(smsPdu, o, ud[i]);
  smsPdu[o] = '\0';
  return o / 2 - 1;
}

static void gsmStartCurrentPart() {
  int tpduLen = smsBuildPdu(SMS_RECIPIENTS[smsRecipientIdx], smsPartIdx);
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", tpduLen);
  gsmCommand(cmd, ">", GSM_PROMPT_TIMEOUT_MS);
  gsmState = GSM_SMS_PROMPT;
}

static void gsmBeginBatch() {
  smsPrepareParts(smsText);
  smsRecipientIdx = 0; smsPartIdx = 0; smsRetry = 0;
  gsmStartCurrentPart();
}

static void gsmNextRecipientOrFinish() {
  smsRecipientIdx++;
  smsPartIdx = 0;
  smsRetry = 0;
  if (smsRecipientIdx < SMS_RECIPIENT_COUNT) {
    gsmStartCurrentPart();
    return;
  }
  smsActive = false;
  if (smsPending) {
    strncpy(smsText, smsPendingText, sizeof(smsText));
    smsText[sizeof(smsText) - 1] = '\0';
    smsPending = false; smsActive = true;
    gsmBeginBatch();
  } else {
    gsmEnter(GSM_READY);
  }
}

static void gsmPartDone() {
  smsPartIdx++;
  smsRetry = 0;
  if (smsPartIdx < smsPartCount) gsmStartCurrentPart();
  else {
    smsSentCount++;
    Serial.printf("[SMS] SENT #%lu to %s (%d part%s) at uptime %lus\n", smsSentCount,
                  SMS_RECIPIENTS[smsRecipientIdx], smsPartCount, smsPartCount > 1 ? "s" : "", millis() / 1000);
    gsmNextRecipientOrFinish();
  }
}

static void gsmRetryOrSkip(const char* why) {
  if (smsRetry < SMS_MAX_RETRIES) {
    smsRetry++;
    Serial.printf("[SMS] %s to %s (part %d) – retry %d/%d\n", why, SMS_RECIPIENTS[smsRecipientIdx],
                  smsPartIdx + 1, smsRetry, SMS_MAX_RETRIES);
    gsmStartCurrentPart();
  } else {
    Serial.printf("[SMS] FAILED to %s (%s) – skipping\n", SMS_RECIPIENTS[smsRecipientIdx], why);
    gsmNextRecipientOrFinish();
  }
}

void simService() {
  int r;
  switch (gsmState) {

    case GSM_BOOT_WAIT:
      if (millis() - gsmStateT >= GSM_BOOT_DELAY_MS) {
        gsmInitIdx = 0;
        gsmCommand(GSM_INIT_CMDS[0], "OK", GSM_CMD_TIMEOUT_MS);
        gsmState = GSM_INIT_STEP;
      }
      break;

    case GSM_INIT_STEP:
      r = gsmPoll();
      if (r == 1) {
        gsmInitIdx++;
        if (gsmInitIdx < GSM_INIT_COUNT) {
          gsmCommand(GSM_INIT_CMDS[gsmInitIdx], "OK", GSM_CMD_TIMEOUT_MS);
        } else {
          Serial.println("[GSM] SIM800L responding, PDU mode set. Waiting for network...");
          gsmRegStartT = millis();
          gsmCommand("AT+CREG?", "OK", GSM_CMD_TIMEOUT_MS);
          gsmState = GSM_REG_CHECK;
        }
      } else if (r < 0) {
        gsmFail(GSM_INIT_CMDS[gsmInitIdx]);
      }
      break;

    case GSM_REG_CHECK:
      r = gsmPoll();
      if (r == 1) {
        if (gsmParseRegistered()) {
          gsmRegistered = true; gsmHealthFails = 0;
          Serial.println("[GSM] Registered on network – SMS ready.");
          if (smsActive) gsmBeginBatch();
          else gsmEnter(GSM_READY);
        } else if (millis() - gsmRegStartT >= GSM_REG_GIVEUP_MS) {
          gsmFail("Network registration (check SIM, antenna, power)");
        } else {
          gsmEnter(GSM_REG_WAIT);
        }
      } else if (r < 0) {
        gsmFail("AT+CREG?");
      }
      break;

    case GSM_REG_WAIT:
      if (millis() - gsmStateT >= GSM_REG_POLL_MS) {
        gsmCommand("AT+CREG?", "OK", GSM_CMD_TIMEOUT_MS);
        gsmState = GSM_REG_CHECK;
      }
      break;

    case GSM_READY:
      while (sim800.available()) sim800.read();
      if (smsActive) {
        gsmBeginBatch();
      } else if (millis() - gsmStateT >= GSM_HEALTH_CHECK_MS) {
        gsmCommand("AT+CREG?", "OK", GSM_CMD_TIMEOUT_MS);
        gsmState = GSM_HEALTH;
      }
      break;

    case GSM_HEALTH:
      r = gsmPoll();
      if (r == 1 && gsmParseRegistered()) {
        gsmHealthFails = 0;
        gsmEnter(GSM_READY);
      } else if (r != 0) {
        if (++gsmHealthFails >= 3) gsmFail("Health check");
        else gsmEnter(GSM_READY);
      }
      break;

    case GSM_SMS_PROMPT:
      r = gsmPoll();
      if (r == 1) {
        sim800.print(smsPdu);
        sim800.write(0x1A);
        gsmClearRx();
        gsmExpect = "+CMGS:";
        gsmCmdTO  = GSM_SEND_TIMEOUT_MS;
        gsmStateT = millis();
        gsmState  = GSM_SMS_BODY;
      } else if (r < 0) {
        sim800.write(0x1B);
        gsmRetryOrSkip(r == -2 ? "no '>' prompt" : "CMGS error");
      }
      break;

    case GSM_SMS_BODY:
      r = gsmPoll();
      if (r == 1) {
        gsmPartDone();
      } else if (r < 0) {
        gsmRetryOrSkip(r == -2 ? "send timeout" : "network rejected");
      }
      break;

    case GSM_BACKOFF:
      if (millis() - gsmStateT >= GSM_RETRY_BACKOFF_MS) {
        gsmInitIdx = 0;
        gsmCommand(GSM_INIT_CMDS[0], "OK", GSM_CMD_TIMEOUT_MS);
        gsmState = GSM_INIT_STEP;
      }
      break;
  }
}

// =============================================================
// SMS – MESSAGE QUEUE & ROAD-CONDITION CHANGE LOGIC
// =============================================================
static void smsFormatValue(char* out, size_t n, float v, const char* unit) {
  if (isnan(v)) snprintf(out, n, "N/A");
  else          snprintf(out, n, "%.1f%s", v, unit);
}

void smsFillTemplate(char* out, size_t n, const char* tmpl, int roadClass,
                     float floodProbPct, float rainfall, float pookLevel, float bayLevel) {
  size_t o = 0;
  for (const char* p = tmpl; *p; ) {
    char val[24]; const char* rep = NULL; size_t skip = 0;
    if (*p == '{') {
      if      (!strncmp(p, "{ROAD}", 6)) { rep = (roadClass >= 0 && roadClass <= 3) ? ROAD_CLASS_NAMES[roadClass] : "UNKNOWN"; skip = 6; }
      else if (!strncmp(p, "{PROB}", 6)) { smsFormatValue(val, sizeof(val), floodProbPct, "%");         rep = val; skip = 6; }
      else if (!strncmp(p, "{RAIN}", 6)) { smsFormatValue(val, sizeof(val), rainfall,  SMS_RAIN_UNIT);  rep = val; skip = 6; }
      else if (!strncmp(p, "{POOK}", 6)) { smsFormatValue(val, sizeof(val), pookLevel, SMS_LEVEL_UNIT); rep = val; skip = 6; }
      else if (!strncmp(p, "{BAY}",  5)) { smsFormatValue(val, sizeof(val), bayLevel,  SMS_LEVEL_UNIT); rep = val; skip = 5; }
    }
    if (rep) {
      for (const char* r = rep; *r && o < n - 1; r++) out[o++] = *r;
      p += skip;
    } else {
      if (o < n - 1) out[o++] = *p;
      p++;
    }
  }
  out[o] = '\0';
}

bool sendSMS(const char* text) {
  if (!text || !text[0]) return false;
  if (!smsActive) {
    strncpy(smsText, text, sizeof(smsText)); smsText[sizeof(smsText) - 1] = '\0';
    smsActive = true;
    if (!gsmRegistered) Serial.println("[SMS] Queued – will send when the modem is registered.");
  } else {
    strncpy(smsPendingText, text, sizeof(smsPendingText)); smsPendingText[sizeof(smsPendingText) - 1] = '\0';
    smsPending = true;
  }
  Serial.printf("[SMS] Message queued for %d numbers:\n", SMS_RECIPIENT_COUNT);
  Serial.println(text);
  return true;
}

static void smsSaveNotified(int level) {
  smsNotifiedLevel = level;
  smsPrefs.putInt("lastLvl", level);
}

void smsCheckRoadCondition(int roadClass, float floodProbPct, float rainfall, float pookLevel, float bayLevel) {
  if (roadClass < ROAD_PASSABLE || roadClass > ROAD_IMPASSABLE) return;

  if (roadClass == smsCandidateLevel) { if (smsCandidateCount < SMS_CONFIRM_SAMPLES) smsCandidateCount++; }
  else { smsCandidateLevel = roadClass; smsCandidateCount = 1; }
  if (smsCandidateCount >= SMS_CONFIRM_SAMPLES) smsConfirmedLevel = smsCandidateLevel;
  if (smsConfirmedLevel == ROAD_UNKNOWN) return;

  unsigned long now = millis();
  int target = ROAD_UNKNOWN;

  if (smsConfirmedLevel > smsNotifiedLevel) {
    target = smsConfirmedLevel;
    smsLowerLevel = ROAD_UNKNOWN; smsLowerSinceT = 0;
  } else if (smsConfirmedLevel < smsNotifiedLevel) {
    if (smsLowerLevel != smsConfirmedLevel) {
      smsLowerLevel  = smsConfirmedLevel;
      smsLowerSinceT = now;
    } else if (now - smsLowerSinceT >= SMS_DOWNGRADE_HOLD_MS) {
      target = smsConfirmedLevel;
      smsLowerLevel = ROAD_UNKNOWN; smsLowerSinceT = 0;
    }
  } else {
    smsLowerLevel = ROAD_UNKNOWN; smsLowerSinceT = 0;
  }

  if (target != ROAD_UNKNOWN) {
    char msg[SMS_MAX_CHARS + 1];
    smsFillTemplate(msg, sizeof(msg), SMS_TEXTS[target], target, floodProbPct, rainfall, pookLevel, bayLevel);
    sendSMS(msg);
    smsSaveNotified(target);
  }
}

// =============================================================
// SERIAL MONITOR (115200 baud, line ending "Newline")
//   MON          turn the 2-second status line on/off
//   SMSTEST      send a test SMS to all numbers
//   SMSRESET     reset the saved road state to PASSABLE
//   CAL S1..S4   select sensor (1 Rain, 2 Pook, 3 Bay, 4 Sensor 4)
//   CAL <mm>     record the current reading at that water depth (e.g. CAL 0, CAL 10)
//   CAL LIST / CAL UNDO / CAL CLEAR / CAL DEFAULT
//   CAL DONE     finish calibrating (SMS alerts are paused while calibrating)
// =============================================================
void printStatus() {
  int rc = telemetry.roadClass;
  const char* led = telemetry.ledRed ? "RED" : telemetry.ledYellow ? "YELLOW" : telemetry.ledBlue ? "BLUE" : "GREEN";
  Serial.printf("[DATA] Rain %.1f mm (%.1f mm/h, ADC %d) | Pook %.2f cm (ADC %d) | Bay %.2f cm (ADC %d) | S4 %.1f mm (ADC %d)\n",
                telemetry.rainfallMm, telemetry.rainRateMmH, telemetry.rawRain,
                telemetry.pookCm, telemetry.rawPook, telemetry.bayCm, telemetry.rawBay,
                telemetry.level4Mm, telemetry.rawLevel4);
  Serial.printf("       Flood probability: %.1f %%\n", telemetry.floodProb);
  Serial.printf("       Road: %s | LED: %s | Rain: %s | GSM: %s | SMS sent: %lu%s | WiFi: %s\n",
                (rc >= 0 && rc <= 3) ? ROAD_CLASS_NAMES[rc] : "UNKNOWN", led, telemetry.rainClass.c_str(),
                gsmRegistered ? "READY" : "CONNECTING", smsSentCount, calMode ? " (PAUSED: calibrating)" : "",
                WiFi.status() == WL_CONNECTED ? "connected" : "AP mode");
}

void handleSerialCommands() {
  static char line[24]; static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line[len] = '\0';
      for (uint8_t i = 0; i < len; i++) line[i] = toupper((unsigned char)line[i]);
      if (len == 0) { }
      else if (!strcmp(line, "MON")) { serialMonitorOn = !serialMonitorOn; Serial.printf("[MON] Status output %s\n", serialMonitorOn ? "ON" : "OFF"); }
      else if (!strcmp(line, "SMSTEST")) sendSMS("FLOOD-SENTINEL\nTest message. SMS module is working.");
      else if (!strcmp(line, "SMSRESET")) { smsSaveNotified(ROAD_PASSABLE); smsLowerLevel = ROAD_UNKNOWN; Serial.println("[SMS] Saved road state reset to PASSABLE."); }
      else if (!strncmp(line, "CAL", 3)) calHandleCommand(line + 3);
      else Serial.println("Commands: MON, SMSTEST, SMSRESET, CAL S1..S4, CAL <mm>, CAL LIST, CAL UNDO, CAL CLEAR, CAL DEFAULT, CAL DONE");
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// =============================================================
// TELEMETRY JSON (local API + Vercel)
// =============================================================
String buildTelemetryJson() {
  int rc = telemetry.roadClass;
  String json = "{";
  json += "\"water_pct\":" + String(telemetry.waterPercent, 1) + ",";
  json += "\"s1_adc\":" + String(telemetry.rawPook) + ",";
  json += "\"s2_adc\":" + String(telemetry.rawBay) + ",";
  json += "\"s3_adc\":" + String(telemetry.rawLevel4) + ",";
  json += "\"rain_adc\":" + String(telemetry.rawRain) + ",";
  json += "\"pook_mm\":" + String(telemetry.pookMm, 1) + ",";
  json += "\"pook_cm\":" + String(telemetry.pookCm, 2) + ",";
  json += "\"bay_cm\":" + String(telemetry.bayCm, 2) + ",";
  json += "\"level4_cm\":" + String(telemetry.level4Mm / 10.0f, 2) + ",";
  json += "\"flood_prob\":" + String(telemetry.floodProb, 1) + ",";
  json += "\"gsm_ready\":" + String(gsmRegistered ? "true" : "false") + ",";
  json += "\"bay_mm\":" + String(telemetry.bayMm, 1) + ",";
  json += "\"level4_mm\":" + String(telemetry.level4Mm, 1) + ",";
  json += "\"rainfall_mm\":" + String(telemetry.rainfallMm, 1) + ",";
  json += "\"rain_rate\":" + String(telemetry.rainRateMmH, 2) + ",";
  json += "\"rain_class\":\"" + telemetry.rainClass + "\",";
  json += "\"drop_diameter\":0,\"avg_diameter\":0,\"drops_per_min\":0,\"peak_impulse_mv\":0,";
  json += "\"road_class\":" + String(rc) + ",";
  json += "\"road_condition\":\"" + String((rc >= 0 && rc <= 3) ? ROAD_CLASS_NAMES[rc] : "UNKNOWN") + "\",";
  json += "\"sms_sent\":" + String(smsSentCount) + ",";
  json += "\"led_green\":" + String(telemetry.ledGreen ? "true" : "false") + ",";
  json += "\"led_yellow\":" + String(telemetry.ledYellow ? "true" : "false") + ",";
  json += "\"led_red\":" + String(telemetry.ledRed ? "true" : "false") + ",";
  json += "\"led_blue\":" + String(telemetry.ledBlue ? "true" : "false") + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  return json;
}

// -------------------------------------------------------------
// REST API Handlers
// -------------------------------------------------------------
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleTelemetryApi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", buildTelemetryJson());
}

// -------------------------------------------------------------
// Vercel Telemetry Push Client (HTTPS)
// -------------------------------------------------------------
void pushTelemetryToVercel() {
  if (WiFi.status() != WL_CONNECTED) return;
  bool roadChanged = (telemetry.roadClass != lastCloudRoadClass);
  if (!roadChanged && millis() - lastCloudSync < CLOUD_SYNC_MS) return;
  lastCloudSync = millis();
  lastCloudRoadClass = telemetry.roadClass;

  if (String(VERCEL_HOST).indexOf("your-project") >= 0) return;

  httpsClient.setInsecure();
  HTTPClient http;
  String url = "https://" + String(VERCEL_HOST) + "/api/telemetry";

  if (http.begin(httpsClient, url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", VERCEL_API_KEY);
    http.addHeader("bridge_key", VERCEL_API_KEY);

    int httpCode = http.POST(buildTelemetryJson());
    if (httpCode > 0) {
      Serial.printf("[VERCEL] Telemetry pushed to %s -> HTTP %d\n", VERCEL_HOST, httpCode);
    } else {
      Serial.printf("[VERCEL] POST failed: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}

// -------------------------------------------------------------
// Setup & Configuration
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- FLOOD-SENTINEL (Project BRIDGE) ---");
  simInit();

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);

  digitalWrite(PIN_LED_GREEN, HIGH); delay(120); digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_BLUE, HIGH); delay(120); digitalWrite(PIN_LED_BLUE, LOW);
  digitalWrite(PIN_LED_YELLOW, HIGH); delay(120); digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_RED, HIGH); delay(120); digitalWrite(PIN_LED_RED, LOW);

  analogReadResolution(12);
  pinMode(PIN_RAIN_COLLECTOR, INPUT);
  pinMode(PIN_POOK_LEVEL, INPUT);
  pinMode(PIN_BAY_LEVEL, INPUT);
  pinMode(PIN_LEVEL4, INPUT);
  analogSetPinAttenuation(PIN_RAIN_COLLECTOR, ADC_11db);
  analogSetPinAttenuation(PIN_POOK_LEVEL, ADC_11db);
  analogSetPinAttenuation(PIN_BAY_LEVEL, ADC_11db);
  analogSetPinAttenuation(PIN_LEVEL4, ADC_11db);

  calLoadAll();
  telemetry.roadClass = ROAD_UNKNOWN;
  telemetry.rainClass = "Dry (No Rain)";

  Serial.print("Connecting to: "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startT = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startT < 8000)) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi unreachable. Starting SoftAP 'HydroSense-AP'...");
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print("SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleRoot);
  server.on("/api/telemetry", handleTelemetryApi);
  server.begin();
  Serial.println("Web server started.");
  Serial.println("Serial commands: MON, SMSTEST, SMSRESET, CAL S1..S4, CAL <mm>, CAL LIST, CAL UNDO, CAL CLEAR, CAL DEFAULT, CAL DONE");
}

// -------------------------------------------------------------
// Main Loop
// -------------------------------------------------------------
void loop() {
  server.handleClient();
  simService();
  handleSerialCommands();

  if (millis() - last1SecWindow >= 1000) {
    last1SecWindow = millis();
    processWaterLevelSensors();
    processRainfall();
    updateClassificationsAndLeds();
    if (calMode && millis() - calLastCmdT >= CAL_MODE_TIMEOUT_MS) {
      calMode = false;
      Serial.println("[CAL] No CAL command for 15 min – SMS alerts active again.");
    }
    if (!calMode) smsCheckRoadCondition(telemetry.roadClass, telemetry.floodProb, telemetry.rainfallMm, telemetry.pookCm, telemetry.bayCm);

    if (serialMonitorOn && millis() - lastStatusPrint >= SERIAL_STATUS_MS) {
      lastStatusPrint = millis();
      printStatus();
    }
  }

  pushTelemetryToVercel();
}
