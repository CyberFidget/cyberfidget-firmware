# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""Bench observation driver for the battery-protection (UVLO) firmware paths.

Logs a serial window with wall-clock + monotonic timestamps. Two modes:

  passive (default)  open with DTR/RTS deasserted, just log. Use to watch
                     timer wakes / shutdown behavior without disturbing state.
  --interact         reset dance, alive-poll `version` until the CLI answers,
                     run `version` + `info`, then send `sleep` (test build
                     only) and keep logging until the window ends.

Run with pio's python (has pyserial) against an energized device:

  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe test\\bench\\uvlo_bench.py \
      --port COM36 --seconds 400 --out runs\\w1.log --interact

Serial-session rules embedded here (see cyberfidget-hil/README.md):
opening a port can wedge the CLI, so interact mode always does the reset
dance then polls until `version` answers; passive mode deasserts DTR/RTS
before open so it does not reboot the device it was sent to observe.
"""

import argparse
import sys
import time
from datetime import datetime, timezone

import serial

BAUD = 921600


def _stamp(t0):
    wall = datetime.now(timezone.utc).strftime("%H:%M:%S.%f")[:-3]
    return "[%s +%9.3fs]" % (wall, time.monotonic() - t0)


class Log:
    def __init__(self, path, t0):
        self.f = open(path, "w", encoding="ascii", errors="replace")
        self.t0 = t0

    def line(self, text):
        self.f.write("%s %s\n" % (_stamp(self.t0), text))
        self.f.flush()

    def raw(self, raw_bytes):
        text = raw_bytes.decode("ascii", errors="replace").rstrip("\r\n")
        if text:
            self.line(text)


def drain(ser, log, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        raw = ser.readline()
        if raw:
            log.raw(raw)


def send(ser, log, cmd):
    log.line("--- tx: %s ---" % cmd)
    ser.write((cmd + "\n").encode("ascii"))


def alive_poll(ser, log, timeout_s=15.0):
    """Poll `version` until the CLI answers; boot prints are logged as they come."""
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        send(ser, log, "version")
        t_cmd = time.monotonic() + 1.5
        while time.monotonic() < t_cmd:
            raw = ser.readline()
            if not raw:
                continue
            log.raw(raw)
            if b"version=" in raw:
                return True
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=600.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--interact", action="store_true")
    args = ap.parse_args(argv)

    t0 = time.monotonic()
    log = Log(args.out, t0)

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = BAUD
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        log.line("--- open failed: %s ---" % e)
        return 1

    log.line("--- open port=%s mode=%s window=%ss ---"
             % (args.port, "interact" if args.interact else "passive", args.seconds))

    try:
        if args.interact:
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
            log.line("--- reset dance issued ---")
            if not alive_poll(ser, log):
                log.line("--- alive-poll FAILED: CLI never answered ---")
                return 1
            # Late async boot prints can trail the first CLI answer.
            drain(ser, log, 2.0)
            send(ser, log, "info")
            drain(ser, log, 2.0)
            send(ser, log, "sleep")
            drain(ser, log, 2.0)
        # Log whatever remains of the window (timer wakes, verdicts, banners).
        remaining = args.seconds - (time.monotonic() - t0)
        if remaining > 0:
            drain(ser, log, remaining)
    except serial.SerialException as e:
        log.line("--- serial error: %s ---" % e)
        return 1
    finally:
        log.line("--- closed ---")
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
