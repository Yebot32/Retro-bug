// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_aotgen` — the offline side of AOT translation. AOT task 2 is the discovery half: read an ARM
// shared object and say which guest `.text` offsets are starts worth translating. The emitter (AOT task
// 3) adds subcommands behind `RB_AOTGEN_EMIT`; this file deliberately links no engine and no Dynarmic.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "code_map.h"
#ifdef RB_AOTGEN_EMIT
#include "emit.h"
#include "sha256.h"
#endif
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
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
    "  --trace-base is the load bias the trace's PCs were recorded at; it defaults to --base, so a\n"
    "  Tier I trace (recorded near the loader's own placement) needs it named explicitly.\n"
    "  --emit --out <prefix> [--base <hex>] <lib.so>\n"
    "  Emits <prefix>.S and <prefix>_table.cpp for AOT (task 3). --base is the assumed load bias\n"
    "  (guest address of the image's vaddr 0); it defaults to 0x10000 and must be 4 KiB-aligned.\n"
    "  --profile <dir> --out <prefix> [--trace <file>] [--base <hex>]\n"
    "  Emits one image per *.so in <dir>, plus <prefix>_profile.cpp and <prefix>_profile.json. Each\n"
    "  library gets a 32 MiB slot from --base (default 0x10000000) in sorted order; the runtime must\n"
    "  map it there. A library whose translation aborts the process (a Dynarmic front-end assertion on\n"
    "  an encoding decoded from data) is skipped by name and stays on Tier I.\n"
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
      rb::aotgen::CountSource(map, rb::aotgen::kFromInit),      rb::aotgen::CountSource(map, rb::aotgen::kFromReloc),
      rb::aotgen::CountSource(map, rb::aotgen::kFromExidx),
      rb::aotgen::CountSource(map, rb::aotgen::kFromVeneer),
      rb::aotgen::CountSource(map, rb::aotgen::kFromTrace));
}

#ifdef RB_AOTGEN_EMIT
std::string Hex32(std::uint32_t value) {
  char buffer[11];
  std::snprintf(buffer, sizeof buffer, "0x%08x", value);
  return buffer;
}

/// Convert a recorded trace into this image's starts (guest PCs -> `text`-relative offsets).
/// `recorded_bias` is the load bias the run recorded its PCs at, which is not the bias this profile
/// emits at: a Tier I trace names the addresses the loader actually chose. Defaults to the emitted bias.
bool BuildTraceFor(const std::string& lib_path, std::uint32_t recorded_bias, const std::string& trace_path,
                   CodeMap* trace, std::string* err) {
  if (trace_path.empty()) return true;
  const std::vector<std::uint8_t> bytes = ReadFile(lib_path);
  if (bytes.empty()) {
    *err = "cannot read " + lib_path;
    return false;
  }
  CodeMap image{};
  std::string base_err;
  image = rb::aotgen::DiscoverCodeMap(bytes, nullptr, &base_err);
  if (!base_err.empty()) {
    *err = lib_path + ": " + base_err;
    return false;
  }
  TraceStats stats;
  if (!ParseTrace(trace_path, image, recorded_bias, trace, &stats, err)) return false;
  std::printf("aotgen: trace %s at base 0x%x: %zu records, %zu unique, %zu mapped, %zu outside%s\n",
              trace_path.c_str(), recorded_bias, stats.records, stats.unique, stats.mapped,
              stats.outside, stats.truncated ? " (truncated tail)" : "");
  if (stats.mapped == 0 && stats.records != 0) {
    std::fprintf(stderr,
                 "aotgen: warning: no recorded start fell inside this image at base 0x%x — pass\n"
                 "        --trace-base with the bias the traced run used (its `load:` lines name it)\n",
                 recorded_bias);
  }
  return true;
}

/// The image's lowest and highest PT_LOAD vaddr, page-rounded: the span that must fit one bias slot.
bool LoadSpan(const std::vector<std::uint8_t>& elf, std::uint32_t* lo, std::uint32_t* hi,
              std::string* err) {
  const auto le16 = [](const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
  };
  const auto le32 = [](const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  };
  if (elf.size() < 52 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
    *err = "not an ELF image";
    return false;
  }
  if (elf[4] != 1) {
    *err = "not a 32-bit ELF image";
    return false;
  }
  const std::uint32_t phoff = le32(elf.data() + 28);
  const std::uint32_t phentsize = le16(elf.data() + 42);
  const std::uint32_t phnum = le16(elf.data() + 44);
  if (phentsize < 32 || static_cast<std::uint64_t>(phoff) + static_cast<std::uint64_t>(phentsize) * phnum >
                            elf.size()) {
    *err = "truncated program header table";
    return false;
  }
  std::uint64_t low = 0x1'0000'0000ull;
  std::uint64_t high = 0;
  const std::uint64_t kPageMask = ~static_cast<std::uint64_t>(0xfff);
  for (std::uint32_t i = 0; i < phnum; ++i) {
    const std::uint8_t* ph = elf.data() + phoff + i * phentsize;
    if (le32(ph) != 1) continue;  // PT_LOAD
    const std::uint64_t vaddr = le32(ph + 8);
    const std::uint64_t memsz = le32(ph + 20);
    low = std::min(low, vaddr & kPageMask);
    high = std::max(high, (vaddr + memsz + 0xfff) & kPageMask);
  }
  if (high <= low) {
    *err = "the image has no PT_LOAD segments";
    return false;
  }
  *lo = static_cast<std::uint32_t>(low);
  *hi = static_cast<std::uint32_t>(high);
  return true;
}

struct ProfileEntry {
  std::string soname;
  std::string prefix;   ///< the per-library output prefix (its .S and table use it)
  std::string symbol;   ///< the image symbol the table defines
  std::uint32_t bias = 0;
  std::uint32_t text_vaddr = 0;
  std::uint32_t text_len = 0;
  std::uint64_t fingerprint = 0;
  std::size_t blocks = 0;
  std::size_t refused = 0;
  std::size_t code_bytes = 0;
  std::string sha256;
};

std::string Sanitize(const std::string& name) {
  std::string s = name;
  for (char& c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!ok) c = '_';
  }
  return s;
}

/// Emit one library in a child process. The front end can assert on an encoding decoded from data
/// (a start the discovery named but the bytes are a literal pool); an assert kills the process, and a
/// profile build must survive one library to keep the others. The child writes its facts to a file the
/// parent reads; a crash is a skipped library, reported by name, not a failed profile.
bool EmitOne(const std::string& lib_path, const ProfileEntry& entry, const std::string& trace_path,
             std::uint32_t recorded_bias, ProfileEntry* out) {
#if defined(_WIN32)
  (void)lib_path;
  (void)entry;
  (void)trace_path;
  (void)recorded_bias;
  (void)out;
  std::fprintf(stderr, "aotgen: --profile needs a POSIX host (child-process isolation)\n");
  return false;
#else
  const std::string facts_path = entry.prefix + ".facts";
  std::remove(facts_path.c_str());
  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = fork();
  if (pid == 0) {
    CodeMap trace{};
    std::string err;
    if (!BuildTraceFor(lib_path, recorded_bias, trace_path, &trace, &err)) {
      std::fprintf(stderr, "aotgen: %s: %s\n", entry.soname.c_str(), err.c_str());
      std::fflush(stderr);
      _exit(2);
    }
    rb::aotgen::EmitFacts facts;
    if (!rb::aotgen::EmitImage(lib_path, entry.prefix, entry.bias, trace_path.empty() ? nullptr : &trace,
                               &facts, &err)) {
      std::fprintf(stderr, "aotgen: %s: %s\n", entry.soname.c_str(), err.c_str());
      std::fflush(stderr);
      _exit(1);
    }
    std::ofstream facts_out(facts_path, std::ios::binary);
    facts_out << facts.text_vaddr << " " << facts.text_len << " " << facts.fingerprint << " "
              << facts.blocks << " " << facts.refused << " " << facts.code_bytes << " " << facts.symbol
              << "\n";
    facts_out.close();
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(facts_out ? 0 : 3);
  }
  if (pid < 0) {
    std::fprintf(stderr, "aotgen: cannot fork for %s\n", entry.soname.c_str());
    return false;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    std::fprintf(stderr, "aotgen: waitpid failed for %s\n", entry.soname.c_str());
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFSIGNALED(status)) {
      std::fprintf(stderr,
                   "aotgen: %s: translation aborted (signal %d: a front-end assertion on an encoding "
                   "decoded from data); library skipped, it stays on Tier I\n",
                   entry.soname.c_str(), WTERMSIG(status));
    }
    return false;
  }
  std::ifstream facts_in(facts_path);
  if (!facts_in) return false;
  *out = entry;
  facts_in >> out->text_vaddr >> out->text_len >> out->fingerprint >> out->blocks >> out->refused >>
      out->code_bytes >> out->symbol;
  std::remove(facts_path.c_str());
  return facts_in.good() || facts_in.eof();
#endif
}

int RunProfile(const std::string& dir, const std::string& out_prefix, const std::string& trace_path,
               std::uint32_t base, bool trace_base_given, std::uint32_t trace_base) {
  namespace fs = std::filesystem;
  // A failed run must not leave an earlier run's profile for CMake to compile; the JSON and table are
  // rewritten only after every library emitted.
  std::remove((out_prefix + "_profile.cpp").c_str());
  std::remove((out_prefix + "_profile.json").c_str());
  // Keep this in step with rb::exec::kAotSlotBytes and with the smallest window the iOS app can reserve
  // (512 MiB). A profile that would run past the window is refused here, by name, instead of making
  // every later load fail.
  constexpr std::uint64_t kSlot = 0x0200'0000ull;
  constexpr std::uint64_t kWindow = 0x2000'0000ull;
  std::error_code ec;
  std::vector<std::string> libs;
  for (const auto& item : fs::directory_iterator(dir, ec)) {
    if (!item.is_regular_file()) continue;
    const std::string name = item.path().filename().string();
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) libs.push_back(name);
  }
  if (ec) {
    std::fprintf(stderr, "aotgen: cannot read %s: %s\n", dir.c_str(), ec.message().c_str());
    return 1;
  }
  std::sort(libs.begin(), libs.end());
  if (libs.empty()) {
    std::fprintf(stderr, "aotgen: %s has no *.so files\n", dir.c_str());
    return 1;
  }

  std::vector<ProfileEntry> emitted;
  for (std::size_t i = 0; i < libs.size(); ++i) {
    ProfileEntry entry;
    entry.soname = libs[i];
    entry.prefix = out_prefix + "_" + Sanitize(libs[i].substr(0, libs[i].size() - 3));
    entry.symbol = rb::aotgen::ImageSymbol(entry.prefix);
    const std::uint64_t bias64 = static_cast<std::uint64_t>(base) + i * kSlot;
    if (bias64 + kSlot > kWindow) {
      std::fprintf(stderr,
                   "aotgen: %s would be placed past the 512 MiB window (slot %zu at 0x%llx); use a "
                   "smaller --base or fewer libraries\n",
                   libs[i].c_str(), i, static_cast<unsigned long long>(bias64));
      return 1;
    }
    entry.bias = static_cast<std::uint32_t>(bias64);
    const std::string lib_path = (fs::path(dir) / libs[i]).string();
    const std::vector<std::uint8_t> bytes = ReadFile(lib_path);
    if (bytes.empty()) {
      std::fprintf(stderr, "aotgen: cannot read %s\n", lib_path.c_str());
      continue;
    }
    std::uint32_t lo = 0, hi = 0;
    std::string span_err;
    if (!LoadSpan(bytes, &lo, &hi, &span_err)) {
      std::fprintf(stderr, "aotgen: %s: %s\n", libs[i].c_str(), span_err.c_str());
      continue;
    }
    if (static_cast<std::uint64_t>(hi) - lo > kSlot) {
      std::fprintf(stderr,
                   "aotgen: %s loads 0x%x bytes (vaddr 0x%x..0x%x), larger than the 32 MiB slot\n",
                   libs[i].c_str(), hi - lo, lo, hi);
      continue;
    }
    entry.sha256 = rb::aotgen::Sha256Hex(rb::aotgen::Sha256(bytes));
    const std::uint32_t recorded_bias = trace_base_given ? trace_base : entry.bias;
    ProfileEntry facts;
    if (!EmitOne(lib_path, entry, trace_path, recorded_bias, &facts)) continue;
    emitted.push_back(facts);
    std::printf("aotgen: profile %s: %zu blocks, %zu refused, %zu code bytes at base 0x%x\n",
                entry.soname.c_str(), facts.blocks, facts.refused, facts.code_bytes, entry.bias);
  }
  if (emitted.empty()) {
    std::fprintf(stderr, "aotgen: no library in %s could be translated\n", dir.c_str());
    return 1;
  }

  const std::string json_path = out_prefix + "_profile.json";
  std::ofstream json(json_path, std::ios::binary);
  if (!json) {
    std::fprintf(stderr, "aotgen: cannot write %s\n", json_path.c_str());
    return 1;
  }
  json << "{\n  \"format\": 1,\n  \"name\": \"" << Sanitize(fs::path(out_prefix).filename().string())
       << "\",\n  \"libraries\": [\n";
  for (std::size_t i = 0; i < emitted.size(); ++i) {
    const ProfileEntry& e = emitted[i];
    json << "    {\"soname\": \"" << e.soname << "\", \"sha256\": \"" << e.sha256
         << "\", \"load_bias\": " << e.bias << ", \"text_vaddr\": " << e.text_vaddr
         << ", \"text_len\": " << e.text_len << ", \"blocks\": " << e.blocks
         << ", \"refused\": " << e.refused << ", \"code_bytes\": " << e.code_bytes
         << ", \"symbol\": \"" << e.symbol << "\"}" << (i + 1 == emitted.size() ? "\n" : ",\n");
  }
  json << "  ]\n}\n";
  json.close();

  const std::string cpp_path = out_prefix + "_profile.cpp";
  std::ofstream cpp(cpp_path, std::ios::binary);
  if (!cpp) {
    std::fprintf(stderr, "aotgen: cannot write %s\n", cpp_path.c_str());
    return 1;
  }
  cpp << "// Generated by rb_aotgen --profile (AOT task 5) from " << dir << "\n";
  cpp << "// Load biases: " << Hex32(base) << " + 32 MiB per library, sorted by name.\n";
  cpp << "// The runtime must map each module at its image's load_bias; a mismatch disables AOT.\n";
  cpp << "#include \"rb/exec/aot.h\"\n\n";
  for (const ProfileEntry& e : emitted) {
    cpp << "extern const rb::exec::AotImageInfo " << e.symbol << ";\n";
  }
  cpp << "\nnamespace {\nconst rb::exec::AotLibrary kAotProfileLibraries[] = {\n";
  for (const ProfileEntry& e : emitted) {
    cpp << "    {\"" << e.soname << "\", \"" << e.sha256 << "\", &" << e.symbol << "},\n";
  }
  cpp << "};\n}  // namespace\n\n";
  cpp << "extern \"C\" const rb::exec::AotProfile rb_aot_builtin_profile = {\""
      << Sanitize(fs::path(out_prefix).filename().string()) << "\", kAotProfileLibraries, "
      << emitted.size() << "};\n";
  cpp.close();
  std::printf("aotgen: profile wrote %s and %s (%zu libraries)\n", json_path.c_str(), cpp_path.c_str(),
              emitted.size());
  return 0;
}
#endif  // RB_AOTGEN_EMIT

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string trace_path;
  std::string out_prefix;
  std::string profile_dir;
  std::uint32_t load_bias = 0x0001'0000u;
  bool base_given = false;
  std::uint32_t trace_base = 0;
  bool trace_base_given = false;
  bool profile = false;
  bool emit = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--profile") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aotgen: --profile needs a directory\n");
        return 2;
      }
      profile_dir = argv[++i];
      profile = true;
    } else if (arg == "--trace-base") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aotgen: --trace-base needs a value\n");
        return 2;
      }
      char* end = nullptr;
      const unsigned long value = std::strtoul(argv[++i], &end, 0);
      if (end == argv[i] || *end != '\0' || value > 0xffff'fffful || (value & 0xfffu) != 0) {
        std::fprintf(stderr, "aotgen: --trace-base must be a 4 KiB-aligned 32-bit value\n");
        return 2;
      }
      trace_base = static_cast<std::uint32_t>(value);
      trace_base_given = true;
    } else if (arg == "--trace") {
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
      base_given = true;
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
  if (profile) {
#ifdef RB_AOTGEN_EMIT
    if (!path.empty() || out_prefix.empty()) {
      std::fprintf(stderr, "aotgen: --profile takes a directory and --out <prefix>\n%s", kUsage);
      return 2;
    }
    // A profile must not land on the low addresses a boot uses for its scope; 256 MiB up with 32 MiB
    // slots leaves every image far from libc's, the stack's and the trap window's placements.
    const std::uint32_t base = base_given ? load_bias : 0x1000'0000u;
    return RunProfile(profile_dir, out_prefix, trace_path, base, trace_base_given, trace_base);
#else
    std::fprintf(stderr, "aotgen: this build has no emitter (only AArch64 hosts build it)\n");
    return 2;
#endif
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
    CodeMap emit_trace{};
    std::string err;
    if (!BuildTraceFor(path, trace_base_given ? trace_base : load_bias, trace_path, &emit_trace, &err)) {
      std::fprintf(stderr, "aotgen: %s: %s\n", path.c_str(), err.c_str());
      return 1;
    }
    if (!rb::aotgen::EmitImage(path, out_prefix, load_bias, trace_path.empty() ? nullptr : &emit_trace,
                               nullptr, &err)) {
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
