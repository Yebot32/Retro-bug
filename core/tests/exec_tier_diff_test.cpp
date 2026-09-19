/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The back-end equivalence test: the same guest program, run by both tiers, compared field by field.
 *
 * This is docs/20 §C02's exit criterion — "both tiers must produce bit-identical `MachineState` for the
 * same guest program" — and it is deliberately called an *equivalence* test rather than a correctness
 * test. Two tiers that agree because they share a bug are entirely possible; what this rules out is the
 * back end compiling something the interpreter does not do, which is the failure that would make Tier J
 * unsafe to ship on. `tests/corpus/oracle-armv7.txt` is what says whether they are both right.
 *
 * The comparison is over every field of MachineState, and a mismatch prints both sides. It is not
 * filtered down to the registers the test "cares about": a tier that disagrees about CPSR's low bits,
 * about the IT mirror, or about a VFP lane nothing read is exactly the kind of disagreement that is
 * invisible if you do not look, and invisible until an app depends on it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/exec/machine_state.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
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

#define CHECK_CTX(cond, detail)                       \
  do {                                                \
    ++checks;                                         \
    if (!(cond)) fail(#cond, __LINE__, detail);        \
  } while (0)

std::string Hex(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

struct Rig {
  std::unique_ptr<GuestMemory> mem;
  std::optional<loader::GuestModule> mod;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::JitExecutor> jit;
  bool ok = false;
};

Rig build() {
  Rig rig;
  const auto bytes = read_file(RB_FIXTURE_PATH);
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = GuestMemory::Create(64u << 20, model, &err);
  if (!rig.mem || bytes.empty()) {
    fail("no guest memory or no fixture", __LINE__, err);
    return rig;
  }
  loader::LoadOptions opts;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return rig;
  }
  if (!rig.mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  rig.interp = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.jit = std::make_unique<exec::JitExecutor>(*rig.mem, kReturnAddress);
  if (!rig.jit->ok()) {
    fail("Tier J could not be built", __LINE__, rig.jit->error());
    return rig;
  }
  rig.ok = true;
  return rig;
}

/// The state a JNI method is entered with: `env` and `clazz` in r0/r1, the declared parameters in r2
/// (and r3 for `it_probe`), LR the sentinel, SP the stack top. `docs/30` §Step 7 records what happens
/// when the argument goes in r0 instead — the guest then computes a correct answer to the wrong question.
exec::MachineState EntryState(exec::GuestAddr where, std::int32_t a, std::int32_t b, bool thumb,
                              bool two = false) {
  exec::MachineState s{};
  s.pc = where;
  s.mode = thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  s.r[2] = static_cast<std::uint32_t>(a);
  if (two) s.r[3] = static_cast<std::uint32_t>(b);
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  s.r[15] = where;
  return s;
}

std::string Describe(const char* which, const exec::RunResult& r, const exec::MachineState& s) {
  std::string out = std::string{which} + ": reason=" + std::to_string(static_cast<int>(r.reason)) +
                    " steps=" + std::to_string(r.instructions_executed);
  if (!r.detail.empty()) out += " detail=" + r.detail;
  out += " r0=" + Hex(s.r[0]) + " r1=" + Hex(s.r[1]) + " r2=" + Hex(s.r[2]) +
         " r4=" + Hex(s.r[4]) + " r5=" + Hex(s.r[5]) + " r13=" + Hex(s.r[13]) +
         " r14=" + Hex(s.r[14]) + " pc=" + Hex(s.pc) + " cpsr=" + Hex(s.cpsr) +
         " fpscr=" + Hex(s.fpscr) + " tp=" + Hex(s.tp) +
         " mode=" + std::to_string(static_cast<int>(s.mode)) +
         " itstate=" + Hex(s.itstate);
  for (int i = 0; i < 32; ++i) {
    if (s.d[i] != 0) out += " d" + std::to_string(i) + "=" + Hex(s.d[i]);
  }
  return out;
}

/// Every field, reported rather than reduced to a bool: the whole value of this test is being able to
/// read the disagreement out of a log line on a device with no debugger attached.
void Compare(const char* what, const exec::MachineState& ia, const exec::RunResult& ir,
             const exec::MachineState& ja, const exec::RunResult& jr) {
  std::string diffs;
  const auto note = [&](const std::string& name, std::uint64_t x, std::uint64_t y) {
    if (x == y) return;
    if (!diffs.empty()) diffs += ", ";
    diffs += name;
    diffs += " ";
    diffs += Hex(x);
    diffs += "!=";
    diffs += Hex(y);
  };
  for (int i = 0; i < 16; ++i) note("r" + std::to_string(i), ia.r[i], ja.r[i]);
  for (int i = 0; i < 32; ++i) note("d" + std::to_string(i), ia.d[i], ja.d[i]);
  note("cpsr", ia.cpsr, ja.cpsr);
  note("fpscr", ia.fpscr, ja.fpscr);
  note("tp", ia.tp, ja.tp);
  note("pc", ia.pc, ja.pc);
  note("mode", static_cast<std::uint64_t>(ia.mode), static_cast<std::uint64_t>(ja.mode));
  note("itstate", ia.itstate, ja.itstate);
  note("reason", static_cast<std::uint64_t>(ir.reason), static_cast<std::uint64_t>(jr.reason));
  CHECK_CTX(diffs.empty(),
            std::string{what} + ": " + diffs + "\n      " + Describe("interp", ir, ia) +
                "\n      " + Describe("jit   ", jr, ja));
}

struct Case {
  const char* name;
  std::int32_t a, b;
  std::int32_t want;  ///< from tests/corpus/oracle-armv7.txt
  bool two = false;   ///< does the method take a second declared parameter?
};

void both_tiers_agree_and_match_hardware() {
  Rig rig = build();
  if (!rig.ok) return;

  const Case cases[] = {
      {"Java_com_retrobug_fixture_MainActivity_fib", 0, 0, 0},
      {"Java_com_retrobug_fixture_MainActivity_fib", 1, 0, 1},
      {"Java_com_retrobug_fixture_MainActivity_fib", 2, 0, 1},
      {"Java_com_retrobug_fixture_MainActivity_fib", 5, 0, 5},
      {"Java_com_retrobug_fixture_MainActivity_fib", 12, 0, 144},
      {"Java_com_retrobug_fixture_MainActivity_fib", 20, 0, 6765},
      {"Java_com_retrobug_fixture_MainActivity_wide", 0, 0, 52},
      {"Java_com_retrobug_fixture_MainActivity_wide", 2, 0, 50},
      {"Java_com_retrobug_fixture_MainActivity_wide", -1, 0, 55},
      // The six it_probe cases and their answers are the oracle's own row, `it_probe: 42 1008 42 104
      // 1005 1000`, with the (a,b) pairs that produced it. Two IT blocks with narrow and 32-bit
      // predicates: the case where a back end and an interpreter are most likely to part company,
      // because the JIT splits a predicated block into one per leg while the IR walk sees the same
      // split as data. Note (5,5) == 42 and (7,3) == 42 by different routes — one takes `addle`, the
      // other `addgt` — which is what makes the pair worth having rather than one of them.
      {"Java_com_retrobug_fixture_MainActivity_it_probe", 7, 3, 42, true},
      {"Java_com_retrobug_fixture_MainActivity_it_probe", 3, 7, 1008, true},
      {"Java_com_retrobug_fixture_MainActivity_it_probe", 5, 5, 42, true},
      {"Java_com_retrobug_fixture_MainActivity_it_probe", 101, 1, 104, true},
      {"Java_com_retrobug_fixture_MainActivity_it_probe", 0, 0, 1005, true},
      {"Java_com_retrobug_fixture_MainActivity_it_probe", -5, 3, 1000, true},
  };

  for (const auto& c : cases) {
    const auto where = rig.mod->FindExport(c.name);
    if (!where) {
      ++checks;
      fail("no export", __LINE__, c.name);
      continue;
    }
    const exec::MachineState start = EntryState(where->addr, c.a, c.b, where->thumb, c.two);
    exec::MachineState si = start;
    exec::MachineState sj = start;
    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const exec::RunResult rj = rig.jit->Run(sj, 8'000'000);
    const std::string what =
        std::string{c.name} + "(" + std::to_string(c.a) + "," + std::to_string(c.b) + ")";
    ++checks;
    if (static_cast<std::int32_t>(si.r[0]) != c.want) {
      fail("Tier I disagrees with hardware", __LINE__,
           what + " gave " + std::to_string(static_cast<std::int32_t>(si.r[0])) + ", oracle said " +
               std::to_string(c.want));
    }
    ++checks;
    if (static_cast<std::int32_t>(sj.r[0]) != c.want) {
      fail("Tier J disagrees with hardware", __LINE__,
           what + " gave " + std::to_string(static_cast<std::int32_t>(sj.r[0])) + ", oracle said " +
               std::to_string(c.want) + "\n      " + Describe("jit", rj, sj));
    }
    Compare(what.c_str(), si, ri, sj, rj);
    std::fprintf(stderr, "[diff] %-58s interp=%d jit=%d want=%d steps I=%u J=%u\n", what.c_str(),
                 static_cast<std::int32_t>(si.r[0]), static_cast<std::int32_t>(sj.r[0]), c.want,
                 ri.instructions_executed, rj.instructions_executed);
  }
}

/// Tier J owns a code cache and Tier I does not, so invalidation has to be tested where it means
/// something: patch a byte of guest code, and both tiers must then produce the *new* answer.
///
/// The new answer is not derived by RetroBug. `patch_probe` and `patch_probe_b` in the fixture differ
/// in exactly one byte — the immediate of one leg of an IT block — and hardware has measured what each
/// of them returns (`tests/corpus/oracle-armv7.txt`). Overwriting the one byte in the first makes its
/// code *literally* the second function's, which is why 42 rather than "some other number" is the
/// expectation: a stale cache cannot produce it.
constexpr std::size_t kPatchWindow = 22;  ///< both probes are this long, and the tail is checked too
constexpr std::int32_t kPatchA_want = 1008;
constexpr std::int32_t kPatchB_want = 42;
constexpr std::int32_t kPatchBoth_want = 42;  ///< (7,3) takes the *other* leg, so the two agree

void invalidation_reaches_the_compiled_code() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto a = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_patch_probe");
  const auto b = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_patch_probe_b");
  if (!a || !b || !a->thumb || !b->thumb) {
    ++checks;
    fail("the fixture has no patch probes", __LINE__);
    return;
  }

  std::uint8_t ba[kPatchWindow] = {};
  std::uint8_t bb[kPatchWindow] = {};
  if (!rig.mem->Read(a->addr, ba, kPatchWindow) || !rig.mem->Read(b->addr, bb, kPatchWindow)) {
    ++checks;
    fail("cannot read the probes' code", __LINE__);
    return;
  }
  // The premise, checked rather than assumed: exactly one byte apart. It is *not* obvious — the first
  // version of this test picked `#5` and `#9` and the assembler answered `#9` with a 32-bit encoding,
  // four bytes apart, so the patch would have been "overwrite eight bytes of a neighbouring
  // instruction" while pretending to be one. If the toolchain's choices move, this check says so.
  std::size_t diff_at = kPatchWindow;
  int diffs = 0;
  for (std::size_t i = 0; i < kPatchWindow; ++i) {
    if (ba[i] != bb[i]) {
      diff_at = i;
      ++diffs;
    }
  }
  CHECK_CTX(diffs == 1, "the two probes differ in " + std::to_string(diffs) +
                            " bytes of their first " + std::to_string(kPatchWindow) +
                            "; the patch is no longer a single byte");
  if (diffs != 1) return;

  const auto run = [&](exec::Executor* tier, exec::GuestAddr where, std::int32_t x, std::int32_t y,
                       exec::MachineState& s) {
    s = EntryState(where, x, y, true, /*two=*/true);
    return tier->Run(s, 100000);
  };

  // Baseline, both tiers, both probes, including the call shape that takes the *untouched* leg.
  for (const auto& c : {std::pair<exec::GuestAddr, std::int32_t>{a->addr, kPatchA_want},
                        std::pair<exec::GuestAddr, std::int32_t>{b->addr, kPatchB_want}}) {
    exec::MachineState si{}, sj{};
    const exec::RunResult ri = run(rig.interp.get(), c.first, 3, 7, si);
    const exec::RunResult rj = run(rig.jit.get(), c.first, 3, 7, sj);
    const std::string what = std::string{"(3,7)@"} + Hex(c.first);
    ++checks;
    if (static_cast<std::int32_t>(si.r[0]) != c.second) {
      fail("baseline Tier I disagrees with hardware", __LINE__,
           what + " gave " + std::to_string(static_cast<std::int32_t>(si.r[0])));
    }
    ++checks;
    if (static_cast<std::int32_t>(sj.r[0]) != c.second) {
      fail("baseline Tier J disagrees with hardware", __LINE__,
           what + " gave " + std::to_string(static_cast<std::int32_t>(sj.r[0])));
    }
    Compare(what.c_str(), si, ri, sj, rj);
  }
  {
    exec::MachineState sa{}, sb{};
    run(rig.jit.get(), a->addr, 7, 3, sa);
    run(rig.jit.get(), b->addr, 7, 3, sb);
    CHECK_CTX(static_cast<std::int32_t>(sa.r[0]) == kPatchBoth_want &&
                  static_cast<std::int32_t>(sb.r[0]) == kPatchBoth_want,
              "the unpatched leg should answer the same in both probes: " +
                  std::to_string(static_cast<std::int32_t>(sa.r[0])) + " and " +
                  std::to_string(static_cast<std::int32_t>(sb.r[0])));
  }

  // A code page is not writable, and that is the design rather than an obstacle (AD-7: a write to
  // executable memory is what has to be detected). Patching from the host therefore goes through the
  // same permission transition a guest's `mprotect` would, which is what makes this exercise the real
  // path instead of a special case the engine would never take.
  std::string perr;
  const exec::GuestAddr page = a->addr & ~static_cast<exec::GuestAddr>(mem::kGuestPage - 1);
  const bool ok = rig.mem->Protect(page, mem::kGuestPage, kRead | kWrite, &perr) &&
                  rig.mem->Write(a->addr + diff_at, &bb[diff_at], 1) &&
                  rig.mem->Protect(page, mem::kGuestPage, kRead | kExec, &perr);
  if (!ok) {
    ++checks;
    fail("could not patch the guest's code page", __LINE__, perr);
    return;
  }
  {
    std::uint8_t now[kPatchWindow] = {};
    CHECK_CTX(rig.mem->Read(a->addr, now, kPatchWindow), "cannot read the patched bytes back");
    CHECK_CTX(std::memcmp(now, bb, kPatchWindow) == 0,
              "after the write, the patched probe's bytes are still not the second probe's, so the "
              "test would be measuring something other than a patched function");
  }

  // Before invalidation: Tier I owns no cache, so it must already answer the new 42 — and Tier J must
  // still answer the *old* 1008, which is the premise of everything below. Measured on the Tab S9:
  // `patched, not yet invalidated: I=42 J=1008`. If a future back end re-reads code and comes back 42
  // here, this test has nothing left to demonstrate and should be deleted rather than left to pass
  // vacuously, so it fails loudly instead.
  {
    exec::MachineState si{}, sj{};
    const exec::RunResult ri = run(rig.interp.get(), a->addr, 3, 7, si);
    const exec::RunResult rj = run(rig.jit.get(), a->addr, 3, 7, sj);
    std::fprintf(stderr,
                 "[diff] patched, not yet invalidated: I=%d (want %d)  J=%d (1008 means still cached)\n",
                 static_cast<std::int32_t>(si.r[0]), kPatchB_want, static_cast<std::int32_t>(sj.r[0]));
    CHECK_CTX(ri.reason == exec::StopReason::Returned && rj.reason == exec::StopReason::Returned,
              "a patched probe must still run to a return: I " + ri.detail + " / J " + rj.detail);
    CHECK_CTX(static_cast<std::int32_t>(si.r[0]) == kPatchB_want,
              "Tier I, which owns no cache, reads " + std::to_string(static_cast<std::int32_t>(si.r[0])) +
                  " of patched code; want " + std::to_string(kPatchB_want));
    CHECK_CTX(static_cast<std::int32_t>(sj.r[0]) == kPatchA_want,
              "Tier J answered " + std::to_string(static_cast<std::int32_t>(sj.r[0])) +
                  " before anything invalidated its cache; it has no cache to invalidate, so this "
                  "test is measuring nothing");
  }

  rig.jit->Invalidate(a->addr, a->addr + kPatchWindow);

  exec::MachineState si{}, sj{};
  const exec::RunResult ri = run(rig.interp.get(), a->addr, 3, 7, si);
  const exec::RunResult rj = run(rig.jit.get(), a->addr, 3, 7, sj);
  CHECK_CTX(ri.reason == exec::StopReason::Returned && rj.reason == exec::StopReason::Returned,
            "after invalidation: I " + ri.detail + " / J " + rj.detail);
  ++checks;
  if (static_cast<std::int32_t>(sj.r[0]) != kPatchB_want) {
    fail("Tier J still runs the cached code", __LINE__,
         "patched probe returns " + std::to_string(static_cast<std::int32_t>(sj.r[0])) +
             " from Tier J and " + std::to_string(static_cast<std::int32_t>(si.r[0])) +
             " from Tier I; hardware says the code it now holds returns " +
             std::to_string(kPatchB_want));
  }
  ++checks;
  if (static_cast<std::int32_t>(si.r[0]) != kPatchB_want) {
    fail("Tier I disagrees with hardware on patched code", __LINE__,
         std::to_string(static_cast<std::int32_t>(si.r[0])));
  }
  Compare("patched (3,7) after invalidation", si, ri, sj, rj);
  std::fprintf(stderr, "[diff] patched probe, one byte at +%zu: before=%d after I=%d J=%d want=%d\n",
               diff_at, kPatchA_want, static_cast<std::int32_t>(si.r[0]),
               static_cast<std::int32_t>(sj.r[0]), kPatchB_want);

  // Put it back. The rig is per-test, but leaving the guest image patched inside its own memory makes
  // every later case in this file a test of a function nothing else describes.
  std::string rerr;
  rig.mem->Protect(page, mem::kGuestPage, kRead | kWrite, &rerr);
  rig.mem->Write(a->addr + diff_at, &ba[diff_at], 1);
  rig.mem->Protect(page, mem::kGuestPage, kRead | kExec, &rerr);
  rig.jit->Invalidate(a->addr, a->addr + kPatchWindow);
  exec::MachineState back{};
  run(rig.jit.get(), a->addr, 3, 7, back);
  CHECK_CTX(static_cast<std::int32_t>(back.r[0]) == kPatchA_want,
            "restoring the byte did not restore the answer: " +
                std::to_string(static_cast<std::int32_t>(back.r[0])) + " " + rerr);
}

/// A fault has to be a fault in both tiers, with a reason rather than a plausible zero.
void unmapped_code_faults_in_both_tiers() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::MachineState s = EntryState(0x0090'0000u, 0, 0, true);
  const exec::RunResult ri = rig.interp->Run(s, 1000);
  exec::MachineState s2 = s;
  const exec::RunResult rj = rig.jit->Run(s2, 1000);
  CHECK_CTX(ri.reason == exec::StopReason::Fault,
            "Tier I: reason " + std::to_string(static_cast<int>(ri.reason)) + " " + ri.detail);
  CHECK_CTX(rj.reason == exec::StopReason::Fault,
            "Tier J: reason " + std::to_string(static_cast<int>(rj.reason)) + " " + rj.detail);
  std::fprintf(stderr, "[diff] unmapped fetch: I said \"%s\", J said \"%s\"\n", ri.detail.c_str(),
               rj.detail.c_str());
}

}  // namespace

int main() {
  both_tiers_agree_and_match_hardware();
  invalidation_reaches_the_compiled_code();
  unmapped_code_faults_in_both_tiers();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
