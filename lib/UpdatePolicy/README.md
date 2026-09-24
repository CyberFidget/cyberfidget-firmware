<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# UpdatePolicy

When the Fidget checks in with the site. `CheckinPolicy` is the pure rule
set (host-tested in `test/test_upd_policy_checkin`, `pio test -e
test_upd_policy`); `CheckinScheduler` (in `lib/CloudSync`, so the
library graph gains no new cycle through `AppManager`) is the device glue
that reads stored settings, the battery and the radio state, asks the policy, and starts
`CloudSync` sessions.

## Sessions

| Session | Started from | Needs |
|---|---|---|
| Boot | first `AppManager::loop` pass while the start-up animation plays | Auto-check on, saved WiFi, a link, a normal start (not a restart that relaunches the portal, Music Player, linking, a check or a skipped animation), at least an hour since the last check-in (`kBootMinGapSec`: every button wake is a start; only a set clock with a stored check-in time, or a success earlier this power-on, counts as a recent check-in, so a cold start after a reset or a flat battery always gets its boot check), no backoff running, VBAT >= 3.6 V and SOC >= 20 % |
| Daily | an hourly battery timer wake whose check-in is due | the same, plus the interval passed, no backoff running; one session per due wake (the headless path runs once) |
| Awake | `CheckinScheduler::loop`, every 30 s | the same as Daily, plus the menu in front, no prompt open, Bluetooth never started this power cycle |
| Manual | `CheckinScheduler::checkNow()` (test CLI `cloud check`) | nothing: works with Auto-check off and at any battery level; after Bluetooth use the device restarts first (`bootcfg.bootcloud`) |

Boot, Daily and Awake never wait with WiFi on for the site's 60 s spacing
between check-ins: an answer that would need it is left for the next
session (`report-deferred`). They also give up as soon as the saved network
is reported absent, and the boot window's join budget is 4 s (see
`lib/CloudSync/README.md`).

## Stored settings

NVS namespace `upd` (shared with the update prompt work):

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
