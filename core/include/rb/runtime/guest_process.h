/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::GuestProcess — the persistent guest process (M2).
 *
 * One guest address space with the device's own 32-bit bionic `libc.so` booted in it the way a device's
 * dynamic linker leaves a process before `main`: the scope loaded with its IFUNC slots resolved, the
 * linker's `__loader_shared_globals` answered (with the auxv and the static-TLS layout in it), a real main
 * thread — bionic's `pthread_internal_t` + `bionic_tcb` + `bionic_tls`, linked as the sole entry of
 * `g_thread_list`, the thread pointer set — and constructors run in bionic's order. It owns the memory, the
 * bridge (`AbiContext`), the syscall monitor, the guest `JNIEnv` and one executor per tier.
 *
 * What it leaves to its caller: every host-provided symbol beyond the linker's own shared globals (the
 * launcher's GL/dl/log slots reach it through `GuestProcessOptions::host_resolver`), files (the monitor's
 * provider), and reporting (`GuestProcessOptions::log` receives one line per fact worth a report line).
 *
 * The bionic layout it builds is a fact of one libc build. It is derived from that image's own code at
 * load (`VerifyBionicLayout`) and every fact is asserted; a different build is refused by name rather than
 * given a layout that only looks right.
 */
#pragma once

#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/abi/jni_env.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/futex_table.h"
#include "rb/syscall/monitor.h"

namespace rb::runtime {

using exec::GuestAddr;

struct GuestProcessOptions {
  /// The guest window. At least 32 MiB (the host-entry window sits at 12 MiB and the main stack at the top).
  std::size_t window_bytes = 64u << 20;
  /// `AddressModel::kBiased` when true, `kFlatIdentity` otherwise.
  bool biased = true;
  /// What an import nothing answers is bound to (a weak one gets 0, as on a device). 0: no trap — an
  /// unanswered strong import then fails the load by name. The page is reserved when inside the window.
  GuestAddr unresolved_trap = 0;
  /// Consulted after the scope (and earlier apps) for a name none of them defines. nullopt, or the trap
  /// address, means "not answered".
  std::function<std::optional<GuestAddr>(const std::string&)> host_resolver;
  /// One line per reportable fact (loads, IFUNC slots, unanswered imports, constructor failures).
  std::function<void(const std::string&)> log;
  /// The tier IFUNC resolvers, the main-thread setup calls and constructors run on: 1 = I, 2 = J.
  int ctor_tier = 2;
};

/// The static-TLS / thread layout of one bionic build, as `VerifyBionicLayout` derived and asserted it.
struct BionicLayout {
  std::uint32_t thread_size = 0;        ///< sizeof(pthread_internal_t)
  std::uint32_t tcb_size = 0;           ///< sizeof(bionic_tcb)
  std::uint32_t tp_offset_in_tcb = 0;   ///< TP = tcb + this (slot 0, TLS_SLOT_DTV)
  std::uint32_t bionic_tls_size = 0;    ///< sizeof(bionic_tls)
  std::uint32_t offset_bionic_tcb = 0;  ///< within static TLS: StaticTlsLayout::offset_bionic_tcb()
  std::uint32_t offset_bionic_tls = 0;  ///< within static TLS: StaticTlsLayout::offset_bionic_tls()
  std::uint32_t static_tls_size = 0;    ///< StaticTlsLayout::size() with no TLS module registered
  std::uint32_t auxv_field = 0;         ///< the shared-globals offset `getauxval` reads the auxv from
};

/// Derives the layout from `libc`'s own code (already placed in `mem`) and checks every fact against the
/// build this was measured on (SM-X710 / Android 16 `libc.so`). nullopt with a named `err` on any mismatch.
std::optional<BionicLayout> VerifyBionicLayout(const mem::GuestMemory& mem, const loader::GuestModule& libc,
                                               std::string* err);

class GuestProcess {
 public:
  /// The identity the monitor answers `getpid`/`gettid` with, and the main thread's `tid`.
  static constexpr std::int32_t kPid = 4242;
  static constexpr std::int32_t kMainTid = 4243;
  /// The return sentinel; the `AbiContext` slot table follows it at +0x1000 (a 0x2000 reservation).
  static constexpr GuestAddr kReturnAddress = 0x00c0'0000u;
  /// Nothing this process (or the monitor's `mmap2`) places is at or above this: the top of a 4 GiB
  /// window holds the runtime's trap (0xfffe0000) and the kuser helpers (0xffff0f60..).
  static constexpr GuestAddr kAllocCeiling = 0xfff0'0000u;
  static constexpr std::uint32_t kMainStackBytes = 8u << 20;
  static constexpr unsigned kCallBudget = 4'000'000;
  static constexpr unsigned kJniArenaBytes = 1u << 20;

  static std::unique_ptr<GuestProcess> Create(const GuestProcessOptions& options, std::string* err);
  ~GuestProcess();
  GuestProcess(const GuestProcess&) = delete;
  GuestProcess& operator=(const GuestProcess&) = delete;

  /// Loads libc (required), libm, libz and — when non-empty — libstdc++, in that order, plays the
  /// linker's part (shared globals, auxv, static-TLS fields, main thread) and leaves constructors unrun.
  /// Once per process. Binds the main guest thread to the calling host thread (M2 Task 3, Ruling 1).
  bool LoadScope(const std::vector<std::uint8_t>& libc, const std::vector<std::uint8_t>& libm,
                 const std::vector<std::uint8_t>& libz, const std::vector<std::uint8_t>& libstdcxx,
                 std::string* err);

  /// Loads an app library against the scope (then earlier apps, then the host resolver). Its constructors
  /// run at the next `RunConstructors`. The pointer stays valid for the process's lifetime.
  const loader::GuestModule* LoadApp(const std::vector<std::uint8_t>& image, const std::string& soname,
                                     std::string* err);

  /// Every module whose constructors have not run, in load order (scope, then apps): `DT_INIT`, then
  /// `DT_INIT_ARRAY`, as bionic's `soinfo::call_constructors`. Stops at the first that does not return
  /// and names it (with the stop reason and the callee's pc) in `failed`. That module is then marked
  /// faulted: a later call refuses by name rather than rerunning its constructors that already ran (a
  /// half-initialized module must not be initialized twice), and runs nothing after it.
  bool RunConstructors(std::string* failed);

  /// A page-aligned free run of the window below `kAllocCeiling`, mapped with `prot`. 0 and `err` when none.
  GuestAddr MapFree(std::size_t bytes, unsigned prot, std::string* err);

  /// The address an import of `name` binds to: the first loaded module that defines it (an IFUNC export
  /// resolved through its resolver), nullopt when none does. The host resolver is not consulted.
  std::optional<GuestAddr> Resolve(const std::string& name);

  mem::GuestMemory& memory() { return *mem_; }
  abi::AbiContext& abi() { return *abi_; }
  syscall::Monitor& monitor() { return *monitor_; }
  abi::JniEnv& jni() { return *jni_; }
  /// The process's Global Guest Lock (owned by the bridge) and its main guest thread, which `LoadScope` binds to
  /// the host thread that called it.
  runtime::Ggl& ggl() { return abi_->ggl(); }
  runtime::GuestThread& main_guest_thread() { return abi_->main_thread(); }
  /// 1 = Tier I, 2 = Tier J (anything else is Tier I).
  exec::Executor& engine(int tier);
  const loader::GuestModule* libc() const { return libc_; }

  const BionicLayout& layout() const { return layout_; }
  GuestAddr shared_globals() const { return globals_; }
  GuestAddr loader_stub() const { return stub_; }
  GuestAddr auxv() const { return auxv_; }
  GuestAddr at_random() const { return at_random_; }
  GuestAddr main_thread() const { return thread_; }
  GuestAddr thread_pointer() const { return tp_; }
  GuestAddr stack_top() const { return stack_top_; }
  std::size_t constructors_run() const { return ctors_run_; }
  /// Every loaded module, in load order (scope, then apps) — what a linker's soinfo walk sees
  /// (`dl_unwind_find_exidx`, a backtrace).
  const std::vector<std::unique_ptr<loader::GuestModule>>& modules() const { return modules_; }
  /// Imports (scope and apps) nothing answered, each once, in first-asked order.
  const std::vector<std::string>& unanswered() const { return unanswered_; }

  // ---- guest threads (M2 Task 4)
  /// The process's futex waiters (the monitor's `futex`, thread exits' clear-child-tid wakes).
  FutexTable& futex() { return futex_; }
  /// Host threads started by guest `clone` that have not finished yet (a thread is finished once its guest
  /// thread called `exit`, faulted, or stopped with the process).
  [[nodiscard]] std::size_t running_threads() const;
  /// Guest threads ever started by `clone`.
  [[nodiscard]] std::size_t threads_started() const { return threads_started_.load(std::memory_order_relaxed); }
  /// Waits (without the GGL — never call it while holding it) until no `clone`d host thread is running, up to
  /// `limit`. True when none is.
  bool WaitForThreads(std::chrono::milliseconds limit);
  /// One line per `clone`d guest thread that stopped other than through `exit` (a fault, a refused call):
  /// such a stop ends the whole process, as a fatal signal in one thread does on a device.
  [[nodiscard]] std::vector<std::string> thread_faults() const;
  /// The budget one `RunThread` turn of a `clone`d thread gets (it is re-entered until the thread exits).
  static constexpr unsigned kThreadTurnBudget = 1u << 30;

 private:
  GuestProcess() = default;
  /// The monitor's `ThreadSpawner`: adopts `t` and starts its host thread (called with the GGL held).
  std::string Spawn(std::unique_ptr<GuestThread> t);
  struct HostThread {
    std::unique_ptr<GuestThread> guest;
    std::thread host;
    std::atomic<bool> done{false};
  };
  void ThreadMain(HostThread* rec);
  /// Joins and drops records whose host thread finished. `threads_mu_` held.
  void ReapLocked();
  /// Stops every guest thread (process teardown) and joins their host threads. Never with the GGL held.
  void StopThreads();

  void Log(const std::string& line) const;
  GuestAddr FindFree(std::size_t bytes) const;
  loader::GuestModule* LoadOne(const std::vector<std::uint8_t>& image, const std::string& soname,
                               bool scope, std::string* err);
  std::optional<std::uint32_t> ResolveImport(const std::string& name);
  std::optional<std::uint32_t> ExportOf(const loader::GuestModule& m, const std::string& name);
  std::optional<std::uint32_t> RunIfunc(const loader::IfuncSlot& slot);
  bool PublishAuxv(std::string* err);
  bool SetUpMainThread(std::string* err);
  bool CallSetup(const char* fn, const abi::Args32& args, std::string* err);

  GuestProcessOptions opts_;
  // Declaration order is destruction order reversed: the executors and the JNIEnv reference the bridge
  // and the memory, so they go first. Guest threads are stopped and joined in the destructor's body,
  // before any member goes.
  FutexTable futex_;
  std::unique_ptr<mem::GuestMemory> mem_;
  std::unique_ptr<abi::AbiContext> abi_;
  std::unique_ptr<syscall::Monitor> monitor_;
  std::unique_ptr<abi::JniEnv> jni_;
  std::unique_ptr<exec::InterpExecutor> interp_;
  std::unique_ptr<exec::JitExecutor> jit_;
  std::vector<std::unique_ptr<loader::GuestModule>> modules_;  ///< load order: scope, then apps
  loader::GuestModule* libc_ = nullptr;
  bool scope_loaded_ = false;
  BionicLayout layout_{};
  GuestAddr ceiling_ = 0;
  GuestAddr stack_top_ = 0;
  GuestAddr globals_ = 0, stub_ = 0, auxv_ = 0, at_random_ = 0;
  GuestAddr thread_ = 0, tp_ = 0, thread_mapping_ = 0;
  std::uint32_t thread_mapping_size_ = 0;
  std::map<std::string, std::uint32_t> export_ifunc_cache_;
  /// Modules whose constructors stopped part-way, with the reason RunConstructors named.
  std::map<const loader::GuestModule*, std::string> ctor_faults_;
  std::vector<std::string> unanswered_;
  std::size_t ctors_run_ = 0;
  /// `clone`d threads. `threads_mu_` is a leaf lock: never held while waiting for the GGL or joining a thread
  /// that might need it.
  mutable std::mutex threads_mu_;
  std::list<std::unique_ptr<HostThread>> threads_;
  std::vector<std::string> thread_faults_;
  std::atomic<std::size_t> threads_started_{0};
};

}  // namespace rb::runtime
