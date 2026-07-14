// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_APP_BASE_H
#define WASM_APP_BASE_H

// Optional base class for AI-generated apps that use inheritance.
// The firmware's built-in apps don't inherit from this, but the AI code
// generator sometimes produces `class MyApp : public App { ... }`.
// This shim makes that pattern compile without error.

class App {
public:
    virtual void begin() {}
    virtual void update() {}
    virtual void end() {}
    virtual ~App() = default;
};

#endif
