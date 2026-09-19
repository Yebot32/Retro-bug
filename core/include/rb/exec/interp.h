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
 *   2. Flags are an ephemeral channel, not just CPSR. `Add32`/`Sub32`/the shifts take a `setflags` U1
 *      and the flag-consuming ops (`GetNZCVFromOp`, `GetNZFromOp`, `GetCarryFromOp`) are handed *the
 *      value the flagged op produced* — Dynarmic's x64 and AArch64 back ends emit `TST v, v`, which
 *      takes N and Z from the value and leaves C and V as they were. So the interpreter has to carry a
 *      flag state per producing instruction as well as the architectural CPSR bits, and reading the
 *      wrong one for C or V produces a plausible fib that is wrong in exactly one branch.
 */
#pragma once

#include <cstdint>
#include <string>

#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"
#include "rb/mem/guest_memory.h"

namespace rb::exec {

/**
 * Interprets guest code one IR instruction at a time, with no executable memory and no host code.
 *
 * `return_address` ends a call: guest code returns there via `BX` of LR, and stopping on the address
 * (rather than on an unmapped fetch) is what keeps a call bridge in C08 from having to map a page it
 * does not own. Checked before translation, so a sentinel in unmapped memory is a clean return.
 */
class InterpExecutor : public Executor {
 public:
  /// The guest memory is mutable because executing writes to it; reads go through the const path.
  InterpExecutor(mem::GuestMemory& memory, GuestAddr return_address)
      : memory_{memory}, return_address_{return_address} {}

  RunResult Run(MachineState& state, unsigned max_instructions) override;

  /// Nothing is cached, so nothing can be stale — and that is a property of this tier, not a stub:
  /// running the same block after an invalidation call has to give the same answer, which the test
  /// asserts rather than assumes.
  void Invalidate(GuestAddr from, GuestAddr to) override;

  CodeTier tier() const override { return CodeTier::Interp; }

 private:
  mem::GuestMemory& memory_;
  GuestAddr return_address_;
};

}  // namespace rb::exec
