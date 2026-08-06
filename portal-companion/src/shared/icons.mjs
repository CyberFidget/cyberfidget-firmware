// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The five navigation icons, as 9x9 pixel bitmaps.
//
// sync_chrome.mjs expands each one into a single precomputed CSS box-shadow
// rule (~130 B) drawn in currentColor, so the shipped surfaces need no icon
// font, no emoji and no fetched sprite. That matters because the device serves
// the memory card over one SPI stream and wedges under concurrent reads: each
// page has to be a single self-contained request. Emoji are additionally
// untintable - they render in fixed multi-colour whatever the surface wants.
//
// '#' is an inked pixel, '.' is empty. Nine rows of nine characters each.

export const PX = 2;   // device pixels per bitmap pixel -> 18x18 rendered icon

export const ICONS = {
  media: [
    '....##...',
    '....#.#..',
    '....#..#.',
    '....#..#.',
    '....#....',
    '..###....',
    '.#####...',
    '.#####...',
    '..###....',
  ],
  notes: [
    '...###...',
    '...###...',
    '...###...',
    '...###...',
    '...###...',
    '.#.....#.',
    '..#####..',
    '....#....',
    '..#####..',
  ],
  listen: [
    '....#....',
    '....#....',
    '..#.#.#..',
    '#.#.#.#.#',
    '#.#.#.#.#',
    '#.#.#.#.#',
    '..#.#.#..',
    '....#....',
    '....#....',
  ],
  files: [
    '.........',
    '###......',
    '#..######',
    '#.......#',
    '#.......#',
    '#.......#',
    '#.......#',
    '#########',
    '.........',
  ],
  settings: [
    '..##.....',
    '#########',
    '..##.....',
    '......##.',
    '#########',
    '......##.',
    '...##....',
    '#########',
    '...##....',
  ],
};

// One CSS rule per icon: a PX-sized ::before block whose box-shadow list paints
// every inked pixel. No colour in the list, so each shadow inherits currentColor
// and the icon tints with its nav item's state.
export function iconRules() {
  return Object.entries(ICONS).map(([name, rows]) => {
    const shadows = [];
    rows.forEach((row, y) => {
      [...row].forEach((cell, x) => {
        if (cell === '#') shadows.push(`${x * PX}px ${y * PX}px`);
      });
    });
    return `.k-px-${name}::before{box-shadow:${shadows.join(',')}}`;
  }).join('\n');
}
