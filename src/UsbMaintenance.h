#pragma once

// ============================================================
// USB maintenance mode — share the SD card with the PC over MTP
//
// Needs (platformio.ini):
//   build_flags = -DUSB_MTPDISK_SERIAL        // serial + MTP, both at once
//
// No extra library: Teensyduino 1.60 bundles MTP in the core and
// Arduino.h already pulls it in whenever an MTP USB type is selected.
// (Adding KurtE's MTP_Teensy library on top redefines the same classes
// and fails to compile.)
//
// Flow: the 'usb' CLI command stops playback (which closes the
// experience file), then calls usbMaintStart(). From then on loop()
// must call usbMaintUpdate() as often as possible. The MTP class has no
// end(), so leaving the mode = reboot, which also reloads config.json.
//
// Never let playback and MTP touch the card at the same time: the PC
// may replace experience.wav while the player still has it open.
// ============================================================

#include <Arduino.h>      // brings in the core's MTP object when MTP is enabled
#include <SD.h>

#if !defined(MTP_INTERFACE)
  #error "USB maintenance mode needs -DUSB_MTPDISK_SERIAL in platformio.ini build_flags"
#endif

static bool usbMaintOn = false;

inline bool usbMaintActive() { return usbMaintOn; }

// Call once, AFTER playback has been stopped and the file closed.
inline void usbMaintStart() {
    if (usbMaintOn) return;
    MTP.begin();                        // starts the MTP session
    MTP.addFilesystem(SD, "SD Card");   // the card shows up under this name
    usbMaintOn = true;
}

// Call every loop() iteration while active.
inline void usbMaintUpdate() {
    if (usbMaintOn) MTP.loop();
}

// Leave maintenance mode (and reload config.json) by restarting the Teensy.
inline void rebootTeensy() {
    Serial.flush();
    delay(50);
    SCB_AIRCR = 0x05FA0004;             // request a system reset
    while (true) {}
}