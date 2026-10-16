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

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

#include "rb/syscall/monitor.h"

namespace rb::abi {

namespace {

/// The 16 bytes AAPCS reserves *below* SP for a callee to spill into across a `bl` without adjusting
/// SP (the intra-procedure call stack). Not counting it is the bug where a function works until it
/// calls another one.
constexpr std::uint32_t kIpswr = 16;

std::string Hex32(std::uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

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
      // `jbyte`/`jshort` are *signed*: AAPCS guarantees only the low 8/16 bits of r0, so mask first
      // and then sign-extend. A bare mask left a callee's `(byte) -1` arriving as 255 — the kinds
      // above mask-then-normalise because their types are unsigned, and these two are not.
      out.kind = Value32::Kind::S32;
      out.bits = shorty.returns() == 'B'
                     ? static_cast<std::uint32_t>(static_cast<std::int8_t>(state.r[0] & 0xffu))
                     : static_cast<std::uint32_t>(static_cast<std::int16_t>(state.r[0] & 0xffffu));
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
  // The main thread's view of the guest's state. Registers start zeroed and every call overwrites the
  // argument ones, so a callee can never read a value the host meant to hide; SP is the stack top and
  // descends one `kCallFrameBytes` frame per nesting level.
  MachineState& s = main_.state;
  s.r[RegLayout::kSp] = stack_top;
  s.r[15] = return_address;
  s.pc = return_address;
  // An initial value only: `PlaceArgs` derives the mode from bit 0 of every target, and this line used
  // to be the bug — a mode fixed here meant the bridge could not enter ARM-state code at all. It is
  // left set to Thumb because the host-entry slots the guest calls *into* are Thumb-shaped, and because
  // a state with no mode at all would be a worse default than one that matches the common case.
  s.mode = isa::Mode::Thumb;
  main_.stack_hi = stack_top;
  main_.owner = this;
}

// The main GuestThread's own destructor drops the destroying host thread's binding to it.
AbiContext::~AbiContext() = default;

runtime::GuestThread* AbiContext::BoundThreadOfMine() {
  runtime::GuestThread* t = runtime::CurrentGuestThread();
  if (t == nullptr || !runtime::BoundHere(*t)) return nullptr;
  // `owner` is compared, never dereferenced: a thread first used by another context is not ours.
  if (t->owner != nullptr && t->owner != this) return nullptr;
  return t;
}

runtime::GuestThread* AbiContext::ThreadForThisHost(std::string* why) {
  if (runtime::GuestThread* t = BoundThreadOfMine()) {
    t->owner = this;  // only the bound host thread writes it, and only before its first call here
    return t;
  }
  // No binding of ours on this host thread. With an adopter (a `GuestProcess`), the host thread gets a guest
  // thread of its own — the main one belongs only to the host thread that ran `LoadScope` (M2 Task 5 ruling a).
  if (adopter_) {
    std::string e;
    runtime::GuestThread* t = adopter_(&e);
    if (t != nullptr && runtime::BoundHere(*t)) {
      t->owner = this;
      return t;
    }
    if (e.empty()) {
      e = t == nullptr ? "the adopter returned no guest thread and gave no reason"
                       : "the adopter returned a guest thread that is not bound to this host thread";
    }
    if (why != nullptr) *why = "host thread not bound to a guest thread, and adopting it failed: " + e;
    return nullptr;
  }
  // A bare bridge (no adopter: a single-threaded rig): the main thread goes to the first host thread that asks,
  // or back to the one that holds it. `BindCurrentHostThread` refuses a main thread another live host thread
  // holds, so two unbound host threads cannot both get it; a holder that exited has released it.
  if (runtime::BindCurrentHostThread(main_)) {
    main_.owner = this;
    return &main_;
  }
  if (why != nullptr) {
    *why = "host thread not bound to a guest thread (this bridge has no adopter, and its main guest thread is "
           "bound to another live host thread)";
  }
  return nullptr;
}

MachineState& AbiContext::state() {
  if (runtime::GuestThread* t = BoundThreadOfMine()) return t->state;
  return main_.state;
}

void AbiContext::set_executor(exec::Executor& executor) {
  executor_.store(&executor, std::memory_order_release);
  if (runtime::GuestThread* t = BoundThreadOfMine()) t->executor = &executor;
}

exec::Executor& AbiContext::executor() const {
  runtime::GuestThread* t = const_cast<AbiContext*>(this)->BoundThreadOfMine();
  return (t != nullptr && t->executor != nullptr) ? *t->executor : *executor_.load(std::memory_order_acquire);
}

void AbiContext::set_monitor(syscall::Monitor* monitor) {
  monitor_ = monitor;
  // The monitor answers a syscall from the calling thread's per-thread fields; for the main thread those
  // are `main_`'s, so it takes over whatever `set_ids`/`set_clear_child_tid` recorded before this.
  if (monitor_ != nullptr) monitor_->set_main_thread(&main_);
}

void AbiContext::Bind(std::uint32_t slot, HostFn fn, SlotLock lock) {
  // Past `kMaxSlots` there is no address `SlotFor` could name that `IsHostEntry` would map back to
  // this slot, so the binding is dropped rather than aliased onto somebody else's. Callers that need
  // to know ask with `IsBound`.
  if (slot >= kMaxSlots) return;
  auto entry = std::make_shared<const Slot>(Slot{std::move(fn), lock});
  runtime::Ggl::Held hold(ggl_);
  bound_[slot] = std::move(entry);
}

void AbiContext::SetSlotLock(std::uint32_t slot, SlotLock lock) {
  runtime::Ggl::Held hold(ggl_);
  const auto it = bound_.find(slot);
  if (it != bound_.end()) it->second = std::make_shared<const Slot>(Slot{it->second->fn, lock});
}

bool AbiContext::IsBound(std::uint32_t slot) const {
  runtime::Ggl::Held hold(const_cast<runtime::Ggl&>(ggl_));
  return bound_.count(slot) != 0;
}

bool AbiContext::IsHostEntry(GuestAddr pc, std::uint32_t* slot) const {
  // The kuser helpers are entered in ARM state at their exact addresses (a `blx`/`mov pc` to a word-
  // aligned constant), so an exact match is the whole test.
  switch (pc) {
    case kKuserCmpxchg64: *slot = kKuserSlotBase + 0; return true;
    case kKuserMemoryBarrier: *slot = kKuserSlotBase + 1; return true;
    case kKuserCmpxchg: *slot = kKuserSlotBase + 2; return true;
    case kKuserGetTls: *slot = kKuserSlotBase + 3; return true;
    default: break;
  }
  if (pc < slot_base_) return false;
  const GuestAddr delta = pc - slot_base_;
  if (delta % 4u != 0) return false;
  const std::uint32_t id = delta / 4u;
  if (id >= kMaxSlots) return false;
  *slot = id;
  return true;
}

bool AbiContext::RunKuserHelper(MachineState& state, std::uint32_t which, std::string* err) {
  // The kernel's contract (kernel_user_helpers.rst): cmpxchg returns r0 = 0 with C set when it stored,
  // non-zero with C clear when it did not; r3, ip and the flags are clobbered; get_tls returns the TLS
  // value in r0 and touches nothing else. Called with the GGL held: the read-then-write below is atomic
  // with respect to every other guest thread only because no other one runs meanwhile.
  constexpr std::uint32_t kCarry = 1u << 29;
  auto& r = state.r;
  const auto set_carry = [&](bool c) { state.cpsr = c ? (state.cpsr | kCarry) : (state.cpsr & ~kCarry); };
  switch (which) {
    case 0: {  // __kuser_cmpxchg64(const int64_t* oldval, const int64_t* newval, volatile int64_t* ptr)
      std::uint64_t oldv = 0, newv = 0, cur = 0;
      if (!memory_.Read(r[0], &oldv, 8) || !memory_.Read(r[1], &newv, 8) || !memory_.Read(r[2], &cur, 8)) {
        *err = "__kuser_cmpxchg64: an operand is not readable";
        return false;
      }
      const bool ok = cur == oldv;
      if (ok && !memory_.Write(r[2], &newv, 8)) {
        *err = "__kuser_cmpxchg64: the target is not writable";
        return false;
      }
      r[0] = ok ? 0u : 1u;
      set_carry(ok);
      return true;
    }
    case 1:  // __kuser_memory_barrier: guest threads run one at a time under the GGL, so ordering is total
      return true;
    case 2: {  // __kuser_cmpxchg(int32_t oldval, int32_t newval, volatile int32_t* ptr)
      std::uint32_t cur = 0;
      if (!memory_.Read(r[2], &cur, 4)) {
        *err = "__kuser_cmpxchg: the target is not readable";
        return false;
      }
      const bool ok = cur == r[0];
      if (ok && !memory_.Write(r[2], &r[1], 4)) {
        *err = "__kuser_cmpxchg: the target is not writable";
        return false;
      }
      r[3] = cur;
      r[0] = ok ? 0u : 1u;
      set_carry(ok);
      return true;
    }
    case 3:  // __kuser_get_tls
      r[0] = state.tp;
      return true;
    default:
      *err = "no such kuser helper";
      return false;
  }
}

Ret32 AbiContext::CallGuest(GuestAddr fn, const Args32& args, const Shorty& shorty,
                            unsigned max_steps, exec::RunResult* outcome,
                            MachineState* exit_state) {
  return CallGuestImpl(fn, args, shorty, max_steps, false, outcome, exit_state);
}

Ret32 AbiContext::CallGuestToCompletion(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned turn_budget,
                                        exec::RunResult* outcome) {
  return CallGuestImpl(fn, args, shorty, turn_budget, true, outcome, nullptr);
}

Ret32 AbiContext::CallGuestImpl(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned max_steps,
                                bool resume_on_budget, exec::RunResult* outcome, MachineState* exit_state) {
  std::string why;
  runtime::GuestThread* const thread = ThreadForThisHost(&why);
  if (thread == nullptr) {
    // Refused by name rather than run on somebody else's registers: a host thread that is not a guest
    // thread has no stack, no tid and no TLS of its own here (and it could not be adopted).
    if (outcome != nullptr) {
      *outcome = exec::RunResult{};
      outcome->reason = exec::StopReason::Internal;
      outcome->detail = std::move(why);
    }
    return Ret32{};
  }
  // Held from here to the restore below, except where `RunThread` releases it (slices, host slots,
  // blocking syscalls) — never across the marshalling or the restore, which touch this thread's registers
  // and guest memory.
  runtime::Ggl::Held hold(ggl_);
  MachineState& state = thread->state;

  // A nested call (a JNI function calling back into the guest) runs on a copy and restores the outer
  // state on the way out, because the outer guest is still mid-instruction: its registers, PC and SP
  // are live. A handler that wants a result out of a nested call takes it from the `Ret32`, *not* from
  // the state — the state it was handed belongs to the outer call and is put back when this returns.
  const MachineState outer = state;
  Ret32 ret{};
  std::string err;

  if (!PlaceArgs(memory_, state, fn, args, return_address_, stack_bytes_, &err)) {
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
    if (exit_state != nullptr) *exit_state = state;
    state = outer;
    return ret;
  }

  exec::RunResult run = RunThread(*thread, max_steps);
  // To completion (a thread's own work, e.g. its TLS-key destructors at exit): a turn that ran out of budget is
  // re-entered from where it stopped, as a clone()d thread's `ThreadMain` re-enters `RunThread`, until the callee
  // returns, exits, faults or the process exits.
  while (resume_on_budget && run.reason == exec::StopReason::Budget &&
         !(monitor_ != nullptr && monitor_->group_exited())) {
    run = RunThread(*thread, max_steps);
  }
  ret = ReturnValue(state, shorty);
  if (outcome != nullptr) *outcome = run;
  // Copied out before the restore below, which is the only reason this is a parameter rather than
  // something a caller reads back from `state()`.
  if (exit_state != nullptr) *exit_state = state;
  state = outer;
  return ret;
}

exec::RunResult AbiContext::RunThread(runtime::GuestThread& thread, unsigned budget) {
  runtime::Ggl::Held hold(ggl_);
  MachineState& state = thread.state;
  exec::Executor* const engine =
      thread.executor != nullptr ? thread.executor : executor_.load(std::memory_order_acquire);
  exec::RunResult run{};
  if (engine == nullptr) {
    run.reason = exec::StopReason::Internal;
    run.detail = "no executor is set on this bridge";
    return run;
  }
  unsigned remaining = budget;
  unsigned held = 0;  ///< progress under the current hold of the GGL (Tier I: IR steps) — the slice clock
  // Checked after every (re-)acquisition of the GGL — the loop top, which every slice boundary returns to, and
  // right after a blocking wait or a released host slot — before any guest work, write-back or dispatch (M2
  // Task 4 fix round 1).
  const auto group_stop = [&] {
    if (monitor_ == nullptr || !monitor_->group_exited()) return false;
    run.reason = exec::StopReason::Exited;
    run.detail = "the guest process exited (" + monitor_->group_exit_reason() + "); this thread stopped with it";
    return true;
  };
  unsigned syscalls = 0;
  for (;;) {
    // `exit_group` on another thread (or a fatal fault in one, or the process's teardown) ends this one too: it
    // stops here, at its next slice boundary, syscall or host call, instead of running guest code in a process
    // that has finished (Review Focus: no host thread is left running guest code).
    if (group_stop()) break;
    if (held >= kSliceSteps) {
      // The slice boundary: hand the GGL to the longest waiter (the lock is a FIFO ticket lock, so this
      // thread queues behind it), then carry on. State is consistent here — the executor stopped on a
      // budget, exactly as it does at the end of any call's budget — and both tiers start every Run with
      // no exclusive reservation, so an LL/SC sequence split by the switch fails its strex and retries.
      slices_.fetch_add(1, std::memory_order_relaxed);
      if (ggl_.waiters() > 0) contended_slices_.fetch_add(1, std::memory_order_relaxed);
      {
        runtime::Ggl::Released off(ggl_);
        std::this_thread::yield();
      }
      held = 0;
      continue;  // re-acquired: back to the top, which re-checks for a group exit before any guest work
    }
    const unsigned slice = std::min(remaining, kSliceSteps - held);
    run = engine->Run(state, slice);
    const unsigned done = std::min(run.instructions_executed, remaining);
    remaining -= done;
    held += done;
    if (run.reason == exec::StopReason::Budget) {
      // A slice ran out (go round: the top of the loop releases the lock) or the whole call's budget did.
      if (remaining == 0 || done == 0) break;
      continue;
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
      // The budget and the syscall cap are checked only *after* this syscall is answered: the tier already
      // stepped past the SVC, so a Budget stop here would have a re-entered turn (ThreadMain,
      // CallGuestToCompletion) resume after it with r0 still holding arg0 — a silently skipped syscall.
      syscall::Outcome o = monitor_->Begin(state, run.svc_number, &thread);
      if (o.verdict == syscall::Verdict::kBlocking) {
        // The wait runs with the GGL released, so other guest threads run meanwhile; the answer is recorded
        // and written back once the lock is held again.
        syscall::Outcome waited;
        {
          runtime::Ggl::Released off(ggl_);
          waited = o.block ? o.block()
                           : syscall::Outcome::Refused(syscall::err::kEnosys, "a blocking answer with no wait");
        }
        held = 0;
        if (group_stop()) break;  // re-acquired after the wait: no write-back, no guest work in an exited process
        o = monitor_->Finish(state, std::move(waited));
      }
      if (o.verdict == syscall::Verdict::kExit) {
        run.reason = exec::StopReason::Exited;
        run.detail = o.detail;
        exit_status_ = o.r0;
        break;
      }
      if (o.verdict == syscall::Verdict::kThreadExit) {
        // `exit` (1): this guest thread is finished; the process and its other threads carry on.
        run.reason = exec::StopReason::Exited;
        run.detail = o.detail;
        break;
      }
      if (remaining == 0) {
        run.reason = exec::StopReason::Budget;
        run.detail = "call budget exhausted at a syscall (answered; the state is past it)";
        break;
      }
      if (++syscalls > kMaxSyscallsPerCall) {
        run.reason = exec::StopReason::Budget;
        run.detail = "more than " + std::to_string(kMaxSyscallsPerCall) +
                     " syscalls in one call, which is a loop rather than a program (the last was answered)";
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
    if (run.host_slot >= kKuserSlotBase && run.host_slot < kKuserSlotBase + 4u) {
      // Under the GGL, never released: this is what makes the helpers atomic.
      if (!RunKuserHelper(state, run.host_slot - kKuserSlotBase, &herr)) {
        run.reason = exec::StopReason::Fault;
        run.detail = "kuser helper at 0x" + Hex32(state.pc) + ": " + herr;
        break;
      }
      // Every helper ends in `bx lr`.
      const GuestAddr lr = state.r[RegLayout::kLr];
      state.mode = (lr & 1u) ? isa::Mode::Thumb : isa::Mode::Arm;
      state.pc = lr & (state.mode == isa::Mode::Thumb ? ~1u : ~3u);
      state.r[15] = state.pc;
      continue;
    }
    SlotRef slot;  // our own reference: a `Bind` while this slot runs (GGL released) replaces, never frees, it
    if (const auto it = bound_.find(run.host_slot); it != bound_.end()) slot = it->second;
    bool ok = false;
    if (slot) {
      if (slot->lock == SlotLock::kHoldsGgl) {
        ok = slot->fn(state, &herr);
      } else {
        // The rule for host code: it may block (a GL driver, a host condition variable, a nested guest
        // call that itself waits), so other guest threads must be able to run while it does. A nested
        // `CallGuest` it makes re-acquires the lock to run guest code (and itself releases it at slices,
        // host slots and blocking syscalls, like any call).
        runtime::Ggl::Released off(ggl_);
        ok = slot->fn(state, &herr);
      }
      if (slot->lock != SlotLock::kHoldsGgl) held = 0;
      // Re-acquired after a released slot, or after a held slot whose nested guest call released it inside.
      if (group_stop()) break;
    }
    if (!ok) {
      run.reason = exec::StopReason::Fault;
      run.detail = "host call at slot " + std::to_string(run.host_slot) +
                   (!slot ? " is not bound" : "") +
                   (herr.empty() ? "" : ": " + herr);
      break;
    }
    // A host function is a callee, so it returns the way one does. Checked against the slot rather
    // than assumed: a handler that called back into the guest has already left PC where the guest
    // should continue, and overwriting that would resume the wrong instruction.
    if (state.pc == SlotFor(run.host_slot) && state.r[RegLayout::kLr] != 0) {
      const GuestAddr lr = state.r[RegLayout::kLr];
      state.mode = (lr & 1u) ? isa::Mode::Thumb : isa::Mode::Arm;
      state.pc = lr & (state.mode == isa::Mode::Thumb ? ~1u : ~3u);
      state.r[15] = state.pc;
    }
  }

  if (run.reason == exec::StopReason::Fault && fault_namer_) {
    const std::string what = fault_namer_(state.pc);
    if (!what.empty()) run.detail += (run.detail.empty() ? "" : ": ") + what;
  }
  // The whole call's count, not the last segment's: `run` is the slice after the final host call or
  // syscall, so a call that made any reported only its tail — Gumball's jniRun read "38 steps" every frame
  // while issuing 17 GL calls. Nested calls a host function makes run on their own budgets and are not
  // included.
  run.instructions_executed = budget - remaining;
  return run;
}

}  // namespace rb::abi
