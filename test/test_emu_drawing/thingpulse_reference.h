/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 by ThingPulse, Daniel Eichhorn
 * Copyright (c) 2018 by Fabrice Weinberg
 * Copyright (c) 2019 by Helmut Tschemernjak - www.radioshuttle.de
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ThingPulse invests considerable time and money to develop these open source libraries.
 * Please support us by buying our products (and not the clones) from
 * https://thingpulse.com
 *
 */


#pragma once
// Drawing bodies copied verbatim from reference/thingpulse/OLEDDisplay.cpp.
// Only hardware scheduling and diagnostic output are stubbed in this harness.
#define _swap_int16_t(a,b) std::swap(a,b)
#define DEBUG_OLEDDISPLAY(...) ((void)0)
class ThingPulseReference {
public:
    uint8_t buffer[1024] = {};
    OLEDDISPLAY_COLOR color = WHITE;
    OLEDDISPLAY_TEXT_ALIGNMENT textAlignment = TEXT_ALIGN_LEFT;
    const uint8_t* fontData = ArialMT_Plain_10;
    static constexpr int displayBufferSize = 1024;
    enum { HEIGHT_POS=1, FIRST_CHAR_POS=2, CHAR_NUM_POS=3,
           JUMPTABLE_START=4, JUMPTABLE_BYTES=4, JUMPTABLE_LSB=1,
           JUMPTABLE_SIZE=2, JUMPTABLE_WIDTH=3 };
    uint16_t width() { return 128; }
    uint16_t height() { return 64; }
    void setColor(OLEDDISPLAY_COLOR c) { color=c; }
    void clear() { memset(buffer,0,sizeof(buffer)); }
    void setFont(const uint8_t* f) { fontData=f; }
    void setTextAlignment(OLEDDISPLAY_TEXT_ALIGNMENT a) { textAlignment=a; }
static char fontTableLookupFunction(const uint8_t ch) {
    // UTF-8 to font table index converter
    // Code form http://playground.arduino.cc/Main/Utf8ascii
	static uint8_t LASTCHAR;

	if (ch < 128) { // Standard ASCII-set 0..0x7F handling
		LASTCHAR = 0;
		return ch;
	}

	uint8_t last = LASTCHAR;   // get last char
	LASTCHAR = ch;

	switch (last) {    // conversion depnding on first UTF8-character
		case 0xC2: return (uint8_t) ch;
		case 0xC3: return (uint8_t) (ch | 0xC0);
		case 0x82: if (ch == 0xAC) return (uint8_t) 0x80;    // special case Euro-symbol
	}

	return (uint8_t) 0; // otherwise: return zero, if character has to be ignored
}
void setPixel(int16_t x, int16_t y) {
  if (x >= 0 && x < this->width() && y >= 0 && y < this->height()) {
    switch (color) {
      case WHITE:   buffer[x + (y / 8) * this->width()] |=  (1 << (y & 7)); break;
      case BLACK:   buffer[x + (y / 8) * this->width()] &= ~(1 << (y & 7)); break;
      case INVERSE: buffer[x + (y / 8) * this->width()] ^=  (1 << (y & 7)); break;
    }
  }
}
void setPixelColor(int16_t x, int16_t y, OLEDDISPLAY_COLOR color) {
  if (x >= 0 && x < this->width() && y >= 0 && y < this->height()) {
    switch (color) {
      case WHITE:   buffer[x + (y / 8) * this->width()] |=  (1 << (y & 7)); break;
      case BLACK:   buffer[x + (y / 8) * this->width()] &= ~(1 << (y & 7)); break;
      case INVERSE: buffer[x + (y / 8) * this->width()] ^=  (1 << (y & 7)); break;
    }
  }
}
void clearPixel(int16_t x, int16_t y) {
  if (x >= 0 && x < this->width() && y >= 0 && y < this->height()) {
    switch (color) {
      case BLACK:   buffer[x + (y >> 3) * this->width()] |=  (1 << (y & 7)); break;
      case WHITE:   buffer[x + (y >> 3) * this->width()] &= ~(1 << (y & 7)); break;
      case INVERSE: buffer[x + (y >> 3) * this->width()] ^=  (1 << (y & 7)); break;
    }
  }
}
void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  int16_t steep = abs(y1 - y0) > abs(x1 - x0);
  if (steep) {
    _swap_int16_t(x0, y0);
    _swap_int16_t(x1, y1);
  }

  if (x0 > x1) {
    _swap_int16_t(x0, x1);
    _swap_int16_t(y0, y1);
  }

  int16_t dx, dy;
  dx = x1 - x0;
  dy = abs(y1 - y0);

  int16_t err = dx / 2;
  int16_t ystep;

  if (y0 < y1) {
    ystep = 1;
  } else {
    ystep = -1;
  }

  for (; x0<=x1; x0++) {
    if (steep) {
      setPixel(y0, x0);
    } else {
      setPixel(x0, y0);
    }
    err -= dy;
    if (err < 0) {
      y0 += ystep;
      err += dx;
    }
  }
}
void drawRect(int16_t x, int16_t y, int16_t width, int16_t height) {
  drawHorizontalLine(x, y, width);
  drawVerticalLine(x, y, height);
  drawVerticalLine(x + width - 1, y, height);
  drawHorizontalLine(x, y + height - 1, width);
}
void fillRect(int16_t xMove, int16_t yMove, int16_t width, int16_t height) {
  for (int16_t x = xMove; x < xMove + width; x++) {
    drawVerticalLine(x, yMove, height);
  }
}
void drawCircle(int16_t x0, int16_t y0, int16_t radius) {
  int16_t x = 0, y = radius;
	int16_t dp = 1 - radius;
	do {
		if (dp < 0)
			dp = dp + (x++) * 2 + 3;
		else
			dp = dp + (x++) * 2 - (y--) * 2 + 5;

		setPixel(x0 + x, y0 + y);     //For the 8 octants
		setPixel(x0 - x, y0 + y);
		setPixel(x0 + x, y0 - y);
		setPixel(x0 - x, y0 - y);
		setPixel(x0 + y, y0 + x);
		setPixel(x0 - y, y0 + x);
		setPixel(x0 + y, y0 - x);
		setPixel(x0 - y, y0 - x);

	} while (x < y);

  setPixel(x0 + radius, y0);
  setPixel(x0, y0 + radius);
  setPixel(x0 - radius, y0);
  setPixel(x0, y0 - radius);
}
void drawCircleQuads(int16_t x0, int16_t y0, int16_t radius, uint8_t quads) {
  int16_t x = 0, y = radius;
  int16_t dp = 1 - radius;
  while (x < y) {
    if (dp < 0)
      dp = dp + (x++) * 2 + 3;
    else
      dp = dp + (x++) * 2 - (y--) * 2 + 5;
    if (quads & 0x1) {
      setPixel(x0 + x, y0 - y);
      setPixel(x0 + y, y0 - x);
    }
    if (quads & 0x2) {
      setPixel(x0 - y, y0 - x);
      setPixel(x0 - x, y0 - y);
    }
    if (quads & 0x4) {
      setPixel(x0 - y, y0 + x);
      setPixel(x0 - x, y0 + y);
    }
    if (quads & 0x8) {
      setPixel(x0 + x, y0 + y);
      setPixel(x0 + y, y0 + x);
    }
  }
  if (quads & 0x1 && quads & 0x8) {
    setPixel(x0 + radius, y0);
  }
  if (quads & 0x4 && quads & 0x8) {
    setPixel(x0, y0 + radius);
  }
  if (quads & 0x2 && quads & 0x4) {
    setPixel(x0 - radius, y0);
  }
  if (quads & 0x1 && quads & 0x2) {
    setPixel(x0, y0 - radius);
  }
}
void fillCircle(int16_t x0, int16_t y0, int16_t radius) {
  int16_t x = 0, y = radius;
	int16_t dp = 1 - radius;
	do {
		if (dp < 0)
      dp = dp + (x++) * 2 + 3;
    else
      dp = dp + (x++) * 2 - (y--) * 2 + 5;

    drawHorizontalLine(x0 - x, y0 - y, 2*x);
    drawHorizontalLine(x0 - x, y0 + y, 2*x);
    drawHorizontalLine(x0 - y, y0 - x, 2*y);
    drawHorizontalLine(x0 - y, y0 + x, 2*y);


	} while (x < y);
  drawHorizontalLine(x0 - radius, y0, 2 * radius);

}
void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               int16_t x2, int16_t y2) {
  drawLine(x0, y0, x1, y1);
  drawLine(x1, y1, x2, y2);
  drawLine(x2, y2, x0, y0);
}
void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               int16_t x2, int16_t y2) {
  int16_t a, b, y, last;

  if (y0 > y1) {
    _swap_int16_t(y0, y1);
    _swap_int16_t(x0, x1);
  }
  if (y1 > y2) {
    _swap_int16_t(y2, y1);
    _swap_int16_t(x2, x1);
  }
  if (y0 > y1) {
    _swap_int16_t(y0, y1);
    _swap_int16_t(x0, x1);
  }

  if (y0 == y2) {
    a = b = x0;
    if (x1 < a) {
      a = x1;
    } else if (x1 > b) {
      b = x1;
    }
    if (x2 < a) {
      a = x2;
    } else if (x2 > b) {
      b = x2;
    }
    drawHorizontalLine(a, y0, b - a + 1);
    return;
  }

  int16_t
    dx01 = x1 - x0,
    dy01 = y1 - y0,
    dx02 = x2 - x0,
    dy02 = y2 - y0,
    dx12 = x2 - x1,
    dy12 = y2 - y1;
  int32_t
    sa   = 0,
    sb   = 0;

  if (y1 == y2) {
    last = y1; // Include y1 scanline
  } else {
    last = y1 - 1; // Skip it
  }

  for (y = y0; y <= last; y++) {
    a = x0 + sa / dy01;
    b = x0 + sb / dy02;
    sa += dx01;
    sb += dx02;

    if (a > b) {
      _swap_int16_t(a, b);
    }
    drawHorizontalLine(a, y, b - a + 1);
  }

  sa = dx12 * (y - y1);
  sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    a = x1 + sa / dy12;
    b = x0 + sb / dy02;
    sa += dx12;
    sb += dx02;

    if (a > b) {
      _swap_int16_t(a, b);
    }
    drawHorizontalLine(a, y, b - a + 1);
  }
}
void drawHorizontalLine(int16_t x, int16_t y, int16_t length) {
  if (y < 0 || y >= this->height()) { return; }

  if (x < 0) {
    length += x;
    x = 0;
  }

  if ( (x + length) > this->width()) {
    length = (this->width() - x);
  }

  if (length <= 0) { return; }

  uint8_t * bufferPtr = buffer;
  bufferPtr += (y >> 3) * this->width();
  bufferPtr += x;

  uint8_t drawBit = 1 << (y & 7);

  switch (color) {
    case WHITE:   while (length--) {
        *bufferPtr++ |= drawBit;
      }; break;
    case BLACK:   drawBit = ~drawBit;   while (length--) {
        *bufferPtr++ &= drawBit;
      }; break;
    case INVERSE: while (length--) {
        *bufferPtr++ ^= drawBit;
      }; break;
  }
}
void drawVerticalLine(int16_t x, int16_t y, int16_t length) {
  if (x < 0 || x >= this->width()) return;

  if (y < 0) {
    length += y;
    y = 0;
  }

  if ( (y + length) > this->height()) {
    length = (this->height() - y);
  }

  if (length <= 0) return;


  uint8_t yOffset = y & 7;
  uint8_t drawBit;
  uint8_t *bufferPtr = buffer;

  bufferPtr += (y >> 3) * this->width();
  bufferPtr += x;

  if (yOffset) {
    yOffset = 8 - yOffset;
    drawBit = ~(0xFF >> (yOffset));

    if (length < yOffset) {
      drawBit &= (0xFF >> (yOffset - length));
    }

    switch (color) {
      case WHITE:   *bufferPtr |=  drawBit; break;
      case BLACK:   *bufferPtr &= ~drawBit; break;
      case INVERSE: *bufferPtr ^=  drawBit; break;
    }

    if (length < yOffset) return;

    length -= yOffset;
    bufferPtr += this->width();
  }

  if (length >= 8) {
    switch (color) {
      case WHITE:
      case BLACK:
        drawBit = (color == WHITE) ? 0xFF : 0x00;
        do {
          *bufferPtr = drawBit;
          bufferPtr += this->width();
          length -= 8;
        } while (length >= 8);
        break;
      case INVERSE:
        do {
          *bufferPtr = ~(*bufferPtr);
          bufferPtr += this->width();
          length -= 8;
        } while (length >= 8);
        break;
    }
  }

  if (length > 0) {
    drawBit = (1 << (length & 7)) - 1;
    switch (color) {
      case WHITE:   *bufferPtr |=  drawBit; break;
      case BLACK:   *bufferPtr &= ~drawBit; break;
      case INVERSE: *bufferPtr ^=  drawBit; break;
    }
  }
}
void drawProgressBar(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t progress) {
  uint16_t radius = height / 2;
  uint16_t xRadius = x + radius;
  uint16_t yRadius = y + radius;
  uint16_t doubleRadius = 2 * radius;
  uint16_t innerRadius = radius - 2;

  setColor(WHITE);
  drawCircleQuads(xRadius, yRadius, radius, 0b00000110);
  drawHorizontalLine(xRadius, y, width - doubleRadius + 1);
  drawHorizontalLine(xRadius, y + height, width - doubleRadius + 1);
  drawCircleQuads(x + width - radius, yRadius, radius, 0b00001001);

  uint16_t maxProgressWidth = (width - doubleRadius + 1) * progress / 100;

  fillCircle(xRadius, yRadius, innerRadius);
  fillRect(xRadius + 1, y + 2, maxProgressWidth, height - 3);
  fillCircle(xRadius + maxProgressWidth, yRadius, innerRadius);
}
void drawFastImage(int16_t xMove, int16_t yMove, int16_t width, int16_t height, const uint8_t *image) {
  drawInternal(xMove, yMove, width, height, image, 0, 0);
}
void drawXbm(int16_t xMove, int16_t yMove, int16_t width, int16_t height, const uint8_t *xbm) {
  int16_t widthInXbm = (width + 7) / 8;
  uint8_t data = 0;

  for(int16_t y = 0; y < height; y++) {
    for(int16_t x = 0; x < width; x++ ) {
      if (x & 7) {
        data >>= 1; // Move a bit
      } else {  // Read new data every 8 bit
        data = pgm_read_byte(xbm + (x / 8) + y * widthInXbm);
      }
      // if there is a bit draw it
      if (data & 0x01) {
        setPixel(xMove + x, yMove + y);
      }
    }
  }
}
void drawIco16x16(int16_t xMove, int16_t yMove, const uint8_t *ico, bool inverse) {
  uint16_t data;

  for(int16_t y = 0; y < 16; y++) {
    data = pgm_read_byte(ico + (y << 1)) + (pgm_read_byte(ico + (y << 1) + 1) << 8);
    for(int16_t x = 0; x < 16; x++ ) {
      if ((data & 0x01) ^ inverse) {
        setPixelColor(xMove + x, yMove + y, WHITE);
      } else {
        setPixelColor(xMove + x, yMove + y, BLACK);
      }
      data >>= 1; // Move a bit
    }
  }
}
uint16_t drawString(int16_t xMove, int16_t yMove, const String &strUser) {
  uint16_t lineHeight = pgm_read_byte(fontData + HEIGHT_POS);

  // char* text must be freed!
  char* text = strdup(strUser.c_str());
  if (!text) {
    DEBUG_OLEDDISPLAY("[OLEDDISPLAY][drawString] Can't allocate char array.\n");
    return 0;
  }

  uint16_t yOffset = 0;
  // If the string should be centered vertically too
  // we need to now how heigh the string is.
  if (textAlignment == TEXT_ALIGN_CENTER_BOTH) {
    uint16_t lb = 0;
    // Find number of linebreaks in text
    for (uint16_t i=0;text[i] != 0; i++) {
      lb += (text[i] == 10);
    }
    // Calculate center
    yOffset = (lb * lineHeight) / 2;
  }

  uint16_t charDrawn = 0;
  uint16_t line = 0;
  char* textPart = strtok(text,"\n");
  while (textPart != NULL) {
    uint16_t length = strlen(textPart);
    charDrawn += drawStringInternal(xMove, yMove - yOffset + (line++) * lineHeight, textPart, length, getStringWidth(textPart, length, true), true);
    textPart = strtok(NULL, "\n");
  }
  free(text);
  return charDrawn;
}
uint16_t drawStringMaxWidth(int16_t xMove, int16_t yMove, uint16_t maxLineWidth, const String &strUser) {
  uint16_t firstChar  = pgm_read_byte(fontData + FIRST_CHAR_POS);
  uint16_t lineHeight = pgm_read_byte(fontData + HEIGHT_POS);

  const char* text = strUser.c_str();

  uint16_t length = strlen(text);
  uint16_t lastDrawnPos = 0;
  uint16_t lineNumber = 0;
  uint16_t strWidth = 0;

  uint16_t preferredBreakpoint = 0;
  uint16_t widthAtBreakpoint = 0;
  uint16_t firstLineChars = 0;
  uint16_t drawStringResult = 1; // later tested for 0 == error, so initialize to 1

  for (uint16_t i = 0; i < length; i++) {
    char c = (this->fontTableLookupFunction)(text[i]);
    if (c == 0)
      continue;
    strWidth += pgm_read_byte(fontData + JUMPTABLE_START + (c - firstChar) * JUMPTABLE_BYTES + JUMPTABLE_WIDTH);

    // Always try to break on a space, dash or slash
    if (text[i] == ' ' || text[i]== '-' || text[i] == '/') {
      preferredBreakpoint = i + 1;
      widthAtBreakpoint = strWidth;
    }

    if (strWidth >= maxLineWidth) {
      if (preferredBreakpoint == 0) {
        preferredBreakpoint = i;
        widthAtBreakpoint = strWidth;
      }
      drawStringResult = drawStringInternal(xMove, yMove + (lineNumber++) * lineHeight , &text[lastDrawnPos], preferredBreakpoint - lastDrawnPos, widthAtBreakpoint, true);
      if (firstLineChars == 0)
        firstLineChars = preferredBreakpoint;
      lastDrawnPos = preferredBreakpoint;
      // It is possible that we did not draw all letters to i so we need
      // to account for the width of the chars from `i - preferredBreakpoint`
      // by calculating the width we did not draw yet.
      strWidth = strWidth - widthAtBreakpoint;
      preferredBreakpoint = 0;
      if (drawStringResult == 0) // we are past the display already?
        break;
    }
  }

  // Draw last part if needed
  if (drawStringResult != 0 && lastDrawnPos < length) {
    drawStringResult = drawStringInternal(xMove, yMove + (lineNumber++) * lineHeight , &text[lastDrawnPos], length - lastDrawnPos, getStringWidth(&text[lastDrawnPos], length - lastDrawnPos, true), true);
  }

  if (drawStringResult == 0 || (yMove + lineNumber * lineHeight) >= this->height()) // text did not fit on screen
    return firstLineChars;
  return 0; // everything was drawn
}
uint16_t getStringWidth(const char* text, uint16_t length, bool utf8 = false) {
  uint16_t firstChar        = pgm_read_byte(fontData + FIRST_CHAR_POS);

  uint16_t stringWidth = 0;
  uint16_t maxWidth = 0;

  for (uint16_t i = 0; i < length; i++) {
    char c = text[i];
    if (utf8) {
      c = (this->fontTableLookupFunction)(c);
      if (c == 0)
        continue;
    }
    stringWidth += pgm_read_byte(fontData + JUMPTABLE_START + (c - firstChar) * JUMPTABLE_BYTES + JUMPTABLE_WIDTH);
    if (c == 10) {
      maxWidth = max(maxWidth, stringWidth);
      stringWidth = 0;
    }
  }

  return max(maxWidth, stringWidth);
}
uint16_t drawStringInternal(int16_t xMove, int16_t yMove, const char* text, uint16_t textLength, uint16_t textWidth, bool utf8 = false) {
  uint8_t textHeight       = pgm_read_byte(fontData + HEIGHT_POS);
  uint8_t firstChar        = pgm_read_byte(fontData + FIRST_CHAR_POS);
  uint16_t sizeOfJumpTable = pgm_read_byte(fontData + CHAR_NUM_POS)  * JUMPTABLE_BYTES;

  uint16_t cursorX         = 0;
  uint16_t cursorY         = 0;
  uint16_t charCount       = 0;

  switch (textAlignment) {
    case TEXT_ALIGN_CENTER_BOTH:
      yMove -= textHeight >> 1;
    // Fallthrough
    case TEXT_ALIGN_CENTER:
      xMove -= textWidth >> 1; // divide by 2
      break;
    case TEXT_ALIGN_RIGHT:
      xMove -= textWidth;
      break;
    case TEXT_ALIGN_LEFT:
      break;
  }

  // Don't draw anything if it is not on the screen.
  if (xMove + textWidth  < 0 || xMove >= this->width() ) {return 0;}
  if (yMove + textHeight < 0 || yMove >= this->height()) {return 0;}

  for (uint16_t j = 0; j < textLength; j++) {
    int16_t xPos = xMove + cursorX;
    int16_t yPos = yMove + cursorY;
    if (xPos > this->width())
      break; // no need to continue
    charCount++;

    uint8_t code;
    if (utf8) {
      code = (this->fontTableLookupFunction)(text[j]);
      if (code == 0)
        continue;
    } else
      code = text[j];
    if (code >= firstChar) {
      uint8_t charCode = code - firstChar;

      // 4 Bytes per char code
      uint8_t msbJumpToChar    = pgm_read_byte( fontData + JUMPTABLE_START + charCode * JUMPTABLE_BYTES );                  // MSB  \ JumpAddress
      uint8_t lsbJumpToChar    = pgm_read_byte( fontData + JUMPTABLE_START + charCode * JUMPTABLE_BYTES + JUMPTABLE_LSB);   // LSB /
      uint8_t charByteSize     = pgm_read_byte( fontData + JUMPTABLE_START + charCode * JUMPTABLE_BYTES + JUMPTABLE_SIZE);  // Size
      uint8_t currentCharWidth = pgm_read_byte( fontData + JUMPTABLE_START + charCode * JUMPTABLE_BYTES + JUMPTABLE_WIDTH); // Width

      // Test if the char is drawable
      if (!(msbJumpToChar == 255 && lsbJumpToChar == 255)) {
        // Get the position of the char data
        uint16_t charDataPosition = JUMPTABLE_START + sizeOfJumpTable + ((msbJumpToChar << 8) + lsbJumpToChar);
        drawInternal(xPos, yPos, currentCharWidth, textHeight, fontData, charDataPosition, charByteSize);
      }

      cursorX += currentCharWidth;
    }
  }
  return charCount;
}
void drawInternal(int16_t xMove, int16_t yMove, int16_t width, int16_t height, const uint8_t *data, uint16_t offset, uint16_t bytesInData) {
  if (width < 0 || height < 0) return;
  if (yMove + height < 0 || yMove > this->height())  return;
  if (xMove + width  < 0 || xMove > this->width())   return;

  uint8_t  rasterHeight = 1 + ((height - 1) >> 3); // fast ceil(height / 8.0)
  int8_t   yOffset      = yMove & 7;

  bytesInData = bytesInData == 0 ? width * rasterHeight : bytesInData;

  int16_t initYMove   = yMove;
  int8_t  initYOffset = yOffset;


  for (uint16_t i = 0; i < bytesInData; i++) {

    // Reset if next horizontal drawing phase is started.
    if ( i % rasterHeight == 0) {
      yMove   = initYMove;
      yOffset = initYOffset;
    }

    uint8_t currentByte = pgm_read_byte(data + offset + i);

    int16_t xPos = xMove + (i / rasterHeight);
    int16_t yPos = ((yMove >> 3) + (i % rasterHeight)) * this->width();

//    int16_t yScreenPos = yMove + yOffset;
    int16_t dataPos    = xPos  + yPos;

    if (dataPos >=  0  && dataPos < displayBufferSize &&
        xPos    >=  0  && xPos    < this->width() ) {

      if (yOffset >= 0) {
        switch (this->color) {
          case WHITE:   buffer[dataPos] |= currentByte << yOffset; break;
          case BLACK:   buffer[dataPos] &= ~(currentByte << yOffset); break;
          case INVERSE: buffer[dataPos] ^= currentByte << yOffset; break;
        }

        if (dataPos < (displayBufferSize - this->width())) {
          switch (this->color) {
            case WHITE:   buffer[dataPos + this->width()] |= currentByte >> (8 - yOffset); break;
            case BLACK:   buffer[dataPos + this->width()] &= ~(currentByte >> (8 - yOffset)); break;
            case INVERSE: buffer[dataPos + this->width()] ^= currentByte >> (8 - yOffset); break;
          }
        }
      } else {
        // Make new offset position
        yOffset = -yOffset;

        switch (this->color) {
          case WHITE:   buffer[dataPos] |= currentByte >> yOffset; break;
          case BLACK:   buffer[dataPos] &= ~(currentByte >> yOffset); break;
          case INVERSE: buffer[dataPos] ^= currentByte >> yOffset; break;
        }

        // Prepare for next iteration by moving one block up
        yMove -= 8;

        // and setting the new yOffset
        yOffset = 8 - yOffset;
      }
#ifndef __MBED__
      yield();
#endif
    }
  }
}
uint16_t getStringWidth(const String &strUser) {
  uint16_t width = getStringWidth(strUser.c_str(), strUser.length());
  return width;
}
};
#undef _swap_int16_t
#undef DEBUG_OLEDDISPLAY
