#!/usr/bin/env bash
# RetroBug — capture the native pthread oracle for M2 Task 4. SPDX-License-Identifier: Apache-2.0
#
# Builds tests/corpus/pthread_fixture.c as libpthread_fixture.so (the same flags tools/run_engine_tests.sh
# uses for the guest copy the engine lane loads) and tests/guest/pthread_oracle.c linked against it, both
# for armeabi-v7a, runs the driver natively on a 32-bit-capable device (R52Y106BECZ) against the device's
# own /system/lib/libc.so, and writes tests/corpus/oracle-pthread.txt — committed, so the numbers the
# tiers are asserted against are raw device output with their provenance attached.
#
# Usage: RB_SERIAL=R52Y106BECZ bash tools/run_pthread_oracle.sh

set -euo pipefail
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/corpus/oracle-pthread.txt"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
ADB="$SDK/platform-tools/adb.exe"
BUILD="$ROOT/build/pthread-oracle"
mkdir -p "$BUILD"

# The fixture: exactly the engine lane's build line (keep the two in step).
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,libpthread_fixture.so -o "$BUILD/libpthread_fixture.so" "$ROOT/tests/corpus/pthread_fixture.c" -lc
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -Os \
  -o "$BUILD/pthread_oracle" "$ROOT/tests/guest/pthread_oracle.c" -L"$BUILD" -lpthread_fixture

to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

SERIAL="${RB_SERIAL:-}"
if [ -z "$SERIAL" ]; then
  n="$("$ADB" devices | awk 'NR>1 && $2=="device"' | wc -l | tr -d ' ')"
  if [ "$n" = "1" ]; then
    SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}')"
  else
    echo "$n devices attached; name the 32-bit-capable one with RB_SERIAL=<serial>" >&2
    exit 1
  fi
fi
adb() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }

adb wait-for-device
abilist32="$(adb shell getprop ro.product.cpu.abilist32 | tr -d '\r')"
model="$(adb shell getprop ro.product.model | tr -d '\r')"
if [ -z "$abilist32" ]; then
  echo "$model ($SERIAL) advertises no 32-bit ABI: it cannot run the oracle. Use R52Y106BECZ." >&2
  exit 2
fi

DIR=/data/local/tmp/rb_pthread_oracle
adb shell "mkdir -p $DIR" >/dev/null
adb push "$(to_host "$BUILD/libpthread_fixture.so")" "$DIR/libpthread_fixture.so" >/dev/null
adb push "$(to_host "$BUILD/pthread_oracle")" "$DIR/pthread_oracle" >/dev/null
rows="$(adb shell "chmod 755 $DIR/pthread_oracle; LD_LIBRARY_PATH=$DIR $DIR/pthread_oracle; echo rc=\$?" | tr -d '\r')"
case "$rows" in
  *rc=0*) ;;
  *) echo "the oracle did not exit 0:" >&2; echo "$rows" >&2; exit 1 ;;
esac
{
  echo "# device=$model serial=$SERIAL abilist32=$abilist32 sdk=$(adb shell getprop ro.build.version.sdk | tr -d '\r')" \
       "libc_md5=$(adb shell md5sum /system/lib/libc.so | awk '{print $1}' | tr -d '\r')"
  echo "# built from $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo 'uncommitted tree') $(date -Is)"
  echo "$rows" | grep -v '^rc='
  # The process-ending row: exit_group from a thread while the caller is parked in pthread_join.
  eg="$(adb shell "LD_LIBRARY_PATH=$DIR $DIR/pthread_oracle exit_group 7; echo rc=\$?" | tr -d '\r' | sed -n 's/^rc=//p')"
  echo "pt_exit_group_from_thread(7):rc=$eg"
} > "$OUT"
cat "$OUT"
