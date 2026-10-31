/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — Tier I, the IR interpreter.
 *
 * Two facts shape this file, and both came from measurement rather than from reading Dynarmic's docs:
 *
 *   - **The scope is bounded by demand.** `docs/30` §Step 6b censused the real block graph: fib reaches
 *     13 distinct IR ops, `wide_probe` 23. Dynarmic declares 612 opcodes. So the dispatch table below
 *     covers what ARMv7 code compiled by a real toolchain actually emits, and refuses the rest by name
 *     rather than guessing at a semantics it does not have.
 *   - **Dynarmic has no interpreter to reuse.** Its `IR::Term::Interpret` terminal is emitted for
 *     instructions it declines to translate (`arm_LDM_usr`, `arm_CPS`, `arm_RFE`…), and every back end
 *     implements it by calling `UserCallbacks::InterpreterFallback`, i.e. handing the problem back to
 *     the embedder. This class is that hand-back, which is why it is also required on the JIT path and
 *     not only where a JIT is unavailable (`docs/10` §5, AD-2 consequence 3).
 *
 * Two traps here are worth naming because they are invisible in a green build:
 *
 *   1. `IR::Opcode::A32GetRegister` is the enumerator; `IR::GetNameOf` returns `"GetRegister"`. Dispatch
 *      is on the enumerator, diagnostics use the name, and the census output in docs/30 uses the same
 *      names — so mixing the two would look consistent right up until an op silently never matched.
 *   2. **Flags belong to the instruction that produced them, not to the machine.** `Add32`/`Sub32` and
 *      the shift family take a **`carry_in`** U1 — *not* a set-flags bit, which is what this file
 *      believed until `docs/30` §Step 6d — and flags are read back by pseudo-operations naming the value
 *      they were computed from (`GetNZCVFromOp`, `GetNZFromOp`, `GetCarryFromOp`, `GetOverflowFromOp`).
 *      Where the producer has no flag-form, the AArch64 back end emits `TST v, v`, which **clears** C and
 *      V rather than leaving them as they were. So dispatch carries a flag record per IR instruction and
 *      resolves a pseudo-op against it. One mutable `Flags` shared by the whole block is how `push` came
 *      to "set" C and every recursive `fib` call returned an arithmetic series instead — a wrong answer
 *      with a plausible shape, which is the failure mode worth writing down.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"
#include "rb/mem/guest_memory.h"

namespace rb::exec {

/**
 * One core's ARMv7 exclusive-access reservation, as Tier I keeps it.
 *
 * Two levels, because the architecture has two: a *local marker*, set by any exclusive load and dropped by
 * `CLREX` or by any executed exclusive store, and the address-plus-value record the store is checked
 * against. An exclusive store whose marker is absent fails without touching memory, and so does one whose
 * target no longer holds the value the load saw.
 *
 * Tier J does not use this type — Dynarmic holds the marker in its own state struct
 * (`backend/arm64/emit_arm64_memory.cpp:156-157,197-200`) and the record in its `ExclusiveMonitor`, and
 * RetroBug only supplies the compare half. The two are equivalent by construction, which is what the
 * differential on `ldaex`/`strex` is testing; if either tier's side of that equivalence ever changes, the
 * change belongs here too.
 */
struct ExclusiveState {
  bool valid = false;
  std::uint64_t address = 0;
  std::uint64_t value = 0;

  /// A fresh Run starts with nothing reserved, in both tiers.
  void Clear() { valid = false; }

  /// The exclusive load: mark `addr` and remember what was read there.
  void Mark(std::uint64_t addr, std::uint64_t seen) {
    valid = true;
    address = addr;
    value = seen;
  }

  /**
   * True when the marker and the reserved address admit a store at `addr`, consuming the marker either
   * way, as ARM does.
   *
   * The two-step shape matters: nothing may touch memory when this returns false, not even to compare,
   * because Tier J's back end branches around the entire callback when its marker is clear
   * (`emit_arm64_memory.cpp:197-200`). A store that faults on one tier and merely fails on the other is a
   * differential failure wearing a success.
   */
  [[nodiscard]] bool AdmitAddress(std::uint64_t addr) {
    if (!valid) return false;
    valid = false;
    return address == addr;
  }

  /// The value half of the check, against what is at the address now.
  [[nodiscard]] bool ValueUnchanged(std::uint64_t current) const { return value == current; }
};

/**
 * Interprets guest code one IR instruction at a time, with no executable memory and no host code.
 *
 * `return_address` ends a call: guest code returns there via `BX` of LR, and stopping on the address
 * (rather than on an unmapped fetch) is what keeps a call bridge in C08 from having to map a page it
 * does not own. Checked before translation, so a sentinel in unmapped memory is a clean return.
 *
 * `host_dispatch` is optional and checked the same way — before translation, so host entry points need
 * no page behind them either. It is what makes a fake `JNIEnv` callable by real guest code.
 */
class InterpExecutor : public Executor {
 public:
  /// The guest memory is mutable because executing writes to it; reads go through the const path.
  InterpExecutor(mem::GuestMemory& memory, GuestAddr return_address,
                 const HostDispatch* host_dispatch = nullptr)
      : memory_{memory}, return_address_{return_address}, dispatch_{host_dispatch} {}

  RunResult Run(MachineState& state, unsigned max_instructions) override;

  /// Decoded blocks, reused across entries. A block is trusted only while the page it starts on is not
  /// writable: code the guest can change without a protection change is re-decoded every time, which is
  /// what keeps self-modifying code correct without a write observer (the loader maps text read+exec, and
  /// `GuestMemory::Write` refuses a read-only page outright). Invalidation from `arm_cacheflush` evicts
  /// every block whose start address falls in the flushed range.
  ///
  /// Dynarmic's `IR::Block` allocates a fixed 4096-instruction pool for every block, whether the block
  /// uses four slots or four thousand. A 8192-block cache therefore reserves gigabytes of host heap; on
  /// iOS, alongside the guest window, that exhaustion surfaced as a crash inside `PrependNewInst` when
  /// Dynarmic's unchecked pool allocation returned null. Keep the cache bounded to a device-safe budget;
  /// hot loops still stay cached, and a full cache is cleared just as before.
  static constexpr std::size_t kBlockCacheLimit = 128;

  /// Nothing is cached, so nothing can be stale — and that is a property of this tier, not a stub:
  /// running the same block after an invalidation call has to give the same answer, which the test
  /// asserts rather than assumes.
  void Invalidate(GuestAddr from, GuestAddr to) override;

  CodeTier tier() const override { return CodeTier::Interp; }

 private:
  struct CachedBlock {
    std::shared_ptr<isa::TranslatedBlock> block;
  };
  /// What a decoded block depends on besides memory: where it starts, the mode, the IT window it was
  /// decoded inside, and FPSCR — Dynarmic bakes the rounding mode and flush-to-zero into the FP
  /// instructions it emits, so a block decoded under one FPSCR must not be reused under another (the
  /// tier-diff fp_rmode rows are what found this). PSR's other bits do not change a decoding.
  struct BlockKey {
    std::uint32_t pc = 0;
    std::uint32_t fpscr = 0;
    std::uint8_t it = 0;
    bool thumb = false;
    bool operator==(const BlockKey& o) const {
      return pc == o.pc && fpscr == o.fpscr && it == o.it && thumb == o.thumb;
    }
  };
  struct BlockKeyHash {
    std::size_t operator()(const BlockKey& k) const noexcept {
      std::uint64_t h = (static_cast<std::uint64_t>(k.pc) << 32) ^ k.fpscr;
      h ^= static_cast<std::uint64_t>(k.it) << 8;
      h ^= static_cast<std::uint64_t>(k.thumb) << 7;
      h ^= h >> 33;
      return static_cast<std::size_t>(h);
    }
  };
  static BlockKey MakeKey(const Dynarmic::A32::LocationDescriptor& location);
  mem::GuestMemory& memory_;
  std::unordered_map<BlockKey, CachedBlock, BlockKeyHash> blocks_;
  GuestAddr return_address_;
  const HostDispatch* dispatch_;
  /// Runs so far, for `RB_T1_TRACE=<n>` (trace the n-th Run). Per executor rather than a static.
  unsigned run_index_ = 0;
  /// The reservation `ldaex`/`strex` consult. Per-core, and outliving one block: Dynarmic's own marker
  /// lives in the JIT state for the whole `Run`, and an LL/SC retry loop whose load and store land in
  /// different basic blocks is normal for a mutex, so a per-block record would make every such store fail
  /// and the guest would spin until its budget ran out.
  ExclusiveState excl_;
};

}  // namespace rb::exec
