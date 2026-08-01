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
| device only | `SerialCli` sync verbs | UART read/write + LittleFS glue; never compiled for native tests |

Native tests: `pio test -e test_sync` (framing, verb parsing, confinement,
corruption rejection). Manifest-ops apply is in `LoadoutManifest::applyOps`
(same suite + `test_loadout`).

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
  Device-to-browser payloads (`lget` and `fread`) work the same way: read the
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
the file verbs; `lget` and `lapply` are its only transport surface.

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
lapply <len> <crc32>              -> [cmd] lapply.ok=applied <n> entries <n>   (or [err] lapply.reject / .crc)
  <len raw bytes of ops JSON follow the line>
```

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

### Status report

```
syncinfo   -> [cmd] syncinfo.fs_total=<n> fs_used=<n> fs_free=<n>
              [cmd] syncinfo.manifest=<0|1> entries=<n> schema=<n>
              [cmd] syncinfo.fw=<version-string>
```

Firmware version is also available via the always-on `version` / `info`
verbs.

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
<-- [cmd] syncinfo.fw=1.4.2+ab12cd3\n
```

(`c1446436` is `crc32("HELLO")`; the manifest-payload CRC/len above are
illustrative.)
