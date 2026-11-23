// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_stub_capture: where every export of a generated stub (M3 Task 5) lands. A stub export is
// `adr x9, RbStubTable; mov w10, #index; ldr x16, <GOT slot of rb_stub_capture>; br x16` — x9, x10 and x16 are
// AAPCS64 scratch registers, so the arguments ART (or the dynamic linker, for JNI_OnLoad) passed in x0-x7, d0-d7 and
// on the stack are untouched. This saves them into a CapturedArgs (x[8] +0, d[8] +64, stack +128, as
// rb::runtime::CapturedArgs) and calls `rb_stub_dispatch(table, index, &captured)`; the result comes back in x0 and
// is copied to d0 for a jfloat/jdouble return. Exported (default visibility): the stubs bind to it by name.

  .text
  .p2align 2
  .globl rb_stub_capture
  .type rb_stub_capture, %function
rb_stub_capture:
  stp   x29, x30, [sp, #-16]!
  mov   x29, sp
  sub   sp, sp, #144
  stp   x0, x1, [sp, #0]
  stp   x2, x3, [sp, #16]
  stp   x4, x5, [sp, #32]
  stp   x6, x7, [sp, #48]
  stp   d0, d1, [sp, #64]
  stp   d2, d3, [sp, #80]
  stp   d4, d5, [sp, #96]
  stp   d6, d7, [sp, #112]
  add   x11, x29, #16          // the caller's stacked arguments start above our saved fp/lr
  str   x11, [sp, #128]
  mov   x0, x9
  mov   w1, w10
  mov   x2, sp
  bl    rb_stub_dispatch
  fmov  d0, x0
  mov   sp, x29
  ldp   x29, x30, [sp], #16
  ret
  .size rb_stub_capture, . - rb_stub_capture
