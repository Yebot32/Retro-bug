// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Tier I throughput, fixture-free: the loop is hand-assembled into a guest page here, so the host lanes
// can print the interpreter's own rate without the tablet's `librb_fixture.so` (which is what keeps
// `rb_exec_interp_tests` device-only). The number is a report, never an assertion — CI runners share
// hardware — but the *result* is checked: if the guest does not leave its counter at zero and its
// accumulator at the trip count, this exits non-zero and prints the register state that disagrees.
//
// Two details decide whether the measurement means anything:
//
//   - The loop page is read-only after the bytes are written. `InterpExecutor` trusts a decoded block
//     only while its page cannot be written (`interp.h`), and guest text is read+exec in a real process,
//     so a writable page here would time re-translation on every trip instead of the cached path the
//     launcher takes. The interpreter decodes by reading, so read-only is enough; no `kExec` is needed
//     (and on macOS an out-of-line anonymous block refuses `PROT_EXEC` anyway).
//   - The counter is seeded from `MachineState` rather than by `movw`/`movt`, keeping the measured loop
//     pure 16-bit ALU/flag work and its bytes the same length the assembler prints.
//
// The encodings were produced by the NDK clang from the mnemonics (see the comments beside the bytes);
// objdump prints the low halfword first, each halfword little-endian in memory.
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "rb/exec/interp.h"
#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace {

using namespace rb;

constexpr std::uint32_t kCode = 0x0100'0000;
constexpr std::uint32_t kStackTop = 0x00ff'f000;
constexpr std::uint32_t kReturn = 0x0fff'fff0;  // never mapped: the executor's sentinel return address

/// Loop trips. Long enough for a steady rate, short enough that a loop which fails to terminate reaches
/// the budget in seconds instead of minutes.
constexpr unsigned kTrips = 500'000;

/// IR steps, not guest instructions: the budget is checked between blocks, and a block can overshoot by
/// its own length. A correct trip is roughly 60 IR steps, so the whole loop is tens of millions and this
/// leaves ample headroom while still bounding a runaway loop.
constexpr unsigned kBudget = 200'000'000;

const char* ReasonName(exec::StopReason r) {
  switch (r) {
    case exec::StopReason::Budget: return "Budget";
    case exec::StopReason::Returned: return "Returned";
    case exec::StopReason::HostCall: return "HostCall";
    case exec::StopReason::Syscall: return "Syscall";
    case exec::StopReason::Suspended: return "Suspended";
    case exec::StopReason::Unimplemented: return "Unimplemented";
    case exec::StopReason::Fault: return "Fault";
    case exec::StopReason::Internal: return "Internal";
    case exec::StopReason::Exited: return "Exited";
  }
  return "?";
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  if (ps > 0) {
    while ((static_cast<long>(1) << shift) < ps) ++shift;
  }
  return shift;
}

int fail(const char* what, const std::string& detail = {}) {
  std::fprintf(stderr, "FAIL  %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
  return 1;
}

std::string Hex32(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "0x%08x", v);
  return buf;
}

}  // namespace

int main() {
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  const std::unique_ptr<mem::GuestMemory> mem = mem::GuestMemory::Create(32u << 20, model, &err);
  if (!mem) return fail("guest window creation failed", err);
  if (!mem->Map(kCode, mem::kGuestPage, mem::kRead | mem::kWrite, &err) ||
      !mem->Map(kStackTop - 0x4000, 0x4000, mem::kRead | mem::kWrite, &err)) {
    return fail("could not map the code page or stack", err);
  }

  // loop: adds r1, r1, r2 ; eors r3, r3, r5 ; ands r4, r4, r3 ; orrs r6, r6, r1 ; lsls r7, r7, #1
  //       cmp r7, #0 ; mvns r3, r3 ; adds r6, r6, r2 ; subs r4, r4, r2 ; subs r0, #1 ; bne loop
  //       bx lr   — r0 arrives from MachineState below.
  const std::uint8_t program[] = {
      0x01, 0x22,              // 0x2201        movs r2, #1
      0x5a, 0x25,              // 0x255a        movs r5, #0x5a
      // loop: (offset 4; the bne target)
      0x89, 0x18,              // 0x1889        adds r1, r1, r2
      0x6b, 0x40,              // 0x406b        eors r3, r5
      0x1c, 0x40,              // 0x401c        ands r4, r3
      0x0e, 0x43,              // 0x430e        orrs r6, r1
      0x7f, 0x00,              // 0x007f        lsls r7, r7, #1
      0x00, 0x2f,              // 0x2f00        cmp r7, #0
      0xdb, 0x43,              // 0x43db        mvns r3, r3
      0xb6, 0x18,              // 0x18b6        adds r6, r6, r2
      0xa4, 0x1a,              // 0x1aa4        subs r4, r4, r2
      0x01, 0x38,              // 0x3801        subs r0, #1
      0xf4, 0xd1,              // 0xd1f4        bne loop (target 4 - (pc 0x18 + 4) = -0x18, imm8 = -12)
      0x70, 0x47,              // 0x4770        bx lr
  };
  if (!mem->Write(kCode, program, sizeof program)) return fail("could not write the program");
  // Read-only from here: the block cache trusts a decoded block only while its page cannot be written,
  // and the loop must run from the cache to measure interpretation rather than re-translation.
  if (!mem->Protect(kCode, mem::kGuestPage, mem::kRead, &err)) {
    return fail("could not protect the code page read-only", err);
  }

  exec::InterpExecutor exec(*mem, kReturn);
  exec::MachineState state{};
  state.mode = isa::Mode::Thumb;
  state.pc = kCode;
  state.r[15] = kCode;
  state.r[13] = kStackTop;
  state.r[14] = kReturn;
  state.r[0] = kTrips;

  const auto start = std::chrono::steady_clock::now();
  const exec::RunResult rr = exec.Run(state, kBudget);
  const auto end = std::chrono::steady_clock::now();

  if (rr.reason != exec::StopReason::Returned) {
    return fail("the loop did not reach bx lr",
                std::string{"reason="} + ReasonName(rr.reason) + " pc=" + Hex32(state.pc) +
                    " r0=" + Hex32(state.r[0]) + " r1=" + Hex32(state.r[1]) +
                    " steps=" + std::to_string(rr.instructions_executed) +
                    (rr.detail.empty() ? "" : " detail=" + rr.detail));
  }
  if (state.r[0] != 0 || state.r[1] != kTrips) {
    return fail("the loop ran a wrong number of trips",
                "r0=" + Hex32(state.r[0]) + " (want 0), r1=" + Hex32(state.r[1]) +
                    " (want " + Hex32(kTrips) + "), steps=" + std::to_string(rr.instructions_executed));
  }

  const double ms = std::chrono::duration<double, std::milli>(end - start).count();
  // steps / ms / 1000 == steps / seconds / 1e6, i.e. millions of IR steps per second.
  const double rate = ms > 0.0 ? static_cast<double>(rr.instructions_executed) / ms / 1000.0 : 0.0;
  std::printf("tier1: %u IR steps in %.1f ms (%.2f M IR-steps/s)\n", rr.instructions_executed, ms, rate);
  return 0;
}
