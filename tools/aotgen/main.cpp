// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_aotgen` — the offline side of AOT translation. AOT task 2 is the discovery half: read an ARM
// shared object and say which guest `.text` offsets are starts worth translating. The emitter (AOT task
// 3) adds subcommands behind `RB_AOTGEN_EMIT`; this file deliberately links no engine and no Dynarmic.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "code_map.h"
#ifdef RB_AOTGEN_EMIT
#include "emit.h"
#endif

namespace {

using rb::aotgen::CodeMap;
using rb::aotgen::CodeMapEntry;
using rb::aotgen::kArm;
using rb::aotgen::kThumb;

constexpr const char* kUsage =
    "usage: rb_aotgen <lib.so> [--trace <offsets.txt>]\n"
    "  Prints one line: starts N (export X, init Y, reloc Z, exidx E, veneer V).\n"
    "  --trace merges a provisional text trace: one '<offset> [thumb|arm]' per line, where offset is\n"
    "  file-relative to .text (the same unit the code map stores). AOT task 4 will replace the format.\n"
#ifdef RB_AOTGEN_EMIT
    "  --emit --out <prefix> [--base <hex>] <lib.so>\n"
    "  Emits <prefix>.S and <prefix>_table.cpp for AOT (task 3). --base is the assumed load bias\n"
    "  (guest address of the image's vaddr 0); it defaults to 0x10000 and must be 4 KiB-aligned.\n"
#endif
    ;

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

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ParseTrace(const std::string& path, CodeMap* trace, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    *err = "cannot read trace " + path;
    return false;
  }
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    const char* cursor = line.c_str();
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor == '\0') continue;

    char* end = nullptr;
    const unsigned long value = std::strtoul(cursor, &end, 0);
    if (end == cursor || value > std::numeric_limits<std::uint32_t>::max()) {
      *err = "trace line " + std::to_string(line_number) + ": bad offset";
      return false;
    }
    while (*end == ' ' || *end == '\t') ++end;
    std::uint8_t flags = 0;
    if (*end != '\0') {
      std::string arch(end);
      while (!arch.empty() && (arch.back() == '\r' || arch.back() == ' ' || arch.back() == '\t')) {
        arch.pop_back();
      }
      if (arch == "t" || arch == "thumb" || arch == "Thumb") {
        flags = kThumb;
      } else if (arch == "a" || arch == "arm" || arch == "ARM") {
        flags = kArm;
      } else {
        *err = "trace line " + std::to_string(line_number) + ": expected thumb or arm";
        return false;
      }
    }
    CodeMapEntry entry{};
    entry.off = static_cast<std::uint32_t>(value);
    entry.flags = flags;
    trace->entries.push_back(entry);
  }
  return true;
}

void PrintSummary(const std::string& label, const CodeMap& map) {
  std::printf(
      "aotgen: %s starts %zu (export %zu, init %zu, reloc %zu, exidx %zu, veneer %zu)\n",
      label.c_str(), map.entries.size(),
      rb::aotgen::CountSource(map, rb::aotgen::kFromExport),
      rb::aotgen::CountSource(map, rb::aotgen::kFromInit),
      rb::aotgen::CountSource(map, rb::aotgen::kFromReloc),
      rb::aotgen::CountSource(map, rb::aotgen::kFromExidx),
      rb::aotgen::CountSource(map, rb::aotgen::kFromVeneer));
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string trace_path;
  std::string out_prefix;
  std::uint32_t load_bias = 0x0001'0000u;
  bool emit = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--trace") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aotgen: --trace needs a file\n");
        return 2;
      }
      trace_path = argv[++i];
    } else if (arg == "--emit") {
      emit = true;
    } else if (arg == "--out") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aotgen: --out needs a prefix\n");
        return 2;
      }
      out_prefix = argv[++i];
    } else if (arg == "--base") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aotgen: --base needs a value\n");
        return 2;
      }
      char* end = nullptr;
      const unsigned long value = std::strtoul(argv[++i], &end, 0);
      if (end == argv[i] || *end != '\0' || value > 0xffff'fffful || (value & 0xfffu) != 0) {
        std::fprintf(stderr, "aotgen: --base must be a 4 KiB-aligned 32-bit value\n");
        return 2;
      }
      load_bias = static_cast<std::uint32_t>(value);
    } else if (arg == "-h" || arg == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "aotgen: unknown option %s\n%s", arg.c_str(), kUsage);
      return 2;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::fprintf(stderr, "aotgen: only one library per invocation\n%s", kUsage);
      return 2;
    }
  }
  if (path.empty()) {
    std::fputs(kUsage, stderr);
    return 2;
  }

  if (emit) {
#ifdef RB_AOTGEN_EMIT
    if (out_prefix.empty()) {
      std::fprintf(stderr, "aotgen: --emit needs --out <prefix>\n%s", kUsage);
      return 2;
    }
    std::string err;
    if (!rb::aotgen::EmitImage(path, out_prefix, load_bias, &err)) {
      std::fprintf(stderr, "aotgen: %s: %s\n", path.c_str(), err.c_str());
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr, "aotgen: this build has no emitter (only AArch64 hosts build it)\n");
    return 2;
#endif
  }

  const std::vector<std::uint8_t> bytes = ReadFile(path);
  if (bytes.empty()) {
    std::fprintf(stderr, "aotgen: cannot read %s\n", path.c_str());
    return 1;
  }

  CodeMap trace{};
  if (!trace_path.empty()) {
    std::string trace_err;
    if (!ParseTrace(trace_path, &trace, &trace_err)) {
      std::fprintf(stderr, "aotgen: %s\n", trace_err.c_str());
      return 1;
    }
  }

  std::string err;
  const CodeMap map = rb::aotgen::DiscoverCodeMap(bytes, trace_path.empty() ? nullptr : &trace, &err);
  if (!err.empty()) {
    std::fprintf(stderr, "aotgen: %s: %s\n", path.c_str(), err.c_str());
    return 1;
  }
  const std::string label = map.soname.empty() ? BaseName(path) : map.soname;
  PrintSummary(label, map);
  return 0;
}
