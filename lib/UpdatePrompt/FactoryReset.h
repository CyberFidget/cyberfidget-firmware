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
// fault: MarkWrite makes the mark write fail (the reset must refuse);
// Format makes the apps erase fail now and on the start-up after the next
// software restart (the finish must end as partial, not done).
enum class Fault { None, MarkWrite, Format };
void confirmFromCli(bool holdBetweenErases = false, Fault fault = Fault::None);
#endif
} // namespace FactoryReset

#endif
