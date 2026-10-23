/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/syscall/monitor.h"
#include "rb/syscall/host_io.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <thread>
#include <vector>

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

/// The host's own clocks, and the only place in this file that asks the host anything. The wall and monotonic
/// clocks are facts a process can have. The CPU-time clocks are answered from the host too: the calling host thread
/// is the one running this guest thread, so its CPU time is that thread's work (plus the emulator's, which makes it
/// larger, never smaller or stuck). They were refused until M4: games budget work by clock() (CLOCK_PROCESS_CPUTIME_ID)
/// and a refusal stalled Fruit Ninja's loader for good.
bool HostClock(std::int32_t clock_id, std::int64_t* nanos) {
  // The wall and monotonic clocks and their raw/coarse/boot variants (0, 1, 4..7) — the guest kernel's answer — and
  // the process and thread CPU clocks (2, 3). A clock id encoding a pid or tid (negative) is not answered.
  const clockid_t id = (clock_id >= 0 && clock_id <= 7) ? clock_id : -1;
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
    case Verdict::kBlocking: return "blocking";
    case Verdict::kThreadExit: return "thread-exit";
  }
  return "unknown";
}

const char* Name(std::uint32_t number) {
  switch (number) {
    case nr::kExit: return "exit";
    case nr::kRead: return "read";
    case nr::kWrite: return "write";
    case nr::kClose: return "close";
    case nr::kGetpid: return "getpid";
    case nr::kBrk: return "brk";
    case nr::kIoctl: return "ioctl";
    case nr::kGettimeofday: return "gettimeofday";
    case nr::kMunmap: return "munmap";
    case nr::kFstat: return "fstat";
    case nr::kClone: return "clone";
    case nr::kMprotect: return "mprotect";
    case nr::kMremap: return "mremap";
    case nr::kWritev: return "writev";
    case nr::kPrctl: return "prctl";
    case nr::kRtSigaction: return "rt_sigaction";
    case nr::kRtSigreturn: return "rt_sigreturn";
    case nr::kRtSigprocmask: return "rt_sigprocmask";
    case nr::kRtSigsuspend: return "rt_sigsuspend";
    case nr::kMmap2: return "mmap2";
    case nr::kFstat64: return "fstat64";
    case nr::kGetdents64: return "getdents64";
    case nr::kMadvise: return "madvise";
    case nr::kFcntl64: return "fcntl64";
    case nr::kGettid: return "gettid";
    case nr::kFutex: return "futex";
    case nr::kSchedYield: return "sched_yield";
    case nr::kNanosleep: return "nanosleep";
    case nr::kExitGroup: return "exit_group";
    case nr::kSetTidAddress: return "set_tid_address";
    case nr::kClockGettime: return "clock_gettime";
    case nr::kClockNanosleep: return "clock_nanosleep";
    case nr::kTgkill: return "tgkill";
    case nr::kTkill: return "tkill";
    case nr::kRtTgsigqueueinfo: return "rt_tgsigqueueinfo";
    case nr::kClockGetres: return "clock_getres";
    case nr::kUname: return "uname";
    case nr::kSysinfo: return "sysinfo";
    case nr::kSchedSetaffinity: return "sched_setaffinity";
    case nr::kSchedGetaffinity: return "sched_getaffinity";
    case nr::kPrlimit64: return "prlimit64";
    case nr::kSchedGetparam: return "sched_getparam";
    case nr::kSchedSetscheduler: return "sched_setscheduler";
    case nr::kSchedGetscheduler: return "sched_getscheduler";
    case nr::kSchedGetPriorityMax: return "sched_get_priority_max";
    case nr::kSchedGetPriorityMin: return "sched_get_priority_min";
    case nr::kSetpriority: return "setpriority";
    case nr::kGetpriority: return "getpriority";
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

std::uint64_t Monitor::AllocLimit() const {
  const std::uint64_t window = memory_->window_bytes();
  return alloc_ceiling_ != 0 ? std::min<std::uint64_t>(window, alloc_ceiling_) : window;
}

std::uint32_t Monitor::FindFreeRun(std::uint32_t hint, std::uint32_t size) const {
  // A hint is honoured only when page-aligned and entirely free, which is the kernel's own rule for a hint it
  // is allowed to ignore; otherwise the highest free run (top-down, below). The floor is one guest page in —
  // window-agnostic on purpose, because the model's window is 64 MiB on the launcher and a few pages
  // wide in tests, and a base tuned to one of them is a base the other cannot allocate above.
  //
  // All bounds are 64-bit and end at `AllocLimit()`: a 4 GiB window's size is 0 as a uint32, which made a
  // 32-bit limit refuse every allocation, and `cand + size` wraps at the top of the space.
  constexpr std::uint64_t kHeapBase = 0x0000'1000u;
  const std::uint64_t limit = AllocLimit();
  const auto free_run = [&](std::uint64_t cand) {
    if (cand < kHeapBase || cand + size > limit) return false;
    for (std::uint64_t pg = cand; pg < cand + size; pg += mem::kGuestPage) {
      if (memory_->IsMapped(static_cast<std::uint32_t>(pg))) return false;
    }
    return true;
  };
  if (hint != 0 && (hint & (mem::kGuestPage - 1u)) == 0 && free_run(hint)) return hint;
  // Top-down, as Linux's default mmap layout places them: the highest free run below the limit. A mapping then
  // sits just below the one made before it, so memory past the end of the newest region is an older region, as
  // on a device — Gumball's cLanguage::read scans past its last string for a non-zero byte and relies on that.
  // A mapped page inside a candidate moves the search to just below that page.
  if (size == 0 || size > limit - kHeapBase) return 0;
  std::uint64_t cand = (limit - size) & ~static_cast<std::uint64_t>(mem::kGuestPage - 1u);
  while (cand >= kHeapBase) {
    std::uint64_t blocked = 0;
    for (std::uint64_t pg = cand + size; pg > cand;) {
      pg -= mem::kGuestPage;
      if (memory_->IsMapped(static_cast<std::uint32_t>(pg))) {
        blocked = pg + 1;  // +1 so a block at page 0 is still "found"
        break;
      }
    }
    if (blocked == 0) return static_cast<std::uint32_t>(cand);
    const std::uint64_t page = blocked - 1;
    if (page < kHeapBase + size) return 0;
    cand = page - size;
  }
  return 0;
}

bool Monitor::ServesItself(const Frame& f) {
  if (HostIo::TakesFd(f.number)) {
    const std::int32_t fd = static_cast<std::int32_t>(f.arg[0]);
    if ((f.number == nr::kWrite || f.number == nr::kWritev) && (fd == 1 || fd == 2)) return true;  // the console
    for (const ServedFile& s : served_) {
      if (s.fd == fd) return true;
    }
    return false;
  }
  unsigned index = 0;
  // Only the path calls this monitor has handlers for consult the provider (its virtual files).
  if (files_ && memory_ != nullptr && HostIo::TakesPath(f.number, &index) &&
      (f.number == nr::kOpenat || f.number == nr::kFstatat64 || f.number == nr::kFaccessat ||
       f.number == nr::kReadlinkat)) {
    std::string path;
    if (ReadGuestPath(*memory_, f.arg[index], &path) != PathStatus::kOk) return false;
    return files_(path).has_value();
  }
  return false;
}

Outcome Monitor::Classify(const Frame& f, exec::MachineState& state, runtime::GuestThread* thread) {
  runtime::GuestThread& th = thread != nullptr ? *thread : *main_;
  // Diagnostics for a stall dump: the last call this thread asked for, even if it is now blocked in it.
  th.last_syscall.store(f.number);
  if (host_io_ && memory_ != nullptr && HostIo::Handles(f.number) && !ServesItself(f)) {
    return HostIo::Serve(f, *memory_);
  }
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
      if (f.number == nr::kGettid && th.tid == 0) {
        return Outcome::Refused(err::kEnosys, "gettid on a guest thread that was given no tid");
      }
      const std::int32_t id = f.number == nr::kGetpid ? pid_ : th.tid;
      return Outcome::Emulated(id, "per-instance id supplied by the launcher");
    }

    case nr::kSchedYield: {
      // Blocking in the GGL's sense: the point of the call is to let another thread run, so the yield
      // happens with the lock released (`Verdict::kBlocking`; the bridge runs `block` unlocked).
      Outcome o{Verdict::kBlocking, 0, "sched_yield() -> 0, yielded with the GGL released", {}};
      o.block = [] {
        std::this_thread::yield();
        return Outcome::Emulated(0, "sched_yield() -> 0, yielded with the GGL released");
      };
      return o;
    }

    case nr::kNanosleep: {
      // nanosleep(req, rem) with the arm32 kernel's 32-bit `struct timespec` {tv_sec, tv_nsec}. The request
      // is read and validated here, under the GGL; only the sleep itself runs unlocked. A signal for this
      // thread cuts the sleep short with EINTR; `rem` is not written, which is a named gap (the answer
      // says so) rather than a fabricated remaining time.
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "nanosleep: no guest memory is attached to this monitor");
      }
      std::int32_t req[2] = {};
      if (!memory_->Read(f.arg[0], req, sizeof req)) {
        return Outcome::Answered(err::kEfault, "nanosleep: the request at 0x" + Hex(f.arg[0]) +
                                                   " is not readable guest memory");
      }
      if (req[0] < 0 || req[1] < 0 || req[1] >= 1'000'000'000) {
        return Outcome::Answered(err::kEinval, "nanosleep({" + std::to_string(req[0]) + ", " +
                                                   std::to_string(req[1]) +
                                                   "}) = -EINVAL (-22): tv_sec < 0 or tv_nsec outside [0, 1e9)");
      }
      const std::int64_t nanos = static_cast<std::int64_t>(req[0]) * 1'000'000'000 + req[1];
      return Sleep("nanosleep({" + std::to_string(req[0]) + ", " + std::to_string(req[1]) + "})", nanos, th);
    }

    case nr::kClockNanosleep: {
      // clock_nanosleep(clockid, flags, req, rem): the raw syscall answers -errno (bionic's wrapper turns it
      // into the positive return). TIMER_ABSTIME (1) makes `req` an absolute time on `clockid`.
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "clock_nanosleep: no guest memory is attached to this monitor");
      }
      const auto clock_id = static_cast<std::int32_t>(f.arg[0]);
      const std::uint32_t flags = f.arg[1];
      std::int32_t req[2] = {};
      if (!memory_->Read(f.arg[2], req, sizeof req)) {
        return Outcome::Answered(err::kEfault, "clock_nanosleep: the request at 0x" + Hex(f.arg[2]) +
                                                   " is not readable guest memory");
      }
      const std::string call = "clock_nanosleep(clock " + std::to_string(clock_id) + ", flags " +
                               std::to_string(flags) + ", {" + std::to_string(req[0]) + ", " +
                               std::to_string(req[1]) + "})";
      if (req[0] < 0 || req[1] < 0 || req[1] >= 1'000'000'000 || (flags & ~1u) != 0) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22)");
      }
      std::int64_t now = 0;
      if (!Now(clock_id, &now)) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): no fact for that clock id here");
      }
      const std::int64_t t = static_cast<std::int64_t>(req[0]) * 1'000'000'000 + req[1];
      const std::int64_t nanos = (flags & 1u) != 0 ? std::max<std::int64_t>(0, t - now) : t;
      return Sleep(call, nanos, th);
    }

    case nr::kExitGroup: {
      // The process is finished: every other guest thread stops at its next slice boundary or syscall, and
      // every one parked in a futex wait or a sleep is woken to do so (the bridge's run loop checks this).
      const std::string why = "exit_group(" + Hex(f.arg[0]) + ")";
      RequestGroupExit(why + " on tid " + std::to_string(th.tid));
      return Outcome{Verdict::kExit, static_cast<std::int32_t>(f.arg[0]), why};
    }

    case nr::kExit:
      return ThreadExit(f, th);

    case nr::kClone:
      return Clone(f, state, th);

    case nr::kTkill:
    case nr::kRtTgsigqueueinfo:
    case nr::kTgkill: {
      // The fatal-signal family, under all three numbers a Unity boot uses: `tgkill(tgid, tid, sig)`,
      // `tkill(tid, sig)` and `rt_tgsigqueueinfo(tgid, tid, sig, info)`. They are the same operation with the
      // arguments shifted, and only `tgkill` was handled, which is how a scan for 268 read a process that never
      // touched a signal — while Cars 3 asked 238 and 363 in the same microsecond as 268.
      //
      // C26: a real signal is now QUEUED for its target thread instead of refused. The target takes it at the
      // ABI's next slice boundary and enters the handler the guest installed (see `DeliverPendingSignal` in
      // the runtime). Measured, and this is what the whole change is for (SM-X710 2026-10-19): Cars 3 installs
      // a handler for signal 30 at libmono.so+0x2bdb59, then `tkill(tid, 30)` — SIGPWR, Mono's GC
      // thread-suspend — was refused, so the runtime aborted (SIGABRT) and called `exit(127)`. MadTruck died on
      // the identical pair. The return value stays the kernel's 0, which is now also the truth: the signal is
      // pending.
      const bool queued = f.number == nr::kRtTgsigqueueinfo;
      const bool tkill_form = f.number == nr::kTkill;  // tkill(tid, sig); the other two are (tgid, tid, sig[, info])
      const auto tgid = static_cast<std::int32_t>(f.arg[0]);
      const auto tid = static_cast<std::int32_t>(tkill_form ? f.arg[0] : f.arg[1]);
      const auto sig = static_cast<std::int32_t>(tkill_form ? f.arg[1] : f.arg[2]);
      const char* what = queued ? "rt_tgsigqueueinfo" : (tkill_form ? "tkill" : "tgkill");
      const std::string call = std::string{what} + "(" +
                               (tkill_form ? "" : std::to_string(tgid) + ", ") +
                               std::to_string(tid) + ", " + std::to_string(sig) +
                               (queued ? ", &info" : "") + ")";
      if (sig < 0 || sig > 64) {
        if (signal_trace_) {
          signal_trace_("sig: " + call + " -> -EINVAL (-22): " + std::to_string(sig) +
                        " is not a signal number on this ABI (0..64)");
        }
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): " + std::to_string(sig) +
                                                   " is not a signal number on this ABI (0..64)");
      }
      // tkill carries no tgid, so the tid alone decides; the other two also require the group id to match.
      const bool group_ok = f.number == nr::kTkill || (ids_set_ && tgid == pid_);
      const bool alive = group_ok && (tid == main_->tid || live_tids_.count(tid) != 0);
      if (!alive) return Outcome::Answered(err::kEsrch, call + " = -ESRCH (-3): no such live thread in this process");
      if (sig == 0) return Outcome::Emulated(0, call + " -> 0: the thread is alive");
      // A signal is checked against the guest's own dispositions, so a report says which of the three things
      // happened: entered a handler, ran a default action, or ignored. SIGKILL/SIGSTOP cannot be caught, and
      // saying so is better than a generic delivery.
      const Disposition d = disposition(sig);
      if (d.handler == 1) {
        return Outcome::Emulated(0, call + " -> 0: signal " + std::to_string(sig) +
                                        " is SIG_IGN for this guest, so it is dropped");
      }
      if (sig == 9 || sig == 19) {
        return Outcome::Refused(err::kEnosys, call + ": signal " + std::to_string(sig) +
                                                  " cannot be caught and this build does not run default "
                                                  "actions; refusing rather than pretending it took");
      }
      QueueSignal(tid, sig);
      if (signal_trace_) {
        signal_trace_("sig: " + call + " -> 0: queued for delivery" +
                      (d.catches() ? ", handler 0x" + Hex(d.handler) + " (restorer 0x" + Hex(d.restorer) + ")"
                                   : ", no handler installed — the default action will be reported, not run"));
      }
      return Outcome::Emulated(0, call + " -> 0: signal " + std::to_string(sig) + " queued for tid " +
                                      std::to_string(tid) +
                                      (d.catches() ? " (a handler is installed)" : " (no handler installed)"));
    }

    case nr::kMadvise: {
      // madvise(addr, len, advice). MADV_DONTNEED (4) drops the pages: a later read sees zeros, which is what an
      // allocator returning memory relies on — for read-only private pages too (kernel semantics), so a page
      // without write access is opened for the zero-fill and put back. Executable pages are the exception: no
      // guest page here records whether it came from a file, and a device would re-read a file-backed code page
      // rather than zero it, so they are left as they are (and the note counts them). The hints a kernel may
      // ignore answer 0; MADV_REMOVE (9) needs a shared mapping this build never makes (the kernel's EINVAL for
      // a private one); any advice the kernel does not know is EINVAL, by number.
      const std::uint32_t at = f.arg[0];
      const std::uint32_t len = f.arg[1];
      const std::uint32_t advice = f.arg[2];
      const std::string call = "madvise(0x" + Hex(at) + ", " + std::to_string(len) + ", " + std::to_string(advice) + ")";
      if ((at & (mem::kGuestPage - 1u)) != 0) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): the address is not page-aligned");
      }
      const bool hint = advice <= 3u /* NORMAL RANDOM SEQUENTIAL WILLNEED */ || advice == 8u /* FREE */ ||
                        (advice >= 10u && advice <= 21u) /* DONTFORK..PAGEOUT */;
      if (advice == 9u) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): MADV_REMOVE needs a shared mapping, and "
                                                      "this build makes none (the kernel's answer for a private one)");
      }
      if (advice != 4u && !hint) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): advice " + std::to_string(advice) +
                                                   " is not one the kernel knows");
      }
      if (hint || len == 0) return Outcome::Emulated(0, call + " -> 0 (a hint: accepted, no effect)");
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "madvise: no guest memory is attached to this monitor");
      }
      const std::uint64_t end = static_cast<std::uint64_t>(at) + len;
      if (end > memory_->window_bytes()) return Outcome::Answered(err::kEnomem, call + " = -ENOMEM (-12)");
      static const std::uint8_t kZero[mem::kGuestPage] = {};
      unsigned zeroed = 0, kept = 0;
      for (std::uint64_t pg = at; pg < end; pg += mem::kGuestPage) {  // checked first: a refusal changes nothing
        const auto p = static_cast<std::uint32_t>(pg);
        if (!memory_->IsMapped(p)) return Outcome::Answered(err::kEnomem, call + " = -ENOMEM (-12): 0x" + Hex(p) + " is not mapped");
      }
      for (std::uint64_t pg = at; pg < end; pg += mem::kGuestPage) {
        const auto p = static_cast<std::uint32_t>(pg);
        const unsigned prot = memory_->ProtAt(p);
        if ((prot & mem::kExec) != 0) {
          ++kept;
          continue;
        }
        // DONTNEED drops whole pages (len is rounded up).
        if ((prot & mem::kWrite) != 0) {
          if (memory_->Write(p, kZero, mem::kGuestPage)) ++zeroed; else ++kept;
          continue;
        }
        std::string perr;
        const bool ok = memory_->Protect(p, mem::kGuestPage, prot | mem::kWrite, &perr) &&
                        memory_->Write(p, kZero, mem::kGuestPage);
        memory_->Protect(p, mem::kGuestPage, prot, &perr);
        if (ok) ++zeroed; else ++kept;
      }
      return Outcome::Emulated(0, call + " -> 0: MADV_DONTNEED zero-filled " + std::to_string(zeroed) +
                                      " page(s)" + (kept != 0 ? "; " + std::to_string(kept) +
                                                                     " executable page(s) left as they were"
                                                              : std::string{}));
    }

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
      // One direction *is* applied: opening a PROT_NONE reservation. bionic's pthread_create maps a whole
      // thread (guard + stack + static TLS + pthread_internal_t) PROT_NONE and then mprotects the usable part
      // read/write; leaving those pages inaccessible would fault the new thread's first store. A page with
      // no access gets exactly the requested protection (and zeroed backing, which is what such a page held).
      const unsigned want = ((prot & 1u) ? mem::kRead : 0u) | ((prot & 2u) ? mem::kWrite : 0u) |
                            ((prot & 4u) ? mem::kExec : 0u);
      unsigned opened = 0;
      if (want != mem::kNone) {
        for (std::uint64_t page = at & ~static_cast<std::uint64_t>(mem::kGuestPage - 1); page < end;
             page += mem::kGuestPage) {
          const auto p = static_cast<std::uint32_t>(page);
          if (memory_->ProtAt(p) != mem::kNone) continue;
          std::string perr;
          if (!memory_->Protect(p, mem::kGuestPage, want, &perr)) {
            return Outcome::Answered(err::kEnomem, "mprotect(" + range + ", " + asked + "): opening 0x" + Hex(p) +
                                                       " failed: " + perr);
          }
          ++opened;
        }
      }
      if (opened != 0) {
        return Outcome::Emulated(0, "mprotect(" + range + ", " + asked + ") = 0: " + std::to_string(opened) +
                                        " PROT_NONE page(s) opened with the requested protection");
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
      th.clear_child_tid = f.arg[0];
      const std::string why = "set_tid_address(0x" + Hex(th.clear_child_tid) + ") -> tid " +
                              std::to_string(th.tid) +
                              "; the clear-child-tid slot is recorded, and no thread exit walks it "
                              "because this build models one thread";
      Observe(why);
      return Outcome::Emulated(th.tid, why);
    }

    case nr::kSetRobustList: {
      // The kernel requires len == sizeof(struct robust_list_head) = 24 and EINVALs anything else;
      // the first draft answered 0 for any len, hiding a caller that passed the wrong object.
      if (f.arg[1] != 24u) {
        return Outcome::Answered(err::kEinval, "set_robust_list(len=" + std::to_string(f.arg[1]) +
                                                   ") = -EINVAL (-22): the kernel requires exactly "
                                                   "sizeof(struct robust_list_head) = 24");
      }
      th.robust_head = f.arg[0];
      th.robust_len = f.arg[1];
      const std::string why = "set_robust_list(head=0x" + Hex(th.robust_head) + ", len=" +
                              std::to_string(th.robust_len) +
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
                                    ") has no fact to answer from: this build answers clock ids 0..7 "
                                    "(wall, monotonic and CPU time); per-process or per-thread clock ids "
                                    "of another pid/tid are not mapped");
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

    case nr::kClockGetres: {
      // clock_getres(clockid, &res). The resolution of a clock is a fact about the host that is running this
      // guest, not a number to invent, so it is asked for and written back — and the guest's `timespec` is the
      // legacy arm32 shape (two 4-byte fields), exactly as in the case above.
      //
      // This call is in the Cars 3 death path: measured (SM-X710 2026-10-19) it is the first of three refusals
      // — 264, then 238 (`tkill`), then 363 (`rt_tgsigqueueinfo`) — after which the game calls exit_group(127).
      const std::int32_t id = static_cast<std::int32_t>(f.arg[0]);
      const std::string call = "clock_getres(" + std::to_string(id) + ")";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor, so there "
                                                   "is nowhere to write the timespec");
      }
      timespec host{};
      if (::clock_getres(static_cast<clockid_t>(id), &host) != 0) {
        // EINVAL is the kernel's own answer for a clock id it does not know, and this is the one clock call whose
        // refusal is not about this build: the host has the same table a device would.
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): no clock with that id");
      }
      const std::uint32_t at = f.arg[1];
      // A resolution of zero is what a kernel reports for a clock it cannot measure; hosts do this for
      // CLOCK_THREAD_CPUTIME_ID under some hypervisors. Written through rather than replaced, because a caller
      // that reads 0 and divides defensively is better served than one handed a plausible non-zero lie.
      const std::uint32_t words[2] = {static_cast<std::uint32_t>(host.tv_sec),
                                      static_cast<std::uint32_t>(host.tv_nsec)};
      if (at != 0 && !memory_->Write(at, words, sizeof words)) {
        return Outcome::Answered(err::kEfault, call + ": the timespec at 0x" + Hex(at) +
                                                   " is not writable guest memory");
      }
      return Outcome::Emulated(0, call + " = " + std::to_string(host.tv_sec) + "s " +
                                      std::to_string(host.tv_nsec) + "ns");
    }

    case nr::kUname: {
      // uname(&buf). The machine field is the one that has to differ from the host's: this guest is ARM32 and
      // asks what it is running on, so "armv7l" is the true answer to the question it asked, and the host's
      // "aarch64" would be the answer to a question nobody put. Every other field is the host's own.
      // `struct utsname` is six 65-byte fields on both the arm32 and the arm64 kernel ABI, so the copy is the
      // same shape; only that one field is corrected.
      struct utsname u {};
      if (::uname(&u) != 0) {
        return Outcome::Answered(err::kEfault, "uname: the host refused to describe itself");
      }
      std::memset(u.machine, 0, sizeof u.machine);
      std::snprintf(u.machine, sizeof u.machine, "armv7l");
      const std::uint32_t at = f.arg[0];
      if (at != 0 && !memory_->Write(at, &u, sizeof u)) {
        return Outcome::Answered(err::kEfault, "uname: the utsname at 0x" + Hex(at) +
                                                   " is not writable guest memory");
      }
      return Outcome::Emulated(0, "uname -> {sysname " + std::string{u.sysname} + ", release " +
                                      std::string{u.release} + ", machine armv7l}");
    }

    case nr::kSchedGetaffinity: {
      // sched_getaffinity(pid, cpusetsize, &mask). A guest asking which CPUs it may run on deserves the host's
      // real answer, truncated to the first 32 bits the arm32 `cpu_set_t` holds — not a fabricated "all of
      // them", which is what makes a thread pool size itself wrong on a phone that is thermally capped.
      const auto pid = static_cast<std::int32_t>(f.arg[0]);
      const std::uint32_t size = f.arg[1];
      const std::string call = "sched_getaffinity(" + std::to_string(pid) + ", " + std::to_string(size) + ")";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor");
      }
      cpu_set_t host;
      CPU_ZERO(&host);
      if (::sched_getaffinity(pid == 0 ? 0 : static_cast<pid_t>(pid), sizeof host, &host) != 0) {
        return Outcome::Answered(err::kEsrch, call + " = -ESRCH (-3): the host has no such thread");
      }
      // The arm32 cpu_set_t is 1024 bits (128 bytes) with the same bit-per-CPU layout, so the leading words map
      // straight across; anything the guest asked for beyond that is zero-filled, as a kernel does.
      std::vector<std::uint8_t> guest(size, 0);
      const std::size_t from_host = std::min<std::size_t>(size, sizeof host);
      std::memcpy(guest.data(), &host, from_host);
      if (size != 0 && !memory_->Write(f.arg[2], guest.data(), size)) {
        return Outcome::Answered(err::kEfault, call + ": the cpu_set at 0x" + Hex(f.arg[2]) +
                                                   " is not writable guest memory");
      }
      unsigned cpus = 0;
      for (int i = 0; i < CPU_SETSIZE; ++i) cpus += CPU_ISSET(i, &host) ? 1u : 0u;
      return Outcome::Emulated(0, call + " -> " + std::to_string(cpus) + " CPU(s)");
    }

    case nr::kSchedSetaffinity: {
      // sched_setaffinity(pid, cpusetsize, &mask). Applying a guest's request to the *host* thread would move
      // the emulator itself, which is not what the guest asked for and is not this build's to decide; the
      // request is read back and reported, and the guest is told it took, because the set it named is one the
      // host can already schedule on. The deviation is stated rather than hidden.
      const auto pid = static_cast<std::int32_t>(f.arg[0]);
      const std::uint32_t size = f.arg[1];
      const std::string call = "sched_setaffinity(" + std::to_string(pid) + ", " + std::to_string(size) + ")";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor");
      }
      std::vector<std::uint8_t> bits(size, 0);
      if (size != 0 && !memory_->Read(f.arg[2], bits.data(), size)) {
        return Outcome::Answered(err::kEfault, call + ": the cpu_set at 0x" + Hex(f.arg[2]) +
                                                   " is not readable guest memory");
      }
      return Outcome::Emulated(0, call + " = 0: recorded, not applied — the guest's threads already run on a "
                                         "superset of the CPUs a phone would give them, and moving the host "
                                         "threads that run them is not this build's decision");
    }

    case nr::kPrlimit64: {
      // prlimit64(pid, resource, &new, &old). The arm32 kernel's `struct rlimit64` is two 8-byte fields, so the
      // guest's shape is fixed by the ABI. RLIMIT_NOFILE and RLIMIT_STACK are the two a game's loader actually
      // reads, and both have a true host answer.
      const auto pid = static_cast<std::int32_t>(f.arg[0]);
      const auto resource = static_cast<std::int32_t>(f.arg[1]);
      const std::string call = "prlimit64(" + std::to_string(pid) + ", " + std::to_string(resource) + ")";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor");
      }
      rlimit host{};
      if (::prlimit(pid == 0 ? 0 : static_cast<pid_t>(pid), resource, nullptr, &host) != 0) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): no such resource, or no such process");
      }
      // A limit of RLIM_INFINITY does not survive the trip into a 32-bit field, so it is clamped to the widest
      // value the guest's own type can hold rather than wrapped to zero.
      constexpr std::uint64_t kInf = ~static_cast<std::uint64_t>(0);
      const auto clamp = [](rlim_t v) -> std::uint64_t {
        return v == RLIM_INFINITY ? kInf : static_cast<std::uint64_t>(v);
      };
      const std::uint64_t old_pair[2] = {clamp(host.rlim_cur), clamp(host.rlim_max)};
      if (f.arg[3] != 0 && !memory_->Write(f.arg[3], old_pair, sizeof old_pair)) {
        return Outcome::Answered(err::kEfault, call + ": the old rlimit at 0x" + Hex(f.arg[3]) +
                                                   " is not writable guest memory");
      }
      if (f.arg[2] != 0) {
        const std::uint64_t asked[2] = {clamp(host.rlim_cur), clamp(host.rlim_max)};
        return Outcome::Emulated(0, call + " = 0: cur " + std::to_string(asked[0]) + ", max " +
                                        std::to_string(asked[1]) + " (the request was read, not applied: this "
                                        "build does not lower the host's own limits)");
      }
      return Outcome::Emulated(0, call + " -> cur " + std::to_string(old_pair[0]) + ", max " +
                                      std::to_string(old_pair[1]));
    }

    case nr::kSysinfo: {
      // sysinfo(&info). A game reads totalram/freeram to size its own caches, and the host has those numbers.
      // The arm32 and arm64 kernels agree on the field layout of the leading `uptime`, `loads` and `totalram`
      // group, so only the leading prefix is copied: the struct grew over kernel versions and writing the
      // host's full 112 bytes over the guest's smaller one would overrun the caller's buffer.
      const std::uint32_t at = f.arg[0];
      const std::string call = "sysinfo";
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor");
      }
      struct ::sysinfo host {};
      if (::sysinfo(&host) != 0) {
        return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): the host refused to describe itself");
      }
      // The arm32 `struct sysinfo` prefix, field for field: uptime, loads[3], totalram, freeram, sharedram,
      // bufferram, totalswap, freeswap, procs. Written field by field so the widths are the guest's, not the
      // host's — `long` is 4 bytes on one side of this ABI pair and 8 on the other.
      const std::uint32_t words[10] = {
          static_cast<std::uint32_t>(host.uptime),
          static_cast<std::uint32_t>(host.loads[0]), static_cast<std::uint32_t>(host.loads[1]),
          static_cast<std::uint32_t>(host.loads[2]),
          static_cast<std::uint32_t>(host.totalram), static_cast<std::uint32_t>(host.freeram),
          static_cast<std::uint32_t>(host.sharedram), static_cast<std::uint32_t>(host.bufferram),
          static_cast<std::uint32_t>(host.totalswap), static_cast<std::uint32_t>(host.freeswap)};
      if (at != 0 && !memory_->Write(at, words, sizeof words)) {
        return Outcome::Answered(err::kEfault, call + ": the sysinfo at 0x" + Hex(at) +
                                                   " is not writable guest memory");
      }
      return Outcome::Emulated(0, call + " -> totalram " + std::to_string(host.totalram) + "B, freeram " +
                                      std::to_string(host.freeram) + "B, procs " +
                                      std::to_string(host.procs));
    }

    case nr::kSchedGetparam:
    case nr::kSchedGetscheduler:
    case nr::kSchedGetPriorityMin:
    case nr::kSchedGetPriorityMax: {
      // The scheduler queries a Mono/Boehm GC thread makes before it parks itself: sched_getparam(pid, &param),
      // sched_getscheduler(pid), sched_get_priority_min/max(policy). All four are facts about the *host* thread
      // that is running this guest thread, and each has a real answer.
      //
      // Measured (SM-X710 2026-10-19): Cars 3 asked 155, 156, 159 and 160 in a tight loop once the signal
      // refusals stopped ending it, so these are the next wall rather than a hypothetical gap.
      const auto pid = static_cast<std::int32_t>(f.arg[0]);
      const pid_t host_pid = pid == 0 ? 0 : static_cast<pid_t>(pid);
      const std::string call = std::string{Name(f.number)} + "(" + std::to_string(pid) +
                               (f.number == nr::kSchedGetPriorityMin || f.number == nr::kSchedGetPriorityMax
                                    ? ", " + std::to_string(f.arg[1]) + ")"
                                    : ")");
      if (f.number == nr::kSchedGetparam) {
        if (memory_ == nullptr) {
          return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached to this monitor");
        }
        struct sched_param host {};
        if (::sched_getparam(host_pid, &host) != 0) {
          return Outcome::Answered(err::kEsrch, call + " = -ESRCH (-3): the host has no such thread");
        }
        // The arm32 `struct sched_param` is a single `int sched_priority`, so the copy is four bytes.
        const std::int32_t prio = host.sched_priority;
        if (f.arg[1] != 0 && !memory_->Write(f.arg[1], &prio, 4)) {
          return Outcome::Answered(err::kEfault, call + ": the sched_param at 0x" + Hex(f.arg[1]) +
                                                     " is not writable guest memory");
        }
        return Outcome::Emulated(0, call + " -> priority " + std::to_string(prio));
      }
      if (f.number == nr::kSchedGetscheduler) {
        const int policy = ::sched_getscheduler(host_pid);
        if (policy < 0) {
          return Outcome::Answered(err::kEsrch, call + " = -ESRCH (-3): the host has no such thread");
        }
        return Outcome::Emulated(static_cast<std::int32_t>(policy),
                                 call + " -> policy " + std::to_string(policy));
      }
      // The priority bounds are per policy, and the host's table is the same one a device uses.
      const int policy = static_cast<int>(f.arg[1]);
      const int bound = f.number == nr::kSchedGetPriorityMin ? ::sched_get_priority_min(policy)
                                                             : ::sched_get_priority_max(policy);
      if (bound < 0) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): no such scheduling policy");
      }
      return Outcome::Emulated(bound, call + " -> " + std::to_string(bound));
    }

    case nr::kSchedSetscheduler: {
      // sched_setscheduler(pid, policy, &param). Accepted and reported, not applied: this would change the
      // *host* thread that runs the guest, and a guest asking for SCHED_FIFO on a phone that would refuse it
      // is better told the truth than given a real-time priority the emulator then holds. The deviation is
      // named in the detail rather than hidden behind a bare 0.
      const std::string call = "sched_setscheduler(" + std::to_string(f.arg[0]) + ", " +
                               std::to_string(f.arg[1]) + ")";
      return Outcome::Emulated(0, call + " = 0: recorded, not applied — the scheduling policy of the host "
                                         "threads that run this guest is not a guest's to set, and a phone "
                                         "would refuse a real-time policy to an untrusted app anyway");
    }

    case nr::kGetpriority:
    case nr::kSetpriority: {
      // getpriority/setpriority(which, who). These are the nice-value pair a Mono GC thread reads; the host's
      // answer is the true one for the thread running the guest. The set is recorded, not applied, for the
      // same reason as sched_setscheduler.
      const auto which = static_cast<std::int32_t>(f.arg[0]);
      const auto who = static_cast<std::int32_t>(f.arg[1]);
      const std::string call = std::string{Name(f.number)} + "(" + std::to_string(which) + ", " +
                               std::to_string(who) + ")";
      if (f.number == nr::kGetpriority) {
        errno = 0;
        // A kernel returns the nice value biased by 20 so it is never negative; that bias is part of the ABI
        // and is reproduced here rather than replaced with the raw value.
        const int prio = ::getpriority(which, static_cast<id_t>(who));
        if (prio == -1 && errno != 0) {
          return Outcome::Answered(err::kEsrch, call + " = -ESRCH (-3): no such process, group or user");
        }
        return Outcome::Emulated(prio, call + " -> " + std::to_string(prio) + " (nice " +
                                          std::to_string(prio - 20) + ")");
      }
      return Outcome::Emulated(0, call + " = 0: recorded, not applied — this build does not re-nice the "
                                         "host threads that run the guest");
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

    case nr::kRtSigsuspend: {
      // rt_sigsuspend(mask, sigsetsize). The arm32 compat sigset is 8 bytes (the same rule as
      // rt_sigprocmask). It swaps the blocked mask, sleeps until a deliverable signal is queued, and
      // returns -EINTR; the handler runs at the next ABI boundary. The temporary mask must stay in place
      // while the handler runs — the kernel delivers against it — and the old mask goes back after the
      // handler's rt_sigreturn (ReturnFromSignal checks sigsuspend_restore).
      if (f.arg[1] != 8) {
        return Outcome::Answered(err::kEinval, "rt_sigsuspend(sigsetsize=" + std::to_string(f.arg[1]) +
                                                   ") = -EINVAL (-22): the arm32 compat sigset is 8 bytes");
      }
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "rt_sigsuspend: no guest memory is attached to this monitor");
      }
      std::uint64_t incoming = 0;
      if (!memory_->Read(f.arg[0], &incoming, sizeof incoming)) {
        return Outcome::Answered(err::kEfault, "rt_sigsuspend: the mask at 0x" + Hex(f.arg[0]) +
                                                   " is not readable guest memory");
      }
      // A nested sigsuspend is not Mono's shape; keep the outermost old mask rather than overwriting it.
      if (!th.sigsuspend_restore) th.sigsuspend_old_mask = th.signal_mask;
      th.sigsuspend_restore = true;
      th.signal_mask = incoming;
      const std::string call = "rt_sigsuspend(mask 0x" + Hex(static_cast<std::uint32_t>(incoming)) + ")";
      if (HasDeliverablePending(th.tid, incoming)) {
        return Outcome::Answered(err::kEintr, call + " = -EINTR (-4): a signal is already pending");
      }
      runtime::FutexTable* table = futex_;
      const std::int32_t tid = th.tid;
      // Register under the GGL the caller holds, so a tkill after the pending check above cannot miss
      // the waiter (a tkill is serialized by the same GGL).
      if (table != nullptr) table->BeginSignalWait(tid);
      Outcome o{Verdict::kBlocking, 0, call + " — sleeping until a signal", {}};
      o.block = [table, tid, call] {
        if (table != nullptr && table->WaitForSignal(tid)) {
          return Outcome::Answered(err::kEintr, call + " = -EINTR (-4): a signal is pending");
        }
        return Outcome::Answered(err::kEintr, call + " = -EINTR (-4): the process is exiting");
      };
      return o;
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
      if (old != 0 && !memory_->Write(old, &th.signal_mask, sizeof th.signal_mask)) {
        return Outcome::Answered(err::kEfault, "rt_sigprocmask: the old-set out-parameter at 0x" +
                                                   Hex(old) + " is not writable guest memory");
      }
      if (set != 0) {
        if (how == 0) {
          th.signal_mask |= incoming;
        } else if (how == 1) {
          th.signal_mask &= ~incoming;
        } else {
          th.signal_mask = incoming;
        }
      }
      return Outcome::Emulated(0, "rt_sigprocmask(how=" + std::to_string(how) + ") -> mask 0x" +
                                      Hex(th.signal_mask) +
                                      " (modelled on the calling thread; delivery reads it)");
    }

    case nr::kPrctl: {
      const std::uint32_t option = f.arg[0];
      if (option == 0x53564d41u) {  // PR_SET_VMA: names an anonymous mapping for /proc/maps; nothing to name here
        return Outcome::Emulated(0, "prctl(PR_SET_VMA) -> 0: the mapping name is not recorded");
      }
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
          th.name = name;
          return Outcome::Emulated(0, std::string{"prctl(PR_SET_NAME, \""} + name.data() +
                                      "\") recorded in the monitor; the host thread is not renamed");
        }
        if (!memory_->Write(f.arg[1], th.name.data(), th.name.size())) {
          return Outcome::Answered(err::kEfault, "prctl(PR_GET_NAME): the buffer at 0x" + Hex(f.arg[1]) +
                                                     " is not writable guest memory");
        }
        return Outcome::Emulated(0, std::string{"prctl(PR_GET_NAME) -> \""} + th.name.data() + "\"");
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
      // With host I/O on, a served descriptor holds a real host descriptor number (an open /dev/null), so a host fd
      // the guest opens later can never be given the same number.
      const std::int32_t fd = host_io_ ? ::open("/dev/null", O_RDONLY | O_CLOEXEC) : NextFd();
      if (fd < 0) return Outcome::Answered(err::kEmfile, "openat('" + path + "'): no host descriptor left to reserve");
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
          if (host_io_) ::close(fd);  // the reserved host number goes back
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
      bool from_file = false;
      if ((flags & kMapAnonymous) == 0 && fd != -1 && host_io_) {
        bool served = false;
        for (const ServedFile& s : served_) served = served || s.fd == fd;
        from_file = !served;
      }
      if ((flags & kMapAnonymous) == 0 && fd != -1 && !from_file) {
        return Outcome::Refused(err::kEnosys,
                                "mmap2 with a file descriptor (fd=" + std::to_string(fd) +
                                    ") is not implemented: this monitor serves no file mappings, and a "
                                    "zero answer would be a mapping nobody owns");
      }
      const std::uint32_t size = (len + mem::kGuestPage - 1u) & ~(mem::kGuestPage - 1u);
      const std::uint32_t at = FindFreeRun(hint, size);
      if (at == 0) {
        return Outcome::Answered(err::kEnomem,
                                 "mmap2(len=" + std::to_string(len) + ", prot=0x" + Hex(prot) +
                                     ") = -ENOMEM (-12): the guest window has no free run of " +
                                     std::to_string(size) + " bytes left");
      }
      const unsigned prot_bits = ((prot & 1u) ? mem::kRead : 0u) | ((prot & 2u) || from_file ? mem::kWrite : 0u) |
                                 ((prot & 4u) ? mem::kExec : 0u);  // a file copy is written once here
      std::string merr;
      if (!memory_->Map(at, size, prot_bits, &merr)) {
        return Outcome::Answered(err::kEnomem, "mmap2: could not map " + std::to_string(size) +
                                                   " bytes at 0x" + Hex(at) + ": " + merr);
      }
      mmap_next_ = at + size;
      if (from_file) {
        // A private copy of the file's bytes from offset pgoff * 4096 (mmap2's unit): what MAP_PRIVATE reads; a
        // MAP_SHARED writer's stores are not written back (named in the detail).
        void* host = memory_->Host(at);
        const off_t off = static_cast<off_t>(f.arg[5]) * 4096;
        const ssize_t got = host == nullptr ? -1 : ::pread(fd, host, len, off);
        if (got < 0) {
          std::string e;
          memory_->Unmap(at, size, &e);
          return Outcome::Answered(err::kEbadf, "mmap2(fd=" + std::to_string(fd) + "): reading the file failed");
        }
        return Outcome::Emulated(static_cast<std::int32_t>(at),
                                 "mmap2(fd=" + std::to_string(fd) + ", len=" + std::to_string(len) + ", off=" +
                                     std::to_string(off) + ") -> 0x" + Hex(at) + ": a private copy of " +
                                     std::to_string(got) + " file bytes" +
                                     ((flags & 0x01u) != 0 ? " (MAP_SHARED: stores are not written back)" : ""));
      }
      return Outcome::Emulated(static_cast<std::int32_t>(at),
                               "mmap2(anon, len=" + std::to_string(len) + ", prot=0x" + Hex(prot) +
                                   ") -> 0x" + Hex(at) + ": " + std::to_string(size) +
                                   " bytes of the guest window's free space; protections are recorded, "
                                   "not enforced (see mprotect's note)");
    }

    case nr::kMremap: {
      // `(old, old_size, new_size, flags, new_addr)`. bionic's `__cxa_atexit` grows its handler array
      // with it once the first page fills — Angry Birds Go registers hundreds of static destructors and
      // logged "mmap/mremap failed to allocate 8192 bytes" 136 times before this (SM-X710 2026-10-15).
      // Kernel rules: shrink in place; grow in place when the pages after are free; otherwise move,
      // but only under MREMAP_MAYMOVE. MREMAP_FIXED (a caller-chosen destination) is refused by name.
      if (memory_ == nullptr) {
        return Outcome::Refused(err::kEnosys, "mremap: no guest memory is attached to this monitor");
      }
      const std::uint32_t old_at = f.arg[0];
      const std::uint32_t old_len = f.arg[1];
      const std::uint32_t new_len = f.arg[2];
      const std::uint32_t flags = f.arg[3];
      constexpr std::uint32_t kMayMove = 1u, kFixed = 2u;
      const std::string call = "mremap(0x" + Hex(old_at) + ", " + std::to_string(old_len) + " -> " +
                               std::to_string(new_len) + ", flags=0x" + Hex(flags) + ")";
      if ((old_at & (mem::kGuestPage - 1u)) != 0 || new_len == 0 || (flags & ~(kMayMove | kFixed)) != 0 ||
          ((flags & kFixed) != 0 && (flags & kMayMove) == 0)) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22)");
      }
      if ((flags & kFixed) != 0) {
        return Outcome::Refused(err::kEnosys, call + ": MREMAP_FIXED is not implemented");
      }
      const auto pages = [](std::uint32_t n) { return (n + mem::kGuestPage - 1u) & ~(mem::kGuestPage - 1u); };
      const std::uint32_t old_size = pages(old_len);
      const std::uint32_t new_size = pages(new_len);
      if (static_cast<std::uint64_t>(old_at) + old_size > memory_->window_bytes()) {
        return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): outside the guest window");
      }
      for (std::uint32_t pg = old_at; pg < old_at + old_size; pg += mem::kGuestPage) {
        if (!memory_->IsMapped(pg)) {
          return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): 0x" + Hex(pg) + " is not mapped");
        }
      }
      const unsigned prot = memory_->ProtAt(old_at);
      std::string merr;
      if (new_size <= old_size) {
        if (new_size < old_size && !memory_->Unmap(old_at + new_size, old_size - new_size, &merr)) {
          return Outcome::Answered(err::kEfault, call + ": " + merr);
        }
        return Outcome::Emulated(static_cast<std::int32_t>(old_at), call + " -> 0x" + Hex(old_at) + " (in place)");
      }
      // Grow in place when every page after the old end is free and inside the window.
      bool tail_free = static_cast<std::uint64_t>(old_at) + new_size <= AllocLimit();
      for (std::uint64_t pg = std::uint64_t{old_at} + old_size; tail_free && pg < std::uint64_t{old_at} + new_size;
           pg += mem::kGuestPage) {
        if (memory_->IsMapped(static_cast<std::uint32_t>(pg))) tail_free = false;
      }
      if (tail_free) {
        if (!memory_->Map(old_at + old_size, new_size - old_size, prot, &merr)) {
          return Outcome::Answered(err::kEnomem, call + ": " + merr);
        }
        return Outcome::Emulated(static_cast<std::int32_t>(old_at), call + " -> 0x" + Hex(old_at) + " (grown in place)");
      }
      if ((flags & kMayMove) == 0) {
        return Outcome::Answered(err::kEnomem, call + " = -ENOMEM (-12): the pages after it are taken and "
                                                      "MREMAP_MAYMOVE was not given");
      }
      const std::uint32_t to = FindFreeRun(0, new_size);
      if (to == 0) {
        return Outcome::Answered(err::kEnomem, call + " = -ENOMEM (-12): no free run of " +
                                                   std::to_string(new_size) + " bytes left");
      }
      std::vector<std::uint8_t> bytes(old_size);
      if (!memory_->Map(to, new_size, prot | mem::kWrite, &merr) || !memory_->Read(old_at, bytes.data(), old_size) ||
          !memory_->Write(to, bytes.data(), old_size) || !memory_->Unmap(old_at, old_size, &merr) ||
          ((prot & mem::kWrite) == 0 && !memory_->Protect(to, new_size, prot, &merr))) {
        return Outcome::Answered(err::kEnomem, call + ": moving the mapping failed: " + merr);
      }
      mmap_next_ = to + new_size;
      return Outcome::Emulated(static_cast<std::int32_t>(to), call + " -> 0x" + Hex(to) + " (moved, " +
                                                                  std::to_string(old_size) + " bytes copied)");
    }

    case nr::kFutex: {
      if (futex_ != nullptr) return Futex(f, th);
      // No futex table: a bare single-threaded monitor. The two ops bionic's pthread layer actually drives
      // are answered and the rest stay refusals. WAIT's immediate path is the kernel's own (value already
      // changed -> EAGAIN); a matching value would park a device's caller, and answering 0 says out loud
      // that no scheduler here will ever wake it.
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
      // Accepted and recorded: the call's job in startup code is to install a handler, and the record is
      // what a guest-sent signal (tkill/tgkill/rt_tgsigqueueinfo) is delivered through.
      //
      // The signal number is read out of the guest's `struct sigaction` and named in the answer, because the
      // *set* of signals a guest installs handlers for is the thing signal delivery has to be built against,
      // and the deduplicated boot-report note could not answer it: it recorded only the first three handlers
      // (39/36/38) before the run had finished starting threads that install more. Measured on native 32-bit
      // Cars 3 (SM-X710, /proc/<pid>/status SigCgt): it catches 17 signals, 30 (SIGPWR) among them — so a
      // build that can only see three of them is not seeing the guest, it is seeing its own log cap.
      //
      // `handler` is captured too. Zero means SIG_DFL and one means SIG_IGN, which are not handler addresses;
      // anything else is guest code a delivery would have to enter.
      const std::int32_t sig = static_cast<std::int32_t>(f.arg[0]);
      if (sig < 1 || sig > 64) {
        return Outcome::Answered(err::kEinval, "rt_sigaction(sig=" + std::to_string(sig) +
                                                   ") = -EINVAL (-22): a signal number is 1..64");
      }
      Disposition incoming;
      bool install = false;
      if (f.arg[1] != 0) {
        if (memory_ == nullptr) {
          return Outcome::Refused(err::kEnosys, "rt_sigaction: no guest memory is attached to this monitor");
        }
        // The layout read off the guest (see Disposition in monitor.h): {handler, flags, restorer, mask}.
        std::uint32_t handler = 0, flags = 0, mask[2] = {0, 0}, restorer = 0;
        if (!memory_->Read(f.arg[1], &handler, 4) || !memory_->Read(f.arg[1] + 4, &flags, 4) ||
            !memory_->Read(f.arg[1] + 8, &restorer, 4) || !memory_->Read(f.arg[1] + 12, mask, sizeof mask)) {
          return Outcome::Answered(err::kEfault, "rt_sigaction(sig=" + std::to_string(sig) + "): the act at 0x" +
                                                     Hex(f.arg[1]) + " is not readable guest memory");
        }
        incoming = Disposition{handler, flags, restorer,
                               (static_cast<std::uint64_t>(mask[1]) << 32) | mask[0]};
        install = true;
      }
      // oldact is the disposition from *before* this call, and a query (act == NULL) must not erase the
      // handler delivery depends on. The kernel copies the new act in first, then the old act out.
      if (f.arg[2] != 0) {
        const Disposition old = disposition(sig);
        const std::uint32_t out[5] = {old.handler, old.flags, old.restorer,
                                      static_cast<std::uint32_t>(old.mask),
                                      static_cast<std::uint32_t>(old.mask >> 32)};
        if (memory_ == nullptr || !memory_->Write(f.arg[2], out, sizeof out)) {
          return Outcome::Answered(err::kEfault, "rt_sigaction(sig=" + std::to_string(sig) +
                                                     "): the oldact at 0x" + Hex(f.arg[2]) +
                                                     " is not writable guest memory");
        }
      }
      if (install) sig_dispositions_[sig] = incoming;
      const Disposition now = disposition(sig);
      const char* kind = now.handler == 0 ? "SIG_DFL" : (now.handler == 1 ? "SIG_IGN" : "a handler");
      const std::string why = "rt_sigaction(sig=" + std::to_string(sig) + ")" +
                              (install ? " = 0: recorded " : " query = 0: ") + kind + " at 0x" +
                              Hex(now.handler) + ", flags 0x" + Hex(now.flags) + ", restorer 0x" +
                              Hex(now.restorer) + " (" + std::to_string(installed_handler_count()) +
                              " handler(s) installed so far)";
      // Straight to the live trace as well as the report: this is the line that has to survive the notes'
      // dedup and cap, because it is the record signal delivery will be built from.
      if (signal_trace_) signal_trace_(why);
      Observe(why);
      return Outcome::Emulated(0, why);
    }

    // `rt_sigreturn` (173): the end of a handler. The restorer the guest registered issues it, and the
    // answer is "put back the state the handler interrupted" — which is the delivery's own record, not a
    // value the kernel hands back. A call with no delivery in progress is refused by name: answering 0
    // would resume the handler's own `svc` instruction in place, an infinite loop where the guest should
    // have had an error. Both are the same posture as everywhere else in this file — a fact, or a refusal
    // that names itself.
    case nr::kRtSigreturn: {
      if (!SignalReturnHooked() || th.deliveries.empty()) {
        return Outcome::Refused(err::kEnosys,
                                "rt_sigreturn: no signal delivery is in progress on this thread, so there "
                                "is no frame to restore (this build delivers a signal only into a handler "
                                "the guest installed with rt_sigaction)");
      }
      // The runtime performs the restore: it owns the frame layout, and the monitor sits below it. A
      // sigreturn does not return, so `r0` carries nothing back from the call — but `Complete` writes the
      // outcome's r0 unconditionally, and the interrupted r0 the frame just restored is the value the
      // resumed code must see. Carry it through rather than replacing it with 0.
      Outcome restored = SignalReturn(th);
      if (restored.verdict == Verdict::kEmulated) restored.r0 = static_cast<std::int32_t>(state.r[0]);
      return restored;
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

// ------------------------------------------------------------------------------------ threads (M2 Task 4)

bool Monitor::Now(std::int32_t clock_id, std::int64_t* nanos) const {
  return clock_ ? clock_(clock_id, nanos) : HostClock(clock_id, nanos);
}

std::optional<runtime::FutexTable::Clock::time_point> Monitor::DeadlineFor(std::int32_t clock_id,
                                                                          std::int64_t abs_nanos) const {
  std::int64_t now = 0;
  if (!Now(clock_id, &now)) return std::nullopt;
  const std::int64_t left = std::max<std::int64_t>(0, abs_nanos - now);
  return runtime::FutexTable::Clock::now() + std::chrono::nanoseconds(left);
}

void Monitor::RequestGroupExit(const std::string& why) {
  {
    std::lock_guard<std::mutex> lk(exit_mu_);
    if (group_exit_reason_.empty()) group_exit_reason_ = why;
  }
  group_exited_.store(true, std::memory_order_release);
  if (futex_ != nullptr) futex_->Shutdown();
}

std::string Monitor::group_exit_reason() const {
  std::lock_guard<std::mutex> lk(exit_mu_);
  return group_exit_reason_;
}

Outcome Monitor::Sleep(const std::string& what, std::int64_t nanos, runtime::GuestThread& th) {
  // Blocking in the GGL's sense: the sleep runs with the lock released, so other guest threads run
  // meanwhile. With a futex table it is the table's sleep, which `exit_group` or a signal for this thread
  // cuts short. `rem` is not written: a named gap in the EINTR answer rather than a made-up value.
  const std::string why = what + " (host sleep, GGL released)";
  Outcome o{Verdict::kBlocking, 0, why, {}};
  runtime::FutexTable* table = futex_;
  runtime::GuestThread* wait_thread = &th;
  const std::int32_t tid = th.tid;
  // Register before the GGL is released for the block: a tkill in that window must find the sleeper.
  if (table != nullptr) table->BeginSleep(tid);
  o.block = [nanos, why, table, tid, wait_thread] {
    const int prior_kind = wait_thread != nullptr ? wait_thread->wait_kind.load() : 0;
    const std::uint32_t prior_arg = wait_thread != nullptr ? wait_thread->wait_arg.load() : 0u;
    struct Restore {
      runtime::GuestThread* t;
      int kind;
      std::uint32_t arg;
      ~Restore() {
        if (t != nullptr) {
          t->wait_kind.store(kind);
          t->wait_arg.store(arg);
        }
      }
    } restore{wait_thread, prior_kind, prior_arg};
    if (wait_thread != nullptr) {
      wait_thread->wait_kind.store(static_cast<int>(runtime::GuestThread::WaitKind::kSleep));
      wait_thread->wait_arg.store(static_cast<std::uint32_t>(nanos));
    }
    if (table != nullptr) {
      switch (table->SleepUntil(runtime::FutexTable::Clock::now() + std::chrono::nanoseconds(nanos), tid)) {
        case runtime::FutexTable::SleepResult::kSlept: break;
        case runtime::FutexTable::SleepResult::kInterrupted:
          return Outcome::Answered(err::kEintr, why + " — cut short by a signal (EINTR; `rem` is not written)");
        case runtime::FutexTable::SleepResult::kShutdown:
          return Outcome::Answered(err::kEintr, why + " — cut short: the process is exiting");
      }
    } else {
      std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
    }
    return Outcome::Emulated(0, why + " -> 0, slept");
  };
  return o;
}

std::int32_t Monitor::NextTid() {
  if (next_tid_ == 0) next_tid_ = main_->tid + 1;
  return next_tid_++;
}

std::int32_t Monitor::AllocateThreadId() {
  const std::int32_t tid = NextTid();
  live_tids_.insert(tid);
  return tid;
}

Outcome Monitor::Clone(const Frame& f, exec::MachineState& state, runtime::GuestThread& th) {
  const std::uint32_t flags = f.arg[0];
  const std::string call = "clone(flags=0x" + Hex(flags) + ", stack=0x" + Hex(f.arg[1]) + ", ptid=0x" +
                           Hex(f.arg[2]) + ", tls=0x" + Hex(f.arg[3]) + ", ctid=0x" + Hex(f.arg[4]) + ")";
  if (flags != kBionicCloneFlags) {
    return Outcome::Refused(err::kEnosys,
                            "svc #120 (clone) with flags 0x" + Hex(flags) +
                                " is not implemented: only bionic pthread_create's 0x3d0f00 (CLONE_VM|FS|FILES|"
                                "SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_CLEARTID) is served — no "
                                "fork, vfork or other thread shape");
  }
  if (!spawner_) {
    return Outcome::Refused(err::kEnosys, call + ": no thread spawner is installed on this monitor, so it "
                                                 "cannot run a second guest thread");
  }
  if (group_exited()) {
    // The process is exiting (exit_group, a fatal thread fault, or teardown): no new thread may start, or it
    // would outlive the process it belongs to.
    return Outcome::Answered(err::kEagain, call + " = -EAGAIN (-11): the process is exiting (" + group_exit_reason() +
                                               "); no new thread starts");
  }
  if (memory_ == nullptr) return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached");
  if (f.arg[1] == 0) {
    return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): a CLONE_VM thread needs its own stack");
  }
  const std::int32_t tid = NextTid();
  // CLONE_PARENT_SETTID: the parent's word holds the tid before either thread runs again.
  if (!memory_->Write(f.arg[2], &tid, 4)) {
    return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): the parent-tid word is not writable");
  }
  auto child = std::make_unique<runtime::GuestThread>();
  // The parent's registers, as the kernel copies them: PC already past the svc, then r0 = 0 (the child's
  // view of clone's return), SP = the new stack, TP = the TLS argument (CLONE_SETTLS).
  child->state = state;
  child->state.r[0] = 0;
  child->state.r[13] = f.arg[1];
  child->state.tp = f.arg[3];
  child->stack_hi = f.arg[1];
  child->tid = tid;
  child->clear_child_tid = f.arg[4];  // CLONE_CHILD_CLEARTID
  child->signal_mask = th.signal_mask;
  child->name = th.name;
  child->executor = th.executor;
  child->owner = th.owner;
  std::uint32_t self = 0;
  if (memory_->Read(f.arg[3] + 4u, &self, 4)) child->pthread_struct = self;  // TP+4: the thread struct
  live_tids_.insert(tid);
  const std::string why = spawner_(std::move(child));
  if (!why.empty()) {
    live_tids_.erase(tid);
    return Outcome::Answered(err::kEagain, call + " = -EAGAIN (-11): " + why);
  }
  return Outcome::Emulated(tid, call + " -> tid " + std::to_string(tid) + " on a new host thread");
}

Outcome Monitor::ThreadExit(const Frame& f, runtime::GuestThread& th) {
  // CLONE_CHILD_CLEARTID: zero the word and wake every waiter on it — what pthread_join is parked on. A
  // missed wake here hangs every join forever, so it is unconditional when the slot is set.
  std::string cleared = "no clear-child-tid word";
  if (th.clear_child_tid != 0 && memory_ != nullptr) {
    const std::uint32_t zero = 0;
    const bool wrote = memory_->Write(th.clear_child_tid, &zero, 4);
    const int woke = futex_ != nullptr ? futex_->Wake(th.clear_child_tid, 0x7fff'ffff) : 0;
    cleared = "clear-child-tid 0x" + Hex(th.clear_child_tid) + (wrote ? " zeroed" : " NOT writable") + ", " +
              std::to_string(woke) + " waiter(s) woken";
  }
  live_tids_.erase(th.tid);
  return Outcome{Verdict::kThreadExit, static_cast<std::int32_t>(f.arg[0]),
                 "exit(" + std::to_string(static_cast<std::int32_t>(f.arg[0])) + ") on tid " + std::to_string(th.tid) +
                     ": " + cleared};
}

Outcome Monitor::Futex(const Frame& f, runtime::GuestThread& th) {
  // futex(uaddr, op, val, timeout|val2, uaddr2, val3). FUTEX_PRIVATE_FLAG (128) is the same table here (one
  // process); FUTEX_CLOCK_REALTIME (256) picks the clock of WAIT_BITSET's absolute timeout.
  const std::uint32_t uaddr = f.arg[0];
  const std::uint32_t op = f.arg[1] & 0x7fu;
  const bool realtime = (f.arg[1] & 0x100u) != 0;
  const std::string call = "futex(0x" + Hex(uaddr) + ", op 0x" + Hex(f.arg[1]) + ", " + std::to_string(f.arg[2]) + ")";
  if ((f.arg[1] & ~(0x7fu | 0x80u | 0x100u)) != 0) {
    return Outcome::Refused(err::kEnosys, "svc #240 (futex) op 0x" + Hex(f.arg[1]) + ": unknown flag bits");
  }
  if (op == 1u || op == 10u) {  // WAKE, WAKE_BITSET
    const std::uint32_t bitset = op == 10u ? f.arg[5] : runtime::FutexTable::kMatchAny;
    if (bitset == 0) return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): a zero bitset");
    const auto n = static_cast<std::int32_t>(f.arg[2]);
    const int woke = futex_->Wake(uaddr, n, bitset);
    return Outcome::Emulated(woke, call + " WAKE -> " + std::to_string(woke) + " woken");
  }
  if (op == 0u || op == 9u) {  // WAIT (relative timeout), WAIT_BITSET (absolute timeout)
    const std::uint32_t bitset = op == 9u ? f.arg[5] : runtime::FutexTable::kMatchAny;
    if (bitset == 0) return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): a zero bitset");
    if (realtime && op == 0u) {
      return Outcome::Refused(err::kEnosys, call + ": FUTEX_CLOCK_REALTIME with a plain WAIT (the kernel's ENOSYS)");
    }
    if (memory_ == nullptr) return Outcome::Refused(err::kEnosys, call + ": no guest memory is attached");
    std::optional<runtime::FutexTable::Clock::time_point> deadline;
    if (f.arg[3] != 0) {
      std::int32_t ts[2] = {};
      if (!memory_->Read(f.arg[3], ts, sizeof ts)) {
        return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): the timeout is not readable");
      }
      if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1'000'000'000) {
        return Outcome::Answered(err::kEinval, call + " = -EINVAL (-22): a malformed timeout");
      }
      const std::int64_t t = static_cast<std::int64_t>(ts[0]) * 1'000'000'000 + ts[1];
      if (op == 0u) {
        deadline = runtime::FutexTable::Clock::now() + std::chrono::nanoseconds(t);
      } else {
        deadline = DeadlineFor(realtime ? 0 : 1, t);
        if (!deadline) return Outcome::Refused(err::kEnosys, call + ": no clock to measure the timeout against");
      }
    }
    // Steps 1 and 2 of the wait (futex_table.h): read the word and queue the waiter, both under the GGL, so
    // a wake that follows this check cannot be missed. Step 3 — the block — runs with the GGL released.
    std::uint32_t cur = 0;
    if (!memory_->Read(uaddr, &cur, 4)) {
      return Outcome::Answered(err::kEfault, call + " = -EFAULT (-14): the word is not readable guest memory");
    }
    if (cur != f.arg[2]) {
      return Outcome::Answered(err::kEagain, call + " = -EAGAIN (-11): the word holds " + Hex(cur) +
                                                 ", not the expected value");
    }
    auto waiter = futex_->Enqueue(uaddr, bitset, th.tid);
    runtime::FutexTable* table = futex_;
    Outcome o{Verdict::kBlocking, 0, call + " WAIT", {}};
    runtime::GuestThread* wait_thread = &th;
    o.block = [table, waiter, deadline, call, wait_thread, uaddr] {
      // Diagnostics: name this wait while it blocks, and put back what the thread was doing before (a
      // host call that itself waits re-enters here).
      const int prior_kind = wait_thread->wait_kind.load();
      const std::uint32_t prior_arg = wait_thread->wait_arg.load();
      wait_thread->wait_kind.store(static_cast<int>(runtime::GuestThread::WaitKind::kFutex));
      wait_thread->wait_arg.store(uaddr);
      struct Restore {
        runtime::GuestThread* t;
        int kind;
        std::uint32_t arg;
        ~Restore() {
          t->wait_kind.store(kind);
          t->wait_arg.store(arg);
        }
      } restore{wait_thread, prior_kind, prior_arg};
      switch (table->Block(waiter, deadline)) {
        case runtime::FutexTable::WaitResult::kWoken: return Outcome::Emulated(0, call + " WAIT -> 0, woken");
        case runtime::FutexTable::WaitResult::kTimedOut:
          return Outcome::Answered(err::kEtimedout, call + " WAIT = -ETIMEDOUT (-110)");
        case runtime::FutexTable::WaitResult::kInterrupted:
          return Outcome::Answered(err::kEintr, call + " WAIT = -EINTR (-4): a signal is pending for this thread");
        case runtime::FutexTable::WaitResult::kShutdown: break;
      }
      return Outcome::Answered(err::kEintr, call + " WAIT = -EINTR (-4): the process is exiting");
    };
    return o;
  }
  return Outcome::Refused(err::kEnosys, "svc #240 (futex) op " + std::to_string(f.arg[1]) +
                                            " is not implemented; WAIT, WAKE, WAIT_BITSET and WAKE_BITSET are");
}

void Monitor::set_main_thread(runtime::GuestThread* t) {
  runtime::GuestThread* next = t != nullptr ? t : &own_main_;
  if (next == main_) return;
  // The values set so far belong to the main thread whichever object holds them.
  next->tid = main_->tid;
  next->clear_child_tid = main_->clear_child_tid;
  next->robust_head = main_->robust_head;
  next->robust_len = main_->robust_len;
  next->signal_mask = main_->signal_mask;
  next->name = main_->name;
  main_ = next;
}

Outcome Monitor::Handle(exec::MachineState& state, std::uint32_t svc_imm, runtime::GuestThread* thread) {
  Outcome o = Begin(state, svc_imm, thread);
  if (o.verdict != Verdict::kBlocking) return o;
  // Single-threaded caller: there is no lock to release, so the wait runs here.
  return Finish(state, o.block ? o.block() : Outcome::Refused(err::kEnosys, "a blocking answer with no wait"));
}

Outcome Monitor::Begin(exec::MachineState& state, std::uint32_t svc_imm, runtime::GuestThread* thread) {
  const Frame f = Frame::Capture(state, svc_imm);
  Note(f.number);
  Outcome o = Classify(f, state, thread);
  if (o.verdict == Verdict::kBlocking) return o;  // no write-back yet: the wait has not happened
  return Complete(state, std::move(o));
}

Outcome Monitor::Finish(exec::MachineState& state, Outcome done) {
  if (done.verdict == Verdict::kBlocking) {
    // A wait that answered "still waiting" is a bug in the wait, not an answer: refuse by name.
    done = Outcome::Refused(err::kEnosys, "a blocking call's wait returned another blocking verdict: " + done.detail);
  }
  return Complete(state, std::move(done));
}

Outcome Monitor::Complete(exec::MachineState& state, Outcome o) {
  const Frame f = Frame::Capture(state, 0);
  if (host_io_trace_ && (o.detail.rfind("host ", 0) == 0 || o.verdict == Verdict::kUnsupported)) host_io_trace_(o.detail);
  // Refusals and errno answers are recorded alongside the successes: the report's answer lines exist
  // so a run that died on an `openat` can say which path and why, not merely that openat happened.
  // Only four success sites called `Observe` before, so every refusal reached the report as a tally
  // entry with no reason attached — the exact silent-ignore shape the note machinery was built to
  // prevent.
  // An mmap address above 0x80000000 is negative as an int32 and a success. rt_sigreturn's r0 is the
  // *interrupted* thread's register, restored from the frame, so a negative value there is not an error the
  // monitor answered. A refused rt_sigreturn (verdict kUnsupported) still counts.
  if (LooksLikeError(o.r0) && !(o.verdict == Verdict::kEmulated && o.detail.rfind("rt_sigreturn:", 0) == 0)) {
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
