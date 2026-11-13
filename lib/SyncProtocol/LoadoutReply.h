// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef LOADOUT_REPLY_H
#define LOADOUT_REPLY_H

#include "SyncProtocol.h"
#include "../LoadoutManifest/LoadoutManifest.h"
#include <cstdio>

namespace SyncProtocol {

// A corrupt reply is an error with raw-byte metadata and no payload. Existing
// readers reject [err] before treating empty or unsupported JSON as a menu.
// A valid reply is followed by exactly json.size() unchanged payload bytes.
inline size_t formatLoadoutHeader(char* out, size_t cap, bool present,
                                 const std::string& json, bool& corrupt) {
    if (!out || !cap) return 0;
    LoadoutManifest::Loadout loadout;
    corrupt = present && (json.find('\0') != std::string::npos ||
                          !LoadoutManifest::parseManifest(json.c_str(), loadout));
    int n;
    if (!present) {
        n = std::snprintf(out, cap, "%s\n", kLgetAbsentReply);
    } else {
        n = std::snprintf(out, cap,
            "%s lget.present=1 entries=%u schema=%d len=%u crc=%08x%s\n",
            corrupt ? "[err]" : "[cmd]",
            corrupt ? 0u : static_cast<unsigned>(loadout.entries.size()),
            corrupt ? 0 : loadout.schemaVersion,
            static_cast<unsigned>(json.size()),
            static_cast<unsigned>(crc32(json.data(), json.size())),
            corrupt ? " corrupt=1" : "");
    }
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

} // namespace SyncProtocol

#endif // LOADOUT_REPLY_H
