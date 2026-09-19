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
