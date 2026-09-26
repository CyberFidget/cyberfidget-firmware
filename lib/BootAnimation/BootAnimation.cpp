// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "globals.h"
#include "BootAnimation.h"
#include "images.h"
#include "HAL.h"

// The same hardware references (NeoPixels, display)
static auto& display = HAL::displayProxy();

BootAnimationApp bootAnimationApp(HAL::buttonManager());

BootAnimationApp* BootAnimationApp::instance = nullptr;

static int drawFrame = 0;
static unsigned long lastFrameTime = 0; // Tracks the last time a frame was drawn
static unsigned long sequenceStartTime = 0; // Tracks when the sequence started

// Adjustable parameters
constexpr unsigned long frameInterval = 500; // Time between frames in milliseconds
constexpr unsigned long sequenceDuration = 5000; // Total sequence duration in milliseconds

void drawBootAnimation() {
    // Check if it's time to draw the next frame
    if (millis_NOW - lastFrameTime >= frameInterval) {
        lastFrameTime = millis_NOW; // Update the last frame time
        drawFrame++; // Increase the frame number
        if (drawFrame >= boot_epd_bitmap_allArray_LEN) {
            drawFrame = 0; // Loop back to the first frame
        }

        display.clear();
        display.drawXbm(0, 0, 128, 64, boot_epd_bitmap_allArray[drawFrame]);
        display.display();
    }

    // Check if the total sequence duration has elapsed
    if (millis_NOW - sequenceStartTime >= sequenceDuration) {
        BootAnimationApp::instance->timeout(); // Exit the sequence
    }
}

BootAnimationApp::BootAnimationApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

// Set when showFirstFrame() drew the opening frame during start-up.
static bool firstFrameShown = false;

void BootAnimationApp::showFirstFrame() {
    // The frame the first update would draw, drawn as soon as the screen is
    // on; the animation carries on from it once start-up finishes.
    drawFrame = 1 % boot_epd_bitmap_allArray_LEN;
    lastFrameTime = millis();
    display.clear();
    display.drawXbm(0, 0, 128, 64, boot_epd_bitmap_allArray[drawFrame]);
    display.display();
    firstFrameShown = true;
}

void BootAnimationApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
    sequenceStartTime = millis_NOW; // Record the start time of the sequence
    // Keep the early frame's time: the next frame follows it.
    if (!firstFrameShown) lastFrameTime = millis_NOW; // Initialize the last frame time
    firstFrameShown = false;
}

void BootAnimationApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void BootAnimationApp::update() {
    drawBootAnimation();
}

void BootAnimationApp::timeout() {
    instance->end();
    MenuManager::instance().returnToMenu();
}

void BootAnimationApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[BootAnimationApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}