#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC

import unittest
from pathlib import Path

import check_shim_drift as checker


class ShimDriftTests(unittest.TestCase):
    def test_layout_mismatch_caught(self):
        real = checker.parse_struct(
            "struct Event { int index; unsigned long duration; };", "Event", Path("real.h")
        )
        shim = checker.parse_struct(
            "struct Event { unsigned long duration; int index; };", "Event", Path("shim.h")
        )
        self.assertNotEqual(real, shim)

    def test_constant_mismatch_caught(self):
        real = checker.parse_constants("constexpr int value = 6;", Path("real.h"))
        shim = checker.parse_constants("static constexpr int value = 5;", Path("shim.h"))
        failures = []
        checker.require_equal("shim.h:value", real["value"], shim["value"], failures)
        self.assertEqual(1, len(failures))

    def test_allowlisted_divergence_passes(self):
        real = checker.parse_methods(
            "class Proxy { public: void draw(int x); };", "Proxy", Path("real.h")
        )
        shim = checker.parse_methods(
            "class Proxy { public: int draw(int x); };", "Proxy", Path("shim.h")
        )
        allowlist = ({"file": "shim.h", "symbol": "draw", "rationale": "adapter"},)
        self.assertEqual([], checker.compare_methods(real, shim, Path("shim.h"), allowlist))

    def test_unlisted_divergence_fails_with_file_and_symbol(self):
        real = checker.parse_methods(
            "class Proxy { public: void draw(int x); };", "Proxy", Path("real.h")
        )
        shim = checker.parse_methods(
            "class Proxy { public: int draw(int x); };", "Proxy", Path("shim.h")
        )
        failures = checker.compare_methods(real, shim, Path("shim.h"), ())
        self.assertEqual(1, len(failures))
        self.assertIn("shim.h:draw", failures[0])

    def test_unparseable_input_fails_loudly(self):
        with self.assertRaisesRegex(checker.CheckError, "unterminated struct Event"):
            checker.parse_struct("struct Event { int index;", "Event", Path("broken.h"))


if __name__ == "__main__":
    unittest.main()
