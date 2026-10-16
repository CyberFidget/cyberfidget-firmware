// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_CONFIG_H
#define HOME_REMOTE_CONFIG_H

// Home Remote's settings: the Home Assistant address and its access key,
// in NVS namespace "homeremote" (keys "url" and "tok"). Set over USB
// serial with the always-present `hr` verbs (lib/SerialCli/README.md).
// The key is never printed, logged or shown: replies say only whether it
// is set and how long it is.

#include <stddef.h>

namespace HomeRemote {
namespace Config {

/// The saved address ("http://host[:port]"), false when none is saved.
bool url(char* out, size_t cap);
bool hasToken();
/// The saved key. The caller wipes `out` when done with it.
bool token(char* out, size_t cap);

/// "Mirror colour on LEDs" (key "mirror", on unless turned off). Read from
/// NVS once, then kept in RAM; setMirror() saves it.
bool mirror();
bool setMirror(bool on);

/// `hr <args>` from the serial console. `args` is the text after "hr ";
/// the caller wipes the line afterwards (it may hold the key).
void cliCommand(const char* args);

} // namespace Config
} // namespace HomeRemote

#endif // HOME_REMOTE_CONFIG_H
