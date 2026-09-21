/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Hardware ground truth for the ARMv7 preload hints (`pld`, and the Thumb encodings RetroBug's lane
 * runs), at three address classes: readable, mapped-`PROT_NONE`, and a hole left by `munmap`.
 *
 * Why this binary exists: `core/tests/exec_tier_diff_test.cpp` asserts that Tier I executes a preload as
 * the no-op the architecture describes, and the only evidence behind that line before 2026-09-21 was that
 * Tier J already continued. Two tiers agreeing is not evidence about hardware, and `docs/30` had already
 * been bitten twice that way in the same week (§Step 35's media lane and §Step 36's code-cache artifact).
 * `pld` is also the instruction standing between RetroBug and bionic's selected `memmove_a15`, so "what a
 * real core does with it" is not a curiosity.
 *
 * The three classes are the part a single-row probe cannot answer. "It did not fault" is worth nothing
 * unless the same address is shown to fault for a real load, so `c1_arm_ldr_control` runs against each
 * class in a forked child: readable must survive, `PROT_NONE` and the hole must die. If the child for the
 * hole returns, the hole is not a hole and every hole row above it is meaningless -- which is why this
 * binary prints that case as a failure rather than as a pass.
 *
 * Rows are announced before they are executed and stdout is unbuffered, so a fault names the row that
 * took it and leaves the rest explicitly unknown instead of quietly absent.
 *
 * Build and run (native 32-bit ARM, so it needs a 32-bit-capable device -- an oracle, never the launch
 * proof; see the fleet note in `docs/30`):
 *
 *   clang --target=armv7-none-linux-androideabi26 -mthumb -mfloat-abi=softfp -mfpu=neon \
 *     -static -O0 pldrows.c pld_rows.s -o pldrows
 *   adb push pldrows /data/local/tmp/ && adb shell "chmod 755 /data/local/tmp/pldrows; /data/local/tmp/pldrows"
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SENT 0x11223344u

extern uint64_t p1_arm_pld_neg(uint32_t sentinel, uint32_t addr);
extern uint64_t p2_arm_pld_pos(uint32_t sentinel, uint32_t addr);
extern uint64_t p3_arm_pld_plain(uint32_t sentinel, uint32_t addr);
extern uint64_t p4_arm_pld_flags(uint32_t sentinel, uint32_t addr);
extern uint64_t t1_thumb_pld_r0(uint32_t addr, uint32_t junk);
extern uint64_t t2_thumb_pld_imm(uint32_t addr, uint32_t junk);
extern uint64_t c1_arm_ldr_control(uint32_t sentinel, uint32_t addr);

static unsigned pass = 0, fail = 0;

/* An ARM row: r0 goes in as SENT, r1 as `addr`; the hint must neither stop the block nor touch r1.
 * `delta` is what the two `add`s around the hint owe us, and 0 is the flag row's answer. */
static void arm_row(const char* name, uint64_t (*fn)(uint32_t, uint32_t), uint32_t delta,
                    const char* cls, uint32_t addr) {
  printf("%-22s @ %-9s 0x%08x ... ", name, cls, addr);
  fflush(stdout);
  const uint64_t got = fn(SENT, addr);
  const uint32_t lo = (uint32_t)got, hi = (uint32_t)(got >> 32);
  const int ok = (lo == SENT + delta) && (hi == addr);
  printf("%s lo=0x%08x hi=0x%08x (want lo=0x%08x hi=0x%08x)\n", ok ? "OK  " : "BAD ", lo, hi,
         SENT + delta, addr);
  if (ok) ++pass; else ++fail;
}

/* A Thumb row addresses through r0, so the halves swap: r0 must come back holding the address it was
 * given and r1 carries the row's own marker past the hint. */
static void thumb_row(const char* name, uint64_t (*fn)(uint32_t, uint32_t), const char* cls,
                      uint32_t addr) {
  printf("%-22s @ %-9s 0x%08x ... ", name, cls, addr);
  fflush(stdout);
  const uint64_t got = fn(addr, 0xdeadbeefu);
  const uint32_t lo = (uint32_t)got, hi = (uint32_t)(got >> 32);
  const int ok = (lo == addr) && (hi == 0xa6u);
  printf("%s lo=0x%08x hi=0x%08x (want lo=0x%08x hi=0x000000a6)\n", ok ? "OK  " : "BAD ", lo, hi,
         addr);
  if (ok) ++pass; else ++fail;
}

/* Does a real load from this address fault? It must for the two classes that are not readable, and that
 * is what makes "the preload did not fault" a measurement instead of a tautology. Run in a child because
 * the expected outcome is a signal. */
static const char* control_outcome(const char* cls, uint32_t addr) {
  fflush(stdout);
  pid_t kid = fork();
  if (kid < 0) return "fork failed";
  if (kid == 0) {
    volatile uint64_t r = c1_arm_ldr_control(SENT, addr);
    (void)r;
    _exit(0);
  }
  int status = 0;
  if (waitpid(kid, &status, 0) < 0) return "wait failed";
  if (WIFSIGNALED(status)) {
    printf("ldr @ %-9s 0x%08x -> SIG%d\n", cls, addr, WTERMSIG(status));
    return "";
  }
  printf("ldr @ %-9s 0x%08x -> survived, exit %d\n", cls, addr, WEXITSTATUS(status));
  return "";
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const long page = sysconf(_SC_PAGESIZE);
  printf("page=%ld  sizeof(void*)=%zu\n", page, sizeof(void*));

  const size_t len = (size_t)page * 3u;
  char* readable = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (readable == MAP_FAILED) {
    printf("FATAL: could not map the readable page\n");
    return 2;
  }
  memset(readable, 0x5a, len);
  char* denied = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (denied == MAP_FAILED) {
    printf("FATAL: could not map the PROT_NONE page\n");
    return 2;
  }
  char* got = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (got == MAP_FAILED) {
    printf("FATAL: could not reserve the hole\n");
    return 2;
  }
  if (munmap(got, len) != 0) {
    printf("FATAL: could not unmap the hole\n");
    return 2;
  }

  /* +2 pages, so a +/-0x400 offset from it is still inside the same class three pages away. */
  const uint32_t r_read = (uint32_t)(uintptr_t)(readable + page * 2);
  const uint32_t r_deny = (uint32_t)(uintptr_t)(denied + page * 2);
  const uint32_t r_hole = (uint32_t)(uintptr_t)(got + page * 2);
  printf("\n-- preload hints, three address classes (want: no fault, block continues, nothing changed) --\n");
  for (int cls = 0; cls < 3; ++cls) {
    const char* name = cls == 0 ? "readable" : (cls == 1 ? "prot_none" : "hole");
    const uint32_t a = cls == 0 ? r_read : (cls == 1 ? r_deny : r_hole);
    arm_row("pld [r1,#-0x400] arm", p1_arm_pld_neg, 2, name, a);
    arm_row("pld [r1,#0x400] arm", p2_arm_pld_pos, 2, name, a);
    arm_row("pld [r1] arm", p3_arm_pld_plain, 2, name, a);
    arm_row("pld+flags arm", p4_arm_pld_flags, 0, name, a);
    thumb_row("pld [r0] thumb", t1_thumb_pld_r0, name, a);
    thumb_row("pld [r0,#0x100] thumb", t2_thumb_pld_imm, name, a);
    control_outcome(name, a);
  }

  printf("\nsummary: %u OK, %u BAD\n", pass, fail);
  return fail != 0;
}
