// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Darwin implementation of rb/platform/host.h (macOS and iOS). Every function here answers a
// *Linux/armv7 guest's* question with whatever Darwin can honestly provide; where Darwin has nothing to
// say, the call fails and the monitor refuses by name rather than inventing a plausible number.
//
// This file has not been compiled on a Mac in this change (the development host is Windows). It is
// written against the public Darwin headers and is expected to be proven by the macOS CI job; treat a
// first-run compile error here as unverified code, not as a design problem.
#include "rb/platform/host.h"

#include <fcntl.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rb::platform {
namespace {

constexpr int kWindowFlags = MAP_PRIVATE | MAP_ANONYMOUS;

std::uint64_t PageBytes() {
  const long ps = ::sysconf(_SC_PAGESIZE);
  return ps > 0 ? static_cast<std::uint64_t>(ps) : 4096u;
}

}  // namespace

void* ReserveWindow(std::size_t bytes) {
  // No MAP_NORESERVE: a PROT_NONE reservation commits nothing on Darwin either way, and the flag's
  // meaning under the iOS VM limits is one more thing not worth relying on.
  return ::mmap(nullptr, bytes, PROT_NONE, kWindowFlags, -1, 0);
}

void* ReserveWindowAt(void* hint, std::size_t bytes) {
  // Darwin has no MAP_FIXED_NOREPLACE. Ask for the hint and verify it was honoured: if the kernel put
  // the mapping elsewhere, that place is untouched and this call must fail, not silently relocate a
  // guest window (the caller's address model depends on the exact base).
  void* got = ::mmap(hint, bytes, PROT_NONE, kWindowFlags, -1, 0);
  if (got == MAP_FAILED) return nullptr;
  if (got != hint) {
    ::munmap(got, bytes);
    errno = EEXIST;
    return nullptr;
  }
  return got;
}

int ImageFile(const void* data, std::size_t bytes) {
  const char* tmp = std::getenv("TMPDIR");
  if (tmp == nullptr || tmp[0] == '\0') tmp = "/tmp";
  char path[1024];
  const int n = std::snprintf(path, sizeof path, "%s/rb-guest-XXXXXX", tmp);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  const int fd = ::mkstemp(path);
  if (fd < 0) return -1;
  ::unlink(path);  // the descriptor is the only handle; nothing is left behind on any exit path
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    return -1;
  }
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

std::int32_t CurrentTid() {
  std::uint64_t tid = 0;
  if (::pthread_threadid_np(nullptr, &tid) != 0) return 0;
  return static_cast<std::int32_t>(tid);
}

Uts LinuxUts() {
  // A deterministic Linux/ARM description. The guest asked what it is running on; "Darwin" would be
  // true of the host and false of the process it is actually in.
  Uts out{};
  std::snprintf(out.sysname, sizeof out.sysname, "Linux");
  std::snprintf(out.nodename, sizeof out.nodename, "localhost");
  std::snprintf(out.release, sizeof out.release, "4.14.0-rb");
  std::snprintf(out.version, sizeof out.version, "#1 SMP PREEMPT RetroBug");
  std::snprintf(out.machine, sizeof out.machine, "armv7l");
  return out;
}

bool MemoryInfo(MemInfo* out) {
  const std::uint64_t page = PageBytes();
  const long total_pages = ::sysconf(_SC_PHYS_PAGES);
  if (total_pages <= 0) return false;
  out->totalram = static_cast<std::uint64_t>(total_pages) * page;

  // Free memory as the VM sees it. `host_statistics64` is public API on macOS and iOS; the app's own
  // jetsam ceiling is a different number (os_proc_available_memory) and belongs to the memory budget,
  // not to what the guest is told the machine has.
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
    out->freeram = (static_cast<std::uint64_t>(vm.free_count) +
                    static_cast<std::uint64_t>(vm.speculative_count)) * page;
  }

  timespec ts{};
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0) out->uptime = static_cast<std::uint64_t>(ts.tv_sec);
  // load averages, shared/buffer RAM and swap: Darwin has no per-field equivalent worth fabricating a
  // precise number from, so they stay zero (a game sizing a cache from totalram/freeram is unaffected).
  return true;
}

bool ResourceLimit(std::int32_t guest_resource, Limit* out) {
  // The guest's numbers are Linux's asm-generic; Darwin numbers a different set. 0..4 coincide, the
  // rest are translated, and anything without an equivalent is refused as the guest's kernel would.
  int host_resource = -1;
  switch (guest_resource) {
    case 0: host_resource = RLIMIT_CPU; break;
    case 1: host_resource = RLIMIT_FSIZE; break;
    case 2: host_resource = RLIMIT_DATA; break;
    case 3: host_resource = RLIMIT_STACK; break;
    case 4: host_resource = RLIMIT_CORE; break;
    case 5: host_resource = RLIMIT_RSS; break;
    case 6: host_resource = RLIMIT_NPROC; break;
    case 7: host_resource = RLIMIT_NOFILE; break;
    case 8: host_resource = RLIMIT_MEMLOCK; break;
    case 9: host_resource = RLIMIT_AS; break;
    default: return false;
  }
  rlimit host{};
  if (::getrlimit(host_resource, &host) != 0) return false;
  out->cur = host.rlim_cur == RLIM_INFINITY ? kInfinite : static_cast<std::uint64_t>(host.rlim_cur);
  out->max = host.rlim_max == RLIM_INFINITY ? kInfinite : static_cast<std::uint64_t>(host.rlim_max);
  return true;
}

bool ClockNs(std::int32_t guest_clock_id, std::int64_t* nanos) {
  // Guest (Linux) id -> Darwin id. Darwin's MONOTONIC is Linux's BOOTTIME and Darwin's UPTIME_RAW is
  // Linux's MONOTONIC; swapping them would advance guest time while the device sleeps.
  clockid_t host;
  switch (guest_clock_id) {
    case 0: host = CLOCK_REALTIME; break;
    case 1: host = CLOCK_UPTIME_RAW; break;
    case 2: host = CLOCK_PROCESS_CPUTIME_ID; break;
    case 3: host = CLOCK_THREAD_CPUTIME_ID; break;
    case 4: host = CLOCK_MONOTONIC_RAW; break;
    case 5: host = CLOCK_REALTIME; break;  // no coarse realtime; the precise clock is a superset
    case 6: host = CLOCK_MONOTONIC; break;  // Linux BOOTTIME (counts through sleep)
    case 7: host = CLOCK_MONOTONIC; break;
    default: return false;
  }
  timespec ts{};
  if (::clock_gettime(host, &ts) != 0) return false;
  *nanos = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(ts.tv_nsec);
  return true;
}

bool SchedPriority(std::int32_t, std::int32_t* priority) {
  // Pthreads on Darwin run at SCHED_OTHER with priority 0; there is no per-thread query to disagree.
  *priority = 0;
  return true;
}

bool SchedPolicy(std::int32_t pid, std::int32_t* policy) {
  if (pid < 0) return false;
  *policy = SCHED_OTHER;
  return true;
}

bool SchedPriorityBounds(std::int32_t policy, std::int32_t* min, std::int32_t* max) {
  if (policy != SCHED_OTHER) return false;
  *min = 0;
  *max = 0;
  return true;
}

std::uint32_t CpuMask() {
  // There is no `sched_getaffinity`; the process may run on every online CPU, which is what the guest's
  // thread pool should see.
  const unsigned n = CpuCount();
  if (n == 0) return 0;
  return n >= 32 ? 0xffffffffu : ((1u << n) - 1u);
}

unsigned CpuCount() {
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<unsigned>(n) : 0;
}

}  // namespace rb::platform
