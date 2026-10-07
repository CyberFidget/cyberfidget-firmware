// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
#include "AudioManager.h"

static AudioManager audio;
static int noteHandle;
static float level, db;
extern "C" {
void app_begin() {
    audio.stopNotes();
    const int first = audio.playNote(330, 0);
    audio.stopNote(first);
    noteHandle = audio.playNote(440, 0);
    audio.enableMic(true);
    level = audio.getMicVolumeLinear();
    db = audio.getMicVolumeDb();
}
void app_update() {}
// Deliberately leave audio running to exercise host-owned cleanup.
void app_end() {}
void app_handle_button(int, int) {}
int test_note_handle() { return noteHandle; }
float test_mic_level() { return level; }
float test_mic_db() { return db; }
}
