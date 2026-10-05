#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2026 Dismo Industries LLC
"""Preprocess table declarations for the existing additive-ABI guard."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def git_file(ref, name, optional=False):
    result = subprocess.run(['git', 'show', f'{ref}:wasm/device_module/{name}'],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        if optional:
            return None
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def preprocess(folder, compiler):
    return subprocess.check_output([compiler, '-E', '-P', '-x', 'c', '-', '-I', str(folder)],
                                   input=(HERE / 'additive_declarations.c').read_text(), text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-ref', required=True)
    parser.add_argument('--cpp', default=os.environ.get('CF_CPP', 'cc'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='cf-additive-') as scratch:
        folder = Path(scratch)
        table = git_file(args.base_ref, 'cf_imports.def', optional=True)
        if table is None:
            base = git_file(args.base_ref, 'cf_hal_imports.h')
        else:
            (folder / 'cf_imports.def').write_text(table)
            (folder / 'cf_params.h').write_text(git_file(args.base_ref, 'cf_params.h'))
            base = preprocess(folder, args.cpp)
        (folder / 'base.h').write_text(base)
        (folder / 'head.h').write_text(preprocess(HERE, args.cpp))
        (folder / 'base-abi.h').write_text(git_file(args.base_ref, 'cf_hal_abi.h'))
        return subprocess.call([
            sys.executable, str(ROOT / 'scripts/check_hal_imports_additive/check_hal_imports_additive.py'),
            '--base', str(folder / 'base.h'), '--head', str(folder / 'head.h'),
            '--base-abi', str(folder / 'base-abi.h'), '--head-abi', str(HERE / 'cf_hal_abi.h')])


if __name__ == '__main__':
    sys.exit(main())
