#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "Config.h"
#include "StatusLed.h"

// ── ESP-NOW callback API selection ─────────────────────────────
// Receive callback: first parameter changed from a plain MAC pointer to
// esp_now_recv_info_t* in ESP-IDF 5.0 (Espressif's 5.0 migration guide),
// i.e. Arduino-ESP32 core 3.x. Default is picked from ESP_IDF_VERSION.
// Force it with -DESPNOW_NEW_RECV_CB=0/1 in build_flags if ever needed.
//
// Send callback: its first parameter changed too (ESP-IDF 5.5), but the
// type's NAME has moved between releases — wifi_tx_info_t in the 5.5
// build seen here, esp_now_send_info_t in current IDF docs. Since we
// ignore that argument anyway, onSent() is a template and the compiler
// takes whatever type the installed header declares. Nothing to set.
#if __has_include(<esp_idf_version.h>)
  #include <esp_idf_version.h>
#endif
#ifndef ESPNOW_NEW_RECV_CB
  #define ESPNOW_NEW_RECV_CB 0            // default: old plain-MAC callback
  // Nested on purpose: if the version header is missing, ESP_IDF_VERSION_VAL
  // doesn't exist and even a short-circuited #if would fail to parse.
  #if defined(ESP_IDF_VERSION) && defined(ESP_IDF_VERSION_VAL)
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      #undef  ESPNOW_NEW_RECV_CB
      #define ESPNOW_NEW_RECV_CB 1
    #endif
  #endif
#endif

// ── Shared message types (must match hand unit) ───────────────
#define MSG_PROXIMITY  0x01
#define MSG_NFC        0x02
#define MSG_TOUCH      0x03
#define MSG_PING       0x10
#define MSG_PONG       0x11
#define MSG_HEARTBEAT  0x12
#define MSG_PAIR_REQUEST 0x20   // broadcast while in pairing mode
#define MSG_PAIR_ACK     0x21   // unicast from hand unit confirming pairing

struct __attribute__((packed)) HandPacket {
    uint8_t  type;
    uint16_t distMm;
    uint8_t  touchState;
    char     nfcUid[20];
};

struct __attribute__((packed)) PingPacket {
    uint8_t type;
    char    deviceId[16];
};

// ── State ─────────────────────────────────────────────────────
bool    espNowReady  = false;
bool    handPaired   = false;
bool     pairingMode      = false;   // set/cleared by PairButton.h, read by StatusLed
uint32_t pairingStartedMs = 0;
uint32_t lastPairReqMs    = 0;
uint8_t handMac[6]   = {};
uint32_t lastRxMs    = 0;    // last packet from the PAIRED hand unit (drives the LED)
uint32_t foreignRxCount = 0; // packets heard from anyone else (diagnostic only)
uint32_t rxProx = 0, rxNfc = 0, rxTouch = 0, rxPing = 0, rxOther = 0;  // per-type, paired hand only
uint32_t lastForeignMs  = 0;
uint8_t  lastForeignMac[6] = {};
uint16_t lastDist    = 9999;
uint8_t  lastTouch   = 0;
char     lastUid[20] = {};

static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void macToStr(const uint8_t* mac, char* buf) {
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static void espNowAddPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    // channel=0 is meant to mean "use the current channel", but that
    // resolution isn't reliable on every ESP-IDF version — pin the peer to
    // our ACTUAL current channel explicitly instead. Symptom otherwise:
    // broadcasts/receiving work fine, but unicast sends silently fail
    // (onSent reports ESP_NOW_SEND_FAIL) with no error at add_peer() time.
    peer.channel = WiFi.channel();
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// ── Send PONG back to hand unit ───────────────────────────────
static void sendPong(const uint8_t* toMac) {
    PingPacket pkt = {};
    pkt.type = MSG_PONG;
    strlcpy(pkt.deviceId, "BODY_BRIDGE", sizeof(pkt.deviceId));
    esp_now_send(toMac, (uint8_t*)&pkt, sizeof(pkt));
}

// ── Receive callback ──────────────────────────────────────────
#if ESPNOW_NEW_RECV_CB
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* srcMac = info->src_addr;
    if (len < 1) return;
#else
static void onRecv(const uint8_t* srcMac, const uint8_t* data, int len) {
    if (len < 1) return;
#endif
    uint8_t type = data[0];

    // ── Discovery: PING from hand unit ────────────────────────
    if (type == MSG_PING) {
        rxPing++;
        char buf[18]; macToStr(srcMac, buf);
        Serial.printf("[ESPNOW] PING from %s — sending PONG\n", buf);
        espNowAddPeer(srcMac);
        sendPong(srcMac);
        // Store as paired hand unit
        memcpy(handMac, srcMac, 6);
        handPaired = true;
        Preferences prefs;
        prefs.begin("bodybridge", false);
        prefs.putBytes("handMac", handMac, 6);
        prefs.end();
        Serial.printf("[ESPNOW] Paired with hand unit: %s\n", buf);
        statusOnPaired();
        return;
    }

    // ── Pairing ack — button-triggered pairing, independent of PING/PONG ──
    if (type == MSG_PAIR_ACK) {
        char buf[18]; macToStr(srcMac, buf);
        if (!pairingMode) {
            Serial.printf("[PAIR] ACK from %s but we are not in pairing mode — ignored\n", buf);
            return;
        }
        espNowAddPeer(srcMac);
        memcpy(handMac, srcMac, 6);
        handPaired = true;
        Preferences prefs;
        prefs.begin("bodybridge", false);
        prefs.putBytes("handMac", handMac, 6);
        prefs.end();
        Serial.printf("[PAIR] ACK from %s — paired, leaving pairing mode\n", buf);
        pairingMode = false;
        statusOnPaired();
        return;
    }

    // ── Only process packets from the paired hand unit ────────
    // PING and PAIR_ACK (handled above) are the only things a NOT-yet-paired
    // bridge should ever act on — everything else here is rejected until
    // genuinely paired, not just when it's paired to someone else. Without
    // the !handPaired check, an unpaired bridge would process (and forward
    // to the Teensy) tag/touch data from any hand unit in range.
    if (!handPaired || memcmp(srcMac, handMac, 6) != 0) {
        foreignRxCount++;
        lastForeignMs = millis();
        memcpy(lastForeignMac, srcMac, 6);
        return;
    }
    lastRxMs = millis();      // only the paired hand unit keeps the link "alive"

    if (len < (int)sizeof(HandPacket)) { rxOther++; return; }
    const HandPacket* pkt = (const HandPacket*)data;

    lastDist  = pkt->distMm;
    lastTouch = pkt->touchState;
    strlcpy(lastUid, pkt->nfcUid, sizeof(lastUid));

    switch (type) {

        case MSG_PROXIMITY:
            rxProx++;
            statusOnLinked();   // active link
            TEENSY_UART.printf("{\"t\":\"p\",\"d\":%u,\"s\":%u}\n",
                               pkt->distMm, pkt->touchState);
            break;

        case MSG_NFC:
            rxNfc++;
            statusOnNfc();
            Serial.printf("[NFC]   uid=%-16s  dist=%4u mm  touch=%u\n",
                          pkt->nfcUid, pkt->distMm, pkt->touchState);
            TEENSY_UART.printf("{\"t\":\"n\",\"u\":\"%s\",\"d\":%u}\n",
                               pkt->nfcUid, pkt->distMm);
            break;

        case MSG_TOUCH:
            rxTouch++;
            statusOnTouch();
            Serial.printf("[TOUCH] uid=%-16s  dist=%4u mm  CONNECTION\n",
                          pkt->nfcUid, pkt->distMm);
            TEENSY_UART.printf("{\"t\":\"c\",\"u\":\"%s\"}\n", pkt->nfcUid);
            break;

        default:
            rxOther++;
            Serial.printf("[ESPNOW] Unknown type 0x%02X len=%d\n", type, len);
    }
}

// Send callback: first argument ignored -> template accepts any header's type.
template <typename TxInfo>
static void onSent(const TxInfo* /*txInfo*/, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS)
        Serial.println("[ESPNOW] send callback: FAIL");
}

// ── Teensy UART ───────────────────────────────────────────────
void teensySetup() {
    TEENSY_UART.begin(TEENSY_BAUD, SERIAL_8N1, TEENSY_RX, TEENSY_TX);
    Serial.println("[UART] Teensy port ready");
}

// ── Setup ─────────────────────────────────────────────────────
void espNowSetup() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed"); return;
    }
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    espNowAddPeer(BROADCAST);

    // Load stored hand unit MAC
    Preferences prefs;
    prefs.begin("bodybridge", true);
    if (prefs.getBytesLength("handMac") == 6) {
        prefs.getBytes("handMac", handMac, 6);
        handPaired = true;
        espNowAddPeer(handMac);
        char buf[18]; macToStr(handMac, buf);
        Serial.printf("[ESPNOW] Loaded hand unit: %s\n", buf);
        statusOnPaired();
    } else {
        Serial.println("[ESPNOW] No hand unit paired — wait for hand unit to scan");
    }
    prefs.end();

    Serial.printf("[ESPNOW] Body bridge MAC: %s\n", WiFi.macAddress().c_str());
    espNowReady = true;
}

// ── Heartbeat — tell hand unit we are alive ───────────────────
void espNowSendHeartbeat() {
    if (!handPaired) return;
    uint8_t pkt[1] = { MSG_HEARTBEAT };
    esp_now_send(handMac, pkt, sizeof(pkt));
}

// ── CLI helpers ───────────────────────────────────────────────
void espNowPrintStatus() {
    char buf[18];
    Serial.printf("ESPNOW:  %s\n", espNowReady ? "ready" : "not init");
    if (handPaired) {
        macToStr(handMac, buf);
        Serial.printf("Hand:    %s\n", buf);
    } else {
        Serial.println("Hand:    not paired");
    }
    if (lastRxMs > 0) {
        Serial.printf("Last rx: %lums ago\n", millis() - lastRxMs);
        Serial.printf("Dist:    %u mm  Touch: %u\n", lastDist, lastTouch);
        if (lastUid[0]) Serial.printf("Last UID: %s\n", lastUid);
    } else {
        Serial.println("Last rx: none");
    }
    Serial.printf("Rx (paired hand): prox=%lu nfc=%lu touch=%lu ping=%lu other=%lu\n",
                  rxProx, rxNfc, rxTouch, rxPing, rxOther);
    Serial.printf("Uptime:  %lus\n", millis() / 1000);
    if (foreignRxCount > 0) {
        macToStr(lastForeignMac, buf);
        Serial.printf("Foreign: %lu packets ignored, last from %s %lums ago\n",
                      foreignRxCount, buf, millis() - lastForeignMs);
    } else {
        Serial.println("Foreign: none");
    }
}

void espNowClearPair() {
    if (handPaired) esp_now_del_peer(handMac);
    handPaired = false;
    memset(handMac, 0, 6);
    Preferences prefs;
    prefs.begin("bodybridge", false);
    prefs.remove("handMac");
    prefs.end();
    Serial.println("[ESPNOW] Pairing cleared");
}