// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Linux/Android implementation of rb/platform/host.h: the code these operations always had, moved
// here so `core/` has one call shape to build against. Behaviour is deliberately unchanged — the device
// lanes and the launcher are the proof.
#include "rb/platform/host.h"

#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace rb::platform {
namespace {

/// `MAP_NORESERVE` and `MAP_FIXED_NOREPLACE` are both Linux. The first is a hint that no swap is
/// reserved for the window; the second is what makes the flat model's low reservation fail cleanly
/// instead of replacing whatever is already there.
constexpr int kWindowFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

}  // namespace

void* ReserveWindow(std::size_t bytes) {
  return ::mmap(nullptr, bytes, PROT_NONE, kWindowFlags, -1, 0);
}

void* ReserveWindowAt(void* hint, std::size_t bytes) {
  return ::mmap(hint, bytes, PROT_NONE, kWindowFlags | MAP_FIXED_NOREPLACE, -1, 0);
}

int ImageFile(const void* data, std::size_t bytes) {
  // Called through syscall() because the memfd_create() wrapper is only declared from API 30 and the
  // engine builds against android-26 (the same reason the loader had).
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(::syscall(SYS_memfd_create, "rb-guest-elf", MFD_CLOEXEC));
#endif
  if (fd < 0) return -1;
  std::size_t off = 0;
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (off < bytes) {
    const ssize_t w = ::write(fd, p + off, bytes - off);
    if (w <= 0) {
      ::close(fd);
      return -1;
    }
    off += static_cast<std::size_t>(w);
  }
  if (::lseek(fd, 0, SEEK_SET) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::int32_t CurrentTid() { return static_cast<std::int32_t>(::gettid()); }

Uts LinuxUts() {
  Uts out{};
  struct utsname u {};
  if (::uname(&u) == 0) {
    std::snprintf(out.sysname, sizeof out.sysname, "%s", u.sysname);
    std::snprintf(out.nodename, sizeof out.nodename, "%s", u.nodename);
    std::snprintf(out.release, sizeof out.release, "%s", u.release);
    std::snprintf(out.version, sizeof out.version, "%s", u.version);
  }
  // This guest is ARM32 and asked what it is running on; the host's "aarch64" would answer a question
  // nobody put (the same correction the monitor made before this moved).
  std::snprintf(out.machine, sizeof out.machine, "armv7l");
  return out;
}

bool MemoryInfo(MemInfo* out) {
  struct ::sysinfo host {};
  if (::sysinfo(&host) != 0) return false;
  // `sysinfo`'s unit is mem_unit bytes per field, not bytes.
  const auto unit = static_cast<std::uint64_t>(host.mem_unit == 0 ? 1 : host.mem_unit);
  out->uptime = static_cast<std::uint64_t>(host.uptime);
  out->procs = static_cast<std::uint32_t>(host.procs);
  out->loads[0] = static_cast<std::uint32_t>(host.loads[0]);
  out->loads[1] = static_cast<std::uint32_t>(host.loads[1]);
  out->loads[2] = static_cast<std::uint32_t>(host.loads[2]);
  out->totalram = static_cast<std::uint64_t>(host.totalram) * unit;
  out->freeram = static_cast<std::uint64_t>(host.freeram) * unit;
  out->sharedram = static_cast<std::uint64_t>(host.sharedram) * unit;
  out->bufferram = static_cast<std::uint64_t>(host.bufferram) * unit;
  out->totalswap = static_cast<std::uint64_t>(host.totalswap) * unit;
  out->freeswap = static_cast<std::uint64_t>(host.freeswap) * unit;
  return true;
}

bool ResourceLimit(std::int32_t guest_resource, Limit* out) {
  // The guest's numbers are this kernel's numbers (both asm-generic), so the value passes straight
  // through. pid 0 is the calling process, which is the only process the guest has.
  rlimit host{};
  if (::prlimit(0, static_cast<int>(guest_resource), nullptr, &host) != 0) return false;
  out->cur = host.rlim_cur == RLIM_INFINITY ? kInfinite : static_cast<std::uint64_t>(host.rlim_cur);
  out->max = host.rlim_max == RLIM_INFINITY ? kInfinite : static_cast<std::uint64_t>(host.rlim_max);
  return true;
}

bool ClockNs(std::int32_t guest_clock_id, std::int64_t* nanos) {
  // Linux's numbering is this kernel's numbering, so the id passes straight through.
  if (guest_clock_id < 0 || guest_clock_id > 7) return false;
  timespec ts{};
  if (::clock_gettime(static_cast<clockid_t>(guest_clock_id), &ts) != 0) return false;
  *nanos = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(ts.tv_nsec);
  return true;
}

bool SchedPriority(std::int32_t pid, std::int32_t* priority) {
  sched_param host{};
  if (::sched_getparam(pid == 0 ? 0 : static_cast<pid_t>(pid), &host) != 0) return false;
  *priority = host.sched_priority;
  return true;
}

bool SchedPolicy(std::int32_t pid, std::int32_t* policy) {
  const int got = ::sched_getscheduler(pid == 0 ? 0 : static_cast<pid_t>(pid));
  if (got < 0) return false;
  *policy = got;
  return true;
}

bool SchedPriorityBounds(std::int32_t policy, std::int32_t* min, std::int32_t* max) {
  const int lo = ::sched_get_priority_min(policy);
  const int hi = ::sched_get_priority_max(policy);
  if (lo < 0 || hi < 0) return false;
  *min = lo;
  *max = hi;
  return true;
}

std::uint32_t CpuMask() {
  cpu_set_t host;
  CPU_ZERO(&host);
  if (::sched_getaffinity(0, sizeof host, &host) != 0) return 0;
  // The arm32 cpu_set_t is 1024 bits with the same bit-per-CPU layout; the leading word is the answer
  // the guest is given, and the rest zero-filled by the caller.
  std::uint32_t bits = 0;
  std::memcpy(&bits, &host, sizeof bits);
  return bits;
}

unsigned CpuCount() {
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<unsigned>(n) : 0;
}

}  // namespace rb::platform
