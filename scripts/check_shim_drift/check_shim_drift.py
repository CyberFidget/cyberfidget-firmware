#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC
"""Check device-WASM shim declarations against their firmware counterparts."""

import argparse
import ast
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple


class CheckError(Exception):
    """Raised when an input cannot be parsed safely or violates the contract."""


@dataclass(frozen=True)
class Field:
    type: str
    name: str


@dataclass(frozen=True)
class Method:
    return_type: str
    name: str
    parameters: Tuple[str, ...]
    is_const: bool

    def render(self) -> str:
        suffix = " const" if self.is_const else ""
        return f"{self.return_type} {self.name}({', '.join(self.parameters)}){suffix}"


COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\r\n]*", re.DOTALL)
CONST_RE = re.compile(
    r"\b(?:static\s+)?(?:inline\s+)?(?:constexpr|const)\s+(?:int|uint16_t)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<value>[^;]+);"
)
METHOD_RE = re.compile(
    r"^\s*(?P<return>[A-Za-z_][\w:<>&*\s]*?)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\((?P<params>[^)]*)\)\s*"
    r"(?P<const>const\s*)?(?:;|\{)",
    re.MULTILINE,
)


STRUCT_SPECS = (
    (
        "ButtonEvent",
        Path("lib/ButtonManager/ButtonManager.h"),
        Path("wasm/device_module/shims/ButtonManager.h"),
    ),
    (
        "ToneStep",
        Path("lib/AudioManager/AudioManager.h"),
        Path("wasm/device_module/shims/AudioManager.h"),
    ),
)

HAL_CONSTANTS = (
    "button_TopLeftIndex",
    "button_TopRightIndex",
    "button_MiddleLeftIndex",
    "button_MiddleRightIndex",
    "button_BottomLeftIndex",
    "button_BottomRightIndex",
    "button_LeftIndex",
    "button_RightIndex",
    "button_UpIndex",
    "button_DownIndex",
    "button_SelectIndex",
    "button_EnterIndex",
    "pixel_Front_Top",
    "pixel_Front_Middle",
    "pixel_Front_Bottom",
    "pixel_Back",
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CheckError(f"cannot read {path}: {exc}") from exc


def strip_comments(text: str) -> str:
    return COMMENT_RE.sub("", text)


def braced_body(text: str, kind: str, name: str, source: Path) -> str:
    match = re.search(rf"\b{re.escape(kind)}\s+{re.escape(name)}\b[^{{;]*{{", text)
    if not match:
        raise CheckError(f"{source}: cannot find {kind} {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
    raise CheckError(f"{source}: unterminated {kind} {name}")


def canonical_type(value: str) -> str:
    value = " ".join(value.split())
    value = re.sub(r"\s*([*&])\s*", r"\1", value)
    return value


def parse_struct(text: str, name: str, source: Path) -> Tuple[Field, ...]:
    body = braced_body(strip_comments(text), "struct", name, source)
    fields: List[Field] = []
    for declaration in body.split(";"):
        declaration = " ".join(declaration.split())
        if not declaration:
            continue
        match = re.fullmatch(r"(.+?)\s+([A-Za-z_]\w*)", declaration)
        if not match or any(token in declaration for token in ("(", ")", "{")):
            raise CheckError(
                f"{source}: unparseable field in struct {name}: {declaration!r}"
            )
        fields.append(Field(canonical_type(match.group(1)), match.group(2)))
    if not fields:
        raise CheckError(f"{source}: struct {name} has no parseable fields")
    return tuple(fields)


def split_parameters(parameters: str) -> Iterable[str]:
    start = 0
    depth = 0
    for index, char in enumerate(parameters):
        if char in "(<[":
            depth += 1
        elif char in ")>]":
            depth -= 1
        elif char == "," and depth == 0:
            yield parameters[start:index]
            start = index + 1
    yield parameters[start:]


def parameter_type(parameter: str) -> str:
    parameter = parameter.split("=", 1)[0].strip()
    if not parameter or parameter == "void":
        return ""
    match = re.fullmatch(r"(.+?(?:\s|[*&]))([A-Za-z_]\w*)", parameter)
    if match:
        parameter = match.group(1)
    return canonical_type(parameter)


def parse_methods(text: str, class_name: str, source: Path) -> Tuple[Method, ...]:
    body = strip_comments(braced_body(strip_comments(text), "class", class_name, source))
    public = body.split("private:", 1)[0]
    public = public.split("public:", 1)[-1]
    methods = []
    for match in METHOD_RE.finditer(public):
        parameters = tuple(
            value
            for value in (parameter_type(item) for item in split_parameters(match.group("params")))
            if value
        )
        methods.append(
            Method(
                canonical_type(match.group("return")),
                match.group("name"),
                parameters,
                bool(match.group("const")),
            )
        )
    if not methods:
        raise CheckError(f"{source}: class {class_name} has no parseable public methods")
    return tuple(methods)


def eval_integer(expression: str, values: Mapping[str, int], source: Path, name: str) -> int:
    expression = re.sub(r"(?<=\d)[uUlL]+\b", "", expression.strip())
    try:
        node = ast.parse(expression, mode="eval").body
    except SyntaxError as exc:
        raise CheckError(f"{source}: unparseable constant {name}: {expression!r}") from exc
    if isinstance(node, ast.Constant) and type(node.value) is int:
        return node.value
    if isinstance(node, ast.Name) and node.id in values:
        return values[node.id]
    raise CheckError(
        f"{source}: constant {name} uses unsupported expression {expression!r}"
    )


def parse_constants(text: str, source: Path) -> Dict[str, int]:
    declarations = list(CONST_RE.finditer(strip_comments(text)))
    if not declarations:
        raise CheckError(f"{source}: no parseable integer constant declarations")
    values: Dict[str, int] = {}
    pending = [(match.group("name"), match.group("value")) for match in declarations]
    while pending:
        next_pending = []
        progressed = False
        for name, expression in pending:
            try:
                values[name] = eval_integer(expression, values, source, name)
                progressed = True
            except CheckError:
                next_pending.append((name, expression))
        if not progressed:
            name, expression = next_pending[0]
            eval_integer(expression, values, source, name)
        pending = next_pending
    return values


def load_allowlist(path: Path) -> Tuple[dict, ...]:
    try:
        document = json.loads(read_text(path))
    except json.JSONDecodeError as exc:
        raise CheckError(f"{path}: invalid JSON: {exc}") from exc
    entries = document.get("entries") if isinstance(document, dict) else None
    if not isinstance(entries, list):
        raise CheckError(f"{path}: expected an 'entries' list")
    required = {"file", "symbol", "rationale"}
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != required:
            raise CheckError(f"{path}: entry {index} must contain exactly {sorted(required)}")
        if not all(isinstance(entry[key], str) and entry[key].strip() for key in required):
            raise CheckError(f"{path}: entry {index} contains an empty or non-string value")
    return tuple(entries)


def is_allowlisted(entries: Sequence[dict], file: Path, symbol: str) -> bool:
    normalized = file.as_posix()
    return any(
        entry["file"] == normalized and entry["symbol"] in (symbol, "*")
        for entry in entries
    )


def compare_methods(
    real_methods: Sequence[Method],
    shim_methods: Sequence[Method],
    shim_path: Path,
    allowlist: Sequence[dict],
) -> List[str]:
    failures = []
    real_by_name: Dict[str, set] = {}
    shim_by_name: Dict[str, set] = {}
    for method in real_methods:
        real_by_name.setdefault(method.name, set()).add(method)
    for method in shim_methods:
        shim_by_name.setdefault(method.name, set()).add(method)
    for symbol in sorted(shim_by_name):
        if symbol not in real_by_name:
            if not is_allowlisted(allowlist, shim_path, symbol):
                actual = "; ".join(sorted(item.render() for item in shim_by_name[symbol]))
                failures.append(
                    f"{shim_path.as_posix()}:{symbol}: shim declaration has no real "
                    f"counterpart [{actual}]. Fix the shim or add a narrowly rationalized "
                    "allowlist entry."
                )
            continue
        unmatched = shim_by_name[symbol] - real_by_name[symbol]
        if unmatched and not is_allowlisted(allowlist, shim_path, symbol):
            expected = "; ".join(sorted(item.render() for item in real_by_name[symbol]))
            actual = "; ".join(sorted(item.render() for item in shim_by_name[symbol]))
            failures.append(
                f"{shim_path.as_posix()}:{symbol}: mirrored signature mismatch; "
                f"real [{expected}], shim [{actual}]. Fix the shim or add a narrowly "
                "rationalized allowlist entry."
            )
    return failures


def require_equal(label: str, real: object, shim: object, failures: List[str]) -> None:
    if real != shim:
        failures.append(f"{label}: real {real!r}, shim {shim!r}")


def run(repo_root: Path, allowlist_path: Path) -> List[str]:
    failures: List[str] = []
    allowlist = load_allowlist(allowlist_path)
    for entry in allowlist:
        listed_path = repo_root / entry["file"]
        if not listed_path.is_file():
            raise CheckError(
                f"{allowlist_path}: allowlist path does not exist: {entry['file']}"
            )

    for name, real_relative, shim_relative in STRUCT_SPECS:
        real = parse_struct(read_text(repo_root / real_relative), name, real_relative)
        shim = parse_struct(read_text(repo_root / shim_relative), name, shim_relative)
        require_equal(f"{shim_relative.as_posix()}:{name} layout", real, shim, failures)

    real_hal_path = Path("lib/HAL/HAL.h")
    shim_hal_path = Path("wasm/device_module/shims/HAL.h")
    real_hal = parse_constants(read_text(repo_root / real_hal_path), real_hal_path)
    shim_hal = parse_constants(read_text(repo_root / shim_hal_path), shim_hal_path)
    for name in HAL_CONSTANTS:
        if name not in real_hal or name not in shim_hal:
            raise CheckError(f"{name}: missing from {real_hal_path} or {shim_hal_path}")
        require_equal(f"{shim_hal_path.as_posix()}:{name}", real_hal[name], shim_hal[name], failures)

    globals_path = Path("lib/Globals/globals.h")
    button_shim_path = Path("wasm/device_module/shims/ButtonManager.h")
    globals_constants = parse_constants(read_text(repo_root / globals_path), globals_path)
    button_constants = parse_constants(read_text(repo_root / button_shim_path), button_shim_path)
    if "numButtons" not in globals_constants or "kMaxButtons" not in button_constants:
        raise CheckError("numButtons or kMaxButtons declaration is missing")
    require_equal(
        f"{button_shim_path.as_posix()}:kMaxButtons",
        globals_constants["numButtons"],
        button_constants["kMaxButtons"],
        failures,
    )

    adapter_pairs = (
        ("ButtonManager", Path("lib/ButtonManager/ButtonManager.h"), button_shim_path),
        (
            "AudioManager",
            Path("lib/AudioManager/AudioManager.h"),
            Path("wasm/device_module/shims/AudioManager.h"),
        ),
        (
            "DisplayProxy",
            Path("lib/DisplayProxy/DisplayProxy.h"),
            Path("wasm/device_module/shims/DisplayProxy.h"),
        ),
    )
    for class_name, real_path, shim_path in adapter_pairs:
        failures.extend(
            compare_methods(
                parse_methods(read_text(repo_root / real_path), class_name, real_path),
                parse_methods(read_text(repo_root / shim_path), class_name, shim_path),
                shim_path,
                allowlist,
            )
        )

    platformio_path = Path("platformio.ini")
    platformio = read_text(repo_root / platformio_path)
    dependency = "thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays@^4.6.1"
    if dependency not in platformio:
        failures.append(
            f"{platformio_path.as_posix()}:ThingPulse dependency mapping changed; "
            f"expected {dependency!r}"
        )
    return failures


def main(argv: Sequence[str] = ()) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=Path(__file__).with_name("adapter_allowlist.json"),
    )
    args = parser.parse_args(argv or None)
    try:
        failures = run(args.repo_root.resolve(), args.allowlist.resolve())
    except CheckError as exc:
        print(f"shim drift check ERROR: {exc}", file=sys.stderr)
        return 2
    if failures:
        print("shim drift check FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("shim drift check PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
