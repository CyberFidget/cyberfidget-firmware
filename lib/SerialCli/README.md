# SerialCli - USB serial command surface

`SerialCli` is the line-oriented control and observation surface on the USB
UART (921600 baud). It covers firmware identification, display observation,
file/loadout synchronization, and a gated set of bench-only device controls.

## Framing conventions

* Requests are newline-terminated ASCII. Verbs are case-insensitive; arguments
  are C strings and remain case-sensitive unless a verb says otherwise. LF and
  CRLF line endings are accepted.
* Successful replies use `[cmd]`; errors use `[err]`. The normal shapes are
  `[cmd] <verb>.<tag>=<fields>` and `[err] <verb>.<reason>=...`.
* A command normally returns one line. Multi-line reports end in a documented
  `.done` line or have a fixed set of tagged lines.
* The `[boot]` and `[uvlo]` prefixes belong to startup and autonomous UVLO
  reporting. CLI verbs never emit `[uvlo]`.
* Sync payloads are length-framed raw bytes with CRC-32. See
  `lib/SyncProtocol/README.md` for the full transport and confinement contract.

## Build gating

| Always present | Requires `CF_TEST_CLI` |
|---|---|
| `version`, `info`, `help`, `mark`, `reboot`, `battery`, `diary` | `apps`, `app`, `launch`, `soak` |
| `menutree`, `screencap`, `screenstream` | `net`, `heapstat`, `tlsprobe`, `tlsalloc`, `mic`, `wifi`, `wasmstat` |
| `fwrite`, `fwdata`, `fwcommit`, `fwabort`, `fdelete`, `flist`, `fstat`, `fread` | `btn`, `sleep`, `rail`, `gauge`, `uvlo` |
| `lget`, `lapply`, `syncinfo` | `prompt`, `status` |

The `local_test` PlatformIO environment defines `CF_TEST_CLI`. A normal
`local` build does not compile the gated dispatch arms or implementations.

## Always-present verbs

### Identification and help

```text
version -> [cmd] version=<firmware-version>
info    -> [cmd] info.fw=...
           [cmd] info.type=...
           [cmd] info.built=...
           [cmd] info.git=...
           [cmd] info.dirty=...
           [cmd] info.chip=...
           [cmd] info.mac=...
           [cmd] info.id=0123456789ab
           [cmd] info.uptime_ms=...
           [cmd] info.battery.voltage_mv=...
           [cmd] info.battery.soc=...
           [cmd] info.battery.crate=...
           [cmd] info.board_rev=<major>.<minor>
           [cmd] info.board=src=<efuse|default|unknown-layout|read-error> hil=<0|1> eng=<0|1> layout=<n>
           [cmd] info.wake.cause=...
help    -> [cmd] help=...
           [cmd] help.sync=...
           [cmd] help.test=...        (test-CLI builds only)
```

`info` reports a fixed tagged set. An implausible cached battery voltage outside
2.0--4.6 V is reported as `-1` mV.
`info.id` is the canonical unit id: the eFuse base MAC in esptool byte order,
as 12 lowercase hex characters without separators. `info.mac` retains its
historical reversed presentation for compatibility.

`info.board_rev` is the mainboard hardware revision as `major.minor` (for
example `1.2`), read once at boot from the board identity block in eFuse BLK3
(parsed by `lib/BoardInfo`). `info.board` says where it came from: `efuse` for
a provisioned board, `default` for a board whose block carries no identity
magic (every board built before provisioning, or an unreadable block), and
`unknown-layout` when the magic is present but the layout version is not one
this firmware understands. The last two report the rev 1.2 defaults with
`hil=0 eng=0`; `layout` is the raw layout byte (0 when no magic). `hil=1`
marks a hardware-in-the-loop bench unit and `eng=1` an engineering sample.
`info.wake.cause` is the reply's terminator line: new `info` keys are always
added above it, and host readers read through it rather than counting lines.
The `version` reply and the boot banner do not carry the board revision.

### Timeline, reset, and battery snapshot

```text
mark <id> -> [cmd] mark=<id> uptime_ms=<millis>
mark      -> [err] mark.usage=mark <id>

reboot    -> [cmd] reboot=now

battery   -> [cmd] battery.vcell_mv=<mV|-1> soc=<percent> crate=<percent/hour>
```

`mark` echoes everything after the verb verbatim and is intended to frame a
bench capture against device uptime. `reboot` is an immediate bench crash-reset:
the reply is emitted, the UART is flushed, and the ESP32 restarts without an
application teardown. `battery` reads the globals refreshed by
`BatteryManager::update()` every 200 ms and performs no gauge transaction.

### Battery diary

```text
diary -> [cmd] diary.stats=boot=<n> checkins=<n> on_s=<n> cycles=<n> vmin=<mv> vmax=<mv> written=<n> dropped=<n>
         [cmd] diary.rec=<seq> ev=<name> t=<n> mv=<n> soc=<x.x> crate=<x.xx>  (up to eight)
         [cmd] diary.done=<total_records>
diary clear -> [cmd] diary.clear=ok
bad argument -> [err] diary.usage=diary [clear]
```

The records and stats are also readable through `flist /apps/.diary` and
`fread /apps/.diary/<file> ...`. Clearing keeps lifetime boot count and
accumulated on-time while resetting the ring and other stats.

### Display observation

```text
menutree -> [cmd] menutree....
screencap -> [cmd] screencap w=128 h=64 bpp=1 fmt=colpage len=<n> b64=<data>
screenstream off -> [cmd] screenstream=off
screenstream on [fps] -> [cmd] screenstream=on fps=<fps> interval_ms=<ms>
```

`menutree` delegates its tagged tree dump to `MenuManager`. `screencap` returns
the 1024-byte OLED framebuffer as base64. `screenstream` periodically emits the
same screencap frame; its accepted frame rate is clamped by the implementation.

### File synchronization

```text
fwrite <path> <size> <crc32>  -> [cmd] fwrite.ok=<path> size=<n> chunk=<n> crc=<hex>
fwdata <off> <len> <crc32>    -> [cmd] fwdata.ok=off <o> len <n>
fwcommit                      -> [cmd] fwcommit.ok=<path> size=<n> crc=<hex>
fwabort                       -> [cmd] fwabort.ok
fdelete <path>                -> [cmd] fdelete.ok=<path>
flist <dir>                   -> [cmd] flist.entry=<name> size=<n> ...
                                 [cmd] flist.done=<dir> entries=<n> truncated=<0|1> max=64
fstat <path>                  -> [cmd] fstat.ok=<path> size=<n> crc=<hex>
fread <path> <off> <len>      -> [cmd] fread.ok=<path> off=<o> len=<n> chunk=<n> crc=<hex>
lget                          -> [cmd] lget.present=<0|1> entries=<n> schema=<n> len=<n> crc=<hex>
lapply <len> <crc32>          -> [cmd] lapply.ok=applied <n> entries <n>
syncinfo                      -> [cmd] syncinfo.fs_total=<n> fs_used=<n> fs_free=<n>
                                 [cmd] syncinfo.manifest=<0|1> entries=<n> schema=<n>
                                 [cmd] syncinfo.id=0123456789ab
                                 [cmd] syncinfo.fw=<firmware-version>
```

Raw bytes follow `fwdata` and `lapply` requests and successful `fread`/`lget`
headers as specified by their lengths. Paths are confined to `/apps/` and
`/assets/`. Whole-file and chunk CRC checks, retry behavior, loadout operations,
and all error replies are documented in `lib/SyncProtocol/README.md`.

## `CF_TEST_CLI` verbs

### Application and network controls

```text
apps -> [cmd] apps.<index>=<name> ...
app  -> [cmd] app.index=<index>
        [cmd] app.name=<name>
        [cmd] app.uptime_ms=<millis>
launch <name|index> -> [cmd] launch.ok=<index>
launch <blob-id>    -> [cmd] launch.ok=blob path=<path>
soak <app>          -> [cmd] soak=<app>
soak off            -> [cmd] soak=off
net  -> [cmd] net.mode=<mode> ...
heapstat -> [cmd] heapstat.free_int=<B> min_free_int=<B> largest_int=<B>
tlsprobe [url] -> [cmd] tlsprobe.started=1
                  [cmd] tlsprobe.ok=<0|1> state=<done|failed|timeout> err=<code> join_ms=<n> tls_ms=<n> get_ms=<n> http=<status> bytes=<n> heap_free_min=<B> largest_min=<B> heap_min_before=<B> heap_min_boot=<B> stack_size=<B> stack_hw=<B> url=<url>
tlsalloc <psram|internal> -> [cmd] tlsalloc.ok=<mode> psram_free=<B>
mic  -> [cmd] mic.heap_free=... ... [cmd] mic.released=1
wifi <ssid>|<pass> -> [cmd] wifi.saved=<ssid>
wasmstat -> [cmd] wasmstat....
```

`apps` lists compiled applications. `launch` switches immediately to a compiled
app or stages a manifest-backed WASM app. `net` reports fields applicable to the
current Wi-Fi mode. `mic` runs a short capture diagnostic and releases it.
`wifi` persists credentials for later portal use. `wasmstat` delegates its
tagged runtime report to `WasmFsApp`.

`heapstat` only reads the internal heap counters. `tlsprobe` starts one plain
task and reports the result later from the main loop. It reads `wificfg`
credentials without changing them, uses STA only, verifies the host against
the trusted root list (`lib/TrustedRoots`, the same list the check-in client
uses), and turns Wi-Fi off before reporting. A host whose chain does not end
in that list fails with `err=tls-connect`. The list's PEM text is held in
PSRAM; parsing it into the handshake follows the `tlsalloc` placement, so
the internal-placement probe now also carries the parsed roots (about
13 KB of DER plus parse structures). The default
URL is `https://cyberfidget.com/update/firmware.php?list=1`; an override must
be HTTPS and contain no whitespace. `tls_ms` measures the SDK connection open,
including DNS, TCP, TLS handshake, and request headers; `get_ms` runs from
connection open through the full response body. Internal free heap and largest block are
sampled before join, after join, after handshake, and after GET. The probe
allows 10 s for STA join and 20 s overall, with individual network calls
limited to 4 s for task-watchdog coverage. It reports
`tlsprobe.error=no-credentials`, `busy`, `radio-busy`, `invalid-url`, or
`task-create` before starting when applicable. `stack_hw` is unused task
stack in bytes; `heap_min_before` and `heap_min_boot` are the allocator's
minimum since boot, read before and after the probe - the handshake trough
falls between samples, so a lower `heap_min_boot` is the real floor. The 4 s
call limit is per socket operation, so a slow DNS lookup plus a multi-read
handshake can still approach the 5 s task watchdog.

`tlsalloc psram` routes mbedTLS allocations to PSRAM (falling back to internal
RAM); `tlsalloc internal` restores internal placement, the SDK default. It is
refused while a probe runs. With the SDK default, a fresh-boot probe drives
internal free RAM to about 2 KB; with PSRAM placement it stays above 40 KB.

`soak` uses the same app resolution and launch path as `launch`, then keeps the
idle interaction clock pinned in `AppManager`. It remains active until
`soak off` or the runtime battery guard shuts the device down. Starting a soak
adds a diary marker carrying the resolved app index.

### Button and sleep controls

```text
btn <index> press   -> [cmd] btn.press=<index>
btn <index> release -> [cmd] btn.release=<index>
btn <index> tap     -> [cmd] btn.tap=<index> release_in_ms=120
sleep               -> [cmd] sleep=requested
```

A tap generates a later `[cmd] btn.release=<index>` line. `sleep` sets a request
consumed by `AppManager`, keeping teardown out of serial dispatch.

### Sample prompt

```text
prompt <n> [timeout_ms] -> [cmd] prompt.open=<n> timeout_ms=<ms>
                           ... later, when it closes:
                           [cmd] prompt.result=<index|none>
bad arguments           -> [err] prompt.usage=prompt <1-8> [timeout_ms]
prompt already open     -> [err] prompt.busy=1
not on the menu         -> [err] prompt.refused=not-menu
```

Opens a `ModalPrompt` (see `lib/MenuManager/README.md`) titled "Sample prompt"
with `n` placeholder options, 1 to 8. The last option is always a long label so
its row scrolls when focused. `timeout_ms` is digits only, 0 to 3600000; 0 or
omitted means no timeout. The prompt opens only over the menu (or the boot screen); drive it
with `btn` (Up = 0, Down = 1, Enter = 5) or the physical buttons.
`prompt.result` is the zero-based chosen option, or `none` when the timeout
expired. Use it with `screencap` for bench screenshots of 2, 3 and 8 options.

### Status bar states

```text
status                -> [cmd] status.bar=<current line|->
                         [cmd] status.badge=<0|1>
                         [cmd] status.glyph=<never|hour|today|days|live> age=<label|-> checkin_age_s=<n|-> cached=<0|1>
                         [cmd] status.item=<kind> pri=<0-2> sticky=<0|1> attn=<0|1> late=<0|1> cached=<0|1> text=<text>   (one per entry)
                         [cmd] status.count=<n>
status post <kind> [late] [cached] [text]
                      -> [cmd] status.post=<kind> badge=<0|1>
                         [err] status.post.refused=<kind>        (info/warning without text)
status popup <kind> [late] [cached] [text]
                      -> [cmd] status.popup=<kind> open=1
                         ... later: [cmd] status.popup.result=<accept|ignore> kind=<kind>
                         [cmd] status.popup=<kind> open=0 routed=bar   (not on the menu)
status clear [kind]   -> [cmd] status.clear=all | [cmd] status.clear=<kind> removed=<n>
status checkin <never|seconds_ago> [cached]
                      -> [cmd] status.checkin=<seconds_ago> cached=<0|1> glyph=<state>
                         [cmd] status.checkin=never glyph=never
bad arguments         -> [err] status.usage=...
```

Drives the main-menu status bar (see `lib/MenuManager/README.md`) without any
networking, so every state can be shown and captured with `screencap`.
`<kind>` is `info`, `warning`, `checking`, `ready`, `changes` or `listening`.
Empty text uses the kind's own copy ("Checking for updates...", "Update
ready", "App changes waiting", "Dev mode"); `info` and `warning` need text.
The words `late` and `cached` right after the kind set those flags. Posts are
sticky at the kind's default priority (`ready` and `changes` badge the Status
item). A popup offers the accept option and "Later"; Later, or no answer,
routes the event to the bar and badge. `checkin` records a check-in that many
seconds ago (up to 8640000, 100 days) so the WiFi glyph and age label can be
shown for every bucket. The read-back ends on `status.count`: new keys go
above it.

### Rail controls

```text
rail aux on|off  -> [cmd] rail.aux=<on|off>
rail oled off    -> [cmd] rail.oled=off note=i2c-down-until-reboot
rail oled on     -> [cmd] rail.oled=on note=display-reinit-best-effort
bad arguments    -> [err] rail.usage=rail <oled|aux> <on|off>
```

`rail oled off` first shuts down the display and I2C controller and releases
SDA/SCL before removing OLED power. **After this command the shared I2C bus is
not operational: the display, fuel gauge, and accelerometer are unavailable
until `reboot`, and cached battery globals freeze at their last values.**
`rail oled on` only attempts the documented display/bus bring-up sequence; it
is a best-effort bench aid, not restoration of every I2C peripheral. Use
`reboot` for a supported recovery.

### Fuel-gauge controls

```text
gauge hibrt force -> [cmd] gauge.hibrt=force hibernating=<0|1>
gauge hibrt auto  -> [cmd] gauge.hibrt=auto hibernating=<0|1>
gauge alert-min <V> -> [cmd] gauge.alert_min_v=<readback-volts>
bad arguments -> [err] gauge.usage=gauge <hibrt force|hibrt auto|alert-min <V>>
```

`force` writes `HIBRT=0xFFFF`. `auto` writes `HIBRT=0x0000`; it means “not
forced,” leaving normal gauge behavior rather than forcing wakefulness. Both
hibernate replies read back the hibernating bit after the write.

`alert-min` accepts 0.0--5.1 V and reports the quantized 20 mV register
readback. The setting is transient: `BatteryManager::init()` resets VALRT.MIN
to 3.9 V on every boot.

### Cloud pull controls (test build only)

```text
cloud base <url>          -> [cmd] cloud.base=ok|error
cloud token <credential>  -> [cmd] cloud.token=ok|error
cloud autoapply on|off    -> [cmd] cloud.autoapply=ok|error
cloud check               -> [cmd] cloud.result=<ok|none|error> err=<code> applied=<batch|-> offered=<fw|-> next_ms=<n> heap_min=<B>
```

`cloud check` starts an asynchronous session and emits its one result line
when the worker completes. The credential is written to `pair.tok` for a LAN
bench and is never echoed. `cloud base` accepts HTTP only in a test build.
The check-in and loadout use the site's real endpoints; the server's rate
limit can make a successful session last over one minute. `err=` carries a
`rejected:<reason>` answer when an offer can never apply, and
`report-deferred` when the answer waits for the next session. The `upd.base`
override is read only by test builds.

While a session runs, `fwrite`, `fwdata`, `fwcommit`, `fwabort`, `fdelete` and `lapply`
answer `[err] sync.busy`. A refused `fwdata` or `lapply` still drains the
payload length from its header first, so the stream stays in frame.
`tlsprobe` answers `radio-busy` during a session, and a session will not start
while a probe runs.

### UVLO simulation

```text
uvlo simulate <mV> -> [cmd] uvlo.simulate=<mV> plausible=<0|1> sleep_verdict=<shutdown|resleep> runtime_verdict=<shutdown|ok> sleep_threshold_mv=<mV> runtime_threshold_mv=<mV> debounce_ms=<ms>
bad arguments      -> [err] uvlo.usage=uvlo simulate <mV>
```

The argument is digits-only in the range 0--9999 mV. Plausibility uses the
same 2000--4600 mV window as the live guard. The command evaluates the pure
sleep decision and a local two-sample runtime debounce at the compiled
thresholds. It never reads, resets, or advances the live battery guard.
