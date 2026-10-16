// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// ForwardingJni: the guest `JNIEnv` (and, with Task 2, `JavaVM`) of the M3 JNI bridge. Like the launcher's host-class
// model (`JniEnv`), it is a function table in guest memory whose entries are `AbiContext` host slots — but every
// entry forwards to real Java (`HostJava`), translating 32-bit guest handles to real references and back.
//
// Guest handles. The guest only ever hands a handle back to a table entry; a correct guest never dereferences one.
// They are tokens high in the 4 GiB window, in ranges the bridge never maps, so a guest that does dereference one
// faults by name:
//   locals         kLocalBase  | thread << 18 | n << 2   per host thread, frames mirror Push/PopLocalFrame
//   globals        kGlobalBase + 4 n                     one table, indices reused after DeleteGlobalRef
//   weak globals   kWeakBase   + 4 n
//   classes        kClassBase  + 4 n                     one stable handle per class (a bijection, pinned by a
//                                                        real global ref), whether it came from FindClass,
//                                                        GetObjectClass or GetSuperclass: old games keep a jclass
//                                                        across calls without NewGlobalRef, which works with this
//   method/field   kIdBase     + 4 n                     a bijection with the real jmethodID/jfieldID
// A local's slot is never reused while its thread is inside the frame that made it, so a deleted local is refused
// as stale rather than silently aliasing a newer one. Every slot releases the Global Guest Lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/abi/host_java.h"
#include "rb/mem/guest_memory.h"

namespace rb::abi {

class ForwardingJni {
 public:
  static constexpr std::uint32_t kEnvSlotBase = 640;  ///< JNIEnv index i is slot kEnvSlotBase + i
  static constexpr std::uint32_t kVmSlotBase = 880;   ///< JavaVM invoke-interface index i is slot kVmSlotBase + i
  static constexpr std::uint32_t kTableEntries = 233;
  static constexpr GuestAddr kLocalBase = 0xF000'0000u;
  static constexpr GuestAddr kGlobalBase = 0xE000'0000u;
  static constexpr GuestAddr kWeakBase = 0xD800'0000u;
  static constexpr GuestAddr kClassBase = 0xD400'0000u;
  static constexpr GuestAddr kIdBase = 0xD000'0000u;
  static constexpr std::uint32_t kMaxThreads = 1024;      ///< host threads with a local table
  static constexpr std::uint32_t kMaxLocals = 1u << 16;   ///< live-or-stale local slots per thread
  /// Guest bytes the bridge maps at `where`: the env word, then the table.
  static constexpr std::uint32_t kBytes = 0x1000;

  /// Maps one page at `where` (page-aligned, free) and binds slots kEnvSlotBase.. on `abi`. Check ok().
  ForwardingJni(mem::GuestMemory& memory, AbiContext& abi, GuestAddr where, HostJava& java, std::string* err);
  /// Rebinds its slots to a named refusal (the slots outlive it) and unmaps its page.
  ~ForwardingJni();
  ForwardingJni(const ForwardingJni&) = delete;
  ForwardingJni& operator=(const ForwardingJni&) = delete;

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return err_; }
  /// The guest `JNIEnv*` (the same value on every thread: per-thread state is keyed by the host thread).
  [[nodiscard]] GuestAddr env() const { return where_; }

  /// A real local reference made into a guest local in the calling thread's current frame (0 stays 0).
  GuestAddr ToGuestLocal(HostRef r, std::string* err = nullptr);
  /// The real reference behind a guest handle (0 for null), or 0 with `err` set when it is not a live handle
  /// this bridge handed to this thread.
  HostRef FromGuest(GuestAddr handle, std::string* err);
  /// One frame per inbound native call: PushFrame on entry, PopFrame on return, which gives the real reference
  /// behind `result` (the native's return value) before the frame's guest locals go.
  void PushFrame();
  HostRef PopFrame(GuestAddr result, std::string* err = nullptr);

  [[nodiscard]] std::size_t live_locals() const { return live_locals_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::size_t live_globals() const;
  [[nodiscard]] std::size_t refusals() const { return refusals_.load(std::memory_order_relaxed); }

 private:
  struct LocalTable {
    std::uint32_t thread = 0;
    std::vector<HostRef> refs;    ///< by slot; 0 = deleted (stale until the frame that made it pops)
    std::vector<std::size_t> frames;  ///< the refs.size() at each PushFrame
  };
  struct IdRec {
    HostId id = 0;
    bool is_field = false;
    bool is_static = false;
    std::vector<char> params;  ///< methods
    char kind = 'V';           ///< a method's return kind, a field's type kind
    std::string name;          ///< for messages: name + sig
  };

  bool Slot(std::uint32_t index, exec::MachineState& s, std::string* err);
  LocalTable* Locals(std::string* err);
  GuestAddr ClassHandle(HostRef cls_local, bool delete_local, std::string* err);
  GuestAddr IdHandle(HostId id, bool is_field, bool is_static, const std::string& name, const std::string& sig,
                     std::string* err);
  const IdRec* Id(GuestAddr handle, std::string* err);
  bool ReadCString(GuestAddr at, std::string* out, std::string* err) const;
  void Return32(exec::MachineState& s, std::uint32_t v) const { s.r[0] = v; }
  void Return64(exec::MachineState& s, std::uint64_t v) const;
  bool ReturnKind(exec::MachineState& s, char kind, std::uint64_t bits, std::string* err);

  bool CallFamily(std::uint32_t index, exec::MachineState& s, std::string* err);
  bool FieldFamily(std::uint32_t index, exec::MachineState& s, std::string* err);

  mem::GuestMemory& memory_;
  AbiContext& abi_;
  HostJava& java_;
  GuestAddr where_ = 0;
  bool ok_ = false;
  std::string err_;
  const std::uint64_t serial_;  ///< distinguishes instances in the per-thread local-table registry

  std::atomic<std::uint32_t> next_thread_{0};
  std::atomic<std::size_t> live_locals_{0};
  std::atomic<std::size_t> refusals_{0};

  mutable std::mutex globals_mu_;  ///< leaf
  std::vector<HostRef> globals_;   ///< 0 = free
  std::vector<std::size_t> global_free_;
  std::vector<HostRef> weaks_;
  std::vector<std::size_t> weak_free_;

  mutable std::mutex classes_mu_;  ///< leaf; never held across a HostJava call except IsSameObject scans
  std::vector<HostRef> classes_;   ///< real global refs
  std::map<std::string, std::size_t> class_by_name_;

  mutable std::mutex ids_mu_;  ///< leaf
  std::vector<IdRec> ids_;
  std::map<std::pair<HostId, bool>, std::size_t> id_index_;
};

}  // namespace rb::abi
