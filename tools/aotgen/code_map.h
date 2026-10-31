// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The AOT code map: which guest `.text` addresses are worth translating offline (AOT task 2).
//
// Deliberately self-contained: `rb::loader` answers "load this image into a guest", which brings a memory
// model, a symbol resolver and the guest's ABI with it. This tool inspects a file and never runs it, so it
// parses just the ELF32 structures the six discovery sources need and depends on nothing in `core/`.
//
// Offsets are *file-relative*: `(vaddr & ~1) - text_vaddr`. A code map therefore survives a guest image
// being loaded at a different base, and is the unit the emitter (AOT task 3) and the on-device dispatcher
// both index by. The Thumb bit is recorded in `flags` and stripped from `off`, because two sources can
// name the same function once as a Thumb symbol (odd `st_value`) and once from `.ARM.exidx` (an even
// prel31 target).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rb::aotgen {

/// One discovered start. `size` is the symbol's `st_size` when a source supplied one, otherwise 0; it is
/// capped at 0xffff because the on-disk RBGM/RBCM row is four bytes.
struct CodeMapEntry {
  std::uint32_t off;
  std::uint16_t size;
  std::uint8_t flags;
  std::uint8_t sources;
};

struct CodeMap {
  std::string soname;
  std::uint32_t text_off;
  std::uint32_t text_len;
  std::vector<CodeMapEntry> entries;
};

/// Exactly one of these is set on every entry (a start is Thumb or ARM, never both).
enum : std::uint8_t { kThumb = 1, kArm = 2 };

/// Why an entry exists. Bits are OR-ed when several sources name the same start.
enum : std::uint8_t {
  kFromExport = 1,   // STT_FUNC in .dynsym/.symtab whose value is in .text
  kFromInit = 2,     // DT_INIT or a DT_INIT_ARRAY slot
  kFromReloc = 4,    // .rel.dyn/.rel.plt addend (or GLOB_DAT symbol) in .text
  kFromExidx = 8,    // .ARM.exidx function start
  kFromVeneer = 16,  // a linker veneer stub found inside .text
  kFromTrace = 32,   // an indirect target recorded on device (AOT task 4)
};

/// Parse `elf` and return every start the static sources name. `err` is cleared on success and set to a
/// human-readable reason (e.g. "ELF64 is not supported") on failure. A `trace` map, if given, is merged
/// last and only contributes `kFromTrace` (plus its architecture when it has one).
CodeMap DiscoverCodeMap(const std::vector<std::uint8_t>& elf, const CodeMap* trace, std::string* err);

/// Number of entries carrying `source` in their `sources` mask.
std::size_t CountSource(const CodeMap& map, std::uint8_t source);

}  // namespace rb::aotgen
