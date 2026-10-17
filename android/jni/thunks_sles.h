// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// OpenSL ES for the guest (M4 Task 4), over this device's libOpenSLES.
//
// OpenSL ES is an object model, not a flat C API: a game calls `slCreateEngine` and then only ever
// `(*itf)->Method(itf, ...)`. So the guest gets guest-side objects. Each host interface (SLObjectItf, SLEngineItf,
// SLPlayItf, ...) is a guest *cell*: one word holding a guest vtable whose entries are host slots, one per method,
// shared by every interface of that kind. A method's `self` (the cell) maps back to the host interface. The
// `SL_IID_*` data imports are guest cells pointing at guest copies of the host GUIDs; IIDs the guest passes in are
// matched back to the host's by GUID. Data sources and sinks are translated (locators: Android simple buffer queue,
// buffer queue, Android fd, URI, output mix, address; formats: PCM, PCM_EX, MIME); a buffer the guest enqueues is
// guest memory, which the host reads in place. Callbacks (object, play, prefetch, buffer queue) come back on the
// host's audio thread and call the guest function there (`SlesHost::call_guest`, which adopts the thread).
//
// Locking: every method runs with the GGL held, except the host call itself when it takes no pointers (Realize,
// Resume, Destroy, SetPlayState, Clear, ...): those can wait for the audio thread, which may be waiting for the GGL
// to run a guest callback, so they run inside `SlesHost::unlocked`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/thunk.h"
#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::launcher {

/// What the OpenSL ES layer needs from the process it serves (HostSlots in the app; a fake in rb_sles_tests).
struct SlesHost {
  /// A host slot: reads its arguments from the guest state, writes r0. False with `why` for a call that could not
  /// be made at all (the method has already answered the guest otherwise).
  using Method = std::function<bool(exec::MachineState& s, std::string* why)>;
  mem::GuestMemory* mem = nullptr;
  abi::HandleTable* handles = nullptr;
  /// Fresh readable+writable guest memory (page-rounded), 0 when none is left. Called with the GGL held.
  std::function<std::uint32_t(std::size_t bytes)> alloc;
  /// Binds `fn` to a host slot named `name` (the same name answers the same slot) and returns its guest address.
  std::function<std::uint32_t(const std::string& name, Method fn)> bind;
  /// Calls guest function `fn` with 32-bit `args` from any host thread (an audio thread: adopted, takes the GGL).
  std::function<void(std::uint32_t fn, const std::vector<std::uint32_t>& args)> call_guest;
  /// Runs `f` with the GGL released (and re-taken after), from a slot that holds it.
  std::function<void(const std::function<void()>& f)> unlocked;
  /// A diagnostic line (a refusal, an unsupported interface). May be called from an audio thread.
  std::function<void(const std::string&)> say;
};

class Sles {
 public:
  explicit Sles(SlesHost host);
  ~Sles();
  Sles(const Sles&) = delete;
  Sles& operator=(const Sles&) = delete;

  /// `slCreateEngine`, `slQueryNumSupportedEngineInterfaces`, `slQuerySupportedEngineInterfaces` (code: a slot
  /// address) and `SL_IID_*` (data: the address of a guest SLInterfaceID cell). nullopt for any other name, or when
  /// this device's libOpenSLES does not have it. Called with the GGL held.
  std::optional<std::uint32_t> Resolve(const std::string& name);
  /// Host objects the guest has created and not destroyed.
  [[nodiscard]] std::size_t live_objects() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rb::launcher
