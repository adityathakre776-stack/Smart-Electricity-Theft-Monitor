/*
 * ================================================================
 *  ZMPT101B AC Voltage Sensor — Test Code for ESP32
 *  
 *  PURPOSE: Verify ZMPT101B is working before integrating
 *           into the full theft detection system.
 *
 *  DIRECT CONNECTION (no ADS1115 needed for this test):
 *  ┌─────────────────────────────────┐
 *  │  ZMPT101B     →    ESP32        │
 *  │  VCC          →    VIN (5V)     │
 *  │  GND          →    GND          │
 *  │  OUT          →    GPIO34       │
 *  │  AC IN+       →    Phase (⚠230V)│
 *  │  AC IN-       →    Neutral(⚠)  │
 *  └─────────────────────────────────┘
 *
 *  Use GPIO34, 35, 36, or 39 — these are INPUT-ONLY analog pins
 *  (best for AC sensing, no internal conflicts)
 *
 *  SERIAL MONITOR: Set to 115200 baud
 * ================================================================
 */

// ── Pin ──────────────────────────────────────────────────────
#define ZMPT_PIN      34      // GPIO34 → ZMPT101B OUT

// ── Calibration ──────────────────────────────────────────────
// Start with 1.0, then adjust:
// new_factor = old_factor × (actual_voltage / displayed_voltage)
// e.g., if shows 198V but multimeter reads 220V:
// new_factor = 1.0 × (220 / 198) = 1.111
#define VOLTAGE_FACTOR  0.475f // ← calibrated value (0.44 × 1.08 = 0.475)

// ── Sampling ─────────────────────────────────────────────────
#define SAMPLES     1000      // number of ADC readings per RMS cycle
#define ADC_BITS    4095.0f  // ESP32 = 12-bit
#define VREF        3.3f     // ESP32 ADC reference voltage

// ─────────────────────────────────────────────────────────────
float readVoltageRMS() {
    float sumRaw  = 0;
    float sumSq   = 0;

    // Pass 1: find midpoint (DC offset from bias)
    for (int i = 0; i < SAMPLES; i++) {
        sumRaw += analogRead(ZMPT_PIN);
        delayMicroseconds(100);
    }
    float midpoint = sumRaw / SAMPLES;

    // Pass 2: calculate RMS around midpoint
    for (int i = 0; i < SAMPLES; i++) {
        float v = analogRead(ZMPT_PIN) - midpoint;
        sumSq += v * v;
        delayMicroseconds(100);
    }

    float rmsRaw   = sqrtf(sumSq / SAMPLES);
    float rmsVolts = (rmsRaw / ADC_BITS) * VREF;  // convert counts to volts

    // Apply calibration factor
    float voltage = rmsVolts * VOLTAGE_FACTOR * 1000.0f;
    // Note: ×1000 is a starting scale factor — adjust VOLTAGE_FACTOR to tune

    return voltage;
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║   ZMPT101B Voltage Sensor — Test Mode   ║");
    Serial.println("║   ESP32 GPIO34 → ZMPT OUT               ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.println();
    Serial.println("Wiring Check:");
    Serial.println("  ZMPT101B VCC  → ESP32 VIN (5V)");
    Serial.println("  ZMPT101B GND  → ESP32 GND");
    Serial.println("  ZMPT101B OUT  → ESP32 GPIO34");
    Serial.println("  ZMPT101B IN+  → AC Phase (230V) ⚠");
    Serial.println("  ZMPT101B IN-  → AC Neutral      ⚠");
    Serial.println();
    Serial.println("Starting readings... compare with your multimeter.");
    Serial.println("──────────────────────────────────────────");

    analogReadResolution(12);       // ESP32: set 12-bit ADC (0–4095)
    analogSetAttenuation(ADC_11db); // allows reading up to ~3.3V range

    delay(500);
}

// ─────────────────────────────────────────────────────────────
void loop() {
    int rawSample = analogRead(ZMPT_PIN);
    float voltage = readVoltageRMS();

    // ── Sensor health check ──────────────────────────────────
    String status;
    String indicator;

    if (rawSample < 10) {
        status    = "NO SIGNAL — Check: VCC connected? OUT wire connected?";
        indicator = "✖";
    } else if (rawSample > 4085) {
        status    = "SATURATED — Signal too high, check ZMPT101B trim pot";
        indicator = "⚠";
    } else if (voltage < 50.0f) {
        status    = "LOW — No AC connected or VOLTAGE_FACTOR needs tuning";
        indicator = "?";
    } else if (voltage >= 150.0f && voltage <= 260.0f) {
        status    = "NORMAL — Sensor working correctly ✔";
        indicator = "✔";
    } else {
        status    = "OUT OF RANGE — Adjust VOLTAGE_FACTOR to calibrate";
        indicator = "~";
    }

    // ── Serial output ────────────────────────────────────────
    Serial.println("══════════════════════════════════════════");
    Serial.printf("  Raw ADC Reading : %d  (0–4095)\n",    rawSample);
    Serial.printf("  Voltage RMS     : %.2f V\n",          voltage);
    Serial.printf("  Status          : %s  %s\n",          indicator.c_str(), status.c_str());
    Serial.println("══════════════════════════════════════════");
    Serial.println();

    // ── Calibration hint ─────────────────────────────────────
    if (voltage > 10.0f) {
        Serial.println("  📐 CALIBRATION HINT:");
        Serial.printf("     If multimeter reads 220V and this shows %.1fV:\n", voltage);
        float corrected = VOLTAGE_FACTOR * (220.0f / voltage); // correct formula
        Serial.printf("     Set VOLTAGE_FACTOR = %.4f  in the code\n\n", corrected);
    }

    delay(2000);
}

/*
 * ================================================================
 *  EXPECTED OUTPUT (good sensor):
 * ══════════════════════════════════════════
 *   Raw ADC Reading : 2048  (0–4095)
 *   Voltage RMS     : 220.5 V
 *   Status          : ✔  NORMAL — Sensor working correctly ✔
 * ══════════════════════════════════════════
 *
 *  IF you see "NO SIGNAL":
 *  → Check ZMPT101B VCC = 5V (not 3.3V!)
 *  → Check OUT wire is on GPIO34
 *  → Check AC is connected to ZMPT101B input
 *
 *  IF voltage shows 0 or very low:
 *  → No AC power connected to ZMPT101B input terminals
 *  → Or VOLTAGE_FACTOR needs to be increased
 *
 *  IF reading is 180V but real is 220V:
 *  → new VOLTAGE_FACTOR = 1.0 × (220/180) = 1.222
 *  → Update #define VOLTAGE_FACTOR 1.222f and re-upload
 *
 *  ZMPT101B TRIM POT:
 *  → Small blue/white potentiometer on ZMPT101B board
 *  → Rotate it slowly to tune the output level
 * ================================================================
 */
