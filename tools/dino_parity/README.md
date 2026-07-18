# DinoGame migration parity proof

This development-only tool loads two parity-enabled Emscripten builds, resets
both with the same 32-bit seed, applies the same input events, manually steps
the firmware at exactly 20 ms per frame, and compares all 8192 framebuffer
bytes after every step.

The emulator surface is semantically 1 bpp but is not device-packed: its
`SSD1306Wire` shim stores one `uint8_t` (0 or 1) per row-major pixel, and the
website's `writeFramebuffer` consumes exactly that 8192-byte form.

The default scripted trace uses zero-based frame numbers:

- frame 50: jump press; frame 51: jump release
- frame 200: duck press; frame 230: duck release (30 stepped frames down)
- frame 400: jump press; frame 401: jump release

`--trace none` applies no button events. The analog input is left at the WASM
shim's zero-initialized value on both builds.

## Build

Parity hooks are opt-in (`CF_DINO_PARITY=ON`) and CMake rejects using them with
anything except `WASM_APP=DinoGame`. Generate the version header before each
direct CMake build, as required by the repository WASM profile:

```powershell
$env:CYBERFIDGET_BUILD_TYPE_OVERRIDE = 'wasm'
python scripts/generate_version.py --standalone --out wasm/generated/version.h --repo-root .
$env:CYBERFIDGET_BUILD_TYPE_OVERRIDE = $null

cmd /c "call ..\emsdk\emsdk_env.bat && emcmake cmake -S wasm -B tools\dino_parity\out\builds\post -G Ninja -DCMAKE_BUILD_TYPE=Release -DWASM_APP=DinoGame -DCF_DINO_PARITY=ON && emmake ninja -C tools\dino_parity\out\builds\post"
```

Build the `6313aec` worktree with the same command and its own output directory.
That revision needs the same opt-in CMake, synthetic-clock, manual-step, reset,
and game-over read hooks applied locally. It also needs the additive
`resetGame(uint32_t seed)` overload because the overload first appears in the
migration commit. Do not commit that proof patch.

## Compare

```powershell
node tools/dino_parity/dino_parity.mjs `
  --pre tools\dino_parity\out\builds\pre `
  --post tools\dino_parity\out\builds\post `
  --frames 500 `
  --seed 0x00c0ffee `
  --trace scripted `
  --out tools/dino_parity/out
```

The process exits 0 only for exact parity, 1 for an honest framebuffer
divergence, and 2 for a harness/build error. Output includes:

- `pre-hashes.jsonl` and `post-hashes.jsonl`: per-frame SHA-256, chained
  SHA-256, input actions, framebuffer push count, collision event, and game-over
  state. The chain is `SHA256(previousChain || uint32be(frame) || framebuffer)`,
  beginning with 32 zero bytes.
- `summary.json`: counts, final chains, first divergence, and collision evidence.
- `mismatch/*.pbm`: binary PBM dumps for the first divergent frame and its two
  predecessors, for both builds. These are written only when a mismatch exists.

The PBM converter packs the emulator's row-major 0/1 bytes into PBM's
MSB-horizontal raster. Frame equality itself always compares the original 8192
bytes, not the converted images or hashes.
