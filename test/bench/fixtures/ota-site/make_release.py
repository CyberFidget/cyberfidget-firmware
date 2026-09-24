# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""Make one bench release folder for router.php (see README.md).

    python make_release.py <releases-dir>/<name> --bin .pio/build/local_test/firmware.bin \
        --version 1.3.4+abc1234.dirty --released-at 2026-09-24T11:00:00Z [--release-id 2]
        [--wrong-sha] [--flip-offset N] [--status 503] [--min-rev 1.2 --max-rev 1.2]
        [--channel stable|rc] [--source official]
"""

import argparse
import hashlib
import json
import shutil
from pathlib import Path


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--bin", type=Path, required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--released-at", required=True)
    ap.add_argument("--release-id", type=int, default=1)
    ap.add_argument("--wrong-sha", action="store_true", help="announce a digest the bytes do not have")
    ap.add_argument("--flip-offset", type=int, help="corrupt one byte in transit")
    ap.add_argument("--status", type=int, help="answer every request with this HTTP status")
    ap.add_argument("--min-rev", default="1.2")
    ap.add_argument("--max-rev", default="1.2")
    ap.add_argument("--channel", default="stable")
    ap.add_argument("--source", default="official")
    args = ap.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.bin, args.out / "firmware.bin")
    data = args.bin.read_bytes()
    info = {
        "version": args.version,
        "app_size": len(data),
        "app_sha256": hashlib.sha256(data).hexdigest(),
        "hw": {"min_rev": args.min_rev, "max_rev": args.max_rev},
        "released_at": args.released_at,
        "release_id": args.release_id,
        "channel": args.channel,
        "source": args.source,
    }
    if args.wrong_sha:
        info["sha256"] = hashlib.sha256(data + b"x").hexdigest()
    if args.flip_offset is not None:
        info["flip_offset"] = args.flip_offset
    if args.status is not None:
        info["status"] = args.status
    (args.out / "release-info.json").write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")
    print("%s: %s %d bytes sha256=%s" % (args.out, args.version, len(data), info["app_sha256"]))


if __name__ == "__main__":
    main()
