// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Mike Garrison

#ifndef COUNTER_H
#define COUNTER_H

// Tools > Counter: two tallies side by side. The left buttons add to the
// left count and the right buttons to the right one; hold the top button
// on a side to zero that count. Bottom-left goes back. Counts are kept
// across power cycles.
void counterBegin();
void counterEnd();
void counterUpdate();

#endif // COUNTER_H
