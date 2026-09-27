# Notes: interrupted reset finishes at start-up; portal password without a space

## Shipped

1. **Reset to factory survives a power cut.** `eraseAndRestart` now writes a
   "reset in progress" mark (NVS namespace `freset`, key `busy`) after every
   shutdown check has passed and immediately before the LittleFS format. The
   NVS erase that ends the reset clears it. `FactoryReset::finishIfInterrupted()`
   runs in `AppManager::setup` right after `UpdateSession::finishBoot()` - before
   the update-session request, the timer check-in, `DeviceIdentity`, bootcfg,
   LittleFS mount, and any radio. If the mark is set it shows
   "Finishing reset...", formats LittleFS, erases NVS and restarts.
   - Decision logic: `FactoryResetPolicy::bootStep(markSet, imagePending)` ->
     Normal / Finish / Wait. A pending (probation) image never erases, matching
     the reset's own `PendingImage` refusal; the mark waits for a later start.
   - Mark stays on a runtime format failure (apps may be half erased; the next
     start finishes). Mark write failure is logged and the reset proceeds as it
     did before the mark existed.
   - Loop guard: an RTC-noinit try counter (reset on power-on and on each new
     reset); from the third crashed try since power-on the finish skips the
     format and still erases NVS.
   - **Bug found by the bench and fixed:** `LittleFS.format()` asserts
     (`esp_littlefs.c:474 partition_label`) when `LittleFS.begin()` never ran
     in that boot - the first build boot-looped. `LoadoutStore::formatForFactoryReset`
     now calls `LittleFS.begin(false); LittleFS.end();` first when unmounted.
   - Test builds only: `reset factory confirm hold` waits 10 s after the format
     (`[reset] factory=formatted hold_ms=10000`) so the bench can cut power.
2. **Portal password shown as 8 digits, no space.** Removed
   `PortalPassword::grouped()`; the portal screen draws the password string
   directly (same 16 px font, centred at x=64). Native test changed: the
   `"0000 0001"` grouped assertion is replaced by "exactly 8 characters, all
   digits" (documented in the test).

## Files touched

- `lib/UpdatePolicy/FactoryResetPolicy.h` (BootStep, bootStep)
- `lib/UpdatePrompt/FactoryReset.cpp`, `FactoryReset.h`, `README.md`
- `lib/LoadoutManifest/LoadoutStore.cpp` (format when unmounted)
- `lib/AppManager/AppManager.cpp` (early-boot call)
- `lib/SerialCli/SerialCli.cpp`, `README.md` (`reset factory confirm hold`)
- `lib/WebPortalApp/PortalPassword.h`, `WebPortalApp.cpp`
- `test/test_upd_policy_factory_reset/test_factory_reset.cpp` (+3 tests)
- `test/test_webportal_shellstamp/test_shellstamp.cpp` (changed assertion)

## Tests run

- Native: `test_upd_policy` 84/84, `test_webportal` 16/16, `test_core` 120/120.
- `pio run -e local_test` SUCCESS (37 min); `pio run -e local` SUCCESS (31 min).
- Both builds are `1.3.3+13d7821.dirty` (built from the uncommitted tree).

## Bench evidence

- HIL-B (COM36, CP2102N `E4ADE838...`, PPK2-powered), local_test
  `1.3.3+13d7821.dirty`: saved dummy WiFi (`wifi.count=1`) + wrote
  `/apps/bench_reset.bin` -> `reset factory confirm hold` -> `marked`,
  `formatted` -> PPK output off 3 s -> on. Boot: `[reset] factory=finishing`,
  `[reset] factory=done`, restart; next boot has no `finishing` (mark gone).
  After: `wifi.count=0`, `fstat.absent=/apps/bench_reset.bin`,
  `lget.present=0` (default built-in list). Normal `reset factory confirm`
  then: `marked`, `done`, clean boot without `finishing`, same empty state.
  Banner `[boot] fw=1.3.3+13d7821.dirty` == `version=1.3.3+13d7821.dirty`.
- HIL-A (COM30, MAC `00:4b:12:a3:0b:50`), local_test same version, banner ==
  `version`: portal launched, `screencap` shows 8 digits, no space, x=30..96,
  fully on screen. Saved to
  `cyberfidget-planning/handovers/2026-09-26-ota-release-prep-artifacts/shots/portal-password.png`.

## Not verified

- The Wait path (mark + pending image) and the loop guard's skip-format path
  are host/logic-only; not exercised on hardware.
- Timer-wake finish (no screen drawn) not exercised.
- `E task_wdt: esp_task_wdt_reset(763): task not found` lines print during
  the runtime LittleFS format; not investigated (likely pre-existing).
- Bench mishap: my first bench script imported
  `test/bench/sync_protocol_bench.py`, which runs its bench at import, so the
  full sync bench ran once against HIL-A on its previous firmware (25/28; the
  3 failures were the bad expected-version arg and a free-space check). It
  cleans up after itself; the persisted manifest remains.
