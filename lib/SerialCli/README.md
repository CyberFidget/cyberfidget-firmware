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
| `menutree`, `screencap`, `screenstream` | `net`, `mic`, `wifi`, `wasmstat` |
| `fwrite`, `fwdata`, `fwcommit`, `fwabort`, `fdelete`, `flist`, `fstat`, `fread` | `btn`, `sleep`, `rail`, `gauge`, `uvlo` |
| `lget`, `lapply`, `syncinfo` | |

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
           [cmd] info.uptime_ms=...
           [cmd] info.battery.voltage_mv=...
           [cmd] info.battery.soc=...
           [cmd] info.battery.crate=...
           [cmd] info.wake.cause=...
help    -> [cmd] help=...
           [cmd] help.sync=...
           [cmd] help.test=...        (test-CLI builds only)
```

`info` reports a fixed tagged set. An implausible cached battery voltage outside
2.0--4.6 V is reported as `-1` mV.

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
mic  -> [cmd] mic.heap_free=... ... [cmd] mic.released=1
wifi <ssid>|<pass> -> [cmd] wifi.saved=<ssid>
wasmstat -> [cmd] wasmstat....
```

`apps` lists compiled applications. `launch` switches immediately to a compiled
app or stages a manifest-backed WASM app. `net` reports fields applicable to the
current Wi-Fi mode. `mic` runs a short capture diagnostic and releases it.
`wifi` persists credentials for later portal use. `wasmstat` delegates its
tagged runtime report to `WasmFsApp`.

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

### UVLO simulation

```text
uvlo simulate <mV> -> [cmd] uvlo.simulate=<mV> plausible=<0|1> sleep_verdict=<shutdown|resleep> runtime_verdict=<shutdown|ok> sleep_threshold_mv=<mV> runtime_threshold_mv=<mV> debounce_ms=<ms>
bad arguments      -> [err] uvlo.usage=uvlo simulate <mV>
```

The argument is digits-only in the range 0--9999 mV. Plausibility uses the
same 2000--4600 mV window as the live guard. The command evaluates the pure
sleep decision and a local two-sample runtime debounce at the compiled
thresholds. It never reads, resets, or advances the live battery guard.
