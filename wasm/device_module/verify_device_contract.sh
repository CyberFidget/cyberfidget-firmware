#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Verify a device-profile WASM module's exports, imports, memory, and HAL ABI.
#
# Usage: verify_device_contract.sh <path-to-device.wasm>
# Requires wasm-dis (binaryen) on PATH.
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "::error::Usage: verify_device_contract.sh <path-to-device.wasm>"
  exit 1
fi

DEVICE_WASM="$1"
DEVICE_WAT="${DEVICE_WASM%.wasm}.wat"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v wasm-dis >/dev/null 2>&1; then
  echo "::error::wasm-dis (binaryen) not on PATH"
  exit 1
fi

if ! wasm-dis "$DEVICE_WASM" -o "$DEVICE_WAT"; then
  echo "::error::Unable to disassemble device WASM: $DEVICE_WASM"
  exit 1
fi

exports_ok=true
for export_name in app_begin app_update app_end app_handle_button; do
  if grep -q "(export \"${export_name}\"" "$DEVICE_WAT"; then
    echo "Device WASM export ${export_name}: present"
  else
    echo "::error::Device WASM export ${export_name} is missing"
    exports_ok=false
  fi
done

imports_ok=true
invalid_import_count=0
while IFS= read -r import_module; do
  case "$import_module" in
    cf|wasi_snapshot_preview1|env) ;;
    *) invalid_import_count=$((invalid_import_count + 1)) ;;
  esac
done < <(sed -nE 's/^[[:space:]]*\(import "([^"]+)".*/\1/p' "$DEVICE_WAT")
if [ "$invalid_import_count" -eq 0 ]; then
  echo "Device WASM import modules: allowed"
else
  echo "::error::Device WASM imports use ${invalid_import_count} unsupported module(s)"
  imports_ok=false
fi

initial_mem_ok=true
memory_line="$(grep -m1 -E '^[[:space:]]*\(memory ' "$DEVICE_WAT" || true)"
initial_pages="$(printf '%s\n' "$memory_line" | awk '{for (i = 2; i <= NF; i++) {gsub(/[()]/, "", $i); if ($i ~ /^[0-9]+$/) {print $i; exit}}}')"
if [ -z "$initial_pages" ] || [ "$initial_pages" -gt 16 ]; then
  echo "::error::Device WASM initial memory is missing or exceeds 1 MiB"
  initial_mem_ok=false
else
  echo "Device WASM initial memory: ${initial_pages} page(s), within 1 MiB"
fi

if [ "$exports_ok" != true ] || [ "$imports_ok" != true ] || [ "$initial_mem_ok" != true ]; then
  exit 1
fi

hal_abi="$(grep -E '^[[:space:]]*#define[[:space:]]+CF_HAL_ABI[[:space:]]+[0-9]+' "$REPO_ROOT/wasm/device_module/cf_hal_abi.h" | awk '{print $3}' || true)"
if ! [[ "$hal_abi" =~ ^[0-9]+$ ]]; then
  echo "::error::Unable to determine CF_HAL_ABI from cf_hal_abi.h"
  exit 1
fi

device_wasm_basename="$(basename "$DEVICE_WASM")"
device_wasm_dir="$(dirname "$DEVICE_WASM")"
printf '{ "hal_abi": %s, "exports_ok": true, "imports_ok": true, "initial_mem_ok": true, "device_wasm": "%s" }\n' \
  "$hal_abi" "$device_wasm_basename" > "$device_wasm_dir/device-build-meta.json"
echo "Device WASM contract: passed"
