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

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "rb/diag/diag.h"
#include "rb/exec/machine_state.h"

namespace rb::syscall {

/// ARM EABI's private base. `__NR_SYSCALL_BASE` is 0 on ARM (measured), so the generic numbers are the
/// bare values and anything at or above this base is an ARM-specific call the host has no counterpart for
/// — which is why dispatch looks at the range as well as the value.
constexpr std::uint32_t kArmNrBase = 0x000f0000u;

/// `__NR_SYSCALL_MASK` from the same header: the kernel masks r7 with this before indexing the table, and
/// a monitor that did not would let a guest reach an entry with a number that has no meaning.
constexpr std::uint32_t kSyscallMask = 0x000fffffu;

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
constexpr std::uint32_t kMunmap = 91;
constexpr std::uint32_t kMprotect = 125;
constexpr std::uint32_t kPrctl = 172;
constexpr std::uint32_t kMmap2 = 192;
constexpr std::uint32_t kGettid = 224;
constexpr std::uint32_t kFutex = 240;
constexpr std::uint32_t kExitGroup = 248;
constexpr std::uint32_t kClockGettime = 263;
constexpr std::uint32_t kOpenat = 322;
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
constexpr std::uint32_t kEinval = 22;
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
  std::uint32_t number = 0;  ///< r7, masked by the kernel's rule
  std::uint32_t imm = 0;     ///< the SVC immediate — a channel, not the number
  std::uint32_t arg[6] = {};  ///< r0..r5
  exec::GuestAddr pc = 0;    ///< already past the SVC, in both tiers (see `StopReason::Syscall`)

  /// r7 is what the kernel dispatches on; the masking is the kernel's own rule and is applied here so a
  /// guest cannot reach past the table by setting a high bit.
  [[nodiscard]] static Frame Capture(const exec::MachineState& state, std::uint32_t svc_imm);

  /// Puts the result back the way the kernel does: r0 only, CPSR untouched — see the measurement in the
  /// header. Returns the value written, for a caller that wants to log it without re-deriving it.
  std::uint32_t ApplyReturn(exec::MachineState& state, const Outcome& outcome) const;
};

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

  /// Capture, dispatch, write the result back. This is what an executor's `Syscall` stop hands to.
  Outcome Handle(exec::MachineState& state, std::uint32_t svc_imm);

  /// Dispatch without the write-back, so a test can assert a verdict without a state to check.
  Outcome Classify(const Frame&, exec::MachineState& for_writeback);

  [[nodiscard]] const std::vector<std::pair<std::uint32_t, unsigned>>& tally() const { return tally_; }
  /// Distinct numbers seen, and how many times each — the line C17 records.
  [[nodiscard]] std::string TallyText() const;

  /// The requested-vs-active echo of the monitor's own policy (C17's `syscall-policy` field): what the
  /// guest asked for against what this build is allowed to do about it. An empty forwarding allowlist is
  /// recorded as the design it is, not as silence.
  void Echo(diag::Recorder& rec, const std::string& stage, const std::string& evidence) const;

  [[nodiscard]] static constexpr bool ForwardsAnything() { return false; }

 private:
  unsigned Note(std::uint32_t number);  ///< the count after this one, which callers rarely want

  Invalidator invalidator_;
  std::int32_t pid_ = 0;
  std::int32_t tid_ = 0;
  bool ids_set_ = false;
  std::vector<std::pair<std::uint32_t, unsigned>> tally_;  ///< first-seen order, not sorted: a launch's
                                                           ///< story is the order the guest asked in
};

}  // namespace rb::syscall
