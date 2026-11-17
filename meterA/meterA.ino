/*
 * ================================================================
 *  METER A — Supply / Pole Side
 *  Smart Electricity Theft Detection System
 *
 *  Hardware : ESP32 Dev Board
 *             ADS1115 16-bit ADC  (I2C 0x48)
 *             SCT-013 CT Sensor   (ADS1115 A0, 100Ω burden + bias)
 *             ZMPT101B Voltage    (ADS1115 A1)
 *             Relay Module        (GPIO26)
 *
 *  WIRING (ADS1115):
 *    VCC  → ESP32 3.3V
 *    GND  → ESP32 GND
 *    SDA  → ESP32 GPIO21
 *    SCL  → ESP32 GPIO22
 *    ADDR → GND  (address 0x48)
 *    A0   → CT Sensor out  (with 100Ω burden & bias circuit)
 *    A1   → ZMPT101B OUT
 *
 *  CT BIAS CIRCUIT (required for single-ended ADC):
 *    3.3V ──10kΩ──┬──10kΩ── GND
 *                 │
 *                 ├── 10µF cap to GND
 *                 └── CT sensor output (via burden)
 *    This biases the AC signal to 1.65V DC midpoint.
 *
 *  Libraries (install via Arduino Library Manager):
 *    - PubSubClient  by Nick O'Leary
 *    - Adafruit ADS1X15
 *    - Adafruit BusIO (dependency)
 * ================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <math.h>

// ── WiFi ─────────────────────────────────────────────────────
// ⚠ Connect ESP32 to PC HOTSPOT (not home router)
// Go to: Windows Settings → System → Mobile Hotspot → note the name+password
const char* WIFI_SSID   = "LAPTOP-5LJI46P9 1083"; // PC Hotspot name
const char* WIFI_PASS   = "12345678";              // PC Hotspot password

// ── MQTT ─────────────────────────────────────────────────────
const char* MQTT_BROKER = "192.168.137.1"; // ← PC Hotspot IP (FIXED, never changes)
const int   MQTT_PORT   = 1883;
const char* CLIENT_ID   = "ESP32_MeterA";

const char* TOPIC_DATA   = "meter/A/data";
const char* TOPIC_STATUS = "meter/A/status";
const char* TOPIC_RELAY  = "meter/A/relay";   // subscribe → control relay

// ── Pins ─────────────────────────────────────────────────────
#define RELAY_PIN   26
#define I2C_SDA     21
#define I2C_SCL     22

// ── ADS1115 Channels ─────────────────────────────────────────
#define CH_CURRENT  0    // A0 → CT Sensor
#define CH_VOLTAGE  1    // A1 → ZMPT101B

// ── CT Sensor Calibration ────────────────────────────────────
// ✔ Using ZMCT103C or ZHT103 (1000:1 ratio, chip-style)
// Thread Phase wire THROUGH the chip hole (not clamp-around!)
// ZMCT103C/ZHT103 spec:
//   Primary turns  : 1 (the wire you pass through)
//   Secondary turns: 1000
//   Max current    : 5A
//   Burden resistor: 200Ω external
// Calibration: new_CT_RATIO = old × (actual_A / displayed_A)
#define CT_RATIO      1000.0f  // ← ZMCT103C / ZHT103 (1000:1)
#define BURDEN_OHMS   200.0f   // ← 200Ω external burden resistor

// ADS1115 GAIN_ONE = ±4.096V → 0.125 mV per LSB
#define VOLTS_PER_BIT 0.0001250f

// ── ZMPT101B Calibration ─────────────────────────────────────
// Calibrated via ADS1115: showed 192V → target 220V
// new factor = 0.475 × (220/192) = 0.544
#define VOLTAGE_FACTOR  0.544f  // ← FINAL calibrated value

// ── Theft Threshold ──────────────────────────────────────────
#define THEFT_THRESHOLD  0.5f   // Amps

// ── Sampling ─────────────────────────────────────────────────
#define SAMPLES       200
#define PUBLISH_MS    2000

// ── Objects ──────────────────────────────────────────────────
Adafruit_ADS1115 ads;
WiFiClient       espClient;
PubSubClient     mqtt(espClient);

// ── State ────────────────────────────────────────────────────
float    voltRMS = 0, currRMS = 0, power = 0, energy = 0;
bool     relayON = false;
unsigned long lastPublish   = 0;
unsigned long lastReconnect = 0;
unsigned long energyTimer   = 0;
unsigned long startTime     = 0;

// ─────────────────────────────────────────────────────────────
//  READ RMS CURRENT  (two-pass: auto-zero + RMS)
// ─────────────────────────────────────────────────────────────
float readCurrentRMS() {
    float sumRaw = 0, sumSq = 0;

    for (int i = 0; i < SAMPLES; i++) { sumRaw += ads.readADC_SingleEnded(CH_CURRENT); }
    float mid = sumRaw / SAMPLES;

    for (int i = 0; i < SAMPLES; i++) {
        float v = ads.readADC_SingleEnded(CH_CURRENT) - mid;
        sumSq += v * v;
    }

    float rmsVolt = sqrtf(sumSq / SAMPLES) * VOLTS_PER_BIT;
    float current = (rmsVolt / BURDEN_OHMS) * CT_RATIO;
    Serial.printf("[DBG-CT] raw=%.4f A  rmsVolt=%.6f V\n", current, rmsVolt);
    return (current < 0.03f) ? 0.0f : current;
}

// ─────────────────────────────────────────────────────────────
//  READ RMS VOLTAGE  (ZMPT101B via ADS1115)
// ─────────────────────────────────────────────────────────────
float readVoltageRMS() {
    float sumRaw = 0, sumSq = 0;

    for (int i = 0; i < SAMPLES; i++) { sumRaw += ads.readADC_SingleEnded(CH_VOLTAGE); }
    float mid = sumRaw / SAMPLES;

    for (int i = 0; i < SAMPLES; i++) {
        float v = ads.readADC_SingleEnded(CH_VOLTAGE) - mid;
        sumSq += v * v;
    }

    float rmsRaw = sqrtf(sumSq / SAMPLES);
    float voltage = rmsRaw * VOLTS_PER_BIT * 1000.0f * VOLTAGE_FACTOR;
    return (voltage < 30.0f || voltage > 290.0f) ? 0.0f : voltage;
}

// ─────────────────────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t++ < 40) { delay(500); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] ✔ IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] ✖ Failed — restarting"); delay(2000); ESP.restart();
    }
}

// ─────────────────────────────────────────────────────────────
//  MQTT CALLBACK
// ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    String msg = "";
    for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    Serial.printf("[MQTT] [%s]: %s\n", topic, msg.c_str());

    if (String(topic) == TOPIC_RELAY) {
        relayON = (msg.indexOf("ON") >= 0 || msg.indexOf("true") >= 0);
        digitalWrite(RELAY_PIN, relayON ? HIGH : LOW);
        Serial.printf("[RELAY] → %s\n", relayON ? "ON" : "OFF");
    }
}

// ─────────────────────────────────────────────────────────────
//  MQTT CONNECT
// ─────────────────────────────────────────────────────────────
bool connectMQTT() {
    if (mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
    // LWT: publish offline status on unexpected disconnect
    if (mqtt.connect(CLIENT_ID, nullptr, nullptr,
                     TOPIC_STATUS, 1, true, "{\"online\":false}")) {
        Serial.println(" ✔ OK");
        mqtt.subscribe(TOPIC_RELAY);
        mqtt.publish(TOPIC_STATUS, "{\"online\":true}", true);
        return true;
    }
    Serial.printf(" ✖ RC=%d\n", mqtt.state());
    return false;
}

// ─────────────────────────────────────────────────────────────
//  PUBLISH DATA
// ─────────────────────────────────────────────────────────────
void publishData() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"A\",\"voltage\":%.2f,\"current\":%.4f,"
        "\"power\":%.2f,\"energy\":%.5f,\"relay\":%s,\"ts\":%lu}",
        voltRMS, currRMS, power, energy,
        relayON ? "true" : "false", millis());
    mqtt.publish(TOPIC_DATA, buf, true);
    Serial.println("[MQTT] Published → meter/A/data");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  METER A — Supply Side            ║");
    Serial.println("║  Power Theft Detection System     ║");
    Serial.println("╚═══════════════════════════════════╝\n");

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!ads.begin()) {
        Serial.println("[ADS] ✖ ADS1115 not found! Check wiring."); while (1);
    }
    ads.setGain(GAIN_ONE);         // ±4.096V range
    ads.setDataRate(RATE_ADS1115_860SPS); // fastest sampling
    Serial.println("[ADS] ✔ ADS1115 ready (GAIN_ONE, 860SPS)");

    Serial.printf("[CFG] CT Ratio: %.0f:1  Burden: %.0fΩ  Calib: %.3f\n",
        CT_RATIO, BURDEN_OHMS, CT_RATIO / BURDEN_OHMS);

    connectWiFi();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(384);
    connectMQTT();

    energyTimer = startTime = millis();
    Serial.println("[SYS] Ready — publishing every 2 seconds\n");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[WiFi] Lost — reconnecting"); connectWiFi(); }

    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnect > 5000) { lastReconnect = now; connectMQTT(); }
    }
    mqtt.loop();

    if (millis() - lastPublish >= PUBLISH_MS) {
        lastPublish = millis();

        voltRMS = readVoltageRMS();
        currRMS = readCurrentRMS();
        power   = voltRMS * currRMS;

        // Energy accumulation (kWh)
        unsigned long now = millis();
        energy += power * ((now - energyTimer) / 3600000.0f) / 1000.0f;
        energyTimer = now;

        // Serial output
        Serial.println("══════════════════════════════════════");
        Serial.printf(" [A] Voltage : %7.2f V\n", voltRMS);
        Serial.printf(" [A] Current : %7.4f A\n", currRMS);
        Serial.printf(" [A] Power   : %7.2f W\n", power);
        Serial.printf(" [A] Energy  : %7.5f kWh\n", energy);
        Serial.printf(" [A] Relay   : %s\n", relayON ? "ON" : "OFF");
        Serial.println("══════════════════════════════════════");

        if (mqtt.connected()) publishData();
    }

    delay(10);
}

/*
 * ================================================================
 *  CALIBRATION NOTES
 * ================================================================
 *  1. CT Sensor ratio:
 *     - Check label on sensor body (e.g. "1000:1" or "SCT-013-030")
 *     - SCT-013-000 → CT_RATIO=2000, BURDEN_OHMS=100
 *     - SCT-013-030 → CT_RATIO=30,   BURDEN_OHMS=1  (built-in burden)
 *
 *  2. Current calibration:
 *     - Connect known load (100W bulb = 0.43A at 230V)
 *     - If reading is wrong, scale CT_RATIO up/down accordingly
 *
 *  3. Voltage calibration:
 *     - Check Serial Monitor vs real multimeter reading
 *     - new VOLTAGE_FACTOR = old × (actual_V / displayed_V)
 *
 *  4. Zero noise:
 *     - With no load, current should read 0.00A
 *     - If it shows 0.10–0.15A, adjust the 0.10f noise floor threshold
 * ================================================================
 */

