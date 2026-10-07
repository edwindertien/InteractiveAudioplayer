#pragma once
#include <Arduino.h>
#include "Proximity.h"   // lastDist, distValid

#define SILENCE_MS 3000  // ms of motor silence after a connection

enum class TouchState { IDLE, RELEASED };
TouchState touchState   = TouchState::IDLE;
uint32_t   silenceUntil = 0;

// Call when NFC tag read triggers a connection
void touchConnect(uint32_t now) {
    touchState   = TouchState::RELEASED;
    silenceUntil = now + SILENCE_MS;
    Serial.printf("[Touch] Connection! Silence %dms, then separate\n", SILENCE_MS);
}

// Call each loop. Returns true once when transitioning back to IDLE.
bool touchUpdate(uint32_t now) {
    if (touchState != TouchState::RELEASED) return false;
    // Both conditions required: silence elapsed AND object has left field
    if (now > silenceUntil && !distValid(lastDist)) {
        touchState = TouchState::IDLE;
        Serial.println("[Touch] Ready — new connection possible");
        return true;
    }
    return false;
}
