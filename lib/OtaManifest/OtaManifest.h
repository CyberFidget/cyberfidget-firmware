// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * OtaManifest - pure hardware compatibility check for update manifests.
 */

#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

#include <stdint.h>

#include "BoardInfo.h"

namespace OtaManifest {

struct Rev {
    bool ok;
    uint32_t major;
    uint32_t minor;
};

enum class HwResult {
    Compatible,
    Incompatible,
    Malformed,
};

constexpr const char* kHardwareRefusal = "This update isn't made for this Fidget.";

/// Parse exactly two nonempty decimal components separated by one dot.
Rev parseRev(const char* text);

/// Check inclusive manifest bounds; Malformed must be treated as a refusal.
HwResult hwCompatible(const BoardInfo::Info& board,
                      const char* minRev, const char* maxRev);

}  // namespace OtaManifest

#endif  // OTA_MANIFEST_H
