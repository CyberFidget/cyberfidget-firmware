# SyncProtocol — serial sync transport (write direction)

The browser drives device storage over the USB serial link (921600 baud)
through verbs on the SerialCli surface. This is the **write** half of the
file/loadout transport family: ferry app/asset blobs to the device, edit
the loadout manifest, and report installed state + storage back so the
site stops guessing what is installed. The **read** half (offload:
`flist` / `fstat` / `fread`) reuses the same framing and checksum
conventions and slots in later.

This library has two halves, mirroring `LoadoutManifest`:

| Runs on | Code | Purpose |
|---------|------|---------|
| device, native tests, WASM | `SyncProtocol.h/.cpp` | pure core: CRC-32, command-arg parsers, write-confinement predicate |
| device only | `SerialCli` sync verbs | UART read/write + LittleFS glue; never compiled for native tests |

Native tests: `pio test -e test_sync` (framing, verb parsing, confinement,
corruption rejection). Manifest-ops apply is in `LoadoutManifest::applyOps`
(same suite + `test_loadout`).

## Framing conventions

* **Commands are newline-terminated ASCII lines** on the existing CLI, same
  as `version` / `info`. Case-insensitive verb; space-separated args.
* **Replies use the stable line prefixes** `[cmd]` (success/data) and
  `[err]` (rejection), so the browser parses line-by-line. Success replies
  are `[cmd] <verb>.<tag>=<fields>`; errors are `[err] <verb>.<reason>=...`.
* **Binary payloads are length-framed, not line-framed.** A verb that moves
  bytes announces `<len>` (decimal) and a `<crc32>` (8 hex digits) on its
  command line; **exactly `len` raw bytes follow immediately after the
  line's newline** and are consumed by the device, not parsed as commands.
  Device→browser payloads (`lget`) work the same way: read the header line,
  then read exactly `len` bytes.
* **Checksum is CRC-32** (IEEE 802.3, reflected, poly `0xEDB88320`, the
  stock zlib/JS crc32). Hex, lowercase, zero-padded to 8 digits.
* **Numbers**: sizes/offsets/lengths are decimal `uint32`; CRCs are hex.
* **Chunk size**: the device advertises `chunk=<N>` (currently 4096) in the
  `fwrite.ok` reply. A `fwdata` chunk may not exceed it.

## Confinement

Writes and deletes are confined to `/apps/` and `/assets/` on the device
filesystem. A path must be absolute, name a file (no trailing `/`), carry
no `.`/`..`/empty segment, contain no spaces or control bytes, and be
`<= 96` bytes. The loadout manifest (`/loadout.json`) is deliberately **not**
writable through `fwrite` — it is edited only through `lapply`, which runs
the manifest apply core instead of a blind byte overwrite.

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
  is NAKed (`[err] fwdata.crc`) and **not** written — the browser resends
  the **same** offset. Chunks are offset-addressed, so they may be sent in
  any order and any chunk may be retried.
* `fwcommit` closes the temp file, **recomputes the whole-file CRC from
  disk**, and checks it against the `fwrite` value (and the size). Only on a
  match does it atomically rename the temp file over the final path. A
  mismatch (any dropped/garbled chunk) is rejected and the temp file
  discarded — never half-applied.
* **Restartable, not resumable-across-disconnect**: to recover from an
  interrupted transfer, re-issue `fwrite` (discards the partial temp) and
  resend. Within a live session, individual chunks retry by offset.

### Delete

```
fdelete <path>                    -> [cmd] fdelete.ok=<path>   (or [err] fdelete.absent / .path)
```

### Loadout manifest

```
lget                              -> [cmd] lget.present=<0|1> entries=<n> schema=<n> len=<n> crc=<hex>
                                     <len raw bytes of manifest JSON follow (omitted when present=0)>
lapply <len> <crc32>              -> [cmd] lapply.ok=applied <n> entries <n>   (or [err] lapply.reject / .crc)
  <len raw bytes of ops JSON follow the line>
```

`lapply` payload is a staged-ops document in the REQ-053 clause-4
vocabulary — adds / removes / hides + ONE declarative `arrange`:

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
so a torn write can never brick the running menu — it falls back per the
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
