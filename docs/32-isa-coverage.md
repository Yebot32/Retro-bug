# ARMv7 instruction demand vs engine coverage (M0 step 3, open risk U-8)

Measured 2026-09-19 with [`../tools/isa_coverage.sh`](../tools/isa_coverage.sh). Reproducible: the script
re-extracts and re-disassembles the corpus and prints the same summary. Raw histogram:
`build/isa-coverage/mnemonics.txt` (759 lines, not committed — regenerate).

**The totals in the first published version of this document were wrong** — 338,034 instructions and
855 mnemonics, from a tokenizer that mistook objdump's data bytes for opcodes and a separate
instruction count that included data lines. §3.3 has the details, and §3.4 records a claim that turned
out never to have been measured at all. The per-mnemonic numbers below, and every conclusion in §2, are
unchanged by the correction.

## 1. What was measured, and why it is the right corpus

The NDK's own AArch32 archives, extracted member by member: `libc.a` (549 objects), `libm.a`,
`libc++_shared.so`, `libz.a` — **711 objects, 325,541 instructions, 759 distinct mnemonics** of real
compiled ARMv7 machine code. This is the code every guest app links against, so its instruction mix is
the demand curve the engine has to satisfy before any app-level `.so` is even considered. It is analysis
input on this machine only, never redistributed.

| Metric | Value |
|---|---|
| Instructions disassembled | 325,541 |
| Distinct mnemonics | 759 |
| Thumb-2 32-bit wide (`.w`) | 47,894 (14.7%) |
| **IT-block instructions (`it`, `itt`, `ite`, …)** | **8,387** (2.6%) |
| Vector/FP mnemonics | 166 distinct / 14,723 instructions (4.5%) |

Top of the demand curve: `mov` 32,371 · `ldr` 32,183 · `cmp` 20,800 · `add` 18,542 · `str` 13,484 ·
`movs` 12,967 · `bl` 11,254 · `b` 10,967 · `add.w` 7,923 · `blx` 7,532 · `adds` 7,306 · `ldr.w` 5,977 ·
`beq` 5,687 · `bne` 5,248 · `it` 4,955 · `mov.w` 3,997 · `movw` 3,284 · `cbz` 3,271 · `itt` 2,609.
Vector/FP top: `vldr` 2,290 · `vmov` 1,580 · `vst1.64` 815 · `vstr` 788 · `vmul.f64` 786 ·
`vadd.f64` 744 · `vmrs` 725 · `vmla.f64` 651 · `vst1.32` 481 · `vcmp.f64` 470.

## 2. Four conclusions that change design assumptions

1. **IT-blocks are load-bearing in practice, not academic.** `docs/research/01` named IT-block TB
   splitting as the single biggest correctness risk, on the reasoning that it is subtle. The measurement
   adds the other half of the argument: bionic itself emits 8,387 IT-block instructions, so any engine
   defect there breaks system libraries before it breaks an app. This is now the highest-priority lane in
   the differential suite (§5), not a nice-to-have.
   **Confirmed, then fixed: `docs/30` §Step 7c.** A purpose-built IT probe in the fixture returned
   `it_probe(7,3) = 1012` on Tier I against `42` on the same compiled Thumb-2 on the SM-X710, and the
   pattern across all six shapes was "both legs of every `ite` ran, last write wins". Tier I had never
   consulted `Block::GetCondition()` / `ConditionFailedLocation()`, which is precisely how Dynarmic
   represents a split IT sequence. The prediction was right, the defect was in RetroBug's code and not
   in Dynarmic's, and the only thing that found it was a measured expectation — §4's warning that
   "presence of a handler and semantic correctness are different claims" is now an example rather than a
   caveat.
2. **VFPv3 double precision is required, not optional.** `vmul.f64`/`vadd.f64`/`vmla.f64`/`vcmp.f64`
   all appear hundreds of times — libm's `double` paths. An engine scoped to single-precision-only FP
   (a plausible mistake when the reference target was a 3DS ARM11) fails here.
3. **ARMv7-only encodings are unavoidable.** `movw` (3,284) and `cbz` (3,271) are ARMv7-era, and
   bitfield/leading-zero access is real: `clz` 516, `ubfx` 173, `sbfx` 9, `mrc` 132. This is the
   concrete form of the ARMv6-vs-ARMv7-A question the architecture had to answer before committing to
   Dynarmic (`docs/10` §4.1). **`sdiv`/`udiv` are not** — see §3.4; this corpus does not contain them,
   so divide support is an ARMv7-A capability RetroBug must have for *app* code rather than something
   bionic's own build demonstrates a need for.
4. **`vst1.64`/`vst1.32` register-list forms are common** (815/481), so the ASIMD load-store-structure
   group must be right including its alignment and register-count edge cases.

## 3. Corrections to how U-8 was argued before this measurement

Four things this document asserted that did not survive checking. Listed because the failure modes are
distinct, and three of the four are the kind that *looks* like a working measurement.

1. **`docs/10` §4.1's "file listing proves ARMv7-A coverage" was the wrong kind of evidence.** Seeing
   `divide.cpp`, `barrier.cpp`, `asimd_*.cpp` in a tree proves those groups are *dispatched*, not that
   they are complete or correct. This document replaces that inference with a demand measurement; the
   supply side is still only partly answered (§5).
2. **A "found gap" that was not a gap.** `frontend/A32/translate/impl/asimd_load_store_structures.cpp`
   returns `UndefinedInstruction()` at two sites, which looked like unimplemented NEON structure forms
   against 815 real uses of `vst1.64`. Reading the surrounding function shows those returns are
   `VST4/VLD4` with `size == 0b11` — **architecturally undefined in ARMv7**, correctly rejected, with
   adjacent `DecodeError`/`UnpredictableInstruction` paths also matching the ARM ARM. The engine
   implements the multi-register `v8_VST_multiple`/`v8_VLD_multiple` families. Recorded because the
   near-miss is the failure mode worth documenting: a plausible-looking gap found by grep is exactly
   what must be read before being reported.
3. **The measurement tool was wrong, in two directions at once.** The totals published here first
   (338,034 instructions, 855 mnemonics) came from a tokenizer anchored on "address, whitespace,
   lowercase" plus a *separate* looser instruction count. Where objdump printed a byte column and a
   mnemonic on the same line, the anchor matched the **bytes**, so 96 two-hex-digit tokens entered the
   histogram as opcodes carrying 3,481 instructions; and the separate count matched `.word` data lines,
   inflating the total. Corrected: **325,541 instructions, 759 mnemonics**. Every per-mnemonic count in
   §1 was unaffected — the bytes-prefixed lines are a different 3,481 instructions each time — which is
   why §2's conclusions stand while its headline numbers did not. Worth the detail because the failure
   was invisible in the output: an inflated total and 96 nonsense rows look like a corpus that merely
   has odd instructions in it.
   Found while building §5.2's tool, which reported "855 rows, 329,022 attributed, 98 unmatched" and
   forced the reconciliation. The first repair then dropped the address anchor and picked up objdump's
   `Disassembly of section .text:` headers instead, which surfaced as 2,686 instructions of a mnemonic
   called `of` — caught only because that number was queried rather than accepted. Both properties
   (anchor, then skip byte columns) are now in one `awk`, and `instructions` is the *sum of the
   histogram* rather than an independent count, so the two cannot drift apart again.
4. **A claim that was never measured.** "`SDIV`/`UDIV` are used too" appeared in §2.3 of the first
   version. Neither mnemonic occurs anywhere in this corpus, in the broken histogram or the corrected
   one — checked by direct grep of both. It was an inference from "this is ARMv7-A code, ARMv7-A has
   divide", and NDK clang targeting `armv7a` without `+div` evidently never emitted them. The
   capability still matters (app `.so` files built with `-mfpu=neon -march=armv7-a` do use `sdiv`, and
   Dynarmic implements it), but it is not something this measurement showed, and the sentence now says
   so in those terms. Recorded because the inference is *plausible*, which is exactly what makes it
   dangerous to leave in a document that is otherwise full of numbers.

## 4. What this does *not* establish

Presence of a mnemonic in the histogram and presence of a handler in the engine are different claims, and
**semantic correctness is a third**. Nothing here says Dynarmic computes `vmla.f64` to the last bit, that
IT-block state is preserved across a translation-block boundary, or that `vmrs` reflects the right FPSCR
flags — Dynarmic's own README already warns that FPSR state is approximate.

So U-8 moves from *unexamined* to **half-closed**: demand is quantified and the risky groups are
identified; correctness against a reference is not yet measured.

## 5. The remaining work, in priority order

1. **Differential semantic lanes — done on both back ends, and the arm64 half was the interesting one.**
   Dynarmic's own
   `dynarmic_tests` target builds with `DYNARMIC_TESTS_USE_UNICORN=OFF` (the default), so **no GPL code
   is configured, built, or linked** — the `docs/10` §13.3 clean-room posture stays intact while we still
   get a large existing ARMv7 test suite. Run via [`../tools/run_engine_tests.sh`](../tools/run_engine_tests.sh):
   **200,561 assertions, all passing** on the x86_64 emulator (48 cases) *and* on arm64 hardware (47 — the
   one-case difference is dynarmic's own x86-only, assertion-free `x64_cpu_info.cpp`, verified in its
   `CMakeLists.txt` rather than backed out of the arithmetic), including
   `A32/test_svc.cpp` (guest kernel entry) and `A32/test_coprocessor.cpp` (CP15 / TLS registers) — the two
   paths RetroBug leans on hardest and the ones a console-oriented front end might plausibly neglect.
   Making the arm64 build compile at all needed a `compat/` Boost-shim fix, and the gap was itself a
   finding: `apply_visitor` must unwrap `recursive_wrapper` the way Boost does (`docs/30` §Step 5f).
   What stays open: it asserts what Dynarmic chose to assert, so anything outside its test set — plus its
   own "FPSR state is approximate" admission — is uncovered, and **item 2 is what turns "green on its own
   tests" into "covers what real code demands"**.
2. **Mnemonic→handler mapping tool. Done** —
   [`../tools/mnemonic_handlers.sh`](../tools/mnemonic_handlers.sh), joining objdump's mnemonic to
   Dynarmic's decoder cases (759 `INST` entries across `frontend/A32/decoder/*.inc`) and then to the
   translator body that implements each one. On the corrected histogram: **757 of 759 rows are
   implemented, covering 325,532 of 325,541 instructions — 100.00%**; the 2 unmatched rows are 9
   instructions of objdump pseudo-tokens (`trap` 7, `rrx` 2), not encodings. So §4's presence half is
   answered: **the demand side is covered by the vendored engine**, and U-8's remaining content is
   semantic correctness plus the two §4 items, not missing instructions.

   Two qualifications the tool prints rather than hides, both of which matter more than the headline:

   - **`ldm`, `stm`, `ldm.w`, `stm.w`, `ldmge`, `stmge` (509 instructions) partly route to
     interpretation, not compilation. §7 shows this claim is false and supersedes it.** The matched decoder
     cases include `arm_LDM_usr`, `arm_STM_usr`,
     `arm_CPS`, `arm_RFE`, `arm_SRS`, whose bodies end in `InterpretThisInstruction()`. Dynarmic's
     `IR::Term::Interpret` is not an interpreter: the back ends emit a call to
     **`UserCallbacks::InterpreterFallback(pc, num_instructions)`** — the *embedder* is expected to
     execute those instructions (`interface/A32/config.h:104`). Dynarmic ships no such interpreter, so
     RetroBug's Tier I is not only the iOS/`execmem`-denied path: it is load-bearing on the Android JIT
     path too, for register-list and PSR-writing instructions that appear in bionic 509 times. Recorded
     against AD-2 (`docs/10` §5), which argued for Tier I's priority on other grounds.
   - Attribution is per **mnemonic**, not per encoding: bare `add` has ARM, Thumb-16 and Thumb-32
     decoder cases, and a rollup of "best bucket among its cases" is mildly optimistic. The tool says so
     in its own output, and per-encoding numbers would need objdump's encoding column, which
     `--no-show-raw-insn` currently suppresses.
3. **IT-block and FP-status vectors first.** Ranked by §2: IT-blocks, `f64` VFP paths, `vmrs`/FPSCR,
   ASIMD structures — these are where Dynarmic's own suite is thinnest relative to measured demand.
   **IT-blocks: done** (`30` §Step 7c found and fixed Tier I's bug; §Step 9 made the six shapes a
   differential with hardware values). **FP/ASIMD: done** (`30` §Step 16, `exec_tier_diff_test.cpp`'s
   `fp_groups_are_accurate_or_explicitly_refused`), and its result is asymmetric in a way that matters:

   | | Tier J (JIT) | Tier I (interpreter) |
   |---|---|---|
   | 35 rows × 7 VFP/ASIMD groups, values measured on the SM-X710 | **35 of 35 equal hardware** | **35 of 35 refuse**, at the first FP/ASIMD instruction |

   The groups covered on the JIT path are scalar `f64` `+ - * /` with `vsqrt`, `fcvt` in both
   directions and a truncating convert, `dcmp.f64` + `vmrs` of FPSCR, `vldr`/`vstr` plus the
   callee-saved `d8-d15` traffic, eight-double softfp argument passing, NaN comparison semantics, and
   ASIMD `vmul.i32`/`vadd.i32` with lane extracts. Two of those deserve emphasis because §2 ranked them
   and nothing had measured them since: **FPSCR flag semantics match silicon including ARM's rule that
   a less-than compare also sets NV** (the row is `34 34 34 129 34`, and `129` is the `t < u` case), and
   `vmul.f64`-class arithmetic — the group §2 item 2 called required rather than optional — is right to
   the returned integer on every pair. Dynarmic's "FPSR state is approximate" caveat was first read as
   *unexercised* on the grounds that the cumulative bits came back 0 — which was wrong twice over, and
   `docs/30` §Step 17 has the correction: the bits masked (`FPSCR[12:8]`) are the ARMv7 locations and
   read as zero on an ARMv8 core regardless of what happened, while the bits that carry the exceptions
   here are `[7:0]` (`common/fp/fpsr.h:88-138`), and an app process arrives with them already raised by
   ART's earlier work. Measured properly — FPSCR cleared, one operation, read back — a `vcmpe.f64`
   against a NaN raises IOC and its non-raising twin does not, and the inexact flag accumulates from the
   divides as it should. So the caveat is neither cleared nor confirmed: **the flags that do work are the
   host's**, which is a different concern from the one the README was warning about.

   Tier I's refusal was the finding with planning consequences, and `docs/30` §Step 17 has since
   changed its extent: the scalar VFP group is implemented, so Tier I now executes and matches hardware on
   35 of the 40 FP rows (the conversions, `dadd`/`dmul`/`ddiv`/`vsqrt`, `dcmp`+`vmrs` with the measured
   compare field, `vldr`/`vstr`, the callee-saved bank, eight-double softfp arguments, NaN compares) and
   refuses only ASIMD, at `VectorBroadcast64` with `StopReason::Unimplemented` — a named refusal, not a
   wrong number. What still stands from the paragraph above: the interpreted path cannot run NEON, which
   reaches AD-2, the `Kind::Interpreted` code-cache backend (C04's third option, i.e. the iOS/`execmem`-
   denied case where M3 has no other choice) and any T1 policy that trades `execmem` for isolation. And
   the FP work also settled §2 item 2's other half. The compare field both tiers produce — as
   FPSCR[31:28] NV:Z:C:GE, `0x8` less, `0x6` equal, `0x2` greater, `0x3` unordered — **is Arm's
   published VFP comparison table**, checked against the measurement *and* against Arm's own text
   (`docs/30` §Step 17). Note the shape, because it is not an integer-flags shape: unordered leaves NV
   **and** Z clear and sets C+GE, so `!=` after a floating-point compare is `VS`, taken from GE.
4. Repeat on the **app-level** corpus once `docs/research/10`'s APK fixtures are downloaded, since
   bionic will under-represent what games' hand-written assembly and codec libraries use.

## 6. Reproducing

```bash
bash tools/isa_coverage.sh [output-dir]     # default build/isa-coverage
# needs: ANDROID_HOME (or the default %LOCALAPPDATA%/Android/Sdk), an NDK with AArch32 sysroot;
# RB_NDK overrides 28.2.13676358. No Python.
```

## 7. The six A32 bodies that end in `InterpretThisInstruction()` — §5 item 2, resolved

Read out of the vendored tree and re-measured against the corpus. **§5 item 2 was wrong in both directions**:
the group it called load-bearing is not reached by the code it cited, and the failure it predicted ("nowhere
to go") is not a stop at all but a process abort on the shipping host — with a hang on the other one.
Everything is [1L] (read or ran here, `external/dynarmic` at `e77b1ba`, NDK 28.2) unless tagged otherwise.

**7.1 The enumeration — six, not five.** `InterpretThisInstruction()` is defined once
(`frontend/A32/translate/impl/a32_translate_impl.cpp:31-34`: `ir.SetTerm(IR::Term::Interpret(ir.current_location))`,
return false), and grep finds exactly six callers, all ARM-state:

| body | file:line | what it is | a real encoding, assembled here | userspace-reachable? |
|---|---|---|---|---|
| `arm_LDM_usr` | `impl/load_store.cpp:868` | banked-reg **load**, no writeback | `e8d20030` = `ldm r2, {r4,r5}^` | no — privileged modes only |
| `arm_LDM_eret` | `:872` | banked load **including PC** = exception return | `e9708010` = `ldmdb r0!, {r4,pc}^` | no — OS exception return |
| `arm_STM_usr` | `:953` | banked-reg **store** | `e9400030` = `stmdb r0, {r4,r5}^` | no — privileged modes only |
| `arm_CPS` | `impl/status_register_access.cpp:14` | change processor state | `f102001f` `cps #0x1f`, `f1080080` `cpsie i`, `f10c01c0` `cpsid aif` | mode change no; A/I/F-only at EL0 is UNPREDICTABLE-to-UNDEF — [UNVERIFIED, measure] |
| `arm_RFE` | `:106` | return from exception | `f8b00a00` = `rfeia r0!` | no |
| `arm_SRS` | `:117` | store return state | `f96d051f` = `srsdb sp!, #0x1f` | no |

Masks: `decoder/arm.inc:184,185,190,310,315,316`. Two facts decide severity. (i) The `_usr` masks wildcard the
condition field, so **predicated banked forms (`ldmeq`/`stmcs` with `^`) do decode here** — §5's worry is
right at the decoder level; `arm_CPS`'s mask instead *fixes* bits 31-28 to 1111, so predicated CPS is not this
instruction. (ii) Here bit 22 is the banked-register marker, bit 20 is L and `Rn` is 19-16, so a mask test is
unambiguous, and `decoder/arm.h:45-47` stable-sorts by mask popcount so the 7-bit plain forms beat the 6-bit
banked ones — verified on a P/U/W/L matrix where bit 22 is the only thing separating `stmdb r0, {…}` from
`stmdb r0, {…} ^`.

**Reachability, measured rather than asserted.** In the §1 corpus (711 objects, 325,541 instructions): **zero**
banked forms (objdump's trailing `^`; 0 hits, and the caret survives the script's `--no-show-raw-insn`,
checked against my own probe) and **zero** `cps`/`rfe`/`srs` mnemonics. The NDK assembler accepts every one of
them for `armv7a-none-linux-androideabi`, rejecting only what the ARM ARM forbids ("system STM cannot have
writeback register"), and `__attribute__((target("arm")))` makes them reachable as inline asm inside a
`-mthumb` TU — so a hand-written `.s` in an app's `.so` *can* ship one. What it cannot get is a defined
meaning: these are privileged-mode instructions, and the guest is EL0 userspace with no ARM32 kernel emulated
(AD-3 — the syscall ABI is translated, not run). QEMU agrees from the other side, raising an undefined
encoding for RFE in user mode and SRS at EL0
([translate.c](https://raw.githubusercontent.com/qemu/qemu/master/target/arm/tcg/translate.c), `gen_srs` and
the RFE path) [1A]. **Low trigger probability because the group is OS-only; not zero, because "the assembler
accepts it" is the whole story for hand-written assembly.**

**7.2 Correction to §5 item 2.** Three of its claims do not survive the reading above.

1. *"509 instructions of bionic … carry those paths"* — **false.** Of the 509 (`ldm.w` 212, `stm.w` 150, `ldm`
   87, `stm` 54, `ldmge` 3, `stmge` 3) the 362 `.w` rows are **Thumb-2** while all six bodies are ARM-state, and
   the other 147 decode to `arm_LDM`/`arm_STM` and compile normally. The 509 is an artifact of the tool's join
   key — "objdump mnemonic → first word of the `INST` mnemonic string" (`build/isa-coverage/handlers.txt:8-9`)
   — against dynarmic's names `"LDM (usr reg)"`/`"STM (usr reg)"`. The tool printed its rollup honestly
   (`handlers.txt:51`: "falls back to the interpreter (not compiled): ldm.w stm.w ldm stm stmge ldmge") and
   flagged per-mnemonic attribution as optimistic one bullet later; the mistake was reading the rollup as
   demand. §5's 100.00% is unaffected — these were never the instructions behind it.
2. *"the back ends emit a call to `UserCallbacks::InterpreterFallback`"* — **true of x64 only, false of the
   back end we ship on.** `backend/x64/a32_emit_x64.cpp:1124-1134` does call it; the AArch64 back end's whole
   handling is `backend/arm64/emit_arm64_a32.cpp:36-38`, `ASSERT_FALSE("Interpret should never be emitted.")`,
   and riscv64 is identical (`backend/riscv64/emit_riscv64_a32.cpp:115-117`). That assert is **not**
   `NDEBUG`-gated (`externals/mcl/include/mcl/assert.hpp:54` → `externals/mcl/src/assert.cpp:14-20`: print,
   then `std::terminate()`), and `interface/A32/config.h:104` declares the callback `= 0`, so nothing is
   defaulted to nothing. The same overstatement sits in `core/exec/interp.cpp:13-17`, `docs/10` §5 AD-2(3) and
   `docs/30` §Step 6c; all three need this correction (not edited here).
3. *"Tier I is load-bearing on the JIT path for these"* — **Tier I cannot run them either.** The terminal
   captures `ir.current_location` *before* `translate_arm.cpp:63` advances the PC, so `next` is the offending
   instruction's own address and the block holds no IR (`ir/terminal.h:20-28`, `num_instructions = 1`).
   `BlockRunner::Take` treats `Term::Interpret` as a `LinkBlock` (`core/exec/interp.cpp`, the
   `is_same_v<T, Term::Interpret>` branch, ~1672 as written), so `successor` is the same PC and `steps` adds 0
   → `InterpExecutor::Run` re-translates the same empty block forever: **a livelock, not a refusal.** [INFER]
   — read off the code, not run: no compiler on this host, devices in use.

**7.3 What happens today, per tier and host.** Tier J is `Dynarmic::A32::Jit`, whose back end is picked by
*host* (`src/dynarmic/CMakeLists.txt:264,343`), so one RetroBug binary behaves differently on the two machines
our tests run on. **arm64 (shipping):** the terminal is hit while the block is *emitted*, and emission happens
inside `jit_->Run()` (`backend/arm64/a32_core.h:19-21` → `GetOrEmit`) → `std::terminate` → SIGABRT; `jit.cpp:187`
never returns and `MapHalt` never runs, so there is **no `RunResult`, no `StopReason`, no C17 record** — the
process dies at the first execution of a block containing one of six 32-bit words. **x86_64 (emulator lane):**
the x64 back end *does* call `JitExecutor::InterpreterFallback` (`core/exec/jit.cpp:395-438`) — real, correct
code, which advances exactly one encoding via `EncodingSizeAt` and propagates other stop reasons — and hands
it to Tier I, which livelocks per 7.2(3): this lane hangs where the hardware lane aborts. Were Tier I fixed to
refuse, the x64 path would return from `Run()` with no `HaltReason` bit and `pending_ == None`, which
`MapHalt` maps to `StopReason::Suspended` (`jit.cpp:217`) — an ordinary-looking stop, not a fault. A green
x86_64 run is therefore no evidence about this path either way.

**7.4 Observable today? No — and the suite is structurally unable to see it.** dynarmic's own A32 test
environment *asserts false* if the fallback is called (`tests/A32/testenv.h:98`, `:185`), so its 200,561
passing assertions prove the upstream suite never routes through `Term::Interpret` — and since those tests also
pass on arm64 hardware (§5 item 1), they never reach `ASSERT_FALSE` there either. The randomized
`A32/fuzz_arm.cpp`/`fuzz_thumb.cpp`, which would eventually generate one of the words, are built only under
`DYNARMIC_TESTS_USE_UNICORN` (`tests/CMakeLists.txt:37-49`), which §5 item 1 turns off. Our tests share one
fixture containing no banked form and no CPS/RFE/SRS, and `core/tests/isa_front_end_test.cpp:137` only checks
that a terminal is *in* `kTerminalNames` — `Interpret` is in that table (`core/isa/front_end.cpp:28`) — so a
wrong or missing Interpret path is invisible by construction.

**Smallest test that would catch it**, entirely in existing shapes: two
`__attribute__((target("arm"), noinline))` JNI exports in `tests/corpus/fixture_jni.c` — one body
`stmdb r13, {r4,r5}^\nldm r13, {r4,r5}^`, one `cps #0x13` — written like `patch_probe` (`fixture_jni.c:147-161`:
complete asm definition, fixed registers, `__builtin_unreachable()` after `bx lr`). Verified compilable with
this NDK at `-mthumb -Os`: the exports land in ARM state and objdump prints them with the caret and as `cps`,
which is the check that the probe reaches its target. Then assert three things, none a guessed number: that
`isa_front_end_test` reports `terminal == "Interpret"` for them; that **both tiers end the `Run` on a named stop
within a bounded budget** — the row that turns today's abort and today's hang into failures, precedent at
`core/tests/exec_tier_diff_test.cpp:260-285` ("equal hardware, *or* stop with `Unimplemented` naming the op; a
third outcome is a bug"); and that `MachineState` is bit-identical across the tiers at the stop. The SM-X710 can
settle the *expected* behaviour rather than our inventing it — `tests/corpus/oracle_armv7.c` already runs each
EL0-restricted probe separately and records `rc=132` (SIGILL) for the `mrc p15` reads — but that is a device
run, **not done here**; the expectation stays [UNVERIFIED].

**7.5 Other embedders.** From the lineage this tree came from: **Azahar** (Citra successor), **A32** front end,
ARM9+ARM11 — `src/core/arm/dynarmic/arm_dynarmic.cpp:102`
([raw](https://raw.githubusercontent.com/azahar-emu/azahar/master/src/core/arm/dynarmic/arm_dynarmic.cpp)) is
`UNREACHABLE_MSG("InterpeterFallback reached with pc = …, code = …, num = {}")`: neither implemented nor
patched out. And it *has* been hit — issue [#1044](https://github.com/azahar-emu/azahar/issues/1044) reports a
game crashing on boot at `code = 0xf10c0040`, which our own `llvm-objdump` decodes as **`cpsid f`**, an
`arm_CPS` body; reached because that embedder runs the guest's *kernel*, which is where these instructions
live. dynarmic's own envs assert false for A32 *and* A64 (§7.4), so upstream ships no reference implementation.
**Ryujinx** (ARMeilleure) and **PPSSPP** (own JIT) do not use Dynarmic's ARM front ends; **yuzu/sudachi/suyu
could not be read from here** (every raw and mirror URL 404'd) — [UNVERIFIED]. The A64 case does not transfer
anyway: A64 reaches `InterpretThisInstruction` from MRS/MSR for any system register off a short whitelist
(`frontend/A64/translate/impl/system.cpp:118,158`) plus three SIMD edges, and has a pass rewriting
`num_instructions` to span a run of them (`ir/opt/a64_merge_interpret_blocks.cpp:18-47`), while **A32 has no
such pass** and is always exactly one instruction: an open-ended interpreter obligation over there, a closed set
of six masks here.

**7.6 Recommendation: (c), plus the Tier I fix that makes (c) true on both tiers.** Cheapest thing that is
also correct, in this order.

1. **Tier I (~3 lines, ours only):** make `Term::Interpret` a named refusal in `BlockRunner::Take` —
   `StopReason::Unimplemented`, detail carrying pc and word — as ASIMD's `VectorBroadcast64` refusal is named
   today. This is the precondition for everything else: nothing here is testable while Tier I spins.
2. **Tier J, no patch (~15 lines, ours only):** the seam already exists — `CodeReader::PreCodeReadHook`
   (`core/include/rb/isa/front_end.h:57-62`, currently `return true`) is called by dynarmic for both states
   *before* a word is read or translated (`translate_arm.cpp:33`, `translate_thumb.cpp:114`), and
   `JitExecutor` inherits the same default and can override it identically. Returning **false** precludes the
   translation and hands the terminal to us (`frontend/A32/translate/translate_callbacks.h:23-25`;
   `interface/A32/config.h:70-72` says "returning true" and is stale against `if (!tcb->PreCodeReadHook(...))`).
   Set the terminal dynarmic's own `RaiseException` sets (`a32_translate_impl.cpp:48-54`: `BranchWritePC`,
   `ExceptionRaised`, `Term::CheckHalt{ReturnToDispatch}`), lowered to a real arm64 trampoline
   (`emit_arm64_a32.cpp:612-624`) and already received by `JitExecutor::ExceptionRaised`. Six mask tests, and
   both hosts on both tiers give the same named stop instead of one aborting and one hanging — with pc and
   word in the C17 record, which is what turns "some app won't start" into a triageable fact.
3. **(a)/(b), per-instruction re-entry: possible, cheap to add on top of 2, buys nothing now.**
   `num_instructions` is always 1 for A32, `RaiseException` leaves PC *past* the instruction, and `Jit` exposes
   `ClearHalt`/`SetCpsr` (`interface/A32/a32.h:65,75`), so `JitExecutor::Run` can loop `jit_->Run()` → one
   encoding through `fallback_interp_` → `SyncIn` → `ClearHalt` → re-enter; `jit.cpp:395-438` already holds the
   per-instruction half. Step 1 is what Tier I would do with it, which is the whole objection.
4. **(d), patching `emit_arm64_a32.cpp` to call the callback as x64 does: not yet.** Small, 0BSD-permitted, and
   inside `patches/README.md` rules 1-2 — `docs/30` §Step 18 already costed the comparable `address_space`
   patch as "~2 lines, permitted by AD-10, forbidden by the posture, nothing in M1 needs it" — but it is
   carried against every re-import, and its payload is a callback into a Tier I that has no semantics for
   these six: abort becomes hang unless step 1 lands first. The real blocker is not the dispatcher but the
   state model — `backend/arm64/a32_jitstate.h:18-31` has 16 GPRs and **no banked register file**, and neither
   does `MachineState`, so banked LDM/STM and `SRS`/`RFE` are not faithfully implementable at any tier without
   adding exception-mode state to both. Only the A/I/F-only `CPS` form is, and that is a CPSR write in code we
   own.

What would change my mind, in order of likelihood: an app-level corpus (§5 item 4) or a live C17 record from
step 2 showing a nonzero count; any decision to run guest-kernel or EL1 code, which moves this group from
unreachable to mandatory and makes the banked-register state model the real project; or a T1/iOS policy where
Tier I must *answer* rather than refuse, which is the case for implementing `CPS`-without-mode-change first.
