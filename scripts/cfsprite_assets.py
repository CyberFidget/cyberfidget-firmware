# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC

"""PlatformIO drift check and explicit regeneration target for .cfsprite assets."""

from pathlib import Path
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injects Import.


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
CLI = PROJECT_DIR / "tools" / "cfsprite_compile_cli.mjs"


def asset_jobs():
    jobs = []
    for source in sorted(PROJECT_DIR.glob("lib/*/assets/*.cfsprite.json")):
        output = source.parent.parent / "generated" / f"{source.name.removesuffix('.cfsprite.json')}.h"
        jobs.append((source, output))
    return jobs


def run_compiler(check_only):
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js is required to compile .cfsprite assets")
    for source, output in asset_jobs():
        command = [node, str(CLI)]
        if check_only:
            command.append("--check-only")
        command.extend([
            "--symbol-prefix",
            source.name.removesuffix(".cfsprite.json"),
            str(source.relative_to(PROJECT_DIR)),
            str(output.relative_to(PROJECT_DIR)),
        ])
        completed = subprocess.run(command, cwd=PROJECT_DIR, check=False)
        if completed.returncode:
            raise RuntimeError(
                f".cfsprite {'drift check' if check_only else 'regeneration'} failed for "
                f"{source.relative_to(PROJECT_DIR)}"
            )


def check_assets(source, target, env):
    run_compiler(check_only=True)


def regenerate_assets(source, target, env):
    run_compiler(check_only=False)


env.AddPreAction("buildprog", check_assets)  # type: ignore[name-defined]
env.AddCustomTarget(  # type: ignore[name-defined]
    name="cfsprite-regen",
    dependencies=None,
    actions=regenerate_assets,
    title="cfsprite-regen",
    description="Regenerate cf::gfx headers from lib/*/assets/*.cfsprite.json",
)
