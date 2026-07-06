// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * LoadoutManifest — pure (Arduino-free) core for the loadout manifest.
 *
 * The manifest (/loadout.json on LittleFS, see LoadoutStore) is the
 * data-driven app registry: it records menu order, a single-level flat
 * category per app, and a hidden flag. This header is deliberately free
 * of Arduino / ESP-IDF dependencies so the same code compiles for
 * native unit tests (pio test -e test_loadout) and the WASM emulator.
 *
 * Schema and merge/apply semantics are documented in README.md next to
 * this file. Sync vocabulary (adds / removes / hides + ONE declarative
 * arrange op) follows REQ-053 clause 4.
 */

#ifndef LOADOUT_MANIFEST_H
#define LOADOUT_MANIFEST_H

#include <string>
#include <vector>

namespace LoadoutManifest {

/// Only schema version this firmware reads. Anything else is treated as
/// unreadable and the menu falls back to compiled-in order (no legacy
/// readers, no migration shims — a version bump is a conscious decision).
constexpr int kSchemaVersion = 1;

/**
 * One manifest entry. `id` is the stable app identifier (the APP_ENTRY
 * enum name, e.g. "APP_BOOPER") — it survives label renames and
 * reorderings of AppManifest.h across firmware updates.
 */
struct LoadoutEntry {
    std::string id;        ///< stable app id, e.g. "APP_BOOPER"
    std::string name;      ///< display label (informational; registry wins)
    std::string category;  ///< flat, single-level category ("" = root)
    int         position = 0;   ///< menu position (0-based, display order)
    bool        hidden   = false; ///< true = keep entry but omit from menu

    // Reserved fields — parsed and round-tripped but unused by firmware
    // today. They exist so future app-delivery work (T-110/T-134) can
    // populate them without a schema bump. Empty string = unset.
    std::string format;    ///< reserved: entry format (e.g. "builtin", "blob")
    std::string blobPath;  ///< reserved: filesystem path to an app blob
    std::string version;   ///< reserved: app version string
    std::string abi;       ///< reserved: required ABI/HAL version
    std::string signature; ///< reserved: blob signature
};

/// A parsed manifest. `entries` is kept in display order (position ascending).
struct Loadout {
    int schemaVersion = kSchemaVersion;
    std::vector<LoadoutEntry> entries;
};

/**
 * Minimal registry view the merge consumes — built by the caller from the
 * compiled-in appDefs[]/appIds[] tables (see buildLoadoutRegistryView() in
 * AppDefs). Kept separate from AppDefinition so this lib stays dependency-
 * free. `category` must already be flattened to one level. Apps with an
 * empty `name` (e.g. the menu itself) are not menu items and are skipped.
 */
struct RegistryApp {
    std::string id;        ///< stable app id ("APP_BOOPER")
    std::string name;      ///< menu label; "" = not a menu item
    std::string category;  ///< flat single-level category
};

/// One merged menu row: an index into the registry array passed to
/// mergeWithRegistry(), plus the category/hidden state the menu should use.
struct MergedApp {
    int         appIndex;  ///< index into the RegistryApp array
    std::string category;  ///< flat category to register the app under
    bool        hidden;    ///< true = do not show in the menu
};

/// One item of the declarative `arrange` op: the full display order,
/// id-anchored. `category` optionally re-categorizes the entry.
struct ArrangeItem {
    std::string id;
    std::string category;
    bool        hasCategory = false;
};

/**
 * Flatten a compiled-in categoryPath to the single-level category model:
 * the FIRST path segment ("Games/Arcade" -> "Games", "Tools/LEDs" ->
 * "Tools", "" -> ""). Deeper nesting is deferred to a future schema bump.
 */
std::string flattenCategory(const char* path);

/**
 * Parse a manifest JSON document into `out`.
 *
 * Strict on structure (malformed JSON, missing/unsupported schemaVersion
 * => false, `out` untouched), lenient on content (unknown fields are
 * skipped for forward compatibility; entries without an id are dropped).
 * Entries are stable-sorted by `position` and renumbered 0..n-1.
 *
 * @return true on success.
 */
bool parseManifest(const char* json, Loadout& out);

/**
 * Serialize a Loadout back to JSON (canonical form: entries in array
 * order, positions renumbered, reserved fields emitted only when set).
 */
std::string serializeManifest(const Loadout& loadout);

/**
 * Build a baseline Loadout from the compiled-in registry (compile order,
 * nothing hidden). Used the first time something needs to persist a
 * manifest on a device that doesn't have one yet. Apps with an empty
 * name are skipped (not menu items).
 */
Loadout buildFromRegistry(const RegistryApp* apps, int count);

/**
 * Merge a manifest with the compiled-in registry to produce the menu:
 *  - manifest entries first, in manifest order; a manifest category
 *    overrides the registry one ("" falls back to the registry category)
 *  - stale manifest ids (no matching registry app) are pruned, not fatal
 *  - duplicate manifest ids: first one wins
 *  - compiled-in apps absent from the manifest are appended in compile
 *    order (so new apps appear after a firmware update)
 *  - hidden entries are still returned, flagged, so callers can both
 *    skip them for display and preserve them on rewrite
 *
 * An empty manifest therefore yields exactly the compile order.
 */
std::vector<MergedApp> mergeWithRegistry(const Loadout& loadout,
                                         const RegistryApp* apps, int count);

// ---- Sync vocabulary (REQ-053 clause 4): adds / removes / hides + ONE
// ---- declarative arrange op. Section contiguity (sections = contiguous
// ---- category runs in flat position order) is preserved by construction.

/// Add a new entry at the end of its category section (new categories
/// become a new section at the end). Fails on duplicate id or empty id.
bool applyAdd(Loadout& loadout, const LoadoutEntry& entry);

/// Remove the entry with `id`. Fails if not present.
bool applyRemove(Loadout& loadout, const char* id);

/// Set the hidden flag on the entry with `id`. Fails if not present.
bool applyHide(Loadout& loadout, const char* id, bool hidden);

/**
 * Apply the declarative arrange op: `order` is the full display order,
 * id-anchored; items may carry a new category. Unknown ids in `order`
 * are ignored; loadout entries missing from `order` keep their relative
 * order and are appended after. The result is normalized so each
 * category forms one contiguous run (first-appearance order), then
 * positions are renumbered.
 */
bool applyArrange(Loadout& loadout, const std::vector<ArrangeItem>& order);

} // namespace LoadoutManifest

#endif // LOADOUT_MANIFEST_H
