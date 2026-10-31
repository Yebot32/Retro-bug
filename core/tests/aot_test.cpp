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

}  // namespace

int main() {
  fib_matches_tier_one_for_every_n();
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
