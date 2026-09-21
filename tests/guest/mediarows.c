/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * What real AArch32 hardware says about the ARMv5TE/v6 media instructions, on the same bytes
 * RetroBug's two tiers execute.
 *
 * Why this exists as a separate binary rather than as another column in the differential lane: the lane
 * compares Tier I with Tier J, and on 2026-09-21 it came back with **13 of its 16 media rows disagreeing
 * between the two tiers, identically on both test devices** — Tier J reporting, in every divergent row,
 * the result and the GE bits the *leading* `uadd8` had produced, as if the instruction under test had not
 * run. Two implementations of the same bytes disagreeing is a question; it is not an answer, and the lane
 * cannot supply one. `docs/30` §Step 34 recorded the same lesson once already: the differential finds a
 * divergence, and only hardware says which side is wrong.
 *
 * It also settles a claim this project currently has backwards. An earlier probe concluded from `SIGILL`
 * that the media instructions are "undefined at EL0" on both SoCs — which was used to justify *not*
 * implementing `SEL`, and the device's own `libc.so` then refused `PackedSelect` for `strcmp`. A SIGILL
 * proves the *encoding that was tried* was not executed, which for an ARM-state-only instruction handed
 * to a Thumb decoder is the same observable as an unavailable instruction. Here the encodings come from
 * `llvm-objdump` over `media_rows.s`, assembled `-mthumb`, and the exact same halfwords the emulator
 * runs — so a SIGILL in this output is a fact about the instruction, and a printed value is a fact about
 * the hardware's answer.
 *
 * Row order is the sequence printed, and each row is announced *before* it is executed: if a row takes
 * `SIGILL`, the last line of output names it, and everything after it is simply unknown rather than
 * silently absent. `stdout` is unbuffered for the same reason (`docs/30`'s standing-facts table).
 *
 * Build and run (a 32-bit ARM executable, run natively on a 32-bit-capable device — see the fleet note in
 * `docs/30`: neither attached device can prove RetroBug emulates anything, which is exactly why they can
 * be asked what these instructions do):
 *
 *   clang --target=armv7-none-linux-androideabi26 -mthumb -mfloat-abi=softfp -mfpu=neon \
 *     -static -O0 mediarows.c media_rows.s -o mediarows
 *   adb push mediarows /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/mediarows \
 *     && adb shell /data/local/tmp/mediarows
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The row blobs, assembled from media_rows.s. One label per instruction; the operand pairs below are
 * what make one label carry several rows. */
extern void row_uadd8(void);
extern void row_usub8(void);
extern void row_sadd8(void);
extern void row_ssub8(void);
extern void row_uqsub8(void);
extern void row_uqadd8(void);
extern void row_qsub8(void);
extern void row_uhadd8(void);
extern void row_uhsub8(void);
extern void row_shadd8(void);
extern void row_uadd16(void);
extern void row_usub16(void);
extern void row_sel(void);

struct Row {
  const char* name;
  void (*fn)(void);
  uint32_t a, b;          /* operands, at [r0] and [r0,#4] */
  uint32_t want_r0;       /* Tier I's expectation, copied from the lane's table verbatim */
  uint32_t want_ge;
};

/*
 * The expectation column below is `core/tests/exec_tier_diff_test.cpp`'s `kMediaRows`, byte for byte.
 * Duplicating it is deliberate: if the two ever disagree, the disagreement is between the hardware
 * reading this prints and the emulator reading the lane prints, and there is no third copy to hide
 * behind.
 */
static const struct Row kRows[] = {
    {"uadd8 all-carry", row_uadd8, 0xFFFFFFFFu, 0x01010101u, 0x00000000u, 15u},
    {"uadd8 no-carry", row_uadd8, 0x7F7F7F7Fu, 0x01010101u, 0x80808080u, 0u},
    {"uadd8 one lane", row_uadd8, 0x00FF7F80u, 0x01010101u, 0x01008081u, 4u},
    {"usub8 mixed borrow", row_usub8, 0x01000080u, 0x01010101u, 0x00FFFF7Fu, 9u},
    {"sadd8 zero lane", row_sadd8, 0x01FF007Fu, 0x01010101u, 0x02000180u, 15u},
    {"sadd8 all-negative", row_sadd8, 0x80808080u, 0xFFFFFFFFu, 0x7F7F7F7Fu, 0u},
    {"ssub8 signed-only", row_ssub8, 0x00000000u, 0xFFFFFFFFu, 0x01010101u, 15u},
    {"uqsub8 no clamp", row_uqsub8, 0xFFFFFFFFu, 0x01010101u, 0xFEFEFEFEu, 15u},
    {"uqadd8 clamped", row_uqadd8, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 15u},
    {"qsub8 clamped", row_qsub8, 0x80808080u, 0x01010101u, 0x80808080u, 0u},
    {"uhadd8 truncating", row_uhadd8, 0x7E7E7E7Eu, 0x01010101u, 0x3F3F3F3Fu, 0u},
    {"uhsub8 through zero", row_uhsub8, 0x00000000u, 0x02020202u, 0xFFFFFFFFu, 0u},
    {"shadd8 positive", row_shadd8, 0x7F7F7F7Fu, 0x7F7F7F7Fu, 0x7F7F7F7Fu, 0u},
    {"uadd16 lane map", row_uadd16, 0x0000FFFFu, 0x00010001u, 0x00010000u, 2u},
    {"usub16 lane map", row_usub16, 0x00010000u, 0x00000001u, 0x0001FFFFu, 8u},
    {"sel from mixed GE", row_sel, 0x00FF7F80u, 0x01010101u, 0x01FF0101u, 4u},
};

/*
 * Run one blob and read back both of its results.
 *
 * The blob returns its value in `r0` and CPSR's GE nibble in `r3`, and no C type carries that pair, so
 * the call is written out: `r0` is pointed at the operand pair, and `blx` on a function pointer whose bit
 * 0 is set (which it is, because these are `.thumb_func` symbols) enters in Thumb state. `r1`, `r2` and
 * `r4` are listed as clobbered because the blob really does use them; `r3` is an *output*, since that is
 * where the blob puts the GE bits.
 */
static uint64_t run_row(void (*fn)(void), const uint32_t* ops) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)ops;
  register uint32_t r3 __asm__("r3") = 0u;
  register void* f __asm__("r12") = (void*)(uintptr_t)((uintptr_t)fn | 1u);
  __asm__ volatile("blx %2"
                   : "+r"(r0), "+r"(r3)
                   : "r"(f)
                   : "memory", "lr", "r1", "r2", "r4");
  return ((uint64_t)r3 << 32) | r0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0); /* a SIGILL mid-list must not eat the lines before it */

  unsigned fails = 0, ran = 0;
  for (unsigned i = 0; i < sizeof kRows / sizeof kRows[0]; ++i) {
    const struct Row* r = &kRows[i];
    uint32_t ops[2];
    ops[0] = r->a;
    ops[1] = r->b;
    /* Announced first: if this row faults, the tail of the output names it. */
    printf("[orace] %-20s A=%08x B=%08x ... ", r->name, r->a, r->b);
    const uint64_t got = run_row(r->fn, ops);
    const uint32_t got_r0 = (uint32_t)got;
    const uint32_t got_ge = (uint32_t)(got >> 32) & 0xFu;
    ++ran;
    const int ok = got_r0 == r->want_r0 && got_ge == r->want_ge;
    if (!ok) ++fails;
    printf("r0=%08x ge=%u  %s   (lane wants r0=%08x ge=%u)\n", got_r0, got_ge, ok ? "MATCHES" : "DIFFERS",
           r->want_r0, r->want_ge);
    /* The operands must survive: a media instruction writing back through its address operand would be a
     * different bug in a different place, and this is the only place that can see it here. */
    if (ops[0] != r->a || ops[1] != r->b) {
      printf("[orace] %-20s OPERAND SLOT OVERWRITTEN: %08x %08x\n", r->name, ops[0], ops[1]);
      ++fails;
    }
  }
  printf("%u rows ran, %u differ from the lane's expectation\n", ran, fails);
  return fails == 0 ? 0 : 1;
}
