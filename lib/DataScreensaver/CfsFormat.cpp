// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

// lib/DataScreensaver/CfsFormat.cpp - the `.cfs` v1 reader (see CfsFormat.h).

#include "CfsFormat.h"

#include <string.h>

namespace Cfs {
namespace {

uint16_t readU16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

}  // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:            return "ok";
        case Status::Truncated:     return "truncated";
        case Status::BadMagic:      return "bad_magic";
        case Status::BadVersion:    return "bad_version";
        case Status::BadEncoding:   return "bad_encoding";
        case Status::BadLoopMode:   return "bad_loop";
        case Status::BadSize:       return "bad_size";
        case Status::BadFrameCount: return "bad_frame_count";
        case Status::BadDuration:   return "bad_duration";
        case Status::SizeMismatch:  return "size_mismatch";
        case Status::TooLarge:      return "too_large";
        case Status::ReadFailed:    return "read_failed";
    }
    return "unknown";
}

uint32_t frameBytes(uint16_t width, uint16_t height) {
    return (uint32_t)((width + 7u) / 8u) * (uint32_t)height;
}

uint32_t framesOffset(const Header& h) {
    return (uint32_t)kHeaderBytes + 2u * (uint32_t)h.frameCount;
}

uint32_t expectedFileBytes(const Header& h) {
    return framesOffset(h) + frameBytes(h.width, h.height) * (uint32_t)h.frameCount;
}

uint32_t frameOffset(const Header& h, uint16_t index) {
    return framesOffset(h) + frameBytes(h.width, h.height) * (uint32_t)index;
}

Status parseHeader(const uint8_t* data, size_t len, uint32_t fileBytes, Header& out) {
    if (fileBytes > kMaxFileBytes) return Status::TooLarge;
    if (data == nullptr || len < kHeaderBytes || fileBytes < kHeaderBytes) return Status::Truncated;
    if (memcmp(data, "CFS1", 4) != 0) return Status::BadMagic;
    Header h;
    h.version    = data[4];
    h.encoding   = data[5];
    h.loopMode   = data[6];
    // data[7] is reserved: written as 0, ignored here.
    h.width      = readU16(data + 8);
    h.height     = readU16(data + 10);
    h.frameCount = readU16(data + 12);
    if (h.version != kVersion) return Status::BadVersion;
    if (h.encoding != kEncodingRaw) return Status::BadEncoding;
    if (h.loopMode > kMaxLoopMode) return Status::BadLoopMode;
    if (h.width == 0 || h.height == 0 || h.width > kMaxWidth || h.height > kMaxHeight)
        return Status::BadSize;
    if (h.frameCount == 0 || h.frameCount > kMaxFrames) return Status::BadFrameCount;
    if (fileBytes != expectedFileBytes(h)) return Status::SizeMismatch;
    out = h;
    return Status::Ok;
}

Status parseDurations(const uint8_t* data, size_t len, uint16_t count, uint16_t* out) {
    if (count > kMaxFrames) return Status::BadFrameCount;
    if (data == nullptr || out == nullptr || len < 2u * (size_t)count) return Status::Truncated;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t ms = readU16(data + 2u * i);
        if (ms < kMinDurationMs) return Status::BadDuration;
        out[i] = ms;
    }
    return Status::Ok;
}

Status Reader::open(ReadAtFn read, void* ctx, uint32_t fileBytes) {
    open_   = false;
    loaded_ = -1;
    read_   = read;
    ctx_    = ctx;
    if (read == nullptr) return Status::ReadFailed;
    if (fileBytes > kMaxFileBytes) return Status::TooLarge;
    if (fileBytes < kHeaderBytes) return Status::Truncated;

    // The frame buffer doubles as scratch for the header and the duration
    // table (both well under one frame), so the reader never needs more
    // than its one frame of RAM.
    if (read(ctx, 0, frame_, kHeaderBytes) != kHeaderBytes) return Status::ReadFailed;
    Header h;
    Status st = parseHeader(frame_, kHeaderBytes, fileBytes, h);
    if (st != Status::Ok) return st;

    const size_t tableBytes = 2u * (size_t)h.frameCount;  // <= 510 < kMaxFrameBytes
    if (read(ctx, (uint32_t)kHeaderBytes, frame_, tableBytes) != tableBytes) return Status::ReadFailed;
    st = parseDurations(frame_, tableBytes, h.frameCount, durations_);
    if (st != Status::Ok) return st;

    memset(frame_, 0, sizeof(frame_));
    header_ = h;
    open_   = true;
    return Status::Ok;
}

bool Reader::loadFrame(uint16_t index) {
    if (!open_ || index >= header_.frameCount) return false;
    const size_t bytes = frameBytes(header_.width, header_.height);
    if (read_(ctx_, frameOffset(header_, index), frame_, bytes) != bytes) {
        loaded_ = -1;
        return false;
    }
    loaded_ = index;
    return true;
}

}  // namespace Cfs
