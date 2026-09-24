// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdint.h>

namespace DeviceIdentity {

struct Fingerprint {
    char id[13] = {0};
    char flashId[17] = {0};
    char serial[9] = {0};
};

bool validFlashReads(bool firstOk, uint64_t first, bool secondOk, uint64_t second);
void hex64(uint64_t value, char out[17]);
void serialHex(uint32_t value, char out[9]);
bool matches(const Fingerprint& stored, const Fingerprint& live, bool releaseBuild);

#ifndef HOST_TEST
Fingerprint readLive();
// Clears all local link state on a mismatch. Never sends a request.
bool checkStored(bool clean = true);
bool takeMismatchNotice();
#endif

} // namespace DeviceIdentity

#endif
