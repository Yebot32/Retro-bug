/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * AOT task 3's exit test: the offline-emitted `Java_com_retrobug_fixture_MainActivity_fib` runs on the
 * device through the AOT dispatcher and produces the same answer and the same final `MachineState` as
 * Tier I on the same fixture bytes.
 *
 * Device-only by construction: the translated code is AArch64 and the fixture is an armeabi-v7a ELF.
 * Every other host prints `# aot: not an AArch64 host — skipped` and exits 0, so the x86_64 lanes stay
 * green without pretending to have run anything.
 *
 * ---------------------------------------------------------------------------------------------
 * Regenerating `aot_fixture.S` and `aot_fixture_table.cpp`
 * ---------------------------------------------------------------------------------------------
 * The emitter (`rb_aotgen --emit`) is built only for AArch64 hosts. From the repository root, with the
 * phone connected:
 *
 *   powershell -ExecutionPolicy Bypass -File build/build_direct.ps1 \
 *       -BuildDir build/tests-arm64-v8a rb_aotgen
 *   adb push build/tests-arm64-v8a/tools/aotgen/rb_aotgen /data/local/tmp/rb_aotgen
 *   adb push build/tests-arm64-v8a/librb_fixture.so /data/local/tmp/librb_fixture.so
 *   adb shell "chmod 755 /data/local/tmp/rb_aotgen"
 *   adb shell "cd /data/local/tmp && ./rb_aotgen --emit librb_fixture.so --out aot_fixture --base 0x10000"
 *   adb pull /data/local/tmp/aot_fixture.S core/tests/aot_fixture.S
 *   adb pull /data/local/tmp/aot_fixture_table.cpp core/tests/aot_fixture_table.cpp
 *
 * The fixture itself is built by `tools/run_engine_tests.sh` with the NDK (`-mthumb`, armeabi-v7a).
 * The generated table carries an FNV-1a-64 of the exact `.so` the tool read; this test recomputes it,
 * so a fixture rebuilt without regenerating the artifacts fails here before the first guest
 * instruction instead of comparing two different byte streams. The image must be loaded at the bias
 * the generated blocks baked (`0x10000`, the tool's default); this test asks the loader for it with
 * `LoadOptions::hint` and asserts the loader honoured the request.
 */
#if defined(__aarch64__)

#include "rb/exec/aot.h"
#include "rb/exec/interp.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

/// Defined by the generated `aot_fixture_table.cpp`. Declared at file scope, not inside the test: a
/// block-scope `extern` inside this file's unnamed namespace would take internal linkage (C++ [basic.link]),
/// and the definition in the generated table has external linkage.
extern const rb::exec::AotImageInfo kAotFixtureImage;

/// Defined by the generated `aot_fixture.S`: a pair of words in `.rodata` holding the emitted code
/// size and block count. They are the `.S` half of the table/assembly pairing check.
extern "C" {
extern const std::uint32_t aot_fixture_code_bytes;
extern const std::uint32_t aot_fixture_block_count;
}

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

#define CHECK(...)                    \
  do {                                \
    ++checks;                         \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)              \
  do {                                       \
    ++checks;                                \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;

std::vector<std::uint8_t> ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

std::uint64_t Fnv1a64(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t h = 0xcbf2'9ce4'8422'2325ull;
  for (const std::uint8_t b : bytes) {
    h ^= b;
    h *= 0x0000'0100'0000'01b3ull;
  }
  return h;
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

std::string Hex(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

struct Rig {
  std::unique_ptr<mem::GuestMemory> mem;
  std::optional<loader::GuestModule> mod;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::AotExecutor> aot;
  bool ok = false;
};

Rig Build(const exec::AotImageInfo& image) {
  Rig rig;
  const auto bytes = ReadFile(RB_FIXTURE_PATH);
  if (bytes.empty()) {
    std::printf("# aot: %s is missing — skipped\n", RB_FIXTURE_PATH);
    return rig;
  }
  if (Fnv1a64(bytes) != image.fingerprint) {
    fail("the committed AOT table is for a different librb_fixture.so",
         __LINE__,
         "fixture fingerprint " + Hex(Fnv1a64(bytes)) + ", table says " + Hex(image.fingerprint) +
             "; regenerate aot_fixture.S/aot_fixture_table.cpp (see this file's header)");
    return rig;
  }
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = mem::GuestMemory::Create(64u << 20, model, &err);
  if (!rig.mem) {
    fail("could not create guest memory", __LINE__, err);
    return rig;
  }
  loader::LoadOptions opts;
  // The emitted blocks baked guest PCs at this bias. Asking for it is what makes the same bytes run:
  // a different placement would leave every baked PC pointing somewhere else.
  opts.hint = image.load_bias;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return rig;
  }
  if (rig.mod->base != image.load_bias) {
    fail("the loader did not honour the AOT load bias", __LINE__,
         "base " + Hex(rig.mod->base) + ", emitted for " + Hex(image.load_bias));
    return rig;
  }
  if (!rig.mem->Map(kStackTop - (64u << 10), 64u << 10, mem::kRead | mem::kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  rig.interp = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.aot = std::make_unique<exec::AotExecutor>(*rig.mem, kReturnAddress, image);
  if (!rig.aot->ok()) {
    fail("AOT executor could not be built", __LINE__, rig.aot->error());
    return rig;
  }
  rig.ok = true;
  return rig;
}

exec::MachineState EntryState(exec::GuestAddr where, std::int32_t n, bool thumb) {
  exec::MachineState s{};
  s.pc = where;
  s.mode = thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  // JNI: env in r0, jclass in r1, the declared parameter in r2.
  s.r[0] = 0;
  s.r[1] = 0;
  s.r[2] = static_cast<std::uint32_t>(n);
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  s.r[15] = where;
  return s;
}

std::int32_t Fib(int n) {
  return n < 2 ? n : Fib(n - 1) + Fib(n - 2);
}

std::string StateDiff(const exec::MachineState& a, const exec::MachineState& b) {
  std::string d;
  const auto note = [&](const std::string& name, std::uint64_t x, std::uint64_t y) {
    if (x == y) return;
    if (!d.empty()) d += ", ";
    d += name + " " + Hex(x) + "!=" + Hex(y);
  };
  for (int i = 0; i <= 15; ++i) note("r" + std::to_string(i), a.r[i], b.r[i]);
  for (int i = 0; i < 32; ++i) note("d" + std::to_string(i), a.d[i], b.d[i]);
  note("cpsr", a.cpsr, b.cpsr);
  note("fpscr", a.fpscr, b.fpscr);
  note("tp", a.tp, b.tp);
  note("pc", a.pc, b.pc);
  note("mode", static_cast<std::uint64_t>(a.mode), static_cast<std::uint64_t>(b.mode));
  note("itstate", a.itstate, b.itstate);
  return d;
}

void fib_matches_tier_one_for_every_n() {
  const exec::AotImageInfo& image = kAotFixtureImage;
  Rig rig = Build(image);
  if (!rig.ok) return;

  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  CHECK_CTX(fib.has_value(), "the fixture exports the fib JNI method");
  if (!fib) return;

  // The table is keyed by the code map's file-relative offsets; the export's guest address minus the
  // image's base is exactly that unit.
  const std::uint32_t text_base = image.load_bias + image.text_vaddr;
  const std::uint32_t fib_off = fib->addr - text_base;
  CHECK(rig.aot->image().Resolve(fib_off, fib->thumb) != nullptr);
  CHECK(rig.aot->image().Resolve(0x00ff'fffeu, true) == nullptr);

  for (int n = 0; n <= 20; ++n) {
    const exec::MachineState start = EntryState(fib->addr, n, fib->thumb);
    exec::MachineState si = start;
    exec::MachineState sj = start;
    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const exec::RunResult rj = rig.aot->Run(sj, 8'000'000);
    const std::string what = "fib(" + std::to_string(n) + ")";
    CHECK_CTX(ri.reason == exec::StopReason::Returned,
              what + " (Tier I) ended in reason " + std::to_string(static_cast<int>(ri.reason)) +
                  " " + ri.detail);
    CHECK_CTX(rj.reason == exec::StopReason::Returned,
              what + " (AOT) ended in reason " + std::to_string(static_cast<int>(rj.reason)) + " " +
                  rj.detail);
    CHECK_CTX(static_cast<std::int32_t>(sj.r[0]) == Fib(n),
              what + " = " + std::to_string(static_cast<std::int32_t>(sj.r[0])) + ", expected " +
                  std::to_string(Fib(n)));
    CHECK_CTX(sj.r[0] == si.r[0], what + " AOT r0 " + Hex(sj.r[0]) + " != Tier I " + Hex(si.r[0]));
    CHECK(rj.instructions_executed > 0);
    // Every field the brief names, plus the rest of the MachineState the differential compares: the
    // two tiers have to agree on the whole value, not just the answer.
    const std::string diff = StateDiff(si, sj);
    CHECK_CTX(diff.empty(), what + ": " + diff);
    std::fprintf(stderr,
                 "[aot] fib(%2d)=%d fallbacks=%zu aot-insns=%u tier1-steps=%u cpsr=%x fpscr=%x\n", n,
                 static_cast<std::int32_t>(sj.r[0]), rig.aot->fallback_count(),
                 rj.instructions_executed, ri.instructions_executed, sj.cpsr, sj.fpscr);
  }
  CHECK_CTX(Fib(20) == 6765, "the oracle value for fib(20) is 6765");
  CHECK_CTX(rig.aot->fallback_count() == 0,
            "the fib run reached " + std::to_string(rig.aot->fallback_count()) +
                " target(s) with no emitted block; every target must resolve");
  std::printf("# aot: fib 0..20 compared, %zu AOT fallbacks (expected 0)\n",
              rig.aot->fallback_count());
}

/// The `.S` and the table are one emission, not two files that happen to share a fingerprint. The
/// generated table carries the code size and block count; the assembly repeats both in its header and
/// in a `.rodata` word pair the table declares. This asserts both halves agree with each other.
void table_and_assembly_are_one_emission() {
  const exec::AotImageInfo& image = kAotFixtureImage;
  CHECK(image.block_count == image.count);
  CHECK(image.code_bytes > 0);
  CHECK_CTX(aot_fixture_code_bytes == image.code_bytes,
            "the .S says " + std::to_string(aot_fixture_code_bytes) + " code bytes, the table " +
                std::to_string(image.code_bytes));
  CHECK_CTX(aot_fixture_block_count == image.block_count,
            "the .S says " + std::to_string(aot_fixture_block_count) + " blocks, the table " +
                std::to_string(image.block_count));
}

/// A live IT state or FPSCR mode is part of a block's identity in Dynarmic's `LocationDescriptor`, but
/// the AOT table is deliberately keyed on `(offset, thumb)` only. Every emitted block is for IT=0 and
/// the default FPCR, so an entry carrying either other fact must not resolve into that code — it has to
/// fall back and let Tier I, which keys blocks on the descriptor, predicate and round correctly.
void non_default_descriptors_fall_back() {
  Rig rig = Build(kAotFixtureImage);
  if (!rig.ok) return;
  const auto it = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_it_probe");
  CHECK_CTX(it.has_value(), "the fixture exports it_probe");
  if (!it) return;
  const std::uint32_t text_base = kAotFixtureImage.load_bias + kAotFixtureImage.text_vaddr;
  // The point of this case is the descriptor check, not a missing block: the entry must resolve.
  CHECK_CTX(rig.aot->image().Resolve(it->addr - text_base, it->thumb) != nullptr,
            "it_probe's entry has no emitted block, so this case would not test the descriptor");
  if (rig.aot->image().Resolve(it->addr - text_base, it->thumb) == nullptr) return;

  const auto run_both = [&](const exec::MachineState& start, std::int32_t expected, const char* what) {
    exec::MachineState si = start;
    exec::MachineState sj = start;
    const std::size_t before = rig.aot->fallback_count();
    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const exec::RunResult rj = rig.aot->Run(sj, 8'000'000);
    CHECK_CTX(ri.reason == exec::StopReason::Returned, std::string{what} + " (Tier I) did not return");
    CHECK_CTX(rj.reason == exec::StopReason::Returned, std::string{what} + " (AOT) did not return");
    CHECK_CTX(static_cast<std::int32_t>(si.r[0]) == expected,
              std::string{what} + ": Tier I answered " + std::to_string(static_cast<std::int32_t>(si.r[0])) +
                  ", the expected result is " + std::to_string(expected));
    CHECK_CTX(sj.r[0] == si.r[0], std::string{what} + ": AOT " + Hex(sj.r[0]) + " != Tier I " +
                                          Hex(si.r[0]));
    const std::string diff = StateDiff(si, sj);
    CHECK_CTX(diff.empty(), std::string{what} + ": " + diff);
    CHECK_CTX(rig.aot->fallback_count() > before,
              std::string{what} + " entered AOT code instead of falling back");
  };

  // A single-leg `IT LT` on it_probe's first instruction: the `cmp` is predicated and skipped (Z=1
  // makes LT false), then the function's own `ite gt` runs normally. 3 -> 8 -> 1008.
  {
    exec::MachineState start = EntryState(it->addr, 3, it->thumb);
    start.r[3] = 7;
    start.cpsr = exec::CpsrWithIt(start.cpsr, 0xb8) | (1u << 30);  // IT LT; Z set
    start.itstate = 0xb8;
    run_both(start, 1008, "it_probe with a live IT state");
  }

  // A nonzero FPSCR rounding mode is the other half of the descriptor: the block's FP immediates were
  // emitted for round-to-nearest, so entering it under a different mode is the same refusal. Fib is the
  // case that actually discriminates the FPSCR half: it_probe would fall back on its IT state anyway,
  // while fib with no IT state must fall back only because the rounding mode differs.
  {
    const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
    CHECK_CTX(fib.has_value(), "the fixture exports fib for the FPSCR case");
    if (fib && rig.aot->image().Resolve(fib->addr - text_base, fib->thumb) != nullptr) {
      exec::MachineState start = EntryState(fib->addr, 1, fib->thumb);
      start.fpscr = 1u << 22;  // FPSCR.RMode = round toward +infinity
      run_both(start, 1, "fib under a non-default FPSCR.RMode");
    }
  }
}

/// A table with a reached block removed must run the rest, hand the hole to Tier I, and count the
/// fallback in the same guest-instruction unit as the AOT ticks. A second copy with every block but one
/// unrelated one removed forces the whole run through the fallback, including its terminal exit — the
/// path that used to return Tier I's `RunResult` and silently drop `executed_` and every earlier
/// fallback.
void a_missing_block_falls_back_and_is_counted() {
  const exec::AotImageInfo& image = kAotFixtureImage;
  Rig full = Build(image);
  if (!full.ok) return;
  const auto fib_full = full.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  CHECK_CTX(fib_full.has_value(), "the fixture exports fib");
  if (!fib_full) return;
  exec::MachineState sf = EntryState(fib_full->addr, 10, fib_full->thumb);
  const exec::RunResult rf = full.aot->Run(sf, 8'000'000);
  CHECK_CTX(rf.reason == exec::StopReason::Returned, "the complete-table reference run did not return");
  CHECK_CTX(sf.r[0] == 55, "the complete-table reference run is not fib(10) = 55");

  // One block removed: the entry block the dispatcher starts on. The rest of the image is untouched,
  // so the run proves that a hole in the table is a fallback rather than a failure.
  {
    const std::uint32_t text_base = image.load_bias + image.text_vaddr;
    const std::uint32_t fib_off = fib_full->addr - text_base;
    std::vector<exec::AotBlock> blocks;
    bool removed = false;
    for (std::size_t i = 0; i < image.count; ++i) {
      if (image.blocks[i].guest_off == fib_off &&
          (image.blocks[i].flags & (fib_full->thumb ? exec::kAotThumb : exec::kAotArm)) != 0) {
        removed = true;
        continue;
      }
      blocks.push_back(image.blocks[i]);
    }
    CHECK_CTX(removed, "the fixture table contains the fib entry block this case removes");
    if (!removed) return;
    exec::AotImageInfo copy = image;
    copy.blocks = blocks.data();
    copy.count = blocks.size();
    copy.block_count = static_cast<std::uint32_t>(blocks.size());

    Rig rig = Build(copy);
    if (!rig.ok) return;
    const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
    if (!fib) return;
    exec::MachineState si = EntryState(fib->addr, 10, fib->thumb);
    exec::MachineState sj = si;
    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const exec::RunResult rj = rig.aot->Run(sj, 8'000'000);
    CHECK(ri.reason == exec::StopReason::Returned);
    CHECK(rj.reason == exec::StopReason::Returned);
    CHECK_CTX(rig.aot->fallback_count() > 0, "a removed reached block produced no fallback");
    CHECK(sj.r[0] == 55);
    CHECK(StateDiff(si, sj).empty());
    CHECK(rj.instructions_executed > 0);
  }

  // Every fib block removed, one unrelated block kept so the image is still loadable: the whole
  // program is a chain of dispatcher misses and its final exit is a fallback terminal.
  {
    std::size_t keep = 0;
    for (std::size_t i = 1; i < image.count; ++i) {
      if (image.blocks[i].guest_off > image.blocks[keep].guest_off) keep = i;
    }
    std::vector<exec::AotBlock> blocks{image.blocks[keep]};
    exec::AotImageInfo copy = image;
    copy.blocks = blocks.data();
    copy.count = blocks.size();
    copy.block_count = 1;

    Rig rig = Build(copy);
    if (!rig.ok) return;
    const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
    if (!fib) return;
    exec::MachineState si = EntryState(fib->addr, 10, fib->thumb);
    exec::MachineState sj = si;
    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const exec::RunResult rj = rig.aot->Run(sj, 8'000'000);
    CHECK(ri.reason == exec::StopReason::Returned);
    CHECK(rj.reason == exec::StopReason::Returned);
    CHECK_CTX(rig.aot->fallback_count() > 0, "the all-miss run reported no fallbacks");
    CHECK(sj.r[0] == 55);
    CHECK(StateDiff(si, sj).empty());
    // Both numbers are guest instructions. This run executes the same program, and each fallback
    // counts one chunk where the complete table counts one per instruction, so it cannot exceed the
    // reference. The old terminal path returned Tier I's IR-step count (thousands); the old map-halt
    // path added 65,536 IR steps per fallback; both blow past this bound.
    CHECK_CTX(rj.instructions_executed > 0, "the fallback-only run reported no work");
    CHECK_CTX(rj.instructions_executed <= rf.instructions_executed,
              "fallback-only run " + std::to_string(rj.instructions_executed) +
                  " > complete-table run " + std::to_string(rf.instructions_executed));
    CHECK_CTX(rj.instructions_executed >= rig.aot->fallback_count(),
              "reported " + std::to_string(rj.instructions_executed) + " instructions for " +
                  std::to_string(rig.aot->fallback_count()) + " fallbacks");
    std::printf("# aot: fallback-only fib(10): %u instructions over %zu fallbacks (full table %u)\n",
                rj.instructions_executed, rig.aot->fallback_count(), rf.instructions_executed);
  }
}

/// The budget is in guest instructions and the fallback is part of it. A three-instruction budget must
/// stop and leave a state the next call can resume; a guest loop with no emitted block must stop at the
/// budget rather than spin inside the dispatcher's fallback.
void the_budget_covers_aot_and_fallback_work() {
  Rig rig = Build(kAotFixtureImage);
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  CHECK_CTX(fib.has_value(), "the fixture exports fib");
  if (!fib) return;

  exec::MachineState state = EntryState(fib->addr, 10, fib->thumb);
  const exec::RunResult small = rig.aot->Run(state, 3);
  CHECK(small.reason == exec::StopReason::Budget);
  CHECK(small.instructions_executed > 0);
  // The Tier I/J invariant: a budget stop leaves a state that can be resumed, and doing so reaches the
  // same answer the uninterrupted run does.
  const exec::RunResult rest = rig.aot->Run(state, 8'000'000);
  CHECK(rest.reason == exec::StopReason::Returned);
  CHECK(state.r[0] == 55);

  // `wide` is one of the fixture functions the emitter refuses (VFP/coprocessor blocks), so this is
  // the refused-block path: no table entry exists anywhere in it.
  const auto wide = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_wide");
  if (wide) {
    const std::uint32_t text_base = kAotFixtureImage.load_bias + kAotFixtureImage.text_vaddr;
    CHECK_CTX(rig.aot->image().Resolve(wide->addr - text_base, wide->thumb) == nullptr,
              "wide's entry unexpectedly has an emitted block");
  }

  // A two-encoding loop in guest memory that the image cannot cover: `movs r0, #0; b .-4`. The `b`
  // targets the `movs`, so every iteration re-enters the same block and pays for its IR operation.
  // That matters: Tier I counts IR operations, and a bare `b .` block has none, so Tier I itself would
  // spin inside one `Run` without consuming its step budget. With a body instruction, every dispatch
  // misses and the only way out is the AOT budget.
  constexpr exec::GuestAddr kPage = 0x0180'0000u;
  std::string err;
  if (!rig.mem->Map(kPage, mem::kGuestPage, mem::kRead | mem::kWrite | mem::kExec, &err)) {
    CHECK_CTX(false, "could not map the spin-loop page: " + err);
    return;
  }
  const std::uint32_t spin = 0xe7fd'2000u;  // little-endian: 0x2000 `movs r0, #0`, 0xe7fd `b .-4`
  if (!rig.mem->Write(kPage, &spin, 4)) {
    CHECK_CTX(false, "could not write the spin loop");
    return;
  }
  const std::size_t before = rig.aot->fallback_count();
  exec::MachineState loop = EntryState(kPage, 0, true);
  const exec::RunResult r = rig.aot->Run(loop, 10);
  CHECK_CTX(r.reason == exec::StopReason::Budget,
            "the spin loop ended in reason " + std::to_string(static_cast<int>(r.reason)));
  CHECK(r.instructions_executed > 0);
  CHECK_CTX(rig.aot->fallback_count() > before, "the spin loop never reached the fallback");
  CHECK_CTX(loop.pc == kPage, "the spin loop left pc at " + Hex(loop.pc));
}

}  // namespace

int main() {
  fib_matches_tier_one_for_every_n();
  table_and_assembly_are_one_emission();
  non_default_descriptors_fall_back();
  a_missing_block_falls_back_and_is_counted();
  the_budget_covers_aot_and_fallback_work();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

#else  // !__aarch64__

#include <cstdio>

int main() {
  std::printf("# aot: not an AArch64 host — skipped\n");
  return 0;
}

#endif
