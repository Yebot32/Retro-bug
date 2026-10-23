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
 * A host thread runs guest code on the `GuestThread` it is bound to (`BindCurrentHostThread`; read back with
 * `CurrentGuestThread`). One guest thread runs on one host thread at a time, and a binding is never stolen:
 * binding a `GuestThread` that another live host thread holds is refused (M2 Task 5 ruling b). A binding ends
 * when its host thread rebinds or unbinds, when its host thread exits (a host `pthread_key` destructor releases
 * it, so a dead host thread never blocks a later binding — ruling d), or when the `GuestThread` is destroyed
 * (the shared binding cell is cleared, so no host thread can reach a destroyed context — ruling c).
 *
 * This header and `guest_thread.cpp` sit *below* the bridge and the monitor (library `rb_guest_thread`,
 * depending on `rb_exec` only), because both of them read a thread's fields; `GuestProcess` composes them.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "rb/exec/machine_state.h"

namespace rb::exec {
class Executor;
}  // namespace rb::exec

namespace rb::runtime {

using exec::GuestAddr;
struct HostBinding;

/// A signal delivery in progress on a guest thread (C26). Set when the runtime enters a handler the guest
/// installed, and cleared by the `rt_sigreturn` that ends it — so the restorer's call knows which frame to
/// put back. Per-thread because the kernel's is: two threads can be in two handlers at once.
///
/// `frame` is the `ucontext_t` the delivery wrote and is the authority for the live restore, because a
/// handler is allowed to edit the ucontext it was handed (and the kernel honours that). The mask to
/// restore is read back out of the frame for the same reason.
struct SignalDeliveryState {
  bool active = false;
  int sig = 0;
  GuestAddr frame = 0;   ///< the `ucontext_t` address; 0 = no delivery in progress
  GuestAddr handler = 0; ///< the address entered
  GuestAddr restorer = 0;  ///< where the handler returns to
  std::uint64_t saved_mask = 0;  ///< the blocked mask from before the handler, for a frame that cannot be read back
  /// The VFP state the handler must not be allowed to destroy. The integer sigcontext is in the frame; the
  /// kernel's VFP block is not modelled there yet, so the interrupted FP file is kept here and put back on
  /// `rt_sigreturn` (a handler that edits the kernel's FP sigcontext is a named gap, not a silent one).
  std::uint64_t d[32] = {};
  std::uint32_t fpscr = 0;
};

struct GuestThread {
  GuestThread() = default;
  /// Clears the binding cell shared with the host thread bound here (whichever host thread that is), so a
  /// destroyed context cannot be reached through any host thread's binding.
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
  /// A `rt_sigsuspend` in progress: it installed a temporary mask, and this is the mask to put back after
  /// the signal handler returns through `rt_sigreturn` (the kernel restores the pre-suspend mask then).
  bool sigsuspend_restore = false;
  std::uint64_t sigsuspend_old_mask = 0;
  /// C26: the signal deliveries this thread is inside, innermost last. A stack, not one record, because
  /// Mono's GC suspend is a two-signal protocol: SIGPWR enters the suspend handler, which waits for the
  /// restart signal (SIGXCPU, also installed by Mono) to run *inside* it before it returns. Delivery
  /// pushes here; `rt_sigreturn` restores the innermost frame and pops, so the outer handler resumes.
  std::vector<SignalDeliveryState> deliveries;
  /// Diagnostics: what the thread is doing while it is not executing guest code. Written by the thread
  /// itself, read by the launcher's liveness supervisor without the GGL; atomics so that read is defined.
  /// The first job is naming a stall, not controlling anything.
  enum class WaitKind : int { kNone = 0, kFutex, kSleep, kHostHeld, kHostReleased };
  std::atomic<int> wait_kind{static_cast<int>(WaitKind::kNone)};
  std::atomic<std::uint32_t> wait_arg{0};  ///< futex address, or the low word of a sleep's nanoseconds
  std::atomic<int> wait_slot{-1};          ///< the ABI host slot, while a host call is running
  /// Diagnostics: which step of the host call `wait_slot` is in (set by the JNI bridge: 0 = the inbound
  /// shadow merge, 1 = the forwarded call itself, 2 = inside the host JVM entry point), and when the host
  /// call began (ms, steady clock). A stall dump reads them to name the host-side wait, not to control it.
  std::atomic<int> host_step{-1};
  std::atomic<std::uint64_t> host_since_ms{0};
  /// Diagnostics: how many ExceptionCheck calls saw a pending Java exception on this thread. A thread that
  /// spins through the bridge (Unity's render loop) is spinning on one; the counter shows the rate.
  std::atomic<std::uint32_t> jni_pending{0};
  std::atomic<std::uint32_t> last_syscall{0};
  std::array<char, 16> name{};    ///< prctl(PR_SET_NAME), TASK_COMM_LEN bytes
  GuestAddr pthread_struct = 0;   ///< this thread's bionic `pthread_internal_t`, when there is one
  bool adopted = false;           ///< a host thread that entered guest code with no binding (Task 5)
  /// For an adopted thread: its synthesized thread mapping (stack, guard pages, static TLS), freed at exit.
  GuestAddr mapping = 0;
  std::uint32_t mapping_size = 0;

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

 private:
  friend bool BindCurrentHostThread(GuestThread&);
  std::shared_ptr<HostBinding> binding_;  ///< the cell shared with the bound host thread (see the .cpp)
};

/// The guest thread the calling host thread is bound to, or null.
GuestThread* CurrentGuestThread();

/// Binds the calling host thread to `t` (and `t` to it): guest calls made from this host thread run on `t`'s
/// registers and per-thread kernel state. The host thread's previous binding (to another GuestThread) is
/// released. False, changing nothing, when `t` is bound to another live host thread.
[[nodiscard]] bool BindCurrentHostThread(GuestThread& t);

/// Drops the calling host thread's binding (its `GuestThread`, if still bound here, then has no host thread).
void UnbindCurrentHostThread();

/// True when `t` is bound to the calling host thread (both directions agree).
bool BoundHere(const GuestThread& t);

/// Runs `hook` on the calling host thread when it exits (from a host `pthread_key` destructor, before its binding
/// is released), after the hooks added before it. Each hook runs once. The hook
/// must tolerate running after whatever it refers to is gone (capture a liveness token, not a raw owner).
void AddHostThreadExitHook(std::function<void()> hook);

}  // namespace rb::runtime
