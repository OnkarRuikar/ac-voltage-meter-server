/**

The AC voltage monitor server is hosted on NodeMCU ESP-12E. And ZMPT101B is used for sensing AC voltage.
You need to calibrate ZMPT101B so that the output waveform is not clipped. For that in loop() function uncomment first two lines and dump raw values.
See the graph in Arduino IDE's Serial Plotter. Adjust the trimpot on the sensor so that the sine wave is not clipped.

My ESP module is NodeMCU E12E (ESP8266EX)
Crystal is 26MHz
Auto-detected Flash size: 4MB

*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <math.h>
#include <ElegantOTA.h>

// Use your WiFi credentials
const char* WIFI_SSID = "abc";
const char* WIFI_PASSWORD = "11223344";

// Update pin numbers as per your ESP module
const uint8_t SENSOR_PIN = A0;
const uint8_t WIFI_LED = D0; // devkit LED
const uint8_t WEB_REQ_LED = D4; // module LED

// ---------------- Sensor configuration ----------------
// Adjust this after comparing against a reliable AC meter/multimeter.
// new calibration factor = (multimeter reading / this program reading) * current calibration factor
float calibrationFactor = 2.83;

// Sample for approximately 200 ms. At 50 Hz, this covers about 10 AC cycles.
const unsigned long SAMPLE_WINDOW_MS = 200;

// Read voltage about twice per second.
const unsigned long UPDATE_INTERVAL_MS = 500;


ESP8266WebServer server(80);
float voltageRMS = 0.0f;
unsigned long lastUpdate = 0;

float readVoltageRMS() {
  unsigned long start = millis();
  unsigned long count = 0;
  double sum = 0;
  double sumSquares = 0;

  while ((millis() - start) < SAMPLE_WINDOW_MS) {
    int sample = analogRead(SENSOR_PIN);

    sum += sample;
    sumSquares += (double)sample * sample;
    count++;
  }

  if (count == 0) return 0;

  double mean = sum / count;
  double variance = (sumSquares / count) - (mean * mean);

  if (variance < 0) variance = 0;

  double adcRms = sqrt(variance);
  return adcRms * calibrationFactor;
}

// ---------------- HTTP handlers ----------------

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AC Voltage Monitor</title>

<style>
  * { box-sizing: border-box; }

  body {
    margin: 0;
    padding: 16px;
    background: #101827;
    color: #e5e7eb;
    font-family: Arial, sans-serif;
  }

  .container {
    max-width: 850px;
    margin: auto;
  }

  h1 {
    font-size: 24px;
    margin-bottom: 6px;
  }

  .subtitle {
    color: #9ca3af;
    margin-bottom: 22px;
  }

  .card {
    background: #1b2638;
    padding: 20px;
    border-radius: 14px;
    margin-bottom: 16px;
    border: 1px solid #334155;
  }

  .label {
    font-size: 14px;
    color: #9ca3af;
  }

  .reading {
    font-size: 48px;
    font-weight: bold;
    margin: 12px 0;
  }

  .unit {
    font-size: 20px;
    color: #9ca3af;
  }

  .status {
    padding: 10px 12px;
    border-radius: 8px;
    font-weight: bold;
    margin-top: 12px;
    background: #263449;
  }

  .normal { color: #4ade80; }
  .warning { color: #fbbf24; }
  .alarm { color: #f87171; }

  .chart-wrap {
    width: 100%;
    height: 280px;
    position: relative;
  }

  canvas {
    width: 100%;
    height: 100%;
    display: block;
  }

  .controls {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    margin-top: 14px;
  }

  button {
    background: #334155;
    color: white;
    border: 1px solid #475569;
    padding: 12px 16px;
    border-radius: 8px;
    font-size: 14px;
    cursor: pointer;
  }

  button:active { opacity: 0.75; }

  .note {
    font-size: 12px;
    color: #9ca3af;
    line-height: 1.5;
    margin-top: 16px;
  }

  .details {
    display: flex;
    flex-wrap: wrap;
    gap: 20px;
    margin-top: 16px;
  }

  .detail {
    flex: 1;
    min-width: 120px;
  }

  .detail-value {
    margin-top: 6px;
    font-size: 18px;
    font-weight: bold;
  }
</style>
</head>

<body>
<div class="container">

  <h1>AC Voltage Monitor</h1>
  <div class="subtitle">NodeMCU ESP-12E · ZMPT101B · 50 Hz mains</div>

  <div class="card">
    <div class="label">LIVE AC RMS VOLTAGE</div>
    <div class="reading">
      <span id="voltage">--.-</span>
      <span class="unit">V</span>
    </div>

    <div id="status" class="status warning">
      Connecting to sensor...
    </div>

    <div class="details">
      <div class="detail">
        <div class="label">Last successful reading</div>
        <div id="lastUpdate" class="detail-value">Never</div>
      </div>

      <div class="detail">
        <div class="label">Low-voltage limit</div>
        <div class="detail-value">214V</div>
      </div>
    </div>

    <div class="controls">
      <button id="soundButton">Enable alarm sound</button>
      <button id="muteButton">Mute alarm</button>
      <button id="clearButton">Clear history</button>
    </div>

    <div class="note">
      Sound must be enabled by a user gesture in the browser.
      Muting disables sound but does not disable alarm detection.
    </div>
  </div>

  <div class="card">
    <div class="label">VOLTAGE HISTORY</div>
    <div class="chart-wrap">
      <canvas id="chart"></canvas>
    </div>
    <div class="note">
      Rolling 24-hour history. Readings are saved when voltage changes by at least 1V
      or every 60 seconds. Failed requests are recorded as 0V. The red dashed line is 214V.
    </div>
  </div>

  <div class="card">
    <div class="label">CONNECTION</div>
    <div id="connection" class="detail-value warning">
      Waiting for first reading
    </div>
    <div class="note">
      A failed request or voltage below 214V triggers the alarm. When voltage
      returns to 214V or above, the alarm clears and a different 3-second recovery tone plays.
    </div>
  </div>

</div>

<script>
(() => {
  "use strict";

  const LOW_LIMIT = 209;
  const REQUEST_TIMEOUT = 2500;
  const POLL_INTERVAL = 2000;
  const HISTORY_DURATION = 24 * 60 * 60 * 1000;
  const MIN_CHANGE = 1.0;
  const MAX_GAP = 60000;
  const HISTORY_KEY = "acVoltageHistoryV1";

  const voltageEl = document.getElementById("voltage");
  const statusEl = document.getElementById("status");
  const connectionEl = document.getElementById("connection");
  const lastUpdateEl = document.getElementById("lastUpdate");
  const canvas = document.getElementById("chart");
  const ctx = canvas.getContext("2d");

  let history = loadHistory();
  let requestInProgress = false;
  let failureCount = 0;
  let lastSuccessTime = null;
  let lastSavedVoltage = history.length
    ? history[history.length - 1].voltage : null;
  let lastSavedTime = history.length
    ? history[history.length - 1].time : 0;
  let outageDetected = history.length > 0 &&
    history[history.length - 1].voltage < LOW_LIMIT;

  let audioContext = null;
  let alarmEnabled = false;
  let alarmMuted = false;
  let alarmTimer = null;
  let alarmActive = false;
  let alarmReason = "";

  function loadHistory() {
    try {
      const saved = JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]");
      if (!Array.isArray(saved)) return [];
      const cutoff = Date.now() - HISTORY_DURATION;
      return saved.filter(p =>
        p && Number.isFinite(p.time) &&
        Number.isFinite(p.voltage) &&
        p.time >= cutoff && p.voltage >= 0 && p.voltage <= 300
      ).sort((a, b) => a.time - b.time);
    } catch (e) {
      console.warn("Could not load saved voltage history:", e);
      return [];
    }
  }

  function trimHistory() {
    const cutoff = Date.now() - HISTORY_DURATION;
    const oldLength = history.length;
    history = history.filter(p => p.time >= cutoff);
    if (history.length !== oldLength) persistHistory();
  }

  function persistHistory() {
    try {
      localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
    } catch (e) {
      console.warn("Could not persist voltage history:", e);
    }
  }

  function saveReading(timestamp, voltage) {
    history.push({ time: timestamp, voltage: voltage });
    trimHistory();
    lastSavedVoltage = voltage;
    lastSavedTime = timestamp;
    persistHistory();
  }

  function addReading(voltage, force = false) {
    const now = Date.now();
    trimHistory();

    if (lastSavedVoltage === null) {
      saveReading(now, voltage);
      return;
    }

    const changed = Math.abs(voltage - lastSavedVoltage) >= MIN_CHANGE;
    const periodic = now - lastSavedTime >= MAX_GAP;

    if (force || changed || periodic) {
      saveReading(now, voltage);
    }
  }

  // ---------- Audio alarms ----------

  async function enableAudio() {
    try {
      const AudioContextClass =
        window.AudioContext || window.webkitAudioContext;

      if (!AudioContextClass) {
        alert("Web Audio API is not supported by this browser.");
        return;
      }

      if (!audioContext) audioContext = new AudioContextClass();
      if (audioContext.state === "suspended") await audioContext.resume();

      alarmEnabled = true;
      document.getElementById("soundButton").textContent =
        "Alarm sound enabled";
      document.getElementById("soundButton").style.display = "none";

      // Brief test tone confirms that browser audio is enabled.
      beep(880, 0.5);
      if (alarmActive) startAlarm(alarmReason);
    } catch (e) {
      console.error("Audio initialization failed", e);
      alert("Could not enable sound. Check your browser settings.");
    }
  }

  function beep(frequency, duration, waveType = "square") {
    if (!audioContext || !alarmEnabled || alarmMuted) return;
    if (audioContext.state !== "running") return;

    const oscillator = audioContext.createOscillator();
    const gain = audioContext.createGain();
    const start = audioContext.currentTime;

    oscillator.type = waveType;
    oscillator.frequency.value = frequency;

    gain.gain.setValueAtTime(0.0001, start);
    gain.gain.exponentialRampToValueAtTime(0.12, start + 0.02);
    gain.gain.setValueAtTime(0.12, start + duration * 0.65);
    gain.gain.exponentialRampToValueAtTime(0.0001, start + duration);

    oscillator.connect(gain);
    gain.connect(audioContext.destination);
    oscillator.start(start);
    oscillator.stop(start + duration + 0.02);
  }

  function playRecoveryBeep() {
    // A smoother, higher-pitched 3-second tone distinct from the alarm.
    beep(2320, 4.0, "sine");
  }

  function startAlarm(reason) {
    alarmActive = true;
    alarmReason = reason;
    updateStatus();

    if (alarmTimer !== null) return;

    function pulse() {
      if (!alarmActive || !alarmEnabled || alarmMuted) {
        alarmTimer = null;
        return;
      }

      // Read the current reason, so a low-voltage/HTTP change is reflected.
      const frequency = alarmReason === "low" ? 1000 : 620;
      beep(frequency, 0.2);

      setTimeout(() => {
        if (alarmActive && alarmEnabled && !alarmMuted) {
          const nextFrequency = alarmReason === "low" ? 1000 : 620;
          beep(nextFrequency, 0.18);
        }
      }, 260);

      alarmTimer = setTimeout(pulse, 1500);
    }

    pulse();
  }

  function stopAlarm() {
    alarmActive = false;
    alarmReason = "";

    if (alarmTimer !== null) {
      clearTimeout(alarmTimer);
      alarmTimer = null;
    }

    updateStatus();
  }

  function updateAlarm(reason) {
    if (reason) startAlarm(reason);
    else stopAlarm();
  }

  document.getElementById("soundButton")
    .addEventListener("click", enableAudio);

  document.getElementById("muteButton")
    .addEventListener("click", () => {
      alarmMuted = !alarmMuted;
      document.getElementById("muteButton").textContent =
        alarmMuted ? "Unmute alarm" : "Mute alarm";

      if (alarmMuted && alarmTimer !== null) {
        clearTimeout(alarmTimer);
        alarmTimer = null;
      }

      if (!alarmMuted && alarmActive) startAlarm(alarmReason);
    });

  // ---------- Status ----------

  function setStatus(message, type) {
    statusEl.textContent = message;
    statusEl.className = "status " + type;
  }

  function updateStatus() {
    if (failureCount > 0) {
      setStatus("ALARM: HTTP request failed", "alarm");
      connectionEl.textContent =
        "Connection failure (" + failureCount + ")";
      connectionEl.className = "detail-value alarm";
      return;
    }

    if (alarmActive && alarmReason === "low") {
      setStatus("ALARM: Voltage below 214V", "alarm");
      connectionEl.textContent = "Connected · Low voltage";
      connectionEl.className = "detail-value alarm";
      return;
    }

    if (lastSuccessTime !== null) {
      setStatus("Voltage normal", "normal");
      connectionEl.textContent = "Connected";
      connectionEl.className = "detail-value normal";
    }
  }

  // ---------- Fetch voltage ----------

  async function pollVoltage() {
    if (requestInProgress) return;
    requestInProgress = true;

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT);

    try {
      const response = await fetch("/voltage?t=" + Date.now(), {
        method: "GET",
        cache: "no-store",
        signal: controller.signal
      });

      if (!response.ok) throw new Error("HTTP " + response.status);

      const data = await response.json();
      if (typeof data.voltage !== "number" ||
          !Number.isFinite(data.voltage) ||
          data.voltage < 0 || data.voltage > 300) {
        throw new Error("Invalid voltage response");
      }

      const now = Date.now();
      failureCount = 0;
      lastSuccessTime = now;
      voltageEl.textContent = data.voltage.toFixed(1);
      lastUpdateEl.textContent = new Date(now).toLocaleTimeString();

      const isLow = data.voltage < LOW_LIMIT;
      const recovered = outageDetected && !isLow;

      // Force a point at a low-voltage/recovery transition.
      addReading(data.voltage, isLow ? !outageDetected : recovered);

      if (isLow) {
        outageDetected = true;
        updateAlarm("low");
      } else {
        if (recovered) playRecoveryBeep();
        outageDetected = false;
        updateAlarm(null);
      }

      updateStatus();
      drawChart();
    } catch (error) {
      failureCount++;
      console.error("Voltage request failed:", error);

      // Treat each request failure as a zero-volt sample, but suppress
      // repeated identical zeros until the periodic-save interval elapses.
      voltageEl.textContent = "0.0";
      addReading(0);
      outageDetected = true;
      updateAlarm("http");
      updateStatus();
      drawChart();
    } finally {
      clearTimeout(timeout);
      requestInProgress = false;
    }
  }

  // ---------- 24-hour rolling chart ----------

  function drawChart() {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * dpr));
    canvas.height = Math.max(1, Math.round(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const width = rect.width;
    const height = rect.height;
    ctx.clearRect(0, 0, width, height);

    const pad = { left: 46, right: 12, top: 20, bottom: 30 };
    const plotW = width - pad.left - pad.right;
    const plotH = height - pad.top - pad.bottom;
    if (plotW <= 0 || plotH <= 0) return;

    const now = Date.now();
    const startTime = now - HISTORY_DURATION;
    const points = history.filter(p => p.time >= startTime && p.time <= now);

    // Fixed scale keeps zero-volt outages visible on the 24-hour chart.
    const minV = 0;
    const maxV = 260;

    function xTime(t) {
      return pad.left + ((t - startTime) / HISTORY_DURATION) * plotW;
    }

    function y(v) {
      return pad.top + ((maxV - v) / (maxV - minV)) * plotH;
    }

    // Grid and voltage labels.
    ctx.font = "11px Arial";
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";

    for (let v = 0; v <= 250; v += 50) {
      const yy = y(v);
      ctx.strokeStyle = "#334155";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(pad.left, yy);
      ctx.lineTo(width - pad.right, yy);
      ctx.stroke();
      ctx.fillStyle = "#9ca3af";
      ctx.fillText(String(v), pad.left - 8, yy);
    }

    // Low-voltage threshold.
    const limitY = y(LOW_LIMIT);
    ctx.save();
    ctx.strokeStyle = "#f87171";
    ctx.setLineDash([6, 4]);
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(pad.left, limitY);
    ctx.lineTo(width - pad.right, limitY);
    ctx.stroke();
    ctx.restore();

    ctx.fillStyle = "#f87171";
    ctx.textAlign = "left";
    ctx.textBaseline = "bottom";
    ctx.fillText("inverter max 214V", pad.left + 5, limitY - 4);

    // Draw timestamp-scaled history. Break the line across long data gaps.
    if (points.length > 0) {
      ctx.beginPath();
      let previousTime = null;
      for (const point of points) {
        const px = xTime(point.time);
        const py = y(point.voltage);
        if (previousTime === null || point.time - previousTime > 90000) {
          ctx.moveTo(px, py);
        } else {
          ctx.lineTo(px, py);
        }
        previousTime = point.time;
      }

      ctx.strokeStyle = "#38bdf8";
      ctx.lineWidth = 2.5;
      ctx.lineJoin = "round";
      ctx.lineCap = "round";
      ctx.stroke();

      const last = points[points.length - 1];
      ctx.beginPath();
      ctx.arc(xTime(last.time), y(last.voltage), 4, 0, Math.PI * 2);
      ctx.fillStyle = last.voltage < LOW_LIMIT ? "#f87171" : "#38bdf8";
      ctx.fill();
    }

    // Fixed 24-hour axis labels.
    ctx.fillStyle = "#9ca3af";
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    ctx.fillText("24h ago", pad.left, height - pad.bottom + 8);
    ctx.textAlign = "center";
    ctx.fillText("12h ago", pad.left + plotW / 2, height - pad.bottom + 8);
    ctx.textAlign = "right";
    ctx.fillText("Now", width - pad.right, height - pad.bottom + 8);
  }

  document.getElementById("clearButton")
    .addEventListener("click", () => {
      history = [];
      lastSavedVoltage = null;
      lastSavedTime = 0;
      persistHistory();
      drawChart();
    });

  window.addEventListener("resize", drawChart);

  // Start with stored history, then poll every two seconds.
  trimHistory();
  drawChart();
  pollVoltage();
  setInterval(pollVoltage, POLL_INTERVAL);
})();
</script>
</body>
</html>
)rawliteral";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

void handleVoltage() {
  String json = "{";
  json += "\"voltage\":";
  json += String(voltageRMS, 2);
  json += ",\"unit\":\"V\"";
  json += ",\"type\":\"AC_RMS\"";
  json += "}";

  server.send(200, "application/json", json);

  digitalWrite(WEB_REQ_LED, HIGH);
  delay(200);
  digitalWrite(WEB_REQ_LED, LOW);
}

void handleRawVoltage() {
  server.send(200, "text/plain", String(voltageRMS, 2));

  digitalWrite(WEB_REQ_LED, HIGH);
  delay(200);
  digitalWrite(WEB_REQ_LED, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(WEB_REQ_LED, OUTPUT);

  delay(3000);
  digitalWrite(WIFI_LED, LOW); // led on my board is active low


  Serial.println();
  Serial.println("AC Voltage Monitor");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("AC_Voltage_sen");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < 20000) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(WIFI_LED, HIGH);
  } else {
    Serial.println("Wi-Fi connection failed.");
    digitalWrite(WIFI_LED, LOW);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/voltage", HTTP_GET, handleVoltage);
  server.on("/v", HTTP_GET, handleRawVoltage);

  server.enableCORS(true);
  ElegantOTA.begin(&server);
  server.begin();
  Serial.println("HTTP server started");
}

// ---------------- Main loop ----------------

void loop() {
  /*
    To calibrate ZMPT101B uncomment following lines and comment remaining lines in the method.
    Deploy and see the output on Serial Plotter. The sine wave needs to be clean.
  */
  //Serial.println(analogRead(A0));
  //delayMicroseconds(500);

  server.handleClient();
  ElegantOTA.loop();
  if((millis() - lastUpdate) > UPDATE_INTERVAL_MS) {
    lastUpdate = millis();
    voltageRMS = readVoltageRMS();
    //Serial.print("Voltage:");
    //Serial.println(voltageRMS);
  }

  if(WiFi.isConnected()) {
    digitalWrite(WIFI_LED, HIGH);
  } else {
    digitalWrite(WIFI_LED, LOW);
  }

  yield();
}