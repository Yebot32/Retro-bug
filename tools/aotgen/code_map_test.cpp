// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::aotgen::DiscoverCodeMap` against the two real game libraries the AOT design measured. This is a
// gating test on the device lane because the corpus is extracted game bytes and cannot live in the repo;
// when they are absent (the CI host lanes) it prints one line and exits 0.
//
// The bounds are the research table's, not guesses: libgame's union of symbol and exidx starts is 16,789
// and libdlgl's is 2,788. The veneer pass is measured separately, because its 1,633 Thumb stubs in
// libgame are *not* among those starts: they sit after the functions' `pop {pc}` and exidx covers them
// only as the tail of the preceding interval. A map that merges them therefore carries 18,565 entries,
// not 16,789 — the upper bound below is the only one widened for that measured fact (see the task report).
//
// Everything outside the two corpus functions runs on crafted byte buffers, so the refusals, the
// constructor fallback and the trace merge are checked in the lanes that have no game bytes at all.
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "code_map.h"

namespace {

using namespace rb::aotgen;

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL  %s\n", what.c_str());
  }
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const std::streamsize size = in.tellg();
  if (size <= 0) return {};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!in) return {};
  return bytes;
}

void Put16(std::vector<std::uint8_t>* bytes, std::size_t at, std::uint16_t value) {
  (*bytes)[at] = static_cast<std::uint8_t>(value);
  (*bytes)[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(std::vector<std::uint8_t>* bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> Header(std::uint8_t elf_class, std::uint8_t data,
                                 std::uint16_t machine, std::uint16_t type) {
  std::vector<std::uint8_t> bytes(52, 0);
  bytes[0] = 0x7f;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = elf_class;
  bytes[5] = data;
  bytes[6] = 1;
  bytes[16] = static_cast<std::uint8_t>(type & 0xff);
  bytes[17] = static_cast<std::uint8_t>(type >> 8);
  bytes[18] = static_cast<std::uint8_t>(machine & 0xff);
  bytes[19] = static_cast<std::uint8_t>(machine >> 8);
  return bytes;
}

/// Little-endian words, the only encoding a `.dynamic` or `.init_array` body needs here.
std::vector<std::uint8_t> Words(std::initializer_list<std::uint32_t> words) {
  std::vector<std::uint8_t> data(words.size() * 4, 0);
  std::size_t at = 0;
  for (std::uint32_t word : words) {
    Put32(&data, at, word);
    at += 4;
  }
  return data;
}

/// One section of a synthetic image. `declared_size` overrides `sh_size` (0 means "the data's size") and
/// `terminate_name` drops the name's NUL, which is how a test damages exactly one field of the table.
struct SynthSection {
  std::string name;
  std::uint32_t type = 1;  // SHT_PROGBITS
  std::uint32_t flags = 0x2;
  std::uint32_t addr = 0;
  std::uint32_t entsize = 0;
  std::uint32_t declared_size = 0;
  bool terminate_name = true;
  std::vector<std::uint8_t> data;
};

struct SynthImage {
  std::vector<std::uint8_t> bytes;
  std::uint32_t shoff = 0;
  std::uint32_t shnum = 0;  // counting the null header and `.shstrtab`

  /// File offset of section header `index`; 0 is the null header and `shnum - 1` is `.shstrtab`.
  std::uint32_t Header(std::size_t index) const {
    return shoff + 40 * static_cast<std::uint32_t>(index);
  }
};

SynthSection TextSection(std::uint32_t length, std::uint32_t addr = 0x1000) {
  SynthSection text;
  text.name = ".text";
  text.flags = 0x6;  // SHF_ALLOC | SHF_EXECINSTR
  text.addr = addr;
  text.data.assign(length, 0);
  return text;
}

/// Assemble an image from nothing but section headers: the ELF header, each section's bytes at 4-aligned
/// offsets, a name table built from `sections`, then the table itself. There are no program headers, so
/// every vaddr -> file mapping has to go through the section table — the same path a stripped image
/// takes. Section header `i + 1` describes `sections[i]`; the returned offsets let a test patch one
/// header field afterwards.
SynthImage Synth(const std::vector<SynthSection>& sections) {
  std::string names(1, '\0');
  std::vector<std::uint32_t> name_at;
  for (const SynthSection& s : sections) {
    name_at.push_back(static_cast<std::uint32_t>(names.size()));
    names += s.name;
    if (s.terminate_name) names.push_back('\0');
  }
  const std::uint32_t shstr_name = static_cast<std::uint32_t>(names.size());
  names += ".shstrtab";
  names.push_back('\0');

  std::vector<std::uint8_t> bytes = Header(1, 1, 40, 3);
  std::vector<std::uint32_t> offset(sections.size(), 0);
  for (std::size_t i = 0; i < sections.size(); ++i) {
    while (bytes.size() % 4 != 0) bytes.push_back(0);
    offset[i] = static_cast<std::uint32_t>(bytes.size());
    bytes.insert(bytes.end(), sections[i].data.begin(), sections[i].data.end());
  }
  while (bytes.size() % 4 != 0) bytes.push_back(0);
  const std::uint32_t shstr_offset = static_cast<std::uint32_t>(bytes.size());
  bytes.insert(bytes.end(), names.begin(), names.end());
  while (bytes.size() % 4 != 0) bytes.push_back(0);

  const std::uint32_t shoff = static_cast<std::uint32_t>(bytes.size());
  const std::uint32_t shnum = static_cast<std::uint32_t>(sections.size()) + 2;
  bytes.resize(bytes.size() + 40 * shnum, 0);  // section 0 stays the all-zero null header
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const std::size_t at = shoff + 40 * (i + 1);
    Put32(&bytes, at + 0, name_at[i]);
    Put32(&bytes, at + 4, sections[i].type);
    Put32(&bytes, at + 8, sections[i].flags);
    Put32(&bytes, at + 12, sections[i].addr);
    Put32(&bytes, at + 16, offset[i]);
    Put32(&bytes, at + 20, sections[i].declared_size != 0
                               ? sections[i].declared_size
                               : static_cast<std::uint32_t>(sections[i].data.size()));
    Put32(&bytes, at + 36, sections[i].entsize);
  }
  const std::size_t shstr_at = shoff + 40 * (shnum - 1);
  Put32(&bytes, shstr_at + 0, shstr_name);
  Put32(&bytes, shstr_at + 4, 3);  // SHT_STRTAB
  Put32(&bytes, shstr_at + 16, shstr_offset);
  Put32(&bytes, shstr_at + 20, static_cast<std::uint32_t>(names.size()));

  Put32(&bytes, 32, shoff);
  Put16(&bytes, 46, 40);
  Put16(&bytes, 48, static_cast<std::uint16_t>(shnum));
  Put16(&bytes, 50, static_cast<std::uint16_t>(shnum - 1));

  SynthImage image;
  image.bytes = std::move(bytes);
  image.shoff = shoff;
  image.shnum = shnum;
  return image;
}

/// Every refusal names its reason; the test checks the discriminating phrase rather than the whole
/// sentence so a wording change does not masquerade as a reader bug.
void ExpectRefusal(const std::vector<std::uint8_t>& bytes, const char* phrase,
                   const std::string& what) {
  std::string err;
  const CodeMap map = DiscoverCodeMap(bytes, nullptr, &err);
  Check(err.find(phrase) != std::string::npos, what + " (" + (err.empty() ? "no error" : err) + ")");
  Check(map.entries.empty(), what + " returns no entries");
}

void HeaderRefusals() {
  ExpectRefusal({}, "not an ELF", "empty input");
  ExpectRefusal(Header(2, 1, 40, 3), "ELF64 is not supported", "ELF64 is refused by name");
  ExpectRefusal(Header(1, 2, 40, 3), "big-endian", "big-endian ELF is refused by name");
  ExpectRefusal(Header(1, 1, 3, 3), "not an ARM ELF", "x86 ELF is refused by name");
  ExpectRefusal(Header(1, 1, 40, 1), "not a shared object", "ET_REL is refused by name");
  ExpectRefusal(Header(1, 1, 40, 3), "no section headers", "ELF without section headers");

  // A section table that claims one more header than the file holds: the reader must not walk past the
  // buffer.
  std::vector<std::uint8_t> truncated = Header(1, 1, 40, 3);
  const std::uint32_t shoff = 52;
  const std::uint16_t shentsize = 40;
  const std::uint16_t shnum = 2;
  Put32(&truncated, 32, shoff);
  Put16(&truncated, 46, shentsize);
  Put16(&truncated, 48, shnum);
  ExpectRefusal(truncated, "truncated section header table", "section table runs past EOF");

  // One valid but empty section: no `.text` anywhere, which is the loader's own refusal to name a code
  // map for an image with no code section.
  std::vector<std::uint8_t> no_text = Header(1, 1, 40, 3);
  no_text.resize(52 + 40, 0);
  Put32(&no_text, 32, shoff);
  Put16(&no_text, 46, shentsize);
  Put16(&no_text, 48, 1);
  ExpectRefusal(no_text, "missing .text", "ELF without .text");
}

void NameTableRefusals() {
  std::vector<SynthSection> sections;
  sections.push_back(TextSection(64));
  const SynthImage plain = Synth(sections);

  // The name-table index is a header field nothing else validates.
  SynthImage bad_index = plain;
  Put16(&bad_index.bytes, 50, 9);
  ExpectRefusal(bad_index.bytes, "bad section-name table index", "section-name index past the table");

  // A name table whose declared size runs past the file must be refused before any of its bytes are
  // read as strings.
  SynthImage bad_table = plain;
  Put32(&bad_table.bytes, bad_table.Header(bad_table.shnum - 1) + 20, 0xffff);
  ExpectRefusal(bad_table.bytes, "truncated section-name table", "section-name table past EOF");

  // A name with no terminator yields an empty name, not "the rest of the table": that reading is what
  // turns one crafted header into an allocation the size of the whole name table, once per section. The
  // follower is longer than the reader's cap, so no NUL is in reach of `.text`'s name.
  std::vector<SynthSection> unterminated;
  unterminated.push_back(TextSection(64));
  unterminated.back().terminate_name = false;
  SynthSection long_name;
  long_name.name = std::string(300, 'x');
  unterminated.push_back(long_name);
  ExpectRefusal(Synth(unterminated).bytes, "missing .text", "unterminated section name yields no name");
}

void DynamicWalk() {
  // DT_INIT_ARRAYSZ that runs past the file is a damaged tag, not a reason to drop every constructor:
  // `.init_array` names the same table and must be used instead. `.init_array` sits at vaddr 0x2000 while
  // its file offset is near the start of the image, so the tag's vaddr has to be mapped through the
  // section table rather than reused as an offset.
  std::vector<SynthSection> sections;
  sections.push_back(TextSection(64));
  SynthSection array;
  array.name = ".init_array";
  array.flags = 0x3;
  array.addr = 0x2000;
  array.data = Words({0x1001});  // .text + 1: a Thumb constructor
  sections.push_back(array);
  SynthSection dynamic;
  dynamic.name = ".dynamic";
  dynamic.flags = 0x3;
  dynamic.data = Words({25, 0x2000, 27, 0x1000, 0, 0});  // INIT_ARRAY, INIT_ARRAYSZ, NULL
  sections.push_back(dynamic);

  std::string err;
  const CodeMap map = DiscoverCodeMap(Synth(sections).bytes, nullptr, &err);
  Check(err.empty(), "an oversized DT_INIT_ARRAYSZ falls back to .init_array: " + err);
  if (err.empty()) {
    Check(map.entries.size() == 1 && map.entries[0].off == 0,
          "the fallback found the single constructor at .text+0");
    Check(CountSource(map, kFromInit) == 1 && map.entries[0].flags == kThumb,
          "the fallback constructor keeps its Thumb bit");
  }

  // The same damaged size with nothing to fall back to: the tag points into `.text` (so it maps) and
  // there is no `.init_array` header, which is a table the file really does not hold.
  std::vector<SynthSection> no_fallback;
  no_fallback.push_back(TextSection(64));
  SynthSection broken;
  broken.name = ".dynamic";
  broken.flags = 0x3;
  broken.data = Words({25, 0x1000, 27, 0x1000, 0, 0});
  no_fallback.push_back(broken);
  ExpectRefusal(Synth(no_fallback).bytes, "truncated .init_array",
                "init_array past EOF with no section to fall back to");

  // A `.dynamic` section that claims more bytes than the file holds. The walk reads it entry by entry,
  // so it has to be refused up front rather than at the first entry past the end.
  std::vector<SynthSection> short_dynamic;
  short_dynamic.push_back(TextSection(64));
  SynthSection cut;
  cut.name = ".dynamic";
  cut.flags = 0x3;
  cut.declared_size = 0x1000;
  cut.data = Words({0, 0});
  short_dynamic.push_back(cut);
  ExpectRefusal(Synth(short_dynamic).bytes, "truncated .dynamic", "dynamic section past EOF");
}

const CodeMapEntry* At(const CodeMap& map, std::uint32_t off) {
  for (const CodeMapEntry& entry : map.entries) {
    if (entry.off == off) return &entry;
  }
  return nullptr;
}

/// The interface's own invariant: a start is Thumb or ARM, never both and never neither, and it exists
/// for at least one source.
bool OneIsaAndASource(const CodeMap& map) {
  for (const CodeMapEntry& entry : map.entries) {
    const std::uint8_t isa = entry.flags & (kThumb | kArm);
    if ((isa != kThumb && isa != kArm) || entry.sources == 0) return false;
  }
  return true;
}

void TraceMerge() {
  // `.text` lands at file offset 52 but vaddr 0x1000, so an offset produced from the address rather than
  // the file would be outside the map. It is a full 128 KiB so a 0xffff trace size is not clipped by the
  // end of the section — the clip has its own start below.
  std::vector<SynthSection> sections;
  sections.push_back(TextSection(0x20000));
  const SynthImage image = Synth(sections);

  CodeMap trace{};
  const auto add = [&trace](std::uint32_t off, std::uint16_t size, std::uint8_t flags) {
    CodeMapEntry entry{};
    entry.off = off;
    entry.size = size;
    entry.flags = flags;
    trace.entries.push_back(entry);
  };
  add(0x00, 0, 0);          // no ISA at all
  add(0x04, 0, kArm);       // explicit ARM
  add(0x04, 0, 0);          // the same start again, still without an ISA: ARM has to survive
  add(0x08, 0xffff, kThumb);
  add(0x0d, 0, kArm);       // odd: the Thumb bit is masked out, so this is start 0x0c
  add(0x1fffc, 0xffff, 0);  // clipped to the four bytes left in `.text`
  add(0x20000, 0, 0);       // == text_len: not a start
  add(0x21000, 0, 0);       // a vaddr past the section: not a start either

  std::string err;
  const CodeMap map = DiscoverCodeMap(image.bytes, &trace, &err);
  Check(err.empty(), "trace merge parses: " + err);
  if (!err.empty()) return;

  Check(map.text_off == 52 && map.text_len == 0x20000, "synthetic .text is located by file offset");
  Check(map.entries.size() == 5, "trace merge keeps exactly the five in-.text starts");
  Check(At(map, 0x00) != nullptr && At(map, 0x00)->flags == kThumb,
        "a trace start without an ISA defaults to Thumb");
  Check(At(map, 0x04) != nullptr && At(map, 0x04)->flags == kArm,
        "an explicit ARM start is kept, and an ISA-less repeat does not clear it");
  Check(At(map, 0x08) != nullptr && At(map, 0x08)->size == 0xffff,
        "a trace size is stored capped at 0xffff");
  Check(At(map, 0x0c) != nullptr && At(map, 0x0d) == nullptr,
        "an odd trace offset is masked to its even start");
  Check(At(map, 0x1fffc) != nullptr && At(map, 0x1fffc)->size == 4,
        "a trace size is clipped at the end of .text");
  Check(At(map, 0x20000) == nullptr && At(map, 0x21000) == nullptr,
        "trace offsets outside .text are dropped");
  Check(CountSource(map, kFromTrace) == 5, "every surviving trace start carries kFromTrace");
  Check(OneIsaAndASource(map), "every merged trace entry has one ISA and one source");
}

void Summary(const char* label, const CodeMap& map) {
  std::printf("aotgen: %s starts %zu (export %zu, init %zu, reloc %zu, exidx %zu, veneer %zu)\n",
              label, map.entries.size(), CountSource(map, kFromExport),
              CountSource(map, kFromInit), CountSource(map, kFromReloc),
              CountSource(map, kFromExidx), CountSource(map, kFromVeneer));
}

bool NearPage(std::uint64_t actual, std::uint64_t kib) {
  const std::uint64_t expected = kib * 1024;
  return actual > expected ? actual - expected <= 4096 : expected - actual <= 4096;
}

bool StrictlyOrdered(const CodeMap& map) {
  for (std::size_t i = 1; i < map.entries.size(); ++i) {
    if (map.entries[i].off <= map.entries[i - 1].off) return false;
  }
  return true;
}

/// Offsets are file-relative to `.text`, so every start -- and every start plus its symbol size -- has to
/// land inside the section. This is the property a regression to vaddr arithmetic breaks, and the reason
/// it is checked per image rather than by a sample.
bool AllOffsetsInText(const CodeMap& map) {
  for (const CodeMapEntry& entry : map.entries) {
    if (entry.off >= map.text_len) return false;
    if (static_cast<std::uint64_t>(entry.off) + entry.size > map.text_len) return false;
  }
  return true;
}

std::uint16_t LargestSize(const CodeMap& map) {
  std::uint16_t largest = 0;
  for (const CodeMapEntry& entry : map.entries) {
    if (entry.size > largest) largest = entry.size;
  }
  return largest;
}

void LibGame(const std::vector<std::uint8_t>& bytes) {
  std::string err;
  const CodeMap map = DiscoverCodeMap(bytes, nullptr, &err);
  Check(err.empty(), "libgame parses: " + err);
  if (!err.empty()) return;

  Check(!map.entries.empty(), "libgame has starts");
  Check(map.entries.size() >= 16000, "libgame starts >= 16000");
  // 16,789 is dynsym ∪ exidx; the 1,633 veneer stubs are additional starts, so the merged map is
  // 18,565. The upper bound keeps the "no runaway merge" intent without contradicting that source.
  Check(map.entries.size() <= 19000, "libgame starts <= 19000 (16,789 + 1,633 veneers)");
  Check(CountSource(map, kFromExidx) >= 14000, "libgame exidx is the main source");
  Check(CountSource(map, kFromVeneer) >= 1500, "libgame veneer scan found the movw/movt stubs");
  Check(CountSource(map, kFromExport) >= 14000, "libgame exports are present");
  Check(CountSource(map, kFromReloc) >= 1000, "libgame relocations reach code");
  Check(CountSource(map, kFromInit) >= 40, "libgame constructors are present");
  Check(map.soname == "libgame.so", "libgame soname comes from DT_SONAME");
  Check(map.text_off == 0x24d0f0, "libgame text_off is the .text file offset");
  Check(NearPage(map.text_len, 1931), "libgame text_len is 1,931 KB ± one page");
  Check(static_cast<std::uint64_t>(map.text_off) + map.text_len <= bytes.size(),
        "libgame text lies inside the file");
  Check(AllOffsetsInText(map), "every libgame start and size lies inside .text");
  Check(LargestSize(map) > 0, "libgame symbol sizes were read");
  Check(StrictlyOrdered(map), "libgame entries are strictly ordered by offset");
  Check(OneIsaAndASource(map), "every libgame entry has one ISA and one source");
  Check(CountSource(map, 0) == 0, "CountSource(0) is zero");
  Summary("libgame", map);
}

void LibDlgl(const std::vector<std::uint8_t>& bytes) {
  std::string err;
  const CodeMap map = DiscoverCodeMap(bytes, nullptr, &err);
  Check(err.empty(), "libdlgl parses: " + err);
  if (!err.empty()) return;

  Check(map.entries.size() >= 2600, "libdlgl starts >= 2600");
  Check(CountSource(map, kFromExidx) >= 2000, "libdlgl exidx is the main source");
  Check(CountSource(map, kFromExport) >= 1800, "libdlgl exports are present");
  Check(CountSource(map, kFromVeneer) >= 4, "libdlgl ARM stubs were found");
  Check(map.soname == "libdlgl.so", "libdlgl soname comes from DT_SONAME");
  Check(map.text_off == 0x2a680, "libdlgl text_off is the .text file offset");
  Check(NearPage(map.text_len, 364), "libdlgl text_len is 364 KB ± one page");
  Check(static_cast<std::uint64_t>(map.text_off) + map.text_len <= bytes.size(),
        "libdlgl text lies inside the file");
  Check(AllOffsetsInText(map), "every libdlgl start and size lies inside .text");
  Check(LargestSize(map) > 0, "libdlgl symbol sizes were read");
  Check(StrictlyOrdered(map), "libdlgl entries are strictly ordered by offset");
  Check(OneIsaAndASource(map), "every libdlgl entry has one ISA and one source");
  Summary("libdlgl", map);
}

}  // namespace

int main(int argc, char** argv) {
  HeaderRefusals();
  NameTableRefusals();
  DynamicWalk();
  TraceMerge();

  const std::string game_path =
      argc > 1 ? argv[1] : "build/br-inspect/lib/armeabi-v7a/libgame.so";
  const std::string dlgl_path =
      argc > 2 ? argv[2] : "build/kk-inspect/lib/armeabi/libdlgl.so";

  const std::vector<std::uint8_t> game = ReadFile(game_path);
  if (game.empty()) {
    std::printf("# no libgame.so — skipped\n");
    // The synthetic checks above are the only ones that ran; a failure there must still fail the lane
    // rather than being hidden by the missing corpus.
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
  }
  LibGame(game);

  const std::vector<std::uint8_t> dlgl = ReadFile(dlgl_path);
  if (dlgl.empty()) {
    std::printf("# no libdlgl.so — skipped\n");
  } else {
    LibDlgl(dlgl);
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
