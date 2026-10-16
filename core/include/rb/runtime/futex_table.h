/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::FutexTable — the guest process's futex waiters (M2 Task 4).
 *
 * One per guest process. A guest `futex(WAIT)` parks its host thread on a host condition variable keyed by
 * the guest address; `futex(WAKE)` (and a thread's exit, through `CLONE_CHILD_CLEARTID`) wakes up to `n` of
 * them in FIFO order. Private and shared futexes are the same table: there is one guest process.
 *
 * **The lost-wakeup rule, and why `Wait` is two calls.** The kernel checks the word and queues the waiter
 * atomically with respect to every waker. Here the GGL is that atomicity: every waker (guest `futex(WAKE)`,
 * a thread exit) runs with the GGL held, so a waiter that (1) reads the word and (2) `Enqueue`s itself while
 * still holding the GGL cannot miss a wake that follows its check — and only then (3) releases the GGL and
 * `Block`s. The monitor does (1)+(2) in `Classify` (GGL held) and hands (3) to the bridge as the blocking
 * call's wait, which runs with the GGL released. A wake that lands between (2) and (3) marks the waiter
 * woken, and `Block` returns at once.
 *
 * `Shutdown` (the process's `exit_group`, or its teardown) wakes every waiter and sleeper and makes every
 * later `Block`/`SleepUntil` return immediately, so no host thread stays parked in a process that ended.
 *
 * Locking: the table's own mutex is a leaf — held only inside these calls, never across a callout, and
 * never while waiting for the GGL.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "rb/exec/machine_state.h"

namespace rb::runtime {

class FutexTable {
 public:
  using Clock = std::chrono::steady_clock;
  /// FUTEX_BITSET_MATCH_ANY: what plain WAIT/WAKE use.
  static constexpr std::uint32_t kMatchAny = 0xffff'ffffu;

  struct Waiter {
    exec::GuestAddr addr = 0;
    std::uint32_t bitset = kMatchAny;
    bool woken = false;  ///< set by Wake (under the table's mutex)
    std::condition_variable cv;
  };

  enum class WaitResult { kWoken, kTimedOut, kShutdown };

  FutexTable() = default;
  FutexTable(const FutexTable&) = delete;
  FutexTable& operator=(const FutexTable&) = delete;

  /// Step 2 of a wait: queue a waiter on `addr`. Called with the GGL held, after the caller read the word
  /// and found the expected value (see the header).
  std::shared_ptr<Waiter> Enqueue(exec::GuestAddr addr, std::uint32_t bitset = kMatchAny);

  /// Step 3: block (GGL released) until woken, `deadline` passes, or `Shutdown`. A waiter that was not
  /// woken is removed from the queue before this returns, so a later wake cannot count it.
  WaitResult Block(const std::shared_ptr<Waiter>& w, std::optional<Clock::time_point> deadline);

  /// Wakes up to `n` waiters on `addr` whose bitset intersects `bitset`, oldest first. Called with the GGL
  /// held. Returns how many were woken.
  int Wake(exec::GuestAddr addr, int n, std::uint32_t bitset = kMatchAny);

  /// A sleep (nanosleep, clock_nanosleep) that `Shutdown` can cut short. False when it was cut short.
  bool SleepUntil(Clock::time_point deadline);

  /// Wakes everyone and makes every later wait return at once (`kShutdown`). Irreversible.
  void Shutdown();
  [[nodiscard]] bool shut_down() const;

  /// Waiters queued right now (all addresses). Advisory.
  [[nodiscard]] std::size_t waiters() const;
  /// Total waits that blocked, wakes delivered and waits that timed out — counters for reports and tests.
  [[nodiscard]] std::uint64_t blocked() const;
  [[nodiscard]] std::uint64_t woken() const;
  [[nodiscard]] std::uint64_t timed_out() const;

 private:
  mutable std::mutex mu_;
  std::condition_variable sleep_cv_;
  std::unordered_map<exec::GuestAddr, std::deque<std::shared_ptr<Waiter>>> queues_;
  bool shutdown_ = false;
  std::uint64_t blocked_ = 0, woken_ = 0, timed_out_ = 0;
};

}  // namespace rb::runtime
