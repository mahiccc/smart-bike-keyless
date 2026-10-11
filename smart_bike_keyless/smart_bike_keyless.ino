/*
 * ============================================================
 *  SMART BIKE KEYLESS IGNITION SYSTEM — Wi-Fi MAC Auth & Captive Portal
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "secrets.h"

// ─────────────────────── PIN DEFINITIONS ───────────────────────
#define RELAY_IGNITION    16
#define RELAY_STARTER     5
#define BUZZER_PIN        4
#define LED_GREEN         18
#define LED_RED           19
#define VIBRATION_SENSOR  21
#define VIBRATION_GPIO    GPIO_NUM_21

// ─────────────────────── CONFIGURATION ─────────────────────────
const char* AP_SSID = "SmartBike-Keyless";
String apPassword = SECRET_WIFI_PASS;
String portalPassword = DEFAULT_PORTAL_PASS;
const byte DNS_PORT = 53;
#define AUTO_LOCK_TIMEOUT 300000 // 5 minutes inactivity

enum BikeState {
  STATE_LOCKED,
  STATE_UNLOCKED,
  STATE_ENGINE_ON
};

BikeState currentState = STATE_LOCKED;
unsigned long lastActivityTime = 0;
bool deepSleepEnabled = false;
bool autoUnlockEnabled = true;
int vibSensitivity = 1; // 1=High, 2=Medium, 3=Low
String whitelistMacs = ""; 
String lastConnectedStaMac = "None";

// ─────────────────────── AUDIT LOGS ──────────────────────────
#define MAX_LOGS 20
String auditLogs[MAX_LOGS];
int logIndex = 0;

void addLog(String msg) {
  unsigned long t = millis() / 1000;
  String timeStr = "[" + String(t) + "s] ";
  auditLogs[logIndex] = timeStr + msg;
  logIndex = (logIndex + 1) % MAX_LOGS;
  Serial.println("AUDIT: " + msg);
}

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

// ─────────────────────── HARDWARE CONTROL ──────────────────────
void beepAuthSuccess() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 2400, 90);
    delay(130);
  }
  noTone(BUZZER_PIN);
}

void beepUnlock() {
  for (int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, 2000, 100);
    delay(150);
  }
  noTone(BUZZER_PIN);
}

void beepLock() {
  tone(BUZZER_PIN, 800, 300);
  delay(350);
  noTone(BUZZER_PIN);
}

void beepLocate() {
  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, 2500, 200);
    delay(300);
  }
  noTone(BUZZER_PIN);
}

void beepStart() {
  for (int f = 500; f <= 2500; f += 200) {
    tone(BUZZER_PIN, f, 50);
    delay(60);
  }
  noTone(BUZZER_PIN);
}

void updateLEDs() {
  if (currentState == STATE_LOCKED) {
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, LOW);
  } else {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_GREEN, HIGH);
  }
}

void ignitionOn()  { 
  digitalWrite(RELAY_IGNITION, HIGH); 
}

void ignitionOff() { 
  digitalWrite(RELAY_IGNITION, LOW); 
}

void startEngine() {
  beepStart();
  ignitionOn();
  delay(500);
  digitalWrite(RELAY_STARTER, HIGH);
  delay(2000);
  digitalWrite(RELAY_STARTER, LOW);
}

void stopEngine() {
  ignitionOff();
  digitalWrite(RELAY_STARTER, LOW);
}

void goToDeepSleep() {
  addLog("Entering Deep Sleep...");
  Serial.println("Entering Deep Sleep... Wake on vibration (GPIO21)");
  ignitionOff();
  updateLEDs();
  
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_ext0_wakeup(VIBRATION_GPIO, 1);
  delay(100);
  esp_deep_sleep_start();
}

bool checkVibrationSensitivity() {
  if (vibSensitivity == 1) return true; // High sensitivity
  
  int pulses = 0;
  int targetPulses = (vibSensitivity == 2) ? 3 : 7;
  int lastState = digitalRead(VIBRATION_SENSOR);
  unsigned long startT = millis();
  
  while (millis() - startT < 400) {
    int state = digitalRead(VIBRATION_SENSOR);
    if (state != lastState && state == HIGH) pulses++;
    lastState = state;
    delay(2);
  }
  
  return (pulses >= targetPulses);
}

// ─────────────────────── WIFI EVENT LISTENER ───────────────────
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
             info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
             info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
    String mac = String(macStr);
    mac.toUpperCase();
    lastConnectedStaMac = mac;
    
    addLog("Phone Connected (Wi-Fi): " + mac);
    
    // Check if MAC is whitelisted
    if (autoUnlockEnabled && whitelistMacs.length() > 0 && whitelistMacs.indexOf(mac) >= 0) {
      addLog("WHITELIST MATCH: " + mac + " -> Ignition ON");
      beepAuthSuccess();
      currentState = STATE_UNLOCKED;
      ignitionOn();
      updateLEDs();
      lastActivityTime = millis();
    } else {
      addLog("Wi-Fi connected device NOT in whitelist: " + mac);
    }
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
             info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
             info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
    addLog("Phone Disconnected: " + String(macStr));
  }
}

// ─────────────────────── WEB SERVER PAGES ──────────────────────
const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Bike Portal</title>
  <style>
    :root {
      --bg: #121418;
      --card-bg: #1e222b;
      --accent-green: #00e676;
      --accent-red: #ff3366;
      --accent-blue: #2979ff;
      --accent-yellow: #ffab00;
      --text: #f0f4f8;
      --text-dim: #9aa5b5;
    }
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      text-align: center;
      margin: 0;
      padding: 16px;
    }
    .header { margin-bottom: 20px; }
    .header h2 { margin: 6px 0; font-size: 24px; }
    .btn {
      display: block;
      width: 100%;
      padding: 14px;
      margin: 10px 0;
      font-size: 16px;
      border: none;
      border-radius: 10px;
      cursor: pointer;
      font-weight: 700;
      transition: opacity 0.2s, transform 0.1s;
    }
    .btn:active { transform: scale(0.98); opacity: 0.9; }
    .btn-green { background: var(--accent-green); color: #000; }
    .btn-red { background: var(--accent-red); color: #fff; }
    .btn-blue { background: var(--accent-blue); color: #fff; }
    .btn-yellow { background: var(--accent-yellow); color: #000; }
    .btn-outline { background: transparent; border: 1px solid #485465; color: var(--text); }
    .card {
      background: var(--card-bg);
      padding: 18px;
      border-radius: 14px;
      margin-bottom: 18px;
      box-shadow: 0 4px 14px rgba(0,0,0,0.3);
      text-align: left;
    }
    .card h3 { margin-top: 0; margin-bottom: 14px; font-size: 18px; text-align: center; }
    input[type=text], input[type=password], select {
      width: 100%;
      padding: 12px;
      margin-bottom: 12px;
      border-radius: 8px;
      border: 1px solid #363e4f;
      background: #15181e;
      color: #fff;
      font-size: 15px;
    }
    .badge {
      display: inline-block;
      padding: 6px 12px;
      border-radius: 6px;
      font-size: 13px;
      font-weight: bold;
    }
    .badge-green { background: var(--accent-green); color: #000; }
    .badge-red { background: var(--accent-red); color: #fff; }
    .badge-yellow { background: var(--accent-yellow); color: #000; }
    .badge-gray { background: #3a4150; color: #fff; }
    .log-box {
      background: #101216;
      padding: 12px;
      border-radius: 8px;
      font-family: monospace;
      font-size: 12px;
      height: 140px;
      overflow-y: auto;
      white-space: pre-wrap;
      border: 1px solid #242a36;
      color: #c9d4e5;
    }
    .modal-overlay {
      position: fixed;
      top: 0; left: 0; width: 100%; height: 100%;
      background: rgba(0,0,0,0.85);
      backdrop-filter: blur(4px);
      z-index: 10000;
      display: none;
      justify-content: center;
      align-items: center;
      padding: 20px;
    }
    .modal {
      background: var(--card-bg);
      border-radius: 16px;
      padding: 24px;
      width: 100%;
      max-width: 360px;
      box-shadow: 0 8px 30px rgba(0,0,0,0.5);
      text-align: center;
    }
    .mac-item {
      background: #171b22;
      padding: 10px 14px;
      margin-bottom: 8px;
      border-radius: 8px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-family: monospace;
    }
  </style>
</head>
<body>

  <!-- LOADING OVERLAY -->
  <div id="loadingOverlay" class="modal-overlay">
    <div style="color:white; font-size:18px; font-weight:bold;">Processing...</div>
  </div>

  <!-- LOGIN MODAL -->
  <div id="loginModal" class="modal-overlay" style="display:flex;">
    <div class="modal">
      <div style="font-size:36px; margin-bottom:10px;">🔐</div>
      <h3 style="margin:0 0 10px 0;">Portal Locked</h3>
      <p style="color:var(--text-dim); font-size:14px; margin-bottom:16px;">Enter your Portal Password to access controls</p>
      <input type="password" id="loginPassInput" placeholder="Enter password (default: 1234)">
      <div id="loginError" style="color:var(--accent-red); font-size:13px; margin-bottom:10px; display:none;">Incorrect password!</div>
      <button class="btn btn-green" onclick="submitLogin()">Unlock Controls</button>
    </div>
  </div>

  <div class="header">
    <h2>🏍️ Smart Bike Keyless</h2>
    <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px;">
      <span id="statusBadge" class="badge badge-gray">Loading...</span>
      <button class="btn btn-outline" style="width:auto; padding:6px 14px; margin:0; font-size:12px;" onclick="logout()">Lock Portal</button>
    </div>
  </div>

  <!-- STATUS CARD -->
  <div class="card">
    <h3>System Status</h3>
    <div style="font-size:14px; line-height:1.6; color:var(--text-dim);">
      <div>Connected Phone MAC: <span id="currentMac" style="font-family:monospace; color:#fff; font-weight:bold;">--</span></div>
      <div>Auto-Unlock Status: <span id="autoAuthStatus" style="font-weight:bold; color:var(--accent-green);">--</span></div>
    </div>
  </div>

  <!-- CONTROLS -->
  <div class="card">
    <h3>Vehicle Controls</h3>
    <button class="btn btn-green" onclick="sendCmd('ign_on')">🔓 Ignition ON</button>
    <button class="btn btn-yellow" onclick="sendCmd('start')">⚡ Self Start Motor</button>
    <button class="btn btn-red" onclick="sendCmd('ign_off')">🔒 Ignition OFF</button>
    <button class="btn btn-blue" onclick="sendCmd('locate')">🔔 Locate Bike (Buzzer)</button>
  </div>

  <!-- WI-FI WHITELIST MANAGEMENT -->
  <div class="card">
    <h3>Wi-Fi Phone Whitelist</h3>
    <p style="font-size:13px; color:var(--text-dim); margin-top:0;">
      When your whitelisted phone connects to this Wi-Fi network, the bike alerts with beeps and turns ignition ON automatically!
    </p>

    <div style="background:#15181e; padding:12px; border-radius:8px; margin-bottom:12px;">
      <div style="font-size:12px; color:var(--text-dim); margin-bottom:4px;">Currently Connected Phone:</div>
      <div id="detectedMacDisplay" style="font-family:monospace; font-size:15px; font-weight:bold; color:var(--accent-green);">None</div>
      <button class="btn btn-blue" id="btnWhitelistCurrent" style="margin-top:10px; padding:10px;" onclick="whitelistCurrentPhone()">+ Whitelist This Connected Phone</button>
    </div>

    <div style="font-size:14px; font-weight:bold; margin-bottom:8px;">Saved Whitelisted MACs:</div>
    <div id="macList"></div>

    <div style="display:flex; gap:8px; margin-top:10px;">
      <input type="text" id="manualMacInput" placeholder="AA:BB:CC:DD:EE:FF" style="margin-bottom:0;">
      <button class="btn btn-green" style="width:auto; padding:10px 16px; margin:0;" onclick="addManualMac()">Add</button>
    </div>
    <button class="btn btn-outline" style="margin-top:10px; font-size:13px; padding:8px;" onclick="sendCmd('clear_macs')">Clear Whitelist</button>
  </div>

  <!-- SETTINGS -->
  <div class="card">
    <h3>Configuration</h3>
    <label style="display:flex; align-items:center; gap:10px; cursor:pointer; margin-bottom:14px;">
      <input type="checkbox" id="autoUnlockToggle" onchange="toggleAutoUnlock()" style="width:20px; height:20px;">
      <span>Auto-Unlock when Whitelisted Phone connects</span>
    </label>

    <label style="display:flex; align-items:center; gap:10px; cursor:pointer; margin-bottom:14px;">
      <input type="checkbox" id="dsToggle" onchange="toggleDS()" style="width:20px; height:20px;">
      <span>Enable Deep Sleep (Wake on Vibration)</span>
    </label>

    <label style="font-size:13px; color:var(--text-dim); display:block; margin-bottom:4px;">Vibration Wake Sensitivity:</label>
    <select id="vibSelect" onchange="updateSensSettings()">
      <option value="1">High (Light touch wakes up)</option>
      <option value="2">Medium (Moderate bump)</option>
      <option value="3">Low (Firm hit)</option>
    </select>

    <hr style="border:none; border-top:1px solid #363e4f; margin:16px 0;">

    <h4>Change Portal Password</h4>
    <input type="password" id="newPortalPass" placeholder="Enter new portal password (min 4 chars)">
    <button class="btn btn-blue" style="margin-top:0;" onclick="changePortalPassword()">Save New Password</button>
  </div>

  <!-- AUDIT LOGS -->
  <div class="card">
    <h3>Activity Logs</h3>
    <div id="logBox" class="log-box">Loading activity...</div>
    <button class="btn btn-outline" style="margin-top:10px; font-size:12px; padding:6px;" onclick="fetchLogs()">Refresh Logs</button>
  </div>

  <script>
    let authToken = localStorage.getItem('bike_portal_auth') || '';

    function checkAuthUI() {
      if (!authToken) {
        document.getElementById('loginModal').style.display = 'flex';
      } else {
        document.getElementById('loginModal').style.display = 'none';
      }
    }

    async function submitLogin() {
      const pass = document.getElementById('loginPassInput').value.trim();
      if (!pass) return;
      showLoading(true);
      try {
        const res = await fetch('/login?p=' + encodeURIComponent(pass));
        const data = await res.json();
        if (data.auth) {
          authToken = pass;
          localStorage.setItem('bike_portal_auth', authToken);
          document.getElementById('loginError').style.display = 'none';
          document.getElementById('loginModal').style.display = 'none';
          fetchStatus();
          fetchLogs();
        } else {
          document.getElementById('loginError').style.display = 'block';
        }
      } catch (e) {
        alert("Failed to communicate with device");
      }
      showLoading(false);
    }

    function logout() {
      authToken = '';
      localStorage.removeItem('bike_portal_auth');
      document.getElementById('loginPassInput').value = '';
      document.getElementById('loginModal').style.display = 'flex';
    }

    async function apiReq(url) {
      if (!authToken) {
        checkAuthUI();
        return null;
      }
      showLoading(true);
      try {
        const sep = url.includes('?') ? '&' : '?';
        const res = await fetch(url + sep + 'token=' + encodeURIComponent(authToken));
        if (res.status === 401) {
          logout();
          showLoading(false);
          return null;
        }
        showLoading(false);
        return res;
      } catch (e) {
        showLoading(false);
        return null;
      }
    }

    function showLoading(show) {
      document.getElementById('loadingOverlay').style.display = show ? 'flex' : 'none';
    }

    async function fetchStatus() {
      if (!authToken) return;
      try {
        const res = await fetch('/status?token=' + encodeURIComponent(authToken));
        if (res.status === 401) { logout(); return; }
        const data = await res.json();

        // Status badge
        const badge = document.getElementById('statusBadge');
        if (data.state === 'locked') {
          badge.className = 'badge badge-red';
          badge.innerText = '🔒 LOCKED';
        } else if (data.state === 'unlocked') {
          badge.className = 'badge badge-green';
          badge.innerText = '🔓 UNLOCKED';
        } else {
          badge.className = 'badge badge-yellow';
          badge.innerText = '⚡ ENGINE RUNNING';
        }

        document.getElementById('currentMac').innerText = data.last_mac || 'None';
        document.getElementById('detectedMacDisplay').innerText = data.last_mac || 'None';

        let macsArray = data.macs ? data.macs.split(',').filter(m => m.trim().length > 0) : [];
        let isWhitelisted = data.last_mac && macsArray.includes(data.last_mac);
        document.getElementById('autoAuthStatus').innerText = isWhitelisted ? 'Authenticated & Whitelisted' : 'Not Whitelisted';
        document.getElementById('autoAuthStatus').style.color = isWhitelisted ? 'var(--accent-green)' : 'var(--accent-red)';

        document.getElementById('dsToggle').checked = data.ds;
        document.getElementById('autoUnlockToggle').checked = data.auto_unlock;
        document.getElementById('vibSelect').value = data.vib_sens;

        // Render MACs
        let macHtml = '';
        if (macsArray.length === 0) {
          macHtml = '<div style="color:var(--text-dim); font-size:13px; padding:6px 0;">No MACs whitelisted yet</div>';
        } else {
          macsArray.forEach(mac => {
            macHtml += `<div class="mac-item">
              <span>${mac}</span>
              <button class="btn btn-red" style="width:auto; padding:6px 12px; margin:0; font-size:12px;" onclick="deleteMac('${mac}')">Remove</button>
            </div>`;
          });
        }
        document.getElementById('macList').innerHTML = macHtml;

      } catch (e) {}
    }

    async function fetchLogs() {
      if (!authToken) return;
      try {
        const res = await fetch('/logs?token=' + encodeURIComponent(authToken));
        if (res.status === 200) {
          const txt = await res.text();
          document.getElementById('logBox').innerText = txt;
        }
      } catch (e) {}
    }

    async function sendCmd(cmd) {
      await apiReq('/cmd?a=' + cmd);
      await fetchStatus();
      await fetchLogs();
    }

    async function toggleDS() {
      const v = document.getElementById('dsToggle').checked ? 1 : 0;
      await apiReq('/set_ds?v=' + v);
      await fetchStatus();
    }

    async function toggleAutoUnlock() {
      const v = document.getElementById('autoUnlockToggle').checked ? 1 : 0;
      await apiReq('/set_auto?v=' + v);
      await fetchStatus();
    }

    async function updateSensSettings() {
      const v = document.getElementById('vibSelect').value;
      await apiReq('/set_sens?v=' + v);
      await fetchStatus();
    }

    async function whitelistCurrentPhone() {
      const mac = document.getElementById('detectedMacDisplay').innerText.trim();
      if (!mac || mac === 'None') {
        alert("No connected device detected yet!");
        return;
      }
      await addMacToList(mac);
    }

    async function addManualMac() {
      const mac = document.getElementById('manualMacInput').value.trim().toUpperCase();
      if (!mac || mac.length < 11) {
        alert("Please enter a valid MAC address");
        return;
      }
      await addMacToList(mac);
      document.getElementById('manualMacInput').value = '';
    }

    async function addMacToList(newMac) {
      const res = await fetch('/status?token=' + encodeURIComponent(authToken));
      const data = await res.json();
      let macsArray = data.macs ? data.macs.split(',').filter(m => m.trim().length > 0) : [];
      if (!macsArray.includes(newMac)) {
        macsArray.push(newMac);
        await apiReq('/set_mac?v=' + encodeURIComponent(macsArray.join(',')));
        await fetchStatus();
        await fetchLogs();
      } else {
        alert("This MAC is already whitelisted!");
      }
    }

    async function deleteMac(macToRemove) {
      const res = await fetch('/status?token=' + encodeURIComponent(authToken));
      const data = await res.json();
      let macsArray = data.macs ? data.macs.split(',').filter(m => m.trim().length > 0) : [];
      macsArray = macsArray.filter(m => m !== macToRemove);
      await apiReq('/set_mac?v=' + encodeURIComponent(macsArray.join(',')));
      await fetchStatus();
      await fetchLogs();
    }

    async function changePortalPassword() {
      const newPass = document.getElementById('newPortalPass').value.trim();
      if (newPass.length < 4) {
        alert("Password must be at least 4 characters!");
        return;
      }
      const res = await apiReq('/set_portal_pass?new_pass=' + encodeURIComponent(newPass));
      if (res && res.status === 200) {
        authToken = newPass;
        localStorage.setItem('bike_portal_auth', newPass);
        document.getElementById('newPortalPass').value = '';
        alert("Portal password updated successfully!");
        fetchLogs();
      } else {
        alert("Failed to update password");
      }
    }

    checkAuthUI();
    if (authToken) {
      fetchStatus();
      fetchLogs();
    }
    setInterval(() => {
      if (authToken) {
        fetchStatus();
        fetchLogs();
      }
    }, 3000);
  </script>
</body>
</html>
)rawliteral";

// ─────────────────────── AUTH HELPER ───────────────────────────
bool isAuthorized() {
  if (server.hasHeader("X-Portal-Auth") && server.header("X-Portal-Auth") == portalPassword) return true;
  if (server.hasArg("token") && server.arg("token") == portalPassword) return true;
  if (server.hasArg("p") && server.arg("p") == portalPassword) return true;
  return false;
}

// ─────────────────────── API HANDLERS ──────────────────────────
void handleRoot() { 
  server.send(200, "text/html", MAIN_PAGE); 
}

void handleLogin() {
  String p = server.arg("p");
  if (p == portalPassword) {
    addLog("Portal login successful");
    server.send(200, "application/json", "{\"auth\":true}");
  } else {
    addLog("Failed portal login attempt");
    server.send(401, "application/json", "{\"auth\":false,\"msg\":\"Invalid password\"}");
  }
}

void handleStatus() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  String stateStr = (currentState == STATE_LOCKED) ? "locked" : 
                    (currentState == STATE_UNLOCKED) ? "unlocked" : "engine_on";
  String json = "{\"state\":\"" + stateStr + 
                "\", \"ds\":" + (deepSleepEnabled ? "true" : "false") + 
                ", \"auto_unlock\":" + (autoUnlockEnabled ? "true" : "false") +
                ", \"vib_sens\":" + String(vibSensitivity) +
                ", \"last_mac\":\"" + lastConnectedStaMac + "\"" +
                ", \"macs\":\"" + whitelistMacs + "\"}";
  server.send(200, "application/json", json);
}

void handleLogs() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  String out = "";
  for (int i = 0; i < MAX_LOGS; i++) {
    int idx = (logIndex + i) % MAX_LOGS;
    if (auditLogs[idx].length() > 0) {
      out += auditLogs[idx] + "\n";
    }
  }
  if (out == "") out = "No logs yet.";
  server.send(200, "text/plain", out);
}

void handleCmd() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  String action = server.arg("a");
  if (action == "ign_on") {
    addLog("Ignition ON via Portal");
    currentState = STATE_UNLOCKED;
    ignitionOn();
    beepUnlock();
  } else if (action == "start") {
    addLog("Engine Started via Portal");
    currentState = STATE_ENGINE_ON;
    startEngine();
  } else if (action == "ign_off") {
    addLog("Ignition OFF via Portal");
    currentState = STATE_LOCKED;
    stopEngine();
    beepLock();
  } else if (action == "locate") {
    addLog("Locate Bike Triggered");
    beepLocate();
  } else if (action == "clear_macs") {
    whitelistMacs = "";
    preferences.begin("bike", false);
    preferences.putString("macs", whitelistMacs);
    preferences.end();
    addLog("Cleared all whitelisted MACs!");
  }
  updateLEDs();
  lastActivityTime = millis();
  server.send(200, "text/plain", "OK");
}

void handleSetDS() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  deepSleepEnabled = (server.arg("v") == "1");
  preferences.begin("bike", false);
  preferences.putBool("ds", deepSleepEnabled);
  preferences.end();
  addLog("Deep Sleep toggled: " + String(deepSleepEnabled));
  server.send(200, "text/plain", "OK");
}

void handleSetAutoUnlock() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  autoUnlockEnabled = (server.arg("v") == "1");
  preferences.begin("bike", false);
  preferences.putBool("auto_unlock", autoUnlockEnabled);
  preferences.end();
  addLog("Auto-Unlock toggled: " + String(autoUnlockEnabled));
  server.send(200, "text/plain", "OK");
}

void handleSetSens() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  vibSensitivity = server.arg("v").toInt();
  if (vibSensitivity < 1 || vibSensitivity > 3) vibSensitivity = 1;
  preferences.begin("bike", false);
  preferences.putInt("vib", vibSensitivity);
  preferences.end();
  addLog("Sensitivity updated: " + String(vibSensitivity));
  server.send(200, "text/plain", "OK");
}

void handleSetMac() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  whitelistMacs = server.arg("v");
  whitelistMacs.toUpperCase();
  preferences.begin("bike", false);
  preferences.putString("macs", whitelistMacs);
  preferences.end();
  addLog("MAC whitelist updated");
  server.send(200, "text/plain", "OK");
}

void handleSetPortalPass() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String newPass = server.arg("new_pass");
  newPass.trim();
  if (newPass.length() < 4) {
    server.send(400, "text/plain", "Password must be at least 4 characters");
    return;
  }
  portalPassword = newPass;
  preferences.begin("bike", false);
  preferences.putString("portal_pass", portalPassword);
  preferences.end();
  addLog("Portal password changed");
  server.send(200, "text/plain", "OK");
}

void handleRedirect() {
  server.sendHeader("Location", String("http://") + server.client().localIP().toString(), true);
  server.send(302, "text/plain", "");
}

// ─────────────────────── SETUP ─────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(RELAY_IGNITION, OUTPUT);
  pinMode(RELAY_STARTER, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(VIBRATION_SENSOR, INPUT);
  
  digitalWrite(RELAY_IGNITION, LOW);
  digitalWrite(RELAY_STARTER, LOW);
  
  preferences.begin("bike", true);
  deepSleepEnabled = preferences.getBool("ds", false);
  autoUnlockEnabled = preferences.getBool("auto_unlock", true);
  vibSensitivity = preferences.getInt("vib", 1);
  whitelistMacs = preferences.getString("macs", "");
  portalPassword = preferences.getString("portal_pass", DEFAULT_PORTAL_PASS);
  preferences.end();
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  // Setup WiFi Access Point & Events
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, apPassword.c_str()); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.on("/set_ds", handleSetDS);
  server.on("/set_auto", handleSetAutoUnlock);
  server.on("/set_sens", handleSetSens);
  server.on("/set_mac", handleSetMac);
  server.on("/set_portal_pass", handleSetPortalPass);
  server.onNotFound(handleRedirect);
  
  server.begin();
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    addLog("Woken up by Vibration");
    if (checkVibrationSensitivity()) {
      addLog("Vibration verified. Waiting for Wi-Fi device...");
      currentState = STATE_LOCKED;
      updateLEDs();
    } else {
      addLog("False vibration bump. Sleeping.");
      goToDeepSleep();
    }
  } else {
    addLog("Normal Boot / Reset");
    currentState = STATE_LOCKED;
    updateLEDs();
    beepLock();
  }
  
  lastActivityTime = millis();
}

// ─────────────────────── MAIN LOOP ─────────────────────────────
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  
  // Auto-lock and Deep Sleep timeout
  if (millis() - lastActivityTime > AUTO_LOCK_TIMEOUT) {
    if (currentState != STATE_LOCKED) {
      Serial.println("[AUTO] Inactivity timeout, locking...");
      currentState = STATE_LOCKED;
      stopEngine();
      beepLock();
      updateLEDs();
    }
    
    if (deepSleepEnabled && currentState == STATE_LOCKED) {
      goToDeepSleep();
    }
    lastActivityTime = millis(); 
  }
  
  delay(10);
}
