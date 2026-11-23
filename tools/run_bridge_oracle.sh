#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# The JNI bridge test app's native oracle (M3 Task 6): installs build/jnibridge/jnibridge.apk (tools/build_bridge_apk.sh)
# as is on a 32-bit-capable device, where the platform runs its armeabi-v7a library natively under the device's
# own ART, starts it, and writes every `BRIDGE: <row>=<value>` line to tests/corpus/oracle-jnibridge.txt with the
# device and libc it came from. Committed, so tools/run_bridge_check.sh compares a bridged run against real answers.
# Afterwards the native copy is uninstalled: the bridged copy installs under the same package name. (The app is
# ours — com.retrobug.jnibridge — never one of the human's installed games.)
#
# Usage: RB_SERIAL=<serial> tools/run_bridge_oracle.sh      (default R52Y106BECZ, the 32-bit-capable tablet)

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
ADB="$SDK/platform-tools/adb.exe"
SERIAL="${RB_SERIAL:-R52Y106BECZ}"
APK="$ROOT/build/jnibridge/jnibridge.apk"
OUT="$ROOT/tests/corpus/oracle-jnibridge.txt"
PKG=com.retrobug.jnibridge
a() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }

[ -s "$APK" ] || { echo "no $APK — run tools/build_bridge_apk.sh first" >&2; exit 1; }
[ -n "$(a shell getprop ro.product.cpu.abilist32 | tr -d '\r')" ] || {
  echo "$SERIAL has no 32-bit ABI: it cannot run the oracle natively" >&2; exit 1; }

a uninstall "$PKG" >/dev/null 2>&1 || true  # a repacked copy from run_bridge_check.sh has another signature
a install "$(cygpath -w "$APK")" >/dev/null
a logcat -c
a shell am start -S -W -n "$PKG/.BridgeActivity" >/dev/null
rows=""
for _ in $(seq 1 60); do
  sleep 1
  rows="$(a logcat -d -s BRIDGE:I | sed -n 's/.* BRIDGE *: //p' | tr -d '\r')"
  printf '%s\n' "$rows" | grep -q '^done ' && break
done
a shell am force-stop "$PKG"
printf '%s\n' "$rows" | grep -q '^done ' || { echo "no 'BRIDGE: done' within 60 s; got:" >&2; printf '%s\n' "$rows" >&2; exit 1; }

{
  echo "# JNI bridge oracle — com.retrobug.jnibridge run natively (tools/run_bridge_oracle.sh)."
  echo "# device: $(a shell getprop ro.product.model | tr -d '\r') ($SERIAL), $(a shell getprop ro.build.fingerprint | tr -d '\r')"
  echo "# libc: /system/lib/libc.so sha256 $(a shell sha256sum /system/lib/libc.so | awk '{print $1}')"
  echo "# app: libjnibridge.so sha256 $(unzip -p "$APK" lib/armeabi-v7a/libjnibridge.so | sha256sum | awk '{print $1}')"
  printf '%s\n' "$rows"
} > "$OUT"
a uninstall "$PKG" >/dev/null || true
echo "wrote $OUT:"
cat "$OUT"
