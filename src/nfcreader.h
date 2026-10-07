#pragma once

// ============================================================
// NfcReader — unified NFC interface for Teensy 4.1
//
// Select reader via platformio.ini build_flags:
//   -DUSE_PN532   → PN532 over I2C (elechouse/Seeed library)
//   (default)     → WS1850S / MFRC522-compatible over I2C
//
// Both present the same API:
//   NfcReader reader;
//   reader.begin();
//   char uid[20];
//   if (reader.poll(uid)) { /* uid = uppercase hex string */ }
//
// Wiring (both share the same I2C pins):
//   SDA → Teensy pin 18
//   SCL → Teensy pin 19
//   RST → Teensy pin 9
//   VCC → 3.3V
//   GND → GND
//
// PN532 extra: set DIP switches for I2C BEFORE connecting:
//   SEL0 = 1 (HIGH), SEL1 = 0 (LOW)  — see module silkscreen
// ============================================================

#include <Wire.h>

// ── PN532 implementation ──────────────────────────────────────
#ifdef USE_PN532

#include <PN532_I2C.h>
#include <PN532.h>

#define NFC_RST_PIN  9
#define NFC_I2C_ADDR 0x24   // PN532 default I2C address

class NfcReader {
public:
    NfcReader() : _i2c(Wire), _nfc(_i2c) {}

    void begin() {
        // Hard reset the PN532 before init
        pinMode(NFC_RST_PIN, OUTPUT);
        digitalWrite(NFC_RST_PIN, LOW);
        delay(10);
        digitalWrite(NFC_RST_PIN, HIGH);
        delay(50);

        _nfc.begin();

        uint32_t ver = _nfc.getFirmwareVersion();
        if (!ver) {
            Serial.println("[NFC] ERROR: PN532 not found — check wiring & DIP switches");
            Serial.println("[NFC]        SEL0=HIGH SEL1=LOW for I2C mode");
            return;
        }
        Serial.printf("[NFC] PN532 ready — fw %u.%u\n",
                      (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);

        _nfc.SAMConfig();
        _nfc.setPassiveActivationRetries(2);  // default — adjust via setSensitivity()
    }

    // Set detection sensitivity (PN532 only). level 0-5
    // Higher = more retries + longer timeout = better range but slower poll
    //   0 = fastest / shortest range  (retries=1  timeout=10ms)
    //   3 = balanced default          (retries=3  timeout=30ms)
    //   5 = max range / slowest poll  (retries=7  timeout=60ms)
    void setSensitivity(uint8_t level) {
        const uint8_t  retries[] = { 1, 2, 3,  4,  6,  7 };
        const uint16_t timeouts[] = { 10, 15, 30, 40, 50, 60 };
        uint8_t idx = min(level, (uint8_t)5);
        _nfc.setPassiveActivationRetries(retries[idx]);
        _pollTimeout = timeouts[idx];
        Serial.printf("[NFC] Sensitivity level %u: retries=%u timeout=%ums\n",
                      idx, retries[idx], timeouts[idx]);
    }

    // Returns true only when a tag newly appears.
    // Staying on the reader → silent. Removed then re-tapped → fires again.
    bool poll(char* uidOut) {
        uint8_t uid[7]  = {};
        uint8_t uidLen  = 0;

        bool found = _nfc.readPassiveTargetID(
                         PN532_MIFARE_ISO14443A, uid, &uidLen, _pollTimeout);

        if (!found || uidLen == 0) {
            // No card — clear memory so next appearance fires
            if (_tagPresent) {
                _tagPresent = false;
                _lastUid[0] = '\0';
            }
            return false;
        }

        char uidStr[20] = {};
        for (uint8_t i = 0; i < uidLen; i++)
            sprintf(uidStr + i * 2, "%02X", uid[i]);
        uidStr[uidLen * 2] = '\0';

        // Only report if this is a new/different tag
        if (_tagPresent && strcmp(uidStr, _lastUid) == 0) return false;

        _tagPresent = true;
        strlcpy(_lastUid, uidStr, sizeof(_lastUid));
        strlcpy(uidOut, uidStr, 20);
        return true;
    }

private:
    PN532_I2C _i2c;
    PN532     _nfc;
    bool      _tagPresent = false;
    char      _lastUid[20] = {};
    uint16_t  _pollTimeout = 30;   // ms per poll attempt
};

// ── WS1850S / MFRC522 implementation ─────────────────────────
#else

#include <MFRC522_I2C.h>

#define NFC_I2C_ADDR 0x28   // WS1850S fixed I2C address
#define NFC_RST_PIN  9

class NfcReader {
public:
    NfcReader() : _mfrc522(NFC_I2C_ADDR, NFC_RST_PIN) {}

    void begin() {
        _mfrc522.PCD_Init();
        // Set antenna gain to maximum for best range
        _mfrc522.PCD_SetAntennaGain(MFRC522::RxGain_max);
        byte ver = _mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
        if (ver == 0x00 || ver == 0xFF) {
            Serial.println("[NFC] ERROR: WS1850S not found — check wiring");
        } else {
            byte gain = (_mfrc522.PCD_ReadRegister(MFRC522::RFCfgReg) >> 4) & 0x07;
            Serial.printf("[NFC] WS1850S ready (reg 0x%02X  gain=0x%02X max=0x07)\n", ver, gain);
        }
    }

    // Set antenna gain (MFRC522 only). level 0-6, default/max = 6
    // Maps to: 0=18dB 1=23dB 2=? 3=33dB 4=38dB 5=43dB 6=48dB(max)
    void setSensitivity(uint8_t level) {
        const MFRC522::PCD_RxGain gains[] = {
            MFRC522::RxGain_18dB,
            MFRC522::RxGain_23dB,
            MFRC522::RxGain_23dB,
            MFRC522::RxGain_33dB,
            MFRC522::RxGain_38dB,
            MFRC522::RxGain_43dB,
            MFRC522::RxGain_max   // 48dB
        };
        uint8_t idx = min(level, (uint8_t)6);
        _mfrc522.PCD_SetAntennaGain(gains[idx]);
        Serial.printf("[NFC] Antenna gain set to level %u\n", idx);
    }

    bool poll(char* uidOut) {
        if (!_mfrc522.PICC_IsNewCardPresent() || !_mfrc522.PICC_ReadCardSerial()) {
            // No card present — clear memory so next appearance fires
            if (_tagPresent) {
                _tagPresent = false;
                _lastUid[0] = '\0';
            }
            return false;
        }

        char uid[20] = {};
        for (byte i = 0; i < _mfrc522.uid.size; i++)
            sprintf(uid + i * 2, "%02X", _mfrc522.uid.uidByte[i]);
        uid[_mfrc522.uid.size * 2] = '\0';

        _mfrc522.PICC_HaltA();
        _mfrc522.PCD_StopCrypto1();

        // Only report if this is a new/different tag
        if (_tagPresent && strcmp(uid, _lastUid) == 0) return false;

        _tagPresent = true;
        strlcpy(_lastUid, uid, sizeof(_lastUid));
        strlcpy(uidOut, uid, 20);
        return true;
    }

private:
    MFRC522 _mfrc522;
    bool    _tagPresent = false;
    char    _lastUid[20] = {};
};

#endif // USE_PN532