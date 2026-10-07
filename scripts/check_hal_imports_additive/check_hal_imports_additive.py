#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC
"""Check that the HAL import surface stays additive forever."""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Optional


IMPORT_START_RE = re.compile(
    r'\bCF_IMPORT\s*\(\s*((?:"[^"]*"\s*)+)'
    r'(?:,\s*(\d+)\s*,\s*"([^"]+)"\s*)?\)')
DECLARATION_RE = re.compile(
    r'^\s*(?P<return_type>.+?)\s+(?P<symbol>[A-Za-z_]\w*)\s*'
    r'\((?P<parameters>.*)\)\s*;\s*$',
    re.DOTALL,
)
ABI_RE = re.compile(r'^\s*#\s*define\s+CF_HAL_ABI\s+(\d+)\s*(?://.*)?$', re.MULTILINE)
HOST_ROW_RE = re.compile(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*"([A-Za-z0-9_]+)"\s*,\s*"([^"]*)"\s*,')
COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\r\n]*', re.DOTALL)


class CheckError(Exception):
    """Raised when an input cannot be checked safely."""


@dataclass(frozen=True)
class Import:
    name: str
    symbol: str
    return_type: str
    parameters: str
    since: int = 1
    wasm_signature: str = ''

    @property
    def signature(self) -> str:
        return f"{self.return_type} {self.symbol}({self.parameters})"


def _normalize(fragment: str) -> str:
    fragment = COMMENT_RE.sub(" ", fragment)
    fragment = re.sub(r'\s+', ' ', fragment).strip()
    fragment = re.sub(r'\s*([,*])\s*', r'\1', fragment)
    return fragment


def parse_imports(text: str, source: str) -> Dict[str, Import]:
    text = COMMENT_RE.sub(' ', text)
    imports: Dict[str, Import] = {}
    starts = list(IMPORT_START_RE.finditer(text))
    if len(starts) != len(re.findall(r'\bCF_IMPORT\s*\(\s*"', text)):
        raise CheckError(f'{source}: malformed CF_IMPORT name, since level, or signature')
    if not starts:
        raise CheckError(f"{source}: no CF_IMPORT declarations found")

    for match in starts:
        name = ''.join(re.findall(r'"([^"]*)"', match.group(1)))
        if '.' not in name:
            name = 'cf.' + name   # headers predating the table named cf imports bare
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
            since=int(match.group(2) or 1),
            wasm_signature=match.group(3) or '',
        )
    return imports


def parse_abi(text: str, source: str) -> int:
    matches = ABI_RE.findall(text)
    if len(matches) != 1:
        raise CheckError(f"{source}: expected exactly one integer CF_HAL_ABI definition")
    return int(matches[0])


def parse_host_table(text: str) -> Dict[str, str]:
    """module.name -> wasm signature, from a device host link table
    ({ "module", "name", "sig", &fn } rows in WasmHostImports.cpp)."""
    rows = {f'{m}.{n}': sig for m, n, sig in HOST_ROW_RE.findall(COMMENT_RE.sub(' ', text))}
    if not rows:
        raise CheckError('base host table: no { "module", "name", "sig" } rows found')
    return rows


def check(base_imports: Dict[str, Import], head_imports: Dict[str, Import], head_abi: int = 1,
          base_abi: int = 0, base_host: Optional[Dict[str, str]] = None) -> Iterable[str]:
    # base_host: for a base from before the import table, what its device
    # actually linked. Its header listed only the cf.* calls, without wasm
    # signatures, and not nop or the WASI/env stubs.
    host = base_host or {}
    for name, base in base_imports.items():
        head = head_imports.get(name)
        base_wasm = base.wasm_signature or host.get(name, '')
        if head is None:
            yield f'{name}: import is missing or renamed'
        elif head.symbol != base.symbol:
            yield f'{name}: C symbol changed from {base.symbol} to {head.symbol}'
        elif (head.return_type, head.parameters) != (base.return_type, base.parameters):
            yield f'{name}: signature changed from "{base.signature}" to "{head.signature}"'
        elif base_wasm and head.wasm_signature != base_wasm:
            yield f'{name}: wasm signature changed from "{base_wasm}" to "{head.wasm_signature}"'
        if head and head.since != base.since:
            yield f'{name}: since changed from {base.since} to {head.since}'
    for name, sig in host.items():
        if name in base_imports:
            continue
        head = head_imports.get(name)
        if head is None:
            yield f'{name}: import the base device provided is missing or renamed'
        elif head.wasm_signature != sig:
            yield f'{name}: wasm signature changed from "{sig}" to "{head.wasm_signature}"'
        elif head.since != 1:
            yield f'{name}: the base device provided it, so since must be 1, not {head.since}'
    for name, head in head_imports.items():
        if head.since < 1:
            yield f'{name}: since must be positive'
        if name in host:
            continue   # existed on the base device: checked above
        if name not in base_imports and head.since != head_abi:
            yield f'{name}: new import since {head.since} must equal CF_HAL_ABI {head_abi}'
        # A level the base already provides may be in released firmware, whose
        # devices accept that level's stamp: a new import needs a new level.
        if name not in base_imports and head.since <= base_abi:
            yield f'{name}: new import since {head.since} must be above the base CF_HAL_ABI {base_abi}'
    if max(row.since for row in head_imports.values()) != head_abi:
        yield 'CF_HAL_ABI must equal max(since)'


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
    parser.add_argument('--base-host', help='for a base from before the import table: '
                        'its WasmHostImports.cpp (what the device linked)')
    return parser


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = make_parser().parse_args(argv)
    if (args.base_ref and not args.base_abi and args.head == 'wasm/device_module/cf_hal_imports.h'
            and args.head_abi == 'wasm/device_module/cf_hal_abi.h'):
        return subprocess.call([sys.executable, str(Path(__file__).resolve().parents[2] /
                                'wasm/device_module/check_interface_additive.py'),
                                '--base-ref', args.base_ref])
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
        if head_abi < base_abi:
            raise CheckError(
                f'CF_HAL_ABI decreased from {base_abi} to {head_abi}; '
                'the ABI level must not move backwards'
            )

        base_host = parse_host_table(_read(args.base_host)) if args.base_host else None
        problems = list(check(base_imports, head_imports, head_abi, base_abi, base_host))
        if problems:
            for problem in problems:
                print(f'ERROR: {problem}; restore the import or add a new import name', file=sys.stderr)
            return 1
        print(f'PASS: HAL imports are additive at CF_HAL_ABI level {head_abi}')
        return 0
    except CheckError as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
