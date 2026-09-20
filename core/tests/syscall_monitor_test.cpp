/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C07's monitor, tested as a decision table. The values asserted here are the measured ones from
 * `tests/corpus/syscall-abi-arm.txt` (an `armeabi-v7a` process on an SM-X710), not the ones from
 * recollection: which number the guest puts in r7, what a real kernel answers for an unknown number, and
 * what the carry flag is doing — the last of which turned out to be nothing, and is a test rather than a
 * comment because a future edit "fixing" it back to the textbook rule would be a silent behaviour change.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/syscall/monitor.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace rb;
namespace sc = rb::syscall;
namespace exec = rb::exec;
namespace diag = rb::diag;
namespace isa = rb::isa;

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

void r7_is_masked_the_way_the_kernel_masks_it() {
  exec::MachineState s = State(0x0010'0000u | sc::nr::kGetpid);
  const sc::Frame f = sc::Frame::Capture(s, 0);
  CHECK_CTX(f.number == sc::nr::kGetpid, "0x" + std::to_string(f.number));
  // Above the table even after masking: refused, not wrapped into something that exists.
  exec::MachineState hi = State(0x00ff'fff0u);
  sc::Monitor m;
  const sc::Outcome o = m.Classify(sc::Frame::Capture(hi, 0), hi);
  CHECK_CTX(o.verdict == sc::Verdict::kUnsupported, sc::Name(o.verdict));
  CHECK_CTX(o.r0 == -static_cast<std::int32_t>(sc::err::kEnosys),
            std::to_string(o.r0) + " want -38, the value measured from hardware for this exact case");
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

int main() {
  frame_takes_the_number_from_r7_not_the_encoding();
  r7_is_masked_the_way_the_kernel_masks_it();
  return_value_and_the_carry_bit();
  known_numbers_are_answered_or_refused_not_guessed();
  exit_group_is_a_verdict_not_a_value();
  tls_travels_on_the_state_field();
  cacheflush_reaches_the_tier_that_owns_the_cache();
  guest_numbers_are_not_host_numbers();
  the_error_range_is_the_discriminator();
  the_names_are_what_the_headers_call_them();
  execve_is_denied_with_the_measured_errno();
  the_policy_is_echoed_not_assumed();
  the_tally_is_per_monitor_and_in_first_seen_order();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
