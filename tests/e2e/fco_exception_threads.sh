#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Linux x86_64 regression for FCO's exception-mediated resolver.  The protected
# program launches many threads that concurrently publish resolver requests and
# fault; clean and obfuscated output must stay identical.
#
# Usage: fco_exception_threads.sh <clang> <plugin> <sdk> <source> <config.toml> [seed]
set -euo pipefail

CLANG="$1"
PLUGIN="$2"
SDK="$3"
SRC="$4"
CONFIG="$5"
SEED="${6:-5757}"

if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
  echo "SKIP FCO exception thread stress requires Linux x86_64"
  exit 77
fi

SYSROOT=()
[ -n "$SDK" ] && SYSROOT=(-isysroot "$SDK")

if command -v timeout >/dev/null 2>&1; then
  LIMIT=(timeout 60)
else
  LIMIT=()
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CFLAGS=(-O2 -std=c11 -D_GNU_SOURCE -fno-builtin -pthread)

"$CLANG" "${SYSROOT[@]}" "${CFLAGS[@]}" "$SRC" -o "$TMP/ref"

env MOROK_ENABLE=1 MOROK_CONFIG="$CONFIG" MOROK_SEED="$SEED" \
  "$CLANG" "${SYSROOT[@]}" "${CFLAGS[@]}" -fpass-plugin="$PLUGIN" \
  "$SRC" -o "$TMP/obf"

REF="$("${LIMIT[@]}" "$TMP/ref")"
for run in 1 2 3 4 5; do
  OBF="$("${LIMIT[@]}" "$TMP/obf")"
  if [ "$REF" != "$OBF" ]; then
    echo "FAIL run=$run ref='$REF' obf='$OBF'" >&2
    exit 1
  fi
done

echo "OK FCO exception thread stress output=$REF"
