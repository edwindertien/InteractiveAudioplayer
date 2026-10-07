# Hand Unit — Development Context & Handover

ESP32-S3 Super Mini hand unit for KEIKEN immersive wearable. Part of a two-device system: hand unit (this) + body bridge (separate ESP32-S3). Body bridge receives ESP-NOW packets and forwards to Teensy 4.1 audio/show-control unit.

---

## Architecture

- **Core 0** (FreeRTOS task `nfcTask`): PN532 UART polling, 50ms timeout per poll, posts UIDs to `nfcQueue`
- **Core 1** (Arduino `loop()`): VL53L0X reads, LED animation, motor, ESP-NOW, CLI, touch state
- **esp_timer ISR**: motor burst timing, truly independent of both cores
- **Single I2C bus** (Wire, GPIO7/8): VL53L0X only
- **UART1** (GPIO5=RX, GPIO6=TX): PN532 only
- All modules as header-only `.h` files included from a single `main.cpp`

---

## Hard-Won Lessons

### PN532 UART Initialisation (critical)

The Seeed PN532 library must be used as follows on ESP32:

```cpp
#define NFC_INTERFACE_HSU
#include <PN532_HSU.h>
#include <PN532_HSU.cpp>   // include .cpp directly — build flags don't reliably propagate
#include <PN532.h>

HardwareSerial MySerial(1);
PN532_HSU pn532hsu(MySerial);  // NO rx/tx args in constructor
PN532     nfc(pn532hsu);
```

Init sequence (order matters):
```cpp
gpio_hold_dis((gpio_num_t)NFC_RST_PIN);  // release sleep hold first
gpio_hold_dis((gpio_num_t)NFC_TX_PIN);
pinMode(NFC_RST_PIN, OUTPUT);
digitalWrite(NFC_RST_PIN, LOW);
{ uint32_t t=millis(); while(millis()-t<100){} }  // RST LOW, no delay()
pinMode(NFC_RST_PIN, INPUT);             // high-Z — module pull-up brings RST HIGH
{ uint32_t t=millis(); while(millis()-t<50){} }   // wait for boot
MySerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
nfc.begin();                             // call AFTER MySerial.begin — keeps our pins
uint32_t ver = nfc.getFirmwareVersion();
```

**Why `INPUT` (high-Z) instead of `digitalWrite(HIGH)`:**  
After deep sleep with `gpio_hold_en(RST)`, releasing the hold and then calling `pinMode(OUTPUT)` + `digitalWrite(HIGH)` sometimes fails because the ESP32 output latch race. Setting `INPUT` mode lets the module's own pull-up bring RST HIGH reliably.

**Why `#include <PN532_HSU.cpp>`:**  
The Seeed library wraps all implementation in `#ifdef NFC_INTERFACE_HSU`. PlatformIO build flags (`-DNFC_INTERFACE_HSU`) do not reliably propagate to library `.cpp` files. Including the `.cpp` directly in the source file ensures the `#define` is visible when the code compiles.

**Why `nfc.begin()` AFTER `MySerial.begin()`:**  
`PN532_HSU::begin()` calls `_serial->begin(115200)` with **no pin arguments**. On ESP32, `HardwareSerial::begin()` with `-1` pins treats it as "keep current pins". So the order must be: set pins via `MySerial.begin(baud, config, rx, tx)` → then `nfc.begin()` which re-calls with `-1/-1` and preserves our assignment.

**Confirmed NOT needed:** wakeup preamble (16× `0x55`) when RST is wired. After a clean RST pulse the PN532 boots into a known state.

### PN532 Pins
```
NFC_TX_PIN = 6   (ESP32 TX → PN532 RXD)
NFC_RX_PIN = 5   (ESP32 RX ← PN532 TXD)
NFC_RST_PIN = 10
```
Pin 21 is NOT populated on the ESP32-S3 Super Mini.

### Deep Sleep / gpio_hold
- `gpio_hold_en()` latches a pin's state through deep sleep (survives full chip reset)
- Must call `gpio_hold_dis()` on wake before `pinMode()`/`digitalWrite()` work again
- Sequence for RST: `gpio_hold_en(RST)` → sleep → wake → `gpio_hold_dis(RST)` → `INPUT` mode → pull-up brings HIGH
- For `powerDownPeripherals()`: must set `pinMode(RST, OUTPUT)` BEFORE `digitalWrite(LOW)` because `nfcSetup()` leaves RST in `INPUT` mode

### LED Animation and NFC Interference
The cycling LED animation using `sinf()` with `TWO_PI` (a `double` constant) caused the NFC task on Core 0 to fail. Root cause: `TWO_PI` promotes arithmetic to double precision; on ESP32-S3 the FPU is single-precision only, so double math runs in software and was interfering with task scheduling.

**Fix:** Replace per-LED `sinf()` with FastLED's `sin8()` (integer lookup table, zero FPU):
```cpp
uint8_t s = sin8(phaseBase + i * (256 / LED_COUNT));
```

### FastLED and WiFi/ESP-NOW
`FastLED.show()` uses RMT peripheral. `esp_wifi_stop()` resets RMT. Always clear LEDs **before** stopping WiFi, never after.

### VL53L0X Crosstalk
Values below 50mm are crosstalk artifacts from the sensor's own laser reflecting off cover glass. Treat `d < 50` the same as `d >= 8190` (no target):
```cpp
if (d >= 8190 || d < 50) lastDist = 9999;
```
Also: `tof.setSignalRateLimit(0.5)` reduces crosstalk false positives (default 0.25).

### Motor PWM
- 25kHz does NOT work on this board — use 20kHz
- `motorProxSet()` is ignored while a burst is active (checked via `motorBurst.active`)
- esp_timer ISR drives bursts independently of loop() — burst timing is precise regardless of NFC poll blocking

### ESP-NOW Link Status
`onSent` callback with `ESP_NOW_SEND_SUCCESS` only confirms the WiFi frame was **transmitted**, not received. Use heartbeat packets from the body bridge to determine true link state:
- Body bridge sends `MSG_HEARTBEAT` every 1 second
- Hand unit goes GREEN on status LED when heartbeats arrive
- Hand unit reverts to AMBER after 2 seconds without heartbeat

### Battery ADC
Use `analogReadMilliVolts()` (eFuse-calibrated Vref) not raw `analogRead() × 3300/4095`. The 11dB attenuation range saturates at ~3.1V not 3.3V, causing ~7% overestimate with the raw formula.

### I2C Bus Assignment
VL53L0X uses the global `Wire` object (Pololu library, not configurable). PN532 in UART mode avoids all I2C conflicts. Earlier attempts at dual I2C (Wire + Wire1) caused reliability issues with PN532 on Wire1 under concurrent access.

---

## Heat Reduction

The ESP32-S3 runs hot at its default 240MHz. Three changes applied to both hand unit and body bridge:

```cpp
// setup() — first line
setCpuFrequencyMhz(80);   // 80MHz sufficient for all use cases

// espNowSetup() — after WiFi.disconnect()
WiFi.setSleep(true);      // modem sleeps between ESP-NOW packets

// loop() — last line
vTaskDelay(pdMS_TO_TICKS(1));  // yields to idle task → CPU light sleep
```

All hardware peripherals (UART1, Wire, RMT/FastLED, LEDC, esp_timer, ADC) operate from independent peripheral clocks and are unaffected by CPU frequency. ESP-NOW throughput at 10 packets/sec is well within 80MHz capability.

---

## Known Issues / Deferred

- **NFC_SLEEP_MS > 0**: PN532 PowerDown between polls saves ~75mA but requires reliable wakeup preamble after each sleep. Currently set to 0 (always on) for reliability. When enabled, the `inPowerDown` flag in `nfcTask` tracks state to avoid sending preamble when PN532 is already awake.
- **Battery voltage** still reads ~10% high even with `analogReadMilliVolts()`. A trim factor may be needed after measuring against a multimeter.
- **Body bridge → Teensy UART**: stubbed with `// TEENSY_UART.printf(...)` comments in `body-bridge/src/EspNow.h`. JSON format defined: `{"t":"n","u":"DE0CC698","d":150}`.
- **UWB positioning**: evaluated Bitcraze LPS for 4-robot tracking, not yet implemented.

---

## Body Bridge

Companion device (`body-bridge/` project), same ESP32-S3 Super Mini hardware.

**ESP-NOW messages received from hand unit:**

| Type | Value | Payload |
|---|---|---|
| `MSG_PROXIMITY` | `0x01` | distMm, touchState |
| `MSG_NFC` | `0x02` | distMm, touchState, nfcUid |
| `MSG_TOUCH` | `0x03` | distMm, nfcUid (connection event) |
| `MSG_HEARTBEAT` | `0x12` | (empty, sent every 1s to hand unit) |
| `MSG_PING` | `0x10` | discovery broadcast |
| `MSG_PONG` | `0x11` | discovery reply |

Pairing stored in NVS under namespace `"bodybridge"`, key `"handMac"`.

---

## Development Sequence (for next session)

1. Confirm full system working: NFC + VL53 + LED + motor + ESP-NOW + sleep/wake
2. Add `NFC_SLEEP_MS 200` once RST/wake cycle is confirmed stable
3. Wire and enable body bridge → Teensy UART forwarding
4. Tune SILENCE_MS and proximity thresholds for performance use
5. Consider `POWER_MANAGEMENT` sleep in body bridge when hand unit is off