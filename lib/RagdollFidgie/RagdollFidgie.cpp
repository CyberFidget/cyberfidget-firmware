// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "RagdollFidgie.h"

#include <Arduino.h>
#include <math.h>

#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "RagdollFidgieRig.h"

using namespace cf::gfx;

RagdollFidgie ragdollFidgieApp(HAL::buttonManager());
RagdollFidgie* RagdollFidgie::instance = nullptr;

RagdollFidgie::RagdollFidgie(ButtonManager& manager)
    : buttonManager(manager),
      display(HAL::displayProxy()),
      lastFrameMs(0),
      lastShakeMs(0),
      accelBaseX(0.0f),
      accelBaseY(0.0f),
      accelBaseZ(0.0f),
      sliderActive(false) {
    instance = this;
}

void RagdollFidgie::begin() {
    registerButtonCallbacks();
    setColorsOff();
    actor.setRig(&ragdoll_fidgie::rig);
    actor.setPos(64, 44);
    actor.update(millis());
    actor.play("idle");
    lastFrameMs = 0;
    lastShakeMs = millis() - 500;
    accelBaseX = accelX;
    accelBaseY = accelY;
    accelBaseZ = accelZ;
    sliderActive = false;
}

void RagdollFidgie::end() {
    unregisterButtonCallbacks();
    setColorsOff();
}

void RagdollFidgie::update() {
    const unsigned long now = millis();
    if (lastFrameMs != 0 && now - lastFrameMs < 20) return;
    lastFrameMs = now;

    if (!actor.isRagdoll()) updateSliderPose();
    const bool shook = shakeTriggered(now);
    if (!actor.isRagdoll() && shook) triggerRagdoll();

    actor.update(now);
    display.clear();
    actor.draw(display);
    display.display();
}

void RagdollFidgie::registerButtonCallbacks() {
    buttonManager.registerCallback(button_BottomLeftIndex, onBack);
    buttonManager.registerCallback(button_LeftIndex, onFace);
    buttonManager.registerCallback(button_RightIndex, onFace);
    buttonManager.registerCallback(button_UpIndex, onFace);
    buttonManager.registerCallback(button_DownIndex, onFace);
    buttonManager.registerCallback(button_BottomRightIndex, onFace);
}

void RagdollFidgie::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
}

void RagdollFidgie::onBack(const ButtonEvent& event) {
    if (instance == nullptr || event.eventType != ButtonEvent_Released) return;
    instance->end();
    MenuManager::instance().returnToMenu();
}

void RagdollFidgie::onFace(const ButtonEvent& event) {
    if (instance != nullptr && event.eventType == ButtonEvent_Pressed) {
        instance->triggerRagdoll();
    }
}

void RagdollFidgie::triggerRagdoll() {
    if (actor.isRagdoll()) return;
    // No synthetic velocity is added: RigActor carries the frame-over-frame
    // joint delta into verlet at 0.9, preserving the current kinematic motion.
    actor.ragdoll();
}

bool RagdollFidgie::shakeTriggered(unsigned long now) {
    accelBaseX += (accelX - accelBaseX) * 0.02f;
    accelBaseY += (accelY - accelBaseY) * 0.02f;
    accelBaseZ += (accelZ - accelBaseZ) * 0.02f;
    const float dx = accelX - accelBaseX;
    const float dy = accelY - accelBaseY;
    const float dz = accelZ - accelBaseZ;
    const float magnitude = sqrtf(dx * dx + dy * dy + dz * dz);
    if (magnitude <= 600.0f || now - lastShakeMs < 500) return false;
    lastShakeMs = now;
    return true;
}

void RagdollFidgie::updateSliderPose() {
    float slider = sliderPosition_Percentage_Filtered;
    if (slider < 0.0f) slider = 0.0f;
    if (slider > 100.0f) slider = 100.0f;

    // The centered control leaves the idle animation running. Moving away
    // switches to the prototype's two-segment crouch/rest/stretch blend.
    const bool shouldBlend = slider < 49.0f || slider > 51.0f;
    if (!shouldBlend) {
        if (sliderActive) actor.play("idle");
        sliderActive = false;
        return;
    }
    sliderActive = true;
    if (slider < 50.0f) {
        const uint8_t amount =
            static_cast<uint8_t>((slider * 255.0f / 50.0f) + 0.5f);
        actor.blend(&ragdoll_fidgie::poses[ragdoll_fidgie::POSE_CROUCH],
                    &ragdoll_fidgie::poses[ragdoll_fidgie::POSE_REST],
                    amount);
    } else {
        const uint8_t amount = static_cast<uint8_t>(
            (((slider - 50.0f) * 255.0f / 50.0f) + 0.5f));
        actor.blend(&ragdoll_fidgie::poses[ragdoll_fidgie::POSE_REST],
                    &ragdoll_fidgie::poses[ragdoll_fidgie::POSE_STRETCH],
                    amount);
    }
}
