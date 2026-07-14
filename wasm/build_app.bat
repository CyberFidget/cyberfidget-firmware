@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Copyright (c) 2023-2026 Dismo Industries LLC
REM
REM Build ONE built-in CyberFidget app to WASM for local emulator development.
REM build_wasm.bat only knows how to build the Demo or a Custom two-file app, but
REM CMakeLists.txt already has branches for every library app (Spaceship, DinoGame,
REM Breakout, ...). This wrapper just passes -DWASM_APP=<name> through to CMake so
REM you can iterate on a firmware app in the browser without compile-and-flash.
REM
REM Usage:
REM   build_app.bat Spaceship
REM     -> build\cyberfidget.js, build\cyberfidget.wasm  (WASM_APP=Spaceship)
REM
REM Then serve this directory and open preview.html:
REM   C:\php\php.exe -S localhost:8000      (or: python -m http.server 8000)
REM   http://localhost:8000/preview.html
REM
REM Emscripten is auto-activated the same way build_wasm.bat does it (workspace-root
REM emsdk\, then %USERPROFILE%\emsdk, or EMSDK_ROOT). emsdk 3.1.51 is expected.

setlocal
cd /d "%~dp0"
set "BUILD_DIR=%~dp0build"

if "%~1"=="" (
    echo Usage: %~nx0 ^<AppName^>
    echo   e.g. %~nx0 Spaceship
    echo   Valid names are the WASM_APP branches in CMakeLists.txt
    echo   ^(Spaceship, DinoGame, Breakout, Flashlight, MatrixScreensaver, ...^).
    exit /b 1
)
set "APP_NAME=%~1"

REM --- Locate Emscripten (same logic as build_wasm.bat) ---
where emcc >nul 2>nul
if not errorlevel 1 goto emcc_ready

set "EMSDK_TRY="
if defined EMSDK_ROOT (
    if exist "%EMSDK_ROOT%\emsdk_env.bat" set "EMSDK_TRY=%EMSDK_ROOT%"
)
if not defined EMSDK_TRY (
    if exist "%~dp0..\..\emsdk\emsdk_env.bat" set "EMSDK_TRY=%~dp0..\..\emsdk"
)
if not defined EMSDK_TRY (
    if exist "%USERPROFILE%\emsdk\emsdk_env.bat" set "EMSDK_TRY=%USERPROFILE%\emsdk"
)
if not defined EMSDK_TRY (
    echo Emscripten not found. Install at %~dp0..\..\emsdk or %USERPROFILE%\emsdk,
    echo or set EMSDK_ROOT. Reference: https://emscripten.org/docs/getting_started/downloads.html
    exit /b 1
)

echo === Activating Emscripten at %EMSDK_TRY% ===
call "%EMSDK_TRY%\emsdk_env.bat"
where emcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: emsdk_env.bat ran but emcc still not on PATH. Check %EMSDK_TRY%.
    exit /b 1
)

:emcc_ready
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo === Building WASM app: %APP_NAME% ===
call emcmake cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DWASM_APP=%APP_NAME%
if errorlevel 1 (
    echo ERROR: cmake failed
    exit /b 1
)

call emmake ninja
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Done. Output: build\cyberfidget.js, build\cyberfidget.wasm
echo Serve this folder and open preview.html to run %APP_NAME% in the browser.
exit /b 0
