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

/**
 * Reads the `index`-th 32-bit word of a call's argument block.
 *
 * The block starts at the callee's entry SP — that is, at `s.r[13]` after `PlaceArgs` has run — and not
 * at `s.r[13] + 16`. This helper used to add the 16, which made every spilled-argument row in this file
 * a restatement of `PlaceArgs`'s own layout instead of a check on it: six rows passed for many revisions
 * while a real softfp callee was reading `vldr d17, [sp]` and getting somebody else's bytes. The address
 * is now what `docs/30` §Step 25 measures from disassembly, and the `rb_guest_apk_tests` rows that call
 * `intThenDouble`/`twoDoubles`/`doubleIntDouble` in a guest library are the second, independent witness:
 * they do not consult this helper at all, they just ask the guest for an answer.
 */
std::uint32_t Spill(const Rig& rig, const exec::MachineState& s, unsigned index) {
  std::uint32_t v = 0;
  const exec::GuestAddr at = s.r[13] + index * 4u;
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
  /**
   * LR carries the ISA tag that `bx lr` reads, and the tag follows the *callee's* mode — which comes
   * from bit 0 of the target, the way `blx` does it. `kFn` is even, so this is an ARM call and LR is
   * untagged.
   *
   * This assertion used to read `== (kReturnAddress | 1u)` unconditionally, and that is exactly how the
   * Thumb-only bridge stayed invisible: a fixed tag looks right for the one mode it was written for, and
   * `PlaceArgs` was ignoring the target's bit 0 entirely, so no ARM-state guest function could be
   * entered. Both rows are checked below, because neither alone can fail for the right reason.
   */
  CHECK_CTX(s.r[14] == kReturnAddress, "ARM callee, LR=" + Hex(s.r[14]));
  CHECK(s.mode == isa::Mode::Arm);
  {
    exec::MachineState t = Fresh();
    const abi::Args32 none = {};
    CHECK(abi::PlaceArgs(*rig.mem, t, kFn | 1u, none, kReturnAddress,
                         abi::AbiContext::kCallFrameBytes, &err));
    CHECK(t.mode == isa::Mode::Thumb);
    CHECK_CTX(t.r[14] == (kReturnAddress | 1u), "Thumb callee, LR=" + Hex(t.r[14]));
    // The tag belongs to the pointer, not to the code: `pc` has to be the masked address or the fetch
    // starts one byte into the instruction.
    CHECK_CTX(t.pc == kFn, "tag not stripped from pc, got " + Hex(t.pc));
  }
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

/**
 * Floats and doubles are placed exactly as their integer equivalents of the same width are.
 *
 * That *is* the softfp ABI: `-mfloat-abi=softfp` says the arguments follow the **soft-float** procedure
 * call standard — which means core registers — and only the *arithmetic* uses the VFP instructions. The
 * VFP-register convention this function used to assert is the hard-float one (`-mfloat-abi=hard`), and
 * clang refuses that option outright for `armv7-none-linux-androideabi26`, so no guest this project can
 * build would ever have wanted it. See `docs/30` §Step 24, and the measurement that replaces the argument
 * from memory: `tests/guest/mathprobe.c` compiled with the project's own flags reads its parameters as
 * `vmov s4, r2`, `vmov d18, r2, r3`, `vldr d17, [sp]` and `vldr d17, [sp, #8]` — core registers, and the
 * stack at the callee's entry SP. Not one of them reads `d0..d7`.
 *
 * The `d[]` assertions below are the negative control, and they are the reason this row exists in the
 * unit suite as well as in the APK lane: `guest_apk_test.cpp` can show a *wrong answer*, which is
 * consistent with half a dozen engine bugs, while this shows the argument went to the wrong **bank**,
 * which is consistent with exactly one.
 */
void float_arguments_take_core_registers_the_way_softfp_says() {
  Rig rig = build();
  if (!rig.ok) return;
  std::string err;

  // A double and a float and an int, in that order: r0:r1, then r2, then r3. Under the hard-float
  // convention these would be d0, s0 and r0, and the integer would land in a different register
  // entirely — which is the failure this row is written to make impossible.
  exec::MachineState s = Fresh();
  s.d[0] = 0xdead'beef'cafe'babeull;  // poison: a softfp call must leave the VFP bank exactly as found
  s.d[1] = 0x1111'2222'3333'4444ull;
  CHECK(abi::PlaceArgs(*rig.mem, s, kFn,
                       {abi::Value32::F64(2.0), abi::Value32::S32(7), abi::Value32::F32(1.5f)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(s.r[0] == 0x0000'0000u && s.r[1] == 0x4000'0000u,
            "the double 2.0 should be in r0:r1, got r0=" + Hex(s.r[0]) + " r1=" + Hex(s.r[1]));
  CHECK_CTX(s.r[2] == 7, "r2=" + Hex(s.r[2]) + " — the int follows the double's pair");
  CHECK_CTX(s.r[3] == 0x3fc0'0000u, "r3=" + Hex(s.r[3]) + " — 1.5f");
  CHECK_CTX(s.d[0] == 0xdead'beef'cafe'babeull && s.d[1] == 0x1111'2222'3333'4444ull,
            "PlaceArgs wrote the VFP bank on a softfp call: d0=0x" + std::to_string(s.d[0]) +
                " d1=0x" + std::to_string(s.d[1]));

  // The pair rule, reached through a double rather than a `jlong`: one 4-byte argument first, so the
  // double cannot start at r1. r1 is consumed by nothing and the pair lands in r2:r3 — the same shape the
  // `jlong` row above asserts, and asserted again here because the two kinds are placed by the same rule
  // and a bridge that special-cases one of them is the bug this whole file exists to prevent.
  exec::MachineState after = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, after, kFn, {abi::Value32::S32(0xaa), abi::Value32::F64(2.0)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(after.r[0] == 0xaa && after.r[1] == 0 && after.r[2] == 0 && after.r[3] == 0x4000'0000u,
            "r1=" + Hex(after.r[1]) + " r2=" + Hex(after.r[2]) + " r3=" + Hex(after.r[3]));

  // Three 4-byte arguments then a double: the last core register is r3, which is odd, so the pair has
  // nowhere to go and the double becomes a stack argument — with r3 consumed by nothing rather than
  // holding the double's low half. This is `Java_..._intThenDouble` minus JNI's two hidden parameters,
  // and it is the case that `jlong` never reaches in this suite because an 8-byte integer has so far
  // always still found a free pair.
  //
  // The double is 0.1 rather than 2.0 because its low word is `0x9999999a`. With `2.0` the low word is
  // zero and `Fresh()` zeroes the registers, so "r3 holds nothing" and "r3 holds the low half" print the
  // same thing and the row cannot fail — the shape of mistake this suite now checks for by asking, for
  // every assertion, what state of the world would make it print.
  exec::MachineState skip = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, skip, kFn,
                       {abi::Value32::S32(1), abi::Value32::S32(2), abi::Value32::S32(3),
                        abi::Value32::F64(0.1)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(skip.r[3] == 0u, "r3 should be consumed by nothing, got " + Hex(skip.r[3]));
  CHECK_CTX(Spill(rig, skip, 0) == 0x9999'999au && Spill(rig, skip, 1) == 0x3fb9'9999u,
            "0.1 should be the first stack argument, got " + Hex(Spill(rig, skip, 0)) + " " +
                Hex(Spill(rig, skip, 1)));

  // The core bank full with doubles: 1.0 in r0:r1, 3.0 in r2:r3, 5.0 first in the block. The block starts
  // at the callee's SP, which is 8-byte aligned, so a double that arrives first needs no pad.
  exec::MachineState spill = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, spill, kFn,
                       {abi::Value32::F64(1.0), abi::Value32::F64(3.0), abi::Value32::F64(5.0)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(spill.r[0] == 0x0000'0000u && spill.r[1] == 0x3ff0'0000u,
            "1.0 should be in r0:r1, got " + Hex(spill.r[0]) + ":" + Hex(spill.r[1]));
  CHECK_CTX(spill.r[2] == 0x0000'0000u && spill.r[3] == 0x4008'0000u,
            "3.0 should be in r2:r3, got " + Hex(spill.r[2]) + ":" + Hex(spill.r[3]));
  CHECK_CTX(Spill(rig, spill, 0) == 0x0000'0000u && Spill(rig, spill, 1) == 0x4014'0000u,
            "5.0 should be the first stack argument at SP+0, got " + Hex(Spill(rig, spill, 0)) + " " +
                Hex(Spill(rig, spill, 1)));

  /**
   * The alignment pad — `Java_..._doubleIntDouble`'s shape.
   *
   * 1.0 and 2.0 fill the core bank, so `0x55` becomes the block's first word at SP+0 and the third double
   * arrives when the block is 4 bytes deep. An 8-byte stack argument keeps its own alignment, so it lands
   * at SP+8 and SP+4 is padding; a bridge that packs the block tightly puts it at SP+4, which is the one
   * difference none of the rows above can see.
   */
  exec::MachineState pad = Fresh();
  CHECK(abi::PlaceArgs(*rig.mem, pad, kFn,
                       {abi::Value32::F64(1.0), abi::Value32::F64(2.0), abi::Value32::S32(0x55),
                        abi::Value32::F64(3.0)},
                       kReturnAddress, abi::AbiContext::kCallFrameBytes, &err));
  CHECK_CTX(Spill(rig, pad, 0) == 0x55u, "the int should be at SP+0, got " + Hex(Spill(rig, pad, 0)));
  CHECK_CTX(Spill(rig, pad, 2) == 0x0000'0000u && Spill(rig, pad, 3) == 0x4008'0000u,
            "3.0 should be at SP+8, got " + Hex(Spill(rig, pad, 2)) + " " + Hex(Spill(rig, pad, 3)));
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
  /**
   * `F` and `D` read **core registers**, because the guest ABI is softfp — `AbiContext`'s header carries
   * the three independent checks that settled it, and `docs/30` §Step 24 records what believing
   * otherwise cost.
   *
   * The fixture above is deliberately built so this cannot pass by accident: `d[0]` holds `0x33333333`
   * while r0/r1 hold `0x11111111`/`0x22222222`. Asserting the softfp values therefore *fails* if anyone
   * restores the hard-float read, which is what the previous version of these two lines asserted — and
   * it passed for as long as nothing called a float-returning JNI method.
   */
  CHECK(abi::ReturnValue(s, abi::Shorty::For("FI")).bits == 0x1111'1111ull);            // r0
  CHECK(abi::ReturnValue(s, abi::Shorty::For("DL")).bits == 0x2222'2222'1111'1111ull);  // r0:r1, not d0
  // Named so the negative control is explicit: this is what the hard-float read would have returned.
  CHECK(abi::ReturnValue(s, abi::Shorty::For("DL")).bits != s.d[0]);

  // 'Z' is an unsigned *byte*: a callee that left a stale upper byte in r0 must not read back as true
  // through it, and must not read back as 0x11 either.
  exec::MachineState z = Fresh();
  z.r[0] = 0x0000'0100u;
  CHECK(abi::ReturnValue(z, abi::Shorty::For("ZL")).w() == 0u);
  z.r[0] = 0x0000'0001u;
  CHECK(abi::ReturnValue(z, abi::Shorty::For("ZL")).w() == 1u);

  // 'B' and 'S' are *signed*: AAPCS guarantees only the low 8/16 bits, so the value must be masked
  // and then sign-extended. The bare mask a first draft used turned `(byte) -1` into 255 and
  // `(short) -2` into 65534 — plausible numbers, which is what made them dangerous. Both shapes of
  // callee garbage are covered: the sign-extended form a callee leaves, and a zeroed upper half.
  exec::MachineState b = Fresh();
  b.r[0] = 0xffff'ffffu;
  CHECK(abi::ReturnValue(b, abi::Shorty::For("BL")).i() == -1);
  CHECK(abi::ReturnValue(b, abi::Shorty::For("BL")).w() == 0xffff'ffffu);
  b.r[0] = 0x0000'00ffu;
  CHECK(abi::ReturnValue(b, abi::Shorty::For("BL")).i() == -1);
  b.r[0] = 0x0000'007fu;
  CHECK(abi::ReturnValue(b, abi::Shorty::For("BL")).i() == 127);
  exec::MachineState sh = Fresh();
  sh.r[0] = 0xffff'fffeu;
  CHECK(abi::ReturnValue(sh, abi::Shorty::For("SL")).i() == -2);
  sh.r[0] = 0x0000'fffeu;
  CHECK(abi::ReturnValue(sh, abi::Shorty::For("SL")).i() == -2);
  sh.r[0] = 0x0000'7fffu;
  CHECK(abi::ReturnValue(sh, abi::Shorty::For("SL")).i() == 0x7fff);
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

/// The Linux ARM kuser helpers (fixed addresses the kernel answers), which Angry Birds Go's constructors
/// branch to: a fetch at 0xffff0fa0 faulted the 248th static initialiser before this. Each is called
/// directly, as a guest's `blx` to the constant would reach it, and each row can fail: a cmpxchg that
/// always stores, never stores, or reports the wrong verdict; a get_tls that returns anything but TP.
void the_kuser_helpers_answer_like_the_kernel() {
  Rig rig = build();
  if (!rig.ok) return;
  using A = abi::AbiContext;
  const exec::GuestAddr word = kStackTop - 0x4000u;  // inside the mapped stack, far below any frame
  const std::uint32_t five = 5;
  CHECK(rig.mem->Write(word, &five, 4));
  exec::RunResult run{};
  const abi::Shorty iiil = abi::Shorty::For("IIIL");

  auto ret = rig.abi_ctx->CallGuest(A::kKuserCmpxchg, {abi::Value32::S32(5), abi::Value32::S32(9),
                                                       abi::Value32::Ptr(word)}, iiil, 1000, &run);
  std::uint32_t now = 0;
  CHECK(rig.mem->Read(word, &now, 4));
  CHECK_CTX(run.reason == exec::StopReason::Returned && ret.w() == 0 && now == 9,
            "cmpxchg(5->9) on 5: " + run.detail + " r0=" + Hex(ret.w()) + " word=" + Hex(now));
  ret = rig.abi_ctx->CallGuest(A::kKuserCmpxchg, {abi::Value32::S32(5), abi::Value32::S32(7),
                                                  abi::Value32::Ptr(word)}, iiil, 1000, &run);
  CHECK(rig.mem->Read(word, &now, 4));
  CHECK_CTX(run.reason == exec::StopReason::Returned && ret.w() != 0 && now == 9,
            "cmpxchg(5->7) on 9 must fail and leave it: r0=" + Hex(ret.w()) + " word=" + Hex(now));

  const std::uint64_t cur = 0x1111'2222'3333'4444ull, newer = 0xaaaa'bbbb'cccc'ddddull;
  CHECK(rig.mem->Write(word + 8, &cur, 8) && rig.mem->Write(word + 16, &cur, 8) && rig.mem->Write(word + 24, &newer, 8));
  ret = rig.abi_ctx->CallGuest(A::kKuserCmpxchg64, {abi::Value32::Ptr(word + 16), abi::Value32::Ptr(word + 24),
                                                    abi::Value32::Ptr(word + 8)}, abi::Shorty::For("ILLL"), 1000, &run);
  std::uint64_t got64 = 0;
  CHECK(rig.mem->Read(word + 8, &got64, 8));
  CHECK_CTX(run.reason == exec::StopReason::Returned && ret.w() == 0 && got64 == newer,
            "cmpxchg64: " + run.detail + " r0=" + Hex(ret.w()));

  ret = rig.abi_ctx->CallGuest(A::kKuserMemoryBarrier, {abi::Value32::S32(0x77)}, abi::Shorty::For("II"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned && ret.w() == 0x77, "barrier: " + run.detail);

  rig.abi_ctx->state().tp = 0x0040'0000u;
  ret = rig.abi_ctx->CallGuest(A::kKuserGetTls, {}, abi::Shorty::For("I"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned && ret.w() == 0x0040'0000u,
            "get_tls returned " + Hex(ret.w()) + ": " + run.detail);

  std::uint32_t slot = 0;
  CHECK(!rig.abi_ctx->IsHostEntry(A::kKuserCmpxchg + 4, &slot));  ///< exact addresses only
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
        where->tagged(),
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
  const auto ret = rig.abi_ctx->CallGuest(fib->tagged(),
                                          {abi::Value32::Ptr(0), abi::Value32::Ptr(0),
                                           abi::Value32::S32(12)},
                                          abi::Shorty::For("IILLI"), 4'000'000, &outcome);
  CHECK(outcome.reason == exec::StopReason::Returned);
  CHECK_CTX(ret.i() == 144, "fib(12) = " + std::to_string(ret.i()));
  // Two calls must agree, and the second must not be affected by where the first left SP: the context
  // restores its state per call, so the frame descends from the same top every time.
  exec::RunResult second{};
  const auto again = rig.abi_ctx->CallGuest(fib->tagged(),
                                            {abi::Value32::Ptr(0), abi::Value32::Ptr(0),
                                             abi::Value32::S32(12)},
                                            abi::Shorty::For("IILLI"), 4'000'000, &second);
  CHECK_CTX(again.i() == ret.i() && second.reason == exec::StopReason::Returned,
            "second call gave " + std::to_string(again.i()));
  CHECK_CTX(rig.abi_ctx->state().r[13] == kStackTop,
            "SP left at " + Hex(rig.abi_ctx->state().r[13]) + " by a completed call");
}

/// A call run to completion is re-entered at each turn's end instead of being cut off by the budget: with a
/// 1000-step turn, fib(12) (which needs far more) still returns 144 and leaves SP where it found it, while
/// the same budget on a plain call stops it.
void a_call_to_completion_is_re_entered_across_turns() {
  Rig rig = build(true);
  if (!rig.ok) return;
  const auto fib = rig.mod->FindExport("Java_com_retrobug_fixture_MainActivity_fib");
  if (!fib) return;
  const abi::Args32 args{abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(12)};
  exec::RunResult cut{};
  rig.abi_ctx->CallGuest(fib->tagged(), args, abi::Shorty::For("IILLI"), 1000, &cut);
  CHECK_CTX(cut.reason == exec::StopReason::Budget, std::string{"a 1000-step plain call stopped: "} + cut.detail);
  exec::RunResult outcome{};
  const auto ret = rig.abi_ctx->CallGuestToCompletion(fib->tagged(), args, abi::Shorty::For("IILLI"), 1000, &outcome);
  CHECK_CTX(outcome.reason == exec::StopReason::Returned, outcome.detail);
  CHECK_CTX(ret.i() == 144, "fib(12) = " + std::to_string(ret.i()));
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
    rig.abi_ctx->CallGuest(probe->tagged(), args(), abi::Shorty::For("IILLI"), 100000, &out);
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
    const auto refused = rig.abi_ctx->CallGuest(probe->tagged(), args(), abi::Shorty::For("IILLI"), 100000,
                                                &out);
    CHECK_CTX(out.reason == exec::StopReason::Returned, out.detail);
    CHECK_CTX(refused.i() == -37, "getpid refused as -38, then the guest's +1: got " +
                                      std::to_string(refused.i()));
  }
  mon.set_ids(4242, 4243);
  {
    exec::RunResult out{};
    const auto ret = rig.abi_ctx->CallGuest(probe->tagged(), args(), abi::Shorty::For("IILLI"), 100000,
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
  rig.abi_ctx->CallGuest(probe->tagged(),
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
  float_arguments_take_core_registers_the_way_softfp_says();
  a_frame_too_small_refuses_without_half_applying_it();
  return_values_come_from_where_the_type_says();
  a_host_entry_stops_the_executor_and_resumes_at_the_link();
  a_refused_host_call_is_a_fault_naming_the_slot();
  an_unbound_slot_says_so_and_which_one_it_was();
  a_slot_range_misalignment_is_not_a_host_call();
  the_kuser_helpers_answer_like_the_kernel();
  guest_functions_called_through_the_bridge_match_hardware();
  the_bridge_leaves_the_guest_stack_where_it_found_it();
  a_call_to_completion_is_re_entered_across_turns();
  a_guest_syscall_in_a_jni_call_is_answered();
  exit_group_stops_the_call();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
