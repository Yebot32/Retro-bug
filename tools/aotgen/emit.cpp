// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The offline emitter (AOT task 3): Dynarmic's own A32 front end and AArch64 back end, driven from a
// tool, writing one assembly file.
//
// Why assembly text and not bytes: the blocks are position-independent instructions with two kinds of
// fixups (branches between blocks and calls into the C++ runtime), and the prelude is Dynarmic's
// `EmitPrelude` logic with the page table/fastmem setup removed and the dispatcher call replacing
// `GetOrEmit`. Emitting `.word` lines plus symbolic branches lets the assembler and linker resolve the
// final addresses; the tool bakes no host address into anything it writes, so the result is a normal
// object file the app can link, not an executable blob.
//
// Everything here is offline. The shipping runtime (`core/exec/aot.cpp`) contains no emitter and does
// not allocate executable memory.
#include "emit.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a32_address_space.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/frontend/A32/FPSCR.h"
#include "dynarmic/frontend/A32/PSR.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

#include "code_map.h"

namespace rb::aotgen {
namespace {

using namespace Dynarmic;
namespace A32 = Dynarmic::A32;
namespace Backend = Dynarmic::Backend::Arm64;
namespace oak = oaknut;
namespace util = oaknut::util;

/// Enough for the fixture several times over. The buffer is the tool's own scratch space, not an
/// executable allocation: it is written and then turned into text.
constexpr std::size_t kCodeBytes = 64u << 20;

/// A block is emitted here first so a relocation the AOT prelude cannot serve refuses that block, not
/// the whole image. Same ceiling as the main stream: the tool never trusts a single block to be small.
constexpr std::size_t kProbeBytes = kCodeBytes;

/// The `IR::Terminal` variant's index for `Interpret` (see `ir/terminal.h` and front_end.cpp's table).
constexpr std::size_t kTerminalInterpret = 1;

std::string Hex32(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", v);
  return buf;
}

std::string Hex64(std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::uint64_t Fnv1a64(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t h = 0xcbf2'9ce4'8422'2325ull;
  for (const std::uint8_t b : bytes) {
    h ^= b;
    h *= 0x0000'0100'0000'01b3ull;
  }
  return h;
}

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string SymbolPrefix(const std::string& out_prefix) {
  std::string s = BaseName(out_prefix);
  for (char& c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) c = '_';
  }
  if (s.empty() || (s[0] >= '0' && s[0] <= '9')) s = "rb_" + s;
  return s;
}

std::string TableName(const std::string& out_prefix) {
  std::string s = BaseName(out_prefix);
  if (s.rfind("aot_", 0) == 0) s = s.substr(4);
  std::string camel;
  bool up = true;
  for (const char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      camel += up ? static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c) : c;
      up = false;
    } else {
      up = true;
    }
  }
  if (camel.empty()) camel = "Image";
  return "kAot" + camel + "Image";
}

/// The translator's memory view: `.text` is the only bytes it can fetch, and constant-data reads
/// outside it answer zero. That is enough for `A32AddressSpace::GenerateIR`, whose constant-memory
/// pass asks only when a callback claims read-only memory (this one never does).
class ElfCallbacks final : public A32::UserCallbacks {
 public:
  ElfCallbacks(const std::vector<std::uint8_t>& elf, const CodeMap& map, std::uint32_t load_bias)
      : elf_(elf),
        text_off_(map.text_off),
        text_len_(map.text_len),
        text_base_(load_bias + map.text_vaddr) {}

  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override {
    std::uint32_t v = 0;
    if (!Read(vaddr, &v, 4)) return std::nullopt;
    return v;
  }
  std::uint8_t MemoryRead8(std::uint32_t vaddr) override {
    std::uint8_t v = 0;
    Read(vaddr, &v, 1);
    return v;
  }
  std::uint16_t MemoryRead16(std::uint32_t vaddr) override {
    std::uint16_t v = 0;
    Read(vaddr, &v, 2);
    return v;
  }
  std::uint32_t MemoryRead32(std::uint32_t vaddr) override {
    std::uint32_t v = 0;
    Read(vaddr, &v, 4);
    return v;
  }
  std::uint64_t MemoryRead64(std::uint32_t vaddr) override {
    std::uint64_t v = 0;
    Read(vaddr, &v, 8);
    return v;
  }
  void MemoryWrite8(std::uint32_t, std::uint8_t) override {}
  void MemoryWrite16(std::uint32_t, std::uint16_t) override {}
  void MemoryWrite32(std::uint32_t, std::uint32_t) override {}
  void MemoryWrite64(std::uint32_t, std::uint64_t) override {}
  void InterpreterFallback(std::uint32_t, std::size_t) override {}
  void CallSVC(std::uint32_t) override {}
  void ExceptionRaised(std::uint32_t, A32::Exception) override {}
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1; }

  /// The bytes of the translated block, for the coprocessor refusal scan.
  bool ReadBytes(std::uint32_t vaddr, void* out, std::size_t n) const { return Read(vaddr, out, n); }
  [[nodiscard]] std::uint32_t text_base() const { return text_base_; }
  [[nodiscard]] std::uint32_t text_len() const { return text_len_; }

 private:
  bool Read(std::uint32_t vaddr, void* out, std::size_t n) const {
    if (vaddr < text_base_) return false;
    const std::uint64_t off = static_cast<std::uint64_t>(vaddr - text_base_);
    if (off + n > text_len_) return false;
    std::memcpy(out, elf_.data() + text_off_ + off, n);
    return true;
  }

  const std::vector<std::uint8_t>& elf_;
  std::size_t text_off_;
  std::size_t text_len_;
  std::uint32_t text_base_;
};

struct ExternalFixup {
  std::ptrdiff_t offset;
  std::string line;  ///< e.g. "b rb_aot_mem_read32"; exactly one 4-byte instruction
};

struct TextFixup {
  std::ptrdiff_t offset;
  std::vector<std::string> lines;  ///< one 4-byte instruction, or ADRP+ADD (8 bytes)
};

enum class Refusal {
  None,
  Coprocessor,
  VectorFallback,
  Interpret,
  OutOfRange,
  LocationDescriptor,
  UnsupportedRelocation,
};

const char* RefusalName(Refusal r) {
  switch (r) {
    case Refusal::None: return "none";
    case Refusal::Coprocessor: return "coprocessor";
    case Refusal::VectorFallback: return "vector-fp fallback";
    case Refusal::Interpret: return "untranslated encoding";
    case Refusal::OutOfRange: return "outside .text";
    case Refusal::LocationDescriptor: return "non-default IT/FPSCR descriptor";
    case Refusal::UnsupportedRelocation: return "unsupported relocation target";
  }
  return "?";
}

constexpr std::size_t kRefusalKinds = 7;

struct BlockRecord {
  IR::LocationDescriptor desc{0};
  std::uint32_t off = 0;
  bool thumb = false;
  std::ptrdiff_t entry = 0;
  std::ptrdiff_t end = 0;
  /// Assigned by `Write` from the `(off, thumb)` sort, not by emission order: a change in queue order
  /// must not rename every block in the generated pair.
  std::size_t symbol = 0;
  std::vector<Backend::Relocation> relocations;
  std::vector<std::pair<IR::LocationDescriptor, std::vector<Backend::BlockRelocation>>> block_rels;
};

struct Prelude {
  std::ptrdiff_t run_code = -1;
  std::ptrdiff_t step_code = -1;
  std::ptrdiff_t return_to_dispatcher = -1;
  std::ptrdiff_t return_from_run_code = -1;
  std::map<Backend::LinkTarget, std::ptrdiff_t> target;
  std::map<Backend::LinkTarget, std::string> label;
};

/// One trampoline the emitted blocks call. The tail forms (every callback whose arguments are already
/// in X1/X2) branch to the C function with `b`, so its `ret` goes straight back to the block's link
/// register; the wrapped memory forms need to shuffle registers and return through `RET`.
///
/// `label_name` is per trampoline, not derived from `symbol`: the callback-only and wrapped memory
/// forms call the same C function but have different ABIs (value in X0 versus Xscratch0 with every
/// other caller-save register preserved), so they are two trampolines that need two symbols.
struct Trampoline {
  oaknut::Label label;
  std::ptrdiff_t offset = 0;
  std::string symbol;
  std::string label_name;
  Backend::LinkTarget target;
};

/// Encode a PC-relative B/BL immediate from two offsets in the same section.
///
/// AArch64 branches are relative to the address of the branch instruction itself, not to the next
/// instruction: `b` at offset 0 with imm 2 lands on offset 8 (verified with the NDK assembler). An
/// extra `- 4` here would put every relocation four bytes early — into the middle of a trampoline's
/// `mov`/`b` pair, one word past a block entry, or on the dispatcher's final `br`.
std::uint32_t EncodeBranch(std::ptrdiff_t site, std::ptrdiff_t target, bool link) {
  const std::ptrdiff_t delta = target - site;
  if (delta % 4 != 0) return 0;
  const std::int64_t words = delta / 4;
  if (words < -(1 << 25) || words >= (1 << 25)) return 0;
  const std::uint32_t imm = static_cast<std::uint32_t>(words) & 0x03ff'ffffu;
  return (link ? 0x9400'0000u : 0x1400'0000u) | imm;
}

bool IsThumb32(std::uint16_t hw) {
  return (hw & 0xe000u) == 0xe000u && (hw & 0x1800u) != 0;
}

/// The refusal rule's byte half: ARM bits 27-25 = 110, Thumb 0xEC00/0xEE00 families.
bool HasCoprocessor(const ElfCallbacks& bytes, std::uint32_t start, std::uint32_t end, bool thumb,
                    std::string* detail) {
  std::uint32_t pc = start;
  while (pc < end) {
    if (thumb) {
      std::uint16_t hw = 0;
      if (!bytes.ReadBytes(pc, &hw, 2)) {
        *detail = "instruction bytes outside .text";
        return true;
      }
      const bool wide = IsThumb32(hw);
      if (wide && (hw & 0xec00u) == 0xec00u) {
        *detail = "Thumb coprocessor encoding at " + Hex32(pc);
        return true;
      }
      pc += wide ? 4 : 2;
    } else {
      std::uint32_t word = 0;
      if (!bytes.ReadBytes(pc, &word, 4)) {
        *detail = "instruction bytes outside .text";
        return true;
      }
      if ((word & 0x0e00'0000u) == 0x0c00'0000u) {
        *detail = "ARM coprocessor encoding at " + Hex32(pc);
        return true;
      }
      pc += 4;
    }
  }
  return false;
}

Refusal ForbiddenOpcode(const IR::Block& block, std::string* detail) {
  for (const auto& inst : block) {
    switch (inst.GetOpcode()) {
      // The only vector-FP emitter that calls EmitTwoOpFallback*: it bakes a host function pointer
      // (`emit_arm64_vector_floating_point.cpp:271`) that no relocation record covers.
      case IR::Opcode::FPVectorRoundInt16:
        *detail = "IR opcode FPVectorRoundInt16 (vector-FP fallback)";
        return Refusal::VectorFallback;
      // Emitters that are ASSERT_FALSE in this pinned Dynarmic: refusing is the only safe answer.
      case IR::Opcode::FPVectorRecipStepFused16:
        *detail = "IR opcode FPVectorRecipStepFused16 (unimplemented emitter)";
        return Refusal::VectorFallback;
      // Backstop for the byte scan: a coprocessor IR op bakes a host callback address in
      // `emit_arm64_a32_coprocessor.cpp` with no relocation.
      case IR::Opcode::A32CoprocInternalOperation:
      case IR::Opcode::A32CoprocSendOneWord:
      case IR::Opcode::A32CoprocSendTwoWords:
      case IR::Opcode::A32CoprocGetOneWord:
      case IR::Opcode::A32CoprocGetTwoWords:
      case IR::Opcode::A32CoprocLoadWords:
      case IR::Opcode::A32CoprocStoreWords:
        *detail = "IR coprocessor operation";
        return Refusal::Coprocessor;
      default:
        break;
    }
  }
  return Refusal::None;
}

bool IsSupportedTarget(Backend::LinkTarget target) {
  switch (target) {
    case Backend::LinkTarget::ReturnToDispatcher:
    case Backend::LinkTarget::ReturnFromRunCode:
    case Backend::LinkTarget::ReadMemory8:
    case Backend::LinkTarget::ReadMemory16:
    case Backend::LinkTarget::ReadMemory32:
    case Backend::LinkTarget::ReadMemory64:
    case Backend::LinkTarget::WrappedReadMemory8:
    case Backend::LinkTarget::WrappedReadMemory16:
    case Backend::LinkTarget::WrappedReadMemory32:
    case Backend::LinkTarget::WrappedReadMemory64:
    case Backend::LinkTarget::ExclusiveReadMemory8:
    case Backend::LinkTarget::ExclusiveReadMemory16:
    case Backend::LinkTarget::ExclusiveReadMemory32:
    case Backend::LinkTarget::ExclusiveReadMemory64:
    case Backend::LinkTarget::WriteMemory8:
    case Backend::LinkTarget::WriteMemory16:
    case Backend::LinkTarget::WriteMemory32:
    case Backend::LinkTarget::WriteMemory64:
    case Backend::LinkTarget::WrappedWriteMemory8:
    case Backend::LinkTarget::WrappedWriteMemory16:
    case Backend::LinkTarget::WrappedWriteMemory32:
    case Backend::LinkTarget::WrappedWriteMemory64:
    case Backend::LinkTarget::ExclusiveWriteMemory8:
    case Backend::LinkTarget::ExclusiveWriteMemory16:
    case Backend::LinkTarget::ExclusiveWriteMemory32:
    case Backend::LinkTarget::ExclusiveWriteMemory64:
    case Backend::LinkTarget::CallSVC:
    case Backend::LinkTarget::ExceptionRaised:
    case Backend::LinkTarget::InstructionSynchronizationBarrierRaised:
    case Backend::LinkTarget::AddTicks:
    case Backend::LinkTarget::GetTicksRemaining:
      return true;
    default:
      return false;
  }
}

struct Emitter {
  Emitter(const std::vector<std::uint8_t>& elf, const CodeMap& map, std::uint32_t load_bias)
      : callbacks(elf, map, load_bias), map_(map), load_bias_(load_bias) {
    std::memset(scratch_.get(), 0, kCodeBytes);
    code = std::make_unique<oak::CodeGenerator>(reinterpret_cast<std::uint32_t*>(scratch_.get()));
  }

  // ---------------------------------------------------------------- prelude

  /// The tail form. The block (or the prelude) reaches this trampoline with `BL`; the branch to the
  /// C function must be a tail branch, not a `BL`: a nested `BL` would overwrite the link register
  /// the block is waiting on and the callback would return into the next trampoline.
  void EmitCallTrampoline(Backend::LinkTarget target, const std::string& symbol,
                          std::vector<Trampoline>* trampolines) {
    code->align(8);
    Trampoline t;
    t.target = target;
    t.symbol = symbol;
    t.label_name = symbol + "_tail_tramp";
    t.offset = code->offset();
    code->l(t.label);
    code->MOV(util::X0, Backend::Xstate);
    const std::ptrdiff_t at = code->offset();
    code->dw(0x1400'0000u);  // placeholder: replaced by "b <symbol>"
    externals.push_back(ExternalFixup{at, "b " + symbol});
    trampolines->push_back(t);
    prelude.target[target] = t.offset;
    prelude.label[target] = t.label_name;
  }

  void EmitWrappedReadTrampoline(Backend::LinkTarget target, const std::string& symbol,
                                 std::vector<Trampoline>* trampolines) {
    code->align(8);
    Trampoline t;
    t.target = target;
    t.symbol = symbol;
    t.label_name = symbol + "_tramp";
    t.offset = code->offset();
    code->l(t.label);
    constexpr Backend::RegisterList save = Backend::ABI_CALLER_SAVE & ~Backend::ToRegList(Backend::Xscratch0);
    Backend::ABI_PushRegisters(*code, save, 0);
    code->MOV(util::X0, Backend::Xstate);
    code->MOV(util::X1, Backend::Xscratch0);
    const std::ptrdiff_t at = code->offset();
    code->dw(0x9400'0000u);
    externals.push_back(ExternalFixup{at, "bl " + symbol});
    code->MOV(Backend::Xscratch0, util::X0);
    Backend::ABI_PopRegisters(*code, save, 0);
    code->RET();
    trampolines->push_back(t);
    prelude.target[target] = t.offset;
    prelude.label[target] = t.label_name;
  }

  void EmitWrappedWriteTrampoline(Backend::LinkTarget target, const std::string& symbol,
                                  std::vector<Trampoline>* trampolines) {
    code->align(8);
    Trampoline t;
    t.target = target;
    t.symbol = symbol;
    t.label_name = symbol + "_tramp";
    t.offset = code->offset();
    code->l(t.label);
    constexpr Backend::RegisterList save = Backend::ABI_CALLER_SAVE;
    Backend::ABI_PushRegisters(*code, save, 0);
    code->MOV(util::X0, Backend::Xstate);
    code->MOV(util::X1, Backend::Xscratch0);
    code->MOV(util::X2, Backend::Xscratch1);
    const std::ptrdiff_t at = code->offset();
    code->dw(0x9400'0000u);
    externals.push_back(ExternalFixup{at, "bl " + symbol});
    Backend::ABI_PopRegisters(*code, save, 0);
    code->RET();
    trampolines->push_back(t);
    prelude.target[target] = t.offset;
    prelude.label[target] = t.label_name;
  }

  void EmitPrelude() {
    // The callback trampolines go first, as Dynarmic's EmitPrelude does; the run-code entries below
    // call them by label, and the blocks call them through the relocation table.
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::ReadMemory8, "rb_aot_mem_read8"},
             {Backend::LinkTarget::ReadMemory16, "rb_aot_mem_read16"},
             {Backend::LinkTarget::ReadMemory32, "rb_aot_mem_read32"},
             {Backend::LinkTarget::ReadMemory64, "rb_aot_mem_read64"},
         }) {
      EmitCallTrampoline(target, sym, &trampolines);
    }
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::WrappedReadMemory8, "rb_aot_mem_read8"},
             {Backend::LinkTarget::WrappedReadMemory16, "rb_aot_mem_read16"},
             {Backend::LinkTarget::WrappedReadMemory32, "rb_aot_mem_read32"},
             {Backend::LinkTarget::WrappedReadMemory64, "rb_aot_mem_read64"},
         }) {
      EmitWrappedReadTrampoline(target, sym, &trampolines);
    }
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::ExclusiveReadMemory8, "rb_aot_excl_read8"},
             {Backend::LinkTarget::ExclusiveReadMemory16, "rb_aot_excl_read16"},
             {Backend::LinkTarget::ExclusiveReadMemory32, "rb_aot_excl_read32"},
             {Backend::LinkTarget::ExclusiveReadMemory64, "rb_aot_excl_read64"},
         }) {
      EmitCallTrampoline(target, sym, &trampolines);
    }
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::WriteMemory8, "rb_aot_mem_write8"},
             {Backend::LinkTarget::WriteMemory16, "rb_aot_mem_write16"},
             {Backend::LinkTarget::WriteMemory32, "rb_aot_mem_write32"},
             {Backend::LinkTarget::WriteMemory64, "rb_aot_mem_write64"},
         }) {
      EmitCallTrampoline(target, sym, &trampolines);
    }
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::WrappedWriteMemory8, "rb_aot_mem_write8"},
             {Backend::LinkTarget::WrappedWriteMemory16, "rb_aot_mem_write16"},
             {Backend::LinkTarget::WrappedWriteMemory32, "rb_aot_mem_write32"},
             {Backend::LinkTarget::WrappedWriteMemory64, "rb_aot_mem_write64"},
         }) {
      EmitWrappedWriteTrampoline(target, sym, &trampolines);
    }
    for (const auto& [target, sym] : std::initializer_list<std::pair<Backend::LinkTarget, const char*>>{
             {Backend::LinkTarget::ExclusiveWriteMemory8, "rb_aot_excl_write8"},
             {Backend::LinkTarget::ExclusiveWriteMemory16, "rb_aot_excl_write16"},
             {Backend::LinkTarget::ExclusiveWriteMemory32, "rb_aot_excl_write32"},
             {Backend::LinkTarget::ExclusiveWriteMemory64, "rb_aot_excl_write64"},
         }) {
      EmitCallTrampoline(target, sym, &trampolines);
    }
    EmitCallTrampoline(Backend::LinkTarget::CallSVC, "rb_aot_call_svc", &trampolines);
    EmitCallTrampoline(Backend::LinkTarget::ExceptionRaised, "rb_aot_exception_raised", &trampolines);
    EmitCallTrampoline(Backend::LinkTarget::InstructionSynchronizationBarrierRaised,
                       "rb_aot_isb_raised", &trampolines);
    EmitCallTrampoline(Backend::LinkTarget::AddTicks, "rb_aot_add_ticks", &trampolines);
    EmitCallTrampoline(Backend::LinkTarget::GetTicksRemaining, "rb_aot_get_ticks_remaining",
                       &trampolines);

    oak::Label return_from_run_code;
    oak::Label ret_dispatch_label;
    oak::Label get_ticks = FindLabel(Backend::LinkTarget::GetTicksRemaining);
    oak::Label add_ticks = FindLabel(Backend::LinkTarget::AddTicks);

    prelude.run_code = code->offset();
    {
      Backend::ABI_PushRegisters(*code, Backend::ABI_CALLEE_SAVE | (1u << 30), sizeof(Backend::StackLayout));
      code->MOV(util::X19, util::X0);
      code->MOV(Backend::Xstate, util::X1);
      code->MOV(Backend::Xhalt, util::X2);
      // Xpagetable/Xfastmem are deliberately not set: the tool config has neither, so no emitted
      // memory access reads them. (The JIT's prelude only sets them when the config asks for them.)
      if (conf_.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
        code->ADR(Backend::Xscratch0, ret_dispatch_label);
        for (std::size_t i = 0; i < Backend::RSBCount; ++i) {
          code->STR(Backend::Xscratch0, util::SP,
                    offsetof(Backend::StackLayout, rsb) + offsetof(Backend::RSBEntry, code_ptr) +
                        i * sizeof(Backend::RSBEntry));
        }
      }
      if (conf_.enable_cycle_counting) {
        code->BL(get_ticks);
        code->MOV(Backend::Xticks, util::X0);
        code->STR(Backend::Xticks, util::SP, offsetof(Backend::StackLayout, cycles_to_run));
      }
      code->LDR(Backend::Wscratch0, Backend::Xstate, offsetof(Backend::A32JitState, upper_location_descriptor));
      code->AND(Backend::Wscratch0, Backend::Wscratch0, 0xffff'0000u);
      code->MRS(Backend::Xscratch1, oak::SystemReg::FPCR);
      code->STR(Backend::Wscratch1, util::SP, offsetof(Backend::StackLayout, save_host_fpcr));
      code->MSR(oak::SystemReg::FPCR, Backend::Xscratch0);
      code->LDAR(Backend::Wscratch0, Backend::Xhalt);
      code->CBNZ(Backend::Wscratch0, return_from_run_code);
      code->BR(util::X19);
    }

    prelude.step_code = code->offset();
    {
      Backend::ABI_PushRegisters(*code, Backend::ABI_CALLEE_SAVE | (1u << 30), sizeof(Backend::StackLayout));
      code->MOV(util::X19, util::X0);
      code->MOV(Backend::Xstate, util::X1);
      code->MOV(Backend::Xhalt, util::X2);
      if (conf_.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
        code->ADR(Backend::Xscratch0, ret_dispatch_label);
        for (std::size_t i = 0; i < Backend::RSBCount; ++i) {
          code->STR(Backend::Xscratch0, util::SP,
                    offsetof(Backend::StackLayout, rsb) + offsetof(Backend::RSBEntry, code_ptr) +
                        i * sizeof(Backend::RSBEntry));
        }
      }
      if (conf_.enable_cycle_counting) {
        code->MOV(Backend::Xticks, 1);
        code->STR(Backend::Xticks, util::SP, offsetof(Backend::StackLayout, cycles_to_run));
      }
      code->LDR(Backend::Wscratch0, Backend::Xstate, offsetof(Backend::A32JitState, upper_location_descriptor));
      code->AND(Backend::Wscratch0, Backend::Wscratch0, 0xffff'0000u);
      code->MRS(Backend::Xscratch1, oak::SystemReg::FPCR);
      code->STR(Backend::Wscratch1, util::SP, offsetof(Backend::StackLayout, save_host_fpcr));
      code->MSR(oak::SystemReg::FPCR, Backend::Xscratch0);

      oak::Label step_hr_loop;
      code->l(step_hr_loop);
      code->LDAXR(Backend::Wscratch0, Backend::Xhalt);
      code->CBNZ(Backend::Wscratch0, return_from_run_code);
      code->ORR(Backend::Wscratch0, Backend::Wscratch0,
                static_cast<std::uint32_t>(Dynarmic::HaltReason::Step));
      code->STLXR(Backend::Wscratch1, Backend::Wscratch0, Backend::Xhalt);
      code->CBNZ(Backend::Wscratch1, step_hr_loop);
      code->BR(util::X19);
    }

    code->align(8);
    prelude.return_to_dispatcher = code->offset();
    code->l(ret_dispatch_label);
    {
      code->LDAR(Backend::Wscratch0, Backend::Xhalt);
      code->CBNZ(Backend::Wscratch0, return_from_run_code);
      if (conf_.enable_cycle_counting) {
        code->CMP(Backend::Xticks, 0);
        code->B(oak::Cond::LE, return_from_run_code);
      }
      code->MOV(util::X0, Backend::Xstate);
      const std::ptrdiff_t at = code->offset();
      code->dw(0x9400'0000u);  // placeholder: "bl rb_aot_dispatch"
      externals.push_back(ExternalFixup{at, "bl rb_aot_dispatch"});
      code->CBZ(util::X0, return_from_run_code);
      code->BR(util::X0);
    }

    code->align(8);
    prelude.return_from_run_code = code->offset();
    code->l(return_from_run_code);
    {
      if (conf_.enable_cycle_counting) {
        code->LDR(util::X1, util::SP, offsetof(Backend::StackLayout, cycles_to_run));
        code->SUB(util::X1, util::X1, Backend::Xticks);
        code->BL(add_ticks);
      }
      code->LDR(Backend::Wscratch0, util::SP, offsetof(Backend::StackLayout, save_host_fpcr));
      code->MSR(oak::SystemReg::FPCR, Backend::Xscratch0);

      oak::Label exit_hr_loop;
      code->l(exit_hr_loop);
      code->LDAXR(util::W0, Backend::Xhalt);
      code->STLXR(Backend::Wscratch0, util::WZR, Backend::Xhalt);
      code->CBNZ(Backend::Wscratch0, exit_hr_loop);
      Backend::ABI_PopRegisters(*code, Backend::ABI_CALLEE_SAVE | (1u << 30), sizeof(Backend::StackLayout));
      code->RET();
    }

    code->align(8);
    prelude_end = code->offset();
  }

  oak::Label FindLabel(Backend::LinkTarget target) {
    for (auto& t : trampolines) {
      if (t.target == target) return t.label;
    }
    return {};
  }

  // ---------------------------------------------------------------- blocks

  bool CoprocessorRefusal(const ElfCallbacks& bytes, std::uint32_t start, std::uint32_t end,
                          bool thumb, std::string* detail) {
    return HasCoprocessor(bytes, start, end, thumb, detail);
  }

  bool EmitQueued(std::deque<IR::LocationDescriptor>* queue, std::string* err,
                  std::map<std::uint64_t, std::size_t>* by_desc,
                  std::map<std::pair<std::uint32_t, std::uint8_t>, std::size_t>* by_key,
                  std::map<std::pair<std::uint32_t, std::uint8_t>, Refusal>* refused) {
    while (!queue->empty()) {
      const IR::LocationDescriptor desc = queue->front();
      queue->pop_front();
      if (by_desc->count(desc.Value()) != 0) continue;
      const A32::LocationDescriptor loc{desc};
      const std::uint32_t pc = loc.PC();
      const bool thumb = loc.TFlag();
      if (loc.IT().Value() != 0 || loc.FPSCR().Value() != 0) {
        /**
         * The table is keyed on `(off, thumb)` only, so a block emitted for a non-default IT/FPSCR
         * descriptor could be selected for an IT=0 dispatch — exactly the mismatch the runtime now
         * refuses, but refusing it at run time would only turn a wrong block into a fallback. The
         * honest statement is that this target has no emittable block: leave it on Tier I and do not
         * poison the `(off, thumb)` refusal map, because an IT=0 descriptor at the same offset is a
         * different block and is still emittable.
         */
        ++refused_counts[static_cast<int>(Refusal::LocationDescriptor)];
        if (refused_log.size() < 32) {
          refused_log.push_back("non-default IT/FPSCR entry (IT=" + Hex32(loc.IT().Value()) +
                                " FPSCR=" + Hex32(loc.FPSCR().Value()) + ") at " + Hex32(pc));
        }
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      if (pc < callbacks.text_base() || pc - callbacks.text_base() >= map_.text_len) {
        const std::pair<std::uint32_t, std::uint8_t> key{pc - callbacks.text_base(), thumb ? kThumb : kArm};
        (*refused)[key] = Refusal::OutOfRange;
        ++refused_counts[static_cast<int>(Refusal::OutOfRange)];
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      const std::uint32_t off = pc - callbacks.text_base();
      const std::pair<std::uint32_t, std::uint8_t> key{off, static_cast<std::uint8_t>(thumb ? kThumb : kArm)};
      if (const auto r = refused->find(key); r != refused->end()) {
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      if (const auto k = by_key->find(key); k != by_key->end()) {
        (*by_desc)[desc.Value()] = k->second;
        continue;
      }

      IR::Block block = address_space->GenerateIR(desc);
      const A32::LocationDescriptor end_loc{block.EndLocation()};
      const std::uint32_t end = end_loc.PC();
      if (end <= pc || end - callbacks.text_base() > map_.text_len) {
        (*refused)[key] = Refusal::OutOfRange;
        ++refused_counts[static_cast<int>(Refusal::OutOfRange)];
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      std::string detail;
      if (CoprocessorRefusal(callbacks, pc, end, thumb, &detail)) {
        (*refused)[key] = Refusal::Coprocessor;
        ++refused_counts[static_cast<int>(Refusal::Coprocessor)];
        if (refused_log.size() < 32) refused_log.push_back(detail);
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      if (block.GetTerminal().which() == kTerminalInterpret) {
        (*refused)[key] = Refusal::Interpret;
        ++refused_counts[static_cast<int>(Refusal::Interpret)];
        if (refused_log.size() < 32) refused_log.push_back("Term::Interpret at " + Hex32(pc));
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }
      const Refusal forbidden = ForbiddenOpcode(block, &detail);
      if (forbidden != Refusal::None) {
        (*refused)[key] = forbidden;
        ++refused_counts[static_cast<int>(forbidden)];
        if (refused_log.size() < 32) refused_log.push_back(detail + " at " + Hex32(pc));
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        continue;
      }

      if (code->offset() + 4096 > static_cast<std::ptrdiff_t>(kCodeBytes)) {
        *err = "emitted code exceeds the tool's scratch buffer";
        return false;
      }
      BlockRecord rec;
      rec.desc = desc;
      rec.off = off;
      rec.thumb = thumb;

      /**
       * Emit into a scratch stream first. A block that needs a relocation target the prelude does not
       * emit — the 128-bit memory forms bake a host callback with no relocation record — used to abort
       * the whole image, so one NEON pair anywhere in a game library meant no AOT image at all. It is a
       * per-block property, so it refuses that block (counted and printed) and the rest of the image is
       * still emitted. Copying the finished words is exact: a block's internal branches and literal
       * pools are relative to its own entry, and both streams start at an 8-byte-aligned offset.
       */
      oak::CodeGenerator probe{reinterpret_cast<std::uint32_t*>(probe_.get())};
      Backend::EmittedBlockInfo info = address_space->EmitForAot(block, probe);
      if (info.size > kProbeBytes || probe.offset() != static_cast<std::ptrdiff_t>(info.size) ||
          info.size % sizeof(std::uint32_t) != 0) {
        *err = "emitted block size disagrees with EmitForAot";
        return false;
      }
      bool unsupported_relocation = false;
      for (const auto& r : info.relocations) {
        if (IsSupportedTarget(r.target)) continue;
        (*refused)[key] = Refusal::UnsupportedRelocation;
        ++refused_counts[static_cast<int>(Refusal::UnsupportedRelocation)];
        if (refused_log.size() < 32) {
          refused_log.push_back(
              std::string{"relocation target the AOT prelude does not emit (link target "} +
              std::to_string(static_cast<int>(r.target)) + ") at " + Hex32(pc));
        }
        (*by_desc)[desc.Value()] = static_cast<std::size_t>(-1);
        unsupported_relocation = true;
        break;
      }
      if (unsupported_relocation) continue;
      if (!info.fastmem_patch_info.empty()) {
        *err = "a block recorded a fastmem patch despite the tool's null fastmem pointer";
        return false;
      }

      code->align(8);
      rec.entry = code->offset();
      const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(probe_.get());
      for (std::size_t i = 0; i < info.size / sizeof(std::uint32_t); ++i) code->dw(words[i]);
      rec.end = code->offset();
      if (rec.end - rec.entry != static_cast<std::ptrdiff_t>(info.size)) {
        *err = "emitted block size disagrees with EmitForAot";
        return false;
      }
      rec.relocations = info.relocations;

      // `block_relocations` is a robin_map: its iteration order is not part of any contract. Sort the
      // targets (and the recorded pairs) so a regeneration emits the same queue and the same bytes.
      std::vector<IR::LocationDescriptor> targets;
      targets.reserve(info.block_relocations.size());
      for (const auto& [target_desc, list] : info.block_relocations) {
        rec.block_rels.emplace_back(target_desc, list);
        const A32::LocationDescriptor t{target_desc};
        if (t.PC() < callbacks.text_base() ||
            t.PC() - callbacks.text_base() >= map_.text_len) {
          continue;  // a host entry or the call sentinel: the dispatcher handles it
        }
        targets.push_back(target_desc);
      }
      std::sort(targets.begin(), targets.end(), [](const IR::LocationDescriptor& a,
                                                    const IR::LocationDescriptor& b) {
        return a.Value() < b.Value();
      });
      for (const IR::LocationDescriptor& target : targets) queue->push_back(target);
      std::sort(rec.block_rels.begin(), rec.block_rels.end(),
                [](const auto& a, const auto& b) { return a.first.Value() < b.first.Value(); });
      by_key->emplace(key, blocks.size());
      by_desc->emplace(desc.Value(), blocks.size());
      blocks.push_back(std::move(rec));
    }
    return true;
  }

  // ---------------------------------------------------------------- output

  bool Write(const std::string& out_prefix, std::uint64_t fingerprint, std::string* err) {
    // Symbol numbers are a property of the block set, not of the order it was discovered in. Sorting by
    // `(off, thumb)` is what makes two runs of the tool over one `.so` produce the same `.S`, the same
    // table, and the same symbol names even when a robin_map's iteration order changes.
    std::vector<std::size_t> order(blocks.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (blocks[a].off != blocks[b].off) return blocks[a].off < blocks[b].off;
      return blocks[a].thumb < blocks[b].thumb;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) blocks[order[rank]].symbol = rank;

    // Build the permanent target names and the per-site fixups.
    std::map<std::ptrdiff_t, std::string> offset_label;
    offset_label[prelude.return_to_dispatcher] = prefix + "_ret_dispatch";
    offset_label[prelude.return_from_run_code] = prefix + "_ret_run_code";
    for (const auto& t : trampolines) offset_label[t.offset] = t.label_name;
    for (const BlockRecord& b : blocks) {
      offset_label[b.entry] = prefix + "_block_" + Pad(b.symbol);
    }

    std::map<std::ptrdiff_t, std::vector<std::string>> label_lines;
    label_lines[prelude.run_code].push_back(".globl " + prefix + "_run_code");
    label_lines[prelude.run_code].push_back(".type " + prefix + "_run_code, %function");
    label_lines[prelude.run_code].push_back(prefix + "_run_code:");
    label_lines[prelude.step_code].push_back(prefix + "_step_code:");
    for (const auto& [at, name] : offset_label) {
      if (at == prelude.run_code) continue;
      label_lines[at].push_back(name + ":");
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      const std::ptrdiff_t at = blocks[i].entry;
      label_lines[at].insert(label_lines[at].begin(),
                             {".globl " + offset_label[at],
                              ".type " + offset_label[at] + ", %function"});
    }

    std::map<std::ptrdiff_t, TextFixup> fixups;
    const auto add_fixup = [&](std::ptrdiff_t at, std::vector<std::string> lines) -> bool {
      if (fixups.count(at) != 0) {
        *err = "two relocations at one code offset";
        return false;
      }
      fixups[at] = TextFixup{at, std::move(lines)};
      return true;
    };

    for (const BlockRecord& rec : blocks) {
      for (const auto& r : rec.relocations) {
        const std::ptrdiff_t at = rec.entry + r.code_offset;
        std::ptrdiff_t target_at = 0;
        switch (r.target) {
          case Backend::LinkTarget::ReturnToDispatcher:
            target_at = prelude.return_to_dispatcher;
            break;
          case Backend::LinkTarget::ReturnFromRunCode:
            target_at = prelude.return_from_run_code;
            break;
          default:
            target_at = prelude.target.at(r.target);
            break;
        }
        const bool link = r.target != Backend::LinkTarget::ReturnToDispatcher &&
                          r.target != Backend::LinkTarget::ReturnFromRunCode;
        const std::uint32_t word = EncodeBranch(at, target_at, link);
        if (word == 0) {
          *err = "branch out of range at " + std::to_string(at);
          return false;
        }
        if (!add_fixup(at, {".word " + Hex32(word)})) return false;
      }
      for (const auto& [target_desc, list] : rec.block_rels) {
        const A32::LocationDescriptor target{target_desc};
        const std::uint32_t target_pc = target.PC();
        const std::uint32_t target_off =
            target_pc >= callbacks.text_base() ? target_pc - callbacks.text_base() : 0xffff'ffffu;
        auto target_key = std::make_pair(target_off, static_cast<std::uint8_t>(
                                                           target.TFlag() ? kThumb : kArm));
        const auto target_block = by_key.find(target_key);
        for (const auto& br : list) {
          const std::ptrdiff_t at = rec.entry + br.code_offset;
          if (br.type == Backend::BlockRelocationType::Branch) {
            if (target_block != by_key.end()) {
              const std::uint32_t word = EncodeBranch(at, blocks[target_block->second].entry, false);
              if (word == 0) {
                *err = "block branch out of range at " + std::to_string(at);
                return false;
              }
              if (!add_fixup(at, {".word " + Hex32(word)})) return false;
            } else {
              if (!add_fixup(at, {".word 0xd503201f"})) return false;  // NOP: fall through to dispatch
            }
          } else {
            const std::string symbol =
                target_block != by_key.end() ? offset_label.at(blocks[target_block->second].entry)
                                             : offset_label.at(prelude.return_to_dispatcher);
            if (!add_fixup(at, {"adrp x17, " + symbol, "add x17, x17, :lo12:" + symbol})) return false;
          }
        }
      }
    }

    std::ofstream out(out_prefix + ".S", std::ios::binary);
    if (!out) {
      *err = "cannot write " + out_prefix + ".S";
      return false;
    }
    out << "// Generated by rb_aotgen (RetroBug AOT task 3) from " << BaseName(elf_path_) << "\n";
    out << "// fingerprint (FNV-1a-64) " << Hex64(fingerprint) << ", .text vaddr "
        << Hex32(map_.text_vaddr) << ", load bias " << Hex32(load_bias_) << "\n";
    out << "// Regenerate: rb_aotgen --emit <lib.so> --out " << BaseName(out_prefix)
        << " --base " << Hex32(load_bias_) << "\n";
    out << "// code size " << (blocks.empty() ? prelude_end : blocks.back().end) << " bytes, "
        << blocks.size() << " blocks\n";
    out << "    .text\n    .p2align 4\n";

    const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(scratch_.get());
    // The prelude is emitted first, then every block. `prelude_end` is only the prelude's extent, so
    // the stream must run to the last block's end; stopping at the prelude used to write a table full
    // of block symbols that the `.S` never defined.
    const std::ptrdiff_t total = blocks.empty() ? prelude_end : blocks.back().end;
    std::ptrdiff_t at = 0;
    while (at < total) {
      if (const auto l = label_lines.find(at); l != label_lines.end()) {
        for (const auto& line : l->second) out << line << "\n";
      }
      bool handled = false;
      if (const auto f = fixups.find(at); f != fixups.end()) {
        for (const auto& line : f->second.lines) out << "    " << line << "\n";
        at += static_cast<std::ptrdiff_t>(f->second.lines.size()) * 4;
        handled = true;
      }
      if (!handled) {
        if (const auto e = external_at(at); e >= 0) {
          out << "    " << externals[static_cast<std::size_t>(e)].line << "\n";
          at += 4;
          handled = true;
        }
      }
      if (!handled) {
        out << "    .word " << Hex32(words[at / 4]) << "\n";
        at += 4;
      }
    }
    code_bytes = total;
    // The trailer is the `.S` half of the pairing check. It is data, not a label after the code:
    // `-ffunction-sections` puts every `.type ...,%function` label in its own section, so a symbol
    // distance would measure link order rather than the emitted size. The table carries the same two
    // numbers, and rb_aot_tests asserts this pair agrees with it.
    out << "\n// The table carries these two numbers; rb_aot_tests asserts they agree.\n";
    out << "    .section .rodata\n    .p2align 2\n";
    out << ".globl " << prefix << "_code_bytes\n";
    out << prefix << "_code_bytes:\n";
    out << "    .word " << total << "\n";
    out << ".globl " << prefix << "_block_count\n";
    out << prefix << "_block_count:\n";
    out << "    .word " << blocks.size() << "\n";
    out << "    .section .note.GNU-stack,\"\",%progbits\n";
    if (!out) {
      *err = "write failed for " + out_prefix + ".S";
      return false;
    }
    return true;
  }

  std::ptrdiff_t external_at(std::ptrdiff_t at) const {
    for (std::size_t i = 0; i < externals.size(); ++i) {
      if (externals[i].offset == at) return static_cast<std::ptrdiff_t>(i);
    }
    return -1;
  }

  static std::string Pad(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04zu", i);
    return buf;
  }

  ElfCallbacks callbacks;
  const CodeMap& map_;
  std::uint32_t load_bias_;
  A32::UserConfig conf_{};
  std::unique_ptr<Backend::A32AddressSpace> address_space;
  std::unique_ptr<std::uint8_t[]> scratch_{new std::uint8_t[kCodeBytes]};
  std::unique_ptr<std::uint8_t[]> probe_{new std::uint8_t[kProbeBytes]};
  std::unique_ptr<oak::CodeGenerator> code;
  std::string prefix;
  std::string elf_path_;
  std::ptrdiff_t code_bytes = 0;  ///< end of the last emitted block, set by `Write`
  std::vector<Trampoline> trampolines;
  std::vector<ExternalFixup> externals;
  Prelude prelude;
  std::ptrdiff_t prelude_end = 0;
  std::vector<BlockRecord> blocks;
  std::map<std::pair<std::uint32_t, std::uint8_t>, std::size_t> by_key;
  std::vector<std::string> refused_log;
  std::array<int, kRefusalKinds> refused_counts{};
};

}  // namespace

bool EmitImage(const std::string& elf_path, const std::string& out_prefix, std::uint32_t load_bias,
               std::string* err) {
  const auto fail = [&](const std::string& msg) {
    if (err != nullptr) *err = msg;
    return false;
  };
  std::ifstream in(elf_path, std::ios::binary | std::ios::ate);
  if (!in) return fail("cannot read " + elf_path);
  const std::streamsize size = in.tellg();
  if (size <= 0) return fail("empty file " + elf_path);
  std::vector<std::uint8_t> elf(static_cast<std::size_t>(size));
  in.seekg(0, std::ios::beg);
  if (!in.read(reinterpret_cast<char*>(elf.data()), size)) return fail("short read " + elf_path);

  CodeMap map{};
  std::string map_err;
  map = DiscoverCodeMap(elf, nullptr, &map_err);
  if (!map_err.empty()) return fail(map_err);
  if (map.entries.empty()) return fail("the code map has no starts");

  Emitter em{elf, map, load_bias};
  em.elf_path_ = elf_path;
  em.prefix = SymbolPrefix(out_prefix);
  em.conf_ = A32::UserConfig{};
  em.conf_.callbacks = &em.callbacks;
  em.conf_.arch_version = A32::ArchVersion::v7;
  em.conf_.define_unpredictable_behaviour = true;
  em.conf_.hook_hint_instructions = true;
  em.conf_.always_little_endian = true;
  em.conf_.check_halt_on_memory_access = true;
  em.conf_.enable_cycle_counting = true;
  em.conf_.very_verbose_debugging_output = false;
  em.conf_.page_table = nullptr;
  em.conf_.fastmem_pointer = std::nullopt;
  em.conf_.code_cache_size = 1u << 20;
  em.address_space = std::make_unique<Backend::A32AddressSpace>(em.conf_);

  std::deque<IR::LocationDescriptor> queue;
  for (const CodeMapEntry& entry : map.entries) {
    if (load_bias + map.text_vaddr + entry.off < load_bias) continue;
    A32::LocationDescriptor d{load_bias + map.text_vaddr + entry.off, A32::PSR{}, A32::FPSCR{}, false};
    d = d.SetTFlag((entry.flags & kThumb) != 0);
    const IR::LocationDescriptor ir_desc = d;
    queue.push_back(ir_desc);
  }
  // The code map is sorted by offset already, but the queue is a traversal input, not a map invariant:
  // sort it explicitly so the block order (and every branch displacement) is regenerable.
  std::sort(queue.begin(), queue.end(),
            [](const IR::LocationDescriptor& a, const IR::LocationDescriptor& b) {
              return a.Value() < b.Value();
            });

  em.EmitPrelude();

  std::map<std::uint64_t, std::size_t> by_desc;
  std::map<std::pair<std::uint32_t, std::uint8_t>, Refusal> refused;
  std::string emit_err;
  if (!em.EmitQueued(&queue, &emit_err, &by_desc, &em.by_key, &refused)) return fail(emit_err);
  if (em.blocks.empty()) return fail("no block could be emitted");

  std::string write_err;
  if (!em.Write(out_prefix, Fnv1a64(elf), &write_err)) return fail(write_err);

  // The table.
  std::string table_name = TableName(out_prefix);
  std::ofstream table(out_prefix + "_table.cpp", std::ios::binary);
  if (!table) return fail("cannot write " + out_prefix + "_table.cpp");
  table << "// Generated by rb_aotgen (RetroBug AOT task 3) from " << BaseName(elf_path) << "\n";
  table << "// fingerprint (FNV-1a-64) " << Hex64(Fnv1a64(elf)) << ", .text vaddr "
        << Hex32(map.text_vaddr) << ", load bias " << Hex32(load_bias) << "\n";
  table << "// Regenerate: rb_aotgen --emit <lib.so> --out " << BaseName(out_prefix)
        << " --base " << Hex32(load_bias) << "\n";
  table << "// code size " << em.code_bytes << " bytes, " << em.blocks.size() << " blocks\n";
  table << "// The emitted .S must be assembled into the same binary as this table.\n";
  table << "// Do not edit; the fixture test's header carries the full regeneration command.\n";
  table << "#include \"rb/exec/aot.h\"\n\n";
  table << "extern \"C\" {\n";
  for (std::size_t i = 0; i < em.blocks.size(); ++i) {
    table << "std::uint32_t " << em.prefix << "_block_" << Emitter::Pad(em.blocks[i].symbol)
          << "(rb::exec::AotState*);\n";
  }
  table << "extern const std::uint32_t " << em.prefix << "_code_bytes;\n";
  table << "extern const std::uint32_t " << em.prefix << "_block_count;\n";
  table << "Dynarmic::HaltReason " << em.prefix
        << "_run_code(void*, void*, volatile std::uint32_t*);\n";
  table << "}\n\n";
  table << "namespace {\n";
  table << "const rb::exec::AotBlock kAotBlocks[] = {\n";
  std::vector<std::size_t> sorted(em.blocks.size());
  for (std::size_t i = 0; i < sorted.size(); ++i) sorted[i] = i;
  std::sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) {
    const auto& x = em.blocks[a];
    const auto& y = em.blocks[b];
    if (x.off != y.off) return x.off < y.off;
    return x.thumb < y.thumb;
  });
  for (const std::size_t i : sorted) {
    const BlockRecord& b = em.blocks[i];
    table << "    {" << Hex32(b.off) << "u, rb::exec::" << (b.thumb ? "kAotThumb" : "kAotArm")
          << ", &" << em.prefix << "_block_" << Emitter::Pad(b.symbol) << "},\n";
  }
  table << "};\n";
  table << "}  // namespace\n\n";
  table << "extern const rb::exec::AotImageInfo " << table_name << ";\n";
  table << "const rb::exec::AotImageInfo " << table_name << " = {\n";
  table << "    kAotBlocks,\n";
  table << "    sizeof(kAotBlocks) / sizeof(kAotBlocks[0]),\n";
  table << "    " << Hex32(map.text_vaddr) << "u,\n";
  table << "    " << Hex32(map.text_len) << "u,\n";
  table << "    " << Hex32(load_bias) << "u,\n";
  table << "    " << Hex64(Fnv1a64(elf)) << "ull,\n";
  table << "    " << Hex32(static_cast<std::uint32_t>(em.code_bytes)) << "u,\n";
  table << "    " << Hex32(static_cast<std::uint32_t>(em.blocks.size())) << "u,\n";
  table << "    &" << em.prefix << "_run_code,\n";
  table << "};\n";
  if (!table) return fail("write failed for " + out_prefix + "_table.cpp");

  std::printf(
      "aotgen: %s: emitted %zu blocks, refused %zu (coprocessor %d, vector-fp %d, untranslated %d, "
      "outside-text %d, location %d, relocation %d), .text %u bytes, prelude %lld bytes, code %lld bytes\n",
      map.soname.empty() ? BaseName(elf_path).c_str() : map.soname.c_str(), em.blocks.size(),
      static_cast<std::size_t>(em.refused_counts[1] + em.refused_counts[2] + em.refused_counts[3] +
                               em.refused_counts[4] + em.refused_counts[5] + em.refused_counts[6]),
      em.refused_counts[1], em.refused_counts[2], em.refused_counts[3], em.refused_counts[4],
      em.refused_counts[5], em.refused_counts[6], map.text_len,
      static_cast<long long>(em.prelude_end),
      static_cast<long long>(em.blocks.empty() ? em.prelude_end : em.blocks.back().end));
  for (const auto& line : em.refused_log) std::printf("aotgen: refused: %s\n", line.c_str());
  if (err != nullptr) err->clear();
  return true;
}

}  // namespace rb::aotgen
