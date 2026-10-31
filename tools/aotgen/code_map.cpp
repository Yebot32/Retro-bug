// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// ELF32/ARM reader and the six discovery passes behind `rb_aotgen` (AOT task 2). The passes run in the
// order the design fixes — exports, constructors, relocations, `.ARM.exidx`, veneer patterns, trace — and
// every hit is merged into one map keyed by file-relative offset. Order matters only for the architecture
// flag: a symbol's low bit is authoritative, while an exidx prel31 target is even for Thumb and ARM alike
// and therefore only supplies `kThumb` when no other source named the start.
#include "code_map.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace rb::aotgen {
namespace {

// ELF constants this reader needs. They are repeated rather than pulled from <elf.h> because the tool
// builds on Windows and macOS as well as Android, and the NDK header is not part of those hosts.
constexpr std::uint8_t kElfClass32 = 1;
constexpr std::uint8_t kElfData2Lsb = 1;
constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEtDyn = 3;
constexpr std::uint16_t kEmArm = 40;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPtArmExidx = 0x7000'0001;

constexpr std::uint32_t kShtNobits = 8;

constexpr std::uint32_t kSttFunc = 2;
constexpr std::uint16_t kShnUndef = 0;
constexpr std::uint16_t kShnXindex = 0xffff;

constexpr std::uint32_t kDtInit = 12;
constexpr std::uint32_t kDtSoname = 14;
constexpr std::uint32_t kDtInitArray = 25;
constexpr std::uint32_t kDtInitArraySz = 27;
constexpr std::uint32_t kDtNull = 0;

constexpr std::uint32_t kRArmAbs32 = 2;
constexpr std::uint32_t kRArmGlobDat = 21;
constexpr std::uint32_t kRArmRelative = 23;

constexpr std::uint32_t kExidxCantUnwind = 1;

/// No real section name comes close to this; the cap exists so a table whose strings never end cannot
/// make one lookup allocate more than a name-sized string (see `SectionName`).
constexpr std::size_t kMaxSectionName = 255;

struct Section {
  std::string name;
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint32_t addr = 0;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t entsize = 0;
};

struct Segment {
  std::uint32_t type = 0;
  std::uint32_t offset = 0;
  std::uint32_t vaddr = 0;
  std::uint32_t filesz = 0;
  std::uint32_t memsz = 0;
};

struct Image {
  const std::vector<std::uint8_t>* bytes = nullptr;
  std::vector<Section> sections;
  std::vector<Segment> segments;
  Section text;
  bool has_text = false;
};

struct Dynamic {
  std::uint32_t init = 0;
  std::uint32_t init_array = 0;
  std::uint32_t init_array_size = 0;
  std::uint32_t soname = 0;
};

using Merged = std::map<std::uint32_t, CodeMapEntry>;

bool Fail(std::string* err, std::string what) {
  if (err != nullptr) *err = std::move(what);
  return false;
}

std::uint16_t Le16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t Le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool InBounds(const Image& img, std::uint64_t offset, std::uint64_t length) {
  const std::uint64_t size = img.bytes->size();
  return offset <= size && length <= size - offset;
}

bool Read16(const Image& img, std::uint64_t offset, std::uint16_t* out) {
  if (!InBounds(img, offset, 2)) return false;
  *out = Le16(img.bytes->data() + offset);
  return true;
}

bool Read32(const Image& img, std::uint64_t offset, std::uint32_t* out) {
  if (!InBounds(img, offset, 4)) return false;
  *out = Le32(img.bytes->data() + offset);
  return true;
}

const Section* FindSection(const Image& img, std::string_view name) {
  for (const Section& s : img.sections) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

/// vaddr -> file offset through PT_LOAD, then the section table. The second path matters for a stripped
/// image with no program headers, and for `.dynamic` when it is found by section rather than by segment.
std::optional<std::uint32_t> FileOffset(const Image& img, std::uint32_t vaddr) {
  for (const Segment& p : img.segments) {
    if (p.type != kPtLoad) continue;
    if (vaddr >= p.vaddr && static_cast<std::uint64_t>(vaddr) <
                                static_cast<std::uint64_t>(p.vaddr) + p.filesz) {
      return p.offset + (vaddr - p.vaddr);
    }
  }
  for (const Section& s : img.sections) {
    if (s.type == kShtNobits || s.size == 0) continue;
    if (vaddr >= s.addr && static_cast<std::uint64_t>(vaddr) <
                               static_cast<std::uint64_t>(s.addr) + s.size) {
      return s.offset + (vaddr - s.addr);
    }
  }
  return std::nullopt;
}

bool ReadWordAt(const Image& img, std::uint32_t vaddr, std::uint32_t* out) {
  const std::optional<std::uint32_t> offset = FileOffset(img, vaddr);
  if (!offset.has_value()) return false;
  return Read32(img, *offset, out);
}

/// The name table is the only section whose contents are read before the section list is complete, so it
/// gets its own bounds-checked reader. A damaged name yields an empty name, which then fails the `.text`
/// lookup by name rather than reading out of bounds.
std::string SectionName(const Image& img, const Section& shstr, std::uint32_t name_offset) {
  if (name_offset >= shstr.size) return {};
  const std::uint64_t begin = static_cast<std::uint64_t>(shstr.offset) + name_offset;
  const std::uint64_t end = static_cast<std::uint64_t>(shstr.offset) + shstr.size;
  if (begin >= end || end > img.bytes->size()) return {};
  const auto* base = img.bytes->data() + begin;
  // A name is a NUL-terminated string and nothing else: without a terminator the table is damaged, so
  // the answer is empty rather than "the rest of the table". That distinction is load-bearing — this
  // reader is called once per section, so returning the remainder would allocate `shstr.size` bytes per
  // header, and a crafted image with 65,535 headers is hundreds of GB. The window is also capped at the
  // length no real section name reaches, so one overlong string cannot allocate the same way.
  const std::size_t room =
      std::min<std::size_t>(static_cast<std::size_t>(end - begin), kMaxSectionName + 1);
  const void* nul = std::memchr(base, '\0', room);
  if (nul == nullptr) return {};
  return std::string(reinterpret_cast<const char*>(base),
                     static_cast<const std::uint8_t*>(nul) - base);
}

bool ParseImage(const std::vector<std::uint8_t>& elf, Image* img, std::string* err) {
  img->bytes = &elf;
  if (elf.size() < 52) return Fail(err, "not an ELF file (shorter than an ELF32 header)");
  if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
    return Fail(err, "not an ELF file (bad magic)");
  }
  if (elf[4] != kElfClass32) {
    return Fail(err, elf[4] == 2 ? "ELF64 is not supported" : "not an ELF32 file");
  }
  if (elf[5] != kElfData2Lsb) return Fail(err, "big-endian ELF is not supported");

  const std::uint16_t type = Le16(elf.data() + 16);
  if (type != kEtDyn && type != kEtExec) {
    return Fail(err, "not a shared object or executable (e_type=" + std::to_string(type) + ")");
  }
  const std::uint16_t machine = Le16(elf.data() + 18);
  if (machine != kEmArm) {
    return Fail(err, "not an ARM ELF (e_machine=" + std::to_string(machine) + ")");
  }

  std::uint32_t shoff = 0;
  std::uint16_t shentsize = 0;
  std::uint16_t shnum16 = 0;
  std::uint16_t shstrndx16 = 0;
  std::uint32_t shnum = 0;
  std::uint32_t shstrndx = 0;
  Read32(*img, 32, &shoff);
  Read16(*img, 46, &shentsize);
  Read16(*img, 48, &shnum16);
  Read16(*img, 50, &shstrndx16);
  shnum = shnum16;
  shstrndx = shstrndx16;
  if (shoff == 0) return Fail(err, "ELF has no section headers");
  if (shentsize < 40) return Fail(err, "bad section header entry size");
  // Extended numbering puts the real count (and, if needed, the name-table index) in section 0.
  if (shnum == 0) {
    if (!InBounds(*img, shoff, 40)) return Fail(err, "truncated section header table");
    Read32(*img, shoff + 20, &shnum);
  }
  if (static_cast<std::uint64_t>(shoff) + static_cast<std::uint64_t>(shnum) * shentsize >
      elf.size()) {
    return Fail(err, "truncated section header table");
  }
  if (shstrndx == kShnXindex) {
    if (!InBounds(*img, shoff, 40)) return Fail(err, "truncated section header table");
    Read32(*img, shoff + 24, &shstrndx);
  }

  // No reserve(shnum): the count is a field from the file — up to 65,535, or 2^32 through extended
  // numbering — and a `Section` carries a string, so reserving it pays up front for a table the check
  // above only proved *fits*. Growth is geometric and costs nothing on the images this tool sees.
  for (std::uint32_t i = 0; i < shnum; ++i) {
    const std::uint64_t base = static_cast<std::uint64_t>(shoff) + i * shentsize;
    const auto* p = elf.data() + base;
    Section s;
    s.type = Le32(p + 4);
    s.flags = Le32(p + 8);
    s.addr = Le32(p + 12);
    s.offset = Le32(p + 16);
    s.size = Le32(p + 20);
    s.link = Le32(p + 24);
    s.entsize = Le32(p + 36);
    s.name.clear();
    img->sections.push_back(std::move(s));
  }
  if (shstrndx >= img->sections.size()) return Fail(err, "bad section-name table index");
  const Section& shstr = img->sections[shstrndx];
  if (shstr.size != 0 && !InBounds(*img, shstr.offset, shstr.size)) {
    return Fail(err, "truncated section-name table");
  }
  for (std::uint32_t i = 0; i < shnum; ++i) {
    const std::uint64_t base = static_cast<std::uint64_t>(shoff) + i * shentsize;
    img->sections[i].name = SectionName(*img, shstr, Le32(elf.data() + base));
  }

  const std::uint16_t phentsize = Le16(elf.data() + 42);
  const std::uint16_t phnum = Le16(elf.data() + 44);
  std::uint32_t phoff = 0;
  Read32(*img, 28, &phoff);
  if (phnum != 0) {
    if (phentsize < 32) return Fail(err, "bad program header entry size");
    if (static_cast<std::uint64_t>(phoff) + static_cast<std::uint64_t>(phnum) * phentsize >
        elf.size()) {
      return Fail(err, "truncated program header table");
    }
    img->segments.reserve(phnum);
    for (std::uint16_t i = 0; i < phnum; ++i) {
      const std::uint64_t base = static_cast<std::uint64_t>(phoff) + i * phentsize;
      const auto* p = elf.data() + base;
      Segment s;
      s.type = Le32(p + 0);
      s.offset = Le32(p + 4);
      s.vaddr = Le32(p + 8);
      s.filesz = Le32(p + 16);
      s.memsz = Le32(p + 20);
      img->segments.push_back(s);
    }
  }

  const Section* text = FindSection(*img, ".text");
  if (text == nullptr) return Fail(err, "missing .text section");
  if (text->size == 0) return Fail(err, "empty .text section");
  if (!InBounds(*img, text->offset, text->size)) return Fail(err, "truncated .text section");
  img->text = *text;
  img->has_text = true;
  return true;
}

bool ReadDynamic(const Image& img, Dynamic* dyn, std::string* err) {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  if (const Section* s = FindSection(img, ".dynamic"); s != nullptr) {
    offset = s->offset;
    size = s->size;
  } else {
    for (const Segment& p : img.segments) {
      if (p.type == kPtDynamic) {
        offset = p.offset;
        size = p.filesz;
        break;
      }
    }
  }
  if (size == 0) return true;
  if (!InBounds(img, offset, size)) return Fail(err, "truncated .dynamic");
  // u64 induction: `size` is a field from the file, so `i + 8` in u32 would wrap near 4 GB and the
  // reads past the last entry would be unchecked by the loop condition.
  for (std::uint64_t i = 0; i + 8 <= size; i += 8) {
    std::uint32_t tag = 0;
    std::uint32_t value = 0;
    Read32(img, static_cast<std::uint64_t>(offset) + i, &tag);
    Read32(img, static_cast<std::uint64_t>(offset) + i + 4, &value);
    if (tag == kDtNull) break;
    if (tag == kDtInit) dyn->init = value;
    if (tag == kDtInitArray) dyn->init_array = value;
    if (tag == kDtInitArraySz) dyn->init_array_size = value;
    if (tag == kDtSoname) dyn->soname = value;
  }
  return true;
}

bool AddressInText(const Image& img, std::uint32_t address) {
  const std::uint32_t aligned = address & ~1u;
  return aligned >= img.text.addr &&
         static_cast<std::uint64_t>(aligned) <
             static_cast<std::uint64_t>(img.text.addr) + img.text.size;
}

/// `size` ends up in the on-disk row's u16. A start that claims to run past the section is clipped: the
/// emitter decodes inside `.text`, so a larger size would only invite a read past the buffer. Both a
/// symbol's `st_size` and a trace's size go through here so the cap cannot be forgotten on one path.
void WidenSize(const Image& img, std::uint32_t off, std::uint32_t size, CodeMapEntry* entry) {
  if (size == 0) return;
  const std::uint32_t capped =
      std::min({size, static_cast<std::uint32_t>(0xffff), img.text.size - off});
  if (capped > entry->size) entry->size = static_cast<std::uint16_t>(capped);
}

/// Merge one discovery. `arch_known` distinguishes "the caller saw a Thumb bit" from `.ARM.exidx`,
/// whose prel31 target has no such bit: an exidx-only start defaults to Thumb (every function in these
/// two corpora that exidx names but dynsym does not is Thumb), but it never overwrites an explicit ARM.
void AddStart(const Image& img, Merged* merged, std::uint32_t address, std::uint8_t source,
              bool arch_known, bool thumb, std::uint32_t size) {
  if (!AddressInText(img, address)) return;
  const std::uint32_t off = (address & ~1u) - img.text.addr;
  CodeMapEntry& entry = (*merged)[off];
  entry.off = off;
  entry.sources |= source;
  if (arch_known) {
    entry.flags = static_cast<std::uint8_t>(thumb ? kThumb : kArm);
  } else if ((entry.flags & (kThumb | kArm)) == 0) {
    entry.flags = kThumb;
  }
  WidenSize(img, off, size, &entry);
}

bool AddSymbols(const Image& img, Merged* merged, std::string* err) {
  for (const char* name : {".dynsym", ".symtab"}) {
    const Section* symtab = FindSection(img, name);
    if (symtab == nullptr) continue;
    if (symtab->size == 0) continue;
    const std::uint32_t entsize = symtab->entsize != 0 ? symtab->entsize : 16;
    if (entsize < 16) return Fail(err, std::string("bad ") + name + " entry size");
    if (!InBounds(img, symtab->offset, symtab->size)) {
      return Fail(err, std::string("truncated ") + name);
    }
    const std::uint32_t count = symtab->size / entsize;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint64_t base = static_cast<std::uint64_t>(symtab->offset) + i * entsize;
      std::uint32_t value = 0;
      std::uint32_t size = 0;
      std::uint16_t shndx = 0;
      Read32(img, base + 4, &value);
      Read32(img, base + 8, &size);
      Read16(img, base + 14, &shndx);
      const std::uint8_t info = (*img.bytes)[static_cast<std::size_t>(base + 12)];
      if ((info & 0x0f) != kSttFunc || shndx == kShnUndef) continue;
      if (!AddressInText(img, value)) continue;
      AddStart(img, merged, value, kFromExport, true, (value & 1u) != 0, size);
    }
  }
  return true;
}

/// A constructor table is only walkable when it is non-empty *and* the file holds all of it. Every other
/// state — no `DT_INIT_ARRAY`, no `DT_INIT_ARRAYSZ`, a size tag the file contradicts — is damage the
/// section header can still describe, so the caller falls back rather than aborting.
bool InitTableFits(const Image& img, std::uint32_t offset, std::uint32_t count) {
  return count != 0 && InBounds(img, offset, static_cast<std::uint64_t>(count) * 4);
}

bool AddInit(const Image& img, const Dynamic& dyn, Merged* merged, std::string* err) {
  if (dyn.init != 0) AddStart(img, merged, dyn.init, kFromInit, true, (dyn.init & 1u) != 0, 0);

  std::uint32_t array_offset = 0;
  std::uint32_t count = 0;
  if (dyn.init_array != 0) {
    if (const std::optional<std::uint32_t> mapped = FileOffset(img, dyn.init_array);
        mapped.has_value()) {
      array_offset = *mapped;
      count = dyn.init_array_size / 4;
    }
  }
  if (!InitTableFits(img, array_offset, count)) {
    // A missing or zero DT_INIT_ARRAYSZ, and a size that runs past the file, are both common in old
    // packed images; the section header names the same table, so use it rather than silently dropping
    // every constructor (or refusing the image over a tag the section survives).
    if (const Section* array = FindSection(img, ".init_array"); array != nullptr) {
      array_offset = array->offset;
      count = array->size / 4;
    }
  }
  if (count == 0) return true;
  if (!InBounds(img, array_offset, static_cast<std::uint64_t>(count) * 4)) {
    return Fail(err, "truncated .init_array");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t value = 0;
    if (!Read32(img, static_cast<std::uint64_t>(array_offset) + i * 4, &value)) break;
    if (value == 0) continue;
    AddStart(img, merged, value, kFromInit, true, (value & 1u) != 0, 0);
  }
  return true;
}

bool SymbolValue(const Image& img, std::uint32_t index, std::uint32_t* value) {
  const Section* symtab = FindSection(img, ".dynsym");
  if (symtab == nullptr || symtab->entsize < 16) return false;
  const std::uint32_t count = symtab->size / (symtab->entsize != 0 ? symtab->entsize : 16);
  if (index >= count) return false;
  return Read32(img, static_cast<std::uint64_t>(symtab->offset) + index * symtab->entsize + 4,
                value);
}

bool AddRelocations(const Image& img, Merged* merged, std::string* err) {
  for (const char* name : {".rel.dyn", ".rel.plt"}) {
    const Section* rel = FindSection(img, name);
    if (rel == nullptr) continue;
    if (rel->size == 0) continue;
    if (!InBounds(img, rel->offset, rel->size)) {
      return Fail(err, std::string("truncated ") + name);
    }
    const std::uint32_t count = rel->size / 8;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint64_t base = static_cast<std::uint64_t>(rel->offset) + i * 8;
      std::uint32_t r_offset = 0;
      std::uint32_t r_info = 0;
      Read32(img, base, &r_offset);
      Read32(img, base + 4, &r_info);
      const std::uint32_t type = r_info & 0xffu;
      const std::uint32_t symbol = r_info >> 8;
      if (type != kRArmAbs32 && type != kRArmGlobDat && type != kRArmRelative) continue;

      // REL (there is no RELA on these targets): the addend is the word at r_offset. A slot in .bss has
      // no file bytes, and a GLOB_DAT slot is zero before relocation.
      std::uint32_t addend = 0;
      const bool have_addend = ReadWordAt(img, r_offset, &addend);
      std::uint32_t target = 0;
      bool have_target = false;
      if (type == kRArmGlobDat) {
        // Bionic ignores a GLOB_DAT addend (docs/research/02-android-runtime-abi-surface.md: "the
        // document specifies that R_ARM_GLOB_DAT has an addend, but Bionic [does not]"). The resolved
        // target is therefore the symbol's value; all 701 of libgame's in-text GLOB_DAT slots are zero
        // in the file, so reading only the slot would miss every one of them.
        if (have_addend && addend != 0) {
          target = addend;
          have_target = true;
        } else {
          have_target = SymbolValue(img, symbol, &target);
        }
      } else if (have_addend) {
        target = addend;
        have_target = true;
      }
      if (have_target && AddressInText(img, target)) {
        AddStart(img, merged, target, kFromReloc, true, (target & 1u) != 0, 0);
      }
    }
  }
  return true;
}

bool AddExidx(const Image& img, Merged* merged, std::string* err) {
  std::uint32_t table_addr = 0;
  std::uint32_t table_offset = 0;
  std::uint32_t table_size = 0;
  if (const Section* exidx = FindSection(img, ".ARM.exidx"); exidx != nullptr) {
    table_addr = exidx->addr;
    table_offset = exidx->offset;
    table_size = exidx->size;
  } else {
    for (const Segment& p : img.segments) {
      if (p.type == kPtArmExidx) {
        table_addr = p.vaddr;
        table_offset = p.offset;
        table_size = p.filesz;
        break;
      }
    }
  }
  if (table_size == 0) return true;
  if (!InBounds(img, table_offset, table_size)) return Fail(err, "truncated .ARM.exidx");

  const std::uint32_t count = table_size / 8;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint64_t base = static_cast<std::uint64_t>(table_offset) + i * 8;
    std::uint32_t word = 0;
    Read32(img, base, &word);
    if (word == kExidxCantUnwind) continue;
    // prel31: sign-extend bit 30 into the high bits, then add the address of the word itself.
    std::int32_t delta = static_cast<std::int32_t>(word << 1) >> 1;
    const std::uint32_t place = table_addr + i * 8;
    const std::uint32_t start = place + static_cast<std::uint32_t>(delta);
    AddStart(img, merged, start, kFromExidx, false, true, 0);
  }
  return true;
}

/// The two linker trampolines GCC/ld emits inside `.text` when a branch cannot reach its target. They are
/// not symbols and not separate exidx entries, but a call can land on them, so they are starts too.
void AddVeneers(const Image& img, Merged* merged) {
  const std::uint8_t* text = img.bytes->data() + img.text.offset;
  // u64 induction, as in the dynamic walk: `.text` is a u32 from the file, so `i + 12` in u32 would
  // wrap on an image that claims ~4 GB of code and read past the buffer instead of stopping.
  const std::uint64_t length = img.text.size;

  // Thumb: movw r12,#lo; movt r12,#hi; add r12,pc; bx r12 (12 bytes). The immediate is deliberately
  // not decoded — the stub's *own* address is the start; its target is usually a .plt slot outside .text.
  for (std::uint64_t i = 0; i + 12 <= length; i += 2) {
    const std::uint16_t h0 = Le16(text + i);
    const std::uint16_t h1 = Le16(text + i + 2);
    const std::uint16_t h2 = Le16(text + i + 4);
    const std::uint16_t h3 = Le16(text + i + 6);
    if ((h0 & 0xfbf0u) != 0xf240u || ((h1 >> 8) & 0x0fu) != 12u) continue;
    if ((h2 & 0xfbf0u) != 0xf2c0u || ((h3 >> 8) & 0x0fu) != 12u) continue;
    if (Le16(text + i + 8) != 0x44fcu || Le16(text + i + 10) != 0x4760u) continue;
    AddStart(img, merged, img.text.addr + static_cast<std::uint32_t>(i), kFromVeneer, true, true, 0);
  }

  // ARM: ldr r12,[pc,#8]; add r12,pc,r12; <one word>; bx r12; .word (20 bytes). The third word is the
  // usual `sub r0,r0,#8` stack adjustment and differs between call sites, so it is not constrained.
  for (std::uint64_t i = 0; i + 20 <= length; i += 4) {
    if (Le32(text + i) != 0xe59fc008u || Le32(text + i + 4) != 0xe08fc00cu) continue;
    if (Le32(text + i + 12) != 0xe12fff1cu) continue;
    AddStart(img, merged, img.text.addr + static_cast<std::uint32_t>(i), kFromVeneer, true, false, 0);
  }

  // The four-word variant: ldr r12,[pc,#4]; add r12,pc,r12; bx r12; .word. Both end-of-text stubs in
  // libdlgl are this shape, which is why the scan is separate rather than a shorter copy of the loop.
  for (std::uint64_t i = 0; i + 16 <= length; i += 4) {
    if (Le32(text + i) != 0xe59fc004u || Le32(text + i + 4) != 0xe08fc00cu) continue;
    if (Le32(text + i + 8) != 0xe12fff1cu) continue;
    AddStart(img, merged, img.text.addr + static_cast<std::uint32_t>(i), kFromVeneer, true, false, 0);
  }
}

/// A trace is written by a different producer (AOT task 4), so the map's invariants are re-established
/// here rather than trusted: `off` is a text-relative (even) file offset inside `.text`, `flags` carries
/// exactly one ISA bit, and `size` obeys the same 0xffff / end-of-`.text` clamp as a symbol.
void MergeTrace(const Image& img, const CodeMap* trace, Merged* merged) {
  if (trace == nullptr) return;
  for (const CodeMapEntry& source : trace->entries) {
    // The Thumb bit never survives into the map's unit: `off` is `(vaddr & ~1) - text_vaddr`. A producer
    // that left the bit in is masked rather than dropped, since the start it names is still real.
    const std::uint32_t off = source.off & ~1u;
    if (off >= img.text.size) continue;
    CodeMapEntry& entry = (*merged)[off];
    entry.off = off;
    entry.sources |= kFromTrace;
    WidenSize(img, off, source.size, &entry);
    const std::uint8_t arch = source.flags & (kThumb | kArm);
    if (arch == kThumb || arch == kArm) {
      entry.flags = arch;
    } else if ((entry.flags & (kThumb | kArm)) == 0) {
      // Same default as an exidx-only start: a trace without an ISA still names a Thumb start, and every
      // entry must carry exactly one ISA bit.
      entry.flags = kThumb;
    }
  }
}

std::string ReadSoname(const Image& img, const Dynamic& dyn) {
  if (dyn.soname == 0) return {};
  const Section* dynstr = FindSection(img, ".dynstr");
  if (dynstr == nullptr || dyn.soname >= dynstr->size) return {};
  const std::uint64_t begin = static_cast<std::uint64_t>(dynstr->offset) + dyn.soname;
  const std::uint64_t end = static_cast<std::uint64_t>(dynstr->offset) + dynstr->size;
  if (begin >= end || end > img.bytes->size()) return {};
  const auto* base = img.bytes->data() + begin;
  const void* nul = std::memchr(base, '\0', static_cast<std::size_t>(end - begin));
  if (nul == nullptr) return {};
  const std::size_t length = static_cast<const std::uint8_t*>(nul) - base;
  return std::string(reinterpret_cast<const char*>(base), length);
}

}  // namespace

CodeMap DiscoverCodeMap(const std::vector<std::uint8_t>& elf, const CodeMap* trace,
                        std::string* err) {
  if (err != nullptr) err->clear();
  CodeMap map{};
  Image img;
  if (!ParseImage(elf, &img, err)) return map;

  Dynamic dynamic;
  if (!ReadDynamic(img, &dynamic, err)) return map;

  Merged merged;
  if (!AddSymbols(img, &merged, err)) return map;
  if (!AddInit(img, dynamic, &merged, err)) return map;
  if (!AddRelocations(img, &merged, err)) return map;
  if (!AddExidx(img, &merged, err)) return map;
  AddVeneers(img, &merged);
  MergeTrace(img, trace, &merged);

  map.soname = ReadSoname(img, dynamic);
  map.text_off = img.text.offset;
  map.text_len = img.text.size;
  map.text_vaddr = img.text.addr;
  map.entries.reserve(merged.size());
  for (const auto& [off, entry] : merged) {
    (void)off;
    map.entries.push_back(entry);
  }
  return map;
}

std::size_t CountSource(const CodeMap& map, std::uint8_t source) {
  std::size_t count = 0;
  for (const CodeMapEntry& entry : map.entries) {
    if ((entry.sources & source) != 0) ++count;
  }
  return count;
}

}  // namespace rb::aotgen
