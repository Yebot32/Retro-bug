# M2: Multi-entry core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Guest code can run on many host threads: a persistent guest process with real bionic thread structures, guest `pthread_create`/`join`/mutex/condvar/TLS keys working through `clone`/`futex`/`exit`, and ART-created threads adopted when they call into guest code — all under one Global Guest Lock (GGL), on both tiers.

**Architecture:** The libc boot that lives inside `Inspect()` (`android/jni/launcher.cpp` ~1310-2420) moves into a reusable core component `GuestProcess` (`core/runtime/`), which plays the dynamic linker's part for the scope (shared globals, auxv, static-TLS layout, main-thread `pthread_internal_t`). Per-thread state (`MachineState`, stack, tid, `clear_child_tid`, robust list, signal mask, thread name, JNI call scratch) moves into a `GuestThread`, reached through a `thread_local` pointer; one `JitExecutor` and one `InterpExecutor` serve every thread (they are stateless between `Run` calls). The GGL is held while any guest instruction runs and released around blocking syscalls, host calls and every slice boundary (preemption by step budget). `clone` starts a real host thread; `futex` blocks on a host condition variable keyed by guest address.

**Tech Stack:** C++20 (`core/`, NDK r28 via existing CMake), the tablet's 32-bit bionic `build/device-libs/libc.so` (has `.symtab`), Git Bash scripts, devices R52Y106BECZ (32-bit-capable oracle) and 75ff8cd3 (OnePlus, 64-bit only).

**Spec:** `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md` (Runtime: Memory, GGL, Thread contexts, Guest threads, Adopted threads; milestone M2) and `HANDOFF.md` "Known deviations" (4) and "Deferred".

## Global Constraints

- **GGL** (spec, mandatory): at most one host thread executes guest code at a time; released around blocking syscalls (futex wait, nanosleep/clock_nanosleep, sched_yield, read on a real fd), every host-slot call, and every preemption slice (default slice: 200,000 guest instructions); re-acquired before guest code resumes. No parallel executors in M2.
- One `JitExecutor` and one `InterpExecutor` per `GuestProcess`, shared by all threads (spec: parallel executors are "Later").
- bionic layout facts (tablet `libc.so`, file vaddr; guest = file + load base): `pthread_internal_t` size **0x2c8**; fields next +0x0, prev +0x4, tid +0x8, cached_pid +0xc, attr +0x10..+0x27, join_state +0x28, start_routine +0x30, arg +0x34, return +0x38, mmap_base +0x60, mmap_size +0x64, bionic_tls ptr +0x298, errno +0x29c, bionic_tcb ptr +0x2a0, is_main_thread byte +0x2c4. TP = tcb + 0xc; TP-4 = bionic_tls, TP+0 = DTV, TP+4 = thread struct, TP+0x14 = stack guard. Shared-globals: static TLS size +0x420, offset_bionic_tcb +0x428, offset_bionic_tls +0x42c, lock +0x43c, tls_modules count +0x464, array +0x46c. `g_thread_list` .bss 0xe40bc (4 B), `g_thread_list_lock` 0xe4094. pthread_create's clone flags **0x003d0f00**. These are facts of this one libc build: every task that relies on one asserts it at load (refuse by name if the build differs) rather than hard-coding silently.
- Locate libc internals by name from the image's `.symtab`, never by fixed address.
- Tier I and Tier J must agree on every new row; expected values for guest-thread behaviour come from the same fixture run natively on R52Y106BECZ (committed oracle file).
- "Refuse loudly": every unhandled clone flag combination, futex op, syscall or layout mismatch is a named refusal, never a silent 0.
- All existing 18 lanes stay green at every commit (`RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`); `bash tools/run_java_tests.sh` stays green; the launcher's Gumball and Angry Birds Go runs (`tools/run_launcher_on_device.sh R52Y106BECZ gumball|angry-birds-go`) keep their RESULT lines.
- Security dialogs and installs on devices are the human's; agents never confirm them.

## Review Focus

- A thread that exits while another is blocked in `pthread_join` on it: join must return (needs `clear_child_tid` write + futex wake on exit); a missed wake hangs forever — tests must have timeouts and report a hang by name.
- Two threads contending on a `pthread_mutex_t` 100,000 times each: the counter is exactly 200,000 on both tiers (exclusive monitor + kuser cmpxchg correctness under GGL preemption at arbitrary points).
- A guest thread spinning without syscalls (busy loop) must not starve others: slice preemption releases the GGL.
- An ART thread calling into guest code for the first time (no guest pthread yet): `pthread_self()`, `errno`, `pthread_getspecific` work on it, and its TLS-key destructors run when the host thread exits.
- `exit_group` from any guest thread ends the guest process cleanly (all guest threads stop; the report says so) instead of leaving host threads running guest code.

---

### Task 1: `GuestProcess` — the persistent guest process with a real main thread

**Files:**
- Create: `core/include/rb/runtime/guest_process.h`, `core/runtime/guest_process.cpp`, `core/runtime/CMakeLists.txt` (library `rb_runtime`, linked by tests and by `rbguest`)
- Modify: `core/loader/guest_elf.cpp`, `core/include/rb/loader/guest_elf.h` — `std::optional<GuestAddr> GuestModule::FindLocalSymbol(const std::string&) const` reading `.symtab`/`.strtab` from the section headers when present (biased by load base); absent section headers → nullopt.
- Test: `core/tests/guest_process_test.cpp` (new lane `rb_guest_process_tests`, gating), `core/tests/CMakeLists.txt`, `tools/run_engine_tests.sh` (push `build/device-libs/libc.so` as `/data/local/tmp/rb_device_libc.so` and the guest libm/libz/libstdc++ it needs; today those pushes are manual — make them scripted)

**Interfaces:**
- Produces:
  - `struct GuestProcessOptions { std::size_t window_bytes; bool biased; GuestAddr unresolved_trap; std::function<std::optional<GuestAddr>(const std::string&)> host_resolver; std::function<void(const std::string&)> log; int ctor_tier; /*1 I, 2 J*/ }`
  - `class GuestProcess` with `static std::unique_ptr<GuestProcess> Create(const GuestProcessOptions&, std::string* err)`; `bool LoadScope(const std::vector<std::uint8_t>& libc, libm, libz, libstdcxx, std::string* err)` (libstdcxx may be empty); `const loader::GuestModule* LoadApp(const std::vector<std::uint8_t>& image, const std::string& soname, std::string* err)`; `bool RunConstructors(std::string* failed)` (scope then apps, bionic order); accessors `mem::GuestMemory& memory()`, `abi::AbiContext& abi()`, `syscall::Monitor& monitor()`, `abi::JniEnv& jni()`, `exec::Executor& engine(int tier)`, `const loader::GuestModule* libc()`.
  - Linker-role setup inside `LoadScope`: `__loader_shared_globals` page with auxv (AT_RANDOM, AT_PAGESZ, AT_HWCAP = VFP|NEON|VFPv3|VFPv4|IDIV|TLS bits for ARMv7, AT_NULL), the static-TLS layout fields (+0x420/+0x428/+0x42c, `tls_modules` count 0) computed the way bionic's linker lays out `bionic_tcb` + `bionic_tls` (sizes derived from the image and asserted), and the main thread: a `pthread_internal_t` + tcb + bionic_tls in that layout, `is_main_thread`=1, linked as the sole entry of `g_thread_list`, `tp` set, `set_tid_address` state, stack guard at TP+0x14 = `__stack_chk_guard`'s value.
  - The trap address for unresolved imports is `unresolved_trap` (launcher keeps 0x0bad0000 with its 64 MiB window; the runtime will pass 0xfffe0000 — the allocator must never hand out pages at or above 0xfff00000).
- Note: the launcher's GL/dl/log host slots stay in `launcher.cpp`; they reach `GuestProcess` through `host_resolver`.

- [ ] **Step 1: Write the failing tests** (`guest_process_test.cpp`, both tiers each):
  - `boots_the_real_libc`: Create (64 MiB, unbiased), LoadScope(device libc + libm + libz), RunConstructors → true; `__libc_preinit` ran (the auxv `getauxval(AT_RANDOM)` call through the guest returns the page address written).
  - `main_thread_is_real`: guest `pthread_self()` returns the synthesized struct; `gettid()` == its +0x8 field; `__errno()` == struct+0x29c; `g_thread_list` head (via `FindLocalSymbol("g_thread_list")`) == the struct.
  - `static_tls_fields_match_libc`: the +0x420/+0x428/+0x42c values equal what `__init_static_tls`/`pthread_create` would compute from this image (assert the derived sizes; a different libc build must make this test FAIL with a named message).
  - `find_local_symbol`: `FindLocalSymbol("__start_thread")` == base + 0x79124 for the tablet libc; a stripped fixture (`librb_fixture.so`) → nullopt.
- [ ] **Step 2: Run** `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a` → new lane fails to build (missing class).
- [ ] **Step 3: Implement.** Move code out of `Inspect()`, don't copy it: the auxv decode of `getauxval`, IFUNC resolution, `export_of`, scope loading and constructor running become `GuestProcess` members; `Inspect()` is rewritten in Task 2.
- [ ] **Step 4: Run all lanes** → all green incl. `rb_guest_process_tests`.
- [ ] **Step 5: Commit** `git commit -m "Runtime: GuestProcess — persistent guest process with bionic's real main thread"`

### Task 2: Launcher and in-app runtime on `GuestProcess`; boot-only mode

**Files:**
- Modify: `android/jni/launcher.cpp` (`Inspect()` builds on `GuestProcess`; its per-game profiles and reports stay), `android/jni/runtime_boot.cpp`, `android/jni/inspect.h`

**Interfaces:**
- Consumes: Task 1's `GuestProcess`.
- Produces:
  - `rb_runtime_boot` keeps its signature but now: creates one **process-lifetime** `GuestProcess` (4 GiB biased window, `unresolved_trap` 0xfffe0000) on first call; loads the scope once; loads `assets/rb32/<lib>` **and any sibling in `assets/rb32/` named in its DT_NEEDED** (closes the M1 "isolated boot" deviation); runs constructors and the guest `JNI_OnLoad` synchronously (spec), **no per-game profile** (closes deviation (4)); reads zip entries by streaming one entry at a time (no whole-APK buffer); catches everything (`catch (...)` too) and names it; marks a lib booted only on success; report path `/data/user/<getuid()/100000>/<pkg>/files/rb-report.txt`, logged after a successful write.
  - `GuestProcess* rb_runtime_process()` (extern "C++", same .so) for M3 to reach the live process.

- [ ] **Step 1: Write the failing check**: bootcheck on the tablet (`-e bootcheck angry-birds-go`) must log `RBRUNTIME` lines `scope: booted once` and `RESULT: startup finished with no fault` **without** any `gumball:` / profile lines, and a second `-e bootcheck gumball` in the same process (new extra `-e bootcheck2 <name>` runs two boots back-to-back) logs `scope: reused`. Today both are absent → FAIL.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify**: bootcheck lines above on the tablet; launcher `-e run gumball` and `-e run angry-birds-go` keep their RESULT lines; all lanes green.
- [ ] **Step 4: Commit** `git commit -m "Runtime: persistent GuestProcess boot without per-game profiles"`

### Task 3: Thread contexts and the Global Guest Lock

**Files:**
- Create: `core/include/rb/runtime/guest_thread.h`, `core/runtime/guest_thread.cpp`, `core/include/rb/runtime/ggl.h`
- Modify: `core/abi/call.cpp`, `core/include/rb/abi/abi.h` (state per `GuestThread`; the run loop factored into `RunThread(GuestThread&, unsigned budget)` used by `CallGuest` and by thread entry), `core/syscall/monitor.cpp`, `monitor.h` (per-thread fields → `GuestThread`: tid, clear_child_tid, robust list, signal mask, thread name; process fields get one mutex where touched off-GGL), `core/abi/jni_env.cpp` (`calling_state_` per thread), `core/exec/interp.cpp:3011` (`static run_index` → member)
- Test: `core/tests/guest_thread_test.cpp` (new gating lane `rb_guest_thread_tests`)

**Interfaces:**
- Produces:
  - `class Ggl` (one per `GuestProcess`): `void Acquire()`, `void Release()`, RAII `Ggl::Held`, RAII `Ggl::Released` (releases for a scope, re-acquires on exit); `bool HeldByMe() const`.
  - `struct GuestThread { exec::MachineState state; GuestAddr stack_lo, stack_hi; std::int32_t tid; GuestAddr clear_child_tid; GuestAddr robust_head; std::uint32_t robust_len; std::uint64_t signal_mask; std::array<char,16> name; GuestAddr pthread_struct; bool adopted; }`; `GuestThread* CurrentGuestThread()` (thread_local).
  - `AbiContext::CallGuest` acquires the GGL if not held, runs on `CurrentGuestThread()`'s state (creating the main thread's binding on first use), slices every `kSliceSteps = 200000` instructions (release + `std::this_thread::yield()` + re-acquire), releases around host-slot calls and blocking syscalls (the monitor returns a `Verdict::kBlocking` with a callable to run unlocked).

- [ ] **Step 1: Write the failing tests** (both tiers): `two_host_threads_share_one_process` — two `std::thread`s each `CallGuest` a planted Thumb blob that loops `ldrex/add/strex` 100,000 times on one guest word (each thread with its own `GuestThread`), result exactly 200,000; same row with the kuser `__kuser_cmpxchg` loop; `busy_thread_does_not_starve` — thread A spins 50M instructions with no syscall, thread B's 10-instruction call completes within 2 s wall time; `ggl_is_released_in_host_calls` — a host slot that blocks on a host condition variable until thread B runs guest code completes (no deadlock, 5 s timeout → named failure).
- [ ] **Step 2: Run** → build failure / FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all lanes** → green; launcher runs unchanged.
- [ ] **Step 5: Commit** `git commit -m "Runtime: per-thread guest contexts under one Global Guest Lock"`

### Task 4: `clone`, `futex`, `exit`, sleeps — guest pthreads on real host threads

**Files:**
- Modify: `core/syscall/monitor.cpp`, `monitor.h` (constants + cases: clone 120, exit 1, futex ops 0/1/9/10/128|op (PRIVATE) incl. timeouts and FUTEX_WAIT_BITSET 9/WAKE_BITSET 10 as today, sched_yield 158, nanosleep 162, clock_nanosleep 265, madvise 220 (DONTNEED zero-fills anonymous pages; others 0), gettid per thread, tgkill 268 refused by name unless sig 0)
- Create: `core/runtime/futex_table.cpp` (+ header) — waiters keyed by guest address, `Wait(addr, expected, timeout)` (checks the word under the GGL, then waits with the GGL released), `Wake(addr, n)` → count woken
- Create: `tests/corpus/pthread_fixture.c` (exports `int pt_create_join(int n)`, `int pt_mutex_contend(int threads, int iters)`, `int pt_condvar_pingpong(int rounds)`, `int pt_keys_and_destructors(void)` (returns destructor-call count after N threads exit), `int pt_detached(int n)`, `int pt_once(void)`, `int pt_errno_per_thread(void)`), `tests/guest/pthread_oracle.c` (native driver printing each row's value), `tools/run_pthread_oracle.sh` (builds both for armv7, runs on R52Y106BECZ, writes `tests/corpus/oracle-pthread.txt`, committed)
- Test: extend `core/tests/guest_thread_test.cpp` with a `GuestProcess`-based rig loading the device libc + `libpthread_fixture.so`; `tools/run_engine_tests.sh` builds and pushes the fixture

**Interfaces:**
- Consumes: Task 1 `GuestProcess`, Task 3 `GuestThread`/`Ggl`.
- Produces: clone semantics for exactly bionic's flags `0x003d0f00` (other combinations refused by name): child `GuestThread` with the parent's registers, r0=0, sp=r1, tp=r3, pc after the `svc`; `*parent_tid = child tid`; `clear_child_tid = r4`; host `std::thread` started running `RunThread` until `exit`; `exit(1)` writes 0 to `clear_child_tid`, `FutexTable::Wake(addr, INT_MAX)`, ends the host thread; `exit_group` stops all guest threads (each returns at its next slice/syscall with `StopReason::Exited`).

- [ ] **Step 1: Write the failing tests:** each fixture function on Tier I and Tier J equals the oracle row (`oracle-pthread.txt`); every row runs under a 20 s watchdog that reports `HANG: <row>` by name; plus `clone_with_unknown_flags_is_refused` (planted blob calling clone with 0x11 → `-ENOSYS` and a named note).
- [ ] **Step 2: Run** `bash tools/run_pthread_oracle.sh` (commit the oracle file) then the lanes → rows FAIL (clone ENOSYS).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all lanes** on R52Y106BECZ **and** on 75ff8cd3 (`RB_SERIAL=75ff8cd3 bash tools/run_engine_tests.sh arm64-v8a` — the 64-bit-only phone must give the same answers) → green.
- [ ] **Step 5: Commit** `git commit -m "Runtime: guest pthreads — clone, futex, exit on real host threads"`

### Task 5: Adopted ART threads

**Files:**
- Modify: `core/runtime/guest_thread.cpp`, `core/runtime/guest_process.cpp` (`GuestThread& AdoptCurrentHostThread()`), `core/abi/call.cpp` (CallGuest on a host thread with no `GuestThread` adopts it)
- Test: extend `core/tests/guest_thread_test.cpp`; add to `pthread_fixture.c`: `int pt_self_tid(void)`, `int pt_key_set(int v)`, `int pt_key_get(void)`, `int pt_key_destructor_count(void)`

**Interfaces:**
- Produces: adoption synthesizes a guest stack (1 MiB + guard, mmap'd in the window), a `pthread_internal_t`/tcb/bionic_tls in the Task 1 layout (is_main_thread 0, join_state detached), links it into `g_thread_list` under the guest's `g_thread_list_lock` semantics (GGL held; lock word checked free, else refuse by name), assigns a fresh tid from the monitor; a host `pthread_key` destructor on host-thread exit acquires the GGL, runs the guest's TLS-key destructors through libc's own `pthread_key_clean_all` (found with `FindLocalSymbol`; refuse by name if absent), unlinks the struct, frees the stack.

- [ ] **Step 1: Write the failing tests** (both tiers): `art_thread_is_adopted` — a fresh `std::thread` calls `pt_self_tid()` → nonzero, != main thread's tid, and the struct is in `g_thread_list`; `pt_key_set(7)` then `pt_key_get()` on the same host thread → 7, on another adopted thread → 0; after the host thread joins, `pt_key_destructor_count()` (called from the main thread) == 1 and the struct is no longer in `g_thread_list`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run all lanes** (both phones) → green.
- [ ] **Step 5: Commit** `git commit -m "Runtime: adopt ART threads with synthesized guest pthreads"`

### Task 6: Device acceptance + handoff

**Files:** `HANDOFF.md`, spec (M2 status), `docs/superpowers/plans/` (this file's checkboxes)

- [ ] **Step 1:** Rebuild the launcher, install it on the tablet (adb) and OnePlus (human confirms), re-repack Angry Birds Go and Gumball on the OnePlus (human confirms installs — this also exercises M1's unverified "re-repack installs as an update": record `status=0` and unchanged `firstInstallTime` from `adb shell dumpsys package <pkg>`).
- [ ] **Step 2:** Launch each by its icon; expected `RBRUNTIME`: `scope: booted once`, constructors and `RESULT: startup finished with no fault` with no profile lines; then the documented end point (`UnsatisfiedLinkError` at the first `Java_*` for Angry Birds Go; Gumball's targetSdk-compat crash or its first `Java_*`). Record pids, timings and the exact end-point symbol.
- [ ] **Step 3:** All lanes green on both phones; `bash tools/run_java_tests.sh` green.
- [ ] **Step 4:** HANDOFF: M2 section (what exists, commands, evidence, deviations closed: (1b) isolated boot, (4) profile-in-boot; remaining deviations; Next = targetSdk shims then M3). Commit `git commit -m "M2 accepted: guest threads on real host threads under one lock"`.
