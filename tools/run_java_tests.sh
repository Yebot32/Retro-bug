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
  # A fixture with res/ gets real resources (so resources.arsc has a package chunk, as a game's does).
  RES=()
  if [ -d "$dir/res" ]; then
    "$BT/aapt2.exe" compile --dir "$(to_host "$dir/res")" -o "$(to_host "$OUT/fixtures/$name.res.zip")"
    RES=("$(to_host "$OUT/fixtures/$name.res.zip")")
  fi
  "$BT/aapt2.exe" link -I "$(to_host "$AJ")" --manifest "$(to_host "$dir/AndroidManifest.xml")" \
    -o "$(to_host "$OUT/fixtures/$name.apk")" ${RES[@]+"${RES[@]}"}
done

# Never trust a piped exit status: the source list is built into an array first, and javac runs bare.
mapfile -t SRCS < <(grep -L 'import android\.' "$ROOT"/android/java/com/retrobug/launcher/*.java)
SRCS+=("$ROOT"/tools/java-tests/*.java "$ROOT"/android/shim/com/retrobug/shim/SaveArchive.java "$ROOT"/android/shim/com/retrobug/shim/PermissionPolicy.java)
HOST_SRCS=(); for s in "${SRCS[@]}"; do HOST_SRCS+=("$(to_host "$s")"); done
javac -encoding UTF-8 -nowarn -Xlint:-options -d "$(to_host "$OUT/classes")" "${HOST_SRCS[@]}"

# DEX fixtures for the risk scan: real javac + d8 output, as in a game's classes.dex. Compiled against
# android.jar so the references are the platform's own descriptors; never loaded or run.
for f in RiskyFixture CleanFixture NativeFixture JniFixture; do
  mkdir -p "$OUT/dexsrc/$f" "$OUT/dexout/$f"
  javac -encoding UTF-8 -nowarn -Xlint:-options --release 8 -classpath "$(to_host "$AJ")" \
    -d "$(to_host "$OUT/dexsrc/$f")" "$(to_host "$ROOT/tools/java-tests/dexfixtures/$f.java")"
  (cd "$OUT/dexsrc/$f" && jar -cf "$(to_host "$OUT/dexsrc/$f.jar")" .)
  "$BT/d8.bat" --lib "$(to_host "$AJ")" --min-api 24 --output "$(to_host "$OUT/dexout/$f")" \
    "$(to_host "$OUT/dexsrc/$f.jar")" >/dev/null
  cp "$OUT/dexout/$f/classes.dex" "$OUT/fixtures/$f.dex"
done

# A real 32-bit library with Java_rb_Fixture_* exports, for the generated-stub rows (skipped without the NDK).
NDK_BIN="$SDK/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin"
FX32="$OUT/fixtures/libjnibridge_fixture.so"
"$NDK_BIN/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$(to_host "$NDK_BIN/../sysroot")" -mthumb \
  -mfloat-abi=softfp -fPIC -shared -nostdlib -Os -fno-vectorize -fno-slp-vectorize \
  -o "$(to_host "$FX32")" "$(to_host "$ROOT/tests/corpus/jni_bridge_fixture.c")" -lc 2>/dev/null || FX32=""

java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ShimPlanTest "$(to_host "$OUT/fixtures")"

java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.AxmlEditorTest \
  "$(to_host "$OUT/fixtures")" "$(to_host "$OUT/scratch")" "$(to_host "$BT/aapt2.exe")"

java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ApkRepackagerTest \
  "$(to_host "$OUT/fixtures")" "$(to_host "$OUT/scratch")" "$(to_host "$BT/aapt2.exe")"

# Generated stubs (M3 Task 5). The game rows read the human's own Gumball / ABGo files from build/games
# (gumball-lite.apk, angry-birds-go-1.10.1.apk; never committed), read in memory, and skip without them.
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.NativeMethodsTest \
  "$(to_host "$OUT/fixtures")" "$(to_host "$ROOT/build/games")"
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.StubElfWriterTest \
  "$(to_host "$OUT/scratch")" "$(to_host "$NDK_BIN/llvm-readelf.exe")" "${FX32:+$(to_host "$FX32")}"

# The Holo launcher's pure parts (docs/superpowers/plans/2026-10-18-holo-launcher-ui.md).
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ApkScanTest "$(to_host "$OUT/scratch")"
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ArtPickerTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.GameSettingsTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.LibraryRulesTest
# The Data tab: OBB names for the copy, and the copy's save export/import.
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ObbRulesTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ElfRepairTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.shim.SaveArchiveTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.shim.PermissionPolicyTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.PatchTest
java -cp "$(to_host "$OUT/classes")" com.retrobug.launcher.ModTest
