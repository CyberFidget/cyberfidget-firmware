// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef PORTAL_SSID_H
#define PORTAL_SSID_H

// The portal's network name: "CyberFidget-" + the last four characters of
// the unit's canonical 12-character id (SyncProtocol::formatDeviceId), the
// same four the website shows as the unit's fingerprint. Several Fidgets in
// one room (a classroom) then almost never share a network name: four hex
// characters give 65,536 names, so two of 30 units match about 0.7% of the
// time. Pure, so it is unit-tested natively.

#include <stddef.h>
#include <string.h>

namespace PortalSsid {

constexpr const char* kPrefix = "CyberFidget-";
constexpr size_t kSuffixLen = 4;
constexpr size_t kMaxLen = 12 + kSuffixLen;  // "CyberFidget-" + 4; WiFi allows 32

// `deviceId` is the canonical lowercase id; any upper-case letters are
// lowered anyway. A missing or short id yields just the prefix plus what
// there is.
inline void build(const char* deviceId, char out[kMaxLen + 1]) {
    const size_t prefixLen = strlen(kPrefix);
    memcpy(out, kPrefix, prefixLen);
    size_t n = prefixLen;
    const size_t idLen = deviceId ? strlen(deviceId) : 0;
    const char* tail = deviceId ? deviceId + (idLen > kSuffixLen ? idLen - kSuffixLen : 0) : "";
    for (; *tail && n < kMaxLen; ++tail) {
        char c = *tail;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

}  // namespace PortalSsid

#endif
