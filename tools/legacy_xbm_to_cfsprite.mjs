// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Provenance converter for DinoGame's legacy XBM assets. The pixel data is
// parsed from DinoSprites.h rather than copied into this script.

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const legacyPath = path.join(repoRoot, 'lib', 'DinoGame', 'DinoSprites.h');
const assetsDir = path.join(repoRoot, 'lib', 'DinoGame', 'assets');

const AUTHOR = 'Dismo Industries';
const LICENSE = 'GPL-3.0-or-later';

function parseArrays(source) {
  const arrays = new Map();
  const pattern = /const\s+unsigned\s+char\s+(\w+)\[\]\s+PROGMEM\s*=\s*\{([\s\S]*?)\};/g;
  for (const match of source.matchAll(pattern)) {
    const bytes = [...match[2].matchAll(/0x([0-9a-f]{1,2})/gi)]
      .map((entry) => Number.parseInt(entry[1], 16));
    arrays.set(match[1], Uint8Array.from(bytes));
  }
  return arrays;
}

function unpackXbm(bytes, width, height) {
  const stride = Math.ceil(width / 8);
  if (bytes.length !== stride * height) {
    throw new Error(`Expected ${stride * height} bytes for ${width}x${height}, found ${bytes.length}`);
  }
  const pixels = new Uint8Array(width * height);
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      pixels[y * width + x] = (bytes[y * stride + (x >> 3)] >> (x & 7)) & 1;
    }
  }
  return pixels;
}

function packProfileBits(pixels) {
  const bytes = new Uint8Array(Math.ceil(pixels.length / 8));
  for (let index = 0; index < pixels.length; ++index) {
    if (pixels[index]) bytes[index >> 3] |= 1 << (index & 7);
  }
  return Buffer.from(bytes).toString('base64');
}

function canvasFrame(arrays, {
  source,
  sourceWidth,
  sourceHeight,
  canvasWidth = sourceWidth,
  canvasHeight = sourceHeight,
  offsetX = 0,
  offsetY = 0,
  name,
}) {
  const bytes = arrays.get(source);
  if (!bytes) throw new Error(`Legacy array ${source} was not found in ${legacyPath}`);
  const sourcePixels = unpackXbm(bytes, sourceWidth, sourceHeight);
  const canvas = new Uint8Array(canvasWidth * canvasHeight);
  for (let y = 0; y < sourceHeight; ++y) {
    for (let x = 0; x < sourceWidth; ++x) {
      const targetX = x + offsetX;
      const targetY = y + offsetY;
      if (targetX < 0 || targetX >= canvasWidth || targetY < 0 || targetY >= canvasHeight) {
        throw new Error(`${source} does not fit its ${canvasWidth}x${canvasHeight} canvas`);
      }
      canvas[targetY * canvasWidth + targetX] = sourcePixels[y * sourceWidth + x];
    }
  }
  return {
    name,
    durationMs: 0,
    layers: [{
      name: source,
      x: 0,
      y: 0,
      visible: true,
      bits: packProfileBits(canvas),
    }],
  };
}

function asset(name, kind, width, height, frames, animations) {
  return {
    schemaVersion: 1,
    meta: {
      name,
      author: AUTHOR,
      license: LICENSE,
      kind,
      size: { w: width, h: height },
      pivot: { x: Math.floor(width / 2), y: height },
    },
    frames,
    animations,
  };
}

async function writeAsset(filename, value) {
  await writeFile(path.join(assetsDir, filename), `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

const source = await readFile(legacyPath, 'utf8');
const arrays = parseArrays(source);
await mkdir(assetsDir, { recursive: true });

const dinoFrames = [
  canvasFrame(arrays, {
    source: 'Dino_Stand_16x16', sourceWidth: 16, sourceHeight: 16,
    canvasWidth: 16, canvasHeight: 16, name: 'Stand',
  }),
  canvasFrame(arrays, {
    source: 'Dino_Duck_16x8', sourceWidth: 16, sourceHeight: 8,
    canvasWidth: 16, canvasHeight: 16, offsetY: 8, name: 'Duck',
  }),
];
await writeAsset('dino.cfsprite.json', asset('Dino', 'character', 16, 16, dinoFrames, {
  run: { frames: [0], loop: true },
  duck: { frames: [1], loop: true },
  jump: { frames: [0], loop: true },
}));

await writeAsset('cactus.cfsprite.json', asset('Cactus', 'sprite', 8, 16, [
  canvasFrame(arrays, {
    source: 'Cactus_8x16', sourceWidth: 8, sourceHeight: 16, name: 'Cactus',
  }),
], { idle: { frames: [0], loop: true } }));

await writeAsset('pterodactyl.cfsprite.json', asset('Pterodactyl', 'sprite', 16, 16, [
  canvasFrame(arrays, {
    source: 'Pterodactyl_16x16', sourceWidth: 16, sourceHeight: 16, name: 'Fly',
  }),
], { fly: { frames: [0], loop: true } }));

const groundFrames = ['A', 'B', 'C', 'D'].map((suffix) => canvasFrame(arrays, {
  source: `GroundTile${suffix}_16x8`,
  sourceWidth: 16,
  sourceHeight: 8,
  name: `Tile ${suffix}`,
}));
await writeAsset('ground.cfsprite.json', asset('Ground', 'tile', 16, 8, groundFrames, {
  tile_a: { frames: [0], loop: true },
  tile_b: { frames: [1], loop: true },
  tile_c: { frames: [2], loop: true },
  tile_d: { frames: [3], loop: true },
}));

console.log(`Generated 4 DinoGame assets in ${path.relative(repoRoot, assetsDir)}`);
