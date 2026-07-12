// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "AppDefs.h"
#include <vector>
#include <string>
#include <cstring>
#include <iostream>
#include "esp_log.h"

#include "LoadoutManifest.h"
#include "LoadoutStore.h"

#include "AppManifest_Includes.h"

// Print APP_COUNT value when the program starts
void printAppCount() {
    std::cout << "APP_COUNT is " << APP_COUNT << std::endl;
}

// This definition should only exist in ONE .cpp file
PrintAppCount<APP_COUNT> appCountPrinter;

extern void menuBegin();
extern void menuEnd();
extern void menuRun();

// 1) Build the array with the lines from "AppManifest.h"
#define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) \
    { LABEL, CATPATH, BEGINF, ENDF, RUNF },

AppDefinition appDefs[APP_COUNT] = {
    #include "AppManifest.h"  // each line expands into { LABEL, PATH, begin..., end..., run... }
};

#undef APP_ENTRY

// Parallel legacy enum strings retained for exact manifest migration.
#define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) #ID,

const char* const appIds[APP_COUNT] = {
    #include "AppManifest.h"  // each line expands into "APP_..."
};

#undef APP_ENTRY

// Debugging
static_assert(sizeof(appDefs)/sizeof(appDefs[0]) == APP_COUNT, 
              "Mismatch between appDefs and APP_COUNT?");

//__attribute__((constructor)) // Ensures it runs at startup
void debugAppDefs() {
   for (int i = 0; i < (int)APP_COUNT; i++) {
       ESP_LOGI(TAG_MAIN, "Index=%d name=%s path=%s beginFunc=%p",
                i,
                (appDefs[i].name ? appDefs[i].name : "(null)"),
                (appDefs[i].categoryPath ? appDefs[i].categoryPath : "(null)"),
                (void*)(appDefs[i].beginFunc));
   }
}

// 2) (Optional) A function to parse each categoryPath
static void addAppToMenu(const char* label, const char* path, int appIndex)
{
    // For example, pass it to your MenuManager or something. We'll do a minimal example:
    // We might store in some global data structure “menuRoot”
    // Then parse path by splitting on '/'
    // E.g. "Tools/WiFi" => [ "Tools", "WiFi" ]
    // Then go create subcategories if needed, etc.

    // Pseudocode for path splitting:
    // Tools => Subcategory "WiFi" => add item "label" => appIndex
    // We'll keep it simple:
    // MenuManager::instance().addItem(path, label, (AppIndex)appIndex);

    // We'll do a simple placeholder:
    MenuManager::instance().registerApp(path, label, (AppIndex)appIndex);
}

std::vector<LoadoutManifest::RegistryApp> buildLoadoutRegistryView() {
    std::vector<LoadoutManifest::RegistryApp> view;
    view.reserve((size_t)APP_COUNT);
    for (int i = 0; i < (int)APP_COUNT; i++) {
        LoadoutManifest::RegistryApp app;
        app.name     = appDefs[i].name ? appDefs[i].name : "";
        app.id       = LoadoutManifest::slugifyBuiltinName(app.name.c_str());
        app.category = LoadoutManifest::flattenCategory(appDefs[i].categoryPath);
        app.legacyId = appIds[i];
        view.push_back(app);
    }
    return view;
}

bool loadLoadoutManifest(LoadoutManifest::Loadout& loadout, std::string* jsonOut) {
    std::string json;
    if (!LoadoutStore::load(json) ||
        !LoadoutManifest::parseManifest(json.c_str(), loadout)) return false;
    auto registry = buildLoadoutRegistryView();
    if (LoadoutManifest::normalizeBuiltinIds(loadout, registry.data(),
                                             (int)registry.size())) {
        json = LoadoutManifest::serializeManifest(loadout);
        if (!LoadoutStore::save(json))
            ESP_LOGE("AppDefs", "Failed to persist builtin id migration");
    }
    if (jsonOut) *jsonOut = std::move(json);
    return true;
}

void buildNestedMenu() {
   // Manifest-driven path: /loadout.json defines menu order, one-level
   // flat categories, and hidden flags. mergeWithRegistry prunes stale
   // ids and appends compiled-in apps the manifest doesn't know about,
   // so the menu and the firmware never fall out of sync.
   std::string json;
   LoadoutManifest::Loadout loadout;
   if (loadLoadoutManifest(loadout, &json)) {
       auto registry = buildLoadoutRegistryView();
       auto merged   = LoadoutManifest::mergeWithRegistry(loadout,
                                                          registry.data(),
                                                          (int)registry.size());
       ESP_LOGI("AppDefs", "Menu from manifest: %d entries (%d compiled-in)",
                (int)merged.size(), (int)APP_COUNT);
       for (const auto& m : merged) {
           if (m.hidden) continue;
           if (m.appIndex < 0) {
               // Ferried wasm app (T-183): register a blob leaf launched
               // through the shared WASM_HOST slot.
               MenuManager::instance().registerBlobApp(
                   m.category, m.label, m.blobPath);
               continue;
           }
           addAppToMenu(appDefs[m.appIndex].name, m.category.c_str(), m.appIndex);
       }
       return;
   }

   // Fallback: no/unreadable manifest => compiled-in order (today's
   // behavior, nested categoryPaths and all).
   ESP_LOGI("AppDefs", "No loadout manifest; menu uses compiled-in order");
   for (int i=0; i<(int)APP_COUNT; i++){
       ESP_LOGI("AppDefs","i=%d name=%s path=%s beginFunc? %s",
                i, appDefs[i].name, appDefs[i].categoryPath,
                (appDefs[i].beginFunc ? "YES":"NO"));
       addAppToMenu(appDefs[i].name, appDefs[i].categoryPath, i);
   }
}
