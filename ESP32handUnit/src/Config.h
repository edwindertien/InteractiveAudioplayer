#pragma once

// ── NeoPixel ring ─────────────────────────────────────────────
#define LED_PIN          4
#define LED_COUNT        16
#define LED_TYPE         WS2812B
#define LED_COLOR_ORDER  GRB

// ── Vibration motor ───────────────────────────────────────────
#define MOTOR_PIN   2
#define MOTOR_FREQ  20000   // 20kHz — inaudible
#define MOTOR_RES   8       // 8-bit = 0-255
#define MOTOR_CH    0
#define MOTOR_MAX   200

// ── I2C bus — VL53L0X on Wire (GPIO7/8) ──────────────────────
#define TOF_SDA     7
#define TOF_SCL     8
#define TOF_FREQ    400000
#define TOF_XSHUT   1

// ── NFC interface selection ───────────────────────────────────
// 0 = I2C  (DIP: SEL0=HIGH SEL1=LOW  — Wire1 on GPIO5/6)
// 1 = UART (DIP: SEL0=LOW  SEL1=LOW  — Serial1 on GPIO16/17)
#define NFC_UART    1

#if NFC_UART
  #define NFC_TX_PIN    6      // ESP TX → PN532 RX
  #define NFC_RX_PIN    5      // ESP RX ← PN532 TX
  #define NFC_BAUD     115200
  #define NFC_RST_PIN  10      // GPIO10 → PN532 RST — required to recover stuck TX line
#else
  #define NFC_SDA      5
  #define NFC_SCL      6
  #define NFC_FREQ     400000
  #define NFC_RST_PIN  10      // GPIO10 → PN532 RST — required to recover stuck TX line
#endif

// ── NFC power management ─────────────────────────────────────
// Time PN532 spends in PowerDown between polls (ms).
// 0 = always on (max responsiveness, ~80mA continuous)
// 200 = good balance — tag detected within 220ms, ~8mA average
// 500 = max savings — tag detected within 520ms, ~3mA average
#define NFC_SLEEP_MS   0      // 0 = always on — eliminates powerdown as a variable

// ── Proximity ─────────────────────────────────────────────────
#define PROX_FAR_MM   300
#define PROX_NEAR_MM  20

// ── Status LED (onboard WS2812B) ──────────────────────────────
#define STATUS_LED_PIN  48

// ── Power management master switch ───────────────────────────
// 0 = always on, no sleep, no battery monitor, no button sleep
// 1 = full power management (deep sleep, button, battery)
#define POWER_MANAGEMENT 1

// ── Power management ──────────────────────────────────────────
#define BUTTON_PIN      9    // button to GND, internal pull-up
#define VBAT_ADC_PIN    3    // 100k+100k voltage divider from VBAT

// Sleep trigger: 1 = long press, 0 = double press
#define USE_LONG_PRESS    1
#define POWER_HOLD_MS  2000  // hold 2s → sleep
#define DOUBLE_PRESS_MS 500  // two taps within 500ms → sleep

// Battery thresholds in millivolts (1S LiPo: 3.0–4.2V)
#define VBAT_LOW_MV    3200  // rapid blink warning
#define VBAT_CRIT_MV   2000  // auto shutdown
#define VBAT_CHECK_MS  10000 // check interval