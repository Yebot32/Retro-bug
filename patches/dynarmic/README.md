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
