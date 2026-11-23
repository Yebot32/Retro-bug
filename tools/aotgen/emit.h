// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The AOT emission half of `rb_aotgen` (AOT task 3). Compiled only on AArch64 hosts, where Dynarmic's
// AArch64 back end exists; the discovery CLI and its test stay buildable everywhere.
#pragma once

#include <cstdint>
#include <string>

namespace rb::aotgen {

struct CodeMap;

/// What one emitted image reports about itself, for the profile writer (AOT task 5).
struct EmitFacts {
  std::uint32_t text_vaddr = 0;
  std::uint32_t text_len = 0;
  std::uint64_t fingerprint = 0;
  std::size_t blocks = 0;      ///< emitted blocks (the table's rows)
  std::size_t refused = 0;     ///< blocks left on Tier I
  std::size_t code_bytes = 0;  ///< emitted code + prelude, as the table records it
  std::string symbol;          ///< the image symbol (`kAot<Camel>Image`) the table defines
};

/// Translate every start in `elf_path`'s code map (and every interior target they branch to) into one
/// AArch64 assembly file `<out_prefix>.S`, and write the `AotBlock[]` table `<out_prefix>_table.cpp`.
///
/// `load_bias` is the guest address the image is assumed to be loaded at. The emitted blocks bake
/// guest PCs (`load_bias + text_vaddr + off`), so the runtime must map the image at exactly this bias;
/// the generated table records it. `trace`, when non-null, is merged into the code map first (its
/// offsets are image-relative; main.cpp builds it from the recorded PCs). `facts`, when non-null,
/// receives the numbers the profile writer needs. `err` is cleared on success and names the first
/// refusal otherwise.
bool EmitImage(const std::string& elf_path, const std::string& out_prefix, std::uint32_t load_bias,
               const CodeMap* trace, EmitFacts* facts, std::string* err);

/// The image symbol `EmitImage` defines in `<out_prefix>_table.cpp` for this output prefix.
std::string ImageSymbol(const std::string& out_prefix);

}  // namespace rb::aotgen
