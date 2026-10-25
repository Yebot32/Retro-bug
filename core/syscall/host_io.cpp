// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See host_io.h. Each handler reads the guest's arguments, makes the host call, and writes any translated result
// back; all of it inside `block`, which the bridge runs with the GGL released.
#include "rb/syscall/host_io.h"
#include "rb/platform/host.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace rb::syscall {

namespace {

// ---- ARM EABI numbers served here
namespace a32 {
constexpr std::uint32_t kRead = 3, kWrite = 4, kOpen = 5, kClose = 6, kCreat = 8, kLink = 9, kUnlink = 10,
                        kChdir = 12, kChmod = 15, kLseek = 19, kAccess = 33, kRename = 38, kMkdir = 39, kRmdir = 40,
                        kDup = 41, kPipe = 42, kIoctl = 54, kFcntl = 55, kDup2 = 63, kSymlink = 83, kReadlink = 85,
                        kFtruncate = 93, kFchmod = 94, kFsync = 118, kLlseek = 140, kNewselect = 142, kFlock = 143,
                        kReadv = 145, kWritev = 146, kFdatasync = 148, kPoll = 168, kPread64 = 180, kPwrite64 = 181,
                        kGetcwd = 183, kFtruncate64 = 194, kStat64 = 195, kLstat64 = 196, kFstat64 = 197,
                        kGetdents64 = 217, kFcntl64 = 221, kEpollCreate = 250, kEpollCtl = 251, kEpollWait = 252,
                        kStatfs64 = 266, kFstatfs64 = 267, kSocket = 281, kBind = 282, kConnect = 283, kListen = 284,
                        kAccept = 285, kGetsockname = 286, kGetpeername = 287, kSocketpair = 288, kSend = 289,
                        kSendto = 290, kRecv = 291, kRecvfrom = 292, kShutdown = 293, kSetsockopt = 294,
                        kGetsockopt = 295, kSendmsg = 296, kRecvmsg = 297, kOpenat = 322, kMkdirat = 323,
                        kFstatat64 = 327, kUnlinkat = 328, kRenameat = 329, kLinkat = 330, kSymlinkat = 331,
                        kReadlinkat = 332, kFchmodat = 333, kFaccessat = 334, kPselect6 = 335, kPpoll = 336,
                        kUtimensat = 348, kEventfd2 = 356, kEpollCreate1 = 357, kDup3 = 358, kPipe2 = 359,
                        kAccept4 = 366, kStatx = 397, kRenameat2 = 382, kGettimeofday = 78, kSigaltstack = 186,
                        kUgetrlimit = 191, kGetuid32 = 199, kGetgid32 = 200, kGeteuid32 = 201, kGetegid32 = 202;
}  // namespace a32

constexpr int kAtFdcwd = -100;

/// The host's -errno for a host call that returned -1, else its result.
long R(long r) { return r < 0 ? -errno : r; }

/// The host address of guest `at`; nullptr for 0 or for an address the window does not map.
void* H(mem::GuestMemory& m, std::uint32_t at) { return at == 0 ? nullptr : m.Host(at); }

/// A guest NUL-terminated path, or false (EFAULT / ENAMETOOLONG in `*err`).
bool Path(mem::GuestMemory& m, std::uint32_t at, std::string* out, long* err) {
  out->clear();
  for (unsigned i = 0; i < 4096; ++i) {
    char c = 0;
    if (!m.Read(at + i, &c, 1)) {
      *err = -EFAULT;
      return false;
    }
    if (c == '\0') return true;
    out->push_back(c);
  }
  *err = -ENAMETOOLONG;
  return false;
}

// ---- struct translation
/// arm32 `struct stat64` (104 bytes; offsets measured, see monitor.cpp's kStat*), from the host-neutral
/// shape the platform fills — so the same guest bytes are produced on Darwin, whose `struct stat` names
/// the timestamps differently.
bool PutStat64(mem::GuestMemory& m, std::uint32_t at, const platform::StatInfo& st) {
  std::uint8_t b[104] = {};
  const auto p32 = [&](unsigned o, std::uint32_t v) { std::memcpy(b + o, &v, 4); };
  const auto p64 = [&](unsigned o, std::uint64_t v) { std::memcpy(b + o, &v, 8); };
  p64(0, st.dev);
  p32(12, static_cast<std::uint32_t>(st.ino));
  p32(16, st.mode);
  p32(20, st.nlink);
  p32(24, st.uid);
  p32(28, st.gid);
  p64(32, st.rdev);
  p64(48, st.size);
  p32(56, st.blksize);
  p64(64, st.blocks);
  p32(72, static_cast<std::uint32_t>(st.atime_s));
  p32(76, st.atime_ns);
  p32(80, static_cast<std::uint32_t>(st.mtime_s));
  p32(84, st.mtime_ns);
  p32(88, static_cast<std::uint32_t>(st.ctime_s));
  p32(92, st.ctime_ns);
  p64(96, st.ino);
  return m.Write(at, b, sizeof b);
}

/// arm32 `struct statfs64` (EABI: packed to 4, 84 bytes), from the host-neutral shape the platform fills
/// (Darwin's `struct statfs` has no f_frsize/f_namelen and keeps its fsid in a two-int array).
bool PutStatfs64(mem::GuestMemory& m, std::uint32_t at, std::uint32_t size, const platform::StatFsInfo& sf) {
  std::uint8_t b[84] = {};
  const auto p32 = [&](unsigned o, std::uint32_t v) { std::memcpy(b + o, &v, 4); };
  const auto p64 = [&](unsigned o, std::uint64_t v) { std::memcpy(b + o, &v, 8); };
  p32(0, static_cast<std::uint32_t>(sf.type));
  p32(4, static_cast<std::uint32_t>(sf.bsize));
  p64(8, sf.blocks);
  p64(16, sf.bfree);
  p64(24, sf.bavail);
  p64(32, sf.files);
  p64(40, sf.ffree);
  p64(48, sf.fsid);
  p32(56, sf.namelen);
  p32(60, sf.frsize);
  p32(64, sf.flags);
  return m.Write(at, b, size < sizeof b ? size : sizeof b);
}

/// A guest `struct timeval` (two 32-bit words) at `at`, or nullptr when `at` is 0.
bool GetTimeval(mem::GuestMemory& m, std::uint32_t at, struct timeval* tv) {
  std::int32_t w[2];
  if (!m.Read(at, w, 8)) return false;
  tv->tv_sec = w[0];
  tv->tv_usec = w[1];
  return true;
}
bool PutTimeval(mem::GuestMemory& m, std::uint32_t at, const struct timeval& tv) {
  const std::int32_t w[2] = {static_cast<std::int32_t>(tv.tv_sec), static_cast<std::int32_t>(tv.tv_usec)};
  return m.Write(at, w, 8);
}
bool GetTimespec(mem::GuestMemory& m, std::uint32_t at, struct timespec* ts) {
  std::int32_t w[2];
  if (!m.Read(at, w, 8)) return false;
  ts->tv_sec = w[0];
  ts->tv_nsec = w[1];
  return true;
}

/// The guest's `struct iovec[n]` (two 32-bit words each) as host iovecs.
bool GetIovecs(mem::GuestMemory& m, std::uint32_t at, std::uint32_t n, std::vector<struct iovec>* out) {
  if (n > 1024) return false;
  out->resize(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint32_t w[2];
    if (!m.Read(at + 8 * i, w, 8)) return false;
    (*out)[i].iov_base = w[1] == 0 ? nullptr : H(m, w[0]);
    (*out)[i].iov_len = w[1];
    if (w[1] != 0 && (*out)[i].iov_base == nullptr) return false;
  }
  return true;
}

/// sendmsg/recvmsg: arm32 `struct msghdr` is 7 words; `cmsghdr` is {u32 len, i32 level, i32 type} + data, 4-aligned,
/// where the host's is {u64 len, i32, i32} + data, 8-aligned.
struct Msg {
  struct msghdr h {};
  std::vector<struct iovec> iov;
  std::vector<std::uint8_t> control;
  std::uint32_t guest_control = 0, guest_controllen = 0;
};
bool GetMsg(mem::GuestMemory& m, std::uint32_t at, Msg* msg, bool sending) {
  std::uint32_t w[7];
  if (!m.Read(at, w, sizeof w)) return false;
  msg->h.msg_name = H(m, w[0]);
  msg->h.msg_namelen = w[1];
  if (!GetIovecs(m, w[2], w[3], &msg->iov)) return false;
  msg->h.msg_iov = msg->iov.data();
  msg->h.msg_iovlen = msg->iov.size();
  msg->guest_control = w[4];
  msg->guest_controllen = w[5];
  msg->h.msg_flags = static_cast<int>(w[6]);
  if (w[4] != 0 && w[5] != 0) {
    // Host buffer: room for each guest message with the host's larger header and alignment.
    msg->control.assign(static_cast<std::size_t>(w[5]) * 2 + 64, 0);
    if (sending) {
      std::size_t out = 0;
      for (std::uint32_t off = 0; off + 12 <= w[5];) {
        std::uint32_t hdr[3];
        if (!m.Read(w[4] + off, hdr, 12) || hdr[0] < 12) break;
        const std::size_t data = hdr[0] - 12;
        auto* c = reinterpret_cast<struct cmsghdr*>(msg->control.data() + out);
        c->cmsg_len = CMSG_LEN(data);
        c->cmsg_level = static_cast<int>(hdr[1]);
        c->cmsg_type = static_cast<int>(hdr[2]);
        if (data != 0 && !m.Read(w[4] + off + 12, CMSG_DATA(c), data)) return false;
        out += CMSG_SPACE(data);
        off += (hdr[0] + 3u) & ~3u;
      }
      msg->control.resize(out);
    }
    msg->h.msg_control = msg->control.empty() ? nullptr : msg->control.data();
    msg->h.msg_controllen = msg->control.size();
  }
  return true;
}
/// recvmsg: writes back the received control messages in the guest's layout, msg_controllen, msg_namelen, flags.
void PutMsg(mem::GuestMemory& m, std::uint32_t at, const Msg& msg) {
  std::uint32_t written = 0;
  if (msg.guest_control != 0 && msg.h.msg_controllen != 0) {
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg.h); c != nullptr; c = CMSG_NXTHDR(const_cast<struct msghdr*>(&msg.h), c)) {
      const std::size_t data = c->cmsg_len - CMSG_LEN(0);
      const std::uint32_t len = static_cast<std::uint32_t>(12 + data);
      if (written + len > msg.guest_controllen) break;
      const std::uint32_t hdr[3] = {len, static_cast<std::uint32_t>(c->cmsg_level), static_cast<std::uint32_t>(c->cmsg_type)};
      m.Write(msg.guest_control + written, hdr, 12);
      if (data != 0) m.Write(msg.guest_control + written + 12, CMSG_DATA(c), data);
      written += (len + 3u) & ~3u;
    }
  }
  m.Write(at + 4, &msg.h.msg_namelen, 4);
  m.Write(at + 20, &written, 4);
  const std::int32_t flags = msg.h.msg_flags;
  m.Write(at + 24, &flags, 4);
}

bool IsTimeoutOpt(int level, int name) { return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO); }

}  // namespace

bool HostIo::Handles(std::uint32_t n) {
  switch (n) {
    case a32::kRead: case a32::kWrite: case a32::kOpen: case a32::kClose: case a32::kCreat: case a32::kLink:
    case a32::kUnlink: case a32::kChdir: case a32::kChmod: case a32::kLseek: case a32::kAccess: case a32::kRename:
    case a32::kMkdir: case a32::kRmdir: case a32::kDup: case a32::kPipe: case a32::kIoctl: case a32::kFcntl:
    case a32::kDup2: case a32::kSymlink: case a32::kReadlink: case a32::kFtruncate: case a32::kFchmod: case a32::kFsync:
    case a32::kLlseek: case a32::kNewselect: case a32::kFlock: case a32::kReadv: case a32::kWritev:
    case a32::kFdatasync: case a32::kPoll: case a32::kPread64: case a32::kPwrite64: case a32::kGetcwd:
    case a32::kFtruncate64: case a32::kStat64: case a32::kLstat64: case a32::kFstat64: case a32::kGetdents64:
    case a32::kFcntl64: case a32::kEpollCreate: case a32::kEpollCtl: case a32::kEpollWait: case a32::kStatfs64:
    case a32::kFstatfs64: case a32::kSocket: case a32::kBind: case a32::kConnect: case a32::kListen: case a32::kAccept:
    case a32::kGetsockname: case a32::kGetpeername: case a32::kSocketpair: case a32::kSend: case a32::kSendto:
    case a32::kRecv: case a32::kRecvfrom: case a32::kShutdown: case a32::kSetsockopt: case a32::kGetsockopt:
    case a32::kSendmsg: case a32::kRecvmsg: case a32::kOpenat: case a32::kMkdirat: case a32::kFstatat64:
    case a32::kUnlinkat: case a32::kRenameat: case a32::kLinkat: case a32::kSymlinkat: case a32::kReadlinkat:
    case a32::kFchmodat: case a32::kFaccessat: case a32::kPselect6: case a32::kPpoll: case a32::kUtimensat:
    case a32::kEventfd2: case a32::kEpollCreate1: case a32::kDup3: case a32::kPipe2: case a32::kAccept4:
    case a32::kStatx:
    case a32::kRenameat2: case a32::kGettimeofday: case a32::kSigaltstack: case a32::kUgetrlimit:
    case a32::kGetuid32: case a32::kGetgid32: case a32::kGeteuid32: case a32::kGetegid32:
      return true;
    default: return false;
  }
}

bool HostIo::TakesFd(std::uint32_t n) {
  switch (n) {
    case a32::kRead: case a32::kWrite: case a32::kClose: case a32::kLseek: case a32::kDup: case a32::kIoctl:
    case a32::kFcntl: case a32::kDup2: case a32::kFtruncate: case a32::kFchmod: case a32::kFsync: case a32::kLlseek:
    case a32::kFlock: case a32::kReadv: case a32::kWritev: case a32::kFdatasync: case a32::kPread64:
    case a32::kPwrite64: case a32::kFtruncate64: case a32::kFstat64: case a32::kGetdents64: case a32::kFcntl64:
    case a32::kFstatfs64: case a32::kDup3:
      return true;
    default: return false;
  }
}

bool HostIo::TakesPath(std::uint32_t n, unsigned* index) {
  switch (n) {
    case a32::kOpen: case a32::kCreat: case a32::kUnlink: case a32::kChdir: case a32::kChmod: case a32::kAccess:
    case a32::kRename: case a32::kMkdir: case a32::kRmdir: case a32::kReadlink: case a32::kStat64:
    case a32::kLstat64: case a32::kStatfs64:
      *index = 0;
      return true;
    case a32::kOpenat: case a32::kMkdirat: case a32::kFstatat64: case a32::kUnlinkat: case a32::kRenameat:
    case a32::kRenameat2:
    case a32::kReadlinkat: case a32::kFchmodat: case a32::kFaccessat: case a32::kUtimensat: case a32::kStatx:
      *index = 1;
      return true;
    default: return false;
  }
}

Outcome HostIo::Serve(const Frame& f, mem::GuestMemory& mem) {
  const Frame fr = f;
  mem::GuestMemory* const mp = &mem;
  Outcome o{Verdict::kBlocking, 0, "host: " + Describe(f.number), {}};
  o.block = [fr, mp]() -> Outcome {
    mem::GuestMemory& m = *mp;
    const std::uint32_t* a = fr.arg;
    const int fd = static_cast<int>(a[0]);
    long r = -ENOSYS;
    std::string p1, p2;
    long perr = 0;
    const auto path = [&](unsigned i, std::string* out) { return Path(m, a[i], out, &perr); };
    // A guest dirfd argument: AT_FDCWD is -100 to Linux and -2 to Darwin, and a real fd passes through.
    const auto dirfd = [&](unsigned i) { return platform::DirFd(static_cast<std::int32_t>(a[i])); };
    switch (fr.number) {
      case a32::kRead: r = R(::read(fd, H(m, a[1]), a[2])); break;
      case a32::kWrite: r = R(::write(fd, H(m, a[1]), a[2])); break;
      case a32::kClose: r = R(::close(fd)); break;
      case a32::kOpen:
        r = path(0, &p1) ? R(::openat(platform::DirFd(kAtFdcwd), p1.c_str(), platform::OpenFlags(a[1]), a[2])) : perr;
        break;
      case a32::kCreat:
        r = path(0, &p1) ? R(::openat(platform::DirFd(kAtFdcwd), p1.c_str(), platform::OpenFlags(0x40u | 1u | 0x200u), a[1])) : perr;
        break;
      case a32::kOpenat:
        r = path(1, &p1) ? R(::openat(dirfd(0), p1.c_str(), platform::OpenFlags(a[2]), a[3])) : perr;
        break;
      case a32::kLink:
        r = path(0, &p1) && path(1, &p2) ? R(::linkat(platform::DirFd(kAtFdcwd), p1.c_str(), platform::DirFd(kAtFdcwd), p2.c_str(), 0)) : perr;
        break;
      case a32::kUnlink: r = path(0, &p1) ? R(::unlinkat(platform::DirFd(kAtFdcwd), p1.c_str(), 0)) : perr; break;
      case a32::kRmdir: r = path(0, &p1) ? R(::unlinkat(platform::DirFd(kAtFdcwd), p1.c_str(), platform::AtFlags(0x200u))) : perr; break;
      case a32::kUnlinkat: r = path(1, &p1) ? R(::unlinkat(dirfd(0), p1.c_str(), platform::AtFlags(a[2]))) : perr; break;
      case a32::kChdir: r = path(0, &p1) ? R(::chdir(p1.c_str())) : perr; break;
      case a32::kChmod: r = path(0, &p1) ? R(::fchmodat(platform::DirFd(kAtFdcwd), p1.c_str(), a[1], 0)) : perr; break;
      case a32::kFchmodat: r = path(1, &p1) ? R(::fchmodat(dirfd(0), p1.c_str(), a[2], platform::AtFlags(a[3]))) : perr; break;
      case a32::kFchmod: r = R(::fchmod(fd, a[1])); break;
      case a32::kAccess: r = path(0, &p1) ? R(::faccessat(platform::DirFd(kAtFdcwd), p1.c_str(), static_cast<int>(a[1]), 0)) : perr; break;
      case a32::kFaccessat:
        r = path(1, &p1) ? R(::faccessat(dirfd(0), p1.c_str(), static_cast<int>(a[2]), platform::AtFlags(a[3]))) : perr;
        break;
      case a32::kRename:
        r = path(0, &p1) && path(1, &p2) ? R(::renameat(platform::DirFd(kAtFdcwd), p1.c_str(), platform::DirFd(kAtFdcwd), p2.c_str())) : perr;
        break;
      case a32::kRenameat:
        r = path(1, &p1) && path(3, &p2) ? R(::renameat(dirfd(0), p1.c_str(), dirfd(2), p2.c_str())) : perr;
        break;
      case a32::kLinkat:
        r = path(1, &p1) && path(3, &p2)
                ? R(::linkat(dirfd(0), p1.c_str(), dirfd(2), p2.c_str(), platform::AtFlags(a[4])))
                : perr;
        break;
      case a32::kSymlink:
        r = path(0, &p1) && path(1, &p2) ? R(::symlinkat(p1.c_str(), platform::DirFd(kAtFdcwd), p2.c_str())) : perr;
        break;
      case a32::kSymlinkat:
        r = path(0, &p1) && path(2, &p2) ? R(::symlinkat(p1.c_str(), dirfd(1), p2.c_str())) : perr;
        break;
      case a32::kMkdir: r = path(0, &p1) ? R(::mkdirat(platform::DirFd(kAtFdcwd), p1.c_str(), a[1])) : perr; break;
      case a32::kMkdirat: r = path(1, &p1) ? R(::mkdirat(dirfd(0), p1.c_str(), a[2])) : perr; break;
      case a32::kReadlink:
        r = path(0, &p1) ? R(::readlinkat(platform::DirFd(kAtFdcwd), p1.c_str(), static_cast<char*>(H(m, a[1])), a[2])) : perr;
        break;
      case a32::kReadlinkat:
        r = path(1, &p1) ? R(::readlinkat(dirfd(0), p1.c_str(), static_cast<char*>(H(m, a[2])), a[3])) : perr;
        break;
      case a32::kUtimensat: {
        // struct timespec[2] of 32-bit words; NULL = now.
        struct timespec ts[2];
        const bool have = a[2] != 0;
        if (have && (!GetTimespec(m, a[2], &ts[0]) || !GetTimespec(m, a[2] + 8, &ts[1]))) {
          r = -EFAULT;
          break;
        }
        if (a[1] == 0) {
          r = R(::futimens(dirfd(0), have ? ts : nullptr));
        } else {
          r = path(1, &p1) ? R(::utimensat(dirfd(0), p1.c_str(), have ? ts : nullptr, platform::AtFlags(a[3]))) : perr;
        }
        break;
      }
      case a32::kLseek: {
        const off_t at = ::lseek(fd, static_cast<std::int32_t>(a[1]), static_cast<int>(a[2]));
        r = at < 0 ? -errno : (at > 0x7fffffff ? -EOVERFLOW : at);
        break;
      }
      case a32::kLlseek: {  // (fd, offset_high, offset_low, loff_t* result, whence)
        const off_t want = static_cast<off_t>((static_cast<std::uint64_t>(a[1]) << 32) | a[2]);
        const off_t at = ::lseek(fd, want, static_cast<int>(a[4]));
        if (at < 0) {
          r = -errno;
        } else {
          const std::int64_t v = at;
          r = m.Write(a[3], &v, 8) ? 0 : -EFAULT;
        }
        break;
      }
      case a32::kPread64: case a32::kPwrite64: {  // (fd, buf, count, pad, off_lo, off_hi)
        const off_t off = static_cast<off_t>((static_cast<std::uint64_t>(a[5]) << 32) | a[4]);
        r = fr.number == a32::kPread64 ? R(::pread(fd, H(m, a[1]), a[2], off)) : R(::pwrite(fd, H(m, a[1]), a[2], off));
        break;
      }
      case a32::kFtruncate: r = R(::ftruncate(fd, static_cast<std::int32_t>(a[1]))); break;
      case a32::kFtruncate64:  // (fd, pad, lo, hi)
        r = R(::ftruncate(fd, static_cast<off_t>((static_cast<std::uint64_t>(a[3]) << 32) | a[2])));
        break;
      case a32::kFsync: r = R(::fsync(fd)); break;
      case a32::kFdatasync: r = R(platform::FdatasyncFd(fd)); break;
      case a32::kFlock: r = R(::flock(fd, static_cast<int>(a[1]))); break;
      case a32::kDup: r = R(::dup(fd)); break;
      case a32::kDup2: r = static_cast<int>(a[1]) == fd ? R(::fcntl(fd, F_GETFD)) >= 0 ? fd : -EBADF
                                                        : R(platform::Dup3(fd, static_cast<int>(a[1]), 0));
        break;
      case a32::kDup3: r = R(platform::Dup3(fd, static_cast<int>(a[1]), a[2])); break;
      case a32::kPipe: case a32::kPipe2: {
        int p[2];
        r = R(platform::Pipe(p, fr.number == a32::kPipe2 ? a[1] : 0));
        if (r == 0 && !m.Write(a[0], p, 8)) r = -EFAULT;
        break;
      }
      case a32::kEventfd2: r = R(platform::EventFd(a[0], a[1])); break;
      case a32::kIoctl: {
        // Only the integer-pointer requests a game uses on sockets and pipes; the rest would need per-request structs.
        const std::uint32_t req = a[1];
        if (req == 0x541B /*FIONREAD*/ || req == 0x5421 /*FIONBIO*/ || req == 0x5451 /*FIOCLEX*/ || req == 0x5450 /*FIONCLEX*/) {
          r = R(::ioctl(fd, req, H(m, a[2])));
        } else {
          r = -ENOTTY;
        }
        break;
      }
      case a32::kFcntl: case a32::kFcntl64: {
        int cmd = static_cast<int>(a[1]);
        if (cmd == 12 || cmd == 13 || cmd == 14) {  // F_GETLK64/F_SETLK64/F_SETLKW64: struct flock64 is byte-identical
          cmd = cmd == 12 ? F_GETLK : cmd == 13 ? F_SETLK : F_SETLKW;
          r = R(::fcntl(fd, cmd, H(m, a[2])));
        } else if (cmd == 5 || cmd == 6 || cmd == 7) {  // the 32-bit-offset struct flock
          r = -EINVAL;
        } else {
          r = R(::fcntl(fd, cmd, static_cast<long>(static_cast<std::int32_t>(a[2]))));
        }
        break;
      }
      case a32::kReadv: case a32::kWritev: {
        std::vector<struct iovec> iov;
        if (!GetIovecs(m, a[1], a[2], &iov)) {
          r = -EFAULT;
          break;
        }
        r = fr.number == a32::kReadv ? R(::readv(fd, iov.data(), static_cast<int>(iov.size())))
                                     : R(::writev(fd, iov.data(), static_cast<int>(iov.size())));
        break;
      }
      case a32::kGetcwd: {
        char* buf = static_cast<char*>(H(m, a[0]));
        r = buf == nullptr ? -EFAULT : (::getcwd(buf, a[1]) == nullptr ? -errno : static_cast<long>(std::strlen(buf) + 1));
        break;
      }
      case a32::kStat64: case a32::kLstat64: case a32::kFstat64: case a32::kFstatat64: {
        platform::StatInfo st {};
        bool ok = false;
        if (fr.number == a32::kFstat64) {
          ok = platform::StatFd(fd, &st);
        } else if (fr.number == a32::kFstatat64) {
          if (!path(1, &p1)) { r = perr; break; }
          ok = platform::StatAt(static_cast<std::int32_t>(a[0]), p1.c_str(),
                                (platform::AtFlags(a[3]) & AT_SYMLINK_NOFOLLOW) == 0, &st);
        } else {
          if (!path(0, &p1)) { r = perr; break; }
          ok = platform::StatAt(kAtFdcwd, p1.c_str(), fr.number != a32::kLstat64, &st);
        }
        r = ok ? 0 : -errno;
        const std::uint32_t buf = fr.number == a32::kFstatat64 ? a[2] : a[1];
        if (r == 0 && !PutStat64(m, buf, st)) r = -EFAULT;
        break;
      }
      case a32::kStatx: {  // (dirfd, path, flags, mask, struct statx*)
        if (!path(1, &p1)) { r = perr; break; }
        platform::StatInfo st {};
        if (!platform::StatAt(static_cast<std::int32_t>(a[0]), p1.c_str(),
                              (platform::AtFlags(a[2]) & AT_SYMLINK_NOFOLLOW) == 0, &st)) {
          r = -errno;
          break;
        }
        // The guest's 256-byte `struct statx`, packed from the neutral shape: Darwin has no statx at all,
        // and only the fields a game reads (mode, size, times, device) carry values.
        std::uint8_t b[256] = {};
        const auto p32 = [&](unsigned o, std::uint32_t v) { std::memcpy(b + o, &v, 4); };
        const auto p64 = [&](unsigned o, std::uint64_t v) { std::memcpy(b + o, &v, 8); };
        const auto ts_at = [&](unsigned o, std::int64_t s, std::uint32_t ns) {
          std::memcpy(b + o, &s, 8);
          std::memcpy(b + o + 8, &ns, 4);
        };
        p32(0, 0x7ffu);  // stx_mask: everything this engine can report
        p32(4, st.blksize);
        p32(16, st.nlink);
        p32(20, st.uid);
        p32(24, st.gid);
        const std::uint16_t mode = static_cast<std::uint16_t>(st.mode);
        std::memcpy(b + 28, &mode, 2);
        p64(32, st.ino);
        p64(40, st.size);
        p64(48, st.blocks);
        ts_at(64, st.atime_s, st.atime_ns);
        // 80: btime — no host-neutral field; a zeroed timestamp says "not reported", which is what a
        // file system without birth times reports too.
        ts_at(96, st.ctime_s, st.ctime_ns);
        ts_at(112, st.mtime_s, st.mtime_ns);
        // Linux encodes dev_t as (major << 20) | minor on this ABI; split both device numbers the same way.
        p32(128, static_cast<std::uint32_t>((st.rdev >> 20) & 0xfffu));
        p32(132, static_cast<std::uint32_t>(st.rdev & 0xfffffu));
        p32(136, static_cast<std::uint32_t>((st.dev >> 20) & 0xfffu));
        p32(140, static_cast<std::uint32_t>(st.dev & 0xfffffu));
        r = m.Write(a[4], b, sizeof b) ? 0 : -EFAULT;
        break;
      }
      case a32::kStatfs64: case a32::kFstatfs64: {  // (path|fd, size, buf)
        platform::StatFsInfo sf {};
        bool ok = false;
        if (fr.number == a32::kFstatfs64) {
          ok = platform::StatFsFd(fd, &sf);
        } else {
          if (!path(0, &p1)) { r = perr; break; }
          ok = platform::StatFsAt(kAtFdcwd, p1.c_str(), &sf);
        }
        r = ok ? 0 : -errno;
        if (r == 0 && !PutStatfs64(m, a[2], a[1], sf)) r = -EFAULT;
        break;
      }
      case a32::kGetdents64: r = R(platform::GetDents(fd, H(m, a[1]), a[2])); break;
      case a32::kPoll: r = R(::poll(static_cast<struct pollfd*>(H(m, a[0])), a[1], static_cast<int>(a[2]))); break;
      case a32::kPpoll: {  // (fds, n, timespec*, sigmask*, sigsetsize)
        std::int64_t ms = -1;
        if (a[2] != 0) {
          struct timespec ts {};
          if (!GetTimespec(m, a[2], &ts)) {
            r = -EFAULT;
            break;
          }
          ms = static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        }
        // A guest signal mask would have to bracket the wait atomically; the loaders pass NULL for it,
        // so the mask is not applied rather than applied in a way that could lose a signal.
        r = R(::poll(static_cast<struct pollfd*>(H(m, a[0])), a[1], static_cast<int>(ms)));
        break;
      }
      case a32::kNewselect: case a32::kPselect6: {  // fd_set is byte-identical; the timeout is not
        struct timeval tv {};
        struct timespec ts {};
        const bool timed = a[4] != 0;
        if (timed && !(fr.number == a32::kNewselect ? GetTimeval(m, a[4], &tv) : GetTimespec(m, a[4], &ts))) {
          r = -EFAULT;
          break;
        }
        auto* rd = static_cast<fd_set*>(H(m, a[1]));
        auto* wr = static_cast<fd_set*>(H(m, a[2]));
        auto* ex = static_cast<fd_set*>(H(m, a[3]));
        if (fr.number == a32::kNewselect) {
          r = R(::select(static_cast<int>(a[0]), rd, wr, ex, timed ? &tv : nullptr));
          if (timed) PutTimeval(m, a[4], tv);
        } else {
          // pselect exists on Darwin; the guest's sigmask pointer is ignored for the same reason ppoll's is.
          r = R(::pselect(static_cast<int>(a[0]), rd, wr, ex, timed ? &ts : nullptr, nullptr));
        }
        break;
      }
      case a32::kEpollCreate: r = R(platform::EpollCreate(0)); break;
      case a32::kEpollCreate1: r = R(platform::EpollCreate(a[0])); break;
      case a32::kEpollCtl:  // struct epoll_event is 16 bytes on both (no packing on ARM)
        r = R(platform::EpollCtl(fd, a[1], static_cast<int>(a[2]), H(m, a[3])));
        break;
      case a32::kEpollWait:
        r = R(platform::EpollWait(fd, H(m, a[1]), static_cast<int>(a[2]), static_cast<int>(a[3])));
        break;
      // ---- sockets (sockaddr is byte-identical)
      case a32::kSocket: r = R(::socket(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]))); break;
      case a32::kSocketpair: {
        int sv[2];
        r = R(::socketpair(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]), sv));
        if (r == 0 && !m.Write(a[3], sv, 8)) r = -EFAULT;
        break;
      }
      case a32::kBind: r = R(::bind(fd, static_cast<const struct sockaddr*>(H(m, a[1])), a[2])); break;
      case a32::kConnect: r = R(::connect(fd, static_cast<const struct sockaddr*>(H(m, a[1])), a[2])); break;
      case a32::kListen: r = R(::listen(fd, static_cast<int>(a[1]))); break;
      case a32::kAccept: case a32::kAccept4:
        r = R(platform::Accept4(fd, H(m, a[1]), H(m, a[2]),
                                fr.number == a32::kAccept4 ? a[3] : 0u));
        break;
      case a32::kGetsockname:
        r = R(::getsockname(fd, static_cast<struct sockaddr*>(H(m, a[1])), static_cast<socklen_t*>(H(m, a[2]))));
        break;
      case a32::kGetpeername:
        r = R(::getpeername(fd, static_cast<struct sockaddr*>(H(m, a[1])), static_cast<socklen_t*>(H(m, a[2]))));
        break;
      case a32::kShutdown: r = R(::shutdown(fd, static_cast<int>(a[1]))); break;
      case a32::kSend: r = R(::send(fd, H(m, a[1]), a[2], static_cast<int>(a[3]))); break;
      case a32::kRecv: r = R(::recv(fd, H(m, a[1]), a[2], static_cast<int>(a[3]))); break;
      case a32::kSendto:
        r = R(::sendto(fd, H(m, a[1]), a[2], static_cast<int>(a[3]), static_cast<const struct sockaddr*>(H(m, a[4])), a[5]));
        break;
      case a32::kRecvfrom: {
        r = R(::recvfrom(fd, H(m, a[1]), a[2], static_cast<int>(a[3]), static_cast<struct sockaddr*>(H(m, a[4])),
                         static_cast<socklen_t*>(H(m, a[5]))));
        break;
      }
      case a32::kSetsockopt: {
        const int level = static_cast<int>(a[1]), name = static_cast<int>(a[2]);
        if (IsTimeoutOpt(level, name)) {
          struct timeval tv {};
          r = GetTimeval(m, a[3], &tv) ? R(::setsockopt(fd, level, name, &tv, sizeof tv)) : -EFAULT;
        } else {
          r = R(::setsockopt(fd, level, name, H(m, a[3]), a[4]));
        }
        break;
      }
      case a32::kGetsockopt: {
        const int level = static_cast<int>(a[1]), name = static_cast<int>(a[2]);
        if (IsTimeoutOpt(level, name)) {
          struct timeval tv {};
          socklen_t len = sizeof tv;
          r = R(::getsockopt(fd, level, name, &tv, &len));
          const std::uint32_t glen = 8;
          if (r == 0 && (!PutTimeval(m, a[3], tv) || !m.Write(a[4], &glen, 4))) r = -EFAULT;
        } else {
          r = R(::getsockopt(fd, level, name, H(m, a[3]), static_cast<socklen_t*>(H(m, a[4]))));
        }
        break;
      }
      case a32::kSendmsg: case a32::kRecvmsg: {
        Msg msg;
        const bool sending = fr.number == a32::kSendmsg;
        if (!GetMsg(m, a[1], &msg, sending)) {
          r = -EFAULT;
          break;
        }
        r = sending ? R(::sendmsg(fd, &msg.h, static_cast<int>(a[2]))) : R(::recvmsg(fd, &msg.h, static_cast<int>(a[2])));
        if (!sending && r >= 0) PutMsg(m, a[1], msg);
        break;
      }
      case a32::kRenameat2:  // bionic's rename() on recent releases
        r = path(1, &p1) && path(3, &p2)
                ? R(platform::RenameAt2(static_cast<std::int32_t>(a[0]), p1.c_str(),
                                          static_cast<std::int32_t>(a[2]), p2.c_str(), a[4]))
                : perr;
        break;
      case a32::kGettimeofday: {
        struct timeval tv {};
        r = R(::gettimeofday(&tv, nullptr));
        if (r == 0 && a[0] != 0 && !PutTimeval(m, a[0], tv)) r = -EFAULT;
        const std::int32_t tz[2] = {0, 0};
        if (r == 0 && a[1] != 0 && !m.Write(a[1], tz, 8)) r = -EFAULT;
        break;
      }
      case a32::kSigaltstack: {
        // Delivery happens only at the ABI's slice boundaries, never inside a host call, so a signal is never
        // delivered on this host stack; the alternate stack is accepted and reported back as disabled.
        const std::uint32_t old[3] = {0, 2 /*SS_DISABLE*/, 0};
        r = a[1] != 0 && !m.Write(a[1], old, 12) ? -EFAULT : 0;
        break;
      }
      case a32::kUgetrlimit: {  // struct rlimit of two 32-bit words; RLIM_INFINITY and anything larger -> ~0u
        struct rlimit rl {};
        r = R(::getrlimit(static_cast<int>(a[0]), &rl));
        const auto narrow = [](rlim_t v) { return v > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(v); };
        const std::uint32_t w[2] = {narrow(rl.rlim_cur), narrow(rl.rlim_max)};
        if (r == 0 && !m.Write(a[1], w, 8)) r = -EFAULT;
        break;
      }
      case a32::kGetuid32: r = ::getuid(); break;
      case a32::kGetgid32: r = ::getgid(); break;
      case a32::kGeteuid32: r = ::geteuid(); break;
      case a32::kGetegid32: r = ::getegid(); break;
      default: r = -ENOSYS; break;
    }
    // Diagnostics only: a connect's IPv4 address, and the printable start of a short write (a netd request names
    // the host it resolves).
    std::string extra;
    if (fr.number == a32::kConnect && a[2] >= 8) {
      std::uint8_t sa[8] = {};
      if (m.Read(a[1], sa, 8) && sa[0] == AF_INET) {
        char b[48];
        std::snprintf(b, sizeof b, " [%u.%u.%u.%u:%u]", sa[4], sa[5], sa[6], sa[7], (sa[2] << 8) | sa[3]);
        extra = b;
      }
    } else if ((fr.number == a32::kWrite || fr.number == a32::kSend) && r > 96 && r < 4096) {
      std::uint8_t h[48] = {};
      if (m.Read(a[1], h, sizeof h)) {
        char b[160];
        int n = 0;
        for (unsigned i = 0; i < sizeof h; ++i) n += std::snprintf(b + n, sizeof b - n, "%02x", h[i]);
        extra = std::string{" [hex "} + b + "]";
      }
    } else if ((fr.number == a32::kWrite || fr.number == a32::kSend || fr.number == a32::kSendto) && r > 0 && r <= 96) {
      std::string text(static_cast<std::size_t>(r), '\0');
      if (m.Read(a[1], text.data(), text.size())) {
        for (char& c : text) c = (c >= 32 && c < 127) ? c : '.';
        extra = " [" + text + "]";
      }
    }
    const std::string why = "host " + Describe(fr.number) + (p1.empty() ? (TakesFd(fr.number) || fr.number == a32::kSend || fr.number == a32::kRecv || fr.number == a32::kSendto || fr.number == a32::kRecvfrom ? "(fd " + std::to_string(fd) + ")" : std::string{}) : "('" + p1 + "')") + " = " + std::to_string(r) + extra;
    return r < 0 && r >= -4095 ? Outcome::Answered(static_cast<std::uint32_t>(-r), why)
                               : Outcome::Emulated(static_cast<std::int32_t>(r), why);
  };
  return o;
}

}  // namespace rb::syscall
