// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "AppManager.h"
#include "HAL.h"
#include "MenuManager.h"
#include "globals.h"
#include "PowerManager.h"
#include "AppDefs.h"
#include "SerialCli.h"
#include "LoadoutManifest.h"
#include "LoadoutStore.h"

void (*keep_functions[])() = {menuBegin, menuEnd, menuRun};

static auto& buttonManager = HAL::buttonManager();
static PowerManager powerManager(buttonManager);

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

    ESP_LOGI(TAG_MAIN, "AppManager setup complete");

    ESP_LOGI(TAG_MAIN, "AppManager setup start");
    // Force creation of MenuManager now, so we can see if it bombs
    MenuManager &m = MenuManager::instance();
    ESP_LOGI(TAG_MAIN, "MenuManager::instance() returned: %p", (void*)&m);

    // Default to menu
    appActive     = APP_BOOT_ANIMATION;
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

    ESP_LOGI(TAG_MAIN, "Returned from beginFunc() for appActive=%d", (int)appActive);
}

void AppManager::loop() {
    HAL::loopHardware();

    processButtonEvents();
    SerialCli::instance().poll();

    if ((millis_NOW - millis_APP_TASK_20MS) >= TASK_20MS) {
        millis_APP_TASK_20MS = millis_NOW;
        runActiveApp();
    }

    if ((millis_NOW - millis_APP_TASK_200MS) >= TASK_200MS) {
        millis_APP_TASK_200MS = millis_NOW;
    }

    if ((millis_NOW - millis_APP_LASTINTERACTION) >= TASK_LASTINTERACT) {
        powerManager.deepSleep();
    }
}

void AppManager::runActiveApp()
{
    // calls the runFunc for the currently active app
    appDefs[appActive].runFunc();
}

void AppManager::processButtonEvents()
{
    ButtonEvent ev;
    while (HAL::buttonManager().getNextEvent(ev))
    {
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
    // Start from the stored manifest; a device that has never persisted
    // one gets a baseline snapshot of the compiled-in registry so the
    // arrange has something to anchor against.
    LoadoutManifest::Loadout loadout;
    std::string json;
    if (!(LoadoutStore::load(json) && LoadoutManifest::parseManifest(json.c_str(), loadout))) {
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

void AppManager::switchToApp(AppIndex newApp)
{
    ESP_LOGI(TAG_MAIN, "Switching to app %d", newApp);
    if (newApp == appActive) return;

    // end old
    appDefs[appActive].endFunc();

    appPreviously = appActive;
    appActive     = newApp;

    // begin new
    appDefs[appActive].beginFunc();
}
