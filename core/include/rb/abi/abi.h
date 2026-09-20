/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C08 `rb::abi` — the AAPCS32 call bridge, host→guest and guest→host.
 *
 * This is where the ABI-correctness review pass concentrates (docs/20 §C08), so the rules are stated
 * as code comments next to the code that applies them, and the ones that are easy to get wrong are
 * called out with the failure they prevent rather than with a citation.
 *
 * Two layers, deliberately separated:
 *
 *   - **Marshalling** (`PlaceArgs`, `ReturnValue`) is pure: registers in, registers out, no executor
 *     and no dispatch. It is the part an ABI review wants to read, and it can be tested without a
 *     guest program at all.
 *   - **`AbiContext`** owns the host-entry table and the run/resume loop, because a guest `blx` into a
 *     host function is not expressible as one function call — the executor has to stop, hand control
 *     here, and resume. It is per-instance for the same reason everything else in the engine is: no
 *     host call, binding or stack is ever shared between two guest instances.
 *
 * **The ABI is softfp, and that means core registers — not VFP ones.** This paragraph said the
 * opposite until `docs/30` §Step 24, and it was confidently wrong in the way that matters: it claimed
 * `-mfloat-abi=softfp` "passes scalars in VFP registers and merely *computes* them in software", which
 * is a description of the *hard*-float variant. Android's `armeabi-v7a` passes and returns
 * floating-point values in **r0-r3 / r0:r1**, and only the arithmetic uses `d[]`. Three independent
 * checks settled it, and they are written down because the wrong version had already been believed
 * once: the tablet's own `/system/lib/libm.so` shows `sqrt` as
 * `vmov d16, r0, r1; vsqrt.f64; vmov r0, r1, d16`; the guest probe compiled for this project ends
 * `vmov r0, r1, d18`; and clang **refuses** `-mfloat-abi=hard` outright for
 * `armv7-none-linux-android26` ("unsupported option"), so there is no hard-float variant to be
 * confused about. `d[]` is still needed for the arithmetic, and `ReturnValue` reads it for nothing.
 *
 * The practical consequence, stated so the next reader does not re-derive it: a `jdouble` argument
 * occupies a core register *pair* and a `jdouble` return arrives in `r0:r1`. `PlaceArgs` still places
 * `F64` arguments in VFP and is wrong for it — tracked as `docs/20` §9's ABI item, not silently.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb {
namespace syscall {
class Monitor;  ///< forward-declared at rb::syscall scope so this header stays cheap to include
}
}  // namespace rb

namespace rb::abi {

using exec::GuestAddr;
using exec::MachineState;

/**
 * One argument or return value.
 *
 * Held as bits plus a kind, because AAPCS assignment depends on the *type class* and not on the value:
 * an 8-byte integer needs an even/odd core-register **pair** (and so can consume an odd slot for
 * nothing), while a float needs no core register at all. Deciding that later from the bits alone is
 * not possible — `0x0000000100000000` and the double 2.0 are the same 64 bits.
 */
struct Value32 {
  enum class Kind { S32, U32, Ptr, I64, F32, F64 };

  Kind kind = Kind::U32;
  std::uint64_t bits = 0;

  static Value32 S32(std::int32_t v) { return {Kind::S32, static_cast<std::uint32_t>(v)}; }
  static Value32 U32(std::uint32_t v) { return {Kind::U32, v}; }
  static Value32 Ptr(GuestAddr v) { return {Kind::Ptr, v}; }
  static Value32 I64(std::int64_t v) {
    return {Kind::I64, static_cast<std::uint64_t>(v)};
  }
  static Value32 F32(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return {Kind::F32, bits};
  }
  static Value32 F64(double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    return {Kind::F64, bits};
  }

  [[nodiscard]] std::uint32_t w() const { return static_cast<std::uint32_t>(bits); }
  [[nodiscard]] std::int32_t i() const { return static_cast<std::int32_t>(bits); }
  [[nodiscard]] std::uint64_t d() const { return bits; }
  [[nodiscard]] float f() const {
    const std::uint32_t b = w();
    float out = 0;
    std::memcpy(&out, &b, 4);
    return out;
  }
  [[nodiscard]] double g() const {
    double out = 0;
    std::memcpy(&out, &bits, 8);
    return out;
  }

  bool operator==(const Value32&) const = default;
};

using Args32 = std::vector<Value32>;

/// A return value: `r0`, or the `r0`/`r1` pair for a 64-bit result, plus `d0` for a VFP one. Reading
/// the wrong half of a `jdouble` is a silent 0.0, so which register a result came from is recorded
/// rather than guessed from the shorty at every call site.
struct Ret32 {
  Value32::Kind kind = Value32::Kind::U32;
  std::uint64_t bits = 0;
  GuestAddr pc_after_return = 0;

  [[nodiscard]] std::int32_t i() const { return static_cast<std::int32_t>(bits); }
  [[nodiscard]] std::uint32_t w() const { return static_cast<std::uint32_t>(bits); }
  [[nodiscard]] std::int64_t j() const { return static_cast<std::int64_t>(bits); }
  [[nodiscard]] double g() const {
    double out = 0;
    std::memcpy(&out, &bits, 8);
    return out;
  }
};

/**
 * ART's shorty descriptor, used here as the return-type class so that C09 has one encoding rather
 * than two (docs/20 §C08). Only the first character is consulted by the bridge — it names the return
 * type — but the whole string is carried because a mismatch between the declared and called shape is
 * the kind of bug worth naming in a diagnostic.
 */
struct Shorty {
  std::string desc;

  /// `'V'` void, `'Z'` boolean, `'B'` byte, `'S'` short, `'I'` int, `'J'` long, `'F'` float,
  /// `'D'` double, `'L'` object/array. Anything else is a caller error, reported as one.
  [[nodiscard]] static Shorty For(const char* d) { return Shorty{d == nullptr ? "" : d}; }
  [[nodiscard]] char returns() const { return desc.empty() ? 'V' : desc[0]; }
};

/// Where the argument/return registers are, in one place, so a review can check the numbers against
/// AAPCS rather than against another call site.
struct RegLayout {
  static constexpr int kFirstCoreArg = 0;   ///< r0
  static constexpr int kLastCoreArg = 3;    ///< r3
  static constexpr int kCoreArgCount = 4;   ///< a1-a4
  static constexpr int kFirstDReg = 0;      ///< d0
  static constexpr int kDRegCount = 8;      ///< d0-d7, which is where s0-s15 live too
  static constexpr int kLr = 14;
  static constexpr int kSp = 13;
};

/**
 * Places `args` into `state` per AAPCS32 with the Android VFP variant, sets PC to `fn` and LR to
 * `return_address`, and writes the spilled arguments into the guest stack.
 *
 * The rules, and the failure each one prevents:
 *
 *   - Integers and pointers fill r0..r3 in order. Overflow goes to the stack, in order, at ascending
 *     addresses (AAPCS passes stack arguments left-to-right *in memory*, which is where a
 *     little-endian "reverse the args" reflex produces a call that works for one argument and is wrong
 *     for two).
 *   - An 8-byte **integer** needs a consecutive even/odd core-register pair: if the next free register
 *     is odd-numbered, it is skipped and consumed by nothing. Passing a `jlong` in r1/r2 is the classic
 *     failure this file is named for, and it is invisible until a call has an odd number of preceding
 *     4-byte arguments.
 *   - A **float or double** consumes no core register at all: it goes to s0.. / d0.., allocated from
 *     the VFP bank independently of the core bank. Filling r0 with the low half of a `jdouble` makes
 *     integer-only tests pass and every floating call wrong.
 *   - SP is 8-byte aligned on entry to the callee (and 16-byte before a `bl`), so the frame this
 *     builds is rounded to 16 bytes with room for the spilled arguments below it.
 *   - r9/`tp` are left exactly as the caller found them: on Android `tp` is `TPIDRURW` and bionic's
 *     `__get_tls()` reads it, so "helpfully" zeroing it breaks the guest's own errno access.
 *
 * Returns false, with `err`, when the arguments cannot fit the frame it was given — a stack of the
 * wrong size is a configuration error, not something to absorb silently.
 */
bool PlaceArgs(mem::GuestMemory& memory, MachineState& state, GuestAddr fn, const Args32& args,
               GuestAddr return_address, unsigned stack_bytes, std::string* err);

/// Reads the result out of `state` according to the shorty's return type, after the callee has
/// returned. `V` yields nothing and is not an error.
Ret32 ReturnValue(const MachineState& state, const Shorty&);

/// A host function the guest can call, reached at `AbiContext::SlotFor(slot)`. It sees the live
/// `MachineState` (so it reads its AAPCS arguments from r0..r3 / d0.. and writes its result to r0), and
/// returning false is a refusal that stops execution with `Fault` naming the slot. It is *not* a zero
/// result: a JNI function that quietly returns NULL is far harder to diagnose than one that says it is
/// missing, and an app that survives it behaves differently still.
using HostFn = std::function<bool(MachineState&, std::string* err)>;

/**
 * One guest instance's call bridge: the host-entry table, the run/resume loop, and the stack it hands
 * callees.
 *
 * It *is* the `exec::HostDispatch` the executor consults, so a guest `blx` to any address this object
 * published stops with `StopReason::HostCall` instead of trying to execute memory that does not exist.
 */
class AbiContext : public exec::HostDispatch {
 public:
  /// The largest host function index `SlotFor` can name. JNI's own table is 229 entries; the rest is
  /// headroom for host-provided symbols, and `IsHostEntry` refuses anything past it rather than
  /// aliasing one slot onto another.
  static constexpr std::uint32_t kMaxSlots = 512;
  /// How much guest stack one call gets: its spilled arguments plus the 16-byte intra-procedure area.
  /// A nesting level descends by this much, so the mapped stack bounds how deep a guest may call
  /// back into the host — which is a limit to state, not to discover at runtime.
  static constexpr unsigned kCallFrameBytes = 8u << 10;
  /// How many syscalls one `CallGuest` may service before the loop is treated as stuck. A number rather
  /// than an unbounded loop because a guest spinning on a refused call would otherwise spin the host
  /// forever inside one JNI method, and the failure that produces is a hung UI, not an error message.
  /// Real apps make thousands of syscalls per *launch*; per call, this is generous.
  static constexpr unsigned kMaxSyscallsPerCall = 100'000u;

  /// No executor at construction, deliberately: the bridge *is* the dispatch table the executor has to
  /// consult, so the two reference each other. Taking the executor later keeps that a one-way
  /// dependency at any instant rather than requiring either object to be born half-built.
  AbiContext(mem::GuestMemory& memory, GuestAddr stack_top, GuestAddr return_address);

  void set_executor(exec::Executor& executor) { executor_ = &executor; }
  [[nodiscard]] exec::Executor& executor() const { return *executor_; }

  /// The monitor an SVC reaches. Optional in the type and not optional in a real launch: without one, a
  /// guest syscall stops the call with a `Fault` that says so, because silently continuing past an SVC
  /// the host never answered is the same silent-ignore mistake in a place where the guest is now holding
  /// a return value that was never produced.
  void set_monitor(syscall::Monitor* monitor) { monitor_ = monitor; }
  [[nodiscard]] syscall::Monitor* monitor() const { return monitor_; }

  /// Set when a call ended in `StopReason::Exited`; the status the guest passed to `exit_group`.
  [[nodiscard]] std::int32_t exit_status() const { return exit_status_; }

  /// Makes host function `slot` callable by the guest. Re-binding replaces, which is what lets a
  /// profile override a default rather than needing a second table.
  void Bind(std::uint32_t slot, HostFn fn);
  [[nodiscard]] bool IsBound(std::uint32_t slot) const { return bound_.count(slot) != 0; }

  /// The guest address a table entry must hold for host function `slot`. Slots are laid out
  /// contiguously from `slot_base`, which is what lets a JNI table be filled by arithmetic — and why
  /// no page is mapped for them.
  GuestAddr SlotFor(std::uint32_t slot) const { return slot_base_ + slot * 4u; }
  static constexpr std::uint32_t kInvalidSlot = 0xffff'ffffu;

  bool IsHostEntry(GuestAddr pc, std::uint32_t* slot) const override;

  /// Marshals, runs until the callee returns, servicing host calls on the way, and returns the result.
  /// `max_steps` bounds the whole call, host calls included, so a guest loop that never returns cannot
  /// take the host with it.
  Ret32 CallGuest(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned max_steps,
                  exec::RunResult* outcome);

  /// The JNI-shaped entry point C09 will use: a shorty plus the values already converted. Kept next to
  /// `CallGuest` so there is one marshalling path, not one per caller.
  Ret32 CallGuestJ(GuestAddr fn, const Shorty& shorty, const std::vector<Value32>& args,
                   unsigned max_steps, exec::RunResult* outcome) {
    return CallGuest(fn, Args32{args}, shorty, max_steps, outcome);
  }

  MachineState& state() { return state_; }
  mem::GuestMemory& memory() { return memory_; }
  GuestAddr return_address() const { return return_address_; }
  unsigned stack_bytes() const { return stack_bytes_; }

 private:
  mem::GuestMemory& memory_;
  exec::Executor* executor_ = nullptr;
  syscall::Monitor* monitor_ = nullptr;
  std::int32_t exit_status_ = 0;
  std::unordered_map<std::uint32_t, HostFn> bound_;
  MachineState state_{};
  GuestAddr slot_base_;
  GuestAddr return_address_;
  unsigned stack_bytes_;
};

}  // namespace rb::abi
