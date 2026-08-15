// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * ShellStamp - pure (Arduino-free) companion-shell identity helpers.
 *
 * The card and built-in companion copies carry a bounded stamp. This header
 * extracts that stamp from the beginning of a shell document and compares
 * only its numeric version triple, keeping card selection deterministic and
 * independently testable from storage and web-server code.
 */

#ifndef SHELL_STAMP_H
#define SHELL_STAMP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ShellStamp {

/// Longest stamp we will store or compare. 31 chars + NUL.
static constexpr size_t kMaxShellStampLen = 32;

namespace detail {

inline bool stampCharAllowed(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '.' || c == '+' || c == '-';
}

inline bool parseComponent(const char*& p, uint16_t& out) {
    if (p == nullptr || *p < '0' || *p > '9') return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;
    uint32_t value = 0;
    do {
        value = value * 10u + static_cast<uint32_t>(*p - '0');
        if (value > UINT16_MAX) return false;
        ++p;
    } while (*p >= '0' && *p <= '9');
    out = static_cast<uint16_t>(value);
    return true;
}

} // namespace detail

/// Parse the ordering part of a stamp ("1.3.3" out of "1.3.3+a1b2c3d4").
/// Returns false if the text is absent, malformed, or has no three numeric parts.
inline bool parseShellStampVersion(const char* stamp, uint16_t out[3]) {
    if (stamp == nullptr || *stamp == '\0' || out == nullptr) return false;

    const char* p = stamp;
    for (size_t i = 0; i < 3; ++i) {
        if (!detail::parseComponent(p, out[i])) return false;
        if (i < 2) {
            if (*p != '.') return false;
            ++p;
        }
    }

    if (*p == '\0') return true;
    if (*p != '+' || p[1] == '\0') return false;
    for (++p; *p != '\0'; ++p) {
        if (!detail::stampCharAllowed(*p)) return false;
    }
    return true;
}

/// Extract the stamp from the head of a shell document. `buf` is not required
/// to be NUL-terminated. Output is restricted to [0-9A-Za-z.+-], so callers
/// may safely place the result in a manually assembled JSON string.
inline bool findShellStamp(const char* buf, size_t len, char* out, size_t outSize) {
    static const char prefix[] = "<meta name=\"cf-shell\" content=\"";
    static constexpr size_t prefixLen = sizeof(prefix) - 1;

    if (out != nullptr && outSize > 0) out[0] = '\0';
    if (buf == nullptr || out == nullptr || outSize == 0 || len < prefixLen + 2) {
        return false;
    }

    for (size_t i = 0; i + prefixLen <= len; ++i) {
        if (memcmp(buf + i, prefix, prefixLen) != 0) continue;

        const size_t valueAt = i + prefixLen;
        size_t end = valueAt;
        while (end < len && buf[end] != '"') {
            if (!detail::stampCharAllowed(buf[end])) return false;
            ++end;
        }
        const size_t valueLen = end - valueAt;
        if (valueLen == 0 || valueLen + 1 > outSize ||
                valueLen + 1 > kMaxShellStampLen) {
            return false;
        }
        if (end + 1 >= len || buf[end] != '"' || buf[end + 1] != '>') {
            return false;
        }

        memcpy(out, buf + valueAt, valueLen);
        out[valueLen] = '\0';
        return true;
    }
    return false;
}

/// True when the card copy must not be served because it predates the built-in
/// copy. An unreadable card stamp is old; an unreadable built-in stamp fails open.
inline bool cardShellIsOlder(const char* cardStamp, const char* flashStamp) {
    uint16_t cardVersion[3];
    uint16_t flashVersion[3];
    if (!parseShellStampVersion(cardStamp, cardVersion)) return true;
    if (!parseShellStampVersion(flashStamp, flashVersion)) return false;
    if (strcmp(cardStamp, flashStamp) == 0) return false;

    for (size_t i = 0; i < 3; ++i) {
        if (cardVersion[i] < flashVersion[i]) return true;
        if (cardVersion[i] > flashVersion[i]) return false;
    }
    return false;
}

} // namespace ShellStamp

#endif // SHELL_STAMP_H
