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
| `lget`, `lapply`, `syncinfo` | `prompt`, `status`, `upd` |
| `wifi scan`, `wifi add`, `wifi try`, `wifi saved` | `wifi <ssid>\|<pass>`, `wifi list\|first\|forget\|hint-bad\|hint-clear`, `cloud ...`, `link ...` |

The `local_test` PlatformIO environment defines `CF_TEST_CLI`. A normal
`local` build does not compile the gated dispatch arms or implementations.
`test/test_sync_usbwifi` pins this split from the driver's source: the four
WiFi setup verbs and `syncinfo.setup=1` outside `CF_TEST_CLI`, and every
`link`, `cloud` and older `wifi` form inside it.

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
                                 [cmd] syncinfo.lapply=<capability>
                                 [cmd] syncinfo.setup=1
                                 [cmd] syncinfo.fw=<firmware-version>
```

`syncinfo.fw` is always the last line; new keys go before it.
`syncinfo.setup=1` announces the WiFi setup verbs below (older firmware does
not send it, and answers them with `[err] unknown command`).

Raw bytes follow `fwdata` and `lapply` requests and successful `fread`/`lget`
headers as specified by their lengths. Paths are confined to `/apps/` and
`/assets/`. Whole-file and chunk CRC checks, retry behavior, loadout operations,
and all error replies are documented in `lib/SyncProtocol/README.md`.

### WiFi setup

```text
wifi scan              -> [cmd] wifi.scan=started
                          later, per network: [cmd] wifi.net=rssi=<dBm> sec=<open|wpa|wpa2|wpa3|ent> ssid_hex=<hex>
                          [cmd] wifi.scan.done=<n>
wifi add <len> <crc32> -> [cmd] wifi.saved=ssid_hex=<hex> position=1
  (then <len> raw bytes)  [err] wifi.full=1 | [err] wifi.invalid | [err] wifi.crc
wifi try               -> [cmd] wifi.try=started
                          later: [cmd] wifi.try=ok ssid_hex=<hex>
                             or: [cmd] wifi.try=fail reason=<auth|absent|timeout|busy>
wifi saved             -> [cmd] wifi.saved.n=<n>
                          [cmd] wifi.saved.net=ssid_hex=<hex>   (one per network, in the order tried)
                          [cmd] wifi.saved.done=<n>
scan or try while the radio is taken
                       -> [err] wifi.busy reason=<portal|music|link|checkin|ferry|update|bluetooth|wifi>
```

Network names always travel as lowercase hex of their bytes (they can hold
spaces, `|` and UTF-8). No reply or log line ever carries a password.

`wifi add`'s payload is `ssid\0pass`: exactly one NUL, a name of 1-32 bytes,
a password of 0-64 bytes (empty for an open network), at most 97 bytes, with
the CRC-32 of the whole payload in the header (lowercase or uppercase hex, as
`lapply`). It is saved as the first network to try, with the portal's rules
(`lib/CloudSync/SavedWifi.h`): at most three; a saved name takes the new
password and moves first; a fourth is refused with `wifi.full=1` and nothing
is dropped. The frame, name and password buffers are zeroed on every path. A
header with a length over 97 has its payload drained (as `lapply` does); one
whose length cannot be read is answered `wifi.invalid` after the input has
gone quiet, so no payload byte is read as a command. The line after a
`wifi add` is never echoed by `[err] unknown command` (it reads
`unknown command: (hidden)`), in case a sender sent more bytes than it
announced.

`wifi scan` and `wifi try` answer at once and report when done; keep other
verbs until the `.done` / `wifi.try=ok|fail` line arrives. `wifi scan` lists
at most 20 names, each once at its strongest, strongest first; hidden
networks are left out. `wifi try` joins only the first saved network (no
remembered place, no scan for the others), then switches WiFi off again:
`auth` means the password was refused, `absent` that the network was not
found (or nothing is saved), `timeout` neither within 15 s, `busy` that the
join was stopped. Both refuse while the setup portal or music player is open,
a link, check-in or USB file transfer is under way, a newly installed
firmware is still being checked, or
Bluetooth has started in this power cycle (WiFi then needs a restart);
`wifi` is the reason when another scan or try is still running. While either
runs, check-ins and links wait (`SerialCli::radioBusy()`), and opening the
portal or music player restarts into it, as it does during a check-in.

### Installing updates

```text
upd slot                     -> [cmd] upd.slot=<label> state=<s> boot=<label> other=<label> other_state=<s> pend_img=<0|1> unsig_ok=<0|1>
upd allow-unsigned on|off    -> [cmd] upd.unsig_ok=<1|0|error>
bad arguments                -> [err] upd.usage=upd allow-unsigned on|off
```

`upd slot` reads the app slots (the running one, the one the next start
uses, the other one) and their update states, whether an update record is
stored, and whether installing is allowed; it changes nothing.
`upd allow-unsigned` sets `upd.unsig_ok`, which lets the update prompt's
Install now hand off to the update session (`lib/OtaUpdate/README.md`).
Official releases are signed and install without it; it lets a Fidget
install unsigned (self-built) images over WiFi. It is a
USB serial command in every build so that holding the cable is the proof,
and nothing on the network can set it.

## `CF_TEST_CLI` verbs

`reset factory confirm` prints `[cmd] reset.factory=start`, uses the same
erase routine as the Settings screen, prints `[reset] factory=done` just before
restart, and is absent from release builds. It refuses an unverified firmware
image or armed update session. It erases LittleFS and NVS; use a bench device.
`reset factory confirm hold` is the same but waits 10 s after the LittleFS
format (`[reset] factory=formatted hold_ms=10000`) so the bench can cut power
before the NVS erase; the next start-up then prints `[reset] factory=finishing`
and `[reset] factory=done` and finishes the reset.
`reset factory confirm failmark` fakes a failed mark write (expect
`[reset] factory=refused reason=mark-write`, nothing erased).
`reset factory confirm failfs` fakes a failed LittleFS format: the reset
refuses with the mark kept, and the start-up after the next `reboot` finishes
it without the format (`[reset] factory=partial saved=1`); the start-up after
that prints `[reset] factory=partial` and shows the notice once.

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
wifi <ssid>|<pass> -> [cmd] wifi.saved=<ssid>   ([err] wifi.full=1 when 3 are saved)
wifi list          -> [cmd] wifi.count=<n> place=<0|1>, then [cmd] wifi.net=<i> name=<ssid> per network
wifi first <ssid>  -> [cmd] wifi.first=<0|1>
wifi forget <ssid> -> [cmd] wifi.forget=<0|1>
wifi hint-bad      -> [cmd] wifi.hint_bad=<0|1>
wifi hint-clear    -> [cmd] wifi.hint_clear=<0|1>
wasmstat -> [cmd] wasmstat....
```

`apps` lists compiled applications. `launch` switches immediately to a compiled
app or stages a manifest-backed WASM app. `net` reports fields applicable to the
current Wi-Fi mode. `mic` runs a short capture diagnostic and releases it.
`wifi <ssid>|<pass>` saves a network as the first one to try (the saved list
of up to 3 in `lib/CloudSync/SavedWifi.h`, the same one the portal and every
session use); `list`, `first` and `forget` read and reorder it (names only,
never passwords). `hint-bad` points the remembered place of the first network
at an access point that is not there, so the quick join fails the way it does
when that network is out of range and the fallback scan can be measured;
`hint-clear` drops the remembered place so the next join is a plain one.
`wasmstat` delegates its
tagged runtime report to `WasmFsApp`.

`heapstat` only reads the internal heap counters. `tlsprobe` starts one plain
task and reports the result later from the main loop. It reads the first
saved network (`wificfg.ssid/pass`, always a copy of it) without changing it, uses STA only, verifies the host against
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
wait for it for up to 5 s (nothing more is read meanwhile; the payload stays
queued), then run, or answer `[err] sync.busy` if it is still running (dev
mode listening only while it is inside a check-in, not in the wait between
check-ins). A refused `fwdata` or `lapply` still drains the payload length
from its header first, so the stream stays in frame. The sync verbs (`info`,
`syncinfo`, `lget`, the file verbs, `lapply`; not `version`) also hold off
new dev mode and awake check-ins for 10 s after the last one, so a browser
send is not interrupted by a check-in starting part-way through. The hold
only stops those two from starting: a start-up, daily, recovery or "Check
for updates" check-in still starts during a USB session, and it keeps the
store busy for its whole run (join, handshake, any download - often longer
than 5 s), so a write that meets one usually still answers `[err] sync.busy`
after the wait. Senders must still handle `sync.busy` (the website re-reads
the menu and tries again). While a write waits, every other command sent
behind it (including `version`) waits too, up to 5 s.
`tlsprobe` answers `radio-busy` during a session, and a session will not start
while a probe runs.

### Update settings (test build only)

```text
upd                          -> [cmd] upd.key=<key> type=<str|u8|u32|i32> value=<value>   (one per stored key)
                                [cmd] upd.done=<count>
upd offer <version> [source] -> [cmd] upd.offer=open version=<v> source=<source>
                                [cmd] upd.offer=suppressed reason=<skipped|not-newer|invalid> version=<v>
                                [err] upd.offer=busy | not-menu | refused
upd install <version>        -> [cmd] upd.install=restarting version=<v>   (then the update session)
                                [cmd] upd.install=refused reason=<no-update-slot|unsigned|battery|version|storage>
upd fault <name>             -> [cmd] upd.fault=<name|error>   (none|crash|hang|hal-hang|loop-crash|version|mount|session-hang)
upd seen-clear               -> [cmd] upd.seen_clear=<count|error>
bad arguments                -> [err] upd.usage=upd [offer <version> [source] | install <version> | fault <...> | slot | allow-unsigned on|off]
```

`upd install` is the Install now hand-off for one version (no newer-version
check: the version is the bench's explicit choice, as a person's choice
would be); the session still runs every manifest gate. `upd fault` stores a
one-shot fault in the test-only namespace `cftest`: `session-hang` stops the
next update session mid-download without feeding the watchdog; `crash`,
`hang`, `version` and `mount` make the next pending image crash, hang, see a
wrong version, or fail its filesystem mount during its self-test; `hal-hang`
hangs it before hardware start-up (right after it joins the watchdog);
`loop-crash` crashes it in its first main-loop pass, after its checks
passed; `none` clears it. `upd seen-clear` forgets the stored release freshness
(`upd.seen_*`) so a case can start from no update history. Bench case: `test/bench/cases/t250-ota-ab.json`.

`upd` lists every key in the NVS namespace `upd` (the update settings, see
`lib/UpdatePolicy/README.md`) with its stored type and value, in storage
order, and changes nothing. `upd offer` opens the update prompt ("Update
<version> ready (<source>)" / Install now / Remind me later / Skip this
version) for a stand-in offer held in RAM, through the same rules as a real
one: a skipped or not-newer version is refused. Only the answer stores
anything. Answer with `btn` (Down = 1, Enter = 5); the device logs
`[upd] prompt=firmware choice=<install|later|skip|none> version=<v> rej_write=<ok|error|->`.
The source defaults to `cyberfidget.com`. Bench case:
`test/bench/cases/t391-prompt-options.json`.

### Awake & dev mode (test build only)

```text
awake                         -> [cmd] awake.mode=<off|stay|dev> stop=<idle|until> listen=<0|1> worker=<0|1>
                                 polls=<n> deliveries=<n> failures=<n> connected=<0|1> since_use_ms=<ms>
                                 since_press_ms=<ms> low_ms=<ms> idle_ms=<ms> safety_ms=<ms> heap_min=<B>
                                 largest_min=<B> free_int=<B> largest_int=<B> bt_restart=<0|1>
awake set off|stay|dev [until|idle] -> [cmd] awake.set=<mode> stop=<stop>   (then [awake] set ... effect=<none|save|restart>)
awake idle <s>                -> [cmd] awake.idle_ms=<ms>     (RAM: "After 30 min without use" becomes <s>; 0 = built-in)
awake safety <s>              -> [cmd] awake.safety_ms=<ms>   (RAM: the 48 h safety net becomes <s>; 0 = built-in)
awake battery low|real        -> [cmd] awake.battery=<low|real>   (RAM: the battery reads below the floor)
awake tls <https-url>|off     -> [cmd] awake.tls=<on|off|error>   (RAM: one extra public GET per dev check-in)
awake poll                    -> [cmd] awake.poll=now          (the next dev check-in starts now)
awake legacy <0|1|2>          -> [cmd] awake.legacy=<v>        (writes the old upd.dev key only; migration bench)
awake stall <ms>              -> [cmd] awake.stall=<ms>        (RAM: the next dev check-in blocks <ms> ignoring cancellation)
awake crash <n>               -> [cmd] awake.crash=<n>         (cftest: panic <n> times, once per start, when dev mode starts listening)
```

`awake set` goes through the same path as the screen: entering or leaving
Dev mode restarts. Dev mode listening prints one line per check-in in test
builds (`[dev] poll=<n> http=<status> result=<ok|none|error> err=<code>
ms=<cycle> wait_ms=<next wait> heap_min=<B> largest_min=<B> free=<B>
at_ms=<ms>`; `free` is the internal free when the check-in ended, the steady
level a delivered app's launch decides on), `[awake] beside-app=<listen|pause>
free_int=<B> rest_int=<B> listening=<0|1>` when a delivered app opens (`rest_int`
= that steady level, 0 before the first check-in ends), `[dev] delivered batch=<id>` when an apply changed the menu,
and `[dev] relaunch id=<id> path=<file> from=<app|menu>` when a new version
of the running (or last-run) app starts. With `awake tls` set, each check-in
is followed by `[dev] tls ok=<0|1> http=<status> ms=<ms> free=<B> largest=<B>
min_boot_before=<B> min_boot=<B>`. The mode logs `[awake] boot ...`,
`[awake] end=<idle|safety-net|battery|restart-loop> ...`, `[awake] abnormal-reset count=<n> reason=<code>`, `[awake] bluetooth=<ask|restart|cancel>`
and `[awake] shown=<listening|not-connected|not-linked|no-wifi|none>`.
Bench case: `test/bench/cases/t379-devmode-poll.json`.

### Device link controls (test build only)

```text
link start   -> [cmd] link.code=<code>, later [cmd] link.state=<confirm|clear_apps|linked|declined|expired|error|unlinked> ...
link ok|no   -> [cmd] link.answer=ok|no
link clear|keep -> [cmd] link.answer=clear|keep
link unlink  -> [cmd] link.state=unlinked (or error reason=<code>)
link status  -> [cmd] link.status=linked:<yes|no> account:<label|-> fingerprint:<ok|mismatch> previous:<yes|no>
link forget  -> [cmd] link.forget=ok|error
```

`link forget` erases the whole `pair` namespace on the unit only: the current
link (including a `cloud token` credential), the previous-account memory used
for the Clear / Keep question, and any pending revokes. It sends nothing to the server and answers `error` while a
session runs. Use it to return a bench unit to the never-linked state before
rerunning the device-link case; a revoke it discards stays active on the
server until that credential is revoked from the site.

### UVLO simulation

```text
uvlo simulate <mV> -> [cmd] uvlo.simulate=<mV> plausible=<0|1> sleep_verdict=<shutdown|resleep> runtime_verdict=<shutdown|ok> sleep_threshold_mv=<mV> runtime_threshold_mv=<mV> debounce_ms=<ms>
bad arguments      -> [err] uvlo.usage=uvlo simulate <mV>
```

The argument is digits-only in the range 0--9999 mV. Plausibility uses the
same 2000--4600 mV window as the live guard. The command evaluates the pure
sleep decision and a local two-sample runtime debounce at the compiled
thresholds. It never reads, resets, or advances the live battery guard.
