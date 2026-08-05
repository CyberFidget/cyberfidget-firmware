# SPDX-License-Identifier: MIT
# Copyright (c) 2023-2026 Dismo Industries LLC
#
# Registers the `sdpack` custom target:
#
#     pio run -t sdpack
#
# It builds the phone companion web app (portal-companion/) into
# portal-companion/dist/web/, which is copied onto the memory card as /web/.
#
# This does NOT run as part of a normal `pio run`. The companion is not part
# of the firmware image, changes far less often than firmware, and needs
# Node.js — taxing every flash with it (and breaking builds on machines
# without Node) would be the wrong trade. Registration below is just a target
# definition; nothing executes until you ask for `-t sdpack`.

try:
    Import("env")
except NameError:
    print("This script is intended to be run by PlatformIO, not directly.")
    import sys
    sys.exit(1)

import os
import subprocess

print(">>> build_companion.py loaded (target: sdpack)")


def _npm():
    # npm ships as npm.cmd on Windows; the bare name isn't executable there.
    return "npm.cmd" if os.name == "nt" else "npm"


def build_sdpack(*_args, **_kwargs):
    root = os.path.join(env.subst("$PROJECT_DIR"), "portal-companion")

    if not os.path.isdir(root):
        print(f"ERROR: {root} not found")
        return 1

    npm = _npm()
    try:
        if not os.path.isdir(os.path.join(root, "node_modules")):
            print("=== Installing companion dependencies (first run) ===")
            subprocess.check_call([npm, "ci"], cwd=root)

        print("=== Building phone companion SD pack ===")
        subprocess.check_call([npm, "run", "build"], cwd=root)
    except FileNotFoundError:
        print("ERROR: npm not found on PATH. Install Node.js "
              "(https://nodejs.org/) to build the companion pack.")
        return 1
    except subprocess.CalledProcessError as exc:
        print(f"ERROR: companion build failed (exit {exc.returncode})")
        return exc.returncode

    out = os.path.join(root, "dist", "web")
    print(f"=== Companion pack ready: {out} ===")
    print("Copy it onto the memory card as /web/ (so the card has "
          "/web/index.html).")
    print("Live listening needs only index.html; captions also need "
          "engine.worker.js + vendor/.")
    return 0


env.AddCustomTarget(
    name="sdpack",
    dependencies=None,
    actions=[build_sdpack],
    title="Phone companion SD pack",
    description="Build the companion web app -> portal-companion/dist/web/",
    always_build=True,
)
