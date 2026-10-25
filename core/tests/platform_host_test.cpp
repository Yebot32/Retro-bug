// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for `rb/platform/host.h` — the seam that lets the same core build on Linux/Android and
// Darwin. Runs on the build machine (no guest, no device, no fixtures), which is what makes it the
// first thing CI can prove about a new platform.
//
// Each check asserts a *contract the engine relies on*, not an implementation detail:
//   * a window reservation is private address space that commits nothing, and the "exactly here"
//     variant fails instead of replacing what is already mapped;
//   * an image file is seekable, readable and mappable read-only (the loader maps it);
//   * the two clock families stay in their lanes (monotonic never goes backwards);
//   * the guest-facing facts are a Linux/armv7 description, never the host's.
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rb/platform/host.h"

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

std::string Errno() { return std::strerror(errno); }

void Windows() {
  constexpr std::size_t kBytes = 4u << 20;
  void* w = rb::platform::ReserveWindow(kBytes);
  Check(w != nullptr, "ReserveWindow returns an address: " + Errno());
  if (w != nullptr) {
    // No access by construction: reading it must fault, which is not something a test can assert
    // without a signal handler. The contract checked here is that nothing is committed, i.e. the
    // mapping is accepted and can be released.
    Check(::munmap(w, kBytes) == 0, "ReserveWindow's mapping can be released");
  }

  // "Exactly here, or fail": reserve, then ask for the same range again. The second call must fail
  // and must leave the first mapping intact. The flat address model depends on exactly this.
  errno = 0;
  void* first = rb::platform::ReserveWindowAt(reinterpret_cast<void*>(0x10000000u), kBytes);
  if (first == nullptr) {
    // Some environments forbid a low fixed mapping (a hardened container, a large mmap_min_addr).
    // That is a host policy, not a seam bug: report it and skip the two dependent checks.
    std::printf("note: ReserveWindowAt(0x10000000) refused (%s); the EEXIST check is skipped\n",
                Errno().c_str());
  } else {
    errno = 0;
    void* second = rb::platform::ReserveWindowAt(first, kBytes);
    const int second_errno = errno;
    // Strict on purpose: a relocated mapping here would mean guest addresses were silently moved, which is
    // the failure mode the whole call exists to prevent (a kernel that ignores MAP_FIXED_NOREPLACE is
    // handled inside the platform, not by loosening this check).
    Check(second == nullptr,
          std::string{"ReserveWindowAt over a live mapping fails (got "} +
              (second == nullptr ? "nullptr" : second == first ? "the same address" : "another address") +
              ", errno " + std::to_string(second_errno) + ")");
    Check(second_errno == EEXIST, "…and says EEXIST (errno " + std::to_string(second_errno) + ")");
    Check(::munmap(first, kBytes) == 0, "the first mapping survived the refused second one");
  }
}

void ImageFiles() {
  std::vector<std::uint8_t> bytes(64 * 1024);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 7 + 3);

  const int fd = rb::platform::ImageFile(bytes.data(), bytes.size());
  Check(fd >= 0, "ImageFile creates a descriptor: " + Errno());
  if (fd < 0) return;

  struct stat st {};
  Check(::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) == bytes.size(),
        "the image file has the bytes' length");

  std::uint8_t head[16] = {};
  Check(::pread(fd, head, sizeof head, 0) == static_cast<ssize_t>(sizeof head) &&
            std::memcmp(head, bytes.data(), sizeof head) == 0,
        "the image file's first bytes are the image's");

  void* mapped = ::mmap(nullptr, bytes.size(), PROT_READ, MAP_PRIVATE, fd, 0);
  Check(mapped != MAP_FAILED, "the image file can be mapped read-only, as the loader does: " + Errno());
  if (mapped != MAP_FAILED) {
    Check(std::memcmp(mapped, bytes.data(), bytes.size()) == 0, "the mapping reads back as the image");
    ::munmap(mapped, bytes.size());
  }
  ::close(fd);
}

void Clocks() {
  std::int64_t mono1 = 0, mono2 = 0;
  Check(rb::platform::ClockNs(1, &mono1), "guest CLOCK_MONOTONIC is answered");
  Check(rb::platform::ClockNs(1, &mono2), "guest CLOCK_MONOTONIC is answerable twice");
  Check(mono2 >= mono1, "the monotonic clock does not go backwards");

  std::int64_t wall = 0;
  Check(rb::platform::ClockNs(0, &wall), "guest CLOCK_REALTIME is answered");
  Check(wall > 1'000'000'000'000'000'000LL, "the wall clock is after 2001 in nanoseconds");

  std::int64_t cpu = 0;
  Check(rb::platform::ClockNs(3, &cpu), "guest CLOCK_THREAD_CPUTIME_ID is answered");
  Check(cpu >= 0, "the thread CPU clock is not negative");

  std::int64_t none = 0;
  Check(!rb::platform::ClockNs(42, &none), "an id outside the guest's clock table is refused");
}

void ProcessFacts() {
  const rb::platform::Uts uts = rb::platform::LinuxUts();
  Check(std::strcmp(uts.sysname, "Linux") == 0, std::string{"uts.sysname is Linux, not "} + uts.sysname);
  Check(std::strcmp(uts.machine, "armv7l") == 0,
        std::string{"uts.machine is armv7l, not "} + uts.machine);

  rb::platform::MemInfo mem{};
  const bool have_mem = rb::platform::MemoryInfo(&mem);
  Check(have_mem, "the host can describe its memory");
  if (have_mem) Check(mem.totalram > 0, "total RAM is a positive number of bytes");

  rb::platform::Limit stack{};
  Check(rb::platform::ResourceLimit(3 /* RLIMIT_STACK, arm32 Linux numbering */, &stack),
        "RLIMIT_STACK is answered in the guest's numbering");
  Check(stack.cur > 0 || stack.cur == rb::platform::kInfinite, "the stack limit is a real value");

  Check(rb::platform::CpuCount() > 0, "the host reports at least one CPU");
  Check(rb::platform::CpuMask() != 0, "the affinity mask names at least one CPU");

  std::int32_t prio = -1;
  Check(rb::platform::SchedPriority(0, &prio), "sched_getparam is answered for the process");
  Check(prio >= 0, "the reported scheduling priority is not negative");

  std::int32_t policy = -1;
  Check(rb::platform::SchedPolicy(0, &policy), "sched_getscheduler is answered for the process");
}

}  // namespace

int main() {
  Windows();
  ImageFiles();
  Clocks();
  ProcessFacts();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
