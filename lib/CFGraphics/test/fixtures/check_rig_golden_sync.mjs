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
  const match = header.match(new RegExp(
    `inline constexpr [^\\n]+ ${name}\\[\\] = \\{([\\s\\S]*?)\\n\\};`));
  if (!match) throw new Error(`missing table ${name}`);
  return match[1];
}

function entries(body) {
  return [...body.matchAll(/^\s*\{(.+)\},?\s*$/gm)].map((m) => m[1]);
}

function numbers(text) {
  return [...text.matchAll(/-?\d+/g)].map((m) => Number(m[0]));
}

const parentIndex = new Map(source.rig.bones.map((bone, i) => [bone.name, i]));
const actualBones = entries(table('bones')).map((entry) => {
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

const poseNames = Object.keys(source.rig.poses);
const actualPoses = entries(table('poses')).map((entry) => {
  const name = entry.match(/^"([^"]+)"/)?.[1];
  const values = numbers(entry.slice(entry.indexOf(',') + 1));
  return {name, rootDy: values[0], offsets: values.slice(1)};
});
const expectedPoses = poseNames.map((name) => {
  const pose = source.rig.poses[name];
  return {
    name,
    rootDy: pose.rootDy ?? 0,
    offsets: source.rig.bones.map((bone) => pose[bone.name] ?? 0),
  };
});

const actualCases = entries(table('cases')).map((entry) => {
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
  entries(table(`case_${caseIndex}_expected`)).map((entry) => numbers(entry)));
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
