#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <Preferences.h>
#include "Config.h"

VL53L0X  tof;
bool     tofReady = false;
uint16_t lastDist = 9999;

// ── Sensor settings ───────────────────────────────────────────
// The defaults below are the values this file used to hard-code. Every one of
// them can be changed live with the 'tof' command (type 'tof' for help) and
// stored per unit with 'tof save'.
#define TOF_RATE_DEFAULT    0.5f   // MCPS: weakest return accepted as a target.
                                   // The library's own default is 0.25; lower =
                                   // longer range, but more false hits.
#define TOF_BUDGET_DEFAULT  0      // ms; 0 = the sensor's own default (~33 ms)
#define TOF_PERIOD_DEFAULT  50     // ms between measurements
#define TOF_MIN_DEFAULT     50     // mm: below this = crosstalk artefact, ignored
#define TOF_MAX_DEFAULT     2000   // mm: above this = no target

struct TofCfg {
    float    rate      = TOF_RATE_DEFAULT;
    uint16_t budgetMs  = TOF_BUDGET_DEFAULT;
    uint16_t periodMs  = TOF_PERIOD_DEFAULT;
    uint16_t minMm     = TOF_MIN_DEFAULT;
    uint16_t maxMm     = TOF_MAX_DEFAULT;
    uint8_t  longRange = 0;      // 1 = laser pulse periods 18/14 instead of 14/10
    uint8_t  version   = 1;      // layout marker for the stored copy
};
TofCfg tofCfg;

// Valid measurement: inside the window set by 'tof min' / 'tof max'.
inline bool distValid(uint16_t d) { return d >= tofCfg.minMm && d <= tofCfg.maxMm; }

// ── Diagnostics: counters + the sensor's own quality registers ─
struct TofDiag {
    bool     stream   = false;       // print every measurement
    uint16_t raw      = 0;           // last raw range, mm
    uint8_t  status   = 0;           // last device range status (0..15)
    float    signal   = 0;           // last return signal rate, MCPS
    float    ambient  = 0;           // last ambient rate, MCPS
    float    spads    = 0;           // last effective SPAD count
    uint32_t n = 0, valid = 0, noTarget = 0, tooClose = 0, tooFar = 0, timeouts = 0;
    uint16_t minSeen = 0xFFFF, maxSeen = 0;      // range of valid readings
    uint32_t statusCount[16] = {};
};
TofDiag tofDiag;

static uint32_t tofBudgetFactoryUs = 33000;   // read back from the sensor at init
static uint32_t tofAppliedBudgetUs = 33000;   // what the sensor currently has
static uint8_t  tofAppliedLong     = 0;       // ditto for the laser pulse periods

// Names are ST's own device range status codes (internal value 0..14).
const char* tofStatusName(uint8_t s) {
    switch (s) {
        case 0:  return "none";
        case 1:  return "VCSEL continuity FAIL (hardware)";
        case 2:  return "VCSEL watchdog FAIL (hardware)";
        case 3:  return "no VHV value (hardware)";
        case 4:  return "MSRC no target";
        case 5:  return "SNR check";
        case 6:  return "range phase check";
        case 7:  return "sigma threshold";
        case 8:  return "TCC";
        case 9:  return "phase consistency";
        case 10: return "min clip (too close)";
        case 11: return "range complete (valid)";
        case 12: return "algo underflow";
        case 13: return "algo overflow";
        case 14: return "range ignore threshold";
        default: return "?";
    }
}

// Keep every setting inside what the sensor/library accepts.
void tofClamp() {
    if (tofCfg.rate < 0.0f)    tofCfg.rate = 0.0f;
    if (tofCfg.rate > 511.99f) tofCfg.rate = 511.99f;
    if (tofCfg.budgetMs != 0) {
        if (tofCfg.budgetMs < 20)   tofCfg.budgetMs = 20;
        if (tofCfg.budgetMs > 1000) tofCfg.budgetMs = 1000;
    }
    if (tofCfg.periodMs > 5000) tofCfg.periodMs = 5000;
    if (tofCfg.maxMm > 8000)    tofCfg.maxMm = 8000;
    if (tofCfg.minMm > tofCfg.maxMm) tofCfg.minMm = tofCfg.maxMm;
    tofCfg.longRange = tofCfg.longRange ? 1 : 0;
}

// Pushes tofCfg to the sensor. Continuous mode is stopped while the ranging
// parameters change. The laser pulse periods and the timing budget are only
// touched when they actually differ (changing the pulse periods makes the
// sensor re-run a reference calibration).
bool tofApply(bool stopFirst = true) {
    tofClamp();
    bool ok = true;
    if (stopFirst) tof.stopContinuous();
    ok &= tof.setSignalRateLimit(tofCfg.rate);
    // "Applied" is only updated when the sensor accepted the change, so a
    // rejected setting is retried (or reverted) correctly next time.
    if (tofCfg.longRange != tofAppliedLong) {
        bool a = tof.setVcselPulsePeriod(VL53L0X::VcselPeriodPreRange,   tofCfg.longRange ? 18 : 14);
        bool b = tof.setVcselPulsePeriod(VL53L0X::VcselPeriodFinalRange, tofCfg.longRange ? 14 : 10);
        if (a && b) tofAppliedLong = tofCfg.longRange; else ok = false;
    }
    uint32_t budgetUs = tofCfg.budgetMs ? (uint32_t)tofCfg.budgetMs * 1000UL : tofBudgetFactoryUs;
    if (budgetUs != tofAppliedBudgetUs) {
        if (tof.setMeasurementTimingBudget(budgetUs)) tofAppliedBudgetUs = budgetUs; else ok = false;
    }
    tof.startContinuous(tofCfg.periodMs);
    return ok;
}

// ── Per-unit storage (flash) ──────────────────────────────────
void tofSave() {
    Preferences p;
    p.begin("tofcfg", false);
    p.putBytes("cfg", &tofCfg, sizeof(tofCfg));
    p.end();
}

bool tofLoad() {
    Preferences p;
    p.begin("tofcfg", false);
    bool ok = (p.getBytesLength("cfg") == sizeof(TofCfg));
    if (ok) p.getBytes("cfg", &tofCfg, sizeof(TofCfg));
    p.end();
    if (ok) tofClamp();
    return ok;
}

void tofClearSaved() {
    Preferences p;
    p.begin("tofcfg", false);
    p.remove("cfg");
    p.end();
}

void proximitySetup() {
    gpio_hold_dis((gpio_num_t)TOF_XSHUT);  // release hold from sleep
    // Wire is used by VL53L0X (Pololu lib uses global Wire)
    Wire.begin(TOF_SDA, TOF_SCL, TOF_FREQ);

    pinMode(TOF_XSHUT, OUTPUT);
    digitalWrite(TOF_XSHUT, LOW);  delay(10);
    digitalWrite(TOF_XSHUT, HIGH); delay(10);

    tof.setTimeout(500);
    if (!tof.init()) {
        Serial.println("[TOF] VL53L0X not found");
        return;
    }
    tofBudgetFactoryUs = tofAppliedBudgetUs = tof.getMeasurementTimingBudget();
    if (tofLoad()) Serial.println("[TOF] using the settings stored for this unit ('tof clear' removes them)");
    if (!tofApply(false)) Serial.println("[TOF] warning: the sensor rejected a stored setting");
    Serial.println("[TOF] VL53L0X ready");
    tofReady = true;
}

// Non-blocking: only reads when sensor has new data ready
void proximityRead() {
    if (!tofReady) return;
    if (!(tof.readReg(0x13) & 0x07)) return;    // RESULT_INTERRUPT_STATUS

    // The quality registers must be read BEFORE readRange...() below, which
    // clears the interrupt. Layout (from ST's API): byte 0 = status (bits 6:3),
    // 2-3 = effective SPADs (8.8), 6-7 = return signal rate (9.7 MCPS),
    // 8-9 = ambient rate (9.7 MCPS).
    uint8_t blk[12] = {};
    tof.readMulti(0x14, blk, 12);

    uint16_t d = tof.readRangeContinuousMillimeters();
    if (tof.timeoutOccurred()) { tofDiag.timeouts++; return; }

    tofDiag.n++;
    tofDiag.raw     = d;
    tofDiag.status  = (blk[0] & 0x78) >> 3;
    tofDiag.spads   = (float)(((uint16_t)blk[2] << 8) | blk[3]) / 256.0f;
    tofDiag.signal  = (float)(((uint16_t)blk[6] << 8) | blk[7]) / 128.0f;
    tofDiag.ambient = (float)(((uint16_t)blk[8] << 8) | blk[9]) / 128.0f;
    tofDiag.statusCount[tofDiag.status & 0x0F]++;

    if (d >= 8190)             { lastDist = 9999; tofDiag.noTarget++; }   // no target
    else if (d < tofCfg.minMm) { lastDist = 9999; tofDiag.tooClose++; }   // crosstalk
    else if (d > tofCfg.maxMm) { lastDist = 9999; tofDiag.tooFar++;   }   // beyond the window
    else {
        lastDist = d;
        tofDiag.valid++;
        if (d < tofDiag.minSeen) tofDiag.minSeen = d;
        if (d > tofDiag.maxSeen) tofDiag.maxSeen = d;
    }

    if (tofDiag.stream) {
        Serial.printf("#%lu raw=%5u mm  status=%2u %-28s signal=%5.2f MCPS (limit %.2f)  ambient=%5.2f  spads=%4.1f  -> %s\n",
                      (unsigned long)tofDiag.n, d, tofDiag.status, tofStatusName(tofDiag.status),
                      tofDiag.signal, tofCfg.rate, tofDiag.ambient, tofDiag.spads,
                      lastDist == 9999 ? "ignored" : "used");
    }
}

// ── 'tof' command ─────────────────────────────────────────────
static const char* tofSkipSpaces(const char* s) { while (*s == ' ') s++; return s; }

static void tofPrintSettings() {
    uint32_t budgetUs = tofCfg.budgetMs ? (uint32_t)tofCfg.budgetMs * 1000UL : tofBudgetFactoryUs;
    Serial.printf("[TOF] rate limit %.2f MCPS (library default 0.25)  budget %lu ms%s  period %u ms  long-range %s\n",
                  tofCfg.rate, (unsigned long)(budgetUs / 1000), tofCfg.budgetMs ? "" : " (sensor default)",
                  tofCfg.periodMs, tofCfg.longRange ? "on (pulses 18/14)" : "off (pulses 14/10)");
    Serial.printf("[TOF] valid window %u-%u mm   (haptics scale %d-%d mm, set in Config.h)\n",
                  tofCfg.minMm, tofCfg.maxMm, PROX_NEAR_MM, PROX_FAR_MM);
}

static void tofPrintStatus() {
    Serial.printf("[TOF] %s\n", tofReady ? "ready" : "NOT READY (sensor not found at boot)");
    tofPrintSettings();
    if (tofDiag.n == 0) { Serial.println("[TOF] no measurements yet"); return; }
    Serial.printf("[TOF] last: raw %u mm  status %u %s  signal %.2f MCPS  ambient %.2f  spads %.1f\n",
                  tofDiag.raw, tofDiag.status, tofStatusName(tofDiag.status),
                  tofDiag.signal, tofDiag.ambient, tofDiag.spads);
    Serial.printf("[TOF] %lu measurements: valid %lu", (unsigned long)tofDiag.n, (unsigned long)tofDiag.valid);
    if (tofDiag.valid) Serial.printf(" (%u-%u mm)", tofDiag.minSeen, tofDiag.maxSeen);
    Serial.printf(" | no target %lu | too close %lu | too far %lu | timeouts %lu\n",
                  (unsigned long)tofDiag.noTarget, (unsigned long)tofDiag.tooClose,
                  (unsigned long)tofDiag.tooFar, (unsigned long)tofDiag.timeouts);
    Serial.print("[TOF] status codes seen:");
    for (int i = 0; i < 16; i++)
        if (tofDiag.statusCount[i]) Serial.printf("  %d %s x%lu", i, tofStatusName(i), (unsigned long)tofDiag.statusCount[i]);
    Serial.println();
}

static void tofUsage() {
    Serial.println(
        "[TOF] commands:\n"
        "  tof                 status, settings and counters\n"
        "  tof raw             toggle: print every measurement with its status + signal rate\n"
        "  tof reset           clear the counters\n"
        "  tof rate <mcps>     weakest return accepted (0-511.99; library default 0.25, long-range recipe 0.1)\n"
        "  tof budget <ms>     time per measurement (20-1000; 0 = sensor default ~33; 200 = high accuracy)\n"
        "  tof period <ms>     interval between measurements\n"
        "  tof long on|off     longer laser pulses (18/14 instead of 14/10): more range, needs dark surroundings\n"
        "  tof min <mm>        readings below this are ignored as crosstalk\n"
        "  tof max <mm>        readings above this count as 'no target'\n"
        "  tof defaults        back to the firmware defaults (not saved)\n"
        "  tof save / clear    store / erase this unit's settings in flash");
}

void tofCommand(const char* args) {
    args = tofSkipSpaces(args);
    char sub[12] = {};
    int n = 0;
    while (args[n] && args[n] != ' ' && n < 11) { sub[n] = args[n]; n++; }
    const char* val = tofSkipSpaces(args + n);

    if (sub[0] == '\0')                 { tofPrintStatus(); return; }
    if (!strcmp(sub, "help"))           { tofUsage();       return; }
    if (!strcmp(sub, "raw"))            { tofDiag.stream = !tofDiag.stream;
                                          Serial.printf("[TOF] raw stream %s\n", tofDiag.stream ? "ON" : "OFF"); return; }
    if (!strcmp(sub, "reset"))          { uint8_t keep = tofDiag.stream;
                                          tofDiag = TofDiag(); tofDiag.stream = keep;
                                          Serial.println("[TOF] counters cleared"); return; }
    if (!strcmp(sub, "save"))           { tofSave();       Serial.println("[TOF] settings stored for this unit"); return; }
    if (!strcmp(sub, "clear"))          { tofClearSaved(); Serial.println("[TOF] stored settings erased (running settings unchanged until 'tof defaults' or reboot)"); return; }

    // Everything below changes the sensor itself.
    const bool known = !strcmp(sub, "rate") || !strcmp(sub, "budget") || !strcmp(sub, "period") ||
                       !strcmp(sub, "long") || !strcmp(sub, "min")    || !strcmp(sub, "max")    ||
                       !strcmp(sub, "defaults");
    if (!known)    { tofUsage(); return; }
    if (!tofReady) { Serial.println("[TOF] sensor not ready"); return; }

    TofCfg before = tofCfg;
    if (!strcmp(sub, "defaults")) {
        tofCfg = TofCfg();
    } else if (*val == '\0') {
        tofUsage(); return;
    } else if (!strcmp(sub, "rate")) {
        float v = atof(val);
        if (v < 0.0f || v > 511.99f) { Serial.println("[TOF] rate must be 0-511.99 MCPS"); return; }
        tofCfg.rate = v;
    } else if (!strcmp(sub, "budget")) {
        int ms = atoi(val);
        if (ms != 0 && (ms < 20 || ms > 1000)) { Serial.println("[TOF] budget must be 20-1000 ms (or 0 for the sensor default)"); return; }
        tofCfg.budgetMs = ms;
    } else if (!strcmp(sub, "period")) {
        int ms = atoi(val);
        if (ms < 0 || ms > 5000) { Serial.println("[TOF] period must be 0-5000 ms"); return; }
        tofCfg.periodMs = ms;
    } else if (!strcmp(sub, "long")) {
        if      (!strcmp(val, "on"))  tofCfg.longRange = 1;
        else if (!strcmp(val, "off")) tofCfg.longRange = 0;
        else { Serial.println("[TOF] usage: tof long on|off"); return; }
    } else if (!strcmp(sub, "min")) {
        int mm = atoi(val);
        if (mm < 0 || mm > tofCfg.maxMm) { Serial.printf("[TOF] min must be 0-%u mm (the current max)\n", tofCfg.maxMm); return; }
        tofCfg.minMm = mm;
    } else if (!strcmp(sub, "max")) {
        int mm = atoi(val);
        if (mm < tofCfg.minMm || mm > 8000) { Serial.printf("[TOF] max must be %u-8000 mm (the current min up)\n", tofCfg.minMm); return; }
        tofCfg.maxMm = mm;
    }

    if (!tofApply()) {
        Serial.println("[TOF] the sensor rejected that setting — reverting");
        tofCfg = before;
        tofApply();
    }
    tofPrintSettings();
}