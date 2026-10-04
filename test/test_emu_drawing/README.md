<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->

# Emulator drawing parity

The oracle contains the actual drawing method bodies from the supplied
ThingPulse `OLEDDisplay.cpp`, with a minimal 128x64 packed-buffer harness.
The MIT notice is retained in the oracle and both production headers that
incorporate its algorithms. Scheduling and diagnostic output are stubbed only
in the oracle; they do not change rasterization. The shim retains its existing
8192-byte row-major browser interface and now rasterizes into a 1024-byte
SSD1306 page buffer internally.

## Primitive audit

The classification describes the original shim versus ThingPulse. All drawing
methods below now use the ThingPulse algorithms, except the explicitly noted
non-raster behavior. Integer types, clipping checks, duplicate writes and
reference quirks are retained.

| Primitive | Original classification | Difference and resolution |
| --- | --- | --- |
| setPixel | identical algorithm | Same bounds and colour effects; now writes packed bits internally. |
| setPixelColor / clearPixel | differs (missing) | Added the reference operations, including colour-dependent clearPixel. |
| drawLine | identical algorithm | Same int16_t Bresenham, endpoint order and error updates. |
| drawRect | differs in call order | Restored top, left, right, bottom order; same ordinary output. |
| fillRect | differs | Row scan replaced with reference column/vertical-line scan. |
| drawCircle | differs | Different midpoint recurrence, write order/multiplicity and zero/negative-radius behavior; ported the do-while and cardinal writes. |
| drawCircleQuads | differs (missing) | Added reference quadrant selection and shared cardinal-point rules. |
| fillCircle | differs | sqrt scanlines replaced with midpoint scanlines of lengths 2*x and 2*y and final length 2*radius. |
| drawTriangle | identical algorithm | Three reference drawLine calls. |
| fillTriangle | differs | All-edge intersections replaced with sorted vertices, two scan phases, int32_t accumulators and exact flat-triangle case. |
| drawHorizontalLine | differs | Per-pixel clipping replaced with reference clipping, int16_t length arithmetic and packed bit writes. |
| drawVerticalLine | differs | Per-pixel scan replaced with reference clipping, initial partial page, whole pages and final partial page. |
| drawProgressBar | differs | Rectangular bar replaced with rounded quadrants/circles and exact geometry; forces and leaves WHITE. |
| drawFastImage / drawInternal | differs | XBM alias replaced with reference column-major page bytes and byte-shift clipping, including padding bits and top-edge quirks. |
| drawXbm | identical algorithm | Same little-endian row bitmap, ceil(width/8) stride and transparent unset bits; uses reference body. |
| drawIco16x16 | differs (missing) | Added opaque WHITE/BLACK pixel writes, optional inversion, independent of current colour. |
| drawString / former drawChar | differs | Reference internal byte renderer, UTF-8 lookup, multiline tokenization, per-line alignment, vertical centering and character-count return. |
| drawStringMaxWidth | differs | Previously ignored width; now uses reference breakpoints, widths and continuation return. |
| getStringWidth, both overloads | differs | Reference table lookup, newline maximum width, explicit UTF-8 option and raw-byte String overload; removed invented fallback widths. |
| setFont / setTextAlignment | identical raster state | Same font-pointer/alignment selection. ThingPulse Print/log-buffer reset is not implemented by this shim. Existing fonts already contain real ThingPulse data. |
| BLACK / WHITE / INVERSE | identical for setPixel, composite differences | Packed reference operations now preserve repeated XOR writes, icon colour overrides and progress-bar colour state. |
| clipping | differs in composite paths | Reference checks and integer widths now apply to each method, rather than relying exclusively on setPixel. |
| clear | identical algorithm | All pixels cleared; packed memset now replaces flat memset. |
| resetDisplay / cls | differs | Now clear and push the frame, matching reference visible behavior; hardware back/log buffers are absent. |
| display / getBuffer / getBufferSize | differs in representation by design | Expand packed raster to the existing stable 8192-byte flat buffer at getBuffer/display; bridge signatures and pixel layout remain unchanged. getBuffer materializes a snapshot. |
| init / end / displayOn / displayOff / invertDisplay / normalDisplay / contrast / brightness / flipScreenVertically / mirrorScreen | differs, non-raster | Existing hardware-control stubs/lifecycle remain. Hardware commands do not mutate the ThingPulse drawing buffer; physical power, scan direction and contrast are not emulated. Default font remains selected by init, not construction. |
| guest fillTriangle | differs | Same ThingPulse scan phases now emitted through the existing hline import; scanEdge removed; no added import or ABI change. |

## Verification

Run from the lane with every command's TEMP/TMP/TMPDIR set to `<lane>/tmp`:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -d firmware -e test_emu
```

The global PlatformIO directory is read-only in this lane. The initial command
failed before compilation with permission denied on `platforms.lock`. Copies
of the installed `platforms/native` and `packages/tool-scons` are under
`tmp/pio-core`; set `PLATFORMIO_CORE_DIR=<lane>/tmp/pio-core` to reproduce.
Unity was already installed and no package downloads were performed.
The baseline line suite passed 18/18 before changes.

Final PlatformIO output (exit 1):

```text
test_emu_drawing         ERRORED
test_emu_draw_line       PASSED
test_emu_guest_triangle  PASSED
33 test cases: 1 failed, 31 succeeded
```

The wrapper includes an additional CTRL_BREAK_EVENT execution error for the
failing drawing process. There are 32 actual Unity cases: 31 pass, one fails.
The drawing executable was also run directly: `13 Tests 1 Failures 0 Ignored`.
The line suite passes all 18 cases. The actual guest header passes its one
case containing 6021 triangle comparisons.

The drawing sweeps cover all three colours on mixed backgrounds, circles
with radii 0-31 (also -3 to -1), ten normal/edge/off-screen centers, every
quadrant mask, fixed-seed random and degenerate triangles, negative/zero
sizes, rectangles, lines, progress bars, image layouts and three fonts with
all four alignments, line breaks and wrapping. Control/signed UTF-8 font
lookups in ThingPulse can index before the font table; the text-equivalence
harness pads copied fonts to make those quirks deterministic without invalid
reads. This does not establish portable behavior for undefined reference
font lookups. The capture tests use the unmodified real font arrays.

All four capture fixtures are verbatim copies. Pages 1, 2 and 5 match all
1024 bytes, including text; there are no masks. Page 3 differs by 24 pixels:
first mismatched byte 334, expected 0x80, actual 0x00. A separate provenance
test replays the historical guest triangle code and matches page 3 in all
1024 bytes. Thus the supplied capture records the old guest algorithm,
not native ThingPulse fillTriangle. Matching it and matching the requested
native algorithm are incompatible. The new page-3 equality assertion remains
failing; no fixture or geometry assertion was weakened. A new hardware
capture after installing the corrected guest is needed.

## Builds and runtime

Emscripten 3.1.51 is used from the supplied read-only SDK. EM_CACHE points to
a lane-local copy at `tmp/em-cache`; the generated version header is in tmp.
`build_app.bat Breakout` was attempted. SDK activation initially emitted a
POSIX environment that the batch wrapper could not use; explicitly supplying
the SDK tools/Python resolved that, then the wrapper failed with WinError 2
because CMake is not installed (Ninja is also unavailable).
A direct `em++.bat @tmp/breakout-args.rsp` build using the CMake Breakout
source list, include paths and browser link settings succeeded (exit 0),
producing `tmp/breakout/cyberfidget.js` and `cyberfidget.wasm`. Its only compiler
warnings were existing NeoPixel constant conversions. Node loaded the result,
rendered three nonempty frames of 8192 binary pixels and stopped successfully.
This proves compilation/linking/runtime, but does not claim the CMake wrapper
passed.

The exact requested `build_custom_device_app.sh AbiExerciser abiExerciser
<lane>/reference/abi_exerciser <lane>/tmp/abi.wasm` succeeded (exit 0), producing
17186 bytes. It emitted unresolved-symbol warnings for the intended host
imports. Module inspection found 32 cf imports, with no filled-triangle
import. The native guest harness emitted expected ignored WebAssembly import
attribute warnings. No live-device installation or on-device manual retest
was performed.

Full logs and build response arguments are under lane tmp: final-tests.log,
drawing-direct.log, baseline-tests.log, baseline-local-tests.log,
breakout-wrapper-build.log, breakout-direct-build.log,
device-module-build.log and runtime-smoke.log. Earlier compile/test retries
are also logged; the initial oracle lacked a default argument and the port
needed std::max qualified for the existing minimal line harness, both fixed.
The first Node smoke attempt collided with CommonJS's module binding, fixed
by renaming the local variable.

## Replacement/deletion self-audit

No pre-existing files were removed. Existing function bodies replaced:

- SSD1306Wire::setPixel: packed storage with the same pixel semantics.
- drawLine, drawTriangle and drawXbm: direct reference transcriptions;
  algorithms were already equivalent.
- drawHorizontalLine, drawVerticalLine, drawRect, fillRect, drawCircle,
  fillCircle, fillTriangle, drawProgressBar and drawFastImage: reference
  drawing order, clipping, integer arithmetic and raster algorithms.
- drawString, drawStringMaxWidth and both getStringWidth overloads: reference
  text rasterization, alignment, wrapping, widths and return values.
- clear, display, getBuffer: packed storage and flat bridge materialization.
- resetDisplay and cls: clear plus frame push as in ThingPulse.
- drawChar: removed in favor of reference drawStringInternal/drawInternal.
- DisplayProxy::fillTriangle: exact native scan phases through hline.
- DisplayProxy::scanEdge: removed because the reference fill does not use
  all-edge intersections.

The old `_buffer` storage role was replaced with `_packed` plus a mutable
flat bridge buffer. The stale scaled-font comment was corrected. The guest
private scanEdge block was the only removed helper. No out-of-scope code,
assets, existing tests or assertions were removed.

## Remaining differences and constraints

- Page-3 capture conflict described above; acceptance is not all green.
- Controller hardware effects and Print/log-buffer behavior are absent.
- Before init the shim has no font, unlike ThingPulse construction; null-font
  guards retain the old shim's behavior. Valid initialized drawing uses the
  ported algorithms.
- Guest progress-bar and wrapped-text routines remain different: the allowed
  guest edit was specifically its filled triangle. They need separate scope
  authorization to change.
- Reference undefined behavior (extreme int16_t loops, invalid font lookups,
  invalid image pointers) has not been converted into a new contract or
  exhaustively tested. Integer types and reference checks remain intact.
- No planning repository edits/review command were possible within the
  explicit lane-only blast radius. No commits, branches or pushes were made.
