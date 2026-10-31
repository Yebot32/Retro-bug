# patches/dynarmic/

RetroBug's changes to the vendored dynarmic fork (`external/dynarmic`, pinned per
`external/PROVENANCE.md`), one patch per change, oldest first. AD-14's rule and this directory's
rules (`../README.md`) apply: the tree stays otherwise byte-identical to the pinned commit, and
`diff -r` between the in-tree copy and upstream must be explicable entirely by the patches here.

Until 2026-10-03 this directory existed as a rule with no contents: 0001 lived only as an in-place
edit (commit `ce607df`), discovered by a provenance audit, and 0002 was written as an edit directly.
Both are captured here now; the re-import procedure's "port any RetroBug change that must survive
into `patches/`" step is what these files exist to make mechanical.

## How these are applied

Nothing applies them automatically. The paths inside each patch are rooted at `src/dynarmic/...`, so
they apply from inside `external/dynarmic`:

```
git -C external/dynarmic apply -p1 ../../patches/dynarmic/0002-<name>.patch
```

or from the repository root with `--directory=external/dynarmic`. Each patch is checked against the
tree with `git apply --check --reverse --directory=external/dynarmic <patch>`, which succeeds only
while the patch and the in-tree bytes still agree — the cheap version of AD-14's `diff -r` rule, and
the check that says whether a re-import would silently drop something.

## 0001-thumb32-acquire-exclusive-ldaex-decoder-rows.patch

**Why:** the Thumb-32 decoder table had `LDREX`/`LDREXB`/`LDREXH`/`LDREXD` but none of the
acquire forms (`LDAEX*`), while the ARM-state table (`decoder/arm.inc`) had all eight. bionic's
`pthread_mutex_lock`/`pthread_mutex_trylock` are Thumb, so every mutex operation in a real guest
libc reached these four encodings and the front end declined them — measured as the wall in
`docs/30` §Step 40-46 bring-up.

**Retire when:** the pinned upstream grows the same four rows (compare `thumb32.inc` at re-import).

## 0002-arm64-callback-only-memory-abort-check.patch

**Why:** the AArch64 back end honours `UserConfig::check_halt_on_memory_access` on the fastmem and
inline-page-table emit paths, both of which end with `emit_check_memory_abort` — a check of the
`MemoryAbort` halt reason that restores PC to the faulting instruction and leaves the block. The
callback-only path (what an embedder without `fastmem_pointer` or a `page_table` gets, which is
RetroBug's configuration) emitted the memory callback call and nothing after it: a faulting access
raised `MemoryAbort`, and the rest of the block still executed with zero-filled loads and a PC that
only surfaced the fault at the next halt-tested point. This patch appends the same abort check to
the four `CallbackOnlyEmit*Memory` emitters, gated on the flag the other paths already consult, so
a config that asks "halt after every data access" gets it on every data-access path.

**Retire when:** upstream routes the callback-only paths through `emit_check_memory_abort` the way
it does the fastmem paths (or when RetroBug supplies a page table and the inline paths serve every
access).

## 0003-arm64-coprocessor-callback-halt-check.patch

**Why:** the same gap as 0002, on the other host callback a guest can reach. `CallCoprocCallback`
emitted the call and nothing after it, so a coprocessor access RetroBug refuses — anything `p15`
other than the thread-pointer register (`core/exec/jit.cpp`'s `Arm15Coprocessor::Refuse`) — recorded
the refusal and raised `HaltReason::UserDefined1`, and the block then ran on with the callback's
zero in the destination register. Tier I stops at the instruction, so the two tiers also disagreed
about where the same bytes stop. The patch adds `EmitA32CheckCallbackHalt` (a `UserDefined1` check
that restores the containing block's PC and leaves the block) and calls it after the callback, gated
on the same `check_halt_on_memory_access` flag 0002 uses. It is a separate patch from 0002 because it
touches the coprocessor emitter rather than the memory ones, and because it can be retired
independently.

**Retire when:** upstream gives the coprocessor callback path a halt re-test of its own, or when
RetroBug stops registering a coprocessor that can refuse (neither is true today: `conf.coprocessors[15]`
is set, and `EmitCoprocessorException` is an unconditional `ASSERT_FALSE` here, so a refusal path is
required).

## 0004-arm64-fpscr-write-installs-fpcr.patch

**Why:** the prelude installs the host `FPCR` once per `Run()` from the JIT state's
`upper_location_descriptor` (`a32_address_space.cpp:252-256`, `:290-294`), and the block emitters that
compare `rounding_mode == ctx.FPCR().RMode()` rely on that invariant to skip their own `MSR`
(`emit_arm64_floating_point.cpp:170-182`). A guest `vmsr fpscr` updates the descriptor at runtime
(`A32SetFpscr` already rewrites it) but left the host register alone, so every arithmetic operation and
every conversion after it kept rounding the *Run-entry* way until the embedder called `SetFpscr` again.
On hardware `vmsr` takes effect for the next instruction. Measured rather than deduced: the corpus's
`fp_rmode` probe sets RMode, divides in double and narrows to float, and Tier J answered with the
round-to-nearest value on the `+inf`, `-inf` and `zero` rows where the SM-X710 did not. The patch writes
the new mode into `FPCR` at the point the descriptor changes; the upper half of the descriptor *is* the
FPCR image, which is what the prelude already relies on when it writes the same `AND` to the register.

**Retire when:** upstream installs `FPCR` where the descriptor changes rather than only at `Run` entry —
or when a guest can be stopped from writing FPSCR mid-run, which is not something an emulator can
decline.

## 0005-thumb32-load-acquire-byte-half-rows.patch

**Why:** the same gap as 0001, one instruction family over. The Thumb decoder table had the
acquire/release *word* rows (`LDA`/`STL`) but none of the byte or halfword forms (`LDAB`/`LDAH`/
`STLB`/`STLH`), while the ARM-state table carries all six (`arm.inc:111-131`). The device's own
bionic reaches `ldab` in `__libc_safe_arc4random_buf` — the AT_RANDOM fallback of the setjmp cookie,
where /dev/urandom is a refused path — and the run stopped there with `UndefinedInstruction` the
moment the auxv fix let libc's constructor get that far (measured on the SM-X710, 2026-10-09).
Note the decoder tables do not consult `ArchVersion`: widening the accepted architecture to v8 was
tried against this wall first and changed nothing, because the missing *rows* were the wall. The
implementations mirror `thumb32_LDA`/`thumb32_STL` with the byte/half sizes and keep
`IR::AccType::ORDERED`, which is the acquire/release semantics the forms exist for. Retire together
with 0001 if the pinned upstream grows one of the two.

**Retire when:** the pinned upstream grows these four rows (compare `thumb32.inc` at re-import).

## 0006-thumb32-store-release-exclusive-stlex-rows.patch

**Why:** the third and last missing member of the same Thumb group. 0001 added the load-acquire-
**exclusive** rows, 0005 the plain acquire/release byte/half rows, and this adds the
store-release-**exclusive** family (`STLEX`/`STLEXB`/`STLEXH`/`STLEXD`) — again present in the
ARM-state table (arm.inc:112-128) and absent from the Thumb one. Measured as the wall immediately
after 0005 cleared `ldab`: libc's `__cxa_guard_release`, which every C++ static initialization in the
image reaches through `__libc_safe_arc4random_buf`'s once-init, spins on `ldrex` + `stlex`
(`e8c4 0fe2`, SM-X710, 2026-10-09). `ORDERED` is the whole difference from `STREX`, per the
LDAEX/STLEX pairing; the doubleword form takes an explicit Rt2 register (`uuuu`), unlike the ARM
encoding's implicit `t + 1`. Retire together with 0001 and 0005 if upstream grows the set.

**Retire when:** the pinned upstream grows these four rows (compare `thumb32.inc` at re-import).

## 0007-ir-block-64-instruction-slab.patch

**Why:** `IR::Block`'s constructor (`ir/basic_block.cpp:25`) hands its instruction pool a fixed slab of
4096 slots, and `sizeof(IR::Inst)` is 104 B, so every block reserved 104 × 4096 = 425,984 B (~416 KiB)
at construction whether it went on to hold ten instructions or four thousand. That number is not an
upstream problem — the JIT emits one block and releases it, inside a code-cache budget — but Tier I
(`rb::exec::InterpExecutor`) caches decoded `IR::Block`s for the life of the executor, so the cost is
paid per *cached* block. It is the reason `kBlockCacheLimit` was 128: 128 × 416 KiB is ~54 MiB already,
and the 8192 it replaced would have reserved ~3.2 GiB. A cache that small is cleared wholesale when
full (`core/exec/interp.cpp:3215`), so an interpreted title whose working set exceeds 128 blocks
re-translates its hot blocks continuously. This patch changes the one constant to 64 slots (~6.7 KiB)
and nothing else. The growth path is untouched and already correct for a smaller initial slab: `Pool`
keeps `initial_pool_size` as `slab_size`, and `Alloc` pushes the current slab onto `slabs` and calls
`AllocateNewSlab` — the same `object_size × slab_size` bytes, malloc'd and freed normally — when
`remaining` reaches zero (`common/memory_pool.cpp:12-42`), so a block longer than 64 instructions pays
additional 64-instruction slabs rather than failing or being truncated. The only cost is a few more
mallocs per long block at translation time. With it, 2048 cached blocks cost ~13 MiB before any block
outgrows one slab, which is what lets `kBlockCacheLimit` rise from 128 to 2048
(`core/include/rb/exec/interp.h`).

**Retire when:** upstream sizes the initial slab per block or grows the pool geometrically (making the
constant's value a non-issue), or when Tier I stops keeping whole `IR::Block`s alive across entries and
the per-block reservation is no longer multiplied by a cache (compare `basic_block.cpp`'s `Block`
constructor at re-import).

## 0008-aot-offline-emission.patch

**Why:** RetroBug's AOT path has a host tool translate a guest's code ahead of time, offline, so the
device never interprets or JITs it. The tool needs the JIT's own translation and emission pipeline — a
copy would drift — but the emission half is unreachable from outside: `AddressSpace::Emit`/`Link` are
protected, and `EmitArm64` needs the address space's code cache and block maps, none of which an offline
emitter may touch. IR generation needs nothing: `AddressSpace::GenerateIR` is a public pure virtual over
a descriptor, is unchanged by this patch, and is what the tool calls, so it gets the exact
`A32::Translate` plus optimisation pipeline rather than a re-derivation. This patch adds exactly one
public entry point and no logic: `AddressSpace::EmitForAot(IR::Block&, oaknut::CodeGenerator&)`
calls `EmitArm64` with the same `GetEmitConfig()` the JIT builds (`a32_address_space.cpp`), writing into
a caller-owned code generator and returning the `EmittedBlockInfo` relocations for the caller to link.
Nothing in `Emit`, `Link`, `EmitPrelude`, or the emitters changes, so every byte the JIT emits is
unchanged. The tool refuses blocks that bake a host address: the coprocessor callbacks and the vector-FP
fallback emit absolute `MOV`s (`emit_arm64_a32_coprocessor.cpp`,
`emit_arm64_vector_floating_point.cpp:271`) with no relocation record, and those blocks stay on Tier I
rather than being made relocatable here.

**Retire when:** upstream exposes an offline emission API — IR generation and block emission into a
caller-owned buffer, with relocation records, reachable without an `AddressSpace` code cache or its block
maps (compare `address_space.h`'s `EmitForAot` at re-import).
