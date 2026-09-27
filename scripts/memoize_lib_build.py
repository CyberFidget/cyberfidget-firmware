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
script prints a warning and does nothing.
"""

import sys

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

if _patched:
    print("[memoize_lib_build] library setup memoized")
else:
    print("[memoize_lib_build] WARNING: LibBuilderBase not found; not patched")
