// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/WebPortalApp/CaptionWrap.h
//
// Greedy word-wrap for the OLED live-caption screen. Pure C++ — pixel
// widths come from an injected measure function, so the wrap logic
// host-tests with a fake measurer (test_miccapture_protocol) and renders
// with DisplayProxy::getStringWidth on target.
//
// Overflow keeps the TAIL: captions are read as they arrive, so when a
// long final wraps to more lines than fit, the newest lines win.

#ifndef CAPTION_WRAP_H
#define CAPTION_WRAP_H

#include <cstring>

namespace CaptionWrap {

// Worst case at the small font is ~25 chars per 128px row; 40 leaves slack
// for narrow glyphs.
static const int kMaxLineChars = 40;

typedef int (*WidthFn)(const char* s, void* ctx);

// Wrap `text` into lines no wider than maxW. Writes up to maxLines lines
// into `lines` and returns the count. When the wrapped text needs more
// than maxLines, the FIRST lines are discarded (tail-biased). Words wider
// than maxW hard-break mid-word. Empty/null text yields 0 lines.
inline int wrap(const char* text, int maxW, WidthFn widthOf, void* ctx,
                char lines[][kMaxLineChars + 1], int maxLines) {
    if (text == nullptr || maxLines <= 0) return 0;

    int count = 0;  // lines committed so far (after tail-shifting, <= maxLines)
    char cur[kMaxLineChars + 1];
    cur[0] = '\0';
    int curLen = 0;

    // Commit `cur` as a finished line, shifting the window up when full.
    auto commit = [&]() {
        if (count == maxLines) {
            for (int i = 1; i < maxLines; ++i) {
                std::strcpy(lines[i - 1], lines[i]);
            }
            count = maxLines - 1;
        }
        std::strcpy(lines[count], cur);
        ++count;
        cur[0] = '\0';
        curLen = 0;
    };

    const char* p = text;
    while (*p != '\0') {
        while (*p == ' ') ++p;       // collapse runs of spaces
        if (*p == '\0') break;
        const char* wordStart = p;
        while (*p != '\0' && *p != ' ') ++p;
        int wordLen = (int)(p - wordStart);

        while (wordLen > 0) {
            // Try to append (with a separating space if the line has text).
            char candidate[kMaxLineChars + 1];
            int take = wordLen;
            int prefix = curLen + (curLen > 0 ? 1 : 0);
            if (prefix + take > kMaxLineChars) take = kMaxLineChars - prefix;

            bool fits = false;
            if (take > 0) {
                std::strcpy(candidate, cur);
                if (curLen > 0) std::strcat(candidate, " ");
                std::strncat(candidate, wordStart, take);
                fits = widthOf(candidate, ctx) <= maxW;
                // A whole word must fit whole; only a word that can't fit
                // on an empty line gets hard-broken below.
                if (fits && take < wordLen && curLen > 0) fits = false;
            }

            if (fits && take == wordLen) {
                std::strcpy(cur, candidate);
                curLen = (int)std::strlen(cur);
                wordLen = 0;
            } else if (curLen > 0) {
                commit();                      // retry the word on a fresh line
            } else {
                // Empty line and the word still doesn't fit: hard-break it
                // at the widest prefix that does (always >= 1 char).
                int fitChars = 1;
                char piece[kMaxLineChars + 1];
                for (int t = 1; t <= wordLen && t <= kMaxLineChars; ++t) {
                    std::strncpy(piece, wordStart, t);
                    piece[t] = '\0';
                    if (widthOf(piece, ctx) > maxW && t > 1) break;
                    fitChars = t;
                }
                std::strncpy(cur, wordStart, fitChars);
                cur[fitChars] = '\0';
                curLen = fitChars;
                commit();
                wordStart += fitChars;
                wordLen -= fitChars;
            }
        }
    }
    if (curLen > 0) commit();
    return count;
}

}  // namespace CaptionWrap

#endif  // CAPTION_WRAP_H
