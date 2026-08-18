// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SPI.h"

#include "HALMock.h"

SPIClass SPI;

void SPIClass::begin(int clock, int miso, int mosi, int cs) {
    HALMock::recordCall(HALMock::CallType::SpiBegin, clock, miso, mosi, cs);
}

void SPIClass::end() {
    HALMock::recordCall(HALMock::CallType::SpiEnd);
}
