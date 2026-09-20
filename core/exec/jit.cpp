/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — Tier J: Dynarmic's AArch64 back end behind the `Executor` seam.
 * See rb/exec/jit.h for the two-tier reasoning, the budget units, and why there is no fastmem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/exec/jit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

#include "rb/exec/interp.h"
#include "rb/isa/front_end.h"

namespace rb::exec {

namespace {

constexpr std::uint32_t kCpsrTFlag = 1u << 5;

/// `HaltReason` is a bitmask enum with `operator&` but no conversion to bool, so a test has to spell out
/// that it means "any of these bits". Named rather than an inline cast because dynarmic exports a `Has`
/// found by ADL on its own enum, and two candidates for one unqualified call is a compile error.
[[nodiscard]] constexpr bool HaltHas(Dynarmic::HaltReason got, Dynarmic::HaltReason want) {
  return (static_cast<std::uint32_t>(got) & static_cast<std::uint32_t>(want)) != 0;
}

/// The widths of Dynarmic's register views, asserted against the types rather than against a call:
/// `jit_->Regs().size()` is not a constant expression. `remove_reference` because those accessors hand
/// back lvalue references, and `sizeof` rather than `size()` because libc++ declares `std::array::size`
/// as a non-static member, so even `T::size()` needs an object.
using JitRegs = std::remove_reference_t<decltype(std::declval<Dynarmic::A32::Jit&>().Regs())>;
using JitExtRegs = std::remove_reference_t<decltype(std::declval<Dynarmic::A32::Jit&>().ExtRegs())>;
static_assert(sizeof(JitRegs) == 16 * sizeof(std::uint32_t), "the guest register file is 16 words");
static_assert(sizeof(JitExtRegs) == 64 * sizeof(std::uint32_t),
              "VFPv3-D32 is 64 single-precision words");

/// `UserConfig::code_cache_size`'s own comment gives the bounds (about 8 MiB minimum, 128 MiB maximum on
/// an arm64 host, the ceiling being how far an arm64 jump can reach). The low end is used because this is
/// a per-instance allocation in a design where a user can have several guest instances alive, not because
/// 8 MiB is enough for a big app — `RbCodeCache` (C04) is where sizing policy belongs.
constexpr std::size_t kCodeCacheBytes = 8u << 20;

/**
 * The size in bytes of the instruction at `pc`.
 *
 * The fallback hand-back has to advance *exactly one* guest encoding, and for Thumb that is decidable
 * from the first halfword: bit pattern 0b111 in bits 13-15 (with 11-12 not 0b00) means a 32-bit
 * Thumb-32 encoding. Reading the halfword here rather than re-deriving it from the IR is deliberate —
 * `InterpreterFallback` is called precisely when translation declined, so there may be no IR to consult.
 */
unsigned EncodingSizeAt(mem::GuestMemory& memory, GuestAddr pc, isa::Mode mode) {
  if (mode == isa::Mode::Arm) return 4;
  std::uint16_t hw = 0;
  if (!memory.Read(pc, &hw, 2)) return 2;  // unmapped: the caller's next read will fault and report it
  const bool wide = (hw & 0xE000u) == 0xE000u && (hw & 0x1800u) != 0;
  return wide ? 4 : 2;
}

const char* ExceptionName(Dynarmic::A32::Exception e) {
  using Dynarmic::A32::Exception;
  switch (e) {
    case Exception::UndefinedInstruction: return "undefined instruction encoding";
    case Exception::UnpredictableInstruction: return "unpredictable instruction";
    case Exception::DecodeError: return "decoder error";
    case Exception::SendEvent: return "SEV";
    case Exception::SendEventLocal: return "SEVL";
    case Exception::WaitForInterrupt: return "WFI";
    case Exception::WaitForEvent: return "WFE";
    case Exception::Yield: return "YIELD";
    case Exception::Breakpoint: return "BKPT";
    case Exception::PreloadData: return "PLD";
    case Exception::PreloadDataWithIntentToWrite: return "PLDW";
    case Exception::PreloadInstruction: return "PLI";
    case Exception::NoExecuteFault: return "instruction fetch from non-executable memory";
  }
  return "unknown exception";
}

}  // namespace

JitExecutor::JitExecutor(mem::GuestMemory& memory, GuestAddr return_address,
                         const HostDispatch* dispatch)
    : memory_{memory}, return_address_{return_address}, dispatch_{dispatch} {
  Dynarmic::A32::UserConfig conf{this};
  // ARMv7-A, not the default. `UserConfig::arch_version` ships as `v8`, which would accept encodings no
  // armeabi-v7a binary may contain and is the same trap `rb::isa::Options()` already pins to v7 — the
  // two tiers have to agree on the *architecture*, or a differential disagreement would be an
  // configuration difference wearing its clothes.
  conf.arch_version = Dynarmic::A32::ArchVersion::v7;
  // Android's guest is little-endian and CPSR.E is not reachable from an app; forcing it removes a
  // whole axis of state that neither tier can test.
  conf.always_little_endian = true;
  // A fault cannot be returned from MemoryRead32. Halting after every data access is what makes
  // "record the fault, ask the core to stop, hand back zero" safe rather than a value the guest gets to
  // use; docs/10 §8.2 named this flag for exactly this reason.
  conf.check_halt_on_memory_access = true;
  // Cycle counting is the budget mechanism, not an accounting pretence: GetTicksForCode gives one tick
  // per guest encoding, so `executed_` is a real guest-instruction count and `max_instructions` means
  // the same thing here that it claims to.
  conf.enable_cycle_counting = true;
  conf.code_cache_size = kCodeCacheBytes;

  try {
    jit_ = std::make_unique<Dynarmic::A32::Jit>(conf);
  } catch (const std::exception& e) {
    err_ = std::string{"could not build the AArch64 back end: "} + e.what();
    return;
  }
  fallback_interp_ = std::make_unique<InterpExecutor>(memory_, return_address, dispatch);
}

JitExecutor::~JitExecutor() = default;

void JitExecutor::SyncIn(const MachineState& state) {
  auto& regs = jit_->Regs();
  static_assert(sizeof(JitRegs) == 16 * sizeof(std::uint32_t), "the guest register file is 16 words");
  std::copy(std::begin(state.r), std::end(state.r), regs.begin());

  // CPSR bit 5 is the JIT's authority on ARM vs Thumb, while Tier I carries the mode on the block's
  // location descriptor. Both are written here so neither tier can be running the wrong ISA off a stale
  // bit — and SyncOut reads it back the same way, so a guest `bx` that flips the mode survives.
  std::uint32_t cpsr = state.cpsr;
  if (state.mode == isa::Mode::Thumb) {
    cpsr |= kCpsrTFlag;
  } else {
    cpsr &= ~kCpsrTFlag;
  }
  jit_->SetCpsr(cpsr);
  jit_->SetFpscr(state.fpscr);

  // ExtRegs is the VFP bank as 64 32-bit words: S<2n> is D<n>'s low half and S<2n+1> its high half,
  // which is the same little-endian pairing `interp.cpp` uses for Get/SetExtendedRegister32. Two tiers
  // with two different orderings here would show up as garbage doubles, not as an obvious failure.
  auto& ext = jit_->ExtRegs();
  static_assert(sizeof(JitExtRegs) == 64 * sizeof(std::uint32_t), "VFPv3-D32 is 64 words");
  for (int i = 0; i < 32; ++i) {
    ext[static_cast<std::size_t>(i) * 2] = static_cast<std::uint32_t>(state.d[i]);
    ext[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint32_t>(state.d[i] >> 32);
  }
  // PC last: setting it is what makes the core runnable, and nothing above depends on it.
  regs[15] = state.pc;
}

void JitExecutor::SyncOut(MachineState& state) {
  const auto& regs = jit_->Regs();
  std::copy(regs.begin(), regs.end(), std::begin(state.r));
  state.pc = regs[15];
  state.r[15] = regs[15];
  const std::uint32_t cpsr = jit_->Cpsr();
  state.cpsr = cpsr;
  state.mode = (cpsr & kCpsrTFlag) ? isa::Mode::Thumb : isa::Mode::Arm;
  // The shared derivation, not a local unpack: see ItStateFromCpsr. Tier I used to take this field from
  // the block's location descriptor and this tier from CPSR, and the two disagree at the moment
  // execution stops, which is a false finding with a real-looking number attached.
  state.itstate = ItStateFromCpsr(cpsr);
  state.fpscr = jit_->Fpscr();
  auto& ext = jit_->ExtRegs();
  for (int i = 0; i < 32; ++i) {
    state.d[i] = static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2]) |
                 (static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2 + 1]) << 32);
  }
}

RunResult JitExecutor::Run(MachineState& state, unsigned max_instructions) {
  RunResult out{};
  if (!ok()) {
    out.reason = StopReason::Internal;
    out.detail = err_;
    return out;
  }
  live_ = &state;
  budget_ = max_instructions;
  executed_ = 0;
  pending_ = Pending::None;
  pending_detail_.clear();
  pending_slot_ = 0;
  pending_svc_ = 0;
  pending_pc_ = 0;

  SyncIn(state);
  const Dynarmic::HaltReason hr = jit_->Run();
  SyncOut(state);
  if ((pending_ == Pending::Returned || pending_ == Pending::HostCall) && pending_pc_ != 0) {
    // Dynarmic advances PC across the fetch it could not read, which leaves the core pointing one
    // instruction *past* the address RetroBug stopped on. The stop address is the one that means
    // something — it is where the sentinel or the host slot lives, and where the call bridge looks to
    // resume — so the state is normalised back to it rather than to the core's arithmetic.
    state.pc = pending_pc_;
    state.r[15] = pending_pc_;
  }

  live_ = nullptr;
  return MapHalt(hr, executed_);
}

RunResult JitExecutor::MapHalt(Dynarmic::HaltReason hr, unsigned executed) {
  RunResult out{};
  out.instructions_executed = executed;
  switch (pending_) {
    case Pending::None:
      // No RetroBug-side reason was recorded, so the halt came from dynarmic's own vocabulary.
      if (HaltHas(hr, Dynarmic::HaltReason::MemoryAbort)) {
        out.reason = StopReason::Fault;
        out.detail = "guest memory access faulted inside compiled code";
      } else if (HaltHas(hr, Dynarmic::HaltReason::CacheInvalidation)) {
        out.reason = StopReason::Budget;  // self-modifying code: resumable, and worth a diagnostic
        out.detail = "code cache invalidated during execution";
      } else if (HaltHas(hr, Dynarmic::HaltReason::Step)) {
        out.reason = StopReason::Budget;
      } else {
        out.reason = StopReason::Suspended;
      }
      break;
    case Pending::Returned:
      out.reason = StopReason::Returned;
      break;
    case Pending::HostCall:
      out.reason = StopReason::HostCall;
      out.host_slot = pending_slot_;
      break;
    case Pending::Syscall:
      out.reason = StopReason::Syscall;
      out.svc_number = pending_svc_;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Fault:
      out.reason = StopReason::Fault;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Unimplemented:
      out.reason = StopReason::Unimplemented;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Suspended:
      out.reason = StopReason::Suspended;
      out.detail = std::move(pending_detail_);
      break;
  }
  return out;
}

bool JitExecutor::SurfaceNonCodeDestination(GuestAddr pc) {
  if (pc == return_address_) {
    SetPending(Pending::Returned);
    pending_pc_ = return_address_;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return true;
  }
  std::uint32_t slot = 0;
  if (dispatch_ != nullptr && dispatch_->IsHostEntry(pc, &slot)) {
    SetPending(Pending::HostCall);
    pending_slot_ = slot;
    pending_pc_ = pc;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return true;
  }
  return false;
}

/**
 * The front-end read. Returning `nullopt` is how a fault is signalled to the translator.
 *
 * This is *one* of the two places the call sentinel is recognised, and only having it here is a trap
 * that cost a cycle: the first guest return translates the block at the sentinel, but Dynarmic then
 * **caches the block it built for that address**, so every later return executes the cached
 * raise-`NoExecuteFault` block and never asks for the code again. The intercept then silently vanishes
 * after one call — observed as the first case ending `Returned` and every case after it ending `Fault`.
 * `ExceptionRaised` therefore performs the same check, and this one is for the addresses a guest can
 * only reach by fetching, such as an unmapped page.
 */
std::optional<std::uint32_t> JitExecutor::MemoryReadCode(GuestAddr vaddr) {
  if (SurfaceNonCodeDestination(vaddr)) return std::nullopt;
  std::uint32_t v = 0;
  if (!memory_.Read(vaddr, &v, 4)) return std::nullopt;
  return v;
}

std::uint8_t JitExecutor::MemoryRead8(GuestAddr vaddr) {
  std::uint8_t v = 0;
  if (!memory_.Read(vaddr, &v, 1)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint16_t JitExecutor::MemoryRead16(GuestAddr vaddr) {
  std::uint16_t v = 0;
  if (!memory_.Read(vaddr, &v, 2)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint32_t JitExecutor::MemoryRead32(GuestAddr vaddr) {
  std::uint32_t v = 0;
  if (!memory_.Read(vaddr, &v, 4)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint64_t JitExecutor::MemoryRead64(GuestAddr vaddr) {
  std::uint64_t v = 0;
  if (!memory_.Read(vaddr, &v, 8)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

void JitExecutor::MemoryWrite8(GuestAddr vaddr, std::uint8_t value) {
  if (!memory_.Write(vaddr, &value, 1)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite16(GuestAddr vaddr, std::uint16_t value) {
  if (!memory_.Write(vaddr, &value, 2)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite32(GuestAddr vaddr, std::uint32_t value) {
  if (!memory_.Write(vaddr, &value, 4)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite64(GuestAddr vaddr, std::uint64_t value) {
  if (!memory_.Write(vaddr, &value, 8)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::CallSVC(std::uint32_t swi) {
  pending_svc_ = swi;
  SetPending(Pending::Syscall, SvcDetail(swi));
  jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void JitExecutor::ExceptionRaised(GuestAddr pc, Dynarmic::A32::Exception exception) {
  const char* name = ExceptionName(exception);
  if (exception == Dynarmic::A32::Exception::NoExecuteFault) {
    // The other half of the sentinel/slot check — see MemoryReadCode for why both are needed.
    if (SurfaceNonCodeDestination(pc)) return;
    SetPending(Pending::Fault, "instruction fetch failed at " + std::to_string(pc) +
                                   " (no code could be read there)");
  } else if (exception == Dynarmic::A32::Exception::Breakpoint) {
    SetPending(Pending::Suspended, "guest breakpoint");
  } else {
    SetPending(Pending::Unimplemented,
               std::string{name} + " at " + std::to_string(pc));
  }
  jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void JitExecutor::AddTicks(std::uint64_t ticks) {
  executed_ += static_cast<unsigned>(ticks);
  if (executed_ >= budget_) {
    // The budget is dynarmic's own stop condition rather than a RetroBug outcome, so `pending_` stays
    // None and MapHalt reads the reason off the HaltReason.
    jit_->HaltExecution(Dynarmic::HaltReason::Step);
  }
}

std::uint64_t JitExecutor::GetTicksRemaining() {
  return executed_ >= budget_ ? 0 : budget_ - executed_;
}

void JitExecutor::InterpreterFallback(GuestAddr pc, std::size_t num_instructions) {
  /**
   * Dynarmic has no interpreter. Its `Term::Interpret` terminal hands the problem to the embedder
   * (`docs/30` §Step 6c), which is why Tier I exists on the JIT path as well as where no JIT is
   * allowed, and why this is a *correctness* path rather than a performance one.
   *
   * The contract is exact: "execute exactly num_instructions starting from PC". So each iteration
   * advances one real encoding's worth of PC — measured in bytes with the Thumb-32 test above, not by a
   * step budget — and any other stop reason (a host call, a fault, the sentinel) is propagated rather
   * than swallowed, because stopping inside the fallback has to reach the same place it would have
   * reached from compiled code.
   */
  if (live_ == nullptr) return;
  SyncOut(*live_);
  for (std::size_t i = 0; i < num_instructions; ++i) {
    const GuestAddr start = live_->pc;
    const GuestAddr target =
        start + EncodingSizeAt(memory_, start, live_->mode);
    while (live_->pc < target && pending_ == Pending::None) {
      const RunResult r = fallback_interp_->Run(*live_, 64);
      if (r.reason == StopReason::Returned || r.reason == StopReason::HostCall) {
        SetPending(r.reason == StopReason::Returned ? Pending::Returned : Pending::HostCall);
        pending_slot_ = r.host_slot;
        pending_pc_ = live_->pc;
        break;
      }
      if (r.reason == StopReason::Fault) {
        SetPending(Pending::Fault, r.detail);
        break;
      }
      if (r.reason == StopReason::Unimplemented) {
        SetPending(Pending::Unimplemented, r.detail);
        break;
      }
      if (r.instructions_executed == 0) break;  // no progress: never spin inside the JIT
    }
    executed_ += 1;
    if (executed_ >= budget_) break;
  }
  SyncIn(*live_);
  if (pending_ != Pending::None) {
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::Invalidate(GuestAddr from, GuestAddr to) {
  if (!ok() || to <= from) return;
  // Real work, unlike Tier I's no-op: this tier has compiled code that a guest write can invalidate, and
  // AD-7's self-modifying-code story depends on this call reaching the cache that holds it.
  jit_->InvalidateCacheRange(from, to - from);
}

}  // namespace rb::exec
