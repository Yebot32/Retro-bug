/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * A weak reference nothing defines, the shape Angry Birds Go's statically linked `__cxa_guard_acquire`
 * has: it loads `__google_potentially_blocking_region_begin` and calls it only when the address is not
 * null. bionic binds an unresolved weak reference to 0, so the test is false and the call never happens.
 *
 *  - `g_weak_ptr` is a data word holding the address (`R_ARM_ABS32` against the weak symbol): a loader
 *    row reads it straight out of guest memory.
 *  - `weak_probe` is the guarded call itself, for anything that wants to run it.
 */

extern void rb_weak_nobody_defines(void) __attribute__((weak));

void (*volatile g_weak_ptr)(void) = rb_weak_nobody_defines;

int weak_probe(void) {
  if (rb_weak_nobody_defines) {
    rb_weak_nobody_defines();
    return 1;
  }
  return 2;
}
