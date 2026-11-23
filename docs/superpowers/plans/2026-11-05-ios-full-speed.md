# iOS at Full Speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Both games run at interactive speed on the owner's iPad with no silent deaths — Tier J when the
launch can run a JIT, the translated profile when it cannot, and no missing instruction in whichever tier
executes a block.

**Architecture:** The engine already carries three executors behind one seam (`rb::exec::Executor`):
Tier J (Dynarmic's AArch64 JIT), AOT (offline-emitted AArch64 blocks + fastmem), Tier I (the IR
interpreter). The iOS work is to make each executor *work on iOS* rather than to build new ones: AOT's
runtime path must be free of runtime code generation (the spin-lock fix proved this was the whole crash),
Tier J's faults must be observable (the Mach port hid them) and then fixed, and every instruction a game
executes must have semantics in the tier that runs it.

**Tech Stack:** C++20, Dynarmic (vendored, patched under `patches/dynarmic/`), oaknut, CMake, XcodeGen,
GitHub Actions (Linux host lane, macOS host lane, iOS arm64 compile, unsigned IPA), `adb` to the SM-X710
tablet for the Android gate, AltStore-style signing for the iPad.

**Spec:** `docs/superpowers/specs/2026-10-31-aot-native-speed-design.md` (the AOT design this plan
extends). Evidence for every task: the device logs `retrobug (23)`–`retrobug (26)` beside this repo, and
the AOT scope harness `rb_aot_scope_tests` on the tablet.

## Global Constraints

- No runtime code generation on the AOT path — no JIT object, no executable allocation, no lazily emitted
  stubs. Every piece of code the AOT tier runs is compiled into the app.
- Never commit game bytes (APK or extracted `.so`); per-game profiles live on the private `aot/profiles`
  ref only.
- Every delivered IPA carries a unique `CFBundleVersion` and a published SHA-256.
- Android behaviour unchanged: `rb_exec_interp_tests` 1112, `rb_exec_tier_diff_tests` 1039,
  `rb_interp_neon_tests` 11, `rb_aot_tests` 178, `rb_aotgen_tests` 48 all pass on the SM-X710 tablet.
- Acceptance for device work is a named log line, not an impression: every task below states the line
  that proves it.

---

### Task 1: AOT reaches the first game frame on the iPad

**Files:**
- Modify: `core/exec/aot.cpp` / `platform/ios/ios_session.cpp` only as the log names a wall
- Test: `rb_aot_scope_tests` and `rb_aot_tests` on the tablet (already green)

**Interfaces:**
- Consumes: the build-38 runtime (spin lock as atomics on Apple, AOT attempted after a JIT crash).
- Produces: `engine: guest code runs on AOT (offline-translated AArch64)` followed by
  `render: N frames in 10 s (F fps)` with N > 1.

- [ ] **Step 1: Install build 38 and launch KitKat, then Blocky.**

Expected on the first launch after install: `jit: the code region can be made writable — trying Tier J`
(or the no-grant line), then either a Tier J boot or `engine: guest code runs on AOT`.
On a second launch (JIT marker set): `jit: the previous Tier J attempt … did not finish` then
`aot: profile aot_kitkat matches (5 libraries)` → `engine: guest code runs on AOT`.

- [ ] **Step 2: If the log ends inside AOT, name the step.**

The `aot-dbg:` breadcrumbs (`run`, `synced`, `dispatch`, `entry`, `code-enter`, `code-exit`) say where.
If the log has a `=== RetroBug host crash` block, its frames name the faulting host function. Fix that
function's iOS assumption and repeat from Step 1.

- [ ] **Step 3: If the log names a Tier I instruction, implement it.**

A refused AOT block falls back to Tier I; a missing op prints `<Opcode>: no Tier I semantics yet`.
Implement that opcode in `core/exec/interp.cpp` beside its family, mirroring the A64 emitter's lowering
(`external/dynarmic/src/dynarmic/backend/arm64/emit_arm64_*.cpp`).

- [ ] **Step 4: Verify the gate and commit.**

Run on the tablet: `rb_exec_interp_tests`, `rb_exec_tier_diff_tests`, `rb_aot_tests`, `rb_interp_neon_tests`.
All pass. Commit with the device log's proving line quoted.

### Task 2: Tier J faults name themselves, then stop happening

**Files:**
- Modify: `external/dynarmic/src/dynarmic/backend/exception_handler_macos.cpp` (already: iOS registers
  nothing — patch 0010) and whatever the fault names
- Test: the same device loop; the tablet's Tier J path is already exercised by the Android launcher

**Interfaces:**
- Consumes: `CrashSignal` in `platform/ios/ios_session.cpp` (alternate stack, stderr fallback).
- Produces: a `=== RetroBug host crash: signal SIGSEGV/SIGBUS` block with `host frames:` naming the
  faulting function, or `engine: guest code runs on Tier J (JIT)` and frames advancing.

- [ ] **Step 1: Launch with JIT and read the crash block.**

Build 38 disables the Mach port, so a Tier J fault is an ordinary signal: the crash block will show the
signal, `translation pc`, `guest thread pc`, and `host frames`. The top RetroBug frame is the target.

- [ ] **Step 2: Fix the named fault.**

Likely candidates, in order: a page-table fastmem access the 16 KiB host page cannot serve in place (fix
in `GuestMemory::Host`/`RefreshPages`), a W^X flip in the JIT code cache (fix in the oaknut path under
`patches/dynarmic/`), or an iOS-specific missing host call. Each fix ships as a patch file under
`patches/dynarmic/` when it touches the vendored tree.

- [ ] **Step 3: Prove it with the engine line and the fps line.**

`engine: guest code runs on Tier J (JIT)` and `render: … fps` climbing to the game's cap. Commit with
both lines quoted.

### Task 3: Measure and close the remaining performance gap

**Files:**
- Modify: `core/exec/aot.cpp` (dispatch/fallback), `tools/aotgen/*` (coverage) as the measurements say
- Test: the device log's `render: … fps`, `aot-runtime: … dispatches, … cache hits, … fallbacks`

**Interfaces:**
- Consumes: the 10 s report (`render:`, `aot-runtime:`, six `aot-fallback` sites).
- Produces: Blocky at its frame cap with no multi-second frame after warm-up; KitKat ≥ 30 fps at the
  loading screen and in play.

- [ ] **Step 1: Read the report on whichever engine ran.**

Cache-hit rate below 90 % → tune `kDispatchCacheSize`/cache key. Fallbacks above 1 % of dispatches → go to
Task 4. Long frames (>2 s) with `stall` reports → the fault report's pc names the guest code; decide
whether it is asset loading (legitimate) or a tier gap.

- [ ] **Step 2: Apply the measurement-driven change, re-test, commit.**

One change per commit, with the before/after lines in the message.

### Task 4: Bulk compatibility from the profile's own refusals

**Files:**
- Modify: `tools/aotgen/emit.cpp` (refusal recording), `tools/aotgen/main.cpp` (profile report)
- Modify: `core/exec/interp.cpp` (the opcodes the hot refusals name)
- Test: `rb_aotgen_tests`, the tablet gate, the device log's fallback sites

**Interfaces:**
- Consumes: per-block refusal reasons in the profile output (`refused` counts by kind today).
- Produces: a profile manifest line per library listing the top refused *opcodes* and counts, and Tier I
  semantics (or emitter support) for each opcode above a threshold.

- [ ] **Step 1: Record the refusing opcode per refused block.**

In `EmitQueued`'s refusal paths, capture the IR opcode that triggered the refusal into a per-library
histogram and print it in the profile summary.

- [ ] **Step 2: Implement the top opcodes in the family the log names.**

Each opcode: a `case` beside its family in `interp.cpp`, semantics copied from the A64 emitter's lowering
for that op, and a tier-diff fixture row when the fixture can encode it.

- [ ] **Step 3: Regenerate the profiles, rebuild the IPA, re-test on device.**

Commit with the fallback count before/after from the log.

---

## Self-review

- The three executors are covered: AOT (Task 1), Tier J (Task 2), Tier I (Tasks 1/4). Performance (Task 3)
  and compatibility (Task 4) each have a measurement and an acceptance line.
- No task re-tests what build 38 already proves (spin lock, Mach port, VShift) without a new claim.
- Every task ends with a gate run and a commit quoting the device log.
