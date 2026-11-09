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
    bool waiting = false;        ///< unsealed app changes remain on the site
    bool sendReport = false;
    bool firmwareOffer = false;
    std::string firmwareUrl;
    std::string mode;
    uint32_t nextPollMs = 0;
    bool hasServerTime = false; ///< body contains server_time, even if unreadable
    uint32_t serverEpoch = 0;   ///< 0 when absent or unreadable
    /// usage_share_set: the owner changed the usage-sharing setting on the
    /// site. Set only for a well-formed request ("on"|"off" and a whole rev).
    bool hasUsageShareSet = false;
    bool usageShareOn = false;
    uint32_t usageShareRev = 0;
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
    /// "normal", "dev" or "always" (what the Awake & dev mode setting says);
    /// null or empty sends no mode.
    const char* mode = nullptr;
    bool applyApps = true;        ///< this session may apply app changes
    /// The usage-sharing setting: "on", "off" or "unset" (usageShareText).
    const char* usageShare = "unset";
    uint32_t usageRev = 0;        ///< last usage_share_set rev applied; 0 if none
};

/// The check-in POST body. Empty on an allocation failure.
std::string buildCheckinBody(const CheckinFields& fields);

/// The usage-sharing setting (upd.usage_share) as reported: "unset" when the
/// key does not exist, else "on" or "off". Unset is treated as off wherever
/// the setting gates sharing.
const char* usageShareText(bool present, bool on);

/// What to do with a reply's usage_share_set request. When in doubt, off wins:
/// - no request, or rev not greater than `appliedRev` (equal = already
///   applied, lower = stale; rev 0 never applies): Ignore;
/// - "off": Apply (turning sharing off is always safe);
/// - "on" while a local change the site has not seen yet is pending
///   (`localGen` != `seenGen`): Consume - store the rev, keep the setting;
/// - "on" otherwise: Apply.
/// `localGen` (upd.usage_lgen) counts every change made on the device;
/// `seenGen` (upd.usage_sgen) is the count the last successful check-in sent.
enum class UsageShareAction : uint8_t { Ignore, Apply, Consume };
UsageShareAction usageShareAction(const CheckinReply& reply, uint32_t appliedRev,
                                  uint32_t localGen, uint32_t seenGen);

/// The usage-sharing keys, so the write order is host-tested.
struct UsageShareStore {
    virtual ~UsageShareStore() = default;
    virtual bool putRev(uint32_t rev) = 0;           ///< upd.usage_rev
    virtual bool putShare(bool on) = 0;              ///< upd.usage_share
    virtual bool putLocalGen(uint32_t gen) = 0;      ///< upd.usage_lgen
    virtual bool putSeenGen(uint32_t gen) = 0;       ///< upd.usage_sgen
};

enum class UsageShareResult : uint8_t {
    Ignored,      ///< nothing written
    Applied,      ///< rev stored, then the setting
    Consumed,     ///< rev stored, setting left as it is
    RevFailed,    ///< the rev could not be stored: nothing changed
    ShareFailed,  ///< rev stored but the setting write failed: consumed unapplied
};

/// Applies a usage_share_set request. The rev is written (and checked) FIRST,
/// so a request can never be replayed over a later local change; the next
/// check-in reports the real setting with the new rev either way.
UsageShareResult applyUsageShare(const CheckinReply& reply, uint32_t appliedRev,
                                 uint32_t localGen, uint32_t seenGen, UsageShareStore& store);

/// A change made on the device (Settings toggle or the USB `usage` verb):
/// the local generation is bumped FIRST, then the setting is written. If the
/// setting write fails after the bump, an "on" request is only consumed until
/// the next check-in reports the real value. False when either write failed.
bool recordLocalUsageShare(bool on, uint32_t localGen, UsageShareStore& store);

/// After a successful check-in whose body carried `sentGen` (usage_lgen when
/// the body was built). The reply's request is decided with `seenGen` from
/// BEFORE this check-in: the site built the reply before it had seen
/// `sentGen`, so a request in it may predate a device change. Only once the
/// request is handled (ignored, or its rev stored) does usage_sgen advance to
/// `sentGen`, and only when that changes it. A failed rev write
/// (RevFailed) leaves usage_sgen where it was, so the same request is decided
/// the same way next time.
UsageShareResult settleUsageAfterCheckin(const CheckinReply& reply, uint32_t appliedRev,
                                         uint32_t localGen, uint32_t seenGen,
                                         uint32_t sentGen, UsageShareStore& store);

/// RAM-only report history for one worker session. A fresh session starts
/// without an acknowledged full report, even if a prior boot sent one.
struct ReportState {
    bool hasFullCrc = false;
    uint32_t fullCrc = 0;
    bool sendReport = false;
    bool lastFullFailed = false;
    std::string lastAckBatch;  ///< Last batch accepted by a 2xx in this session (at most 40 chars)
};

/// Whether this check-in must include installed. `ackBatch` is the valid
/// applied_batch sent with a result, or null when there is no answer.
bool fullReportRequired(const ReportState& state, uint32_t manifestCrc, const char* ackBatch);

/// Pure transition after one HTTP attempt. A transport failure uses status 0;
/// sendReport is taken only from a successfully parsed 2xx reply.
ReportState reportAfterCheckin(const ReportState& state, uint32_t manifestCrc,
                               bool sentFull, int status, bool sendReport,
                               const char* ackBatch = nullptr);

/// Retain a completed dev check-in connection only across a short wait.
constexpr uint32_t kDevKeepConnectionMs = 5000;
bool keepDevConnection(int status, bool cycleOk, uint32_t waitMs,
                       bool wifiConnected, bool cancelled);

/// Seconds since the epoch for "YYYY-MM-DDTHH:MM:SSZ", or 0.
uint32_t serverEpoch(const char* iso);

/// RFC 7231 IMF-fixdate (including a matching weekday), UTC epoch, or 0.
uint32_t httpDateEpoch(const char* date);

/// True for a device the server polls every 2 s (dev / always modes).
bool devMode(const std::string& mode, uint32_t nextPollMs);

/// Minimum wait between a check-in and the follow-up that answers it.
uint32_t followupFloorMs(const CheckinReply& reply);

/// Dev mode listening: the wait before the next check-in. A success uses the
/// site's next_poll_ms held to [kDevPollMs, kDevMaxPollMs] (a missing value
/// is kDevPollMs). After `failures` consecutive failures the wait doubles
/// from kDevPollMs up to kDevMaxBackoffMs, and an error reply's own
/// next_poll_ms is honoured when it is longer. A Retry-After is honoured up
/// to kDevMaxRetryMs. `jitter` (any number; the device passes a random one)
/// adds 0-10 % so many Fidgets do not poll in step.
constexpr uint32_t kDevMaxPollMs = 30000;
constexpr uint32_t kDevMaxBackoffMs = 60000;
constexpr uint32_t kDevMaxRetryMs = 300000;
uint32_t devPollWaitMs(uint32_t serverNextMs, uint8_t failures, int retryAfterSec,
                       uint32_t jitter, bool modeNotTaken = false);
/// `modeNotTaken`: the site answered with a mode other than the one sent (it
/// takes a mode change at most once per 10 s, and answers the old mode's
/// pace until then). The next check-in comes after kDevModeSettleMs at most,
/// so switching dev mode on is not held up by a normal-mode pace.
constexpr uint32_t kDevModeSettleMs = 11000;
/// Whether a dev check-in's answer shows the site has not taken the sent
/// mode yet: a 200 naming another mode, or any answer at a normal-mode pace
/// (an hour or more; a 204 carries no mode, only X-Next-Poll-Ms).
bool devModeNotTaken(int status, const std::string& replyMode, const char* sentMode,
                     uint32_t nextPollMs);

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
