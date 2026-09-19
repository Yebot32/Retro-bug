/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Guest AArch32 ELF loading (component C05).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/mem/guest_memory.h"

namespace rb::loader {

/// A guest address that holds code.
///
/// The ARM ABI carries the ISA state in bit 0 of every function address: set means Thumb, clear means
/// ARM. Branching to such an address with the bit still in it, or having thrown the bit away, decodes
/// the guest's Thumb instructions as ARM — which does not fault, it executes different semantics. So
/// the flag has to travel with the address rather than being masked out at each use.
struct CodeAddress {
  std::uint32_t addr = 0;  ///< Byte address, bit 0 cleared.
  bool thumb = false;
  explicit operator bool() const { return addr != 0; }
  bool operator==(const CodeAddress&) const = default;
};

/// One exported or imported dynamic symbol, in guest terms.
struct Symbol {
  std::string name;
  /// st_value plus the load bias, kept exactly as the image states it: for a function that includes
  /// the bit-0 Thumb tag, which is what a data reference to the function has to point at. Use
  /// GuestModule::FindExport to get an address to execute.
  std::uint32_t value = 0;
  std::uint32_t size = 0;
  bool defined = false;      ///< False for undefined symbols the loader must resolve elsewhere.
  bool weak = false;
  bool is_function = false;
  bool thumb = false;        ///< is_function and st_value bit 0 set.
};

/// The linker-facing facts about one loaded module.
struct GuestModule {
  std::string path;
  std::string soname;
  std::uint32_t base = 0;      ///< Guest address the image was placed at.
  std::uint32_t extent = 0;    ///< Guest bytes covered, rounded to guest pages.
  CodeAddress entry;           ///< e_entry, already biased by `base`.
  CodeAddress init;            ///< DT_INIT, biased. Absent (addr 0) on NDK images, which use init_array.
  CodeAddress fini;            ///< DT_FINI, biased.
  std::vector<std::string> needed;
  std::vector<Symbol> exports;
  std::vector<Symbol> imports;
  std::vector<std::string> unsupported_relocs;  ///< Relocation kinds refused, named for diagnostics.

  /// PT_NOTE build-id, if the image carries one. The NDK-built fixture used in tests does **not**,
  /// so nothing here may assume it is present. App Revival's patch identity (docs/10 §12.2) needs a
  /// content hash as the fallback key; that lands with tier-3 patching, which is deliberately held.
  std::vector<std::uint8_t> build_id;

  /// An address to branch to, with the Thumb tag split out of it. nullopt when the name is not
  /// defined by this module.
  std::optional<CodeAddress> FindExport(const std::string& name) const;
  std::vector<std::string> ExportNames() const;
};

/// Supplies guest addresses for symbols the module imports. Returning nullopt means "unresolved":
/// the loader records it and, unless the symbol is weak, fails the load rather than binding to zero,
/// because a silent null here becomes a guest jump to address 0 at runtime.
using SymbolResolver = std::function<std::optional<std::uint32_t>(const std::string& name)>;

struct LoadOptions {
  std::uint32_t hint = 0;              ///< Preferred guest load address; 0 lets the loader choose.
  bool apply_relocations = true;
  bool enforce_relro = true;           ///< Strip write from PT_GNU_RELRO after relocation.
  SymbolResolver resolve;
};

/// Reads a whole image from `fd` (seekable) and maps it into `mem`.
/// Returns nullopt and fills `err` with a named stage — a load failure must be diagnosable from the
/// log alone, since most of them will be seen on a phone and not at a desk.
std::optional<GuestModule> LoadFromFd(mem::GuestMemory& mem, int fd, const std::string& path,
                                      const LoadOptions&, std::string* err);

std::optional<GuestModule> LoadFromBytes(mem::GuestMemory& mem, const std::vector<std::uint8_t>& image,
                                         const std::string& path, const LoadOptions&, std::string* err);

/// True for \x7fELF with class 1 (32-bit) and machine EM_ARM — the gate the import pipeline uses to
/// decide whether an APK's `lib/armeabi-v7a` entry is loadable here at all.
bool LooksLikeAArch32Elf(const std::uint8_t* data, std::size_t len);

}  // namespace rb::loader
