# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC

"""Regenerate lib/*/library.json dependency lists from #include scanning.

To run, create a new terminal and enter:
    python generate_library_json.py

!! Read this before running it. !!

This rewrites EVERY lib/*/library.json from scratch, and creates one for any
library that currently has none. The committed files were generated years ago
and have not been kept in step, so a run today produces a substantially
different dependency graph -- new edges, reordered lists, and new files for
libraries that deliberately rely on the LDF instead. That is a real change in
build behavior, not a formatting pass.

So: run it deliberately, read the whole diff, and build before committing.
Do not run it as a drive-by while fixing something else.

Only *intra-repo* dependencies are emitted -- an include is recorded as a
dependency solely when its header resolves to another directory under lib/.

Everything else is deliberately dropped, because PlatformIO treats a
`dependencies` entry as a package to resolve, not as documentation:

  - Standard-library and system headers (<vector>, <time.h>, <sys/time.h>)
    are not packages. `sys/time` is the worst of them: PlatformIO reads the
    slash as `owner/name` and fails the whole build with UnknownPackageError.
    That entry broke every release build under PlatformIO 6.2.0, which began
    resolving the dependencies declared by private libraries in lib/.
  - External libraries (Adafruit_NeoPixel, SSD1306Wire, the SparkFun drivers)
    are pinned with real versions in platformio.ini's lib_deps. Repeating an
    include-derived guess at their package name here can only contradict it.
  - Headers that live inside the library being scanned (icons, images,
    DinoSprites) are files, not libraries.

The library's own name is skipped so a library never depends on itself.
"""

import os
import json
import re

LIB_DIR = "lib"
LIB_JSON_FILENAME = "library.json"

HEADER_REGEX = re.compile(r'#include\s+[<"]([^">]+)[">]')
SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cpp")


def sibling_libraries():
    """Map lowercased directory name -> real directory name for every lib/*."""
    return {
        entry.lower(): entry
        for entry in os.listdir(LIB_DIR)
        if os.path.isdir(os.path.join(LIB_DIR, entry))
    }


def extract_dependencies(library_path, siblings):
    """Collect the sibling libraries this library includes headers from."""
    own_name = os.path.basename(library_path).lower()
    found = {}

    for root, _, files in os.walk(library_path):
        for file in files:
            if not file.endswith(SOURCE_SUFFIXES):
                continue
            with open(os.path.join(root, file), "r", encoding="utf-8") as f:
                for line in f:
                    match = HEADER_REGEX.search(line)
                    if not match:
                        continue
                    # lib/Foo/Foo.h and <Foo.h> both identify the library Foo.
                    stem = os.path.basename(match.group(1))
                    for suffix in SOURCE_SUFFIXES:
                        if stem.endswith(suffix):
                            stem = stem[: -len(suffix)]
                            break
                    key = stem.lower()
                    if key == own_name or key not in siblings:
                        continue
                    found[key] = siblings[key]

    return [found[key] for key in sorted(found)]


def generate_library_json(lib_path, siblings):
    """Create or update library.json for a given library directory."""
    lib_name = os.path.basename(lib_path)
    dependencies = extract_dependencies(lib_path, siblings)

    # Always emit the field, empty list included. "We scanned and found none"
    # and "this file predates the field" are different claims, and at least one
    # library (CFGraphics) documents its empty list as a deliberate choice.
    lib_json = {
        "name": lib_name,
        "version": "1.0.0",
        "dependencies": [{"name": dep} for dep in dependencies],
    }

    lib_json_path = os.path.join(lib_path, LIB_JSON_FILENAME)

    # Preserve fields the scanner can't infer -- notably the "platforms" guard
    # on lib/HALMock, which keeps its Arduino.h/SD.h/SPI.h test shims from
    # shadowing the real framework headers in device and wasm builds.
    if os.path.exists(lib_json_path):
        with open(lib_json_path, "r", encoding="utf-8") as existing_file:
            existing = json.load(existing_file)
        for key, value in existing.items():
            if key not in ("name", "version", "dependencies"):
                lib_json[key] = value

    with open(lib_json_path, "w", encoding="utf-8", newline="\n") as json_file:
        json.dump(lib_json, json_file, indent=4)
        json_file.write("\n")

    print(f"Generated {LIB_JSON_FILENAME} for {lib_name}")


def main():
    """Scan the lib directory and create library.json files."""
    if not os.path.exists(LIB_DIR):
        print(f"Error: {LIB_DIR} directory not found.")
        return

    siblings = sibling_libraries()
    for library in sorted(os.listdir(LIB_DIR)):
        lib_path = os.path.join(LIB_DIR, library)
        if os.path.isdir(lib_path):
            generate_library_json(lib_path, siblings)


if __name__ == "__main__":
    main()
