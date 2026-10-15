/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::GuestThread binding — see rb/runtime/guest_thread.h.
 */
#include "rb/runtime/guest_thread.h"

namespace rb::runtime {

namespace {
thread_local GuestThread* tls_current = nullptr;
}  // namespace

GuestThread::~GuestThread() {
  if (tls_current == this) tls_current = nullptr;
}

GuestThread* CurrentGuestThread() { return tls_current; }

void BindCurrentHostThread(GuestThread& t) {
  if (tls_current != nullptr && tls_current != &t &&
      tls_current->host.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    tls_current->host.store(std::thread::id{}, std::memory_order_release);
  }
  t.host.store(std::this_thread::get_id(), std::memory_order_release);
  tls_current = &t;
}

void UnbindCurrentHostThread() {
  if (tls_current != nullptr && tls_current->host.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    tls_current->host.store(std::thread::id{}, std::memory_order_release);
  }
  tls_current = nullptr;
}

bool BoundHere(const GuestThread& t) {
  return tls_current == &t && t.host.load(std::memory_order_acquire) == std::this_thread::get_id();
}

}  // namespace rb::runtime
