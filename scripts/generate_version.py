# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC

"""
PlatformIO pre-build script + standalone CLI: generate generated/version.h.

Embeds semver (from version.txt), short git commit hash, dirty flag,
build type, and ISO-8601 build timestamp. Used by firmware, WASM, and
any external CI to give every produced binary a uniquely identifying
surface visible in the boot banner and serial CLI.

Two invocation modes:
1. PlatformIO pre-build (registered via extra_scripts in platformio.ini):
   Imports SCons env, derives output path from PROJECT_DIR, also
   registers a post-build action that re-prints the build summary at
   the end of the build for easy spot-check before flashing.
2. Standalone CLI (used by wasm/build_wasm.sh, ESP-IDF, Arduino IDE):
   python generate_version.py --out PATH [--build-type-override LABEL]

Exits 0 on success, non-zero on hard-fail (missing/malformed version.txt).
Always prints a build summary block to stdout so the developer can
verify the device banner matches what they just built.

For the design rationale, see the firmware-version reference page
in cyberfidget-docs.
"""

import argparse
import datetime
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
RELEASE_TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
PRERELEASE_TAG_RE = re.compile(r"^v\d+\.\d+\.\d+-(rc|alpha|beta).*$", re.IGNORECASE)
# Permissive: any suffix on a v<MAJOR.MINOR.PATCH>-<...> tag is captured. The
# captured portion lands in FW_VERSION_PRERELEASE / FW_VERSION_STRING per the
# semver spec (item 9 — pre-release identifiers). Build-type classification
# remains the stricter responsibility of PRERELEASE_TAG_RE above.
TAG_SUFFIX_RE = re.compile(r"^v\d+\.\d+\.\d+-(.+)$")


def find_project_root(start: str | Path) -> Path:
    """Walk up from start until version.txt is found; that's the firmware repo root."""
    p = Path(start).resolve()
    for candidate in [p, *p.parents]:
        if (candidate / "version.txt").exists():
            return candidate
    raise SystemExit(
        "[generate_version] could not find version.txt walking up from {}".format(start)
    )


def read_version_txt(repo_root: Path) -> tuple[int, int, int]:
    path = repo_root / "version.txt"
    if not path.exists():
        raise SystemExit(
            "[generate_version] {} missing - refusing to guess a version".format(path)
        )
    text = path.read_text().strip()
    m = VERSION_RE.match(text)
    if not m:
        raise SystemExit(
            "[generate_version] {} content {!r} does not match MAJOR.MINOR.PATCH".format(
                path, text
            )
        )
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def run_git(args: list[str], cwd: Path) -> tuple[int, str]:
    """Returns (returncode, stdout). returncode 127 means git not on PATH."""
    try:
        result = subprocess.run(
            ["git"] + args,
            cwd=str(cwd),
            capture_output=True,
            text=True,
        )
        return result.returncode, result.stdout.strip()
    except FileNotFoundError:
        return 127, ""


def get_git_state(repo_root: Path) -> tuple[str, int, bool, str]:
    """Returns (short_hash, dirty_int, git_available_bool, commit_iso)."""
    rc, hash_out = run_git(["rev-parse", "--short=7", "HEAD"], repo_root)
    if rc != 0 or not hash_out:
        return "unknown", 0, False, ""
    rc, status_out = run_git(["status", "--porcelain"], repo_root)
    dirty = 1 if (rc == 0 and status_out) else 0
    rc, commit_iso = run_git(["log", "-1", "--format=%cI", "HEAD"], repo_root)
    if rc != 0:
        commit_iso = ""
    return hash_out, dirty, True, commit_iso


def extract_prerelease_tag(ref: str) -> str:
    """Extract the prerelease portion of a v<MAJOR.MINOR.PATCH>-<suffix> ref.

    Returns the suffix verbatim (e.g. 'rc1', 'alpha.2', 'beta', 'foobar') or
    an empty string for non-tag refs / tags without a suffix. The script trusts
    the human's tag — any suffix the operator chose is propagated through to
    FW_VERSION_PRERELEASE and FW_VERSION_STRING. Build-type classification
    (release vs. prerelease vs. ci-dev) remains stricter; see select_build_type.
    """
    m = TAG_SUFFIX_RE.match(ref or "")
    return m.group(1) if m else ""


def select_build_type(git_available: bool, dirty: int, override: str | None) -> str:
    if override:
        return override
    in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
    ref = os.environ.get("GITHUB_REF_NAME", "")
    if in_ci:
        if RELEASE_TAG_RE.match(ref):
            return "release"
        if PRERELEASE_TAG_RE.match(ref):
            return "prerelease"
        return "ci-dev"
    if not git_available:
        return "unknown"
    if dirty:
        return "dirty"
    return "dev"


def build_full_string(
    major: int,
    minor: int,
    patch: int,
    git_hash: str,
    dirty: int,
    prerelease: str = "",
) -> tuple[str, str]:
    """Assemble the semver strings.

    Returns (base, full):
      base = "X.Y.Z" or "X.Y.Z-<prerelease>" (semver item 9)
      full = base + "+<hash>" + optional "-dirty" (semver item 10 build metadata)

    A prerelease tag from a CI tag push (e.g. v1.2.0-rc1) lifts into both
    base and full; without one, base is just "X.Y.Z".
    """
    base = "{}.{}.{}".format(major, minor, patch)
    if prerelease:
        base += "-{}".format(prerelease)
    full = "{}+{}".format(base, git_hash)
    if dirty:
        # Build-metadata identifiers are dot-separated per SemVer item 10.
        # Using "." (not "-") here keeps the suffix unambiguously inside the
        # build-metadata segment and avoids visual collision with prerelease
        # markers (which live before "+", introduced by "-").
        full += ".dirty"
    return base, full


def render_version_h(
    major: int,
    minor: int,
    patch: int,
    git_hash: str,
    dirty: int,
    build_type: str,
    build_timestamp: str,
    prerelease: str = "",
) -> str:
    base, full = build_full_string(major, minor, patch, git_hash, dirty,
                                   prerelease)
    return (
        "// Generated by scripts/generate_version.py - DO NOT EDIT.\n"
        "// Source of truth: version.txt + git working tree.\n"
        "// See the firmware-version reference page in cyberfidget-docs.\n"
        "\n"
        "#ifndef CYBERFIDGET_VERSION_H\n"
        "#define CYBERFIDGET_VERSION_H\n"
        "\n"
        "#define FW_VERSION_MAJOR {major}\n"
        "#define FW_VERSION_MINOR {minor}\n"
        "#define FW_VERSION_PATCH {patch}\n"
        "#define FW_VERSION_PRERELEASE \"{prerelease}\"\n"
        "\n"
        "#define VERSION_ENCODE(major, minor, patch) "
        "(((major) << 16) | ((minor) << 8) | (patch))\n"
        "#define FW_VERSION "
        "VERSION_ENCODE(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)\n"
        "\n"
        "#define FW_VERSION_STRING \"{base}\"\n"
        "#define FW_GIT_HASH \"{git_hash}\"\n"
        "#define FW_GIT_DIRTY {dirty}\n"
        "#define FW_BUILD_TIMESTAMP \"{ts}\"\n"
        "#define FW_BUILD_TYPE \"{build_type}\"\n"
        "#define FW_VERSION_FULL_STRING \"{full}\"\n"
        "\n"
        "#endif  // CYBERFIDGET_VERSION_H\n"
    ).format(
        major=major, minor=minor, patch=patch,
        prerelease=prerelease,
        base=base, git_hash=git_hash, dirty=dirty,
        ts=build_timestamp, build_type=build_type, full=full,
    )


def write_atomic_if_changed(out_path: Path, content: str) -> bool:
    """Write content to out_path atomically; skip if byte-identical to existing."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        try:
            if out_path.read_text() == content:
                return False  # unchanged - leave mtime alone
        except Exception:
            pass
    tmp = out_path.with_suffix(out_path.suffix + ".tmp")
    tmp.write_text(content)
    os.replace(str(tmp), str(out_path))
    return True


def build_hints(build_type: str, in_ci: bool) -> list[str]:
    """Returns advisory hint lines (or [] in CI / clean local builds)."""
    if in_ci:
        return []
    if build_type == "dirty":
        return [
            "[hint] Working tree has uncommitted changes. Commit before",
            "       building to give this binary a unique, traceable",
            "       identifier in serial output.",
        ]
    if build_type == "unknown":
        return [
            "[hint] Built without git access - the embedded hash is \"unknown\",",
            "       so this binary can't be uniquely identified later. Building",
            "       inside a git clone (with git installed) gives every binary",
            "       a distinct commit hash visible in serial output.",
        ]
    return []


def print_summary(
    full: str,
    build_type: str,
    build_timestamp: str,
    output_artifact: Path | None,
    hints: list[str],
) -> None:
    out = sys.stdout
    out.write("\n")
    for hint in hints:
        out.write(hint + "\n")
    out.write("================================================\n")
    out.write("  Cyber Fidget firmware build complete\n")
    out.write("    fw:     {}\n".format(full))
    out.write("    type:   {}\n".format(build_type))
    out.write("    built:  {}\n".format(build_timestamp))
    if output_artifact:
        out.write("    output: {}\n".format(output_artifact))
    out.write("\n")
    out.write("  After flashing, the device's boot banner and\n")
    out.write("  `version` CLI command should report:\n")
    out.write("    [cmd] version={}\n".format(full))
    out.write("================================================\n")
    out.flush()


def generate(
    repo_root: Path,
    out_path: Path,
    output_artifact: Path | None = None,
    build_type_override: str | None = None,
) -> dict[str, Any]:
    major, minor, patch = read_version_txt(repo_root)
    git_hash, dirty, git_available, commit_iso = get_git_state(repo_root)
    override = build_type_override or os.environ.get("CYBERFIDGET_BUILD_TYPE_OVERRIDE")
    build_type = select_build_type(git_available, dirty, override or None)
    in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
    # CI tag pushes carry a prerelease suffix (e.g. v1.2.0-rc1); propagate it
    # into the version string per semver item 9. Non-CI / non-tag refs leave
    # FW_VERSION_PRERELEASE empty.
    ref = os.environ.get("GITHUB_REF_NAME", "")
    prerelease = extract_prerelease_tag(ref) if in_ci else ""
    # Embedded timestamp is the *commit* date for clean builds — deterministic,
    # so re-running this script with no source changes yields a byte-identical
    # version.h and avoids unnecessary rebuilds. For dirty / no-git / overridden
    # builds we fall back to the wall-clock time since the binary isn't
    # reproducible from a single commit anyway.
    if git_available and commit_iso and not dirty and not (override or "").strip():
        build_timestamp = commit_iso
    else:
        build_timestamp = datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )

    content = render_version_h(
        major, minor, patch, git_hash, dirty, build_type, build_timestamp,
        prerelease,
    )
    written = write_atomic_if_changed(out_path, content)

    base, full = build_full_string(major, minor, patch, git_hash, dirty,
                                   prerelease)
    hints = build_hints(build_type, in_ci)
    print_summary(full, build_type, build_timestamp, output_artifact, hints)

    return {
        "major": major, "minor": minor, "patch": patch,
        "prerelease": prerelease,
        "version_string": base,
        "git_hash": git_hash, "dirty": dirty,
        "build_type": build_type, "build_timestamp": build_timestamp,
        "version_full": full,
        "header_written": written,
    }


def standalone_main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate version.h for Cyber Fidget firmware builds."
    )
    parser.add_argument("--out", required=True,
                        help="Output path for version.h (will be created).")
    parser.add_argument("--build-type-override", default=None,
                        help="Override FW_BUILD_TYPE (also via CYBERFIDGET_BUILD_TYPE_OVERRIDE env).")
    parser.add_argument("--repo-root", default=None,
                        help="Repo root containing version.txt (default: walk up from cwd).")
    parser.add_argument("--standalone", action="store_true",
                        help="Informational: marks this invocation as standalone (outside "
                             "PlatformIO). The script auto-detects either way; this flag "
                             "is accepted so docs/build scripts can name the mode explicitly.")
    args = parser.parse_args()
    out_path = Path(args.out)
    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        repo_root = find_project_root(Path.cwd())
    generate(repo_root, out_path,
             output_artifact=None,
             build_type_override=args.build_type_override)


def platformio_main(env: Any) -> None:
    project_dir = Path(env["PROJECT_DIR"])
    out_path = project_dir / "generated" / "version.h"
    info = generate(project_dir, out_path, output_artifact=None)

    def post_summary(source: Any, target: Any, env: Any) -> None:
        # SCons calls this with keyword args (source=, target=, env=), so the
        # parameter names are part of the contract — don't rename. Only `target`
        # is read; `del` signals the rest are intentionally unused.
        del source, env
        artifact = None
        try:
            if target:
                artifact = Path(str(target[0]))
        except Exception:
            artifact = None
        in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
        hints = build_hints(info["build_type"], in_ci)
        print_summary(
            info["version_full"], info["build_type"],
            info["build_timestamp"], artifact, hints,
        )

    try:
        env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", post_summary)
    except Exception:
        pass


# Mode dispatch: SCons defines `Import` in our namespace; standalone Python does not.
_running_under_scons = False
try:
    Import("env")  # type: ignore[name-defined]  # noqa: F821
    _running_under_scons = True
except NameError:
    _running_under_scons = False

if _running_under_scons:
    platformio_main(env)  # type: ignore[name-defined]  # noqa: F821
elif __name__ == "__main__":
    standalone_main()
