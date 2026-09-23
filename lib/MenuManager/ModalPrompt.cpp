// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "ModalPrompt.h"

#include <Arduino.h>

#include "HAL.h"
#include "DisplayProxy.h"

// Layout on the 128x64 screen; matches the Music Player's list screens.
static const int MP_SCREEN_W       = 128;
static const int MP_TITLE_BAR_H    = 14;
static const int MP_LIST_Y         = 16;
static const int MP_ROW_H          = 12;
static const int MP_TEXT_X         = 4;
static const int MP_SCROLLBAR_X    = 126;
static const int MP_SCROLLBAR_W    = 2;
static const int MP_SCROLL_GUTTER  = 4;   // columns kept clear for the scrollbar
static const int MP_LIST_H         = ModalPrompt::kVisibleRows * MP_ROW_H;

ModalPrompt &ModalPrompt::instance()
{
    static ModalPrompt single;
    return single;
}

bool ModalPrompt::open(const char *titleText,
                       const char *const *optionTexts,
                       int optionCount,
                       DoneCallback onDone,
                       uint32_t timeoutMs)
{
    if (!hostAllows) return false;
    if (model.isOpen() || optionCount < 1 || !optionTexts) return false;
    const uint32_t now = (uint32_t)millis();
    if (!model.open(optionCount, kVisibleRows, now, timeoutMs)) return false;

    title = titleText ? titleText : "";
    options.clear();
    for (int i = 0; i < optionCount; i++) {
        options.push_back(optionTexts[i] ? optionTexts[i] : "");
    }
    done = onDone;
    titleLabel.restart(now);
    focusLabel.restart(now);
    labelFor = model.selected();
    guard.beginOpen();
    openGeneration = hostGeneration;
    // A prompt appearing counts as activity once, so the idle-sleep clock
    // starts from now; it does not hold the device awake after that.
    millis_APP_LASTINTERACTION = millis_NOW;

    // Take the buttons: remember whatever the app underneath had so it
    // gets exactly those back when the prompt closes.
    auto &buttons = HAL::buttonManager();
    for (int i = 0; i < kButtons; i++) {
        savedCallbacks[i] = buttons.getCallback(i);
        buttons.unregisterCallback(i);
    }
    buttons.registerCallback(button_UpIndex, onUp);
    buttons.registerCallback(button_DownIndex, onDown);
    buttons.registerCallback(button_EnterIndex, onEnter);
    // The rest only feed the guard, so their presses cannot leak either.
    buttons.registerCallback(button_LeftIndex, onOther);
    buttons.registerCallback(button_RightIndex, onOther);
    buttons.registerCallback(button_SelectIndex, onOther);
    return true;
}

void ModalPrompt::update()
{
    if (!model.isOpen()) return;
    if (model.tick((uint32_t)millis())) {
        finish();
        return;
    }
    draw();
}

void ModalPrompt::finish()
{
    auto &buttons = HAL::buttonManager();
    // Buttons still down (or pressed inside the prompt and not yet released)
    // must not finish their press in the app underneath.
    uint32_t stillDown = 0;
    for (int i = 0; i < kButtons; i++) {
        if (buttons.isPressed(i)) stillDown |= (uint32_t)1u << i;
    }
    guard.armOnClose(stillDown);

    // Hand the callbacks back only to the app they came from. The host
    // closes the prompt before any app switch, so a changed generation
    // means that was skipped; the new app's own callbacks then stay.
    const bool sameApp = (openGeneration == hostGeneration);
    for (int i = 0; i < kButtons; i++) {
        if (sameApp) {
            if (savedCallbacks[i]) buttons.registerCallback(i, savedCallbacks[i]);
            else                   buttons.unregisterCallback(i);
        }
        savedCallbacks[i] = nullptr;
    }

    // Clear before calling out so the callback may open another prompt.
    DoneCallback cb = done;
    done = nullptr;
    if (cb) cb(model.result());
}

void ModalPrompt::setHostAllows(bool allows)
{
    hostGeneration++;
    hostAllows = allows;
}

void ModalPrompt::closeForTeardown()
{
    if (!model.isOpen()) return;
    model.dismiss();
    // No new prompt may open from the done callback mid-teardown.
    const bool allowed = hostAllows;
    hostAllows = false;
    finish();
    hostAllows = allowed;
}

bool ModalPrompt::swallowEvent(const ButtonEvent &event)
{
    return guard.consume(event.buttonIndex, kindOf(event));
}

PromptButtonGuard::Kind ModalPrompt::kindOf(const ButtonEvent &event)
{
    if (event.eventType == ButtonEvent_Pressed)  return PromptButtonGuard::Press;
    if (event.eventType == ButtonEvent_Released) return PromptButtonGuard::Release;
    return PromptButtonGuard::Held;
}

void ModalPrompt::draw()
{
    auto &display = HAL::displayProxy();
    display.clear();
    display.setFont(ArialMT_Plain_10);

    // Title bar (inverted)
    display.setColor(WHITE);
    display.fillRect(0, 0, MP_SCREEN_W, MP_TITLE_BAR_H);
    display.setColor(BLACK);
    titleLabel.draw(MP_TEXT_X, 1, MP_SCREEN_W - 2 * MP_TEXT_X, title.c_str(), true);
    display.setColor(WHITE);

    const bool scrollbar = model.optionCount() > model.windowCount();
    const int rowW  = scrollbar ? MP_SCREEN_W - MP_SCROLL_GUTTER : MP_SCREEN_W;
    const int textW = rowW - 2 * MP_TEXT_X;

    if (model.selected() != labelFor) {
        // A newly focused long row holds still for one step before scrolling.
        focusLabel.restart((uint32_t)millis());
        labelFor = model.selected();
    }

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    for (int i = 0; i < model.windowCount(); i++) {
        const int idx = model.windowStart() + i;
        const int y   = MP_LIST_Y + i * MP_ROW_H;
        if (idx == model.selected()) {
            display.setColor(WHITE);
            display.fillRect(0, y, rowW, MP_ROW_H);
            display.setColor(BLACK);
            focusLabel.draw(MP_TEXT_X, y - 1, textW, options[idx].c_str(), false);
            display.setColor(WHITE);
            display.setTextAlignment(TEXT_ALIGN_LEFT);
        } else {
            display.drawString(MP_TEXT_X, y - 1, options[idx].c_str());
        }
    }

    if (scrollbar) {
        // Unfocused long rows run under the gutter; clear it first.
        display.setColor(BLACK);
        display.fillRect(MP_SCREEN_W - MP_SCROLL_GUTTER, MP_LIST_Y, MP_SCROLL_GUTTER, MP_LIST_H);
        display.setColor(WHITE);
        const int total  = model.optionCount();
        int thumbH = (MP_LIST_H * model.windowCount()) / total;
        if (thumbH < 4) thumbH = 4;
        const int thumbY = MP_LIST_Y + (MP_LIST_H * model.windowStart()) / total;
        display.drawRect(MP_SCROLLBAR_X, MP_LIST_Y, MP_SCROLLBAR_W, MP_LIST_H);
        display.fillRect(MP_SCROLLBAR_X, thumbY, MP_SCROLLBAR_W, thumbH);
    }

    display.display();
}

// Up/Down act on the press edge, like the main menu.
void ModalPrompt::onUp(const ButtonEvent &event)
{
    instance().guard.noteWhileOpen(event.buttonIndex, kindOf(event));
    if (event.eventType == ButtonEvent_Pressed) {
        instance().model.moveUp((uint32_t)millis());
    }
}

void ModalPrompt::onDown(const ButtonEvent &event)
{
    instance().guard.noteWhileOpen(event.buttonIndex, kindOf(event));
    if (event.eventType == ButtonEvent_Pressed) {
        instance().model.moveDown((uint32_t)millis());
    }
}

// Enter chooses on release, like the main menu, and only when the press
// also happened while the prompt was open.
void ModalPrompt::onEnter(const ButtonEvent &event)
{
    ModalPrompt &self = instance();
    self.guard.noteWhileOpen(event.buttonIndex, kindOf(event));
    if (event.eventType == ButtonEvent_Pressed) {
        self.model.selectPressed((uint32_t)millis());
    } else if (event.eventType == ButtonEvent_Released) {
        if (self.model.selectReleased()) self.finish();
    }
}

// Left, Right and Back do nothing in a prompt; they are tracked only so a
// press started here is not finished in the app after the prompt closes.
void ModalPrompt::onOther(const ButtonEvent &event)
{
    instance().guard.noteWhileOpen(event.buttonIndex, kindOf(event));
}
