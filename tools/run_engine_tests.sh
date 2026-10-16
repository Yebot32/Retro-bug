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
SUITES="rb_interval_compat_tests rb_variant_compat_tests rb_guest_memory_tests rb_guest_elf_tests rb_linker_tests rb_isa_front_end_tests rb_exec_census_tests rb_exec_interp_tests rb_abi_call_tests rb_abi_jni_tests rb_exec_tier_diff_tests rb_diag_tests rb_syscall_monitor_tests rb_codecache_tests rb_profile_tests rb_guest_process_tests rb_guest_thread_tests rb_jni_forward_tests rb_inbound_tests dynarmic_tests"
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
# The same image without a `.symtab`, for `GuestModule::FindLocalSymbol`'s "nothing to read" case: the
# fixture as linked carries one, so the stripped copy is made here rather than assumed.
FIXTURE_STRIPPED="$BUILD/librb_fixture_stripped.so"
"$TC/bin/llvm-strip.exe" --strip-all -o "$FIXTURE_STRIPPED" "$FIXTURE"

# The guest images the libc/process lanes boot. None is made here: the device's own bionic is pulled off a
# 32-bit-capable phone (`build/device-libs/`, not in git) and libm/libz come from tools/build-guest-libs.sh.
# These pushes used to be manual (a stale or missing copy on the device was a lane failing for a reason
# the tree could not see). An input a *gating* lane needs is a refusal that names the command that makes
# it; an input only an advisory lane needs is a warning, and that lane is skipped rather than run against
# whatever stale copy the device may still hold.
DEVICE_LIBC="$ROOT/build/device-libs/libc.so"
GUEST_LIBM="$ROOT/build/guest-libs/libm.so"
GUEST_LIBZ="$ROOT/build/guest-libs/libz.so"
GUEST_APK="$ROOT/build/guest-apk/rb_guestprobe.apk"
for spec in "$DEVICE_LIBC|adb pull /system/lib/libc.so build/device-libs/libc.so (from a 32-bit-capable device)" \
            "$GUEST_LIBM|bash tools/build-guest-libs.sh libm" "$GUEST_LIBZ|bash tools/build-guest-libs.sh libz"; do
  in="${spec%%|*}"
  [ -s "$in" ] || { echo "missing $in — this script does not create it. Run: ${spec##*|}" >&2; exit 1; }
done
# Only the advisory rb_guest_apk_tests cannot run without the probe APK (rb_guest_libc_tests reads it
# optionally and says so when it is absent).
if [ ! -s "$GUEST_APK" ]; then
  echo "WARNING: missing $GUEST_APK (run: bash tools/build-guest-apk.sh) — skipping the advisory" \
       "rb_guest_apk_tests" >&2
  ADVISORY="${ADVISORY/rb_guest_apk_tests/}"
fi

# C06's fixture: two libraries with a real DT_NEEDED edge and a constructor each, built `-nostdlib` so
# the chain under test is two modules and not libc wearing a hat. `-Wl,-soname` is what makes the
# dependent's DT_NEEDED read `librbleaf.so`, which is the string the linker is asked to place.
LEAF="$BUILD/librbleaf.so"
MID="$BUILD/librbmid.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbleaf.so -o "$LEAF" "$ROOT/tests/corpus/link_leaf.c"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbmid.so -o "$MID" "$ROOT/tests/corpus/link_mid.c" -L"$BUILD" -lrbleaf

# The cross-module IFUNC pair, built the same way: a leaf whose IFUNC its own image never relocates
# against (so only an importer can need it resolved) and the importer. And the libc importer, which is the
# same question asked of the device's own `strcpy`/`strcat` — linked against the NDK's stub `libc.so` for
# its `DT_NEEDED`, `-fno-builtin` so the calls are emitted, fortify off so they are not `__strcpy_chk`.
IFLEAF="$BUILD/librbifuncleaf.so"
IFUSER="$BUILD/librbifuncuser.so"
IFLIBC="$BUILD/librbifunclibc.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbifuncleaf.so -o "$IFLEAF" "$ROOT/tests/corpus/link_ifunc_leaf.c"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbifuncuser.so -o "$IFUSER" "$ROOT/tests/corpus/link_ifunc_user.c" -L"$BUILD" -lrbifuncleaf
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os -fno-builtin -U_FORTIFY_SOURCE \
  -Wl,-soname,librbifunclibc.so -o "$IFLIBC" "$ROOT/tests/corpus/ifunc_libc_user.c" -lc

# An unresolved weak reference (Angry Birds Go's __cxa_guard_acquire shape): the loader must bind it to 0.
WEAK="$BUILD/librbweak.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbweak.so -o "$WEAK" "$ROOT/tests/corpus/weak_unresolved.c"

# Constructors that fault part-way (GuestProcess must not rerun the ones that already ran).
CTORFAULT="$BUILD/librbctorfault.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,librbctorfault.so -o "$CTORFAULT" "$ROOT/tests/corpus/ctor_fault.c"

# M2 Task 4: guest pthreads. Built exactly as tools/run_pthread_oracle.sh builds the copy it runs natively
# (keep the two lines in step), linked against the NDK stub libc for its DT_NEEDED.
PTHREAD_FIXTURE="$BUILD/libpthread_fixture.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os \
  -Wl,-soname,libpthread_fixture.so -o "$PTHREAD_FIXTURE" "$ROOT/tests/corpus/pthread_fixture.c" -lc

# M3: the JNI bridge fixture, built the same way (the bridge test APK of Task 6 builds the same source natively),
# without auto-vectorization: Tier I has no NEON compare (VectorEqual32) yet, and the fixture tests the bridge.
JNIBRIDGE_FIXTURE="$BUILD/libjnibridge_fixture.so"
"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -nostdlib -Os -fno-vectorize -fno-slp-vectorize \
  -Wl,-soname,libjnibridge_fixture.so -o "$JNIBRIDGE_FIXTURE" "$ROOT/tests/corpus/jni_bridge_fixture.c" -lc

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
adb push "$(to_host "$LEAF")" /data/local/tmp/librbleaf.so >/dev/null
adb push "$(to_host "$MID")" /data/local/tmp/librbmid.so >/dev/null
adb push "$(to_host "$IFLEAF")" /data/local/tmp/librbifuncleaf.so >/dev/null
adb push "$(to_host "$IFUSER")" /data/local/tmp/librbifuncuser.so >/dev/null
adb push "$(to_host "$IFLIBC")" /data/local/tmp/librbifunclibc.so >/dev/null
adb push "$(to_host "$WEAK")" /data/local/tmp/librbweak.so >/dev/null
adb push "$(to_host "$CTORFAULT")" /data/local/tmp/librbctorfault.so >/dev/null
adb push "$(to_host "$PTHREAD_FIXTURE")" /data/local/tmp/libpthread_fixture.so >/dev/null
adb push "$(to_host "$JNIBRIDGE_FIXTURE")" /data/local/tmp/libjnibridge_fixture.so >/dev/null
adb push "$(to_host "$ROOT/tests/corpus/oracle-pthread.txt")" /data/local/tmp/oracle-pthread.txt >/dev/null
adb push "$(to_host "$FIXTURE_STRIPPED")" /data/local/tmp/librb_fixture_stripped.so >/dev/null
adb push "$(to_host "$DEVICE_LIBC")" /data/local/tmp/rb_device_libc.so >/dev/null
# libc's own DT_NEEDED, from the same 32-bit system (optional: a 32-bit-capable device has them in /system/lib;
# a 64-bit-only one needs them pushed — adb pull /system/lib/{libdl,ld-android}.so build/device-libs/).
for dep in libdl.so ld-android.so; do
  if [ -s "$ROOT/build/device-libs/$dep" ]; then
    adb push "$(to_host "$ROOT/build/device-libs/$dep")" "/data/local/tmp/rb_device_$dep" >/dev/null
  fi
done
adb push "$(to_host "$GUEST_LIBM")" /data/local/tmp/libm.so >/dev/null
adb push "$(to_host "$GUEST_LIBZ")" /data/local/tmp/libz.so >/dev/null
if [ -s "$GUEST_APK" ]; then adb push "$(to_host "$GUEST_APK")" /data/local/tmp/rb_guestprobe.apk >/dev/null; fi
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
