# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2026 Dismo Industries LLC
#
# T-194: declarative serial test-case replay harness for the Cyber Fidget.
#
# A test case is DATA, not a script: a JSON list of steps over the T-191
# serial tunnel (menutree / btn / launch / ferry / lapply / screencap /
# wasmstat / lget / syncinfo). One format, two players - this headless
# runner (CI + agent bench verification) and, later, the browser chassis
# player (T-191 leg 4). Store cases in test/bench/cases/ and grow the repo
# instead of writing throwaway scripts.
#
# Usage:  python cf_replay.py cases/<case>.json [--port COM30] [--out dir]
#
# The runner embeds the bench session rules (reset dance + alive-poll,
# drain-before-framed-read) once, here, so no case ever re-invents them.
# Every capture is echoed as machine-parseable [cmd] text; screencaps are
# saved as a 1024-byte .bin plus an inline ASCII render an agent can read.

import argparse, base64, json, os, re, sys, time, zlib

try:
    import serial
except ImportError:
    print("pyserial required: run with pio's python (has pyserial)", file=sys.stderr)
    sys.exit(2)

FB_BYTES = 1024  # 128x64 / 8


class Tunnel:
    """The serial session + the T-191 verb primitives. All bench rules live
    here (see cyberfidget-hil README): open asserts DTR/RTS, so a reset
    dance + alive-poll is mandatory before the CLI is trustworthy."""

    def __init__(self, port, baud=921600):
        self.s = serial.Serial(port, baud, timeout=0.3)
        self.rx = b""

    def close(self):
        self.s.close()

    def reset(self):
        self.s.dtr = False
        self.s.rts = True
        time.sleep(0.1)
        self.s.rts = False
        time.sleep(0.05)
        self.rx = b""

    def drain(self, secs=1.0):
        end = time.time() + secs
        buf = self.rx
        self.rx = b""
        while time.time() < end:
            b = self.s.read(4096)
            if b:
                buf += b
                end = time.time() + 0.2
        return buf.decode("utf-8", "replace")

    def send(self, line):
        self.s.write(line.encode() + b"\n")

    def read_cmd(self, timeout=4.0):
        """Next [cmd]/[err] line; leaves trailing payload bytes in rx."""
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.rx:
                ln, self.rx = self.rx.split(b"\n", 1)
                t = ln.decode("utf-8", "replace").strip()
                if t.startswith("[cmd]") or t.startswith("[err]"):
                    return t
            c = self.s.read(4096)
            if c:
                self.rx += c
        return ""

    def wait_cli(self, timeout=8.0):
        end = time.time() + timeout
        buf = ""
        while time.time() < end:
            buf += self.drain(0.4)
            self.send("version")
            if "version=" in buf:
                for l in buf.splitlines():
                    if "version=" in l:
                        return l.strip()
        return None

    def cmd(self, line, wait=0.9):
        self.drain(0.1)
        self.send(line)
        return self.drain(wait)

    # --- composite primitives cases lean on ---
    def nav_to_menu(self, tries=10):
        for _ in range(tries):
            self.cmd("btn 5 tap", 0.4)
            mt = self.cmd("menutree", 0.8)
            if "roots=" in mt and "roots=0" not in mt:
                return mt
            time.sleep(0.9)
        return ""

    def ferry(self, path, data):
        self.send("fwrite %s %d %s" % (path, len(data), _crc(data)))
        if ".ok" not in self.read_cmd():
            return False, "fwrite rejected"
        off = 0
        while off < len(data):
            part = data[off:off + 4096]
            self.send("fwdata %d %d %s" % (off, len(part), _crc(part)))
            self.s.write(part)
            rr = self.read_cmd()
            if "[err]" in rr:
                return False, "fwdata: " + rr
            off += 4096
        self.send("fwcommit")
        r = self.read_cmd(timeout=10)
        return (".ok" in r), r

    def lapply(self, ops_obj):
        doc = json.dumps(ops_obj)
        self.send("lapply %d %s" % (len(doc), _crc(doc.encode())))
        self.s.write(doc.encode())
        return self.drain(1.5).strip()

    def screencap(self):
        cap = self.cmd("screencap", 1.5)
        m = re.search(r"b64=([A-Za-z0-9+/=]+)", cap)
        if not m:
            return None
        return base64.b64decode(m.group(1))


def _crc(b):
    return format(zlib.crc32(b) & 0xFFFFFFFF, "08x")


def _ascii_fb(raw):
    """Render the 128x64 1bpp column-page framebuffer as coarse ASCII so an
    agent can read a screencap inline (2x2 downsample -> 64x32 chars)."""
    if not raw or len(raw) < FB_BYTES:
        return "(no framebuffer)"
    rows = []
    for y in range(0, 64, 2):
        line = []
        for x in range(0, 128, 2):
            bit = (raw[(y // 8) * 128 + x] >> (y % 8)) & 1
            line.append("#" if bit else " ")
        rows.append("".join(line).rstrip())
    return "\n".join(rows)


class Result:
    def __init__(self, name):
        self.name = name
        self.steps = []
        self.ok = True

    def step(self, do, ok, detail=""):
        self.steps.append({"do": do, "ok": ok, "detail": detail})
        if not ok:
            self.ok = False
        mark = "PASS" if ok else "FAIL"
        print("  [%s] %-16s %s" % (mark, do, detail[:120]))

    def note(self, do, detail=""):
        self.steps.append({"do": do, "ok": None, "detail": detail})
        print("  [ .. ] %-16s %s" % (do, detail[:120]))


def run_case(case, port, outdir):
    name = case.get("name", "unnamed")
    fixdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
    print("=== CASE: %s  (port %s) ===" % (name, port))
    res = Result(name)
    t = Tunnel(port)
    caps = {}
    try:
        for st in case.get("steps", []):
            do = st.get("do")
            if do == "reset":
                t.reset(); time.sleep(0.3); t.drain(2.5)
                res.note("reset")
            elif do == "wait_cli":
                v = t.wait_cli()
                res.step("wait_cli", v is not None, v or "no CLI")
            elif do == "nav_to_menu":
                mt = t.nav_to_menu()
                res.step("nav_to_menu", bool(mt), "menu built" if mt else "menu never built")
            elif do == "wait":
                time.sleep(st.get("ms", 500) / 1000.0)
                res.note("wait", "%dms" % st.get("ms", 500))
            elif do == "btn":
                out = t.cmd("btn %d %s" % (st["index"], st.get("action", "tap")), 0.5)
                res.note("btn", out.strip().splitlines()[-1] if out.strip() else "")
            elif do == "ferry":
                data = open(os.path.join(fixdir, st["fixture"]), "rb").read()
                ok, detail = t.ferry(st["path"], data)
                res.step("ferry", ok, detail)
            elif do == "lapply_add":
                entry = {k: st[k] for k in ("id", "name", "category", "format", "version", "abi") if k in st}
                entry.setdefault("category", "Tools")
                entry.setdefault("format", "wasm")
                if "path" in st:
                    entry["blobPath"] = st["path"]
                out = t.lapply({"ops": [{"op": "add", "entry": entry}]})
                res.step("lapply_add", ".ok" in out, out[:100])
            elif do == "lapply_remove":
                out = t.lapply({"ops": [{"op": "remove", "id": st["id"]}]})
                res.step("lapply_remove", ".ok" in out, out[:100])
            elif do == "fdelete":
                out = t.cmd("fdelete %s" % st["path"], 0.6)
                res.note("fdelete", out.strip().splitlines()[-1] if out.strip() else "")
            elif do == "launch":
                out = t.cmd("launch %s" % st["id"], 1.5)
                last = [l for l in out.splitlines() if "launch" in l or "[err]" in l]
                # A refusal (e.g. abi_unsupported) is a SUCCESSFUL assertion, not
                # a failed launch: expect_error asserts the launch.error carrier
                # instead of launch.ok.
                exp_err = st.get("expect_error")
                if exp_err:
                    ok = any(("launch.error" in l and exp_err in l) for l in last)
                else:
                    ok = any(".ok" in l for l in last)
                res.step("launch", ok, " | ".join(last)[:120])
            elif do == "screencap":
                raw = t.screencap()
                key = st.get("save", "cap")
                caps[key] = raw
                if raw and outdir:
                    open(os.path.join(outdir, "%s.bin" % key), "wb").write(raw)
                lit = sum(bin(x).count("1") for x in raw) if raw else 0
                res.note("screencap", "%s: %d bytes, %d lit px, review=%s"
                         % (key, len(raw or b""), lit, st.get("review", "auto")))
                if st.get("review") == "human" and raw:
                    print(_ascii_fb(raw))
            elif do == "assert_menutree":
                mt = t.cmd("menutree", 1.2)
                has = st.get("has")
                nhas = st.get("not_has")
                ok = True; d = ""
                if has is not None:
                    found = any(has in l for l in mt.splitlines())
                    ok = ok and found; d += "has '%s'=%s " % (has, found)
                if nhas is not None:
                    absent = not any(nhas in l for l in mt.splitlines())
                    ok = ok and absent; d += "not_has '%s'=%s " % (nhas, absent)
                res.step("assert_menutree", ok, d)
            elif do == "assert_wasmstat":
                ws = t.cmd("wasmstat", 0.8)
                fr = re.search(r"frames=(\d+)", ws)
                # error runs to end of line (it is the last printf field and
                # a human string with spaces), so capture the rest and
                # substring-match rather than exact - "malformed Wasm binary".
                er = re.search(r"error=(.+?)\s*$", ws, re.M)
                frames = int(fr.group(1)) if fr else -1
                err = er.group(1).strip() if er else "?"
                ok = True; d = "frames=%d error='%s'" % (frames, err)
                if "frames_gt" in st:
                    ok = ok and frames > st["frames_gt"]
                if "error" in st:
                    ok = ok and st["error"] in err
                res.step("assert_wasmstat", ok, d)
            elif do == "assert_syncinfo":
                si = t.cmd("syncinfo", 1.0)
                ok = all(str(v) in si for v in st.get("contains", []))
                res.step("assert_syncinfo", ok, ("contains %s" % st.get("contains", [])))
            else:
                res.step(do or "?", False, "unknown step verb")
    finally:
        t.close()

    print("=== RESULT: %s ===" % ("PASS" if res.ok else "FAIL"))
    if outdir:
        with open(os.path.join(outdir, "result.json"), "w") as f:
            json.dump({"name": name, "ok": res.ok, "steps": res.steps}, f, indent=2)
    return res.ok


def main(argv):
    ap = argparse.ArgumentParser(description="Replay a Cyber Fidget serial test case (T-194).")
    ap.add_argument("case", help="path to a case JSON file")
    # Port is the one platform-specific bit: Windows "COM30", Linux
    # "/dev/ttyUSB0" or "/dev/ttyACM0". CF_BENCH_PORT is the portable knob
    # the (eventually Linux) HIL host sets; --port overrides. Everything
    # else here is pure pyserial + os.path and runs unchanged on Linux.
    ap.add_argument("--port", default=os.environ.get("CF_BENCH_PORT"),
                    help="serial port (or set CF_BENCH_PORT); e.g. COM30 or /dev/ttyUSB0")
    ap.add_argument("--out", default=None, help="dir for screencaps + result.json")
    args = ap.parse_args(argv)
    if not args.port:
        ap.error("no serial port: pass --port or set CF_BENCH_PORT "
                 "(Windows COMx, Linux /dev/ttyUSB0)")
    case = json.load(open(args.case))
    outdir = args.out
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    ok = run_case(case, args.port, outdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
