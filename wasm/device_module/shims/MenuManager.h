// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side MenuManager shim: returnToMenu() asks the host to exit the wasm
// app. The host defers the actual switch until the current guest call
// returns (see WasmAppShell).

#ifndef MENU_MANAGER_H  // same guard as the real header — must shadow it
#define MENU_MANAGER_H

#include "cf_hal_imports.h"
#include "globals.h"  // apps reach millis_NOW / TAG_MAIN through this header

class MenuManager {
public:
    static MenuManager& instance();
    void returnToMenu() { cf_exit_to_menu(); }
};

#endif  // MENU_MANAGER_H
