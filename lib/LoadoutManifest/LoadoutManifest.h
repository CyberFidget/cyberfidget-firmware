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

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace LoadoutManifest {

/// Only schema version this firmware reads. Anything else is treated as
/// unreadable and the menu falls back to compiled-in order (no legacy
/// readers, no migration shims — a version bump is a conscious decision).
constexpr int kSchemaVersion = 1;

/**
 * One manifest entry. Builtin ids are slugs of registry display names.
 */
struct LoadoutEntry {
    std::string id;        ///< stable app id, e.g. "booper"
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
    std::string abi;       ///< required ABI/HAL version for a blob
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
    std::string id;        ///< canonical builtin slug id ("booper")
    std::string name;      ///< menu label; "" = not a menu item
    std::string category;  ///< flat single-level category
    std::string legacyId;  ///< exact APP_ENTRY enum name for migration
};

std::string slugifyBuiltinName(const char* name);
bool normalizeBuiltinIds(Loadout& loadout, const RegistryApp* apps, int count);

/// One merged menu row: an index into the registry array passed to
/// mergeWithRegistry(), plus the category/hidden state the menu should use.
struct MergedApp {
    int         appIndex;  ///< builtin: index into the RegistryApp array; blob: -1
    std::string category;  ///< flat category to register the app under
    bool        hidden;    ///< true = do not show in the menu
    // A ferried wasm app has no compile-time registry row. `appIndex == -1`
    // marks a blob row; `label`/`blobPath` carry what the menu needs to
    // register and launch it (T-183). Empty for builtin rows.
    std::string label;     ///< blob: menu label (builtin uses registry name)
    std::string blobPath;  ///< blob: confined /apps/... path to the .wasm
    int         abi = 0;   ///< blob: required HAL ABI; 0 = unversioned
    std::string id;        ///< blob: manifest entry id; empty for builtin rows
};

/// Parse a manifest ABI string. Empty or invalid values are unversioned (0).
int parseAbiVersion(const std::string& abi);

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

/// Drop every non-builtin entry and return the blob paths it referenced.
std::vector<std::string> removeNonBuiltin(Loadout& loadout);

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

/**
 * Swap the delivered blob of an EXISTING entry: `entry.id` names the entry;
 * its blobPath, version, abi, name (the label) and signature are replaced
 * with the values in `entry`. Position, category, hidden flag and format
 * are kept. Fails (loadout untouched) on an empty/unknown id, an empty
 * blobPath (a replace always names the new blob), or an existing entry
 * that is not a delivered blob app (format other than "wasm" / "blob").
 */
bool applyReplace(Loadout& loadout, const LoadoutEntry& entry);

/**
 * Collect the non-empty `blobPath` of every op entry (add / replace) in an
 * ops document, so the transport can confine them before applying. Returns
 * false on a malformed document (which applyOps rejects anyway).
 */
bool collectOpBlobPaths(const char* opsJson, std::vector<std::string>& out);

/// Longest `batch` id an ops document may carry.
constexpr size_t kMaxBatchIdLen = 40;

/**
 * The optional top-level delivery fields of an ops document. Absent fields
 * leave has* false. `base` is the CRC-32 of the manifest bytes the document
 * was built against (the value `lget` reports; 0 = no manifest stored).
 */
struct OpsMeta {
    bool        hasBatch = false;
    std::string batch;
    bool        hasBase = false;
    uint32_t    base = 0;
};

/**
 * Read just the top-level `batch` / `base` fields of an ops document
 * (the ops themselves are not applied or validated here). Returns false
 * when the document is malformed or either field is invalid: `batch` must
 * be a string of 1..kMaxBatchIdLen printable ASCII bytes, `base` a string
 * of 1..8 hex digits. applyOps() applies the same rules.
 */
bool parseOpsMeta(const char* opsJson, OpsMeta& out);

/**
 * The durable record of the last batch applied (`/apps/.applied.json`).
 * `ops` and `entries` are the counts the original success reply carried,
 * so a repeated batch can be answered with that exact reply.
 */
struct AppliedRecord {
    std::string batch;
    std::string result;         ///< "applied" (only successes are recorded)
    uint32_t    crcAfter = 0;   ///< CRC-32 of the manifest after the apply
    uint32_t    at = 0;         ///< wall-clock seconds, 0 = clock unknown
    int         ops = 0;
    int         entries = 0;
    /// CRC-32 of the ops document bytes (the `lapply` header CRC). A batch
    /// id is only a repeat when this matches too; 0 when absent.
    uint32_t    docCrc = 0;
};

/// Serialize a record: {"batch","result","crc_after","at","ops","entries",
/// "doc_crc"}.
std::string serializeAppliedRecord(const AppliedRecord& rec);

/// Parse a record. False on malformed JSON or a missing batch/result.
bool parseAppliedRecord(const char* json, AppliedRecord& out);

/**
 * Apply a staged-ops document to `loadout`, atomically. `opsJson` is a
 * JSON document in the transport's ops vocabulary (the write direction of
 * the serial sync protocol):
 *
 *   { "batch"?: "<id>", "base"?: "<crc32 hex>",
 *     "ops": [
 *       { "op": "add",     "entry": { "id": ..., "category": ..., ... } },
 *       { "op": "remove",  "id": ... },
 *       { "op": "hide",    "id": ..., "hidden": true },
 *       { "op": "arrange", "order": [ { "id": ..., "category"?: ... }, ... ] },
 *       { "op": "replace", "entry": { "id": ..., "blobPath": ..., ... } }
 *   ] }
 *
 * The vocabulary is adds / removes / hides + ONE declarative arrange — the
 * same staged-change set the on-device reorder and the website staging
 * model speak — plus `replace` for swapping a delivered app's blob. This
 * reuses applyAdd/applyRemove/applyHide/applyArrange/applyReplace rather
 * than forking their logic.
 *
 * `batch` and `base` are optional and validated here (see parseOpsMeta) but
 * not acted on: the stale-revision check and the applied-batch record need
 * the stored manifest bytes and the filesystem, so the transport session
 * owns them. A document without them applies exactly as before.
 *
 * Atomic: the ops are applied to a working copy; if the document is
 * malformed OR any op fails (duplicate/absent id, etc.) the function
 * returns false and `loadout` is left untouched — never half-applied.
 * On success `loadout` is replaced with the result and true is returned.
 * When non-null, `*appliedOut` receives the number of ops applied.
 */
bool applyOps(Loadout& loadout, const char* opsJson, int* appliedOut = nullptr);

} // namespace LoadoutManifest

#endif // LOADOUT_MANIFEST_H
