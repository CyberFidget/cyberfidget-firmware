#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# T-192: compile ONE user app to a DEVICE-PROFILE wasm (wasm3 / cf.* imports,
# app_* exports, <=1MB memory) - the recipe the website compile workflow
# ports. Same user source that builds for the browser emulator; only the HAL
# backend (device shims) and link flags differ.
#
# Usage: build_custom_device_app.sh <AppName> <instance> <src_dir> <out.wasm>
#   <AppName>   the class header <AppName>.h in <src_dir>
#   <instance>  the global instance (emulator convention: lowerFirst(AppName))
# Requires emcc 3.1.51 on PATH (emsdk).
set -euo pipefail
APP="$1"; INST="$2"; SRC="$3"; OUT="$4"
HERE="$(cd "$(dirname "$0")" && pwd)"
CF_GFX="$HERE/../../lib/CFGraphics"
em++ -O2 -fno-exceptions -fno-rtti --no-entry -sSTANDALONE_WASM=1 \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=262144 -sMAXIMUM_MEMORY=1048576 \
  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=32768 \
  -mnontrapping-fptoint -mbulk-memory -msign-ext \
  -I "$HERE/shims" -I "$HERE" -I "$SRC" -I "$CF_GFX/include" \
  -DCF_WASM_APP_CUSTOM "-DCF_CUSTOM_APP_HEADER=\"${APP}.h\"" "-DCUSTOM_APP_INSTANCE=${INST}" \
  -include "$HERE/shims/cf_custom_app_device.h" \
  "$HERE/shims/cf_app_glue.cpp" "$SRC/${APP}.cpp" \
  "$CF_GFX/src/cf_gfx_sprite.cpp" \
  "$CF_GFX/src/cf_gfx_actor.cpp" \
  "$CF_GFX/src/cf_gfx_collision.cpp" \
  -o "$OUT"
echo "built device wasm: $OUT ($(wc -c < "$OUT") bytes)"
