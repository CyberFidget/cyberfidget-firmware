// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/WebPortalApp/LiveLinkProtocol.h
//
// The /ws/live message contract between the device and the browser
// companion, in code form. One WebSocket endpoint carries:
//   - binary frames, device -> browser: raw PCM (16kHz s16le mono) in
//     fixed kPcmFrameBytes frames
//   - text frames, JSON, typed by a "t" field:
//       device -> browser:  {"t":"hello","rate":16000,"bits":16,"ch":1,"fw":"..."}
//                           {"t":"err","reason":"..."}  {"t":"stop","reason":"..."}
//       browser -> device:  {"t":"time","epoch":<ms>}
//                           {"t":"caption","text":"...","final":true|false}
//
// Pure C++ (no Arduino/ESP types) so the parser and builders compile
// host-side for the native test env (test_miccapture_protocol). The hand
// parser only walks the two fixed inbound shapes above — it is not a
// general JSON parser and ignores unknown fields by design.

#ifndef LIVE_LINK_PROTOCOL_H
#define LIVE_LINK_PROTOCOL_H

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace LiveLink {

// --- Pinned framing constants (change these only together with the
// companion client and the planning-side link contract) ---
static const uint32_t kPcmSampleRate = 16000;
static const uint8_t  kPcmBits       = 16;
static const uint8_t  kPcmChannels   = 1;
static const uint32_t kPcmFrameBytes = 5120;   // 160ms at 16kHz s16le mono

// Caption text beyond this is truncated by the device (the OLED shows a
// few lines; the companion sends a bounded tail).
static const size_t kMaxCaptionText = 256;

enum class MsgType : uint8_t { Unknown, Time, Caption };

struct Msg {
    MsgType  type = MsgType::Unknown;
    uint64_t epochMs = 0;                  // Time
    bool     isFinal = false;              // Caption
    char     text[kMaxCaptionText + 1];    // Caption (unescaped)

    Msg() { text[0] = '\0'; }
};

namespace detail {

// Find `"key"` at object level and return a pointer just past the ':'
// (skipping spaces), or nullptr. Good enough for the two flat inbound
// shapes; a key appearing inside a string value could false-match, which
// for this contract only ever mis-parses hostile input into a no-op.
inline const char* findValue(const char* json, const char* key) {
    char pat[24];
    int n = std::snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) return nullptr;
    const char* p = json;
    while ((p = std::strstr(p, pat)) != nullptr) {
        const char* q = p + n;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q == ':') {
            ++q;
            while (*q == ' ' || *q == '\t') ++q;
            return q;
        }
        p = q;
    }
    return nullptr;
}

// Unescape a JSON string starting at its opening quote into out (bounded,
// always NUL-terminated, truncating silently). Returns false if `p` is not
// a string. Non-ASCII \uXXXX escapes become '?' — the OLED fonts are
// effectively ASCII and a placeholder beats mojibake.
inline bool parseString(const char* p, char* out, size_t outSize) {
    if (p == nullptr || *p != '"' || outSize == 0) return false;
    ++p;
    size_t o = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n': case 'r': case 't': c = ' '; break;
                case 'b': case 'f': c = ' '; break;
                case 'u': {
                    unsigned v = 0;
                    for (int i = 0; i < 4 && *p != '\0'; ++i) {
                        char h = *p++;
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    }
                    c = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
                    break;
                }
                case '\0': goto done;     // dangling backslash at end
                default:   c = e; break;  // lenient: keep the escaped char
            }
        }
        if (o + 1 < outSize) out[o++] = c;
        // past the cap: keep consuming so we still find the closing quote
    }
done:
    out[o] = '\0';
    return true;
}

inline uint64_t parseU64(const char* p) {
    uint64_t v = 0;
    if (p == nullptr) return 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        ++p;
    }
    return v;
}

}  // namespace detail

// Parse one inbound text frame. Returns false (type Unknown) for anything
// that isn't a well-formed time/caption message — the caller drops it.
inline bool parseMessage(const char* json, Msg& out) {
    out = Msg();
    if (json == nullptr) return false;

    char t[12];
    if (!detail::parseString(detail::findValue(json, "t"), t, sizeof(t))) {
        return false;
    }

    if (std::strcmp(t, "time") == 0) {
        const char* v = detail::findValue(json, "epoch");
        if (v == nullptr) return false;
        out.type = MsgType::Time;
        out.epochMs = detail::parseU64(v);
        return true;
    }

    if (std::strcmp(t, "caption") == 0) {
        if (!detail::parseString(detail::findValue(json, "text"),
                                 out.text, sizeof(out.text))) {
            return false;
        }
        const char* f = detail::findValue(json, "final");
        out.isFinal = (f != nullptr && std::strncmp(f, "true", 4) == 0);
        out.type = MsgType::Caption;
        return true;
    }

    return false;
}

// --- Outbound builders (device -> browser). Each returns the formatted
// length, or 0 if the buffer was too small. ---

inline size_t buildHello(char* buf, size_t n, const char* fwVersion) {
    int r = std::snprintf(buf, n,
        "{\"t\":\"hello\",\"rate\":%u,\"bits\":%u,\"ch\":%u,\"fw\":\"%s\"}",
        (unsigned)kPcmSampleRate, (unsigned)kPcmBits, (unsigned)kPcmChannels,
        fwVersion != nullptr ? fwVersion : "");
    return (r > 0 && (size_t)r < n) ? (size_t)r : 0;
}

inline size_t buildError(char* buf, size_t n, const char* reason) {
    int r = std::snprintf(buf, n, "{\"t\":\"err\",\"reason\":\"%s\"}",
                          reason != nullptr ? reason : "");
    return (r > 0 && (size_t)r < n) ? (size_t)r : 0;
}

inline size_t buildStop(char* buf, size_t n, const char* reason) {
    int r = std::snprintf(buf, n, "{\"t\":\"stop\",\"reason\":\"%s\"}",
                          reason != nullptr ? reason : "");
    return (r > 0 && (size_t)r < n) ? (size_t)r : 0;
}

}  // namespace LiveLink

#endif  // LIVE_LINK_PROTOCOL_H
