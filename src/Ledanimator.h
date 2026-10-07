#pragma once

// ============================================================
// LedAnimator — single LED ring, synced with the haptic channel
//
// One SK6812/WS2812 ring on one pin. Two layers, same model as
// before:
//   setBackground()    — persistent looping pattern: chapter mood,
//                         or a live haptic-sync pattern like
//                         'level' / 'heartbeat'
//   triggerForeground() — brief one-shot event (chapter enter,
//                         tag-triggered flash) that temporarily
//                         overrides the background, then reverts
//
// setBeatLevel(peak) feeds the live haptic envelope into the
// 'level' pattern — call every loop() with hapticPeak.read().
//
// Previously there were two rings: a 24-LED tap-feedback ring on
// pin 5 driven by the local NFC reader, and a 16-LED haptic ring
// on pin 14. The reader and its ring have moved to the wearer's
// hand unit + ESP32 body bridge (see EspBridge.h). Only the
// haptic ring remains here — pin 5 is now free.
// ============================================================

#include <FastLED.h>
#include <SD.h>
#include <ArduinoJson.h>

#define LED_PIN          14    // the one remaining ring
#define LED_COUNT        16    // match your physical ring — see README
#define LED_COLOR_ORDER  GRB
#define LED_FRAME_MS     40    // 40ms = 25fps — see Context.md re: PWM stutter

// ── Custom frame animations (loaded from led_animations.json) ─
#define CUSTOM_ANIM_MAX    12
#define CUSTOM_FRAME_MAX   64

struct LedAnimFrame {
    uint16_t ms = 100;
    uint8_t  r[LED_COUNT] = {};
    uint8_t  g[LED_COUNT] = {};
    uint8_t  b[LED_COUNT] = {};
};

struct CustomLedAnim {
    char         id[24] = {};
    bool         loop   = true;
    uint8_t      frameCount = 0;
    LedAnimFrame frames[CUSTOM_FRAME_MAX];
};

struct LedParams {
    char     animation[20] = "level";
    uint32_t color         = 0x0044FF;
    float    intensity     = 0.6f;
    float    speed         = 0.4f;
    uint16_t durationMs    = 0;
};

class LedAnimator {
public:
    void begin(uint8_t brightness = 160) {
        FastLED.addLeds<SK6812, LED_PIN, LED_COLOR_ORDER>(_leds, LED_COUNT);
        FastLED.setBrightness(brightness);
        Serial.printf("[LED] %u LEDs on pin %u  brightness:%u\n",
                      LED_COUNT, LED_PIN, brightness);

        // Hardware test: flash red for 500ms then go dark
        fill_solid(_leds, LED_COUNT, CRGB::Red);
        FastLED.show();
        Serial.println("[LED] Hardware test: RED — if dark, check wiring/pin");
        delay(500);
        fill_solid(_leds, LED_COUNT, CRGB::Black);
        FastLED.show();
    }

    // ── Background / foreground layers ─────────────────────────

    void setBackground(const char* anim, uint32_t color,
                       float intensity, float speed) {
        strlcpy(_bg.animation, anim, sizeof(_bg.animation));
        _bg.color     = color;
        _bg.intensity = constrain(intensity, 0.0f, 1.0f);
        _bg.speed     = constrain(speed,     0.0f, 1.0f);
        if (strcmp(anim, _lastBgAnim) != 0) {
            _bgPhase = 0; _bgStep = 0;
            _customIdx = -1;
            strlcpy(_lastBgAnim, anim, sizeof(_lastBgAnim));
        }
    }

    void triggerForeground(const char* anim, uint32_t color,
                           float intensity, float speed, uint16_t durationMs) {
        if (durationMs == 0) return;
        strlcpy(_fg.animation, anim, sizeof(_fg.animation));
        _fg.color      = color;
        _fg.intensity  = constrain(intensity, 0.0f, 1.0f);
        _fg.speed      = constrain(speed,     0.0f, 1.0f);
        _fg.durationMs = durationMs;
        _fgActive      = true;
        _fgTimer       = 0;
        _fgPhase       = 0;
        _fgStep        = 0;
    }

    void setBackgroundFromParams(const LedParams& p) {
        setBackground(p.animation, p.color, p.intensity, p.speed);
    }
    void triggerForegroundFromParams(const LedParams& p) {
        triggerForeground(p.animation, p.color, p.intensity,
                          p.speed, p.durationMs);
    }

    void setGlobalBrightness(uint8_t b) { FastLED.setBrightness(b); }

    // Live haptic envelope (0.0-1.0), feeds the 'level' pattern.
    // Call every loop() with hapticPeak.read() when available.
    void setBeatLevel(float peak) { _beatLevel = peak; }

    // ── Custom frame animations ──────────────────────────────────
    bool loadAnimations(const char* path = "/led_animations.json") {
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[LED] No %s on SD — custom animations unavailable\n", path);
            return false;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            Serial.printf("[LED] led_animations.json parse error: %s\n", err.c_str());
            return false;
        }
        _customCount = 0;
        for (JsonObject a : doc["animations"].as<JsonArray>()) {
            if (_customCount >= CUSTOM_ANIM_MAX) break;
            CustomLedAnim& ca = _custom[_customCount];
            strlcpy(ca.id, a["id"] | "", sizeof(ca.id));
            ca.loop = a["loop"] | true;
            ca.frameCount = 0;
            for (JsonObject fr : a["frames"].as<JsonArray>()) {
                if (ca.frameCount >= CUSTOM_FRAME_MAX) break;
                LedAnimFrame& lf = ca.frames[ca.frameCount];
                lf.ms = fr["ms"] | 100;
                uint8_t pi = 0;
                for (const char* px : fr["pixels"].as<JsonArray>()) {
                    if (pi >= LED_COUNT) break;
                    uint32_t v = strtoul(px + 1, nullptr, 16);
                    lf.r[pi] = (v >> 16) & 0xFF;
                    lf.g[pi] = (v >> 8)  & 0xFF;
                    lf.b[pi] =  v        & 0xFF;
                    pi++;
                }
                ca.frameCount++;
            }
            Serial.printf("[LED] Custom anim '%s': %u frames\n", ca.id, ca.frameCount);
            _customCount++;
        }
        Serial.printf("[LED] Loaded %u custom animations\n", _customCount);
        return true;
    }

    int8_t findCustomAnim(const char* id) {
        for (uint8_t i = 0; i < _customCount; i++)
            if (strcmp(_custom[i].id, id) == 0) return (int8_t)i;
        return -1;
    }

    // ── Main update — call every loop() ───────────────────────────
    void update() {
        if (_masterTimer < LED_FRAME_MS) return;
        _masterTimer = 0;

        if (_fgActive && _fgTimer >= _fg.durationMs) _fgActive = false;

        if (_fgActive) render(_fg, _fgPhase, _fgStep);
        else           render(_bg, _bgPhase, _bgStep);

        FastLED.show();
        // No yield() needed — FastLED on Teensy 4.x uses FlexIO DMA (non-blocking)
    }

private:
    CRGB _leds[LED_COUNT];

    LedParams     _bg, _fg;
    bool          _fgActive = false;
    elapsedMillis _fgTimer;
    elapsedMillis _masterTimer;
    float         _bgPhase = 0.0f, _fgPhase = 0.0f;
    uint16_t      _bgStep  = 0,    _fgStep  = 0;
    char          _lastBgAnim[20] = {};

    CustomLedAnim _custom[CUSTOM_ANIM_MAX];
    uint8_t       _customCount = 0;
    int8_t        _customIdx   = -1;
    uint8_t       _frameIdx    = 0;
    elapsedMillis _frameTimer;

    float _beatLevel = 0.0f;   // live haptic level, 0.0-1.0
    float _beatEnv   = 0.0f;   // envelope follower output

    // ── Colour helper ─────────────────────────────────────────
    CRGB col(uint32_t c, float brightness) {
        float b = constrain(0.05f + brightness * 0.95f, 0.0f, 1.0f);
        b = b * b;
        return CRGB(
            (uint8_t)(((c >> 16) & 0xFF) * b),
            (uint8_t)(((c >>  8) & 0xFF) * b),
            (uint8_t)(( c        & 0xFF) * b)
        );
    }

    float phaseStep(float speed) { return 0.08f + speed * 0.50f; }

    // ── Render dispatch ────────────────────────────────────────
    void render(LedParams& p, float& phase, uint16_t& step) {
        if (_customIdx < 0) _customIdx = findCustomAnim(p.animation);
        if (_customIdx >= 0) { renderCustom(); return; }

        const char* a = p.animation;
        if      (strcmp(a, "off")        == 0) fill_solid(_leds, LED_COUNT, CRGB::Black);
        else if (strcmp(a, "solid")      == 0) fill_solid(_leds, LED_COUNT, col(p.color, p.intensity));
        else if (strcmp(a, "level")      == 0) level(p);
        else if (strcmp(a, "heartbeat")  == 0) heartbeat(p, phase);
        else if (strcmp(a, "breathe")    == 0) breathe(p, phase);
        else if (strcmp(a, "sparkle")    == 0) sparkle(p);
        else if (strcmp(a, "fire")       == 0) fire(p);
        else if (strcmp(a, "ocean")      == 0) ocean(p, phase);
        else if (strcmp(a, "rainbow")    == 0) rainbow(p, phase);
        else if (strcmp(a, "spin")       == 0) spin(p, step);
        else if (strcmp(a, "flash")      == 0) flash(p, step);
        else if (strcmp(a, "pulse_ring") == 0) pulseRing(p, step);
        else if (strcmp(a, "confetti")   == 0) confetti(p);
        else if (strcmp(a, "comet")      == 0) comet(p, step);
        else fill_solid(_leds, LED_COUNT, col(p.color, p.intensity));

        phase += phaseStep(p.speed);
        if (phase > TWO_PI) phase -= TWO_PI;
        step++;
    }

    void renderCustom() {
        CustomLedAnim& ca = _custom[_customIdx];
        if (ca.frameCount == 0) return;
        LedAnimFrame& f = ca.frames[_frameIdx];
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i] = CRGB(f.r[i], f.g[i], f.b[i]);
        if (_frameTimer >= f.ms) {
            _frameTimer = 0;
            _frameIdx++;
            if (_frameIdx >= ca.frameCount) _frameIdx = ca.loop ? 0 : ca.frameCount - 1;
        }
    }

    // ── Patterns ──────────────────────────────────────────────────
    // 'level' and 'heartbeat' are the primary haptic-sync patterns.
    // The rest are general mood/event patterns — usable as a chapter
    // background instead of haptic sync, or as one-shot tag/enter flashes.

    void level(const LedParams& p) {
        if (_beatLevel > _beatEnv) _beatEnv = _beatLevel;
        else                        _beatEnv *= 0.88f;
        float gain       = 2.0f + p.speed * 12.0f;
        float compressed = 1.0f - expf(-gain * _beatEnv);
        float b = max(compressed * p.intensity, 0.04f);
        fill_solid(_leds, LED_COUNT, col(p.color, b));
    }

    void heartbeat(const LedParams& p, float ph) {
        float t = ph / TWO_PI;
        float brightness;
        if      (t < 0.06f) brightness = t / 0.06f;
        else if (t < 0.14f) brightness = 1.0f - (t-0.06f)/0.08f * 0.4f;
        else if (t < 0.20f) brightness = 0.6f + (t-0.14f)/0.06f * 0.4f;
        else if (t < 0.30f) brightness = 1.0f - (t-0.20f)/0.10f * 0.8f;
        else                brightness = 0.2f * (1.0f - (t-0.30f)/0.70f);
        brightness = max(brightness, 0.05f);
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity * brightness));
    }

    void breathe(const LedParams& p, float ph) {
        float t = 0.25f + ((sinf(ph) + 1.0f) * 0.5f) * 0.75f;
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity * t));
    }

    void sparkle(const LedParams& p) {
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity * 0.12f));
        uint8_t cnt = max((uint8_t)2, (uint8_t)(p.intensity * 5));
        for (uint8_t i = 0; i < cnt; i++)
            _leds[random8(LED_COUNT)] = col(0xFFFFFF, p.intensity * (0.5f + random8(128)/255.0f));
    }

    void fire(const LedParams& p) {
        static uint8_t heat[LED_COUNT] = {};
        for (uint8_t i = 0; i < LED_COUNT; i++) heat[i] = qsub8(heat[i], random8(8, 22));
        for (uint8_t i = 0; i < LED_COUNT; i++) {
            uint8_t prev = heat[(i + LED_COUNT - 1) % LED_COUNT];
            uint8_t next = heat[(i + 1) % LED_COUNT];
            heat[i] = (heat[i] + prev + next) / 3;
        }
        uint8_t hotspots = 2 + (uint8_t)(p.intensity * 2);
        for (uint8_t h = 0; h < hotspots; h++) {
            uint8_t pos = random8(LED_COUNT);
            heat[pos] = qadd8(heat[pos], random8(120, 200));
            heat[(pos+1)%LED_COUNT]           = qadd8(heat[(pos+1)%LED_COUNT], random8(60,120));
            heat[(pos+LED_COUNT-1)%LED_COUNT] = qadd8(heat[(pos+LED_COUNT-1)%LED_COUNT], random8(60,120));
        }
        uint8_t bright = (uint8_t)(p.intensity * 255);
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i] = HeatColor(scale8(heat[i], bright));
    }

    void ocean(const LedParams& p, float ph) {
        for (uint8_t i = 0; i < LED_COUNT; i++) {
            float wave = (sinf(ph + i * TWO_PI / LED_COUNT) + 1.0f) * 0.5f;
            float b = p.intensity * (0.25f + wave * 0.75f);
            _leds[i] = CRGB(0, (uint8_t)(wave * b * 160), (uint8_t)(b * 200));
        }
    }

    void rainbow(const LedParams& p, float ph) {
        uint8_t hue = (uint8_t)(ph * 40.0f);
        uint8_t brt = (uint8_t)(p.intensity * 230);
        for (uint8_t i = 0; i < LED_COUNT; i++)
            _leds[i] = CHSV(hue + i * (256/LED_COUNT), 220, brt);
    }

    void spin(const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(170);
        uint8_t head = s % LED_COUNT;
        for (uint8_t t = 0; t < 6 && t < LED_COUNT; t++)
            _leds[(head + LED_COUNT - t) % LED_COUNT] = col(p.color, p.intensity * (6-t)/6.0f);
    }

    void flash(const LedParams& p, uint16_t s) {
        uint16_t f = s % 24;
        bool on = (f < 3) || (f >= 6 && f < 9) || (f >= 12 && f < 15);
        fill_solid(_leds, LED_COUNT, col(p.color, p.intensity * (on ? 1.0f : 0.08f)));
    }

    void pulseRing(const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(160);
        uint8_t arc = (s * 2) % LED_COUNT;
        for (uint8_t i = 0; i < arc; i++) {
            float b = (float)(i + 1) / (arc + 1);
            _leds[i] += col(p.color, p.intensity * b);
        }
    }

    void confetti(const LedParams& p) {
        for (uint8_t i = 0; i < LED_COUNT; i++) {
            _leds[i].nscale8(200);
            _leds[i] += col(p.color, p.intensity * 0.08f);
        }
        uint8_t cnt = max((uint8_t)1, (uint8_t)(p.intensity * 3));
        for (uint8_t i = 0; i < cnt; i++)
            _leds[random8(LED_COUNT)] = CHSV(random8(), 200, (uint8_t)(p.intensity*230));
    }

    void comet(const LedParams& p, uint16_t s) {
        for (uint8_t i = 0; i < LED_COUNT; i++) _leds[i].nscale8(185);
        uint8_t pos = s % LED_COUNT;
        _leds[pos]                          = col(p.color, p.intensity);
        _leds[(pos+LED_COUNT-1)%LED_COUNT]  = col(p.color, p.intensity * 0.5f);
        _leds[(pos+LED_COUNT-2)%LED_COUNT]  = col(p.color, p.intensity * 0.2f);
    }
};