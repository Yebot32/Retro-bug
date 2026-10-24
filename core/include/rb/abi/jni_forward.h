// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// ForwardingJni: the guest `JNIEnv` (and, with Task 2, `JavaVM`) of the M3 JNI bridge. Like the launcher's host-class
// model (`JniEnv`), it is a function table in guest memory whose entries are `AbiContext` host slots — but every
// entry forwards to real Java (`HostJava`), translating 32-bit guest handles to real references and back.
//
// Guest handles. The guest only ever hands a handle back to a table entry; a correct guest never dereferences one.
// They are tokens high in the 4 GiB window, in ranges the bridge never maps, so a guest that does dereference one
// faults by name:
//   locals         kLocalBase  | thread << 20 | n << 6 | serial << 2   per host thread, frames mirror Push/PopLocalFrame
//   globals        kGlobalBase + 4 n                     one table, indices reused after DeleteGlobalRef
//   weak globals   kWeakBase   + 4 n
//   classes        kClassBase  + 4 n                     one stable handle per class (a bijection, pinned by a
//                                                        real global ref), whether it came from FindClass,
//                                                        GetObjectClass or GetSuperclass: old games keep a jclass
//                                                        across calls without NewGlobalRef, which works with this
//   method/field   kIdBase     + 4 n                     a bijection with the real jmethodID/jfieldID
// A local's slot is reused (a native looping inside one call must not run out), but each reuse bumps the slot's 4-bit
// serial, as ART's indirect references do, so a deleted local or one whose frame has popped is refused as stale
// rather than silently aliasing a newer one. Thread numbers are recycled when a host thread exits. Every slot
// releases the Global Guest Lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <deque>
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
  static constexpr std::uint32_t kMaxThreads = 256;       ///< host threads with a local table at once (recycled)
  static constexpr std::uint32_t kMaxLocals = 1u << 14;   ///< local slots per thread (live, or stale in a frame)
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

  /// Diagnostics: one line per Java method call, until `budget` runs out. Set once at boot, before any guest
  /// thread runs; null in tests. A loop that keeps calling the same method names itself here.
  using CallTrace = std::function<void(const std::string& line)>;
  static void SetCallTrace(CallTrace trace, int budget);

  /// Guest tid -> the Linux tid of the host thread bound to it (0 when nothing is bound). Set once at boot:
  /// `android.os.Process.setThreadPriority` is given a Linux tid, and the guest's tids are the engine's.
  using GuestTidResolver = std::function<std::int32_t(std::int32_t guest_tid)>;
  static void SetGuestTidResolver(GuestTidResolver resolver);

  /// A real local reference made into a guest local in the calling thread's current frame (0 stays 0).
  GuestAddr ToGuestLocal(HostRef r, std::string* err = nullptr);
  /// The real reference behind a guest handle (0 for null), or 0 with `err` set when it is not a live handle
  /// this bridge handed to this thread.
  HostRef FromGuest(GuestAddr handle, std::string* err);
  /// One frame per inbound native call: PushFrame on entry, PopFrame on return, which gives the real reference
  /// behind `result` (the native's return value) before the frame's guest locals go.
  void PushFrame();
  HostRef PopFrame(GuestAddr result, std::string* err = nullptr);
  /// Called once the native's arguments (`this` or its class, then its references) are in the new frame. ART
  /// reads a native's arguments from the call's stack, unchecked without CheckJNI, so a game that keeps one past
  /// the native's return (Gumball's `this` in a static, overwritten by a nested native's) still gets its object:
  /// when the frame pops, each argument slot keeps a weak global ("ghost") its old handle resolves to while the
  /// object lives, one per slot.
  void ArgumentsIn();

  /// Guest memory for the copies JNI hands out (Get*Chars, Get*ArrayElements, the criticals): `bytes` of
  /// readable+writable guest memory, or 0 with `err`. Called without the GGL (the callee takes it to map). The
  /// bridge carves its blocks out of the pages it gets and reuses them; it never gives pages back.
  using GuestAlloc = std::function<GuestAddr(std::uint32_t bytes, std::string* err)>;
  /// A host entry point for a guest native a guest `RegisterNatives` binds (the runtime's trampoline pool).
  using NativeBinder =
      std::function<void*(GuestAddr guest_fn, const std::string& name, const std::string& sig, std::string* err)>;
  /// Both set once, before any guest call.
  void set_allocator(GuestAlloc alloc) { alloc_ = std::move(alloc); }
  void set_native_binder(NativeBinder binder) { binder_ = std::move(binder); }
  /// The guest `JavaVM*` (GetJavaVM answers it; the runtime passes it to a guest `JNI_OnLoad`).
  [[nodiscard]] GuestAddr vm() const { return where_ + kVmOffset; }
  /// Named observations that are not refusals (a call made with an exception pending, a sanitized string, an
  /// unsupported entry answered as ART answers it), each once, at most kMaxNotes.
  [[nodiscard]] std::vector<std::string> notes() const;

  [[nodiscard]] std::size_t live_locals() const { return shared_->live.load(std::memory_order_relaxed); }
  [[nodiscard]] std::size_t live_globals() const;
  /// M4 host-memory window service: merges every small direct-buffer shadow with its host memory (block-wise, three-way
  /// against the last synced copy, so each side keeps the other's writes). Run around every JNI transition; a no-op while
  /// there is no such shadow.
  void SyncShadows();
  [[nodiscard]] std::size_t refusals() const { return refusals_.load(std::memory_order_relaxed); }

 private:
  /// What outlives the bridge for a thread's table: the thread-number pool and the live count.
  struct Shared {
    std::mutex mu;
    std::vector<std::uint32_t> free_threads;
    std::uint32_t next_thread = 0;
    std::atomic<std::size_t> live{0};
    std::vector<HostRef> orphans;  ///< ghosts of exited threads' tables, for the bridge to delete (under mu)
  };
  struct LocalTable {
    std::shared_ptr<Shared> shared;
    std::uint32_t thread = 0;
    std::vector<HostRef> refs;          ///< by slot; 0 = free or deleted
    std::vector<std::uint8_t> serials;  ///< by slot, never shrinks: the serial of the slot's current local
    std::vector<std::size_t> frames;    ///< the refs.size() at each PushFrame
    std::vector<std::vector<std::uint32_t>> free;  ///< per frame (+ the base): deleted slots of that frame
    std::vector<std::size_t> args_end;  ///< per frame: the refs.size() once its native's arguments were in
    std::vector<HostRef> ghosts;        ///< by slot: a weak global to the argument a popped frame held there, or 0
    std::vector<std::uint8_t> ghost_serials;  ///< by slot: the serial that argument's handle carries
    ~LocalTable();
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
  /// Resolves `android.os.Process.setThreadPriority`'s two ids once (guest-tid translation, see SetGuestTidResolver).
  void EnsureProcessPriorityIds();
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
  bool StringEntry(std::uint32_t index, exec::MachineState& s, std::string* err);
  bool ArrayEntry(std::uint32_t index, exec::MachineState& s, std::string* err);
  bool OtherEntry(std::uint32_t index, exec::MachineState& s, std::string* err);
  bool VmSlot(std::uint32_t index, exec::MachineState& s, std::string* err);
  void Note(const std::string& note);
  /// A guest-initiated Java call left a pending exception: when it is a SecurityException thrown from inside one of
  /// the platform's device-identifier reads, clears it and returns true (the guest is answered null). Any other
  /// exception is put back exactly as it was.
  bool MaskIdentifierRefusal(const std::string& called);
  bool ThrowNamed(const char* cls, const std::string& message);

  /// A copy JNI handed out, by its guest address: what Release has to put back, and how big it is.
  struct Copy {
    HostRef pinned = 0;  ///< a global ref to the array or string, held until the final release
    char kind = 0;       ///< the array's element kind; 0 for a string copy
    std::int32_t length = 0;
    std::uint32_t bytes = 0;
  };
  GuestAddr Alloc(std::uint32_t bytes, std::string* err);
  void Free(GuestAddr at);
  GuestAddr CopyOut(const void* data, std::uint32_t bytes, const Copy& rec, std::string* err);

  static constexpr std::uint32_t kVmOffset = 0x400;  ///< the JavaVM word; its table follows at +0x10
  static constexpr std::uint32_t kVmEntries = 8;
  static constexpr std::size_t kMaxNotes = 64;

  /// `android.os.Process.setThreadPriority`'s ids, resolved once: the calls whose tid is translated.
  HostId process_priority_1_ = 0;
  HostId process_priority_2_ = 0;
  std::mutex process_ids_mu_;
  bool process_ids_ready_ = false;

  mem::GuestMemory& memory_;
  AbiContext& abi_;
  HostJava& java_;
  GuestAddr where_ = 0;
  bool ok_ = false;
  std::string err_;
  const std::uint64_t serial_;  ///< distinguishes instances in the per-thread local-table registry

  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
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
  std::deque<IdRec> ids_;  ///< a deque: a record never moves when another is added, so Id()'s pointer stays valid unlocked
  std::map<std::pair<HostId, bool>, std::size_t> id_index_;

  GuestAlloc alloc_;
  NativeBinder binder_;
  std::mutex arena_mu_;  ///< leaf: never held across alloc_ (which takes the GGL) — see Alloc
  std::map<GuestAddr, std::uint32_t> block_size_;            ///< live blocks: rounded size
  std::map<std::uint32_t, std::vector<GuestAddr>> free_by_size_;
  GuestAddr chunk_next_ = 0, chunk_end_ = 0;
  std::map<GuestAddr, Copy> copies_;
  /// A Java direct buffer over host memory, seen by the guest through a copy in its window.
  static constexpr std::int64_t kSmallShadow = 1 << 20;  ///< up to 1 MiB: merged at every transition
  struct Shadow {
    std::uint64_t capacity = 0;
    GuestAddr guest = 0;
    std::vector<std::uint8_t> snap;  ///< last synced bytes (small buffers only: those are merged at every transition)
  };
  std::mutex shadows_mu_;  ///< leaf
  std::map<void*, Shadow> shadows_;  ///< by host address
  std::atomic<unsigned> small_shadows_{0};
  mutable std::mutex notes_mu_;  ///< leaf
  std::vector<std::string> notes_;
};

}  // namespace rb::abi
