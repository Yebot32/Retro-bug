/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * What real ARMv7-A hardware returns for the instructions whose IR ops Tier I was refusing: `sdiv`,
 * `udiv`, `smull`/`umull`, `rev` and `clz`. Run as a 32-bit static executable on a 32-bit-capable device
 * (both the Reno 5 and the Tab S9), so every value printed here is hardware's answer and not this
 * project's reading of a manual.
 *
 * The interesting rows are the ones where C++ and ARM disagree, because those are the ones an
 * implementation written from the C++ operator gets wrong without anything noticing:
 *
 *   - a zero divisor is a **defined 0 result**, not a fault — `SIGFPE`/UB would be the C++ answer, and
 *     `atoi` reaching a divide-by-zero on malformed input is ordinary;
 *   - `INT_MIN / -1` **saturates to `INT_MIN`** (the truncation of +2^31 into 32 bits), where C++ is
 *     undefined and an `idiv` would trap on some hosts;
 *   - `clz #0` is **32**, the one case the host `__builtin_clz` refuses to define.
 *
 * The widening multiplies report both halves on one line: a `Mul64` whose low 32 bits are right and high
 * 32 bits are zero is exactly what an implementation that forgot to widen looks like, and nothing else
 * here would catch it.
 */
#include <stdint.h>
#include <stdio.h>

static uint32_t sdiv(uint32_t a, uint32_t b) {
  uint32_t r;
  __asm__ volatile("sdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}

static uint32_t udiv(uint32_t a, uint32_t b) {
  uint32_t r;
  __asm__ volatile("udiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}

static uint32_t clz(uint32_t a) {
  uint32_t r;
  __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(a));
  return r;
}

static uint32_t rev(uint32_t a) {
  uint32_t r;
  __asm__ volatile("rev %0, %1" : "=r"(r) : "r"(a));
  return r;
}

/* The pair form returns lo in the first register and hi in the second, which is the order `umull RdLo,
 * RdHi, Rm, Rs` names them — the same lo/hi split Dynarmic's `Smull`/`Umull` lower into a `Mul64`. */
static uint64_t umull64(uint32_t a, uint32_t b) {
  uint32_t lo, hi;
  __asm__ volatile("umull %0, %1, %2, %3" : "=r"(lo), "=r"(hi) : "r"(a), "r"(b));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t smull64(uint32_t a, uint32_t b) {
  uint32_t lo, hi;
  __asm__ volatile("smull %0, %1, %2, %3" : "=r"(lo), "=r"(hi) : "r"(a), "r"(b));
  return ((uint64_t)hi << 32) | lo;
}

/* Whether SDIV touching its defined-zero case writes CPSR is a question about the *guest flags*, and the
 * two tiers disagree on it, so hardware gets to answer it. Baseline first, because the flag of interest
 * could already be set by whatever ran before. */
static uint32_t sdiv_flags(uint32_t a, uint32_t b, uint32_t* out_pair) {
  uint32_t before = 0, after = 0, r = 0;
  __asm__ volatile("mrs %0, cpsr\n\t"
                   "sdiv %3, %1, %2\n\t"
                   "mrs %4, cpsr\n\t"
                   : "=&r"(before), "+r"(a), "+r"(b), "=&r"(r), "=&r"(after));
  *out_pair = (before << 8) | (after & 0xff);
  return r;
}

int main(void) {
  printf("sdiv  7/2          %08x\n", sdiv(7u, 2u));
  printf("sdiv  -7/2         %08x\n", sdiv(0xfffffff9u, 2u));
  printf("sdiv  5/-2         %08x\n", sdiv(5u, 0xffffffffu));
  printf("sdiv  MIN/-1       %08x\n", sdiv(0x80000000u, 0xffffffffu));
  printf("sdiv  5/0          %08x\n", sdiv(5u, 0u));
  printf("sdiv  0/0          %08x\n", sdiv(0u, 0u));
  printf("sdiv  -1/2         %08x\n", sdiv(0xffffffffu, 2u));
  printf("udiv  MAX/2        %08x\n", udiv(0xffffffffu, 2u));
  printf("udiv  5/0          %08x\n", udiv(5u, 0u));
  printf("udiv  0/0          %08x\n", udiv(0u, 0u));
  printf("clz   0            %08x\n", clz(0u));
  printf("clz   1            %08x\n", clz(1u));
  printf("clz   0x100        %08x\n", clz(0x100u));
  printf("clz   0x80000000   %08x\n", clz(0x80000000u));
  printf("clz   0x7fffffff   %08x\n", clz(0x7fffffffu));
  printf("rev   0x12345678   %08x\n", rev(0x12345678u));
  printf("rev   0x000000ff   %08x\n", rev(0xffu));
  printf("rev   0            %08x\n", rev(0u));
  printf("umull 0x10000^2    %016llx\n", (unsigned long long)umull64(0x10000u, 0x10000u));
  printf("umull 0xffffffff^2 %016llx\n", (unsigned long long)umull64(0xffffffffu, 0xffffffffu));
  printf("umull 7*0x100000000-ish %016llx\n", (unsigned long long)umull64(3u, 0x55555555u));
  printf("smull -1*0x10000   %016llx\n", (unsigned long long)smull64(0xffffffffu, 0x10000u));
  printf("smull -1*-1        %016llx\n", (unsigned long long)smull64(0xffffffffu, 0xffffffffu));
  printf("smull 0x80000000*2 %016llx\n", (unsigned long long)smull64(0x80000000u, 2u));
  /* The three rows below exist because everything above them has a product that fits in 32 bits, so an
   * implementation that multiplies in 32 and hands back zero in the high half would match hardware on
   * every one of them. Without a non-zero high half the pair says nothing about `Mul64` at all. */
  printf("umull MAX*0x03000000 %016llx\n", (unsigned long long)umull64(0xffffffffu, 0x03000000u));
  printf("umull MAX^2 hi only  %016llx\n", (unsigned long long)umull64(0xffffffffu, 0xffffffffu));
  printf("smull MIN*MIN        %016llx\n", (unsigned long long)smull64(0x80000000u, 0x80000000u));

  {
    uint32_t f = 0;
    uint32_t q;
    q = sdiv_flags(5u, 0u, &f);
    printf("sdiv 5/0             q=%08x cpsr before=%08x after=%08x\n", q, f >> 8, (uint32_t)(f & 0xff));
    q = sdiv_flags(0x80000000u, 0xffffffffu, &f);
    printf("sdiv MIN/-1          q=%08x cpsr before=%08x after=%08x\n", q, f >> 8, (uint32_t)(f & 0xff));
    q = sdiv_flags(7u, 2u, &f);
    printf("sdiv 7/2             q=%08x cpsr before=%08x after=%08x\n", q, f >> 8, (uint32_t)(f & 0xff));
  }
  return 0;
}
