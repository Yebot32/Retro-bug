/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Ground truth for `mathprobe.c`, produced by running the *same* library natively on real ARMv7
 * hardware. `tools/build-guest-apk.sh` links this against the .so it just built and pushes both, so
 * the numbers it prints are what a real device computes for those bytes.
 *
 * Linked rather than `dlopen`ed, and that is not a style choice: Android does not resolve symbols for a
 * library opened by absolute path out of `/data/local/tmp`, so a `dlopen`-based oracle fails on `sin`
 * for reasons that have nothing to do with the library under test. Linking makes the dynamic linker do
 * it through the ordinary `DT_NEEDED` chain.
 *
 * Printed as raw bit patterns as well as decimals. A last-bit difference is invisible in `%f`, and the
 * comparison against these values is the whole point of the exercise.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int JNI_OnLoad(void* vm, void* reserved);
extern double Java_com_retrobug_guestprobe_Probe_mathProbe(void* env, void* clazz, int seed);
extern float Java_com_retrobug_guestprobe_Probe_floatProbe(void* env, void* clazz, float x);
extern double Java_com_retrobug_guestprobe_Probe_doubleProbe(void* env, void* clazz, double x);
extern double Java_com_retrobug_guestprobe_Probe_intThenDouble(void* env, void* clazz, int a, double b);
extern double Java_com_retrobug_guestprobe_Probe_twoDoubles(void* env, void* clazz, double a, double b);
extern double Java_com_retrobug_guestprobe_Probe_doubleIntDouble(void* env, void* clazz, double a, int b,
                                                                 double c);

static void show(const char* what, double v) {
  uint64_t bits = 0;
  memcpy(&bits, &v, sizeof bits);
  printf("%-46s value=%.17g bits=%016llx\n", what, v, (unsigned long long)bits);
}

static void showf(const char* what, float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof bits);
  printf("%-46s value=%.9g bits=%08llx\n", what, v, (unsigned long long)bits);
}

int main(void) {
  /* The JNI convention is not followed here on purpose: this is the *device's* view of the same
   * functions, called directly, so that what is compared is the arithmetic and not the marshalling. */
  printf("JNI_OnLoad=%d\n", JNI_OnLoad(NULL, NULL));
  const int seeds[] = {0, 1, 4, 7, 100, -3};
  for (unsigned i = 0; i < sizeof seeds / sizeof seeds[0]; ++i) {
    char label[64];
    snprintf(label, sizeof label, "mathProbe seed=%d", seeds[i]);
    show(label, Java_com_retrobug_guestprobe_Probe_mathProbe(NULL, NULL, seeds[i]));
  }

  /* The argument-placement rows. Every input below is written as a decimal literal rather than as a bit
   * pattern, in both this file and the lane that asserts against it, so that the two sides cannot drift:
   * `docs/30` §Step 24's diagnosis was briefly led astray by a hand-written hex literal for 10001.0 that
   * was actually 10000.25. The *outputs* are compared as bits, because that is where a last-bit
   * difference has to be visible.
   *
   * Each row is chosen so that a wrong placement cannot accidentally produce the right answer: the
   * doubles are not zero, and the two halves of each are different, so a bridge that passed the low half
   * twice, or swapped them, or left a register stale, moves the result. */
  showf("floatProbe f=1.75 (jfloat, one core register)",
        Java_com_retrobug_guestprobe_Probe_floatProbe(NULL, NULL, 1.75f));
  showf("floatProbe f=-0.125", Java_com_retrobug_guestprobe_Probe_floatProbe(NULL, NULL, -0.125f));
  show("doubleProbe x=10001.0 (jdouble, even/odd pair)",
       Java_com_retrobug_guestprobe_Probe_doubleProbe(NULL, NULL, 10001.0));
  show("doubleProbe x=0.1 (inexact: nothing folds)",
       Java_com_retrobug_guestprobe_Probe_doubleProbe(NULL, NULL, 0.1));
  show("intThenDouble a=7 b=10001.0 (pair skip -> stack)",
       Java_com_retrobug_guestprobe_Probe_intThenDouble(NULL, NULL, 7, 10001.0));
  show("intThenDouble a=-3 b=0.1", Java_com_retrobug_guestprobe_Probe_intThenDouble(NULL, NULL, -3, 0.1));
  show("twoDoubles a=10001.0 b=0.5 (second is [sp])",
       Java_com_retrobug_guestprobe_Probe_twoDoubles(NULL, NULL, 10001.0, 0.5));
  show("doubleIntDouble a=2.25 b=7 c=0.125 (align pad)",
       Java_com_retrobug_guestprobe_Probe_doubleIntDouble(NULL, NULL, 2.25, 7, 0.125));
  show("doubleIntDouble a=0.1 b=-1 c=65537.0",
       Java_com_retrobug_guestprobe_Probe_doubleIntDouble(NULL, NULL, 0.1, -1, 65537.0));
  return 0;
}
