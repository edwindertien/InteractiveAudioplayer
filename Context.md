# Engineering Context & Lessons Learned

What didn't work, why, and what was chosen instead — plus how the system got to its current
shape, what has been verified on hardware, and what is still open. Useful for future
development, porting and debugging. The user-facing manual is [README.md](README.md).

Legend: ❌ tried and abandoned · ✅ works and is kept · ⚠️ open / unresolved · 📜 historical (superseded)

## Contents

1. [Where the design stands](#1-where-the-design-stands)
2. [Audio](#2-audio)
3. [SD card, files and USB maintenance mode](#3-sd-card-files-and-usb-maintenance-mode)
4. [Configuration and show logic](#4-configuration-and-show-logic)
5. [LED](#5-led)
6. [ESP32 firmware and toolchain](#6-esp32-firmware-and-toolchain)
7. [How changes were verified](#7-how-changes-were-verified)
8. [Platform considerations](#8-platform-considerations)
9. [Open issues and next steps](#9-open-issues-and-next-steps)
10. [Housekeeping](#10-housekeeping)

---

## 1. Where the design stands

* **Player:** Teensy 4.1. Headphone audio from a PCM5102A on I²S 1, haptics from a MAX98357A on I²S 2.
  One 8-channel WAV on the built-in SD slot; short effects in RAM; one LED ring that follows the haptic
  signal. USB gives a serial console and an MTP maintenance mode.
* **Wearer:** an ESP32-S3 hand unit (PN532 NFC, VL53L0X distance, vibration motor, LED ring, LiPo).
* **Link:** hand unit ⇄ body bridge (ESP32-S3) over ESP-NOW; body bridge → Teensy over UART as JSON lines.
* **Experience:** ten 45-second chapters; tags have roles (start, person, narrator, overlay) with spares;
  a timed ending wraps the show up and switches the player off.
* **Content pipeline:** stems → `assemble_71.sh` → `experience.wav`; tag UIDs → `tag_catalogue.json` →
  `generate_player_configs.py` → seven `config.json`.

The figures in `docs/` (system overview, audio graph, show flow, startup sequences, …) are the quickest
way into the structure; see the README.

---

## 2. Audio

### ✅ One interleaved 7.1 file instead of several players

**Tried:** three `AudioPlaySdWav` objects at once (base + 2 overlays), one WAV each.

**Happened:** CPU rose from ~1 % to ~30 % per extra player (37 % with two overlays). `AudioProcessorUsage()`
counts time inside the audio interrupt *including SD wait states*; with concurrent readers the SDIO bus
serialises the reads and the wait shows up as CPU. Enough players caused glitches and crashes.

**Root cause:** at the time (Teensyduino 1.60) the Teensy 4.1 SDIO driver does not do concurrent reads
efficiently — each `AudioPlaySdWav::update()` blocks until its read completes.

**Solution:** a single 7.1 interleaved WAV read by `AudioPlaySdWavMulti`: one SD read per interrupt feeds
all eight channels. CPU is ~4 %.

### ✅ Keep `AUDIO_BLOCK_SAMPLES = 128`

Doubling it to 256 (to halve SD read frequency) made the audio severely degraded — bit-crushed, almost
unrecognisable. The I²S driver and the audio library are calibrated for 128-sample blocks. Treat it as fixed.

### ✅ Seeks happen inside the audio interrupt

**Tried:** calling `multiPlayer.seekMs()` from `loop()` in the seek crossfade state machine.

**Happened:** after several tag interactions the player silently stopped (`pos:0`, `CPU:0.0%`, `mem:0`);
effects from RAM still worked.

**Root cause:** a race. `seekMs()` called `_file.seek()` from `loop()` while `update()` in the interrupt also
used `_file`.

**Solution:** *deferred seek*. `seekMs()` only sets `_seekPending` / `_seekTargetMs` (`volatile`); the real
`_file.seek()` happens in `update()`, single-threaded. A second bug in the same area: when a once-chapter
ended naturally (`_playing = false`) a later seek request was dropped because `update()` returned before
checking the flag. The pending seek is now processed first, and a seek sets `_playing = true`.

### ✅ Edge-detect `isPlaying()` (no restart loop)

`isPlaying()` is false for a few milliseconds after a file ends while the last buffer drains. A naive
`if (!player.isPlaying()) player.play(FILE)` in `loop()` called `play()` thousands of times, corrupting state
and opening the file repeatedly (a loud click on every loop restart). Track the previous state and act on the
falling edge only.

### ✅ `AudioPlayMemory` header format

Effects played as a ~1 ms click regardless of length when the header carried a block count. `update()`
decrements `length` by `AUDIO_BLOCK_SAMPLES` per tick and stops below 128, so `length` is in **samples**.
Header: `(0x81 << 24) | (numSamples & 0x00FFFFFF)`; bits 31–24 = type (0x81 = 16-bit PCM 44.1 kHz), two
`uint16_t` words in little-endian order.

**Consequence for effect files:** `RamPlayer` always writes type 0x81 (mono, 44.1 kHz) and does not resample.
A stereo file plays at half pitch and double length; a 48 kHz file plays slowly. Effects must be 16-bit mono
44.1 kHz, and are clamped to 180 000 bytes (~2 s).

### 📜 Haptic output through PWM, DAC and MQS

The first haptic output was a PWM hack; it is **replaced by a MAX98357A on I²S 2**. Kept as history:

* `AudioOutputAnalog` — the Teensy 4.1 has **no DAC**; on 4.x that class is PWM. `AudioOutputPWM` is the right class.
* `AudioOutputPWM` on pins 3 + 4 with a 1 kΩ + 270 kΩ resistor network and an RC filter: only the LSB pin
  reliably fired its DMA; pin 3 did not. The library had to be patched to `begin(2, 4)` (and `begin()` is
  private). Output sounded bit-crushed — fine for a haptic transducer, not for audio.
* `AudioOutputMQS` — fixed pins 10 and 12 collide with the SPI pins the (since removed) Audio Shield used.
* PWM output was also sensitive to `FastLED.show()` timing — see [LED](#5-led).

### ✅ Dual I²S: PCM5102A (headphones) + MAX98357A (haptics)

The Audio Shield (SGTL5000) was replaced by two small modules; no codec object or I²C is needed.

* **PCM5102A** — I²S 1: MCLK 23, BCLK 21, LRCLK 20, DIN 7; SCK grounded. The module's four solder jumpers
  matter: the factory default bridges `H3L` (XSMT, soft-mute) to **LOW = muted**. Bridge it to **HIGH**.
  `H4L` (FMT) must stay LOW (I²S). `H1L` (FLT) and `H2L` (DEMP) to L. The symptom of a wrong `H3L` is *no
  sound at all* with a perfectly healthy software chain.
* **MAX98357A** — I²S 2 (`AudioOutputI2S2`): LRC 3, BCLK 4, DIN 2. Tie **SD to VIN**; the pin has an
  internal pull-down, so a floating SD means shutdown. GAIN floating = 9 dB. The mono haptic signal is
  duplicated to both channels.

### ✅ Haptic soft limiter (waveshaper)

The haptic output felt softer after the move from the PWM path + amp. Instead of raising the mixer gain
(which hard-clips at 16 bit) a soft limiter sits between `hapticMixer` and the I²S 2 output.

* `AudioEffectWaveshaper` with the curve `y = tanh(k·x) / tanh(k)`: full scale in is full scale out, quiet
  parts get louder (small-signal gain `k / tanh(k)`; k = 2.5 gives +8.1 dB), peaks round off.
  `hdrive 0` is a straight line (bypass). Default `HAPTIC_DRIVE_DEFAULT = 0.5` — the value that was chosen
  by ear on the hardware.
* **An `AudioEffectWaveshaper` with no table outputs silence.** `setup()` must install a table before audio
  is expected.
* **`shape()` frees and reallocates the table the audio interrupt reads**, so a change at runtime has to be
  wrapped in `AudioNoInterrupts()` / `AudioInterrupts()`.
* The library requires a table length of 2ⁿ + 1; 1025 points are used.
* The LED ring's `level` pattern is tapped from the mixer **before** the limiter, so changing `hdrive`
  does not change the light.

### ✅ Ducking: effects over the general audio

Effects played from RAM were hard to hear over the stems. While an effect plays, the stems are lowered.

* The ducking envelope multiplies **`mainMixer` channel 0** (stems), next to the seek fade:
  `gain = stemGain × duck`, written every 20 ms tick. (Previously channel 0 was written only during fades.)
  It is a separate factor so the seek crossfade is undisturbed.
* Attack 40 ms, release 400 ms (`DUCK_ATTACK_MS`, `DUCK_RELEASE_MS`); detection is `ramFx.isPlaying()`, so
  every effect — feedback sounds, chapter-entry and tag-triggered ones — ducks automatically.
* `duck_level` (config) or `duck <0-1>` (CLI). `1` = off, which is exactly the old behaviour.
* The haptic path is not ducked.

### ❌ A headphone gain stage (tried, reverted)

**Asked:** more headphone volume. **Built:** a second soft limiter pair after `mainMixer` with a tunable gain
(`hp <gain>`), plus a peak monitor. It was placed *after* the mixer because the seek fades overwrite
`mainMixer`'s own gain on every chapter change, so a master volume there would be lost.

**Measured** (1 kHz sine through the real transfer curve): at +3.5 dB the output stays clean (0.01 % THD) up
to a peak of −5.5 dBFS, then rises (≈ 7 % at −2 dBFS, ≈ 15 % at 0 dBFS); at +6 dB it is clean up to −8 dBFS.

**Outcome:** reverted — the headphones turned out to have a built-in amplifier. If more level is ever needed,
the cleaner route is a hotter, properly limited master of the stems; a static soft limiter cannot do what a
look-ahead limiter does.

---

## 3. SD card, files and USB maintenance mode

### ✅ macOS hidden files on FAT32

Finder writes `._filename` next to every copied file, plus `.DS_Store`, `.Spotlight-V100`, `.fseventsd`.
They fill the FAT directory table, slow lookups and can confuse the SD library. Always run
`dot_clean /Volumes/CARD/` after copying (or copy with `cp`). The config loader also ignores any name that
starts with a dot, so a `._player2_config.json` is never mistaken for a config.

### ✅ Slow SD cards

A generic Class-4 card gave irregular vinyl-like clicks even with one player. Class 4 (4 MB/s minimum) is not
enough for real-time streaming plus directory lookups. Use SanDisk Ultra or Extreme (A1/A2, UHS-I) — random-read
IOPS matter, not only sequential speed.

### ✅ WAVE_FORMAT_EXTENSIBLE (65534) must be accepted

ffmpeg writes multi-channel WAVs with format tag 65534, not 1. The parser accepts both (the samples are
16-bit signed PCM either way).

### ✅ `amerge`, not `join`

`join` is meant for upmixing and may mix inputs (haptic content bled into the overlays). `amerge=inputs=N`
interleaves strictly in the order given.

### ✅ `assemble_71.sh` is defensive

Two stereo files where mono was expected used to produce a **10-channel** file that still reported "Done".
The script now probes each stem, converts mono ↔ stereo where that is safe, refuses more than 2 channels,
verifies the result is 8 channels at 44.1 kHz and deletes it otherwise, and never touches an existing output
if it fails before writing. Two details worth knowing:

* **`amerge` cuts the output to the shortest input.** A 452 s stem next to 450 s stems silently loses 2 s.
  The script now warns when lengths differ by more than 0.1 s.
* It runs under macOS's default **bash 3.2**: no `${var,,}`, associative arrays or `mapfile`.

### ✅ The config file can be named `*_config.json`

The loader opens `/config.json`; if that does not exist it uses the single `*_config.json` in the card root
(case-insensitive; folders and dot-files ignored). Several candidates → it refuses to guess. This lets a
card be labelled `player3_config.json`.

### ✅ USB maintenance mode (MTP)

The SD card is hard to remove from the Teensy. The `usb` command stops playback (closing `experience.wav` — the
only file the player keeps open), starts MTP and shares the card; `reboot` leaves the mode and reloads the config.

* USB type `USB_MTPDISK_SERIAL` gives a real CDC serial port **and** MTP. PlatformIO's Teensy platform 5.1.0
  knows this flag (so it does not also add `-DUSB_SERIAL`).
* ❌ **Do not add KurtE's `MTP_Teensy` library.** The **Teensyduino 1.60 core already contains MTP**
  (`cores/teensy4/MTP_Teensy.h`, pulled in by `Arduino.h`). The library redefines `MTPStorage` and `MTP_class`
  → "redefinition" errors. The earlier release notes ("you also need the MTP_Teensy library") describe the
  1.57 era.
* ❌ **FastLED 3.10.2 / 3.10.3 break the build with the MTP USB type.** Their SD-card source includes the core's
  `fs.h` *before* `Arduino.h`; with MTP enabled, `fs.h → Arduino.h → MTP headers → needs FS`, which is not yet
  declared. It only triggers on a case-insensitive filesystem (FastLED probes `<fs.h>`, the core's file is
  `FS.h`), i.e. macOS. Reproduced with the real core headers: fs.h-first + MTP gives exactly the error seen;
  Arduino.h-first, or MTP off, compiles. **Pinned to FastLED 3.10.1**, which never compiles that file.
  (Forcing `-include Arduino.h` also fixes it in the reproduction but would hit every C and assembler file.)
* The MTP class has no `end()`, so leaving the mode is a reboot.
* PJRC labels MTP as experimental and notes that simultaneous access by the PC and the Teensy is limited — hence
  the dedicated mode that stops playback first. macOS has no native MTP support.

---

## 4. Configuration and show logic

### ✅ NFC and the tap ring moved off the Teensy

The local reader (`NfcReader.h`) and its 24-LED tap ring are gone. Tags are read by the wearer's ESP32-S3 hand
unit and forwarded (ESP-NOW → body bridge → UART JSON, `EspBridge.h`). Feedback — LED flash, vibration
double-click — happens on the hand unit, at the point of contact. This also removed the second I²C consumer
from the Teensy and freed pins.

**Dispatch semantics.** The hand unit sends `p` (proximity, 10 Hz, informational), `n` (raw detection, logged
for UID discovery only) and `c` (confirmed connection). Only `c` dispatches. The hand unit's touch state machine
already debounces and enforces a post-connection silence, so `c` fires once per genuine tap; the Teensy's
per-tag `cooldown_ms` is a second, optional safety net. `EspBridge.h` parses lines with ArduinoJson (already a
dependency); a torn line returns an error instead of being misread.

### ✅ LED config schema simplified; `led_brightness` fixed

Chapters once had three LED parameter sets (`led_background`, `led_enter` for the removed tap ring, `led_beat`).
They became two: `led_background` (the one ring's persistent pattern, usually `level` or `heartbeat`) and
`led_enter` (a one-shot on entry). A tag's `"do": "led"` action still works. In passing, a bug was fixed:
`led_brightness` was parsed but never applied — `leds.setGlobalBrightness(cfg.ledBrightness)` is now called
after the config loads.

### ✅ Several UIDs per tag role (spares)

The catalogue calls for **39 physical tags per card** (3 per person × 7, 3 start, 3 narrator, 6 + 6 overlay), but
the loader holds 32 tag entries. A tag entry can now list several UIDs (`"uids"`, up to 8), so a card needs
11 entries; the old single `"uid"` still works and the two merge. UIDs are stored upper-case; the same UID
under two roles gives a warning and the first entry wins.

`tag_catalogue.json` is the single source for every UID; `generate_player_configs.py` validates it (4- or
7-byte hex, no duplicates across roles, at most 8 per role, no unknown keys) and writes nothing if anything is
wrong. Empty strings are unprogrammed spare slots.

### ✅ Tag types, the person-tag lock and the sound roles

`type` is `person`, `start`, `location` or the legacy `device` (descriptive before; now it drives behaviour).

* **`person`** tags start a chapter. With `encounter_lock` they are accepted only while the base chapter plays;
  otherwise they are refused with the *denied* sound. The generator's default is the testing behaviour (switch
  at any time); `--lock` builds the show version. Narrator, overlay and start tags are never locked.
* **Three feedback sounds**, slots configured under `sounds`: *connect* (a person tag accepted), *denied* (refused),
  *ready* (an encounter ended and it returned to base). A slot of `-1` is silent.

### ✅ Start tag and the timed ending

The **start** tag begins the show clock. `end_after_min` later the show wraps up. Design choices:

* **Finish the current chapter first.** A once-chapter simply plays on. A *looping* chapter (`base`) has no end
  of its own, so it is **released**: `setPlayOnce(start, loopEnd)` with the chapter's own times flips the player's
  stop-at-end flag mid-play, and it stops cleanly at its loop end. The finish detection treats a released loop
  like a once-chapter.
* **No new encounters while ending** (person tags are refused with the *denied* sound); otherwise taps could
  extend the show indefinitely. Narrator and overlay tags still work.
* **End chapter, then "show over".** When `end_reflection` finishes (however it was reached) the player stops,
  overlays are cleared and the LED ring is set to `off`. Only the start tag is accepted; it restarts the show
  from `base` with a fresh clock. A start tag during the ending also restarts. No "ready" sound when going to the
  end chapter.
* **A missing end chapter** (typo) would otherwise retry every loop iteration; instead the show ends cleanly
  without it, and the boot log warns.
* The clock runs from the **start tag**, not from boot (tested: a unit booted 20 minutes earlier still gets its
  full 10 minutes).
* Scope: it switches off the **Teensy's** sound and ring. Hand units and body bridges keep running.

---

## 5. LED

### ✅ `FastLED.show()` must be rate-limited

Un-throttled `show()` calls from `loop()` made the PWM haptic output stutter in time with LED frames (the PWM DMA
was more sensitive than I²S, which has a hardware FIFO). Hard cap at 25 fps (`LED_FRAME_MS = 40`) and `yield()`
after each `show()`. The haptic output is now on I²S 2, but the throttle was kept.

### ✅ Very low intensities look like "off"

`breathe` at `intensity: 0.12` fell below the LED's visual threshold for ~10 s per cycle. Fixes: a 25 % floor in
the breathe range (`0.25 + t × 0.75`), an absolute floor in the colour helper (`0.05 + b × 0.95`), idle intensity 0.35.

### ✅ Don't reset an animation's phase when only its parameters change

Calling `setBackground()` on every chapter entry restarted the running animation (a visible brightness jump).
Phase and step now reset only when the animation *type* changes.

### ℹ️ `led_animations.json`

Custom animations are loaded at boot and checked **before** the built-in names. The shipped file defines one
(`clockwise_chase`) that no config references; it costs a small SD read and some reserved RAM. It can be deleted
from the cards safely (the boot log says custom animations are unavailable).

---

## 6. ESP32 firmware and toolchain

### ✅ Pin the ESP32 platform: `platformio/espressif32@6.13.0`

The toolchain moved under this project several times in a few months:

1. **Arduino-ESP32 core 3.0 (ESP-IDF 5.x)** is a major version. It changed the **ESP-NOW callback signatures**
   (receive *and* send, not in lockstep — one intermediate pioarduino release had the new receive signature and
   the old send signature) and replaced the **LEDC (PWM) API** (`ledcSetup/ledcAttachPin` → `ledcAttach`). Both are
   in Espressif's 2.x→3.0 migration guide. The code carries compile-time switches (`ESPNOW_OLD_API`, a
   `MOTOR_LEDC_NEW_API` auto-detect) for both generations.
2. PlatformIO's *official* platform lagged on 3.x support, which is why the community **pioarduino** fork exists.
   Adopting it brought its own problems: a bundled **esptool 5.4.0 that crashed on upload**, a stray `pioarduino`
   PyPI package conflicting with `platformio` in the same venv, and PIO Home showing the wrong branding (fixed
   by reinstalling the `platformio` package). pioarduino was dropped.
3. `platformio/espressif32@6.13.0` (the **official** platform) compiles and flashes. Its numbering is PlatformIO's,
   not Arduino's: the build log shows `framework-arduinoespressif32 @ 3.20017.241212` = **Arduino core 2.0.17**
   (ESP-IDF 4.4), with esptool 4.11.
4. ❌ **An unpinned `platform = espressif32` does not reproduce an old environment.** "Latest" moves with the
   registry; in practice PlatformIO re-used the cached 6.13.0, so removing the pin changed nothing.

Version-tied lessons: rule out toolchain changes by reading the build log's `PLATFORM:` and `framework-…` lines
before theorising about firmware.

### ✅ ESP32-S3 SuperMini variants: use the standard board, not v0.0.2

Two board versions are in circulation. The **v0.0.2** (supposedly corrected USB OTG) behaves differently — notably a
different **status-LED colour order**. The default in the firmware is **GRB** (standard board); a v0.0.2 needs
`-DSTATUS_LED_ORDER=RGB`. Mixed boards on the bench explained a long stretch of wrong LED colours (red/green swapped)
and was suspected in the next item.

### ✅ One-directional ESP-NOW failure — gone with standard boards (mechanism not isolated)

**Symptom:** hand → body worked (broadcast and unicast reached the body), body → hand failed: every body unicast
logged `send callback: FAIL`, even at boot to a stale stored peer. PONGs and heartbeats never reached the hand unit,
so it stayed unpaired.

**Ruled out (no change):** platform version, CPU frequency (80 vs 240 MHz), `peer.channel = 0` vs the Wi-Fi
channel, forcing a common channel with `esp_wifi_set_channel`, removing the explicit `WiFi.setSleep(false)`,
and un-pinning the platform (it resolved to the same cached 6.13.0). The `git` history of the project showed the
original working code was simply `WiFi.mode(WIFI_STA); WiFi.disconnect();` then `esp_now_init()`.

**What changed it:** swapping boards — a **v0.0.2 board had ended up on the test bench** next to standard ones. Once the
test units were standard boards, pairing with `espnow scan`, PONG, heartbeat and tag messages all worked in both
directions.

**Honest status:** the exact failing mechanism was not isolated. A web search turned up reports that some
SuperMini boards ship with the ceramic antenna mounted backwards (the feed trace lands on the unconnected pad), which
would fit a transmit-only failure, but that was never confirmed on these boards. If it ever returns, inspect the
antenna orientation and test with a different board before touching software.

### ✅ ESP-NOW behaviours to remember

* Unicast needs a peer; broadcasts work without one. The hand unit falls back to broadcast for NFC/touch while it
  considers itself unpaired.
* **A body bridge must reject data until it is paired**, and only from its paired MAC. It once accepted tag and
  proximity packets from any hand unit while "not paired" (the filter was `handPaired && mismatch`, which is a no-op
  when unpaired); the check is now `!handPaired || mismatch`. `lastRxMs` (link liveness) is only updated by the
  paired hand, otherwise foreign traffic would keep the LED green.
* Pairing: `espnow scan` (PING broadcast → PONG, both store the MAC) and a button method (body long-press 2 s →
  PAIR_REQUEST broadcast every 300 ms for 30 s → the hand's tap → PAIR_ACK).

### ✅ Hand-unit power management

* A reset or power-up goes to deep sleep; the button (GPIO 9, ext1 wake) starts it. A tap < 2 s is a "short press"
  (confirms a pending pairing); ≥ 2 s sleeps.
* Pins that must hold their level through sleep (button, PN532 reset / TX, VL53L0X XSHUT) are latched with
  `gpio_hold_en`, and every hold must be released with `gpio_hold_dis` after wake before `pinMode`/`digitalWrite`
  work again.
* The PN532 needs its reset line (GPIO 10) to recover a stuck TX line; `NFC_SLEEP_MS = 0` keeps it always on.
* The CPU runs at 80 MHz — 240 MHz ran hot.

### ✅ Battery measurement: the divider is on the 5 V rail

**Symptom:** the hand unit logged `[BAT] 4772 … 4882 mV` — above anything a 1S LiPo reaches (4.2 V) — and the log
only exists on USB.

**Cause:** the divider on GPIO 3 hangs on the board's **5 V rail**, which is the USB 5 V when plugged in, or the cell
voltage **minus a Schottky diode** (about 0.3 V) when on battery. Measured on one unit: cell 3.80 V, pin 1.74 V →
rail 3.48 V → offset 320 mV.

**Fix in firmware:** estimated cell voltage = rail + a per-unit offset (default 320 mV, `battery cal <batV> <pinV>`,
stored in flash); a rail above 4.3 V means USB, reported as "battery not measurable" and never triggers LOW or
CRITICAL. Thresholds (3600 mV low, 3300 mV critical → sleep) apply to the corrected cell voltage. Caveats: a fixed
offset is good to about ±0.1 V because the diode drop varies with load; there is no charge indication on USB.

**The proper hardware fix** is to move the divider's top to the BAT+ pad (true cell on USB and battery) and add
about 100 nF from GPIO 3 to ground — a 50–110 kΩ divider reads a little low on the ESP32's ADC without it (not checked
against Espressif's guidance).

### ⚠️ VL53L0X: one unit still not working properly

**Symptom:** some hand units reacted only at very short range.

**Library facts (Pololu VL53L0X):** `init()` follows ST's sequence (factory reference SPADs, tuning table, VHV and phase
calibration) but not offset or crosstalk calibration (the library says reference-SPAD management is done by ST and
"should work well enough unless a cover glass is added"). The default signal-rate limit is 0.25 MCPS; this project used
0.5, and ignores readings under 50 mm as crosstalk.

**Diagnostics added** (`tof …`): per measurement the device status code (ST's names: 4 MSRC no target, 6 range phase
check, 9 phase consistency, 11 range complete, …), the return signal rate, the ambient rate and the SPAD count; the
window, rate limit, timing budget, period and long-range mode are tunable and storable per unit.

**What the data showed** (empty room, then a hand waved at 10–15 cm):

| | good unit | bad unit |
|---|---|---|
| ambient rate | ≈ 0.4 MCPS | 12–15 MCPS (about 30×) |
| idle SPAD count | 194 | 181 |
| return signal with a hand | 20–28 MCPS peaks, 10–12 valid readings per wave | ≤ 4.7 MCPS, almost no valid readings |
| empty room | clean | a few false "valid" readings (0 mm, 154–591 mm) |

The ambient rate and SPAD count are measurements, not settings; no register value is known that fixes them. The bad
module was replaced; **one unit is still not working completely** (to do). Next checks: reboot several times and note
the ambient each time, swap modules between a good and a bad unit, test with the ring off and the motor idle.

### ⚠️ GPIO 48: a second (red) LED shares the pin with the RGB LED

On the SuperMini schematic the onboard WS2812 (`LE1`) *and* a separate red LED (`LED1`, via a 1 kΩ `R1` to GND) are
on GPIO 48. On two of seven hand units the red LED glows faintly when the unit should be off. All seven run the same
sleep code (which never drives or holds GPIO 48 — the pin floats), so the cause is probably the board: the schematic
shows an **optional 10 kΩ pull-up `WSR1` (marked NC)** from 3 V3 to the data line, and a fitted one would push roughly
0.1 mA through the red LED. **Test not yet done:** hold the chip in reset and see if the glow stays (hardware) or goes
(firmware pin state). Fixes: remove `WSR1` / `LED1` / `R1`; driving the pin low and holding it during sleep also works
but wastes ~0.3 mA through a fitted pull-up.

---

## 7. How changes were verified

This project is developed with an AI assistant in an environment that **cannot flash hardware or compile for the
Teensy/ESP32 targets**. Verification therefore has two tiers, and this document records which is which.

* **On hardware (by the user):** reported working.
* **Simulated:** the real source text is compiled on a host against mocks — the config loader with the real ArduinoJson
  and every generated card; the real `ramp`/finish-detection/tag-dispatch/CLI code sliced out of `main.cpp`; the
  waveshaper tables run through the Audio library's own `shape()`/`update()` code on all 65 536 sample values. Several
  suites were also checked with deliberate breakage ("mutation checks") to prove the tests can fail — one test
  harness passed vacuously on its first version and was rewritten.

| Feature | Status |
|---|---|
| ESP-NOW pairing by `espnow scan`, heartbeat, tag dispatch | confirmed on hardware |
| Haptic soft limiter (drive 0.5) | confirmed on hardware |
| Timed ending, show over, restart by start tag | confirmed on hardware ("works in one go") |
| `tof` diagnostics | used on hardware (readings captured) |
| USB maintenance mode (MTP) | builds and flashes; use not reported |
| Spare tags (`uids`), tag catalogue and generator | simulated against all seven generated cards; not reported |
| Person-tag lock, three feedback sounds | simulated; not reported |
| Ducking | simulated; not reported |
| Battery correction + `battery cal` | simulated; not reported |
| Config name `*_config.json` | simulated; not reported |
| `assemble_71.sh` channel handling | tested with synthetic stems incl. the 10-channel accident; not reported |
| **Button pairing** (body long press + hand tap) | implemented; **not confirmed end to end** |

---

## 8. Platform considerations

### Raspberry Pi Pico 2W — evaluated, not chosen

* No Teensy Audio Library equivalent — no `AudioMixer4`, `AudioPlaySdWav`, patch-bay model; everything would be rewritten.
* `BackgroundAudio` (the best Pico audio library) cannot read from SD inside the audio interrupt — it needs main-loop
  feeding, which makes the SD-read-per-interrupt design impossible.
* No audio-shield equivalent — an external I²S DAC is needed.
* 520 KB RAM versus 1 MB — tighter for RAM effects.

The Teensy 4.1 (600 MHz Cortex-M7, 1 MB RAM, mature Audio Library) is much better suited. If a Pico 2W were ever
needed, its built-in Wi-Fi would allow device-to-device sync over UDP — a cleaner basis for a duet mechanic than NFC
proximity.

---

## 9. Open issues and next steps

| # | Item | State |
|---|---|---|
| 1 | One hand unit's VL53L0X still misbehaves | ⚠️ open — see section 6 ("VL53L0X: one unit still not working properly") |
| 2 | Faint red LED glow on GPIO 48 on two hand units | ⚠️ open — reset test not yet run |
| 3 | Button pairing confirmed end to end | ⚠️ open |
| 4 | Move the hand unit's battery divider to BAT+ (+ 100 nF) | optional hardware fix; firmware correction is in |
| 5 | Calibrate `battery cal` on every hand unit (needs a multimeter and an unplugged unit) | to do |
| 6 | Real PCB schematic / board exports into `docs/` (placeholders are in place) | to do |
| 7 | Generate show builds (`--lock`, final `--end-after`), program the spare tags | to do before a show |
| 8 | `tools/editor.py` does not know `uids`, the new tag types or the show settings | known limitation; the generator is the master |

---

## 10. Housekeeping

* `configs/gnerate_player_configs.py` (typo in the name) is a stale duplicate of `generate_player_configs.py` — delete it.
* `tools/card_log.csv` contains only a header line (`timestamp,uid_hex,tag_type`) — probably left over from the removed
  local reader; delete it if nothing uses it.
* `docs/experiencemap.png` is the old figure; the README now uses `docs/experience_map.*`, generated by
  `tools/make_docs.py`.
* The figures are code: change the protocol or the firmware flow, then update the matching function in
  `tools/make_docs.py` and re-run it. Real PCB exports under the placeholder names are never overwritten.
