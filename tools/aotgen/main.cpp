// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_aotgen` — the offline side of AOT translation. AOT task 2 is the discovery half: read an ARM
// shared object and say which guest `.text` offsets are starts worth translating. The emitter (AOT task
// 3) adds subcommands behind `RB_AOTGEN_EMIT`; this file deliberately links no engine and no Dynarmic.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
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

// The code-map trace's on-disk contract, from `core/include/rb/exec/trace.h`. Duplicated rather than
// included because this tool deliberately links nothing from `core/`; the two are checked against each
// other by `rb_code_map_trace_tests` (which writes a file with the runtime's writer) and by
// `rb_aotgen`'s own use of the same bytes.
constexpr char kTraceMagic[8] = {'R', 'B', 'C', 'M', 'T', 'R', 'C', '\0'};
constexpr std::uint32_t kTraceVersion = 1;
constexpr std::uint32_t kTraceHeaderBytes = 16;
constexpr std::uint32_t kTraceRecordBytes = 10;

constexpr const char* kUsage =
    "usage: rb_aotgen <lib.so> [--trace <trace.rbcm>] [--base <hex>]\n"
    "  Prints one line: starts N (export X, init Y, reloc Z, exidx E, veneer V, trace T).\n"
    "  --trace merges the block starts a RetroBug run recorded (RB_CODEMAP / the Android\n"
    "  debug.retrobug.codemap property): a 16-byte header then {u64 pc, u8 thumb, u8 source} records.\n"
    "  --base is the load bias those guest PCs were recorded at (default 0x10000); a trace from a\n"
    "  different placement maps no starts and the command says how many fell outside .text.\n"
#ifdef RB_AOTGEN_EMIT
    "  --emit --out <prefix> [--base <hex>] <lib.so>\n"
    "  Emits <prefix>.S and <prefix>_table.cpp for AOT (task 3). --base is the assumed load bias\n"
    "  (guest address of the image's vaddr 0); it defaults to 0x10000 and must be 4 KiB-aligned.\n"
#endif
    ;

std::uint32_t ReadU32Le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64Le(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(ReadU32Le(p)) |
         (static_cast<std::uint64_t>(ReadU32Le(p + 4)) << 32);
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

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct TraceStats {
  std::size_t records = 0;    ///< complete records in the file
  std::size_t unique = 0;     ///< distinct (pc, thumb) pairs
  std::size_t mapped = 0;     ///< unique pairs that fell inside .text and became starts
  std::size_t outside = 0;    ///< unique pairs outside .text at the requested load bias
  std::size_t duplicates = 0; ///< records naming an already-seen (pc, thumb)
  bool truncated = false;     ///< a partial record at the end (a run killed before its last flush)
};

/**
 * Merge a `RB_CODEMAP` trace into code-map offsets.
 *
 * The file stores guest PCs, so the conversion is `pc - (load_bias + .text vaddr)`; the caller supplies
 * the same `--base` the emitter would use. Records outside that range belong to another library in the
 * same process (a real run maps libc, libm and the game at once), or to the wrong base — either way they
 * are counted, not guessed at. The runtime already dedupes, but the tool re-establishes it: a file
 * assembled from several runs must still merge to one start per address and ISA.
 */
bool ParseTrace(const std::string& path, const CodeMap& image, std::uint32_t load_bias,
                CodeMap* trace, TraceStats* stats, std::string* err) {
  const std::vector<std::uint8_t> bytes = ReadFile(path);
  if (bytes.size() < kTraceHeaderBytes) {
    *err = "cannot read " + path + " as a code-map trace (missing or shorter than its header)";
    return false;
  }
  if (std::memcmp(bytes.data(), kTraceMagic, sizeof kTraceMagic) != 0) {
    *err = path + " is not a code-map trace (bad magic; expected RBCMTRC)";
    return false;
  }
  const std::uint32_t version = ReadU32Le(bytes.data() + 8);
  if (version != kTraceVersion) {
    *err = path + ": unsupported code-map trace version " + std::to_string(version) + " (this tool reads " +
           std::to_string(kTraceVersion) + ")";
    return false;
  }
  const std::uint32_t record_bytes = ReadU32Le(bytes.data() + 12);
  if (record_bytes != kTraceRecordBytes) {
    *err = path + ": unsupported trace record size " + std::to_string(record_bytes) + " (expected " +
           std::to_string(kTraceRecordBytes) + ")";
    return false;
  }

  const std::size_t body = bytes.size() - kTraceHeaderBytes;
  stats->records = body / kTraceRecordBytes;
  stats->truncated = (body % kTraceRecordBytes) != 0;
  const std::uint64_t text_base =
      static_cast<std::uint64_t>(load_bias) + static_cast<std::uint64_t>(image.text_vaddr);

  std::unordered_set<std::uint64_t> seen;
  for (std::size_t i = 0; i < stats->records; ++i) {
    const std::uint8_t* record = bytes.data() + kTraceHeaderBytes + i * kTraceRecordBytes;
    const std::uint64_t pc = ReadU64Le(record);
    const std::uint8_t thumb = record[8];
    // record[9] is the source (`kSourceBlockEntry` today). The map needs only address and ISA, and
    // accepting unknown sources keeps an older tool able to read a newer producer's file.
    if (thumb > 1) {
      *err = path + ": record " + std::to_string(i) + " has thumb=" + std::to_string(thumb) +
             " (expected 0 or 1)";
      return false;
    }
    if (pc > 0xffff'ffffull) {
      ++stats->outside;  // the guest address space is 32-bit
      continue;
    }
    if (!seen.insert((pc << 1) | thumb).second) {
      ++stats->duplicates;
      continue;
    }
    ++stats->unique;
    if (pc < text_base || pc - text_base >= image.text_len) {
      ++stats->outside;
      continue;
    }
    CodeMapEntry entry{};
    entry.off = static_cast<std::uint32_t>(pc - text_base) & ~1u;
    entry.flags = thumb != 0 ? kThumb : kArm;
    trace->entries.push_back(entry);
    ++stats->mapped;
  }
  return true;
}

void PrintSummary(const std::string& label, const CodeMap& map) {
  std::printf(
      "aotgen: %s starts %zu (export %zu, init %zu, reloc %zu, exidx %zu, veneer %zu, trace %zu)\n",
      label.c_str(), map.entries.size(),
      rb::aotgen::CountSource(map, rb::aotgen::kFromExport),
      rb::aotgen::CountSource(map, rb::aotgen::kFromInit),
      rb::aotgen::CountSource(map, rb::aotgen::kFromReloc),
      rb::aotgen::CountSource(map, rb::aotgen::kFromExidx),
      rb::aotgen::CountSource(map, rb::aotgen::kFromVeneer),
      rb::aotgen::CountSource(map, rb::aotgen::kFromTrace));
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
    } else if (arg.rfind("--trace=", 0) == 0) {
      trace_path = arg.substr(std::strlen("--trace="));
      if (trace_path.empty()) {
        std::fprintf(stderr, "aotgen: --trace needs a file\n");
        return 2;
      }
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
    // `MergeTrace` works in `.text`-relative offsets, while the file stores guest PCs. The conversion
    // needs the section's vaddr and length, so discover once for those facts; the real discovery runs
    // again below with the converted trace merged. An image discovery rejects would fail either call.
    std::string base_err;
    const CodeMap image = rb::aotgen::DiscoverCodeMap(bytes, nullptr, &base_err);
    if (!base_err.empty()) {
      std::fprintf(stderr, "aotgen: %s: %s\n", path.c_str(), base_err.c_str());
      return 1;
    }
    TraceStats stats;
    std::string trace_err;
    if (!ParseTrace(trace_path, image, load_bias, &trace, &stats, &trace_err)) {
      std::fprintf(stderr, "aotgen: %s\n", trace_err.c_str());
      return 1;
    }
    std::fprintf(stderr,
                 "aotgen: trace %s: %zu records, %zu unique, %zu in .text, %zu outside at base 0x%x, "
                 "%zu duplicate(s)%s\n",
                 trace_path.c_str(), stats.records, stats.unique, stats.mapped, stats.outside,
                 load_bias, stats.duplicates,
                 stats.truncated ? ", truncated tail ignored" : "");
  }

  std::string err;
  const CodeMap map = rb::aotgen::DiscoverCodeMap(bytes, trace_path.empty() ? nullptr : &trace, &err);
  if (!err.empty()) {
    std::fprintf(stderr, "aotgen: %s: %s\n", path.c_str(), err.c_str());
    return 1;
  }
  const std::string label = map.soname.empty() ? BaseName(path) : map.soname;
  PrintSummary(label, map);
  if (!trace_path.empty()) {
    std::size_t trace_only = 0;
    for (const CodeMapEntry& entry : map.entries) {
      if (entry.sources == rb::aotgen::kFromTrace) ++trace_only;
    }
    const std::size_t touched = rb::aotgen::CountSource(map, rb::aotgen::kFromTrace);
    std::printf(
        "aotgen: trace contributed %zu new start(s); %zu of the %zu traced starts were already "
        "discovered\n",
        trace_only, touched - trace_only, touched);
  }
  return 0;
}
