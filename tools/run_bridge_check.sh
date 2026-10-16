#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# The JNI bridge test app, bridged (M3 Task 6): the launcher repacks the staged jnibridge.apk (`-e repack jnibridge
# -e install 0`: generated stub, assets/rb32, signed with the phone's repack key), the signed output is pulled and
# installed, started, and its `BRIDGE: <row>=<value>` lines are compared with tests/corpus/oracle-jnibridge.txt
# (the same app run natively). Exit 0 only when every oracle row is printed with the same value.
#
# Needs: the launcher installed; jnibridge.apk staged in its library once (push build/jnibridge/jnibridge.apk to
# /sdcard/Download and add it with the launcher's "Add a game" picker). A natively installed copy of
# com.retrobug.jnibridge (ours, from the oracle run) is uninstalled first: the repacked one has another signature.
#
# Usage: tools/run_bridge_check.sh <serial>

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
ADB="$SDK/platform-tools/adb.exe"
SERIAL="${1:?usage: tools/run_bridge_check.sh <serial>}"
ORACLE="$ROOT/tests/corpus/oracle-jnibridge.txt"
PKG=com.retrobug.jnibridge
OUT="$ROOT/build/jnibridge-check"
REPACK=/sdcard/Android/data/com.retrobug.launcher/files/repack
a() { MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" "$@"; }
to_host() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
mkdir -p "$OUT"

a shell rm -f "$REPACK/jnibridge.apk" "$REPACK/jnibridge.txt"
a shell am start -S -n com.retrobug.launcher/.LauncherActivity -e repack jnibridge -e install 0 >/dev/null
for _ in $(seq 1 60); do
  sleep 1
  a shell "test -s $REPACK/jnibridge.txt" 2>/dev/null && break
done
a pull "$REPACK/jnibridge.txt" "$(to_host "$OUT/repack.txt")" >/dev/null || { echo "no repack report — is jnibridge staged?" >&2; exit 1; }
grep -q "RESULT: repack OK" "$OUT/repack.txt" || { cat "$OUT/repack.txt" >&2; exit 1; }
grep -E "stub" "$OUT/repack.txt" | sed 's/^/repack: /'
a pull "$REPACK/jnibridge.apk" "$(to_host "$OUT/jnibridge-repacked.apk")" >/dev/null
a uninstall "$PKG" >/dev/null 2>&1 || true
a install "$(to_host "$OUT/jnibridge-repacked.apk")" >/dev/null

a logcat -c
a shell am start -S -n "$PKG/.BridgeActivity" >/dev/null
rows=""
for _ in $(seq 1 120); do
  sleep 1
  rows="$(a logcat -d -s BRIDGE:I | sed -n 's/.* BRIDGE *: //p' | tr -d '\r')"
  printf '%s\n' "$rows" | grep -q '^done ' && break
done
a logcat -d -s RBRUNTIME:* > "$OUT/runtime.log" || true
a shell am force-stop "$PKG"
printf '%s\n' "$rows" > "$OUT/bridged.txt"

fail=0; n=0
while IFS= read -r line; do
  case "$line" in '#'*|'') continue ;; esac
  n=$((n + 1))
  key="${line%%=*}"
  [ "$line" = "${line#done }" ] || key="done"
  got="$(grep -m1 -F -- "$key" "$OUT/bridged.txt" | grep -E "^$key(=| )" || true)"
  if [ "$got" = "$line" ]; then
    echo "PASS $key"
  else
    echo "FAIL $key"
    echo "  oracle:  $line"
    echo "  bridged: ${got:-<missing>}"
    fail=$((fail + 1))
  fi
done < "$ORACLE"
echo "bridge check on $SERIAL: $((n - fail)) of $n rows match the native oracle (runtime log: $OUT/runtime.log)"
[ "$fail" -eq 0 ]
