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
#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/exec/machine_state.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
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
namespace abi = rb::abi;
namespace sc = rb::syscall;

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
    // Labelled, because `steps I=… J=…` reads as a speedup and is not one: the tiers count different
    // units (see `RunResult::instructions_executed`), so this pair says how dense the IR is for this
    // program and nothing at all about how fast either tier runs.
    std::fprintf(stderr, "[diff] %-58s interp=%d jit=%d want=%d ir-ops I=%u guest-insn J=%u\n",
                 what.c_str(), static_cast<std::int32_t>(si.r[0]), static_cast<std::int32_t>(sj.r[0]),
                 c.want, ri.instructions_executed, rj.instructions_executed);
  }
}

/// The VFP/NEON groups, per instruction class, with hardware as the authority and a refusal allowed.
///
/// `docs/32` §1 measured the demand (14,723 vector/FP instructions in bionic and libc++, `vmul.f64`
/// 786 times, `vldr` 2,290) and §5 item 3 ranked this lane next after IT-blocks. What it did not
/// answer is what RetroBug *does* about them, and this lane is that answer in units of real code: what
/// began as one vector IR opcode (`VectorAdd32`) out of the 725-entry table now covers the scalar VFP
/// group (§Step 17) and the integer ASIMD lane group (§Step 19). What is still refused is *vector
/// floating-point* (`FPVectorAdd` and friends), the widening/narrowing and saturating ASIMD forms,
/// shifts by a register, and the table lookups — so the example this comment used to give, a scalar
/// `dadd`, is now answered rather than refused, and the refusal belongs to `vadd.f32`.
///
/// So each row asserts two different things, deliberately not the same one:
///   - Tier J must equal the hardware number. A miss here is a bug, and it fails.
///   - Tier I must *either* equal it *or* stop with `Unimplemented` naming the op. A third outcome —
///     a completed run with a different value — is the one this cannot allow, because two tiers that
///     agree with each other are not evidence: they can both be wrong together, which is exactly what
///     §Step 7c found for IT blocks. Hence the hardware column rather than a tier-vs-tier diff.
/// The stderr summary is the deliverable: which group, which op, how far Tier I gets today.
///
/// One limit worth stating rather than leaving to the reader: a row passing on Tier I proves the
/// handlers *this probe happens to reach* are right, and the probe reaching them is an assumption about
/// compiler output. The assumption is checked two ways — the refusal count below is the instrument that
/// says "nothing was needed that is missing", and `llvm-objdump` on the built fixture is what says the
/// mnemonics are really there. See the `RB_HOLD` comment in `tests/corpus/fixture_jni.c` for the second
/// one, which is how a 64-bit `vdup` turned out to be unencodable rather than untested.
///
/// The negative control, run 2026-09-20 on the SM-X710 with two mutations applied at once to
/// `interp.cpp` — the 8-bit lane loop stepping in 16s, and `VectorAndNot` computed as `~x & y`:
/// **all ten ASIMD rows failed, 20 checks**, then all passed again on revert. A row that cannot
/// distinguish those two bugs would not be worth adding, and "0 of 50 refused" alone does not show
/// that it can.
struct FpCase {
  const char* name;
  std::int32_t a, b;
  std::int32_t want;  ///< one value from tests/corpus/oracle-armv7.txt, in that row's order
  const char* group;  ///< what the probe is supposed to be exercising, for the summary
};

void fp_groups_are_accurate_or_explicitly_refused() {
  Rig rig = build();
  if (!rig.ok) return;

  // Every probe below takes at most two declared parameters, because that is all a JNI method *can*
  // take in registers: the ARM AAPCS passes arguments in r0-r3 and the rest on the stack, and
  // `JNIEnv*`/`jclass` already occupy r0/r1. A third parameter would be an unmapped read at entry —
  // which is not an engine defect but would have been reported as one. Spilled arguments are covered by
  // the `intThenDouble` family in `rb_guest_apk_tests`, where RetroBug lays the frame out; `fp_args`
  // below does not, because its eight doubles all fit in d0-d7.
  const FpCase cases[] = {
      // fp_arith: vmov/vcvt/dmul/dadd/ddiv/vsqrt/fmdrr
      {"Java_com_retrobug_fixture_MainActivity_fp_arith", 3, 7, 13, "scalar f64 +-*/,sqrt"},
      {"Java_com_retrobug_fixture_MainActivity_fp_arith", 7, 3, 15, "scalar f64 +-*/,sqrt"},
      {"Java_com_retrobug_fixture_MainActivity_fp_arith", -5, 3, -22, "scalar f64 +-*/,sqrt"},
      {"Java_com_retrobug_fixture_MainActivity_fp_arith", 100, -50, -5113, "scalar f64 +-*/,sqrt"},
      {"Java_com_retrobug_fixture_MainActivity_fp_arith", 0, 1, -1, "scalar f64 +-*/,sqrt"},
      // fpsr_flags: dcmp.f64 then vmrs of FPSCR — the flag *semantics*, not the value. `129` is the
      // `t < u` case, and it is the row that carries ARM's rule that a less-than compare also sets NV.
      // Four of the five pairs land on the same number, which is why the fifth is in the set.
      {"Java_com_retrobug_fixture_MainActivity_fpsr_flags", 3, 7, 34, "dcmp + FPSCR flags"},
      {"Java_com_retrobug_fixture_MainActivity_fpsr_flags", 7, 3, 34, "dcmp + FPSCR flags"},
      {"Java_com_retrobug_fixture_MainActivity_fpsr_flags", -5, 3, 34, "dcmp + FPSCR flags"},
      {"Java_com_retrobug_fixture_MainActivity_fpsr_flags", 100, -50, 129, "dcmp + FPSCR flags"},
      {"Java_com_retrobug_fixture_MainActivity_fpsr_flags", 0, 1, 34, "dcmp + FPSCR flags"},
      // fp_cmp: the compare field per relation, measured with FPSCR cleared first, so the low byte is
      // this compare's own exception state rather than the process's history. Both tiers must produce
      // the same five values. The row exists because Tier I's first version used a compare table recalled
      // from the manual rather than looked up, disagreed with hardware on three of four relations, and
      // was wrong — the measurement here is also Arm's published VFP table. See docs/30 §Step 17.
      {"Java_com_retrobug_fixture_MainActivity_fp_cmp", 0, 0, 0x800, "compare field: less"},
      {"Java_com_retrobug_fixture_MainActivity_fp_cmp", 1, 0, 0x600, "compare field: equal"},
      {"Java_com_retrobug_fixture_MainActivity_fp_cmp", 2, 0, 0x200, "compare field: greater"},
      // The last two differ only in whether the invalid exception is raised, which is the entire
      // observable difference between `vcmpe.f64` and `vcmp.f64` — and so the only evidence in this
      // suite that the IR's `exc_on_qnan` operand means anything.
      {"Java_com_retrobug_fixture_MainActivity_fp_cmp", 3, 0, 0x301,
       "compare field: unordered, VCMPE raises IOC"},
      {"Java_com_retrobug_fixture_MainActivity_fp_cmp", 4, 0, 0x300,
       "compare field: unordered, VCMP does not"},
      // fp_stack: vldr/vstr through a stack array, and the callee-saved d8-d15 bank across a call.
      {"Java_com_retrobug_fixture_MainActivity_fp_stack", 3, 7, 110125, "FP stack + save/restore"},
      {"Java_com_retrobug_fixture_MainActivity_fp_stack", 7, 3, 144035, "FP stack + save/restore"},
      {"Java_com_retrobug_fixture_MainActivity_fp_stack", -5, 3, -55957, "FP stack + save/restore"},
      {"Java_com_retrobug_fixture_MainActivity_fp_stack", 100, -50, 1023099,
       "FP stack + save/restore"},
      {"Java_com_retrobug_fixture_MainActivity_fp_stack", 0, 1, 11269, "FP stack + save/restore"},
      // fp_args: eight doubles into one *guest-internal* call. All eight go in d0-d7, not the four-plus-
      // stack split the label used to claim — `sum8` is `static`, and clang gives such a function the VFP
      // register ABI even under `-mfloat-abi=softfp`. Measured, not reasoned about; see the comment on
      // `sum8` in tests/corpus/fixture_jni.c and docs/30 §Step 25.
      {"Java_com_retrobug_fixture_MainActivity_fp_args", 3, 7, 56, "8-double call through d0-d7"},
      {"Java_com_retrobug_fixture_MainActivity_fp_args", 7, 3, 62, "8-double call through d0-d7"},
      {"Java_com_retrobug_fixture_MainActivity_fp_args", -5, 3, -13, "8-double call through d0-d7"},
      {"Java_com_retrobug_fixture_MainActivity_fp_args", 100, -50, 351, "8-double call through d0-d7"},
      {"Java_com_retrobug_fixture_MainActivity_fp_args", 0, 1, 8, "8-double call through d0-d7"},
      // fp_round: fcvt both directions and a truncating convert to int. `a` is the oracle's argument
      // already shifted (`pairs[i][0] - 2` there), which is why it is not the pair above.
      {"Java_com_retrobug_fixture_MainActivity_fp_round", 1, 0, 6, "fcvt + rounding"},
      {"Java_com_retrobug_fixture_MainActivity_fp_round", 5, 0, 32, "fcvt + rounding"},
      {"Java_com_retrobug_fixture_MainActivity_fp_round", -7, 0, -45, "fcvt + rounding"},
      {"Java_com_retrobug_fixture_MainActivity_fp_round", 98, 0, 637, "fcvt + rounding"},
      {"Java_com_retrobug_fixture_MainActivity_fp_round", -2, 0, -13, "fcvt + rounding"},
      // fp_nan: 0.0/0.0 then four compares. `8` on all five is the point — exactly one of the four
      // branches may be taken, and a tier that invents its own NaN payload still gets this right,
      // which is deliberate: the payload is not what app code branches on.
      {"Java_com_retrobug_fixture_MainActivity_fp_nan", 1, 0, 8, "NaN comparison semantics"},
      {"Java_com_retrobug_fixture_MainActivity_fp_nan", 5, 0, 8, "NaN comparison semantics"},
      {"Java_com_retrobug_fixture_MainActivity_fp_nan", -7, 0, 8, "NaN comparison semantics"},
      {"Java_com_retrobug_fixture_MainActivity_fp_nan", 98, 0, 8, "NaN comparison semantics"},
      {"Java_com_retrobug_fixture_MainActivity_fp_nan", -2, 0, 8, "NaN comparison semantics"},
      // simd_int: vmul.i32/vadd.i32 on q registers plus two lane extracts.
      {"Java_com_retrobug_fixture_MainActivity_simd_int", 3, 7, -25, "ASIMD i32 lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_int", 7, 3, 67, "ASIMD i32 lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_int", -5, 3, 67, "ASIMD i32 lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_int", 100, -50, 30250, "ASIMD i32 lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_int", 0, 1, -1, "ASIMD i32 lanes"},
      // simd_lane: one lane width per row, and a fold over all 16 result bytes rather than a returned
      // lane, so a lane loop of the wrong width shows up as a changed number. `a` is the mode (0=8,
      // 1=16, 2=32, 3=64 bit) and `b` seeds the register; row 4 repeats the 64-bit path with different
      // data because ARMv7 has exactly four integer lane widths, not five.
      {"Java_com_retrobug_fixture_MainActivity_simd_lane", 0, 3, 1709337648, "ASIMD 8-bit lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_lane", 1, 7, -1185945012, "ASIMD 16-bit lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_lane", 2, -5, -1238834555, "ASIMD 32-bit lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_lane", 3, 100, -450686130, "ASIMD 64-bit lanes"},
      {"Java_com_retrobug_fixture_MainActivity_simd_lane", 4, -2, -1656795248, "ASIMD 64-bit lanes"},
      // simd_bits: the width-independent logical group, one operation per row, including `vbic` twice
      // with its operands swapped — an implementation of `A & ~B` as `~A & B` is a plausible mistake
      // and passes every symmetric test, so the two rows differ only in operand order. `vorn` reaches
      // `VectorNot` and `vneg` reaches `ZeroVector`; see the fixture for both lowerings.
      {"Java_com_retrobug_fixture_MainActivity_simd_bits", 0, 3, 1148729636, "ASIMD logical + VORN/VNEG"},
      {"Java_com_retrobug_fixture_MainActivity_simd_bits", 1, 7, 1143559836, "ASIMD logical + VORN/VNEG"},
      {"Java_com_retrobug_fixture_MainActivity_simd_bits", 2, -5, -1080895096, "ASIMD logical + VORN/VNEG"},
      {"Java_com_retrobug_fixture_MainActivity_simd_bits", 3, 100, -1479472860, "ASIMD logical + VORN/VNEG"},
      {"Java_com_retrobug_fixture_MainActivity_simd_bits", 4, -2, 442803864, "ASIMD logical + VORN/VNEG"},
  };

  int refused = 0;
  std::string ops;
  for (const auto& c : cases) {
    const auto where = rig.mod->FindExport(c.name);
    if (!where) {
      ++checks;
      fail("no export", __LINE__, c.name);
      continue;
    }
    exec::MachineState si = EntryState(where->addr, c.a, c.b, where->thumb, /*two=*/true);
    exec::MachineState sj = si;  // Run() mutates the state it is handed, so neither copy can be const

    const exec::RunResult rj = rig.jit->Run(sj, 8'000'000);
    CHECK_CTX(rj.reason == exec::StopReason::Returned &&
                  static_cast<std::int32_t>(sj.r[0]) == c.want,
              std::string{c.name} + " on Tier J gave " +
                  std::to_string(static_cast<std::int32_t>(sj.r[0])) + ", hardware said " +
                  std::to_string(c.want) + "\n      " + Describe("jit", rj, sj));

    const exec::RunResult ri = rig.interp->Run(si, 8'000'000);
    const bool i_ran = ri.reason == exec::StopReason::Returned;
    const int i_got = static_cast<std::int32_t>(si.r[0]);
    CHECK_CTX(i_ran ? i_got == c.want : ri.reason == exec::StopReason::Unimplemented,
              std::string{c.name} + " on Tier I: reason=" +
                  std::to_string(static_cast<int>(ri.reason)) + " r0=" + std::to_string(i_got) +
                  " hardware=" + std::to_string(c.want) + " detail=" + ri.detail);
    if (!i_ran) {
      ++refused;
      const std::string op = ri.detail.substr(0, ri.detail.find(':'));
      if (ops.find(op) == std::string::npos) ops += (ops.empty() ? "" : ", ") + op;
    }
    if (i_ran) Compare(c.name, si, ri, sj, rj);
    std::fprintf(stderr, "[fp] %-28s %-26s hardware=%-8d jit=%-8d interp=%s%s\n", c.name + 39,
                 c.group, c.want, static_cast<std::int32_t>(sj.r[0]),
                 i_ran ? std::to_string(i_got).c_str() : "refused",
                 i_ran ? "" : (" (" + ri.detail + ")").c_str());
  }
  std::fprintf(stderr, "[fp] Tier I refused %d of %zu FP rows; missing ops: %s\n", refused,
               sizeof cases / sizeof cases[0], ops.empty() ? "none" : ops.c_str());
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

/// An SVC has to reach the syscall monitor with the same frame from both tiers: the same reason, the
/// same immediate, `r7` untouched, and a PC already past the instruction so the monitor's resume point is
/// the same address for both. This is C07's precondition, and it belongs in the differential because
/// every part of it is a comparison rather than an absolute: RetroBug decides what a syscall *means*, so
/// no external oracle can say whether the frame is right — but the two tiers have to agree, and a guest
/// that resumed in the wrong place would not come back with 42.
void both_tiers_stop_at_the_same_svc() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto p = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_svc_probe");
  if (!p || !p->thumb) {
    ++checks;
    fail("the fixture has no svc_probe", __LINE__);
    return;
  }
  constexpr std::uint32_t kSvcImm = 0x9c;
  constexpr std::uint32_t kNrExitGroup = 0xf8;  ///< __NR_exit_group, ARM EABI, from the NDK's own header

  exec::MachineState si = EntryState(p->addr, 41, 0, p->thumb);
  exec::MachineState sj = si;
  const exec::RunResult ri = rig.interp->Run(si, 100000);
  const exec::RunResult rj = rig.jit->Run(sj, 100000);

  CHECK_CTX(ri.reason == exec::StopReason::Syscall,
            "Tier I stopped with reason " + std::to_string(static_cast<int>(ri.reason)) + " " + ri.detail);
  CHECK_CTX(rj.reason == exec::StopReason::Syscall,
            "Tier J stopped with reason " + std::to_string(static_cast<int>(rj.reason)) + " " + rj.detail);
  CHECK_CTX(ri.svc_number == kSvcImm, "Tier I reports the SVC immediate as " + Hex(ri.svc_number));
  CHECK_CTX(rj.svc_number == kSvcImm, "Tier J reports the SVC immediate as " + Hex(rj.svc_number));
  CHECK_CTX(si.r[7] == kNrExitGroup, "Tier I left r7 at " + Hex(si.r[7]));
  CHECK_CTX(sj.r[7] == kNrExitGroup, "Tier J left r7 at " + Hex(sj.r[7]));
  // The two channels have to be distinguishable, which is the reason the fixture's immediate and `r7` are
  // different numbers: with `svc 0` either reading gives 0, and a monitor that dispatched on the encoding
  // instead of the register would pass every such test until an app used a non-zero immediate.
  CHECK_CTX(ri.svc_number != si.r[7], "the immediate and r7 are the same value, so this proves nothing");
  CHECK_CTX(ri.detail == rj.detail,
            "Tier I said \"" + ri.detail + "\" and Tier J said \"" + rj.detail + "\" of one SVC");
  CHECK_CTX(si.pc > p->addr && sj.pc > p->addr && (si.pc & 1u) == 0,
            "PC at the stop is inside the function and Thumb-tagged: I pc=" + Hex(si.pc) +
                " J pc=" + Hex(sj.pc));
  Compare("svc #0x9c stop", si, ri, sj, rj);
  std::fprintf(stderr, "[diff] svc stop: I \"%s\" pc=%s r7=%s | J \"%s\" pc=%s r7=%s\n",
               ri.detail.c_str(), Hex(si.pc).c_str(), Hex(si.r[7]).c_str(), rj.detail.c_str(),
               Hex(sj.pc).c_str(), Hex(sj.r[7]).c_str());

  // Resume. Nothing has to *emulate* exit_group for this to be meaningful: the monitor's job is to leave
  // r7 and r0 alone and put the guest at pc, so pretending it returned 0 is the whole handler. What the
  // test then measures is whether the resume address was right, by arithmetic the guest itself does.
  const exec::RunResult ri2 = rig.interp->Run(si, 100000);
  const exec::RunResult rj2 = rig.jit->Run(sj, 100000);
  CHECK_CTX(ri2.reason == exec::StopReason::Returned, "Tier I after the syscall: " + ri2.detail);
  CHECK_CTX(rj2.reason == exec::StopReason::Returned, "Tier J after the syscall: " + rj2.detail);
  CHECK_CTX(static_cast<std::int32_t>(si.r[0]) == 42,
            "Tier I resumed at the wrong instruction: r0 = " +
                std::to_string(static_cast<std::int32_t>(si.r[0])) + " want 42");
  CHECK_CTX(static_cast<std::int32_t>(sj.r[0]) == 42,
            "Tier J resumed at the wrong instruction: r0 = " +
                std::to_string(static_cast<std::int32_t>(sj.r[0])) + " want 42");
  Compare("svc #0x9c resumed", si, ri2, sj, rj2);
  std::fprintf(stderr, "[diff] svc resumed: I r0=%d J r0=%d (want 41+1=42)\n",
               static_cast<std::int32_t>(si.r[0]), static_cast<std::int32_t>(sj.r[0]));
}

/// AD-7 told end to end, from the guest's side: it patches nothing (its code page is not writable, which
/// is the design) but it *says* the bytes changed, with `__ARM_NR_cacheflush`, and the tier has to stop
/// believing what it compiled for that address.
///
/// The host does the write here only because a guest cannot: the test's patch step is the same
/// `Protect`→write→`Protect` a guest's `mprotect` would take, and the point of the case is what happens
/// *after* it. Two things make this more than "call Invalidate and check": the flush range that misses
/// the patched byte must leave Tier J stale, which is what proves the range reaches the tier rather than
/// any cacheflush clearing the world; and the value Tier J then returns is the one hardware measured for
/// the other probe, so a cache that was only half-invalidated could not produce it.
void a_guest_cacheflush_reaches_the_tier_that_owns_the_cache() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto patch_a = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_patch_probe");
  const auto patch_b = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_patch_probe_b");
  const auto flush = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_flush_probe");
  if (!patch_a || !patch_b || !flush) {
    ++checks;
    fail("the fixture has no patch probes or no flush probe", __LINE__);
    return;
  }

  // The same window as the other case: the differing byte is at +8, so a window of eight would find no
  // difference at all and the test would report "not one byte apart" — which is the right diagnosis for
  // the wrong reason, and exactly what it was the first time this was written.
  std::uint8_t ba[kPatchWindow] = {};
  std::uint8_t bb[kPatchWindow] = {};
  CHECK_CTX(rig.mem->Read(patch_a->addr, ba, sizeof ba), "cannot read patch_probe");
  CHECK_CTX(rig.mem->Read(patch_b->addr, bb, sizeof bb), "cannot read patch_probe_b");
  std::size_t at = kPatchWindow;
  int diffs = 0;
  for (std::size_t i = 0; i < sizeof ba; ++i) {
    if (ba[i] != bb[i]) {
      at = i;
      ++diffs;
    }
  }
  if (diffs != 1) {
    ++checks;
    fail("the probes are not one byte apart any more", __LINE__,
         std::to_string(diffs) + " differences in " + std::to_string(sizeof ba) + " bytes");
    return;
  }

  sc::Monitor mon;
  abi::AbiContext bridge(*rig.mem, kStackTop, kReturnAddress);
  bridge.set_executor(*rig.jit);
  bridge.set_monitor(&mon);
  // The ranges are recorded, not just "an invalidation happened": a flush of the wrong address also
  // reaches the invalidator, and a test that only looked at whether it was called would pass while the
  // guest's range was thrown away.
  std::vector<std::pair<exec::GuestAddr, exec::GuestAddr>> flushed;
  mon.set_invalidator([&](exec::GuestAddr lo, exec::GuestAddr hi) {
    flushed.emplace_back(lo, hi);
    rig.jit->Invalidate(lo, hi);
  });

  // The same guest flush driven through Tier I, into its own monitor: both have to hand the monitor the
  // same number, because everything downstream is interpreted from it. This is what caught a wrong
  // `movt` immediate in the fixture — both tiers reported the *same* wrong number, which is the shape
  // that means the bug is in the test's own bytes rather than in a tier.
  sc::Monitor mon_i;
  abi::AbiContext bridge_i(*rig.mem, kStackTop, kReturnAddress);
  bridge_i.set_executor(*rig.interp);
  bridge_i.set_monitor(&mon_i);
  mon_i.set_invalidator([&](exec::GuestAddr lo, exec::GuestAddr hi) { rig.interp->Invalidate(lo, hi); });
  const auto flush_through = [&](abi::AbiContext& b, exec::GuestAddr at, exec::GuestAddr expect_lo,
                                 exec::GuestAddr expect_hi) {
    exec::RunResult out{};
    const auto ret = b.CallGuest(flush->tagged(),
                                 {abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(at),
                                  abi::Value32::S32(7)},
                                 abi::Shorty::For("IILLII"), 100000, &out);
    CHECK_CTX(out.reason == exec::StopReason::Returned, "flush_probe did not return: " + out.detail);
    // b+1: the flush returned, the guest kept going, and what it saw in r0 afterwards was its own
    // argument rather than the syscall's.
    CHECK_CTX(ret.i() == 8, "flush_probe returned " + std::to_string(ret.i()) + ", want 8");
    if (expect_hi != 0) {
      CHECK_CTX(!flushed.empty() && flushed.back().first == expect_lo &&
                    flushed.back().second == expect_hi,
                "Tier J's last invalidation was not the range the guest asked for");
    }
    return out;
  };
  flush_through(bridge_i, patch_a->addr + 64, 0, 0);
  flush_through(bridge, patch_a->addr + 64, patch_a->addr + 64, patch_a->addr + 80);
  CHECK_CTX(mon_i.TallyText() == mon.TallyText(),
            "one guest cacheflush, two tiers, two numbers: I \"" + mon_i.TallyText() + "\" J \"" +
                mon.TallyText() + "\"");
  CHECK_CTX(mon.TallyText() == "arm_cacheflush x1",
            "the guest's r7 reached the monitor as " + mon.TallyText() + "; want arm_cacheflush, which "
            "is 0xf0002 and nothing else");

  const auto run_patch = [&](exec::Executor* tier, exec::MachineState& s) {
    s = EntryState(patch_a->addr, 3, 7, true, true);
    return tier->Run(s, 100000);
  };
  exec::MachineState s0{};
  run_patch(rig.jit.get(), s0);
  CHECK_CTX(static_cast<std::int32_t>(s0.r[0]) == kPatchA_want, "baseline changed");

  std::string perr;
  const exec::GuestAddr page = patch_a->addr & ~static_cast<exec::GuestAddr>(mem::kGuestPage - 1);
  if (!rig.mem->Protect(page, mem::kGuestPage, kRead | kWrite, &perr) ||
      !rig.mem->Write(patch_a->addr + at, &bb[at], 1) ||
      !rig.mem->Protect(page, mem::kGuestPage, kRead | kExec, &perr)) {
    ++checks;
    fail("could not patch through the permission transition", __LINE__, perr);
    return;
  }
  flushed.clear();

  // A flush of the wrong range must not help. The address is 64 bytes past the patched byte: same page,
  // no overlap, and Tier J's cache keyed on the block that starts where the code actually is.
  flush_through(bridge, patch_a->addr + 64, patch_a->addr + 64, patch_a->addr + 80);
  {
    exec::MachineState s1{};
    run_patch(rig.jit.get(), s1);
    CHECK_CTX(static_cast<std::int32_t>(s1.r[0]) == kPatchA_want,
              "a flush of an unrelated range cleared the whole cache: it answered " +
                  std::to_string(static_cast<std::int32_t>(s1.r[0])));
  }
  // The right range, from the guest, and nothing else: this is the call the test is really about.
  flush_through(bridge, patch_a->addr, patch_a->addr, patch_a->addr + 16);

  exec::MachineState sj{};
  const exec::RunResult rj = run_patch(rig.jit.get(), sj);
  exec::MachineState si{};
  const exec::RunResult ri = run_patch(rig.interp.get(), si);
  CHECK_CTX(static_cast<std::int32_t>(sj.r[0]) == kPatchB_want,
            "after the guest's own cacheflush, Tier J still answers " +
                std::to_string(static_cast<std::int32_t>(sj.r[0])) + "; hardware says the bytes it now "
                "holds answer " + std::to_string(kPatchB_want));
  CHECK_CTX(static_cast<std::int32_t>(si.r[0]) == kPatchB_want, "Tier I disagrees");
  Compare("guest cacheflush then patched (3,7)", si, ri, sj, rj);
  // Three of them by now: the two-tier comparison, the deliberate miss, and the hit. One *number*, and
  // the number is the only entry — a monitor that had also seen the exit probe would mean this test ran
  // someone else's guest code.
  CHECK_CTX(mon.tally().size() == 1u, "expected one number in the tally: " + mon.TallyText());
  CHECK_CTX(mon.TallyText() == "arm_cacheflush x3", mon.TallyText());
  std::fprintf(stderr, "[diff] guest-issued cacheflush: stale=%d then I=%d J=%d want=%d (%s)\n",
               kPatchA_want, static_cast<std::int32_t>(si.r[0]), static_cast<std::int32_t>(sj.r[0]),
               kPatchB_want, mon.TallyText().c_str());

  // Restore, and flush again so the tier is left holding the bytes the fixture actually has.
  rig.mem->Protect(page, mem::kGuestPage, kRead | kWrite, &perr);
  rig.mem->Write(patch_a->addr + at, &ba[at], 1);
  rig.mem->Protect(page, mem::kGuestPage, kRead | kExec, &perr);
  rig.jit->Invalidate(patch_a->addr, patch_a->addr + kPatchWindow);
  exec::MachineState back{};
  run_patch(rig.jit.get(), back);
  CHECK_CTX(static_cast<std::int32_t>(back.r[0]) == kPatchA_want,
            "the fixture was left patched inside its own memory");
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

/**
 * The ARM-state instructions Dynarmic's A32 front end *declines* to translate.
 *
 * Their translators end in `InterpretThisInstruction()`, which produces `IR::Term::Interpret` rather
 * than code. That terminal is not a control-flow edge: its `next` is the location at which
 * interpretation was to start, meaning the instruction that was just refused. A tier that follows it as
 * if it were a `LinkBlock` re-translates the same instruction and returns here forever — which is what
 * this file did until `docs/32` §7 found it, and it is a hang with no step counted, no reason returned
 * and no PC to report.
 *
 * The encodings were assembled from source and read back out of the disassembly, not copied out of a
 * manual (`docs/30`'s standing rule about facts recalled from a specification):
 *
 *   $TC/bin/clang.exe --target=armv7-none-linux-androideabi26 -marm -c bank.S
 *   $TC/bin/llvm-objdump.exe -d bank.o
 *
 * with `bank.S` holding the seven mnemonics below. Two things came out of doing that rather than
 * writing them down: LLVM's own diagnostic calls `stmdb r0!, {...}^` a *"system STM"* and rejects the
 * writeback, and the banked `^` goes **after** the register list in LLVM's syntax, which is not where
 * the ARM manual's pseudocode suggests to look for it.
 *
 * Tier J used to be excluded from this lane, because on an ARM64 host its handling of the terminal is
 * `ASSERT_FALSE("Interpret should never be emitted.")` and mcl's `ASSERT` is not `NDEBUG`-gated, so
 * running it terminated the process and a suite that dies is not a suite that failed. `docs/30` §Step 20
 * measured that (SIGABRT, rc=134) and §Step 21 vetoes the six encodings before the back end can be
 * asked, which is why both tiers are asserted here. `--interpret-probe <tier>` remains for re-deriving
 * the host-level behaviour from scratch.
 */
struct Declined {
  std::uint32_t encoding;
  const char* asm_text;
  const char* body;  ///< the translator dynarmic should name, by its own specificity rule
};
constexpr Declined kDeclined[] = {
    {0xe8d0'0030u, "ldm r0, {r4, r5}^", "arm_LDM_usr"},
    // Writeback as well as R15 in the list. That combination is what makes this row reach
    // `arm_LDM_eret` and nothing else: `arm_LDM_usr` asks for bit 21 (W) to be clear, so the two
    // patterns are disjoint here, and only `arm_LDM_eret` matches. The first draft used
    // `ldmdb r0, {r4, r5, pc}^` (0xe9508030) and expected `arm_LDM_eret` — the assertion below failed
    // it, because that encoding matches both patterns and dynarmic's `count_ones` ranking ties at six
    // mask bits, so its `arm.inc` declaration order leaves `arm_LDM_usr` first. Which body a
    // declined instruction "was" is not observable from the embedder side — every one of them ends in
    // the same `Term::Interpret` — so an encoding has to be chosen where only one answer exists.
    {0xe8f0'8030u, "ldm r0!, {r4, r5, pc}^", "arm_LDM_eret"},
    {0xe940'0006u, "stmdb r0, {r1, r2}^", "arm_STM_usr"},
    {0xf102'001fu, "cps #0x1f", "arm_CPS"},
    {0xf108'0040u, "cpsie f", "arm_CPS"},
    {0xf8b0'0a00u, "rfeia r0!", "arm_RFE"},
    {0xf96d'0504u, "srsdb sp!, #4", "arm_SRS"},
};

/// Mapped just past the declined encodings. A veto wide enough to catch these would take the whole
/// suite down, so the cheapest control is an ordinary ARM instruction that has to still run.
constexpr std::uint32_t kLegalArmPair[] = {0xe3a0'002au,  // mov r0, #42
                                           0xe12f'ff1eu};  // bx lr

/// Map one page of guest memory holding `kDeclined` and `kLegalArmPair`, or 0 on failure.
exec::GuestAddr PlantDeclined(Rig& rig) {
  constexpr exec::GuestAddr kPage = 0x0180'0000u;
  std::string err;
  if (!rig.mem->Map(kPage, mem::kGuestPage, kRead | kWrite | kExec, &err)) {
    ++checks;
    fail("could not map a page for the declined encodings", __LINE__, err);
    return 0;
  }
  for (unsigned i = 0; i < sizeof kDeclined / sizeof kDeclined[0]; ++i) {
    const std::uint32_t word = kDeclined[i].encoding;
    if (!rig.mem->Write(kPage + i * 4, &word, 4)) {
      ++checks;
      fail("could not write a declined encoding into guest memory", __LINE__, kDeclined[i].asm_text);
      return 0;
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (!rig.mem->Write(kPage + 0x80 + i * 4, &kLegalArmPair[i], 4)) {
      ++checks;
      fail("could not write the legal control pair", __LINE__, "");
      return 0;
    }
  }
  return kPage;
}

void declined_instructions_refuse_by_name() {
  Rig rig = build();
  if (!rig.ok) return;
  const exec::GuestAddr page = PlantDeclined(rig);
  if (page == 0) return;

  // A small budget, because one of the regressions this guards is a livelock rather than a crash:
  // following `Term::Interpret` as a control edge re-translates the same pc forever, and 100k steps
  // says so in milliseconds instead of after four million.
  constexpr unsigned kBudget = 100'000;
  std::set<std::string> named_bodies;

  for (unsigned i = 0; i < sizeof kDeclined / sizeof kDeclined[0]; ++i) {
    const exec::GuestAddr where = page + i * 4;
    const std::string want_pc = Hex(where);

    exec::MachineState si = EntryState(where, 0, 0, /*thumb=*/false);
    const exec::RunResult ri = rig.interp->Run(si, kBudget);
    CHECK_CTX(ri.reason == exec::StopReason::Unimplemented,
              std::string{kDeclined[i].asm_text} + " on Tier I: reason=" +
                  std::to_string(static_cast<int>(ri.reason)) + " steps=" +
                  std::to_string(ri.instructions_executed) + " detail=" + ri.detail);
    exec::MachineState sj = EntryState(where, 0, 0, /*thumb=*/false);
    const exec::RunResult rj = rig.jit->Run(sj, kBudget);
    CHECK_CTX(rj.reason == exec::StopReason::Unimplemented,
              std::string{kDeclined[i].asm_text} + " on Tier J: reason=" +
                  std::to_string(static_cast<int>(rj.reason)) + " steps=" +
                  std::to_string(rj.instructions_executed) + " detail=" + rj.detail);
    // Both tiers name the instruction and its address, because a refusal that cannot say which
    // instruction is the one a reader cannot act on.
    const std::pair<const char*, const exec::RunResult&> both[] = {{"I", ri}, {"J", rj}};
    for (const auto& [which, r] : both) {
      CHECK_CTX(r.detail.find("declined to translate") != std::string::npos &&
                    r.detail.find(want_pc) != std::string::npos,
                std::string{kDeclined[i].asm_text} + " refused on Tier " + which +
                    " without naming the instruction and its pc 0x" + want_pc + ": " + r.detail);
    }
    // **The load-bearing one.** A veto that instead let the instruction be *skipped* would report a
    // clean stop with guest state advanced past it — a silent wrong answer dressed as a refusal, and
    // strictly worse than the abort this replaced. Zero steps says nothing executed.
    CHECK_CTX(ri.instructions_executed == 0 && rj.instructions_executed == 0,
              std::string{kDeclined[i].asm_text} + " ran " +
                  std::to_string(ri.instructions_executed) + " Tier I and " +
                  std::to_string(rj.instructions_executed) +
                  " Tier J steps before refusing; a refused instruction must not also be executed");
    // Tier J names the translator body it vetoed; checking it against dynarmic's own specificity rule
    // is what turns "these seven encodings cover all six bodies" from prose into a checked fact.
    CHECK_CTX(rj.detail.find(kDeclined[i].body) != std::string::npos,
              std::string{kDeclined[i].asm_text} + " was vetoed as something other than " +
                  kDeclined[i].body + ": " + rj.detail);
    named_bodies.insert(kDeclined[i].body);
    std::fprintf(stderr, "[declined] %-26s I: %s\n%*s J: %s\n", kDeclined[i].asm_text,
                 ri.detail.c_str(), 27, "", rj.detail.c_str());
  }

  // The enumeration in `docs/32` §7 is six bodies. If the lane only ever reaches five of them, the
  // sixth is untested and the sentence "all six are refused" would be about the table, not the tests.
  CHECK_CTX(named_bodies.size() == 6,
            "the seven encodings named " + std::to_string(named_bodies.size()) +
                " distinct translator bodies; docs/32 §7 enumerates six");

  // Control: an ordinary ARM `mov`+`bx lr` still executes on both tiers, so what the loop above proves
  // is a refusal at six addresses rather than a veto over the whole ARM-state translator.
  const std::pair<const char*, exec::Executor*> engines[] = {
      {"I", rig.interp.get()}, {"J", rig.jit.get()}};
  for (const auto& [which, engine] : engines) {
    exec::MachineState s = EntryState(page + 0x80, 0, 0, /*thumb=*/false);
    const exec::RunResult r = engine->Run(s, kBudget);
    CHECK_CTX(r.reason == exec::StopReason::Returned && s.r[0] == 42,
              std::string{"the legal ARM control pair on Tier "} + which + ": reason=" +
                  std::to_string(static_cast<int>(r.reason)) + " r0=" + std::to_string(s.r[0]) +
                  " detail=" + r.detail);
  }
}

/// `--interpret-probe jit|interp`: one tier, one declined encoding, nothing else. Exists so the
/// arm64-host question (`does the back end abort, and where?`) can be asked once without the answer
/// taking the rest of the lane down with it.
int interpret_probe(const char* which) {
  const bool want_jit = std::string_view{which} == "jit";
  if (!want_jit && std::string_view{which} != "interp") {
    std::fprintf(stderr, "usage: %s --interpret-probe jit|interp\n", which);
    return 2;
  }
  Rig rig = build();
  if (!rig.ok) return 1;
  const exec::GuestAddr page = PlantDeclined(rig);
  if (page == 0) return 1;
  std::fprintf(stderr, "[probe] entering Tier %s on %s at 0x%s\n", want_jit ? "J" : "I",
               kDeclined[0].asm_text, Hex(page).c_str());
  std::fflush(stderr);
  exec::MachineState s = EntryState(page, 0, 0, false);
  const exec::RunResult r = want_jit ? rig.jit->Run(s, 100'000) : rig.interp->Run(s, 100'000);
  std::fprintf(stderr, "[probe] survived: reason=%d steps=%u detail=%s\n",
               static_cast<int>(r.reason), r.instructions_executed, r.detail.c_str());
  return 0;
}

/**
 * The ARMv7 / Thumb-2 exclusive pair — `ldaex*`/`ldrex*` with `strex*` — as both tiers see it.
 *
 * What this lane asserts is not "STREX returns 0 when it stores". It is the asymmetry in ARM's contract:
 * **an STREX may fail spuriously, and software has to retry**. The two directions of error are not
 * equivalent. Failing a store hardware would have passed costs the guest one more turn of a loop it can
 * leave; passing a store hardware could have failed loses an update, and nothing downstream can undo it.
 * So an emulator may be *stricter* than any device and may never be looser, and RetroBug's rule is the
 * strict one: an exclusive store is checked against the value the exclusive load saw, and it reserves the
 * exact address rather than a granule.
 *
 * `tests/guest/exclprobe.s` is where that was settled — hand-written single-core sequences run on two
 * AArch32 devices, an Oppo Reno 5 (Cortex-A76/A55) and a Samsung Tab S9 (Cortex-A710/A510). Eight of the
 * thirteen rows below are the answers both devices gave: every plain pair passes (`Rd = 0`) at halfword,
 * byte, word and double width, both acquire forms pass, and `clrex` between a pair or a store with no
 * preceding exclusive load fails (`Rd = 1`). Three do not simply copy a device, and each row says which:
 *   - f2, `ldrexh` then a plain `strh` to the same halfword *by this same core* then `strexh`: the Reno 5
 *     answered `Rd = 0` (store passed, the plain store's value overwritten) and the Tab S9 `Rd = 1` (store
 *     failed, plain store's value survived). f7, the `ldaexh` form of the same sequence, split the same
 *     way; it is not planted here. Neither device is wrong — address comparison and value comparison are
 *     both inside the contract — and RetroBug answers like the Tab S9 because that is the only side a
 *     guest cannot detect being wrong about.
 *   - f5, `ldrexh [r0]` then `strexh [r0,#2]`: `Rd = 0` on **both**, so hardware reserves a granule wider
 *     than the halfword. RetroBug answers 1, deliberately, in the safe direction.
 *   - the retry loop, whose first iteration carries that same racing plain store. The Reno 5 would exit it
 *     after one pass; here it exits after two, and asserting *that* (`r3`, the value the successful load
 *     saw) is what makes the loop a test rather than a smoke check.
 * The last two rows — `clrex` with nothing else between the pair, and a rejected store aimed at an
 * unmapped page — are RetroBug's rule at points no device was asked about, and they are labelled as such.
 *
 * The rest is the reason the lane is in *this* file: `Compare()` on every row. Tier J answers through
 * Dynarmic's `ExclusiveMonitor` plus RetroBug's four `MemoryWriteExclusive*` callbacks, Tier I through
 * `rb::exec::ExclusiveState`, and the two are equivalent only by construction — Dynarmic's mask is a
 * constant in one library and the address comparison is a method in another. A granule on one side, a
 * marker that survives a failed store on the other, a reservation that dies with its basic block: each of
 * those is invisible to every other test in this repository, and fatal under a mutex.
 *
 * The rows are planted as raw halfwords in a mapped page, the way `declined_instructions_refuse_by_name`
 * plants its encodings, and not as fixture functions: the fixture `.so` has no exclusive probe, and giving
 * it one would mean editing `tests/corpus/fixture_jni.c`, which this change is not allowed to touch. Two
 * consequences are checked below rather than assumed. The page really is executable, and a planted
 * `movs r0, #42; bx lr` really runs and returns on both tiers — so a red row is about the exclusive logic
 * and not about the plant. And nothing has to be invalidated, because the encodings are in memory before
 * either tier first fetches them; the lane above, `invalidation_reaches_the_compiled_code`, is what covers
 * the path where that is not true.
 *
 * Every encoding was assembled and read back out of the disassembly, per `docs/30`'s standing rule (the
 * NDK ships `llvm-objdump` and no `llvm-mc`):
 *
 *   $TC/bin/clang.exe --target=armv7-none-linux-androideabi26 -mthumb -c excl.s
 *   $TC/bin/llvm-objdump.exe -d --triple=thumbv8 excl.o
 *
 * `--triple=thumbv8` because without it the disassembler prints the two `ldaex` rows as `<unknown>` — fine
 * for reading bytes, useless for checking that the bytes mean what the mnemonic says. `docs/30` §Step 29
 * quotes them named for the same reason.
 *
 * Some of them are not what the field order in the manual, or this work's first notes, suggested. In the
 * STREX forms the **status** register is the *low* nibble of the second halfword and the **source** the
 * high one: `0xe8c0 0x1f52` is `strexh r2, r1, [r0]` and not `strexh r1, r2, [r0]`, and
 * `strexd r5, r3, r1, [r0]` is `0xe8c0 0x3175`, where the pattern's constant `0111` sits *between* the two
 * source registers. `ldrex`/`strex` (word) are the `0xe850`/`0xe840` group whose `imm8` Dynarmic scales by
 * four, and LLVM answers `#0` with `0x00`, so `ldrex r3, [r0]` is `0xe850 0x3f00` — not the
 * `0xe8d0 0x3f1f` the instruction name was first guessed to be. And `clrex` has no 16-bit form at all:
 * `0xf3bf 0x8f2f`.
 * That status/source split is also why the rows claim the bytes around the target, masked, and not just
 * `Rd`: an engine that read the two nibbles the other way round stores the wrong register, and that shows
 * in memory rather than in the flag.
 *
 * One trap worth recording, because it produces a failure that points at the wrong file: the loop's
 * backward branch must name a **local** label. Written `bne x12` to a `.global` symbol, LLVM emits
 * `R_ARM_THM_JUMP19` and leaves a placeholder in the object — `0xf47f 0xaffe`, which decodes as a branch
 * to itself. Planted verbatim that is a livelock the test would report as an engine bug.
 * `llvm-readelf -r` says "no relocations" for the blobs below.
 */
enum class ExclProof : int {
  kMeasured,     ///< both devices answered this way
  kConservative, ///< the devices split; RetroBug follows the side that fails the store
  kStricter,     ///< both devices were looser than RetroBug, on purpose
  kRule,         ///< not measured: RetroBug's own contract, asserted absolutely and tier-vs-tier
};
/// The per-class tally in the runner is a plain array indexed by these, because a fifth kind of
/// provenance needs saying so in the summary line as well.
static_assert(static_cast<int>(ExclProof::kRule) == 3, "the provenance tally below is sized to four");

/// "This row says nothing about that register." A row with nothing to say is caught by the guard in the
/// runner, not silently passed.
constexpr std::uint32_t kNoCheck = 0xFFFF'FFFFu;

constexpr std::uint32_t kExclStore = 0xBEEF;  ///< r1, the value every row's store tries to land

constexpr exec::GuestAddr kExclCodePage = 0x0194'0000u;
constexpr exec::GuestAddr kExclDataPage = 0x0198'0000u;
/// Mapped by nobody, on purpose, and checked with `IsMapped` before the row that uses it runs. All three
/// addresses are 16 KiB-aligned even though the guest granule is 4 KiB, because the x86_64 emulator lane
/// is the 16 KiB one (`docs/30`'s environment table) and a host granule shared with another mapping would
/// make this lane's pages go out-of-line for reasons the lane then would not see.
constexpr exec::GuestAddr kExclUnmapped = 0x019C'0000u;
constexpr exec::GuestAddr kExclSlot = 16;  ///< each row gets its own 16 bytes of data

/// Small on purpose: the loop row reaches its `bx lr` in a couple of dozen guest instructions, and a
/// reservation that stops living at a block boundary makes it spin until this runs out — which is the
/// regression being caught, not an accident to be waited out.
constexpr unsigned kExclBudget = 1000;

/// Each blob is a sequence of Thumb halfwords as `llvm-objdump` printed them, ended by a zero halfword
/// (not a legal Thumb encoding), and each mirrors the like-named function in `tests/guest/exclprobe.s`.
/// Two blobs carry one `nop` the measured source does not, where a 32-bit encoding would otherwise start
/// at an address ending in 2 — after `adds r0, r0, #2` in one and `movs r5, #0` in the other. LLVM leaves
/// the padding out until a `.p2align` asks for it; alignment of the instruction stream is not what this
/// lane measures, so the pad keeps it out of the way rather than making a red row ambiguous.
constexpr std::uint16_t kF1PairHalfword[] = {0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]
                                             0xe8c0, 0x1f52,  ///< strexh r2, r1, [r0]
                                             0x4610, 0x4770,  ///< mov r0, r2 ; bx lr
                                             0};
constexpr std::uint16_t kF2PlainStoreBetween[] = {
    0x235a, 0x8003,  ///< movs r3, #0x5a ; strh r3, [r0]      — give the load something known to see
    0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]
    0x2277, 0x8002,  ///< movs r2, #0x77 ; strh r2, [r0]      — the racing store, same core
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kF3ClrexAfterStore[] = {
    0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]
    0x2299, 0x8002,  ///< movs r2, #0x99 ; strh r2, [r0]
    0xf3bf, 0x8f2f,  ///< clrex
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kClrexWithNothingElse[] = {
    0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]
    0xf3bf, 0x8f2f,  ///< clrex
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kF4NoPriorLoad[] = {
    0xf3bf, 0x8f2f,  ///< clrex                          — no exclusive load has happened in this Run
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kF5StoreTwoBytesOn[] = {
    0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]
    0x1c80,          ///< adds r0, r0, #2
    0xbf00,          ///< nop — the alignment pad described above the blobs
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kF6AcquireHalfword[] = {0xe8d0, 0x3fdf,  ///< ldaexh r3, [r0]
                                                0xe8c0, 0x1f52,  ///< strexh r2, r1, [r0]
                                                0x4610, 0x4770,  ///< mov r0, r2 ; bx lr
                                                0};
constexpr std::uint16_t kF8BytePair[] = {0xe8d0, 0x3f4f,  ///< ldrexb r3, [r0]
                                         0xe8c0, 0x1f44,  ///< strexb r4, r1, [r0]
                                         0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
                                         0};
/// Not f9's `mov r6, r1; mov r7, r1`: storing the same word in both halves cannot tell a swapped pair from
/// a correct one. Here the low half of what gets stored is the register `ldrexd` produced and the high half
/// is r1, and the two seeded halves differ — so the read is pinned by `r3`/`r4` and the write by the eight
/// masked bytes, and neither can be satisfied by the other's mistake.
constexpr std::uint16_t kF9DoublePair[] = {0xe8d0, 0x347f,  ///< ldrexd r3, r4, [r0]
                                           0xe8c0, 0x3175,  ///< strexd r5, r3, r1, [r0]
                                           0x4628, 0x4770,  ///< mov r0, r5 ; bx lr
                                           0};
constexpr std::uint16_t kF10WordPair[] = {0xe850, 0x3f00,  ///< ldrex r3, [r0]
                                          0xe840, 0x1200,  ///< strex r2, r1, [r0]
                                          0x4610, 0x4770,  ///< mov r0, r2 ; bx lr
                                          0};
/// f11: `strex` the value `ldaex` returned, which is bionic's `pthread_mutex_lock` shape one word wider.
constexpr std::uint16_t kF11AcquireWord[] = {0xe8d0, 0x3fef,  ///< ldaex r3, [r0]
                                             0xe840, 0x3200,  ///< strex r2, r3, [r0]
                                             0x4610, 0x4770,  ///< mov r0, r2 ; bx lr
                                             0};
/// The LL/SC loop, in the shape a mutex lock has it: `cbnz` splits the exclusive load from the exclusive
/// store, so they land in **different basic blocks** — which is the whole reason this row exists. Tier I's
/// `BlockRunner` is constructed per block, and Dynarmic's marker lives in the JIT state for the whole run;
/// a reservation record on either side that died with its block makes every store fail, and the guest
/// spins until the budget runs out. The plain store races in on the first iteration only (`r5` is the
/// "already wrote" flag the loop sets itself), so the second iteration's store has to pass.
///
/// The retry is `cmp`+`bne` rather than a second `cbnz` because CBZ/CBNZ cannot branch backwards — found
/// by asking the assembler, not by recalling the encoding table: `cbnz r4, .Lback` on a preceding label
/// is rejected with `out of range pc-relative fixup value`, since the field is zero-extended.
constexpr std::uint16_t kRetryLoop[] = {
    0x2500,          ///< movs r5, #0
    0xbf00,          ///< nop — alignment pad, see the note above
    0xe8d0, 0x3f5f,  ///< ldrexh r3, [r0]   <- the loop head, and the target of the `bne` below
    0xb915,          ///< cbnz r5, .Lstore  -> the store, on every iteration but the first
    0x2277, 0x8002,  ///< movs r2, #0x77 ; strh r2, [r0]   <- the store that races with the pair
    0x2501,          ///< movs r5, #1
    0xe8c0, 0x1f54,  ///< strexh r4, r1, [r0]
    0x2c00,          ///< cmp r4, #0
    0xd1f5,          ///< bne .Lretry       -> back to the ldrexh; the first pass lands here with Rd=1
    0x4620, 0x4770,  ///< mov r0, r4 ; bx lr
    0};
constexpr std::uint16_t kControl[] = {0x202a, 0x4770,  ///< movs r0, #42 ; bx lr
                                      0};

struct ExclRow {
  const char* name = "";      ///< the question, in the log line
  const char* asm_text = "";  ///< the planted sequence, as disassembled
  ExclProof proof = ExclProof::kRule;
  const char* source = "";    ///< where this row's expectations come from, per row
  const std::uint16_t* code = nullptr;
  std::uint32_t seed_lo = 0;  ///< the row's first guest word
  std::uint32_t seed_hi = 0;  ///< and its second; the word and 64-bit rows are the ones that use it
  bool unmapped_target = false;
  std::uint32_t want_r0 = kNoCheck;  ///< Rd, returned by the planted `mov r0, <status>`
  std::uint32_t want_r3 = kNoCheck;
  std::uint32_t want_r4 = kNoCheck;
  std::uint32_t want_r5 = kNoCheck;
  std::uint64_t want_mem = 0;  ///< the eight bytes at the row's data address, after the run...
  std::uint64_t mem_mask = 0;  ///< ...of which only the masked ones are claimed, and never zero by
                               ///< accident: eight bytes are read for every row, so an exclusive store of
                               ///< the wrong width fails on the bits outside the width it was asked for.
};

/// Designated initializers here, where the neighbouring tables do without them: fourteen positional fields
/// per row and a slip between `seed_hi` and `want_r0` would change what a row *claims* without changing
/// anything a reader can see. Each row's comments are the answer hardware gave, not a restatement of the
/// fields, which are self-naming.
constexpr ExclRow kExclRows[] = {
    {// The pair alone, nothing between: the baseline every `Rd = 1` row below is read against. Without it
     // this table would be satisfied by an engine whose STREX never works.
     .name = "f1 plain halfword pair",
     .asm_text = "ldrexh r3,[r0]; strexh r2,r1,[r0]; mov r0,r2; bx lr",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0, and the store lands",
     .code = kF1PairHalfword,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 0,
     .want_r3 = 0x1111,
     .want_mem = 0x2222'BEEFull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The row the two devices disagreed about. RetroBug takes the failing side: a spurious failure is
     // legal and a false success is not, and only one of the two is observable from inside the guest.
     .name = "f2 plain strh between",
     .asm_text = "strh #0x5a; ldrexh r3; strh #0x77; strexh r4,r1,[r0]",
     .proof = ExclProof::kConservative,
     .source = "Reno 5 said Rd=0, Tab S9 Rd=1 — RetroBug follows the Tab S9 on purpose",
     .code = kF2PlainStoreBetween,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 1,
     .want_r3 = 0x5a,  // what the exclusive load saw, before the plain store changed it
     .want_mem = 0x2222'0077ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// f3 as measured. Worth stating plainly: this row does **not** isolate `clrex`, because the plain
     // store inside the sequence already breaks the value comparison. The next row is the one that does.
     .name = "f3 clrex after a store",
     .asm_text = "ldrexh; movs r2,#0x99; strh r2,[r0]; clrex; strexh r4,r1,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=1, plain store's value survives",
     .code = kF3ClrexAfterStore,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 1,
     .want_r3 = 0x1111,
     .want_mem = 0x2222'0099ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The marker on its own: nothing has touched memory between the load and the store, so an engine that
     // reads `clrex` as a no-op passes the store here and fails. Not measured — ARM's text is unambiguous,
     // and f3 above measured the same direction with the value rule also in play.
     .name = "clrex, nothing else",
     .asm_text = "ldrexh r3,[r0]; clrex; strexh r4,r1,[r0]",
     .proof = ExclProof::kRule,
     .source = "ARM's CLREX contract; the control that makes f3's clrex observable",
     .code = kClrexWithNothingElse,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 1,
     .want_r3 = 0x1111,
     .want_r4 = 1,  // the status register itself, not just the copy of it the function returns
     .want_mem = 0x2222'1111ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// A store with no reservation anywhere in this run. `SyncIn` / `InterpExecutor::Run` clear the state
     // on entry, so a reservation cannot leak in from the previous row — which is what makes this row's
     // answer mean something in a table where eleven of the other twelve do an exclusive load first.
     .name = "f4 store, no prior load",
     .asm_text = "clrex; strexh r4,r1,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=1",
     .code = kF4NoPriorLoad,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 1,
     .want_r4 = 1,
     .want_mem = 0x2222'1111ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// f5, and the one row where RetroBug is stricter than *both* devices. The two halfwords hold the
     // **same** value on purpose: then a value-only monitor would pass the store, and an `Rd = 1` here can
     // only come from the reservation being for the address that was read. Hardware reserved a granule and
     // said 0; the divergence is the design, and it is in the direction a guest can retry out of.
     .name = "f5 reserved [r0], stored [r0,#2]",
     .asm_text = "ldrexh r3,[r0]; adds r0,r0,#2; strexh r4,r1,[r0]",
     .proof = ExclProof::kStricter,
     .source = "both devices said Rd=0 (granule); RetroBug reserves the exact address",
     .code = kF5StoreTwoBytesOn,
     .seed_lo = 0x1111'1111u,
     .want_r0 = 1,
     .want_r3 = 0x1111,
     .want_mem = 0x1111'1111ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// `ldaexh`, the instruction `docs/30` §Step 29 found in bionic's `pthread_mutex_lock` and
     // `trylock` — the reason this whole group was implemented. Same rule as f1, different opcode: a tier
     // that translated `ldrexh` and declined the acquire form would leave every mutex in a guest libc
     // broken while this table's first row stayed green.
     .name = "f6 ldaexh pair",
     .asm_text = "ldaexh r3,[r0]; strexh r2,r1,[r0]; mov r0,r2; bx lr",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0",
     .code = kF6AcquireHalfword,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 0,
     .want_r3 = 0x1111,
     .want_mem = 0x2222'BEEFull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The 8-bit width, which is its own callback on Tier J and its own `Opcode` on Tier I. The claim is
     // the *masked* byte: `0x11` -> `0xEF`, with the other three bytes of the word untouched, so a 16- or
     // 32-bit store cannot pass it.
     .name = "f8 byte pair",
     .asm_text = "ldrexb r3,[r0]; strexb r4,r1,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0",
     .code = kF8BytePair,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 0,
     .want_r3 = 0x11,
     .want_mem = 0x2222'11EFull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The 64-bit pair, at an 8-byte-aligned address as ARM requires. Both words are claimed, and the
     // values in them are deliberately different in each half and in each direction, so a swapped
     // `Rt`/`Rt2` — the one place in the IR where `ldrexd`/`strexd` can be got wrong and both tiers still
     // agree with each other — fails here rather than under a mutex.
     .name = "f9 ldrexd/strexd pair",
     .asm_text = "ldrexd r3,r4,[r0]; strexd r5,r3,r1,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0",
     .code = kF9DoublePair,
     .seed_lo = 0xAAAA'5555u,
     .seed_hi = 0xCCCC'3333u,
     .want_r0 = 0,
     .want_r3 = 0xAAAA'5555u,
     .want_r4 = 0xCCCC'3333u,
     .want_mem = 0x0000'BEEF'AAAA'5555ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The word pair through `0xe850`/`0xe840`, the only rows here whose encoding carries an `imm8`. The
     // translator computes the address as `Rn + (imm8 << 2)`, so a decoded field that was not zero would
     // store somewhere else entirely — and the masked word below says so.
     .name = "f10 word pair",
     .asm_text = "ldrex r3,[r0]; strex r2,r1,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0",
     .code = kF10WordPair,
     .seed_lo = 0x1111'2222u,
     .seed_hi = 0x3333'4444u,
     .want_r0 = 0,
     .want_r3 = 0x1111'2222u,
     .want_mem = 0x3333'4444'0000'BEEFull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// f11: the acquire load's own result is what gets stored. One blind spot, named rather than papered
     // over: because the value written equals the value that was already there, this row cannot tell "the
     // store landed" from "the store was skipped with Rd=0". f10 is the same width storing a *different*
     // word, and it is what covers that; what f11 adds over f10 is the acquire opcode and the load-to-store
     // data dependency, both of which are checked here.
     .name = "f11 ldaex, store what it read",
     .asm_text = "ldaex r3,[r0]; strex r2,r3,[r0]",
     .proof = ExclProof::kMeasured,
     .source = "measured on both devices: Rd=0",
     .code = kF11AcquireWord,
     .seed_lo = 0x1111'2222u,
     .want_r0 = 0,
     .want_r3 = 0x1111'2222u,
     .want_mem = 0x0000'0000'1111'2222ull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// The loop has to *terminate*, and terminate for the right reason. `r3 = 0x77` is the proof: the
     // second iteration's exclusive load saw the racing plain store's value, which can only happen if the
     // first store failed and the loop came round. The Reno 5 would have exited on the first pass with
     // `r3 = 0x1111` — the same conservative choice as f2, and here it costs one iteration and nothing
     // else. `r5 = 1` says the racing store ran at all, so the row cannot quietly become a plain pair.
     .name = "retry loop, store races once",
     .asm_text = "movs r5,#0; ldrexh; cbnz; strh #0x77; strexh r4,r1; cmp; bne back",
     .proof = ExclProof::kConservative,
     .source = "the number of passes is RetroBug's, not a measurement; a per-block reservation makes the "
               "store never pass and the run end on the budget",
     .code = kRetryLoop,
     .seed_lo = 0x2222'1111u,
     .want_r0 = 0,
     .want_r3 = 0x77,
     .want_r5 = 1,
     .want_mem = 0x2222'BEEFull,
     .mem_mask = 0xFFFF'FFFF'FFFF'FFFFull},
    {// Same code as f4, one premise changed: the target is not mapped and there is no reservation. Neither
     // may touch memory, so neither may fault — Tier I's `AdmitAddress` rejects before it reads, and Tier
     // J's back end branches around the whole callback when its marker is clear (`emit_arm64_memory.cpp`
     // `CBZ` on the state's exclusive marker). An engine that reads the target *before* deciding what to
     // do with it ends the run with `Fault` instead of a returned `Rd = 1` — and the two tiers are free to
     // disagree about that, which is exactly the shape Compare() exists to catch. No hardware claim;
     // neither device was asked.
     .name = "f4 shape, target unmapped",
     .asm_text = "clrex; strexh r4,r1,[r0] at an unmapped address",
     .proof = ExclProof::kRule,
     .source = "tier-vs-tier only, no hardware claim: a rejected store must not read or write",
     .code = kF4NoPriorLoad,
     .unmapped_target = true,
     .want_r0 = 1,
     .want_r4 = 1},
};

void exclusive_access_rows_match_hardware_rule_and_each_other() {
  Rig rig = build();
  if (!rig.ok) return;

  std::string err;
  const auto map = [&](exec::GuestAddr at, unsigned prot) {
    if (rig.mem->Map(at, mem::kGuestPage, prot, &err)) return true;
    ++checks;
    fail("could not map a page for the exclusive rows", __LINE__, "0x" + Hex(at) + ": " + err);
    return false;
  };
  if (!map(kExclCodePage, kRead | kWrite | kExec) || !map(kExclDataPage, kRead | kWrite)) return;

  // Halfwords in, bytes planted, 0 when the guest refused the write. The terminator is not written.
  const auto plant = [&](exec::GuestAddr at, const std::uint16_t* h) -> std::size_t {
    std::size_t n = 0;
    for (; h[n] != 0; ++n) {
      if (!rig.mem->Write(at + static_cast<exec::GuestAddr>(n * 2), &h[n], 2)) return 0;
    }
    return n * 2;
  };

  // The premise of every red row below: a planted Thumb function runs and returns on both tiers, so a
  // failure is about the exclusive logic and not about this mechanism. Same control the declined lane has.
  CHECK_CTX(plant(kExclCodePage, kControl) == sizeof kControl - 2, "the control pair did not land");
  CHECK_CTX((rig.mem->ProtAt(kExclCodePage) & kExec) != 0u,
            "the page the rows are planted in is not executable");
  {
    const std::pair<const char*, exec::Executor*> engines[] = {{"I", rig.interp.get()},
                                                               {"J", rig.jit.get()}};
    for (const auto& [which, engine] : engines) {
      exec::MachineState s = EntryState(kExclCodePage, 0, 0, /*thumb=*/true);
      const exec::RunResult r = engine->Run(s, kExclBudget);
      CHECK_CTX(r.reason == exec::StopReason::Returned && s.r[0] == 42,
                std::string{"the planted `movs r0, #42; bx lr` control on Tier "} + which + ": reason=" +
                    std::to_string(static_cast<int>(r.reason)) + " r0=" + std::to_string(s.r[0]) +
                    " detail=" + r.detail);
    }
  }

  exec::GuestAddr cursor = kExclCodePage + 0x100;  // clear of the control
  unsigned slot = 1;                               // slot 0 is the control's, which reads no memory
  int counted[4] = {};
  for (const auto& row : kExclRows) {
    ++counted[static_cast<int>(row.proof)];
    const std::size_t len = plant(cursor, row.code);
    ++checks;
    if (len == 0 || row.code[len / 2 - 1] != 0x4770) {
      fail("a planted row is not code that returns", __LINE__,
           std::string{row.name} + ": " + std::to_string(len) +
               " bytes, last halfword " + Hex(len == 0 ? 0 : row.code[len / 2 - 1]) +
               "; every blob has to end in `bx lr` (0x4770) before its zero terminator");
      continue;
    }
    // A row's own expectations are part of what is tested: one that asserts nothing about `Rd`, or nothing
    // about memory, would be satisfied by an STREX that always fails *and* by one that always passes, and
    // this table is read as evidence about which of those it is not.
    CHECK_CTX(row.want_r0 != kNoCheck,
              std::string{row.name} + " asserts nothing about Rd, so a store that never works passes it");
    CHECK_CTX(row.mem_mask != 0 || row.unmapped_target,
              std::string{row.name} + " asserts nothing about memory, so a store that writes anything "
                                      "passes it (the unmapped row has no memory to read, and its claim "
                                      "is that the run returns rather than faults)");

    const exec::GuestAddr data =
        row.unmapped_target ? kExclUnmapped : kExclDataPage + slot * kExclSlot;
    ++slot;
    if (row.unmapped_target) {
      CHECK_CTX(!rig.mem->IsMapped(kExclUnmapped),
                "the target of \"" + std::string{row.name} + "\" is mapped after all, so the row is no "
                "longer an unmapped access");
    } else {
      const std::uint64_t seed =
          (static_cast<std::uint64_t>(row.seed_hi) << 32) | static_cast<std::uint64_t>(row.seed_lo);
      CHECK_CTX(rig.mem->Write(data, &seed, 8), "could not seed " + std::string{row.name});
    }

    // `EntryState` seeds r2/r3 because the fixture's probes are JNI methods; these are freestanding code
    // with no `JNIEnv` in sight, so the address and the value go into the registers the code reads.
    exec::MachineState si = EntryState(cursor, 0, 0, /*thumb=*/true);
    si.r[0] = data;  ///< the address the exclusive pair reserves — the code's only handle on its data
    si.r[1] = kExclStore;
    exec::MachineState sj = si;
    const exec::RunResult ri = rig.interp->Run(si, kExclBudget);
    const exec::RunResult rj = rig.jit->Run(sj, kExclBudget);

    std::uint64_t mem_i = 0, mem_j = 0;
    if (!row.unmapped_target) {
      CHECK_CTX(rig.mem->Read(data, &mem_i, 8) && rig.mem->Read(data, &mem_j, 8),
                std::string{row.name} + ": the row's data could not be read back");
    }

    int checked = 0;
    const auto verdict = [&](const char* which, const exec::RunResult& res, const exec::MachineState& s,
                             std::uint64_t mem) {
      std::string wrong;
      const auto reg = [&](int n, std::uint32_t want) {
        if (want == kNoCheck) return;
        ++checked;
        if (s.r[n] != want) {
          wrong += " r" + std::to_string(n) + "=" + Hex(s.r[n]) + " want " + Hex(want);
        }
      };
      reg(0, row.want_r0);
      reg(3, row.want_r3);
      reg(4, row.want_r4);
      reg(5, row.want_r5);
      if (row.mem_mask != 0) {
        ++checked;
        if ((mem & row.mem_mask) != (row.want_mem & row.mem_mask)) {
          wrong += " mem@" + Hex(data) + "=" + Hex(mem) + ", masked " + Hex(mem & row.mem_mask) +
                   " want " + Hex(row.want_mem & row.mem_mask);
        }
      }
      ++checked;
      if (res.reason != exec::StopReason::Returned) {
        wrong += " stopped as reason=" + std::to_string(static_cast<int>(res.reason)) + " \"" +
                 res.detail + "\"";
      }
      return wrong.empty() ? std::string{} : std::string{which} + ":" + wrong;
    };
    const std::string i_bad = verdict("interp", ri, si, mem_i);
    const std::string j_bad = verdict("jit   ", rj, sj, mem_j);
    const std::string tag = std::string{row.name} + " | " + row.source;
    CHECK_CTX(checked > 0, tag + " checked nothing at all");
    CHECK_CTX(i_bad.empty(), tag + "\n      " + i_bad + "\n      " + Describe("interp", ri, si));
    CHECK_CTX(j_bad.empty(), tag + "\n      " + j_bad + "\n      " + Describe("jit   ", rj, sj));
    Compare(tag.c_str(), si, ri, sj, rj);

    std::fprintf(stderr,
                 "[excl] %-26s %-42s Rd want=%u I=%u J=%u | mem want=%s I=%s J=%s | steps I=%u J=%u\n",
                 row.name, row.asm_text, row.want_r0, si.r[0], sj.r[0],
                 Hex(row.want_mem & row.mem_mask).c_str(), Hex(mem_i & row.mem_mask).c_str(),
                 Hex(mem_j & row.mem_mask).c_str(), ri.instructions_executed, rj.instructions_executed);
    cursor = static_cast<exec::GuestAddr>(cursor + ((len + 3) & ~static_cast<std::size_t>(3)));
  }

  std::fprintf(stderr, "[excl] %zu rows: %d both devices agree, %d follow the failing device, %d stricter "
                       "than either, %d RetroBug's own rule\n",
               sizeof kExclRows / sizeof kExclRows[0], counted[static_cast<int>(ExclProof::kMeasured)],
               counted[static_cast<int>(ExclProof::kConservative)],
               counted[static_cast<int>(ExclProof::kStricter)],
               counted[static_cast<int>(ExclProof::kRule)]);
  // The three rows where RetroBug answers differently from at least one device are the reason this lane
  // encodes a rule instead of a set of opcodes. If they were relabelled as measurements — or deleted —
  // the lane would quietly go back to being a tier-vs-tier check with a hardware-sounding name.
  CHECK_CTX(counted[static_cast<int>(ExclProof::kConservative)] == 2 &&
                counted[static_cast<int>(ExclProof::kStricter)] == 1,
            "expected three divergence rows, found " +
                std::to_string(counted[static_cast<int>(ExclProof::kConservative)] +
                               counted[static_cast<int>(ExclProof::kStricter)]));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 && std::string_view{argv[1]} == "--interpret-probe") return interpret_probe(argv[2]);

  both_tiers_agree_and_match_hardware();
  fp_groups_are_accurate_or_explicitly_refused();
  declined_instructions_refuse_by_name();
  exclusive_access_rows_match_hardware_rule_and_each_other();
  invalidation_reaches_the_compiled_code();
  both_tiers_stop_at_the_same_svc();
  a_guest_cacheflush_reaches_the_tier_that_owns_the_cache();
  unmapped_code_faults_in_both_tiers();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
