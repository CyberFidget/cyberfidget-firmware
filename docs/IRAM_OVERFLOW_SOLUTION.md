# How We Solved the ESP32 IRAM Overflow

## The Problem

The CyberFidget is an ESP32-based handheld gadget (Adafruit Feather ESP32 v2) with an OLED display, 6 buttons, a slider, LEDs, an accelerometer, and an SD card slot. Its firmware runs an AppManager that switches between multiple mini-apps. We wanted to add a **Music Player app** that streams MP3 files from the SD card to a Bluetooth A2DP speaker.

The audio stack requires three libraries:
- **arduino-audio-tools** (v1.2.0) - Audio pipeline framework
- **ESP32-A2DP** (v1.8.3) - Bluetooth A2DP streaming
- **arduino-libhelix** (v0.8.7) - MP3 decoder (Helix codec)

The first attempt (on a `music_player` branch) compiled the audio stack alongside **25+ mini-apps** (games, screensavers, utilities) totaling ~10,000+ lines of code. The linker failed with:

```
region `iram0_0_seg' overflowed by 2376 bytes
```

The firmware was 2,376 bytes over the ESP32's 128KB IRAM limit.

## What is IRAM and Why Does It Overflow?

### ESP32 Memory Architecture (Simplified)

The ESP32 has several distinct memory regions:

| Region | Size | Address Range | Purpose |
|--------|------|--------------|---------|
| **IRAM** | 128 KB | `0x40080000 - 0x4009FFFF` | Instruction RAM - fast code execution |
| **DRAM** | 320 KB | `0x3FFB0000 - 0x3FFFFFFF` | Data RAM - variables, heap, stack |
| **Flash** | 4-16 MB | `0x400D0020+` | Slow storage - most code runs from here via cache |

Most code runs from **flash** through the instruction cache. But certain code **must** live in IRAM:

1. **Interrupt Service Routines (ISRs)** - Can't wait for flash cache misses during interrupts
2. **FreeRTOS kernel** - Scheduler, context switching, tick handler
3. **WiFi/BT radio drivers** - Time-critical PHY layer code
4. **Hot-path functions** - ESP-IDF places performance-critical libc functions in IRAM

The ESP-IDF framework decides what goes in IRAM via a **linker script** called `sections.ld`. This file contains explicit rules like:

```ld
/* Place in IRAM for ISR safety */
*libc.a:lib_a-strftime.*(.literal .literal.* .text .text.*)
*libc.a:lib_a-mktime.*(.literal .literal.* .text .text.*)
/* ...~100 more libc objects... */
```

These rules say: "Take the `.text` (code) and `.literal` (constants used by code) sections from these specific libc object files and put them in IRAM instead of flash."

### Why It's a Zero-Sum Game

IRAM is a fixed 128KB. The first ~1KB is reserved for the interrupt vector table. Everything else is a competition:

```
128 KB IRAM
  - 1 KB   vectors
  - 31 KB  BT Classic (A2DP) stack
  - 18 KB  FreeRTOS kernel
  - 9 KB   PHY radio drivers
  - 8 KB   WiFi (even though we don't use it, BT shares the radio)
  - 15 KB  libc functions (time, string, memory operations)
  - 10 KB  SPI flash drivers, cache management
  - 5 KB   HAL timer, interrupt controller
  - ???    Your app's ISR handlers, template instantiations, etc.
  --------
  ~97+ KB  spoken for before your code even links
```

When you add BT A2DP (~31KB of IRAM), you're eating a quarter of the entire budget. Add 25 apps with their own ISR handlers, NeoPixel RMT drivers, template instantiations, etc., and you blow past 128KB.

## The Investigation

### Step 1: Strip the App Manifest

The first realization was that the `music_player` branch only removed 4 small apps (~700 lines) while keeping ~25 apps (~10,000+ lines). Each app contributes to IRAM through:
- Static constructors that may trigger ISR registrations
- Template instantiations (C++ templates can generate IRAM-placed code)
- Library dependencies (NeoPixel's RMT ISR handler alone is ~2-4KB)

We stripped the manifest down to **5 entries**: Boot Animation, Menu, Power Manager, Spaceship, and Music Player. We deleted 19 app directories. This was necessary but **not sufficient** - we still overflowed by 2,376 bytes.

### Step 2: Build Flags (IRAM Diet)

We added every IRAM-saving build flag we could find:

```ini
; platformio.ini
build_flags =
    -Os                                    ; Size optimization
    -DCORE_DEBUG_LEVEL=0                   ; No debug logging
    -DCONFIG_BT_BLE_ENABLED=0             ; Disable BLE (saves ~10-20KB)
    -DCONFIG_BT_NIMBLE_ENABLED=0
    -DAUDIOTOOLS_NO_ANALOG                 ; No analog audio drivers
    -DAUDIOTOOLS_NO_PWM                    ; No PWM audio
    -DAUDIOTOOLS_NO_ADC                    ; No ADC audio
    -DAUDIOTOOLS_NO_DAC                    ; No DAC audio
    -DCONFIG_ESP32_WIFI_IRAM_OPT=0        ; WiFi IRAM optimizations off
    -DCONFIG_ESP32_WIFI_RX_IRAM_OPT=0
    -DCONFIG_NEWLIB_NANO_FORMAT=1          ; Smaller printf family

build_type = release                       ; Release avoids debug IRAM overhead
```

Important note: `-DCONFIG_BT_BLE_ENABLED=0` might not work if the framework's pre-compiled `sdkconfig.h` hardcodes it. Check `framework-arduinoespressif32-libs/esp32/qio_qspi/include/sdkconfig.h` - if it defines `CONFIG_BT_BLE_ENABLED 1` without `#ifndef` guards, build flags can't override it. In our case, BLE was already disabled in the pre-compiled libraries, so this flag was belt-and-suspenders.

Still overflowing by 2,376 bytes after all these flags.

### Step 3: Generate a Linker Map

To see exactly what's consuming IRAM, we added:

```ini
build_flags =
    ; ...existing flags...
    -Wl,-Map,firmware.map
```

This produces a `firmware.map` file in `.pio/build/adafruit_feather_esp32_v2/` that lists every section, its address, its size, and which library/object it came from.

### Step 4: Analyze the Map

We wrote [scripts/analyze_iram.py](../scripts/analyze_iram.py) to parse the map file and find everything placed in the IRAM address range (`0x40080000` - `0x400A0000`):

```bash
python scripts/analyze_iram.py
```

Output (abbreviated):
```
Total code in IRAM range: 86,547 bytes (84.5 KB)

=== IRAM usage by LIBRARY ===
Library                                                  Bytes       KB
---------------------------------------------------------------------------
libbtdm_app.a                                           31,286     30.6
libfreertos.a                                           18,422     18.0
libc.a                                                  15,489     15.1
libphy.a                                                 9,010      8.8
libspi_flash.a                                           4,186      4.1
libhal.a                                                 3,502      3.4
...
```

The critical finding: **`libc.a` was consuming 15.5KB of IRAM** with time-related functions that don't need to be ISR-safe:

```
=== Top objects in IRAM ===
libc.a(lib_a-strftime.o)                                  3,266
libc.a(lib_a-svfprintf.o)                                 2,800
libc.a(lib_a-mktime.o)                                    1,435
libc.a(lib_a-vfprintf.o)                                  1,339
libc.a(lib_a-lcltime_r.o)                                   817
libc.a(lib_a-strptime.o)                                    667
libc.a(lib_a-wcsftime.o)                                    596
...
```

`strftime` alone (3,266 bytes) was larger than our entire overflow! These time functions are placed in IRAM by ESP-IDF's `sections.ld` for performance, but they're not called from ISR context. They don't need to be there.

### Step 5: The Linker Script Problem

The framework's `sections.ld` (located at `~/.platformio/packages/framework-arduinoespressif32-libs/esp32/ld/sections.ld`) has a two-part system:

**Part 1 - IRAM placement** (in `.iram0.text` section):
```ld
.iram0.text : {
    /* ...other entries... */
    *libc.a:lib_a-strftime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-mktime.*(.literal .literal.* .text .text.*)
    /* ...~100 more libc entries... */
} > iram0_0_seg
```

**Part 2 - Flash exclusion** (in `.flash.text` section):
```ld
.flash.text : {
    *(EXCLUDE_FILE(
        *libc.a:lib_a-strftime.*
        *libc.a:lib_a-mktime.*
        /* ...same ~100 entries... */
    ) .literal EXCLUDE_FILE(...) .text ...)
} > default_code_seg
```

The `.flash.text` section has `EXCLUDE_FILE` lists that prevent these objects from going to flash (since they're supposed to be in IRAM). To move code from IRAM to flash, you need to:

1. **Remove** the explicit IRAM placement lines from `.iram0.text`
2. **Remove** the objects from `EXCLUDE_FILE` lists in `.flash.text` so the linker places them there instead

### Step 6: Failed Approaches

We tried several approaches before finding one that worked:

#### Attempt 1: Supplementary Linker Script
Created an `iram_diet.ld` with:
```ld
SECTIONS {
    .flash_iram_override : {
        *libc.a:lib_a-strftime.*(.literal .literal.* .text .text.*)
    } INSERT BEFORE .iram0.text
    /* > default_code_seg */
}
```
**Failed**: The `-T iram_diet.ld` flag wasn't passed correctly by PlatformIO (it was treated as a library flag, not a linker flag).

#### Attempt 2: PlatformIO Script with LINKFLAGS
Created a pre-build script to inject `-T iram_diet.ld` via `env.Append(LINKFLAGS=[...])`.
**Failed**: The supplementary script was processed before `memory.ld` defined the `default_code_seg` region alias. The linker complained about an undeclared region.

#### Attempt 3: INSERT AFTER .flash.text
Changed the supplementary script to `INSERT AFTER .flash.text` without specifying a memory region.
**Failed**: Without an explicit `> region`, the section inherited the *previous* section's region (`iram0_0_seg`). The overflow got **worse** - from 2,376 to 11,244 bytes! The linker was now placing our overrides *in IRAM too*.

#### Why Supplementary Scripts Don't Work Here
The fundamental problem is that GNU ld's `INSERT BEFORE/AFTER` directive doesn't let you control which memory region a section goes into if the region isn't declared yet. And the ESP-IDF linker script chain (`memory.ld` -> `sections.ld`) is designed as a single unit. You can't easily inject between them.

### Step 7: The Working Solution - Patch sections.ld In Place

Since we can't work around `sections.ld`, we **patch it directly** at build time.

The script [scripts/add_iram_diet.py](../scripts/add_iram_diet.py) runs as a PlatformIO pre-build step and modifies the framework's `sections.ld` before the linker runs:

```ini
; platformio.ini
extra_scripts =
    pre:scripts/add_iram_diet.py
```

#### What the Script Does

1. **Locates** `sections.ld` in the PlatformIO packages directory
2. **Saves a backup** as `sections.ld.orig` (only on first run)
3. **Removes 21 libc time-function entries** from the `.iram0.text` section
4. **Removes the same entries** from `EXCLUDE_FILE` lists in `.flash.text`
5. **Adds a marker** (`/* IRAM_DIET_PATCHED */`) to prevent double-patching on incremental builds

#### Critical Detail: Only Patch .text Lines, Not .rodata

The `sections.ld` has **two types** of entries for each libc object:

```ld
/* In .iram0.text - CODE goes to IRAM */
*libc.a:lib_a-strftime.*(.literal .literal.* .text .text.*)

/* In .dram0.data - DATA stays in DRAM */
*libc.a:lib_a-strftime.*(.rodata .rodata.*)
```

We must **only remove the `.literal .text` lines** (code placement). The `.rodata` lines must stay - they keep the function's read-only data in fast DRAM where it belongs. An early version of our regex was too broad and removed `.rodata` lines too, causing a linker syntax error.

The final regex:
```python
pattern = rf'^\s*\*libc\.a:{re.escape(obj_name)}\.\*\(\.literal\s+\.literal\.\*\s+\.text\s+\.text\.\*\)\s*$'
```

#### EXCLUDE_FILE Cleanup Is Section-Bounded

When removing objects from `EXCLUDE_FILE` lists, we only modify the `.flash.text` section (between `.flash.text :` and `>default_code_seg`). The `.flash.rodata` section has its own `EXCLUDE_FILE` lists - we leave those alone.

```python
flash_text_start = patched.find('.flash.text :')
flash_text_end = patched.find('>default_code_seg', flash_text_start)
# Only modify content between these boundaries
```

### Step 8: The Result

```
RAM:   [==        ]  16.3% (used 53368 bytes from 327680 bytes)
Flash: [=====     ]  51.6% (used 1624001 bytes from 3145728 bytes)
```

IRAM analysis after patching:
```
Total code in IRAM: ~79,800 bytes (77.9 KB)
Available IRAM: ~130,044 bytes (127.0 KB)
Headroom: ~50,244 bytes (49.1 KB)
```

We went from **2,376 bytes over** to **~50KB of headroom**. The time functions we moved to flash:

| Function | Size (bytes) | ISR-safe needed? |
|----------|-------------|-----------------|
| `strftime` | 3,266 | No |
| `mktime` | 1,435 | No |
| `lcltime_r` | 817 | No |
| `strptime` | 667 | No |
| `wcsftime` | 596 | No |
| `gmtime_r` | 209 | No |
| `gmtime` | 38 | No |
| `tzset_r` | 270 | No |
| `asctime` | 38 | No |
| `ctime` | 38 | No |
| Other time utils | ~500 | No |
| **Total saved** | **~7,400** | |

These functions are called during normal program flow (e.g., displaying the time on a clock), never from interrupt context. Moving them to flash adds a negligible cache-miss penalty on first call but frees precious IRAM for the BT stack.

## The MusicPlayerApp Crash Fix

After solving IRAM, the firmware compiled and flashed successfully. The menu system, boot animation, and Spaceship app all worked. But selecting "Music Player" caused an immediate reboot.

### Root Cause

The `MusicPlayerApp` was declared as a **global variable**:

```cpp
// MusicPlayerApp.cpp
MusicPlayerApp musicPlayerApp(HAL::buttonManager());
```

The constructor initialized `AudioSourceIdxSD` and `AudioPlayer` as direct member variables:

```cpp
// Old MusicPlayerApp.h (broken)
class MusicPlayerApp {
    audio_tools::AudioSourceIdxSD sourceSD;  // Direct member
    audio_tools::AudioPlayer player;          // Direct member
};
```

Problems:
1. `AudioSourceIdxSD` constructor may try to access SPI/SD hardware at global init time (before `setup()` runs and hardware is initialized)
2. `AudioPlayer` constructor chains to audio pipeline setup that expects hardware to be ready
3. Global constructors run in undefined order on ESP32 - HAL might not be initialized yet

### Fix: Deferred Heap Allocation

Changed audio pipeline members to **pointers**, initialized to `nullptr`:

```cpp
// New MusicPlayerApp.h (working)
class MusicPlayerApp {
    audio_tools::AudioSourceIdxSD* pSourceSD = nullptr;  // Pointer
    audio_tools::AudioPlayer* pPlayer = nullptr;          // Pointer
    bool audioPipelineReady = false;
};
```

Audio pipeline is now created on demand in `initAudioPipeline()`, called only when the user actually connects to a BT device:

```cpp
bool MusicPlayerApp::initAudioPipeline() {
    if (audioPipelineReady) return true;

    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    SD.begin(PIN_SD_CS);

    pSourceSD = new AudioSourceIdxSD("/", "mp3", PIN_SD_CS);
    pPlayer = new AudioPlayer(*pSourceSD, a2dpStream, decoder);

    pPlayer->setSilenceOnInactive(true);
    pPlayer->setVolume(0.1);
    pPlayer->setActive(false);

    audioPipelineReady = true;
    return true;
}
```

The `begin()` method now only sets state and registers button callbacks - no hardware init at all:

```cpp
void MusicPlayerApp::begin() {
    currentState = STATE_BT_SCAN;
    menuCursorIndex = 0;
    isPlaying = false;
    isConnected = false;

    buttonManager.registerCallback(button_UpIndex, onButtonUpPressed);
    // ...
    scanForDevices();
}
```

All audio operations check for null before use:
```cpp
void MusicPlayerApp::update() {
    if (audioPipelineReady && pPlayer) {
        pPlayer->copy();  // Feed the audio pipeline
    }
    // ...render UI...
}
```

Cleanup in `end()` properly frees heap memory:
```cpp
void MusicPlayerApp::end() {
    stopPlayback();
    if (pPlayer) { delete pPlayer; pPlayer = nullptr; }
    if (pSourceSD) { delete pSourceSD; pSourceSD = nullptr; }
    audioPipelineReady = false;
}
```

## Partition Table

Previously, we used `huge_app.csv`, which gave a single 3.1MB app partition with no OTA:

```
# Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x300000,
spiffs,   data, spiffs,  0x310000,0xF0000,
```

That was the partition layout during the original IRAM investigation. The current `platformio.ini` selects `default_8MB.csv`, with two 0x330000-byte app slots for OTA. The release build reserves 64KB in each slot as a size margin. The 51.6% flash figure above is historical, not a measurement of today's firmware.

## Build Configuration Reference

### platformio.ini Key Settings

This snapshot records the original IRAM build settings; the partition line below reflects the current `platformio.ini`. Consult that file for the current values of the other flags.

```ini
[env:adafruit_feather_esp32_v2]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/51.03.03/platform-espressif32.zip
board = adafruit_feather_esp32_v2
framework = arduino

board_build.partitions = default_8MB.csv
board_build.flash_mode = qio
board_build.f_cpu = 240000000L
board_build.flash_size = 8MB
board_build.psram = enabled

build_type = release

lib_deps =
    thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays@^4.6.1
    sparkfun/SparkFun LIS2DH12 Arduino Library@^1.0.3
    sparkfun/SparkFun MAX1704x Fuel Gauge Arduino Library@^1.0.4
    git+https://github.com/pschatzmann/arduino-audio-tools.git#v1.2.0
    git+https://github.com/pschatzmann/ESP32-A2DP.git#v1.8.3
    git+https://github.com/pschatzmann/arduino-libhelix.git#v0.8.7
    git+https://github.com/me-no-dev/ESPAsyncWebServer.git

build_flags =
    -Os
    -DCORE_DEBUG_LEVEL=0
    -DCONFIG_ARDUHAL_LOG_DEFAULT_LEVEL=0
    -DNDEBUG
    -DCONFIG_BT_BLE_ENABLED=0
    -DCONFIG_BT_NIMBLE_ENABLED=0
    -DAUDIOTOOLS_NO_ANALOG
    -DAUDIOTOOLS_NO_PWM
    -DAUDIOTOOLS_NO_ADC
    -DAUDIOTOOLS_NO_DAC
    -DA2DP_SPP_SUPPORT=1
    -DCONFIG_ESP32_WIFI_IRAM_OPT=0
    -DCONFIG_ESP32_WIFI_RX_IRAM_OPT=0
    -DCONFIG_NEWLIB_NANO_FORMAT=1
    -DPSTR_ALIGN=1

extra_scripts =
    pre:scripts/add_iram_diet.py      ; IRAM linker script patcher
    post:scripts/merge_firmware.py    ; Merged binary for flashing
```

### What Each Flag Does

| Flag | Saves | How |
|------|-------|-----|
| `-Os` | Flash + some IRAM | Compiler optimizes for size over speed |
| `build_type = release` | IRAM | Debug builds add assertion handlers and debug stubs in IRAM |
| `-DCORE_DEBUG_LEVEL=0` | Flash + DRAM | Removes all `ESP_LOGx()` string data |
| `-DCONFIG_BT_BLE_ENABLED=0` | ~10-20KB IRAM | BLE stack shares IRAM with Classic |
| `-DAUDIOTOOLS_NO_*` | Flash | Prevents compilation of unused audio drivers |
| `-DCONFIG_ESP32_WIFI_IRAM_OPT=0` | IRAM | WiFi fast-path code stays in flash |
| `-DCONFIG_NEWLIB_NANO_FORMAT=1` | Flash + IRAM | Smaller printf/scanf family |
| `default_8MB.csv` | Usable flash | Two 0x330000-byte OTA app slots in the current build |

## Tools

### analyze_iram.py

Run after a build (even a failed one that generates a map file) to see what's in IRAM:

```bash
# Add to build_flags first: -Wl,-Map,firmware.map
python scripts/analyze_iram.py
```

Shows per-library and per-object-file IRAM consumption. Essential for diagnosing overflows.

### add_iram_diet.py

Runs automatically as a pre-build step. On first run:
- Creates a backup of the framework's `sections.ld` at `sections.ld.orig`
- Patches `sections.ld` to move libc time functions to flash
- Adds a `/* IRAM_DIET_PATCHED */` marker to prevent double-patching

If you need to restore the original:
```bash
cp ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/ld/sections.ld.orig \
   ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/ld/sections.ld
```

### merge_firmware.py

Runs as a post-build step. Creates a single `merged_firmware.bin` that includes bootloader + partitions + app. Useful for initial flashing via esptool.

## Lessons Learned

1. **IRAM is the real bottleneck on ESP32 with BT Classic, not flash or DRAM.** BT A2DP alone consumes ~31KB of the 128KB budget. Plan for it.

2. **Strip aggressively.** Each app, library, and template instantiation contributes to IRAM. The fix isn't micro-optimizing one function - it's removing entire subsystems you don't need.

3. **ESP-IDF's IRAM placement is conservative.** The framework places ~100 libc objects in IRAM for ISR safety, but many of them (time functions, string formatting) are never called from interrupt context. You can safely move them to flash.

4. **You can't easily override `sections.ld` with supplementary scripts.** GNU ld's `INSERT BEFORE/AFTER` doesn't give you control over memory region assignment. Patching the file in place (with a backup) is the pragmatic solution.

5. **Global constructors are dangerous on ESP32.** Any C++ object with a non-trivial constructor that's declared at file scope will construct before `setup()` runs. If it touches hardware, it crashes. Use pointer members with deferred `new` allocation instead.

6. **Always generate a linker map.** Without `-Wl,-Map,firmware.map` and a script to parse it, you're flying blind on IRAM. The build output only tells you "overflowed by X bytes" - the map tells you exactly who's responsible.

7. **PlatformIO pre-build scripts are powerful.** `extra_scripts = pre:script.py` lets you modify the build environment, patch files, add flags, etc. before compilation starts. It's the right hook for framework-level modifications.

8. **The partition table matters.** We previously used `huge_app.csv` for one large app slot. The current `default_8MB.csv` provides two OTA app slots; check `platformio.ini` before relying on a historical layout.

## Appendix: IRAM Budget Breakdown (Post-Fix)

```
128.0 KB  Total IRAM (iram0_0_seg)
 -1.0 KB  Interrupt vectors
-------
127.0 KB  Available

Consumers:
 30.6 KB  libbtdm_app.a      (BT Classic A2DP stack)
 18.0 KB  libfreertos.a      (RTOS kernel)
  8.8 KB  libphy.a           (Radio PHY layer)
  8.1 KB  libc.a             (String/memory ops - time functions moved to flash)
  4.1 KB  libspi_flash.a     (Flash driver)
  3.4 KB  libhal.a           (Hardware abstraction)
  2.8 KB  libesp_system.a    (System init, panic handler)
  1.5 KB  libxt*.a           (Xtensa arch support)
  0.8 KB  libheap.a          (Heap allocator)
  0.5 KB  libapp_trace.a     (Debug tracing)
  0.4 KB  Application code   (Our firmware)
  0.8 KB  Other libs
-------
 ~79.8 KB Used
 ~47.2 KB Headroom

Recovered by IRAM diet script: ~7.4 KB (libc time functions)
```

This ~47KB headroom is comfortable for adding features. If it ever gets tight again, candidates for the next round of IRAM diet:
- `libc.a:lib_a-svfprintf.o` (2,800 bytes) - printf formatting, not ISR-critical
- `libc.a:lib_a-vfprintf.o` (1,339 bytes) - more printf
- `libfreertos.a` entries that are only needed for SMP (we run single-core effectively)
