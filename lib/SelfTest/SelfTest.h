// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SELF_TEST_H
#define SELF_TEST_H
namespace SelfTest {
void begin();
void update();
void end();
void command(const char* args);
}
#endif
