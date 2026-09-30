// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "OtaManifest.h"

#include <stdint.h>

namespace OtaManifest {

namespace {

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool component(const char*& text, uint32_t& value) {
    if (!digit(*text)) return false;
    value = 0;
    while (digit(*text)) {
        const uint32_t next = static_cast<uint32_t>(*text - '0');
        if (value > (UINT32_MAX - next) / 10) return false;
        value = value * 10 + next;
        ++text;
    }
    return true;
}

int compare(uint32_t majorA, uint32_t minorA,
            uint32_t majorB, uint32_t minorB) {
    if (majorA != majorB) return majorA < majorB ? -1 : 1;
    if (minorA != minorB) return minorA < minorB ? -1 : 1;
    return 0;
}

}  // namespace

Rev parseRev(const char* text) {
    Rev rev{};
    if (text == nullptr || !component(text, rev.major) || *text++ != '.' ||
        !component(text, rev.minor) || *text != '\0') {
        return Rev{};
    }
    rev.ok = true;
    return rev;
}

HwResult hwCompatible(const BoardInfo::Info& board,
                      const char* minRev, const char* maxRev) {
    const Rev min = parseRev(minRev);
    const Rev max = parseRev(maxRev);
    if (!min.ok || !max.ok ||
        compare(min.major, min.minor, max.major, max.minor) > 0) {
        return HwResult::Malformed;
    }

    // Unprogrammed, read-error, and unknown-layout boards compare as
    // BoardInfo's reported 1.2 default, matching the server fallback.
    const bool fallback = board.source != BoardInfo::Source::Efuse;
    const uint32_t major = fallback ? BoardInfo::kDefaultMajor : board.major;
    const uint32_t minor = fallback ? BoardInfo::kDefaultMinor : board.minor;
    if (compare(major, minor, min.major, min.minor) < 0 ||
        compare(major, minor, max.major, max.minor) > 0) {
        return HwResult::Incompatible;
    }
    return HwResult::Compatible;
}

}  // namespace OtaManifest
