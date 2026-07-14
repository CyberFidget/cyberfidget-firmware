// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * SyncProtocol — pure (Arduino-free) core for the serial sync transport.
 *
 * The browser drives device storage over USB serial (921600 baud) through
 * verbs on the SerialCli surface. This header holds the parts that are pure
 * data logic, so the same code compiles for native unit tests and the WASM
 * emulator: the CRC-32 the transport checksums every payload with, the
 * command-line argument parsers, and the write-confinement predicate.
 *
 * The device-side glue that actually reads bytes off the UART and touches
 * the filesystem lives in SerialCli (Arduino-coupled) and never compiles
 * for the native tests. Wire framing, verb list, and example byte flows are
 * documented in README.md next to this file.
 */

#ifndef SYNC_PROTOCOL_H
#define SYNC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace SyncProtocol {

/// Largest payload a single write chunk (`fwdata`) may carry. The device
/// advertises it in the `fwrite.ok` reply; a browser that exceeds it is
/// rejected. Chosen to bound the device-side read buffer while keeping the
/// framing overhead per chunk small at 921600 baud.
constexpr uint32_t kMaxChunkBytes = 4096;

/// Largest loadout-ops document (`lapply` payload) the device will accept.
/// Ops documents are small JSON (a handful of adds/removes plus one arrange
/// over the installed app set), so this is generous headroom, not a target.
constexpr uint32_t kMaxApplyBytes = 8192;

/// Longest confined path the transport accepts, including the leading root.
/// Bounds the SerialCli command buffer and every path scratch buffer.
constexpr size_t kMaxPathLen = 96;

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — the checksum every
// framed payload carries. Matches the stock zlib / JS crc32 so the browser
// and device agree byte for byte.
//
// One-shot:  crc32(buf, len).
// Streaming: seed = crc32Begin(); seed = crc32Update(seed, buf, len); ...;
//            result = crc32Finish(seed);
// ---------------------------------------------------------------------------
uint32_t crc32(const void* data, size_t len);
uint32_t crc32Begin();
uint32_t crc32Update(uint32_t crc, const void* data, size_t len);
uint32_t crc32Finish(uint32_t crc);

/**
 * True if `path` is an absolute path inside a write-confined storage root
 * (`/apps/` or `/assets/`) that names a file. Rejects: null/empty, any path
 * not beginning with an allowed root, any `..` traversal segment, embedded
 * spaces or control bytes, a bare root or a trailing-slash directory, and
 * anything longer than kMaxPathLen. Mirrors the portal's card-confinement
 * stance so a hostile browser can't read, overwrite, or delete anything
 * outside the app/asset area. The loadout manifest itself is intentionally
 * NOT writable here — it is edited only through the `lapply` verb, which
 * runs the manifest apply core rather than a blind byte overwrite.
 */
bool pathConfined(const char* path);

// ---------------------------------------------------------------------------
// Command-line argument parsers. Each takes the argument tail (everything
// after the verb token) and fills the out-params, returning false on any
// malformed field or trailing garbage. Kept pure so the native suite can
// pin the framing without a UART.
// ---------------------------------------------------------------------------

/// `fwrite <path> <size> <crc32hex>` — open a write session.
bool parseWriteOpen(const char* args, char* pathOut, size_t pathCap,
                    uint32_t& sizeOut, uint32_t& crcOut);

/// `fwdata <offset> <len> <crc32hex>` — a chunk header (payload bytes follow
/// on the wire, consumed by the device, not by this parser).
bool parseChunkHeader(const char* args, uint32_t& offsetOut,
                      uint32_t& lenOut, uint32_t& crcOut);

/// `lapply <len> <crc32hex>` — an ops-document header (JSON payload follows).
bool parseApplyHeader(const char* args, uint32_t& lenOut, uint32_t& crcOut);

/// A lone confined path argument (`fdelete <path>`).
bool parsePathArg(const char* args, char* pathOut, size_t pathCap);

} // namespace SyncProtocol

#endif // SYNC_PROTOCOL_H
