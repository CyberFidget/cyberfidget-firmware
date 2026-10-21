// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_CONFIG_H
#define HOME_REMOTE_CONFIG_H

// Home Remote's settings: the Home Assistant address and its access key,
// in NVS namespace "homeremote" (keys "url" and "tok"). Set in the portal
// or over USB with the always-present `hr` verbs (lib/SerialCli/README.md).
// The key is never printed, logged or shown: replies say only whether it
// is set and how long it is.

#include <stddef.h>
#include <stdint.h>

namespace HomeRemote {
namespace Config {

enum class SaveResult : uint8_t { Ok, InvalidUrl, InvalidKey, Storage };
// Validate and trim without saving. Output capacities are kUrlMax + 1 and
// kTokenMax + 1; callers wipe the key output after use.
SaveResult validateUrlValue(const char* in, char* normalized);
SaveResult validateKeyValue(const char* in, char* trimmed);
SaveResult saveUrlValue(const char* in);
SaveResult saveKeyValue(const char* in);

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
