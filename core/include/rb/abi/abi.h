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
 * floating-point values in **r0-r3 / r0:r1**, and only the arithmetic uses `d[]`. Four independent
 * checks settled it, and they are written down because the wrong version had already been believed
 * once: the tablet's own `/system/lib/libm.so` shows `sqrt` as
 * `vmov d16, r0, r1; vsqrt.f64; vmov r0, r1, d16`; the guest probe compiled for this project ends
 * `vmov r0, r1, d18`; clang **refuses** `-mfloat-abi=hard` outright for
 * `armv7-none-linux-android26` ("unsupported option"), so there is no hard-float variant to be
 * confused about; and the probe's own entry points read their parameters from r2 and r2:r3
 * (`vmov s4, r2`, `vmov d18, r2, r3`), which is the argument direction rather than the return one.
 *
 * The consequence, stated so the next reader does not re-derive it: a `jdouble` argument occupies a core
 * register *pair* and a `jdouble` return arrives in `r0:r1`, exactly like a `jlong`. Both halves of that
 * sentence used to be wrong in this file, in opposite directions — `ReturnValue` read `d0` and
 * `PlaceArgs` wrote `d0` — and `PlaceArgs`'s half took five new JNI entry points in a real APK before a
 * row existed that could fail on it.
 *
 * One caveat that is easy to trip over when reading a disassembly of a *guest library*: this is the rule
 * at the **boundary**. Clang still gives a `static` function whose address never escapes its translation
 * unit the VFP register ABI under the very same `-mfloat-abi=softfp` (`sum8` in
 * `tests/corpus/fixture_jni.c` is called with `d0`-`d7`). That is guest-internal and needs no support
 * here — the CPU emulates whichever convention the compiler chose — but "softfp therefore never VFP" is
 * false as a statement about disassembly, and true only as a statement about calls crossing into a
 * library.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/guest_thread.h"

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
 * an 8-byte argument needs an even/odd core-register **pair** (and so can consume an odd slot for
 * nothing), while a 4-byte one takes the next slot whatever it is. Deciding that later from the bits
 * alone is not possible — `0x0000000100000000` and the double 2.0 are the same 64 bits. Under softfp the
 * kind selects only the *width*; it never selects a different register bank.
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

/// A return value: `r0`, or the `r0`/`r1` pair for a 64-bit result — which under softfp includes every
/// `jdouble` as well as every `jlong`. Reading the wrong register for a `jdouble` is a silent truncation
/// rather than a crash, so which register a result came from is recorded rather than guessed from the
/// shorty at every call site.
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
/// AAPCS rather than against another call site. There is deliberately no entry for a VFP *argument*
/// register: `PlaceArgs` allocates none, because softfp passes arguments in the core bank.
struct RegLayout {
  static constexpr int kFirstCoreArg = 0;   ///< r0
  static constexpr int kLastCoreArg = 3;    ///< r3
  static constexpr int kCoreArgCount = 4;   ///< a1-a4
  static constexpr int kLr = 14;
  static constexpr int kSp = 13;
};

/**
 * Places `args` into `state` per the ARM Architecture Procedure Call Standard **in its soft-float
 * variant**, sets PC to `fn` and LR to `return_address`, and writes the spilled arguments into the guest
 * stack.
 *
 * The rules, and the failure each one prevents:
 *
 *   - Integers and pointers fill r0..r3 in order. Overflow goes to the stack, in order, at ascending
 *     addresses, and the block starts **at the callee's entry SP** — the value SP holds at the `bl`,
 *     which is also the address its `[sp]` load reads. (AAPCS passes stack arguments left-to-right *in
 *     memory*, which is where a little-endian "reverse the args" reflex produces a call that works for
 *     one argument and is wrong for two.)
 *   - The 16 bytes AAPCS lets a callee write without adjusting SP are *below* that entry SP, not between
 *     it and the argument block. `PlaceArgs` used to put them between, so every spilled argument sat
 *     sixteen bytes away from where a real guest looked for it, and the one test that read the spill area
 *     added the same sixteen and stayed green over the top of it.
 *   - An 8-byte argument — `jlong` **or** `jdouble` — needs a consecutive even/odd core-register pair:
 *     if the next free register is odd-numbered, it is skipped and consumed by nothing. Passing a `jlong`
 *     in r1/r2 is the classic failure this file is named for, and it is invisible until a call has an odd
 *     number of preceding 4-byte arguments. The `jdouble` case is the same rule and was the same bug.
 *   - An 8-byte *stack* argument keeps its own 8-byte alignment inside the argument block, so a block
 *     that currently holds one 4-byte argument gets 4 bytes of padding before it. Packing tightly instead
 *     is off by four bytes on exactly the calls that have an odd number of spilled 32-bit arguments.
 *   - **There is no VFP path, and that is the softfp rule rather than a simplification.** A `jfloat` is
 *     placed as an `int32_t` and a `jdouble` as an `int64_t`, because `armeabi-v7a` is
 *     `-mfloat-abi=softfp`: arguments and results follow the *soft-float* PCS and only the arithmetic
 *     uses the VFP registers. `PlaceArgs` used to allocate floats from `d0..d7` independently of the core
 *     bank, which is the *hard*-float convention — see the note at the top of this header, and the
 *     disassembly in `core/abi/call.cpp` that measures it rather than recalling it.
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
/// `MachineState` (so it reads its AAPCS arguments from r0..r3, with a 64-bit argument in an even/odd
/// pair, and writes its result to r0 or r0:r1 — the same softfp placement `PlaceArgs` performs, because
/// from the guest's side a host function *is* a callee) and returns false is a refusal that stops
/// execution with `Fault` naming the slot. It is *not* a zero result: a JNI function that quietly returns
/// NULL is far harder to diagnose than one that says it is missing, and an app that survives it behaves
/// differently still.
using HostFn = std::function<bool(MachineState&, std::string* err)>;

/**
 * One guest instance's call bridge: the host-entry table, the run/resume loop, and the stack it hands
 * callees.
 *
 * It *is* the `exec::HostDispatch` the executor consults, so a guest `blx` to any address this object
 * published stops with `StopReason::HostCall` instead of trying to execute memory that does not exist.
 *
 * **Threads (M2).** Registers and per-thread kernel state live in a `runtime::GuestThread`; a host thread
 * runs guest code on the one it is bound to (`runtime::BindCurrentHostThread`). The bridge owns the process's
 * main `GuestThread` and its Global Guest Lock (`runtime::Ggl`): `CallGuest` holds the lock while guest
 * code runs and releases it at every slice boundary (`kSliceSteps`), around host-slot calls bound
 * `SlotLock::kReleasesGgl` (the default is to hold it) and around a blocking syscall's wait. The executors
 * are shared by every thread — they are stateless between `Run` calls — and so is the host-entry table:
 * `Bind` belongs to setup, before more than one host thread calls in.
 */
class AbiContext : public exec::HostDispatch {
 public:
  /// The largest host function index `SlotFor` can name. JNI's own table is 229 entries; the rest is
  /// headroom for host-provided symbols, and `IsHostEntry` refuses anything past it rather than
  /// aliasing one slot onto another.
  static constexpr std::uint32_t kMaxSlots = 1024;
  /**
   * The Linux ARM "kuser helpers": fixed addresses at the top of every 32-bit process the kernel
   * answers with tiny routines (`Documentation/arm/kernel_user_helpers.rst`). Old GCC-built code calls
   * them for atomics and barriers instead of using ldrex/strex — Angry Birds Go's static constructors
   * branch to `__kuser_memory_barrier` (0xffff0fa0). They are recognised here as host entries, beyond
   * `kMaxSlots` so no `Bind` can alias them, and `CallGuest` runs their semantics and returns to `lr`.
   * They run with the GGL held (it is never released around them), which is what makes each one atomic
   * with respect to every other guest thread.
   */
  static constexpr GuestAddr kKuserCmpxchg64 = 0xffff'0f60u;
  static constexpr GuestAddr kKuserMemoryBarrier = 0xffff'0fa0u;
  static constexpr GuestAddr kKuserCmpxchg = 0xffff'0fc0u;
  static constexpr GuestAddr kKuserGetTls = 0xffff'0fe0u;
  static constexpr std::uint32_t kKuserSlotBase = kMaxSlots;  ///< +0 cmpxchg64, +1 barrier, +2 cmpxchg, +3 get_tls
  /// How much guest stack one call gets: its spilled arguments plus the 16-byte intra-procedure area.
  /// A nesting level descends by this much, so the mapped stack bounds how deep a guest may call
  /// back into the host — which is a limit to state, not to discover at runtime.
  static constexpr unsigned kCallFrameBytes = 8u << 10;
  /// How many syscalls one `CallGuest` may service before the loop is treated as stuck. A number rather
  /// than an unbounded loop because a guest spinning on a refused call would otherwise spin the host
  /// forever inside one JNI method, and the failure that produces is a hung UI, not an error message.
  /// Real apps make thousands of syscalls per *launch*; per call, this is generous.
  static constexpr unsigned kMaxSyscallsPerCall = 100'000u;
  /// The preemption slice: after this many units of executor progress under one hold of the GGL — guest
  /// instructions on Tier J, but IR steps on Tier I (its `Run` counts IR steps, not guest instructions, so a
  /// Tier I slice is a different number of guest instructions than a Tier J one) —
  /// the running thread releases it, yields, and re-acquires — so a guest thread spinning without a syscall
  /// cannot starve the others (spec: Runtime, GGL).
  static constexpr unsigned kSliceSteps = 200'000u;

  /// Whether a host slot runs with the GGL held (the default since M2 Task 4's fix round: the safe choice) or
  /// released. `kReleasesGgl` is for host code that may block on something other than a guest thread and
  /// touches no guest memory and no GGL-protected state while it runs — marshal in and out before and after,
  /// with the lock held. Held slots (the JNIEnv table, the dynamic-linker and log slots, the launcher's GL
  /// forwarding, whose driver reads guest memory) never wait on another guest thread. What is held is exactly
  /// the slot's own host code: the GGL is not released
  /// between the slot's entry and its return. A nested guest call such a slot makes (a host method calling
  /// back into the guest) does *not* keep the lock throughout: its `RunThread` releases it at every slice
  /// boundary, around every `kReleasesGgl` host slot and around every blocking syscall, exactly as a
  /// top-level call does, and re-acquires before returning to the slot — so the slot's host code must not
  /// rely on process state staying unchanged across a nested call.
  enum class SlotLock { kReleasesGgl, kHoldsGgl };

  /// No executor at construction, deliberately: the bridge *is* the dispatch table the executor has to
  /// consult, so the two reference each other. Taking the executor later keeps that a one-way
  /// dependency at any instant rather than requiring either object to be born half-built.
  AbiContext(mem::GuestMemory& memory, GuestAddr stack_top, GuestAddr return_address);
  ~AbiContext() override;

  /// The tier guest calls run on. Set from a host thread bound to one of this context's guest threads, it
  /// is that thread's choice (so another thread's `set_executor` cannot change the tier of a call this one
  /// is about to make); it is also the default for threads that never chose.
  void set_executor(exec::Executor& executor);
  [[nodiscard]] exec::Executor& executor() const;

  /// The monitor an SVC reaches. Optional in the type and not optional in a real launch: without one, a
  /// guest syscall stops the call with a `Fault` that says so, because silently continuing past an SVC
  /// the host never answered is the same silent-ignore mistake in a place where the guest is now holding
  /// a return value that was never produced. Attaching hands the monitor this context's main thread.
  void set_monitor(syscall::Monitor* monitor);

  /// Makes a guest thread for a host thread that calls in with no binding of this context's (M2 Task 5): returns
  /// it already bound to the calling host thread, or null with `why`. Called without the GGL (it takes it). Set
  /// once, at setup, before any host thread calls in.
  using Adopter = std::function<runtime::GuestThread*(std::string* why)>;
  void set_adopter(Adopter adopter) { adopter_ = std::move(adopter); }
  /// Says what a fault's PC is when the context alone cannot (`GuestProcess`: an unanswered import's trap slot,
  /// by name), or "" for nothing to add. `RunThread` appends it to a `Fault`'s detail — every call, guest thread
  /// and adopted host thread runs there. Called under the GGL. Set once, at setup.
  using FaultNamer = std::function<std::string(GuestAddr pc)>;
  void set_fault_namer(FaultNamer namer) { fault_namer_ = std::move(namer); }
  [[nodiscard]] syscall::Monitor* monitor() const { return monitor_; }

  /// Set when a call ended in `StopReason::Exited`; the status the guest passed to `exit_group`.
  [[nodiscard]] std::int32_t exit_status() const { return exit_status_; }

  /// Makes host function `slot` callable by the guest. Re-binding replaces, which is what lets a
  /// profile override a default rather than needing a second table. Takes the GGL (the table is GGL-protected,
  /// and a slot running with the lock released keeps its own reference to the entry it runs), so it is safe
  /// after guest threads exist (M2 Task 5 ruling e).
  /// The default is `kHoldsGgl`, the safe one: a slot that releases the lock must say so (`kReleasesGgl`), because
  /// its host code then must not touch guest memory or any GGL-protected state (M2 Task 4 fix round 1).
  void Bind(std::uint32_t slot, HostFn fn, SlotLock lock = SlotLock::kHoldsGgl);
  /// Changes how an already-bound slot treats the GGL (no-op for an unbound slot).
  void SetSlotLock(std::uint32_t slot, SlotLock lock);
  [[nodiscard]] bool IsBound(std::uint32_t slot) const;

  /// The guest address a table entry must hold for host function `slot`. Slots are laid out
  /// contiguously from `slot_base`, which is what lets a JNI table be filled by arithmetic — and why
  /// no page is mapped for them.
  GuestAddr SlotFor(std::uint32_t slot) const { return slot_base_ + slot * 4u; }
  static constexpr std::uint32_t kInvalidSlot = 0xffff'ffffu;

  bool IsHostEntry(GuestAddr pc, std::uint32_t* slot) const override;

  /// Marshals, runs until the callee returns, servicing host calls on the way, and returns the result.
  /// `max_steps` bounds the whole call, host calls included, so a guest loop that never returns cannot
  /// take the host with it.
  ///
  /// It runs on the calling host thread's `GuestThread` (`runtime::CurrentGuestThread()`). A host thread with
  /// no binding of this context's is *adopted* when an adopter is installed (`set_adopter`; `GuestProcess`
  /// installs one, M2 Task 5): it gets a guest thread of its own, never the main one. With no adopter (a bare
  /// single-threaded bridge) it gets the main thread when no live host thread holds that (the first caller
  /// claims it); otherwise the call is refused by name (`StopReason::Internal`, "host thread not bound to a
  /// guest thread"). The GGL is acquired unless already held.
  ///
  /// `exit_state` is optional and exists because without it there is no way to ask the question: the
  /// bridge restores the caller's registers on the way out — correctly, since a nested call's caller is
  /// mid-instruction — so `state()` after a `CallGuest` is the state *before* it, and reading it as the
  /// callee's last state reports a set of registers that were never involved. That misreading cost a
  /// wrong public diagnosis during `docs/30` §Step 24, which is why the field is now explicit rather
  /// than left to be inferred.
  Ret32 CallGuest(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned max_steps,
                  exec::RunResult* outcome, MachineState* exit_state = nullptr);
  /// `CallGuest` that re-enters the callee each time a `turn_budget` runs out, until it returns, exits, faults or the
  /// process exits (a thread's own unbounded work: never for a call the host must bound). `outcome` is the last
  /// turn's.
  Ret32 CallGuestToCompletion(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned turn_budget,
                              exec::RunResult* outcome, MachineState* exit_state = nullptr);

  /// The JNI-shaped entry point C09 will use: a shorty plus the values already converted. Kept next to
  /// `CallGuest` so there is one marshalling path, not one per caller.
  Ret32 CallGuestJ(GuestAddr fn, const Shorty& shorty, const std::vector<Value32>& args,
                   unsigned max_steps, exec::RunResult* outcome) {
    return CallGuest(fn, Args32{args}, shorty, max_steps, outcome);
  }

  /// The run loop: runs `thread` from its current state until it returns to the sentinel, exits, faults
  /// or spends `budget`, servicing syscalls and host calls and slicing for the GGL on the way. Acquires the
  /// GGL unless held. `instructions_executed` is the whole run's count. `CallGuest` and thread entry use it.
  exec::RunResult RunThread(runtime::GuestThread& thread, unsigned budget);

  /// The calling host thread's guest-thread state when it is bound to one of this context's threads, else
  /// the main thread's — the state a single-threaded caller sets `tp` or SP on before its first call.
  MachineState& state();
  mem::GuestMemory& memory() { return memory_; }
  GuestAddr return_address() const { return return_address_; }
  unsigned stack_bytes() const { return stack_bytes_; }

  runtime::GuestThread& main_thread() { return main_; }
  runtime::Ggl& ggl() { return ggl_; }
  /// How many slice boundaries released the GGL so far (all threads).
  [[nodiscard]] std::uint64_t slices() const { return slices_.load(std::memory_order_relaxed); }
  /// Slice boundaries at which another host thread was waiting for the GGL — i.e. the boundary handed guest
  /// execution to a different thread. Evidence of real interleaving, which `slices()` alone is not.
  [[nodiscard]] std::uint64_t contended_slices() const { return contended_slices_.load(std::memory_order_relaxed); }

 private:
  struct Slot {
    HostFn fn;
    SlotLock lock = SlotLock::kHoldsGgl;
  };
  /// Entries are immutable once published: `Bind`/`SetSlotLock` replace the pointer under the GGL, and a call
  /// in flight (possibly with the GGL released) holds its own reference.
  using SlotRef = std::shared_ptr<const Slot>;
  /// One kuser helper's semantics on `state` (index: 0 cmpxchg64, 1 barrier, 2 cmpxchg, 3 get_tls).
  bool RunKuserHelper(MachineState& state, std::uint32_t which, std::string* err);
  /// The guest thread a call from this host thread runs on (see `CallGuest`), or null with `why`.
  runtime::GuestThread* ThreadForThisHost(std::string* why);
  Ret32 CallGuestImpl(GuestAddr fn, const Args32& args, const Shorty& shorty, unsigned max_steps,
                      bool resume_on_budget, exec::RunResult* outcome, MachineState* exit_state);
  /// The calling host thread's guest thread when it belongs to this context, else null.
  runtime::GuestThread* BoundThreadOfMine();

  mem::GuestMemory& memory_;
  /// The default tier, for threads that never chose: atomic, since any host thread may set it (ruling e).
  std::atomic<exec::Executor*> executor_{nullptr};
  syscall::Monitor* monitor_ = nullptr;
  Adopter adopter_;
  FaultNamer fault_namer_;
  std::int32_t exit_status_ = 0;
  std::unordered_map<std::uint32_t, SlotRef> bound_;  ///< GGL-protected
  runtime::Ggl ggl_;
  runtime::GuestThread main_;
  std::atomic<std::uint64_t> slices_{0};
  std::atomic<std::uint64_t> contended_slices_{0};
  GuestAddr slot_base_;
  GuestAddr return_address_;
  unsigned stack_bytes_;
};

}  // namespace rb::abi
