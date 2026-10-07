#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "Config.h"
#include "Proximity.h"    // lastDist, distValid
#include "TouchManager.h" // touchState

CRGB leds[LED_COUNT];

enum class LedMode { IDLE, RAINBOW, SOLID, OFF };
LedMode ledMode    = LedMode::IDLE;
CRGB    solidColor = CRGB::Black;
uint8_t rainbowHue = 0;

void ledSetup(uint8_t brightness = 40) {
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(brightness);
    FastLED.clear(true);
}

void ledSolid(CRGB c) { fill_solid(leds, LED_COUNT, c); FastLED.show(); }
void ledOff()          { FastLED.clear(true); }

// Call at 25fps from loop()
void ledUpdate(uint32_t now) {
    switch (ledMode) {

        case LedMode::IDLE: {
            // During RELEASED: stay teal (t=0), no colour shift toward red
            float t = (touchState == TouchState::IDLE
                       && tofReady && distValid(lastDist) && lastDist < PROX_FAR_MM)
                ? constrain(1.0f - (float)(lastDist - PROX_NEAR_MM)
                                   / (float)(PROX_FAR_MM - PROX_NEAR_MM), 0.0f, 1.0f)
                : 0.0f;

            // Per-frame colour weights (float, once)
            // Far: teal (0,140,200) → close: warm red (220,40,0)
            uint8_t rMax  = (uint8_t)(t * t * 220.0f);
            uint8_t gMax  = (uint8_t)((1.0f - t) * 140.0f);
            uint8_t bMax  = (uint8_t)((1.0f - t) * 200.0f);

            // Travelling sine wave — integer only per LED (sin8 lookup table)
            // 256 units × 15ms = 3.84s per full rotation
            uint8_t phaseBase = (uint8_t)(now / 15);
            for (uint8_t i = 0; i < LED_COUNT; i++) {
                uint8_t phase = phaseBase + i * (256 / LED_COUNT);
                uint8_t s = sin8(phase);
                uint8_t b = 15 + ((uint16_t)s * 215 / 255);  // 15–230 range
                leds[i] = CRGB(
                    (uint16_t)rMax * b / 255,
                    (uint16_t)gMax * b / 255,
                    (uint16_t)bMax * b / 255
                );
            }
            FastLED.show();
            break;
        }

        case LedMode::RAINBOW:
            fill_rainbow(leds, LED_COUNT, rainbowHue += 2, 256 / LED_COUNT);
            FastLED.show();
            break;

        case LedMode::SOLID:
            fill_solid(leds, LED_COUNT, solidColor);
            FastLED.show();
            break;

        case LedMode::OFF:
            break;
    }
}
