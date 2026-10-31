/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/isa/front_end.h"

#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/FPSCR.h"
#include "dynarmic/frontend/A32/PSR.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

#include <array>
#include <atomic>

namespace rb::isa {
namespace {
std::atomic<std::uint32_t> g_translation_pc{0};
std::atomic<bool> g_translation_thumb{false};
std::atomic<std::uint8_t> g_translation_it{0};
}  // namespace

TranslationSite LastTranslationSite() noexcept {
  return TranslationSite{g_translation_pc.load(std::memory_order_relaxed),
                         g_translation_thumb.load(std::memory_order_relaxed),
                         g_translation_it.load(std::memory_order_relaxed)};
}

namespace {

using Dynarmic::A32::IREmitter;
using Dynarmic::A32::LocationDescriptor;
using Dynarmic::A32::TranslationOptions;
namespace Term = Dynarmic::IR::Term;

/// `Term::Terminal`'s alternatives in the order the vendored ir/terminal.h declares them, named by
/// index because that is all the variant offers without reaching through the Boost shim — which
/// external/dynarmic owns and core/ does not include. The ISA test fails if a real block ever
/// reports a terminal outside this table, so a reordering upstream cannot pass silently.
constexpr std::array<const char*, 10> kTerminalNames = {
    "Invalid",   "Interpret", "ReturnToDispatch", "LinkBlock", "LinkBlockFast",
    "PopRSBHint", "FastDispatchHint", "If",       "CheckBit",  "CheckHalt",
};

}  // namespace

std::optional<std::uint32_t> CodeReader::MemoryReadCode(std::uint32_t vaddr) {
  std::uint32_t word = 0;
  if (!memory_.Read(vaddr, &word, 4)) {
    unmapped_ = true;
    return std::nullopt;
  }
  // Only successful fetches are recorded. What is recorded matters: a code cache invalidates a
  // translated block when the guest writes to one of these words, and an unaligned Thumb-2 fetch
  // puts a neighbouring word in the list that the block's own address range would have missed.
  for (const std::uint32_t seen : code_words_) {
    if (seen == vaddr) return word;
  }
  code_words_.push_back(vaddr);
  return word;
}

TranslationOptions Options() {
  TranslationOptions opts;
  // v7, deliberately — and this comment is where "just widen it to v8" gets answered. The guest set
  // does contain v8 encodings (the SM-X710's bionic runs `ldab` in its arc4random fallback, measured
  // 2026-10-09), but the decoder *tables* do not consult this version at all: what stops a v8 encoding
  // is a missing row, and each one the corpus actually reaches is added as a captured patch
  // (`patches/dynarmic/0001` LDAEX, `0005` LDA/LDAB/LDAH + stores) rather than by accepting the whole
  // v8 architecture. Widening to v8 was tried against this exact wall and changed *nothing* — the
  // missing rows were the wall — while silently accepting every other v8-only encoding with no
  // recorded decision. The pin stays v7; the same answer has to be given in `jit.cpp`, or a
  // differential disagreement becomes a configuration difference wearing its clothes.
  opts.arch_version = Dynarmic::A32::ArchVersion::v7;
  // ARMv7-A leaves some encodings unpredictable rather than undefined, and a real core still does
  // *something* instead of faulting. RetroBug exists to keep legacy code running, so it takes the
  // defined behaviour Dynarmic offers; the alternative raises ExceptionRaised and kills an app that
  // hardware would have run.
  opts.define_unpredictable_behaviour = true;
  // BKPT and friends have to reach RetroBug rather than become NOPs: guest debug traps and the
  // cache-maintenance calls `__ARM_NR_cacheflush` arrives through land here.
  opts.hook_hint_instructions = true;
  return opts;
}

bool HintIsNoOp(Dynarmic::A32::Exception exception) {
  using Dynarmic::A32::Exception;
  switch (exception) {
    // The three preloads. `pld`/`pldw` are what bionic's selected `memcpy`/`memmove` uses (`pld` x10 in
    // `memmove_a15`), and they are the reason this function exists.
    //
    // `PreloadInstruction` is reachable from Thumb only: Dynarmic's ARM decoder has no `pli` entry, so an
    // ARM-state `pli` word decodes as `arm_LDC` and becomes a coprocessor load (`Exception::Preload*` has
    // exactly one producer for the instruction, `thumb32_load_byte.cpp`). It is listed because the
    // exception is the same one; a guest `pli` in ARM state is refused today by both tiers, which is a
    // named gap in `docs/30` §Step 37 rather than a silent one.
    case Exception::PreloadData:
    case Exception::PreloadDataWithIntentToWrite:
    case Exception::PreloadInstruction:
    // `yield` is a scheduling hint; `sev`/`sevl` set an event register nothing in a single-threaded guest
    // can observe, and RetroBug refuses `wfe` rather than waiting on it, so there is no waiter to mislead.
    case Exception::Yield:
    case Exception::SendEvent:
    case Exception::SendEventLocal:
      return true;
    // `wfi`/`wfe` wait. Continuing past one is a host spin loop with no wake source, and halting is the
    // answer that names itself, so they stay out of the no-op set on purpose.
    case Exception::WaitForInterrupt:
    case Exception::WaitForEvent:
    // `bkpt` has to reach the caller, and `UndefinedInstruction`/`UnpredictableInstruction`/`DecodeError`
    // and the no-execute fault are all real stops.
    case Exception::Breakpoint:
    case Exception::UndefinedInstruction:
    case Exception::UnpredictableInstruction:
    case Exception::DecodeError:
    case Exception::NoExecuteFault:
      return false;
  }
  return false;  // an exception number from a Dynarmic this file has not read: stop, naming the number
}

std::string TerminalName(const Term::Terminal& terminal) {
  const std::size_t index = terminal.which();
  return index < kTerminalNames.size() ? kTerminalNames[index] : "unknown";
}

std::optional<TranslatedBlock> TranslateOne(const mem::GuestMemory& memory,
                                            const LocationDescriptor& location, std::string* err) {
  const auto fail = [&](const std::string& msg) -> std::optional<TranslatedBlock> {
    if (err) *err = msg;
    return std::nullopt;
  };
  const bool thumb = location.TFlag();
  g_translation_pc.store(location.PC(), std::memory_order_relaxed);
  g_translation_thumb.store(thumb, std::memory_order_relaxed);
  g_translation_it.store(location.IT().Value(), std::memory_order_relaxed);
  // `location` is a byte address plus a mode, not a tagged symbol value: an odd address is the ABI's
  // Thumb tag left in place by a caller that should have split it off (rb::loader's CodeAddress does),
  // and a halfword address under ARM decoding is a genuine misalignment.
  if (thumb ? (location.PC() & 1u) != 0 : (location.PC() & 3u) != 0) {
    return fail("guest code address is misaligned for the requested mode");
  }

  CodeReader reader{memory, thumb ? Mode::Thumb : Mode::Arm};
  TranslatedBlock block{Dynarmic::A32::Translate(location, &reader, Options())};
  block.ops.reserve(block.ir.size());
  for (const auto& inst : block.ir) block.ops.push_back(Dynarmic::IR::GetNameOf(inst.GetOpcode()));
  // A dense ordinal per instruction (1..N). Tier I's value slots are indexed by it: the interpreter used
  // a hash map keyed by instruction pointer for every operand read and every result write, and this field
  // is already the dense index those maps were hashing for. Dynarmic's JIT fills the same field from its
  // naming pass; the front-end path used here never runs that pass, so without this every block would run
  // unnamed.
  unsigned ordinal = 1;
  for (auto& inst : block.ir) inst.SetName(ordinal++);
  block.terminal = TerminalName(block.ir.GetTerminal());
  block.entry = location.PC();
  block.mode = thumb ? Mode::Thumb : Mode::Arm;
  block.itstate = location.IT().Value();
  block.code_words = reader.code_words();
  block.truncated_by_unmapped_read = reader.saw_unmapped_read();
  if (block.ir.empty() && block.truncated_by_unmapped_read) {
    return fail("no instruction could be fetched at " + std::to_string(location.PC()));
  }
  return block;
}

std::optional<TranslatedBlock> TranslateOne(const mem::GuestMemory& memory, std::uint32_t pc, Mode mode,
                                           std::string* err) {
  const LocationDescriptor descriptor{pc, Dynarmic::A32::PSR{}, Dynarmic::A32::FPSCR{}, false};
  return TranslateOne(memory, descriptor.SetTFlag(mode == Mode::Thumb), err);
}

}  // namespace rb::isa
