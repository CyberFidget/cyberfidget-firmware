// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <Arduino.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include "UvloLogic.h"

class BatteryManager {
public:
    BatteryManager();
    void init();
    void update();
    void debug();
    void prepareForDeepSleep();
    bool consumeRuntimeShutdownRequest();
    bool gaugeHibernateForce();
    bool gaugeHibernateAuto();
    bool gaugeIsHibernating();
    bool gaugeSetAlertMin(float volts);
    float gaugeGetAlertMin();
private:
    SFE_MAX1704X lipo;
    UvloLogic::RuntimeDebounce runtimeGuard;
    volatile bool runtimeShutdownRequested = false;
};

extern BatteryManager batteryManager;

#endif // BATTERY_MANAGER_H
