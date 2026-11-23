#!/usr/bin/env bash
# RetroBug — on-device check of the targetSdk compatibility shims. SPDX-License-Identifier: Apache-2.0
#
# Runs the launcher's `-e shimcheck 1` (android/java/.../ShimCheck.java) in a fresh launcher process and
# reads its RBSHIM rows: for each behaviour change a "before" row that must see the platform throw and an
# "after" row (com.retrobug.shim.Shims applied, original target 8) that must not. The shims stay applied to
# that process, so it is started with -S and force-stopped afterwards.
#
# Usage: tools/run_shimcheck.sh <serial>
# The launcher must already be installed (tools/run_launcher_on_device.sh <serial> installs and opens it).
# Exit 0 only when the RESULT line reports 0 failures.

set -euo pipefail
SERIAL="${1:?usage: run_shimcheck.sh <serial>}"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
adb() { MSYS_NO_PATHCONV=1 "$SDK/platform-tools/adb.exe" -s "$SERIAL" "$@"; }

adb shell "logcat -c" >/dev/null
adb shell "am start -S -W -n com.retrobug.launcher/.LauncherActivity -e shimcheck 1" >/dev/null
OUT=""
for _ in $(seq 1 30); do
  OUT="$(adb shell "logcat -d -s RBSHIM:I" | tr -d '\r')"
  case "$OUT" in *"RESULT: shimcheck"*) break ;; esac
  sleep 1
done
adb shell "am force-stop com.retrobug.launcher" >/dev/null
printf '%s\n' "$OUT" | grep -E "shimcheck:|shim |RESULT" | sed 's/^.*RBSHIM *: //'
RESULT="$(printf '%s\n' "$OUT" | grep -o 'RESULT: shimcheck .*' | tail -1)"
[ -n "$RESULT" ] || { echo "no RESULT line within 30 s" >&2; exit 1; }
case "$RESULT" in *" 0 failures"*) exit 0 ;; *) exit 1 ;; esac
