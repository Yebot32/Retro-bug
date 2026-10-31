// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Tier I NEON semantics the guest's own libc depends on, plus the flag/branch/memory smoke program the
// interpreter's dispatch must keep green. The fixture-based interpreter suite needs the tablet's
// `librb_fixture.so`; these cases need no fixture — the instructions are assembled into a guest page
// here, so the same test runs on every host lane and pins the semantics a real bionic routine
// (jemalloc's extent hash in `extent_lock2`) reaches on every allocation.
//
// The encodings were produced by clang from the mnemonic (see the comments), and the expected lane values
// come from ARM's definition of the instruction, not from a previous run of this interpreter.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "rb/exec/interp.h"
#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace {

using namespace rb;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                   \
  do {                                            \
    ++checks;                                     \
    if (!(cond)) fail(#cond, __LINE__, (detail)); \
  } while (0)

constexpr std::uint32_t kCode = 0x0100'0000;
constexpr std::uint32_t kData = 0x0180'0000;  // a writable page the flag/memory case stores through
constexpr std::uint32_t kStackTop = 0x00ff'f000;
constexpr std::uint32_t kReturn = 0x0fff'fff0;  // never mapped: the executor's sentinel return address

struct Rig {
  std::unique_ptr<mem::GuestMemory> mem;
  std::unique_ptr<exec::InterpExecutor> exec;
  bool ok = false;
};

std::string Hex64(std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
  return buf;
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  if (ps > 0) {
    while ((static_cast<long>(1) << shift) < ps) ++shift;
  }
  return shift;
}

Rig build() {
  Rig rig;
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = mem::GuestMemory::Create(32u << 20, model, &err);
  if (!rig.mem) {
    fail("guest window creation failed", __LINE__, err);
    return rig;
  }
  // Read+write, not exec: the interpreter decodes by reading the bytes, and macOS refuses PROT_EXEC on an
  // out-of-line anonymous block (a page the window could not serve in place on a 16 KiB granule).
  if (!rig.mem->Map(kCode, mem::kGuestPage, mem::kRead | mem::kWrite, &err) ||
      !rig.mem->Map(kData, mem::kGuestPage, mem::kRead | mem::kWrite, &err) ||
      !rig.mem->Map(kStackTop - 0x4000, 0x4000, mem::kRead | mem::kWrite, &err)) {
    fail("could not map the code page, data page or stack", __LINE__, err);
    return rig;
  }
  rig.exec = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturn);
  rig.ok = true;
  return rig;
}

/// Runs `program` from the code page with the caller's register state — including the D registers the case
/// prepared — and returns the outcome.
exec::RunResult Run(Rig& rig, const std::uint8_t* program, std::size_t len, exec::MachineState& state) {
  if (!rig.mem->Write(kCode, program, len)) {
    fail("could not write the program", __LINE__);
    return exec::RunResult{};
  }
  state.mode = isa::Mode::Thumb;
  state.pc = kCode;
  state.r[15] = kCode;
  state.r[13] = kStackTop;
  state.r[14] = kReturn;
  return rig.exec->Run(state, 1000);
}

/// `vshl.i32 d16, d2, #0x1` — clang assembles it to 0xefe10512, the instruction at libc.so+0x5ee72 in the
/// iPad's own bionic (jemalloc's extent hash, on every allocation). Tier I stopped with
/// "VectorLogicalShiftLeft32: no Tier I semantics yet" and KitKat's first post-load malloc died with it.
void vshl_i32_shifts_each_lane_left() {
  Rig rig = build();
  if (!rig.ok) return;
  // vshl.i32 d16, d2, #1 ; bx lr
  const std::uint8_t program[] = {0xe1, 0xef, 0x12, 0x05, 0x70, 0x47};
  exec::MachineState in{};
  in.d[2] = 0x8000'0001'0000'0001ull;
  const exec::RunResult rr = Run(rig, program, sizeof program, in);
  CHECK_CTX(rr.reason == exec::StopReason::Returned, rr.detail);
  CHECK_CTX(in.d[16] == 0x0000'0002'0000'0002ull,
            "d16 = " + Hex64(in.d[16]) + ": each 32-bit lane shifted left by one, the top bit dropped");
}

/// The shift count is a constant in the encoding: `vshl.i32 d16, d2, #0x1f` (clang: 0xefff0512) keeps only
/// bit 0 of each lane, in bit 31.
void vshl_i32_shift_amount_masks_the_lane() {
  Rig rig = build();
  if (!rig.ok) return;
  const std::uint8_t program[] = {0xff, 0xef, 0x12, 0x05, 0x70, 0x47};
  exec::MachineState in{};
  in.d[2] = 0x0000'0003'0000'0001ull;
  const exec::RunResult rr = Run(rig, program, sizeof program, in);
  CHECK_CTX(rr.reason == exec::StopReason::Returned, rr.detail);
  CHECK_CTX(in.d[16] == 0x8000'0000'8000'0000ull, "d16 = " + Hex64(in.d[16]) + ": each lane's bit 0 to bit 31");
}

/// The 16-bit form over a Q register: `vshl.i16 q3, q1, #0x3` (clang: 0xef936552) shifts all eight lanes,
/// which is where a lane-width mix-up between the D halves shows up.
void vshl_i16_shifts_both_halves_of_the_quad() {
  Rig rig = build();
  if (!rig.ok) return;
  const std::uint8_t program[] = {0x93, 0xef, 0x52, 0x65, 0x70, 0x47};
  exec::MachineState in{};
  in.d[2] = 0x0001'0002'0003'0004ull;  // lanes, low first: 0x0004 0x0003 0x0002 0x0001
  in.d[3] = 0x8001'8002'8003'8004ull;  // top bits fall out of the lane
  const exec::RunResult rr = Run(rig, program, sizeof program, in);
  CHECK_CTX(rr.reason == exec::StopReason::Returned, rr.detail);
  CHECK_CTX(in.d[6] == 0x0008'0010'0018'0020ull, "d6 = " + Hex64(in.d[6]) + ": q3's low half");
  CHECK_CTX(in.d[7] == 0x0008'0010'0018'0020ull, "d7 = " + Hex64(in.d[7]) + ": q3's high half");
}

/// Regression coverage for the interpreter's just-landed dispatch optimization, on the paths it touches:
/// `cmp` must publish the flags its own subtraction produced, `bne` must read those flags, and the
/// store/load pair must reach the guest data page. The failure leg returns through the same `bx lr` with
/// r0 = 0, so a wrong condition shows up as a wrong final register rather than a different stop reason;
/// the register *and* the stored word are asserted, so a load that never occurs cannot hide behind a
/// correct-looking sum.
void conditions_and_memory_round_trip() {
  Rig rig = build();
  if (!rig.ok) return;
  // movs r1, #5 ; str r1, [r2] ; ldr r3, [r2] ; cmp r3, #5 ; bne fail ; add r0, r1, r3 ; bx lr
  // fail: movs r0, #0 ; bx lr
  const std::uint8_t program[] = {
      0x05, 0x21,              // 0x2105        movs r1, #5
      0x11, 0x60,              // 0x6011        str r1, [r2]
      0x13, 0x68,              // 0x6813        ldr r3, [r2]
      0x05, 0x2b,              // 0x2b05        cmp r3, #5
      0x02, 0xd1,              // 0xd102        bne fail (target 0x10 - (pc 0x8 + 4) = 4, imm8 = 2)
      0x01, 0xeb, 0x03, 0x00,  // 0xeb01 0x0003 add.w r0, r1, r3
      0x70, 0x47,              // 0x4770        bx lr
      // fail:
      0x00, 0x20,              // 0x2000        movs r0, #0
      0x70, 0x47,              // 0x4770        bx lr
  };
  exec::MachineState in{};
  in.r[2] = kData;
  const exec::RunResult rr = Run(rig, program, sizeof program, in);
  CHECK_CTX(rr.reason == exec::StopReason::Returned, rr.detail);
  CHECK_CTX(in.r[0] == 10, "r0 = " + Hex64(in.r[0]) + ": the taken path's r1 + r3, not the fail leg's 0");
  CHECK_CTX(in.r[3] == 5, "r3 = " + Hex64(in.r[3]) + ": the word loaded back from the data page");
  std::uint32_t stored = 0;
  CHECK_CTX(rig.mem->Read(kData, &stored, sizeof stored) && stored == 5,
            "data[" + Hex64(kData) + "] = " + Hex64(stored) + ": the word str wrote");
}

}  // namespace

int main() {
  vshl_i32_shifts_each_lane_left();
  vshl_i32_shift_amount_masks_the_lane();
  vshl_i16_shifts_both_halves_of_the_quad();
  conditions_and_memory_round_trip();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
