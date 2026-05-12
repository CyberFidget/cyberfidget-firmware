// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "BatteryManager.h"
#include "globals.h"
#include "HAL.h"

BatteryManager::BatteryManager() : lipo(MAX1704X_MAX17048) {}

BatteryManager batteryManager;

void BatteryManager::init() {
    lipo.enableDebugging();
    if (!lipo.begin()) {
        ESP_LOGI(TAG_MAIN, "MAX17048 not detected. Please check wiring. Freezing.");
        while(1);
    }

    uint8_t id = lipo.getID();
    uint8_t ver = lipo.getVersion();
    ESP_LOGI(TAG_MAIN, "MAX17048 Device ID: 0x%02X  Version: 0x%02X", id, ver);

    bool RI = lipo.isReset(true);
    ESP_LOGD(TAG_MAIN, "Reset Indicator was: %d", RI);
    if (RI) {
        RI = lipo.isReset();
        ESP_LOGD(TAG_MAIN, "Reset Indicator is now: %d", RI);
    }

    //lipo.quickStart();

    lipo.setThreshold(20);
    ESP_LOGD(TAG_MAIN, "Battery empty threshold is now: %d%%", lipo.getThreshold());

    float highVoltage = ((float)lipo.getVALRTMax()) * 0.02;
    ESP_LOGD(TAG_MAIN, "High voltage threshold is currently: %.2fV", highVoltage);

    lipo.setVALRTMax((float)4.1);
    highVoltage = ((float)lipo.getVALRTMax()) * 0.02;
    ESP_LOGD(TAG_MAIN, "High voltage threshold is now: %.2fV", highVoltage);

    float lowVoltage = ((float)lipo.getVALRTMin()) * 0.02;
    ESP_LOGD(TAG_MAIN, "Low voltage threshold is currently: %.2fV", lowVoltage);

    lipo.setVALRTMin((float)3.9);
    lowVoltage = ((float)lipo.getVALRTMin()) * 0.02;
    ESP_LOGD(TAG_MAIN, "Low voltage threshold is now: %.2fV", lowVoltage);

    if(lipo.enableSOCAlert()) {
        ESP_LOGI(TAG_MAIN, "Enabling the 1%% State Of Change alert: success.");
    } else {
        ESP_LOGI(TAG_MAIN, "Enabling the 1%% State Of Change alert: FAILED!");
    }

    float actThr = ((float)lipo.getHIBRTActThr()) * 0.00125;
    ESP_LOGD(TAG_MAIN, "Hibernate active threshold is: %.5fV", actThr);

    float hibThr = ((float)lipo.getHIBRTHibThr()) * 0.208;
    ESP_LOGD(TAG_MAIN, "Hibernate hibernate threshold is: %.3f%%/h", hibThr);
}

void BatteryManager::update() {
    // Update global battery status variables
    batteryVoltagePercentage = lipo.getSOC();
    batteryVoltage = lipo.getVoltage();
    batteryChangeRate = lipo.getChangeRate();

    // Manage LiPo charger based on SOC and change rate
    // Requires Jumper on R64 to be soldered
    // RED LED on front stays on when charging is blocked
    if(enableBatterySOCCutoff) {
      if ((batteryVoltagePercentage > batterySOCCutoff) && (lipo.getChangeRate() > sleepChargingChangeThreshold)) {
        HAL::chargingDisable();
      } else {
        HAL::chargingEnable();
      }
    }
}

void BatteryManager::prepareForDeepSleep() {
    // Force the MAX17048 into hibernate (writes 0xFFFF to HIBRT).
    // Drops gauge supply current from ~23 µA to ~4 µA across sleep.
    // Hibernate clears automatically on the next sufficiently-large
    // VCELL change, so no explicit disableHibernate() is needed at wake.
    lipo.enableHibernate();
}

void BatteryManager::debug() {
    ESP_LOGD(TAG_MAIN, "Battery: %.2fV  SOC: %.2f%%  Rate: %.2f%%/hr  Alert: %d  VHigh: %d  VLow: %d  Empty: %d  SOC1%%: %d  Hibernating: %d",
        batteryVoltage,
        batteryVoltagePercentage,
        batteryChangeRate,
        lipo.getAlert(),
        lipo.isVoltageHigh(),
        lipo.isVoltageLow(),
        lipo.isLow(),
        lipo.isChange(),
        lipo.isHibernating());
}
