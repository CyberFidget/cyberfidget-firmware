# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""Home Remote portal hardware bench for a device running local_test.

Uses the existing bench's reset, alive, command and state handling. The real
access key is sent only through the portal and remains saved afterwards.
Only a short summary is written; raw serial and responses stay in memory for
the leak check. Requires Python 3 and pyserial.
"""

import argparse
import http.client
import json
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.request

from home_remote_bench import Dev as SerialDev, wait_state, leave_app, BTN_BACK, BTN_ENTER


results = []


def report(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + ("  -- " + detail if detail else ""), flush=True)


class Dev(SerialDev):
    """Keep receiving during browser requests and waits as well as commands."""

    def __init__(self, port, secret):
        super().__init__(port, secret)
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.read_failed = False
        self.reader = threading.Thread(target=self._receive, daemon=True)
        self.reader.start()

    def _receive(self):
        try:
            while not self.stopped.is_set():
                chunk = self.s.read(4096)
                if chunk:
                    with self.lock:
                        self.received += chunk
                        self.rxbuf += chunk
        except Exception:
            self.read_failed = True

    def _read_some(self):
        # The reader alone owns serial reads. The inherited wait loop takes
        # queued lines on its next iteration; drain keeps its time budget.
        time.sleep(0.01)
        return b""

    def _take_lines(self):
        with self.lock:
            super()._take_lines()

    def close(self):
        self.stopped.set()
        self.reader.join(timeout=2)
        super().close()


class BenchFailure(Exception):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Portal:
    def __init__(self, base):
        self.base = base
        self.received = bytearray()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, path, data=None, raw=None):
        payload = raw if raw is not None else (json.dumps(data).encode("ascii") if data is not None else None)
        req = urllib.request.Request(self.base + path, data=payload,
                                     headers={"Content-Type": "application/json", "Cache-Control": "no-cache"})
        try:
            try:
                response = self.opener.open(req, timeout=12)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                code = response.code
                version = "1.0" if response.version == 10 else "1.1"
                self.received += ("HTTP/%s %d %s\r\n" % (version, code, response.reason)).encode("utf-8")
                for name, value in response.headers.raw_items():
                    self.received += (name + ": " + value + "\r\n").encode("iso-8859-1")
                self.received += b"\r\n"
                chunks = []
                while True:
                    try:
                        chunk = response.read(4096)
                    except http.client.IncompleteRead as error:
                        self.received += error.partial
                        raise
                    if not chunk:
                        break
                    self.received += chunk
                    chunks.append(chunk)
                body = b"".join(chunks)
                parsed = json.loads(body)
                if not isinstance(parsed, dict):
                    raise BenchFailure()
                return code, parsed
        except Exception:
            # Never include a request, response, filename or exception text.
            raise BenchFailure("portal request failed") from None

    def state(self):
        return self.request("/api/homeremote")

    def start(self, url, key):
        return self.request("/api/homeremote/test", {"url": url, "key": key})

    def finish(self, timeout=16):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            code, data = self.request("/api/homeremote/test")
            if code != 200:
                return "request-failed"
            status = data.get("status")
            if status != "checking":
                return status
            time.sleep(0.5)
        return "timed-out"

    def check(self, url, key, expected):
        code, data = self.start(url, key)
        if code == 202:
            return self.finish() == expected
        return code == 200 and data.get("status") == expected


def require(name, ok):
    report(name, ok)
    if not ok:
        raise BenchFailure()


def normalized_url(value):
    value = value.strip(" \r").rstrip("/")
    if value[:7].lower() != "http://":
        return None
    return "http://" + value[7:]


def portal_in_front(dev, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if dev.alive(timeout=3):
            name = dev.cmd("app", ["[cmd] app.name="], timeout=2)
            if name == "[cmd] app.name=CyberFidget Portal":
                return True
        time.sleep(0.5)
    return False


def station_ip(dev, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = dev.cmd("net", ["[cmd] net.sta_ip="], timeout=2)
        match = re.fullmatch(r"\[cmd\] net\.sta_ip=(\d+\.\d+\.\d+\.\d+)", line or "")
        if match and match[1] != "0.0.0.0":
            return match[1]
        time.sleep(0.5)
    return None


def run(dev, args, key, portals):
    dev.reset()
    require("cli.alive", dev.alive())
    dev.drain(1)
    reply = dev.cmd("awake set stay until", ["[cmd] awake.set="])
    require("awake.stay", bool(reply and reply.startswith("[cmd] awake.set=stay")))
    require("settings.cleared", dev.cmd("hr clear", ["[cmd] hr.clear="]) == "[cmd] hr.clear=ok")
    reply = dev.cmd("hr show", ["[cmd] hr.url="])
    require("settings.key_unset", bool(reply and " token=unset " in reply))

    # A reboot on portal entry is expected when Bluetooth was already used.
    dev.cmd("launch CyberFidget Portal", ["[cmd] launch.ok="], timeout=3)
    require("portal.in_front", portal_in_front(dev))
    # Reboots reset the awake mode, so restore it before the longer checks.
    reply = dev.cmd("awake set stay until", ["[cmd] awake.set="])
    require("portal.awake", bool(reply and reply.startswith("[cmd] awake.set=stay")))
    address = station_ip(dev)
    require("portal.home_wifi_joined", address is not None)
    portal = Portal("http://" + address)
    portals.append(portal)
    code, state = portal.state()
    require("portal.key_unset", code == 200 and state.get("keySet") is False)

    bad_key = ("Abc123.-_" * 21)[:183]
    for name, url, value, want in (
            ("check.bad_key_rejected", args.ha_url, bad_key, "rejected"),
            ("check.wrong_host", "http://192.0.2.1:8123", key, "unreachable"),
            ("check.unknown_name", "http://cf-bench-nohost.local:8123", key, "unreachable"),
            ("check.no_key", args.ha_url, "", "no-key"),
            ("check.real_key", args.ha_url, key, "reachable")):
        started = time.monotonic()
        ok = portal.check(url, value, want)
        report(name, ok, "%.1fs" % (time.monotonic() - started))
    # A connection test must never save anything.
    code, state = portal.state()
    report("check.saved_nothing", code == 200 and state == {"url": "", "keySet": False}, str(state))

    code, saved = portal.request("/api/homeremote", {"url": args.ha_url, "key": key})
    require("save.accepted", code == 200 and saved == {"status": "saved", "keySet": True})
    expected = {"url": normalized_url(args.ha_url), "keySet": True}
    code, state = portal.state()
    require("save.readback", code == 200 and state == expected)
    bad_requests = [
        ("malformed", None, b'{"url":'),
        ("address", {"url": "ftp://x", "key": ""}, None),
        ("key", {"url": "http://should-not-be-saved:8123", "key": "has space"}, None),
    ]
    for name, data, raw in bad_requests:
        code, response = portal.request("/api/homeremote", data, raw)
        report("save.reject_" + name, code == 400 and response.get("error") ==
               ("invalid" if name == "malformed" else name))
        code, state = portal.state()
        report("save.preserved_after_" + name, code == 200 and state == expected)

    # A blank key must keep the saved value for both checks and saves.
    report("check.kept_key", portal.check(args.ha_url, "", "reachable"))
    code, saved = portal.request("/api/homeremote", {"url": args.ha_url, "key": ""})
    report("save.kept_key", code == 200 and saved.get("keySet") is True)
    first, first_data = portal.start("http://192.0.2.1:8123", "")
    second, second_data = portal.start(args.ha_url, "")
    if first == 202 and second == 409 and second_data.get("error") == "busy":
        report("check.busy", True)
    elif first == 202 and second == 202:
        report("check.busy", True, "timing made overlap untestable")
    else:
        report("check.busy", False)
    report("check.busy_finished", portal.finish() in ("reachable", "unreachable"))

    dev.tap(BTN_BACK)
    dev.tap(BTN_ENTER)
    time.sleep(1)
    require("exit.alive", dev.alive(timeout=25))
    reply = dev.cmd("hr show", ["[cmd] hr.url="])
    require("settings.persisted", bool(reply and reply.startswith("[cmd] hr.url=" + expected["url"] + " ")
                                       and " token=set " in reply))
    reply = dev.cmd("awake set stay until", ["[cmd] awake.set="])
    require("app.awake", bool(reply and reply.startswith("[cmd] awake.set=stay")))
    reply = dev.cmd("launch Home Remote", ["[cmd] launch.ok="])
    require("app.launched", bool(reply and reply.startswith("[cmd] launch.ok=")))
    state, _ = wait_state(dev, lambda s, _: s.get("screen") == "top" and
                          s.get("fav") == str(args.expect_favorites), 35)
    report("app.favorites", bool(state and state.get("screen") == "top" and
                                 state.get("fav") == str(args.expect_favorites)))
    report("app.back_to_menu", leave_app(dev))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--ha-url", required=True)
    parser.add_argument("--expect-favorites", type=int, default=6)
    parser.add_argument("--out", default="home_remote_portal_bench_out")
    args = parser.parse_args()
    try:
        with open(args.token_file, encoding="utf-8") as source:
            key = source.read().strip()
    except Exception:
        print("Could not read the access key file.")
        return 2
    if not 1 <= len(key) <= 300 or any(ord(c) <= 32 or ord(c) >= 127 for c in key):
        print("The access key file must hold one printable access key of 1 to 300 characters.")
        return 2
    if not normalized_url(args.ha_url):
        print("The Home Assistant address must start with http://.")
        return 2
    dev = None
    portals = []
    try:
        os.makedirs(args.out, exist_ok=True)
        dev = Dev(args.port, key)
        run(dev, args, key, portals)
    except BenchFailure:
        report("run.completed", False)
    except Exception:
        report("run.completed", False, "device or bench operation failed")
    finally:
        if dev:
            try:
                reply = dev.cmd("awake set off", ["[cmd] awake.set="])
                report("awake.off", bool(reply and reply.startswith("[cmd] awake.set=off")))
                dev.drain(1)
            except Exception:
                report("awake.off", False)
            finally:
                try:
                    dev.close()
                except Exception:
                    report("serial.closed", False)
            report("serial.reader", not dev.read_failed)
        needles = {key.encode("ascii"), key[:24].encode("ascii"), key[-24:].encode("ascii")}
        serial_bytes = bytes(dev.received) if dev else b""
        http_bytes = b"".join(bytes(p.received) for p in portals)
        for name, data in (("serial", serial_bytes), ("http", http_bytes)):
            hits = sum(data.count(needle) for needle in needles)
            report("leak." + name, hits == 0, "hits=%d" % hits)
    failed = sum(not ok for _, ok, _ in results)
    summary = ["%s %s%s" % ("PASS" if ok else "FAIL", name, " -- " + detail if detail else "")
               for name, ok, detail in results]
    summary.append("%d checks, %d failed" % (len(results), failed))
    try:
        with open(os.path.join(args.out, "summary.txt"), "w", encoding="utf-8") as output:
            output.write("\n".join(summary) + "\n")
    except Exception:
        print("FAIL summary.write -- could not write summary")
        return 1
    print(summary[-1])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
