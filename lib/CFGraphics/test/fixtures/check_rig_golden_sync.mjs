#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Usage:
//   node check_rig_golden_sync.mjs path/to/cfsprite_pose_golden.json \
//     path/to/cf_gfx_rig_golden.h
//
// The header intentionally uses regular aggregate tables. This script parses
// those tables with regex and compares every authored bone, pose, case, and
// expected transform with the website JSON contract.

import fs from 'node:fs';

const [jsonPath, headerPath] = process.argv.slice(2);
if (!jsonPath || !headerPath) {
  console.error('usage: node check_rig_golden_sync.mjs <golden.json> <golden.h>');
  process.exit(2);
}

const source = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
const header = fs.readFileSync(headerPath, 'utf8');

function table(name) {
  const match = new RegExp(
    `inline\\s+constexpr\\s+[^;]*?\\b${name}\\s*\\[\\s*\\]\\s*=\\s*\\{`).exec(header);
  if (!match) throw new Error(`missing table ${name}`);
  const open = match.index + match[0].lastIndexOf('{');
  const close = matchingBrace(header, open);
  if (close < 0) throw new Error(`unterminated table ${name}`);
  return header.slice(open + 1, close);
}

function matchingBrace(text, open) {
  let depth = 0;
  let quote = null;
  for (let i = open; i < text.length; ++i) {
    const char = text[i];
    const next = text[i + 1];
    if (quote !== null) {
      if (char === '\\') ++i;
      else if (char === quote) quote = null;
    } else if (char === '"' || char === "'") {
      quote = char;
    } else if (char === '/' && next === '/') {
      i = text.indexOf('\n', i + 2);
      if (i < 0) return -1;
    } else if (char === '/' && next === '*') {
      i = text.indexOf('*/', i + 2);
      if (i < 0) return -1;
      ++i;
    } else if (char === '{') {
      ++depth;
    } else if (char === '}' && --depth === 0) {
      return i;
    }
  }
  return -1;
}

function entries(name) {
  const body = table(name);
  const result = [];
  let quote = null;
  for (let i = 0; i < body.length; ++i) {
    const char = body[i];
    const next = body[i + 1];
    if (quote !== null) {
      if (char === '\\') ++i;
      else if (char === quote) quote = null;
      continue;
    }
    if (char === '"' || char === "'") {
      quote = char;
      continue;
    }
    if (char === '/' && next === '/') {
      i = body.indexOf('\n', i + 2);
      if (i < 0) break;
      continue;
    }
    if (char === '/' && next === '*') {
      i = body.indexOf('*/', i + 2);
      if (i < 0) throw new Error(`unterminated comment in table ${name}`);
      ++i;
      continue;
    }
    if (char !== '{') continue;
    const close = matchingBrace(body, i);
    if (close < 0) throw new Error(`unterminated row in table ${name}`);
    result.push(stripComments(body.slice(i + 1, close)).trim());
    i = close;
  }
  if (result.length === 0) throw new Error(`table ${name} has no rows`);
  return result;
}

function stripComments(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
}

function numbers(text) {
  return [...text.matchAll(/-?\d+/g)].map((m) => Number(m[0]));
}

const parentIndex = new Map(source.rig.bones.map((bone, i) => [bone.name, i]));
const actualBones = entries('bones').map((entry) => {
  const name = entry.match(/^"([^"]+)"/)?.[1];
  const values = numbers(entry.slice(entry.indexOf(',') + 1));
  return {name, parent: values[0], len: values[1],
    at: [values[2], values[3]], rest: values[4]};
});
const expectedBones = source.rig.bones.map((bone) => ({
  name: bone.name,
  parent: bone.parent === null ? -1 : parentIndex.get(bone.parent),
  len: bone.len,
  at: bone.at ?? [0, 0],
  rest: bone.rest ?? 0,
}));

for (const [i, bone] of expectedBones.entries()) {
  if (!Number.isInteger(bone.parent) || bone.parent >= i) {
    throw new Error(`bone ${bone.name} must name an earlier parent`);
  }
}
for (const [i, bone] of actualBones.entries()) {
  if (!Number.isInteger(bone.parent) || bone.parent >= i) {
    throw new Error(`header bone ${bone.name} must have parent < ${i}`);
  }
}

const poseNames = Object.keys(source.rig.poses);
const actualPoses = entries('poses').map((entry) => {
  const name = entry.match(/^"([^"]+)"/)?.[1];
  const offsetsOpen = entry.indexOf('{');
  if (offsetsOpen < 0) {
    throw new Error(`pose ${name} is missing its boneDegrees array literal`);
  }
  const offsetsClose = matchingBrace(entry, offsetsOpen);
  const rootValues = numbers(entry.slice(entry.indexOf(',') + 1, offsetsOpen));
  return {name, rootDy: rootValues[0],
    offsets: numbers(entry.slice(offsetsOpen + 1, offsetsClose))};
});
for (const pose of actualPoses) {
  if (pose.offsets.length !== actualBones.length) {
    throw new Error(`pose ${pose.name} has ${pose.offsets.length} bone offsets; ` +
                    `expected ${actualBones.length}`);
  }
}
const expectedPoses = poseNames.map((name) => {
  const pose = source.rig.poses[name];
  return {
    name,
    rootDy: pose.rootDy ?? 0,
    offsets: source.rig.bones.map((bone) => pose[bone.name] ?? 0),
  };
});

const actualCases = entries('cases').map((entry) => {
  const name = entry.match(/^"([^"]+)"/)?.[1];
  const values = numbers(entry.slice(entry.indexOf(',') + 1));
  return {name, poseA: values[0], poseB: values[1], blend: values[2]};
});
const expectedCases = source.cases.map((item) => ({
  name: item.name,
  poseA: poseNames.indexOf(item.pose ?? item.poseA),
  poseB: item.poseB ? poseNames.indexOf(item.poseB) : -1,
  blend: item.blend ?? 0,
}));

const actualExpected = source.cases.map((_, caseIndex) =>
  entries(`case_${caseIndex}_expected`).map((entry) => numbers(entry)));
const expectedExpected = source.cases.map((item) => item.expected.map((bone) => [
  bone.start[0], bone.start[1], bone.end[0], bone.end[1], bone.angle,
]));

const comparisons = [
  ['bones', actualBones, expectedBones],
  ['poses', actualPoses, expectedPoses],
  ['cases', actualCases, expectedCases],
  ['expected transforms', actualExpected, expectedExpected],
];
for (const [label, actual, expected] of comparisons) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    console.error(`${label} differ`);
    console.error('header:', JSON.stringify(actual));
    console.error('json:  ', JSON.stringify(expected));
    process.exit(1);
  }
}

console.log(`rig golden sync OK: ${actualBones.length} bones, ` +
            `${actualPoses.length} poses, ${actualCases.length} cases`);
