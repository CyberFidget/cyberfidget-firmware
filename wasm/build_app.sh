#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2023-2026 Dismo Industries LLC
#
# Build ONE built-in CyberFidget app to WASM for local emulator development.
# build_wasm.sh only knows how to build the Demo or a Custom two-file app, but
# CMakeLists.txt already has branches for every library app (Spaceship, DinoGame,
# Breakout, ...). This wrapper passes -DWASM_APP=<name> through to CMake so you can
# iterate on a firmware app in the browser without compile-and-flash.
#
# Usage:
#   ./build_app.sh Spaceship
#     -> build/cyberfidget.js, build/cyberfidget.wasm  (WASM_APP=Spaceship)
#
# Then serve this directory and open preview.html:
#   python3 -m http.server 8000
#   http://localhost:8000/preview.html
#
# Emscripten is auto-activated the same way build_wasm.sh does it (workspace-root
# emsdk/, then $HOME/emsdk, or EMSDK_ROOT). emsdk 3.1.51 is expected.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if [ -z "$1" ]; then
    echo "Usage: $(basename "$0") <AppName>"
    echo "  e.g. $(basename "$0") Spaceship"
    echo "  Valid names are the WASM_APP branches in CMakeLists.txt"
    echo "  (Spaceship, DinoGame, Breakout, Flashlight, MatrixScreensaver, ...)."
    exit 1
fi
APP_NAME="$1"

# --- Locate Emscripten (same logic as build_wasm.sh) ---
if ! command -v emcc >/dev/null 2>&1; then
    EMSDK_TRY=""
    if [ -n "$EMSDK_ROOT" ] && [ -f "$EMSDK_ROOT/emsdk_env.sh" ]; then
        EMSDK_TRY="$EMSDK_ROOT"
    elif [ -f "$SCRIPT_DIR/../../emsdk/emsdk_env.sh" ]; then
        EMSDK_TRY="$SCRIPT_DIR/../../emsdk"
    elif [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
        EMSDK_TRY="$HOME/emsdk"
    fi
    if [ -z "$EMSDK_TRY" ]; then
        echo "Emscripten not found. Install at $SCRIPT_DIR/../../emsdk or \$HOME/emsdk,"
        echo "or set EMSDK_ROOT. Reference: https://emscripten.org/docs/getting_started/downloads.html"
        exit 1
    fi
    echo "=== Activating Emscripten at $EMSDK_TRY ==="
    # shellcheck disable=SC1090
    source "$EMSDK_TRY/emsdk_env.sh"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Building WASM app: $APP_NAME ==="
emcmake cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DWASM_APP="$APP_NAME"
emmake ninja

echo
echo "Done. Output: build/cyberfidget.js, build/cyberfidget.wasm"
echo "Serve this folder and open preview.html to run $APP_NAME in the browser."
