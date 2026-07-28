# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC

"""PlatformIO drift check and explicit regeneration targets for sprite and model assets."""

# The filename is retained for compatibility even though this hook now covers both asset kinds.

from pathlib import Path
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injects Import.


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
ASSET_KINDS = (
    (".cfsprite", PROJECT_DIR / "tools" / "cfsprite_compile_cli.mjs"),
    (".cfmesh", PROJECT_DIR / "tools" / "cfmesh_compile_cli.mjs"),
)


def asset_jobs():
    jobs = []
    outputs = {}
    for asset_kind, cli in ASSET_KINDS:
        suffix = f"{asset_kind}.json"
        for source in sorted(PROJECT_DIR.glob(f"lib/*/assets/*{suffix}")):
            output = source.parent.parent / "generated" / f"{source.name.removesuffix(suffix)}.h"
            if output in outputs:
                raise RuntimeError(
                    f"Asset header collision: {outputs[output].relative_to(PROJECT_DIR)} and "
                    f"{source.relative_to(PROJECT_DIR)} both generate "
                    f"{output.relative_to(PROJECT_DIR)}"
                )
            outputs[output] = source
            jobs.append((asset_kind, cli, source, output))
    return jobs


def run_compiler(check_only, selected_kind=None):
    jobs = asset_jobs()
    node = shutil.which("node")
    if not node:
        has_models = any(asset_kind == ".cfmesh" for asset_kind, _, _, _ in jobs)
        asset_kind = selected_kind or (".cfsprite and .cfmesh" if has_models else ".cfsprite")
        raise RuntimeError(f"Node.js is required to compile {asset_kind} assets")
    for asset_kind, cli, source, output in jobs:
        if selected_kind and asset_kind != selected_kind:
            continue
        command = [node, str(cli)]
        if check_only:
            command.append("--check-only")
        command.extend([
            "--symbol-prefix",
            source.name.removesuffix(f"{asset_kind}.json"),
            str(source.relative_to(PROJECT_DIR)),
            str(output.relative_to(PROJECT_DIR)),
        ])
        completed = subprocess.run(command, cwd=PROJECT_DIR, check=False)
        if completed.returncode:
            raise RuntimeError(
                f"{asset_kind} {'drift check' if check_only else 'regeneration'} failed for "
                f"{source.relative_to(PROJECT_DIR)}"
            )


def check_assets(source, target, env):
    run_compiler(check_only=True)


def regenerate_sprites(source, target, env):
    run_compiler(check_only=False, selected_kind=".cfsprite")


def regenerate_models(source, target, env):
    run_compiler(check_only=False, selected_kind=".cfmesh")


env.AddPreAction("buildprog", check_assets)  # type: ignore[name-defined]
env.AddCustomTarget(  # type: ignore[name-defined]
    name="cfsprite-regen",
    dependencies=None,
    actions=regenerate_sprites,
    title="cfsprite-regen",
    description="Regenerate cf::gfx headers from lib/*/assets/*.cfsprite.json",
)
env.AddCustomTarget(  # type: ignore[name-defined]
    name="cfmesh-regen",
    dependencies=None,
    actions=regenerate_models,
    title="cfmesh-regen",
    description="Regenerate cf::gfx headers from lib/*/assets/*.cfmesh.json",
)
