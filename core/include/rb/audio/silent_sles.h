// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// OpenSL ES guest objects with optional native PCM output.
//
// Titles built on OpenAL Soft (KitKat Challenge's DLGE engine is one) do not treat a failed
// `slCreateEngine` as "run without sound": their startup path still uses the device it asked for.
// The safe compatibility answer is therefore a real object model whose objects satisfy the calls
// that open a device and build a player. A host without PCM support accepts buffers silently.
//
// It is deliberately not a stub: every interface pointer is a guest cell holding a guest vtable,
// exactly as Android's real libOpenSLES answers them, so the guest's own `(*itf)->Method` path is
// exercised. Hosts that provide PCM output deliver completions on a guest-capable thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "rb/exec/machine_state.h"

namespace rb::mem {
class GuestMemory;
}

namespace rb::audio {

struct PcmFormat {
  unsigned sample_rate;
  unsigned channels;
  unsigned bits;
  unsigned buffers;
};

class PcmOutput {
 public:
  virtual ~PcmOutput() = default;
  virtual bool Enqueue(std::span<const std::uint8_t> bytes) = 0;
  virtual void SetPlaying(bool playing) = 0;
  virtual void Clear() = 0;
  virtual unsigned TakeCompleted() = 0;
};

/// The object graph supports either a host PCM device or a muted sink.
struct SilentSlesHost {
  using Method = std::function<bool(exec::MachineState& s, std::string* why)>;
  mem::GuestMemory* mem = nullptr;
  std::function<std::uint32_t(std::size_t bytes)> alloc;
  std::function<std::uint32_t(const std::string& name, Method fn)> bind;
  std::function<void(const std::string& name, std::uint32_t at)> data_symbol;
  std::function<void(const std::string&)> log;
  std::function<std::unique_ptr<PcmOutput>(PcmFormat)> open_pcm;
};

class SilentSles {
 public:
  /// Registers `slCreateEngine`, the interface-ID data imports and every method slot.
  /// Call once while the guest is not running.
  explicit SilentSles(SilentSlesHost host);
  ~SilentSles();
  SilentSles(const SilentSles&) = delete;
  SilentSles& operator=(const SilentSles&) = delete;
  /// Runs on a guest-capable host thread with the guest lock held.
  void Pump(const std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)>& callback);
  void SetPaused(bool paused);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rb::audio
