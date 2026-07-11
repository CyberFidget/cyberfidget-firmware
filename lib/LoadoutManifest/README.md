# LoadoutManifest — the loadout manifest (data-driven app registry)

The loadout manifest is a JSON file, `/loadout.json`, stored on the
device's LittleFS partition. It records the menu in flat display order:
which apps appear, under which single-level category, at which position,
and whether they are hidden. At boot, `buildNestedMenu()` (lib/AppDefs)
merges the manifest with the compiled-in registry to build the menu;
without a manifest the menu falls back to compiled-in order exactly as
before T-115.

This library has two halves:

| File | Runs on | Purpose |
|------|---------|---------|
| `LoadoutManifest.h/.cpp` | device, native tests, WASM emulator | pure core: parse, serialize, merge, sync ops. No Arduino/ESP-IDF includes. |
| `LoadoutStore.h/.cpp` | device only (`#ifndef HOST_TEST`, not in the WASM build) | LittleFS mount + read/write of `/loadout.json` with temp-file-then-rename saves |

Native tests: `pio test -e test_loadout` (see `test/test_loadout_*`).

## Schema (version 1)

```json
{
  "schemaVersion": 1,
  "entries": [
    {
      "id": "booper",
      "name": "Booper",
      "category": "Games",
      "position": 0,
      "hidden": false,
      "format": "builtin"
    }
  ]
}
```

Top level:

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `schemaVersion` | int | yes | Must be exactly `1`. Anything else (including absence) makes the whole file unreadable and the menu falls back to compiled-in order. There are no legacy readers and no migration shims — changing the schema is a conscious version bump. |
| `entries` | array | no (defaults empty) | Menu entries in display order. |

Per entry:

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `id` | string | yes | Stable app identifier. Builtins use a slug of the registry display name: lowercase, each non-alphanumeric run becomes `-`, then leading/trailing dashes are trimmed (for example, `"Dino Run"` becomes `"dino-run"`). Existing persisted `APP_ENTRY` enum ids are migrated once on load. Non-builtin ids are supplied by their source. Entries without an id are dropped. |
| `name` | string | no | Display label at the time the manifest was written. Informational — the compiled-in label wins at render time. |
| `category` | string | no | Flat, **single-level** category (`"Games"`, `"Tools"`, `""` = root). Overrides the compiled-in category; `""` falls back to it. Sections in the menu are contiguous runs of the same category in position order. Nested categories (`"Games/Arcade"`) are NOT part of schema 1 — deeper nesting is deferred to a future `schemaVersion` bump. |
| `position` | int | no | Display position, 0-based. Entries are stable-sorted by position on load and renumbered on save. Missing positions fall back to array order. |
| `hidden` | bool | no (false) | Keep the entry (and its position) but omit it from the menu. |

Reserved fields — accepted, round-tripped, and **unused** by firmware
today. They exist so the app-delivery work (T-110/T-134) can populate
them without a schema bump. Omitted from serialization while empty:

| Field | Type | Reserved for |
|-------|------|--------------|
| `format` | string | Entry format discriminator. Firmware-seeded registry entries use `builtin`; loadable entries use their blob format and carry `blobPath`. |
| `blobPath` | string | filesystem path to an app blob |
| `version` | string | app version |
| `abi` | string | required ABI / HAL version for a blob |
| `signature` | string | blob signature |

Unknown fields anywhere in the document are skipped (forward
compatibility); malformed JSON rejects the whole file (fallback to
compiled-in order — a bad manifest can never brick the menu).

## Merge semantics (`mergeWithRegistry`)

* Manifest entries first, in manifest order.
* Stale ids (app removed from firmware) are pruned, not fatal.
* Duplicate ids: first entry wins.
* Compiled-in apps missing from the manifest are appended in compile
  order — new apps appear after a firmware update without any migration.
* Hidden entries are returned flagged so callers can skip them for
  display but preserve them when rewriting.
* Apps with an empty compiled-in label (the menu app itself) are never
  menu entries.
* An empty manifest merges to exactly the compile order.

## Sync vocabulary (REQ-053 clause 4)

The manifest-apply core speaks **adds / removes / hides + ONE declarative
`arrange` op** (`applyAdd` / `applyRemove` / `applyHide` /
`applyArrange`). There are no per-item reorder ops. `arrange` carries the
full display order, id-anchored, optionally re-categorizing items.
Section contiguity is preserved by construction: `add` inserts at the end
of its category section, and `arrange` normalizes to contiguous sections
(first-appearance order, stable within a section). Unknown ids in an
arrange are ignored; entries missing from it are appended, never lost.

## On-device reordering

Long-press (hold ~1s) the select button on a menu leaf to pick it up;
up/down move it within its category section; select commits, back
cancels. Commit walks the menu tree, emits one `arrange` op with the full
order, and persists via `AppManager::persistMenuArrangement` →
`LoadoutStore::save` (write temp file, rename over — a torn write leaves
the old manifest or none, never a corrupt one). The arrangement is
re-applied from `/loadout.json` on every boot.

On a device that has never persisted a manifest, the first commit
snapshots the compiled-in registry (`buildFromRegistry`) with categories
flattened to their first path segment (`"Tools/LEDs"` → `"Tools"`), then
applies the arrange on top.
