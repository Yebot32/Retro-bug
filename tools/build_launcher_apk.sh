#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Build the host-side launcher: an installable `arm64-v8a` APK whose native half is RetroBug's own engine.
#
# What it produces is the readout for the question `tools/app_domain_probe.sh` could only ask about
# individual syscalls: does the engine, running as an installed app, get the hardware's answer? The APK
# carries `lib/arm64-v8a/librbguest.so` (the engine plus `android/jni/launcher.cpp`) and assets — the
# guest's APK and the guest libraries a chosen app is resolved against, one of them optional — which the
# Activity reads as byte arrays and hands across JNI.
#
# It runs **no adb command**. Two phones are attached to this machine most of the time and a verification
# run is normally in flight on them; an install from here would replace a binary mid-run, which is a worse
# outcome than a script that stops and prints the commands to run by hand. Those are printed at the end
# instead.
#
# The build is the no-Gradle chain recorded as working in `docs/20` §1 and used by
# `tools/app_domain_probe.sh`: javac -> d8 -> aapt2 link -> jar to add classes.dex + lib + assets ->
# zipalign -> apksigner. It is spelled out here rather than delegated so the whole path is auditable and
# runs on a machine with only the SDK and a JDK on it.
#
# Usage: tools/build_launcher_apk.sh          # RB_BUILD_TOOLS=36.1.0 pins build-tools
#                                             # RB_NDK=<ver> pins the NDK
#                                             # RB_LAUNCHER_BUILD=<dir> redirects the CMake build dir

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
CM="$SDK/cmake/3.31.6/bin/cmake.exe"
NJ="$SDK/cmake/3.31.6/bin/ninja.exe"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
BT="$SDK/build-tools/${RB_BUILD_TOOLS:-36.1.0}"
AJ="$SDK/platforms/android-36/android.jar"
SRC="$ROOT/android"
# A build directory of its own, and that is load-bearing rather than tidiness: `build/tests-arm64-v8a` is
# the one `tools/run_engine_tests.sh` configures and pushes from, and a launcher target in it would be
# rebuilt over a verification run's binaries. `RB_LAUNCHER_BUILD=<dir>` points it elsewhere, which is how
# the packaging half below can be exercised against a build directory that already exists.
BUILD="${RB_LAUNCHER_BUILD:-$ROOT/build/launcher-arm64}"
OUT="$ROOT/build/launcher-apk"
# The keystore lives outside $OUT on purpose: $OUT is deleted at the start of every run, and a key that is
# regenerated each time makes the *next* install fail with
# `INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match newer version` — a rebuild appearing as a
# device problem.
KS="$ROOT/build/launcher-apk-key.keystore"

GUEST_APK="$ROOT/build/guest-apk/rb_guestprobe.apk"
GUEST_LIBM="$ROOT/build/guest-libs/libm.so"
GUEST_LIBZ="$ROOT/build/guest-libs/libz.so"
# Optional, and deliberately not in the requirement list below: the guest's real libc is the device's own
# bionic, pulled off a 32-bit-capable phone, and no script in this tree produces one.
GUEST_LIBC="$ROOT/build/device-libs/libc.so"

to_host() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

# ---- the guest images this script insists on. None is downloaded, generated, or stubbed by this script:
# they are outputs of their own build scripts, and a launcher that quietly manufactured a stand-in would
# report PASS about bytes that no device ever ran. `libc.so` is absent from this list on purpose — see the
# staging step, which treats it as required-if-present.
for spec in "$GUEST_APK|tools/build-guest-apk.sh" "$GUEST_LIBM|tools/build-guest-libs.sh libm" \
            "$GUEST_LIBZ|tools/build-guest-libs.sh libz"; do
  in="${spec%%|*}"; maker="${spec##*|}"
  if [ ! -s "$in" ]; then
    echo "missing $in — this script does not create it. Run '$maker' first." >&2
    exit 1
  fi
done

[ -x "$CM" ] || { echo "no cmake at $CM" >&2; exit 1; }
[ -x "$NJ" ] || { echo "no ninja at $NJ" >&2; exit 1; }
[ -x "$TC/bin/clang++.exe" ] || { echo "no NDK r$NDK_VER clang++ at $TC (RB_NDK=<ver>)" >&2; exit 1; }
[ -d "$BT" ] || { echo "no build-tools at $BT (RB_BUILD_TOOLS=<ver>)" >&2; exit 1; }
[ -f "$AJ" ] || { echo "no android.jar at $AJ" >&2; exit 1; }
for t in "$BT/aapt2.exe" "$BT/zipalign.exe" "$BT/d8.bat" "$BT/apksigner.bat"; do
  [ -e "$t" ] || { echo "missing $t" >&2; exit 1; }
done
command -v javac >/dev/null || { echo "no javac on PATH" >&2; exit 1; }
command -v jar >/dev/null || { echo "no jar on PATH (the JDK's is fine)" >&2; exit 1; }
command -v keytool >/dev/null || { echo "no keytool on PATH" >&2; exit 1; }

# ---- native: the engine, cross-built for the host ABI only.
#
# `arm64-v8a` and nothing else, so the package manager has no ABI choice to make. It is not free to make
# one wrongly in this shape — `tests/appdomain`'s results show an arm64 device taking `arm64` from a
# two-directory APK — but that probe had to build two APKs precisely because it *needed* the choice to be
# explicit, and here the opposite holds: `librbguest.so` runs AArch64 code (Tier J's output), so a second
# directory inviting a 32-bit process would make the run's outcome depend on a picker nobody is watching.
#
# `-DRB_BUILD_TESTS=OFF` keeps the test lanes out of this build dir entirely, and `rbguest` is
# `EXCLUDE_FROM_ALL` in `CMakeLists.txt`, so the explicit target below is the only thing that builds it.
mkdir -p "$BUILD"
"$CM" -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NJ" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/$NDK_VER/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
  -DRB_BUILD_TESTS=OFF -DRB_BUILD_LAUNCHER=ON >/dev/null
"$NJ" -C "$BUILD" rbguest rbstub
LIB="$BUILD/librbguest.so"
[ -f "$LIB" ] || { echo "no $LIB: the rbguest target did not produce it" >&2; exit 1; }
# The stub a repackaged game loads in place of each 32-bit library. The launcher does not load it; it
# ships it so the repackager can copy it out of this APK (`lib/arm64-v8a/librbstub.so`).
STUB="$BUILD/librbstub.so"
[ -f "$STUB" ] || { echo "no $STUB: the rbstub target did not produce it" >&2; exit 1; }

# ---- staging. The guest images travel as assets because that is the form the engine is handed them
# in: `LoadFromBytes` takes a byte vector, and `android/java/.../LauncherActivity.java` reads the asset into
# one. It also keeps the comparison with the C++ lane honest — `tools/run_engine_tests.sh` pushes
# `build/guest-apk/rb_guestprobe.apk` to `/data/local/tmp/` and this copies the same file.
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/stage/lib/arm64-v8a" "$OUT/stage/assets/guest"
# Strip the staged copy only. This target's unstripped Release `.so` is 50,307,560 bytes and
# `llvm-strip --strip-unneeded` leaves 4,079,208 (both measured on this build, and the stripped figure
# repeated exactly across two rebuilds). That is a 12x saving on the installed library for information a
# phone cannot use, and nothing is lost: the debug copy stays at `$LIB`, which is the file a tombstone
# gets symbolized against afterwards.
cp "$LIB" "$OUT/stage/lib/arm64-v8a/librbguest.so"
"$TC/bin/llvm-strip.exe" --strip-unneeded "$OUT/stage/lib/arm64-v8a/librbguest.so"
cp "$STUB" "$OUT/stage/lib/arm64-v8a/librbstub.so"
"$TC/bin/llvm-strip.exe" --strip-unneeded "$OUT/stage/lib/arm64-v8a/librbstub.so"
cp "$GUEST_APK" "$OUT/stage/assets/guest/rb_guestprobe.apk"
cp "$GUEST_LIBM" "$OUT/stage/assets/guest/libm.so"
cp "$GUEST_LIBZ" "$OUT/stage/assets/guest/libz.so"
# libc.so is the one guest library this script cannot insist on: it is a file pulled off a 32-bit-capable
# device, and a relink of the NDK's static `libc.a` is not the dynamic libc a real guest expects. Absent is
# a warning rather than a build failure — the Activity reads the missing asset as an empty array and the
# native side reports the scope it could link, which is the run this app had before libc was shipped. It is
# deliberately not stubbed: a fabricated libc would be reported as a loaded one.
if [ -s "$GUEST_LIBC" ]; then
  cp "$GUEST_LIBC" "$OUT/stage/assets/guest/libc.so"
else
  echo "warning: $GUEST_LIBC is missing — packaging no guest libc.so;" >&2
  echo "         the launcher then reports the runs it can still make without it. To include one, pull the" >&2
  echo "         device's 32-bit /system/lib/libc.so to that path." >&2
fi
# The device's libstdc++.so, by the same rule: pulled, never relinked or stubbed. Only games that name it
# in DT_NEEDED get it placed (pre-r8 NDK images such as Gumball 2013 do; Angry Birds does not).
GUEST_LIBSTDCXX="$ROOT/build/device-libs/libstdc++.so"
if [ -s "$GUEST_LIBSTDCXX" ]; then
  cp "$GUEST_LIBSTDCXX" "$OUT/stage/assets/guest/libstdc++.so"
else
  echo "warning: $GUEST_LIBSTDCXX is missing — a game that needs libstdc++.so will report it unplaced;" >&2
  echo "         pull the device's 32-bit /system/lib/libstdc++.so to that path." >&2
fi

# ---- java -> dex
#
# The `|| true` that used to end the javac line applied to the whole pipeline, so a compile error exited 0
# and the *next* command failed instead: `d8` was handed an empty `$OUT/classes` and reported a dex error
# naming no source line, which reads as a toolchain problem rather than as the Java file not compiling.
# The filter that drops javac's harmless "bootstrap class path" note still runs, but javac's own status is
# captured before it and checked here. `set +e`/`set -e` around the capture because the pipeline below
# legitimately exits non-zero when the filter matches nothing.
#
# apksig (the library behind `apksigner`) signs repackaged games on the phone (RepackSigner). It ships in
# build-tools as `lib/apksigner.jar`, which also carries the apksigner CLI and a copy of org.conscrypt; only
# `com/android/apksig/**` is taken, so no conscrypt class can shadow or clash with the platform's. The
# filtered jar is javac's classpath and a d8 program input, so the classes end up in classes.dex.
APKSIG_SRC="$BT/lib/apksigner.jar"
[ -f "$APKSIG_SRC" ] || { echo "no $APKSIG_SRC (apksig for on-device signing)" >&2; exit 1; }
mkdir -p "$OUT/apksig"
(cd "$OUT/apksig" && unzip -q -o "$(to_host "$APKSIG_SRC")")
APKSIG="$OUT/apksig.jar"
(cd "$OUT/apksig" && jar -cf "$(to_host "$APKSIG")" com/android/apksig)
[ -s "$APKSIG" ] || { echo "could not build $APKSIG from $APKSIG_SRC" >&2; exit 1; }
JAVA_SRCS=()
for s in "$SRC"/java/com/retrobug/launcher/*.java "$SRC"/shim/com/retrobug/shim/*.java; do JAVA_SRCS+=("$(to_host "$s")"); done
set +e
javac_out="$(javac -encoding UTF-8 -nowarn -source 8 -target 8 -bootclasspath "$(to_host "$AJ")" \
  -classpath "$(to_host "$APKSIG")" \
  -d "$(to_host "$OUT/classes")" "${JAVA_SRCS[@]}" 2>&1)"
javac_status=$?
set -e
printf '%s\n' "$javac_out" | grep -v "bootstrap class path" || true
if [ "$javac_status" -ne 0 ]; then
  echo "javac failed (exit $javac_status); stopping here rather than letting d8 report it as a dex error" >&2
  exit 1
fi
# The launcher's classes go to d8 as one jar: d8.bat is a cmd script, and a growing list of .class paths
# on its command line runs into cmd's 8191-character limit.
(cd "$OUT/classes" && jar -cf "$(to_host "$OUT/launcher-classes.jar")" .)
"$BT/d8.bat" --lib "$(to_host "$AJ")" --min-api 26 --output "$(to_host "$OUT/dex")" \
  "$(to_host "$OUT/launcher-classes.jar")" "$(to_host "$APKSIG")" >/dev/null
[ -s "$OUT/dex/classes.dex" ] || { echo "d8 produced no classes.dex" >&2; exit 1; }
if [ -e "$OUT/dex/classes2.dex" ]; then
  echo "d8 produced more than one dex; this script packages only classes.dex" >&2; exit 1
fi
# The targetSdk compatibility shims (android/shim) are also dexed on their own: the repackager adds this dex
# to a game as an extra classesN.dex (ShimPlan). The launcher's classes.dex carries the same classes for its
# own `-e shimcheck`. --min-api 24 because that is the lowest minSdk a repackaged game declares.
mkdir -p "$OUT/shim-classes" "$OUT/shim-dex"
(cd "$OUT/classes" && jar -cf "$(to_host "$OUT/shim-classes.jar")" com/retrobug/shim)
"$BT/d8.bat" --lib "$(to_host "$AJ")" --min-api 24 --output "$(to_host "$OUT/shim-dex")" \
  "$(to_host "$OUT/shim-classes.jar")" >/dev/null
[ -s "$OUT/shim-dex/classes.dex" ] || { echo "d8 produced no shim classes.dex" >&2; exit 1; }
cp "$OUT/shim-dex/classes.dex" "$OUT/stage/assets/rbshim.dex"

# ---- package
# `aapt2 link` takes *compiled* resources, so the icon tree is compiled first. Until the icon the launcher
# had none and this step did not exist; without it the app installs with the platform's default icon, which
# is a thing you only notice on a home screen.
RES_ARGS=()
if [ -d "$SRC/res" ]; then
  RESZIP="$OUT/res.zip"
  "$BT/aapt2.exe" compile --dir "$(to_host "$SRC/res")" -o "$(to_host "$RESZIP")"
  RES_ARGS+=("$(to_host "$RESZIP")")
else
  echo "note: no $SRC/res, building without an icon" >&2
fi
"$BT/aapt2.exe" link -I "$(to_host "$AJ")" --manifest "$(to_host "$SRC/AndroidManifest.xml")" \
  --min-sdk-version 26 --target-sdk-version 34 --version-code 1 --version-name 1 \
  ${RES_ARGS[@]+"${RES_ARGS[@]}"} \
  -o "$(to_host "$OUT/base.apk")"
cp "$OUT/dex/classes.dex" "$OUT/stage/"
(cd "$OUT/stage" && jar -uf "$(to_host "$OUT/base.apk")" classes.dex lib assets)
"$BT/zipalign.exe" -f -p 4 "$(to_host "$OUT/base.apk")" "$(to_host "$OUT/aligned.apk")"
# ---- signing key
#
# A *stable* key matters more than it looks. An install of a newer build over an older one is refused when
# the two are signed by different keys -- `INSTALL_FAILED_UPDATE_INCOMPATIBLE` -- and the refusal arrives on
# the phone, as one line of adb output, at the end of a command that otherwise looks like it worked. A build
# that generates a fresh key every run therefore cannot be installed over the last one: the app has to be
# uninstalled first, which deletes its data. So the key comes from the environment when it is named there,
# and is generated into `build/` only as a local throwaway:
#
#   RB_KEYSTORE       path to the keystore (CI: materialized from the RB_KEYSTORE_B64 secret)
#   RB_KEYSTORE_PASS  store and key password; the key is PKCS12, where the two have to match
#   RB_KEY_ALIAS      entry name, default `launcher`
# `$KS` is already set above, outside `$OUT`, and that is the default on purpose. An earlier revision
# wrote `KS="${RB_KEYSTORE:-$OUT/rb-test.keystore}"` here, which put the key back inside the directory
# line 107 deletes every run: the assignment at the top of the script looked authoritative and was dead,
# and the failure it produced (`INSTALL_FAILED_UPDATE_INCOMPATIBLE` on the *next* install) is the exact
# one the comment above says the outside-$OUT location exists to prevent. Only a named `RB_KEYSTORE`
# may move the key now.
KS="${RB_KEYSTORE:-$KS}"
KS_PASS="${RB_KEYSTORE_PASS:-android}"
KEY_ALIAS="${RB_KEY_ALIAS:-launcher}"
if [ -n "${RB_KEYSTORE:-}" ] && [ ! -f "$KS" ]; then
  echo "RB_KEYSTORE=$KS does not exist; refusing to generate a throwaway key where a stable one was named" >&2
  exit 1
fi
# The invariant the whole block rests on, checked rather than assumed: a key inside `$OUT` cannot
# survive the next run, and a build that quietly minted a new one would report success and then fail
# on the phone. This is the "refuse loudly" rule applied to the one path that was silently wrong.
case "$KS" in
  "$OUT"/*)
    echo "keystore $KS is inside \$OUT ($OUT), which this script deletes on every run;" >&2
    echo "that yields a fresh key each build and the next 'adb install -r' fails with" >&2
    echo "INSTALL_FAILED_UPDATE_INCOMPATIBLE. Set RB_KEYSTORE to a path outside \$OUT." >&2
    exit 1
    ;;
esac
[ -f "$KS" ] || keytool -genkeypair -noprompt -keystore "$(to_host "$KS")" -storetype PKCS12 \
  -storepass "$KS_PASS" -keypass "$KS_PASS" -alias "$KEY_ALIAS" \
  -dname "CN=RetroBug test build, OU=none, O=none, L=none, S=none, C=US" \
  -keyalg RSA -keysize 2048 -validity 3650 >/dev/null 2>&1
"$BT/apksigner.bat" sign --ks "$(to_host "$KS")" --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
  --ks-key-alias "$KEY_ALIAS" \
  --out "$(to_host "$OUT/rbguest-launcher.apk")" "$(to_host "$OUT/aligned.apk")" >/dev/null
"$BT/apksigner.bat" verify "$(to_host "$OUT/rbguest-launcher.apk")" && echo "apksigner verify: clean"

echo
echo "built:"
printf '  %-38s %s bytes  (stripped, this is what installs)\n' "lib/arm64-v8a/librbguest.so" \
  "$(wc -c < "$OUT/stage/lib/arm64-v8a/librbguest.so" | tr -d ' ')"
printf '  %-38s %s bytes  (unstripped, keep it for tombstones)\n' "$LIB" "$(wc -c < "$LIB" | tr -d ' ')"
printf '  %-38s %s bytes  (stripped)\n' "lib/arm64-v8a/librbstub.so" \
  "$(wc -c < "$OUT/stage/lib/arm64-v8a/librbstub.so" | tr -d ' ')"
printf '  %-38s %s bytes\n' "rbguest-launcher.apk" "$(wc -c < "$OUT/rbguest-launcher.apk" | tr -d ' ')"
echo "  packaged entries:"
unzip -v "$OUT/rbguest-launcher.apk" | awk 'NR>3 && NF>7{printf "    %s  %s (%s)\n", $2, $8, $3}'
echo
echo "Not installed: this script runs no adb. To run it,"
echo "  adb install -r -g $(to_host "$OUT/rbguest-launcher.apk")"
echo "  adb shell am start -n com.retrobug.launcher/.LauncherActivity"
echo "  adb logcat -s RBLAUNCH"
echo "Expected on a PASS: for each of the six seeds, a 'JNI_OnLoad tier=I' / 'tier=J' pair and a"
echo "'mathProbe seed=N tier=I' / 'tier=J' pair, all PASS, then 'summary: 4 PASS, 0 FAIL over 4 calls'."
echo "The values are the ones the same bytes produced natively on the SM-X710 via"
echo "tools/build-guest-apk.sh's oracle_linked."
