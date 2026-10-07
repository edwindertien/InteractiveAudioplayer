#pragma once
#include <Arduino.h>
#include <FastLED.h>

// GPIO48 — onboard WS2812B on ESP32-S3 Super Mini
#define STATUS_LED_PIN  48
#define STATUS_LED_N    1

// Colour order of the onboard LED. The standard (non-v0.0.2) Super Mini
// board is GRB. The v0.0.2 variant (different USB OTG wiring) is RGB —
// if one ever ends up back on the bench, override with
// -DSTATUS_LED_ORDER=RGB in build_flags rather than editing this default.
#ifndef STATUS_LED_ORDER
  #define STATUS_LED_ORDER GRB
#endif

CRGB statusLed[STATUS_LED_N];

// ── LED language (same on hand unit and body bridge) ──────────
//   not paired                              red     slow pulse
//   paired, hand unit not answering         yellow  pulse
//   paired, hand unit answering (linked)    green   pulse
//   tag / touch message received            blue    quick double pulse
//   pairing mode (long-press GP9)           blue    fast hard blink — outranks all of the above
//
// The colour is derived from live data every frame — nothing latches.
// ─────────────────────────────────────────────────────────────
#define LINK_TIMEOUT_MS   2000   // no packet from the paired hand for this long -> yellow
#define FLASH_MS           600   // total length of the double touch pulse
#define PAIRING_BLINK_MS   150   // on/off half-period while in pairing mode

enum class LinkMode : uint8_t { UNPAIRED, WAITING, LINKED };

volatile uint32_t flashStart = 0;   // millis() of last tag/touch message, 0 = none
uint32_t          ledTimer   = 0;   // 25 fps gate
int8_t            shownMode  = -1;  // last mode logged

void statusLedSetup() {
    FastLED.addLeds<WS2812B, STATUS_LED_PIN, STATUS_LED_ORDER>(statusLed, STATUS_LED_N);
    FastLED.setBrightness(60);   // onboard LED — keep modest
    statusLed[0] = CRGB::Black;
    FastLED.show();
}

// ── Event hooks (called from the ESP-NOW callback) ────────────
// Any tag message — raw NFC or confirmed touch — gives the same blue pulse.
void statusOnNfc()    { uint32_t t = millis(); flashStart = t ? t : 1; }
void statusOnTouch()  { statusOnNfc(); }
// Linked/paired are derived from lastRxMs / handPaired each frame; kept so
// existing callers keep compiling.
void statusOnLinked() {}
void statusOnPaired() {}

static void reportMode(LinkMode m) {
    if ((int8_t)m == shownMode) return;
    shownMode = (int8_t)m;
    switch (m) {
        case LinkMode::UNPAIRED: Serial.println("[LED] not paired -> red");                    break;
        case LinkMode::WAITING:  Serial.println("[LED] paired, waiting for hand unit -> yellow"); break;
        case LinkMode::LINKED:   Serial.println("[LED] linked -> green");                      break;
    }
}

// Call each loop.  lastRxMs = last packet from the PAIRED hand unit.
// pairingMode (from PairButton.h) outranks everything else — a fast blink
// while actively pairing, so it can't be masked by a touch flash mid-handshake.
void statusLedUpdate(uint32_t now, bool paired, uint32_t lastRxMs, bool pairingMode = false) {
    if (now - ledTimer < 40) return;
    ledTimer = now;

    if (pairingMode) {
        if (shownMode != 9) { shownMode = 9; Serial.println("[LED] pairing mode -> fast blue blink"); }
        bool on = ((now / PAIRING_BLINK_MS) % 2) == 0;
        statusLed[0] = on ? CRGB(0, 0, 220) : CRGB::Black;
        FastLED.show();
        return;
    }

    LinkMode mode = !paired ? LinkMode::UNPAIRED
                  : (lastRxMs != 0 && (int32_t)(now - lastRxMs) < LINK_TIMEOUT_MS)
                        ? LinkMode::LINKED : LinkMode::WAITING;
    reportMode(mode);

    // Double-pulse: two humps across FLASH_MS via |sin(2*pi*t)|, so it
    // reads as a quick double blink, not one smooth breath.
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
        case LinkMode::UNPAIRED:    // slow red pulse
            br = 0.05f + 0.55f * ((sinf(now / 1500.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), 0, 0);
            break;
        case LinkMode::WAITING:     // yellow pulse
            br = 0.05f + 0.45f * ((sinf(now / 1000.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB((uint8_t)(br * 255), (uint8_t)(br * 120), 0);
            break;
        case LinkMode::LINKED:      // green pulse
            br = 0.15f + 0.30f * ((sinf(now / 1200.0f) + 1.0f) * 0.5f);
            statusLed[0] = CRGB(0, (uint8_t)(br * 255), (uint8_t)(br * 40));
            break;
    }
    FastLED.show();
}