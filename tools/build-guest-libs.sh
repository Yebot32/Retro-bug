#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Build the guest's real 32-bit system libraries from the NDK's own archives. No AOSP checkout.
#
# Why this exists: `docs/33` established that the NDK's arm32 `libm.so`/`libz.so` *stubs* are names
# without bodies — `sin` is `Size 2`, a bare `bx lr`, so linking a guest app against a stub resolves
# every symbol and executes none of them. `docs/34` §1 found that the same NDK also ships the real
# compiled **static archives** (`libm.a`, `libz.a`), genuine ELF32-ARM `Tag_CPU_arch: v7` code, and
# that `--whole-archive` over them yields proper shared objects. This script is that recipe, so the
# guest's math and compression come from bionic's own implementation executed by the CPU core —
# which is the direction AD-1 takes — rather than from hand-written stand-ins.
#
# Two claims this script checks rather than asserts, because both were measured once by hand and a
# build that silently stops satisfying them is worse than one that fails:
#
#   1. **ABI-surface parity.** Every `FUNC` the stub for this API level exports must be defined by
#      what we build. `docs/34` measured 284/284 for `libm` and 93/93 for `libz`; `comm` re-checks it
#      on every build, so a shrink is a red build rather than an app that fails to load in M1.
#   2. **No text relocations.** No `DT_TEXTREL` and no `.rel.text`. With them, the guest linker would
#      need writable executable code, which is exactly what AD-7's W^X story and a host sepolicy that
#      denies `execmem` both refuse to give.
#
# It also prints the **undefined set** of each library. That list is the real deliverable for C11: it
# is the size of the host-provided surface a guest library actually needs, and for `libm` it is five
# symbols, not the ~1,300 a reader might fear.
#
# Usage: tools/build-guest-libs.sh            # builds every library in TARGETS below
#        tools/build-guest-libs.sh libm       # builds one

set -euo pipefail
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
API="${RB_GUEST_API:-34}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
BASE="$TC/sysroot/usr/lib/arm-linux-androideabi"
RT="$TC/lib/clang/19/lib/linux/libclang_rt.builtins-arm-android.a"
OUT="$ROOT/build/guest-libs"
RE="$TC/bin/llvm-readelf.exe"

for tool in "$TC/bin/clang.exe" "$RE"; do
  [ -x "$tool" ] || { echo "missing $tool — check RB_NDK ($NDK_VER)" >&2; exit 1; }
done

# name:extra archives. `libm` needs compiler-rt: without it the guest's `__aeabi_memclr4` and friends
# stay undefined, and with it four of the nine imports disappear. Linked as an ordinary archive, not
# whole-archive, or the builtins pull in every member and the "library" grows past libc.
declare -A ARCHIVES=(
  [libm]="$BASE/libm.a -Wl,--no-whole-archive $RT"
  [libz]="$BASE/libz.a"
)

# zlib's own internals, exported by the device's libz.so but LOCAL HIDDEN in the NDK's archive, so
# they are present in what we build yet not importable by name. Named individually rather than
# "allow anything zlib": an allowlist that fails open is how a real ABI regression gets waved through,
# and every one of these was checked to be present-as-hidden before it was listed here. Two of them
# (_dist_code, _length_code) are OBJECT data tables that the names-only stub advertises as FUNC.
declare -A KNOWN_HIDDEN=(
  [libz]="_dist_code _length_code _tr_align _tr_flush_bits _tr_flush_block _tr_init _tr_stored_block _tr_tally"
)

# A handful of entry points whose *body size* says whether this is an implementation or a name. `sqrt`
# is deliberately in the list: `docs/34` §1.2 found it is not a libm routine at all but four
# instructions of compiler-rt (`vmov`, `vsqrt.f64`, `vmov`, `bx lr` = 16 bytes), which is above the
# stub threshold but far below the others -- a real distinction worth having in the output, because it
# means a `sqrt`-heavy guest exercises the VFP unit and not the library.
declare -A PROBES=(
  [libm]="sin cos pow fmod sqrt"
  [libz]="deflateInit_ inflateInit_ inflate deflate inflateEnd"
)

mkdir -p "$OUT"
status=0

# `${@:-$(...)}` is the wrong shape here and failed loudly-but-confusingly the first time: command
# substitution keeps newlines inside one word, so "build every target" became one iteration with a
# two-line name and a stub path that could not exist.
if [ "$#" -gt 0 ]; then targets=("$@"); else targets=("${!ARCHIVES[@]}"); fi

for name in "${targets[@]}"; do
  echo "=========== $name.so"
  src="$BASE/$API/$name.so"                                  # the NDK stub = the requirement list
  [ -f "$src" ] || { echo "no stub at $src" >&2; status=1; continue; }

  "$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" \
    -mthumb -mfloat-abi=softfp -mfpu=neon -shared -nostdlib \
    -Wl,-soname,"$name.so" -Wl,--whole-archive ${ARCHIVES[$name]} -o "$OUT/$name.so"

  # --- gate 1: parity with the stub's exported functions -------------------------------
  # readelf's `--dyn-syms` columns are Num Value Size **Type** Bind Vis Ndx Name, so FUNC is field 4
  # and the section index is field 7. Getting that wrong does not error: both lists come back empty,
  # `comm` finds nothing missing, and the gate passes while checking nothing — so the non-empty check
  # below is part of the gate, not decoration.
  stub_names="$("$RE" --dyn-syms "$src" \
    | awk '$4=="FUNC"{print $NF}' | sed 's/@.*//' | sort -u)"
  built_names="$("$RE" --dyn-syms "$OUT/$name.so" \
    | awk '$4=="FUNC" && $7!="UND"{print $NF}' | sed 's/@.*//' | sort -u)"
  if [ -z "$stub_names" ]; then
    echo "  FAIL: parsed no FUNC symbols out of the stub $src — the gate cannot judge an empty list" >&2
    status=1
    continue
  fi
  n_stub=$(printf '%s\n' "$stub_names" | grep -c . || true)
  missing="$(comm -23 <(printf '%s\n' "$stub_names") <(printf '%s\n' "$built_names") | grep -v '^$' || true)"
  # Split the gap into the eight zlib internals already accounted for above and anything else. Only
  # the rest fails the build, and `known` is checked against the list rather than trusted: a name that
  # was allowlisted and has since *become* resolvable is reported too, so the list cannot quietly rot.
  known="${KNOWN_HIDDEN[$name]:-}"
  unknown=""; tolerated=""; stale=""
  while IFS= read -r m; do
    [ -n "$m" ] || continue
    case " $known " in
      *" $m "*) tolerated+="  $m"$'\n' ;;
      *)        unknown+="  $m"$'\n' ;;
    esac
  done <<< "$missing"
  for k in $known; do
    printf '%s\n' "$built_names" | grep -qx "$k" || continue
    stale+="  $k"$'\n'
  done
  n_unk=$(printf '%s' "$unknown" | grep -c . || true)
  echo "  defined $(printf '%s\n' "$built_names" | grep -c . || true)/$n_stub stub FUNC; $(printf '%s' "$tolerated" | grep -c . || true) known-hidden, $n_unk unexplained"
  if [ -n "$stale" ]; then
    echo "  NOTE: allowlisted as hidden but now exported normally -- shrink the list:" >&2
    printf '%s' "$stale" | sed 's/^/    /' >&2
  fi
  if [ "$n_unk" != 0 ]; then
    # "Missing" is not one finding but three, and collapsing them sends someone down the wrong path.
    # The code can be absent from the archive; present but not dynamically resolvable because the
    # archive hid it; or present under a different symbol *kind* than the stub advertises — and the
    # NDK stubs are names-only (docs/33 §2), so their kinds are not authoritative. libz's eight
    # internals are all of the second kind, and two of them (`_dist_code`, `_length_code`) are the
    # third: data tables the stub prints as `FUNC` with a 2-byte body.
    echo "  FAIL: $n_unk stub FUNC unaccounted for; classified:" >&2
    all_syms="$("$RE" --syms "$OUT/$name.so" 2>/dev/null)"
    while IFS= read -r m; do
      [ -n "$m" ] || continue
      row="$(printf '%s\n' "$all_syms" | awk -v s="$m" '$NF==s{print; exit}')"
      if [ -z "$row" ]; then
        printf '    absent     %s\n' "$m" >&2
      else
        bind=$(printf '%s' "$row" | awk '{print $5}')
        vis=$(printf '%s' "$row" | awk '{print $6}')
        typ=$(printf '%s' "$row" | awk '{print $4}')
        if [ "$bind" = "GLOBAL" ] && [ "$vis" = "DEFAULT" ]; then
          printf '    RESOLVABLE but not in .dynsym: %s (%s)\n' "$m" "$typ" >&2
        else
          printf '    present    %-16s as %s %s %s -- not importable by name\n' \
                 "$m" "$typ" "$bind" "$vis" >&2
        fi
      fi
    done <<< "$unknown"
    # (backticks here would be command substitution, which is how the first draft of this line managed
    # to "run" _tr_* and print an error in the middle of a failure report)
    echo "    A guest app imports only the public API; zlib's _tr_*/_*_code internals are zlib's own," >&2
    echo "    so this is a caveat to record rather than a blocker to libz.so for real apps." >&2
    status=1
  fi

  # --- gate 2: no writable-executable requirement --------------------------------------
  if "$RE" -d "$OUT/$name.so" | grep -q "TEXTREL"; then
    echo "  FAIL: DT_TEXTREL present — the guest linker would need writable code" >&2
    status=1
  fi
  if "$RE" -S "$OUT/$name.so" | grep -q "\.rel\.text"; then
    echo "  FAIL: .rel.text present — same consequence, found in the section table" >&2
    status=1
  fi
  echo "  no DT_TEXTREL, no .rel.text"
  echo "  relocations the guest linker must apply:"
  "$RE" -r "$OUT/$name.so" 2>/dev/null | grep -oE "R_ARM_[A-Z0-9_]+" | sort | uniq -c | sed 's/^/    /'

  # --- the host-provided surface this library still needs ------------------------------
  echo "  undefined symbols (what C11 must supply for this one to load):"
  "$RE" --dyn-syms "$OUT/$name.so" | awk '$7=="UND"{print $NF}' | sed 's/@.*//' | grep -vE '^$|^UND$' \
    | sort -u | sed 's/^/    /'
  # `--whole-archive` carries bionic's `-g` sections along with the code, which is 300+ KiB of debug
  # info per library and would be shipped inside every APK. Strip after the gates, so the symbol
  # comparisons above still see the linker's own view.
  "$TC/bin/llvm-strip.exe" --strip-unneeded "$OUT/$name.so"

  # The property that actually distinguishes a real guest library from a stub, and the one this whole
  # file exists to guarantee: `docs/33` §2 measured the NDK stub's `sin` as `Size 2` (a bare `bx lr`),
  # so a build that accidentally resolved its input to the stub directory instead of `libm.a` would
  # produce a library that loads, exports every name, and computes nothing. Size is the tell.
  #
  # (An earlier version of this line re-ran the parity check after stripping instead, on the theory
  # that strip might eat `.dynsym`. Tested with `--strip-all`: dynamic symbols survive every strip
  # mode, so that check could not have failed. This one can, and the negative control is the stub
  # itself -- `libz.so`/`libm.so` in $BASE/$API report Size 2 for exactly these names.)
  probe_fail() { echo "  FAIL: $1" >&2; status=1; }
  # Unquoted on purpose: this is a space-separated list to iterate over, and "$..." would hand the loop
  # one word containing every name. Names are matched with any `@@VERSION` suffix removed, which the
  # stubs carry and the built archive does not -- comparing `$NF` literally looked up `sin` in a table
  # of `sin@@LIBC` and reported every probe absent from both libraries alike.
  for probe in ${PROBES[$name]:-}; do
    sz="$("$RE" --dyn-syms "$OUT/$name.so" \
         | awk -v s="$probe" '{n=$NF; sub(/@.*/, "", n); if ($4=="FUNC" && n==s) {print $3; exit}}')"
    stub_sz="$("$RE" --dyn-syms "$src" \
         | awk -v s="$probe" '{n=$NF; sub(/@.*/, "", n); if ($4=="FUNC" && n==s) {print $3; exit}}')"
    case "$sz" in
      ''|0) probe_fail "$probe not exported by the stripped $name.so at all" ;;
      1|2|4) probe_fail "$probe is $sz bytes in the guest build -- a stub body ($probe is $stub_sz bytes in the NDK stub), so the input archive was wrong" ;;
      *) echo "  $probe: $sz bytes in the guest build (the stub's $stub_sz is a name, not code)" ;;
    esac
  done
  echo "  -> $OUT/$name.so ($(stat -c%s "$OUT/$name.so") bytes stripped, $(file -b "$OUT/$name.so" 2>/dev/null | cut -d, -f1-4))"
done

exit $status
