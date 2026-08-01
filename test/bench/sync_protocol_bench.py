# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
#
"""Sync-protocol hardware bench against the Cyber Fidget on COM30.

Drives the serial verbs end-to-end: version ritual, syncinfo, write/list/stat/
read-back comparison, corruption rejection, confinement, nested paths, lapply
and reboot persistence, timeout/hostage fix, CRLF tolerance, deep-nesting
rejection.
Leaves the device clean (test blobs deleted, manifest restored).
"""
import sys, time, zlib, json
import serial

PORT = __import__("os").environ.get("CF_BENCH_PORT", "COM30")
BAUD = 921600
results = []

def report(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ("  -- " + detail if detail else ""))

class Dev:
    def __init__(self):
        self.s = serial.Serial(PORT, BAUD, timeout=0.25)
        self.rxbuf = b""  # carry-over so payload bytes after a header line are never lost
    def close(self):
        self.s.close()
    def reset(self):
        # ESP32 auto-reset: pulse RTS (EN) low with DTR high state dance
        self.s.dtr = False
        self.s.rts = True
        time.sleep(0.1)
        self.s.rts = False
        time.sleep(0.05)
    def drain(self, secs=0.5):
        end = time.time() + secs
        buf = self.rxbuf
        self.rxbuf = b""
        while time.time() < end:
            b = self.s.read(4096)
            if b:
                buf += b
                end = time.time() + 0.2
        return buf.decode("utf-8", "replace")
    def send_line(self, line, term=b"\n"):
        self.s.write(line.encode() + term)
    def send_bytes(self, b):
        self.s.write(b)
    def read_lines(self, want_prefixes=("[cmd]", "[err]"), timeout=5.0, n=1):
        """Collect up to n reply lines starting with the given prefixes.
        Unconsumed bytes (e.g. a payload following a header line) stay in rxbuf."""
        out = []
        end = time.time() + timeout
        while time.time() < end and len(out) < n:
            while b"\n" in self.rxbuf and len(out) < n:
                line, self.rxbuf = self.rxbuf.split(b"\n", 1)
                t = line.decode("utf-8", "replace").strip()
                if any(t.startswith(p) for p in want_prefixes):
                    out.append(t)
            if len(out) >= n:
                break
            chunk = self.s.read(4096)
            if chunk:
                self.rxbuf += chunk
        return out
    def read_payload_reply(self, timeout=5.0):
        """Read a header line then a length-framed payload (lget or fread)."""
        hdr = self.read_lines(n=1, timeout=timeout)
        if not hdr:
            return None, None
        h = hdr[0]
        length = None
        for tok in h.split():
            if tok.startswith("len="):
                length = int(tok[4:])
        if length is None or length == 0:
            return h, (b"" if length == 0 else None)
        data = self.rxbuf[:length]
        self.rxbuf = self.rxbuf[length:]
        end = time.time() + timeout
        while len(data) < length and time.time() < end:
            b = self.s.read(length - len(data))
            if b:
                data += b
        return h, data
    def read_until(self, marker, timeout=5.0, max_lines=66):
        """Collect reply lines through the first line containing marker."""
        out = []
        end = time.time() + timeout
        while time.time() < end and len(out) < max_lines:
            line = self.read_lines(n=1, timeout=max(0.1, end - time.time()))
            if not line:
                break
            out.extend(line)
            if marker in line[0]:
                break
        return out

def crc(b):
    return format(zlib.crc32(b) & 0xFFFFFFFF, "08x")

def write_file(d, path, data, corrupt_chunk=False, term=b"\n"):
    """Full fwrite/fwdata/fwcommit flow. Returns (ok_line_or_None, transcript)."""
    d.send_line(f"fwrite {path} {len(data)} {crc(data)}", term)
    r = d.read_lines(n=1)
    if not r or ".ok" not in r[0]:
        return None, r
    chunk = 4096
    off = 0
    while off < len(data):
        part = data[off:off+chunk]
        send = part
        if corrupt_chunk and off == 0:
            send = bytes([part[0] ^ 0xFF]) + part[1:]
        d.send_line(f"fwdata {off} {len(part)} {crc(part)}", term)
        d.send_bytes(send)
        rr = d.read_lines(n=1)
        if not rr:
            return None, ["<no fwdata reply>"]
        if "[err]" in rr[0]:
            if corrupt_chunk and off == 0:
                # expected NAK; resend clean
                d.send_line(f"fwdata {off} {len(part)} {crc(part)}", term)
                d.send_bytes(part)
                rr = d.read_lines(n=1)
                if not rr or "[err]" in rr[0]:
                    return None, rr
            else:
                return None, rr
        off += chunk
    d.send_line("fwcommit", term)
    r = d.read_lines(n=1, timeout=10)
    return (r[0] if r and ".ok" in r[0] else None), r

def read_file(d, path, size):
    """Read a file in advertised-size chunks and verify every reply CRC."""
    out = bytearray()
    transcript = []
    while len(out) < size:
        want = min(4096, size - len(out))
        d.send_line(f"fread {path} {len(out)} {want}")
        header, payload = d.read_payload_reply(timeout=6)
        transcript.append(header or "<no fread header>")
        if not header or ".ok" not in header or payload is None:
            return None, transcript
        chunk_crc = None
        for tok in header.split():
            if tok.startswith("crc="):
                chunk_crc = tok[4:]
        if len(payload) != want or chunk_crc != crc(payload):
            return None, transcript
        out.extend(payload)
    return bytes(out), transcript

def main():
    d = Dev()
    try:
        # --- 0. reset + boot banner / version ritual
        d.reset()
        banner = d.drain(3.5)
        d.send_line("version")
        vout = d.drain(1.5)
        fw = ""
        for t in (banner + vout).splitlines():
            if "fw=" in t:
                fw = t.strip()
        report("boot banner + version reachable", "fw=" in (banner + vout), fw[:120])
        expected = sys.argv[1] if len(sys.argv) > 1 else ""
        if expected:
            report("version matches build summary", expected in banner + vout,
                   f"expected {expected}")

        # --- 1. syncinfo
        d.send_line("syncinfo")
        lines = d.read_lines(n=3, timeout=4)
        ok = any("fs_total" in l for l in lines) and any("manifest" in l for l in lines) and any(".fw=" in l for l in lines)
        report("syncinfo three report lines", ok, " | ".join(lines))
        fs_free0 = 0
        for l in lines:
            for tok in l.split():
                if tok.startswith("fs_free="):
                    fs_free0 = int(tok.split("=")[1])

        # --- 2. write -> list -> stat -> read -> compare (2.5 chunks)
        data = bytes((i * 7 + 13) & 0xFF for i in range(10240))
        okline, tr = write_file(d, "/apps/bench_t.bin", data)
        report("read-back case: write 10KB", bool(okline), okline or str(tr))

        d.send_line("flist /apps")
        lines = d.read_until("flist.done=", timeout=6)
        listed = any("flist.entry=bench_t.bin size=10240" in line for line in lines)
        complete = any("flist.done=" in line and "truncated=0" in line for line in lines)
        report("read-back case: list finds complete entry", listed and complete,
               " | ".join(lines))

        d.send_line("fstat /apps/bench_t.bin")
        lines = d.read_lines(n=1, timeout=10)
        stat_ok = bool(lines) and ".ok" in lines[0] and \
            f"size={len(data)}" in lines[0] and f"crc={crc(data)}" in lines[0]
        report("read-back case: stat size and CRC match", stat_ok,
               lines[0] if lines else "")

        readback, tr = read_file(d, "/apps/bench_t.bin", len(data))
        report("read-back case: fread bytes compare", readback == data,
               " | ".join(tr))

        # --- 3. corrupted chunk NAK + clean resend succeeds
        okline, tr = write_file(d, "/apps/bench_c.bin", data[:5000], corrupt_chunk=True)
        report("corrupted chunk NAKed then clean resend commits", bool(okline), okline or str(tr))

        # --- 4. confinement
        d.send_line("fwrite /loadout.json 4 00000000")
        r = d.read_lines(n=1)
        report("fwrite /loadout.json rejected", bool(r) and r[0].startswith("[err]"), r[0] if r else "")
        d.send_line("fwrite /etc/pw 4 00000000")
        r = d.read_lines(n=1)
        report("fwrite outside roots rejected", bool(r) and r[0].startswith("[err]"), r[0] if r else "")
        d.send_line("fwrite /apps/../boot.bin 4 00000000")
        r = d.read_lines(n=1)
        report("traversal rejected", bool(r) and r[0].startswith("[err]"), r[0] if r else "")

        # --- 5. nested path (mkdir walk)
        okline, tr = write_file(d, "/apps/sub/nested.dat", b"NESTED-PATH-TEST")
        report("nested confined path commits (mkdir walk)", bool(okline), okline or str(tr))

        # --- 6. CRLF tolerance: full flow with \r\n terminators
        okline, tr = write_file(d, "/apps/bench_crlf.bin", b"CRLF" * 600, term=b"\r\n")
        report("CRLF-terminated flow no desync", bool(okline), okline or str(tr))
        d.send_line("syncinfo", b"\r\n")
        lines = d.read_lines(n=3, timeout=4)
        report("session alive after CRLF flow", any("fs_total" in l for l in lines))

        # --- 7. lget baseline
        h, payload = d.read_payload_reply(timeout=0.1)  # flush stray
        d.send_line("lget")
        h, payload = d.read_payload_reply(timeout=6)
        base_manifest = None
        if payload:
            try:
                base_manifest = json.loads(payload.decode("utf-8", "replace"))
            except Exception:
                pass
        report("lget returns manifest", h is not None and "lget" in (h or ""),
               (h or "") + (f" ({len(payload)} bytes json ok={base_manifest is not None})" if payload else " present=0"))
        identities_ok = base_manifest is not None and all(
            not entry.get("id", "").startswith("APP_") and
            (entry.get("format") == "builtin" or
             (bool(entry.get("format")) and bool(entry.get("blobPath"))))
            for entry in base_manifest.get("entries", []))
        report("lget manifest identities are canonical", identities_ok)

        # --- 8. lapply add + reboot persistence + rollback of a bad doc
        ops = json.dumps({"ops": [{"op": "add", "entry": {
            "id": "APP_BENCH_T", "name": "Bench Test", "category": "Examples",
            "format": "blob", "blobPath": "/apps/bench_t.bin", "version": "0.0", "abi": "1"}}]}).encode()
        d.send_line(f"lapply {len(ops)} {crc(ops)}")
        d.send_bytes(ops)
        r = d.read_lines(n=1, timeout=8)
        report("lapply add accepted", bool(r) and ".ok" in r[0], r[0] if r else "")

        bad = b'{"ops":[{"op":"add","entry":{"x":' + b"[" * 500 + b"]" * 500 + b"}}]}"
        d.send_line(f"lapply {len(bad)} {crc(bad)}")
        d.send_bytes(bad)
        r = d.read_lines(n=1, timeout=8)
        report("deeply nested lapply rejected cleanly", bool(r) and r[0].startswith("[err]"), r[0] if r else "")
        d.send_line("syncinfo")
        lines = d.read_lines(n=3)
        report("device alive after deep-nest reject", any("fs_total" in l for l in lines))

        # reboot, verify persistence. A fixed post-reset delay races the boot:
        # commands sent before the CLI is up are silently eaten (verified on
        # hardware 2026-07-11 - the entry HAD persisted but this read missed
        # it). Poll until the CLI answers, then query.
        d.reset()
        d.drain(4.5)
        for _ in range(15):
            d.s.reset_input_buffer()
            d.rxbuf = b""
            d.send_line("version")
            if any("version=" in l for l in d.read_lines(n=1, timeout=1.5)):
                break
            time.sleep(1.0)
        d.s.reset_input_buffer()
        d.rxbuf = b""
        d.send_line("lget")
        h, payload = d.read_payload_reply(timeout=6)
        persisted = payload is not None and b"APP_BENCH_T" in (payload or b"")
        report("lapply add persisted across reboot", persisted)

        # --- 9. timeout / hostage fix: announce payload then stall
        d.send_line("fwrite /apps/bench_h.bin 4096 00000000")
        r = d.read_lines(n=1)
        d.send_line("fwdata 0 4096 00000000")
        # send NO payload bytes; device should error out within ~ gap(1s)/cap window
        t0 = time.time()
        r = d.read_lines(n=1, timeout=8)
        dt = time.time() - t0
        report("stalled payload times out (not forever)", bool(r) and r[0].startswith("[err]") and dt < 6,
               f"{dt:.1f}s {r[0] if r else ''}")
        d.drain(1.5)
        d.send_line("fwabort")
        d.read_lines(n=1)
        d.drain(0.5)
        d.send_line("version")
        # the version reply says "version=", not "fw=" (fw= only appears in
        # the boot banner) - grepping fw= here fails on a healthy device
        alive = "version=" in d.drain(2.5)
        report("device responsive after stall", alive)

        # --- 10. cleanup: remove bench entry + files, verify
        ops = json.dumps({"ops": [{"op": "remove", "id": "APP_BENCH_T"}]}).encode()
        d.send_line(f"lapply {len(ops)} {crc(ops)}")
        d.send_bytes(ops)
        r = d.read_lines(n=1, timeout=8)
        report("cleanup: lapply remove ok", bool(r) and ".ok" in r[0], r[0] if r else "")
        for p in ["/apps/bench_t.bin", "/apps/bench_c.bin", "/apps/sub/nested.dat", "/apps/bench_crlf.bin"]:
            d.send_line(f"fdelete {p}")
            r = d.read_lines(n=1)
            report(f"cleanup: fdelete {p}", bool(r) and (".ok" in r[0] or ".absent" in r[0]), r[0] if r else "")
        d.send_line("syncinfo")
        lines = d.read_lines(n=3)
        fs_free1 = 0
        for l in lines:
            for tok in l.split():
                if tok.startswith("fs_free="):
                    fs_free1 = int(tok.split("=")[1])
        # The bench legitimately leaves a persisted manifest behind (created by
        # the first lapply), so allow a modest permanent delta for it.
        report("storage reclaimed after cleanup (manifest remains)", fs_free1 >= fs_free0 - 16384,
               f"free before={fs_free0} after={fs_free1}")

        # final reboot to leave the device in a fresh state
        d.reset()
        d.drain(2.0)
    finally:
        d.close()

    fails = [r for r in results if not r[1]]
    print(f"\n=== BENCH SUMMARY: {len(results) - len(fails)}/{len(results)} passed ===")
    for name, ok, detail in fails:
        print(f"  FAILED: {name} -- {detail}")
    sys.exit(1 if fails else 0)

main()
