#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Build the guest probe APK and its hardware oracle.
#
# What this produces, and why each piece exists:
#
#   libmathprobe.so  a real `armeabi-v7a` JNI library built by the ordinary Android toolchain, with a
#                    real `JNI_OnLoad` and a JNI method that calls `sin`/`cos`/`sqrt` out of a *second*
#                    library. Deliberately not self-contained: a self-contained probe would test the CPU
#                    core and nothing else, whereas this one also exercises PLT relocation and
#                    cross-library symbol resolution, which is what M1 needs to be true.
#   rb_guestprobe.apk  the .so packaged the way Android packages native libraries — `lib/<abi>/*.so`,
#                    stored uncompressed (the `extractNativeLibs=false` layout), with a manifest compiled
#                    by aapt2. 3 KB, and the same file the lane extracts from.
#   oracle_linked    the same .so, linked and run *natively* on a real device, printing the answers as
#                    raw bit patterns. This is the authority the lane asserts against; a host 64-bit
#                    libm would be corroboration, not authority, because the guest's answer comes from
#                    32-bit bionic's own code paths and a last-bit difference between two libm revisions
#                    is a legitimate difference rather than a bug.
#
# One toolchain trap is stepped around here on purpose: `sin(x)` and `cos(x)` on the same argument are
# fused by LLVM into a single `sincos` call, which the NDK's libm stub *exports* and the NDK's math.h
# does not even declare. The probe is built with `-fno-builtin-sin -fno-builtin-cos` so it imports `sin`
# and `cos`, which every libm has. Whether any given device's libm exports `sincos` is not established
# here and nothing depends on it.
#
# Also deliberately: `-nostartfiles`. Without it the driver links crtbegin_so/crtend_so, which drag
# `__cxa_atexit`, `__cxa_finalize` and `__register_atfork` in as imports — three libc symbols that have
# nothing to do with this probe's question. With them the probe would need a guest libc to load at all;
# without them it needs exactly one symbol, `sincos`-era math, and can be loaded the day guest libm
# works. The dependency the lane actually tests stays visible in `llvm-readelf --dyn-syms`.
#
# Usage: tools/build-guest-apk.sh [outdir]     # default build/guest-apk
set -euo pipefail
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
OUT="${1:-$ROOT/build/guest-apk}"
SRC="$ROOT/tests/guest"
# The native tools need native paths, while MSYS_NO_PATHCONV (set on every toolchain call below so the
# device paths in -rpath survive) stops the shell converting the local ones. Converting once here is
# what keeps that from being tripped over five times.
OUTW="$(cygpath -w "$OUT")"
SRCW="$(cygpath -w "$SRC")"
AAPT2="$(ls -d "$SDK"/build-tools/*/aapt2.exe 2>/dev/null | tail -1)"
ANDROID_JAR="$(ls -d "$SDK"/platforms/android-3*/android.jar 2>/dev/null | tail -1)"
JAR="$(command -v jar || true)"

mkdir -p "$OUT"

echo "=========== libmathprobe.so"
# -fno-builtin-{sin,cos,sqrt} so the calls stay calls; -nostartfiles so the probe does not need libc.
MSYS_NO_PATHCONV=1 "$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -Os -nostartfiles \
  -fno-builtin-sin -fno-builtin-cos -fno-builtin-sqrt \
  -o "$OUTW/libmathprobe.so" "$SRCW/mathprobe.c"

echo "  exports:"
"$TC/bin/llvm-readelf.exe" --dyn-syms "$OUT/libmathprobe.so" | awk '$4=="FUNC" && $7!="UND"{printf "    %s (%s bytes)\n", $NF, $3}'
echo "  imports (what the guest must resolve for this to load):"
"$TC/bin/llvm-readelf.exe" --dyn-syms "$OUT/libmathprobe.so" | awk '$7=="UND"{print $NF}' \
  | sed 's/@.*//' | grep -vE '^$|^UND$' | sort -u | sed 's/^/    /'
echo "  relocations:"
"$TC/bin/llvm-readelf.exe" -r "$OUT/libmathprobe.so" | grep -oE "R_ARM_[A-Z0-9_]+" | sort | uniq -c | sed 's/^/    /'

echo "=========== rb_guestprobe.apk"
[ -n "$AAPT2" ] || { echo "no aapt2 under $SDK/build-tools" >&2; exit 1; }
[ -n "$ANDROID_JAR" ] || { echo "no android.jar under $SDK/platforms" >&2; exit 1; }
[ -n "$JAR" ] || { echo "no jar on PATH (the JDK's is fine)" >&2; exit 1; }
"$AAPT2" link --manifest "$SRCW/AndroidManifest.xml" -o "$OUTW/rb_guestprobe.apk" -I "$ANDROID_JAR" \
  --min-sdk-version 21 --target-sdk-version 34 >/dev/null
mkdir -p "$OUT/zip/lib/armeabi-v7a"
cp "$OUT/libmathprobe.so" "$OUT/zip/lib/armeabi-v7a/"
# `u0` = update, no compression. Android ships native libraries stored, and it is also what lets the
# lane's minimal zip reader copy the entry out without an inflate implementation.
(cd "$OUT/zip" && "$JAR" uf0 "$OUT/rb_guestprobe.apk" lib/armeabi-v7a/libmathprobe.so >/dev/null)
echo "  $(unzip -v "$OUT/rb_guestprobe.apk" | awk '/libmathprobe/{print $1" bytes, method "$2}')"
"$TC/bin/llvm-readelf.exe" -h "$OUT/libmathprobe.so" | grep -E "Class|Machine|Type" | sed 's/^/  /'

echo "=========== oracle_linked  (run this on a 32-bit-capable device for the expected values)"
# Linked, not dlopen'd: Android does not resolve symbols for a library opened by path from
# /data/local/tmp, so a dlopen-based oracle fails on `sin` for reasons that have nothing to do with the
# library. -L needs a native path while -rpath must stay a device path, hence OUTW above and the flag.
MSYS_NO_PATHCONV=1 "$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -Os -o "$OUTW/oracle_linked" "$SRCW/oracle_linked.c" \
  -L"$OUTW" -lmathprobe -Wl,-rpath,/data/local/tmp -Wl,--no-as-needed -lm
echo "  $OUT/oracle_linked"
echo
echo "Push all three, then run the oracle where 32-bit code executes natively:"
echo "  adb push $OUT/rb_guestprobe.apk /data/local/tmp/rb_guestprobe.apk"
echo "  adb push $OUT/libmathprobe.so   /data/local/tmp/libmathprobe.so"
echo "  adb push $OUT/oracle_linked     /data/local/tmp/oracle_linked"
echo "  adb push build/guest-libs/libm.so /data/local/tmp/libm.so"
echo "  adb shell 'chmod 755 /data/local/tmp/oracle_linked && /data/local/tmp/oracle_linked'"
