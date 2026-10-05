// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/AudioEngineTask.h
//
// Device glue for the audio engine (lib/AudioEngine): a render task that owns
// I2S0 (IDF i2s_std, 44.1 kHz, 16-bit stereo slots carrying the mono mix,
// Philips, BCLK 26 / WS 27 / DOUT 14) and feeds it one block at a time. The
// blocking channel write is the clock.
//
// Control side (loop task, or the device-module guest task while the loop
// task waits on it - never both at once): start/stop/send/engine. Commands go
// through a FreeRTOS queue and are applied by the render task at the start of
// each block; engine state is touched only by the render task.
//
// stop() fades every voice, lets the DMA cushion play out silence, joins the
// task (acknowledged exit, bounded wait) and only then disables and deletes
// the channel and frees the buffers. If the task does not exit in time,
// nothing is deleted: the port stays held and an error is logged.

#ifndef AUDIO_ENGINE_TASK_H
#define AUDIO_ENGINE_TASK_H

#include <stdint.h>

#include "AudioEngine.h"

namespace AudioEngineTask {

// DMA cushion 10 x 256 frames (~58 ms): sized to ride through the ~45 ms
// render stalls that flash writes cause (the render code runs from flash).
constexpr int kDmaDescNum   = 10;
constexpr int kBlockFrames  = cf_audio::kDefaultBlock;   // 256 = DMA frame count
constexpr int kTaskPriority = 5;      // above the loop task (1) and the guest task
constexpr int kTaskCore     = 1;
constexpr int kTaskStack    = 4096;   // bytes
constexpr int kQueueDepth   = 28;     // x 32-byte commands: same 896 B as before the stamps grew

// True when running (also if already running); unwinds fully on failure.
// After a timed-out stop it starts again only once the task has left.
bool start();
// True when I2S0 is free (also if already stopped). False if the render task
// did not acknowledge in time: the port, task and buffers are all kept, and a
// later stop() or start() finishes the job.
bool stop();
bool running();   // started and not mid-way through a timed-out stop

// Queue a command for the next block, stamped with the engine's current stop
// counters. False (dropped) when not running or the queue stays full.
// Stops do not go through here: use engine()->requestStop(), which is never lost.
bool send(const cf_audio::Command& c);

// The running engine, for its thread-safe parts only (sequence mailbox,
// sequenceStatus). nullptr when stopped.
cf_audio::Engine* engine();

// Bench statistics (render task writes, readers take a consistent snapshot).
struct Stats {
    uint32_t blocks;
    uint64_t cycleSum;      // render cycles (engine render + bus), all blocks
    uint32_t cycleMax;
    uint32_t sendQueueOverflows;   // IDF on_send_q_ovf: blind during flash writes
    uint32_t gapUnderruns;  // write-to-write gaps longer than cushion + one block
    uint32_t gapMaxUs;
    uint32_t clips;
    uint32_t peakVoices;
    uint32_t stackHighWater;   // bytes never used, as the render task last measured it
};
void readStats(Stats& out);
void requestStatsReset();   // applied by the render task at its next block

}  // namespace AudioEngineTask

#endif  // AUDIO_ENGINE_TASK_H
