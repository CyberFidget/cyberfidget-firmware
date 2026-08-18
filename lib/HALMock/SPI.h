// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HAL_MOCK_SPI_H
#define HAL_MOCK_SPI_H

class SPIClass {
public:
    void begin(int clock, int miso, int mosi, int cs);
    void end();
};

extern SPIClass SPI;

#endif // HAL_MOCK_SPI_H
