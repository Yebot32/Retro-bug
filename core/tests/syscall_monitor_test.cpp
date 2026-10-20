/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C07's monitor, tested as a decision table. The values asserted here are the measured ones from
 * `tests/corpus/syscall-abi-arm.txt` (an `armeabi-v7a` process on an SM-X710), not the ones from
 * recollection: which number the guest puts in r7, what a real kernel answers for an unknown number, and
 * what the carry flag is doing — the last of which turned out to be nothing, and is a test rather than a
 * comment because a future edit "fixing" it back to the textbook rule would be a silent behaviour change.
 * The `mprotect` cases are the exception to that provenance: a grant this build deliberately does not
 * enforce has no device value to assert, so what they pin down is the monitor's own record of it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace rb;
namespace sc = rb::syscall;
namespace exec = rb::exec;
namespace diag = rb::diag;
namespace isa = rb::isa;
namespace mem = rb::mem;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK_CTX(cond, detail)                      \
  do {                                               \
    ++checks;                                        \
    if (!(cond)) fail(#cond, __LINE__, detail);       \
  } while (0)
#define CHECK(cond) CHECK_CTX(cond, {})

exec::MachineState State(std::uint32_t nr) {
  exec::MachineState s{};
  s.mode = isa::Mode::Thumb;
  s.pc = 0x1000;
  s.r[7] = nr;
  s.r[13] = 0x8000;
  s.r[15] = s.pc;
  return s;
}

/// A guest window for the one call in this lane whose answer depends on what the model has mapped. Its
/// granule comes from the host rather than from the guest page constant, because `GuestMemory::Create`
/// refuses a window finer than the kernel's own pages: a lane that hard-coded 4 KiB would pass on a
/// 4 KiB-page emulator and fail to build a window at all on the 16 KiB-page phone these cases exist for.
std::unique_ptr<mem::GuestMemory> Window() {
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;  // never assume where the OS landed the window
  const long host_page = sysconf(_SC_PAGESIZE);
  model.host_page_shift = mem::kGuestPageShift;
  while (host_page > 0 && (1L << model.host_page_shift) < host_page) ++model.host_page_shift;
  std::string err;
  auto window = mem::GuestMemory::Create(16u * mem::kGuestPage, model, &err);
  if (!window) fail("guest window creation failed", __LINE__, err);
  return window;
}

// ------------------------------------------------------- the frame

void frame_takes_the_number_from_r7_not_the_encoding() {
  exec::MachineState s = State(sc::nr::kExitGroup);
  const sc::Frame f = sc::Frame::Capture(s, 0x9c);
  CHECK_CTX(f.number == sc::nr::kExitGroup, std::to_string(f.number));
  CHECK_CTX(f.imm == 0x9c, std::to_string(f.imm));
  CHECK_CTX(f.pc == 0x1000, std::to_string(f.pc));
  // The two channels have to stay separate: bionic puts the number in r7 and leaves the immediate at
  // zero, and ARM EABI's old OABI style put it in the immediate. A monitor that read the wrong one would
  // work for half of history and silently misroute every call for the other half.
  CHECK(f.number != f.imm);
}

void r7_is_the_number_not_a_masked_remainder() {
  // r7 is taken as the syscall number, full stop. The kernel bounds-checks it; the first draft
  // masked it with __NR_SYSCALL_MASK, which is a bounds-check constant rather than a mask, and a
  // hand-written stub with a garbage high bit then *executed* the aliased low syscall where the
  // kernel would have answered -ENOSYS.
  exec::MachineState s = State(0x0010'0000u | sc::nr::kGetpid);
  const sc::Frame f = sc::Frame::Capture(s, 0);
  CHECK_CTX(f.number == (0x0010'0000u | sc::nr::kGetpid),
            "0x" + std::to_string(f.number) + ": the high bit survives capture");
  sc::Monitor m;
  const sc::Outcome o = m.Classify(sc::Frame::Capture(s, 0), s);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEnosys),
            std::to_string(o.r0) + " want -38, the value measured from hardware for this exact case");

  // Above the table outright: refused, not wrapped into something that exists.
  exec::MachineState hi = State(0x00ff'fff0u);
  const sc::Outcome o2 = m.Classify(sc::Frame::Capture(hi, 0), hi);
  CHECK_CTX(o2.verdict == sc::Verdict::kUnsupported, sc::Name(o2.verdict));
  CHECK_CTX(o2.r0 == -static_cast<std::int32_t>(sc::err::kEnosys), std::to_string(o2.r0));
}

/// The measured return convention. `-36` for ENAMETOOLONG is the number that settles the
/// kernel-versus-EABI question (see `tools/syscall_abi_probe.sh`), and the carry bit is settled by the
/// fact that the real kernel leaves it set after a *successful* `sched_yield` returning 0. So a monitor
/// that "helpfully" cleared C on success and set it on failure would differ from every real 32-bit
/// Android process, and the difference would only show up in a guest that tests the flag.
void return_value_and_the_carry_bit() {
  exec::MachineState s = State(sc::nr::kWrite);
  s.cpsr = 0x80000020u;  // N and T, with C clear: the state the two designs disagree about
  const sc::Frame f = sc::Frame::Capture(s, 0);
  const std::uint32_t before = s.cpsr;

  const sc::Outcome bad = sc::Outcome::Refused(sc::err::kEnametoolong, "measured -36 on hardware");
  CHECK_CTX(bad.r0 == -36, std::to_string(bad.r0));
  CHECK_CTX(sc::LooksLikeError(bad.r0), "a refused call must look like an error by range");
  CHECK_CTX(f.ApplyReturn(s, bad) == static_cast<std::uint32_t>(-36), "the value written");
  CHECK_CTX(s.r[0] == static_cast<std::uint32_t>(-36), std::to_string(s.r[0]));
  CHECK_CTX(s.cpsr == before, "CPSR moved: the monitor is synthesising a flag the real kernel does not");
  CHECK_CTX(s.r[15] == s.pc, "r15 must stay equal to pc for the resume to be the same in both tiers");

  const sc::Outcome good = sc::Outcome::Emulated(23284, "the measured getpid result");
  f.ApplyReturn(s, good);
  CHECK_CTX(s.r[0] == 23284u, std::to_string(s.r[0]));
  CHECK_CTX(s.cpsr == before, "success must not touch the flags either");
}

// ------------------------------------------------------- decisions

void known_numbers_are_answered_or_refused_not_guessed() {
  sc::Monitor m;
  // No ids configured yet: refused rather than answered with a plausible zero.
  exec::MachineState s = State(sc::nr::kGetpid);
  sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(s.r[0] == static_cast<std::uint32_t>(-38), "r0 after an unconfigured getpid");

  m.set_ids(4242, 4243);
  s = State(sc::nr::kGetpid);
  o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated, sc::Name(o.verdict));
  CHECK_CTX(s.r[0] == 4242u, std::to_string(s.r[0]));
  s = State(sc::nr::kGettid);
  o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated && s.r[0] == 4243u, "gettid must differ from getpid");

  // The I/O and memory calls are *named* refusals: the message has to say which component is missing,
  // because "ENOSYS" alone in a support report describes a gap nobody can triage.
  s = State(sc::nr::kOpenat);
  o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -38, std::to_string(o.r0));
  CHECK_CTX(o.detail.find("openat") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("allowlist") != std::string::npos, o.detail);
}

void exit_group_is_a_verdict_not_a_value() {
  sc::Monitor m;
  exec::MachineState s = State(sc::nr::kExitGroup);
  s.r[0] = 3;  // the status
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kExit, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == 3, std::to_string(o.r0));
  CHECK_CTX(!sc::LooksLikeError(3), "an exit code of 3 is not an errno");
  // The verdict is what *ends* the run rather than resuming it: `abi::CallGuest` turns `kExit` into
  // `StopReason::Exited` and takes its status from the same r0 asserted above (`abi/call.cpp`), so a
  // guest that asked to die is not handed a value and stepped past. The detail becomes that stop's
  // reason, so it has to name the call that caused it.
  CHECK_CTX(o.detail.find("exit_group") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("3") != std::string::npos, o.detail);
}

void tls_travels_on_the_state_field() {
  sc::Monitor m;
  exec::MachineState s = State(sc::nr::kSetTls);
  s.r[0] = 0x1234'5000u;
  s.r[15] = s.pc;
  CHECK(m.Handle(s, 0).verdict == sc::Verdict::kEmulated);
  CHECK_CTX(s.tp == 0x1234'5000u, "tp after arm_set_tls");
  // The read round-trips, which is what makes the *syscall* TLS path usable while the `mrc` one is still
  // a missing Coprocessor hook (`30` §Step 9). Two tiers and one field: `tp` is in MachineState, so this
  // works identically under both.
  s = State(sc::nr::kGetTls);
  s.tp = 0x1234'5000u;
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated, o.detail);
  CHECK_CTX(s.r[0] == 0x1234'5000u, std::to_string(s.r[0]));
}

void cacheflush_reaches_the_tier_that_owns_the_cache() {
  std::vector<std::pair<exec::GuestAddr, exec::GuestAddr>> seen;
  sc::Monitor m;
  m.set_invalidator([&](exec::GuestAddr a, exec::GuestAddr b) { seen.emplace_back(a, b); });

  exec::MachineState s = State(sc::nr::kCacheFlush);
  s.r[0] = 0x4000;
  s.r[1] = 0x4100;
  CHECK_CTX(m.Handle(s, 0).verdict == sc::Verdict::kEmulated, "cacheflush must answer 0");
  CHECK_CTX(seen.size() == 1u, std::to_string(seen.size()));
  CHECK_CTX(seen[0].first == 0x4000 && seen[0].second == 0x4100, "the range must reach the tier intact");
  CHECK_CTX(s.r[0] == 0u, std::to_string(s.r[0]));

  // Without a tier wired up, the flush is refused *and says why*: an silently-succeeding cacheflush with
  // nothing invalidated behind it is the self-modifying-code bug that would be hardest to find.
  sc::Monitor bare;
  s = State(sc::nr::kCacheFlush);
  s.r[0] = 0x4000;
  s.r[1] = 0x4100;
  const sc::Outcome o = bare.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(o.detail.find("invalidator") != std::string::npos, o.detail);
  CHECK_CTX(seen.size() == 1u, "the refused flush must not have reached the invalidator");
}

// ------------------------------------------------------- mprotect

/// The grant this build deliberately gives without enforcing it. The assertion that matters most here is
/// the last one: a monitor that answered 0 *and* made the page read-only would pass every check about the
/// value and the note while doing something no other part of this emulator asked it to do.
void mprotect_inside_the_window_is_granted_and_recorded_as_not_enforced() {
  auto window = Window();
  if (!window) return;
  std::string err;
  CHECK_CTX(window->Map(0x2000, mem::kGuestPage, mem::kRead | mem::kWrite, &err), err);

  sc::Monitor m;
  m.set_memory(window.get());
  exec::MachineState s = State(sc::nr::kMprotect);
  s.r[0] = 0x2000;             // addr
  s.r[1] = mem::kGuestPage;    // len
  s.r[2] = mem::kRead;         // prot
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == 0, std::to_string(o.r0));
  CHECK_CTX(s.r[0] == 0u, "the grant never reached the guest");
  CHECK_CTX(o.detail.find("mprotect") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("[0x2000, 0x3000)") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("prot=0x1") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("not enforced") != std::string::npos, o.detail);
  CHECK_CTX(m.notes().size() == 1u && m.notes()[0].find("not enforced") != std::string::npos,
            m.notes().empty() ? std::string{"no note was recorded"} : m.notes()[0]);
  CHECK_CTX(m.TallyText() == "mprotect x1", m.TallyText());
  // `Write` asks the model, so this is the model's answer and not a second opinion: the page is exactly
  // as writable as it was mapped. On a device the call above is what stops this write.
  const std::uint32_t probe = 0x5a5a5a5au;
  CHECK_CTX(window->Write(0x2000, &probe, sizeof probe),
            "a grant that had really applied PROT_READ would refuse this write");
}

void mprotect_outside_the_window_is_refused_with_the_range_and_enomem() {
  auto window = Window();
  if (!window) return;
  sc::Monitor m;
  m.set_memory(window.get());
  exec::MachineState s = State(sc::nr::kMprotect);
  s.r[0] = 0x7f000000;  // far above a window this case made sixteen guest pages wide
  s.r[1] = mem::kGuestPage;
  s.r[2] = mem::kRead | mem::kWrite;
  const sc::Outcome o = m.Handle(s, 0);
  // Serviced, and the service *is* the errno: a kernel walks the range's VMAs and answers ENOMEM for an
  // address no VMA covers, so this is not one of RetroBug's gaps and must not be reported as ENOSYS.
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -12, std::to_string(o.r0) + " want -12 (ENOMEM)");
  CHECK_CTX(s.r[0] == static_cast<std::uint32_t>(-12), "the refusal never reached the guest");
  CHECK_CTX(o.detail.find("mprotect") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("[0x7f000000, 0x7f001000)") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("ENOMEM") != std::string::npos, o.detail);
}

/// The case that separates "the range is inside the window" from "the range is mapped": page 0x2000 is
/// mapped and 0x3000/0x4000 are not, so a handler that looked at the first page only would grant here.
void mprotect_over_a_hole_in_the_window_names_the_unmapped_page() {
  auto window = Window();
  if (!window) return;
  std::string err;
  CHECK_CTX(window->Map(0x2000, mem::kGuestPage, mem::kRead | mem::kWrite, &err), err);
  sc::Monitor m;
  m.set_memory(window.get());
  exec::MachineState s = State(sc::nr::kMprotect);
  s.r[0] = 0x2000;
  s.r[1] = 3u * mem::kGuestPage;
  s.r[2] = mem::kRead;
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.r0 == -12, std::to_string(o.r0) + " want -12 (ENOMEM)");
  CHECK_CTX(o.detail.find("mprotect") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("0x3000") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("no mapping") != std::string::npos, o.detail);
}

// ------------------------------------------------------- the numbers themselves

/// Every shared name differs between the guest and the host, which is the measured fact behind
/// `docs/20` §C07's invariant. Asserted as data so a future table that quietly reuses a number fails
/// here rather than in the field.
void guest_numbers_are_not_host_numbers() {
  struct { std::uint32_t n; const char* name; std::uint32_t host; } rows[] = {
      {sc::nr::kRead, "read", 63},   {sc::nr::kWrite, "write", 64},
      {sc::nr::kClose, "close", 57}, {sc::nr::kGetpid, "getpid", 172},
      {sc::nr::kGettid, "gettid", 178}, {sc::nr::kOpenat, "openat", 56},
      {sc::nr::kExitGroup, "exit_group", 94}, {sc::nr::kMmap2, "mmap2", 0 /* no host counterpart */},
      {sc::nr::kMremap, "mremap", 216},
  };
  for (const auto& r : rows) {
    CHECK_CTX(sc::Name(r.n) != nullptr, "nr 0x" + std::to_string(r.n) + " has no name");
    if (r.host != 0) {
      CHECK_CTX(r.n != r.host, std::string{r.name} + " is the same number on both sides: " +
                                   std::to_string(r.n) +
                                   " — if that is ever true, check which table this test was written from");
    }
  }
  CHECK_CTX(sc::Name(0xdeadu) == nullptr, "an unnamed number must be nullptr, not a guess");
  CHECK_CTX(sc::Describe(0xdeadu).find("0xdead") != std::string::npos, sc::Describe(0xdeadu));
}

void the_error_range_is_the_discriminator() {
  CHECK(sc::LooksLikeError(-1));
  CHECK(sc::LooksLikeError(-38));
  CHECK(sc::LooksLikeError(-sc::kMaxErrno));
  CHECK(!sc::LooksLikeError(0));
  CHECK(!sc::LooksLikeError(23284));           // the measured getpid result
  CHECK(!sc::LooksLikeError(-4096));           // below the range: a pointer, not an errno
  CHECK(!sc::LooksLikeError(0x8000'0000u));    // a large unsigned value stays unsigned
}

void the_names_are_what_the_headers_call_them() {
  CHECK(std::string{sc::Name(sc::Verdict::kEmulated)} == "emulated");
  CHECK(std::string{sc::Name(sc::nr::kCacheFlush)} == std::string{"arm_cacheflush"});
  CHECK_CTX(sc::nr::kCacheFlush == 0xf0002u, "measured from the NDK header and the device table");
  CHECK_CTX(sc::kArmNrBase == 0xf0000u, std::to_string(sc::kArmNrBase));
}

/// The one refusal whose errno is a measurement rather than a default. `execve` from an app process on
/// real Android answers `EACCES` because SELinux will not let an app execute a file in its own data
/// directory, and the distinction the guest sees matters: `ENOSYS` says "RetroBug has no handler", which
/// an app developer reads as an emulator gap and reports as one, while `EACCES` says "the platform
/// refuses this everywhere", which is the truth and sends them to the right document.
void execve_is_denied_with_the_measured_errno() {
  sc::Monitor m;
  exec::MachineState s = State(sc::nr::kExecve);
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kDenied, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -13, std::to_string(o.r0) + " want -13 (EACCES), measured on SM-X710");
  CHECK_CTX(s.r[0] == static_cast<std::uint32_t>(-13), "the refusal never reached the guest");
  CHECK_CTX(o.detail.find("EACCES") != std::string::npos, o.detail);
  CHECK_CTX(o.detail.find("measured") != std::string::npos, o.detail);
  // And the default for an unmeasured number is still ENOSYS, so the two cases stay distinguishable —
  // a policy that answers everything the same way is not measuring anything.
  exec::MachineState u = State(0x0fff0u);
  const sc::Outcome d = m.Handle(u, 0);
  CHECK_CTX(d.verdict == sc::Verdict::kUnsupported, sc::Name(d.verdict));
  CHECK_CTX(d.r0 == -38, std::to_string(d.r0));
}

// ------------------------------------------------------- the C17 hook

void the_policy_is_echoed_not_assumed() {
  sc::Monitor m;
  m.set_ids(7, 8);
  exec::MachineState s = State(sc::nr::kGetpid);
  m.Handle(s, 0);
  s = State(sc::nr::kOpenat);
  m.Handle(s, 0);
  s = State(sc::nr::kOpenat);
  m.Handle(s, 0);

  CHECK_CTX(m.tally().size() == 2u, std::to_string(m.tally().size()));
  CHECK_CTX(m.TallyText() == "getpid x1, openat x2", m.TallyText());

  diag::Recorder rec;
  m.Echo(rec, "launch", "measured on SM-X710");
  const std::string text = rec.Render();
  CHECK_CTX(text.find("field = syscall-policy") != std::string::npos, text);
  CHECK_CTX(text.find("no forwarding") != std::string::npos, text);
  CHECK_CTX(text.find("openat x2") != std::string::npos, text);
  CHECK_CTX(text.find("measured on SM-X710") != std::string::npos, text);
  // A launch that records this has answered the field, so the report's own gap list shrinks.
  CHECK_CTX(rec.MissingRequired().size() == 7u, "syscall-policy is not statically required; " +
                                                    std::to_string(rec.MissingRequired().size()));
}

/// A monitor that counted nothing would still answer correctly, and the tally is the only evidence a
/// later profile has about what an app actually needs. Per instance, because an app's syscall history
/// mixed into another's is a diagnostics lie.
void the_tally_is_per_monitor_and_in_first_seen_order() {
  sc::Monitor a;
  sc::Monitor b;
  exec::MachineState s = State(sc::nr::kGettid);
  a.Handle(s, 0);
  s = State(sc::nr::kGetpid);
  a.Handle(s, 0);
  s = State(sc::nr::kGettid);
  b.Handle(s, 0);
  CHECK_CTX(a.tally().size() == 2u, std::to_string(a.tally().size()));
  CHECK_CTX(b.tally().size() == 1u, std::to_string(b.tally().size()));
  CHECK_CTX(a.TallyText() == "gettid x1, getpid x1", a.TallyText());
  CHECK_CTX(b.TallyText().find("getpid") == std::string::npos, b.TallyText());
}

}  // namespace

// ------------------------------------------------------- the heap/console batch

/// `mmap2` is what bionic's allocator calls before an app's first allocation, and until 2026-10-09 it
/// was refused with ENOSYS — every allocation in the game's constructors failed and the first
/// allocating constructor left through `exit_group(127)`. Served, the answer must be *real* memory:
/// mapped in the model, writable, disjoint from the next allocation, and inside the window.
void mmap2_serves_real_anonymous_pages() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  const auto mmap = [&](std::uint32_t hint, std::uint32_t len, std::uint32_t prot,
                        std::uint32_t flags, std::uint32_t fd) {
    exec::MachineState s = State(sc::nr::kMmap2);
    s.r[0] = hint;
    s.r[1] = len;
    s.r[2] = prot;
    s.r[3] = flags;
    s.r[4] = fd;
    return m.Handle(s, 0);
  };
  constexpr std::uint32_t kRw = 3, kPrivateAnon = 0x22, kNoFd = 0xffff'ffffu;

  const sc::Outcome a = mmap(0, 0x2000, kRw, kPrivateAnon, kNoFd);
  CHECK_CTX(a.verdict == sc::Verdict::kEmulated, a.detail);
  const std::uint32_t at = static_cast<std::uint32_t>(a.r0);
  CHECK_CTX(at != 0 && (at & 0xfffu) == 0, "returned " + std::to_string(at));
  CHECK_CTX(window->IsMapped(at) && window->IsMapped(at + 0x1000),
            "the mapping is not in the model's page table");
  const std::uint32_t magic = 0x51ab'1e00u;
  CHECK_CTX(window->Write(at + 0x40, &magic, 4), "the served mapping is not writable");
  std::uint32_t back = 0;
  CHECK(window->Read(at + 0x40, &back, 4));
  CHECK_CTX(back == magic, std::to_string(back) + " want " + std::to_string(magic));

  const sc::Outcome b = mmap(0, 0x1000, kRw, kPrivateAnon, kNoFd);
  CHECK_CTX(b.verdict == sc::Verdict::kEmulated && static_cast<std::uint32_t>(b.r0) != at,
            "the second allocation landed on the first");

  const sc::Outcome zero = mmap(0, 0, kRw, kPrivateAnon, kNoFd);
  CHECK_CTX(zero.r0 == -static_cast<std::int32_t>(sc::err::kEinval),
            "len=0 must be EINVAL, got " + std::to_string(zero.r0));

  const sc::Outcome file = mmap(0, 0x1000, kRw, 0x02 /*MAP_PRIVATE, no ANON*/, 7);
  CHECK_CTX(file.verdict == sc::Verdict::kUnsupported, sc::Name(file.verdict));
  CHECK_CTX(file.detail.find("file descriptor") != std::string::npos, file.detail);
}

/// The runtime's window is the whole 32-bit space (4 GiB). `uint32(window_bytes)` is 0 there, so a 32-bit
/// limit refused every allocation; and the top of that window belongs to the trap and the kuser helpers, so
/// the monitor allocates only below its ceiling (GuestProcess's 0xfff00000), whatever the window holds.
void mmap2_in_a_4gib_window_stays_below_the_ceiling() {
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  const long host_page = sysconf(_SC_PAGESIZE);
  model.host_page_shift = mem::kGuestPageShift;
  while (host_page > 0 && (1L << model.host_page_shift) < host_page) ++model.host_page_shift;
  std::string err;
  auto window = mem::GuestMemory::Create(std::size_t{1} << 32, model, &err);
  CHECK_CTX(window != nullptr, "a 4 GiB biased window: " + err);
  if (!window) return;
  constexpr std::uint32_t kCeiling = 0xfff0'0000u;
  sc::Monitor m;
  m.set_memory(window.get());
  m.set_alloc_ceiling(kCeiling);
  const auto mmap = [&](std::uint32_t hint, std::uint32_t len) {
    exec::MachineState s = State(sc::nr::kMmap2);
    s.r[0] = hint;
    s.r[1] = len;
    s.r[2] = 3;            // PROT_READ | PROT_WRITE
    s.r[3] = 0x22;         // MAP_PRIVATE | MAP_ANONYMOUS
    s.r[4] = 0xffff'ffffu;  // fd -1
    return m.Handle(s, 0);
  };
  const sc::Outcome few = mmap(0, 4 * mem::kGuestPage);
  CHECK_CTX(few.verdict == sc::Verdict::kEmulated, few.detail);
  const std::uint32_t at = static_cast<std::uint32_t>(few.r0);
  CHECK_CTX(!(few.r0 < 0 && few.r0 >= -4095) && at != 0 && static_cast<std::uint64_t>(at) + 4 * mem::kGuestPage <= kCeiling &&
                window->IsMapped(at),
            "few pages -> 0x" + std::to_string(at));
  // A hint straddling the ceiling is not honoured (the kernel may ignore a hint); the answer lands below.
  const sc::Outcome hinted = mmap(kCeiling - mem::kGuestPage, 2 * mem::kGuestPage);
  CHECK_CTX(hinted.verdict == sc::Verdict::kEmulated &&
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(hinted.r0)) + 2 * mem::kGuestPage <= kCeiling,
            hinted.detail);
  // Fits in the 4 GiB window, not below the ceiling: ENOMEM.
  const sc::Outcome big = mmap(0, kCeiling);
  CHECK_CTX(big.r0 == -static_cast<std::int32_t>(sc::err::kEnomem), "len=0xfff00000: " + big.detail);
}

/// Anonymous mappings are placed top-down, as Linux places them (mmap_base under the stack, each new mapping
/// in the highest gap below): the first lands just under the ceiling, the next directly below it, and a freed
/// gap at the top is reused first. A guest that reads a little past the end of its newest heap region (Gumball's
/// cLanguage::read scans past its last string for a non-zero byte) then reads the older region above it, as
/// on a device, instead of running off the end of the window's used space.
std::string Hex(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%x", v);
  return b;
}

void mmap2_places_top_down_like_the_kernel() {
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  const long host_page = sysconf(_SC_PAGESIZE);
  model.host_page_shift = mem::kGuestPageShift;
  while (host_page > 0 && (1L << model.host_page_shift) < host_page) ++model.host_page_shift;
  std::string err;
  auto window = mem::GuestMemory::Create(std::size_t{1} << 32, model, &err);
  CHECK_CTX(window != nullptr, "a 4 GiB biased window: " + err);
  if (!window) return;
  constexpr std::uint32_t kCeiling = 0xfff0'0000u;
  sc::Monitor m;
  m.set_memory(window.get());
  m.set_alloc_ceiling(kCeiling);
  const auto mmap = [&](std::uint32_t len) {
    exec::MachineState s = State(sc::nr::kMmap2);
    s.r[1] = len;
    s.r[2] = 3;
    s.r[3] = 0x22;
    s.r[4] = 0xffff'ffffu;
    return static_cast<std::uint32_t>(m.Handle(s, 0).r0);
  };
  const std::uint32_t a = mmap(4 * mem::kGuestPage);
  const std::uint32_t b = mmap(2 * mem::kGuestPage);
  CHECK_CTX(a == kCeiling - 4 * mem::kGuestPage, "first at 0x" + Hex(a));
  CHECK_CTX(b == a - 2 * mem::kGuestPage, "second at 0x" + Hex(b) + ", first at 0x" + Hex(a));
  // Free the top one: a mapping that fits there goes back to the highest gap.
  exec::MachineState u = State(sc::nr::kMunmap);
  u.r[0] = a;
  u.r[1] = 4 * mem::kGuestPage;
  m.Handle(u, 0);
  const std::uint32_t c = mmap(mem::kGuestPage);
  CHECK_CTX(c == kCeiling - mem::kGuestPage, "after freeing the top, 0x" + Hex(c));
  const std::uint32_t d = mmap(8 * mem::kGuestPage);  // does not fit above b: goes below it
  CHECK_CTX(d == b - 8 * mem::kGuestPage, "a larger one at 0x" + Hex(d) + ", b at 0x" + Hex(b));
  // An address above 0x80000000 is a negative int32 but not an error (only -4095..-1 are): never a refusal.
  CHECK_CTX(m.refusals() == 0u, std::to_string(m.refusals()) + " refusals counted for successful mappings");
}

/// `munmap` releases what `mmap2` served: the pages stop being mapped, the call answers 0 whether or
/// not everything in the range was mapped — which is what a kernel does — and the range is validated
/// rather than trusted (a range past the window is EINVAL, not a silent no-op).
void munmap_releases_served_pages() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  exec::MachineState s = State(sc::nr::kMmap2);
  s.r[1] = 0x2000;
  s.r[2] = 3;
  s.r[3] = 0x22;
  s.r[4] = 0xffff'ffffu;
  const std::uint32_t at = static_cast<std::uint32_t>(m.Handle(s, 0).r0);
  CHECK(at != 0);
  exec::MachineState u = State(sc::nr::kMunmap);
  u.r[0] = at;
  u.r[1] = 0x2000;
  const sc::Outcome o = m.Handle(u, 0);
  CHECK_CTX(o.r0 == 0, o.detail);
  CHECK_CTX(!window->IsMapped(at) && !window->IsMapped(at + 0x1000), "pages still mapped");

  exec::MachineState past = State(sc::nr::kMunmap);
  past.r[0] = 0x0001'0000u + 0x1000u;  // past the 16-page window
  past.r[1] = 0x1000;
  CHECK_CTX(m.Handle(past, 0).r0 == -static_cast<std::int32_t>(sc::err::kEinval),
            "a range outside the window must be EINVAL");
}

/// `mremap` is how bionic's `__cxa_atexit` grows its handler array: Angry Birds Go logged "mmap/mremap
/// failed to allocate 8192 bytes" 136 times while it was ENOSYS. Kernel rules, each a row: grow in
/// place when the next pages are free; refuse with ENOMEM when they are taken and moving was not
/// allowed; move (contents and all, old pages released) under MREMAP_MAYMOVE; shrink in place.
void mremap_grows_moves_and_shrinks_like_a_kernel() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  // Placed by hint: the rows need room above a mapping, and anonymous mappings are placed top-down.
  const auto mmap = [&](std::uint32_t len, std::uint32_t hint) {
    exec::MachineState s = State(sc::nr::kMmap2);
    s.r[0] = hint;
    s.r[1] = len;
    s.r[2] = 3;
    s.r[3] = 0x22;
    s.r[4] = 0xffff'ffffu;
    return static_cast<std::uint32_t>(m.Handle(s, 0).r0);
  };
  const auto mremap = [&](std::uint32_t at, std::uint32_t old_len, std::uint32_t new_len, std::uint32_t flags) {
    exec::MachineState s = State(sc::nr::kMremap);
    s.r[0] = at;
    s.r[1] = old_len;
    s.r[2] = new_len;
    s.r[3] = flags;
    return m.Handle(s, 0);
  };
  const std::uint32_t a = mmap(0x1000, 0x4000);
  CHECK(a != 0);
  const std::uint32_t magic = 0xa7e7'1700u;
  CHECK(window->Write(a + 0x10, &magic, 4));

  sc::Outcome o = mremap(a, 0x1000, 0x2000, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated && static_cast<std::uint32_t>(o.r0) == a, o.detail);
  CHECK_CTX(window->IsMapped(a + 0x1000), "grown in place, but the new page is not mapped");

  const std::uint32_t blocker = mmap(0x1000, a + 0x2000);  // right after it, by hint
  CHECK_CTX(blocker == a + 0x2000, "the blocker landed at " + std::to_string(blocker));
  o = mremap(a, 0x2000, 0x3000, 0);
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEnomem), "no MAYMOVE, no room: " + o.detail);
  CHECK_CTX(window->IsMapped(a) && window->IsMapped(a + 0x1000), "a refused mremap touched the mapping");

  o = mremap(a, 0x2000, 0x3000, 1 /* MREMAP_MAYMOVE */);
  const std::uint32_t moved = static_cast<std::uint32_t>(o.r0);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated && moved != a && (moved & 0xfffu) == 0, o.detail);
  std::uint32_t back = 0;
  CHECK(window->Read(moved + 0x10, &back, 4));
  CHECK_CTX(back == magic, "the contents did not move: " + std::to_string(back));
  CHECK_CTX(!window->IsMapped(a) && !window->IsMapped(a + 0x1000), "the old pages were not released");
  CHECK_CTX(window->IsMapped(moved + 0x2000), "the moved mapping is not the new size");

  o = mremap(moved, 0x3000, 0x1000, 0);
  CHECK_CTX(static_cast<std::uint32_t>(o.r0) == moved && !window->IsMapped(moved + 0x1000) &&
                window->IsMapped(moved),
            "shrink: " + o.detail);

  o = mremap(moved + 4, 0x1000, 0x2000, 1);
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEinval), "a misaligned address must be EINVAL");
  o = mremap(moved, 0x1000, 0x2000, 2 /* FIXED without MAYMOVE */);
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEinval), "MREMAP_FIXED alone must be EINVAL");
}

/// fd 1 and 2 are the guest's console, and its own words must survive: the app printed to fd 2 four
/// times before exiting 127 and every byte was dropped until this capture existed. The full length is
/// reported written — that is what the fd does — and an unreadable buffer is EFAULT, not a silent
/// loss.
void console_writes_are_captured_and_answered_as_written() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr),
            "could not map the fixture pages: " + werr);
  const char text[] = "fatal: no level\n";
  CHECK(window->Write(0x100, text, sizeof(text)));
  exec::MachineState s = State(sc::nr::kWrite);
  s.r[0] = 2;
  s.r[1] = 0x100;
  s.r[2] = sizeof(text) - 1;
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.r0 == static_cast<std::int32_t>(sizeof(text) - 1), o.detail);
  CHECK_CTX(m.captured_stderr() == "fatal: no level\n", m.captured_stderr());
  CHECK_CTX(m.captured_stdout().empty(), "stderr text landed in stdout");

  exec::MachineState t = State(sc::nr::kWrite);
  t.r[0] = 1;
  t.r[1] = 0x100;
  t.r[2] = 7;
  CHECK(m.Handle(t, 0).r0 == 7);
  CHECK_CTX(m.captured_stdout() == "fatal: ", m.captured_stdout());

  exec::MachineState bad = State(sc::nr::kWrite);
  bad.r[0] = 2;
  bad.r[1] = 0x0001'0000u + 0x100u;  // outside the 16-page window
  bad.r[2] = 4;
  CHECK_CTX(m.Handle(bad, 0).r0 == -static_cast<std::int32_t>(sc::err::kEfault), "want EFAULT");
}

/// The futex answers are the single-threaded truth, said out loud: WAIT's immediate path is the
/// kernel's own (value changed -> EAGAIN), a matching value returns 0 with a note that no scheduler
/// here can wake it, and WAKE reports no waiters.
void futex_answers_the_single_threaded_truth() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr),
            "could not map the fixture pages: " + werr);
  const std::uint32_t word = 7;
  CHECK(window->Write(0x200, &word, 4));

  exec::MachineState mismatch = State(sc::nr::kFutex);
  mismatch.r[0] = 0x200;
  mismatch.r[1] = 0;  // WAIT
  mismatch.r[2] = 8;
  CHECK_CTX(m.Handle(mismatch, 0).r0 == -static_cast<std::int32_t>(sc::err::kEagain),
            "value-changed WAIT must be EAGAIN");

  exec::MachineState wait = State(sc::nr::kFutex);
  wait.r[0] = 0x200;
  wait.r[1] = 0x80u;  // WAIT | FUTEX_PRIVATE_FLAG
  wait.r[2] = 7;
  const sc::Outcome o = m.Handle(wait, 0);
  CHECK_CTX(o.r0 == 0 && o.verdict == sc::Verdict::kEmulated, o.detail);
  CHECK_CTX(o.detail.find("no wake can arrive") != std::string::npos, o.detail);

  exec::MachineState wake = State(sc::nr::kFutex);
  wake.r[0] = 0x200;
  wake.r[1] = 1;  // WAKE
  wake.r[2] = 1;
  CHECK(m.Handle(wake, 0).r0 == 0);

  // bionic's mutex slow path: FUTEX_WAIT_BITSET|PRIVATE (0x89) and FUTEX_WAKE_BITSET|PRIVATE (0x8a).
  // A mask that keeps the private flag, or one that drops the 9/10 mapping, refuses both as ENOSYS.
  exec::MachineState wait_bs = State(sc::nr::kFutex);
  wait_bs.r[0] = 0x200;
  wait_bs.r[1] = 0x89u;
  wait_bs.r[2] = 7;
  const sc::Outcome ob = m.Handle(wait_bs, 0);
  CHECK_CTX(ob.r0 == 0 && ob.verdict == sc::Verdict::kEmulated, ob.detail);
  wait_bs.r[2] = 8;
  CHECK_CTX(m.Handle(wait_bs, 0).r0 == -static_cast<std::int32_t>(sc::err::kEagain),
            "value-changed WAIT_BITSET must be EAGAIN");
  exec::MachineState wake_bs = State(sc::nr::kFutex);
  wake_bs.r[0] = 0x200;
  wake_bs.r[1] = 0x8au;
  wake_bs.r[2] = 1;
  const sc::Outcome wb = m.Handle(wake_bs, 0);
  CHECK_CTX(wb.r0 == 0 && wb.verdict == sc::Verdict::kEmulated, wb.detail);
}

/// `rt_sigaction` records a disposition, answers a query with the previous one, and names what it recorded.
/// `faccessat` goes through the same provider as `openat` and answers ENOENT for what the provider does not serve.
void sigaction_and_faccessat_answer_their_facts() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr),
            "could not map the fixture pages: " + werr);
  exec::MachineState sig = State(sc::nr::kRtSigaction);
  sig.r[0] = 6;      // SIGABRT
  sig.r[1] = 0x300;  // new handler struct (never read: nothing is delivered)
  sig.r[2] = 0x400;  // previous handler out
  const std::uint32_t dirty[5] = {0xdead, 0xdead, 0xdead, 0xdead, 0xdead};
  CHECK(window->Write(0x400, dirty, sizeof(dirty)));
  const sc::Outcome o = m.Handle(sig, 0);
  CHECK_CTX(o.r0 == 0, o.detail);
  std::uint32_t old[5] = {1, 1, 1, 1, 1};
  CHECK(window->Read(0x400, old, sizeof(old)));
  CHECK_CTX(old[0] == 0 && old[1] == 0, "oldact was not zeroed: " + std::to_string(old[0]));

  m.set_file_provider([](const std::string& path) -> std::optional<std::vector<std::uint8_t>> {
    if (path != "/data/app/game.apk") return std::nullopt;
    return std::vector<std::uint8_t>{0x50, 0x4b};  // the bytes are not this row's subject
  });
  CHECK(window->Write(0x500, "/data/app/game.apk\0", 19));
  exec::MachineState hit = State(sc::nr::kFaccessat);
  hit.r[0] = 0xffff'ff9cu;  // AT_FDCWD
  hit.r[1] = 0x500;
  hit.r[2] = 0;  // F_OK
  CHECK_CTX(m.Handle(hit, 0).r0 == 0, "a served path must be reachable");

  CHECK(window->Write(0x600, "/nope\0", 6));
  exec::MachineState miss = State(sc::nr::kFaccessat);
  miss.r[0] = 0xffff'ff9cu;
  miss.r[1] = 0x600;
  miss.r[2] = 0;
  CHECK_CTX(m.Handle(miss, 0).r0 == -static_cast<std::int32_t>(sc::err::kEnoent), "want ENOENT");
}

/// The ids this lane uses. `Monitor::set_ids` is what makes a tid live, and these are the values
/// `GuestProcess` itself uses, so the lane reads like the real launch path.
namespace GuestProcessIds {
constexpr std::int32_t kPid = 4242;
constexpr std::int32_t kMainTid = 4243;
}  // namespace GuestProcessIds

/// A signal number, passed through unchanged. It exists so the loop below reads as "the uncatchable
/// signals" rather than as two bare integers beside an assertion about being uncatchable.
constexpr int Uncatchable(int sig) { return sig; }

/// ---- C26: signal delivery.
///
/// The three facts these pin down, in the order delivery needs them: (1) what the guest installed is
/// readable back — bionic's arm32 `struct sigaction` is `{handler, flags, restorer, mask}`, which the
/// measured probe in tests/corpus/sigframe-arm-layout.txt confirms (flags at +4, not the mask); (2) a
/// signal aimed at a live thread is *queued*, not refused, and is taken exactly once; (3) `rt_sigreturn`
/// with no frame in progress is refused by name rather than answered with a 0 that would resume the
/// handler's own `svc` forever.
void signal_dispositions_are_what_the_guest_installed() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr),
            "could not map the fixture pages: " + werr);

  // The values libmono actually passes for SIGPWR (measured, Cars 3 on an SM-X710):
  // act = {handler 0x5fab59, flags 0x14000000 (SA_RESTART|SA_RESTORER), restorer 0x1a7240, mask 0xffffbfd9}.
  const std::uint32_t act[5] = {0x005f'ab59u, 0x1400'0000u, 0x001a'7240u, 0xffff'bfd9u, 0};
  CHECK(window->Write(0x300, act, sizeof act));
  exec::MachineState sig = State(sc::nr::kRtSigaction);
  sig.r[0] = 30;     // SIGPWR, Mono's GC thread-suspend
  sig.r[1] = 0x300;  // the struct a device reads
  sig.r[2] = 0;      // no previous handler wanted
  CHECK_CTX(m.Handle(sig, 0).r0 == 0, "rt_sigaction with a guest struct must be recorded");

  const sc::Monitor::Disposition d = m.disposition(30);
  CHECK_CTX(d.handler == 0x005f'ab59u, "handler was not read out of the guest struct");
  CHECK_CTX(d.flags == 0x1400'0000u, "flags are at +4; reading the mask there is the classic mistake");
  CHECK_CTX(d.restorer == 0x001a'7240u, "the restorer is required on AArch32 and is what delivery returns through");
  CHECK_CTX(d.mask == 0x0000'0000ffff'bfd9ull, "the blocked mask is at +12, as two 32-bit words");
  CHECK_CTX(d.catches(), "a handler above 1 catches");
  CHECK_CTX(m.installed_handler_count() == 1, "one handler installed so far");

  // A query (act == NULL) returns the recorded disposition in oldact and must not erase it.
  exec::MachineState q = State(sc::nr::kRtSigaction);
  q.r[0] = 30;
  q.r[1] = 0;
  q.r[2] = 0x340;
  CHECK_CTX(m.Handle(q, 0).r0 == 0, "a query is answered");
  std::uint32_t old[5] = {};
  CHECK_CTX(window->Read(0x340, old, sizeof old), "oldact is written");
  CHECK_CTX(old[0] == 0x005f'ab59u && old[2] == 0x001a'7240u,
            "oldact is the previous handler and restorer");
  CHECK_CTX(m.disposition(30).handler == 0x005f'ab59u, "a query must not erase the handler");

  // SIG_DFL (0) and SIG_IGN (1) are not handler addresses; a delivery must not enter them.
  const std::uint32_t ign[5] = {1, 0, 0, 0, 0};
  CHECK(window->Write(0x320, ign, sizeof ign));
  exec::MachineState s2 = State(sc::nr::kRtSigaction);
  s2.r[0] = 15;  // SIGTERM
  s2.r[1] = 0x320;
  CHECK_CTX(m.Handle(s2, 0).r0 == 0, "SIG_IGN is a valid disposition");
  CHECK_CTX(m.disposition(15).handler == 1, "SIG_IGN is recorded as 1");
  CHECK_CTX(!m.disposition(15).catches(), "SIG_IGN does not catch, so it is dropped rather than entered");
  CHECK_CTX(m.installed_handler_count() == 1, "SIG_IGN is not an installed handler");
}

void a_signal_to_a_live_thread_is_queued_and_taken_once() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr), werr);
  m.set_ids(GuestProcessIds::kPid, GuestProcessIds::kMainTid);

  // Signal 0 stays the existence probe even for a live thread: it must answer 0 and queue nothing.
  exec::MachineState probe = State(sc::nr::kTkill);
  probe.r[0] = GuestProcessIds::kMainTid;
  probe.r[1] = 0;
  CHECK_CTX(m.Handle(probe, 0).r0 == 0, "signal 0 is an existence probe");
  CHECK_CTX(m.signals_queued() == 0, "a probe queues nothing");
  CHECK_CTX(m.TakePendingSignal(GuestProcessIds::kMainTid) == 0, "and nothing is waiting");

  // A real signal, with a handler installed.
  const std::uint32_t act[5] = {0x005f'ab59u, 0x1400'0000u, 0x001a'7240u, 0, 0};
  CHECK(window->Write(0x300, act, sizeof act));
  exec::MachineState sa = State(sc::nr::kRtSigaction);
  sa.r[0] = 30;
  sa.r[1] = 0x300;
  CHECK(m.Handle(sa, 0).r0 == 0);

  exec::MachineState kill = State(sc::nr::kTkill);
  kill.r[0] = GuestProcessIds::kMainTid;
  kill.r[1] = 30;
  const sc::Outcome o = m.Handle(kill, 0);
  // 0, which is the kernel's answer *and* the truth: the signal is pending, not delivered yet.
  CHECK_CTX(o.r0 == 0, "tkill of a live thread with a handler must answer 0, not -ENOSYS: " + o.detail);
  CHECK_CTX(o.verdict == sc::Verdict::kEmulated, sc::Name(o.verdict));
  CHECK_CTX(m.signals_queued() == 1, "one signal queued");

  // Taken exactly once: a second look must not re-enter the handler (Mono would deadlock on a
  // suspend it never asked for the second time).
  CHECK_CTX(m.TakePendingSignal(GuestProcessIds::kMainTid) == 30, "the queued signal is taken");
  CHECK_CTX(m.TakePendingSignal(GuestProcessIds::kMainTid) == 0, "and not taken again");

  // A thread that does not exist is still ESRCH — a queued signal would be a lie.
  exec::MachineState dead = State(sc::nr::kTkill);
  dead.r[0] = 99999;
  dead.r[1] = 30;
  CHECK_CTX(m.Handle(dead, 0).r0 == -static_cast<std::int32_t>(sc::err::kEsrch), "no such thread is -ESRCH");

  // SIGKILL and SIGSTOP cannot be caught; this build does not run default actions, so they are refused
  // by name rather than queued into a delivery that would never happen.
  for (const int uncatchable : {9, 19}) {
    exec::MachineState k = State(sc::nr::kTkill);
    k.r[0] = GuestProcessIds::kMainTid;
    k.r[1] = Uncatchable(uncatchable);
    CHECK_CTX(m.Handle(k, 0).r0 == -static_cast<std::int32_t>(sc::err::kEnosys),
              "signal " + std::to_string(uncatchable) + " cannot be caught and must be refused by name");
  }

  // Outside the ABI's 1..64 there is no signal to queue; the kernel answers EINVAL, not 0.
  for (const int bad : {65, -1}) {
    exec::MachineState k = State(sc::nr::kTkill);
    k.r[0] = GuestProcessIds::kMainTid;
    k.r[1] = static_cast<std::uint32_t>(bad);
    CHECK_CTX(m.Handle(k, 0).r0 == -static_cast<std::int32_t>(sc::err::kEinval),
              "signal " + std::to_string(bad) + " is EINVAL");
  }
}

/// `rt_sigreturn` (173) closes a delivery. Without one in progress it is refused by name — answering 0
/// would resume the handler's own `svc` in place, which is an infinite loop rather than an error.
void sigreturn_outside_a_delivery_is_refused_by_name() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr), werr);

  exec::MachineState s = State(sc::nr::kRtSigreturn);
  const sc::Outcome o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEnosys), std::to_string(o.r0));
  CHECK_CTX(o.detail.find("rt_sigreturn") != std::string::npos, o.detail);
}

/// `rt_sigsuspend` (179) installs its temporary mask, sleeps until a deliverable signal is queued, and
/// returns -EINTR; the signal stays pending for the delivery path. Mono's suspend handler uses this to wait
/// for its restart signal; before C26's follow-up the call was refused with ENOSYS and the GC handshake
/// never settled (Cars 3, `svc #179` in the runtime log).
void sigsuspend_waits_for_a_signal_and_returns_eintr() {
  sc::Monitor m;
  auto window = Window();
  if (!window) return;
  m.set_memory(window.get());
  std::string werr;
  CHECK_CTX(window->Map(0, 0x8000, mem::kRead | mem::kWrite, &werr), werr);
  m.set_ids(GuestProcessIds::kPid, GuestProcessIds::kMainTid);
  runtime::FutexTable ft;
  m.set_futex_table(&ft);
  runtime::GuestThread t;
  t.tid = GuestProcessIds::kMainTid;
  m.set_thread_lookup([&t](std::int32_t tid) { return tid == t.tid ? &t : nullptr; });
  const std::uint64_t mask = 0;  // everything deliverable
  CHECK(window->Write(0x400, &mask, sizeof mask));
  exec::MachineState s = State(sc::nr::kRtSigsuspend);
  s.r[0] = 0x400;
  s.r[1] = 8;
  sc::Outcome o = m.Begin(s, 0, &t);
  CHECK_CTX(o.verdict == sc::Verdict::kBlocking, sc::Name(o.verdict));
  CHECK_CTX(t.sigsuspend_restore && t.signal_mask == mask, "the temporary mask is installed for the suspend");

  sc::Outcome got;
  std::thread waiter([&] { got = o.block(); });
  m.QueueSignal(GuestProcessIds::kMainTid, 30);
  waiter.join();
  CHECK_CTX(got.r0 == -static_cast<std::int32_t>(sc::err::kEintr), "sigsuspend returns -EINTR");
  CHECK_CTX(m.HasDeliverablePending(GuestProcessIds::kMainTid, mask), "the signal is still pending for delivery");
  CHECK_CTX(m.TakePendingSignal(GuestProcessIds::kMainTid) == 30, "taken once");
  t.sigsuspend_restore = false;  // no handler ran in this lane; a real delivery restores it at rt_sigreturn
}

int main() {
  frame_takes_the_number_from_r7_not_the_encoding();
  r7_is_the_number_not_a_masked_remainder();
  return_value_and_the_carry_bit();
  known_numbers_are_answered_or_refused_not_guessed();
  exit_group_is_a_verdict_not_a_value();
  tls_travels_on_the_state_field();
  cacheflush_reaches_the_tier_that_owns_the_cache();
  mprotect_inside_the_window_is_granted_and_recorded_as_not_enforced();
  mprotect_outside_the_window_is_refused_with_the_range_and_enomem();
  mprotect_over_a_hole_in_the_window_names_the_unmapped_page();
  guest_numbers_are_not_host_numbers();
  the_error_range_is_the_discriminator();
  the_names_are_what_the_headers_call_them();
  execve_is_denied_with_the_measured_errno();
  the_policy_is_echoed_not_assumed();
  the_tally_is_per_monitor_and_in_first_seen_order();
  mmap2_serves_real_anonymous_pages();
  mmap2_in_a_4gib_window_stays_below_the_ceiling();
  mmap2_places_top_down_like_the_kernel();
  munmap_releases_served_pages();
  mremap_grows_moves_and_shrinks_like_a_kernel();
  console_writes_are_captured_and_answered_as_written();
  futex_answers_the_single_threaded_truth();
  sigaction_and_faccessat_answer_their_facts();
  signal_dispositions_are_what_the_guest_installed();
  a_signal_to_a_live_thread_is_queued_and_taken_once();
  sigsuspend_waits_for_a_signal_and_returns_eintr();
  sigreturn_outside_a_delivery_is_refused_by_name();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
