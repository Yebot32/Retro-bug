/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C01 `rb::isa` — the thin door onto the vendored ARMv7 front end.
 *
 * The vendored Dynarmic stays byte-identical to upstream (AD-14), so everything RetroBug needs to
 * say about the front end is said here instead: which architecture we claim to be, where instruction
 * bytes come from, and what one translated block consists of. Both execution tiers are consumers of
 * exactly this — Tier J hands the `ir` field to a Dynarmic back end, Tier I walks `ops`/`ir` itself —
 * which is what "two tiers over one IR" (AD-1) means in interface terms.
 *
 * Nothing here interprets anything. See docs/20 §C02 for the tier that does.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/frontend/A32/translate/translate_callbacks.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

#include "rb/mem/guest_memory.h"

namespace Dynarmic::A32 {
class IREmitter;
}  // namespace Dynarmic::A32

namespace rb::isa {

/// Which instruction set a guest address is to be decoded as. ARMv7-A codes differently in each, and
/// the choice is ours to track rather than something readable out of a word.
enum class Mode { Arm, Thumb };

/**
 * The front end's instruction fetch, served from RetroBug's guest memory.
 *
 * It records the words it was asked for, which is not decoration: a code cache can only invalidate a
 * translated block correctly if it knows which guest words the translation depended on, and that set
 * is wider than the block's own instruction range (an unaligned Thumb-2 fetch pulls in a neighbouring
 * word). Self-modifying code (AD-7) is detected against this list and nothing else can supply it.
 *
 * An unmapped fetch is reported as "no value" rather than as zero, because Dynarmic's translators
 * handle `nullopt` by ending the block — whereas a synthesised zero would be decoded as a real
 * instruction and quietly produce code for memory the guest does not have.
 */
class CodeReader : public Dynarmic::A32::TranslateCallbacks {
 public:
  CodeReader(const mem::GuestMemory& memory, Mode mode) : memory_{memory}, mode_{mode} {}

  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
  bool PreCodeReadHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
    (void)is_thumb;
    (void)pc;
    (void)ir;
    return true;
  }
  void PreCodeTranslationHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
    (void)is_thumb;
    (void)pc;
    (void)ir;
  }
  /// Cycle accounting is not part of M0's scope; anything that starts consuming this needs a real
  /// model of instruction timing behind it, not this constant.
  std::uint64_t GetTicksForCode(bool is_thumb, std::uint32_t vaddr, std::uint32_t instruction) override {
    (void)is_thumb;
    (void)vaddr;
    (void)instruction;
    return 1;
  }

  const std::vector<std::uint32_t>& code_words() const { return code_words_; }
  bool saw_unmapped_read() const { return unmapped_; }
  Mode mode() const { return mode_; }

 private:
  const mem::GuestMemory& memory_;
  Mode mode_;
  std::vector<std::uint32_t> code_words_;
  bool unmapped_ = false;
};

/// One translated basic block, in the form both tiers and the diagnostics layer want.
struct TranslatedBlock {
  /// `IR::Block` has no default constructor (it is built from a location descriptor), so this struct
  /// has to be constructed with one rather than filled in afterwards.
  explicit TranslatedBlock(Dynarmic::IR::Block block) : ir{std::move(block)} {}

  Dynarmic::IR::Block ir;
  /// `IR::GetNameOf` for each instruction, in block order. The presence half of U-8 is a question
  /// about these names, and Tier I's dispatch table is keyed by them.
  std::vector<std::string> ops;
  std::string terminal;
  std::uint32_t entry = 0;
  Mode mode = Mode::Arm;
  /// Guest word addresses the front end actually read, in the order it read them.
  std::vector<std::uint32_t> code_words;
  /// True when the block ended because code could not be fetched, rather than on a real terminal —
  /// callers must not cache such a block, and must not report it as a translated one.
  bool truncated_by_unmapped_read = false;
};

/// How the front end is told to behave. ARMv7-A is the architecture RetroBug reproduces: `v8` would
/// accept encodings no armeabi-v7a binary may legitimately contain, and `v6T2` or older would refuse
/// the Thumb-2 wide encodings `docs/32-isa-coverage.md` measured 47,894 times in real code. Mode is
/// not part of this — it travels on the block's location descriptor, because the front end reads it
/// as the CPSR.T bit.
Dynarmic::A32::TranslationOptions Options();

/// Translate the single basic block starting at `pc`. `err` is filled only on failure, and a failure
/// here is always something the caller has to know about: an unmapped page, or a front-end error.
std::optional<TranslatedBlock> TranslateOne(const mem::GuestMemory& memory, std::uint32_t pc, Mode mode,
                                            std::string* err);

/// Name of the variant a block's terminal currently holds. "unknown" means the vendored terminal list
/// gained an alternative this wrapper has not been told about, which the ISA test asserts against.
std::string TerminalName(const Dynarmic::IR::Term::Terminal& terminal);

}  // namespace rb::isa
