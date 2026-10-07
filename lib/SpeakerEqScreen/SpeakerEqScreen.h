// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SPEAKER_EQ_SCREEN_H
#define SPEAKER_EQ_SCREEN_H

// Settings > Sound > Speaker EQ (APP_SPEAKER_EQ): Left / Right step through
// the presets in lib/AudioManager/SpeakerEqPresets.h. Each step applies at
// once and plays a short preview; AudioManager saves the choice when no
// sound is playing. Select goes back. Bluetooth output is not affected (the
// EQ is part of the on-board speaker path only).

namespace SpeakerEqScreen {

void begin();
void end();
void update();

} // namespace SpeakerEqScreen

#endif
