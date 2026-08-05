# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC
"""
PlatformIO pre-build script + standalone CLI: generate generated/portal_page_gz.h.

The portal page is ~86 KB of markup, CSS and JS held in a PROGMEM string literal
in lib/WebPortalApp/portal_page.h and, until this script existed, served
uncompressed. Gzipping it reclaims roughly two thirds of that from app0.

Unlike the companion shell -- whose gzipped header is a *tracked* file, because
producing it needs Node and the firmware must build without a JS toolchain --
this runs during every firmware build. Python's gzip is in the standard library,
so there is nothing extra to install, nothing to regenerate by hand, and no way
for the compressed copy to drift from the source. portal_page.h stays the single
editable source of truth.

Determinism: gzip.compress() would stamp an mtime, so GzipFile is used with
mtime=0. The output is a build artifact under generated/ (gitignored) rather than
a tracked file, so churn would be harmless -- but a byte-stable build is worth
having anyway, and it keeps ccache-style tooling happy.

Two invocation modes:
1. PlatformIO pre-build (registered via extra_scripts in platformio.ini).
2. Standalone:
     python gzip_portal_page.py --out PATH [--source PATH]

Exits 0 on success, non-zero if the source literal cannot be found.
"""

import argparse
import gzip
import io
import sys
from pathlib import Path

OPEN_MARK = 'R"rawliteral('
CLOSE_MARK = ')rawliteral"'
SYMBOL = "PORTAL_PAGE_GZ"


def find_repo_root(start: Path) -> Path:
    """Walk up from start until version.txt is found; that's the firmware repo root."""
    for candidate in [start, *start.parents]:
        if (candidate / "version.txt").exists():
            return candidate
    raise SystemExit(
        "[gzip_portal_page] could not find version.txt walking up from {}".format(start)
    )


def extract_literal(source: Path) -> bytes:
    """Pull the raw-string payload out of the PROGMEM page header."""
    text = source.read_text(encoding="utf-8")
    open_at = text.find(OPEN_MARK)
    close_at = text.rfind(CLOSE_MARK)
    if open_at < 0 or close_at < 0 or close_at <= open_at:
        raise SystemExit(
            "[gzip_portal_page] no {}...{} literal in {}".format(
                OPEN_MARK, CLOSE_MARK, source
            )
        )
    return text[open_at + len(OPEN_MARK) : close_at].encode("utf-8")


def gz(raw: bytes) -> bytes:
    """Deterministic gzip: no mtime, max compression."""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(raw)
    return buf.getvalue()


def render_header(payload: bytes, raw_len: int) -> str:
    rows = []
    for i in range(0, len(payload), 16):
        rows.append("  " + ",".join("0x{:02x}".format(b) for b in payload[i : i + 16]))
    return (
        "// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception\n"
        "// Copyright (c) 2023-2026 Dismo Industries LLC\n"
        "//\n"
        "// GENERATED AT BUILD TIME - DO NOT EDIT, DO NOT COMMIT.\n"
        "// Produced by scripts/gzip_portal_page.py from\n"
        "// lib/WebPortalApp/portal_page.h, which is the editable source.\n"
        "//\n"
        "// Gzipped portal page: {raw} B raw -> {gzl} B.\n"
        "// Served with Content-Encoding: gzip; browsers decompress natively.\n"
        "\n#pragma once\n\n#include <stdint.h>\n\n"
        "const uint8_t {sym}[] PROGMEM = {{\n{rows}\n}};\n".format(
            raw=raw_len, gzl=len(payload), sym=SYMBOL, rows=",\n".join(rows)
        )
    )


def build(source: Path, out: Path) -> tuple[int, int]:
    raw = extract_literal(source)
    payload = gz(raw)
    header = render_header(payload, len(raw))
    # Only rewrite when the content actually changes, so an unchanged portal page
    # does not invalidate the compilation unit on every build.
    if not out.exists() or out.read_text(encoding="utf-8") != header:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(header, encoding="utf-8")
    return len(raw), len(payload)


def platformio_main(env) -> None:
    project_dir = Path(env["PROJECT_DIR"])
    source = project_dir / "lib" / "WebPortalApp" / "portal_page.h"
    out = project_dir / "generated" / "portal_page_gz.h"
    raw_len, gz_len = build(source, out)
    pct = (1 - gz_len / raw_len) * 100 if raw_len else 0
    print(
        "[gzip_portal_page] portal page {} B -> {} B gzipped "
        "({:.1f}% smaller in flash)".format(raw_len, gz_len, pct)
    )


def standalone_main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Generate the gzipped portal page header.")
    ap.add_argument("--out", required=True, help="path to write portal_page_gz.h")
    ap.add_argument("--source", help="path to portal_page.h (default: derive from repo)")
    args = ap.parse_args(argv)
    if args.source:
        source = Path(args.source)
    else:
        source = find_repo_root(Path(__file__).resolve()) / "lib" / "WebPortalApp" / "portal_page.h"
    raw_len, gz_len = build(source, Path(args.out))
    print("[gzip_portal_page] {} B -> {} B".format(raw_len, gz_len))
    return 0


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
    sys.exit(standalone_main())
