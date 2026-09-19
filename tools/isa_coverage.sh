#!/usr/bin/env bash
# RetroBug — ARMv7 instruction-demand histogram. SPDX-License-Identifier: Apache-2.0
#
# Measures which ARMv7 encodings real 32-bit Android code actually uses, so the engine's coverage
# can be judged against demand rather than against a guess. This is half of open risk U-8; the
# other half — whether implemented encodings are semantically *correct* — needs the differential
# oracle lanes in CI (see docs/32-isa-coverage.md §5).
#
# Bash + NDK tools only: no Python (absent from this machine, and docs/20 §1 forbids depending on it).
#
# Usage:  tools/isa_coverage.sh [output-dir]
#
# Corpus source: the NDK's own AArch32 archives. These are real compiled bionic/libm/libc++/zlib
# machine code for armv7 — exactly the instruction mix a guest's system libraries will present —
# and they are analysis input on this machine, never redistributed.

set -euo pipefail

OUT="${1:-build/isa-coverage}"
# Absolute, because the script cd's into $OUT/objs to extract archive members and would otherwise
# resolve the remaining paths relative to that directory.
OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK_VER="${RB_NDK:-28.2.13676358}"
TC="$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64"
ABIS="$TC/sysroot/usr/lib/arm-linux-androideabi"

for t in llvm-objdump llvm-ar; do
  command -v "$TC/bin/$t.exe" >/dev/null || { echo "missing $t — set RB_NDK or ANDROID_HOME" >&2; exit 1; }
done
[ -d "$ABIS" ] || { echo "no AArch32 sysroot at $ABIS" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/objs" "$OUT/asm"
cd "$OUT/objs"

# Extract every member of the static archives: each .o is a translation unit of real ARMv7 code.
"$TC/bin/llvm-ar.exe" x "$ABIS/libc.a"  2>/dev/null || true
"$TC/bin/llvm-ar.exe" x "$ABIS/libm.a"  2>/dev/null || true
cp "$ABIS/libc++_shared.so" "$ABIS/libz.a" . 2>/dev/null || true
N_OBJS=$(ls -1 | wc -l | tr -d ' ')

n=0
for f in "$OUT/objs"/*; do
  n=$((n+1))
  "$TC/bin/llvm-objdump.exe" -d --no-show-raw-insn "$f" 2>/dev/null \
    > "$OUT/asm/$(printf '%04d' "$n").txt" || true
done

# One mnemonic per line, with its raw count. objdump's AArch32 output has three shapes to get right:
#
#     142c:	movw	r0, #0x456        an instruction (--no-show-raw-insn drops the encoding column)
#      418:	01 00 00 00             .word / undecodable bytes inside an executable section
#      3c:	e0 0f 00 00 	and r0, r0, r0   bytes and a mnemonic on one line
#     Disassembly of section .text:   a section header, which also has to be excluded
#
# The first version anchored on "address, whitespace, lowercase" and so mistook the *byte column* of
# the third shape for the opcode (96 two-hex-digit rows entered the histogram, 3,481 instructions),
# while counting the instruction total with a looser separate pattern that included the second shape
# (docs/32 therefore published 338,034 instructions and 855 mnemonics; the corpus is 325,541 and
# 759). The second version dropped the anchor and picked up the fourth shape instead, which showed up
# as 2,686 instructions of a mnemonic called `of` — the word after "Disassembly".
#
# Both properties are needed together, so one awk does both: require an address-prefixed line, then
# skip any leading run of two-hex-digit byte columns, then take what is left. TOTAL is the sum of the
# histogram rather than an independent count, which is what stops the two disagreeing again.
cat "$OUT"/asm/*.txt | awk '{
  if ($0 ~ /^[[:space:]]*[0-9a-f]+:/) {
    for (i = 2; i <= NF && $i ~ /^[0-9a-f][0-9a-f]$/; i++) {}
    tok = $i
    if (tok ~ /^[a-z][a-z0-9.]*$/) print tok
  }
}' | sort | uniq -c | sort -rn > "$OUT/mnemonics.txt"

TOTAL=$(awk '{s += $1} END {print s + 0}' "$OUT/mnemonics.txt")
DISTINCT=$(wc -l < "$OUT/mnemonics.txt" | tr -d ' ')
VF=$(awk '$2 ~ /^v/ {s+=$1; n++} END {printf "%d %d", n+0, s+0}' "$OUT/mnemonics.txt")
IT=$(awk '$2 ~ /^it/ {s+=$1} END {print s+0}' "$OUT/mnemonics.txt")
T32=$(awk '$2 ~ /\.w$/ {s+=$1} END {print s+0}' "$OUT/mnemonics.txt")

{
  echo "corpus_objects=$N_OBJS"
  echo "instructions=$TOTAL"
  echo "distinct_mnemonics=$DISTINCT"
  echo "neon_vfp_mnemonics=$(echo "$VF" | cut -d' ' -f1)"
  echo "neon_vfp_instructions=$(echo "$VF" | cut -d' ' -f2)"
  echo "it_block_instructions=$IT"
  echo "thumb32_wide_instructions=$T32"
} > "$OUT/summary.txt"

cd - >/dev/null
echo "wrote $OUT/{mnemonics.txt,summary.txt}"
cat "$OUT/summary.txt"
