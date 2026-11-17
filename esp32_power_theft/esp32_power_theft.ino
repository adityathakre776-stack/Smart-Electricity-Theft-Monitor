/*
 * ============================================================
 *  Smart Router Battery Backup Monitor
 *  ACETECH IOT PROJECT
 * ============================================================
 *
 *  PROBLEM:  When AC supply goes OFF, router runs on backup
 *            battery. This battery dies silently — now we
 *            monitor it in real-time via BLE on your phone!
 *
 *  HARDWARE:
 *  ─────────────────────────────────────────────────────────
 *  Component              ESP32 Pin
 *  ─────────────────────────────────────────────────────────
 *  Voltage Sensor OUT  →  GPIO34  (ADC input, 0–3.3V)
 *  Voltage Sensor VCC  →  3.3V or 5V (check your module)
 *  Voltage Sensor GND  →  GND
 *  LED (optional)      →  GPIO2   (onboard LED)
 *
 *  BATTERY:
 *  ─────────────────────────────────────────────────────────
 *  Type    : 7.4V Li-ion / LiPo (2S)
 *  Max     : 7.4V  = 100%
 *  Min     : 3.0V  =   0%
 *
 *  BLE:
 *  ─────────────────────────────────────────────────────────
 *  Device Name : Router Battery
 *  Service UUID: 0x180F (Standard Battery Service)
 *  Char UUID   : 0x2A19 (Battery Level — READ + NOTIFY)
 *
 *  LIBRARIES REQUIRED (install via Arduino Library Manager):
 *  ─────────────────────────────────────────────────────────
 *  - ESP32 BLE Arduino (built-in with ESP32 board package)
 *
 * ============================================================
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "battery.h"
#include "ble_battery.h"

// ─────────────────────────────────────────────────────────────
//  CONFIGURATION — Edit these if needed
// ─────────────────────────────────────────────────────────────
#define BLE_DEVICE_NAME     "Router Battery"   // Name seen on phone
#define ONBOARD_LED          2                 // GPIO2 = built-in LED
#define READ_INTERVAL_MS   3000                // Read battery every 3 seconds
#define LOW_BATTERY_PCT     20                 // % threshold for "low" alert

// ─────────────────────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────────────────────
BLEServer*          pServer   = nullptr;
BLECharacteristic*  pBatChar  = nullptr;
bool                clientConnected = false;
unsigned long       lastReadTime    = 0;
uint8_t             lastPct         = 255;    // 255 = "not read yet"

// ─────────────────────────────────────────────────────────────
//  BLE Server Callbacks — track connect / disconnect
// ─────────────────────────────────────────────────────────────
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pSvr) override {
        clientConnected = true;
        digitalWrite(ONBOARD_LED, HIGH);      // LED ON when phone connected
        Serial.println("[BLE] ✅ Phone connected!");
    }

    void onDisconnect(BLEServer* pSvr) override {
        clientConnected = false;
        digitalWrite(ONBOARD_LED, LOW);
        Serial.println("[BLE] 📴 Phone disconnected — restarting advertising...");
        // Restart advertising so phone can reconnect
        BLEDevice::startAdvertising();
    }
};

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║  Smart Router Battery Backup Monitor     ║");
    Serial.println("║  ACETECH IOT PROJECT                     ║");
    Serial.println("║  Battery: 7.4V = 100%  |  3.0V = 0%     ║");
    Serial.println("╚══════════════════════════════════════════╝");

    // ── LED setup ──────────────────────────────────────────
    pinMode(ONBOARD_LED, OUTPUT);
    digitalWrite(ONBOARD_LED, LOW);

    // ── Battery sensor init ────────────────────────────────
    batteryInit();
    Serial.println("[Battery] Sensor initialized on GPIO34");

    // ── Initial battery reading (printed before BLE) ───────
    float v   = batteryReadVoltage();
    uint8_t p = batteryVoltageToPercent(v);
    Serial.printf("[Battery] First reading: %.2f V  →  %u %%\n", v, p);

    // ── BLE init ───────────────────────────────────────────
    BLEDevice::init(BLE_DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Create Battery Service + Characteristic
    pBatChar = bleBatterySetup(pServer);

    // Set initial value so phone sees it on first connect
    pBatChar->setValue(&p, 1);

    // ── BLE Advertising ────────────────────────────────────
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID("0000180F-0000-1000-8000-00805F9B34FB");
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);   // helps iPhone connections
    BLEDevice::startAdvertising();

    Serial.printf("[BLE] Advertising as \"%s\" — open battery.html on phone\n", BLE_DEVICE_NAME);
    Serial.println("[System] Ready!\n");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // Read battery at fixed interval
    if (now - lastReadTime >= READ_INTERVAL_MS) {
        lastReadTime = now;

        // ── Read voltage & convert to % ───────────────────
        float   voltage = batteryReadVoltage();
        uint8_t pct     = batteryVoltageToPercent(voltage);

        // ── Health label ──────────────────────────────────
        const char* health;
        if      (pct >= 80) health = "EXCELLENT 🟢";
        else if (pct >= 50) health = "GOOD      🟡";
        else if (pct >= 20) health = "LOW       🟠";
        else                health = "CRITICAL  🔴";

        // ── Serial print ──────────────────────────────────
        Serial.println("─────────────────────────────────────────");
        Serial.printf("  Voltage : %.2f V  (7.4V=100%%, 3.0V=0%%)\n", voltage);
        Serial.printf("  Battery : %u %%\n", pct);
        Serial.printf("  Health  : %s\n", health);
        Serial.printf("  BLE     : %s\n", clientConnected ? "Phone Connected" : "Waiting for phone...");
        if (pct < LOW_BATTERY_PCT) {
            Serial.println("  ⚠️  LOW BATTERY — Charge the backup battery!");
        }
        Serial.println("─────────────────────────────────────────");

        // ── Update BLE value ──────────────────────────────
        // Always update the readable value (even if no client)
        pBatChar->setValue(&pct, 1);

        // Send BLE notification if phone is connected
        // and percentage has actually changed
        if (clientConnected && pct != lastPct) {
            bleBatteryNotify(pBatChar, pct);
        }

        lastPct = pct;

        // ── LED blink on low battery ──────────────────────
        if (pct < LOW_BATTERY_PCT && !clientConnected) {
            // Fast blink = low battery warning (when not connected)
            for (int i = 0; i < 3; i++) {
                digitalWrite(ONBOARD_LED, HIGH); delay(100);
                digitalWrite(ONBOARD_LED, LOW);  delay(100);
            }
        }
    }

    delay(10);  // Yield to ESP32 background tasks (WiFi/BLE stack)
}

// ─────────────────────────────────────────────────────────────
//  END OF FILE
// ─────────────────────────────────────────────────────────────
