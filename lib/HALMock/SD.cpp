// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SD.h"

#include "Arduino.h"
#include "HALMock.h"

SDClass SD;

bool SDClass::begin(int cs) {
    HALMock::recordCall(HALMock::CallType::SdBegin, cs);
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    return HALMock::cardPresent();
}

void SDClass::end() {
    HALMock::recordCall(HALMock::CallType::SdEnd);
}
