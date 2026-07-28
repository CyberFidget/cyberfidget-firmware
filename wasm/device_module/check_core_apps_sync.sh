#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Keep the CI core-app inventory aligned with the shipped batch build and glue.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
APP_LIST="$HERE/core_apps.txt"
BUILD_FILE="$HERE/build_device_modules.bat"
GLUE_FILE="$HERE/shims/cf_app_glue.cpp"
MODE="${1:-check}"

if [ "$MODE" != "check" ] && [ "$MODE" != "--matrix" ]; then
  echo "Usage: $0 [--matrix]" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TMP_DIR"' EXIT

awk -F '|' '
  /^[[:space:]]*(#|$)/ { next }
  NF != 4 { printf "Invalid core app row at line %d: expected four fields\n", NR > "/dev/stderr"; bad = 1; next }
  $1 !~ /^[A-Za-z_][A-Za-z0-9_]*$/ || $2 !~ /^[A-Za-z_][A-Za-z0-9_]*$/ ||
    $3 !~ /^lib\/[A-Za-z0-9_/-]+$/ || $4 !~ /^[a-z0-9_-]+\.wasm$/ {
      printf "Invalid core app row at line %d: unsafe field value\n", NR > "/dev/stderr"; bad = 1; next
    }
  { print $1 "|" $2 "|" $3 "|" $4 }
  END { if (bad) exit 1 }
' "$APP_LIST" > "$TMP_DIR/list"

if [ ! -s "$TMP_DIR/list" ]; then
  echo "Core app list is empty" >&2
  exit 1
fi

cut -d '|' -f 1,3,4 "$TMP_DIR/list" | sort > "$TMP_DIR/expected-build"
awk '
  /^call em\+\+/ {
    line = $0
    # A stanza may span any number of caret-continued lines; join them all
    # (the old single-getline join silently dropped apps whose stanza grew).
    while (line ~ /\^[[:space:]]*$/ && (getline next_line) > 0) {
      sub(/\^[[:space:]]*$/, " ", line)
      line = line next_line
    }
    gsub(/\\/, "/", line)
    print line
  }
' "$BUILD_FILE" | while IFS= read -r command; do
  src_dir="$(printf '%s\n' "$command" | sed -nE 's@.*-I \.\./\.\./(lib/[^ ]+).*@\1@p')"
  # The module is the .cpp that lives in the app'\''s own -I directory; other
  # compiled sources (shared runtime .cpps) must not be mistaken for it.
  module="$(printf '%s\n' "$command" | sed -nE "s@.*\.\./\.\./${src_dir}/([A-Za-z_][A-Za-z0-9_]*)\.cpp .*@\1@p")"
  output="$(printf '%s\n' "$command" | sed -nE 's@.* -o ([a-z0-9_-]+\.wasm).*@\1@p')"
  if [ -n "$src_dir" ] && [ -n "$module" ] && [ -n "$output" ]; then
    printf '%s|%s|%s\n' "$module" "$src_dir" "$output"
  fi
done | sort > "$TMP_DIR/actual-build"

cut -d '|' -f 1,2 "$TMP_DIR/list" | sort > "$TMP_DIR/expected-glue"
awk '
  /^#include "[A-Za-z_][A-Za-z0-9_]*\.h"/ {
    header = $0
    sub(/^#include "/, "", header)
    sub(/\.h".*/, "", header)
  }
  /^#define CF_APP_INSTANCE [A-Za-z_][A-Za-z0-9_]*$/ && header != "" {
    print header "|" $3
    header = ""
  }
' "$GLUE_FILE" | sort > "$TMP_DIR/actual-glue"

sync_ok=true
if ! diff -u "$TMP_DIR/expected-build" "$TMP_DIR/actual-build"; then
  echo "Core app list does not match build_device_modules.bat" >&2
  sync_ok=false
fi
if ! diff -u "$TMP_DIR/expected-glue" "$TMP_DIR/actual-glue"; then
  echo "Core app list does not match cf_app_glue.cpp" >&2
  sync_ok=false
fi
if [ "$sync_ok" != true ]; then
  exit 1
fi

while IFS='|' read -r module _ src_dir _; do
  if [ ! -f "$REPO_ROOT/$src_dir/$module.cpp" ] || [ ! -f "$REPO_ROOT/$src_dir/$module.h" ]; then
    echo "Core app source pair is missing: $src_dir/$module.{h,cpp}" >&2
    exit 1
  fi
done < "$TMP_DIR/list"

if [ "$MODE" = "--matrix" ]; then
  awk -F '|' '
    BEGIN { printf "{\"app\":["; separator = "" }
    {
      printf "%s{\"module\":\"%s\",\"instance\":\"%s\",\"src_dir\":\"%s\",\"output\":\"%s\"}", separator, $1, $2, $3, $4
      separator = ","
    }
    END { print "]}" }
  ' "$TMP_DIR/list"
else
  echo "Core app list sync: passed ($(wc -l < "$TMP_DIR/list") apps)"
fi
