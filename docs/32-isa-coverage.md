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
     interpretation, not compilation.** The matched decoder cases include `arm_LDM_usr`, `arm_STM_usr`,
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
   the FP work surfaced something §2's ranking had not: **`VCMP`'s FPSCR compare field on this ARMv8 host
   is not the ARMv7 manual's table** — three of four relations differ, in the GE bit and in NV for
   unordered — with both tiers following the host. Whether a genuine ARMv7 VFP agrees with the manual
   here is the next question in this lane, and it is [UNVERIFIED]: `docs/30` §Step 17 states the table,
   the experiment and the fallback.
4. Repeat on the **app-level** corpus once `docs/research/10`'s APK fixtures are downloaded, since
   bionic will under-represent what games' hand-written assembly and codec libraries use.

## 6. Reproducing

```bash
bash tools/isa_coverage.sh [output-dir]     # default build/isa-coverage
# needs: ANDROID_HOME (or the default %LOCALAPPDATA%/Android/Sdk), an NDK with AArch32 sysroot;
# RB_NDK overrides 28.2.13676358. No Python.
```
