// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SerialCli.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <FS.h>
#include <LittleFS.h>

#include "globals.h"  // getFirmwareVersionString() and friends
#include "version.h"  // FW_GIT_DIRTY for the `info` command

#include "AppManager.h"       // applyLoadoutOps (manifest apply + persist)
#include "BatteryDiary.h"
#include "AppDefs.h"          // load + builtin identity migration
#include "LoadoutManifest.h"  // parse for the lget entry count
#include "LoadoutStore.h"     // LittleFS mount + manifest read
#include "MenuManager.h"      // menutree dump (T-183/T-191)
#include "SyncProtocol.h"     // pure crc / confinement / arg parsing
#include "FerrySession.h"     // pure write session (fwrite..fwabort, lapply)
#include "LittleFsFerryStorage.h"
#include "CloudSync.h"
#include "UpdateSession.h"   // upd allow-unsigned / slot (every build), install / fault (test)
#include "DeviceIdentity.h"
#include "FactoryReset.h"

#include "HAL.h"              // displayProxy() for screencap (T-191)
#include "DisplayProxy.h"     // frameBuffer()
#include <mbedtls/base64.h>   // framebuffer -> base64 for screencap

#ifdef CF_TEST_CLI
#include <stdlib.h>          // strtol for `btn` argument parsing
#include "ButtonManager.h"   // injectEvent / ButtonEvent for `btn` (spike port)
#include "HAL.h"             // HAL::buttonManager()
#include "WasmFsApp.h"       // wasmstat (T-183)
#include "WasmHostImports.h"  // kDeviceHalAbi (REQ-063)
#include "UvloLogic.h"
#include "ModalPrompt.h"     // prompt (sample modal for bench screenshots)
#include "StatusView.h"      // status (menu status bar bench states)
#include "UpdatePrompt.h"    // upd (update settings read-back, stand-in offer)
#include "AwakeMode.h"       // awake (Awake & dev mode bench verbs)
#endif

#ifdef CF_TEST_CLI
#include <Preferences.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <mbedtls/platform.h>
#include <esp_task_wdt.h>
#include <esp_bt.h>
#include <esp_bt_main.h>

#include <esp_heap_caps.h>

#include "AppDefs.h"
#include "AppManager.h"
#include "CheckinScheduler.h"
#include "CheckinPolicy.h"
#include "MicCapture.h"
#include "TlsProbeSession.h"
#include "SavedWifi.h"
#include "TrustedRoots.h"
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

// Case-insensitive prefix match; returns the remainder of `line` after the
// prefix, or nullptr if it does not match. Used by the T-191 stream verb.
const char* ieqPrefix(const char* line, const char* prefix) {
    while (*prefix) {
        char ca = *line++;
        char cb = *prefix++;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return nullptr;
    }
    return line;
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

bool twoArgsEqual(const char* args, const char* first, const char* second) {
    const char* rest = nullptr;
    return verbWithArg(args, first, &rest) && ieq(rest, second);
}

// =========================================================================
// Sync-transport glue (device-only; SerialCli never compiles for the native
// tests). The write session itself - `fwrite`/`fwdata`/`fwcommit`/`fwabort`
// state, policy, and reply text, plus `lapply` - lives in the pure
// SyncProtocol::FerrySession. This file supplies the UART byte source and
// prints replies verbatim; LittleFsFerryStorage is shared with the WiFi driver.
// =========================================================================

// Read-verb payload buffer. FerrySession's shared LittleFS adapter owns its
// separate buffer while a write is active; these reads release theirs after
// each command.
using SyncProtocol::kPayloadBufBytes;
uint8_t* g_payloadBuf = nullptr;

bool allocatePayloadBuffer() {
    if (g_payloadBuf != nullptr) return true;
    g_payloadBuf = static_cast<uint8_t*>(heap_caps_malloc(
        kPayloadBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_payloadBuf == nullptr) {
        g_payloadBuf = static_cast<uint8_t*>(malloc(kPayloadBufBytes));
    }
    return g_payloadBuf != nullptr;
}

void freePayloadBuffer() {
    if (g_payloadBuf != nullptr) {
        free(g_payloadBuf);
        g_payloadBuf = nullptr;
    }
}

// UART timeout ownership: the payload read bounds below belong to this serial
// driver, not to FerrySession. The session asks its FerryByteSource for
// "exactly n bytes" or "drain n bytes" and only learns success or give-up;
// UartByteSource (below) turns that into these gap/total-duration limits.
// Another transport supplies its own byte source with its own timing.
//
// Payload read bounds. A framed payload read runs inside SerialCli::poll(),
// which runs inside AppManager::loop(), so an unbounded read hands a hostile
// (or merely wedged) host a lever to freeze the whole device. Two independent
// bounds close that off:
//   * Inter-byte gap: at 921600 baud a byte is ~11us on the wire, so a full
//     second of silence mid-payload already means the transfer has died -
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
// payload (fwdata/lapply) - which would fail the chunk CRC AND leave a stray
// byte that corrupts the next command line - nor be seen as a spurious empty
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

// Payload bytes straight off the UART, bounded by the serial driver's own
// gap/total-duration limits (see "UART timeout ownership" above).
class UartByteSource : public SyncProtocol::FerryByteSource {
public:
    bool readExact(uint8_t* buf, size_t n) override {
        return ::readExact(buf, n, kPayloadGapMs);
    }
    void drain(size_t n) override { drainBytes(n, kPayloadGapMs); }
};

// The serial and WiFi drivers use the same LittleFS ferry adapter.
SyncProtocol::LittleFsFerryStorage g_ferryStorage;
SyncProtocol::FerrySession g_ferry(g_ferryStorage);

// Read verbs use a separate buffer from the ferry and release it afterwards.
void releaseReadPayload() {
    freePayloadBuffer();
}

void sendReply(const SyncProtocol::FerryReply& reply) {
    Serial.write((const uint8_t*)reply.text, reply.len);
}

#ifdef CF_TEST_CLI
constexpr uint32_t kTlsProbeBudgetMs = 20000;
constexpr uint32_t kTlsJoinBudgetMs = 10000;
constexpr uint32_t kTlsCallMaxMs = 4000;  // below the configured 5 s task watchdog
constexpr uint32_t kTlsStackBytes = 8192;
constexpr char kTlsDefaultUrl[] = "https://cyberfidget.com/update/firmware.php?list=1";

TlsProbeSession g_tlsSession;
char g_tlsUrl[SerialCli::kBufferSize] = {0};
char g_tlsSsid[33] = {0};
char g_tlsPass[65] = {0};  // 64-char hex key + terminator

struct TlsProbeResult {
    const char* err = "none";
    uint32_t joinMs = 0;
    uint32_t tlsMs = 0;
    uint32_t getMs = 0;
    int http = 0;
    uint32_t bytes = 0;
    size_t heapFreeMin = SIZE_MAX;
    size_t largestMin = SIZE_MAX;
    size_t heapMinBefore = 0;  // since-boot low-water before the probe ran
    size_t heapMinBoot = 0;
    uint32_t stackHw = 0;
};
TlsProbeResult g_tlsResult;

void sampleTlsHeap(TlsProbeResult& result) {
    size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (freeBytes < result.heapFreeMin) result.heapFreeMin = freeBytes;
    if (largest < result.largestMin) result.largestMin = largest;
}

bool tlsCallReady(esp_http_client_handle_t client, uint32_t start) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= kTlsProbeBudgetMs) return false;
    uint32_t remaining = kTlsProbeBudgetMs - elapsed;
    int timeout = (int)(remaining < kTlsCallMaxMs ? remaining : kTlsCallMaxMs);
    if (timeout < 1) timeout = 1;
    esp_task_wdt_reset();
    return esp_http_client_set_timeout_ms(client, timeout) == ESP_OK;
}

void tlsProbeTask(void*) {
    TlsProbeResult result;
    TlsProbeSession::State state = TlsProbeSession::State::Failed;
    uint32_t start = millis();
    esp_http_client_handle_t client = nullptr;
    char* roots = nullptr;  // trusted roots as PEM; must outlive the client
    bool watched = (esp_task_wdt_add(nullptr) == ESP_OK);
    // The handshake trough falls inside esp_http_client_open() where no sample
    // can run; a drop in the since-boot low-water mark attributes it to us.
    result.heapMinBefore = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    sampleTlsHeap(result);  // before STA join

    do {
        if (!watched) { result.err = "watchdog"; break; }
        WiFi.persistent(false);
        if (!WiFi.mode(WIFI_STA)) { result.err = "sta-mode"; break; }
        uint32_t joinStart = millis();
        WiFi.begin(g_tlsSsid, g_tlsPass);
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - joinStart >= kTlsJoinBudgetMs ||
                millis() - start >= kTlsProbeBudgetMs) {
                state = TlsProbeSession::State::Timeout;
                result.err = "join-timeout";
                break;
            }
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        result.joinMs = millis() - joinStart;
        sampleTlsHeap(result);  // after join attempt
        if (state == TlsProbeSession::State::Timeout) break;

        roots = TrustedRoots::newPem();
        if (!roots) { result.err = "roots-alloc"; break; }
        // A partial parse would trust fewer roots: never connect on one.
        if (!TrustedRoots::pemParsesCompletely(roots)) { result.err = "roots-parse"; break; }
        esp_http_client_config_t config = {};
        config.url = g_tlsUrl;
        config.method = HTTP_METHOD_GET;
        config.cert_pem = roots;
        config.disable_auto_redirect = true;
        config.timeout_ms = (int)kTlsCallMaxMs;
        client = esp_http_client_init(&config);
        if (!client) { result.err = "http-init"; break; }
        uint32_t getStart = millis();
        if (!tlsCallReady(client, start)) {
            state = TlsProbeSession::State::Timeout;
            result.err = "budget";
            break;
        }
        uint32_t tlsStart = millis();
        esp_err_t openErr = esp_http_client_open(client, 0);
        result.tlsMs = millis() - tlsStart;  // DNS, TCP, TLS, and request headers
        if (openErr != ESP_OK) {
            result.err = "tls-connect";
            break;
        }
        sampleTlsHeap(result);  // after verified handshake
        if (!tlsCallReady(client, start)) {
            state = TlsProbeSession::State::Timeout;
            result.err = "budget";
            break;
        }
        int64_t length = esp_http_client_fetch_headers(client);
        if (length < 0) { result.err = "headers"; break; }
        result.http = esp_http_client_get_status_code(client);
        char body[256];
        for (;;) {
            if (!tlsCallReady(client, start)) {
                state = TlsProbeSession::State::Timeout;
                result.err = "budget";
                break;
            }
            int n = esp_http_client_read(client, body, sizeof(body));
            if (n < 0) { result.err = "body"; break; }
            if (n == 0) {
                if (!esp_http_client_is_complete_data_received(client)) result.err = "body-short";
                break;
            }
            result.bytes += (uint32_t)n;
        }
        result.getMs = millis() - getStart;
        sampleTlsHeap(result);  // after GET attempt
        if (state == TlsProbeSession::State::Timeout ||
            strcmp(result.err, "none") != 0) break;
        if (result.http != 200) { result.err = "http-status"; break; }
        if (result.bytes == 0) { result.err = "empty-body"; break; }
        state = TlsProbeSession::State::Done;
    } while (false);

    if (client) esp_http_client_cleanup(client);
    TrustedRoots::freePem(roots);
    WiFi.disconnect(true);
    if (!WiFi.mode(WIFI_OFF)) {
        state = TlsProbeSession::State::Failed;
        result.err = "wifi-off";
    }
    if (millis() - start >= kTlsProbeBudgetMs) {
        state = TlsProbeSession::State::Timeout;
        result.err = "budget";
    }
    result.heapMinBoot = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    result.stackHw = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (watched) esp_task_wdt_delete(nullptr);
    memset(g_tlsPass, 0, sizeof(g_tlsPass));
    g_tlsResult = result;
    g_tlsSession.finish(state);
    vTaskDelete(nullptr);
}
#endif
}  // namespace

bool SerialCli::ferryActive() const { return g_ferry.active(); }

void SerialCli::closeStorageForFactoryReset() {
    if (g_ferry.active()) g_ferry.abort();
    releaseReadPayload();
}

bool SerialCli::radioBusy() const {
#ifdef CF_TEST_CLI
    return g_tlsSession.current() != TlsProbeSession::State::Idle;
#else
    return false;
#endif
}

SerialCli& SerialCli::instance() {
    static SerialCli singleton;
    return singleton;
}

void SerialCli::poll() {
    pollScreenStream();
#ifdef CF_TEST_CLI
    pollPendingTapReleases();
    pollTlsprobeResult();
    CloudSync::Result cloudResult;
    if (CloudSync::consumeResult(cloudResult)) {
        Serial.printf("[cmd] cloud.result=%s err=%s applied=%s offered=%s next_ms=%u heap_min=%u\n",
                      cloudResult.ok ? (cloudResult.none ? "none" : "ok") : "error",
                      cloudResult.err, cloudResult.applied, cloudResult.offered,
                      (unsigned)cloudResult.nextMs, (unsigned)cloudResult.heapMin);
    }
    static uint32_t linkGeneration = 0;
    const CloudSync::LinkSnapshot link = CloudSync::linkSnapshot();
    if (link.generation != linkGeneration) {
        linkGeneration = link.generation;
        if (link.state == CloudSync::LinkState::Code)
            Serial.printf("[cmd] link.code=%s\n", link.code);
        else if (link.state == CloudSync::LinkState::Confirm)
            Serial.printf("[cmd] link.state=confirm account=%s\n", link.account);
        else if (link.state == CloudSync::LinkState::ClearApps)
            Serial.println("[cmd] link.state=clear_apps");
        else if (link.state == CloudSync::LinkState::Linked)
            Serial.printf("[cmd] link.state=linked account=%s%s\n", link.account,
                          link.error[0] ? " apps_clear=failed" : "");
        else if (link.state == CloudSync::LinkState::Declined)
            Serial.println("[cmd] link.state=declined");
        else if (link.state == CloudSync::LinkState::Expired)
            Serial.println("[cmd] link.state=expired");
        else if (link.state == CloudSync::LinkState::Error)
            Serial.printf("[cmd] link.state=error reason=%s\n", link.error);
        else if (link.state == CloudSync::LinkState::Idle ||
                 link.state == CloudSync::LinkState::Unlinked)
            Serial.println("[cmd] link.state=unlinked");
    }
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
#ifdef CF_TEST_CLI
    if (ieq(line, "reset factory confirm")) {
        FactoryReset::confirmFromCli();
        return;
    }
    if (ieq(line, "reset factory confirm hold")) {
        FactoryReset::confirmFromCli(true);
        return;
    }
#endif
    if (CloudSync::storeBusy()) {
        // Refuse writes while a network pull owns the store (dev mode
        // listening only while it is inside a check-in). A refused
        // fwdata/lapply still drains its payload first, as FerrySession
        // does on its own early refusals, so the stream stays in frame.
        uint32_t offset = 0, len = 0, crc = 0;
        size_t payload = 0;
        bool refuse = ieq(line, "fwcommit") || ieq(line, "fwabort") ||
                      ieq(line, "lapply") || verbWithArg(line, "fwrite", &arg) ||
                      verbWithArg(line, "fdelete", &arg);
        if (!refuse && verbWithArg(line, "fwdata", &arg)) {
            refuse = true;
            if (SyncProtocol::parseChunkHeader(arg, offset, len, crc)) payload = len;
        } else if (!refuse && verbWithArg(line, "lapply", &arg)) {
            refuse = true;
            if (SyncProtocol::parseApplyHeader(arg, len, crc)) payload = len;
        }
        if (refuse) {
            if (payload > 0) {
                UartByteSource in;
                in.drain(payload);
            }
            Serial.println("[err] sync.busy");
            return;
        }
    }
    if (ieq(line, "version")) { cmdVersion(); return; }
    if (ieq(line, "info"))    { cmdInfo();    return; }
    if (ieq(line, "help"))    { cmdHelp();    return; }
    if (ieq(line, "mark"))    { Serial.println("[err] mark.usage=mark <id>"); return; }
    if (verbWithArg(line, "mark", &arg)) { cmdMark(arg); return; }
    if (ieq(line, "reboot"))  { cmdReboot();  return; }
    if (ieq(line, "battery")) { cmdBattery(); return; }
    if (ieq(line, "diary"))   { cmdDiary(""); return; }
    if (verbWithArg(line, "diary", &arg)) { cmdDiary(arg); return; }

    // Sync-transport family (always compiled).
    if (ieq(line, "fwcommit")) { cmdFwcommit(); return; }
    if (ieq(line, "fwabort"))  { cmdFwabort();  return; }
    if (ieq(line, "lget"))     { cmdLget();     return; }
    if (ieq(line, "syncinfo")) { cmdSyncinfo(); return; }
    if (ieq(line, "menutree")) { MenuManager::instance().dumpTree(); return; }
    if (ieq(line, "screencap")) { cmdScreencap(); return; }
    if (verbWithArg(line, "screenstream", &arg)) { cmdScreenstream(arg); return; }
    if (verbWithArg(line, "fwrite", &arg))  { cmdFwrite(arg);  return; }
    if (verbWithArg(line, "fwdata", &arg))  { cmdFwdata(arg);  return; }
    if (verbWithArg(line, "fdelete", &arg)) { cmdFdelete(arg); return; }
    if (verbWithArg(line, "flist", &arg))   { cmdFlist(arg);   return; }
    if (verbWithArg(line, "fstat", &arg))   { cmdFstat(arg);   return; }
    if (verbWithArg(line, "fread", &arg))   { cmdFread(arg);   return; }
    if (verbWithArg(line, "lapply", &arg))  { cmdLapply(arg);  return; }

    // Installing updates (every build). Allowing unsigned installs is a USB
    // serial command only: holding the cable is the proof, and nothing on
    // the network can reach it.
    if (verbWithArg(line, "upd", &arg)) {
        const char* value = nullptr;
        if (ieq(arg, "slot")) { UpdateSession::printSlots(); return; }
        if (verbWithArg(arg, "allow-unsigned", &value)) {
            const bool on = ieq(value, "on");
            if (!on && !ieq(value, "off")) {
                Serial.println("[err] upd.usage=upd allow-unsigned on|off");
                return;
            }
            Serial.printf("[cmd] upd.unsig_ok=%s\n",
                          UpdateSession::setAllowUnsigned(on) ? (on ? "1" : "0") : "error");
            return;
        }
        // Test builds carry more `upd` verbs below.
    }
#ifdef CF_TEST_CLI
    if (ieq(line, "link start")) {
        if (!CloudSync::startLink()) Serial.println("[cmd] link.state=error reason=busy");
        return;
    }
    if (ieq(line, "link ok")) { CloudSync::answerLink(true); Serial.println("[cmd] link.answer=ok"); return; }
    if (ieq(line, "link no")) { CloudSync::answerLink(false); Serial.println("[cmd] link.answer=no"); return; }
    if (ieq(line, "link clear")) { CloudSync::answerClearApps(true); Serial.println("[cmd] link.answer=clear"); return; }
    if (ieq(line, "link keep")) { CloudSync::answerClearApps(false); Serial.println("[cmd] link.answer=keep"); return; }
    if (ieq(line, "link unlink")) {
        if (!CloudSync::startUnlink()) Serial.println("[cmd] link.state=error reason=busy");
        return;
    }
    if (ieq(line, "link forget")) {
        Serial.printf("[cmd] link.forget=%s\n", CloudSync::forgetLink() ? "ok" : "error");
        return;
    }
    if (ieq(line, "link status")) {
        char account[40];
        bool fingerprint = true;
        const bool has = CloudSync::busy() ? CloudSync::linkStatus(account, fingerprint) :
                         (fingerprint = DeviceIdentity::checkStored(), CloudSync::linked(account));
        Serial.printf("[cmd] link.status=linked:%s account:%s fingerprint:%s previous:%s\n",
                       has ? "yes" : "no", has ? account : "-",
                       fingerprint ? "ok" : "mismatch",
                       CloudSync::hadPreviousAccount() ? "yes" : "no");
        return;
    }
    if (ieq(line, "cloud check")) {
        if (!CheckinScheduler::checkNow())
            Serial.println("[cmd] cloud.result=error err=busy applied=- offered=- next_ms=0 heap_min=0");
        return;
    }
    if (verbWithArg(line, "cloud", &arg)) {
        const char* value = nullptr;
        if (verbWithArg(arg, "base", &value)) {
            Serial.printf("[cmd] cloud.base=%s\n", CloudSync::setBase(value) ? "ok" : "error");
            return;
        }
        if (verbWithArg(arg, "token", &value)) {
            const bool saved = CloudSync::setToken(value);
            memset(const_cast<char*>(value), 0, strlen(value));
            Serial.printf("[cmd] cloud.token=%s\n", saved ? "ok" : "error");
            return;
        }
        if (verbWithArg(arg, "autoapply", &value)) {
            const bool on = ieq(value, "on");
            const bool off = ieq(value, "off");
            Serial.printf("[cmd] cloud.autoapply=%s\n",
                          (on || off) && CloudSync::setAutoapply(on) ? "ok" : "error");
            return;
        }
        // Check-in scheduling bench hooks (see lib/UpdatePolicy/README.md).
        if (verbWithArg(arg, "interval", &value)) {
            Serial.printf("[cmd] cloud.interval=%s\n",
                          CheckinScheduler::setIntervalHours(strtoul(value, nullptr, 10)) ? "ok" : "error");
            return;
        }
        if (verbWithArg(arg, "due", &value)) {
            Serial.printf("[cmd] cloud.due=%s\n",
                          CheckinScheduler::setDueIn(strtoul(value, nullptr, 10)) ? "ok" : "error");
            return;
        }
        if (verbWithArg(arg, "guard", &value)) {
            Serial.printf("[cmd] cloud.guard=%s\n",
                          CheckinScheduler::setGuardMs(strtoul(value, nullptr, 10)) ? "ok" : "error");
            return;
        }
        if (ieq(arg, "press")) {
            Serial.printf("[cmd] cloud.press=%s\n", CheckinScheduler::setPressTest() ? "ok" : "error");
            return;
        }
        if (verbWithArg(arg, "ssid", &value)) {
            const bool absent = ieq(value, "absent");
            const bool saved = ieq(value, "saved");
            Serial.printf("[cmd] cloud.ssid=%s\n",
                          (absent || saved) && CloudSync::setAbsentSsidTest(absent) ? value : "error");
            return;
        }
        if (verbWithArg(arg, "bootcheck", &value)) {
            // "Check at start-up" (Settings > Updates), for the bench.
            const bool on = ieq(value, "on");
            if (!on && !ieq(value, "off")) { Serial.println("[cmd] cloud.bootcheck=error"); return; }
            Preferences upd;
            const bool ok = upd.begin("upd", false) &&
                            upd.putUChar(CheckinPolicy::kKeyBootCheck, on ? 1 : 0) != 0;
            upd.end();
            Serial.printf("[cmd] cloud.bootcheck=%s\n", ok ? (on ? "on" : "off") : "error");
            return;
        }
        if (verbWithArg(arg, "btafterwifi", &value)) {
            const bool allow = ieq(value, "allow");
            if (!allow && !ieq(value, "block")) { Serial.println("[cmd] cloud.btafterwifi=error"); return; }
            AppManager::instance().setTestAllowBtAfterWifi(allow);
            Serial.printf("[cmd] cloud.btafterwifi=%s\n", allow ? "allow" : "block");
            return;
        }
        Serial.println("[cmd] cloud.error=usage");
        return;
    }
    if (ieq(line, "awake")) { AwakeMode::cliCommand(""); return; }
    if (verbWithArg(line, "awake", &arg)) { AwakeMode::cliCommand(arg); return; }
    if (ieq(line, "apps")) { cmdApps(); return; }
    if (ieq(line, "app"))  { cmdApp();  return; }
    if (ieq(line, "net"))  { cmdNet();  return; }
    if (ieq(line, "heapstat")) { cmdHeapstat(); return; }
#ifdef CF_TEST_CLI
    if (ieq(line, "wasm forcerestart on") || ieq(line, "wasm forcerestart off")) {
        // Bench the restart-with-resume fallback for delivered apps.
        const bool on = ieq(line, "wasm forcerestart on");
        WasmFsApp::testForceRestart(on);
        Serial.printf("[cmd] wasm.forcerestart=%s\n", on ? "on" : "off");
        return;
    }
#endif
    if (ieq(line, "heapmap")) {
        // Where the internal heap is split: the stacks of the tasks the
        // network leaves behind, then every internal block (rom printf).
        static const char* const names[] = {"tiT", "sys_evt", "arduino_events", "wifi",
                                            "esp_timer", "cloudsync", "wasm_guest", "loopTask",
                                            "async_tcp", "Tmr Svc", "ipc0", "ipc1"};
        for (const char* n : names) {
            TaskHandle_t h = xTaskGetHandle(n);
            Serial.printf("[cmd] heapmap.task=%s stack=%p\n", n,
                          h ? (void*)pxTaskGetStackStart(h) : nullptr);
        }
        Serial.printf("[cmd] heapmap.largest=%u free=%u\n",
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        Serial.flush();
        heap_caps_dump(MALLOC_CAP_INTERNAL);
        Serial.println("[cmd] heapmap.done=1");
        return;
    }
    if (ieq(line, "btstat")) { cmdBtstat(); return; }
    if (verbWithArg(line, "tlsalloc", &arg)) { cmdTlsalloc(arg); return; }
    if (ieq(line, "tlsprobe")) { cmdTlsprobe(kTlsDefaultUrl); return; }
    if (verbWithArg(line, "tlsprobe", &arg)) { cmdTlsprobe(arg); return; }
    if (ieq(line, "mic"))  { cmdMic();  return; }
    if (ieq(line, "sleep")) { cmdSleep(); return; }
    if (verbWithArg(line, "launch", &arg)) { cmdLaunch(arg); return; }
    if (verbWithArg(line, "soak", &arg))   { cmdSoak(arg); return; }
    if (verbWithArg(line, "wifi", &arg))   { cmdWifi(arg);   return; }
    if (ieq(line, "wasmstat")) { WasmFsApp::statCli(); return; }
    if (verbWithArg(line, "btn", &arg))    { cmdBtn(arg);    return; }
    if (ieq(line, "rail"))                  { cmdRail("");    return; }
    if (verbWithArg(line, "rail", &arg))   { cmdRail(arg);  return; }
    if (ieq(line, "gauge"))                 { cmdGauge("");   return; }
    if (verbWithArg(line, "gauge", &arg))  { cmdGauge(arg); return; }
    if (ieq(line, "uvlo"))                  { cmdUvlo("");    return; }
    if (verbWithArg(line, "uvlo", &arg))   { cmdUvlo(arg);  return; }
    if (ieq(line, "prompt"))                { cmdPrompt("");  return; }
    if (verbWithArg(line, "prompt", &arg)) { cmdPrompt(arg); return; }
    if (ieq(line, "status"))                { cmdStatus("");  return; }
    if (verbWithArg(line, "status", &arg)) { cmdStatus(arg); return; }
    if (ieq(line, "upd"))                   { UpdatePrompt::printState(); return; }
    if (verbWithArg(line, "upd", &arg)) {
        const char* value = nullptr;
        if (verbWithArg(arg, "offer", &value)) { UpdatePrompt::injectOffer(value); return; }
        if (verbWithArg(arg, "install", &value)) {
            // The same hand-off as the prompt's Install now, for one version.
            const char* why = "";
            if (!UpdateSession::armInstall(value, &why)) {
                Serial.printf("[cmd] upd.install=refused reason=%s\n", why);
                return;
            }
            Serial.printf("[cmd] upd.install=restarting version=%s\n", value);
            Serial.flush();
            delay(50);
            ESP.restart();
            return;
        }
        if (ieq(arg, "seen-clear")) {
            const int n = UpdateSession::clearSeen();
            if (n < 0) Serial.println("[cmd] upd.seen_clear=error");
            else Serial.printf("[cmd] upd.seen_clear=%d\n", n);
            return;
        }
        if (ieq(arg, "verify-test")) {
            Serial.printf("[cmd] upd.verify_test=%s\n", UpdateSession::verifyTestFixture() ? "ok" : "fail");
            return;
        }
        if (verbWithArg(arg, "fault", &value)) {
            Serial.printf("[cmd] upd.fault=%s\n", UpdateSession::setTestFault(value) ? value : "error");
            return;
        }
        Serial.println("[err] upd.usage=upd [offer <version> [source] | install <version> | "
                       "fault <none|crash|hang|hal-hang|loop-crash|version|mount|session-hang> | "
                       "seen-clear | verify-test | slot | "
                       "allow-unsigned on|off]");
        return;
    }
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

void SerialCli::cmdSleep() {
    sleepRequested = true;
    Serial.println("[cmd] sleep=requested");
}

bool SerialCli::consumeSleepRequest() {
    if (!sleepRequested) return false;
    sleepRequested = false;
    return true;
}

void SerialCli::cmdRail(const char* args) {
    if (twoArgsEqual(args, "aux", "on")) {
        HAL::setAuxPower(true);
        Serial.println("[cmd] rail.aux=on");
        return;
    }
    if (twoArgsEqual(args, "aux", "off")) {
        HAL::setAuxPower(false);
        Serial.println("[cmd] rail.aux=off");
        return;
    }
    if (twoArgsEqual(args, "oled", "off")) {
        HAL::oledRailOffForBench();
        Serial.println("[cmd] rail.oled=off note=i2c-down-until-reboot");
        return;
    }
    if (twoArgsEqual(args, "oled", "on")) {
        HAL::oledRailOnForBench();
        Serial.println("[cmd] rail.oled=on note=display-reinit-best-effort");
        return;
    }
    Serial.println("[err] rail.usage=rail <oled|aux> <on|off>");
}

void SerialCli::cmdGauge(const char* args) {
    if (twoArgsEqual(args, "hibrt", "force")) {
        HAL::gaugeHibernateForce();
        Serial.printf("[cmd] gauge.hibrt=force hibernating=%d\n",
                      HAL::gaugeIsHibernating() ? 1 : 0);
        return;
    }
    if (twoArgsEqual(args, "hibrt", "auto")) {
        HAL::gaugeHibernateAuto();
        Serial.printf("[cmd] gauge.hibrt=auto hibernating=%d\n",
                      HAL::gaugeIsHibernating() ? 1 : 0);
        return;
    }

    const char* valueArg = nullptr;
    if (verbWithArg(args, "alert-min", &valueArg)) {
        char* end = nullptr;
        const float volts = strtof(valueArg, &end);
        if (end != valueArg && *end == '\0' && volts >= 0.0f && volts <= 5.1f) {
            HAL::gaugeSetAlertMin(volts);
            Serial.printf("[cmd] gauge.alert_min_v=%.2f\n",
                          HAL::gaugeGetAlertMin());
            return;
        }
    }
    Serial.println("[err] gauge.usage=gauge <hibrt force|hibrt auto|alert-min <V>>");
}

void SerialCli::cmdUvlo(const char* args) {
    const char* mvArg = nullptr;
    int32_t mv = 0;
    if (!verbWithArg(args, "simulate", &mvArg) ||
        !UvloLogic::parseMillivolts(mvArg, mv)) {
        Serial.println("[err] uvlo.usage=uvlo simulate <mV>");
        return;
    }

    const bool plausible = mv >= 2000 && mv <= 4600;
    const UvloLogic::SleepDecision sleepDecision =
        UvloLogic::decideSleep(mv, plausible);
    UvloLogic::RuntimeDebounce runtime;
    const uint32_t t0 = 1;
    runtime.feed(mv, t0, plausible);
    const bool runtimeTrip = runtime.feed(
        mv, t0 + (uint32_t)CF_UVLO_RUNTIME_DEBOUNCE_MS, plausible);

    Serial.printf(
        "[cmd] uvlo.simulate=%ld plausible=%d sleep_verdict=%s "
        "runtime_verdict=%s sleep_threshold_mv=%d runtime_threshold_mv=%d "
        "debounce_ms=%d\n",
        (long)mv, plausible ? 1 : 0,
        sleepDecision == UvloLogic::SleepDecision::Shutdown ? "shutdown" : "resleep",
        runtimeTrip ? "shutdown" : "ok", CF_UVLO_SLEEP_THRESHOLD_MV,
        CF_UVLO_RUNTIME_THRESHOLD_MV, CF_UVLO_RUNTIME_DEBOUNCE_MS);
}

// Sample prompt for bench screenshots: opens a ModalPrompt with n options
// (the last one is deliberately long so its row scrolls) and reports the
// answer when it closes. The strings are placeholders, not product copy.
namespace {
constexpr int kPromptMaxOptions = 8;
constexpr uint32_t kPromptMaxTimeoutMs = 3600000;
const char* const kPromptSamples[kPromptMaxOptions] = {
    "Option one", "Option two", "Option three", "Option four",
    "Option five", "Option six", "Option seven", "Option eight",
};
const char kPromptLongSample[] = "A much longer sample option that scrolls";

void onSamplePromptDone(int result) {
    if (result == ModalPrompt::kNoChoice) {
        Serial.println("[cmd] prompt.result=none");
    } else {
        Serial.printf("[cmd] prompt.result=%d\n", result);
    }
}

bool parseDecimal(const char* s, const char** end, uint32_t max, uint32_t* out) {
    if (*s < '0' || *s > '9') return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > max) return false;
        ++s;
    }
    *end = s;
    *out = v;
    return true;
}
}  // namespace

void SerialCli::cmdPrompt(const char* args) {
    uint32_t count = 0, timeoutMs = 0;
    const char* p = args;
    bool ok = parseDecimal(p, &p, kPromptMaxOptions, &count) && count >= 1;
    if (ok) {
        while (*p == ' ') ++p;
        if (*p != '\0') {
            ok = parseDecimal(p, &p, kPromptMaxTimeoutMs, &timeoutMs);
            while (ok && *p == ' ') ++p;
            ok = ok && *p == '\0';
        }
    }
    if (!ok) {
        Serial.println("[err] prompt.usage=prompt <1-8> [timeout_ms]");
        return;
    }

    const char* options[kPromptMaxOptions];
    for (uint32_t i = 0; i < count; ++i) options[i] = kPromptSamples[i];
    options[count - 1] = kPromptLongSample;

    ModalPrompt& prompt = ModalPrompt::instance();
    if (prompt.isOpen()) {
        Serial.println("[err] prompt.busy=1");
        return;
    }
    // Prompts only pause the menu (or the boot screen leading to it).
    if (!prompt.canOpen() ||
        !prompt.open("Sample prompt", options, (int)count,
                     onSamplePromptDone, timeoutMs)) {
        Serial.println("[err] prompt.refused=not-menu");
        return;
    }
    Serial.printf("[cmd] prompt.open=%lu timeout_ms=%lu\n",
                  (unsigned long)count, (unsigned long)timeoutMs);
}

// Menu status bar bench verbs: post / popup / clear every state and read the
// service back, so each bar, badge and Status screen state can be shown and
// screen-captured without any networking. Replies are single lines except
// the read-back, which ends on its status.count line.
namespace {
// Copies the next space-separated word of *p (lowercased) into out and
// advances *p past it and any following spaces.
bool nextWord(const char** p, char* out, size_t len) {
    const char* s = *p;
    while (*s == ' ') ++s;
    size_t n = 0;
    while (s[n] && s[n] != ' ') ++n;
    if (n == 0 || n >= len) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
    s += n;
    while (*s == ' ') ++s;
    *p = s;
    return true;
}

// Parses "<kind> [late] [cached] [text...]".
bool parseStatusEvent(const char* args, StatusKind* kind, uint8_t* flags,
                      const char** text) {
    const char* p = args;
    char word[16];
    if (!nextWord(&p, word, sizeof(word)) ||
        !StatusService::kindFromName(word, kind)) return false;
    *flags = 0;
    for (;;) {
        const char* q = p;
        if (!nextWord(&q, word, sizeof(word))) break;
        if (strcmp(word, "late") == 0)        *flags |= StatusFlag::Late;
        else if (strcmp(word, "cached") == 0) *flags |= StatusFlag::Cached;
        else break;
        p = q;
    }
    *text = p;
    return true;
}

void onStatusPopupResult(StatusKind kind, bool accepted, bool shown) {
    if (!shown) return;  // the open reply already said it was routed
    Serial.printf("[cmd] status.popup.result=%s kind=%s\n",
                  accepted ? "accept" : "ignore", StatusService::kindName(kind));
}

void printStatus() {
    StatusService& svc = StatusService::instance();
    svc.expire((uint32_t)millis());
    const uint32_t now = StatusView::nowSec();
    const StatusEntry* cur = svc.current();
    char age[8];
    svc.ageLabel(now, age, sizeof(age));
    Serial.printf("[cmd] status.bar=%s\n", cur ? cur->text : "-");
    Serial.printf("[cmd] status.badge=%d\n", svc.badge() ? 1 : 0);
    if (svc.hasCheckIn()) {
        Serial.printf("[cmd] status.glyph=%s age=%s checkin_age_s=%lu cached=%d\n",
                      StatusService::glyphName(svc.glyph(now)), age[0] ? age : "-",
                      (unsigned long)svc.checkInAgeSec(now),
                      svc.checkInCached() ? 1 : 0);
    } else {
        Serial.printf("[cmd] status.glyph=%s age=%s checkin_age_s=- cached=0\n",
                      StatusService::glyphName(svc.glyph(now)), age[0] ? age : "-");
    }
    const StatusEntry* list[StatusService::kMaxEntries];
    const int n = svc.pending(list, StatusService::kMaxEntries);
    for (int i = 0; i < n; ++i) {
        const StatusEntry* e = list[i];
        Serial.printf("[cmd] status.item=%s pri=%u sticky=%d attn=%d late=%d "
                      "cached=%d text=%s\n",
                      StatusService::kindName(e->kind), (unsigned)e->priority,
                      e->sticky ? 1 : 0, e->attention ? 1 : 0,
                      e->late() ? 1 : 0, e->cached() ? 1 : 0, e->text);
    }
    // status.count is the reply terminator: new keys go above it.
    Serial.printf("[cmd] status.count=%d\n", n);
}
}  // namespace

void SerialCli::cmdStatus(const char* args) {
    StatusService& svc = StatusService::instance();
    const char* rest = nullptr;
    StatusKind kind = StatusKind::Info;
    uint8_t flags = 0;
    const char* text = "";

    if (*args == '\0') { printStatus(); return; }

    if (verbWithArg(args, "post", &rest)) {
        if (!parseStatusEvent(rest, &kind, &flags, &text)) {
            Serial.println("[err] status.usage=status post <kind> [late] [cached] [text]");
            return;
        }
        if (!svc.post(kind, text, StatusService::defaultPriority(kind), true,
                      (uint32_t)millis(), flags)) {
            Serial.printf("[err] status.post.refused=%s\n", StatusService::kindName(kind));
            return;
        }
        Serial.printf("[cmd] status.post=%s badge=%d\n",
                      StatusService::kindName(kind), svc.badge() ? 1 : 0);
        return;
    }

    if (verbWithArg(args, "popup", &rest)) {
        if (!parseStatusEvent(rest, &kind, &flags, &text)) {
            Serial.println("[err] status.usage=status popup <kind> [late] [cached] [text]");
            return;
        }
        const bool opened = StatusView::popup(kind, text, flags, nullptr,
                                              onStatusPopupResult);
        Serial.printf("[cmd] status.popup=%s open=%d%s\n",
                      StatusService::kindName(kind), opened ? 1 : 0,
                      opened ? "" : " routed=bar");
        return;
    }

    if (ieq(args, "clear")) {
        svc.clearAll();
        Serial.println("[cmd] status.clear=all");
        return;
    }
    if (verbWithArg(args, "clear", &rest)) {
        char word[16];
        const char* p = rest;
        if (!nextWord(&p, word, sizeof(word)) || *p != '\0' ||
            !StatusService::kindFromName(word, &kind)) {
            Serial.println("[err] status.usage=status clear [kind]");
            return;
        }
        const int removed = svc.clear(kind);
        Serial.printf("[cmd] status.clear=%s removed=%d\n",
                      StatusService::kindName(kind), removed);
        return;
    }

    if (verbWithArg(args, "checkin", &rest)) {
        char word[16];
        const char* p = rest;
        uint32_t ago = 0;
        const char* end = nullptr;
        bool ok = nextWord(&p, word, sizeof(word));
        const bool never = ok && strcmp(word, "never") == 0;
        if (ok && !never) {
            ok = parseDecimal(word, &end, StatusView::kClockBaseSec, &ago) && *end == '\0';
        }
        bool cached = false;
        if (ok && *p != '\0') {
            ok = !never && nextWord(&p, word, sizeof(word)) &&
                 strcmp(word, "cached") == 0 && *p == '\0';
            cached = ok;
        }
        if (!ok) {
            Serial.println("[err] status.usage=status checkin <never|seconds_ago> [cached]");
            return;
        }
        if (never) {
            svc.clearCheckIn();
            Serial.println("[cmd] status.checkin=never glyph=never");
            return;
        }
        const uint32_t now = StatusView::nowSec();
        svc.setCheckIn(now - ago, cached);
        Serial.printf("[cmd] status.checkin=%lu cached=%d glyph=%s\n",
                      (unsigned long)ago, cached ? 1 : 0,
                      StatusService::glyphName(svc.glyph(now)));
        return;
    }

    Serial.println("[err] status.usage=status [post|popup|clear|checkin] ...");
}
#endif

void SerialCli::cmdVersion() {
    Serial.printf("[cmd] version=%s\n", getFirmwareVersionString());
}

void SerialCli::cmdMark(const char* arg) {
    Serial.printf("[cmd] mark=%s uptime_ms=%lu\n", arg,
                  static_cast<unsigned long>(millis()));
}

void SerialCli::cmdReboot() {
    Serial.println("[cmd] reboot=now");
    Serial.flush();
    delay(50);
    esp_restart();
}

void SerialCli::cmdBattery() {
    const bool plausible = batteryVoltage >= 2.0f && batteryVoltage <= 4.6f;
    const long batteryMv = plausible
        ? (long)(batteryVoltage * 1000.0f + 0.5f)
        : -1L;
    Serial.printf("[cmd] battery.vcell_mv=%ld soc=%.2f crate=%.2f\n",
                  batteryMv, batteryVoltagePercentage, batteryChangeRate);
}

void SerialCli::cmdDiary(const char* arg) {
    if (arg[0] != '\0') {
        if (!ieq(arg, "clear")) {
            Serial.println("[err] diary.usage=diary [clear]");
            return;
        }
        if (!BatteryDiary::clear()) {
            Serial.println("[err] diary.fs=clear failed");
            return;
        }
        Serial.println("[cmd] diary.clear=ok");
        return;
    }

    BatteryDiary::Stats stats;
    if (!BatteryDiary::getStats(&stats)) {
        Serial.println("[err] diary.fs=unavailable");
        return;
    }
    uint32_t vmin = (stats.min_vcell_mv == UINT32_MAX) ? 0 : stats.min_vcell_mv;
    Serial.printf("[cmd] diary.stats=boot=%lu checkins=%lu on_s=%lu cycles=%lu "
                  "vmin=%lu vmax=%lu written=%lu dropped=%lu\n",
                  (unsigned long)stats.boot_count,
                  (unsigned long)stats.checkin_count,
                  (unsigned long)stats.cum_on_time_s,
                  (unsigned long)stats.charge_cycle_count,
                  (unsigned long)vmin,
                  (unsigned long)stats.max_vcell_mv,
                  (unsigned long)stats.records_written,
                  (unsigned long)stats.records_dropped);

    BatteryDiary::Record records[8];
    uint32_t total = 0;
    size_t count = BatteryDiary::readLastRecords(records, 8, &total);
    for (size_t i = 0; i < count; ++i) {
        const BatteryDiary::Record& record = records[i];
        unsigned socWhole = record.soc_half_pct / 2U;
        unsigned socTenth = (record.soc_half_pct & 1U) ? 5U : 0U;
        int crate = (int)record.crate_qtr_pct_hr;
        const char* sign = (crate < 0) ? "-" : "";
        unsigned crateAbs = (unsigned)((crate < 0) ? -crate : crate);
        Serial.printf("[cmd] diary.rec=%lu ev=%s t=%lu mv=%d soc=%u.%u "
                      "crate=%s%u.%02u\n",
                      (unsigned long)record.seq,
                      BatteryDiary::eventName(record.event),
                      (unsigned long)record.uptime_or_count,
                      (int)record.vcell_mv, socWhole, socTenth, sign,
                      crateAbs / 4U, (crateAbs % 4U) * 25U);
    }
    Serial.printf("[cmd] diary.done=%lu\n", (unsigned long)total);
}

void SerialCli::cmdInfo() {
    uint64_t mac = ESP.getEfuseMac();
    char id[13];
    SyncProtocol::formatDeviceId(mac, id);
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
    Serial.printf("[cmd] info.id=%s\n", id);
    Serial.printf("[cmd] info.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
    const bool batteryPlausible = batteryVoltage >= 2.0f && batteryVoltage <= 4.6f;
    const long batteryMv = batteryPlausible
        ? (long)(batteryVoltage * 1000.0f + 0.5f)
        : -1L;
    Serial.printf("[cmd] info.battery.voltage_mv=%ld\n", batteryMv);
    Serial.printf("[cmd] info.battery.soc=%.2f\n", batteryVoltagePercentage);
    Serial.printf("[cmd] info.battery.crate=%.2f\n", batteryChangeRate);
    const BoardInfo::Info& board = HAL::boardInfo();
    Serial.printf("[cmd] info.board_rev=%u.%u\n",
                  static_cast<unsigned>(board.major),
                  static_cast<unsigned>(board.minor));
    Serial.printf("[cmd] info.board=src=%s hil=%d eng=%d layout=%u\n",
                  BoardInfo::sourceName(board.source),
                  board.hil ? 1 : 0, board.engSample ? 1 : 0,
                  static_cast<unsigned>(board.layoutVersion));
    const DeviceIdentity::Fingerprint identity = DeviceIdentity::readLive();
    Serial.printf("[cmd] info.flash_id=%s\n", identity.flashId[0] ? identity.flashId : "none");
    Serial.printf("[cmd] info.serial=%s\n", identity.serial[0] ? identity.serial : "none");
    // info.wake.cause is the reply terminator: new keys go above it.
    Serial.printf("[cmd] info.wake.cause=%s\n", HAL::bootWakeupCauseName());
}

void SerialCli::cmdHelp() {
    Serial.println("[cmd] help=version,info,help,mark <id>,reboot,battery,menutree,"
                   "screencap,screenstream <off|on [fps]>,diary [clear]");
    Serial.println("[cmd] help.sync=fwrite,fwdata,fwcommit,fwabort,fdelete,flist,"
                   "fstat,fread,lget,lapply,syncinfo");
    Serial.println("[cmd] help.update=upd slot,upd allow-unsigned <on|off>");
#ifdef CF_TEST_CLI
    Serial.println("[cmd] help.test=apps,app,launch <name|index>,net,heapstat,"
                   "tlsprobe [url],tlsalloc <psram|internal>,mic,"
                   "wifi <ssid>|<pass>,wasmstat,btn,sleep,rail,gauge,uvlo,"
                   "soak <app|off>,prompt <n> [timeout_ms],"
                   "status [post|popup|clear|checkin],cloud <base|token|check|autoapply|"
                   "interval|due|guard|press|ssid|btafterwifi>,btstat,"
                   "upd [offer <version> [source]|install <version>|fault <name>]");
#endif
}

// =========================================================================
// Sync-transport verbs (always compiled). The browser drives these over USB
// to install app/asset blobs and edit the loadout manifest. Pure framing,
// checksum, and confinement logic lives in SyncProtocol (native-tested); the
// UART + LittleFS glue lives here. Every reply keeps the stable [cmd]/[err]
// prefixes so the browser side can parse without regex acrobatics.
// =========================================================================

// The write-session verbs are thin: FerrySession owns the state, the checks,
// and the exact reply bytes; this driver only supplies the UART payload bytes
// and prints the reply.
void SerialCli::cmdFwrite(const char* args) {
    sendReply(g_ferry.open(args));
}

void SerialCli::cmdFwdata(const char* args) {
    UartByteSource in;
    sendReply(g_ferry.chunk(args, in));
}

void SerialCli::cmdFwcommit() {
    sendReply(g_ferry.commit());
}

void SerialCli::cmdFwabort() {
    sendReply(g_ferry.abort());
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

void SerialCli::cmdFlist(const char* args) {
    char dir[SyncProtocol::kMaxPathLen + 1];
    char probe[SyncProtocol::kMaxPathLen + 1];
    if (!SyncProtocol::parseListArgs(args, dir, sizeof(dir))) {
        Serial.println("[err] flist.usage=flist <dir>");
        return;
    }
    // Directory arguments omit the trailing slash. A synthetic child makes
    // the existing file-shaped predicate validate the directory unchanged;
    // pathConfined() remains the only root/segment/byte security decision.
    if (!SyncProtocol::makeListConfinementProbe(dir, probe, sizeof(probe)) ||
        !SyncProtocol::pathConfined(probe)) {
        Serial.printf("[err] flist.path=%s (confined to /apps or /assets)\n", dir);
        return;
    }
    if (!LoadoutStore::begin()) {
        Serial.println("[err] flist.fs=mount failed");
        return;
    }
    File directory = LittleFS.open(dir, FILE_READ);
    if (!directory) {
        Serial.printf("[err] flist.absent=%s\n", dir);
        return;
    }
    if (!directory.isDirectory()) {
        directory.close();
        Serial.printf("[err] flist.notdir=%s\n", dir);
        return;
    }

    SyncProtocol::ListProgress progress;
    while (true) {
        File entry = directory.openNextFile();
        if (!entry) break;
        if (!SyncProtocol::admitListEntry(progress)) {
            entry.close();
            break;
        }
        const char* name = entry.name();
        const char* slash = strrchr(name, '/');
        if (slash != nullptr) name = slash + 1;
        Serial.printf("[cmd] flist.entry=%s size=%u\n",
                      name, (unsigned)entry.size());
        entry.close();
    }
    directory.close();

    char summary[SyncProtocol::kReadReplyBytes];
    size_t summaryLen = SyncProtocol::formatListSummary(
        summary, sizeof(summary), dir, progress);
    if (summaryLen == 0) {
        Serial.println("[err] flist.reply");
        return;
    }
    Serial.write((const uint8_t*)summary, summaryLen);
}

void SerialCli::cmdFstat(const char* args) {
    char path[SyncProtocol::kMaxPathLen + 1];
    if (!SyncProtocol::parseStatArgs(args, path, sizeof(path))) {
        Serial.println("[err] fstat.usage=fstat <path>");
        return;
    }
    if (!SyncProtocol::pathConfined(path)) {
        Serial.printf("[err] fstat.path=%s (confined to /apps/ or /assets/)\n", path);
        return;
    }
    if (!LoadoutStore::begin()) {
        Serial.println("[err] fstat.fs=mount failed");
        return;
    }
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        Serial.printf("[err] fstat.absent=%s\n", path);
        return;
    }
    if (file.isDirectory()) {
        file.close();
        Serial.printf("[err] fstat.notfile=%s\n", path);
        return;
    }
    if (!allocatePayloadBuffer()) {
        file.close();
        Serial.println("[err] fstat.nomem");
        return;
    }

    const uint32_t size = (uint32_t)file.size();
    uint32_t remaining = size;
    uint32_t crc = SyncProtocol::crc32Begin();
    while (remaining > 0) {
        const size_t want = remaining > SyncProtocol::kMaxChunkBytes
            ? SyncProtocol::kMaxChunkBytes : (size_t)remaining;
        const int got = file.read(g_payloadBuf, want);
        if (got <= 0) {
            file.close();
            releaseReadPayload();
            Serial.printf("[err] fstat.read=%s\n", path);
            return;
        }
        crc = SyncProtocol::crc32Update(crc, g_payloadBuf, (size_t)got);
        remaining -= (uint32_t)got;
    }
    file.close();
    crc = SyncProtocol::crc32Finish(crc);
    releaseReadPayload();
    Serial.printf("[cmd] fstat.ok=%s size=%u crc=%08x\n",
                  path, (unsigned)size, (unsigned)crc);
}

void SerialCli::cmdFread(const char* args) {
    char path[SyncProtocol::kMaxPathLen + 1];
    uint32_t offset = 0, len = 0;
    if (!SyncProtocol::parseReadArgs(args, path, sizeof(path), offset, len)) {
        Serial.println("[err] fread.usage=fread <path> <offset> <len>");
        return;
    }
    if (!SyncProtocol::pathConfined(path)) {
        Serial.printf("[err] fread.path=%s (confined to /apps/ or /assets/)\n", path);
        return;
    }
    if (len == 0) {
        Serial.println("[err] fread.len=0");
        return;
    }
    if (!SyncProtocol::readLengthAllowed(len)) {
        Serial.printf("[err] fread.toobig=%u max=%u\n",
                      (unsigned)len, (unsigned)SyncProtocol::kMaxChunkBytes);
        return;
    }
    if (!LoadoutStore::begin()) {
        Serial.println("[err] fread.fs=mount failed");
        return;
    }
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        Serial.printf("[err] fread.absent=%s\n", path);
        return;
    }
    if (file.isDirectory()) {
        file.close();
        Serial.printf("[err] fread.notfile=%s\n", path);
        return;
    }
    const uint32_t size = (uint32_t)file.size();
    if ((uint64_t)offset + len > size) {
        file.close();
        Serial.printf("[err] fread.range=off %u len %u size %u\n",
                      (unsigned)offset, (unsigned)len, (unsigned)size);
        return;
    }
    if (!allocatePayloadBuffer()) {
        file.close();
        Serial.println("[err] fread.nomem");
        return;
    }
    if (!file.seek(offset, SeekSet)) {
        file.close();
        releaseReadPayload();
        Serial.printf("[err] fread.seek=%u\n", (unsigned)offset);
        return;
    }
    size_t got = 0;
    while (got < len) {
        const int n = file.read(g_payloadBuf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    file.close();
    if (got != len) {
        releaseReadPayload();
        Serial.printf("[err] fread.read=%u/%u\n", (unsigned)got, (unsigned)len);
        return;
    }

    const uint32_t crc = SyncProtocol::crc32(g_payloadBuf, len);
    char header[SyncProtocol::kReadReplyBytes];
    const size_t headerLen = SyncProtocol::formatReadHeader(
        header, sizeof(header), path, offset, len, crc);
    if (headerLen == 0) {
        releaseReadPayload();
        Serial.println("[err] fread.reply");
        return;
    }
    Serial.write((const uint8_t*)header, headerLen);
    Serial.write(g_payloadBuf, len);
    releaseReadPayload();
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
    UartByteSource in;
    sendReply(g_ferry.applyManifest(args, in));
}

// T-191: base64-encode and emit the 128x64 1bpp framebuffer as one
// `[cmd] screencap=<b64>` line. ~1024 bytes -> ~1368 chars; ~15ms at
// 921600 baud. The copy happens here on the loop task, so the serial
// side never touches the live buffer across contexts.
void SerialCli::emitScreencap() {
    const uint8_t* fb = HAL::displayProxy().frameBuffer();
    if (!fb) { Serial.println("[err] screencap: no framebuffer"); return; }
    uint8_t* b64 = static_cast<uint8_t*>(malloc(1400));
    if (!b64) { Serial.println("[err] screencap: no mem"); return; }
    size_t olen = 0;
    int rc = mbedtls_base64_encode(b64, 1400, &olen, fb,
                                   DisplayProxy::kFrameBufferBytes);
    if (rc != 0) {
        free(b64);
        Serial.println("[err] screencap: encode failed");
        return;
    }
    b64[olen] = '\0';
    // w/h/bpp let the decoder reconstruct without hardcoding geometry.
    Serial.printf("[cmd] screencap w=128 h=64 bpp=1 fmt=colpage len=%u b64=%s\n",
                  (unsigned)olen, (const char*)b64);
    free(b64);
}

void SerialCli::cmdScreencap() { emitScreencap(); }

// T-191: screenstream on [fps] | off. Emits a screencap frame at the
// requested rate from poll(), bounded so it never starves the app loop
// (frames are dropped, ticks are not). Default 8 fps, cap 20.
void SerialCli::cmdScreenstream(const char* arg) {
    while (*arg == ' ') arg++;
    if (ieqPrefix(arg, "off") || arg[0] == '\0') {
        streamIntervalMs = 0;
        Serial.println("[cmd] screenstream=off");
        return;
    }
    const char* rest = ieqPrefix(arg, "on");
    int fps = 8;
    const char* num = rest ? rest : arg;
    while (*num == ' ') num++;
    if (*num >= '0' && *num <= '9') fps = atoi(num);
    if (fps < 1) fps = 1;
    if (fps > 20) fps = 20;  // 20fps * 1.4KB = 28KB/s, ~30% of the link
    streamIntervalMs = 1000UL / (unsigned long)fps;
    lastStreamMs = 0;  // fire immediately
    Serial.printf("[cmd] screenstream=on fps=%d interval_ms=%lu\n", fps, streamIntervalMs);
}

void SerialCli::pollScreenStream() {
    if (streamIntervalMs == 0) return;
    unsigned long now = millis();
    if (lastStreamMs != 0 && (now - lastStreamMs) < streamIntervalMs) return;
    lastStreamMs = now;
    emitScreencap();
}

void SerialCli::cmdSyncinfo() {
    char id[13];
    SyncProtocol::formatDeviceId(ESP.getEfuseMac(), id);
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
    // New syncinfo lines go BEFORE the fw line: deployed readers stop after
    // the fw line and would otherwise take a trailing line as the reply to
    // their next command.
    Serial.printf("[cmd] syncinfo.id=%s\n", id);
    Serial.printf("[cmd] syncinfo.lapply=%s\n", SyncProtocol::kLapplyCapability);
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

bool SerialCli::launchResolved(const char* arg, const char* replyVerb,
                               int* appIndex) {
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
                if (e.format != "builtin" && !e.format.empty() && !e.blobPath.empty() &&
                    (ieq(arg, e.id.c_str()) || ieq(arg, e.name.c_str()))) {
                    int abi = LoadoutManifest::parseAbiVersion(e.abi);
                    WasmFsApp::setPending(e.blobPath.c_str(),
                                          e.name.empty() ? e.id.c_str() : e.name.c_str(), abi,
                                          e.id.c_str());
                    bool abiSupported = WasmFsApp::pendingAbiSupported();
                    AppManager::instance().switchToApp(APP_WASM_HOST);
                    if (!abiSupported) {
                        if (ieq(replyVerb, "launch")) {
                            Serial.printf("[cmd] launch.error=abi_unsupported abi=%d abimax=%d\n",
                                          abi, kDeviceHalAbi);
                        } else {
                            Serial.printf("[err] soak.abi=unsupported abi=%d max=%d\n",
                                          abi, kDeviceHalAbi);
                        }
                        return false;
                    }
                    if (appIndex) *appIndex = APP_WASM_HOST;
                    if (ieq(replyVerb, "launch"))
                        Serial.printf("[cmd] launch.ok=blob path=%s\n", e.blobPath.c_str());
                    else
                        Serial.printf("[cmd] soak=%s\n", arg);
                    return true;
                }
            }
        }
    }
    if (target < 0) {
        if (ieq(replyVerb, "launch"))
            Serial.printf("[err] unknown app: %s (try `apps`)\n", arg);
        else
            Serial.printf("[err] soak.app=%s\n", arg);
        return false;
    }
    AppManager::instance().switchToApp((AppIndex)target);
    if (appIndex) *appIndex = target;
    if (ieq(replyVerb, "launch"))
        Serial.printf("[cmd] launch.ok=%d\n", target);
    else
        Serial.printf("[cmd] soak=%s\n", arg);
    return true;
}

void SerialCli::cmdLaunch(const char* arg) {
    int target = -1;
    launchResolved(arg, "launch", &target);
}

void SerialCli::cmdSoak(const char* arg) {
    if (ieq(arg, "off")) {
        soaking = false;
        Serial.println("[cmd] soak=off");
        return;
    }
    int target = -1;
    if (!launchResolved(arg, "soak", &target)) return;
    soaking = true;
    BatteryDiary::onFlushMarker((uint32_t)target, batteryVoltage,
                                batteryVoltagePercentage, batteryChangeRate);
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

// The pinned SDK allocates every TLS buffer from internal RAM. This routes
// mbedTLS allocations to PSRAM instead (falling back to internal), so the
// probe can measure whether that placement makes a session fit. Switch only
// while no TLS session is open; heap_caps_free releases either region.
static void* tlsPsramCalloc(size_t n, size_t size) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Same placement as the SDK default (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC).
static void* tlsInternalCalloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void SerialCli::cmdTlsalloc(const char* arg) {
    if (g_tlsSession.current() != TlsProbeSession::State::Idle) {
        Serial.println("[cmd] tlsalloc.error=busy");
        return;
    }
    if (ieq(arg, "psram")) {
        mbedtls_platform_set_calloc_free(tlsPsramCalloc, heap_caps_free);
    } else if (ieq(arg, "internal")) {
        mbedtls_platform_set_calloc_free(tlsInternalCalloc, heap_caps_free);
    } else {
        Serial.println("[cmd] tlsalloc.error=usage");
        return;
    }
    Serial.printf("[cmd] tlsalloc.ok=%s psram_free=%u\n", arg,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void SerialCli::cmdHeapstat() {
    Serial.printf("[cmd] heapstat.free_int=%u min_free_int=%u largest_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Bluetooth controller / host state with the internal heap, for the
// WiFi-then-Bluetooth bench.
void SerialCli::cmdBtstat() {
    const esp_bt_controller_status_t ctl = esp_bt_controller_get_status();
    const esp_bluedroid_status_t host = esp_bluedroid_get_status();
    Serial.printf("[cmd] btstat.controller=%s bluedroid=%s wifi_mode=%d free_int=%u "
                  "min_free_int=%u largest_int=%u\n",
                  ctl == ESP_BT_CONTROLLER_STATUS_IDLE ? "idle" :
                  ctl == ESP_BT_CONTROLLER_STATUS_INITED ? "inited" :
                  ctl == ESP_BT_CONTROLLER_STATUS_ENABLED ? "enabled" : "other",
                  host == ESP_BLUEDROID_STATUS_ENABLED ? "enabled" :
                  host == ESP_BLUEDROID_STATUS_INITIALIZED ? "initialized" : "off",
                  (int)WiFi.getMode(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void SerialCli::cmdTlsprobe(const char* url) {
    if (g_tlsSession.current() != TlsProbeSession::State::Idle) {
        Serial.println("[cmd] tlsprobe.error=busy");
        return;
    }
    size_t urlLen = strlen(url);
    if (urlLen >= sizeof(g_tlsUrl) || strncmp(url, "https://", 8) != 0 ||
        url[8] == '\0' || strpbrk(url, " \t\r\n") != nullptr) {
        Serial.println("[cmd] tlsprobe.error=invalid-url");
        return;
    }
    if (WiFi.getMode() != WIFI_OFF || CloudSync::busy()) {
        Serial.println("[cmd] tlsprobe.error=radio-busy");
        return;
    }
    Preferences prefs;
    if (!prefs.begin("wificfg", true)) {
        Serial.println("[cmd] tlsprobe.error=no-credentials");
        return;
    }
    memset(g_tlsSsid, 0, sizeof(g_tlsSsid));
    memset(g_tlsPass, 0, sizeof(g_tlsPass));
    size_t ssidLen = prefs.getString("ssid", g_tlsSsid, sizeof(g_tlsSsid));
    prefs.getString("pass", g_tlsPass, sizeof(g_tlsPass));
    prefs.end();
    if (ssidLen == 0) {
        Serial.println("[cmd] tlsprobe.error=no-credentials");
        return;
    }
    if (!g_tlsSession.start()) {
        Serial.println("[cmd] tlsprobe.error=busy");
        return;
    }
    memcpy(g_tlsUrl, url, urlLen + 1);
    if (xTaskCreate(tlsProbeTask, "tlsprobe", kTlsStackBytes, nullptr, 1, nullptr)
            != pdPASS) {
        memset(g_tlsPass, 0, sizeof(g_tlsPass));
        g_tlsSession.finish(TlsProbeSession::State::Failed);
        TlsProbeSession::State ignored;
        g_tlsSession.consume(ignored);
        Serial.println("[cmd] tlsprobe.error=task-create");
        return;
    }
    Serial.println("[cmd] tlsprobe.started=1");
}

void SerialCli::pollTlsprobeResult() {
    TlsProbeSession::State state;
    if (!g_tlsSession.consume(state)) return;
    const char* label = state == TlsProbeSession::State::Done ? "done" :
                        state == TlsProbeSession::State::Timeout ? "timeout" : "failed";
    Serial.printf("[cmd] tlsprobe.ok=%u state=%s err=%s join_ms=%lu tls_ms=%lu "
                  "get_ms=%lu http=%d bytes=%lu heap_free_min=%u largest_min=%u "
                  "heap_min_before=%u heap_min_boot=%u stack_size=%lu stack_hw=%lu url=%s\n",
                  state == TlsProbeSession::State::Done ? 1u : 0u,
                  label, g_tlsResult.err,
                  (unsigned long)g_tlsResult.joinMs,
                  (unsigned long)g_tlsResult.tlsMs,
                  (unsigned long)g_tlsResult.getMs,
                  g_tlsResult.http, (unsigned long)g_tlsResult.bytes,
                  (unsigned)g_tlsResult.heapFreeMin,
                  (unsigned)g_tlsResult.largestMin,
                  (unsigned)g_tlsResult.heapMinBefore,
                  (unsigned)g_tlsResult.heapMinBoot,
                  (unsigned long)kTlsStackBytes,
                  (unsigned long)g_tlsResult.stackHw, g_tlsUrl);
}

// Mic pipeline diagnostic: acquire the shared capture service in the
// CURRENT app context (whatever is running - that's the point: it can
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
    // The live stream's heap-diet config - this diagnostic exists to prove
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
    // `wifi <ssid>|<pass>` - '|' separates because SSIDs may contain spaces.
    // An omitted pass ("wifi MyNet|") saves an open network. Without a '|'
    // it is one of the saved-network verbs (list, first, forget, hint-bad).
    const char* sep = strchr(arg, '|');
    if (sep == nullptr) {
        SavedWifi::cliCommand(arg);
        return;
    }
    if (sep == arg) {
        Serial.println("[err] usage: wifi <ssid>|<pass>");
        return;
    }
    char ssid[33];
    size_t n = (size_t)(sep - arg);
    if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
    memcpy(ssid, arg, n);
    ssid[n] = '\0';
    const char* pass = sep + 1;

    // Saved as the first network to try, like the portal's Connect (the
    // portal and every session read the same list).
    const WifiList::AddResult r = SavedWifi::add(ssid, pass);
    if (r == WifiList::AddResult::Full) {
        Serial.println("[err] wifi.full=1 (forget one first)");
        return;
    }
    if (r == WifiList::AddResult::Invalid) {
        Serial.println("[err] wifi store failed");
        return;
    }
    Serial.printf("[cmd] wifi.saved=%s\n", ssid);
}
#endif  // CF_TEST_CLI
