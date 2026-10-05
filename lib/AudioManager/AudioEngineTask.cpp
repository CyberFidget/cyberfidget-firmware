// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/AudioEngineTask.cpp - see AudioEngineTask.h.

#include "AudioEngineTask.h"

#include <Arduino.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include <atomic>
#include <new>

#include "driver/i2s_std.h"

namespace {

using cf_audio::Command;
using cf_audio::Engine;
using AudioEngineTask::kBlockFrames;
using AudioEngineTask::kDmaDescNum;

constexpr uint32_t kEngineSeed = 1;
constexpr uint32_t kStopWaitMs = 500;       // fade + cushion flush is ~75 ms
constexpr int kMaxFadeBlocks = 20;          // give up waiting for idle voices after this

// Owned by the control side between start() and stop(); the render task only
// reads the pointers.
Engine* s_engine = nullptr;
int16_t* s_buf = nullptr;          // one block, stereo (the mono mix renders into its first half)
QueueHandle_t s_queue = nullptr;
i2s_chan_handle_t s_tx = nullptr;
TaskHandle_t s_task = nullptr;
bool s_wedged = false;             // a render task that has not acknowledged its stop yet

std::atomic<bool> s_stopReq{false};
std::atomic<bool> s_exited{true};
std::atomic<bool> s_statsResetReq{false};

// Stats: written by the render task (s_qovf by the I2S ISR), read by the
// control side through readStats().
volatile uint32_t s_blocks = 0;
volatile uint64_t s_cycleSum = 0;
volatile uint32_t s_cycleMax = 0;
volatile uint32_t s_qovf = 0;
volatile uint32_t s_gapUnderruns = 0;
volatile uint32_t s_gapMaxUs = 0;
// Published by the render task about every 64 blocks from its own handle, so
// no reader ever inspects a task that may have deleted itself.
volatile uint32_t s_stackHwm = 0;
std::atomic<uint32_t> s_lastSoundMs{0};
int64_t s_lastWriteUs = 0;
const uint32_t kBlockUs = (uint32_t)(1000000ull * kBlockFrames / cf_audio::kSampleRate);
const uint32_t kCushionUs = (uint32_t)(1000000ull * kBlockFrames * kDmaDescNum / cf_audio::kSampleRate);

bool onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    s_qovf = s_qovf + 1;
    return false;
}

void resetStatsNow() {
    s_blocks = 0;
    s_cycleSum = 0;
    s_cycleMax = 0;
    s_qovf = 0;
    s_gapUnderruns = 0;
    s_gapMaxUs = 0;
    s_lastWriteUs = 0;
    if (s_engine) s_engine->resetCounters();
}

void renderTask(void*) {
    bool stopping = false;
    int fadeBlocks = 0;
    int flushLeft = -1;   // blocks of silence still to send once the voices are idle
    for (;;) {
        if (s_statsResetReq.exchange(false)) resetStatsNow();

        Command c;
        while (xQueueReceive(s_queue, &c, 0) == pdTRUE) {
            if (!stopping) s_engine->apply(c);
        }

        if (!stopping && s_stopReq.load(std::memory_order_acquire)) {
            stopping = true;
            s_engine->setBlockHook(nullptr, nullptr);
            s_engine->allOff();   // 5 ms fade, no click
        }
        if (stopping && flushLeft < 0 &&
            (s_engine->activeVoices() == 0 || ++fadeBlocks > kMaxFadeBlocks)) {
            // Fill the whole DMA cushion with the silent tail before the
            // channel is disabled, so nothing audible is cut off.
            flushLeft = kDmaDescNum + 1;
        }
        if (flushLeft == 0) break;
        if (flushLeft > 0) --flushLeft;

        const uint32_t t0 = esp_cpu_get_cycle_count();
        s_engine->render(s_buf, kBlockFrames);
        for (int i = kBlockFrames - 1; i >= 0; --i) {   // mono -> L/R, in place, back to front
            const int16_t s = s_buf[i];
            s_buf[2 * i] = s;
            s_buf[2 * i + 1] = s;
        }
        const uint32_t cycles = esp_cpu_get_cycle_count() - t0;
        s_cycleSum = s_cycleSum + cycles;
        if (cycles > s_cycleMax) s_cycleMax = cycles;
        s_blocks = s_blocks + 1;
        if ((s_blocks & 63u) == 1u) s_stackHwm = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

        size_t written = 0;
        i2s_channel_write(s_tx, s_buf, sizeof(int16_t) * 2 * kBlockFrames, &written, pdMS_TO_TICKS(100));

        // Underrun evidence that does not depend on the I2S ISR (flash writes
        // hold that off): a blocking write returns once per block, so a gap
        // longer than the cushion plus one block means the DMA ran dry.
        const int64_t nowUs = esp_timer_get_time();
        if (s_lastWriteUs) {
            const uint32_t gap = (uint32_t)(nowUs - s_lastWriteUs);
            if (gap > s_gapMaxUs) s_gapMaxUs = gap;
            if (gap > kCushionUs + kBlockUs) s_gapUnderruns = s_gapUnderruns + 1;
        }
        s_lastWriteUs = nowUs;
        if (s_engine->activeVoices() > 0 || (s_engine->sequenceStatus() & 1u)) {
            s_lastSoundMs.store((uint32_t)(nowUs / 1000) | 1u, std::memory_order_relaxed);
        }
    }
    s_exited.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void freeBuffers() {
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_engine) { s_engine->~Engine(); heap_caps_free(s_engine); s_engine = nullptr; }
    if (s_buf) { heap_caps_free(s_buf); s_buf = nullptr; }
}

// Only after the render task acknowledged its exit: release the port.
void finishStop() {
    s_task = nullptr;
    s_wedged = false;
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    freeBuffers();
}

}  // namespace

namespace AudioEngineTask {

bool start() {
    if (s_task) {
        if (!s_wedged) return true;
        // A stop that timed out: start again only once the task has left.
        if (!s_exited.load(std::memory_order_acquire)) return false;
        finishStop();
    }

    // Every buffer and the queue exist before the port is touched.
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* mem = heap_caps_malloc(sizeof(Engine), caps);
    s_buf = (int16_t*)heap_caps_malloc(sizeof(int16_t) * 2 * kBlockFrames, caps);
    s_queue = xQueueCreate(kQueueDepth, sizeof(Command));
    if (mem) s_engine = new (mem) Engine(kEngineSeed);
    if (!s_engine || !s_buf || !s_queue) {
        if (!s_engine && mem) heap_caps_free(mem);
        freeBuffers();
        Serial.println("[audio] err=engine_alloc");
        return false;
    }
    memset(s_buf, 0, sizeof(int16_t) * 2 * kBlockFrames);

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = kDmaDescNum;
    chan.dma_frame_num = kBlockFrames;
    chan.auto_clear = true;   // an underrun sends zeros, not a stale buffer
    bool enabled = false;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, nullptr);
    if (err == ESP_OK) {
        i2s_std_config_t std = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cf_audio::kSampleRate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = GPIO_NUM_26,
                .ws = GPIO_NUM_27,
                .dout = GPIO_NUM_14,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {false, false, false},
            },
        };
        err = i2s_channel_init_std_mode(s_tx, &std);
    }
    if (err == ESP_OK) {
        i2s_event_callbacks_t cbs = {};
        cbs.on_send_q_ovf = onSendQueueOverflow;
        err = i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
    }
    // No i2s_channel_preload_data: with auto_clear it panicked in the TX ISR on
    // IDF 5.1.0. auto_clear sends zeros until the first block arrives.
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
        enabled = err == ESP_OK;
    }
    if (err == ESP_OK) {
        resetStatsNow();
        s_stopReq.store(false);
        s_exited.store(false);
        if (xTaskCreatePinnedToCore(renderTask, "audio", kTaskStack, nullptr, kTaskPriority, &s_task,
                                    kTaskCore) != pdPASS) {
            s_task = nullptr;
            s_exited.store(true);
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        if (enabled) i2s_channel_disable(s_tx);
        if (s_tx) { i2s_del_channel(s_tx); s_tx = nullptr; }
        freeBuffers();
        Serial.printf("[audio] err=engine_start esp_err=0x%x\n", (unsigned)err);
        return false;
    }
    return true;
}

bool stop() {
    if (!s_task) return true;
    s_stopReq.store(true, std::memory_order_release);   // stays set until the task leaves
    const uint32_t t0 = millis();
    while (!s_exited.load(std::memory_order_acquire) && millis() - t0 < kStopWaitMs) vTaskDelay(pdMS_TO_TICKS(5));
    if (!s_exited.load(std::memory_order_acquire)) {
        // Deleting the channel or buffers under a live task would crash it:
        // keep everything, report, and let a later stop()/start() finish.
        s_wedged = true;
        Serial.println("[audio] err=render_task_stuck (I2S0 kept)");
        return false;
    }
    finishStop();
    return true;
}

bool running() { return s_task != nullptr && !s_wedged; }

uint32_t lastSoundMs() { return s_lastSoundMs.load(std::memory_order_relaxed); }

bool send(const Command& c) {
    if (!running() || !s_queue) return false;
    Command stamped = c;
    s_engine->stamp(stamped);   // so a later stop can overtake it
    return xQueueSend(s_queue, &stamped, pdMS_TO_TICKS(10)) == pdTRUE;
}

Engine* engine() { return running() ? s_engine : nullptr; }

void readStats(Stats& out) {
    uint32_t before;
    do {
        before = s_blocks;
        out.blocks = before;
        out.cycleSum = s_cycleSum;
        out.cycleMax = s_cycleMax;
    } while (before != s_blocks);
    out.sendQueueOverflows = s_qovf;
    out.gapUnderruns = s_gapUnderruns;
    out.gapMaxUs = s_gapMaxUs;
    out.clips = s_engine ? s_engine->clipCount() : 0;
    out.peakVoices = s_engine ? s_engine->peakVoices() : 0;
    out.stackHighWater = s_stackHwm;   // last value the render task published (0 before it ran)
}

void requestStatsReset() { s_statsResetReq.store(true); }

}  // namespace AudioEngineTask
