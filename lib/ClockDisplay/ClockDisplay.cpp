// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "ClockDisplay.h"

#include "HAL.h"     // For DisplayProxy
#include "MenuManager.h"  // For returning back to menu
#include "globals.h"

#include <WiFi.h>   // We use WiFi.status() to see if we're connected
#include <sys/time.h>

// Define a minimum valid epoch (here we assume any time after Sept 2020 is valid)
#define MIN_VALID_EPOCH 1600000000UL

ClockDisplay clockDisplay;

ClockDisplay* ClockDisplay::instance = nullptr;

// ---------------------------------------------------------------------
// Constructor: Initialize member variables.
// ---------------------------------------------------------------------
ClockDisplay::ClockDisplay()
    : m_display(HAL::displayProxy()),
      buttonManager(HAL::buttonManager()),
      m_accurateTime(false),
      m_initialized(false),
      m_currentTime(0),
      m_lastUpdateMillis(0),
      m_internalBaseTime(0),
      m_internalBaseMillis(0)
{
        instance = this;
}

// ---------------------------------------------------------------------
// begin()
// Called once when the clock app is launched.
// It loads saved time from Preferences and initiates an NTP sync if WiFi is connected.
// ---------------------------------------------------------------------
void ClockDisplay::begin() {

    ESP_LOGI(TAG_MAIN, "begin() => registering app callbacks...");
    registerButtonCallbacks();

    // If already initialized, simply return.
    if (m_initialized) {
        return;
    }

    // Open Preferences under the namespace "clock"
    m_preferences.begin("clock", false);
    
    // Load saved time data (if available) to serve as our internal time base.
    m_internalBaseTime = m_preferences.getULong("lastTime", 0);
    m_internalBaseMillis = m_preferences.getULong("lastMillis", millis());
    
    // Check if the system time is already valid (i.e. has been synced via NTP)
    if (isSystemTimeValid()) {
        m_currentTime = time(nullptr);
        m_accurateTime = true;
    } else {
        // Otherwise, use the saved (or default) internal time.
        m_currentTime = m_internalBaseTime;
        m_accurateTime = false;
    }
    
    // Record the current millis for timing our updates.
    m_lastUpdateMillis = millis();
    
    // If WiFi is connected, initiate NTP synchronization.
    if (WiFi.status() == WL_CONNECTED) {
        // Configure time with timezone and NTP servers.
        // (This call is non-blocking; the system will update time when NTP packets arrive.)
        configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    }
    
    // Draw an initial screen.
    m_display.clear();
    displayTime();
    
    m_preferences.end();

    m_initialized = true;  // Mark as initialized.
}

// ---------------------------------------------------------------------
// update()
// Called repeatedly from loop() to update the clock display.
// This function is non-blocking and updates the display roughly every second.
// ---------------------------------------------------------------------
void ClockDisplay::update() {
    unsigned long nowMillis = millis();
    
    // Only update once per m_updateInterval (1 second)
    if (nowMillis - m_lastUpdateMillis >= m_updateInterval) {
        m_lastUpdateMillis = nowMillis;
        
        // Check if system time has become valid (i.e. an NTP sync occurred)
        if (!m_accurateTime && isSystemTimeValid()) {
            m_currentTime = time(nullptr);
            m_accurateTime = true;
            // Update our internal base values so our fallback (if needed later) is current.
            m_internalBaseTime = m_currentTime;
            m_internalBaseMillis = nowMillis;
        }
        else if (!m_accurateTime) {
            // In low-accuracy mode, we compute the current time from our saved base and elapsed millis.
            unsigned long elapsed = nowMillis - m_internalBaseMillis;
            m_currentTime = m_internalBaseTime + elapsed / 1000;
        }
        else {
            // If we have accurate time, simply query the system.
            m_currentTime = time(nullptr);
        }
        
        // Refresh the display with the new time.
        displayTime();
    }
    
    // If we are still in low-accuracy mode but WiFi is now connected, try to sync.
    if (!m_accurateTime && WiFi.status() == WL_CONNECTED) {
        attemptTimeSync();
    }
}

// ---------------------------------------------------------------------
// attemptTimeSync()
// Checks if the system time is now valid, and if so, updates our variables.
// ---------------------------------------------------------------------
void ClockDisplay::attemptTimeSync() {
    if (isSystemTimeValid()) {
        m_currentTime = time(nullptr);
        m_accurateTime = true;
        // Reset our internal base time to the new accurate time.
        m_internalBaseTime = m_currentTime;
        m_internalBaseMillis = millis();
    }
}

// ---------------------------------------------------------------------
// displayTime()
// Formats the current time and draws it on the display.
// If time is not accurate, a small “Low Accuracy” notice is drawn underneath.
// ---------------------------------------------------------------------
void ClockDisplay::displayTime() {
    // Format the time as HH:MM:SS
    char buffer[10];  // Enough for "HH:MM:SS" + null terminator
    struct tm timeInfo;
    localtime_r(&m_currentTime, &timeInfo);
    strftime(buffer, sizeof(buffer), "%I:%M:%S", &timeInfo);
    
    // Clear the display before drawing
    m_display.clear();
    
    m_display.setFont(ArialMT_Plain_24);
    m_display.setTextAlignment(TEXT_ALIGN_CENTER);
    // Draw the time string (positioned roughly centered)
    m_display.drawString(64, 22, String(buffer));
    
    // Determine the time source message
    String sourceMessage;
    if (m_accurateTime) {
        // If we have an accurate time (system time updated via NTP)
        if (WiFi.status() == WL_CONNECTED) {
            sourceMessage = "Src: Internet (NTP)";
        } else {
            sourceMessage = "Src: Cached NTP";

        }
    } else {
        sourceMessage = "Src: Internal (Low Accuracy)";
    }
    
    // Draw the time source message in a smaller font below the main time.
    m_display.setFont(ArialMT_Plain_10);
    m_display.drawString(64, 45, sourceMessage);
    
    // Finally, push the buffer to the display.
    m_display.display();
}


// ---------------------------------------------------------------------
// isSystemTimeValid()
// Returns true if the system time is greater than MIN_VALID_EPOCH,
// which indicates that an NTP sync has likely occurred.
// ---------------------------------------------------------------------
bool ClockDisplay::isSystemTimeValid() {
    time_t now = time(nullptr);
    return (now > MIN_VALID_EPOCH);
}

// ---------------------------------------------------------------------
// saveTime()
// Saves the current time and current millis() into Preferences.
// This allows the clock to recover its “base time” after a deep sleep cycle.
// ---------------------------------------------------------------------
void ClockDisplay::saveTime() {
    m_preferences.begin("clock", false);
    m_preferences.putULong("lastTime", m_currentTime);
    m_preferences.putULong("lastMillis", millis());
    m_preferences.end();
    ESP_LOGD(TAG_MAIN, "ClockDisplay: Time saved to Preferences.");
}

// ---------------------------------------------------------------------
// reset()
// Reset internal initialization flag (or ignore subsequent calls to begin()).
// ---------------------------------------------------------------------
void ClockDisplay::reset() {
    // This method can be called when switching away from the clock app.
    m_initialized = false;
}


/* AppDef and ButtonManager Integration */


// ---------------------------------------------------------------------
// onButtonBackPressed()
// Exit the clock app and return to the main menu.
// ---------------------------------------------------------------------
void ClockDisplay::onButtonBackPressed(const ButtonEvent& event)
{    // Press
    if (event.eventType == ButtonEvent_Released){
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    } 
}

// ---------------------------------------------------------------------
// end()
// Internally and externally callable function to unregister callbacks and cleanup.
// ---------------------------------------------------------------------
void ClockDisplay::end() {
    ESP_LOGI(TAG_MAIN, "end() => unregistering app callbacks...");
    unregisterButtonCallbacks();
}

void ClockDisplay::registerButtonCallbacks() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ClockDisplay::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}