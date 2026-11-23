/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — Tier J, the AArch64 JIT, over Dynarmic's own `A32::Jit`.
 *
 * The relationship between the two tiers is the whole point of AD-1, so this file is written to be
 * *compared* with `interp.cpp` rather than to be faster than it:
 *
 *   - One `MachineState`, in and out. `SyncIn`/`SyncOut` are the only place guest state crosses, and the
 *     two tiers must agree on it bit for bit — that equivalence is docs/20 §C02's exit criterion, and it
 *     is meaningless if either tier is allowed to read state through a private back door.
 *   - The instruction budget is not the IR-step count Tier I reports. Dynarmic's cycle-counting hooks are
 *     the natural place (its `GetTicksForCode` returns one tick per guest encoding, which `rb::isa`
 *     already fixes), so Tier J's `instructions_executed` really is guest instructions. **The two numbers
 *     are different units and the differential says so** rather than quietly comparing them.
 *   - No fastmem, and that is a measurement rather than a shortcut: docs/30 §Step 8 found that an
 *     installed Android app cannot reserve a contiguous 4 GiB low window at all, so `fastmem_pointer` —
 *     which wants exactly that — is unavailable on the shipping platform. Every access goes through the
 *     callbacks below, and the fast path that replaces them is `UserConfig::page_table`, which is
 *     per-1 MiB-block and therefore survives a biased guest base. Wiring that up is a speed change with
 *     no correctness claim, deliberately not made here.
 *
 * `Jit::Run` is documented as not recursively callable, so this tier cannot do what the call bridge does
 * with Tier I (nest a guest→host→guest sequence inside one `Run`). Host calls therefore *exit* the JIT via
 * `HaltExecution`, which is also why they have to be resumable from `MachineState` alone.
 */
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::exec {

class InterpExecutor;

/**
 * Tier J: Dynarmic's AArch64 back end, driven as one `Executor`.
 *
 * It is also the front end's `UserCallbacks`, because those are the same object's two faces: the
 * translation needs to read guest memory while it translates, and the compiled code needs to call back
 * for the accesses fastmem cannot serve. Keeping them in one class is what makes "the JIT and the
 * interpreter see the same memory" a structural fact rather than a discipline.
 */
class JitExecutor : public Executor, public Dynarmic::A32::UserCallbacks {
 public:
  /// `dispatch` may be null, in which case a host entry point is unreachable and a guest branch to one
  /// is a fault like any other unmapped fetch — the shape the ISA/census tests use.
  JitExecutor(mem::GuestMemory& memory, GuestAddr return_address,
              const HostDispatch* dispatch = nullptr);
  ~JitExecutor() override;

  RunResult Run(MachineState& state, unsigned max_instructions) override;

  /// The JIT owns a real code cache, so this is the first place `Invalidate` has to actually do
  /// something: self-modifying guest code is detected by a write to a range with compiled code in it.
  void Invalidate(GuestAddr from, GuestAddr to) override;

  CodeTier tier() const override { return CodeTier::Translate; }

  [[nodiscard]] bool ok() const override { return err_.empty(); }
  [[nodiscard]] const std::string& error() const { return err_; }

  // ---------------------------------------------------------- Dynarmic::A32::UserCallbacks
  //
  // Reads may be unaligned; a fault cannot be *returned* from these (the signature has no error), so the
  // record-and-halt pattern applies: note the address, ask the core to stop, hand back zero. Zero is not
  // a value the guest should see — `check_halt_on_memory_access` is on, so the core re-tests the halt
  // flag after every data access and does not get to use it.

  /// 4-byte aligned; the sentinel and host-entry checks happen here because this is the only callback that
  /// is asked for a *destination* rather than for data. Returning nullopt ends the block, which is how a
  /// non-executable fetch is reported to the translator.
  std::optional<std::uint32_t> MemoryReadCode(GuestAddr vaddr) override;

  std::uint8_t MemoryRead8(GuestAddr vaddr) override;
  std::uint16_t MemoryRead16(GuestAddr vaddr) override;
  std::uint32_t MemoryRead32(GuestAddr vaddr) override;
  std::uint64_t MemoryRead64(GuestAddr vaddr) override;
  void MemoryWrite8(GuestAddr vaddr, std::uint8_t value) override;
  void MemoryWrite16(GuestAddr vaddr, std::uint16_t value) override;
  void MemoryWrite32(GuestAddr vaddr, std::uint32_t value) override;
  void MemoryWrite64(GuestAddr vaddr, std::uint64_t value) override;

  /**
   * The compare-and-store half of an exclusive store, on top of Dynarmic's reservation check.
   *
   * The base class defaults all four to `return false`, which is a *silently always-failing* STREX: the
   * guest's `ldrex/strex` retry loop would spin forever and the run would end as an exhausted budget
   * with no instruction named. Overriding them is therefore mandatory, not optional polish.
   *
   * Dynarmic has already decided the reservation was address-valid (`ExclusiveMonitor::CheckAndClear`)
   * and hands us the value the exclusive load saw as `expected`. Comparing it is our job, and it is the
   * part that carries the atomicity: a plain store to the same word between the load and this callback
   * changes what is there, so `current != expected`, and the store must fail with `Rd = 1` the way
   * hardware does it. Returning true without comparing would turn every lost-update race into a silent
   * write.
   *
   * The marker Dynarmic keeps in its own state struct is cleared by the back end before this callback
   * runs, so a failed store also clears it -- which matches ARM, where any executed STREX drops the local
   * marker and the guest has to re-execute the load.
   */
  bool MemoryWriteExclusive8(GuestAddr vaddr, std::uint8_t value, std::uint8_t expected) override {
    return ExclusiveCompareAndStore(vaddr, value, expected, 1);
  }
  bool MemoryWriteExclusive16(GuestAddr vaddr, std::uint16_t value, std::uint16_t expected) override {
    return ExclusiveCompareAndStore(vaddr, value, expected, 2);
  }
  bool MemoryWriteExclusive32(GuestAddr vaddr, std::uint32_t value, std::uint32_t expected) override {
    return ExclusiveCompareAndStore(vaddr, value, expected, 4);
  }
  bool MemoryWriteExclusive64(GuestAddr vaddr, std::uint64_t value, std::uint64_t expected) override {
    return ExclusiveCompareAndStore(vaddr, value, expected, 8);
  }

  void InterpreterFallback(GuestAddr pc, std::size_t num_instructions) override;
  void CallSVC(std::uint32_t swi) override;
  void ExceptionRaised(GuestAddr pc, Dynarmic::A32::Exception exception) override;

  void AddTicks(std::uint64_t ticks) override;
  std::uint64_t GetTicksRemaining() override;

  /**
   * Vetoes the six ARM-state instructions Dynarmic declines to translate, before it can ask for them.
   *
   * Their translators end in `InterpretThisInstruction()`, and on an ARM64 host the resulting
   * `IR::Term::Interpret` is not a hand-back to Tier I: the AArch64 back end answers it with
   * `ASSERT_FALSE("Interpret should never be emitted.")`, which is not `NDEBUG`-gated, so the process
   * dies inside `Run()`. Measured, and the enumeration is `docs/32` §7; see `docs/30` §Step 20a.
   *
   * This is the one place that stop can be turned into a reason, because the hook is called before the
   * word is decoded and the embedder — here, explicitly — owns the block terminal when it declines to
   * translate. Returns false to preclude, after asking for a halt and setting `CheckHalt`, which is what
   * makes the emitted code observe the stop rather than run on past the instruction.
   */
  bool PreCodeReadHook(bool is_thumb, GuestAddr pc, Dynarmic::A32::IREmitter& ir) override;

  // ---------------------------------------------------------- coprocessor 15, for Arm15Coprocessor
  //
  // These three exist because Dynarmic hands a coprocessor callback nothing but the `user_arg` pointer it
  // baked into the compiled code. They are not an interface for anyone else, and `Arm15Coprocessor` is the
  // only caller.
  /// One entry per distinct coprocessor register a guest actually touches, so the refusal can name the
  /// register rather than the class of thing it was.
  ///
  /// `user_arg` is compiled in as a raw address, so a site has to live as long as the block that mentions
  /// it — hence a `std::deque`, whose elements do not move when more are appended. In practice there is one
  /// entry: the device's `libc.so` reads TPIDRURW (`mrc p15, #0x0, rX, c13, c0, #0x3`) from 98 places and
  /// makes no other coprocessor access at all, which `llvm-objdump -d` over the whole image establishes.
  struct CoprocSite {
    JitExecutor* exec = nullptr;
    bool tp_read = false;   ///< answer from `MachineState::tp`
    bool tp_write = false;  ///< store into it
    std::string detail;     ///< what to say when the honest answer is "not modelled"
  };
  /// Returns the site for this shape of access, creating it once.
  CoprocSite* CoprocSiteFor(bool tp_read, bool tp_write, std::string detail);
  /// The live guest state, or null outside a `Run`. A compiled block cannot execute there, so a handler
  /// that sees null has found a bug rather than an edge case.
  MachineState* live_state() const { return live_; }
  /// Stops the core with `Unimplemented` naming `site`. Dynarmic's own answer to an unhandled coprocessor
  /// is `ASSERT_FALSE` in the AArch64 back end, which kills the host process; this is the reason that does
  /// not happen. See `docs/30` §Step 26.
  void RefuseCoprocessor(const CoprocSite& site);

 private:
  void SyncIn(const MachineState& state);
  void SyncOut(MachineState& state);
  [[nodiscard]] RunResult MapHalt(Dynarmic::HaltReason hr, unsigned executed);

  /// The body of all four `MemoryWriteExclusive*` callbacks: re-read the word, compare against what the
  /// exclusive load saw, and store only if nothing has changed it. A fault here stops the core the same
  /// way an ordinary access does; the `false` it returns is then irrelevant, because the guest never gets
  /// to act on the result.
  bool ExclusiveCompareAndStore(GuestAddr vaddr, std::uint64_t value, std::uint64_t expected,
                                std::size_t bytes);

  /// Why the core is being asked to stop, distinguished from `HaltReason` because several of RetroBug's
  /// reasons (a host call, a returned call, an unimplemented op) are ordinary outcomes that dynarmic has
  /// no vocabulary for.
  enum class Pending { None, Returned, HostCall, Syscall, Fault, Unimplemented, Suspended };

  /**
   * Records a stop reason, first one wins.
   *
   * The order matters and is the reason this is a helper rather than an assignment: detecting the call
   * sentinel happens in `MemoryReadCode`, which then returns `nullopt` to end the block — and Dynarmic
   * responds to a nullopt code read by raising `NoExecuteFault`. Assigning unconditionally let that
   * second, downstream callback overwrite `Returned` with `Fault`, so every guest call appeared to end
   * in a fault at an address one past the sentinel (dynarmic's own pc bookkeeping across the failed
   * fetch). The *first* reason a Run produced is the causal one; anything after it is the core cleaning
   * up after the stop we asked for.
   */
  void SetPending(Pending p, std::string detail = {}) {
    if (pending_ != Pending::None) return;
    pending_ = p;
    if (!detail.empty()) pending_detail_ = std::move(detail);
  }

  /// A guest access that could not be completed, with the address a SIGSEGV handler is entitled to as
  /// `si_addr`. First stop wins, like SetPending: a later callback from Dynarmic's cleanup must not
  /// replace the access that actually faulted.
  void SetFault(std::string detail, GuestAddr addr, bool write) {
    if (pending_ != Pending::None) return;
    pending_ = Pending::Fault;
    pending_detail_ = std::move(detail);
    pending_fault_addr_ = addr;
    pending_fault_has_addr_ = true;
    pending_fault_write_ = write;
  }

  /**
   * True when `pc` is a non-code destination this tier must surface rather than execute: the call
   * sentinel, or a host entry point. Called from *both* the code-read path and the fetch-fault path,
   * because Dynarmic caches the block it builds for such an address and afterwards never asks for its
   * code again — see MemoryReadCode.
   */
  bool SurfaceNonCodeDestination(GuestAddr pc);

  mem::GuestMemory& memory_;
  /// Dynarmic's page table (M4): the host address of every mapped, readable guest page whose backing is directly
  /// addressable, so compiled code loads and stores it inline; nullptr pages (unmapped, guard, out-of-line) take the
  /// checked callbacks, which name a fault. Kept current by a GuestMemory observer.
  std::vector<std::uint8_t*> page_table_;
  int page_observer_ = 0;
  std::atomic<std::uint64_t> translated_{0}, invalidations_{0};
  void RefreshPages(std::uint32_t at, std::size_t len);

 public:
  /// Pages compiled code reaches inline (a test hook).
  [[nodiscard]] std::size_t inline_pages() const;
  /// Diagnostics: guest instructions translated (compiled) and cache invalidations, since construction.
  [[nodiscard]] std::uint64_t translated() const { return translated_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t invalidations() const { return invalidations_.load(std::memory_order_relaxed); }

 private:
  GuestAddr return_address_;
  const HostDispatch* dispatch_;

  /**
   * Dynarmic's global exclusive monitor, for the one core this object is.
   *
   * It is not optional: the AArch64 back end builds a trampoline for every exclusive access that calls
   * `conf.global_monitor->ReadAndMark(...)` / `->DoExclusiveOperation(...)` with no null check
   * (`backend/arm64/a32_address_space.cpp:81,136`), and `UserConfig::global_monitor` defaults to
   * `nullptr`. So before this member existed, decoding an `ldaex`/`ldrex` was a host-process null
   * dereference rather than a guest-visible failure -- which is also why Dynarmic's own test harness
   * supplies one (`tests/A32/testenv.h`).
   *
   * Declared before `jit_` deliberately: members are destroyed in reverse declaration order, so this has
   * to outlive the compiled code cache that holds its address.
   *
   * Per-executor, and `processor_id` 0 within it, because an executor *is* a core. Two guest threads
   * therefore have two local markers, which is architecturally right; the atomicity that a *shared*
   * monitor would provide comes instead from the compare against `expected` in
   * `MemoryWriteExclusive*`, so a race between two threads on the same word still loses exactly one
   * update. What this arrangement cannot do is fail an STREX whose target another core wrote but left
   * holding the same value -- an ABA that real hardware would also usually let through, since its
   * marker is address-granular too.
   */
  Dynarmic::ExclusiveMonitor monitor_{1};
  /// Coprocessor 15, registered so that it is *never* absent. See `Arm15Coprocessor` for why an empty
  /// slot is a host-process abort rather than a guest-visible failure.
  std::shared_ptr<Dynarmic::A32::Coprocessor> coproc15_;
  /// Coprocessor numbers 0-14, each a name-refusing object rather than an empty config slot. An empty
  /// slot is not a refusal — the back end compiles the access into `ASSERT_FALSE` and the host process
  /// dies — and these must outlive the code cache the `Jit` below builds around them, which is why
  /// they are declared *above* it (members destruct in reverse declaration order).
  std::array<std::shared_ptr<Dynarmic::A32::Coprocessor>, 15> refusing_coprocs_{};
  /// The per-access sites the coprocessor callbacks point at, declared above the `Jit` for the same
  /// reason: the code cache holds raw pointers into this deque.
  std::deque<CoprocSite> coproc_sites_;
  std::unique_ptr<Dynarmic::A32::Jit> jit_;
  std::unique_ptr<InterpExecutor> fallback_interp_;
  std::string err_;

  MachineState* live_ = nullptr;  ///< the caller's state, for the duration of one Run
  unsigned budget_ = 0;
  unsigned executed_ = 0;
  Pending pending_ = Pending::None;
  std::string pending_detail_;
  std::uint32_t pending_slot_ = 0;
  std::uint32_t pending_svc_ = 0;  ///< the SVC immediate, when `pending_` is `Syscall`
  GuestAddr pending_pc_ = 0;  ///< the address that caused the stop, before Dynarmic advanced past it
  GuestAddr pending_fault_addr_ = 0;
  bool pending_fault_has_addr_ = false;
  bool pending_fault_write_ = false;
  /// Set when the raise was an architecturally undefined encoding (SIGILL), with that instruction's address:
  /// the stop becomes a `Fault` carrying `illegal_instruction`, which a monitored process delivers to the
  /// guest's SIGILL handler. Any other raise stays `Unimplemented` (a gap in this engine, not a guest-visible trap).
  bool pending_illegal_ = false;
  GuestAddr pending_illegal_pc_ = 0;
};

}  // namespace rb::exec
