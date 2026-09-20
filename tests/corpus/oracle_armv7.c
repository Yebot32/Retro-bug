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
extern jint Java_com_retrobug_fixture_MainActivity_it_probe(JNIEnv* env, jclass clazz, jint a,
                                                            jint b);
extern jint Java_com_retrobug_fixture_MainActivity_patch_probe(JNIEnv* env, jclass clazz, jint a,
                                                               jint b);
extern jint Java_com_retrobug_fixture_MainActivity_patch_probe_b(JNIEnv* env, jclass clazz, jint a,
                                                                 jint b);
extern jint Java_com_retrobug_fixture_MainActivity_jni_utf(JNIEnv* env, jclass clazz, jint seed);
extern jint Java_com_retrobug_fixture_MainActivity_fp_arith(JNIEnv* env, jclass clazz, jint a,
                                                            jint b);
extern jint Java_com_retrobug_fixture_MainActivity_fp_round(JNIEnv* env, jclass clazz, jint a);
extern jint Java_com_retrobug_fixture_MainActivity_fpsr_flags(JNIEnv* env, jclass clazz, jint a,
                                                              jint b);
extern jint Java_com_retrobug_fixture_MainActivity_fp_stack(JNIEnv* env, jclass clazz, jint a,
                                                            jint b);
extern jint Java_com_retrobug_fixture_MainActivity_fp_args(JNIEnv* env, jclass clazz, jint a,
                                                           jint b);
extern jint Java_com_retrobug_fixture_MainActivity_fp_nan(JNIEnv* env, jclass clazz, jint a);
extern jint Java_com_retrobug_fixture_MainActivity_fp_cmp(JNIEnv* env, jclass clazz, jint mode);
extern jint Java_com_retrobug_fixture_MainActivity_simd_int(JNIEnv* env, jclass clazz, jint a,
                                                            jint b);
extern jint Java_com_retrobug_fixture_MainActivity_simd_lane(JNIEnv* env, jclass clazz, jint mode,
                                                             jint a);
extern jint Java_com_retrobug_fixture_MainActivity_simd_bits(JNIEnv* env, jclass clazz, jint mode,
                                                             jint a);

/*
 * A JNIEnv for the guest to call out through, so the oracle can measure the *JNI call path* on real
 * silicon and not only the arithmetic.
 *
 * The table is initialised by member name, which means the C compiler resolves NewStringUTF's byte
 * offset from jni.h and the guest resolves it again from its own `ldr [r2, #0x29c]`. Two independent
 * derivations of the same number, agreeing, is what makes the offset evidence rather than a transcription.
 */
static int g_utf_calls;
static const char* g_last_utf;

static jstring oracle_NewStringUTF(JNIEnv* env, const char* utf) {
  (void)env;
  ++g_utf_calls;
  g_last_utf = utf; /* still the guest's own bytes; the oracle shares their address space */
  return (jstring)(intptr_t)0x7ac0ffee;
}

static const struct JNINativeInterface g_oracle_table = {.NewStringUTF = oracle_NewStringUTF};
static const struct JNINativeInterface* g_oracle_env = &g_oracle_table;

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

  // it_probe: real IT blocks, narrow and 32-bit, taken on both sides of each condition. The values are
  // the arithmetic `docs/30` §Step 7 needs to assert against, and the four shapes are chosen so each of
  // the four predicated slots is exercised by at least one of them.
  printf("it_probe:");
  static const jint it_cases[][2] = {{7, 3}, {3, 7}, {5, 5}, {101, 1}, {0, 0}, {-5, 3}};
  for (unsigned i = 0; i < sizeof it_cases / sizeof it_cases[0]; ++i) {
    printf(" %d", Java_com_retrobug_fixture_MainActivity_it_probe(NULL, NULL, it_cases[i][0],
                                                                  it_cases[i][1]));
  }
  printf("\n");

  // patch_probe / patch_probe_b: the code-cache invalidation case. Two functions whose machine code
  // differs in exactly one byte — the immediate of one leg of the first IT block — measured on silicon
  // so the test's expectation of *patched* code is a hardware number rather than something RetroBug
  // computed about itself. (3,7) takes the patched leg and so separates them; (7,3) takes the other leg
  // and must therefore agree, which is what shows the patch is local to the byte written.
  {
    typedef jint (*probe_fn)(JNIEnv*, jclass, jint, jint);
    static const probe_fn probes[] = {Java_com_retrobug_fixture_MainActivity_patch_probe,
                                      Java_com_retrobug_fixture_MainActivity_patch_probe_b};
    static const jint pairs[][2] = {{3, 7}, {7, 3}, {0, 0}};
    for (unsigned p = 0; p < 2; ++p) {
      printf("patch_probe%s:", p == 0 ? "" : "_b");
      for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        printf(" %d", probes[p](NULL, NULL, pairs[i][0], pairs[i][1]));
      }
      printf("\n");
    }
  }

  // The VFP/NEON group, one row per probe, five argument pairs per row.
  //
  // Why these are here at all: Tier I implements one vector IR opcode out of the ~70 the A32 front end
  // can emit, so the interesting question is not whether the tiers agree with each other — they can
  // agree while both being wrong — but which of them agrees with silicon, per instruction group. These
  // rows are the hardware column of that table, from the same compiled text both tiers load.
  {
    // (100, -50) is the pair that makes `t < u` reachable: the other four all take the `t > u` branch,
    // which is how the first capture of this row came back with five identical numbers — a probe whose
    // argument set cannot reach both legs of a compare measures the compare's absence, not its presence.
    static const jint pairs[][2] = {{3, 7}, {7, 3}, {-5, 3}, {100, -50}, {0, 1}};
    static const unsigned npairs = sizeof pairs / sizeof pairs[0];
    typedef jint (*fn2)(JNIEnv*, jclass, jint, jint);
    typedef jint (*fn1)(JNIEnv*, jclass, jint);
    const struct {
      const char* label;
      fn2 f2;
    } two[] = {{"fp_arith", Java_com_retrobug_fixture_MainActivity_fp_arith},
               {"fpsr_flags", Java_com_retrobug_fixture_MainActivity_fpsr_flags},
               {"fp_stack", Java_com_retrobug_fixture_MainActivity_fp_stack},
               {"fp_args", Java_com_retrobug_fixture_MainActivity_fp_args}};
    for (unsigned p = 0; p < sizeof two / sizeof two[0]; ++p) {
      printf("%s:", two[p].label);
      for (unsigned i = 0; i < npairs; ++i) {
        printf(" %d", two[p].f2(NULL, NULL, pairs[i][0], pairs[i][1]));
      }
      printf("\n");
    }
    const struct {
      const char* label;
      fn1 f1;
    } one[] = {{"fp_round", Java_com_retrobug_fixture_MainActivity_fp_round},
               {"fp_nan", Java_com_retrobug_fixture_MainActivity_fp_nan}};
    for (unsigned p = 0; p < sizeof one / sizeof one[0]; ++p) {
      printf("%s:", one[p].label);
      for (unsigned i = 0; i < npairs; ++i) {
        printf(" %d", one[p].f1(NULL, NULL, pairs[i][0] - 2));
      }
      printf("\n");
    }
    printf("simd_int:");
    const fn2 simd = Java_com_retrobug_fixture_MainActivity_simd_int;
    for (unsigned i = 0; i < npairs; ++i) {
      printf(" %d", simd(NULL, NULL, pairs[i][0], pairs[i][1]));
    }
    printf("\n");

    // simd_lane / simd_bits: the first argument is the *mode* (a lane width, or a logical operation),
    // not an arithmetic operand, so this pair set is its own — {mode, seed} with all five modes covered.
    // Row 4 repeats the 64-bit path of `simd_lane` with different data rather than inventing a fifth
    // width, because ARMv7 has exactly four integer lane widths.
    static const jint simd_modes[][2] = {{0, 3}, {1, 7}, {2, -5}, {3, 100}, {4, -2}};
    printf("simd_lane:");
    for (unsigned i = 0; i < sizeof simd_modes / sizeof simd_modes[0]; ++i) {
      printf(" %d", Java_com_retrobug_fixture_MainActivity_simd_lane(
                        NULL, NULL, simd_modes[i][0], simd_modes[i][1]));
    }
    printf("\n");
    printf("simd_bits:");
    for (unsigned i = 0; i < sizeof simd_modes / sizeof simd_modes[0]; ++i) {
      printf(" %d", Java_com_retrobug_fixture_MainActivity_simd_bits(
                        NULL, NULL, simd_modes[i][0], simd_modes[i][1]));
    }
    printf("\n");

    // fp_cmp: the FPSCR compare field per relation, in hex, because reading it as decimal invites a
    // transcription error on the exact bits this row exists to establish. Less, equal, greater,
    // unordered-with-raise, unordered-without.
    printf("fp_cmp(nibble<<8|sticky):");
    for (int m = 0; m <= 4; ++m) {
      printf(" 0x%03x", Java_com_retrobug_fixture_MainActivity_fp_cmp(NULL, NULL, m) & 0xfff);
    }
    printf("\n");
  }

  // jni_utf, against a JNIEnv built right here.
  //
  // The table is initialised by *member name*, so the compiler resolves NewStringUTF's offset from the
  // same header the fixture was compiled against; the guest then reaches it through its own
  // `ldr [r2, #0x29c]`. If RetroBug's own table put the function anywhere else, its guest would call a
  // different slot, this counter would stay at zero, and the returned value would differ — which makes
  // the offset a measured fact on both sides rather than a number transcribed from a specification.
  g_utf_calls = 0;
  g_last_utf = NULL;
  const jint u1 = Java_com_retrobug_fixture_MainActivity_jni_utf(&g_oracle_env, NULL, 1);
  const jint u0 = Java_com_retrobug_fixture_MainActivity_jni_utf(&g_oracle_env, NULL, 0);
  printf("jni_utf(1)=%d jni_utf(0)=%d calls=%d utf=%s\n", u1, u0, g_utf_calls,
         g_last_utf != NULL ? g_last_utf : "(null)");

  // The exact string M1 renders on screen, from the same snprintf the guest will run.
  char buf[128];
  snprintf(buf, sizeof buf, "fib(20)=%d wide=%d", Java_com_retrobug_fixture_MainActivity_fib(NULL, NULL, 20),
           Java_com_retrobug_fixture_MainActivity_wide(NULL, NULL, 2));
  printf("string=%s\n", buf);
  printf("cp15: run `--cp15 <name>` per register; a refused one kills this process\n");
  return 0;
}
