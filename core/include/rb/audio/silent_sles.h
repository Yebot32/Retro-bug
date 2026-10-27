// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A complete-enough OpenSL ES object graph for a host with no audio backend.
//
// Titles built on OpenAL Soft (KitKat Challenge's DLGE engine is one) do not treat a failed
// `slCreateEngine` as "run without sound": their startup path still uses the device it asked for.
// The safe compatibility answer is therefore a real object model whose objects satisfy the calls
// that open a device and build a player, while accepting buffers and dropping them.
//
// It is deliberately not a stub: every interface pointer is a guest cell holding a guest vtable,
// exactly as Android's real libOpenSLES answers them, so the guest's own `(*itf)->Method` path is
// exercised. Buffer callbacks are not delivered; audio remains silent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "rb/exec/machine_state.h"

namespace rb::mem {
class GuestMemory;
}

namespace rb::audio {

/// What the silent engine needs from its host. Deliberately as small as the real OpenSL layer's
/// seam, so the object graph can be exercised without a guest process or an Apple framework.
struct SilentSlesHost {
  using Method = std::function<bool(exec::MachineState& s, std::string* why)>;
  mem::GuestMemory* mem = nullptr;
  std::function<std::uint32_t(std::size_t bytes)> alloc;
  std::function<std::uint32_t(const std::string& name, Method fn)> bind;
  std::function<void(const std::string& name, std::uint32_t at)> data_symbol;
  std::function<void(const std::string&)> log;
};

class SilentSles {
 public:
  /// Registers `slCreateEngine`, the interface-ID data imports and every method slot.
  /// Call once while the guest is not running.
  explicit SilentSles(SilentSlesHost host);
  ~SilentSles();
  SilentSles(const SilentSles&) = delete;
  SilentSles& operator=(const SilentSles&) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rb::audio
