/*
 * RetroBug — the AArch32 signal frame and guest signal delivery (C26). SPDX-License-Identifier: Apache-2.0
 *
 * Why this file exists: Unity/Mono suspends a thread with `tkill(tid, SIGPWR)` and expects the handler it
 * installed to run. Until C26 the monitor refused the call, so Mono aborted (SIGABRT) and the game left
 * through `exit(127)` — measured on Disney Cars 3 and MadTruck, 2026-10-19. Delivering a signal to a guest
 * handler means building, on the guest's own stack, exactly the frame a kernel would have built.
 *
 * Every offset below is MEASURED, not recalled: `tests/corpus/sigframe_probe.c` installs a handler, has a
 * helper thread interrupt it with `tkill`, and finds the interrupted PC/SP inside the `ucontext` the kernel
 * handed it. `tests/corpus/sigframe-arm-layout.txt` is that measurement. `docs/research/02` §5.4 — this
 * project's own research — calls the layout "the ugly part" and asks for exactly this verification, so the
 * offsets are pinned to a device run rather than to a header someone remembered.
 *
 * What is deliberately NOT here: a real `siginfo_t`. Nothing in the eight Unity boots read one back, and the
 * project's posture is that a fact is answered with the fact or refused by name — a plausible-looking
 * fabricated `si_pid`/`si_code` would be a wrong answer where a zeroed one is an honest gap. The `si_signo`
 * and `si_errno` fields are filled because a handler that switches on them is entitled to the signal it got.
 */
#pragma once

#include <cstdint>
#include <string>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/guest_thread.h"
#include "rb/syscall/monitor.h"

namespace rb::runtime {

/// The offsets measured on SM-X710 (see tests/corpus/sigframe-arm-layout.txt). Absolute within `ucontext_t`;
/// the sigcontext begins at `kUcMcontext` and each register is `kUcMcontext + rel`.
namespace sigframe {
constexpr std::uint32_t kSizeofSiginfo = 128;
constexpr std::uint32_t kSizeofUcontext = 744;
constexpr std::uint32_t kFrameBytes = kSizeofSiginfo + kSizeofUcontext;  // 872
/// The kernel leaves 8 bytes of slack between the top of the 872-byte frame and SP; measured SP - info = 880.
constexpr std::uint32_t kFrameFromSp = 880;
constexpr std::uint32_t kStackAlign = 8;

constexpr std::uint32_t kUcFlags = 0;
constexpr std::uint32_t kUcLink = 4;
constexpr std::uint32_t kUcStackSp = 8;
constexpr std::uint32_t kUcStackFlags = 12;
constexpr std::uint32_t kUcStackSize = 16;
constexpr std::uint32_t kUcMcontext = 20;
constexpr std::uint32_t kUcSigmask = 104;

// struct sigcontext, relative to kUcMcontext.
constexpr std::uint32_t kScTrapNo = 0;
constexpr std::uint32_t kScErrorCode = 4;
constexpr std::uint32_t kScOldmask = 8;
constexpr std::uint32_t kScR0 = 12;
constexpr std::uint32_t kScR7 = 40;
constexpr std::uint32_t kScR10 = 52;
constexpr std::uint32_t kScFp = 56;   // r11
constexpr std::uint32_t kScIp = 60;   // r12
constexpr std::uint32_t kScSp = 64;
constexpr std::uint32_t kScLr = 68;
constexpr std::uint32_t kScPc = 72;
constexpr std::uint32_t kScCpsr = 76;
constexpr std::uint32_t kScFaultAddress = 80;

/// siginfo_t: the two fields delivery fills (the rest is left as the zeroed gap it is).
constexpr std::uint32_t kSiSigno = 0;
constexpr std::uint32_t kSiErrno = 4;

/// `SA_SIGINFO` (asm-generic/signal-defs.h): the handler takes three arguments, not one.
constexpr std::uint32_t kSaSiginfo = 0x00000004;
constexpr std::uint32_t kSaRestorer = 0x04000000;
constexpr std::uint32_t kSaRestart = 0x10000000;

/// The Thumb bit in CPSR (bit 5). The guest here is Thumb (`-mthumb`), and a handler entered without it
/// would be read as ARM and decode to something else entirely.
constexpr std::uint32_t kCpsrThumb = 0x20;
}  // namespace sigframe

/// A signal delivery that has been set up on a guest thread: the handler it entered and the state the
/// handler's return must restore. Filled by `DeliverSignal`, consumed by `ReturnFromSignal`, and mirrored
/// into `GuestThread::deliveries` (a stack; Mono nests a restart signal inside its suspend handler) so
/// the `rt_sigreturn` path can find the frame from the restorer's call.
struct SignalDelivery {
  bool active = false;
  int sig = 0;
  std::uint32_t handler = 0;
  std::uint32_t restorer = 0;
  bool sa_siginfo = false;
  /// The frame address; the handler's `rt_sigreturn` reads the sigcontext from here.
  std::uint32_t frame = 0;
  /// Where the interrupted code resumes, and the register file to restore.
  std::uint32_t saved[16] = {};
  std::uint32_t saved_cpsr = 0;
  std::uint64_t saved_mask = 0;
};

/// Builds the frame for `sig` on `t`'s guest stack and points the thread at `handler`.
///
/// On success `t.state` is ready to run the handler and `*out` describes what must be restored when it
/// returns through `rt_sigreturn`. Returns false with a named `why` when the frame cannot be placed (no
/// stack room, an unmapped frame page, a handler of 0) — a refusal that names itself, never a silent skip.
[[nodiscard]] bool DeliverSignal(GuestThread& t, mem::GuestMemory& mem, const syscall::Monitor::Disposition& disp,
                                 int sig, SignalDelivery* out, std::string* why);

/// Undoes the innermost delivery: restores the register file that handler interrupted, as the kernel's
/// `rt_sigreturn` does, and pops the thread's delivery stack (so an outer handler resumes if one is
/// active). Reads the sigcontext out of the frame the delivery recorded so a handler that edited it
/// changes what resumes.
[[nodiscard]] bool ReturnFromSignal(GuestThread& t, mem::GuestMemory& mem, std::string* why);

/// Takes the next signal `monitor` has queued for `t` and enters the handler the guest installed for it.
///
/// This is the ABI's slice boundary: a thread that has a signal pending enters it before it runs more
/// guest code, exactly as the kernel does on the way back to user mode. Returns false — with `why` naming
/// the case — when there is nothing queued, when the signal is blocked, when the guest installed no
/// handler (SIG_DFL, which this build does not run), when the nesting cap is reached, or when the frame
/// cannot be placed; in every one of those the thread's state is untouched, so a caller that ignores the
/// answer has not corrupted anything.
[[nodiscard]] bool DeliverPendingSignal(GuestThread& t, mem::GuestMemory& mem, syscall::Monitor& monitor,
                                        std::string* why);

}  // namespace rb::runtime
