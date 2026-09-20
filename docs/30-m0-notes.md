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

## Step 3 — coverage audit, demand half **(done: `docs/32-isa-coverage.md`; totals corrected in Step 6c)**

`tools/isa_coverage.sh` measures what real ARMv7 Android code actually demands, from the NDK's own
AArch32 archives: **711 objects, 325,541 instructions, 759 distinct mnemonics**. (This line originally
read 338,034 / 855; §Step 6c explains the tokenizer bug and the second claim of mine it caught.) Three
results that change assumptions rather than merely confirming them:

- **8,387 IT-block instructions** in bionic itself — the IT-block risk named in `docs/research/01` is now
  backed by demand, not just subtlety, so it leads the differential queue.
- Hundreds of `vadd.f64`/`vmul.f64`/`vcmp.f64` — libm's double-precision paths mean **VFPv3 D32 is
  mandatory**; a single-precision-scoped engine fails here.
- `movw` 3,284 and `cbz` 3,271 — the concrete ARMv7-only forms the ARMv6-vs-ARMv7 question hinged on.

Four self-corrections are now recorded in that doc, because each would otherwise be re-cited as evidence:
the §4.1 "file listing proves coverage" argument was the wrong kind of proof (dispatched ≠ correct); a
`UndefinedInstruction()` I initially flagged as a NEON gap turned out to be the architecturally-correct
rejection of undefined `VST4/VLD4 .64` encodings; the totals above were inflated by the measurement
tool's own tokenizer; and "SDIV/UDIV are used too" had never been measured at all — neither mnemonic
occurs in this corpus. Grep-found gaps have to be read before being reported, and so do numbers that
come out of a tool I wrote.

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

## Step 6c — the mnemonic→handler survey, two corrections to my own numbers, and a finding about Tier I

`tools/mnemonic_handlers.sh` answers docs/32 §5.2 by joining objdump's mnemonic to Dynarmic's decoder
cases and then to the translator body that implements each. **Result: 757 of 759 demanded mnemonics have
an implemented translator, covering 325,532 of 325,541 instructions (100.00%)**; the 2 unmatched rows are
9 instructions of objdump pseudo-tokens (`trap`, `rrx`), not encodings. So the *presence* half of U-8 is
closed and what remains is semantics.

Three things worth recording about how it was produced:

1. **A premise of mine was wrong and the tool caught it.** I specified that a translator body consisting
   of `return false;` should count as "declared but unimplemented". It is the opposite: `false` is
   Dynarmic's ordinary "the block ended here" — `arm_B` returns `false` after setting the terminal. The
   real refusal idiom is `return UndefinedInstruction();`, of which there are 11 decoder cases (`arm_LDRT`
   at `load_store.cpp:33`, `arm_UDF`, `thumb16_UDF`, …), and the correct bucket for the `return false`
   shape is *implemented*. Had I classified it my way the report would have claimed dozens of ARM
   instructions were unimplemented — a gap that does not exist, produced by an assumption I wrote into a
   task specification rather than checked. Verified by reading `arm_B` and `arm_LDRT` directly.
2. **The survey forced a re-look at Step 3's numbers, which were wrong.** The corpus is **325,541
   instructions across 759 mnemonics**, not 338,034 across 855: objdump prints a byte column on
   data-bearing lines, and the old tokenizer took that column as the opcode (96 bogus rows, 3,481
   instructions) while a separately-written looser count for the total included `.word` data lines. The
   first repair then dropped the address anchor and began counting objdump's `Disassembly of section
   .text:` headers, which appeared as 2,686 instructions of a mnemonic called `of` — caught only because
   that number was queried instead of accepted. Full write-up in `docs/32` §3.3. Per-mnemonic counts never
   changed, so no §2 conclusion moved; the corrected totals are in §1.
3. **One claim of mine had never been measured.** "`SDIV`/`UDIV` are used too" — `grep` of both the old
   and the corrected histogram: zero occurrences of either. It was an inference from "ARMv7-A has divide"
   plus "this is ARMv7-A code", and NDK clang's `armv7a` target without `+div` evidently never emitted
   them. Corrected in `docs/32` §2.3/§3.4 with the reason, since the inference is plausible enough to be
   worth flagging rather than just deleting.

**The architectural finding, which changes C02's status.** `InterpretThisInstruction()` sets
`IR::Term::Interpret`, and the back ends implement that terminal by calling
`UserCallbacks::InterpreterFallback(pc, num_instructions)` (`interface/A32/config.h:104`,
`backend/x64/a32_emit_x64.cpp:1133`) — **Dynarmic ships no interpreter**; the embedder is expected to
execute those instructions itself. The translator routes `arm_LDM_usr`, `arm_STM_usr`, `arm_CPS`,
`arm_RFE`, `arm_SRS` there, and the survey shows those paths carry 509 instructions of bionic
(`ldm`, `stm`, `ldm.w`, `stm.w`, `ldmge`, `stmge`). So Tier I is not only the iOS build and the
`execmem`-denied fallback — **a Tier-J Android build has nowhere to go without it**, which is now AD-2's
third consequence in `docs/10` §5 and is part of why the census in Step 6b keyed on real translated code
rather than on the op list I would have guessed.

Also from the same source: `docs/10`'s U-8 row is rewritten to "presence answered, semantics open", and
`docs/20` §9 item 3 closes.

## Step 6d — Tier I executes: fib matches the hardware curve, and three things I believed about the IR were wrong

`core/exec/interp.cpp` + `core/tests/exec_interp_test.cpp`. **Definition of done, met:** the interpreter
computes `fib(0..8, 20) = 0 1 1 2 3 5 8 13 21 6765` and `wide(0, 2, -1) = 52 50 55`, each value taken from
`tests/corpus/oracle-armv7.txt` on the SM-X710 rather than from this engine. Step counts grow the way a
real recursion must — 46 → 114 → 316 → 2266 → **735,946** for `fib(20)` — which is the observation that
distinguishes "returns the right number" from "actually recursed". Suite green on hardware: 33 · 12 · 131 ·
71 · 101 · 41 · **80** checks, 0 failures, plus Dynarmic's own 200,561 assertions in 47 cases.

It took three wrong beliefs to get there, and each one was load-bearing enough to be worth writing down
rather than fixing silently.

**1. The first "engine bug" was my test, and it produced plausible answers.** The exports are JNI
methods: `fib(JNIEnv*, jclass, jint n)`, so AAPCS delivers `n` in **r2** and my harness was putting it in
r0. The guest then read `n = 0` and returned `fib(0) = 0` — correctly, for every input. A uniformly wrong
but individually plausible value is the worst shape of harness bug, because it points at the engine's
arithmetic and not at the register file. The hardware oracle is what pinned the positions down (it calls
`NULL, NULL, arg`), and the expectation list is now the measured curve rather than two points, because
"breaks at n=20" is far harder to localise than "breaks at n=4". `[1L]`

**2. `Add32`/`Sub32`/shift third operand is `carry_in`, not `setflags`.** I had read it as the A64 IR's
flag-setting bit, and built a working-looking rule on it: `if (operand2 != 0) { compute flags }`. It is
`IR::IREmitter::AddWithCarry`/`SubWithCarry` (`ir/ir_emitter.cpp:151-180`), lowered to AArch64
`ADC`/`SBC` when the carry is not an immediate, and plain `Sub(a, b)` is *emitted as*
`Sub32(a, b, Imm1(1))`. Two separate damages followed:

- Every `adc`/`sbc` computes the wrong **value**, since the carry-in was read and then discarded. Plain
  add/sub were right by accident (ci is 0 for one, 1 for the other), which is exactly how this survived
  the `wide_probe` checks.
- **`push` sets flags in Tier I.** `push {r4,r5,r7,lr}` lowers to `Sub32(sp, #0x10, #1)`, whose `#1` is a
  carry-in; my gate read it as "this op set flags" and recorded `C=1` for it.

**3. Which exposed the real design error: one mutable copy of the flags.** The IR does not update CPSR in
place. An arithmetic op *emits a value*, and a pseudo-operation naming that value — `GetNZCVFromOp`,
`GetNZFromOp`, `GetCarryFromOp`, `GetOverflowFromOp` — is what reads flags back, so the only faithful
question about a value is "what would this instruction's flag-form have left in the host flags
register". The ambient `Flags flags` member made `push`'s invented flags readable by the next
un-predicated consumer, which is what turned `cmp r0, #2` into `Z=1`. The symptom was
`g(n) = g(n-1) + (n-2)` — `bhi` never taken, because `C` had been overwritten by a `movs` that had
inherited it from a `push` — and the closed form matching the printed values is what confirmed the
mechanism before the fix rather than after.

The model now is: per-producing-instruction records (`produced_`, plus `carry_` for shifter carries),
read by whichever pseudo-op names the producer; when the producer has no flag-form, the fallback is
`TST v, v` — which is literally what the back end emits (`backend/arm64/emit_arm64.cpp:87`) and which
**clears** C and V rather than carrying them through. `SetCpsrNZ` keeps C and V, `SetCpsrNZC` keeps only
V, `SetCpsrNZCVQ` also writes Q from bit 27: each one is a mask over CPSR's top nibble, mirroring
`emit_arm64_a32.cpp:398-492` line for line.

Two corollaries of reading the back end instead of the ARM ARM:

- **A shift's carry-in never affects its result.** The AArch64 lowering emits a plain `LSL`/`LSR`/`ASR`
  and reads the carry-in only as the carry-*out* when the amount is zero
  (`emit_arm64_data_processing.cpp:276`). So amount 0 is "no shift, carry unchanged" — **not** RRX; ARM's
  `LSR #0` = shift-by-32 and `ROR #0` = RRX quirks are applied by the *translator*, which emits
  `LogicalShiftRight32(v, 32, ci)` or `RotateRightExtended(v, ci)` to say so. My old code re-applied them
  to IR that had already accounted for them, which would have shifted twice.
- Bitwise ops need **no** flag record: `ANDS`/`ORS`/`EORS` leave N/Z from the result and clear C/V, which
  is what the `TST` fallback already computes. Shifting that fact into a comment saved an entire case.

**A latent one, found while writing the above:** every `SetCpsr*` masked CPSR with `~0xf'0000'0000ull`. On
a `uint32_t` that promotes to a 64-bit complement whose low 32 bits are all zero, so it cleared the
**entire** register and restored only the nibble — destroying the mode bits and anything else in the low
28. Nothing in the fixture reads CPSR back, so it was invisible; it is now `~0xf000'0000u`. `[1L]` for the
mask being wrong, `[UNVERIFIED]` for its reachability from a guest `MRS`.

**`pop {…, pc}` does not restore LR, so LR-after-return is not an invariant.** Three of my assertions
expected r14 to still hold the sentinel; `fib`'s epilogue loads PC from the stack and leaves r14 holding
the last executed `bl`'s link (`0x34d7`) — correct behaviour, wrong test. Replaced with a callee-saved
check (`r4/r5/r7` loaded with distinct values that nothing in `fib` could compute, so a `pop` writing to
the wrong slot or in the wrong order is caught at the frame rather than several instructions later).

**Not established by any of this.** IT-block semantics are untested — the fixture's Thumb-2 `fib`
predicates nothing, and `docs/32` §2.1 measured 8,387 IT instructions in bionic, so the highest-priority
differential lane still has no Tier I coverage. `[UNVERIFIED]` similarly: `GetGEFromOp` (SIMD32 GE flags,
unimplemented — Tier I stops with `Unimplemented` rather than guessing), the FPSCR/Q flags beyond their
masking, and whether the arm64 back end can even compile `wide_probe`'s `GetCarryFromOp` on a
`MostSignificantWord` (it `ASSERT`s the pseudo-op is attached; that question belongs to Tier J bring-up in
step 8, not to this file). Two tiers agreeing is also still *asserted nowhere* — the exit test that
compares `MachineState` between them is step 7.

**On-device debugging, kept rather than removed.** An interpreter on a phone has no debugger, so
`interp.cpp` retains two switches: `RB_T1_TRACE=<run-index>` dumps each block's IR via
`IR::DumpBlock` with pre/post registers, and `RB_T1_PROBE=1` prints the arithmetic and flag-derivation
decisions. Both are off unless set, and the IR dump is what turned a 20-minute guess into the 5-minute
identification of the `push`-sets-flags bug — it showed the `#1` on an operand that could not have meant
set-flags on a `push`.

## Step 7 — `rb::abi` and the fake `JNIEnv`, which found Tier I's IT-block bug by measurement

Three pieces, because the third was only reachable once the first two existed. Ten lanes green on the
SM-X710: 33 · 12 · 131 · 81 · 101 · 41 · 80 · 78 · **41** checks, 0 failures, plus Dynarmic's 200,561
assertions.

**7a — the bridge (`core/abi/call.cpp`).** docs/20 §C08 names this file as where the ABI-correctness
review pass concentrates, so the rules that hide are stated where they are applied:

- An 8-byte integer needs a consecutive **even/odd core-register pair** — with one 4-byte argument
  ahead of it, the pair goes to r2/r3 and **r1 is consumed by nothing**. Taking r1/r2 is the classic
  `jlong` failure, invisible until a call has an odd number of preceding 32-bit arguments.
- ~~`-mfloat-abi=softfp` **passes** scalars in VFP registers; it merely computes them in software. A
  `double` goes to `d0..d7` and consumes **no** core register, so `f(double, int)` leaves the int in
  r0.~~ **This bullet was wrong, and it was the origin of a real defect.** softfp passes arguments and
  results by the *soft-float* PCS — core registers and stack — and only the arithmetic uses VFP
  instructions; the VFP-register argument convention belongs to `-mfloat-abi=hard`, which clang refuses
  for this target — re-measured while writing Step 25, and the message is worth having verbatim because it
  is the whole argument in one line:
  `clang: error: unsupported option '-mfloat-abi=hard' for target 'thumbv7-none-linux-android26'`.
  `f(double, int)` leaves the int in **r2**. The rule as written here was implemented
  literally and stayed in `PlaceArgs` until Step 25; the disassembly that disproves it is in Step 24
  (results) and Step 25 (arguments).
- ~~VFP allocation is counted in **32-bit units** (a float takes `s(n)`, a double takes an even unit
  pair, so `s0,s1 == d0`), and a single write leaves the other half of its `D` register alone.~~ The
  allocation described no longer exists — `PlaceArgs` allocates nothing from the VFP bank. The
  half-preserving-write rule it cites is still real and still applies, but inside the *instruction*
  tier (`SetExtendedRegister32`, §Step 6d), not at the call boundary.
- Spilled arguments go at **ascending** addresses (reversing them is invisible with one spill and wrong
  with two), the frame keeps the 16-byte intra-procedure area **below** SP that AAPCS lets a leaf use
  across a `bl` — which means the argument block starts *at* the callee's entry SP, and does not begin 16
  bytes above it — and a frame too small to hold its spills **refuses before mutating any register**, so
  there is no half-applied call to debug. The middle clause is where `PlaceArgs` and this sentence
  disagreed for many revisions; Step 25 has the details and the test that could not see them.

**7b — a `JNIEnv` made of guest memory and host-call slots.** What the guest's own instructions say the
type is, measured out of `jni_utf`:

```
ldr   r2, [r0]            ; r2 = *env        -> the function table
ldr.w r2, [r2, #0x29c]    ; r2 = table->NewStringUTF
blx   r2                  ; r0 = env, r1 = the char*
```

so `env` is a guest word holding a guest address, which is a table of guest addresses — and the entries
are not code but `AbiContext` slots, which the executor recognises **before** translating. No page has
to be executable for a host function to be callable, which is the same mechanism the return sentinel
already used.

- **All 233 entries are bound**, implemented or not. An unfilled entry is a guest branch to address 0:
  a fault with nothing to say about itself. A bound-but-refusing entry names its index, which is the
  difference between "this app needs FindClass" and a blank screen.
- The indices are **measured, not transcribed**. `tests/corpus/jni_offsets.c` prints
  `offsetof / sizeof(void*)` from the NDK's own header (so the number is pointer-width independent), and
  `NewStringUTF`'s **167** is confirmed a second time by the `#0x29c` above (668 / 4). The four reserved
  NULL slots at the head of the table are why counting function *declarations* gives 163 — an
  off-by-four that would have the guest calling `ReleaseStringChars` when it meant `NewStringUTF`,
  **successfully**.
- `core/` may not include Android headers (`core/CMakeLists.txt` enforces it; it is what keeps the
  engine buildable for an iOS host), so the indices are transcribed into `rb/abi/jni_env.h` *from a
  measurement* and cross-checked against the guest's encoding rather than reached for `jni.h`.
- A `jstring` handle is validated against the arena and its magic before being read. A handle is opaque
  to the guest, so what arrives is ours, corrupted, or a pointer to something else — and reading that
  last as text yields a plausible string out of arbitrary guest memory.
- Unbound/refused host calls are a **`Fault` naming the slot**. `CallGuest` still hands back an `r0`, and
  it is meaningless in that case — which is why one of my own assertions was deleted: it checked `r0`
  after a refusal, i.e. it encoded the exact confusion the design exists to prevent.

The oracle was extended to **build its own `JNIEnv`** on the tablet from the same header, so the offset
is verified against silicon and not self-certified: `jni_utf(1)=115 jni_utf(0)=114 calls=2
utf=retrobug`. If RetroBug's table put the function anywhere else, the oracle's stub would never fire.

**7c — the finding: Tier I executed both legs of every IT block.** `it_probe(7,3)` returned **1012**
where the same compiled Thumb-2 on the SM-X710 returns **42**. Across all six shapes the pattern was
exact: both `addgt` and `addle` ran and the last one won, then both `addlt` and `orrge` ran. So
`docs/32` §2.1 — which named IT-block handling the engine's single biggest correctness risk, on the
strength of 8,387 IT instructions counted in bionic — has now predicted a real defect and had it
confirmed by a measured value rather than by argument. After the fix all six agree:
`42 1008 42 104 1005 1000`.

The mechanism was missing entirely: Dynarmic's A32 translator splits an IT sequence into **one
predicated block per leg**, carrying `Block::GetCondition()` and
`Block::ConditionFailedLocation()` (`frontend/A32/translate/conditional_state.cpp:45-49`), and the
AArch64 back end branches at the entry label so a failed leg executes **no** instructions
(`backend/arm64/emit_arm64.cpp:205-217`). Tier I walked every block's instructions and consulted
neither. `ConditionFailedLocation` already has `AdvancePC`/`AdvanceIT` applied by the translator, so
following it is the whole fix and no IT bookkeeping belongs in the interpreter — which is also why
`itstate` provably cannot leak: it is re-read from the descriptor each time.

Two residuals from this, both for the differential rather than for argument:

- `[UNVERIFIED]` A predicated block whose flags were set by an instruction **in the same block** would
  read stale CPSR, because Tier I materialises flags into CPSR at each `SetCpsr*` and the JIT reads the
  host flags register instead. Every case seen so far has the predecessor write CPSR first; whether the
  translator can avoid splitting in some IT shape is a Tier J comparison question, and it is on the list.
- An IT sequence **straddling a block boundary** is still untested. It cannot be forced from C or from
  inline asm in a way that survives the translator's own splitting decision, so it needs either a
  hand-assembled image or an app that produces it. `docs/32` §5 keeps it as the top lane.

**Harness change that came out of two devices being attached.** `run_engine_tests.sh` now refuses to
guess: `RB_SERIAL` names the device, one device is auto-selected, more than one without it is an error,
and every run prints `model / abilist32 / sdk / page`. A green line means "green on *this* hardware",
which is the entire content of the claim — and a silently-chosen device is how an arm64-only phone would
eventually be reported as having run the ARMv7 lane.

`it_probe` also recorded a small lesson in why the oracle exists: hand-derived, I expected `it_probe(101,1)`
to return 136. The hardware says **104**, because `101+3 = 104` already has bit 5 set, so the `orrge`
leg is a no-op. An expectation computed by reading the assembly is not the same thing as a measurement.

## Step 8 — the app-domain probe: `execmem` is granted, and the low 4 GiB is not

Two of the plan's design-reversing risks (U-2, U-13) were blocked on the same thing: a measurement taken
*inside an installed app*. From `adb shell` the answer describes the `shell` SELinux domain, which is not
the domain RetroBug ships into, and both rows said so. The blocker was never elevation or the emulator —
`net start aehd` was the wrong door. It needed a device attached and an APK.

`tests/appdomain/` + `tools/app_domain_probe.sh`: an arm64 native library run by a minimal launcher
Activity, built with the no-Gradle chain docs/20 §1 proved out (`javac → d8 → aapt2 → jar → zipalign →
apksigner`), installed, launched, and read back out of `logcat`. Results are committed per device as
`tests/appdomain/results-<model>.txt`.

**Every "ok" in this file is an execution, not a permission.** Where a mapping comes back executable, the
probe writes a real `mov w0, #42 ; ret`, calls it under a `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGSYS` guard, and
reports what came back. A granted `mprotect(RX)` that faults on fetch is a completely different problem
from a refused one, and `mmap` alone cannot tell them apart. The `SIGSYS` case came from the AOSP
`SECCOMP_RET_TRAP` default — seccomp blocks kill the process rather than returning `EPERM`, so without it
a blocked syscall looks like "the probe stopped answering".

**U-13 — answered, positively, on two OEMs.** Inside `u:r:untrusted_app`, Android 16, `getenforce`
Enforcing, on both SM-X710 (Samsung) and CPH2573 (OnePlus/ColorOS): anon `RWX` executes; `RW` →
`mprotect(RX)` executes; `RX` → `RW` succeeds, so patching an existing block works; a `memfd` executes via
`RW`→`RX` and also mapped `RWX` directly. This corroborates §8.3's *policy reading* (`allow appdomain
self:process execmem;`, `appdomain_tmpfs:file { execute }`) with behaviour. **Tier J is not blocked on OEM
policy.** Xiaomi / vivo / Huawei remain unmeasured — the row says 2 of 5 rather than "solved".

**U-2 — answered, negatively, and it reverses AD-6's Android half.** A contiguous 4 GiB low window cannot
be obtained: `MAP_FIXED_NOREPLACE` `PROT_NONE` is `EEXIST` at `0x10000`, `0x10000000` and `0x80000000`, for
4 GiB down through 2 GiB, 1 GiB and 512 MiB. The reason is not policy. `/proc/self/maps`, printed by the
probe before it asks anything: 26 mappings, **1254 MiB occupied**, spanning `0x2000000..0xebad7000` — ART's
heap and the app's own mapped files sit exactly where the guest window would go. Largest free gap below
4 GiB: 1872 MiB; largest window actually resolvable there, found by bisection: **1856 MiB**. Individual low
pages map happily (4 KiB at `0x10000`, 4 KiB at `0x80000000`), so the wall is **contiguity, not
permission** — and `0x1000` is `EPERM`, which is `mmap_min_addr` and a different error with a different
remedy, so the two are reported separately rather than collapsed into "denied".

The old shell-domain "yes" was not wrong; it was a different process. AD-6 already made the startup probe
the authority, so the mechanism is unchanged and the expected answer is what moved: biased `GUEST_BASE` is
now the Android default, exactly as on iOS. The consolation is real rather than rhetorical — the 32-bit
handle table for host→guest crossing was already mandatory for iOS, so Android is now sharing machinery
that had to exist anyway, and a non-zero `fastmem_pointer` is one addition. What does change: a
`guest_base == 0` shortcut stops being a hypothetical iOS-only bug and becomes a defect hit on every
Android device, which is worth stating plainly where the original warning lived.

**Three bugs of mine, each caught by checking rather than by hoping.**

1. The first U-2 test asked with a *bare hint* and reported `ok addr=0x7b00000000` as a success. A hint
   without `MAP_FIXED*` is a suggestion; the kernel placed the window 480 GiB up and the test was
   measuring something else entirely. The corrected test insists, and prints where a hint-only request
   actually landed — "ok" at a high address is a *negative* result for identity addressing.
2. The gap search looked only *between* mappings, missing the head and tail of the range, which under-read
   the largest window.
3. The bisection could not terminate: the midpoint is rounded to 16 MiB granularity, so once `hi - lo`
   dropped below 32 MiB the rounded midpoint equalled `lo`, the map succeeded, `lo` never advanced, and the
   loop condition stayed true. No signal, no tombstone — the app just stopped answering, which on an
   unattended device is the least legible failure there is. Now it breaks on a step that makes no progress
   (the iteration cap stayed, as a bound rather than as the fix).

**Operational finding for M1, recorded because it will bite a runbook.** `adb install` on the CPH2573
blocks on an on-device confirmation and never returns — so an unattended session cannot install there, and
a script that does not bound the step simply stops doing anything. Every device step in
`app_domain_probe.sh` is now wrapped in a `timeout`, and the failure message says what it thinks happened
instead of blaming the device. Note that `timeout` cannot wrap a shell function (`timeout 30 adb shell …`
fails with "No such file or directory"), so the bound lives inside the `adb()` wrapper.

**What is still open here.** One device has the full low-window measurement (the OnePlus number could not
be taken without a tap, and its earlier run predates the bisection); three of the five OEM families in
U-13 have no data at all; and whether anything in the ART layout that causes this is version-dependent is
unanswered — it is measured on Android 16 on both devices, and the reservation is app-content rather than
policy, so it should get *worse* not better with a bigger runtime, but that is an inference and labelled as
one. `rb::diag` (C17) should record the probe's answer per launch, which is what turns these into fleet
data instead of bench data.

## Step 9 — Tier J exists, and the two tiers agree on numbers hardware produced

`30` §Step 7 left item 7's second half open — *"both tiers, identical state"* — because there was no second
tier. `core/exec/jit.cpp` is it: `JitExecutor` over `Dynarmic::A32::Jit`, and
`core/tests/exec_tier_diff_test.cpp` is the comparison. 65 checks, 0 failures, **identical output on both
devices** (SM-X710 and CPH2573), on top of dynarmic's own 200,561 assertions.

**Config, pinned for accuracy rather than for speed** (`[1A]`, read from `interface/A32/config.h` and set
line by line): `arch_version = v7` (the default is `v8`, and the difference is architectural, not cosmetic);
`always_little_endian`; `check_halt_on_memory_access = true`, so a fault can be raised between accesses
rather than at block end; `enable_cycle_counting = true` because that is Tier J's only progress counter;
`code_cache_size = 8 MiB` (the documented minimum — sizing is C04's job and this leaves it there); and
**no `fastmem_pointer`, no page table**. The last one is the design decision that makes the test worth
running: every guest read and write from compiled code goes through the same `GuestMemory` the interpreter
uses, so a disagreement can only come from the back end, not from two different memory models. What it
costs is a second pass at speed — fastmem is the point at which Tier J earns its keep, and it is C03/C04
work, not this.

`[INFER]` **The step counts are not a speedup and are now labelled as such.** Tier I advances per Dynarmic
*IR operation*; Tier J advances per *guest instruction* (one tick per fetched encoding). `fib(20)` printed
`735,946` against `160,005` and the ratio is IR density, not throughput. `RunResult::instructions_executed`
carries the warning where the interface can't be misread, and unifying the unit is a C02 item that has to
land **before** C14/C15 put a per-app budget on top of it — a budget whose meaning depends on whether the
code got hot enough to compile is a budget an app escapes by being hot.

**The comparison is over every field.** Registers, all 32 `d` lanes, `cpsr`, `fpscr`, `tp`, `pc`, `mode`,
`itstate`, and the stop reason — unfiltered, because a tier that disagrees about a VFP lane nothing read is
invisible unless it is looked for. All 15 shapes carry hardware values from `tests/corpus/oracle-armv7.txt`
(six `it_probe`, six `fib` including `fib(20)=6765`, three `wide_probe`), so a shared bug in both tiers
would still show up as a disagreement with the tab.

### Three findings, each one only available because two tiers could finally be compared

**1. `GuestMemory::MapFile` kept the caller's file descriptor instead of duplicating it.** `Page::fd` is
re-read by every later re-granulation, and a loader that closes its fd once the mapping exists is doing
nothing wrong — the mapping holds its own reference. First symptom: `Protect` on a loaded module's text
page failing with `Bad file descriptor (errno 9)`, hundreds of lines and one function away from the cause.
`dup()` on entry, owned and closed with the instance, and the file-backed memory case now closes the
caller's fd immediately to keep the contract honest. `[1A]` **Negative control run**: reverting the `dup`
fails 12 checks across both host page shifts, with the reads after the failed re-map faulting on memory
`Protect`'s error path had already dropped — the mapping does not half-work.

**2. Dynarmic caches the block it builds for an address it cannot read.** A fetch at an unmapped guest
address reaches `MemoryReadCode` *once*; the second time through the same code the answer comes from the
cached block and the callback never fires, so Tier J ended its run with no reason at all. The fix is
`SurfaceNonCodeDestination()`, called from `MemoryReadCode` **and** from
`ExceptionRaised(NoExecuteFault)`, with `SetPending` keeping the first reason recorded so a later, vaguer
one cannot overwrite it. `[1A]` Both paths are exercised: the tier-diff suite's unmapped-fetch case now
faults in both tiers with a message from each.

**3. The invalidation test asserted something it could not fail.** Its first version flipped a bit of
`it_probe` and required the answer to change. It did not change, and the reason is in `docs/32` §2.1's own
subject matter: **inside an IT block the legs are encoded unpredicated** — the condition lives in CPSR's IT
field — so `addgt r0, r2, #3` and `addle r0, r2, #5` are just two immediates, and the bit that was being
flipped was inert. `[1A]` Measured, not recalled: `#3`→`0x1cd0`, `#5`→`0x1d50`, `#7`→`0x1dd0`, while `#9`
leaves the narrow form entirely (`f102 0009`), which is also why the pair the test uses is 5 and 7 — 9 and
5 are four bytes apart, not one.

The replacement is the shape the original should have been. `patch_probe` and `patch_probe_b` in the fixture
differ in **exactly one byte** (checked at run time before anything relies on it, so a toolchain that
changes its encoding choices fails the test instead of silently voiding it), hardware has measured both
(`patch_probe: 1008 42 1005`, `patch_probe_b: 42 42 1007`), and the patch makes the first function's bytes
*equal* the second's — verified by comparison, not by inference. The expectation after invalidation is then
a hardware number, which a stale cache cannot produce. And the test asserts the premise:

```
[diff] patched, not yet invalidated: I=42 (want 42)  J=1008 (1008 means still cached)
[diff] patched probe, one byte at +8: before=1008 after I=42 J=42 want=42
```

Tier J answering the *old* 1008 before `Invalidate` is what makes the second line evidence about
invalidation rather than about two tiers that never cached anything. If a future back end reads 42 there,
the test should be deleted, and it now fails loudly enough to force that conversation.

### What Tier J does not do yet

- **TLS.** Dynarmic models no `tpidrurw` anywhere in its tree `[1A]`, so `tp` is a `MachineState` field
  nothing in compiled code can read or write; it needs a `Coprocessor` hook on `c13, c0, 4`. The guest's
  expectation is real — the tab's 32-bit process reads `tpidrurw` as `0xf55affe0` while `midr`/`ctr`/
  `mpidr`/`fpsid` all raise SIGILL (`oracle-armv7.txt`, reconfirmed this step). Until then, any bionic
  code that touches `__get_tls()` misbehaves in Tier J and *also* in Tier I, which is why this is a gap
  rather than a difference.
- **`GetGEFromOp` / SIMD32** (`docs/32` §5.2's two demanded-without-handler rows) — Tier I refuses them and
  Tier J has the same hole via the shared front end.
- **An IT sequence straddling a block boundary** — still untested; forcing one needs a branch inside a
  block whose split point is chosen by the translator, not by the test.
- **C04's own job.** Tier J runs on *Dynarmic's* cache. `rb::codecache` — instance tagging, eviction,
  per-app limits, the invalidation bookkeeping above moved out of the executor — is unbuilt, and until it
  exists the "two address models, one cache" claims in `docs/10` §8.2 are design, not behaviour. Worth
  stating precisely because `docs/10` §13.1 lists the host-cache discipline as platform glue: the
  `dc civac` / `ic iallu` / `isb` sequence is **Dynarmic's and stays Dynarmic's** — `JitExecutor::Invalidate`
  calls `InvalidateCacheRange` and touches no host cache itself. C04 therefore owns *policy* (what to drop,
  when, on whose behalf), not maintenance, and the row in §13.1 needs no correction.
- **16 KiB host pages.** The x86_64 emulator lane has not been re-run since Tier J existed; the ARMv7 guest
  is 4 KiB-granular either way, but `AddressModel` and the granule tests are the parts that would notice.

**On the lesson rather than the findings:** twice now the failure has been an assertion that could not fail
— `|| true` in Step 7, a patch that patched nothing here. Both were invisible in a green run, both were in
code written to be rigorous, and both were only caught by asking "what value would this line print if the
thing it tests were broken?" The differential makes that question cheap to ask, which may be the best reason
to have built Tier J when the plan said "later".

## Step 10 — `rb::diag`: the rule is "nothing is applied without an echo", so make discarding impossible

C17 is the component the plan leans on to turn every other component's *"must be reviewed"* line into
something mechanical: `docs/20` §C17 says each of those lines is expressed as a diag field, and `docs/10`
§12.3 names the pattern (Xenia's Vulkan diagnostics, audited in `docs/research/08`): an ordered set of
stages, **the report rewritten in full after every append** so a hang or a crash mid-initialisation leaves
the last stage visible, and `>>> INITIALIZATION FAILED AT: <stage> <<<`.

The audit's own bill is the reason the API is shaped the way it is: six silent-ignore interfaces in that
codebase — a wrong TOML section, restricted entitlements that were silently inert, a patch DB's filename
rules, a driver fallback nobody was told about, an evidence file deleted by the reset that produced it, and
a `buildTypes.debug.packaging` block "silently ignored by AGP 8" — and "every one cost a debug session".
That wording is what `docs/research/08` says; where `docs/20` had been citing it as *"the most expensive
lesson"* it now cites the list, because the superlative was mine and the source makes no ranking.

`core/include/rb/diag/diag.h` + `core/diag/diag.cpp` + `core/tests/diag_test.cpp`: 105 checks as written
here, 108 now that §Step 11's `syscall-policy` field exists. 0 failures on both devices, no fixture and no
guest program — the recorder is pure string work, which is also the
point: what reports on a launch must be testable without one.

**Three decisions that carry the rule, each with a test that fails if it is removed.**

1. **`Record` cannot discard.** A record missing its stage, its requested side, or its active side — or an
   `inert`/`failed` outcome with no reason — is stored anyway as a *failure naming the field whose echo
   went missing*, and it sets the banner. The tempting implementation, `if (!valid) return false;`, is the
   bug: it is a silent-ignore interface in a component whose subject matter is silent-ignore interfaces.
   A caller that ignores the return value still gets a legible report, which is the only way "mandatory
   echo" survives contact with a caller in a hurry.
2. **A failed echo does not satisfy a requirement.** `MissingRequired()` ignores `kFailed` entries, so a
   field cannot be closed out by its own complaint. Required fields come from a static set — code tier,
   address model, guest base, code-cache backend, both page sizes, isolation entry — plus whatever
   `Require()` demands at runtime (a profile that declares patches makes `patch-match` required). The two
   OEM probes are deliberately *not* in the static set: they are Android-shaped questions, and `core/` may
   not encode one platform's answer set as a universal requirement; the Android glue asks for them, which
   is also what turns "this build forgot to probe" into a report line.
3. **`EnterStage` is an append.** It stamps the stage and flushes *before* the work, which is the whole
   reason the rewrite-in-full cost is worth paying. `EnterStage("")` stamps nothing rather than un-stamping
   the real one.

Two smaller shapes worth naming: the **first** failure keeps the banner (a later, vaguer one must not
overwrite what actually went wrong — the same rule `JitExecutor::SetPending` reached independently), and
rendering caps each value at 512 bytes with the true length in the marker while `entries()` keeps the real
string — the *report* is the bounded artifact, since it is what gets copied out of a UI, and the
programmatic view should not be a truncation. The recorder has no clock and no launch id, on purpose: two
launches that did the same thing must render byte-identically, which is what makes the determinism test and
any future "diff these two launches" UI feature possible. A caller that wants a timestamp puts one in a
record's evidence.

**A sink failure reports itself one flush late** (`written = FAILED n times: …`) — it cannot be inside the
payload that just failed to be written, and saying so beats retrying quietly. That is also the only
deliberate inconsistency in the file format, so it is stated in the header rather than left to be
discovered.

**M1's exit criterion is now an assertion.** "`rb::diag` shows active tier + address model + code-cache
backend" was a sentence about a UI; it is now `MissingRequired().empty()` over those fields, checked in
this suite, and `unrecorded = …` is the line a support report shows when they are not there.

**What is left for the glue to do, and is not done here.** Nothing populates a `Recorder` yet: the launch
path has to call it from the address-model probe (AD-6), the app-domain probes (U-2/U-13, which `30` §Step
8 asks be recorded *per launch* so bench data becomes fleet data), the code-cache backend selection (§8.3's
"probed, never assumed"), the T0/T1 transition, and C14/C15's profile overrides. Each is a one-line
`Record` at the place the decision is already made — and each one that is forgotten will show up as an
`unrecorded` name rather than as a plausible-looking report, which is the asymmetry this component was
built to create. The privacy surface is also still unstated by any doc: the report is copyable from the UI,
so *what may not go in it* wants a decision before M1's export button exists, not after.

## Step 11 — the SVC channel, what it found in Tier I, and a syscall ABI that is not the textbook one

C07 cannot be built on guesses about what a guest syscall looks like, and the first thing it produced was
two Tier I bugs plus three measurements that contradict things written down in this project's own docs.

### 11a — the channel, and the latent PC-write bug

An SVC has to reach the monitor with the same frame from both tiers. Tier I notes Dynarmic's
`A32CallSupervisor` and surfaces it at the block's `CheckHalt` terminal — which is where Dynarmic's own
back ends test the halt flag, and where the translator has already put `BranchWritePC(pc+n)` — so both
tiers stop *after* the instruction. Stopping mid-block would have given the two tiers different resume
addresses for one instruction.

Then the differential disagreed, and the cause was in Tier I. Dynarmic's `BranchWritePC` masks its own
target and emits `A32SetRegister(PC)` (`frontend/A32/a32_ir_emitter.cpp:101-110`); only `BXWritePC` is the
interworking one that reads bit 0 as the ISA tag (`backend/arm64/emit_arm64_a32.cpp:551-577`). Tier I
applied the `BX` rule to **every** PC write. `bx lr` and `ldr pc, [sp]` all arrive with a tag-bearing
value, so fifteen differential cases, three hardware curves and 8,387 census instructions never reached the
difference — and the first guest construct that does is a syscall continuation at an even address, which
Tier I read as "switch to ARM" and then decoded its own Thumb bytes with the ARM decoder, reporting an
exception at an instruction that had executed correctly moments earlier. `[1L]`

The same run exposed a representational inconsistency: CPSR bit 5 *is* the mode, Tier J reads its mode out
of that bit and Tier I carries it on the location descriptor, so Tier I returned `cpsr = 0x0` with
`mode = Thumb` — a struct containing a contradiction, and one guest-visible difference waiting for the
first `mrs r0, cpsr`. Both tiers now normalise it through one `CpsrWithMode`, following the `ItStateFromCpsr`
precedent; what neither tier models is an `MSR CPSR` that writes bit 5, and that is named in the comment
rather than assumed away. `[INFER]` (no test reaches it yet).

`svc_probe` in the fixture keeps `r7` (248 = `__NR_exit_group`, read from the NDK's own
`arm-linux-androideabi/asm/unistd-eabi.h:214`) different from the encoding's immediate (0x9c), because
bionic uses `svc #0` and a probe that conflated the two would pass every test anyone thinks to write. The
resume is proved by guest arithmetic: `adds r0, r2, #1` after the SVC, so `41 → 42` only if execution
continued at exactly the right address — two halfwords early runs the tail of a `movw` as code, two late
gives 43. `[1A]` Tier diff is 80 checks with both tiers identical at the stop *and* after the resume.

### 11b — measuring the ABI instead of recalling it

`tests/corpus/syscall_abi.c` + `tools/syscall_abi_probe.sh`: one source, built as `armeabi-v7a` and
`arm64`, run on the Tab S9, and the guest/host map **computed** from the two outputs rather than typed.
Three results, and each one changes something:

1. **Every shared syscall number differs.** `read` 3↔63, `write` 4↔64, `close` 6↔57, `openat` 322↔56,
   `gettid` 224↔178, `exit_group` 248↔94 — fourteen shared names, fourteen divergences, plus `mmap2` and
   `sigaction` and the three `__ARM_NR_*` with no host counterpart at all. `[1L]` So "translate by name,
   never reuse the number" is not a guard against an occasional clash; a forwarded call that skips the
   translation is wrong every time.
2. **Errno values are identical — and that contradicts this project's own invariant.** `docs/20` §C07 says
   guest errnos are ARM-EABI numbers "not host ones", which is the received story (`ENAMETOOLONG` 62 vs
   36). All 47 sampled names agree between the ABIs, and the discriminating case was executed rather than
   looked up: `openat` of a 300-character name from a real 32-bit process returned **-36**, so this kernel
   already speaks bionic's numbers. The *rule* survives — resolve by name, keep one function for the
   translation — but the premise as written is false for the pair RetroBug actually runs, and `docs/20`
   now says so where someone would otherwise re-assume it. `[1L]`
3. **CPSR.C is not the error flag on this kernel.** The textbook ARM EABI return is "negative errno in r0,
   C set; success clears C" — and that sentence was in the probe's own header comment *before* the probe
   ran, which is exactly how an assumption becomes documentation. Measured: `getpid` → C=1, `sched_yield`
   returning **0** → C=1, both failures → C=1. `[1L]` So the flag carries nothing, the discriminator is the
   `-4095..-1` range, and `ApplyReturn` writes r0 and leaves CPSR alone. Had the comment been trusted, every
   guest stub that tests the flag would have disagreed with real hardware.

Two tooling mistakes in that step, both of the "green and wrong" kind: the probe's `%-18s` padding meant a
`grep` expecting one space after a name matched only the names long enough not to be padded, so the first
version of the map reported *one* divergence out of fifteen and looked plausible (`__NR_clock_gettime` is
exactly 18 characters; `EPERM` is 5); and the first `openat` failure case passed a null pathname, which the
kernel rejects with EFAULT before it ever validates the dirfd — so it "measured EBADF" while measuring
something else. Both are fixed where they were, and the second is why the probe now passes a valid pointer.

### 11c — the monitor, with an allowlist that is empty on purpose

`core/include/rb/syscall/monitor.h` + `core/syscall/monitor.cpp`, 77 checks, green on both devices.
`Frame::Capture` (masked r7, the immediate kept separate, r0-r5, the PC), `Outcome`, `Verdict`,
`LooksLikeError` as the discriminator, and handlers for what can be answered with components that exist:
`getpid`/`gettid` from ids the launcher supplies (refused rather than invented, and refused rather than
asked from the host kernel), `exit_group` as a verdict, `arm_cacheflush` handed to an injectable invalidator
— the AD-7 self-modifying-code story reached through the *guest's own* instruction rather than a host call —
and `arm_set_tls`/`arm_get_tls` through `MachineState::tp`, which is the syscall half of TLS while the
`mrc` half waits on a `Coprocessor` hook.

The forwarding allowlist is empty and `Verdict::kForward` is a state with no rows behind it, so the posture
"is data, and the data is empty" rather than "no such concept". `Monitor::Echo` writes it into C17's new
`syscall-policy` field — the first component to populate a recorder — as
`requested = no forwarding: nothing reaches the host kernel` / `active = … the guest asked for getpid x1,
openat x2`. Refusals name the missing component (`openat is not forwarded: this build has an empty
allowlist and no handler yet`), because a bare ENOSYS in a support report describes a gap nobody can triage.
Unknown numbers answer `-ENOSYS`, which is also what the measured device kernel answers for an r7 above its
table, so at least the shape of a refusal matches silicon. `[1A]`

### 11d — the loop, and a fixture bug both tiers agreed about

`AbiContext::CallGuest` now services `StopReason::Syscall` the way it already services `HostCall`: capture,
dispatch, resume — with `Verdict::kExit` ending the call in a new `StopReason::Exited` (appended to the
enum, because its numeric value appears in differential logs and renumbering it would make old logs and
new code disagree about what `reason 6` meant), a missing monitor ending it in a `Fault` that says so, and
`kMaxSyscallsPerCall` bounding a guest that spins on a refused call.

Two new cases, and the interesting one is the failure that came first. `syscall_add` asks for `getpid` and
returns one more than the answer, so the value asserted (4243 from a configured 4242) can only be reached
if the monitor's return actually landed in the guest's own arithmetic; a monitor that never ran, or ran and
whose answer was dropped, produces something else. `exit_group` ends the call.

`flush_probe` is AD-7 told from the guest's side, and it was wrong before it was right. The first version
of the case failed with both tiers handing the monitor **the same wrong number** (`nr:0x2` where
`__ARM_NR_cacheflush` is 0xf0002) — and two tiers agreeing is the signal that the bug is in the test, not
in a back end. It was in the fixture: `movt` writes the top *halfword*, so `movt r7, #0xf000` after
`movw r7, #2` builds 0xf0000002, which the kernel's r7 masking — faithfully reproduced in
`Frame::Capture` — folds down to 2. The same masking that makes a malformed guest number harmless made a
malformed *test* look like an emulator bug. `[1L]`

With that fixed, the case is the strongest self-modifying-code statement available without a guest that
writes its own text (it cannot: the page is not writable, which is the design): the host patches a byte
through the same `Protect` transition a guest `mprotect` takes, Tier J answers the *stale* value, a guest
flush of an **unrelated** range leaves it stale — which is what proves the range travels rather than "any
cacheflush clears the world" — and then the guest's own `__ARM_NR_cacheflush` over the patched address
makes Tier J answer the number hardware measured for the other probe, identical to Tier I:

```
[diff] guest-issued cacheflush: stale=1008 then I=42 J=42 want=42 (arm_cacheflush x3)
```

Twelve lanes, 1,022 engine checks, 0 failures on the Tab S9; dynarmic's 200,561 assertions unchanged.

### What this does not do yet

* `/proc/self`-shaped identities, `clock_gettime`, and anything with an errno that is *policy* rather
  than a gap — the monitor answers `ENOSYS` for everything it has no handler for, which is a decision the
  launcher can revisit per app but is not one this component should make up.
* `docs/20` §C07's review focus is answered for `execve` (`EACCES`, measured in the app domain in §Step 12,
  and implemented as the monitor's first `kDenied` row) and still open for `fork`/`vfork`/`clone`, which
  cannot be asked without forking inside the probe. What §Step 12 did establish about them is the
  precondition: the app-domain filter does not `SECCOMP_RET_TRAP` these calls at all, so "the platform
  refuses it" is not available as an answer and every refusal has to be RetroBug's own. `[UNVERIFIED]` for
  the three calls, `[1L]` for the filter.
* Signals, `clone`/`futex`, and the 32-bit `ucontext` layout: untouched.
* Forwarding rows, and with them the first thing that must not be able to escape C10's resolver. One
  monitor per guest instance, which is already how `Monitor` is written.

## Step 12 — the probe runs in the guest's own ABI, and answers three questions the arm64 build could not

`tests/appdomain` grew a second build of the same source: `armeabi-v7a`, installed as its own APK after the
arm64 round, reporting into `results-<model>-armv7.txt`. The reason is that three of this project's open
questions are not questions about an arm64 process pretending to be a guest — they are questions about what
a 32-bit ARM process may do:

```
# device=SM-X710 … apk=probe32.apk
java: … nativeLibraryDir=/data/app/…/com.retrobug.probe-…/lib/arm …
selinux=u:r:untrusted_app:s0:c8,c258,c512,c768
build_abi=armeabi-v7a (thumb-2, the guest ISA)
```

`nativeLibraryDir` and `build_abi` are two independent statements that the process was 32-bit, which
matters because `uname -m` says `armv8l` for exactly that process — the machine string describes the
kernel, not the personality. Without those two lines a report cannot be attributed to an ABI at all.

**U-13, for the guest ABI: answered, positively, on the SM-X710.** Anon `RWX` executes; `RW` →
`mprotect(RX)` executes; `RX` → `RW` succeeds; a `memfd` executes via `RW`→`RX` **and** mapped `RWX`
directly — every one of them by calling a real `movs r0, #42 ; bx lr` and getting 42 back, under the same
`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGSYS` guard. So the code-cache permission answer does not depend on which
ABI the process is running, at least where both answers are available; the arm64 report is otherwise
byte-identical apart from uids, pids, SELinux categories and mapping addresses. **One device, one OEM
family** still — the OnePlus 12 cannot answer this half at all, which is the standing fact that makes the
pair worth having: `abilist32` empty means the 32-bit round is *skipped and says so*, not silently passed.

**`tpidrurw` is readable from an app in the guest ABI**: `mrc p15, 0, rX, c13, c0, 3` returned
`0xef484be0` inside `u:r:untrusted_app`. `[1L]` That closes one of Step 5g's questions — the read is not
blocked in the domain RetroBug's guests will be in — and sharpens the other: the platform permits the
guest to read *its own* TLS, so the emulator's job is not to allow it, it is to **supply a value that
means something**, which is the `Coprocessor` hook `30` §Step 9 says is missing. `tp` travelling on
`MachineState` already makes `__ARM_NR_get_tls`/`set_tls` work; the `mrc` path is the remaining half.

**Seccomp: the app filter did not trap *any* of the twelve numbers asked, and several demonstrably reached
the kernel body.** `[1L]` SM-X710, Android 16, `u:r:untrusted_app`:

| r7 | call | result | why that is the kernel's answer, not a filter's |
|---|---|---|---|
| 0x14 | getpid | 2412 | control: the harness works |
| 0x182 | bpf | `-E2BIG` | `size=0` is checked inside `__sys_bpf`; a filter would not have let it get that far |
| 0x88 | personality | `8` | `PER_LINUX32` — the process's own personality, which only the kernel knows |
| 0xb | execve | `-EFAULT` | argument copy, with a null filename |
| 0x179 | process_vm_writev | `-EINVAL` | body-level argument check |
| 0x177 | setns | `-EBADF` | fd validation happens in the syscall body |
| all twelve | — | none trapped | no `SIGSYS` anywhere, so no `SECCOMP_RET_TRAP` for these |

The consequence is for `docs/10` §10.2, which says RetroBug installs its own TSYNC'd filter "denying
`execve`/`ptrace`/`process_vm_*`/`unshare`/`setns`/`perf_event_open`/`userfaultfd`/`bpf`/`socket` outside
AF_UNIX" and cites AOSP's "filters only ever tighten". Both statements stand, but the measurement removes a
third one that was never written down and would have been assumed: **the platform's app filter is not
doing that denying on this build.** For a 32-bit app process here, those calls are available, so the
enforcement RetroBug designs is the only enforcement there is — which is a stronger argument for the T1
filter, not a weaker one, and it is worth saying in exactly those terms where the filter is specced.

`fork`/`vfork`/`clone` are deliberately *not* in the list. They were asked in the previous step's
shell-domain attempt, `fork` succeeded, and the child inherited the parent's unflushed stdio buffer, so
every line printed before it appeared twice — see the standing facts. Asking a call that duplicates the
report in order to find out whether it is blocked is a bad trade when `bpf` and `personality` answer the
same question without side effects.

**`execve` has a measured refusal errno now: `EACCES`.** The `docs/20` §C07 review focus asks for "the
same errno a real 32-bit Android would give", and the honest answer to that has to come from a real 32-bit
Android in the app domain, because the *interesting* part is not the syscall but the policy behind it. The
probe writes a file into its own `cache/` (the package name read out of `/proc/self/cmdline`, so nothing is
hardcoded), then `execve`s it with valid `argv` **and** `envp`:

```
r7=0x0000b execve(own file) <- the policy answer RETURNED r0=0xfffffff3 (-13) C=1
```

`-EACCES`, which is `app_data_file:file execute_no_trans` doing what `docs/10` §13's row says it does.
`rb::syscall` now answers `execve` with `Verdict::kDenied` and that errno, with the measurement named in
the `detail` — first use of `kDenied`, and the distinction the guest sees is the point: `ENOSYS` would
tell an app developer "this emulator cannot do it", which is false, while `EACCES` tells them "Android
will not let you do it, anywhere", which is what was measured.

**Two mistakes the harness made before the device did.** The first `execve` attempt passed only two
arguments and left `r2` holding whatever the compiler had there — an invalid `envp` pointer — so the kernel
answered `EFAULT` about an argument instead of `EACCES` about the file, and the row looked like a policy
answer while measuring something else. And the low-4 GiB battery, built for 64-bit addresses, compiles in
the 32-bit build with `uintptr_t` at 32 bits, where `4u * kGiB` overflows to zero and the final test
compares against a constant that is now smaller than every address: it would have printed **"guest 32-bit
space reachable without a bias: all of it"**. Both are fixed by *removing* the code from that build rather
than by making it print the right thing, because a tautology in a measurement file is worse than a gap — it
reads like a result. The 32-bit report says what is skipped and why, in the output, next to the totals.

**Still unmeasured, and the shape of each experiment:** whether the filter traps anything at all for a
number not in that table (`ENOSYS`-vs-`SIGSYS` for one known-blocked call would settle the filter's action,
but no such call is known yet — which is itself the finding); the same battery on Xiaomi/vivo/Huawei; the
32-bit half on a *second* 32-bit-capable device, since there is exactly one here; and whether an app can
read `enforce` at all (it cannot — `selinux_enforce=(unreadable)` in both reports, so "Enforcing" remains a
`getenforce` reading from the shell domain next to an `untrusted_app` context read from inside the app,
which is two measurements joined by an inference and is labelled that way in `docs/10` U-13).

## Step 13 — `rb::codecache` (C04): the policy half, with the memory left to the platform

`core/codecache/codecache.cpp` + 86 checks. What landed is *who owns which block, and what gets dropped
when there is no room* — not an `mmap`. The executable memory is behind an injected `Allocator`
(`Reserve` writable → `Commit` executable-and-smaller → `Release` the whole reservation), so the three
backends `docs/10` §8.3 names — `AnonRwx`, `MemfdDualAlias`, `Interpreted` — arrive with M1's platform
glue where the U-13 probe answer is actually consumed, and the W^X ordering is testable here without any
memory being mapped at all. `[1A]`

**`BlockKey` is `{instance, image, pc, mode}`** and every field is load-bearing, with a test per field.
`instance` closes the hole `docs/research/05` §2.9 names — a cache looked up by guest PC alone serves one
app's compiled block to another, which would break the "no isolation property is shared across apps" rule
through a hash table that forgot a member. `image` because two modules map the same guest address (the
fixture's own exports and `liblog`'s stubs already do). `mode` because getting it wrong is the failure that
does not fault: an ARM-mode fetch served a Thumb block *decodes*, and the guest runs the result.

**`kInterpreted` is a shape, not a failure.** It returns a slot with no range and reserves nothing, so
Tier I runs with no cache — on iOS, and on any OEM where the probe says no. A cache that reported
`kNoRoom` there would send a caller into an eviction loop over a table it never filled, which is how a
fallback stops being takeable.

**`FlushInstance` and `Forget` return guest ranges instead of invalidating anything.** Step 9 established
that the host-cache maintenance is Dynarmic's and C04's job is policy; that split is now in the signature
rather than in a comment. `Instances()` exists so "FlushInstance is provably called on every T1 exit
path" is checkable against a list rather than against hope — the *proving* is C16's, which does not exist
yet.

**The state machine, found by two of my own tests failing.** An entry is created at `Reserve`, and until
`Commit` it has no executable bytes. That has to be a real state, not an entry with a size in it:
`Find` returning an uncommitted block would hand a caller a writable range to branch into, and
`EvictOne` choosing one would `Release` memory out from under a back end mid-write — the symptom of which
is one app executing another's code. Both were wrong in the first version (the entry stored its
reservation as "used", so an uncommitted block was evictable and findable), and both were caught because a
test asked for the opposite. `Find` now refreshes recency — hence non-`const`, which is honest: a lookup
that pretends not to change anything would evict the block a hot loop is sitting in.

**`Echo` fills C17's `code-cache-backend` field** with `requested`/`active`, and records `inert` when a
stronger backend was asked for and a weaker one is in use, with the reason in `evidence` — the
requested-`MemfdDualAlias`, active-`Interpreted` case is a test, not a hypothetical.

**One operational lesson about test harnesses.** This lane's first run was `Segmentation fault` with
nothing printed: an allocator-fixture bug emptied the cache, the assertion on `dropped.size()` reported it,
and the *next line* indexed `dropped[0]` anyway. A lane that crashes tells you less than a lane that
fails, so the case list now prints the case name as it starts, and index-after-size-check is guarded. The
same pattern as Step 9's "what value would this line print if the thing it tests were broken?", on this
side of the boundary.

**Not done, and not pretended done:** no `AnonRwx`/`MemfdDualAlias` allocator (M1's, next to the probe
that selects them); nothing wired into `JitExecutor`, which still runs on Dynarmic's own cache and whose
`Invalidate` is still what a flush has to reach; no thread-safety, so one `Cache` per instance *and per
thread* until C16's threading story exists; and the eviction *policy* has no measurement behind it yet —
LRU-with-per-instance-quotas is a reasoned choice, `[INFER]`, and the experiment is a real app's block
access trace against `bytes_per_instance` at 1, 4 and 16 MiB, which also closes C04's sizing question.

## Step 14 — the guest-library sweep, and the §1 paragraph it falsified

M0's last unstarted row was **"Audio proxy strategy locked"** (`20` §5). It was the cheapest row in the milestone —
one `llvm-readelf` sweep over the NDK's AArch32 stub dirs plus a header reading per library — and the most
consequential to get wrong, because its output is the C11/C12/C13 work list. Delegated to a sub-agent while Step 13
was being written up, on the grounds that a wide file-tree enumeration is exactly what a second pair of hands is for.

**Result: `docs/33-guest-library-inventory.md`, 27 libraries, and §1's conclusion falsified rather than confirmed.**
The claim under test was `20` §1's "*audio is the **only** category where the generator approach breaks down*". The
sweep found nine more: `libandroid` (`ANativeActivityCallbacks`, 15 fn ptrs in a struct the guest calls through),
`libmediandk` (a **struct of 4 fn ptrs passed by value**), `libz` (`z_stream` inline-allocated by the guest, plus
`(int)sizeof(z_stream)` as a runtime argument), `libvulkan`, `libdl`, `libEGL` (one symbol: `eglGetProcAddress`),
`libaaudio`, `libcamera2ndk`, `libbinder_ndk` — and that `20` §C13 already contradicted §1 *inside the same file*,
since C13 hand-models input as well as audio.

**Its numbers were re-measured here before any of it was written down**, because a report is a claim:

| Checked | Sub-agent | Measured here |
|---|---|---|
| `34/libc++.so` is not a stub | 19 bytes, `INPUT(-lc++_shared)` | **identical**, `cat` of the file |
| libraries in the newest API dir / union across dirs | 26 / 27 | **26 / 27** |
| `libOpenSLES.so` FUNC / OBJECT | 3 / 53 | **3 / 53** |
| `libmediandk.so` FUNC / OBJECT | 161 / 143 | **161 / 143** |
| `AMediaCodec_setAsyncNotifyCallback` by-value struct | `NdkMediaCodec.h:145-150`, `:499` | struct of 4 fn ptrs at `:150-154`, call at `:499-502`, signature takes it **by value** |
| `deflateInit_` passes the guest's `sizeof` | `zlib.h:1856-1864` | `deflateInit_(strm, level, ZLIB_VERSION, (int)sizeof(z_stream))` at `:1856`; `zalloc`/`zfree` in the struct at `:124-143` |

Two line-number drifts, no substantive disagreement. What the sweep *cannot* close it says so about: nothing was
executed, so every classification is **[1L]** for "the header and the symbol table say this" and **[UNVERIFIED]**
for "a generated stub would actually fail this way". The `z_stream` case is the sharpest — `Z_VERSION_ERROR` on a
32/64 `sizeof` mismatch is a header reading, not a crash anyone has seen — and its experiment is M1's: generate one
`libz` proxy and call `inflateInit_` from a guest program. The fallback if it turns out fine is the cheap one
(forward it and mirror the struct); the fallback if `dlsym`/`eglGetProcAddress` turn out to be the only
pointer-returning hazards is to special-case five symbols, which `docs/33` §4 item 2 already isolates as a
generated-code case for exactly that reason.

**Reconciled, not overridden** (brief: a Phase 0–4 finding that conflicts with something committed gets flagged and
reconciled): §1's measurements were kept and its generalisation replaced with a block quote naming what was wrong and
where the rule now lives; §5's row was marked done *with the falsification in the cell*; `20` §C11's two buckets were
corrected in place — `libc++` out of "symbol authority", `libz`/`libmediandk` out of "generated proxies", and the
FUNC-only filter called out as emitting half a library (`libOpenSLES` 3 of 56). The 53 `SL_IID_*` are the reason that
matters rather than tidiness: they are *inputs* to the vtable lookup, so item 1 of the hand-modelled list cannot even
be tested until the generator emits `OBJECT` symbols too.

## Step 15 — `rb::profile` (C14): the three mistakes that had to become types

`20` §C14 is the last core component M0 asks for, and the one whose acceptance criterion is unusual: its invariants
are *impossibilities* ("there is no overload accepting an un-namespaced key"; "`tier` and `patches` … may not
reference each other"; "a T1 row is created from probes, **never copied from** a T0 row"), which a runtime test can
only demonstrate by refusing a call it was able to make. `core/include/rb/profile/profile.h`,
`core/profile/profile.cpp`, `core/tests/profile_test.cpp`, 133 checks.

**The static_assert that was decorative until it wasn't.** The first draft constrained `Store::Get`/`Set` with
`static_assert(IsProfileKey<K>::value, ...)` *inside the template body*, and a test asserting
`!CanLookup<std::string, Name>` — which failed to compile in the wrong direction. A body-level assert is only checked
when something instantiates the template, and a `requires`-expression does not instantiate: it only asks whether the
name resolves. So `Get("com.foo", name)` was *valid* and the rule was a convention with a comment on it. Moving the
constraint to `template <ProfileKey K>` (a concept over the closed `IsProfileKey` trait) makes the expression itself
ill-formed, which is what the static_assert needed in order to mean anything. The lesson generalises to any "the type
system forbids it" claim in this project: **the test must be `!predicate`, and if `predicate` can be formed at all,
the mechanism is not doing the work.**

Then a second hole of the same kind, found while writing the tests rather than by them: `Effective(row, name)`,
`SetRow`, `LoadTomlRow` and `EchoRow` were public, so the row *string* — the thing the key types exist to make
untypable — was an entry point. Making every public method a `template <ProfileKey K>` forwarder to a private
row-string half closes it. That one cannot be `static_assert`ed from outside (a `requires` that touches a private
member is a hard error, not substitution failure — which the build demonstrated by failing), so it is recorded in the
test as a named review point instead of pretending to be covered.

**The schema, and the three rows that are decisions.** `tier` defaults to `normal` and is **not** probe-supplied: a
probe allowed to answer "this device looks safer isolated" is the silent fallback AD-18 forbids, arriving through the
most plausible-looking channel in the system. `code_tier` defaults to `interp`, because with no probe and no override
the tier that cannot fail to translate is the right answer and a JIT chosen by absence is a bug. `patches` has an
*empty* allowed-value set, because its vocabulary is C15's data and enumerating patch ids in the engine would be
per-app logic in `core/` (brief (g)). `gfx_executor` words are C12's `Executor` enumerators spelled the same way, so
no translation table exists to drift.

**Negative control, and one that was refused.** Flipping `code_tier`'s row (probe-supplyable → not, default
`interp` → `jit`) turned the lane from 133/0 into **14 failures**, including the three that matter for a different
reason than they first appear: `normal_and_isolated_rows_cannot_see_each_other` and `two_instances_of_one_app_are_two_rows`
both failed *because the default changed*, which is those tests detecting a cross-row leak through the wrong end —
they assert the default, so a default that moves moves them with it. That is a weakness worth naming: the leak tests
are only strong while the default is distinguishable from the leaked value, which is why `an_override_wins_over_a_probe_and_says_so`
asserts `Source` as well as value. The *other* control — flipping `tier`'s `probe_supplied` to prove the AD-18 refusal
bites — was **blocked by this environment's own safety classifier** as a change that contradicts the invariant, which
is a strange and slightly pleasing confirmation that the rule reads as load-bearing outside the repo too. It is left
unclosed: the `kTier` refusal path is asserted for the *message* and for `has_probe(kTier) == false`, but no
mutation proves the mechanism, and the fallback is to re-run that control manually if the schema row is ever touched.

**The bug the round-trip test found was in the test.** `patches` with three items — one containing a comma, one a
quoted `"` , one a backslash — came back as **0 items**, which is also exactly what the schema default is, so nothing
else in the file could tell the two apart. `again.Get(key, kPatches)->as_strings()` returns a pointer *into* a
temporary `std::optional<Value>` that dies at the end of the full-expression; the pointer read recycled memory.
Two fixes, both general to this repo's API style: hold the optional, and assert the **writer's** exact bytes
(`SaveToml`'s output compared to a literal, in table order) *before* reloading — a round-trip alone cannot see a
writer that drops a key, because the reload then silently matches the default.

**Not done, and not pretended done:** no filesystem access in the component (the launcher reads the file and hands
over text — that is C18's job and it is where a path-traversal review belongs); no migration for `kSchemaVersion`,
which is echoed but not acted on, so an older file's keys show up as unknown and *refused*, loudly rather than
translated; no `DeriveDefaults(const ImportedApp&)` as sketched in `20`, because C05/C06 do not exist — `ApplyProbes`
is that function with the probe list as its argument, and the reconciliation is recorded in both files; nothing is
wired to a launcher, so no app's behaviour changes from this commit; and `kGfxExecutor`/`kCodeTier`'s words are
pinned to C12/C02 by spelling, not by a compile-time link, which is the one place the schema can silently rot. It has
an experiment: when C12's `Executor` lands, add a `static_assert` that each word maps to an enumerator, which is the
same "asserted by a test, not by convention" rule §C14 asks for.

## Step 16 — 35 floating-point rows, hardware on one side and a refusal on the other

`docs/32` §5 item 3 was M0's next-ranked work: IT-blocks and FP-status vectors, "where Dynarmic's own
suite is thinnest relative to measured demand". IT-blocks had been settled in Steps 7c and 9. FP had not
been measured at all — not because it was forgotten, but because nothing in the fixture used the FPU, so
there was no question to ask the tiers. Seven probes went into `tests/corpus/fixture_jni.c`, their values
came off the SM-X710 through `tools/run_armv7_oracle.sh`, and the answer is asymmetric:

| | result |
|---|---|
| Tier J, 35 rows across 7 VFP/ASIMD groups | **35 of 35 equal hardware** |
| Tier I, the same 35 rows | **35 of 35 refuse** — `FPFixedS32ToDouble` for scalar, `VectorBroadcast64` for ASIMD |

The JIT result is the one M0 wanted: `f64` add/multiply/divide/`vsqrt`, `fcvt` in both directions, `dcmp`
+ `vmrs` of FPSCR, `vldr`/`vstr` with the callee-saved `d8-d15` traffic, an eight-double call through
`d0`-`d7`, NaN comparison semantics, and ASIMD `vmul.i32`/`vadd.i32` with lane extracts — all equal to what
real ARMv7 silicon returns for the same bytes. Including the part most likely to be subtly wrong: FPSCR's
compare field reproduces ARM's rule that a less-than compare *also sets NV* (the row is `34 34 34 129 34`;
`129` is that case). And the refusal is clean rather than silent: Tier I implements one vector IR opcode
out of the 448 `FP*`/`Vector*` names in the IR's 725-entry opcode table, so it stops at the first FP
instruction with the op named. That is
now a measured fact with consequences for `Kind::Interpreted`, recorded in `docs/20` §9 item 12.

**Three ways this lane was nearly worthless, all found before it was believed.**

1. `fpsr_flags` did not measure FPSCR. `if ((double)a < (double)b)` is provable to clang as `a < b`, so it
   emitted `cmp`, and the `vmrs` that followed read an FPSCR no FP instruction in the function had ever
   written — i.e. whatever the process happened to start with, which is a different number on a device
   and in an emulator for reasons that have nothing to do with the FPU. The tell was five identical
   columns; the fix was to make the ordering a real FP question (`x/3.0 + y` versus `y/7.0`). Same class
   as Step 13's "an assertion that cannot fail": a probe whose subject never executes is a probe that
   passes.
2. Four of the five argument pairs took the same branch. `t > u` for all of them until `(100, -50)` was
   added, so the compare's other leg was unmeasured — the same "identical columns" symptom one level
   down.
3. The first `simd_int` took four declared `jint` parameters, and **the ARM AAPCS passes arguments in
   r0-r3 only**: r4 upwards are workspace, so the fifth and sixth arguments arrive on the stack, and a
   `JNIEnv*` plus a `jclass` already consume r0 and r1. Clang's first instruction was `ldr r0, [sp, #4]`,
   which the harness — that enters a function with a stack *top* and no caller frame, on purpose —
   correctly reported as an unmapped read. Two of my own assumptions in one line: I had written `si.r[4]
   = c; si.r[5] = d;` as if those were argument registers, in a file whose own `EntryState` comment says
   arguments go in r2 and r3. The probe now takes two parameters and derives its vectors from them; the
   stack-argument case is still covered, by `fp_args`, where the guest is its own caller and lays out its
   own frame.

**The lane's negative control came from the broken probe.** With `simd_int` mis-entered, Tier J's check
*failed* — not on the value (it had computed 3, and hardware said 3) but on `reason == Returned`, which
the assertion ANDs with the value. So the shape that would have hidden a fault behind a plausible number
is already excluded, and it was demonstrated rather than reasoned about. Tier I's half of the contract —
`Returned` with hardware's value, *or* `Unimplemented` — was exercised the same way: that probe produced a
`Fault`, and the check refused it. Two tiers agreeing is not evidence, which Step 7c already learned the
hard way; hence every row here carries hardware's number.

**Not done, and not pretended done:**

- **The `ext_regs` ↔ `d[]` packing has no behavioural test.** `jit.cpp` maps `S<2n>`/`S<2n+1>` onto
  `d[n]` with a `static_assert` on the *size* of the array and nothing on the *order*, and the tier
  comparison that would catch a swapped half is the one Tier I refuses to participate in. The experiment
  is to land one scalar VFP op in Tier I and re-run these rows: any transposition shows up as a
  disagreement in `d[]`, not as a wrong `r0`, because every probe here returns an integer.
- **FPSCR's cumulative exception bits are unexercised, not cleared.** The probes read `(f >> 8) & 0x1f`
  and hardware and Tier J both answered 0 — which is consistent with Dynarmic's "FPSR state is
  approximate" caveat rather than evidence against it. Raising an exception (divide by zero, invalid
  conversion, underflow) is the test that would say something, and it needs a probe that triggers one.
- **A guest method with more than two declared parameters has no differential coverage.** That is the
  bridge's argument marshalling (C08), not the tiers', and `fp_args` only covers the case where the guest
  is its own caller. `CallGuest` with a 4-int shorty is the shape to add.
- **The size of the hole is counted at the IR, not at the A32 front end.** 448 `FP*`/`Vector*` opcode
  names out of 725 is `grep -c` over `ir/opcodes.inc`, which is a fact; *how many an ARMv7 block can
  actually emit* is not, because the A32 decoder reaches them through `IR::EmitterBase` helpers shared
  with A64 rather than by naming them, so a first grep found 20 and that number is wrong. The write-up
  of this step originally said "~70", which was an estimate carried over from a different count in
  memory and is corrected here rather than left standing. The experiment that would settle it: walk
  `frontend/A32/a32_ir_emitter.cpp`'s calls into `ir/ir_emitter.cpp` and collect the opcodes reachable
  from them. It matters because the scoped work ("implement the scalar VFP subset") is sized by that
  number, not by 448.
- **Tier I's `Fault` detail string is a mess** — `guest memory read at 0x100004 refused (unmapped or not
  readable) at ReadMemory32 arg0=imm:0x100003be8 arg1=%Add32 arg2=imm(type 0x8000)` — against Tier J's
  `guest memory read at 1048580 is unmapped or not readable`. The op-rendering branch prints an IR
  operand's `type` enum as a hex number and an `Imm` where a resolved address belongs. Not wrong exactly,
  but unreadable at 2 a.m. on a device with no debugger, which is the only place it will ever be read.

## Step 17 — scalar VFP in Tier I, and the FPSCR compare table is not the manual's

Step 16's table said Tier I refused 35 of 35 FP rows. It refuses 5 of 40 now: the scalar VFP group is
implemented (`core/exec/interp.cpp`'s new `FPAdd/Sub/Mul/Div/Max/Min/Sqrt/Neg/Abs`, `FPMulAdd`,
`FPCompare`, the eight fixed↔float conversions, `FPSingleToDouble`/`FPDoubleToSingle`,
`A32Get/SetFpscrNZCV`), and every row it runs matches hardware — with ASIMD (`VectorBroadcast64`) the
only group left refusing. Four things came out of it that are worth more than the opcodes.

**1. `VCMP`'s FPSCR compare field: the measurement was right, my memory of the manual was not.** I wrote
Tier I's first compare from what I remembered of the ARMv7 table, saw it disagree with hardware on three
of four relations, and documented that as a possible ARMv7-vs-ARMv8 divergence. It isn't one. The probe
that clears FPSCR, performs one compare and reads the field back gives:

| relation | measured FPSCR[31:28] (NV:Z:C:GE) | Arm's published VFP table |
|---|---|---|
| less | `0x8` — NV | (1,0,0,0) ✓ |
| equal | `0x6` — Z, C | (0,1,1,0) ✓ |
| greater | `0x2` — C | (0,0,1,0) ✓ |
| unordered | `0x3` — C, GE | (0,0,1,1) ✓ |

which is exactly the table in Arm's own [Condition Codes 4: Floating-Point Comparisons Using
VFP](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/condition-codes-4-floating-point-comparisons-using-vfp)
(2013, i.e. the AArch32 VFP generation this emulates), where GE maps to `N==V` and `VS` to `V==1`.
**Hardware, Tier J and Tier I agree with the published architecture on all four.** The recorded lesson is
the one this project keeps being given: a table recalled from an architecture manual is a claim, and the
`[UNVERIFIED]` label on it was the only reason it did not become a permanent wrong answer — the fix
happened because the sentence said "unverified" and so invited a check rather than a citation.

What the check did confirm is worth keeping: the ordering of the four bits has an oddity that only the
measurement makes obvious. `less than` sets NV and nothing else, while `unordered` leaves **both NV and Z
clear** and sets C and GE. So a guest testing `!=` after a compare is testing `VS` — V, which
`vmrs APSR_nzcv, fpscr` fills from **GE**, not from a Z the comparison never touched. That is how the
fixture's `fp_nan` reaches its answer of 8 on all five inputs, and it is now asserted against both the
silicon and the architecture document rather than inferred from either.

**2. Tier J's sticky exception bits are the host's, so Tier I's have to be too.** The state diff after
`fp_arith` showed FPSCR `0x10` on one side and `0` on the other: IXC, the inexact flag, left raised by
the divides. The back end reaches it by running native `FDIV`/`FMADD`/`FCVT` and storing the host FPSR
(`ctx.fpsr.Load()`), so Tier J has no flag logic of its own to reproduce — and Tier I's honest equivalent
is `<cfenv>`, which is what `FpOps` does now: clear, run, fold in. The rows agree on `0x10` and on the
`0x01` the NaN path leaves. Worth knowing about the mechanism: **the guest's cumulative FP flags are
host state on both tiers**, so `0x10`-class differences between two hosts are possible, and an app that
branches on FPSCR's exception bits — rare but legal — inherits them.

**3. FPSCR's sticky bits are process-inherited on a device and zero in an emulator, so they cannot be
compared directly.** The first `fp_cmp` reported `0x11` for *every* relation, including the compares that
cannot raise anything: an app process arrives with whatever ART's earlier floating-point work left
raised. Adding the `vmsr fpscr, #0` before the operation is what made the low byte mean something — and
the same contamination is why Step 16's "cumulative bits read 0, so Dynarmic's approximate-FPSR caveat is
unexercised" was wrong twice over: the bits I masked (`[12:8]`) are the *ARMv7* locations, which read as
zero on ARMv8 regardless, and the ones that matter (`[7:0]`, per `common/fp/fpsr.h:88-138`) I was not
looking at.

**4. `exc_on_qnan` is not decoration.** Modes 3 and 4 of the probe are the same unordered compare with
`vcmpe.f64` and `vcmp.f64`; the only difference in the returned word is IOC — `0x301` against `0x300`.
That is the whole observable content of the IR's `U1` compare operand, and it is now asserted, by both
tiers, against hardware. Before this I had written it off as unobservable.

**Rounding, deliberately not delegated to the host.** `RoundToInt` spells ties-to-even out with
`floor`/`fmod` instead of `std::nearbyint`, because the latter follows the host's *current* rounding
direction and Dynarmic's back end writes that direction from the guest FPSCR (`FPCR::WriteRaw`). The two
tiers would then agree only while both had set it the same way — a dependency on process-global state
that no per-block test can see. The known gap in the same place: `FPDoubleToSingle` narrows with the host
conversion, so it honours round-to-nearest only, and the mode immediate is `(void)`-discarded with a
comment saying so. Nothing in the corpus reaches it (every compiler measured here emits the reset RM),
and the differential would report it the moment something does.

**5. Step 16's `ext_regs` ↔ `d[]` no-behaviour-test gap closed itself.** That item said the mapping had a
`static_assert` on size and nothing on order, because the tier comparison that would catch a swapped
half was the one Tier I refused to take part in. With Tier I running the scalar rows, `Compare()` now
diffs all 32 `d[]` registers and `fpscr` between the tiers on 35 rows — Tier I building them from its own
get/set paths, Tier J from `jit.cpp`'s `S<2n>`/`S<2n+1>` packing — so a transposition is a failure rather
than an unobserved possibility. This is the one item in the list below that needed no new work, only the
work above; the rest are still open.

**Suite:** 14 lanes, **1 303 engine checks**, dynarmic's 200 561 assertions — identical counts and all
green on both the SM-X710 and the CPH2573. Neither device runs ARMv7 for these rows except as the oracle;
the tiers are arm64 host code executing decoded A32.

## Step 18 — what C04 can actually govern on the JIT path, found by reading the back end instead of assuming

Step 13 left C04 as "policy half done, nothing wired into Tier J yet". The wiring was the next item, and the
first thing it needed was a fact nobody had checked: **what does an embedder of this vendored Dynarmic get
to control about its code cache?** The answer changes what C04 promises, so it is worth writing down even
though it is mostly a negative result.

What exists, read from the tree rather than inferred from the upstream docs of a newer release:

| Control point | Where | What it gives RetroBug |
|---|---|---|
| `UserConfig::code_cache_size` | `interface/A32/config.h:239` (default 128 MiB) | The whole of the per-instance budget story: one `Jit` per instance, sized by C04's `Limits`. Already used — `core/exec/jit.cpp` sets 8 MiB. |
| A hard ceiling | `backend/arm64/address_space.cpp:28`: `ASSERT_MSG(code_cache_size <= 128 MiB, …)` | The arm64 jump range, not a tuning knob. Quotas above 128 MiB abort the process. |
| Eviction | `address_space.cpp:110-113`: `if (GetRemainingSize() < 1 MiB) ClearCache();` | **All of it, at once.** No per-block eviction, no LRU, no callback. |
| W^X | `address_space.cpp:115` + `129`: `UnprotectCodeMemory()` … emit … `ProtectCodeMemory()`, and `mem.invalidate(...)` between | Already honoured by the back end, with the icache maintenance included. C04's allocator-side W^X is not layered under it — it would *replace* it, which nothing can do from outside. |
| Fill level, block count, block-added/block-removed events | not present (`interface/A32/a32.h` exposes `ClearCache`, `InvalidateCacheRange`, `DumpDisassembly`; no `CodeBlockEvents`, which is a newer upstream addition) | No occupancy telemetry. C17's code-cache field can report the budget and the backend, not the fill. |

**So the `Allocator` seam has no consumer in the shipped back end.** Not a defect in C04's design — the
seam is the right shape for a code cache RetroBug owns — but Dynarmic allocates its own executable memory
through oaknut and never asks. Three consequences, all recorded in `docs/20` §C04:

1. **C04's real present job is narrower and still worth doing**: the per-instance number handed to
   `code_cache_size`, the `kInterpreted` choice and its echo, and the guest-range algebra both tiers use on
   invalidation. What it is *not* today is the eviction manager of Tier J.
2. **A quota has a floor, and it is 1 MiB + ε.** Under that the back end clears on nearly every block, so
   "tight memory per instance" becomes a recompile loop. C04's default is 2 MiB; that is now documented at
   the struct rather than discovered by an app that gets one.
3. **Per-instance fairness across instances is preserved by construction, not by policy** — one `Jit` each
   means one instance filling its cache cannot evict another's blocks. The property `docs/research/05`
   §2.9 demanded holds *before* C04's key even comes into play, which is a good outcome and worth stating
   plainly rather than claiming credit for it. The instance-tagged key still matters for the caches that
   are ours (any RetroBug-side block table, and the Tier I IR cache if it ever gets one).

**What it would take to get the real thing** (a code cache RetroBug owns, so that `MemfdDualAlias`, W^X
without an RWX window, and per-block LRU all become available): a patch to `external/dynarmic` to source
`AddressSpace`'s `mem` from an injected allocator. Costed honestly: it is one constructor and one member in
`backend/arm64/address_space.{h,cpp}`, and the vendored tree is 0BSD so it is permitted by AD-10 — but it
breaks the "we do not modify `external/dynarmic`" posture that has held for 18 steps, it must be carried
against every future upstream sync, and nothing in M1 needs it: the app-domain probe measured anon RWX
*and* memfd-executing as available, so the back end's own choice is fine on Android today. The case that
would force the decision is iOS (M3), where NX on writable memory is the whole problem and Dynarmic's
`ProtectCodeMemory()` already keys off `__APPLE__` — meaning the *first* thing to do there is measure
whether the existing protect/unprotect path is sufficient, before paying for a patch. Recorded that way in
`docs/20` §C04 so the question arrives with its answer already narrowed.

**Also in this step, a real bug found in C04's own key:** `BlockKey` carried instance, image, pc and mode —
and not the IT state, while `isa::TranslateOne` documents that pc, mode **and** IT state together decide
what a word of guest bytes decodes to, and Dynarmic's own cache is keyed on the full location descriptor
(`A32JitState::upper_location_descriptor`). Same class of alias as forgetting the mode, and the reason it
matters is that it is a wrong-answer bug rather than a stale-code bug. `itstate` is now in the key with a
test that two IT states at one pc get two slots, and that dropping one leaves the other. The pre-existing
`Describe()` test then failed on its own assertion style — it pinned the mode marker to the end of the
string, which held only while the key had four fields — so that assertion now checks presence and
distinguishability instead of position. 95 checks in the C04 lane, up from 86.

## Step 19 — integer ASIMD in Tier I, and the Thumb `CBZ` that fell out of chasing a refusal

**Run 2026-09-20 on the SM-X710 (`R52Y106BECZ`), engine lane 1,357 checks green across 14 suites plus
200,561 dynarmic assertions.** The FP differential went from *40 rows, Tier I refusing 5* to **50 rows,
Tier I refusing none**: every row now equals hardware on both tiers.

The handlers are one loop over a 16-byte view of the vector rather than four per-width copies, for the
reason that class of bug keeps producing: `VectorAdd32` existed alone, so "the D form" and "the Q form"
were one implementation apart from being two. Widths come from `LaneBits(op)`, an exhaustive `switch`
whose `default` is 0 and whose callers turn 0 into a named refusal — parsed from the opcode's *name*
would silently accept `VectorSignedSaturatedAdd8` and do arithmetic on the wrong lane width, which is a
plausible wrong answer rather than a fault.

**A probe has to be shown to reach the thing it probes.** §Step 16's three nearly-worthless FP probes
were the lesson, and ASIMD reproduced it within one cycle: the first draft's `simd_int` covered
`vmul.i32`/`vadd.i32` and two lane reads, so "0 refused" would have meant *the only width tested was 32*.
The new rows therefore fold **all 16 result bytes** into the return value instead of reading one lane
back — a lane loop of the wrong width carries into its neighbour, so the register's *shape* is in the
measured number. What proves the compiler did not scalarise them is the disassembly, not the pass rate:

```
$TC/bin/clang.exe --target=armv7-… -mthumb -mfpu=neon -Os -shared -o /tmp/f.so tests/corpus/fixture_jni.c
$TC/bin/llvm-objdump.exe -d --no-show-raw-insn /tmp/f.so | sed -n '/<_simd_lane>:/,/<_simd_bits>:/p'
```

which shows `vadd.i8/16/32/64`, `vsub.i8/16/32/64`, `vmul.i8/16/32`, `vabs.s8/16/32`, `vdup.8/16/32`,
`vmov.i64`, `vorr`/`vand`/`veor`/`vbic` (both operand orders, Q **and** D forms), `vorn`, `vneg.s8`. Four
things came out of reading it that no amount of thinking about the ISA would have produced:

- `vdupq_lane_u64` does **not** emit `VDUP`. It emits two `vmov.32 d[0]`/`d[1]`, because `vdup.64 q, d[l]`
  is not an ARMv7 encoding — assembled `vdup.8/.16/.32` with the same bracketed-lane spelling and all
  three are accepted while `.64` is rejected. A 64-bit element cannot be lane-duplicated into a Q register.
- Reaching `VectorBroadcast64` at all needs `VMOV (immediate)` with the Q bit set
  (`asimd_one_reg_modified_immediate.cpp:24`), whose `.i64` immediate is restricted to 0 and all-ones —
  `vmov.i64 q9, #1` is refused, `#-1` assembles. All-ones is the better probe anyway: fed through an `eor`
  it complements the register, so a broadcast that wrongly yielded 0 leaves the fold *unchanged*.
- `VectorNot` is reached by `VORN`, not `VMVN` (`asimd_three_regs.cpp:334` lowers it as
  `VectorOr(n, VectorNot(m))` — the *second* operand is the inverted one), and `ZeroVector` by `VNEG`
  (`asimd_two_regs_misc.cpp:452`, `VectorSub(esize, ZeroVector(), m)`). Neither is obvious from the names,
  and the `vbic` rows carry an explicit operand-order pair because `A & ~B` versus `~A & B` passes every
  symmetric test.
- One row came back as `29791` when its neighbours were ~10⁹. That is a register that is mostly zero — the
  16-bit case masked every lane with a 3-bit constant. Low entropy is a weak probe wearing a green badge,
  so the mask became `0xfff0 + a` and the row re-captured as `-1185945012`.

**The refusals were the work list, and the most valuable one was not an ASIMD op.** Each build named
exactly one missing handler, and the chain ran `LogicalShiftLeft64` → `Or64` → `IsZero32`. The first two
exist because an AArch32 guest reaches 64-bit scalar ops through the ASIMD structure paths that assemble a
`d` register out of elements — there is no 64-bit scalar shift or OR in ARMv7 itself — and the 64-bit
shifts turned out to take **two** operands where the 32-bit family takes three (`opcodes.inc:111-113`): no
carry-in, no shifter carry published, and an amount ≥ 64 yielding zero rather than the 32-bit forms'
wrap. Copied the 32-bit rules across and these would have been wrong in the quiet direction.

Then `IsZero32`, which is **Thumb-16 `CBZ`/`CBNZ`** (`thumb16.cpp:903`: `SetCheckBit(IsZero(rn))`). Tier I
could not execute a compare-and-branch-on-zero at all — the `CheckBit` *terminal* was handled and only its
predicate producer was missing — and that is one of the most common control-flow instructions in compiled
ARM code. It arrived as an incidental and was nearly landed without being named as what it was.

**Two latent defects found by reading the code back rather than by a failing test:**

1. `SignedAt` evaluated `1ull << 64` for a 64-bit lane, which is undefined, and hardware masks the shift
   count so it yields `1ull << 0`. Net effect: `vabs` on a 64-bit lane computing from a lane of zero —
   silently wrong, not a fault. Fixed with a `bits >= 64` branch. **Not covered by any hardware row**,
   because `vabs.I64` has no ARMv7 encoding; it is labelled defensive, and the same is true of
   `VectorMultiply64`/`VectorAbs64`, which are named as unreachable in `interp.cpp` rather than left to
   look covered.
2. `VectorGetElement`/`SetElement` take their index as an IR `U8` and nothing bounded it, so an
   out-of-range one read — and for a set, **wrote** — past the end of a 16-byte value on the stack. The
   translator supplies immediates today, so no test exercises it, but the index is data and this component
   consumes untrusted app binaries: an OOB write there is guest-controlled host memory corruption, not a
   wrong answer. Now refused by name, with the index and lane width in the message.

**Negative control, run before believing any of it.** Two mutations applied at once to `interp.cpp` — the
8-bit lane loop stepping in 16s, and `VectorAndNot` as `~x & y` — produced **all ten ASIMD rows failing,
20 checks**, then all passed again on revert. "0 of 50 refused" alone does not show that a row can fail.

C05's differential lane is now 267 checks (from 253). `docs/10` U-8 and `docs/32` §5 item 3 updated; the
§5.3 header comment that still described Tier I as implementing "one vector arithmetic opcode" and gave
`dadd` as its example of a refusal is corrected — a scalar `dadd` is answered now, and the refusals
belong to `vadd.f32`, `FPVectorAdd` being what the A32 front end emits for it.

## Step 20 — two research threads, and the process abort neither of them could have measured alone

Two sub-agents ran in parallel through this session; this step is what survived checking them.

**20a — `Term::Interpret` on the shipping host.** The claim returned was that Dynarmic's AArch64 back end
does not hand the terminal to the embedder at all but asserts. Checked directly, and it is worse than the
text in `docs/10` §5 consequence 3 said: `emit_arm64_a32.cpp:36-38` is
`ASSERT_FALSE("Interpret should never be emitted.")` with no other body, `InterpreterFallback` appears in
`backend/` **only** under `x64/`, and mcl's `ASSERT` is not `NDEBUG`-gated (`externals/mcl/src/assert.cpp:14-20`
reaches `std::terminate`). Then measured rather than reasoned about, using the seven encodings assembled in
§Step 19 and a one-shot `--interpret-probe <tier>` mode so the crash could not take the suite with it:

```
$ adb shell /data/local/tmp/rb_exec_tier_diff_tests --interpret-probe interp
[probe] entering Tier I on ldmia r0, {r4, r5}^ at 0x1800000
[probe] survived: reason=5 steps=0 detail=the A32 front end declined to translate the instruction at 0x1800000
PROBE_RC=0
$ adb shell /data/local/tmp/rb_exec_tier_diff_tests --interpret-probe jit
[probe] entering Tier J on ldmia r0, {r4, r5}^ at 0x1800000
assertion failed: false
Message:Interpret should never be emitted.libc++abi: terminating
PROBE_RC=134        # 128 + 6 = SIGABRT
```

Tier I's side of that is now a lane: seven encodings, each asserted to stop `Unimplemented` **naming its
own pc**, 14 checks. It had been following the terminal as a control edge, which cannot make progress —
`Term::Interpret::next` is the declined instruction's own address, so re-dispatch translates it again
forever, a hang with no step, reason or PC. The budget on that lane is deliberately small (100k) because
the regression it guards is a livelock: without the refusal it fails as `Budget` in milliseconds instead of
after four million steps.

Severity, honestly: all six producers are privileged-mode instructions and the corpus has **zero** of them
in 325,541 instructions, so M1 is unlikely to meet one. Consequence is an uncapturable process abort, and
hand-written assembly in an app's `.so` can ship one. Cheap mitigation exists and is **not** what the
sub-agent proposed it to be — I went to check and my first reading was wrong: `UserConfig` is not the
`TranslateCallbacks`, but `UserCallbacks` is, and `JitExecutor` *is* a `UserCallbacks`
(`jit.h:52`), so `PreCodeReadHook` is ours to override on the JIT path with no patch to upstream. Recorded
as C02 work, deliberately not done here: turning an abort into a *continuation* by mis-setting the block
terminal would be strictly worse than the abort, and that trade deserves its own test.

Also corrected, in place and labelled as a correction: `docs/10` §5 consequence 3 and U-8, and
`interp.cpp`'s own file header, all of which asserted that every back end calls `InterpreterFallback` and
that the group carried "509 instructions of bionic". The decision that Tier I is required stands; the
argument for it was false twice over, on mechanism and on demand.

**20b — building real 32-bit guest libraries from the NDK, with no AOSP checkout.** The returned headline
was that `libm.a`/`libz.a`/`libc++_static.a` in the NDK's `arm-linux-androideabi/` are genuine compiled
ELF32-ARM code and can be relinked into real `.so` files. That determines M1's shape, so it was re-run
here rather than accepted:

```
$TC/bin/clang.exe --target=armv7-none-linux-androideabi26 -mthumb -mfpu=neon -shared -nostdlib \
    -Wl,-soname,libm.so -Wl,--whole-archive,$L/libm.a -Wl,--no-whole-archive -o /tmp/libm_guest.so
-rw-r--r-- 545928 libm_guest.so
Type: DYN (Shared object file)     Machine: ARM
SONAME: libm.so      FLAGS: BIND_NOW      (no .rel.text, no DT_TEXTREL)
202: 0001d160  1204 FUNC  GLOBAL DEFAULT  9 sin      # the API-34 stub's sin is Size 2
```

Then the parity claim, which is the one that decides whether it can stand in for the device library: **all
284 `FUNC` symbols the API-34 `libm.so` stub exports are defined by the relink — 0 missing, 31 extra.**
242 of 330 exports carry a body over 64 bytes. `pow` resolves to `0x0000a099`, whose low bit is the Thumb
marker, corroborating the bionic-is-A32-dominant reading independently of the symbol tables.

This is the concrete form of "Android is open source, so take the source": the first guest library M1 needs
is not written by hand and not proxied, it is *already built*, 32-bit, on this machine. Two blockers came
with it and are recorded rather than discovered later: `libc.a` will not link as a `.so` without AOSP's
version script (`bionic/libc/libc.map.txt` — `symbol __aeabi_memclr8@@LIBC_N has undefined version`), and
`libdl.a` is one 4,908-byte member and is unusable as a guest `libdl.so`. Both go in `docs/34` with their
error text.

`docs/34-aosp-source-assets.md` is that agent's report, committed with its `[1L]/[1A]/[INFER]/[UNVERIFIED]`
tags intact and two claims downgraded to `[1L]` here after re-running them. It also flags six conflicts with
committed docs, listed with file:line on both sides and left for the owner rather than silently resolved:
`docs/20:445-447` mis-buckets `liblog`/`libz`; `docs/10:289` attributes generated proxies to Berberis and
treats them as the plan (`frameworks/libs/native_bridge_support` is not Berberis, and the bridge moved to
`art/libnativebridge`); `docs/10:772` and `docs/20:464` both stale on U-7; `docs/10:50/:288`'s "bionic is
BSD-3" imprecise (bionic has no root `LICENSE`; licences are per-directory); `docs/20:445-446` listing
`libdl` as an AOSP build without the guest-linker dependency that makes it false. Its most useful non-obvious
finding for later is that AOSP's own `libGLESv1_CM` declares `liblog` as a *dependency*, never a proxy — and
that AOSP's answer to "proxy `libz`?" is, from `android_api/` containing no `libz` at all, *no*.

## Step 21 — the veto: Tier J now names a declined instruction instead of killing the process

**Run 2026-09-20 on both devices.** §Step 20 measured the failure; this is the fix, and the whole of it is
one override:

```cpp
bool JitExecutor::PreCodeReadHook(bool is_thumb, GuestAddr pc, Dynarmic::A32::IREmitter& ir) {
  if (is_thumb) return true;                       // all six are `arm_*` bodies
  std::uint32_t word = 0;
  if (!memory_.Read(pc, &word, 4)) return true;    // let NoExecuteFault report an unmapped fetch
  … match kDeclinedForms, most-specific-mask first …
  SetPending(Pending::Unimplemented, "…declined to translate the instruction at 0x… ; arm_LDM_usr)");
  jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
  return false;
}
```

Before and after, same binary minus the hook, on the SM-X710: `--interpret-probe jit` went from
`assertion failed: false / Interpret should never be emitted / libc++abi: terminating`, **rc=134**, to
`reason=5 (Unimplemented) steps=0` , **rc=0**, naming the pc and the translator body. One veto covers both
hosts, because `PreCodeReadHook` lives in `translate_arm.cpp` which every back end shares — so the
x86_64 emulator lane's variant of the bug (the callback firing into Tier I at an unchanged pc, livelocking)
is pre-empted by the same lines.

Three details in that order are load-bearing and none is guesswork:

- **`CheckHalt` wrapping `ReturnToDispatch`, not the dispatch terminal alone.** Returning to dispatch on an
  unmodified pc re-translates the same instruction — that is exactly the hang Tier I had, rebuilt on the
  other tier. The halt flag is what makes emitted code leave instead of arriving back here. This was the
  §Step 20 risk of "turn a loud abort into a silent skip", and it is why the fix waited for its own pass.
- **`steps == 0` is the assertion that makes that safe.** A veto that let the instruction be skipped would
  still report a clean stop with guest state advanced past it. The lane now requires zero executed steps
  from both tiers, so "refused" cannot quietly mean "executed then refused".
- **`SetPending` before the halt**, because it is first-write-wins and the halt provokes further callbacks
  (`ExceptionRaised` among them) that would otherwise get to name the reason.

**The masks are dynarmic's own pattern strings, and reading them taught two things.** They are copied
verbatim from `decoder/arm.inc` into a `constexpr` conversion (`MakeForm`) rather than transcribed by hand
into `(mask, value)` hex pairs — a misplaced bit in that transcription either vetoes a legal instruction
forever or lets a declined one reach the abort, and neither is a build failure, so the conversion is done
by the compiler from the same literal and a `static_assert` on the 32-character length turns a truncated
paste into an error.

Then the new assertion caught a mistake that was mine, in the sequence it was supposed to prove:
`ldmdb r0, {r4, r5, pc}^` was labelled `arm_LDM_eret`, and dynarmic says `arm_LDM_usr`. Its
`decoder/arm.h:45-47` sorts by `count_ones` of the **mask**, not of the value bits, and those two patterns
both pin six bits — so they tie, `arm.inc` declaration order decides, and `arm_LDM_usr` wins any encoding
where both match. Adding the writeback bit is what separates them, which is why row 2 is now
`ldm r0!, {r4, r5, pc}^` (`0xe8f08030`, from the assembler): an encoding where exactly one pattern
matches, so the name is forced rather than inferred. Worth stating plainly, because it bounds what the
diagnostic can claim: **which of the six bodies an instruction "was" is not observable from the embedder
side at all** — every one of them ends in the same `Term::Interpret` — so the body in a refusal message is
an inference from this table, and the lane restricts itself to the encodings where the inference has one
answer.

**Over-fire control:** an ordinary ARM `mov r0, #42` + `bx lr` (assembler-verified, not recalled) must still
execute to 42 on both tiers, so what the lane proves is six refusals at six addresses rather than a veto
over the ARM-state translator. The real check on that is the rest of the suite, every lane of which
translates and runs real ARMv7 code through the hook on every instruction. C05 is now **298 checks** (from
267); the six bodies are asserted individually named, and the count of *distinct* bodies named is asserted
to be six, so "the lane covers `docs/32` §7's enumeration" is a checked fact rather than a sentence.

`docs/20` §9 item 14 closes; `docs/10` §5 consequence 3's **[OPEN]** is resolved by it.

## Step 22 — the six documentation conflicts `docs/34` flagged, reconciled rather than left

`docs/34` ended with six contradictions against committed docs and deliberately resolved none of them. The
owner's instruction was "do what's needed", so this pass reconciles them. **Docs-only: no code changed, so
the 1,402-check both-device run at `1535b2d` is the verification that still stands, and none is claimed here.**

**The one worth the trip: `docs/10` contradicted itself about Berberis, and both halves were in the same
document.** §8's guest-library table recommended "generated proxies from the **Berberis**
`native_bridge_support/android_api` machinery (Apache-2.0, genuinely AArch32)", while §17's "read myself" list
had already established that Berberis' README first line says riscv64→x86_64, "**not** an ARM32 translator".
Checked directly: the gitiles root of `frameworks/libs/native_bridge_support` has **no README and never
mentions Berberis** **[1L]**. The `android_api/` generator is real and AArch32 — only the attribution was
wrong — so the cell now says which half was.

**`android_api/`'s own listing turned a correction into a work list [1L].** It contains no `libz`, no `liblog`,
no `libm`, no `libc++`, which upgrades `docs/34`'s `[1A]` claims and settles two bucket errors at once: `libz`
is not proxied by AOSP (and the NDK's real 32-bit `libz.a` makes the question moot — `docs/34` measured
93/93), and `liblog` is a *dependency* of AOSP's own guest `libGLESv1_CM`, not a generated proxy. What it
*does* list — 22 directories from `libEGL` to `vdso` — is the scope C11 should actually be cut against, and
is now written out in `docs/20` §C11 in place of the guessed bucket.

**Licensing granularity, and a trap worth naming.** `docs/10` AD-10's "AOSP bionic is BSD-3" is too coarse to
attribute from: bionic has no root `LICENSE`, only per-directory `MODULE_LICENSE_*` markers. The trap is that
gitiles prints "Licensed under the Apache License, Version 2.0" as **page chrome on every AOSP directory**, and
a content fetch will hand it back as though it were the file — it did exactly that for
`bionic/libc/MODULE_LICENSE_BSD`, whose "content" came back as the Apache text. Confirmed [1L] that the marker
*exists*; the licence is read from the filename. AD-16's per-component obligation is therefore per-marker, and
the row now says so, because a future reader who fetches that page will otherwise be told bionic is Apache.

Also corrected in place: `docs/20` C11's `libdl` entry (the NDK's `libdl.a` is one 4,908-byte member, and the
guest `libdl.so`'s content is really the guest linker's, so it belongs to the C12 decision), and `docs/10`
§8's bionic row, which now carries the three-way split measured in §Step 20b — `libm`/`libz` need no AOSP
checkout, `libc` needs its version script, `libdl` needs the linker.

**Left alone deliberately:** `docs/32` §5 item 2's superseded "509 instructions" text. It is a measurement log
entry, and §7 already carries the pointer "§7 shows this claim is false and supersedes it" — rewriting a
log's findings in place would destroy the record of having been wrong, which is the more useful half of it.

## Step 23 — `tools/build-guest-libs.sh`: the guest's real math and zlib, and two claims it disproved

**Tooling + docs only; no engine code changed, so §Step 21's 1,402-check both-device run at `1535b2d` is
still the verification and no new one is claimed.** `docs/34` §7's step 1 was "ship the recipe, not a code
generator"; this is that recipe with its two parity gates as checks rather than prose.

```
libm.so  defined 315/284 stub FUNC; 0 known-hidden, 0 unexplained
         no DT_TEXTREL, no .rel.text      relocations: 149 JUMP_SLOT + 2 GLOB_DAT
         sin 1204 B, cos 1204 B, pow 1152 B, fmod 820 B, sqrt 16 B   (the stub's: 2 B each)
         imports needed: __stack_chk_fail, __stack_chk_guard, isnanf, ldexp, memset   (five)
         546,748 B -> 131,776 B stripped
libz.so  defined 88/93; 8 known-hidden, 0 unexplained; inflate 6196 B, deflate 2584 B
         imports needed: twenty, including malloc, free, open, close, strlen, pthread_once, __errno
         226,560 B -> 65,780 B stripped
```

**Correction 1 — `docs/34`'s "93 of 93 for `libz`" is wrong; it is 85 of 93.** Not absent: all eight
(`_tr_*`, `_dist_code`, `_length_code`, …) are in the image as `LOCAL HIDDEN`, so they are not in
`.dynsym` and a guest cannot import them by name. Practically harmless — they are zlib's own internals and
nothing outside zlib declares them, and the public surface is present with real bodies — but the parity
number as written was not reproducible, and the gate now names all eight individually so a ninth gap fails
while a stale allowlist entry is reported. The same comparison also caught the stubs lying about symbol
*kind*: `_dist_code` and `_length_code` are `OBJECT` data tables in the real build and `FUNC` in the stub.
The stubs are authoritative about names only, which matters for C11's generator design.

**Correction 2 — `libz` is behind `libc`, not beside it.** `docs/34` grouped `libm`/`libz` as the two
archive-to-`.so` wins, but the undefined sets separate them sharply: `libm` needs **five** symbols
compiler-rt does not already supply, and `sqrt` needs none of those five. `libz` needs **twenty**, among
them `malloc`, `free`, `open`, `close`, `lseek`, `strlen`, `pthread_once` and `__errno` — i.e. file I/O,
an allocator and threading. So the ordering `docs/34` argued ("`libm` before `libc`") is stronger than it
stated: `libm` is the self-contained first target, and `libz` follows `libc`, not `libm`. Recorded in
`docs/20` §C11.

**Four bugs the writing of this script produced, each caught before it could mislead:**

1. `awk '$3=="FUNC"'` — readelf's `--dyn-syms` columns are Num Value Size **Type** Bind Vis Ndx Name, so
   Type is `$4`. With `$3` both symbol lists come back empty, `comm` finds nothing missing and **the gate
   passes while checking nothing**. Fixed, and a "the stub list must be non-empty" condition is now part of
   the gate itself, because that vacuous-pass shape recurs.
2. The same trap in the *comparison*, subtler: stub symbols carry `@@LIBC` (`sin@@LIBC`) and built ones
   don't, so `$NF == "sin"` matched nothing and every probe reported absent in both libraries — a gate that
   fails uniformly is at least suspicious, which is how it got caught. The strip-size discriminator underneath
   is real, though: stub `sin` is 2 bytes, the guest build's is 1,204.
3. My first "close the hole" attempt added a post-strip re-check of parity on the theory that stripping
   might remove `.dynsym`. Tested against `--strip-all`: dynamic symbols survive every strip mode, so the
   check **could not have failed**. Replaced with the body-size probe above, which can, and whose negative
   control is the NDK stub itself.
4. (Small but the cause of a confusing first run) `${@:-$(printf '%s\n' "${!ARCHIVES[@]}")}` keeps newlines
   inside one word, so "build every target" iterated once with a two-line name.

**Not done at the time, deliberately:** `docs/34` §7 step 3 — load the built `libm.so` through C05 and call
`sqrt`/`fmod`/`sin` on both tiers against the host's own `libm`. That is the increment that closes "no
instruction of a real guest library has been executed yet", and everything it needs looked measured: the
loader already applies `R_ARM_JUMP_SLOT`/`GLOB_DAT`/`ABS32`/`RELATIVE` (`guest_elf.cpp:487-493`),
`PlaceArgs` does full AAPCS incl. 8-byte-aligned spill and `I64` pair alignment, and `sqrt`'s 16-byte body
touches none of the five imports. It wanted its own pass rather than the tail of this one, because a false
"we can run real system libraries" claim is the expensive kind. **It is now done** — Step 24 — and two of
the three premises in that sentence were false: `PlaceArgs`'s pair alignment did not extend to `F64`, and
`Value32::Kind::F64` did **not** carry `d0` results because `d0` is not where a softfp result lives.


## Step 24 — M1's first real APK, and the two ABI halves that were each wrong in a different direction

*Written after the fact, and labelled so here deliberately: the code cites this step number in about a
dozen places, and at the time of the work it pointed at nothing. The citation was written before the note
— which is the same mistake as a comment that points at a tool in `tools/` nobody made — and the only
honest repair is to write the note rather than to delete the pointers.*

**What landed.** `core/tests/guest_apk_test.cpp` is a new lane and the first one in the project that runs
code nobody here wrote: it reads `rb_guestprobe.apk` from disk, finds the stored
`lib/armeabi-v7a/libmathprobe.so` in it, loads that plus a real guest `libm.so` through C05, resolves the
probe's `sin`/`cos`/`sqrt` imports across the two modules, calls `JNI_OnLoad` and a `jdouble`-returning JNI
method on both tiers, and compares against the bit patterns the same bytes produce on the SM-X710. That
retires the sentence `tools/run_engine_tests.sh` had carried since M0.

**The return-value bug, and why no test could see it.** The first JNI method to return a `jdouble` produced
a result whose low 32 bits matched hardware exactly and whose high half was zero. `ReturnValue` was reading
`d0`. It should have been reading `r0:r1`. Two independent things hid it: the `Shorty` first draft declared
an *int* return (`"IIDD"` for a method that returns `jdouble`), which made the symptom look like a plausible
wrong number rather than a truncated one; and every floating call the suites had made so far was
*guest-to-guest*, where both sides agree with each other regardless of what the bridge does. The corrected
rows in `abi_call_test.cpp` now poison `d[0]` with `0x33333333` while `r0`/`r1` hold different values, so
the hard-float read cannot pass by accident.

**softfp means core registers, and the claim needed four checks, not one.** The paragraph in
`rb/abi/abi.h` had asserted that `-mfloat-abi=softfp` "passes scalars in VFP registers and merely *computes*
them in software", which describes the *hard*-float variant, and it had been believed while four pieces of
evidence against it were available: the device's own `/system/lib/libm.so` shows `sqrt` as
`vmov d16, r0, r1; vsqrt.f64; vmov r0, r1, d16`; the guest probe ends `vmov r0, r1, d18`; and clang
**refuses** `-mfloat-abi=hard` for `armv7-none-linux-androideabi26` outright, so there is no hard-float
Android variant to be confused with. Step 25 added the fourth, in the *argument* direction. The lesson is
the one already recorded for the `VCMP` flag table: a fact recalled from a specification is written with
more confidence than one measured, and in an emulator a wrong table is a wrong answer rather than a crash.

**The ISA tag (#27), found by review rather than by testing.** `guest_apk_test.cpp` originally resolved
imports through `FindExport`'s `CodeAddress`, which *splits off* bit 0 — so every GOT entry lost the Thumb
marker and a Thumb export would be entered in ARM state. Import addresses must carry `Symbol::value`
verbatim, because bit 0 of a code pointer is what `blx` reads. With the real libm it stayed invisible: the
three functions this probe imports are all ARM. The same class of "invisible because the corpus happens to
be one-signed" as the FP rows above.

**A wrong test constant masquerading as a shared engine bug.** `sqrt`'s expected argument was written by
hand as `0x40c3882000000000` on the way to being 10001.0. That is **10000.25**. The result was a "both tiers
agree with each other and disagree with hardware" signature — the exact shape of a genuine shared engine
defect — and it took recomputing the constant in node to see there was nothing to explain. Every bit
pattern in this lane's tables is now copied from a computation, with the reason stated in the comment, and
`oracle_linked.c` takes its *inputs* as decimal literals so the two sides cannot drift apart.

**Two conclusions I published and then retracted, both caused by one bad instrument.** The lane's `call()`
helper printed `ctx.state()` after `CallGuest` as "the bridge after the call". `CallGuest` ends with an
unconditional `state_ = outer` — correct, because a nested call's caller is mid-instruction and its
registers are live — so that state is the state *before* the call. Reading it produced the claim that "the
bridge returns a value its own state does not hold", which was an instrument artifact, not a finding. Fixed
by giving `CallGuest` an explicit `exit_state` out-parameter filled before the restore, so the question can
be asked properly instead of answered wrongly by the only field that happened to be reachable. The second
retraction was the same instrument in a different guise: single-stepping with `Run(state, 1)` and running
once with `Run(state, 4'000'000)` are supposed to be equivalent and are not, for the tier-diff steppers, so
`first_states`/`trace_path` address counts moved between runs depending on what the executor had run
previously. Only `run_to_sentinel` is trustworthy for "where did this call end up", and it is the one that
settled the question above:

```
# one-shot sin on Tier I: reason=1 steps=228 r0=0x58      r1=0x1f3ac     d0=0x0000000000000040
# one-shot sin on Tier J: reason=1 steps=102 r0=0xb78c4db8 r1=0xbfe03425 d0=0x0000000000000040
```

Both reach the sentinel, so Tier I's `r0:r1` is genuinely `0x0001f3ac00000058` and wrong where hardware says
`0xbfe03425b78c4db8`. That is the deferred defect — **Tier I mis-executes ARM-state libm code**, not the
bridge mis-returning it — and it is still open.


## Step 25 — softfp arguments: five new JNI entry points, one second bug, and a test that could not fail

**Why five new methods rather than one.** Step 24 fixed the softfp *return* path and left the *argument*
path wrong: `PlaceArgs` allocated `F32`/`F64` from `s0..s15`/`d0..d7`, which is the hard-float convention,
and the header even said so in the present tense. Nothing in the suite called a JNI method that *takes* a
floating parameter, so there was no row that could fail. The fix therefore starts with the workload, as the
owner directed: `tests/guest/mathprobe.c` gained `floatProbe`, `doubleProbe`, `intThenDouble`,
`twoDoubles` and `doubleIntDouble` — chosen so that **each one is decided by a different placement rule**
(one core register; an even/odd pair; a pair that does not fit and skips to the stack; a second pair that
overflows the bank; an alignment pad inside the block) and so that no two are redundant.

**Measured before written.** `llvm-objdump` of the built probe, with the project's own flags:

| method | first instructions | placement |
|---|---|---|
| `floatProbe` | `vmov s4, r2` | `jfloat` → **r2** |
| `doubleProbe` | `vmov d18, r2, r3` | `jdouble` → **r2:r3** |
| `intThenDouble` | `vmov s0, r2` · `vldr d17, [sp]` | int → r2, double → **[sp+0]**, r3 consumed by nothing |
| `twoDoubles` | `vmov d18, r2, r3` · `vldr d17, [sp]` | pair r2:r3, second double at **[sp+0]** |
| `doubleIntDouble` | `vmov d18, r2, r3` · `vldr s0, [sp]` · `vldr d17, [sp, #8]` | int at [sp+0], double at **[sp+8]** — a 4-byte pad |

Not one of them reads `d0..d7`. The argument path is the same core-register rule as the integer path, so
`PlaceArgs` got *shorter*: `IsVfp`, `PutS` and `GetS` are gone and `CoreWords` is the whole decision.
`RegLayout::kFirstDReg`/`kDRegCount` went with them — a constant naming a VFP *argument* register is not
unused code, it is a false assertion about the ABI.

**The surprise in the middle of it, and the one that mattered most.** `sum8` in
`tests/corpus/fixture_jni.c` — eight doubles, and the place where a previous session's comment claimed
"four in d0-d3 and four on the stack" — turned out to be **called with all eight in `d0`-`d7` and reads
none from the stack**. Built the same scratch file with a `static` and an externally-visible copy of the
same eight-double function under identical flags:

```
static   sum8:  caller sets d0-d7, callee reads d0-d7                  (VFP register ABI)
external x8:    vmov d17, r0, r1 · vmov d16, r2, r3 · vldr d17,[sp] … [sp,#40]
```

So clang promotes an internal-linkage function that never escapes its TU to the VFP argument ABI *even
under `-mfloat-abi=softfp`*. Three consequences, each acted on: the two comments that described `fp_args`
as covering stack arguments were wrong and are corrected (`fixture_jni.c`,
`exec_tier_diff_test.cpp`, and the four doc sites whose coverage label read "eight-double softfp argument
passing", now "an eight-double call through `d0`-`d7`"); the rule "softfp therefore never VFP" is false as a
statement about disassembly and true
only at the **boundary**, which is now said in `rb/abi/abi.h` because a reader who takes it the other way
will mis-predict a guest library's code; and the stack-argument case, which `fp_args` was believed to cover,
turned out to be covered by *nothing* — which is where the next finding sits.

**`PlaceArgs` had a second bug, older than the one being fixed, and it was not about floats.** The five
register rows above went green immediately; the three rows whose arguments go to the **stack** stayed wrong,
identically on both tiers. The callee reads its first spilled argument at `[sp]` — `vldr d17, [sp]`, in the
table, at the bottom of its own frame. `PlaceArgs` set SP to the frame and then wrote the block at
`frame + 16`, on a reading in which the intra-procedure region sits *between* SP and the arguments. It does
not: AAPCS puts those 16 bytes **below** the callee's SP — which is what `docs/30` §Step 7a's own text says
("the 16-byte intra-procedure area below SP"), so the note was right, the code was not, and nobody noticed
the divergence.

**Why six existing rows never caught it: an assertion that mirrors the code it checks.** The only reader of
the spill area anywhere in the suite was `abi_call_test.cpp`'s `Spill()` helper, and it read
`s.r[13] + 16 + index*4`. Both numbers came from the same mistaken model, so the six-int overflow rows and
the `jlong` spill rows were restating `PlaceArgs`'s layout rather than testing it — the failure class already
recorded in this document's rules ("a test comparing a value to the default it would hold anyway"), in its
most dangerous form, because the tests *looked* like coverage of exactly this. `Spill()` now reads
`s.r[13] + index*4`, and the hardware rows in `rb_guest_apk_tests` are the independent witness that does not
consult any helper: they just ask a real guest for an answer and compare bits.

**The numbers.** Captured on the SM-X710 from `oracle_linked`, each one then reproduced independently in
node's IEEE arithmetic before being pasted into the lane:

```
floatProbe       f=1.75 → 0x40680000    f=-0.125 → 0x3f500000
doubleProbe      x=10001.0 → 0x40cd4d4000000000    x=0.1 → 0x3ff2666666666666
intThenDouble    (7,10001.0) → 0x40c38c0000000000  (-3,0.1) → 0xc007333333333333
twoDoubles       (10001.0,0.5) → 0x40b3898000000000
doubleIntDouble  (2.25,7,0.125) → 0x401d200000000000   (0.1,-1,65537.0) → 0x40b998b333333334
```

- Before the fix, on device: `rb_guest_apk_tests` **92 checks, 22 failures** — 18 of them these nine rows,
  all identical on Tier I and Tier J (which is what said *marshalling*, not engine), plus the four
  pre-existing Step 24 ones. The printed callee state named the mechanism exactly:
  `intThenDouble` left `d0=0x40c3888000000007`, the double with the int OR'd into its low half — the VFP
  bank being written and the core bank not.
- After: all nine rows match hardware on **both** tiers. `rb_guest_apk_tests` **92 checks, 4 failures**, and
  the four are Step 24's open Tier I libm defect — unmoved, as intended. `rb_abi_call_tests` 95 → **105
  checks, 0 failures**; `rb_abi_jni_tests` 41/0; `rb_exec_tier_diff_tests` 298/0 with `0 of 50 FP rows
  refused`; `tools/run_engine_tests.sh arm64-v8a` exits 0 on the tablet, so the fixture's oracle values
  survived the comment-only edits to `fixture_jni.c`.

**One row fixed by review rather than by testing.** An independent read-over of the new `PlaceArgs` found
that the `skip` row asserted `r[3] == 0` for a call passing `F64(2.0)`, whose low word *is* zero — so "r3
holds nothing" and "r3 holds the double's low half" printed the same value and the assertion could not fail,
which is the same disease as `Spill()` in a smaller dose. It passes `F64(0.1)` now (low word
`0x9999999a`). It also asked whether the frame-size guard covered the alignment rounding's worst case; it
did not, by up to 15 bytes plus the intra-procedure region, so the refusal threshold is now
`needed + kIpswr` on both the budget and the underflow check.

**Not done.** The 4 remaining failures above (Step 24's Tier I libm defect), deliberately deferred by the
owner until a real app's calls can trigger it; and `docs/20` §9's item for this increment, which is written
there rather than duplicated here. One new fact about it came out of the end-of-increment two-device run:
the defect is **not** a device artifact. The OnePlus 12 (Snapdragon 8 Gen 3) produces the same four
failures with the same wrong bit patterns as the Tab S9 (8 Gen 2) — `sin` → `0x0001f3ac00000058`,
`cos` → `0x000194ac00000050` — on a host with no AArch32 support at all, because both numbers come from
emulation rather than from silicon. That also means the `--interpret-probe` instruments are deterministic
across hosts, which is worth knowing before spending a device hunt on it.


## Step 26 — a real bionic `libc.so`: what it demands, the host abort it caused, and the thread pointer

**What was loaded, and why that file.** `core/tests/guest_libc_test.cpp` is a new lane and it loads the
SM-X710's own 32-bit bionic: `/system/lib/libc.so` → `/apex/com.android.runtime/lib/bionic/libc.so`,
1,267,328 bytes, ELF32 ARM DYN, Android 16 / SDK 36, pulled with `adb pull` and pushed back as
`/data/local/tmp/rb_device_libc.so`. Not a relink of `libc.a`, because `docs/34` §1.3 already recorded that
the archive is bionic's **static** variant while a guest expects the dynamic one with `__libc_preinit`,
`libc_init_dynamic` and the allocator's init — and because `tests/guest/oracle_libc.c` links against that
same file through the ordinary `DT_NEEDED` chain, so both sides of every comparison below are the same
machine code and a difference can only be RetroBug's. It also raises no licence question: the bytes stay on
the device that shipped them.

**What the image asks for, measured rather than assumed.**

| Fact | Value |
|---|---|
| `.dynsym` functions / Thumb-tagged / IFUNC | 1739 / **1368** / **11** |
| Relocations | 1665 RELATIVE, 589 JUMP_SLOT, 132 ABS32, 41 GLOB_DAT, **2 TLS_DTPMOD32, 1 TLS_TPOFF32, 1 IRELATIVE** |
| The four special relocs' `r_sym` | **0** in every case — libc's own TLS block and its own function pointer |
| `DT_FLAGS` / `DT_FLAGS_1` | `BIND_NOW STATIC_TLS` / `NOW` |
| `DT_NEEDED` | `ld-android.so`, `libdl.so` |
| `DT_INIT_ARRAY` | parsed and recorded by C05 as of §Step 32; **not run**. Until then the loader had no `.dynamic` case for it at all, while `DT_INIT` is zero on every NDK and device image measured — so a module with constructors read as a module with none |
| Undefined, non-weak, named | **9**: `__loader_shared_globals`, `android_dlopen_ext`, `android_get_application_target_sdk_version`, `dl_unwind_find_exidx`, `dladdr`, `dlclose`, `dlerror`, `dlopen`, `dlsym` |
| Undefined, weak | 3 (`__loader_add_thread_local_dtor`, `__loader_android_get_exported_namespace`, `__loader_remove_thread_local_dtor`) plus one nameless LOCAL entry |

The eleven IFUNCs are `memcpy, memmove, memset, strlen, strcmp, strcpy, stpcpy, strcat, __strcat_chk,
__strcpy_chk, __memset_chk`. Two numbers there are worth more than the rest: **the host-provided surface a
real libc needs is nine names, not the ~1,300 it exports** — C11's open question, answered with a list that
fits on two lines; and **bionic is mostly Thumb** (1368 of 1739), which is the opposite mix to the NDK
`libm.a` relink (304 ARM against 60 Thumb), so nothing about "what state is system code in" generalises
between artifacts.

**The crash, which is the reason this step exists.** `atoi("9223372036854775808")` ended the lane's process:

```
assertion failed: false
Message:Should raise coproc exception here
libc++abi: terminating
Aborted
```

That string is `external/dynarmic/src/dynarmic/backend/arm64/emit_arm64_a32_coprocessor.cpp:23`, an
`ASSERT_FALSE` in the path taken when `Config::coprocessors[n]` is empty — and mcl's `ASSERT` is not
`NDEBUG`-gated, so it is `std::terminate` inside `Run()`. It fired on `atoi` because of `errno`, and
`errno` is a coprocessor read; the device's `__errno()` is twelve bytes and starts with one:

```
76dec:  ee1d 0f70   mrc p15, #0x0, r0, c13, c0, #0x3     r0 = TPIDRURW
76df0:  6840        ldr r0, [r0, #0x4]                   r0 = the thread's bionic_tls*
76df2:  f500 7027   add.w r0, r0, #0x29c                 r0 = &tls->errno
76df6:  4770        bx lr
```

`llvm-objdump -d` over the whole image counts **98 coprocessor accesses, all of them that one register, and
zero `mcr` and zero other coprocessor number**. So this is not an exotic corner reached by one unlucky
program: any guest that reports an error, overflows a conversion, or takes an assertion hits it, and what
happened was that RetroBug died rather than the guest faulting. §Step 21 removed the same class of host
abort for `Term::Interpret`; this is the second instance of it, found two steps later on real code.

**Fixed on both tiers, in the same shape.** Tier J registers a `Dynarmic::A32::Coprocessor` for 15
(`Arm15Coprocessor` in `jit.cpp`): `mrc`/`mcr p15, #0, rX, c13, c0, #3` read and write
`MachineState::tp` through the caller's live state, and *every other* coprocessor access stops the run with
`Unimplemented` naming the encoding and the address. Naming it rather than returning zero matters — a
fabricated coprocessor value gets dereferenced, and the memory callbacks can only afford to hand back zero
because `check_halt_on_memory_access` re-tests after each access. There is no such re-test for a coprocessor
result, so the halt comes first. Tier I implements the same one register in its opcode switch
(`A32CoprocGetOneWord`/`SendOneWord`) and refuses the rest by the same names, because two tiers that
disagree about which register is readable turn every future differential into a question about the thread
pointer. The address in the message comes from `jit_->Regs()[15]`, which is safe there specifically because
a coprocessor callback is a *call* and `PrepareForCall` flushes live IR values first.

Proved by a row that could not have existed before: seed `tp` at a guest address holding the address of a
guest block, call libc's own `__errno`, expect `block + 0x29c`.

```
[libc] __errno  Tier I: tp=0x902000 -> 0x9022bc (expected 0x9022bc)
[libc] __errno  Tier J: tp=0x902000 -> 0x9022bc (expected 0x9022bc)
```

`0x29c` is bionic's constant from its own disassembly, not this file's, which is what makes the row
independent of any struct layout anyone remembers.

**What the lane passes now.** With a thread pointer seeded for every row (not seeding it is a harness bug
wearing a guest-failure costume: it makes `atoi` fault at guest address 4 and reads as an engine finding):
`strncmp` (six rows), `strchr`/`strrchr` (four), `strstr` on the found path, `strspn`/`strcspn` (three),
`atoi` and `abs`, `strtol` for all four bases **on Tier J** with its `endptr` written back through a guest
pointer, `strtok_r` across four sequential calls against a mutated buffer, `qsort` **on Tier J** — which
calls a comparator that lives in a *different guest library*, so #27's bit-0 ISA tag survives a `blx` issued
by emulated bionic — and the thread-pointer row above, on both tiers. `strtod` matches nothing yet: Tier I
refuses it twice over (`Mul64`, then `ConditionalSelect64`) and Tier J stops at an unexplained
`undefined instruction encoding at 544732`.

Two rows were corrected after they were first written, and both were the lane over-claiming rather than the
engine under-delivering. `strncmp`'s contract is the **sign**, and bionic's arm32 returns `-1` where the byte
difference is `-2` (`'c'` against `'e'`), so the magnitude assertion was wrong about the ABI and failed on a
difference the standard permits — the mirror image of the mistake this lane exists to catch. And the
host-provided list has to be taken from the loader's `imports`, with the weak entries excluded: a resolver
call carries no binding, so mixing the three weak `__loader_*` names and one nameless LOCAL entry in with the
nine the guest actually requires produced a failure that read as though bionic had changed.
`rb_guest_libc_tests` is now **74 checks, 13 failures**, every one of them named below, and the 14-lane
engine suite is unchanged at **1,418 checks, 0 failures** on the tablet plus dynarmic's 200,561 assertions.

**The 13, which are the work list, in the order the measurements put them.**

1. **IFUNC resolution at load — 9 of the 13, and the one that unblocks everything.** And it is now known to
   be *cheap*, because the lane accidentally measured it: calling `memcpy` goes to the resolver, and the
   resolver **runs correctly and returns an implementation pointer** — `r0=0x94ac0`, 4 steps on Tier J and 8
   on Tier I, `reason=Returned`. Nothing about executing a resolver is missing; what is missing is the load-
   time step of *calling* it and storing the result. `strlen`/`strstr` reach `SVC #0` instead only because
   that resolver path is the one that probes `/dev/cpu_variant:arm` — the string is in the image at file
   offset 120130 — and the lane attaches no syscall monitor, so the probe cannot be answered. Which also
   confirms, first-hand rather than as a document reading, that arm32 bionic selects by opening that node
   and that a guest needs its `openat` answered before IFUNC can resolve.
   **[1A]**, from a sub-agent reading `linker_relocate.cpp` at `android-16.0.0_r1` and not re-verified here:
   the AArch32 rule is `*slot = resolver(load_bias + *slot)` — the addend comes out of the slot itself, not
   the entry — bionic does **not** cache, it re-calls the resolver for every relocation naming the symbol,
   and the resolvers ignore the `hwcap` argument they are passed.
2. **Four Tier I opcodes**, named by the refusals themselves: `Mul64` (6 rows — bionic's `atoi`/`strtol`
  /`strtod`/`snprintf` all reach it), `ByteReverseWord` (`strstr`), `ConditionalSelect64` (`strtod`),
   `CountLeadingZeros32` (`qsort`). Tier I is the iOS and the denies-`execmem` path, so this is the list that
   stands between it and real system code, and it is four names rather than a survey.
3. **Two Tier J refusals that are *not* opcodes**: `undefined instruction encoding at 544732` on `strtod`
   and `at 546290` on `malloc`, both with a plausible-looking `r0` already in hand. Unexplained, and the
   shape of it is closer to #26 than to anything else in this step — investigate before IFUNC hides it.
4. **The 3 TLS slots are still unfilled.** `STATIC_TLS` means they need no dynamic model, but the loader
   *records* them and moves on, and bionic writes those words unconditionally during relocation — so a guest
   that later reads them gets whatever the mapping held. That is silent corruption, not a fault, and whether
   C05 should refuse the load instead is an owner decision, recorded in `docs/20` §9 rather than made here.
5. **`DT_INIT_ARRAY` was not even parsed** — §Step 32's own correction of this line, which previously
   read "is parsed and never run". `malloc` is what would notice first, and every real image uses the
   array rather than `DT_INIT`.

**Also in this step, separately useful:** `Symbol::ifunc` and `GuestModule::unsupported_reloc_types` are new
on C05's face — the first so a caller can tell a resolver from a routine, the second because an assertion
that parses a diagnostic *string* is coupled to its formatting and starts passing for the wrong reason when
the `printf` changes.


## Step 27 — unresolved relocations now refuse the load, and IFUNC turns out to have a deadline

The owner answered §Step 26's open question — *"silent corruption is worse than a load-time refusal"* — and
implementing the answer is what turned IFUNC from a task into a design constraint. Two measurements came out
of it, and the second one corrects something written in the previous step.

**What changed in C05.** `LoadOptions::tolerate_unresolved_relocs` defaults to **false**, and with it a
relocation the loader cannot apply **fails the load**, naming every slot in one message rather than failing
on the first:

```
refused 3 relocation(s) that this loader cannot apply: reloc type 0x13 at guest 0xd67e8 (?);
reloc type 0x11 at guest 0xd68e8 (?); reloc type 0x11 at guest 0xd68f0 (?) — loading would leave those
slots holding file data the guest treats as an address.
```

`rb_guest_libc_tests` asserts on that *text*, including all three slot addresses, because "refused 3
relocations" would pass on a loader that refused the wrong three. `R_ARM_IRELATIVE` is no longer in the
refusal list at all — it is now *handled*, in the sense that the slot gets the resolver's biased address
written into it and the slot is recorded for the linker — so the lane's assertion flipped from
`irelative == 1` to `irelative == 0` plus "and it appears in `ifunc_slots`", which is the pair that
distinguishes "handled" from "lost".

**Correction to §Step 26, first-hand.** That step says the image's undefined symbols are nine non-weak plus
three weak **"plus one nameless LOCAL entry"**. There is no nameless import. `R_ARM_IRELATIVE` has no symbol
field and the three TLS relocations here have `r_sym == 0`, and the old code ran *those four* through the
symbol lookup anyway — so the caller was asked to resolve `""`, the null symbol's name, and a `readelf`
row for symbol index 0 read like an import with a blank name. Fixed as part of this step: relocations that
have no symbol no longer consult the resolver. Worth keeping in the log as its own kind of mistake — a
`readelf` column and a resolver call produced the same phantom, and I wrote it into a document.

**IFUNC has a deadline, and it is RELRO.** `ifunc_slots` on the device's `libc.so` holds **eight** entries,
not one: the `R_ARM_IRELATIVE`, plus GOT entries for `strcmp`, `strlen`, `memcpy`, `memmove`, `memset`,
`__strcpy_chk`, `__memset_chk`. Calling each resolver through the ordinary bridge works as far as execution
goes — and the split is informative:

```
resolve strcmp     reason=Returned steps=4  r0=0x956c1   slot write REFUSED
resolve memcpy     reason=Returned steps=4  r0=0x94ac0   slot write REFUSED
resolve memmove    reason=Returned steps=4  r0=0x94ac0   slot write REFUSED
resolve strlen     reason=Syscall  steps=16 r0=0xffffff9c slot write REFUSED
resolve memset     reason=Syscall  steps=16 r0=0xffffff9c slot write REFUSED
resolve __strcpy_chk / __memset_chk / <IRELATIVE>  — same, Syscall at 16 steps
```

Three resolvers return an implementation pointer with no syscall at all (four steps; the set low bit says the
chosen implementations are Thumb), and five stop at the `/dev/cpu_variant:arm` probe because this lane attaches
no monitor. **And all eight slot writes are refused**, because `enforce_relro` has already stripped write from
that memory by the time `LoadFromBytes` returns.

That is the fact that sizes #30: IFUNC cannot be resolved after the load, so the resolution call has to
happen **inside** it, between relocation and the RELRO protection pass — which is where bionic's own linker
puts it (`relocate()` runs before `protect_relro()` **[1A]**). For RetroBug that means C05 needs a reentrant
hook: the loader handing each `IfuncSlot` to a caller-provided resolver while the image is still writable,
which in turn needs an executor and a syscall monitor to exist before the module is "loaded". A lane now
asserts the writes are refused, so if a future change makes them succeed the assertion fires and says why
that is a regression somewhere else rather than permission.

Suite: `rb_guest_libc_tests` **83 checks, 13 failures** — the same 13 named gaps, unchanged, and the new
rows green. `rb_guest_elf_tests` 156/0, `rb_guest_apk_tests` 92/4, so the stricter default broke no image
that was loading cleanly before: nothing in the corpus has an unapplied relocation except the device's
`libc.so`.


## Step 28 — the second oracle, and IFUNC resolving for real

**The fleet changed.** The OnePlus 12 left and an **Oppo Reno 5 (CPH2145, Snapdragon 720G/`lito`, Android
13 / SDK 33, `abilist32=armeabi-v7a,armeabi`, 4 KiB pages)** arrived. That is a *second device that executes
32-bit ARM*, which the project has never had, and it answers the question the Tab S9 could not: are the
hardware constants device-bound?

**They are not — so far.** Every value in `oracle_libc` (35 rows: string scanning, `atoi`/`strtol`/`strtod`
including the subnormal `1e-320` at `0x00000000000007e8`, `qsort`, `strtok_r`, `snprintf`) and every value in
`oracle_linked` — the `mathProbe` seeds, the five softfp argument rows — is **identical on the two devices**,
across two different SoCs and two different bionic major versions. The one exception in quality, not value:
`strcmp("abc","abd")` is `-32` on both, so that oddity is bionic's and not one chip's. What this does *not*
yet establish is the case it was bought for — #26's Tier I divergence has no device value to compare against,
and a timing or EL0-behaviour claim still needs the Reno 5 run. M1's install-refusal evidence also needs a
**64-bit-only** device again, which is now in the drawer, not on the desk.

**A bonus measurement, immediately useful:** `/dev/cpu_variant:arm` exists on both and reports *different*
strings — `cortex-a9` on the Reno 5, `cortex-a75` on the Tab S9 (the arm32 node reports the 32-bit-side
identity, not the SoC's real cores). So the two devices select **different IFUNC bodies** and still agree on
every result above, which is the evidence that RetroBug does not have to reproduce a phone's variant choice
to reproduce its answers — the bodies differ in tuning, not in what they compute.

**IFUNC resolves now, and the load is where it had to go.** `LoadOptions::resolve_ifunc` is a callback the
loader invokes for each `IfuncSlot` **before** RELRO strips write — the shape §Step 27's measurement
demanded, and it keeps C05's "no executor needed" invariant intact for every caller that leaves it unset.
Three of the eight resolved on the first try, exactly the three that need no syscall:

```
strcmp  -> 0x956c1 (Tier J)      memcpy -> 0x94ac0      memmove -> 0x94ac0
still needing an answer: <IRELATIVE> strlen memset __strcpy_chk __memset_chk
```

and with them, for the first time, real bionic string and move code runs: `strcmp("abc","abd")` returns
negative in 30 steps, and the overlapping `memmove` produces `ABABCDEFGH` — the device's answer, byte for
byte, including the case that distinguishes `memmove` from `memcpy`.

**Two findings from the attempt, both bigger than the rows they came out of.**

1. **`FindExport` on an IFUNC name is a trap, and it was quietly returning the resolver.** With the slot
   resolved and the export read naively, `strcmp` "returned" `612033` — `0x95601`, an address inside libc's
   own text — with `reason=Returned` and nothing odd about it at all. `st_value` for an IFUNC *is* the
   resolver, so a caller that enters it gets the resolver's answer as its own result. `FindExport` now
   prefers a resolved implementation, and a lane row asserts the export is never the resolver again. This
   one nearly shipped as a green test of the wrong thing.
2. **Named GOT slots are not the whole of IFUNC: so is `.iplt`.** The non-overlapping `memcpy`/`memmove`
   fail on both tiers with `SVC #0x0`, and it is fully explained: `0x94ac0` is `memmove_a15`, its
   forward-copy path ends in `b 0xd2b00`, and `llvm-objdump` puts that address in **`.iplt`** —
   `add r12, pc, …; ldr pc, [r12, #0xed8]!`. The IPLT's target is another IFUNC slot that was never filled,
   so the stub branches to file data and both tiers faithfully execute what decodes as `svc #0`.
   The general lesson is the one this project keeps relearning: **two tiers agreeing is not evidence of
   correctness** — here it is evidence that both ran the same bad pointer. #30's scope is now eight slots
   *plus* the `.iplt` relocations.

**Also new:** Tier I's gap list grew a fifth named opcode, `PackedAddU8`, from bionic's `strcmp_a15` — so the
interpreted tier's distance to real system code is now five specific names (`Mul64`, `ByteReverseWord`,
`ConditionalSelect64`, `CountLeadingZeros32`, `PackedAddU8`) rather than a vibe.

`rb_guest_libc_tests` is **99 checks, 19 failures** and `rb_guest_apk_tests` **92 checks, 4 failures**,
*identically* on both hosts — the same three IFUNC resolutions (`0x956c1`, `0x94ac0`), the same unresolved
five, the same `__errno` answer. That equality is itself a result: the engine is deterministic across an
8 Gen 2 and a 720G host, so a divergence found later is in the code, not the silicon.

One trap that came out of it, and it is the classic one: the first Reno 5 run reported **98** checks, not
99, and nothing failed differently. The cause was that the new device had not been pushed the probe APK yet.
A missing input artifact changed the *number of assertions* — which is precisely how a silently-skipped test
looks, and the reason a fresh device has to receive the full artifact set before its first run rather than
being trusted on a green count.


## Step 29 — all eight IFUNC slots resolve, and the last Tier J gap is one instruction

**#30 finished, and it closed itself from two ends.** `LoadOptions::resolve_ifunc` plus a syscall monitor on
the lane resolves **8 of 8** IFUNC slots on the device's `libc.so`, and the resolvers asked for exactly one
syscall between them — the tally reads `openat x1`, because bionic remembers the variant after the first
probe. The implementations it picked:

```
<IRELATIVE> -> 0x949c0    strcmp -> 0x956c1    strlen -> 0x95d01    memcpy  -> 0x94ac0
memmove     -> 0x94ac0    memset -> 0x94dc0    __strcpy_chk -> 0xa7fc1   __memset_chk -> 0x94d80
```

(`memcpy` and `memmove` resolving to the *same* body is bionic's own choice, not a bug here.) What that
unlocks is wider than IFUNC: with the memory and string routines real, bionic's formatting works —
`snprintf(buf, 64, "%d/%s/%c", 42, "xy", '!')` returns **7** and leaves **`"42/xy/!"`** in the guest
buffer, matching the device, and `strlen` returns **43**, `getpid` **4242** (the monitor's own id),
`strstr` on an absent needle returns **0** where hardware returned `NULL`.

**A bug the resolution itself introduced, caught by the row that looks least important.** `memcpy` was still
copying nothing after a "successful" resolution, because `IfuncSlot::where` stored a **`vaddr`** and the
write-back went 0x2000 below its target — and *reported success*. The reason nothing else noticed is that
`FindExport` answers by name, so every name-based row was green while the slot bionic actually branches
through was untouched. The fix is a read-back: for each resolved slot, read guest memory at `where` and
require the implementation. That is the only row in the lane that could fail for that bug, and it is worth
noting as a pattern — **assert on the artifact, not on the lookup that hides it.** The `.iplt` question from
§Step 28 dissolved at the same time: decoding the stub's arithmetic (`add r12, pc, #0xc000; ldr pc,
[r12, #0xed8]`) puts its target at `0xd69e0`, which *is* the `R_ARM_IRELATIVE` slot. One unfilled word, seen
from two ends.

**The last Tier J failure in real libc is a single encoding class, and it is not `strtod`.** Both remaining
Tier J refusals — inside `strtod` and inside `malloc` — report the same instruction once the load bias is
subtracted: `0x82fdc` and `0x835f2` are both

```
e8d0 3fdf    ldaexh  r3, [r0]      <- and four bytes later, e8c0 2f53  strexh r3, r2, [r0]
```

inside `pthread_mutex_lock` / `pthread_mutex_trylock`. So what stands between Tier J and bionic's locking
paths is ARMv7's **exclusive load/store pair** (here the acquire/release halfword forms) — not a math bug and
not a libc quirk. That is a feature with memory-model consequences (a reservation address, and `LDREX`/
`STREX` semantics that a single-threaded emulator still has to define honestly), so it is #31 rather than a
tonight fix. It also explains the Tier I `the front end raised an exception at this instruction` rows: same
instructions, surfaced differently by the two tiers — which is the first case in this project where a Tier I
and a Tier J failure are the *same* underlying gap rather than a divergence.

`rb_guest_libc_tests` is now **116 checks, 17 failures**, and every one of the 17 is Tier I's five named
opcodes, `strtod`/`malloc` on the exclusive pair, or the rows that inherit them.


## Step 30 — LL/SC: the Thumb encodings, a monitor that was never wired up, and two devices that disagree

§Step 29 left one instruction standing between RetroBug and a real libc's mutexes: `e8d0 3fdf
ldaexh r3, [r0]`. It turned out to be three gaps, not one, and the middle of them was a crash rather than
a refusal.

**Which devices can prove what, settled before anything else.** Both devices on the desk were re-checked
by *executing* a static `EM_ARM` binary rather than by reading a property: the Reno 5
(`e99abf39`, `lito`/SM7250, Android 13 / SDK 33) and the Tab S9 (`R52Y106BECZ`, `kalama`/SM8550,
Android 16 / SDK 36) each print `EXEC32-OK` with `rc=0`, and each reports
`abilist32=armeabi-v7a,armeabi` with `ro.zygote=zygote64_32`. **Neither device can therefore serve as
evidence that RetroBug emulates anything** — an APK running on either could be running through the OS's
real AArch32 support. What they *are* good for, and what this step uses them for, is as an **oracle**: a
place to ask real ARMv7 hardware what an instruction does. The end-to-end claim — a guest APK that
installs and runs *because an emulator interpreted it* — still needs the OnePlus 12 (SD 8 Gen 3,
`abilist32` empty), which is not on the desk. Stated here rather than left implied, because every green
row below is an oracle row or a tier-vs-tier row, and neither is the launch claim.

**Gap 1, the decoder.** `frontend/A32/decoder/thumb32.inc` had `STREX`/`STREXB`/`STREXH`/`STREXD` and
`LDREX`/`LDREXB`/`LDREXH`/`LDREXD` — the store half and the non-acquiring load half — and no
`LDAEX*` at all, while `decoder/arm.inc:115-131` has all four ARM-state forms and
`synchronization.cpp:98-158` implements them. So an exclusive *store* decoded, its acquiring *load* did
not, and the pair could never complete in Thumb — which is where all of bionic's arm32 mutexes live.
Four rows added, with the encodings taken from LLVM rather than from arithmetic
(`clang .arch armv8-a` + `llvm-objdump`): `ldaexh r3,[r0]` = `e8d0 3fdf`, `ldaex` = `e8d0 3fef`,
`ldaexb` = `e8d0 3fcf`, `ldaexd r3,r4,[r0]` = `e8d0 34ff`. The independent pass over the file corrected
three more of my hand-written encodings — `strexh r3,r2,[r0]` is `e8c0 2f53` (the *status* register is
the low nibble, the source the high one), plain `ldrex`/`strex` live in the `e850`/`e840` group with an
`imm8` the translator scales by four, and `clrex` is `f3bf 8f2f`, which is not the 16-bit form I
assumed.

**Gap 2, the monitor — and this one was a host crash, not a refusal.** `A32::UserConfig::global_monitor`
defaults to `nullptr` (`interface/A32/config.h:126`) and the AArch64 back end dereferences it with no
check when it builds a trampoline for *any* exclusive access:
`conf.global_monitor->ReadAndMark<T>(...)` and `->DoExclusiveOperation<T>(...)`
(`backend/arm64/a32_address_space.cpp:81,136`). RetroBug set `arch_version`, the endianness, cycle
counting, the code cache and coprocessor 15, and never set it — so the ARM-state `LDAEX`/`STREX`, which
*was* translated, would have dereferenced null the first time a guest executed one. Nothing ever found
that because no test had ever executed an exclusive pair: dynarmic's own harness supplies a monitor
(`tests/A32/testenv.h:168-183`), which is the difference between "the backend implements it" and "the
backend implements it *for you*". Now a `Dynarmic::ExclusiveMonitor` member, declared before `jit_` so
destruction order keeps it alive as long as the code cache that holds its address.

**Gap 3, Tier I, and the reason the base class default is a trap.** `UserCallbacks::MemoryWriteExclusive*`
defaults to `return false` — an STREX that *always fails*. In a `ldrex/strex` retry loop that is an
infinite guest loop ending as an exhausted instruction budget with no instruction named, which is the
shape of a mysterious hang rather than of a loud failure. All four are now overridden, and Tier I gained
`A32ExclusiveReadMemory*`, `A32ExclusiveWriteMemory*` and `A32ClearExclusive`, with the reservation held
in `rb::exec::ExclusiveState` on the *executor* rather than in `BlockRunner` — a load and its store in
different basic blocks is the ordinary shape of a mutex, and a per-block record would make every store
fail and the guest spin.

**The measurement that decided the semantics, and it did not agree with itself.** `tests/guest/exclprobe.s`
runs twelve hand-written sequences on real hardware, each reporting `Rd` *and* the memory afterwards.
For "an exclusive load, then a **plain store by the same core** to the same halfword, then an exclusive
store" the two devices split:

| sequence | Reno 5 (A76/A55) | Tab S9 (A710/A510) |
|---|---|---|
| `ldrexh`; plain `strh`; `strexh` | `rd=0`, new value written | `rd=1`, plain store's value survives |
| `ldaexh`; plain `strh`; `strexh` | `rd=0` | `rd=1` |
| `ldrexh [r0]`; `strexh [r0,#2]` | `rd=0` | `rd=0` |
| plain pair / `clrex` between / no prior load | `rd=0` / `1` / `1` | `rd=0` / `1` / `1` |

So there is no "what hardware does" answer to code against — the ARM ARM leaves a same-PE write to the
reserved address implementation-defined, and these two ARMv7-A cores chose differently. The granule row
is the other half: reserving one halfword and storing the next *succeeds* on both, so a real monitor
reserves a granule wider than the access, not the exact address.

**The rule, and why its direction matters more than its content.** ARM's contract is that **STREX may
fail spuriously and software must retry**. That makes the two error directions asymmetric: failing where
hardware would pass costs one extra loop iteration, while passing where hardware would fail loses an
update a real device would have caught — which for a mutex is a silent double-lock. RetroBug therefore
takes the conservative branch on both axes: `MemoryWriteExclusive*` compares the value the exclusive
load saw against what is there now (a same-core plain store breaks the reservation, as on the Tab S9 and
not as on the Reno 5), and the reservation is exact-address rather than granule-wide. Both choices are
stricter than at least one real device and never looser than either, which is the only side it is
allowed to be on.

**What is covered, and what is not.** 13 rows in
`exclusive_access_rows_match_hardware_rule_and_each_other()` (`core/tests/exec_tier_diff_test.cpp`): the
plain pair, the conservative same-core-write row, `clrex`, no-prior-load, the exact-address row, the
`ldaexh` form bionic uses, byte/word/doubleword widths, and a retry loop that must terminate inside its
budget — the row that would catch a reservation that does not survive a block boundary. Each row claims
`Rd`, the load's own result *and* the bytes at the data address, and says whether its expectation is
"measured on hardware", "conservative versus a device that measured otherwise", or tier-vs-tier only.
Named gaps: no **ARM-state** exclusive rows (the path whose null deref this step fixed is now exercised
only through Thumb, so the ARM-state `LDAEX` remains unexecuted code), and no **two-core contention**
row — the compare against `expected` is exactly what a race would rely on, and one executor cannot
produce a race, so that claim is argued from the code rather than tested.

**The lane found a bug in itself, and the wrong bug first.** The first version of this lane came back
**419 checks, 12 failures** on both devices, all twelve in one consistent shape: `Rd` and memory agreed
across tiers, but the exclusive load's *destination register* did not — Tier I held the value the load
saw, Tier J held what the address contained after the store. That was written up here, in the commit
message, and in task #35 as a Tier J code-generation defect. **It was no such thing: the harness seeded
each row's data slot once, ran Tier I — whose entire job in these rows is to store into that slot — and
then ran Tier J against the memory Tier I had left behind.** Tier J was returning the other tier's
input, correctly. With the slot seeded immediately before each tier, the lane is **478 checks, 0
failures, green on both devices**.

What is worth keeping from the wrong reading is *why it was believable*, since that is the failure mode
to watch for:

- **The mechanism fit every row it was tested against, including the negative evidence.** f3's exclusive
  store *fails*, so the value in `r3` could not be the store's source register and had to be "whatever
  the address held last" — which is true, and true for the stale-input reason too. f11 stores exactly
  what it loads, so it is the one row where "late" and "correct" are indistinguishable, and it was the
  one row that passed. A shared input and a late read produce **identical numbers on every row in the
  table**, because the tier that runs second always sees the first tier's output as its input.
- **The one experiment run to discriminate it was not discriminating.** `RB_JIT_NOOPT=1` reproduced the
  lane byte for byte and was read as "not the optimizer, therefore the code generator". A stale input is
  unaffected by optimization flags too. The experiment could not have come out differently, which is the
  definition of a probe that cannot fail.
- **The control row that existed to widen the scope is what caught it.** Adding `ldrh r3,[r0]; ldrh
  r5,[r0]; … strh` — a row with **no exclusive instruction in it** — was meant to ask "is this about
  exclusives or about Tier J's callback loads generally?". It answered "neither", by showing an ordinary
  load misbehaving identically, which should have been the loudest possible clue that the shared
  assumption was the harness and not the engine. When a bug's fingerprint spreads to code paths that
  have been green for weeks, check what the *test* feeds both paths before believing the engine broke
  both of them.
- Two smaller artifacts from the same hour, both the same species: moving the re-seed *after* `sj = si`
  made Tier J start from Tier I's *ending PC*, so it executed one step, "returned", and reported a
  spotless agreement about nothing — a green that was emptier than the red.

**The durable rule, for every two-tier row in this project:** each tier gets its own initial state — its
own copy of the entry `MachineState` **and** its own re-seeded memory — or the comparison is not between
tiers, it is between one tier and the other's output. `#35` is closed as a probe artifact, not an engine
defect; `RB_JIT_NOOPT` stays in `core/exec/jit.cpp` as the diagnostic it was, now that its answer means
something.

What *is* established about the engine, and what the 478 green checks now cover: the exclusive pair
decodes in Thumb at all four widths, executes on both tiers, computes `Rd` correctly, writes the right
bytes, honors `CLREX`, refuses a store with no reservation, and the null-monitor crash is gone. The
16 rows break down as 8 measured on both devices, 2 where RetroBug deliberately follows the device that
fails, 1 deliberately stricter than either, and 5 asserting RetroBug's own rule. Named gaps: no
**ARM-state** exclusive rows (the path whose null deref this step fixed is exercised only through Thumb,
so ARM-state `LDAEX` remains unexecuted code), and no **two-core contention** row — the compare against
`expected` is what a race would rely on, and one executor cannot produce a race, so that claim is argued
from code rather than tested.


## Step 31 — the seven IR ops between Tier I and bionic, and the lane that had never been built

§Step 30 took out the last Tier J refusal on real `libc.so`; the lane then showed what was left, and it
was one family: instructions whose IR ops Tier I declines. Measured on both devices, the refusals are
`SignedDiv32`, `UnsignedDiv32`, `Mul64`, `ByteReverseWord`, `CountLeadingZeros32`,
`ConditionalSelect64`, `PackedAddU8` — three more than `docs/20` §9 listed, because that list was counted
against **one** device's image.

**Six implemented, one deliberately not.** `PackedAddU8` stays a named refusal: in Dynarmic it carries a
`GetGEFromOp` pseudo-operation (`backend/arm64/emit_arm64_packed.cpp:48`, which writes the per-lane
`GE` result alongside the sum), so implementing the sum without the `GE` half would make Tier I answer
half of `ADD8` and invent nothing for the other half. That is a separate piece of work, not a shortcut
to take now.

**Values measured, not recalled.** `tests/guest/divprobe.c` runs each instruction on both devices and
prints the result — 26 rows, **identical on the Reno 5 and the Tab S9** — because the interesting cases
are exactly the ones where C++ and ARM disagree and an implementation written from the C++ operator
would not notice:

| case | hardware | why it is a row |
|---|---|---|
| `sdiv 5/0`, `udiv 5/0` | `0`, and **CPSR unchanged** | ARM defines a zero result; `/` is UB, and an x86 `idiv` traps |
| `sdiv INT_MIN/-1` | `0x80000000` | ARM saturates; C++ is undefined here |
| `clz 0` | `32` | the one input `__builtin_clz` refuses to define |
| `rev 0x12345678` | `0x78563412` | a byte in every position, so a half-swap cannot pass it |
| `umull MAX × 0x03000000` | `0x02ffffff'fd000000` | the only case with a non-zero high half — the only one that can tell a real 64-bit multiply from a 32-bit one |
| `smull -1 × 0x10000` | `0xffffffff'ffff0000` | the high half is *sign extension*, which is all that separates `Smull` from `Umull` |

The lane rows are planted as raw Thumb (`ldr r1,[r0]; ldr r2,[r0,#4]; <op>; …; bx lr`, every encoding
from `clang` + `llvm-objdump --triple=thumbv8`), so the inputs are per-row constants and nothing outside
the instruction under test is load-bearing — an earlier version used `mvns` to build `-1`, and Tier I's
refusal of *that* op turned a row about `sdiv` into a row about `mvns`.

**What the differential caught: Tier I was the wrong tier.** The first run came back with exactly one
red row on both devices, `sdiv INT_MIN/-1`, and Tier J agreed with hardware while Tier I returned **0**.
The divide case had been special-cased to avoid invoking C++'s undefined behaviour and the special case
forgot to assign the result — a guard that prevents the bad computation without supplying the right one.
Fixed, and the row is the fix's evidence. One observation stays open rather than settled: with the
divisor built by `movs` instead of loaded, Tier J reported CPSR `V=1` after a divide by zero where Tier I
reported no change (hardware: no change). Both tiers now agree with the operands loaded, so the leak is
reachable only behind a preceding flag-setting instruction, and it is **not** proven to be fixed — the
repro is in this paragraph's first sentence.

**The lane nobody built, which cost an hour of wrong conclusions.** `rb_guest_libc_tests` and
`rb_guest_apk_tests` were absent from `tools/run_engine_tests.sh`'s `SUITES`, and that list drives the
build as well as the run — so `ninja` never rebuilt them, and every "the libc lane still says 17
failures" I read today was a **stale binary from before the fix**. Building the target explicitly moved
the Tab S9 lane from **116 checks / 17 failures** to **125 / 9**, and the Reno 5's from **119 / 25** to
**128 / 8** (the check counts rose because rows that used to stop at a refusal now run further and
assert more). The script now has a second list, `ADVISORY`: built, pushed, run, reported — and not
gating, which needs an explicit `|| true` because `set -o pipefail` otherwise lets a lane that exits
non-zero to *say* it is red abort the script before the next lane runs. A failure that cannot be seen is
worse than a failure that fails.

**Three rows were asserting one phone's image as if it were libc's.** All three failed only on the Reno
5, and all three were mine-or-predecessor literals rather than engine faults:

- `__errno`'s expected address was `block + 0x29c`, read out of the Tab S9's disassembly. The Reno 5's
  bionic adds **`0x298`**. Both tiers now assert the property that survives a different build — the
  answer is inside the seeded block, not its base, word-aligned, and the same address on both tiers and
  both reads — and *print* the offset, so a human sees the number the row no longer hard-codes. What the
  row can no longer catch is written in its comment: an offset that is wrong but still lands inside the
  mapping now passes.
- The strict-refusal row listed three slot addresses (`0xd67e8`, `0xd68e8`, `0xd68f0`). It now loads the
  same image twice — once tolerantly, to count what the loader refused, and once strictly — and requires
  the refusal text to name **that** many slots, so the assertion is about the loader's own consistency
  rather than about a build.
- `dtpmod == 2` / `dtpoff == 0` / `tpoff == 1`, justified by a comment claiming it compared against
  `llvm-readelf` when it compared against a transcription of one image. The Reno 5 has **no**
  `R_ARM_TLS_DTPMOD32` at all. What is universal is kept (nothing but TLS kinds is refused; the kinds
  sum to the refusals; `R_ARM_IRELATIVE` is recorded not refused; an unrecognised kind is named before it
  is implemented) and the per-image distribution became a printed value plus the command that checks it
  by hand, written into the comment.

**What the libc lane still refuses, by family** (now nearly the same list on both devices): `strcmp` on
Tier I's `PackedAddU8`; `memcpy`/`memmove` where Tier I's front end raises an exception, and the
overlapping `memmove` that hardware answers byte-exactly; `strtod` and `qsort` faulting at **low absolute
addresses** (`0xb49` read on the Tab S9, `0x2b45` written on the Reno) with **both tiers agreeing** — a
guest-side pointer derived from a null base, which is a probe-construction question and not a tier
divergence; and the guest-APK lane's 4 standing failures.

**An architecture question worth more than it looks.** Thumb-state `SDIV`/`UDIV` do not exist in
ARMv7-A — the NDK rejects them under `-march=armv7-a` ("instruction requires: divide in THUMB") and they
arrive with ARMv8-A. The device bionics contain them, so they were built for v8 in Thumb. RetroBug pins
`UserConfig::arch_version = v7` precisely so the two tiers agree on the architecture, and Dynarmic
accepts these encodings regardless (no gate anywhere consults `ArchVersion()` before translating). So
the pin is currently **decorative**: it cannot be the thing that stops the emulator executing an
instruction no armeabi-v7a binary may contain, which was the reason for setting it. Either the pin becomes
enforced — and then real bionic's `atoi` refuses to run — or it is documented as a tie-breaker for the
cases Dynarmic *does* gate on. Deciding that is a question for the owner, not a thing to quietly pick.


## Step 32 — Tier I's jump tables, what `DT_INIT_ARRAY` really was, and the actual distance to a launchable APK

**#26 was never an arithmetic bug.** Two hours of "Tier I mis-executes ARM-state libm" resolved to one
line: `Term::ReturnToDispatch` was mapped to `StopReason::Returned`. That terminal is produced only by
the ALU-writes-PC forms — `add pc, r1, r0`, which is how a compiler's `switch` or a
position-independent call reaches a jump table — so every computed branch in ARM-state code ended the
call where it should have continued. The garbage proves the mechanism on its own: guest libm's `sin`
came back as `r0:r1 = 0x0001f3ac00000058`, which is `0x2000` load bias + `0x1d3ac` (the reduction
table's address) and `0x58` (its first entry) in exactly the two registers a double return is read
from. §Step 24's written conclusion — "Tier I mis-executes ARM-state libm code", with both tiers
reaching the sentinel — is wrong on both halves, and that is why it survived this long: it was a
correct reading of a symptom whose cause was never tested. Tier I was not computing `sin` wrongly, it
was never computing `sin`, and it never reached the sentinel.

Continuing at the location `A32BXWritePC` already moved is what Dynarmic's own back end does with the
same IR (branch to its dispatcher, re-test the halt flag, translate at the new PC), and it also means a
computed jump into a host entry now stops at the host-entry check instead of reporting a return. The
result: **`rb_guest_apk_tests` is 92 checks, 0 failures on both devices** — guest libm `sin`/`cos`/`sqrt`
and every `mathProbe` argument match hardware on both tiers. Nothing else moved: engine still green,
libc lane still 8/9, so no lane was leaning on the early return.

**`DT_INIT_ARRAY` was not parsed.** Three places in these notes and `docs/20` §9 said it was "parsed /
recorded, never run". The `.dynamic` switch had cases for `DT_STRTAB` through `DT_PLTGOT`, `DT_INIT` and
`DT_FINI` — and none for `DT_INIT_ARRAY` or `DT_INIT_ARRAYSZ`, which are not even in the `using` list,
and the parser struct had no field for them. So the array was not read at all. That matters more than a
missing feature, because `DT_INIT` is **zero on every image measured here** (both devices' `libc.so` and
the NDK's): the array is the only constructor tag real images use, so a module with startup work was
reporting that it had none. Recorded now (biased, Thumb-tagged, in image order), and the gated lane
cross-checks it entry by entry against an independent read of the same file — a parse the loader got
wrong cannot agree with a parse that reads the bytes twice through a different path. **Still not run**:
constructors have to run after a dependency scope exists, which is #32's next step, not this one.

**The distance to "an APK installs and a guest JNI method runs", measured rather than estimated.** Each
claim below is file:line against the tree, not against the plan:

| what | status | evidence |
|---|---|---|
| APK read, `.so` extracted, loaded, JNI export called, result compared to hardware | **works today**, as a command-line lane | `core/tests/guest_apk_test.cpp:105-217,278,310,363,673` |
| Guest bytes never handed to the OS linker | **holds, and is checkable** | loader is `ifstream` → memfd → own `MapFile` (`guest_elf.cpp:145-191`); no `dlopen`/`android_dlopen` anywhere in `core/` |
| `DT_NEEDED` walked | **no** — parsed into `mod.needed` and never used; no cross-module symbol scope exists | `guest_elf.cpp:350,680`; the APK lane hand-builds a name→address map "standing in for the C06 linker" (`guest_apk_test.cpp:285-309`) |
| Installable RetroBug app carrying the engine | **absent** — the only `.java` in the repo is the app-domain probe, whose native library runs as *host* code | `tests/appdomain/ProbeActivity.java:30-37`, `probe.cpp:439` |
| `JNIEnv` a real app could use | 7 of 233 slots bound; no `RegisterNatives`, no `Call*Method`, no `JavaVM`; the APK lane passes `env = 0` | `core/abi/jni_env.cpp:118-186`, `guest_apk_test.cpp:646,743` |
| Syscalls a running app needs | only `getpid`/`gettid`/`exit_group`/`cacheflush`/`arm_get_tls`/`arm_set_tls`; `openat`/`read`/`mmap2`/`mprotect`/`futex` are refused, and `clone` unhandled → **guest threads cannot exist** | `core/syscall/monitor.cpp:98-169` |
| TLS relocations applied | refused, recorded, not applied (the `tp` register path and `errno` chain do work) | `guest_elf.cpp:480,568-595` |

Ordered, that is: **C06-lite** (walk `DT_NEEDED`, one global symbol scope, then run `DT_INIT`/the array,
apply the static TLS slots) → **the nine host symbols and a shippable `libc.so`** (#29) → **a host
`.so` plus a one-method Activity** so Java really calls in → **the syscall/VFS minimum**
(`openat`/`read`/`mmap2`/`futex`), which is by far the largest unknown and the one that decides whether
*a third-party* app can run at all. The first three are buildable and device-verifiable; the fourth is a
component, not an evening.

**And the thing that cannot be verified on this desk.** Both attached devices run 32-bit ARM natively
(re-confirmed tonight by executing a static `EM_ARM` binary on each, `rc=0`), so a green APK lane proves
the emulator produced the values — the two tiers agree and the guest bytes were mapped by our loader —
but it cannot prove that a 32-bit-incapable device would launch anything. That claim needs the OnePlus 12
attached, and until then every launch statement in this project has to carry the words "on 32-bit-capable
hardware" in front of it.


## Standing environment facts (so they are not re-derived)

| Fact | Value |
|---|---|
| Host compiler for Windows binaries | **none** (`gcc`/`cc`/`clang`/`cl` all absent) → M0 tests are **Android** binaries: arm64 on a phone or tablet is the primary lane (fastest, real hardware, 4 KiB pages), x86_64 in the emulator is the 16 KiB-granule lane |
| Devices available, and the difference that matters | **OnePlus 12** (`CPH2573`, SD 8 Gen 3): `abilist32` empty, kernel refuses a 32-bit ELF with ENOEXEC → M1's negative control. **SM-X710 Tab S9** (SD 8 Gen 2): `abilist32=armeabi-v7a,armeabi`, executes AArch32 → the hardware oracle. **Oppo Reno 5** (CPH2145, `lito`/SD 720G, Android 13 / SDK 33, `abilist32=armeabi-v7a,armeabi`, 4 KiB pages) joined **2026-09-20, replacing the OnePlus on the desk** — a second AArch32 host, and every `oracle_libc`/`oracle_linked` value so far is identical between the two tablets/phones despite different SoCs and bionic majors. **M1's install-refusal evidence needs the 64-bit-only device back**; ask for it rather than assuming it. Two 2023 arm64 devices, opposite capability: never infer one from the other or from a spec sheet |
| `adb push` and mtimes | the destination keeps the **source** file's mtime, so two binaries pushed in one second show different timestamps. Useful for attributing a run to a device, and the reason `ls -la /data/local/tmp` can look like a stale push when it is not |
| `adb install` on ColorOS (CPH2573) | **blocks on an on-device confirmation and never returns** — `Performing Streamed Install` is the last thing printed, with no timeout of its own. Unattended runs cannot install there; bound every device step (see `tools/app_domain_probe.sh`'s `adb()` wrapper) and say which device was skipped. `timeout` cannot wrap a shell *function*, so the bound has to live inside the wrapper |
| Two devices attached at once | `adb` refuses any command that does not name one (`more than one device/emulator`), so every script takes `RB_SERIAL` / a serial argument and prints `model · abilist32 · sdk · page` per run. A silently-chosen device is how an arm64-only phone would end up "running" the ARMv7 lane |
| A crashing device process loses its buffered stdout | `adb shell` pipes stdout, so it is block-buffered and `printf` output before a `SIGILL`/`abort` disappears. `setvbuf(stdout, NULL, _IONBF, 0)` in any probe that might die — without it, "crashed on line 1" and "crashed on the last line" are the same observable (this cost a cycle in Step 5g). **The same buffering lies the other way too**: a probe that calls `fork()` leaves the child holding the parent's unflushed buffer, so everything already printed appears a second time and the output looks like the harness ran twice (Step 11e found it that way) |
| Emulator acceleration | AEHD 2.2 **installed but stoppable**: it was "usable" for two boots, then the `aehd` service was found `Stopped` and the AVD refused to start. Re-arm with an elevated `net start aehd`; check `Get-Service aehd` before trusting `-accel-check`'s "not installed" wording. AVD `Medium_Phone` exists |
| Emulator image | `android-37.1/google_apis_playstore_ps16k/x86_64`, `abilist=x86_64,arm64-v8a`, **`abilist32=` empty** |
| `openat2`/`io_uring`/`landlock` from an app | unavailable (outside bionic's `SYSCALLS.TXT` allowlist) — see `docs/research/05` §1.4 |
| Asking the assembler what an encoding actually is | the NDK ships **`llvm-objdump` but no `llvm-mc`**, so: `clang --target=armv7-none-linux-androideabi26 -mthumb -c x.s` then `llvm-objdump -d x.o`. This is how `#5`→`0x1d50` and `#9`→`f102 0009` were settled in Step 9, and it needs no device — the answer is the same bytes the guest will load |
| ARM AAPCS argument registers, and what a JNI method can take in registers | **r0-r3 only.** r4 upwards are *workspace*, not arguments, so a 32-bit guest's fifth declared argument is on the stack; with `JNIEnv*` in r0 and `jobject`/`jclass` in r1, **a JNI method has exactly two register parameters**. `EntryState` seeds r2/r3 and provides no caller frame, so a probe with three or more declared parameters faults on its first instruction — correctly. Found the expensive way in Step 16 (a four-`jint` NEON probe), and it is a fact about the ABI rather than about RetroBug |
| Where a softfp guest puts floats, and where it puts spilled arguments | **Core registers, both directions, at the boundary.** A `jfloat` takes one of r0-r3, a `jdouble` an even/odd pair, a `jdouble` result returns in `r0:r1` — `d0..d7` carry arguments only under `-mfloat-abi=hard`, which the NDK refuses for this target (`docs/30` §Step 25's disassembly table is the primary source; §Step 24 the results half). Two traps attached: (1) a `static` function whose address never escapes its TU *does* get the VFP register ABI from clang under `softfp`, so "softfp ⇒ never VFP" is false about a disassembly and true only about a cross-boundary call; (2) the argument **block starts at the callee's entry SP** — a callee reads its fifth-and-later parameters at `[sp]`, `[sp,#8]`, … — and the 16-byte intra-procedure area is *below* that, not between it and SP |
| A real 32-bit `libc.so`, and the two things it catches you out on | **`errno` is a coprocessor read**, not a memory access: bionic's `__errno()` begins `mrc p15, #0, r0, c13, c0, #3` (TPIDRURW), and in the device's image **all 98 coprocessor accesses are that one register** with no `mcr` at all — so an emulator without a coprocessor-15 handler does not merely misbehave, it dies in Dynarmic's arm64 `ASSERT_FALSE` (§Step 26). And **`memcpy`/`memmove`/`memset`/`strlen`/`strcmp`/`strcpy`/`stpcpy`/`strcat` + the three `_chk` variants are `STT_GNU_IFUNC`**: `st_value` is a resolver and the address a caller wants is what it *returns*, so resolving them means running guest code during load. Selection is by `openat("/dev/cpu_variant:arm")` (string present at file offset 120130) with `ENOENT` ⇒ generic, so a guest syscall path has to exist before IFUNC does. Pull the device's own `/system/lib/libc.so` for the real dynamic variant: the NDK's `libc.a` is the **static** one (`docs/34` §1.3) and a relink of it is not the same artifact. |
| Executable memory from an app | permitted: `allow appdomain self:process execmem;` (`private/app.te:189`, read directly) |
| Exec from app-writable files | forbidden: `app_neverallows.te:69-73` (read directly) → why AD-5 ships one pre-built agent `.so` |
