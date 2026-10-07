#pragma once
#include <Arduino.h>
#include <driver/adc.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include "Config.h"
#include "StatusLed.h"   // for low-battery blink
#include "Motor.h"       // stop motor before sleep
#include "LedRing.h"     // LEDs off before sleep

#if POWER_MANAGEMENT


// ── Wakeup cause ──────────────────────────────────────────────
void powerPrintWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("[PWR] Woke from button press"); break;
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("[PWR] Woke from timer"); break;
        default:
            Serial.println("[PWR] Normal boot (not waking from sleep)"); break;
    }
}

// ── Power-down sequence ───────────────────────────────────────
// Call before esp_deep_sleep_start()
void powerDownPeripherals() {
    // ── 1. LEDs off FIRST — before WiFi stop resets the RMT peripheral
    fill_solid(leds, LED_COUNT, CRGB::Black);
    statusLed[0] = CRGB::Black;
    FastLED.show();
    delay(10);

    // ── 2. Motor off
    ledcWrite(MOTOR_CH, 0);

    // ── 3. Peripherals to standby, latch all critical pins through sleep
    #if NFC_RST_PIN >= 0
    pinMode(NFC_RST_PIN, OUTPUT);           // nfcSetup() leaves it INPUT — must set OUTPUT
    digitalWrite(NFC_RST_PIN, LOW);         // drive LOW → PN532 in reset
    gpio_hold_en((gpio_num_t)NFC_RST_PIN);  // latch LOW through sleep
    #endif
    digitalWrite(TOF_XSHUT, LOW);
    gpio_hold_en((gpio_num_t)TOF_XSHUT);    // hold LOW → VL53L0X standby

    #if NFC_UART
    // UART TX idles HIGH. Without hold, GPIO5 goes LOW during sleep
    // → PN532 sees continuous break signal on its RX → UART state corrupted
    // → wakeup preamble fails → getFirmwareVersion() returns 0
    NfcSerial.flush();                      // ensure TX is idle (HIGH)
    gpio_hold_en((gpio_num_t)NFC_TX_PIN);   // latch TX HIGH through sleep
    #endif

    // ── 4. WiFi/ESP-NOW off (after LEDs — resets RMT)
    esp_wifi_stop();
    delay(150);   // let WiFi actually stop
}

// ── Minimal sleep — call before peripherals are initialised ──────
// Used at boot to immediately go to sleep on fresh power-on.
// Full powerSleep() is used at runtime (shuts down peripherals first).
void powerSleepNow() {
    // Use INPUT_PULLUP + gpio_hold_en() — simpler than rtc_gpio_* and more reliable.
    // gpio_hold_en() latches the current HIGH state through deep sleep
    // so the pin cannot float to LOW and trigger a spurious wakeup.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    delay(20);                                   // let pull-up charge up

    gpio_hold_en((gpio_num_t)BUTTON_PIN);        // latch HIGH through sleep

    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.println("[PWR] Sleeping — press button to wake");
    delay(50);
    esp_deep_sleep_start();
    // Never returns
}

// ── Sleep ─────────────────────────────────────────────────────
void powerSleep() {
    Serial.println("[PWR] Sleep in 500ms — release button");
    delay(100);   // flush serial

    powerDownPeripherals();

    // Wait for button release after peripherals are down
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(300);   // generous debounce after release

    // Latch button pin HIGH before sleep — prevents floating → spurious wake
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    delay(20);
    gpio_hold_en((gpio_num_t)BUTTON_PIN);

    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);

    Serial.println("[PWR] Deep sleep — press button to wake");
    delay(50);
    esp_deep_sleep_start();
    // Never returns
}

// ── Battery monitor ───────────────────────────────────────────
// The divider on VBAT_ADC_PIN hangs on the board's 5V rail, not on the cell.
// On battery that rail is the cell voltage minus the Schottky diode (about
// 0.3 V); on USB it is the USB 5 V. So:
//   rail above VBAT_USB_MV -> USB powered: the battery cannot be measured
//   otherwise              -> cell voltage ~= rail + batOffsetMv
// VBAT_LOW_MV / VBAT_CRIT_MV (Config.h) are cell voltages. The correction is
// a fixed number, so the estimate is only good to roughly +-0.1 V (the diode
// drop changes a little with load). Set the offset per unit with
// 'battery cal <battery V> <divider V>'.
#ifndef VBAT_OFFSET_MV
#define VBAT_OFFSET_MV  320     // mV added back to the rail reading
#endif
#ifndef VBAT_USB_MV
#define VBAT_USB_MV     4300    // a LiPo rail tops out near 4.1 V, so more than this is USB
#endif

uint32_t lastBatCheckMs = 0;
uint16_t lastBatMv      = 0;      // estimated cell voltage; 0 while on USB
uint16_t lastRailMv     = 0;      // what the divider actually measures
bool     batOnUsb       = false;  // rail is at USB level: battery not measurable
bool     batLow         = false;
bool     batCritical    = false;
int16_t  batOffsetMv    = VBAT_OFFSET_MV;

// Rail voltage in mV (divider midpoint x2, equal resistors).
// Uses analogReadMilliVolts() which applies ESP32 eFuse Vref calibration —
// much more accurate than raw ADC × 3300/4095 (which overestimates by ~7%)
uint16_t readRailMv() {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogReadMilliVolts(VBAT_ADC_PIN);
        delay(2);
    }
    return (uint16_t)((sum / 8) * 2);
}

// Pure conversion, kept separate so it can be tested.
void batteryFromRail(uint16_t railMv, int16_t offsetMv, uint16_t* batMv, bool* onUsb) {
    *onUsb = railMv > VBAT_USB_MV;
    int32_t mv = (int32_t)railMv + offsetMv;
    if (mv < 0)     mv = 0;
    if (mv > 65535) mv = 65535;
    *batMv = *onUsb ? 0 : (uint16_t)mv;
}

// Estimated cell voltage in mV, 0 while on USB. Does not touch the state below.
uint16_t readBatteryMv() {
    uint16_t b; bool u;
    batteryFromRail(readRailMv(), batOffsetMv, &b, &u);
    return b;
}

// One measurement -> lastRailMv, lastBatMv, batOnUsb, batLow, batCritical.
void batteryMeasure() {
    lastRailMv = readRailMv();
    batteryFromRail(lastRailMv, batOffsetMv, &lastBatMv, &batOnUsb);
    batLow      = !batOnUsb && lastBatMv < VBAT_LOW_MV;
    batCritical = !batOnUsb && lastBatMv < VBAT_CRIT_MV;
}

void batteryDescribe(char* out, size_t n) {
    if (batOnUsb) snprintf(out, n, "USB powered (rail %u mV) - battery not measurable", lastRailMv);
    else          snprintf(out, n, "%u mV (rail %u %+d)", lastBatMv, lastRailMv, batOffsetMv);
}

// ── Per-unit offset, kept in flash ────────────────────────────
void batteryLoadCal() {
    Preferences p;
    p.begin("batcal", false);
    int16_t v = p.getShort("off", INT16_MIN);
    p.end();
    if (v != INT16_MIN && v >= -1000 && v <= 1000) batOffsetMv = v;
}
void batterySaveCal() {
    Preferences p;
    p.begin("batcal", false);
    p.putShort("off", batOffsetMv);
    p.end();
}
void batteryClearCal() {
    Preferences p;
    p.begin("batcal", false);
    p.remove("off");
    p.end();
    batOffsetMv = VBAT_OFFSET_MV;
}

// 'battery' command: status, or 'cal <battery V> <divider V>' / 'offset <mV>' / 'clear'
void batteryUsage() {
    Serial.println(
        "Battery commands:\n"
        "  battery                      level now\n"
        "  battery cal <batV> <pinV>    set the offset from two multimeter readings taken on battery\n"
        "                               (batV = at the battery terminals, pinV = at GPIO3), e.g. cal 3.80 1.74\n"
        "  battery offset <mV>          set the offset directly\n"
        "  battery clear                back to the default offset");
}

void batteryCommand(const char* args) {
    while (*args == ' ') args++;

    if (*args == '\0') {
        batteryMeasure();
        char d[96];
        batteryDescribe(d, sizeof d);
        Serial.printf("Battery: %s%s\n", d,
                      batCritical ? "  CRITICAL" : batLow ? "  LOW" : batOnUsb ? "" : "  OK");
        Serial.printf("  divider pin %.3f V  offset %d mV  (low < %d, critical < %d mV; USB above %d mV on the rail)\n",
                      lastRailMv / 2000.0f, batOffsetMv, VBAT_LOW_MV, VBAT_CRIT_MV, VBAT_USB_MV);
        return;
    }
    if (!strncmp(args, "cal ", 4)) {
        float bv = 0, pv = 0;
        if (sscanf(args + 4, "%f %f", &bv, &pv) != 2 || bv < 2.5f || bv > 4.5f || pv < 1.0f || pv > 2.5f) {
            Serial.println("[BAT] cal needs two voltages: the real battery (2.5-4.5 V) and the GPIO3 pin (1.0-2.5 V), measured with USB unplugged");
            return;
        }
        int off = (int)lroundf(bv * 1000.0f - pv * 2000.0f);
        if (off < -1000 || off > 1000) { Serial.printf("[BAT] that gives an offset of %d mV - check the readings\n", off); return; }
        batOffsetMv = (int16_t)off;
        batterySaveCal();
        Serial.printf("[BAT] offset %d mV stored for this unit\n", off);
        return;
    }
    if (!strncmp(args, "offset ", 7)) {
        int off = atoi(args + 7);
        if (off < -1000 || off > 1000) { Serial.println("[BAT] offset must be -1000..1000 mV"); return; }
        batOffsetMv = (int16_t)off;
        batterySaveCal();
        Serial.printf("[BAT] offset %d mV stored for this unit\n", off);
        return;
    }
    if (!strcmp(args, "clear")) {
        batteryClearCal();
        Serial.printf("[BAT] stored offset erased, back to the default %d mV\n", batOffsetMv);
        return;
    }
    batteryUsage();
}

void batterySetup() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);   // 0-3.3V range
    pinMode(VBAT_ADC_PIN, INPUT);
    batteryLoadCal();
    lastBatCheckMs = millis();
    batteryMeasure();
    char d[96];
    batteryDescribe(d, sizeof d);
    Serial.printf("[BAT] %s\n", d);
}

// Returns true if battery is critically low (caller should sleep)
bool batteryUpdate(uint32_t now) {
    if (now - lastBatCheckMs < VBAT_CHECK_MS) return false;
    lastBatCheckMs = now;

    batteryMeasure();
    char d[96];
    batteryDescribe(d, sizeof d);
    Serial.printf("[BAT] %s%s\n", d, batCritical ? " CRITICAL" : batLow ? " LOW" : "");

    if (batCritical) {
        Serial.println("[BAT] Critical — shutting down to protect battery");
        // Rapid red blink before sleep
        for (int i = 0; i < 6; i++) {
            statusLed[0] = (i % 2 == 0) ? CRGB(255,0,0) : CRGB::Black;
            FastLED.show(); delay(120);
        }
        powerSleep();
    }
    return false;
}

// batteryLowBlink() removed — the low-battery indicator is now the
// statusLedUpdate(now, paired, batLow) quick orange pulse in StatusLed.h,
// so the two no longer fight over statusLed[0].

// ── Button state machine ──────────────────────────────────────
// Detects long press (USE_LONG_PRESS=1) or double press (USE_LONG_PRESS=0)

enum class BtnState { IDLE, PRESSED, HELD, RELEASED };
BtnState btnState    = BtnState::IDLE;
uint32_t btnDownMs   = 0;      // time button went down
uint32_t btnUpMs     = 0;      // time button last released
uint8_t  pressCount  = 0;      // for double-press counting
volatile bool shortPressEvent = false;   // set on a brief tap, consumed by main.cpp
#define  SHORT_PRESS_MIN_MS  30          // ignore anything shorter — contact bounce

void buttonSetup() {
    gpio_hold_dis((gpio_num_t)BUTTON_PIN);  // release hold latched before sleep
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

// Returns true when sleep trigger fires
bool buttonUpdate(uint32_t now) {
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);

    if (USE_LONG_PRESS) {
        // ── Long press mode ───────────────────────────────────
        switch (btnState) {
            case BtnState::IDLE:
                if (pressed) { btnState = BtnState::PRESSED; btnDownMs = now; }
                break;
            case BtnState::PRESSED:
                if (!pressed) {
                    uint32_t heldMs = now - btnDownMs;
                    btnState = BtnState::IDLE;
                    if (heldMs >= SHORT_PRESS_MIN_MS && heldMs < POWER_HOLD_MS) {
                        shortPressEvent = true;   // tap — e.g. confirm a pending pairing request
                    }
                }
                else if (now - btnDownMs >= POWER_HOLD_MS) {
                    btnState = BtnState::HELD;
                    // Pulse status LED to confirm
                    statusLed[0] = CRGB(255, 60, 0); FastLED.show();
                    delay(200);
                    statusLed[0] = CRGB::Black; FastLED.show();
                    return true;  // → sleep
                }
                break;
            case BtnState::HELD:
                if (!pressed) btnState = BtnState::IDLE;
                break;
            default: break;
        }
    } else {
        // ── Double press mode ─────────────────────────────────
        switch (btnState) {
            case BtnState::IDLE:
                if (pressed) {
                    btnState   = BtnState::PRESSED;
                    btnDownMs  = now;
                }
                break;
            case BtnState::PRESSED:
                if (!pressed) {
                    btnState = BtnState::RELEASED;
                    btnUpMs  = now;
                    pressCount++;
                }
                break;
            case BtnState::RELEASED:
                // Timeout window: single press, reset
                if (now - btnUpMs > DOUBLE_PRESS_MS) {
                    pressCount = 0;
                    btnState   = BtnState::IDLE;
                }
                // Second press within window
                if (pressed) {
                    if (pressCount >= 1) {
                        pressCount = 0;
                        btnState   = BtnState::IDLE;
                        return true;  // → sleep
                    }
                    btnState  = BtnState::PRESSED;
                    btnDownMs = now;
                }
                break;
            default: break;
        }
    }
    return false;
}

// ── Unified power update — call from loop() ───────────────────
void powerUpdate(uint32_t now) {
    if (buttonUpdate(now)) powerSleep();
    batteryUpdate(now);
}


#else  // POWER_MANAGEMENT = 0 — always on, stubs

uint16_t lastBatMv   = 0;
uint16_t lastRailMv  = 0;
bool     batOnUsb    = false;
bool     batLow      = false;
bool     batCritical = false;
int16_t  batOffsetMv = 0;

void batteryDescribe(char* out, size_t n) { snprintf(out, n, "n/a (power management disabled)"); }
void batteryCommand(const char*)          { Serial.println("[PWR] Power management disabled"); }

void buttonSetup()              {}
void batterySetup()             { Serial.println("[PWR] Power management disabled"); }
void powerPrintWakeReason()     {}
void powerUpdate(uint32_t)      {}
void powerSleep()               { Serial.println("[PWR] Sleep disabled — set POWER_MANAGEMENT 1"); }
void powerSleepNow()            {}
uint16_t readBatteryMv()        { return 0; }

#endif // POWER_MANAGEMENT