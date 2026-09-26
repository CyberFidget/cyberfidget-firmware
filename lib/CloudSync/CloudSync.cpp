// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "CloudSync.h"
#include "CloudPlanner.h"
#include "CloudProtocol.h"
#include "LinkSession.h"

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
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <sys/time.h>
#include <time.h>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <atomic>

#include "AppDefs.h"
#include "BatteryDiary.h"
#include "UsageUpload.h"
#include "CheckinPolicy.h"
#include "AppManager.h"
#include "FerrySession.h"
#include "HAL.h"
#include "LittleFsFerryStorage.h"
#include "LoadoutManifest.h"
#include "LoadoutStore.h"
#include "MenuManager.h"
#include "StatusService.h"
#include "SerialCli.h"
#include "SyncProtocol.h"
#include "DeviceIdentity.h"
#include "DeviceLinkApp.h"
#include "TrustedRoots.h"
#include "AwakePolicy.h"
#include "PromptPolicy.h"
#include "UpdateSession.h"
#include "SavedWifi.h"
#include "WasmHostImports.h"
#include "globals.h"

namespace CloudSync {
namespace {
constexpr uint32_t kJoinMs = 10000;
// The boot window's join budget: the start-up animation is 5 s and the
// menu never waits for it.
constexpr uint32_t kBootJoinMs = 4000;
// Scheduled sessions never wait out the server's spacing, so they need far
// less than a manual one. The headless wake's own guard sits above this.
constexpr uint32_t kScheduledSessionMs = 60000;
constexpr uint32_t kCallMs = 4000;
constexpr uint32_t kSessionMs = 150000; // includes the server's 60 s check-in floor
constexpr uint32_t kLinkSessionMs = 660000;
constexpr uint32_t kConfirmGraceMs = 120000; // retry window after a late OK
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
Reason sessionReason = Reason::Manual;
int32_t dailyVbatMv = -1;
int32_t dailySocPct = -1;
// "Get them now": this one session applies waiting app changes even with
// app auto-apply off. Never affects firmware (always an offer).
bool sessionApplyOnce = false;
bool taskFailed = false;   // loop task only
enum class WorkerKind : uint8_t { Cloud, Link, Unlink };
WorkerKind workerKind = WorkerKind::Cloud;
std::atomic<int> linkChoice{-1};
LinkSnapshot linkView;
portMUX_TYPE linkViewLock = portMUX_INITIALIZER_UNLOCKED;

void publishLink(LinkState state, const char* code = nullptr,
                 const char* account = nullptr, const char* error = nullptr) {
    portENTER_CRITICAL(&linkViewLock);
    linkView.state = state;
    if (code) { strncpy(linkView.code, code, 6); linkView.code[6] = '\0'; }
    if (account) { strncpy(linkView.account, account, 39); linkView.account[39] = '\0'; }
    if (error) { strncpy(linkView.error, error, 23); linkView.error[23] = '\0'; }
    ++linkView.generation;
    portEXIT_CRITICAL(&linkViewLock);
}

// Internal-heap low-water marks of this session (worker task only).
size_t heapLow = SIZE_MAX;
size_t largestLow = SIZE_MAX;
// The trusted root list failed to parse completely (worker task only).
bool rootsRejected = false;
void sampleHeap() {
    const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeBytes < heapLow) heapLow = freeBytes;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < largestLow) largestLow = largest;
}

bool scheduled(Reason reason) {
    return reason == Reason::Boot || reason == Reason::Daily || reason == Reason::Awake;
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
    std::string date;
    std::string body;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data) return ESP_OK;
    HttpReply* reply = static_cast<HttpReply*>(event->user_data);
    if (strcasecmp(event->header_key, "Retry-After") == 0)
        reply->retry = atoi(event->header_value);
    if (strcasecmp(event->header_key, "X-Next-Poll-Ms") == 0)
        reply->nextMs = (uint32_t)strtoul(event->header_value, nullptr, 10);
    if (strcasecmp(event->header_key, "Date") == 0)
        reply->date = event->header_value;
    return ESP_OK;
}

using Sink = bool (*)(void*, const uint8_t*, size_t);
bool jsonSink(void* arg, const uint8_t* data, size_t len) {
    HttpReply& reply = *static_cast<HttpReply*>(arg);
    if (reply.body.size() + len > kJsonMax) return false;
    reply.body.append((const char*)data, len);
    return true;
}

// Owned by the worker. The PEM is allocated in PSRAM and must remain alive
// for the entire handle lifetime, including idle keep-alive time. The handle
// also retains HTTP/TLS buffers (mostly PSRAM, plus internal socket/lwIP and
// mbedTLS bookkeeping); release both before a long wait or app handoff.
struct HttpConnection {
    esp_http_client_handle_t client = nullptr;
    char* roots = nullptr;
    ~HttpConnection() { clear(); }
    void clear() {
        if (client) esp_http_client_cleanup(client);
        client = nullptr;
        if (roots) TrustedRoots::freePem(roots);
        roots = nullptr;
        sampleHeap();
    }
    HttpConnection() = default;
    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
};

// Everything one worker run needs; the credential is wiped at exit (the
// WiFi passwords stay inside SavedWifi::join).
struct Session {
    uint32_t started = 0;
    char token[96] = {0};
    std::string base;
    std::string query;
    std::string checkinUrl;
    std::string fingerprint;
    char id[13] = {0};
    char flashId[17] = {0};
    char serial[9] = {0};
    bool credential = true;
    mutable bool serverUnlinked = false;
    mutable bool serverWrongDevice = false;
    std::vector<std::string> attemptedRevokes;
    uint32_t limitMs = kSessionMs;
    bool mayWait = true;          // false: never sleep through a server wait
    const char* fw = "";
    char abi[12] = {0};
    char board[16] = {0};
    uint32_t lastCheckinAt = 0;   // millis() when the last check-in finished
    CheckinReply reply;           // the first check-in's answer
    const char* mode = "normal";  // the Awake & dev mode setting, as the site names it
    ReportState reportState;       // RAM only; first check-in of this session is full
    HttpConnection* checkinConnection = nullptr; // dev loop only, worker owned

    uint32_t elapsed() const { return millis() - started; }
    bool covers(uint32_t waitMs) const {
        if (!mayWait && waitMs) return false;
        return budgetCovers(waitMs, elapsed(), limitMs, kReserveMs);
    }
};

// A join ends early when the session is cancelled or out of time.
bool sessionStop(void* ctx) {
    const Session& s = *static_cast<const Session*>(ctx);
    return cancelRequested || s.elapsed() >= s.limitMs;
}

void setSessionFingerprint(Session& s, const char* account, const char* linkedAt) {
    unsigned char hash[32];
    mbedtls_sha256_context identity;
    mbedtls_sha256_init(&identity);
    mbedtls_sha256_starts(&identity, 0);
    mbedtls_sha256_update(&identity, (const unsigned char*)s.token, strlen(s.token));
    mbedtls_sha256_update(&identity, (const unsigned char*)account, strlen(account));
    mbedtls_sha256_update(&identity, (const unsigned char*)linkedAt, strlen(linkedAt));
    mbedtls_sha256_finish(&identity, hash);
    mbedtls_sha256_free(&identity);
    char fp[65]; digestHex(hash, fp);
    s.fingerprint.assign(fp, 16);
}

class PrefsLinkStore : public LinkStore {
public:
    explicit PrefsLinkStore(Preferences& pair) : pair_(pair) {}
    std::string getString(const char* key) override { return pair_.getString(key, "").c_str(); }
    uint32_t getUInt(const char* key) override { return pair_.getUInt(key, 0); }
    bool getBool(const char* key) override { return pair_.getBool(key, false); }
    bool putString(const char* key, const std::string& value) override {
        return !value.empty() && pair_.putString(key, value.c_str()) == value.size();
    }
    bool putUInt(const char* key, uint32_t value) override { return pair_.putUInt(key, value) != 0; }
    bool putBool(const char* key, bool value) override { return pair_.putBool(key, value) != 0; }
    bool remove(const char* key) override { return !pair_.isKey(key) || pair_.remove(key); }
private:
    Preferences& pair_;
};

// All HTTP calls are bounded by a short per-call timeout and the session
// deadline. The URL must be same-origin and certificate-checked in releases:
// the server's chain must end in the trusted root list (lib/TrustedRoots),
// whose PEM text must outlive the client.
bool request(const Session& s, const std::string& url, const std::string* post,
             HttpReply& reply, Sink sink, void* sinkArg) {
    HttpConnection once;
    const bool reusable = s.checkinConnection && post && url == s.checkinUrl;
    HttpConnection& connection = reusable ? *s.checkinConnection : once;
    // Every authenticated endpoint is built under the configured origin.
    if (url.compare(0, s.base.size(), s.base) != 0 ||
        url.size() <= s.base.size() || url[s.base.size()] != '/' ||
        (s.credential && !DeviceIdentity::checkStored()) ||
        s.elapsed() >= s.limitMs || cancelRequested || WiFi.status() != WL_CONNECTED) {
        connection.clear();
        return false;
    }
    const bool wasOpen = connection.client != nullptr;
    if (!connection.client) {
        connection.roots = TrustedRoots::newPem();
        if (!connection.roots) return false;
        // A partial parse would trust fewer roots: never connect on one.
        if (!TrustedRoots::pemParsesCompletely(connection.roots)) {
            rootsRejected = true;
            connection.clear();
            return false;
        }
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
        config.cert_pem = connection.roots;
        config.disable_auto_redirect = true;
        config.timeout_ms = kCallMs;
        config.event_handler = onHttpEvent;
        config.user_data = &reply;
        connection.client = esp_http_client_init(&config);
        if (!connection.client) { connection.clear(); return false; }
    }
    esp_http_client_handle_t client = connection.client;
    if (esp_http_client_set_user_data(client, &reply) != ESP_OK ||
        esp_http_client_set_timeout_ms(client, kCallMs) != ESP_OK) {
        connection.clear();
        return false;
    }
    char auth[112];
    snprintf(auth, sizeof(auth), "Bearer %s", s.token);
    const bool authSet = esp_http_client_set_header(client, "Authorization", auth) == ESP_OK;
    memset(auth, 0, sizeof(auth));
    if (!authSet || (post && esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK)) {
        connection.clear();
        return false;
    }
    bool ok = esp_http_client_open(client, post ? post->size() : 0) == ESP_OK;
    sampleHeap();
    if (ok && post) {
        int wrote = esp_http_client_write(client, post->data(), post->size());
        ok = wrote == (int)post->size();
    }
    if (ok) {
        ok = esp_http_client_fetch_headers(client) >= 0;
    }
    if (!ok && wasOpen) {
        // The server may have closed the kept connection while idle (its
        // keep-alive timeout or request cap): nothing was answered, so retry
        // once on a fresh connection instead of reporting a failed check-in.
        connection.clear();
        reply = HttpReply();
        return request(s, url, post, reply, sink, sinkArg);
    }
    if (ok) {
        reply.status = esp_http_client_get_status_code(client);
        if (reply.status == 204) {
            // A 204 has no entity. If the parser did not finish it, do not
            // carry an ambiguous response into the next request.
            const bool stillValid = s.elapsed() < s.limitMs && !cancelRequested &&
                                    WiFi.status() == WL_CONNECTED;
            if (!stillValid || !esp_http_client_is_complete_data_received(client) || !reusable)
                connection.clear();
            else esp_http_client_set_user_data(client, nullptr);
            return stillValid;
        }
        if (reply.status >= 400) { sink = jsonSink; sinkArg = &reply; }
        uint8_t buf[1024];
        while (s.elapsed() < s.limitMs && !cancelRequested) {
            int n = esp_http_client_read(client, (char*)buf, sizeof(buf));
            if (n < 0) { ok = false; break; }
            if (n == 0) {
                ok = esp_http_client_is_complete_data_received(client);
                break;
            }
            if (!sink(sinkArg, buf, (size_t)n)) { ok = false; break; }
        }
        if (s.elapsed() >= s.limitMs || cancelRequested) ok = false;
    }
    sampleHeap();
    if (!ok || reply.status < 200 || reply.status >= 300 || !reusable || cancelRequested ||
        WiFi.status() != WL_CONNECTED) connection.clear();
    else esp_http_client_set_user_data(client, nullptr);
    if (s.credential && ok && reply.status == 401) {
        cJSON* body = cJSON_Parse(reply.body.c_str());
        const cJSON* item = body ? cJSON_GetObjectItemCaseSensitive(body, "error") : nullptr;
        const CredentialError error = credentialError(reply.status,
            cJSON_IsString(item) && item->valuestring ? item->valuestring : "");
        cJSON_Delete(body);
        if (error != CredentialError::Other) {
            Preferences pair;
            if (pair.begin("pair", false)) {
                PrefsLinkStore store(pair);
                const bool removed = error == CredentialError::WrongDevice
                    ? wipeMismatchedLink(store) : forgetServerUnlinkedLink(store);
                pair.end();
                if (removed) {
                    s.serverUnlinked = error == CredentialError::NotLinked;
                    s.serverWrongDevice = error == CredentialError::WrongDevice;
                }
            }
        }
    }
    return ok;
}

// Stores next_poll_ms and applies the reply's effect on the stored backoff
// (see backoffFor: any Retry-After is kept, only a success clears it).
void rememberPoll(uint32_t ms, int status, int retry) {
    Preferences prefs;
    if (!prefs.begin("upd", false)) return;
    // Only a change is written (dev mode checks in every few seconds).
    if (ms && prefs.getUInt("next_ms", 0) != ms) prefs.putUInt("next_ms", ms);
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
    Preferences pair;
    if (pair.begin("pair", false)) {
        PrefsLinkStore store(pair);
        setLinkTimeIfMissing(store, serverSec);
        pair.end();
    }
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

std::string checkinBody(const Session& s, const char* answerBatch, const char* answer,
                        uint32_t& crc, bool& full, std::string& ackBatch) {
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
    fields.flashId = s.flashId;
    fields.serial = s.serial;
    fields.fw = s.fw;
    fields.abi = s.abi;
    fields.board = s.board;
    fields.fsTotal = (uint32_t)LittleFS.totalBytes();
    fields.fsUsed = (uint32_t)LittleFS.usedBytes();
    fields.manifestCrc = present ? SyncProtocol::crc32(manifest.data(), manifest.size()) : 0;
    crc = fields.manifestCrc;
    fields.mode = s.mode;
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
    ackBatch = fields.appliedBatch && fields.result &&
               validBatchId(fields.appliedBatch) && validResult(fields.result)
                   ? fields.appliedBatch : "";
    full = fullReportRequired(s.reportState, crc, ackBatch.empty() ? nullptr : ackBatch.c_str());
    fields.installed = full ? &loadout : nullptr;
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
        if (cancelRequested || s.elapsed() >= s.limitMs) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return true;
}

std::string pairBody(const char* action, const char* code = nullptr,
                     const Session* s = nullptr, bool relinked = false) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return {};
    cJSON_AddStringToObject(root, "action", action);
    if (code) cJSON_AddStringToObject(root, "device_code", code);
    if (s) {
        cJSON_AddStringToObject(root, "device_id", s->id);
        if (s->flashId[0]) cJSON_AddStringToObject(root, "flash_id", s->flashId);
        if (s->serial[0]) cJSON_AddStringToObject(root, "serial", s->serial);
        if (strcmp(action, "begin") == 0) {
            cJSON_AddStringToObject(root, "board_rev", s->board);
            cJSON_AddStringToObject(root, "fw", s->fw);
        }
    }
    if (relinked) cJSON_AddStringToObject(root, "reason", "relinked");
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded ? encoded : "";
    cJSON_free(encoded);
    cJSON_Delete(root);
    return body;
}

bool postPair(Session& s, const std::string& body, HttpReply& reply) {
    reply = HttpReply();
    return !body.empty() && request(s, s.base + "/api/device-pair.php",
                                    &body, reply, jsonSink, &reply);
}

// A retained old credential is sent only from the hardware that stored it.
void drainRevoke(Session& s) {
    if (!DeviceIdentity::checkStored()) return;
    Preferences pair;
    for (int i = 0; i < 2; ++i) {
        if (!pair.begin("pair", false)) return;
        PrefsLinkStore store(pair);
        std::string old;
        bool relinked = false, second = false;
        if (!pendingRevoke(store, old, relinked, second)) { pair.end(); return; }
        const time_t now = time(nullptr);
        auto unavailable = [&](const std::string& token, bool slot) {
            for (const std::string& prior : s.attemptedRevokes)
                if (prior == token) return true;
            return revokeRetryPending(store, slot, now > 1577836800 ? (uint32_t)now : 0);
        };
        if (unavailable(old, second) && !second) {
            old = store.getString("rev_tok2");
            relinked = store.getBool("rev_rel2");
            second = true;
        }
        if (old.empty() || unavailable(old, second)) { pair.end(); return; }
        LinkRecord activeLink;
        readLink(store, activeLink);
        if (!shouldSendRevoke(old, activeLink.token) && !second) {
            const std::string other = store.getString("rev_tok2");
            if (!other.empty() && !unavailable(other, true)) {
                old = other;
                relinked = store.getBool("rev_rel2");
                second = true;
            }
        }
        if (!shouldSendRevoke(old, activeLink.token)) {
            pair.end();
            return;
        }
        pair.end();
        s.attemptedRevokes.push_back(old);
        char active[sizeof(s.token)];
        memcpy(active, s.token, sizeof(active));
        strncpy(s.token, old.c_str(), sizeof(s.token) - 1);
        s.token[sizeof(s.token) - 1] = '\0';
        const bool credential = s.credential;
        s.credential = false;
        HttpReply reply;
        bool answered = false;
        for (int attempt = 0; attempt < 2 && !cancelRequested; ++attempt) {
            reply = HttpReply();
            const bool sent = postPair(s, pairBody("revoke", nullptr, &s, relinked), reply);
            answered = sent && revokeIsFinal(reply.status);
            if (answered || attempt == 1) break;
            const uint32_t wait = reply.retry > 0 ? retryWaitMs(reply.retry) : 1000;
            if (!wait || !sleepFor(s, wait)) break;
        }
        s.credential = credential;
        memcpy(s.token, active, sizeof(active));
        memset(active, 0, sizeof(active));
        if (!answered && cancelRequested) return;
        if (!pair.begin("pair", false)) return;
        PrefsLinkStore done(pair);
        if (answered) finishRevoke(done, reply.status, second);
        else if (reply.retry > 0 && reply.retry <= kMaxRetryAfterSec) {
            const time_t at = time(nullptr);
            if (at > 1577836800)
                done.putUInt(second ? "rev_at2" : "rev_at", (uint32_t)at + reply.retry);
        }
        pair.end();
        if (!answered) return;
    }
}

bool clearPreviousApps(bool& changed) {
    changed = false;
    LoadoutStore::Guard guard;
    if (!LoadoutStore::begin()) return false;
    std::string raw;
    if (!LoadoutStore::load(raw)) return !LittleFS.exists("/loadout.json");
    LoadoutManifest::Loadout loadout;
    if (!loadLoadoutManifest(loadout)) return false;
    const size_t before = loadout.entries.size();
    const std::vector<std::string> blobs = LoadoutManifest::removeNonBuiltin(loadout);
    if (loadout.entries.size() == before) return true;
    bool removed = true;
    for (const auto& path : blobs) {
        if (path.compare(0, 6, "/apps/") == 0 &&
            SyncProtocol::pathConfined(path.c_str()) &&
            LittleFS.exists(path.c_str()) && !LittleFS.remove(path.c_str())) removed = false;
    }
    if (!removed || !LoadoutStore::save(LoadoutManifest::serializeManifest(loadout))) return false;
    changed = true;
    return true;
}

bool clearAppsForLink(void* context) {
    return clearPreviousApps(*static_cast<bool*>(context));
}

void recoverClearBeforeSession() {
    Preferences pair;
    if (!pair.begin("pair", false)) return;
    PrefsLinkStore store(pair);
    bool changed = false;
    recoverPendingClear(store, clearAppsForLink, &changed);
    pair.end();
    if (changed) MenuManager::instance().markManifestDirty();
}

void runPairWorker(Result& r) {
    Session s;
    s.started = millis();
    s.limitMs = kLinkSessionMs;
    s.credential = false;
    bool radioStarted = false;
    bool locallyUnlinked = false;
    do {
        if (!DeviceIdentity::checkStored()) { setError(r, "link-mismatch"); break; }
        if (workerKind == WorkerKind::Unlink) {
            Preferences pair;
            if (!pair.begin("pair", false)) { setError(r, "storage"); break; }
            PrefsLinkStore store(pair);
            const bool unlinked = prepareUnlink(store);
            LinkRecord remaining;
            locallyUnlinked = !readLink(store, remaining);
            pair.end();
            if (!unlinked) {
                if (locallyUnlinked) publishLink(LinkState::Unlinked);
                setError(r, "storage"); break;
            }
            publishLink(LinkState::Unlinked);
        }
        if (mbedtls_platform_set_calloc_free(tlsPsramCalloc, heap_caps_free) != 0) {
            setError(r, "tls-allocator"); break;
        }
        cJSON_Hooks hooks = {};
        hooks.malloc_fn = jsonPsramMalloc;
        hooks.free_fn = heap_caps_free;
        cJSON_InitHooks(&hooks);
        if (!SavedWifi::anySaved()) { setError(r, "no-wifi"); break; }
        s.base = kDefaultBase;
#ifdef CF_TEST_CLI
        Preferences upd;
        if (upd.begin("upd", true)) {
            s.base = upd.getString("base", kDefaultBase).c_str();
            upd.end();
        }
#endif
        if (!validBase(s.base.c_str())) { setError(r, "invalid-base"); break; }
        const DeviceIdentity::Fingerprint live = DeviceIdentity::readLive();
        strcpy(s.id, live.id);
        strcpy(s.flashId, live.flashId);
        strcpy(s.serial, live.serial);
        s.fw = getFirmwareVersionString();
        const BoardInfo::Info& info = HAL::boardInfo();
        snprintf(s.board, sizeof(s.board), "%u.%u", info.major, info.minor);
        if (WiFi.getMode() != WIFI_OFF) { setError(r, "radio-busy"); break; }
        WiFi.persistent(false);
        radioStarted = true;
        radioUsed = true;
        if (!WiFi.mode(WIFI_STA)) { setError(r, "sta-mode"); break; }
        SavedWifi::JoinOptions joinOpt;
        joinOpt.firstMs = kJoinMs;
        joinOpt.fallbackMs = kJoinMs;
        joinOpt.stop = [](void*) { return cancelRequested.load(); };
        SavedWifi::JoinResult joined;
        if (!SavedWifi::join(joinOpt, joined)) { setError(r, "join"); break; }

        drainRevoke(s);
        Preferences pendingPair;
        if (!pendingPair.begin("pair", true)) { setError(r, "storage"); break; }
        PrefsLinkStore pendingStore(pendingPair);
        const bool pending = hasPendingRevoke(pendingStore);
        const bool slotFree = canBeginLink(pendingStore);
        pendingPair.end();
        if (workerKind == WorkerKind::Unlink) {
            if (pending) { setError(r, "try-again"); break; }
            r.ok = true;
            break;
        }
        if (!slotFree) { setError(r, "try-again"); break; }

        HttpReply reply;
        if (!postPair(s, pairBody("begin", nullptr, &s), reply) || reply.status != 200) {
            setError(r, "begin"); break;
        }
        cJSON* json = cJSON_Parse(reply.body.c_str());
        const cJSON* codeItem = json ? cJSON_GetObjectItemCaseSensitive(json, "device_code") : nullptr;
        const cJSON* userItem = json ? cJSON_GetObjectItemCaseSensitive(json, "user_code") : nullptr;
        const cJSON* intervalItem = json ? cJSON_GetObjectItemCaseSensitive(json, "interval") : nullptr;
        const cJSON* expiryItem = json ? cJSON_GetObjectItemCaseSensitive(json, "expires_in") : nullptr;
        const std::string code = cJSON_IsString(codeItem) && codeItem->valuestring ? codeItem->valuestring : "";
        const std::string user = cJSON_IsString(userItem) && userItem->valuestring ? userItem->valuestring : "";
        uint32_t interval = cJSON_IsNumber(intervalItem) ? (uint32_t)intervalItem->valuedouble : 5;
        uint32_t expiry = cJSON_IsNumber(expiryItem) ? (uint32_t)expiryItem->valuedouble : 600;
        cJSON_Delete(json);
        if (code.size() != 43 || user.size() != 6 || interval < 1 || expiry < 1) {
            setError(r, "begin-reply"); break;
        }
        LinkRecord oldLink;
        Preferences existing;
        if (existing.begin("pair", true)) {
            PrefsLinkStore store(existing);
            readLink(store, oldLink);
            if (oldLink.account.empty() && oldLink.accountRef.empty()) {
                oldLink.account = store.getString("prev_acct");
                oldLink.accountRef = store.getString("prev_aref");
            }
            existing.end();
        }
        LinkSession exchange(oldLink.account, oldLink.accountRef);
        publishLink(LinkState::Code, user.c_str());
        const uint32_t issued = millis();
        bool sentConfirm = false;
        uint32_t nextWait = interval;
        while (!cancelRequested && pairingMayContinue(sentConfirm,
                   millis() - issued, expiry * 1000u, s.elapsed(), s.limitMs)) {
            if (exchange.phase() == PairPhase::Waiting && !sleepFor(s, nextWait * 1000u)) break;
            int choice = linkChoice.exchange(-1);
            if (exchange.phase() == PairPhase::Confirm && choice >= 0 && choice <= 1) {
                exchange.answer(choice == 1);
                if (exchange.phase() == PairPhase::ClearApps) publishLink(LinkState::ClearApps);
                else if (exchange.phase() == PairPhase::Confirming) publishLink(LinkState::Confirming);
            }
            if (exchange.phase() == PairPhase::ClearApps && (choice == 2 || choice == 3)) {
                exchange.clearApps(choice == 2);
                publishLink(LinkState::Confirming);
            }
            if (exchange.phase() == PairPhase::Declined) {
                for (int attempt = 0; attempt < 3 && !cancelRequested; ++attempt) {
                    HttpReply declined;
                    if (postPair(s, pairBody("decline", code.c_str()), declined) &&
                        (declined.status == 200 || declined.status == 404)) break;
                    if (attempt < 2 && !sleepFor(s, 1000)) break;
                }
                publishLink(LinkState::Declined);
                r.ok = true;
                break;
            }
            if (exchange.phase() == PairPhase::Confirm || exchange.phase() == PairPhase::ClearApps) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (exchange.phase() == PairPhase::Confirming && sentConfirm &&
                !sleepFor(s, nextWait * 1000u)) break;
            HttpReply poll;
            const bool confirming = exchange.phase() == PairPhase::Confirming;
            if (confirming && !sentConfirm)
                s.limitMs = confirmSessionLimit(s.limitMs, s.elapsed(), kConfirmGraceMs);
            if (confirming) sentConfirm = true;
            const bool sent = postPair(s, pairBody(confirming ? "confirm" : "poll", code.c_str()), poll);
            if (!sent || poll.status == 429 || poll.status >= 500) {
                nextWait = pairPollWait(interval, poll.status, poll.retry);
                continue;
            }
            if (poll.status != 200) { setError(r, "request"); break; }
            nextWait = pairPollWait(interval, poll.status, poll.retry);
            json = cJSON_Parse(poll.body.c_str());
            const cJSON* statusItem = json ? cJSON_GetObjectItemCaseSensitive(json, "status") : nullptr;
            const cJSON* accountItem = json ? cJSON_GetObjectItemCaseSensitive(json, "account_label") : nullptr;
            const cJSON* accountRefItem = json ? cJSON_GetObjectItemCaseSensitive(json, "account_ref") : nullptr;
            const cJSON* tokenItem = json ? cJSON_GetObjectItemCaseSensitive(json, "token") : nullptr;
            const cJSON* nextItem = json ? cJSON_GetObjectItemCaseSensitive(json, "interval") : nullptr;
            const cJSON* timeItem = json ? cJSON_GetObjectItemCaseSensitive(json, "server_time") : nullptr;
            const std::string status = cJSON_IsString(statusItem) ? statusItem->valuestring : "";
            const std::string account = cJSON_IsString(accountItem) ? accountItem->valuestring : "";
            const std::string accountRef = cJSON_IsString(accountRefItem) ? accountRefItem->valuestring : "";
            const std::string token = cJSON_IsString(tokenItem) ? tokenItem->valuestring : "";
            const uint32_t serverTime = cJSON_IsString(timeItem) ? serverEpoch(timeItem->valuestring) : 0;
            if (cJSON_IsNumber(nextItem) && nextItem->valuedouble >= 1) {
                interval = (uint32_t)nextItem->valuedouble;
                nextWait = interval;
            }
            cJSON_Delete(json);
            exchange.reply(status, account, token, serverTime, accountRef);
            if (exchange.phase() == PairPhase::Waiting) continue;
            if (exchange.phase() == PairPhase::Confirming) continue;
            if (exchange.phase() == PairPhase::Confirm && status == "confirm_on_device") {
                publishLink(LinkState::Confirm, nullptr, account.c_str());
                continue;
            }
            if (exchange.phase() == PairPhase::Declined) { publishLink(LinkState::Declined); r.ok = true; break; }
            if (exchange.phase() == PairPhase::Expired) { publishLink(LinkState::Expired); r.ok = true; break; }
            if (exchange.phase() == PairPhase::Store) {
                Preferences pair;
                if (!pair.begin("pair", false)) { setError(r, "storage"); break; }
                PrefsLinkStore store(pair);
                LinkRecord next;
                next.token = exchange.token();
                next.account = exchange.account();
                next.accountRef = exchange.accountRef();
                next.at = exchange.linkTime();
                next.id = s.id;
                next.flashId = s.flashId;
                next.serial = s.serial;
                bool changed = false;
                const LinkCompletion completion = completeConfirmedLink(
                    store, next, exchange.shouldClearApps(), clearAppsForLink, &changed);
                pair.end();
                if (completion == LinkCompletion::StorageFailed) { setError(r, "storage"); break; }
                exchange.stored();
                r.ok = true;
                if (completion == LinkCompletion::LinkedClearFailed) {
                    r.appsClearFailed = true;
                    setError(r, "apps-clear");
                }
                r.manifestChanged = changed;
                publishLink(LinkState::Linked, nullptr, exchange.account().c_str(),
                            r.appsClearFailed ? "apps-clear" : "");
                drainRevoke(s);
                break;
            }
            setError(r, "reply"); break;
        }
        if (!r.ok && strcmp(r.err, "none") == 0) {
            if (cancelRequested) setError(r, "cancelled");
            else { publishLink(LinkState::Expired); r.ok = true; }
        }
    } while (false);
    if (radioStarted) {
        WiFi.disconnect(true);
        if (!WiFi.mode(WIFI_OFF) || WiFi.getMode() != WIFI_OFF) esp_restart();
    }
    memset(s.token, 0, sizeof(s.token));
    if (!r.ok && !locallyUnlinked)
        publishLink(LinkState::Error, nullptr, nullptr, r.err);
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
                      uint32_t manifestCrc, bool sentFull, const std::string& ackBatch,
                      HttpReply& reply, CheckinReply& parsed, bool autoapply,
                      bool followUp, Result& r) {
    for (;;) {
        if (!postCheckin(s, body, reply)) {
            s.reportState = reportAfterCheckin(s.reportState, manifestCrc, sentFull, 0, false,
                                                ackBatch.c_str());
            setError(r, followUp ? "ack-transport" : "checkin-transport");
            return Step::Error;
        }
        const bool readable = parseCheckin(reply.status == 200 ? reply.body.c_str() : nullptr,
                                           reply.nextMs, parsed);
        if (reply.status >= 200 && reply.status < 300 && readable && !parsed.hasServerTime &&
            time(nullptr) <= 1577836800) {
            const uint32_t fromDate = httpDateEpoch(reply.date.c_str());
            if (fromDate) {
                timeval tv = {(time_t)fromDate, 0};
                settimeofday(&tv, nullptr);
            }
        }
        s.reportState = reportAfterCheckin(s.reportState, manifestCrc, sentFull,
                                            reply.status, readable && parsed.sendReport,
                                            ackBatch.c_str());
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

// ---- Dev mode listening: state shared with the loop task --------------------
// The worker writes, the loop task reads a copy (devSnapshot()).
portMUX_TYPE devLock = portMUX_INITIALIZER_UNLOCKED;
DevSnapshot devView;
std::atomic<bool> devWake{false};
std::atomic<bool> devManualRefresh{false};
// Inside a check-in (not waiting between them): the store may change.
std::atomic<bool> devInCycle{false};
#ifdef CF_TEST_CLI
char devTlsUrl[160] = {0};   // bench: an extra public HTTPS GET per poll
std::atomic<uint32_t> devStallMs{0};   // bench: one stuck call
#endif
// Worker only.
uint32_t devRecordedAt = 0;
bool devOfferRead = false;
uint32_t devOfferAt = 0;
constexpr uint32_t kDevRecordEveryMs = 600000;
constexpr uint32_t kDevOfferEveryMs = 3600000;
// Rejoin: wait this long for the station to reconnect by itself first.
constexpr uint32_t kDevReconnectMs = 5000;

bool devRecordDue() {
    if (devRecordedAt && millis() - devRecordedAt < kDevRecordEveryMs) return false;
    devRecordedAt = millis();
    return true;
}

bool devOfferDue() {
    if (devOfferRead && millis() - devOfferAt < kDevOfferEveryMs) return false;
    devOfferRead = true;
    devOfferAt = millis();
    return true;
}

void devPublishDelivery(const char* batchId) {
    portENTER_CRITICAL(&devLock);
    ++devView.deliveries;
    strncpy(devView.lastBatch, batchId, sizeof(devView.lastBatch) - 1);
    devView.lastBatch[sizeof(devView.lastBatch) - 1] = '\0';
    portEXIT_CRITICAL(&devLock);
    Serial.printf("[dev] delivered batch=%s at_ms=%lu\n", batchId, (unsigned long)millis());
}

// One check-in and what follows from it: the offer, the blobs, the apply
// and the follow-up that answers it. A plain session runs it once; dev mode
// listening runs it at the site's pace. `out` carries the first check-in's
// HTTP status and Retry-After for the dev mode pacing.
struct CycleOut {
    int status = 0;
    int retry = 0;
    bool modeNotTaken = false;   // a 200 naming another mode than the one sent
};
void checkinCycle(Session& s, bool autoapply, const String& account, String& linkedAt,
                  Result& r, CycleOut& out) {
    const bool dev = sessionReason == Reason::Dev;
    CloudPlanner plan;
    plan.start(true);
    // ---- 1. check-in -------------------------------------------------
    uint32_t manifestCrc = 0;
    bool sentFull = false;
    std::string ackBatch;
    std::string body = checkinBody(s, nullptr, nullptr, manifestCrc, sentFull, ackBatch);
    if (body.empty()) { setError(r, "checkin-body"); return; }
    HttpReply check;
    Step step = checkinWithRetry(s, plan, body, manifestCrc, sentFull, ackBatch,
                                 check, s.reply, autoapply, false, r);
    out.status = check.status;
    out.retry = check.retry;
    out.modeNotTaken = devModeNotTaken(check.status, s.reply.mode, s.mode, s.reply.nextPollMs);
    r.nextMs = plan.nextMs();
    if (step == Step::Error || step == Step::Deferred) return;
    // A loadout/firmware offer can allocate large internal blocks or start
    // an app. Do not carry an idle TLS client into that work.
    if (s.checkinConnection && (s.reply.hasBatch || s.reply.firmwareOffer))
        s.checkinConnection->clear();
    // Dev mode checks in every few seconds: the check-in time is stored
    // (a flash write) only when the site sends its clock, or every 10 min.
    if (!dev || s.reply.serverEpoch || devRecordDue()) recordCheckIn(r, s.reply.serverEpoch);
    if (s.reply.serverEpoch && linkedAt == "0") {
        linkedAt = String(s.reply.serverEpoch);
        setSessionFingerprint(s, account.c_str(), linkedAt.c_str());
    }
    if (s.reply.firmwareOffer) {
        // Only an offer: the update lane compares the manifest and asks.
        strcpy(r.offered, "fw");
        Preferences offered;
        if (offered.begin("upd", false)) {
            offered.putString("fw_url", s.reply.firmwareUrl.c_str());
            offered.end();
        }
    }
    // A quiet check-in may be 204 with no offer. Manual and stale scheduled
    // checks still read the update site, within the same deadline reserve.
    const time_t clock = time(nullptr);
    const uint32_t now = clock > 0 && (uint64_t)clock <= UINT32_MAX ? (uint32_t)clock : 0;
    uint32_t lastManifest = 0;
    Preferences manifestStamp;
    if (manifestStamp.begin("upd", true)) {
        lastManifest = manifestStamp.getUInt(CheckinPolicy::kKeyManifestAt, 0);
        manifestStamp.end();
    }
    const bool manualDev = dev && devManualRefresh.exchange(false);
    const CheckinPolicy::ManifestSession manifestSession = manualDev ? CheckinPolicy::ManifestSession::Manual :
        dev ? CheckinPolicy::ManifestSession::Dev :
        (sessionReason == Reason::Manual || sessionReason == Reason::Recovery)
            ? CheckinPolicy::ManifestSession::Manual : CheckinPolicy::ManifestSession::Scheduled;
    const bool budget = budgetCovers(kCallMs, s.elapsed(), s.limitMs, kReserveMs);
    if (CheckinPolicy::manifestRefreshDue(manifestSession, s.reply.firmwareOffer, now,
                                          lastManifest, budget) &&
        (!dev || manualDev || (!s.reply.hasBatch && devOfferDue()))) {
        if (s.checkinConnection) s.checkinConnection->clear();
        const bool answered = UpdateSession::refreshOffer(s.started + s.limitMs - kReserveMs);
        if (answered && CheckinPolicy::clockPlausible(now) && manifestStamp.begin("upd", false)) {
            manifestStamp.putUInt(CheckinPolicy::kKeyManifestAt, now);
            manifestStamp.end();
        }
    }
    if (step == Step::Done) { r.ok = true; r.none = true; return; }
    if (step == Step::Waiting) { r.ok = true; r.none = true; r.waiting = true; return; }
    if (step != Step::Loadout) { setError(r, "planner"); return; }
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
    if (step == Step::Deferred) return;
    if (step == Step::Done) { r.ok = true; r.none = true; return; }
    if (step == Step::Error) {
        if (strcmp(r.err, "none") == 0)
            setError(r, offerError == OfferError::Json ? "offer-body" : "offer-http");
        return;
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
        if (step == Step::Error) return;
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
    if (step != Step::Ack || !answer) { setError(r, "planner"); return; }
    // Dev mode: the menu and a running copy of the app follow the committed
    // apply at once; the follow-up report does not hold them up.
    if (dev && appliedOk && r.appliedNow) devPublishDelivery(batchId.c_str());
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
        return;
    }
    if (!sleepFor(s, wait)) {
        setError(r, cancelRequested ? "cancelled" : "deadline");
        r.ok = false;
        return;
    }
    body = checkinBody(s, batchId.c_str(), answer, manifestCrc, sentFull, ackBatch);
    if (body.empty()) { setError(r, "ack-body"); r.ok = false; return; }
    HttpReply follow;
    CheckinReply followParsed;
    const bool wasOk = r.ok;
    char answered[sizeof(r.err)];
    memcpy(answered, r.err, sizeof(answered));
    step = checkinWithRetry(s, plan, body, manifestCrc, sentFull, ackBatch,
                            follow, followParsed, autoapply, true, r);
    r.nextMs = plan.nextMs();
    if (step == Step::Done) {
        recordCheckIn(r, followParsed.serverEpoch);
    } else if (step == Step::Deferred) {
        // Same as a deferral before sending: the next session answers.
        setError(r, wasOk ? "report-deferred" : answered);
    } else {
        r.ok = false;
    }
}

// Device uploads use the same two binary files and hashes as browser pulls.
// The record window is deliberately small enough for a short daily session.
bool addUsageFile(cJSON* files, const char* role, const char* name,
                  const uint8_t* bytes, size_t size) {
    unsigned char digest[32];
    mbedtls_sha256_context shaContext;
    mbedtls_sha256_init(&shaContext);
    mbedtls_sha256_starts(&shaContext, 0);
    mbedtls_sha256_update(&shaContext, bytes, size);
    const bool hashed = mbedtls_sha256_finish(&shaContext, digest) == 0;
    mbedtls_sha256_free(&shaContext);
    if (!hashed) return false;
    char sha[65]; digestHex(digest, sha);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08x", (unsigned)SyncProtocol::crc32(bytes, size));
    std::vector<unsigned char> encoded(((size + 2) / 3) * 4 + 1);
    size_t written = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &written, bytes, size) != 0) return false;
    cJSON* file = cJSON_CreateObject();
    if (!file) return false;
    cJSON_AddStringToObject(file, "role", role);
    cJSON_AddStringToObject(file, "path", role[0] == 'r' ? "/apps/.diary/batdiary.bin" : "/apps/.diary/batstats.bin");
    cJSON_AddStringToObject(file, "local", name);
    cJSON_AddNumberToObject(file, "size", size);
    cJSON_AddStringToObject(file, "crc32", crc);
    cJSON_AddStringToObject(file, "sha256", sha);
    cJSON_AddStringToObject(file, "b64", (const char*)encoded.data());
    cJSON_AddItemToArray(files, file);
    return true;
}

void uploadDailyUsage(const Session& s, const Result& r) {
    // Automatic sessions only: a Fidget used every day checks in at start-up
    // and may never reach a timer (Daily) session.
    if ((sessionReason != Reason::Daily && sessionReason != Reason::Boot) ||
        !r.ok || s.serverUnlinked || s.serverWrongDevice) return;
    Preferences upd;
    if (!upd.begin("upd", true)) return;
    const bool enabled = upd.getBool("usage_share", false);
    const uint32_t last = upd.getUInt("usage_at", 0);
    const uint32_t acked = upd.getUInt("usage_seq", 0);
    upd.end();
    const time_t clockNow = time(nullptr);
    const uint32_t now = clockNow > 0 && clockNow <= UINT32_MAX ? (uint32_t)clockNow : 0;
    const uint32_t remaining = s.elapsed() < s.limitMs ? s.limitMs - s.elapsed() : 0;
    if (!BatteryDiary::uploadDue(enabled, s.token[0] != 0,
            CheckinPolicy::batteryEligible(dailyVbatMv, dailySocPct), true, r.ok, now, last, remaining)) return;

    // The whole ring (48 KiB, lands in PSRAM): the window sends the OLDEST
    // unsent records first, so a Fidget that was offline for weeks catches
    // up over a few daily uploads instead of losing its older records.
    std::vector<BatteryDiary::Record> records(BatteryDiary::kMaxRecords);
    uint8_t stats[38];
    uint32_t total = 0;
    const size_t count = BatteryDiary::readUploadSnapshot(records.data(), records.size(), &total, stats);
    if (!count) return;
    const BatteryDiary::UploadWindow window = BatteryDiary::uploadWindow(
        records.data(), count, acked, total > count);
    if (!window.count) return;
    std::vector<uint8_t> raw(window.count * BatteryDiary::kRecordSize);
    for (size_t i = 0; i < window.count; ++i)
        BatteryDiary::encode(records[window.first + i], raw.data() + i * BatteryDiary::kRecordSize);

    cJSON* bundle = cJSON_CreateObject();
    if (!bundle) return;
    cJSON_AddNumberToObject(bundle, "bundle_schema", 1);
    cJSON_AddNumberToObject(bundle, "record_schema", 1);
    cJSON_AddNumberToObject(bundle, "stats_schema", 1);
    cJSON_AddStringToObject(bundle, "device_id", s.id);
    cJSON* identity = cJSON_AddObjectToObject(bundle, "identity");
    char observed[18];
    snprintf(observed, sizeof(observed), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
             s.id[10], s.id[11], s.id[8], s.id[9], s.id[6], s.id[7],
             s.id[4], s.id[5], s.id[2], s.id[3], s.id[0], s.id[1]);
    cJSON_AddStringToObject(identity, "observed_mac", observed);
    cJSON_AddStringToObject(identity, "encoding", "info.mac-reversed");
    cJSON_AddStringToObject(identity, "canonical_order", "efuse base MAC as esptool read_mac prints it");
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&clockNow));
    cJSON_AddStringToObject(bundle, "pulled_at_utc", timestamp);
    cJSON_AddStringToObject(bundle, "source", "device");
    cJSON_AddStringToObject(bundle, "label", "");
    cJSON_AddStringToObject(bundle, "note", "");
    cJSON* producer = cJSON_AddObjectToObject(bundle, "producer");
    cJSON_AddStringToObject(producer, "name", "device");
    cJSON_AddStringToObject(producer, "version", "1");
    cJSON* capture = cJSON_AddObjectToObject(bundle, "capture");
    cJSON_AddBoolToObject(capture, "reset_on_open", false);
    cJSON_AddNumberToObject(capture, "alive_after_s", 0);
    cJSON_AddNullToObject(capture, "checkin_period_s");
    cJSON_AddStringToObject(capture, "cadence_source", "unknown");
    cJSON* device = cJSON_AddObjectToObject(bundle, "device");
    cJSON_AddStringToObject(device, "version", s.fw);
    cJSON* info = cJSON_AddObjectToObject(device, "info");
    cJSON_AddStringToObject(info, "fw", s.fw);
    cJSON* files = cJSON_AddArrayToObject(bundle, "files");
    const bool encoded = addUsageFile(files, "records", "batdiary.bin", raw.data(), raw.size()) &&
                         addUsageFile(files, "stats", "batstats.bin", stats, sizeof(stats));
    cJSON_AddBoolToObject(bundle, "complete", window.complete);
    cJSON_AddStringToObject(bundle, "consent", "device_setting");
    cJSON_AddNumberToObject(bundle, "disclosure_version", 1);
    char* printed = encoded ? cJSON_PrintUnformatted(bundle) : nullptr;
    cJSON_Delete(bundle);
    if (!printed) return;
    std::string body(printed);
    cJSON_free(printed);
    if (body.size() > 98304 || !budgetCovers(kCallMs, s.elapsed(), s.limitMs, kCallMs)) return;
    HttpReply reply;
    // Answered but not accepted: the attempt still starts the day-long wait
    // (the sequence stays, so the same records go next time).
    auto stampAttempt = [&]() {
        if (!BatteryDiary::uploadStartsWait(reply.status) || !upd.begin("upd", false)) return;
        upd.putUInt("usage_at", now);
        upd.end();
    };
    if (!request(s, s.base + "/api/device-usage.php", &body, reply, jsonSink, &reply) ||
        reply.status < 200 || reply.status >= 300) {
        stampAttempt();
        return;
    }
    cJSON* answer = cJSON_Parse(reply.body.c_str());
    const cJSON* acknowledged = answer ? cJSON_GetObjectItemCaseSensitive(answer, "acked_seq") : nullptr;
    const uint32_t newest = records[window.first + window.count - 1].seq;
    const bool accepted = cJSON_IsNumber(acknowledged) && acknowledged->valuedouble >= acked &&
                          acknowledged->valuedouble <= newest &&
                          acknowledged->valuedouble == (double)(uint32_t)acknowledged->valuedouble;
    const uint32_t newAck = accepted ? (uint32_t)acknowledged->valuedouble : acked;
    cJSON_Delete(answer);
    if (!accepted) {
        stampAttempt();
        return;
    }
    if (!upd.begin("upd", false)) return;
    upd.putUInt("usage_seq", newAck);
    upd.putUInt("usage_at", now);
    upd.end();
}

// Dev mode: sleeps in short steps until `ms` has passed. False when
// cancelled; returns early (true) when devPollNow() asks for a check now.
bool devSleep(uint32_t ms) {
    const uint32_t since = millis();
    while (millis() - since < ms) {
        if (cancelRequested) return false;
        if (devWake.exchange(false)) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !cancelRequested;
}

// Dev mode: back on the saved network. The station usually reconnects by
// itself; after kDevReconnectMs it is asked to join again.
bool devRejoin() {
    uint32_t at = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - at < kDevReconnectMs) {
        if (cancelRequested) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect(false);
    // A fresh join in the same order as any session's.
    SavedWifi::JoinOptions joinOpt;
    joinOpt.firstMs = kJoinMs;
    joinOpt.fallbackMs = kJoinMs;
    joinOpt.stop = [](void*) { return cancelRequested.load(); };
    SavedWifi::JoinResult joined;
    return SavedWifi::join(joinOpt, joined);
}

#ifdef CF_TEST_CLI
bool discardSink(void*, const uint8_t*, size_t) { return true; }

// Bench: one public HTTPS GET in the listening context, to measure the
// internal heap under a TLS handshake while an app runs.
void devTlsProbe() {
    char url[sizeof(devTlsUrl)];
    portENTER_CRITICAL(&devLock);
    memcpy(url, devTlsUrl, sizeof(url));
    portEXIT_CRITICAL(&devLock);
    if (!url[0]) return;
    const size_t bootLowBefore = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t at = millis();
    FetchReply reply;
    const bool ok = fetchPublic(url, kCallMs, millis() + 8000, discardSink, nullptr, reply);
    Serial.printf("[dev] tls ok=%d http=%d bytes=%u ms=%u free=%u largest=%u "
                  "min_boot_before=%u min_boot=%u\n",
                  ok ? 1 : 0, reply.status, (unsigned)reply.received, (unsigned)(millis() - at),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)bootLowBefore,
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
#endif

// Dev mode listening: stays joined and checks in at the site's pace until
// cancelled (the mode is turned off, a radio app starts, or the Fidget
// restarts). Deliveries apply whatever "Apply app changes automatically"
// says: sending an app is the person asking for it. `total` is the
// worker's result when it ends.
void devLoop(Session& s, const String& account, String& linkedAt, Result& total,
             uint32_t firstWaitMs) {
    HttpConnection checkinConnection;
    s.checkinConnection = &checkinConnection;
    uint8_t failures = 0;
    uint32_t polls = 0;
    devOfferRead = false;
    devRecordedAt = 0;
    if (firstWaitMs) {
        // A backoff the site asked for is still running.
        if (firstWaitMs > kDevMaxRetryMs) firstWaitMs = kDevMaxRetryMs;
        if (!devSleep(firstWaitMs)) { s.checkinConnection = nullptr; return; }
    }
    for (;;) {
        if (cancelRequested) break;
        // Claim the store before looking at the serial side: from here a
        // new serial transfer is refused (storeBusy), and one already open
        // makes this poll skip.
        devInCycle = true;
        if (SerialCli::instance().ferryActive()) {
            checkinConnection.clear();
            devInCycle = false;
            if (!devSleep(kDevPollMs)) break;
            continue;
        }
        Result r;
        r.reason = Reason::Dev;
        CycleOut out;
        heapLow = SIZE_MAX;
        largestLow = SIZE_MAX;
        rootsRejected = false;
        sampleHeap();
        bool connected = WiFi.status() == WL_CONNECTED;
        const uint32_t cycleAt = millis();
        if (!connected) checkinConnection.clear();
        if (!connected) connected = devRejoin();
        if (cancelRequested) { devInCycle = false; break; }
        // A rejoin can take seconds: a transfer that opened meanwhile wins.
        if (SerialCli::instance().ferryActive()) {
            checkinConnection.clear();
            devInCycle = false;
            if (!devSleep(kDevPollMs)) break;
            continue;
        }
        if (!connected) {
            devInCycle = false;
            setError(r, "join");
        } else {
            s.started = millis();
            s.limitMs = kSessionMs;
            s.mayWait = true;
#ifdef CF_TEST_CLI
            const uint32_t stall = devStallMs.exchange(0);
            if (stall) {
                Serial.printf("[dev] stall ms=%u\n", (unsigned)stall);
                vTaskDelay(pdMS_TO_TICKS(stall));
            }
#endif
            checkinCycle(s, true, account, linkedAt, r, out);
            devInCycle = false;
            if (!r.ok && cancelRequested) setError(r, "cancelled");
            if (rootsRejected) { r.ok = false; setError(r, "roots-parse"); }
        }
        sampleHeap();
        r.heapMin = heapLow == SIZE_MAX ? 0 : (uint32_t)heapLow;
        r.largestMin = largestLow == SIZE_MAX ? 0 : (uint32_t)largestLow;
        r.totalMs = millis() - cycleAt;
        r.unlinkedNotice = s.serverUnlinked;
        r.mismatchNotice = s.serverWrongDevice;
        ++polls;
        const uint8_t failedBefore = failures;
        failures = r.ok ? 0 : (failures < 250 ? failures + 1 : failures);
        const uint32_t wait = devPollWaitMs(r.nextMs, failures, out.retry, esp_random(),
                                            out.modeNotTaken);
        // Apache's idle keep-alive is normally about 5 s. A long wait only
        // holds heap and is likely to meet a server-closed socket.
        if (!keepDevConnection(out.status, r.ok, wait,
                               WiFi.status() == WL_CONNECTED, cancelRequested))
            checkinConnection.clear();
        portENTER_CRITICAL(&devLock);
        devView.connected = WiFi.status() == WL_CONNECTED;
        devView.polls = polls;
        devView.failures = failures;
        devView.lastPollMs = millis();
        devView.last = r;
        if (r.heapMin && (devView.heapMin == 0 || r.heapMin < devView.heapMin))
            devView.heapMin = r.heapMin;
        if (r.largestMin && (devView.largestMin == 0 || r.largestMin < devView.largestMin))
            devView.largestMin = r.largestMin;
        portEXIT_CRITICAL(&devLock);
#ifdef CF_TEST_CLI
        Serial.printf("[dev] poll=%u http=%d result=%s err=%s ms=%u wait_ms=%u heap_min=%u "
                      "largest_min=%u free=%u at_ms=%lu\n",
                      (unsigned)polls, out.status, r.ok ? (r.none ? "none" : "ok") : "error", r.err,
                      (unsigned)r.totalMs, (unsigned)wait, (unsigned)r.heapMin,
                      (unsigned)r.largestMin,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned long)millis());
        devTlsProbe();
#else
        // Release builds print only the changes.
        if (failures == 1) Serial.printf("[dev] poll failing err=%s\n", r.err);
        if (failures == 0 && failedBefore) Serial.println("[dev] poll ok again");
#endif
        (void)failedBefore;
        total = r;
        if (s.serverUnlinked || s.serverWrongDevice) break;   // the link is gone
        if (!devSleep(wait)) break;
    }
    checkinConnection.clear();
    s.checkinConnection = nullptr;
    total.reason = Reason::Dev;
    if (cancelRequested) { total.ok = true; total.none = true; setError(total, "cancelled"); }
}

// The whole session. Every object with heap storage lives in this frame and
// is destroyed on return, before the task deletes itself.
void runWorker(Result& r) {
    CloudPlanner plan;
    plan.start(true);
    Session s;
    s.started = millis();
    // Scheduled sessions defer a report instead of holding WiFi on to wait
    // out the server's spacing, and have a shorter budget.
    const bool automatic = scheduled(sessionReason);
    s.mayWait = !automatic;
    s.limitMs = automatic ? kScheduledSessionMs : kSessionMs;
    r.reason = sessionReason;
    heapLow = SIZE_MAX;
    largestLow = SIZE_MAX;
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
        if (!DeviceIdentity::checkStored()) { setError(r, "link-mismatch"); break; }
        Preferences pair;
        if (!pair.begin("pair", true)) { setError(r, "not-linked"); break; }
        if (pair.getBool("ok", false)) pair.getString("tok", s.token, sizeof(s.token));
        PrefsLinkStore linkStore(pair);
        const bool revokeOnly = !s.token[0] && revokeOnlySession(linkStore);
        String account = pair.getString("acct", "");
        String linkedAt = pair.getString("at", "");
        if (linkedAt.isEmpty()) linkedAt = String(pair.getUInt("at", 0));
        pair.end();
        if (!s.token[0] && !revokeOnly) { setError(r, "not-linked"); break; }
        if (!revokeOnly) setSessionFingerprint(s, account.c_str(), linkedAt.c_str());

        if (!SavedWifi::anySaved()) { setError(r, "no-wifi"); break; }
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
            AwakePolicy::Stored awake;
            awake.hasMode = upd.isKey(AwakePolicy::kKeyMode);
            awake.mode = awake.hasMode ? upd.getUChar(AwakePolicy::kKeyMode, 0) : 0;
            awake.hasStop = upd.isKey(AwakePolicy::kKeyStop);
            awake.stop = awake.hasStop ? upd.getUChar(AwakePolicy::kKeyStop, 0) : 0;
            awake.hasLegacyDev = upd.isKey(AwakePolicy::kLegacyKeyDev);
            awake.legacyDev = awake.hasLegacyDev ? upd.getUChar(AwakePolicy::kLegacyKeyDev, 0) : 0;
            s.mode = AwakePolicy::wireMode(AwakePolicy::parseStored(awake).setting);
            const uint32_t until = upd.getUInt("backoff_to", 0);
            const time_t now = time(nullptr);
            if (until && now > 1577836800 && (uint32_t)now < until) {
                r.nextMs = (until - (uint32_t)now) * 1000;
                backedOff = true;
            }
            upd.end();
        }
        autoapply = PromptPolicy::appBatch(autoapply, sessionApplyOnce) == PromptPolicy::AppBatch::Apply;
        // Dev mode waits a running backoff out inside its loop instead.
        const bool dev = sessionReason == Reason::Dev;
        if (backedOff && !revokeOnly && !dev) { setError(r, "backoff"); break; }
        if (!validBase(s.base.c_str())) { setError(r, "invalid-base"); break; }
        if (WiFi.getMode() != WIFI_OFF) { setError(r, "radio-busy"); break; }
        WiFi.persistent(false);
        radioStarted = true;
        radioUsed = true;
        if (!WiFi.mode(WIFI_STA)) { setError(r, "sta-mode"); break; }
        // The last network that worked first, at its remembered place; then
        // one scan and the strongest saved network present (SavedWifi). The
        // connect's own scan reports a saved network that is not in range; a
        // scheduled session stops there instead of running out its join
        // budget. A manual check keeps trying.
        const uint32_t joinLimit = sessionReason == Reason::Boot ? kBootJoinMs : kJoinMs;
        SavedWifi::JoinOptions joinOpt;
        joinOpt.firstMs = joinLimit;
        joinOpt.fallbackMs = joinLimit;
        joinOpt.scheduled = automatic;
        joinOpt.stop = sessionStop;
        joinOpt.ctx = &s;
#ifdef CF_TEST_CLI
        {
            // Bench: a network name that is not in range, so the absent-
            // network bail can be measured without touching saved settings.
            Preferences test;
            if (test.begin("cftest", true)) {
                if (test.getBool("badssid", false)) joinOpt.benchFirstName = "cf-bench-absent-network";
                test.end();
            }
        }
#endif
        SavedWifi::JoinResult joined;
        SavedWifi::join(joinOpt, joined);
        sampleHeap();
        // Dev mode keeps trying to join inside its loop.
        if (WiFi.status() != WL_CONNECTED && (!dev || cancelRequested || revokeOnly)) {
            setError(r, cancelRequested ? "cancelled" : joined.absent ? "no-network" : "join");
            break;
        }
        if (WiFi.status() == WL_CONNECTED) r.joinMs = joined.totalMs;

        const DeviceIdentity::Fingerprint live = DeviceIdentity::readLive();
        strcpy(s.id, live.id);
        strcpy(s.flashId, live.flashId);
        strcpy(s.serial, live.serial);
        s.fw = getFirmwareVersionString();
        if (revokeOnly) {
            drainRevoke(s);
            Preferences remaining;
            if (!remaining.begin("pair", true)) { setError(r, "storage"); break; }
            PrefsLinkStore remainingStore(remaining);
            const bool pending = hasPendingRevoke(remainingStore);
            remaining.end();
            if (pending) setError(r, "try-again");
            else { r.ok = true; r.none = true; }
            break;
        }
        snprintf(s.abi, sizeof(s.abi), "%d", kDeviceHalAbi);
        const BoardInfo::Info& info = HAL::boardInfo();
        snprintf(s.board, sizeof(s.board), "%u.%u", info.major, info.minor);
        LoadoutStore::begin();
        s.query = std::string("device_id=") + s.id + "&lapply_cap=" + SyncProtocol::kLapplyCapability;
        if (s.flashId[0]) s.query += std::string("&flash_id=") + s.flashId;
        if (s.serial[0]) s.query += std::string("&serial=") + s.serial;
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
        drainRevoke(s);

        if (dev) {
            const uint32_t firstWait = backedOff ? r.nextMs : 0;
            devLoop(s, account, linkedAt, r, firstWait);
            break;
        }
        CycleOut out;
        checkinCycle(s, autoapply, account, linkedAt, r, out);
        uploadDailyUsage(s, r);
    } while (false);
    // A cancelled request surfaces as a transport error; name the cause.
    if (!r.ok && cancelRequested) setError(r, "cancelled");
    // Reported over the transport error the failed request left behind.
    if (rootsRejected) { r.ok = false; setError(r, "roots-parse"); }

    bool radioOff = true;
    if (radioStarted) {
        WiFi.disconnect(true);
        radioOff = WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_OFF;
        if (!radioOff) { r.ok = false; setError(r, "wifi-off"); }
    }
    memset(s.token, 0, sizeof(s.token));
    sampleHeap();
    // The handshake trough falls inside esp_http_client_open() where no
    // sample runs; a drop in the since-boot low-water mark is ours.
    const size_t bootLowAfter = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (bootLowAfter < bootLowBefore && bootLowAfter < heapLow) heapLow = bootLowAfter;
    r.heapMin = (uint32_t)heapLow;
    r.unlinkedNotice = s.serverUnlinked;
    r.mismatchNotice = s.serverWrongDevice;
    r.largestMin = largestLow == SIZE_MAX ? 0 : (uint32_t)largestLow;
    r.totalMs = s.elapsed();
    if (!r.ok && strcmp(r.err, "none") == 0 && s.elapsed() >= s.limitMs) setError(r, "deadline");
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
        if (workerKind == WorkerKind::Cloud) runWorker(r);
        else runPairWorker(r);
        const bool mismatch = DeviceIdentity::takeMismatchNotice();
        r.mismatchNotice = r.mismatchNotice || mismatch;
        result = r;
        if (workerKind == WorkerKind::Cloud) {
            // One read-only timing line per check-in, written before the
            // session counts as finished, so it precedes anything that
            // waited for this session to end (such as Bluetooth start-up).
            Serial.printf("[checkin] reason=%s join_ms=%u total_ms=%u result=%s err=%s "
                          "heap_min=%u largest_min=%u wifi=%s\n",
                          reasonName(r.reason), (unsigned)r.joinMs, (unsigned)r.totalMs,
                          r.ok ? (r.none ? "none" : "ok") : "error", r.err,
                          (unsigned)r.heapMin, (unsigned)r.largestMin,
                          WiFi.getMode() == WIFI_OFF ? "off" : "on");
        }
    }
    finished = true;
    running = false;
    vTaskDelete(nullptr);
}
} // namespace

const char* reasonName(Reason reason) {
    switch (reason) {
        case Reason::Boot:     return "boot";
        case Reason::Daily:    return "daily";
        case Reason::Manual:   return "manual";
        case Reason::Dev:      return "dev";
        case Reason::Recovery: return "recovery";
        case Reason::Awake:    return "awake";
    }
    return "?";
}

bool runSession(Reason reason, bool applyWaiting, int32_t dailyVbat, int32_t dailySoc) {
    if (UpdateSession::imagePending()) return false;
    if (running || finished || SerialCli::instance().ferryActive() ||
        SerialCli::instance().radioBusy()) return false;
    recoverClearBeforeSession();
    // Never under an app that owns a radio: it would share the power cycle.
    const AppIndex active = AppManager::instance().activeApp();
    if (active == APP_MUSIC_PLAYER || active == APP_WEB_PORTAL) return false;
    CloudPlanner entry;
    if (entry.start(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) == Step::Reboot) {
        // The check continues after the restart; "Get them now" goes with it.
        const PromptPolicy::RestartOneShot shot = PromptPolicy::restartForCheck(applyWaiting);
        Preferences prefs;
        if (!prefs.begin("bootcfg", false)) return false;
        // `bootcloud` is written last: it is what schedules the check, so a
        // failed earlier write never leaves a check armed for a later boot.
        bool saved = prefs.putBool("bootapply", shot.bootapply) != 0 &&
                     prefs.putBool("skipanim", shot.skipanim) != 0 &&
                     prefs.putBool("bootcloud", shot.bootcloud) != 0;
        if (!saved) {
            prefs.remove("bootcloud");
            prefs.remove("bootapply");
        }
        prefs.end();
        if (!saved) return false;
        esp_restart();
        return true;
    }
    if (WiFi.getMode() != WIFI_OFF) return false;
    workerKind = WorkerKind::Cloud;
    sessionReason = reason;
    dailyVbatMv = dailyVbat;
    dailySocPct = dailySoc;
    sessionApplyOnce = applyWaiting;
    cancelRequested = false;
    available = false;
    running = true;
    // Dev mode listening runs for as long as the mode is on: it is shown by
    // its own marker, not as a check.
    if (reason != Reason::Dev) {
        StatusService::instance().post(StatusKind::Checking, nullptr,
                StatusPriority::Normal, true, millis());
    } else {
        portENTER_CRITICAL(&devLock);
        devView = DevSnapshot();
        portEXIT_CRITICAL(&devLock);
        devWake = false;
        devManualRefresh = false;
        devInCycle = false;
    }
    if (xTaskCreate(worker, "cloudsync", kStackBytes, nullptr, 1, nullptr) != pdPASS) {
        running = false;
        StatusService::instance().clear(StatusKind::Checking);
        result = Result();
        setError(result, "task-create");
        result.heapMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        result.reason = reason;
        available = true;
        taskFailed = true;
        return true;
    }
    return true;
}

static bool beginPairWorker(WorkerKind kind) {
    if (UpdateSession::imagePending()) return false;
    if (running || finished || SerialCli::instance().ferryActive() ||
        SerialCli::instance().radioBusy()) return false;
    recoverClearBeforeSession();
    const AppIndex active = AppManager::instance().activeApp();
    if (active == APP_MUSIC_PLAYER || active == APP_WEB_PORTAL) return false;
    CloudPlanner entry;
    if (entry.start(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) == Step::Reboot) {
        Preferences boot;
        if (!boot.begin("bootcfg", false)) return false;
        const bool saved = boot.putBool(kind == WorkerKind::Link ? "bootlink" : "bootunlink", true) != 0 &&
                           boot.putBool("skipanim", true) != 0;
        boot.end();
        if (!saved) return false;
        esp_restart();
        return true;
    }
    if (WiFi.getMode() != WIFI_OFF) return false;
    workerKind = kind;
    cancelRequested = false;
    linkChoice = -1;
    available = false;
    running = true;
    publishLink(LinkState::Starting, "", "", "");
    if (xTaskCreate(worker, "devicelink", kStackBytes, nullptr, 1, nullptr) != pdPASS) {
        running = false;
        publishLink(LinkState::Error, nullptr, nullptr, "task-create");
    }
    return true;
}

bool startLink() { return beginPairWorker(WorkerKind::Link); }
bool startUnlink() { return beginPairWorker(WorkerKind::Unlink); }
void answerLink(bool accept) { linkChoice = accept ? 1 : 0; }
void answerClearApps(bool clear) { linkChoice = clear ? 2 : 3; }
LinkSnapshot linkSnapshot() {
    portENTER_CRITICAL(&linkViewLock);
    LinkSnapshot copy = linkView;
    portEXIT_CRITICAL(&linkViewLock);
    return copy;
}
bool linked(char account[40]) {
    if (!DeviceIdentity::checkStored()) { account[0] = '\0'; return false; }
    Preferences pair;
    if (!pair.begin("pair", true)) { account[0] = '\0'; return false; }
    PrefsLinkStore store(pair);
    LinkRecord record;
    const bool has = readLink(store, record);
    strncpy(account, record.account.c_str(), 39);
    account[39] = '\0';
    pair.end();
    return has;
}

bool linkStatus(char account[40], bool& fingerprint) {
    fingerprint = DeviceIdentity::checkStored(false);
    Preferences pair;
    if (!pair.begin("pair", true)) { account[0] = '\0'; return false; }
    PrefsLinkStore store(pair);
    LinkRecord record;
    const bool has = fingerprint && readLink(store, record);
    strncpy(account, record.account.c_str(), 39);
    account[39] = '\0';
    pair.end();
    return has;
}

bool hadPreviousAccount() {
    Preferences pair;
    if (!pair.begin("pair", true)) return false;
    const bool had = pair.isKey("prev_aref") || pair.isKey("prev_acct");
    pair.end();
    return had;
}

void resetLinkStatus() {
    if (!running) publishLink(LinkState::Idle, "", "", "");
}

bool poll() {
    if (taskFailed) {
        // A session that could not start still finished (with an error).
        taskFailed = false;
        return true;
    }
    if (!running && DeviceIdentity::takeMismatchNotice())
        StatusService::instance().post(StatusKind::Warning,
            "This Fidget's link came from another Fidget. Link it again.",
            StatusPriority::High, true, millis());
    DeviceLinkApp::closeStalePrompt();
    if (!finished) return false;
    finished = false;
    available = workerKind == WorkerKind::Cloud;
    if (result.mismatchNotice)
        StatusService::instance().post(StatusKind::Warning,
            "This Fidget's link came from another Fidget. Link it again.",
            StatusPriority::High, true, millis());
    if (result.unlinkedNotice) {
        publishLink(LinkState::Unlinked);
        DeviceLinkApp::refreshStoredLink();
        StatusService::instance().post(StatusKind::Info, "Unlinked",
            StatusPriority::Normal, false, millis());
    }
    if (workerKind != WorkerKind::Cloud) {
        if (result.manifestChanged) MenuManager::instance().markManifestDirty();
        if (result.appsClearFailed)
            StatusService::instance().post(StatusKind::Warning,
                "Linked, but could not clear the previous apps.",
                StatusPriority::High, true, millis());
        return false;
    }
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
    return true;
}

const Result& lastResult() { return result; }

void recoverFailure() {
    recoverClearBeforeSession();
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
void requestCancel() { cancelRequested = true; }
bool busy() { return running; }
bool automaticSessionRunning() {
    return running && workerKind == WorkerKind::Cloud && sessionReason != Reason::Manual &&
           sessionReason != Reason::Dev;
}
bool storeBusy() {
    if (!running) return false;
    return workerKind != WorkerKind::Cloud || sessionReason != Reason::Dev || devInCycle;
}
bool radioUsedThisPowerCycle() { return radioUsed; }

bool devListening() {
    return running && workerKind == WorkerKind::Cloud && sessionReason == Reason::Dev;
}

DevSnapshot devSnapshot() {
    portENTER_CRITICAL(&devLock);
    DevSnapshot copy = devView;
    portEXIT_CRITICAL(&devLock);
    return copy;
}

void devPollNow() { devManualRefresh = true; devWake = true; }

// A full update URL (the site plus a path and query): longer than a site
// base, same scheme rules.
static bool validFetchUrl(const char* url) {
    if (strlen(url) > 600 || strpbrk(url, " \t\r\n#@")) return false;
    if (strncmp(url, "https://", 8) == 0) return url[8] != '\0';
#ifdef CF_TEST_CLI
    if (strncmp(url, "http://", 7) == 0) return url[7] != '\0';
#endif
    return false;
}

bool useExternalTlsMemory() {
    return mbedtls_platform_set_calloc_free(tlsPsramCalloc, heap_caps_free) == 0;
}

bool siteBase(char* out, size_t len) {
    char base[128];
    strcpy(base, kDefaultBase);
#ifdef CF_TEST_CLI
    Preferences upd;
    if (upd.begin("upd", true)) {
        if (upd.isKey("base")) upd.getString("base", base, sizeof(base));
        upd.end();
    }
#endif
    const size_t n = strlen(base);
    if (!validBase(base) || n >= len) return false;
    memcpy(out, base, n + 1);
    return true;
}

bool fetchPublic(const char* url, uint32_t callTimeoutMs, uint32_t deadlineMs,
                 ChunkSink sink, void* arg, FetchReply& reply) {
    reply = FetchReply();
    if (!url || !validFetchUrl(url) || !sink) return false;
    char* roots = TrustedRoots::newPem();
    if (!roots) return false;
    // A partial parse would trust fewer roots: never connect on one.
    if (!TrustedRoots::pemParsesCompletely(roots)) {
        TrustedRoots::freePem(roots);
        return false;
    }
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.cert_pem = roots;
    config.disable_auto_redirect = true;
    config.timeout_ms = (int)callTimeoutMs;
    config.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { TrustedRoots::freePem(roots); return false; }
    // PSRAM: this buffer must not come out of the internal heap.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = buf && esp_http_client_open(client, 0) == ESP_OK;
    if (ok) {
        reply.length = esp_http_client_fetch_headers(client);
        ok = reply.length >= 0;
    }
    if (ok) {
        reply.status = esp_http_client_get_status_code(client);
        while ((int32_t)(millis() - deadlineMs) < 0) {
            const int n = esp_http_client_read(client, (char*)buf, 4096);
            if (n < 0) { ok = false; break; }
            if (n == 0) {
                reply.complete = esp_http_client_is_complete_data_received(client);
                break;
            }
            reply.received += (uint32_t)n;
            if (!sink(arg, buf, (size_t)n)) { ok = false; break; }
        }
        if (!reply.complete) ok = false;
    }
    heap_caps_free(buf);
    esp_http_client_cleanup(client);
    TrustedRoots::freePem(roots);
    return ok;
}

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
    PrefsLinkStore store(prefs);
    const bool ok = replaceTestToken(store, token);
    prefs.end(); return ok;
}
// Bench reruns: drops the link, the previous-account memory and any pending
// revokes without contacting the server.
bool forgetLink() {
    if (running) return false;
    Preferences prefs;
    if (!prefs.begin("pair", false)) return false;
    const bool ok = prefs.clear();
    prefs.end();
    if (ok) publishLink(LinkState::Idle, "", "", "");
    return ok;
}
bool setAutoapply(bool enabled) {
    if (running) return false;
    Preferences prefs;
    if (!prefs.begin("upd", false)) return false;
    const bool ok = prefs.putBool("autoapply", enabled) != 0;
    prefs.end(); return ok;
}
bool setDevStallMs(uint32_t ms) {
    devStallMs = ms;
    return true;
}
bool setDevTlsProbe(const char* url) {
    if (!url || strlen(url) >= sizeof(devTlsUrl)) return false;
    if (url[0] && strncmp(url, "https://", 8) != 0) return false;
    portENTER_CRITICAL(&devLock);
    strcpy(devTlsUrl, url);
    portEXIT_CRITICAL(&devLock);
    return true;
}
bool setAbsentSsidTest(bool enabled) {
    if (running) return false;
    Preferences prefs;
    if (!prefs.begin("cftest", false)) return false;
    const bool ok = enabled ? prefs.putBool("badssid", true) != 0
                            : (!prefs.isKey("badssid") || prefs.remove("badssid"));
    prefs.end(); return ok;
}
#endif
} // namespace CloudSync
#endif // HOST_TEST
