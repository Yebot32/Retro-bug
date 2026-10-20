/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C07 `rb::syscall` — the monitor between a guest SVC and whatever RetroBug decides to do about it.
 *
 * The posture this slice takes is the strictest one available: **nothing is forwarded to the host kernel.**
 * Every number is answered inside RetroBug or refused, and the forwarding allowlist the plan requires
 * (`docs/20` §C07: "no syscall reaches the host kernel except through an explicit allowlist here") is
 * empty and *says so* rather than being absent. An empty allowlist cannot be widened by accident, and it
 * means the whole component is testable with no host side effects at all — which is the phase-appropriate
 * choice, because the calls that will need forwarding (`read`, `write`, `futex`, `mmap2`) are the ones
 * whose semantics depend on C10's VFS and C07's own thread work, not the other way round.
 *
 * The calls a real libc makes during its own startup are answered from four sources, none of which is
 * the host's syscall table:
 *
 *   - **facts the host knows** — the process's own id (`set_ids`, from the launcher), the wall and
 *     monotonic clocks, the thread pointer, entropy. A fact is answered with the fact or refused by
 *     name; it is never estimated. (`getauxval` is deliberately not here: on a real system it is a read
 *     of the auxiliary vector the kernel wrote at process entry, not an `SVC`, so the guest's libc
 *     reaches it through the linker's `__libc_shared_globals`, which is the launcher's stub and not
 *     this monitor's business.)
 *   - **a file provider the launcher installs** — a `std::function` from guest path to bytes. Bytes, not
 *     a host fd, so there is no host file descriptor anywhere for a guest to reach through twice, and
 *     nothing in this file can open a path the launcher did not answer. A path the provider does not
 *     serve is `ENOENT`, which is what a real Android answers and is *also* the answer bionic's IFUNC
 *     resolvers are built to survive (their fallback for `/dev/cpu_variant:arm` is a plain `bmi` on the
 *     negative return, read out of the device's own `libc.so` at `get_cpu_variant+0x26`).
 *   - **`ENOSYS` naming the call** — everything else. The string starts `svc #<n> (<name>) is not
 *     implemented`, so a reader on a phone can tell "no monitor was attached" from "the monitor does not
 *     know this call" from "the call was answered with ENOENT".
 *   - **a grant recorded and not enforced** — `mprotect`, and it is the one deliberate deviation from a
 *     device here. A real libc's startup strips write from its own globals page and turns a refusal into a
 *     fatal report, so the call answers the 0 a device answers, and its note says in the words a report
 *     carries that this monitor does not apply the request to the guest's pages: a guest that later writes
 *     what a device's `PROT_READ` page would refuse is the difference, stated rather than hidden.
 *
 * Two numbers here are measured rather than transcribed, from `tests/corpus/syscall-abi-arm.txt`
 * (`tools/syscall_abi_probe.sh`, executed on an SM-X710 as an `armeabi-v7a` process):
 *
 *   - the guest-side syscall numbers. Every name shared with the host's AArch64 table **differs** —
 *     `read` is 3 here and 63 there, `write` 4 and 64, `gettid` 224 and 178, and so on for all
 *     fourteen — so a future forwarding row that reuses a number instead of translating by name is wrong
 *     in every case, not in some.
 *   - the return convention. The ARM EABI story usually told is "negative errno in r0 with CPSR.C set,
 *     clear on success". Measured on this kernel: C is set after *every* return tried, including
 *     `sched_yield` returning 0. So the flag carries no error information to the guest, and the only
 *     discriminator is the value range — which is why `ApplyReturn` writes r0 and deliberately leaves
 *     CPSR alone. Synthesising "C on error" would have made RetroBug's guests differ from every real
 *     32-bit Android process in exactly the way that shows up only under a hand-written stub.
 *   - and the errno tables agree. `ENAMETOOLONG` is the case that decides it: the ARM *kernel* EABI table
 *     is historically 62, bionic's header says 36, and a real `openat` of an over-long name returned
 *     **-36** — so the kernel this runs on speaks bionic's numbers, and every one of the 47 names
 *     sampled is equal on both sides. The map therefore stays *by name* with an identity result today,
 *     which is a different claim from "no map needed": `docs/20` §C07's invariant said guest errnos are
 *     "not host ones", and on this pair that is not true — recorded where it would otherwise be
 *     re-assumed wrongly.
 *
 * See docs/20 §C07 and docs/30 §Step 11.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "rb/diag/diag.h"
#include "rb/exec/machine_state.h"
#include "rb/runtime/futex_table.h"
#include "rb/runtime/guest_thread.h"

namespace rb::mem {
class GuestMemory;  ///< the monitor never owns it: the launcher builds one and hands it over
}  // namespace rb::mem

namespace rb::syscall {

/// ARM EABI's private base. `__NR_SYSCALL_BASE` is 0 on ARM (measured), so the generic numbers are the
/// bare values and anything at or above this base is an ARM-specific call the host has no counterpart for
/// — which is why dispatch looks at the range as well as the value.
constexpr std::uint32_t kArmNrBase = 0x000f0000u;

/// There is deliberately **no mask constant here**. `__NR_SYSCALL_MASK` in the kernel's own header is a
/// bounds-check constant, not a mask applied to `r7`: the kernel takes `r7` as the syscall number and
/// answers `-ENOSYS` past its table. An earlier revision of this file defined `kSyscallMask` and masked
/// `r7` with `0x000fffff`, which aliased `0x00100003` onto `read` and *executed* it where the kernel
/// would have refused — and the same revision returned `ENOSYS` for numbers the kernel serves. The
/// number goes to dispatch untouched, and a constant kept "for the ARM private range check" was removed
/// with this comment because no such check exists; `kArmNrBase` above is what the range test uses.

/// The ARM Linux error range: `r0` in [-4095, -1] is `-errno`. See the measurement above — this, not the
/// carry flag, is what a guest's own glue tests.
constexpr std::int32_t kMaxErrno = 4095;

/// Guest-side numbers, from the `arm-eabi` column of `tests/corpus/syscall-abi-map.txt`. Only the ones
/// this file actually uses are given a name; the rest are deliberately not a copy of a kernel header.
namespace nr {
/// `exit`: ends the calling *thread* (bionic's `__exit`, reached from `pthread_exit`); `exit_group` ends the process.
constexpr std::uint32_t kExit = 1;
constexpr std::uint32_t kRead = 3;
constexpr std::uint32_t kWrite = 4;
constexpr std::uint32_t kClose = 6;
constexpr std::uint32_t kGetpid = 20;
/// From here down: the calls a real libc's own startup reaches for. The numbers are from the NDK's
/// `arm-linux-androideabi/asm/unistd-eabi.h` — the header the guest itself is compiled against — and the
/// ones the OnePlus 12 run actually used were checked a second way, against the device's `libc.so`: its
/// `__openat`, `__close`, `__mmap2`, `fstat64`, `__rt_sigprocmask`, `__set_tid_address` and `getrandom`
/// stubs load r7 with exactly these values (`llvm-objdump -d build/device-libs/libc.so`).
constexpr std::uint32_t kBrk = 45;
constexpr std::uint32_t kIoctl = 54;
constexpr std::uint32_t kGettimeofday = 78;
constexpr std::uint32_t kMunmap = 91;
constexpr std::uint32_t kFstat = 108;  ///< the 32-bit-time `stat`; bionic does not use it
/// `clone`: bionic's `__bionic_clone` (r7 = 120, measured in the tablet libc). Only `pthread_create`'s flags are served.
constexpr std::uint32_t kClone = 120;
constexpr std::uint32_t kMprotect = 125;
constexpr std::uint32_t kSchedYield = 158;  ///< blocking: answered with the GGL released (M2)
constexpr std::uint32_t kNanosleep = 162;   ///< blocking: answered with the GGL released (M2)
constexpr std::uint32_t kMremap = 163;
constexpr std::uint32_t kWritev = 146;
constexpr std::uint32_t kPrctl = 172;
/// `rt_sigreturn` (173): the call a signal handler returns through. AArch32 requires `SA_RESTORER`, so a
/// guest handler never issues this itself — the restorer it registered does
/// (`mov r7, #__NR_rt_sigreturn; svc 0`), which is why delivery may rely on one existing. The number is
/// from the NDK's own `arm-linux-androideabi/asm/unistd.h:143`, the header the guest is compiled against.
constexpr std::uint32_t kRtSigreturn = 173;
constexpr std::uint32_t kRtSigaction = 174;
constexpr std::uint32_t kRtSigprocmask = 175;
/// `rt_sigsuspend` (179, from the NDK's arm-linux-androideabi/asm/unistd-eabi.h): the call Mono's suspend
/// handler makes while it waits for its restart signal. It swaps the blocked mask, sleeps until a
/// deliverable signal is queued, then returns EINTR; delivery runs at the next ABI boundary and the old
/// mask is put back after the handler's `rt_sigreturn`.
constexpr std::uint32_t kRtSigsuspend = 179;
constexpr std::uint32_t kMmap2 = 192;
constexpr std::uint32_t kFstat64 = 197;
constexpr std::uint32_t kGetdents64 = 217;
constexpr std::uint32_t kMadvise = 220;
constexpr std::uint32_t kFcntl64 = 221;
constexpr std::uint32_t kGettid = 224;
constexpr std::uint32_t kFutex = 240;
constexpr std::uint32_t kExitGroup = 248;
constexpr std::uint32_t kSetTidAddress = 256;
constexpr std::uint32_t kClockGettime = 263;
constexpr std::uint32_t kClockNanosleep = 265;
constexpr std::uint32_t kTgkill = 268;
/// `tkill` is *not* `tgkill` and was missed by the first survey of what a Unity boot asks for: the same fatal
/// pair is written down here twice under two numbers, and a scan for 268 alone reads a process that never
/// signals anything. Measured (SM-X710 2026-10-19): Cars 3 asks 264, then 238, then 363, and leaves through
/// `exit_group(127)` two microseconds later — the refusal, not the signal, is what ends it.
constexpr std::uint32_t kTkill = 238;
/// `rt_tgsigqueueinfo`: `tgkill` carrying a `siginfo_t`. Same family, same answer.
constexpr std::uint32_t kRtTgsigqueueinfo = 363;
/// The gaps C34 names, each one a call that has a true answer on the host and had none here. `clock_getres`
/// is the first of them: Cars 3 asks it immediately before the two signal calls above.
constexpr std::uint32_t kClockGetres = 264;
constexpr std::uint32_t kUname = 122;
constexpr std::uint32_t kSysinfo = 116;
constexpr std::uint32_t kSchedSetaffinity = 241;
constexpr std::uint32_t kSchedGetaffinity = 242;
constexpr std::uint32_t kPrlimit64 = 369;
/// The scheduler family a Mono game reaches the moment it starts a GC thread. Measured (SM-X710 2026-10-19):
/// once the signal calls above stopped ending Cars 3, it asked 155, 156, 159 and 160 in a tight loop — the
/// calls a thread makes when it sets its own scheduling policy before parking. Each has a true answer here.
constexpr std::uint32_t kSchedGetparam = 155;
constexpr std::uint32_t kSchedSetscheduler = 156;
constexpr std::uint32_t kSchedGetscheduler = 157;
constexpr std::uint32_t kSchedGetPriorityMax = 159;
constexpr std::uint32_t kSchedGetPriorityMin = 160;
constexpr std::uint32_t kSetpriority = 97;
constexpr std::uint32_t kGetpriority = 96;
constexpr std::uint32_t kOpenat = 322;
/// ARM's name for what the 64-bit ABIs call `newfstatat`: there is no separate `newfstatat` number here,
/// and the NDK header has only this one (`unistd-eabi.h:291`).
constexpr std::uint32_t kFstatat64 = 327;
constexpr std::uint32_t kReadlinkat = 332;
constexpr std::uint32_t kFaccessat = 334;
constexpr std::uint32_t kSetRobustList = 338;
constexpr std::uint32_t kUtimensat = 348;
constexpr std::uint32_t kGetrandom = 384;
/// `clock_gettime` for a 64-bit `time_t`, which is what a bionic built for modern API levels reaches for
/// when the vDSO is not there to answer it (`docs/research/02` §5.3 measured the pair 263/403).
constexpr std::uint32_t kClockGettime64 = 403;
/// Measured refusal, not a gap: an `armeabi-v7a` process in `u:r:untrusted_app` on an SM-X710 asked for
/// `execve` of a file the app itself had just written into its own cache directory and got **EACCES**
/// (`tests/appdomain/results-SM-X710-armv7.txt`). That is the SELinux `app_data_file:file
/// execute_no_trans` rule, and it is the answer `docs/20` §C07's "refused with the same errno a real
/// 32-bit Android would give" was asking for — as opposed to `ENOSYS`, which is what a missing handler
/// says and would have been a wrong answer that looks right.
constexpr std::uint32_t kExecve = 11;
constexpr std::uint32_t kCacheFlush = kArmNrBase + 2;
constexpr std::uint32_t kSetTls = kArmNrBase + 5;
constexpr std::uint32_t kGetTls = kArmNrBase + 6;
}  // namespace nr

/// Errno values as *guests* see them. Measured identical to the host's on the ABI pair RetroBug runs
/// (see the header), so the identity mapping below is a result and not an assumption; `ToGuest` exists so
/// the translation is one editable function the day a case appears where it is not identity.
namespace err {
constexpr std::uint32_t kEperm = 1;
constexpr std::uint32_t kEnoent = 2;
constexpr std::uint32_t kEsrch = 3;  ///< tgkill of a tid that is not a live thread of this process
constexpr std::uint32_t kEintr = 4;
constexpr std::uint32_t kEbadf = 9;
constexpr std::uint32_t kEagain = 11;
constexpr std::uint32_t kEnomem = 12;
constexpr std::uint32_t kEacces = 13;
constexpr std::uint32_t kEfault = 14;
constexpr std::uint32_t kEexist = 17;
constexpr std::uint32_t kEnotdir = 20;  ///< what a file-only provider answers an `O_DIRECTORY` open with
constexpr std::uint32_t kEinval = 22;
constexpr std::uint32_t kEmfile = 24;  ///< the synthetic fd table is full
constexpr std::uint32_t kEnospc = 28;
constexpr std::uint32_t kErofs = 30;
constexpr std::uint32_t kErrange = 34;
constexpr std::uint32_t kEnametoolong = 36;   ///< measured: -36 from a real openat, not the 62 of history
constexpr std::uint32_t kEnosys = 38;
constexpr std::uint32_t kEoverflow = 75;
constexpr std::uint32_t kEtimedout = 110;  ///< a futex wait or a timed sleep whose deadline passed

/// Host errno to guest errno. Identity today, measured on the SM-X710 for all 47 names sampled; the
/// function exists so that "identity" is a value in one place rather than a property of the code.
[[nodiscard]] constexpr std::uint32_t ToGuest(std::int32_t host_errno) {
  return static_cast<std::uint32_t>(host_errno);
}
}  // namespace err

/// What the monitor decided to do. `kForward` is a verdict with no implementation yet — the allowlist is
/// empty by design, and having the state named means a future row is a change of data, not of shape.
enum class Verdict {
  kEmulated,   ///< RetroBug answered it in guest terms.
  kDenied,     ///< refused with a specific errno, on purpose (policy, not a gap).
  kUnsupported, ///< RetroBug has no answer: refused with ENOSYS, which is what the real kernel does.
  kExit,       ///< the instance is finished; `r0` is the exit code.
  kForward,    ///< handed to the host kernel through the allowlist. Never returned today.
  /// The call waits (a sleep, a yield; Task 4's futex WAIT). Only `Monitor::Begin` returns it: the wait is
  /// `Outcome::block`, which the caller runs with the Global Guest Lock *released* — so other guest threads
  /// run meanwhile — and whose result it hands to `Monitor::Finish` once the lock is held again. The callable
  /// captures what it needs by value and touches no monitor state.
  kBlocking,
  /// `exit` (1): the calling guest *thread* is finished (its clear-child-tid word zeroed and woken); `r0` is
  /// its status. The bridge's run loop returns `StopReason::Exited` for that thread only — the process and
  /// its other threads carry on (contrast `kExit`, which is `exit_group`).
  kThreadExit,
};

/// The one `clone` flag set served: bionic `pthread_create`'s, read from the tablet libc (`clone(flags
/// 0x003d0f00)` -> `__bionic_clone`): CLONE_VM | FS | FILES | SIGHAND | THREAD | SYSVSEM | SETTLS |
/// PARENT_SETTID | CHILD_CLEARTID. Anything else (fork, vfork, a new namespace) is refused by name.
constexpr std::uint32_t kBionicCloneFlags = 0x003d'0f00u;

[[nodiscard]] const char* Name(Verdict v);

/// The name of a guest syscall number, or nullptr when this file has no name for it. A table of strings
/// the monitor prints, not a dispatch key: a wrong entry here changes a log line, never a decision.
[[nodiscard]] const char* Name(std::uint32_t number);

/// `Name` with the number as fallback, so a refusal always identifies what was refused.
[[nodiscard]] std::string Describe(std::uint32_t number);

/// True for the range a guest's own glue tests. Taking `int32_t` rather than a `Value` keeps this usable
/// on a host return value too, which is the direction that matters once anything forwards.
[[nodiscard]] constexpr bool LooksLikeError(std::int32_t r0) {
  return r0 < 0 && r0 >= -kMaxErrno;
}

struct Outcome {
  Verdict verdict = Verdict::kUnsupported;
  /// The value the guest sees in r0. On an error this is already the negative guest errno — the sign is
  /// applied here, once, so a handler cannot half-do it.
  std::int32_t r0 = 0;
  /// Why, in the terms a reader of a log needs: which name, which policy row, what was missing.
  std::string detail;
  /// `Verdict::kBlocking` only: the wait itself, run without the GGL; its result is the call's answer.
  std::function<Outcome()> block;

  [[nodiscard]] static Outcome Emulated(std::int32_t value, std::string why = {}) {
    return Outcome{Verdict::kEmulated, value, std::move(why)};
  }
  /// The call *was* serviced, and the service itself is an errno — a path the provider does not have
  /// (`ENOENT`), a descriptor number that is not open (`EBADF`), a write-intent open of a read-only
  /// provider (`EROFS`). `kEmulated` rather than `kUnsupported` because nothing is missing from
  /// RetroBug here: a report that could not tell this from "no handler" is the distinction the sharper
  /// reason text exists for.
  [[nodiscard]] static Outcome Answered(std::uint32_t guest_errno, std::string why) {
    return Outcome{Verdict::kEmulated, -static_cast<std::int32_t>(guest_errno), std::move(why)};
  }
  [[nodiscard]] static Outcome Refused(std::uint32_t guest_errno, std::string why) {
    return Outcome{Verdict::kUnsupported, -static_cast<std::int32_t>(guest_errno), std::move(why)};
  }
  [[nodiscard]] static Outcome Denied(std::uint32_t guest_errno, std::string why) {
    return Outcome{Verdict::kDenied, -static_cast<std::int32_t>(guest_errno), std::move(why)};
  }
};

/// One syscall as the guest left it. Held separately from `MachineState` so a monitor can be tested with
/// a literal frame, and so the fields that matter for dispatch cannot be confused with the ones that
/// matter for the result.
struct Frame {
  std::uint32_t number = 0;  ///< r7, taken as the syscall number — see the note above `kArmNrBase`
  std::uint32_t imm = 0;     ///< the SVC immediate — a channel, not the number
  std::uint32_t arg[6] = {};  ///< r0..r5
  exec::GuestAddr pc = 0;    ///< already past the SVC, in both tiers (see `StopReason::Syscall`)

  /// `r7` *is* the number. It is captured whole and dispatched on whole, because that is what the kernel
  /// does: a monitor that masked it turned a garbage number into a valid one and then served it.
  [[nodiscard]] static Frame Capture(const exec::MachineState& state, std::uint32_t svc_imm);

  /// Puts the result back the way the kernel does: r0 only, CPSR untouched — see the measurement in the
  /// header. Returns the value written, for a caller that wants to log it without re-deriving it.
  std::uint32_t ApplyReturn(exec::MachineState& state, const Outcome& outcome) const;
};

/// The launcher's read-only view of the files this instance may see. `guest_path` is the NUL-terminated
/// path the guest passed, copied out of guest memory; the answer is the file's bytes, or `nullopt` for
/// "this path is not served here", which the monitor turns into `ENOENT`.
///
/// Bytes rather than a host file descriptor, and that is the isolation property: a guest can never hold
/// or guess something that *is* a host fd, there is no offset/lock/`mmap` state that outlives the call,
/// and the only paths that ever resolve are the ones this function was written to answer. It is also why
/// the provider is the whole of C10's surface for now: the mount trie, the fd capability table and
/// synthetic `/proc` belong behind this same answer.
using FileProvider = std::function<std::optional<std::vector<std::uint8_t>>(const std::string& path)>;

/// A clock fact: nanoseconds for `clock_id` (`CLOCK_REALTIME` 0, `CLOCK_MONOTONIC` 1), false when this
/// source has no fact for that id. Unset, the monitor reads the host's own clocks — the fact the host
/// has — rather than guessing one.
using ClockSource = std::function<bool(std::int32_t clock_id, std::int64_t* nanos)>;

/// `getrandom`'s bytes. Unset, the monitor asks the host's entropy pool, because a constant here would
/// be a fabricated answer handed to whatever is seeding a cookie or a key — the one syscall whose whole
/// contract is "these bytes are not predictable".
using RandomSource = std::function<bool(std::uint8_t* dst, std::size_t len)>;

/// Starts a host thread for a guest thread `clone` just made (registers, tid, clear-child-tid all set). The
/// owner of the threads (`GuestProcess`) installs it; it is called with the GGL held, so the new host thread
/// cannot run guest code before the parent's `clone` has returned and released the lock. Returns an empty
/// string on success, or why the thread could not be started (the guest then sees EAGAIN, as from a kernel
/// out of threads).
using ThreadSpawner = std::function<std::string(std::unique_ptr<runtime::GuestThread> thread)>;

/// The monitor. One per guest instance, like everything else that is per-instance: a refusal tally that
/// mixes apps would be a diagnostics lie, and no isolation property is shared across apps.
///
/// Threads (M2): every entry point that dispatches (`Handle`, `Begin`, `Finish`, `Classify`) is called with
/// the process's Global Guest Lock held — the bridge's run loop is the only caller with more than one guest
/// thread — so the process-wide tables here (fd table, allocator cursor, tally, notes, captured output) need
/// no lock of their own: nothing touches them off the GGL. A blocking call's wait (`Outcome::block`) is the
/// one piece that runs without the GGL, and it touches no monitor state. The accessors (`tally`, `notes`,
/// captured output) are for a reader that holds the GGL or knows no guest thread is running.
class Monitor {
 public:
  /// Self-modifying code (AD-7): ARM's `cacheflush` is how a guest says "the bytes you compiled are no
  /// longer the bytes in memory". The tier's `Invalidate` is the same call both tiers already answer to.
  using Invalidator = std::function<void(exec::GuestAddr from, exec::GuestAddr to)>;

  void set_invalidator(Invalidator inv) { invalidator_ = std::move(inv); }

  /// The identity guests see for `getpid`/`gettid`. Nothing in here may ask the host kernel — see
  /// `set_ids`'s caller in the launch path — so an unset id is refused rather than invented.
  void set_ids(std::int32_t pid, std::int32_t tid) {
    pid_ = pid;
    main_->tid = tid;
    ids_set_ = true;
  }

  /// The main guest thread's per-thread state (tid, clear-child-tid, robust list, signal mask, name) — what
  /// a syscall is answered from when its caller names no thread. The monitor holds its own until a bridge
  /// attaches its main `GuestThread` here (`AbiContext::set_monitor`), which takes over the values set so far
  /// so `set_ids` / `set_clear_child_tid` before or after the attach mean the same thread.
  void set_main_thread(runtime::GuestThread* t);
  [[nodiscard]] runtime::GuestThread& main_thread() { return *main_; }

  /// The pointer-shaped calls need guest memory: to copy a path out, to copy a buffer in or out, to
  /// write a `timespec` or a `stat64`. Without one attached, every such call is a named refusal rather
  /// than a write to address 0 or a silently skipped copy.
  void set_memory(mem::GuestMemory* memory) { memory_ = memory; }

  /// Where a guest `openat` is answered from. Unset is a named refusal, not an empty file.
  void set_file_provider(FileProvider provider) { files_ = std::move(provider); }

  /// M4: file and socket syscalls on real paths and host descriptors go to the host kernel (host_io.h), with the
  /// app's own permissions; the provider still answers its own paths first, and its descriptors keep their handlers.
  void set_host_io(bool on) { host_io_ = on; }
  [[nodiscard]] bool host_io() const { return host_io_; }
  /// Receives the answer line of every host-I/O call as it completes (diagnostics; unset = nothing is formatted twice).
  void set_host_io_trace(std::function<void(const std::string&)> trace) { host_io_trace_ = std::move(trace); }
  /// Receives every `rt_sigaction` as it is recorded, on its own channel so the notes' dedup and cap cannot
  /// hide it. The signal-disposition set is what C26's delivery has to be built against, and it is a fact
  /// about the guest that has to be readable from a live log rather than inferred from a capped report.
  void set_signal_trace(std::function<void(const std::string&)> trace) { signal_trace_ = std::move(trace); }

  /// The signal dispositions the guest has installed, by signal number: the handler address (0 = SIG_DFL,
  /// 1 = SIG_IGN), the flags it asked for, the restorer, and its blocked mask. `rt_sigaction` is the only
  /// writer.
  ///
  /// The layout was read off the guest rather than taken from a header (SM-X710 2026-10-19, Cars 3):
  /// `rt_sigaction(sig=30)` passed `+0=0x5fab59 +4=0x14000000 +8=0x1a7240`, so bionic's struct is
  /// `{handler, flags, restorer, mask}` — flags at +4, not mask, and `SA_RESTORER` (0x04000000) is set in
  /// practice on AArch32 exactly as docs/research/02 §5.4 says it must be.
  struct Disposition {
    std::uint32_t handler = 0;
    std::uint32_t flags = 0;
    std::uint32_t restorer = 0;  ///< `sa_restorer`; required on AArch32, and the return path a delivery uses
    std::uint64_t mask = 0;
    [[nodiscard]] bool catches() const { return handler > 1; }  ///< a real handler, not SIG_DFL/SIG_IGN
  };
  [[nodiscard]] Disposition disposition(int sig) const {
    const auto it = sig_dispositions_.find(sig);
    return it == sig_dispositions_.end() ? Disposition{} : it->second;
  }
  /// Every disposition the guest has installed, for a report or a test.
  [[nodiscard]] const std::map<int, Disposition>& dispositions() const { return sig_dispositions_; }
  /// How many signals the guest has installed a real handler for (neither SIG_DFL nor SIG_IGN). The number a
  /// signal-delivery report should quote, because it is what delivery can actually enter.
  [[nodiscard]] std::size_t installed_handler_count() const {
    std::size_t n = 0;
    for (const auto& [sig, d] : sig_dispositions_) {
      if (d.catches()) ++n;
    }
    return n;
  }

  /// ---- C26: pending signals, per target tid.
  ///
  /// A signal a guest sends (`tkill`/`tgkill`/`rt_tgsigqueueinfo`) is queued here instead of being refused.
  /// The target thread picks it up at the ABI's next slice boundary and enters the handler the guest
  /// installed — which is what Mono's GC suspend/restart protocol needs, and what its absence turned into
  /// `exit(127)` on two games. Only the signal number is remembered: nothing in the eight Unity boots read
  /// back a `siginfo_t`, and a fabricated one would be a wrong answer where a missing one is an honest gap.
  void QueueSignal(std::int32_t tid, int sig) {
    if (sig < 1 || sig > 64) return;  // the caller turns an invalid number into EINVAL; never store it
    {
      std::lock_guard<std::mutex> lk(sig_mu_);
      auto& q = pending_[tid];
      // Standard signals coalesce: a second SIGPWR while one is already pending is the same signal, and a
      // tkill loop must not grow this vector without bound (measured: MadTruck retries billing every frame).
      if (std::find(q.begin(), q.end(), sig) != q.end()) return;
      if (q.size() >= 64) return;  // the whole standard-signal space is 64; a queue longer than that is a bug
      q.push_back(sig);
      ++signals_queued_;
    }
    // A signal wakes a blocked thread as EINTR, exactly as the kernel does — but only when the thread
    // has not blocked that signal. Interrupting for a blocked one made Unity log "Failed to wait on a
    // semaphore (Interrupted system call)" in a loop (MadTruck, 2026-10-20) where a device would not
    // have woken the wait at all. The lookup is installed by GuestProcess; a bare monitor has none and
    // keeps the old behaviour (nothing in a unit test blocks a signal it then expects to interrupt).
    bool deliverable = true;
    if (thread_lookup_ != nullptr) {
      if (runtime::GuestThread* target = thread_lookup_(tid)) {
        if (sig >= 1 && sig <= 64 && (target->signal_mask & (1ull << (sig - 1))) != 0) deliverable = false;
      }
    }
    if (deliverable && futex_ != nullptr) futex_->Interrupt(tid);
  }
  /// Takes the next pending signal for `tid`, or 0 when there is none. Removed, not marked: a blocked
  /// delivery re-queues it through `QueueSignal`, so repeated looks cannot grow the queue.
  int TakePendingSignal(std::int32_t tid) {
    std::lock_guard<std::mutex> lk(sig_mu_);
    const auto it = pending_.find(tid);
    if (it == pending_.end() || it->second.empty()) return 0;
    const int sig = it->second.front();
    it->second.erase(it->second.begin());
    if (it->second.empty()) pending_.erase(it);
    return sig;
  }
  /// True when a signal for `tid` is pending and its bit is clear in `mask`. `rt_sigsuspend` uses this to
  /// answer without blocking when the signal it waits for is already there.
  [[nodiscard]] bool HasDeliverablePending(std::int32_t tid, std::uint64_t mask) const {
    std::lock_guard<std::mutex> lk(sig_mu_);
    const auto it = pending_.find(tid);
    if (it == pending_.end()) return false;
    for (const int sig : it->second) {
      if (sig >= 1 && sig <= 64 && (mask & (1ull << (sig - 1))) == 0) return true;
    }
    return false;
  }
  [[nodiscard]] std::size_t signals_queued() const { return signals_queued_.load(); }
  [[nodiscard]] std::size_t signals_delivered() const { return signals_delivered_.load(); }
  void CountDelivered() { signals_delivered_.fetch_add(1); }

  /// ---- C26: the return path of a delivery.
  ///
  /// `rt_sigreturn` is issued by the restorer the guest registered, and what it must do — read the
  /// sigcontext the delivery wrote and put the interrupted register file back — is the *runtime's*
  /// knowledge, not the monitor's: the monitor sits below `rb::runtime` and may not know the frame
  /// layout, so it cannot restore a frame itself. The runtime installs this one hook, called with the
  /// calling guest thread (which carries the per-thread delivery record).
  ///
  /// Unset (a bare monitor in a test, or a launch with no runtime), `rt_sigreturn` is a named refusal —
  /// never a 0, which would resume the restorer's own `svc` forever.
  using SignalReturnRestore = std::function<Outcome(runtime::GuestThread& thread)>;
  void set_signal_return(SignalReturnRestore restore) { sig_return_restore_ = std::move(restore); }
  /// Finds a live guest thread by tid, for the blocked-mask check in `QueueSignal`. Set once by the
  /// runtime; a bare monitor leaves it unset and interrupts every target.
  using ThreadLookup = std::function<runtime::GuestThread*(std::int32_t tid)>;
  void set_thread_lookup(ThreadLookup lookup) { thread_lookup_ = std::move(lookup); }
  [[nodiscard]] bool SignalReturnHooked() const { return static_cast<bool>(sig_return_restore_); }
  /// The restore itself. Only called from the `rt_sigreturn` case, with the calling thread.
  Outcome SignalReturn(runtime::GuestThread& thread) { return sig_return_restore_(thread); }

  /// Overrides the host-clock default, for a test that needs a value it can assert and for a launcher
  /// that wants the guest to see its own notion of time.
  void set_clock(ClockSource clock) { clock_ = std::move(clock); }

  /// Overrides the host-entropy default.
  void set_random(RandomSource random) { random_ = std::move(random); }

  /// `set_tid_address` state for a thread whose call the guest never made: bionic's linker calls
  /// `set_tid_address(&main_thread.tid)` before libc runs, so a process that plays the linker's part
  /// records the same pointer here. Read back by whatever will clear it and wake a joiner on exit.
  void set_clear_child_tid(std::uint32_t at) { main_->clear_child_tid = at; }
  [[nodiscard]] std::uint32_t clear_child_tid() const { return main_->clear_child_tid; }

  /// Anonymous mappings (`mmap2`, a moving or growing `mremap`) are placed only below this guest address,
  /// whatever the window holds above it: the top of a 4 GiB window carries the runtime's trap and the kuser
  /// helpers. 0 (the default) means "the window's end".
  void set_alloc_ceiling(std::uint32_t ceiling) { alloc_ceiling_ = ceiling; }

  /// Guest threads (M2 Task 4). With a futex table attached, `futex(WAIT)` really parks the caller (with the
  /// GGL released) until a `futex(WAKE)`, a thread exit's clear-child-tid wake, its timeout or `exit_group`;
  /// without one (a bare single-threaded monitor) a matching WAIT is answered 0 without blocking, as before,
  /// and the note says no other thread exists to wake it. Not owned.
  void set_futex_table(runtime::FutexTable* table) { futex_ = table; }
  [[nodiscard]] runtime::FutexTable* futex_table() const { return futex_; }
  /// What `clone` hands its new thread to. Unset, `clone` is refused by name (this monitor has no way to run
  /// a second thread).
  void set_thread_spawner(ThreadSpawner spawner) { spawner_ = std::move(spawner); }

  /// `exit_group` (from any thread), a fatal fault in a guest thread, or the process's teardown: every guest
  /// thread stops at its next slice boundary, syscall or host call (the bridge's run loop checks this) and
  /// every futex waiter and sleeper is woken. Sticky. `why` is kept for the report; the first one wins.
  void RequestGroupExit(const std::string& why);
  [[nodiscard]] bool group_exited() const { return group_exited_.load(std::memory_order_acquire); }
  /// Why the process stopped (empty while it has not). Read after `group_exited()` is true.
  [[nodiscard]] std::string group_exit_reason() const;
  /// Guest threads alive besides the main one: `clone`d threads that have not called `exit` yet, and adopted host
  /// threads (`AllocateThreadId`) that have not been retired.
  [[nodiscard]] std::size_t live_threads() const { return live_tids_.size(); }
  /// A fresh tid for a guest thread the host made (an adopted host thread, M2 Task 5), counted live until
  /// `ReleaseThreadId`. From the same sequence as `clone`'s tids. GGL held.
  std::int32_t AllocateThreadId();
  /// The thread `tid` is gone (an adopted thread's teardown). GGL held.
  void ReleaseThreadId(std::int32_t tid) { live_tids_.erase(tid); }

  /// Capture, dispatch, write the result back, for a single-threaded caller: a blocking call's wait runs in
  /// place (there is no lock to release), so the verdict returned is never `kBlocking`. `thread` is the
  /// calling guest thread (null = the main thread).
  Outcome Handle(exec::MachineState& state, std::uint32_t svc_imm, runtime::GuestThread* thread = nullptr);

  /// The bridge's entry (an executor's `Syscall` stop, with the GGL held). Like `Handle`, except that a
  /// blocking call comes back as `kBlocking` *without* a write-back: the caller runs `Outcome::block` with
  /// the GGL released, re-acquires it, and passes the result to `Finish`. Every other verdict is complete.
  Outcome Begin(exec::MachineState& state, std::uint32_t svc_imm, runtime::GuestThread* thread);
  /// Records and writes back the answer a blocking call's wait produced (GGL held again).
  Outcome Finish(exec::MachineState& state, Outcome done);

  /// Dispatch without the write-back, so a test can assert a verdict without a state to check. `thread`
  /// as for `Handle`.
  Outcome Classify(const Frame&, exec::MachineState& for_writeback, runtime::GuestThread* thread = nullptr);

  [[nodiscard]] const std::vector<std::pair<std::uint32_t, unsigned>>& tally() const { return tally_; }
  /// Distinct numbers seen, and how many times each — the line C17 records.
  [[nodiscard]] std::string TallyText() const;

  /// One line for each distinct call that did not simply succeed: refusals (`ENOSYS` and why), policy
  /// denials, and the serviced-with-an-errno answers (`ENOENT` from the provider, `EBADF` from the fd
  /// table). First-seen order, deduplicated, capped at `kMaxNotes` so a guest looping on one refusal
  /// cannot grow a report without bound. This is the half of the story the tally's counts cannot tell: a
  /// phone report that lists `openat x3` still cannot say whether those three were answered or refused.
  /// `Handle` records any outcome with a negative `r0`, not only the four original success-path sites.
  [[nodiscard]] const std::vector<std::string>& notes() const { return notes_; }

  /// How many distinct notes the cap had to drop, so a report can say the list is not the whole story
  /// rather than presenting a truncated list as complete.
  [[nodiscard]] std::size_t dropped_notes() const { return dropped_notes_; }

  /// How many answers this run refused (negative errno). The self-audit asserts the notes kept up with
  /// this: a refusal with no note is exactly the silent-ignore shape the note machinery exists to
  /// prevent.
  [[nodiscard]] std::size_t refusals() const { return refusals_; }

  /// What the guest wrote to fd 1 / fd 2, capped per stream at `kCaptureCap`. The app's own words are
  /// the only diagnostic a real process would show on a console, and dropping them was measured: the
  /// game's constructor wrote to fd 2 four times before `exit_group(127)` and every byte went nowhere
  /// (SM-X710, 2026-10-09). `capture_dropped` says the cap clipped something.
  [[nodiscard]] const std::string& captured_stdout() const { return captured_out_; }
  [[nodiscard]] const std::string& captured_stderr() const { return captured_err_; }
  [[nodiscard]] bool capture_dropped() const { return capture_dropped_; }
  /// How many bytes are kept per stream before the rest are counted rather than stored.
  static constexpr std::size_t kCaptureCap = 8192;

  /// How many distinct notes are kept before the rest are counted rather than stored.
  static constexpr std::size_t kMaxNotes = 24;

  /// The descriptor table's ceiling. A guest that opens more than this is refused with `EMFILE` naming
  /// the limit, rather than the table growing until the host's allocator says so. 256 is a number, not a
  /// fact about Android — but it is stated once, here, and the refusal prints it, so a guest that hits it
  /// is told which limit it hit.
  static constexpr std::size_t kMaxServedFds = 256;

  /// The requested-vs-active echo of the monitor's own policy (C17's `syscall-policy` field): what the
  /// guest asked for against what this build is allowed to do about it. An empty forwarding allowlist is
  /// recorded as the design it is, not as silence.
  void Echo(diag::Recorder& rec, const std::string& stage, const std::string& evidence) const;

  [[nodiscard]] static constexpr bool ForwardsAnything() { return false; }

 private:
  unsigned Note(std::uint32_t number);  ///< the count after this one, which callers rarely want
  /// Records `detail` unless it is already there, and counts what it had to drop.
  void Observe(const std::string& detail);

  /// One entry per guest-visible fd. The bytes are a copy, so `close` is a real close and nothing host
  /// side outlives the guest's own table.
  struct ServedFile {
    std::string path;
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    std::int32_t fd = 0;
  };
  /// Lowest number at or above 3 that is not open. A real kernel hands out the lowest free descriptor,
  /// and bionic's `fdsan`/`fdtrack` are built around that shape; starting at 1000 would be a value no
  /// real Android process ever sees.
  [[nodiscard]] std::int32_t NextFd() const;
  [[nodiscard]] ServedFile* FileFor(std::int32_t fd);

  /// Records a completed answer: the refusal count and note for a negative `r0`, then the write-back.
  Outcome Complete(exec::MachineState& state, Outcome o);

  Invalidator invalidator_;
  std::int32_t pid_ = 0;
  bool ids_set_ = false;
  /// Per-thread state (tid, clear-child-tid, robust list, signal mask, name) lives in `GuestThread`; this is
  /// the main thread's until a bridge attaches its own (`set_main_thread`).
  runtime::GuestThread own_main_;
  runtime::GuestThread* main_ = &own_main_;
  std::vector<std::pair<std::uint32_t, unsigned>> tally_;  ///< first-seen order, not sorted: a launch's
                                                           ///< story is the order the guest asked in

  mem::GuestMemory* memory_ = nullptr;
  FileProvider files_;
  ClockSource clock_;
  RandomSource random_;
  std::vector<ServedFile> served_;
  bool host_io_ = false;
  std::function<void(const std::string&)> host_io_trace_;
  std::function<void(const std::string&)> signal_trace_;
  /// The runtime's restore path (see `set_signal_return`): unset means a bare monitor, and `rt_sigreturn`
  /// is then refused by name.
  SignalReturnRestore sig_return_restore_;
  ThreadLookup thread_lookup_;
  /// Signal dispositions the guest installed, by number. `rt_sigaction` fills this in; delivery reads it.
  std::map<int, Disposition> sig_dispositions_;
  /// Pending signals by target tid (C26). Its own mutex: a sender (`tkill`) and the target's run loop
  /// (`TakePendingSignal`) are different threads, and neither holds the GGL when it touches this.
  mutable std::mutex sig_mu_;
  std::map<std::int32_t, std::vector<int>> pending_;
  std::atomic<std::size_t> signals_queued_{0};
  std::atomic<std::size_t> signals_delivered_{0};
  /// Whether this monitor answers `f` itself even with host I/O on: a descriptor it served, console output, or a
  /// path its provider serves.
  bool ServesItself(const Frame& f);
  // `rt_sigprocmask`'s mask (the kernel's arm32 sigset is 8 bytes, so a u64 is the whole of it; no signal is
  // ever delivered by this build), `set_tid_address`'s clear-child-tid pointer, `set_robust_list`'s head and
  // `prctl(PR_SET_NAME)`'s name are per-thread: they live in the calling `runtime::GuestThread`.
  /// Guest output for fd 1/2 — see `captured_stdout`. `CaptureOutput` answers the call as a console
  /// fd would (the full length reported consumed) and counts over-cap bytes rather than losing them
  /// silently.
  Outcome CaptureOutput(std::int32_t fd, std::uint32_t at, std::uint32_t len);
  std::string captured_out_;
  std::string captured_err_;
  bool capture_dropped_ = false;
  /// Where the next anonymous `mmap2` tries first, so consecutive allocations stay consecutive the
  /// way a real allocator expects its arenas to be.
  std::uint32_t mmap_next_ = 0;
  std::uint32_t alloc_ceiling_ = 0;  ///< see set_alloc_ceiling; 0 = the window's end
  /// Threads (Task 4). `live_tids_`/`next_tid_` are GGL-protected like every other table here.
  runtime::FutexTable* futex_ = nullptr;
  ThreadSpawner spawner_;
  std::set<std::int32_t> live_tids_;
  std::int32_t next_tid_ = 0;  ///< 0 = not started: the first clone takes main tid + 1
  std::atomic<bool> group_exited_{false};
  mutable std::mutex exit_mu_;  ///< leaf lock for `group_exit_reason_` (written by a thread that may not hold the GGL)
  std::string group_exit_reason_;
  /// "Now" on `clock_id` as the guest sees it (the installed clock, else the host's).
  bool Now(std::int32_t clock_id, std::int64_t* nanos) const;
  /// A host steady-clock deadline for a guest absolute time on `clock_id`.
  std::optional<runtime::FutexTable::Clock::time_point> DeadlineFor(std::int32_t clock_id, std::int64_t abs_nanos) const;
  /// The clone / exit / futex / sleep cases, split out of Classify.
  std::int32_t NextTid();
  Outcome Clone(const Frame& f, exec::MachineState& state, runtime::GuestThread& th);
  Outcome ThreadExit(const Frame& f, runtime::GuestThread& th);
  Outcome Futex(const Frame& f, runtime::GuestThread& th);
  Outcome Sleep(const std::string& what, std::int64_t nanos, runtime::GuestThread& th);
  /// The first page-aligned run of `size` free bytes for an anonymous mapping (`mmap2`, a moving
  /// `mremap`), or 0. A hint is honoured only when aligned and entirely free.
  std::uint32_t FindFreeRun(std::uint32_t hint, std::uint32_t size) const;
  /// min(window end, alloc ceiling), 64-bit: the first byte no anonymous mapping may reach.
  std::uint64_t AllocLimit() const;
  std::vector<std::string> notes_;
  std::size_t dropped_notes_ = 0;
  /// Counted at the same site that observes a refusal (`monitor.cpp` `Handle`), so the accessor and the
  /// notes cannot drift apart.
  std::size_t refusals_ = 0;
};

}  // namespace rb::syscall
