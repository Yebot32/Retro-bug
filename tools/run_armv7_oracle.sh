#!/usr/bin/env bash
# RetroBug — capture the ARMv7 hardware oracle for M0's exit test. SPDX-License-Identifier: Apache-2.0
#
# Builds tests/corpus/oracle_armv7.c against the fixture it must mirror, for armeabi-v7a, and runs it
# on whatever 32-bit-capable Android device is attached. See that file's header for why hardware is
# the right source for these numbers; tools/run_engine_tests.sh is the lane for everything that does
# not need AArch32.
#
# The device swap is the reason this is a script: a 64-bit-only phone answers `./oracle` with
# "not executable: 32-bit ELF file" (ENOEXEC, docs/21 §E1), so the same command silently does nothing
# useful on half the fleet. Check `adb shell getprop ro.product.cpu.abilist32` first and say so loudly
# instead of reporting a missing file.
#
# Usage: tools/run_armv7_oracle.sh   # rewrites tests/corpus/oracle-armv7.txt and echoes it
#        RB_SERIAL=<serial> when more than one device is attached (`adb devices`).
#
# The output is committed on purpose: the numbers RetroBug's tiers are asserted against should be
# auditable as raw device output, with the model, ABI list, page size and source revision attached,
# rather than existing only as a transcription in a doc.

set -euo pipefail
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/corpus/oracle-armv7.txt"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
ADB="$SDK/platform-tools/adb.exe"
BIN="$ROOT/build/oracle_armv7"

mkdir -p "$(dirname "$OUT")"
# Same flags as the fixture's shared object, so the code under oracle is the code M0 executes.
# -static because a device with 32-bit runtime may still have no 32-bit libc.so to link against at
# launch, and an oracle that fails to start is not an oracle. No -llog: with -static there is no
# liblog.a to link, and neither file here logs anything.
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -Os -static \
  -o "$BIN" "$ROOT/tests/corpus/oracle_armv7.c" "$ROOT/tests/corpus/fixture_jni.c"

# MSYS_NO_PATHCONV is needed for the device path and blocks conversion of the local one; adb.exe
# cannot open a /c/... path. (Same trap as tools/run_engine_tests.sh.)
to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

# Which device, named in the output. Two are attached in normal use, and `adb` refuses to guess; worse,
# guessing wrong here would be silent, because the numbers would still come out of *an* ARMv7 core. This
# file is the evidence M0 asserts against, so its provenance carries the serial next to the model
# (same rule as tools/run_engine_tests.sh, and the reason the SM-X710 is the one that gets named).
SERIAL="${RB_SERIAL:-}"
if [ -z "$SERIAL" ]; then
  n="$("$ADB" devices | awk 'NR>1 && $2=="device"' | wc -l | tr -d ' ')"
  if [ "$n" = "1" ]; then
    SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}')"
  else
    echo "$n devices attached; name the 32-bit-capable one with RB_SERIAL=<serial>" >&2
    "$ADB" devices | awk 'NR>1 && $2=="device"' >&2
    exit 1
  fi
fi
adb() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }

adb wait-for-device
abilist32="$(adb shell getprop ro.product.cpu.abilist32 | tr -d '\r')"
model="$(adb shell getprop ro.product.model | tr -d '\r')"
if [ -z "$abilist32" ]; then
  echo "$model advertises no 32-bit ABI (abilist32 empty): this device cannot be the oracle." >&2
  echo "That is docs/21 §E1's positive case, not a failure here — attach an arm64 device whose" >&2
  echo "abilist32 is populated (the SM-X710 Tab S9 is) and re-run." >&2
  exit 2
fi

adb push "$(to_host "$BIN")" /data/local/tmp/oracle_armv7 >/dev/null
{
  echo "# device=$model serial=$SERIAL abilist32=$abilist32 page=$(adb shell getconf PAGE_SIZE | tr -d '\r')"
  echo "# built from $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo 'uncommitted tree') $(date -Is)"
  echo "--- numeric oracle"
  adb shell "chmod 755 /data/local/tmp/oracle_armv7; /data/local/tmp/oracle_armv7; echo rc=\$?"
  # One process per CP15 register: a refused read raises SIGILL and takes the process with it, so a
  # single loop would report only the first refusal. rc=132 is SIGILL.
  echo "--- cp15 reads reachable from an AArch32 EL0 process (rc=132 means SIGILL)"
  for reg in midr ctr mpidr tpidrurw fpsid; do
    adb shell "/data/local/tmp/oracle_armv7 --cp15 $reg; echo rc=\$?"
  done
} > "$OUT" 2>&1
cat "$OUT"
