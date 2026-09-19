#!/usr/bin/env bash
# RetroBug — run the vendored engine's own ARMv7 test suite. SPDX-License-Identifier: Apache-2.0
#
# Why this exists as a script: it is the cheapest large signal we have that the A32 front end is
# trustworthy, and it runs **without** Unicorn — dynarmic's DYNARMIC_TESTS_USE_UNICORN option
# defaults to OFF, so no GPL code is configured, built, or linked, which keeps the whole
# docs/10 §13.3 clean-room posture intact.
#
# Executed 2026-09-19 on an x86_64 Android 37.1 emulator: 200,561 assertions in 48 test cases,
# all passing — including A32 SVC and coprocessor tests, which are the paths RetroBug leans on
# hardest (guest kernel entry and CP15/TLS register access).
#
# Two limits to read before treating "green" as "verified":
#   1. This build exercises the **x64** backend for A32. The AArch64 backend — what Android on
#      real arm64 hardware actually uses — is not covered here, and running an arm64 build under
#      the emulator's arm64→x86 translation would not be representative of a JIT. That lane
#      belongs on real arm64 hardware or CI runners (docs/10 §17, U-14).
#   2. It asserts what dynarmic chose to assert. Semantic gaps outside its test set (and its own
#      README's "FPSR state is approximate") remain open.
#
# Usage: tools/run_dynarmic_tests.sh [abi]        # abi defaults to x86_64

set -euo pipefail
ABI="${1:-x86_64}"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tests-$ABI"

command -v "$SDK/cmake/3.31.6/bin/cmake.exe" >/dev/null 2>&1 && CM="$SDK/cmake/3.31.6/bin/cmake.exe" || CM=cmake
command -v "$SDK/cmake/3.31.6/bin/ninja.exe" >/dev/null 2>&1 && NJ="$SDK/cmake/3.31.6/bin/ninja.exe" || NJ=ninja
ADB="$SDK/platform-tools/adb.exe"

"$CM" -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NJ" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/$NDK_VER/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_TESTS=ON -DDYNARMIC_TESTS_USE_UNICORN=OFF -DRB_BUILD_TESTS=ON >/dev/null
"$NJ" -C "$BUILD" dynarmic_tests rb_interval_compat_tests rb_guest_memory_tests rb_guest_elf_tests

# The armeabi-v7a fixture. Built for the guest ISA, not the test host's: the loader and the
# execution tiers have to consume real ARMv7 bytes, and these are the same bytes M1 launches on
# the phone (docs/21 §3).
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
FIXTURE="$BUILD/librb_fixture.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -Os \
  -o "$FIXTURE" "$ROOT/tests/corpus/fixture_jni.c" -llog

BIN="$BUILD/external/dynarmic/tests/dynarmic_tests"
[ -f "$BIN" ] || { echo "build produced no binary at $BIN" >&2; exit 1; }

"$ADB" wait-for-device
# Device paths must not be rewritten by MSYS, and adb push clears the execute bit — both bit us
# once already (docs/30-m0-notes.md).
for spec in "$FIXTURE:/data/local/tmp/librb_fixture.so" \
            "$BUILD/core/tests/rb_interval_compat_tests:/data/local/tmp/rb_interval_compat_tests" \
            "$BUILD/core/tests/rb_guest_memory_tests:/data/local/tmp/rb_guest_memory_tests" \
            "$BUILD/core/tests/rb_guest_elf_tests:/data/local/tmp/rb_guest_elf_tests" \
            "$BIN:/data/local/tmp/dynarmic_tests"; do
  MSYS_NO_PATHCONV=1 "$ADB" push "${spec%%:*}" "${spec##*:}" >/dev/null
done

status=0
for t in rb_interval_compat_tests rb_guest_memory_tests rb_guest_elf_tests dynarmic_tests; do
  echo "=== $t ==="
  MSYS_NO_PATHCONV=1 "$ADB" shell "chmod 755 /data/local/tmp/$t; /data/local/tmp/$t" || status=1
done
exit $status
