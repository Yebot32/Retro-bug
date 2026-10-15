/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::GuestThread — one guest thread's context (M2).
 *
 * Everything that differs between two guest threads of one process lives here, and nothing else does: the
 * registers (`MachineState`, which carries the thread pointer `tp` too), the stack, and the per-thread
 * kernel state the syscall monitor answers from — the tid, `set_tid_address`'s clear-child-tid slot,
 * `set_robust_list`'s head, the signal mask and the `PR_SET_NAME` name. The process-wide state (memory,
 * executors, the host-entry table, the monitor's fd table and allocator) stays where it was.
 *
 * A host thread runs guest code on the `GuestThread` it is bound to (`BindCurrentHostThread`); the binding is
 * a `thread_local` pointer (`CurrentGuestThread`). One guest thread runs on one host thread at a time:
 * binding a `GuestThread` to a second host thread takes it from the first (whose binding then reads as
 * absent).
 *
 * This header and `guest_thread.cpp` sit *below* the bridge and the monitor (library `rb_guest_thread`,
 * depending on `rb_exec` only), because both of them read a thread's fields; `GuestProcess` composes them.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

#include "rb/exec/machine_state.h"

namespace rb::exec {
class Executor;
}  // namespace rb::exec

namespace rb::runtime {

using exec::GuestAddr;

struct GuestThread {
  GuestThread() = default;
  /// Clears this host thread's binding when it points here, so a destroyed context cannot be reached
  /// through a stale pointer from the thread that destroyed it.
  ~GuestThread();
  GuestThread(const GuestThread&) = delete;
  GuestThread& operator=(const GuestThread&) = delete;

  exec::MachineState state{};
  GuestAddr stack_lo = 0, stack_hi = 0;
  std::int32_t tid = 0;
  GuestAddr clear_child_tid = 0;  ///< set_tid_address / CLONE_CHILD_CLEARTID
  GuestAddr robust_head = 0;      ///< set_robust_list
  std::uint32_t robust_len = 0;
  std::uint64_t signal_mask = 0;  ///< rt_sigprocmask (the arm32 kernel's 8-byte sigset)
  std::array<char, 16> name{};    ///< prctl(PR_SET_NAME), TASK_COMM_LEN bytes
  GuestAddr pthread_struct = 0;   ///< this thread's bionic `pthread_internal_t`, when there is one
  bool adopted = false;           ///< a host thread that entered guest code first (Task 5)

  // ---- bridge bookkeeping (not guest-visible)
  /// The tier this thread's guest calls run on, set by `AbiContext::set_executor` from the bound host
  /// thread; null = the context's default. Per thread so that `set_executor(); CallGuest()` on one host
  /// thread cannot be split by another thread's `set_executor`.
  exec::Executor* executor = nullptr;
  /// The `AbiContext` this thread runs in, fixed at its first guest call (null until then). Compared, never
  /// dereferenced.
  const void* owner = nullptr;
  /// The host thread this guest thread is bound to (default id = none).
  std::atomic<std::thread::id> host{};
};

/// The guest thread the calling host thread is bound to, or null.
GuestThread* CurrentGuestThread();

/// Binds the calling host thread to `t` (and `t` to it): guest calls made from this host thread run on `t`'s
/// registers and per-thread kernel state. Rebinding replaces the previous binding.
void BindCurrentHostThread(GuestThread& t);

/// Drops the calling host thread's binding (its `GuestThread`, if still bound here, then has no host thread).
void UnbindCurrentHostThread();

/// True when `t` is bound to the calling host thread (both directions agree).
bool BoundHere(const GuestThread& t);

}  // namespace rb::runtime
