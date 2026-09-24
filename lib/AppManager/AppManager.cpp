// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "AppManager.h"
#include "HAL.h"
#include "MenuManager.h"
#include "ModalPrompt.h"
#include "globals.h"
#include "PowerManager.h"
#include "BatteryDiary.h"
#include "AppDefs.h"
#include "SerialCli.h"
#include "LoadoutManifest.h"
#include "LoadoutStore.h"
#include "CloudSync.h"
#include "CheckinScheduler.h"
#include "UpdateSession.h"
#include "DeviceIdentity.h"
#include "UpdatePrompt.h"
#include "PromptPolicy.h"
#include <Preferences.h>

void (*keep_functions[])() = {menuBegin, menuEnd, menuRun};

static auto& buttonManager = HAL::buttonManager();

// One centered line while a radio app waits for a network check to end.
static void showRadioNotice(const char* text) {
    DisplayProxy& screen = HAL::displayProxy();
    screen.clear();
    screen.setColor(WHITE);
    screen.setFont(ArialMT_Plain_10);
    screen.setTextAlignment(TEXT_ALIGN_CENTER);
    screen.drawString(64, 27, text);
    screen.display();
}
static PowerManager powerManager(buttonManager);

// Set in setup(); the boot-window check starts on the first loop pass, once
// the battery has been read.
static bool bootWindowPending = false;
static bool bootWasOneShot = false;

#ifdef CF_TEST_CLI
// Bench only: lets the Music Player start after WiFi in the same power
// cycle, to measure whether that works. Never compiled into a release.
static bool testAllowBtAfterWifi = false;
void AppManager::setTestAllowBtAfterWifi(bool allow) { testAllowBtAfterWifi = allow; }
#endif

// Prompts pause the menu only (and the boot screen that hands over to it).
// Apps that need a prompt must extend this deliberately: the prompt pauses
// whatever app is underneath, and most apps are not written to be paused.
// The update screens only draw and wait for buttons, so pausing them is safe.
static bool appTakesPrompts(AppIndex app)
{
    return app == APP_MENU || app == APP_BOOT_ANIMATION || app == APP_LINK ||
           app == APP_CHECK_UPDATES || app == APP_UPDATES;
}

// Singleton instance
AppManager& AppManager::instance() {
    static AppManager singleton;
    return singleton;
}

// Private constructor
AppManager::AppManager() {
}

void AppManager::setup() {
    // A freshly installed image checks itself before anything else runs:
    // nothing below may start while it is still unconfirmed.
    UpdateSession::beginSelfTest();
    HAL::configureWakeupPins();
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    esp_log_level_set(TAG_MAIN, ESP_LOG_VERBOSE);
    HAL::initHardware();
    // Pending image: pass, or restart into the previous image.
    UpdateSession::finishBoot();
    {
        // "Install now" restarted into the update session: it never
        // returns (it restarts), and nothing else starts in this power cycle.
        char version[32];
        if (UpdateSession::takeSessionRequest(version, sizeof(version)))
            UpdateSession::runSession(version);
    }
    int32_t wakeVcellMv = -1, wakeSocPct = -1;
    if (HAL::timerCheckinWake(wakeVcellMv, wakeSocPct)) {
        // A timer wake with a check-in due: headless, then back to sleep.
        CheckinScheduler::runHeadless(wakeVcellMv, wakeSocPct);
    }
    HAL::setBeforeSleep(CheckinScheduler::armBeforeSleep);
    DeviceIdentity::checkStored();

    // Mount the filesystem before the first menu build: MenuManager::begin
    // -> buildNestedMenu reads /loadout.json through LoadoutStore.
    LoadoutStore::begin();
    BatteryDiary::begin(HAL::bootWakeupCauseName());

    ESP_LOGI(TAG_MAIN, "AppManager setup complete");

    ESP_LOGI(TAG_MAIN, "AppManager setup start");
    // Force creation of MenuManager now, so we can see if it bombs
    MenuManager &m = MenuManager::instance();
    ESP_LOGI(TAG_MAIN, "MenuManager::instance() returned: %p", (void*)&m);

    // Portal entry and exit set these one-shots immediately before reboot.
    // Read-write mode is required so the same boot consumes the flags.
    Preferences bootPrefs;
    bool skipBootAnimation = false;
    bool bootPortal = false;
    bool bootCloud = false;
    bool bootApply = false;
    bool bootMusic = false;
    bool bootLink = false;
    bool bootUnlink = false;
    if (bootPrefs.begin("bootcfg", false)) {
        skipBootAnimation = bootPrefs.getBool("skipanim", false);
        bootPortal = bootPrefs.getBool("bootapp", false);
        bootCloud = bootPrefs.getBool("bootcloud", false);
        bootApply = bootPrefs.getBool("bootapply", false);
        bootMusic = bootPrefs.getBool("bootmusic", false);
        bootLink = bootPrefs.getBool("bootlink", false);
        bootUnlink = bootPrefs.getBool("bootunlink", false);
        bootPrefs.remove("skipanim");
        bootPrefs.remove("bootapp");
        bootPrefs.remove("bootcloud");
        bootPrefs.remove("bootapply");
        bootPrefs.remove("bootmusic");
        bootPrefs.remove("bootlink");
        bootPrefs.remove("bootunlink");
        bootPrefs.end();
    } else {
        ESP_LOGW(TAG_MAIN, "Failed to open boot preferences");
    }
    CloudSync::recoverFailure();
    // A manual check that restarted after Bluetooth use continues here.
    const PromptPolicy::CheckResume resume =
        PromptPolicy::resumeAfterRestart(bootCloud, bootApply, bootPortal || bootMusic);

    // A Bluetooth app relaunched across the reboot that followed a network
    // check starts in a clean power cycle. A check asked for after Bluetooth
    // use continues on the Check for updates screen, which shows its result.
    appActive     = bootPortal ? APP_WEB_PORTAL
                  : bootMusic  ? APP_MUSIC_PLAYER
                  : bootLink   ? APP_LINK
                  : resume.openCheckScreen ? APP_CHECK_UPDATES
                               : (skipBootAnimation ? APP_MENU : APP_BOOT_ANIMATION);
    appPreviously = APP_MENU;

    printAppCount(); // Debugging
    ESP_LOGI(TAG_MAIN, "About to call debugAppDefs() for appActive=%d", (int)appActive);
    debugAppDefs();

    ESP_LOGI(TAG_MAIN, "App active index = %d", (int)appActive);
    ESP_LOGI(TAG_MAIN, "beginFunc is %s", (appDefs[appActive].beginFunc ? "set" : "null"));
    ESP_LOGI(TAG_MAIN, "menuBegin address: %p", (void*)menuBegin);
    ESP_LOGI(TAG_MAIN, "menuEnd address: %p", (void*)menuEnd);
    ESP_LOGI(TAG_MAIN, "menuRun address: %p", (void*)menuRun);

    // Start the menu
    appDefs[appActive].beginFunc();
    ModalPrompt::instance().setHostAllows(appTakesPrompts(appActive));
    if (bootLink && !CloudSync::busy()) CloudSync::startLink();
    if (resume.runCheck) {
        const bool started = CloudSync::runSession(CloudSync::Reason::Recovery, resume.applyWaiting);
        // The screen shows this session's result once and never starts
        // another check for it, however fast it finishes.
        if (appActive == APP_CHECK_UPDATES) UpdatePrompt::resumeCheck(started);
    }
    if (bootUnlink && !bootPortal && !bootMusic) CloudSync::startUnlink();
    bootWindowPending = true;
    bootWasOneShot = skipBootAnimation || bootPortal || bootMusic || bootLink ||
                     bootUnlink || bootCloud;
    // The update popup follows the start-up animation only.
    if (appActive == APP_BOOT_ANIMATION) UpdatePrompt::armBootPopup();

    ESP_LOGI(TAG_MAIN, "Returned from beginFunc() for appActive=%d", (int)appActive);
}

void AppManager::loop() {
    // A just-installed image is kept only after one full pass that ran the
    // active app (its first frame); a crash before that returns to the
    // previous image.
    static bool frameDrawn = false;
    UpdateSession::loopTick(frameDrawn);
    HAL::loopHardware();

    if (bootWindowPending) {
        // loopHardware() has read the battery by now. Starting the check
        // only creates its task: the animation and menu never wait for it.
        bootWindowPending = false;
        CheckinScheduler::startBootWindow(bootWasOneShot);
    }
    CheckinScheduler::loop();
    UpdatePrompt::loop();

    processButtonEvents();
    SerialCli::instance().poll();

#ifdef CF_TEST_CLI
    if (SerialCli::instance().soakActive()) {
        millis_APP_LASTINTERACTION = millis_NOW;
    }
#endif

    if ((millis_NOW - millis_APP_TASK_20MS) >= TASK_20MS) {
        millis_APP_TASK_20MS = millis_NOW;
        runActiveApp();
        frameDrawn = true;
    }

    if ((millis_NOW - millis_APP_TASK_200MS) >= TASK_200MS) {
        millis_APP_TASK_200MS = millis_NOW;
    }

    if (HAL::consumeRuntimeBatteryShutdownRequest()) {
        BatteryDiary::onRuntimeShutdown(batteryVoltage,
                                        batteryVoltagePercentage,
                                        batteryChangeRate);
        ModalPrompt::instance().closeForTeardown();
        powerManager.shutdownForEmptyBattery();
        return;
    }

#ifdef CF_TEST_CLI
    if (SerialCli::instance().consumeSleepRequest()) {
        ModalPrompt::instance().closeForTeardown();
        powerManager.deepSleep(true);
        return;
    }
#endif

    // A network check in progress holds off the idle sleep: sleeping would
    // cut WiFi mid-exchange and lose the follow-up report and the result.
    // The session is bounded by its own deadline, and the idle period
    // restarts when it ends so its status line can be seen.
    if (CloudSync::busy()) {
        millis_APP_LASTINTERACTION = millis_NOW;
    }

    if ((millis_NOW - millis_APP_LASTINTERACTION) >= TASK_LASTINTERACT) {
        // An open prompt does not keep the device awake: it closes with no
        // choice first, then sleep proceeds exactly as it does without one.
        ModalPrompt::instance().closeForTeardown();
        powerManager.deepSleep();
    }
}

void AppManager::runActiveApp()
{
    // An open prompt owns the screen and buttons; the app underneath is
    // paused (its update is skipped) until the prompt closes.
    if (ModalPrompt::instance().isOpen()) {
        ModalPrompt::instance().update();
        return;
    }
    // calls the runFunc for the currently active app
    appDefs[appActive].runFunc();
}

void AppManager::processButtonEvents()
{
    ButtonEvent ev;
    while (HAL::buttonManager().getNextEvent(ev))
    {
        // The tail (Held/Release) of a press that began inside a prompt
        // stays out of the app the prompt was covering.
        if (ModalPrompt::instance().swallowEvent(ev)) continue;
        if (HAL::buttonManager().hasCallback(ev.buttonIndex)) {
            auto cb = HAL::buttonManager().getCallback(ev.buttonIndex);
            if (cb) cb(ev);
        } else {
            ESP_LOGD(TAG_MAIN, "Unhandled button event: %d %d", ev.buttonIndex, ev.eventType);
        }
    }
}

void AppManager::persistMenuArrangement(const std::vector<LoadoutManifest::ArrangeItem>& order)
{
    // The network worker applies documents to the same manifest.
    LoadoutStore::Guard manifestGuard;
    // Start from the stored manifest; a device that has never persisted
    // one gets a baseline snapshot of the compiled-in registry so the
    // arrange has something to anchor against.
    LoadoutManifest::Loadout loadout;
    std::string json;
    if (!loadLoadoutManifest(loadout, &json)) {
        auto registry = buildLoadoutRegistryView();
        loadout = LoadoutManifest::buildFromRegistry(registry.data(), (int)registry.size());
    }

    LoadoutManifest::applyArrange(loadout, order);

    if (LoadoutStore::save(LoadoutManifest::serializeManifest(loadout))) {
        ESP_LOGI(TAG_MAIN, "Loadout manifest persisted (%d entries)",
                 (int)loadout.entries.size());
    } else {
        // Non-fatal: the in-memory menu keeps the new order until reboot;
        // worst case the reorder doesn't survive power-off.
        ESP_LOGE(TAG_MAIN, "Failed to persist loadout manifest");
    }
}

bool AppManager::applyLoadoutOps(const char* opsJson, int* entriesOut, int* appliedOut)
{
    // Read-modify-write under the same lock as the menu reorder.
    LoadoutStore::Guard manifestGuard;
    if (entriesOut) *entriesOut = 0;
    if (appliedOut) *appliedOut = 0;

    // Start from the stored manifest; a device that has never persisted one
    // gets a baseline snapshot of the compiled-in registry so removes/hides/
    // arranges have real entries to anchor against (same seed the reorder
    // path uses).
    LoadoutManifest::Loadout loadout;
    std::string json;
    if (!loadLoadoutManifest(loadout, &json)) {
        auto registry = buildLoadoutRegistryView();
        loadout = LoadoutManifest::buildFromRegistry(registry.data(), (int)registry.size());
    }

    int applied = 0;
    if (!LoadoutManifest::applyOps(loadout, opsJson, &applied)) {
        ESP_LOGW(TAG_MAIN, "Loadout ops rejected; manifest unchanged");
        return false; // malformed or a rejected op — nothing was applied
    }

    // Persist atomically (temp file + rename): a torn write leaves the old
    // manifest or none, never a corrupt one, so the boot menu can always
    // fall back per the missing/stale-manifest rules.
    if (!LoadoutStore::save(LoadoutManifest::serializeManifest(loadout))) {
        ESP_LOGE(TAG_MAIN, "Failed to persist loadout after ops apply");
        return false;
    }

    if (entriesOut) *entriesOut = (int)loadout.entries.size();
    if (appliedOut) *appliedOut = applied;
    ESP_LOGI(TAG_MAIN, "Loadout ops applied (%d ops, %d entries)",
             applied, (int)loadout.entries.size());
    // T-183: the menu tree is built from this manifest; a sync just changed
    // it, so the next menu entry must rebuild (ferried apps appear without a
    // reboot). The rebuild happens on menu-entry, not here.
    MenuManager::instance().markManifestDirty();
    return true;
}

void AppManager::switchToApp(AppIndex newApp)
{
    ESP_LOGI(TAG_MAIN, "Switching to app %d", newApp);
    if (newApp == appActive) return;
    if (newApp == APP_MUSIC_PLAYER || newApp == APP_WEB_PORTAL) {
        // Radio apps: a network check must be over (WiFi off) before they
        // start. Bluetooth after any WiFi use this power cycle, or a check
        // that would not stop in time, goes through a reboot that
        // relaunches the app.
        const bool checking = CloudSync::busy();
        if (checking) showRadioNotice("Finishing check...");
        const bool stopped = CloudSync::cancelPending();
        bool btAfterWifi = newApp == APP_MUSIC_PLAYER &&
                           CloudSync::radioUsedThisPowerCycle();
#ifdef CF_TEST_CLI
        if (btAfterWifi && testAllowBtAfterWifi) {
            Serial.println("[checkin] bt-after-wifi=allowed-by-test");
            btAfterWifi = false;
        }
#endif
        if (checking) Serial.printf("[checkin] cancel-for-radio-app stopped=%d\n", stopped ? 1 : 0);
        if (!stopped || btAfterWifi) {
            Preferences boot;
            if (boot.begin("bootcfg", false)) {
                boot.putBool("skipanim", true);
                boot.putBool(newApp == APP_MUSIC_PLAYER ? "bootmusic" : "bootapp", true);
                boot.end();
            }
            if (!checking) showRadioNotice("Restarting...");
            ModalPrompt::instance().closeForTeardown();
            Serial.flush();
            delay(50);
            ESP.restart();
            return;
        }
    }

    // An open prompt belongs to the app being left: close it (no choice)
    // while that app's button callbacks can still be handed back.
    ModalPrompt::instance().closeForTeardown();

    // end old
    appDefs[appActive].endFunc();

    appPreviously = appActive;
    appActive     = newApp;

    // begin new
    appDefs[appActive].beginFunc();
    ModalPrompt::instance().setHostAllows(appTakesPrompts(appActive));
}
