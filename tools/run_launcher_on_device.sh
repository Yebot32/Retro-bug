#!/usr/bin/env bash
# RetroBug — run the launcher app on a phone and show what the engine did. SPDX-License-Identifier: Apache-2.0
#
# Why this exists: the device loop for the launcher used to be retyped by hand — build, push, install,
# launch, tap a game in the list, wait, screenshot, read logcat — and two of those steps are traps this
# project has already paid for. `adb install` is a silent no-op on the ColorOS phone (it returns success,
# prints nothing and installs nothing), so the only install path that works there is the PackageInstaller
# session API; and driving the app by taps lands in whatever screen it happened to resume on, which has
# twice produced a "nothing happened" reading of a run that never started. Both are encoded here, and the
# game is named on the command line instead of tapped.
#
# Usage:
#   tools/run_launcher_on_device.sh <serial> [game-name-substring]
# With no game name the app just opens, which is the right run for a screenshot of its home screen.
#
# The APK must already be built: tools/build_launcher_apk.sh. This script does not build it, on purpose —
# a run should be pinned to a specific artifact, and the build prints what it packaged.

set -euo pipefail
SERIAL="${1:?usage: run_launcher_on_device.sh <serial> [game-name-substring]}"
GAME="${2:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/build/launcher-apk/rbguest-launcher.apk"
PKG=com.retrobug.launcher

SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
ADB="$SDK/platform-tools/adb.exe"
[ -f "$APK" ] || { echo "no APK at $APK — run tools/build_launcher_apk.sh first" >&2; exit 1; }
# MSYS hands native programs /c/... paths and converts them on the way out, which is wrong for the device
# paths below and equally wrong for the local one, so the local one is converted explicitly.
to_host() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
adb() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }

{
  echo "# device=$SERIAL model=$(adb shell getprop ro.product.model | tr -d '\r')"
  echo "#        abilist32=[$(adb shell getprop ro.product.cpu.abilist32 | tr -d '\r')] sdk=$(adb shell getprop ro.build.version.sdk | tr -d '\r')"
} >&2

echo "push" >&2
adb push "$(to_host "$APK")" /data/local/tmp/rb.apk >/dev/null

# The session API, because `adb install` and `pm install` both silently do nothing on this phone.
SIZE="$(stat -c%s "$APK")"
SESSION="$(adb shell "pm install-create -r -g -S $SIZE" | tr -d '\r' | grep -o '[0-9]\+' | head -1)"
[ -n "$SESSION" ] || { echo "no install session id came back" >&2; exit 1; }
adb shell "pm install-write $SESSION base /data/local/tmp/rb.apk >/dev/null" >/dev/null
COMMIT="$(adb shell "pm install-commit $SESSION" | tr -d '\r' | tail -1)"
case "$COMMIT" in
  *Success*) echo "install: session $SESSION $COMMIT" >&2 ;;
  *) echo "install failed: session $SESSION said '$COMMIT'" >&2; exit 1 ;;
esac
INSTALLED="$(adb shell "pm list packages $PKG" | tr -d '\r')"
case "$INSTALLED" in
  *"$PKG"*) echo "installed: yes" >&2 ;;
  *) echo "the commit reported success and $PKG is not installed — this is the ColorOS silent-drop case" >&2; exit 1 ;;
esac

adb shell "logcat -c" >/dev/null
if [ -n "$GAME" ]; then
  adb shell "am force-stop $PKG; am start -n $PKG/.LauncherActivity -e run '$GAME'" >/dev/null
else
  adb shell "am force-stop $PKG; am start -n $PKG/.LauncherActivity" >/dev/null
fi

# The engine run is on a worker thread and the report is written when it finishes, so this waits rather
# than polling for a line: a constructor sequence through the interpreter is not a millisecond job.
WAIT="${RB_WAIT:-90}"
echo "waiting ${WAIT}s for the run" >&2
sleep "$WAIT"

adb shell "screencap -p /sdcard/rb-run.png" >/dev/null 2>&1
adb pull /sdcard/rb-run.png "$(to_host "/tmp/rb-run-$SERIAL.png")" >/dev/null 2>&1
echo "screenshot: /tmp/rb-run-$SERIAL.png" >&2
echo "--- RBLAUNCH ---"
adb logcat -d -s RBLAUNCH | tr -d '\r'
