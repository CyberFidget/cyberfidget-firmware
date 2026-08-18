// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HAL_MOCK_ARDUINO_H
#define HAL_MOCK_ARDUINO_H

constexpr int INPUT = 0;
constexpr int OUTPUT = 1;
constexpr int LOW = 0;
constexpr int HIGH = 1;

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);

#endif // HAL_MOCK_ARDUINO_H
