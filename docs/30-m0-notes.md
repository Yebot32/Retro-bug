# M0 implementation notes

Running log of what was found while executing the [M0 breakdown](20-implementation-plan.md) §9. Things go
here when they invalidate a plan assumption or settle an open risk, so the plan and `docs/10` §17 stay
accurate rather than aspirational.

## Step 1 — vendoring **(done, `419fe9f`)**

Done as specified in AD-14: `external/dynarmic` at `e77b1ba`, nine submodules flattened, nested `.git`
removed, every licence swept into `external/PROVENANCE.md` and `REUSE.toml`. All permissive, none GPL.

**U-6 closed as a side effect.** Both `RbCodeCache` backends already exist upstream under MIT
(`oaknut/code_block.hpp` anon-mmap forms, `oaknut/dual_code_block.hpp` `memfd_create` + dual `MAP_SHARED`
RW/RX). Only `Interpreted` is new work. See `docs/10` §8.3.

## Step 2 — first build **(blocked on a dependency decision, diagnosed to the line)**

Toolchain facts established on this machine, worth not rediscovering:

- The NDK CMake toolchain file works; `ANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-26` configures
  past compiler detection.
- `cmake` and `ninja` both come from the SDK and are **not on PATH**; `-G Ninja` fails with "unable to
  find a build program" unless `-DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.31.6/bin/ninja.exe` is passed
  explicitly. Same for `cmake.exe` itself, and for `android.jar`, which is under `platforms/` not
  `build-tools/`.

**Blocker: `external/dynarmic/CMakeLists.txt:151` — `find_package(Boost 1.57 REQUIRED)`.** This was not in
any Phase 0 document, so it is not in the licence table either. Ten files use it, for two things, and
neither is incidental:

| Boost use | Where | Why RetroBug cares |
|---|---|---|
| `boost::icl::interval_set<u32>` + `discrete_interval::closed` | `backend/arm64/a32_interface.cpp:71,162`, `a32_address_space.{h,cpp}`, and the x64 equivalent via `backend/block_range_information.{h,cpp}` | **This is the self-modifying-code invalidation tracking** — the mechanism AD-7/C10 chose instead of `userfaultfd`, because it is the one that also works on iOS. It is load-bearing for correctness of any guest that JITs or self-modifies |
| `boost::variant` | `ir/terminal.h`, `common/variant_util.h`, `ir/opt/a64_merge_interpret_blocks.cpp` | IR block terminal kinds — every tier reads this |

Boost is BSL-1.0, so this is a build-weight and maintenance question, **not** a licensing problem.

### The two ways forward

**A. Supply Boost.** Vendor a header-only subset (or fetch it in CI). Unblocks the build fastest.
Cost: `boost::icl` fans out transitively across a large part of Boost (`concept_check`, `mpl`,
`preprocessor`, `rational`, `container_hash`, `numeric/traits`, …), so a hand-picked subset is fragile —
in practice this means carrying most of a Boost tree as a build dependency for the rest of the project.

**B. Patch Boost out.** `boost::variant` → `std::variant` is mechanical. The interval container is roughly
one `patches/dynarmic/0001-*.patch` of ~150–250 lines with its own unit tests: `interval_set` here needs
only insert-with-merge, iterate, and containment — a sorted `std::vector<pair<u32,u32>>` with overlap
coalescing. Cost: a day, plus the responsibility for a data structure on the SMC path — which is also the
argument for owning it, since we would otherwise be trusting our correctness story to a container we
cannot audit quickly.

**Resolution (step 2 done).** The owner chose "patch Boost out", and the implemented shape turned out to be
better than either option above: instead of editing dynarmic at all, `compat/` provides the eight Boost
names as headers on the include path, and `cmake/FindBoost.cmake` answers `find_package(Boost REQUIRED)`
with an `IMPORTED` `Boost::boost` target pointing there. So **the vendored tree stays genuinely
byte-identical** — no patch files to rebase — and `-DRB_USE_REAL_BOOST=ON` swaps back to real Boost with
one flag. `external/dynarmic` builds clean for x86_64 Android: **114 MB `libdynarmic.a`**, A32 front end
and x64 backend objects present, no Boost installed.

Three things the shim needed that are worth knowing before touching it again:

1. `boost::recursive_wrapper<T>` cannot alias to `T`. dynarmic's `ir/terminal.h` declares the `Terminal`
   variant while `If`/`CheckBit`/`CheckHalt` are only forward-declared, and `std::variant` computes its
   traits at that point. The shim holds a `unique_ptr<T>` and converts to `const T&` on use, exactly the
   trick Boost uses.
2. `boost::variant` needs to be a *derived* type, not an alias, because dynarmic calls the member
   `which()`. Consequence: `std::visit` cannot deduce through a derived type, so `apply_visitor`
   down-casts explicitly.
3. Real Boost headers drag in `<map>`, `<set>`, `<tuple>`, `<array>`, `<functional>`, `<vector>`, and
   dynarmic's backend headers rely on that without including them. Without the same transitive surface the
   error is "too few template arguments for class template 'map'" in an unrelated header — confusing at a
   distance, so the includes are there deliberately and commented as such.

**Step 2 tests: green.** `core/tests/interval_compat_test.cpp` runs on the local x86_64 emulator —
**33 checks, 0 failures** — after the second pass fixed two real defects the tests found:

1. `insert()` never created a segment for the part of a new interval that lay past or between existing
   segments, so `[10,20]∪[15,25]` produced two segments instead of three and an invalidation at `[23,23]`
   reported nothing.
2. Worse, out-of-order registration broke the **sorted invariant** that `equal_range` binary-searches
   over: adding `[1000,1099]` after `[3000,3099]` appended it out of position, so queries silently
   under-reported. Both failure modes are stale translated code executing unnoticed, not a crash — which is
   the argument for these tests existing before any guest runs.

A third test (`map_stays_queryable_when_blocks_are_registered_out_of_order`) was added specifically to
pin defect 2, since blocks are registered in translation order, not address order.

**Also settled by booting the emulator: it is a 16 KiB-page Android.** `getconf PAGESIZE` → `16384`, and
`abilist32` is empty there too. So AD-7's 4 KiB-guest / 16 KiB-host mixed-granule path **is** testable
locally, not only on a future Android 16 device — see the correction in `docs/21` §1.

Practical notes for re-running, both of which cost a cycle:
- `adb push` **clears the execute bit**; `chmod 755` after every push.
- Under Git Bash a device path like `/data/local/tmp/x` is rewritten to `C:/Program Files/Git/data/...`;
  set `MSYS_NO_PATHCONV=1` for every `adb push`/`pull` with an absolute device path.

## Step 3 — coverage audit, demand half **(done: `docs/32-isa-coverage.md`)**

`tools/isa_coverage.sh` measures what real ARMv7 Android code actually demands, from the NDK's own
AArch32 archives: **711 objects, 338,034 instructions, 855 distinct mnemonics**. Three results that
change assumptions rather than merely confirming them:

- **8,387 IT-block instructions** in bionic itself — the IT-block risk named in `docs/research/01` is now
  backed by demand, not just subtlety, so it leads the differential queue.
- Hundreds of `vadd.f64`/`vmul.f64`/`vcmp.f64` — libm's double-precision paths mean **VFPv3 D32 is
  mandatory**; a single-precision-scoped engine fails here.
- `movw` 3,284 and `cbz` 3,271 — the concrete ARMv7-only forms the ARMv6-vs-ARMv7 question hinged on.

Two self-corrections are recorded in that doc, because both would otherwise be re-cited as evidence: the
§4.1 "file listing proves coverage" argument was the wrong kind of proof (dispatched ≠ correct), and a
`UndefinedInstruction()` I initially flagged as a NEON gap turned out to be the architecturally-correct
rejection of undefined `VST4/VLD4 .64` encodings. Grep-found gaps have to be read before being reported.

**Still open in U-8:** semantic correctness. Next is Dynarmic's own `test_generator`/`test_reader` lanes on
the emulator, with two named constraints — its reference is GPL Unicorn (test-side only, never linked), and
three cases the harness itself excludes because *Unicorn* is wrong need a different oracle.

## Step 3b — engine test lane green, and U-2 has a first answer

**Dynarmic's own suite runs here, GPL-free.** `DYNARMIC_TESTS_USE_UNICORN` defaults to **OFF**, so
`dynarmic_tests` builds and runs with no Unicorn involved: **200,561 assertions in 48 test cases, all
passing** on the x86_64 emulator, including `A32/test_svc.cpp` (guest kernel entry) and
`A32/test_coprocessor.cpp` (CP15/TLS). Reproduce with `tools/run_engine_tests.sh`. Two caveats kept
attached to that result, because "green" is easy to over-read: the build exercises the **x64** backend for
A32, so the AArch64 backend that real arm64 devices use is still untested, and a suite asserts what its
author chose to assert — Dynarmic's own README still admits FPSR state is approximate.

**U-2, first data point** (`tools/va_probe.c`, x86_64 Android emulator, `shell` domain):

```
pagesize=16384  sizeof(void*)=8
anon PROT_NONE 4GiB, any address                        ok      addr=0x7eeeb3e00000
anon PROT_NONE 4GiB at hint 0x10000 (MAP_FIXED_NOREPLACE) ok    addr=0x10000
anon PROT_NONE 4GiB at hint 0 (MAP_FIXED_NOREPLACE)     FAILED  errno=1 (EPERM)
anon PROT_NONE 4GiB at hint 0x80000000                  ok      addr=0x80000000
anon RWX (single-mapping JIT)                           ok
```

So a contiguous low 4 GiB window **can** be reserved, which is what fastmem needs — but it cannot start at
address 0, only at `0x10000`, because the kernel refuses to map page zero. That looks like a constraint on
an identity-mapped model and is actually a match for it: a real 32-bit Android has the same
`mmap_min_addr` rule, so guest page 0 is unmapped there too and a guest NULL dereference faults. Biasing
the window base to `0x10000` therefore reproduces guest behaviour rather than compromising it. Still,
`GUEST_BASE` must stay a variable and not a literal, per AD-6.

Caveat carried forward: these are kernel VA answers from the `shell` domain, so they transfer; the
**executable-memory** answer does not necessarily transfer, since `execmem` is decided per SELinux domain.
The authoritative app-domain result has to come from inside an installed RetroBug build (U-13).

## Step 4 — `rb::mem` (C03): green, with one asymmetry worth knowing

`core/mem/guest_memory.cpp` implements the window model, and `core/tests/guest_memory_test.cpp` covers it:
**94 checks, 0 failures, 1 skipped** on the x86_64 emulator (page shift 14), alongside the interval suite's
33/33. The mixed-granule behaviour AD-7 specified is now real and tested: two adjacent guest pages with
different protections inside one host granule leave fastmem and are served out-of-line, rather than the
model granting the union of their permissions — which would have quietly permitted guest writes to an RX
page and destroyed both the W^X story and self-modifying-code detection.

**The asymmetry: `host_page_shift` can be injected coarser than the kernel's page size, but never
finer.** mmap/mprotect granularity is enforced by the kernel, so asking for 4 KiB on a 16 KiB host is
`EINVAL`, not a simulation. The model now refuses that at `Create` with an explanatory error instead of
silently mis-simulating. Consequences, one good and one to correct elsewhere:

- **Good, and it changes the device plan:** a 4 KiB host can test *both* granules — native 4 KiB plus an
  injected 16 KiB grouping, which is exactly the mixed case. The **OnePlus 12 is therefore the better
  machine for granule testing**, not the worse; `docs/21` had listed 16 KiB coverage as something it
  could not provide.
- To correct: the earlier note in this file framing the emulator as making the 16 KiB path testable
  "rather than requiring a second device" is only half right — it tests the 16 KiB case natively and
  *loses* the 4 KiB one. Both cases together need one of each, and the suite now says so explicitly via
  `SKIP` lines rather than passing quietly.

Three defects the tests caught before this was committed, all worth remembering because two of them were
silent rather than fatal:

1. **Out-of-line migration read a page after withdrawing its permissions.** `mprotect(granule,
   PROT_NONE)` ran before the snapshot `memcpy`, so the copy faulted. Caught as a SIGSEGV, and the crash
   was only localisable after adding per-test progress markers, because on Android the crashing
   process's own stdout is lost.
2. **Fresh-page zeroing ran after the final protection was applied**, so mapping a guest page RX
   faulted on its own memset. Now the granule is mapped writable, zeroed, then dropped to its final
   permissions.
3. **`Host()` ignored the intra-page offset** and returned the page base. This one is the dangerous
   class: it returned a valid-looking pointer, so the first wrong read came from a *different* address
   than requested rather than from any error at the call.

Also fixed while here: the destructor unmapped from the aligned window base with a rounded-up length,
which could have run past the reservation and released unrelated host memory; the original reservation is
now tracked and released as a whole.

## Step 5 — `rb::loader` (C05): implemented and compiling; **execution not yet verified**

`core/loader/guest_elf.cpp` loads a real armeabi-v7a shared object into `rb::mem`: program headers,
per-segment file mapping at the correct residual offset (`p_offset - (p_vaddr & ~PAGE_MASK)`),
`R_ARM_RELATIVE` / `ABS32` / `GLOB_DAT` / `JUMP_SLOT` application, import refusal when a symbol cannot
be bound, final segment protections then `PT_GNU_RELRO` stripping, export/import tables, `DT_NEEDED`,
and an optional `PT_NOTE` build-id. `core/tests/guest_elf_test.cpp` asserts all of it.

**Status, stated precisely: the loader and its tests build, but they have not been executed.** The
emulator stopped being able to start partway through this step (see the acceleration note below), and
these tests need a running Android guest to load an image into. Everything claimed green earlier in
this file was actually run; nothing claimed here was.

**The test derives its expectations from the fixture instead of hard-coding them.** The first draft
asserted offsets taken from an earlier spike `.so`; rebuilding the fixture as `tests/corpus/fixture_jni.c`
moved every one of them, and `__register_atfork` appeared as a new import purely because the compiler
decided to register an atfork handler. Any address baked in would have made the test fail loudly and
meaninglessly, or worse, pass against a stale image. It now parses the fixture's own program headers,
`.dynamic`, `.rel.dyn`, `.rel.plt` and symbol table, and compares each against what the loader wrote.
The ELF structures it needs are shared with the loader via `core/include/rb/loader/elf_defs.h`.

Two bugs caught by writing the shared definitions properly rather than trusting recall:

1. **`Elf32_Sym.st_shndx` is 16-bit, not 32.** A `static_assert` on `sizeof(Sym32) == 16` fired
   immediately; a 20-byte symbol entry would have walked the symbol table at the wrong stride and
   read garbage names from the third entry onward, with no crash.
2. **ARM relocation numbers.** Checked against the fixture's real `r_info` values:
   `R_ARM_RELATIVE` is **23** (0x17) and `R_ARM_JUMP_SLOT` is **22** (0x16). The first draft had
   `RELATIVE = 0x11` and `JUMP_SLOT = 0x17` — a swap that would have applied load-bias fixes to GOT
   slots and vice versa, producing plausible pointers instead of an error. The values are now
   commented with where they were verified.

**Unverifiable-by-design point worth recording:** the loader's `LoadFromBytes` goes through
`memfd_create`, because guest images arrive as bytes or asset fds and the isolated environment hands
the same descriptors over Binder — and because code executed *from a file in app storage is forbidden*
(docs/10 §7). It is called via `syscall(SYS_memfd_create, ...)` rather than the wrapper, since
`memfd_create()` is only declared from API 30 and the core builds against android-26.

**One real bug the test found by being written properly:** a load that failed partway left its
already-mapped segments in place, because `LoadFromFd` returned early without unwinding. The test
asserts `mapped_page_count() == 0` after a refused load, which is the behaviour that matters — a
rejected image's leftover pages get picked up by the next load's free-space scan and surface as
corruption somewhere unrelated to their cause. `LoadFromFd` now carries an RAII rollback that unmaps
every segment it mapped unless the load commits.

### The emulator lost its acceleration mid-step — cause identified

`emulator.exe -accel-check` reported **"AEHD (version 2.2) is installed and usable"** at the start of
this session, and the AVD booted twice successfully (the step 3 and step 4 test runs above). It then
reported **"Android Emulator hypervisor driver is not installed on this machine"** and refused to
start the x86_64 AVD.

**That message is wrong, and the distinction matters.** The driver is installed; the service is only
stopped:

```
Name  Status   StartType
aehd  Stopped  System
```

with `aehd.sys`, `aehd.inf` and `aehd.cat` present under
`%LOCALAPPDATA%/Android/Sdk/extras/google/Android_Emulator_Hypervisor_Driver`. Stopping the emulator
tore down a driver that was working, and the recovery is to start it again rather than reinstall
anything:

```
net start aehd          # requires an elevated shell
```

Elevation puts this outside what a build script — or an agent — should do unprompted, so the step 5
tests below are recorded as built and not run, and `tools/run_engine_tests.sh` stays the single command
that turns them into verified. `-accel off` was tried as a workaround and did not bring the image up
within 15 minutes, as expected for an x86_64 guest on an x86_64 host with no hypervisor.

Also fixed in the runner: `[ -x "$BIN" ]` is the wrong test on Windows for an extension-less ELF
output — the linker produced an 85 MB `dynarmic_tests` that `-x` called non-executable. It is now `-f`.

## Step 5b — arm64 host build: the Boost shim does not cover Dynarmic's AArch64 backend

Trying to build the suite for `arm64-v8a` — which is the whole point of that ABI, since it is the only
way to exercise Dynarmic's **AArch64** A32 backend — failed on `compat/`:

```
backend/arm64/emit_arm64_a32.cpp:153: error: call to 'EmitA32Terminal' is ambiguous
   boost::apply_visitor([&](const auto& t) { EmitA32Terminal(code, ctx, t, ...); }, terminal);
```

same at `emit_arm64_a64.cpp:136`. The x86_64 build never showed this because its x64 backend does not
use that overload pattern.

Cause, confirmed by reading the call site: Dynarmic declares `EmitA32Terminal` overloads taking each
terminal **by value** *and* one taking `Terminal` (the variant). For a wrapper alternative there are then
two equally-ranked user-defined conversions — `recursive_wrapper<If>` → `const If&` for the by-value
overload, and `recursive_wrapper<If>` → `Terminal` through the inherited converting constructor — so
resolution cannot choose. Removing the non-const `operator T&()` did **not** fix it, which is worth
recording because it was the obvious guess and it was wrong.

**Status: unresolved, and it is a real limitation of `compat/`, not of any device.** Options, none of
them cheap or silent: give the wrapper a conversion set that makes the variant path non-viable; make
`variant`'s inherited converting constructor reject wrapper sources; or use real Boost headers for the
AArch64 lane only. Deciding this needs a proper look, so it is written down rather than patched blind.

> **Resolved in Step 5f, and none of the three options above was the cause.** All three assume the
> conversion set is the problem; it is not. `apply_visitor` was forwarding the `recursive_wrapper` to
> the visitor, which is what Boost does *not* do. Left as written above because the diagnosis here was
> the state of knowledge at this point in the sequence.

It does not block the tests that matter today. **`rb::mem` and `rb::loader` have no engine dependency**
(docs/20 §3 lists their dependencies as none), so `core/CMakeLists.txt` is split into `rb_mem`,
`rb_loader`, and an aggregate `rb_core`, and the memory and loader suites now build and run on a target
without compiling the DBT at all. All three binaries cross-build clean for `arm64-v8a`:
`rb_guest_elf_tests`, `rb_guest_memory_tests`, `rb_interval_compat_tests`.

Two side effects of the split worth keeping: a component that does not need the engine can no longer
accidentally pull it in through a header, and an engine-side breakage can no longer hide the fact that
the memory model and loader were never exercised. The arm64 attempt also caught a config bug of mine —
`foreach(t RB_MEM RB_LOADER)` named nonexistent targets and broke the whole configure.

### Device facts from the OnePlus 12 (`CPH2573`, Android 16 / SDK 36), read directly

| Property | Value | Why it matters |
|---|---|---|
| `ro.product.cpu.abilist` | `arm64-v8a` | confirms the owner's statement |
| `ro.product.cpu.abilist32` | **empty** | no native route for an `armeabi-v7a` APK → M1's E1 negative control is sound |
| `getconf PAGESIZE` | **4096** | the mixed-granule cases the 16 KiB emulator must `SKIP` run natively here, and an injected coarser granule is still legal |
| cores / RAM | 8, ~15.2 GB (`MemAvailable` ~4.4 GB) | a 4 GiB `PROT_NONE` `MAP_NORESERVE` reservation is unremarkable |

So this single device covers **both** page-granule cases, which the emulator could not — reinforcing the
`docs/30` §Step 4 finding that granule simulation can go coarser than the kernel's page size but never
finer.

## Step 5c — first real device run (Samsung Tab S9, SM-X710, Android 16)

Device read directly: `abilist=arm64-v8a,armeabi-v7a,armeabi`, **`abilist32` populated**, `PAGESIZE` 4096, 8 cores, ~11 GB.

**Suitability:** this tablet is **not** usable for M1's acceptance run. That step's independent
negative control is the platform *refusing* to install an `armeabi-v7a` APK, and here the platform
advertises 32-bit support. The OnePlus 12, whose `abilist32` is empty, remains the M1 device.

**Three things established by running on it:**

1. **Hardware AArch32 at EL0 is real on a 2023 flagship.** A statically-linked `armeabi-v7a` binary
   (`tools/aarch32_el0_probe.c`) executed: `rc=0`, `hwcap=0x37b0d6` with SWP/HALF/VFP/NEON/VFPv3 set.
   This settles the question `docs/research/01` left open, and it *strengthens* rather than weakens
   AD-1's reasoning: AArch32 EL0 survives on some ARMv9 parts (Cortex-X3 keeps it; A715/A510 drop it),
   and the devices that have it are the ones that can already run 32-bit apps natively — a fair-weather
   path by construction, so not worth building on. (The probe's per-bit labels are my reading of
   `HWCAP_*` positions and at least one, `thumb`, is wrong — `HWCAP_THUMB` is bit 7, not 8. The
   execution result is the finding; the decoded flags are not to be trusted.)
2. **Memory model: 99 checks, 0 failures, 0 skipped**, on a native 4 KiB host — including the
   granule-specific cases the 16 KiB emulator had to skip. Interval containers 33/33 here too.
3. **The loader had a genuine, serious bug, found only by running it.** `DT_PLTRELSZ` was never
   parsed, so `if (jmprel && pltrelsz)` skipped the entire `.rel.plt` table. Consequences, in the
   order that matters: imported functions stayed null in the GOT, and the refuse-unresolved-imports
   path never executed, so an image with unresolvable imports **loaded successfully** — exactly the
   silent null-pointer failure mode the loader was written to prevent. The earlier "17 failures" were
   mostly this, plus two of my own test bugs: the test's own `PT_DYNAMIC` scan also failed to stop at
   `DT_NULL`, and a `sed` I used to relabel a diagnostic corrupted the expression instead.

After the `DT_PLTRELSZ` fix plus refusing an unknown `DT_PLTREL` form rather than assuming REL, the
device run is **13 checks, 3 failures**, and the 3 are one cause: the loader now correctly aborts with
`unresolved symbol: __cxa_finalize` while the test does not populate its own resolver map in time. The
loader's behaviour is the desired one; the remaining failures are test wiring, and are **not** counted
as a pass. Recorded here rather than tidied away, because "the loader refuses unresolved imports" is now
load-bearing for M1 and must be re-demonstrated by a green run, not inferred from a correct-looking abort.

Still open before the loader can be called verified: fix the test's resolver wiring and re-run to
0 failures; re-run the same three suites on the OnePlus 12 when it is free; and the unresolved
`compat/` limitation for the AArch64 backend (Step 5b), which blocks `dynarmic_tests` on arm64.

> **Correction, Step 5e:** the diagnosis in the two paragraphs above — "the test does not populate its
> own resolver map in time" — was wrong, and so was the implied praise ("the loader's behaviour is the
> desired one"). The loader had a second, larger bug. Nothing in Step 5c's *result* (13 checks /
> 3 failures, DT_PLTRELSZ fixed) changes; only its interpretation did. See Step 5e.

## Step 5d — measurements only the OnePlus 12 can give (taken before it was put away)

`tools/aarch32_el0_probe.c` and `tools/va_probe.c`, cross-built and pushed; run from `adb shell`
(`shell` domain, so the executable-memory lines are indicative and the app-domain question stays open).

**M1's E1 negative control is stronger than planned.** Instead of the anticipated "PackageManager
refuses to install the APK", the kernel refuses to *start* a 32-bit binary at all:

```
/system/bin/sh: ./aarch32_probe: not executable: 32-bit ELF file     # ENOEXEC
```

That is a lower, harder level of evidence than a package-manager policy decision: on this SoC AArch32
execution does not exist, so nothing about RetroBug's result can be explained by a native path. It also
contrasts sharply with the Tab S9, where the same binary ran (see Step 5c) — two 2023 arm64 devices,
opposite capability, which is precisely the fleet variance AD-1 refuses to build on.

**U-2 closed on the real target device.** A contiguous 4 GiB `PROT_NONE` window reserves at hint
`0x10000` (`addr=0x10000`), and address 0 is refused with `EPERM` exactly as on the emulator — so
fastmem's low identity window is available where it matters, and the biased `GUEST_BASE` fallback stays
in the design rather than becoming the default.

**First U-13 data point, favourable:** anonymous `PROT_READ|PROT_WRITE|PROT_EXEC` succeeds on an OEM
(OnePlus/ColorOS, Android 16) build, as does `RW` then `mprotect(RX)`. Still only the `shell` domain, so
the app-domain case must be measured inside an installed RetroBug build — but it is the first evidence
that OEM policy has not tightened beyond AOSP here. Device page size confirmed 4096, so both granule
cases remain testable on this phone.

## Step 5e — first green load: three real bugs, and the "13 checks" was a masked count

Re-running the loader test on the OnePlus 12 with the diagnostics Step 5c left behind took the guesswork
out of it in one pass. The reported symptom was `[fixture] … symsz=16` where the image plainly has more
symbols than that, and `llvm-readelf` settled what the truth was:

```
Symbol table '.dynsym' contains 8 entries:      # .dynsym sh_size = 0x80 = 128 bytes
 0 … UND (null)   1 __cxa_finalize   2 __cxa_atexit   3 __register_atfork   4 snprintf
 5 …stringFromJNI (0x1481)  6 …fib (0x14e5)  7 …wide (0x14e9)          # all three odd ⇒ Thumb
DT_SYMTAB 0x1ec   DT_SYMENT 16 (bytes)   — and no SYMSZ anywhere in the 24-entry table
```

**Bug 1 — `elf_defs.h` invented a tag that does not exist.** I had `DT_SYMSZ = 11`. Tag 11 is
`DT_SYMENT`: the size of *one* entry, 16 — and ELF has no symbol-table *length* tag at all, which is
easy to miss because every other neighbour of `DT_SYMTAB` has an `SZ` partner (`STRSZ`, `RELSZ`,
`PLTRELSZ`). The loader therefore bounded its export scan at one entry, and entry 0 is the null symbol
with an empty name, which the scan skips. Net effect: **every guest library loads with zero exports and
zero imports reported, successfully.** That is worse than the `DT_PLTRELSZ` miss in Step 5c, because
`GuestModule::exports` is what AD-5's `RegisterNatives` index is built from — the failure would have
surfaced as "native method not found" inside a running app, with a loader log saying the image was fine.
`imports` was equally empty, so the "unresolved symbol" refusal this design leans on never fired for a
single import either; the abort seen in Step 5c came from the *relocation* path, which indexes symbols
by number and so was unaffected. That is why it looked like the resolver's fault.

Fix: `DT_SYMENT` named correctly and actually used (an image whose entries are not 16 bytes is refused),
and the table's length taken from the section header table — the only authoritative file-backed source —
matched on `sh_addr == DT_SYMTAB` as well as type, because the fixture also carries a non-alloc `.symtab`
whose symbols index a *different* string table. When there is no section header table the load now
**fails** with `cannot bound the symbol table … no matching SHT_DYNSYM section header` rather than
returning an empty export set, on the grounds that an empty set is indistinguishable from a library that
genuinely exports nothing. That refusal is tested (`symbol_table_without_a_bound_is_refused`, which zeroes
`e_shoff` in a copy of the real fixture and also proves the rollback unmaps the pages it had already put
in). `[UNVERIFIED]` how common a section-header-less `.so` is in real legacy APKs — the case is packed or
hand-built images, and if one turns up the honest extension is `DT_HASH`'s `nchain`, which the fixture
does **not** have (`--hash-style=gnu`, only `DT_GNU_HASH`), so it could not be written against anything
I could test today.

**Bug 2 — the memory model reverted the loader's writes, and only on 4 KiB hosts.** Once the scan
worked, 18 assertions fired that the truncated scan had been skipping, and the numbers were unambiguous:
every relocated word read back as its **pristine file value** (`JUMP_SLOT … want 0x9000000 got 0x1510`,
which is the linker's placeholder `.plt` address). Relocations were being applied — the unresolved-import
refusal proves the code runs — and then thrown away. Cause: `RebalanceGranule`'s in-place branch does a
`MAP_FIXED` `mmap` of the granule to change its protection, and re-mapping a *private file* page from its
fd resets it to the file's contents. The loader maps a segment writable, writes its GOT and
`.data.rel.ro`, then calls `Protect` to drop write — so the protection pass undid the relocations. A
kernel's `mprotect` never re-reads the file; now neither does this.

The snapshot loop existed but only covered pages held **out of line** (`!IsFastmem`), which is precisely
the set that excludes the pages this sequence damages. `[INFER]` This is invisible on a 16 KiB host: the
fixture's `R`/`RX`/`RW`-then-RELRO pages collide inside granules, so those pages take the out-of-line
branch, which *did* snapshot. Every device we own has 4 KiB pages, so the emulator lane would have passed
and the phones would not — do not treat a green 16 KiB run as evidence for this. Regression test:
`file_backed_writes_survive_a_protection_change` (both granules, in-place **and** out-of-line, with an
untouched-page check so the fix cannot be "zero it instead").

**Bug 3 — `PT_LOAD` tails read neighbouring file bytes as BSS.** The segment loop file-mapped
`AlignUp(vaddr + max(memsz, filesz))`, so for the fixture's RW segment (`filesz 0xe8`, `memsz 0xa90`) the
232 bytes past its content were served from the file's `.comment`/`.ARM.attributes` region instead of
zeros. Now the file is mapped only across its own content, the remainder of the last content page is
explicitly cleared, and whole BSS pages are mapped anonymous — the kernel's shape, and the reason the
`BSS byte at +232` check exists.

**Also fixed, in the test, after the loader stopped being the suspect:** `f.needed` was assembled by a
second `PT_DYNAMIC` pass that assigned every `DT_NEEDED` to `f.needed.back()`, so two of three
dependencies were discarded and the third was `libc.so`; and its string helper took a `.dynstr` *offset*
where a *vaddr* was needed. Both made the loader look wrong when it was right — `liblog.so, libdl.so,
libc.so` in the loader's output matches `readelf` exactly. Recorded because the same two mistakes are
waiting for whoever writes the next ELF-side test, and because "the fixture and the loader disagree" has
now cost two rounds in which the fixture was the broken side.

**Measured, OnePlus 12 (arm64 host, 4 KiB pages, guest = the armeabi-v7a fixture), all three suites in one
session, current binaries:**

| Suite | Result |
|---|---|
| `rb_interval_compat_tests` | 33 checks, 0 failures |
| `rb_guest_memory_tests` | **131 checks, 0 failures, 0 skipped** (was 99; +32 from the file-backed regression test) |
| `rb_guest_elf_tests` | **71 checks, 0 failures** (was 13/3 — the old count *was* the symptom: a scan of one entry runs almost no assertions) |

x86_64 lane: all three targets compile clean against the same headers (`build/x64-recheck.log`, `exit=0`).
**Running** them there still needs the emulator, i.e. `net start aehd`.

**What may now be claimed:** `rb::loader` is executed-and-verified against one real `armeabi-v7a` shared
object for segment placement, `R_ARM_RELATIVE`/`R_ARM_JUMP_SLOT` correctness, per-symbol import refusal,
export enumeration, RELRO and BSS content, and Thumb-tag reporting — on hardware, with the numbers
cross-checked against `llvm-readelf` rather than against itself. **What may not:** no guest instruction
has been *executed* yet (that is Step 6, `rb::exec`), the Thumb tag is verified as a reported value and
not as a mode switch that ran, `.so` files needing `R_ARM_COPY`/`THM_*` relocations are still untested,
and the mixed-granule loader path on a **native** 16 KiB host is still emulator-blocked.

## Step 5f — Step 5b resolved: `apply_visitor` has to unwrap, and the arm64 DBT lane runs on hardware

Step 5b left the AArch64 ambiguity **unresolved** and listed three candidate fixes — weaken the
wrapper's conversion set, make `variant`'s converting constructor reject wrapper sources, or take real
Boost for that lane. All three were the wrong shape: they attacked the conversions, and the conversions
were fine. The missing piece is that **Boost's visitor dispatch unwraps `recursive_wrapper` before
calling the visitor**, so a visitor's `operator()(const If&)` receives an `If`. A bare `std::visit` —
what my shim forwarded to — hands over the wrapper instead, and *that* is what makes the call
ambiguous. Unwrapped, the by-value alternative overload is an exact match and the whole-variant overload
is not even a candidate. Fourth option, none of the three above needed, and no edit to
`external/dynarmic`: `detail::invoke_visitor` in `compat/boost/variant.hpp`, used by all three
`apply_visitor` overloads.

Two small changes fell out of the same reading, both in the shim:

- `recursive_wrapper`'s default constructor left `held_` null while `operator const T&()`
  dereferences it unconditionally — reachable undefined behaviour from legal code. It now always
  holds a `T`, as Boost's does, and the null branches in copy/`operator==` went away with it.
- `mutate()` (added earlier, used nowhere) became a `value()` pair, which is what the unwrapping
  dispatch needs: `T&` from a mutable variant, `const T&` from a const one.

**Why this was worth a test, not just a green build.** "It compiles now" is not the property; the
property is *which overload the visitor reaches*, because a shim that quietly preferred the
whole-variant overload would also compile and would recurse forever. `core/tests/variant_compat_test.cpp`
(12 checks) asserts the unwrapped type is what the generic lambda sees, drives dynarmic's exact
overload shape and records which function ran, reproduces the incomplete-type declaration order from
`ir/terminal.h`, and checks deep-copy and `which()`/`get<>` behaviour. `boost::get<Interpret>` is
included because that is the only wrapped-variant `get` dynarmic performs, and my shim forwards it to
`std::get_if` — which requires the exact alternative type, so had it asked for a wrapped one it would
not have compiled.

**Measured, all five suites, one run:**

| Suite | Result |
|---|---|
| `rb_interval_compat_tests` | 33 checks, 0 failures |
| `rb_variant_compat_tests` (new) | 12 checks, 0 failures |
| `rb_guest_memory_tests` | 131 checks, 0 failures, 0 skipped |
| `rb_guest_elf_tests` | 71 checks, 0 failures |
| `dynarmic_tests` | **200,561 assertions in 47 test cases, all passing** |

The engine number matters more than the four above it: it is Dynarmic's **AArch64** back end translating
A32 IR into real arm64 host code and executing it, which is the configuration RetroBug ships on Android.
The x86_64 emulator run was 48 cases for the same 200,561 assertions, and the difference is not a lost
test — `tests/x64_cpu_info.cpp` is added by dynarmic's own `CMakeLists.txt` only
`if ("x86_64" IN_LIST ARCHITECTURE)` and its `TEST_CASE` prints xbyak's CPU-feature table with **zero**
assertions. Checked in the CMakeLists, not inferred from the arithmetic.

`tools/run_engine_tests.sh` now keeps one `SUITES` list for build, push and run (a suite missing from the
push list silently stops being evidence, which is how `rb_variant_compat_tests` would have been missed),
and the `MSYS_NO_PATHCONV` trap got its fix: that variable is needed for the *device* path but also
stops conversion of the *local* path, so `adb.exe` was handed a `/c/...` path it cannot open and the
push failed on a file that plainly exists. `to_host()` converts the local path explicitly; a Linux CI
runner takes the no-op branch.

**Attribution, stated carefully.** The five-suite run above went to the **SM-X710 Tab S9** — confirmed by
reading `ro.product.model` and the `/data/local/tmp` timestamps on it. The earlier `dynarmic_tests` pass
that first showed the arm64 lane green was run while the OnePlus 12 was the attached device; that
attribution rests on which device was connected at the time rather than on a property read from the
device, so it is the weaker of the two claims. What is not in doubt: the AArch64 lane has now passed on
two different arm64 devices, so it is not a one-machine result.

Consequence for the plan: `docs/10` §17's U-8 line "still unverified: the AArch64 A32 backend" is
closed, and Step 5b's `compat/` limitation is gone. What that lane still does **not** cover is unchanged
and is written in the script's header: dynarmic asserts what it asserts, and no guest app instruction has
run yet — that is Step 6, `rb::exec`.

## Step 5g — the ARMv7 hardware oracle, and what a real 32-bit process cannot read

The Tab S9 was attached and it is the only machine here that executes AArch32, so the M0 exit test's
expected numbers stopped being assumed and became measured: `tests/corpus/oracle_armv7.c` +
`tools/run_armv7_oracle.sh` link **the fixture's own source** (`fixture_jni.c`, same flags:
`-mthumb -mfloat-abi=softfp -mfpu=neon -Os`) into a static armeabi-v7a executable and run it. Its two
JNI entry points discard `JNIEnv*` and `jclass`, so they are callable with no JVM.

```
# device=SM-X710 abilist32=armeabi-v7a,armeabi page=4096
thumb=1 fib_addr=0x1b1ed
fib(0)=0 fib(1)=1 fib(2)=1 fib(20)=6765 fib(20)again=6765
wide(0)=52 wide(2)=50 wide(-1)=55
string=fib(20)=6765 wide=50
```

Raw output, with the device model, ABI list, page size and source revision attached, is committed as
[`tests/corpus/oracle-armv7.txt`](../tests/corpus/oracle-armv7.txt) — the script rewrites that path, so
the numbers asserted against stay auditable rather than living only as a transcription here.

`thumb=1` is the check that the oracle tested what it claims: bit 0 of an ARM function pointer is the
ISA tag, and every function in a `-mthumb` image carries it. `wide` is `table_sum XOR 0x56`, so the
three values also reproduce by hand (98^86=52, 100^86=50, 97^86=55) — hardware, the source's stated
intent and x86 arithmetic agreeing is the point of an oracle. These are now the numbers Step 6/7 assert
against, in `docs/20` §M0 and `docs/21` §3.

**Then it crashed, and the crash is the more useful result.** The first version ended with a CP15 MIDR
read and died with SIGILL after printing *nothing* — which looked like a broken build. It was not:
stdout through `adb shell` into a pipe is block-buffered, and the buffer is lost when the process dies
on a signal, so "crashed on line 1" and "crashed on the last line" are the same observable. Unbuffered,
the truth appeared in order: every number above printed, then the CP15 read killed it.

Run one process per register (`--cp15 <name>`, because a refusal takes the process with it and a single
run would report only the first):

| Read from an AArch32 EL0 process | SM-X710, native 32-bit |
|---|---|
| `mrc p15, 0, Rd, c0, c0, 0` — MIDR | **SIGILL** |
| `mrc p15, 0, Rd, c0, c0, 1` — CTR | **SIGILL** |
| `mrc p15, 0, Rd, c0, c0, 5` — MPIDR | **SIGILL** |
| `vmrs Rd, fpsid` | **SIGILL** |
| `mrc p15, 0, Rd, c13, c0, 3` — TPIDRURW | **works**, `0xf7450fe0` |

The split is coherent: the ID registers are off-limits to EL0 on an ARMv8 core running AArch32 and this
kernel does not emulate the read, while the thread-register bionic actually uses for TLS works normally.
`[INFER]` that the four failures share that cause rather than four separate ones — what was *measured* is
the pass/fail column, and the register numbers themselves come from reading the AArch32 encoding table,
not from this run. `[UNVERIFIED]` whether any kernel does emulate EL0 MIDR for AArch32 (Linux's
`armv8_deprecated` insn-emulation framework covers SETEND/SWPCP/legacy `LDREX`, and I have not confirmed
either way that ID reads are or are not on that list); one CI job or one different vendor kernel answers
it.

Three consequences, all of which change a plan document:

1. **`docs/21` §E5's MIDR assertion has no native comparator.** It was written expecting to compare
   RetroBug's answer against "the host's" — but the host cannot produce an answer, because the read
   faults. So the assertion is against the ARMv7-A *architecture*: RetroBug answering a MIDR read is
   deliberately more permissive than the silicon it runs on, and that is the brief's requirement
   (reproduce ARMv7 CPU semantics) rather than a divergence to apologise for. Recorded in `docs/21` §3.
2. **The fixture becomes unusable as a native control on a 32-bit-capable device once it reads MIDR** —
   it would die for a reason unrelated to emulation. E1's control stays on the 64-bit-only device, where
   the kernel refuses the binary before any instruction runs. This is exactly the kind of trap that
   looks like a milestone failure when it is a fixture property.
3. **CP15 emulation has a cheap ground truth available for the part that matters.** TPIDRURW working
   natively means a real 32-bit Android process's TLS traffic is observable on this device, which is the
   behaviour `docs/research/01` says RetroBug must reproduce for `__get_tls()`. The ID-register half has
   no native reference; the TLS half does.

Also fixed in passing: `-llog` cannot link into a static 32-bit build (no `liblog.a`), and the first
`mrc` attempt was rejected by the assembler for a missing comma — GNU ARM syntax needs
`mrc p15, 0, Rd, c0, c0, 0`, commas and all, which is what `docs/21` §3 already had right.

## Step 6a — `rb::isa` (C01): the front-end door is open, and it found a loader bug on the way in

`core/isa/front_end.cpp` + `core/include/rb/isa/front_end.h`: one function,
`TranslateOne(guest_memory, pc, Mode) → TranslatedBlock{ir, ops, terminal, entry, mode, code_words,
truncated_by_unmapped_read}`. It is the wrapper AD-14 requires — the vendored Dynarmic is untouched
and everything RetroBug needs to *say* about the front end is said here: `ArchVersion::v7` (not `v8`:
that would accept encodings no armeabi-v7a binary may contain; not `v6T2`: it would refuse the 47,894
Thumb-2 wide encodings `docs/32` counted), `define_unpredictable_behaviour = true` because a real
ARMv7 core does something rather than faulting, and `hook_hint_instructions = true` so BKPT and the
cache-maintenance path reach RetroBug instead of becoming NOPs.

`rb::isa` is the first core component that legitimately links the engine, so `core/CMakeLists.txt` now
has `rb_isa` beside `rb_mem`/`rb_loader` and the split from Step 5b survives: the memory and loader
suites still build without the DBT. One build-system fact worth keeping: **Dynarmic links
`merry::mcl`, `fmt::fmt`, `tsl::robin_map` and `Boost::boost` PRIVATE**, so any consumer that includes
its public headers must supply them again — reaching `ir/basic_block.h` alone pulls in
`<mcl/stdint.hpp>` and `<boost/variant.hpp>`. `Boost::boost` is additionally *directory-scoped* (the
stub `FindBoost` creates it as an IMPORTED target inside Dynarmic's subtree), so `core/` takes the
compat include directory rather than the target.

**The test found a bug in the loader, and the bug was found by writing an assertion the loader did not
satisfy.** `Translate` reads 4-byte-aligned words; recording which words a block needed is what lets a
code cache invalidate on a guest write (AD-7 self-modifying code), so the test asserts every recorded
word is aligned, mapped, in the window, and **inside `[mod.base, mod.base + mod.extent)`**. That last
one was not true in general: the loader placed segments at `base + map_start` (absolute image vaddr)
while reporting `extent = hi - lo`. For every ordinary `ET_DYN` shared object `lo == 0`, so the two
agree and no existing test can see it. For an image whose first `PT_LOAD` starts above 0 the image
would be mapped up to `lo` bytes past the extent it advertises — and the free-space scan checked a
*different* range than the one it then wrote into, which is precisely the "placed over somebody else's
pages while reporting a clean load" failure this component has now produced twice. Fixed by separating
the two ideas: `base` is the **load bias** (`image vaddr V lives at base + V`), and
`mod.base = base + lo` is where the lowest byte sits, which makes the reported extent exactly the mapped
range and makes the scan check the range it writes. `[1L]` for the `lo == 0` case, which is what runs;
`[INFER]` that the fix is correct for `lo > 0` — no such image exists in the corpus, and hand-crafting
one was not worth its error budget today. The assertion is the thing that will catch it if that
reasoning is wrong.

**One of my own test assertions was the wrong contract, and the code was right.** I wrote
"an odd Thumb address is the ABI's mode tag, so it must not be refused" — but `TranslateOne` takes a
*byte* address plus an explicit `Mode`, and a halfword-addressed fetch is the ARM-mode misalignment
case, not a Thumb one. The tag belongs to symbols and function pointers, and `CodeAddress{addr,
thumb}` (Step 5e) is where it gets split off. The test now states that contract instead of arguing
with it.

**What real Thumb-2 compiles to, printed rather than asserted** (101 checks, 0 failures on the SM-X710;
the fixture loaded through `rb::loader`):

| Export | IR | Terminal |
|---|---|---|
| `…_fib` @0x34e4 | 2 ops: `GetRegister`, `SetRegister` | `LinkBlock` — the JNI entry is a thunk; the recursive body is elsewhere |
| `…_wide` @0x34e8 | 37 ops incl. `GetVector`/`VectorAdd32`/`SetVector`, `Add32`, `Eor32`, `GetNZCVFromOp`/`SetCpsrNZCV`, `BXWritePC` | `PopRSBHint` |
| `…_stringFromJNI` @0x3480 | 28 ops incl. 4× `WriteMemory32`, `PushRSB` | `LinkBlock` |
| `…_fib`'s bytes decoded as **ARM** | 4 ops ending `ExceptionRaised` | `CheckHalt` — which is the proof the mode flag reaches the decoder rather than being ignored |

`wide` lowering to NEON element ops on a `-mfpu=neon -Os` build is the measured demand `docs/32` argued
for ASIMD mattering, arriving from the other direction: the front end emits those ops for code a
compiler wrote, so Tier I's dispatch table has to cover `GetVector`/`SetVector`/`VectorAdd32` and not
only the scalar set.

**Full lane, SM-X710, one run:** interval 33/0 · variant 12/0 · memory 131/0 · loader 71/0 ·
**isa 101/0 (new)** · dynarmic 200,561 assertions / 47 cases.

Not done, and not claimed: nothing has been *interpreted* yet. `TranslatedBlock::ops` is the list Tier
I dispatches on, and the exit test (`fib(20) == 6765` under both tiers, `docs/20` §9 item 7) is still
the milestone boundary.

## Step 6b — `rb::exec` interface + block census: Tier I's work list is 13 ops, not 612

`Dynarmic::IR` has **612 opcode declarations**, which makes "implement the IR interpreter" look
unbounded. It is not, and the census is what shows it: `core/exec/census.cpp` walks the block graph the
way an executor will — following `LinkBlock`/`LinkBlockFast`/`Interpret` successors and descending the
nested `If`/`CheckBit`/`CheckHalt` legs — keyed on the **full location descriptor**, and histograms the
IR ops it reaches. Measured on the fixture, on hardware (41 checks, 0 failures):

| Function | Blocks | Ops | Distinct ops | Terminals seen |
|---|---|---|---|---|
| `…_fib` | 4 | 55 | **13** | `LinkBlock`×2, `If`, `PopRSBHint` |
| `…_wide` | 1 | 37 | 23 | `PopRSBHint` |
| `…_stringFromJNI` | 4 | 81 | 13 | `LinkBlock`×2, `If`, `PopRSBHint` |

fib's 13 are `GetRegister`, `SetRegister`, `Add32`, `Sub32`, `ReadMemory32`, `WriteMemory32`,
`GetNZCVFromOp`, `SetCpsrNZCV`, `GetNZFromOp`, `SetCpsrNZ`, `BXWritePC`, `PushRSB`,
`UpdateUpperLocationDescriptor`. `wide_probe` adds the VFP/NEON path (`GetVector`/`SetVector`/
`VectorAdd32`, `GetExtendedRegister64`, `Pack2x32To1x64`, `LeastSignificantWord`, `MostSignificantWord`,
`LeastSignificantByte`, `ZeroExtendByteToWord`, `GetCarryFromOp`, `And32`/`Or32`/`Eor32`).

So Tier I's first milestone — run fib and wide correctly — is about 25 ops plus terminal handling, not
612. That is the number to argue with if the tier ever looks enormous, and it came from the front end
rather than from a guess, which is the point of building the census first.

Three things the walk has to get right, each with a test rather than an assurance:

- **Termination on guest loops**, by keying on `LocationDescriptor::UniqueHash()`. A loop returns to the
  same descriptor, so it is a repeat key and not a repeat walk.
- **Keying on the whole descriptor, not the pc.** ARMv7 IT state changes what the same bytes decode
  to, so a block reached with two IT states is two blocks. Keying on pc alone would have merged them
  and under-reported exactly the predication paths `docs/32` measured as load-bearing — which is why
  `rb::isa` grew a descriptor-taking `TranslateOne` alongside the `(pc, mode)` one.
- **An incomplete walk says so.** `Census::blocks_left_unvisited` is a field, not an error code, and the
  test forces it non-zero with `max_blocks = 2` to prove the flag fires; otherwise a capped census is
  indistinguishable from a small function. Same reasoning for a start address with no code behind it:
  Dynarmic *does* emit a real block there (an `ExceptionRaised` epilogue), so the assertion is that the
  walk reports *why* (`truncated_blocks`/`translate_failures`), not that it returns nothing. That
  distinction came out of the test being wrong first: I asserted `blocks == 0` and it failed, because
  zero was never the honest expectation.

`rb::exec`'s `MachineState` is now fixed in types (r[16] and d[32] as docs/20 §C02 specifies, `tp` for
TLS, IT state carried explicitly) with the width invariant asserted rather than commented: guest
addresses are 4 bytes, and `tp` — the one field that could plausibly be mistaken for a host pointer —
is checked to be one.

## Standing environment facts (so they are not re-derived)

| Fact | Value |
|---|---|
| Host compiler for Windows binaries | **none** (`gcc`/`cc`/`clang`/`cl` all absent) → M0 tests are **Android** binaries: arm64 on a phone or tablet is the primary lane (fastest, real hardware, 4 KiB pages), x86_64 in the emulator is the 16 KiB-granule lane |
| Devices available, and the difference that matters | **OnePlus 12** (`CPH2573`, SD 8 Gen 3): `abilist32` empty, kernel refuses a 32-bit ELF with ENOEXEC → M1's negative control. **SM-X710 Tab S9** (SD 8 Gen 2): `abilist32=armeabi-v7a,armeabi`, executes AArch32 → the only ARMv7 hardware oracle here. Two 2023 arm64 devices, opposite capability: never infer one from the other or from a spec sheet |
| `adb push` and mtimes | the destination keeps the **source** file's mtime, so two binaries pushed in one second show different timestamps. Useful for attributing a run to a device, and the reason `ls -la /data/local/tmp` can look like a stale push when it is not |
| A crashing device process loses its buffered stdout | `adb shell` pipes stdout, so it is block-buffered and `printf` output before a `SIGILL`/`abort` disappears. `setvbuf(stdout, NULL, _IONBF, 0)` in any probe that might die — without it, "crashed on line 1" and "crashed on the last line" are the same observable (this cost a cycle in Step 5g) |
| Emulator acceleration | AEHD 2.2 **installed but stoppable**: it was "usable" for two boots, then the `aehd` service was found `Stopped` and the AVD refused to start. Re-arm with an elevated `net start aehd`; check `Get-Service aehd` before trusting `-accel-check`'s "not installed" wording. AVD `Medium_Phone` exists |
| Emulator image | `android-37.1/google_apis_playstore_ps16k/x86_64`, `abilist=x86_64,arm64-v8a`, **`abilist32=` empty** |
| `openat2`/`io_uring`/`landlock` from an app | unavailable (outside bionic's `SYSCALLS.TXT` allowlist) — see `docs/research/05` §1.4 |
| Executable memory from an app | permitted: `allow appdomain self:process execmem;` (`private/app.te:189`, read directly) |
| Exec from app-writable files | forbidden: `app_neverallows.te:69-73` (read directly) → why AD-5 ships one pre-built agent `.so` |
