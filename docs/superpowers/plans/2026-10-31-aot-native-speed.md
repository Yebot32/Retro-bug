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
- Produces: `Dynarmic::Backend::Arm64::EmitArm64(CodeGenerator&, IR::Block&, const EmitConfig&, FastmemManager&)`
  and its `EmittedBlockInfo{entry_point, size, relocations, block_relocations, fastmem_patch_info}`, both
  already present at `emit_arm64.h:101-107,166`; the patch only makes them reachable from outside the
  address space and makes coprocessor callbacks relocatable.

- [ ] **Step 1: Read the coupling points.** Open `external/dynarmic/src/dynarmic/backend/arm64/emit_arm64.h`
  (the `EmitConfig` fields and `EmitArm64`'s signature), `a32_address_space.cpp:23-72,193-357` (how the
  config is built and how the prelude bakes callbacks), and `emit_arm64_a32_coprocessor.cpp:29-34` (the
  absolute `MOV Xscratch0, fn` that has no relocation record). Write down the exact `EmitConfig` field
  values the address space uses — the AOT harness must use the same ones.

- [ ] **Step 2: Expose the emission entry point.** In `address_space.h`, add next to the protected
  `Emit`/`block_infos`:

```cpp
public:
    /// AOT (RetroBug patch 0008): translate one block into `code` without touching the code cache, and
    /// return its size and relocation records. The caller owns the buffer and the linking step; the JIT
    /// path is unchanged and still uses Emit()/Link() below.
    EmittedBlockInfo EmitForAot(IR::LocationDescriptor descriptor, IR::Block& ir_block, CodeGenerator& code);
```

  and in `address_space.cpp` (definition beside `Emit`):

```cpp
EmittedBlockInfo AddressSpace::EmitForAot(IR::LocationDescriptor descriptor, IR::Block& ir_block,
                                          CodeGenerator& code) {
    const EmitConfig emit_config = GetEmitConfig();
    return EmitArm64(code, ir_block, emit_config, fastmem_manager);
}
```

  (`GetEmitConfig()` already exists in `a32_address_space.cpp`; if it is private there, widen it in the same
  patch. Verify by compiling.)

- [ ] **Step 3: Make coprocessor calls relocatable.** In `emit_arm64_a32_coprocessor.cpp`, replace the
  absolute `MOV Xscratch0, reinterpret_cast<u64>(fn)` with a literal-pool load that `Link` (JIT) and the
  AOT linker both patch:

```cpp
// RetroBug patch 0008: the callback address was baked as an absolute immediate, which made a block's bytes
// position-dependent with no relocation record. A literal load is what the block linker can rewrite.
const auto pointer = code.MOVP(Xscratch0, reinterpret_cast<u64>(callback.data));
relocations.emplace_back(pointer.offset(), LinkTarget{&callback.data});
```

  Use the emitter's existing literal helper (`MOVP`/`LDR literal`) and the same `LinkTarget` machinery the
  prelude uses; verify `git grep -n "LinkTarget{" external/dynarmic/src/dynarmic/backend/arm64` for the
  exact type.

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
```

- [ ] **Step 1: Write the failing test.** In `tools/aotgen/code_map_test.cpp`, load the two real libraries
  from `build/kk-inspect/lib/armeabi/libdlgl.so` and `build/br-inspect/lib/armeabi-v7a/libgame.so` (skip with
  a printed line when absent) and assert the measured lower bounds from the research:

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

  Run it (`cmake --build build/host --target rb_aotgen_tests && ./...`) and expect failure: the symbols do
  not exist yet.

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

- [ ] **Step 4: Wire the target and commit.** Add `add_executable(rb_aotgen main.cpp code_map.cpp)` and
  `add_executable(rb_aotgen_tests code_map_test.cpp code_map.cpp)` in `tools/aotgen/CMakeLists.txt`, and in
  the top-level `CMakeLists.txt` after `project(...)`:

```cmake
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tools/aotgen)
endif()
```

  Build and run the test locally via the existing host test lane; commit.

---

### Task 3: AOT runtime — prelude, dispatcher, fixture `fib` end to end

**Files:**
- Create: `core/include/rb/exec/aot.h`, `core/exec/aot.cpp`
- Create: `tools/aotgen/emit.cpp` (block emission + symbolic linking)
- Create: `core/tests/aot_test.cpp`, `core/tests/aot_fixture_table.cpp` (generated by the tool during the build)
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

- [ ] **Step 2: Generate the prelude.** In `core/exec/aot.cpp`, copy Dynarmic's `EmitPrelude`
  (`a32_address_space.cpp:225-357`) verbatim into a function that takes the same `EmitConfig` the JIT uses
  and emits: the state pointer loads, `Xpagetable` (leave 0 — the AOT path uses the callbacks only),
  `Xhalt` and the tick accounting the backend expects. Add `static_assert`s for every JitState offset the
  prelude reads against `AotState` (`offsetof`), citing `a32_jitstate.h:17-43`. Do not hand-edit the
  register mapping: if an offset differs, fix the struct.

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

- [ ] **Step 4: Implement the dispatcher and fallback.** `AotImage::Resolve` is a sorted
  `{guest_off, thumb, entry}` table (binary search; a 1-entry inline cache per call site is Task 6). The
  dispatcher entry (`AotDispatch`) reads the block's `HaltReason`: `UserDefined1` (host call/syscall) →
  return to `CallGuestImpl` exactly as Tier J does; a target with no entry → call the existing Tier I
  executor for that block and continue. Reuse `JitExecutor::InterpreterFallback`'s shape
  (`core/exec/jit.cpp:920`) and keep the same `RunResult` fields.

- [ ] **Step 5: Make the fixture test pass, then assert the bytes run on the device.** Build the test on the
  host lane and run it. Then build it for `arm64-v8a`, push, and run on the tablet — the translated `fib`
  must produce `6765` there too. Commit.

---

### Task 4: Indirect-target trace (`RB_CODEMAP`) from the tablet

**Files:**
- Create: `core/include/rb/exec/trace.h`, `core/exec/trace.cpp`
- Modify: `core/exec/jit.cpp` (`PreCodeReadHook` → `trace::OnBlockEntry`, `core/exec/jit.cpp:878`)
- Modify: `core/exec/interp.cpp` (the translate-miss path → `trace::OnBlockEntry`)
- Modify: `core/host/boot.cpp` (open the trace file from `RB_CODEMAP`)
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
- [ ] **Step 3: Collect a real trace.** On the tablet, run KitKat through the Android launcher with
  `RB_CODEMAP=/data/local/tmp/kitkat.rbcm`, play to the menu for two minutes, pull the file. Feed it to
  `rb_aotgen --trace=<file>` and print the added-start count. Expected: every indirect site observed has a
  target; report the numbers in the commit message.
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
- [ ] **Step 2:** TBB/TBH: translate the jump-table read and route the computed target through the
  dispatcher (miss → Tier I). Test with a hand-assembled `tbb` program in `rb_interp_neon_tests`-style,
  host lane.
- [ ] **Step 3:** KitKat's one open check — whether a libgame `svc` is `__ARM_NR_cacheflush`: run the
  device trace and grep the recorded syscall numbers; if yes, the fallback must own those pages (document in
  the spec).
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
