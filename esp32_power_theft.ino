/*
 * ============================================================
 * Smart Power Theft Detection System
 * ESP32 Dev Board - Dual CT Sensor + ZMPT101B + ADS1115
 * Protocol: WiFi + MQTT
 * ============================================================
 *
 * HARDWARE CONNECTIONS:
 *  ADS1115  -->  ESP32
 *  VDD      -->  3.3V
 *  GND      -->  GND
 *  SCL      -->  GPIO22
 *  SDA      -->  GPIO21
 *  ADDR     -->  GND  (I2C address 0x48)
 *  A0       -->  CT Sensor A (Supply side)
 *  A1       -->  CT Sensor B (Load side)
 *  A2       -->  ZMPT101B Voltage Sensor
 *  A3       -->  (spare)
 *
 * BIAS CIRCUIT (per CT sensor):
 *  CT output --> 10 ohm burden resistor to GND
 *  2x 10k voltage divider from 3.3V to GND; midpoint = 1.65V bias
 *  100uF cap from bias point to GND (smoothing)
 *  ADS1115 input connected after burden resistor & bias
 *
 * REQUIRED LIBRARIES:
 *  - PubSubClient by Nick O'Leary
 *  - Adafruit ADS1X15
 *  - Adafruit BusIO
 *  - ArduinoJson (optional, for JSON payload)
 *
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <math.h>

// ─────────────────────────────────────────────────────────────
//  CONFIGURATION — EDIT THESE VALUES
// ─────────────────────────────────────────────────────────────

// WiFi credentials
const char* WIFI_SSID     = "Thakre";
const char* WIFI_PASS     = "KRISHNA2277";

// MQTT Broker
const char* MQTT_BROKER   = "192.168.1.100";   // Replace with your broker IP
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";                 // Leave empty if no auth
const char* MQTT_PASS_    = "";
const char* CLIENT_ID     = "ESP32_PowerTheft";

// MQTT Topics
const char* TOPIC_MODULE_A  = "powertheft/moduleA/data";
const char* TOPIC_MODULE_B  = "powertheft/moduleB/data";
const char* TOPIC_ALERT     = "powertheft/alert";
const char* TOPIC_STATUS    = "powertheft/status";

// Theft detection threshold (Amps)
const float THEFT_THRESHOLD = 0.50;            // 0.5A difference triggers alert

// Calibration factors (adjust after calibration procedure)
float VOLTAGE_CALIBRATION   = 234.26;          // ZMPT101B: multiply raw RMS to get Volts
float CURRENT_CAL_A         = 30.0;            // SCT-013: turns ratio / burden (30A/1V with 33ohm)
float CURRENT_CAL_B         = 30.0;            // SCT-013 module B

// Sampling
const int SAMPLES           = 1000;            // Number of ADC samples per RMS calculation
const int PUBLISH_INTERVAL  = 2000;            // Publish data every 2 seconds (ms)

// ─────────────────────────────────────────────────────────────
//  GLOBAL OBJECTS & VARIABLES
// ─────────────────────────────────────────────────────────────

Adafruit_ADS1115 ads;
WiFiClient       espClient;
PubSubClient     mqttClient(espClient);

float voltageRMS  = 0.0;
float currentA    = 0.0;
float currentB    = 0.0;
float powerA      = 0.0;
float powerB      = 0.0;
float diffCurrent = 0.0;
bool  theftDetected = false;

unsigned long lastPublish     = 0;
unsigned long lastReconnect   = 0;
int           alertCount      = 0;

// ADS1115 channel definitions
#define ADC_CURRENT_A   0   // ADS1115 A0
#define ADC_CURRENT_B   1   // ADS1115 A1
#define ADC_VOLTAGE     2   // ADS1115 A2

// ─────────────────────────────────────────────────────────────
//  FUNCTION: WiFi Connection
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
    Serial.printf("\n[WiFi] Connecting to SSID: %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] Signal Strength: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\n[WiFi] Connection FAILED! Restarting...");
        delay(3000);
        ESP.restart();
    }
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: MQTT Callback (for subscribed topics)
// ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    Serial.printf("[MQTT] Received [%s]: %s\n", topic, msg.c_str());
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: MQTT Reconnection
// ─────────────────────────────────────────────────────────────
bool connectMQTT() {
    if (mqttClient.connected()) return true;

    Serial.printf("[MQTT] Connecting to broker %s:%d ...", MQTT_BROKER, MQTT_PORT);

    bool connected;
    if (strlen(MQTT_USER) > 0) {
        connected = mqttClient.connect(CLIENT_ID, MQTT_USER, MQTT_PASS_);
    } else {
        connected = mqttClient.connect(CLIENT_ID);
    }

    if (connected) {
        Serial.println(" Connected!");
        // Publish online status
        mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\",\"device\":\"ESP32_PowerTheft\"}");
        // Subscribe to alert acknowledgement
        mqttClient.subscribe("powertheft/ack");
        return true;
    } else {
        Serial.printf(" Failed! RC=%d\n", mqttClient.state());
        return false;
    }
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: Read RMS Current from ADS1115 channel
// ─────────────────────────────────────────────────────────────
float readCurrentRMS(int channel, float calibration) {
    long   sumSquares = 0;
    int16_t sample;
    int16_t offset = 0;

    // First pass: compute DC offset (mean)
    long sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sample = ads.readADC_SingleEnded(channel);
        sum += sample;
    }
    offset = sum / SAMPLES;

    // Second pass: compute RMS around offset
    for (int i = 0; i < SAMPLES; i++) {
        sample = ads.readADC_SingleEnded(channel);
        long centered = (long)sample - offset;
        sumSquares += centered * centered;
    }

    float rmsRaw = sqrt((float)sumSquares / SAMPLES);

    // Convert raw ADC units to millivolts (ADS1115 @ ±4.096V, PGA=1)
    // 1 LSB = 0.125 mV at ±4.096V gain with 16-bit
    float rmsVoltage = rmsRaw * 0.125;   // mV

    // Convert mV to current using calibration (SCT-013-030: 30A/1V)
    float current = (rmsVoltage / 1000.0) * calibration;

    // Noise floor filter — small values treated as zero
    if (current < 0.05) current = 0.0;

    return current;
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: Read RMS Voltage from ADS1115
// ─────────────────────────────────────────────────────────────
float readVoltageRMS() {
    long   sumSquares = 0;
    int16_t sample;

    // Compute DC offset
    long sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sample = ads.readADC_SingleEnded(ADC_VOLTAGE);
        sum += sample;
    }
    int16_t offset = sum / SAMPLES;

    // RMS
    for (int i = 0; i < SAMPLES; i++) {
        sample = ads.readADC_SingleEnded(ADC_VOLTAGE);
        long centered = (long)sample - offset;
        sumSquares += centered * centered;
    }

    float rmsRaw  = sqrt((float)sumSquares / SAMPLES);
    float rmsVolt = rmsRaw * 0.125;   // mV raw output of ZMPT101B

    // Apply calibration multiplier (calibrate to match known mains voltage)
    float voltage = rmsVolt * VOLTAGE_CALIBRATION / 1000.0;

    // Sanity check
    if (voltage < 50.0 || voltage > 280.0) voltage = 0.0;

    return voltage;
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: Build JSON payload
// ─────────────────────────────────────────────────────────────
String buildModuleAJson() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"ModuleA\","
        "\"voltage\":%.2f,"
        "\"current\":%.4f,"
        "\"power\":%.2f,"
        "\"timestamp\":%lu}",
        voltageRMS, currentA, powerA, millis());
    return String(buf);
}

String buildModuleBJson() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"ModuleB\","
        "\"voltage\":%.2f,"
        "\"current\":%.4f,"
        "\"power\":%.2f,"
        "\"timestamp\":%lu}",
        voltageRMS, currentB, powerB, millis());
    return String(buf);
}

String buildAlertJson(bool detected) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"alert\":%s,"
        "\"difference\":%.4f,"
        "\"currentA\":%.4f,"
        "\"currentB\":%.4f,"
        "\"voltage\":%.2f,"
        "\"threshold\":%.2f,"
        "\"count\":%d,"
        "\"timestamp\":%lu}",
        detected ? "true" : "false",
        diffCurrent, currentA, currentB,
        voltageRMS, THEFT_THRESHOLD,
        alertCount, millis());
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
//  FUNCTION: Print to Serial Monitor
// ─────────────────────────────────────────────────────────────
void printReadings() {
    Serial.println("─────────────────────────────────────────");
    Serial.printf("  Voltage     : %.2f V\n",  voltageRMS);
    Serial.printf("  Current A   : %.4f A  (Supply Side)\n", currentA);
    Serial.printf("  Current B   : %.4f A  (Load Side)\n",   currentB);
    Serial.printf("  Difference  : %.4f A\n", diffCurrent);
    Serial.printf("  Power A     : %.2f W\n", powerA);
    Serial.printf("  Power B     : %.2f W\n", powerB);
    Serial.printf("  THEFT       : %s\n", theftDetected ? "*** DETECTED ***" : "NORMAL");
    Serial.println("─────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("╔═══════════════════════════════════════╗");
    Serial.println("║  Smart Power Theft Detection System   ║");
    Serial.println("║  ESP32 + ADS1115 + SCT-013 + ZMPT101B ║");
    Serial.println("╚═══════════════════════════════════════╝");

    // Initialize I2C
    Wire.begin(21, 22);   // SDA=GPIO21, SCL=GPIO22
    Serial.println("[I2C] Initialized on GPIO21/22");

    // Initialize ADS1115
    if (!ads.begin()) {
        Serial.println("[ADS1115] ERROR: Sensor not found! Check wiring.");
        while (1) { delay(100); }
    }
    Serial.println("[ADS1115] Initialized successfully");

    // Set gain: GAIN_ONE = ±4.096V range → 0.125mV/bit
    // This is safe for CT sensor outputs with 10 ohm burden (max ~±3V)
    ads.setGain(GAIN_ONE);
    Serial.println("[ADS1115] Gain set to GAIN_ONE (±4.096V)");

    // Set data rate (samples per second)
    ads.setDataRate(RATE_ADS1115_860SPS);   // 860 samples/sec for fast RMS

    // Connect WiFi
    connectWiFi();

    // Setup MQTT
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);

    // Connect MQTT
    connectMQTT();

    Serial.println("[SYSTEM] Ready — Starting measurements...\n");
    delay(1000);
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
    // Maintain WiFi
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Disconnected! Reconnecting...");
        connectWiFi();
    }

    // Maintain MQTT with non-blocking reconnect
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnect > 5000) {
            lastReconnect = now;
            connectMQTT();
        }
    }
    mqttClient.loop();

    // Read and publish at interval
    unsigned long currentTime = millis();
    if (currentTime - lastPublish >= PUBLISH_INTERVAL) {
        lastPublish = currentTime;

        // ── Read sensors ──────────────────────────────────
        voltageRMS = readVoltageRMS();
        currentA   = readCurrentRMS(ADC_CURRENT_A, CURRENT_CAL_A);
        currentB   = readCurrentRMS(ADC_CURRENT_B, CURRENT_CAL_B);

        // ── Calculate power ───────────────────────────────
        powerA = voltageRMS * currentA;
        powerB = voltageRMS * currentB;

        // ── Theft detection ───────────────────────────────
        diffCurrent  = currentA - currentB;
        bool prevTheft = theftDetected;
        theftDetected  = (diffCurrent > THEFT_THRESHOLD);

        if (theftDetected && !prevTheft) {
            alertCount++;   // Count new theft events
        }

        // ── Print to Serial ───────────────────────────────
        printReadings();

        // ── Publish to MQTT ───────────────────────────────
        if (mqttClient.connected()) {
            // Module A data
            String payloadA = buildModuleAJson();
            bool okA = mqttClient.publish(TOPIC_MODULE_A, payloadA.c_str(), true);
            Serial.printf("[MQTT] Published ModuleA: %s\n", okA ? "OK" : "FAIL");

            // Module B data
            String payloadB = buildModuleBJson();
            bool okB = mqttClient.publish(TOPIC_MODULE_B, payloadB.c_str(), true);
            Serial.printf("[MQTT] Published ModuleB: %s\n", okB ? "OK" : "FAIL");

            // Alert topic
            String alertPayload = buildAlertJson(theftDetected);
            bool okAlert = mqttClient.publish(TOPIC_ALERT, alertPayload.c_str(), true);
            Serial.printf("[MQTT] Published Alert: %s\n", okAlert ? "OK" : "FAIL");

            if (theftDetected) {
                // Flash extra message on theft
                mqttClient.publish("powertheft/alarm",
                    "{\"alarm\":\"THEFT_DETECTED\",\"severity\":\"HIGH\"}");
            }
        } else {
            Serial.println("[MQTT] Not connected — skipping publish");
        }
    }

    delay(10);  // Yield to ESP32 background tasks
}

// ─────────────────────────────────────────────────────────────
//  END OF FILE
// ─────────────────────────────────────────────────────────────
