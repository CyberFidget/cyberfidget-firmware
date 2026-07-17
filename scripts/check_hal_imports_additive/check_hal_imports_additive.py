#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC
"""Check that the HAL import surface stays additive within an ABI major."""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Optional


IMPORT_START_RE = re.compile(r'\bCF_IMPORT\s*\(\s*"([^"]+)"\s*\)')
DECLARATION_RE = re.compile(
    r'^\s*(?P<return_type>.+?)\s+(?P<symbol>[A-Za-z_]\w*)\s*'
    r'\((?P<parameters>.*)\)\s*;\s*$',
    re.DOTALL,
)
ABI_RE = re.compile(r'^\s*#\s*define\s+CF_HAL_ABI\s+(\d+)\s*(?://.*)?$', re.MULTILINE)
COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\r\n]*', re.DOTALL)


class CheckError(Exception):
    """Raised when an input cannot be checked safely."""


@dataclass(frozen=True)
class Import:
    name: str
    symbol: str
    return_type: str
    parameters: str

    @property
    def signature(self) -> str:
        return f"{self.return_type} {self.symbol}({self.parameters})"


def _normalize(fragment: str) -> str:
    fragment = COMMENT_RE.sub(" ", fragment)
    fragment = re.sub(r'\s+', ' ', fragment).strip()
    fragment = re.sub(r'\s*([,*])\s*', r'\1', fragment)
    return fragment


def parse_imports(text: str, source: str) -> Dict[str, Import]:
    imports: Dict[str, Import] = {}
    starts = list(IMPORT_START_RE.finditer(text))
    if not starts:
        raise CheckError(f"{source}: no CF_IMPORT declarations found")

    for match in starts:
        name = match.group(1)
        semicolon = text.find(';', match.end())
        next_start = IMPORT_START_RE.search(text, match.end())
        if semicolon < 0 or (next_start and next_start.start() < semicolon):
            raise CheckError(f'{source}: could not parse declaration for import "{name}"')
        declaration = COMMENT_RE.sub(' ', text[match.end():semicolon + 1])
        parsed = DECLARATION_RE.match(declaration)
        if not parsed:
            raise CheckError(f'{source}: could not parse declaration for import "{name}"')
        if name in imports:
            raise CheckError(f'{source}: duplicate import name "{name}"')
        imports[name] = Import(
            name=name,
            symbol=parsed.group('symbol'),
            return_type=_normalize(parsed.group('return_type')),
            parameters=_normalize(parsed.group('parameters')),
        )
    return imports


def parse_abi(text: str, source: str) -> int:
    matches = ABI_RE.findall(text)
    if len(matches) != 1:
        raise CheckError(f"{source}: expected exactly one integer CF_HAL_ABI definition")
    return int(matches[0])


def check(base_imports: Dict[str, Import], head_imports: Dict[str, Import]) -> Iterable[str]:
    for name, base in base_imports.items():
        head = head_imports.get(name)
        if head is None:
            yield f'{name}: import is missing or renamed'
        elif head.symbol != base.symbol:
            yield f'{name}: C symbol changed from {base.symbol} to {head.symbol}'
        elif (head.return_type, head.parameters) != (base.return_type, base.parameters):
            yield f'{name}: signature changed from "{base.signature}" to "{head.signature}"'


def _git_show(ref: str, path: str) -> str:
    result = subprocess.run(
        ['git', 'show', f'{ref}:{path}'], capture_output=True, text=True, check=False
    )
    if result.returncode:
        detail = result.stderr.strip() or 'git show failed'
        raise CheckError(f"could not read {path} at {ref}: {detail}")
    return result.stdout


def _read(path: str) -> str:
    try:
        return Path(path).read_text(encoding='utf-8')
    except (OSError, UnicodeError) as exc:
        raise CheckError(f"could not read {path}: {exc}") from exc


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--base', help='base cf_hal_imports.h file')
    source.add_argument('--base-ref', help='git ref containing both base headers')
    parser.add_argument('--head', default='wasm/device_module/cf_hal_imports.h')
    parser.add_argument('--base-abi', help='base cf_hal_abi.h file (required with --base)')
    parser.add_argument('--head-abi', default='wasm/device_module/cf_hal_abi.h')
    return parser


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        if args.base_ref:
            if args.base_abi:
                raise CheckError('--base-abi cannot be used with --base-ref')
            base_imports_text = _git_show(args.base_ref, 'wasm/device_module/cf_hal_imports.h')
            base_abi_text = _git_show(args.base_ref, 'wasm/device_module/cf_hal_abi.h')
            base_imports_source = f'{args.base_ref}:wasm/device_module/cf_hal_imports.h'
            base_abi_source = f'{args.base_ref}:wasm/device_module/cf_hal_abi.h'
        else:
            if not args.base_abi:
                raise CheckError('--base-abi is required with --base')
            base_imports_text = _read(args.base)
            base_abi_text = _read(args.base_abi)
            base_imports_source, base_abi_source = args.base, args.base_abi

        head_imports_text = _read(args.head)
        head_abi_text = _read(args.head_abi)
        base_imports = parse_imports(base_imports_text, base_imports_source)
        head_imports = parse_imports(head_imports_text, args.head)
        base_abi = parse_abi(base_abi_text, base_abi_source)
        head_abi = parse_abi(head_abi_text, args.head_abi)
        if head_abi > base_abi:
            print(f'PASS: CF_HAL_ABI major changed from {base_abi} to {head_abi}')
            return 0
        if head_abi < base_abi:
            raise CheckError(
                f'CF_HAL_ABI major decreased from {base_abi} to {head_abi}; '
                'the ABI major must not move backwards'
            )

        problems = list(check(base_imports, head_imports))
        if problems:
            for problem in problems:
                print(f'ERROR: {problem}; bump CF_HAL_ABI (major) or restore the import', file=sys.stderr)
            return 1
        print(f'PASS: HAL imports are additive within CF_HAL_ABI major {head_abi}')
        return 0
    except CheckError as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
