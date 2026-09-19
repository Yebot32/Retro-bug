/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the front-end wrapper (C01), against real Thumb-2 code: the same armeabi-v7a fixture the
 * loader test consumes, loaded through the loader and then translated. Nothing here is a hand-made
 * instruction sequence, which matters because the point is that a *compiler's* output reaches the
 * front end intact.
 *
 * Deliberately free of assertions on which IR instructions the fixture compiles *to*. Naming the ops
 * a Thumb-2 prologue should produce would tie this test to Dynarmic's current lowering choices, and
 * those are its own business. What RetroBug depends on is the wrapper's contract: the IR is
 * reachable, it is reachable consistently, the mode actually reaches the decoder, unmapped code is
 * reported rather than decoded as zero, and the fetched-word set is something a code cache can key
 * invalidation on. The op list is printed rather than asserted, so the shape stays visible to whoever
 * writes Tier I's dispatch table next.
 */
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

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& e : v) {
    if (!s.empty()) s += " ";
    s += e;
  }
  return s;
}

void dump(const char* label, const isa::TranslatedBlock& b) {
  std::fprintf(stderr, "[isa] %s @0x%x: %zu ops, terminal=%s, %zu code words, truncated=%d\n      %s\n",
               label, b.entry, b.ops.size(), b.terminal.c_str(), b.code_words.size(),
               b.truncated_by_unmapped_read ? 1 : 0, Join(b.ops).c_str());
  std::fflush(stderr);
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

/// The fixture, through the loader that has its own green test, with imports answered by an address
/// that is recognisable in a dump.
Loaded load_fixture() {
  Loaded l;
  const auto bytes = read_file(RB_FIXTURE_PATH);
  l.mem = make_mem();
  if (bytes.empty() || !l.mem) {
    fail("fixture or memory unavailable", __LINE__);
    return l;
  }
  loader::LoadOptions opts;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> {
    return 0x0900'0000u;
  };
  std::string err;
  l.mod = loader::LoadFromBytes(*l.mem, bytes, "librb_fixture.so", opts, &err);
  if (!l.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return l;
  }
  l.ok = true;
  return l;
}

/// Shared expectations for any block that claims to have been translated.
void check_block_shape(const char* label, const isa::TranslatedBlock& b) {
  CHECK(!b.ops.empty());
  // "unknown" means the vendored terminal list changed shape and this wrapper's table is now
  // describing the wrong thing — which would otherwise be invisible in every later tier.
  CHECK_CTX(b.terminal != "unknown", std::string{label} + ": terminal is not in kTerminalNames");
  CHECK_CTX(std::none_of(b.ops.begin(), b.ops.end(),
                         [](const std::string& o) { return o.empty(); }),
            std::string{label} + ": an opcode has no name");
  dump(label, b);
}

void translates_the_fixtures_functions() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const std::uint32_t base = f.mod->base;
  const std::uint32_t window = f.mem->window_bytes();

  for (const char* name : {"Java_com_retrobug_fixture_MainActivity_fib",
                           "Java_com_retrobug_fixture_MainActivity_wide",
                           "Java_com_retrobug_fixture_MainActivity_stringFromJNI"}) {
    const auto where = f.mod->FindExport(name);
    CHECK_CTX(where.has_value(), std::string{"export "} + name);
    if (!where) continue;
    CHECK_CTX(where->thumb, std::string{name} + " is not Thumb in this build");

    std::string err;
    const auto b = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Thumb, &err);
    CHECK_CTX(b.has_value(), std::string{name} + " did not translate: " + err);
    if (!b) continue;
    check_block_shape(name, *b);
    CHECK(!b->truncated_by_unmapped_read);
    CHECK(b->entry == where->addr);
    CHECK(b->mode == isa::Mode::Thumb);
    CHECK(!b->code_words.empty());

    // Every recorded word has to be aligned, distinct, inside the guest window, and mapped. This is
    // the set a code cache invalidates against: a word outside it would make the cache deaf to the
    // one write that should have thrown the block away.
    std::vector<std::uint32_t> sorted = b->code_words;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    const std::string tag{name};
    for (const std::uint32_t w : b->code_words) {
      CHECK_CTX((w & 3u) == 0, tag + ": fetch at " + Hex(w) + " is not word-aligned");
      CHECK_CTX(w < window, tag + ": fetch at " + Hex(w) + " is past the guest window");
      CHECK_CTX(f.mem->IsMapped(w), tag + ": fetch at " + Hex(w) + " is not mapped");
      // Translating one block must not look outside the image it belongs to; if it ever did, the
      // invalidation range for this block would include somebody else's memory.
      CHECK_CTX(w >= base && w < base + f.mod->extent,
                tag + ": fetch at " + Hex(w) + " is outside the module (" + Hex(base) + "+" +
                    std::to_string(f.mod->extent) + ")");
    }
  }
}

void translation_is_deterministic() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const auto where = f.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!where) return;
  std::string err;
  const auto a = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Thumb, &err);
  const auto b = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Thumb, &err);
  CHECK(a.has_value());
  CHECK(b.has_value());
  if (!a || !b) return;
  // Two translations of identical bytes must agree, or the code cache's dedup and the tier-equivalence
  // test are comparing things that were never comparable in the first place.
  CHECK_CTX(a->ops == b->ops, "same bytes translated into different IR");
  CHECK_CTX(a->terminal == b->terminal, "same bytes ended in different terminals");
  CHECK(a->code_words == b->code_words);
}

/// Mode is not decoration. If the wrapper's Thumb flag failed to reach the decoder, ARM and Thumb
/// translations of the same words would agree; for genuine Thumb-2 code they must not.
void mode_reaches_the_decoder() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const auto where = f.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!where) return;
  std::string err;
  const auto thumb = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Thumb, &err);
  const auto arm = isa::TranslateOne(*f.mem, where->addr, isa::Mode::Arm, &err);
  CHECK(thumb.has_value());
  if (!thumb) return;
  const bool differs = !arm.has_value() || arm->ops != thumb->ops || arm->terminal != thumb->terminal;
  CHECK_CTX(differs, "ARM and Thumb translation of the same words were identical");
  if (arm) dump("same-bytes-as-arm", *arm);
}

/// `TranslateOne` takes a **byte** address plus an explicit mode, so an odd address is not a thing to
/// fetch in either mode. This is a contract statement, not a robustness one: the odd values that
/// crowd ARM code are the ABI's Thumb tag on a *symbol* or function pointer, and `CodeAddress` is
/// where that tag gets split off (`docs/30` §Step 5e) — a caller that passes a symbol value straight
/// in here has already lost the distinction the loader preserved.
void misaligned_entry_is_refused() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  for (const auto mode : {isa::Mode::Arm, isa::Mode::Thumb}) {
    std::string err;
    CHECK(!isa::TranslateOne(*f.mem, 0x1001u, mode, &err));
    CHECK_CTX(err.find("aligned") != std::string::npos,
              std::string{mode == isa::Mode::Arm ? "arm" : "thumb"} + ": error was: " + err);
  }
  // An ARM fetch must also be 4-aligned, which a Thumb fetch legitimately is not.
  std::string err;
  CHECK(!isa::TranslateOne(*f.mem, 0x1002u, isa::Mode::Arm, &err));
  CHECK(err.find("aligned") != std::string::npos);
  err.clear();
  CHECK(!isa::TranslateOne(*f.mem, 0x1002u, isa::Mode::Thumb, &err) || err.empty());
  CHECK_CTX(err.find("aligned") == std::string::npos,
            "a halfword-aligned Thumb fetch was refused as misaligned: " + err);
}

/// Unmapped code must never be decoded as zero.
void unmapped_code_is_reported_not_decoded() {
  Loaded f = load_fixture();
  if (!f.ok) return;
  const std::uint32_t far_away = f.mod->base + f.mod->extent + (1u << 20);  // inside the window,
  std::string err;                                                          // mapped by nothing
  const auto b = isa::TranslateOne(*f.mem, far_away, isa::Mode::Thumb, &err);
  if (b) {
    // Acceptable only if it says so: an empty-but-successful translation is precisely the silent
    // nothing a cached block would then serve to the guest forever.
    CHECK_CTX(b->truncated_by_unmapped_read, "translation past the mapped window claimed success");
    dump("unmapped-window", *b);
  } else {
    CHECK(!err.empty());
  }
}

}  // namespace

int main() {
  translates_the_fixtures_functions();
  translation_is_deterministic();
  mode_reaches_the_decoder();
  misaligned_entry_is_refused();
  unmapped_code_is_reported_not_decoded();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
