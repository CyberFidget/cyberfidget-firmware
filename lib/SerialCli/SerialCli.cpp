// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SerialCli.h"

#include <Arduino.h>
#include <esp_system.h>

#include <FS.h>
#include <LittleFS.h>

#include "globals.h"  // getFirmwareVersionString() and friends
#include "version.h"  // FW_GIT_DIRTY for the `info` command

#include "AppManager.h"       // applyLoadoutOps (manifest apply + persist)
#include "AppDefs.h"          // load + builtin identity migration
#include "LoadoutManifest.h"  // parse for the lget entry count
#include "LoadoutStore.h"     // LittleFS mount + manifest read
#include "MenuManager.h"      // menutree dump (T-183/T-191)
#include "SyncProtocol.h"     // pure crc / confinement / arg parsing

#ifdef CF_TEST_CLI
#include <stdlib.h>          // strtol for `btn` argument parsing
#include "ButtonManager.h"   // injectEvent / ButtonEvent for `btn` (spike port)
#include "HAL.h"             // HAL::buttonManager()
#include "WasmFsApp.h"       // wasmstat (T-183)
#endif

#ifdef CF_TEST_CLI
#include <Preferences.h>
#include <WiFi.h>

#include <esp_heap_caps.h>

#include "AppDefs.h"
#include "AppManager.h"
#include "MicCapture.h"
#endif

namespace {
// Case-insensitive C-string compare (avoids depending on platform strcasecmp,
// which has different headers across toolchains).
bool ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

// True if `line` starts with `verb` followed by a space; sets *arg to the
// first character after the space run. Case-insensitive on the verb.
bool verbWithArg(const char* line, const char* verb, const char** arg) {
    size_t n = strlen(verb);
    for (size_t i = 0; i < n; ++i) {
        char ca = line[i];
        char cb = verb[i];
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (ca != cb) return false;
    }
    if (line[n] != ' ') return false;
    const char* p = line + n;
    while (*p == ' ') ++p;
    if (*p == '\0') return false;
    *arg = p;
    return true;
}

// =========================================================================
// Sync-transport session state (device-only; SerialCli never compiles for
// the native tests). One write session at a time — the browser opens with
// `fwrite`, streams `fwdata` chunks, and closes with `fwcommit`/`fwabort`.
// The whole-file CRC is recomputed from the temp file at commit, so chunks
// may arrive in any order and any chunk may be retried (restartable).
// =========================================================================
bool     g_writeActive = false;
File     g_writeFile;
char     g_writePath[128]  = {0};  // final path (confined)
char     g_writeTemp[128]  = {0};  // temp path (final + ".part")
uint32_t g_writeSize       = 0;    // expected total size
uint32_t g_writeCrc        = 0;    // expected whole-file crc32

// Shared payload staging buffer for `fwdata` chunks and `lapply` documents
// (never used by both at once). +1 for a null terminator on ops JSON.
uint8_t  g_payloadBuf[SyncProtocol::kMaxApplyBytes + 1];

// Payload read bounds. A framed payload read runs inside SerialCli::poll(),
// which runs inside AppManager::loop(), so an unbounded read hands a hostile
// (or merely wedged) host a lever to freeze the whole device. Two independent
// bounds close that off:
//   * Inter-byte gap: at 921600 baud a byte is ~11us on the wire, so a full
//     second of silence mid-payload already means the transfer has died —
//     stop waiting rather than hang on a yanked cable.
//   * Total duration: a 4KB chunk is ~45ms of actual wire time and an 8KB ops
//     doc ~90ms, so any legal payload completes in well under a second even
//     with generous USB-host scheduling slack. Capping the whole read at
//     max(2s, size/floor-rate) means a host that dribbles one byte just
//     inside the gap window every time still can't hold the loop past a
//     couple of seconds. Without the total cap the gap alone lets a
//     1-byte-per-(gap-epsilon) drip block poll() indefinitely.
constexpr uint32_t kPayloadGapMs       = 1000;  // max silence between bytes
constexpr uint32_t kPayloadMinTotalMs  = 2000;  // floor for the whole read
constexpr uint32_t kPayloadMaxTotalMs  = 4000;  // hard ceiling for the whole read
// Conservative floor throughput used only for the size-proportional term:
// ~10 bytes/ms (~10KB/s) is ~9x slower than the 921600-baud line, so this is
// pure slack. In practice min/ceiling dominate for the current payload sizes.
constexpr uint32_t kPayloadFloorBytesPerMs = 10;

uint32_t payloadTotalCapMs(size_t n) {
    uint32_t sized = (uint32_t)(n / kPayloadFloorBytesPerMs);
    uint32_t total = (sized > kPayloadMinTotalMs) ? sized : kPayloadMinTotalMs;
    return (total > kPayloadMaxTotalMs) ? kPayloadMaxTotalMs : total;
}

// Read exactly n bytes of raw payload off the UART into buf. Returns false if
// the stream stalls (no byte for gapMs) OR the whole read outruns the total
// cap (size-proportional, so it can never wedge poll() for more than a couple
// of seconds). Both are duration deltas, so millis() wraparound is a no-op.
bool readExact(uint8_t* buf, size_t n, uint32_t gapMs) {
    size_t got = 0;
    const uint32_t start    = millis();
    const uint32_t totalCap = payloadTotalCapMs(n);
    uint32_t lastByte = start;
    while (got < n) {
        int b = Serial.read();
        if (b < 0) {
            uint32_t now = millis();
            if (now - lastByte > gapMs)    return false;  // inter-byte stall
            if (now - start    > totalCap) return false;  // total-duration cap
            continue;
        }
        buf[got++] = (uint8_t)b;
        lastByte = millis();
    }
    return true;
}

// Drain and discard exactly n bytes (used to stay in frame sync after a
// header the device must reject but whose payload the browser still sends).
// Bounded by the same gap + total-duration rules as readExact so a stalled
// drain can't hang the loop either.
void drainBytes(size_t n, uint32_t gapMs) {
    const uint32_t start    = millis();
    const uint32_t totalCap = payloadTotalCapMs(n);
    uint32_t lastByte = start;
    while (n > 0) {
        int b = Serial.read();
        if (b < 0) {
            uint32_t now = millis();
            if (now - lastByte > gapMs)    return;  // inter-byte stall
            if (now - start    > totalCap) return;  // total-duration cap
            continue;
        }
        n--;
        lastByte = millis();
    }
}

// A CRLF host terminates a command line with "\r\n". poll() dispatches on the
// '\r'; the paired '\n' is ~11us behind it on the wire (921600 baud) and is
// normally already sitting in the UART FIFO. Swallow exactly that one '\n'
// before dispatch(), so it can never be read as byte 0 of a length-framed
// payload (fwdata/lapply) — which would fail the chunk CRC AND leave a stray
// byte that corrupts the next command line — nor be seen as a spurious empty
// line. The short bounded wait covers the rare case where '\r' was the last
// byte drained just before '\n' landed; a lone-'\r' host (no following '\n')
// or an LF-only host falls through fast without consuming a real byte.
constexpr uint32_t kCrlfPairWaitMs = 4;

void swallowPairedLf() {
    uint32_t start = millis();
    for (;;) {
        int b = Serial.peek();
        if (b >= 0) {
            if (b == '\n') Serial.read();  // consume the pair's '\n'
            return;                         // stop at the first byte either way
        }
        if (millis() - start > kCrlfPairWaitMs) return;  // no pair arrived
    }
}

// Create every intermediate directory in a confined file path so a nested
// target (e.g. /apps/sub/x.dat) can actually be opened for write — Arduino
// LittleFS does not auto-create parent directories on open(). `path` is an
// absolute, already-confined file path; only its directory prefixes are
// created, never the file itself.
void ensureParentDirs(const char* path) {
    char dir[sizeof(g_writeTemp)];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    // Walk each '/' after the leading one, creating the prefix directory.
    for (char* s = dir + 1; *s; ++s) {
        if (*s != '/') continue;
        *s = '\0';
        if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
        *s = '/';
    }
}

void clearWriteSession() {
    if (g_writeFile) g_writeFile.close();
    g_writeActive = false;
    g_writePath[0] = '\0';
    g_writeTemp[0] = '\0';
    g_writeSize = 0;
    g_writeCrc  = 0;
}
}  // namespace

SerialCli& SerialCli::instance() {
    static SerialCli singleton;
    return singleton;
}

void SerialCli::poll() {
#ifdef CF_TEST_CLI
    pollPendingTapReleases();
#endif
    while (Serial.available() > 0) {
        int byte = Serial.read();
        if (byte < 0) break;
        char c = static_cast<char>(byte);
        if (c == '\n' || c == '\r') {
            if (overflow) {
                Serial.println("[err] line too long");
                overflow = false;
                bufferLen = 0;
                continue;
            }
            if (bufferLen == 0) continue;  // ignore empty lines / lone \r before \n
            buffer[bufferLen] = '\0';
            // On a '\r' terminator, swallow a paired '\n' BEFORE dispatch, so a
            // CRLF host's '\n' is never consumed as the first payload byte by a
            // fwdata/lapply read (nor left to desync the next command line).
            if (c == '\r') swallowPairedLf();
            dispatch(buffer);
            bufferLen = 0;
            continue;
        }
        if (bufferLen + 1 >= kBufferSize) {
            // No room for char + null terminator. Mark overflow; drain until newline.
            overflow = true;
            continue;
        }
        buffer[bufferLen++] = c;
    }
}

void SerialCli::dispatch(const char* line) {
    const char* arg = nullptr;
    if (ieq(line, "version")) { cmdVersion(); return; }
    if (ieq(line, "info"))    { cmdInfo();    return; }
    if (ieq(line, "help"))    { cmdHelp();    return; }

    // Sync-transport family (always compiled).
    if (ieq(line, "fwcommit")) { cmdFwcommit(); return; }
    if (ieq(line, "fwabort"))  { cmdFwabort();  return; }
    if (ieq(line, "lget"))     { cmdLget();     return; }
    if (ieq(line, "syncinfo")) { cmdSyncinfo(); return; }
    if (ieq(line, "menutree")) { MenuManager::instance().dumpTree(); return; }
    if (verbWithArg(line, "fwrite", &arg))  { cmdFwrite(arg);  return; }
    if (verbWithArg(line, "fwdata", &arg))  { cmdFwdata(arg);  return; }
    if (verbWithArg(line, "fdelete", &arg)) { cmdFdelete(arg); return; }
    if (verbWithArg(line, "lapply", &arg))  { cmdLapply(arg);  return; }
#ifdef CF_TEST_CLI
    if (ieq(line, "apps")) { cmdApps(); return; }
    if (ieq(line, "app"))  { cmdApp();  return; }
    if (ieq(line, "net"))  { cmdNet();  return; }
    if (ieq(line, "mic"))  { cmdMic();  return; }
    if (verbWithArg(line, "launch", &arg)) { cmdLaunch(arg); return; }
    if (verbWithArg(line, "wifi", &arg))   { cmdWifi(arg);   return; }
    if (ieq(line, "wasmstat")) { WasmFsApp::statCli(); return; }
    if (verbWithArg(line, "btn", &arg))    { cmdBtn(arg);    return; }
#endif
    Serial.printf("[err] unknown command: %s\n", line);
}

#ifdef CF_TEST_CLI
// Serial button injection (T-191 leg 3; spike ButtonManager::injectEvent).
// Drives a real app's ButtonManager events without touching GPIO, so a
// bench or a remote human can navigate the menu and play an app over
// serial. `tap` auto-releases after a short delay, polled in poll().
void SerialCli::cmdBtn(const char* args) {
    char* rest = nullptr;
    long idx = strtol(args, &rest, 10);
    if (rest == args) { Serial.println("[err] btn: usage: btn <index> <press|release|tap>"); return; }
    while (*rest == ' ') rest++;
    int numButtons = HAL::buttonManager().getNumButtons();
    if (idx < 0 || idx >= numButtons || idx >= kMaxInjectButtons) {
        Serial.printf("[err] btn: index %ld out of range (0..%d)\n", idx, numButtons - 1);
        return;
    }
    if (ieq(rest, "press")) {
        tapReleaseDueMs[idx] = 0;
        HAL::buttonManager().injectEvent((int)idx, ButtonEvent_Pressed);
        Serial.printf("[cmd] btn.press=%ld\n", idx);
    } else if (ieq(rest, "release")) {
        tapReleaseDueMs[idx] = 0;
        HAL::buttonManager().injectEvent((int)idx, ButtonEvent_Released);
        Serial.printf("[cmd] btn.release=%ld\n", idx);
    } else if (ieq(rest, "tap")) {
        HAL::buttonManager().injectEvent((int)idx, ButtonEvent_Pressed);
        unsigned long due = millis() + kTapReleaseMs;
        tapReleaseDueMs[idx] = (due == 0) ? 1 : due;
        Serial.printf("[cmd] btn.tap=%ld release_in_ms=%lu\n", idx, kTapReleaseMs);
    } else {
        Serial.println("[err] btn: usage: btn <index> <press|release|tap>");
    }
}

void SerialCli::pollPendingTapReleases() {
    unsigned long now = millis();
    for (int i = 0; i < kMaxInjectButtons; i++) {
        if (tapReleaseDueMs[i] != 0 && (long)(now - tapReleaseDueMs[i]) >= 0) {
            tapReleaseDueMs[i] = 0;
            HAL::buttonManager().injectEvent(i, ButtonEvent_Released);
            Serial.printf("[cmd] btn.release=%d\n", i);
        }
    }
}
#endif

void SerialCli::cmdVersion() {
    Serial.printf("[cmd] version=%s\n", getFirmwareVersionString());
}

void SerialCli::cmdInfo() {
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[cmd] info.fw=%s\n",      getFirmwareVersionString());
    Serial.printf("[cmd] info.type=%s\n",    getFirmwareBuildType());
    Serial.printf("[cmd] info.built=%s\n",   getFirmwareBuildTimestamp());
    Serial.printf("[cmd] info.git=%s\n",     getFirmwareGitHash());
    Serial.printf("[cmd] info.dirty=%d\n",   FW_GIT_DIRTY);
    Serial.printf("[cmd] info.chip=%s rev %d\n",
                  ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[cmd] info.mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  static_cast<uint8_t>((mac >> 40) & 0xFF),
                  static_cast<uint8_t>((mac >> 32) & 0xFF),
                  static_cast<uint8_t>((mac >> 24) & 0xFF),
                  static_cast<uint8_t>((mac >> 16) & 0xFF),
                  static_cast<uint8_t>((mac >>  8) & 0xFF),
                  static_cast<uint8_t>((mac >>  0) & 0xFF));
    Serial.printf("[cmd] info.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
}

void SerialCli::cmdHelp() {
    Serial.println("[cmd] help=version,info,help,fwrite <path> <size> <crc>,"
                   "fwdata <off> <len> <crc>,fwcommit,fwabort,fdelete <path>,"
                   "lget,lapply <len> <crc>,syncinfo");
#ifdef CF_TEST_CLI
    Serial.println("[cmd] help.test=apps,launch <name|index>,app,net,mic,wifi <ssid>|<pass>");
#endif
}

// =========================================================================
// Sync-transport verbs (always compiled). The browser drives these over USB
// to install app/asset blobs and edit the loadout manifest. Pure framing,
// checksum, and confinement logic lives in SyncProtocol (native-tested); the
// UART + LittleFS glue lives here. Every reply keeps the stable [cmd]/[err]
// prefixes so the browser side can parse without regex acrobatics.
// =========================================================================

void SerialCli::cmdFwrite(const char* args) {
    char path[SyncProtocol::kMaxPathLen + 1];
    uint32_t size = 0, crc = 0;
    if (!SyncProtocol::parseWriteOpen(args, path, sizeof(path), size, crc)) {
        Serial.println("[err] fwrite.usage=fwrite <path> <size> <crc32>");
        return;
    }
    if (!SyncProtocol::pathConfined(path)) {
        Serial.printf("[err] fwrite.path=%s (confined to /apps/ or /assets/)\n", path);
        return;
    }
    if (!LoadoutStore::begin()) {
        Serial.println("[err] fwrite.fs=mount failed");
        return;
    }
    // Free-space guard (approximate — usedBytes includes an old copy if this
    // overwrites, so this only rejects clearly-too-large transfers).
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t freeB = (total > used) ? (total - used) : 0;
    if ((size_t)size > freeB) {
        Serial.printf("[err] fwrite.space=need %u free %u\n",
                      (unsigned)size, (unsigned)freeB);
        return;
    }

    // Abort any stale session, then open a fresh temp file.
    if (g_writeActive) {
        if (g_writeTemp[0]) LittleFS.remove(g_writeTemp);
        clearWriteSession();
    }
    strncpy(g_writePath, path, sizeof(g_writePath) - 1);
    g_writePath[sizeof(g_writePath) - 1] = '\0';
    snprintf(g_writeTemp, sizeof(g_writeTemp), "%s.part", g_writePath);

    // Nested confined paths (/apps/sub/x.dat) pass confinement but LittleFS
    // won't create the intermediate dirs on open — do it first so the open
    // succeeds. Spot-check nested writes on hardware.
    ensureParentDirs(g_writeTemp);

    g_writeFile = LittleFS.open(g_writeTemp, FILE_WRITE);
    if (!g_writeFile) {
        Serial.printf("[err] fwrite.open=%s\n", g_writeTemp);
        clearWriteSession();
        return;
    }
    g_writeActive = true;
    g_writeSize   = size;
    g_writeCrc    = crc;
    Serial.printf("[cmd] fwrite.ok=%s size=%u chunk=%u crc=%08x\n",
                  g_writePath, (unsigned)size,
                  (unsigned)SyncProtocol::kMaxChunkBytes, (unsigned)crc);
}

void SerialCli::cmdFwdata(const char* args) {
    uint32_t offset = 0, len = 0, crc = 0;
    if (!SyncProtocol::parseChunkHeader(args, offset, len, crc)) {
        // Length unknown -> cannot resync the stream; the browser must abort.
        Serial.println("[err] fwdata.usage=fwdata <offset> <len> <crc32>");
        return;
    }
    if (len == 0) {
        Serial.println("[err] fwdata.len=0");
        return;
    }
    if (len > SyncProtocol::kMaxChunkBytes) {
        drainBytes(len, kPayloadGapMs);
        Serial.printf("[err] fwdata.toobig=%u max=%u\n",
                      (unsigned)len, (unsigned)SyncProtocol::kMaxChunkBytes);
        return;
    }
    if (!g_writeActive) {
        drainBytes(len, kPayloadGapMs);
        Serial.println("[err] fwdata.nosession");
        return;
    }
    if ((uint64_t)offset + len > g_writeSize) {
        drainBytes(len, kPayloadGapMs);
        Serial.printf("[err] fwdata.range=off %u len %u size %u\n",
                      (unsigned)offset, (unsigned)len, (unsigned)g_writeSize);
        return;
    }
    if (!readExact(g_payloadBuf, len, kPayloadGapMs)) {
        if (g_writeTemp[0]) LittleFS.remove(g_writeTemp);
        clearWriteSession();
        Serial.println("[err] fwdata.timeout");
        return;
    }
    // Per-chunk integrity: a corrupt chunk is NAKed and never written, so the
    // browser resends the same offset. The whole-file crc at commit is the
    // final gate against a silently-missed chunk.
    uint32_t got = SyncProtocol::crc32(g_payloadBuf, len);
    if (got != crc) {
        Serial.printf("[err] fwdata.crc=off %u got %08x want %08x\n",
                      (unsigned)offset, (unsigned)got, (unsigned)crc);
        return;
    }
    if (!g_writeFile.seek(offset, SeekSet)) {
        Serial.printf("[err] fwdata.seek=%u\n", (unsigned)offset);
        return;
    }
    size_t wrote = g_writeFile.write(g_payloadBuf, len);
    if (wrote != len) {
        if (g_writeTemp[0]) LittleFS.remove(g_writeTemp);
        clearWriteSession();
        Serial.printf("[err] fwdata.write=%u/%u\n", (unsigned)wrote, (unsigned)len);
        return;
    }
    Serial.printf("[cmd] fwdata.ok=off %u len %u\n", (unsigned)offset, (unsigned)len);
}

void SerialCli::cmdFwcommit() {
    if (!g_writeActive) {
        Serial.println("[err] fwcommit.nosession");
        return;
    }
    g_writeFile.flush();
    g_writeFile.close();

    // Re-read the finished temp file and recompute the whole-file crc, so a
    // dropped or out-of-order chunk (any transfer that doesn't reproduce the
    // browser's bytes exactly) is rejected here rather than half-applied.
    File rf = LittleFS.open(g_writeTemp, FILE_READ);
    if (!rf) {
        LittleFS.remove(g_writeTemp);
        clearWriteSession();
        Serial.println("[err] fwcommit.reopen");
        return;
    }
    uint32_t fileSize = (uint32_t)rf.size();
    uint32_t crc = SyncProtocol::crc32Begin();
    while (true) {
        int n = rf.read(g_payloadBuf, sizeof(g_payloadBuf));
        if (n <= 0) break;
        crc = SyncProtocol::crc32Update(crc, g_payloadBuf, (size_t)n);
    }
    crc = SyncProtocol::crc32Finish(crc);
    rf.close();

    if (fileSize != g_writeSize) {
        LittleFS.remove(g_writeTemp);
        Serial.printf("[err] fwcommit.size=got %u want %u\n",
                      (unsigned)fileSize, (unsigned)g_writeSize);
        clearWriteSession();
        return;
    }
    if (crc != g_writeCrc) {
        LittleFS.remove(g_writeTemp);
        Serial.printf("[err] fwcommit.crc=got %08x want %08x\n",
                      (unsigned)crc, (unsigned)g_writeCrc);
        clearWriteSession();
        return;
    }

    // Atomic-ish publish: rename temp over the final path. littlefs renames
    // atomically; keep the remove+retry fallback the loadout store uses in
    // case the VFS refuses an overwrite. A power cut here leaves the old file
    // or none — never a half-written blob.
    bool ok = LittleFS.rename(g_writeTemp, g_writePath);
    if (!ok) {
        LittleFS.remove(g_writePath);
        ok = LittleFS.rename(g_writeTemp, g_writePath);
    }
    if (!ok) {
        LittleFS.remove(g_writeTemp);
        Serial.printf("[err] fwcommit.rename=%s\n", g_writePath);
        clearWriteSession();
        return;
    }
    Serial.printf("[cmd] fwcommit.ok=%s size=%u crc=%08x\n",
                  g_writePath, (unsigned)fileSize, (unsigned)crc);
    clearWriteSession();
}

void SerialCli::cmdFwabort() {
    if (g_writeActive && g_writeTemp[0]) LittleFS.remove(g_writeTemp);
    clearWriteSession();
    Serial.println("[cmd] fwabort.ok");
}

void SerialCli::cmdFdelete(const char* args) {
    char path[SyncProtocol::kMaxPathLen + 1];
    if (!SyncProtocol::parsePathArg(args, path, sizeof(path))) {
        Serial.println("[err] fdelete.usage=fdelete <path>");
        return;
    }
    if (!SyncProtocol::pathConfined(path)) {
        Serial.printf("[err] fdelete.path=%s (confined to /apps/ or /assets/)\n", path);
        return;
    }
    if (!LoadoutStore::begin()) {
        Serial.println("[err] fdelete.fs=mount failed");
        return;
    }
    if (!LittleFS.exists(path)) {
        Serial.printf("[err] fdelete.absent=%s\n", path);
        return;
    }
    if (!LittleFS.remove(path)) {
        Serial.printf("[err] fdelete.fail=%s\n", path);
        return;
    }
    Serial.printf("[cmd] fdelete.ok=%s\n", path);
}

void SerialCli::cmdLget() {
    LoadoutStore::begin();
    std::string json;
    LoadoutManifest::Loadout lo;
    bool present = loadLoadoutManifest(lo, &json);
    if (!present) {
        Serial.println("[cmd] lget.present=0 entries=0 schema=0 len=0 crc=00000000");
        return;
    }
    // Parse only to report entry count + schema alongside the raw bytes; the
    // browser gets the authoritative document either way.
    int entries = 0, schema = 0;
    entries = (int)lo.entries.size();
    schema  = lo.schemaVersion;
    uint32_t crc = SyncProtocol::crc32(json.data(), json.size());
    Serial.printf("[cmd] lget.present=1 entries=%d schema=%d len=%u crc=%08x\n",
                  entries, schema, (unsigned)json.size(), (unsigned)crc);
    Serial.write((const uint8_t*)json.data(), json.size());
}

void SerialCli::cmdLapply(const char* args) {
    uint32_t len = 0, crc = 0;
    if (!SyncProtocol::parseApplyHeader(args, len, crc)) {
        Serial.println("[err] lapply.usage=lapply <len> <crc32>");
        return;
    }
    if (len == 0) {
        Serial.println("[err] lapply.len=0");
        return;
    }
    if (len > SyncProtocol::kMaxApplyBytes) {
        drainBytes(len, kPayloadGapMs);
        Serial.printf("[err] lapply.toobig=%u max=%u\n",
                      (unsigned)len, (unsigned)SyncProtocol::kMaxApplyBytes);
        return;
    }
    if (!readExact(g_payloadBuf, len, kPayloadGapMs)) {
        Serial.println("[err] lapply.timeout");
        return;
    }
    uint32_t got = SyncProtocol::crc32(g_payloadBuf, len);
    if (got != crc) {
        Serial.printf("[err] lapply.crc=got %08x want %08x\n",
                      (unsigned)got, (unsigned)crc);
        return;
    }
    g_payloadBuf[len] = '\0';
    int entries = 0, applied = 0;
    if (!AppManager::instance().applyLoadoutOps((const char*)g_payloadBuf,
                                                &entries, &applied)) {
        // Malformed document, a rejected op, or a failed save — the stored
        // manifest is untouched, so the menu still falls back cleanly.
        Serial.println("[err] lapply.reject");
        return;
    }
    Serial.printf("[cmd] lapply.ok=applied %d entries %d\n", applied, entries);
}

void SerialCli::cmdSyncinfo() {
    LoadoutStore::begin();
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t freeB = (total > used) ? (total - used) : 0;
    Serial.printf("[cmd] syncinfo.fs_total=%u fs_used=%u fs_free=%u\n",
                  (unsigned)total, (unsigned)used, (unsigned)freeB);

    std::string json;
    int entries = 0, schema = 0, present = 0;
    LoadoutManifest::Loadout lo;
    if (loadLoadoutManifest(lo, &json)) {
        present = 1;
        entries = (int)lo.entries.size();
        schema  = lo.schemaVersion;
    }
    Serial.printf("[cmd] syncinfo.manifest=%d entries=%d schema=%d\n",
                  present, entries, schema);
    Serial.printf("[cmd] syncinfo.fw=%s\n", getFirmwareVersionString());
}

#ifdef CF_TEST_CLI
// =========================================================================
// Test-mode device-control commands (-DCF_TEST_CLI=1, `local_test` env).
// Output keeps the stable [cmd]/[err] line prefixes so a harness can parse
// without regex acrobatics. The flow these exist for: a test agent flashes
// the device, `wifi <ssid>|<pass>` saves LAN credentials, `launch <portal>`
// opens the web portal, `net` reports the IP to point a browser at.
// =========================================================================

void SerialCli::cmdApps() {
    for (int i = 0; i < APP_COUNT; ++i) {
        // The menu has an empty label; report it as "menu" so it stays
        // addressable.
        const char* name = (appDefs[i].name[0] != '\0') ? appDefs[i].name : "menu";
        Serial.printf("[cmd] apps.%d=%s\n", i, name);
    }
}

void SerialCli::cmdLaunch(const char* arg) {
    int target = -1;
    if (arg[0] >= '0' && arg[0] <= '9') {
        target = atoi(arg);
        if (target < 0 || target >= APP_COUNT) target = -1;
    } else if (ieq(arg, "menu")) {
        target = APP_MENU;
    } else {
        for (int i = 0; i < APP_COUNT; ++i) {
            if (ieq(arg, appDefs[i].name)) { target = i; break; }
        }
    }
    // T-183: a ferried wasm app has no builtin AppIndex. Resolve a manifest
    // blob id (or name) to its path, stage it, and launch the WASM_HOST slot
    // - the same path the menu leaf takes, so the bench drives it headlessly.
    if (target < 0) {
        LoadoutManifest::Loadout lo;
        if (loadLoadoutManifest(lo, nullptr)) {
            for (const auto& e : lo.entries) {
                if (e.format == "blob" && !e.blobPath.empty() &&
                    (ieq(arg, e.id.c_str()) || ieq(arg, e.name.c_str()))) {
                    WasmFsApp::setPending(e.blobPath.c_str(),
                                          e.name.empty() ? e.id.c_str() : e.name.c_str());
                    AppManager::instance().switchToApp(APP_WASM_HOST);
                    Serial.printf("[cmd] launch.ok=blob path=%s\n", e.blobPath.c_str());
                    return;
                }
            }
        }
    }
    if (target < 0) {
        Serial.printf("[err] unknown app: %s (try `apps`)\n", arg);
        return;
    }
    AppManager::instance().switchToApp((AppIndex)target);
    Serial.printf("[cmd] launch.ok=%d\n", target);
}

void SerialCli::cmdApp() {
    AppIndex idx = AppManager::instance().activeApp();
    const char* name = (appDefs[idx].name[0] != '\0') ? appDefs[idx].name : "menu";
    Serial.printf("[cmd] app.index=%d\n", (int)idx);
    Serial.printf("[cmd] app.name=%s\n", name);
    Serial.printf("[cmd] app.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
}

void SerialCli::cmdNet() {
    wifi_mode_t mode = WiFi.getMode();
    Serial.printf("[cmd] net.mode=%d\n", (int)mode);
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        Serial.printf("[cmd] net.ap_ip=%s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[cmd] net.ap_clients=%d\n", WiFi.softAPgetStationNum());
    }
    if (mode == WIFI_STA || mode == WIFI_AP_STA) {
        bool up = (WiFi.status() == WL_CONNECTED);
        Serial.printf("[cmd] net.sta_connected=%d\n", up ? 1 : 0);
        if (up) {
            Serial.printf("[cmd] net.sta_ssid=%s\n", WiFi.SSID().c_str());
            Serial.printf("[cmd] net.sta_ip=%s\n", WiFi.localIP().toString().c_str());
        }
    }
}

// Mic pipeline diagnostic: acquire the shared capture service in the
// CURRENT app context (whatever is running — that's the point: it can
// reproduce a context-dependent open failure), read the post-gain peak for
// ~300ms, release. Refuses politely if an app holds the mic.
void SerialCli::cmdMic() {
    MicCapture& mic = MicCapture::instance();
    // Internal-heap picture first: acquire needs ~4KB DMA + a 4KB task
    // stack from INTERNAL ram, so free vs largest-block tells apart
    // "exhausted" from "fragmented" when it fails.
    Serial.printf("[cmd] mic.heap_free=%u largest=%u min_ever=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (mic.acquired()) {
        Serial.printf("[cmd] mic.held_by=%s\n", mic.ownerTag());
        return;
    }
    const char* err = nullptr;
    // The live stream's heap-diet config — this diagnostic exists to prove
    // the portal context can open exactly this.
    if (!mic.acquire("cli", 16000, &err, 4)) {
        Serial.printf("[err] mic acquire failed: %s\n", err ? err : "?");
        return;
    }
    Serial.println("[cmd] mic.acquired=1");
    Serial.printf("[cmd] mic.heap_after=%u largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    mic.clearVuPeak();
    mic.startStreaming(0);
    delay(300);
    uint16_t peak = mic.vuPeakExchange();
    uint32_t avail = mic.ring().available();
    mic.stopStreaming();
    mic.release();
    Serial.printf("[cmd] mic.peak=%u\n", (unsigned)peak);
    Serial.printf("[cmd] mic.ring_bytes_300ms=%lu\n", (unsigned long)avail);
    Serial.println("[cmd] mic.released=1");
}

void SerialCli::cmdWifi(const char* arg) {
    // `wifi <ssid>|<pass>` — '|' separates because SSIDs may contain spaces.
    // An omitted pass ("wifi MyNet|") saves an open network.
    const char* sep = strchr(arg, '|');
    if (sep == nullptr || sep == arg) {
        Serial.println("[err] usage: wifi <ssid>|<pass>");
        return;
    }
    char ssid[33];
    size_t n = (size_t)(sep - arg);
    if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
    memcpy(ssid, arg, n);
    ssid[n] = '\0';
    const char* pass = sep + 1;

    // Same NVS keys the web portal reads at startup (loadWifiCreds), so the
    // next `launch` of the portal auto-connects to this network.
    Preferences prefs;
    if (!prefs.begin("wificfg", false)) {
        Serial.println("[err] wifi store failed");
        return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    Serial.printf("[cmd] wifi.saved=%s\n", ssid);
}
#endif  // CF_TEST_CLI
