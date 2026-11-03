#!/usr/bin/env bash
# RetroBug — build the unsigned iOS IPA. SPDX-License-Identifier: Apache-2.0
#
# macOS only: iOS binaries need Apple's SDK and linker. Produces build/ios/ipa/RetroBug-unsigned.ipa, which
# is then signed by AltStore/SideStore/a personal team. No Apple developer account is needed here.
#
# Usage: tools/build_ios_ipa.sh            # RB_IOS_BUILD=<dir> redirects the CMake build dir
#        RB_AOT_PROFILE=<dir> tools/build_ios_ipa.sh   # per-game AOT profile (rb_aotgen --profile output)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${RB_IOS_BUILD:-$ROOT/build/ios}"

command -v xcodebuild >/dev/null || { echo "no xcodebuild (macOS required)" >&2; exit 1; }
command -v xcodegen >/dev/null || { echo "no xcodegen: brew install xcodegen" >&2; exit 1; }
command -v cmake >/dev/null || { echo "no cmake" >&2; exit 1; }

PROFILE_ARG="-DRB_AOT_PROFILE_DIR="
if [ -n "${RB_AOT_PROFILE:-}" ]; then
  PROFILE_ROOT="$(cd "$RB_AOT_PROFILE" && pwd)"
  AOT_SOURCES="$ROOT/ios/RetroBug/AotProfile"
  rm -rf "$AOT_SOURCES"
  mkdir -p "$AOT_SOURCES"
  registry="$AOT_SOURCES/aot_registry.cpp"
  echo '#include "rb/exec/aot.h"' > "$registry"
  entries=""
  count=0
  # One profile per subdirectory; a directory of generated files directly is the single-profile layout.
  for dir in "$PROFILE_ROOT" "$PROFILE_ROOT"/*/; do
    [ -d "$dir" ] || continue
    if ! ls "$dir"/*_profile.cpp >/dev/null 2>&1; then continue; fi
    name="$(basename "$dir")"
    dest="$AOT_SOURCES/$name"
    mkdir -p "$dest"
    cp "$dir"/*.S "$dir"/*_table.cpp "$dir"/*_profile.cpp "$dest"/
    # A translated image can exceed the ref host close file limit (libgame's .S is 110 MiB); the ref
    # stores it gzipped and the app compiles the expanded copy.
    cp "$dir"/*.S.gz "$dest"/ 2>/dev/null || true
    for gz in "$dest"/*.S.gz; do
      [ -e "$gz" ] || continue
      gunzip -c "$gz" > "${gz%.gz}"
      rm -f "$gz"
    done
    symbol="$(head -1 "$dir"/*_profile.symbol 2>/dev/null || true)"
    if [ -z "$symbol" ]; then symbol="rb_aot_profile_${name#aot_}"; fi
    echo "extern \"C\" const rb::exec::AotProfile $symbol;" >> "$registry"
    entries="${entries}    &${symbol},\n"
    count=$((count + 1))
    echo "aot: profile $name -> $symbol"
  done
  if [ "$count" -eq 0 ]; then echo "aot: no *_profile.cpp under $PROFILE_ROOT" >&2; exit 1; fi
  echo >> "$registry"
  echo 'extern "C" const rb::exec::AotProfile* const rb_aot_profiles[] = {' >> "$registry"
  printf "%b" "$entries" >> "$registry"
  echo '    nullptr,' >> "$registry"
  echo '};' >> "$registry"
  echo "aot: app target compiles $count profile(s) from $PROFILE_ROOT"
fi
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_BUILD_TYPE=Release -DRB_BUILD_TESTS=OFF $PROFILE_ARG
else
  # Always re-state the flag: a cache left over from a profiled build would otherwise keep compiling
  # that profile into what the operator believes is a plain Tier I IPA.
  cmake -S "$ROOT" -B "$BUILD" $PROFILE_ARG >/dev/null
fi
cmake --build "$BUILD" --target rb_engine_ios -j "$(sysctl -n hw.ncpu)"

# The profile is compiled by the app target, not merged into the engine archive (libtool's merged
# members were invisible to ld). Copy it into the app's sources before XcodeGen reads the project.

( cd "$ROOT/ios" && xcodegen generate )

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
set +e
xcodebuild -project "$ROOT/ios/RetroBug.xcodeproj" -scheme RetroBug -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$BUILD/dd" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO RB_ENGINE_DIR="$BUILD/ios-engine" \
  RB_GIT_COMMIT="$COMMIT" build
XCODE_STATUS=$?
set -e
if [ "$XCODE_STATUS" -ne 0 ] && [ -n "${RB_AOT_PROFILE:-}" ]; then
  # Name the state of the app's own assembled object: an object with local symbols, or one the link
  # file list omits, both look like "undefined symbols" from the linker.
  asm_object="$(find "$BUILD/dd" -name 'aot_kitkat_libz.o' | head -1)"
  echo "aot: assembled object: ${asm_object:-missing}"
  if [ -n "$asm_object" ]; then
    echo "aot: $(nm -g "$asm_object" | grep -c ' T .*_block_' || true) global block symbols in the app object"
    nm -g "$asm_object" | grep '_block_0000$' | head -2
    nm -m "$asm_object" | grep ' _aot_kitkat_libz_blocks$' | head -2
  fi
  resp="$(find "$BUILD/dd" -name '*-common-args.resp' | head -1)"
  echo "aot: visibility flags: $(tr ' ' '\n' < "$resp" 2>/dev/null | grep -c visibility || true)"
  tr ' ' '\n' < "$resp" 2>/dev/null | grep visibility | head -3
  grep -c 'aot_kitkat_libz.o' "$BUILD/dd/Build/Intermediates.noindex/RetroBug.build/Release-iphoneos/RetroBug.build/Objects-normal/arm64/RetroBug.LinkFileList" 2>/dev/null | sed 's/^/aot: link-file-list mentions of the libz object: /' || true
fi
[ "$XCODE_STATUS" -eq 0 ]

APP="$BUILD/dd/Build/Products/Release-iphoneos/RetroBug.app"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
mkdir -p "$BUILD/ipa/Payload"
rm -rf "$BUILD/ipa/Payload/RetroBug.app"
cp -R "$APP" "$BUILD/ipa/Payload/"
rm -f "$BUILD/ipa/RetroBug-unsigned.ipa"
( cd "$BUILD/ipa" && zip -qry RetroBug-unsigned.ipa Payload )
echo "built $BUILD/ipa/RetroBug-unsigned.ipa"
