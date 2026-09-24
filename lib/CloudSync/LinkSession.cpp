// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "LinkSession.h"

namespace CloudSync {

void LinkSession::reply(const std::string& status, const std::string& account,
                        const std::string& token, uint32_t serverTime,
                        const std::string& accountRef) {
    if (phase_ == PairPhase::Linked || phase_ == PairPhase::Declined ||
        phase_ == PairPhase::Expired || phase_ == PairPhase::Error) return;
    if (status == "authorization_pending" || status == "slow_down") return;
    if (status == "confirm_on_device") {
        if (phase_ == PairPhase::Waiting) {
            account_ = account;
            accountRef_ = accountRef;
            linkTime_ = serverTime;
            phase_ = PairPhase::Confirm;
        }
        return;
    }
    if (status == "access_denied") { phase_ = PairPhase::Declined; return; }
    if (status == "expired_token") { phase_ = PairPhase::Expired; return; }
    if (status == "granted" && phase_ == PairPhase::Confirming && token.size() == 43) {
        account_ = account;
        accountRef_ = accountRef;
        token_ = token;
        if (serverTime) linkTime_ = serverTime;
        phase_ = PairPhase::Store;
        return;
    }
    phase_ = PairPhase::Error;
}

void LinkSession::answer(bool accept) {
    if (phase_ != PairPhase::Confirm) return;
    if (!accept) { phase_ = PairPhase::Declined; return; }
    const bool changed = !oldAccountRef_.empty() && !accountRef_.empty()
        ? oldAccountRef_ != accountRef_
        : !oldAccount_.empty() && oldAccount_ != account_;
    phase_ = changed
        ? PairPhase::ClearApps : PairPhase::Confirming;
}

void LinkSession::clearApps(bool clear) {
    if (phase_ != PairPhase::ClearApps) return;
    clearApps_ = clear;
    phase_ = PairPhase::Confirming;
}

void LinkSession::expire() {
    if (phase_ != PairPhase::Linked && phase_ != PairPhase::Declined) phase_ = PairPhase::Expired;
}

void LinkSession::stored() {
    if (phase_ == PairPhase::Store) phase_ = PairPhase::Linked;
}

bool readLink(LinkStore& store, LinkRecord& out) {
    out = LinkRecord();
    if (!store.getBool("ok") || store.getBool("unlk")) return false;
    out.token = store.getString("tok");
    if (out.token.empty()) { out = LinkRecord(); return false; }
    out.account = store.getString("acct");
    out.accountRef = store.getString("aref");
    out.at = store.getUInt("at");
    out.id = store.getString("id");
    out.flashId = store.getString("fid");
    out.serial = store.getString("ser");
    return true;
}

namespace {
bool rememberAccount(LinkStore& store, const LinkRecord& record) {
    if (!(record.accountRef.empty() ? store.remove("prev_aref") :
          store.putString("prev_aref", record.accountRef))) return false;
    if (!(record.account.empty() ? store.remove("prev_acct") :
          store.putString("prev_acct", record.account))) return false;
    return true;
}

bool stageRevoke(LinkStore& store, const std::string& token, bool relinked) {
    return store.putBool("rev_hrel", relinked) && store.putString("rev_hold", token);
}
}

bool hasPendingRevoke(LinkStore& store) {
    return !store.getString("rev_tok").empty() || !store.getString("rev_tok2").empty() ||
           !store.getString("rev_hold").empty();
}

bool recoverPendingRevoke(LinkStore& store) {
    const std::string held = store.getString("rev_hold");
    if (held.empty()) return true;
    if (!store.remove("ok") || !store.remove("tok")) return false;
    const std::string first = store.getString("rev_tok");
    const std::string second = store.getString("rev_tok2");
    if (held != first && held != second) {
        const char* tokenKey = first.empty() ? "rev_tok" : "rev_tok2";
        const char* flagKey = first.empty() ? "rev_rel" : "rev_rel2";
        if (!first.empty() && !second.empty()) return false;
        if (!store.remove(first.empty() ? "rev_fail" : "rev_fail2") ||
            !store.remove(first.empty() ? "rev_at" : "rev_at2") ||
            !store.putBool(flagKey, store.getBool("rev_hrel")) ||
            !store.putString(tokenKey, held)) return false;
    }
    return store.remove("rev_hold") && store.remove("rev_hrel") && store.remove("unlk");
}

bool writeLink(LinkStore& store, const LinkRecord& next) {
    if (next.token.empty()) return false;
    if (!recoverPendingRevoke(store)) return false;
    LinkRecord prior;
    if (readLink(store, prior) && prior.token == next.token) return true;
    if (!prior.token.empty() && prior.token != next.token) {
        if (!store.getString("rev_tok").empty() &&
            !store.getString("rev_tok2").empty()) return false;
        if (!rememberAccount(store, prior)) return false;
        if (!stageRevoke(store, prior.token, true)) return false;
    }
    if (!store.remove("ok")) return false;
    if (!store.remove("tok")) return false;
    if (!recoverPendingRevoke(store)) return false;
    bool saved = next.account.empty() ? store.remove("acct") : store.putString("acct", next.account);
    saved = (next.accountRef.empty() ? store.remove("aref") : store.putString("aref", next.accountRef)) && saved;
    saved = store.putUInt("at", next.at) && saved;
    saved = (next.id.empty() ? store.remove("id") : store.putString("id", next.id)) && saved;
    saved = (next.flashId.empty() ? store.remove("fid") : store.putString("fid", next.flashId)) && saved;
    saved = (next.serial.empty() ? store.remove("ser") : store.putString("ser", next.serial)) && saved;
    if (!saved || !store.putString("tok", next.token)) return false;
    return store.putBool("ok", true);
}

LinkCompletion completeConfirmedLink(LinkStore& store, const LinkRecord& next,
                                     bool clearRequested, bool (*clear)(void*), void* context) {
    if (clearRequested) {
        std::string previous = store.getString("aref");
        if (previous.empty()) previous = store.getString("prev_aref");
        if (previous.empty()) previous = "1";
        if (!store.putString("clr", previous)) return LinkCompletion::StorageFailed;
    }
    if (!writeLink(store, next)) {
        if (clearRequested) store.remove("clr");
        return LinkCompletion::StorageFailed;
    }
    if (clearRequested && !recoverPendingClear(store, clear, context))
        return LinkCompletion::LinkedClearFailed;
    return LinkCompletion::Linked;
}

bool recoverPendingClear(LinkStore& store, bool (*clear)(void*), void* context) {
    if (store.getString("clr").empty()) return true;
    return clear && clear(context) && store.remove("clr");
}

bool prepareUnlink(LinkStore& store) {
    if (!recoverPendingRevoke(store)) return false;
    LinkRecord prior;
    readLink(store, prior);
    const std::string active = store.getString("tok");
    if (!active.empty() && !store.getString("rev_tok").empty() &&
        !store.getString("rev_tok2").empty()) return false;
    if (!prior.token.empty() && !rememberAccount(store, prior)) return false;
    if (!store.putBool("unlk", true)) return false;
    if (!active.empty() && !stageRevoke(store, active, false)) return false;
    if (!store.remove("ok")) return false;
    if (!store.remove("tok")) return false;
    return recoverPendingRevoke(store) && store.remove("unlk");
}

bool finishRevoke(LinkStore& store, int httpStatus) {
    return finishRevoke(store, httpStatus, false);
}

bool pendingRevoke(LinkStore& store, std::string& token, bool& relinked, bool& second) {
    if (!recoverPendingRevoke(store)) return false;
    token = store.getString("rev_tok");
    second = token.empty();
    if (second) token = store.getString("rev_tok2");
    relinked = store.getBool(second ? "rev_rel2" : "rev_rel");
    return !token.empty();
}

bool revokeIsFinal(int httpStatus) {
    return httpStatus == 200 ||
           (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429);
}

bool revokeRetryPending(LinkStore& store, bool second, uint32_t now) {
    const uint32_t until = store.getUInt(second ? "rev_at2" : "rev_at");
    return now > 1577836800 && until > now;
}

namespace {
bool dropRevokeSlot(LinkStore& store, bool second) {
    return store.remove(second ? "rev_tok2" : "rev_tok") &&
           store.remove(second ? "rev_rel2" : "rev_rel") &&
           store.remove(second ? "rev_at2" : "rev_at") &&
           store.remove(second ? "rev_fail2" : "rev_fail");
}
}

bool finishRevoke(LinkStore& store, int httpStatus, bool second) {
    if (!revokeIsFinal(httpStatus)) return false;
    return dropRevokeSlot(store, second);
}

bool replaceTestToken(LinkStore& store, const std::string& token) {
    if (token.empty() || !recoverPendingRevoke(store)) return false;
    LinkRecord record;
    readLink(store, record);
    if (!store.remove("ok") || !store.remove("tok")) return false;
    if (!store.putString("tok", token)) return false;
    return store.putBool("ok", true);
}

bool wipeMismatchedLink(LinkStore& store) {
    LinkRecord active;
    if (readLink(store, active) && !rememberAccount(store, active)) return false;
    const char* const keys[] = {
        "ok", "tok", "acct", "aref", "at", "id", "fid", "ser", "unlk",
        "rev_tok", "rev_rel", "rev_tok2", "rev_rel2", "rev_hold", "rev_hrel", "clr",
        "rev_at", "rev_at2",
        "rev_fail", "rev_fail2"
    };
    for (const char* key : keys) if (!store.remove(key)) return false;
    return true;
}

bool forgetServerUnlinkedLink(LinkStore& store) {
    LinkRecord active;
    if (readLink(store, active) && !rememberAccount(store, active)) return false;
    const char* const keys[] = {"ok", "tok", "acct", "aref", "at"};
    for (const char* key : keys) if (!store.remove(key)) return false;
    if (!hasPendingRevoke(store)) {
        const char* const identity[] = {"id", "fid", "ser"};
        for (const char* key : identity) if (!store.remove(key)) return false;
    }
    return true;
}

CredentialError credentialError(int httpStatus, const std::string& error) {
    if (httpStatus != 401) return CredentialError::Other;
    if (error == "not_linked") return CredentialError::NotLinked;
    if (error == "wrong_device") return CredentialError::WrongDevice;
    return CredentialError::Other;
}

bool revokeOnlySession(LinkStore& store) {
    LinkRecord active;
    return !readLink(store, active) && hasPendingRevoke(store);
}

bool canBeginLink(LinkStore& store) {
    const std::string first = store.getString("rev_tok");
    const std::string second = store.getString("rev_tok2");
    const std::string held = store.getString("rev_hold");
    int used = (first.empty() ? 0 : 1) + (second.empty() ? 0 : 1);
    if (!held.empty() && held != first && held != second) ++used;
    return used < 2;
}

bool shouldSendRevoke(const std::string& pending, const std::string& active) {
    return !pending.empty() && pending != active;
}

bool pairingMayContinue(bool confirmSent, uint32_t codeAgeMs, uint32_t expiryMs,
                        uint32_t sessionAgeMs, uint32_t sessionLimitMs) {
    return sessionAgeMs < sessionLimitMs && (confirmSent || codeAgeMs < expiryMs);
}

uint32_t confirmSessionLimit(uint32_t limitMs, uint32_t elapsedMs, uint32_t graceMs) {
    const uint32_t floor = elapsedMs + graceMs;
    return floor > limitMs ? floor : limitMs;
}

uint32_t pairPollWait(uint32_t interval, int httpStatus, int retryAfter) {
    return (httpStatus == 429 || httpStatus >= 500) && retryAfter > 0
        ? (uint32_t)retryAfter : interval;
}

bool setLinkTimeIfMissing(LinkStore& store, uint32_t serverTime) {
    if (!serverTime || !store.getBool("ok") || store.getBool("unlk") ||
        store.getString("tok").empty()) return false;
    if (store.getUInt("at")) return true;
    return store.putUInt("at", serverTime);
}

} // namespace CloudSync
