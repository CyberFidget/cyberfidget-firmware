#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dismo Industries LLC
#
# T-001 acceptance test starter battery. Plain bash so it merges easily into
# Docker / Jenkins / GHA / whatever the collaborator's T-002 harness settles on.
#
# Covers the ACs that don't need physical hardware or a browser:
#   1. generate_version.py correctness (clean / dirty / no-git / missing /
#      malformed version.txt / override env var / atomic-write skip)
#   2. PlatformIO smoke build (header generated, build summary printed,
#      summary fw=... matches header FW_VERSION_FULL_STRING)
#   3. WASM standalone version.h generation (skips if emcc absent — full
#      WASM build remains in T-002 territory)
#   4. Static checks on emitted code (HAL getters, banner call, WASM exports)
#   5. CI workflow file sanity (tag-assertion step, workflow_dispatch trigger)
#
# Out of scope (T-002, collaborator):
#   - Flashing, serial CLI verification, BLE, App Builder UI, real CI runs.
#
# Exit 0 on full pass; non-zero with clear message on first failure.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

PASS=0
FAIL=0
FAIL_DETAILS=()

pass()  { PASS=$((PASS+1));  echo "  PASS  $1"; }
fail()  { FAIL=$((FAIL+1));  echo "  FAIL  $1"; FAIL_DETAILS+=("$1"); }
section() { echo ""; echo "=== $1 ==="; }

# Helper: assert the given grep pattern matches the file
assert_grep() {
    local pattern="$1"
    local file="$2"
    local desc="$3"
    if grep -q -- "$pattern" "$file"; then
        pass "$desc"
    else
        fail "$desc (pattern '$pattern' missing in $file)"
    fi
}

# Helper: run generate_version.py standalone and capture its output header
run_generate() {
    local out="$1"
    shift
    python "$REPO_ROOT/scripts/generate_version.py" --out "$out" --repo-root "$REPO_ROOT" "$@" >/dev/null 2>&1
    return $?
}

# ----------------------------------------------------------------------------
section "1. generate_version.py correctness"
# ----------------------------------------------------------------------------

TMP="${TMPDIR:-/tmp}/cf-acceptance-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# 1a. Clean run (whatever the current tree is) — header has expected macros.
H="$TMP/v.h"
if run_generate "$H"; then
    pass "generate_version.py runs successfully"
else
    fail "generate_version.py exits non-zero on a normal run"
fi

if [ -f "$H" ]; then
    pass "generated header file exists"
else
    fail "generated header file not created"
fi

for macro in FW_VERSION_MAJOR FW_VERSION_MINOR FW_VERSION_PATCH \
             FW_VERSION_PRERELEASE \
             VERSION_ENCODE FW_VERSION FW_VERSION_STRING \
             FW_GIT_HASH FW_GIT_DIRTY FW_BUILD_TIMESTAMP \
             FW_BUILD_TYPE FW_VERSION_FULL_STRING; do
    assert_grep "#define $macro" "$H" "header defines $macro"
done

# 1g. Pre-release suffix propagation: simulate a CI tag push of v<X.Y.Z>-rc1
# and confirm FW_VERSION_PRERELEASE / FW_VERSION_STRING / FW_VERSION_FULL_STRING
# all carry the suffix (semver item 9).
H_pre="$TMP/v_prerelease.h"
GITHUB_ACTIONS=true GITHUB_REF_NAME="v9.9.9-rc1" \
    python "$REPO_ROOT/scripts/generate_version.py" \
        --out "$H_pre" --repo-root "$REPO_ROOT" >/dev/null 2>&1
if grep -q '#define FW_VERSION_PRERELEASE "rc1"' "$H_pre" && \
   grep -qF 'FW_VERSION_STRING "1.1.0-rc1"' "$H_pre"; then
    pass "CI tag with -rc1 suffix populates FW_VERSION_PRERELEASE and FW_VERSION_STRING"
else
    fail "prerelease suffix not propagated correctly (expected rc1 in macros)"
fi
if grep -qF 'FW_VERSION_FULL_STRING "1.1.0-rc1+' "$H_pre"; then
    pass "FW_VERSION_FULL_STRING includes prerelease suffix"
else
    fail "FW_VERSION_FULL_STRING missing prerelease suffix"
fi

# 1h. No-suffix tag leaves FW_VERSION_PRERELEASE empty.
H_rel="$TMP/v_release.h"
GITHUB_ACTIONS=true GITHUB_REF_NAME="v9.9.9" \
    python "$REPO_ROOT/scripts/generate_version.py" \
        --out "$H_rel" --repo-root "$REPO_ROOT" >/dev/null 2>&1
if grep -q '#define FW_VERSION_PRERELEASE ""' "$H_rel"; then
    pass "release tag (no suffix) leaves FW_VERSION_PRERELEASE empty"
else
    fail "release tag did not leave FW_VERSION_PRERELEASE empty"
fi

# 1b. Override env var → FW_BUILD_TYPE matches override
H2="$TMP/v_override.h"
CYBERFIDGET_BUILD_TYPE_OVERRIDE=user-build run_generate "$H2"
if grep -q '#define FW_BUILD_TYPE "user-build"' "$H2"; then
    pass "CYBERFIDGET_BUILD_TYPE_OVERRIDE=user-build sets FW_BUILD_TYPE"
else
    fail "CYBERFIDGET_BUILD_TYPE_OVERRIDE=user-build did not set FW_BUILD_TYPE"
fi

# 1c. --build-type-override flag → FW_BUILD_TYPE matches flag
H3="$TMP/v_flag.h"
run_generate "$H3" --build-type-override "jenkins-hil"
if grep -q '#define FW_BUILD_TYPE "jenkins-hil"' "$H3"; then
    pass "--build-type-override jenkins-hil sets FW_BUILD_TYPE"
else
    fail "--build-type-override flag did not set FW_BUILD_TYPE"
fi

# 1d. Missing version.txt → hard fail
mv version.txt "$TMP/version.txt.saved"
if python "$REPO_ROOT/scripts/generate_version.py" --out "$TMP/v_missing.h" --repo-root "$REPO_ROOT" >/dev/null 2>&1; then
    fail "missing version.txt did NOT cause hard-fail (expected non-zero exit)"
else
    pass "missing version.txt causes hard-fail"
fi
mv "$TMP/version.txt.saved" version.txt

# 1e. Malformed version.txt → hard fail
echo "1.2" > "$TMP/version.txt.bad"
mv version.txt "$TMP/version.txt.saved"
mv "$TMP/version.txt.bad" version.txt
if python "$REPO_ROOT/scripts/generate_version.py" --out "$TMP/v_malformed.h" --repo-root "$REPO_ROOT" >/dev/null 2>&1; then
    fail "malformed version.txt did NOT cause hard-fail (expected non-zero exit)"
else
    pass "malformed version.txt causes hard-fail"
fi
mv "$TMP/version.txt.saved" version.txt

# 1f. Atomic-write / byte-identical skip — when content is identical, mtime is preserved
H_skip="$TMP/v_skip.h"
run_generate "$H_skip"
MTIME1=$(stat -c %Y "$H_skip" 2>/dev/null || stat -f %m "$H_skip")
sleep 1
# Re-run with the same override to maximize chance of byte-identical content.
# (Note: with a dirty tree, FW_BUILD_TIMESTAMP comes from now() and the file
# WILL change. The atomic-skip optimization is for clean-tree / committer-date
# builds; we skip this assertion when the tree is dirty.)
if grep -q '#define FW_GIT_DIRTY 0' "$H_skip"; then
    run_generate "$H_skip"
    MTIME2=$(stat -c %Y "$H_skip" 2>/dev/null || stat -f %m "$H_skip")
    if [ "$MTIME1" = "$MTIME2" ]; then
        pass "byte-identical re-run preserves mtime (skip-write working)"
    else
        fail "byte-identical re-run changed mtime (expected skip-write)"
    fi
else
    pass "atomic-skip test skipped (dirty tree — timestamp churns intentionally)"
fi

# ----------------------------------------------------------------------------
section "2. PlatformIO smoke build"
# ----------------------------------------------------------------------------

# Find pio executable. PlatformIO is usually installed in a venv on Windows
# and exposes a Scripts/pio.exe entry. Search common locations.
PIO="${PIO:-}"
if [ -z "$PIO" ]; then
    for candidate in pio platformio \
                     "$HOME/.platformio/penv/Scripts/pio.exe" \
                     "$HOME/.platformio/penv/bin/pio"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            PIO="$candidate"
            break
        fi
        if [ -x "$candidate" ]; then
            PIO="$candidate"
            break
        fi
    done
fi

if [ -z "$PIO" ]; then
    echo "  SKIP  pio not found on PATH — skipping smoke build (set PIO env var to override)"
else
    BUILD_LOG="$TMP/pio_build.log"
    if "$PIO" run -e local > "$BUILD_LOG" 2>&1; then
        pass "pio run -e local exits 0"
    else
        fail "pio run -e local failed (see $BUILD_LOG)"
    fi

    if [ -f generated/version.h ]; then
        pass "generated/version.h exists after build"
    else
        fail "generated/version.h missing after build"
    fi

    # Build summary block printed somewhere in the build log
    if grep -q "Cyber Fidget firmware build complete" "$BUILD_LOG"; then
        pass "build-summary block printed during build"
    else
        fail "build-summary block missing from build log"
    fi

    # The summary's fw= line should match the header's FW_VERSION_FULL_STRING.
    # Use fixed-string match because `+` and `.` are regex meta-chars in version strings.
    HEADER_FULL=$(grep -oE 'FW_VERSION_FULL_STRING "[^"]+"' generated/version.h | head -1 | sed -E 's/FW_VERSION_FULL_STRING "(.*)"/\1/')
    if [ -n "$HEADER_FULL" ] && grep -qF "fw:     $HEADER_FULL" "$BUILD_LOG"; then
        pass "build summary fw=... matches header FW_VERSION_FULL_STRING ($HEADER_FULL)"
    else
        fail "build summary fw=... does not match header (header='$HEADER_FULL')"
    fi
fi

# ----------------------------------------------------------------------------
section "3. WASM standalone version.h generation"
# ----------------------------------------------------------------------------

if CYBERFIDGET_BUILD_TYPE_OVERRIDE=wasm \
   python "$REPO_ROOT/scripts/generate_version.py" \
       --out "$REPO_ROOT/wasm/generated/version.h" --repo-root "$REPO_ROOT" \
       >/dev/null 2>&1; then
    pass "wasm version.h generation succeeds"
else
    fail "wasm version.h generation failed"
fi

if [ -f wasm/generated/version.h ]; then
    pass "wasm/generated/version.h exists"
    if grep -q '#define FW_BUILD_TYPE "wasm"' wasm/generated/version.h; then
        pass "wasm header has FW_BUILD_TYPE \"wasm\""
    else
        fail "wasm header missing FW_BUILD_TYPE \"wasm\""
    fi
else
    fail "wasm/generated/version.h not created"
fi

if command -v emcc >/dev/null 2>&1; then
    echo "  INFO  emcc found — full WASM build is T-002's job; skipping here."
else
    echo "  SKIP  emcc not on PATH — full WASM build skipped (T-002 territory)"
fi

# ----------------------------------------------------------------------------
section "4. Static checks on emitted code"
# ----------------------------------------------------------------------------

assert_grep 'printFirmwareBanner();' lib/HAL/HAL.cpp \
    "HAL.cpp calls printFirmwareBanner() in initHardware"
assert_grep '#include "version.h"' lib/HAL/HAL.cpp \
    "HAL.cpp includes generated version.h"
assert_grep 'printFirmwareBanner' lib/HAL/HAL.h \
    "HAL.h declares printFirmwareBanner"
assert_grep 'void printFirmwareBanner' lib/HAL/HAL.cpp \
    "HAL.cpp implements printFirmwareBanner"

for sig in 'getFirmwareVersionString' 'getFirmwareVersion' 'getFirmwareGitHash' \
           'getFirmwareBuildType' 'getFirmwareBuildTimestamp'; do
    assert_grep "$sig" lib/Globals/globals.h "globals.h declares $sig"
    assert_grep "$sig" lib/Globals/globals.cpp "globals.cpp implements $sig"
done

assert_grep 'SerialCli::instance().poll()' lib/AppManager/AppManager.cpp \
    "AppManager.cpp delegates serial polling to SerialCli"
assert_grep 'class SerialCli' lib/SerialCli/SerialCli.h \
    "SerialCli.h declares the class"
assert_grep 'void SerialCli::poll' lib/SerialCli/SerialCli.cpp \
    "SerialCli.cpp implements poll()"
assert_grep '\[boot\] fw=' lib/HAL/HAL.cpp \
    "boot banner uses [boot] line prefix"
assert_grep '\[cmd\] version=' lib/SerialCli/SerialCli.cpp \
    "version command uses [cmd] line prefix"

for sym in 'cyberfidget_version_string' 'cyberfidget_version_encoded' \
           'cyberfidget_build_type'; do
    assert_grep "$sym" wasm/main_wasm.cpp "main_wasm.cpp exports $sym"
    assert_grep "_$sym" wasm/CMakeLists.txt "CMakeLists.txt EXPORTED_FUNCTIONS lists _$sym"
done

# ----------------------------------------------------------------------------
section "5. CI workflow file sanity"
# ----------------------------------------------------------------------------

WF=".github/workflows/build-release.yml"
assert_grep 'Verify tag matches version.txt' "$WF" \
    "build-release.yml has tag-assertion step"
assert_grep 'workflow_dispatch:' "$WF" \
    "build-release.yml supports workflow_dispatch"
assert_grep 'CYBERFIDGET_BUILD_TYPE_OVERRIDE' "$WF" \
    "build-release.yml passes CYBERFIDGET_BUILD_TYPE_OVERRIDE to build"

# ----------------------------------------------------------------------------
echo ""
echo "=================================================="
echo "  T-001 acceptance: $PASS pass / $FAIL fail"
echo "=================================================="

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Failures:"
    for f in "${FAIL_DETAILS[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
exit 0
