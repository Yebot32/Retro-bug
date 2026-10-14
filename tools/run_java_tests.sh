#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Host tests for the launcher's pure-Java parts (the ones that import nothing from `android.*`).
#
# Why on the PC and not on a phone: the repackager rewrites a game's binary manifest, and the cheapest
# place to find out it wrote bytes the platform rejects is `aapt2` here, not an install failure on a
# device. The fixtures are real aapt2 output — each manifest under tests/corpus/manifests is built with
# `aapt2 link -I android.jar` (so a resources.arsc exists, as in a real game) — so the editor is tested
# against what the toolchain emits instead of against bytes this repo made up.
#
# Only sources without `import android.` are compiled: LauncherActivity needs the framework and has no
# host test; anything that sneaks an android import into a host-testable class fails to compile here
# rather than passing and breaking on a device.
#
# Usage: tools/run_java_tests.sh          # RB_BUILD_TOOLS=<ver> pins build-tools

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
BT="$SDK/build-tools/${RB_BUILD_TOOLS:-37.0.0}"
AJ="$SDK/platforms/android-36/android.jar"
OUT="$ROOT/build/java-tests"

to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

for t in "$BT/aapt2.exe" "$AJ"; do
  [ -e "$t" ] || { echo "run_java_tests: missing $t" >&2; exit 2; }
done

rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/fixtures" "$OUT/scratch"

for dir in "$ROOT"/tests/corpus/manifests/*/; do
  name="$(basename "$dir")"
  "$BT/aapt2.exe" link -I "$(to_host "$AJ")" --manifest "$(to_host "$dir/AndroidManifest.xml")" \
    -o "$(to_host "$OUT/fixtures/$name.apk")"
done

# Never trust a piped exit status: the source list is built into an array first, and javac runs bare.
mapfile -t SRCS < <(grep -L 'import android\.' "$ROOT"/android/java/com/retrobug/launcher/*.java)
SRCS+=("$ROOT"/tools/java-tests/*.java)
HOST_SRCS=(); for s in "${SRCS[@]}"; do HOST_SRCS+=("$(to_host "$s")"); done
javac -nowarn -Xlint:-options -d "$(to_host "$OUT/classes")" "${HOST_SRCS[@]}"

java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.AxmlEditorTest \
  "$(to_host "$OUT/fixtures")" "$(to_host "$OUT/scratch")" "$(to_host "$BT/aapt2.exe")"
