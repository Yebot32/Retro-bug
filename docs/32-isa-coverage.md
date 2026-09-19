# ARMv7 instruction demand vs engine coverage (M0 step 3, open risk U-8)

Measured 2026-09-19 with [`../tools/isa_coverage.sh`](../tools/isa_coverage.sh). Reproducible: the script
re-extracts and re-disassembles the corpus and prints the same summary. Raw histogram:
`build/isa-coverage/mnemonics.txt` (855 lines, not committed — regenerate).

## 1. What was measured, and why it is the right corpus

The NDK's own AArch32 archives, extracted member by member: `libc.a` (549 objects), `libm.a`,
`libc++_shared.so`, `libz.a` — **711 objects, 338,034 instructions, 855 distinct mnemonics** of real
compiled ARMv7 machine code. This is the code every guest app links against, so its instruction mix is
the demand curve the engine has to satisfy before any app-level `.so` is even considered. It is analysis
input on this machine only, never redistributed.

| Metric | Value |
|---|---|
| Instructions disassembled | 338,034 |
| Distinct mnemonics | 855 |
| Thumb-2 32-bit wide (`.w`) | 47,894 (14.2%) |
| **IT-block instructions (`it`, `itt`, `ite`, …)** | **8,387** |
| Vector/FP mnemonics | 166 distinct / 14,723 instructions (4.4%) |

Top of the demand curve: `mov` 32,371 · `ldr` 32,183 · `cmp` 20,800 · `add` 18,542 · `str` 13,484 ·
`movs` 12,967 · `bl` 11,254 · `b` 10,967 · `add.w` 7,923 · `blx` 7,532 · `ldr.w` 5,977 · `beq` 5,687 ·
`it` 4,955 · `movw` 3,284 · `cbz` 3,271 · `itt` 2,609.
Vector/FP top: `vldr` 2,290 · `vmov` 1,580 · `vst1.64` 815 · `vstr` 788 · `vmul.f64` 786 ·
`vadd.f64` 744 · `vmrs` 725 · `vmla.f64` 651 · `vst1.32` 481 · `vcmp.f64` 470.

## 2. Four conclusions that change design assumptions

1. **IT-blocks are load-bearing in practice, not academic.** `docs/research/01` named IT-block TB
   splitting as the single biggest correctness risk, on the reasoning that it is subtle. The measurement
   adds the other half of the argument: bionic itself emits 8,387 IT-block instructions, so any engine
   defect there breaks system libraries before it breaks an app. This is now the highest-priority lane in
   the differential suite (§5), not a nice-to-have.
2. **VFPv3 double precision is required, not optional.** `vmul.f64`/`vadd.f64`/`vmla.f64`/`vcmp.f64`
   all appear hundreds of times — libm's `double` paths. An engine scoped to single-precision-only FP
   (a plausible mistake when the reference target was a 3DS ARM11) fails here.
3. **ARMv7-only encodings are unavoidable.** `movw` (3,284) and `cbz` (3,271) are ARMv7-era; SDIV/UDIV
   are used too. This is the concrete form of the ARMv6-vs-ARMv7-A question the architecture had to
   answer before committing to Dynarmic (`docs/10` §4.1).
4. **`vst1.64`/`vst1.32` register-list forms are common** (815/481), so the ASIMD load-store-structure
   group must be right including its alignment and register-count edge cases.

## 3. Corrections to how U-8 was argued before this measurement

Two earlier claims were weaker than they looked, and saying so keeps them from being reused as evidence.

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
2. **Mnemonic→handler mapping tool.** Automate the presence half: map each histogram entry onto the
   `Translate*` group that implements it, so "which of the 855 mnemonics real code uses are unhandled" is
   a query rather than a grep. Small, and it turns §4 into a number.
3. **IT-block and FP-status vectors first.** Ranked by §2: IT-blocks, `f64` VFP paths, `vmrs`/FPSCR,
   ASIMD structures — these are where Dynarmic's own suite is thinnest relative to measured demand.
4. Repeat on the **app-level** corpus once `docs/research/10`'s APK fixtures are downloaded, since
   bionic will under-represent what games' hand-written assembly and codec libraries use.

## 6. Reproducing

```bash
bash tools/isa_coverage.sh [output-dir]     # default build/isa-coverage
# needs: ANDROID_HOME (or the default %LOCALAPPDATA%/Android/Sdk), an NDK with AArch32 sysroot;
# RB_NDK overrides 28.2.13676358. No Python.
```
