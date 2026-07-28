@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
REM Copyright (c) 2023-2026 Dismo Industries LLC
REM
REM Builds the device wasm modules (run ON the Cyber Fidget inside wasm3) and
REM regenerates the embedded C headers in lib/. Run whenever bench.c, the
REM guest shims, or the wrapped app sources change; commit the regenerated
REM headers (the firmware build itself never needs emsdk).
REM
REM Usage: build_device_modules.bat        (from wasm\device_module\)

setlocal
set SCRIPT_DIR=%~dp0

REM --- Locate emsdk: workspace root (sibling of the repos), then fallbacks ---
REM EMSDK_ROOT is the user-facing override, but it is copied into CF_EMSDK
REM immediately: emsdk_env.bat (construct_env) UNSETS every env var starting
REM with EMSDK_ that it didn't set itself, so a variable named EMSDK_ROOT
REM would be clobbered by the very call below.
set CF_EMSDK=%EMSDK_ROOT%
if not defined CF_EMSDK (
    if exist "%SCRIPT_DIR%..\..\..\emsdk\emsdk_env.bat" set CF_EMSDK=%SCRIPT_DIR%..\..\..\emsdk
)
if not defined CF_EMSDK (
    if exist "%USERPROFILE%\emsdk\emsdk_env.bat" set CF_EMSDK=%USERPROFILE%\emsdk
)
if not defined CF_EMSDK (
    echo ERROR: emsdk not found. Set EMSDK_ROOT or install to the workspace root.
    exit /b 1
)

call "%CF_EMSDK%\emsdk_env.bat" >nul 2>&1

cd /d "%SCRIPT_DIR%"

REM --- Shared flags for app modules (Emscripten standalone, no JS runtime) ---
REM ERROR_ON_UNDEFINED_SYMBOLS=0: the cf.* imports resolve on-device, not at
REM link time. Feature flags (-mnontrapping-fptoint -mbulk-memory -msign-ext)
REM are all supported by the vendored wasm3.
set APPFLAGS=-O2 -fno-exceptions -fno-rtti --no-entry -sSTANDALONE_WASM=1 ^
 -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=262144 -sMAXIMUM_MEMORY=1048576 ^
 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=32768 ^
 -mnontrapping-fptoint -mbulk-memory -msign-ext -I shims -I . -I ..\..\lib\CFGraphics\include

echo === bench.wasm (freestanding clang, no libc) ===
"%CF_EMSDK%\upstream\bin\clang.exe" --target=wasm32 -O3 -nostdlib ^
 -Wl,--no-entry -Wl,--strip-all -o bench\bench.wasm bench\bench.c
if errorlevel 1 exit /b 1

echo === reaction.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\ReactionTimeGame -DCF_WASM_APP_REACTION ^
 shims\cf_app_glue.cpp ..\..\lib\ReactionTimeGame\ReactionTimeGame.cpp -o reaction.wasm
if errorlevel 1 exit /b 1

echo === breakout.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\BreakoutGame -DCF_WASM_APP_BREAKOUT ^
 shims\cf_app_glue.cpp ..\..\lib\BreakoutGame\BreakoutGame.cpp -o breakout.wasm
if errorlevel 1 exit /b 1

echo === starburst.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\Starburst -DCF_WASM_APP_STARBURST ^
 shims\cf_app_glue.cpp ..\..\lib\Starburst\Starburst.cpp -o starburst.wasm
if errorlevel 1 exit /b 1

echo === spaceship.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\Spaceship -DCF_WASM_APP_SPACESHIP ^
 shims\cf_app_glue.cpp ..\..\lib\Spaceship\Spaceship.cpp ^
 ..\..\lib\CFGraphics\src\cf_gfx_sprite.cpp ^
 ..\..\lib\CFGraphics\src\cf_gfx_actor.cpp ^
 ..\..\lib\CFGraphics\src\cf_gfx_collision.cpp -o spaceship.wasm
if errorlevel 1 exit /b 1

echo === sphfluid.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\SPHFluidGame -DCF_WASM_APP_SPHFLUID ^
 shims\cf_app_glue.cpp ..\..\lib\SPHFluidGame\SPHFluidGame.cpp -o sphfluid.wasm
if errorlevel 1 exit /b 1

echo === splooty.wasm ===
call em++ %APPFLAGS% -I ..\..\lib\Splooty -DCF_WASM_APP_SPLOOTY ^
 shims\cf_app_glue.cpp ..\..\lib\Splooty\Splooty.cpp -o splooty.wasm
if errorlevel 1 exit /b 1

echo.
echo All device modules built and headers regenerated.
endlocal

