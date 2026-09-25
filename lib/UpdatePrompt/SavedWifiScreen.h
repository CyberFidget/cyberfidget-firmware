// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SAVED_WIFI_SCREEN_H
#define SAVED_WIFI_SCREEN_H

// Settings > Saved WiFi (APP_SAVED_WIFI): the saved networks by name, in the
// order they are tried, each with "Use this first" and "Forget". No text
// entry on the device: the last row opens Setup WiFi (the portal's WiFi
// page) to add one. Storage and join rules: lib/CloudSync/SavedWifi.h.

namespace SavedWifiScreen {

void begin();
void end();
void update();

} // namespace SavedWifiScreen

#endif
