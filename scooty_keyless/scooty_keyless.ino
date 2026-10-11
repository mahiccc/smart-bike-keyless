/*
 * ============================================================
 *  SCOOTY KEYLESS IGNITION SYSTEM
 *  Board: Seeed Studio XIAO ESP32S3 / ESP32C3
 * ============================================================
 *
 *  Features:
 *    - Bluetooth Low Energy (BLE) phone-based unlock/lock
 *    - Relay-controlled ignition ON/OFF
 *    - Buzzer feedback (lock/unlock/alarm sounds)
 *    - LED status indicators
 *    - Auto-lock after timeout
 *    - Anti-theft alarm (optional vibration sensor)
 *    - Secure PIN authentication over BLE
 *
 *  Wiring Diagram:
 *    XIAO ESP32 Pin  ->  Component
 *    ─────────────────────────────────
 *    D0 (GPIO1)      ->  Relay Module IN (Ignition)
 *    D1 (GPIO2)      ->  Relay Module IN (Starter Motor)
 *    D2 (GPIO3)      ->  Buzzer (+)
 *    D3 (GPIO4)      ->  LED Green (Unlocked indicator)
 *    D4 (GPIO5)      ->  LED Red (Locked indicator)
 *    D5 (GPIO6)      ->  Vibration Sensor (SW-420) [Optional]
 *    5V              ->  Relay VCC, Buzzer VCC
 *    GND             ->  Common GND for all components
 *
 *  BLE Commands (send from phone app like "nRF Connect" or "Serial Bluetooth Terminal"):
 *    "UNLOCK:<PIN>"  ->  Unlock the scooty (e.g., "UNLOCK:1234")
 *    "LOCK"          ->  Lock the scooty
 *    "START"         ->  Start the engine (must be unlocked first)
 *    "STOP"          ->  Stop the engine
 *    "STATUS"        ->  Get current status
 *    "SETPIN:<OLD>:<NEW>"  ->  Change PIN (e.g., "SETPIN:1234:5678")
 *
 * ============================================================
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ─────────────────────── PIN DEFINITIONS ───────────────────────
#define RELAY_IGNITION    16    // Relay 1: Ignition circuit
#define RELAY_STARTER     5     // Relay 2: Starter motor
#define BUZZER_PIN        4     // Piezo buzzer
#define LED_GREEN         18    // Green LED - Unlocked
#define LED_RED           19    // Red LED - Locked
#define VIBRATION_SENSOR  21    // SW-420 Vibration sensor (optional)

// ─────────────────────── CONFIGURATION ─────────────────────────
#define DEFAULT_PIN           "1234"        // Default security PIN
#define AUTO_LOCK_TIMEOUT     300000        // Auto-lock after 5 min (ms) of inactivity
#define STARTER_DURATION      2000          // Starter motor run time (ms)
#define ALARM_VIBRATION_THRESH 3            // Vibration triggers before alarm
#define ALARM_DURATION        10000         // Alarm duration (ms)
#define BLE_DEVICE_NAME       "MyScooty"    // Bluetooth device name

// ─────────────────────── BLE UUIDs ─────────────────────────────
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_COMMAND_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Write commands
#define CHAR_STATUS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a9"  // Read status/notifications

// ─────────────────────── STATE MACHINE ─────────────────────────
enum ScootyState {
  STATE_LOCKED,
  STATE_UNLOCKED,
  STATE_ENGINE_ON,
  STATE_ALARM
};

// ─────────────────────── GLOBAL VARIABLES ──────────────────────
ScootyState currentState = STATE_LOCKED;
String securityPIN = DEFAULT_PIN;
unsigned long lastActivityTime = 0;
unsigned long alarmStartTime = 0;
int vibrationCount = 0;
unsigned long lastVibrationTime = 0;
bool deviceConnected = false;
bool oldDeviceConnected = false;

Preferences preferences;
BLEServer* pServer = NULL;
BLECharacteristic* pCommandChar = NULL;
BLECharacteristic* pStatusChar = NULL;

// ─────────────────────── BUZZER TONES ──────────────────────────
void beepUnlock() {
  // Two short high beeps = unlocked
  for (int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, 2000, 100);
    delay(150);
  }
  noTone(BUZZER_PIN);
}

void beepLock() {
  // One long low beep = locked
  tone(BUZZER_PIN, 800, 300);
  delay(350);
  noTone(BUZZER_PIN);
}

void beepError() {
  // Three rapid low beeps = error/denied
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 400, 80);
    delay(120);
  }
  noTone(BUZZER_PIN);
}

void beepStart() {
  // Rising tone = engine starting
  for (int freq = 500; freq <= 2500; freq += 200) {
    tone(BUZZER_PIN, freq, 50);
    delay(60);
  }
  noTone(BUZZER_PIN);
}

void beepConfirm() {
  // Single medium beep = confirmed
  tone(BUZZER_PIN, 1500, 150);
  delay(200);
  noTone(BUZZER_PIN);
}

void alarmSound() {
  // Alternating siren
  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, 3000, 200);
    delay(250);
    tone(BUZZER_PIN, 1500, 200);
    delay(250);
  }
  noTone(BUZZER_PIN);
}

// ─────────────────────── LED CONTROL ───────────────────────────
void updateLEDs() {
  switch (currentState) {
    case STATE_LOCKED:
      digitalWrite(LED_RED, HIGH);
      digitalWrite(LED_GREEN, LOW);
      break;
    case STATE_UNLOCKED:
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GREEN, HIGH);
      break;
    case STATE_ENGINE_ON:
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GREEN, HIGH);  // Solid green when engine is on
      break;
    case STATE_ALARM:
      // Rapid blink both LEDs (handled in loop)
      break;
  }
}

// ─────────────────────── RELAY CONTROL ─────────────────────────
void ignitionOn() {
  digitalWrite(RELAY_IGNITION, HIGH);
  Serial.println("[RELAY] Ignition ON");
}

void ignitionOff() {
  digitalWrite(RELAY_IGNITION, LOW);
  Serial.println("[RELAY] Ignition OFF");
}

void startEngine() {
  Serial.println("[ENGINE] Starting motor...");
  beepStart();
  ignitionOn();
  delay(500);
  
  // Engage starter motor briefly
  digitalWrite(RELAY_STARTER, HIGH);
  delay(STARTER_DURATION);
  digitalWrite(RELAY_STARTER, LOW);
  
  Serial.println("[ENGINE] Starter disengaged, engine running");
}

void stopEngine() {
  Serial.println("[ENGINE] Stopping...");
  ignitionOff();
  digitalWrite(RELAY_STARTER, LOW);
}

// ─────────────────────── STATE TRANSITIONS ─────────────────────
String getStatusString() {
  String status = "State: ";
  switch (currentState) {
    case STATE_LOCKED:    status += "LOCKED 🔒";    break;
    case STATE_UNLOCKED:  status += "UNLOCKED 🔓";  break;
    case STATE_ENGINE_ON: status += "ENGINE ON 🏍️"; break;
    case STATE_ALARM:     status += "⚠️ ALARM ⚠️";  break;
  }
  status += " | BLE: ";
  status += deviceConnected ? "Connected" : "Disconnected";
  return status;
}

void sendStatusNotification(String message) {
  if (deviceConnected && pStatusChar != NULL) {
    pStatusChar->setValue(message.c_str());
    pStatusChar->notify();
  }
  Serial.println("[STATUS] " + message);
}

bool unlockScooty(String pin) {
  if (pin != securityPIN) {
    beepError();
    sendStatusNotification("❌ Wrong PIN!");
    Serial.println("[AUTH] Wrong PIN attempt: " + pin);
    return false;
  }
  
  currentState = STATE_UNLOCKED;
  lastActivityTime = millis();
  beepUnlock();
  updateLEDs();
  sendStatusNotification("✅ Scooty UNLOCKED");
  Serial.println("[AUTH] Scooty unlocked successfully");
  return true;
}

void lockScooty() {
  if (currentState == STATE_ENGINE_ON) {
    stopEngine();
  }
  currentState = STATE_LOCKED;
  ignitionOff();
  beepLock();
  updateLEDs();
  vibrationCount = 0;
  sendStatusNotification("🔒 Scooty LOCKED");
  Serial.println("[AUTH] Scooty locked");
}

bool startScooty() {
  if (currentState != STATE_UNLOCKED) {
    beepError();
    sendStatusNotification("❌ Unlock first!");
    return false;
  }
  
  currentState = STATE_ENGINE_ON;
  lastActivityTime = millis();
  startEngine();
  updateLEDs();
  sendStatusNotification("🏍️ Engine STARTED");
  return true;
}

void stopScooty() {
  if (currentState != STATE_ENGINE_ON) {
    beepError();
    sendStatusNotification("❌ Engine is not running");
    return;
  }
  
  stopEngine();
  currentState = STATE_UNLOCKED;
  lastActivityTime = millis();
  updateLEDs();
  beepConfirm();
  sendStatusNotification("🛑 Engine STOPPED");
}

bool changePIN(String oldPin, String newPin) {
  if (oldPin != securityPIN) {
    beepError();
    sendStatusNotification("❌ Wrong current PIN!");
    return false;
  }
  if (newPin.length() < 4 || newPin.length() > 8) {
    beepError();
    sendStatusNotification("❌ PIN must be 4-8 digits!");
    return false;
  }
  
  securityPIN = newPin;
  // Save to non-volatile storage
  preferences.begin("scooty", false);
  preferences.putString("pin", securityPIN);
  preferences.end();
  
  beepConfirm();
  sendStatusNotification("✅ PIN changed successfully");
  Serial.println("[AUTH] PIN changed");
  return true;
}

void triggerAlarm() {
  currentState = STATE_ALARM;
  alarmStartTime = millis();
  sendStatusNotification("⚠️ ALARM TRIGGERED! Possible theft!");
  Serial.println("[ALARM] TRIGGERED!");
}

// ─────────────────────── COMMAND PROCESSOR ─────────────────────
void processCommand(String command) {
  command.trim();
  command.toUpperCase();
  Serial.println("[CMD] Received: " + command);
  lastActivityTime = millis();
  
  if (command.startsWith("UNLOCK:")) {
    String pin = command.substring(7);
    pin.trim();
    // PIN is case-sensitive, re-read from original
    unlockScooty(pin);
  }
  else if (command == "LOCK") {
    lockScooty();
  }
  else if (command == "START") {
    startScooty();
  }
  else if (command == "STOP") {
    stopScooty();
  }
  else if (command == "STATUS") {
    sendStatusNotification(getStatusString());
  }
  else if (command.startsWith("SETPIN:")) {
    // Format: SETPIN:oldpin:newpin
    String params = command.substring(7);
    int colonIdx = params.indexOf(':');
    if (colonIdx > 0) {
      String oldPin = params.substring(0, colonIdx);
      String newPin = params.substring(colonIdx + 1);
      oldPin.trim();
      newPin.trim();
      changePIN(oldPin, newPin);
    } else {
      beepError();
      sendStatusNotification("❌ Format: SETPIN:oldpin:newpin");
    }
  }
  else {
    beepError();
    sendStatusNotification("❌ Unknown command: " + command);
  }
}

// ─────────────────────── BLE CALLBACKS ─────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Device connected");
    beepConfirm();
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Device disconnected");
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      processCommand(value);
    }
  }
};

// ─────────────────────── BLE SETUP ─────────────────────────────
void setupBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  
  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Create BLE Service
  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  // Command Characteristic (Write)
  pCommandChar = pService->createCharacteristic(
    CHAR_COMMAND_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCommandChar->setCallbacks(new CommandCallbacks());
  
  // Status Characteristic (Read + Notify)
  pStatusChar = pService->createCharacteristic(
    CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());
  pStatusChar->setValue("🔒 Scooty Locked");
  
  // Start service and advertising
  pService->start();
  
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("[BLE] Service started, advertising as: " + String(BLE_DEVICE_NAME));
}

// ─────────────────────── LOAD SAVED PIN ────────────────────────
void loadSavedPIN() {
  preferences.begin("scooty", true);  // read-only
  String savedPin = preferences.getString("pin", "");
  preferences.end();
  
  if (savedPin.length() >= 4) {
    securityPIN = savedPin;
    Serial.println("[INIT] Loaded saved PIN");
  } else {
    securityPIN = DEFAULT_PIN;
    Serial.println("[INIT] Using default PIN: " + String(DEFAULT_PIN));
  }
}

// ─────────────────────── SETUP ─────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║   SCOOTY KEYLESS IGNITION SYSTEM      ║");
  Serial.println("║   Seeed Studio XIAO ESP32             ║");
  Serial.println("╚═══════════════════════════════════════╝");
  
  // Initialize pins
  pinMode(RELAY_IGNITION, OUTPUT);
  pinMode(RELAY_STARTER, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(VIBRATION_SENSOR, INPUT);
  
  // Ensure relays are OFF at startup
  digitalWrite(RELAY_IGNITION, LOW);
  digitalWrite(RELAY_STARTER, LOW);
  
  // Load saved PIN from flash
  loadSavedPIN();
  
  // Initial state: LOCKED
  currentState = STATE_LOCKED;
  updateLEDs();
  
  // Initialize BLE
  setupBLE();
  
  // Startup sound
  beepLock();
  
  lastActivityTime = millis();
  Serial.println("[INIT] System ready! Connect via BLE app.");
  Serial.println("[INIT] Send 'UNLOCK:1234' to unlock (default PIN)");
}

// ─────────────────────── MAIN LOOP ─────────────────────────────
void loop() {
  unsigned long currentTime = millis();
  
  // ── Handle BLE reconnection ──
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("[BLE] Restarting advertising...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // ── Auto-lock timeout ──
  if ((currentState == STATE_UNLOCKED) && 
      (currentTime - lastActivityTime > AUTO_LOCK_TIMEOUT)) {
    Serial.println("[AUTO] Auto-locking due to inactivity...");
    lockScooty();
  }
  
  // ── Vibration sensor anti-theft (only when locked) ──
  if (currentState == STATE_LOCKED) {
    if (digitalRead(VIBRATION_SENSOR) == HIGH) {
      if (currentTime - lastVibrationTime > 1000) {  // Debounce 1 sec
        vibrationCount++;
        lastVibrationTime = currentTime;
        Serial.println("[SENSOR] Vibration detected! Count: " + String(vibrationCount));
        
        if (vibrationCount >= ALARM_VIBRATION_THRESH) {
          triggerAlarm();
        }
      }
    }
    
    // Reset vibration count after 30 seconds of no vibration
    if (currentTime - lastVibrationTime > 30000) {
      vibrationCount = 0;
    }
  }
  
  // ── Alarm state handling ──
  if (currentState == STATE_ALARM) {
    alarmSound();
    
    // Blink both LEDs during alarm
    bool blinkState = (currentTime / 200) % 2;
    digitalWrite(LED_RED, blinkState);
    digitalWrite(LED_GREEN, !blinkState);
    
    // Auto-stop alarm after duration, return to locked
    if (currentTime - alarmStartTime > ALARM_DURATION) {
      currentState = STATE_LOCKED;
      vibrationCount = 0;
      updateLEDs();
      Serial.println("[ALARM] Alarm stopped, returning to locked state");
    }
  }
  
  // ── Engine running LED pulse effect ──
  if (currentState == STATE_ENGINE_ON) {
    // Gentle pulse on green LED to indicate engine running
    int brightness = (sin(currentTime / 500.0) + 1) * 127;
    analogWrite(LED_GREEN, brightness);
  }
  
  delay(50);  // Small delay to prevent watchdog issues
}
