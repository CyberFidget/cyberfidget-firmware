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

// Prompts pause the menu only (and the boot screen that hands over to it).
// Apps that need a prompt must extend this deliberately: the prompt pauses
// whatever app is underneath, and most apps are not written to be paused.
static bool appTakesPrompts(AppIndex app)
{
    return app == APP_MENU || app == APP_BOOT_ANIMATION;
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
    HAL::configureWakeupPins();
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    esp_log_level_set(TAG_MAIN, ESP_LOG_VERBOSE);
    HAL::initHardware();

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
    bool bootMusic = false;
    if (bootPrefs.begin("bootcfg", false)) {
        skipBootAnimation = bootPrefs.getBool("skipanim", false);
        bootPortal = bootPrefs.getBool("bootapp", false);
        bootCloud = bootPrefs.getBool("bootcloud", false);
        bootMusic = bootPrefs.getBool("bootmusic", false);
        bootPrefs.remove("skipanim");
        bootPrefs.remove("bootapp");
        bootPrefs.remove("bootcloud");
        bootPrefs.remove("bootmusic");
        bootPrefs.end();
    } else {
        ESP_LOGW(TAG_MAIN, "Failed to open boot preferences");
    }
    CloudSync::recoverFailure();

    // A Bluetooth app relaunched across the reboot that followed a network
    // check starts in a clean power cycle.
    appActive     = bootPortal ? APP_WEB_PORTAL
                  : bootMusic  ? APP_MUSIC_PLAYER
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
    if (bootCloud && !bootPortal && !bootMusic) CloudSync::runSession(CloudSync::Reason::Recovery);

    ESP_LOGI(TAG_MAIN, "Returned from beginFunc() for appActive=%d", (int)appActive);
}

void AppManager::loop() {
    HAL::loopHardware();

    CloudSync::poll();

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
        const bool btAfterWifi = newApp == APP_MUSIC_PLAYER &&
                                 CloudSync::radioUsedThisPowerCycle();
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
