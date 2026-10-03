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
  /// IT-block state, as the low 8 bits of the PSR's IT encoding. It has to survive every block
  /// boundary (docs/20 §C02's review focus): an ARMv7 IT sequence can straddle a block edge, and
  /// losing it executes instructions unconditionally that the guest meant to predicate. The
  /// interpreter takes it from the block's location descriptor, which is where Dynarmic keeps it.
  std::uint8_t itstate = 0;

  /// The address execution is at. `r[15]` mirrors it for dump/telemetry convenience only.
  GuestAddr pc = 0;
};

static_assert(sizeof(GuestAddr) == 4, "guest addresses are 32-bit by definition");

/**
 * The CPSR IT field. The 8-bit ITSTATE value (`firstcond:mask`) is **not contiguous** in CPSR:
 * ITSTATE<7:2> live at CPSR bits 15:10 and ITSTATE<1:0> at CPSR bits 26:25 — the low two bits of the
 * byte move up to sit under the Jazelle J bit. Bit-for-bit identical to dynarmic's own
 * `PSR::IT()`/`PSR::IT(ITState)` composition (`frontend/A32/PSR.h`), which is the mapping
 * `LocationDescriptor` round-trips through, and what `A32JitState::Cpsr()`/`SetCpsr()` compose.
 *
 * The first draft of these helpers treated the byte as if IT[1:0] were its bits 7:6 — reading
 * CPSR bits 25:24 (J and IT[0]) and writing the same back. Every resume into the middle of an IT
 * sequence then reconstructed a scrambled byte (ITE-GT's 0xCC came back as 0x33: firstcond CC, mask
 * 0011), the translator predicated the remaining legs on the wrong condition, and execution walked
 * into neighbouring functions. A differential could not see the original form because both tiers
 * shared the same wrong derivation; the fold below is what made it visible, and this mapping is what
 * makes the fold correct.
 *
 * Both tiers derive `MachineState::itstate` through these two functions rather than each computing it
 * their own way. That is not tidiness: an earlier form had Tier I's mirror come from the block's
 * location descriptor and Tier J's from CPSR, and the two differ at the moment execution stops — a
 * redundant field with two definitions is a field that produces false findings and hides real ones.
 */
[[nodiscard]] inline std::uint8_t ItStateFromCpsr(std::uint32_t cpsr) {
  return static_cast<std::uint8_t>(((cpsr >> 8) & 0xFCu) | ((cpsr >> 25) & 0x03u));
}

[[nodiscard]] inline std::uint32_t CpsrWithIt(std::uint32_t cpsr, std::uint8_t itstate) {
  const std::uint32_t cleared = cpsr & ~(0x0600'0000u | 0x0000'fc00u);
  return cleared | (static_cast<std::uint32_t>(itstate & 0xFCu) << 8) |
         (static_cast<std::uint32_t>(itstate & 0x03u) << 25);
}

/// CPSR bit 5 *is* the execution mode: `MachineState::mode` and that bit are one fact kept in two
/// representations, and both tiers have to leave them agreeing.
///
/// Tier J gets this for free — Dynarmic's `Cpsr()` carries the bit and its mode is read back from it.
/// Tier I carries the mode on the block's location descriptor, so without this its `cpsr` can come back
/// with bit 5 clear while `mode` says Thumb: two fields that contradict each other in one struct, in the
/// one place the design says a guest's state is fully described. It is also the difference between a
/// guest's `mrs r0, cpsr` seeing 0x20 in one tier and 0x0 in the other, which is a real divergence
/// waiting for the first piece of guest code that reads CPSR — bionic's `__get_tls()` neighbours do.
///
/// Not modelled in either tier, and named here so nobody mistakes the field for complete: an `MSR CPSR`
/// that writes bit 5 is an exception return in disguise, and Dynarmic handles it by ending the block with
/// a fresh location descriptor. Nothing in the current corpus does it, so there is no measurement of what
/// either tier would do.
[[nodiscard]] inline std::uint32_t CpsrWithMode(std::uint32_t cpsr, isa::Mode mode) {
  constexpr std::uint32_t kTBit = 1u << 5;
  return (cpsr & ~kTBit) | (mode == isa::Mode::Thumb ? kTBit : 0u);
}

/// The diagnostic text for an SVC immediate, from one definition shared by both tiers. Format matters
/// here more than it looks: an earlier version of this produced `SVC 0x42` out of `std::to_string`, so a
/// `svc #0x2a` was reported as `SVC 0x42` — the same number in the wrong radix, in the one line a reader
/// uses to look up a syscall. Both tiers print hex, both print it through this function, and the
/// differential compares what they said.
[[nodiscard]] inline std::string SvcDetail(std::uint32_t imm) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "SVC #0x";
  std::string hex;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const unsigned nib = (imm >> shift) & 0xfu;
    if (hex.empty() && nib == 0 && shift > 0) continue;
    hex += kDigits[nib];
  }
  if (hex.empty()) hex = "0";
  return out + hex;
}

/// Why a Run call returned. `halted` is the only normal outcome; the rest are diagnostics that must
/// reach a log, because each one is a case where the guest stopped for a reason RetroBug chose.
enum class StopReason {
  Budget,        ///< max_instructions reached; state is consistent and Run can continue.
  Returned,      ///< the guest returned from the entry (Tier I: popped the link stack empty).
  HostCall,      ///< a `bl`/`blx` reached a host entry point. `host_slot` says which; resume after the
                 ///< handler has run and left PC at the link, exactly as a guest callee would.
  Syscall,       ///< an SVC reached the syscall monitor, which will resume us. `svc_number` carries the
                 ///< immediate and `pc` is already the instruction after the SVC, because both tiers
                 ///< observe the halt at the block terminal rather than in the middle of the block.
  Suspended,     ///< the caller stopped us (thread park, debugger).
  Unimplemented, ///< an IR op with no Tier I semantics. Never silent: `detail` names the op.
  Fault,         ///< unmapped/permission failure the guest would have seen.
  Internal,      ///< RetroBug's own bug — a state we assert cannot happen happened.
  /// The guest asked to end the instance (`exit_group`) and the monitor agreed. Last rather than
  /// inserted, because `StopReason`'s numeric value appears in differential logs and renumbering it
  /// would make old logs and new code disagree about what `reason 6` meant.
  Exited,
};

struct RunResult {
  StopReason reason = StopReason::Budget;
  /// Op name or address involved, for `Unimplemented`/`Fault`. Empty otherwise.
  std::string detail;
  /// Work done against `max_instructions`, and **the tiers count different things**: Tier I advances
  /// per Dynarmic IR operation, Tier J per guest instruction (Dynarmic's cycle counting, one tick per
  /// fetched encoding). Two tiers that agree on state are not two tiers that agree on a unit, and an
  /// IR-op count is typically several times the guest-instruction count for the same program — so this
  /// number is a progress report, not a comparison, and the ratio between the two tiers says something
  /// about IR density rather than about speed. Unifying it belongs with C02 before C14 or C15 put a
  /// per-app budget on top of it, because a budget whose meaning depends on which tier took the block
  /// is a budget an app can escape by being hot enough to compile.
  unsigned instructions_executed = 0;
  /// Meaningful only for `HostCall`: the index the `HostDispatch` reported, which is the host
  /// function's identity. Zero is a valid slot, so it is the reason code that says this is set, not
  /// the value.
  std::uint32_t host_slot = 0;
  /// Meaningful only for `Syscall`: the SVC immediate, which is where an ARMv7 guest puts the syscall
  /// number (r7) *and* a small code in the encoding, so both are the monitor's business. Recorded by
  /// both tiers from the same operand, because an SVC that reaches the monitor with a different number
  /// per tier would be dispatched to a different syscall — the worst kind of disagreement, since both
  /// runs would look plausible. Thumb-16 `svc` carries an 8-bit immediate, so a tier that zero-extended
  /// something else would still produce a number; only the comparison catches it.
  std::uint32_t svc_number = 0;
};

/// Which execution path a block went through. Naming discipline is AD-8's, so the strings a user can
/// see are decided here rather than at the call site.
enum class CodeTier { Interp, Translate };

/**
 * A guest address that means "hand control back to the host" rather than "execute the code here".
 *
 * JNI's `JNIEnv` is a table of *guest function pointers*, and the guest reaches an entry with
 * `ldr r3, [r0]; ldr r3, [r3, #off]; blx r3` — measured in the fixture at docs/30 §Step 7, not taken
 * from the spec. So a host-implemented JNI function has to be *branchable to*, and the way it does
 * that is by being an address the executor recognises before it translates anything: the same trick
 * `return_address` uses for a normal return, which is why neither needs a mapped or executable page.
 *
 * Deliberately in `rb::exec` and deliberately ignorant of JNI: the executor asks "is this pc a host
 * entry", and `rb::abi` owns what the answer means.
 */
class HostDispatch {
 public:
  virtual ~HostDispatch() = default;
  virtual bool IsHostEntry(GuestAddr pc, std::uint32_t* slot) const = 0;
};

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
  /// False when this tier could not be constructed, so `Run` answers `Internal` for every call rather than
  /// executing anything. Tier I is always available; the JIT tier is not (no execmem, or a back end that
  /// would not build), and a caller that is about to print one row per tier wants to ask before it runs —
  /// `android/jni/launcher.cpp`'s "run both tiers" table does exactly that. The query lives here because
  /// it is a property of the seam: the launcher holds `Executor*`s, and `ok()` used to exist on
  /// `JitExecutor` alone, which is a compile error in a lane no test under `core/tests` builds.
  [[nodiscard]] virtual bool ok() const { return true; }
};

}  // namespace rb::exec
