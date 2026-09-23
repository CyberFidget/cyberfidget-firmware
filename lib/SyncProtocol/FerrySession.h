// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * FerrySession - the transport-agnostic write session behind the sync verbs
 * `fwrite` / `fwdata` / `fwcommit` / `fwabort` and the manifest verb `lapply`.
 *
 * The session owns every piece of write-session state (idle/active, final
 * and temp paths, expected size and whole-file CRC, the staging buffer it
 * holds while active) and every policy decision (confinement, free-space
 * guard, per-chunk CRC gate, range check, whole-file CRC recompute at
 * commit, atomic rename, abort cleanup). It also formats every reply, byte
 * for byte, so any driver that forwards the reply text unchanged produces
 * the same wire outcome as the serial CLI.
 *
 * What it does NOT own is injected:
 *   - FerryStorage: filesystem effects (mount, free space, temp open/seek/
 *     write/close, read-back, rename, remove), the staging-buffer allocator,
 *     and the manifest apply used by `lapply`.
 *   - FerryByteSource: the raw payload bytes that follow a `fwdata` or
 *     `lapply` header. Transport timing (gap / total-duration limits) is the
 *     byte source's concern; the session only sees "got the bytes" or
 *     "timed out".
 *
 * Pure C++17: no Arduino, UART, or filesystem includes, so the native
 * `test_sync` suite drives it directly with fakes.
 */

#ifndef SYNC_PROTOCOL_FERRY_SESSION_H
#define SYNC_PROTOCOL_FERRY_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "SyncProtocol.h"

namespace SyncProtocol {

/// Staging-buffer capacity: one `lapply` document plus a null terminator.
/// Also covers a kMaxChunkBytes `fwdata` chunk and is the read-back block
/// size at commit.
constexpr size_t kPayloadBufBytes = kMaxApplyBytes + 1;

/// Reply scratch capacity. Every ferry reply is one line; the longest carries
/// a kMaxPathLen path plus a few decimal/hex fields and the terminator.
constexpr size_t kFerryReplyBytes = 192;

/// Filesystem + buffer effects a FerrySession drives. The device adapter
/// wraps LittleFS; native tests use an in-memory fake.
class FerryStorage {
public:
    virtual ~FerryStorage() = default;

    /// Mount (or confirm mounted) the storage the session writes to.
    virtual bool mount() = 0;
    /// Bytes currently free (approximate; used only as an up-front guard).
    virtual size_t freeBytes() = 0;

    /// Return a staging buffer of kPayloadBufBytes, or nullptr when out of
    /// memory. Repeated calls may return the same buffer.
    virtual uint8_t* acquirePayload() = 0;
    /// Release the staging buffer. The session calls this only while idle.
    virtual void releasePayload() = 0;

    /// Create any missing parent directories of `tempPath`, then open it for
    /// writing, truncating any existing file. One temp file is open at a time.
    virtual bool openTemp(const char* tempPath) = 0;
    virtual bool seekTemp(uint32_t offset) = 0;
    /// Returns the number of bytes written.
    virtual size_t writeTemp(const uint8_t* data, size_t len) = 0;
    /// Flush and close the temp file. A no-op when none is open.
    virtual void closeTemp() = 0;

    /// Open `path` for read-back; reports its size.
    virtual bool openReadBack(const char* path, uint32_t& sizeOut) = 0;
    /// Read up to `cap` bytes; returns <= 0 at end of file or on error.
    virtual int readBack(uint8_t* buf, size_t cap) = 0;
    virtual void closeReadBack() = 0;

    virtual bool rename(const char* from, const char* to) = 0;
    virtual bool remove(const char* path) = 0;

    /// Apply a null-terminated staged-ops document to the stored manifest
    /// and persist it atomically. Returns false (manifest untouched) on a
    /// malformed document, a rejected op, or a failed save.
    virtual bool applyManifestOps(const char* opsJson, int& entriesOut,
                                  int& appliedOut) = 0;
};

/// Raw payload bytes that follow a length-framed header.
class FerryByteSource {
public:
    virtual ~FerryByteSource() = default;
    /// Read exactly `n` bytes into `buf`; false if the transport gave up
    /// (stall or overall time limit) before all `n` arrived.
    virtual bool readExact(uint8_t* buf, size_t n) = 0;
    /// Consume and discard up to `n` bytes so the stream stays in frame sync
    /// after a header the session refuses. Gives up on the same limits.
    virtual void drain(size_t n) = 0;
};

/// One complete reply line, terminator included, ready to send verbatim.
struct FerryReply {
    bool   ok = false;               ///< true for a `[cmd]` reply
    size_t len = 0;
    char   text[kFerryReplyBytes] = {0};
};

class FerrySession {
public:
    enum class State : uint8_t { Idle, Active };

    explicit FerrySession(FerryStorage& storage) : storage_(storage) {}
    /// Destruction aborts: an active session's temp file is removed.
    ~FerrySession() { reset(); }

    FerrySession(const FerrySession&) = delete;
    FerrySession& operator=(const FerrySession&) = delete;

    State state() const { return state_; }
    bool active() const { return state_ == State::Active; }

    /// `fwrite <path> <size> <crc32>` argument tail.
    FerryReply open(const char* args);
    /// `fwdata <offset> <len> <crc32>` argument tail; the payload follows in
    /// `in`.
    FerryReply chunk(const char* args, FerryByteSource& in);
    /// `fwcommit`.
    FerryReply commit();
    /// `fwabort`.
    FerryReply abort();
    /// `lapply <len> <crc32>` argument tail; the ops document follows in
    /// `in`. Independent of the write state: it neither needs nor disturbs
    /// an active write session.
    FerryReply applyManifest(const char* args, FerryByteSource& in);

    /// Abort without a reply: remove only this session's temp file (if a
    /// session is active) and return to Idle.
    void reset();

private:
    void clear();
    void releasePayloadIfIdle();
    void discard();  // remove the temp file, then clear()

    FerryStorage& storage_;
    State    state_ = State::Idle;
    uint8_t* payload_ = nullptr;  // staging buffer held while Active
    char     path_[kMaxPathLen + 1] = {0};   // final path (confined)
    char     temp_[kMaxPathLen + 6] = {0};   // final path + ".part"
    uint32_t size_ = 0;                      // expected total size
    uint32_t crc_  = 0;                      // expected whole-file crc32
};

} // namespace SyncProtocol

#endif // SYNC_PROTOCOL_FERRY_SESSION_H
