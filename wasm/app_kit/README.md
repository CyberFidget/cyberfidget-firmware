<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->
# Firmware app kit

Every firmware release supplies its own app compilation and emulation inputs.
A consumer can follow releases without embedding firmware headers or compiler
flags in the website. This tool uses Node ES modules, the exactly pinned
YoWASP compiler, Node 22 and, when building the core, an activated Emscripten SDK,
CMake and Ninja. It uses no ports. Once dependencies and the SDK are present,
builds use no network. The compiler is fetched separately, not shipped in the kit.

From the firmware root, after `npm ci --prefix wasm/app_kit`:

```sh
mkdir -p /path/to/scratch
export TEMP=/path/to/scratch TMP=/path/to/scratch TMPDIR=/path/to/scratch EMCC_TEMP_DIR=/path/to/scratch
source /path/to/emsdk/emsdk_env.sh
node wasm/app_kit/build_app_kit.mjs --out /path/to/output --tag v1.4.2
node wasm/app_kit/build_app_kit.mjs --check /path/to/output/cf-app-kit
node wasm/app_kit/smoke_kit.mjs /path/to/output/cf-app-kit --out /path/to/scratch/modules
export PATH="$EMSDK/upstream/bin:$PATH"
bash wasm/device_module/verify_device_contract.sh /path/to/scratch/modules/StopwatchDemo.wasm
bash wasm/device_module/verify_device_contract.sh /path/to/scratch/modules/BreakoutGame.wasm
```

On Windows, set the four environment variables to a writable scratch path in
PowerShell (`$env:TEMP='D:/path/to/scratch'`, etc.), activate `emsdk_env.bat`
in the invoking environment and place CMake/Ninja on PATH. Node is on PATH.
Python is needed by Emscripten itself, but this builder does not call the
firmware's Python version generator. All core intermediates go under the
scratch directory and are removed after the build. Nothing writes to the SDK
except its normal toolchain cache; prewarm that cache if the SDK is read-only.

`--out` must name an output parent without an existing `cf-app-kit` directory.
`--tag` is optional (null in the manifest when omitted); when supplied it must
match version.txt: `vMAJOR.MINOR.PATCH` with an optional `-[A-Za-z0-9.-]+`
prerelease suffix. The release plan rejects other tags before building.
`--core <dir>`
copies an already-built `cyberfidget-core.js`, `cyberfidget-core.wasm` and
`cf-imports.json` instead of invoking CMake. They must come from this exact
checkout with the same tag and toolchain. The caller is responsible for their
provenance; a hash cannot prove which source built an externally supplied core.
See [the module-host build](../module_host/README.md). The default path runs
that target via `cmake -DCMAKE_TOOLCHAIN_FILE=<emsdk>/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`
(without the emcmake shell wrapper) and supplies a private version header using the commit timestamp,
so dirty-tree timestamps do not make a kit build nondeterministic. The kit's
version is the tagged/base version plus `git rev-parse --short=7 HEAD`, matching
the firmware version generator (Git may lengthen it to disambiguate commits).
Build release kits from clean commits; working-tree edits change kit inputs.

`--check <kit-dir>` rebuilds the inputs in memory and compares every byte, the
exact file inventory, the sibling zip and the sibling sidecar manifest. It
inherits the recorded tag when `--tag` is omitted. Supply the same `--core`
when checking a kit that copied a core. Without it, the core is rebuilt too.
For fixed sources, tag, compiler/core toolchain and Node/zlib version, entries
are sorted, timestamps are 1980-01-01, permissions are regular-file 0644 and
compression is raw DEFLATE level 9. No host paths or dates enter the archive.
Firmware source text, fixtures and licence text use LF regardless of Git's
checkout line-ending settings. The core and `cf-imports.json` are read raw.
Release kits are built on Linux CI; cross-OS byte identity is not promised.
The consumer contract is the manifest's per-file hashes, not the zip hash:
zip bytes depend on Node's bundled zlib. Keep Node/zlib fixed for archive comparisons.

## Files and manifest

```text
output/
  cf-app-kit.zip
  cf-app-kit.manifest.json       exact copy of cf-app-kit/manifest.json
  cf-app-kit/
    manifest.json
    recipe.json                 compiler instructions as data
    sources.json                flat virtual path -> source text
    prelude.pch
    cf_gfx_sprite.o
    cf_gfx_actor.o
    cf_gfx_collision.o
    cyberfidget-core.js
    cyberfidget-core.wasm
    cf-imports.json              generated import names/signatures/policies
    examples.json               Stopwatch and folded Breakout payloads
    LICENSE
    LICENSES.md
```

Manifest format 1 has the following shape (hashes abbreviated here only):

```json
{
  "kit_format": 1,
  "version": "1.4.2+824cf93",
  "tag": "v1.4.2",
  "commit": "824cf933faa60e978bb44085bc47c4792e185f37",
  "hal_abi": 1,
  "compiler": {
    "package": "@yowasp/clang",
    "version": "22.0.0-git20542-10",
    "files": { "bundle.js": { "sha256": "...", "size": 253926 } }
  },
  "files": { "recipe.json": { "sha256": "...", "size": 1234 } }
}
```

`compiler.files` covers **every decoded file** in the package's `gen/`
directory, including the resource archive and all compiler wasm segments.
`files` covers every other kit file except manifest.json itself (a manifest
cannot hash itself). Sizes are bytes; hashes are lowercase SHA-256 hex.
The sidecar is identical to the internal manifest.

## Recipe format 1

`driver`, `compile_flags`, `defines` and `link_flags` are ordered arguments.
`identifier_pattern` is `^[A-Za-z_][A-Za-z0-9_]*$`; app name and instance must
match it before any substitution. `force_include` identifies the hosted header.
`prelude` names the single wrapper, expanded header and PCH, with `expand`
and `build` steps.
`graphics` specifies the three source/object pairs and their checkout paths;
the actual source text is in sources.json. `graphics_build` is their command
template. `layout` names the app header/source and shim/interface/graphics
directories. Every step (`prelude.expand`, `prelude.build`, `graphics_build`,
`commands.glue`, `.app`, `.link`) has ordered `args` and `inputs` arrays.
Each command is `[driver, ...compile_flags, ...substituted_step.args]`.
`inputs` selects exactly the virtual files visible to that invocation:
`sources` is the source tree (including the two app files for app compilation),
`prelude` supplies `prelude.pch`, and object names supply those object bytes.
`{objects}` expands to the ordered graphics object names. Prelude steps see
only sources; graphics/glue/app compilation sees sources plus PCH; linking
sees only `glue.o`, `app.o` and the three graphics objects.

```json
{"inputs": ["sources", "prelude"], "args": ["{defines}", "-include-pch", "{prelude_output}", "-c", "{app_source}", "-o", "app.o"]}
```

For example, `-DCF_CUSTOM_APP_HEADER="{name}.h"` becomes
`-DCF_CUSTOM_APP_HEADER="StopwatchDemo.h"`. Scalar tokens are `name`, `instance`,
`app_source`, `prelude_source`, `prelude_expanded`, `prelude_output`,
`graphics_source` and `graphics_object`. Whole-argument tokens `{defines}`,
`{objects}` and `{link_flags}` splice ordered arrays. App identifiers must
match the recipe's `identifier_pattern`. Sources.json is expanded into nested virtual
directories without rewriting paths. Install the app's two strings at
`layout.header` and `layout.source`, and the PCH at `prelude.output`. Compile
glue and app independently, then link only those objects plus the graphics
objects. The repository's [compile_recipe.mjs](compile_recipe.mjs) demonstrates
consumption without hard-coded compiler or linker flags; it is not bundled.

The prelude includes only the existing force-include header. It is first
expanded using `-E -dD`, preserving macros and line markers, then precompiled
as a single input. This avoids the pinned WASI adapter's file-descriptor limit
when an app parses more standard headers. The current force-include already
includes functional, vector and algorithm; no additional implicit includes
are invented here. The expanded text stays in sources.json beside the PCH.

## Verification and consumers

Obtain the manifest through a trusted release channel. Before parsing sources,
loading JavaScript or WebAssembly, or using the PCH and objects, hash **every
file** against its manifest SHA-256 and size. Reject unknown kit/recipe formats
and unsafe paths; confirm the sidecar/internal manifests agree. Fetch the
recorded compiler package separately and verify all decoded `gen/` files
against `compiler.files` **before importing its code**. Do not treat the
manifest as a signature or use an untrusted manifest to authenticate itself.
Keep the core, import table and compile inputs from the same kit. Before
attaching a built guest, a consumer checks imports against `cf-imports.json`
with its own checker. Repository tools (the recipe consumer, import checker
and host adapter) are not shipped in the kit.

**Trust:** Executing kit code (the emulator core) runs with the consumer's
privileges. A same-release manifest proves self-consistency, not provenance.
Consumers should isolate executing kit code; the website plans a separate-origin
sandbox.

The smoke script verifies hashes first, compiles both bundled examples twice,
compares the resulting bytes, verifies imports and runs each for 300 frames
with real-time pacing, scripted buttons, slider and motion. Every frame must
be nonblank and the framebuffer must change. It reports warm compile timings.
Node mocks audio; this test does not establish audible or hardware parity.
`--out` retains both guest modules for the existing shell contract checker.
An optional `--reference /path/to/compiler-options.mjs` compares Stopwatch
with the website's compileApp using identical kit inputs. The supplied
snapshot omits its diagnostics module; only that unused-success-path import
is replaced in memory, leaving the reference compile/link logic intact.

CI builds, checks and smokes the kit, checks both device contracts, and uploads
the zip and sidecar as the `cf-app-kit` artifact. Release builds do the same
build/smoke preparation with the release tag and attach both files to the
existing release. Determinism checks run in WASM CI rather than repeating the
core build on the release path. Kit failures block publishing, and both assets
must exist before push/release creation. A dispatch may explicitly set
`skip_app_kit` (default false): all kit steps are skipped with a warning that
the website will not follow this release. Tag-push releases always require
the kit. Rehearsals build it by default and preserve the existing publish guards.
