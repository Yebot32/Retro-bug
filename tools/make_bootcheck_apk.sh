#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Make a "bootcheck" APK: a copy of a 32-bit game's APK with its libraries laid out the way the M1
# repackager will lay them out, so the in-process boot (`rb_runtime_boot` in librbguest.so) can be checked
# from the launcher before the repackager exists.
#
#   lib/armeabi-v7a/*.so                         -> assets/rb32/<name>.so     (the game's 32-bit libraries)
#   build/device-libs/{libc,libstdc++}.so        -> assets/rb32sys/            (the device's own bionic)
#   build/guest-libs/{libm,libz}.so              -> assets/rb32sys/
#
# Zip level only: nothing is removed, re-signed or aligned. The output is never installed — it is staged
# into the launcher's games dir through the "Add a game" picker and read as a file — so its now-invalid
# signature does not matter. The input is usually a copyrighted game: never commit the output.
#
# Usage: tools/make_bootcheck_apk.sh <in.apk> <out.apk>

set -euo pipefail
IN="${1:?usage: make_bootcheck_apk.sh <in.apk> <out.apk>}"
OUT="${2:?usage: make_bootcheck_apk.sh <in.apk> <out.apk>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
to_host() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

[ -s "$IN" ] || { echo "no input APK at $IN" >&2; exit 1; }
command -v unzip >/dev/null || { echo "no unzip on PATH" >&2; exit 1; }
command -v jar >/dev/null || { echo "no jar on PATH (the JDK's is fine)" >&2; exit 1; }
SYS=("$ROOT/build/device-libs/libc.so" "$ROOT/build/device-libs/libstdc++.so"
     "$ROOT/build/guest-libs/libm.so" "$ROOT/build/guest-libs/libz.so")
for f in "${SYS[@]}"; do
  [ -s "$f" ] || { echo "missing $f — pull the device's 32-bit library or run tools/build-guest-libs.sh" >&2; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/x" "$STAGE/s/assets/rb32" "$STAGE/s/assets/rb32sys"
unzip -q -o "$IN" 'lib/armeabi-v7a/*.so' -d "$STAGE/x" 2>/dev/null || true
shopt -s nullglob
libs=("$STAGE/x/lib/armeabi-v7a/"*.so)
[ "${#libs[@]}" -gt 0 ] || { echo "$IN has no lib/armeabi-v7a/*.so entry" >&2; exit 1; }
for l in "${libs[@]}"; do cp "$l" "$STAGE/s/assets/rb32/"; done
for f in "${SYS[@]}"; do cp "$f" "$STAGE/s/assets/rb32sys/"; done

mkdir -p "$(dirname "$OUT")"
cp "$IN" "$OUT"
OUT_HOST="$(to_host "$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")")"
(cd "$STAGE/s" && jar -uMf "$OUT_HOST" assets/rb32 assets/rb32sys)
echo "built $OUT:"
unzip -l "$OUT" | awk '$4 ~ /^assets\/rb32(sys)?\//{printf "  %s  %s\n", $4, $1}'
