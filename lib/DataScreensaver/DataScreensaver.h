// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef DATA_SCREENSAVER_H
#define DATA_SCREENSAVER_H

// Plays one stored drawing (a `.cfs` file under /assets/ss/, see
// CfsFormat.h) as a screensaver. This is the single registry slot
// (APP_ENTRY APP_DATA_SCREENSAVER) behind every `format:"cfsprite"`
// manifest entry, the same way APP_WASM_HOST hosts every delivered app:
// the menu (or the test CLI's `launch`) stages the file with setPending()
// and switches to the slot.
//
// Frames stream from the file through one frame buffer; the drawing is
// centered when smaller than the screen and plays through
// cf::gfx::AnimationPlayer with its own timing and loop mode. Back returns
// to the menu. A file that is missing or fails any format check shows
// "Can't play this one" and any button returns to the menu; nothing on
// this path can reset the device.
namespace DataScreensaver {

/// Stage the next launch (copied; takes effect at the next appBegin()).
void setPending(const char* path, const char* label);

// Registry glue (wired via APP_ENTRY in AppManifest.h).
void appBegin();
void appUpdate();
void appEnd();

}  // namespace DataScreensaver

#endif  // DATA_SCREENSAVER_H
