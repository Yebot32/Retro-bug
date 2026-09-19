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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

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

  [[nodiscard]] bool ok() const { return err_.empty(); }
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

  void InterpreterFallback(GuestAddr pc, std::size_t num_instructions) override;
  void CallSVC(std::uint32_t swi) override;
  void ExceptionRaised(GuestAddr pc, Dynarmic::A32::Exception exception) override;

  void AddTicks(std::uint64_t ticks) override;
  std::uint64_t GetTicksRemaining() override;

 private:
  void SyncIn(const MachineState& state);
  void SyncOut(MachineState& state);
  [[nodiscard]] RunResult MapHalt(Dynarmic::HaltReason hr, unsigned executed);

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

  /**
   * True when `pc` is a non-code destination this tier must surface rather than execute: the call
   * sentinel, or a host entry point. Called from *both* the code-read path and the fetch-fault path,
   * because Dynarmic caches the block it builds for such an address and afterwards never asks for its
   * code again — see MemoryReadCode.
   */
  bool SurfaceNonCodeDestination(GuestAddr pc);

  mem::GuestMemory& memory_;
  GuestAddr return_address_;
  const HostDispatch* dispatch_;
  std::unique_ptr<Dynarmic::A32::Jit> jit_;
  std::unique_ptr<InterpExecutor> fallback_interp_;
  std::string err_;

  MachineState* live_ = nullptr;  ///< the caller's state, for the duration of one Run
  unsigned budget_ = 0;
  unsigned executed_ = 0;
  Pending pending_ = Pending::None;
  std::string pending_detail_;
  std::uint32_t pending_slot_ = 0;
  GuestAddr pending_pc_ = 0;  ///< the address that caused the stop, before Dynarmic advanced past it
};

}  // namespace rb::exec
