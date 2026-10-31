/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — the AOT runtime: block table, dispatcher, callbacks and Tier I fallback.
 *
 * No code is generated here and no executable memory is allocated. The AArch64 bytes, the prelude and
 * the callback trampolines all come from `aot_<image>.S`, emitted offline by `tools/aotgen/emit.cpp`
 * and linked into the binary. This file supplies only the C++ half the prelude branches to:
 *
 *   - `AotImage`: sorted block table and `Resolve`.
 *   - `AotExecutor`: the `run_code` caller, the `Dispatch` entry the return-to-dispatcher trampoline
 *     calls, the callback bodies behind the other trampolines, and the fallback to `InterpExecutor`.
 *
 * The callback semantics are deliberately a copy of `JitExecutor`'s `UserCallbacks` methods: same
 * fault details, same zero return, same halt bit, same first-reason-wins ordering. The AOT tier must
 * be indistinguishable from Tier J at the `MachineState` boundary, so any deliberate difference here
 * would be a bug in one of the two tiers.
 */
#include "rb/exec/aot.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

#include "rb/isa/front_end.h"

namespace rb::exec {

namespace {

constexpr std::uint32_t kCpsrTFlag = 1u << 5;

/// The prelude uses Dynarmic's own `A32JitState` offsets. `AotState` holds that struct first, so a
/// callback receives the same address as the `jit_state` the prelude was entered with, and every
/// offset the emitted blocks index through X28 is the vendored offset. If any of this stops being
/// true the code below is silently wrong — which is why it is compile-time checked here rather than
/// trusted to a comment.
static_assert(std::is_standard_layout_v<AotState>, "AotState's first-member address must be usable");
static_assert(offsetof(AotState, jit) == 0, "the prelude and every block address A32JitState at X28");
static_assert(offsetof(AotState, jit.cpsr_nzcv) == offsetof(A32JitState, cpsr_nzcv));
static_assert(offsetof(AotState, jit.cpsr_q) == offsetof(A32JitState, cpsr_q));
static_assert(offsetof(AotState, jit.cpsr_jaifm) == offsetof(A32JitState, cpsr_jaifm));
static_assert(offsetof(AotState, jit.cpsr_ge) == offsetof(A32JitState, cpsr_ge));
static_assert(offsetof(AotState, jit.fpsr) == offsetof(A32JitState, fpsr));
static_assert(offsetof(AotState, jit.fpsr_nzcv) == offsetof(A32JitState, fpsr_nzcv));
static_assert(offsetof(AotState, jit.regs) == offsetof(A32JitState, regs));
static_assert(offsetof(AotState, jit.upper_location_descriptor) ==
              offsetof(A32JitState, upper_location_descriptor));
static_assert(offsetof(AotState, jit.ext_regs) == offsetof(A32JitState, ext_regs));
static_assert(offsetof(AotState, jit.exclusive_state) == offsetof(A32JitState, exclusive_state));
static_assert(sizeof(std::declval<A32JitState>().regs) == 16 * sizeof(std::uint32_t));
static_assert(sizeof(std::declval<A32JitState>().ext_regs) == 64 * sizeof(std::uint32_t));

[[nodiscard]] constexpr bool HaltHas(Dynarmic::HaltReason got, Dynarmic::HaltReason want) {
  return (static_cast<std::uint32_t>(got) & static_cast<std::uint32_t>(want)) != 0;
}

std::string Hex(std::uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

const char* ExceptionName(std::uint32_t e) {
  using Dynarmic::A32::Exception;
  switch (static_cast<Exception>(e)) {
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

bool AotImage::Load(const AotBlock* blocks, std::size_t n, const AotImageInfo* info,
                    std::string* err) {
  const auto fail = [&](const std::string& msg) {
    if (err != nullptr) *err = msg;
    return false;
  };
  if (blocks == nullptr || n == 0) return fail("AOT image has no blocks");
  if (info == nullptr) return fail("AOT image has no generated info");
  if (info->count != n) return fail("AOT image count does not match its block array");
  if (info->run_code == nullptr) return fail("AOT image has no prelude run_code entry");
  if (info->text_len == 0) return fail("AOT image has an empty .text range");
  const std::uint64_t text_base = static_cast<std::uint64_t>(info->text_vaddr) + info->load_bias;
  if (text_base > 0xffff'ffffull) return fail("AOT text base overflows the 32-bit guest address space");

  std::vector<Row> rows;
  rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const AotBlock& block = blocks[i];
    const unsigned isa = block.flags & (kAotThumb | kAotArm);
    if (isa != kAotThumb && isa != kAotArm) {
      return fail("AOT block " + std::to_string(i) + " has no single ISA flag");
    }
    if (block.entry == nullptr) {
      return fail("AOT block " + std::to_string(i) + " has a null entry");
    }
    if (block.guest_off >= info->text_len) {
      return fail("AOT block " + std::to_string(i) + " lies outside .text");
    }
    rows.push_back(Row{block.guest_off, block.flags, block.entry});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.off != b.off) return a.off < b.off;
    return a.flags < b.flags;
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (rows[i].off == rows[i - 1].off && rows[i].flags == rows[i - 1].flags &&
        rows[i].entry != rows[i - 1].entry) {
      return fail("AOT image has two entries for one guest offset");
    }
  }
  rows.erase(std::unique(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
               return a.off == b.off && a.flags == b.flags;
             }),
             rows.end());
  rows_ = std::move(rows);
  info_ = *info;
  if (err != nullptr) err->clear();
  return true;
}

AotCode AotImage::Resolve(std::uint32_t guest_off, bool thumb) const {
  const std::uint8_t want = thumb ? kAotThumb : kAotArm;
  const auto it = std::lower_bound(rows_.begin(), rows_.end(), guest_off,
                                   [](const Row& row, std::uint32_t off) { return row.off < off; });
  for (auto cur = it; cur != rows_.end() && cur->off == guest_off; ++cur) {
    if ((cur->flags & want) != 0) return cur->entry;
  }
  return nullptr;
}

AotExecutor::AotExecutor(mem::GuestMemory& memory, GuestAddr return_address,
                         const AotImageInfo& info, const HostDispatch* dispatch)
    : memory_(memory), return_address_(return_address), dispatch_(dispatch) {
#if defined(__aarch64__)
  if (!image_.Load(info.blocks, info.count, &info, &err_)) return;
  text_base_ = info.load_bias + info.text_vaddr;
  state_.exec = this;
  fallback_interp_ = std::make_unique<InterpExecutor>(memory_, return_address_, dispatch_);
#else
  (void)info;
  err_ = "AOT execution requires an AArch64 host";
#endif
}

AotExecutor::~AotExecutor() = default;

#if defined(__aarch64__)

void AotExecutor::SyncIn(const MachineState& state) {
  state_.jit.exclusive_state = 0;
  monitor_.ClearProcessor(0);

  auto& regs = state_.jit.regs;
  std::copy(std::begin(state.r), std::end(state.r), regs.begin());

  std::uint32_t cpsr = state.cpsr;
  if (state.mode == isa::Mode::Thumb) {
    cpsr |= kCpsrTFlag;
  } else {
    cpsr &= ~kCpsrTFlag;
  }
  state_.jit.SetCpsr(cpsr);
  state_.jit.SetFpscr(state.fpscr);

  auto& ext = state_.jit.ext_regs;
  for (int i = 0; i < 32; ++i) {
    ext[static_cast<std::size_t>(i) * 2] = static_cast<std::uint32_t>(state.d[i]);
    ext[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint32_t>(state.d[i] >> 32);
  }
  regs[15] = state.pc;
}

void AotExecutor::SyncOut(MachineState& state) {
  const auto& regs = state_.jit.regs;
  std::copy(regs.begin(), regs.end(), std::begin(state.r));
  state.pc = regs[15];
  state.r[15] = regs[15];
  const std::uint32_t cpsr = state_.jit.Cpsr();
  state.cpsr = cpsr;
  state.mode = (cpsr & kCpsrTFlag) ? isa::Mode::Thumb : isa::Mode::Arm;
  state.itstate = ItStateFromCpsr(cpsr);
  state.fpscr = state_.jit.Fpscr();
  const auto& ext = state_.jit.ext_regs;
  for (int i = 0; i < 32; ++i) {
    state.d[i] = static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2]) |
                 (static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2 + 1]) << 32);
  }
}

void AotExecutor::Halt(Dynarmic::HaltReason hr) {
  __atomic_fetch_or(&state_.halt_reason, static_cast<std::uint32_t>(hr), __ATOMIC_RELEASE);
}

void AotExecutor::SetPending(Pending p, std::string detail) {
  if (pending_ != Pending::None) return;
  pending_ = p;
  if (!detail.empty()) pending_detail_ = std::move(detail);
}

void AotExecutor::SetFault(std::string detail, GuestAddr addr, bool write) {
  if (pending_ != Pending::None) return;
  pending_ = Pending::Fault;
  pending_detail_ = std::move(detail);
  pending_fault_addr_ = addr;
  pending_fault_has_addr_ = true;
  pending_fault_write_ = write;
}

void* AotExecutor::Dispatch() {
  const std::uint32_t pc = state_.jit.regs[15];
  const bool thumb = (state_.jit.upper_location_descriptor & 1u) != 0;

  if (pc == return_address_) {
    SetPending(Pending::Returned);
    pending_pc_ = pc;
    Halt(Dynarmic::HaltReason::UserDefined1);
    return nullptr;
  }
  std::uint32_t slot = 0;
  if (dispatch_ != nullptr && dispatch_->IsHostEntry(pc, &slot)) {
    SetPending(Pending::HostCall);
    pending_slot_ = slot;
    pending_pc_ = pc;
    Halt(Dynarmic::HaltReason::UserDefined1);
    return nullptr;
  }
  AotCode entry = nullptr;
  if (pc >= text_base_ && pc - text_base_ < image_.info().text_len) {
    entry = image_.Resolve(pc - text_base_, thumb);
  }
  if (entry == nullptr) {
    state_.fallback_pc = pc;
    state_.fallback_pending = 1;
    Halt(Dynarmic::HaltReason::UserDefined1);
    return nullptr;
  }
  return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(entry));
}

RunResult AotExecutor::Run(MachineState& state, unsigned max_instructions) {
  RunResult out{};
  if (!ok()) {
    out.reason = StopReason::Internal;
    out.detail = err_;
    return out;
  }
  if (max_instructions == 0) {
    SyncIn(state);
    SyncOut(state);
    out.reason = StopReason::Budget;
    out.detail = "a zero-instruction budget runs nothing";
    return out;
  }

  live_ = &state;
  budget_ = max_instructions;
  executed_ = 0;
  fallback_steps_ = 0;
  pending_ = Pending::None;
  pending_detail_.clear();
  pending_slot_ = 0;
  pending_svc_ = 0;
  pending_pc_ = 0;
  pending_fault_addr_ = 0;
  pending_fault_has_addr_ = false;
  pending_fault_write_ = false;
  pending_illegal_ = false;
  pending_illegal_pc_ = 0;
  state_.halt_reason = 0;
  state_.fallback_pc = 0;
  state_.fallback_pending = 0;

  SyncIn(state);

  for (;;) {
    state_.halt_reason = 0;
    void* entry = Dispatch();
    if (entry != nullptr) {
      const Dynarmic::HaltReason hr =
          image_.info().run_code(entry, &state_.jit, &state_.halt_reason);
      SyncOut(state);
      if (pending_pc_ != 0 && pending_ != Pending::Syscall && pending_ != Pending::None) {
        state.pc = pending_pc_;
        state.r[15] = pending_pc_;
      }
      if (state_.fallback_pending == 0) {
        live_ = nullptr;
        return MapHalt(hr, executed_ + fallback_steps_);
      }
      state_.fallback_pending = 0;
      ++fallbacks_;
    } else if (state_.fallback_pending != 0) {
      state_.fallback_pending = 0;
      ++fallbacks_;
      SyncOut(state);
    } else {
      SyncOut(state);
      if (pending_pc_ != 0) {
        state.pc = pending_pc_;
        state.r[15] = pending_pc_;
      }
      live_ = nullptr;
      return MapHalt(Dynarmic::HaltReason::UserDefined1, executed_ + fallback_steps_);
    }

    if (executed_ >= budget_) {
      live_ = nullptr;
      out.reason = StopReason::Budget;
      out.instructions_executed = executed_ + fallback_steps_;
      return out;
    }
    // The dispatcher's miss is executed on Tier I, then AOT resumes wherever Tier I stopped. The
    // budget unit differs (guest instructions here, IR steps there), so the fallback gets a fixed
    // chunk and its count is reported separately; a run with zero fallbacks never enters this path.
    const RunResult r = fallback_interp_->Run(state, 1u << 16);
    fallback_steps_ += r.instructions_executed;
    if (r.reason != StopReason::Budget) {
      live_ = nullptr;
      return r;
    }
    SyncIn(state);
  }
}

RunResult AotExecutor::MapHalt(Dynarmic::HaltReason hr, unsigned executed) {
  RunResult out{};
  out.instructions_executed = executed;
  switch (pending_) {
    case Pending::None:
      if (HaltHas(hr, Dynarmic::HaltReason::MemoryAbort)) {
        out.reason = StopReason::Fault;
        out.detail = "guest memory access faulted inside compiled code";
      } else if (HaltHas(hr, Dynarmic::HaltReason::CacheInvalidation)) {
        out.reason = StopReason::Budget;
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
      out.fault_addr = pending_fault_addr_;
      out.fault_has_addr = pending_fault_has_addr_;
      out.fault_write = pending_fault_write_;
      break;
    case Pending::Unimplemented:
      if (pending_illegal_) {
        out.reason = StopReason::Fault;
        out.detail = std::move(pending_detail_);
        out.illegal_instruction = true;
        out.illegal_pc = pending_illegal_pc_;
      } else {
        out.reason = StopReason::Unimplemented;
        out.detail = std::move(pending_detail_);
      }
      break;
    case Pending::Suspended:
      out.reason = StopReason::Suspended;
      out.detail = std::move(pending_detail_);
      break;
  }
  return out;
}

void AotExecutor::Invalidate(GuestAddr from, GuestAddr to) {
  // The AOT image is immutable. A guest write into translated text is a self-modifying-code case the
  // plan handles with a cache-flush check (Task 6); until then the page table's write rules decide
  // whether the write was legal at all, and this hook has no cache to drop.
  (void)from;
  (void)to;
}

std::uint8_t AotExecutor::MemoryRead8(std::uint32_t vaddr) {
  std::uint8_t v = 0;
  if (!memory_.Read(vaddr, &v, 1)) {
    SetFault("guest memory read at 0x" + Hex(vaddr) + " is unmapped or not readable", vaddr, false);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
  return v;
}

std::uint16_t AotExecutor::MemoryRead16(std::uint32_t vaddr) {
  std::uint16_t v = 0;
  if (!memory_.Read(vaddr, &v, 2)) {
    SetFault("guest memory read at 0x" + Hex(vaddr) + " is unmapped or not readable", vaddr, false);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
  return v;
}

std::uint32_t AotExecutor::MemoryRead32(std::uint32_t vaddr) {
  std::uint32_t v = 0;
  if (!memory_.Read(vaddr, &v, 4)) {
    SetFault("guest memory read at 0x" + Hex(vaddr) + " is unmapped or not readable", vaddr, false);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
  return v;
}

std::uint64_t AotExecutor::MemoryRead64(std::uint32_t vaddr) {
  std::uint64_t v = 0;
  if (!memory_.Read(vaddr, &v, 8)) {
    SetFault("guest memory read at 0x" + Hex(vaddr) + " is unmapped or not readable", vaddr, false);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
  return v;
}

void AotExecutor::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
  if (!memory_.Write(vaddr, &value, 1)) {
    SetFault("guest memory write at 0x" + Hex(vaddr) +
                 " is unmapped, read-only, or an executing page",
             vaddr, true);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
}

void AotExecutor::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
  if (!memory_.Write(vaddr, &value, 2)) {
    SetFault("guest memory write at 0x" + Hex(vaddr) +
                 " is unmapped, read-only, or an executing page",
             vaddr, true);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
}

void AotExecutor::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
  if (!memory_.Write(vaddr, &value, 4)) {
    SetFault("guest memory write at 0x" + Hex(vaddr) +
                 " is unmapped, read-only, or an executing page",
             vaddr, true);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
}

void AotExecutor::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
  if (!memory_.Write(vaddr, &value, 8)) {
    SetFault("guest memory write at 0x" + Hex(vaddr) +
                 " is unmapped, read-only, or an executing page",
             vaddr, true);
    Halt(Dynarmic::HaltReason::MemoryAbort);
  }
}

std::uint8_t AotExecutor::ExclusiveRead8(std::uint32_t vaddr) {
  return monitor_.ReadAndMark<std::uint8_t>(0, vaddr, [&] { return MemoryRead8(vaddr); });
}
std::uint16_t AotExecutor::ExclusiveRead16(std::uint32_t vaddr) {
  return monitor_.ReadAndMark<std::uint16_t>(0, vaddr, [&] { return MemoryRead16(vaddr); });
}
std::uint32_t AotExecutor::ExclusiveRead32(std::uint32_t vaddr) {
  return monitor_.ReadAndMark<std::uint32_t>(0, vaddr, [&] { return MemoryRead32(vaddr); });
}
std::uint64_t AotExecutor::ExclusiveRead64(std::uint32_t vaddr) {
  return monitor_.ReadAndMark<std::uint64_t>(0, vaddr, [&] { return MemoryRead64(vaddr); });
}

bool AotExecutor::ExclusiveCompareAndStore(std::uint32_t vaddr, std::uint64_t value,
                                           std::uint64_t expected, std::size_t bytes) {
  unsigned char raw[8] = {};
  if (!memory_.Read(vaddr, raw, bytes)) {
    SetFault("guest exclusive access at 0x" + Hex(vaddr) + " is unmapped or not readable", vaddr,
             false);
    Halt(Dynarmic::HaltReason::MemoryAbort);
    return false;
  }
  std::uint64_t current = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    current |= static_cast<std::uint64_t>(raw[i]) << (i * 8);
  }
  if (current != expected) return false;
  for (std::size_t i = 0; i < bytes; ++i) {
    raw[i] = static_cast<unsigned char>(value >> (i * 8));
  }
  if (!memory_.Write(vaddr, raw, bytes)) {
    SetFault("guest exclusive write at 0x" + Hex(vaddr) +
                 " is unmapped, read-only, or an executing page",
             vaddr, true);
    Halt(Dynarmic::HaltReason::MemoryAbort);
    return false;
  }
  return true;
}

std::uint32_t AotExecutor::ExclusiveWrite8(std::uint32_t vaddr, std::uint8_t value) {
  const bool ok = monitor_.DoExclusiveOperation<std::uint8_t>(0, vaddr, [&](std::uint8_t expected) {
    return ExclusiveCompareAndStore(vaddr, value, expected, 1);
  });
  return ok ? 0u : 1u;
}
std::uint32_t AotExecutor::ExclusiveWrite16(std::uint32_t vaddr, std::uint16_t value) {
  const bool ok = monitor_.DoExclusiveOperation<std::uint16_t>(0, vaddr, [&](std::uint16_t expected) {
    return ExclusiveCompareAndStore(vaddr, value, expected, 2);
  });
  return ok ? 0u : 1u;
}
std::uint32_t AotExecutor::ExclusiveWrite32(std::uint32_t vaddr, std::uint32_t value) {
  const bool ok = monitor_.DoExclusiveOperation<std::uint32_t>(0, vaddr, [&](std::uint32_t expected) {
    return ExclusiveCompareAndStore(vaddr, value, expected, 4);
  });
  return ok ? 0u : 1u;
}
std::uint32_t AotExecutor::ExclusiveWrite64(std::uint32_t vaddr, std::uint64_t value) {
  const bool ok = monitor_.DoExclusiveOperation<std::uint64_t>(0, vaddr, [&](std::uint64_t expected) {
    return ExclusiveCompareAndStore(vaddr, value, expected, 8);
  });
  return ok ? 0u : 1u;
}

void AotExecutor::CallSVC(std::uint32_t swi) {
  pending_svc_ = swi;
  SetPending(Pending::Syscall, SvcDetail(swi));
  Halt(Dynarmic::HaltReason::UserDefined1);
}

void AotExecutor::ExceptionRaised(std::uint32_t pc, std::uint32_t exception) {
  const auto e = static_cast<Dynarmic::A32::Exception>(exception);
  if (isa::HintIsNoOp(e)) return;
  const char* name = ExceptionName(exception);
  if (e == Dynarmic::A32::Exception::NoExecuteFault) {
    SetFault("instruction fetch failed at 0x" + Hex(pc) + " (no code could be read there)", pc,
             false);
  } else if (e == Dynarmic::A32::Exception::Breakpoint) {
    SetPending(Pending::Suspended, "guest breakpoint");
  } else {
    if (e == Dynarmic::A32::Exception::UndefinedInstruction ||
        e == Dynarmic::A32::Exception::UnpredictableInstruction) {
      if (pending_ == Pending::None) {
        pending_illegal_ = true;
        pending_illegal_pc_ = pc;
      }
    }
    SetPending(Pending::Unimplemented, std::string{name} + " at 0x" + Hex(pc));
  }
  Halt(Dynarmic::HaltReason::UserDefined1);
}

void AotExecutor::AddTicks(std::uint64_t ticks) {
  executed_ += static_cast<unsigned>(ticks);
  if (executed_ >= budget_) {
    Halt(Dynarmic::HaltReason::Step);
  }
}

std::uint64_t AotExecutor::GetTicksRemaining() {
  return executed_ >= budget_ ? 0 : budget_ - executed_;
}

// The prelude's trampolines load X0 with Xstate (an `AotState*`, which is also `&state->jit`) and
// tail-branch here. The block has already placed the remaining arguments in X1/X2; C linkage means
// nothing else has to be moved, exactly like the devirtualized member calls the JIT's prelude makes.
extern "C" {

std::uint8_t rb_aot_mem_read8(AotState* state, std::uint32_t vaddr) {
  return state->exec->MemoryRead8(vaddr);
}
std::uint16_t rb_aot_mem_read16(AotState* state, std::uint32_t vaddr) {
  return state->exec->MemoryRead16(vaddr);
}
std::uint32_t rb_aot_mem_read32(AotState* state, std::uint32_t vaddr) {
  return state->exec->MemoryRead32(vaddr);
}
std::uint64_t rb_aot_mem_read64(AotState* state, std::uint32_t vaddr) {
  return state->exec->MemoryRead64(vaddr);
}
void rb_aot_mem_write8(AotState* state, std::uint32_t vaddr, std::uint8_t value) {
  state->exec->MemoryWrite8(vaddr, value);
}
void rb_aot_mem_write16(AotState* state, std::uint32_t vaddr, std::uint16_t value) {
  state->exec->MemoryWrite16(vaddr, value);
}
void rb_aot_mem_write32(AotState* state, std::uint32_t vaddr, std::uint32_t value) {
  state->exec->MemoryWrite32(vaddr, value);
}
void rb_aot_mem_write64(AotState* state, std::uint32_t vaddr, std::uint64_t value) {
  state->exec->MemoryWrite64(vaddr, value);
}

std::uint8_t rb_aot_excl_read8(AotState* state, std::uint32_t vaddr) {
  return state->exec->ExclusiveRead8(vaddr);
}
std::uint16_t rb_aot_excl_read16(AotState* state, std::uint32_t vaddr) {
  return state->exec->ExclusiveRead16(vaddr);
}
std::uint32_t rb_aot_excl_read32(AotState* state, std::uint32_t vaddr) {
  return state->exec->ExclusiveRead32(vaddr);
}
std::uint64_t rb_aot_excl_read64(AotState* state, std::uint32_t vaddr) {
  return state->exec->ExclusiveRead64(vaddr);
}
std::uint32_t rb_aot_excl_write8(AotState* state, std::uint32_t vaddr, std::uint32_t value) {
  return state->exec->ExclusiveWrite8(vaddr, static_cast<std::uint8_t>(value));
}
std::uint32_t rb_aot_excl_write16(AotState* state, std::uint32_t vaddr, std::uint32_t value) {
  return state->exec->ExclusiveWrite16(vaddr, static_cast<std::uint16_t>(value));
}
std::uint32_t rb_aot_excl_write32(AotState* state, std::uint32_t vaddr, std::uint32_t value) {
  return state->exec->ExclusiveWrite32(vaddr, value);
}
std::uint32_t rb_aot_excl_write64(AotState* state, std::uint32_t vaddr, std::uint64_t value) {
  return state->exec->ExclusiveWrite64(vaddr, value);
}

void rb_aot_call_svc(AotState* state, std::uint32_t swi) {
  state->exec->CallSVC(swi);
}
void rb_aot_exception_raised(AotState* state, std::uint32_t pc, std::uint32_t exception) {
  state->exec->ExceptionRaised(pc, exception);
}
void rb_aot_isb_raised(AotState*) {}
void rb_aot_add_ticks(AotState* state, std::uint64_t ticks) {
  state->exec->AddTicks(ticks);
}
std::uint64_t rb_aot_get_ticks_remaining(AotState* state) {
  return state->exec->GetTicksRemaining();
}
void* rb_aot_dispatch(AotState* state) {
  return state->exec->Dispatch();
}

}  // extern "C"

#else  // !__aarch64__

RunResult AotExecutor::Run(MachineState&, unsigned) {
  RunResult out{};
  out.reason = StopReason::Internal;
  out.detail = err_;
  return out;
}

void AotExecutor::Invalidate(GuestAddr, GuestAddr) {}

void* AotExecutor::Dispatch() { return nullptr; }

void AotExecutor::SyncIn(const MachineState&) {}
void AotExecutor::SyncOut(MachineState&) {}

RunResult AotExecutor::MapHalt(Dynarmic::HaltReason, unsigned) {
  RunResult out{};
  out.reason = StopReason::Internal;
  out.detail = err_;
  return out;
}

void AotExecutor::Halt(Dynarmic::HaltReason) {}
void AotExecutor::SetPending(Pending, std::string) {}
void AotExecutor::SetFault(std::string, GuestAddr, bool) {}
bool AotExecutor::ExclusiveCompareAndStore(std::uint32_t, std::uint64_t, std::uint64_t,
                                           std::size_t) {
  return false;
}

std::uint8_t AotExecutor::MemoryRead8(std::uint32_t) { return 0; }
std::uint16_t AotExecutor::MemoryRead16(std::uint32_t) { return 0; }
std::uint32_t AotExecutor::MemoryRead32(std::uint32_t) { return 0; }
std::uint64_t AotExecutor::MemoryRead64(std::uint32_t) { return 0; }
void AotExecutor::MemoryWrite8(std::uint32_t, std::uint8_t) {}
void AotExecutor::MemoryWrite16(std::uint32_t, std::uint16_t) {}
void AotExecutor::MemoryWrite32(std::uint32_t, std::uint32_t) {}
void AotExecutor::MemoryWrite64(std::uint32_t, std::uint64_t) {}
std::uint8_t AotExecutor::ExclusiveRead8(std::uint32_t) { return 0; }
std::uint16_t AotExecutor::ExclusiveRead16(std::uint32_t) { return 0; }
std::uint32_t AotExecutor::ExclusiveRead32(std::uint32_t) { return 0; }
std::uint64_t AotExecutor::ExclusiveRead64(std::uint32_t) { return 0; }
std::uint32_t AotExecutor::ExclusiveWrite8(std::uint32_t, std::uint8_t) { return 1; }
std::uint32_t AotExecutor::ExclusiveWrite16(std::uint32_t, std::uint16_t) { return 1; }
std::uint32_t AotExecutor::ExclusiveWrite32(std::uint32_t, std::uint32_t) { return 1; }
std::uint32_t AotExecutor::ExclusiveWrite64(std::uint32_t, std::uint64_t) { return 1; }
void AotExecutor::CallSVC(std::uint32_t) {}
void AotExecutor::ExceptionRaised(std::uint32_t, std::uint32_t) {}
void AotExecutor::AddTicks(std::uint64_t) {}
std::uint64_t AotExecutor::GetTicksRemaining() { return 0; }

#endif  // __aarch64__

}  // namespace rb::exec
