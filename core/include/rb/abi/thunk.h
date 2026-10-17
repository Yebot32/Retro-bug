// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The generic thunk caller (M4): a guest call that reached a host slot, forwarded to a real host function from a
// signature string. The guest passed its arguments by AAPCS32 with the soft-float variant Android's armeabi uses
// (floats and doubles in core registers and stack words, doubles and 64-bit integers 8-aligned); the host function
// takes them by AAPCS64 (integers in x0..x7 then the stack, floating point in v0..v7).
//
// Signature: "<ret>:<args>". Argument letters:
//   i  int32 (sign-extended)        u  uint32 (zero-extended)       f  float        d  double (aligned pair)
//   l  int64 (aligned pair)         p  guest pointer: 0 stays null, anything else must be guest memory
//   o  GL pointer-or-offset: translated when it is guest memory, passed as is otherwise (a buffer offset)
//   h  a handle this table issued (0 = null) -> the host pointer
// Return letters: v, i, u, f, d, l, and H (a host pointer, returned to the guest as a handle).
// A host pointer is never handed to the guest raw: it does not fit in 32 bits.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::abi {

/// 32-bit guest names for host objects the guest only passes back (EGLDisplay, AAsset*, SLObjectItf, ...). One
/// handle per host pointer for as long as it is known; 0 is null; a value this table never issued is refused.
class HandleTable {
 public:
  static constexpr std::uint32_t kBase = 0xC000'0000u;
  std::uint32_t Put(void* host);
  /// False (and `*host` untouched) for a value this table did not issue or has forgotten.
  bool Get(std::uint32_t handle, void** host) const;
  void Forget(void* host);
  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mu_;
  std::map<void*, std::uint32_t> by_host_;
  std::vector<void*> by_index_;  ///< index = (handle - kBase) / 4; nullptr once forgotten
};

class ThunkCaller {
 public:
  /// Reads the guest's arguments for `spec` from `s` (r0..r3, then the stack at sp through `mem`), calls `fn`, and
  /// writes the result to r0 (r0:r1 for `d`/`l`). False, with `err` naming the cause, for a malformed signature,
  /// an unreadable stacked word, a pointer that is not guest memory, or an unknown handle; nothing is called then.
  static bool Call(const char* spec, void* fn, exec::MachineState& s, const mem::GuestMemory& mem,
                   HandleTable& handles, std::string* err);
};

}  // namespace rb::abi
