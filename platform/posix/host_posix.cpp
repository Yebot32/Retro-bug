// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Linux/Android implementation of rb/platform/host.h: the code these operations always had, moved
// here so `core/` has one call shape to build against. Behaviour is deliberately unchanged — the device
// lanes and the launcher are the proof.
#include "rb/platform/host.h"

#include <sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <array>
#include <cstddef>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>

namespace rb::platform {
namespace {

/// `MAP_NORESERVE` and `MAP_FIXED_NOREPLACE` are both Linux. The first is a hint that no swap is
/// reserved for the window; the second is what makes the flat model's low reservation fail cleanly
/// instead of replacing whatever is already there.
constexpr int kWindowFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

}  // namespace

void* ReserveWindow(std::size_t bytes) {
  // mmap hands back MAP_FAILED, not nullptr. Returning it as-is once let a failed 4 GiB reservation on
  // iOS pass for a window at address 0 (the alignment then wrapped to zero) and every later mprotect
  // named a low address that was never ours.
  void* p = ::mmap(nullptr, bytes, PROT_NONE, kWindowFlags, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

void* ReserveWindowAt(void* hint, std::size_t bytes) {
  void* got = ::mmap(hint, bytes, PROT_NONE, kWindowFlags | MAP_FIXED_NOREPLACE, -1, 0);
  if (got == MAP_FAILED) return nullptr;
  // Kernels older than 4.17 ignore MAP_FIXED_NOREPLACE and treat it as a hint. Relocating a guest window
  // would put guest addresses somewhere the caller did not ask for — the one thing this call exists to
  // prevent — so a mapping anywhere but the hint is withdrawn and reported as "taken".
  if (got != hint) {
    ::munmap(got, bytes);
    errno = EEXIST;
    return nullptr;
  }
  return got;
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

// The guest is Linux, so every argument translation is the identity: these exist so the call sites read
// the same on both platforms, and so a future divergence has one place to land.
int OpenFlags(std::uint32_t guest_flags) { return static_cast<int>(guest_flags); }

int DirFd(std::int32_t guest_fd) { return guest_fd; }

int AtFlags(std::uint32_t guest_flags) { return static_cast<int>(guest_flags); }

namespace {

void FillStat(const struct stat& st, StatInfo* out) {
  out->dev = static_cast<std::uint64_t>(st.st_dev);
  out->ino = static_cast<std::uint64_t>(st.st_ino);
  out->rdev = static_cast<std::uint64_t>(st.st_rdev);
  out->size = static_cast<std::uint64_t>(st.st_size);
  out->blocks = static_cast<std::uint64_t>(st.st_blocks);
  out->mode = static_cast<std::uint32_t>(st.st_mode);
  out->nlink = static_cast<std::uint32_t>(st.st_nlink);
  out->uid = static_cast<std::uint32_t>(st.st_uid);
  out->gid = static_cast<std::uint32_t>(st.st_gid);
  out->blksize = static_cast<std::uint32_t>(st.st_blksize);
  out->atime_s = static_cast<std::int64_t>(st.st_atim.tv_sec);
  out->atime_ns = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
  out->mtime_s = static_cast<std::int64_t>(st.st_mtim.tv_sec);
  out->mtime_ns = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
  out->ctime_s = static_cast<std::int64_t>(st.st_ctim.tv_sec);
  out->ctime_ns = static_cast<std::uint32_t>(st.st_ctim.tv_nsec);
}

}  // namespace

bool StatFd(int fd, StatInfo* out) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) return false;
  FillStat(st, out);
  return true;
}

bool StatAt(std::int32_t dirfd, const char* path, bool follow, StatInfo* out) {
  struct stat st {};
  const int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
  if (::fstatat(dirfd, path, &st, flags) != 0) return false;
  FillStat(st, out);
  return true;
}

namespace {

void FillStatFs(const struct statfs& sf, StatFsInfo* out) {
  out->type = static_cast<std::uint64_t>(sf.f_type);
  out->bsize = static_cast<std::uint64_t>(sf.f_bsize);
  out->blocks = static_cast<std::uint64_t>(sf.f_blocks);
  out->bfree = static_cast<std::uint64_t>(sf.f_bfree);
  out->bavail = static_cast<std::uint64_t>(sf.f_bavail);
  out->files = static_cast<std::uint64_t>(sf.f_files);
  out->ffree = static_cast<std::uint64_t>(sf.f_ffree);
  // Android spells fsid as __fsid_t { int __val[2] }; Darwin spells it fsid_t { int val[2] }.
  out->fsid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sf.f_fsid.__val[0])) << 32) |
              static_cast<std::uint32_t>(sf.f_fsid.__val[1]);
  out->namelen = static_cast<std::uint32_t>(sf.f_namelen);
  out->frsize = static_cast<std::uint32_t>(sf.f_frsize);
  out->flags = static_cast<std::uint32_t>(sf.f_flags);
}

}  // namespace

bool StatFsFd(int fd, StatFsInfo* out) {
  struct statfs sf {};
  if (::fstatfs(fd, &sf) != 0) return false;
  FillStatFs(sf, out);
  return true;
}

bool StatFsAt(std::int32_t dirfd, const char* path, StatFsInfo* out) {
  struct statfs sf {};
  if (::statfs(path, &sf) != 0) return false;
  FillStatFs(sf, out);
  return true;
}

long GetDents(int fd, void* buf, std::size_t bytes) {
  return ::syscall(SYS_getdents64, fd, buf, bytes);
}

int Pipe(int fds[2], std::uint32_t guest_flags) {
  // Linux's pipe2 takes the same O_* bits, and this is Linux.
  return ::pipe2(fds, static_cast<int>(guest_flags)) == 0 ? 0 : -1;
}

int Dup3(int old_fd, int new_fd, std::uint32_t guest_flags) {
  return ::dup3(old_fd, new_fd, static_cast<int>(guest_flags));
}

int EventFd(std::uint32_t init, std::uint32_t guest_flags) {
  return static_cast<int>(::syscall(SYS_eventfd2, init, guest_flags));
}

int EpollCreate(std::uint32_t guest_flags) { return ::epoll_create1(static_cast<int>(guest_flags)); }

int EpollCtl(int epfd, std::uint32_t op, int fd, void* guest_event) {
  return ::epoll_ctl(epfd, static_cast<int>(op), fd, static_cast<struct epoll_event*>(guest_event));
}

int EpollWait(int epfd, void* guest_events, int max_events, int timeout_ms) {
  return ::epoll_wait(epfd, static_cast<struct epoll_event*>(guest_events), max_events, timeout_ms);
}

int RenameAt2(std::int32_t old_dirfd, const char* old_path, std::int32_t new_dirfd, const char* new_path,
              std::uint32_t guest_flags) {
  return static_cast<int>(
      ::syscall(SYS_renameat2, old_dirfd, old_path, new_dirfd, new_path, guest_flags));
}

int FdatasyncFd(int fd) { return ::fdatasync(fd); }

int Accept4(int fd, void* addr, void* addrlen, std::uint32_t guest_flags) {
  // Through syscall() because bionic declares accept4() only with _GNU_SOURCE visible, and this file
  // does not restate the libc's feature-test macros.
  return static_cast<int>(::syscall(SYS_accept4, fd, addr, addrlen, guest_flags));
}

bool RandomBytes(void* dst, std::size_t len) {
  std::random_device device;  // bionic: /dev/urandom; no getentropy below API 28
  auto* p = static_cast<std::uint8_t*>(dst);
  for (std::size_t i = 0; i < len;) {
    const std::uint32_t word = device();
    for (unsigned b = 0; b < 4 && i < len; ++b, ++i) p[i] = static_cast<std::uint8_t>((word >> (8 * b)) & 0xffu);
  }
  return true;
}

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
  // The guest's numbers are asm-generic (0..15); the host's constants carry the type its prlimit
  // wants (an enum on glibc, an int on bionic), so the table is typed by deduction rather than a cast.
  static const auto kResources = std::array{RLIMIT_CPU,   RLIMIT_FSIZE, RLIMIT_DATA,  RLIMIT_STACK,
                                            RLIMIT_CORE,  RLIMIT_RSS,   RLIMIT_NPROC, RLIMIT_NOFILE,
                                            RLIMIT_MEMLOCK, RLIMIT_AS,  RLIMIT_LOCKS, RLIMIT_SIGPENDING,
                                            RLIMIT_MSGQUEUE, RLIMIT_NICE, RLIMIT_RTPRIO, RLIMIT_RTTIME};
  if (guest_resource < 0 || static_cast<std::size_t>(guest_resource) >= kResources.size()) return false;
  if (::prlimit(0, kResources[static_cast<std::size_t>(guest_resource)], nullptr, &host) != 0) return false;
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
