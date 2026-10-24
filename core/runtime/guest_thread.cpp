/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::GuestThread binding — see rb/runtime/guest_thread.h.
 *
 * Per host thread: a `HostState`, reached through a trivially destructible `thread_local` pointer (safe to read
 * from inside pthread-key destructors, where a `thread_local` object with a destructor may already be gone) and
 * owned by one process-wide host `pthread_key_t` whose destructor runs when the host thread exits. The
 * GuestThread <-> host-thread link is a shared `HostBinding` cell: the GuestThread's destructor nulls it (so no
 * host thread's binding can reach a destroyed context — M2 Task 5 ruling c) and the host thread's exit nulls it
 * and releases the GuestThread's `host` (ruling d). Its mutex is a leaf: held only to read-and-clear the cell.
 */
#include "rb/runtime/guest_thread.h"

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace rb::runtime {

struct HostBinding {
  std::mutex mu;
  std::atomic<GuestThread*> thread{nullptr};
};

namespace {

struct HostState {
  std::shared_ptr<HostBinding> binding;
  std::vector<std::function<void()>> exit_hooks;
};

thread_local HostState* tls_state = nullptr;

void ReleaseBinding(HostState& s, std::thread::id me) {
  if (!s.binding) return;
  {
    std::lock_guard<std::mutex> lk(s.binding->mu);
    if (GuestThread* t = s.binding->thread.load(std::memory_order_acquire)) {
      std::thread::id expected = me;
      t->host.compare_exchange_strong(expected, std::thread::id{}, std::memory_order_acq_rel);
      s.binding->thread.store(nullptr, std::memory_order_release);
    }
  }
  s.binding.reset();
}

void OnHostThreadExit(void* p) {
  auto* s = static_cast<HostState*>(p);
  // The exit hook (an adopted thread's guest teardown) runs first, while the binding is still in place: it runs
  // guest code on the adopted GuestThread. Then the binding goes, then the state.
  while (!s->exit_hooks.empty()) {  // a hook may add another (it is run too)
    std::vector<std::function<void()>> hooks;
    hooks.swap(s->exit_hooks);
    for (auto& h : hooks) h();
  }
  ReleaseBinding(*s, std::this_thread::get_id());
  tls_state = nullptr;
  delete s;
}

pthread_key_t HostKey() {
  static pthread_key_t key = [] {
    pthread_key_t k{};
    if (pthread_key_create(&k, OnHostThreadExit) != 0) {
      std::fprintf(stderr, "rb::runtime: pthread_key_create failed — host-thread exit cannot be observed\n");
      std::abort();
    }
    return k;
  }();
  return key;
}

HostState& State() {
  if (tls_state == nullptr) {
    tls_state = new HostState();
    pthread_setspecific(HostKey(), tls_state);
  }
  return *tls_state;
}

}  // namespace

GuestThread::~GuestThread() {
  if (binding_) {
    std::lock_guard<std::mutex> lk(binding_->mu);
    GuestThread* self = this;
    binding_->thread.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  }
}

GuestThread* CurrentGuestThread() {
  HostState* s = tls_state;
  if (s == nullptr || !s->binding) return nullptr;
  return s->binding->thread.load(std::memory_order_acquire);
}

bool BindCurrentHostThread(GuestThread& t) {
  const std::thread::id me = std::this_thread::get_id();
  std::thread::id expected{};
  if (!t.host.compare_exchange_strong(expected, me, std::memory_order_acq_rel) && expected != me) {
    return false;  // bound to another live host thread: never stolen (ruling b)
  }
  // The host's Linux tid, for the Java APIs that take one: a guest tid is ours, and ART answers
  // "Given thread N does not exist" when the guest hands it one (Unity's Process.setThreadPriority does).
  t.host_tid.store(static_cast<std::int32_t>(::gettid()), std::memory_order_release);
  HostState& s = State();
  if (s.binding && s.binding->thread.load(std::memory_order_acquire) == &t) return true;
  ReleaseBinding(s, me);  // this host thread's previous GuestThread, if any, is now free
  auto b = std::make_shared<HostBinding>();
  b->thread.store(&t, std::memory_order_release);
  t.binding_ = b;  // only the host thread holding `t.host` writes it
  s.binding = std::move(b);
  return true;
}

void UnbindCurrentHostThread() {
  if (tls_state != nullptr) ReleaseBinding(*tls_state, std::this_thread::get_id());
}

bool BoundHere(const GuestThread& t) {
  return CurrentGuestThread() == &t && t.host.load(std::memory_order_acquire) == std::this_thread::get_id();
}

void AddHostThreadExitHook(std::function<void()> hook) { State().exit_hooks.push_back(std::move(hook)); }

}  // namespace rb::runtime
