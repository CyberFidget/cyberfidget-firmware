// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CLOUD_PROTOCOL_H
#define CLOUD_PROTOCOL_H

// The site's device check-in and loadout wire format: parsing, request body
// building, and wait selection. Pure C++17 + cJSON (plain C), so the native
// test_sync suite runs it on the host with fixture bodies.

#include <stdint.h>
#include <string>
#include <vector>

#include "LoadoutManifest.h"

namespace CloudSync {

/// The server's follow-up floor is 60 s (2 s for a device in a dev mode);
/// it compares whole seconds, so each floor carries a margin.
constexpr uint32_t kDevPollMs = 2000;
constexpr uint32_t kNormalFloorMs = 61000;
constexpr uint32_t kDevFloorMs = 3000;
/// Longest Retry-After the session will sleep through in place.
constexpr int kMaxRetryAfterSec = 90;

/// A 200 check-in answer. A 204 has no body: every field keeps its default
/// and nextPollMs comes from the X-Next-Poll-Ms header.
struct CheckinReply {
    bool hasBatch = false;
    std::string batchId;
    bool sendReport = false;
    bool firmwareOffer = false;
    std::string firmwareUrl;
    std::string mode;
    uint32_t nextPollMs = 0;
    uint32_t serverEpoch = 0;   ///< 0 when absent or unreadable
};

/// Parses a check-in body. `headerNextMs` is X-Next-Poll-Ms (0 when absent)
/// and is used when the body has no next_poll_ms. A null or empty body is a
/// valid no-change answer. False on malformed JSON or a batch id the server
/// could not accept back as applied_batch.
bool parseCheckin(const char* body, uint32_t headerNextMs, CheckinReply& out);

struct BlobOffer {
    std::string sha256;                 ///< 64 lowercase hex
    uint32_t size = 0;
    std::string url;                    ///< server-relative, same origin
    std::vector<std::string> targets;   ///< doc blob paths ending <sha8>.wasm
};

struct Offer {
    std::string doc;        ///< exact document bytes after JSON un-escaping
    uint32_t docCrc = 0;
    std::vector<BlobOffer> blobs;
    uint32_t nextPollMs = 0;
};

enum class OfferError : uint8_t {
    None,
    Json,        ///< unreadable body: treated as a transport failure
    DocCrc,      ///< doc missing or doc_crc does not match its bytes
    Batch,       ///< doc batch/base missing or batch differs from check-in
    BlobEntry,   ///< a malformed blob row (sha, size, url)
    BlobPrefix,  ///< two different hashes share an 8-hex prefix
    BlobPath,    ///< a doc blob path outside the write roots
    BlobTarget,  ///< an offered blob no doc path points at
};

/// Parses and cross-checks a loadout offer against the check-in batch id.
OfferError parseOffer(const char* body, const char* batchId, Offer& out);

/// True when the offer can never succeed as sent (answered as rejected).
bool offerErrorPermanent(OfferError e);

/// The rejected:<reason> result for a permanent error, else nullptr.
const char* offerRejection(OfferError e);

/// Same rule as the server's applied_batch check: [A-Za-z0-9_-]{1,40}.
bool validBatchId(const char* id);

/// Same rule as the server's result check.
bool validResult(const char* result);

struct CheckinFields {
    const char* deviceId = "";
    const char* flashId = "";
    const char* serial = "";
    const char* fw = "";
    const char* abi = "";
    const char* board = "";
    uint32_t fsTotal = 0;
    uint32_t fsUsed = 0;
    uint32_t manifestCrc = 0;
    const char* appliedBatch = nullptr;   ///< with result, or both null
    const char* result = nullptr;
    const LoadoutManifest::Loadout* installed = nullptr;  ///< null = no report
};

/// The check-in POST body. Empty on an allocation failure.
std::string buildCheckinBody(const CheckinFields& fields);

/// Seconds since the epoch for "YYYY-MM-DDTHH:MM:SSZ", or 0.
uint32_t serverEpoch(const char* iso);

/// True for a device the server polls every 2 s (dev / always modes).
bool devMode(const std::string& mode, uint32_t nextPollMs);

/// Minimum wait between a check-in and the follow-up that answers it.
uint32_t followupFloorMs(const CheckinReply& reply);

/// Sleep for a Retry-After value, or 0 when it is not one to wait through.
uint32_t retryWaitMs(int retryAfterSec);

/// What a reply does to the stored backoff.
enum class BackoffAction : uint8_t { Keep, Store, Clear };

/// Any reply carrying Retry-After (a 429, or a 503 the site sends with 60 or
/// 3600) stores it; a success (200/204) clears an old one; any other reply
/// leaves an existing backoff in place.
BackoffAction backoffFor(int status, int retryAfterSec);

/// True when `waitMs` plus `reserveMs` for the request itself still fits in
/// the session budget after `elapsedMs`.
bool budgetCovers(uint32_t waitMs, uint32_t elapsedMs, uint32_t sessionMs,
                  uint32_t reserveMs);

} // namespace CloudSync

#endif
