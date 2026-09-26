// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

namespace FactoryReset {
void begin();
void end();
void update();
#ifdef CF_TEST_CLI
void confirmFromCli();
#endif
} // namespace FactoryReset

#endif
