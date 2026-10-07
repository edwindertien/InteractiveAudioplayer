#pragma once
// ── UART to Teensy ────────────────────────────────────────────
#define TEENSY_UART   Serial1
#define TEENSY_TX     43      // ESP32 TX → Teensy pin 0 (RX)
#define TEENSY_RX     44      // ESP32 RX ← Teensy pin 1 (TX)
#define TEENSY_BAUD   115200
 

// ── Pairing button ────────────────────────────────────────────
#define PAIR_BUTTON_PIN            9      // button to GND, internal pull-up
#define PAIR_HOLD_MS            2000      // long-press duration to enter pairing mode
#define PAIR_REQUEST_INTERVAL_MS 300      // how often to broadcast while in pairing mode
#define PAIR_MODE_TIMEOUT_MS   30000      // give up if nobody acknowledges