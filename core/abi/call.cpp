/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C08 `rb::abi` — AAPCS32 argument placement, return extraction, and the run/resume loop around host
 * calls *and* guest syscalls. See rb/abi/abi.h for the layering and for why each rule below is here
 * rather than assumed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/abi/abi.h"

#include <utility>

#include "rb/syscall/monitor.h"

namespace rb::abi {

namespace {

/// The 16 bytes AAPCS reserves *below* SP for a callee to spill into across a `bl` without adjusting
/// SP (the intra-procedure call stack). Not counting it is the bug where a function works until it
/// calls another one.
constexpr std::uint32_t kIpswr = 16;

/**
 * How many core registers an argument occupies.
 *
 * Under `-mfloat-abi=softfp` this is the *whole* of the placement rule, which is what makes the function
 * one switch rather than the two banks it used to be: the soft-float procedure call standard passes
 * every argument in r0-r3 and the stack, so a `jfloat` is placed exactly as an `int32_t` is and a
 * `jdouble` exactly as a `jlong` is. The VFP registers are what the *arithmetic* uses.
 *
 * The one exception, measured rather than reasoned about, is a `static` function whose address never
 * escapes its translation unit: clang promotes those to the VFP register ABI even under `softfp`
 * (`sum8` in `tests/corpus/fixture_jni.c` is called with `d0`-`d7`, while `x8` in a scratch file built
 * with the identical flags reads `r0:r1`, `r2:r3` and then the stack). Nothing here changes for that,
 * because a call the guest makes to itself never comes through this file — and it is worth saying so
 * plainly, because "softfp means core registers, always" is a claim that is true of the *boundary* and
 * false *inside* a library, and a reader who takes it as a universal law will mis-predict a disassembly.
 */
[[nodiscard]] unsigned CoreWords(const Value32::Kind kind) {
  switch (kind) {
    case Value32::Kind::S32:
    case Value32::Kind::U32:
    case Value32::Kind::Ptr:
    case Value32::Kind::F32: return 1;
    case Value32::Kind::I64:
    case Value32::Kind::F64: return 2;
  }
  return 1;
}

}  // namespace

bool PlaceArgs(mem::GuestMemory& memory, MachineState& state, GuestAddr fn, const Args32& args,
               GuestAddr return_address, unsigned stack_bytes, std::string* err) {
  /**
   * The instruction set comes from bit 0 of the target, exactly as `blx` reads it.
   *
   * `AbiContext` used to fix the mode to Thumb at construction, which was invisible for as long as the
   * only things called through here were Thumb-compiled JNI methods — and wrong for everything else:
   * bionic's libm is mostly A32 (304 ARM against 60 Thumb by `readelf`), so no ARM-state guest function
   * could be entered at all. It showed up as `undefined instruction encoding` on Tier J and a decoder
   * exception on Tier I, at addresses that were perfectly valid ARM.
   *
   * Callers pass a *tagged* address (`CodeAddress::tagged()`); `state.pc` is then masked below, because
   * the tag is a property of the pointer rather than of the code.
   */
  state.mode = (fn & 1u) != 0 ? isa::Mode::Thumb : isa::Mode::Arm;
  // Compute the frame first, so a call whose arguments do not fit fails *before* any register or byte
  // has been mutated: a half-applied argument list is a call that looks like it happened.
  unsigned core = 0;   ///< next free core register, 0..4
  unsigned spill = 0;  ///< bytes of stack argument so far
  for (const Value32& a : args) {
    const unsigned need = CoreWords(a.kind);
    if (need == 2) {
      // An 8-byte argument needs an even/odd core-register *pair*: if the next free register is odd it
      // is consumed by nothing, and skipping that step is the classic `jlong` bug — with `softfp` it is
      // equally the `jdouble` bug, because the two are placed by the same rule.
      if ((core & 1u) != 0) ++core;
      if (core + 2 <= 4) {
        core += 2;
        continue;
      }
      core = 4;
    } else if (core < 4) {
      ++core;
      continue;
    }
    if (need == 2) {
      spill = (spill + 7u) & ~7u;  // stack arguments keep their own 8-byte alignment
      spill += 8;
    } else {
      spill += 4;
    }
  }

  const unsigned needed = kIpswr + spill;
  if (stack_bytes < needed + kIpswr) {
    if (err != nullptr) {
      *err = "call needs " + std::to_string(needed + kIpswr) + " bytes of guest stack (" +
             std::to_string(spill) + " argument block + " + std::to_string(2 * kIpswr) +
             " for the intra-procedure region and its alignment), the frame has " +
             std::to_string(stack_bytes);
    }
    return false;
  }

  const std::uint32_t caller_sp = state.r[RegLayout::kSp];
  if (caller_sp < needed + kIpswr) {
    if (err != nullptr) *err = "guest stack would run off its bottom at 0x" + std::to_string(caller_sp);
    return false;
  }
  /**
   * 16-byte aligned, which is what "8-byte aligned on entry, 16-byte before a bl" both reduce to: the
   * value SP holds at the `bl` is the value the callee sees on entry — and, because the argument block
   * starts *at* that value, it is also the address of the first spilled argument.
   *
   * That last clause is the correction. `PlaceArgs` used to lay the block out at `frame + kIpswr`, on
   * the reading that the intra-procedure region sits between SP and the arguments. It does not: it is
   * the 16 bytes *below* the callee's SP, which is why a callee can spill into it across its own `bl`
   * without moving SP. A real softfp callee proves the difference — `Java_..._intThenDouble` in
   * `tests/guest/mathprobe.c` reads its double with `vldr d17, [sp]`, at the bottom of its own frame and
   * not 16 bytes above it — and until this was fixed RetroBug wrote every spilled argument sixteen bytes
   * away from where the guest looked, so each one read as whatever the page happened to hold.
   *
   * The reason no test saw it for as long as it did is worth recording separately, because it is a
   * property of the *tests* and not of the bug: the one reader of the spill area in the suite was
   * `abi_call_test.cpp`'s `Spill()` helper, which added the same 16. An assertion that mirrors the code
   * it is checking cannot fail, and this one did not, across six-int rows that passed for many revisions.
   */
  const GuestAddr frame = (caller_sp - needed) & ~0xFu;
  state.r[RegLayout::kSp] = frame;

  // Registers and stack in one pass now that the frame is known to exist. Spilled arguments go at
  // *ascending* addresses from the bottom of the frame, which for little-endian AArch32 means the
  // first spilled argument is at the lowest address — reversing this is invisible for one argument and
  // wrong for two.
  core = 0;
  unsigned off = 0;  ///< bytes into the argument block, which starts at the callee's SP
  for (const Value32& a : args) {
    const unsigned need = CoreWords(a.kind);
    if (need == 2) {
      if ((core & 1u) != 0) ++core;
      if (core + 2 <= 4) {
        state.r[core] = a.w();
        state.r[core + 1] = static_cast<std::uint32_t>(a.bits >> 32);
        core += 2;
        continue;
      }
      core = 4;
    } else if (core < 4) {
      state.r[core++] = a.w();
      continue;
    }

    if (need == 2) {
      // The 8-byte alignment step belongs to the 8-byte argument alone. Rounding for every argument
      // would pack a 4-byte one at SP+0, skip to SP+8 for its neighbour, and leave a hole that the
      // callee reads as the next parameter — so the size pass above has to agree with this one, word
      // for word, and both only ever align before a double-width value.
      off = (off + 7u) & ~7u;
      const std::uint64_t bits = a.bits;
      if (!memory.Write(frame + off, &bits, 8)) {
        if (err != nullptr) *err = "could not write an 8-byte stack argument at " + std::to_string(frame + off);
        return false;
      }
      off += 8;
    } else {
      const std::uint32_t bits = a.w();
      if (!memory.Write(frame + off, &bits, 4)) {
        if (err != nullptr) *err = "could not write a stack argument at " + std::to_string(frame + off);
        return false;
      }
      off += 4;
    }
  }

  state.r[RegLayout::kLr] = return_address | (state.mode == isa::Mode::Thumb ? 1u : 0u);
  state.pc = fn & (state.mode == isa::Mode::Thumb ? ~1u : ~3u);
  state.r[15] = state.pc;
  return true;
}

Ret32 ReturnValue(const MachineState& state, const Shorty& shorty) {
  Ret32 out{};
  out.pc_after_return = state.pc;
  switch (shorty.returns()) {
    case 'V': break;
    case 'Z':
      out.kind = Value32::Kind::U32;
      // jboolean is an unsigned 8-bit type: the callee leaves 0/1 in the low byte, and reading the
      // whole register would turn a stale upper byte into "true".
      out.bits = (state.r[0] & 0xffu) != 0 ? 1u : 0u;
      break;
    case 'B':
    case 'S':
      out.kind = Value32::Kind::S32;
      out.bits = shorty.returns() == 'B' ? (state.r[0] & 0xffu) : (state.r[0] & 0xffffu);
      break;
    case 'C':  // char, unsigned
      out.kind = Value32::Kind::U32;
      out.bits = state.r[0] & 0xffffu;
      break;
    case 'I':
      out.kind = Value32::Kind::S32;
      out.bits = state.r[0];
      break;
    case 'J':
      out.kind = Value32::Kind::I64;
      out.bits = (static_cast<std::uint64_t>(state.r[1]) << 32) | state.r[0];
      break;
    case 'F':
      /**
       * `-mfloat-abi=softfp`, which is the ABI this project builds every guest for because it is
       * Android's own `armeabi-v7a` ABI: a `jfloat` comes back in **r0**, not in `s0`.
       *
       * This was wrong for a long time and no test could see it, which is worth stating plainly. Every
       * cross-boundary call the suites made until M1's first real APK either took no floating-point
       * argument or returned none — the fixture's `fp_args` passes eight doubles *guest to guest* without
       * ever coming through this file, so neither side was ever asked to agree with the bridge. The first
       * real JNI method to return a `jdouble` produced a result whose low 32 bits matched hardware
       * exactly and whose high half was zero, because the value was in r0:r1 and this function was reading
       * d0 (`docs/30` §Step 24).
       */
      out.kind = Value32::Kind::F32;
      out.bits = state.r[0];
      break;
    case 'D':
      // softfp again: a `jdouble` returns in `r0:r1`, exactly as the `J` case above does. Taken from the
      // guest's own disassembly rather than from the ABI document — the probe's method ends
      // `vmov r0, r1, d18` then `pop {…, pc}`.
      out.kind = Value32::Kind::F64;
      out.bits = (static_cast<std::uint64_t>(state.r[1]) << 32) | state.r[0];
      break;
    default:  // 'L' and anything ART adds later: a 32-bit handle.
      out.kind = Value32::Kind::Ptr;
      out.bits = state.r[0];
      break;
  }
  return out;
}

AbiContext::AbiContext(mem::GuestMemory& memory, GuestAddr stack_top, GuestAddr return_address)
    : memory_{memory},
      slot_base_{return_address + 0x1000u},
      return_address_{return_address},
      stack_bytes_{kCallFrameBytes} {
  // The bridge's own view of the guest's state. Registers start zeroed and every call overwrites the
  // argument ones, so a callee can never read a value the host meant to hide; SP is the stack top and
  // descends one `kCallFrameBytes` frame per nesting level.
  state_.r[RegLayout::kSp] = stack_top;
  state_.r[15] = return_address;
  state_.pc = return_address;
  // An initial value only: `PlaceArgs` derives the mode from bit 0 of every target, and this line used
  // to be the bug — a mode fixed here meant the bridge could not enter ARM-state code at all. It is
  // left set to Thumb because the host-entry slots the guest calls *into* are Thumb-shaped, and because
  // a state with no mode at all would be a worse default than one that matches the common case.
  state_.mode = isa::Mode::Thumb;
}

void AbiContext::Bind(std::uint32_t slot, HostFn fn) {
  // Past `kMaxSlots` there is no address `SlotFor` could name that `IsHostEntry` would map back to
  // this slot, so the binding is dropped rather than aliased onto somebody else's. Callers that need
  // to know ask with `IsBound`.
  if (slot >= kMaxSlots) return;
  bound_[slot] = std::move(fn);
}

bool AbiContext::IsHostEntry(GuestAddr pc, std::uint32_t* slot) const {
  if (pc < slot_base_) return false;
  const GuestAddr delta = pc - slot_base_;
  if (delta % 4u != 0) return false;
  const std::uint32_t id = delta / 4u;
  if (id >= kMaxSlots) return false;
  *slot = id;
  return true;
}

Ret32 AbiContext::CallGuest(GuestAddr fn, const Args32& args, const Shorty& shorty,
                            unsigned max_steps, exec::RunResult* outcome,
                            MachineState* exit_state) {
  // A nested call (a JNI function calling back into the guest) runs on a copy and restores the outer
  // state on the way out, because the outer guest is still mid-instruction: its registers, PC and SP
  // are live. A handler that wants a result out of a nested call takes it from the `Ret32`, *not* from
  // the state — the state it was handed belongs to the outer call and is put back when this returns.
  const MachineState outer = state_;
  Ret32 ret{};
  unsigned remaining = max_steps;
  std::string err;

  if (!PlaceArgs(memory_, state_, fn, args, return_address_, stack_bytes_, &err)) {
    if (outcome != nullptr) {
      outcome->reason = exec::StopReason::Internal;
      outcome->detail = std::move(err);
    }
    // The two checks above are the transactional ones: `PlaceArgs` refuses a call whose frame is too
    // small before touching a register, so the state here still *is* the caller's. A `memory.Write`
    // failing further down is not — SP and the registers are already set by then, and the copy below
    // reports that half-built state. It is a mapped-memory failure rather than a bad argument list, so
    // the outcome is a fault either way, but `exit_state` is documented as "the state when the call
    // ended" and that is what it gets.
    if (exit_state != nullptr) *exit_state = state_;
    state_ = outer;
    return ret;
  }

  exec::RunResult run{};
  unsigned syscalls = 0;
  for (;;) {
    run = executor().Run(state_, remaining);
    if (run.instructions_executed >= remaining) {
      remaining = 0;
    } else {
      remaining -= run.instructions_executed;
    }
    if (run.reason == exec::StopReason::Syscall) {
      // The tier stops with PC already past the instruction and the number in `svc_number`, which is the
      // whole content of the two-tier agreement checked in `exec_tier_diff_test.cpp`: from here the
      // monitor's work is the same regardless of which tier got it here.
      if (monitor_ == nullptr) {
        run.reason = exec::StopReason::Fault;
        run.detail = "guest " + run.detail +
                     " with no syscall monitor attached; continuing would hand the guest a return value "
                     "nobody produced";
        break;
      }
      if (remaining == 0) {
        run.reason = exec::StopReason::Budget;
        run.detail = "call budget exhausted inside a syscall";
        break;
      }
      if (++syscalls > kMaxSyscallsPerCall) {
        run.reason = exec::StopReason::Budget;
        run.detail = "more than " + std::to_string(kMaxSyscallsPerCall) +
                     " syscalls in one call, which is a loop rather than a program";
        break;
      }
      const syscall::Outcome o = monitor_->Handle(state_, run.svc_number);
      if (o.verdict == syscall::Verdict::kExit) {
        run.reason = exec::StopReason::Exited;
        run.detail = o.detail;
        exit_status_ = o.r0;
        break;
      }
      continue;
    }
    if (run.reason != exec::StopReason::HostCall) break;
    if (remaining == 0) {
      run.reason = exec::StopReason::Budget;
      run.detail = "call budget exhausted inside a host call";
      break;
    }

    std::string herr;
    const auto fn = bound_.find(run.host_slot);
    const bool ok = fn != bound_.end() && fn->second(state_, &herr);
    if (!ok) {
      run.reason = exec::StopReason::Fault;
      run.detail = "host call at slot " + std::to_string(run.host_slot) +
                   (fn == bound_.end() ? " is not bound" : "") +
                   (herr.empty() ? "" : ": " + herr);
      break;
    }
    // A host function is a callee, so it returns the way one does. Checked against the slot rather
    // than assumed: a handler that called back into the guest has already left PC where the guest
    // should continue, and overwriting that would resume the wrong instruction.
    if (state_.pc == SlotFor(run.host_slot) && state_.r[RegLayout::kLr] != 0) {
      const GuestAddr lr = state_.r[RegLayout::kLr];
      state_.mode = (lr & 1u) ? isa::Mode::Thumb : isa::Mode::Arm;
      state_.pc = lr & (state_.mode == isa::Mode::Thumb ? ~1u : ~3u);
      state_.r[15] = state_.pc;
    }
  }

  ret = ReturnValue(state_, shorty);
  if (outcome != nullptr) *outcome = run;
  // Copied out before the restore below, which is the only reason this is a parameter rather than
  // something a caller reads back from `state()`.
  if (exit_state != nullptr) *exit_state = state_;
  state_ = outer;
  return ret;
}

}  // namespace rb::abi
