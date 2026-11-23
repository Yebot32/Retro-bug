// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Inbound trampolines (M3): host entry points ART calls as JNI natives, each standing for one guest function.
//
// rb_inbound_pool is kPoolSize (4096) entries of 8 bytes: `mov w9, #index; b rb_inbound_capture`. Entry k is what
// `Inbound::BindPool` hands to RegisterNatives for the k-th binding. rb_inbound_capture saves the AAPCS64 argument
// registers and the caller's stack-argument pointer into a CapturedArgs on its own frame —
//     x[8] at +0, d[8] at +64, stack at +128 (136 bytes, 144 reserved)
// — and calls `rb_inbound_entry(index, &captured)` (C++). The result comes back as raw bits in x0; it is also copied
// to d0 so a jfloat/jdouble return is where the caller reads it (s0 is d0's low half). x9 is IP1-class scratch,
// free at a call boundary, so the trampoline touches nothing the caller's arguments live in.
//
// The symbol spelling is the platform's: Mach-O prefixes every C symbol with `_` and has no `.type`/`.size`,
// ELF does not. The same source serves the Android device and the iOS build (S1), which is why the macros.

#if defined(__APPLE__)
  .section __TEXT,__text,regular,pure_instructions
  #define RB_SYM(x) _##x
  #define RB_GLOBAL(x) .globl _##x
  #define RB_PRIVATE(x) .private_extern _##x
#else
  .text
  #define RB_SYM(x) x
  #define RB_GLOBAL(x) .globl x
  #define RB_PRIVATE(x) .hidden x
#endif
  .p2align 2

  RB_GLOBAL(rb_inbound_capture)
  RB_PRIVATE(rb_inbound_capture)
#if !defined(__APPLE__)
  .type rb_inbound_capture, %function
#endif
RB_SYM(rb_inbound_capture):
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
  add   x10, x29, #16          // the caller's stacked arguments start above our saved fp/lr
  str   x10, [sp, #128]
  mov   w0, w9
  mov   x1, sp
  bl    RB_SYM(rb_inbound_entry)
  fmov  d0, x0
  mov   sp, x29
  ldp   x29, x30, [sp], #16
  ret
#if !defined(__APPLE__)
  .size rb_inbound_capture, . - rb_inbound_capture
#endif

  RB_GLOBAL(rb_inbound_pool)
  RB_PRIVATE(rb_inbound_pool)
#if !defined(__APPLE__)
  .type rb_inbound_pool, %function
#endif
  .p2align 3
RB_SYM(rb_inbound_pool):
  .set rb_entry_index, 0
  .rept 4096
  mov   w9, #rb_entry_index
  b     RB_SYM(rb_inbound_capture)
  .set rb_entry_index, rb_entry_index + 1
  .endr
#if !defined(__APPLE__)
  .size rb_inbound_pool, . - rb_inbound_pool
#endif
