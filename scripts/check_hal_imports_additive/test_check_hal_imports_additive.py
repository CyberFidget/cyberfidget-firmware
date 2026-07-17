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
    def run_check(self, base_imports=IMPORTS, head_imports=IMPORTS, base_abi=1, head_abi=1):
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
                ])
            return result, stdout.getvalue(), stderr.getvalue()

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

    def test_addition_passes(self):
        result, output, _ = self.run_check(head_imports=IMPORTS + 'CF_IMPORT("gamma") void cf_gamma(void);\n')
        self.assertEqual(0, result)
        self.assertIn('PASS', output)

    def test_removal_with_abi_bump_passes(self):
        result, output, _ = self.run_check(
            head_imports='CF_IMPORT("alpha") int32_t cf_alpha(int32_t value);\n',
            head_abi=2,
        )
        self.assertEqual(0, result)
        self.assertIn('major changed from 1 to 2', output)

    def test_abi_decrease_fails(self):
        result, _, error = self.run_check(base_abi=2, head_abi=1)
        self.assertEqual(1, result)
        self.assertIn('ABI major must not move backwards', error)

    def test_unparseable_file_fails_loudly(self):
        result, _, error = self.run_check(head_imports='CF_IMPORT("alpha") this is not a declaration;')
        self.assertEqual(1, result)
        self.assertIn('could not parse declaration for import "alpha"', error)


if __name__ == '__main__':
    unittest.main()
