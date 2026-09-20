#!/usr/bin/env bash
# RetroBug — run the vendored engine's own ARMv7 test suite. SPDX-License-Identifier: Apache-2.0
#
# Why this exists as a script: it is the cheapest large signal we have that the A32 front end is
# trustworthy, and it runs **without** Unicorn — dynarmic's DYNARMIC_TESTS_USE_UNICORN option
# defaults to OFF, so no GPL code is configured, built, or linked, which keeps the whole
# docs/10 §13.3 clean-room posture intact.
#
# Executed 2026-09-19 on three hosts, all passing 200,561 assertions:
#   * x86_64 Android 37.1 emulator   - 48 test cases
#   * arm64 phones (OnePlus 12, SM-X710 Tab S9) - 47 test cases
# The one-case difference is `tests/x64_cpu_info.cpp`, which dynarmic's own CMakeLists adds only
# `if ("x86_64" IN_LIST ARCHITECTURE)`. Its TEST_CASE prints xbyak's CPU-feature table and contains
# zero assertions, so the totals agree and the arm64 count is not a missing test. Checked against the
# CMakeLists rather than inferred from the counts.
#
# The arm64 runs are the ones that matter: Dynarmic's **AArch64** back end compiling A32 IR to real
# arm64 host code and executing it is what ships on the target. Getting there needed one shim fix -
# apply_visitor had to unwrap recursive_wrapper the way Boost does (docs/30-m0-notes.md §Step 5f) -
# and nothing in external/dynarmic was touched.
#
# Two limits to read before treating "green" as "verified":
#   1. It asserts what dynarmic chose to assert. Semantic gaps outside its test set (and its own
#      README's "FPSR state is approximate") remain open.
#   2. A correct A32 front end is not a correct RetroBug pipeline. The tiers that host it (`rb::exec`,
#      `rb::abi`) are separate components with separate tests, and no instruction of a real guest app
#      has been executed yet.
#
# Usage: tools/run_engine_tests.sh [abi]        # abi defaults to x86_64; arm64-v8a runs on a phone
# Pick the device with RB_SERIAL=<serial> when more than one is attached (`adb devices`).

set -euo pipefail
ABI="${1:-x86_64}"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tests-$ABI"

command -v "$SDK/cmake/3.31.6/bin/cmake.exe" >/dev/null 2>&1 && CM="$SDK/cmake/3.31.6/bin/cmake.exe" || CM=cmake
command -v "$SDK/cmake/3.31.6/bin/ninja.exe" >/dev/null 2>&1 && NJ="$SDK/cmake/3.31.6/bin/ninja.exe" || NJ=ninja
ADB="$SDK/platform-tools/adb.exe"

# One list for build, push and run: a suite that is missing here silently does not execute, which is
# the least useful kind of green.
SUITES="rb_interval_compat_tests rb_variant_compat_tests rb_guest_memory_tests rb_guest_elf_tests rb_isa_front_end_tests rb_exec_census_tests rb_exec_interp_tests rb_abi_call_tests rb_abi_jni_tests rb_exec_tier_diff_tests rb_diag_tests rb_syscall_monitor_tests rb_codecache_tests rb_profile_tests dynarmic_tests"
# Lanes that are red on purpose because every failure names a real gap (a Tier I refusal, an unlinked
# loader step), so they cannot gate a green run. They are still built and run here: before this list
# existed they were in no build at all, which let a stale binary report the same failure count for hours
# and make a fix that had changed everything look like it had changed nothing.
ADVISORY="rb_guest_libc_tests rb_guest_apk_tests"

"$CM" -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NJ" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/$NDK_VER/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_TESTS=ON -DDYNARMIC_TESTS_USE_UNICORN=OFF -DRB_BUILD_TESTS=ON >/dev/null
"$NJ" -C "$BUILD" $SUITES $ADVISORY

# The armeabi-v7a fixture. Built for the guest ISA, not the test host's: the loader and the
# execution tiers have to consume real ARMv7 bytes, and these are the same bytes M1 launches on
# the phone (docs/21 §3).
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
FIXTURE="$BUILD/librb_fixture.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -Os \
  -o "$FIXTURE" "$ROOT/tests/corpus/fixture_jni.c" -llog

# MSYS hands native programs /c/... paths and converts them on the way out — which is wrong for the
# *device* path in `adb push a /data/local/tmp/b`, hence the MSYS_NO_PATHCONV=1 on every adb call
# below. That switch also stops the conversion of the **local** path, so adb.exe is then handed a
# /c/... path it cannot open and the push fails on a file that plainly exists. Convert this one
# explicitly; on a non-Windows runner (CI) there is nothing to convert.
to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

# Which device, and said out loud.
#
# Two are attached in normal use now: the SM-X710 (armeabi-v7a, so it can *execute* the guest ISA and
# is the oracle) and the OnePlus 12 (abilist32 empty, so it is M1's negative control). `adb` refuses to
# guess when more than one is present, and a silently-chosen device would be worse than an error: a
# green run means "green *on this hardware*", which is the whole content of the claim (docs/30's
# standing facts make the same point about attributing a run to a device).
SERIAL="${RB_SERIAL:-}"
if [ -z "$SERIAL" ]; then
  n="$("$ADB" devices | awk 'NR>1 && $2=="device"' | wc -l | tr -d ' ')"
  if [ "$n" = "1" ]; then
    SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}')"
  elif [ "$n" = "0" ]; then
    echo "no device attached (adb devices is empty)" >&2
    exit 1
  else
    echo "$n devices attached; name one with RB_SERIAL=<serial>" >&2
    "$ADB" devices | awk 'NR>1 && $2=="device"' >&2
    exit 1
  fi
fi
adb() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }

adb wait-for-device
# Device paths must not be rewritten by MSYS, and adb push clears the execute bit — both bit us once
# already (docs/30-m0-notes.md).
{
  echo "# device=$SERIAL model=$(adb shell getprop ro.product.model | tr -d '\r')" \
       "abilist32=$(adb shell getprop ro.product.cpu.abilist32 | tr -d '\r')" \
       "sdk=$(adb shell getprop ro.build.version.sdk | tr -d '\r')" \
       "page=$(adb shell getconf PAGE_SIZE | tr -d '\r')"
} >&2

adb push "$(to_host "$FIXTURE")" /data/local/tmp/librb_fixture.so >/dev/null
for t in $SUITES $ADVISORY; do
  src="$BUILD/core/tests/$t"
  [ -f "$src" ] || src="$BUILD/external/dynarmic/tests/$t"
  [ -f "$src" ] || { echo "no binary for $t under $BUILD" >&2; exit 1; }
  adb push "$(to_host "$src")" "/data/local/tmp/$t" >/dev/null
done

status=0
for t in $SUITES; do
  echo "=== $t ==="
  adb shell "chmod 755 /data/local/tmp/$t; /data/local/tmp/$t" || status=1
done
# Reported, not gated: these lanes carry named gaps, and a green engine must not hide them either. The
# `|| true` is load-bearing — under `set -o pipefail` a lane that exits non-zero to say "red" would
# otherwise abort the script before the next one ran, which is gating with extra steps.
for t in $ADVISORY; do
  echo "=== $t (advisory) ==="
  { adb shell "chmod 755 /data/local/tmp/$t; /data/local/tmp/$t" || true; } 2>&1
done
exit $status
