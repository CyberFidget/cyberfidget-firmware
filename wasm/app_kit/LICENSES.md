<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->
# App kit licences

The firmware-derived sources, prelude, graphics objects, emulator core,
interface table and examples in this kit are distributed
under GPL-3.0-or-later WITH Cyberfidget-HAL-exception. See the included LICENSE
for the full licence and linking exception. Generated app modules may carry
their own licence under that exception. The manifest and recipe describe
these firmware-derived build inputs.

Compiler binaries and resources are **not bundled**. Fetch the exact
@yowasp/clang version recorded in manifest.json separately, then verify every
decoded file in its gen/ directory against manifest.compiler.files.
LLVM/Clang/LLD use Apache-2.0 WITH LLVM-exception; wasi-libc includes
MIT/Apache-2.0 components and their notices; YoWASP's adapter uses ISC.
Retain the compiler distribution's own licence notices when redistributing
it. These licences do not become the firmware's licence.
