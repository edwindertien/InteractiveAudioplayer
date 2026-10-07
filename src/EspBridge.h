#pragma once

// ============================================================
// EspBridge — UART link to the ESP32 body bridge
//
// The body bridge receives ESP-NOW packets from the wearer's
// hand unit (proximity + PN532 NFC reads) and forwards them to
// the Teensy as newline-terminated JSON over UART.
//
// All hand-tap feedback (LED flash, vibration double-click) now
// lives on the hand unit and body bridge — the Teensy no longer
// needs to react visually to a raw tag touch, only to look up
// the tag in config.json and change chapter/overlay/fx state.
//
// Wiring:
//   ESP32 body bridge GPIO43 (TX) → Teensy pin 0  (RX1)
//   ESP32 body bridge GPIO44 (RX) → Teensy pin 1  (TX1)
//   Common GND
//
// Message format — one JSON object per line, 115200 baud:
//   {"t":"p","d":150,"s":0}            proximity (10Hz, informational)
//   {"t":"n","u":"DE0CC698","d":150}   raw NFC detection (logged only —
//                                      useful for discovering tag UIDs)
//   {"t":"c","u":"DE0CC698"}           confirmed connection — DISPATCHES
//
// Only 'c' triggers a tag lookup/dispatch. The hand unit's own touch
// state machine already debounces repeats and enforces a post-connection
// silence period, so 'c' fires once per genuine interaction — the
// Teensy-side per-tag cooldown_ms in config.json is a second, optional
// safety net on top of that, not the primary debounce.
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>

class EspBridge {
public:
    void begin(HardwareSerial& serial = Serial1, uint32_t baud = 115200) {
        _serial = &serial;
        _serial->begin(baud);
        Serial.printf("[Bridge] listening @ %lu baud (pins 0/1)\n", baud);
    }

    // Call every loop() iteration. Non-blocking line reader.
    // Returns true when a confirmed connection ('c') arrived;
    // uidOut receives the tag UID (uppercase hex, same format
    // used by config.json tag uids).
    bool poll(char* uidOut) {
        while (_serial->available()) {
            char c = _serial->read();
            if (c == '\n' || c == '\r') {
                if (_pos > 0) {
                    _buf[_pos] = '\0';
                    _pos = 0;
                    if (handleLine(_buf, uidOut)) return true;
                }
            } else if (_pos < sizeof(_buf) - 1) {
                _buf[_pos++] = c;
            }
        }
        return false;
    }

    // ── Status (for the 'bridge' serial debug command) ────────
    uint16_t lastProximityMm() const { return _lastDist; }
    uint8_t  lastTouchState()  const { return _lastTouch; }
    bool     isAlive() const {
        return _lastMsgMs != 0 && (millis() - _lastMsgMs) < 3000;
    }
    uint32_t lastMessageAgeMs() const {
        return _lastMsgMs == 0 ? 0 : millis() - _lastMsgMs;
    }

private:
    bool handleLine(const char* line, char* uidOut) {
        JsonDocument doc;
        if (deserializeJson(doc, line)) return false;   // malformed — ignore
        _lastMsgMs = millis();

        const char* type = doc["t"] | "";

        if (strcmp(type, "p") == 0) {
            _lastDist  = doc["d"] | 9999;
            _lastTouch = doc["s"] | 0;
            return false;
        }

        if (strcmp(type, "n") == 0) {
            const char* uid = doc["u"] | "";
            Serial.printf("[Bridge] NFC: %s\n", uid);
            return false;
        }

        if (strcmp(type, "c") == 0) {
            const char* uid = doc["u"] | "";
            strlcpy(uidOut, uid, 20);
            Serial.printf("[Bridge] Connection: %s\n", uidOut);
            return true;
        }

        return false;
    }

    HardwareSerial* _serial     = &Serial1;
    char            _buf[96]    = {};
    uint8_t         _pos        = 0;
    uint16_t        _lastDist   = 9999;
    uint8_t         _lastTouch  = 0;
    uint32_t        _lastMsgMs  = 0;
};