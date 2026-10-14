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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rb/diag/diag.h"
#include "rb/exec/machine_state.h"

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
constexpr std::uint32_t kMprotect = 125;
constexpr std::uint32_t kMremap = 163;
constexpr std::uint32_t kWritev = 146;
constexpr std::uint32_t kPrctl = 172;
constexpr std::uint32_t kRtSigaction = 174;
constexpr std::uint32_t kRtSigprocmask = 175;
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
};

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

/// The monitor. One per guest instance, like everything else that is per-instance: a refusal tally that
/// mixes apps would be a diagnostics lie, and no isolation property is shared across apps.
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
    tid_ = tid;
    ids_set_ = true;
  }

  /// The pointer-shaped calls need guest memory: to copy a path out, to copy a buffer in or out, to
  /// write a `timespec` or a `stat64`. Without one attached, every such call is a named refusal rather
  /// than a write to address 0 or a silently skipped copy.
  void set_memory(mem::GuestMemory* memory) { memory_ = memory; }

  /// Where a guest `openat` is answered from. Unset is a named refusal, not an empty file.
  void set_file_provider(FileProvider provider) { files_ = std::move(provider); }

  /// Overrides the host-clock default, for a test that needs a value it can assert and for a launcher
  /// that wants the guest to see its own notion of time.
  void set_clock(ClockSource clock) { clock_ = std::move(clock); }

  /// Overrides the host-entropy default.
  void set_random(RandomSource random) { random_ = std::move(random); }

  /// Capture, dispatch, write the result back. This is what an executor's `Syscall` stop hands to.
  Outcome Handle(exec::MachineState& state, std::uint32_t svc_imm);

  /// Dispatch without the write-back, so a test can assert a verdict without a state to check.
  Outcome Classify(const Frame&, exec::MachineState& for_writeback);

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

  Invalidator invalidator_;
  std::int32_t pid_ = 0;
  std::int32_t tid_ = 0;
  bool ids_set_ = false;
  std::vector<std::pair<std::uint32_t, unsigned>> tally_;  ///< first-seen order, not sorted: a launch's
                                                           ///< story is the order the guest asked in

  mem::GuestMemory* memory_ = nullptr;
  FileProvider files_;
  ClockSource clock_;
  RandomSource random_;
  std::vector<ServedFile> served_;
  /// `rt_sigprocmask`'s model: the kernel's arm32 sigset is 8 bytes, so this is the whole of it. Stored
  /// because a guest may read back what it blocked, and no signal is ever delivered by this build.
  std::uint64_t signal_mask_ = 0;
  /// `set_tid_address`'s clear-child-tid pointer, and `set_robust_list`'s head: recorded so the guest
  /// gets the identity and the acceptance a real kernel gives, and named in the notes as un-walked.
  std::uint32_t clear_child_tid_ = 0;
  std::uint32_t robust_head_ = 0;
  std::uint32_t robust_len_ = 0;
  std::array<char, 16> thread_name_{};  ///< `prctl(PR_SET_NAME)`, TASK_COMM_LEN bytes
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
  /// The first page-aligned run of `size` free bytes for an anonymous mapping (`mmap2`, a moving
  /// `mremap`), or 0. A hint is honoured only when aligned and entirely free.
  std::uint32_t FindFreeRun(std::uint32_t hint, std::uint32_t size) const;
  std::vector<std::string> notes_;
  std::size_t dropped_notes_ = 0;
  /// Counted at the same site that observes a refusal (`monitor.cpp` `Handle`), so the accessor and the
  /// notes cannot drift apart.
  std::size_t refusals_ = 0;
};

}  // namespace rb::syscall
