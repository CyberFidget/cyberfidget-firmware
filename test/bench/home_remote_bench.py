# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""Home Remote hardware bench: a Cyber Fidget running a `local_test` build,
a Home Assistant server on the same network, and its REST API as the
cross-check.

Steps (each prints PASS/FAIL; the exit code is 0 only when all pass):

  1. Reset dance, alive-poll `version`, `awake set stay until`.
  2. Configure with `hr url` and `hr token` / `hr token+` (the key is sent
     in pieces: it is longer than one serial line). `hr show` says set.
  3. `launch Home Remote`; the favorites list appears with the expected
     number of rows (`hr state`).
  4. Long-hold Enter on the toggle light's row: Home Assistant's state flips.
  5. Open the colour light, select R, tap Right: Home Assistant's rgb_color
     follows the device's R value.
  6. Back out of the app: `net` reports WiFi off.
  7. A wrong key shows the "not accepted" screen; Back leaves; the real key
     is restored.
  8. Every serial byte received is searched for the key: 0 hits.

A long hold is `btn <i> press`, `btn <i> held`, `btn <i> release` (the pin
scan does not time an injected press). Row lookups first Back up to the
app's top list.

Manual checks this script cannot make (LEDs and sleep):
  - With "Mirror on LEDs" on, open a lit colour light: LEDs 1-3 (front)
    show its colour, dimmed; LED 0 (back) stays dark. Turn the light off:
    dark. Back to a list: dark.
  - Leave a lit light screen untouched with Stay awake off: at 5 minutes
    the LEDs go dark, and about a minute later the device sleeps with the
    LEDs still dark.

The exact template text sent to Home Assistant can be written out without
a device: HR_DUMP_TEMPLATES=<dir> pio test -e test_home_remote (raw Jinja
.j2 and the request body .json, ready to POST to /api/template).

Screen captures (PNG) and a transcript (the key masked, should it ever
appear) go to --out. Lights the run changed are put back through the REST
API at the end. `awake set off` is always sent before closing.

The key is read from --token-file and never printed: commands that carry
it are logged as `hr token <N chars>`.

Optional --speaker media_player.<name> opens a speaker from favorites or
Rooms, checks that opening preserves its volume, taps Right for one percent,
then Left to restore it (within half a percent for display rounding). Start
below 100% with volume setting supported. Playback is untouched unless
--speaker-allow-playback is passed; that check needs play and pause support
and a speaker already playing or paused. The original volume is restored
in cleanup if this scenario changed it.

Run with pio's python (has pyserial):

  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe test\\bench\\home_remote_bench.py ^
      --port COM30 --ha-url http://192.168.1.156 --token-file <path> --out <dir>

Serial-session rules embedded here (see cyberfidget-hil/README.md): the
reset dance after open, polling `version` until the CLI answers, and stay
awake for the session (restored to off at the end).
"""

import argparse
import base64
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib

import serial

BAUD = 921600
BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_BACK, BTN_ENTER = 0, 1, 2, 3, 4, 5
TOKEN_CHUNK = 100   # well under the device's 160-byte line

results = []


def report(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + ("  -- " + detail if detail else ""), flush=True)


class Dev:
    """Line-oriented serial session. Every received byte is kept in
    `received` for the key search; sent lines are logged with the key
    masked."""

    def __init__(self, port, secret):
        self.s = serial.Serial(port, BAUD, timeout=0.1)
        self.rxbuf = b""
        self.queue = []          # received lines not yet taken by wait_for
        self.received = bytearray()
        self.log = []
        self.secret = secret

    def close(self):
        self.s.close()

    def reset(self):
        # ESP32 auto-reset: DTR low, pulse RTS (EN).
        self.s.dtr = False
        self.s.rts = True
        time.sleep(0.1)
        self.s.rts = False
        time.sleep(0.05)

    def _read_some(self):
        chunk = self.s.read(4096)
        if chunk:
            self.received += chunk
            self.rxbuf += chunk
        return chunk

    def drain(self, secs=0.3):
        end = time.time() + secs
        while time.time() < end:
            if self._read_some():
                end = time.time() + 0.1
        self._take_lines()
        self.queue.clear()

    def _take_lines(self):
        """Moves every whole received line into the queue (and the log)."""
        while b"\n" in self.rxbuf:
            line, self.rxbuf = self.rxbuf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").strip()
            if t:
                self.log.append("< " + t)
                self.queue.append(t)

    def send(self, line, shown=None):
        self.log.append("> " + (shown if shown is not None else line))
        self.s.write(line.encode() + b"\n")

    def _pop_match(self, prefixes):
        for i, t in enumerate(self.queue):
            if any(t.startswith(p) for p in prefixes):
                del self.queue[i]
                return t
        return None

    def wait_for(self, prefixes, timeout=4.0):
        """The first queued (or arriving) line starting with any prefix, or
        None. Other lines stay queued for a later wait_for."""
        if isinstance(prefixes, str):
            prefixes = (prefixes,)
        end = time.time() + timeout
        while True:
            self._take_lines()
            t = self._pop_match(prefixes)
            if t is not None or time.time() >= end:
                return t
            self._read_some()

    def cmd(self, line, prefixes, timeout=4.0, shown=None):
        # Earlier lines (late tap releases, log lines) are not this reply.
        self._take_lines()
        self.queue.clear()
        self.send(line, shown)
        return self.wait_for(tuple(prefixes) + ("[err]",), timeout)

    def alive(self, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            self.send("version")
            if self.wait_for("[cmd] version=", 1.0):
                return True
        return False

    def tap(self, idx):
        r = self.cmd("btn %d tap" % idx, ["[cmd] btn.tap="])
        time.sleep(0.25)   # the release comes 120 ms later
        return r

    def hold(self, idx, secs=0.3):
        """A long hold: an injected press is not timed by the pin scan, so
        the Held event is injected (`btn <i> held`) before the release."""
        self.cmd("btn %d press" % idx, ["[cmd] btn.press="])
        time.sleep(secs)
        r = self.cmd("btn %d held" % idx, ["[cmd] btn.held="])
        time.sleep(0.2)
        self.cmd("btn %d release" % idx, ["[cmd] btn.release="])
        time.sleep(0.2)
        return r is not None and r.startswith("[cmd] btn.held=")

    def state(self):
        """`hr state` as dicts: (state, light-or-speaker-details-or-None)."""
        line = self.cmd("hr state", ["[cmd] hr.state="])
        if not line or not line.startswith("[cmd] hr.state="):
            return None, None
        st = parse_fields(line[len("[cmd] hr.state="):], first="screen")
        light = None
        if st.get("screen") in ("light", "effects"):
            l2 = self.wait_for("[cmd] hr.light=", 1.0)
            if l2:
                light = parse_fields(l2[len("[cmd] hr.light="):], first="entity")
        elif st.get("screen") == "speaker":
            l2 = self.wait_for("[cmd] hr.speaker=", 1.0)
            if l2:
                light = parse_fields(l2[len("[cmd] hr.speaker="):], first="entity")
        return st, light


def parse_fields(text, first):
    """'top msg=x label=Table Lamp' -> {'screen':'top','msg':'x','label':'Table Lamp'}.
    `label` is last and may hold spaces."""
    out = {}
    head, _, rest = text.partition(" ")
    out[first] = head
    label = None
    if " label=" in " " + rest:
        idx = (" " + rest).index(" label=")
        label = (" " + rest)[idx + len(" label="):]
        rest = (" " + rest)[:idx]
    for tok in rest.split():
        k, _, v = tok.partition("=")
        out[k] = v
    if label is not None:
        out["label"] = label
    return out


# ---- screen captures --------------------------------------------------------

def write_png(path, fb, scale=3):
    """fb: 1024 bytes, 8 pages x 128 columns, LSB = top pixel of the page."""
    w, h = 128, 64
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            on = (fb[(y // 8) * w + x] >> (y % 8)) & 1
            row += bytes([255 if on else 0]) * scale
        for _ in range(scale):
            rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def screencap(dev, out_dir, name):
    line = dev.cmd("screencap", ["[cmd] screencap "], timeout=4.0)
    if not line or "b64=" not in line:
        print("  (screencap failed: %s)" % (line or "no reply"))
        return None
    fb = base64.b64decode(line.split("b64=", 1)[1])
    path = os.path.join(out_dir, name + ".png")
    write_png(path, fb)
    print("  screen -> " + path)
    return path


# ---- Home Assistant REST (the cross-check) ----------------------------------

class HA:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token

    def _req(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token)
        req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                return r.status, json.loads(r.read().decode() or "null")
        except urllib.error.HTTPError as e:
            return e.code, None
        except Exception as e:   # noqa: BLE001 - reported, never the key
            return 0, str(type(e).__name__)

    def state(self, entity):
        code, body = self._req("GET", "/api/states/" + entity)
        return body if code == 200 else None

    def call(self, domain, service, data):
        code, _ = self._req("POST", "/api/services/%s/%s" % (domain, service), data)
        return code

    def restore(self, saved):
        """Puts a light back to a state read earlier."""
        if not saved:
            return
        entity = saved["entity_id"]
        if saved.get("state") != "on":
            self.call("light", "turn_off", {"entity_id": entity})
            return
        data = {"entity_id": entity}
        attrs = saved.get("attributes", {})
        if attrs.get("brightness") is not None:
            data["brightness"] = attrs["brightness"]
        if attrs.get("color_mode") in ("rgb", "hs", "xy", "rgbw", "rgbww") and attrs.get("rgb_color"):
            data["rgb_color"] = attrs["rgb_color"]
        elif attrs.get("color_temp_kelvin"):
            data["color_temp_kelvin"] = attrs["color_temp_kelvin"]
        self.call("light", "turn_on", data)


# ---- steps ------------------------------------------------------------------

def set_token(dev, token):
    parts = [token[i:i + TOKEN_CHUNK] for i in range(0, len(token), TOKEN_CHUNK)]
    reply = None
    for i, part in enumerate(parts):
        verb = "hr token" if i == 0 else "hr token+"
        reply = dev.cmd("%s %s" % (verb, part), ["[cmd] hr.token="],
                        shown="%s <%d chars>" % (verb, len(part)))
        if not reply or not reply.startswith("[cmd] hr.token=saved"):
            return False, reply
    return reply == "[cmd] hr.token=saved chars=%d" % len(token), reply


def wait_state(dev, pred, timeout):
    end = time.time() + timeout
    st = light = None
    while time.time() < end:
        st, light = dev.state()
        if st and pred(st, light):
            return st, light
        time.sleep(0.5)
    return st, light


def go_top(dev, tries=6):
    """Back up to the app's top list (never Back from it: that leaves)."""
    for _ in range(tries):
        st, _ = dev.state()
        if st and st.get("screen") == "top":
            return True
        if not st or st.get("screen") == "message":
            return False
        dev.tap(BTN_BACK)
        time.sleep(0.4)
    return False


def select_row(dev, matches, max_steps=40):
    for _ in range(max_steps):
        st, light = dev.state()
        if st and matches(st.get("label", "")):
            return st, light
        dev.tap(BTN_DOWN)
    return None, None


def leave_app(dev, timeout=10.0):
    """Back until the menu is in front."""
    end = time.time() + timeout
    while time.time() < end:
        name = dev.cmd("app", ["[cmd] app.name="], timeout=1.5)
        if name and name != "[cmd] app.name=Home Remote":
            return True
        dev.tap(BTN_BACK)
        time.sleep(0.5)
    return False


def wifi_off(dev, timeout=10.0):
    end = time.time() + timeout
    mode = None
    while time.time() < end:
        mode = dev.cmd("net", ["[cmd] net.mode="])
        if mode == "[cmd] net.mode=0":
            return True, mode
        time.sleep(0.5)
    return False, mode


def open_speaker(dev, entity):
    """Search favorites, then every room, checking the opened speaker's id."""

    def scan_list():
        st, _ = dev.state()
        for _ in range(int((st or {}).get("rows", "0"))):
            st, _ = dev.state()
            if st and st.get("id") == entity:
                dev.tap(BTN_ENTER)
                opened, speaker = wait_state(dev, lambda s, l: s.get("screen") == "speaker", 3)
                if speaker and speaker.get("entity") == entity:
                    return True
                if opened and opened.get("screen") in ("speaker", "light"):
                    dev.tap(BTN_BACK)
                    time.sleep(0.8)
            dev.tap(BTN_DOWN)
        return False

    if not go_top(dev):
        return False
    if scan_list():
        return True
    if not go_top(dev):
        return False
    st = None
    for _ in range(35):
        st, _ = dev.state()
        if st and st.get("label") == "Rooms" and st.get("id") == "-":
            break
        dev.tap(BTN_DOWN)
    else:
        st = None
    if not st:
        return False
    dev.tap(BTN_ENTER)
    st, _ = wait_state(dev, lambda s, l: s.get("screen") == "rooms" and s.get("label") != "Loading...", 10)
    if not st or st.get("screen") != "rooms" or st.get("label") == "No rooms found":
        return False
    for _ in range(int(st.get("rows", "0"))):
        dev.tap(BTN_ENTER)
        opened, _ = wait_state(dev, lambda s, l: s.get("screen") == "room" and s.get("label") != "Loading...", 10)
        if opened and opened.get("screen") == "room":
            if scan_list():
                return True
            dev.tap(BTN_BACK)
        dev.tap(BTN_DOWN)
    return False


def wait_speaker_volume(ha, entity, expected, tolerance=0.002, timeout=7):
    end = time.time() + timeout
    got = None
    while time.time() < end:
        state = ha.state(entity)
        got = (state or {}).get("attributes", {}).get("volume_level")
        if got is not None and abs(got - expected) <= tolerance:
            return True, got
        time.sleep(0.3)
    return False, got


def speaker_scenario(dev, ha, args, saved, changed):
    entity = args.speaker
    report("speaker.opened", open_speaker(dev, entity))
    st, speaker = dev.state()
    if not st or st.get("screen") != "speaker" or not speaker or speaker.get("entity") != entity:
        return
    time.sleep(2.0)   # opening plus at least one poll must leave volume unchanged
    baseline = saved.get("attributes", {}).get("volume_level")
    fresh = ha.state(entity)
    got = (fresh or {}).get("attributes", {}).get("volume_level")
    report("speaker.open_preserved_volume", baseline is not None and got == baseline,
           "before=%s after=%s" % (baseline, got))
    if baseline is None:
        return
    st, speaker = select_row(dev, lambda label: label == "Volume", max_steps=5)
    if not st or not speaker or speaker.get("known") != "1":
        report("speaker.volume_supported", False)
        return
    pct = int(speaker["volume"])
    report("speaker.volume_can_rise", pct < 100, "volume=%d%%" % pct)
    if pct >= 100:
        return
    changed["volume"] = True   # cleanup restores even if an assertion fails
    dev.tap(BTN_RIGHT)
    expected = (pct + 1) / 100
    ok, got = wait_speaker_volume(ha, entity, expected)
    report("speaker.right_raised_volume", ok and got > baseline,
           "expected=%.2f got=%s" % (expected, got))
    dev.tap(BTN_LEFT)
    ok, got = wait_speaker_volume(ha, entity, pct / 100)
    report("speaker.left_restored_volume", ok and abs(got - baseline) <= 0.0051,
           "before=%s restored=%s" % (baseline, got))
    screencap(dev, args.out, "speaker_volume")
    if not args.speaker_allow_playback:
        return
    before = ha.state(entity)
    features = (before or {}).get("attributes", {}).get("supported_features", 0)
    state = (before or {}).get("state")
    if state not in ("playing", "paused") or features & (1 | 16384) != (1 | 16384):
        report("speaker.playback_supported", False, "needs play/pause and an existing playing or paused track")
        return
    st, _ = select_row(dev, lambda label: label == "Play/Pause", max_steps=5)
    report("speaker.playback_row", st is not None)
    if st:
        changed["playback"] = state
        dev.tap(BTN_ENTER)
        expected_state = "paused" if state == "playing" else "playing"
        end = time.time() + 7
        after = None
        while time.time() < end:
            after = (ha.state(entity) or {}).get("state")
            if after == expected_state:
                break
            time.sleep(0.3)
        report("speaker.playback_toggled", after == expected_state, "%s -> %s" % (state, after))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--ha-url", required=True, help="e.g. http://192.168.1.156")
    ap.add_argument("--token-file", required=True, help="file holding the long-lived access key")
    ap.add_argument("--out", default="home_remote_bench_out")
    ap.add_argument("--expect-favorites", type=int, default=3)
    ap.add_argument("--toggle-entity", default="light.table_lamp")
    ap.add_argument("--color-entity", default="light.table_lamp")
    ap.add_argument("--skip-bad-token", action="store_true")
    ap.add_argument("--speaker", help="optional speaker to test; start below full volume")
    ap.add_argument("--speaker-allow-playback", action="store_true", help="also exercise play/pause for --speaker")
    args = ap.parse_args()
    if args.speaker and not args.speaker.startswith("media_player."):
        ap.error("--speaker must name a media_player speaker")
    if args.speaker_allow_playback and not args.speaker:
        ap.error("--speaker-allow-playback requires --speaker")

    with open(args.token_file, "r", encoding="utf-8") as f:
        token = f.read().strip()
    if not token or " " in token:
        print("The token file is empty or holds spaces.")
        return 2
    os.makedirs(args.out, exist_ok=True)
    ha = HA(args.ha_url, token)

    saved = {e: ha.state(e) for e in {args.toggle_entity, args.color_entity}}
    for e, st in saved.items():
        report("ha.reachable." + e, st is not None, "state=%s" % (st or {}).get("state"))
    if not all(saved.values()):
        return 1
    saved_speaker = ha.state(args.speaker) if args.speaker else None
    speaker_changed = {}
    if args.speaker:
        report("speaker.reachable", saved_speaker is not None)
        if not saved_speaker:
            return 1

    dev = Dev(args.port, token)
    try:
        dev.reset()
        report("cli.alive", dev.alive())
        dev.drain(1.0)
        r = dev.cmd("awake set stay until", ["[cmd] awake.set="])
        report("awake.stay", r is not None and r.startswith("[cmd] awake.set=stay"), r or "")

        # ---- 2. configure ---------------------------------------------------
        r = dev.cmd("hr url " + args.ha_url, ["[cmd] hr.url="])
        report("hr.url", r == "[cmd] hr.url=" + args.ha_url.rstrip("/"), r or "")
        ok, r = set_token(dev, token)
        report("hr.token", ok, (r or "").replace(token, "<KEY>"))
        r = dev.cmd("hr show", ["[cmd] hr.url="])
        report("hr.show", r is not None and " token=set" in r, r or "")
        # The mirror setting is saved, so whatever an earlier session left is
        # not a default: set it each way, read it back, and leave it on.
        for want in ("off", "on"):
            r = dev.cmd("hr mirror " + want, ["[cmd] hr.mirror="])
            report("hr.mirror_" + want, r == "[cmd] hr.mirror=" + want, r or "")
            r = dev.cmd("hr show", ["[cmd] hr.url="])
            fields = parse_fields((r or "")[len("[cmd] hr.url="):], first="url")
            report("hr.show_mirror_" + want, fields.get("mirror") == want, r or "")

        # ---- 3. favorites ---------------------------------------------------
        r = dev.cmd("launch Home Remote", ["[cmd] launch.ok="])
        report("launch", r is not None and r.startswith("[cmd] launch.ok="), r or "")
        st, _ = wait_state(dev, lambda s, l: s.get("screen") != "message" or
                           s.get("msg") not in ("joining", "none"), 30)
        report("favorites.shown", st is not None and st.get("screen") == "top" and
               int(st.get("fav", "-1")) == args.expect_favorites, str(st))
        screencap(dev, args.out, "01_favorites")

        # ---- 4. long-hold toggles ------------------------------------------
        name = (saved[args.toggle_entity].get("attributes", {}).get("friendly_name") or "").strip()
        go_top(dev)
        st, _ = select_row(dev, lambda label: name and label.startswith(name[:20]))
        report("toggle.row_found", st is not None, "label=%s" % name)
        if st:
            before = ha.state(args.toggle_entity)["state"]
            report("toggle.held_injected", dev.hold(BTN_ENTER))
            time.sleep(0.5)
            after = None
            end = time.time() + 6
            while time.time() < end:
                after = ha.state(args.toggle_entity)["state"]
                if after != before:
                    break
                time.sleep(0.5)
            st2, _ = dev.state()
            report("toggle.ha_flipped", after != before, "%s -> %s" % (before, after))
            report("toggle.stayed_on_list", st2 is not None and st2.get("screen") == "top", str(st2))
            screencap(dev, args.out, "02_after_long_hold")

        # ---- 5. R change ----------------------------------------------------
        cname = (saved[args.color_entity].get("attributes", {}).get("friendly_name") or "").strip()
        report("light.back_to_top", go_top(dev))
        st, _ = select_row(dev, lambda label: cname and label.startswith(cname[:20]))
        if st:
            dev.tap(BTN_ENTER)
            time.sleep(1.5)   # opened, and one reading in
            st, light = dev.state()
            report("light.opened", st is not None and st.get("screen") == "light", str(st))
            screencap(dev, args.out, "03_light_open")
            st, light = select_row(dev, lambda label: label == "R", max_steps=8)
            report("light.r_row", st is not None, str(st))
            if st and light:
                r0 = int(light["rgb"].split(",")[0])
                key = BTN_RIGHT if r0 <= 250 else BTN_LEFT
                for _ in range(5):
                    dev.tap(key)
                time.sleep(3.0)   # sent (at most 5 per second) and settled
                _, light = dev.state()
                want = int(light["rgb"].split(",")[0]) if light else -1
                got = None
                end = time.time() + 6
                while time.time() < end:
                    hs = ha.state(args.color_entity)
                    rgb = (hs or {}).get("attributes", {}).get("rgb_color")
                    got = rgb[0] if rgb else None
                    if got is not None and abs(got - want) <= 3:
                        break
                    time.sleep(0.5)
                report("light.r_changed_on_device", want != r0, "%d -> %d" % (r0, want))
                report("light.r_in_ha", got is not None and abs(got - want) <= 3,
                       "device=%d ha=%s" % (want, got))
                screencap(dev, args.out, "04_after_r_change")
        else:
            report("light.row_found", False, "label=%s" % cname)

        if args.speaker:
            speaker_scenario(dev, ha, args, saved_speaker, speaker_changed)

        # ---- 6. leave: WiFi off --------------------------------------------
        report("exit.back_to_menu", leave_app(dev))
        ok, mode = wifi_off(dev)
        report("exit.wifi_off", ok, mode or "")

        # ---- 7. wrong key ---------------------------------------------------
        if not args.skip_bad_token:
            r = dev.cmd("hr token bench-wrong-key-0000", ["[cmd] hr.token="],
                        shown="hr token <wrong key>")
            report("badkey.saved", r == "[cmd] hr.token=saved chars=%d" % len("bench-wrong-key-0000"), r or "")
            dev.cmd("launch Home Remote", ["[cmd] launch.ok="])
            st, _ = wait_state(dev, lambda s, l: s.get("msg") in ("rejected", "unreachable", "bad-reply",
                                                                  "join-failed"), 30)
            report("badkey.rejected_screen", st is not None and st.get("msg") == "rejected", str(st))
            screencap(dev, args.out, "05_key_rejected")
            report("badkey.back_exits", leave_app(dev))
            ok, mode = wifi_off(dev)
            report("badkey.wifi_off", ok, mode or "")
            ok, r = set_token(dev, token)
            report("badkey.real_key_restored", ok, (r or "").replace(token, "<KEY>"))
    finally:
        try:
            dev.cmd("awake set off", ["[cmd] awake.set="])
            dev.drain(1.0)
        finally:
            # Close the app before REST cleanup so a pending device send cannot
            # undo the original volume we restore here.
            if speaker_changed:
                leave_app(dev)
                if speaker_changed.get("volume"):
                    ha.call("media_player", "volume_set", {
                        "entity_id": args.speaker,
                        "volume_level": saved_speaker["attributes"]["volume_level"],
                    })
                if speaker_changed.get("playback"):
                    service = "media_play" if speaker_changed["playback"] == "playing" else "media_pause"
                    ha.call("media_player", service, {"entity_id": args.speaker})
            for st in saved.values():
                ha.restore(st)
            # ---- 8. the key never came back over serial --------------------
            text = bytes(dev.received).decode("utf-8", "replace")
            needles = {token, token[:24], token[-24:]}
            hits = sum(text.count(n) for n in needles if n)
            report("token.never_printed", hits == 0, "hits=%d bytes=%d" % (hits, len(dev.received)))
            with open(os.path.join(args.out, "transcript.log"), "w", encoding="utf-8") as f:
                for line in dev.log:
                    for n in needles:
                        if n:
                            line = line.replace(n, "<KEY>")
                    f.write(line + "\n")
            dev.close()

    with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as f:
        json.dump([{"name": n, "ok": ok, "detail": d.replace(token, "<KEY>")} for n, ok, d in results],
                  f, indent=2)
    failed = [n for n, ok, _ in results if not ok]
    print("\n%d checks, %d failed%s" % (len(results), len(failed),
                                      (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
