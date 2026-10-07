#pragma once
#include <Arduino.h>
#include "Motor.h"
#include "Proximity.h"
#include "LedRing.h"
#include "NfcTask.h"
#include "TouchManager.h"
#include "EspNow.h"
#include "PowerManager.h"

bool streaming = false;   // VL53L0X distance stream — toggled by 'stream' command

void printHelp() {
    Serial.println(
        "\nCommands:\n"
        "  status              — all sensor readings\n"
        "  stream              — toggle VL53L0X distance stream\n"
        "  tof                 — VL53L0X diagnostics and tuning ('tof help' for the list)\n"
        "  scan                — I2C scan (both buses)\n"
        "  rainbow             — LED rainbow\n"
        "  solid <r> <g> <b>   — solid colour\n"
        "  bright <0-255>      — brightness\n"
        "  off / idle          — LED modes\n"
        "  motor <pulses>      — trigger burst\n"
        "  sleep               — go to deep sleep\n"
        "  battery             — battery level ('battery help' lists the calibration commands)\n"
        "  espnow              — ESP-NOW status\n"
        "  espnow scan         — discover and pair body bridge\n"
        "  espnow bind <MAC>   — manually set peer MAC\n"
        "  espnow clear        — clear stored pairing\n"
        "  espnow test         — send test packet\n"
        "  nfc                 — re-run NFC init + 3s tag poll\n"
        "  debug               — dump all state\n"
    );
}

void handleCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        Serial.printf("NFC  %s  last: %s\n", nfcReady?"ok":"fail",
                      nfcLastUid[0] ? nfcLastUid : "none");
        Serial.printf("TOF  %s  dist: %u mm\n", tofReady?"ok":"fail", lastDist);
        Serial.printf("LED  mode:%d  bright:%u\n",
                      (int)ledMode, FastLED.getBrightness());
        Serial.printf("Touch: %s%s\n",
                      touchState==TouchState::RELEASED?"RELEASED":"IDLE",
                      touchState==TouchState::RELEASED
                          ? (millis()<silenceUntil?" (silence)":" (waiting separation)")
                          : "");

    } else if (strncmp(cmd, "tof", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        tofCommand(cmd + 3);

    } else if (strcmp(cmd, "stream") == 0) {
        streaming = !streaming;
        Serial.printf("VL53 stream: %s\n", streaming ? "ON" : "OFF");

    } else if (strcmp(cmd, "scan") == 0) {
        Serial.println("Wire (TOF bus):");
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0)
                Serial.printf("  0x%02X%s\n", a, a==0x29?" ← VL53L0X":"");
        }

    } else if (strcmp(cmd, "rainbow") == 0) {
        ledMode = LedMode::RAINBOW;

    } else if (strcmp(cmd, "off") == 0) {
        ledMode = LedMode::OFF; ledOff();

    } else if (strcmp(cmd, "idle") == 0) {
        ledMode = LedMode::IDLE;

    } else if (strncmp(cmd, "solid", 5) == 0) {
        int r=0,g=0,b=0;
        if (sscanf(cmd+6, "%d %d %d", &r, &g, &b) == 3) {
            solidColor = CRGB(r,g,b); ledMode = LedMode::SOLID; ledSolid(solidColor);
        } else Serial.println("Usage: solid <r> <g> <b>");

    } else if (strncmp(cmd, "bright", 6) == 0) {
        FastLED.setBrightness(constrain(atoi(cmd+7), 0, 255));

    } else if (strncmp(cmd, "motor", 5) == 0) {
        int n = atoi(cmd+6); if (n<=0) n=3;
        motorBurstStart(n, 200, 55, 35);

    } else if (strcmp(cmd, "sleep") == 0) {
        Serial.println("Sleeping...");
        powerSleep();

    } else if (strncmp(cmd, "battery", 7) == 0 && (cmd[7] == '\0' || cmd[7] == ' ')) {
        batteryCommand(cmd + 7);

    } else if (strncmp(cmd, "espnow", 6) == 0) {
        const char* sub = (strlen(cmd) > 7) ? cmd + 7 : "";
        if      (strcmp(sub, "scan")  == 0) espNowStartScan();
        else if (strcmp(sub, "clear") == 0) espNowClearPair();
        else if (strcmp(sub, "test")  == 0) espNowSendTest();
        else if (strncmp(sub, "bind", 4) == 0) espNowBindManual(sub + 5);
        else espNowPrintStatus();

    } else if (strcmp(cmd, "nfc") == 0) {
        // Re-run NFC detection and print full trace
        Serial.println("[NFC] manual init...");
        Serial.printf("  TX=GPIO%d  RX=GPIO%d  baud=%d\n", NFC_TX_PIN, NFC_RX_PIN, NFC_BAUD);
        NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
        delay(20); while (NfcSerial.available()) NfcSerial.read();

        // Loopback test — wire TX to RX pin physically before running this
        NfcSerial.print("PING"); delay(20);
        char lb[8]={}; int lbi=0;
        uint32_t lt=millis();
        while(millis()-lt<50&&lbi<4) if(NfcSerial.available()) lb[lbi++]=NfcSerial.read();
        if(strncmp(lb,"PING",4)==0)
            Serial.println("  UART loopback: PASS (remove wire, connect PN532)");
        else
            Serial.printf("  UART loopback: FAIL (%d bytes back) — GPIO5/6 UART broken in this context\n", lbi);
        delay(20); while (NfcSerial.available()) NfcSerial.read();
        // Wakeup preamble
        uint8_t w[] = {0x55,0x55,0x00,0x00,0x00,0x00,0x00,0x00,
                       0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        NfcSerial.write(w, sizeof(w));

        // Show any raw bytes received — if PN532 is in UART mode it responds
        Serial.print("  Raw RX after preamble (200ms): ");
        int rxCount = 0;
        uint32_t rt = millis();
        while (millis() - rt < 200) {
            if (NfcSerial.available()) {
                Serial.printf("0x%02X ", NfcSerial.read());
                rxCount++;
            }
        }
        if (rxCount == 0)
            Serial.println("(nothing) ← PN532 not responding at all");
        else
            Serial.printf("\n  %d bytes received\n", rxCount);
        delay(10);
        uint32_t ver = nfc.getFirmwareVersion();
        Serial.printf("  getFirmwareVersion = 0x%08X\n", ver);
        if (!ver) {
            Serial.println("  FAIL — check TX/RX wires and DIP (SEL0=LOW SEL1=LOW)");
            nfcReady = false;
        } else {
            Serial.printf("  OK — PN532 fw %u.%u\n", (ver>>16)&0xFF, (ver>>8)&0xFF);
            nfc.SAMConfig();
            nfc.setPassiveActivationRetries(0xFF);
            nfcReady = true;
            Serial.println("  Tap a tag to test...");
            uint32_t t0 = millis();
            char uid[20] = {};
            bool found = false;
            while (millis() - t0 < 3000) {
                uint8_t buf[7]={}, len=0;
                if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &len, 50) && len) {
                    for (uint8_t i=0;i<len;i++) sprintf(uid+i*2,"%02X",buf[i]);
                    uid[len*2]=0;
                    Serial.printf("  Tag: %s (%u bytes)\n", uid, len);
                    found = true; break;
                }
            }
            if (!found) Serial.println("  No tag in 3s");
        }

    } else if (strcmp(cmd, "debug") == 0) {
        Serial.println("=== DEBUG ===");
        Serial.printf("NFC_UART     %d  TX=%d RX=%d RST=%d SLEEP=%dms\n", NFC_UART, NFC_TX_PIN, NFC_RX_PIN, NFC_RST_PIN, NFC_SLEEP_MS);
        Serial.printf("nfcReady     %d  lastUid=%s tagPresent=%d\n", nfcReady, nfcLastUid[0]?nfcLastUid:"none", nfcTagPresent);
        Serial.printf("TOF          SDA=%d SCL=%d XSHUT=%d ready=%d dist=%u\n", TOF_SDA, TOF_SCL, TOF_XSHUT, tofReady, lastDist);
        Serial.printf("streaming    %d\n", streaming);
        Serial.printf("touchState   %s\n", touchState==TouchState::RELEASED?"RELEASED":"IDLE");
        Serial.printf("wakeReason   %d  (3=EXT1/button)\n", (int)esp_sleep_get_wakeup_cause());
        { char d[96]; batteryDescribe(d, sizeof d); Serial.printf("battery      %s\n", d); }
        Serial.println("=============");

    } else if (strcmp(cmd,"help")==0 || cmd[0]=='?') {
        printHelp();
    } else if (cmd[0] != '\0') {
        Serial.printf("Unknown: '%s'\n", cmd);
    }
}

char    cliBuf[64] = {};
uint8_t cliPos     = 0;

void cliPoll() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c=='\n'||c=='\r') {
            if (cliPos>0) { cliBuf[cliPos]='\0'; cliPos=0; handleCommand(cliBuf); }
        } else if (cliPos < sizeof(cliBuf)-1) {
            cliBuf[cliPos++] = c;
        }
    }
}