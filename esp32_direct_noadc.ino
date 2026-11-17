/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║           MQTT SETUP — DO THIS BEFORE UPLOADING         ║
 * ╠══════════════════════════════════════════════════════════╣
 * ║                                                          ║
 * ║  STEP 1 — Install Mosquitto Broker on your PC            ║
 * ║    Download: https://mosquitto.org/download/             ║
 * ║    Install with default settings                         ║
 * ║                                                          ║
 * ║  STEP 2 — Edit mosquitto.conf (Notepad as Admin)         ║
 * ║    File: C:\Program Files\mosquitto\mosquitto.conf       ║
 * ║    Add these 4 lines at the bottom:                      ║
 * ║      listener 1883                                       ║
 * ║      allow_anonymous true                                ║
 * ║      listener 9001                                       ║
 * ║      protocol websockets                                 ║
 * ║                                                          ║
 * ║  STEP 3 — Restart Mosquitto (CMD as Admin):              ║
 * ║      net stop mosquitto                                  ║
 * ║      net start mosquitto                                 ║
 * ║                                                          ║
 * ║  STEP 4 — Open Firewall ports (CMD as Admin):            ║
 * ║      netsh advfirewall firewall add rule                 ║
 * ║        name="MQTT 1883" protocol=TCP                     ║
 * ║        dir=in localport=1883 action=allow                ║
 * ║      netsh advfirewall firewall add rule                 ║
 * ║        name="MQTT WS 9001" protocol=TCP                  ║
 * ║        dir=in localport=9001 action=allow                ║
 * ║                                                          ║
 * ║  STEP 5 — Find your PC IP address (CMD):                 ║
 * ║      ipconfig                                            ║
 * ║    Look for IPv4 Address (e.g. 192.168.1.105)            ║
 * ║                                                          ║
 * ║  STEP 6 — Update MQTT_BROKER below with your PC IP       ║
 * ║    const char* MQTT_BROKER = "192.168.1.105"; ← your IP ║
 * ║                                                          ║
 * ║  STEP 7 — Upload this code → Open Serial Monitor         ║
 * ║    Baud: 115200                                          ║
 * ║    Should show: [MQTT] Connecting ... ✔ OK               ║
 * ║    Then: Published OK every 2 seconds                    ║
 * ║                                                          ║
 * ║  FULL GUIDE: http://localhost/Electricity/mqtt_setup.html║
 * ╚══════════════════════════════════════════════════════════╝
 */

/*
 * ============================================================
 * Smart Power Theft Detection System
 * CT SENSOR VERSION — 2-Wire Current Transformer
 *
 * Uses ESP32 built-in 12-bit ADC directly
 * ============================================================
 *
 * HARDWARE REQUIRED:
 *   1. ESP32 Dev Board
 *   2. CT Current Sensor (2-wire: Red + Black) × 2
 *   3. ZMPT101B AC Voltage Sensor Module × 1
 *   4. 100Ω resistor × 2  (burden resistor — ONE per CT sensor)
 *   5. 5V SMPS Power Supply
 *   6. 6A MCB for protection
 *
 * ⚠️  WARNING: NEVER connect CT sensor without burden resistor!
 *     Open-circuit CT generates dangerously high voltage.
 *
 * WIRING — CT Sensor A (Supply side):
 * ─────────────────────────────────────────────────────
 *   CT Red  ──┬──────────── ESP32 GPIO34
 *             │
 *           100Ω  ← burden resistor (REQUIRED!)
 *             │
 *   CT Black ─┴──────────── ESP32 GND
 *
 *   [AC Phase wire passes THROUGH the CT sensor ring/clamp]
 *
 * WIRING — CT Sensor B (Load side):
 * ─────────────────────────────────────────────────────
 *   CT Red  ──┬──────────── ESP32 GPIO35
 *             │
 *           100Ω  ← burden resistor (REQUIRED!)
 *             │
 *   CT Black ─┴──────────── ESP32 GND
 *
 * WIRING — ZMPT101B Voltage Module:
 *   VCC  → ESP32 5V (VIN pin)
 *   GND  → ESP32 GND
 *   OUT  → ESP32 GPIO32
 *   [Connected in parallel across AC Phase and Neutral]
 *
 * HOW CT SENSOR MATH WORKS:
 *   CT ratio (e.g. 1000:1) means 1A primary → 1mA secondary
 *   Burden resistor converts: I_secondary → V_burden
 *   V_burden = I_secondary × R_burden
 *   Primary current = V_burden(rms) / R_burden × CT_RATIO
 *
 * LIBRARIES NEEDED:
 *   - PubSubClient by Nick O'Leary
 *   - WiFi (built-in with ESP32 board package)
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

// ─────────────────────────────────────────────────────────────
//  CONFIGURATION — EDIT THESE
// ─────────────────────────────────────────────────────────────

const char* WIFI_SSID    = "Thakre";
const char* WIFI_PASS    = "KRISHNA2277";

const char* MQTT_BROKER  = "192.168.137.1";  // PC Hotspot IP (from ipconfig)
const int   MQTT_PORT    = 1883;
const char* CLIENT_ID    = "ESP32_PowerTheft_Direct";

// MQTT Topics
const char* TOPIC_A      = "powertheft/moduleA/data";
const char* TOPIC_B      = "powertheft/moduleB/data";
const char* TOPIC_ALERT  = "powertheft/alert";
const char* TOPIC_STATUS = "powertheft/status";

// ── ADC PINS ──────────────────────────────────────────────────
#define PIN_CURRENT_A   34    // CT Sensor A output (via 100Ω burden)
#define PIN_CURRENT_B   35    // CT Sensor B output (via 100Ω burden)
#define PIN_VOLTAGE     32    // ZMPT101B output

// ── ADC SETTINGS ─────────────────────────────────────────────
// ESP32 ADC: 12-bit (0-4095), reference ≈ 3.3V
// 1 LSB ≈ 0.8056 mV
#define ADC_VREF        3300.0f   // mV
#define ADC_BITS        4095.0f   // 12-bit

// ── CT SENSOR CALIBRATION ─────────────────────────────────────
// Formula: I_primary = V_rms_on_burden / BURDEN_OHMS * CT_RATIO
//
// Common CT sensor ratios:
//   SCT-013-030 (30A/1V) : ratio=30,  burden built-in (no external needed)
//   SCT-013-000 (raw)    : ratio=2000, use 100Ω burden
//   Generic 1000:1 CT    : ratio=1000, use 100Ω burden
//
// ADJUST CT_RATIO to match your sensor's label:
#define CT_RATIO        1000.0f   // turns ratio (primary:secondary) — CHECK YOUR SENSOR
#define BURDEN_OHMS     100.0f    // your burden resistor value in Ohms
// Derived calibration factor (do not change this line):
#define CT_CALIB  (CT_RATIO / BURDEN_OHMS)  // = Amps per Volt on burden resistor

// ── ZMPT101B CALIBRATION ─────────────────────────────────────
// ZMPT101B module powered from 5V, output swings around midpoint.
// Adjust VOLTAGE_FACTOR until displayed value matches your multimeter.
// Start with 1.0 and tune after first measurement.
#define ZMPT_MIDPOINT   2047.0f   // ADC count at 0V AC (adjust if needed)
#define VOLTAGE_FACTOR  0.71f     // Multiply raw RMS count → Volts (tune this!)

// ── THEFT THRESHOLD ───────────────────────────────────────────
float THEFT_THRESHOLD = 0.50f;   // Amps — trigger alert if diff > this

// ── SAMPLING ─────────────────────────────────────────────────
#define SAMPLES          2000     // ADC samples for RMS (covers multiple AC cycles)
#define PUBLISH_MS       2000     // Publish every 2 seconds

// ─────────────────────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────────────────────
WiFiClient   espClient;
PubSubClient mqtt(espClient);

float voltageRMS  = 0.0f;
float currentA    = 0.0f;
float currentB    = 0.0f;
float powerA      = 0.0f;
float powerB      = 0.0f;
float diffCurrent = 0.0f;
bool  theftDetected = false;
int   alertCount  = 0;

unsigned long lastPublish    = 0;
unsigned long lastReconnect  = 0;

// ─────────────────────────────────────────────────────────────
//  CONFIGURE ADC ATTENUATION
// ─────────────────────────────────────────────────────────────
void configureADC() {
    // Set 11dB attenuation → full 0–3.3V input range
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);   // 12-bit = 0–4095
    Serial.println("[ADC] Configured: 12-bit, 11dB attenuation, 0-3.3V range");
}

// ─────────────────────────────────────────────────────────────
//  READ RMS CURRENT — 2-Wire CT Sensor (with 100Ω burden)
// ─────────────────────────────────────────────────────────────
// CT sensor outputs AC current → burden resistor converts to AC voltage
// The voltage swings symmetrically around 0V (no DC bias needed!)
// Two-pass: find auto-midpoint first, then compute RMS
// ─────────────────────────────────────────────────────────────
float readCurrentRMS(int pin, float calibFactor) {
    long   sumSquares = 0;
    long   sumRaw     = 0;
    int    sample;

    // ── Pass 1: find actual DC midpoint (auto-zero) ─────────
    for (int i = 0; i < SAMPLES; i++) {
        sample = analogRead(pin);
        sumRaw += sample;
        delayMicroseconds(50);   // ~20 kHz > 50 Hz AC
    }
    float midpoint = (float)sumRaw / SAMPLES;

    // ── Pass 2: compute RMS around midpoint ─────────────────
    for (int i = 0; i < SAMPLES; i++) {
        sample = analogRead(pin);
        float centered = (float)sample - midpoint;
        sumSquares += (long)(centered * centered);
        delayMicroseconds(50);
    }

    float rmsCount  = sqrtf((float)sumSquares / SAMPLES);
    float rmsVolt_V = (rmsCount / ADC_BITS) * (ADC_VREF / 1000.0f);  // Convert to Volts

    // CT formula: I_primary = V_burden_rms × (CT_ratio / burden_ohms)
    float current = rmsVolt_V * calibFactor;

    // Noise floor suppression (ADC noise reads ~0.05A)
    if (current < 0.10f) current = 0.0f;

    return current;
}

// ─────────────────────────────────────────────────────────────
//  READ RMS VOLTAGE — ZMPT101B Module (direct ADC)
// ─────────────────────────────────────────────────────────────
float readVoltageRMS() {
    long  sumSquares = 0;
    long  sumRaw     = 0;
    int   sample;

    // Pass 1: midpoint
    for (int i = 0; i < SAMPLES; i++) {
        sample = analogRead(PIN_VOLTAGE);
        sumRaw += sample;
        delayMicroseconds(50);
    }
    float midpoint = (float)sumRaw / SAMPLES;

    // Pass 2: RMS
    for (int i = 0; i < SAMPLES; i++) {
        sample = analogRead(PIN_VOLTAGE);
        float centered = (float)sample - midpoint;
        sumSquares += (long)(centered * centered);
        delayMicroseconds(50);
    }

    float rmsCount = sqrtf((float)sumSquares / SAMPLES);

    // Apply calibration factor to get actual Volts
    float voltage  = rmsCount * VOLTAGE_FACTOR;

    // Sanity range check
    if (voltage < 50.0f || voltage > 280.0f) voltage = 0.0f;

    return voltage;
}

// ─────────────────────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
    Serial.printf("\n[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500); Serial.print("."); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] ✔ Connected! IP: %s  RSSI: %d dBm\n",
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("\n[WiFi] ✖ Failed — restarting ESP32");
        delay(2000); ESP.restart();
    }
}

// ─────────────────────────────────────────────────────────────
//  MQTT
// ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    String msg = ""; for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    Serial.printf("[MQTT] Received [%s]: %s\n", topic, msg.c_str());
}

bool connectMQTT() {
    if (mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
    if (mqtt.connect(CLIENT_ID)) {
        Serial.println(" ✔ OK");
        mqtt.publish(TOPIC_STATUS, "{\"status\":\"online\"}");
        return true;
    }
    Serial.printf(" ✖ Failed RC=%d\n", mqtt.state());
    return false;
}

// ─────────────────────────────────────────────────────────────
//  BUILD JSON
// ─────────────────────────────────────────────────────────────
String jsonModuleA() {
    char b[200];
    snprintf(b, sizeof(b),
        "{\"device\":\"A\",\"voltage\":%.2f,\"current\":%.4f,\"power\":%.2f,\"ts\":%lu}",
        voltageRMS, currentA, powerA, millis());
    return String(b);
}
String jsonModuleB() {
    char b[200];
    snprintf(b, sizeof(b),
        "{\"device\":\"B\",\"voltage\":%.2f,\"current\":%.4f,\"power\":%.2f,\"ts\":%lu}",
        voltageRMS, currentB, powerB, millis());
    return String(b);
}
String jsonAlert() {
    char b[256];
    snprintf(b, sizeof(b),
        "{\"alert\":%s,\"diff\":%.4f,\"currentA\":%.4f,\"currentB\":%.4f,"
        "\"voltage\":%.2f,\"threshold\":%.2f,\"count\":%d,\"ts\":%lu}",
        theftDetected ? "true" : "false",
        diffCurrent, currentA, currentB,
        voltageRMS, THEFT_THRESHOLD, alertCount, millis());
    return String(b);
}

// ─────────────────────────────────────────────────────────────
//  SERIAL PRINT
// ─────────────────────────────────────────────────────────────
void printReadings() {
    Serial.println("══════════════════════════════════════════");
    Serial.printf("  Voltage   : %6.2f V\n",  voltageRMS);
    Serial.printf("  Current A : %6.4f A  ← Supply\n", currentA);
    Serial.printf("  Current B : %6.4f A  ← Load\n",   currentB);
    Serial.printf("  Diff(A-B) : %6.4f A  (Threshold: %.2fA)\n", diffCurrent, THEFT_THRESHOLD);
    Serial.printf("  Power A   : %6.2f W\n",  powerA);
    Serial.printf("  Power B   : %6.2f W\n",  powerB);
    Serial.printf("  STATUS    : %s\n", theftDetected ? "⚠  THEFT DETECTED!" : "✔  NORMAL");
    Serial.println("══════════════════════════════════════════");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(800);

    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║  Power Theft Detection — Direct Connect  ║");
    Serial.println("║  ESP32 + ACS712 x2 + ZMPT101B (No ADC)  ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.println();

    // Configure ADC
    configureADC();

    // Print pin config
    Serial.println("[PINS] Current A → GPIO34");
    Serial.println("[PINS] Current B → GPIO35");
    Serial.println("[PINS] Voltage   → GPIO32");
    Serial.println();

    // Quick ADC sanity test
    Serial.printf("[ADC] GPIO34 raw: %d\n", analogRead(PIN_CURRENT_A));
    Serial.printf("[ADC] GPIO35 raw: %d\n", analogRead(PIN_CURRENT_B));
    Serial.printf("[ADC] GPIO32 raw: %d\n", analogRead(PIN_VOLTAGE));
    Serial.println();

    // WiFi
    connectWiFi();

    // MQTT
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(384);
    connectMQTT();

    Serial.println("[SYSTEM] Ready — measuring every 2 seconds\n");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
    // Keep WiFi alive
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Reconnecting…");
        connectWiFi();
    }

    // Keep MQTT alive
    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnect > 5000) {
            lastReconnect = now;
            connectMQTT();
        }
    }
    mqtt.loop();

    // Measure & publish every PUBLISH_MS
    if (millis() - lastPublish >= PUBLISH_MS) {
        lastPublish = millis();

        //── Read sensors ──────────────────────────────────────
        voltageRMS = readVoltageRMS();
        currentA   = readCurrentRMS(PIN_CURRENT_A, CT_CALIB);
        currentB   = readCurrentRMS(PIN_CURRENT_B, CT_CALIB);

        //── Compute power ─────────────────────────────────────
        powerA = voltageRMS * currentA;
        powerB = voltageRMS * currentB;

        //── Theft detection ───────────────────────────────────
        diffCurrent = currentA - currentB;
        bool prev   = theftDetected;
        theftDetected = (diffCurrent > THEFT_THRESHOLD);
        if (theftDetected && !prev) alertCount++;

        //── Serial output ─────────────────────────────────────
        printReadings();

        //── Publish MQTT ──────────────────────────────────────
        if (mqtt.connected()) {
            mqtt.publish(TOPIC_A,     jsonModuleA().c_str(), true);
            mqtt.publish(TOPIC_B,     jsonModuleB().c_str(), true);
            mqtt.publish(TOPIC_ALERT, jsonAlert().c_str(),   true);
            if (theftDetected)
                mqtt.publish("powertheft/alarm",
                    "{\"alarm\":\"THEFT_DETECTED\",\"sev\":\"HIGH\"}");
            Serial.println("[MQTT] Published OK");
        }
    }

    delay(10);
}

/*
 * ============================================================
 *  CALIBRATION GUIDE — CT SENSOR VERSION
 * ============================================================
 *
 * STEP 1 — Identify your CT sensor ratio:
 *  Check the label on your CT sensor body:
 *  - "1000:1" or "1000A:1mA" → CT_RATIO = 1000
 *  - "2000:1"                → CT_RATIO = 2000
 *  - "SCT-013-030" (30A/1V)  → CT_RATIO = 30, BURDEN_OHMS = 1
 *    (SCT-013-030 has built-in burden — no external resistor!)
 *  Set CT_RATIO and BURDEN_OHMS above accordingly.
 *
 * STEP 2 — Current Calibration:
 *  a) Connect a KNOWN load (e.g. 100W bulb = 0.43A at 230V)
 *  b) Open Serial Monitor at 115200 baud
 *  c) Check displayed current vs known value
 *  d) If displayed = 0.86A but actual = 0.43A:
 *       CT_RATIO is 2× too high → divide CT_RATIO by 2
 *  e) If displayed = 0.22A but actual = 0.43A:
 *       CT_RATIO is too low → multiply CT_RATIO by 2
 *
 * STEP 3 — Voltage Calibration (ZMPT101B):
 *  a) Measure real voltage with a multimeter (e.g. 231V)
 *  b) Check Serial Monitor displayed voltage
 *  c) Adjust VOLTAGE_FACTOR:
 *       new_factor = VOLTAGE_FACTOR × (actual_V / displayed_V)
 *
 * STEP 4 — Zero current (no load test):
 *  a) Clamp CT on wire with NO current flowing
 *  b) Should read 0.00A. If it reads 0.10-0.15A → adjust
 *     the noise floor: change 0.10f in readCurrentRMS to match
 *
 * STEP 5 — Theft threshold:
 *  a) Put both CT sensors on the SAME wire → diff must be < 0.1A
 *  b) Move sensor B to the load side → add illegal load on supply
 *     side → diff should exceed 0.5A → THEFT alert triggers
 *
 * COMMON CT SENSOR RATIOS:
 *  Sensor Label  | CT_RATIO | BURDEN_OHMS
 *  1000:1        |   1000   |    100
 *  2000:1        |   2000   |    100
 *  SCT-013-000   |   2000   |    100  (no built-in burden)
 *  SCT-013-030   |     30   |      1  (has built-in 62Ω burden!)
 *  SCT-013-020   |     20   |      1  (has built-in 62Ω burden!)
 * ============================================================
 */
