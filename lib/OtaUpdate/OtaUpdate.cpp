// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "OtaUpdate.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>

#include "OtaManifest.h"
#include "CheckinPolicy.h"
#include "PromptPolicy.h"

namespace OtaUpdate {

namespace {

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isSha256(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; ++i) if (!isHex(s[i])) return false;
    return true;
}

bool validKeyId(const char* s) {
    for (; *s; ++s)
        if (!( (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
    return true;
}

bool validSignature(const char* s) {
    const size_t n = strlen(s);
    if (n < 8 || n % 4 != 0) return false;
    size_t pad = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c == '=') { ++pad; if (i < n - 2 || pad > 2) return false; }
        else if (pad || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
    }
    const size_t bytes = n / 4 * 3 - pad;
    if (bytes < 8 || bytes > 78) return false;
    uint8_t der[78];
    size_t used = 0;
    unsigned acc = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < n - pad; ++i) {
        const char c = s[i];
        const unsigned value = c >= 'A' && c <= 'Z' ? c - 'A' :
            c >= 'a' && c <= 'z' ? c - 'a' + 26 :
            c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : 63;
        acc = (acc << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der[used++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    if (used != bytes || acc != 0 || der[0] != 0x30 || der[1] != bytes - 2 || der[2] != 0x02)
        return false;
    const size_t rLen = der[3];
    const size_t sTag = 4 + rLen;
    if (rLen < 1 || rLen > 33 || sTag + 2 > bytes || der[sTag] != 0x02) return false;
    const size_t sLen = der[sTag + 1];
    if (sLen < 1 || sLen > 33 || sTag + 2 + sLen != bytes) return false;
    const auto canonical = [](const uint8_t* p, size_t len) {
        return !(p[0] & 0x80) && (len == 1 || p[0] != 0 || (p[1] & 0x80));
    };
    return canonical(der + 4, rLen) && canonical(der + sTag + 2, sLen);
}

// Copies a JSON string member exactly; false when missing, not a string,
// empty, or too long for `cap` (including the terminator).
bool copyString(const cJSON* obj, const char* key, char* out, size_t cap) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t n = strlen(item->valuestring);
    if (n == 0 || n >= cap) return false;
    memcpy(out, item->valuestring, n + 1);
    return true;
}

// A whole, positive JSON number no larger than `max`.
bool wholeNumber(const cJSON* obj, const char* key, double max, double& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    const double v = item->valuedouble;
    if (!(v >= 1.0) || v > max || v != (double)(uint64_t)v) return false;
    out = v;
    return true;
}

bool validUrl(const char* url) {
    // A path on the same site: never another host ("//", a scheme) and
    // nothing that could smuggle one in.
    if (url[0] != '/' || url[1] == '/') return false;
    for (const char* p = url; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '#' || c == '@') return false;
    }
    return true;
}

bool repoChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_';
}

bool validSource(const char* s) {
    if (strcmp(s, "official") == 0) return true;
    if (strncmp(s, "fork:", 5) != 0) return false;
    const char* owner = s + 5;
    const char* slash = strchr(owner, '/');
    if (!slash) return false;
    const size_t ownerLen = (size_t)(slash - owner);
    const char* repo = slash + 1;
    const size_t repoLen = strlen(repo);
    if (ownerLen < 1 || ownerLen > 39 || repoLen < 1 || repoLen > 100) return false;
    for (const char* p = owner; p < slash; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    for (const char* p = repo; *p; ++p) if (!repoChar(*p)) return false;
    return strstr(repo, "..") == nullptr;
}

int digits(const char* p, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool sameText(const char* a, const char* b) { return strcmp(a, b) == 0; }

const char* parseFields(const cJSON* root, Manifest& out) {
    PromptPolicy::Version parsed;
    if (!copyString(root, "version", out.version, sizeof(out.version)) ||
        !PromptPolicy::parseVersion(out.version, parsed)) {
        return "version";
    }
    double num = 0;
    if (!wholeNumber(root, "size", (double)kSlotSize, num)) return "size";
    out.size = (uint32_t)num;
    if (!copyString(root, "sha256", out.sha256, sizeof(out.sha256)) || !isSha256(out.sha256))
        return "sha256";
    const cJSON* sig = cJSON_GetObjectItemCaseSensitive(root, "sig");
    const cJSON* keyId = cJSON_GetObjectItemCaseSensitive(root, "key_id");
    if ((sig == nullptr) != (keyId == nullptr)) return sig ? "key_id" : "sig";
    if (sig) {
        if (!copyString(root, "sig", out.sig, sizeof(out.sig)) || !validSignature(out.sig)) return "sig";
        if (!copyString(root, "key_id", out.keyId, sizeof(out.keyId)) || !validKeyId(out.keyId)) return "key_id";
    }
    if (!copyString(root, "url", out.url, sizeof(out.url)) || !validUrl(out.url)) return "url";
    const cJSON* hw = cJSON_GetObjectItemCaseSensitive(root, "hw");
    if (!cJSON_IsObject(hw) || !copyString(hw, "min_rev", out.minRev, sizeof(out.minRev)) ||
        !copyString(hw, "max_rev", out.maxRev, sizeof(out.maxRev))) {
        return "hw";
    }
    if (!copyString(root, "channel", out.channel, sizeof(out.channel)) ||
        !(sameText(out.channel, "stable") || sameText(out.channel, "rc"))) {
        return "channel";
    }
    if (!copyString(root, "source", out.source, sizeof(out.source)) || !validSource(out.source))
        return "source";
    if (!wholeNumber(root, "release_id", 9007199254740991.0, num)) return "release_id";
    out.releaseId = (uint64_t)num;
    char releasedAt[24] = {0};
    if (!copyString(root, "released_at", releasedAt, sizeof(releasedAt)) ||
        !parseUtc(releasedAt, out.releasedAt)) {
        return "released_at";
    }
    return nullptr;
}

}  // namespace

bool parseUtc(const char* t, uint32_t& out) {
    if (!t || strlen(t) != 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' ||
        t[13] != ':' || t[16] != ':' || t[19] != 'Z') {
        return false;
    }
    const int y = digits(t, 4), mo = digits(t + 5, 2), d = digits(t + 8, 2);
    const int h = digits(t + 11, 2), mi = digits(t + 14, 2), s = digits(t + 17, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        s < 0 || s > 59) {
        return false;
    }
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int maxDay = kDays[mo - 1] + ((mo == 2 && leap) ? 1 : 0);
    if (d > maxDay) return false;
    const int64_t secs = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    if (secs < 0 || secs > 0xFFFFFFFFLL) return false;
    out = (uint32_t)secs;
    return true;
}

const char* parseManifest(const char* json, size_t len, Manifest& out) {
    out = Manifest();
    if (!json || len == 0) return "json";
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return "json";
    }
    const char* err = parseFields(root, out);
    cJSON_Delete(root);
    if (err) out = Manifest();
    return err;
}

const char* normalizeSource(const char* stored) {
    if (!stored || !stored[0] || sameText(stored, "official") || sameText(stored, "cyberfidget.com"))
        return "official";
    return stored;
}

const char* normalizeChannel(const char* stored) {
    return (stored && sameText(stored, "rc")) ? "rc" : "stable";
}

void seenKey(const char* source, const char* channel, char out[kSeenKeyLen + 1]) {
    uint32_t h = 2166136261u;
    auto mix = [&h](const char* s) {
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    };
    mix(normalizeSource(source));
    mix("|");
    mix(normalizeChannel(channel));
    snprintf(out, kSeenKeyLen + 1, "seen_%08x", (unsigned)h);
}

Verdict gate(const Manifest& m, const Context& ctx) {
    const BoardInfo::Info board = ctx.board ? *ctx.board : BoardInfo::defaults();
    switch (OtaManifest::hwCompatible(board, m.minRev, m.maxRev)) {
        case OtaManifest::HwResult::Malformed: return Verdict::Malformed;
        case OtaManifest::HwResult::Incompatible: return Verdict::HwIncompatible;
        case OtaManifest::HwResult::Compatible: break;
    }
    const char* source = normalizeSource(ctx.source);
    if (!sameText(m.source, source)) return Verdict::WrongSource;
    if (!sameText(source, "official") && !ctx.sourceAcknowledged) return Verdict::SourceNotAcknowledged;
    // A stable device takes stable releases only; an rc device takes both
    // (the site reports each release's own channel).
    const char* channel = normalizeChannel(ctx.channel);
    if (sameText(channel, "stable") && !sameText(m.channel, "stable")) return Verdict::WrongChannel;
    if (m.releasedAt < ctx.seenTs) return Verdict::Stale;
    if (m.size == 0 || m.size > kSlotSize) return Verdict::TooLarge;
    if (ctx.wanted && !sameText(m.version, ctx.wanted)) return Verdict::NotWanted;
    return Verdict::Ok;
}

const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::Malformed: return "hw-malformed";
        case Verdict::HwIncompatible: return "hw";
        case Verdict::WrongSource: return "source";
        case Verdict::SourceNotAcknowledged: return "source-ack";
        case Verdict::WrongChannel: return "channel";
        case Verdict::Stale: return "stale";
        case Verdict::TooLarge: return "size";
        case Verdict::NotWanted: return "changed";
    }
    return "?";
}

const char* verdictCopy(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "";
        case Verdict::Malformed:
        case Verdict::HwIncompatible: return OtaManifest::kHardwareRefusal;
        case Verdict::WrongSource:
        case Verdict::SourceNotAcknowledged: return "This update comes from a source this Fidget does not use.";
        case Verdict::WrongChannel: return "This update is not on this Fidget's update channel.";
        case Verdict::Stale: return "This update is older than one already installed.";
        case Verdict::TooLarge: return "This update is too big for this Fidget.";
        case Verdict::NotWanted: return "The update changed. Check for updates again.";
    }
    return "";
}

bool fallbackAllowed(FetchOutcome outcome) {
    return outcome == FetchOutcome::Transport || outcome == FetchOutcome::ServerError;
}

bool installPermitted(bool hasSignature, bool knownKey, bool allowUnsigned) {
    return hasSignature ? knownKey : allowUnsigned;
}

OfferAction offerAction(FetchOutcome outcome, const char* badField) {
    if (outcome == FetchOutcome::Ok) return OfferAction::Store;
    if (outcome == FetchOutcome::GateRefused) return OfferAction::Withdraw;
    if (outcome == FetchOutcome::BadManifest && badField &&
        (strcmp(badField, "sig") == 0 || strcmp(badField, "key_id") == 0)) {
        return OfferAction::Withdraw;
    }
    return OfferAction::Unchanged;
}

bool refusalMarksFailed(bool hasSignature, bool knownKey) { return hasSignature && knownKey; }

const char* armRefusal(bool hasUpdateSlot, bool installAllowed, int32_t vbatMv, int32_t socPct) {
    if (!hasUpdateSlot) return "no-update-slot";
    if (!installAllowed) return "unsigned";
    if (!CheckinPolicy::batteryEligible(vbatMv, socPct)) return "battery";
    return nullptr;
}

const char* armRefusalCopy(const char* reason) {
    return reason && strcmp(reason, "battery") == 0 ? "Charge your Fidget first." : nullptr;
}

bool formatPending(const Pending& p, char* out, size_t len) {
    if (!isSha256(p.sha256) || p.size == 0 || p.size > kSlotSize ||
        strlen(p.seenKey) != kSeenKeyLen || !p.version[0] || strchr(p.version, ' ')) {
        return false;
    }
    const int n = snprintf(out, len, "%s %lu %lu %s %s", p.sha256, (unsigned long)p.size,
                           (unsigned long)p.releasedAt, p.seenKey, p.version);
    return n > 0 && (size_t)n < len;
}

namespace {
// Reads one space-terminated decimal field (no sign, no leading "+").
bool readNumber(const char*& p, uint32_t& out) {
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
        ++p;
    }
    if (*p != ' ') return false;
    ++p;
    out = (uint32_t)v;
    return true;
}
}  // namespace

bool parsePending(const char* text, Pending& out) {
    out = Pending();
    if (!text || strlen(text) < 64 || text[64] != ' ') return false;
    memcpy(out.sha256, text, 64);
    out.sha256[64] = '\0';
    if (!isSha256(out.sha256)) { out = Pending(); return false; }
    const char* p = text + 65;
    if (!readNumber(p, out.size) || out.size == 0 || out.size > kSlotSize ||
        !readNumber(p, out.releasedAt)) {
        out = Pending();
        return false;
    }
    if (strncmp(p, "seen_", 5) != 0 || strlen(p) < kSeenKeyLen + 2 || p[kSeenKeyLen] != ' ') {
        out = Pending();
        return false;
    }
    for (size_t i = 5; i < kSeenKeyLen; ++i) {
        if (!isHex(p[i])) { out = Pending(); return false; }
    }
    memcpy(out.seenKey, p, kSeenKeyLen);
    out.seenKey[kSeenKeyLen] = '\0';
    p += kSeenKeyLen + 1;
    const size_t vlen = strlen(p);
    if (vlen == 0 || vlen > kMaxVersionLen || strchr(p, ' ')) { out = Pending(); return false; }
    memcpy(out.version, p, vlen + 1);
    return true;
}

const char* installResultName(InstallResult r) {
    switch (r) {
        case InstallResult::Ready: return "ready";
        case InstallResult::BeginFailed: return "begin";
        case InstallResult::WriteFailed: return "write";
        case InstallResult::TooLong: return "too-long";
        case InstallResult::Short: return "short";
        case InstallResult::HashMismatch: return "hash";
        case InstallResult::VerificationFailed: return "verify";
        case InstallResult::ImageInvalid: return "image";
        case InstallResult::StoreFailed: return "store";
        case InstallResult::BootFailed: return "boot";
        case InstallResult::Aborted: return "aborted";
    }
    return "?";
}

bool Installer::begin(const Manifest& m, const char* seenKeyText) {
    if (begun_ || done_) return false;
    manifest_ = m;
    pending_ = Pending();
    memcpy(pending_.sha256, m.sha256, sizeof(pending_.sha256));
    pending_.size = m.size;
    pending_.releasedAt = m.releasedAt;
    if (!seenKeyText || strlen(seenKeyText) != kSeenKeyLen) {
        done_ = true;
        result_ = InstallResult::BeginFailed;
        return false;
    }
    memcpy(pending_.seenKey, seenKeyText, kSeenKeyLen + 1);
    memcpy(pending_.version, m.version, sizeof(pending_.version));
    received_ = 0;
    hasher_.start();
    if (m.size == 0 || m.size > kSlotSize || !target_.begin(m.size)) {
        done_ = true;
        result_ = InstallResult::BeginFailed;
        return false;
    }
    begun_ = true;
    return true;
}

void Installer::fail(InstallResult r) {
    if (begun_ && !done_) target_.abort();
    done_ = true;
    result_ = r;
}

bool Installer::feed(const uint8_t* data, size_t len) {
    if (!begun_ || done_) return false;
    if (len > pending_.size - received_) {
        fail(InstallResult::TooLong);
        return false;
    }
    hasher_.update(data, len);
    if (!target_.write(data, len)) {
        fail(InstallResult::WriteFailed);
        return false;
    }
    received_ += (uint32_t)len;
    return true;
}

InstallResult Installer::complete() {
    if (done_) return result_;
    if (!begun_) {
        done_ = true;
        result_ = InstallResult::Aborted;
        return result_;
    }
    if (received_ != pending_.size) {
        fail(InstallResult::Short);
        return result_;
    }
    uint8_t digest[32];
    hasher_.finish(digest);
    char hex[65];
    static const char kDigits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex[i * 2] = kDigits[digest[i] >> 4];
        hex[i * 2 + 1] = kDigits[digest[i] & 15];
    }
    hex[64] = '\0';
    // Compared before the image is finished: unverified bytes never reach
    // the point where the boot slot could be selected.
    if (strcmp(hex, pending_.sha256) != 0) {
        fail(InstallResult::HashMismatch);
        return result_;
    }
    const VerifyResult verification = verifier_ ? verifier_->verify(manifest_, digest) :
        (manifest_.sig[0] ? VerifyResult::Bad : VerifyResult::Unsigned);
    if (verification != VerifyResult::Ok &&
        !(verification == VerifyResult::Unsigned && allowUnsigned_)) {
        fail(InstallResult::VerificationFailed);
        return result_;
    }
    // From here on the image handle is closed by end() whatever it returns.
    done_ = true;
    if (!target_.end()) { result_ = InstallResult::ImageInvalid; return result_; }
    char record[160];
    // The expected identity is stored before the boot slot changes, so the
    // new image can check itself and the old one can tell what happened.
    if (!formatPending(pending_, record, sizeof(record)) || !target_.persistPending(record)) {
        result_ = InstallResult::StoreFailed;
        return result_;
    }
    if (!target_.selectBoot()) { result_ = InstallResult::BootFailed; return result_; }
    result_ = InstallResult::Ready;
    return result_;
}

void Installer::abort() {
    if (!done_) fail(InstallResult::Aborted);
}

uint8_t Installer::percent() const {
    if (pending_.size == 0) return 0;
    return (uint8_t)(((uint64_t)received_ * 100u) / pending_.size);
}

bool versionMatches(const char* expected, const char* running) {
    if (!expected || !running || !expected[0] || !running[0]) return false;
    if (strcmp(expected, running) == 0) return true;
    if (strchr(expected, '+')) return false;
    const size_t n = strlen(expected);
    return strncmp(expected, running, n) == 0 && running[n] == '+';
}

SelfTestResult decideSelfTest(const SelfTestInputs& in) {
    SelfTestResult r;
    if (!in.recordValid) r.reason = "record";
    else if (!in.halOk) r.reason = "hal";
    else if (!in.fsMounted) r.reason = "filesystem";
    else if (!in.versionOk) r.reason = "version";
    else if (!in.imageOk) r.reason = "image";
    else if (!in.inTime) r.reason = "deadline";
    else { r.markValid = true; r.reason = "ok"; }
    return r;
}

ConfirmStep confirmStep(bool checksPassed, bool frameDrawn, uint32_t elapsedMs, uint32_t budgetMs) {
    if (!checksPassed || elapsedMs > budgetMs) return ConfirmStep::RollBack;
    return frameDrawn ? ConfirmStep::Confirm : ConfirmStep::Wait;
}

SleepStep sleepStep(bool imagePending, bool checksPassed, bool criticalVoltage) {
    if (!imagePending) return SleepStep::Proceed;
    if (!criticalVoltage) return SleepStep::Defer;
    return checksPassed ? SleepStep::KeepFirst : SleepStep::AbortWithoutFailure;
}

bool automaticOfferAllowed(const char* avail, const char* failedVersion) {
    if (!avail || !avail[0]) return false;
    return !failedVersion || strcmp(avail, failedVersion) != 0;
}

BootNotice bootNotice(bool recordPresent, bool recordValid, bool runningIsRecord,
                      bool criticalPowerAbort) {
    if (criticalPowerAbort) return BootNotice::None;
    if (!recordPresent) return BootNotice::None;
    if (recordValid && runningIsRecord) return BootNotice::Completed;
    return BootNotice::DidNotFinish;
}

}  // namespace OtaUpdate
