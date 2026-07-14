#include "StopwatchDemo.h"
// cf_custom_app_device.h is force-included by the build (-include), so
// HAL/DisplayProxy/ButtonManager/globals are all in scope even though this
// file only #includes its own header - the generated-app convention.

StopwatchDemo stopwatchDemo;
static StopwatchDemo* self = nullptr;

static void btnTrampoline(const ButtonEvent& e) { if (self) self->onButton(e); }

void StopwatchDemo::begin() {
    self = this;
    startMs = 0; accumMs = 0; running = false;
    HAL::buttonManager().registerCallback(button_BottomRightIndex, &btnTrampoline); // Enter: start/stop
    HAL::buttonManager().registerCallback(button_BottomLeftIndex,  &btnTrampoline); // Back: reset
}

void StopwatchDemo::onButton(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (e.buttonIndex == button_BottomRightIndex) {
        if (running) { accumMs += millis() - startMs; running = false; }
        else { startMs = millis(); running = true; }
    } else if (e.buttonIndex == button_BottomLeftIndex) {
        accumMs = 0; running = false;
    }
}

void StopwatchDemo::update() {
    unsigned long shown = accumMs + (running ? (millis() - startMs) : 0);
    unsigned long secs = shown / 1000, tenths = (shown % 1000) / 100;
    auto& d = HAL::displayProxy();
    d.clear();
    d.setFont(ArialMT_Plain_10);
    d.drawString(2, 2, "STOPWATCH");
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu.%lu s", secs, tenths);
    d.setFont(ArialMT_Plain_24);
    d.drawString(18, 24, buf);
    d.setFont(ArialMT_Plain_10);
    d.drawString(2, 54, running ? "Enter=stop  Back=reset" : "Enter=start Back=reset");
    d.display();
}

void StopwatchDemo::end() {
    HAL::buttonManager().unregisterCallback(button_BottomRightIndex);
    HAL::buttonManager().unregisterCallback(button_BottomLeftIndex);
}
