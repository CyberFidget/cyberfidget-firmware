// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic framebuffer comparator for the DinoGame cf::gfx migration.

import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const WIDTH = 128;
const HEIGHT = 64;
// The WASM SSD1306 shim is semantically 1bpp but stores each pixel as one
// uint8_t (0 or 1), matching the website emulator's writeFramebuffer contract.
const FRAMEBUFFER_BYTES = WIDTH * HEIGHT;
const JUMP_BUTTON = 2;
const DUCK_BUTTON = 3;

function usage(message) {
  if (message) console.error(`error: ${message}\n`);
  console.error('usage: node dino_parity.mjs --pre DIR --post DIR [--out DIR] [--seed N] [--frames 500] [--trace scripted|none]');
  process.exit(message ? 2 : 0);
}

function parseInteger(text, flag) {
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value < 0 || value > 0xffffffff) {
    usage(`${flag} must be an integer from 0 through 0xffffffff`);
  }
  return value;
}

function parseArgs(argv) {
  const options = {
    out: path.resolve('tools/dino_parity/out'),
    seed: 0x00c0ffee,
    frames: 500,
    trace: 'scripted',
  };
  for (let i = 0; i < argv.length; i += 1) {
    const flag = argv[i];
    if (flag === '--help' || flag === '-h') usage();
    if (!flag.startsWith('--')) usage(`unexpected argument ${flag}`);
    const value = argv[++i];
    if (value === undefined) usage(`${flag} requires a value`);
    if (flag === '--pre' || flag === '--post' || flag === '--out') {
      options[flag.slice(2)] = path.resolve(value);
    } else if (flag === '--seed') {
      options.seed = parseInteger(value, flag);
    } else if (flag === '--frames') {
      options.frames = parseInteger(value, flag);
      if (options.frames === 0) usage('--frames must be positive');
    } else if (flag === '--trace') {
      if (!['scripted', 'none'].includes(value)) usage('--trace must be scripted or none');
      options.trace = value;
    } else {
      usage(`unknown option ${flag}`);
    }
  }
  if (!options.pre || !options.post) usage('--pre and --post are required');
  return options;
}

function scriptedActions(frame) {
  // Frame indices are zero-based. A one-frame jump tap is pressed at 50 and
  // released at 51. Duck is down for frames 200..229 and released at 230.
  const actions = [];
  if (frame === 50 || frame === 400) actions.push(['press', JUMP_BUTTON]);
  if (frame === 51 || frame === 401) actions.push(['release', JUMP_BUTTON]);
  if (frame === 200) actions.push(['press', DUCK_BUTTON]);
  if (frame === 230) actions.push(['release', DUCK_BUTTON]);
  return actions;
}

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest();
}

function toPbm(framebuffer) {
  const raster = Buffer.alloc(WIDTH * HEIGHT / 8);
  for (let y = 0; y < HEIGHT; y += 1) {
    for (let x = 0; x < WIDTH; x += 1) {
      const lit = framebuffer[y * WIDTH + x] !== 0;
      if (lit) raster[y * (WIDTH / 8) + Math.floor(x / 8)] |= 0x80 >> (x % 8);
    }
  }
  return Buffer.concat([Buffer.from(`P4\n${WIDTH} ${HEIGHT}\n`, 'ascii'), raster]);
}

async function locateBuild(buildDir) {
  const js = path.join(buildDir, 'cyberfidget.js');
  const wasm = path.join(buildDir, 'cyberfidget.wasm');
  await Promise.all([readFile(js), readFile(wasm)]).catch((error) => {
    throw new Error(`build directory ${buildDir} must contain cyberfidget.js and cyberfidget.wasm: ${error.message}`);
  });
  return { js, wasm };
}

async function runBuild(label, buildDir, options) {
  const { js } = await locateBuild(buildDir);
  const require = createRequire(import.meta.url);
  const resolvedLoader = require.resolve(js);
  delete require.cache[resolvedLoader];
  const factory = require(resolvedLoader);
  if (typeof factory !== 'function') throw new Error(`${js} did not export an Emscripten module factory`);

  let pushedFrames = 0;
  const serial = [];
  const module = await factory({
    print: () => {},
    printErr: (text) => serial.push(String(text)),
    onSerialOutput: (text) => serial.push(String(text)),
    onFrameReady: () => { pushedFrames += 1; },
  });

  const required = [
    '_wasm_dino_parity_reset', '_wasm_dino_parity_step',
    '_wasm_dino_parity_game_over', '_wasm_get_framebuffer',
    '_wasm_get_framebuffer_size', '_wasm_button_press',
    '_wasm_button_release',
  ];
  for (const name of required) {
    if (typeof module[name] !== 'function') throw new Error(`${label} build lacks parity export ${name}`);
  }

  const size = module._wasm_get_framebuffer_size();
  if (size !== FRAMEBUFFER_BYTES) {
    throw new Error(`${label} framebuffer is ${size} bytes; expected ${FRAMEBUFFER_BYTES}`);
  }

  module._wasm_dino_parity_reset(options.seed);
  const frames = [];
  const records = [];
  let chain = Buffer.alloc(32);
  let previousGameOver = false;

  for (let frame = 0; frame < options.frames; frame += 1) {
    const actions = options.trace === 'scripted' ? scriptedActions(frame) : [];
    for (const [kind, button] of actions) {
      module[kind === 'press' ? '_wasm_button_press' : '_wasm_button_release'](button);
    }

    const pushesBefore = pushedFrames;
    const collisionEvent = module._wasm_dino_parity_step() !== 0;
    const gameOver = module._wasm_dino_parity_game_over() !== 0;
    const ptr = module._wasm_get_framebuffer();
    if (!ptr) throw new Error(`${label} returned a null framebuffer at frame ${frame}`);
    const framebuffer = Buffer.from(module.HEAPU8.slice(ptr, ptr + size));
    const index = Buffer.alloc(4);
    index.writeUInt32BE(frame);
    const frameHash = sha256(framebuffer);
    chain = sha256(Buffer.concat([chain, index, framebuffer]));
    const pushes = pushedFrames - pushesBefore;

    if (!previousGameOver && pushes !== 1) {
      throw new Error(`${label} active frame ${frame} pushed ${pushes} framebuffers; expected exactly 1`);
    }
    if (collisionEvent !== (!previousGameOver && gameOver)) {
      throw new Error(`${label} collision-event hook disagrees with game-over transition at frame ${frame}`);
    }

    frames.push(framebuffer);
    records.push({
      frame,
      actions: actions.map(([kind, button]) => ({ kind, button })),
      frameSha256: frameHash.toString('hex'),
      chainSha256: chain.toString('hex'),
      collisionEvent,
      gameOver,
      framebufferPushes: pushes,
    });
    previousGameOver = gameOver;
  }

  return {
    label,
    buildDir,
    frames,
    records,
    finalChainSha256: chain.toString('hex'),
    collisionFrames: records.filter((record) => record.collisionEvent).map((record) => record.frame),
    serial,
  };
}

async function writeJsonLines(file, records) {
  const body = records.map((record) => JSON.stringify(record)).join('\n') + '\n';
  await writeFile(file, body);
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  await mkdir(options.out, { recursive: true });

  const pre = await runBuild('pre', options.pre, options);
  const post = await runBuild('post', options.post, options);
  const mismatches = [];
  for (let frame = 0; frame < options.frames; frame += 1) {
    if (!pre.frames[frame].equals(post.frames[frame])) mismatches.push(frame);
  }

  await Promise.all([
    writeJsonLines(path.join(options.out, 'pre-hashes.jsonl'), pre.records),
    writeJsonLines(path.join(options.out, 'post-hashes.jsonl'), post.records),
  ]);

  const firstDivergence = mismatches.length ? mismatches[0] : null;
  const dumpedFrames = [];
  if (firstDivergence !== null) {
    const dumpDir = path.join(options.out, 'mismatch');
    await mkdir(dumpDir, { recursive: true });
    for (let frame = Math.max(0, firstDivergence - 2); frame <= firstDivergence; frame += 1) {
      const stem = `frame_${String(frame).padStart(4, '0')}`;
      await Promise.all([
        writeFile(path.join(dumpDir, `${stem}_pre.pbm`), toPbm(pre.frames[frame])),
        writeFile(path.join(dumpDir, `${stem}_post.pbm`), toPbm(post.frames[frame])),
      ]);
      dumpedFrames.push(frame);
    }
  }

  const summary = {
    width: WIDTH,
    height: HEIGHT,
    framebufferFormat: 'WASM shim row-major 0/1 bytes, semantically 1bpp, 8192 bytes',
    frameIndexing: 'zero-based',
    seed: options.seed,
    seedHex: `0x${options.seed.toString(16).padStart(8, '0')}`,
    trace: options.trace,
    framesCompared: options.frames,
    identicalFrames: options.frames - mismatches.length,
    mismatchingFrames: mismatches.length,
    firstDivergence,
    dumpedFrames,
    pre: {
      buildDir: pre.buildDir,
      finalChainSha256: pre.finalChainSha256,
      collisionFrames: pre.collisionFrames,
      serialLines: pre.serial,
    },
    post: {
      buildDir: post.buildDir,
      finalChainSha256: post.finalChainSha256,
      collisionFrames: post.collisionFrames,
      serialLines: post.serial,
    },
    firstDivergenceCollisionEvidence: firstDivergence === null ? null : {
      preCollisionEvent: pre.records[firstDivergence].collisionEvent,
      postCollisionEvent: post.records[firstDivergence].collisionEvent,
      preGameOver: pre.records[firstDivergence].gameOver,
      postGameOver: post.records[firstDivergence].gameOver,
    },
  };
  await writeFile(path.join(options.out, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);

  console.log(JSON.stringify(summary, null, 2));
  process.exitCode = mismatches.length ? 1 : 0;
}

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exitCode = 2;
});
