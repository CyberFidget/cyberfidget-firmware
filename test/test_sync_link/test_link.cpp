// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "LinkSession.h"

using namespace CloudSync;

class MemoryLinkStore : public LinkStore {
public:
    std::map<std::string, std::string> values;
    std::string failKey;
    std::vector<std::string> events;
    int commits = 0;
    std::string getString(const char* key) override { return values[key]; }
    uint32_t getUInt(const char* key) override {
        const std::string value = values[key];
        return value.empty() ? 0 : (uint32_t)std::stoul(value);
    }
    bool getBool(const char* key) override { return values[key] == "1"; }
    bool putString(const char* key, const std::string& value) override {
        if (failKey == key) return false;
        events.push_back(std::string("put:") + key);
        values[key] = value;
        if (std::string(key) == "ok") ++commits;
        return true;
    }
    bool putUInt(const char* key, uint32_t value) override {
        return putString(key, std::to_string(value));
    }
    bool putBool(const char* key, bool value) override {
        return putString(key, value ? "1" : "0");
    }
    bool remove(const char* key) override {
        events.push_back(std::string("remove:") + key);
        values.erase(key); return true;
    }
};

LinkRecord record(char token, const char* account) {
    LinkRecord r;
    r.token = std::string(43, token);
    r.account = account;
    r.id = "a1b2c3d4e5f6";
    r.flashId = "0123456789abcdef";
    r.serial = "12345678";
    return r;
}

void test_wait_confirm_store_and_lost_reply() {
    LinkSession session("");
    session.reply("authorization_pending", "", "", 0);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Waiting, (int)session.phase());
    session.reply("confirm_on_device", "alice", "", 0);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirm, (int)session.phase());
    session.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)session.phase());
    session.reply("confirm_on_device", "alice", "", 0);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)session.phase());
    session.reply("granted", "alice", std::string(43, 'b'), 1770000000);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Store, (int)session.phase());
    MemoryLinkStore store;
    LinkRecord next = record('b', "alice");
    next.at = session.linkTime();
    TEST_ASSERT_TRUE(writeLink(store, next));
    session.stored();
    session.reply("granted", "alice", std::string(43, 'b'), 1770000000);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Linked, (int)session.phase());
    TEST_ASSERT_TRUE(writeLink(store, next));
    TEST_ASSERT_EQUAL_INT(1, store.commits);
    LinkRecord read;
    TEST_ASSERT_TRUE(readLink(store, read));
    TEST_ASSERT_EQUAL_UINT32(1770000000, read.at);
}

void test_decline_and_expiry() {
    LinkSession declined("");
    declined.reply("confirm_on_device", "alice", "", 0);
    declined.answer(false);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Declined, (int)declined.phase());
    LinkSession expired("");
    expired.expire();
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Expired, (int)expired.phase());
    LinkSession serverExpired("");
    serverExpired.reply("expired_token", "", "", 0);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Expired, (int)serverExpired.phase());
}

void test_change_of_hands_decision() {
    LinkSession different("alice");
    different.reply("confirm_on_device", "bob", "", 0);
    different.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::ClearApps, (int)different.phase());
    different.clearApps(true);
    TEST_ASSERT_TRUE(different.shouldClearApps());
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)different.phase());
    LinkSession keep("alice");
    keep.reply("confirm_on_device", "bob", "", 0);
    keep.answer(true);
    keep.clearApps(false);
    TEST_ASSERT_FALSE(keep.shouldClearApps());
    LinkSession same("alice");
    same.reply("confirm_on_device", "alice", "", 0);
    same.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)same.phase());
}

void test_account_reference_decides_change_of_hands() {
    LinkSession renamed("alice", "0123456789abcdef");
    renamed.reply("confirm_on_device", "alice-new", "", 1770000000, "0123456789abcdef");
    renamed.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)renamed.phase());

    LinkSession sameLabelDifferentAccount("alice", "0123456789abcdef");
    sameLabelDifferentAccount.reply("confirm_on_device", "alice", "", 1770000000,
                                    "fedcba9876543210");
    sameLabelDifferentAccount.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::ClearApps, (int)sameLabelDifferentAccount.phase());

    LinkSession legacySameLabel("alice");
    legacySameLabel.reply("confirm_on_device", "alice", "", 1770000000,
                          "fedcba9876543210");
    legacySameLabel.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)legacySameLabel.phase());

    LinkSession legacyDifferentLabel("alice");
    legacyDifferentLabel.reply("confirm_on_device", "bob", "", 1770000000,
                               "fedcba9876543210");
    legacyDifferentLabel.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::ClearApps, (int)legacyDifferentLabel.phase());
}

void test_account_reference_and_server_time_are_stored() {
    LinkSession session("");
    session.reply("confirm_on_device", "alice", "", 1770000000, "0123456789abcdef");
    session.answer(true);
    session.reply("granted", "alice", std::string(43, 'b'), 1770000001,
                  "0123456789abcdef");
    TEST_ASSERT_EQUAL_UINT32(1770000001, session.linkTime());
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", session.accountRef().c_str());
    MemoryLinkStore store;
    LinkRecord next = record('b', "alice");
    next.accountRef = session.accountRef();
    next.at = session.linkTime();
    TEST_ASSERT_TRUE(writeLink(store, next));
    LinkRecord read;
    TEST_ASSERT_TRUE(readLink(store, read));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", read.accountRef.c_str());
    TEST_ASSERT_EQUAL_UINT32(1770000001, read.at);

    LinkSession earlierTime("");
    earlierTime.reply("confirm_on_device", "alice", "", 1770000000,
                      "0123456789abcdef");
    earlierTime.answer(true);
    earlierTime.reply("granted", "alice", std::string(43, 'c'), 0,
                      "0123456789abcdef");
    TEST_ASSERT_EQUAL_UINT32(1770000000, earlierTime.linkTime());

    LinkSession noTime("");
    noTime.reply("confirm_on_device", "alice", "", 0, "0123456789abcdef");
    noTime.answer(true);
    noTime.reply("granted", "alice", std::string(43, 'd'), 0,
                 "0123456789abcdef");
    TEST_ASSERT_EQUAL_UINT32(0, noTime.linkTime());
}

void test_account_reference_write_failure_hides_partial_link() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    LinkRecord next = record('b', "bob");
    next.accountRef = "fedcba9876543210";
    store.failKey = "aref";
    TEST_ASSERT_FALSE(writeLink(store, next));
    LinkRecord read;
    TEST_ASSERT_FALSE(readLink(store, read));
    store.failKey.clear();
    TEST_ASSERT_TRUE(writeLink(store, next));
    TEST_ASSERT_TRUE(readLink(store, read));
    TEST_ASSERT_EQUAL_STRING("fedcba9876543210", read.accountRef.c_str());
}

void test_relink_revoke_retry_and_401() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    TEST_ASSERT_TRUE(writeLink(store, record('b', "bob")));
    LinkRecord active;
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_EQUAL_STRING("bob", active.account.c_str());
    TEST_ASSERT_EQUAL_STRING(std::string(43, 'a').c_str(), store.getString("rev_tok").c_str());
    TEST_ASSERT_TRUE(store.getBool("rev_rel"));
    TEST_ASSERT_FALSE(finishRevoke(store, 0));
    TEST_ASSERT_FALSE(finishRevoke(store, 503));
    TEST_ASSERT_TRUE(finishRevoke(store, 401));
    TEST_ASSERT_TRUE(store.getString("rev_tok").empty());
    TEST_ASSERT_TRUE(readLink(store, active));
}

void test_unlink_network_failure_is_local_first() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    TEST_ASSERT_TRUE(prepareUnlink(store));
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_FALSE(store.getBool("rev_rel"));
    TEST_ASSERT_FALSE(store.getString("rev_tok").empty());
    TEST_ASSERT_FALSE(finishRevoke(store, 0));
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_TRUE(finishRevoke(store, 200));
    TEST_ASSERT_TRUE(store.getString("rev_tok").empty());
}

void test_unlink_intent_hides_link_before_revoke_is_saved() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    store.failKey = "rev_tok";
    TEST_ASSERT_FALSE(prepareUnlink(store));
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_TRUE(store.getBool("unlk"));
    store.failKey.clear();
    TEST_ASSERT_TRUE(prepareUnlink(store));
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_FALSE(store.getString("rev_tok").empty());
}

void test_reader_rejects_torn_link_and_time_uses_server() {
    MemoryLinkStore store;
    store.values["tok"] = std::string(43, 'a');
    store.values["acct"] = "alice";
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(store, active));
    store.values.clear();
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    store.failKey = "ser";
    TEST_ASSERT_FALSE(writeLink(store, record('b', "bob")));
    TEST_ASSERT_FALSE(readLink(store, active));
    store.failKey.clear();
    TEST_ASSERT_TRUE(writeLink(store, record('b', "bob")));
    TEST_ASSERT_FALSE(setLinkTimeIfMissing(store, 0));
    TEST_ASSERT_TRUE(setLinkTimeIfMissing(store, 1770000123));
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_EQUAL_UINT32(1770000123, active.at);
    TEST_ASSERT_TRUE(setLinkTimeIfMissing(store, 1770000999));
    TEST_ASSERT_EQUAL_UINT32(1770000123, store.getUInt("at"));
}

void test_two_pending_revokes_survive_unlink_and_drain_separately() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    TEST_ASSERT_TRUE(writeLink(store, record('b', "bob")));
    TEST_ASSERT_TRUE(canBeginLink(store));
    TEST_ASSERT_TRUE(prepareUnlink(store));
    TEST_ASSERT_FALSE(canBeginLink(store));
    TEST_ASSERT_TRUE(revokeOnlySession(store));
    TEST_ASSERT_EQUAL_STRING("bob", store.getString("prev_acct").c_str());
    std::string token;
    bool relinked = false, second = false;
    TEST_ASSERT_TRUE(pendingRevoke(store, token, relinked, second));
    TEST_ASSERT_EQUAL_STRING(std::string(43, 'a').c_str(), token.c_str());
    TEST_ASSERT_TRUE(relinked);
    TEST_ASSERT_FALSE(second);
    TEST_ASSERT_TRUE(finishRevoke(store, 200, second));
    TEST_ASSERT_TRUE(pendingRevoke(store, token, relinked, second));
    TEST_ASSERT_EQUAL_STRING(std::string(43, 'b').c_str(), token.c_str());
    TEST_ASSERT_FALSE(relinked);
    TEST_ASSERT_TRUE(second);
    TEST_ASSERT_TRUE(finishRevoke(store, 401, second));
    TEST_ASSERT_FALSE(hasPendingRevoke(store));
    TEST_ASSERT_TRUE(canBeginLink(store));
}

void test_revoke_write_order_and_interrupted_recovery() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    store.events.clear();
    TEST_ASSERT_TRUE(writeLink(store, record('b', "bob")));
    auto index = [&store](const char* name) {
        return std::find(store.events.begin(), store.events.end(), name) - store.events.begin();
    };
    TEST_ASSERT_TRUE(index("remove:ok") < index("put:rev_tok"));
    TEST_ASSERT_TRUE(index("remove:tok") < index("put:rev_tok"));

    MemoryLinkStore interrupted;
    TEST_ASSERT_TRUE(writeLink(interrupted, record('a', "alice")));
    interrupted.failKey = "rev_tok";
    TEST_ASSERT_FALSE(prepareUnlink(interrupted));
    TEST_ASSERT_FALSE(interrupted.getString("rev_hold").empty());
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(interrupted, active));
    interrupted.failKey.clear();
    TEST_ASSERT_TRUE(recoverPendingRevoke(interrupted));
    TEST_ASSERT_TRUE(revokeOnlySession(interrupted));
    TEST_ASSERT_EQUAL_STRING("alice", interrupted.getString("prev_acct").c_str());
}

void test_confirm_retry_budget_and_active_revoke_guard() {
    TEST_ASSERT_TRUE(pairingMayContinue(false, 599999, 600000, 610000, 660000));
    TEST_ASSERT_FALSE(pairingMayContinue(false, 600000, 600000, 610000, 660000));
    TEST_ASSERT_TRUE(pairingMayContinue(true, 650000, 600000, 650000, 660000));
    TEST_ASSERT_FALSE(pairingMayContinue(true, 650000, 600000, 660000, 660000));
    TEST_ASSERT_FALSE(shouldSendRevoke(std::string(43, 'a'), std::string(43, 'a')));
    TEST_ASSERT_TRUE(shouldSendRevoke(std::string(43, 'a'), std::string(43, 'b')));
    TEST_ASSERT_EQUAL_UINT32(17, pairPollWait(5, 429, 17));
    TEST_ASSERT_EQUAL_UINT32(5, pairPollWait(5, 200, 17));
    TEST_ASSERT_EQUAL_UINT32(5, pairPollWait(5, 429, 0));
}

void test_previous_account_prompts_only_after_a_link() {
    MemoryLinkStore store;
    LinkSession never(store.getString("prev_acct"), store.getString("prev_aref"));
    never.reply("confirm_on_device", "alice", "", 0, "0123456789abcdef");
    never.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)never.phase());

    LinkRecord first = record('a', "alice");
    first.accountRef = "0123456789abcdef";
    TEST_ASSERT_TRUE(writeLink(store, first));
    TEST_ASSERT_TRUE(prepareUnlink(store));
    LinkSession same(store.getString("prev_acct"), store.getString("prev_aref"));
    same.reply("confirm_on_device", "renamed", "", 0, "0123456789abcdef");
    same.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::Confirming, (int)same.phase());
    LinkSession different(store.getString("prev_acct"), store.getString("prev_aref"));
    different.reply("confirm_on_device", "alice", "", 0, "fedcba9876543210");
    different.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::ClearApps, (int)different.phase());
}

void test_mismatch_wipe_preserves_last_account_without_revoking() {
    MemoryLinkStore store;
    LinkRecord first = record('a', "alice");
    first.accountRef = "0123456789abcdef";
    TEST_ASSERT_TRUE(writeLink(store, first));
    TEST_ASSERT_TRUE(wipeMismatchedLink(store));
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_FALSE(hasPendingRevoke(store));
    TEST_ASSERT_EQUAL_STRING("alice", store.getString("prev_acct").c_str());
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", store.getString("prev_aref").c_str());
    LinkSession different(store.getString("prev_acct"), store.getString("prev_aref"));
    different.reply("confirm_on_device", "bob", "", 0, "fedcba9876543210");
    different.answer(true);
    TEST_ASSERT_EQUAL_INT((int)PairPhase::ClearApps, (int)different.phase());
    TEST_ASSERT_TRUE(wipeMismatchedLink(store));
    TEST_ASSERT_EQUAL_STRING("alice", store.getString("prev_acct").c_str());
}

void test_test_token_replacement_does_not_queue_revoke() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    TEST_ASSERT_TRUE(replaceTestToken(store, std::string(43, 'b')));
    LinkRecord active;
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_EQUAL_STRING(std::string(43, 'b').c_str(), active.token.c_str());
    TEST_ASSERT_FALSE(hasPendingRevoke(store));
}

struct ClearProbe { MemoryLinkStore* store; bool sawConfirmed = false; };
bool failingClear(void* context) {
    ClearProbe& probe = *static_cast<ClearProbe*>(context);
    LinkRecord active;
    probe.sawConfirmed = readLink(*probe.store, active);
    return false;
}

void test_app_clear_failure_keeps_confirmed_link() {
    MemoryLinkStore store;
    ClearProbe probe{&store};
    TEST_ASSERT_EQUAL_INT((int)LinkCompletion::LinkedClearFailed,
        (int)completeConfirmedLink(store, record('a', "alice"), true, failingClear, &probe));
    TEST_ASSERT_TRUE(probe.sawConfirmed);
    LinkRecord active;
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_EQUAL_STRING("alice", active.account.c_str());
}

void test_begin_needs_only_one_free_revoke_slot() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(canBeginLink(store));
    store.values["rev_tok"] = std::string(43, 'a');
    TEST_ASSERT_TRUE(canBeginLink(store));
    store.values["rev_hold"] = std::string(43, 'a');
    TEST_ASSERT_TRUE(canBeginLink(store));
    store.values["rev_hold"] = std::string(43, 'c');
    TEST_ASSERT_FALSE(canBeginLink(store));
    store.values.erase("rev_hold");
    store.values["rev_tok2"] = std::string(43, 'b');
    TEST_ASSERT_FALSE(canBeginLink(store));
    store.values.erase("rev_tok");
    TEST_ASSERT_TRUE(canBeginLink(store));
}

void test_final_client_errors_drop_revoke_slot() {
    const int finalStatuses[] = {200, 400, 401, 403, 404, 410};
    for (int status : finalStatuses) {
        MemoryLinkStore store;
        store.values["rev_tok"] = std::string(43, 'a');
        store.values["rev_rel"] = "1";
        store.values["rev_fail"] = "3";
        TEST_ASSERT_TRUE(revokeIsFinal(status));
        TEST_ASSERT_TRUE(finishRevoke(store, status, false));
        TEST_ASSERT_FALSE(hasPendingRevoke(store));
        TEST_ASSERT_TRUE(store.getString("rev_fail").empty());
    }
    const int retryStatuses[] = {0, 408, 429, 500, 503};
    for (int status : retryStatuses) {
        MemoryLinkStore store;
        store.values["rev_tok"] = std::string(43, 'a');
        TEST_ASSERT_FALSE(revokeIsFinal(status));
        TEST_ASSERT_FALSE(finishRevoke(store, status, false));
        TEST_ASSERT_TRUE(hasPendingRevoke(store));
    }
}

void test_revoke_slots_survive_repeated_transient_failures() {
    MemoryLinkStore store;
    TEST_ASSERT_TRUE(writeLink(store, record('a', "alice")));
    TEST_ASSERT_TRUE(writeLink(store, record('b', "bob")));
    TEST_ASSERT_TRUE(prepareUnlink(store));
    const int transient[] = {0, 408, 429, 500, 503};
    for (int i = 0; i < 30; ++i) {
        TEST_ASSERT_FALSE(finishRevoke(store, transient[i % 5], false));
        TEST_ASSERT_FALSE(finishRevoke(store, transient[i % 5], true));
        TEST_ASSERT_TRUE(hasPendingRevoke(store));
        TEST_ASSERT_FALSE(canBeginLink(store));
    }
    TEST_ASSERT_TRUE(finishRevoke(store, 200, false));
    TEST_ASSERT_TRUE(store.getString("rev_tok").empty());
    TEST_ASSERT_TRUE(canBeginLink(store));
    std::string token;
    bool relinked = true, second = false;
    TEST_ASSERT_TRUE(pendingRevoke(store, token, relinked, second));
    TEST_ASSERT_EQUAL_STRING(std::string(43, 'b').c_str(), token.c_str());
    TEST_ASSERT_TRUE(second);

    MemoryLinkStore wiped;
    wiped.values["rev_fail"] = "4";
    wiped.values["rev_fail2"] = "5";
    TEST_ASSERT_TRUE(wipeMismatchedLink(wiped));
    TEST_ASSERT_TRUE(wiped.getString("rev_fail").empty());
    TEST_ASSERT_TRUE(wiped.getString("rev_fail2").empty());
}

void test_revoke_retry_after_is_kept_until_final_answer() {
    MemoryLinkStore store;
    store.values["rev_tok"] = std::string(43, 'a');
    TEST_ASSERT_TRUE(store.putUInt("rev_at", 1770000060));
    TEST_ASSERT_TRUE(revokeRetryPending(store, false, 1770000000));
    TEST_ASSERT_FALSE(revokeRetryPending(store, false, 1770000060));
    TEST_ASSERT_FALSE(finishRevoke(store, 429));
    TEST_ASSERT_TRUE(revokeRetryPending(store, false, 1770000000));
    TEST_ASSERT_TRUE(finishRevoke(store, 401));
    TEST_ASSERT_FALSE(revokeRetryPending(store, false, 1770000000));
    TEST_ASSERT_FALSE(hasPendingRevoke(store));
}

bool successfulClear(void* context) {
    ClearProbe& probe = *static_cast<ClearProbe*>(context);
    LinkRecord active;
    probe.sawConfirmed = readLink(*probe.store, active);
    return true;
}

void test_clear_intent_survives_interruption_and_retries_failure() {
    MemoryLinkStore store;
    ClearProbe probe{&store};
    TEST_ASSERT_EQUAL_INT((int)LinkCompletion::LinkedClearFailed,
        (int)completeConfirmedLink(store, record('a', "alice"), true, failingClear, &probe));
    TEST_ASSERT_TRUE(probe.sawConfirmed);
    TEST_ASSERT_FALSE(store.getString("clr").empty());
    LinkRecord active;
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_TRUE(recoverPendingClear(store, successfulClear, &probe));
    TEST_ASSERT_TRUE(probe.sawConfirmed);
    TEST_ASSERT_TRUE(store.getString("clr").empty());
    TEST_ASSERT_TRUE(readLink(store, active));
    TEST_ASSERT_TRUE(recoverPendingClear(store, successfulClear, &probe));

    MemoryLinkStore interrupted;
    ClearProbe interruptedProbe{&interrupted};
    interrupted.values["clr"] = "previous-account";
    TEST_ASSERT_TRUE(writeLink(interrupted, record('b', "bob")));
    TEST_ASSERT_TRUE(recoverPendingClear(interrupted, successfulClear, &interruptedProbe));
    TEST_ASSERT_TRUE(interruptedProbe.sawConfirmed);
    TEST_ASSERT_TRUE(interrupted.getString("clr").empty());

    MemoryLinkStore ordered;
    ClearProbe orderedProbe{&ordered};
    TEST_ASSERT_EQUAL_INT((int)LinkCompletion::Linked,
        (int)completeConfirmedLink(ordered, record('c', "carol"), true,
                                   successfulClear, &orderedProbe));
    auto index = [&ordered](const char* name) {
        return std::find(ordered.events.begin(), ordered.events.end(), name) - ordered.events.begin();
    };
    TEST_ASSERT_TRUE(index("put:clr") < index("put:ok"));
    TEST_ASSERT_TRUE(index("put:ok") < index("remove:clr"));
    TEST_ASSERT_TRUE(orderedProbe.sawConfirmed);
}

void test_server_unlink_preserves_previous_account_and_revoke() {
    MemoryLinkStore store;
    LinkRecord first = record('a', "alice");
    first.accountRef = "0123456789abcdef";
    TEST_ASSERT_TRUE(writeLink(store, first));
    store.values["rev_tok"] = std::string(43, 'z');
    TEST_ASSERT_TRUE(forgetServerUnlinkedLink(store));
    LinkRecord active;
    TEST_ASSERT_FALSE(readLink(store, active));
    TEST_ASSERT_EQUAL_STRING("alice", store.getString("prev_acct").c_str());
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", store.getString("prev_aref").c_str());
    TEST_ASSERT_TRUE(hasPendingRevoke(store));
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4e5f6", store.getString("id").c_str());
    TEST_ASSERT_EQUAL_INT((int)CredentialError::NotLinked,
        (int)credentialError(401, "not_linked"));
    TEST_ASSERT_EQUAL_INT((int)CredentialError::WrongDevice,
        (int)credentialError(401, "wrong_device"));
    TEST_ASSERT_EQUAL_INT((int)CredentialError::Other,
        (int)credentialError(401, "sign_in_required"));
    TEST_ASSERT_EQUAL_INT((int)CredentialError::Other,
        (int)credentialError(500, "not_linked"));
}

void test_confirm_extends_session_limit_once_bounded() {
    TEST_ASSERT_EQUAL_UINT32(770000, confirmSessionLimit(660000, 650000, 120000));
    TEST_ASSERT_EQUAL_UINT32(660000, confirmSessionLimit(660000, 100000, 120000));
    TEST_ASSERT_EQUAL_UINT32(660000, confirmSessionLimit(660000, 540000, 120000));
    const uint32_t extended = confirmSessionLimit(660000, 659000, 120000);
    TEST_ASSERT_EQUAL_UINT32(779000, extended);
    TEST_ASSERT_TRUE(pairingMayContinue(true, 700000, 600000, 700000, extended));
    TEST_ASSERT_FALSE(pairingMayContinue(true, 779000, 600000, 779000, extended));
}

void setUp(void) {}
void tearDown(void) {}

// Advance widths of printable ASCII (0x20..0x7E) in ArialMT_Plain_10, copied
// from the display library's font table (its jump table's width byte).
const uint8_t kArial10Widths[95] = {
    3, 3, 4, 6, 6, 9, 7, 2, 3, 3, 4, 6, 3, 3, 3, 3, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 3, 3, 6, 6, 6, 6, 10, 7, 7, 7, 7, 7, 6, 8, 7, 3, 5, 7, 6, 8, 7, 8,
    7, 8, 7, 7, 6, 7, 7, 9, 7, 7, 6, 3, 3, 3, 5, 6, 3, 6, 6, 5, 6, 6, 3, 6,
    6, 2, 2, 5, 2, 8, 6, 6, 6, 6, 3, 5, 3, 6, 5, 7, 5, 5, 5, 3, 3, 3, 6,
};

int arial10Width(const char* s) {
    int w = 0;
    for (; *s; ++s) {
        TEST_ASSERT_TRUE_MESSAGE(*s >= 0x20 && *s <= 0x7E, "link screen text must be printable ASCII");
        w += kArial10Widths[*s - 0x20];
    }
    return w;
}

void test_link_error_text_names_the_fix_and_fits_the_screen() {
    LinkErrorText t = linkErrorText("no-wifi");
    TEST_ASSERT_EQUAL_STRING("Needs WiFi first", t.line1);
    TEST_ASSERT_EQUAL_STRING("Settings > Setup WiFi", t.line2);
    t = linkErrorText("join");
    TEST_ASSERT_EQUAL_STRING("Can't reach WiFi", t.line1);
    TEST_ASSERT_EQUAL_STRING("Check it's in range", t.line2);
    // Every other code the link session can publish, plus none at all.
    const char* const others[] = {
        "link-mismatch", "storage", "tls-allocator", "invalid-base", "radio-busy",
        "sta-mode", "try-again", "begin", "begin-reply", "request", "reply",
        "cancelled", "task-create", "", nullptr,
    };
    for (const char* code : others) {
        t = linkErrorText(code);
        TEST_ASSERT_EQUAL_STRING("Could not link", t.line1);
        TEST_ASSERT_EQUAL_STRING("Try again later", t.line2);
    }
    const char* const codes[] = {"no-wifi", "join", "begin"};
    for (const char* code : codes) {
        t = linkErrorText(code);
        TEST_ASSERT_LESS_OR_EQUAL_INT(128, arial10Width(t.line1));
        TEST_ASSERT_LESS_OR_EQUAL_INT(128, arial10Width(t.line2));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_link_error_text_names_the_fix_and_fits_the_screen);
    RUN_TEST(test_wait_confirm_store_and_lost_reply);
    RUN_TEST(test_decline_and_expiry);
    RUN_TEST(test_change_of_hands_decision);
    RUN_TEST(test_account_reference_decides_change_of_hands);
    RUN_TEST(test_account_reference_and_server_time_are_stored);
    RUN_TEST(test_account_reference_write_failure_hides_partial_link);
    RUN_TEST(test_relink_revoke_retry_and_401);
    RUN_TEST(test_unlink_network_failure_is_local_first);
    RUN_TEST(test_unlink_intent_hides_link_before_revoke_is_saved);
    RUN_TEST(test_reader_rejects_torn_link_and_time_uses_server);
    RUN_TEST(test_two_pending_revokes_survive_unlink_and_drain_separately);
    RUN_TEST(test_revoke_write_order_and_interrupted_recovery);
    RUN_TEST(test_confirm_retry_budget_and_active_revoke_guard);
    RUN_TEST(test_previous_account_prompts_only_after_a_link);
    RUN_TEST(test_mismatch_wipe_preserves_last_account_without_revoking);
    RUN_TEST(test_test_token_replacement_does_not_queue_revoke);
    RUN_TEST(test_app_clear_failure_keeps_confirmed_link);
    RUN_TEST(test_begin_needs_only_one_free_revoke_slot);
    RUN_TEST(test_final_client_errors_drop_revoke_slot);
    RUN_TEST(test_revoke_slots_survive_repeated_transient_failures);
    RUN_TEST(test_revoke_retry_after_is_kept_until_final_answer);
    RUN_TEST(test_clear_intent_survives_interruption_and_retries_failure);
    RUN_TEST(test_server_unlink_preserves_previous_account_and_revoke);
    RUN_TEST(test_confirm_extends_session_limit_once_bounded);
    return UNITY_END();
}
