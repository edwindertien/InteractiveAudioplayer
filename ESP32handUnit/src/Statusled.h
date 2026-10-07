#pragma once
#include <Arduino.h>
#include <FastLED.h>

// GPIO48 — onboard WS2812B on ESP32-S3 Super Mini
// Second FastLED controller alongside ring (GPIO4) — same show() updates both

#define STATUS_LED_PIN 48

// Colour order of the onboard LED (GRB on the original board). If red and
// green look swapped on a different board: -DSTATUS_LED_ORDER=RGB.
#ifndef STATUS_LED_ORDER
  #define STATUS_LED_ORDER GRB
#endif

CRGB statusLed[1];   // also written directly by PowerManager.h (battery warnings)

// ── LED language (same on hand unit and body bridge) ──────────
//   not paired                                red     slow pulse
//   paired, body bridge not answering         yellow  pulse
//   paired, body bridge answering (linked)    green   pulse
//   tag / touch (sent from here)              blue    quick double pulse
//   pairing mode (button press pending ack)   blue    fast hard blink — outranks link state
//                                                      and battery, but not the tag pulse
//
// "Answering" = a heartbeat from OUR paired bridge within LINK_TIMEOUT.
// The bridge sends one per second, so 3.5 s tolerates two lost ones.
// The colour is derived from live data every frame — nothing latches.
// ─────────────────────────────────────────────────────────────
#define LINK_TIMEOUT   3500   // ms without a heartbeat from our bridge -> yellow
#define FLASH_MS          600   // total length of the double tag/touch pulse
#define BATTERY_PERIOD_MS 400   // battery-low pulse period — quick, vs 1000-1500ms above
#define PAIRING_BLINK_MS  150   // on/off half-period while a pairing request is pending

enum class LinkMode : uint8_t { UNPAIRED, WAITING, LINKED };

volatile uint32_t flashStart   = 0;   // millis() of last tag/touch, 0 = none
volatile uint32_t lastLinkedMs = 0;   // last heartbeat from our paired bridge
uint32_t          statusTimer  = 0;   // 25 fps gate
int8_t            shownMode    = -1;  // last mode logged

void statusLedSetup() {
    FastLED.addLeds<WS2812B, STATUS_LED_PIN, STATUS_LED_ORDER>(statusLed, 1);
    statusLed[0] = CRGB::Black;
}

// ── Event hooks ───────────────────────────────────────────────
void statusOnLinked()   { lastLinkedMs = millis(); }   // heartbeat from our bridge
void statusOnUnpaired() { lastLinkedMs = 0; }
void statusOnPaired()   {}                             // derived from peerPaired each frame
// Any tag message — raw NFC or confirmed touch — gives the same blue pulse.
void statusOnNfc()      { uint32_t t = millis(); flashStart = t ? t : 1; }
void statusOnTouch()    { statusOnNfc(); }

static void reportMode(LinkMode m) {
    if ((int8_t)m == shownMode) return;
    shownMode = (int8_t)m;
    switch (m) {
        case LinkMode::UNPAIRED: Serial.println("[LED] not paired -> red");                      break;
        case LinkMode::WAITING:  Serial.println("[LED] paired, waiting for body bridge -> yellow"); break;
        case LinkMode::LINKED:   Serial.println("[LED] linked -> green");                        break;
    }
}

// ── Update — call every loop ──────────────────────────────────
// batteryLow overrides everything else — a dying battery matters more than
// link state or a touch flash, so it's checked first and shown unconditionally.
// pairingPending: a body bridge asked to pair and we're waiting on the
// button press to confirm it — ranks below batteryLow (device safety wins)
// but above ordinary link state, so it isn't masked by a routine touch flash.
void statusLedUpdate(uint32_t now, bool paired, bool batteryLow = false, bool pairingPending = false) {
    if (now - statusTimer < 40) return;   // 25fps
    statusTimer = now;

    if (batteryLow) {
        if (shownMode != 9) { shownMode = 9; Serial.println("[LED] battery low -> orange quick pulse"); }
        float br = 0.10f + 0.60f * ((sinf(now * 2.0f * 3.14159265f / BATTERY_PERIOD_MS) + 1.0f) * 0.5f);
        statusLed[0] = CRGB((uint8_t)(br * 255), (uint8_t)(br * 80), 0);
        FastLED.show();
        return;
    }

    if (pairingPending) {
        if (shownMode != 8) { shownMode = 8; Serial.println("[LED] pairing request pending -> fast blue blink"); }
        bool on = ((now / PAIRING_BLINK_MS) % 2) == 0;
        statusLed[0] = on ? CRGB(0, 0, 220) : CRGB::Black;
        FastLED.show();
        return;
    }

    uint32_t lk = lastLinkedMs;
    LinkMode mode = !paired ? LinkMode::UNPAIRED
                  : (lk != 0 && (int32_t)(now - lk) < LINK_TIMEOUT)
                        ? LinkMode::LINKED : LinkMode::WAITING;
    reportMode(mode);

    // Double-pulse: two humps across FLASH_MS via |sin(2*pi*t)|.
    uint32_t fs  = flashStart;
    int32_t  age = fs ? (int32_t)(now - fs) : (int32_t)FLASH_MS;
    if (fs && age < (int32_t)FLASH_MS) {
        float t  = (age < 0 ? 0 : age) / (float)FLASH_MS;
        float br = 0.10f + 0.90f * fabsf(sinf(2.0f * 3.14159265f * t));
        statusLed[0] = CRGB(0, 0, (uint8_t)(br * 255));
        FastLED.show();
        return;
    }

    float br;
    switch (mode) {
        case LinkMode::UNPAIRED:
            br = 0.05f + 0.55f * ((sinf(now / 1500.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), 0, 0);
            break;
        case LinkMode::WAITING:
            br = 0.05f + 0.45f * ((sinf(now / 1000.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), (uint8_t)(br * 120), 0);
            break;
        case LinkMode::LINKED:
            br = 0.15f + 0.30f * ((sinf(now / 1200.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB(0, (uint8_t)(br * 255), (uint8_t)(br * 40));
            break;
    }
    FastLED.show();
}