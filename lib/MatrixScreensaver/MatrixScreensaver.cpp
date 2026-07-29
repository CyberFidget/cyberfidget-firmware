// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "MatrixScreensaver.h"
#include "HAL.h"

MatrixScreensaver matrixScreensaver(HAL::buttonManager()); // AppManager Integration
MatrixScreensaver* MatrixScreensaver::instance = nullptr;

// Just an example set
const char MatrixScreensaver::ALIEN_CHARS[16] = {
    '@', '#', '$', '%', '*', '+', '=','?',
    'Z', 'N', 'J', '!', ':', ';', '(', ')'
};

/* 
   Suppose we have an 8-high font stored in an array MY_FONT[] where:

   - Each glyph is 8 bytes (one byte per row).
   - The bits in each byte define which columns are on/off.
   - Typically, a '1' bit means "pixel on."
   - We have a function findGlyphOffset(c) to get the start index in MY_FONT.

   We'll do a small dummy example here. In real code, you'll have the full array.
*/
static const uint8_t MY_FONT[] PROGMEM = {
    // minimal data... you'd have full glyphs for your entire ASCII or custom set
    // For demonstration, let's say each glyph = 8 bytes
    // '!' => 0x00,0x00,0xF8,... etc. 
    // We'll just have placeholders:
    // Use this to draw custom characters, careful of bytes per glyph and height/width match - https://sourpuss.net/projects/fontedit/
    //0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x5F, // '!'
    //0x18,0x3E,0x22,0x40,0x1A,0x32,0x26,0x18,
	0x00,0x7C,0x76,0x58,0x1A,0x76,0x3C,	// 33
	0x00,0x62,0x66,0x3E,0x0C,0x78,0x70,	// 34
	0x00,0x7E,0x3E,0x02,0x3E,0x7E,0x06,	// 35
	0x00,0x7E,0x0E,0x02,0x38,0x7C,0x44,	// 36
	0x00,0x74,0x14,0x16,0x16,0x7E,0x7C,	// 37
    
    // ... etc. for other chars ...
};

// If your font maps ASCII 33 ('!') at index 0 in the array, 
// ASCII 34 ('"') at index 8, etc., then:
static uint16_t glyphBase = 32; // first character in MY_FONT is '!'
static uint16_t glyphSize = 7;  // 8 bytes per glyph

// Example function: findGlyphOffset(c)
// Returns the index in MY_FONT for character c, or a default if out of range
uint16_t MatrixScreensaver::findGlyphOffset(char c) {
    // ASCII code
    uint16_t code = (uint8_t)c;
    // clamp
    if (code < glyphBase || code > 126) {
        // fallback to e.g. '!'
        code = glyphBase;
    }
    // index in MY_FONT
    uint16_t glyphIndex = (code - glyphBase) * glyphSize; 
    return glyphIndex;
}

// Constructor
MatrixScreensaver::MatrixScreensaver(ButtonManager& btnMgr)
    : display(HAL::displayProxy()),
    buttonManager(btnMgr) // AppManager Integration
{
    // e.g. update the state machine every 30ms
    frameInterval = 30;

    // rowPixelDelay = how many ms between increments of litPixels
    // for example, 30ms means each pixel line lights up or down each 30ms
    // => 8 lines * 30ms = 240ms to fully light a row
    rowPixelDelay = 30;
    instance = this; // AppManager Integration    
}

void MatrixScreensaver::begin() {
    registerButtonCallbacks(); // AppManager integration
    unsigned long now = millis();

    for (int i = 0; i < NUM_COLUMNS; i++) {
        Column &col = columns[i];

        col.state = OFF;
        col.currentRow = -1; // no row being turned on/off
        col.nextStep = 0;

        // random durations
        col.onDuration = random(1500, 4000);
        col.offDuration = random(500, 2000);
        col.nextStateChangeTime = now + col.offDuration; // remain OFF until then

        // init row data
        for (int r = 0; r < NUM_ROWS; r++) {
            col.rows[r].ch = ' ';
            col.rows[r].litPixels = 0;
        }
    }

    lastUpdateTime = now;
}

void MatrixScreensaver::update() {
    unsigned long now = millis();
    if (now - lastUpdateTime < frameInterval) {
        return;
    }
    lastUpdateTime = now;

    for (int i = 0; i < NUM_COLUMNS; i++) {
        Column &col = columns[i];

        switch (col.state) {
            case OFF: {
                // check if time to transition to TURNING_ON
                if (now >= col.nextStateChangeTime) {
                    startTurningOn(col, now);
                }
            } break;

            case TURNING_ON: {
                // col.currentRow goes from 0..NUM_ROWS-1
                if (col.currentRow >= 0 && col.currentRow < NUM_ROWS) {
                    RowInfo &rw = col.rows[col.currentRow];
                    // increment litPixels
                    if (now >= col.nextStep) {
                        if (rw.litPixels < FONT_HEIGHT) {
                            rw.litPixels++;
                            col.nextStep = now + rowPixelDelay;
                        } else {
                            // row is fully lit, move to next row
                            col.currentRow++;
                            if (col.currentRow < NUM_ROWS) {
                                col.nextStep = now + rowPixelDelay;
                            } else {
                                // reached bottom => fully ON
                                col.state = ON;
                                col.nextStateChangeTime = now + col.onDuration;
                            }
                        }
                    }
                }
            } break;

            case ON: {
                // Check if time to start turning off
                if (now >= col.nextStateChangeTime) {
                    startTurningOff(col, now);
                }

                // Flicker for any fully lit row
                for (int r = 0; r < NUM_ROWS; r++) {
                    RowInfo &rw = col.rows[r];
                    // If the row is fully lit
                    if (rw.litPixels == FONT_HEIGHT) {
                        if ((int32_t)((uint32_t)now - rw.nextChange) >= 0) {
                            rw.ch = randomAlienChar();
                            // next flicker in 300..800 ms
                            rw.nextChange = now + random(300, 800);
                        }
                    }
                }
            } break;

            case TURNING_OFF: {
                // turning off from top to bottom means row 0 first, then row 1, etc., turning off pixels bottom to top in each glyph
                if (col.currentRow >= 0 && col.currentRow < NUM_ROWS) {
                    RowInfo &rw = col.rows[col.currentRow];
                    if (now >= col.nextStep) {
                        if (rw.litPixels > 0) {
                            rw.litPixels--;
                            col.nextStep = now + rowPixelDelay;
                        } else {
                            // row fully off, move to next
                            col.currentRow++;
                            if (col.currentRow < NUM_ROWS) {
                                col.nextStep = now + rowPixelDelay;
                            } else {
                                // all rows off => OFF state
                                col.state = OFF;
                                col.nextStateChangeTime = now + col.offDuration;
                            }
                        }
                    }
                }
            } break;
        }
    }
    draw();
}

void MatrixScreensaver::draw() {
    display.clear();
    // If using built-in text, set a font, but we'll do partial draws ourselves:
    display.setFont(ArialMT_Plain_10); // Not used for partial pixel rendering

    for (int i = 0; i < NUM_COLUMNS; i++) {
        int colX = i * (FONT_WIDTH + 1);
        Column &col = columns[i];

        bool isTurningOn = (col.state == TURNING_ON || col.state == ON);

        // Draw each row
        for (int r = 0; r < NUM_ROWS; r++) {
            int rowY = r * FONT_HEIGHT;
            char c   = col.rows[r].ch;
            int lp   = col.rows[r].litPixels; // 0..8

            if (lp > 0) {
                // Call partial draw with direction info
                drawCharPartial(colX, rowY, c, lp, isTurningOn);
            }
        }
    }

    display.display();
}

// OFF->TURNING_ON
void MatrixScreensaver::startTurningOn(Column &col, unsigned long now) {
    col.state = TURNING_ON;
    col.currentRow = 0; // start at row #0
    col.nextStep = now + rowPixelDelay;

    // randomize entire column’s symbol set each time we go from OFF->ON
    randomizeColumnSymbols(col);

    // reset all litPixels=0
    for (int r = 0; r < NUM_ROWS; r++) {
        col.rows[r].litPixels = 0;
    }
}

// ON->TURNING_OFF
void MatrixScreensaver::startTurningOff(Column &col, unsigned long now) {
    col.state = TURNING_OFF;
    col.currentRow = 0;  // fade out from top row first
    col.nextStep = now + rowPixelDelay;
}


// Fill with new random chars
void MatrixScreensaver::randomizeColumnSymbols(Column &col) {
    for (int r = 0; r < NUM_ROWS; r++) {
        col.rows[r].ch = randomAlienChar();
        col.rows[r].nextChange = millis() + random(300, 800);
        // or just set nextChange to some future time
    }
}
char MatrixScreensaver::randomAlienChar() {
    int idx = random(0, 16);
    return ALIEN_CHARS[idx];
}

/**
 * @param x, y      Top-left of the character cell on screen.
 * @param c         The character to draw.
 * @param litPixels How many pixel lines are lit (0..FONT_HEIGHT).
 * @param fill      True if we are filling ON (top to bottom),
 *                  False if we are fading OFF (top to bottom).
 */
void MatrixScreensaver::drawCharPartial(int x, int y, char c, int litPixels, bool fill) {
    // find glyph offset in your font
    uint16_t offset = findGlyphOffset(c);

    // If fill == true, interpret litPixels as "draw lines [0..litPixels-1]"
    // so if litPixels=1 => only row 0, litPixels=8 => rows [0..7].
    // This yields a top-down reveal (top line appears first).
    if (fill) {
        // clamp
        if (litPixels > FONT_HEIGHT) {
            litPixels = FONT_HEIGHT;
        }
        int endRow = litPixels - 1; 
        for (int row = 0; row <= endRow; row++) {
            uint8_t rowData = pgm_read_byte(&MY_FONT[offset + row]);
            int drawY = y + row; 
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (rowData & (1 << col)) {
                    display.setPixel(x + col, drawY);
                }
            }
        }
    } else {
        // fill == false => turning OFF from the top line first
        // If litPixels=8 => full glyph rows [0..7].
        // If litPixels=7 => skip row 0 => draw [1..7].
        // If litPixels=1 => skip rows [0..6], only draw row 7. 
        // => top row disappears first.
        
        if (litPixels > FONT_HEIGHT) {
            litPixels = FONT_HEIGHT;
        }
        // start row = how many lines from the top we skip
        int startRow = FONT_HEIGHT - litPixels; 
        if (startRow < 0) {
            startRow = 0;
        }
        for (int row = startRow; row < FONT_HEIGHT; row++) {
            uint8_t rowData = pgm_read_byte(&MY_FONT[offset + row]);
            int drawY = y + row; 
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (rowData & (1 << col)) {
                    display.setPixel(x + col, drawY);
                }
            }
        }
    }
}

// AppManager integration

void MatrixScreensaver::registerButtonCallbacks() {
    // Exit App
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void MatrixScreensaver::unregisterButtonCallbacks() {
    // Unregister callbacks
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void MatrixScreensaver::end() {
    ESP_LOGI(TAG_MAIN, "end() => unregistering booper callbacks...");
    unregisterButtonCallbacks();
}


void MatrixScreensaver::onButtonBackPressed(const ButtonEvent& event)
{    // Press
    if (event.eventType == ButtonEvent_Released){
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    } 
}
void MatrixScreensaver::onButtonSelectPressed(const ButtonEvent& event)
{    
    // Press
    if (event.eventType == ButtonEvent_Pressed){
        //instance().selectCurrentItem();
    }
}

