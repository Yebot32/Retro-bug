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
#include <cstdio>
#include <fstream>
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

/// Every refusal names its reason; the test checks the discriminating phrase rather than the whole
/// sentence so a wording change does not masquerade as a reader bug.
void ExpectRefusal(const std::vector<std::uint8_t>& bytes, const char* phrase,
                   const std::string& what) {
  std::string err;
  const CodeMap map = DiscoverCodeMap(bytes, nullptr, &err);
  Check(err.find(phrase) != std::string::npos, what + " (" + (err.empty() ? "no error" : err) + ")");
  Check(map.entries.empty(), what + " returns no entries");
}

void MalformedInputs() {
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
  truncated[32] = static_cast<std::uint8_t>(shoff);
  truncated[46] = static_cast<std::uint8_t>(shentsize);
  truncated[47] = static_cast<std::uint8_t>(shentsize >> 8);
  truncated[48] = static_cast<std::uint8_t>(shnum);
  truncated[49] = static_cast<std::uint8_t>(shnum >> 8);
  ExpectRefusal(truncated, "truncated section header table", "section table runs past EOF");

  // One valid but empty section: no `.text` anywhere, which is the loader's own refusal to name a code
  // map for an image with no code section.
  std::vector<std::uint8_t> no_text = Header(1, 1, 40, 3);
  no_text.resize(52 + 40, 0);
  no_text[32] = static_cast<std::uint8_t>(shoff);
  no_text[46] = static_cast<std::uint8_t>(shentsize);
  no_text[48] = 1;
  ExpectRefusal(no_text, "missing .text", "ELF without .text");
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

bool OneIsaAndASource(const CodeMap& map) {
  for (const CodeMapEntry& entry : map.entries) {
    const std::uint8_t isa = entry.flags & (kThumb | kArm);
    if ((isa != kThumb && isa != kArm) || entry.sources == 0) return false;
  }
  return true;
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
  Check(NearPage(map.text_len, 1931), "libgame text_len is 1,931 KB ± one page");
  Check(static_cast<std::uint64_t>(map.text_off) + map.text_len <= bytes.size(),
        "libgame text lies inside the file");
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
  Check(NearPage(map.text_len, 364), "libdlgl text_len is 364 KB ± one page");
  Check(static_cast<std::uint64_t>(map.text_off) + map.text_len <= bytes.size(),
        "libdlgl text lies inside the file");
  Check(StrictlyOrdered(map), "libdlgl entries are strictly ordered by offset");
  Check(OneIsaAndASource(map), "every libdlgl entry has one ISA and one source");
  Summary("libdlgl", map);
}

}  // namespace

int main(int argc, char** argv) {
  MalformedInputs();

  const std::string game_path =
      argc > 1 ? argv[1] : "build/br-inspect/lib/armeabi-v7a/libgame.so";
  const std::string dlgl_path =
      argc > 2 ? argv[2] : "build/kk-inspect/lib/armeabi/libdlgl.so";

  const std::vector<std::uint8_t> game = ReadFile(game_path);
  if (game.empty()) {
    std::printf("# no libgame.so — skipped\n");
    // The synthetic refusals above are the only checks that ran; a failure there must still fail the
    // lane rather than being hidden by the missing corpus.
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
