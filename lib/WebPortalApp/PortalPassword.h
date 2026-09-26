// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef PORTAL_PASSWORD_H
#define PORTAL_PASSWORD_H

#include <stdint.h>

namespace PortalPassword {

// Supply a 32-bit random source (esp_random on the device). Rejection keeps
// all eight-digit values equally likely, including those with leading zeros.
template <typename Random32>
void generate(char out[9], Random32 random32) {
    constexpr uint32_t kCount = 100000000;
    const uint32_t threshold = uint32_t(-kCount) % kCount;
    uint32_t value;
    do {
        value = random32();
    } while (value < threshold);
    value %= kCount;
    for (int i = 7; i >= 0; --i) {
        out[i] = char('0' + value % 10);
        value /= 10;
    }
    out[8] = '\0';
}

// OLED display form. The network password itself has no space.
inline void grouped(const char password[9], char out[10]) {
    for (int i = 0; i < 4; ++i) out[i] = password[i];
    out[4] = ' ';
    for (int i = 4; i < 8; ++i) out[i + 1] = password[i];
    out[9] = '\0';
}

}  // namespace PortalPassword

#endif
