#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import check_hal_imports_additive as checker


IMPORTS = '''
#define CF_IMPORT(NAME) ignored
CF_IMPORT("alpha") int32_t cf_alpha(int32_t value);
CF_IMPORT("beta") void cf_beta(const char * message, int32_t length);
'''
ABI = '#define CF_HAL_ABI {major}\n'


class CheckerTests(unittest.TestCase):
    def run_check(self, base_imports=IMPORTS, head_imports=IMPORTS, base_abi=1, head_abi=1, legacy=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = {
                'base.h': base_imports,
                'head.h': head_imports,
                'base_abi.h': ABI.format(major=base_abi),
                'head_abi.h': ABI.format(major=head_abi),
            }
            for name, content in files.items():
                (root / name).write_text(content, encoding='utf-8')
            stderr = io.StringIO()
            stdout = io.StringIO()
            with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(stdout):
                result = checker.main([
                    '--base', str(root / 'base.h'), '--head', str(root / 'head.h'),
                    '--base-abi', str(root / 'base_abi.h'),
                    '--head-abi', str(root / 'head_abi.h'),
                ] + (['--legacy-base'] if legacy else []))
            return result, stdout.getvalue(), stderr.getvalue()

    # A base header from before the import table: bare cf names, no nop or stubs.
    def test_legacy_base_names_match_module_qualified_head(self):
        head = IMPORTS.replace('"alpha"', '"cf.alpha", 1, "i(i)"').replace('"beta"', '"cf.beta", 1, "v(ii)"')
        result, output, error = self.run_check(head_imports=head, legacy=True)
        self.assertEqual(0, result, error)
        self.assertIn('PASS', output)

    def test_legacy_base_accepts_level_one_rows_it_never_listed(self):
        head = IMPORTS + 'CF_IMPORT("cf.nop", 1, "i(i)") int32_t cf_nop(int32_t x);\n'
        result, _, error = self.run_check(head_imports=head, legacy=True)
        self.assertEqual(0, result, error)

    def test_legacy_base_still_catches_a_removal(self):
        result, _, error = self.run_check(
            head_imports='CF_IMPORT("alpha") int32_t cf_alpha(int32_t value);\n', legacy=True)
        self.assertEqual(1, result)
        self.assertIn('cf.beta: import is missing', error)

    def test_legacy_base_still_requires_new_levels_above_one(self):
        head = IMPORTS + 'CF_IMPORT("cf.gamma", 2, "v()") void cf_gamma(void);\n'
        result, _, error = self.run_check(head_imports=head, head_abi=2, legacy=True)
        self.assertEqual(0, result, error)
        result, _, error = self.run_check(
            head_imports=IMPORTS + 'CF_IMPORT("cf.gamma", 3, "v()") void cf_gamma(void);\n',
            head_abi=2, legacy=True)
        self.assertEqual(1, result)

    def test_level_one_rows_are_new_without_the_legacy_flag(self):
        head = IMPORTS + 'CF_IMPORT("cf.nop", 1, "i(i)") int32_t cf_nop(int32_t x);\n'
        result, _, error = self.run_check(head_imports=head)
        self.assertEqual(1, result)
        self.assertIn('must be above the base CF_HAL_ABI 1', error)

    def test_removal_is_caught(self):
        result, _, error = self.run_check(head_imports=IMPORTS.split('CF_IMPORT("beta")')[0])
        self.assertEqual(1, result)
        self.assertIn('beta: import is missing', error)

    def test_rename_is_caught(self):
        result, _, error = self.run_check(head_imports=IMPORTS.replace('"alpha"', '"renamed"'))
        self.assertEqual(1, result)
        self.assertIn('alpha: import is missing or renamed', error)

    def test_symbol_renumber_is_caught(self):
        result, _, error = self.run_check(head_imports=IMPORTS.replace('cf_alpha', 'cf_alpha_2'))
        self.assertEqual(1, result)
        self.assertIn('C symbol changed', error)

    def test_signature_change_is_caught(self):
        result, _, error = self.run_check(head_imports=IMPORTS.replace('int32_t value', 'float value'))
        self.assertEqual(1, result)
        self.assertIn('signature changed', error)

    def test_addition_at_the_base_level_fails(self):
        result, _, error = self.run_check(head_imports=IMPORTS + 'CF_IMPORT("gamma") void cf_gamma(void);\n')
        self.assertEqual(1, result)
        self.assertIn('must be above the base CF_HAL_ABI 1', error)

    def test_addition_to_a_level_the_base_has_fails(self):
        base = IMPORTS + 'CF_IMPORT("gamma", 2, "v()") void cf_gamma(void);'
        result, _, error = self.run_check(
            base_imports=base, base_abi=2, head_abi=2,
            head_imports=base + 'CF_IMPORT("delta", 2, "v()") void cf_delta(void);')
        self.assertEqual(1, result)
        self.assertIn('delta: new import since 2 must be above the base CF_HAL_ABI 2', error)

    def test_removal_with_abi_bump_fails(self):
        result, _, error = self.run_check(
            head_imports='CF_IMPORT("alpha") int32_t cf_alpha(int32_t value);\n',
            head_abi=2,
        )
        self.assertEqual(1, result)
        self.assertIn('beta: import is missing', error)

    def test_wrong_since_on_new_row_fails(self):
        result, _, error = self.run_check(
            head_imports=IMPORTS + 'CF_IMPORT("gamma", 1, "v()") void cf_gamma(void);', head_abi=2)
        self.assertEqual(1, result)
        self.assertIn('new import since 1 must equal CF_HAL_ABI 2', error)

    def test_changed_since_on_existing_row_fails(self):
        result, _, error = self.run_check(
            head_imports=IMPORTS.replace('CF_IMPORT("alpha")', 'CF_IMPORT("alpha", 2, "i(i)")'), head_abi=2)
        self.assertEqual(1, result)
        self.assertIn('since changed', error)

    def test_correct_new_level_passes(self):
        result, output, _ = self.run_check(
            head_imports=IMPORTS + 'CF_IMPORT("gamma", 2, "v()") void cf_gamma(void);', head_abi=2)
        self.assertEqual(0, result)
        self.assertIn('PASS', output)

    def test_signature_change_with_abi_bump_fails(self):
        head = IMPORTS.replace('int32_t value', 'float value')
        head += 'CF_IMPORT("gamma", 2, "v()") void cf_gamma(void);'
        result, _, error = self.run_check(head_imports=head, head_abi=2)
        self.assertEqual(1, result)
        self.assertIn('signature changed', error)

    def test_stub_removal_with_abi_bump_fails(self):
        stub = 'CF_IMPORT("env." "notify", 1, "v(i)") void cf_stub_notify(int32_t index);'
        head = IMPORTS + 'CF_IMPORT("gamma", 2, "v()") void cf_gamma(void);'
        result, _, error = self.run_check(base_imports=IMPORTS + stub, head_imports=head, head_abi=2)
        self.assertEqual(1, result)
        self.assertIn('env.notify: import is missing', error)

    def test_highest_level_must_match_abi(self):
        result, _, error = self.run_check(head_abi=2)
        self.assertEqual(1, result)
        self.assertIn('CF_HAL_ABI must equal max(since)', error)

    def test_malformed_since_cannot_be_skipped(self):
        head = IMPORTS + 'CF_IMPORT("gamma", -1, "v()") void cf_gamma(void);'
        result, _, error = self.run_check(head_imports=head)
        self.assertEqual(1, result)
        self.assertIn('malformed CF_IMPORT', error)

    def test_abi_decrease_fails(self):
        result, _, error = self.run_check(base_abi=2, head_abi=1)
        self.assertEqual(1, result)
        self.assertIn('ABI level must not move backwards', error)

    def test_unparseable_file_fails_loudly(self):
        result, _, error = self.run_check(head_imports='CF_IMPORT("alpha") this is not a declaration;')
        self.assertEqual(1, result)
        self.assertIn('could not parse declaration for import "cf.alpha"', error)


if __name__ == '__main__':
    unittest.main()
