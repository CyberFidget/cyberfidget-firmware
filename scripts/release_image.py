# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC

"""Check the app image size and write its release metadata."""

import argparse
import configparser
import hashlib
import json
from pathlib import Path


# default_8MB.csv has two 0x330000-byte app slots. Reserve 64 KB of each.
SLOT_SIZE = 3_342_336
MARGIN = 65_536
THRESHOLD = SLOT_SIZE - MARGIN


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--info", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--hw-min-rev")
    parser.add_argument("--hw-max-rev")
    args = parser.parse_args()

    data = args.image.read_bytes()
    size = len(data)
    print(f"Image size: {size} bytes; slot size: {SLOT_SIZE} bytes; "
          f"margin: {MARGIN} bytes; threshold: {THRESHOLD} bytes")
    if size > THRESHOLD:
        parser.exit(1, "App image exceeds the release threshold.\n")

    if args.info:
        if not all((args.version, args.hw_min_rev, args.hw_max_rev)):
            parser.error("--info requires --version, --hw-min-rev and --hw-max-rev")
        config = configparser.ConfigParser(inline_comment_prefixes=(';',))
        config.read(Path(__file__).resolve().parent.parent / 'platformio.ini')
        flash_mode = config.get('base', 'board_build.flash_mode')
        # PlatformIO writes dio into the image header for a qio/qout board
        # setting; record the mode the image actually carries.
        if flash_mode in ('qio', 'qout'):
            flash_mode = 'dio'
        info = {
            "version": args.version,
            "flash_mode": flash_mode,
            "app_size": size,
            "app_sha256": hashlib.sha256(data).hexdigest(),
            "hw": {"min_rev": args.hw_min_rev, "max_rev": args.hw_max_rev},
        }
        args.info.write_text(json.dumps(info, separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
