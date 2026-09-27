# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC

"""
PlatformIO pre-build script: stop the library setup re-walking shared subtrees

PlatformIO's LibBuilderBase.build() (platformio/builder/tools/piolib.py,
Core 6.2.0) recurses into every dependency BEFORE checking is_built, so a
library reached along N paths has its whole subtree walked N times, with a
PrependUnique of every include path at each step. The cost grows with the
number of paths through the library graph, not the number of libraries: with
~70 cross-linked libs under lib/ the step took 25+ minutes before the first
compile, and each new cross-library include multiplied it.

A second call on an already-built library does nothing except re-merge
include/lib paths its env already holds and return the same list of library
nodes. This caches that list on the first repeat and returns it on later
repeats, so the build graph, flags and image are unchanged.

SCons loads PlatformIO's builder tools from a toolpath under its own module
name, so `import platformio.builder.tools.piolib` would patch a second, unused
copy. The class is found in the loaded modules instead. If it is missing the
script prints a warning banner and does nothing.

It also times the dependency scan and the library setup on every build and
prints a banner when either passes its budget, so a regression shows up in the
build that caused it. With CF_BUILD_PERF_STRICT=1 (set in CI) a missing patch
or a blown budget fails the build instead.
"""

import os
import sys
import time

Import("env")


def _memoize(lib_builder_base):
    original_build = lib_builder_base.build

    def memoized_build(self):
        if self.is_built and hasattr(self, "_cf_repeat_libs"):
            return list(self._cf_repeat_libs)
        was_built = self.is_built
        libs = original_build(self)
        if was_built:
            self._cf_repeat_libs = list(libs)
        return libs

    memoized_build._cf_memoized = True
    lib_builder_base.build = memoized_build


_patched = 0
for _module in list(sys.modules.values()):
    _cls = getattr(_module, "LibBuilderBase", None)
    if isinstance(_cls, type) and hasattr(_cls, "build"):
        if not getattr(_cls.build, "_cf_memoized", False):
            _memoize(_cls)
        _patched += 1

_STRICT = os.environ.get("CF_BUILD_PERF_STRICT") == "1"


def _budget(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


def _alarm(lines):
    """Print a banner nobody scrolls past; in strict mode (CI), fail the build."""
    bar = "!" * 72
    sys.stderr.write("\n%s\n" % bar)
    for line in lines:
        sys.stderr.write("!!  %s\n" % line)
    sys.stderr.write("%s\n\n" % bar)
    sys.stderr.flush()
    if _STRICT:
        sys.stderr.write("[memoize_lib_build] CF_BUILD_PERF_STRICT=1: failing the build\n")
        env.Exit(1)


if _patched:
    print("[memoize_lib_build] library setup memoized")
else:
    print("[memoize_lib_build] WARNING: LibBuilderBase not found; not patched")
    _alarm([
        "BUILD PERFORMANCE: the library setup patch did not apply.",
        "PlatformIO's internals have moved (a Core upgrade?). Without the patch,",
        "library setup can take 25+ minutes. Update scripts/memoize_lib_build.py.",
    ])


# Tripwire: time the dependency scan and the library setup on every build and
# shout when either passes its budget. The budgets are far above normal
# (setup takes ~1 s and the scan ~45 s on the workstation), so only a
# structural regression trips them, not a slow or busy machine. Override with
# CF_BUILD_BUDGET_SCAN_S / CF_BUILD_BUDGET_SETUP_S (0 forces the alarm, to
# test it).
_SCAN_BUDGET_S = _budget("CF_BUILD_BUDGET_SCAN_S", 120)
_SETUP_BUDGET_S = _budget("CF_BUILD_BUDGET_SETUP_S", 30)
_scan_s = []

try:
    # The plain functions behind env's methods, so a cloned env still runs
    # them against itself rather than against this one.
    _configure = env.ConfigureProjectLibBuilder.method
    _process_deps = env.ProcessProjectDeps.method
except AttributeError:
    print("[memoize_lib_build] WARNING: build phases not found; not timed")
    _alarm([
        "BUILD PERFORMANCE: the build-time tripwire could not attach.",
        "PlatformIO's build phases have moved (a Core upgrade?), so the scan",
        "and setup budgets are not being checked. Update",
        "scripts/memoize_lib_build.py.",
    ])
else:
    def _timed_configure(_env):
        start = time.monotonic()
        result = _configure(_env)
        _scan_s.append(time.monotonic() - start)
        return result

    def _timed_process_deps(_env):
        scans_before = len(_scan_s)
        start = time.monotonic()
        result = _process_deps(_env)
        # Only the scans that ran inside this call count against it.
        scan = sum(_scan_s[scans_before:])
        setup = time.monotonic() - start - scan
        print("[memoize_lib_build] dependency scan %.1f s, library setup %.1f s"
              % (scan, setup))
        over = []
        if scan > _SCAN_BUDGET_S:
            over.append("dependency scan took %.1f s (budget %.0f s)"
                        % (scan, _SCAN_BUDGET_S))
        if setup > _SETUP_BUDGET_S:
            over.append("library setup took %.1f s (budget %.0f s)"
                        % (setup, _SETUP_BUDGET_S))
        if over:
            _alarm(["BUILD PERFORMANCE REGRESSION: " + over[0]] + over[1:] + [
                "Normal is ~45 s scan and ~1 s setup. Find the cause before",
                "living with it: profile the stuck build (py-spy dump --pid).",
            ])
        return result

    env.AddMethod(_timed_configure, "ConfigureProjectLibBuilder")
    env.AddMethod(_timed_process_deps, "ProcessProjectDeps")
