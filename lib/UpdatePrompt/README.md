<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# UpdatePrompt

The device screens and prompts for updates, Awake & dev mode and saved WiFi.
This library is glue only: it reads and writes NVS, opens `ModalPrompt`
prompts and draws the screens. The rules live elsewhere and are
host-tested there:

| File | What it runs | Rules |
|---|---|---|
| `UpdatePrompt.*` | the post-boot update popup, root "Check for updates" (`APP_CHECK_UPDATES`), Settings > Updates (`APP_UPDATES`) | `lib/UpdatePolicy/PromptPolicy`, `CheckinPolicy` |
| `AwakeMode.*` | Settings > Awake & dev mode (`APP_AWAKE`), the stored mode and its stop rules, dev mode listening, the Bluetooth-app prompt | `lib/UpdatePolicy/AwakePolicy` |
| `SavedWifiScreen.*` | Settings > Saved WiFi (`APP_SAVED_WIFI`) | `lib/CloudSync/WifiList` (via `SavedWifi`) |

Behavior, stored keys and on-screen copy for the first two are described in
`lib/UpdatePolicy/README.md` (sections "Update prompt" and "Awake & dev
mode"); this file does not repeat them.

## Update prompt

- `armBootPopup()` (from `setup()` when the start-up animation plays) and
  `loop()` (every pass): once the menu first appears, shows the firmware
  offer and/or "App changes waiting" for the boot-window result. A version
  whose self-test already failed on this Fidget (`upd.fail_ver`) is held
  back from this popup.
- `checkBegin/Update/End`: the Check for updates screen. It runs a manual
  check (or a dev-mode poll while listening, or watches a check already
  running), shows one result line ("Update <version> ready", "App changes
  applied", "App changes waiting", "Your Fidget is up to date", "Could not
  check" plus a reason), then the same prompts. A held-back version is
  shown here with "It did not finish last time".
- `resumeCheck()`: after the restart a check needs following Bluetooth use,
  the screen shows that session's result once instead of starting another.
- `settingsBegin/Update/End`: Settings > Updates.

## Awake & dev mode

`beginBoot()` reads (and migrates) the setting at start-up and releases the
Bluetooth memory in a dev mode power cycle; `loop()` keeps the Fidget awake,
applies the stop rules and runs listening; `noteButton()` counts use;
`interceptSwitch()` lets `AppManager::switchToApp` ask before a Bluetooth app.
`screenBegin/Update/End` draw the `< Mode >` selector.

## Saved WiFi

A list of the saved networks by name, in the order they are tried (the
first shows "(first)" when more than one is saved), then a "Setup WiFi" row;
with nothing saved the list shows "Nothing saved yet". Enter on a network
opens a prompt with "Use this first", "Forget" and "Cancel" (no "Use this
first" for the network already tried first). Enter on "Setup WiFi" switches
to `APP_SETUP_WIFI`, the portal opening on its WiFi page. There is no text
entry on the device: networks are added in the portal. Back returns to the
menu. Each choice logs `[wifi] screen=first|forget ok=<0|1>`.

Storage (up to three networks in NVS `wificfg`, migration of the older
single-network keys) and the join order are in `lib/CloudSync/WifiList.h` and
`lib/CloudSync/SavedWifi.h`; the rules are host-tested in
`test/test_sync_wifilist`.

## Test-build hooks

`upd` and `upd offer <version>` (`UpdatePrompt::printState`, `injectOffer`)
and the `awake` verbs (`AwakeMode::cliCommand`); see `lib/SerialCli/README.md`.
