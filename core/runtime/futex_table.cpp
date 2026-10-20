/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::FutexTable — see rb/runtime/futex_table.h.
 */
#include "rb/runtime/futex_table.h"

#include <algorithm>

namespace rb::runtime {

std::shared_ptr<FutexTable::Waiter> FutexTable::Enqueue(exec::GuestAddr addr, std::uint32_t bitset,
                                                        std::int32_t tid) {
  auto w = std::make_shared<Waiter>();
  w->addr = addr;
  w->bitset = bitset;
  w->tid = tid;
  std::lock_guard<std::mutex> lk(mu_);
  if (shutdown_) {
    w->woken = true;  // nothing will ever wake it: let Block report the shutdown at once
    return w;
  }
  queues_[addr].push_back(w);
  return w;
}

FutexTable::WaitResult FutexTable::Block(const std::shared_ptr<Waiter>& w, std::optional<Clock::time_point> deadline) {
  std::unique_lock<std::mutex> lk(mu_);
  ++blocked_;
  const auto ready = [&] { return w->woken || w->interrupted || shutdown_; };
  bool got = true;
  if (deadline) {
    got = w->cv.wait_until(lk, *deadline, ready);
  } else {
    w->cv.wait(lk, ready);
  }
  // Interrupted wins over woken: the thread must take its pending signal before it retries the futex,
  // and a wake that arrived in the same instant must not be consumed by a thread about to run a handler.
  if (w->interrupted && !shutdown_) {
    const auto q = queues_.find(w->addr);
    if (q != queues_.end()) {
      auto& d = q->second;
      d.erase(std::remove(d.begin(), d.end(), w), d.end());
      if (d.empty()) queues_.erase(q);
    }
    return WaitResult::kInterrupted;
  }
  if (w->woken && !shutdown_) return WaitResult::kWoken;
  // Not woken (timed out, or shut down): take it off its queue so no later wake counts it.
  const auto q = queues_.find(w->addr);
  if (q != queues_.end()) {
    auto& d = q->second;
    d.erase(std::remove(d.begin(), d.end(), w), d.end());
    if (d.empty()) queues_.erase(q);
  }
  if (shutdown_) return WaitResult::kShutdown;
  if (!got) ++timed_out_;
  return got ? WaitResult::kWoken : WaitResult::kTimedOut;
}

int FutexTable::Wake(exec::GuestAddr addr, int n, std::uint32_t bitset) {
  if (n <= 0) return 0;
  int count = 0;
  std::lock_guard<std::mutex> lk(mu_);
  const auto q = queues_.find(addr);
  if (q == queues_.end()) return 0;
  auto& d = q->second;
  for (auto it = d.begin(); it != d.end() && count < n;) {
    if (((*it)->bitset & bitset) == 0 || (*it)->interrupted) {
      ++it;
      continue;
    }
    (*it)->woken = true;
    (*it)->cv.notify_one();
    it = d.erase(it);
    ++count;
  }
  if (d.empty()) queues_.erase(q);
  woken_ += static_cast<std::uint64_t>(count);
  return count;
}

int FutexTable::Interrupt(std::int32_t tid) {
  if (tid == 0) return 0;
  std::lock_guard<std::mutex> lk(mu_);
  int count = 0;
  for (auto& [addr, d] : queues_) {
    for (auto& w : d) {
      if (w->tid != tid || w->woken || w->interrupted) continue;
      w->interrupted = true;
      w->cv.notify_all();
      ++count;
    }
  }
  if (sleepers_.count(tid) != 0) {
    interrupted_sleepers_.insert(tid);
    sleep_cv_.notify_all();
  }
  if (signal_waiters_.count(tid) != 0) {
    signal_wake_.insert(tid);
    signal_cv_.notify_all();
  }
  return count;
}

void FutexTable::BeginSignalWait(std::int32_t tid) {
  if (tid == 0) return;
  std::lock_guard<std::mutex> lk(mu_);
  signal_wake_.erase(tid);
  signal_waiters_.insert(tid);
}

bool FutexTable::WaitForSignal(std::int32_t tid) {
  std::unique_lock<std::mutex> lk(mu_);
  signal_cv_.wait(lk, [&] { return shutdown_ || signal_wake_.count(tid) != 0; });
  signal_waiters_.erase(tid);
  const bool woke = signal_wake_.erase(tid) != 0;
  return woke && !shutdown_;
}

void FutexTable::BeginSleep(std::int32_t tid) {
  if (tid == 0) return;
  std::lock_guard<std::mutex> lk(mu_);
  sleepers_.insert(tid);
}

FutexTable::SleepResult FutexTable::SleepUntil(Clock::time_point deadline, std::int32_t tid) {
  std::unique_lock<std::mutex> lk(mu_);
  if (tid != 0) sleepers_.insert(tid);  // idempotent with BeginSleep; covers a direct SleepUntil caller
  sleep_cv_.wait_until(lk, deadline, [&] {
    return shutdown_ || (tid != 0 && interrupted_sleepers_.count(tid) != 0);
  });
  bool interrupted = false;
  if (tid != 0) {
    sleepers_.erase(tid);
    interrupted = interrupted_sleepers_.erase(tid) != 0;
  }
  if (shutdown_) return SleepResult::kShutdown;
  return interrupted ? SleepResult::kInterrupted : SleepResult::kSlept;
}

void FutexTable::Shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  shutdown_ = true;
  for (auto& [addr, d] : queues_) {
    for (auto& w : d) w->cv.notify_all();
  }
  sleep_cv_.notify_all();
  signal_cv_.notify_all();
}

bool FutexTable::shut_down() const {
  std::lock_guard<std::mutex> lk(mu_);
  return shutdown_;
}

std::size_t FutexTable::waiters() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t n = 0;
  for (const auto& [addr, d] : queues_) n += d.size();
  return n;
}

std::uint64_t FutexTable::blocked() const {
  std::lock_guard<std::mutex> lk(mu_);
  return blocked_;
}
std::uint64_t FutexTable::woken() const {
  std::lock_guard<std::mutex> lk(mu_);
  return woken_;
}
std::uint64_t FutexTable::timed_out() const {
  std::lock_guard<std::mutex> lk(mu_);
  return timed_out_;
}

}  // namespace rb::runtime
