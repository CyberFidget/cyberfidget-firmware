// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WIFI_REQUEST_H
#define WIFI_REQUEST_H

// The portal's saved-network requests (POST /api/wifi/connect, /first,
// /forget): collecting a body that arrives in pieces, and reading it
// strictly. Pure (host-tested in test/test_sync_wifilist); the portal
// (lib/WebPortalApp) keeps one Body per request.

#include <stddef.h>
#include <stdint.h>

#include "WifiList.h"

namespace WifiRequest {

constexpr size_t kBodyMax = 255;   // {"ssid": 32 escaped, "pass": 64 escaped} fits

// One request's body. Holds a password: wipe() before it is freed.
struct Body {
    char text[kBodyMax + 1];
    size_t len;
    size_t total;
    bool failed;
};

void begin(Body& b);
void wipe(Body& b);

enum class Collect : uint8_t { More, Done, Invalid };
/// One piece of the body at `index` of `total`. Invalid (and failed for
/// good) on a body too big, a total that changes, a piece out of order or
/// running past the total.
Collect collect(Body& b, const uint8_t* data, size_t len, size_t index, size_t total);

/// What a route accepts. `connect`: {"ssid": text, "pass": text} - both
/// present, as the portal page sends them ("pass" is "" for an open
/// network). `first` / `forget`: {"ssid": text} only.
enum class Kind : uint8_t { Connect, NameOnly };

/// Reads a complete body strictly: the whole measured body is one JSON
/// object and nothing after it, no NUL inside it (raw or as \u0000), only
/// the route's fields, each a string once, the name 1-32 bytes, the
/// password at most 64. `pass` may be null for NameOnly. Outputs are
/// always NUL-terminated (empty on failure).
bool parse(const char* body, size_t len, Kind kind,
           char name[WifiList::kNameMax + 1], char pass[WifiList::kPassMax + 1]);

} // namespace WifiRequest

#endif // WIFI_REQUEST_H
