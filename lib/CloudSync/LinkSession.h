// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef LINK_SESSION_H
#define LINK_SESSION_H

#include <stdint.h>
#include <string>

namespace CloudSync {

enum class PairPhase : uint8_t { Waiting, Confirm, ClearApps, Confirming, Store, Linked, Declined, Expired, Error };

class LinkSession {
public:
    explicit LinkSession(const std::string& oldAccount, const std::string& oldAccountRef = "")
        : oldAccount_(oldAccount), oldAccountRef_(oldAccountRef) {}
    PairPhase phase() const { return phase_; }
    void reply(const std::string& status, const std::string& account,
               const std::string& token, uint32_t serverTime,
               const std::string& accountRef = "");
    void answer(bool accept);
    void clearApps(bool clear);
    void expire();
    void stored();
    const std::string& account() const { return account_; }
    const std::string& accountRef() const { return accountRef_; }
    const std::string& token() const { return token_; }
    uint32_t linkTime() const { return linkTime_; }
    bool shouldClearApps() const { return clearApps_; }
private:
    PairPhase phase_ = PairPhase::Waiting;
    std::string oldAccount_;
    std::string oldAccountRef_;
    std::string account_;
    std::string accountRef_;
    std::string token_;
    uint32_t linkTime_ = 0;
    bool clearApps_ = false;
};

class LinkStore {
public:
    virtual ~LinkStore() = default;
    virtual std::string getString(const char* key) = 0;
    virtual uint32_t getUInt(const char* key) = 0;
    virtual bool getBool(const char* key) = 0;
    virtual bool putString(const char* key, const std::string& value) = 0;
    virtual bool putUInt(const char* key, uint32_t value) = 0;
    virtual bool putBool(const char* key, bool value) = 0;
    virtual bool remove(const char* key) = 0;
};

struct LinkRecord {
    std::string token, account, accountRef, id, flashId, serial;
    uint32_t at = 0;
};

enum class LinkCompletion : uint8_t { StorageFailed, Linked, LinkedClearFailed };

// The marker is committed last and removed before any replacement write.
bool readLink(LinkStore& store, LinkRecord& out);
bool writeLink(LinkStore& store, const LinkRecord& next);
LinkCompletion completeConfirmedLink(LinkStore& store, const LinkRecord& next,
                                     bool clearRequested, bool (*clear)(void*), void* context);
bool prepareUnlink(LinkStore& store);
bool finishRevoke(LinkStore& store, int httpStatus);
bool recoverPendingRevoke(LinkStore& store);
bool hasPendingRevoke(LinkStore& store);
bool pendingRevoke(LinkStore& store, std::string& token, bool& relinked, bool& second);
bool finishRevoke(LinkStore& store, int httpStatus, bool second);
// A final answer ends a pending revoke: 200, or a 4xx other than 408/429.
bool revokeIsFinal(int httpStatus);
// Counts one failed drain of a slot; true when the slot was given up.
constexpr uint32_t kRevokeMaxFailures = 10;
bool noteRevokeFailure(LinkStore& store, bool second);
bool replaceTestToken(LinkStore& store, const std::string& token);
bool wipeMismatchedLink(LinkStore& store);
bool revokeOnlySession(LinkStore& store);
// True while at least one pending-revoke slot is free.
bool canBeginLink(LinkStore& store);
bool shouldSendRevoke(const std::string& pending, const std::string& active);
bool pairingMayContinue(bool confirmSent, uint32_t codeAgeMs, uint32_t expiryMs,
                        uint32_t sessionAgeMs, uint32_t sessionLimitMs);
// Once confirm is first sent, the session keeps at least graceMs more.
uint32_t confirmSessionLimit(uint32_t limitMs, uint32_t elapsedMs, uint32_t graceMs);
uint32_t pairPollWait(uint32_t interval, int httpStatus, int retryAfter);
bool setLinkTimeIfMissing(LinkStore& store, uint32_t serverTime);

} // namespace CloudSync

#endif // LINK_SESSION_H
