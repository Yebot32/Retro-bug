/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The guest probe: a real `armeabi-v7a` JNI library, packaged into a real APK, loaded by RetroBug in
 * `core/tests/guest_apk_test.cpp`.
 *
 * Deliberately NOT self-contained. It calls `sin`, `cos` and `sqrt` out of whatever libm the guest
 * links — so loading it exercises PLT relocation and cross-library symbol resolution, which is the part
 * of M1 a self-contained probe would skip. The method is also deliberately non-constant: an earlier
 * attempt at this project's library work used `x = 2.0`, LLVM folded the whole expression, and the
 * "we linked libm" claim it produced was hollow. `seed` arrives at run time and the result is compared
 * in raw bits.
 *
 * `sqrt` is included for a second reason: in bionic it is not a libm routine at all but four
 * instructions of compiler-rt (`vmov`, `vsqrt.f64`, `vmov`, `bx lr` — 16 bytes, measured). So a
 * wrong answer here separates a broken VFP `vsqrt` from a broken libm, which a `sin`-only probe could
 * not do.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <jni.h>
#include <math.h>

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
}

JNIEXPORT jdouble JNICALL
Java_com_retrobug_guestprobe_Probe_mathProbe(JNIEnv* env, jclass clazz, jint seed) {
  (void)env;
  (void)clazz;
  const double x = (double)seed;
  return sin(x) * cos(x) + sqrt(x * x + 1.0);
}

/* --------------------------------------------------------------------------- argument placement
 *
 * The five methods below exist to make one question observable per calling-convention decision, and
 * each one is decided by a *different* rule. `mathProbe` above takes a `jint` and returns a `jdouble`,
 * so it tests the return path only; until these existed, RetroBug had never placed a floating-point
 * *argument* across the host/guest boundary, which is why `docs/30` §Step 24's wrong-VFP-bank bug was
 * able to sit in `PlaceArgs` unnoticed for as long as it did.
 *
 * `docs/30` §Step 24 recorded a general lesson about probes of this kind: a self-contained method whose
 * result the compiler can fold tests nothing, because the load that would have read the argument never
 * happens. Each method here consumes *every* parameter in a way that changes the answer, and each is
 * checked below by disassembling the result rather than by trusting the ABI document.
 */

/* jfloat after the two hidden JNI parameters: one 4-byte core register, r2. */
JNIEXPORT jfloat JNICALL
Java_com_retrobug_guestprobe_Probe_floatProbe(JNIEnv* env, jclass clazz, jfloat x) {
  (void)env;
  (void)clazz;
  return x * 1.5f + 1.0f;
}

/* jdouble: an 8-byte argument on an even slot, so r2:r3 — the pair rule that `jlong` already exercises. */
JNIEXPORT jdouble JNICALL
Java_com_retrobug_guestprobe_Probe_doubleProbe(JNIEnv* env, jclass clazz, jdouble x) {
  (void)env;
  (void)clazz;
  return x * 1.5 + 1.0;
}

/**
 * jint then jdouble, so the double arrives when the next free core register is r3.
 *
 * This is the case with no integer analogue: `jlong` after one `jint` can still find r2:r3, so the
 * existing `jlong` test never had to *skip* a register and drop to the stack. Here there is no pair
 * left, so the double goes to the stack argument block while r3 is consumed by nothing. A bridge that
 * packs the two halves into r3 and the next slot — or that spills at the wrong offset — is wrong only
 * on this row.
 */
JNIEXPORT jdouble JNICALL
Java_com_retrobug_guestprobe_Probe_intThenDouble(JNIEnv* env, jclass clazz, jint a, jdouble b) {
  (void)env;
  (void)clazz;
  return b + (double)a;
}

/* jdouble then jdouble: the first takes r2:r3, the second is the first stack argument. */
JNIEXPORT jdouble JNICALL
Java_com_retrobug_guestprobe_Probe_twoDoubles(JNIEnv* env, jclass clazz, jdouble a, jdouble b) {
  (void)env;
  (void)clazz;
  return a * b + 1.0;
}

/**
 * jdouble, jint, jdouble — the argument-block alignment pad.
 *
 * `a` takes the last core pair, so `b` and `c` are both stack arguments, and the block is 4 bytes deep
 * when `c` arrives. An 8-byte argument in the block keeps its own 8-byte alignment *relative to the
 * block*, so `c` lands at offset 8 and offset 4 is padding — which a bridge that packs the block
 * tightly gets wrong in a way that is invisible on every other row here, because every other row starts
 * the block at an already-aligned offset.
 */
JNIEXPORT jdouble JNICALL
Java_com_retrobug_guestprobe_Probe_doubleIntDouble(JNIEnv* env, jclass clazz, jdouble a, jint b,
                                                   jdouble c) {
  (void)env;
  (void)clazz;
  return a * c + (double)b;
}

/**
 * A comparator for `qsort`, reached *from inside emulated libc*.
 *
 * Deliberately not a JNI-shaped method: `qsort`'s fourth argument is a plain `int (*)(const void*,
 * const void*)`, so RetroBug has to hand a real guest function address to a real guest library and then
 * let that library call back into guest code. That is the same indirection a `jvalue` callback uses, with
 * neither side being RetroBug's own, and the bit-0 ISA tag on the address is what makes the callback
 * decode at all.
 *
 * Written as `(x > y) - (x < y)` rather than `x - y` because the subtraction overflows for
 * `INT_MIN`/`INT_MAX`, and the oracle sorts exactly that pair. An overflowing comparator makes `qsort`'s
 * result depend on undefined behaviour, which is the one thing a differential test cannot tolerate.
 */
JNIEXPORT int cmpInt(const void* a, const void* b) {
  const int x = *(const int*)a;
  const int y = *(const int*)b;
  return (x > y) - (x < y);
}
