// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_APP_H
#define HOME_REMOTE_APP_H

// Tools > Home Remote (APP_HOME_REMOTE): controls lights, scenes and
// scripts through a Home Assistant server on the home network.
//
// Screens: the top list (the favorites - entities with the "Cyber Fidget"
// label in Home Assistant - inline, then "Rooms" and "Status"), Rooms (Home
// Assistant areas) and each room's lights, scenes and scripts, the light
// screen (Power, Brightness, R, G, B, Color presets, Effect) and the effect
// list. Up/Down move, Enter opens a light or runs a scene/script, holding
// Enter for a second on a row toggles that light (or runs it) without
// opening it, Back goes up and leaves the app from the top list. On the
// light screen Left/Right change the row's value (hold to speed up) and
// the slider sets the brightness once it has reached the light's level
// (opening a light never changes it).
//
// Network: lib/HomeRemote/HomeRemoteClient.h. Settings: HomeRemoteConfig.h.

namespace HomeRemoteApp {

void begin();
void end();
void update();

#ifdef CF_TEST_CLI
/// `hr state`: the screen, selected row and (light screen) its values.
void printState();
#endif

} // namespace HomeRemoteApp

#endif // HOME_REMOTE_APP_H
