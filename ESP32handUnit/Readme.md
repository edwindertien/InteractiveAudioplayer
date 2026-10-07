# Hand Unit — ESP32-S3 Super Mini

Wearable hand unit for the KEIKEN immersive performance project. Detects proximity and NFC tag contact, drives haptic feedback and LED animation, and communicates wirelessly with a body bridge unit via ESP-NOW.

---

## Hardware

| Component | Part |
|---|---|
| MCU | ESP32-S3 Super Mini (ESP32-S3FH4R2, 4MB flash, 2MB PSRAM) |
| NFC reader | PN532 module — UART/HSU mode |
| Proximity sensor | VL53L0X ToF (I2C) |
| LED ring | 8× WS2812B NeoPixel |
| Status LED | Onboard WS2812B (GPIO48) |
| Vibration motor | Via N-channel MOSFET, PWM |
| Power button | Momentary, to GND |
| Battery | 1S LiPo, voltage divider to ADC |

---

## Pin Assignments

| Signal | GPIO | Notes |
|---|---|---|
| NeoPixel ring | 4 | WS2812B data |
| Motor PWM | 2 | LEDC channel 0, 20kHz |
| VL53L0X SDA | 7 | Wire (I2C0) |
| VL53L0X SCL | 8 | Wire (I2C0) |
| VL53L0X XSHUT | 1 | Active LOW shutdown |
| PN532 TX (ESP→PN532 RX) | 6 | UART1 |
| PN532 RX (ESP←PN532 TX) | 5 | UART1 |
| PN532 RST | 10 | Active LOW reset |
| Status LED | 48 | Onboard WS2812B |
| Power button | 9 | To GND, internal pull-up |
| Battery ADC | 3 | 100kΩ+100kΩ divider (1:2) |

### PN532 DIP Switches (UART mode)
```
SEL0 = LOW  (0)
SEL1 = LOW  (0)
```

### Battery Voltage Divider
```
VBAT ──── 100kΩ ──┬── 100kΩ ──── GND
                  │
               GPIO3 (+ 100nF cap to GND)
```

---

## Power On / Off

**Turn on:** press button once (wakes from deep sleep)  
**Turn off:** hold button for 2 seconds (long press)  
**Default state:** device boots into deep sleep — button is required to start

On fresh battery connection the device immediately enters deep sleep. The PN532 is held in reset (GPIO10 LOW, latched) during sleep. The VL53L0X is in hardware standby (XSHUT LOW, latched). Current in sleep: ~20µA (ESP32) + ~5µA (PN532) + ~5µA (VL53L0X).

---

## LED Ring Behaviour (8× NeoPixel, GPIO4)

| State | Colour | Pattern |
|---|---|---|
| Idle, nothing nearby | Teal | Travelling sine wave, ~3.8s rotation |
| Object approaching | Teal → red | Colour shifts with proximity |
| NFC tag contact | White | 80ms flash |
| Post-connection silence | Teal | No colour shift (motor suppressed) |

## Status LED Behaviour (GPIO48)

| Colour | Pattern | Meaning |
|---|---|---|
| 🔴 Red | Slow breathe | Not paired with body bridge |
| 🟡 Amber | Pulse | Paired, no recent heartbeat |
| 🟢 Green | Gentle breathe | Active ESP-NOW link |
| ⚪ White | 400ms flash | NFC tag detected |
| 🔵 Cyan | 800ms flash | Touch/connection event |

---

## Interaction Sequence

1. **Proximity detected** (VL53L0X < 300mm): vibration motor ramps up quadratically; LED shifts teal→red
2. **NFC tag contact**: motor fires double-click (2 pulses, 40ms on/30ms off); LED flashes white
3. **Silence period** (3 seconds): motor off regardless of proximity; LED stays teal; new connections blocked
4. **Separation** (object leaves field AND silence expired): returns to idle

---

## ESP-NOW Pairing

1. Note body bridge MAC from its serial output: `Body bridge MAC: XX:XX:XX:XX:XX:XX`
2. On hand unit serial: `espnow scan` — broadcasts PING, waits for PONG
3. Or manually: `espnow bind XX:XX:XX:XX:XX:XX`
4. Pairing stored in NVS — survives power cycles

---

## CLI Commands (serial monitor, 115200 baud)

| Command | Action |
|---|---|
| `status` | All sensor and system state |
| `debug` | Verbose pin/state dump |
| `stream` | Toggle VL53L0X distance stream |
| `scan` | I2C bus scan |
| `nfc` | Re-run NFC init + 3s tag poll |
| `rainbow` | LED rainbow animation |
| `solid <r> <g> <b>` | Solid LED colour |
| `bright <0-255>` | LED brightness |
| `off` / `idle` | LED off / return to proximity mode |
| `motor <n>` | Fire n-pulse motor burst |
| `sleep` | Immediate deep sleep |
| `battery` | Read battery voltage |
| `espnow` | ESP-NOW status |
| `espnow scan` | Discover body bridge |
| `espnow bind <MAC>` | Manual peer set |
| `espnow clear` | Clear stored pairing |
| `espnow test` | Send test packet |

---

## Config.h Parameters

| Define | Default | Description |
|---|---|---|
| `POWER_MANAGEMENT` | `1` | `0` = always on (debug), `1` = full sleep/wake |
| `NFC_SLEEP_MS` | `0` | PN532 PowerDown between polls (0=always on, 200=~8mA avg) |
| `SILENCE_MS` | `3000` | Post-connection motor silence in ms |
| `PROX_FAR_MM` | `300` | Proximity detection range |
| `PROX_NEAR_MM` | `40` | Near threshold (motor at max) |
| `MOTOR_FREQ` | `20000` | PWM frequency (20kHz = inaudible) |
| `MOTOR_MAX` | `200` | Maximum motor duty (0–255) |
| `POWER_HOLD_MS` | `2000` | Long-press duration to sleep |
| `VBAT_LOW_MV` | `3600` | Battery low warning threshold |
| `VBAT_CRIT_MV` | `3300` | Battery critical → auto shutdown |
| `USE_LONG_PRESS` | `1` | `1`=long press, `0`=double tap to sleep |

---

## File Structure

```
hand-unit/
  platformio.ini
  README.md
  context.md
  src/
    Config.h          — all pin assignments and parameters
    Motor.h           — esp_timer burst, proximity motor
    Proximity.h       — VL53L0X non-blocking reads
    TouchManager.h    — connection state machine (IDLE/RELEASED)
    LedRing.h         — NeoPixel animations
    NfcTask.h         — PN532 UART, FreeRTOS Core 0 task, queue
    EspNow.h          — ESP-NOW pairing, send functions
    StatusLed.h       — GPIO48 status indicator
    PowerManager.h    — deep sleep, button, battery
    Cli.h             — serial command handler
    main.cpp          — setup() + loop()
```