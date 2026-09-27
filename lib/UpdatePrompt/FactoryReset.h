// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

namespace FactoryReset {
void begin();
void end();
void update();
// Early start-up: finishes a reset that a power cut interrupted.
void finishIfInterrupted();
#ifdef CF_TEST_CLI
// holdBetweenErases: wait 10 s after the apps erase (bench power-cut aim).
void confirmFromCli(bool holdBetweenErases = false);
#endif
} // namespace FactoryReset

#endif
