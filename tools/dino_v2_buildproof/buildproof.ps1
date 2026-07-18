# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
[CmdletBinding()]
param(
    [string]$WebsiteRoot,
    [string]$EmsdkRoot
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $WebsiteRoot) {
    $WebsiteRoot = Join-Path (Split-Path $RepoRoot -Parent) 'cyberfidget_website'
}
if (-not $EmsdkRoot) {
    $EmsdkRoot = Join-Path (Split-Path $RepoRoot -Parent) 'emsdk'
}
$env:CF_WEBSITE_ROOT = (Resolve-Path $WebsiteRoot).Path

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')"
    }
}

Push-Location $RepoRoot
try {
    foreach ($Asset in @('dino', 'cactus', 'pterodactyl', 'ground')) {
        Invoke-Checked 'node' @(
            'tools/cfsprite_compile_cli.mjs', '--check-only',
            '--symbol-prefix', $Asset,
            "lib/DinoGame/assets/$Asset.cfsprite.json",
            "lib/DinoGame/generated/$Asset.h"
        )
    }

    Invoke-Checked 'node' @('tools/dino_v2_buildproof/prepare_fold.mjs')
    $IsWindowsHost = $IsWindows -or $env:OS -eq 'Windows_NT'
    $Python = 'python'
    if ($IsWindowsHost) {
        $env:EMSDK_PYTHON = Join-Path $EmsdkRoot 'python\3.13.3_64bit\python.exe'
        $env:EM_CONFIG = Join-Path $EmsdkRoot '.emscripten'
        $env:PATH = (@(
            (Join-Path $EmsdkRoot 'upstream\emscripten'),
            (Join-Path $EmsdkRoot 'upstream\bin'),
            (Join-Path $EmsdkRoot 'node\22.16.0_64bit\bin'),
            $env:PATH
        ) -join [IO.Path]::PathSeparator)
        $Emcmake = 'emcmake.bat'
        $WasmDis = Join-Path $EmsdkRoot 'upstream\bin\wasm-dis.exe'
        $Python = $env:EMSDK_PYTHON
    } else {
        $Emcmake = 'emcmake'
        $WasmDis = 'wasm-dis'
    }

    New-Item -ItemType Directory -Force -Path 'wasm/generated' | Out-Null
    $env:CYBERFIDGET_BUILD_TYPE_OVERRIDE = 'wasm'
    Invoke-Checked $Python @(
        'scripts/generate_version.py', '--standalone',
        '--out', 'wasm/generated/version.h', '--repo-root', '.'
    )

    $BuildDir = 'wasm/build/dino-v2-buildproof'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Invoke-Checked $Emcmake @(
        'cmake', '-S', 'wasm', '-B', $BuildDir, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DWASM_APP=Custom',
        '-DCUSTOM_APP_NAME=DinoGame', '-DCUSTOM_APP_INSTANCE=dinoGame'
    )
    Invoke-Checked 'cmake' @('--build', $BuildDir)

    $EmulatorWasm = Join-Path $BuildDir 'cyberfidget.wasm'
    $EmulatorWat = Join-Path $BuildDir 'cyberfidget.wat'
    Invoke-Checked $WasmDis @($EmulatorWasm, '-o', $EmulatorWat)
    foreach ($Export in @('main', 'wasm_button_press', 'wasm_button_release')) {
        if (-not (Select-String -Quiet -SimpleMatch "(export `"$Export`"" $EmulatorWat)) {
            throw "Emulator WASM export $Export is missing"
        }
    }
    $MissingRequestedExports = @('app_begin', 'app_update') | Where-Object {
        -not (Select-String -Quiet -SimpleMatch "(export `"$_`"" $EmulatorWat)
    }

    $DeviceWasm = 'wasm/build/dino-v2-buildproof/cyberfidget.device.wasm'
    if ($IsWindowsHost) {
        Invoke-Checked 'bash' @(
            'wasm/device_module/build_custom_device_app.sh',
            'DinoGame', 'dinoGame', 'wasm/app', $DeviceWasm
        )
    } else {
        Invoke-Checked 'bash' @(
            'wasm/device_module/build_custom_device_app.sh',
            'DinoGame', 'dinoGame', 'wasm/app', $DeviceWasm
        )
    }
    Invoke-Checked 'bash' @('wasm/device_module/verify_device_contract.sh', $DeviceWasm)

    "Emulator JS: $((Get-Item (Join-Path $BuildDir 'cyberfidget.js')).Length) bytes"
    "Emulator WASM: $((Get-Item $EmulatorWasm).Length) bytes"
    "Device WASM: $((Get-Item $DeviceWasm).Length) bytes"
    if ($MissingRequestedExports.Count) {
        throw "Requested emulator export(s) missing: $($MissingRequestedExports -join ', ')"
    }
} finally {
    Pop-Location
}
