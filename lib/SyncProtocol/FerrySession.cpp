// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/SyncProtocol/FerrySession.cpp - see FerrySession.h for the design and
// README.md for the wire framing. Pure C++17, no Arduino.
//
// Reply bytes are part of the frozen serial contract. Two terminators are in
// use and must not be unified: replies the serial CLI historically sent with
// println() end in "\r\n" (setLine), replies it sent with printf("...\n")
// end in "\n" (setFormat). Checks and side effects run in the historical
// order too (e.g. a refused `fwdata` drains its payload BEFORE the reply).

#include "FerrySession.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace SyncProtocol {

namespace {

// println()-style reply: text + "\r\n".
FerryReply setLine(bool ok, const char* text) {
    FerryReply r;
    r.ok = ok;
    const int n = std::snprintf(r.text, sizeof(r.text), "%s\r\n", text);
    r.len = (n < 0) ? 0
          : ((size_t)n >= sizeof(r.text) ? sizeof(r.text) - 1 : (size_t)n);
    return r;
}

// printf()-style reply: the format carries its own "\n".
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
FerryReply setFormat(bool ok, const char* fmt, ...) {
    FerryReply r;
    r.ok = ok;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(r.text, sizeof(r.text), fmt, ap);
    va_end(ap);
    r.len = (n < 0) ? 0
          : ((size_t)n >= sizeof(r.text) ? sizeof(r.text) - 1 : (size_t)n);
    return r;
}

} // namespace

void FerrySession::releasePayloadIfIdle() {
    if (state_ == State::Idle) storage_.releasePayload();
}

void FerrySession::clear() {
    storage_.closeTemp();
    state_ = State::Idle;
    payload_ = nullptr;
    path_[0] = '\0';
    temp_[0] = '\0';
    size_ = 0;
    crc_  = 0;
    releasePayloadIfIdle();
}

// The temp file is closed before it is removed: the device's filesystem
// refuses to unlink an open file, and the orphan would hold space until the
// next reboot's sweep.
void FerrySession::discard() {
    storage_.closeTemp();
    if (temp_[0]) storage_.remove(temp_);
    clear();
}

void FerrySession::reset() {
    if (state_ == State::Active) storage_.closeTemp();
    if (state_ == State::Active && temp_[0]) storage_.remove(temp_);
    clear();
}

FerryReply FerrySession::open(const char* args) {
    char path[kMaxPathLen + 1];
    uint32_t size = 0, crc = 0;
    if (!parseWriteOpen(args, path, sizeof(path), size, crc)) {
        return setLine(false, "[err] fwrite.usage=fwrite <path> <size> <crc32>");
    }
    // pathConfined() is also what keeps /loadout.json out of reach here: the
    // manifest changes only through applyManifest().
    if (!pathConfined(path)) {
        return setFormat(false,
                         "[err] fwrite.path=%s (confined to /apps/ or /assets/)\n",
                         path);
    }
    if (!storage_.mount()) {
        return setLine(false, "[err] fwrite.fs=mount failed");
    }
    // Free-space guard (approximate - used bytes include an old copy if this
    // overwrites, so this only rejects clearly-too-large transfers).
    const size_t freeB = storage_.freeBytes();
    if ((size_t)size > freeB) {
        return setFormat(false, "[err] fwrite.space=need %u free %u\n",
                         (unsigned)size, (unsigned)freeB);
    }

    // Abort any stale session, then open a fresh temp file.
    if (state_ == State::Active) discard();
    uint8_t* buf = storage_.acquirePayload();
    if (buf == nullptr) {
        return setLine(false, "[err] fwrite.nomem");
    }
    std::strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';
    std::snprintf(temp_, sizeof(temp_), "%s.part", path_);

    if (!storage_.openTemp(temp_)) {
        FerryReply r = setFormat(false, "[err] fwrite.open=%s\n", temp_);
        clear();
        return r;
    }
    state_   = State::Active;
    payload_ = buf;
    size_    = size;
    crc_     = crc;
    return setFormat(true, "[cmd] fwrite.ok=%s size=%u chunk=%u crc=%08x\n",
                     path_, (unsigned)size, (unsigned)kMaxChunkBytes,
                     (unsigned)crc);
}

FerryReply FerrySession::chunk(const char* args, FerryByteSource& in) {
    uint32_t offset = 0, len = 0, crc = 0;
    if (!parseChunkHeader(args, offset, len, crc)) {
        // Length unknown -> cannot resync the stream; the host must abort.
        return setLine(false, "[err] fwdata.usage=fwdata <offset> <len> <crc32>");
    }
    if (len == 0) {
        return setLine(false, "[err] fwdata.len=0");
    }
    if (len > kMaxChunkBytes) {
        in.drain(len);
        return setFormat(false, "[err] fwdata.toobig=%u max=%u\n",
                         (unsigned)len, (unsigned)kMaxChunkBytes);
    }
    if (state_ != State::Active) {
        in.drain(len);
        return setLine(false, "[err] fwdata.nosession");
    }
    if ((uint64_t)offset + len > size_) {
        in.drain(len);
        return setFormat(false, "[err] fwdata.range=off %u len %u size %u\n",
                         (unsigned)offset, (unsigned)len, (unsigned)size_);
    }
    if (payload_ == nullptr) {
        in.drain(len);
        discard();
        return setLine(false, "[err] fwdata.nomem");
    }
    if (!in.readExact(payload_, len)) {
        discard();
        return setLine(false, "[err] fwdata.timeout");
    }
    // Per-chunk integrity: a corrupt chunk is NAKed and never written, so the
    // host resends the same offset. The whole-file crc at commit is the final
    // gate against a silently-missed chunk.
    const uint32_t got = crc32(payload_, len);
    if (got != crc) {
        return setFormat(false, "[err] fwdata.crc=off %u got %08x want %08x\n",
                         (unsigned)offset, (unsigned)got, (unsigned)crc);
    }
    if (!storage_.seekTemp(offset)) {
        return setFormat(false, "[err] fwdata.seek=%u\n", (unsigned)offset);
    }
    const size_t wrote = storage_.writeTemp(payload_, len);
    if (wrote != len) {
        discard();
        return setFormat(false, "[err] fwdata.write=%u/%u\n",
                         (unsigned)wrote, (unsigned)len);
    }
    return setFormat(true, "[cmd] fwdata.ok=off %u len %u\n",
                     (unsigned)offset, (unsigned)len);
}

FerryReply FerrySession::commit() {
    if (state_ != State::Active) {
        return setLine(false, "[err] fwcommit.nosession");
    }
    if (payload_ == nullptr) {
        discard();
        return setLine(false, "[err] fwcommit.nomem");
    }
    storage_.closeTemp();

    // Re-read the finished temp file and recompute the whole-file crc, so a
    // dropped or out-of-order chunk (any transfer that doesn't reproduce the
    // host's bytes exactly) is rejected here rather than half-applied.
    uint32_t fileSize = 0;
    if (!storage_.openReadBack(temp_, fileSize)) {
        discard();
        return setLine(false, "[err] fwcommit.reopen");
    }
    uint32_t crc = crc32Begin();
    while (true) {
        const int n = storage_.readBack(payload_, kPayloadBufBytes);
        if (n <= 0) break;
        crc = crc32Update(crc, payload_, (size_t)n);
    }
    crc = crc32Finish(crc);
    storage_.closeReadBack();

    if (fileSize != size_) {
        FerryReply r = setFormat(false, "[err] fwcommit.size=got %u want %u\n",
                                 (unsigned)fileSize, (unsigned)size_);
        discard();
        return r;
    }
    if (crc != crc_) {
        FerryReply r = setFormat(false, "[err] fwcommit.crc=got %08x want %08x\n",
                                 (unsigned)crc, (unsigned)crc_);
        discard();
        return r;
    }

    // Atomic-ish publish: rename temp over the final path. Keep the
    // remove+retry fallback in case the filesystem refuses an overwrite. A
    // power cut here leaves the old file or none - never a half-written blob.
    bool ok = storage_.rename(temp_, path_);
    if (!ok) {
        storage_.remove(path_);
        ok = storage_.rename(temp_, path_);
    }
    if (!ok) {
        FerryReply r = setFormat(false, "[err] fwcommit.rename=%s\n", path_);
        discard();
        return r;
    }
    FerryReply r = setFormat(true, "[cmd] fwcommit.ok=%s size=%u crc=%08x\n",
                             path_, (unsigned)fileSize, (unsigned)crc);
    clear();
    return r;
}

FerryReply FerrySession::abort() {
    reset();
    return setLine(true, "[cmd] fwabort.ok");
}

FerryReply FerrySession::applyManifest(const char* args, FerryByteSource& in) {
    uint32_t len = 0, crc = 0;
    if (!parseApplyHeader(args, len, crc)) {
        return setLine(false, "[err] lapply.usage=lapply <len> <crc32>");
    }
    if (len == 0) {
        return setLine(false, "[err] lapply.len=0");
    }
    if (len > kMaxApplyBytes) {
        in.drain(len);
        return setFormat(false, "[err] lapply.toobig=%u max=%u\n",
                         (unsigned)len, (unsigned)kMaxApplyBytes);
    }
    uint8_t* buf = storage_.acquirePayload();
    if (buf == nullptr) {
        in.drain(len);
        return setLine(false, "[err] lapply.nomem");
    }
    if (!in.readExact(buf, len)) {
        releasePayloadIfIdle();
        return setLine(false, "[err] lapply.timeout");
    }
    const uint32_t got = crc32(buf, len);
    if (got != crc) {
        releasePayloadIfIdle();
        return setFormat(false, "[err] lapply.crc=got %08x want %08x\n",
                         (unsigned)got, (unsigned)crc);
    }
    buf[len] = '\0';
    int entries = 0, applied = 0;
    if (!storage_.applyManifestOps((const char*)buf, entries, applied)) {
        // Malformed document, a rejected op, or a failed save - the stored
        // manifest is untouched, so the menu still falls back cleanly.
        releasePayloadIfIdle();
        return setLine(false, "[err] lapply.reject");
    }
    releasePayloadIfIdle();
    return setFormat(true, "[cmd] lapply.ok=applied %d entries %d\n",
                     applied, entries);
}

} // namespace SyncProtocol
