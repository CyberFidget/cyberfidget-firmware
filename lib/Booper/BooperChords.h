// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BOOPER_CHORDS_H
#define BOOPER_CHORDS_H

// Chord sets for Booper's four note buttons (header-only so native tests can use it).
// Frequencies are at the default octave (octave 0); all are >= C4 for the speaker.
struct BooperChordSet {
    const char* name;
    float freq[4]; // top-left, top-right, middle-left, middle-right
};

static constexpr int kBooperChordSetCount = 5;

static const BooperChordSet kBooperChordSets[kBooperChordSetCount] = {
    { "Major",      { 261.63f, 329.63f, 392.00f, 523.25f } }, // C  E  G  C
    { "Minor",      { 261.63f, 311.13f, 392.00f, 523.25f } }, // C  Eb G  C
    { "Power",      { 261.63f, 392.00f, 523.25f, 783.99f } }, // C  G  C  G
    { "Sus4",       { 261.63f, 349.23f, 392.00f, 523.25f } }, // C  F  G  C
    { "Pentatonic", { 261.63f, 293.66f, 329.63f, 392.00f } }, // C  D  E  G
};

#endif // BOOPER_CHORDS_H
