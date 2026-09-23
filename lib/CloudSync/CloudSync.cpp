// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "CloudSync.h"
#include "CloudPlanner.h"
#include "CloudProtocol.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_system.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <sys/time.h>
#include <time.h>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <atomic>

#include "AppDefs.h"
#include "AppManager.h"
#include "FerrySession.h"
#include "HAL.h"
#include "LittleFsFerryStorage.h"
#include "LoadoutManifest.h"
#include "LoadoutStore.h"
#include "StatusService.h"
#include "SerialCli.h"
#include "SyncProtocol.h"
#include "TrustedRoots.h"
#include "WasmHostImports.h"
#include "globals.h"

namespace CloudSync {
namespace {
constexpr uint32_t kJoinMs = 10000;
constexpr uint32_t kCallMs = 4000;
constexpr uint32_t kSessionMs = 150000; // includes the server's 60 s check-in floor
constexpr uint32_t kReserveMs = 3 * kCallMs; // one request after a wait
constexpr uint32_t kCancelWaitMs = 2 * kCallMs + 2000;
constexpr uint32_t kStackBytes = 12288;
constexpr size_t kJsonMax = 32768;
constexpr char kDefaultBase[] = "https://cyberfidget.com";

std::atomic<bool> running{false};
std::atomic<bool> finished{false};
std::atomic<bool> radioUsed{false};
bool available = false;
std::atomic<bool> cancelRequested{false};
Result result;

// Internal-heap low-water mark of this session (worker task only).
size_t heapLow = SIZE_MAX;
// The trusted root list failed to parse completely (worker task only).
bool rootsRejected = false;
void sampleHeap() {
    const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeBytes < heapLow) heapLow = freeBytes;
}

void* tlsPsramCalloc(size_t n, size_t size) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void* jsonPsramMalloc(size_t size) { return tlsPsramCalloc(1, size); }

void setError(Result& r, const char* value) {
    strncpy(r.err, value, sizeof(r.err) - 1);
    r.err[sizeof(r.err) - 1] = '\0';
}

bool validBase(const char* url) {
    if (!url || !*url || strlen(url) > 120 || strpbrk(url, " \t\r\n#@")) return false;
    if (strncmp(url, "https://", 8) == 0) return url[8] != '\0';
#ifdef CF_TEST_CLI
    if (strncmp(url, "http://", 7) == 0) return url[7] != '\0';
#endif
    return false;
}

void digestHex(const unsigned char hash[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[64] = '\0';
}

struct HttpReply {
    int status = 0;
    int retry = 0;
    uint32_t nextMs = 0;
    std::string body;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data) return ESP_OK;
    HttpReply* reply = static_cast<HttpReply*>(event->user_data);
    if (strcasecmp(event->header_key, "Retry-After") == 0)
        reply->retry = atoi(event->header_value);
    if (strcasecmp(event->header_key, "X-Next-Poll-Ms") == 0)
        reply->nextMs = (uint32_t)strtoul(event->header_value, nullptr, 10);
    return ESP_OK;
}

using Sink = bool (*)(void*, const uint8_t*, size_t);
bool jsonSink(void* arg, const uint8_t* data, size_t len) {
    HttpReply& reply = *static_cast<HttpReply*>(arg);
    if (reply.body.size() + len > kJsonMax) return false;
    reply.body.append((const char*)data, len);
    return true;
}

// Everything one worker run needs; the credential and WiFi password are
// wiped at exit.
struct Session {
    uint32_t started = 0;
    char token[96] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};
    std::string base;
    std::string query;
    std::string checkinUrl;
    std::string fingerprint;
    char id[13] = {0};
    const char* fw = "";
    char abi[12] = {0};
    char board[16] = {0};
    uint32_t lastCheckinAt = 0;   // millis() when the last check-in finished
    CheckinReply reply;           // the first check-in's answer

    uint32_t elapsed() const { return millis() - started; }
    bool covers(uint32_t waitMs) const {
        return budgetCovers(waitMs, elapsed(), kSessionMs, kReserveMs);
    }
};

// All HTTP calls are bounded by a short per-call timeout and the session
// deadline. The URL must be same-origin and certificate-checked in releases:
// the server's chain must end in the trusted root list (lib/TrustedRoots),
// whose PEM text must outlive the client.
bool request(const Session& s, const std::string& url, const std::string* post,
             HttpReply& reply, Sink sink, void* sinkArg) {
    if (s.elapsed() >= kSessionMs || cancelRequested) return false;
    char* roots = TrustedRoots::newPem();
    if (!roots) return false;
    // A partial parse would trust fewer roots: never connect on one.
    if (!TrustedRoots::pemParsesCompletely(roots)) {
        TrustedRoots::freePem(roots);
        rootsRejected = true;
        return false;
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.cert_pem = roots;
    config.disable_auto_redirect = true;
    config.timeout_ms = kCallMs;
    config.event_handler = onHttpEvent;
    config.user_data = &reply;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { TrustedRoots::freePem(roots); return false; }
    char auth[112];
    snprintf(auth, sizeof(auth), "Bearer %s", s.token);
    esp_http_client_set_header(client, "Authorization", auth);
    memset(auth, 0, sizeof(auth));
    if (post) esp_http_client_set_header(client, "Content-Type", "application/json");
    bool ok = esp_http_client_open(client, post ? post->size() : 0) == ESP_OK;
    sampleHeap();
    if (ok && post) {
        int wrote = esp_http_client_write(client, post->data(), post->size());
        ok = wrote == (int)post->size();
    }
    if (ok) {
        ok = esp_http_client_fetch_headers(client) >= 0;
    }
    if (ok) {
        reply.status = esp_http_client_get_status_code(client);
        if (reply.status == 204) {
            esp_http_client_cleanup(client);
            TrustedRoots::freePem(roots);
            sampleHeap();
            return true;
        }
        if (reply.status >= 400) { sink = jsonSink; sinkArg = &reply; }
        uint8_t buf[1024];
        while (s.elapsed() < kSessionMs && !cancelRequested) {
            int n = esp_http_client_read(client, (char*)buf, sizeof(buf));
            if (n < 0) { ok = false; break; }
            if (n == 0) {
                ok = esp_http_client_is_complete_data_received(client);
                break;
            }
            if (!sink(sinkArg, buf, (size_t)n)) { ok = false; break; }
        }
        if (s.elapsed() >= kSessionMs || cancelRequested) ok = false;
    }
    sampleHeap();
    esp_http_client_cleanup(client);
    TrustedRoots::freePem(roots);
    return ok;
}

// Stores next_poll_ms and applies the reply's effect on the stored backoff
// (see backoffFor: any Retry-After is kept, only a success clears it).
void rememberPoll(uint32_t ms, int status, int retry) {
    Preferences prefs;
    if (!prefs.begin("upd", false)) return;
    if (ms) prefs.putUInt("next_ms", ms);
    switch (backoffFor(status, retry)) {
        case BackoffAction::Store: {
            const time_t now = time(nullptr);
            if (now > 1577836800) prefs.putUInt("backoff_to", (uint32_t)now + retry);
            prefs.putUInt("backoff_ms", (uint32_t)retry * 1000);
            break;
        }
        case BackoffAction::Clear:
            prefs.remove("backoff_to");
            prefs.remove("backoff_ms");
            break;
        case BackoffAction::Keep:
            break;
    }
    prefs.end();
}

// next_poll_ms from an error body (the site sends it to authenticated
// devices), else the header.
uint32_t errorNextMs(const HttpReply& reply) {
    CheckinReply parsed;
    if (!parseCheckin(reply.body.c_str(), reply.nextMs, parsed)) return reply.nextMs;
    return parsed.nextPollMs;
}

// The server's clock when it sent one, else a set local clock.
void recordCheckIn(Result& r, uint32_t serverSec) {
    uint32_t at = serverSec;
    if (at) {
        timeval tv = {(time_t)at, 0};
        settimeofday(&tv, nullptr);
    } else {
        const time_t now = time(nullptr);
        if (now > 1577836800) at = (uint32_t)now;
    }
    if (!at) return;
    r.checkInSec = at;
    Preferences prefs;
    if (prefs.begin("upd", false)) {
        prefs.putUInt("last_chk", at);
        prefs.end();
    }
}

bool readAppliedRecord(LoadoutManifest::AppliedRecord& out) {
    File f = LittleFS.open(SyncProtocol::kAppliedRecordPath, FILE_READ);
    if (!f) return false;
    char buf[256];
    const size_t size = f.size();
    size_t n = size < sizeof(buf) ? f.read((uint8_t*)buf, size) : 0;
    f.close();
    buf[n] = '\0';
    return n > 0 && LoadoutManifest::parseAppliedRecord(buf, out);
}

std::string checkinBody(const Session& s, const char* answerBatch, const char* answer) {
    LoadoutManifest::Loadout loadout;
    std::string manifest;
    bool present = false;
    LoadoutManifest::AppliedRecord applied;
    bool haveApplied = false;
    {
        LoadoutStore::Guard guard;
        LoadoutStore::begin();
        present = loadLoadoutManifest(loadout, &manifest);
        if (!answerBatch) haveApplied = readAppliedRecord(applied);
    }
    CheckinFields fields;
    fields.deviceId = s.id;
    fields.fw = s.fw;
    fields.abi = s.abi;
    fields.board = s.board;
    fields.fsTotal = (uint32_t)LittleFS.totalBytes();
    fields.fsUsed = (uint32_t)LittleFS.usedBytes();
    fields.manifestCrc = present ? SyncProtocol::crc32(manifest.data(), manifest.size()) : 0;
    fields.installed = &loadout;
    if (answerBatch && answer) {
        fields.appliedBatch = answerBatch;
        fields.result = answer;
    } else if (haveApplied) {
        // The last applied record is reported only for the pair identity
        // that applied it (a relinked device starts clean).
        Preferences prefs;
        if (prefs.begin("upd", true)) {
            if (s.fingerprint == prefs.getString("ack_fp", "").c_str()) {
                fields.appliedBatch = applied.batch.c_str();
                fields.result = applied.result.c_str();
            }
            prefs.end();
        }
    }
    return buildCheckinBody(fields);
}

bool postCheckin(Session& s, const std::string& body, HttpReply& reply) {
    reply = HttpReply();
    const bool ok = request(s, s.checkinUrl, &body, reply, jsonSink, &reply);
    s.lastCheckinAt = millis();
    return ok;
}

// Sleeps in watchdog-fed steps; false when cancelled or out of budget.
bool sleepFor(const Session& s, uint32_t ms) {
    const uint32_t since = millis();
    while (millis() - since < ms) {
        if (cancelRequested || s.elapsed() >= kSessionMs) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return true;
}

struct HashSink {
    mbedtls_sha256_context sha;
    uint32_t crc = SyncProtocol::crc32Begin();
    uint32_t bytes = 0;
    uint32_t limit = 0;
    bool overflow = false;
    SyncProtocol::FerrySession* ferry = nullptr;
    bool write = false;
    bool ferryFailed = false;
};
class MemoryBytes : public SyncProtocol::FerryByteSource {
public:
    MemoryBytes(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    bool readExact(uint8_t* out, size_t n) override {
        if (n != len_) return false;
        memcpy(out, data_, n);
        return true;
    }
    void drain(size_t) override {}
private:
    const uint8_t* data_;
    size_t len_;
};
bool blobSink(void* arg, const uint8_t* data, size_t len) {
    HashSink& sink = *static_cast<HashSink*>(arg);
    if (len > sink.limit - sink.bytes) { sink.overflow = true; return false; }
    if (mbedtls_sha256_update(&sink.sha, data, len) != 0) return false;
    sink.crc = SyncProtocol::crc32Update(sink.crc, data, len);
    if (sink.write) {
        MemoryBytes bytes(data, len);
        char header[48];
        snprintf(header, sizeof(header), "%u %u %08x", (unsigned)sink.bytes,
                 (unsigned)len, (unsigned)SyncProtocol::crc32(data, len));
        if (!sink.ferry->chunk(header, bytes).ok) { sink.ferryFailed = true; return false; }
    }
    sink.bytes += len;
    return true;
}
void beginHash(HashSink& sink, bool write, SyncProtocol::FerrySession* ferry,
               uint32_t limit) {
    sink = HashSink();
    sink.write = write;
    sink.ferry = ferry;
    sink.limit = limit;
    mbedtls_sha256_init(&sink.sha);
    mbedtls_sha256_starts(&sink.sha, 0);
}
bool endHash(HashSink& sink, const std::string& expected, uint32_t size,
             uint32_t* crcOut = nullptr) {
    unsigned char hash[32] = {0};
    const bool done = mbedtls_sha256_finish(&sink.sha, hash) == 0;
    mbedtls_sha256_free(&sink.sha);
    char hex[65]; digestHex(hash, hex);
    if (crcOut) *crcOut = SyncProtocol::crc32Finish(sink.crc);
    return done && !sink.overflow && sink.bytes == size && expected == hex;
}

// A target already on disk with the offered size and SHA-256 needs no GET.
bool fileMatches(const std::string& path, const std::string& sha, uint32_t size) {
    File f = LittleFS.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory() || f.size() != size) { if (f) f.close(); return false; }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buf[512];
    uint32_t total = 0;
    for (;;) {
        const int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        mbedtls_sha256_update(&ctx, buf, (size_t)n);
        total += (uint32_t)n;
    }
    f.close();
    unsigned char hash[32] = {0};
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    char hex[65]; digestHex(hash, hex);
    return total == size && sha == hex;
}

// Fetches and commits every blob an offer needs. Transport trouble is
// retryable; bytes that fail the offered hash on two reads, or a document
// that points at a file nothing provides, never will succeed.
BlobVerdict fetchBlobs(const Session& s, const Offer& offer,
                       SyncProtocol::FerrySession& ferry, Result& r,
                       const char*& rejection) {
    for (const BlobOffer& blob : offer.blobs) {
        std::vector<const std::string*> needed;
        for (const std::string& target : blob.targets)
            if (!fileMatches(target, blob.sha256, blob.size)) needed.push_back(&target);
        if (needed.empty()) continue;
        const std::string url = s.base + blob.url + "&" + s.query;
        // FerrySession needs the whole-file CRC at open, so the first GET
        // checks SHA-256 and computes it.
        uint32_t crc = 0;
        bool verified = false;
        for (int attempt = 0; attempt < 2 && !verified; ++attempt) {
            HashSink first; beginHash(first, false, nullptr, blob.size);
            HttpReply reply;
            const bool fetched = request(s, url, nullptr, reply, blobSink, &first);
            verified = endHash(first, blob.sha256, blob.size, &crc);
            if (!first.overflow && (!fetched || reply.status != 200)) {
                if (reply.status >= 400) {
                    r.nextMs = errorNextMs(reply);
                    rememberPoll(r.nextMs, reply.status, reply.retry);
                }
                setError(r, "blob-transport");
                return BlobVerdict::Retryable;
            }
        }
        if (!verified) { rejection = "rejected:blob-sha"; return BlobVerdict::Permanent; }
        // One GET per destination streams into FerrySession's checked
        // chunks; SHA is checked again before commit, so a response that
        // changed between reads never publishes a bad blob.
        for (const std::string* target : needed) {
            char header[128];
            snprintf(header, sizeof(header), "%s %u %08x", target->c_str(),
                     (unsigned)blob.size, (unsigned)crc);
            if (!ferry.open(header).ok) { setError(r, "blob-open"); return BlobVerdict::Retryable; }
            HashSink second; beginHash(second, true, &ferry, blob.size);
            HttpReply reply;
            const bool fetched = request(s, url, nullptr, reply, blobSink, &second);
            const bool match = endHash(second, blob.sha256, blob.size);
            if (!fetched || reply.status != 200 || !match ||
                SyncProtocol::crc32Finish(second.crc) != crc) {
                if (reply.status >= 400) {
                    r.nextMs = errorNextMs(reply);
                    rememberPoll(r.nextMs, reply.status, reply.retry);
                }
                ferry.abort();
                setError(r, second.ferryFailed ? "blob-write" : "blob-transport");
                return BlobVerdict::Retryable;
            }
            if (!ferry.commit().ok) { setError(r, "blob-commit"); return BlobVerdict::Retryable; }
        }
    }
    std::vector<std::string> required;
    LoadoutManifest::collectOpBlobPaths(offer.doc.c_str(), required);
    for (const std::string& path : required) {
        if (!LittleFS.exists(path.c_str())) {
            rejection = "rejected:blob-missing";
            return BlobVerdict::Permanent;
        }
    }
    return BlobVerdict::Ok;
}

// Sends `body` as a check-in, retrying once through a 429 when the budget
// covers its Retry-After. Returns the planner's step after the answer.
Step checkinWithRetry(Session& s, CloudPlanner& plan, const std::string& body,
                      HttpReply& reply, CheckinReply& parsed, bool autoapply,
                      bool followUp, Result& r) {
    for (;;) {
        if (!postCheckin(s, body, reply)) {
            setError(r, followUp ? "ack-transport" : "checkin-transport");
            return Step::Error;
        }
        const bool readable = parseCheckin(reply.status == 200 ? reply.body.c_str() : nullptr,
                                           reply.nextMs, parsed);
        if (reply.status == 200 && !readable) {
            setError(r, followUp ? "ack-body" : "checkin-body");
            return Step::Error;
        }
        if (reply.status >= 400) parsed.nextPollMs = errorNextMs(reply);
        const uint32_t retryMs = reply.retry > 0 ? (uint32_t)reply.retry * 1000 : 0;
        Step step = followUp ? plan.ack(reply.status, parsed.nextPollMs, retryMs)
                             : plan.checkin(reply.status, parsed.hasBatch, parsed.firmwareOffer,
                                            autoapply, parsed.nextPollMs, retryMs);
        if (step != Step::Backoff) {
            rememberPoll(plan.nextMs(), reply.status, reply.retry);
            if (step == Step::Error) setError(r, followUp ? "ack-http" : "checkin-http");
            return step;
        }
        rememberPoll(plan.nextMs(), reply.status, reply.retry);
        const uint32_t wait = retryWaitMs(reply.retry);
        step = plan.retry(wait != 0 && s.covers(wait));
        if (step == Step::Deferred) { setError(r, "rate-limited"); return step; }
        if (!sleepFor(s, wait)) {
            setError(r, cancelRequested ? "cancelled" : "deadline");
            return Step::Error;
        }
    }
}

// The whole session. Every object with heap storage lives in this frame and
// is destroyed on return, before the task deletes itself.
void runWorker(Result& r) {
    CloudPlanner plan;
    plan.start(true);
    Session s;
    s.started = millis();
    heapLow = SIZE_MAX;
    rootsRejected = false;
    const size_t bootLowBefore = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    sampleHeap();
    // Not registered with the task watchdog: a TLS open can block longer
    // than its period. Every wait is bounded by the per-call timeout, the
    // session deadline and cancellation instead.
    bool radioStarted = false;
    do {
        // Set before even joining STA: the first HTTPS handshake must use
        // external RAM, regardless of prior test-mode allocator settings.
        if (mbedtls_platform_set_calloc_free(tlsPsramCalloc, heap_caps_free) != 0) {
            setError(r, "tls-allocator"); break;
        }
        cJSON_Hooks hooks = {};
        hooks.malloc_fn = jsonPsramMalloc;
        hooks.free_fn = heap_caps_free;
        cJSON_InitHooks(&hooks);
        Preferences pair;
        if (!pair.begin("pair", true)) { setError(r, "not-linked"); break; }
        pair.getString("tok", s.token, sizeof(s.token));
        String account = pair.getString("acct", "");
        String linkedAt = pair.getString("at", "");
        if (linkedAt.isEmpty()) linkedAt = String(pair.getUInt("at", 0));
        pair.end();
        if (!s.token[0]) { setError(r, "not-linked"); break; }
        unsigned char hash[32];
        mbedtls_sha256_context identity;
        mbedtls_sha256_init(&identity);
        mbedtls_sha256_starts(&identity, 0);
        mbedtls_sha256_update(&identity, (const unsigned char*)s.token, strlen(s.token));
        mbedtls_sha256_update(&identity, (const unsigned char*)account.c_str(), account.length());
        mbedtls_sha256_update(&identity, (const unsigned char*)linkedAt.c_str(), linkedAt.length());
        mbedtls_sha256_finish(&identity, hash);
        mbedtls_sha256_free(&identity);
        char fp[65]; digestHex(hash, fp);
        s.fingerprint.assign(fp, 16);

        Preferences wifi;
        if (!wifi.begin("wificfg", true)) { setError(r, "no-wifi"); break; }
        wifi.getString("ssid", s.ssid, sizeof(s.ssid));
        wifi.getString("pass", s.pass, sizeof(s.pass));
        wifi.end();
        if (!s.ssid[0]) { setError(r, "no-wifi"); break; }
        s.base = kDefaultBase;
        bool autoapply = true;
        bool backedOff = false;
        Preferences upd;
        if (upd.begin("upd", true)) {
#ifdef CF_TEST_CLI
            // Bench override; release builds always use the compiled site.
            s.base = upd.getString("base", kDefaultBase).c_str();
#endif
            autoapply = upd.getBool("autoapply", true);
            const uint32_t until = upd.getUInt("backoff_to", 0);
            const time_t now = time(nullptr);
            if (until && now > 1577836800 && (uint32_t)now < until) {
                r.nextMs = (until - (uint32_t)now) * 1000;
                backedOff = true;
            }
            upd.end();
        }
        if (backedOff) { setError(r, "backoff"); break; }
        if (!validBase(s.base.c_str())) { setError(r, "invalid-base"); break; }
        if (WiFi.getMode() != WIFI_OFF) { setError(r, "radio-busy"); break; }
        WiFi.persistent(false);
        radioStarted = true;
        radioUsed = true;
        if (!WiFi.mode(WIFI_STA)) { setError(r, "sta-mode"); break; }
        WiFi.begin(s.ssid, s.pass);
        const uint32_t join = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (cancelRequested || millis() - join >= kJoinMs || s.elapsed() >= kSessionMs) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        sampleHeap();
        if (WiFi.status() != WL_CONNECTED) { setError(r, cancelRequested ? "cancelled" : "join"); break; }

        SyncProtocol::formatDeviceId(ESP.getEfuseMac(), s.id);
        s.fw = getFirmwareVersionString();
        snprintf(s.abi, sizeof(s.abi), "%d", kDeviceHalAbi);
        const BoardInfo::Info& info = HAL::boardInfo();
        snprintf(s.board, sizeof(s.board), "%u.%u", info.major, info.minor);
        LoadoutStore::begin();
        s.query = std::string("device_id=") + s.id + "&lapply_cap=" + SyncProtocol::kLapplyCapability;
        {
            // Loadout GETs carry the same telemetry the site validates.
            char extra[96];
            snprintf(extra, sizeof(extra), "&abi=%s&board_rev=%s&fs_total=%u&fs_used=%u",
                     s.abi, s.board, (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
            std::string fwParam;
            for (const char* p = s.fw; *p; ++p) {
                const char c = *p;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') fwParam += c;
                else { char esc[4]; snprintf(esc, sizeof(esc), "%%%02X", (unsigned)(uint8_t)c); fwParam += esc; }
            }
            s.query += "&fw=" + fwParam + extra;
        }
        s.checkinUrl = s.base + "/api/device-checkin.php";

        // ---- 1. check-in -------------------------------------------------
        std::string body = checkinBody(s, nullptr, nullptr);
        if (body.empty()) { setError(r, "checkin-body"); break; }
        HttpReply check;
        Step step = checkinWithRetry(s, plan, body, check, s.reply, autoapply, false, r);
        r.nextMs = plan.nextMs();
        if (step == Step::Error || step == Step::Deferred) break;
        recordCheckIn(r, s.reply.serverEpoch);
        if (s.reply.firmwareOffer) {
            // Only an offer: the update lane compares the manifest and asks.
            strcpy(r.offered, "fw");
            Preferences offered;
            if (offered.begin("upd", false)) {
                offered.putString("fw_url", s.reply.firmwareUrl.c_str());
                offered.end();
            }
        }
        if (step == Step::Done) { r.ok = true; r.none = true; break; }
        if (step == Step::Waiting) { r.ok = true; r.none = true; r.waiting = true; break; }
        if (step != Step::Loadout) { setError(r, "planner"); break; }
        const std::string batchId = s.reply.batchId;

        // ---- 2. offer ----------------------------------------------------
        const std::string loadoutUrl = s.base + "/api/device-loadout.php?" + s.query;
        HttpReply offerReply;
        Offer offer;
        OfferError offerError = OfferError::None;
        for (;;) {
            offerReply = HttpReply();
            if (!request(s, loadoutUrl, nullptr, offerReply, jsonSink, &offerReply)) {
                setError(r, "offer-transport"); step = Step::Error; break;
            }
            offerError = offerReply.status == 200
                ? parseOffer(offerReply.body.c_str(), batchId.c_str(), offer) : OfferError::None;
            OfferVerdict verdict = OfferVerdict::Ok;
            if (offerError == OfferError::Json) verdict = OfferVerdict::Retryable;
            else if (offerErrorPermanent(offerError)) verdict = OfferVerdict::Permanent;
            else if (offerReply.status == 200) {
                LoadoutManifest::AppliedRecord prior;
                bool have = false;
                {
                    LoadoutStore::Guard guard;
                    have = readAppliedRecord(prior);
                }
                if (have && prior.batch == batchId && prior.docCrc == offer.docCrc &&
                    prior.result == "applied") verdict = OfferVerdict::AlreadyApplied;
            }
            step = plan.loadout(offerReply.status, verdict);
            if (step != Step::Backoff) break;
            r.nextMs = errorNextMs(offerReply);
            rememberPoll(r.nextMs, offerReply.status, offerReply.retry);
            const uint32_t wait = retryWaitMs(offerReply.retry);
            step = plan.retry(wait != 0 && s.covers(wait));
            if (step == Step::Deferred) { setError(r, "rate-limited"); break; }
            if (!sleepFor(s, wait)) {
                setError(r, cancelRequested ? "cancelled" : "deadline");
                step = Step::Error; break;
            }
        }
        if (offerReply.status == 200 && offer.nextPollMs) {
            r.nextMs = offer.nextPollMs;
            rememberPoll(r.nextMs, offerReply.status, offerReply.retry);
        } else if (offerReply.status == 204) {
            r.nextMs = offerReply.nextMs ? offerReply.nextMs : r.nextMs;
            rememberPoll(r.nextMs, offerReply.status, offerReply.retry);
        } else if (offerReply.status >= 400 && offerReply.status != 429) {
            r.nextMs = errorNextMs(offerReply);
            rememberPoll(r.nextMs, offerReply.status, offerReply.retry);
        }
        if (step == Step::Deferred) break;
        if (step == Step::Done) { r.ok = true; r.none = true; break; }
        if (step == Step::Error) {
            if (strcmp(r.err, "none") == 0)
                setError(r, offerError == OfferError::Json ? "offer-body" : "offer-http");
            break;
        }

        // ---- 3. blobs + apply ----------------------------------------------
        const char* answer = nullptr;
        bool appliedOk = false;
        if (step == Step::Ack) {
            answer = plan.rejected() ? offerRejection(offerError) : "already-applied";
        } else {
            SyncProtocol::LittleFsFerryStorage storage;
            SyncProtocol::FerrySession ferry(storage);
            const char* rejection = nullptr;
            step = plan.blob(fetchBlobs(s, offer, ferry, r, rejection));
            if (step == Step::Error) break;
            if (step == Step::Ack) answer = rejection;
            if (step == Step::Apply) {
                // The exact decoded doc bytes, with the server's CRC in the
                // header. No reserialization or local op interpretation.
                MemoryBytes bytes((const uint8_t*)offer.doc.data(), offer.doc.size());
                char applyHeader[32];
                snprintf(applyHeader, sizeof(applyHeader), "%u %08x",
                         (unsigned)offer.doc.size(), (unsigned)offer.docCrc);
                SyncProtocol::FerryReply applied;
                {
                    LoadoutStore::Guard guard;
                    applied = ferry.applyManifest(applyHeader, bytes);
                }
                step = plan.applied(applied.ok);
                appliedOk = applied.ok;
                r.appliedNow = plan.appliedNow();
                const bool stale = strstr(applied.text, "lapply.stale=") != nullptr;
                const bool recordFailure = strstr(applied.text, "lapply.record") != nullptr;
                answer = applied.ok ? "applied" :
                         stale ? "stale-revision" :
                         recordFailure ? "rejected:record" : "rejected:apply";
            }
        }
        if (step != Step::Ack || !answer) { setError(r, "planner"); break; }
        if (appliedOk || strcmp(answer, "already-applied") == 0) {
            Preferences ack;
            if (ack.begin("upd", false)) {
                ack.putString("ack_fp", s.fingerprint.c_str());
                ack.end();
            }
            strncpy(r.applied, batchId.c_str(), sizeof(r.applied) - 1);
            r.ok = true;
            setError(r, "none");
        } else {
            r.ok = false;
            setError(r, answer);
        }

        // ---- 4. follow-up check-in carrying the answer ----------------------
        const uint32_t floorMs = followupFloorMs(s.reply);
        const uint32_t since = millis() - s.lastCheckinAt;
        const uint32_t wait = since >= floorMs ? 0 : floorMs - since;
        step = plan.report(s.covers(wait));
        if (step == Step::Deferred) {
            // An applied batch is reported from its record next session; a
            // rejection is found and answered again when it is re-offered.
            if (r.ok) setError(r, "report-deferred");
            break;
        }
        if (!sleepFor(s, wait)) {
            setError(r, cancelRequested ? "cancelled" : "deadline");
            r.ok = false;
            break;
        }
        body = checkinBody(s, batchId.c_str(), answer);
        if (body.empty()) { setError(r, "ack-body"); r.ok = false; break; }
        HttpReply follow;
        CheckinReply followParsed;
        const bool wasOk = r.ok;
        char answered[sizeof(r.err)];
        memcpy(answered, r.err, sizeof(answered));
        step = checkinWithRetry(s, plan, body, follow, followParsed, autoapply, true, r);
        r.nextMs = plan.nextMs();
        if (step == Step::Done) {
            recordCheckIn(r, followParsed.serverEpoch);
        } else if (step == Step::Deferred) {
            // Same as a deferral before sending: the next session answers.
            setError(r, wasOk ? "report-deferred" : answered);
        } else {
            r.ok = false;
        }
    } while (false);
    // Reported over the transport error the failed request left behind.
    if (rootsRejected) { r.ok = false; setError(r, "roots-parse"); }

    bool radioOff = true;
    if (radioStarted) {
        WiFi.disconnect(true);
        radioOff = WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_OFF;
        if (!radioOff) { r.ok = false; setError(r, "wifi-off"); }
    }
    memset(s.token, 0, sizeof(s.token));
    memset(s.pass, 0, sizeof(s.pass));
    sampleHeap();
    // The handshake trough falls inside esp_http_client_open() where no
    // sample runs; a drop in the since-boot low-water mark is ours.
    const size_t bootLowAfter = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (bootLowAfter < bootLowBefore && bootLowAfter < heapLow) heapLow = bootLowAfter;
    r.heapMin = (uint32_t)heapLow;
    if (!r.ok && strcmp(r.err, "none") == 0 && s.elapsed() >= kSessionMs) setError(r, "deadline");
    if (plan.failure(radioOff) == Step::Reboot) {
        // A radio that did not switch off must not be followed by
        // Bluetooth in this power cycle. Deliver the error after restart.
        Preferences boot;
        if (boot.begin("bootcfg", false)) {
            boot.putString("clderr", r.err);
            boot.putBool("skipanim", true);
            boot.end();
        }
        esp_restart();
    }
}

void worker(void*) {
    {
        Result r;
        runWorker(r);
        result = r;
    }
    finished = true;
    running = false;
    vTaskDelete(nullptr);
}
} // namespace

bool runSession(Reason reason) {
    (void)reason;
    if (running || finished || SerialCli::instance().ferryActive() ||
        SerialCli::instance().radioBusy()) return false;
    // Never under an app that owns a radio: it would share the power cycle.
    const AppIndex active = AppManager::instance().activeApp();
    if (active == APP_MUSIC_PLAYER || active == APP_WEB_PORTAL) return false;
    CloudPlanner entry;
    if (entry.start(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) == Step::Reboot) {
        Preferences prefs;
        if (!prefs.begin("bootcfg", false)) return false;
        bool saved = prefs.putBool("bootcloud", true) != 0 &&
                     prefs.putBool("skipanim", true) != 0;
        prefs.end();
        if (!saved) return false;
        esp_restart();
        return true;
    }
    if (WiFi.getMode() != WIFI_OFF) return false;
    cancelRequested = false;
    available = false;
    running = true;
    StatusService::instance().post(StatusKind::Checking, nullptr,
            StatusPriority::Normal, true, millis());
    if (xTaskCreate(worker, "cloudsync", kStackBytes, nullptr, 1, nullptr) != pdPASS) {
        running = false;
        StatusService::instance().clear(StatusKind::Checking);
        result = Result();
        setError(result, "task-create");
        result.heapMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        available = true;
        return true;
    }
    return true;
}

void poll() {
    if (!finished) return;
    finished = false;
    available = true;
    StatusService::instance().clear(StatusKind::Checking);
    if (result.checkInSec) StatusService::instance().setCheckIn(result.checkInSec, false);
    // A firmware offer is not posted here: the check-in always carries one,
    // and only the update lane can tell whether it is newer.
    if (result.waiting)
        StatusService::instance().post(StatusKind::ChangesWaiting, nullptr,
            StatusService::defaultPriority(StatusKind::ChangesWaiting), true, millis());
    if (strcmp(result.applied, "-") != 0)
        StatusService::instance().clear(StatusKind::ChangesWaiting);
    // Only an apply in this session is news; an already-applied batch is not.
    if (result.appliedNow)
        StatusService::instance().post(StatusKind::Info, "App changes applied",
            StatusPriority::Normal, false, millis());
}

void recoverFailure() {
    Preferences boot;
    if (!boot.begin("bootcfg", false)) return;
    const String error = boot.getString("clderr", "");
    if (!error.isEmpty()) {
        boot.remove("clderr");
        result = Result();
        setError(result, error.c_str());
        available = true;
    }
    boot.end();
}

bool consumeResult(Result& out) {
    if (!available) return false;
    out = result;
    available = false;
    return true;
}
bool cancelPending() {
    if (!running) return WiFi.getMode() == WIFI_OFF;
    cancelRequested = true;
    const uint32_t started = millis();
    while (running && millis() - started < kCancelWaitMs) delay(10);
    return !running && WiFi.getMode() == WIFI_OFF;
}
bool busy() { return running; }
bool radioUsedThisPowerCycle() { return radioUsed; }

#ifdef CF_TEST_CLI
bool setBase(const char* url) {
    if (running || !validBase(url)) return false;
    Preferences prefs;
    if (!prefs.begin("upd", false)) return false;
    const bool ok = prefs.putString("base", url) != 0;
    prefs.end(); return ok;
}
bool setToken(const char* token) {
    if (running || !token || !*token || strlen(token) > 90 || strpbrk(token, " \t\r\n")) return false;
    Preferences prefs;
    if (!prefs.begin("pair", false)) return false;
    const bool ok = prefs.putString("tok", token) != 0;
    prefs.end(); return ok;
}
bool setAutoapply(bool enabled) {
    if (running) return false;
    Preferences prefs;
    if (!prefs.begin("upd", false)) return false;
    const bool ok = prefs.putBool("autoapply", enabled) != 0;
    prefs.end(); return ok;
}
#endif
} // namespace CloudSync
#endif // HOST_TEST
