#pragma once
#include <Arduino.h>
#include "EspNow.h"

char    cliBuf[64] = {};
uint8_t cliPos     = 0;
bool    streamProx = false;   // toggle proximity stream to terminal

void printHelp() {
    Serial.println(
        "\nCommands:\n"
        "  status          — ESPNOW + last packet info\n"
        "  mac             — print this device's MAC\n"
        "  stream          — toggle continuous proximity stream\n"
        "  clear           — clear stored hand unit pairing\n"
    );
}

void handleCommand(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        espNowPrintStatus();

    } else if (strcmp(cmd, "mac") == 0) {
        Serial.printf("Body bridge MAC: %s\n", WiFi.macAddress().c_str());
        Serial.println("Give this to hand unit: espnow bind <MAC>");

    } else if (strcmp(cmd, "stream") == 0) {
        streamProx = !streamProx;
        Serial.printf("Proximity stream: %s\n", streamProx ? "ON" : "OFF");

    } else if (strcmp(cmd, "clear") == 0) {
        espNowClearPair();

    } else if (strcmp(cmd, "help") == 0 || cmd[0] == '?') {
        printHelp();

    } else if (cmd[0] != '\0') {
        Serial.printf("Unknown: '%s'\n", cmd);
    }
}

void cliPoll() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cliPos > 0) {
                cliBuf[cliPos] = '\0';
                cliPos = 0;
                handleCommand(cliBuf);
            }
        } else if (cliPos < sizeof(cliBuf) - 1) {
            cliBuf[cliPos++] = c;
        }
    }
}
