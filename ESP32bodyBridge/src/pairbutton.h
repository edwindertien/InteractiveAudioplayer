#pragma once
#include <Arduino.h>
#include "Config.h"
#include "EspNow.h"   // BROADCAST, PingPacket, MSG_PAIR_REQUEST, pairingMode

// Long-press GP9 to enter pairing mode: broadcasts a PAIR_REQUEST every
// PAIR_REQUEST_INTERVAL_MS until a hand unit acknowledges (short-press on
// its own button) or PAIR_MODE_TIMEOUT_MS elapses with no reply.

enum class PairBtnState { IDLE, PRESSED };
PairBtnState pairBtnState  = PairBtnState::IDLE;
uint32_t     pairBtnDownMs = 0;

void pairButtonSetup() {
    pinMode(PAIR_BUTTON_PIN, INPUT_PULLUP);
}

static void sendPairRequest() {
    PingPacket pkt = {};
    pkt.type = MSG_PAIR_REQUEST;
    strlcpy(pkt.deviceId, "BODY_BRIDGE", sizeof(pkt.deviceId));
    esp_err_t r = esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
    if (r != ESP_OK) Serial.printf("[PAIR] esp_now_send(broadcast) failed: %d\n", r);
}

// Call every loop().
void pairButtonUpdate(uint32_t now) {
    bool pressed = (digitalRead(PAIR_BUTTON_PIN) == LOW);

    switch (pairBtnState) {
        case PairBtnState::IDLE:
            if (pressed) { pairBtnState = PairBtnState::PRESSED; pairBtnDownMs = now; }
            break;
        case PairBtnState::PRESSED:
            if (!pressed) {
                pairBtnState = PairBtnState::IDLE;   // released early — ignored
            } else if (!pairingMode && (now - pairBtnDownMs >= PAIR_HOLD_MS)) {
                pairingMode      = true;
                pairingStartedMs = now;
                lastPairReqMs    = 0;   // fire the first broadcast immediately below
                Serial.println("[PAIR] Long press — entering pairing mode, broadcasting...");
            }
            break;
    }

    if (!pairingMode) return;

    if (now - pairingStartedMs >= PAIR_MODE_TIMEOUT_MS) {
        pairingMode = false;
        Serial.println("[PAIR] Timed out — no hand unit acknowledged");
        return;
    }
    if (now - lastPairReqMs >= PAIR_REQUEST_INTERVAL_MS) {
        lastPairReqMs = now;
        sendPairRequest();
    }
}