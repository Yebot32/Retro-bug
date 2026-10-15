/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::FutexTable — see rb/runtime/futex_table.h.
 */
#include "rb/runtime/futex_table.h"

#include <algorithm>

namespace rb::runtime {

std::shared_ptr<FutexTable::Waiter> FutexTable::Enqueue(exec::GuestAddr addr, std::uint32_t bitset) {
  auto w = std::make_shared<Waiter>();
  w->addr = addr;
  w->bitset = bitset;
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
  const auto ready = [&] { return w->woken || shutdown_; };
  bool got = true;
  if (deadline) {
    got = w->cv.wait_until(lk, *deadline, ready);
  } else {
    w->cv.wait(lk, ready);
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
    if (((*it)->bitset & bitset) == 0) {
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

bool FutexTable::SleepUntil(Clock::time_point deadline) {
  std::unique_lock<std::mutex> lk(mu_);
  return !sleep_cv_.wait_until(lk, deadline, [&] { return shutdown_; });
}

void FutexTable::Shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  shutdown_ = true;
  for (auto& [addr, d] : queues_) {
    for (auto& w : d) w->cv.notify_all();
  }
  sleep_cv_.notify_all();
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
