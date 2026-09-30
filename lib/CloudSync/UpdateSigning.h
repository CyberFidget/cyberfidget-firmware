// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
#ifndef UPDATE_SIGNING_H
#define UPDATE_SIGNING_H

#include "OtaUpdate.h"

namespace UpdateSigning {
bool knownKey(const char* id);
OtaUpdate::VerifyResult verify(const OtaUpdate::Manifest& m, const uint8_t digest[32]);
#ifdef CF_TEST_CLI
bool verifyTestFixture();
#endif
}  // namespace UpdateSigning

#endif
