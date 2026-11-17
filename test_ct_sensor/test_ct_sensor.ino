/*
 * ================================================================
 *  CT Sensor (SCT-013) — Test Code via ADS1115
 *
 *  WIRING:
 *  ┌──────────────────────────────────────────┐
 *  │  ADS1115    →    ESP32                   │
 *  │  VCC        →    3.3V                    │
 *  │  GND        →    GND                     │
 *  │  SDA        →    GPIO21                  │
 *  │  SCL        →    GPIO22                  │
 *  │  ADDR       →    GND  (addr = 0x48)      │
 *  └──────────────────────────────────────────┘
 *
 *  BIAS CIRCUIT on ADS1115 A0:
 *  ┌──────────────────────────────────────────┐
 *  │  ESP32 3.3V ──[10kΩ]──┐                 │
 *  │                        ├── A0 (ADS1115)  │
 *  │  CT OUT(+) ──[100Ω]───┘                 │
 *  │                        ├──[10kΩ]── GND  │
 *  │                        └──[10µF] ── GND  │
 *  │  CT OUT(-) ────────────── GND            │
 *  └──────────────────────────────────────────┘
 *
 *  CT SENSOR LABEL:    Check your sensor and set below
 *  Libraries needed:   Adafruit ADS1X15
 * ================================================================
 */

#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <math.h>

// ── ADS1115 ──────────────────────────────────────────────────
#define I2C_SDA       21
#define I2C_SCL       22
#define CH_CURRENT    0    // A0 → CT Sensor via bias circuit

// ── CT Sensor Calibration ────────────────────────────────────
// Check the label printed on your CT sensor body:
//   SCT-013-000  → CT_RATIO = 2000,  BURDEN_OHMS = 100
//   SCT-013-030  → CT_RATIO = 30,    BURDEN_OHMS = 1 (built-in)
//   SCT-013-020  → CT_RATIO = 20,    BURDEN_OHMS = 1 (built-in)
//   Generic 1000:1 → CT_RATIO = 1000, BURDEN_OHMS = 100
#define CT_RATIO      2000.0f  // ← match your sensor label
#define BURDEN_OHMS   100.0f   // ← your actual burden resistor

// ADS1115 GAIN_ONE = ±4.096V → 0.125 mV per LSB
#define VOLTS_PER_BIT 0.0001250f

// ── Sampling ─────────────────────────────────────────────────
#define SAMPLES 500

// ── Object ───────────────────────────────────────────────────
Adafruit_ADS1115 ads;

// ─────────────────────────────────────────────────────────────
float readCurrentRMS() {
    float sumRaw = 0, sumSq = 0;

    // Pass 1: find midpoint (removes 1.65V DC bias)
    for (int i = 0; i < SAMPLES; i++) {
        sumRaw += ads.readADC_SingleEnded(CH_CURRENT);
    }
    float mid = sumRaw / SAMPLES;

    // Pass 2: compute RMS around midpoint
    for (int i = 0; i < SAMPLES; i++) {
        float v = ads.readADC_SingleEnded(CH_CURRENT) - mid;
        sumSq += v * v;
    }

    float rmsRaw    = sqrtf(sumSq / SAMPLES);
    float rmsVoltV  = rmsRaw * VOLTS_PER_BIT;          // convert to volts
    float current   = (rmsVoltV / BURDEN_OHMS) * CT_RATIO; // Ohm's law + ratio
    return (current < 0.02f) ? 0.0f : current;         // noise floor
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(800);

    Serial.println();
    Serial.println("╔════════════════════════════════════════════╗");
    Serial.println("║   CT Sensor (SCT-013) — Test via ADS1115  ║");
    Serial.println("╚════════════════════════════════════════════╝");
    Serial.println();
    Serial.println("Wiring check:");
    Serial.println("  ADS1115 VCC  → ESP32 3.3V");
    Serial.println("  ADS1115 GND  → ESP32 GND");
    Serial.println("  ADS1115 SDA  → ESP32 GPIO21");
    Serial.println("  ADS1115 SCL  → ESP32 GPIO22");
    Serial.println("  ADS1115 ADDR → GND");
    Serial.println("  ADS1115 A0   → Bias circuit output");
    Serial.println("  CT OUT(+)    → 100Ω → Bias Node");
    Serial.println("  3.3V → 10kΩ → Bias Node → 10kΩ → GND");
    Serial.println("  CT OUT(-)    → GND");
    Serial.println();
    Serial.printf("  CT_RATIO     = %.0f\n", CT_RATIO);
    Serial.printf("  BURDEN_OHMS  = %.0f Ω\n", BURDEN_OHMS);
    Serial.println();

    Wire.begin(I2C_SDA, I2C_SCL);

    if (!ads.begin()) {
        Serial.println("╔══════════════════════════════════════════╗");
        Serial.println("║  ✖ ADS1115 NOT FOUND!                  ║");
        Serial.println("║  Check: SDA=GPIO21, SCL=GPIO22, VCC=3.3V║");
        Serial.println("║  Check: ADDR pin connected to GND        ║");
        Serial.println("╚══════════════════════════════════════════╝");
        while (1) delay(500);
    }

    ads.setGain(GAIN_ONE);            // ±4.096V range
    ads.setDataRate(RATE_ADS1115_860SPS);
    Serial.println("  ✔ ADS1115 found and ready!");
    Serial.println("──────────────────────────────────────────────");
    Serial.println("  Clamp CT sensor around a LIVE AC wire.");
    Serial.println("  Plug in a load (bulb, fan etc.).");
    Serial.println("  Watch current reading change below.");
    Serial.println("══════════════════════════════════════════════");
    delay(1000);
}

// ─────────────────────────────────────────────────────────────
void loop() {
    // Read raw bias point (midpoint check)
    int rawMid = ads.readADC_SingleEnded(CH_CURRENT);

    // Read current
    float current = readCurrentRMS();
    float approxPower = current * 230.0f; // assume 230V supply

    // ── Status check ─────────────────────────────────────────
    String status;
    if (rawMid < 100) {
        status = "✖ NO BIAS — Check 10kΩ resistors & 3.3V connection";
    } else if (rawMid > 32000) {
        status = "✖ SATURATED — Signal too high, check burden resistor";
    } else if (rawMid < 10000 || rawMid > 22000) {
        status = "⚠ BIAS OFF-CENTER — Expected ~16000 (1.65V midpoint)";
    } else if (current == 0.0f) {
        status = "✔ BIAS OK — No load detected (0.0A) — clamp around live wire";
    } else if (current < 10.0f) {
        status = "✔ WORKING — Current detected!";
    } else {
        status = "⚠ HIGH CURRENT — Check CT_RATIO setting";
    }

    // ── Print ─────────────────────────────────────────────────
    Serial.println("══════════════════════════════════════════════");
    Serial.printf("  Raw ADC (bias point) : %d  (ideal: ~16384)\n", rawMid);
    Serial.printf("  Bias Voltage         : %.3f V  (ideal: 1.65V)\n",
                   rawMid * VOLTS_PER_BIT);
    Serial.println("  ──────────────────────────────────────────");
    Serial.printf("  Current RMS          : %.4f A\n", current);
    Serial.printf("  Approx Power         : %.2f W  (at 230V)\n", approxPower);
    Serial.printf("  Status               : %s\n", status.c_str());
    Serial.println("══════════════════════════════════════════════");
    Serial.println();

    delay(2000);
}

/*
 * ================================================================
 *  WHAT TO EXPECT:
 * ================================================================
 *
 *  TEST 1 — No AC load connected:
 *  ──────────────────────────────
 *  Raw ADC (bias point) : 16250  (ideal: ~16384)
 *  Bias Voltage         : 2.031 V  (ideal: 1.65V)
 *  Current RMS          : 0.0000 A
 *  Status               : ✔ BIAS OK — No load detected
 *
 *  TEST 2 — CT clamped around wire with 100W bulb ON:
 *  ──────────────────────────────────────────────────
 *  Raw ADC (bias point) : 16100
 *  Bias Voltage         : 2.012 V
 *  Current RMS          : 0.4300 A    ← 100W/230V = 0.43A ✔
 *  Approx Power         : 98.90 W
 *  Status               : ✔ WORKING — Current detected!
 *
 * ================================================================
 *  COMMON PROBLEMS & FIXES:
 * ================================================================
 *
 *  ✖ ADS1115 NOT FOUND:
 *    → Check SDA=GPIO21, SCL=GPIO22 are correct
 *    → Check ADS1115 VCC = 3.3V (not 5V)
 *    → Check ADDR pin connected to GND
 *
 *  Raw ADC = 0 or very low (< 100):
 *    → 10kΩ bias resistors not connected
 *    → 3.3V not reaching the bias node
 *    → CT sensor OUT(-) not connected to GND
 *
 *  Current = 0.0A even with load ON:
 *    → CT sensor is NOT clamped around the live AC wire
 *    → Clamp the CT ring AROUND the Phase wire (not Neutral)
 *    → Make sure load is actually ON and drawing current
 *    → CT_RATIO might need to be changed (check sensor label)
 *
 *  Current value is wrong (too high or too low):
 *    → Check CT_RATIO matches your sensor label
 *    → For 100W bulb: expected = 0.43A
 *    → new CT_RATIO = old × (0.43 / displayed_current)
 *
 *  ⚠ Bias off-center (raw < 10000 or > 22000):
 *    → Verify both 10kΩ resistors are correct value
 *    → Check 3.3V is stable on ESP32
 * ================================================================
 */
