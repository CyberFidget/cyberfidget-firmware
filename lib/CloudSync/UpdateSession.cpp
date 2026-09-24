// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "UpdateSession.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "CloudSync.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "OtaUpdate.h"
#include "StatusService.h"
#include "globals.h"

namespace UpdateSession {
namespace {

using OtaUpdate::Pending;

// Time limits and keys are shared with the native tests (OtaUpdate.h).
using OtaUpdate::kHalBudgetMs;
using OtaUpdate::kSelfTestBudgetMs;
using OtaUpdate::kConfirmBudgetMs;
using OtaUpdate::kPendingWdtMs;
using OtaUpdate::kDefaultWdtMs;
using OtaUpdate::kSessionBudgetMs;
using OtaUpdate::kSessionWdtMs;
using OtaUpdate::kBootFailed;
using OtaUpdate::kBootVersion;
constexpr uint32_t kCallMs = OtaUpdate::kSessionCallMs;
constexpr uint32_t kOfferCallMs = 4000;
constexpr uint32_t kJoinMs = 20000;
constexpr size_t kManifestMax = 4096;
constexpr int kFailScreenMs = 4000;

constexpr const char* kBootNs = "bootcfg";
constexpr const char* kBootKey = OtaUpdate::kBootSession;
constexpr const char* kUpdNs = "upd";

constexpr const char* kDidNotFinish = "The update did not finish. Nothing changed.";

bool pending = false;
bool watched = false;       // the loop task is subscribed to the task watchdog
bool awaitingFrame = false; // checks passed; kept once the first frame is drawn
uint32_t selfTestStart = 0;
Pending confirmed;          // the record of the image waiting for its first frame
char fault[16] = {0};       // test builds: the injected fault, "" = none

bool setWatchdogPeriod(uint32_t ms) {
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = ms;
    wdt.idle_core_mask = 1u << 0;   // as the SDK configures it: CPU0's idle task
    wdt.trigger_panic = true;
    return esp_task_wdt_reconfigure(&wdt) == ESP_OK;
}

const char* stateName(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}

const char* partState(const esp_partition_t* p) {
    esp_ota_img_states_t s;
    if (!p || esp_ota_get_state_partition(p, &s) != ESP_OK) return "undefined";
    return stateName(s);
}

bool isFault(const char* name) {
#ifdef CF_TEST_CLI
    return strcmp(fault, name) == 0;
#else
    (void)name;
    return false;
#endif
}

// Test builds: read and remove the one-shot fault meant for this stage (the
// update session takes only "session-hang"; the pending image takes the
// rest), so a rollback never meets it again.
void takeFault(bool session) {
#ifdef CF_TEST_CLI
    Preferences test;
    if (!test.begin("cftest", false)) return;
    if (test.isKey(OtaUpdate::kTestFault)) {
        char stored[16] = {0};
        test.getString(OtaUpdate::kTestFault, stored, sizeof(stored));
        if ((strcmp(stored, "session-hang") == 0) == session) {
            strcpy(fault, stored);
            test.remove(OtaUpdate::kTestFault);
        }
    }
    test.end();
#else
    (void)session;
#endif
}

bool readRecord(char* out, size_t len) {
    out[0] = '\0';
    Preferences upd;
    if (!upd.begin(kUpdNs, true)) return false;
    bool present = upd.isKey(OtaUpdate::kKeyPendImg);
    if (present) upd.getString(OtaUpdate::kKeyPendImg, out, len);
    upd.end();
    return present;
}

void clearRecord() {
    Preferences upd;
    if (!upd.begin(kUpdNs, false)) return;
    if (upd.isKey(OtaUpdate::kKeyPendImg)) upd.remove(OtaUpdate::kKeyPendImg);
    upd.end();
}

// Freshness per (source, channel) only moves forward.
void recordSeen(const Pending& p) {
    Preferences upd;
    if (!upd.begin(kUpdNs, false)) return;
    const uint32_t current = upd.isKey(p.seenKey) ? upd.getUInt(p.seenKey, 0) : 0;
    if (p.releasedAt > current) upd.putUInt(p.seenKey, p.releasedAt);
    upd.end();
}

void hexOf(const uint8_t digest[32], char out[65]) {
    static const char kDigits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kDigits[digest[i] >> 4];
        out[i * 2 + 1] = kDigits[digest[i] & 15];
    }
    out[64] = '\0';
}

// The running image's first `size` bytes hash to the record's SHA-256: the
// Fidget is running exactly the bytes that were checked when they arrived.
bool runningImageIs(const Pending& p) {
    const esp_partition_t* part = esp_ota_get_running_partition();
    if (!part || p.size == 0 || p.size > part->size) return false;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    if (!buf) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (uint32_t off = 0; off < p.size;) {
        const uint32_t n = (p.size - off) < 4096 ? (p.size - off) : 4096;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) { ok = false; break; }
        mbedtls_sha256_update(&ctx, buf, n);
        off += n;
        if (watched) esp_task_wdt_reset();
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    heap_caps_free(buf);
    if (!ok) return false;
    char hex[65];
    hexOf(digest, hex);
    return strcmp(hex, p.sha256) == 0;
}

void postNotice(const char* text, bool attention) {
    StatusService& svc = StatusService::instance();
    svc.post(StatusKind::Info, text, attention ? StatusPriority::High : StatusPriority::Normal,
             true, millis());
}

[[noreturn]] void rollBack(const char* reason) {
    Serial.printf("[update] rollback reason=%s\n", reason);
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only reached if the call failed: a reset while still pending makes the
    // bootloader return to the previous image anyway.
    esp_restart();
    for (;;) {}
}

void selfTest() {
    OtaUpdate::SelfTestInputs in;
    const uint32_t halMs = millis() - selfTestStart;
    in.halOk = halMs <= kHalBudgetMs;
    if (isFault("crash")) {
        Serial.println("[update] selftest fault=crash");
        Serial.flush();
        abort();
    }
    if (isFault("hang")) {
        // Stops feeding: the task watchdog must reset the Fidget.
        Serial.println("[update] selftest fault=hang");
        Serial.flush();
        for (;;) {}
    }
    char text[160];
    Pending p;
    in.recordValid = readRecord(text, sizeof(text)) && OtaUpdate::parsePending(text, p);
    // Never format here: a pending image that cannot mount the filesystem
    // fails its self-test and the previous image gets the data back as is.
    in.fsMounted = !isFault("mount") && LittleFS.begin(false);
    const char* running = getFirmwareVersionString();
    in.versionOk = in.recordValid && !isFault("version") && OtaUpdate::versionMatches(p.version, running);
    in.imageOk = in.recordValid && runningImageIs(p);
    const uint32_t totalMs = millis() - selfTestStart;
    in.inTime = totalMs <= kSelfTestBudgetMs;
    const OtaUpdate::SelfTestResult r = OtaUpdate::decideSelfTest(in);
    Serial.printf("[update] selftest=%s reason=%s hal_ms=%lu fs=%d version=%d image=%d ms=%lu "
                  "running=%s expected=%s\n",
                  r.markValid ? "pass" : "fail", r.reason, (unsigned long)halMs,
                  in.fsMounted ? 1 : 0, in.versionOk ? 1 : 0, in.imageOk ? 1 : 0,
                  (unsigned long)totalMs, running, in.recordValid ? p.version : "-");
    if (!r.markValid) rollBack(r.reason);
    // Not kept yet: the rest of setup and the first frame must also run
    // (loopTick), still under the watchdog and the wall-clock budget.
    confirmed = p;
    awaitingFrame = true;
}

// The image passed every check and drew its first frame: keep it.
void confirm() {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) rollBack("mark-valid");
    awaitingFrame = false;
    if (watched) {
        esp_task_wdt_delete(nullptr);
        watched = false;
    }
    setWatchdogPeriod(kDefaultWdtMs);
    clearRecord();
    recordSeen(confirmed);
    {
        Preferences upd;
        if (upd.begin(kUpdNs, false)) {
            if (upd.isKey(OtaUpdate::kKeyFailVer)) upd.remove(OtaUpdate::kKeyFailVer);
            upd.end();
        }
        Preferences boot;
        if (boot.begin(kBootNs, false)) {
            if (boot.isKey(kBootFailed)) boot.remove(kBootFailed);
            boot.end();
        }
    }
    char notice[64];
    snprintf(notice, sizeof(notice), "Updated to %s", confirmed.version);
    postNotice(notice, false);
    Serial.printf("[update] confirmed ms=%lu\n", (unsigned long)(millis() - selfTestStart));
    Serial.printf("[update] result=updated version=%s\n", confirmed.version);
}

// ---- update session ------------------------------------------------------------

DisplayProxy& screen() { return HAL::displayProxy(); }

void drawSession(const char* line, int percent) {
    DisplayProxy& d = screen();
    d.clear();
    d.setColor(WHITE);
    d.setFont(ArialMT_Plain_10);
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.drawString(64, 2, "Updating your Fidget...");
    d.drawString(64, 16, "Keep it charged");
    if (percent >= 0) {
        d.drawProgressBar(8, 34, 111, 10, (uint8_t)percent);
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        d.drawString(64, 48, pct);
    } else if (line) {
        d.drawString(64, 40, line);
    }
    d.display();
    d.setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawResult(const char* first, const char* second) {
    DisplayProxy& d = screen();
    d.clear();
    d.setColor(WHITE);
    d.setFont(ArialMT_Plain_10);
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.drawStringMaxWidth(64, 6, 120, first);
    if (second && second[0]) d.drawString(64, 46, second);
    d.display();
    d.setTextAlignment(TEXT_ALIGN_LEFT);
}

void feed() {
    if (watched) esp_task_wdt_reset();
}

bool before(uint32_t deadline) { return (int32_t)(millis() - deadline) < 0; }

[[noreturn]] void endSession(bool installed, const char* reason, const char* copy, uint32_t started) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Preferences boot;
    if (boot.begin(kBootNs, false)) {
        boot.putBool("skipanim", true);
        // Set when the session began; only a finished install clears it.
        if (installed && boot.isKey(kBootFailed)) boot.remove(kBootFailed);
        boot.end();
    }
    Serial.printf("[update] session=end result=%s ms=%lu\n", installed ? "installed" : reason,
                  (unsigned long)(millis() - started));
    if (installed) {
        drawResult("Restarting...", "");
        delay(300);
    } else {
        drawResult(copy && copy[0] ? copy : "The update did not finish.", "Nothing changed.");
        for (int i = 0; i < kFailScreenMs / 100; ++i) { delay(100); feed(); }
    }
    Serial.flush();
    esp_restart();
    for (;;) {}
}

// The update slot seen through OtaUpdate::Target.
class SlotTarget : public OtaUpdate::Target {
public:
    const esp_partition_t* slot = nullptr;
    bool begin(uint32_t) override {
        slot = esp_ota_get_next_update_partition(nullptr);
        if (!slot) return false;
        // Sequential writes: the slot is erased sector by sector as bytes
        // arrive, so no single call blocks for the whole erase.
        return esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle_) == ESP_OK;
    }
    bool write(const uint8_t* data, size_t len) override {
        return esp_ota_write(handle_, data, len) == ESP_OK;
    }
    bool end() override { return esp_ota_end(handle_) == ESP_OK; }
    void abort() override { esp_ota_abort(handle_); }
    bool persistPending(const char* record) override {
        Preferences upd;
        if (!upd.begin(kUpdNs, false)) return false;
        const bool ok = upd.putString(OtaUpdate::kKeyPendImg, record) == strlen(record);
        upd.end();
        return ok;
    }
    bool selectBoot() override { return esp_ota_set_boot_partition(slot) == ESP_OK; }
private:
    esp_ota_handle_t handle_ = 0;
};

class Sha256 : public OtaUpdate::Hasher {
public:
    Sha256() { mbedtls_sha256_init(&ctx_); }
    ~Sha256() override { mbedtls_sha256_free(&ctx_); }
    void start() override { mbedtls_sha256_starts(&ctx_, 0); }
    void update(const uint8_t* data, size_t len) override { mbedtls_sha256_update(&ctx_, data, len); }
    void finish(uint8_t out[32]) override { mbedtls_sha256_finish(&ctx_, out); }
private:
    mbedtls_sha256_context ctx_;
};

struct TextSink {
    char* buf;
    size_t cap;
    size_t len;
    bool overflow;
};

bool textSink(void* arg, const uint8_t* data, size_t n) {
    TextSink& t = *static_cast<TextSink*>(arg);
    if (t.len + n >= t.cap) { t.overflow = true; return false; }
    memcpy(t.buf + t.len, data, n);
    t.len += n;
    t.buf[t.len] = '\0';
    feed();
    return true;
}

struct Download {
    OtaUpdate::Installer* installer;
    const CloudSync::FetchReply* reply;
    uint32_t deadline;
    int shown;
    int logged;
};

bool downloadSink(void* arg, const uint8_t* data, size_t n) {
    Download& d = *static_cast<Download*>(arg);
    if (d.reply->status != 200) return false;   // an error body is never an image
    if (!d.installer->feed(data, n)) return false;
    feed();
    const int pct = d.installer->percent();
    if (pct != d.shown) {
        d.shown = pct;
        drawSession(nullptr, pct);
    }
    if (pct / 10 != d.logged / 10) {
        d.logged = pct;
        Serial.printf("[update] progress=%d bytes=%lu\n", pct, (unsigned long)d.installer->received());
    }
    if (isFault("session-hang") && pct >= 30) {
        Serial.println("[update] session fault=hang");
        Serial.flush();
        for (;;) {}
    }
    return before(d.deadline);
}

OtaUpdate::FetchOutcome outcomeOf(bool ok, const CloudSync::FetchReply& r) {
    if (r.status >= 500) return OtaUpdate::FetchOutcome::ServerError;
    if (r.status >= 400 || (r.status != 0 && r.status != 200)) return OtaUpdate::FetchOutcome::Refused;
    if (!ok || r.status == 0) return OtaUpdate::FetchOutcome::Transport;
    return OtaUpdate::FetchOutcome::Ok;
}

struct ManifestCheck {
    OtaUpdate::FetchOutcome outcome = OtaUpdate::FetchOutcome::Transport;
    const char* bad = "json";                 // parse failure field, nullptr when parsed
    OtaUpdate::Manifest m;
    OtaUpdate::Verdict verdict = OtaUpdate::Verdict::Malformed;
    char seen[OtaUpdate::kSeenKeyLen + 1] = {0};
};

// Fetches the update site's manifest for this Fidget's source and channel
// and runs every gate on it (nothing is downloaded or stored here).
void checkManifest(const char* base, const char* wanted, uint32_t deadline, uint32_t callMs,
                   ManifestCheck& out) {
    char source[OtaUpdate::kMaxSourceLen + 1] = {0}, channel[8] = {0};
    uint32_t seenTs = 0;
    Preferences upd;
    if (upd.begin(kUpdNs, true)) {
        if (upd.isKey("src")) upd.getString("src", source, sizeof(source));
        if (upd.isKey("chan")) upd.getString("chan", channel, sizeof(channel));
        upd.end();
    }
    const char* src = OtaUpdate::normalizeSource(source);
    const char* chan = OtaUpdate::normalizeChannel(channel);
    OtaUpdate::seenKey(src, chan, out.seen);
    if (upd.begin(kUpdNs, true)) {
        if (upd.isKey(out.seen)) seenTs = upd.getUInt(out.seen, 0);
        upd.end();
    }
    char url[640];
    const char* slash = strncmp(src, "fork:", 5) == 0 ? strchr(src + 5, '/') : nullptr;
    if (slash) {
        snprintf(url, sizeof(url), "%s/update/firmware.php?manifest=1&channel=%s&repo=%.*s%%2F%s",
                 base, chan, (int)(slash - (src + 5)), src + 5, slash + 1);
    } else {
        snprintf(url, sizeof(url), "%s/update/firmware.php?manifest=1&channel=%s", base, chan);
    }
    char* text = (char*)heap_caps_malloc(kManifestMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) return;
    text[0] = '\0';
    TextSink sink{text, kManifestMax, 0, false};
    CloudSync::FetchReply reply;
    const bool fetched = CloudSync::fetchPublic(url, callMs, deadline, textSink, &sink, reply);
    out.outcome = outcomeOf(fetched, reply);
    if (reply.status == 200 && sink.overflow) {
        // The site answered, with something that is not a manifest (a site
        // without the manifest route serves the whole install image here):
        // a refusal, never a reason to try another host.
        out.outcome = OtaUpdate::FetchOutcome::BadManifest;
        out.bad = "too-long";
    }
    Serial.printf("[update] manifest host=site http=%d outcome=%d bytes=%u\n", reply.status,
                  (int)out.outcome, (unsigned)sink.len);
    if (out.outcome == OtaUpdate::FetchOutcome::Ok) {
        out.bad = OtaUpdate::parseManifest(text, sink.len, out.m);
        if (out.bad) out.outcome = OtaUpdate::FetchOutcome::BadManifest;
    }
    heap_caps_free(text);
    if (out.outcome != OtaUpdate::FetchOutcome::Ok) {
        if (out.bad && out.outcome == OtaUpdate::FetchOutcome::BadManifest)
            Serial.printf("[update] manifest=invalid field=%s\n", out.bad);
        return;
    }
    OtaUpdate::Context ctx;
    ctx.board = &HAL::boardInfo();
    ctx.source = src;
    ctx.channel = chan;
    ctx.sourceAcknowledged = false;   // no source other than the official one can be chosen yet
    ctx.seenTs = seenTs;
    ctx.wanted = wanted;
    out.verdict = OtaUpdate::gate(out.m, ctx);
    if (out.verdict != OtaUpdate::Verdict::Ok) out.outcome = OtaUpdate::FetchOutcome::GateRefused;
    Serial.printf("[update] manifest version=%s size=%lu released_at=%lu gate=%s\n", out.m.version,
                  (unsigned long)out.m.size, (unsigned long)out.m.releasedAt,
                  OtaUpdate::verdictName(out.verdict));
}

}  // namespace

// ---- boot ------------------------------------------------------------------------

void beginSelfTest() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    pending = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
              state == ESP_OTA_IMG_PENDING_VERIFY;
    if (!pending) return;
    selfTestStart = millis();
    // Hardware start-up, the checks, the rest of setup and the first frame
    // run under the task watchdog (a longer period while pending, so the
    // hardware start-up budget below it can fire first): a hang resets the
    // Fidget, and a reset while pending returns to the previous image.
    // Failing to subscribe fails the self-test (in finishBoot).
    watched = setWatchdogPeriod(kPendingWdtMs) && esp_task_wdt_add(nullptr) == ESP_OK;
    takeFault(false);
    if (isFault("hal-hang")) {
        // Test builds: a hang before hardware start-up (no serial yet).
        for (;;) {}
    }
}

void loopTick(bool frameDrawn) {
    if (!awaitingFrame) return;
    if (watched) esp_task_wdt_reset();
    if (isFault("loop-crash")) {
        // Test builds: a crash after the checks passed, in the first loop
        // pass - the point where the image used to be kept already.
        Serial.println("[update] loop fault=crash");
        Serial.flush();
        abort();
    }
    const OtaUpdate::ConfirmStep step =
        OtaUpdate::confirmStep(true, frameDrawn, millis() - selfTestStart, kConfirmBudgetMs);
    if (step == OtaUpdate::ConfirmStep::RollBack) rollBack("deadline");
    if (step == OtaUpdate::ConfirmStep::Confirm) confirm();
}

void finishBoot() {
    // The hourly battery wake stays as short as it was: nothing to report on
    // a screen that is not shown. (A pending image never starts on a timer
    // wake: any reset before it is marked valid returns to the old image.)
    if (!pending && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    Serial.printf("[update] boot slot=%s state=%s other=%s other_state=%s\n",
                  running ? running->label : "?", partState(running),
                  other ? other->label : "?", partState(other));
    if (pending) {
        if (!watched) rollBack("watchdog");
        selfTest();
        return;
    }
    // A session that never handed over (set at its start, cleared once the
    // install is ready).
    bool sessionFailed = false;
    {
        Preferences boot;
        if (boot.begin(kBootNs, false)) {
            if (boot.isKey(kBootFailed)) {
                sessionFailed = true;
                boot.remove(kBootFailed);
            }
            boot.end();
        }
    }
    bool noticed = false;
    char text[160];
    if (readRecord(text, sizeof(text))) {
        Pending p;
        const bool valid = OtaUpdate::parsePending(text, p);
        const OtaUpdate::BootNotice n = OtaUpdate::bootNotice(true, valid, valid && runningImageIs(p));
        clearRecord();
        if (n == OtaUpdate::BootNotice::Completed) {
            // The new image was kept but its tidy-up was cut short.
            recordSeen(p);
            char notice[64];
            snprintf(notice, sizeof(notice), "Updated to %s", p.version);
            postNotice(notice, false);
            Serial.printf("[update] result=updated version=%s\n", p.version);
        } else {
            if (valid && !sessionFailed) {
                // The new image started and did not keep itself: never offer
                // this version automatically again (a manual check still can).
                Preferences upd;
                if (upd.begin(kUpdNs, false)) {
                    upd.putString(OtaUpdate::kKeyFailVer, p.version);
                    upd.end();
                }
            }
            postNotice(kDidNotFinish, true);
            noticed = true;
            Serial.printf("[update] result=did-not-finish expected=%s running=%s\n",
                          valid ? p.version : "-", getFirmwareVersionString());
        }
    }
    if (sessionFailed) {
        if (!noticed) postNotice(kDidNotFinish, true);
        Serial.println("[update] result=did-not-finish reason=session");
    }
}

bool takeSessionRequest(char* version, size_t len) {
    version[0] = '\0';
    Preferences boot;
    if (!boot.begin(kBootNs, false)) return false;
    const bool armed = boot.isKey(kBootKey) && boot.getBool(kBootKey, false);
    if (boot.isKey(kBootVersion)) boot.getString(kBootVersion, version, len);
    // Consumed before the session runs: a crash inside it never loops back
    // into another session.
    if (boot.isKey(kBootKey)) boot.remove(kBootKey);
    if (boot.isKey(kBootVersion)) boot.remove(kBootVersion);
    boot.end();
    // A timer wake never shows a screen; the one-shot is dropped.
    return armed && version[0] && esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
}

void runSession(const char* wanted) {
    const uint32_t started = millis();
    const uint32_t deadline = started + kSessionBudgetMs;
    drawSession("Getting ready", -1);
    takeFault(true);
    {
        // Until the session hands over to a new image, the next start treats
        // it as not finished - also after a crash or a watchdog reset here.
        Preferences boot;
        if (boot.begin(kBootNs, false)) {
            boot.putBool(kBootFailed, true);
            boot.end();
        }
    }
    // The whole session runs under the task watchdog with a period longer
    // than any single blocking call; a hang resets the Fidget, and nothing
    // before the very last step changes which slot it starts from.
    watched = setWatchdogPeriod(kSessionWdtMs) && esp_task_wdt_add(nullptr) == ESP_OK;
    Serial.printf("[update] session=start wanted=%s watchdog=%d\n", wanted, watched ? 1 : 0);
    if (!watched) endSession(false, "watchdog", nullptr, started);
    if (!installAllowed()) endSession(false, "unsigned", nullptr, started);
    if (!CloudSync::useExternalTlsMemory()) endSession(false, "tls-allocator", nullptr, started);

    char base[128];
    if (!CloudSync::siteBase(base, sizeof(base))) endSession(false, "invalid-base", nullptr, started);
    char ssid[33] = {0}, pass[65] = {0};
    {
        Preferences wifi;
        if (wifi.begin("wificfg", true)) {
            if (wifi.isKey("ssid")) wifi.getString("ssid", ssid, sizeof(ssid));
            if (wifi.isKey("pass")) wifi.getString("pass", pass, sizeof(pass));
            wifi.end();
        }
    }
    if (!ssid[0]) endSession(false, "no-wifi", "No saved network yet.", started);

    // Station only; Bluetooth is never started in this power cycle.
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_STA)) {
        memset(pass, 0, sizeof(pass));
        endSession(false, "sta-mode", nullptr, started);
    }
    WiFi.begin(ssid, pass);
    memset(pass, 0, sizeof(pass));
    const uint32_t joinStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - joinStart < kJoinMs) {
        delay(100);
        feed();
    }
    if (WiFi.status() != WL_CONNECTED) endSession(false, "join", "Network not in range.", started);
    Serial.printf("[update] joined ms=%lu\n", (unsigned long)(millis() - joinStart));

    // ---- manifest (the update site first) ----
    ManifestCheck mc;
    checkManifest(base, wanted, deadline, kCallMs, mc);
    if (OtaUpdate::fallbackAllowed(mc.outcome)) {
        // The site could not answer. GitHub releases are the fallback host,
        // but the device has no GitHub manifest reader yet: say so and stop.
        Serial.println("[update] manifest host=github outcome=unavailable");
        endSession(false, "site-unavailable", "The update site is not answering. Try again later.", started);
    }
    if (mc.outcome == OtaUpdate::FetchOutcome::GateRefused)
        endSession(false, OtaUpdate::verdictName(mc.verdict), OtaUpdate::verdictCopy(mc.verdict), started);
    if (mc.outcome == OtaUpdate::FetchOutcome::BadManifest) endSession(false, "manifest", nullptr, started);
    if (mc.outcome != OtaUpdate::FetchOutcome::Ok) endSession(false, "manifest-refused", nullptr, started);
    const OtaUpdate::Manifest& m = mc.m;
    const char* seen = mc.seen;
    CloudSync::FetchReply reply;
    char url[640];

    // ---- image ----
    SlotTarget target;
    Sha256 hasher;
    OtaUpdate::Installer installer(target, hasher);
    if (!installer.begin(m, seen)) endSession(false, "begin", nullptr, started);
    Serial.printf("[update] slot=%s\n", target.slot ? target.slot->label : "?");
    drawSession(nullptr, 0);
    snprintf(url, sizeof(url), "%s%s", base, m.url);
    Download dl{&installer, &reply, deadline, 0, 0};
    const uint32_t dlStart = millis();
    const bool got = CloudSync::fetchPublic(url, kCallMs, deadline, downloadSink, &dl, reply);
    if (!got) installer.abort();
    const OtaUpdate::InstallResult result = installer.complete();
    Serial.printf("[update] install=%s http=%d bytes=%lu ms=%lu\n", OtaUpdate::installResultName(result),
                  reply.status, (unsigned long)installer.received(), (unsigned long)(millis() - dlStart));
    if (result != OtaUpdate::InstallResult::Ready)
        endSession(false, OtaUpdate::installResultName(result), nullptr, started);
    {
        // Handed over: from here the new image reports its own outcome, so a
        // failure in the tear-down below must not read as "did not finish".
        Preferences boot;
        if (boot.begin(kBootNs, false)) {
            if (boot.isKey(kBootFailed)) boot.remove(kBootFailed);
            boot.end();
        }
    }
    endSession(true, "installed", nullptr, started);
}

// ---- Install now -----------------------------------------------------------------

bool installAllowed() {
    Preferences upd;
    if (!upd.begin(kUpdNs, true)) return false;
    const bool allowed = upd.isKey(OtaUpdate::kKeyUnsigOk) && upd.getBool(OtaUpdate::kKeyUnsigOk, false);
    upd.end();
    return allowed;
}

bool armInstall(const char* version, const char** why) {
    const char* ignored = nullptr;
    if (!why) why = &ignored;
    if (!installAllowed()) { *why = "unsigned"; return false; }
    if (!version || !version[0] || strlen(version) > OtaUpdate::kMaxVersionLen || strchr(version, ' ')) {
        *why = "version";
        return false;
    }
    Preferences boot;
    if (!boot.begin(kBootNs, false)) { *why = "storage"; return false; }
    // The session key is written last: it is what schedules the session.
    bool ok = boot.putString(kBootVersion, version) == strlen(version) &&
              boot.putBool("skipanim", true) != 0 &&
              boot.putBool(kBootKey, true) != 0;
    if (!ok) {
        boot.remove(kBootKey);
        boot.remove(kBootVersion);
    }
    boot.end();
    if (!ok) *why = "storage";
    return ok;
}

void refreshOffer(uint32_t deadlineMs) {
    char base[128];
    if (!CloudSync::siteBase(base, sizeof(base))) return;
    ManifestCheck mc;
    // The check-in's own short call timeout: a Music Player launch waits
    // for this worker to stop.
    checkManifest(base, nullptr, deadlineMs, kOfferCallMs, mc);
    if (mc.outcome != OtaUpdate::FetchOutcome::Ok && mc.outcome != OtaUpdate::FetchOutcome::GateRefused) {
        Serial.printf("[update] offer=unchanged outcome=%d\n", (int)mc.outcome);
        return;
    }
    Preferences upd;
    if (!upd.begin(kUpdNs, false)) return;
    bool ok;
    if (mc.outcome == OtaUpdate::FetchOutcome::Ok) {
        ok = upd.putString(OtaUpdate::kKeyAvail, mc.m.version) == strlen(mc.m.version);
    } else {
        // A release this Fidget must not take is never offered.
        ok = !upd.isKey(OtaUpdate::kKeyAvail) || upd.remove(OtaUpdate::kKeyAvail);
    }
    upd.end();
    Serial.printf("[update] offer=%s version=%s gate=%s write=%s\n",
                  mc.outcome == OtaUpdate::FetchOutcome::Ok ? "stored" : "withdrawn", mc.m.version,
                  OtaUpdate::verdictName(mc.verdict), ok ? "ok" : "error");
}

bool setAllowUnsigned(bool allow) {
    Preferences upd;
    if (!upd.begin(kUpdNs, false)) return false;
    const bool ok = upd.putBool(OtaUpdate::kKeyUnsigOk, allow) != 0;
    upd.end();
    return ok;
}

void printSlots() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    char record[160];
    const bool present = readRecord(record, sizeof(record));
    Serial.printf("[cmd] upd.slot=%s state=%s boot=%s other=%s other_state=%s pend_img=%d unsig_ok=%d\n",
                  running ? running->label : "?", partState(running), boot ? boot->label : "?",
                  other ? other->label : "?", partState(other), present ? 1 : 0,
                  installAllowed() ? 1 : 0);
}

#ifdef CF_TEST_CLI
int clearSeen() {
    // Collect first, then remove: never edit a namespace mid-iteration.
    char keys[8][16];
    int n = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kUpdNs, NVS_TYPE_U32, &it);
    while (err == ESP_OK && it && n < 8) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "seen_", 5) == 0) {
            strncpy(keys[n], info.key, sizeof(keys[n]) - 1);
            keys[n][sizeof(keys[n]) - 1] = '\0';
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    Preferences upd;
    if (!upd.begin(kUpdNs, false)) return -1;
    for (int i = 0; i < n; ++i) upd.remove(keys[i]);
    upd.end();
    return n;
}

bool setTestFault(const char* name) {
    static const char* kFaults[] = {"none", "crash", "hang", "hal-hang", "loop-crash", "version", "mount",
                                    "session-hang"};
    bool known = false;
    for (const char* f : kFaults) known = known || strcmp(f, name) == 0;
    if (!known) return false;
    Preferences test;
    if (!test.begin("cftest", false)) return false;
    bool ok;
    if (strcmp(name, "none") == 0) ok = !test.isKey(OtaUpdate::kTestFault) || test.remove(OtaUpdate::kTestFault);
    else ok = test.putString(OtaUpdate::kTestFault, name) == strlen(name);
    test.end();
    return ok;
}
#endif

}  // namespace UpdateSession

#endif  // HOST_TEST
