// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HomeRemoteConfig.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "HomeRemoteApp.h"
#include "HomeRemoteLogic.h"

namespace HomeRemote {
namespace Config {
namespace {

constexpr const char* kNamespace = "homeremote";
constexpr const char* kUrlKey = "url";
constexpr const char* kTokenKey = "tok";

bool ieqWord(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char ca = *a;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (ca != *b) return false;
    }
    return *a == '\0' && *b == '\0';
}

// "verb rest": true when args starts with verb followed by a space (or the
// end), rest pointing past the spaces.
bool takeVerb(const char* args, const char* verb, const char** rest) {
    const size_t n = strlen(verb);
    for (size_t i = 0; i < n; i++) {
        char c = args[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != verb[i]) return false;
    }
    if (args[n] != ' ' && args[n] != '\0') return false;
    const char* p = args + n;
    while (*p == ' ') p++;
    *rest = p;
    return true;
}

size_t trimmedLen(const char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r')) n--;
    return n;
}

// `hr token` starts the key; `hr token+` adds to the end of it (a whole
// Home Assistant key is longer than one serial line). Every reply says
// only the length.
void saveToken(const char* part, bool append) {
    const size_t n = trimmedLen(part);
    if (n == 0 || !validTokenChars(part, n)) {
        Serial.println("[err] hr.token=invalid");
        return;
    }
    char buf[kTokenMax + 1];
    size_t have = 0;
    buf[0] = '\0';
    if (append && token(buf, sizeof(buf))) have = strlen(buf);
    if (have + n > kTokenMax) {
        memset(buf, 0, sizeof(buf));
        Serial.println("[err] hr.token=too-long");
        return;
    }
    memcpy(buf + have, part, n);
    buf[have + n] = '\0';
    Preferences prefs;
    bool ok = prefs.begin(kNamespace, false);
    if (ok) {
        ok = prefs.putString(kTokenKey, buf) == have + n;
        prefs.end();
    }
    memset(buf, 0, sizeof(buf));
    if (ok) Serial.printf("[cmd] hr.token=saved chars=%u\n", (unsigned)(have + n));
    else Serial.println("[err] hr.token=storage");
}

void saveUrl(const char* in) {
    char tmp[kUrlMax + 2];
    const size_t n = trimmedLen(in);
    if (n == 0 || n >= sizeof(tmp)) {
        Serial.println("[err] hr.url=invalid");
        return;
    }
    memcpy(tmp, in, n);
    tmp[n] = '\0';
    char norm[kUrlMax + 1];
    if (!normalizeBaseUrl(tmp, norm, sizeof(norm))) {
        Serial.println("[err] hr.url=invalid");
        return;
    }
    Preferences prefs;
    bool ok = prefs.begin(kNamespace, false);
    if (ok) {
        ok = prefs.putString(kUrlKey, norm) == strlen(norm);
        prefs.end();
    }
    if (ok) Serial.printf("[cmd] hr.url=%s\n", norm);
    else Serial.println("[err] hr.url=storage");
}

} // namespace

bool url(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const bool have = prefs.isKey(kUrlKey) && prefs.getString(kUrlKey, out, cap) > 0;
    prefs.end();
    if (!have) { out[0] = '\0'; return false; }
    // Re-checked on every read: only a plain http address is ever used.
    char norm[kUrlMax + 1];
    if (!normalizeBaseUrl(out, norm, sizeof(norm)) || strlen(norm) + 1 > cap) {
        out[0] = '\0';
        return false;
    }
    strcpy(out, norm);
    return true;
}

bool hasToken() {
    char buf[kTokenMax + 1];
    const bool have = token(buf, sizeof(buf));
    memset(buf, 0, sizeof(buf));
    return have;
}

bool token(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const bool have = prefs.isKey(kTokenKey) && prefs.getString(kTokenKey, out, cap) > 0;
    prefs.end();
    if (!have || !validTokenChars(out, strlen(out))) {
        memset(out, 0, cap);
        return false;
    }
    return true;
}

void cliCommand(const char* args) {
    const char* rest = nullptr;
    if (!args) args = "";
    if (takeVerb(args, "url", &rest) && *rest) { saveUrl(rest); return; }
    if (takeVerb(args, "token+", &rest) && *rest) { saveToken(rest, true); return; }
    if (takeVerb(args, "token", &rest) && *rest) { saveToken(rest, false); return; }
    if (ieqWord(args, "show")) {
        char u[kUrlMax + 1];
        const bool haveUrl = url(u, sizeof(u));
        Serial.printf("[cmd] hr.url=%s token=%s\n", haveUrl ? u : "unset",
                      hasToken() ? "set" : "unset");
        return;
    }
    if (ieqWord(args, "clear")) {
        Preferences prefs;
        bool ok = prefs.begin(kNamespace, false);
        if (ok) {
            if (prefs.isKey(kUrlKey)) ok = prefs.remove(kUrlKey) && ok;
            if (prefs.isKey(kTokenKey)) ok = prefs.remove(kTokenKey) && ok;
            prefs.end();
        }
        Serial.printf("[cmd] hr.clear=%s\n", ok ? "ok" : "error");
        return;
    }
#ifdef CF_TEST_CLI
    if (ieqWord(args, "state")) { HomeRemoteApp::printState(); return; }
#endif
    // Never echoes what was sent: it may hold the key.
    Serial.println("[err] hr.usage=hr url <url>|token <key>|token+ <more>|show|clear");
}

} // namespace Config
} // namespace HomeRemote

#endif // HOST_TEST
