// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef RAGDOLL_FIDGIE_H
#define RAGDOLL_FIDGIE_H

#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "cf_gfx_rig.h"

class RagdollFidgie {
public:
    explicit RagdollFidgie(ButtonManager& buttonManager);

    void begin();
    void update();
    void end();

    static RagdollFidgie* instance;

private:
    static void onBack(const ButtonEvent& event);
    static void onFace(const ButtonEvent& event);

    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
    void triggerRagdoll();
    bool shakeTriggered(unsigned long now);
    void updateSliderPose();

    ButtonManager& buttonManager;
    DisplayProxy& display;
    cf::gfx::RigActor actor;
    unsigned long lastFrameMs;
    unsigned long lastShakeMs;
    float accelBaseX;
    float accelBaseY;
    float accelBaseZ;
    bool sliderActive;
};

extern RagdollFidgie ragdollFidgieApp;

#endif  // RAGDOLL_FIDGIE_H
