/*
 * ============================================================
 *  SMART BIKE KEYLESS IGNITION SYSTEM — Advanced Captive Portal + BLE Pairing
 *  Board: Seeed Studio XIAO ESP32S3
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <BLEAdvertisedDevice.h>

// ─────────────────────── PIN DEFINITIONS ───────────────────────
#define RELAY_IGNITION    D0
#define RELAY_STARTER     D1
#define BUZZER_PIN        D2
#define LED_GREEN         D3
#define LED_RED           D4
#define VIBRATION_SENSOR  D5
#define VIBRATION_GPIO    GPIO_NUM_6  // D5 on XIAO ESP32S3 is GPIO6

// ─────────────────────── CONFIG ────────────────────────────────
const char* AP_SSID = "SmartBike-Keyless";
const char* AP_PASS = "kkl12345";
const byte DNS_PORT = 53;
#define AUTO_LOCK_TIMEOUT 300000 // 5 minutes

enum BikeState {
  STATE_LOCKED,
  STATE_UNLOCKED,
  STATE_ENGINE_ON
};

BikeState currentState = STATE_LOCKED;
unsigned long lastActivityTime = 0;
bool deepSleepEnabled = false;
bool autoUnlockEnabled = false;
int vibSensitivity = 1; // 1=High, 2=Medium, 3=Low
int btRange = 2; // 1=High (Near), 2=Medium, 3=Low (Far)
String whitelistMacs = ""; 
int scanTime = 4; // seconds

// ─────────────────────── AUDIT LOGS ──────────────────────────
#define MAX_LOGS 15
String auditLogs[MAX_LOGS];
int logIndex = 0;
bool btNearby = false;
unsigned long lastBtCheck = 0;

void addLog(String msg) {
  unsigned long t = millis() / 1000;
  String timeStr = "[" + String(t) + "s] ";
  auditLogs[logIndex] = timeStr + msg;
  logIndex = (logIndex + 1) % MAX_LOGS;
  Serial.println("AUDIT: " + msg);
}

bool pairingMode = false;
uint32_t pairingPIN = 123456;

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
BLEServer* pServer = NULL;

// ─────────────────────── HARDWARE CONTROL ──────────────────────
void beepUnlock() {
  for(int i=0;i<2;i++){tone(BUZZER_PIN,2000,100);delay(150);}
  noTone(BUZZER_PIN);
}
void beepLock() {
  tone(BUZZER_PIN,800,300);delay(350);noTone(BUZZER_PIN);
}
void beepLocate() {
  for(int i=0;i<5;i++){tone(BUZZER_PIN,2500,200);delay(300);}
  noTone(BUZZER_PIN);
}
void beepStart() {
  for(int f=500;f<=2500;f+=200){tone(BUZZER_PIN,f,50);delay(60);}
  noTone(BUZZER_PIN);
}
void updateLEDs() {
  if (currentState == STATE_LOCKED) {
    digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW);
  } else {
    digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, HIGH);
  }
}

void ignitionOn()  { digitalWrite(RELAY_IGNITION, HIGH); }
void ignitionOff() { digitalWrite(RELAY_IGNITION, LOW); }
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

// ─────────────────────── BLE SCANNING & PAIRING ────────────────
bool foundAuthorizedDevice = false;

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      String deviceMac = advertisedDevice.getAddress().toString().c_str();
      deviceMac.toUpperCase();
      int rssi = advertisedDevice.getRSSI();
      
      if (whitelistMacs.length() > 0 && whitelistMacs.indexOf(deviceMac) >= 0) {
        // Check RSSI based on user setting
        bool rssiOk = false;
        if (btRange == 1 && rssi >= -60) rssiOk = true; // High (Near)
        else if (btRange == 2 && rssi >= -75) rssiOk = true; // Medium
        else if (btRange == 3 && rssi >= -95) rssiOk = true; // Low (Far)
        
        if (rssiOk) {
          Serial.println("Authorized device found near: " + deviceMac + " (RSSI: " + String(rssi) + ")");
          foundAuthorizedDevice = true;
          btNearby = true;
        } else {
          Serial.println("Device " + deviceMac + " found but too far (RSSI: " + String(rssi) + ")");
        }
      }
    }
};

bool scanForAuthorizedBluetooth() {
  Serial.println("Scanning for whitelisted BLE devices...");
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); 
  foundAuthorizedDevice = false;
  pBLEScan->start(scanTime, false);
  pBLEScan->clearResults(); 
  BLEDevice::deinit(false);
  return foundAuthorizedDevice;
}

class MyServerCallbacks: public BLEServerCallbacks {
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);
        String mac = String(macStr);
        mac.toUpperCase();
        
        if(whitelistMacs.indexOf(mac) < 0) {
            if(whitelistMacs.length() > 0 && !whitelistMacs.endsWith(",")) whitelistMacs += ",";
            whitelistMacs += mac;
            preferences.begin("bike", false);
            preferences.putString("macs", whitelistMacs);
            preferences.end();
            addLog("New MAC Whitelisted: " + mac);
        }
        BLEDevice::getAdvertising()->stop();
        pairingMode = false;
        beepUnlock();
    }
#elif defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer* pServer, ble_gap_conn_desc *desc) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 desc->peer_id_addr.val[5], desc->peer_id_addr.val[4], desc->peer_id_addr.val[3],
                 desc->peer_id_addr.val[2], desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
        String mac = String(macStr);
        mac.toUpperCase();
        
        if(whitelistMacs.indexOf(mac) < 0) {
            if(whitelistMacs.length() > 0 && !whitelistMacs.endsWith(",")) whitelistMacs += ",";
            whitelistMacs += mac;
            preferences.begin("bike", false);
            preferences.putString("macs", whitelistMacs);
            preferences.end();
            addLog("New MAC Whitelisted: " + mac);
        }
        BLEDevice::getAdvertising()->stop();
        pairingMode = false;
        beepUnlock();
    }
#endif
    void onDisconnect(BLEServer* pServer) {}
};

class MySecurity : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest(){ return pairingPIN; }
  void onPassKeyNotify(uint32_t pass_key){}
  bool onConfirmPIN(uint32_t pass_key){ return true; }
  bool onSecurityRequest(){ return true; }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl){
    // MAC is already captured in onConnect
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *desc){
    // MAC is already captured in onConnect
  }
#endif
};

void enableBluetoothPairing() {
  if (pairingMode) return;
  Serial.println("Starting BLE Pairing Mode...");
  
  // Generate random 6-digit PIN
  pairingPIN = random(100000, 999999);
  addLog("BLE Pairing started. PIN: " + String(pairingPIN));
  
  BLEDevice::init("Bike_Pair");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEDevice::setSecurityCallbacks(new MySecurity());
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  
  BLEService *pService = pServer->createService("180A");
  BLECharacteristic *pChar = pService->createCharacteristic("2A29", BLECharacteristic::PROPERTY_READ);
  // Require encryption to force pairing when phone tries to connect
  pChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_READ_ENC_MITM);
  pChar->setValue("Smart Bike Keyless");
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID("180A");
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  
  pairingMode = true;
}

void goToDeepSleep() {
  addLog("Entering Deep Sleep...");
  Serial.println("Entering Deep Sleep... Wake on vibration (D5/GPIO6)");
  ignitionOff();
  updateLEDs();
  
  // Turn off WiFi and BT to ensure minimum power
  WiFi.mode(WIFI_OFF);
  if(pairingMode) BLEDevice::deinit(false);
  
  esp_sleep_enable_ext0_wakeup(VIBRATION_GPIO, 1);
  delay(100);
  esp_deep_sleep_start();
}

bool checkVibrationSensitivity() {
  if (vibSensitivity == 1) return true; // High sensitivity, any wake is enough
  
  // For Medium (>= 3 pulses) or Low (>= 7 pulses) in a 400ms window
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
  
  Serial.println("Vibration pulses counted: " + String(pulses));
  return (pulses >= targetPulses);
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
    body { font-family: sans-serif; background: #121212; color: #fff; text-align: center; margin: 0; padding: 20px; }
    .btn { display: block; width: 100%; padding: 15px; margin: 10px 0; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; }
    .btn-green { background: #00e676; color: #000; }
    .btn-red { background: #ff3d57; color: #fff; }
    .btn-blue { background: #448aff; color: #fff; }
    .btn-yellow { background: #ffea00; color: #000; }
    .card { background: #1e1e1e; padding: 20px; border-radius: 12px; margin-bottom: 20px; }
    input[type=text], select { width: calc(100% - 22px); padding: 10px; margin-bottom: 10px; border-radius: 6px; border: 1px solid #444; background: #222; color: #fff; }
    .pin-box { background: #333; padding: 15px; font-size: 24px; letter-spacing: 5px; color: #00e676; border-radius: 8px; margin-top: 10px; display: none; }
    .badge { display: inline-block; padding: 5px 10px; border-radius: 4px; font-size: 14px; margin-left: 10px; }
    .badge-green { background: #00e676; color: #000; }
    .badge-gray { background: #555; color: #fff; }
    .log-box { background: #111; padding: 10px; border-radius: 6px; font-family: monospace; font-size: 12px; text-align: left; height: 150px; overflow-y: auto; white-space: pre-wrap; }
  </style>
</head>
<body>
  <h2>🏍️ Smart Bike Portal</h2>
  
  <div class="card" id="statusCard">
    <h3 style="display:inline-block; margin:0;" id="statusText">Checking Status...</h3>
    <span id="btBadge" class="badge badge-gray">BT: Unknown</span>
  </div>

  <div class="card">
    <h3>Controls</h3>
    <button class="btn btn-blue" onclick="cmd('check_bt')" style="margin-bottom: 20px;">📡 Scan for Phone Proximity</button>
    <button class="btn btn-green" onclick="cmd('ign_on')">🔓 Ignition ON</button>
    <button class="btn btn-yellow" onclick="cmd('start')">⚡ Self Start</button>
    <button class="btn btn-red" onclick="cmd('ign_off')">🔒 Ignition OFF</button>
    <button class="btn btn-blue" onclick="cmd('locate')">🔔 Locate Bike (Buzzer)</button>
  </div>

  <div class="card">
    <h3>Settings</h3>
    <label>
      <input type="checkbox" id="dsToggle" onchange="toggleDS()"> 
      Enable Deep Sleep (Wake on Vibration)
    </label>
    <br><br>
    <label>
      <input type="checkbox" id="autoUnlockToggle" onchange="toggleAutoUnlock()"> 
      Auto Unlock on Vibration (via Whitelisted BT)
    </label>
    <br><br>
    
    <label>Vibration Sensitivity:</label>
    <select id="vibSelect" onchange="updateSensSettings()">
      <option value="1">High (Very Sensitive)</option>
      <option value="2">Medium (Normal)</option>
      <option value="3">Low (Hard hit needed)</option>
    </select>
    
    <label>Bluetooth Auto-Unlock Range:</label>
    <select id="btSelect" onchange="updateSensSettings()">
      <option value="1">High (Very Close)</option>
      <option value="2">Medium (Within a few meters)</option>
      <option value="3">Low (Max Range)</option>
    </select>

    <h4>Whitelisted BT MACs:</h4>
    <div id="macList" style="margin-bottom: 10px;"></div>
    <input type="text" id="macInput" placeholder="Add New MAC Address">
    <button class="btn btn-blue" onclick="saveMac()">Add Manual MAC</button>
    <button class="btn btn-green" onclick="fetchStatus()">Refresh Whitelist</button>
    <button class="btn btn-red" onclick="cmd('clear_macs')" style="margin-top:10px;">Clear All MACs</button>
    <br><hr style="border-color:#333;"><br>
    
    <button class="btn btn-yellow" onclick="startPairing()">Enable Bluetooth Discovery</button>
    <div id="pinBox" class="pin-box"></div>
    <p style="font-size:12px; color:#aaa; margin-top:5px;">Connect your phone to 'Bike_Pair' and enter this PIN. Your MAC will be saved automatically.</p>
  </div>

  <div class="card">
    <h3>Audit Logs</h3>
    <div id="logBox" class="log-box">Loading logs...</div>
  </div>

  <script>
    async function fetchStatus() {
      const res = await fetch('/status');
      const data = await res.json();
      document.getElementById('statusText').innerText = "Status: " + data.state.toUpperCase();
      document.getElementById('dsToggle').checked = data.ds;
      document.getElementById('autoUnlockToggle').checked = data.auto_unlock;
      document.getElementById('vibSelect').value = data.vib_sens;
      document.getElementById('btSelect').value = data.bt_sens;
      
      let macsArray = data.macs.split(',').filter(m => m.trim().length > 0);
      let macHtml = '';
      macsArray.forEach(mac => {
        macHtml += `<div style="background:#333; padding:10px; margin-bottom:5px; border-radius:5px; display:flex; justify-content:space-between; align-items:center;">
                     <span style="font-family:monospace;">${mac}</span>
                     <button style="background:#ff4d4d; color:white; border:none; padding:5px 10px; border-radius:3px; cursor:pointer; font-weight:bold;" onclick="deleteMac('${mac}')">Delete</button>
                   </div>`;
      });
      document.getElementById('macList').innerHTML = macHtml;
      

      if(data.bt_nearby) {
        document.getElementById('btBadge').className = "badge badge-green";
        document.getElementById('btBadge').innerText = "📱 Phone Nearby";
      } else {
        document.getElementById('btBadge').className = "badge badge-gray";
        document.getElementById('btBadge').innerText = "Phone Not Found";
      }
      
      if(data.pairing) {
        document.getElementById('pinBox').style.display = "block";
        document.getElementById('pinBox').innerText = data.pin;
      } else {
        document.getElementById('pinBox').style.display = "none";
      }
      
      const logRes = await fetch('/logs');
      const logData = await logRes.text();
      document.getElementById('logBox').innerText = logData;
    }
    
    async function cmd(action) {
      await fetch('/cmd?a=' + action);
      fetchStatus();
    }
    
    async function toggleDS() {
      const val = document.getElementById('dsToggle').checked ? 1 : 0;
      await fetch('/set_ds?v=' + val);
    }
    
    async function toggleAutoUnlock() {
      const val = document.getElementById('autoUnlockToggle').checked ? 1 : 0;
      await fetch('/set_auto?v=' + val);
    }
    
    async function updateSensSettings() {
      const v = document.getElementById('vibSelect').value;
      const b = document.getElementById('btSelect').value;
      await fetch(`/set_sens?v=${v}&b=${b}`);
    }
    
    async function saveMac() {
      const res = await fetch('/status');
      const data = await res.json();
      let macsArray = data.macs.split(',').filter(m => m.trim().length > 0);
      
      const newMac = document.getElementById('macInput').value.trim().toUpperCase();
      if (newMac && !macsArray.includes(newMac)) {
        macsArray.push(newMac);
        await fetch('/set_mac?v=' + encodeURIComponent(macsArray.join(',')));
        document.getElementById('macInput').value = '';
        fetchStatus();
      }
    }
    
    async function deleteMac(macToRemove) {
      const res = await fetch('/status');
      const data = await res.json();
      let macsArray = data.macs.split(',').filter(m => m.trim().length > 0);
      
      macsArray = macsArray.filter(m => m !== macToRemove);
      await fetch('/set_mac?v=' + encodeURIComponent(macsArray.join(',')));
      fetchStatus();
    }
    
    async function startPairing() {
      await fetch('/start_pairing');
      fetchStatus();
    }
    
    fetchStatus();
    setInterval(fetchStatus, 3000);
  </script>
</body>
</html>
)rawliteral";

// ─────────────────────── API HANDLERS ──────────────────────────
void handleRoot() { server.send(200, "text/html", MAIN_PAGE); }

void handleStatus() {
  String stateStr = (currentState == STATE_LOCKED) ? "locked" : 
                    (currentState == STATE_UNLOCKED) ? "unlocked" : "engine_on";
  String json = "{\"state\":\"" + stateStr + 
                "\", \"ds\":" + (deepSleepEnabled ? "true" : "false") + 
                ", \"auto_unlock\":" + (autoUnlockEnabled ? "true" : "false") +
                ", \"vib_sens\":" + String(vibSensitivity) +
                ", \"bt_sens\":" + String(btRange) +
                ", \"pairing\":" + (pairingMode ? "true" : "false") + 
                ", \"bt_nearby\":" + (btNearby ? "true" : "false") + 
                ", \"pin\":\"" + String(pairingPIN) + "\"" +
                ", \"macs\":\"" + whitelistMacs + "\"}";
  server.send(200, "application/json", json);
}

void handleLogs() {
  String out = "";
  // print from oldest to newest in the circular buffer
  for(int i=0; i<MAX_LOGS; i++) {
    int idx = (logIndex + i) % MAX_LOGS;
    if(auditLogs[idx].length() > 0) {
      out += auditLogs[idx] + "\n";
    }
  }
  if(out == "") out = "No logs yet.";
  server.send(200, "text/plain", out);
}

void handleCmd() {
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
  } else if (action == "check_bt") {
    addLog("Scanning for Phone...");
    scanForAuthorizedBluetooth();
    if(btNearby) addLog("Phone found nearby!");
    else addLog("Phone not found nearby.");
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
  deepSleepEnabled = (server.arg("v") == "1");
  preferences.begin("bike", false);
  preferences.putBool("ds", deepSleepEnabled);
  preferences.end();
  addLog("Deep Sleep toggled: " + String(deepSleepEnabled));
  server.send(200, "text/plain", "OK");
}

void handleSetAutoUnlock() {
  autoUnlockEnabled = (server.arg("v") == "1");
  preferences.begin("bike", false);
  preferences.putBool("auto_unlock", autoUnlockEnabled);
  preferences.end();
  addLog("Auto-Unlock toggled: " + String(autoUnlockEnabled));
  server.send(200, "text/plain", "OK");
}

void handleSetSens() {
  vibSensitivity = server.arg("v").toInt();
  btRange = server.arg("b").toInt();
  if(vibSensitivity < 1 || vibSensitivity > 3) vibSensitivity = 1;
  if(btRange < 1 || btRange > 3) btRange = 2;
  
  preferences.begin("bike", false);
  preferences.putInt("vib", vibSensitivity);
  preferences.putInt("bt", btRange);
  preferences.end();
  addLog("Sensitivities updated");
  server.send(200, "text/plain", "OK");
}

void handleSetMac() {
  whitelistMacs = server.arg("v");
  whitelistMacs.toUpperCase();
  preferences.begin("bike", false);
  preferences.putString("macs", whitelistMacs);
  preferences.end();
  addLog("MAC whitelist updated manually");
  server.send(200, "text/plain", "OK");
}

void handleStartPairing() {
  enableBluetoothPairing();
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
  autoUnlockEnabled = preferences.getBool("auto_unlock", false);
  vibSensitivity = preferences.getInt("vib", 1);
  btRange = preferences.getInt("bt", 2);
  whitelistMacs = preferences.getString("macs", "");
  preferences.end();
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  // Power Saving
  setCpuFrequencyMhz(80);
  
  // Setup WiFi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogs);
  server.on("/cmd", handleCmd);
  server.on("/set_ds", handleSetDS);
  server.on("/set_auto", handleSetAutoUnlock);
  server.on("/set_sens", handleSetSens);
  server.on("/set_mac", handleSetMac);
  server.on("/start_pairing", handleStartPairing);
  server.onNotFound(handleRedirect);
  
  server.begin();
  
  // Handle Wakeup Cause
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    addLog("Woke up by Vibration Sensor");
    Serial.println("[WAKE] Woken up by Vibration Sensor!");
    
    if (checkVibrationSensitivity()) {
      if (autoUnlockEnabled && whitelistMacs.length() > 0) {
        if (scanForAuthorizedBluetooth()) {
          addLog("Whitelisted BT found! Unlocking.");
          Serial.println("[AUTH] Whitelisted BT found! Unlocking...");
          currentState = STATE_ENGINE_ON;
          ignitionOn();
          updateLEDs();
          beepUnlock();
        } else {
          addLog("No whitelisted BT found. Sleeping.");
          Serial.println("[AUTH] No whitelisted BT found. Sleeping.");
          goToDeepSleep(); 
        }
      } else {
        addLog("Auto-unlock disabled. Waiting for portal.");
        Serial.println("[WAKE] Auto-unlock disabled or no MACs. Staying awake for portal.");
        currentState = STATE_LOCKED;
        updateLEDs();
        beepLock();
      }
    } else {
      addLog("Vibration false alarm. Sleeping.");
      Serial.println("[WAKE] Vibration false alarm (sensitivity threshold not met). Sleeping.");
      goToDeepSleep();
    }
  } else {
    addLog("Normal Boot / Reset");
    Serial.println("[BOOT] Normal Boot");
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
  
  // Auto-lock and Deep Sleep
  if (!pairingMode && (millis() - lastActivityTime > AUTO_LOCK_TIMEOUT)) {
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
