// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The AOT emission half of `rb_aotgen` (AOT task 3). Compiled only on AArch64 hosts, where Dynarmic's
// AArch64 back end exists; the discovery CLI and its test stay buildable everywhere.
#pragma once

#include <cstdint>
#include <string>

namespace rb::aotgen {

/// Translate every start in `elf_path`'s code map (and every interior target they branch to) into one
/// AArch64 assembly file `<out_prefix>.S`, and write the `AotBlock[]` table `<out_prefix>_table.cpp`.
///
/// `load_bias` is the guest address the image is assumed to be loaded at. The emitted blocks bake
/// guest PCs (`load_bias + text_vaddr + off`), so the runtime must map the image at exactly this bias;
/// the generated table records it. `err` is cleared on success and names the first refusal otherwise.
bool EmitImage(const std::string& elf_path, const std::string& out_prefix, std::uint32_t load_bias,
               std::string* err);

}  // namespace rb::aotgen
