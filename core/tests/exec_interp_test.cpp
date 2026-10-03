/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for Tier I, the IR interpreter (C02).
 *
 * This is the first place in RetroBug where guest instructions are *executed* rather than described,
 * so the assertions are deliberately not self-referential: every expected value comes from an ARMv7
 * core running the same compiled code, captured in `tests/corpus/oracle-armv7.txt` by
 * `tools/run_armv7_oracle.sh` (docs/30 §Step 5g). Nothing here says "the interpreter agrees with the
 * interpreter".
 *
 * The three arithmetic-heavy checks are also the flag-channel test, indirectly and strongly. fib's body
 * is `n < 2 ? n : fib(n-1) + fib(n-2)`: getting 6765 requires SUBS/CMP to set N and Z the way ARM
 * defines, `blt`/`itt`-style predicates to read the *right* copy of them, and the carry bit — which
 * `wide_probe` XORs into its result after a `movw`/`movt` — to survive the intervening instructions.
 * A wrong C or V does not crash; it produces a number that is almost right, which is why both
 * functions are run and why the exact hardware values are hardcoded rather than a range.
 */
#include "rb/exec/interp.h"
#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
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

/// The sentinel return address. Unmapped on purpose: Run checks it *before* translating, so a guest
/// returning here stops cleanly rather than faulting on a fetch — the property C08's call bridge needs.
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;

/// Stand-ins for the two arguments JNI inserts ahead of every native method's own parameters. The
/// fixture casts both to `void`, so nothing dereferences them; step 7's `rb::abi` fake `JNIEnv` will
/// supply a real one.
constexpr exec::GuestAddr kFakeJNIEnv = 0x0000'0000u;
constexpr exec::GuestAddr kFakeClass = 0x0000'0000u;

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

/// What the callee-saved registers are loaded with before each call, indexed by register number so the
/// assertion below reads as the same fact the epilogue does. Distinct per register, and nothing `fib`
/// could compute: a `pop` that wrote a saved value back to the wrong slot, or the list in the wrong
/// order, changes one of these and is caught here instead of showing up as a wrong total several
/// instructions later.
constexpr std::uint32_t kCalleeSaved[16] = {
    0, 0, 0, 0, 0x4444'4444u, 0x5555'5555u, 0, 0x7777'7777u, 0, 0, 0, 0, 0, 0, 0, 0,
};

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
  std::unique_ptr<exec::InterpExecutor> exec;
  bool ok = false;
};

Rig build() {
  Rig rig;
  const auto bytes = read_file(RB_FIXTURE_PATH);
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = GuestMemory::Create(32u << 20, model, &err);
  if (!rig.mem || bytes.empty()) {
    fail("could not create guest memory or read the fixture", __LINE__, err);
    return rig;
  }
  loader::LoadOptions opts;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return rig;
  }
  // A stack the guest can push to and read back. Grown downward from kStackTop.
  if (!rig.mem->Map(kStackTop - 0x8000, 0x8000, kRead | kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  rig.exec = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.ok = true;
  return rig;
}

/// One guest call: LR = the sentinel, SP at the top of the mapped stack, and the argument placed where
/// the *declaration* says it goes rather than where an intuition about "the argument" suggests. These
/// exports are JNI methods, so AAPCS hands `env` in r0 and `clazz` in r1 and the C parameter arrives in
/// **r2**. Passing it in r0 is not a wrong answer the engine can be debugged against: `fib(n)` reads
/// `n = 0` and correctly returns 0, which is a plausible result for every input and looks exactly like a
/// flag-channel defect. `tools/run_armv7_oracle.sh` is what pins the register positions down — the
/// hardware was called the same way, with `NULL, NULL, arg`.
exec::RunResult call(Rig& rig, exec::GuestAddr where, std::int32_t arg, exec::MachineState& state,
                     bool thumb) {
  state = exec::MachineState{};
  state.pc = where;
  state.mode = thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  state.r[0] = kFakeJNIEnv;
  state.r[1] = kFakeClass;
  state.r[2] = static_cast<std::uint32_t>(arg);
  for (int r : {4, 5, 7}) state.r[r] = kCalleeSaved[r];
  state.r[13] = kStackTop;
  state.r[14] = kReturnAddress;
  state.r[15] = where;
  return rig.exec->Run(state, 4'000'000);
}

/// The hardware values from tests/corpus/oracle-armv7.txt, verbatim.
struct Expect {
  const char* export_name;
  std::int32_t arg;
  std::int32_t want;
};

void runs_the_functions_the_way_the_hardware_does() {
  Rig rig = build();
  if (!rig.ok) return;

  const Expect cases[] = {
      {"Java_com_retrobug_fixture_MainActivity_fib", 0, 0},
      {"Java_com_retrobug_fixture_MainActivity_fib", 1, 1},
      {"Java_com_retrobug_fixture_MainActivity_fib", 2, 1},
      {"Java_com_retrobug_fixture_MainActivity_fib", 3, 2},
      {"Java_com_retrobug_fixture_MainActivity_fib", 4, 3},
      {"Java_com_retrobug_fixture_MainActivity_fib", 5, 5},
      {"Java_com_retrobug_fixture_MainActivity_fib", 6, 8},
      {"Java_com_retrobug_fixture_MainActivity_fib", 7, 13},
      {"Java_com_retrobug_fixture_MainActivity_fib", 8, 21},
      {"Java_com_retrobug_fixture_MainActivity_fib", 20, 6765},
      {"Java_com_retrobug_fixture_MainActivity_wide", 0, 52},
      {"Java_com_retrobug_fixture_MainActivity_wide", 2, 50},
      {"Java_com_retrobug_fixture_MainActivity_wide", -1, 55},
  };

  for (const auto& c : cases) {
    const auto where = rig.mod->FindExport(c.export_name);
    CHECK_CTX(where.has_value(), std::string{"export "} + c.export_name);
    if (!where) continue;
    exec::MachineState state;
    const exec::RunResult r = call(rig, where->addr, c.arg, state, where->thumb);
    const std::string what = std::string{c.export_name} + "(" + std::to_string(c.arg) + ")";
    CHECK_CTX(r.reason == exec::StopReason::Returned,
              what + " stopped with reason " + std::to_string(static_cast<int>(r.reason)) +
                  " detail=" + r.detail);
    CHECK_CTX(static_cast<std::int32_t>(state.r[0]) == c.want,
              what + " returned " + std::to_string(static_cast<std::int32_t>(state.r[0])) +
                  ", hardware said " + std::to_string(c.want) + " after " +
                  std::to_string(r.instructions_executed) + " IR steps");
    CHECK(r.instructions_executed > 0);
    // `fib`'s prologue is `push {r4, r5, r7, lr}` and its epilogue `pop {r4, r5, r7, pc}`: the return
    // address comes off the stack into PC, and **r14 is never written back**. So LR on return holds
    // whatever the last executed `bl` left in it (`0x34d7`, inside `fib`), and an assertion that it
    // still equals the sentinel would be a claim about the ABI that the code contradicts — the
    // disassembly in docs/30 §Step 6d is what settled it. What the frame *does* promise is that the
    // callee-saved registers come back, so that is what is checked, with values the guest has no
    // reason to produce itself.
    CHECK_CTX(state.r[4] == kCalleeSaved[4] && state.r[5] == kCalleeSaved[5] &&
                  state.r[7] == kCalleeSaved[7] && state.r[13] == kStackTop,
              "callee-saved state after " + what + ": r4=" + Hex(state.r[4]) +
                  " r5=" + Hex(state.r[5]) + " r7=" + Hex(state.r[7]) + " sp=" + Hex(state.r[13]));
    std::fprintf(stderr, "[interp] %s = %d in %u steps  sp=%x lr=%x cpsr=%x\n", what.c_str(),
                 static_cast<std::int32_t>(state.r[0]), r.instructions_executed, state.r[13],
                 state.r[14], state.cpsr);
  }
}

/// Reading a register that only the guest can write, after the call, is the difference between an
/// interpreter that mutates the caller's state and one that mutates a copy.
void guest_writes_are_visible_in_the_callers_state() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!fib) return;
  exec::MachineState state;
  const exec::RunResult r = call(rig, fib->addr, 10, state, fib->thumb);
  CHECK(r.reason == exec::StopReason::Returned);
  CHECK(static_cast<std::int32_t>(state.r[0]) == 55);  // fib(10), hardware value
  // The callee moved SP and must have restored it; if it had not, a stack leak would show up here as
  // a difference between entry and exit that the guest itself never intended.
  CHECK_CTX(state.r[13] == kStackTop,
            "SP came back at 0x" + std::to_string(state.r[13]) + " instead of the entry value");
}

void two_calls_from_the_same_state_agree() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto wide = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_wide");
  if (!wide) return;
  std::uint32_t first = 0, second = 0;
  {
    exec::MachineState state;
    const auto r = call(rig, wide->addr, 2, state, wide->thumb);
    CHECK(r.reason == exec::StopReason::Returned);
    first = state.r[0];
  }
  {
    exec::MachineState state;
    const auto r = call(rig, wide->addr, 2, state, wide->thumb);
    CHECK(r.reason == exec::StopReason::Returned);
    second = state.r[0];
  }
  CHECK_CTX(first == second, "the same call produced " + std::to_string(first) + " then " +
                                 std::to_string(second));
}

/// `Invalidate` exists for self-modifying code. Tier I caches nothing, so a call after an
/// invalidation covering the whole image must be indistinguishable from one before it. Asserted rather
/// than assumed, because "it is a no-op" is also the shape of a broken implementation of it.
void invalidation_changes_nothing_it_should_not() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!fib) return;
  exec::MachineState state;
  CHECK(call(rig, fib->addr, 12, state, fib->thumb).reason == exec::StopReason::Returned);
  const std::uint32_t before = state.r[0];
  rig.exec->Invalidate(rig.mod->base, rig.mod->base + rig.mod->extent);
  CHECK(call(rig, fib->addr, 12, state, fib->thumb).reason == exec::StopReason::Returned);
  CHECK_CTX(before == state.r[0] && before == 144u,
            "before " + std::to_string(before) + " after " + std::to_string(state.r[0]));
}

/// A budget is a budget: it must stop, name itself, and leave state that can be resumed.
void the_instruction_budget_stops_cleanly() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!fib) return;
  exec::MachineState state;
  state.pc = fib->addr;
  state.mode = isa::Mode::Thumb;
  state.r[0] = kFakeJNIEnv;
  state.r[1] = kFakeClass;
  state.r[2] = 20;  // `n`, in the third argument register — see `call`
  state.r[13] = kStackTop;
  state.r[14] = kReturnAddress;
  const exec::RunResult small = rig.exec->Run(state, 3);
  CHECK(small.reason == exec::StopReason::Budget);
  CHECK(small.instructions_executed > 0);
  // Resuming has to be possible: the same state carried forward must reach the answer.
  const exec::RunResult rest = rig.exec->Run(state, 4'000'000);
  CHECK_CTX(rest.reason == exec::StopReason::Returned,
            "after a budget stop the resume ended in reason " +
                std::to_string(static_cast<int>(rest.reason)) + " detail=" + rest.detail);
  CHECK(static_cast<std::int32_t>(state.r[0]) == 6765);
}

/// Code that cannot be fetched is a Fault with a name attached, never a silent zero-step return.
void unmapped_code_faults_rather_than_returning() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::MachineState state;
  state.pc = 0x0090'0000u;  // in the window, mapped by nothing
  state.mode = isa::Mode::Thumb;
  state.r[13] = kStackTop;
  const exec::RunResult r = rig.exec->Run(state, 1000);
  CHECK_CTX(r.reason == exec::StopReason::Fault,
            "unmapped code produced reason " + std::to_string(static_cast<int>(r.reason)));
  CHECK(!r.detail.empty());
}

/// A budget stop that lands *between the legs of an IT block* is the one place IT state has to cross
/// a Run boundary, and losing it there executes the remaining legs unconditionally — `addgt` and
/// `addle` would both run and the last would win. The sweep below stops `it_probe` at every budget
/// from 1 and resumes it in chunks of the same size until it returns; every chunking must produce the
/// hardware answer. `saw_live_it` proves the sweep actually crossed a leg boundary with IT state live
/// — without it, a sweep that always stopped between blocks would pass with or without the fold.
void a_budget_stop_between_it_legs_resumes_predicated() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto probe = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_it_probe");
  if (!probe) return;

  struct Case {
    std::int32_t a, b, want;  // oracle-armv7.txt's it_probe row
  };
  const Case cases[] = {{7, 3, 42}, {3, 7, 1008}, {5, 5, 42}, {101, 1, 104}, {0, 0, 1005}, {-5, 3, 1000}};
  bool saw_live_it = false;
  for (const auto& c : cases) {
    // The unchunked answer, for the comparison. Built inline rather than through `call`, because
    // `call` places one argument and `it_probe` takes two — a baseline that ran (a, 0) against a
    // chunked run of (a, b) would fail its own comparison for every b that mattered.
    exec::MachineState whole;
    whole.pc = probe->addr;
    whole.mode = probe->thumb ? isa::Mode::Thumb : isa::Mode::Arm;
    whole.r[0] = kFakeJNIEnv;
    whole.r[1] = kFakeClass;
    whole.r[2] = static_cast<std::uint32_t>(c.a);
    whole.r[3] = static_cast<std::uint32_t>(c.b);
    for (int r : {4, 5, 7}) whole.r[r] = kCalleeSaved[r];
    whole.r[13] = kStackTop;
    whole.r[14] = kReturnAddress;
    whole.r[15] = probe->addr;
    const auto whole_call = rig.exec->Run(whole, 4'000'000);
    CHECK_CTX(whole_call.reason == exec::StopReason::Returned, "unchunked it_probe stopped early");
    CHECK_CTX(static_cast<std::int32_t>(whole.r[0]) == c.want, "unchunked it_probe is wrong");

    for (unsigned budget = 1; budget <= 64; ++budget) {
      exec::MachineState state;
      state.pc = probe->addr;
      state.mode = probe->thumb ? isa::Mode::Thumb : isa::Mode::Arm;
      state.r[0] = kFakeJNIEnv;
      state.r[1] = kFakeClass;
      state.r[2] = static_cast<std::uint32_t>(c.a);
      state.r[3] = static_cast<std::uint32_t>(c.b);
      for (int r : {4, 5, 7}) state.r[r] = kCalleeSaved[r];
      state.r[13] = kStackTop;
      state.r[14] = kReturnAddress;
      state.r[15] = probe->addr;

      exec::RunResult chunk = rig.exec->Run(state, budget);
      unsigned guard = 0;
      while (chunk.reason == exec::StopReason::Budget && guard++ < 100'000) {
        // The mirror and CPSR must describe the same IT state at every stop; a stop inside an IT
        // sequence has to carry it forward rather than report the zero from before the block.
        //
        // Spelled out here rather than compared against `exec::ItStateFromCpsr`: Tier I assigns that
        // expression to `state.itstate` itself, so `state.itstate == ItStateFromCpsr(state.cpsr)` could
        // not fail whatever the helper did. This form pins the layout the fold depends on — ITSTATE<7:2>
        // at CPSR bits 15:10 and ITSTATE<1:0> at bits 26:25 — so a change to the helper's bit positions
        // breaks this check instead of travelling with it.
        const std::uint8_t expect_it =
            static_cast<std::uint8_t>(((state.cpsr >> 8) & 0xFCu) | ((state.cpsr >> 25) & 0x03u));
        CHECK_CTX(state.itstate == expect_it,
                  "itstate " + std::to_string(state.itstate) + " disagrees with cpsr " +
                      Hex(state.cpsr) + " at a budget stop");
        if (state.itstate != 0) saw_live_it = true;
        chunk = rig.exec->Run(state, budget);
      }
      const std::string what = "it_probe(" + std::to_string(c.a) + "," + std::to_string(c.b) +
                               ") chunked at " + std::to_string(budget);
      CHECK_CTX(chunk.reason == exec::StopReason::Returned,
                what + " ended in reason " + std::to_string(static_cast<int>(chunk.reason)) + " " +
                    chunk.detail);
      CHECK_CTX(static_cast<std::int32_t>(state.r[0]) == c.want,
                what + " = " + std::to_string(static_cast<std::int32_t>(state.r[0])) +
                    ", hardware said " + std::to_string(c.want));
    }
  }
  CHECK_CTX(saw_live_it,
            "no budget in the sweep stopped with IT state live, so this test proved nothing");
}

}  // namespace

int main() {
  runs_the_functions_the_way_the_hardware_does();
  guest_writes_are_visible_in_the_callers_state();
  two_calls_from_the_same_state_agree();
  invalidation_changes_nothing_it_should_not();
  the_instruction_budget_stops_cleanly();
  unmapped_code_faults_rather_than_returning();
  a_budget_stop_between_it_legs_resumes_predicated();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
