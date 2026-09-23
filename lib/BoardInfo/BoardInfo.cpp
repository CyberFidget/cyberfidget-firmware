// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "BoardInfo.h"

namespace BoardInfo {

namespace {
constexpr int kOffMagic   = 7;
constexpr int kOffLayout  = 8;
constexpr int kOffMajor   = 9;
constexpr int kOffMinor   = 10;
constexpr int kOffYear    = 11;
constexpr int kOffFlags   = 20;
constexpr int kOffMonth   = 21;
constexpr int kOffLot     = 22;
constexpr int kOffVariant = 24;
}  // namespace

Info defaults() {
    Info info{};
    info.source = Source::Default;
    info.major = kDefaultMajor;
    info.minor = kDefaultMinor;
    return info;
}

Info parseBoardBlock(const uint8_t blk[32]) {
    Info info = defaults();
    if (blk == nullptr || blk[kOffMagic] != kMagic) {
        return info;
    }
    info.layoutVersion = blk[kOffLayout];
    if (info.layoutVersion != kLayoutV1) {
        info.source = Source::UnknownLayout;
        return info;
    }
    const uint8_t flags = blk[kOffFlags];
    info.source    = Source::Efuse;
    info.major     = blk[kOffMajor];
    info.minor     = blk[kOffMinor];
    info.year      = blk[kOffYear];
    info.hil       = (flags & kFlagHil) != 0;
    info.engSample = (flags & kFlagEngSample) != 0;
    info.month     = blk[kOffMonth];
    info.lot       = blk[kOffLot];
    info.variant   = blk[kOffVariant];
    return info;
}

const char* sourceName(Source source) {
    switch (source) {
        case Source::Efuse:         return "efuse";
        case Source::UnknownLayout: return "unknown-layout";
        case Source::ReadError:     return "read-error";
        case Source::Default:
        default:                    return "default";
    }
}

}  // namespace BoardInfo
