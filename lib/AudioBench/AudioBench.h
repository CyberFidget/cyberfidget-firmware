// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioBench/AudioBench.h
//
// Test-build-only (CF_TEST_CLI) audio bench verbs, `abench ...` on the serial
// CLI, driven by cyberfidget-hil/tools/audio_bench. They measure the running
// audio engine (lib/AudioEngine + AudioEngineTask): render CPU, write-gap
// underruns, clips, an 8-voice stress pattern, a sine sweep, raw notes, the
// speaker EQ, product tones (AudioManager::playTone) recorded through the
// on-board mic, and flash-write stalls. Grammar and output lines match the
// earlier bench firmware's `aspike` verbs, with the `[abench] ` prefix.
// `abench help` lists the verbs.

#ifndef AUDIO_BENCH_H
#define AUDIO_BENCH_H

namespace AudioBench {
void command(const char* arg);   // loop task (SerialCli)
void poll();                     // loop task, every pass
}

#endif
