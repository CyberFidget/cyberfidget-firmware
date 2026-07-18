# Dino v2 two-file build proof

`buildproof.ps1` verifies that the publishable pair is a mechanical derivative
of `lib/DinoGame`, checks all four website-compiled sprite headers against the
committed symbol contract, reproduces the website fold in the fixed order
`dino`, `cactus`, `pterodactyl`, `ground`, and stages the folded pair in
`wasm/app/`.

It then runs the same version generation, custom-app CMake profile, device
builder, and device contract checker used by `.github/workflows/compile-wasm.yml`.
Outputs stay under ignored `wasm/build/dino-v2-buildproof/`.

Requirements: Node.js, Python, CMake, Ninja, Emscripten 3.1.51, Git Bash on
Windows, and the sibling website checkout containing
`assets/js/models/asset_fold.mjs`.

```powershell
powershell -File tools/dino_v2_buildproof/buildproof.ps1
```

Use `-WebsiteRoot` and `-EmsdkRoot` when those sibling defaults do not apply.
The proof uses the workflow scripts and fails on any profile or contract
mismatch. It checks the emulator profile's browser exports separately from the
device profile's `app_*` exports, imports, memory, and HAL ABI contract.
