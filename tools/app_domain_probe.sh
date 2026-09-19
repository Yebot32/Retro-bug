#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Build, sign, install and run the app-domain probe, then print what the device said.
#
# This is the measurement that answers docs/10 U-2 (a 4 GiB low reservation) and U-13 (`execmem` and
# memfd-exec) for the domain RetroBug actually ships into. Both rows say, in their own words, that the
# answer they hold came from the `shell` domain — which is a different SEPolicy domain, so the answers
# are true but not the ones the design depends on. Getting them from a real installed app needed no
# emulator and no elevation, only a device attached, which is why this exists now.
#
# The build is the no-Gradle chain recorded as working in docs/20 §1: javac -> d8 -> aapt2 -> jar ->
# zipalign -> apksigner. It is spelled out here rather than delegated so that the whole path is
# auditable and runs on a machine with only the SDK and a JDK on it.
#
# Usage: tools/app_domain_probe.sh [serial]      # runs on every attached device when omitted
#        RB_BUILD_TOOLS=36.1.0 to pin build-tools
#
# Output is written to tests/appdomain/results-<model>.txt per device and echoed, so a claim in docs/10
# can always be traced to raw device output rather than to a transcription.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
ADB="$SDK/platform-tools/adb.exe"
BT="$SDK/build-tools/${RB_BUILD_TOOLS:-36.1.0}"
AJ="$SDK/platforms/android-36/android.jar"
SRC="$ROOT/tests/appdomain"
OUT="$ROOT/build/appdomain-probe"
PKG="com.retrobug.probe"

[ -x "$ADB" ] || { echo "no adb at $ADB" >&2; exit 1; }
[ -d "$BT" ] || { echo "no build-tools at $BT (RB_BUILD_TOOLS=<ver>)" >&2; exit 1; }
[ -f "$AJ" ] || { echo "no android.jar at $AJ" >&2; exit 1; }

to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/stage/lib/arm64-v8a"

# ---- native: arm64 only. The probe measures what an arm64 RetroBug process may do; a 32-bit build
# would answer for a domain that is itself the fallback case, and the stub it executes is arm64 code.
#
# `-static-libstdc++` because the default shared STL makes the library unloadable: the log said
# `dlopen failed: library "libc++_shared.so" not found: needed by librbprobe.so`, which is a link-time
# choice presenting as a runtime failure in a completely different place.
"$TC/bin/clang++.exe" --target=aarch64-none-linux-android26 --sysroot="$TC/sysroot" \
  -O2 -fPIC -shared -std=gnu++20 -static-libstdc++ -o "$OUT/stage/lib/arm64-v8a/librbprobe.so" \
  "$SRC/probe.cpp" -llog 2>&1 | sed 's/^/clang: /'
[ -f "$OUT/stage/lib/arm64-v8a/librbprobe.so" ] || { echo "native build failed" >&2; exit 1; }

# ---- java -> dex
JAVAC=javac; D8="$BT/d8.bat"; AAPT2="$BT/aapt2.exe"; ZIPALIGN="$BT/zipalign.exe"; SIGNER="$BT/apksigner.bat"
for t in "$AAPT2" "$ZIPALIGN"; do [ -e "$t" ] || { echo "missing $t" >&2; exit 1; }; done
command -v "$JAVAC" >/dev/null || { echo "no javac on PATH" >&2; exit 1; }

"$JAVAC" -nowarn -source 8 -target 8 -bootclasspath "$(to_host "$AJ")" \
  -d "$(to_host "$OUT/classes")" "$(to_host "$SRC/ProbeActivity.java")" 2>&1 \
  | grep -v "bootstrap class path" || true
find "$OUT/classes" -name '*.class' -print0 |
  xargs -0 "$D8" --lib "$(to_host "$AJ")" --min-api 26 --output "$(to_host "$OUT/dex")" >/dev/null

# ---- package: aapt2 produces the manifest+resources, jar adds the dex and the lib
"$AAPT2" link -I "$(to_host "$AJ")" --manifest "$(to_host "$SRC/AndroidManifest.xml")" \
  --min-sdk-version 26 --target-sdk-version 34 --version-code 1 --version-name 1 \
  -o "$(to_host "$OUT/base.apk")"
cp "$OUT/dex/classes.dex" "$OUT/stage/"
(cd "$OUT/stage" && jar -uf "$(to_host "$OUT/base.apk")" classes.dex lib)

# The keystore lives outside $OUT on purpose: $OUT is deleted at the start of every run, and a key that
# is regenerated each time makes the *next* install fail with
# `INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match newer version` — a rebuild appearing as
# a device problem.
KS="$ROOT/build/appdomain-probe-key.keystore"
[ -f "$KS" ] || keytool -genkeypair -noprompt -keystore "$(to_host "$KS")" -storepass android \
  -keypass android -alias probe -dname "CN=RetroBug Probe, OU=none, O=none, L=none, S=none, C=US" \
  -keyalg RSA -keysize 2048 -validity 3650 >/dev/null 2>&1
"$ZIPALIGN" -f -p 4 "$(to_host "$OUT/base.apk")" "$(to_host "$OUT/probe-aligned.apk")"
"$SIGNER" sign --ks "$(to_host "$KS")" --ks-pass pass:android --key-pass pass:android \
  --out "$(to_host "$OUT/probe.apk")" "$(to_host "$OUT/probe-aligned.apk")" >/dev/null
"$SIGNER" verify "$(to_host "$OUT/probe.apk")" && echo "apksigner verify: clean"

# ---- devices
mapfile -t DEVICES < <("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}')
if [ $# -ge 1 ]; then DEVICES=("$1"); fi
if [ "${#DEVICES[@]}" -eq 0 ]; then echo "no devices attached" >&2; exit 1; fi

status=0
# Every device step is bounded. An OEM installer can put a confirmation dialog on the screen ("Install
# via USB" on ColorOS, seen here on the CPH2573) and then `adb install` waits for a finger
# indefinitely — which on an unattended run is not a failure to report so much as a session that
# quietly stops doing anything. A bounded step names the device it gave up on and moves on.
#
# The bound has to live *inside* the function: `timeout 30 adb shell ...` fails with "No such file or
# directory", because `timeout` execs a program and `adb` here is a shell function.
for serial in "${DEVICES[@]}"; do
  adb() { local secs="$1"; shift; MSYS_NO_PATHCONV=1 timeout "$secs" "$ADB" -s "$serial" "$@"; }
  model="$(adb 30 shell getprop ro.product.model | tr -d '\r')"
  echo "=== $serial ($model) ==="
  if ! adb 180 install -r -g "$(to_host "$OUT/probe.apk")" >/dev/null 2>&1; then
    # The one recoverable install failure is a leftover package signed by a key that no longer exists
    # (an earlier run of this script, before the keystore was moved out of the wiped directory). Say so
    # rather than reporting a device fault, and retry once after removing it.
    echo "  install failed on $model; uninstalling the stale package and retrying once" >&2
    adb 60 uninstall "$PKG" >/dev/null 2>&1 || true
    if ! adb 180 install -r -g "$(to_host "$OUT/probe.apk")" 2>&1 | tail -2; then
      echo "  install failed twice on $model — if this device shows an install-confirmation dialog," >&2
      echo "  it needs a tap that no one is in front of. Skipping it, not failing the run." >&2
      status=1
      continue
    fi
  fi
  adb 30 logcat -c >/dev/null 2>&1 || true
  adb 30 shell am force-stop "$PKG" >/dev/null 2>&1 || true
  adb 60 shell am start -n "$PKG/.ProbeActivity" >/dev/null
  # The probe executes code it wrote, so a denial can take the process down: wait for the completion
  # line rather than for a fixed sleep, and keep whatever was printed either way.
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    if adb 20 logcat -d -s rbprobe:I 2>/dev/null | grep -q "probe complete"; then break; fi
  done
  report="$SRC/results-$model.txt"
  {
    echo "# device=$model serial=$serial"
    echo "# release=$(adb 20 shell getprop ro.build.version.release | tr -d '\r') sdk=$(adb 20 shell getprop ro.build.version.sdk | tr -d '\r') abilist32='$(adb 20 shell getprop ro.product.cpu.abilist32 | tr -d '\r')'"
    echo "# ran $(date -Is) with apk built from $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo 'uncommitted tree')"
    adb 30 logcat -d -s rbprobe:I | sed -n '/rbprobe/p' | sed 's/^.*rbprobe *: //'
    echo "# seccomp/avc denials visible to the shell domain, if any:"
    adb 40 logcat -d 2>/dev/null | grep -iE "avc: *denied" | grep -i "$PKG\|rbprobe" | head -20 || true
  } > "$report" 2>&1
  cat "$report"
  adb 30 shell am force-stop "$PKG" >/dev/null 2>&1 || true
done
exit $status
