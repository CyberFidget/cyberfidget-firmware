<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# Bench update site

A stand-in for the website's `update/firmware.php?manifest=1` / `?app=1`
that serves LOCAL builds, so the update session can be exercised on the
bench without a GitHub release. The manifest carries the same fields as the
site (version, size, sha256, url, hw, channel, source, release_id,
released_at) and the `?app=1` URL is bound to its release id and digest
(409 otherwise), as on the site.

`/<name>/api/device-checkin.php` answers any check-in with a firmware offer
and nothing else, so the check-in's own manifest read (which stores
`upd.avail`) can be exercised with a stand-in credential (`cloud token`).

It is not the site: it does not read GitHub, cache, throttle forks or check
`release-info.json` against the tag. Those are the website's own tests.

## Run

    set CF_OTA_RELEASES=C:\path\to\releases
    C:\php\php.exe -S 0.0.0.0:8297 router.php

Use a port the website's own tests do not pin (8297 on Sam's bench). The
Fidget reads a release through its test-build base: `cloud base
http://<host>:8297/<name>` selects `<releases>/<name>/`.

## Releases

`make_release.py` copies a build's app image (`.pio/build/<env>/firmware.bin`,
never the merged image) and writes `release-info.json`:

    python make_release.py %CF_OTA_RELEASES%\y --bin .pio\build\local_test\firmware.bin ^
        --version 1.3.4+abc1234.dirty --released-at 2026-09-24T11:00:00Z --release-id 2

The version must be exactly what the build reports (`version` on the serial
CLI), or the new image fails its self-test and the Fidget returns to the
old one - which is itself one of the cases. Test knobs (never sent to the
device): `--wrong-sha` (the manifest announces a digest the bytes do not
have), `--flip-offset N` (one byte is corrupted in transit), `--status 503`
(the site is not answering).

The cases that use it are in `test/bench/cases/t250-ota-ab.json`.
