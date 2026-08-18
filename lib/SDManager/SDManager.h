// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SD_MANAGER_H
#define SD_MANAGER_H

namespace SDManager {

constexpr int kPinClock = 5;
constexpr int kPinMiso  = 21;
constexpr int kPinMosi  = 19;
constexpr int kPinCs    = 8;

bool mount();
void release();
void releaseForSleep();
bool isMounted();

} // namespace SDManager

#endif // SD_MANAGER_H
