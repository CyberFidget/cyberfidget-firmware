// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SerialCli.h"

#include <Arduino.h>
#include <esp_system.h>

#include "globals.h"  // getFirmwareVersionString() and friends
#include "version.h"  // FW_GIT_DIRTY for the `info` command

namespace {
// Case-insensitive C-string compare (avoids depending on platform strcasecmp,
// which has different headers across toolchains).
bool ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}
}  // namespace

SerialCli& SerialCli::instance() {
    static SerialCli singleton;
    return singleton;
}

void SerialCli::poll() {
    while (Serial.available() > 0) {
        int byte = Serial.read();
        if (byte < 0) break;
        char c = static_cast<char>(byte);
        if (c == '\n' || c == '\r') {
            if (overflow) {
                Serial.println("[err] line too long");
                overflow = false;
                bufferLen = 0;
                continue;
            }
            if (bufferLen == 0) continue;  // ignore empty lines / lone \r before \n
            buffer[bufferLen] = '\0';
            dispatch(buffer);
            bufferLen = 0;
            continue;
        }
        if (bufferLen + 1 >= kBufferSize) {
            // No room for char + null terminator. Mark overflow; drain until newline.
            overflow = true;
            continue;
        }
        buffer[bufferLen++] = c;
    }
}

void SerialCli::dispatch(const char* line) {
    if (ieq(line, "version")) { cmdVersion(); return; }
    if (ieq(line, "info"))    { cmdInfo();    return; }
    if (ieq(line, "help"))    { cmdHelp();    return; }
    Serial.printf("[err] unknown command: %s\n", line);
}

void SerialCli::cmdVersion() {
    Serial.printf("[cmd] version=%s\n", getFirmwareVersionString());
}

void SerialCli::cmdInfo() {
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[cmd] info.fw=%s\n",      getFirmwareVersionString());
    Serial.printf("[cmd] info.type=%s\n",    getFirmwareBuildType());
    Serial.printf("[cmd] info.built=%s\n",   getFirmwareBuildTimestamp());
    Serial.printf("[cmd] info.git=%s\n",     getFirmwareGitHash());
    Serial.printf("[cmd] info.dirty=%d\n",   FW_GIT_DIRTY);
    Serial.printf("[cmd] info.chip=%s rev %d\n",
                  ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[cmd] info.mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  static_cast<uint8_t>((mac >> 40) & 0xFF),
                  static_cast<uint8_t>((mac >> 32) & 0xFF),
                  static_cast<uint8_t>((mac >> 24) & 0xFF),
                  static_cast<uint8_t>((mac >> 16) & 0xFF),
                  static_cast<uint8_t>((mac >>  8) & 0xFF),
                  static_cast<uint8_t>((mac >>  0) & 0xFF));
    Serial.printf("[cmd] info.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
}

void SerialCli::cmdHelp() {
    Serial.println("[cmd] help=version,info,help");
}
