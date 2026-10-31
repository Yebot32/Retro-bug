/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * AOT task 4: the code-map trace records the guest addresses a run actually reached as block starts,
 * including the ones only an indirect branch names. This test drives the fixture's `fib` through
 * Tier I with `RB_CODEMAP` set and reads the file back, so the recorder, the file format and the
 * translate-miss hook are all measured by executing real guest code.
 *
 * What is asserted, and why each one is not a restatement of the implementation:
 *
 *   - The export `Java_..._fib` and the static `fib` it calls recursively are both present. The
 *     export is one block (a tail branch); `fib` is reached by `bl`, so the second address is exactly
 *     the kind of start a static scan of exports would not necessarily name. Both are looked up in the
 *     fixture's own symbol table, not hardcoded, so rebuilding the fixture does not silently weaken it.
 *   - No `(pc, thumb)` pair appears twice. The recorder dedupes by construction; a second record would
 *     mean the hook is being called somewhere it should not be (or the dedupe key is wrong).
 *   - The record count is smaller than the IR-step count for the same call. That is the difference
 *     between "one record per block start" and "one record per instruction", which is the mistake a
 *     `PreCodeReadHook` hook makes if it forgets to ask whether this is the block's first instruction.
 *   - With no path configured, the same call writes nothing: `record_count()` stays zero and no file
 *     appears even when `OnBlockEntry` is called directly.
 *
 * Fixture-only, like the other execution lanes: without `/data/local/tmp/librb_fixture.so` this
 * prints a skip line and exits 0. It has no AArch64 requirement — Tier I is host-ISA independent —
 * so it runs on the tablet and on an x86_64 emulator lane that has the fixture.
 */
#include "rb/exec/interp.h"
#include "rb/exec/trace.h"
#include "rb/isa/front_end.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

namespace {

using namespace rb;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace loader = rb::loader;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                        \
  do {                                                 \
    ++checks;                                          \
    if (!(cond)) fail(#cond, __LINE__, detail);        \
  } while (0)

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
constexpr exec::GuestAddr kLoadBias = 0x0001'0000u;
/// The file this lane writes. A fixed path rather than a random one: the test runs as the shell user
/// on the device, `/data/local/tmp` is the one directory guaranteed writable, and a failure leaves the
/// file behind for inspection instead of hiding it in a randomized name.
constexpr const char* kTracePath = "/data/local/tmp/rb_codemap_trace_test.rbcm";
/// A second path for the "Opening a trace truncates" check, so that check does not destroy the trace
/// this lane leaves for `rb_aotgen --trace` (the tool's parser is exercised against a runtime-written
/// file, not only against the writer's mirror image).
constexpr const char* kTruncatePath = "/data/local/tmp/rb_codemap_trace_truncate_test.rbcm";

std::vector<std::uint8_t> ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

bool Exists(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return in.good();
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

struct Rig {
  std::unique_ptr<mem::GuestMemory> mem;
  std::optional<loader::GuestModule> mod;
  std::unique_ptr<exec::InterpExecutor> exec;
  bool ok = false;
};

Rig Build() {
  Rig rig;
  const auto bytes = ReadFile(RB_FIXTURE_PATH);
  if (bytes.empty()) {
    std::printf("# codemap: %s is missing — skipped\n", RB_FIXTURE_PATH);
    return rig;
  }
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = mem::GuestMemory::Create(32u << 20, model, &err);
  if (!rig.mem) {
    fail("could not create guest memory", __LINE__, err);
    return rig;
  }
  loader::LoadOptions opts;
  // A fixed bias makes the file's guest PCs predictable to a reader; more importantly it exercises the
  // same "PC minus base" conversion the tool performs. The loader honours the hint exactly.
  opts.hint = kLoadBias;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return rig;
  }
  if (rig.mod->base != kLoadBias) {
    fail("the loader did not honour the test's load bias", __LINE__, std::to_string(rig.mod->base));
    return rig;
  }
  if (!rig.mem->Map(kStackTop - 0x8000, 0x8000, mem::kRead | mem::kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  rig.exec = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.ok = true;
  return rig;
}

/// The same JNI call shape the other fixture lanes use: env in r0, class in r1, the declared argument
/// in r2.
exec::RunResult Call(Rig& rig, exec::GuestAddr where, std::int32_t arg, exec::MachineState& state,
                     bool thumb) {
  state = exec::MachineState{};
  state.pc = where;
  state.mode = thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  state.r[0] = 0;
  state.r[1] = 0;
  state.r[2] = static_cast<std::uint32_t>(arg);
  state.r[13] = kStackTop;
  state.r[14] = kReturnAddress;
  state.r[15] = where;
  return rig.exec->Run(state, 4'000'000);
}

// ---------------------------------------------------------------- the trace file, read independently

struct TraceFile {
  std::vector<std::pair<std::uint32_t, bool>> entries;
  std::size_t records = 0;
  std::size_t duplicates = 0;
  bool all_sources_are_block_entries = true;
};

std::uint32_t ReadU32Le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64Le(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(ReadU32Le(p)) |
         (static_cast<std::uint64_t>(ReadU32Le(p + 4)) << 32);
}

/// Parse with the header's own numbers, not with a copy of the writer's loop: a wrong magic, version
/// or record size has to fail here rather than be skipped over.
bool ParseTrace(const char* path, TraceFile* out, std::string* err) {
  const std::vector<std::uint8_t> bytes = ReadFile(path);
  if (bytes.size() < exec::trace::kFileHeaderBytes) {
    *err = "the trace file is missing or shorter than its header";
    return false;
  }
  if (std::memcmp(bytes.data(), exec::trace::kFileMagic, sizeof exec::trace::kFileMagic) != 0) {
    *err = "wrong trace magic";
    return false;
  }
  if (ReadU32Le(bytes.data() + 8) != exec::trace::kFileVersion) {
    *err = "wrong trace version";
    return false;
  }
  const std::uint32_t record_bytes = ReadU32Le(bytes.data() + 12);
  if (record_bytes != exec::trace::kFileRecordBytes) {
    *err = "wrong trace record size";
    return false;
  }
  const std::size_t body = bytes.size() - exec::trace::kFileHeaderBytes;
  if (body % record_bytes != 0) {
    *err = "the trace has a partial record at the end";
    return false;
  }
  out->records = body / record_bytes;
  std::unordered_set<std::uint64_t> seen;
  for (std::size_t i = 0; i < out->records; ++i) {
    const std::uint8_t* record =
        bytes.data() + exec::trace::kFileHeaderBytes + i * exec::trace::kFileRecordBytes;
    const std::uint64_t pc = ReadU64Le(record);
    const std::uint8_t thumb = record[8];
    if (pc > 0xffff'ffffull || thumb > 1) {
      *err = "record " + std::to_string(i) + " has an impossible pc/thumb";
      return false;
    }
    if (record[9] != exec::trace::kSourceBlockEntry) out->all_sources_are_block_entries = false;
    if (!seen.insert((pc << 1) | thumb).second) ++out->duplicates;
    out->entries.emplace_back(static_cast<std::uint32_t>(pc), thumb != 0);
  }
  return true;
}

bool Contains(const TraceFile& file, std::uint32_t pc, bool thumb) {
  for (const auto& [recorded_pc, recorded_thumb] : file.entries) {
    if (recorded_pc == pc && recorded_thumb == thumb) return true;
  }
  return false;
}

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

// ---------------------------------------------------------------- the cases

/// One Tier I run with the recorder open. The export is the entry PC; the recursive call target is the
/// fixture's static `fib`, which only the callee's `bl` names.
void tier_one_records_the_reached_starts() {
  Rig rig = Build();
  if (!rig.ok) return;
  const auto export_fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  const loader::LocalSymbol* recursive = rig.mod->FindLocalSymbolEntry("fib");
  CHECK_CTX(export_fib.has_value(), "the fixture exports the fib JNI method");
  CHECK_CTX(recursive != nullptr, "the fixture's .symtab names the static fib");
  if (!export_fib || recursive == nullptr) return;
  CHECK_CTX(export_fib->addr != recursive->addr,
            "the export and the static fib are the same address, so this case proves nothing");

  std::remove(kTracePath);
  CHECK_CTX(setenv("RB_CODEMAP", kTracePath, 1) == 0, "setenv(RB_CODEMAP) failed");
  std::string err = "StartFromEnvironment did not run";
  CHECK_CTX(exec::trace::StartFromEnvironment(&err), "trace start: " + err);
  CHECK(exec::trace::enabled());
  CHECK_CTX(exec::trace::path() == kTracePath,
            "the recorder opened " + exec::trace::path() + " instead of " + kTracePath);

  exec::MachineState state;
  const exec::RunResult r = Call(rig, export_fib->addr, 10, state, export_fib->thumb);
  CHECK_CTX(r.reason == exec::StopReason::Returned,
            "fib(10) ended in reason " + std::to_string(static_cast<int>(r.reason)) + " " + r.detail);
  CHECK_CTX(static_cast<std::int32_t>(state.r[0]) == 55,
            "fib(10) returned " + std::to_string(static_cast<std::int32_t>(state.r[0])));
  const std::uint64_t recorded = exec::trace::record_count();
  exec::trace::Stop();
  unsetenv("RB_CODEMAP");

  CHECK_CTX(recorded > 0, "the trace recorded no block starts");
  // A hook that reported every instruction would produce one record per IR step; fib(10) executes
  // thousands. Block starts are a small set even before dedupe.
  CHECK_CTX(recorded < r.instructions_executed,
            "recorded " + std::to_string(recorded) + " starts for " +
                std::to_string(r.instructions_executed) + " IR steps — this looks per-instruction");
  CHECK_CTX(recorded < 200,
            "recorded " + std::to_string(recorded) + " starts for fib(10), which is more than blocks");

  TraceFile file;
  CHECK_CTX(ParseTrace(kTracePath, &file, &err), "trace parse: " + err);
  CHECK_CTX(file.records == recorded,
            "the file has " + std::to_string(file.records) + " records, record_count said " +
                std::to_string(recorded));
  CHECK_CTX(file.duplicates == 0,
            "the file has " + std::to_string(file.duplicates) + " duplicate (pc, thumb) record(s)");
  CHECK_CTX(file.all_sources_are_block_entries, "a record carries an unexpected source byte");
  CHECK_CTX(Contains(file, export_fib->addr, export_fib->thumb),
            "the trace does not contain the export entry 0x" + Hex(export_fib->addr));
  CHECK_CTX(Contains(file, recursive->addr, recursive->thumb),
            "the trace does not contain the recursive-call target 0x" + Hex(recursive->addr));
  // Every recorded PC is a guest address in this image, not a raw file offset: the tool subtracts the
  // load bias, and an offset here would land under 0x10000 and be reported as outside .text.
  bool all_in_image = true;
  for (const auto& [pc, thumb] : file.entries) {
    (void)thumb;
    if (pc < rig.mod->base || pc >= rig.mod->base + rig.mod->extent) all_in_image = false;
  }
  CHECK_CTX(all_in_image, "a recorded PC is not a guest address inside the loaded image");

  std::printf("# codemap: fib(10) -> %zu starts in %u IR steps (entry 0x%x, recursive target 0x%x)\n",
              file.records, r.instructions_executed, export_fib->addr, recursive->addr);

  // Opening the same path again truncates rather than appending a second header: a trace is one run's
  // observation, and the tool must never have two headers to interpret. A second path is used so the
  // real trace above survives for `rb_aotgen --trace`.
  std::remove(kTruncatePath);
  CHECK_CTX(exec::trace::Start(kTruncatePath, &err), "second trace start: " + err);
  CHECK(exec::trace::record_count() == 0);
  exec::trace::Stop();
  std::ifstream size_in(kTruncatePath, std::ios::binary | std::ios::ate);
  CHECK_CTX(size_in.tellg() == static_cast<std::streamsize>(exec::trace::kFileHeaderBytes),
            "a fresh trace is not exactly its header");
  std::remove(kTruncatePath);
  // kTracePath is deliberately left in place: it is this lane's output and the input the discovery
  // tool's --trace parser is verified with. The next run removes it before starting.
}

/// With no path configured the recorder is inert: the same call runs, no file appears, and even a
/// direct `OnBlockEntry` writes nothing because there is no file to write to.
void a_disabled_trace_records_nothing() {
  Rig rig = Build();
  if (!rig.ok) return;
  const auto export_fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!export_fib) {
    CHECK_CTX(false, "the fixture exports the fib JNI method");
    return;
  }

  std::remove(kTracePath);
  unsetenv("RB_CODEMAP");
#if defined(__ANDROID__)
  // The property is the Android fallback, and a previous experiment on the tablet could have left it
  // set. Clearing it is what makes "nothing configured" true rather than assumed.
  (void)__system_property_set("debug.retrobug.codemap", "");
#endif

  std::string err = "not cleared";
  CHECK(!exec::trace::StartFromEnvironment(&err));
  CHECK_CTX(err.empty(), "an unconfigured trace reported an error: " + err);
  CHECK(!exec::trace::enabled());
  CHECK(exec::trace::record_count() == 0);

  exec::MachineState state;
  const exec::RunResult r = Call(rig, export_fib->addr, 10, state, export_fib->thumb);
  CHECK_CTX(r.reason == exec::StopReason::Returned,
            "the untraced fib(10) ended in reason " + std::to_string(static_cast<int>(r.reason)));
  CHECK_CTX(static_cast<std::int32_t>(state.r[0]) == 55, "the untraced fib(10) is wrong");
  exec::trace::OnBlockEntry(export_fib->addr, export_fib->thumb);
  CHECK(exec::trace::record_count() == 0);
  CHECK_CTX(!Exists(kTracePath), "a disabled trace created " + std::string{kTracePath});
  exec::trace::Stop();  // idempotent, and must not resurrect a file

  std::printf("# codemap: disabled run recorded nothing\n");
}

}  // namespace

int main() {
  // Disabled first: its `std::remove(kTracePath)` clears a previous run's artifact, and the enabled
  // case then leaves this run's trace in place for `rb_aotgen --trace`.
  a_disabled_trace_records_nothing();
  tier_one_records_the_reached_starts();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
