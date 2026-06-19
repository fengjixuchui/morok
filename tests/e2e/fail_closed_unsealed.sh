#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Regression for #106 (fail-closed-on-unsealed).  Build a protected binary with
# the fail_closed_on_unsealed knob and assert it is NON-FUNCTIONAL when the
# post-link seal step is skipped (the developer forgot to seal), yet runs
# normally once sealed.  A control build WITHOUT the knob must run fine while
# unsealed, proving the death is caused by the knob — not the fixture or preset.
#
# Usage:
#   fail_closed_unsealed.sh <python3> <clang> <plugin> <sdk> <source> [seed]
set -uo pipefail

PYTHON="$1"
CLANG="$2"
PLUGIN="$3"
SDK="$4"
SRC="$5"
SEED="${6:-4242}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
TOOL="$ROOT/adversarial_binary.py"

if [ -z "$SDK" ] && command -v xcrun >/dev/null 2>&1; then
  SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi
SYSROOT=()
[ -n "$SDK" ] && SYSROOT=(-isysroot "$SDK")

if command -v timeout >/dev/null 2>&1; then LIMIT=(timeout)
elif command -v gtimeout >/dev/null 2>&1; then LIMIT=(gtimeout)
else LIMIT=(); fi

run_limited() {
  local secs="$1"; shift
  if [ "${#LIMIT[@]}" -gt 0 ]; then
    "${LIMIT[@]}" "$secs" "$@"
  else
    "$@"
  fi
}

resign_if_needed() {
  local exe="$1"
  if [ "$(uname -s)" = "Darwin" ]; then
    /usr/bin/codesign --force --sign - "$exe" >/dev/null 2>&1 || {
      echo "FAIL codesign $exe" >&2
      return 1
    }
  fi
}

# Run after (re)signing so a failure is the fail-closed corruption, not a
# missing/invalid code signature.  Returns the process exit status.
run_rc() {
  local exe="$1"
  resign_if_needed "$exe" || return 99
  run_limited 30 "$exe" >/dev/null 2>&1
  echo "$?"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

STRICT_CFG="$TMP/strict.toml"
printf '[global]\npreset = "high"\n\n[passes]\nfail_closed_on_unsealed = true\n' \
  >"$STRICT_CFG"
CONTROL_CFG="$TMP/control.toml"
printf '[global]\npreset = "high"\n' >"$CONTROL_CFG"

compile() { # <out> <config>
  env MOROK_ENABLE=1 MOROK_CONFIG="$2" MOROK_SEED="$SEED" \
    "$CLANG" "${SYSROOT[@]}" -O2 -std=c11 -D_GNU_SOURCE \
    -fpass-plugin="$PLUGIN" "$SRC" -o "$1" >"$1.log" 2>&1
}

STRICT="$TMP/strict"
CONTROL="$TMP/control"

if ! compile "$STRICT" "$STRICT_CFG"; then
  echo "FAIL strict compile" >&2; tail -120 "$STRICT.log" >&2; exit 1
fi
if ! compile "$CONTROL" "$CONTROL_CFG"; then
  echo "FAIL control compile" >&2; tail -120 "$CONTROL.log" >&2; exit 1
fi

# Control: a non-fail-closed build must run fine while unsealed (today's
# fail-SAFE behaviour).  If it does not, the fixture/preset is the problem and
# the strict assertion below would be meaningless.
control_rc="$(run_rc "$CONTROL")"
if [ "$control_rc" != "0" ]; then
  echo "SKIP control (non-strict) build did not run unsealed rc=$control_rc" >&2
  exit 77
fi

# Strict + UNSEALED: forgetting the seal must make the binary non-functional.
strict_unsealed_rc="$(run_rc "$STRICT")"
if [ "$strict_unsealed_rc" = "0" ]; then
  echo "FAIL strict build ran successfully while UNSEALED (fail-closed not enforced)" >&2
  exit 1
fi

# Strict + SEALED: sealing must make the same binary run normally (the
# corruption is bound only to the unsealed sentinel; sealed builds unaffected).
if ! seal_log="$("$PYTHON" "$TOOL" seal "$STRICT" --window 262144 2>&1)"; then
  echo "FAIL post-link seal produced no integrity manifests" >&2
  printf '%s\n' "$seal_log" >&2
  exit 1
fi
strict_sealed_rc="$(run_rc "$STRICT")"
if [ "$strict_sealed_rc" != "0" ]; then
  echo "FAIL strict build did not run after sealing (bricked a sealed binary) rc=$strict_sealed_rc" >&2
  exit 1
fi

echo "OK fail-closed-on-unsealed: control_unsealed=$control_rc strict_unsealed=$strict_unsealed_rc strict_sealed=$strict_sealed_rc"
echo "   $seal_log"
exit 0
