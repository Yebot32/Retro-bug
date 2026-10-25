// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The small set of host-OS facts and operations the engine cannot get from the C++ standard library.
//
// `core/` must not include Android, JNI or GL headers (core/README.md), and it must not call a system
// interface whose *existence* differs between Linux and Darwin. Everything of that kind is declared here
// and implemented once per platform under `platform/`. Keep this list to what a caller actually needs:
// every entry is a portability obligation, not an abstraction for its own sake.
//
// Deliberately absent: mmap/mprotect/munmap of a *given* address, clock_gettime, open/read/write, and
// pthreads — all four exist unchanged on Linux, Android, macOS and iOS, and wrapping them would only
// hide which of their semantics the engine relies on.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rb::platform {

// ---------------------------------------------------------------- address space

/// Reserve `bytes` of address space with no access, wherever the OS prefers. nullptr on failure
/// (errno set). The mapping is private and anonymous and commits nothing until a page is touched.
void* ReserveWindow(std::size_t bytes);

/// Reserve exactly at `hint` (page-aligned, non-null) with no access, or fail without disturbing
/// whatever is already there. nullptr with errno == EEXIST when the range is taken. This is
/// `MAP_FIXED_NOREPLACE` on Linux and "hint, then verify it was honoured" on Darwin, which has no
/// flag for it.
void* ReserveWindowAt(void* hint, std::size_t bytes);

// ---------------------------------------------------------------- files

/// A seekable file descriptor holding `bytes` copied out of `data`, positioned at 0, or -1 (errno set).
/// Linux uses an anonymous memfd; Darwin an unlinked file in TMPDIR, because `memfd_create` does not
/// exist there and an image has to be mappable as a file rather than as a writable buffer. The caller
/// closes it.
int ImageFile(const void* data, std::size_t bytes);

/// Guest (arm32 Linux) `O_*` bits to this host's. Linux's are the guest's, so it is an identity there;
/// Darwin numbers them differently and has no `O_LARGEFILE`/`O_NOATIME`/`O_DIRECT`, which are dropped
/// because the guest's intent (a 64-bit offset, no atime update) is already the only behaviour it has.
int OpenFlags(std::uint32_t guest_flags);

/// A guest `*at` directory argument in this host's numbering: the guest's `AT_FDCWD` is -100, Darwin's
/// is -2. Any real fd passes through.
int DirFd(std::int32_t guest_fd);

/// Guest `AT_*` bits (symlink handling, `AT_REMOVEDIR`, `AT_EACCESS`) to this host's.
int AtFlags(std::uint32_t guest_flags);

/// A stat result independent of the host's `struct stat`, so the guest's 104-byte `stat64` is packed from
/// one shape on every platform (Darwin names the timestamps `st_atimespec`, and its `struct statfs` has
/// no `f_frsize`/`f_namelen`).
struct StatInfo {
  std::uint64_t dev = 0;
  std::uint64_t ino = 0;
  std::uint64_t rdev = 0;
  std::uint64_t size = 0;
  std::uint64_t blocks = 0;
  std::uint32_t mode = 0;
  std::uint32_t nlink = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint32_t blksize = 0;
  std::int64_t atime_s = 0, mtime_s = 0, ctime_s = 0;
  std::uint32_t atime_ns = 0, mtime_ns = 0, ctime_ns = 0;
};
bool StatFd(int fd, StatInfo* out);
/// `dirfd` is a *guest* directory argument (translated here); `follow` false is `lstat` semantics.
bool StatAt(std::int32_t dirfd, const char* path, bool follow, StatInfo* out);

struct StatFsInfo {
  std::uint64_t type = 0;
  std::uint64_t bsize = 0;
  std::uint64_t blocks = 0;
  std::uint64_t bfree = 0;
  std::uint64_t bavail = 0;
  std::uint64_t files = 0;
  std::uint64_t ffree = 0;
  std::uint64_t fsid = 0;
  std::uint32_t namelen = 0;
  std::uint32_t frsize = 0;
  std::uint32_t flags = 0;
};
bool StatFsFd(int fd, StatFsInfo* out);
bool StatFsAt(std::int32_t dirfd, const char* path, StatFsInfo* out);

/// Directory entries in the guest's `linux_dirent64` layout, written straight into `buf` (host memory over
/// the guest window). Returns the byte count, or -1 with errno set. Darwin has no `getdents64`: it packs
/// the same records from `getdirentries`.
long GetDents(int fd, void* buf, std::size_t bytes);

/// `pipe2` and `dup3`. Darwin has neither; it has `pipe` + `fcntl` and `dup2` + `fcntl`.
int Pipe(int fds[2], std::uint32_t guest_flags);
int Dup3(int old_fd, int new_fd, std::uint32_t guest_flags);

/// An eventfd as the guest means it (a counter with poll readiness). Linux's kernel one; Darwin has no
/// equivalent and refuses with ENOSYS — named, not stubbed, because a stub that returns a pipe would give
/// the guest different semantics under the same fd.
int EventFd(std::uint32_t init, std::uint32_t guest_flags);

/// epoll. Linux's kernel implementation; Darwin refuses with ENOSYS (kqueue is not a drop-in: it has no
/// per-fd "modify" and needs a registration table to emulate EPOLL_CTL_DEL) until a title needs it.
int EpollCreate(std::uint32_t guest_flags);
int EpollCtl(int epfd, std::uint32_t op, int fd, void* guest_event);
int EpollWait(int epfd, void* guest_events, int max_events, int timeout_ms);

/// `renameat2`: Darwin's `renameat` has no flags; a non-zero flag set is refused (EINVAL), as a kernel
/// without the feature does.
int RenameAt2(std::int32_t old_dirfd, const char* old_path, std::int32_t new_dirfd, const char* new_path,
              std::uint32_t guest_flags);

/// accept4(fd, addr, addrlen, guest_flags). Darwin has no accept4; it accepts and applies the flags.
int Accept4(int fd, void* addr, void* addrlen, std::uint32_t guest_flags);

/// Fill `dst` with `len` bytes from the host's entropy pool.
bool RandomBytes(void* dst, std::size_t len);

// ---------------------------------------------------------------- process facts

/// The calling thread's kernel id (Linux `gettid`, Darwin `pthread_threadid_np`). Used to describe a
/// guest thread to the Java APIs that take a tid; it is never exposed as a guest tid.
std::int32_t CurrentTid();

/// The `utsname` a Linux/armv7 guest is entitled to see. On Linux the host answers (with `machine`
/// forced to `armv7l`); on Darwin the fields are synthesised, because "Darwin"/"arm64" would answer a
/// question the guest did not ask.
struct Uts {
  char sysname[65]{};
  char nodename[65]{};
  char release[65]{};
  char version[65]{};
  char machine[65]{};
};
Uts LinuxUts();

/// What `sysinfo(2)` exposes. `false` when the host cannot say; the monitor then refuses by name rather
/// than inventing numbers a game would size its caches from.
struct MemInfo {
  std::uint64_t uptime = 0;      ///< seconds
  std::uint32_t procs = 0;       ///< runnable processes (0 when the host has no such count)
  std::uint32_t loads[3] = {0, 0, 0};
  std::uint64_t totalram = 0;    ///< bytes
  std::uint64_t freeram = 0;
  std::uint64_t sharedram = 0;
  std::uint64_t bufferram = 0;
  std::uint64_t totalswap = 0;
  std::uint64_t freeswap = 0;
};
bool MemoryInfo(MemInfo* out);

/// A resource limit, in the guest's own numbering (`guest_resource` is the arm32 RLIMIT_* value).
struct Limit {
  std::uint64_t cur = 0;
  std::uint64_t max = 0;
};
/// `kInfinite` marks RLIM_INFINITY so the caller can clamp it to the guest's widest field.
constexpr std::uint64_t kInfinite = ~static_cast<std::uint64_t>(0);
bool ResourceLimit(std::int32_t guest_resource, Limit* out);

// ---------------------------------------------------------------- time

/// A guest clock, in nanoseconds, identified by Linux's numbering: REALTIME 0, MONOTONIC 1,
/// PROCESS_CPUTIME 2, THREAD_CPUTIME 3, MONOTONIC_RAW 4, REALTIME_COARSE 5, BOOTTIME 6, BOOTTIME_ALARM 7.
///
/// Darwin's CLOCK_MONOTONIC *counts through sleep*, which is Linux's CLOCK_BOOTTIME; Linux's
/// CLOCK_MONOTONIC does not. Passing the id straight through would make a guest's frame clock jump
/// forward by every minute the device spent asleep, so the mapping lives here.
bool ClockNs(std::int32_t guest_clock_id, std::int64_t* nanos);

// ---------------------------------------------------------------- scheduling

/// The scheduler facts a Mono/Boehm GC thread reads before it parks (Linux sched_getparam /
/// sched_getscheduler / sched_get_priority_min|max). Darwin exposes no equivalent; it answers the
/// SCHED_OTHER defaults, which is what a thread that has not asked for a policy would be given.
bool SchedPriority(std::int32_t pid, std::int32_t* priority);
bool SchedPolicy(std::int32_t pid, std::int32_t* policy);
bool SchedPriorityBounds(std::int32_t policy, std::int32_t* min, std::int32_t* max);

// ---------------------------------------------------------------- CPUs

/// The CPUs this process may run on, as the leading 32 bits of a `cpu_set_t` (the arm32 guest's shape).
/// Zero when the host cannot say.
std::uint32_t CpuMask();

/// How many CPUs the host has online (for logs and for `sched_getaffinity` counts).
unsigned CpuCount();

}  // namespace rb::platform
