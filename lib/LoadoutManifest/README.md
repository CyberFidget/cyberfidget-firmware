# LoadoutManifest — the loadout manifest (data-driven app registry)

The loadout manifest is a JSON file, `/loadout.json`, stored on the
device's LittleFS partition. It records the menu in flat display order:
which apps appear, under which category (a slash-separated path), at which position,
and whether they are hidden. At boot, `buildNestedMenu()` (lib/AppDefs)
merges the manifest with the compiled-in registry to build the menu;
without a manifest the menu falls back to compiled-in order exactly as
before the manifest can supply menu ordering.

Normal boot mounts LittleFS with format-on-failed-mount (one attempt, no
retry loop); a pending firmware image instead mounts without formatting until
it is verified. Factory reset unmounts and formats this partition, so an
interrupted reset boots with either its old manifest or the compiled-in menu.

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
| `category` | string | no | Category as a **slash-separated path** (`"Games"`, `"Tools"`, `"Tools/LEDs"`, `""` = root). The endorsed top level is closed and curated: `Screensavers`, `Games`, `Tools`, `Examples`, `Media`, plus the curated sub-levels the compiled-in menu uses (`"Tools/LEDs"`) — writers should emit only these (the parser tolerates other strings for forward compatibility, but they render as their own ad-hoc section and tooling may flag them). Overrides the compiled-in category; `""` falls back to it. The device menu splits the path on `/` into nested submenus; at each level a folder sits where its first shown entry is (see the arrange rules below). A manifest that stores only the top segment (`"Tools"` for an app compiled under `"Tools/LEDs"`, as earlier firmware wrote it) is still valid and shows that app directly under `Tools`. The merge fallbacks (an empty category, a compiled app the file does not list) always use the top segment of the compiled path, as earlier firmware did; nested paths come only from categories stored in the file (a fresh seed, an arrange). So a nested app added by a later firmware first shows in its top-level category until the menu is arranged. |
| `position` | int | no | Display position, 0-based. Entries are stable-sorted by position on load and renumbered on save. Missing positions fall back to array order. |
| `hidden` | bool | no (false) | Keep the entry (and its position) but omit it from the menu. |

Reserved fields — accepted, round-tripped, and **unused** by firmware
today. They allow future app delivery to populate
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

## Sync operations

The manifest-apply core speaks **adds / removes / hides + ONE declarative
`arrange` op** (`applyAdd` / `applyRemove` / `applyHide` /
`applyArrange`). There are no per-item reorder ops. `arrange` carries the
full display order, id-anchored, optionally re-categorizing items.
`add` inserts at the end of its category section. `arrange` leaves the
file in the order of the menu the device displays (a depth-first walk of
it), so what is saved is what is shown:

1. With a registry, the compiled apps the file does not list are written
   into it at the end, exactly as the merge appends them (compile order,
   first-segment category, `format: "builtin"`, not hidden). The device
   already shows them there; this lets the ordering see the same menu.
   Later built-in entries that repeat an earlier built-in id are dropped
   (the merge only uses the first).
2. The file is put in displayed-menu order, so entries the arrange does
   not name keep their displayed relative order.
3. The named entries are pulled to the front in order (taking any new
   category); the rest follow.
4. The result is put in displayed-menu order again.

"Displayed-menu order" uses the merge's own view: an entry is shown when
the merge keeps it (not a stale or duplicate id, not a delivered app
without its file) and it is not hidden, and it is placed by the category
the merge renders (an empty category is the first segment of the compiled
path). At every level, root included, a folder sits where its first shown
entry is and an app keeps its own place - how the device builds its menu.
Entries that are not shown never anchor a folder while it has a shown
entry; they keep their position among the entries of their own folder. A
folder with no shown entry sits at its first entry. Device callers pass
the registry to `applyArrange` / `applyOps` for this; without one, every
non-hidden entry counts as shown and is placed by its stored category.
Only the first 8 path levels are used for ordering; entries that share
them keep their relative order (the stored category is not changed).
With single-level categories this matches the earlier rule (each exact
category one contiguous run, first-appearance order), except that root
entries (`""`) now keep their own places among the folders instead of
being gathered into one run, and a hidden or pruned entry no longer
decides where its section goes. Unknown ids in an arrange are ignored;
entries missing from it are kept, never lost.

### Batch documents (`batch`, `base`, `replace`)

`applyOps` also accepts two optional top-level fields and one more op, for
deliveries that must be retried safely. A document without them applies
exactly as before.

* `batch` (string, 1-40 printable ASCII bytes, no spaces) and `base`
  (string, 1-8 hex digits: the CRC-32 of the manifest bytes the document
  was built against) are validated here - a wrong type or value rejects the
  whole document - but not acted on. The stale-revision refusal, the
  applied-batch record, and the orphan-blob sweep need the stored manifest
  bytes and the filesystem, so the transport session owns them (see
  `lib/SyncProtocol/README.md`, "Batch documents"). `parseOpsMeta` reads
  just these two fields for that session.
* `replace` (`applyReplace`): `{ "op": "replace", "entry": { ...full
  entry... } }` swaps an EXISTING entry's `blobPath`, `version`, `abi`,
  `name` and `signature` (a signature belongs to the blob it signed),
  keeping its position, category, hidden flag and format. An unknown id,
  an empty `blobPath`, or an existing entry whose `format` is not `wasm` /
  `blob` / `cfsprite` (builtin and other entries) rejects the document. `add` of an
  installed id stays rejected.
* `collectOpBlobPaths` lists every `add` / `replace` entry's `blobPath` so
  the transport can refuse a document pointing outside its confined write
  roots (same check as the file verbs).
* `serializeAppliedRecord` / `parseAppliedRecord` read and write the
  applied-batch record (`{"batch","result","crc_after","at","ops",
  "entries","doc_crc"}`) the transport keeps at `/apps/.applied.json`.
  `doc_crc` (the document's CRC-32) tells a retry of the same batch from a
  different document that reuses its id.
* The transport only sends these to firmware that advertises
  `syncinfo.lapply=batch1`, requires `base` with every `batch`, and skips
  the orphan sweep when there was no stored manifest before the apply.

A menu already on screen is not rebuilt when the manifest changes (it is
rebuilt on menu entry), so after a `replace` its item keeps pointing at the
old blob file; once the sweep has deleted that file, launching from the
stale menu fails until the menu is re-entered. Contract rule for senders:
blobs for a batch are uploaded only after the previous batch's reply; the
sweep after a batch may delete any delivered blob the manifest does not
reference.

Delivered app blobs are named `/apps/<id>-<hash8>.wasm` (hash8 = the first
8 lowercase hex digits of the blob's SHA-256). Because the name changes
with the content, a `replace` points the entry at a new file and the old
one becomes an orphan the transport sweeps after the apply.

Data screensavers (drawings sent as data) are entries with
`format: "cfsprite"` and `blobPath` = `/assets/ss/<id>-<hash8>.cfs`. The
merge keeps them as file-backed rows like delivered apps (no path = dropped)
and carries the format on the merged row, so the menu launches them through
the data-screensaver player (`lib/DataScreensaver`), never as an app. One
with an empty `category` shows under `Screensavers`. They are swept,
replaced and cleared like delivered apps. The schema is
unchanged (still version 1), so firmware without `replace` still reads a
manifest written by firmware with it.

## On-device reordering

Long-press (hold ~1s) the select button on a menu leaf to pick it up;
up/down move it within its category section; select commits, back
cancels. Commit walks the menu tree, emits one `arrange` op with the full
order, and persists via `AppManager::persistMenuArrangement` →
`LoadoutStore::save` (write temp file, rename over — a torn write leaves
the old manifest or none, never a corrupt one). The arrangement is
re-applied from `/loadout.json` on every boot.

On a device that has never persisted a manifest, the first commit
snapshots the compiled-in registry (`buildFromRegistry`) with each app's
compiled category path (`"Tools/LEDs"` stays `"Tools/LEDs"`), then applies
the arrange on top. The arrange carries each leaf's full category path
(menu labels from the root joined with `/`).

## Built-in menu report (`buildBuiltinReport`)

`buildBuiltinReport(apps, categoryPaths, count)` describes the firmware's
built-in menu for the `lbuiltin` serial read (see `lib/SyncProtocol/README.md`).
It is a report, not a seed: nothing stores it. Rows are the same ones
`compiledMenuRows` returns (every compiled app with a non-empty name, in
registry order); each has `id` = `slugifyBuiltinName(name)`, the `name`,
`format: "builtin"`, and `category` = the compiled category path **unflattened**
(`"Tools/LEDs"` stays `"Tools/LEDs"`), as the device shows its menu when no
manifest is stored. `categoryPaths` is parallel to `apps`; a top-level app
gets `""`, which `serializeManifest` writes as `"category": ""`.

The seed (`buildFromRegistry`, fed by `buildLoadoutRegistryView()`) uses
the same compiled paths, so a first write stores the categories this report
describes.
