/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * An ARMv7 **hardware** oracle for the numbers RetroBug's execution tiers must reproduce.
 *
 * Why this exists: the M0 exit test asserts `fib(20) == 6765`, and "RetroBug agrees with RetroBug" is
 * not evidence. AD-11 planned `qemu-arm` as the external oracle, which needs a Linux CI job. A device
 * that still has AArch32 at EL0 is better than both — it is the real instruction set, not an
 * emulator's model of it — and one is occasionally available here (docs/30-m0-notes.md §Step 5c: the
 * SM-X710 executes the same 32-bit binary the OnePlus 12 refuses with ENOEXEC). So the expected values
 * are taken from hardware and checked into the docs, and any tier that disagrees is the one that is
 * wrong — including the host compiler that produced the test's own arithmetic.
 *
 * It links against `fixture_jni.c` itself rather than repeating its expressions: a copy of `fib` in a
 * second file is a second thing to keep in sync, and the two entry points called here take `JNIEnv*`
 * and `jclass` only to discard them, so they are callable without a JVM.
 *
 * Built and run by tools/run_armv7_oracle.sh. `-static` so nothing depends on a loader resolving a
 * 32-bit `libc.so` on a device that may not have one.
 */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern jint Java_com_retrobug_fixture_MainActivity_fib(JNIEnv* env, jclass clazz, jint n);
extern jint Java_com_retrobug_fixture_MainActivity_wide(JNIEnv* env, jclass clazz, jint seed);

/// CP15 reads, one per invocation because a refused one kills the process and the useful answer is
/// *which* ones an AArch32 EL0 process may perform on this hardware. `mrc p15, 0, Rd, c0, c0, 0`
/// (MIDR) is the one docs/21 §E5 wants the guest to answer, so it is the one RetroBug must emulate —
/// and the table below is what tells us whether emulating it is faithful or merely permissive.
typedef struct Cp15Probe {
  const char* name;
  uint32_t (*read)(void);
} Cp15Probe;

#define PROBE(ID, OPS)                        \
  static uint32_t read_##ID(void) {           \
    uint32_t v = 0;                           \
    __asm__ volatile(OPS : [out] "=r"(v));    \
    return v;                                 \
  }

PROBE(midr, "mrc p15, 0, %[out], c0, c0, 0")
PROBE(ctr, "mrc p15, 0, %[out], c0, c0, 1")
PROBE(mpidr, "mrc p15, 0, %[out], c0, c0, 5")
PROBE(tpidrurw, "mrc p15, 0, %[out], c13, c0, 3")
PROBE(fpsid, "vmrs %[out], fpsid")

static const Cp15Probe kProbes[] = {
    {"midr", read_midr}, {"ctr", read_ctr}, {"mpidr", read_mpidr},
    {"tpidrurw", read_tpidrurw}, {"fpsid", read_fpsid},
};

/// A named probe, run as its own process by the caller so that a refusal is a data point rather than
/// the end of the run.
static int run_cp15(const char* name) {
  const Cp15Probe* p = NULL;
  for (size_t i = 0; i < sizeof kProbes / sizeof kProbes[0]; ++i) {
    if (strcmp(kProbes[i].name, name) == 0) {
      p = &kProbes[i];
      break;
    }
  }
  if (!p) {
    printf("unknown probe '%s'; have:", name);
    for (size_t i = 0; i < sizeof kProbes / sizeof kProbes[0]; ++i) printf(" %s", kProbes[i].name);
    printf("\n");
    return 2;
  }
  printf("%s: attempting\n", p->name);
  fflush(stdout);
  const uint32_t v = p->read();
  printf("%s: 0x%08x\n", p->name, v);
  return 0;
}

int main(int argc, char** argv) {
  // Unbuffered, deliberately: run through `adb shell` into a pipe, a block-buffered stdout is lost
  // entirely when the process dies on a signal, and then an oracle that crashed on its last line is
  // indistinguishable from one that crashed on its first. That ambiguity cost a cycle here.
  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc > 2 && strcmp(argv[1], "--cp15") == 0) return run_cp15(argv[2]);

  // Proof that this really ran as Thumb-2: in an ARMv7 function pointer, bit 0 is the ISA tag, and it
  // is set for every function in a -mthumb build. If this prints even, the build was ARM-mode and the
  // numbers below are not testing what M0 claims.
  const uintptr_t f = (uintptr_t)(void*)&Java_com_retrobug_fixture_MainActivity_fib;
  printf("thumb=%d fib_addr=0x%x\n", (int)(f & 1u), (unsigned)f);

  printf("fib(0)=%d fib(1)=%d fib(2)=%d fib(20)=%d fib(20)again=%d\n",
         Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 0),
         Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 1),
         Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 2),
         Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 20),
         Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 20));

  // The short end of the curve, measured rather than assumed. Tier I's fib cases use these as their
  // expected values, and a recursion whose first wrong output is at n=20 is far harder to localise
  // than one that breaks at n=4 — the whole point of printing the shape, not just two points.
  printf("fib-curve:");
  for (int n = 0; n <= 12; ++n) {
    printf(" %d", Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, n));
  }
  printf("\n");

  // wide_probe: table sum XOR the low byte of a movw/movt-materialised 0x123456.
  printf("wide(0)=%d wide(2)=%d wide(-1)=%d\n",
         Java_com_retrobug_fixture_MainActivity_wide(NULL, NULL, 0),
         Java_com_retrobug_fixture_MainActivity_wide(NULL, NULL, 2),
         Java_com_retrobug_fixture_MainActivity_wide(NULL, NULL, -1));

  // The exact string M1 renders on screen, from the same snprintf the guest will run.
  char buf[128];
  snprintf(buf, sizeof buf, "fib(20)=%d wide=%d", Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 20),
           Java_com_retrobug_fixture_MainActivity_wide(NULL, NULL, 2));
  printf("string=%s\n", buf);
  printf("cp15: run `--cp15 <name>` per register; a refused one kills this process\n");
  return 0;
}
