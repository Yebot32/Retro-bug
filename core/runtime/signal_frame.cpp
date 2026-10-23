/*
 * RetroBug — the AArch32 signal frame and guest signal delivery (C26). SPDX-License-Identifier: Apache-2.0
 *
 * The offsets are the measured ones (see the header and tests/corpus/sigframe-arm-layout.txt). This file is
 * the only place a frame is built, so the layout lives in exactly one place.
 */
#include "rb/runtime/signal_frame.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rb::runtime {
namespace {

namespace sf = sigframe;

std::string Hex(std::uint32_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "0x%x", static_cast<unsigned>(v));
  return buf;
}

std::string Hex64(std::uint64_t v) {
  char buf[23];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

/// Writes a 32-bit word into the frame, reporting the failing offset rather than silently dropping it. A
/// frame that is half-written must not be entered: the handler would read a stack pointer of zero.
bool Put(mem::GuestMemory& mem, std::uint32_t at, const void* src, std::size_t len, std::uint32_t base,
         std::string* why) {
  if (mem.Write(at, src, len)) return true;
  *why = "the signal frame is not writable at " + Hex(at) + " (+" + std::to_string(at - base) +
         " into the frame): the guest stack page is not mapped or not writable";
  return false;
}

}  // namespace

bool DeliverSignal(GuestThread& t, mem::GuestMemory& mem, const syscall::Monitor::Disposition& disp, int sig,
                   SignalDelivery* out, std::string* why, int fault_code, std::uint32_t fault_addr) {
  if (disp.handler <= 1) {
    *why = "signal " + std::to_string(sig) + " has no handler installed (the disposition is " +
           (disp.handler == 0 ? "SIG_DFL" : "SIG_IGN") + "), so there is nothing to enter";
    return false;
  }

  // The frame sits below the current stack pointer, as the kernel places it, 8-byte aligned. The guest's
  // stack grows down and the handler may use everything above the frame, so the interrupted SP is where the
  // handler's own frame starts from.
  const std::uint32_t interrupted_pc = t.state.pc;
  const std::uint32_t interrupted_sp = t.state.r[13];
  const std::uint32_t interrupted_r4 = t.state.r[4];
  const std::uint32_t interrupted_fpscr = t.state.fpscr;
  std::uint64_t interrupted_d[32];
  for (unsigned i = 0; i < 32; ++i) interrupted_d[i] = t.state.d[i];
  const std::uint32_t sp = t.state.r[13] & ~(sf::kStackAlign - 1u);
  if (sp == 0) {
    *why = "signal " + std::to_string(sig) + ": the thread's stack pointer is 0, so no frame can be built";
    return false;
  }
  const std::uint32_t frame = (sp - sf::kFrameFromSp) & ~(sf::kStackAlign - 1u);
  // The info block precedes the ucontext in the kernel's layout, and the handler is handed both.
  const std::uint32_t info = frame;

  // Every page the frame touches has to be mapped and writable before a word is written: a partial frame is
  // worse than a refusal, because the handler would run against a stack pointer the guest never had.
  // Start at the page *containing* the frame, not at the frame address itself: the frame is 8-byte aligned,
  // not page-aligned, and stepping by a page from an unaligned base would check the wrong page after a
  // boundary. The frame can end in a page the old loop never looked at.
  const std::uint32_t first_page = frame & ~(mem::kGuestPage - 1u);
  for (std::uint32_t at = first_page; at < frame + sf::kFrameBytes; at += mem::kGuestPage) {
    if (!mem.IsMapped(at) || (mem.ProtAt(at) & mem::kWrite) == 0) {
      *why = "signal " + std::to_string(sig) + ": the frame page at " + Hex(at) +
             " is not writable guest memory (the stack has no room for the " +
             std::to_string(sf::kFrameBytes) + "-byte frame)";
      return false;
    }
  }

  // Zero the whole frame first, so every field this build does not fill is a zero the guest can recognise
  // as "not filled" rather than stale bytes from an earlier call. The last chunk is shortened to the frame:
  // a 64-byte chunk at offset 832 used to write 24 bytes past the 872-byte frame, and the 16 at SP..SP+16
  // are the *interrupted function's saved registers* (the generic `syscall()` wrapper keeps r4..r7 there).
  // Measured on Cars 3 (SM-X710 2026-10-20): the pop loaded r4=0, and bionic's futex error path read
  // through the null errno pointer at libc+0x78468.
  std::string werr;
  for (std::uint32_t off = 0; off < sf::kFrameBytes; off += 64) {
    const std::uint32_t zeros[16] = {};
    const std::uint32_t n = std::min<std::uint32_t>(sizeof zeros, sf::kFrameBytes - off);
    if (!Put(mem, frame + off, zeros, n, frame, &werr)) {
      *why = "signal " + std::to_string(sig) + ": " + werr;
      return false;
    }
  }

  const std::uint32_t uc = info + sf::kSizeofSiginfo;
  const auto Put32 = [&](std::uint32_t at, std::uint32_t v) {
    return Put(mem, at, &v, 4, frame, &werr);
  };

  // ---- siginfo_t. The signal it got and a zero errno are always filled. A synchronous fault also fills
  // si_code and si_addr: a handler that has to tell its own fault from a sent signal reads them, and Mono's
  // SIGSEGV handler throws NullReferenceException from exactly that decision. Nothing invents si_pid.
  if (!Put32(info + sf::kSiSigno, static_cast<std::uint32_t>(sig)) ||
      !Put32(info + sf::kSiErrno, 0) ||
      (fault_code != 0 && (!Put32(info + sf::kSiCode, static_cast<std::uint32_t>(fault_code)) ||
                           !Put32(info + sf::kSiAddr, fault_addr)))) {
    *why = "signal " + std::to_string(sig) + ": " + werr;
    return false;
  }

  // ---- ucontext_t: the interrupted register file, then the mask the handler runs with.
  const auto PutSc = [&](std::uint32_t rel, std::uint32_t v) {
    return Put32(uc + sf::kUcMcontext + rel, v);
  };
  if (!Put32(uc + sf::kUcFlags, 0) || !Put32(uc + sf::kUcLink, 0) ||
      !Put32(uc + sf::kUcStackSp, 0) || !Put32(uc + sf::kUcStackFlags, 0) ||
      !Put32(uc + sf::kUcStackSize, 0)) {
    *why = "signal " + std::to_string(sig) + ": " + werr;
    return false;
  }
  // The interrupted registers, r0..r12 then sp/lr/pc/cpsr. r0..r12 are copied exactly, because a handler
  // that inspects them (Mono's does: it reads the interrupted PC to decide where the thread was) must see
  // what the interrupted code had.
  bool ok = true;
  for (std::uint32_t i = 0; i < 13 && ok; ++i) {
    ok = PutSc(sf::kScR0 + i * 4, t.state.r[i]);
  }
  ok = ok && PutSc(sf::kScSp, t.state.r[13]) && PutSc(sf::kScLr, t.state.r[14]) &&
       PutSc(sf::kScPc, t.state.pc) && PutSc(sf::kScCpsr, t.state.cpsr) &&
       PutSc(sf::kScFaultAddress, fault_code != 0 ? fault_addr : 0);
  if (!ok) {
    *why = "signal " + std::to_string(sig) + ": " + werr;
    return false;
  }
  // The blocked mask the handler runs under: the guest's current mask, with this signal added unless it
  // asked for SA_NODEFER. Only the low 32 bits are written, which is the whole of bionic's arm32 sigset_t.
  const std::uint64_t saved_mask = t.signal_mask;
  // The handler's mask is the interrupted mask | the sigaction's sa_mask | this signal (unless SA_NODEFER,
  // which leaves only the signal itself unblocked). `disp.mask` was recorded and previously unused.
  std::uint64_t handler_mask = saved_mask | disp.mask;
  if ((disp.flags & 0x40000000u /* SA_NODEFER */) == 0 && sig >= 1 && sig <= 64) {
    handler_mask |= (1ull << (sig - 1));
  }
  // `uc_sigmask` is the mask the INTERRUPTED context had, not the one the handler runs under: the kernel
  // saves the old mask here and `rt_sigreturn` puts it back. Writing the handler's mask here would leave
  // the signal blocked forever after the first delivery.
  const std::uint32_t mask_lo = static_cast<std::uint32_t>(saved_mask);
  const std::uint32_t mask_hi = static_cast<std::uint32_t>(saved_mask >> 32);
  if (!Put32(uc + sf::kUcSigmask, mask_lo) || !Put32(uc + sf::kUcSigmask + 4, mask_hi)) {
    *why = "signal " + std::to_string(sig) + ": " + werr;
    return false;
  }

  // ---- what the handler's return has to restore, and the entry itself. Captured *before* the thread's
  // live state is rewritten below, so the record is the interrupted state and not the handler's.
  if (out != nullptr) {
    out->active = true;
    out->sig = sig;
    out->handler = disp.handler;
    out->restorer = disp.restorer;
    out->sa_siginfo = (disp.flags & sf::kSaSiginfo) != 0;
    out->frame = uc;
    for (unsigned i = 0; i < 16; ++i) out->saved[i] = t.state.r[i];
    out->saved_cpsr = t.state.cpsr;
    out->saved_mask = saved_mask;
  }

  // The handler's arguments. With SA_SIGINFO it takes (sig, &info, &ucontext); without it, just the signal.
  // r0 is the signal either way, and a handler that only takes `int` ignores the rest — passing them is what
  // the kernel does, and it costs nothing.
  t.state.r[0] = static_cast<std::uint32_t>(sig);
  t.state.r[1] = info;
  t.state.r[2] = uc;
  // The return address is the restorer the guest supplied (SA_RESTORER is required on AArch32 and Mono sets
  // it). Its code is `mov r7, #__NR_rt_sigreturn; svc 0`, which this monitor answers by restoring the frame.
  // `lr` keeps the Thumb bit: both tiers read the mode of a return address from it (`bx lr` semantics).
  t.state.r[14] = disp.restorer;
  t.state.r[13] = frame & ~(sf::kStackAlign - 1u);  // the handler's stack starts at the frame
  // The handler's address carries the Thumb bit the guest's own `rt_sigaction` stored (an ARM function
  // pointer is `address | T`, and bionic's `SA_RESTORER` restorer is stored the same way). `pc` is kept
  // *clean* and the mode carries the T bit — that is this engine's convention, and handing an odd `pc` to
  // either tier is the "misaligned for the requested mode" fault the delivery test caught.
  const isa::Mode handler_mode = (disp.handler & 1u) != 0 ? isa::Mode::Thumb : isa::Mode::Arm;
  const std::uint32_t handler_pc = disp.handler & (handler_mode == isa::Mode::Thumb ? ~1u : ~3u);
  t.state.pc = handler_pc;
  t.state.r[15] = handler_pc;
  // Enter in the state the handler was written for: the interrupted CPSR, aligned to the handler's mode, with
  // the interrupted IT block cleared. Tier J derives ITSTATE from CPSR, so leaving the IT bits set would
  // predicate the handler's first instructions.
  t.state.cpsr = exec::CpsrWithIt(exec::CpsrWithMode(t.state.cpsr, handler_mode), 0);
  t.state.mode = handler_mode;
  t.state.itstate = 0;  // a handler never starts inside the interrupted IT block
  // The handler keeps the thread pointer and the signal mask it will run under.
  t.signal_mask = handler_mask;
  // The record the restorer's `rt_sigreturn` reads from the calling thread. Pushed here, popped there:
  // an inner delivery (Mono's restart signal inside its suspend handler) leaves the outer one underneath.
  t.deliveries.push_back(SignalDeliveryState{true, sig, uc, disp.handler, disp.restorer, saved_mask});
  {
    SignalDeliveryState& rec = t.deliveries.back();
    for (unsigned i = 0; i < 32; ++i) rec.d[i] = interrupted_d[i];
    rec.fpscr = interrupted_fpscr;
  }

  *why = "signal " + std::to_string(sig) + " delivered: handler " + Hex(disp.handler) + " entered with " +
         (out != nullptr && out->sa_siginfo ? "SA_SIGINFO (sig, &info, &ucontext)" : "the signal as r0") +
         ", frame at " + Hex(frame) + ", restorer " + Hex(disp.restorer) + "; interrupted pc " +
         Hex(interrupted_pc) + " sp " + Hex(interrupted_sp) + " r4 " + Hex(interrupted_r4);
  return true;
}

bool ReturnFromSignal(GuestThread& t, mem::GuestMemory& mem, std::string* why) {
  if (t.deliveries.empty()) {
    *why = "rt_sigreturn with no delivery in progress on this thread: there is no frame to restore";
    return false;
  }
  const SignalDeliveryState& d = t.deliveries.back();
  const std::uint32_t uc = d.frame;
  const auto Get32 = [&](std::uint32_t at, std::uint32_t* v) { return mem.Read(at, v, 4); };
  const auto GetSc = [&](std::uint32_t rel, std::uint32_t* v) { return Get32(uc + sf::kUcMcontext + rel, v); };

  // Read the sigcontext back out of the frame rather than from the saved copy: a handler is allowed to edit
  // the ucontext it was given (to change where the interrupted code resumes), and the kernel honours that.
  std::uint32_t sp = 0, lr = 0, pc = 0, cpsr = 0;
  bool ok = true;
  for (std::uint32_t i = 0; i < 13 && ok; ++i) {
    std::uint32_t v = 0;
    ok = GetSc(sf::kScR0 + i * 4, &v);
    if (ok) t.state.r[i] = v;
  }
  ok = ok && GetSc(sf::kScSp, &sp) && GetSc(sf::kScLr, &lr) && GetSc(sf::kScPc, &pc) &&
       GetSc(sf::kScCpsr, &cpsr);
  if (!ok) {
    *why = "rt_sigreturn: the sigcontext at " + Hex(uc) + " is not readable guest memory, so the "
           "interrupted state cannot be restored";
    return false;
  }
  t.state.r[13] = sp;
  t.state.r[14] = lr;
  t.state.pc = pc;
  t.state.r[15] = pc;
  t.state.cpsr = cpsr;
  // The handler may have used VFP freely; the interrupted FP file is put back from the delivery record.
  for (unsigned i = 0; i < 32; ++i) t.state.d[i] = d.d[i];
  t.state.fpscr = d.fpscr;
  // The handler is entered as Thumb and may return having changed the T bit; take the restored CPSR's mode,
  // which is what the kernel does (the sigcontext's arm_cpsr is authoritative).
  t.state.mode = (cpsr & sf::kCpsrThumb) != 0 ? isa::Mode::Thumb : isa::Mode::Arm;
  t.state.itstate = 0;
  // The mask returns to what it was before the handler, read from the frame for the same reason as above.
  // A frame that cannot be read back is not a reason to leave the handler's mask in place: the delivery's
  // own record is the fallback, and it is the value the frame was written from.
  std::uint32_t mask_lo = 0, mask_hi = 0;
  if (Get32(uc + sf::kUcSigmask, &mask_lo) && Get32(uc + sf::kUcSigmask + 4, &mask_hi)) {
    t.signal_mask = (static_cast<std::uint64_t>(mask_hi) << 32) | mask_lo;
  } else {
    t.signal_mask = d.saved_mask;
  }
  // A handler entered while the thread was in rt_sigsuspend ran with the temporary mask; the kernel puts
  // the pre-suspend mask back when the handler returns.
  if (t.sigsuspend_restore) {
    t.signal_mask = t.sigsuspend_old_mask;
    t.sigsuspend_restore = false;
  }
  // This handler is over. If an outer one is on the stack, its interrupted state is exactly what the
  // sigcontext just restored, so the next `rt_sigreturn` reads the outer frame in turn.
  t.deliveries.pop_back();
  *why = "rt_sigreturn: restored pc " + Hex(pc) + ", sp " + Hex(sp) + ", cpsr " + Hex(cpsr) +
         (t.deliveries.empty() ? "" : ", " + std::to_string(t.deliveries.size()) + " outer handler(s) still active");
  return true;
}

bool DeliverPendingSignal(GuestThread& t, mem::GuestMemory& mem, syscall::Monitor& monitor, std::string* why) {
  // Nothing in the guest's blocked mask is deliverable: the kernel holds it until the mask is lowered, so
  // taking it here would enter a handler the guest is not ready for and lose the signal on the way out.
  int sig = monitor.TakePendingSignal(t.tid);
  if (sig == 0) {
    *why = "no signal is pending for tid " + std::to_string(t.tid);
    return false;
  }
  // Nesting is required, not tolerated: Mono's suspend handler waits for a restart signal (SIGXCPU)
  // to run inside it. The cap exists only so a handler that signals itself cannot grow the host stack
  // without bound; at the cap the signal stays pending rather than corrupting the outer delivery.
  constexpr std::size_t kMaxNestedSignals = 8;
  if (t.deliveries.size() >= kMaxNestedSignals) {
    monitor.QueueSignal(t.tid, sig);
    *why = "signal " + std::to_string(sig) + " is pending with " + std::to_string(t.deliveries.size()) +
           " handlers already active on this thread; the nesting cap is " +
           std::to_string(kMaxNestedSignals) + ", so it stays pending";
    return false;
  }
  if (sig >= 1 && sig <= 64 && (t.signal_mask & (1ull << (sig - 1))) != 0) {
    // Blocked: put it back. It is re-queued at the tail, so another deliverable signal still gets through
    // first rather than the blocked one spinning at the head of the queue.
    monitor.QueueSignal(t.tid, sig);
    *why = "signal " + std::to_string(sig) + " is blocked in this thread's mask (" + Hex64(t.signal_mask) +
           "), so it stays pending";
    return false;
  }
  const syscall::Monitor::Disposition disp = monitor.disposition(sig);
  if (!disp.catches()) {
    // SIG_DFL / SIG_IGN. This build runs no default action, and it says so rather than pretending the
    // signal was handled: the guest asked for a disposition it did not install a handler for. A
    // sigsuspend waiting for it has ended (the syscall already returned EINTR), so put its old mask back.
    if (t.sigsuspend_restore) {
      t.signal_mask = t.sigsuspend_old_mask;
      t.sigsuspend_restore = false;
    }
    *why = "signal " + std::to_string(sig) + " has disposition " +
           (disp.handler == 0 ? "SIG_DFL, and this build runs no default action" : "SIG_IGN") +
           "; nothing is entered";
    return false;
  }
  SignalDelivery d;
  if (!DeliverSignal(t, mem, disp, sig, &d, why)) {
    if (t.sigsuspend_restore) {
      t.signal_mask = t.sigsuspend_old_mask;
      t.sigsuspend_restore = false;
    }
    return false;
  }
  monitor.CountDelivered();
  return true;  // `why` keeps DeliverSignal's success description: the live log is built from it
}

}  // namespace rb::runtime
