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
