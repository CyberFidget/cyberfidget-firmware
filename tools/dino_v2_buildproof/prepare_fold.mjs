// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Reproduce the website's asset fold and stage the standard custom-app pair.

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');
const websiteRoot = path.resolve(
  process.env.CF_WEBSITE_ROOT || path.join(repoRoot, '..', 'cyberfidget_website'),
);
const appSourceDir = path.join(repoRoot, 'wasm', 'apps', 'dino-v2');
const stagedDir = path.join(repoRoot, 'wasm', 'app');
const assetDir = path.join(repoRoot, 'lib', 'DinoGame', 'assets');
const foldModule = path.join(websiteRoot, 'assets', 'js', 'models', 'asset_fold.mjs');
const assetOrder = ['dino', 'cactus', 'pterodactyl', 'ground'];

function normalized(value) {
  return String(value).replace(/\r\n/g, '\n');
}

function fail(message) {
  throw new Error(message);
}

const [publishableHeader, publishableCpp, inTreeHeader, inTreeCpp, demoText] =
  await Promise.all([
    readFile(path.join(appSourceDir, 'DinoGame.h'), 'utf8'),
    readFile(path.join(appSourceDir, 'DinoGame.cpp'), 'utf8'),
    readFile(path.join(repoRoot, 'lib', 'DinoGame', 'DinoGame.h'), 'utf8'),
    readFile(path.join(repoRoot, 'lib', 'DinoGame', 'DinoGame.cpp'), 'utf8'),
    readFile(path.join(appSourceDir, 'demo.json'), 'utf8'),
  ]);

const expectedHeader = normalized(inTreeHeader).replace(
  /^#include "generated\/(?:cactus|dino|ground|pterodactyl)\.h"\n/gm,
  '',
);
if (normalized(publishableHeader) !== expectedHeader) {
  fail('DinoGame.h drifted: it must differ from lib/DinoGame only by generated includes');
}
if (normalized(publishableCpp) !== normalized(inTreeCpp)) {
  fail('DinoGame.cpp drifted from lib/DinoGame/DinoGame.cpp');
}

const demo = JSON.parse(demoText);
if (demo.hCode !== publishableHeader || demo.cppCode !== publishableCpp) {
  fail('demo.json hCode/cppCode do not exactly match the unfolded source pair');
}

const assets = await Promise.all(assetOrder.map(async (name) => ({
  path: `assets/${name}.cfsprite.json`,
  data: JSON.parse(await readFile(path.join(assetDir, `${name}.cfsprite.json`), 'utf8')),
})));
const { foldAssetsIntoHeader } = await import(pathToFileURL(foldModule).href);
const folded = foldAssetsIntoHeader(publishableHeader, assets);
const rawBytes = Buffer.byteLength(folded.header, 'utf8');
const base64Bytes = 4 * Math.ceil(rawBytes / 3);

await mkdir(stagedDir, { recursive: true });
await Promise.all([
  writeFile(path.join(stagedDir, 'DinoGame.h'), folded.header, 'utf8'),
  writeFile(path.join(stagedDir, 'DinoGame.cpp'), publishableCpp, 'utf8'),
  writeFile(
    path.join(stagedDir, 'app_include.h'),
    '#include "App.h"\n#include "DinoGame.h"\n',
    'utf8',
  ),
]);

console.log(`Asset order: ${assetOrder.join(', ')}`);
console.log(`Folded header: ${rawBytes} raw UTF-8 bytes`);
console.log(`Folded header: ${base64Bytes} base64 payload bytes / ${100 * 1024} byte cap`);
for (const note of folded.notes) console.log(`Fold note: ${note}`);
