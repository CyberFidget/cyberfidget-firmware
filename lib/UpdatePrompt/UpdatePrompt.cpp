// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "UpdatePrompt.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "AppDefs.h"
#include "AppManager.h"
#include "AwakeMode.h"
#include "CheckinPolicy.h"
#include "CheckinScheduler.h"
#include "CloudSync.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"
#include "ModalPrompt.h"
#include "ModalPromptModel.h"
#include "PromptPolicy.h"
#include "RGBController.h"
#include "ScrollLabel.h"
#include "StatusService.h"
#include "OtaUpdate.h"
#include "UpdateSession.h"
#include "globals.h"

namespace UpdatePrompt {
namespace {
using namespace PromptPolicy;
using CheckinPolicy::Policy;
using CheckinPolicy::Verdict;

auto& display = HAL::displayProxy();

constexpr int kScreenW = 128;
constexpr int kTitleH  = 14;
constexpr int kListY   = 16;
constexpr int kRowH    = 12;
constexpr int kRows    = 4;
constexpr int kTextX   = 4;
constexpr int kGutter  = 4;   // columns kept clear for the scrollbar

const char* running() { return getFirmwareVersionString(); }

// ---- stored settings (NVS `upd`) ---------------------------------------------

struct Stored {
    Policy policy = Policy::Auto;
    bool bootCheck = true;
    char rej[kMaxVersionLen + 1] = {0};
    char avail[kMaxVersionLen + 1] = {0};
    char src[40] = {0};
    char chan[16] = {0};
    char failVer[kMaxVersionLen + 1] = {0};   // an update that did not keep itself
    char websiteSeen[kMaxVersionLen + 1] = {0};
    bool autoapply = true;
};

void readText(Preferences& p, const char* key, char* out, size_t len) {
    out[0] = '\0';
    if (p.isKey(key)) p.getString(key, out, len);
}

Stored readStored() {
    Stored st;
    Preferences upd;
    if (!upd.begin(kNamespace, true)) return st;   // nothing stored yet
    char policy[8];
    readText(upd, kKeyPolicy, policy, sizeof(policy));
    st.policy = CheckinPolicy::parsePolicy(policy);
    const bool hasBootCheck = upd.isKey(CheckinPolicy::kKeyBootCheck);
    st.bootCheck = CheckinPolicy::parseBootCheck(
        hasBootCheck, hasBootCheck ? upd.getUChar(CheckinPolicy::kKeyBootCheck, 1) : 1);
    readText(upd, kKeyRej, st.rej, sizeof(st.rej));
    readText(upd, kKeyAvail, st.avail, sizeof(st.avail));
    readText(upd, kKeySrc, st.src, sizeof(st.src));
    readText(upd, kKeyChan, st.chan, sizeof(st.chan));
    readText(upd, OtaUpdate::kKeyFailVer, st.failVer, sizeof(st.failVer));
    readText(upd, kKeyWebsiteSeen, st.websiteSeen, sizeof(st.websiteSeen));
    const bool hasApply = upd.isKey(kKeyAutoapply);
    st.autoapply = parseAutoapply(hasApply, hasApply && upd.getBool(kKeyAutoapply, true));
    upd.end();
    return st;
}

bool writeText(const char* key, const char* value) {
    Preferences upd;
    if (!upd.begin(kNamespace, false)) return false;
    const bool ok = upd.putString(key, value) == strlen(value);
    upd.end();
    return ok;
}

bool removeKey(const char* key) {
    Preferences upd;
    if (!upd.begin(kNamespace, false)) return false;
    const bool ok = !upd.isKey(key) || upd.remove(key);
    upd.end();
    return ok;
}

void drawMessage(const char* first, const char* second) {
    display.clear();
    display.setColor(WHITE);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, first);
    if (second && second[0]) display.drawString(64, 34, second);
    display.display();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

// ---- prompts --------------------------------------------------------------------

char offerVersion[kMaxVersionLen + 1] = {0};
char offerSource[40] = {0};
char promptTitle[96] = {0};
bool appsAfter = false;        // the app-changes prompt follows the firmware one
bool pendingApplyOnce = false; // "Get them now": the next check applies waiting changes

void startApplyCheck();

void onAppsDone(int result) {
    const AppEffect e = appChoice(result);
    Serial.printf("[upd] prompt=apps choice=%s\n",
                  result == (int)AppChoice::GetNow ? "get" : result == (int)AppChoice::Later ? "later" : "none");
    if (e.keepInBar) {
        // Nothing stored: the waiting changes stay in the bar and the badge.
        StatusService::instance().post(StatusKind::ChangesWaiting, nullptr,
            StatusService::defaultPriority(StatusKind::ChangesWaiting), true, millis());
    }
    if (e.applyNow) startApplyCheck();
}

bool openAppsPrompt() {
    appsTitle(promptTitle, sizeof(promptTitle), 0);   // the check-in carries no count
    return ModalPrompt::instance().open(promptTitle, kAppOptions, 2, onAppsDone);
}

void openAppsIfPending() {
    if (!appsAfter) return;
    appsAfter = false;
    openAppsPrompt();
}

void onComingSoonDone(int) { openAppsIfPending(); }

// Install now: the update session runs after a restart (the start-up
// animation is skipped), in a power cycle that never starts Bluetooth.
bool handOff(const char* version) {
    const char* why = "";
    if (!UpdateSession::armInstall(version, &why)) {
        Serial.printf("[upd] install=refused reason=%s\n", why);
        return false;
    }
    Serial.printf("[upd] install=restarting version=%s\n", version);
    drawMessage("Restarting to update...", "");
    // Dev mode listening (or any check) switches WiFi off before the restart.
    CloudSync::cancelPending();
    ModalPrompt::instance().closeForTeardown();
    Serial.flush();
    delay(300);
    ESP.restart();
    return true;
}

void onFirmwareDone(int result) {
    // Until updates are signed, only a Fidget allowed over USB installs here;
    // every other one keeps the "update from the website" message.
    const FwEffect e = firmwareChoice(result, UpdateSession::installAllowed());
    StatusService& svc = StatusService::instance();
    bool stored = false;
    if (e.writeRej) {
        stored = writeText(kKeyRej, offerVersion);
        svc.clear(StatusKind::UpdateReady);
    }
    if (e.keepInBar) {
        char text[48];
        snprintf(text, sizeof(text), "Update %s ready", offerVersion);
        svc.post(StatusKind::UpdateReady, text,
                 StatusService::defaultPriority(StatusKind::UpdateReady), true, millis());
    }
    Serial.printf("[upd] prompt=firmware choice=%s version=%s rej_write=%s\n",
                  result == (int)FwChoice::Install ? "install" :
                  result == (int)FwChoice::Later ? "later" :
                  result == (int)FwChoice::Skip ? "skip" : "none",
                  offerVersion, e.writeRej ? (stored ? "ok" : "error") : "-");
    // A refused hand-off (storage failed) says so like "coming soon" would
    // not: the offer stays in the bar either way.
    if (e.handoff && !handOff(offerVersion)) {
        char text[48];
        snprintf(text, sizeof(text), "Update %s ready", offerVersion);
        svc.post(StatusKind::UpdateReady, text,
                 StatusService::defaultPriority(StatusKind::UpdateReady), true, millis());
        const char* const ok[] = {"OK"};
        if (ModalPrompt::instance().open("The update could not start. Nothing changed.", ok, 1,
                                         onComingSoonDone)) return;
    }
    if (e.comingSoon) {
        const char* const ok[] = {"OK"};
        if (ModalPrompt::instance().open(kInstallComingSoon, ok, 1, onComingSoonDone)) return;
    }
    openAppsIfPending();
}

bool openFirmwarePrompt(const char* version, const char* source) {
    strncpy(offerVersion, version, sizeof(offerVersion) - 1);
    offerVersion[sizeof(offerVersion) - 1] = '\0';
    strncpy(offerSource, source && source[0] ? source : kDefaultSource, sizeof(offerSource) - 1);
    offerSource[sizeof(offerSource) - 1] = '\0';
    firmwareTitle(promptTitle, sizeof(promptTitle), offerVersion, offerSource);
    return ModalPrompt::instance().open(promptTitle, kFwOptions, 3, onFirmwareDone);
}

bool openWebsitePrompt(const char* version) {
    const char* const ok[] = {"OK"};
    if (!ModalPrompt::instance().open(kWebsiteUpdateCopy, ok, 1, onComingSoonDone)) return false;
    // Remember only an instruction actually shown; a newer version may show
    // it once again. This path never offers Install now or restarts.
    writeText(kKeyWebsiteSeen, version);
    StatusService::instance().clear(StatusKind::UpdateReady);
    return true;
}

void showPlan(const PromptPlan& plan, const Stored& st) {
    if (plan.firmware) {
        appsAfter = plan.apps;
        if (plan.website ? openWebsitePrompt(st.avail) : openFirmwarePrompt(st.avail, st.src)) return;
        appsAfter = false;
    }
    if (plan.apps) openAppsPrompt();
}

bool bootArmed = false;

// ---- Check for updates screen ------------------------------------------------------

enum class CheckState : uint8_t { Start, Waiting, WaitingDev, Done };
CheckState checkState = CheckState::Start;
// Dev mode listening checks in every few seconds: a manual check asks for
// its next check-in now and shows that one's result.
uint32_t devPollsAtStart = 0;
uint32_t devCheckAt = 0;
constexpr uint32_t kDevCheckTimeoutMs = 20000;
char checkLine[48] = {0};
const char* checkNote = "";
bool checkExplainOff = false;

const char* errorNote(const char* err) {
    if (strcmp(err, "no-wifi") == 0) return "No saved network yet";
    if (strcmp(err, "not-linked") == 0) return "Link this Fidget first";
    if (strcmp(err, "join") == 0 || strcmp(err, "no-network") == 0) return "Network not in range";
    return "Try again later";
}

void setErrorText(CloudSync::Result& r, const char* err) {
    strncpy(r.err, err, sizeof(r.err) - 1);
    r.err[sizeof(r.err) - 1] = '\0';
}

void finishCheck(const CloudSync::Result& r) {
    const Stored st = readStored();
    const bool fw = offerEligible(st.avail, st.rej, running());
    const PromptPlan plan = manualPlan(st.policy, fw, r.waiting,
                                     UpdateSession::hasUpdateSlot(),
                                     sameVersion(st.avail, st.websiteSeen));
    checkExplainOff = plan.explainOff;
    checkNote = "";
    if (!r.ok) {
        snprintf(checkLine, sizeof(checkLine), "Could not check");
        checkNote = errorNote(r.err);
    } else if (r.appliedNow) {
        snprintf(checkLine, sizeof(checkLine), "App changes applied");
    } else if (r.waiting) {
        snprintf(checkLine, sizeof(checkLine), "App changes waiting");
    } else if (fw) {
        snprintf(checkLine, sizeof(checkLine), UpdateSession::hasUpdateSlot()
                 ? "Update %s ready" : "Update %s available", st.avail);
        // Honest about a version that did not keep itself here before.
        if (UpdateSession::hasUpdateSlot() &&
            !OtaUpdate::automaticOfferAllowed(st.avail, st.failVer))
            checkNote = "It did not finish last time";
    } else {
        snprintf(checkLine, sizeof(checkLine), "Your Fidget is up to date");
    }
    checkState = CheckState::Done;
    Serial.printf("[upd] check=done ok=%d err=%s firmware=%d apps=%d explain_off=%d\n",
                  r.ok ? 1 : 0, r.err, plan.firmware ? 1 : 0, plan.apps ? 1 : 0,
                  plan.explainOff ? 1 : 0);
    showPlan(plan, st);
}

void startCheck() {
    if (CloudSync::devListening()) {
        devPollsAtStart = CloudSync::devSnapshot().polls;
        devCheckAt = millis();
        CloudSync::devPollNow();
        checkState = CheckState::WaitingDev;
        Serial.println("[upd] check=dev-poll");
        return;
    }
    if (checkEntry(false, CloudSync::busy()) == CheckEntry::WatchSession) {
        // A check is already running (the start-up one): show its result.
        checkState = CheckState::Waiting;
        return;
    }
    CheckinPolicy::Inputs in;
    in.btIdle = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
    const Verdict v = CheckinPolicy::decide(CheckinPolicy::Session::Manual, in);
    if (v == Verdict::RebootFirst) {
        // After Bluetooth use the check runs after a restart; say so first.
        drawMessage(manualCopy(v), "");
        Serial.println("[upd] check=restart-first");
        delay(800);
    }
    const bool apply = pendingApplyOnce;
    pendingApplyOnce = false;
    if (!CheckinScheduler::checkNow(apply)) {
        snprintf(checkLine, sizeof(checkLine), "Could not check right now");
        checkNote = "Try again in a moment";
        checkExplainOff = false;
        checkState = CheckState::Done;
        return;
    }
    checkState = CheckState::Waiting;
}

void startApplyCheck() {
    pendingApplyOnce = true;
    if (AppManager::instance().activeApp() == APP_CHECK_UPDATES) {
        checkState = CheckState::Start;
    } else {
        AppManager::instance().switchToApp(APP_CHECK_UPDATES);
    }
}

void onCheckBack(const ButtonEvent& event) {
    // Leaving does not stop a running check: its result reaches the bar.
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

void onCheckEnter(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released && checkState == CheckState::Done)
        MenuManager::instance().returnToMenu();
}

// ---- Settings > Updates screen ---------------------------------------------------

ModalPromptModel listModel;   // selection + window, wraps like the menu
ScrollLabel focusLabel;
int focusFor = -1;
bool enterArmed = false;
Stored cache;
bool cacheLinked = false;

void refresh() {
    cache = readStored();
    char account[40];
    bool fingerprint = false;
    cacheLinked = CloudSync::linkStatus(account, fingerprint);
}

SettingsState settingsState() {
    SettingsState s;
    s.policy = cache.policy;
    s.bootCheck = cache.bootCheck;
    s.autoapply = cache.autoapply;
    s.channel = cache.chan;
    s.source = cache.src;
    s.avail = cache.avail;
    s.rej = cache.rej;
    s.running = running();
    s.linked = cacheLinked;
    s.hasUpdateSlot = UpdateSession::hasUpdateSlot();
    s.awake = AwakeMode::setting();
    const StatusEntry* cur = StatusService::instance().current();
    s.status = cur ? cur->text : "";
    return s;
}

void activate(Row row) {
    switch (row) {
        case Row::CheckNow:
            AppManager::instance().switchToApp(APP_CHECK_UPDATES);
            return;
        case Row::AutoCheck: {
            const bool turnOff = cache.policy != Policy::Never;
            writeText(kKeyPolicy, turnOff ? kPolicyNever : kPolicyAuto);
            refresh();
            if (turnOff) {
                const char* const ok[] = {"OK"};
                ModalPrompt::instance().open(kOffExplanation, ok, 1, nullptr);
            }
            return;
        }
        case Row::BootCheck: {
            Preferences upd;
            if (upd.begin(kNamespace, false)) {
                upd.putUChar(CheckinPolicy::kKeyBootCheck, cache.bootCheck ? 0 : 1);
                upd.end();
            }
            Serial.printf("[upd] boot_check=%d\n", cache.bootCheck ? 0 : 1);
            refresh();
            const char* const ok[] = {"OK"};
            ModalPrompt::instance().open(kBootCheckExplanation, ok, 1, nullptr);
            return;
        }
        case Row::AutoApply: {
            Preferences upd;
            if (upd.begin(kNamespace, false)) {
                upd.putBool(kKeyAutoapply, !cache.autoapply);
                upd.end();
            }
            refresh();
            return;
        }
        case Row::Skip: {
            const SkipAction action = skipAction(settingsState());
            if (action == SkipAction::Unskip) removeKey(kKeyRej);
            if (action == SkipAction::Skip) {
                writeText(kKeyRej, cache.avail);
                StatusService::instance().clear(StatusKind::UpdateReady);
            }
            refresh();
            return;
        }
        case Row::Link:
            AppManager::instance().switchToApp(APP_LINK);
            return;
        case Row::Awake:
            AppManager::instance().switchToApp(APP_AWAKE);
            return;
        case Row::Status:
            AppManager::instance().switchToApp(APP_STATUS);
            return;
        case Row::Channel:
        case Row::Source:
            return;   // shown only; there is nothing to change yet
    }
}

void onSettingsUp(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveUp((uint32_t)millis());
}
void onSettingsDown(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveDown((uint32_t)millis());
}
void onSettingsEnter(const ButtonEvent& event) {
    // Only a press that started on this screen chooses (on its release).
    if (event.eventType == ButtonEvent_Pressed) enterArmed = true;
    if (event.eventType != ButtonEvent_Released || !enterArmed) return;
    enterArmed = false;
    activate(settingsRow(listModel.selected()));
}
void onSettingsBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

} // namespace

// ---- post-boot popup ------------------------------------------------------------------

void armBootPopup() { bootArmed = true; }

void loop() {
    if (!bootArmed) return;
    const AppIndex active = AppManager::instance().activeApp();
    if (active == APP_BOOT_ANIMATION) return;
    if (active != APP_MENU) { bootArmed = false; return; }
    ModalPrompt& prompt = ModalPrompt::instance();
    if (prompt.isOpen() || !prompt.canOpen()) return;
    bootArmed = false;
    CloudSync::Result r;
    const bool have = CheckinScheduler::takeBootResult(r);
    const Stored st = readStored();
    // An update that already failed to keep itself here is not offered
    // automatically again; a manual check still shows it.
    const bool retryHeld = !OtaUpdate::automaticOfferAllowed(st.avail, st.failVer);
    const PromptPlan plan = bootPlan(st.policy,
                                     offerEligible(st.avail, st.rej, running()) && !retryHeld,
                                     have && r.waiting, UpdateSession::hasUpdateSlot(),
                                     sameVersion(st.avail, st.websiteSeen));
    Serial.printf("[upd] boot-popup result=%d firmware=%d apps=%d failed_held=%d\n",
                  have ? 1 : 0, plan.firmware ? 1 : 0, plan.apps ? 1 : 0,
                  retryHeld && st.avail[0] ? 1 : 0);
    showPlan(plan, st);
}

// ---- Check for updates --------------------------------------------------------------

void checkBegin() {
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_SelectIndex, onCheckBack);
    buttons.registerCallback(button_EnterIndex, onCheckEnter);
    setColorsOff();
    checkState = CheckState::Start;
    checkLine[0] = '\0';
    checkNote = "";
    checkExplainOff = false;
}

void resumeCheck(bool sessionStarted) {
    // Decided before the screen's first pass: a recovery session that has
    // already finished is still this screen's result, never a reason to
    // start a second check.
    const bool watch = checkEntry(sessionStarted, CloudSync::busy()) == CheckEntry::WatchSession;
    if (watch) checkState = CheckState::Waiting;
    Serial.printf("[upd] check=resumed started=%d watch=%d\n", sessionStarted ? 1 : 0, watch ? 1 : 0);
}

void checkEnd() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_SelectIndex);
    buttons.unregisterCallback(button_EnterIndex);
    pendingApplyOnce = false;
    setColorsOff();
}

void checkUpdate() {
    if (checkState == CheckState::Start) startCheck();
    if (checkState == CheckState::WaitingDev) {
        const CloudSync::DevSnapshot snap = CloudSync::devSnapshot();
        if (snap.polls > devPollsAtStart) {
            finishCheck(snap.last);
            if (ModalPrompt::instance().isOpen()) return;
        } else if (!CloudSync::devListening() || millis() - devCheckAt > kDevCheckTimeoutMs) {
            CloudSync::Result none;
            setErrorText(none, "join");
            finishCheck(none);
            if (ModalPrompt::instance().isOpen()) return;
        } else {
            drawMessage(kChecking, "");
            return;
        }
    }
    if (checkState == CheckState::Waiting) {
        if (!CloudSync::busy()) {
            finishCheck(CloudSync::lastResult());
            if (ModalPrompt::instance().isOpen()) return;   // the prompt draws
        } else {
            drawMessage(kChecking, "");
            return;
        }
    }
    display.clear();
    display.setColor(WHITE);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (checkExplainOff) {
        // No title bar: the explanation needs three lines, so a failed
        // check shows its reason ("Link this Fidget first") as the headline.
        display.drawString(64, 1, checkNote[0] ? checkNote : checkLine);
        display.drawStringMaxWidth(64, 16, kScreenW - 4, kOffExplanation);
    } else {
        display.fillRect(0, 0, kScreenW, kTitleH);
        display.setColor(BLACK);
        display.drawString(64, 1, "Check for updates");
        display.setColor(WHITE);
        display.drawString(64, 22, checkLine);
        if (checkNote[0]) display.drawString(64, 36, checkNote);
        display.drawString(64, 51, "Enter: done");
    }
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

// ---- Settings > Updates ------------------------------------------------------------------

void settingsBegin() {
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_UpIndex, onSettingsUp);
    buttons.registerCallback(button_DownIndex, onSettingsDown);
    buttons.registerCallback(button_EnterIndex, onSettingsEnter);
    buttons.registerCallback(button_SelectIndex, onSettingsBack);
    setColorsOff();
    listModel.open(kSettingsRows, kRows, (uint32_t)millis(), 0);
    focusFor = -1;
    enterArmed = false;
    refresh();
}

void settingsEnd() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_UpIndex);
    buttons.unregisterCallback(button_DownIndex);
    buttons.unregisterCallback(button_EnterIndex);
    buttons.unregisterCallback(button_SelectIndex);
    listModel.dismiss();
    setColorsOff();
}

void settingsUpdate() {
    const SettingsState s = settingsState();
    char line[kRowText];

    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 1, "Updates");
    display.setColor(WHITE);

    if (listModel.selected() != focusFor) {
        focusFor = listModel.selected();
        focusLabel.restart((uint32_t)millis());
    }
    const int rowW = kScreenW - kGutter;
    const int start = listModel.windowStart();
    for (int r = 0; r < listModel.windowCount(); r++) {
        const int idx = start + r;
        const int y = kListY + r * kRowH;
        settingsLabel(settingsRow(idx), s, line, sizeof(line));
        if (idx == listModel.selected()) {
            display.setColor(WHITE);
            display.fillRect(0, y, rowW, kRowH);
            display.setColor(BLACK);
            focusLabel.draw(kTextX, y - 1, rowW - 2 * kTextX, line, false);
            display.setColor(WHITE);
        } else {
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(kTextX, y - 1, line);
        }
    }
    // Unfocused long rows run under the gutter: clear it, then the scrollbar.
    const int listH = kRows * kRowH;
    display.setColor(BLACK);
    display.fillRect(rowW, kListY, kGutter, listH);
    display.setColor(WHITE);
    const int thumbH = (listH * listModel.windowCount()) / kSettingsRows;
    const int thumbY = kListY + (listH * start) / kSettingsRows;
    display.drawRect(kScreenW - 2, kListY, 2, listH);
    display.fillRect(kScreenW - 2, thumbY, 2, thumbH);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

// ---- bench verbs --------------------------------------------------------------------------

#ifdef CF_TEST_CLI
void printState() {
    Preferences upd;
    const bool open = upd.begin(kNamespace, true);
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace, NVS_TYPE_ANY, &it);
    int count = 0;
    while (err == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        count++;
        switch (info.type) {
            case NVS_TYPE_STR: {
                char value[128] = {0};
                if (open) upd.getString(info.key, value, sizeof(value));
                Serial.printf("[cmd] upd.key=%s type=str value=%s\n", info.key, value);
                break;
            }
            case NVS_TYPE_U8:
                Serial.printf("[cmd] upd.key=%s type=u8 value=%u\n", info.key,
                              open ? (unsigned)upd.getUChar(info.key, 0) : 0u);
                break;
            case NVS_TYPE_U32:
                Serial.printf("[cmd] upd.key=%s type=u32 value=%lu\n", info.key,
                              open ? (unsigned long)upd.getUInt(info.key, 0) : 0ul);
                break;
            case NVS_TYPE_I32:
                Serial.printf("[cmd] upd.key=%s type=i32 value=%ld\n", info.key,
                              open ? (long)upd.getInt(info.key, 0) : 0l);
                break;
            default:
                Serial.printf("[cmd] upd.key=%s type=0x%02x\n", info.key, (unsigned)info.type);
                break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (open) upd.end();
    Serial.printf("[cmd] upd.done=%d\n", count);
}

void injectOffer(const char* args) {
    char version[kMaxVersionLen + 1] = {0};
    const char* space = strchr(args, ' ');
    const size_t n = space ? (size_t)(space - args) : strlen(args);
    if (n == 0 || n >= sizeof(version)) {
        Serial.println("[err] upd.usage=upd [offer <version> [source]]");
        return;
    }
    memcpy(version, args, n);
    const char* source = space ? space + 1 : "";
    while (*source == ' ') source++;
    ModalPrompt& prompt = ModalPrompt::instance();
    if (prompt.isOpen()) { Serial.println("[err] upd.offer=busy"); return; }
    if (!prompt.canOpen()) { Serial.println("[err] upd.offer=not-menu"); return; }
    const Stored st = readStored();
    if (!offerEligible(version, st.rej, running())) {
        Version parsed;
        Serial.printf("[cmd] upd.offer=suppressed reason=%s version=%s\n",
                      sameVersion(version, st.rej) ? "skipped" :
                      parseVersion(version, parsed) ? "not-newer" : "invalid", version);
        return;
    }
    appsAfter = false;
    const bool website = !UpdateSession::hasUpdateSlot();
    if (website && sameVersion(version, st.websiteSeen)) {
        Serial.printf("[cmd] upd.offer=suppressed reason=website-shown version=%s\n", version);
        return;
    }
    if (!(website ? openWebsitePrompt(version) : openFirmwarePrompt(version, source))) {
        Serial.println("[err] upd.offer=refused"); return;
    }
    Serial.printf("[cmd] upd.offer=open version=%s source=%s\n", version, source);
}
#endif

} // namespace UpdatePrompt
