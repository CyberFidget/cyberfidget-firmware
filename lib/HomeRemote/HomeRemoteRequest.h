// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_REQUEST_H
#define HOME_REMOTE_REQUEST_H

#include "HomeRemoteLogic.h"

namespace HomeRemote {
namespace Request {

constexpr size_t kBodyMax = 512;

// One portal request; wipe before freeing or on disconnect.
struct Body {
    char text[kBodyMax + 1];
    size_t len;
    size_t total;
    bool failed;
};

void begin(Body& b);
void wipe(Body& b);
enum class Collect : uint8_t { More, Done, Invalid };
Collect collect(Body& b, const uint8_t* data, size_t len, size_t index, size_t total);

// Exactly {"url":string,"key":string}, in either order, without escapes.
// Empty key keeps the saved key. Outputs are empty on failure.
bool parse(const char* body, size_t len, char url[kUrlMax + 1], char key[kTokenMax + 1]);

} // namespace Request
} // namespace HomeRemote

#endif // HOME_REMOTE_REQUEST_H
