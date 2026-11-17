/*
 * ================================================================
 *  METER B — Consumer / House Side
 *  Smart Electricity Theft Detection System
 *
 *  Identical hardware as Meter A but different MQTT topics.
 *  Dashboard compares both meters and triggers theft alert.
 *
 *  WIRING: Same as Meter A (ADS1115 A0=CT, A1=ZMPT101B, GPIO26=Relay)
 * ================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <math.h>

// ── WiFi ─────────────────────────────────────────────────────
// ⚠ Connect ESP32 to PC HOTSPOT — same as Meter A
const char* WIFI_SSID   = "LAPTOP-5LJI46P9 1083"; // PC Hotspot name
const char* WIFI_PASS   = "12345678";              // PC Hotspot password

// ── MQTT ─────────────────────────────────────────────────────
const char* MQTT_BROKER = "192.168.137.1"; // ← PC Hotspot IP (FIXED forever)
const int   MQTT_PORT   = 1883;
const char* CLIENT_ID   = "ESP32_MeterB";

const char* TOPIC_DATA   = "meter/B/data";
const char* TOPIC_STATUS = "meter/B/status";
const char* TOPIC_RELAY  = "meter/B/relay";

// ── Pins ─────────────────────────────────────────────────────
#define RELAY_PIN   26
#define I2C_SDA     21
#define I2C_SCL     22

// ── ADS1115 Channels ─────────────────────────────────────────
#define CH_CURRENT  0
#define CH_VOLTAGE  1

// ── Calibration ──────────────────────────────────────────────
#define CT_RATIO      1000.0f  // ← ZMCT103C / ZHT103 (1000:1)
#define BURDEN_OHMS   200.0f   // ← 200Ω external burden resistor
#define VOLTS_PER_BIT 0.0001250f
#define VOLTAGE_FACTOR 0.544f  // ← FINAL calibrated (same as Meter A)

// ── Sampling ─────────────────────────────────────────────────
#define SAMPLES     200
#define PUBLISH_MS  2000

// ── Objects ──────────────────────────────────────────────────
Adafruit_ADS1115 ads;
WiFiClient       espClient;
PubSubClient     mqtt(espClient);

// ── State ────────────────────────────────────────────────────
float    voltRMS = 0, currRMS = 0, power = 0, energy = 0;
bool     relayON = false;
unsigned long lastPublish = 0, lastReconnect = 0, energyTimer = 0;

// ─────────────────────────────────────────────────────────────
float readCurrentRMS() {
    float sumRaw = 0, sumSq = 0;
    for (int i = 0; i < SAMPLES; i++) sumRaw += ads.readADC_SingleEnded(CH_CURRENT);
    float mid = sumRaw / SAMPLES;
    for (int i = 0; i < SAMPLES; i++) {
        float v = ads.readADC_SingleEnded(CH_CURRENT) - mid;
        sumSq += v * v;
    }
    float rmsVolt = sqrtf(sumSq / SAMPLES) * VOLTS_PER_BIT;
    float current = (rmsVolt / BURDEN_OHMS) * CT_RATIO;
    return (current < 0.15f) ? 0.0f : current;  // 0.15A noise floor (EMI suppression)
}

float readVoltageRMS() {
    float sumRaw = 0, sumSq = 0;
    for (int i = 0; i < SAMPLES; i++) sumRaw += ads.readADC_SingleEnded(CH_VOLTAGE);
    float mid = sumRaw / SAMPLES;
    for (int i = 0; i < SAMPLES; i++) {
        float v = ads.readADC_SingleEnded(CH_VOLTAGE) - mid;
        sumSq += v * v;
    }
    float voltage = sqrtf(sumSq / SAMPLES) * VOLTS_PER_BIT * 1000.0f * VOLTAGE_FACTOR;
    return (voltage < 30.0f || voltage > 290.0f) ? 0.0f : voltage;
}

void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t++ < 40) { delay(500); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] ✔ IP: %s\n", WiFi.localIP().toString().c_str());
    } else { Serial.println("\n[WiFi] ✖ Failed — restarting"); delay(2000); ESP.restart(); }
}

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

bool connectMQTT() {
    if (mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
    if (mqtt.connect(CLIENT_ID, nullptr, nullptr, TOPIC_STATUS, 1, true, "{\"online\":false}")) {
        Serial.println(" ✔ OK");
        mqtt.subscribe(TOPIC_RELAY);
        mqtt.publish(TOPIC_STATUS, "{\"online\":true}", true);
        return true;
    }
    Serial.printf(" ✖ RC=%d\n", mqtt.state()); return false;
}

void publishData() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"B\",\"voltage\":%.2f,\"current\":%.4f,"
        "\"power\":%.2f,\"energy\":%.5f,\"relay\":%s,\"ts\":%lu}",
        voltRMS, currRMS, power, energy, relayON ? "true" : "false", millis());
    mqtt.publish(TOPIC_DATA, buf, true);
    Serial.println("[MQTT] Published → meter/B/data");
}

void setup() {
    Serial.begin(115200); delay(600);
    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  METER B — Consumer / House Side  ║");
    Serial.println("║  Power Theft Detection System     ║");
    Serial.println("╚═══════════════════════════════════╝\n");

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!ads.begin()) { Serial.println("[ADS] ✖ ADS1115 not found!"); while (1); }
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_860SPS);
    Serial.println("[ADS] ✔ ADS1115 ready");

    connectWiFi();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(384);
    connectMQTT();

    energyTimer = millis();
    Serial.println("[SYS] Ready — publishing every 2 seconds\n");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

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

        unsigned long now = millis();
        energy += power * ((now - energyTimer) / 3600000.0f) / 1000.0f;
        energyTimer = now;

        Serial.println("══════════════════════════════════════");
        Serial.printf(" [B] Voltage : %7.2f V\n", voltRMS);
        Serial.printf(" [B] Current : %7.4f A\n", currRMS);
        Serial.printf(" [B] Power   : %7.2f W\n", power);
        Serial.printf(" [B] Energy  : %7.5f kWh\n", energy);
        Serial.printf(" [B] Relay   : %s\n", relayON ? "ON" : "OFF");
        Serial.println("══════════════════════════════════════");

        if (mqtt.connected()) publishData();
    }
    delay(10);
}

