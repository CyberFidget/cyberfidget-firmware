// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * SyncProtocol - pure (Arduino-free) core for the serial sync transport.
 *
 * The browser drives device storage over USB serial (921600 baud) through
 * verbs on the SerialCli surface. This header holds the parts that are pure
 * data logic, so the same code compiles for native unit tests and the WASM
 * emulator: the CRC-32 the transport checksums every payload with, command
 * argument parsers, confinement, and bounded reply formatting.
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

/// Largest payload a single write (`fwdata`) or read (`fread`) chunk may
/// carry. The device advertises it in both open/read replies and refuses an
/// oversized request. Chosen to bound device-side buffering while keeping
/// framing overhead per chunk small at 921600 baud.
constexpr uint32_t kMaxChunkBytes = 4096;

/// Largest loadout-ops document (`lapply` payload) the device will accept.
/// Ops documents are small JSON (a handful of adds/removes plus one arrange
/// over the installed app set), so this is generous headroom, not a target.
constexpr uint32_t kMaxApplyBytes = 8192;

/// Longest confined path the transport accepts, including the leading root.
/// Bounds the SerialCli command buffer and every path scratch buffer.
constexpr size_t kMaxPathLen = 96;

/// Most entries one `flist` call emits. Sixty-four bounds filesystem walks
/// and serial output while covering the normal app/asset directory sizes;
/// the summary explicitly reports when another entry was present.
constexpr uint32_t kMaxListEntries = 64;

/// Reply scratch capacity for the longest path-bearing read header. This
/// covers a kMaxPathLen path plus decimal uint32 fields and terminator.
constexpr size_t kReadReplyBytes = 192;

/// Where the device durably records the last batch `lapply` applied. The
/// file verbs never reach it (pathConfined() refuses this path and anything
/// that extends it, such as its `.part` temp), so a host can neither forge
/// nor erase the record; only the apply path writes it.
constexpr const char* kAppliedRecordPath = "/apps/.applied.json";
constexpr const char* kAppliedRecordTemp = "/apps/.applied.json.part";

/// `lapply` capability advertised as `[cmd] syncinfo.lapply=<value>`. A
/// sender only sends `batch` / `base` / `replace` to a device reporting it
/// (older firmware silently ignores `batch` and `base`). Bump the suffix
/// for an incompatible change to the batch contract.
constexpr const char* kLapplyCapability = "batch1";

/// Directory the orphan-blob sweep after a batch apply walks (top level only).
constexpr const char* kDeliveredBlobDir = "/apps";

/**
 * True if `name` (a basename, no directory) has the delivered-app blob shape
 * `<id>-<hash8>.wasm`: a non-empty id that does not start with '.', one '-',
 * exactly 8 lowercase hex digits, then `.wasm`. Only files of this shape are
 * ever deleted by the orphan sweep; any other name (for example a browser
 * send's `<id>.wasm`) is left alone.
 */
bool isDeliveredBlobName(const char* name);

/// Format ESP.getEfuseMac() as the canonical 12-character lowercase unit id.
/// `out` must have room for 13 bytes including the terminator.
void formatDeviceId(uint64_t efuseMac, char* out);

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) - the checksum every
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
 * True if `path` is an absolute path inside a confined storage root
 * (`/apps/` or `/assets/`) that names a file. Rejects: null/empty, any path
 * not beginning with an allowed root, any `..` traversal segment, embedded
 * spaces or control bytes, a bare root or a trailing-slash directory, and
 * anything longer than kMaxPathLen. Mirrors the portal's card-confinement
 * stance so a hostile browser can't read, overwrite, or delete anything
 * outside the app/asset area. The loadout manifest itself is intentionally
 * NOT writable here - it is edited only through the `lapply` verb, which
 * runs the manifest apply core rather than a blind byte overwrite. The
 * applied-batch record (kAppliedRecordPath, and any path extending it) is
 * refused for the same reason.
 */
bool pathConfined(const char* path);

// ---------------------------------------------------------------------------
// Command-line argument parsers. Each takes the argument tail (everything
// after the verb token) and fills the out-params, returning false on any
// malformed field or trailing garbage. Kept pure so the native suite can
// pin the framing without a UART.
// ---------------------------------------------------------------------------

/// `fwrite <path> <size> <crc32hex>` - open a write session.
bool parseWriteOpen(const char* args, char* pathOut, size_t pathCap,
                    uint32_t& sizeOut, uint32_t& crcOut);

/// `fwdata <offset> <len> <crc32hex>` - a chunk header (payload bytes follow
/// on the wire, consumed by the device, not by this parser).
bool parseChunkHeader(const char* args, uint32_t& offsetOut,
                      uint32_t& lenOut, uint32_t& crcOut);

/// `lapply <len> <crc32hex>` - an ops-document header (JSON payload follows).
bool parseApplyHeader(const char* args, uint32_t& lenOut, uint32_t& crcOut);

/// A lone confined path argument (`fdelete <path>`).
bool parsePathArg(const char* args, char* pathOut, size_t pathCap);

/// `flist <dir>` - one trailing-slash-free directory argument.
bool parseListArgs(const char* args, char* dirOut, size_t dirCap);

/// `fstat <path>` - one file path argument.
bool parseStatArgs(const char* args, char* pathOut, size_t pathCap);

/// `fread <path> <offset> <len>` - one device-to-host chunk request.
bool parseReadArgs(const char* args, char* pathOut, size_t pathCap,
                   uint32_t& offsetOut, uint32_t& lenOut);

/**
 * Turn a trailing-slash-free `flist` directory into a synthetic child path.
 * The caller must pass that child to pathConfined(); no directory-specific
 * confinement rule is introduced. This is what lets `/apps` and `/assets`
 * name their directory roots while preserving the file-shaped predicate.
 */
bool makeListConfinementProbe(const char* dir, char* pathOut, size_t pathCap);

/// True only for a non-empty fread length within kMaxChunkBytes.
bool readLengthAllowed(uint32_t len);

/// Format the device-to-host fread header, including its line terminator.
/// Returns bytes written, or zero if the output buffer is too small.
size_t formatReadHeader(char* out, size_t cap, const char* path,
                        uint32_t offset, uint32_t len, uint32_t crc);

/// Pure flist cap state used by SerialCli and native tests. Calling
/// admitListEntry() for the first entry beyond the cap marks truncation.
struct ListProgress {
    uint32_t entries = 0;
    bool truncated = false;
};

bool admitListEntry(ListProgress& progress);

/// Format the terminating flist summary, including its line terminator.
/// Returns bytes written, or zero if the output buffer is too small.
size_t formatListSummary(char* out, size_t cap, const char* dir,
                         const ListProgress& progress);

// ---------------------------------------------------------------------------
// USB sync session vs. cloud check-ins. A browser send is a short burst of
// sync verbs; a cloud check-in that starts in the middle of it would hold
// the store and refuse the rest. The device treats recent sync traffic as a
// session in progress (no new wire verb: older senders work unchanged).
// ---------------------------------------------------------------------------

/// How long after the last sync verb a new cloud check-in stays held off.
constexpr uint32_t kUsbSessionHoldMs = 10000;

/// How long a store-writing verb waits for a check-in already in progress
/// to finish before it is refused with `[err] sync.busy`. Below the
/// sender's shortest reply wait (10 s), so a refusal still arrives in time.
constexpr uint32_t kBusyWaitMs = 5000;

/// True if `line` is a sync-session verb: the ones a browser send, update
/// check or recovery issues (`info`, `syncinfo`, `lget`, `lapply`, the
/// `fwrite` family, `fdelete`, `flist`, `fstat`, `fread`). `version` is not
/// one: bench tools poll it constantly and it says nothing about a send.
bool isSessionVerb(const char* line);

/// True if `line` is a verb that writes the store (`fwrite`, `fwdata`,
/// `fwcommit`, `fwabort`, `fdelete`, `lapply`) and so must not run while a
/// cloud check-in owns it.
bool isStoreWriteVerb(const char* line);

/// True while a USB sync session holds off new check-ins: a session verb was
/// `seen`, most recently at `lastAtMs`, less than kUsbSessionHoldMs before
/// `nowMs` (millis() wrap-safe).
bool sessionHeld(bool seen, uint32_t lastAtMs, uint32_t nowMs);

/// Next step for a store-writing verb that arrived while the store may be
/// busy: Run once it is free, Wait while it is busy and kBusyWaitMs has not
/// passed since `waitingSinceMs`, Refuse after that.
enum class BusyStep { Run, Wait, Refuse };
BusyStep busyStep(bool busy, uint32_t waitingSinceMs, uint32_t nowMs);

} // namespace SyncProtocol

#endif // SYNC_PROTOCOL_H
