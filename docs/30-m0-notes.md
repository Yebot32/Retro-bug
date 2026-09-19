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

## Standing environment facts (so they are not re-derived)

| Fact | Value |
|---|---|
| Host compiler for Windows binaries | **none** (`gcc`/`cc`/`clang`/`cl` all absent) → M0 tests are x86_64 **Android** binaries in the emulator |
| Emulator acceleration | AEHD 2.2 **installed but stoppable**: it was "usable" for two boots, then the `aehd` service was found `Stopped` and the AVD refused to start. Re-arm with an elevated `net start aehd`; check `Get-Service aehd` before trusting `-accel-check`'s "not installed" wording. AVD `Medium_Phone` exists |
| Emulator image | `android-37.1/google_apis_playstore_ps16k/x86_64`, `abilist=x86_64,arm64-v8a`, **`abilist32=` empty** |
| `openat2`/`io_uring`/`landlock` from an app | unavailable (outside bionic's `SYSCALLS.TXT` allowlist) — see `docs/research/05` §1.4 |
| Executable memory from an app | permitted: `allow appdomain self:process execmem;` (`private/app.te:189`, read directly) |
| Exec from app-writable files | forbidden: `app_neverallows.te:69-73` (read directly) → why AD-5 ships one pre-built agent `.so` |
