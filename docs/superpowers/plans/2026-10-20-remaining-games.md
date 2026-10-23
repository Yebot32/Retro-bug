# Remaining Unity Game Walls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the remaining Unity games past their named walls — starting with the shared null platform-table call (Crash Drive 2, Angry Birds Epic), the Cars 3 UI stall, the Teeny Titans 2 boot abort, and Rolling Sky's host-ART field error — and verify each on the Tab S9.

**Architecture:** Three independent static investigations run in parallel with sub-agents (each read-only, each must return file/offset evidence). In parallel the main agent adds a SIGABRT-delivery stack capture so TT2's abort caller can be named. Each finding becomes a minimal engine/loader/repack fix with a test and a device acceptance run. No fix is claimed without a gate run and a screenshot/log from the tablet.

**Tech Stack:** C++20 engine (Dynarmic A32 JIT, custom ELF loader, syscall monitor, JNI bridge), PowerShell+Cecil for managed tracing, NDK llvm-objdump/readelf, adb/Tab S9 (R52Y106BECZ), RetroBug launcher repack loop.

**Spec:** `NEXT-AI-HANDOFF.txt`, `docs/session/worklog.md` (2026-10-20/21), `docs/compatibility.md` rows 3/6/7/17/25.

## Global Constraints

- Never modify or replace the natively installed originals; only `<pkg>.rb` copies.
- The engine is baked into each converted APK: an engine change requires a repack before a device test.
- Build with the SDK's own ninja at `%LOCALAPPDATA%\Android\Sdk\cmake\3.31.6\bin\ninja.exe`; package with `build/build_launcher_noclean.sh`.
- Every engine fix gets a test lane and the full 23-suite gate on the SM-X710 before commit.
- Every false/unknown fact is refused or named; never fabricate a `siginfo`, a table, or a symbol.
- Commit/push only after the gate and device evidence; do not merge into `main`.

---

### Task 1: Parallel static investigations (three sub-agents)

**Files:** read-only; outputs are reports in the conversation.
- C41: `build/m5/com.spilgames.a10.google.crashdrive2/` and `build/m5/com.rovio.gold/` (orig.apk, libunity.so, logs).
- C37: `build/m5/com.disney.cars3_goo/libunity.so`, `.ARM.exidx`, runtime logs.
- Rolling Sky: `build/m5/com.turbochilli.rollingsky/orig.apk`, DEX/smali.

- [ ] **Step 1:** Dispatch agent A with the CD2 fault (`pc 0x4`, `lr 0x11d645c`) and ABE fault (`pc 0x4`, `lr libunity+0x597b3c`): compute libunity offsets from each run's load base, disassemble the call site, identify the null table entry/object/offset and what should fill it. Acceptance: exact `module+offset`, instruction, table offset, candidate filler, and a proposed test.
- [ ] **Step 2:** Dispatch agent B with the Cars 3 stall (`adopted tid 4245`, `lr libc+0x78465`, caller `libunity+0x55c89c`, futex `0xfaae1ac8`, stack `libunity+0x39243c`): decode the wait function and its callers via objdump/exidx, name the Unity subsystem, and state the missing engine behavior. Acceptance: named function/offset and a concrete hypothesis with evidence.
- [ ] **Step 3:** Dispatch agent C with Rolling Sky's `NoSuchFieldError libcore.util.EmptyArray.INT`: decode the DEX, find the exact class/method/field use, and propose a minimal smali/Java patch or shim. Acceptance: exact smali path/line and the patch text.

### Task 2: TT2 SIGABRT-delivery stack capture

**Files:**
- Modify: `core/runtime/guest_process.cpp` (signal-deliver lambda), `core/include/rb/runtime/guest_process.h` (public `StackWords`).
- Test: build + existing lanes; device run.

**Interfaces:**
- Produces: `std::string GuestProcess::StackWords(GuestAddr sp, std::size_t bytes) const` returning module-named words separated by ` <- `.
- Consumes: `modules()`, `mem_`.

- [ ] **Step 1:** Add `StackWords` (reuse the StallReport scan) and log `signals: SIGABRT interrupted sp … stack …` when a delivery of signal 6 succeeds.
- [ ] **Step 2:** Build `rbguest`, package, install, repack TT2, run; read the new line.
- [ ] **Step 3:** Symbolize the first caller(s) in the report and record the abort path.

### Task 3: Fix C41 (null platform table)

**Files:** whichever engine/loader/repack file the evidence names.
- [ ] **Step 1:** Write the failing test that reproduces the null table (a loader/JNI/bridge unit test, or a fixture).
- [ ] **Step 2:** Implement the minimal fix.
- [ ] **Step 3:** Build, run the gate, repack CD2 and ABE, launch both on the Tab S9.
- [ ] **Step 4:** Acceptance: both reach the next screen (splash/menu) or a new named wall; evidence screenshots/logs.

### Task 4: Fix C37 (Cars 3 UI stall)

**Files:** per agent B's evidence.
- [ ] **Step 1:** Add the missing engine behavior with a test.
- [ ] **Step 2:** Gate; repack Cars 3; run on the Tab S9.
- [ ] **Step 3:** Acceptance: Cars 3 reaches MainMenu or a new named wall; screenshot/log.

### Task 5: Rolling Sky host-ART patch

**Files:** repack/DEX patch per agent C; possibly a Java shim in the launcher's shim DEX.
- [ ] **Step 1:** Apply the patch to a copy of the APK; repack via the normal path.
- [ ] **Step 2:** Install and launch; acceptance: no `NoSuchFieldError`, or a new named wall.

### Task 6: Regression gate, docs, commit/push

- [ ] **Step 1:** Full 23-suite gate on the SM-X710; launcher APK packages.
- [ ] **Step 2:** Update `docs/compatibility.md`, `docs/session/worklog.md`, `backlog.md`, `NEXT-AI-HANDOFF.txt`, memory.
- [ ] **Step 3:** Commit engine and docs separately; push `session/m5-m6`; verify the remote SHA.
