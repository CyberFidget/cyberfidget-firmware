// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HomeRemoteRequest.h"

#include <string.h>

namespace HomeRemote {
namespace Request {

void begin(Body& b) {
    memset(&b, 0, sizeof(b));
}

void wipe(Body& b) {
    secureWipe(b.text, sizeof(b.text));
    b.len = 0;
}

Collect collect(Body& b, const uint8_t* data, size_t len, size_t index, size_t total) {
    if (b.failed) return Collect::Invalid;
    if (index == 0 && b.len == 0 && b.total == 0) b.total = total;
    if (!total || total > kBodyMax || total != b.total || index != b.len ||
        index > total || len > total - index || (!len && index < total) || (!data && len)) {
        wipe(b);
        b.failed = true;
        return Collect::Invalid;
    }
    if (len) memcpy(b.text + b.len, data, len);
    b.len += len;
    b.text[b.len] = '\0';
    return b.len == total ? Collect::Done : Collect::More;
}

namespace {
void spaces(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
}

bool take(const char*& p, const char* end, char c) {
    spaces(p, end);
    if (p == end || *p != c) return false;
    p++;
    return true;
}

bool string(const char*& p, const char* end, char* out, size_t cap) {
    if (!take(p, end, '"')) return false;
    size_t n = 0;
    while (p < end && *p != '"') {
        const unsigned char c = (unsigned char)*p++;
        if (c < 0x20 || c == '\\' || n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (p == end) return false;
    p++;
    return true;
}
} // namespace

bool parse(const char* body, size_t len, char url[kUrlMax + 1], char key[kTokenMax + 1]) {
    if (url) secureWipe(url, kUrlMax + 1);
    if (key) secureWipe(key, kTokenMax + 1);
    if (!url || !key || !body || !len || len > kBodyMax || memchr(body, 0, len)) return false;
    const char* p = body;
    const char* end = body + len;
    bool haveUrl = false, haveKey = false;
    bool ok = take(p, end, '{');
    for (int i = 0; i < 2 && ok; i++) {
        char field[4] = {};
        ok = (i == 0 || take(p, end, ',')) && string(p, end, field, sizeof(field)) &&
             take(p, end, ':');
        if (!ok) break;
        if (strcmp(field, "url") == 0 && !haveUrl) {
            ok = string(p, end, url, kUrlMax + 1) && url[0];
            haveUrl = true;
        } else if (strcmp(field, "key") == 0 && !haveKey) {
            ok = string(p, end, key, kTokenMax + 1);
            haveKey = true;
        } else ok = false;
    }
    ok = ok && haveUrl && haveKey && take(p, end, '}');
    spaces(p, end);
    ok = ok && p == end;
    if (!ok) {
        secureWipe(url, kUrlMax + 1);
        secureWipe(key, kTokenMax + 1);
    }
    return ok;
}

} // namespace Request
} // namespace HomeRemote
