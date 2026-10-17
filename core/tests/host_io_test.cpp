// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_host_io_tests (M4 Task 3b): with `Monitor::set_host_io(true)`, guest file and socket syscalls reach the host
// kernel. Each row builds the guest's registers and memory as a 32-bit caller leaves them at `svc #0` (ARM EABI
// numbers, 32-bit structs) and checks the guest-visible answer and the host-side effect.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

namespace {

using namespace rb;

int checks = 0, failures = 0;
void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}
#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)
#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr std::uint32_t kBuf = 0x10000;  // guest scratch: 128 KiB mapped here
const char* const kDir = "/data/local/tmp/rb_hostio";

struct Rig {
  std::unique_ptr<mem::GuestMemory> m;
  syscall::Monitor mon;
  std::uint32_t next = kBuf;
  Rig() {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    model.host_page_shift = mem::kGuestPageShift;
    while ((1L << model.host_page_shift) < sysconf(_SC_PAGESIZE)) ++model.host_page_shift;
    std::string err;
    m = mem::GuestMemory::Create(std::size_t{1} << 20, model, &err);
    m->Map(kBuf, 0x20000, mem::kRead | mem::kWrite, &err);
    mon.set_memory(m.get());
    mon.set_host_io(true);
  }
  /// Guest copy of `bytes` (or `n` zero bytes), 8-aligned.
  std::uint32_t Put(const void* bytes, std::size_t n) {
    const std::uint32_t at = next;
    next += static_cast<std::uint32_t>((n + 7) & ~std::size_t{7});
    if (bytes != nullptr) m->Write(at, bytes, n);
    return at;
  }
  std::uint32_t Str(const std::string& s) { return Put(s.c_str(), s.size() + 1); }
  std::uint32_t Zero(std::size_t n) {
    const std::uint32_t at = Put(nullptr, n);
    std::vector<std::uint8_t> z(n, 0);
    m->Write(at, z.data(), n);
    return at;
  }
  std::int32_t Sys(std::uint32_t nr, std::uint32_t a0 = 0, std::uint32_t a1 = 0, std::uint32_t a2 = 0,
                   std::uint32_t a3 = 0, std::uint32_t a4 = 0, std::uint32_t a5 = 0) {
    exec::MachineState s{};
    s.r[7] = nr;
    s.r[0] = a0;
    s.r[1] = a1;
    s.r[2] = a2;
    s.r[3] = a3;
    s.r[4] = a4;
    s.r[5] = a5;
    return mon.Handle(s, 0).r0;
  }
  std::string Read(std::uint32_t at, std::size_t n) {
    std::string out(n, '\0');
    m->Read(at, out.data(), n);
    return out;
  }
  std::uint32_t U32(std::uint32_t at) {
    std::uint32_t v = 0;
    m->Read(at, &v, 4);
    return v;
  }
};

constexpr std::uint32_t AtFdcwd = static_cast<std::uint32_t>(-100);

void files_round_trip() {
  Rig r;
  std::system("rm -rf /data/local/tmp/rb_hostio");
  CHECK_CTX(r.Sys(323 /*mkdirat*/, AtFdcwd, r.Str(kDir), 0755) == 0, "mkdirat");
  const std::string path = std::string{kDir} + "/a.txt";
  const std::int32_t fd = r.Sys(322 /*openat*/, AtFdcwd, r.Str(path), O_CREAT | O_RDWR | O_TRUNC, 0644);
  CHECK_CTX(fd >= 3, "openat -> " + std::to_string(fd));
  CHECK(r.Sys(4 /*write*/, fd, r.Str("hello world"), 11) == 11);
  const std::uint32_t res = r.Zero(8);
  CHECK(r.Sys(140 /*_llseek*/, fd, 0, 0, res, SEEK_SET) == 0 && r.U32(res) == 0);
  const std::uint32_t buf = r.Zero(16);
  CHECK(r.Sys(3 /*read*/, fd, buf, 5) == 5 && r.Read(buf, 5) == "hello");
  CHECK(r.Sys(180 /*pread64*/, fd, buf, 5, 0, 6, 0) == 5 && r.Read(buf, 5) == "world");
  const std::uint32_t st = r.Zero(104);
  CHECK_CTX(r.Sys(197 /*fstat64*/, fd, st) == 0, "fstat64");
  std::uint64_t size = 0;
  r.m->Read(st + 48, &size, 8);
  CHECK_CTX(size == 11 && S_ISREG(r.U32(st + 16)), "st_size " + std::to_string(size));
  CHECK(r.Sys(6 /*close*/, fd) == 0);
  const std::uint32_t st2 = r.Zero(104);
  CHECK(r.Sys(195 /*stat64*/, r.Str(path), st2) == 0);
  r.m->Read(st2 + 48, &size, 8);
  CHECK(size == 11);
  // getdents64 lists it
  const std::int32_t dfd = r.Sys(322, AtFdcwd, r.Str(kDir), O_RDONLY | O_DIRECTORY, 0);
  const std::uint32_t dents = r.Zero(1024);
  const std::int32_t n = r.Sys(217 /*getdents64*/, dfd, dents, 1024);
  CHECK_CTX(n > 0 && r.Read(dents, static_cast<std::size_t>(n)).find("a.txt") != std::string::npos,
            "getdents64 -> " + std::to_string(n));
  r.Sys(6, dfd);
  const std::string moved = std::string{kDir} + "/b.txt";
  CHECK(r.Sys(329 /*renameat*/, AtFdcwd, r.Str(path), AtFdcwd, r.Str(moved)) == 0);
  CHECK(r.Sys(334 /*faccessat*/, AtFdcwd, r.Str(path), F_OK) == -ENOENT);
  CHECK(r.Sys(328 /*unlinkat*/, AtFdcwd, r.Str(moved), 0) == 0);
  CHECK(::access(moved.c_str(), F_OK) != 0);
  // an unmapped guest buffer: the kernel's EFAULT
  const std::int32_t fd2 = r.Sys(322, AtFdcwd, r.Str(moved), O_CREAT | O_RDWR, 0644);
  CHECK(r.Sys(4, fd2, 0x80000, 4) == -EFAULT);
  r.Sys(6, fd2);
}

void pipes_and_messages() {
  Rig r;
  const std::uint32_t fds = r.Zero(8);
  CHECK(r.Sys(359 /*pipe2*/, fds, 0) == 0);
  const std::int32_t rd = static_cast<std::int32_t>(r.U32(fds)), wr = static_cast<std::int32_t>(r.U32(fds + 4));
  CHECK(r.Sys(4, wr, r.Str("pipe!"), 5) == 5);
  const std::uint32_t buf = r.Zero(8);
  CHECK(r.Sys(3, rd, buf, 8) == 5 && r.Read(buf, 5) == "pipe!");
  r.Sys(6, rd);
  r.Sys(6, wr);
  // sendmsg/recvmsg with two iovecs each side (32-bit msghdr and iovec)
  const std::uint32_t sv = r.Zero(8);
  CHECK(r.Sys(288 /*socketpair*/, AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  const std::uint32_t s0 = r.U32(sv), s1 = r.U32(sv + 4);
  const std::uint32_t iov_out[4] = {r.Str("ab"), 2, r.Str("cd"), 2};
  const std::uint32_t out_iov = r.Put(iov_out, 16);
  const std::uint32_t mh_out[7] = {0, 0, out_iov, 2, 0, 0, 0};
  CHECK(r.Sys(296 /*sendmsg*/, s0, r.Put(mh_out, 28), 0) == 4);
  const std::uint32_t b1 = r.Zero(8), b2 = r.Zero(8);
  const std::uint32_t iov_in[4] = {b1, 3, b2, 5};
  const std::uint32_t in_iov = r.Put(iov_in, 16);
  const std::uint32_t mh_in[7] = {0, 0, in_iov, 2, 0, 0, 0};
  const std::uint32_t mh = r.Put(mh_in, 28);
  CHECK(r.Sys(297 /*recvmsg*/, s1, mh, 0) == 4);
  CHECK_CTX(r.Read(b1, 3) == "abc" && r.Read(b2, 1) == "d", r.Read(b1, 3) + "|" + r.Read(b2, 1));
  r.Sys(6, s0);
  r.Sys(6, s1);
}

void tcp_to_a_host_listener() {
  Rig r;
  const int lis = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(lis, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  ::listen(lis, 1);
  socklen_t len = sizeof addr;
  ::getsockname(lis, reinterpret_cast<sockaddr*>(&addr), &len);
  const std::int32_t s = r.Sys(281 /*socket*/, AF_INET, SOCK_STREAM, 0);
  CHECK(s >= 3);
  CHECK_CTX(r.Sys(283 /*connect*/, s, r.Put(&addr, sizeof addr), sizeof addr) == 0, "connect");
  const int peer = ::accept(lis, nullptr, nullptr);
  CHECK(r.Sys(289 /*send*/, s, r.Str("ping"), 4, 0) == 4);
  char got[8] = {};
  CHECK(::recv(peer, got, 4, 0) == 4 && std::string(got, 4) == "ping");
  // nothing to read yet: select with a 50 ms timeval times out, and the timeval is updated (Linux)
  const std::uint32_t set = r.Zero(128);
  std::uint8_t bits[128] = {};
  bits[s / 8] |= static_cast<std::uint8_t>(1u << (s % 8));
  r.m->Write(set, bits, sizeof bits);
  const std::int32_t tv_in[2] = {0, 50000};
  const std::uint32_t tv = r.Put(tv_in, 8);
  CHECK(r.Sys(142 /*_newselect*/, static_cast<std::uint32_t>(s + 1), set, 0, 0, tv) == 0);
  ::send(peer, "pong", 4, 0);
  const std::int16_t pfd[4] = {static_cast<std::int16_t>(s), POLLIN, 0, 0};
  const std::uint32_t pf = r.Put(pfd, 8);
  CHECK(r.Sys(168 /*poll*/, pf, 1, 1000) == 1);
  const std::uint32_t buf = r.Zero(8);
  CHECK(r.Sys(291 /*recv*/, s, buf, 8, 0) == 4 && r.Read(buf, 4) == "pong");
  // SO_RCVTIMEO: a 32-bit timeval in, a 32-bit timeval (and length 8) out
  const std::int32_t to[2] = {1, 500000};
  CHECK(r.Sys(294 /*setsockopt*/, s, SOL_SOCKET, SO_RCVTIMEO, r.Put(to, 8), 8) == 0);
  const std::uint32_t back = r.Zero(8), blen = r.Put(nullptr, 4);
  const std::uint32_t eight = 8;
  r.m->Write(blen, &eight, 4);
  CHECK(r.Sys(295 /*getsockopt*/, s, SOL_SOCKET, SO_RCVTIMEO, back, blen) == 0);
  CHECK_CTX(r.U32(back) == 1 && r.U32(back + 4) == 500000 && r.U32(blen) == 8,
            std::to_string(r.U32(back)) + "." + std::to_string(r.U32(back + 4)) + " len " + std::to_string(r.U32(blen)));
  r.Sys(6, s);
  ::close(peer);
  ::close(lis);
}

void served_and_host_descriptors_never_share_a_number() {
  Rig r;
  r.mon.set_file_provider([](const std::string& p) -> std::optional<std::vector<std::uint8_t>> {
    if (p == "/virtual/cpuinfo") return std::vector<std::uint8_t>{'v', 'f', 'p'};
    return std::nullopt;
  });
  const std::int32_t served = r.Sys(322, AtFdcwd, r.Str("/virtual/cpuinfo"), O_RDONLY, 0);
  const std::int32_t host = r.Sys(322, AtFdcwd, r.Str("/proc/self/stat"), O_RDONLY, 0);
  CHECK_CTX(served >= 3 && host >= 3 && served != host, std::to_string(served) + " vs " + std::to_string(host));
  const std::uint32_t buf = r.Zero(8);
  CHECK(r.Sys(3, served, buf, 8) == 3 && r.Read(buf, 3) == "vfp");  // the provider still answers its own
  CHECK(r.Sys(6, served) == 0);
  CHECK(r.Sys(6, host) == 0);
  CHECK(r.Sys(322, AtFdcwd, r.Str("/nonexistent/x"), O_RDONLY, 0) == -ENOENT);  // the host's own answer
}

}  // namespace

int main() {
  files_round_trip();
  pipes_and_messages();
  tcp_to_a_host_listener();
  served_and_host_descriptors_never_share_a_number();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
