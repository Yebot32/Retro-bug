# AOT native speed for iOS — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Translate a game's 32-bit ARM `.so` files ahead of time on the PC and link the result into a
per-game iOS app, so guest native code runs at near-native CPU speed with **no runtime code generation**.

**Architecture:** Reuse Dynarmic's A32 front end as the decoder and its arm64 `EmitArm64` as the code
generator, driven by an offline host tool (`rb_aotgen`). The emitted blocks are linked into the app behind
a small dispatch prelude that reproduces Dynarmic's JIT-state/register ABI; the guest's code map (static
discovery merged with recorded indirect targets) decides what is translated, and Tier I interprets any
target the code map does not contain. The IPA carries translated code and a code map, never the APK; at
import the app matches the APK's library hashes before using AOT.

**Tech Stack:** C++20; Dynarmic (vendored fork, 0BSD) A32 front end + arm64 backend; oaknut codegen; CMake;
GitHub Actions (Linux/macOS host lanes, iOS arm64, IPA); adb + the SM-X710 tablet for traces; the M1 iPad
for measurements.

**Spec:** `docs/superpowers/specs/2026-10-31-aot-native-speed-design.md` (research: measured entry-point
coverage, emission separability, packaging constraints). `docs/ios-notes.txt` §4 is the originating idea.

## Global Constraints

- **No runtime JIT on the AOT path.** Nothing in these tasks may call `mprotect(PROT_EXEC)`, `MAP_JIT`, or
  construct `Dynarmic::A32::Jit` on the device. The existing opt-in JIT switch is unrelated and stays.
- **Vendored-tree rule:** every change under `external/dynarmic` must be a patch in `patches/dynarmic/`
  whose `git apply --check --reverse --directory=external/dynarmic <patch>` succeeds from the repo root,
  described in `patches/dynarmic/README.md` ("Why:" / "Retire when:"), per `docs/10-architecture.md` AD-14.
- **Never commit a game APK or extracted game bytes.** Generated translation code and code maps live in
  `build/aot/<game>/` on the PC; only tool/test code and fixtures go into git.
- **Android behavior unchanged:** every shared-engine change must pass the Linux host lane and the arm64
  device gate (`rb_exec_interp_tests` 1096 checks, `rb_exec_tier_diff_tests` 1039 checks,
  `rb_interp_neon_tests` 11, `rb_interp_bench`) before it is considered done.
- **Every delivered IPA gets a unique `CFBundleVersion`** (currently 20) and a SHA-256 in the worklog.
- **Host tests are fixture-free unless marked device-only** (the CI lanes have no `librb_fixture.so`).

---

### Task 1: Patch 0008 — let a host tool emit a block outside the JIT

**Files:**
- Modify: `external/dynarmic/src/dynarmic/backend/arm64/address_space.h`
- Modify: `external/dynarmic/src/dynarmic/backend/arm64/emit_arm64_a32_coprocessor.cpp`
- Create: `patches/dynarmic/0008-aot-offline-emission.patch`
- Modify: `patches/dynarmic/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: `Dynarmic::Backend::Arm64::EmitArm64(CodeGenerator&, IR::Block, const EmitConfig&, FastmemManager&)`
  and its `EmittedBlockInfo{entry_point, size, relocations, block_relocations, fastmem_patch_info}`, both
  already present at `emit_arm64.h:101-107,166`; the patch only makes IR generation and block emission
  reachable from outside the address space.

**Pre-flight ruling (2026-10-31):** the original Step 3 rewrote the coprocessor callback's absolute
`MOV` into a literal load with `relocations.emplace_back(pointer.offset(), LinkTarget{&callback.data})`.
That cannot compile: `LinkTarget` is a plain enum (`emit_arm64.h:43-84`), `Relocation` carries no payload
(`:86-89`), there is no `relocations` in scope in `CallCoprocCallback` (the container is
`ctx.ebi.relocations`), and oaknut has no `MOVP`. Position-dependent blocks are instead **refused** by the
AOT tool (Task 3): any block containing a coprocessor transfer or an FP-fallback helper — the only two
places in the A32 emitter that bake a host address (`emit_arm64_a32_coprocessor.cpp:30,33,109,151-152,195,234-235`,
`emit_arm64_vector_floating_point.cpp:271`) — stays on Tier I. This keeps the JIT byte-identical and the
patch to one function each. Follow-up if the measurements show a hot refused block: make those sites
relocatable with a payload-bearing relocation.

- [ ] **Step 1: Read the coupling points.** Open `external/dynarmic/src/dynarmic/backend/arm64/emit_arm64.h`
  (the `EmitConfig` fields and `EmitArm64`'s signature), `a32_address_space.cpp:23-72,193-357` (how the
  config is built and how the prelude bakes callbacks), and `emit_arm64_a32_coprocessor.cpp:29-34` (the
  absolute `MOV Xscratch0, fn` that has no relocation record). Write down the exact `EmitConfig` field
  values the address space uses — the AOT harness must use the same ones.

- [ ] **Step 2: Expose the emission entry point.** `AddressSpace::GenerateIR` is already public
  (`address_space.h:30`), so the tool calls it directly; the patch adds only `EmitForAot`. In
  `address_space.h`, next to the protected `Emit`/`block_infos`:

```cpp
public:
    /// AOT (RetroBug patch 0008): translate one block into `code` without touching the code cache, and
    /// return its size and relocation records. The caller owns the buffer and the linking step; the JIT
    /// path is unchanged and still uses Emit()/Link() below.
    EmittedBlockInfo EmitForAot(IR::Block& ir_block, oaknut::CodeGenerator& code);
```

  and in `address_space.cpp` (definition beside `Emit`; `IR::Block` is passed by value to `EmitArm64`, so
  the wrapper moves it):

```cpp
EmittedBlockInfo AddressSpace::EmitForAot(IR::Block& ir_block, oaknut::CodeGenerator& code) {
    return EmitArm64(code, std::move(ir_block), GetEmitConfig(), fastmem_manager);
}
```

- [ ] **Step 3: Confirm the position-dependent sites are the two the ruling names.** `git grep -n
  "bit_cast<u64>\|reinterpret_cast<u64>" external/dynarmic/src/dynarmic/backend/arm64/emit_arm64*.cpp`,
  ignoring `a64_*` (the A32 front end never reaches it), `address_space`/prelude (not block emission) and
  `emit_arm64_memory.cpp` (`FastmemPatchInfo.call_pc`, only with a page table, which the tool disables).
  The result must be only the coprocessor and vector-FP fallback lines quoted in the ruling above; if
  anything else appears, stop and report before proceeding — the tool's refusal list depends on it.

- [ ] **Step 4: Record the patch.**

```bash
git -C external/dynarmic diff > patches/dynarmic/0008-aot-offline-emission.patch
git apply --check --reverse --directory=external/dynarmic patches/dynarmic/0008-aot-offline-emission.patch
```

  The check must exit 0. Append a `0008` section to `patches/dynarmic/README.md` in the existing format:
  Why (offline AOT must emit blocks outside the code cache; coprocessor immediates made blocks
  position-dependent), Retire when (upstream exposes an offline emission API).

- [ ] **Step 5: Verify the JIT still works.** Rebuild the arm64 device tests and run the JIT-differential
  suite, which exercises `Link`/`RelinkForDescriptor` and the coprocessor callbacks:

```bash
powershell -ExecutionPolicy Bypass -File build/build_direct.ps1 -BuildDir build/tests-arm64-v8a rb_exec_tier_diff_tests
MSYS_NO_PATHCONV=1 adb -s R52Y106BECZ push build/tests-arm64-v8a/core/tests/rb_exec_tier_diff_tests /data/local/tmp/
MSYS_NO_PATHCONV=1 adb -s R52Y106BECZ shell "cd /data/local/tmp && chmod 755 rb_exec_tier_diff_tests && ./rb_exec_tier_diff_tests | tail -1"
```

  Expected: `1039 checks, 0 failures`. Commit.

---

### Task 2: `rb_aotgen` — code-map discovery with measured coverage

**Files:**
- Create: `tools/aotgen/main.cpp`
- Create: `tools/aotgen/code_map.{h,cpp}` (ELF32 reader + discovery)
- Create: `tools/aotgen/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add the subdirectory when not cross-compiling)
- Create: `tools/aotgen/code_map_test.cpp`

**Interfaces:**
- Consumes: nothing (self-contained ELF32 reader; do **not** depend on `rb::loader`, which targets guest
  loading rather than inspection).
- Produces:

```cpp
struct CodeMapEntry { std::uint32_t off; std::uint16_t size; std::uint8_t flags; std::uint8_t sources; };
struct CodeMap { std::string soname; std::uint32_t text_off, text_len; std::vector<CodeMapEntry> entries; };
enum : std::uint8_t { kThumb = 1, kArm = 2 };
enum : std::uint8_t { kFromExport = 1, kFromInit = 2, kFromReloc = 4, kFromExidx = 8, kFromVeneer = 16, kFromTrace = 32 };
CodeMap DiscoverCodeMap(const std::vector<std::uint8_t>& elf, const CodeMap* trace, std::string* err);
std::size_t CountSource(const CodeMap& map, std::uint8_t source);   // entries carrying `source`
```

- [ ] **Step 1: Write the failing test.** In `tools/aotgen/code_map_test.cpp`, take two library paths from
  `argv` (defaults `build/kk-inspect/lib/armeabi/libdlgl.so` and
  `build/br-inspect/lib/armeabi-v7a/libgame.so`; on the tablet pass `/data/local/tmp/...`), skip with a
  printed line when absent, and assert the measured lower bounds from the research:

```cpp
int main() {
    const auto lib = ReadFile("build/br-inspect/lib/armeabi-v7a/libgame.so");
    if (lib.empty()) { std::printf("# no libgame.so — skipped\n"); return 0; }
    std::string err;
    const CodeMap map = DiscoverCodeMap(lib, nullptr, &err);
    if (!map.entries.empty() && map.entries[0].off == 0) { /* no-op, keeps the struct used */ }
    Check(map.entries.size() >= 16000, "libgame starts >= 16000");
    Check(map.entries.size() <= 18000, "libgame starts <= 18000");
    Check(CountSource(map, kFromExidx) >= 14000, "exidx is the main source");
    Check(CountSource(map, kFromVeneer) >= 1500, "veneer scan found the movw/movt stubs");
}
```

  Run it in the Android arm64 tree and on the tablet (this PC has no host compiler; the CI host lanes
  build it too but skip without the corpus):

```bash
powershell -ExecutionPolicy Bypass -File build/build_direct.ps1 -BuildDir build/tests-arm64-v8a rb_aotgen_tests
MSYS_NO_PATHCONV=1 adb -s R52Y106BECZ push build/tests-arm64-v8a/tools/aotgen/rb_aotgen_tests /data/local/tmp/
MSYS_NO_PATHCONV=1 adb -s R52Y106BECZ shell "cd /data/local/tmp && chmod 755 rb_aotgen_tests && ./rb_aotgen_tests"
```

  Expect failure: the symbols do not exist yet.

- [ ] **Step 2: Implement discovery** in `code_map.cpp`, in this order, each adding `sources` bits to a
  merged `map<uint32_t, Entry>`:
  1. `.dynsym`/`.symtab` `STT_FUNC` with value inside `.text` (use `st_size` when non-zero);
  2. `DT_INIT`/`DT_INIT_ARRAY` (from `.dynamic`, addend is a `vaddr`; bias by the image base that the
     runtime will use — the tool emits *file-relative* offsets: `vaddr - text_vaddr`);
  3. `.rel.dyn`/`.rel.plt` entries whose addend lands in `.text` (`R_ARM_ABS32`, `R_ARM_RELATIVE`,
     `R_ARM_GLOB_DAT` addends stored in-place);
  4. `.ARM.exidx` (8-byte entries, prel31-encoded words): every non-`EXIDX_CANTUNWIND` start;
  5. veneer pattern: Thumb `movw r12,#lo; movt r12,#hi; add r12,pc; bx r12` (12 B) and the ARM 5-word stub;
  6. optional `trace` merge (`kFromTrace`, `kFromIndirect` sites recorded on device — Task 4).
  For each entry record Thumb vs ARM from the low bit at discovery (`exidx` starts are Thumb-tagged).

- [ ] **Step 3: Run the test, then verify against the research table.** Additional assertions with the
  dlgl image: `entries.size() >= 2600`, `kFromExidx >= 2000`, and both images' `text_len` equal to their
  section sizes (364 KB / 1,931 KB ± 1 page). Print a one-line summary per image
  (`aotgen: libgame starts N (export X, init Y, reloc Z, exidx E, veneer V)`).

- [ ] **Step 4: Wire the targets and commit.** In `tools/aotgen/CMakeLists.txt` add
  `add_executable(rb_aotgen main.cpp code_map.cpp)` (the discovery CLI — pure ELF, builds everywhere) and
  `add_executable(rb_aotgen_tests code_map_test.cpp code_map.cpp)`, and in the top-level `CMakeLists.txt`
  after `add_subdirectory(core)`:

```cmake
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tools/aotgen)
endif()
```

  **Pre-flight ruling:** Task 3's emitter references `EmitArm64`, which only exists when Dynarmic's arm64
  backend is in the build, and `ARCHITECTURE` is auto-detected from the host — on the x86_64 host lanes
  that backend is absent, so an unconditional emission target would fail to link there. Task 3 therefore
  adds `emit.cpp` and its dynarmic link only under `if(RB_AARCH64_HOST)`, exposing the emission subcommand
  to `main.cpp` through a `RB_AOTGEN_EMIT` compile definition; the code-map reader and its test build on
  every host. The IPA is generated on the arm64 macOS runner, which is where the emitter runs for real.

  Build and run the test via the Android arm64 lane (above); commit.

---

### Task 3: AOT runtime — prelude, dispatcher, fixture `fib` end to end

**Files:**
- Create: `core/include/rb/exec/aot.h`, `core/exec/aot.cpp`
- Create: `tools/aotgen/emit.cpp` (block emission + symbolic linking)
- Modify: `tools/aotgen/CMakeLists.txt` (add `emit.cpp` to `rb_aotgen`)
- Create: `core/tests/aot_test.cpp`, `core/tests/aot_fixture.S`, `core/tests/aot_fixture_table.cpp`
  (the last two generated once by `rb_aotgen` from `tests/corpus/fixture_jni.c` and committed — the fixture
  is our own code, not game bytes; the test's header says how to regenerate them)
- Modify: `core/CMakeLists.txt` (add `exec/aot.cpp` to `rb_exec`), `core/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `EmitForAot`, Task 2's `CodeMap`.
- Produces:

```cpp
namespace rb::exec {
/// One translated guest block: where it lives, how it is entered, and what it must re-link when the
/// dispatcher loads the image (the AOT analogue of AddressSpace::Link).
struct AotBlock { std::uint32_t guest_off; std::uint8_t flags; AotCode entry; /* u32 (*)(AotState*) */ };
/// The dispatch environment: the prelude expects these at the same offsets Dynarmic's JitState uses.
struct AotState { /* copied from a32_jitstate.h: registers, CPSR, FPSCR, ticks, halt_reason */ };
class AotImage {                      // one per translated .so
  bool Load(const AotBlock* blocks, std::size_t n, const CodeMap* map, std::string* err);
  AotCode Resolve(std::uint32_t guest_off, bool thumb) const;   // nullptr → Tier I fallback
};
}
```

- [ ] **Step 1: Write the failing fixture test.** `core/tests/aot_test.cpp`: read the generated
  `aot_fixture_table.cpp` (built by `rb_aotgen` from `tests/corpus/fixture_jni.c` at build time), call the
  translated `Java_com_retrobug_fixture_MainActivity_fib` for n = 0..20 through the AOT dispatcher, and
  compare each result and the final `MachineState` (r0..r12, sp, lr, pc, cpsr, fpscr) against Tier I on the
  same fixture. Expected: identical; the oracle value for 20 is 6765 and the final CPSR matches
  `tests/corpus/oracle-armv7.txt`. Build and run: it fails because `AotImage` does not exist.

- [ ] **Step 2: Emit the prelude offline, not at runtime.** **Pre-flight ruling:** the original wording
  ("copy `EmitPrelude` in `core/exec/aot.cpp`") would have the device generate machine code at runtime,
  which the Global Constraints forbid and which needs executable memory. Instead `tools/aotgen/emit.cpp`
  emits the prelude — Dynarmic's `EmitPrelude` logic (`a32_address_space.cpp:225-357`) rewritten against
  symbolic fixups instead of baked addresses — into the same `aot_<game>.S` as the blocks. The runtime side
  (`core/exec/aot.cpp`) contains no code generation: it holds the block table (`AotImage`), the
  return-to-dispatcher C++ entry the prelude jumps to, and the callbacks the prelude's trampolines call.
  The prelude differs from the JIT's in exactly two ways: `Xpagetable`/`Xfastmem` are 0 (the tool sets no
  page table and no fastmem, so every access goes through the callback relocations), and the
  `return_to_dispatcher` trampoline calls the AOT dispatcher instead of `GetOrEmit`. Add `static_assert`s
  for every JitState offset the prelude reads, comparing `AotState` against `A32JitState`
  (`a32_jitstate.h:17-43`); do not hand-edit the register mapping — if an offset differs, fix `AotState`.

- [ ] **Step 3: Emit blocks from the tool.** In `tools/aotgen/emit.cpp`, for each `CodeMap` entry decode
  from its guest offset with `Dynarmic::A32::Translate(...)` + a `CodeReader` over the ELF's text bytes
  (copy `core/isa/front_end.cpp`'s reader), call `EmitForAot`, append the bytes to one oaknut
  `CodeGenerator` stream, and record:
  - the block's code offset for the entry table;
  - every `Relocation`: to the prelude (callback trampolines), to the dispatcher (block exits), and to the
    literal pool — emit symbolic fixups (`MOVW/MOVT` pairs) rather than numbers;
  - `BlockRelocation`s (cross-block `B`/`ADRL`) as links to the target block's entry once it is emitted;
    missing targets get a dispatcher stub.
  Output `aot_<game>.S` (the code + tables) and `aot_<game>_table.cpp` (the `AotBlock[]` with guest
  offsets, flags and symbol names). The emitted `.S` is assembled by clang as part of the normal build.

  **Refusal rule (pre-flight ruling):** a block whose guest bytes contain a coprocessor transfer
  (ARM bits 27-25 = 110, or the Thumb `0xEE00`/`0xEC00` families) or whose IR contains one of the
  vector-FP fallback opcodes that call `EmitTwoOpFallback*`/`EmitThreeOpFallback*`
  (`emit_arm64_vector_floating_point.cpp:271`) is **not emitted**: it stays on Tier I, which already models
  CP15's thread pointer and CNTVCT and names anything else. Both emitters bake host addresses that no
  relocation record covers (Task 1's ruling); refusing the block is the only safe choice this plan makes.
  The tool prints the refused count per image, and a non-zero refused count is expected, not an error.

- [ ] **Step 4: Implement the dispatcher and fallback.** `AotImage::Resolve` is a sorted
  `{guest_off, thumb, entry}` table (binary search; a 1-entry inline cache per call site is Task 6). The
  dispatcher entry (`AotDispatch`) reads the block's `HaltReason`: `UserDefined1` (host call/syscall) →
  return to `CallGuestImpl` exactly as Tier J does; a target with no entry → call the existing Tier I
  executor for that block and continue. Reuse `JitExecutor::InterpreterFallback`'s shape
  (`core/exec/jit.cpp:920`) and keep the same `RunResult` fields.

- [ ] **Step 5: Make the fixture test pass, then assert the bytes run on the device.** The test is
  fixture- and AArch64-dependent, so it is device-only: it prints `# aot: not an AArch64 host — skipped`
  (or skips without the fixture) everywhere else, and the x86_64 host lanes must stay green. Build it for
  `arm64-v8a`, push, and run on the tablet — the translated `fib` must produce `6765` there too. Commit.

---

### Task 4: Indirect-target trace (`RB_CODEMAP`) from the tablet

**Files:**
- Create: `core/include/rb/exec/trace.h`, `core/exec/trace.cpp`
- Modify: `core/exec/jit.cpp` (`PreCodeReadHook` → `trace::OnBlockEntry`, `core/exec/jit.cpp:878`)
- Modify: `core/exec/interp.cpp` (the translate-miss path → `trace::OnBlockEntry`)
- Modify: `core/host/boot.cpp` (open the trace file from `RB_CODEMAP`)
- Modify: `core/CMakeLists.txt` (add `exec/trace.cpp` to `rb_exec`)
- Create: `core/tests/code_map_trace_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void trace::OnBlockEntry(std::uint32_t pc, bool thumb);` and an append-only file of
  `{u64 pc, u8 thumb, u8 source}` records, deduped in an `unordered_set`, written on close.

- [ ] **Step 1: Write the failing test.** Drive `InterpExecutor` over the fixture with the trace enabled
  (`RB_CODEMAP=<tmp>`), then assert the file contains the entry PC of `fib` and of its recursive call
  target, and that no record is duplicated. Run it: fails (no trace).
- [ ] **Step 2: Implement** the hook and the file writer (open in append mode, 1 MB buffer, flush on
  destruction). Call it from both tiers so a Tier-J run on the tablet records the same set.
- [ ] **Step 3: Collect a real trace.** On the tablet the launcher's process has no shell to inherit an
  environment from, so the path is also read from the `debug.retrobug.codemap` system property when
  `RB_CODEMAP` is unset (`adb shell setprop debug.retrobug.codemap /data/local/tmp/kitkat.rbcm`). The
  property is debuggable-build-only, which matches the launcher. Run KitKat with it set, play to the menu
  for two minutes, pull the file. Feed it to `rb_aotgen --trace=<file>` and print the added-start count.
  Expected: every indirect site observed has a target; report the numbers in the commit message.
- [ ] **Step 4: Commit**, including the test (device-only marker like the other fixture tests).

---

### Task 5: KitKat per-game IPA and the first measurement

**Files:**
- Create: `tools/aotgen/game_profile.{h,cpp}` (manifest: APK library hashes + code map + entry table name)
- Modify: `platform/ios/ios_session.cpp` (load a profile, verify hashes, choose AOT vs Tier I)
- Modify: `platform/ios/rb_ios.h` (report which engine the session booted: `rb_ios_session_engine`)
- Modify: `tools/build_ios_ipa.sh` (accept `RB_AOT_PROFILE=<dir>` and build the per-game app)
- Modify: `.github/workflows/host-and-ios.yml` (a `workflow_dispatch` input for the profile artifact)
- Docs: worklog entry with the M1 numbers

**Interfaces:**
- Consumes: Tasks 2–4 (`CodeMap`, `AotImage`, trace merge).
- Produces: a `GameProfile` JSON (`{name, librb: [{name, sha256, code_map}], entry_table}`) that the session
  loads from the app bundle; `rb_ios_session_engine()` returns 0 = Tier I, 1 = AOT, 2 = JIT.

- [ ] **Step 1:** `rb_aotgen --profile=kitkat --apk=<bootcheck.apk> --trace=<trace>` writes
  `build/aot/kitkat/{aot_kitkat.S, aot_kitkat_table.cpp, profile.json}` and prints translated
  blocks/functions and the text size. Test: the tool refuses an APK whose libraries do not match the trace's
  sonames, with a named error.
- [ ] **Step 2:** `ios_session` loads `profile.json` from the bundle, hashes the imported APK's
  `lib/**/*.so`, and only uses `AotImage` on a full match; log
  `aot: profile kitkat matches (3 libraries, N blocks)` or `aot: no profile for this APK — Tier I`.
- [ ] **Step 3:** Build `RetroBug-kitkat.ipa` via the workflow with the profile; install on the M1; run
  KitKat to the menu. Acceptance: `librbengine` log shows `aot:` lines, the LodePNG minutes drop to seconds,
  and the menu appears. Record wall-clock load time against build 20's >100 s.
- [ ] **Step 4:** Commit the workflow input, the profile plumbing, and the worklog numbers.

---

### Task 6: Blocky Roads, veneers and the last 1 %

**Files:**
- Modify: `tools/aotgen/emit.cpp` (veneer targets, TBB/TBH jump tables, `.plt` stubs)
- Modify: `tools/aotgen/code_map.cpp` (scan `add r12, pc` veneers into the map)
- Create: `core/tests/aot_blocky_test.cpp` (device, corpus-dependent)

- [ ] **Step 1:** Emit veneers as 12-byte translated blocks (their `add pc` is a normal PC-relative
  `Add32`), and `.plt` slot targets through the existing `HaltExecution`/host-call path (they are imports).
  **Pre-flight ruling:** the veneer scanner itself ships in Task 2 (pattern step 5 and its
  `kFromVeneer >= 1500` assertion); this task only fills gaps that the Task 5 measurement and kitkat trace
  expose, and must not re-implement it.
- [ ] **Step 2:** TBB/TBH: translate the jump-table read and route the computed target through the
  dispatcher (miss → Tier I). Test with a hand-assembled `tbb` program in `rb_interp_neon_tests`-style,
  host lane.
- [ ] **Step 3:** Blocky Roads' one open check — whether a `libgame.so` `svc` is `__ARM_NR_cacheflush`: run
  the device trace and grep the recorded syscall numbers; if yes, the fallback must own those pages
  (document in the spec). (KitKat's `libdlgl.so` has no cacheflush call in the trace; `libgame` is Blocky's
  library, and the plan's original wording named the wrong title.)
- [ ] **Step 4:** Build `RetroBug-blocky.ipa`, measure the 31 MB pack parse and first frame on the M1
  against build 20 (20.7 s on the earlier iPad), and commit the numbers.

---

## Self-review

- **Spec coverage:** discovery/coverage → Task 2; offline emission + relocations → Task 1; runtime dispatch +
  prelude + fallback → Task 3; indirect targets → Task 4; per-game IPA + hash match + measurement → Task 5;
  veneers/TBB/SMC check → Task 6. Packaging constraint (static link, never `dlopen`) is in Global
  Constraints and Task 5's build. The threaded Tier I engine is deliberately out of this plan (separate
  subsystem; it only changes the fallback's speed, not the AOT path).
- **Placeholders:** none — every step names files, functions and commands from the research.
- **Type consistency:** `CodeMap`/`CodeMapEntry`, `AotBlock`/`AotState`/`AotImage`, `GameProfile` and
  `rb_ios_session_engine` are used with the same names across tasks 2–6.
