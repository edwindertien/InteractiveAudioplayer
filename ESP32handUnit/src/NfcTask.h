#pragma once
#include <Arduino.h>
#include "Config.h"

#if NFC_UART
  // Include .cpp directly — same as Seeed example
  // Avoids relying on PlatformIO build flags propagating to library files
  #define NFC_INTERFACE_HSU
  #include <PN532_HSU.h>
  #include <PN532_HSU.cpp>
  #include <PN532.h>

  HardwareSerial NfcSerial(1);
  PN532_HSU      pn532hsu(NfcSerial);   // plain constructor, no rx/tx args
  PN532          nfc(pn532hsu);
#else
  #include <Wire.h>
  #define NFC_INTERFACE_I2C
  #include <PN532_I2C.h>
  #include <PN532_I2C.cpp>
  #include <PN532.h>
  PN532_I2C pn532i2c(Wire1);
  PN532     nfc(pn532i2c);
#endif

bool          nfcReady     = false;
QueueHandle_t nfcQueue;
char          nfcLastUid[20] = {};
bool          nfcTagPresent  = false;

void nfcSendWakeup() {
#if NFC_UART
    uint8_t w[] = {0x55,0x55,0x00,0x00,0x00,0x00,
                   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    NfcSerial.write(w, sizeof(w));
    vTaskDelay(pdMS_TO_TICKS(2));
#endif
}

void nfcPowerDown() {
#if NFC_UART && NFC_SLEEP_MS > 0
    const uint8_t dchk = (uint8_t)(0x100 - ((0xD4 + 0x16 + 0x20) & 0xFF));
    uint8_t pkt[] = {0x00,0x00,0xFF,0x03,0xFD,0xD4,0x16,0x20,dchk,0x00};
    NfcSerial.write(pkt, sizeof(pkt));
    NfcSerial.flush();
#endif
}

void nfcReinit() {
    Serial.println("[NFC] reinitialising...");
#if NFC_UART
    #if NFC_RST_PIN >= 0
    digitalWrite(NFC_RST_PIN, LOW);  vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(NFC_RST_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(50));
    #endif
    NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    nfc.begin();
    vTaskDelay(pdMS_TO_TICKS(20));
#else
    nfc.begin();
#endif
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) { Serial.println("[NFC] reinit failed"); nfcReady = false; return; }
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0xFF);
    nfcReady = true;
    Serial.printf("[NFC] reinit ok fw %u.%u\n", (ver>>16)&0xFF, (ver>>8)&0xFF);
}

void nfcTask(void*) {
    uint32_t lastSuccessMs = millis();
    bool inPowerDown = false;
    for (;;) {
        if (millis() - lastSuccessMs > 8000) {
            nfcReinit();
            lastSuccessMs = millis();
            inPowerDown = false;
        }
#if NFC_SLEEP_MS > 0
        if (inPowerDown) { nfcSendWakeup(); inPowerDown = false; }
#endif
        uint8_t uid[7]={}, len=0;
        bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50);
        lastSuccessMs = millis();
        if (!found || len == 0) {
            if (nfcTagPresent) { nfcTagPresent=false; nfcLastUid[0]='\0'; }
#if NFC_SLEEP_MS > 0
            nfcPowerDown(); inPowerDown=true;
            vTaskDelay(pdMS_TO_TICKS(NFC_SLEEP_MS));
#else
            vTaskDelay(pdMS_TO_TICKS(20));
#endif
            continue;
        }
        char s[20]={};
        for (uint8_t i=0;i<len;i++) sprintf(s+i*2,"%02X",uid[i]);
        s[len*2]='\0';
        if (nfcTagPresent && strcmp(s,nfcLastUid)==0) {
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        nfcTagPresent=true;
        strlcpy(nfcLastUid,s,sizeof(nfcLastUid));
        xQueueSend(nfcQueue,s,0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void nfcSetup() {
    nfcQueue = xQueueCreate(8, 20);

#if NFC_UART
    Serial.printf("[NFC] UART1 TX=GPIO%d RX=GPIO%d RST=GPIO%d\n",
                  NFC_TX_PIN, NFC_RX_PIN, NFC_RST_PIN);

    // ── NFC init: release holds, RST sequence, serial ────────
    // Release gpio_hold latched by powerDownPeripherals() before sleep
    gpio_hold_dis((gpio_num_t)NFC_RST_PIN);
    gpio_hold_dis((gpio_num_t)NFC_TX_PIN);
    Serial.println("[NFC] gpio holds released");

    // Assert RST LOW → PN532 in reset
    pinMode(NFC_RST_PIN, OUTPUT);
    digitalWrite(NFC_RST_PIN, LOW);
    Serial.printf("[NFC] RST LOW (GPIO%d) — PN532 in reset\n", NFC_RST_PIN);
    { uint32_t t = millis(); while (millis() - t < 100) {} }

    // Release RST to INPUT (high-Z) — relies on module pull-up to reach HIGH
    // This avoids any gpio_hold override that would keep the pin LOW
    pinMode(NFC_RST_PIN, INPUT);
    Serial.printf("[NFC] RST INPUT/high-Z (GPIO%d) — check pin is HIGH now\n", NFC_RST_PIN);
    { uint32_t t = millis(); while (millis() - t < 50) {} }
    Serial.println("[NFC] RST wait done — initialising serial...");

    // Init serial with our pins
    NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);
    Serial.println("[NFC] NfcSerial.begin done");

    // nfc.begin() re-inits serial with -1/-1 (keeps our pins on ESP32)
    nfc.begin();
    Serial.println("[NFC] nfc.begin done — calling getFirmwareVersion...");

    // 4. Init serial with our pins
    NfcSerial.begin(NFC_BAUD, SERIAL_8N1, NFC_RX_PIN, NFC_TX_PIN);

    // 5. nfc.begin() — re-inits serial with -1/-1 (keeps our pins on ESP32)
    nfc.begin();

#else
    Wire1.begin(NFC_SDA, NFC_SCL, NFC_FREQ);
    nfc.begin();
#endif

    uint32_t ver = nfc.getFirmwareVersion();
    Serial.printf("[NFC] getFirmwareVersion = 0x%08X\n", ver);
    if (!ver) {
        Serial.println("[NFC] not found"); return;
    }
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0xFF);
    nfcReady = true;
    Serial.printf("[NFC] PN532 fw %u.%u ready\n", (ver>>16)&0xFF, (ver>>8)&0xFF);
    xTaskCreatePinnedToCore(nfcTask,"nfc",4096,NULL,1,NULL,0);
    Serial.println("[NFC] task on Core 0");
}