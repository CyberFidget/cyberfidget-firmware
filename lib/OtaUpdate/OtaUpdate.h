// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * OtaUpdate - the pure rules of installing an update on the Fidget.
 *
 * No Arduino, no flash, no network: the device glue (lib/CloudSync
 * UpdateSession) reads storage and the running image, then asks these
 * functions what to do, so the native test_ota_manifest suite drives every
 * branch. Described in lib/OtaUpdate/README.md.
 *
 * - parseManifest(): the site's update manifest (update/firmware.php
 *   ?manifest=1), every field validated, nothing truncated.
 * - gate(): hardware range, source, source acknowledgment, channel,
 *   freshness per (source, channel), slot size, and the version the person
 *   chose. Run before any download.
 * - Installer: streams bytes into a Target while hashing them; the digest is
 *   compared with the manifest BEFORE the image is finished, the expected
 *   identity is stored BEFORE the boot slot is selected, and a mismatch
 *   aborts so the boot slot is never selected for unverified bytes.
 * - Pending record (NVS `upd.pend_img`) format, the pending-image self-test
 *   decision, and what a normal boot does with a leftover record.
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#include "BoardInfo.h"

namespace OtaUpdate {

/// default_8MB.csv app slot (0x330000); an image never exceeds it.
constexpr uint32_t kSlotSize = 3342336;
constexpr size_t kMaxVersionLen = 31;
constexpr size_t kMaxUrlLen = 300;
constexpr size_t kMaxSourceLen = 150;   // "fork:" + owner (39) + "/" + repo (100)
constexpr size_t kMaxKeyIdLen = 31;
constexpr size_t kMaxSignatureLen = 104; // base64 of at most 78 DER bytes
constexpr size_t kSeenKeyLen = 13;      // "seen_" + 8 hex digits

// NVS keys (at most 15 characters). Namespace `upd`:
constexpr const char* kKeyPendImg  = "pend_img";   ///< the expected new image
constexpr const char* kKeyUnsigOk  = "unsig_ok";   ///< installing allowed (USB serial)
constexpr const char* kKeyFailVer  = "fail_ver";   ///< a version whose self-test failed
constexpr const char* kKeyPowerAbort = "pwr_abort"; ///< critical battery shutdown before checks pass
constexpr const char* kKeyAvail    = "avail";      ///< the version the prompt may offer
// Namespace `bootcfg` (one-shots):
constexpr const char* kBootSession = "bootupd";    ///< run the update session
constexpr const char* kBootVersion = "updver";     ///< ...for this version
constexpr const char* kBootFailed  = "updfail";    ///< a session did not hand over
// Namespace `cftest` (test builds only):
constexpr const char* kTestFault   = "otafault";
constexpr size_t kMaxNvsKeyLen = 15;

// Time limits. A pending image runs setup under a task-watchdog period of
// kPendingWdtMs (the default 5 s is restored once it is confirmed), so
// hardware start-up slower than kHalBudgetMs is caught by the self-test
// before the watchdog would fire; the checks must end by
// kSelfTestBudgetMs and the first frame by kConfirmBudgetMs (both counted
// from the first line of setup). The update session's watchdog period is
// longer than any single blocking call it makes.
constexpr uint32_t kPendingWdtMs     = 15000;
constexpr uint32_t kHalBudgetMs      = 8000;
constexpr uint32_t kSelfTestBudgetMs = 20000;
constexpr uint32_t kConfirmBudgetMs  = 45000;
constexpr uint32_t kDefaultWdtMs     = 5000;   // the SDK's own period
constexpr uint32_t kSessionWdtMs     = 30000;
constexpr uint32_t kSessionCallMs    = 10000;
constexpr uint32_t kSessionBudgetMs  = 300000;

// ---- manifest ---------------------------------------------------------------

struct Manifest {
    char version[kMaxVersionLen + 1] = {0};
    uint32_t size = 0;
    char sha256[65] = {0};              ///< 64 lowercase hex digits
    char sig[kMaxSignatureLen + 1] = {0}; ///< optional base64 DER ECDSA signature
    char keyId[kMaxKeyIdLen + 1] = {0};  ///< optional signing key identifier
    char url[kMaxUrlLen + 1] = {0};     ///< a path on the same site ("/...")
    char minRev[16] = {0};
    char maxRev[16] = {0};
    char channel[8] = {0};              ///< "stable" | "rc"
    char source[kMaxSourceLen + 1] = {0};   ///< "official" | "fork:<owner>/<repo>"
    uint64_t releaseId = 0;
    uint32_t releasedAt = 0;            ///< epoch seconds (UTC)
};

/// Parses and validates the whole manifest. Returns nullptr on success, else
/// a short reason ("json", "version", "size", "sha256", "url", "hw",
/// "channel", "source", "release_id", "released_at", "sig", "key_id"). Text that does not fit
/// is refused, never truncated.
const char* parseManifest(const char* json, size_t len, Manifest& out);

/// "YYYY-MM-DDTHH:MM:SSZ" -> epoch seconds. False for anything else.
bool parseUtc(const char* text, uint32_t& out);

// ---- gates ------------------------------------------------------------------

/// The device's stored source setting as the manifest names it: empty,
/// "official" and "cyberfidget.com" are the official source.
const char* normalizeSource(const char* stored);

/// The device's stored channel: anything but "rc" is "stable".
const char* normalizeChannel(const char* stored);
/// Clamp a persisted freshness floor to at most two days beyond trusted time.
/// An unset clock cannot authorize a new floor.
uint32_t boundedSeenFloor(uint32_t stored, uint32_t offered, uint32_t now);

/// NVS key holding the newest `released_at` installed for one (source,
/// channel): "seen_" + 8 hex digits of an FNV-1a hash. Always 13 characters.
void seenKey(const char* source, const char* channel, char out[kSeenKeyLen + 1]);

struct Context {
    const BoardInfo::Info* board = nullptr;
    const char* source = "";            ///< stored `upd.src` (normalized here)
    const char* channel = "";           ///< stored `upd.chan` (normalized here)
    bool sourceAcknowledged = false;    ///< a non-official source was accepted once
    uint32_t seenTs = 0;                ///< stored freshness for (source, channel)
    const char* wanted = nullptr;       ///< version chosen; nullptr = any (an offer)
};

enum class Verdict : uint8_t {
    Ok,
    Malformed,              ///< hw bounds unusable
    HwIncompatible,
    WrongSource,            ///< the manifest names another source
    SourceNotAcknowledged,  ///< non-official source not accepted on the device
    WrongChannel,
    Stale,                  ///< released before the newest already installed
    TooLarge,               ///< cannot fit the app slot
    NotWanted,              ///< not the version the person chose
};

Verdict gate(const Manifest& m, const Context& ctx);
const char* verdictName(Verdict v);
/// Plain copy for the screen (no jargon).
const char* verdictCopy(Verdict v);

// ---- source fallback --------------------------------------------------------

enum class FetchOutcome : uint8_t {
    Ok,
    Transport,      ///< no connection, timeout, cut short
    ServerError,    ///< 5xx
    Refused,        ///< 4xx (including 409 "the update changed")
    BadManifest,    ///< answered, but the manifest did not parse
    GateRefused,    ///< parsed, but a gate said no
};

/// Whether the next update host may be tried after this outcome. Only a
/// host that could not answer falls through; an answer that refuses (4xx, a
/// bad manifest, a gate) never does, so a fallback cannot get around a
/// refusal.
bool fallbackAllowed(FetchOutcome outcome);

/// A signed offer is installable only when its id is compiled in. Verification
/// still happens after download; unsigned offers require the USB opt-in.
bool installPermitted(bool hasSignature, bool knownKey, bool allowUnsigned);

/// What a check-in does with the stored offer after asking the update site.
enum class OfferAction : uint8_t {
    Store,      ///< keep this release on offer (version + key id)
    Withdraw,   ///< remove the offer
    Unchanged,  ///< the site did not answer usefully: leave what is stored
};

/// `badField` is the manifest field that failed to parse (BadManifest only).
/// A release signed with a key id this firmware does not know is still
/// stored: installPermitted() then says no, so the owner is told to update
/// from the website instead of never hearing about the release. Only a
/// malformed signature or key id, or a gate refusal, withdraws the offer.
OfferAction offerAction(FetchOutcome outcome, const char* badField);

/// Whether a refused install marks the version as failed (never offered
/// again automatically). Only a signature checked against a known key that
/// did not match counts; an unknown key or a missing signature does not.
bool refusalMarksFailed(bool hasSignature, bool knownKey);

/// Why Install now refuses to arm, as a short log reason, or nullptr when it
/// may arm. The battery floor is the automatic check-in's
/// (CheckinPolicy::batteryEligible): a download and flash write must not
/// run on a nearly flat battery. An unreadable value (-1) refuses.
const char* armRefusal(bool hasUpdateSlot, bool installAllowed, int32_t vbatMv, int32_t socPct);

/// On-screen copy for an armRefusal reason, or nullptr for the generic one.
const char* armRefusalCopy(const char* reason);

// ---- install ----------------------------------------------------------------

/// Where the image goes. On the device: esp_ota_begin/write/end/abort, NVS,
/// esp_ota_set_boot_partition.
class Target {
public:
    virtual ~Target() {}
    virtual bool begin(uint32_t size) = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual bool end() = 0;          ///< finish the image (validates its structure)
    virtual void abort() = 0;        ///< drop a begun image; the boot slot is untouched
    virtual bool persistPending(const char* record) = 0;
    virtual bool selectBoot() = 0;
};

class Hasher {
public:
    virtual ~Hasher() {}
    virtual void start() = 0;
    virtual void update(const uint8_t* data, size_t len) = 0;
    virtual void finish(uint8_t out[32]) = 0;
};

enum class VerifyResult : uint8_t { Ok, Unsigned, Bad, UnknownKey };
class Verifier {
public:
    virtual ~Verifier() {}
    virtual VerifyResult verify(const Manifest& m, const uint8_t digest[32]) = 0;
};

struct Pending {
    char sha256[65] = {0};
    uint32_t size = 0;
    uint32_t releasedAt = 0;
    char seenKey[kSeenKeyLen + 1] = {0};
    char version[kMaxVersionLen + 1] = {0};
};

/// "<sha256> <size> <released_at> <seen key> <version>".
bool formatPending(const Pending& p, char* out, size_t len);
bool parsePending(const char* text, Pending& out);

enum class InstallResult : uint8_t {
    Ready,          ///< boot slot selected; restart to run the new image
    BeginFailed,
    WriteFailed,
    TooLong,        ///< more bytes than the manifest's size
    Short,          ///< fewer bytes than the manifest's size
    HashMismatch,
    VerificationFailed,
    ImageInvalid,   ///< the finished image did not validate
    StoreFailed,    ///< the pending record could not be stored
    BootFailed,     ///< the boot slot could not be selected
    Aborted,        ///< stopped before the end (network, deadline)
};
const char* installResultName(InstallResult r);

class Installer {
public:
    Installer(Target& target, Hasher& hasher, Verifier* verifier = nullptr, bool allowUnsigned = true)
        : target_(target), hasher_(hasher), verifier_(verifier), allowUnsigned_(allowUnsigned) {}
    /// Starts an image of m.size bytes. False: nothing was begun.
    bool begin(const Manifest& m, const char* seenKeyText);
    /// Streams one piece. False: the install is over (see result()).
    bool feed(const uint8_t* data, size_t len);
    /// All bytes received: compare, finish, store the record, select boot.
    InstallResult complete();
    /// Stop early; a begun image is dropped.
    void abort();
    InstallResult result() const { return result_; }
    uint32_t received() const { return received_; }
    /// 0..100 of the manifest's size.
    uint8_t percent() const;

private:
    Target& target_;
    Hasher& hasher_;
    Verifier* verifier_;
    bool allowUnsigned_;
    Manifest manifest_;
    Pending pending_;
    uint32_t received_ = 0;
    bool begun_ = false;
    bool done_ = false;
    InstallResult result_ = InstallResult::Aborted;
    void fail(InstallResult r);
};

// ---- after the restart ------------------------------------------------------

/// The running version against the version the manifest promised. Equal
/// text always matches. A manifest version without build metadata ("1.4.0")
/// also matches a running version that adds it ("1.4.0+abc1234"): releases
/// carry the commit after "+", the release tag does not.
bool versionMatches(const char* expected, const char* running);

struct SelfTestInputs {
    bool recordValid = false;       ///< `upd.pend_img` present and parsed
    bool halOk = false;             ///< hardware start-up finished in time
    bool fsMounted = false;         ///< filesystem mounted WITHOUT formatting
    bool versionOk = false;         ///< versionMatches(record, running)
    bool imageOk = false;           ///< running image hashes to the record
    bool inTime = false;            ///< the self-test's own deadline held
};

struct SelfTestResult {
    bool markValid = false;         ///< false: roll back to the previous image
    const char* reason = "";        ///< first failed check, "ok" when valid
};

/// Pending image only. Every check must pass to keep the new image.
SelfTestResult decideSelfTest(const SelfTestInputs& in);

/// After the checks pass, the new image is kept only once it has drawn its
/// first frame (a full main-loop pass), still under the watchdog and the
/// wall-clock budget. `elapsedMs` counts from the start of the self-test.
enum class ConfirmStep : uint8_t { Wait, Confirm, RollBack };
enum class SleepStep : uint8_t { Proceed, Defer, KeepFirst, AbortWithoutFailure };
/// Any reset while PENDING_VERIFY returns to the old image. Ordinary sleep
/// waits for the first frame or confirmation deadline; critical voltage may
/// sleep immediately, keeping passed checks first or suppressing fail_ver.
SleepStep sleepStep(bool imagePending, bool checksPassed, bool criticalVoltage);
ConfirmStep confirmStep(bool checksPassed, bool frameDrawn, uint32_t elapsedMs, uint32_t budgetMs);

/// Automatic offers (the post-boot popup) skip a version whose self-test
/// already failed on this Fidget; a manual check still shows it.
bool automaticOfferAllowed(const char* avail, const char* failedVersion);

enum class BootNotice : uint8_t {
    None,           ///< no record: nothing to report
    Completed,      ///< the record is this image: finish what the self-test began
    DidNotFinish,   ///< the record is another image: the update did not finish
};

/// A boot that is NOT pending verification, with `recordPresent` =
/// `upd.pend_img` exists. `runningIsRecord`: the running image hashes to it.
BootNotice bootNotice(bool recordPresent, bool recordValid, bool runningIsRecord,
                      bool criticalPowerAbort = false);

}  // namespace OtaUpdate

#endif  // OTA_UPDATE_H
