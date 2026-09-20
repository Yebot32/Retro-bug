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

int main(void) {
  /* The JNI convention is not followed here on purpose: this is the *device's* view of the same
   * functions, called directly, so that what is compared is the arithmetic and not the marshalling. */
  printf("JNI_OnLoad=%d\n", JNI_OnLoad(NULL, NULL));
  const int seeds[] = {0, 1, 4, 7, 100, -3};
  for (unsigned i = 0; i < sizeof seeds / sizeof seeds[0]; ++i) {
    const double v = Java_com_retrobug_guestprobe_Probe_mathProbe(NULL, NULL, seeds[i]);
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    printf("seed=%d value=%.17g bits=%016llx\n", seeds[i], v, (unsigned long long)bits);
  }
  return 0;
}
