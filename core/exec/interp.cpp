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
 *   - **Dynarmic has no interpreter to reuse, and no hand-back on the host we ship on.** Its
 *     `IR::Term::Interpret` terminal marks instructions it declines to translate — six
 *     banked-register and privileged ARM-state forms, enumerated in `docs/32` §7. Only the **x64** back
 *     end turns that into a call to `UserCallbacks::InterpreterFallback` (`a32_emit_x64.cpp:1124-1134`,
 *     and `InterpreterFallback` appears nowhere else in `backend/`). The AArch64 back end's entire
 *     handling is `ASSERT_FALSE("Interpret should never be emitted.")` (`emit_arm64_a32.cpp:36-38`), and
 *     mcl's `ASSERT` is not `NDEBUG`-gated — it reaches `std::terminate`
 *     (`externals/mcl/src/assert.cpp:14-20`). So on
 *     arm64 a declined instruction ends the process during block emission: not a `StopReason`, not a
 *     resumable stop. Tier I therefore is *not* the JIT path's safety net here — it stands on its own,
 *     and this class refuses `Term::Interpret` by name rather than following it. The x64-only premise
 *     behind `docs/10` AD-2 consequence 3 is corrected there.
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

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include "dynarmic/interface/A32/config.h"  // `A32::Exception`, the operand `A32ExceptionRaised` carries
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

/**
 * CPSR's GE field ([19:16]) to the form dynarmic's `cpsr_ge` actually holds: one *whole byte lane*
 * (`0xFF`) per set GE bit, not a nibble.
 *
 * The two layouts are not interchangeable and the mismatch is silent. The back end expands on every
 * `MRS` and compresses on every `MSR` with a magic multiply (`emit_arm64_a32.cpp:333-338, 362-373`),
 * reading the GE bits out of **bit 7/15/23/31** of that word — so `GE0` corresponds to byte lane 0, the
 * *least* significant one, and an implementation that shifted the nibble into place instead of
 * expanding it would put every GE bit one lane high. `SEL` would then pick bytes from the wrong half of
 * the word, and `UADD8`'s carry reports would be off by a byte.
 */
std::uint32_t ExpandGe(std::uint32_t cpsr) {
  const std::uint32_t ge = (cpsr >> 16) & 0xFu;
  std::uint32_t out = 0;
  for (unsigned lane = 0; lane < 4; ++lane) {
    if (ge & (1u << lane)) out |= 0xFFu << (8 * lane);
  }
  return out;
}

/// The inverse: bit 7/15/23/31 of a 0xFF-per-lane GE word into CPSR [19:16].
std::uint32_t CompressGe(std::uint32_t lanes) {
  std::uint32_t out = 0;
  for (unsigned lane = 0; lane < 4; ++lane) {
    if (lanes & (0x80u << (8 * lane))) out |= 1u << (16 + lane);
  }
  return out;
}

/// A lane of a packed operand, as an unsigned or a signed value of `width` bits.
std::int64_t LaneOf(std::uint32_t v, unsigned shift, unsigned width, bool is_signed) {
  const std::uint32_t mask = (1u << width) - 1u;
  const std::uint32_t raw = (v >> shift) & mask;
  if (!is_signed) return raw;
  return raw & (1u << (width - 1)) ? std::int64_t(raw) - (1ll << width) : std::int64_t(raw);
}

/// Per-lane add or subtract, each result truncated back to the lane width.
///
/// There is no signed form of this on purpose: two's-complement addition and subtraction are the same
/// bit operation, which is why dynarmic's back end emits one `ADD Vd.8B, Vn.8B, Vm.8B` for both
/// `PackedAddU8` and `PackedAddS8` (`emit_arm64_packed.cpp:57,77`) and lets the *GE* word carry the
/// difference between them. An interpreter that branched on signedness here would produce the same
/// result by accident and hide the fact that the flag is the only thing that differs.
std::uint32_t LaneAddSub(std::uint32_t a, std::uint32_t b, unsigned width, bool subtract) {
  const std::uint32_t mask = (1u << width) - 1u;
  std::uint32_t out = 0;
  for (unsigned shift = 0; shift < 32; shift += width) {
    const std::uint32_t x = (a >> shift) & mask, y = (b >> shift) & mask;
    out |= ((subtract ? x - y : x + y) & mask) << shift;
  }
  return out;
}

/// Per-lane saturating add or subtract: clamp to the lane's own range instead of wrapping.
std::uint32_t LaneSaturate(std::uint32_t a, std::uint32_t b, unsigned width, bool subtract,
                           bool is_signed) {
  const std::uint32_t mask = (1u << width) - 1u;
  const std::int64_t lo = is_signed ? -(1ll << (width - 1)) : 0;
  const std::int64_t hi = is_signed ? (1ll << (width - 1)) - 1 : std::int64_t{mask};
  std::uint32_t out = 0;
  for (unsigned shift = 0; shift < 32; shift += width) {
    const std::int64_t x = LaneOf(a, shift, width, is_signed);
    const std::int64_t y = LaneOf(b, shift, width, is_signed);
    const std::int64_t r = subtract ? x - y : x + y;
    out |= (static_cast<std::uint32_t>(r < lo ? lo : r > hi ? hi : r) & mask) << shift;
  }
  return out;
}

/**
 * Per-lane halving add or subtract: the wider sum is shifted right one place and truncated, with **no
 * rounding** — which is the difference between these and AArch64's `URHADD`-family, and the thing
 * easiest to get wrong by writing the obvious `(x + y + 1) >> 1`.
 *
 * Two independent dynarmic paths say the same thing, which is why this is stated rather than assumed:
 * the AArch64 back end emits `UHADD`/`UHSUB`/`SHADD`/`SHSUB` (`emit_arm64_packed.cpp:286,296`), and the
 * x64 back end's portable route derives `~pavg(~a, ~b) == (a + b) >> 1` from `pavg`'s `+1` and then
 * removes it (`emit_x64_packed.cpp:256-259, 275-285`). A `+1` here would agree with neither and would
 * survive any test that only checked a small positive operand.
 */
std::uint32_t LaneHalve(std::uint32_t a, std::uint32_t b, unsigned width, bool subtract) {
  const std::uint32_t mask = (1u << width) - 1u;
  const std::uint32_t wide = (mask << 1) | 1u;  // the lane plus its carry/borrow bit
  std::uint32_t out = 0;
  for (unsigned shift = 0; shift < 32; shift += width) {
    const std::uint32_t x = (a >> shift) & mask, y = (b >> shift) & mask;
    const std::uint32_t d = (subtract ? x - y : x + y) & wide;
    // Bits [width:1] of the wider intermediate, ARM's own wording for UHADD8/UHSUB8/SHADD8/SHSUB8. For
    // the signed forms this is the arithmetic shift; the bit pattern is the same expression either way.
    out |= (d >> 1) << shift;
  }
  return out;
}

/**
 * The three things every `Packed*` opcode encodes in its name, factored out so a case list can stay a
 * list. The naming is dynarmic's (`opcodes.inc:221-255`) and follows ARM's: `U`/`S` is the lane's
 * signedness, `Add`/`Sub` the operation, and the trailing width is the *lane* width — 4 lanes of 8 bits
 * or 2 lanes of 16, both over the same 32-bit word.
 *
 * Spelled as switches over the opcodes rather than by reading `GetNameOf`, because these three answers
 * are what the arithmetic is built from: a name-parser that mis-reads `PackedAddSubU16` as an addition
 * of 8-bit lanes computes a plausible word for an instruction that is nothing of the kind, and the
 * failure surfaces as a wrong byte in a string comparison a thousand instructions later.
 */
bool IsPackedSub(Opcode op) {
  switch (op) {
    case Opcode::PackedSubU8:
    case Opcode::PackedSubS8:
    case Opcode::PackedSubU16:
    case Opcode::PackedSubS16:
    case Opcode::PackedSaturatedSubU8:
    case Opcode::PackedSaturatedSubS8:
    case Opcode::PackedSaturatedSubU16:
    case Opcode::PackedSaturatedSubS16:
    case Opcode::PackedHalvingSubU8:
    case Opcode::PackedHalvingSubS8:
    case Opcode::PackedHalvingSubU16:
    case Opcode::PackedHalvingSubS16:
      return true;
    default:
      return false;
  }
}
unsigned PackedLaneWidth(Opcode op) {
  switch (op) {
    case Opcode::PackedAddU16:
    case Opcode::PackedAddS16:
    case Opcode::PackedSubU16:
    case Opcode::PackedSubS16:
    case Opcode::PackedSaturatedAddU16:
    case Opcode::PackedSaturatedAddS16:
    case Opcode::PackedSaturatedSubU16:
    case Opcode::PackedSaturatedSubS16:
    case Opcode::PackedHalvingAddU16:
    case Opcode::PackedHalvingAddS16:
    case Opcode::PackedHalvingSubU16:
    case Opcode::PackedHalvingSubS16:
      return 16;
    default:
      return 8;
  }
}
bool IsPackedSigned(Opcode op) {
  switch (op) {
    case Opcode::PackedAddS8:
    case Opcode::PackedSubS8:
    case Opcode::PackedAddS16:
    case Opcode::PackedSubS16:
    case Opcode::PackedSaturatedAddS8:
    case Opcode::PackedSaturatedSubS8:
    case Opcode::PackedHalvingAddS8:
    case Opcode::PackedHalvingSubS8:
    case Opcode::PackedSaturatedAddS16:
    case Opcode::PackedSaturatedSubS16:
    case Opcode::PackedHalvingAddS16:
    case Opcode::PackedHalvingSubS16:
      return true;
    default:
      return false;
  }
}

/**
 * The GE word for the four packed add/subtract kinds, lane-for-lane as `0xFF` (or `0xFFFF`) per set bit.
 *
 * Written out per kind because the back end's four NEON recipes only look uniform:
 *
 *   - `UADD8`   `CMHI(Va, Vresult)` → carry out of the lane, i.e. `a > a+b`.
 *   - `USUB8`   `UHSUB` then `CMGE #0` → the halving difference is negative exactly when a borrow
 *              occurred, so this is `a >= b` — not `a > b`, which is the off-by-one this row exists
 *              to keep out.
 *   - `SADD8`   `SHADD` then `CMGE #0` → `(a+b+1)>>1 >= 0` as a signed lane, i.e. `a+b >= -1`.
 *   - `SSUB8`   `SHSUB` then `CMGE #0` → `(a-b)>>1 >= 0`, i.e. `a >= b` signed.
 *
 * The `>= 0` versus `> 0` question is real and has no single answer in dynarmic: the x64 back end
 * compares the *saturating* sum strictly greater than zero (`emit_x64_packed.cpp:41-63`), which differs
 * from arm64 on the exactly-zero lane (say `0x01 + 0xFF` as signed bytes). Tier I matches **arm64** —
 * which is the back end that ships on this host, and the differential is the invariant — and on
 * 2026-09-21 a 32-bit oracle run natively on both devices agreed with it (the `sadd8 zero lane` row in
 * `tests/guest/mediarows.c`, whose byte lane 1 sums to exactly zero), so the choice is no longer only a
 * matter of matching a sibling backend.
 */
std::uint32_t GeOfPackedAddSub(Opcode op, std::uint32_t a, std::uint32_t b, unsigned width) {
  const bool is_sub = op == Opcode::PackedSubU8 || op == Opcode::PackedSubU16 ||
                      op == Opcode::PackedSubS8 || op == Opcode::PackedSubS16;
  const bool is_signed = op == Opcode::PackedAddS8 || op == Opcode::PackedAddS16 ||
                         op == Opcode::PackedSubS8 || op == Opcode::PackedSubS16;
  const std::uint32_t mask = (1u << width) - 1u;
  std::uint32_t out = 0;
  for (unsigned shift = 0; shift < 32; shift += width) {
    const std::uint32_t x = (a >> shift) & mask, y = (b >> shift) & mask;
    bool set = false;
    if (is_signed) {
      const std::int64_t sx = LaneOf(a, shift, width, true), sy = LaneOf(b, shift, width, true);
      set = is_sub ? (sx - sy >= 0) : (sx + sy >= -1);
    } else {
      set = is_sub ? (x >= y) : ((x + y) & ~mask) != 0;  // carry out of the lane
    }
    if (set) out |= mask << shift;
  }
  return out;
}

/**
 * Scalar VFP helpers.
 *
 * The IR carries floats as *bit patterns* in `U32`/`U64` operands — `GetExtendedRegister32` hands out
 * S&lt;n&gt;'s bits, not a host `float` — so every op below begins and ends with an integer, and the
 * reinterpretation is the only conversion that is not architectural.
 *
 * Rounding is the part worth being precise about. The translator resolves FPSCR's RMode field **at
 * translation time** and passes it as a `U8` immediate (`ir_emitter.cpp:2359-2400`), which is why the
 * mode is part of block identity and why the AArch64 back end can `ASSERT(rounding_mode ==
 * ctx.FPCR().RMode())` (`emit_arm64_floating_point.cpp:68`) rather than switch at runtime. Tier I reads
 * the same immediate, so both tiers round the same way for the same block — with one exception, stated
 * where it is used: the back end uses native AArch64 `FCVT*`, which honours every mode in the source
 * precision, while Tier I's `f64`→`f32` narrowing goes through the host conversion, which is
 * round-to-nearest. `FPSCR.RMode` has four values (0-3) and three of them are not nearest, so a guest
 * that changes RMode and then narrows is where the two would part. No probe here does, and the
 * differential would report it if one did.
 */
template <class F, class B>
F BitsAs(B b) {
  static_assert(sizeof(F) == sizeof(B), "a bit pattern has to be the width of the type it becomes");
  F f{};
  std::memcpy(&f, &b, sizeof f);
  return f;
}

template <class B, class F>
B AsBits(F f) {
  static_assert(sizeof(F) == sizeof(B), "a bit pattern has to be the width of the type it came from");
  B b{};
  std::memcpy(&b, &f, sizeof b);
  return b;
}

/// FPSCR's sticky exception bits, at the positions `Dynarmic::FP::FPSR` uses (`common/fp/fpsr.h:88-138`:
/// IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7). These are the *ARMv8* positions. The legacy ARMv7
/// locations in [12:8] read as zero on the device measured here, which is why the first FPSCR probe
/// looked at the wrong half of the word and reported the flags as unexercised.
enum FpSticky : std::uint32_t {
  kIoc = 1u << 0,
  kDzc = 1u << 1,
  kOfc = 1u << 2,
  kUfc = 1u << 3,
  kIxc = 1u << 4,
};
constexpr std::uint32_t kFpStickyMask = 0x0000'009fu;

/**
 * Fold in whatever the host FPU flagged while computing the operation that just ran.
 *
 * Not a shortcut around softfloat's flag logic — it is the same source the AArch64 back end uses:
 * native `FDIV`/`FSQRT`/`FMADD` set the host FPSR and the back end stores those bits into guest state
 * (`ctx.fpsr.Load()` on every FP op), so on that path the guest's cumulative flags *are* the host's.
 * Tier I reaching the same bits through `<cfenv>` is agreement, not approximation. Without it Tier I
 * left FPSCR at 0 where hardware and Tier J both had 0x10 — IXC, from an inexact divide — and no
 * probe's returned value showed the difference; only the state diff did (`docs/30` §Step 16).
 */
std::uint32_t TakeHostFpFlags() {
  const int raised = std::fetestexcept(FE_ALL_EXCEPT);
  std::feclearexcept(FE_ALL_EXCEPT);
  return (raised & FE_INVALID ? kIoc : 0u) | (raised & FE_DIVBYZERO ? kDzc : 0u) |
         (raised & FE_OVERFLOW ? kOfc : 0u) | (raised & FE_UNDERFLOW ? kUfc | kIxc : 0u) |
         (raised & FE_INEXACT ? kIxc : 0u);
}

/// Clears the host flags for one FP operation and merges into the guest FPSCR whatever it raised.
class FpOps {
 public:
  explicit FpOps(std::uint32_t& fpscr) : fpscr_(fpscr) { std::feclearexcept(FE_ALL_EXCEPT); }
  ~FpOps() { fpscr_ |= TakeHostFpFlags() & kFpStickyMask; }

  /// For an operation whose inexactness or invalidity is known without asking the host: a rounding
  /// step, a clamp, or a compare that is unordered. Clearing afterwards is what keeps each
  /// operation's flags its own rather than marking everything after the first exception.
  void Raise(std::uint32_t bits) {
    std::feclearexcept(FE_ALL_EXCEPT);
    fpscr_ |= bits & kFpStickyMask;
  }

 private:
  std::uint32_t& fpscr_;
};

/// `FP::RoundingMode`, as the translator's `U8` immediates. 4 and 5 (ties-away, ToOdd) are not
/// reachable from ARMv7's two-bit RMode field, and the back end's own `ToOdd` case is
/// `ASSERT_FALSE("Unimplemented")`.
enum Rp : std::uint8_t { kRn = 0, kRp = 1, kRm = 2, kRz = 3 };

/**
 * Round to an integer per ARM's four RMode values, and say whether rounding changed the value.
 *
 * Spelled out rather than delegated to `std::nearbyint`, which follows the *host's current* rounding
 * direction: Dynarmic's back end writes that direction into the host FPCR from the guest FPSCR
 * (`FPCR::WriteRaw`), so leaning on it would make the two tiers agree only while both had set it the
 * same way — a dependency on global state that the differential cannot see. Ties-to-even is the case
 * that needs the floor-and-test; the other three are one host call each.
 */
template <class F>
F RoundToInt(F x, std::uint8_t rm, bool* inexact) {
  F r{};
  switch (rm) {
    case kRn: {
      const F f = std::floor(x);
      const F d = x - f;
      // Ties go to the even neighbour. `fmod` rather than `%` because the remainder's sign has to be
      // the divisor's: -2.5 and -1.5 both have to land on -2, which a truncating-removal test gets
      // wrong for exactly one of them.
      r = (d > F{0.5} || (d == F{0.5} && std::fmod(f, F{2}) != F{0})) ? f + F{1} : f;
      break;
    }
    case kRp: r = std::ceil(x); break;
    case kRm: r = std::floor(x); break;
    default: r = std::trunc(x); break;
  }
  if (inexact != nullptr) *inexact = !(r == x) && !std::isnan(x);
  return r;
}

/**
 * ARM's convert-to-fixed-point saturates, and what it saturates *to* is the interesting half: NaN
 * becomes the sign-preferring extreme (0x80000000 signed, 0 unsigned) rather than either bound, and an
 * out-of-range value raises IOC as it clamps. AArch64's `FCVT{N,Z,P,M}{S,U}` — what Tier J compiles
 * these to — follow the same rules, which is why this is a clamp-and-special-case rather than a cast:
 * `(std::int32_t)some_huge_double` is *undefined* in C++, and an optimiser may hand back anything,
 * which is the shape of a heisenbug that only appears at -O3.
 */
std::uint64_t ToFixed(double rounded, bool is_signed, unsigned dst_bits, std::uint32_t* flags) {
  if (std::isnan(rounded)) {
    *flags |= kIoc;
    return is_signed ? (1ull << (dst_bits - 1)) : 0ull;
  }
  const double lo = is_signed ? -std::ldexp(1.0, static_cast<int>(dst_bits) - 1) : 0.0;
  const double hi = is_signed ? std::ldexp(1.0, static_cast<int>(dst_bits) - 1) - 1.0
                              : std::ldexp(1.0, static_cast<int>(dst_bits)) - 1.0;
  if (!(rounded < hi)) {
    *flags |= kIoc;
    return is_signed ? static_cast<std::uint64_t>(static_cast<std::int64_t>(hi))
                     : static_cast<std::uint64_t>(hi);
  }
  if (!(rounded > lo)) {
    // Below range raises IOC and clamps; *equal* to the bound is exact and raises nothing, which is
    // the distinction the unsigned case turns on (a negative value clamps to 0 and is invalid, while
    // the already-zero value is simply zero).
    if (rounded != lo) *flags |= kIoc;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
  }
  return is_signed ? static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded))
                   : static_cast<std::uint64_t>(static_cast<std::uint64_t>(rounded));
}

/// Lane width in bits, for the integer ASIMD family whose opcodes are the same operation at 8, 16, 32
/// and 64 bits. Listed exhaustively rather than parsed out of the opcode's name: a name-based reader
/// would silently accept `VectorSignedSaturatedAdd8` or reject something new, and either mistake turns
/// into arithmetic on the wrong lane width — a wrong answer, not a fault. Anything not listed returns
/// 0, which the caller turns into a named refusal.
unsigned LaneBits(Dynarmic::IR::Opcode op) {
  using Dynarmic::IR::Opcode;
  switch (op) {
    case Opcode::VectorAdd8: case Opcode::VectorSub8: case Opcode::VectorMultiply8:
    case Opcode::VectorAbs8: case Opcode::VectorGetElement8: case Opcode::VectorSetElement8:
      return 8;
    case Opcode::VectorAdd16: case Opcode::VectorSub16: case Opcode::VectorMultiply16:
    case Opcode::VectorAbs16: case Opcode::VectorGetElement16: case Opcode::VectorSetElement16:
      return 16;
    case Opcode::VectorAdd32: case Opcode::VectorSub32: case Opcode::VectorMultiply32:
    case Opcode::VectorAbs32: case Opcode::VectorGetElement32: case Opcode::VectorSetElement32:
    case Opcode::VectorBroadcast32:
      return 32;
    case Opcode::VectorAdd64: case Opcode::VectorSub64: case Opcode::VectorMultiply64:
    case Opcode::VectorAbs64: case Opcode::VectorGetElement64: case Opcode::VectorSetElement64:
    case Opcode::VectorBroadcast64:
      return 64;
    case Opcode::VectorBroadcast8: return 8;
    case Opcode::VectorBroadcast16: return 16;
    default: return 0;
  }
}

/// Sign-extend a lane of `bits` width held in the low bits of `value`. Written here rather than
/// routed through `SignExtend` below, which takes a target width and lives further down the file.
///
/// `bits == 64` needs its own branch: the shift form below evaluates `1ull << 64`, which is undefined
/// rather than zero, and hardware masks the shift count to 6 bits so it yields `1ull << 0`. The
/// observable result is that `vabs` on a 64-bit lane silently computes from a lane of 0 — a wrong
/// answer, which is the failure mode this whole file is trying not to have.
std::int64_t SignedAt(std::uint64_t value, unsigned bits) {
  if (bits >= 64) return static_cast<std::int64_t>(value);
  const std::uint64_t low = value & ((1ull << bits) - 1);
  const std::uint64_t sign = 1ull << (bits - 1);
  return static_cast<std::int64_t>((low ^ sign) - sign);
}

/// How many lanes of `bits` width a 128-bit vector register holds, for validating a
/// `VectorGetElement`/`VectorSetElement` index.
///
/// The index arrives as an IR `U8` argument, so it is data this file does not control: the A32
/// translator supplies element indices as immediates today, but an out-of-range index from a
/// mis-translated or future path would otherwise read — or, for a set, *write* — past the end of the
/// 16-byte value on the stack. Bounding it here is what keeps a malformed guest binary from becoming
/// host memory corruption inside the emulator.
unsigned LaneCount(unsigned bits) { return bits == 0 ? 0 : 16u * 8u / bits; }

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
  BlockRunner(mem::GuestMemory& memory, MachineState& state, LocationDescriptor& location,
              ExclusiveState& excl)
      : memory_{memory}, state_{state}, location_{location}, excl_{excl} {}

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

  /// The same, for a message assembled from the offending values. Named overloads rather than a
  /// `string_view` parameter because the call sites that need this one build a `std::string` inline,
  /// and a `const char*` taken from a temporary at the call boundary is exactly the thing to avoid.
  bool Fail(const Inst* inst, const std::string& why) { return Fail(inst, why.c_str()); }

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

  /// An `Imm8` operand, which is how the translator attaches non-data parameters to an instruction:
  /// the FP conversions carry their fixed-point scale and their resolved rounding mode this way, and
  /// both tiers read the same immediate, so neither can drift from the other on a per-block basis.
  [[nodiscard]] bool ByteArg(const Inst* inst, std::size_t i, std::uint8_t& out) {
    std::uint64_t v = 0;
    if (!W64(inst, i, v)) return false;
    out = static_cast<std::uint8_t>(v);
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
  /// A write to the program counter. `interwork` is the `BX` rule — bit 0 of the value selects the ISA —
  /// and it belongs to exactly one opcode. Everything else that reaches PC arrives already masked, and
  /// changing the mode there would be inventing a state transition the guest did not ask for.
  void WritePC(std::uint32_t value, bool interwork);
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
  /// Passed in, not a member of this class: the exclusive reservation has to outlive a block, and this
  /// object deliberately does not (see the comment on the class).
  ExclusiveState& excl_;
  std::unordered_map<const Inst*, V> temps_;
  std::unordered_map<const Inst*, Flags> produced_;
  /// Shifter carries, kept apart from `produced_` because their N/Z reach CPSR by a different route
  /// (`TST`) than their C does (`GetCarryFromOp`), and merging them would make Tier I disagree with
  /// the back end about what a `GetNZCVFromOp` on a shift means.
  std::unordered_map<const Inst*, bool> carry_;
  /// The GE word a packed add/subtract produced, in dynarmic's expanded one-lane-per-bit form, keyed by
  /// the instruction that produced it — the same shape as `carry_` and for the same reason: the IR names
  /// the *producer* in `GetGEFromOp`, so the value has to be recorded where a producer can leave it and
  /// read where a consumer asks. Held apart from `produced_` because it is not a flag: it survives an
  /// unrelated `ADDS` in between, and `MRS rD, CPSR` reads it out of CPSR's own bits.
  std::unordered_map<const Inst*, std::uint32_t> ge_;
  bool check_bit = false;
  /// An SVC noted by `A32CallSupervisor`, waiting to be surfaced at the block's `CheckHalt` terminal —
  /// see the two places that read it. Pending rather than immediate so the PC is already past the
  /// instruction, which is what Tier J's back end reports and what the syscall monitor resumes from.
  bool svc_pending_ = false;
  std::uint32_t svc_ = 0;
};

void BlockRunner::WriteReg(Reg reg, std::uint32_t value) {
  if (reg == Reg::PC) {
    // `A32SetRegister(PC, v)`. Dynarmic's `IREmitter::BranchWritePC` masks the target itself and then
    // emits exactly this op (`a32_ir_emitter.cpp:101-110`), so a write arriving here has *already* had
    // its low bits cleared and carries no ISA tag: the mode has to survive it. Only `BXWritePC`
    // interworks, and it is a distinct opcode handled below.
    //
    // Getting this wrong is invisible until something exercises a non-`bx` PC write, because the ordinary
    // ones (`bx lr`, `ldr pc, [sp]`) go through `BXWritePC` with a tag-bearing value. The first one in
    // this corpus is a syscall's continuation — `BranchWritePC(pc+2)`, an even address — and Tier I took
    // that to mean "switch to ARM", then decoded its own Thumb code with the ARM decoder and reported an
    // exception at an instruction that had run fine a moment earlier.
    WritePC(value, /*interwork=*/false);
    return;
  }
  state_.r[static_cast<int>(reg)] = value;
}

void BlockRunner::WritePC(std::uint32_t value, bool interwork) {
  const bool was_thumb = state_.mode == isa::Mode::Thumb;
  // In Thumb the low bit of a branch target is the ISA tag rather than part of the address; in ARM it is
  // not a tag at all. `interwork` is the `BX` rule, which reads that bit; the other case keeps whatever
  // mode the block was translated for.
  const bool thumb = interwork ? (value & 1u) != 0 : was_thumb;
  const std::uint32_t next = thumb ? (value & ~1u) : (value & ~3u);
  state_.mode = thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  location_ = location_.SetPC(next).SetTFlag(thumb);
  state_.pc = next;
  state_.r[15] = next;
  // CPSR bit 5 *is* the mode — see `CpsrWithMode` for why both tiers keep that true on the way out.
  state_.cpsr = CpsrWithMode(state_.cpsr, thumb ? isa::Mode::Thumb : isa::Mode::Arm);
}

/**
 * The one coprocessor register an ARMv7 guest may read for its thread pointer: `c13`, with `opc1 == 0`,
 * `CRm == c0` and `opc2 == 3` — i.e. `mrc p15, #0, rX, c13, c0, #3`, which is what bionic's `__get_tls()`
 * compiles to.
 *
 * The indices are the front end's packing, read straight out of `emit_arm64_a32_coprocessor.cpp`:
 * `[0]` coprocessor number, `[1]` the "2" extension bit, `[2]` opcode 1, `[3]` CRn, `[4]` CRm,
 * `[5]` opcode 2. Same order Tier J sees, which is the point of naming the check once per tier rather than
 * inlining an index comparison twice.
 */
bool IsThreadPointerAccess(const Dynarmic::IR::Value::CoprocessorInfo& i) {
  return i[0] == 15 && i[1] == 0 && i[2] == 0 && i[3] == 13 && i[4] == 0 && i[5] == 3;
}

std::string CoprocName(const char* mnemonic, const Dynarmic::IR::Value::CoprocessorInfo& i) {
  return std::string{mnemonic} + (i[1] ? "2" : "") + " p" + std::to_string(static_cast<unsigned>(i[0])) +
         ", #" + std::to_string(static_cast<unsigned>(i[2])) + ", c" +
         std::to_string(static_cast<unsigned>(i[3])) + ", c" +
         std::to_string(static_cast<unsigned>(i[4])) + ", #" +
         std::to_string(static_cast<unsigned>(i[5]));
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

    case Opcode::A32ExceptionRaised: {
      /**
       * The exception op's own operands answer the question this file previously could only gesture at.
       * Dynarmic emits it as `Inst(A32ExceptionRaised, Imm32(current_location.PC()),
       * Imm64(exception))` (`a32_ir_emitter.cpp:134`), so argument 0 is the **address of the instruction
       * that raised** and argument 1 is the exception — which is better than anything reachable from the
       * block's location descriptor. The first version of this message printed the block entry and
       * warned that it might not be the raising instruction; for `pld` it was off by exactly one
       * encoding, and the probe rows in `core/tests/exec_tier_diff_test.cpp` were what showed it.
       */
      std::uint64_t where = 0, kind = 0;
      if (!HasArgs(inst, 2) || !ImmediateAsU64(inst->GetArg(0), where) ||
          !ImmediateAsU64(inst->GetArg(1), kind)) {
        const GuestAddr at = location_.PC();
        std::uint32_t w = 0;
        const bool readable = memory_.Read(at, &w, 4);
        return Fault("the front end raised an exception in the block entering at 0x" + Hex(at) +
                     (location_.TFlag() ? " (Thumb, halfwords " : " (ARM, word ") +
                     (readable ? Hex(w & 0xFFFFu) + " " + Hex(w >> 16) : "<unreadable>") + ")");
      }
      using Exception = Dynarmic::A32::Exception;
      if (isa::HintIsNoOp(static_cast<Exception>(kind))) {
        /**
         * A hint, executed as the no-op the architecture says it is. `pld` specifically is the case that
         * was blocking real code: bionic's selected `memcpy`/`memmove` on both test devices is
         * `memmove_a15`, which contains ten of them, and refusing one stopped Tier I inside `memcpy`
         * while Tier J ran the routine to the end.
         *
         * Tier J ran it because Dynarmic's `arm_PLD_imm` emits **nothing at all** unless
         * `hook_hint_instructions` is set (`frontend/A32/translate/impl/hint.cpp:11-18`), and Tier J's
         * `UserConfig` never set it — so until `docs/30` §Step 37 the two tiers were not handed the same IR
         * here: Tier I got an `A32ExceptionRaised` to answer and Tier J got no opcode to answer with. The
         * policy is now `isa::HintIsNoOp`, one predicate both tiers consult, with Tier J's config brought
         * in line so the raise actually reaches it.
         *
         * Not a tolerance: the raise op is not the block terminator, so returning true lets the rest of the
         * block run as if the instruction had not been there, which is what the hint does on hardware.
         * `tests/guest/pldrows.c` measured that on the SM7250 across three address classes, including a
         * hole where a real `ldr` took `SIGSEGV`.
         */
        return true;
      }
      // The exception *number* rather than a name: `jit.cpp`'s `ExceptionName` owns that table, and a
      // second copy here would be a second thing allowed to drift. The number indexes that table directly.
      //
      // The reason code is chosen to match Tier J's `ExceptionRaised` for the same event, because the
      // differential compares reasons and two tiers naming one raise differently is the thing it exists to
      // catch. That mapping is: a failed instruction fetch is a `Fault` the guest would have seen, a
      // breakpoint suspends, and anything else -- an undefined or unmodelled encoding, an unimplemented
      // hint -- is `Unimplemented`, not a fault. Making every raise a `Fault` was my error two revisions
      // ago and the two lanes that caught it are `docs/30` §Step 37's record of it.
      const Exception raised = static_cast<Exception>(kind);
      const std::string where_text = "the A32 front end raised exception " + std::to_string(kind) +
                                     " at 0x" + Hex(static_cast<std::uint32_t>(where));
      if (raised == Exception::NoExecuteFault) return Fault(where_text);
      if (raised == Exception::Breakpoint) {
        stop.reason = StopReason::Suspended;
        stop.detail = where_text;
        return false;
      }
      return Fail(inst, where_text);
    }

    /**
     * Coprocessor 15, and only the thread pointer.
     *
     * The same rule `Arm15Coprocessor` encodes for Tier J, and it has to be the same rule: the two tiers
     * may differ in speed and in step accounting, but a register one tier can read and the other cannot
     * turns every future differential into a tier-dependent answer about the guest's TLS. What the device's
     * `libc.so` actually contains is 98 reads of exactly this register and no other coprocessor access at
     * all (`llvm-objdump -d` over the whole image), so this covers real bionic rather than guessing at it.
     *
     * Anything else *refuses*, naming the coordinates, rather than returning zero: a guest that
     * dereferences a fabricated coprocessor read gets a wrong answer, and in an emulator a wrong answer is
     * worse than a stop. Tier J's equivalent of that refusal has to be a stop too, because Dynarmic's own
     * answer to an unhandled coprocessor is `ASSERT_FALSE` in the AArch64 back end, which terminates the
     * host process — see `docs/30` §Step 26.
     */
    case Opcode::A32CoprocGetOneWord: {
      if (!HasArgs(inst, 1)) return false;
      const auto info = inst->GetArg(0).GetCoprocInfo();
      if (!IsThreadPointerAccess(info)) {
        return Fail(inst, CoprocName("MRC", info) + " is not modelled");
      }
      Put(inst, V{state_.tp, 0});
      return true;
    }
    case Opcode::A32CoprocSendOneWord: {
      if (!HasArgs(inst, 2)) return false;
      const auto info = inst->GetArg(0).GetCoprocInfo();
      if (!IsThreadPointerAccess(info)) {
        return Fail(inst, CoprocName("MCR", info) + " is not modelled");
      }
      std::uint32_t value = 0;
      if (!W32(inst, 1, value)) return false;
      state_.tp = value;
      return true;
    }

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

    // ---------------------------------------------------------------- CPSR's GE bits
    //
    // GE exists because four instructions report a *per-lane* result and none of them has a flag field
    // wide enough: `UADD8`'s carry, `USUB8`'s no-borrow, and the signed pair. It is also the only thing
    // `SEL` reads, which is why the state has to be right rather than merely present — a GE that is
    // never written lets `UADD8` look fine and `SEL` silently pick the wrong bytes.
    //
    // Tier I keeps these bits inside `state_.cpsr` at their architectural positions [19:16]; dynarmic's
    // back end keeps a separate expanded `cpsr_ge` word and converts at every read and write. That is a
    // representation difference, not a semantic one, and the rows that pin it down are `MRS rD, CPSR`
    // after a `UADD8` (which reads exactly what this packing produced) and `MSR` (which writes it).
    case Opcode::A32GetGEFlags:
      Put(inst, V{ExpandGe(state_.cpsr), 0});
      return true;
    case Opcode::A32SetGEFlags: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.cpsr = (state_.cpsr & ~0x000f'0000u) | CompressGe(v);
      return true;
    }
    case Opcode::A32SetGEFlagsCompressed: {
      // The `MSR` route: the translator hands over `value & 0x000F0000`, already in CPSR's layout, so
      // this is a masked store with no lane arithmetic — and the mask has to be applied to the *source*
      // as well as the destination here rather than trusting the caller, because this opcode is also
      // reachable from an IR the translator did not build.
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      state_.cpsr = (state_.cpsr & ~0x000f'0000u) | (v & 0x000f'0000u);
      return true;
    }
    case Opcode::GetGEFromOp: {
      if (!HasArgs(inst, 1)) return false;
      const Value arg = inst->GetArg(0);
      if (arg.IsImmediate()) return Fail(inst, "a GE pseudo-operation on an immediate has no producer");
      const auto it = ge_.find(arg.GetInst());
      if (it == ge_.end()) {
        // Refusing rather than answering 0: the back end asserts the producer defined the value
        // (`emit_arm64.cpp:80-84`), so a missing producer means this file computed the op without
        // recording its GE, which is a bug in the row above and not a state the guest can be in. Zero
        // would turn that bug into quietly-wrong `SEL` results.
        return Fail(inst, "no GE recorded for " +
                              std::string{Dynarmic::IR::GetNameOf(arg.GetInst()->GetOpcode())});
      }
      Put(inst, V{it->second, 0});
      return true;
    }

    // ---------------------------------------------------------------- the ARMv5TE/v6 media ("packed") ops
    //
    // Implemented as one set rather than one at a time on demand, because the arithmetic below is shared
    // and because a per-byte subset invites the mistake this shape has: `UADD8` wrapping while `UQADD8`
    // clamping are four lines apart in the same table, and adding only the first leaves the second
    // refusing in a library that needed both.
    //
    // Deliberately **not** here: `PackedAddSubU16`/`PackedAddSubS16`/`PackedSubAddU16`/`PackedSubAddS16`
    // (the `UADDASX` family, whose lane-to-GE mapping dynarmic does with a `shl 15; sar 31` recipe I have
    // not read in the AArch64 emitter) and `PackedAbsDiffSumU8` (`USAD8`, whose arm64 path ends in an
    // `AND` with a replicated `0x0F` and a `UADDLV` whose half-lane accumulation I have not derived).
    // Each still refuses by name, and the refusal is the reason: an op whose semantics I cannot state
    // precisely is one I cannot tell apart from a correct implementation when the guest's answer merely
    // looks plausible. Neither appears in the disassembly of either device's `libc.so` string routines.
    //
    // `PackedSelect` **is** here, because the device's own `strcmp` asked for it: `rb_guest_libc_tests`
    // refused `PackedSelect: no Tier I semantics yet` on a real `libc.so` (`docs/30` §Step 34). That is
    // worth recording as a correction, because an earlier revision of this file excluded `SEL` on the
    // grounds that the ARMv6 media instructions are unreachable on this hardware — a claim resting on a
    // probe whose result is now disproved: `tests/guest/mediarows.c` executes `uadd8`, `usub8`, `sadd8`,
    // `ssub8`, `uqadd8`, `uqsub8`, `qsub8`, `uhadd8`, `uhsub8`, `shadd8`, `uadd16`, `usub16` and `sel`
    // natively at EL0 on both the SM7250 and the SM8550, all without faulting. A `SIGILL` says only that
    // the encoding which was tried was not executed, which for an instruction reached through the wrong
    // decoder is the
    // same observable as an unavailable instruction — and nothing in this repo should treat the earlier
    // probe as a fact about availability again.
    case Opcode::PackedSelect: {
      if (!HasArgs(inst, 3)) return false;
      std::uint32_t ge = 0, to = 0, from = 0;
      if (!W32(inst, 0, ge) || !W32(inst, 1, to) || !W32(inst, 2, from)) return false;
      /**
       * `SEL`: byte lane *i* takes `Rn` (`from`) where GE_i is set and `Rm` (`to`) where it is clear —
       * ARM's definition, and the law dynarmic's x64 back end implements (`(from & ge) | (to & ~ge)`,
       * `emit_x64_packed.cpp:654-690`).
       *
       * Stated as a choice at the time, because its AArch64 sibling expresses the same thing as
       * `FMOV(Vresult, Vge); BSL(Vresult, Vb, Va)` (`emit_arm64_packed.cpp:395-407`) and the operand
       * order in that one line is not something to read the polarity out of without running it. The
       * `sel from mixed GE` row now settles it against hardware on both devices: with GE = 0b0100 and
       * operands `0x00FF7F80`/`0x01010101`, the answer is `0x01FF0101`, which is this expression and not
       * the reversed one (`0x007F0180`). Tier J does not reach that answer — task #43 — so the row is
       * evidence for this line from the oracle, not from the other tier.
       */
      Put(inst, V{(from & ge) | (to & ~ge), 0});
      return true;
    }
    case Opcode::PackedAddU8:
    case Opcode::PackedAddS8:
    case Opcode::PackedSubU8:
    case Opcode::PackedSubS8:
    case Opcode::PackedAddU16:
    case Opcode::PackedAddS16:
    case Opcode::PackedSubU16:
    case Opcode::PackedSubS16: {
      if (!HasArgs(inst, 2)) return false;
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      const unsigned width = PackedLaneWidth(op);
      Put(inst, V{LaneAddSub(a, b, width, IsPackedSub(op)), 0});
      ge_[inst] = GeOfPackedAddSub(op, a, b, width);
      return true;
    }
    case Opcode::PackedSaturatedAddU8:
    case Opcode::PackedSaturatedAddS8:
    case Opcode::PackedSaturatedSubU8:
    case Opcode::PackedSaturatedSubS8:
    case Opcode::PackedSaturatedAddU16:
    case Opcode::PackedSaturatedAddS16:
    case Opcode::PackedSaturatedSubU16:
    case Opcode::PackedSaturatedSubS16: {
      if (!HasArgs(inst, 2)) return false;
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      // No GE, no C, no Q: the saturating forms report nothing, which is what makes them a different
      // instruction rather than a flag variant (`parallel.cpp:295-308` gives the translator's own
      // `SetRegister` and nothing else). dynarmic's AArch64 emitter adds an FPSR spill around these
      // (`emit_arm64_packed.cpp:35,42`) for the *FP* state its host shares, not for anything an
      // AArch32 guest can observe here.
      Put(inst, V{LaneSaturate(a, b, PackedLaneWidth(op), IsPackedSub(op), IsPackedSigned(op)), 0});
      return true;
    }
    case Opcode::PackedHalvingAddU8:
    case Opcode::PackedHalvingAddS8:
    case Opcode::PackedHalvingSubU8:
    case Opcode::PackedHalvingSubS8:
    case Opcode::PackedHalvingAddU16:
    case Opcode::PackedHalvingAddS16:
    case Opcode::PackedHalvingSubU16:
    case Opcode::PackedHalvingSubS16: {
      if (!HasArgs(inst, 2)) return false;
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{LaneHalve(a, b, PackedLaneWidth(op), IsPackedSub(op)), 0});
      return true;
    }

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
    /**
     * `ConditionalSelect64` — the back end loads `cpsr_nzcv` and emits `CSEL Xd, Xn, Xm, cond`
     * (emit_arm64_data_processing.cpp:220), so the choice reads the guest's architectural NZCV, which
     * `A32SetCpsrNZCV` writes into `state_.cpsr`. An AArch32 guest reaches this op from one place only:
     * `VSEL` on a D register (vfp.cpp:353), whose FP compare (`A32SetCpsrNZCV`) is the flag write that
     * feeds it — hence the same read `Term::If` below uses.
     */
    case Opcode::ConditionalSelect64: {
      if (!HasArgs(inst, 3)) return false;
      std::uint64_t a = 0, b = 0;
      if (!W64(inst, 1, a) || !W64(inst, 2, b)) return false;
      const bool then_arm = Holds(inst->GetArg(0).GetCond(), Flags::from_cpsr(state_.cpsr));
      Put(inst, V{then_arm ? a : b, 0});
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

    // ---------------------------------------------------------------- exclusive accesses (LL/SC)
    //
    // The same rule Tier J runs, in one place: `ExclusiveState`. What the *result* means is worth stating
    // because it is inverted from intuition — `Rd = 0` means the store **passed**, and 1 means it failed —
    // which is ARM's convention and the one Dynarmic's trampoline produces too
    // (`a32_address_space.cpp:136-141` returns `? 0 : 1`).
    case Opcode::A32ExclusiveReadMemory8:
    case Opcode::A32ExclusiveReadMemory16:
    case Opcode::A32ExclusiveReadMemory32:
    case Opcode::A32ExclusiveReadMemory64: {
      std::uint64_t addr = 0;
      if (!W64(inst, 1, addr)) return false;
      const std::size_t bytes = op == Opcode::A32ExclusiveReadMemory8    ? 1
                                : op == Opcode::A32ExclusiveReadMemory16 ? 2
                                : op == Opcode::A32ExclusiveReadMemory32 ? 4
                                                                         : 8;
      unsigned char raw[8] = {};
      if (!ReadMem(addr, raw, bytes, inst)) return false;
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(raw[i]) << (i * 8);  // little-endian, as ARM is
      }
      excl_.Mark(addr, value);
      Put(inst, V{value, 0});
      return true;
    }
    case Opcode::A32ExclusiveWriteMemory8:
    case Opcode::A32ExclusiveWriteMemory16:
    case Opcode::A32ExclusiveWriteMemory32:
    case Opcode::A32ExclusiveWriteMemory64: {
      std::uint64_t addr = 0;
      std::uint64_t want = 0;
      if (!W64(inst, 1, addr)) return false;
      if (!W64(inst, 2, want)) return false;
      const std::size_t bytes = op == Opcode::A32ExclusiveWriteMemory8    ? 1
                                : op == Opcode::A32ExclusiveWriteMemory16 ? 2
                                : op == Opcode::A32ExclusiveWriteMemory32 ? 4
                                                                          : 8;
      bool passed = false;
      // A rejected store reads nothing and writes nothing, so an unmapped target is a *failure* rather
      // than a fault exactly when the reservation is absent — see `AdmitAddress`.
      if (excl_.AdmitAddress(addr)) {
        unsigned char raw[8] = {};
        if (!ReadMem(addr, raw, bytes, inst)) return false;
        std::uint64_t current = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
          current |= static_cast<std::uint64_t>(raw[i]) << (i * 8);
        }
        passed = excl_.ValueUnchanged(current);
        if (passed) {
          for (std::size_t i = 0; i < bytes; ++i) {
            raw[i] = static_cast<unsigned char>(want >> (i * 8));
          }
          if (!WriteMem(addr, raw, bytes, inst)) return false;
        }
      }
      Put(inst, V{passed ? 0u : 1u, 0});
      return true;
    }
    case Opcode::A32ClearExclusive:
      // `clrex` drops the marker and nothing else: bionic's `pthread_mutex_trylock` uses it after a failed
      // attempt precisely so the next caller's `ldaex` starts clean. Dynarmic's back end clears only its
      // own marker here, not the monitor's address, which is equivalent because a store cannot pass
      // without a fresh exclusive load re-marking it.
      excl_.Clear();
      return true;

    // ---------------------------------------------------------------- control transfer
    case Opcode::A32BXWritePC: {
      std::uint32_t target = 0;
      if (!W32(inst, 0, target)) return false;
      // The one interworking PC write: bit 0 of the target is the ISA tag, so `bx lr` out of a Thumb
      // function and `bx` into an ARM one differ only in that bit. Dynarmic ends the block after this
      // with PopRSBHint or Interpret, and its own lowering of this op is what reads bit 0
      // (`backend/arm64/emit_arm64_a32.cpp:551-577`) — `BranchWritePC` is not this op, so the rule does
      // not belong anywhere else.
      WritePC(target, /*interwork=*/true);
      return true;
    }

    // ---------------------------------------------------------------- exception-generating
    case Opcode::A32CallSupervisor: {
      std::uint32_t imm = 0;
      if (!W32(inst, 0, imm)) return false;
      // Noted, not stopped here. Both translators emit `BranchWritePC(pc+n)` before this op and end the
      // block with `CheckHalt{PopRSBHint}`, so the halt is *observed* at the terminal — which is where
      // Tier J's back end stops too (it checks the halt flag at that same terminal). Stopping mid-block
      // instead would leave the two tiers reporting different PCs for one SVC, and PC is the resume
      // address the syscall monitor hands back to.
      svc_pending_ = true;
      svc_ = imm;
      return true;
    }

    // ---------------------------------------------------------------- FPSCR and the check bit
    case Opcode::A32GetFpscr:
      Put(inst, V{state_.fpscr, 0});
      return true;
    case Opcode::A32SetFpscr: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      // Masked, not stored whole. FPSCR is three different things to the translator: the compare field
      // [31:28], the sticky/QC bits, and the *mode* bits (AHP/DN/FZ/RMode/Stride/Len) that change what an
      // instruction means and therefore take part in block identity. The AArch64 back end splits them
      // across `fpsr_nzcv`, `fpsr` and the top half of the upper location descriptor, keeping
      // 0xF0000000 | 0x0800009F | 0x07F70000 (`a32_jitstate.cpp:61-72`, whose `Fpscr()` ORs the three
      // back together), so a `vmsr` of a garbage word retains exactly that union and nothing else.
      // Tier I holds one architectural word, so it applies the same union once.
      state_.fpscr = v & 0xFFF7'009Fu;
      return true;
    }
    case Opcode::A32GetFpscrNZCV:
      // The compare field alone, at its [31:28] position — `vmrs APSR_nzcv, fpscr` reads this rather
      // than the whole word precisely so the mode and sticky bits cannot leak into CPSR. The back end
      // loads a separate `fpsr_nzcv` word; Tier I keeps one architectural word and masks.
      Put(inst, V{state_.fpscr & 0xf000'0000u, 0});
      return true;
    case Opcode::A32SetFpscrNZCV: {
      std::uint32_t v = 0;
      if (!W32(inst, 0, v)) return false;
      // `VCMP` writes only the compare field; the mode and sticky bits survive. On the back end this is
      // a store to a separate `fpsr_nzcv` word that `Fpscr()` ORs back in, which is the same rule.
      state_.fpscr = (state_.fpscr & 0x0fff'ffffu) | (v & 0xf000'0000u);
      return true;
    }
    case Opcode::A32SetCheckBit: {
      if (!HasArgs(inst, 1)) return false;
      V t{};
      if (!Read(inst->GetArg(0), t)) return false;
      check_bit = t.d() != 0;
      return true;
    }
    /**
     * `IsZero32` — and with it, Thumb-16 `CBZ`/`CBNZ`.
     *
     * It arrived as an incidental on the ASIMD lane (`docs/30` §Step 19), but it is worth naming for
     * what it actually is: `thumb16.cpp:903` lowers `CBZ`/`CBNZ` to `SetCheckBit(IsZero(rn))`, so until
     * this line Tier I could not run a compare-and-branch-on-zero at all, which is one of the most
     * common control-flow instructions in compiled ARM code. The `If`/`CheckBit` *terminal* was already
     * handled, so the missing half was only ever this predicate.
     *
     * `IsZero64` is not here because the A32 front end has exactly one `IsZero` call site and it passes
     * a `U32`; an ARMv7 guest cannot reach the 64-bit form.
     */
    case Opcode::IsZero32: {
      if (!HasArgs(inst, 1)) return false;
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      Put(inst, V{a == 0 ? 1u : 0u, 0});
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
    /**
     * `Mul64` is AArch64's `MUL Xd, Xn, Xm` (emit_arm64_data_processing.cpp:1047): the low 64 bits of
     * the 64x64 product, which is what `SMULL`/`UMULL` reach after widening their operands with
     * `SignExtendWordToLong`/`ZeroExtendWordToLong` (multiply.cpp:114, :182). No flag record: their
     * flag form would be `SETS`, which nothing in an ARMv7 guest emits from this path.
     */
    case Opcode::Mul64: {
      std::uint64_t a = 0, b = 0;
      if (!W64(inst, 0, a) || !W64(inst, 1, b)) return false;
      Put(inst, V{a * b, 0});
      return true;
    }
    /**
     * ARM's integer divide, which the AArch64 back end lowers to native `SDIV`/`UDIV`
     * (emit_arm64_data_processing.cpp:1076-1101) — so the two defined-result cases are ARM's, not C++'s:
     *
     *   - divisor 0 → 0, not a trap. `SignedDivide`/`UnsignedDivide` in the ARM ARM return 0 and
     *     AArch64's SDIV/UDIV implement that; C++'s `/` is UB there, so this branch has to be explicit.
     *   - `SDIV INT_MIN, -1` → `INT_MIN` (the truncation of `+2^31` to 32 bits), which is what Dynarmic
     *     asserts on its own A32 path (`tests/A32/test_arm_instructions.cpp:586-602`). C++ would trap
     *     or produce UB, so it too is spelled out before the operator runs.
     *
     * Every other case is `a / b` on the signed or unsigned interpretation, and C++'s truncation
     * toward zero matches ARM's.
     */
    case Opcode::SignedDiv32:
    case Opcode::UnsignedDiv32: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      const bool sdiv = op == Opcode::SignedDiv32;
      std::uint32_t r = 0;
      if (b == 0) {
        r = 0;  // ARM's defined result for a zero divisor, both forms.
      } else if (sdiv && a == 0x8000'0000u && b == 0xffff'ffffu) {
        // Saturates to INT_MIN — the truncation of +2^31 into 32 bits, which is `a` itself. Spelled out
        // because C++ would be evaluating undefined behaviour here, not merely a different number.
        r = a;
      } else {
        r = sdiv ? static_cast<std::uint32_t>(static_cast<std::int32_t>(a) /
                                              static_cast<std::int32_t>(b))
                 : a / b;
      }
      Put(inst, V{r, 0});
      return true;
    }
    // Bitwise ops need no flag record at all: their flag-form on AArch64 is `ANDS`/`ORS`/`EORS`, which
    // leaves N and Z from the result and clears C and V — exactly what the `TST v, v` fallback
    // `FlagsFromOp` already computes. A shifted *operand* carries a shifter C, but the translator
    // takes that from a separate `GetCarryFromOp` on the shift and recombines it with `SetCpsrNZC`.
    /**
     * `Not32`/`Not64`, which the front end emits as the scalar complement — the same operation the 128-bit
     * `VectorNot` below performs lane-wise, so a NEON routine and a scalar one land in different cases for
     * one concept. Tier I had `VectorNot` and not these, which is why bionic's own constructor stopped at
     * `Not32: no Tier I semantics yet` on the phone: a real libc reaches the scalar form in its startup
     * code, and the interpreter refused it there.
     *
     * No flag record, for the reason the neighbours state: their flag-form on AArch64 is `ANDS`/`ORS`/
     * `EORS`-shaped, which the `TST v, v` fallback in `FlagsFromOp` already computes.
     */
    /**
     * The barriers, executed as instructions that do nothing — which for this guest is what they are.
     *
     * `dmb`, `dsb` and `isb` order accesses *between* observers. A RetroBug guest is one thread on one host
     * thread: program order already provides every ordering these can ask for, there is no second observer
     * to publish to, and no guest store buffer to drain. Nothing is silently skipped — the instruction is
     * decoded, counted and continued, and Tier J emits a real fence for the same word (see its
     * `emit_arm64_a32.cpp`), so the two tiers differ in mechanism and not in what the guest observes.
     *
     * The *frequency* is why these are worth a comment at all: `dmb` appears 1067 times in the
     * 325,541-instruction bionic/libm/libc corpus this repo measures against — more than any other
     * unimplemented opcode — so refusing it is the difference between Tier I running a real system library
     * and stopping in the first hundred instructions of one.
     *
     * What would change this: a guest with more than one thread. Then these become real fences, and the
     * place to implement them is wherever the second thread's view of memory is introduced.
     */
    case Opcode::A32DataMemoryBarrier:
    case Opcode::A32DataSynchronizationBarrier:
    case Opcode::A32InstructionSynchronizationBarrier:
      return true;

    /**
     * `ByteReverseHalf` is U16→U16: the byte swap *within* each half, which the front end feeds to
     * `SignExtendHalfToWord` for `revsh` and to the register write for `rev16`. Tier I had `ByteReverseWord`
     * and not this, so every `rev16`/`revsh` in a guest refused while `rev` worked.
     */
    case Opcode::ByteReverseHalf: {
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      Put(inst, V{((a & 0x00ffu) << 8) | ((a & 0xff00u) >> 8), 0});
      return true;
    }

    case Opcode::Not32: {
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      Put(inst, V{~a, 0});
      return true;
    }
    case Opcode::Not64: {
      std::uint64_t a = 0;
      if (!W64(inst, 0, a)) return false;
      Put(inst, V{~a, 0});
      return true;
    }
    case Opcode::And32:
    case Opcode::Or32:
    case Opcode::Eor32: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{op == Opcode::And32 ? a & b : op == Opcode::Or32 ? a | b : a ^ b, 0});
      return true;
    }
    /// The same three at 64 bits, which an AArch32 guest reaches through the ASIMD structure paths that
    /// assemble a `d` register out of element-sized pieces. No flag record, for the reason given above:
    /// their flag-form is the `TST v, v` fallback, and `FlagsFromOp` already takes the width from the
    /// value's IR type rather than assuming 32.
    case Opcode::And64:
    case Opcode::Or64:
    case Opcode::Eor64: {
      std::uint64_t a = 0, b = 0;
      if (!W64(inst, 0, a) || !W64(inst, 1, b)) return false;
      const std::uint64_t r = op == Opcode::And64 ? a & b : op == Opcode::Or64 ? a | b : a ^ b;
      Put(inst, V{r, 0});
      return true;
    }
    /**
     * `AndNot` — `a & ~b`, which is ARM's BIC and AArch64's BICS. The operand order is the whole of the
     * semantics and it is not symmetric: `~a & b` is the same *shape* of expression and would pass any
     * test whose operands happen to be bitwise complements of each other. It is named here because the
     * first real guest library RetroBug tried to execute stopped on it — `docs/30` §Step 24 — which is
     * also why the ordering claim is taken from the back end's own `BICS(Rresult, Ra, Rb)` rather than
     * from the ARM ARM's prose about BIC.
     */
    case Opcode::AndNot32: {
      std::uint32_t a = 0, b = 0;
      if (!W32(inst, 0, a) || !W32(inst, 1, b)) return false;
      Put(inst, V{a & ~b, 0});
      return true;
    }
    case Opcode::AndNot64: {
      std::uint64_t a = 0, b = 0;
      if (!W64(inst, 0, a) || !W64(inst, 1, b)) return false;
      Put(inst, V{a & ~b, 0});
      return true;
    }

    /**
     * The 64-bit shift pair, which is *not* the 32-bit family above widened: `LogicalShiftLeft64` and
     * its partner take two operands rather than three (opcodes.inc:111-113 — no `U1` carry-in), so they
     * publish no shifter carry at all, and an amount of 64 or more yields zero rather than the
     * ARMv7-style "shift by 32 leaves the operand alone".
     *
     * These exist because the AArch32 guest reaches them without a 64-bit shift instruction: the ASIMD
     * structure paths insert and extract an element out of a 64-bit word with exactly these ops
     * (`asimd_load_store_structures.cpp:161` for the store side), so `vst1`/`vld1` need them. The first
     * demand was measured, not guessed — `docs/30` §Step 19's ASIMD probes refused on
     * `LogicalShiftLeft64` while Tier J ran them fine.
     *
     * `ArithmeticShiftRight64` and `RotateRight64` are deliberately absent: their back-end forms take a
     * register operand through an AArch64 *signed*-shift rule (`emit_arm64_data_processing.cpp`, the
     * `ASR Xd, Xt, Xm` branch) that has not been looked up here, and nothing in an AArch32 guest has
     * asked for them yet. Refusing names that instead of inventing semantics is the cheaper mistake.
     */
    case Opcode::LogicalShiftLeft64:
    case Opcode::LogicalShiftRight64: {
      if (!HasArgs(inst, 2)) return false;
      std::uint64_t a = 0, raw = 0;
      if (!W64(inst, 0, a) || !W64(inst, 1, raw)) return false;
      // Masked to 8 bits, then the whole result goes to zero at 64 or above — both halves of
      // `AND Xscratch0, Xshift, 0xff` followed by `CMP Xscratch0, 64` + `CSEL …, XZR, LT`.
      const unsigned n = static_cast<unsigned>(raw) & 0xffu;
      if (n >= 64) {
        Put(inst, V{0, 0});
        return true;
      }
      Put(inst, V{op == Opcode::LogicalShiftLeft64 ? (a << n) : (a >> n), 0});
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
    /**
     * The widening converts, from 8, 16 or 32 bits to a word or a long.
     *
     * The four `…ToLong` forms from 8 and 16 bits were the last thing standing between Tier I and
     * bionic's `memmove_a15`: Dynarmic's only A32 producer of them is the ASIMD structure load with
     * one-byte elements (`frontend/A32/translate/impl/asimd_load_store_structures.cpp:160`,
     * `ZeroExtendToLong(ReadMemory(ebytes * 8, …))` with `ebytes == 1`), which is what `vld1`/`vld4` of
     * `.8` decode to. They reached the `default:` branch below and refused as "no Tier I semantics yet".
     */
    case Opcode::ZeroExtendByteToWord:
    case Opcode::SignExtendByteToWord:
    case Opcode::ZeroExtendByteToLong:
    case Opcode::SignExtendByteToLong:
    case Opcode::ZeroExtendHalfToWord:
    case Opcode::SignExtendHalfToWord:
    case Opcode::ZeroExtendHalfToLong:
    case Opcode::SignExtendHalfToLong:
    case Opcode::ZeroExtendWordToLong:
    case Opcode::SignExtendWordToLong: {
      std::uint64_t a = 0;
      if (!W64(inst, 0, a)) return false;
      const bool byte = op == Opcode::ZeroExtendByteToWord || op == Opcode::SignExtendByteToWord ||
                        op == Opcode::ZeroExtendByteToLong || op == Opcode::SignExtendByteToLong;
      const bool half = op == Opcode::ZeroExtendHalfToWord || op == Opcode::SignExtendHalfToWord ||
                        op == Opcode::ZeroExtendHalfToLong || op == Opcode::SignExtendHalfToLong;
      const unsigned from = byte ? 8 : half ? 16 : 32;
      const bool to_long = op == Opcode::ZeroExtendByteToLong || op == Opcode::SignExtendByteToLong ||
                           op == Opcode::ZeroExtendHalfToLong || op == Opcode::SignExtendHalfToLong ||
                           op == Opcode::ZeroExtendWordToLong || op == Opcode::SignExtendWordToLong;
      const bool sgn = op == Opcode::SignExtendByteToWord || op == Opcode::SignExtendByteToLong ||
                       op == Opcode::SignExtendHalfToWord || op == Opcode::SignExtendHalfToLong ||
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
    /// AArch64 `REV Wd, Wn` and `CLZ Wd, Wn` (emit_arm64_data_processing.cpp:1367,1388). CLZ's 0→32 is
    /// ARM's defined result and is what `__builtin_clz` refuses to be, so the zero case is spelled out
    /// rather than left to the host.
    case Opcode::ByteReverseWord: {
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      Put(inst, V{__builtin_bswap32(a), 0});
      return true;
    }
    case Opcode::CountLeadingZeros32: {
      std::uint32_t a = 0;
      if (!W32(inst, 0, a)) return false;
      const std::uint32_t r = a == 0 ? 32u : static_cast<std::uint32_t>(__builtin_clz(a));
      Put(inst, V{r, 0});
      return true;
    }

    // ---------------------------------------------------------------- scalar VFP
    //
    // `docs/30` §Step 16 measured why this group exists: with none of it, Tier I stops at the first FP
    // instruction of any kind, which makes the interpreted path — the one an `execmem`-denied host has
    // to take, and C04's `kInterpreted` backend — unable to run libm or any `double` in a guest `.so`.
    // The expectations below are not invented: every one of these ops is exercised by a fixture probe
    // whose value silicon returned, in `tests/corpus/oracle-armv7.txt`.
    case Opcode::FPAdd32: case Opcode::FPSub32: case Opcode::FPMul32: case Opcode::FPDiv32:
    case Opcode::FPMax32: case Opcode::FPMin32: case Opcode::FPMaxNumeric32:
    case Opcode::FPMinNumeric32: {
      std::uint32_t raw_a = 0, raw_b = 0;
      if (!W32(inst, 0, raw_a) || !W32(inst, 1, raw_b)) return false;
      FpOps flags{state_.fpscr};
      const float a = BitsAs<float>(raw_a), b = BitsAs<float>(raw_b);
      float r{};
      switch (inst->GetOpcode()) {
        case Opcode::FPAdd32: r = a + b; break;
        case Opcode::FPSub32: r = a - b; break;
        case Opcode::FPMul32: r = a * b; break;
        case Opcode::FPDiv32: r = a / b; break;
        case Opcode::FPMaxNumeric32: case Opcode::FPMinNumeric32:
          // The "Numeric" forms are the ones that treat a NaN operand as the *other* one, which is
          // VMAXNM/VMINNM; the plain forms propagate it. Getting these two the same way round is a
          // wrong answer that only shows up on inputs nobody thinks to test.
          r = (std::isnan(a) && !std::isnan(b)) ? b
              : (std::isnan(b) && !std::isnan(a)) ? a
              : (inst->GetOpcode() == Opcode::FPMaxNumeric32 ? a > b ? a : b : a < b ? a : b);
          break;
        default:  // VMAX/VMIN: NaN in, NaN out
          r = (std::isnan(a) || std::isnan(b))
                  ? std::numeric_limits<float>::quiet_NaN()
                  : (inst->GetOpcode() == Opcode::FPMax32 ? (a > b ? a : b) : (a < b ? a : b));
          break;
      }
      Put(inst, V{AsBits<std::uint32_t>(r), 0});
      return true;
    }

    case Opcode::FPAdd64: case Opcode::FPSub64: case Opcode::FPMul64: case Opcode::FPDiv64:
    case Opcode::FPMax64: case Opcode::FPMin64: case Opcode::FPMaxNumeric64:
    case Opcode::FPMinNumeric64: {
      std::uint64_t raw_a = 0, raw_b = 0;
      if (!W64(inst, 0, raw_a) || !W64(inst, 1, raw_b)) return false;
      FpOps flags{state_.fpscr};
      const double a = BitsAs<double>(raw_a), b = BitsAs<double>(raw_b);
      double r{};
      switch (inst->GetOpcode()) {
        case Opcode::FPAdd64: r = a + b; break;
        case Opcode::FPSub64: r = a - b; break;
        case Opcode::FPMul64: r = a * b; break;
        case Opcode::FPDiv64: r = a / b; break;
        case Opcode::FPMaxNumeric64: case Opcode::FPMinNumeric64:
          r = (std::isnan(a) && !std::isnan(b)) ? b
              : (std::isnan(b) && !std::isnan(a)) ? a
              : (inst->GetOpcode() == Opcode::FPMaxNumeric64 ? a > b ? a : b : a < b ? a : b);
          break;
        default:
          r = (std::isnan(a) || std::isnan(b))
                  ? std::numeric_limits<double>::quiet_NaN()
                  : (inst->GetOpcode() == Opcode::FPMax64 ? (a > b ? a : b) : (a < b ? a : b));
          break;
      }
      Put(inst, V{AsBits<std::uint64_t>(r), 0});
      return true;
    }

    case Opcode::FPMulSub32: case Opcode::FPMulSub64: {
      // Fused `a - b*c`, the sibling of the row above and for the same reason: the back end emits a single
      // `FMSUB` (`emit_arm64_floating_point.cpp`, `FPMulSub32`), and this is where `vnmls.f32`/`.f64` land —
      // a pattern the coverage audit measured 45 times in this repo's own bionic/libm corpus.
      //
      // `std::fma(-b, c, a)` rather than `a - b*c` written out: negating a float is exact under IEEE-754,
      // so the fused call still rounds once and returns the correctly-rounded `a - b*c`. The two-step form
      // would round the product first and disagree in the last bit on exactly the inputs a physical engine
      // feeds it.
      const bool dbl = inst->GetOpcode() == Opcode::FPMulSub64;
      FpOps flags{state_.fpscr};
      if (dbl) {
        std::uint64_t a = 0, b = 0, c = 0;
        if (!W64(inst, 0, a) || !W64(inst, 1, b) || !W64(inst, 2, c)) return false;
        const double r = std::fma(-BitsAs<double>(b), BitsAs<double>(c), BitsAs<double>(a));
        Put(inst, V{AsBits<std::uint64_t>(r), 0});
      } else {
        std::uint32_t a = 0, b = 0, c = 0;
        if (!W32(inst, 0, a) || !W32(inst, 1, b) || !W32(inst, 2, c)) return false;
        const float r = std::fma(-BitsAs<float>(b), BitsAs<float>(c), BitsAs<float>(a));
        Put(inst, V{AsBits<std::uint32_t>(r), 0});
      }
      return true;
    }

    case Opcode::FPMulAdd32: case Opcode::FPMulAdd64: {
      // Fused: `a + b*c` with one rounding, because the back end emits a single `FMADD`. Two host
      // operations would be a different number, and this op is how `vmla.f64` lowers — which the
      // fixture's `fp_arith` reaches through `x*x`.
      const bool dbl = inst->GetOpcode() == Opcode::FPMulAdd64;
      FpOps flags{state_.fpscr};
      if (dbl) {
        std::uint64_t a = 0, b = 0, c = 0;
        if (!W64(inst, 0, a) || !W64(inst, 1, b) || !W64(inst, 2, c)) return false;
        const double r = std::fma(BitsAs<double>(b), BitsAs<double>(c), BitsAs<double>(a));
        Put(inst, V{AsBits<std::uint64_t>(r), 0});
      } else {
        std::uint32_t a = 0, b = 0, c = 0;
        if (!W32(inst, 0, a) || !W32(inst, 1, b) || !W32(inst, 2, c)) return false;
        const float r = std::fma(BitsAs<float>(b), BitsAs<float>(c), BitsAs<float>(a));
        Put(inst, V{AsBits<std::uint32_t>(r), 0});
      }
      return true;
    }

    case Opcode::FPSqrt32: case Opcode::FPSqrt64: case Opcode::FPNeg32: case Opcode::FPNeg64:
    case Opcode::FPAbs32: case Opcode::FPAbs64: {
      const bool wide = inst->GetOpcode() != Opcode::FPSqrt32 &&
                        inst->GetOpcode() != Opcode::FPNeg32 &&
                        inst->GetOpcode() != Opcode::FPAbs32;
      // Only the sqrt can raise anything — `FPNEG`/`FPABS` are sign edits on the bit pattern. The
      // scope is still opened for them, because clearing-then-merging nothing is the same observable
      // as not touching the sticky bits, and a branch per opcode here would only invite a mistake.
      FpOps flags{state_.fpscr};
      if (wide) {
        std::uint64_t raw = 0;
        if (!W64(inst, 0, raw)) return false;
        if (inst->GetOpcode() == Opcode::FPSqrt64) {
          Put(inst, V{AsBits<std::uint64_t>(std::sqrt(BitsAs<double>(raw))), 0});
        } else if (inst->GetOpcode() == Opcode::FPNeg64) {
          Put(inst, V{raw ^ (1ull << 63), 0});
        } else {
          Put(inst, V{raw & ~(1ull << 63), 0});
        }
      } else {
        std::uint32_t raw = 0;
        if (!W32(inst, 0, raw)) return false;
        if (inst->GetOpcode() == Opcode::FPSqrt32) {
          Put(inst, V{AsBits<std::uint32_t>(std::sqrt(BitsAs<float>(raw))), 0});
        } else if (inst->GetOpcode() == Opcode::FPNeg32) {
          Put(inst, V{raw ^ (1u << 31), 0});
        } else {
          Put(inst, V{raw & ~(1u << 31), 0});
        }
      }
      return true;
    }

    case Opcode::FPCompare32: case Opcode::FPCompare64: {
      /**
       * ARM's floating-point compare writes FPSCR[31:28] as NV : Z : C : GE:
       *
       *   less        NV           0x8
       *   equal       Z,   C       0x6
       *   greater     C            0x2
       *   unordered   C,   GE      0x3
       *
       * Both measured (`fp_cmp` in the fixture, which clears FPSCR so the field is this compare's and
       * not the process's) and matching Arm's published VFP comparison table — see `docs/30` §Step 17,
       * which also records that this file's first version used a table recalled from the manual rather
       * than looked up, was wrong on three of these four, and the differential said so.
       *
       * The pair worth memorising is the last two: a *less-than* sets NV alone, and an *unordered*
       * leaves NV and Z clear. So `!=` after a float compare is tested with `VS` — V, which
       * `vmrs APSR_nzcv, fpscr` takes from **GE** — which is how `fp_nan` reaches its hardware-measured
       * answer of 8. Anyone reading this table as an integer-flags shape gets that wrong.
       */
      bool unordered = false;
      std::uint8_t exc_on_qnan = 0;
      if (!ByteArg(inst, 2, exc_on_qnan)) return false;
      FpOps flags{state_.fpscr};
      std::uint64_t out = 0;
      if (inst->GetOpcode() == Opcode::FPCompare64) {
        std::uint64_t ra = 0, rb = 0;
        if (!W64(inst, 0, ra) || !W64(inst, 1, rb)) return false;
        const double a = BitsAs<double>(ra), b = BitsAs<double>(rb);
        unordered = !(a < b) && !(a > b) && !(a == b);
        const std::uint64_t nv = a < b, z = a == b, c = a > b || a == b || unordered,
                              ge = unordered;
        out = (nv << 31) | (z << 30) | (c << 29) | (ge << 28);
      } else {
        std::uint32_t ra = 0, rb = 0;
        if (!W32(inst, 0, ra) || !W32(inst, 1, rb)) return false;
        const float a = BitsAs<float>(ra), b = BitsAs<float>(rb);
        unordered = !(a < b) && !(a > b) && !(a == b);
        const std::uint64_t nv = a < b, z = a == b, c = a > b || a == b || unordered,
                              ge = unordered;
        out = (nv << 31) | (z << 30) | (c << 29) | (ge << 28);
      }
      Put(inst, V{out, 0});
      // `VCMPE` raises the invalid exception on a quiet NaN and `VCMP` does not — the only observable
      // difference between the two encodings, and the whole content of the operand. A compare is not a
      // host arithmetic operation here (C++'s `<` on a NaN raises nothing), so the flag is stated
      // rather than borrowed.
      if (unordered && exc_on_qnan != 0) flags.Raise(kIoc);
      return true;
    }

    // Conversions between integers and floats. `fbits` is the ARM fixed-point scale: the result is
    // value × 2^fbits on the way in and × 2^-fbits on the way out, with `fbits == 0` the ordinary
    // case (`vcvt.f64.s32`) and non-zero the `float64_to_fixed*` forms `vcmp`'s cousins use.
    case Opcode::FPFixedS32ToDouble: case Opcode::FPFixedS32ToSingle:
    case Opcode::FPFixedU32ToDouble: case Opcode::FPFixedU32ToSingle:
    case Opcode::FPFixedS64ToDouble: case Opcode::FPFixedS64ToSingle:
    case Opcode::FPFixedU64ToDouble: case Opcode::FPFixedU64ToSingle: {
      const bool src64 = inst->GetOpcode() == Opcode::FPFixedS64ToDouble ||
                         inst->GetOpcode() == Opcode::FPFixedS64ToSingle ||
                         inst->GetOpcode() == Opcode::FPFixedU64ToDouble ||
                         inst->GetOpcode() == Opcode::FPFixedU64ToSingle;
      const bool uns = inst->GetOpcode() == Opcode::FPFixedU32ToDouble ||
                       inst->GetOpcode() == Opcode::FPFixedU32ToSingle ||
                       inst->GetOpcode() == Opcode::FPFixedU64ToDouble ||
                       inst->GetOpcode() == Opcode::FPFixedU64ToSingle;
      const bool to_single = inst->GetOpcode() == Opcode::FPFixedS32ToSingle ||
                             inst->GetOpcode() == Opcode::FPFixedU32ToSingle ||
                             inst->GetOpcode() == Opcode::FPFixedS64ToSingle ||
                             inst->GetOpcode() == Opcode::FPFixedU64ToSingle;
      std::uint64_t raw = 0;
      std::uint8_t fbits = 0, rm = 0;
      if (src64) {
        if (!W64(inst, 0, raw)) return false;
      } else {
        std::uint32_t narrow = 0;
        if (!W32(inst, 0, narrow)) return false;
        raw = narrow;
      }
      if (!ByteArg(inst, 1, fbits) || !ByteArg(inst, 2, rm)) return false;
      FpOps flags{state_.fpscr};
      const double scaled = src64
          ? std::ldexp(uns ? static_cast<double>(static_cast<std::uint64_t>(raw))
                           : static_cast<double>(static_cast<std::int64_t>(raw)),
                       -static_cast<int>(fbits))
          : std::ldexp(uns ? static_cast<double>(static_cast<std::uint32_t>(raw))
                           : static_cast<double>(static_cast<std::int32_t>(raw)),
                       -static_cast<int>(fbits));
      bool inexact = false;
      if (to_single) {
        const float r = RoundToInt(static_cast<float>(scaled), rm, &inexact);
        if (inexact) flags.Raise(kIxc);
        Put(inst, V{AsBits<std::uint32_t>(r), 0});
      } else {
        const double r = RoundToInt(scaled, rm, &inexact);
        if (inexact) flags.Raise(kIxc);
        Put(inst, V{AsBits<std::uint64_t>(r), 0});
      }
      return true;
    }

    case Opcode::FPDoubleToFixedS32: case Opcode::FPDoubleToFixedU32:
    case Opcode::FPDoubleToFixedS64: case Opcode::FPDoubleToFixedU64:
    case Opcode::FPSingleToFixedS32: case Opcode::FPSingleToFixedU32:
    case Opcode::FPSingleToFixedS64: case Opcode::FPSingleToFixedU64: {
      const bool src_double = inst->GetOpcode() == Opcode::FPDoubleToFixedS32 ||
                              inst->GetOpcode() == Opcode::FPDoubleToFixedU32 ||
                              inst->GetOpcode() == Opcode::FPDoubleToFixedS64 ||
                              inst->GetOpcode() == Opcode::FPDoubleToFixedU64;
      const bool uns = inst->GetOpcode() == Opcode::FPDoubleToFixedU32 ||
                       inst->GetOpcode() == Opcode::FPDoubleToFixedU64 ||
                       inst->GetOpcode() == Opcode::FPSingleToFixedU32 ||
                       inst->GetOpcode() == Opcode::FPSingleToFixedU64;
      const bool to64 = inst->GetOpcode() == Opcode::FPDoubleToFixedS64 ||
                        inst->GetOpcode() == Opcode::FPDoubleToFixedU64 ||
                        inst->GetOpcode() == Opcode::FPSingleToFixedS64 ||
                        inst->GetOpcode() == Opcode::FPSingleToFixedU64;
      std::uint64_t raw = 0;
      std::uint8_t fbits = 0, rm = 0;
      if (src_double) {
        if (!W64(inst, 0, raw)) return false;
      } else {
        std::uint32_t narrow = 0;
        if (!W32(inst, 0, narrow)) return false;
        raw = narrow;
      }
      if (!ByteArg(inst, 1, fbits) || !ByteArg(inst, 2, rm)) return false;
      FpOps flags{state_.fpscr};
      const double in = src_double ? BitsAs<double>(raw)
                                   : static_cast<double>(BitsAs<float>(static_cast<std::uint32_t>(raw)));
      const double scaled = std::ldexp(in, static_cast<int>(fbits));
      bool inexact = false;
      const double rounded = RoundToInt(scaled, rm, &inexact);
      std::uint32_t raised = inexact ? kIxc : 0u;
      const std::uint64_t out = ToFixed(rounded, !uns, to64 ? 64u : 32u, &raised);
      flags.Raise(raised);
      Put(inst, V{to64 ? out : (out & 0xffffffffu), 0});
      return true;
    }

    case Opcode::FPSingleToDouble: case Opcode::FPDoubleToSingle: {
      std::uint8_t rm = 0;
      if (!ByteArg(inst, 1, rm)) return false;
      // Only round-to-nearest is honoured on the narrowing side: the host conversion does that one,
      // and picking a *neighbour* in a requested direction is a nextafter-sized piece of work nothing
      // in the corpus reaches (ARM's `vcvt.f32.f64` is emitted with the FPSCR at its reset RM by every
      // compiler measured here). Named rather than left implicit, because a silent single-direction
      // difference is exactly the class of bug this file has already been bitten by once.
      (void)rm;
      FpOps flags{state_.fpscr};
      if (inst->GetOpcode() == Opcode::FPSingleToDouble) {
        std::uint32_t raw = 0;
        if (!W32(inst, 0, raw)) return false;
        // Widening a float to a double is exact in IEEE-754, so the rounding mode cannot apply — which
        // is why this branch reads the immediate and does not use it.
        Put(inst, V{AsBits<std::uint64_t>(static_cast<double>(BitsAs<float>(raw))), 0});
      } else {
        std::uint64_t raw = 0;
        if (!W64(inst, 0, raw)) return false;
        const double src = BitsAs<double>(raw);
        if (std::isnan(src)) {
          // Narrowing a NaN keeps a NaN: the sign, the all-ones exponent, and the *leading* bits of the
          // payload — and the quiet bit forced, because ARM's VCVT never emits a signalling NaN from a
          // conversion. The payload is the one part of a NaN a guest can observe, so it is carried
          // rather than replaced with a canonical one.
          const std::uint32_t sign = static_cast<std::uint32_t>(raw >> 63) << 31;
          const std::uint32_t payload = static_cast<std::uint32_t>(raw >> 30) & 0x003f'ffffu;
          Put(inst, V{sign | 0x7f80'0000u | 0x0040'0000u | payload, 0});
          return true;
        }
        Put(inst, V{AsBits<std::uint32_t>(static_cast<float>(src)), 0});
      }
      return true;
    }

    // ---------------------------------------------------------------- ASIMD lanes
    //
    // The 16-byte view is the whole point: every integer lane operation in this group is the same loop
    // over lanes of 8/16/32/64 bits, and writing it once against bytes rather than four times against
    // `u64` pairs is what keeps "the D form" and "the Q form" from being two implementations where only
    // one lane width was ever finished. `VectorAdd32`, the single vector op this file had before
    // `docs/30` §Step 16 counted it, was already in that shape — which is the reason the shape is
    // reused rather than reinvented here.
    case Opcode::ZeroVector:
      Put(inst, V{0, 0});
      return true;
    case Opcode::VectorBroadcast8: case Opcode::VectorBroadcast16:
    case Opcode::VectorBroadcast32: case Opcode::VectorBroadcast64: {
      const unsigned bits = LaneBits(inst->GetOpcode());
      std::uint64_t v = 0;
      if (!W64(inst, 0, v)) return false;
      const std::uint64_t lane = bits == 64 ? v : v & ((1ull << bits) - 1);
      const auto fill = [&](std::uint64_t& word) {
        word = 0;
        for (unsigned off = 0; off < 64; off += bits) word |= lane << off;
      };
      std::uint64_t lo = 0, hi = 0;
      fill(lo);
      if (bits == 64) hi = lo; else fill(hi);
      Put(inst, V{lo, hi});
      return true;
    }

    case Opcode::VectorOr: case Opcode::VectorAnd: case Opcode::VectorEor:
    case Opcode::VectorAndNot: {
      V a{}, b{};
      if (!W128(inst, 0, a) || !W128(inst, 1, b)) return false;
      const auto op = inst->GetOpcode();
      const auto combine = [&](std::uint64_t x, std::uint64_t y) {
        if (op == Opcode::VectorOr) return x | y;
        if (op == Opcode::VectorAnd) return x & y;
        if (op == Opcode::VectorAndNot) return x & ~y;
        return x ^ y;
      };
      Put(inst, V{combine(a.lo, b.lo), combine(a.hi, b.hi)});
      return true;
    }
    case Opcode::VectorNot: {
      V a{};
      if (!W128(inst, 0, a)) return false;
      Put(inst, V{~a.lo, ~a.hi});
      return true;
    }

    case Opcode::VectorAdd8: case Opcode::VectorAdd16: case Opcode::VectorAdd32:
    case Opcode::VectorAdd64: case Opcode::VectorSub8: case Opcode::VectorSub16:
    case Opcode::VectorSub32: case Opcode::VectorSub64: case Opcode::VectorMultiply8:
    case Opcode::VectorMultiply16: case Opcode::VectorMultiply32: case Opcode::VectorMultiply64:
    case Opcode::VectorAbs8: case Opcode::VectorAbs16: case Opcode::VectorAbs32:
    case Opcode::VectorAbs64: {
      const unsigned bits = LaneBits(inst->GetOpcode());
      if (bits == 0) return Fail(inst, "unrecognised lane width in the vector arithmetic group");
      const unsigned bytes = bits / 8;
      V a{}, b{};
      if (!W128(inst, 0, a)) return false;
      // One operand for `abs`, two for the rest. Asked of the instruction rather than derived from the
      // opcode's name, because `NumArgs()` is what the IR actually carries.
      const bool abs_only = inst->GetOpcode() == Opcode::VectorAbs8 ||
                            inst->GetOpcode() == Opcode::VectorAbs16 ||
                            inst->GetOpcode() == Opcode::VectorAbs32 ||
                            inst->GetOpcode() == Opcode::VectorAbs64;
      if (!abs_only && !W128(inst, 1, b)) return false;
      const bool sub = inst->GetOpcode() == Opcode::VectorSub8 ||
                       inst->GetOpcode() == Opcode::VectorSub16 ||
                       inst->GetOpcode() == Opcode::VectorSub32 ||
                       inst->GetOpcode() == Opcode::VectorSub64;
      const auto* pa = reinterpret_cast<const std::uint8_t*>(&a);
      const auto* pb = reinterpret_cast<const std::uint8_t*>(&b);
      V out{};
      auto* pd = reinterpret_cast<std::uint8_t*>(&out);
      for (unsigned i = 0; i < 16; i += bytes) {
        std::uint64_t x = 0, y = 0;
        for (unsigned k = 0; k < bytes; ++k) {
          x |= static_cast<std::uint64_t>(pa[i + k]) << (8 * k);
          if (!abs_only) y |= static_cast<std::uint64_t>(pb[i + k]) << (8 * k);
        }
        std::uint64_t r = 0;
        if (abs_only) {
          // Signed lanes only, and the most-negative lane stays negative through `abs` — ARM's
          // documented behaviour for integer VABS, not an oversight here.
          const std::int64_t sv = SignedAt(x, bits);
          r = static_cast<std::uint64_t>(sv < 0 ? -sv : sv);
        } else if (sub) {
          r = x - y;
        } else if (inst->GetOpcode() == Opcode::VectorMultiply8 ||
                   inst->GetOpcode() == Opcode::VectorMultiply16 ||
                   inst->GetOpcode() == Opcode::VectorMultiply32 ||
                   inst->GetOpcode() == Opcode::VectorMultiply64) {
          r = x * y;
        } else {
          r = x + y;
        }
        // No mask here, and that is deliberate: only the low `bytes` of `r` are stored below, so the
        // lane-width wraparound falls out of the store. The widening multiply forms are distinct
        // opcodes with distinct names (`VectorMultiplySignedWiden*`), and a lane that silently took
        // the narrow path for one of those would be a plausible wrong answer rather than a fault —
        // which is why they are not in this case label at all.
        for (unsigned k = 0; k < bytes; ++k) pd[i + k] = static_cast<std::uint8_t>(r >> (8 * k));
      }
      Put(inst, out);
      return true;
    }

    case Opcode::VectorGetElement8: case Opcode::VectorGetElement16:
    case Opcode::VectorGetElement32: case Opcode::VectorGetElement64: {
      const unsigned bits = LaneBits(inst->GetOpcode());
      V a{};
      std::uint8_t index = 0;
      if (!W128(inst, 0, a) || !ByteArg(inst, 1, index)) return false;
      if (index >= LaneCount(bits)) {
        return Fail(inst, "vector lane index " + std::to_string(index) + " is out of range for a " +
                              std::to_string(bits) + "-bit lane in a 128-bit register");
      }
      std::uint64_t x = 0;
      const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&a);
      for (unsigned k = 0; k < bits / 8; ++k) {
        x |= static_cast<std::uint64_t>(p[index * (bits / 8) + k]) << (8 * k);
      }
      Put(inst, V{x, 0});
      return true;
    }
    case Opcode::VectorSetElement8: case Opcode::VectorSetElement16:
    case Opcode::VectorSetElement32: case Opcode::VectorSetElement64: {
      const unsigned bits = LaneBits(inst->GetOpcode());
      V a{};
      std::uint8_t index = 0;
      std::uint64_t value = 0;
      if (!W128(inst, 0, a) || !ByteArg(inst, 1, index)) return false;
      if (index >= LaneCount(bits)) {
        return Fail(inst, "vector lane index " + std::to_string(index) + " is out of range for a " +
                              std::to_string(bits) + "-bit lane in a 128-bit register");
      }
      // The value argument is the lane's own width, so it has to be read at that width; reading it
      // as a U64 on a `VectorSetElement8` would take whatever the register above the byte held.
      if (bits == 64) {
        if (!W64(inst, 2, value)) return false;
      } else {
        V raw{};
        if (!Read(inst->GetArg(2), raw)) return false;
        value = raw.lo & ((1ull << bits) - 1);
      }
      std::uint8_t* p = reinterpret_cast<std::uint8_t*>(&a);
      for (unsigned k = 0; k < bits / 8; ++k) {
        p[index * (bits / 8) + k] = static_cast<std::uint8_t>(value >> (8 * k));
      }
      Put(inst, a);
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
    if constexpr (std::is_same_v<T, Term::LinkBlock> || std::is_same_v<T, Term::LinkBlockFast>) {
      successor = LocationDescriptor{t.next};
      return true;
    } else if constexpr (std::is_same_v<T, Term::Interpret>) {
      /**
       * `Term::Interpret` is not a control-flow edge: it is the A32 front end saying "I declined to
       * translate the instruction at `next`", and the only things that produce it are the six
       * banked-register and privileged forms — `docs/32` §7 enumerates them against `decoder/arm.inc`.
       * Following it, which is what this branch used to do by being folded in with `LinkBlock`, cannot
       * make progress: the block never *executed* that instruction, so `next` names the same PC the
       * block stopped at, re-dispatch translates it the same way, and the loop is endless with no step
       * counted, no reason returned and no PC to report. A hang with no diagnostics is strictly worse
       * than the refusal below.
       *
       * Refusing by name is also the only honest option available, because actually interpreting a
       * banked `LDM` needs a banked register file that neither tier has — dynarmic's own arm64 guest
       * state omits it too (`a32_jitstate.h:18-31`), which is why the front end declines in the first
       * place. Supplying one is the M3-scale question, not a branch here.
       */
      stop.reason = StopReason::Unimplemented;
      // `t.next` is the generic IR descriptor; the program counter is a property of the A32 one, so it
      // is converted the same way dynarmic's own x64 back end converts it before calling its fallback.
      stop.detail = "the A32 front end declined to translate the instruction at 0x" +
                    Hex(LocationDescriptor{t.next}.PC()) +
                    " (banked-register or privileged load/store)";
      return false;
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
      if (svc_pending_) {
        // The halt is observed here, at the terminal, which is where Dynarmic's own back ends test the
        // halt flag — so both tiers stop at the same instruction with the same PC already advanced past
        // the SVC by the `BranchWritePC` the translator emitted before it.
        stop.reason = StopReason::Syscall;
        stop.detail = SvcDetail(svc_);
        stop.svc_number = svc_;
        return false;
      }
      return Take(t.else_, successor);  // a halt request stops the executor, not the guest
    } else if constexpr (std::is_same_v<T, Term::ReturnToDispatch>) {
      // Not a return, and treating it as one was task #26. The only things that produce this terminal are
      // the ALU-writes-PC forms — `arm_ADD_reg` with `Rd == PC`, which is how a compiler's switch or
      // position-independent call reaches a jump table (`add pc, r1, r0` after `ldr r0, [r1, r0, lsl #2]`)
      // — and in ARM state at v7 `ALUWritePC` lowers to `BXWritePC`, so the `A32BXWritePC` above has
      // already moved `location_` to the target by the time this branch runs. Continuing is what the
      // AArch64 back end does with the same IR: it branches to its dispatcher, which re-tests the halt
      // flag and otherwise translates and jumps to the new PC (`a32_address_space.cpp:305-334`), and the
      // loop below re-checks the return sentinel and the host-entry table on the way — so this is also
      // the branch where a computed jump into a host entry stops correctly instead of silently returning.
      //
      // What stopping with `Returned` did, measured on a device before this line changed: guest libm's
      // `sin` handed its caller `r0:r1 = 0x0001f3ac00000058` where hardware and Tier J both said
      // `0xbfe03425b78c4db8` — the reduction table's address (`0x2000` load bias + `0x1d3ac`) and its
      // first entry, in the two registers a double result is read from. Tier I was not computing sin
      // wrongly; it was never computing sin.
      successor = location_;
      return true;
    } else {
      stop.reason = StopReason::Internal;
      stop.detail = "a block ended in a terminal this interpreter does not leave through";
      return false;
    }
  });
}

RunResult InterpExecutor::Run(MachineState& state, unsigned max_instructions) {
  RunResult result{};
  // A Run begins with no reservation in either tier, so a differential disagreement about an LL/SC loop
  // cannot be a leftover from the previous Run. Tier J's `SyncIn` does the same on its way in.
  excl_.Clear();
  // CPSR's T bit and `mode` are one fact; a caller that set only the latter (which is what every entry
  // point here does) must not see a state back that contradicts what it handed over. Tier J's SyncIn does
  // the same on its way in.
  state.cpsr = CpsrWithMode(state.cpsr, state.mode);
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
        ++steps;
        result.reason = StopReason::Budget;
        continue;
      }
    }

    BlockRunner runner{memory_, state, location, excl_};
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
    result.reason = StopReason::Budget;
  }
  // The IT mirror is derived from CPSR here rather than from each block's location descriptor, in both
  // tiers, so that the two-tier comparison measures guest behaviour instead of two representations of
  // the same field. Translation still takes IT from the descriptor, which is where Dynarmic keeps it.
  state.cpsr = CpsrWithMode(state.cpsr, state.mode);
  state.itstate = ItStateFromCpsr(state.cpsr);
  result.instructions_executed = steps;
  return result;
}

void InterpExecutor::Invalidate(GuestAddr from, GuestAddr to) {
  (void)from;
  (void)to;
}

}  // namespace rb::exec
