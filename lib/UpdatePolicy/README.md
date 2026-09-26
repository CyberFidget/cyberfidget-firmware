<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# UpdatePolicy

When the Fidget checks in with the site, and what the update prompt offers. `CheckinPolicy` is the pure rule
set (host-tested in `test/test_upd_policy_checkin`, `pio test -e
test_upd_policy`); `CheckinScheduler` (in `lib/CloudSync`, so the
library graph gains no new cycle through `AppManager`) is the device glue
that reads stored settings, the battery and the radio state, asks the policy, and starts
`CloudSync` sessions.

## Sessions

| Session | Started from | Needs |
|---|---|---|
| Boot | first `AppManager::loop` pass while the start-up animation plays | Auto-check on, "Check at start-up" on (`upd.boot_chk`, u8, missing = on), saved WiFi, a link, a normal start (not a restart that relaunches the portal, Music Player, linking, a check or a skipped animation), at least an hour since the last check-in (`kBootMinGapSec`: every button wake is a start; only a set clock with a stored check-in time, or a success earlier this power-on, counts as a recent check-in, so a cold start after a reset or a flat battery always gets its boot check), no backoff running, VBAT >= 3.6 V and SOC >= 20 % |
| Daily | an hourly battery timer wake whose check-in is due | the same, plus the interval passed, no backoff running; one session per due wake (the headless path runs once) |
| Awake | `CheckinScheduler::loop`, every 30 s | the same as Daily, plus the menu in front, no prompt open, Bluetooth never started this power cycle |

Stay awake (Awake & dev mode, `upd.awake` = 1) starts no automatic session:
Boot and Awake answer `stay-awake`, and the Fidget never sleeps, so no Daily
wake runs either. A manual check still works.
| Manual | `CheckinScheduler::checkNow()` (test CLI `cloud check`) | nothing: works with Auto-check off and at any battery level; after Bluetooth use the device restarts first (`bootcfg.bootcloud`) |

Boot, Daily and Awake never wait with WiFi on for the site's 60 s spacing
between check-ins: an answer that would need it is left for the next
session (`report-deferred`). They also give up as soon as the saved network
is reported absent, and the boot window's join budget is 4 s (see
`lib/CloudSync/README.md`).

## Stored settings

NVS namespace `upd` (the prompt's keys are under "Update prompt" below):

- `policy` (string): `never` turns automatic check-ins off; anything else,
  or no key, is `auto`.
- `interval_h` (u32): hours between automatic check-ins; 0, missing or more
  than a year reads as 24.
- `last_chk` (u32): written by `CloudSync` from the site's `server_time`
  (else a set local clock) after each successful check-in.
- `backoff_to` (u32): the site's own Retry-After backoff, also honoured.

The scheduler itself writes nothing to flash. Its backoff after failed
automatic attempts (1 h, 2 h, 4 h, 8 h, 16 h, then 24 h) and the local-clock
time of the last success live in RTC memory, which survives deep sleep and
is cleared by a power cycle or restart. Only real session results count: a
start refused because a serial transfer or probe is busy, or a session
cancelled (for example by a Music Player launch), records nothing.

The timing rules the device uses are pure functions in `CheckinPolicy`
(`timing()`, `applySchedule()`, `armDueSec()`) over a plain `Schedule`
(policy, saved WiFi, link, `interval_h`, `last_chk`, `backoff_to`) and the
RTC `Backoff`; `CheckinScheduler` only reads storage into a `Schedule`, and
the native tests drive the same functions.

## Elapsed time

The stored server time wins when the clock is set. After a clock reset (the
clock reads before 2020, or behind `last_chk`) the local-clock time of the
last success this power-on is used, else the clock's own count since reset
(it keeps running through deep sleep, so that is the time since power-on).

## Daily wake

There is no second wake timer. Before every deep sleep that keeps the
hourly battery timer, `armBeforeSleep()` (HAL hook) computes when the next
check-in is due and stores it as a clock value in RTC memory. The hourly
wake (`HAL.cpp` `timerWakeBatteryCheck`) compares the clock with it and
resleeps as before when it is not due, so the common wake opens no storage.
A due wake also reads the charge, skips the rest of hardware start-up (no
display, LEDs, audio or animation), and `runHeadless()` decides from the
stored settings and RTC state first; only a session that will run mounts
the filesystem and checks the stored link. It runs one session, re-arms,
and sleeps again with the same timer whatever happened.

A due wake that finds the battery below the floor holds the next look off
for 24 h (`kLowBatteryHoldSec`), so a low cell's hourly wakes stay RTC-only
instead of reading settings every hour. A successful check-in clears the
hold.

Pressing the wake button during a headless check-in stops it (up to 5 s
for WiFi to switch off) and restarts the device; a software restart is not
a timer wake, so the Fidget starts normally with its animation and menu.

The headless path has a wall-clock guard of 75 s (the scheduled session's
own budget is 60 s). Past it the session is cancelled, waited for up to
10 s, logged as `guard=fired`, counted as a failure, and the device sleeps
regardless: deep sleep powers the radio down and the next wake starts
clean. If the worker did not stop, the battery diary flush is skipped for
that sleep (the worker could still be writing to the filesystem).

## Boot result

A boot-window result that arrives while the animation is still playing is
kept for the menu: `takeBootResult()` returns it once (the popup itself
belongs to the update prompt work). A later result only reaches the status
bar, which `CloudSync::poll()` already posts. Either way the serial log
says `[checkin] boot-result=early|late`.

## Serial output

Every session prints one read-only line (never the credential):

    [checkin] reason=boot join_ms=3120 total_ms=4310 result=none err=none heap_min=... largest_min=... wifi=off

A scheduled session that does not start prints
`[checkin] reason=<boot|daily|awake> skip=<verdict> vbat_mv=.. soc=..`
(awake only when an overdue check is held back, and only when the reason
changes). A headless wake ends with
`[checkin] headless_ms=..` before the usual `[uvlo] ... verdict=resleep`.

## Bench hooks (test builds only)

- `cloud check`: the on-demand check through `checkNow()`.
- `cloud interval <hours>`: sets `upd.interval_h`.
- `cloud due <seconds>`: moves `upd.last_chk` so the next automatic check
  is due in that many seconds (needs a set clock; clears the RTC backoff).
- `cloud press`: the next headless check-in behaves as if the wake button
  were pressed during it (one-shot, `cftest.press`).
- `cloud guard <ms>`: headless wall-clock guard override (`0` = built-in),
  stored in the test-only namespace `cftest`.
- `cloud ssid absent|saved`: scheduled sessions look for a network that is
  not in range (`cftest.badssid`), to measure the absent-network bail
  without touching saved WiFi.
- `cloud btafterwifi allow|block`: RAM-only; lets the Music Player start
  after WiFi in the same power cycle, to measure it. Release builds always
  restart first.
- `btstat`: Bluetooth controller and host state with the internal heap.
- `pio run -e local_test_fastwake`: `local_test` with a 60 s battery timer
  wake instead of 3600 s. With `cloud due 90` and `sleep`, the second timer
  wake runs a headless check-in. For a power-profiler capture of a daily
  check-in, run this build on the profiler bench unit and log the serial
  port with a no-reset open.

## Update prompt

`PromptPolicy` is the pure rule set for the prompt and Settings > Updates
(host-tested in `test/test_upd_policy_prompt`; menu placement and row labels
in `test/test_core_updatemenu`, `pio test -e test_core`). The device glue is
`lib/UpdatePrompt`: it reads and writes NVS `upd`, opens the prompts
(`ModalPrompt`, long labels scroll) and runs two screens.

### Prompts

    Update 1.4.0 ready (cyberfidget.com)      App changes waiting
    > Install now                             > Get them now
      Remind me later                           Later
      Skip this version

- A firmware offer is shown when `upd.avail` holds a version that is newer
  than the running one and is not `upd.rej`. Versions are semantic versions
  (`MAJOR.MINOR.PATCH`, optional `-prerelease`, optional `+build`, at most
  31 characters), validated as a whole: anything else is never offered. A
  prerelease sorts below its final; build metadata does not count. An empty `avail` is no
  notification, not an error. The check-in says that a firmware manifest
  exists (`offered=fw`, `upd.fw_url`); the check-in worker then reads that
  manifest, runs the install gates and writes (or removes) `avail`
  (`UpdateSession::refreshOffer`, lib/OtaUpdate), in every check-in
  session including the scheduled ones. The bench can also use
  `upd offer`.
- A version whose update did not keep itself on this Fidget (`upd.fail_ver`,
  written by the previous image after the rollback) is not offered by the
  post-boot popup again; a manual check still shows it with "It did not
  finish last time". A newer version is offered as usual.
- Install now: on a Fidget allowed to install (`upd.unsig_ok`, set over USB
  serial with `upd allow-unsigned on` until updates are signed) it hands off
  to the update session: the `bootcfg` one-shot (`bootupd`, `updver`,
  `skipanim`) and a restart ("Restarting to update..."). Every other Fidget
  says "Installing on your Fidget is coming soon. Update it from the website
  for now." and stores nothing; the offer stays in the status bar.
- Remind me later (or no answer): stores nothing; the offer stays in the
  status bar and the next start-up or check offers it again.
- Skip this version: `upd.rej = <version>`. Only that exact version is
  suppressed; a newer one is offered again. Settings > Updates can unskip.
- App changes waiting (only when "Apply app changes automatically" is off):
  Get them now runs a check that applies them this once; Later leaves them
  waiting in the status bar. The check-in carries no count, so the title
  has none.

### When they appear

- After the start-up animation, once: the boot-window result
  (`CheckinScheduler::takeBootResult()`) and the stored offer. Auto-check Off
  silences this popup. A result that arrives after the animation only
  reaches the status bar. A restart that relaunches something (portal,
  Music Player, link, a check) shows no popup.
- After a manual check (root "Check for updates", Settings > Updates >
  Check now): the result screen, then the same prompts. With Auto-check Off
  it also says "Automatic check-ins are off. Remote changes wait for a
  manual check."
- A manual check after Bluetooth use says "Restarting to check...", sets
  `bootcfg.bootcloud` (plus `bootcfg.bootapply` when it came from "Get them
  now") and restarts (`CloudSync::runSession`, `PromptPolicy::restartForCheck`).
  The next start consumes both (`resumeAfterRestart`), runs the check as a
  recovery session with the same apply choice, and opens the Check for
  updates screen already watching that session (`checkEntry`): it shows
  that session's result once, even if it finished before the screen's first
  pass, and never starts a second check.

### Settings > Updates

Check now, Auto-check On/Off (`upd.policy` `auto`/`never`; turning it off
explains the choice), Check at start-up On/Off (`upd.boot_chk`; Off stops only
the boot session - the daily sleep check-in, a Fidget kept awake past its
interval, a manual check and dev mode are unaffected; changing it shows
"Checks for updates when you wake your Fidget. Turning it off avoids a short
restart when you open a new app."), Share battery data On/Off
(`upd.usage_share`, default off; turning it on explains what is sent and
that it can be turned off), Apply app changes automatically On/Off
(`upd.autoapply`, default on; it never installs firmware), Channel and Source
(shown only: `upd.chan`, default `stable`; `upd.src`, default
`cyberfidget.com`), Skip/Unskip the current version (Unskip when the offered
version is the skipped one, or when nothing is offered; a newer offer shows
Skip, which replaces the older skip), Link / Unlink this
Fidget (opens the Link screen), "Awake & dev mode: <mode>" (opens that
screen, below), and the status line the bar is showing (opens the Status
screen).

Keys the prompt work writes: `policy`, `boot_chk`, `usage_share`, `rej`, `autoapply`. It reads `avail`,
`src`, `chan`. "Forget" on a saved network (portal or Settings > Saved WiFi)
changes only `wificfg`, never `upd`.

## Awake & dev mode

`AwakePolicy` is the pure rule set (host-tested in `test/test_upd_policy_awake`);
the device glue is `lib/UpdatePrompt/AwakeMode` (the setting, the screen, the
exits, dev mode listening's start-up and relaunch, the Bluetooth prompt).

| Mode | Behavior |
|---|---|
| Off (default) | Sleeps after 60 s without use; daily check-in |
| Stay awake | Never sleeps on its own; Bluetooth works; no network |
| Dev mode | Stays awake and listens for sent apps (CloudSync's dev worker, `lib/CloudSync/README.md`); the Bluetooth memory is released at start-up exactly as the setup portal does |

Both awake modes are a latch in NVS `upd`: `awake` (u8: 0 off, 1 stay
awake, 2 dev mode) and `awake_stop` (u8: 0 "After 30 min without use",
1 "Until I stop it"). They end by themselves, and the stored mode is then Off:

- **After 30 min without use** (only that stop setting): use is a button
  press or an app delivered in dev mode.
- **48 h without a button press** (either stop setting): the safety net for
  a Fidget left on a desk or a booth table. Deliveries do not count. The
  clock is RAM only, so it restarts at every restart.
- **Battery floor** (either): VBAT under 3.6 V or charge under 20 % while
  not charging, held for 60 s. An unreadable gauge never counts as low (the
  runtime empty-battery guard still applies).

Stay awake ending leaves a status line ("Stay awake is off: battery low")
and normal sleep resumes. Dev mode ending shows "Dev mode is off" and the
reason, restarts (animation skipped), and says why in the status bar after
the restart (`bootcfg.awk_end`).

Changing the mode: entering or leaving Dev mode restarts (animation
skipped), and so does a new stop setting inside Dev mode (the listening
worker tells the site "dev" or "always" when it starts); Off <-> Stay awake
and a Stay awake stop change only save. When the write that ends a mode
fails twice, the Fidget does not restart into the same mode: it stops
listening and keeping awake and says "Could not turn Dev mode off. Restart
to try again."

**Restart loop**: three abnormal resets in a row (panic, watchdog, brownout)
while an awake mode is stored turn it Off before anything starts ("Dev mode
is off: it kept restarting"). The count lives in memory that survives a
reset (not a power cycle); a normal start clears it, and so does a clean
stretch (5 min of uptime, or dev mode's first successful check-in). Only this counter
survives a reset - the use, press and low-battery clocks restart with every
start. Bench: `awake crash <n>`.

**Screen** (Settings > Awake & dev mode, also opened from Settings >
Updates): a `< Mode >` selector (Left / Right, wraps) over five choices -
Off, Stay awake / Dev mode each with "After 30 min without use" or "Until I
stop it" on the line below - then the choice's description scrolling
(`ScrollLabel`), then "In use" or "Enter: use this". Enter applies, Back
leaves without changing anything.

**Bluetooth apps** (the Music Player is the only one): while dev mode
listens, the menu shows a small Bluetooth mark after the name, and choosing
it asks "Music uses Bluetooth. Restart without dev mode? Dev mode comes back
next restart." Restart sets `bootcfg.bootmusic` + `skipanim` and restarts;
that one power cycle has no listening and keeps Bluetooth; the next restart
listens again. Cancel stays on the menu.

**Visible state**: the status bar draws an eye left of the battery in both
awake modes; dev mode listening also shows the inverted WiFi glyph and "Dev
mode" (StatusKind::Listening). After three failed check-ins in a row the bar
says "Dev mode: not connected" instead (and "Dev mode: link this Fidget
first" / "Dev mode: no saved network" when listening cannot start). While
the menu is in front, the back LED breathes (blue, 4 s); any app owns the
LEDs and the breathing comes back when it ends.

**Delivered apps**: every delivery applies at once (whatever "Apply app
changes automatically" says) and is use.

**Which apps listening runs beside** (`AwakePolicy::listensDuring`, an
allow-list by manifest name): the menu, the boot animation, Status, Updates,
Check for updates, Awake & dev mode, Particle Sim and Snake - what was
measured to leave a network session at least 24 KB of internal heap. Every
other app pauses listening until it ends: the portal, Music Player and Link
need the radio or the worker, Voice Notes dipped to 13 KB. A delivered
(WASM) app is not on the list; listening continues beside it when the heap
allows (`AwakePolicy::listensBesideDeliveredApp`: internal free when it
opens >= 24 KB floor + 18 KB check-in cost + 8 KB app), and a check-in whose
trough falls below 24 KB while it runs pauses listening until it ends
(`[awake] pause=heap`). A send of the running app then relaunches it in
place, with no Back press. An app added to the firmware pauses listening until it is measured
and listed. Opening a paused app first shows "Pausing dev mode..." while
WiFi goes off; if the worker does not stop in time (a stuck network call),
the app is not opened beside it: a delivered app opens in a fresh start, a
built-in one waits and the bar says "Dev mode is busy. Try again in a
moment." A send made meanwhile arrives when
the person goes back to the menu; when it carries a new file for the app last
run this power cycle, that app opens again (through the restart below).
Anything else just appears in the menu.

**Opening a delivered app after the network was used** (any power cycle, any
mode): a delivered app's task needs a small internal-RAM block (4 KB stack
plus its control block) and its interpreter stack (128 KB) in PSRAM, so it
opens directly after a check-in and beside dev mode listening. Only when
either cannot be allocated does the app open through a restart: "Opening <app>...", `bootcfg.wasmid` (+
`wasmcat`, the menu category it was opened from) and `skipanim`, restart, and
the app starts straight after boot before anything uses the network. The
one-shot is removed before the app starts (a failing app never loops; an id
no longer in the menu falls back to the menu), counts as a one-shot boot (no
boot check-in), and Back returns to the category with the app selected
(`MenuManager::restoreAfterRestart`). `AppManager::switchToApp` and
`relaunchActive` decide it from `WasmFsApp::guestStackFits()`.

**Migration**: the earlier Settings > Updates "Dev mode" row stored `upd.dev`
(0/1/2) and `upd.dev_idle_min`. At start-up, with no `awake` key, `dev` 1
becomes Dev mode "After 30 min without use", 2 becomes Dev mode "Until I
stop it", anything else Off, and the new keys are written (`[awake] migrated
...`). The old keys are left in place, so an image that rolls back still
reads its own setting; once `awake` exists they are ignored. The idle
minutes are dropped: the idle stop is fixed at 30 min.

Check-ins carry `mode` (`normal` for Off and Stay awake, `dev` / `always`
for Dev mode after 30 min / until stopped), read from the stored setting.

Bench: `awake` verbs (`lib/SerialCli/README.md`), case
`test/bench/cases/t379-devmode-poll.json`.

Bench: `upd` (read-only list of every `upd` key) and `upd offer <version>`
(test builds; see `lib/SerialCli/README.md`), case
`test/bench/cases/t391-prompt-options.json`.
