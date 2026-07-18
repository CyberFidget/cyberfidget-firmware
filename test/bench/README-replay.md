<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
# Serial test-case replay harness (T-194)

A Cyber Fidget bench test is **data, not a script**. A case is a JSON
list of steps driven over the T-191 serial tunnel; `cf_replay.py` runs
it, asserts, and captures - so cases are stored, diffable, replayable,
and readable by both humans and agents. Stop writing throwaway
`t183_drive.py`-style scripts; add a case here instead and the repo of
cases grows.

## Run

```
# Windows (pio's python has pyserial):
%USERPROFILE%\.platformio\penv\Scripts\python.exe cf_replay.py cases/t183-menu-visible.json --port COM30 --out runs/menu

# Linux (the HIL host will land here): ports are /dev/ttyUSB0 or /dev/ttyACM0
CF_BENCH_PORT=/dev/ttyUSB0 ~/.platformio/penv/bin/python cf_replay.py cases/t183-menu-visible.json
```

The runner is pure pyserial + os.path - cross-platform by
construction. The ONLY platform-specific bit is the port name; set
`CF_BENCH_PORT` (the knob the Linux HIL host sets) or pass `--port`.
`--out <dir>` saves screencaps (`.bin` = raw 1024-byte 1bpp) +
`result.json`. Exit code 0 = all asserts passed.

The runner embeds the bench session rules ONCE (reset dance +
alive-poll before trusting the CLI; drain before framed reads) so no
case re-invents them.

## Step verbs (the instruction set = the T-191 tunnel)

| do | fields | what it does |
|---|---|---|
| `reset` | - | DTR/RTS reboot, drain the boot banner |
| `wait_cli` | - | poll `version` until the CLI answers |
| `nav_to_menu` | - | tap Enter until `menutree` shows a built menu |
| `wait` | `ms` | sleep |
| `btn` | `index`,`action`(press/release/tap) | inject a button event (no GPIO) |
| `ferry` | `path`,`fixture` | fwrite/fwdata/fwcommit a file from `fixtures/` |
| `lapply_add` | `id`,`name`,`category`,`format`,`path`,`version` | add a manifest entry (blob apps use format `wasm` + a blobPath) |
| `lapply_remove` | `id` | remove a manifest entry |
| `fdelete` | `path` | delete a ferried file |
| `launch` | `id` | launch a builtin name/index, `menu`, or a manifest blob id |
| `screencap` | `save`,`review`(auto/human) | capture the OLED; `human` renders it inline as ASCII |

## Assertions (the analyze layer)

Structured captures are machine-assertable (auto green/red); the OLED
is either golden-matched or shown to a human oracle - no image-diff
engine.

| do | fields | passes when |
|---|---|---|
| `assert_menutree` | `has`,`not_has` | a menu row contains / omits the text |
| `assert_wasmstat` | `frames_gt`,`error`,`timeout_s` | frame count / error field match (`timeout_s` widens the reply window - a busy wasm app can starve the serial task past the 0.8s default) |
| `assert_syncinfo` | `contains[]` | syncinfo carries the substrings |

`screencap` with `review:"human"` is the deliberate escape hatch for
"a person watching the OLED is the oracle" (Sam, 2026-07-12) - the
frame is captured + rendered, a human judges. The browser player
(T-191 leg 4, Phase 2) makes that judging point-and-click.

## Adding a case

Copy a `cases/*.json`, edit the steps, drop any needed binary in
`fixtures/`. No runner code to touch. Keep cases small and named for
the ticket + AC they prove (e.g. `t183-ferried-app-runs.json`).

## Requires a test-CLI build

`launch`, `btn`, `wasmstat` need a `-DCF_TEST_CLI=1` firmware
(`pio run -e local_test`). `menutree`, `screencap`, `ferry`, `lapply`,
`syncinfo`, `lget` are always compiled.

Symptom of running a case against a non-test build: `launch`/`btn`
steps fail with `[err] unknown command: ...` and `assert_wasmstat`
reports `frames=-1 error='?'` (no reply at all). Check the boot
banner's build type before debugging the case.
