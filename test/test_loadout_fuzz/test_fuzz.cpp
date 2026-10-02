// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_loadout_fuzz/test_fuzz.cpp
//
// Property tests for the arrange op against a reference model of what the
// device DISPLAYS: the real mergeWithRegistry, then the nested menu built
// the way buildNestedMenu / registerApp / findOrCreateCategory build it
// (hidden rows skipped; at each level a folder is created where its first
// row is registered; apps are appended in order).
//
// Random manifests (fixed seed) mix nested and flat categories, "" categories,
// hidden rows, retired ids, duplicate built-in ids, delivered apps and
// compiled apps the file omits (the merge appends those). Each
// case moves ONE shown app and checks the displayed menu changed only for
// that app. A failure is shrunk (rows removed while it still fails) and
// printed.

#include <unity.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

static const uint64_t kSeed = 20261002ULL;
static const int kSingleCases = 4000; // single-item arrange [app (+category)]
static const int kMoveCases   = 4000; // full-order arrange after a menu move
static const int kFlatCases   = 2000; // old flat files vs the old rule

// ---------- deterministic generator ----------

struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 11);
    }
    int below(int n) { return n <= 0 ? 0 : (int)(next() % (uint32_t)n); }
    bool chance(int pct) { return below(100) < pct; }
};

// ---------- registry ----------

static const RegistryApp kReg[] = {
    { "",              "",              "",                 "" }, // menu slot
    { "flashlight",    "Flashlight",    "Tools/LEDs",       "" },
    { "power-manager", "Power Manager", "Tools",            "" },
    { "clock",         "Clock",         "Tools",            "" },
    { "accelerometer", "Accelerometer", "Tools/LEDs",       "" },
    { "booper",        "Booper",        "Games",            "" },
    { "snake",         "Snake",         "Games/Arcade",     "" },
    { "music",         "Music",         "Media",            "" },
    { "status",        "Status",        "",                 "" },
    { "check",         "Check",         "",                 "" },
    { "link",          "Link",          "Settings",         "" },
    { "fancy",         "Fancy",         "Tools/LEDs/Fancy", "" },
};
static const int kRegCount = (int)(sizeof(kReg) / sizeof(kReg[0]));

static const char* const kPool[] = {
    "", "Tools", "Tools/LEDs", "Tools/Other", "Games", "Games/Arcade",
    "Media", "Settings", "Extra/Sub", "Tools/LEDs/Fancy",
};
static const int kPoolCount = (int)(sizeof(kPool) / sizeof(kPool[0]));

static std::string topSeg(const std::string& p) { return p.substr(0, p.find('/')); }

static int regIndex(const std::string& id) {
    for (int i = 0; i < kRegCount; i++) if (kReg[i].id == id) return i;
    return -1;
}

static bool isBuiltin(const LoadoutEntry& e) {
    return e.format == "builtin" || e.format.empty();
}

// ---------- display model ----------

struct Node {
    std::string name;   // folder label, or leaf key
    bool folder = false;
    std::vector<Node> kids;
};

// Same rule as MenuManager's CategoryPath.h: split on '/', skip empties.
static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        if (slash > start) out.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

static Node* findFolder(Node& level, const std::string& name) {
    for (auto& k : level.kids) if (k.folder && k.name == name) return &k;
    return nullptr;
}

static Node* findOrCreateFolder(Node& level, const std::string& name) {
    Node* f = findFolder(level, name);
    if (f) return f;
    Node n; n.name = name; n.folder = true;
    level.kids.push_back(n);
    return &level.kids.back();
}

static std::string leafKey(const MergedApp& m) {
    if (m.appIndex >= 0) return "b:" + kReg[m.appIndex].id;
    return "w:" + m.id + "|" + m.blobPath;
}

static std::string keyToId(const std::string& key) {
    std::string rest = key.substr(2);
    return rest.substr(0, rest.find('|'));
}

static Node display(const Loadout& l) {
    Node root; root.folder = true;
    for (const auto& m : mergeWithRegistry(l, kReg, kRegCount)) {
        if (m.hidden) continue;
        Node* level = &root;
        for (const auto& seg : splitPath(m.category)) level = findOrCreateFolder(*level, seg);
        Node leaf; leaf.name = leafKey(m);
        level->kids.push_back(leaf);
    }
    return root;
}

static void serialize(const Node& n, std::string& out) {
    for (size_t i = 0; i < n.kids.size(); i++) {
        if (i) out += ",";
        const Node& k = n.kids[i];
        out += k.name;
        if (k.folder) { out += "{"; serialize(k, out); out += "}"; }
    }
}
static std::string show(const Node& n) { std::string s; serialize(n, s); return s; }

static bool removeLeaf(Node& n, const std::string& key) {
    for (size_t i = 0; i < n.kids.size(); i++) {
        if (!n.kids[i].folder && n.kids[i].name == key) {
            n.kids.erase(n.kids.begin() + (long)i);
            return true;
        }
        if (n.kids[i].folder && removeLeaf(n.kids[i], key)) {
            if (n.kids[i].kids.empty()) n.kids.erase(n.kids.begin() + (long)i);
            return true;
        }
    }
    return false;
}

static void collectLeaves(const Node& n, const std::string& path,
                          std::vector<std::pair<std::string, std::string>>& out) {
    for (const auto& k : n.kids) {
        if (k.folder) collectLeaves(k, path.empty() ? k.name : path + "/" + k.name, out);
        else out.push_back({ k.name, path });
    }
}

static void collectFolders(const Node& n, const std::string& path,
                           std::vector<std::string>& out) {
    out.push_back(path);
    for (const auto& k : n.kids) {
        if (k.folder) collectFolders(k, path.empty() ? k.name : path + "/" + k.name, out);
    }
}

static Node* folderAt(Node& root, const std::string& path) {
    Node* level = &root;
    for (const auto& seg : splitPath(path)) {
        level = findFolder(*level, seg);
        if (!level) return nullptr;
    }
    return level;
}

// The shown apps: (leaf key, first manifest row id).
static std::vector<std::string> shownKeys(const Loadout& l) {
    std::vector<std::pair<std::string, std::string>> leaves;
    collectLeaves(display(l), "", leaves);
    std::vector<std::string> keys;
    for (const auto& p : leaves) keys.push_back(p.first);
    return keys;
}

// ---------- manifest generator ----------

static LoadoutEntry row(const std::string& id, const std::string& cat, bool hidden,
                        const char* format) {
    LoadoutEntry e;
    e.id = id; e.name = id; e.category = cat; e.hidden = hidden; e.format = format;
    return e;
}

static std::string randomCategory(Rng& r, const std::string& compiled) {
    int p = r.below(100);
    if (p < 40) return compiled;
    if (p < 60) return topSeg(compiled);
    if (p < 75) return "";
    return kPool[r.below(kPoolCount)];
}

static Loadout genManifest(Rng& r) {
    Loadout l;
    for (int i = 0; i < kRegCount; i++) {
        if (kReg[i].name.empty()) continue;
        int copies = r.chance(15) ? 0 : r.chance(15) ? 2 : 1; // 0 = omitted
        for (int c = 0; c < copies; c++) {
            l.entries.push_back(row(kReg[i].id, randomCategory(r, kReg[i].category),
                                    r.chance(15), r.chance(50) ? "builtin" : ""));
        }
    }
    int retired = r.below(3);
    for (int i = 0; i < retired; i++) {
        l.entries.push_back(row("retired-" + std::to_string(i), kPool[r.below(kPoolCount)],
                                r.chance(20), "builtin"));
    }
    int blobs = r.below(4);
    for (int i = 0; i < blobs; i++) {
        LoadoutEntry e = row("w-" + std::to_string(i), kPool[r.below(kPoolCount)],
                             r.chance(15), "wasm");
        if (r.chance(85)) e.blobPath = "/apps/w-" + std::to_string(i) + ".wasm";
        l.entries.push_back(e);
    }
    for (int i = (int)l.entries.size() - 1; i > 0; i--) {
        std::swap(l.entries[(size_t)i], l.entries[(size_t)r.below(i + 1)]);
    }
    return l;
}

static std::string describe(const Loadout& l) {
    std::string s;
    int parked = 0;
    for (const auto& e : l.entries) {
        if (e.category == "Zz" && e.hidden) { parked++; continue; }
        s += "\n    " + e.id + " [" + e.category + "]";
        if (e.hidden) s += " hidden";
        if (!isBuiltin(e)) s += e.blobPath.empty() ? " wasm(no file)" : " wasm";
    }
    if (parked) s += "\n    (+" + std::to_string(parked) + " other apps listed hidden)";
    return s;
}

// ---------- one case ----------

// A single-app change and its expected display.
struct Change {
    bool single = true;      // true: arrange [id (+category)]; false: full move
    std::string key;         // the app's leaf key
    bool hasCategory = false;
    std::string category;    // single: the new category
    std::string target;      // move: destination folder path ("" = root)
    bool newFolder = false;  // move: create `newName` under `target`
    std::string newName;
    int index = 0;           // move: position among the destination's children
};

enum Outcome { kInvalid, kPass, kFail };

static Outcome runCase(const Loadout& file, const Change& ch,
                       std::string* expectedOut, std::string* gotOut) {
    Node before = display(file);
    std::vector<std::pair<std::string, std::string>> leaves;
    collectLeaves(before, "", leaves);
    bool present = false;
    for (const auto& p : leaves) if (p.first == ch.key) present = true;
    if (!present) return kInvalid;
    const std::string id = keyToId(ch.key);
    const bool builtin = ch.key[0] == 'b';

    Node expected = before;
    removeLeaf(expected, ch.key);
    Node leaf; leaf.name = ch.key;
    std::vector<ArrangeItem> order;

    if (ch.single) {
        // Category after the arrange, as the merge renders it.
        const LoadoutEntry* first = nullptr;
        for (const auto& e : file.entries) {
            if (e.id == id) { first = &e; break; }
        }
        // An app the file does not list is written in by the arrange with
        // its first-segment category, as the merge shows it.
        std::string saved = ch.hasCategory ? ch.category
                          : first ? first->category
                                  : topSeg(kReg[regIndex(id)].category);
        std::string path = saved;
        if (builtin && saved.empty()) path = topSeg(kReg[regIndex(id)].category);
        // The app goes first: each folder on its path moves to the front of
        // its parent, and the app to the front of its folder.
        Node* level = &expected;
        for (const auto& seg : splitPath(path)) {
            Node* f = findFolder(*level, seg);
            Node moved;
            if (f) { moved = *f; level->kids.erase(level->kids.begin() + (f - &level->kids[0])); }
            else   { moved.name = seg; moved.folder = true; }
            level->kids.insert(level->kids.begin(), moved);
            level = &level->kids[0];
        }
        level->kids.insert(level->kids.begin(), leaf);
        ArrangeItem it; it.id = id; it.hasCategory = ch.hasCategory; it.category = ch.category;
        order.push_back(it);
    } else {
        Node* dest = folderAt(expected, ch.target);
        if (!dest) return kInvalid;
        if (ch.newFolder) {
            if (findFolder(*dest, ch.newName)) return kInvalid;
            Node f; f.name = ch.newName; f.folder = true; f.kids.push_back(leaf);
            int k = std::min(ch.index, (int)dest->kids.size());
            dest->kids.insert(dest->kids.begin() + k, f);
        } else {
            // A built-in cannot be stored at the root unless it is compiled
            // there ("" renders as its compiled top level).
            if (builtin && ch.target.empty() && !topSeg(kReg[regIndex(id)].category).empty())
                return kInvalid;
            int k = std::min(ch.index, (int)dest->kids.size());
            dest->kids.insert(dest->kids.begin() + k, leaf);
        }
        std::vector<std::pair<std::string, std::string>> newLeaves;
        collectLeaves(expected, "", newLeaves);
        for (const auto& p : newLeaves) {
            ArrangeItem it; it.id = keyToId(p.first); it.category = p.second; it.hasCategory = true;
            order.push_back(it);
        }
    }

    Loadout work = file;
    TEST_ASSERT_TRUE(applyArrange(work, order, kReg, kRegCount));
    const std::string want = show(expected);
    const std::string got  = show(display(work));
    if (expectedOut) *expectedOut = want;
    if (gotOut) *gotOut = got;
    return want == got ? kPass : kFail;
}

static void failShrunk(const char* what, int caseNo, Loadout file, const Change& ch) {
    // Greedy shrink: drop rows while the case still fails.
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < file.entries.size() && !progress; i++) {
            Loadout smaller = file;
            smaller.entries.erase(smaller.entries.begin() + (long)i);
            if (runCase(smaller, ch, nullptr, nullptr) == kFail) {
                file = smaller;
                progress = true;
                break;
            }
            // Or park the row hidden out of the way (keeps the app listed).
            const LoadoutEntry& e = file.entries[i];
            if (e.category == "Zz" && e.hidden) continue;
            Loadout parked = file;
            parked.entries[i] = row(e.id, "Zz", true, "builtin");
            if (runCase(parked, ch, nullptr, nullptr) == kFail) {
                file = parked;
                progress = true;
            }
        }
    }
    std::string want, got;
    runCase(file, ch, &want, &got);
    std::string msg = std::string(what) + " case " + std::to_string(caseNo) +
        " (seed " + std::to_string(kSeed) + ") minimal manifest:" + describe(file) +
        "\n  change: " + (ch.single ? "arrange [" : "move ") + keyToId(ch.key) +
        (ch.single ? (ch.hasCategory ? " -> [" + ch.category + "]]" : "]")
                   : " -> " + (ch.target.empty() ? std::string("(root)") : ch.target) +
                     (ch.newFolder ? "/" + ch.newName : "") + " @" + std::to_string(ch.index)) +
        "\n  shown before: " + show(display(file)) +
        "\n  expected:     " + want + "\n  got:          " + got;
    TEST_FAIL_MESSAGE(msg.c_str());
}

// ---------- the properties ----------

// Single-item arrange: only the named app moves (to the front, under its
// new or current category); every other app and folder keeps its parent
// and relative order; no app appears or disappears.
void test_fuzz_single_item_arrange(void) {
    Rng r{ kSeed };
    int ran = 0;
    for (int c = 0; c < kSingleCases; c++) {
        Loadout file = genManifest(r);
        std::vector<std::pair<std::string, std::string>> leaves;
        collectLeaves(display(file), "", leaves);
        if (leaves.empty()) continue;
        Change ch;
        ch.single = true;
        ch.key = leaves[(size_t)r.below((int)leaves.size())].first;
        if (r.chance(50)) {
            ch.hasCategory = true;
            ch.category = r.chance(80) ? std::string(kPool[r.below(kPoolCount)])
                                       : std::string("Tools/Fresh");
        }
        Outcome o = runCase(file, ch, nullptr, nullptr);
        if (o == kFail) failShrunk("single-item arrange", c, file, ch);
        if (o == kPass) ran++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "single-item arrange: %d cases passed (seed %llu)",
             ran, (unsigned long long)kSeed);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(ran > kSingleCases / 2);
}

// Full-order arrange after moving one app anywhere in the displayed menu
// (another folder, a new folder, any position): the menu afterwards is
// exactly the moved-to menu.
void test_fuzz_menu_move(void) {
    Rng r{ kSeed ^ 0x9E3779B97F4A7C15ULL };
    int ran = 0;
    for (int c = 0; c < kMoveCases; c++) {
        Loadout file = genManifest(r);
        Node before = display(file);
        std::vector<std::pair<std::string, std::string>> leaves;
        collectLeaves(before, "", leaves);
        if (leaves.empty()) continue;
        Change ch;
        ch.single = false;
        ch.key = leaves[(size_t)r.below((int)leaves.size())].first;
        Node rest = before;
        removeLeaf(rest, ch.key);
        std::vector<std::string> folders;
        collectFolders(rest, "", folders);
        ch.target = folders[(size_t)r.below((int)folders.size())];
        ch.newFolder = r.chance(25);
        ch.newName = "Fresh";
        Node* dest = folderAt(rest, ch.target);
        ch.index = r.below((int)dest->kids.size() + 1);
        Outcome o = runCase(file, ch, nullptr, nullptr);
        if (o == kFail) failShrunk("menu move", c, file, ch);
        if (o == kPass) ran++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "menu move: %d cases passed (seed %llu)",
             ran, (unsigned long long)kSeed);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(ran > kMoveCases / 2);
}

// The earlier rule, as released firmware applies it: pull the named items
// to the front, append the rest, then group each exact category string
// into one run by first appearance (hidden rows included).
static std::vector<std::string> oldArrange(const Loadout& l, const ArrangeItem& item) {
    std::vector<LoadoutEntry> in;
    for (const auto& e : l.entries) {
        if (e.id == item.id) {
            LoadoutEntry x = e;
            if (item.hasCategory) x.category = item.category;
            in.insert(in.begin(), x);
        } else {
            in.push_back(e);
        }
    }
    std::vector<std::string> ids;
    std::vector<bool> placed(in.size(), false);
    for (size_t i = 0; i < in.size(); i++) {
        if (placed[i]) continue;
        for (size_t j = i; j < in.size(); j++) {
            if (!placed[j] && in[j].category == in[i].category) {
                placed[j] = true;
                ids.push_back(in[j].id);
            }
        }
    }
    return ids;
}

// A file earlier firmware saved: one row per app, single-level categories
// (its flattened compiled category, sometimes another flat one), some
// hidden, already grouped by its own arrange. Arranging a non-root app
// gives exactly the old result.
void test_fuzz_old_flat_files_match_old_rule(void) {
    static const char* const kFlat[] = { "Tools", "Games", "Media", "Settings", "Extra" };
    Rng r{ kSeed + 7 };
    int ran = 0;
    for (int c = 0; c < kFlatCases; c++) {
        Loadout file;
        for (int i = 0; i < kRegCount; i++) {
            if (kReg[i].name.empty()) continue;
            std::string cat = topSeg(kReg[i].category);
            if (!cat.empty() && r.chance(30)) cat = kFlat[r.below(5)];
            file.entries.push_back(row(kReg[i].id, cat, r.chance(20), "builtin"));
        }
        for (int i = (int)file.entries.size() - 1; i > 0; i--) {
            std::swap(file.entries[(size_t)i], file.entries[(size_t)r.below(i + 1)]);
        }
        // Grouped as the old firmware's own arrange left it.
        ArrangeItem none; none.id = "-";
        std::vector<std::string> grouped = oldArrange(file, none);
        Loadout saved;
        for (const auto& id : grouped) {
            for (const auto& e : file.entries) if (e.id == id) saved.entries.push_back(e);
        }
        // Pick a shown, non-root app.
        std::vector<std::string> candidates;
        for (const auto& e : saved.entries) {
            if (!e.hidden && !e.category.empty()) candidates.push_back(e.id);
        }
        if (candidates.empty()) continue;
        ArrangeItem it;
        it.id = candidates[(size_t)r.below((int)candidates.size())];
        if (r.chance(30)) { it.hasCategory = true; it.category = kFlat[r.below(5)]; }

        std::vector<std::string> want = oldArrange(saved, it);
        Loadout work = saved;
        std::vector<ArrangeItem> order = { it };
        TEST_ASSERT_TRUE(applyArrange(work, order, kReg, kRegCount));
        bool same = want.size() == work.entries.size();
        for (size_t i = 0; same && i < want.size(); i++) same = want[i] == work.entries[i].id;
        if (!same) {
            std::string w, g;
            for (const auto& s : want) w += s + " ";
            for (const auto& e : work.entries) g += e.id + " ";
            std::string msg = "old flat case " + std::to_string(c) + ":" + describe(saved) +
                "\n  arrange " + it.id + (it.hasCategory ? " -> [" + it.category + "]" : "") +
                "\n  old: " + w + "\n  new: " + g;
            TEST_FAIL_MESSAGE(msg.c_str());
        }
        ran++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "old flat files: %d cases matched (seed %llu)",
             ran, (unsigned long long)kSeed);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(ran > kFlatCases / 2);
}

// ---------- the review's two scenarios, as fixed regression cases ----------

// List every registry app the case does not mention, hidden, so the merge
// does not append it and the display holds only the case's apps.
static void fillHidden(Loadout& l) {
    for (int i = 0; i < kRegCount; i++) {
        if (kReg[i].name.empty()) continue;
        bool listed = false;
        for (const auto& e : l.entries) if (e.id == kReg[i].id) listed = true;
        if (!listed) l.entries.push_back(row(kReg[i].id, "Zz", true, "builtin"));
    }
}

static void expectDisplay(const Loadout& l, const char* want) {
    TEST_ASSERT_EQUAL_STRING(want, show(display(l)).c_str());
}

// Clock's "" category renders under Tools (flat fallback). Arranging only
// Booper must leave Tools as LEDs, Clock, Power Manager.
void test_rendered_category_orders_rows(void) {
    Loadout l;
    l.entries.push_back(row("flashlight",    "Tools/LEDs", false, "builtin"));
    l.entries.push_back(row("clock",         "",           false, "builtin"));
    l.entries.push_back(row("power-manager", "Tools",      false, "builtin"));
    l.entries.push_back(row("booper",        "Games",      false, "builtin"));
    fillHidden(l);
    expectDisplay(l, "Tools{LEDs{b:flashlight},b:clock,b:power-manager},Games{b:booper}");
    std::vector<ArrangeItem> order(1);
    order[0].id = "booper";
    TEST_ASSERT_TRUE(applyArrange(l, order, kReg, kRegCount));
    expectDisplay(l, "Games{b:booper},Tools{LEDs{b:flashlight},b:clock,b:power-manager}");
}

// A retired row under Tools/LEDs must not anchor Tools. The device shows
// Games, Tools, Media; arranging Music gives Media, Games, Tools. Same
// with a hidden Flashlight in its place.
void test_unshown_rows_do_not_anchor_top_level(void) {
    for (int hiddenFlash = 0; hiddenFlash < 2; hiddenFlash++) {
        Loadout l;
        if (hiddenFlash) l.entries.push_back(row("flashlight", "Tools/LEDs", true, "builtin"));
        else             l.entries.push_back(row("retired-led", "Tools/LEDs", false, "builtin"));
        l.entries.push_back(row("snake",         "Games",       false, "builtin"));
        l.entries.push_back(row("power-manager", "Tools/Other", false, "builtin"));
        l.entries.push_back(row("music",         "Media",       false, "builtin"));
        fillHidden(l);
        expectDisplay(l, "Games{b:snake},Tools{Other{b:power-manager}},Media{b:music}");
        std::vector<ArrangeItem> order(1);
        order[0].id = "music";
        TEST_ASSERT_TRUE(applyArrange(l, order, kReg, kRegCount));
        expectDisplay(l, "Media{b:music},Games{b:snake},Tools{Other{b:power-manager}}");
    }
}

// A compiled app the file omits (Clock) is appended by the merge under
// Tools. Arranging only Flashlight into Media must not swap the unnamed
// Tools and Games folders.
void test_unlisted_compiled_app_keeps_place(void) {
    Loadout l;
    l.entries.push_back(row("flashlight", "Tools/LEDs", false, "builtin"));
    l.entries.push_back(row("booper",     "Games",      false, "builtin"));
    for (int i = 0; i < kRegCount; i++) {
        if (kReg[i].name.empty() || kReg[i].id == "clock") continue;
        bool listed = false;
        for (const auto& e : l.entries) if (e.id == kReg[i].id) listed = true;
        if (!listed) l.entries.push_back(row(kReg[i].id, "Zz", true, "builtin"));
    }
    expectDisplay(l, "Tools{LEDs{b:flashlight},b:clock},Games{b:booper}");
    std::vector<ArrangeItem> order(1);
    order[0].id = "flashlight";
    order[0].hasCategory = true;
    order[0].category = "Media";
    TEST_ASSERT_TRUE(applyArrange(l, order, kReg, kRegCount));
    expectDisplay(l, "Media{b:flashlight},Tools{b:clock},Games{b:booper}");
    // Clock is now written in the file, as the merge showed it.
    bool clockRow = false;
    for (const auto& e : l.entries) {
        if (e.id == "clock") {
            clockRow = true;
            TEST_ASSERT_EQUAL_STRING("Tools", e.category.c_str());
            TEST_ASSERT_EQUAL_STRING("builtin", e.format.c_str());
            TEST_ASSERT_FALSE(e.hidden);
        }
    }
    TEST_ASSERT_TRUE(clockRow);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_rendered_category_orders_rows);
    RUN_TEST(test_unshown_rows_do_not_anchor_top_level);
    RUN_TEST(test_unlisted_compiled_app_keeps_place);
    RUN_TEST(test_fuzz_single_item_arrange);
    RUN_TEST(test_fuzz_menu_move);
    RUN_TEST(test_fuzz_old_flat_files_match_old_rule);
    return UNITY_END();
}
