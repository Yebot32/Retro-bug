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
