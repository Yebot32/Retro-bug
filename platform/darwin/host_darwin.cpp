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

#include <dirent.h>
#include <fcntl.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
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

// ---- guest (arm32 Linux) argument translation
//
// The guest's bit values are Linux's; Darwin numbers the same concepts differently. Only the flags with a
// Darwin equivalent are carried: O_LARGEFILE (offsets are always 64-bit here), O_NOATIME, O_DIRECT,
// O_PATH and O_TMPFILE have none, and dropping them keeps the guest's *intent* (which those flags
// describe) rather than inventing behaviour.
int OpenFlags(std::uint32_t g) {
  int out = static_cast<int>(g & 3u);  // O_RDONLY/O_WRONLY/O_RDWR/O_ACCMODE
  if (g & 0x40u) out |= O_CREAT;
  if (g & 0x80u) out |= O_EXCL;
  if (g & 0x100u) out |= O_NOCTTY;
  if (g & 0x200u) out |= O_TRUNC;
  if (g & 0x400u) out |= O_APPEND;
  if (g & 0x800u) out |= O_NONBLOCK;
  if (g & 0x1000u) out |= O_DSYNC;
  if (g & 0x10000u) out |= O_DIRECTORY;
  if (g & 0x20000u) out |= O_NOFOLLOW;
  if (g & 0x80000u) out |= O_CLOEXEC;
  if ((g & 0x101000u) == 0x101000u) out |= O_SYNC;  // Linux's O_SYNC is O_DSYNC|0x100000
  return out;
}

int DirFd(std::int32_t guest_fd) {
  // Linux's AT_FDCWD is -100; Darwin's is -2, and it is the only negative value a *at call takes.
  return guest_fd == -100 ? AT_FDCWD : guest_fd;
}

int AtFlags(std::uint32_t g) {
  int out = 0;
  if (g & 0x100u) out |= AT_SYMLINK_NOFOLLOW;  // guest 0x100 -> Darwin's value
  if (g & 0x200u) out |= AT_REMOVEDIR;
#ifdef AT_SYMLINK_FOLLOW
  if (g & 0x400u) out |= AT_SYMLINK_FOLLOW;
#endif
  if (g & 0x2u) out |= AT_EACCESS;
  return out;
}

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
  // Darwin names the timestamps `*timespec`, and st_ctime is the inode-change time there too (not a
  // creation time; st_birthtimespec would be that, and the guest's stat64 has no field for it).
  out->atime_s = static_cast<std::int64_t>(st.st_atimespec.tv_sec);
  out->atime_ns = static_cast<std::uint32_t>(st.st_atimespec.tv_nsec);
  out->mtime_s = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
  out->mtime_ns = static_cast<std::uint32_t>(st.st_mtimespec.tv_nsec);
  out->ctime_s = static_cast<std::int64_t>(st.st_ctimespec.tv_sec);
  out->ctime_ns = static_cast<std::uint32_t>(st.st_ctimespec.tv_nsec);
}

void FillStatFs(const struct statfs& sf, StatFsInfo* out) {
  out->type = static_cast<std::uint64_t>(sf.f_type);
  out->bsize = static_cast<std::uint64_t>(sf.f_bsize);
  out->blocks = static_cast<std::uint64_t>(sf.f_blocks);
  out->bfree = static_cast<std::uint64_t>(sf.f_bfree);
  out->bavail = static_cast<std::uint64_t>(sf.f_bavail);
  out->files = static_cast<std::uint64_t>(sf.f_files);
  out->ffree = static_cast<std::uint64_t>(sf.f_ffree);
  out->fsid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sf.f_fsid.val[0])) << 32) |
              static_cast<std::uint32_t>(sf.f_fsid.val[1]);
  out->namelen = 255;         // NAME_MAX; Darwin has no f_namelen field
  out->frsize = static_cast<std::uint32_t>(sf.f_bsize);  // no f_frsize; the block size is the answer
  out->flags = static_cast<std::uint32_t>(sf.f_flags);
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
  if (::fstatat(DirFd(dirfd), path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) return false;
  FillStat(st, out);
  return true;
}

bool StatFsFd(int fd, StatFsInfo* out) {
  struct statfs sf {};
  if (::fstatfs(fd, &sf) != 0) return false;
  FillStatFs(sf, out);
  return true;
}

bool StatFsAt(std::int32_t dirfd, const char* path, StatFsInfo* out) {
  struct statfs sf {};
  if (dirfd != -100) {
    // statfs takes a path only; the *at form is fstatfs on an opened directory.
    const int fd = ::openat(DirFd(dirfd), path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fstatfs(fd, &sf) == 0;
    ::close(fd);
    if (!ok) return false;
    FillStatFs(sf, out);
    return true;
  }
  if (::statfs(path, &sf) != 0) return false;
  FillStatFs(sf, out);
  return true;
}

long GetDents(int fd, void* buf, std::size_t bytes) {
  // Darwin has no getdents64: read one `dirent` at a time and pack the guest's linux_dirent64 record
  // (d_ino 8, d_off 8, d_reclen 2, d_type 1, name, NUL) into the caller's buffer. The directory cursor is
  // the fd's own position, which `getdirentries` advances, so repeated calls continue where they left off.
  auto* out = static_cast<std::uint8_t*>(buf);
  std::size_t used = 0;
  char rec[sizeof(struct dirent) + 1];
  while (used + 19 + 256 <= bytes) {
    long got = ::getdirentries(fd, rec, sizeof rec, nullptr);
    if (got < 0) return -1;
    if (got == 0) break;  // end of directory
    const auto* d = reinterpret_cast<const struct dirent*>(rec);
    const std::size_t name_len = std::strlen(d->d_name);
    if (name_len == 0) continue;
    const std::size_t reclen = (19 + name_len + 1 + 7) & ~std::size_t{7};  // 8-byte aligned, as Linux packs it
    if (used + reclen > bytes) {
      // Not enough room for this record: rewind the directory cursor so the next call returns it again.
      if (::lseek(fd, -got, SEEK_CUR) < 0) return -1;
      break;
    }
    std::uint64_t ino = d->d_fileno;
    std::int64_t off = 0;  // the guest uses d_off as a cookie; its own lseek state is authoritative here
    std::uint16_t rl = static_cast<std::uint16_t>(reclen);
    std::memcpy(out + used, &ino, 8);
    std::memcpy(out + used + 8, &off, 8);
    std::memcpy(out + used + 16, &rl, 2);
    out[used + 18] = d->d_type;
    std::memcpy(out + used + 19, d->d_name, name_len + 1);
    used += reclen;
  }
  return static_cast<long>(used);
}

int Pipe(int fds[2], std::uint32_t guest_flags) {
  if (::pipe(fds) != 0) return -1;
  const int want = OpenFlags(guest_flags);
  if ((want & O_NONBLOCK) != 0) {
    for (int i = 0; i < 2; ++i) ::fcntl(fds[i], F_SETFL, ::fcntl(fds[i], F_GETFL) | O_NONBLOCK);
  }
  if ((want & O_CLOEXEC) != 0) {
    for (int i = 0; i < 2; ++i) ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
  }
  return 0;
}

int Dup3(int old_fd, int new_fd, std::uint32_t guest_flags) {
  if (old_fd == new_fd) {
    errno = EINVAL;
    return -1;
  }
  if (::dup2(old_fd, new_fd) < 0) return -1;
  if ((OpenFlags(guest_flags) & O_CLOEXEC) != 0) ::fcntl(new_fd, F_SETFD, FD_CLOEXEC);
  return new_fd;
}

int EventFd(std::uint32_t, std::uint32_t) {
  // A pipe is not an eventfd: the counter semantics (accumulating writes, EFD_SEMAPHORE) are what the
  // guest asked for, and a pipe would answer with bytes under the same fd. Refuse by name.
  errno = ENOSYS;
  return -1;
}

int EpollCreate(std::uint32_t) {
  errno = ENOSYS;
  return -1;
}

int EpollCtl(int, std::uint32_t, int, void*) {
  errno = ENOSYS;
  return -1;
}

int EpollWait(int, void*, int, int) {
  errno = ENOSYS;
  return -1;
}

int RenameAt2(std::int32_t old_dirfd, const char* old_path, std::int32_t new_dirfd, const char* new_path,
              std::uint32_t guest_flags) {
  if (guest_flags != 0) {
    errno = EINVAL;  // Darwin's renameat has no NOREPLACE/EXCHANGE; a kernel without them says EINVAL
    return -1;
  }
  return ::renameat(DirFd(old_dirfd), old_path, DirFd(new_dirfd), new_path);
}

int Accept4(int fd, void* addr, void* addrlen, std::uint32_t guest_flags) {
  const int accepted = ::accept(fd, static_cast<struct sockaddr*>(addr), static_cast<socklen_t*>(addrlen));
  if (accepted < 0) return -1;
  // The guest's Linux SOCK_* bits for the two flags accept4 can set.
  if ((guest_flags & 0x800u /*SOCK_NONBLOCK*/) != 0) {
    ::fcntl(accepted, F_SETFL, ::fcntl(accepted, F_GETFL) | O_NONBLOCK);
  }
  if ((guest_flags & 0x80000u /*SOCK_CLOEXEC*/) != 0) ::fcntl(accepted, F_SETFD, FD_CLOEXEC);
  return accepted;
}

bool RandomBytes(void* dst, std::size_t len) {
  auto* p = static_cast<std::uint8_t*>(dst);
  // getentropy is limited to 256 bytes per call; arc4random_buf has no limit and never fails.
  ::arc4random_buf(p, len);
  return true;
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
