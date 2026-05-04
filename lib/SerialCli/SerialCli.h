// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include <stddef.h>

// Line-buffered Serial command processor for USB UART. Today's verbs are
// limited to identification (`version`, `info`, `help`); the dispatcher and
// `[<category>] ...` line framing are designed to grow into a full device
// control mode (button injection, app start/stop, accel sim, log streaming).
//
// All output uses a stable line-prefix so a test harness can parse without
// regex acrobatics:
//   [boot] - one-shot boot banner (printed by HAL, not here)
//   [cmd]  - CLI command responses
//   [err]  - CLI errors (overflow, unknown command)
//   [evt]  - reserved for future event streaming
//
// Call SerialCli::instance().poll() once per main loop. Buffer overflow,
// case-insensitive matching, and unknown-verb handling are all internal.
class SerialCli {
public:
    static SerialCli& instance();

    void poll();

    // Buffer size is exposed for testing and for callers that want to reason
    // about the maximum acceptable command length. Anything longer triggers
    // an `[err] line too long` and the buffer resets at the next newline.
    static constexpr size_t kBufferSize = 32;

private:
    SerialCli() = default;
    SerialCli(const SerialCli&) = delete;
    SerialCli& operator=(const SerialCli&) = delete;

    void dispatch(const char* line);
    void cmdVersion();
    void cmdInfo();
    void cmdHelp();

    char   buffer[kBufferSize] = {0};
    size_t bufferLen           = 0;
    bool   overflow            = false;
};

#endif  // SERIAL_CLI_H
