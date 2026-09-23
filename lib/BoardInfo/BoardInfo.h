// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * BoardInfo - pure (Arduino-free) parser for the board identity block.
 *
 * Each mainboard can carry its hardware revision and a few provisioning
 * fields in eFuse BLK3 (the 32-byte user block). The HAL reads the raw
 * block once at boot and hands it to parseBoardBlock(); everything here is
 * plain data logic so it compiles for the native unit tests.
 *
 * Layout (byte offsets into the 32-byte block, layout version 1):
 *   7   magic 0xCF (absent = board not provisioned)
 *   8   layout version (0x01)
 *   9   board rev MAJOR
 *   10  board rev MINOR
 *   11  manufacturing year minus 2020
 *   20  flags: bit0 HIL bench unit, bit1 engineering sample
 *   21  manufacturing month (1-12)
 *   22  batch/lot id (0 = unknown)
 *   24  variant/population bitfield
 *
 * A board without the magic byte (every board built before provisioning
 * began, and any read failure) reports the defaults: rev 1.2, no flags.
 * A block with the magic but an unrecognised layout version also reports
 * the defaults, keeping the raw layout number so the mismatch is visible.
 */

#ifndef BOARD_INFO_H
#define BOARD_INFO_H

#include <stdint.h>

namespace BoardInfo {

constexpr uint8_t kMagic         = 0xCF;
constexpr uint8_t kLayoutV1      = 0x01;
constexpr uint8_t kDefaultMajor  = 1;
constexpr uint8_t kDefaultMinor  = 2;
constexpr uint8_t kFlagHil       = 0x01;
constexpr uint8_t kFlagEngSample = 0x02;

enum class Source : uint8_t {
    Default,        // magic absent (unprogrammed board) or block unreadable
    Efuse,          // magic present and layout understood
    UnknownLayout,  // magic present, layout version not understood
    ReadError,      // the block could not be read; defaults reported so the
                    // board still boots, but a provisioned board must not be
                    // mistaken for an unprogrammed one
};

struct Info {
    Source  source;
    uint8_t major;
    uint8_t minor;
    bool    hil;
    bool    engSample;
    uint8_t year;           // manufacturing year minus 2020
    uint8_t month;
    uint8_t lot;
    uint8_t variant;
    uint8_t layoutVersion;  // raw byte 8 when the magic is present, else 0
};

/// The identity reported when no usable block is available.
Info defaults();

/// Parse the 32-byte BLK3 image exactly as the chip returned it.
Info parseBoardBlock(const uint8_t blk[32]);

/// Stable lowercase name for the `info` reply: efuse | default | unknown-layout.
const char* sourceName(Source source);

}  // namespace BoardInfo

#endif  // BOARD_INFO_H
