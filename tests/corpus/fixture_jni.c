/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * M0 test fixture: a real armeabi-v7a JNI shared object, built by tools/run_engine_tests.sh with
 * the NDK. Deliberately tiny and deliberately ARMv7-flavoured, so the loader and the execution
 * tiers are exercised against code that could not have come from a 64-bit build:
 *
 *   - Thumb-2 only function bodies (-mthumb, as Android ships armeabi-v7a)
 *   - movw/movt, i.e. ARMv7-only encodings
 *   - a recursive call graph and a table lookup, so branch/link and PC-relative addressing both run
 *   - one JNI function returning a formatted string, which is the shape AD-5's binding path expects
 *
 * This same file becomes the M0 exit test's fib(20) == 6765 and, in M1, the app whose launch proves
 * end-to-end emulation on the OnePlus 12 (docs/21 §3).
 */
#include <jni.h>
#include <stdio.h>

/* ARMv7-only materialisations: 0x12345 requires movw+movt, not a single literal move. */
static const unsigned kTable[8] = {3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u};

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* Table sum plus a movw/movt-formed constant: fails on any engine missing Thumb-2 wide moves. */
static int wide_probe(int seed) {
  unsigned acc = seed;
  for (unsigned i = 0; i < 8; ++i) acc += kTable[i];
  unsigned big;
  __asm__("movw %0, #0x456\n\t"
          "movt %0, #0x123\n\t"
          : "=r"(big));
  return (int)(acc ^ (big & 0xff));
}

JNIEXPORT jstring JNICALL
Java_com_retrobug_fixture_MainActivity_stringFromJNI(JNIEnv* env, jobject thiz) {
  (void)thiz;
  char buf[128];
  snprintf(buf, sizeof buf, "fib(20)=%d wide=%d", fib(20), wide_probe(2));
  return (*env)->NewStringUTF(env, buf);
}

/* Numeric entry points, so tier equivalence can be asserted on values rather than strings. */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fib(JNIEnv* env, jclass clazz,
                                                                  jint n) {
  (void)env; (void)clazz;
  return fib(n);
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_wide(JNIEnv* env, jclass clazz,
                                                                    jint seed) {
  (void)env; (void)clazz;
  return wide_probe(seed);
}

/*
 * A guest call *out* through the JNIEnv function table, with no libc dependency at all.
 *
 * Why it exists: `stringFromJNI` reaches `NewStringUTF` only after a `snprintf`, and `snprintf` is an
 * import — so testing the JNI call path with it would entangle C08 (the bridge) with C11 (guest
 * libraries), and a failure would name neither. This one is the call path alone: `(*env)->NewStringUTF`
 * is `ldr r3,[r0]; ldr r3,[r3,#0x29c]; blx r3`, and the return value depends only on the handle being
 * non-null plus a byte of the string the host received.
 *
 * It also makes the JNI table offset *falsifiable* rather than copied from a header: `tools/run_armv7_oracle.sh`
 * builds its own fake `JNIEnv` on real ARMv7 silicon, so if RetroBug's offset is wrong the guest calls
 * some other entry, the oracle's stub never runs, and the number differs.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_jni_utf(JNIEnv* env, jclass clazz,
                                                                     jint seed) {
  (void)clazz;
  static const char probe[] = "retrobug";
  void* made = (*env)->NewStringUTF(env, probe);
  return made == NULL ? 0 : seed + (jint)probe[0];
}

/*
 * An explicit IT block, written in assembly so the compiler gets no vote on whether the encoding is
 * present.
 *
 * `docs/32` §2.1 measured 8,387 IT-block instructions in bionic and named IT-block TB splitting as the
 * single biggest correctness risk in the engine, and nothing else in this fixture predicates anything
 * — clang prefers `it`-free conditional select for the shapes a C `if`-`else`-assignment produces at
 * `-Os`. So this is the coverage hole closed as directly as a fixture can close it: two IT blocks, each
 * with the taken leg on one side of its condition and the not-taken leg on the other, across four call
 * shapes. What it still does *not* cover is an IT sequence straddling a translation-block boundary,
 * which needs a branch in the middle of one and cannot be forced from C or from inline asm in a way
 * that survives the translator's own block-splitting choice. That case stays open and is named as such
 * in `docs/30` §Step 7.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_it_probe(JNIEnv* env, jclass clazz,
                                                                      jint a, jint b) {
  (void)env; (void)clazz;
  jint out;
  /*
   * `ite`, not `it`: LLVM's integrated assembler reads a bare `it gt` as covering exactly one
   * instruction and rejects the second predicated slot with "predicated instructions must be in IT
   * block", while GNU as accepts it for the same encoding. Measured both ways against this toolchain
   * before settling. The two blocks also differ in width on purpose — `#3`/`#5` fit the 16-bit
   * conditional forms and `#1000` cannot, so one IT block carries narrow predicates and the other
   * 32-bit ones, which are separate paths through a decoder.
   */
  __asm__ volatile(
      "cmp   %[x], %[y]\n"
      "ite   gt\n"
      "addgt %[o], %[x], #3\n"
      "addle %[o], %[x], #5\n"
      "cmp   %[o], #10\n"
      "ite   lt\n"
      "addlt %[o], %[o], #1000\n"
      "orrge %[o], %[o], #0x20\n"
      : [o] "=&r"(out)
      : [x] "r"(a), [y] "r"(b)
      : "cc");
  return out;
}

/*
 * Two functions whose machine code differs in exactly one byte, for the code-cache invalidation test.
 *
 * The first attempt at that test patched a bit of `it_probe` and asserted the answer would change. It
 * does not: inside an IT block the legs are encoded *unpredicated* — the condition lives in CPSR's IT
 * field, not in the instruction — so `addgt` and `addle` differ only in their immediates, and the bit
 * that was flipped happened to be one that neither leg's arithmetic cared about. The test then "passed"
 * a patch that patched nothing, and its own author had to notice. So the byte being written here is
 * known-bad-by-construction: the assertion is not "some other number came out" but "the number the
 * *other* function returns on hardware came out", which is only reachable if the old code really is
 * gone from the cache.
 *
 * Bodies are complete asm definitions with fixed registers, for two reasons: the compiler must not
 * choose a different register allocation between the two (that would put differences in bytes nobody
 * intended), and the shapes have to survive `-Os` the way `it_probe`'s do. `r2`/`r3` are the declared
 * parameters under AAPCS, matching what `EntryState` sets up in the test rig. The trailing
 * `__builtin_unreachable()` is not paranoia: `bx lr` means control never reaches the end of the body,
 * and without that statement the compiler warns about a non-void function that "does not return", on a
 * function that returns on every path.
 *
 * Expected values, derived here so the reader can check them: with (a,b) = (3,7) the `LE` leg adds five
 * giving 8, and 8 < 10 takes the second block's `LT` leg to 1008. Adding seven gives 10, and `cmp 10,10`
 * is not `LT`, so the patched form takes the `GE` leg to 10|0x20 = 42 — the one-byte patch also flips
 * the *other* IT block, which is what makes it exercise two invalidated blocks rather than one. With
 * (7,3) the first block takes the `GT` leg, which the patch does not touch, so both answer 42: the
 * patch is local to the byte written. Immediates above 7 need the 32-bit form (`#9` does, and changed
 * four bytes instead of one), which is why this pair is 5 and 7 rather than something larger.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_patch_probe(JNIEnv* env, jclass clazz,
                                                                         jint a, jint b) {
  (void)env; (void)clazz; (void)a; (void)b;
  __asm__ volatile(
      "movs   r0, #0\n"
      "cmp    r2, r3\n"
      "ite    gt\n"
      "addgt  r0, r2, #3\n"
      "addle  r0, r2, #5\n"
      "cmp    r0, #10\n"
      "ite    lt\n"
      "addlt  r0, r0, #1000\n"
      "orrge  r0, r0, #0x20\n"
      "bx     lr\n" ::: "r0", "cc");
  __builtin_unreachable();
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_patch_probe_b(JNIEnv* env,
                                                                           jclass clazz, jint a,
                                                                           jint b) {
  (void)env; (void)clazz; (void)a; (void)b;
  __asm__ volatile(
      "movs   r0, #0\n"
      "cmp    r2, r3\n"
      "ite    gt\n"
      "addgt  r0, r2, #3\n"
      "addle  r0, r2, #7\n"
      "cmp    r0, #10\n"
      "ite    lt\n"
      "addlt  r0, r0, #1000\n"
      "orrge  r0, r0, #0x20\n"
      "bx     lr\n" ::: "r0", "cc");
  __builtin_unreachable();
}

/*
 * A syscall, for the channel between an executor and the syscall monitor (C07).
 *
 * Two things are deliberately *not* the same number here. `r7` holds 248, which is `__NR_exit_group` on
 * ARM EABI (`arm-linux-androideabi/asm/unistd-eabi.h:214`, read from the NDK rather than recalled), and
 * the encoding's immediate is 0x9c. Bionic puts the number in `r7` and leaves the immediate at zero, so a
 * probe that conflated the two would look right in a test that used `svc 0` — which is every test anyone
 * writes until one of them is wrong. As it stands, a tier or a monitor that reports `r7` as the
 * immediate, or the immediate as the number, is caught by the pair.
 *
 * The `add` after the SVC is the part that cannot be faked: the run only returns 42 for input 41 if
 * execution resumed at the instruction after the SVC and executed it exactly once. Two halfwords early
 * and the guest runs the tail of the `movw` as code; two late and r0 comes back 43.
 *
 * No hardware oracle value for this one, and that is honest rather than an omission: on real silicon the
 * same bytes ask the *kernel* for `exit_group`, which ends the oracle process instead of measuring it.
 * The claim being tested is that RetroBug's two tiers hand the monitor the same frame, so it is checked
 * between them, and the resume arithmetic is checked by `r0`.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_svc_probe(JNIEnv* env, jclass clazz,
                                                                       jint a) {
  (void)env; (void)clazz; (void)a;
  __asm__ volatile(
      "movw   r7, #0xf8\n"
      "svc    #0x9c\n"
      "adds   r0, r2, #1\n"
      "bx     lr\n" ::: "r0", "r7", "cc");
  __builtin_unreachable();
}

/*
 * Two more probes that a *monitor* answers, for the executor→C07 loop.
 *
 * `syscall_add` asks for getpid and returns one more than whatever came back, so the value the host
 * monitor supplied is visible in the guest's own arithmetic: 4242 in, 4243 out, or nothing matches. It
 * also proves the guest continued after the SVC a second time, from a different call site than
 * `svc_probe`.
 *
 * `flush_probe` is AD-7 told the honest way. The guest flushes `[addr, addr+16)` with
 * `__ARM_NR_cacheflush` (0xf0002 — the number `docs/20` §C07 names, and the one the measured 32-bit
 * table gives) and then returns `b+1`. The test that uses it patches a byte of guest code from the host
 * first, because a guest cannot write to its own executing page — that refusal is the design — so this
 * is the guest's half of self-modifying code, reached through its own syscall rather than through a host
 * call that happens to invalidate a cache.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_syscall_add(JNIEnv* env, jclass clazz,
                                                                         jint a) {
  (void)env; (void)clazz; (void)a;
  __asm__ volatile(
      "mov    r0, r2\n"
      "mov    r7, #20\n"  // __NR_getpid on ARM EABI, measured
      "svc    #0\n"
      "adds   r0, r0, #1\n"
      "bx     lr\n" ::: "r0", "r7", "cc");
  __builtin_unreachable();
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_flush_probe(JNIEnv* env, jclass clazz,
                                                                         jint addr, jint b) {
  (void)env; (void)clazz; (void)addr; (void)b;
  __asm__ volatile(
      "mov    r0, r2\n"
      "mov    r1, r2\n"
      "adds   r1, #16\n"
      "movw   r7, #0x0002\n"
      "movt   r7, #0x000f\n"  // __ARM_NR_cacheflush = 0xf0002. `movt` writes the top *halfword*, so
                              // #0xf000 here yields 0xf0000002, which the kernel's r7 masking (and
                              // `Frame::Capture`'s copy of it) folds down to 2 — a wrong number that
                              // reaches the monitor looking plausible. Found by both tiers agreeing.
      "svc    #0\n"
      "movs   r0, #0\n"
      "adds   r0, r3, #1\n"
      "bx     lr\n" ::: "r0", "r1", "r7", "cc");
  __builtin_unreachable();
}
