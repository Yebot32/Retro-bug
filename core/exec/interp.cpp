/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — Tier I, the IR interpreter.
 *
 * Two facts shaped this file, and both came from measurement rather than from reading Dynarmic's docs:
 *
 *   - **The scope is bounded by demand.** `docs/30` §Step 6b censused the real block graph: fib reaches
 *     13 distinct IR ops, `wide_probe` 23. Dynarmic declares 612 opcodes. The dispatch table below
 *     covers what a real toolchain's ARMv7 code actually emits, and refuses anything else by name
 *     rather than guessing at a semantics it does not have — a guessed flag bit or shift-carry rule
 *     would produce a plausible fib that is wrong in exactly one branch.
 *   - **Dynarmic has no interpreter to reuse.** Its `IR::Term::Interpret` terminal is emitted for
 *     instructions it declines to translate (`arm_LDM_usr`, `arm_CPS`, `arm_RFE`…), and every back end
 *     implements that terminal by calling `UserCallbacks::InterpreterFallback`, handing the problem to
 *     the embedder. This class is that hand-back, which is why Tier I is required on the JIT path too
 *     and not only where a JIT is unavailable (`docs/10` §5, AD-2 consequence 3).
 *
 * Two traps are worth naming because neither shows up as a build failure:
 *
 *   1. `IR::Opcode::A32GetRegister` is the enumerator while `IR::GetNameOf` returns `"GetRegister"`.
 *      Dispatch is on the enumerator, diagnostics use the name, and the census output uses the same
 *      names — so mixing the two reads as consistent right up until an op silently never matches.
 *   2. Flags are a channel, not just CPSR. `Add32`/`Sub32`/the shifts take a `setflags` U1, and the
 *      consuming ops (`GetNZCVFromOp`, `GetNZFromOp`, `GetCarryFromOp`) are handed *the value the
 *      flagged op produced*: Dynarmic's own back ends emit `TST v, v` for them, which sets N and Z from
 *      the value and leaves C and V as they were. So there is a flag state per producing instruction as
 *      well as the architectural CPSR bits, and reading the wrong one for C or V is a wrong-answer bug
 *      rather than a crash.
 */
#include "rb/exec/interp.h"

#include "dynarmic/common/variant_util.h"
#include "dynarmic/frontend/A32/FPSCR.h"
#include "dynarmic/frontend/A32/PSR.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"
#include "dynarmic/ir/value.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace rb::exec {
namespace {

using Dynarmic::A32::ExtReg;
using Dynarmic::A32::FPSCR;
using Dynarmic::A32::LocationDescriptor;
using Dynarmic::A32::PSR;
using Dynarmic::A32::Reg;
using Dynarmic::IR::Cond;
using Dynarmic::IR::Inst;
using Dynarmic::IR::Opcode;
using Dynarmic::IR::Type;
using Dynarmic::IR::Value;
namespace Term = Dynarmic::IR::Term;

/// An interpreter temporary: 128 bits is the widest IR type (U128), and a packed NZCV rides in `lo`.
struct V {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;

  [[nodiscard]] std::uint32_t w() const { return static_cast<std::uint32_t>(lo); }
  [[nodiscard]] std::uint64_t d() const { return lo; }
};

/**
 * ARM's condition-code bits, in the layout `A32JitState::cpsr_nzcv` uses on the AArch64 back end: N, Z,
 * C, V at bits 31..28, i.e. exactly their positions inside CPSR, which is why an NZCV value can be
 * stored with one `STR` (emit_arm64_a32.cpp:400).
 *
 * Held per *producing instruction*, not as one ambient variable. That is the shape the IR forces: an
 * arithmetic op does not update CPSR, it emits a value, and a pseudo-operation naming that value
 * (`GetNZCVFromOp`, `GetNZFromOp`, `GetCarryFromOp`, `GetOverflowFromOp`, `GetGEFromOp`) is what reads
 * the flags back. So the only faithful question about a value is "what would this instruction's
 * flag-form have left in the host flags register" — and a single mutable copy, which is what this file
 * had before, lets one instruction's flags be read by an unrelated consumer. That is not hypothetical:
 * `push` lowers to a `Sub32` of SP which carries a `#1` third operand (a carry-in, not a setflags
 * flag), and treating that as "this op set flags" poisoned the copy that `movs r4, #0` then read,
 * turning a `cmp r0, #2` into `Z=1` and making every recursive call in the fixture return a small
 * arithmetic-series value instead of fib.
 */
struct Flags {
  bool n = false;
  bool z = false;
  bool c = false;
  bool v = false;

  [[nodiscard]] std::uint32_t in_cpsr() const {
    return (n ? 0x8000'0000u : 0u) | (z ? 0x4000'0000u : 0u) | (c ? 0x2000'0000u : 0u) |
           (v ? 0x1000'0000u : 0u);
  }
  [[nodiscard]] static Flags from_cpsr(std::uint32_t cpsr) {
    return Flags{(cpsr & 0x8000'0000u) != 0, (cpsr & 0x4000'0000u) != 0,
                 (cpsr & 0x2000'0000u) != 0, (cpsr & 0x1000'0000u) != 0};
  }
  [[nodiscard]] std::uint64_t pack() const {
    return (n ? 8u : 0u) | (z ? 4u : 0u) | (c ? 2u : 0u) | (v ? 1u : 0u);
  }
};

/// `TST v, v`: what the AArch64 back end emits for a `GetNZCVFromOp`/`GetNZFromOp` that is *not*
/// attached to an instruction with a flag-form (emit_arm64.cpp:87,118). N and Z come from the value;
/// AArch64's logical-flag instructions clear C and V, so this does too rather than carrying them
/// through, which is the difference between matching the back end and merely looking plausible.
Flags Tst(std::uint64_t value, unsigned width) {
  return Flags{(value & (1ull << (width - 1))) != 0, value == 0, false, false};
}

std::string Hex(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
  return buf;
}

/// Temporary diagnostic switch (docs/30 §Step 6d): RB_T1_TRACE names the Run to dump.
bool Probe() {
  static const bool on = [] {
    const char* e = std::getenv("RB_T1_PROBE");
    return e != nullptr && e[0] != '\0';
  }();
  return on;
}

/**
 * Immediate payload, for the integer types only.
 *
 * Dynarmic's typed readers assert on a type mismatch even in this Release build — measured, not
 * assumed: calling `GetU32()` on a flag operand aborted the process with `type == Type::U32`, and
 * `GetImmediateAsU64()` documents the same precondition. The IR also carries operand kinds that have
 * no integer reading at all (NZCV travels as SSA values with one exception, `EmptyNZCVImmediateMarker`,
 * which is why `FlagsOf` checks emptiness before anything else). So a raw read is a request that can
 * legitimately fail, and the caller decides what that means rather than reaching for a bit pattern
 * that was never there.
 */
bool ImmediateAsU64(const Value& v, std::uint64_t& out) {
  const Type t = v.GetType();
  if (t != Type::U1 && t != Type::U8 && t != Type::U16 && t != Type::U32 && t != Type::U64) {
    return false;
  }
  out = v.GetImmediateAsU64();
  return true;
}

/// The ARM condition codes as the architecture defines them. AL and NV are both spelled out rather
/// than left to a default, so a new enumerator in Dynarmic's list fails to compile here instead of
/// quietly becoming "never taken".
bool Holds(Cond cond, const Flags& f) {
  switch (cond) {
    case Cond::EQ: return f.z;
    case Cond::NE: return !f.z;
    case Cond::CS: return f.c;
    case Cond::CC: return !f.c;
    case Cond::MI: return f.n;
    case Cond::PL: return !f.n;
    case Cond::VS: return f.v;
    case Cond::VC: return !f.v;
    case Cond::HI: return f.c && !f.z;
    case Cond::LS: return !f.c || f.z;
    case Cond::GE: return f.n == f.v;
    case Cond::LT: return f.n != f.v;
    case Cond::GT: return !f.z && (f.n == f.v);
    case Cond::LE: return f.z || (f.n != f.v);
    case Cond::AL: return true;
    case Cond::NV: return false;
  }
  return false;
}

std::uint64_t SignExtend(std::uint64_t value, unsigned from_bits, unsigned to_bits) {
  const std::uint64_t mask = to_bits >= 64 ? ~0ull : ((1ull << to_bits) - 1);
  const std::uint64_t low = value & ((1ull << from_bits) - 1);
  if ((low >> (from_bits - 1)) & 1) return (low | ~((1ull << from_bits) - 1)) & mask;
  return low & mask;
}

}  // namespace

/// One basic block's interpretation, so that per-block state (temporaries, the recorded flags, the
/// check bit) cannot leak into the next block and be read as if it belonged there. SSA values do not
/// cross a block boundary: each block re-reads what it needs from CPSR or from its own registers.
class BlockRunner {
 public:
  BlockRunner(mem::GuestMemory& memory, MachineState& state, LocationDescriptor& location)
      : memory_{memory}, state_{state}, location_{location} {}

  /// Returns false when execution must stop, with `stop` filled in. On true, `successor` is where to
  /// go next — already reflected in the location reference handed to the constructor.
  bool Run(const isa::TranslatedBlock& block, LocationDescriptor& successor) {
    for (const auto& inst : block.ir) {
      if (!Step(&inst)) return false;
      ++steps;
    }
    return Take(block.ir.GetTerminal(), successor);
  }

  RunResult stop{};
  unsigned steps = 0;

 private:
  [[nodiscard]] bool HasArgs(const Inst* inst, std::size_t n) {
    if (inst->NumArgs() < n) {
      stop.reason = StopReason::Unimplemented;
      stop.detail = std::string{Dynarmic::IR::GetNameOf(inst->GetOpcode())} + ": expected " +
                    std::to_string(n) + " arguments, the IR gave " + std::to_string(inst->NumArgs());
      return false;
    }
    return true;
  }

  bool Fail(const Inst* inst, const char* why) {
    stop.reason = StopReason::Unimplemented;
    stop.detail = std::string{Dynarmic::IR::GetNameOf(inst->GetOpcode())} + ": " + why;
    return false;
  }

  bool Fault(const std::string& why) {
    stop.reason = StopReason::Fault;
    stop.detail = why;
    return false;
  }

  [[nodiscard]] bool Read(const Value& v, V& out) {
    if (v.IsImmediate()) {
      std::uint64_t bits = 0;
      if (ImmediateAsU64(v, bits)) {
        out = V{bits, 0};
        return true;
      }
      if (v.IsEmpty()) {
        out = V{};  // the empty NZCV marker: no value, which is what its consumers ask for
        return true;
      }
      stop.reason = StopReason::Internal;
      stop.detail = "IR immediate of a type this interpreter cannot read (type mask 0x" +
                    Hex(static_cast<std::uint64_t>(v.GetType())) + ")";
      return false;
    }
    const auto it = temps_.find(v.GetInst());
    if (it == temps_.end()) {
      stop.reason = StopReason::Internal;
      stop.detail = "IR temporary read before it was written";
      return false;
    }
    out = it->second;
    return true;
  }

  [[nodiscard]] bool W32(const Inst* inst, std::size_t i, std::uint32_t& out) {
    if (!HasArgs(inst, i + 1)) return false;
    V t{};
    if (!Read(inst->GetArg(i), t)) return false;
    out = t.w();
    return true;
  }

  [[nodiscard]] bool W64(const Inst* inst, std::size_t i, std::uint64_t& out) {
    if (!HasArgs(inst, i + 1)) return false;
    V t{};
    if (!Read(inst->GetArg(i), t)) return false;
    out = t.d();
    return true;
  }

  [[nodiscard]] bool W128(const Inst* inst, std::size_t i, V& out) {
    if (!HasArgs(inst, i + 1)) return false;
    return Read(inst->GetArg(i), out);
  }

  /**
   * The flags a pseudo-operation (`GetNZCVFromOp`, `GetNZFromOp`, …) reads from its argument: what the
   * instruction that produced the value would have left in the host flags register, or `TST v, v` when
   * that instruction has no flag-form — which is exactly the choice the back end makes at
   * emit_arm64.cpp:87. The width comes from the value's IR type, not from a 32-bit assumption:
   * `GetNZCVFromOp` is emitted for 64-bit operands too, and deriving N from bit 31 of a 64-bit value
   * reads the low word's sign bit instead of the result's.
   */
  [[nodiscard]] bool FlagsFromOp(const Value& arg, Flags& out) {
    if (arg.IsImmediate()) {
      out = Flags{};  // the empty NZCV marker contributes no bits of its own
      return true;
    }
    if (const auto it = produced_.find(arg.GetInst()); it != produced_.end()) {
      out = it->second;
      return true;
    }
    V value{};
    if (!Read(arg, value)) return false;
    out = Tst(value.d(), arg.GetType() == Type::U64 ? 64 : 32);
    return true;
  }

  /// The NZCV a `SetCpsr*` writer consumes. NZCV-typed values are stored in CPSR's own nibble layout
  /// (`STR Wnzcv, cpsr_nzcv`), so a producer this file did not record is decoded rather than dropped.
  [[nodiscard]] bool NzcvOf(const Value& arg, Flags& out) {
    if (arg.IsImmediate()) {
      std::uint64_t bits = 0;
      out = ImmediateAsU64(arg, bits) ? Flags::from_cpsr(static_cast<std::uint32_t>(bits)) : Flags{};
      return true;
    }
    if (const auto it = produced_.find(arg.GetInst()); it != produced_.end()) {
      out = it->second;
      return true;
    }
    V value{};
    if (!Read(arg, value)) return false;
    out = Flags::from_cpsr(value.w());
    return true;
  }

  void Put(const Inst* inst, V value) { temps_[inst] = value; }

  void WriteReg(Reg reg, std::uint32_t value);
  bool Step(const Inst* inst);
  bool Take(const Term::Terminal& terminal, LocationDescriptor& successor);

  /// Every argument of an instruction, in the IR's own terms, for a failure message that a reader
  /// cannot improve on by attaching a debugger: an SSA operand is named by the op that produced it,
  /// because "the address was 0x1000034c4" only says the *producer* was misunderstood.
  std::string Describe(const Inst* inst) {
    std::string out = Dynarmic::IR::GetNameOf(inst->GetOpcode());
    for (std::size_t i = 0; i < inst->NumArgs(); ++i) {
      const Value v = inst->GetArg(i);
      out += " arg";
      out += std::to_string(i);
      out += '=';
      if (v.IsImmediate()) {
        std::uint64_t bits = 0;
        out += ImmediateAsU64(v, bits) ? "imm:0x" + Hex(bits)
                                       : "imm(type 0x" + Hex(static_cast<std::uint64_t>(v.GetType())) + ")";
      } else {
        out += "%" + Dynarmic::IR::GetNameOf(v.GetInst()->GetOpcode());
      }
    }
    return out;
  }

  [[nodiscard]] bool ReadMem(std::uint64_t addr, void* dst, std::size_t bytes, const Inst* inst) {
    if (addr > memory_.window_bytes() || bytes > memory_.window_bytes() - addr) {
      return Fault("guest memory read past the window: 0x" + Hex(addr) + " (" + std::to_string(bytes) +
                   " bytes, window 0x" + Hex(memory_.window_bytes()) + ") at " + Describe(inst));
    }
    if (!memory_.Read(static_cast<GuestAddr>(addr), dst, bytes)) {
      return Fault("guest memory read at 0x" + Hex(addr) +
                   " refused (unmapped or not readable) at " + Describe(inst));
    }
    return true;
  }

  [[nodiscard]] bool WriteMem(std::uint64_t addr, const void* src, std::size_t bytes,
                               const Inst* inst) {
    if (addr > memory_.window_bytes() || bytes > memory_.window_bytes() - addr) {
      return Fault("guest memory write past the window: 0x" + Hex(addr) + " (" +
                   std::to_string(bytes) + " bytes, window 0x" + Hex(memory_.window_bytes()) +
                   ") at " + Describe(inst));
    }
    if (!memory_.Write(static_cast<GuestAddr>(addr), src, bytes)) {
      return Fault("guest memory write at 0x" + Hex(addr) +
                   " refused (unmapped, not writable, or an executing page) at " + Describe(inst));
    }
    return true;
  }

  /// Records the flags this instruction's flag-form would have left, for whichever pseudo-operation
  /// names it. Recorded whether or not one exists: it is a property of the instruction, and the
  /// consumer — not the producer — decides which bits it reads.
  void NoteFlags(const Inst* inst, const Flags& f) { produced_[inst] = f; }

  mem::GuestMemory& memory_;
  MachineState& state_;
  LocationDescriptor& location_;
  std::unordered_map<const Inst*, V> temps_;
  std::unordered_map<const Inst*, Flags> produced_;
  /// Shifter carries, kept apart from `produced_` because their N/Z reach CPSR by a different route
  /// (`TST`) than their C does (`GetCarryFromOp`), and merging them would make Tier I disagree with
  /// the back end about what a `GetNZCVFromOp` on a shift means.
  std::unordered_map<const Inst*, bool> carry_;
  bool check_bit = false;
};

void BlockRunner::WriteReg(Reg reg, std::uint32_t value) {
  if (reg == Reg::PC) {
    // A guest write to PC. Dynarmic ends the block after one (usually with Interpret), so this only
    // has to get the next location right. In Thumb the low bit is the ISA tag rather than part of the
    // address; in ARM it is not set at all.
    const bool thumb = state_.mode == isa::Mode::Thumb;
    const std::uint32_t next = thumb ? (value & ~1u) : (value & ~3u);
    if (thumb && (value & 1u) == 0) state_.mode = isa::Mode::Arm;  // BX-like PC write into ARM code
    location_ = location_.SetPC(next).SetTFlag(state_.mode == isa::Mode::Thumb);
    state_.pc = next;
    state_.r[15] = next;
    return;
  }
  state_.r[static_cast<int>(reg)] = value;
}

bool BlockRunner::Step(const Inst* inst) {
  const Opcode op = inst->GetOpcode();
  // RRX is the one shift whose carry-in reaches the result, so it is the one whose operands sit one
  // index lower: `RotateRightExtended(value, carry_in)` against `LogicalShiftLeft32(value, amount,
  // carry_in)`.
  const bool is_rrx = op == Opcode::RotateRightExtended;
  switch (op) {
    // ---------------------------------------------------------------- no-ops and hints
    case Opcode::Void:
    case Opcode::A32UpdateUpperLocationDescriptor:  // a JIT bookkeeping hint; no packed descriptor here
    case Opcode::PushRSB:                          // return-stack predictor: a performance hint only
      return true;

    case Opcode::Identity: {
      if (!HasArgs(inst, 1)) return false;
      V t{};
      if (!Read(inst->GetArg(0), t)) return false;
      Put(inst, t);
      return true;
    }

    case Opcode::Breakpoint:
      stop.reason = StopReason::Suspended;
      stop.detail = "guest breakpoint";
      return false;

    case Opcode::A32ExceptionRaised:
      return Fault("the front end raised an exception at this instruction");

    // ---------------------------------------------------------------- register file
    case Opcode::A32GetRegister: {
      if (!HasArgs(inst, 1)) return false;
      const int index = static_cast<int>(inst->GetArg(0).GetA32RegRef());
      Put(inst, V{index == static_cast<int>(Reg::PC) ? location_.PC() : state_.r[index], 0});
      return true;
    }
    case Opcode::A32SetRegister: {
      if (!HasArgs(inst, 2)) return false;
      std::uint32_t value = 0;
      if (!W32(inst, 1, value)) return false;
      WriteReg(inst->GetArg(0).GetA32RegRef(), value);
      return true;
    }

    case Opcode::A32GetExtendedRegister32:
    case Opcode::A32GetExtendedRegister64:
    case Opcode::A32GetVector: {
      if (!HasArgs(inst, 1)) return false;
      const int r = static_cast<int>(inst->GetArg(0).GetA32ExtRegRef());
      if (op == Opcode::A32GetVector) {
        // Dynarmic emits GetVector for D-sized NEON operands as well as Q-sized ones — measured, not
        // assumed: the fixture's `wide_probe` asks for index 48, which is D16. A D register is the low
        // half of a 128-bit lane group with nothing above it, so the upper half reads as zero rather
        // than as whatever the previous Q operation left in the neighbouring D register: reading
        // d[n+1] here would feed a stale lane into VectorAdd32, whose result then looks plausible in
        // the low half and is simply never inspected.
        if (r >= 64 && r <= 79) {
          Put(inst, V{state_.d[(r - 64) * 2], state_.d[(r - 64) * 2 + 1]});
          return true;
        }
        if (r >= 32 && r <= 63) {
          Put(inst, V{state_.d[r - 32], 0});
          return true;
        }
        return Fail(inst, ("vector register index " + std::to_string(r) + " is neither a D nor a Q")
                              .c_str());
      }
      if (r < 32) {  // S<2n> is D<n>'s low half and S<2n+1> its high half on little-endian ARM
        const std::uint64_t w = state_.d[r >> 1];
        Put(inst, V{static_cast<std::uint32_t>((r & 1) ? w >> 32 : w & 0xffffffffu), 0});
        return true;
      }
      if (r < 64) {
        const std::uint64_t w = state_.d[r - 32];
        Put(inst, op == Opcode::A32GetExtendedRegister64 ? V{w, 0}
                                                         : V{static_cast<std::uint32_t>(w), 0});
        return true;
      }
      return Fail(inst, "unknown extended register index");
    }

    case Opcode::A32SetExtendedRegister32:
    case Opcode::A32SetExtendedRegister64:
    case Opcode::A32SetVector: {
      if (!HasArgs(inst, 2)) return false;
      const int r = static_cast<int>(inst->GetArg(0).GetA32ExtRegRef());
      if (op == Opcode::A32SetVector) {
        V q{};
        if (!W128(inst, 1, q)) return false;
        if (r >= 64 && r <= 79) {
          state_.d[(r - 64) * 2] = q.d();
          state_.d[(r - 64) * 2 + 1] = q.hi;
          return true;
        }
        if (r >= 32 && r <= 63) {
          state_.d[r - 32] = q.d();  // as for GetVector: a D write leaves the lane above it alone
          return true;
        }
        return Fail(inst, "vector register is neither a D nor a Q");
      }
      std::uint64_t value = 0;
      if (op == Opcode::A32SetExtendedRegister64) {
        if (!W64(inst, 1, value)) return false;
      } else {
        std::uint32_t w = 0;
        if (!W32(inst, 1, w)) return false;
        value = w;
      }
      if (r < 32) {
        std::uint64_t& slot = state_.d[r >> 1];
        slot = (r & 1) ? ((slot & 0xffffffffu) | (value << 32))
                       : ((slot & ~0xffffffffull) | (value & 0xffffffffu));
        return true;
      }
      if (r < 64) {
        std::uint64_t& slot = state_.d[r - 32];
        slot = op == Opcode::A32SetExtendedRegister64
                   ? value
                   : ((slot & ~0xffffffffull) | (value & 0xffffffffu));
        return true;
      }
      return Fail(inst, "unknown extended register index");
    }

    // ---------------------------------------------------------------- CPSR and the flag writers
    //
    // On the AArch64 back end `cpsr_nzcv` is one word wide and every writer below is a mask over
    // CPSR's top nibble, so they are written the same way here — with the low 28 bits, mode bits
    // included, left alone. Masking with a 64-bit complement instead (which is what an
    // `~0xf'0000'0000ull` on a `uint32_t` promotes to) silently clears the whole low word, and the
    // damage only shows up when something reads CPSR back.
    case Opcode::A32GetCpsr:
      Put(inst, V{state_.cpsr, 0});
      return true;
    case Opcode::A32SetCpsr: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.cpsr = v;
      return true;
    }
    case Opcode::A32SetCpsrNZCVRaw: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.cpsr = (state_.cpsr & ~0xf000'0000u) | (v & 0xf000'0000u);
      return true;
    }
    case Opcode::A32SetCpsrNZCVQ: {
      /// NZCV plus Q, which is CPSR bit 27 — the two are stored from one word because that is where
      /// the saturating arithmetic instructions leave them (emit_arm64_a32.cpp:417).
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.cpsr = (state_.cpsr & ~0xf800'0000u) | (v & 0xf800'0000u);
      return true;
    }
    case Opcode::A32SetCpsrNZCV:
    case Opcode::A32SetCpsrNZ: {
      if (!HasArgs(inst, 1)) return false;
      Flags f{};
      if (!NzcvOf(inst->GetArg(0), f)) return false;
      if (op == Opcode::A32SetCpsrNZCV) {
        state_.cpsr = (state_.cpsr & ~0xf000'0000u) | f.in_cpsr();
      } else {  // NZ only: the guest's C and V stay as they were.
        const Flags cur = Flags::from_cpsr(state_.cpsr);
        state_.cpsr = (state_.cpsr & ~0xf000'0000u) | Flags{f.n, f.z, cur.c, cur.v}.in_cpsr();
      }
      return true;
    }
    case Opcode::A32SetCpsrNZC: {
      // N and Z from the first operand, C from the second, V left as it was. This is what a
      // flag-setting shift needs: the IR takes N/Z from a `TST` of the result and the carry from
      // `GetCarryFromOp` on the shift, and only a two-operand writer can recombine them.
      if (!HasArgs(inst, 2)) return false;
      Flags f{};
      if (!NzcvOf(inst->GetArg(0), f)) return false;
      V c{};
      if (!Read(inst->GetArg(1), c)) return false;
      const Flags cur = Flags::from_cpsr(state_.cpsr);
      state_.cpsr = (state_.cpsr & ~0xf000'0000u) | Flags{f.n, f.z, c.d() != 0, cur.v}.in_cpsr();
      return true;
    }
    case Opcode::A32GetCFlag:
      Put(inst, V{Flags::from_cpsr(state_.cpsr).c ? 1u : 0u, 0});
      return true;

    // ---------------------------------------------------------------- reading an op's flags back
    case Opcode::GetCarryFromOp: {
      if (!HasArgs(inst, 1)) return false;
      const Value arg = inst->GetArg(0);
      if (!arg.IsImmediate()) {
        if (const auto c = carry_.find(arg.GetInst()); c != carry_.end()) {
          Put(inst, V{c->second ? 1u : 0u, 0});
          return true;
        }
      }
      Flags f{};
      if (!FlagsFromOp(arg, f)) return false;
      Put(inst, V{f.c ? 1u : 0u, 0});
      return true;
    }
    case Opcode::GetOverflowFromOp: {
      if (!HasArgs(inst, 1)) return false;
      Flags f{};
      if (!FlagsFromOp(inst->GetArg(0), f)) return false;
      Put(inst, V{f.v ? 1u : 0u, 0});
      return true;
    }
    case Opcode::GetNZCVFromOp:
    case Opcode::GetNZFromOp: {
      if (!HasArgs(inst, 1)) return false;
      const Value arg = inst->GetArg(0);
      Flags f{};
      if (!FlagsFromOp(arg, f)) return false;
      if (Probe()) {
        const bool attached = !arg.IsImmediate() && produced_.count(arg.GetInst()) != 0;
        std::fprintf(stderr, "  [%s] %s -> n%d z%d c%d v%d\n",
                     Dynarmic::IR::GetNameOf(op).c_str(),
                     attached ? "producer-flags" : "TST", f.n, f.z, f.c, f.v);
      }
      NoteFlags(inst, f);
      // The value an NZCV consumer stores is in CPSR's nibble layout, which is why `in_cpsr` and not
      // `pack` here: a writer that takes this word without consulting `produced_` still gets the bits
      // in the positions it expects.
      Put(inst, V{f.in_cpsr(), 0});
      return true;
    }
    case Opcode::NZCVFromPackedFlags: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      const Flags f = Flags::from_cpsr(v);
      NoteFlags(inst, f);
      Put(inst, V{v, 0});
      return true;
    }
    case Opcode::GetCFlagFromNZCV: {
      if (!HasArgs(inst, 1)) return false;
      Flags f{};
      if (!NzcvOf(inst->GetArg(0), f)) return false;
      Put(inst, V{f.c ? 1u : 0u, 0});
      return true;
    }

    // ---------------------------------------------------------------- memory
    //
    // Every A32 memory op carries the location descriptor as its **first** operand:
    // `Inst(Opcode::A32WriteMemory32, ImmCurrentLocationDescriptor(), vaddr, value, acc_type)` in
    // a32_ir_emitter.cpp. It is there so a back end can see the endianness the translator resolved —
    // for BE it emits a ByteReverseWord rather than consulting the flag later — which means an
    // interpreter ignores it entirely, and that the real operands start at index 1. Reading index 0 as
    // an address is not a crash: it is a store to `pc | upper<<32`, which is a plausible-looking value
    // in a wrong place, and the reason the fault messages below name the instruction that produced
    // them.
    case Opcode::A32ReadMemory8:
    case Opcode::A32ReadMemory16:
    case Opcode::A32ReadMemory32:
    case Opcode::A32ReadMemory64: {
      std::uint64_t addr = 0;
      if (!W64(inst, 1, addr)) return false;
      const std::size_t bytes = op == Opcode::A32ReadMemory8    ? 1
                                : op == Opcode::A32ReadMemory16  ? 2
                                : op == Opcode::A32ReadMemory32  ? 4
                                                                 : 8;
      unsigned char raw[8] = {};
      if (!ReadMem(addr, raw, bytes, inst)) return false;
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(raw[i]) << (i * 8);  // little-endian, as ARM is
      }
      Put(inst, V{value, 0});
      return true;
    }
    case Opcode::A32WriteMemory8:
    case Opcode::A32WriteMemory16:
    case Opcode::A32WriteMemory32:
    case Opcode::A32WriteMemory64: {
      std::uint64_t addr = 0;
      if (!W64(inst, 1, addr)) return false;
      std::uint64_t value = 0;
      if (!W64(inst, 2, value)) return false;
      const std::size_t bytes = op == Opcode::A32WriteMemory8    ? 1
                                : op == Opcode::A32WriteMemory16  ? 2
                                : op == Opcode::A32WriteMemory32  ? 4
                                                                  : 8;
      unsigned char raw[8] = {};
      for (std::size_t i = 0; i < bytes; ++i) {
        raw[i] = static_cast<unsigned char>(value >> (i * 8));
      }
      return WriteMem(addr, raw, bytes, inst);
    }

    // ---------------------------------------------------------------- control transfer
    case Opcode::A32BXWritePC: {
      std::uint32_t target = 0;
      if (!W32(inst, 0, target)) return false;
      // Same rule as a PC write through SetRegister, and the reason the two share one helper: bit 0 of
      // a Thumb target is the ISA tag, so `bx lr` out of a Thumb function and `bx` into an ARM one
      // differ only in that bit. Dynarmic ends the block after this with PopRSBHint or Interpret.
      WriteReg(Reg::PC, target);
      return true;
    }

    // ---------------------------------------------------------------- FPSCR and the check bit
    case Opcode::A32GetFpscr:
      Put(inst, V{state_.fpscr, 0});
      return true;
    case Opcode::A32SetFpscr: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.fpscr = v;
      return true;
    }
    case Opcode::A32SetCheckBit: {
      if (!HasArgs(inst, 1)) return false;
      V t{};
      if (!Read(inst->GetArg(0), t)) return false;
      check_bit = t.d() != 0;
      return true;
    }

    // ---------------------------------------------------------------- 32- and 64-bit arithmetic
    //
    // `Add32`/`Sub32`/`Add64`/`Sub64` are **add/subtract with a carry-in**, not plain add/sub. The
    // third operand is that carry-in (`ir_emitter.cpp:153-180`: `Add(a, b, carry_in)`, and plain
    // `Sub(a, b)` is emitted as `Sub32(a, b, Imm1(1))`), and the back end lowers a non-immediate one
    // straight to AArch64 `ADC`/`SBC`. So:
    //
    //   add:  result = a + b + ci            carry = the addition overflowed the width
    //   sub:  result = a + ~b + ci           carry = the addition did *not* overflow — ARM's C after
    //                                         SUBS is "no borrow", which is why it is easy to invert
    //
    // Both are computed as one addition, which is what makes the carry rule fall out. Reading operand
    // 2 as a set-flags bit gets plain add and subtract right by accident — ci is 0 for one and 1 for
    // the other — and every `adc`, `sbc` and their flag-setting forms wrong.
    case Opcode::Add32:
    case Opcode::Sub32:
    case Opcode::Add64:
    case Opcode::Sub64: {
      if (!HasArgs(inst, 3)) return false;
      const bool is64 = op == Opcode::Add64 || op == Opcode::Sub64;
      const bool sub = op == Opcode::Sub32 || op == Opcode::Sub64;
      std::uint64_t a = 0, b = 0;
      if (is64) {
        if (!W64(inst, 0, a) || !W64(inst, 1, b)) return false;
      } else {
        std::uint32_t a32 = 0, b32 = 0;
        if (!W32(inst, 0, a32) || !W32(inst, 1, b32)) return false;
        a = a32;
        b = b32;
      }
      V in_ci{};
      if (!Read(inst->GetArg(2), in_ci)) return false;

      const std::uint64_t mod = is64 ? ~0ull : 0xffff'ffffull;
      const std::uint64_t top = is64 ? (1ull << 63) : (1u << 31);
      const unsigned __int128 sum =
          static_cast<unsigned __int128>(a) + (sub ? mod - b : b) + (in_ci.d() ? 1u : 0u);
      const std::uint64_t result = static_cast<std::uint64_t>(sum) & mod;
      Put(inst, V{result, 0});

      Flags f{};
      f.n = (result & top) != 0;
      f.z = result == 0;
      f.c = sum > mod;
      f.v = sub ? (((a ^ b) & (a ^ result)) & top) != 0
                : (((a ^ result) & (b ^ result)) & top) != 0;
      if (Probe()) {
        std::fprintf(stderr, "  [%s] %x %c %x ci=%d -> %x n%d z%d c%d v%d\n",
                     Dynarmic::IR::GetNameOf(op).c_str(), static_cast<std::uint32_t>(a),
                     sub ? '-' : '+', static_cast<std::uint32_t>(b), static_cast<int>(in_ci.d() != 0),
                     static_cast<std::uint32_t>(result), f.n, f.z, f.c, f.v);
      }
      NoteFlags(inst, f);
      return true;
    }
    case Opcode::Mul32: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{static_cast<std::uint32_t>(static_cast<std::uint64_t>(a) * b), 0});
      return true;
    }
    // Bitwise ops need no flag record at all: their flag-form on AArch64 is `ANDS`/`ORS`/`EORS`, which
    // leaves N and Z from the result and clears C and V — exactly what the `TST v, v` fallback
    // `FlagsFromOp` already computes. A shifted *operand* carries a shifter C, but the translator
    // takes that from a separate `GetCarryFromOp` on the shift and recombines it with `SetCpsrNZC`.
    case Opcode::And32:
    case Opcode::Or32:
    case Opcode::Eor32: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{op == Opcode::And32 ? a & b : op == Opcode::Or32 ? a | b : a ^ b, 0});
      return true;
    }

    /**
     * The shift family, under the contract the AArch64 back end actually implements — which is *not*
     * ARM's textual set of shift rules, and the difference is the kind that hides:
     *
     *   - The operand is `carry_in`, again, not a set-flags bit (`ir_emitter.cpp:151-176`), and it
     *     **never** affects the result: the back end emits a plain `LSL`/`LSR`/`ASR`, which fills with
     *     zeros or the sign bit. It is only ever read back as the carry-out when the shift amount is
     *     zero (`DefineAsExisting(carry_inst, carry_arg)`, emit_arm64_data_processing.cpp:276).
     *   - Amount 0 therefore means "no shift, carry unchanged", not "RRX". ARM's `LSR #0`, `ASR #0`
     *     and `ROR #0` quirks — shift by 32, shift by 32, RRX — are the *translator's* business; it
     *     has already applied them and emits `LogicalShiftRight32(v, 32, ci)` or
     *     `RotateRightExtended(v, ci)` to say so. Reproducing them here would apply them twice.
     *   - The amount is masked to its low 8 bits (`ANDS Wscratch0, Wshift, 0xff`), which is also what
     *     makes the `n == 0` test below the right place to look for a register-shifted `RRX`.
     *
     * N and Z come from the result (identical to the `TST` fallback); C is the shifter's carry-out and
     * is published separately, because a `GetNZCVFromOp` on a shift is lowered as `TST` and would lose
     * it — so reading C out of the ordinary flag record would disagree with the back end.
     */
    case Opcode::LogicalShiftLeft32:
    case Opcode::LogicalShiftRight32:
    case Opcode::ArithmeticShiftRight32:
    case Opcode::RotateRight32:
    case Opcode::RotateRightExtended: {
      if (!HasArgs(inst, is_rrx ? 2 : 3)) return false;
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      std::uint32_t ci = 0;
      if (!is_rrx) {
        V in_ci{};
        if (!Read(inst->GetArg(2), in_ci)) return false;
        ci = in_ci.d() ? 1u : 0u;
      } else {
        // RRX *does* take its top bit from the carry-in — that is what "extend" means here.
        V in_ci{};
        if (!Read(inst->GetArg(1), in_ci)) return false;
        ci = in_ci.d() ? 1u : 0u;
      }

      std::uint32_t raw_amount = 0;
      unsigned n = 0;
      if (!is_rrx) {
        if (!W32(inst, 1, raw_amount)) return false;
        n = raw_amount & 0xffu;
      }

      std::uint32_t result = a;
      bool carry = ci != 0;
      if (n == 0 && !is_rrx) {  // pass the operand through and hand back the carry-in
        result = a;
        carry = ci != 0;
      } else if (is_rrx) {
        result = (a >> 1) | (ci << 31);
        carry = (a & 1u) != 0;
      } else if (op == Opcode::LogicalShiftLeft32) {
        if (n < 32) {
          result = a << n;
          carry = ((a >> (32 - n)) & 1u) != 0;
        } else {
          result = 0;
          carry = n == 32 && (a & 1u);
        }
      } else if (op == Opcode::LogicalShiftRight32) {
        if (n < 32) {
          result = a >> n;
          carry = ((a >> (n - 1)) & 1u) != 0;
        } else {
          result = 0;
          carry = n == 32 && (a & 0x8000'0000u) != 0;
        }
      } else if (op == Opcode::ArithmeticShiftRight32) {
        const bool sign = (a & 0x8000'0000u) != 0;
        if (n < 32) {
          result = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> n);
          carry = ((a >> (n - 1)) & 1u) != 0;
        } else {
          result = sign ? 0xffff'ffffu : 0u;
          carry = sign;
        }
      } else {  // RotateRight32: a full 32-bit rotation is the operand itself, with bit 31 last out.
        const unsigned r = n % 32;
        result = r == 0 ? a : ((a >> r) | (a << (32 - r)));
        carry = ((a >> (r == 0 ? 31 : r - 1)) & 1u) != 0;
      }

      Put(inst, V{result, 0});
      carry_[inst] = carry;
      NoteFlags(inst, Tst(result, 32));  // N and Z from the result; C is published separately, V clear
      return true;
    }

    // ---------------------------------------------------------------- extension, truncation, packing
    case Opcode::LeastSignificantByte:
    case Opcode::LeastSignificantHalf:
    case Opcode::LeastSignificantWord:
    case Opcode::MostSignificantWord:
    case Opcode::MostSignificantBit: {
      std::uint64_t a = 0;
      if (!W64(inst, 0, a)) return false;
      Put(inst, V{op == Opcode::LeastSignificantByte   ? a & 0xffu
                  : op == Opcode::LeastSignificantHalf ? a & 0xffffu
                  : op == Opcode::LeastSignificantWord ? a & 0xffffffffu
                  : op == Opcode::MostSignificantWord  ? a >> 32
                                                       : (a >> 31) & 1u,
                  0});
      return true;
    }
    case Opcode::ZeroExtendByteToWord:
    case Opcode::SignExtendByteToWord:
    case Opcode::ZeroExtendHalfToWord:
    case Opcode::SignExtendHalfToWord:
    case Opcode::ZeroExtendWordToLong:
    case Opcode::SignExtendWordToLong: {
      std::uint64_t a = 0;
      if (!W64(inst, 0, a)) return false;
      const bool byte = op == Opcode::ZeroExtendByteToWord || op == Opcode::SignExtendByteToWord;
      const bool half = op == Opcode::ZeroExtendHalfToWord || op == Opcode::SignExtendHalfToWord;
      const unsigned from = byte ? 8 : half ? 16 : 32;
      const bool to_long = op == Opcode::ZeroExtendWordToLong || op == Opcode::SignExtendWordToLong;
      const bool sgn = op == Opcode::SignExtendByteToWord || op == Opcode::SignExtendHalfToWord ||
                       op == Opcode::SignExtendWordToLong;
      if (!sgn) {
        Put(inst, V{a & ((1ull << from) - 1), 0});
        return true;
      }
      Put(inst, V{SignExtend(a, from, to_long ? 64 : 32), 0});
      return true;
    }
    case Opcode::Pack2x32To1x64: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{(static_cast<std::uint64_t>(b) << 32) | a, 0});
      return true;
    }

    // ---------------------------------------------------------------- 128-bit vectors
    case Opcode::VectorAdd32: {
      V x{}, y{};
      if (!W128(inst, 0, x) || !W128(inst, 1, y)) return false;
      const auto lanes = [](std::uint64_t a, std::uint64_t b) {
        const std::uint64_t lo = ((a & 0xffffffffu) + (b & 0xffffffffu)) & 0xffffffffu;
        const std::uint64_t hi = ((a >> 32) + (b >> 32)) & 0xffffffffu;
        return lo | (hi << 32);
      };
      Put(inst, V{lanes(x.lo, y.lo), lanes(x.hi, y.hi)});
      return true;
    }

    default:
      return Fail(inst, "no Tier I semantics yet");
  }
}

/// Dispatch through the visitor rather than `boost::get<Term::If>`: `If`, `CheckBit` and `CheckHalt`
/// are held as `recursive_wrapper` alternatives, so asking the variant for the bare type does not find
/// them — the same reason `apply_visitor` has to unwrap (docs/30 §Step 5f). The legs are terminals
/// themselves, so this recurses: `If` inside `If` is ordinary for an ARMv7 IT block, and a tree that
/// stopped at the top level would execute one leg of an IT-block twice or skip one entirely.
bool BlockRunner::Take(const Term::Terminal& terminal, LocationDescriptor& successor) {
  return Dynarmic::Common::VisitVariant<bool>(terminal, [&](const auto& t) -> bool {
    using T = std::remove_cvref_t<decltype(t)>;
    if constexpr (std::is_same_v<T, Term::LinkBlock> || std::is_same_v<T, Term::LinkBlockFast> ||
                  std::is_same_v<T, Term::Interpret>) {
      successor = LocationDescriptor{t.next};
      return true;
    } else if constexpr (std::is_same_v<T, Term::PopRSBHint> ||
                         std::is_same_v<T, Term::FastDispatchHint>) {
      successor = location_;  // whatever BXWritePC put here is the next location
      return true;
    } else if constexpr (std::is_same_v<T, Term::If>) {
      // Predicated on the architectural flags: a conditional branch tests CPSR's NZCV, which the
      // flag-setting ops in this block wrote there through SetCpsrNZCV.
      return Take(Holds(t.if_, Flags::from_cpsr(state_.cpsr)) ? t.then_ : t.else_, successor);
    } else if constexpr (std::is_same_v<T, Term::CheckBit>) {
      // The check bit is ambient state that A32SetCheckBit writes, not a terminal operand — the JIT
      // back ends keep it in a stack slot for exactly the same reason.
      return Take(check_bit ? t.then_ : t.else_, successor);
    } else if constexpr (std::is_same_v<T, Term::CheckHalt>) {
      return Take(t.else_, successor);  // a halt request stops the executor, not the guest
    } else if constexpr (std::is_same_v<T, Term::ReturnToDispatch>) {
      stop.reason = StopReason::Returned;
      return false;
    } else {
      stop.reason = StopReason::Internal;
      stop.detail = "a block ended in a terminal this interpreter does not leave through";
      return false;
    }
  });
}

RunResult InterpExecutor::Run(MachineState& state, unsigned max_instructions) {
  RunResult result{};
  LocationDescriptor location{state.pc, PSR{state.cpsr}, FPSCR{state.fpscr}, false};
  location = location.SetTFlag(state.mode == isa::Mode::Thumb);

  // IR steps, not guest instructions: Dynarmic's Block does not report how many encodings it consumed,
  // and calling that number something it is not would make Tier I's telemetry and Tier J's disagree in
  // the one place the two tiers are meant to be compared. C04's code cache can carry a real count.
  unsigned steps = 0;
  static unsigned run_index = 0;
  ++run_index;
  const char* tenv = std::getenv("RB_T1_TRACE");
  const bool trace = tenv != nullptr && static_cast<unsigned>(std::atoi(tenv)) == run_index;
  unsigned shown = 0;
  while (steps < max_instructions) {
    if (location.PC() == return_address_) {
      result.reason = StopReason::Returned;
      break;
    }
    if (dispatch_ != nullptr) {
      // Before translation, like the return sentinel: a host entry has no code to translate, and
      // mapping a page for it would be inventing guest memory to represent a host function.
      std::uint32_t slot = 0;
      if (dispatch_->IsHostEntry(location.PC(), &slot)) {
        result.reason = StopReason::HostCall;
        result.host_slot = slot;
        break;
      }
    }
    std::string err;
    auto block = isa::TranslateOne(memory_, location, &err);
    if (!block) {
      result.reason = StopReason::Fault;
      result.detail = std::move(err);
      break;
    }

    // A predicated *block*, not merely a predicated terminal. Dynarmic's A32 translator splits an IT
    // sequence into one block per leg: the block carries `GetCondition()` and a
    // `ConditionFailedLocation()`, and the AArch64 back end branches on the condition at the entry
    // label so a failed leg jumps to that location without executing a single instruction
    // (backend/arm64/emit_arm64.cpp:205-217). `frontend/A32/translate/conditional_state.cpp:45-49` is
    // where the pair is built, and the location it stores has already had `AdvancePC` and `AdvanceIT`
    // applied — so following it is all that is needed, and no IT bookkeeping belongs here.
    //
    // This was the missing piece, and the measured values are what found it: `it_probe(7,3)` returned
    // 1012 where silicon returns 42, which is precisely "both legs of `ite gt` ran, and the last one
    // won". `docs/32` §2.1 predicted this class of defect from bionic's 8,387 IT instructions; it is no
    // longer a prediction.
    const Dynarmic::IR::Cond entry_cond = block->ir.GetCondition();
    if (entry_cond != Dynarmic::IR::Cond::AL) {
      if (!Holds(entry_cond, Flags::from_cpsr(state.cpsr))) {
        if (!block->ir.HasConditionFailedLocation()) {
          result.reason = StopReason::Internal;
          result.detail = "a predicated block arrived with no condition-failed location";
          break;
        }
        // The leg is skipped whole. It still costs the loop a step, because a chain of skipped blocks
        // would otherwise spin inside one Run call without consuming any budget.
        location = LocationDescriptor{block->ir.ConditionFailedLocation()};
        state.mode = location.TFlag() ? isa::Mode::Thumb : isa::Mode::Arm;
        state.pc = location.PC();
        state.r[15] = location.PC();
        state.itstate = location.IT().Value();
        ++steps;
        result.reason = StopReason::Budget;
        continue;
      }
    }

    BlockRunner runner{memory_, state, location};
    LocationDescriptor successor{location};
    const bool contd = trace && ++shown <= 25;
    if (contd) {
      std::fprintf(stderr, "=== block pc=%08x cpsr=%08x r0=%x r1=%x r2=%x r4=%x r5=%x r13=%x r14=%x\n",
                   location.PC(), state.cpsr, state.r[0], state.r[1], state.r[2], state.r[4], state.r[5],
                   state.r[13], state.r[14]);
      std::fputs(Dynarmic::IR::DumpBlock(block->ir).c_str(), stderr);
    }
    const bool continued = runner.Run(*block, successor);
    if (contd) {
      std::fprintf(stderr, "--- after cpsr=%08x r0=%x r4=%x r5=%x r13=%x r14=%x pc=%08x\n", state.cpsr,
                   state.r[0], state.r[4], state.r[5], state.r[13], state.r[14], location.PC());
    }
    steps += runner.steps;
    if (!continued) {
      result = runner.stop;
      break;
    }
    location = successor;
    state.mode = location.TFlag() ? isa::Mode::Thumb : isa::Mode::Arm;
    state.pc = location.PC();
    state.r[15] = location.PC();
    state.itstate = location.IT().Value();
    result.reason = StopReason::Budget;
  }
  result.instructions_executed = steps;
  return result;
}

void InterpExecutor::Invalidate(GuestAddr from, GuestAddr to) {
  (void)from;
  (void)to;
}

}  // namespace rb::exec
