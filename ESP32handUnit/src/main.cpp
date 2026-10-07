#include <Arduino.h>
#include "Config.h"
#include "Motor.h"
#include "Proximity.h"
#include "TouchManager.h"
#include "LedRing.h"
#include "NfcTask.h"
#include "EspNow.h"
#include "StatusLed.h"
#include "PowerManager.h"
#include "Cli.h"

void setup() {
    setCpuFrequencyMhz(80);   // 80MHz is plenty — 240MHz default runs hot
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Hand Unit ===");

#if POWER_MANAGEMENT
    buttonSetup();
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    if (wakeReason != ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println("[PWR] Fresh boot → sleep. Press button to start.");
        delay(50);
        while (digitalRead(BUTTON_PIN) == LOW) delay(10);
        delay(100);
        powerSleepNow();
    }
    Serial.println("[PWR] Woke from button — release to continue...");
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(200);
#else
    powerPrintWakeReason();
#endif

    motorSetup();
    proximitySetup();
    ledSetup(40);
    statusLedSetup();
    batterySetup();
    nfcSetup();      // RST sequence + 50ms delay built in
    espNowSetup();

    ledSolid(nfcReady && tofReady ? CRGB(0,40,0) : CRGB(40,20,0));
    motorBurstStart(2, 160, 60, 40);
    delay(300);
    ledOff();

    printHelp();
}

uint32_t ledTimer      = 0;
uint32_t streamTimer   = 0;
uint32_t proxSendTimer = 0;

void loop() {
    uint32_t now = millis();

    powerUpdate(now);
    if (shortPressEvent) {
        shortPressEvent = false;
        espNowConfirmPairing();
    }
    bool pairingPending = pendingPairMs != 0 &&
                          (now - pendingPairMs) < PAIR_REQUEST_WINDOW_MS;
    statusLedUpdate(now, peerPaired, batLow, pairingPending);

    cliPoll();
    proximityRead();
    touchUpdate(now);

    if (touchState == TouchState::RELEASED) {
        motorProxSet(0);
    } else if (tofReady && distValid(lastDist) && lastDist < PROX_FAR_MM
               && ledMode == LedMode::IDLE) {
        float t = 1.0f - (float)(lastDist - PROX_NEAR_MM)
                         / (float)(PROX_FAR_MM - PROX_NEAR_MM);
        motorProxSet((uint8_t)(constrain(t,0.0f,1.0f)*t*MOTOR_MAX));
    } else {
        motorProxSet(0);
    }

    char uid[20] = {};
    if (nfcQueue && xQueueReceive(nfcQueue, uid, 0) == pdTRUE) {
        Serial.printf("[NFC] Tag: %s\n", uid);
        statusOnNfc();
        espNowSendNfc(uid);
        if (touchState == TouchState::IDLE) {
            motorBurstStart(2, 230, 40, 30);
            touchConnect(now);
            statusOnTouch();
            espNowSendTouch(uid);
            ledSolid(CRGB(200,200,200));
            delay(80);
        } else {
            Serial.println("[Touch] Ignored — still in silence");
        }
    }

    if (now - proxSendTimer >= 100) {
        proxSendTimer = now;
        espNowSendProximity();
    }

    if (now - ledTimer >= 40) {
        ledTimer = now;
        ledUpdate(now);
    }

    if (streaming && tofReady && (now - streamTimer >= 100)) {
        streamTimer = now;
        uint8_t bars = (uint8_t)map(constrain(lastDist,0,500),0,500,30,0);
        char bar[32]={}; memset(bar,'#',bars);
        Serial.printf("%4u mm  %s\n", lastDist, bar);
    }

    // Yield 1ms — lets idle task run, allows CPU light sleep between iterations
    vTaskDelay(pdMS_TO_TICKS(1));
}