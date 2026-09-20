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
#include <stdint.h>
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

/*
 * ---------------------------------------------------------------------------
 * The VFP/NEON probes: where Tier I's boundary actually is, in units of real code.
 *
 * `docs/32` §1 measured the demand — 14,723 vector/FP instructions across bionic and libc++, including
 * `vmul.f64` 786 times and `vldr` 2,290 — and §5 item 3 ranked FP-status vectors as the next lane after
 * IT-blocks. It did not say what RetroBug *does* about them, and the answer is not obvious from the
 * handler list: Tier I implements one vector opcode (`VectorAdd32`) out of the ~70 the A32 front end can
 * emit, so a double-precision `dadd` reaches `Fail(inst, "no Tier I semantics yet")`. That is a clean
 * stop rather than a wrong answer, which is the good news; the bad news is that Tier I is the path an
 * `execmem`-denied host has to run everything on, so "it stops" is a M1-sized gap that needs to be
 * measured rather than assumed.
 *
 * Hence these. Each one is a *distinct IR group*, not a distinct arithmetic result:
 *
 *   fp_arith   — scalar d-register arithmetic: `dmul`/`dadd`/`ddiv` and a `vsqrt`, plus the
 *                `fmdrr`/`ftosi` conversion out. This is the `vmul.f64` group `docs/32` §2 item 2 calls
 *                required rather than optional.
 *   fp_round   — single/double mixing: `fcvt` both ways and a truncating convert to int. Rounds and
 *                conversions are where a 32-bit emulation of `fmod`-style library code diverges.
 *   fpsr_flags — `dcmp.f64` writing the NV/ZF/C field, then `vmrs` reading FPSCR back out. This is the
 *                group Dynarmic's own README warns about ("FPSR state is approximate"), which is exactly
 *                why it needs a hardware number in front of it rather than a reading of the code.
 *   fp_stack   — `vldr`/`vstr` through a real stack array, and a call with enough live doubles to want
 *                the callee-saved `d8-d15` bank: the `vpush`/`vpop` prologue every compiled FP function
 *                has, and the one whose absence would show up as corrupt arguments rather than a crash.
 *   fp_nan     — `0.0/0.0`, then four comparisons against the result. NaN *semantics* rather than NaN
 *                bit patterns: on a correct engine exactly one of the four is true, and a tier that
 *                invents a payload still gets this right, which is the point — the payload is not what
 *                app code branches on.
 *   simd_int   — ASIMD integer lanes: `vmul.i32`, `vadd.i32`, and two lane extracts. The
 *                `vst1`/`vld1` structure group's nearest cheap relative.
 *
 * All six return `jint`, so the existing bridge drives them unchanged, and none of them calls into a
 * library: every instruction that runs is in this file's own text, which is what makes the hardware
 * value and the emulator value the same question about the same bytes.
 */
#include <arm_neon.h>

__attribute__((noinline)) static double use_one(double v) { return v * 1.0000001 + 0.5; }

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_arith(JNIEnv* env, jclass clazz,
                                                                      jint a, jint b) {
  (void)env; (void)clazz;
  const double x = a, y = b;
  const double z = x * y + (x / y) - __builtin_sqrt(x * x + y * y);
  return (int)z;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_round(JNIEnv* env, jclass clazz,
                                                                      jint a) {
  (void)env; (void)clazz;
  const double big = a * 2.5;
  const float small = (float)big * 3.0f;
  return (int)(double)small - a;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fpsr_flags(JNIEnv* env, jclass clazz,
                                                                        jint a, jint b) {
  (void)env; (void)clazz;
  const double x = a, y = b;
  /*
   * The divides are not padding. Given `(double)a < (double)b`, clang proves it equivalent to `a < b`
   * and emits `cmp` — which was what the first version of this probe did, leaving `vmrs` reading an
   * FPSCR that no FP instruction in this function had ever written. The comparison then measures
   * whatever the process started with: a different number on a device and in an emulator for a reason
   * that has nothing to do with either. Dividing by 3 and by 7 makes the ordering a real FP question.
   */
  const double t = x / 3.0 + y;
  const double u = y / 7.0;
  int r = 0;
  if (t < u) r |= 1;
  if (t > u) r |= 2;
  if (t == u) r |= 4;
  uint32_t f = 0;
  __asm__ volatile("vmrs %0, fpscr" : "=r"(f));
  /* Bits 31:28 are the compare result (NV, Z, C, GE); bit 16 is DXC and 12:8 the cumulative set. */
  //
  // A low-byte term used to live here, and it was measuring the wrong thing: an app process arrives
  // with FPSCR's sticky bits already raised by whatever ART did before, so `(f & 0xff)` reported that
  // history rather than anything this function did — five identical 0x11s, including for operations
  // that cannot raise an exception. `fp_cmp` below is the probe that measures the sticky bits
  // properly, by clearing FPSCR first. What stays here is the compare field, which `vcmpe` rewrites
  // and so cannot inherit.
  return r + (int)((f >> 27) & 0x1fu) * 8 + (int)((f >> 16) & 1u) * 256 +
         (int)((f >> 8) & 0x1fu) * 512;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_stack(JNIEnv* env, jclass clazz,
                                                                      jint a, jint b) {
  (void)env; (void)clazz;
  const double p = a, q = b;
  volatile double s[8];
  for (int i = 0; i < 8; ++i) s[i] = p * (i + 1) + q;
  /*
   * Seven doubles have to survive the call below, and only d0-d3 are caller-saved, so the prologue is
   * forced into `vpush {d8-…}`. The first version of this probe kept four live and clang solved it
   * inside d0-d3: the instruction group this is supposed to cover never appeared, and a tier that
   * cannot save a callee-saved register would have passed anyway.
   */
  double x0 = p, x1 = q, x2 = p + q, x3 = p - q, x4 = p * q, x5 = p / q, x6 = x0 * x1 + x2;
  for (int i = 0; i < 8; ++i) {
    x0 += use_one(s[i]);
    x1 += x0; x2 += x1; x3 += x2; x4 += x3; x5 += x4; x6 += x5;
  }
  return (int)(x0 + x1 + x2 + x3 + x4 + x5 + x6);
}

/*
 * Eight `double` arguments, which is the softfp ABI's boundary: the first four go in d0-d3 and the rest
 * come off the stack. Worth its own probe because the guest→host and host→guest paths will both have to
 * marshal arguments this shape once JNI starts passing `double`s (`CallStaticDoubleMethod` and its
 * friends), and a tier that reads the fifth argument from the wrong place gets a plausible-looking
 * garbage double rather than a fault.
 */
__attribute__((noinline)) static double sum8(double a, double b, double c, double d, double e,
                                             double f, double g, double h) {
  return a + b + c + d + e + f + g + h;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_args(JNIEnv* env, jclass clazz,
                                                                     jint a, jint b) {
  (void)env; (void)clazz;
  const double p = a, q = b;
  const double t = sum8(p, q, p + 1, q + 2, p * 3, q * 4, p - q, p / q);
  return (int)t;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_nan(JNIEnv* env, jclass clazz,
                                                                    jint a) {
  (void)env; (void)clazz;
  const double z = (double)a * 0.0;
  const double m = (z / z) * 1.5;
  int r = 0;
  if (m < 0.0) r |= 1;
  if (m > 0.0) r |= 2;
  if (m == 0.0) r |= 4;
  if (m != m) r |= 8;
  return r;
}

JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_simd_int(JNIEnv* env, jclass clazz,
                                                                      jint a, jint b) {
  (void)env; (void)clazz;
  /*
   * Two declared parameters, not four, and that is an ABI fact rather than a style choice: the ARM
   * AAPCS passes arguments in r0-r3 only — r4 upwards are workspace, and a fifth argument arrives on
   * the stack. A `JNIEnv*` and a `jclass` already eat r0 and r1, so a JNI method has exactly two
   * register parameters. The first draft of this probe took four and clang emitted
   * `ldr r0, [sp, #0x4]` as its first instruction, which a test that enters a function directly with
   * no caller frame reads as an unmapped read. `fp_args` above covers the stack-argument case, because
   * there the *guest* is the caller and lays its own frame out.
   */
  int32x4_t v = {a, b, a + b, a - b};
  const int32x4_t w = {a - b, a + b, b, a};
  v = vaddq_s32(vmulq_s32(v, w), w);
  return vgetq_lane_s32(v, 0) + vgetq_lane_s32(v, 3);
}

/*
 * Forces `vec` to stay in a vector register.
 *
 * At -Os clang will scalarise a whole int8x16 chain it can prove is only read back lane by lane, and
 * then this file's "NEON probe" measures the compiler's integer add rather than the emulator's `vadd`
 * — which is the `fpsr_flags` mistake (docs/30 §Step 16) in a new costume. An empty asm with a `+w`
 * tied operand is opaque to the optimiser, so the value has to be in a `d`/`q` register at that point.
 * The proof that it worked is the disassembly of the built library, not the pass rate:
 *
 *   $TC/bin/clang.exe --target=armv7-none-linux-androideabi26 ... -o /tmp/f.so tests/corpus/fixture_jni.c
 *   $TC/bin/llvm-objdump.exe -d --no-show-raw-insn /tmp/f.so | sed -n '/<_simd_lane>:/,/<_simd_bits>:/p'
 *
 * which is how the `vdupq_lane_u64` below was found to have been lowered into two `vmov.32`s rather
 * than the `VDUP` it was written as — a probe that silently stops probing is the failure this guards.
 */
#define RB_HOLD(vec) __asm__ volatile("" : "+w"(vec))

/// The same, for a value that is not a writable lvalue: an opaque *use* of it forces it into a vector
/// register at this point without asking to modify it.
#define RB_SEE(vec) __asm__ volatile("" : : "w"(vec))

/**
 * Integer ASIMD arithmetic and lane indexing, one lane width per `mode`.
 *
 * The returned value is a fold over all 16 bytes of the result register rather than one lane, because
 * a probe that reads back a single lane only proves that lane's arithmetic: an implementation of
 * `vadd.8` that walked the register in 16-bit steps would very likely leave lane 3 still correct. The
 * fold makes the *shape* of the operation part of the measured value — a lane loop with the wrong
 * width carries into its neighbour and the fold moves.
 *
 * Operands are deliberately different at each step (`v`, `w`, `u`), so an add is not undone by the
 * following sub and two wrong handlers cannot cancel into a green row.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_simd_lane(JNIEnv* env, jclass clazz,
                                                                       jint mode, jint a) {
  (void)env; (void)clazz;
  uint8_t bytes[16], seed[16], third[16];
  for (int i = 0; i < 16; ++i) {
    bytes[i] = (uint8_t)(a * 3 + i * 61 + mode * 17);
    seed[i] = (uint8_t)(mode * 5 + i * 29 + 7);
    third[i] = (uint8_t)(a - i * 13 + mode);
  }
  uint8x16_t v = vld1q_u8(bytes);
  uint8x16_t w = vld1q_u8(seed);
  uint8x16_t u = vld1q_u8(third);
  RB_HOLD(v); RB_HOLD(w); RB_HOLD(u);

  uint8x16_t x = v;
  switch (mode) {
    case 0: /* 8-bit lanes: the width a lane loop most easily gets wrong */
      x = vaddq_u8(v, w);
      x = vsubq_u8(x, u);
      x = vreinterpretq_u8_s8(vmulq_s8(vreinterpretq_s8_u8(x), vreinterpretq_s8_u8(w)));
      x = vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(x)));
      x = vsetq_lane_u8((uint8_t)(vgetq_lane_u8(x, 3) + 5u), x, 11);
      x = vorrq_u8(x, vdupq_n_u8((uint8_t)a));
      break;
    case 1: /* 16-bit lanes */
      x = vaddq_u16(vreinterpretq_u16_u8(v), vreinterpretq_u16_u8(w));
      x = vsubq_u16(vreinterpretq_u16_u8(x), vreinterpretq_u16_u8(u));
      x = vmulq_s16(vreinterpretq_s16_u16(x), vreinterpretq_s16_u16(w));
      x = vabsq_s16(x);
      x = vsetq_lane_u16((uint16_t)(vgetq_lane_u16(x, 1) + 9u), x, 6);
      /*
       * `0xfff0 + a` rather than `a`, which the first capture exposed: masking a lane with a 3-bit
       * constant zeroes most of the register, and the fold came back as 29791 — a row that measures
       * mostly nothing and would let a great many lane-loop mistakes through.
       */
      x = vandq_u16(x, vdupq_n_u16((uint16_t)(0xfff0u + (uint16_t)a)));
      break;
    case 2: /* 32-bit lanes, plus a broadcast and a rotate through the logical group */
      x = vaddq_u32(vreinterpretq_u32_u8(v), vreinterpretq_u32_u8(w));
      x = vsubq_u32(vreinterpretq_u32_u8(x), vreinterpretq_u32_u8(u));
      x = vmulq_s32(vreinterpretq_s32_u32(x), vreinterpretq_s32_u32(w));
      x = vabsq_s32(x);
      x = vsetq_lane_u32(vgetq_lane_u32(x, 2) ^ 0x5a5a5a5au, x, 0);
      x = veorq_u32(x, vdupq_n_u32((uint32_t)a));
      break;
    default: /* 64-bit lanes: no `vmul.I64`/`vabs.I64` in ARMv7, so add/sub/index only */
      x = vaddq_u64(vreinterpretq_u64_u8(v), vreinterpretq_u64_u8(w));
      x = vsubq_u64(vreinterpretq_u64_u8(x), vreinterpretq_u64_u8(u));
      x = vsetq_lane_u64(vgetq_lane_u64(x, 0) + vgetq_lane_u64(x, 1), x, 1);
      /*
       * Meant to be `VDUP q, D[m]`, and the disassembly says otherwise: clang lowered it to two
       * `vmov.32 d[0]`/`d[1]`. It had to — assembling `vdup.64 q6, d7[0]` is rejected while
       * `vdup.8/.16/.32` with the same bracketed-lane spelling are accepted, so ARMv7 has no
       * lane-indexed broadcast of a 64-bit element into a Q register at all. This line stays because
       * those two `vmov`s are what a 64-bit get-and-set looks like out of a compiler.
       */
      x = vdupq_lane_u64(vget_high_u64(vreinterpretq_u64_u8(x)), 0);
      /*
       * `VMOV (immediate)` with the Q bit set is how `VectorBroadcast64` is reached: dynarmic lowers
       * exactly that case to `VectorBroadcast(64, imm64)` (asimd_one_reg_modified_immediate.cpp:24).
       * The encoding restricts the `.i64` immediate to 0 or all-ones, so `-1` it is, and all-ones is
       * the better probe anyway: fed through the `veor` below it complements the register, so a
       * broadcast that wrongly yielded 0 leaves the fold unchanged instead of matching it.
       */
      uint64x2_t dup;
      __asm__ ("vmov.i64 %0, #-1" : "=w"(dup));
      RB_HOLD(dup);
      x = vreinterpretq_u8_u64(veorq_u64(vreinterpretq_u64_u8(x), dup));
      break;
  }
  RB_HOLD(x);
  vst1q_u8(bytes, x);
  unsigned r = 0;
  for (int i = 0; i < 16; ++i) r = r * 31u + bytes[i];
  return (int)r;
}

/**
 * The width-independent ASIMD logical group, one operation per `mode`, at both register sizes.
 *
 * `vorr`/`vand`/`veor`/`vbic` are 128-bit operations with no lane structure, so the lane loop cannot
 * hide a width bug here — what these rows can catch is an operand-order mistake. `vbic` is the one
 * worth naming: it is `A & ~B`, and `~A & B` is the same *shape* of expression and would pass every
 * symmetric test above. Hence the pairs below, whose operands differ in more than one byte.
 *
 * `vdupq_n_*` with a runtime value is what reaches `VectorBroadcast`; the `_n_` forms with a constant
 * get folded into an immediate the translator never shows my handler.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_simd_bits(JNIEnv* env, jclass clazz,
                                                                       jint mode, jint a) {
  (void)env; (void)clazz;
  uint8_t bytes[16], seed[16];
  for (int i = 0; i < 16; ++i) {
    bytes[i] = (uint8_t)(a + i * 37 + mode);
    seed[i] = (uint8_t)(0xa5u ^ (i * 19u) ^ (unsigned)mode);
  }
  uint8x16_t v = vld1q_u8(bytes);
  uint8x16_t w = vld1q_u8(seed);
  uint8x16_t bcast = vdupq_n_u8((uint8_t)a);
  RB_HOLD(v); RB_HOLD(w); RB_HOLD(bcast);

  uint8x16_t x = v;
  switch (mode) {
    case 0: x = vorrq_u8(v, w); break;
    case 1: x = vandq_u8(v, w); break;
    case 2: x = veorq_u8(v, w); break;
    case 3: x = vbicq_u8(v, w); break;   /* A & ~B, and the order is the whole point */
    default: x = vbicq_u8(w, v); break;  /* the *other* operand masked: a distinct answer */
  }
  /* Fold the broadcast in rather than leaving it live-only-until-optimised-away. */
  x = veorq_u8(x, bcast);
  /*
   * `VORN`, because that is what reaches `VectorNot`: dynarmic lowers it to
   * `VectorOr(reg_n, VectorNot(reg_m))` (asimd_three_regs.cpp:334) — the *second* operand is the one
   * inverted, so an implementation that reads it the other way round still produces a bitwise
   * function of two operands and only a value comparison catches it. AArch32 has no `vmvnq`
   * intrinsic, so VORN is the reachable form of the same handler.
   */
  x = vornq_u8(x, w);
  /*
   * `VNEG`, which reaches `ZeroVector`: dynarmic lowers integer VNEG to
   * `VectorSub(esize, ZeroVector(), reg_m)` (asimd_two_regs_misc.cpp:452). Signed here so the
   * most-negative lane is included and the wrap is the documented one rather than a new behaviour.
   */
  x = vreinterpretq_u8_s8(vnegq_s8(vreinterpretq_s8_u8(x)));
  RB_HOLD(x);

  /*
   * The same two operations on the D halves, because the Q form alone cannot tell an implementation
   * that stops at 64 bits from one that walks the whole register. `vbic` again on the second leg, with
   * the operands swapped relative to the first, so the D path is checked for order as well as range.
   */
  const uint8x8_t lo = vget_low_u8(x);
  const uint8x8_t hi = vget_high_u8(x);
  RB_SEE(lo); RB_SEE(hi);
  const uint8x8_t m = vorr_u8(lo, hi);
  const uint8x8_t n = vbic_u8(hi, lo);
  uint8_t out[16];
  RB_SEE(m); RB_SEE(n);
  vst1_u8(out, m);
  vst1_u8(out + 8, n);

  unsigned r = 0;
  for (int i = 0; i < 16; ++i) r = r * 31u + out[i];
  for (int i = 0; i < 16; ++i) r = r * 31u + bytes[i];
  return (int)r;
}

/*
 * The FPSCR compare field, one relation at a time, straight out of the register.
 *
 * Why this exists rather than a table copied from the ARMv7 manual: Tier I implemented `VCMP` from the
 * documented NV/Z/C/GE matrix and disagreed with hardware on every row, while Tier J agreed. One of the
 * three is wrong, and the manual is not the arbiter — the silicon is. So this asks the four questions
 * directly (less, equal, greater, unordered), reads FPSCR back with `vmrs` immediately after the
 * `dcmp` so nothing can intervene, and returns both halves that matter:
 *
 *   bits [11:8]  FPSCR[31:28], i.e. NV : Z : C : GE
 *   bits  [7:0]  FPSCR[7:0], the sticky exception bits (IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7)
 *
 * The `vcmpe`/`vmrs` pair is inline asm on purpose: it is the only way to guarantee the read follows the
 * compare for *this* relation rather than for whichever compare the compiler decided to run last.
 */
JNIEXPORT jint JNICALL Java_com_retrobug_fixture_MainActivity_fp_cmp(JNIEnv* env, jclass clazz,
                                                                    jint mode) {
  (void)env; (void)clazz;
  double a = 0.0, b = 2.0;
  int quiet = 0;
  switch (mode) {
    case 0: a = 1.0; break;   /* less */
    case 1: a = 2.0; break;   /* equal */
    case 2: a = 3.0; break;   /* greater */
    case 3:               /* unordered, with the form that raises on a quiet NaN */
      __asm__ volatile("vmov.i32 %[n], #0\n\t"
                       "vdiv.f64 %[n], %[n], %[n]\n\t"
                       : [n] "+w"(a) :: );
      break;
    default:                  /* unordered, with the form that does not */
      quiet = 1;
      __asm__ volatile("vmov.i32 %[n], #0\n\t"
                       "vdiv.f64 %[n], %[n], %[n]\n\t"
                       : [n] "+w"(a) :: );
      break;
  }
  /*
   * FPSCR is cleared first, and that is not decoration. The sticky bits are process state: an app
   * process arrives with the exceptions whatever ART's earlier floating-point work left raised, and
   * this probe's first version reported 0x11 in every row including the ones whose operation cannot
   * raise anything. Clearing makes the low byte a measurement of *this* compare, which is the only
   * way the number means anything.
   */
  uint32_t zero = 0;
  __asm__ volatile("vmsr fpscr, %0" :: "r"(zero) : "cc");
  uint32_t f = 0;
  if (quiet) {
    __asm__ volatile("vcmp.f64 %[x], %[y]\n\t"
                     "vmrs   %0, fpscr\n\t"
                     : "=r"(f)
                     : [x] "w"(a), [y] "w"(b)
                     : "cc");
  } else {
    __asm__ volatile("vcmpe.f64 %[x], %[y]\n\t"
                     "vmrs   %0, fpscr\n\t"
                     : "=r"(f)
                     : [x] "w"(a), [y] "w"(b)
                     : "cc");
  }
  return (int)((((f >> 28) & 0xfu) << 8) | (f & 0xffu));
}
