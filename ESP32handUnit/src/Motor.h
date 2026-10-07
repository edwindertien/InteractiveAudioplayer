#pragma once
#include <Arduino.h>
#include "Config.h"

// ── LEDC API version switch ────────────────────────────────────
// 0 = old channel-based API — Arduino-ESP32 core 2.x
//     (official platformio/espressif32 6.x).
//     ledcSetup() + ledcAttachPin(), ledcWrite(CHANNEL, duty)
// 1 = new pin-based API — Arduino-ESP32 core 3.x (pioarduino).
//     ledcAttach(pin, freq, res), ledcWrite(PIN, duty); MOTOR_CH unused.
//
// Espressif's 2.x -> 3.0 migration guide puts this break at the 3.0
// boundary, so the default is picked from the core's own version macro.
// Force it either way with -DMOTOR_LEDC_NEW_API=0/1 in build_flags, or
// by defining it above this include.
#ifndef MOTOR_LEDC_NEW_API
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    #define MOTOR_LEDC_NEW_API 1
  #else
    #define MOTOR_LEDC_NEW_API 0
  #endif
#endif

#if MOTOR_LEDC_NEW_API
  #define MOTOR_LEDC_ATTACH()  ledcAttach(MOTOR_PIN, MOTOR_FREQ, MOTOR_RES)
  #define MOTOR_LEDC_WRITE(d)  ledcWrite(MOTOR_PIN, (d))
#else
  #define MOTOR_LEDC_ATTACH()  do { ledcSetup(MOTOR_CH, MOTOR_FREQ, MOTOR_RES); \
                                    ledcAttachPin(MOTOR_PIN, MOTOR_CH); } while (0)
  #define MOTOR_LEDC_WRITE(d)  ledcWrite(MOTOR_CH, (d))
#endif

// ── Motor burst — esp_timer ISR ───────────────────────────────
// Fires independently of loop() timing.
// motorProxSet() is ignored while a burst is active.

struct MotorBurst {
    volatile bool     active  = false;
    volatile uint8_t  pulses  = 0;
    volatile uint8_t  done    = 0;
    volatile uint8_t  duty    = 0;
    volatile uint16_t onMs    = 0;
    volatile uint16_t offMs   = 0;
    volatile bool     motorOn = false;
} motorBurst;

esp_timer_handle_t motorTimer;

void IRAM_ATTR motorTimerCb(void*) {
    if (!motorBurst.active) return;
    if (motorBurst.motorOn) {
        MOTOR_LEDC_WRITE(0);
        motorBurst.motorOn = false;
        esp_timer_start_once(motorTimer, motorBurst.offMs * 1000ULL);
    } else {
        if (++motorBurst.done >= motorBurst.pulses) {
            motorBurst.active = false; return;
        }
        MOTOR_LEDC_WRITE(motorBurst.duty);
        motorBurst.motorOn = true;
        esp_timer_start_once(motorTimer, motorBurst.onMs * 1000ULL);
    }
}

void motorSetup() {
    MOTOR_LEDC_ATTACH();
    MOTOR_LEDC_WRITE(0);
    esp_timer_create_args_t args = {};
    args.callback = motorTimerCb;
    args.name     = "motor";
    esp_timer_create(&args, &motorTimer);
}

void motorBurstStart(uint8_t pulses = 3, uint8_t duty = 200,
                     uint16_t onMs = 55, uint16_t offMs = 35) {
    esp_timer_stop(motorTimer);
    motorBurst.active  = true;  motorBurst.pulses  = pulses;
    motorBurst.done    = 0;     motorBurst.duty    = duty;
    motorBurst.onMs    = onMs;  motorBurst.offMs   = offMs;
    motorBurst.motorOn = true;
    MOTOR_LEDC_WRITE(duty);
    esp_timer_start_once(motorTimer, onMs * 1000ULL);
}

// Continuous intensity — ignored while burst is active
void motorProxSet(uint8_t duty) {
    if (!motorBurst.active) MOTOR_LEDC_WRITE(duty);
}