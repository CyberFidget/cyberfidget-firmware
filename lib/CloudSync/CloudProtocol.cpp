// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "CloudProtocol.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#include "SyncProtocol.h"

namespace CloudSync {
namespace {

const char* str(const cJSON* root, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool num(const cJSON* root, const char* key, uint32_t& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble > 4294967295.0) return false;
    out = (uint32_t)item->valuedouble;
    return true;
}

bool lowerHex(const char* s, size_t len) {
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

bool hex32(const char* s, uint32_t& out) {
    if (!lowerHex(s, 8)) return false;
    uint32_t n = 0;
    for (int i = 0; i < 8; ++i)
        n = (n << 4) | (uint32_t)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10);
    out = n;
    return true;
}

// A same-origin relative URL of plain query characters. The site's blob
// rows are "/api/device-loadout.php?device_id=..&blob=<sha256>".
bool relativeUrl(const char* url, const char* prefix) {
    if (!url || strncmp(url, prefix, strlen(prefix)) != 0 || strlen(url) > 200) return false;
    for (const char* p = url; *p; ++p) {
        const char c = *p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || strchr("/._-?=&%", c) != nullptr;
        if (!plain) return false;
    }
    return strstr(url, "//") == nullptr && strstr(url, "..") == nullptr;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() &&
           s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

bool validBatchId(const char* id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n < 1 || n > 40) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

bool validResult(const char* result) {
    if (!result) return false;
    if (strcmp(result, "applied") == 0 || strcmp(result, "already-applied") == 0 ||
        strcmp(result, "stale-revision") == 0) return true;
    if (strncmp(result, "rejected:", 9) != 0) return false;
    const char* reason = result + 9;
    const size_t n = strlen(reason);
    if (n < 1 || n > 64) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = reason[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

uint32_t serverEpoch(const char* iso) {
    if (!iso) return 0;
    int y = 0, m = 0, d = 0, hour = 0, minute = 0, second = 0;
    char z = 0;
    if (strlen(iso) != 20 ||
        sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &m, &d, &hour, &minute,
               &second, &z) != 7 || z != 'Z' || y < 2020 || y > 2099 ||
        m < 1 || m > 12 || d < 1 || d > 31 || hour > 23 || minute > 59 || second > 59)
        return 0;
    // Gregorian days from civil date, relative to 1970-01-01.
    y -= m <= 2;
    const int era = y / 400;
    const unsigned yearOfEra = (unsigned)(y - era * 400);
    const unsigned dayOfYear = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = (int64_t)era * 146097 + dayOfEra - 719468;
    const int64_t epoch = days * 86400 + hour * 3600 + minute * 60 + second;
    return epoch > 1577836800 && epoch <= 4294967295LL ? (uint32_t)epoch : 0;
}

bool parseCheckin(const char* body, uint32_t headerNextMs, CheckinReply& out) {
    out = CheckinReply();
    out.nextPollMs = headerNextMs;
    if (!body || !*body) return true;
    cJSON* root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    bool ok = true;
    const cJSON* batch = cJSON_GetObjectItemCaseSensitive(root, "batch_id");
    if (cJSON_IsString(batch)) {
        ok = validBatchId(batch->valuestring);
        out.hasBatch = ok;
        if (ok) out.batchId = batch->valuestring;
    } else if (batch && !cJSON_IsNull(batch)) {
        ok = false;
    }
    out.sendReport = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "send_report"));
    const cJSON* fw = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (cJSON_IsObject(fw) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fw, "offer"))) {
        const char* url = str(fw, "url");
        if (relativeUrl(url, "/")) {
            out.firmwareOffer = true;
            out.firmwareUrl = url;
        }
    }
    const char* mode = str(root, "mode");
    if (mode) out.mode = mode;
    num(root, "next_poll_ms", out.nextPollMs);
    out.serverEpoch = serverEpoch(str(root, "server_time"));
    cJSON_Delete(root);
    return ok;
}

OfferError parseOffer(const char* body, const char* batchId, Offer& out) {
    out = Offer();
    cJSON* root = body ? cJSON_Parse(body) : nullptr;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return OfferError::Json; }
    OfferError err = OfferError::None;
    num(root, "next_poll_ms", out.nextPollMs);
    const char* doc = str(root, "doc");
    LoadoutManifest::OpsMeta meta;
    std::vector<std::string> paths;
    const cJSON* blobs = cJSON_GetObjectItemCaseSensitive(root, "blobs");
    do {
        if (!doc || !hex32(str(root, "doc_crc"), out.docCrc) ||
            SyncProtocol::crc32(doc, strlen(doc)) != out.docCrc) {
            err = OfferError::DocCrc; break;
        }
        out.doc = doc;
        if (!batchId || !LoadoutManifest::parseOpsMeta(doc, meta) ||
            !meta.hasBatch || !meta.hasBase || meta.batch != batchId) {
            err = OfferError::Batch; break;
        }
        if (!LoadoutManifest::collectOpBlobPaths(doc, paths)) { err = OfferError::BlobPath; break; }
        for (const auto& p : paths)
            if (!SyncProtocol::pathConfined(p.c_str())) { err = OfferError::BlobPath; break; }
        if (err != OfferError::None) break;
        if (!cJSON_IsArray(blobs)) { err = OfferError::BlobEntry; break; }
        for (const cJSON* row = blobs->child; row; row = row->next) {
            BlobOffer blob;
            const char* sha = str(row, "sha256");
            const char* url = str(row, "url");
            if (!cJSON_IsObject(row) || !lowerHex(sha, 64) || !num(row, "size", blob.size) ||
                blob.size == 0 || !relativeUrl(url, "/api/device-loadout.php?")) {
                err = OfferError::BlobEntry; break;
            }
            blob.sha256 = sha;
            blob.url = url;
            for (const auto& prior : out.blobs) {
                if (prior.sha256.compare(0, 8, blob.sha256, 0, 8) == 0 &&
                    prior.sha256 != blob.sha256) { err = OfferError::BlobPrefix; break; }
            }
            if (err != OfferError::None) break;
            const std::string tail = "-" + blob.sha256.substr(0, 8) + ".wasm";
            for (const auto& p : paths)
                if (endsWith(p, tail)) blob.targets.push_back(p);
            if (blob.targets.empty()) { err = OfferError::BlobTarget; break; }
            out.blobs.push_back(blob);
        }
    } while (false);
    cJSON_Delete(root);
    return err;
}

bool offerErrorPermanent(OfferError e) {
    return e != OfferError::None && e != OfferError::Json;
}

const char* offerRejection(OfferError e) {
    switch (e) {
        case OfferError::DocCrc:     return "rejected:offer-crc";
        case OfferError::Batch:      return "rejected:offer-batch";
        case OfferError::BlobEntry:  return "rejected:blob-offer";
        case OfferError::BlobPrefix: return "rejected:blob-prefix";
        case OfferError::BlobPath:   return "rejected:blob-path";
        case OfferError::BlobTarget: return "rejected:blob-target";
        default:                     return nullptr;
    }
}

std::string buildCheckinBody(const CheckinFields& f) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return std::string();
    char crcText[9];
    snprintf(crcText, sizeof(crcText), "%08x", (unsigned)f.manifestCrc);
    cJSON_AddStringToObject(root, "device_id", f.deviceId);
    if (f.flashId && *f.flashId) cJSON_AddStringToObject(root, "flash_id", f.flashId);
    if (f.serial && *f.serial) cJSON_AddStringToObject(root, "serial", f.serial);
    cJSON_AddStringToObject(root, "fw", f.fw);
    cJSON_AddStringToObject(root, "abi", f.abi);
    cJSON_AddStringToObject(root, "board_rev", f.board);
    cJSON_AddNumberToObject(root, "fs_total", f.fsTotal);
    cJSON_AddNumberToObject(root, "fs_used", f.fsUsed);
    cJSON_AddStringToObject(root, "lapply_cap", SyncProtocol::kLapplyCapability);
    cJSON_AddStringToObject(root, "manifest_crc", crcText);
    if (f.mode && *f.mode) cJSON_AddStringToObject(root, "mode", f.mode);
    if (f.appliedBatch && f.result && validBatchId(f.appliedBatch) && validResult(f.result)) {
        cJSON_AddStringToObject(root, "applied_batch", f.appliedBatch);
        cJSON_AddStringToObject(root, "result", f.result);
    }
    if (f.installed) {
        cJSON* installed = cJSON_AddArrayToObject(root, "installed");
        for (const auto& e : f.installed->entries) {
            cJSON* row = cJSON_CreateObject();
            if (!row) continue;
            cJSON_AddStringToObject(row, "id", e.id.c_str());
            if (!e.name.empty()) cJSON_AddStringToObject(row, "name", e.name.c_str());
            if (!e.category.empty()) cJSON_AddStringToObject(row, "category", e.category.c_str());
            if (!e.version.empty()) cJSON_AddStringToObject(row, "version", e.version.c_str());
            if (!e.abi.empty()) cJSON_AddStringToObject(row, "abi", e.abi.c_str());
            if (!e.format.empty()) cJSON_AddStringToObject(row, "format", e.format.c_str());
            if (!e.blobPath.empty()) cJSON_AddStringToObject(row, "blobPath", e.blobPath.c_str());
            cJSON_AddBoolToObject(row, "hidden", e.hidden);
            cJSON_AddNumberToObject(row, "position", e.position);
            cJSON_AddItemToArray(installed, row);
        }
    }
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded ? std::string(encoded) : std::string();
    cJSON_free(encoded);
    cJSON_Delete(root);
    return body;
}

bool devMode(const std::string& mode, uint32_t nextPollMs) {
    return mode == "dev" || mode == "always" || mode == "dev-always" ||
           nextPollMs == kDevPollMs;
}

uint32_t followupFloorMs(const CheckinReply& reply) {
    return devMode(reply.mode, reply.nextPollMs) ? kDevFloorMs : kNormalFloorMs;
}

uint32_t devPollWaitMs(uint32_t serverNextMs, uint8_t failures, int retryAfterSec,
                       uint32_t jitter, bool modeNotTaken) {
    uint32_t wait = serverNextMs ? serverNextMs : kDevPollMs;
    if (wait < kDevPollMs) wait = kDevPollMs;
    if (wait > kDevMaxPollMs) wait = kDevMaxPollMs;
    if (failures) {
        const uint8_t doublings = failures > 5 ? 5 : failures;
        uint32_t backoff = kDevPollMs << doublings;
        if (backoff > kDevMaxBackoffMs) backoff = kDevMaxBackoffMs;
        if (backoff > wait) wait = backoff;
    }
    if (retryAfterSec > 0) {
        const uint64_t retryMs = (uint64_t)retryAfterSec * 1000u;
        const uint32_t capped = retryMs > kDevMaxRetryMs ? kDevMaxRetryMs : (uint32_t)retryMs;
        if (capped > wait) wait = capped;
    }
    if (modeNotTaken && !failures && retryAfterSec <= 0 && wait > kDevModeSettleMs)
        wait = kDevModeSettleMs;
    return wait + jitter % (wait / 10 + 1);
}

bool devModeNotTaken(int status, const std::string& replyMode, const char* sentMode,
                     uint32_t nextPollMs) {
    if (status != 200 && status != 204) return false;
    if (status == 200 && !replyMode.empty() && sentMode && replyMode != sentMode) return true;
    return nextPollMs >= 3600000u;
}

uint32_t retryWaitMs(int retryAfterSec) {
    if (retryAfterSec < 1 || retryAfterSec > kMaxRetryAfterSec) return 0;
    return (uint32_t)retryAfterSec * 1000 + 1000;
}

BackoffAction backoffFor(int status, int retryAfterSec) {
    if (retryAfterSec > 0) return BackoffAction::Store;
    if (status == 200 || status == 204) return BackoffAction::Clear;
    return BackoffAction::Keep;
}

bool budgetCovers(uint32_t waitMs, uint32_t elapsedMs, uint32_t sessionMs,
                  uint32_t reserveMs) {
    if (elapsedMs >= sessionMs) return false;
    return (uint64_t)waitMs + reserveMs <= (uint64_t)(sessionMs - elapsedMs);
}

} // namespace CloudSync
