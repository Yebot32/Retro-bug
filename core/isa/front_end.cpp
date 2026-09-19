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

namespace rb::isa {
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
