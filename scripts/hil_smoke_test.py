#!/usr/bin/env python3
"""T-001 HIL smoke test: capture boot banner + exercise version/info/help/garbage CLI."""
import argparse
import sys
import time
import serial


def collect(ser, duration_s):
    end = time.time() + duration_s
    chunks = []
    while time.time() < end:
        if ser.in_waiting:
            chunks.append(ser.read(ser.in_waiting))
        else:
            time.sleep(0.05)
    return b"".join(chunks).decode("utf-8", errors="replace")


def send_cmd(ser, cmd, wait_s=0.5):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    return collect(ser, wait_s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    print(f"Opening {args.port} @ {args.baud}...")
    ser = serial.Serial(args.port, args.baud, timeout=0.5)

    # Reset device via DTR/RTS toggle so we can catch the boot banner.
    print("Resetting device via DTR/RTS toggle...")
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)
    ser.reset_input_buffer()
    # Now release: pulling RTS low releases EN (active-low reset on this board)
    # The DTR/RTS dance above triggers a hard reset on most ESP32 dev boards.

    print("\n=== Boot banner (3s capture) ===")
    boot_output = collect(ser, 3.0)
    print(boot_output)
    print("=== end boot capture ===\n")

    boot_ok = "[boot] fw=" in boot_output
    print(f"Boot banner detected: {boot_ok}")

    # Wait a bit so the boot animation app finishes initializing and AppManager
    # is in its main loop ready to consume CLI input.
    time.sleep(2.0)

    print("\n=== version ===")
    out = send_cmd(ser, "version", wait_s=0.5)
    print(out)
    version_ok = "[cmd] version=" in out

    print("\n=== info ===")
    out = send_cmd(ser, "info", wait_s=0.7)
    print(out)
    info_ok = ("[cmd] info.fw=" in out and "[cmd] info.type=" in out
               and "[cmd] info.uptime_ms=" in out)

    print("\n=== help ===")
    out = send_cmd(ser, "help", wait_s=0.5)
    print(out)
    help_ok = "[cmd] help=" in out

    print("\n=== unknown command ===")
    out = send_cmd(ser, "garbage", wait_s=0.5)
    print(out)
    err_ok = "[err] unknown command:" in out

    print("\n=== overflow (100-char line) ===")
    out = send_cmd(ser, "x" * 100, wait_s=0.5)
    print(out)
    overflow_ok = "[err] line too long" in out

    print("\n=== case-insensitive (VERSION upper) ===")
    out = send_cmd(ser, "VERSION", wait_s=0.5)
    print(out)
    case_ok = "[cmd] version=" in out

    ser.close()

    print("\n========================================")
    print(f"  boot banner:    {'PASS' if boot_ok else 'FAIL'}")
    print(f"  version cmd:    {'PASS' if version_ok else 'FAIL'}")
    print(f"  info cmd:       {'PASS' if info_ok else 'FAIL'}")
    print(f"  help cmd:       {'PASS' if help_ok else 'FAIL'}")
    print(f"  unknown err:    {'PASS' if err_ok else 'FAIL'}")
    print(f"  overflow err:   {'PASS' if overflow_ok else 'FAIL'}")
    print(f"  case insens:    {'PASS' if case_ok else 'FAIL'}")
    print("========================================")

    all_ok = all([boot_ok, version_ok, info_ok, help_ok, err_ok, overflow_ok, case_ok])
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
