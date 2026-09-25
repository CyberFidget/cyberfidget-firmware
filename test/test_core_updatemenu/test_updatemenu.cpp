// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_core_updatemenu/test_updatemenu.cpp
//
// Where the update surfaces sit in the menu (root "Check for updates",
// Settings > Updates, next to Link), the Settings > Updates rows and their
// labels, and the prompt option indexes the device glue relies on. The
// menu rows come from the real AppManifest.h, expanded here with a macro
// that keeps only the label and category path.

#include <unity.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../lib/MenuManager/CategoryPath.h"
#include "../../lib/MenuManager/MenuIdentity.h"
#include "AwakePolicy.h"
#include "PromptPolicy.h"

using namespace PromptPolicy;

namespace {
struct ManifestRow {
    const char* id;
    const char* label;
    const char* path;
};

const ManifestRow kRows[] = {
#define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) {#ID, LABEL, CATPATH},
#include "../../lib/AppDefs/AppManifest.h"
#undef APP_ENTRY
};

const ManifestRow* find(const char* id) {
    for (const ManifestRow& r : kRows) {
        if (strcmp(r.id, id) == 0) return &r;
    }
    return nullptr;
}

void labels(const SettingsState& s, std::vector<std::string>& out) {
    out.clear();
    char line[kRowText];
    for (int i = 0; i < kSettingsRows; i++) {
        settingsLabel(settingsRow(i), s, line, sizeof(line));
        out.push_back(line);
    }
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_awake_screen_sits_in_settings(void) {
    const ManifestRow* awake = find("APP_AWAKE");
    const ManifestRow* updates = find("APP_UPDATES");
    TEST_ASSERT_NOT_NULL(awake);
    TEST_ASSERT_EQUAL_STRING("Awake & dev mode", awake->label);
    TEST_ASSERT_EQUAL_STRING(updates->path, awake->path);
    // Only the Music Player uses Bluetooth today (the marker and the
    // restart prompt key on it).
    TEST_ASSERT_NOT_NULL(find("APP_MUSIC_PLAYER"));
}

// Every name on dev mode's listening allow-list is a real app entry (a
// renamed entry would silently pause listening).
void test_listening_allow_list_names_real_apps(void) {
    int allowed = 0;
    for (const ManifestRow& r : kRows) if (AwakePolicy::listensDuring(r.id)) allowed++;
    TEST_ASSERT_EQUAL_INT(8, allowed);
    TEST_ASSERT_FALSE(AwakePolicy::listensDuring("APP_VOICE_RECORDER"));
    TEST_ASSERT_NOT_NULL(find("APP_VOICE_RECORDER"));
}

void test_check_for_updates_is_a_root_item(void) {
    const ManifestRow* r = find("APP_CHECK_UPDATES");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("Check for updates", r->label);
    TEST_ASSERT_EQUAL_UINT32(0, splitCategoryPath(r->path).size());
}

void test_updates_sits_in_settings_next_to_link(void) {
    const ManifestRow* updates = find("APP_UPDATES");
    const ManifestRow* link = find("APP_LINK");
    TEST_ASSERT_NOT_NULL(updates);
    TEST_ASSERT_NOT_NULL(link);
    TEST_ASSERT_EQUAL_STRING("Updates", updates->label);
    const std::vector<std::string> path = splitCategoryPath(updates->path);
    TEST_ASSERT_EQUAL_UINT32(1, path.size());
    TEST_ASSERT_EQUAL_STRING("Settings", path[0].c_str());
    TEST_ASSERT_EQUAL_STRING(link->path, updates->path);
}

void test_settings_rows_fresh_device(void) {
    SettingsState s;
    s.running = "1.2.2+8d08668";
    std::vector<std::string> l;
    labels(s, l);
    const char* want[kSettingsRows] = {
        "Check now",
        "Auto-check: On",
        "Check at start-up: On",
        "Apply app changes automatically: On",
        "Channel: stable",
        "Source: cyberfidget.com",
        "Skip: no update waiting",
        "Link this Fidget",
        "Awake & dev mode: Off",
        "Status: nothing waiting",
    };
    for (int i = 0; i < kSettingsRows; i++) TEST_ASSERT_EQUAL_STRING(want[i], l[i].c_str());
}

void test_settings_rows_follow_state(void) {
    SettingsState s;
    s.running = "1.2.2";
    s.avail = "1.4.0";
    s.linked = true;
    s.autoapply = false;
    s.awake.mode = AwakePolicy::Mode::Dev;
    s.awake.stop = AwakePolicy::Stop::UntilStopped;
    s.status = "Update 1.4.0 ready";
    std::vector<std::string> l;
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Apply app changes automatically: Off", l[3].c_str());
    TEST_ASSERT_EQUAL_STRING("Skip 1.4.0", l[6].c_str());
    TEST_ASSERT_EQUAL_STRING("Unlink this Fidget", l[7].c_str());
    TEST_ASSERT_EQUAL_STRING("Awake & dev mode: Dev mode", l[8].c_str());
    TEST_ASSERT_EQUAL_STRING("Status: Update 1.4.0 ready", l[9].c_str());
    TEST_ASSERT_EQUAL_INT((int)SkipAction::Skip, (int)skipAction(s));

    s.rej = "1.4.0";   // skipped: the row undoes it
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Unskip 1.4.0", l[6].c_str());
    TEST_ASSERT_EQUAL_INT((int)SkipAction::Unskip, (int)skipAction(s));

    // A newer offer arrives while 1.4.0 is skipped: the row skips the new
    // version (replacing the old skip), it does not offer to unskip 1.4.0.
    s.avail = "1.5.0";
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Skip 1.5.0", l[6].c_str());
    TEST_ASSERT_EQUAL_INT((int)SkipAction::Skip, (int)skipAction(s));
    // Nothing offered at all: the old skip can still be undone.
    s.avail = "";
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Unskip 1.4.0", l[6].c_str());
    s.avail = "1.4.0";

    s.policy = CheckinPolicy::Policy::Never;
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Auto-check: Off", l[1].c_str());
    TEST_ASSERT_EQUAL_STRING(kOffExplanation, l[9].c_str());
    s.bootCheck = false;
    labels(s, l);
    TEST_ASSERT_EQUAL_STRING("Check at start-up: Off", l[2].c_str());
}

// The menu's in-place rebuild keeps the highlight on "the same item"
// (MenuIdentity.h, used by MenuManager::rebuildInPlace). A root list with
// two delivered apps of the same name, rebuilt with a new app delivered in
// front and the second one replaced by a new file.
namespace {
struct Item {
    std::string label;
    bool isCategory;
    int appIndex;
    std::string blobPath;
    std::string blobId;
};
}

void test_rebuild_keeps_selection_with_duplicate_labels(void) {
    const std::vector<Item> before = {
        {"Games", true, -1, "", ""},
        {"Timer", false, 50, "/apps/timer-a-00000001.wasm", "timer-a"},
        {"Timer", false, 50, "/apps/timer-b-00000002.wasm", "timer-b"},
        {"Clock", false, 8, "", ""},
    };
    const std::vector<Item> after = {
        {"Games", true, -1, "", ""},
        {"New app", false, 50, "/apps/new-00000003.wasm", "new"},
        {"Timer", false, 50, "/apps/timer-a-00000001.wasm", "timer-a"},
        {"Timer", false, 50, "/apps/timer-b-0000000f.wasm", "timer-b"},
        {"Clock", false, 8, "", ""},
    };
    // The second Timer stays selected, although its file changed and the
    // first Timer carries the same name.
    TEST_ASSERT_EQUAL_INT(3, findSameMenuItem(after, before[2]));
    TEST_ASSERT_EQUAL_INT(2, findSameMenuItem(after, before[1]));
    // A built-in app by its index, a category by its label.
    TEST_ASSERT_EQUAL_INT(4, findSameMenuItem(after, before[3]));
    TEST_ASSERT_EQUAL_INT(0, findSameMenuItem(after, before[0]));
    // A removed app is not found (the menu then goes to the top).
    const std::vector<Item> removed = {before[0], before[1], before[3]};
    TEST_ASSERT_EQUAL_INT(-1, findSameMenuItem(removed, before[2]));
    // A built-in never matches a delivered app with the same name.
    const Item builtinTimer = {"Timer", false, 50, "", ""};
    TEST_ASSERT_EQUAL_INT(-1, findSameMenuItem(before, builtinTimer));
}

void test_popup_choice_indexes(void) {
    // ModalPrompt reports the zero-based option; the effects key on these.
    TEST_ASSERT_EQUAL_INT(0, (int)FwChoice::Install);
    TEST_ASSERT_EQUAL_INT(1, (int)FwChoice::Later);
    TEST_ASSERT_EQUAL_INT(2, (int)FwChoice::Skip);
    TEST_ASSERT_EQUAL_INT(-1, (int)FwChoice::None);
    TEST_ASSERT_TRUE(firmwareChoice(2, false).writeRej);
    TEST_ASSERT_FALSE(firmwareChoice(1, false).writeRej);
    TEST_ASSERT_FALSE(firmwareChoice(0, false).writeRej);
}

void test_app_change_prompt(void) {
    char title[40];
    appsTitle(title, sizeof(title), 0);
    TEST_ASSERT_EQUAL_STRING("App changes waiting", title);
    TEST_ASSERT_EQUAL_STRING("Get them now", kAppOptions[0]);
    TEST_ASSERT_EQUAL_STRING("Later", kAppOptions[1]);
    TEST_ASSERT_TRUE(appChoice(0).applyNow);
    TEST_ASSERT_TRUE(appChoice(1).keepInBar);
    // Waiting app changes prompt after the start-up animation and after a
    // manual check, independent of any firmware offer.
    TEST_ASSERT_TRUE(bootPlan(CheckinPolicy::Policy::Auto, false, true).apps);
    TEST_ASSERT_TRUE(manualPlan(CheckinPolicy::Policy::Auto, false, true).apps);
}

void test_user_copy_has_no_jargon(void) {
    // Every string the update surfaces put on screen.
    SettingsState s;
    s.running = "1.0.0";
    s.avail = "1.1.0";
    std::vector<std::string> shown;
    labels(s, shown);
    s.policy = CheckinPolicy::Policy::Never;
    s.linked = true;
    std::vector<std::string> more;
    labels(s, more);
    shown.insert(shown.end(), more.begin(), more.end());
    for (const char* t : kFwOptions) shown.push_back(t);
    for (const char* t : kAppOptions) shown.push_back(t);
    shown.push_back(kOffExplanation);
    shown.push_back(kBootCheckExplanation);
    shown.push_back(kRestartingToCheck);
    shown.push_back(kChecking);
    shown.push_back(kInstallComingSoon);
    const char* banned[] = {"WiFi", "Wi-Fi", "OTA", "token", "server", "firmware", "image"};
    for (const std::string& t : shown) {
        for (const char* b : banned) {
            TEST_ASSERT_NULL_MESSAGE(strstr(t.c_str(), b), t.c_str());
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_check_for_updates_is_a_root_item);
    RUN_TEST(test_awake_screen_sits_in_settings);
    RUN_TEST(test_listening_allow_list_names_real_apps);
    RUN_TEST(test_updates_sits_in_settings_next_to_link);
    RUN_TEST(test_settings_rows_fresh_device);
    RUN_TEST(test_settings_rows_follow_state);
    RUN_TEST(test_rebuild_keeps_selection_with_duplicate_labels);
    RUN_TEST(test_popup_choice_indexes);
    RUN_TEST(test_app_change_prompt);
    RUN_TEST(test_user_copy_has_no_jargon);
    return UNITY_END();
}
