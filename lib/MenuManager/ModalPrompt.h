// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MODAL_PROMPT_H
#define MODAL_PROMPT_H

#include <stdint.h>
#include <string>
#include <vector>

#include "ButtonManager.h"
#include "ModalPromptModel.h"
#include "ScrollLabel.h"

/**
 * @brief Full-screen prompt: a title plus one or more options, answered
 * with Up / Down / Enter.
 *
 * While open it takes over the display and the six buttons: it saves the
 * current button callbacks, installs its own, and hands the originals back
 * when it closes. AppManager draws it in place of the active app's frame,
 * so the app underneath is paused, not ended.
 *
 * It only reports which option was chosen (zero-based) or kNoChoice when
 * the optional timeout expires; what a choice means, and anything it
 * stores, belongs to the caller. Left, Right and Back do nothing, so a
 * prompt without a timeout waits for an answer.
 *
 * Layout (128x64): inverted title bar, then up to kVisibleRows option rows
 * with the focused row filled; a scrollbar appears on the right when the
 * options do not all fit. The focused row marquees when too long.
 */
class ModalPrompt {
public:
    typedef void (*DoneCallback)(int result);

    static constexpr int kNoChoice    = ModalPromptModel::kNoChoice;
    static constexpr int kVisibleRows = 4;

    static ModalPrompt &instance();

    /**
     * @brief Open the prompt. Strings are copied.
     * @param timeoutMs  0 = no timeout (the default). Otherwise the prompt
     *                   closes with kNoChoice after this long without Up/Down.
     * @param done       called once after the prompt closes and the previous
     *                   button callbacks are back; may be nullptr.
     * @return false if a prompt is already open or optionCount < 1.
     */
    bool open(const char *title,
              const char *const *options,
              int optionCount,
              DoneCallback done,
              uint32_t timeoutMs = 0);

    bool isOpen() const { return model.isOpen(); }

    /** Timeout check + one frame. Called by AppManager while open. */
    void update();

    /** Read-only access for tests / diagnostics. */
    const ModalPromptModel &state() const { return model; }

private:
    ModalPrompt() {}

    void draw();
    void finish();

    static void onUp(const ButtonEvent &event);
    static void onDown(const ButtonEvent &event);
    static void onEnter(const ButtonEvent &event);

    ModalPromptModel         model;
    ScrollLabel              titleLabel;
    ScrollLabel              focusLabel;
    int                      labelFor = -1;   // option the marquee belongs to
    std::string              title;
    std::vector<std::string> options;
    DoneCallback             done = nullptr;

    static constexpr int kButtons = 6;
    ButtonCallback savedCallbacks[kButtons] = {};
};

#endif
