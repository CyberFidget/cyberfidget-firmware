// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "ScrollLabel.h"

#include <Arduino.h>
#include <string.h>

#include "HAL.h"
#include "DisplayProxy.h"

void ScrollLabel::draw(int x, int y, int boxWidth, const char* text, bool centerWhenShort)
{
    if (!text) text = "";
    auto &display = HAL::displayProxy();

    const int textWidth = display.getStringWidth(text, (uint16_t)strlen(text));
    tick(textWidth, boxWidth, (uint32_t)millis());

    if (needsScroll(textWidth, boxWidth)) {
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(x - offset_, y, text);
    } else if (centerWhenShort) {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(x + boxWidth / 2, y, text);
    } else {
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(x, y, text);
    }
}
