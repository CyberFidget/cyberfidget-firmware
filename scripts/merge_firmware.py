# SPDX-License-Identifier: MIT
# Copyright (c) 2023-2026 Dismo Industries LLC

try:
    Import("env")
except NameError:
    print("This script is intended to be run by PlatformIO, not directly.")
    import sys
    sys.exit(1)

import os

print(">>> merge_firmware.py loaded")

def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    firmware = os.path.join(build_dir, "firmware.bin")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    merged_output = os.path.join(build_dir, "merged_firmware.bin")

    print("=== Merging Binaries with esptool ===")
    print(f"Build directory: {build_dir}")
    print(f"Bootloader:      {bootloader}")
    print(f"Partitions:      {partitions}")
    print(f"Firmware:        {firmware}")
    print(f"Output (merged): {merged_output}")
    print("Running esptool...")

    result = env.Execute(
        f"esptool --chip esp32 merge_bin -o {merged_output} "
        f"--flash_mode dio --flash_freq 80m --flash_size 8MB "  # header mode PlatformIO writes for qio; see build-release.yml
        f"0x1000 {bootloader} 0x8000 {partitions} 0x10000 {firmware}"
    )

    if result == 0:
        print("Merged firmware created successfully.")
    else:
        print("Failed to merge firmware.")

# Run merging after firmware is built (triggered by build or upload)
env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
