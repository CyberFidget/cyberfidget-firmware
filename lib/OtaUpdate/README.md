<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# OtaUpdate

Installing an update on the Fidget over WiFi, with two app slots and an
automatic return to the previous image. This library is the pure rule set
(no Arduino, no flash, no network; host-tested in `test/test_ota_update`,
`pio test -e test_ota_manifest`). The device glue is
`lib/CloudSync/UpdateSession` (it lives in CloudSync so the library graph
gains no cycle through AppManager).

## Flow

1. **Offer.** When a check-in answers with a firmware offer, the check-in
   worker - in every session, scheduled ones (boot window, daily wake,
   awake) included, when the remaining budget covers one more call and its
   usual reserve; it never waits for this - reads the update site's manifest
   (`update/firmware.php?manifest=1&channel=<chan>`), runs every gate below
   and stores `upd.avail` (or removes it when a gate refuses). Nothing is
   downloaded. The prompt (lib/UpdatePrompt) offers `avail` only when it is
   newer than the running version and not skipped.
2. **Install now** arms the `bootcfg` one-shot (`bootupd`, `updver` = the
   version chosen, `skipanim`) and restarts - but only on a Fidget with
   `upd.unsig_ok` set (below). Every other Fidget keeps the "update from the
   website" message.
3. **Update session** (that boot, right after hardware start-up, before the
   menu, the check-in scheduler or anything that could start Bluetooth):
   the one-shot is removed first, so a crash never loops into another
   session. `bootcfg.updfail` is set until the session hands over to a new
   image. WiFi station only; a 300 s wall-clock budget; the loop task is
   subscribed to the task watchdog with a 30 s period (longer than any one
   blocking call, 10 s). It fetches the manifest, gates it with the chosen
   version as `wanted`, then streams the image from the manifest's `url`
   (a path on the same site) into the other app slot
   (`esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` / `esp_ota_write`) while
   hashing every byte with mbedTLS SHA-256. `esp_https_ota` is not used: the
   pinned SDK does not allow plain HTTP there, and the bench site is HTTP.
4. **Order of the last steps** (`Installer::complete`): byte count, then the
   digest against the manifest (mismatch: `esp_ota_abort`, the boot slot is
   never touched), then `esp_ota_end`, then the pending record
   `upd.pend_img` is stored, then `esp_ota_set_boot_partition`, then a
   restart. Any failure before the last step leaves the running slot
   selected.
5. **Self-test.** `src/main.cpp` defines `verifyRollbackLater()` to return
   true, so the Arduino core no longer marks a new image valid before
   `setup()`. The first line of `AppManager::setup()` notes whether the
   running image is pending verification; if so it sets the task-watchdog
   period to 15 s (`kPendingWdtMs`) and subscribes the loop task. After
   hardware start-up the self-test checks, in order: the record parses;
   hardware start-up returned within 8 s (shorter than the watchdog period,
   so a slow start is caught here, a stuck one by the watchdog); LittleFS
   mounts WITHOUT formatting (`LittleFS.begin(false)`); the running version
   matches the record (below); the running slot's first `size` bytes hash to
   the record's SHA-256; the checks took at most 20 s. Any failure:
   `esp_ota_mark_app_invalid_rollback_and_reboot()`.
6. **Kept after the first frame.** Passing the checks does not keep the
   image yet: the rest of `setup()` (menu, filesystem, apps) and one full
   main-loop pass that ran the active app must also finish, still under the
   watchdog, within 45 s of the first line of `setup()` (`loopTick`,
   `confirmStep`). Then `esp_ota_mark_app_valid_cancel_rollback()`, the
   watchdog goes back to the SDK's 5 s without the loop task, the record,
   `upd.fail_ver` and `bootcfg.updfail` are removed, freshness is stored, and
   the bar says "Updated to X". A crash, a hang (the watchdog), a power cut
   or the 45 s budget running out before that leaves the image pending, and
   the bootloader (or the rollback call) returns to the previous slot.
7. **Back on the previous image**, a leftover record that is not the
   running image (or `bootcfg.updfail`) posts "The update did not finish.
   Nothing changed." to the bar once and is cleared. When the new image did
   start (a record, no `updfail`), its version is stored in `upd.fail_ver`:
   automatic offers (the post-boot popup) skip that version; a manual check
   still shows it, saying "It did not finish last time". A newer version is
   offered as usual; a successful update clears `fail_ver`.

`LoadoutStore::begin` also refuses to format while the running image is
pending (a second guard; the self-test runs before it anyway).

## Gates (`gate()`, before any download)

In order: hardware range (`hw.min_rev`/`max_rev` against the board
revision, OtaManifest); source (the manifest's `source` must be the
Fidget's `upd.src`: empty, `official` and `cyberfidget.com` are the official
source); acknowledgment of a non-official source (none can be chosen yet,
so fork manifests are always refused); channel (a stable Fidget takes
stable releases only, an rc Fidget takes both); freshness (a release whose
`released_at` is older than the newest already installed for this
(source, channel) is refused; the same release again is allowed); size
(at most the 3,342,336-byte slot); the version chosen (exact text).

Freshness lives in `upd.seen_<8 hex>` (u32 epoch seconds): the hex is an
FNV-1a hash of `<source>|<channel>`, so every key is 13 characters. It is
written only after a new image passes its self-test and only moves forward.

## Version match

The self-test compares the record's version (the manifest's `version`)
with the running version. Equal text matches. A manifest version without
build metadata (`1.4.0`, what releases carry: the tag without `v`) also
matches a running version that adds it (`1.4.0+abc1234`). A manifest version
with build metadata must match exactly (the bench serves full versions).
The image hash check makes the identity exact either way.

## Source order

The update site first. `fallbackAllowed()` lets only a host that could not
answer (no connection, timeout, 5xx) fall through; a refusal (4xx, a bad
manifest, a gate) never does. The GitHub fallback itself is NOT implemented:
the session logs `[update] manifest host=github outcome=unavailable` and
stops. GitHub serves a release list, not this manifest, so the device would
need its own release reader (and GitHub's redirects to its asset host); that
is a follow-up.

## Who may install (until updates are signed)

`upd.unsig_ok` (bool) gates Install now and the session. It is set only by
the serial command `upd allow-unsigned on|off`, compiled into every build:
holding the USB cable is the proof, and nothing on the network can write it.

## Stored data

A rollback returns the app, not the data: both images share NVS and the
filesystem, so anything a pending image writes must stay readable by the
previous one. The self-test itself writes nothing (removing a test-build
fault flag in `cftest` is the only exception). Between passing its checks
and being kept, the pending image runs its ordinary start-up (menu build,
battery diary, link check), which writes the same kinds of data the
previous image writes; the one destructive step there, formatting the
filesystem, is refused while pending (`LoadoutStore::begin`). The boot
check-in does not run in that start-up (the update's restart is a one-shot
start). Keys added in `upd`: `pend_img` (string `<sha256> <size>
<released_at> <seen key> <version>`), `unsig_ok` (bool), `seen_<hex>`
(u32), `fail_ver` (string). In `bootcfg`: `bootupd`, `updver`, `updfail`.
Older images ignore all of them. Every key is at most 15 characters
(checked in the native tests).

## Serial

Every build: `upd slot` (running/boot/other slot and their states,
`pend_img`, `unsig_ok`), `upd allow-unsigned on|off`. Test builds: `upd
install <version>` (the Install now hand-off for that version),
`upd fault <none|crash|hang|version|mount|session-hang>` (a one-shot fault:
`session-hang` for the next session, the rest for the next pending image),
`upd seen-clear` (forget the stored freshness).

Device lines: `[update] boot slot=.. state=.. other=.. other_state=..` at
every start (not on timer wakes), `[update] session=start|end ...`,
`[update] manifest ...`, `[update] progress=NN bytes=..` every 10 %,
`[update] install=<result>`, `[update] selftest=pass|fail reason=..`,
`[update] confirmed ms=..` (kept after the first frame),
`[update] rollback reason=..`, `[update] result=updated|did-not-finish ...`,
`[update] offer=stored|withdrawn|unchanged ...`.

## Bench

`test/bench/cases/t250-ota-ab.json` with the local site in
`test/bench/fixtures/ota-site/`.
