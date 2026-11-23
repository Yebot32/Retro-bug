/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::Ggl — the Global Guest Lock (M2).
 *
 * One per guest process (the `AbiContext` owns it; `GuestProcess::ggl()` reaches it). At most one host
 * thread executes guest code at a time: the lock is held while any guest instruction runs, and released
 * around blocking syscalls, around host-slot calls, and at every preemption slice (`AbiContext::kSliceSteps`),
 * then re-acquired before guest code resumes. It is what makes the shared executors (one Tier J, one Tier I
 * per process — they are stateless between `Run` calls), the kuser helpers' read-then-write and the monitor's
 * process tables safe without each having its own lock.
 *
 * **Fair, by construction.** A plain `std::mutex` lets the releasing thread barge straight back in (release,
 * yield, re-acquire can win every time), so a guest thread spinning without syscalls would starve every other
 * one — the failure slicing exists to prevent. This is a ticket lock: each `Acquire` takes the next ticket and
 * waits for its turn, so a slice boundary hands the lock to the longest waiter, in FIFO order.
 *
 * Lock order (the only rule): the GGL is never *waited for* while holding any other engine lock that a GGL
 * holder may also wait for. Everything the GGL protects is touched only by its holder; host code that runs
 * with it released (host slots, a blocking syscall's wait) must not touch guest-process tables without
 * re-acquiring it (`Held`), and must not hold a host lock across that re-acquire that a GGL holder takes.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace rb::runtime {

class Ggl {
 public:
  Ggl() = default;
  Ggl(const Ggl&) = delete;
  Ggl& operator=(const Ggl&) = delete;

  /// Waits for this thread's turn (FIFO) and takes the lock. Not recursive: a thread that already holds it
  /// gets nothing new — the call is counted in `misuses()` and returns — because a second hold could only be
  /// released by a second `Release`, and the RAII helpers below are the supported way in.
  void Acquire() {
    if (HeldByMe()) {
      misuses_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::unique_lock<std::mutex> lk(mu_);
    const std::uint64_t mine = next_ticket_++;
    cv_.wait(lk, [&] { return now_serving_ == mine; });
    owner_.store(std::this_thread::get_id(), std::memory_order_release);
    acquisitions_.fetch_add(1, std::memory_order_relaxed);
  }

  /// Hands the lock to the next ticket. A thread that does not hold it changes nothing (counted in
  /// `misuses()`), so a stray release cannot let two threads into guest code.
  void Release() {
    if (!HeldByMe()) {
      misuses_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      owner_.store(std::thread::id{}, std::memory_order_release);
      ++now_serving_;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool HeldByMe() const {
    return owner_.load(std::memory_order_acquire) == std::this_thread::get_id();
  }

  /// How many host threads are waiting for the lock right now (0 or more). Advisory: it can change the
  /// moment it is read.
  [[nodiscard]] std::uint64_t waiters() const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t taken = next_ticket_ - now_serving_;
    return owner_.load(std::memory_order_acquire) == std::thread::id{} ? taken : (taken == 0 ? 0 : taken - 1);
  }
  [[nodiscard]] std::uint64_t acquisitions() const { return acquisitions_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t misuses() const { return misuses_.load(std::memory_order_relaxed); }

  /// Holds the GGL for a scope; a no-op when this thread already holds it (a nested guest call made from
  /// code that runs under the lock).
  class Held {
   public:
    explicit Held(Ggl& g) : g_{g}, took_{!g.HeldByMe()} {
      if (took_) g_.Acquire();
    }
    ~Held() {
      if (took_) g_.Release();
    }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

   private:
    Ggl& g_;
    bool took_;
  };

  /// Releases the GGL for a scope and re-acquires it on exit (a host call, a blocking syscall's wait, a
  /// slice boundary); a no-op when this thread does not hold it.
  class Released {
   public:
    explicit Released(Ggl& g) : g_{g}, was_{g.HeldByMe()} {
      if (was_) g_.Release();
    }
    ~Released() {
      if (was_) g_.Acquire();
    }
    Released(const Released&) = delete;
    Released& operator=(const Released&) = delete;

   private:
    Ggl& g_;
    bool was_;
  };

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::uint64_t next_ticket_ = 0;
  std::uint64_t now_serving_ = 0;
  std::atomic<std::thread::id> owner_{};
  std::atomic<std::uint64_t> acquisitions_{0};
  std::atomic<std::uint64_t> misuses_{0};
};

}  // namespace rb::runtime
