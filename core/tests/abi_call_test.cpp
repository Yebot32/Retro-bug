/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for C08 `rb::abi`, the AAPCS32 call bridge.
 *
 * docs/20 names this file as where the ABI-correctness review pass concentrates, so the bulk of it is
 * marshalling cases read straight off the AAPCS rules rather than off a guest program: an argument
 * placed in the wrong register is not a crash, it is a call that computes something plausible from the
 * wrong inputs. The guest-level half then calls the real fixture and asserts the same hardware values
 * `docs/30` §Step 6d established, so the bridge is tested against silicon and not against itself.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace abi = rb::abi;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace loader = rb::loader;
namespace sc = rb::syscall;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                              \
  do {                                          \
    ++checks;                                   \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                     \
  do {                                              \
    ++checks;                                       \
    if (!(cond)) fail(#cond, __LINE__, detail);      \
  } while (0)

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
constexpr exec::GuestAddr kFn = 0x0002'0000u;  ///< a stand-in callee for the marshalling-only cases

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
  std::unique_ptr<exec::InterpExecutor> engine;
  std::optional<loader::GuestModule> mod;
  std::unique_ptr<abi::AbiContext> abi_ctx;
  bool ok = false;
};

/// Host-call recorder. `HostFn` gets the live state and nothing else, so the results travel in
/// file-scope state that each test clears before it runs — and reads back to prove the guest's
/// arguments really did arrive in the registers AAPCS says they should.
int host_calls = 0;
std::uint32_t seen_r0 = 0, seen_r1 = 0, seen_r2 = 0, seen_r3 = 0;

/// Records the incoming argument registers and returns a value the guest could not have produced.
bool record_and_succeed(exec::MachineState& state, std::string*) {
  ++host_calls;
  seen_r0 = state.r[0];
  seen_r1 = state.r[1];
  seen_r2 = state.r[2];
  seen_r3 = state.r[3];
  state.r[0] = 0x1234'5678u;  ///< what the guest should see back in r0
  return true;
}

/// A host function that refuses, which must surface as a named fault rather than as a zero result.
bool refuse(exec::MachineState& state, std::string* err) {
  ++host_calls;
  (void)state;
  if (err != nullptr) *err = "refused by the test";
  return false;
}

/// A rig with the engine wired to the bridge's dispatch, which is the pair the tiers and C09 will
/// always use together.
Rig build(bool with_fixture = false) {
  Rig rig;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  // 64 MiB: the stack alone wants 32 KiB at kStackTop, and the host-call slots live just above the
  // return sentinel at an address no page backs (they are recognised before translation).
  rig.mem = GuestMemory::Create(64u << 20, model, &err);
  if (!rig.mem) {
    fail("could not create guest memory", __LINE__, err);
    return rig;
  }
  if (!rig.mem->Map(kStackTop - (32u << 10), 32u << 10, kRead | kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  // The bridge is the dispatch table, so it is built first and the executor then points at it; the
  // back-reference is set immediately after, which is the only ordering that leaves neither object
  // holding a reference to something not yet constructed.
  rig.abi_ctx = std::make_unique<abi::AbiContext>(*rig.mem, kStackTop, kReturnAddress);
  rig.engine = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress, rig.abi_ctx.get());
  rig.abi_ctx->set_executor(*rig.engine);
  rig.ok = true;

  if (!with_fixture) return rig;
  const auto bytes = read_file(RB_FIXTURE_PATH);
  if (bytes.empty()) {
    fail("could not read the fixture", __LINE__);
    rig.ok = false;
    return rig;
  }
  loader::LoadOptions opts;
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    rig.ok = false;
  }
  return rig;
}

exec::MachineState Fresh() {
  exec::MachineState s{};
  s.mode = isa::Mode::Thumb;
  s.r[13] = kStackTop;
  return s;
}

std::uint32_t Spill(const Rig& rig, const exec::MachineState& s, unsigned index) {
  std::uint32_t v = 0;
  const exec::GuestAddr at = s.r[13] + 16u + index * 4u;
  if (!rig.mem->Read(at, &v, 4)) fail("cannot read spill slot", __LINE__, Hex(at));
  return v;
}

// --------------------------------------------------------------------- argument placement

void four_or_less_stay_in_core_registers() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::MachineState s = Fresh();
  std::string err;
  const abi::Args32 a = {abi::Value32::S32(1), abi::Value32::S32(2), abi::Value32::Ptr(3),
                         abi::Value32::U32(4)};
  CHECK(abi::PlaceArgs(*rig.mem, s, kFn, a, kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK(s.r[0] == 1 && s.r[1] == 2 && s.r[2] == 3 && s.r[3] == 4);
  CHECK(s.pc == kFn);
  // Thumb bit 0 of LR is the ISA tag, and `bx lr` reads it: dropping it here returns the guest into
  // ARM-mode decoding of Thumb bytes, which faults somewhere far away from the cause.
  CHECK_CTX(s.r[14] == (kReturnAddress | 1u), "LR=" + Hex(s.r[14]));
  CHECK((s.r[13] & 0xFu) == 0);  ///< 16-byte aligned below the caller, per the plan's invariant
}

void the_fifth_argument_spills_at_the_lowest_stack_address() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::MachineState s = Fresh();
  std::string err;
  const abi::Args32 a = {abi::Value32::S32(10), abi::Value32::S32(20), abi::Value32::S32(30),
                         abi::Value32::S32(40), abi::Value32::S32(50), abi::Value32::S32(60)};
  CHECK(abi::PlaceArgs(*rig.mem, s, kFn, a, kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK(s.r[0] == 10 && s.r[3] == 40);
  // Ascending addresses for successive arguments. A "reverse the stack args" reflex is invisible with
  // one spilled argument and wrong with two, which is why both are asserted here.
  CHECK_CTX(Spill(rig, s, 0) == 50, "got " + std::to_string(Spill(rig, s, 0)));
  CHECK_CTX(Spill(rig, s, 1) == 60, "got " + std::to_string(Spill(rig, s, 1)));
}

void a_jlong_needs_an_even_odd_pair_and_will_not_take_r1_r2() {
  Rig rig = build();
  if (!rig.ok) return;
  std::string err;

  exec::MachineState alone = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, alone, kFn, {abi::Value32::I64(0x1'0000'0002LL)}, kReturnAddress,
                       abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(alone.r[0] == 2 && alone.r[1] == 1, "r0=" + Hex(alone.r[0]) + " r1=" + Hex(alone.r[1]));

  // The case that hides: one 4-byte argument first, so the 8-byte one *cannot* start at r1. r1 is
  // consumed by nothing, and the pair lands in r2/r3.
  exec::MachineState after = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, after, kFn,
                       {abi::Value32::S32(0xaa), abi::Value32::I64(0x1'0000'0002LL)}, kReturnAddress,
                       abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(after.r[0] == 0xaa && after.r[1] == 0 && after.r[2] == 2 && after.r[3] == 1,
            "r1=" + Hex(after.r[1]) + " r2=" + Hex(after.r[2]) + " r3=" + Hex(after.r[3]));
}

void floats_and_doubles_take_no_core_register_at_all() {
  Rig rig = build();
  if (!rig.ok) return;
  std::string err;

  // softfp *passes* in VFP registers — it only computes in software. A double in d0 must leave r0..r3
  // untouched for the integers that follow, or every `jdouble` in the corpus shifts its neighbours.
  exec::MachineState s = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, s, kFn,
                       {abi::Value32::F64(2.0), abi::Value32::S32(7), abi::Value32::F32(1.5f)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(s.r[0] == 7, "r0=" + Hex(s.r[0]) + " — the int should be the first core argument");
  CHECK_CTX(s.d[0] == 0x4000'0000'0000'0000ull, "d0=" + std::to_string(s.d[0]));  // double 2.0
  // 1.5f in s2 (units 0,1 are d0), and the *other* half of d1 must be left alone.
  CHECK_CTX((s.d[1] & 0xffff'ffffu) == 0x3fc0'0000u, "d1 low=" + Hex(static_cast<std::uint32_t>(s.d[1])));

  exec::MachineState two = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, two, kFn, {abi::Value32::F32(1.0f), abi::Value32::F32(2.0f)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK((two.d[0] & 0xffff'ffffu) == 0x3f80'0000u);
  CHECK((two.d[0] >> 32) == 0x4000'0000u);  ///< s1 is the *high* half of d0 on little-endian ARM
}

void a_frame_too_small_refuses_without_half_applying_it() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::MachineState s = Fresh();
  const std::uint32_t sp_before = s.r[13];
  std::string err;
  // Six 8-byte arguments cannot fit the registers, and a frame of 8 bytes cannot hold the spills.
  const abi::Args32 many = {abi::Value32::I64(1),  abi::Value32::I64(2),  abi::Value32::I64(3),
                            abi::Value32::I64(4),  abi::Value32::I64(5),  abi::Value32::I64(6)};
  CHECK(!abi::PlaceArgs(*rig.mem, s, kFn, many, kReturnAddress, 8, &err));
  CHECK_CTX(!err.empty(), "refusal carried no reason");
  CHECK_CTX(s.r[0] == 0 && s.r[13] == sp_before,
            "SP moved to " + Hex(s.r[13]) + " on a call that was refused");
}

// --------------------------------------------------------------------- return values

void return_values_come_from_where_the_type_says() {
  exec::MachineState s = Fresh();
  s.r[0] = 0x1111'1111u;
  s.r[1] = 0x2222'2222u;
  s.d[0] = 0x3333'3333u;  // s0 = 0x33333333, s1 = 0

  CHECK(abi::ReturnValue(s, abi::Shorty::For("II")).i() == 0x1111'1111);
  CHECK(abi::ReturnValue(s, abi::Shorty::For("JI")).j() == 0x2222'2222'1111'1111ull);
  CHECK(abi::ReturnValue(s, abi::Shorty::For("FI")).bits == 0x3333'3333ull);  // s0, the low half of d0
  CHECK(abi::ReturnValue(s, abi::Shorty::For("DL")).bits == 0x3333'3333ull);  // d0 whole

  // 'Z' is an unsigned *byte*: a callee that left a stale upper byte in r0 must not read back as true
  // through it, and must not read back as 0x11 either.
  exec::MachineState z = Fresh();
  z.r[0] = 0x0000'0100u;
  CHECK(abi::ReturnValue(z, abi::Shorty::For("ZL")).w() == 0u);
  z.r[0] = 0x0000'0001u;
  CHECK(abi::ReturnValue(z, abi::Shorty::For("ZL")).w() == 1u);
}

// --------------------------------------------------------------------- host-call dispatch

void a_host_entry_stops_the_executor_and_resumes_at_the_link() {
  Rig rig = build();
  if (!rig.ok) return;
  host_calls = 0;
  rig.abi_ctx->Bind(7, &record_and_succeed);

  const abi::Shorty shorty = abi::Shorty::For("IL");
  exec::RunResult outcome{};
  // Calling a slot directly is the whole mechanism without needing guest code to reach it: PlaceArgs
  // puts PC on the slot, the executor recognises it before translating, and the loop returns to LR —
  // which is the sentinel — so the call completes normally.
  const auto ret = rig.abi_ctx->CallGuest(rig.abi_ctx->SlotFor(7), {abi::Value32::S32(0x5a)}, shorty,
                                          1000, &outcome);
  CHECK_CTX(outcome.reason == exec::StopReason::Returned,
            "reason " + std::to_string(static_cast<int>(outcome.reason)) + " " + outcome.detail);
  CHECK_CTX(host_calls == 1, "host calls: " + std::to_string(host_calls));
  CHECK_CTX(seen_r0 == 0x5a, "r0 at the host call was " + Hex(seen_r0));
  CHECK_CTX(ret.bits == 0x1234'5678ull, "returned " + Hex(ret.w()));
}

void a_refused_host_call_is_a_fault_naming_the_slot() {
  Rig rig = build();
  if (!rig.ok) return;
  host_calls = 0;
  rig.abi_ctx->Bind(42, &refuse);
  exec::RunResult outcome{};
  rig.abi_ctx->CallGuest(rig.abi_ctx->SlotFor(42), {}, abi::Shorty::For("VL"), 1000, &outcome);
  CHECK(outcome.reason == exec::StopReason::Fault);
  CHECK_CTX(outcome.detail.find("42") != std::string::npos, outcome.detail);
  CHECK_CTX(outcome.detail.find("refused") != std::string::npos, outcome.detail);
  CHECK(host_calls == 1);
}

/// A table entry nobody implemented must be a fault that says which, not a call that returns zero: a
/// guest that receives a NULL from a missing JNI function often carries on and produces a wrong number
/// somewhere else entirely.
void an_unbound_slot_says_so_and_which_one_it_was() {
  Rig rig = build();
  if (!rig.ok) return;
  host_calls = 0;
  CHECK(!rig.abi_ctx->IsBound(99));
  exec::RunResult outcome{};
  rig.abi_ctx->CallGuest(rig.abi_ctx->SlotFor(99), {}, abi::Shorty::For("IL"), 1000, &outcome);
  CHECK(outcome.reason == exec::StopReason::Fault);
  CHECK_CTX(outcome.detail.find("not bound") != std::string::npos, outcome.detail);
  CHECK_CTX(outcome.detail.find("99") != std::string::npos, outcome.detail);
  CHECK(host_calls == 0);
}

void a_slot_range_misalignment_is_not_a_host_call() {
  Rig rig = build();
  if (!rig.ok) return;
  std::uint32_t slot = 0;
  CHECK(!rig.abi_ctx->IsHostEntry(kReturnAddress, &slot));         ///< the sentinel is not a slot
  CHECK(!rig.abi_ctx->IsHostEntry(rig.abi_ctx->SlotFor(3) + 2, &slot));  ///< slots are 4 apart
  CHECK(!rig.abi_ctx->IsHostEntry(rig.abi_ctx->SlotFor(abi::AbiContext::kMaxSlots), &slot));
  CHECK(rig.abi_ctx->IsHostEntry(rig.abi_ctx->SlotFor(229), &slot));
  CHECK_CTX(slot == 229, "slot " + std::to_string(slot));
}

// --------------------------------------------------------------------- through real guest code

struct Expect {
  const char* export_name;
  std::int32_t arg;
  std::int32_t want;
};

void guest_functions_called_through_the_bridge_match_hardware() {
  Rig rig = build(true);
  if (!rig.ok) return;

  const Expect cases[] = {
      {"Java_com_retrobug_fixture_MainActivity_fib", 0, 0},   {"Java_com_retrobug_fixture_MainActivity_fib", 1, 1},
      {"Java_com_retrobug_fixture_MainActivity_fib", 2, 1},   {"Java_com_retrobug_fixture_MainActivity_fib", 5, 5},
      {"Java_com_retrobug_fixture_MainActivity_fib", 10, 55}, {"Java_com_retrobug_fixture_MainActivity_fib", 20, 6765},
      {"Java_com_retrobug_fixture_MainActivity_wide", 0, 52}, {"Java_com_retrobug_fixture_MainActivity_wide", 2, 50},
      {"Java_com_retrobug_fixture_MainActivity_wide", -1, 55},
  };

  for (const auto& c : cases) {
    const auto where = rig.mod->FindExport(c.export_name);
    CHECK(where.has_value());
    if (!where) continue;
    exec::RunResult outcome{};
    // The shorty is the one ART would produce for this method: int return, then the two hidden JNI
    // arguments, then the int parameter. `n` is the *third* argument and so lands in r2 — the fact
    // that got `docs/30` §Step 6d wrong once already, checked here by the hardware value rather than
    // by a register dump.
    const auto ret = rig.abi_ctx->CallGuest(
        where->addr,
        {abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(c.arg)},
        abi::Shorty::For("IILLI"), 4'000'000, &outcome);
    const std::string what = std::string{c.export_name} + "(" + std::to_string(c.arg) + ")";
    CHECK_CTX(outcome.reason == exec::StopReason::Returned,
              what + " stopped with " + std::to_string(static_cast<int>(outcome.reason)) +
                  " " + outcome.detail);
    CHECK_CTX(ret.i() == c.want, what + " returned " + std::to_string(ret.i()) + ", hardware said " +
                                     std::to_string(c.want));
    std::fprintf(stderr, "[abi] %s = %d via the bridge\n", what.c_str(), ret.i());
  }
}

void the_bridge_leaves_the_guest_stack_where_it_found_it() {
  Rig rig = build(true);
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!fib) return;
  exec::RunResult outcome{};
  const auto ret = rig.abi_ctx->CallGuest(fib->addr,
                                          {abi::Value32::Ptr(0), abi::Value32::Ptr(0),
                                           abi::Value32::S32(12)},
                                          abi::Shorty::For("IILLI"), 4'000'000, &outcome);
  CHECK(outcome.reason == exec::StopReason::Returned);
  CHECK_CTX(ret.i() == 144, "fib(12) = " + std::to_string(ret.i()));
  // Two calls must agree, and the second must not be affected by where the first left SP: the context
  // restores its state per call, so the frame descends from the same top every time.
  exec::RunResult second{};
  const auto again = rig.abi_ctx->CallGuest(fib->addr,
                                            {abi::Value32::Ptr(0), abi::Value32::Ptr(0),
                                             abi::Value32::S32(12)},
                                            abi::Shorty::For("IILLI"), 4'000'000, &second);
  CHECK_CTX(again.i() == ret.i() && second.reason == exec::StopReason::Returned,
            "second call gave " + std::to_string(again.i()));
  CHECK_CTX(rig.abi_ctx->state().r[13] == kStackTop,
            "SP left at " + Hex(rig.abi_ctx->state().r[13]) + " by a completed call");
}

/// The loop from the other side: a guest that makes a syscall during a JNI call, with the monitor behind
/// it. The value asserted is 4243 rather than 4242 because the guest adds one to whatever came back — so
/// the test proves the answer reached the guest's own arithmetic, not merely that the host noticed the
/// SVC — and the no-monitor case is first, because a refused syscall leaves the state mid-call and the
/// rest of the case wants a clean one.
void a_guest_syscall_in_a_jni_call_is_answered() {
  Rig rig = build(true);
  if (!rig.ok) return;
  const auto probe = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_syscall_add");
  if (!probe) {
    ++checks;
    fail("the fixture has no syscall_add", __LINE__);
    return;
  }
  const auto args = [&] {
    return std::vector<abi::Value32>{abi::Value32::Ptr(0), abi::Value32::Ptr(0),
                                     abi::Value32::S32(4242)};
  };

  {
    exec::RunResult out{};
    rig.abi_ctx->CallGuest(probe->addr, args(), abi::Shorty::For("IILLI"), 100000, &out);
    CHECK_CTX(out.reason == exec::StopReason::Fault,
              "without a monitor the call must not look successful: reason " +
                  std::to_string(static_cast<int>(out.reason)) + " " + out.detail);
    CHECK_CTX(out.detail.find("no syscall monitor") != std::string::npos, out.detail);
  }

  sc::Monitor mon;
  rig.abi_ctx->set_monitor(&mon);
  {
    // Still no ids: the monitor refuses, and the guest sees the refusal as a value rather than as a
    // crash — which is the difference between an unimplemented syscall and a broken emulator.
    exec::RunResult out{};
    const auto refused = rig.abi_ctx->CallGuest(probe->addr, args(), abi::Shorty::For("IILLI"), 100000,
                                                &out);
    CHECK_CTX(out.reason == exec::StopReason::Returned, out.detail);
    CHECK_CTX(refused.i() == -37, "getpid refused as -38, then the guest's +1: got " +
                                      std::to_string(refused.i()));
  }
  mon.set_ids(4242, 4243);
  {
    exec::RunResult out{};
    const auto ret = rig.abi_ctx->CallGuest(probe->addr, args(), abi::Shorty::For("IILLI"), 100000,
                                            &out);
    CHECK_CTX(out.reason == exec::StopReason::Returned, out.detail);
    CHECK_CTX(ret.i() == 4243, "the monitor's getpid never reached the guest: " +
                                   std::to_string(ret.i()));
    std::fprintf(stderr, "[abi] getpid through the monitor: guest saw %d (%s)\n", ret.i(),
                 mon.TallyText().c_str());
  }
  CHECK_CTX(mon.tally().size() == 1u && mon.TallyText() == "getpid x2", mon.TallyText());
}

/// `exit_group` ends the instance, so the bridge has to stop rather than resume: a guest that asked to
/// die and then keeps executing is the failure mode, and it is invisible to any assertion about the
/// return value because there isn't one.
void exit_group_stops_the_call() {
  Rig rig = build(true);
  if (!rig.ok) return;
  const auto probe = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_svc_probe");
  if (!probe) {
    ++checks;
    fail("the fixture has no svc_probe", __LINE__);
    return;
  }
  sc::Monitor mon;
  rig.abi_ctx->set_monitor(&mon);
  exec::RunResult out{};
  // r0 is the exit status, so the first hidden argument carries 7 into it: what a JNI caller would put
  // in `env` is here what `exit_group` reads.
  rig.abi_ctx->CallGuest(probe->addr,
                         {abi::Value32::Ptr(7), abi::Value32::Ptr(0), abi::Value32::S32(41)},
                         abi::Shorty::For("IILLI"), 100000, &out);
  CHECK_CTX(out.reason == exec::StopReason::Exited,
            "reason " + std::to_string(static_cast<int>(out.reason)) + " " + out.detail);
  CHECK_CTX(rig.abi_ctx->exit_status() == 7, std::to_string(rig.abi_ctx->exit_status()));
  CHECK_CTX(out.detail.find("exit_group") != std::string::npos, out.detail);
  // Not asserted: the state. A nested call restores the *outer* state on the way out — the caller's
  // registers are live, so the bridge puts them back — and `abi_ctx->state().r[0]` here is the outer r0,
  // not the guest's at the moment it stopped. Reaching for it was a wrong assertion rather than a wrong
  // emulator, and the two facts above (reason and status) are what the bridge actually promises.
  CHECK_CTX(mon.tally().size() == 1u && mon.TallyText() == "exit_group x1", mon.TallyText());
}

}  // namespace

int main() {
  four_or_less_stay_in_core_registers();
  the_fifth_argument_spills_at_the_lowest_stack_address();
  a_jlong_needs_an_even_odd_pair_and_will_not_take_r1_r2();
  floats_and_doubles_take_no_core_register_at_all();
  a_frame_too_small_refuses_without_half_applying_it();
  return_values_come_from_where_the_type_says();
  a_host_entry_stops_the_executor_and_resumes_at_the_link();
  a_refused_host_call_is_a_fault_naming_the_slot();
  an_unbound_slot_says_so_and_which_one_it_was();
  a_slot_range_misalignment_is_not_a_host_call();
  guest_functions_called_through_the_bridge_match_hardware();
  the_bridge_leaves_the_guest_stack_where_it_found_it();
  a_guest_syscall_in_a_jni_call_is_answered();
  exit_group_stops_the_call();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
