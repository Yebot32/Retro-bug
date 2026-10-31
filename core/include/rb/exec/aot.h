/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — the AOT tier: offline-emitted AArch64 blocks behind the same `Executor` seam.
 *
 * The shape is deliberately the analogue of `JitExecutor` (jit.h), because the emitted code is
 * Dynarmic's own AArch64 code: the same `A32JitState` layout, the same prelude logic, the same
 * callbacks and the same `HaltReason` vocabulary. What differs is where the bytes come from — a
 * `rb_aotgen`-generated `.S` linked into the binary, never generated at runtime — and how a block
 * that was not translated is handled: the dispatcher hands the PC back to C++ as a fallback, and
 * Tier I executes it (jit.cpp's `InterpreterFallback` shape) before the AOT code resumes.
 *
 * There is no code generator and no executable allocation in this file. `AotImage` is a sorted table,
 * `AotExecutor` is a dispatcher plus the callback bodies the emitted prelude's trampolines call.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "rb/exec/interp.h"
#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::exec {

/// The vendored state struct the emitted blocks were compiled against. Named here so the public
/// interface says which ABI it is; the static asserts in aot.cpp compare every offset the prelude
/// and the block emitters touch against this type rather than against a hand-written copy.
using A32JitState = Dynarmic::Backend::Arm64::A32JitState;

struct AotState;
class AotExecutor;

/// One translated guest block: the code-map offset it starts at, the ISA, and its entry point.
///
/// `entry` is never called from C++. An emitted block is entered with Dynarmic's register ABI
/// (X28 = `A32JitState*`, X27 = halt pointer, X26 = ticks); the dispatcher returns the pointer to
/// the prelude, which branches to it. The function-pointer type is the table's way of naming a code
/// address without inventing a second pointer type.
using AotCode = std::uint32_t (*)(AotState*);

/// The ISA bits in `AotBlock::flags`; exactly one is set. They match `rb::aotgen::kThumb`/`kArm` by
/// value and by intent, but the runtime does not depend on the tool's header.
enum : std::uint8_t { kAotThumb = 1, kAotArm = 2 };

struct AotBlock {
  std::uint32_t guest_off = 0;  ///< file-relative to `.text`, the code map's unit
  std::uint8_t flags = 0;       ///< kAotThumb or kAotArm
  AotCode entry = nullptr;      ///< assembly label from `aot_<game>.S`
};

/**
 * The dispatch environment.
 *
 * `jit` is first and the rest is dispatcher bookkeeping, so `&state->jit == state`: the prelude's
 * X28 (which blocks index through `A32JitState` offsets) is also the `AotState*` the callback
 * trampolines put in X0. That is what makes the generated trampolines able to find their callbacks
 * without baking a host address, and it is asserted in aot.cpp.
 *
 * `halt_reason` is the `volatile u32*` the prelude receives as its third argument. Callbacks set
 * bits in it with release semantics; the prelude reads with LDAR and clears with LDAXR/STLXR, the
 * same protocol Dynarmic's `A32::Jit` uses for its own `halt_reason`.
 */
struct AotState {
  A32JitState jit;
  volatile std::uint32_t halt_reason = 0;
  AotExecutor* exec = nullptr;
  /// Set by the dispatcher when the target has no emitted block. The executor reads it after the
  /// prelude returns from run code, runs Tier I from `fallback_pc`, then resumes AOT.
  std::uint32_t fallback_pc = 0;
  std::uint32_t fallback_pending = 0;
};

/// The hand-off from a generated prelude back into C++: the AArch64 ARM64 AAPCS signature of
/// Dynarmic's `PreludeInfo::RunCodeFuncType`, so the table can store the assembly symbol directly.
using AotRunCode = Dynarmic::HaltReason (*)(void* entry_point, void* jit_state,
                                            volatile std::uint32_t* halt_reason);

/**
 * Everything one generated table knows about its image.
 *
 * `text_vaddr` and `load_bias` are the two facts the runtime needs that a discovery `CodeMap` does
 * not carry: the emitted blocks baked guest PCs as `load_bias + text_vaddr + off`, so the image must
 * be mapped at that same bias (the test asks the loader for it with `LoadOptions::hint`). `text_len`
 * bounds `Resolve`'s offsets. `fingerprint` is FNV-1a-64 over the exact `.so` the tool read; the test
 * recomputes it so a stale table fails loudly instead of comparing two different byte streams.
 */
struct AotImageInfo {
  const AotBlock* blocks = nullptr;
  std::size_t count = 0;
  std::uint32_t text_vaddr = 0;
  std::uint32_t text_len = 0;
  std::uint32_t load_bias = 0;
  std::uint64_t fingerprint = 0;
  AotRunCode run_code = nullptr;
};

/**
 * One translated `.so`: a sorted `{guest_off, thumb, entry}` table (binary search).
 *
 * `Load` takes the block array, its length and the generated `AotImageInfo`. The plan's sketch named
 * a `CodeMap*` here; that type lives in `tools/aotgen` and carries neither `text_vaddr` nor
 * `load_bias` nor the prelude symbol, so the generated table is the useful input. The block array is
 * the discovered map (each `AotBlock` is a code-map entry plus its emitted entry), plus the interior
 * targets the emitter reached and translated so the fixture run can resolve every branch.
 */
class AotImage {
 public:
  bool Load(const AotBlock* blocks, std::size_t n, const AotImageInfo* info, std::string* err);
  /// nullptr means "no translated block at this offset" — the dispatcher's Tier I fallback.
  AotCode Resolve(std::uint32_t guest_off, bool thumb) const;

  [[nodiscard]] const AotImageInfo& info() const { return info_; }
  [[nodiscard]] std::size_t size() const { return rows_.size(); }

 private:
  struct Row {
    std::uint32_t off = 0;
    std::uint8_t flags = 0;
    AotCode entry = nullptr;
  };
  std::vector<Row> rows_;
  AotImageInfo info_{};
};

/**
 * The AOT executor: one `run_code` entry, one block table, the callback bodies, and the fallback.
 *
 * The `Run` contract matches `JitExecutor::Run` field for field: same `RunResult` reasons, same
 * `instructions_executed` unit (guest instructions, from the prelude's tick accounting), same
 * `pending_pc` adjustment and the same first-reason-wins fault recording. A miss in the block table
 * is not a failure: it is a counted fallback to the existing Tier I executor, after which AOT
 * resumes from the PC Tier I stopped at.
 */
class AotExecutor final : public Executor {
 public:
  AotExecutor(mem::GuestMemory& memory, GuestAddr return_address, const AotImageInfo& info,
              const HostDispatch* dispatch = nullptr);
  ~AotExecutor() override;

  RunResult Run(MachineState& state, unsigned max_instructions) override;
  /// AOT code is immutable and mapped read-execute; self-modifying guest code is Task 6's problem.
  void Invalidate(GuestAddr from, GuestAddr to) override;
  CodeTier tier() const override { return CodeTier::Translate; }
  [[nodiscard]] bool ok() const override { return err_.empty(); }
  [[nodiscard]] const std::string& error() const { return err_; }

  /// Fallback hand-backs since construction. Zero for a fully covered run; the fixture asserts it.
  [[nodiscard]] std::size_t fallback_count() const { return fallbacks_; }
  [[nodiscard]] const AotImage& image() const { return image_; }
  [[nodiscard]] AotState& state() { return state_; }
  /// Guest address the code map's offset 0 corresponds to (`load_bias + text_vaddr`).
  [[nodiscard]] std::uint32_t text_base() const { return text_base_; }

  // ---------------------------------------------------------------- callbacks the prelude calls
  //
  // These are the bodies behind the generated trampolines. Their observable behaviour mirrors
  // JitExecutor's UserCallbacks methods one for one (same details, same zero return, same halt bit),
  // because a fault in AOT code has to reach a caller exactly as the JIT's does.
  std::uint8_t MemoryRead8(std::uint32_t vaddr);
  std::uint16_t MemoryRead16(std::uint32_t vaddr);
  std::uint32_t MemoryRead32(std::uint32_t vaddr);
  std::uint64_t MemoryRead64(std::uint32_t vaddr);
  void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value);
  void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value);
  void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value);
  void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value);
  std::uint8_t ExclusiveRead8(std::uint32_t vaddr);
  std::uint16_t ExclusiveRead16(std::uint32_t vaddr);
  std::uint32_t ExclusiveRead32(std::uint32_t vaddr);
  std::uint64_t ExclusiveRead64(std::uint32_t vaddr);
  /// 0 = stored, 1 = failed reservation/value check — the inverse of Dynarmic's callback bool and
  /// exactly what the emitted block's `STREX` result register expects.
  std::uint32_t ExclusiveWrite8(std::uint32_t vaddr, std::uint8_t value);
  std::uint32_t ExclusiveWrite16(std::uint32_t vaddr, std::uint16_t value);
  std::uint32_t ExclusiveWrite32(std::uint32_t vaddr, std::uint32_t value);
  std::uint32_t ExclusiveWrite64(std::uint32_t vaddr, std::uint64_t value);
  void CallSVC(std::uint32_t swi);
  void ExceptionRaised(std::uint32_t pc, std::uint32_t exception);
  void AddTicks(std::uint64_t ticks);
  std::uint64_t GetTicksRemaining();
  /// The prelude's return-to-dispatcher trampoline calls this. Returns the next block entry, or
  /// nullptr after recording a fallback or a non-code destination (sentinel/host entry).
  void* Dispatch();

 private:
  enum class Pending { None, Returned, HostCall, Syscall, Fault, Unimplemented, Suspended };

  void SyncIn(const MachineState& state);
  void SyncOut(MachineState& state);
  [[nodiscard]] RunResult MapHalt(Dynarmic::HaltReason hr, unsigned executed);
  void SetPending(Pending p, std::string detail = {});
  void SetFault(std::string detail, GuestAddr addr, bool write);
  void Halt(Dynarmic::HaltReason hr);
  bool ExclusiveCompareAndStore(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected,
                                std::size_t bytes);

  mem::GuestMemory& memory_;
  GuestAddr return_address_;
  const HostDispatch* dispatch_;
  AotImage image_;
  AotState state_{};
  std::uint32_t text_base_ = 0;
  /// One core. The monitor is still required: Dynarmic's trampolines call it unconditionally, and
  /// AOT's exclusive callbacks use the same ReadAndMark/DoExclusiveOperation pairing.
  Dynarmic::ExclusiveMonitor monitor_{1};
  std::unique_ptr<InterpExecutor> fallback_interp_;
  std::string err_;

  MachineState* live_ = nullptr;
  unsigned budget_ = 0;
  unsigned executed_ = 0;
  unsigned fallback_steps_ = 0;  ///< Tier I's IR-step count, reported with (not compared to) executed_
  std::size_t fallbacks_ = 0;
  Pending pending_ = Pending::None;
  std::string pending_detail_;
  std::uint32_t pending_slot_ = 0;
  std::uint32_t pending_svc_ = 0;
  GuestAddr pending_pc_ = 0;
  GuestAddr pending_fault_addr_ = 0;
  bool pending_fault_has_addr_ = false;
  bool pending_fault_write_ = false;
  bool pending_illegal_ = false;
  GuestAddr pending_illegal_pc_ = 0;
};

}  // namespace rb::exec
