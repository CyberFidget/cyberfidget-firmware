#!/usr/bin/env python3
"""Read an official public key from the firmware source, without extra packages."""

import argparse
import ast
import pathlib
import re
import sys


SOURCE = pathlib.Path(__file__).resolve().parents[1] / "lib/CloudSync/UpdateSigning.cpp"


def production_keys(text):
    table = re.search(r"static const PublicKey kProductionKeys\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not table:
        raise ValueError("production key table missing")
    entries = re.findall(r'\{\s*"([a-z0-9-]+)"\s*,\s*(k\w+Pem)\s*\}', table.group(1))
    if not entries or len(entries) != table.group(1).count("{"):
        raise ValueError("production key table empty or malformed")
    keys = {}
    for key_id, symbol in entries:
        if key_id in keys:
            raise ValueError(f"duplicate production key id: {key_id}")
        definition = re.search(r"static const char " + re.escape(symbol) + r"\[\]\s*=\s*(.*?);", text, re.S)
        if not definition:
            raise ValueError(f"public key definition missing for {key_id}")
        literals = re.findall(r'"(?:\\.|[^"\\])*"', definition.group(1))
        pem = "".join(ast.literal_eval(literal) for literal in literals)
        if not pem.startswith("-----BEGIN PUBLIC KEY-----\n") or not pem.endswith("-----END PUBLIC KEY-----\n"):
            raise ValueError(f"public key PEM malformed for {key_id}")
        keys[key_id] = pem
    return keys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-table", action="store_true")
    parser.add_argument("--key-id")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not args.check_table and not (args.key_id and args.output):
        parser.error("use --check-table or --key-id with --output")
    try:
        keys = production_keys(SOURCE.read_text(encoding="utf-8"))
        if args.key_id:
            if args.key_id not in keys:
                raise ValueError(f"key id is absent from firmware: {args.key_id}")
            args.output.write_text(keys[args.key_id], encoding="ascii")
    except ValueError as exc:
        print(f"release public key check: {exc}", file=sys.stderr)
        return 1
    print(f"Checked {len(keys)} distinct production public keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
