# SyncProtocol - serial sync file transport

The browser drives device storage over the USB serial link (921600 baud)
through verbs on the SerialCli surface. The file/loadout transport can ferry
app and asset blobs in either direction, verify stored bytes, edit the loadout
manifest, and report installed state plus storage so the site does not have to
guess what is installed.

This library has two halves, mirroring `LoadoutManifest`:

| Runs on | Code | Purpose |
|---------|------|---------|
| device, native tests, WASM | `SyncProtocol.h/.cpp` | pure core: CRC-32, command-arg parsers, confinement, bounded reply formatting |
| device, native tests | `FerrySession.h/.cpp` | pure write session behind `fwrite`/`fwdata`/`fwcommit`/`fwabort` and `lapply`: idle/active state, every check, and the exact reply bytes; storage effects and payload bytes are injected (`FerryStorage`, `FerryByteSource`) |
| device only | `SerialCli` sync verbs | one transport driver: UART byte source (owns the payload gap/total-duration timeouts), LittleFS storage adapter, read verbs; never compiled for native tests |

Native tests: `pio test -e test_sync` (framing, verb parsing, confinement,
corruption rejection, and the ferry session driven with in-memory fakes).
Manifest-ops apply is in `LoadoutManifest::applyOps` (same suite +
`test_loadout`).

## Framing conventions

* **Commands are newline-terminated ASCII lines** on the existing CLI, same
  as `version` / `info`. Case-insensitive verb; space-separated args. Both
  `\n` (LF) and `\r\n` (CRLF) are accepted line terminators: the device
  dispatches on the `\r` and swallows a paired `\n`, so a length-framed
  payload begins only after the **full** terminator sequence - never with a
  stray `\n` misread as payload byte 0.
* **Replies use the stable line prefixes** `[cmd]` (success/data) and
  `[err]` (rejection), so the browser parses line-by-line. Success replies
  are `[cmd] <verb>.<tag>=<fields>`; errors are `[err] <verb>.<reason>=...`.
* **Binary payloads are length-framed, not line-framed.** A verb that moves
  bytes announces `<len>` (decimal) and a `<crc32>` (8 hex digits) on its
  command line; **exactly `len` raw bytes follow immediately after the
  line's terminator** (the `\n`, or the full `\r\n`) and are consumed by the
  device, not parsed as commands.
  Device-to-browser payloads (`lget`, `lbuiltin` and `fread`) work the same way: read the
  header line, then read exactly `len` bytes.
* **Checksum is CRC-32** (IEEE 802.3, reflected, poly `0xEDB88320`, the
  stock zlib/JS crc32). Hex, lowercase, zero-padded to 8 digits.
* **Numbers**: sizes/offsets/lengths are decimal `uint32`; CRCs are hex.
* **Chunk size**: the device advertises `chunk=<N>` (currently 4096) in the
  `fwrite.ok` and `fread.ok` replies. A `fwdata` payload or requested `fread`
  length may not exceed it; oversized reads are refused, not clamped.

## Confinement

All file reads, writes, listings, and deletes use the same `pathConfined()`
predicate and are confined to `/apps/` and `/assets/` on the device filesystem.
A file path must be absolute, name a file (no trailing `/`), carry no
`.`/`..`/empty segment, contain no spaces or control bytes, and be `<= 96`
bytes. The loadout manifest (`/loadout.json`) is deliberately not exposed by
the file verbs; `lget` and `lapply` are its only transport surface. The
applied-batch record `/apps/.applied.json` (and any path extending it, such
as its `.part` temp) is refused the same way; only `lapply` writes it.

`flist` takes a trailing-slash-free directory path: `/apps`, `/assets`, or a
subdirectory such as `/apps/icons`. The device appends a synthetic child
component and passes that file-shaped probe through the same `pathConfined()`
function. It does not relax or duplicate the confinement rules. Consequently
`flist /`, `flist /apps/`, and every traversal or outside-root form are refused.

## Verbs

### File write (chunked, restartable)

```
fwrite <path> <size> <crc32>      -> [cmd] fwrite.ok=<path> size=<n> chunk=<n> crc=<hex>
fwdata <offset> <len> <crc32>     -> [cmd] fwdata.ok=off <o> len <l>
  <len raw bytes follow the line>    (or [err] fwdata.crc=... to NAK -> resend)
fwcommit                          -> [cmd] fwcommit.ok=<path> size=<n> crc=<hex>
fwabort                           -> [cmd] fwabort.ok
```

* `fwrite` opens one session (any prior session is discarded), validates
  confinement + free space, and opens a temp file (`<path>.part`). One
  session at a time.
* `fwdata` writes a chunk. The per-chunk CRC gates each chunk: a mismatch
  is NAKed (`[err] fwdata.crc`) and **not** written - the browser resends
  the **same** offset. Chunks are offset-addressed, so they may be sent in
  any order and any chunk may be retried.
* `fwcommit` closes the temp file, **recomputes the whole-file CRC from
  disk**, and checks it against the `fwrite` value (and the size). Only on a
  match does it atomically rename the temp file over the final path. A
  mismatch (any dropped/garbled chunk) is rejected and the temp file
  discarded - never half-applied.
* **Restartable, not resumable-across-disconnect**: to recover from an
  interrupted transfer, re-issue `fwrite` (discards the partial temp) and
  resend. Within a live session, individual chunks retry by offset.
* **Abandoned sessions expire**: a session with no `fwrite`/`fwdata` for
  `kTransferIdleMs` (60 s) is dropped exactly as `fwabort` drops it (temp
  file removed, store released, cloud check-ins free to run again). The
  device logs one `[sync] fwrite.expired idle_ms=60000` line - deliberately
  not a `[cmd]`/`[err]` reply, so a returning sender never mistakes it for
  an answer. A late `fwdata` then gets `[err] fwdata.nosession` (payload
  drained, stream stays in frame). A live sender answers each chunk within
  its reply wait (10 s), far inside the timeout.
* **Idle sleep**: the verbs that move data (the store-writing verbs and
  `fread`) count as use for the device's 60 s idle-to-sleep timer, like a
  button press. `version` and the status reads (`info`, `syncinfo`, `lget`,
  `lbuiltin`, `flist`, `fstat`) do not, so a host that only polls status cannot keep the
  screen on forever.

### Delete

```
fdelete <path>                    -> [cmd] fdelete.ok=<path>   (or [err] fdelete.absent / .path)
```

### File list, stat, and read

```
flist <dir>                       -> [cmd] flist.entry=<name> size=<n>  (zero or more)
                                    [cmd] flist.done=<dir> entries=<n> truncated=<0|1> max=64
fstat <path>                      -> [cmd] fstat.ok=<path> size=<n> crc=<hex>
fread <path> <offset> <len>       -> [cmd] fread.ok=<path> off=<o> len=<l> chunk=4096 crc=<hex>
                                    <len raw bytes follow the header line>
```

`flist` enumerates exactly one directory. It emits at most 64 entries and
inspects at most one additional entry to set `truncated=1`; callers can never
mistake a capped result for a complete listing. Each entry carries its basename
and byte size. `fstat` streams the whole file through CRC-32 without loading the
file into memory. `fread` returns one non-empty range, refuses lengths above the
advertised chunk ceiling, and checks that `offset + len` lies within the file.
The CRC in each `fread.ok` header covers only that returned chunk.

### Loadout manifest

```
lget                              -> [cmd] lget.present=<0|1> entries=<n> schema=<n> len=<n> crc=<hex>
                                     <len raw bytes of manifest JSON follow (omitted when present=0)>
lbuiltin                          -> [cmd] lbuiltin.present=1 entries=<n> schema=1 len=<n> crc=<hex>
                                     <len raw bytes of the built-in menu JSON follow>
lapply <len> <crc32>              -> [cmd] lapply.ok=applied <n> entries <n>   (or [err] lapply.reject / .crc / .usage / .stale / .batchreuse / .record)
  <len raw bytes of ops JSON follow the line>
```

`lget` reports the stored manifest; with none stored it answers
`present=0` and no payload, and that line never changes (released readers
stop there). `lbuiltin` (advertised by `syncinfo.builtin=1`) reports the
firmware's built-in menu instead: every compiled menu app with a name, in
compiled order, each `{"id": <slug of the name>, "name", "category": <the
compiled path, nested and unflattened, e.g. "Tools/LEDs"; "" for a
top-level app>, "position", "hidden": false, "format": "builtin"}`, in the
same document shape as `/loadout.json`. It is the same whether or not a
manifest is stored, reads no file and writes nothing. A host that gets
`lget.present=0` asks `lbuiltin` to list what the device shows.

Hosts must stay stop-and-wait: send one command (and its payload), then wait
for its reply before the next. A write that waits for a cloud check-in
(`SerialCli`: up to `kBusyWaitMs`) leaves its payload unread in the device's
serial receive buffer (12 KB: one maximum `lapply` document, 8 KB, plus its
header); a host that streams further commands behind it can overrun that
buffer. The released website is stop-and-wait (`sync_protocol.mjs`,
`lapply()` awaits each reply).

`lapply` payload is a staged-ops document with adds, removes, hides, and one
declarative `arrange`:

```json
{ "ops": [
    { "op": "add",     "entry": { "id": "APP_X", "name": "X", "category": "Games",
                                  "format": "blob", "blobPath": "/apps/x.wasm",
                                  "version": "1.0", "abi": "1" } },
    { "op": "remove",  "id": "APP_Y" },
    { "op": "hide",    "id": "APP_Z", "hidden": true },
    { "op": "arrange", "order": [ { "id": "APP_A" },
                                  { "id": "APP_B", "category": "Tools" } ] }
] }
```

The device applies the ops to the stored manifest (or a compiled-in
registry snapshot if none exists yet) **atomically**: a malformed document
or any rejected op (duplicate/absent id, unknown op) leaves the stored
manifest untouched, and a valid document is persisted with the same
temp-file-then-rename save the on-device reorder uses. **Applied changes
take effect at the next boot menu build** (same as the long-press reorder),
so a torn write can never brick the running menu - it falls back per the
missing/stale-manifest rules.

#### Batch documents (`batch`, `base`, `replace`)

A delivery that must be retried safely adds two optional top-level fields
and may use the `replace` op. A document with neither field applies exactly
as above (same checks, same reply bytes, no extra effects).

```json
{ "batch": "7f3c-0001",
  "base":  "1b9d1c4e",
  "ops": [
    { "op": "replace", "entry": { "id": "booper", "name": "Booper",
                                  "blobPath": "/apps/booper-89abcdef.wasm",
                                  "version": "2.0", "abi": "1" } }
] }
```

* **Capability gate**: a device that supports this section reports
  `[cmd] syncinfo.lapply=batch1` (see Status report). Senders send `batch`,
  `base` or `replace` only to such a device: older firmware silently skips
  unknown top-level fields, so it would apply a stale document.
* `batch` - opaque id, 1-40 printable ASCII bytes (no spaces). `base` -
  1-8 hex digits, either case. Any other value (wrong type, too long,
  non-hex) rejects the whole document as `[err] lapply.reject`. A document
  with `batch` must also carry `base`; without it the reply is
  `[err] lapply.usage=batch requires base` and nothing changes. `base`
  alone is allowed (a stale check with no record).
* `replace` swaps an existing entry's `blobPath`, `version`, `abi`,
  `name` (label) and `signature`; position, category, hidden flag and
  format are kept. It is refused (whole document) for an unknown id, a
  missing `blobPath`, or an existing entry that is not a delivered blob app
  (`format` other than `wasm` / `blob` - builtin and sprite entries are
  refused). `add` of an installed id stays rejected.
* **Blob paths are confined**: in ANY document, the `blobPath` of every
  `add` / `replace` entry must pass the same `pathConfined()` check as the
  file verbs, or the whole document answers `lapply.reject`.
* The device checks, in this order:
  1. **Repeat**: if `batch` equals the batch recorded in
     `/apps/.applied.json` AND the `lapply` header CRC equals the recorded
     `doc_crc`, nothing is re-applied and the device answers the recorded
     success line (`[cmd] lapply.ok=applied <n> entries <n>`). The same id
     with different document bytes answers `[err] lapply.batchreuse` and
     nothing changes. This runs before the `base` check because the batch
     itself moved the manifest off its base.
  2. **Stale**: if `base` differs from the CRC-32 of the stored manifest
     (the `crc` `lget` reports; `00000000` when none is stored), the whole
     document is refused with `[err] lapply.stale=<current crc hex>\n` and
     nothing changes.
  3. **Apply**: as for any document; a rejected op answers `lapply.reject`.
  4. **Record** (only with `batch`): `{"batch","result","crc_after","at",
     "ops","entries","doc_crc"}` is written to `/apps/.applied.json` via
     `/apps/.applied.json.part` + rename BEFORE the success reply. `at` is
     wall-clock seconds, or 0 when the clock is unset. If the record cannot
     be written the manifest change stands and the reply is
     `[err] lapply.record=<crc after hex>\n`.
  5. **Orphan sweep**: after a successful batch document, top-level
     `/apps` files named `<id>-<hash8>.wasm` (8 lowercase hex digits) that
     the manifest no longer references are deleted. Skipped while a
     `fwrite` session is active, when the manifest cannot be re-read, and
     when the manifest BEFORE the apply was absent or unreadable (`base`
     `00000000`): a manifest rebuilt from the built-in apps references no
     blobs and must never be used to judge them orphans.
* **Upload order (contract)**: blobs for a batch are uploaded only after
  the previous batch's reply has arrived. The sweep after any batch may
  delete ANY delivered-shape blob the resulting manifest does not
  reference, including one uploaded early for a later batch.
* **Replaced app on a menu already on screen**: the menu is rebuilt on
  menu entry, not when the manifest changes. Until the menu is re-entered,
  its item for a replaced app still points at the old file; after the sweep
  deletes that file, launching the app from that stale menu fails. Leaving
  and re-entering the menu fixes it.
* Only successes are recorded. A refused document (stale, rejected) leaves
  the record as it was, and a retry is re-checked from scratch.
* Failure between the manifest save and the record (power cut, or the
  `lapply.record` error): the new manifest is in place, the record is the
  previous one or none (the rename fallback may already have removed it),
  and no success line was sent. A retry of the same batch
  carrying `base` is refused as stale with the post-apply CRC, so it is
  never applied twice; the sender reconciles with `lget`.
* `/apps/.applied.json` (and any path extending it) is refused by every
  file verb, so it cannot be written, deleted, or read over the wire; only
  the apply path writes it. It is not a menu entry (the menu is built from
  the manifest).

**Names the orphan sweep never deletes.** A browser send today writes
`/apps/<id>.wasm`; that shape has no `-<8 hex>` suffix, so those blobs are
never swept (the site removes them itself with `fdelete`). Also never swept:
anything outside the top level of `/apps` (nested files, `/apps/.diary/`,
`/assets/`), non-`.wasm` files, `.part` temps (the boot sweep owns those),
and names whose suffix is not exactly 8 lowercase hex digits. One overlap
remains: a browser-sent id that itself ends in `-` plus 8 lowercase hex
digits looks delivered, and is deleted if a batch apply runs while the
manifest does not reference it.

### Status report

```
syncinfo   -> [cmd] syncinfo.fs_total=<n> fs_used=<n> fs_free=<n>
              [cmd] syncinfo.manifest=<0|1> entries=<n> schema=<n>
              [cmd] syncinfo.id=0123456789ab
              [cmd] syncinfo.lapply=batch1
              [cmd] syncinfo.setup=1
              [cmd] syncinfo.builtin=1
              [cmd] syncinfo.fw=<version-string>
```

`syncinfo.builtin=1` advertises `lbuiltin` (see "Loadout manifest"). Absent
on older firmware, which answers `lbuiltin` with `[err] unknown command`.

`syncinfo.lapply` advertises the `lapply` batch contract (`batch`, `base`,
`replace`, applied record, orphan sweep - see "Batch documents"). Absent on
older firmware, which must not be sent those fields. An incompatible change
to that contract bumps the value (`batch2`, ...).

Firmware version is also available via the always-on `version` / `info`
verbs.

`syncinfo.id` and `info.id` report the same canonical unit id (the eFuse
base MAC in esptool byte order, 12 lowercase hex characters without separators).
`info.mac` remains the historical reversed compatibility output.

Compatibility rule: `syncinfo.fw` is always the LAST `syncinfo` line and
`info.wake.cause` the last `info` line. Readers stop there, so any new key
must be emitted before them; a line after the terminator would be read as
the reply to the reader's next command.

## Example byte flow (install one blob + stage a manifest edit)

```
--> fwrite /apps/hello.wasm 5 c1446436\n
<-- [cmd] fwrite.ok=/apps/hello.wasm size=5 chunk=4096 crc=c1446436\n
--> fwdata 0 5 c1446436\n
--> HELLO                      (5 raw bytes, no newline)
<-- [cmd] fwdata.ok=off 0 len 5\n
--> fwcommit\n
<-- [cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n

--> flist /apps\n
<-- [cmd] flist.entry=hello.wasm size=5\n
<-- [cmd] flist.done=/apps entries=1 truncated=0 max=64\n
--> fstat /apps/hello.wasm\n
<-- [cmd] fstat.ok=/apps/hello.wasm size=5 crc=c1446436\n
--> fread /apps/hello.wasm 1 3\n
<-- [cmd] fread.ok=/apps/hello.wasm off=1 len=3 chunk=4096 crc=aab69b8b\n
<-- ELL                          (3 raw bytes, no newline)

--> lapply 78 1b9d1c4e\n
--> {"ops":[{"op":"add","entry":{"id":"APP_HELLO","category":"Games", ... }}]}
<-- [cmd] lapply.ok=applied 1 entries 13\n

--> syncinfo\n
<-- [cmd] syncinfo.fs_total=1441792 fs_used=131072 fs_free=1310720\n
<-- [cmd] syncinfo.manifest=1 entries=13 schema=1\n
<-- [cmd] syncinfo.id=0123456789ab\n
<-- [cmd] syncinfo.lapply=batch1\n
<-- [cmd] syncinfo.setup=1\n
<-- [cmd] syncinfo.builtin=1\n
<-- [cmd] syncinfo.fw=1.4.2+ab12cd3\n
```

(`c1446436` is `crc32("HELLO")`; the manifest-payload CRC/len above are
illustrative.)
