// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// PowerManager.cpp
#include "PowerManager.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"

PowerManager powermanager(HAL::buttonManager());

// Initialize the static instance reference
PowerManager* PowerManager::instance = nullptr;

PowerManager::PowerManager(ButtonManager& btnMgr)
    : display(HAL::displayProxy()),
    buttonManager(btnMgr),
    lastTapTime(0)
{
    // Set the static instance to this object
    instance = this;
}

void PowerManager::begin() {
    registerShutdownCallback();
}

void PowerManager::update() {
    drawShutdownScreen();
}

void PowerManager::end() {
    unregisterShutdownCallback();
}

void PowerManager::registerShutdownCallback() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
    buttonManager.registerCallback(button_BottomRightIndex, onButtonPressCallback);
}

void PowerManager::unregisterShutdownCallback() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
}

void PowerManager::drawShutdownScreen() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 10, "Shutdown");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 30, "Double-tap Bottom Right");
    display.drawString(64, 40, "to power off");
    display.display();
}

void PowerManager::onButtonPressCallback(const ButtonEvent &event) {
    if (instance) {
        if (event.eventType == ButtonEvent_Pressed) {
            unsigned long currentTime = millis();
            if (currentTime - instance->lastTapTime <= DOUBLE_TAP_THRESHOLD_MS) {
                // Detected a double-tap
                instance->display.clear();
                instance->display.setTextAlignment(TEXT_ALIGN_CENTER);
                instance->display.setFont(ArialMT_Plain_10);
                instance->display.drawString(64, 30, "Powering off...");
                instance->display.display();

                instance->buttonManager.saveButtonCounters();

                delay(500);
                ESP_LOGV(TAG_MAIN, "Entering deep sleep...");

                HAL::enterDeepSleep();
            } else {
                // Update last tap time
                instance->lastTapTime = currentTime;
            }
        }
    }
}

void PowerManager::onButtonBackPressed(const ButtonEvent& event)
{    // Press
    if (event.eventType == ButtonEvent_Released){
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

void PowerManager::deepSleep() {
    // Go to deep sleep
    if(preventSleepWhileCharging){
        if(batteryChangeRate < sleepChargingChangeThreshold){ // If discharging greater than 10% per hour, shut down
            instance->buttonManager.saveButtonCounters();

            ESP_LOGI(TAG_MAIN, "Going to sleep now...");

            display.clear();
            display.setFont(ArialMT_Plain_10);
            display.drawString(64, 20, "Going to sleep now...");
            display.display();

            delay(3000);
            HAL::enterDeepSleep();
        }
        } else {
            ESP_LOGI(TAG_MAIN, "Going to sleep now...");

            display.clear();
            display.setFont(ArialMT_Plain_10);
            display.drawString(64, 20, "Going to sleep now...");
            display.display();

            delay(3000);
            HAL::enterDeepSleep();
        }
}

// Global functions for the app system
void powerManagerBegin() {
    powermanager.begin();
}

void powerManagerEnd() {
    powermanager.end();
}

void powerManagerUpdate() {
    powermanager.update();
}
