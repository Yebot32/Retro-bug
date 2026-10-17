#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Build the JNI bridge test app (M3 Task 6): build/jnibridge/jnibridge.apk, package com.retrobug.jnibridge.
#
#   lib/armeabi-v7a/libjnibridge.so   tests/apk/jnibridge/jni/bridge_app.c + tests/corpus/jni_bridge_fixture.c,
#                                     Thumb softfp, auto-vectorization off (Tier I has no NEON compares)
#   classes.dex                       tests/apk/jnibridge/java (BridgeActivity), javac -source 8 against android.jar
#
# 32-bit only, so a 64-bit-only phone cannot install it as is — the launcher repacks it (tools/run_bridge_check.sh);
# a 32-bit-capable one runs it natively for the oracle (tools/run_bridge_oracle.sh). Signed with a throwaway key
# this script generates at build/jnibridge-key.keystore (kept between builds, so `adb install -r` keeps working).
# Runs no adb.
#
# Usage: tools/build_bridge_apk.sh

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
TC="$SDK/ndk/${RB_NDK:-28.2.13676358}/toolchains/llvm/prebuilt/windows-x86_64"
BT="$SDK/build-tools/${RB_BUILD_TOOLS:-36.1.0}"
AJ="$SDK/platforms/android-36/android.jar"
SRC="$ROOT/tests/apk/jnibridge"
OUT="$ROOT/build/jnibridge"
KS="$ROOT/build/jnibridge-key.keystore"

to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

for t in "$TC/bin/clang.exe" "$BT/aapt2.exe" "$BT/zipalign.exe" "$BT/d8.bat" "$BT/apksigner.bat" "$AJ"; do
  [ -e "$t" ] || { echo "build_bridge_apk: missing $t" >&2; exit 1; }
done
rm -rf "$OUT"; mkdir -p "$OUT/stage/lib/armeabi-v7a" "$OUT/classes" "$OUT/dex"

"$TC/bin/clang.exe" --target=armv7-none-linux-androideabi21 --sysroot="$(to_host "$TC/sysroot")" \
  -mthumb -mfloat-abi=softfp -mfpu=neon -fPIC -shared -Os -fno-vectorize -fno-slp-vectorize \
  -Wl,-soname,libjnibridge.so -o "$(to_host "$OUT/stage/lib/armeabi-v7a/libjnibridge.so")" \
  "$(to_host "$SRC/jni/bridge_app.c")" "$(to_host "$ROOT/tests/corpus/jni_bridge_fixture.c")"

JAVA_SRCS=()
for s in "$SRC"/java/com/retrobug/jnibridge/*.java; do JAVA_SRCS+=("$(to_host "$s")"); done
set +e
javac_out="$(javac -encoding UTF-8 -nowarn -source 8 -target 8 -bootclasspath "$(to_host "$AJ")" \
  -d "$(to_host "$OUT/classes")" "${JAVA_SRCS[@]}" 2>&1)"
javac_status=$?
set -e
printf '%s\n' "$javac_out" | grep -v "bootstrap class path" || true
[ "$javac_status" -eq 0 ] || { echo "javac failed (exit $javac_status)" >&2; exit 1; }
(cd "$OUT/classes" && jar -cf "$(to_host "$OUT/classes.jar")" .)
"$BT/d8.bat" --lib "$(to_host "$AJ")" --min-api 19 --output "$(to_host "$OUT/dex")" "$(to_host "$OUT/classes.jar")" >/dev/null
cp "$OUT/dex/classes.dex" "$OUT/stage/"

"$BT/aapt2.exe" link -I "$(to_host "$AJ")" --manifest "$(to_host "$SRC/AndroidManifest.xml")" \
  --version-code 1 --version-name 1 -o "$(to_host "$OUT/base.apk")"
(cd "$OUT/stage" && jar -uf "$(to_host "$OUT/base.apk")" classes.dex lib)
"$BT/zipalign.exe" -f -p 4 "$(to_host "$OUT/base.apk")" "$(to_host "$OUT/aligned.apk")"
[ -f "$KS" ] || keytool -genkeypair -noprompt -keystore "$(to_host "$KS")" -storetype PKCS12 \
  -storepass android -keypass android -alias jnibridge \
  -dname "CN=RetroBug JNI bridge test, OU=none, O=none, L=none, S=none, C=US" \
  -keyalg RSA -keysize 2048 -validity 3650 >/dev/null 2>&1
"$BT/apksigner.bat" sign --ks "$(to_host "$KS")" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias jnibridge --min-sdk-version 19 \
  --out "$(to_host "$OUT/jnibridge.apk")" "$(to_host "$OUT/aligned.apk")" >/dev/null
"$BT/apksigner.bat" verify "$(to_host "$OUT/jnibridge.apk")" >/dev/null && echo "built $OUT/jnibridge.apk ($(wc -c < "$OUT/jnibridge.apk" | tr -d ' ') bytes)"
