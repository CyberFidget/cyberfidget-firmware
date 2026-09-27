// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "BTScanner.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_log.h>

static const char* TAG_BT = "BTScanner";

BTScanner* BTScanner::_instance = nullptr;

bool BTScanner::begin() {
    _instance = this;
    _resultCount = 0;
    _scanning = false;

    // If bluedroid is already running (e.g. A2DPStream owns the BT stack),
    // just register our GAP callback and scan — don't touch the stack.
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGI(TAG_BT, "BT stack already running, reusing");
        esp_bt_gap_register_callback(gapCallback);
        _btInitialized = true;
        _ownsStack = false;
        return true;
    }

    // First time — full BT init. btStart() also forces esp32-hal-bt.o to
    // link, providing the strong btInUse()→true that prevents initArduino()
    // from releasing BT controller memory at startup.
    if (!btStarted()) {
        if (!btStart()) {
            ESP_LOGI(TAG_BT, "btStart failed");
            return false;
        }
    }

    ESP_LOGI(TAG_BT, "BT controller enabled OK");

    esp_err_t ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGI(TAG_BT, "bluedroid init failed: 0x%x", ret);
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGI(TAG_BT, "bluedroid enable failed: 0x%x", ret);
        esp_bluedroid_deinit();
        return false;
    }

    esp_bt_gap_register_callback(gapCallback);
    _btInitialized = true;
    _ownsStack = true;
    return true;
}

bool BTScanner::startScan(int durationSec) {
    if (!_btInitialized) return false;

    portENTER_CRITICAL(&_mutex);
    _resultCount = 0;
    portEXIT_CRITICAL(&_mutex);

    _scanning = true;

    // GAP discovery duration in 1.28s units
    int units = (durationSec * 100) / 128;
    if (units < 1) units = 1;
    if (units > 0x30) units = 0x30;

    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0);
    if (err != ESP_OK) {
        ESP_LOGI(TAG_BT, "start_discovery failed: %d", err);
        _scanning = false;
        return false;
    }
    return true;
}

void BTScanner::stopScan() {
    esp_bt_gap_cancel_discovery();
    _scanning = false;
}

void BTScanner::end() {
    if (!_btInitialized) return;

    if (_scanning) {
        stopScan();
    }

    if (_ownsStack) {
        // We initialized the BT stack — tear it down
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        // Leave BT controller ENABLED (can't deinit after BLE memory release)
    }
    // If !_ownsStack, A2DPStream owns the stack — don't touch it

    _btInitialized = false;
    _ownsStack = false;
    _scanning = false;
    _resultCount = 0;
}

bool BTScanner::isScanning() const {
    return _scanning;
}

int BTScanner::getResultCount() const {
    portENTER_CRITICAL(&_mutex);
    int count = _resultCount;
    portEXIT_CRITICAL(&_mutex);
    return count;
}

BTScanResult BTScanner::getResult(int index) {
    BTScanResult copy;
    portENTER_CRITICAL(&_mutex);
    if (index < _resultCount) {
        copy = _results[index];
    } else {
        memset(&copy, 0, sizeof(copy));
    }
    portEXIT_CRITICAL(&_mutex);
    return copy;
}

bool BTScanner::addResult(const char* name, esp_bd_addr_t addr, int rssi) {
    portENTER_CRITICAL(&_mutex);

    // Check for duplicate by address
    for (int i = 0; i < _resultCount; i++) {
        if (memcmp(_results[i].address, addr, ESP_BD_ADDR_LEN) == 0) {
            // Update name if we got a better one
            if (name[0] && !_results[i].name[0]) {
                strncpy(_results[i].name, name, sizeof(_results[i].name) - 1);
            }
            _results[i].rssi = rssi;
            portEXIT_CRITICAL(&_mutex);
            return false;
        }
    }

    if (_resultCount >= MAX_RESULTS) {
        portEXIT_CRITICAL(&_mutex);
        return false;
    }

    int idx = _resultCount;
    strncpy(_results[idx].name, name, sizeof(_results[idx].name) - 1);
    _results[idx].name[sizeof(_results[idx].name) - 1] = '\0';
    memcpy(_results[idx].address, addr, ESP_BD_ADDR_LEN);
    _results[idx].rssi = rssi;
    // Explicit read-then-write: C++20 deprecates ++ on a volatile. Same code,
    // without the warning; the critical section already makes it atomic.
    _resultCount = _resultCount + 1;

    portEXIT_CRITICAL(&_mutex);
    return true;
}

void BTScanner::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (!_instance) return;

    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char name[64] = {0};
            int rssi = 0;

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t* prop = &param->disc_res.prop[i];

                switch (prop->type) {
                    case ESP_BT_GAP_DEV_PROP_BDNAME: {
                        int len = prop->len > 63 ? 63 : prop->len;
                        memcpy(name, prop->val, len);
                        name[len] = '\0';
                        break;
                    }
                    case ESP_BT_GAP_DEV_PROP_RSSI:
                        rssi = *(int8_t*)prop->val;
                        break;
                    case ESP_BT_GAP_DEV_PROP_EIR: {
                        if (!name[0]) {
                            uint8_t* eir = (uint8_t*)prop->val;
                            uint8_t eirLen = 0;
                            uint8_t* nameData = esp_bt_gap_resolve_eir_data(
                                eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &eirLen);
                            if (!nameData) {
                                nameData = esp_bt_gap_resolve_eir_data(
                                    eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &eirLen);
                            }
                            if (nameData && eirLen > 0) {
                                int copyLen = eirLen > 63 ? 63 : eirLen;
                                memcpy(name, nameData, copyLen);
                                name[copyLen] = '\0';
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            // Only add named devices
            if (name[0]) {
                _instance->addResult(name, param->disc_res.bda, rssi);
            }
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                _instance->_scanning = false;
            }
            break;

        default:
            break;
    }
}
