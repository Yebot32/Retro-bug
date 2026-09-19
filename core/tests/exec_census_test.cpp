/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the block walker and IR-op census (C02, the part that exists before the interpreter).
 *
 * The census is the thing that makes "implement Tier I" finite, so what these assert is that the
 * number it reports is trustworthy: the walk terminates on guest code that loops, it follows
 * successors rather than stopping at the entry block, it is reproducible, and — the part a walker
 * usually gets wrong — when it stops early, that is visible in the result instead of silently
 * shrinking the answer.
 */
#include "rb/exec/census.h"
#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace loader = rb::loader;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                 \
  do {                             \
    ++checks;                      \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)              \
  do {                                       \
    ++checks;                                \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

std::unique_ptr<GuestMemory> make_mem() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  AddressModel m;
  m.kind = AddressModel::kBiased;
  m.host_page_shift = shift;
  std::string err;
  auto gm = GuestMemory::Create(32u << 20, m, &err);
  if (!gm) fail("memory creation failed", __LINE__, err);
  return gm;
}

struct Loaded {
  std::unique_ptr<GuestMemory> mem;
  std::optional<loader::GuestModule> mod;
  bool ok = false;
};

Loaded load_fixture() {
  Loaded l;
  const auto bytes = read_file(RB_FIXTURE_PATH);
  l.mem = make_mem();
  if (bytes.empty() || !l.mem) {
    fail("fixture or memory unavailable", __LINE__);
    return l;
  }
  loader::LoadOptions opts;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  std::string err;
  l.mod = loader::LoadFromBytes(*l.mem, bytes, "librb_fixture.so", opts, &err);
  if (!l.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return l;
  }
  l.ok = true;
  return l;
}

void report(const char* label, const exec::Census& c) {
  std::fprintf(stderr, "[census] %s: %u blocks, %u ops, %zu distinct, %u translate-failures, "
                       "%u truncated, %u left unvisited\n",
               label, c.blocks, c.TotalOps(), c.ops.size(), c.translate_failures, c.truncated_blocks,
               c.blocks_left_unvisited);
  std::fprintf(stderr, "[census]   terminals:");
  for (const auto& [name, n] : c.terminals) std::fprintf(stderr, " %s=%u", name.c_str(), n);
  std::fprintf(stderr, "\n[census]   ops:");
  for (const auto& [name, n] : c.ops) std::fprintf(stderr, " %s=%u", name.c_str(), n);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
}

/// The state an interpreter has to hold. Asserted here rather than in the interpreter's test because
/// it is a promise about a type, and the type is what would change.
void machine_state_holds_no_wide_addresses() {
  exec::MachineState s;
  static_assert(sizeof(s.pc) == 4, "a guest address is 32 bits");
  static_assert(sizeof(s.tp) == 4, "TLS is a guest address, not a host pointer");
  static_assert(sizeof(s.r) == 64 && sizeof(s.d) == 256, "register file layout");
  static_assert(sizeof(s.mode) == sizeof(isa::Mode), "mode is the front end's, not a second enum");
  CHECK(sizeof(s) < 512);  // 16*4 + 32*8 + 4*4 + 1 + 1 + 1 + padding; a wide field shows up here
  CHECK(s.pc == 0 && s.tp == 0 && s.itstate == 0 && s.mode == isa::Mode::Thumb);
}

void census_walks_the_whole_function_graph() {
  Loaded f = load_fixture();
  if (!f.ok) return;

  for (const char* name : {"Java_com_retrobug_fixture_MainActivity_fib",
                           "Java_com_retrobug_fixture_MainActivity_wide",
                           "Java_com_retrobug_fixture_MainActivity_stringFromJNI"}) {
    const auto where = f.mod->FindExport(name);
    if (!where) {
      fail("export missing", __LINE__, name);
      continue;
    }
    const exec::Census c = exec::CensusFrom(*f.mem, where->addr, isa::Mode::Thumb);
    report(name, c);
    CHECK(c.blocks > 0);
    CHECK(c.TotalOps() > 0);
    // A census that quietly hit its block cap would report a smaller graph as if it were the graph.
    CHECK_CTX(c.blocks_left_unvisited == 0, std::string{name} + ": walk stopped early");
    CHECK(c.OpCount("GetRegister") > 0);

    // Reaching more than the entry block is what successor-following means. fib's JNI entry is a two-op
    // thunk (docs/30 §Step 6a), so a census that returned 2 ops walked one block and stopped.
    std::string err;
    const auto first = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Thumb, &err);
    CHECK(first.has_value());
    if (first) {
      CHECK_CTX(c.TotalOps() >= first->ops.size(),
                std::string{name} + ": graph holds fewer ops than its own entry block");
      CHECK_CTX(c.blocks > 1 || first->ops.size() >= 20u,
                std::string{name} + ": only the entry block was reached");
    }

    // Reproducible, because the tier-equivalence test will compare runs of this.
    const exec::Census again = exec::CensusFrom(*f.mem, where->addr, isa::Mode::Thumb);
    CHECK_CTX(c.ops == again.ops, std::string{name} + ": census not reproducible");
    CHECK(c.blocks == again.blocks);

    CHECK_CTX(std::none_of(c.terminals.begin(), c.terminals.end(),
                           [](const auto& e) { return e.first == "unknown"; }),
              std::string{name} + ": a terminal name is not in the table");

    // The op histogram is the work list, so an empty or absurdly large entry is the signal that
    // something upstream (decoder, walk) is broken rather than that the function is simple.
    CHECK(c.ops.size() < 400);
  }
}

/// Deliberately tiny caps: the "I did not finish" flag has to be observable, or it is decoration.
void an_incomplete_walk_says_so() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const auto where = f.mod->FindExport("Java_com_retrobug_fixture_MainActivity_stringFromJNI");
  if (!where) return;
  exec::WalkLimits limits;
  limits.max_blocks = 2;
  const exec::Census small = exec::CensusFrom(*f.mem, where->addr, isa::Mode::Thumb, limits);
  const exec::Census whole = exec::CensusFrom(*f.mem, where->addr, isa::Mode::Thumb);
  CHECK(small.blocks <= 2);
  CHECK_CTX(whole.blocks_left_unvisited == 0, "the unbounded walk should have finished");
  if (whole.blocks > small.blocks) {
    CHECK_CTX(small.blocks_left_unvisited > 0,
              "capped at " + std::to_string(small.blocks) + " of " + std::to_string(whole.blocks) +
                  " blocks but reported nothing left");
    CHECK(small.TotalOps() < whole.TotalOps());
  }
}

/// A census from an address that is not code must not come back looking like a small function. What
/// "looking like a function" means here is specific: the walk has to have said *why* it produced what
/// it did, either by failing to translate or by marking the block as ending on an unfetchable word.
/// Dynarmic does emit a real block for unmapped code — an `ExceptionRaised` epilogue — so a non-zero
/// block count is legitimate; an unflagged one is not.
void census_from_unmapped_code_is_not_silently_empty() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const exec::Census c =
      exec::CensusFrom(*f.mem, f.mod->base + f.mod->extent + (1u << 20), isa::Mode::Thumb);
  CHECK_CTX(c.translate_failures + c.truncated_blocks > 0,
            "blocks=" + std::to_string(c.blocks) + " failures=" + std::to_string(c.translate_failures) +
                " truncated=" + std::to_string(c.truncated_blocks));
  report("unmapped", c);
  // And a walk that starts on unfetchable code must not claim it explored a graph.
  CHECK(c.blocks <= 1);
}

}  // namespace

int main() {
  machine_state_holds_no_wide_addresses();
  census_walks_the_whole_function_graph();
  an_incomplete_walk_says_so();
  census_from_unmapped_code_is_not_silently_empty();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
