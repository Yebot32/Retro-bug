#!/usr/bin/env bash
# RetroBug — build the unsigned iOS IPA. SPDX-License-Identifier: Apache-2.0
#
# macOS only: iOS binaries need Apple's SDK and linker. Produces build/ios/ipa/RetroBug-unsigned.ipa, which
# is then signed by AltStore/SideStore/a personal team. No Apple developer account is needed here.
#
# Usage: tools/build_ios_ipa.sh            # RB_IOS_BUILD=<dir> redirects the CMake build dir
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${RB_IOS_BUILD:-$ROOT/build/ios}"

command -v xcodebuild >/dev/null || { echo "no xcodebuild (macOS required)" >&2; exit 1; }
command -v xcodegen >/dev/null || { echo "no xcodegen: brew install xcodegen" >&2; exit 1; }
command -v cmake >/dev/null || { echo "no cmake" >&2; exit 1; }

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_BUILD_TYPE=Release -DRB_BUILD_TESTS=OFF
fi
cmake --build "$BUILD" --target rb_engine_ios -j "$(sysctl -n hw.ncpu)"

( cd "$ROOT/ios" && xcodegen generate )

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
xcodebuild -project "$ROOT/ios/RetroBug.xcodeproj" -scheme RetroBug -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$BUILD/dd" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO RB_ENGINE_DIR="$BUILD/ios-engine" \
  RB_GIT_COMMIT="$COMMIT" build

APP="$BUILD/dd/Build/Products/Release-iphoneos/RetroBug.app"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
mkdir -p "$BUILD/ipa/Payload"
rm -rf "$BUILD/ipa/Payload/RetroBug.app"
cp -R "$APP" "$BUILD/ipa/Payload/"
rm -f "$BUILD/ipa/RetroBug-unsigned.ipa"
( cd "$BUILD/ipa" && zip -qry RetroBug-unsigned.ipa Payload )
echo "built $BUILD/ipa/RetroBug-unsigned.ipa"
