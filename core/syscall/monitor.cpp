/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/syscall/monitor.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <random>

#include "rb/mem/guest_memory.h"

namespace rb::syscall {
namespace {

std::string Hex(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

/// A protection mask the way `/proc/self/maps` spells it, always printed *next to* the raw value rather
/// than instead of it: this build names three bits, and a guest may pass a fourth (arm32's `PROT_SEM`,
/// the `PROT_GROWSDOWN`/`UP` pair), which a decoded-only report would quietly drop.
std::string ProtText(std::uint32_t prot) {
  std::string out{"---"};
  if ((prot & mem::kRead) != 0) out[0] = 'r';
  if ((prot & mem::kWrite) != 0) out[1] = 'w';
  if ((prot & mem::kExec) != 0) out[2] = 'x';
  return out;
}

/// The host's own clocks, and the only place in this file that asks the host anything. `CLOCK_REALTIME`
/// and `CLOCK_MONOTONIC` are facts a process can have; the process-CPU clocks are not answered here
/// because a guest's "CPU time" measured by the emulator's host would be a different quantity wearing the
/// right name, and a named refusal is better than a plausible wrong number.
bool HostClock(std::int32_t clock_id, std::int64_t* nanos) {
  const clockid_t id = clock_id == static_cast<std::int32_t>(CLOCK_REALTIME)
                           ? CLOCK_REALTIME
                           : (clock_id == static_cast<std::int32_t>(CLOCK_MONOTONIC) ? CLOCK_MONOTONIC
                                                                                    : -1);
  if (id < 0) return false;
  timespec ts{};
  if (::clock_gettime(id, &ts) != 0) return false;
  *nanos = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(ts.tv_nsec);
  return true;
}

/// The host's entropy pool, through `<random>` because `getentropy`/`getrandom` are API 28 and this
/// engine builds at 26. libc++'s `random_device` is `/dev/urandom` on Android; the `try` is here because
/// it is allowed to throw on a host with no entropy source, and a syscall handler that threw would take
/// the instance down instead of naming a refusal.
bool HostEntropy(std::uint8_t* dst, std::size_t len) {
  try {
    std::random_device device;
    for (std::size_t i = 0; i < len;) {
      const std::uint32_t word = device();
      for (unsigned b = 0; b < 4 && i < len; ++b, ++i) {
        dst[i] = static_cast<std::uint8_t>((word >> (8 * b)) & 0xffu);
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

/// `AT_FDCWD`, measured: the wall run's `openat` had `r0 = 0xffffff9c`.
constexpr std::int32_t kAtFdcwd = -100;
/// The longest pathname this file will copy before answering `ENAMETOOLONG`, which is `PATH_MAX` and the
/// length a real `openat` of an over-long name was measured to refuse with -36 on the SM-X710.
constexpr std::size_t kMaxPath = 4096;
/// Linux fcntl bits this file has to read, from `asm-generic/fcntl.h`: O_ACCMODE (write intent),
/// O_CREAT, O_TRUNC, O_APPEND, O_DIRECTORY. A provider serves bytes, so a write-intent open is `EROFS`
/// and a directory open is `ENOTDIR`, both of which are what a real Android gives for those shapes.
constexpr std::uint32_t kOAccMode = 3u;
constexpr std::uint32_t kOCreat = 00000100u;
constexpr std::uint32_t kOTrunc = 00001000u;
constexpr std::uint32_t kOAppend = 00002000u;
constexpr std::uint32_t kODirectory = 00200000u;

/// The arm32 `struct stat64`, whose field offsets were *measured* rather than recalled: the NDK's own
/// `arm-linux-androideabi/asm/stat.h` compiled for `armv7a-linux-androideabi26` puts `st_size` at 48 and
/// the two inode words at 12 and 96, sizeof == 104 — the four bytes of alignment after `__pad3` are the
/// part a hand-written layout gets wrong. See the test lane, which asserts the offsets it reads back.
constexpr std::uint32_t kStatSize = 16;    ///< st_mode
constexpr std::uint32_t kStatNlink = 20;   ///< st_nlink
constexpr std::uint32_t kStatSizeOff = 48; ///< st_size, the 8-byte one
constexpr std::uint32_t kStatBlksize = 56;
constexpr std::uint32_t kStatBlocks = 64;
constexpr std::uint32_t kStatIno = 96;
constexpr std::uint32_t kStatBytes = 104;
/// S_IFREG | 0444: what the provider's images are, and the reason `write` on one is `EBADF` rather than a
/// silently discarded buffer.
constexpr std::uint32_t kModeReadonly = 0100444u;

/// Fills a guest `struct stat64` for a read-only regular file of `size` bytes. Only the fields a caller
/// can observe from a read-only image are written: everything else is left zero rather than filled with
/// a host value (`st_dev`, times) that would be a fact about the wrong machine.
bool WriteStat64(mem::GuestMemory& mem, std::uint32_t at, std::uint64_t size, std::string* why) {
  std::uint8_t bytes[kStatBytes] = {};
  const auto put32 = [&](std::uint32_t off, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) bytes[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu);
  };
  const auto put64 = [&](std::uint32_t off, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) bytes[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu);
  };
  put32(kStatSize, kModeReadonly);
  put32(kStatNlink, 1u);
  put64(kStatSizeOff, size);
  put32(kStatBlksize, mem::kGuestPage);
  put64(kStatBlocks, (size + 511u) / 512u);
  put32(12, 1u);  ///< the old `__st_ino` word, which bionic's struct still carries
  put64(kStatIno, 1u);
  if (!mem.Write(at, bytes, sizeof bytes)) {
    if (why != nullptr) *why = "the stat buffer at 0x" + Hex(at) + " is not writable guest memory";
    return false;
  }
  return true;
}

enum class PathStatus { kOk, kUnmapped, kTooLong };

/// Copies a NUL-terminated guest path out, one byte at a time so a path that ends at the last byte of a
/// mapped page still works. `kTooLong` is a distinct answer from `kUnmapped` because the two have
/// different errnos — `ENAMETOOLONG` is measured (`-36`, SM-X710), and `EFAULT` is what a real kernel
/// says for a pointer that is not there.
PathStatus ReadGuestPath(mem::GuestMemory& mem, std::uint32_t at, std::string* out) {
  out->clear();
  for (std::size_t i = 0; i < kMaxPath; ++i) {
    // The uint32 add wraps: a path pointer near the top of the window must refuse, not wrap round to
    // address 0 and read a plausible-looking path from there.
    const std::uint32_t where = at + static_cast<std::uint32_t>(i);
    if (static_cast<std::uint64_t>(at) + i > 0xffff'ffffull) return PathStatus::kUnmapped;
    char c = 0;
    if (!mem.Read(where, &c, 1)) return PathStatus::kUnmapped;
    if (c == '\0') return PathStatus::kOk;
    out->push_back(c);
  }
  return PathStatus::kTooLong;
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
    case nr::kBrk: return "brk";
    case nr::kIoctl: return "ioctl";
    case nr::kGettimeofday: return "gettimeofday";
    case nr::kMunmap: return "munmap";
    case nr::kFstat: return "fstat";
    case nr::kMprotect: return "mprotect";
    case nr::kWritev: return "writev";
    case nr::kPrctl: return "prctl";
    case nr::kRtSigaction: return "rt_sigaction";
    case nr::kRtSigprocmask: return "rt_sigprocmask";
    case nr::kMmap2: return "mmap2";
    case nr::kFstat64: return "fstat64";
    case nr::kGetdents64: return "getdents64";
    case nr::kMadvise: return "madvise";
    case nr::kFcntl64: return "fcntl64";
    case nr::kGettid: return "gettid";
    case nr::kFutex: return "futex";
    case nr::kExitGroup: return "exit_group";
    case nr::kSetTidAddress: return "set_tid_address";
    case nr::kClockGettime: return "clock_gettime";
    case nr::kClockNanosleep: return "clock_nanosleep";
    case nr::kTgkill: return "tgkill";
    case nr::kOpenat: return "openat";
    case nr::kFstatat64: return "fstatat64";
    case nr::kReadlinkat: return "readlinkat";
    case nr::kFaccessat: return "faccessat";
    case nr::kSetRobustList: return "set_robust_list";
    case nr::kUtimensat: return "utimensat";
    case nr::kGetrandom: return "getrandom";
    case nr::kClockGettime64: return "clock_gettime64";
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
  // r7 is the syscall number, full stop. The kernel bounds-checks it against its table and answers
  // -ENOSYS for a number past the end; masking it with `__NR_SYSCALL_MASK` — a bounds-check constant,
  // not a mask — let a hand-written stub with a garbage high bit (r7 = 0x00100003) alias onto a real
  // entry and *execute* it where the kernel would have refused. Numbers past the ARM private range
  // are refused by the dispatch default below, which is the honest answer.
  f.number = state.r[7];
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

Outcome Monitor::CaptureOutput(std::int32_t fd, std::uint32_t at, std::uint32_t len) {
  if (memory_ == nullptr) {
    return Outcome::Refused(err::kEnosys, "write to fd " + std::to_string(fd) +
                                              ": no guest memory is attached to this monitor, so the "
                                              "bytes cannot be read");
  }
  std::string& sink = fd == 2 ? captured_err_ : captured_out_;
  // Chunked, not one capped read: a single write larger than the per-read window used to store its
  // first 4 KiB and silently lose the rest — the cap notice was keyed on a per-call `want` that never
  // exceeded the window, so it could not fire (caught in review). Bytes past `kCaptureCap`, or a call
  // past the 64 KiB sanity ceiling, are counted in `capture_dropped_` rather than lost wordlessly.
  const std::size_t hard = std::min<std::size_t>(len, 65536u);
  std::size_t consumed = 0;
  std::string shown;
  while (consumed < hard) {
    const std::size_t want = std::min<std::size_t>(4096u, hard - consumed);
    std::string buf;
    buf.resize(want);
    if (!memory_->Read(at + consumed, buf.data(), want)) {
      return Outcome::Answered(err::kEfault, std::string(fd == 2 ? "write(fd=2)" : "write(fd=1)") +
                                                 ": the buffer at 0x" + Hex(at + consumed) +
                                                 " is not readable guest memory");
    }
    const std::size_t room = sink.size() < kCaptureCap ? kCaptureCap - sink.size() : 0;
    const std::size_t kept = std::min(want, room);
    sink.append(buf.data(), kept);
    if (kept < want) capture_dropped_ = true;
    for (std::size_t i = 0; i < kept && shown.size() < 120; ++i) {
      const unsigned char c = static_cast<unsigned char>(buf[i]);
      shown.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
    }
    consumed += want;
  }
  if (len > hard) capture_dropped_ = true;
  // The full length is reported as written, which is what a console fd does; anything past the caps
  // is counted in `capture_dropped`, not silently lost.
  return Outcome::Emulated(static_cast<std::int32_t>(len),
                           (fd == 2 ? "write(fd=2, " : "write(fd=1, ") + std::to_string(len) +
                               ") -> " + std::to_string(len) + ": \"" + shown + "\"" +
                               (capture_dropped_ ? " (capture capped)" : ""));
}

void Monitor::Observe(const std::string& detail) {

  for (const std::string& seen : notes_) {
    if (seen == detail) return;
  }
  if (notes_.size() >= kMaxNotes) {
    ++dropped_notes_;
    return;
  }
  notes_.push_back(detail);
}

std::int32_t Monitor::NextFd() const {
  for (std::int32_t candidate = 3;; ++candidate) {
    bool used = false;
    for (const ServedFile& open : served_) {
      if (open.fd == candidate) {
        used = true;
        break;
      }
    }
    if (!used) return candidate;
  }
}

Monitor::ServedFile* Monitor::FileFor(std::int32_t fd) {
  for (ServedFile& open : served_) {
    if (open.fd == fd) return &open;
  }
  return nullptr;
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

    case nr::kMprotect: {
      // The call a real libc's own startup cannot get past: bionic's
      // `WriteProtected<libc_globals>::initialize` strips write from its globals page before it has a
      // channel to report a failure through, so a refusal here becomes `async_safe_fatal` and a spin.
      // Granting is the answer this build can give, and what the grant is *not* is part of the answer.
      const std::uint32_t at = f.arg[0];
      const std::uint32_t len = f.arg[1];
      const std::uint32_t prot = f.arg[2];
      const std::string asked = "prot=0x" + Hex(prot) + " (" + ProtText(prot) + ")";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys,
                                "mprotect(0x" + Hex(at) + ", " + std::to_string(len) + ", " + asked +
                                    "): no guest memory is attached to this monitor, so there is no "
                                    "window to check the range against");
      }
      const std::uint64_t end = static_cast<std::uint64_t>(at) + len;
      const std::string range = "[0x" + Hex(at) + ", 0x" + Hex(end) + ")";
      // ENOMEM rather than the EFAULT the copying calls above answer: a kernel's mprotect never reads or
      // writes the range, it walks the VMAs covering it, and an address no VMA covers comes back ENOMEM
      // — which is also the errno a guest expects from a range with a hole in it.
      if (end > memory_->window_bytes()) {
        return Outcome::Answered(err::kEnomem,
                                 "mprotect(" + range + ", " + asked + ") = -ENOMEM (-12): the range is "
                                 "not inside this instance's guest window of 0x" +
                                 Hex(memory_->window_bytes()) + " bytes");
      }
      // Guest pages, because that is the granule a guest asked about; the model is the only thing that
      // knows which of them have a mapping, and `IsMapped` refuses an address outside the window too.
      for (std::uint64_t page = at & ~static_cast<std::uint64_t>(mem::kGuestPage - 1); page < end;
           page += mem::kGuestPage) {
        if (!memory_->IsMapped(static_cast<std::uint32_t>(page))) {
          return Outcome::Answered(err::kEnomem,
                                   "mprotect(" + range + ", " + asked + ") = -ENOMEM (-12): 0x" +
                                       Hex(page) + " in the range has no mapping");
        }
      }
      // The deliberate deviation, and it is worded as one: granting the value a device grants is what
      // lets a guest proceed, but nothing here applied the request to anything, so a report that read
      // this as "the range is read-only now" would be describing an emulator that does not exist.
      const std::string why = "mprotect(" + range + ", " + asked +
                              ") = 0: recorded as granted and not enforced — this monitor does not apply "
                              "the requested protection to the guest's pages, so the range keeps the "
                              "access it already had instead of becoming read-only the way a device "
                              "would make it";
      Observe(why);
      return Outcome::Emulated(0, why);
    }

    case nr::kCacheFlush: {
      if (!invalidator_) {
        return Outcome::Refused(err::kEnosys, "arm_cacheflush with no invalidator installed");
      }
      // AD-7's self-modifying-code path: the guest's own statement that executable bytes changed. An
      // empty range is the guest asking for nothing, and the tier's Invalidate is a no-op for it. A
      // reversed range is EINVAL on the measured kernel — a guest relying on the flush's success to
      // have invalidated garbage gets a 0 here no device would give.
      if (f.arg[1] < f.arg[0]) {
        return Outcome::Answered(err::kEinval,
                                 "arm_cacheflush(start=0x" + Hex(f.arg[0]) + ", end=0x" +
                                     Hex(f.arg[1]) + "): the end precedes the start");
      }
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

    case nr::kSetTidAddress: {
      if (!ids_set_) {
        return Outcome::Refused(err::kEnosys,
                                "svc #256 (set_tid_address) before the launcher set this instance's ids; "
                                "refusing rather than returning a tid nobody produced");
      }
      clear_child_tid_ = f.arg[0];
      const std::string why = "set_tid_address(0x" + Hex(clear_child_tid_) + ") -> tid " +
                              std::to_string(tid_) +
                              "; the clear-child-tid slot is recorded, and no thread exit walks it "
                              "because this build models one thread";
      Observe(why);
      return Outcome::Emulated(tid_, why);
    }

    case nr::kSetRobustList: {
      // The kernel requires len == sizeof(struct robust_list_head) = 24 and EINVALs anything else;
      // the first draft answered 0 for any len, hiding a caller that passed the wrong object.
      if (f.arg[1] != 24u) {
        return Outcome::Answered(err::kEinval, "set_robust_list(len=" + std::to_string(f.arg[1]) +
                                                   ") = -EINVAL (-22): the kernel requires exactly "
                                                   "sizeof(struct robust_list_head) = 24");
      }
      robust_head_ = f.arg[0];
      robust_len_ = f.arg[1];
      const std::string why = "set_robust_list(head=0x" + Hex(robust_head_) + ", len=" +
                              std::to_string(robust_len_) +
                              ") recorded -> 0; this build models one thread and no thread exit, so "
                              "nothing ever walks the list";
      Observe(why);
      return Outcome::Emulated(0, why);
    }

    case nr::kClockGettime:
    case nr::kClockGettime64: {
      const bool wide = f.number == nr::kClockGettime64;
      const std::string call = wide ? "clock_gettime64" : "clock_gettime";
      const std::int32_t id = static_cast<std::int32_t>(f.arg[0]);
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + "(" + std::to_string(id) +
                                                   "): no guest memory is attached to this monitor, so "
                                                   "there is nowhere to write the timespec");
      }
      std::int64_t nanos = 0;
      const bool have = clock_ ? clock_(id, &nanos) : HostClock(id, &nanos);
      if (!have) {
        return Outcome::Refused(err::kEnosys,
                                call + "(" + std::to_string(id) +
                                    ") has no fact to answer from: this build answers CLOCK_REALTIME "
                                    "(0) and CLOCK_MONOTONIC (1) only, and a CPU-time clock measured on "
                                    "the host would be the emulator's time under the guest's name");
      }
      std::int64_t sec = nanos / 1'000'000'000;
      std::int64_t nsec = nanos % 1'000'000'000;
      if (nsec < 0) {
        nsec += 1'000'000'000;
        --sec;
      }
      if (!wide && (sec > 0x7fff'ffffll || sec < -0x8000'0000ll)) {
        // The 32-bit kernel answers EOVERFLOW when a time doesn't fit its timespec, not a silently
        // truncated second count. (Inert until 2038, but it is the same class of plausible wrong
        // number the monitor exists not to give.)
        return Outcome::Answered(err::kEoverflow, call + "(" + std::to_string(id) +
                                                      "): " + std::to_string(sec) +
                                                      "s does not fit a 32-bit timespec");
      }
      const std::uint32_t at = f.arg[1];
      bool wrote = false;
      if (wide) {
        // `struct __kernel_timespec` for the 64-bit `time_t` variant: two 8-byte fields, 16 bytes.
        const std::uint64_t words[2] = {static_cast<std::uint64_t>(sec),
                                        static_cast<std::uint64_t>(nsec)};
        wrote = memory_->Write(at, words, sizeof words);
      } else {
        // The legacy arm32 `struct timespec`: two 4-byte fields, 8 bytes.
        const std::uint32_t words[2] = {static_cast<std::uint32_t>(sec),
                                        static_cast<std::uint32_t>(nsec)};
        wrote = memory_->Write(at, words, sizeof words);
      }
      if (!wrote) {
        return Outcome::Answered(err::kEfault, call + ": the timespec at 0x" + Hex(at) +
                                                   " is not writable guest memory");
      }
      return Outcome::Emulated(0, call + "(" + std::to_string(id) + ") = " + std::to_string(sec) + "s " +
                                      std::to_string(nsec) + "ns");
    }

    case nr::kGetrandom: {
      const std::uint32_t at = f.arg[0];
      const std::uint32_t len = f.arg[1];
      const std::uint32_t flags = f.arg[2];
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "getrandom: no guest memory is attached to this monitor");
      }
      if (flags & ~3u) {
        // GRND_VALIDFLAGS: the kernel rejects unknown bits, not all flags. GRND_NONBLOCK (1) and
        // GRND_RANDOM (2) are valid and succeed on a device; the first draft's "the kernel requires
        // flags == 0" was factually wrong and EINVAL'd a call a real guest passes.
        return Outcome::Answered(err::kEinval, "getrandom(flags=0x" + Hex(flags) +
                                                   "): unknown GRND_ flags (valid mask is 0x3)");
      }
      if (len == 0) return Outcome::Emulated(0, "getrandom(0) -> 0");
      /// Filled in bounded chunks straight into guest memory: the guest picks `len`, so a `std::vector`
      /// of its choosing is a host allocation a guest controls, and there is no reason to make one.
      std::uint8_t chunk[256];
      std::uint32_t done = 0;
      while (done < len) {
        const std::uint32_t n = std::min<std::uint32_t>(sizeof chunk, len - done);
        if (!(random_ ? random_(chunk, n) : HostEntropy(chunk, n))) {
          return Outcome::Refused(err::kEnosys, "svc #384 (getrandom) is not implemented: no random "
                                               "source is installed and the host's own pool refused");
        }
        // `at + done` wraps mod 2^32 like any uint32 add; a buffer near the top of the window would
        // write its second chunk at address 0, where a real kernel faults at the task-size boundary.
        // Checked before the write, not after.
        const std::uint64_t chunk_end = static_cast<std::uint64_t>(at) + done + n;
        if (chunk_end > 0x1'0000'0000ull) {
          return Outcome::Answered(err::kEfault, "getrandom: the buffer at 0x" + Hex(at) +
                                                     " wraps past the end of guest memory");
        }
        if (!memory_->Write(at + done, chunk, n)) {
          return Outcome::Answered(err::kEfault, "getrandom: the buffer at 0x" + Hex(at) +
                                                     " is not writable guest memory");
        }
        done += n;
      }
      return Outcome::Emulated(static_cast<std::int32_t>(len),
                               "getrandom(" + std::to_string(len) + ") from the host's entropy pool");
    }

    case nr::kRtSigprocmask: {
      const std::uint32_t how = f.arg[0];
      const std::uint32_t set = f.arg[1];
      const std::uint32_t old = f.arg[2];
      const std::uint32_t sigsetsize = f.arg[3];
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "rt_sigprocmask: no guest memory is attached to this monitor");
      }
      // The arm32 kernel's `compat_sigset_t` is 8 bytes and it refuses anything else with EINVAL before
      // it looks at the mask — so a size of 4 (bionic's *userspace* `sigset_t`) is a real EINVAL, not a
      // gap, and answering it would hide a caller that is passing the wrong object.
      if (sigsetsize != 8) {
        return Outcome::Answered(err::kEinval,
                                 "rt_sigprocmask(sigsetsize=" + std::to_string(sigsetsize) +
                                     ") = -EINVAL (-22): the arm32 kernel's compat sigset is 8 bytes");
      }
      if (how > 2) {
        return Outcome::Answered(err::kEinval, "rt_sigprocmask(how=" + std::to_string(how) +
                                                   ") = -EINVAL: not SIG_BLOCK(0)/SIG_UNBLOCK(1)/"
                                                   "SIG_SETMASK(2)");
      }
      // The new set is read *before* the old set is written: the kernel validates and copies in first,
      // so a stale `set` pointer leaves the caller's old-set out-parameter intact. The first draft
      // wrote `old` first, and an EFAULT on `set` clobbered guest memory a device would not have.
      std::uint64_t incoming = 0;
      if (set != 0 && !memory_->Read(set, &incoming, sizeof incoming)) {
        return Outcome::Answered(err::kEfault, "rt_sigprocmask: the set at 0x" + Hex(set) +
                                                   " is not readable guest memory");
      }
      if (old != 0 && !memory_->Write(old, &signal_mask_, sizeof signal_mask_)) {
        return Outcome::Answered(err::kEfault, "rt_sigprocmask: the old-set out-parameter at 0x" +
                                                   Hex(old) + " is not writable guest memory");
      }
      if (set != 0) {
        if (how == 0) {
          signal_mask_ |= incoming;
        } else if (how == 1) {
          signal_mask_ &= ~incoming;
        } else {
          signal_mask_ = incoming;
        }
      }
      return Outcome::Emulated(0, "rt_sigprocmask(how=" + std::to_string(how) + ") -> mask 0x" +
                                      Hex(signal_mask_) +
                                      " (modelled in the monitor; this build delivers no signals)");
    }

    case nr::kPrctl: {
      const std::uint32_t option = f.arg[0];
      // PR_SET_NAME (15) / PR_GET_NAME (16): the thread name is guest-visible state that RetroBug can
      // hold without touching the host thread, so these two are answered and everything else is refused
      // by name rather than answered with a 0 the way a kernel would answer an option it *does* know.
      if (option == 15u || option == 16u) {
        if (memory_ == nullptr) {
          return Outcome::Refused(err::kEnosys, "prctl(PR_SET_NAME/PR_GET_NAME): no guest memory is "
                                               "attached to this monitor");
        }
        if (option == 15u) {
          // TASK_COMM_LEN bytes, NUL-terminated: read up to the terminator the way `strncpy_from_user`
          // does, so a name whose last byte is the last byte of a page still reads. The per-byte
          // address is checked against the 32-bit ceiling before the read, like every other
          // guest-pointer walk in this file.
          std::array<char, 16> name{};
          for (std::size_t i = 0; i + 1 < name.size(); ++i) {
            if (static_cast<std::uint64_t>(f.arg[1]) + i > 0xffff'ffffull ||
                !memory_->Read(f.arg[1] + static_cast<std::uint32_t>(i), &name[i], 1)) {
              return Outcome::Answered(err::kEfault, "prctl(PR_SET_NAME): the name pointer at 0x" +
                                                         Hex(f.arg[1]) + " is not readable guest memory");
            }
            if (name[i] == '\0') break;
          }
          thread_name_ = name;
          return Outcome::Emulated(0, std::string{"prctl(PR_SET_NAME, \""} + name.data() +
                                      "\") recorded in the monitor; the host thread is not renamed");
        }
        if (!memory_->Write(f.arg[1], thread_name_.data(), thread_name_.size())) {
          return Outcome::Answered(err::kEfault, "prctl(PR_GET_NAME): the buffer at 0x" + Hex(f.arg[1]) +
                                                     " is not writable guest memory");
        }
        return Outcome::Emulated(0, std::string{"prctl(PR_GET_NAME) -> \""} + thread_name_.data() + "\"");
      }
      return Outcome::Refused(err::kEnosys, "svc #172 (prctl) is not implemented for option 0x" +
                                                Hex(option) +
                                                "; this build answers PR_SET_NAME (15) and PR_GET_NAME "
                                                "(16) only");
    }

    case nr::kOpenat: {
      if (!files_) {
        // The existing shape, kept because it is what a launch with no provider must say: nothing here
        // can serve a file, and the empty allowlist is the other half of why.
        return Outcome::Refused(err::kEnosys,
                                "openat: no file provider is installed on this monitor and the "
                                "forwarding allowlist is empty, so there is nothing that can serve a "
                                "file; refused with ENOSYS (-38) rather than answered with a 0");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "openat: no guest memory is attached to this monitor, so "
                                              "the path cannot be read");
      }
      const std::int32_t dirfd = static_cast<std::int32_t>(f.arg[0]);
      if (dirfd != kAtFdcwd) {
        return Outcome::Refused(err::kEnosys,
                                "svc #322 (openat) is not implemented for a dirfd other than AT_FDCWD "
                                "(-100): this build has no directory descriptors to be relative to");
      }
      std::string path;
      const PathStatus status = ReadGuestPath(*memory_, f.arg[1], &path);
      if (status == PathStatus::kUnmapped) {
        return Outcome::Answered(err::kEfault, "openat: the path pointer 0x" + Hex(f.arg[1]) +
                                                   " is not readable guest memory");
      }
      if (status == PathStatus::kTooLong) {
        return Outcome::Answered(err::kEnametoolong,
                                 "openat: the name at 0x" + Hex(f.arg[1]) +
                                     " has no NUL in the first " + std::to_string(kMaxPath) +
                                     " bytes; a real openat of an over-long name was measured at -36 "
                                     "(ENAMETOOLONG) on the SM-X710");
      }
      const std::uint32_t flags = f.arg[2];
      // The provider is consulted *before* the policy refusals: a device answers ENOENT for a
      // nonexistent path regardless of the open's intent, and an app that probes with
      // open(path, O_WRONLY) to test existence then branches on the errno takes the wrong branch when
      // EROFS comes back for a path nothing serves.
      std::optional<std::vector<std::uint8_t>> bytes = files_(path);
      if (!bytes) {
        return Outcome::Answered(err::kEnoent, "openat('" + path +
                                                   "') = -ENOENT (-2): the launcher's file provider does "
                                                   "not serve that path");
      }
      if ((flags & kOAccMode) != 0 || (flags & (kOCreat | kOTrunc | kOAppend)) != 0) {
        return Outcome::Answered(err::kErofs,
                                 "openat('" + path + "', flags=0x" + Hex(flags) + ") = -EROFS (-30): the "
                                 "launcher's file provider serves read-only images, so a write-intent "
                                 "open is refused rather than answered with a descriptor writes would "
                                 "silently go nowhere through");
      }
      if ((flags & kODirectory) != 0) {
        return Outcome::Answered(err::kEnotdir, "openat('" + path + "', O_DIRECTORY) = -ENOTDIR (-20): "
                                                    "the provider serves files, and this build has no "
                                                    "directory descriptors");
      }
      if (served_.size() >= kMaxServedFds) {
        return Outcome::Answered(err::kEmfile, "openat('" + path + "') = -EMFILE (-24): this monitor's "
                                                          "descriptor table is full (" +
                                                   std::to_string(kMaxServedFds) + ")");
      }
      const std::int32_t fd = NextFd();
      served_.push_back(ServedFile{path, std::move(*bytes), 0, fd});
      const std::string why = "openat('" + path + "') -> fd " + std::to_string(fd) + " (" +
                              std::to_string(served_.back().bytes.size()) +
                              " bytes from the launcher's provider, read-only)";
      Observe(why);
      return Outcome::Emulated(fd, why);
    }

    case nr::kClose: {
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
      for (auto it = served_.begin(); it != served_.end(); ++it) {
        if (it->fd == fd) {
          served_.erase(it);
          return Outcome::Emulated(0, "close(" + std::to_string(fd) + ") -> 0");
        }
      }
      return Outcome::Answered(err::kEbadf, "close(" + std::to_string(fd) +
                                                ") = -EBADF (-9): no descriptor with that number is open "
                                                "in this instance");
    }

    case nr::kRead: {
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
      ServedFile* file = FileFor(fd);
      if (file == nullptr) {
        return Outcome::Answered(err::kEbadf, "read(fd=" + std::to_string(fd) +
                                                  ") = -EBADF (-9): no descriptor with that number is open "
                                                  "in this instance");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "read: no guest memory is attached to this monitor");
      }
      const std::uint32_t count = f.arg[2];
      if (count == 0) return Outcome::Emulated(0, "read(fd=" + std::to_string(fd) + ", 0) -> 0");
      const std::size_t left = file->bytes.size() - file->offset;
      const std::size_t n = std::min<std::size_t>(count, left);
      if (n != 0 && !memory_->Write(f.arg[1], file->bytes.data() + file->offset, n)) {
        return Outcome::Answered(err::kEfault, "read(fd=" + std::to_string(fd) + "): the buffer at 0x" +
                                                   Hex(f.arg[1]) + " is not writable guest memory");
      }
      file->offset += n;
      if (n == 0) {
        return Outcome::Emulated(0, "read(fd=" + std::to_string(fd) + ") -> 0: end of the " +
                                        std::to_string(file->bytes.size()) + "-byte file");
      }
      return Outcome::Emulated(static_cast<std::int32_t>(n),
                               "read(fd=" + std::to_string(fd) + ", " + std::to_string(count) + ") -> " +
                                   std::to_string(n) + " (offset " + std::to_string(file->offset) +
                                   " of " + std::to_string(file->bytes.size()) + ")");
    }

    case nr::kWrite: {
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
      // The guest's own words, before the descriptor table is consulted: fd 1 and 2 are the console,
      // and a build that answers EBADF drops exactly the line that explains what the guest is about to
      // do — measured, not imagined (SM-X710 2026-10-09: the game's constructor wrote to fd 2 four
      // times before `exit_group(127)`, and every byte went nowhere).
      if (fd == 1 || fd == 2) return CaptureOutput(fd, f.arg[1], f.arg[2]);
      if (FileFor(fd) != nullptr) {
        return Outcome::Answered(err::kEbadf, "write(fd=" + std::to_string(fd) +
                                                  ") = -EBADF (-9): this descriptor was opened read-only "
                                                  "from the launcher's file provider");
      }
      return Outcome::Answered(err::kEbadf, "write(fd=" + std::to_string(fd) +
                                                ") = -EBADF (-9): no descriptor with that number is open "
                                                "in this instance (this build has no stdout/stderr "
                                                "descriptors either)");
    }

    case nr::kWritev: {
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
      if (fd != 1 && fd != 2) {
        return Outcome::Answered(err::kEbadf, "writev(fd=" + std::to_string(fd) +
                                                  ") = -EBADF (-9): no descriptor with that number is "
                                                  "open in this instance");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "writev: no guest memory is attached to this monitor");
      }
      // The iovec array is guest memory: `{u32 base, u32 len}` per entry, arm32 layout. Entries past
      // the eighth are not walked — the full count is still reported written, the way a console fd
      // swallows everything — and the detail says so rather than pretending the cap does not exist.
      const std::uint32_t asked = f.arg[2];
      const std::uint32_t count = std::min<std::uint32_t>(asked, 8u);
      std::int32_t total = 0;
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t base = 0, len = 0;
        if (!memory_->Read(f.arg[1] + i * 8u, &base, 4) ||
            !memory_->Read(f.arg[1] + i * 8u + 4u, &len, 4)) {
          return Outcome::Answered(err::kEfault,
                                   "writev(fd=" + std::to_string(fd) + "): the iovec entry " +
                                       std::to_string(i) + " at 0x" + Hex(f.arg[1] + i * 8u) +
                                       " is not readable guest memory");
        }
        const Outcome one = CaptureOutput(fd, base, len);
        if (one.r0 < 0) return one;
        total += static_cast<std::int32_t>(len);
      }
      return Outcome::Emulated(total, "writev(fd=" + std::to_string(fd) + ", " +
                                          std::to_string(asked) + " iovec(s)) -> " +
                                          std::to_string(total) +
                                          (asked > count ? " (entries past the eighth not walked)" : ""));
    }

    case nr::kMunmap: {
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "munmap: no guest memory is attached to this monitor");
      }
      const std::uint64_t end = static_cast<std::uint64_t>(f.arg[0]) + f.arg[1];
      if (f.arg[1] == 0 || end > memory_->window_bytes()) {
        return Outcome::Answered(err::kEinval, "munmap(0x" + Hex(f.arg[0]) + ", " +
                                                   std::to_string(f.arg[1]) +
                                                   ") = -EINVAL (-22): the range is empty or outside the "
                                                   "guest window");
      }
      // A kernel unmaps what covers the range and returns 0 for holes; doing the same per page keeps
      // `free()` of a partly-freed arena from being an error the guest cannot act on.
      const std::uint32_t first = f.arg[0] & ~(mem::kGuestPage - 1u);
      const std::uint32_t stop =
          static_cast<std::uint32_t>((end + mem::kGuestPage - 1u) &
                                     ~static_cast<std::uint64_t>(mem::kGuestPage - 1u));
      std::size_t unmapped = 0;
      for (std::uint32_t at = first; at < stop; at += mem::kGuestPage) {
        if (!memory_->IsMapped(at)) continue;
        std::string uerr;
        if (!memory_->Unmap(at, mem::kGuestPage, &uerr)) {
          return Outcome::Answered(err::kEfault, "munmap(0x" + Hex(at) + "): " + uerr);
        }
        ++unmapped;
      }
      return Outcome::Emulated(0, "munmap(0x" + Hex(first) + "..0x" + Hex(stop) + ") -> 0 (" +
                                     std::to_string(unmapped) + " mapped page(s) released)");
    }

    case nr::kMmap2: {
      // ARM's `mmap2`: the sixth argument is an offset in *pages*. bionic's allocator reaches this
      // call before anything else in an app can run, and refusing it is what made every allocation in
      // the game's constructors fail — measured (SM-X710 2026-10-09: DT_INIT_ARRAY[5] left through
      // `exit_group(127)` right after 19 refused mmaps). Anonymous mappings are served from the guest
      // window's free space; fd-backed ones are refused by name rather than half-answered.
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "mmap2: no guest memory is attached to this monitor");
      }
      const std::uint32_t hint = f.arg[0];
      const std::uint32_t len = f.arg[1];
      const std::uint32_t prot = f.arg[2];
      const std::uint32_t flags = f.arg[3];
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[4]);
      if (len == 0) return Outcome::Answered(err::kEinval, "mmap2(len=0) = -EINVAL (-22)");
      constexpr std::uint32_t kMapAnonymous = 0x20u;
      if ((flags & kMapAnonymous) == 0 && fd != -1) {
        return Outcome::Refused(err::kEnosys,
                                "mmap2 with a file descriptor (fd=" + std::to_string(fd) +
                                    ") is not implemented: this monitor serves no file mappings, and a "
                                    "zero answer would be a mapping nobody owns");
      }
      const std::uint32_t size = (len + mem::kGuestPage - 1u) & ~(mem::kGuestPage - 1u);
      // The scan starts where the last anonymous mapping landed, so consecutive arenas stay
      // consecutive, then wraps to the base; a hint is honoured only when page-aligned and entirely
      // free, which is the kernel's own rule for a hint it is allowed to ignore. The base is one guest
      // page in — window-agnostic on purpose, because the model's window is 64 MiB on the launcher and
      // a few pages wide in tests, and a base tuned to one of them is a base the other cannot allocate
      // above.
      constexpr std::uint32_t kHeapBase = 0x0000'1000u;
      const auto free_run = [&](std::uint32_t cand) {
        if (cand < kHeapBase ||
            static_cast<std::uint64_t>(cand) + size > memory_->window_bytes()) {
          return false;
        }
        for (std::uint32_t pg = cand; pg < cand + size; pg += mem::kGuestPage) {
          if (memory_->IsMapped(pg)) return false;
        }
        return true;
      };
      std::uint32_t at = 0;
      if (hint != 0 && (hint & (mem::kGuestPage - 1u)) == 0 && free_run(hint)) at = hint;
      if (at == 0) {
        const std::uint32_t start = mmap_next_ >= kHeapBase ? mmap_next_ : kHeapBase;
        const std::uint32_t limit = static_cast<std::uint32_t>(memory_->window_bytes());
        for (std::uint32_t cand = start; at == 0 && cand + size <= limit; cand += mem::kGuestPage) {
          if (free_run(cand)) at = cand;
        }
        for (std::uint32_t cand = kHeapBase; at == 0 && cand + size <= start &&
                                             cand + size <= limit; cand += mem::kGuestPage) {
          if (free_run(cand)) at = cand;
        }
      }
      if (at == 0) {
        return Outcome::Answered(err::kEnomem,
                                 "mmap2(len=" + std::to_string(len) + ", prot=0x" + Hex(prot) +
                                     ") = -ENOMEM (-12): the guest window has no free run of " +
                                     std::to_string(size) + " bytes left");
      }
      const unsigned prot_bits = ((prot & 1u) ? mem::kRead : 0u) | ((prot & 2u) ? mem::kWrite : 0u) |
                                 ((prot & 4u) ? mem::kExec : 0u);
      std::string merr;
      if (!memory_->Map(at, size, prot_bits, &merr)) {
        return Outcome::Answered(err::kEnomem, "mmap2: could not map " + std::to_string(size) +
                                                   " bytes at 0x" + Hex(at) + ": " + merr);
      }
      mmap_next_ = at + size;
      return Outcome::Emulated(static_cast<std::int32_t>(at),
                               "mmap2(anon, len=" + std::to_string(len) + ", prot=0x" + Hex(prot) +
                                   ") -> 0x" + Hex(at) + ": " + std::to_string(size) +
                                   " bytes of the guest window's free space; protections are recorded, "
                                   "not enforced (see mprotect's note)");
    }

    case nr::kFutex: {
      // One thread is modelled, so the two ops bionic's pthread layer actually drives are answered and
      // the rest stay refusals. WAIT's immediate path is the kernel's own (value already changed ->
      // EAGAIN); a matching value would park a device's caller, and answering 0 says out loud that no
      // scheduler here will ever wake it.
      const std::uint32_t uaddr = f.arg[0];
      const std::uint32_t raw = f.arg[1] & 0x7fu;  ///< FUTEX_CMD_MASK: PRIVATE (128) and CLOCK_RT (256) off
      // bionic drives BOTH forms: plain `__futex_wait`/`wake` (0x0/0x1, device libc at 0x43180/0x43144)
      // and the _BITSET forms `__futex_wait_ex`/`wake_ex` use (0x89/0x8a, at 0x78418/0x75026 — measured
      // on the SM-X710's libc). The bitset selects which futex bits to wake; with one thread there is no
      // waiter set to select from, so both forms answer identically here and the note says as much. The
      // 0x7f mask was always right (0x89 -> op 9); what was missing was mapping 9/10 onto WAIT/WAKE. An
      // intermediate `& 0xff` kept the private flag and refused every private op — the device suite
      // caught it (`futex_answers_the_single_threaded_truth`'s 0x80 row).
      const std::uint32_t op = (raw == 9u) ? 0u : (raw == 10u) ? 1u : raw;
      if (op == 1 /*WAKE*/) {
        return Outcome::Emulated(0, "futex(WAKE, 0x" + Hex(uaddr) +
                                        ") -> 0: no waiter existed — this build models one thread, so no "
                                        "other thread can be parked");
      }
      if (op == 0 /*WAIT*/) {
        if (memory_ == nullptr) {
          return Outcome::Refused(err::kEnosys, "futex(WAIT): no guest memory is attached");
        }
        std::uint32_t cur = 0;
        if (!memory_->Read(uaddr, &cur, 4)) {
          return Outcome::Answered(err::kEfault, "futex(WAIT, 0x" + Hex(uaddr) +
                                                     "): the word is not readable guest memory");
        }
        if (cur != f.arg[2]) {
          return Outcome::Answered(err::kEagain,
                                   "futex(WAIT, 0x" + Hex(uaddr) + ") = -EAGAIN (-11): the word holds " +
                                       Hex(cur) + ", not the expected " + Hex(f.arg[2]) +
                                       "; the kernel's own immediate-return path");
        }
        const std::string why =
            "futex(WAIT, 0x" + Hex(uaddr) + ") saw the expected value and was answered 0 without "
            "blocking: this build models one thread, so no wake can arrive — a device's scheduler would "
            "park the caller here";
        Observe(why);
        return Outcome::Emulated(0, why);
      }
      return Outcome::Refused(err::kEnosys, "svc #240 (futex) op " + std::to_string(f.arg[1]) +
                                                " is not implemented; WAIT and WAKE are");
    }

    case nr::kRtSigaction: {
      // Accepted, recorded, never delivered: the host raises no signals into this build, and the call's
      // job in startup code is to install a handler — a fact about the guest's own state. `oldact` is
      // zeroed so a caller reading back the previous handler sees NULL, not whatever the buffer held.
      if (f.arg[2] != 0 && memory_ != nullptr) {
        const std::uint32_t zero[5] = {};  // arm32 struct sigaction: handler, flags, restorer, mask[2]
        memory_->Write(f.arg[2], zero, sizeof(zero));
      }
      const std::string why = "rt_sigaction(sig=" + std::to_string(f.arg[0]) +
                              ") accepted and recorded; no signal is ever delivered by this build, and "
                              "the previous handler reads back as NULL";
      Observe(why);
      return Outcome::Emulated(0, why);
    }

    case nr::kFaccessat: {
      if (!files_) {
        return Outcome::Refused(err::kEnosys,
                                "faccessat: no file provider is installed on this monitor");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "faccessat: no guest memory is attached");
      }
      if (static_cast<std::int32_t>(f.arg[0]) != kAtFdcwd) {
        return Outcome::Refused(err::kEnosys,
                                "svc #334 (faccessat) is not implemented for a dirfd other than "
                                "AT_FDCWD (-100)");
      }
      std::string path;
      const PathStatus status = ReadGuestPath(*memory_, f.arg[1], &path);
      if (status == PathStatus::kUnmapped) {
        return Outcome::Answered(err::kEfault, "faccessat: the path pointer 0x" + Hex(f.arg[1]) +
                                                   " is not readable guest memory");
      }
      if (status == PathStatus::kTooLong) {
        return Outcome::Answered(err::kEnametoolong,
                                 "faccessat: the name at 0x" + Hex(f.arg[1]) +
                                     " has no NUL in the first " + std::to_string(kMaxPath) + " bytes");
      }
      if (!files_(path)) {
        return Outcome::Answered(err::kEnoent, "faccessat('" + path +
                                                   "') = -ENOENT (-2): the launcher's file provider does "
                                                   "not serve that path");
      }
      return Outcome::Emulated(0, "faccessat('" + path + "') -> 0 (served and readable)");
    }

    case nr::kFstat64: {
      const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
      ServedFile* file = FileFor(fd);
      if (file == nullptr) {
        return Outcome::Answered(err::kEbadf, "fstat64(fd=" + std::to_string(fd) +
                                                  ") = -EBADF (-9): no descriptor with that number is open "
                                                  "in this instance");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "fstat64: no guest memory is attached to this monitor");
      }
      std::string why;
      if (!WriteStat64(*memory_, f.arg[1], file->bytes.size(), &why)) {
        return Outcome::Answered(err::kEfault, "fstat64(fd=" + std::to_string(fd) + "): " + why);
      }
      return Outcome::Emulated(0, "fstat64(fd=" + std::to_string(fd) + ") -> a " +
                                      std::to_string(file->bytes.size()) +
                                      "-byte read-only regular file in the arm32 struct stat64 layout");
    }

    case nr::kFstatat64: {
      if (!files_) {
        return Outcome::Refused(err::kEnosys,
                                "fstatat64: no file provider is installed on this monitor and the "
                                "forwarding allowlist is empty, so there is nothing that can answer for "
                                "a path; refused with ENOSYS (-38) rather than answered with a 0");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "fstatat64: no guest memory is attached to this monitor, "
                                              "so the path cannot be read");
      }
      const std::int32_t dirfd = static_cast<std::int32_t>(f.arg[0]);
      if (dirfd != kAtFdcwd) {
        return Outcome::Refused(err::kEnosys,
                                "svc #327 (fstatat64) is not implemented for a dirfd other than "
                                "AT_FDCWD (-100): this build has no directory descriptors");
      }
      std::string path;
      const PathStatus status = ReadGuestPath(*memory_, f.arg[1], &path);
      if (status == PathStatus::kUnmapped) {
        return Outcome::Answered(err::kEfault, "fstatat64: the path pointer 0x" + Hex(f.arg[1]) +
                                                   " is not readable guest memory");
      }
      if (status == PathStatus::kTooLong) {
        return Outcome::Answered(err::kEnametoolong,
                                 "fstatat64: the name at 0x" + Hex(f.arg[1]) +
                                     " has no NUL in the first " + std::to_string(kMaxPath) + " bytes");
      }
      std::optional<std::vector<std::uint8_t>> bytes = files_(path);
      if (!bytes) {
        return Outcome::Answered(err::kEnoent, "fstatat64('" + path +
                                                   "') = -ENOENT (-2): the launcher's file provider does "
                                                   "not serve that path");
      }
      std::string why;
      if (!WriteStat64(*memory_, f.arg[2], bytes->size(), &why)) {
        return Outcome::Answered(err::kEfault, "fstatat64('" + path + "'): " + why);
      }
      return Outcome::Emulated(0, "fstatat64('" + path + "') -> a " + std::to_string(bytes->size()) +
                                      "-byte read-only regular file");
    }

    case nr::kExecve:
      // The one refusal with a *measured* errno rather than a default. A real 32-bit app process asking
      // for its own file gets EACCES from SELinux, so that is what a guest gets here — and the reason it
      // is spelled out rather than left to the default is that `ENOSYS` and `EACCES` look equally
      // plausible in a log while telling an app's developer two different stories: "this emulator cannot
      // do it" versus "Android will not let you do it, anywhere".
      return Outcome::Denied(err::kEacces,
                             "execve: measured EACCES (13) from u:r:untrusted_app on SM-X710, SELinux "
                             "app_data_file execute_no_trans; see 30 §Step 12");

    default: {
      // The reason names the call, so a reader on a phone can tell this from "no monitor attached" and
      // from "answered with ENOENT". The number is decimal because that is what `svc #<n>` means; the
      // name is the same table `Describe` prints.
      const char* named = Name(f.number);
      std::string why = "svc #" + std::to_string(f.number);
      if (named != nullptr) {
        why += std::string{" ("} + named +
               ") is not implemented in this build; refused with ENOSYS (-38) rather than inventing a"
               " result a real kernel would produce";
      } else {
        why += " is not implemented; refusing with ENOSYS (-38), the value this kernel answers for a"
               " number outside its table, rather than a 0 nobody produced";
      }
      return Outcome::Refused(err::kEnosys, why);
    }
  }
}

Outcome Monitor::Handle(exec::MachineState& state, std::uint32_t svc_imm) {
  const Frame f = Frame::Capture(state, svc_imm);
  Note(f.number);
  Outcome o = Classify(f, state);
  // Refusals and errno answers are recorded alongside the successes: the report's answer lines exist
  // so a run that died on an `openat` can say which path and why, not merely that openat happened.
  // Only four success sites called `Observe` before, so every refusal reached the report as a tally
  // entry with no reason attached — the exact silent-ignore shape the note machinery was built to
  // prevent.
  if (o.r0 < 0) {
    ++refusals_;
    Observe(o.detail);
  }
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
  // The counts say what was asked; only these lines say what was answered. Without them a report that
  // ends on a refusal cannot distinguish a call the monitor answered with ENOENT from one it had no
  // handler for — the distinction `docs/20` §C07 asks the reason text to carry.
  if (!notes_.empty()) {
    e.active += ". Answered (or refused) in guest terms: ";
    for (std::size_t i = 0; i < notes_.size(); ++i) {
      if (i != 0) e.active += " | ";
      e.active += notes_[i];
    }
    if (dropped_notes_ != 0) {
      e.active += " | +" + std::to_string(dropped_notes_) + " more distinct answer(s), not stored";
    }
  }
  // `inert` rather than `active` for the forwarding case, because a declared row that was not honoured is
  // exactly the shape C17 exists to surface. Today nothing is declared, so `active` is the truth.
  e.outcome = ForwardsAnything() ? diag::Outcome::kInert : diag::Outcome::kActive;
  e.evidence = evidence;
  rec.Record(e);
}

}  // namespace rb::syscall
