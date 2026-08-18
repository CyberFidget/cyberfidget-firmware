// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HAL_MOCK_SD_H
#define HAL_MOCK_SD_H

class SDClass {
public:
    bool begin(int cs);
    void end();
};

extern SDClass SD;

#endif // HAL_MOCK_SD_H
