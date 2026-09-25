// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WifiRequest.h"

#include <string.h>

#include <cJSON.h>

namespace WifiRequest {

void begin(Body& b) {
    memset(b.text, 0, sizeof(b.text));
    b.len = 0;
    b.total = 0;
    b.failed = false;
}

void wipe(Body& b) {
    // volatile so the clear is not optimised away before a free.
    volatile char* p = b.text;
    for (size_t i = 0; i < sizeof(b.text); i++) p[i] = 0;
    b.len = 0;
}

Collect collect(Body& b, const uint8_t* data, size_t len, size_t index, size_t total) {
    if (b.failed) return Collect::Invalid;
    const bool first = index == 0 && b.len == 0 && b.total == 0;
    if (first) b.total = total;
    if (total == 0 || total > kBodyMax || total != b.total || index != b.len ||
        len > total - index || (len == 0 && index + len < total) || (!data && len)) {
        wipe(b);
        b.failed = true;
        return Collect::Invalid;
    }
    memcpy(b.text + b.len, data, len);
    b.len += len;
    b.text[b.len] = '\0';
    return b.len == b.total ? Collect::Done : Collect::More;
}

namespace {

// An escaped NUL ("\u0000") would decode into a string the C side cuts short.
bool hasEscapedNul(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '\\') continue;
        if (i + 5 < len && s[i + 1] == 'u' && s[i + 2] == '0' && s[i + 3] == '0' &&
            s[i + 4] == '0' && s[i + 5] == '0')
            return true;
        i++;   // skip the escaped character
    }
    return false;
}

void clearOut(char* name, char* pass) {
    memset(name, 0, WifiList::kNameMax + 1);
    if (pass) memset(pass, 0, WifiList::kPassMax + 1);
}

} // namespace

bool parse(const char* body, size_t len, Kind kind,
           char name[WifiList::kNameMax + 1], char pass[WifiList::kPassMax + 1]) {
    clearOut(name, pass);
    if (!body || len == 0 || len > kBodyMax || body[len] != '\0') return false;
    if (memchr(body, 0, len) || hasEscapedNul(body, len)) return false;
    if (kind == Kind::Connect && !pass) return false;

    const char* end = nullptr;
    // The whole measured body (plus its terminator) must be the object:
    // anything after it but whitespace fails.
    cJSON* root = cJSON_ParseWithLengthOpts(body, len + 1, &end, 1);
    if (!root) return false;
    bool ok = cJSON_IsObject(root);
    bool haveName = false, havePass = false;
    for (cJSON* item = ok ? root->child : nullptr; item && ok; item = item->next) {
        const char* key = item->string;
        if (!key || !cJSON_IsString(item) || !item->valuestring) { ok = false; break; }
        const size_t n = strlen(item->valuestring);
        if (strcmp(key, "ssid") == 0) {
            if (haveName || n == 0 || n > WifiList::kNameMax) { ok = false; break; }
            memcpy(name, item->valuestring, n + 1);
            haveName = true;
        } else if (kind == Kind::Connect && strcmp(key, "pass") == 0) {
            if (havePass || n > WifiList::kPassMax) { ok = false; break; }
            memcpy(pass, item->valuestring, n + 1);
            havePass = true;
        } else {
            ok = false;   // a field this route does not take
        }
    }
    ok = ok && haveName && (kind != Kind::Connect || havePass);
    // Wipe every parsed string (one may be a password) before freeing.
    for (cJSON* item = cJSON_IsObject(root) ? root->child : nullptr; item; item = item->next)
        if (cJSON_IsString(item) && item->valuestring)
            memset(item->valuestring, 0, strlen(item->valuestring));
    cJSON_Delete(root);
    if (!ok) clearOut(name, pass);
    return ok;
}

} // namespace WifiRequest
