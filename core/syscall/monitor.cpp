/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/syscall/monitor.h"

#include <cstdio>

namespace rb::syscall {
namespace {

std::string Hex(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

const char* Name(Verdict v) {
  switch (v) {
    case Verdict::kEmulated: return "emulated";
    case Verdict::kDenied: return "denied";
    case Verdict::kUnsupported: return "unsupported";
    case Verdict::kExit: return "exit";
    case Verdict::kForward: return "forward";
  }
  return "unknown";
}

const char* Name(std::uint32_t number) {
  switch (number) {
    case nr::kRead: return "read";
    case nr::kWrite: return "write";
    case nr::kClose: return "close";
    case nr::kGetpid: return "getpid";
    case nr::kMunmap: return "munmap";
    case nr::kMprotect: return "mprotect";
    case nr::kPrctl: return "prctl";
    case nr::kMmap2: return "mmap2";
    case nr::kGettid: return "gettid";
    case nr::kFutex: return "futex";
    case nr::kExitGroup: return "exit_group";
    case nr::kClockGettime: return "clock_gettime";
    case nr::kOpenat: return "openat";
    case nr::kExecve: return "execve";
    case nr::kCacheFlush: return "arm_cacheflush";
    case nr::kSetTls: return "arm_set_tls";
    case nr::kGetTls: return "arm_get_tls";
    default: return nullptr;  ///< not a gap: the caller prints the number when there is no name
  }
}

std::string Describe(std::uint32_t number) {
  const char* n = Name(number);
  return n != nullptr ? std::string{n} : "nr:0x" + Hex(number);
}

Frame Frame::Capture(const exec::MachineState& state, std::uint32_t svc_imm) {
  Frame f{};
  // The kernel's own masking rule, applied here so a high bit cannot walk the table out of range.
  f.number = state.r[7] & kSyscallMask;
  f.imm = svc_imm;
  for (int i = 0; i < 6; ++i) f.arg[i] = state.r[i];
  f.pc = state.pc;
  return f;
}

std::uint32_t Frame::ApplyReturn(exec::MachineState& state, const Outcome& outcome) const {
  const std::uint32_t value = static_cast<std::uint32_t>(outcome.r0);
  state.r[0] = value;
  // Deliberately nothing else. CPSR is not touched, because on the measured kernel the carry bit is set
  // after a successful `sched_yield` returning 0 just as it is after a failure — the flag is not the
  // signal, and clearing it here would make RetroBug's return path disagree with every real 32-bit
  // Android process for no benefit. Nor is `r15`: both tiers keep it equal to `pc`, and the resume
  // address is the one the SVC already advanced past.
  return value;
}

unsigned Monitor::Note(std::uint32_t number) {
  for (auto& [n, count] : tally_) {
    if (n == number) return ++count;
  }
  tally_.emplace_back(number, 1u);
  return 1;
}

std::string Monitor::TallyText() const {
  std::string out;
  for (const auto& [n, count] : tally_) {
    if (!out.empty()) out += ", ";
    out += Describe(n);
    out += " x";
    out += std::to_string(count);
  }
  return out.empty() ? std::string{"(nothing yet)"} : out;
}

Outcome Monitor::Classify(const Frame& f, exec::MachineState& state) {
  switch (f.number) {
    case nr::kGetpid:
    case nr::kGettid: {
      if (!ids_set_) {
        // Refused rather than answered with a plausible 0 or a host `getpid()`: an identity the guest
        // uses for `kill`, for `/proc/self`, or for a lock file is not a value to invent, and asking the
        // host kernel for it is precisely what the empty allowlist forbids.
        return Outcome::Refused(err::kEnosys,
                                "getpid/gettid before the launcher set this instance's ids");
      }
      const std::int32_t id = f.number == nr::kGetpid ? pid_ : tid_;
      return Outcome::Emulated(id, "per-instance id supplied by the launcher");
    }

    case nr::kExitGroup:
      return Outcome{Verdict::kExit, static_cast<std::int32_t>(f.arg[0]),
                     "exit_group(" + Hex(f.arg[0]) + ")"};

    case nr::kCacheFlush: {
      if (!invalidator_) {
        return Outcome::Refused(err::kEnosys, "arm_cacheflush with no invalidator installed");
      }
      // AD-7's self-modifying-code path: the guest's own statement that executable bytes changed. An
      // empty range is the guest asking for nothing, and the tier's Invalidate is a no-op for it.
      invalidator_(f.arg[0], f.arg[1]);
      return Outcome::Emulated(0, "invalidated [" + Hex(f.arg[0]) + ", " + Hex(f.arg[1]) + ")");
    }

    case nr::kGetTls:
      // The thread pointer is a MachineState field, so this channel works today even though the `mrc`
      // read of `tpidrurw` does not (no Coprocessor hook yet, `30` §Step 9). Both paths have to exist:
      // guests from different toolchain generations use one or the other.
      return Outcome::Emulated(static_cast<std::int32_t>(state.tp), "MachineState::tp");

    case nr::kSetTls:
      state.tp = f.arg[0];
      return Outcome::Emulated(0, "MachineState::tp = " + Hex(f.arg[0]));

    case nr::kRead:
    case nr::kWrite:
    case nr::kClose:
    case nr::kOpenat:
    case nr::kMmap2:
    case nr::kMunmap:
    case nr::kMprotect:
    case nr::kFutex:
    case nr::kClockGettime:
    case nr::kPrctl:
      // Known, and refused *as a decision* rather than as an accident: each of these needs a component
      // that does not exist yet (C10's resolver for the file calls, C07's own thread work for futex, and
      // so on). ENOSYS is what a kernel with no entry says, and the measured device kernel says exactly
      // that for a number outside its table, so the guest at least sees a consistent shape.
      return Outcome::Refused(err::kEnosys, Describe(f.number) +
                                               " is not forwarded: this build has an empty allowlist and "
                                               "no handler yet");

    case nr::kExecve:
      // The one refusal with a *measured* errno rather than a default. A real 32-bit app process asking
      // for its own file gets EACCES from SELinux, so that is what a guest gets here — and the reason it
      // is spelled out rather than left to the default is that `ENOSYS` and `EACCES` look equally
      // plausible in a log while telling an app's developer two different stories: "this emulator cannot
      // do it" versus "Android will not let you do it, anywhere".
      return Outcome::Denied(err::kEacces,
                             "execve: measured EACCES (13) from u:r:untrusted_app on SM-X710, SELinux "
                             "app_data_file execute_no_trans; see 30 §Step 12");

    default:
      return Outcome::Refused(err::kEnosys, "no handler for " + Describe(f.number) +
                                                "; measured on SM-X710: an out-of-table r7 answers "
                                                "-ENOSYS (-38)");
  }
}

Outcome Monitor::Handle(exec::MachineState& state, std::uint32_t svc_imm) {
  const Frame f = Frame::Capture(state, svc_imm);
  Note(f.number);
  Outcome o = Classify(f, state);
  f.ApplyReturn(state, o);
  return o;
}

void Monitor::Echo(diag::Recorder& rec, const std::string& stage,
                   const std::string& evidence) const {
  diag::Entry e{};
  e.field = diag::Field::kSyscallPolicy;
  e.stage = stage;
  e.requested = ForwardsAnything() ? std::string{"a non-empty forwarding allowlist"}
                                   : std::string{"no forwarding: nothing reaches the host kernel"};
  e.active = "allowlist empty, so every answer came from RetroBug. Guest asked for: " + TallyText();
  // `inert` rather than `active` for the forwarding case, because a declared row that was not honoured is
  // exactly the shape C17 exists to surface. Today nothing is declared, so `active` is the truth.
  e.outcome = ForwardsAnything() ? diag::Outcome::kInert : diag::Outcome::kActive;
  e.evidence = evidence;
  rec.Record(e);
}

}  // namespace rb::syscall
