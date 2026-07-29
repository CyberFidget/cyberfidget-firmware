// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef APP_DEFS_H
#define APP_DEFS_H

#include <functional>
#include <vector>
#include <string>

#include "LoadoutManifest.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/**
 * @brief The function pointer types for an app's lifecycle
 */
typedef void (*AppBeginFunc)();
typedef void (*AppEndFunc)();
typedef void (*AppRunFunc)();

struct AppDefinition {
    const char*   name;          // "Menu", "Booper", etc.
    const char*   categoryPath;  // "", "Tools", etc.
    AppBeginFunc  beginFunc;
    AppEndFunc    endFunc;
    AppRunFunc    runFunc;
};

/**
 * We'll generate the enum from the lines in AppManifest.h
 */
enum AppIndex {
    #define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) ID,
    #include "AppManifest.h"
    #undef APP_ENTRY

    APP_COUNT  // final sentinel
};

/**
 * Print APP_COUNT as a compiler message
 */
// #pragma message ("APP_COUNT is " STR(APP_COUNT))

/**
 * Ensure APP_COUNT is properly defined and greater than zero
 */
static_assert(APP_COUNT > 0, "Error: APP_COUNT is zero or not properly defined.");

/**
 * We'll also declare a global array appDefs, 
 * each entry has (name, categoryPath, beginFunc, endFunc, runFunc)
 */
extern const AppDefinition appDefs[APP_COUNT];

/**
 * Legacy APP_ENTRY enum strings retained only for exact migration of
 * manifests written by older firmware. Parallel to appDefs[].
 */
extern const char* const appIds[APP_COUNT];

/**
 * Adapt appDefs[]/appIds[] into the minimal registry view the pure
 * LoadoutManifest core consumes (categories flattened to one level).
 */
std::vector<LoadoutManifest::RegistryApp> buildLoadoutRegistryView();
bool loadLoadoutManifest(LoadoutManifest::Loadout& loadout,
                         std::string* jsonOut = nullptr);

/**
 * We'll define a function to build the nested menu from these definitions.
 * We'll show an example of how to parse the categoryPath into subcategories.
 */
void buildNestedMenu();

/**
 * Ensure APP_COUNT is properly defined and greater than zero
 */
static_assert(APP_COUNT > 0, "Error: APP_COUNT is zero or not properly defined.");

/**
 * Force compiler to print APP_COUNT's value in case of an issue
 * This template will instantiate only if APP_COUNT is valid.
 */
template<int N>
struct PrintAppCount {
    static constexpr int value = N;
};

// This line forces the compiler to process APP_COUNT as a template argument
extern PrintAppCount<APP_COUNT> appCountPrinter;

void printAppCount();
void debugAppDefs();

#endif // APP_DEFS_H
