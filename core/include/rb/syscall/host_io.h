// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host file and socket pass-through (M4 Task 3b). With `Monitor::set_host_io(true)` a guest syscall on a real path
// or a host descriptor is served by the host kernel, with the app's own permissions — exactly the access the app's
// native code has on a device. The guest's descriptor numbers *are* host descriptor numbers. ARM EABI syscall
// numbers are mapped to their AArch64 equivalents; the structures whose layout differs between the two ABIs are
// translated (stat64, statfs64, timeval, timespec, iovec, msghdr and its control messages, the 64-bit flock
// commands); the rest (sockaddr, pollfd, fd_set, epoll_event, linux_dirent64, statx) are byte-identical and pass
// as translated pointers. A guest pointer the window does not map reaches the kernel as an address it cannot read,
// so the answer is the kernel's own EFAULT.
#pragma once

#include <cstdint>

#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

namespace rb::syscall {

class HostIo {
 public:
  /// Whether `number` (ARM EABI) is one this layer serves.
  static bool Handles(std::uint32_t number);
  /// Whether `number` takes a descriptor as its first argument (so a monitor-served fd keeps its own handler).
  static bool TakesFd(std::uint32_t number);
  /// Whether `number` takes a path as argument `*index` (0 or 1), for the provider's virtual paths.
  static bool TakesPath(std::uint32_t number, unsigned* index);
  /// Serves the call. Every answer is `Verdict::kBlocking`: the host call itself runs in `block`, with the GGL
  /// released, because any descriptor may be a socket or a pipe that waits.
  static Outcome Serve(const Frame& f, mem::GuestMemory& mem);
};

}  // namespace rb::syscall
