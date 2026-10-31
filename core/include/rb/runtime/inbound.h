// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Inbound (M3): Java calling a guest native. A host entry point (from the trampoline pool in inbound_aarch64.S, or a
// generated stub's export) captures the AAPCS64 arguments ART passed; `Inbound::Dispatch` marshals them per the
// method's shorty into an AAPCS32-softfp guest call on the calling host thread's guest thread (adopted on first use),
// inside a local frame of the forwarding JNI bridge, and hands the result back in the shape ART expects.
//
// A guest fault does not crash the app: it becomes a pending java.lang.Error naming the fault, thrown into the Java
// caller when the native returns — a named line in the app's own stack trace.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rb/abi/host_java.h"
#include "rb/abi/jni_forward.h"
#include "rb/runtime/guest_process.h"

namespace rb::runtime {

/// What a capture saves: the integer and FP argument registers, and where the caller's stacked arguments begin.
/// The layout is the assembly's (offsets 0, 64, 128).
struct CapturedArgs {
  std::uint64_t x[8];
  std::uint64_t d[8];  ///< raw bits of d0-d7 (a jfloat is the low 32 bits)
  const std::uint64_t* stack;
};
static_assert(offsetof(CapturedArgs, d) == 64 && offsetof(CapturedArgs, stack) == 128, "the assembly's layout");

/// One guest native: its guest entry point (Thumb-tagged), its JNI shorty after (env, this/class) — return kind
/// first, e.g. "JIJF" for `long f(int, long, float)` — and a name for messages.
struct InboundTarget {
  GuestAddr fn = 0;
  std::string shorty;
  std::string name;
};

/// A JNI method signature's shorty (`(IJ)Ljava/lang/String;` -> "LIJ"), or empty when it is not one.
std::string ShortyOf(const std::string& sig);

class Inbound {
 public:
  static constexpr std::size_t kPoolSize = 4096;

  /// `tier` is the executor natives run on (2 = J). One Inbound per process: the pool is process-wide.
  Inbound(GuestProcess& p, abi::ForwardingJni& fwd, abi::HostJava& java, int tier = 2);
  ~Inbound();
  Inbound(const Inbound&) = delete;
  Inbound& operator=(const Inbound&) = delete;

  /// A host function pointer that, called by ART as a native with `t`'s shape, runs `t`. Refused by name when the
  /// shorty is not one or the pool (kPoolSize) is exhausted.
  void* BindPool(const InboundTarget& t, std::string* err);
  /// Runs `t` with the captured arguments; the result as raw bits (a reference as a real local's bits).
  std::uint64_t Dispatch(const InboundTarget& t, const CapturedArgs& a);

  /// What a guest `exit`/`exit_group` inside a native does on the host (default: a pending java.lang.Error naming
  /// it; the runtime installs `_exit`, which is what the same call does on a device).
  void set_on_exit(std::function<void(int status)> f) { on_exit_ = std::move(f); }

  /// Where a failed dispatch is reported. The fault still becomes a pending java.lang.Error for a Java caller,
  /// but a host whose Java is a table has no caller to read it: without this line the registers and stack of a
  /// native that faults — the only evidence of where the guest died — go to stderr, which the app does not keep.
  void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }

  /// The process's Inbound the trampolines reach (the most recently constructed live one).
  static Inbound* current();

  /// The bound entry `index` (for the trampoline entry; index < bound count).
  const InboundTarget& target(std::size_t index) const { return pool_[index]; }

 private:
  friend std::uint64_t InboundEntry(std::uint32_t index, CapturedArgs* a);
  void Fail(const InboundTarget& t, const std::string& why);

  GuestProcess& p_;
  abi::ForwardingJni& fwd_;
  abi::HostJava& java_;
  int tier_;
  std::function<void(const std::string&)> log_;
  std::mutex fail_mu_;
  std::map<std::string, unsigned> fail_counts_;  ///< per native: first three logged, then every 300th
  std::function<void(int)> on_exit_;
  std::mutex mu_;  ///< leaf: serializes BindPool
  /// Fixed-size, so a trampoline reading entry k never races a later BindPool growing it: an entry is complete
  /// before `bound_` (release) counts it.
  std::unique_ptr<InboundTarget[]> pool_;
  std::atomic<std::size_t> bound_{0};
};

}  // namespace rb::runtime
