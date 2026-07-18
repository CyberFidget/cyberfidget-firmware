// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

import { access, mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ENV_NAME = 'CF_CFSPRITE_COMPILER';
const repoRoot = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const fallbackCompiler = path.resolve(
  repoRoot, '..', 'cyberfidget_website', 'assets', 'js', 'cfsprite_compile.mjs',
);

function usage() {
  return 'Usage: node tools/cfsprite_compile_cli.mjs [--check-only] [--symbol-prefix NAME] INPUT OUTPUT';
}

async function exists(filename) {
  try {
    await access(filename);
    return true;
  } catch {
    return false;
  }
}

async function resolveCompiler() {
  const configured = process.env[ENV_NAME];
  const configuredPath = configured
    ? (path.isAbsolute(configured) ? configured : path.resolve(repoRoot, configured))
    : null;
  if (configuredPath && await exists(configuredPath)) return configuredPath;
  if (await exists(fallbackCompiler)) return fallbackCompiler;

  const configuredDetail = configuredPath
    ? `Configured ${ENV_NAME} path was not found: ${configuredPath}\n`
    : '';
  throw new Error(
    `${configuredDetail}Could not find the .cfsprite compiler at ${fallbackCompiler}.\n`
    + `Set ${ENV_NAME} to the compiler module path.`,
  );
}

function parseArgs(argv) {
  let checkOnly = false;
  let symbolPrefix;
  const positional = [];
  for (let index = 0; index < argv.length; ++index) {
    const arg = argv[index];
    if (arg === '--check-only' || arg === '--check') {
      checkOnly = true;
    } else if (arg === '--symbol-prefix') {
      symbolPrefix = argv[++index];
      if (!symbolPrefix) throw new Error('--symbol-prefix requires a value');
    } else if (arg === '--help' || arg === '-h') {
      console.log(usage());
      process.exit(0);
    } else if (arg.startsWith('-')) {
      throw new Error(`Unknown option: ${arg}`);
    } else {
      positional.push(arg);
    }
  }
  if (positional.length !== 2) throw new Error(usage());
  return { checkOnly, symbolPrefix, input: positional[0], output: positional[1] };
}

function withLicenseHeader(header) {
  if (header.startsWith('// SPDX-License-Identifier:')) return header;
  return '// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception\n'
    + '// Copyright (c) 2023-2026 Dismo Industries LLC\n\n'
    + header;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const compilerPath = await resolveCompiler();
  const compiler = await import(pathToFileURL(compilerPath).href);
  if (typeof compiler.compile !== 'function') {
    throw new Error(`Compiler module does not export compile(): ${compilerPath}`);
  }

  const inputPath = path.resolve(repoRoot, args.input);
  const outputPath = path.resolve(repoRoot, args.output);
  const json = JSON.parse(await readFile(inputPath, 'utf8'));
  const result = compiler.compile(json, { symbolPrefix: args.symbolPrefix });
  const header = withLicenseHeader(result.header);
  for (const warning of result.warnings ?? []) console.warn(`warning: ${warning}`);

  if (args.checkOnly) {
    const existing = await readFile(outputPath, 'utf8').catch(() => null);
    if (existing !== header) {
      throw new Error(
        `Generated header drift: ${path.relative(repoRoot, outputPath)}\n`
        + 'Regenerate target: pio run -t cfsprite-regen',
      );
    }
    console.log(`Checked ${path.relative(repoRoot, outputPath)}`);
    return;
  }

  await mkdir(path.dirname(outputPath), { recursive: true });
  await writeFile(outputPath, header, 'utf8');
  console.log(`Generated ${path.relative(repoRoot, outputPath)}`);
}

main().catch((error) => {
  console.error(`cfsprite_compile_cli: ${error.message}`);
  process.exitCode = 1;
});
