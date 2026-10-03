// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

/**
 * CfsFormat - pure (Arduino-free) reader for `.cfs` v1, the on-device form
 * of a drawing or animation sent as data (a "data screensaver").
 *
 * Layout (little-endian):
 *
 *   offset size  field
 *   0      4     magic "CFS1"
 *   4      1     version     (1)
 *   5      1     encoding    (0 = raw frames; other values are reserved)
 *   6      1     loop mode   (cf::gfx::LoopMode: 0 once, 1 once-hold,
 *                             2 loop, 3 ping-pong)
 *   7      1     reserved    (written as 0, ignored by this reader)
 *   8      2     width       (1..128)
 *   10     2     height      (1..64)
 *   12     2     frameCount  (1..255)
 *   14     2*N   durations   (ms per frame, each >= 20)
 *   ...          frames      (N frames, each ((w + 7) / 8) * h bytes:
 *                             row-major, rows padded to whole bytes,
 *                             LSB-first within a byte - the cf::gfx::Sprite
 *                             / drawXbm bit order)
 *
 * The file must be exactly that long and at most kMaxFileBytes. Anything
 * this reader does not know (another magic, version or encoding, a loop
 * mode above 3) is refused, never guessed at. A duration below
 * kMinDurationMs is refused too rather than clamped, so an encoder bug
 * shows up as "can't play" instead of silently playing at another speed.
 *
 * The same code compiles for the firmware, the native tests
 * (pio test -e test_core) and, later, the emulator.
 */

#ifndef CFS_FORMAT_H
#define CFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

namespace Cfs {

constexpr uint8_t  kVersion        = 1;
constexpr uint8_t  kEncodingRaw    = 0;
constexpr uint8_t  kMaxLoopMode    = 3;
constexpr size_t   kHeaderBytes    = 14;
constexpr uint32_t kMaxFileBytes   = 32u * 1024u;
constexpr uint16_t kMaxWidth       = 128;
constexpr uint16_t kMaxHeight      = 64;
constexpr uint16_t kMaxFrames      = 255;
constexpr uint16_t kMinDurationMs  = 20;
/// Largest single frame (a full 128x64 screen). The player's one buffer.
constexpr size_t   kMaxFrameBytes  = ((kMaxWidth + 7) / 8) * kMaxHeight;

enum class Status : uint8_t {
    Ok = 0,
    Truncated,      ///< fewer bytes than the header / table needs
    BadMagic,
    BadVersion,
    BadEncoding,
    BadLoopMode,
    BadSize,        ///< width/height zero or above 128x64
    BadFrameCount,  ///< zero or above 255
    BadDuration,    ///< a frame shorter than kMinDurationMs
    SizeMismatch,   ///< file length differs from what the header implies
    TooLarge,       ///< file above kMaxFileBytes
    ReadFailed,     ///< the byte source returned short
};

/// Short stable token for logs ("ok", "bad_magic", ...).
const char* statusName(Status s);

struct Header {
    uint8_t  version    = 0;
    uint8_t  encoding   = 0;
    uint8_t  loopMode   = 0;
    uint16_t width      = 0;
    uint16_t height     = 0;
    uint16_t frameCount = 0;
};

/// Bytes in one frame: ((w + 7) / 8) * h.
uint32_t frameBytes(uint16_t width, uint16_t height);
/// Byte offset of the first frame (after the duration table).
uint32_t framesOffset(const Header& h);
/// Exact file length the header implies.
uint32_t expectedFileBytes(const Header& h);
/// Byte offset of frame `index` (caller keeps index < frameCount).
uint32_t frameOffset(const Header& h, uint16_t index);

/**
 * Parse and check the fixed 14-byte header. `fileBytes` is the whole
 * file's length: above kMaxFileBytes is TooLarge, and once the fields are
 * valid it must equal expectedFileBytes() (SizeMismatch otherwise). `out`
 * is written only on Ok.
 */
Status parseHeader(const uint8_t* data, size_t len, uint32_t fileBytes, Header& out);

/**
 * Read `count` little-endian u16 durations from `data` (2*count bytes) into
 * `out`, refusing any below kMinDurationMs.
 */
Status parseDurations(const uint8_t* data, size_t len, uint16_t count, uint16_t* out);

/**
 * Streaming reader: holds the header, the duration table and ONE frame
 * buffer (kMaxFrameBytes); frames are read from the source on demand, so
 * the whole file is never in memory. The source is any "read len bytes at
 * offset" function (LittleFS on the device, a byte array in the tests).
 */
typedef size_t (*ReadAtFn)(void* ctx, uint32_t offset, uint8_t* dst, size_t len);

class Reader {
public:
    /// Check the file (header, sizes, durations). On Ok, frame data can be
    /// loaded; on any other status the reader stays closed.
    Status open(ReadAtFn read, void* ctx, uint32_t fileBytes);

    /// Read frame `index` into the frame buffer. False when not open, the
    /// index is out of range, or the source returned short.
    bool loadFrame(uint16_t index);

    bool isOpen() const { return open_; }
    const Header& header() const { return header_; }
    const uint16_t* durations() const { return durations_; }
    /// The frame buffer (the last frame loaded).
    const uint8_t* frame() const { return frame_; }
    /// Index of the frame in the buffer, or -1 before the first load.
    int loadedIndex() const { return loaded_; }

private:
    ReadAtFn read_ = nullptr;
    void*    ctx_  = nullptr;
    Header   header_;
    bool     open_ = false;
    int      loaded_ = -1;
    uint16_t durations_[kMaxFrames] = {};
    uint8_t  frame_[kMaxFrameBytes] = {};
};

}  // namespace Cfs

#endif  // CFS_FORMAT_H
