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
