// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <cJSON.h>
#include <stdio.h>
#include <string>

#include "CloudPlanner.h"
#include "CloudProtocol.h"
#include "SyncProtocol.h"

using namespace CloudSync;

void setUp(void)    {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

// PHP json_encode's default string escaping: "\/" for slashes and \uXXXX
// (surrogate pairs above the BMP) for every non-ASCII code point.
std::string phpJsonString(const std::string& utf8) {
    std::string out = "\"";
    char buf[16];
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = (unsigned char)utf8[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
        i += n;
        if (cp == '"') out += "\\\"";
        else if (cp == '\\') out += "\\\\";
        else if (cp == '/') out += "\\/";
        else if (cp < 0x20) { snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)cp); out += buf; }
        else if (cp < 0x80) out += (char)cp;
        else if (cp < 0x10000) { snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)cp); out += buf; }
        else {
            cp -= 0x10000;
            snprintf(buf, sizeof(buf), "\\u%04x\\u%04x",
                     (unsigned)(0xD800 + (cp >> 10)), (unsigned)(0xDC00 + (cp & 0x3FF)));
            out += buf;
        }
    }
    return out + "\"";
}

std::string crcHex(const std::string& bytes) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", (unsigned)SyncProtocol::crc32(bytes.data(), bytes.size()));
    return buf;
}

const char* kShaA = "0123abcd00000000000000000000000000000000000000000000000000000001";
const char* kShaB = "0123abcd00000000000000000000000000000000000000000000000000000002";
const char* kShaC = "89abcdef00000000000000000000000000000000000000000000000000000003";

// A sealed document the way the site freezes it (unescaped slashes and
// unicode, one escaped quote inside a name).
std::string docFor(const char* batch, const char* blobPath) {
    return std::string("{\"batch\":\"") + batch + "\",\"base\":\"1a2b3c4d\",\"ops\":["
           "{\"op\":\"add\",\"entry\":{\"id\":\"cafe\","
           "\"name\":\"Caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x98\x80 \\\"q\\\"\","
           "\"category\":\"Games\",\"blobPath\":\"" + blobPath + "\","
           "\"format\":\"wasm\"}}]}";
}

std::string blobRow(const char* sha, unsigned size) {
    return std::string("{\"sha256\":\"") + sha + "\",\"size\":" + std::to_string(size) +
           ",\"url\":" + phpJsonString(std::string("/api/device-loadout.php?device_id=a1b2c3d4e5f6&blob=") + sha) + "}";
}

std::string offerBody(const std::string& doc, const std::string& crc, const std::string& blobs) {
    return "{\"doc\":" + phpJsonString(doc) + ",\"doc_crc\":\"" + crc + "\",\"blobs\":[" +
           blobs + "],\"version\":4,\"next_poll_ms\":86400000}";
}

// ---------------------------------------------------------------------------
// Planner (the worker acts on each of these returns)
// ---------------------------------------------------------------------------

void test_bt_tainted_entry_reboots_before_checkin() {
    CloudPlanner plan;
    TEST_ASSERT_EQUAL_INT((int)Step::Reboot, (int)plan.start(false));
    TEST_ASSERT_EQUAL_INT((int)Step::Error,
        (int)plan.checkin(200, true, false, true, 1000, 0));
}

void test_no_change_and_rate_limit_backoff() {
    CloudPlanner plan;
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Done,
        (int)plan.checkin(204, false, false, true, 86400000, 0));
    TEST_ASSERT_EQUAL_UINT32(86400000, plan.nextMs());
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Backoff,
        (int)plan.checkin(429, false, false, true, 2000, 60000));
    TEST_ASSERT_EQUAL_UINT32(60000, plan.nextMs());
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Error,
        (int)plan.checkin(503, false, false, true, 3000, 0));
}

void test_offer_then_download_apply_and_ack() {
    CloudPlanner plan;
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Loadout,
        (int)plan.checkin(200, true, true, true, 86400000, 0));
    TEST_ASSERT_TRUE(plan.firmwareOffered());
    TEST_ASSERT_EQUAL_INT((int)Step::Download,
        (int)plan.loadout(200, OfferVerdict::Ok));
    TEST_ASSERT_EQUAL_INT((int)Step::Apply, (int)plan.blob(BlobVerdict::Ok));
    TEST_ASSERT_EQUAL_INT((int)Step::Ack, (int)plan.applied(true));
    TEST_ASSERT_FALSE(plan.rejected());
    TEST_ASSERT_EQUAL_INT((int)Step::Ack, (int)plan.report(true));
    TEST_ASSERT_EQUAL_INT((int)Step::Done, (int)plan.ack(204, 86400000, 0));
}

void test_waiting_leaves_batch_and_firmware_only_offered() {
    CloudPlanner plan;
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Waiting,
        (int)plan.checkin(200, true, true, false, 86400000, 0));
    TEST_ASSERT_TRUE(plan.firmwareOffered());
    TEST_ASSERT_EQUAL_INT((int)Step::Waiting, (int)plan.step());
}

// Permanent offer/blob defects are answered (rejected), not left to wedge
// the queue; transport-like failures stay retryable errors.
void test_bad_offer_and_sha_never_reach_apply() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    TEST_ASSERT_EQUAL_INT((int)Step::Ack,
        (int)plan.loadout(200, OfferVerdict::Permanent));
    TEST_ASSERT_TRUE(plan.rejected());

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    TEST_ASSERT_EQUAL_INT((int)Step::Error,
        (int)plan.loadout(200, OfferVerdict::Retryable));

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    TEST_ASSERT_EQUAL_INT((int)Step::Ack, (int)plan.blob(BlobVerdict::Permanent));
    TEST_ASSERT_TRUE(plan.rejected());
    TEST_ASSERT_EQUAL_INT((int)Step::Error, (int)plan.applied(true));

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    TEST_ASSERT_EQUAL_INT((int)Step::Error, (int)plan.blob(BlobVerdict::Retryable));
}

void test_apply_refusal_is_still_acknowledged() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    plan.blob(BlobVerdict::Ok);
    TEST_ASSERT_EQUAL_INT((int)Step::Ack, (int)plan.applied(false));
    TEST_ASSERT_TRUE(plan.rejected());
    plan.report(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Backoff, (int)plan.ack(429, 2000, 61000));
}

void test_failed_radio_teardown_requires_reboot() {
    CloudPlanner plan;
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Error, (int)plan.failure(true));
    TEST_ASSERT_EQUAL_INT((int)Step::Reboot, (int)plan.failure(false));
}

void test_already_applied_batch_skips_download() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    TEST_ASSERT_EQUAL_INT((int)Step::Ack,
        (int)plan.loadout(200, OfferVerdict::AlreadyApplied));
    TEST_ASSERT_FALSE(plan.rejected());
    TEST_ASSERT_EQUAL_INT((int)Step::Error, (int)plan.blob(BlobVerdict::Ok));
}

void test_backoff_retries_once_within_budget() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(429, false, false, true, 2000, 3000);
    TEST_ASSERT_EQUAL_INT((int)Step::Checkin, (int)plan.retry(true));
    TEST_ASSERT_EQUAL_INT((int)Step::Backoff,
        (int)plan.checkin(429, false, false, true, 2000, 3000));
    TEST_ASSERT_EQUAL_INT((int)Step::Deferred, (int)plan.retry(true));

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    TEST_ASSERT_EQUAL_INT((int)Step::Backoff, (int)plan.loadout(429, OfferVerdict::Ok));
    TEST_ASSERT_EQUAL_INT((int)Step::Loadout, (int)plan.retry(true));

    plan.start(true);
    plan.checkin(429, false, false, true, 2000, 90000);
    TEST_ASSERT_EQUAL_INT((int)Step::Deferred, (int)plan.retry(false));
}

void test_report_without_budget_is_deferred_not_rebooted() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 86400000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    plan.blob(BlobVerdict::Ok);
    plan.applied(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Deferred, (int)plan.report(false));
    // Deadline passed with the radio cleanly off: an error, never a reboot.
    TEST_ASSERT_EQUAL_INT((int)Step::Error, (int)plan.failure(true));
}

void test_follow_up_backoff_retries_the_ack() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::AlreadyApplied);
    plan.report(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Backoff, (int)plan.ack(429, 2000, 2000));
    TEST_ASSERT_EQUAL_INT((int)Step::Ack, (int)plan.retry(true));
    TEST_ASSERT_EQUAL_INT((int)Step::Done, (int)plan.ack(200, 2000, 0));
}

// ---------------------------------------------------------------------------
// Check-in parsing
// ---------------------------------------------------------------------------

void test_checkin_null_batch_with_firmware_offer_is_no_action() {
    const char* body =
        "{\"loadout_version\":3,\"batch_id\":null,\"send_report\":false,"
        "\"firmware\":{\"offer\":true,\"url\":\"\\/update\\/firmware.php?manifest=1\"},"
        "\"mode\":\"normal\",\"next_poll_ms\":86400000,\"server_time\":\"2026-09-23T10:00:00Z\"}";
    CheckinReply reply;
    TEST_ASSERT_TRUE(parseCheckin(body, 0, reply));
    TEST_ASSERT_FALSE(reply.hasBatch);
    TEST_ASSERT_TRUE(reply.firmwareOffer);
    TEST_ASSERT_EQUAL_STRING("/update/firmware.php?manifest=1", reply.firmwareUrl.c_str());
    TEST_ASSERT_EQUAL_UINT32(86400000, reply.nextPollMs);
    TEST_ASSERT_EQUAL_UINT32(1790157600u, reply.serverEpoch);
    TEST_ASSERT_EQUAL_UINT32(kNormalFloorMs, followupFloorMs(reply));
    // The offer alone routes to "nothing to do": no batch, nothing posted
    // or installed from the check-in (the update lane owns the manifest).
    CloudPlanner plan;
    plan.start(true);
    TEST_ASSERT_EQUAL_INT((int)Step::Done,
        (int)plan.checkin(200, reply.hasBatch, reply.firmwareOffer, true, reply.nextPollMs, 0));
}

void test_checkin_with_batch_and_dev_mode() {
    const char* body =
        "{\"loadout_version\":4,\"batch_id\":\"b-7_x\",\"send_report\":true,"
        "\"firmware\":{\"offer\":true,\"url\":\"\\/update\\/firmware.php?manifest=1\"},"
        "\"mode\":\"dev\",\"next_poll_ms\":2000,\"server_time\":\"2024-02-29T23:59:59Z\"}";
    CheckinReply reply;
    TEST_ASSERT_TRUE(parseCheckin(body, 0, reply));
    TEST_ASSERT_TRUE(reply.hasBatch);
    TEST_ASSERT_EQUAL_STRING("b-7_x", reply.batchId.c_str());
    TEST_ASSERT_TRUE(reply.sendReport);
    TEST_ASSERT_EQUAL_UINT32(1709251199u, reply.serverEpoch);
    TEST_ASSERT_EQUAL_UINT32(kDevFloorMs, followupFloorMs(reply));
}

void test_checkin_204_and_429_bodies() {
    CheckinReply reply;
    TEST_ASSERT_TRUE(parseCheckin(nullptr, 2000, reply));
    TEST_ASSERT_FALSE(reply.hasBatch);
    TEST_ASSERT_EQUAL_UINT32(2000, reply.nextPollMs);
    TEST_ASSERT_EQUAL_UINT32(kDevFloorMs, followupFloorMs(reply));

    TEST_ASSERT_TRUE(parseCheckin("{\"error\":\"too_soon\",\"next_poll_ms\":86400000}", 0, reply));
    TEST_ASSERT_EQUAL_UINT32(86400000, reply.nextPollMs);
    TEST_ASSERT_EQUAL_UINT32(43000, retryWaitMs(42));
    TEST_ASSERT_EQUAL_UINT32(0, retryWaitMs(0));
    TEST_ASSERT_EQUAL_UINT32(0, retryWaitMs(3600));
}

void test_checkin_rejects_unreportable_batch_id() {
    CheckinReply reply;
    TEST_ASSERT_FALSE(parseCheckin("{\"batch_id\":\"has space\"}", 0, reply));
    TEST_ASSERT_FALSE(parseCheckin("{\"batch_id\":7}", 0, reply));
    TEST_ASSERT_FALSE(parseCheckin("{not json", 0, reply));
}

void test_server_epoch_rejects_bad_text() {
    TEST_ASSERT_EQUAL_UINT32(0, serverEpoch(nullptr));
    TEST_ASSERT_EQUAL_UINT32(0, serverEpoch("2026-09-23 10:00:00"));
    TEST_ASSERT_EQUAL_UINT32(0, serverEpoch("2026-13-23T10:00:00Z"));
    TEST_ASSERT_EQUAL_UINT32(0, serverEpoch("2026-09-23T10:00:00Zjunk"));
}

// ---------------------------------------------------------------------------
// Offer parsing
// ---------------------------------------------------------------------------

void test_offer_escaped_doc_hashes_verbatim() {
    const std::string doc = docFor("b-7_x", "/apps/cafe-0123abcd.wasm");
    const std::string body = offerBody(doc, crcHex(doc), blobRow(kShaA, 1234));
    // The fixture really is escaped the way the site sends it.
    TEST_ASSERT_TRUE(body.find("\\/apps\\/cafe") != std::string::npos);
    TEST_ASSERT_TRUE(body.find("\\u00e9") != std::string::npos);
    TEST_ASSERT_TRUE(body.find("\\ud83d\\ude00") != std::string::npos);
    Offer offer;
    TEST_ASSERT_EQUAL_INT((int)OfferError::None, (int)parseOffer(body.c_str(), "b-7_x", offer));
    TEST_ASSERT_TRUE(offer.doc == doc);
    TEST_ASSERT_EQUAL_UINT32(SyncProtocol::crc32(doc.data(), doc.size()), offer.docCrc);
    TEST_ASSERT_EQUAL_UINT32(86400000, offer.nextPollMs);
    TEST_ASSERT_EQUAL_INT(1, (int)offer.blobs.size());
    TEST_ASSERT_EQUAL_UINT32(1234, offer.blobs[0].size);
    TEST_ASSERT_EQUAL_STRING(kShaA, offer.blobs[0].sha256.c_str());
    TEST_ASSERT_EQUAL_INT(1, (int)offer.blobs[0].targets.size());
    TEST_ASSERT_EQUAL_STRING("/apps/cafe-0123abcd.wasm", offer.blobs[0].targets[0].c_str());
    TEST_ASSERT_EQUAL_STRING_LEN("/api/device-loadout.php?device_id=", offer.blobs[0].url.c_str(), 34);
}

void test_offer_doc_crc_mismatch_is_permanent() {
    const std::string doc = docFor("b-7_x", "/apps/cafe-0123abcd.wasm");
    Offer offer;
    const OfferError e = parseOffer(offerBody(doc, "00000000", blobRow(kShaA, 10)).c_str(), "b-7_x", offer);
    TEST_ASSERT_EQUAL_INT((int)OfferError::DocCrc, (int)e);
    TEST_ASSERT_TRUE(offerErrorPermanent(e));
}

void test_offer_batch_mismatch_is_rejected() {
    const std::string doc = docFor("b-other", "/apps/cafe-0123abcd.wasm");
    Offer offer;
    const OfferError e = parseOffer(offerBody(doc, crcHex(doc), blobRow(kShaA, 10)).c_str(), "b-7_x", offer);
    TEST_ASSERT_EQUAL_INT((int)OfferError::Batch, (int)e);
    TEST_ASSERT_EQUAL_STRING("rejected:offer-batch", offerRejection(e));
}

void test_offer_duplicate_sha_prefix_is_rejected() {
    const std::string doc = docFor("b-7_x", "/apps/cafe-0123abcd.wasm");
    Offer offer;
    const std::string blobs = blobRow(kShaA, 10) + "," + blobRow(kShaB, 10);
    TEST_ASSERT_EQUAL_INT((int)OfferError::BlobPrefix,
        (int)parseOffer(offerBody(doc, crcHex(doc), blobs).c_str(), "b-7_x", offer));
}

void test_offer_bad_blob_rows_are_rejected() {
    const std::string doc = docFor("b-7_x", "/apps/cafe-0123abcd.wasm");
    Offer offer;
    // Off-origin url.
    std::string row = std::string("{\"sha256\":\"") + kShaA + "\",\"size\":10,\"url\":\"https:\\/\\/evil.example\\/x\"}";
    TEST_ASSERT_EQUAL_INT((int)OfferError::BlobEntry,
        (int)parseOffer(offerBody(doc, crcHex(doc), row).c_str(), "b-7_x", offer));
    // Zero size.
    TEST_ASSERT_EQUAL_INT((int)OfferError::BlobEntry,
        (int)parseOffer(offerBody(doc, crcHex(doc), blobRow(kShaA, 0)).c_str(), "b-7_x", offer));
    // A blob no document path points at.
    TEST_ASSERT_EQUAL_INT((int)OfferError::BlobTarget,
        (int)parseOffer(offerBody(doc, crcHex(doc), blobRow(kShaC, 10)).c_str(), "b-7_x", offer));
    // A document path outside the write roots.
    const std::string outside = docFor("b-7_x", "/loadout-0123abcd.wasm");
    TEST_ASSERT_EQUAL_INT((int)OfferError::BlobPath,
        (int)parseOffer(offerBody(outside, crcHex(outside), blobRow(kShaA, 10)).c_str(), "b-7_x", offer));
}

void test_offer_unreadable_body_is_retryable() {
    Offer offer;
    const OfferError e = parseOffer("{\"doc\":\"trunc", "b-7_x", offer);
    TEST_ASSERT_EQUAL_INT((int)OfferError::Json, (int)e);
    TEST_ASSERT_FALSE(offerErrorPermanent(e));
    TEST_ASSERT_NULL(offerRejection(e));
}

void test_rejection_strings_match_server_rule() {
    const OfferError permanent[] = {OfferError::DocCrc, OfferError::Batch, OfferError::BlobEntry,
                                    OfferError::BlobPrefix, OfferError::BlobPath, OfferError::BlobTarget};
    for (OfferError e : permanent) TEST_ASSERT_TRUE(validResult(offerRejection(e)));
    const char* driverAnswers[] = {"applied", "already-applied", "stale-revision",
                                   "rejected:blob-sha", "rejected:blob-missing",
                                   "rejected:record", "rejected:apply"};
    for (const char* a : driverAnswers) TEST_ASSERT_TRUE(validResult(a));
    TEST_ASSERT_FALSE(validResult("rejected:"));
    TEST_ASSERT_FALSE(validResult("rejected:has space"));
    TEST_ASSERT_FALSE(validResult("failed"));
}

// ---------------------------------------------------------------------------
// Check-in body
// ---------------------------------------------------------------------------

void test_checkin_body_carries_answer_and_report() {
    LoadoutManifest::Loadout installed;
    LoadoutManifest::LoadoutEntry e;
    e.id = "cafe"; e.name = "Caf\xC3\xA9"; e.category = "Games";
    e.format = "wasm"; e.blobPath = "/apps/cafe-0123abcd.wasm"; e.position = 2;
    installed.entries.push_back(e);
    CheckinFields f;
    f.deviceId = "a1b2c3d4e5f6"; f.fw = "1.4.0+abc123"; f.abi = "3"; f.board = "1.2";
    f.fsTotal = 1441792; f.fsUsed = 20480; f.manifestCrc = 0x1a2b3c4d;
    f.appliedBatch = "b-7_x"; f.result = "rejected:blob-sha";
    f.installed = &installed;
    const std::string body = buildCheckinBody(f);
    cJSON* root = cJSON_Parse(body.c_str());
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4e5f6", cJSON_GetObjectItem(root, "device_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("batch1", cJSON_GetObjectItem(root, "lapply_cap")->valuestring);
    TEST_ASSERT_EQUAL_STRING("1a2b3c4d", cJSON_GetObjectItem(root, "manifest_crc")->valuestring);
    TEST_ASSERT_EQUAL_STRING("b-7_x", cJSON_GetObjectItem(root, "applied_batch")->valuestring);
    TEST_ASSERT_EQUAL_STRING("rejected:blob-sha", cJSON_GetObjectItem(root, "result")->valuestring);
    TEST_ASSERT_EQUAL_INT(1441792, (int)cJSON_GetObjectItem(root, "fs_total")->valuedouble);
    cJSON* rows = cJSON_GetObjectItem(root, "installed");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(rows));
    cJSON* row = cJSON_GetArrayItem(rows, 0);
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", cJSON_GetObjectItem(row, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("/apps/cafe-0123abcd.wasm", cJSON_GetObjectItem(row, "blobPath")->valuestring);
    TEST_ASSERT_EQUAL_INT(2, (int)cJSON_GetObjectItem(row, "position")->valuedouble);
    cJSON_Delete(root);
}

void test_checkin_body_omits_unacceptable_answer() {
    CheckinFields f;
    f.deviceId = "a1b2c3d4e5f6"; f.fw = "1.4.0"; f.abi = "3"; f.board = "1.2";
    f.appliedBatch = "b-7_x"; f.result = "rejected:bad reason";
    const std::string body = buildCheckinBody(f);
    cJSON* root = cJSON_Parse(body.c_str());
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "applied_batch"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "result"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "installed"));
    TEST_ASSERT_EQUAL_STRING("00000000", cJSON_GetObjectItem(root, "manifest_crc")->valuestring);
    cJSON_Delete(root);
}

void test_checkin_body_carries_hardware_fields_when_present() {
    CheckinFields f;
    f.deviceId = "aabbccddeeff";
    f.flashId = "0123456789abcdef";
    f.serial = "1234abcd";
    const std::string body = buildCheckinBody(f);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("\"flash_id\":\"0123456789abcdef\""));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("\"serial\":\"1234abcd\""));
    f.flashId = "";
    f.serial = "";
    const std::string absent = buildCheckinBody(f);
    TEST_ASSERT_EQUAL(std::string::npos, absent.find("\"flash_id\""));
    TEST_ASSERT_EQUAL(std::string::npos, absent.find("\"serial\""));
}

void test_budget_selection() {
    TEST_ASSERT_TRUE(budgetCovers(61000, 10000, 150000, 12000));
    TEST_ASSERT_FALSE(budgetCovers(61000, 80000, 150000, 12000));
    TEST_ASSERT_FALSE(budgetCovers(0, 150000, 150000, 12000));
    TEST_ASSERT_TRUE(devMode("always", 86400000));
    TEST_ASSERT_TRUE(devMode("normal", 2000));
    TEST_ASSERT_FALSE(devMode("normal", 86400000));
}

void test_backoff_retention() {
    // Retry-After is honoured on any status that carries it.
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Store, (int)backoffFor(429, 42));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Store, (int)backoffFor(503, 60));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Store, (int)backoffFor(503, 3600));
    // Only a success clears an old one; other replies leave it in place.
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Clear, (int)backoffFor(200, 0));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Clear, (int)backoffFor(204, 0));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Keep, (int)backoffFor(503, 0));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Keep, (int)backoffFor(401, 0));
    TEST_ASSERT_EQUAL_INT((int)BackoffAction::Keep, (int)backoffFor(0, 0));
}

void test_applied_notice_only_for_this_sessions_apply() {
    CloudPlanner plan;
    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    plan.blob(BlobVerdict::Ok);
    plan.applied(true);
    TEST_ASSERT_TRUE(plan.appliedNow());

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::AlreadyApplied);
    TEST_ASSERT_FALSE(plan.appliedNow());

    plan.start(true);
    plan.checkin(200, true, false, true, 2000, 0);
    plan.loadout(200, OfferVerdict::Ok);
    plan.blob(BlobVerdict::Ok);
    plan.applied(false);
    TEST_ASSERT_FALSE(plan.appliedNow());
}

// ---------------------------------------------------------------------------
// Dev mode listening: the mode the site records, and the pace of check-ins
// ---------------------------------------------------------------------------

void test_checkin_body_carries_the_mode() {
    CheckinFields f;
    f.deviceId = "aabbccddeeff";
    f.mode = "always";
    TEST_ASSERT_NOT_EQUAL(std::string::npos, buildCheckinBody(f).find("\"mode\":\"always\""));
    f.mode = "normal";
    TEST_ASSERT_NOT_EQUAL(std::string::npos, buildCheckinBody(f).find("\"mode\":\"normal\""));
    f.mode = nullptr;   // no setting known: no mode sent (the site keeps its record)
    TEST_ASSERT_EQUAL(std::string::npos, buildCheckinBody(f).find("\"mode\""));
    f.mode = "";
    TEST_ASSERT_EQUAL(std::string::npos, buildCheckinBody(f).find("\"mode\""));
}

void test_dev_poll_follows_the_site_pace() {
    // No jitter (0): the site's next_poll_ms, held to [2 s, 30 s].
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(2000, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(0, 0, 0, 0));        // missing: the 2 s default
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(500, 0, 0, 0));      // never faster than 2 s
    TEST_ASSERT_EQUAL_UINT32(5000, devPollWaitMs(5000, 0, 0, 0));
    // A normal-mode answer (a day) while the mode change settles: 30 s.
    TEST_ASSERT_EQUAL_UINT32(kDevMaxPollMs, devPollWaitMs(86400000, 0, 0, 0));
}

void test_dev_poll_jitter_is_bounded() {
    // 0-10 % on top, whatever the random number.
    const uint32_t seeds[] = {0u, 1u, 199u, 200u, 201u, 12345u, 0xFFFFFFFFu};
    for (uint32_t seed : seeds) {
        const uint32_t w = devPollWaitMs(2000, 0, 0, seed);
        TEST_ASSERT_TRUE(w >= 2000 && w <= 2200);
    }
    bool varied = false;
    for (uint32_t seed = 0; seed < 50; ++seed)
        if (devPollWaitMs(2000, 0, 0, seed) != 2000) varied = true;
    TEST_ASSERT_TRUE(varied);
}

void test_dev_poll_backs_off_after_failures() {
    // Doubling from 2 s, capped at 60 s.
    TEST_ASSERT_EQUAL_UINT32(4000, devPollWaitMs(0, 1, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(8000, devPollWaitMs(0, 2, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(16000, devPollWaitMs(0, 3, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(32000, devPollWaitMs(0, 4, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(kDevMaxBackoffMs, devPollWaitMs(0, 5, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(kDevMaxBackoffMs, devPollWaitMs(0, 250, 0, 0));
    // An authenticated error reply's own next_poll_ms is honoured when longer.
    TEST_ASSERT_EQUAL_UINT32(20000, devPollWaitMs(20000, 1, 0, 0));
    // A success right after resets to the site's pace (failures = 0).
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(2000, 0, 0, 0));
}

void test_dev_poll_settles_a_mode_the_site_has_not_taken() {
    // The site still answers "normal" (a day's pace, held to 30 s): check
    // again after 11 s so the mode change lands.
    TEST_ASSERT_EQUAL_UINT32(kDevModeSettleMs, devPollWaitMs(86400000, 0, 0, 0, true));
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(2000, 0, 0, 0, true));   // already fast
    // Never shortens a failure backoff or a Retry-After.
    TEST_ASSERT_EQUAL_UINT32(16000, devPollWaitMs(0, 3, 0, 0, true));
    TEST_ASSERT_EQUAL_UINT32(60000, devPollWaitMs(2000, 0, 60, 0, true));
    // Recognising it: a 200 with another mode, or a normal-mode pace on a
    // 204 (no body, only X-Next-Poll-Ms).
    TEST_ASSERT_TRUE(devModeNotTaken(200, "normal", "always", 86400000));
    TEST_ASSERT_TRUE(devModeNotTaken(204, "", "always", 86400000));
    TEST_ASSERT_FALSE(devModeNotTaken(204, "", "always", 2000));
    TEST_ASSERT_FALSE(devModeNotTaken(200, "always", "always", 2000));
    TEST_ASSERT_FALSE(devModeNotTaken(429, "", "always", 86400000));
    TEST_ASSERT_FALSE(devModeNotTaken(200, "", "dev", 30000));   // a slow dev pace is fine
}

void test_dev_poll_honours_retry_after() {
    TEST_ASSERT_EQUAL_UINT32(2000, devPollWaitMs(2000, 0, 1, 0));     // shorter than the pace
    TEST_ASSERT_EQUAL_UINT32(60000, devPollWaitMs(2000, 1, 60, 0));   // a 503's 60 s
    TEST_ASSERT_EQUAL_UINT32(kDevMaxRetryMs, devPollWaitMs(2000, 1, 3600, 0));   // capped
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_bt_tainted_entry_reboots_before_checkin);
    RUN_TEST(test_no_change_and_rate_limit_backoff);
    RUN_TEST(test_offer_then_download_apply_and_ack);
    RUN_TEST(test_waiting_leaves_batch_and_firmware_only_offered);
    RUN_TEST(test_bad_offer_and_sha_never_reach_apply);
    RUN_TEST(test_apply_refusal_is_still_acknowledged);
    RUN_TEST(test_failed_radio_teardown_requires_reboot);
    RUN_TEST(test_already_applied_batch_skips_download);
    RUN_TEST(test_backoff_retries_once_within_budget);
    RUN_TEST(test_report_without_budget_is_deferred_not_rebooted);
    RUN_TEST(test_follow_up_backoff_retries_the_ack);
    RUN_TEST(test_checkin_null_batch_with_firmware_offer_is_no_action);
    RUN_TEST(test_checkin_with_batch_and_dev_mode);
    RUN_TEST(test_checkin_204_and_429_bodies);
    RUN_TEST(test_checkin_rejects_unreportable_batch_id);
    RUN_TEST(test_server_epoch_rejects_bad_text);
    RUN_TEST(test_offer_escaped_doc_hashes_verbatim);
    RUN_TEST(test_offer_doc_crc_mismatch_is_permanent);
    RUN_TEST(test_offer_batch_mismatch_is_rejected);
    RUN_TEST(test_offer_duplicate_sha_prefix_is_rejected);
    RUN_TEST(test_offer_bad_blob_rows_are_rejected);
    RUN_TEST(test_offer_unreadable_body_is_retryable);
    RUN_TEST(test_rejection_strings_match_server_rule);
    RUN_TEST(test_checkin_body_carries_answer_and_report);
    RUN_TEST(test_checkin_body_omits_unacceptable_answer);
    RUN_TEST(test_checkin_body_carries_hardware_fields_when_present);
    RUN_TEST(test_budget_selection);
    RUN_TEST(test_backoff_retention);
    RUN_TEST(test_applied_notice_only_for_this_sessions_apply);
    RUN_TEST(test_checkin_body_carries_the_mode);
    RUN_TEST(test_dev_poll_follows_the_site_pace);
    RUN_TEST(test_dev_poll_jitter_is_bounded);
    RUN_TEST(test_dev_poll_backs_off_after_failures);
    RUN_TEST(test_dev_poll_honours_retry_after);
    RUN_TEST(test_dev_poll_settles_a_mode_the_site_has_not_taken);
    return UNITY_END();
}
