/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — guest machine state and the executor seam.
 *
 * The shape here is docs/20 §C02's, and the two invariants it carries with it:
 *
 *   1. **No host pointer in guest state, and nothing address-shaped wider than 32 bits.** A guest is
 *      32-bit; a `uint64_t` that means an address is either a bug or a smuggled host pointer, and both
 *      end the biased-address model (AD-6) silently. The `d[]` registers are the one place 64-bit
 *      values belong, because those are VFP data rather than addresses — which is why the check below
 *      is about widths of the *address-typed* fields rather than a blanket ban.
 *   2. **One state type for both tiers.** Tier J writes this same structure through Dynarmic's
 *      `Backend::Interface`; Tier I writes it directly. Bit-identical `MachineState` after the same
 *      guest program is the back-end equivalence test, and it is only expressible if the state is one
 *      type rather than two that happen to agree.
 */
#pragma once

#include <cstdint>

#include "rb/isa/front_end.h"

namespace rb::exec {

/// A guest address. Always 32 bits: the guest is armeabi-v7a, and `guest_base` is never assumed to be
/// zero (core/README.md), so an address is a guest-window offset and nothing more.
using GuestAddr = std::uint32_t;

/// Visible-from-outside CPU state. Layout is asserted in the test rather than trusted: a field that
/// quietly widens to 64 bits is the failure this struct has to make loud.
struct MachineState {
  std::uint32_t r[16] = {};  ///< r15 is PC, held in step with `pc` below rather than aliased to it.
  std::uint64_t d[32] = {};  ///< VFPv3-D32. The only 64-bit state, and it is data, not addresses.
  std::uint32_t fpscr = 0;
  std::uint32_t cpsr = 0;
  std::uint32_t tp = 0;      ///< TPIDRURW — the TLS register bionic's __get_tls() reads.
  isa::Mode mode = isa::Mode::Thumb;
  /// IT-block state. It has to survive every block boundary (docs/20 §C02's review focus): an
  /// ARMv7 IT sequence can straddle a block edge, and losing it executes instructions unconditionally
  /// that the guest meant to predicate.
  std::uint8_t itstate = 0;
  std::uint8_t itbits = 0;

  /// The address execution is at. `r[15]` mirrors it for dump/telemetry convenience only.
  GuestAddr pc = 0;
};

static_assert(sizeof(GuestAddr) == 4, "guest addresses are 32-bit by definition");

/// Why a Run call returned. `halted` is the only normal outcome; the rest are diagnostics that must
/// reach a log, because each one is a case where the guest stopped for a reason RetroBug chose.
enum class StopReason {
  Budget,        ///< max_instructions reached; state is consistent and Run can continue.
  Returned,      ///< the guest returned from the entry (Tier I: popped the link stack empty).
  Syscall,       ///< an SVC reached the syscall monitor, which will resume us.
  Suspended,     ///< the caller stopped us (thread park, debugger).
  Unimplemented, ///< an IR op with no Tier I semantics. Never silent: `detail` names the op.
  Fault,         ///< unmapped/permission failure the guest would have seen.
  Internal,      ///< RetroBug's own bug — a state we assert cannot happen happened.
};

struct RunResult {
  StopReason reason = StopReason::Budget;
  /// Op name or address involved, for `Unimplemented`/`Fault`. Empty otherwise.
  std::string detail;
  unsigned instructions_executed = 0;
};

/// Which execution path a block went through. Naming discipline is AD-8's, so the strings a user can
/// see are decided here rather than at the call site.
enum class CodeTier { Interp, Translate };

/// What an instance is allowed to do. `enable_jit` false is the iOS shape and the OEM-denies-execmem
/// shape, which is why Tier I has to be a complete path rather than a bootstrap (AD-2).
struct ExecOpts {
  bool enable_jit = true;
  unsigned jit_threshold = 8;  ///< how often a block runs before it is worth compiling.
};

/// Both tiers. Tier I is `InterpExecutor`; the JIT tier arrives with C04's code cache.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual RunResult Run(MachineState&, unsigned max_instructions) = 0;
  /// Self-modifying code (AD-7): drop anything compiled for guest addresses in [from, to).
  virtual void Invalidate(GuestAddr from, GuestAddr to) = 0;
  virtual CodeTier tier() const = 0;
};

}  // namespace rb::exec
