/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * M2 Task 3: per-thread guest contexts (`rb::runtime::GuestThread`) under one Global Guest Lock
 * (`rb::runtime::Ggl`), on both tiers.
 *
 * Each row builds its own small process (guest memory, one `AbiContext`, one monitor, one executor per tier)
 * and drives it from real host threads (`std::thread`), each bound to its own `GuestThread`. The blobs are
 * planted Thumb code, assembled with the NDK's clang (`--target=armv7a-linux-androideabi26`) and read back
 * with `llvm-objdump -d`; the halfwords below are that listing.
 *
 * Every row that waits on another thread has a deadline, and a watchdog per row turns a hang into a named
 * `HANG: <row>` line and a non-zero exit instead of a suite that never finishes.
 */
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/signal_frame.h"
#include "rb/runtime/guest_process.h"
#include "rb/runtime/guest_thread.h"
#include "rb/syscall/monitor.h"

namespace {

using namespace rb;
using runtime::GuestThread;
using Clock = std::chrono::steady_clock;

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

#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

/// One per row: if the row is still running after `limit`, names it and ends the process (a deadlock in the
/// GGL would otherwise hang the whole suite with no line saying where).
class Watchdog {
 public:
  Watchdog(std::string name, std::chrono::seconds limit) : name_{std::move(name)} {
    th_ = std::thread([this, limit] {
      std::unique_lock<std::mutex> lk(mu_);
      if (!cv_.wait_for(lk, limit, [this] { return done_; })) {
        std::fprintf(stderr, "HANG: %s — still running after %lld s: a guest thread is deadlocked, starved or never woken\n",
                     name_.c_str(), static_cast<long long>(limit.count()));
        std::printf("%d checks, %d failures, 1 hang (%s)\n", checks, failures, name_.c_str());
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(3);
      }
    });
  }
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_ = true;
    }
    cv_.notify_all();
    th_.join();
  }

 private:
  std::string name_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread th_;
};

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  while ((1L << shift) < ps) ++shift;
  return shift;
}

constexpr exec::GuestAddr kReturn = 0x00c0'0000u;
constexpr exec::GuestAddr kCode = 0x0100'0000u;
constexpr exec::GuestAddr kData = 0x0101'0000u;
constexpr exec::GuestAddr kStacks = 0x0200'0000u;
constexpr std::uint32_t kStackBytes = 0x1'0000u;
constexpr int kStackSlots = 8;  ///< slot 0 is the context's main thread
constexpr std::int32_t kPid = 5000, kMainTid = 5001;

/// The listing (see the header): offsets into the code page.
const std::uint16_t kBlobs[] = {
    // 0x00 ldrex_loop(r0 = word, r1 = count): ldrex/adds/strex/cmp/bne, subs/bne, bx lr
    0xe850, 0x2f00, 0x1c52, 0xe840, 0x2300, 0x2b00, 0xd1f8, 0x1e49, 0xd1f6, 0x4770,
    // 0x14 kuser_loop(r0 = word, r1 = count): __kuser_cmpxchg(old, old + 1, word) until it stores
    0xb570, 0x4604, 0x460d, 0xf640, 0x76c0, 0xf6cf, 0x76ff, 0x6820, 0x1c41, 0x4622, 0x47b0, 0x2800, 0xd1f9,
    0x1e6d, 0xd1f7, 0xbd70,
    // 0x34 busy(r0 = iterations, r1 = flag): *flag = 1, then subs/bne r0 times — no syscall, no host call
    0x2201, 0x600a, 0x1e40, 0xd1fd, 0x4770,
    // 0x3e ten(): movs r0, #0; 8 x adds r0, #1; bx lr — ten instructions, returns 8
    0x2000, 0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0x3001, 0x4770,
    // 0x52 hostcall(r0 = slot address): push {r4, lr}; blx r0; pop {r4, pc}
    0xb510, 0x4780, 0xbd10,
    // 0x58 sleeper(r0 = timespec): nanosleep(r0, NULL) via svc (r7 = 162)
    0xb580, 0x2100, 0x27a2, 0xdf00, 0xbd80,
    // 0x62 gettid(): svc with r7 = 224
    0xb580, 0x27e0, 0xdf00, 0xbd80,
};
constexpr exec::GuestAddr kLdrexLoop = kCode + 0x00 + 1;
constexpr exec::GuestAddr kKuserLoop = kCode + 0x14 + 1;
constexpr exec::GuestAddr kBusy = kCode + 0x34 + 1;
constexpr exec::GuestAddr kTen = kCode + 0x3e + 1;
constexpr exec::GuestAddr kHostCall = kCode + 0x52 + 1;
constexpr exec::GuestAddr kSleeper = kCode + 0x58 + 1;
constexpr exec::GuestAddr kGettid = kCode + 0x62 + 1;

const char* TierName(int tier) { return tier == 1 ? "I" : "J"; }

struct Rig {
  std::unique_ptr<mem::GuestMemory> mem;
  std::unique_ptr<abi::AbiContext> abi;
  std::unique_ptr<syscall::Monitor> mon;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::JitExecutor> jit;
  bool ok = false;
};

Rig Build(int tier) {
  Rig r;
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  r.mem = mem::GuestMemory::Create(64u << 20, model, &err);
  if (!r.mem) {
    fail("guest memory", __LINE__, err);
    return r;
  }
  mem::GuestMemory& m = *r.mem;
  if (!m.Map(kCode, mem::kGuestPage, mem::kRead | mem::kWrite, &err) || !m.Write(kCode, kBlobs, sizeof kBlobs) ||
      !m.Protect(kCode, mem::kGuestPage, mem::kRead | mem::kExec, &err) ||
      !m.Map(kData, mem::kGuestPage, mem::kRead | mem::kWrite, &err) ||
      !m.Map(kStacks, kStackBytes * kStackSlots, mem::kRead | mem::kWrite, &err)) {
    fail("mapping the rig", __LINE__, err);
    return r;
  }
  r.abi = std::make_unique<abi::AbiContext>(m, kStacks + kStackBytes, kReturn);
  r.mon = std::make_unique<syscall::Monitor>();
  r.mon->set_memory(&m);
  r.mon->set_ids(kPid, kMainTid);
  r.abi->set_monitor(r.mon.get());
  r.interp = std::make_unique<exec::InterpExecutor>(m, kReturn, r.abi.get());
  r.jit = std::make_unique<exec::JitExecutor>(m, kReturn, r.abi.get());
  if (tier == 1) {
    r.abi->set_executor(*r.interp);
  } else {
    r.abi->set_executor(*r.jit);
  }
  r.ok = true;
  return r;
}

/// A worker's guest thread: its own stack slot, its own tid, entered in Thumb at the return sentinel.
void Prepare(GuestThread& t, int slot, std::int32_t tid) {
  t.stack_lo = kStacks + kStackBytes * static_cast<std::uint32_t>(slot);
  t.stack_hi = t.stack_lo + kStackBytes;
  t.state.r[13] = t.stack_hi;
  t.state.pc = kReturn;
  t.state.r[15] = kReturn;
  t.state.mode = isa::Mode::Thumb;
  t.tid = tid;
}

std::uint32_t Word(Rig& r, exec::GuestAddr at) {
  std::uint32_t v = 0xdead'dead;
  if (!r.mem->Read(at, &v, 4)) return 0xdead'dead;
  return v;
}

struct CallOut {
  abi::Ret32 ret{};
  exec::RunResult rr{};
  Clock::time_point start{}, end{};
  bool done = false;
};

/// A host thread bound to `t` that makes one guest call.
std::thread Worker(Rig& r, GuestThread& t, exec::GuestAddr fn, abi::Args32 args, unsigned budget, CallOut& out) {
  return std::thread([&r, &t, fn, args, budget, &out] {
    if (!runtime::BindCurrentHostThread(t)) {
      std::fprintf(stderr, "FATAL: a fresh GuestThread could not be bound (line %d)\n", __LINE__);
      std::abort();
    }
    out.start = Clock::now();
    out.ret = r.abi->CallGuest(fn, args, abi::Shorty::For("I"), budget, &out.rr);
    out.end = Clock::now();
    out.done = true;
  });
}

std::string Why(const CallOut& c) {
  return "reason " + std::to_string(static_cast<int>(c.rr.reason)) + ": " + c.rr.detail;
}

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds limit) {
  const auto until = Clock::now() + limit;
  while (Clock::now() < until) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return cond();
}

// ---- rows

/// The shared-counter rows. Both workers are released together from a start gate (each binds its own
/// GuestThread first, then waits until both are ready), so their calls start within microseconds of each
/// other. The row then proves the calls really interleaved rather than ran back to back:
///   - the two calls overlap in wall time (a.start < b.end && b.start < a.end), and
///   - at least one slice boundary found the *other* thread waiting for the GGL (`contended_slices()`), i.e.
///     a boundary handed guest execution to the other thread mid-loop.
/// With that established, an exact total is the atomicity claim. Iterations per thread: 100,000 on Tier I
/// (the brief's number; ~700k IR steps per thread, ~22 slices in all) and 1,000,000 on Tier J — at Tier J's
/// speed 100,000 iterations are ~1 ms of work, too short to guarantee overlap against thread start-up skew.
void SharedCounterRow(int tier, const char* row, exec::GuestAddr blob, exec::GuestAddr word) {
  const std::string name = std::string{row} + "/" + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(120));
  Rig r = Build(tier);
  if (!r.ok) return;
  const std::uint32_t zero = 0;
  r.mem->Write(word, &zero, 4);
  const std::uint32_t per_thread = tier == 1 ? 100'000u : 1'000'000u;
  GuestThread a, b;
  Prepare(a, 1, 6001);
  Prepare(b, 2, 6002);
  CallOut ca, cb;
  std::atomic<int> ready{0};
  const abi::Args32 args{abi::Value32::Ptr(word), abi::Value32::U32(per_thread)};
  const auto worker = [&](GuestThread& t, CallOut& out) {
    return std::thread([&, args] {
      if (!runtime::BindCurrentHostThread(t)) {
        std::fprintf(stderr, "FATAL: a fresh GuestThread could not be bound (line %d)\n", __LINE__);
        std::abort();
      }
      ready.fetch_add(1);
      while (ready.load() < 2) std::this_thread::yield();
      out.start = Clock::now();
      out.ret = r.abi->CallGuest(blob, args, abi::Shorty::For("I"), 0xc000'0000u, &out.rr);
      out.end = Clock::now();
      out.done = true;
    });
  };
  std::thread ta = worker(a, ca);
  std::thread tb = worker(b, cb);
  ta.join();
  tb.join();
  const std::uint32_t got = Word(r, word);
  CHECK_CTX(ca.rr.reason == exec::StopReason::Returned, Why(ca));
  CHECK_CTX(cb.rr.reason == exec::StopReason::Returned, Why(cb));
  CHECK_CTX(got == 2 * per_thread, name + ": the word is " + std::to_string(got) + ", want " +
                                       std::to_string(2 * per_thread));
  const bool overlap = ca.start < cb.end && cb.start < ca.end;
  CHECK_CTX(overlap, name + ": the two calls did not overlap in time (A " + std::to_string(Ms(ca.start, ca.end)) +
                         " ms, B " + std::to_string(Ms(cb.start, cb.end)) + " ms) — nothing interleaved");
  CHECK_CTX(r.abi->contended_slices() >= 1,
            name + ": no slice boundary found the other thread waiting (" + std::to_string(r.abi->slices()) +
                " slices, 0 contended) — the threads never handed the GGL to each other");
  CHECK(r.abi->ggl().misuses() == 0);
  CHECK(!r.abi->ggl().HeldByMe());
  std::fprintf(stderr,
               "# Tier %s: %s counter %u after 2 x %u; calls overlapped %s (A %.1f ms, B %.1f ms); %llu slices, "
               "%llu contended\n",
               TierName(tier), row, got, per_thread, overlap ? "yes" : "NO", Ms(ca.start, ca.end), Ms(cb.start, cb.end),
               static_cast<unsigned long long>(r.abi->slices()),
               static_cast<unsigned long long>(r.abi->contended_slices()));
}

/// Two host threads, each on its own GuestThread, increment one guest word with an ldrex/add/strex loop:
/// the GGL preempts them at arbitrary instructions (every slice), and the total is exact — a reservation
/// never survives a switch (both tiers clear it at Run entry).
void two_host_threads_share_one_process(int tier) {
  SharedCounterRow(tier, "two_host_threads_share_one_process", kLdrexLoop, kData);
}

/// The same row through `__kuser_cmpxchg` (0xffff0fc0): the helper's read-then-write is atomic because it
/// runs under the GGL, and a switch between the guest's load of the old value and the helper makes the
/// helper fail and the guest retry.
void kuser_cmpxchg_shared_by_two_threads(int tier) {
  SharedCounterRow(tier, "kuser_cmpxchg_shared_by_two_threads", kKuserLoop, kData + 4);
}

/// Thread A spins 50M instructions (Tier I: 5M, see below) with no syscall and no host call; thread B's ten-instruction call, made
/// while A spins, completes within 2 s — and before A does — because the slice boundary hands the GGL over.
void busy_thread_does_not_starve(int tier) {
  const std::string name = std::string{"busy_thread_does_not_starve/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(180));
  Rig r = Build(tier);
  if (!r.ok) return;
  const exec::GuestAddr flag = kData + 0x100;
  const std::uint32_t zero = 0;
  r.mem->Write(flag, &zero, 4);
  GuestThread a, b;
  Prepare(a, 1, 6001);
  Prepare(b, 2, 6002);
  CallOut ca, cb;
  // Tier J: 25M iterations of subs/bne = 50M instructions (the brief's number). Tier I: 2.5M iterations =
  // 5M instructions — Tier I translates every block on every execution (no code cache), about 0.6M
  // instructions/s on the tablet, so 50M took 87 s; 5M is still ~25 slices of spinning, which is all the row
  // needs. The budget is far above either (Tier I counts IR steps).
  const std::uint32_t iterations = tier == 1 ? 2'500'000u : 25'000'000u;
  std::thread ta = Worker(r, a, kBusy, {abi::Value32::U32(iterations), abi::Value32::Ptr(flag)}, 0xc000'0000u, ca);
  const bool spinning = WaitFor([&] { return Word(r, flag) == 1u; }, std::chrono::milliseconds(10'000));
  CHECK_CTX(spinning, name + ": thread A never started its loop");
  std::thread tb = Worker(r, b, kTen, {}, 1000, cb);
  tb.join();
  ta.join();
  CHECK_CTX(cb.rr.reason == exec::StopReason::Returned && cb.ret.w() == 8u, Why(cb));
  CHECK_CTX(ca.rr.reason == exec::StopReason::Returned && ca.ret.w() == 0u, Why(ca));
  const double b_ms = Ms(cb.start, cb.end);
  CHECK_CTX(b_ms < 2000.0, name + ": B's ten instructions took " + std::to_string(b_ms) + " ms (bound 2000)");
  CHECK_CTX(cb.end < ca.end, name + ": B finished " + std::to_string(Ms(ca.end, cb.end)) +
                                 " ms after A — B waited for A instead of being let in at a slice boundary");
  std::fprintf(stderr, "# Tier %s: busy A %.1f ms; B (10 instructions) %.3f ms, done %.1f ms before A\n",
               TierName(tier), Ms(ca.start, ca.end), b_ms, Ms(cb.end, ca.end));
}

/// A host slot blocks on a host condition variable until thread B has run guest code. That only completes
/// if the GGL is released around host calls; if it were not, the slot gives up after 5 s and refuses by
/// name, so the row fails with that name instead of deadlocking.
void ggl_is_released_in_host_calls(int tier) {
  const std::string name = std::string{"ggl_is_released_in_host_calls/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Rig r = Build(tier);
  if (!r.ok) return;
  std::mutex mu;
  std::condition_variable cv;
  bool in_slot = false, b_ran = false;
  constexpr std::uint32_t kSlot = 300;
  r.abi->Bind(kSlot, [&](exec::MachineState& s, std::string* err) {
    std::unique_lock<std::mutex> lk(mu);
    in_slot = true;
    cv.notify_all();
    if (!cv.wait_for(lk, std::chrono::seconds(5), [&] { return b_ran; })) {
      *err = "thread B never ran guest code while this host call waited 5 s: the GGL was held across the host call";
      return false;
    }
    s.r[0] = 0x55;
    return true;
  }, abi::AbiContext::SlotLock::kReleasesGgl);
  GuestThread a, b;
  Prepare(a, 1, 6001);
  Prepare(b, 2, 6002);
  CallOut ca, cb;
  std::thread ta = Worker(r, a, kHostCall, {abi::Value32::Ptr(r.abi->SlotFor(kSlot))}, 10'000, ca);
  {
    std::unique_lock<std::mutex> lk(mu);
    CHECK_CTX(cv.wait_for(lk, std::chrono::seconds(5), [&] { return in_slot; }), name + ": A never reached the slot");
  }
  std::thread tb([&] {
    if (!runtime::BindCurrentHostThread(b)) {
      std::fprintf(stderr, "FATAL: a fresh GuestThread could not be bound (line %d)\n", __LINE__);
      std::abort();
    }
    cb.start = Clock::now();
    cb.ret = r.abi->CallGuest(kTen, {}, abi::Shorty::For("I"), 1000, &cb.rr);
    cb.end = Clock::now();
    std::lock_guard<std::mutex> lk(mu);
    b_ran = true;
    cv.notify_all();
  });
  tb.join();
  ta.join();
  CHECK_CTX(cb.rr.reason == exec::StopReason::Returned && cb.ret.w() == 8u, Why(cb));
  CHECK_CTX(ca.rr.reason == exec::StopReason::Returned && ca.ret.w() == 0x55u, Why(ca));
}

/// `nanosleep` (svc 162) is a blocking syscall: the monitor returns `Verdict::kBlocking` with the wait as a
/// callable, the bridge runs it with the GGL released, and thread B's call completes while A sleeps.
void blocking_syscall_releases_the_ggl(int tier) {
  const std::string name = std::string{"blocking_syscall_releases_the_ggl/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Rig r = Build(tier);
  if (!r.ok) return;
  const exec::GuestAddr ts = kData + 0x200;
  const std::uint32_t req[2] = {0, 300'000'000u};  // 32-bit timespec: 0 s, 300 ms
  r.mem->Write(ts, req, sizeof req);
  GuestThread a, b;
  Prepare(a, 1, 6001);
  Prepare(b, 2, 6002);
  CallOut ca, cb;
  std::thread ta = Worker(r, a, kSleeper, {abi::Value32::Ptr(ts)}, 10'000, ca);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  std::thread tb = Worker(r, b, kTen, {}, 1000, cb);
  tb.join();
  ta.join();
  CHECK_CTX(ca.rr.reason == exec::StopReason::Returned && ca.ret.w() == 0u, Why(ca) + " r0=" + Hex(ca.ret.w()));
  CHECK_CTX(cb.rr.reason == exec::StopReason::Returned && cb.ret.w() == 8u, Why(cb));
  CHECK_CTX(Ms(ca.start, ca.end) >= 290.0, name + ": A slept " + std::to_string(Ms(ca.start, ca.end)) + " ms");
  CHECK_CTX(cb.end < ca.end, name + ": B finished after A — the GGL was held across the sleep");
  CHECK_CTX(Ms(cb.start, cb.end) < 200.0, name + ": B took " + std::to_string(Ms(cb.start, cb.end)) + " ms");

  // A bad timespec is the kernel's EINVAL, answered without blocking.
  const std::uint32_t bad[2] = {0, 1'000'000'000u};
  r.mem->Write(ts, bad, sizeof bad);
  CallOut cc;
  std::thread tc = Worker(r, a, kSleeper, {abi::Value32::Ptr(ts)}, 10'000, cc);
  tc.join();
  CHECK_CTX(cc.rr.reason == exec::StopReason::Returned && cc.ret.i() == -22, Why(cc) + " r0=" + Hex(cc.ret.w()));
}

/// The monitor's side of the blocking contract, without a bridge: `Begin` hands back the wait, `Handle`
/// (the single-threaded entry) runs it in place and answers.
void monitor_blocking_verdict(void) {
  Watchdog wd("monitor_blocking_verdict", std::chrono::seconds(30));
  syscall::Monitor m;
  exec::MachineState s{};
  s.r[7] = 158;  // sched_yield
  syscall::Outcome o = m.Begin(s, 0, nullptr);
  CHECK_CTX(o.verdict == syscall::Verdict::kBlocking && static_cast<bool>(o.block), o.detail);
  if (o.block) {
    const syscall::Outcome done = o.block();
    CHECK(done.verdict == syscall::Verdict::kEmulated && done.r0 == 0);
    m.Finish(s, done);
    CHECK(s.r[0] == 0u);
  }
  s.r[0] = 0x1234;
  s.r[7] = 158;
  o = m.Handle(s, 0);
  CHECK_CTX(o.verdict == syscall::Verdict::kEmulated && s.r[0] == 0u, o.detail);
  CHECK(std::string{syscall::Name(syscall::Verdict::kBlocking)} == "blocking");
}

/// A bare bridge (no adopter: only a `GuestProcess` adopts): a host thread with no binding, on a context whose
/// main thread another live host thread holds, is refused by name — never silently run on somebody else's
/// registers. (With a `GuestProcess` the same host thread is adopted instead: `art_thread_is_adopted`.)
void unbound_host_thread_is_refused(int tier) {
  const std::string name = std::string{"unbound_host_thread_is_refused/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(30));
  Rig r = Build(tier);
  if (!r.ok) return;
  exec::RunResult rr{};
  const abi::Ret32 mine = r.abi->CallGuest(kTen, {}, abi::Shorty::For("I"), 1000, &rr);  // claims main here
  CHECK_CTX(rr.reason == exec::StopReason::Returned && mine.w() == 8u, rr.detail);
  CHECK(runtime::BoundHere(r.abi->main_thread()));
  CallOut c;
  std::thread t([&] {
    c.ret = r.abi->CallGuest(kTen, {}, abi::Shorty::For("I"), 1000, &c.rr);
    c.done = true;
  });
  t.join();
  CHECK_CTX(c.rr.reason == exec::StopReason::Internal &&
                c.rr.detail.find("host thread not bound to a guest thread (this bridge has no adopter") !=
                    std::string::npos,
            Why(c));
}

// ------------------------------------------------------------------------------------ M2 Task 5: bindings
//
// The controller's rulings on the binding itself: (b) a GuestThread bound to another live host thread is never
// stolen; (d) a host thread's exit releases its binding, so a dead host thread never blocks a later one; (c) a
// GuestThread destroyed while a host thread is bound to it leaves that host thread unbound, not dangling.

/// A start/stop gate for a helper host thread.
struct Gate {
  std::mutex mu;
  std::condition_variable cv;
  int stage = 0;
  void Set(int s) {
    {
      std::lock_guard<std::mutex> lk(mu);
      stage = s;
    }
    cv.notify_all();
  }
  bool Wait(int s) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::seconds(10), [&] { return stage >= s; });
  }
};

void bind_refuses_a_live_owner_and_frees_a_dead_one(void) {
  Watchdog wd("bind_refuses_a_live_owner_and_frees_a_dead_one", std::chrono::seconds(30));
  GuestThread g;
  Gate gate;
  bool bound_there = false;
  std::thread x([&] {
    bound_there = runtime::BindCurrentHostThread(g);
    gate.Set(1);
    gate.Wait(2);  // stays alive (and bound) until the main thread has tried
  });
  CHECK(gate.Wait(1));
  CHECK(bound_there);
  // (b): bound to a live host thread -> refused, and nothing changed on either side.
  CHECK(!runtime::BindCurrentHostThread(g));
  CHECK(runtime::CurrentGuestThread() != &g);
  CHECK(g.host.load() != std::this_thread::get_id());
  gate.Set(2);
  x.join();
  // (d): that host thread exited; its binding went with it, so this one can take the GuestThread.
  CHECK(g.host.load() == std::thread::id{});
  CHECK(runtime::BindCurrentHostThread(g));
  CHECK(runtime::BoundHere(g));
  runtime::UnbindCurrentHostThread();
  CHECK(runtime::CurrentGuestThread() == nullptr);
}

void destroyed_guest_thread_leaves_no_dangling_binding(void) {
  Watchdog wd("destroyed_guest_thread_leaves_no_dangling_binding", std::chrono::seconds(30));
  auto g = std::make_unique<GuestThread>();
  Gate gate;
  bool before = false, after_is_null = false;
  std::thread x([&] {
    before = runtime::BindCurrentHostThread(*g) && runtime::CurrentGuestThread() == g.get();
    gate.Set(1);
    gate.Wait(2);
    after_is_null = runtime::CurrentGuestThread() == nullptr;  // (c): reads as unbound, not a freed pointer
  });
  CHECK(gate.Wait(1));
  g.reset();  // destroyed on this thread while `x` is bound to it
  gate.Set(2);
  x.join();
  CHECK(before);
  CHECK(after_is_null);
}

/// Each guest thread answers `gettid` with its own tid; the main thread with the monitor's.
void each_thread_has_its_own_tid(int tier) {
  const std::string name = std::string{"each_thread_has_its_own_tid/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(30));
  Rig r = Build(tier);
  if (!r.ok) return;
  GuestThread a, b;
  Prepare(a, 1, 6001);
  Prepare(b, 2, 6002);
  CallOut ca, cb;
  std::thread ta = Worker(r, a, kGettid, {}, 1000, ca);
  std::thread tb = Worker(r, b, kGettid, {}, 1000, cb);
  ta.join();
  tb.join();
  exec::RunResult rr{};
  const abi::Ret32 m = r.abi->CallGuest(kGettid, {}, abi::Shorty::For("I"), 1000, &rr);
  CHECK_CTX(ca.ret.i() == 6001, Why(ca) + " tid=" + std::to_string(ca.ret.i()));
  CHECK_CTX(cb.ret.i() == 6002, Why(cb) + " tid=" + std::to_string(cb.ret.i()));
  CHECK_CTX(m.i() == kMainTid, rr.detail + " tid=" + std::to_string(m.i()));
}

/// The GGL's contended handoff: two host threads pass the lock back and forth (each re-queues right after
/// releasing, so every acquisition is a handoff from the other). The latency is release -> the waiter's
/// `Acquire` returning. Task 3 measured ~15 ms per handoff on a dozing tablet; awake it must be well under a
/// millisecond, or every guest futex/slice handoff costs a frame.
void ggl_handoff_latency(void) {
  Watchdog wd("ggl_handoff_latency", std::chrono::seconds(30));
  runtime::Ggl g;
  constexpr int kRounds = 400;
  std::atomic<std::int64_t> released_at{0};
  std::vector<double> lat_a, lat_b;
  std::atomic<int> ready{0};
  const auto now_ns = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  };
  const auto body = [&](std::vector<double>& lat) {
    ready.fetch_add(1);
    while (ready.load() < 2) std::this_thread::yield();
    for (int i = 0; i < kRounds; ++i) {
      g.Acquire();
      const std::int64_t t = now_ns();
      const std::int64_t r = released_at.load();
      if (r != 0) lat.push_back(static_cast<double>(t - r) / 1e6);
      released_at.store(now_ns());
      g.Release();
    }
  };
  std::thread a([&] { body(lat_a); });
  std::thread b([&] { body(lat_b); });
  a.join();
  b.join();
  std::vector<double> all = lat_a;
  all.insert(all.end(), lat_b.begin(), lat_b.end());
  std::sort(all.begin(), all.end());
  const double median = all.empty() ? 1e9 : all[all.size() / 2];
  const double p90 = all.empty() ? 1e9 : all[all.size() * 9 / 10];
  std::fprintf(stderr, "#   ggl_handoff_latency: %zu handoffs, median %.3f ms, p90 %.3f ms, max %.3f ms\n", all.size(),
               median, p90, all.empty() ? 0.0 : all.back());
  CHECK_CTX(median < 1.0, "median GGL handoff " + std::to_string(median) + " ms (bound 1 ms)");
  CHECK(g.misuses() == 0u);
}

// ------------------------------------------------------------------------------------ M2 Task 4: guest pthreads
//
// A `GuestProcess` booting the device's own libc and loading `libpthread_fixture.so` (tests/corpus/
// pthread_fixture.c). Every fixture call creates real guest threads through libc's pthread_create (clone
// 0x003d0f00 -> a host std::thread per guest thread), and its answer must equal the same call run natively on
// the tablet (tests/corpus/oracle-pthread.txt, pushed by tools/run_engine_tests.sh). Each row runs under a
// 20 s watchdog that prints `HANG: <row>`.

std::vector<std::uint8_t> ReadFile(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return {};
  std::vector<std::uint8_t> out;
  std::uint8_t buf[65536];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

/// "name=value" and "name:rc=value" lines of the oracle file, by name.
std::map<std::string, std::string> Oracle() {
  std::map<std::string, std::string> rows;
  const std::vector<std::uint8_t> bytes = ReadFile(RB_PTHREAD_ORACLE_PATH);
  std::string text(bytes.begin(), bytes.end());
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t rc = line.find(":rc=");
    const std::size_t eq = rc != std::string::npos ? rc + 3 : line.find('=');
    if (eq == std::string::npos) continue;
    rows[line.substr(0, rc != std::string::npos ? rc : eq)] = line.substr(eq + 1);
  }
  return rows;
}

struct Images {
  std::vector<std::uint8_t> libc, libm, libz, fixture;
  bool ok = false;
};

const Images& PthreadImages() {
  static Images im = [] {
    Images i;
    i.libc = ReadFile(RB_DEVICE_LIBC_PATH);
    i.libm = ReadFile(RB_GUEST_LIBM_PATH);
    i.libz = ReadFile(RB_GUEST_LIBZ_PATH);
    i.fixture = ReadFile(RB_PTHREAD_FIXTURE_PATH);
    i.ok = !i.libc.empty() && !i.libm.empty() && !i.libz.empty() && !i.fixture.empty();
    return i;
  }();
  return im;
}

struct Proc {
  std::unique_ptr<runtime::GuestProcess> p;
  const loader::GuestModule* fixture = nullptr;
  std::string err;
};

constexpr std::uint32_t kDlSlotBase = 400;  ///< the rig's dynamic-linker answers, far above the JNIEnv's slots

/// Boots a process on `tier` with the fixture loaded and every constructor run. libc's own constructors call
/// the dynamic-linker interface (netd's `dlopen`), answered here as bionic's linker would for a process with
/// nothing more to load (NULL / 0, target SDK 34) — the same answers as guest_process_test.cpp.
Proc BootPthreads(int tier) {
  Proc pr;
  const Images& im = PthreadImages();
  if (!im.ok) {
    pr.err = std::string{"missing images: "} + RB_DEVICE_LIBC_PATH + ", " + RB_GUEST_LIBM_PATH + ", " +
             RB_GUEST_LIBZ_PATH + " or " + RB_PTHREAD_FIXTURE_PATH + " (pushed by tools/run_engine_tests.sh)";
    return pr;
  }
  static const char* const kDl[] = {"dlopen", "android_dlopen_ext", "dlsym", "dlclose", "dladdr", "dlerror",
                                    "dl_unwind_find_exidx", "android_get_application_target_sdk_version"};
  auto holder = std::make_shared<runtime::GuestProcess*>(nullptr);
  runtime::GuestProcessOptions opts;
  opts.window_bytes = 64u << 20;
  opts.biased = true;
  opts.unresolved_trap = 0x0bad'0000u;
  opts.ctor_tier = tier;
  opts.host_resolver = [holder](const std::string& name) -> std::optional<exec::GuestAddr> {
    for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
      if (name == kDl[i]) return (*holder)->abi().SlotFor(kDlSlotBase + i);
    }
    return std::nullopt;
  };
  pr.p = runtime::GuestProcess::Create(opts, &pr.err);
  if (!pr.p) return pr;
  *holder = pr.p.get();
  for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
    const bool sdk = std::strcmp(kDl[i], "android_get_application_target_sdk_version") == 0;
    pr.p->abi().Bind(
        kDlSlotBase + i,
        [sdk](exec::MachineState& s, std::string*) {
          s.r[0] = sdk ? 34u : 0u;
          return true;
        },
        abi::AbiContext::SlotLock::kHoldsGgl);
  }
  if (!pr.p->LoadScope(im.libc, im.libm, im.libz, {}, &pr.err)) {
    pr.p.reset();
    return pr;
  }
  pr.fixture = pr.p->LoadApp(im.fixture, "libpthread_fixture.so", &pr.err);
  std::string failed;
  if (pr.fixture == nullptr || !pr.p->RunConstructors(&failed)) {
    if (pr.err.empty()) pr.err = "RunConstructors: " + failed;
    pr.p.reset();
  }
  return pr;
}

struct FixtureCall {
  abi::Ret32 ret{};
  exec::RunResult rr{};
  double ms = 0;
};

FixtureCall CallFixture(Proc& pr, int tier, const char* fn, const abi::Args32& args) {
  FixtureCall c;
  const auto at = pr.fixture->FindExport(fn);
  if (!at) {
    c.rr.reason = exec::StopReason::Internal;
    c.rr.detail = std::string{"the fixture exports no "} + fn;
    return c;
  }
  pr.p->abi().set_executor(pr.p->engine(tier));
  const auto t0 = Clock::now();
  c.ret = pr.p->abi().CallGuest(at->tagged(), args, abi::Shorty::For("I"), 2'000'000'000u, &c.rr);
  c.ms = Ms(t0, Clock::now());
  return c;
}

std::string Faults(runtime::GuestProcess& p) {
  std::string out;
  for (const std::string& f : p.thread_faults()) out += (out.empty() ? "" : " | ") + f;
  return out;
}

/// One oracle row: `row` is the oracle's name for the call (exactly as tests/guest/pthread_oracle.c prints it).
void PthreadRow(Proc& pr, int tier, const std::map<std::string, std::string>& oracle, const std::string& row,
                const char* fn, const abi::Args32& args) {
  const std::string name = row + "/" + TierName(tier);
  // 20 s per row (the brief), except the one 200,000-lock row on Tier I: the interpreter runs ~1.2M guest
  // instructions/s on the tablet and that row is ~23 s of guest work there (Tier J: ~1 s), so its limit is 60 s.
  const bool slow = tier == 1 && row == "pt_mutex_contend(2,100000)";
  Watchdog wd(name, std::chrono::seconds(slow ? 60 : 20));
  const auto want = oracle.find(row);
  CHECK_CTX(want != oracle.end(), "no row " + row + " in " + RB_PTHREAD_ORACLE_PATH);
  if (want == oracle.end() || !pr.p) return;
  const std::size_t started = pr.p->threads_started();
  const FixtureCall c = CallFixture(pr, tier, fn, args);
  const std::string got = std::to_string(c.ret.i());
  std::fprintf(stderr, "#   %s = %s (oracle %s) in %.1f ms, %zu guest thread(s) started\n", name.c_str(), got.c_str(),
               want->second.c_str(), c.ms, pr.p->threads_started() - started);
  CHECK_CTX(c.rr.reason == exec::StopReason::Returned,
            name + ": reason " + std::to_string(static_cast<int>(c.rr.reason)) + " " + c.rr.detail + " " + Faults(*pr.p));
  CHECK_CTX(got == want->second, name + " = " + got + ", the device says " + want->second);
  // Every thread the row started has finished (detached ones tear themselves down) and none faulted.
  CHECK_CTX(pr.p->WaitForThreads(std::chrono::milliseconds(5000)),
            name + ": " + std::to_string(pr.p->running_threads()) + " guest thread(s) still running 5 s later");
  CHECK_CTX(pr.p->thread_faults().empty(), name + ": " + Faults(*pr.p));
  CHECK_CTX(pr.p->monitor().live_threads() == 0, name + ": the monitor still counts " +
                                                     std::to_string(pr.p->monitor().live_threads()) + " live thread(s)");
}

/// `clone` with flags other than bionic pthread_create's is refused by name with -ENOSYS: a planted ARM blob
/// `mov r0, #0x11; mov r1, #0; mov r7, #120; svc #0; bx lr` (0x11 = SIGCHLD alone: a fork).
void clone_with_unknown_flags_is_refused(Proc& pr, int tier) {
  const std::string name = std::string{"clone_with_unknown_flags_is_refused/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(20));
  if (!pr.p) return;
  std::string err;
  const exec::GuestAddr page = pr.p->MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &err);
  CHECK_CTX(page != 0, err);
  if (page == 0) return;
  const std::uint32_t code[] = {0xe3a0'0011u, 0xe3a0'1000u, 0xe3a0'7078u, 0xef00'0000u, 0xe12f'ff1eu};
  CHECK(pr.p->memory().Write(page, code, sizeof code));
  CHECK(pr.p->memory().Protect(page, mem::kGuestPage, mem::kRead | mem::kExec, &err));
  const std::size_t started = pr.p->threads_started();
  pr.p->abi().set_executor(pr.p->engine(tier));
  exec::RunResult rr{};
  const abi::Ret32 r = pr.p->abi().CallGuest(page, {}, abi::Shorty::For("I"), 1000, &rr);
  CHECK_CTX(rr.reason == exec::StopReason::Returned && r.i() == -38,
            name + ": r0=" + std::to_string(r.i()) + " " + rr.detail);
  CHECK(pr.p->threads_started() == started);
  // The refusal names the call and the flags: in the run's notes, or (when the note cap is already full from
  // boot) in the monitor's answer itself.
  bool named = false;
  for (const std::string& n : pr.p->monitor().notes()) {
    if (n.find("clone") != std::string::npos && n.find("0x11") != std::string::npos) named = true;
  }
  syscall::Frame f{};
  f.number = syscall::nr::kClone;
  f.arg[0] = 0x11;
  exec::MachineState scratch{};
  const syscall::Outcome o = pr.p->monitor().Classify(f, scratch, nullptr);
  CHECK_CTX(o.r0 == -38 && o.detail.find("clone") != std::string::npos && o.detail.find("0x11") != std::string::npos,
            o.detail);
  CHECK_CTX(named || pr.p->monitor().dropped_notes() != 0, "no note names the refused clone");
}

/// `exit_group` from one guest thread while another is parked in a futex wait, a third spins with no syscall
/// and the caller is parked in pthread_join: the process ends with the status (the device's own rc), every
/// guest thread stops, and no host thread is left running guest code.
void exit_group_stops_every_thread(int tier, const std::map<std::string, std::string>& oracle) {
  const std::string row = "pt_exit_group_from_thread(7)";
  const std::string name = row + "/" + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(20));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const auto want = oracle.find(row);
  CHECK_CTX(want != oracle.end(), "no row " + row + " in the oracle");
  const FixtureCall c = CallFixture(pr, tier, "pt_exit_group_from_thread", {abi::Value32::S32(7)});
  std::fprintf(stderr, "#   %s: reason %d, status %d (oracle rc %s), %zu thread(s) started, in %.1f ms — %s\n",
               name.c_str(), static_cast<int>(c.rr.reason), pr.p->abi().exit_status(),
               want == oracle.end() ? "?" : want->second.c_str(), pr.p->threads_started(), c.ms, c.rr.detail.c_str());
  CHECK_CTX(c.rr.reason == exec::StopReason::Exited, name + ": " + c.rr.detail);
  CHECK_CTX(want != oracle.end() && std::to_string(pr.p->abi().exit_status()) == want->second,
            name + ": status " + std::to_string(pr.p->abi().exit_status()));
  CHECK(pr.p->threads_started() == 3u);
  CHECK_CTX(pr.p->WaitForThreads(std::chrono::milliseconds(3000)),
            name + ": " + std::to_string(pr.p->running_threads()) + " host thread(s) still running guest code");
  CHECK_CTX(pr.p->thread_faults().empty(), Faults(*pr.p));
  CHECK(pr.p->monitor().group_exited());
}

/// Fix round 1: `exit_group` while another guest thread `clone`s in a tight loop, repeated on fresh processes.
/// After the process exits no guest instruction may run and no clone may succeed: the cloner's success counter
/// (`pt_storm_clones`, read from guest memory) must not move after the call returns `Exited`; every host thread
/// must finish; destroying the process must join them all (a thread left in the list, or one started after
/// teardown took the list, would be `std::terminate` or a run on a freed bridge).
void clone_storm_then_exit_group(int tier, const std::map<std::string, std::string>& oracle) {
  const std::string row = "pt_clone_storm_exit(9)";
  const auto want = oracle.find(row);
  CHECK_CTX(want != oracle.end(), "no row " + row + " in the oracle");
  constexpr int kRounds = 5;
  for (int round = 0; round < kRounds; ++round) {
    const std::string name = row + "/" + TierName(tier) + "#" + std::to_string(round);
    Watchdog wd(name, std::chrono::seconds(20));
    Proc pr = BootPthreads(tier);
    CHECK_CTX(pr.p != nullptr, pr.err);
    if (!pr.p) return;
    const auto counter = pr.fixture->FindExport("pt_storm_clones");
    CHECK_CTX(counter.has_value(), "the fixture exports no pt_storm_clones");
    if (!counter) return;
    const FixtureCall c = CallFixture(pr, tier, "pt_clone_storm_exit", {abi::Value32::S32(9)});
    std::uint32_t at_exit = 0xdead'dead, later = 0xdead'dead;
    pr.p->memory().Read(counter->addr, &at_exit, 4);  // main has stopped: no guest thread holds the GGL now
    const bool all_done = pr.p->WaitForThreads(std::chrono::milliseconds(3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pr.p->memory().Read(counter->addr, &later, 4);
    std::fprintf(stderr, "#   %s: reason %d status %d (oracle rc %s), %u clone(s) before exit, %zu thread(s) started, "
                         "%.1f ms\n",
                 name.c_str(), static_cast<int>(c.rr.reason), pr.p->abi().exit_status(),
                 want == oracle.end() ? "?" : want->second.c_str(), at_exit, pr.p->threads_started(), c.ms);
    if (std::getenv("RB_STORM_DEBUG") != nullptr) {
      std::fprintf(stderr, "#     tally: %s\n", pr.p->monitor().TallyText().c_str());
      for (const std::string& n : pr.p->monitor().notes()) std::fprintf(stderr, "#     note: %s\n", n.c_str());
    }
    CHECK_CTX(c.rr.reason == exec::StopReason::Exited, name + ": " + c.rr.detail);
    CHECK_CTX(want != oracle.end() && std::to_string(pr.p->abi().exit_status()) == want->second,
              name + ": status " + std::to_string(pr.p->abi().exit_status()));
    CHECK_CTX(at_exit > 0u && at_exit != 0xdead'dead, name + ": no clone succeeded before the exit, so the race was not exercised");
    CHECK_CTX(at_exit == later, name + ": the clone counter moved after exit_group (" + std::to_string(at_exit) +
                                    " -> " + std::to_string(later) + "): guest code ran in an exited process");
    CHECK_CTX(all_done, name + ": " + std::to_string(pr.p->running_threads()) + " host thread(s) outlived the process");
    CHECK_CTX(pr.p->thread_faults().empty(), name + ": " + Faults(*pr.p));
    pr.p.reset();  // joins whatever is left; a thread outside the list would terminate the lane here
  }
}

// ------------------------------------------------------------------------------------ M2 Task 5: adopted threads
//
// A host thread that never went through guest pthread_create (an ART thread) calling into guest code: the
// process adopts it (its own pthread_internal_t, stack, TLS and tid, linked into g_thread_list), and its guest
// TLS-key destructors run when the host thread exits. The composite matches the native oracle's
// `adopted_thread_keys()`, where the same fixture calls run on threads the device's pthread_create made.

/// The `pthread_internal_t`s on libc's `g_thread_list`, head first (at most 64).
std::vector<std::uint32_t> ThreadList(runtime::GuestProcess& p) {
  std::vector<std::uint32_t> out;
  runtime::Ggl::Held hold(p.ggl());
  const auto head = p.libc()->FindLocalSymbol("g_thread_list");
  std::uint32_t at = 0;
  if (!head || !p.memory().Read(*head, &at, 4)) return out;
  while (at != 0 && out.size() < 64) {
    out.push_back(at);
    if (!p.memory().Read(at, &at, 4)) break;
  }
  return out;
}

bool Has(const std::vector<std::uint32_t>& v, std::uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

struct Adopted {
  int tid = 0, key_get = -1, b_tid = 0, b_key_get = -1;
  std::uint32_t self = 0, b_self = 0, errno_slot = 0, struct_tid = 0;
  bool bound = false, adopted = false, struct_matches = false, in_list = false, b_in_list = false;
  std::size_t adopted_while_b_alive = 0, adopted_after_b = 0;
  std::string why;
};

void art_thread_is_adopted(int tier, const std::map<std::string, std::string>& oracle) {
  const std::string name = std::string{"art_thread_is_adopted/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  using V = abi::Value32;
  // The main thread (the host thread that ran LoadScope) calls first and returns.
  const FixtureCall mt = CallFixture(pr, tier, "pt_self_tid", {});
  const FixtureCall ms = CallFixture(pr, tier, "pt_self", {});
  CHECK_CTX(mt.rr.reason == exec::StopReason::Returned && mt.ret.i() == runtime::GuestProcess::kMainTid, mt.rr.detail);
  CHECK(ms.ret.w() == pr.p->main_thread());
  CHECK(runtime::BoundHere(pr.p->main_guest_thread()));

  Adopted a;
  std::thread ta([&] {
    // Ruling a: a second host thread, after main returned, is adopted — never given the main thread.
    const FixtureCall t = CallFixture(pr, tier, "pt_self_tid", {});
    if (t.rr.reason != exec::StopReason::Returned) a.why = "pt_self_tid: " + t.rr.detail;
    a.tid = t.ret.i();
    GuestThread* g = runtime::CurrentGuestThread();
    a.bound = g != nullptr && runtime::BoundHere(*g) && g != &pr.p->main_guest_thread();
    a.adopted = g != nullptr && g->adopted && g->tid == a.tid;
    a.self = CallFixture(pr, tier, "pt_self", {}).ret.w();
    a.struct_matches = g != nullptr && g->pthread_struct == a.self;
    {
      runtime::Ggl::Held hold(pr.p->ggl());
      pr.p->memory().Read(a.self + 0x8u, &a.struct_tid, 4);
    }
    a.errno_slot = CallFixture(pr, tier, "pt_errno_slot", {V::S32(1234)}).ret.w();
    a.in_list = Has(ThreadList(*pr.p), a.self);
    const FixtureCall set = CallFixture(pr, tier, "pt_key_set", {V::S32(7)});
    if (set.ret.i() != 0) a.why += " pt_key_set(7) = " + std::to_string(set.ret.i()) + " " + set.rr.detail;
    a.key_get = CallFixture(pr, tier, "pt_key_get", {}).ret.i();
    // A second adopted thread while this one is alive (and holds its value): its own key slot reads 0.
    std::thread tb([&] {
      a.b_key_get = CallFixture(pr, tier, "pt_key_get", {}).ret.i();
      a.b_tid = CallFixture(pr, tier, "pt_self_tid", {}).ret.i();
      a.b_self = CallFixture(pr, tier, "pt_self", {}).ret.w();
      a.b_in_list = Has(ThreadList(*pr.p), a.b_self);
      a.adopted_while_b_alive = pr.p->adopted_threads();
    });
    tb.join();
    a.adopted_after_b = pr.p->adopted_threads();  // b's host thread exited: retired before join returned
  });
  ta.join();

  const int dtors = CallFixture(pr, tier, "pt_key_destructor_count", {}).ret.i();
  const auto list = ThreadList(*pr.p);
  bool a_mapped = true;
  {
    runtime::Ggl::Held hold(pr.p->ggl());
    a_mapped = pr.p->memory().IsMapped(a.self & ~0xfffu);
  }
  std::fprintf(stderr, "#   %s: main tid %d self 0x%x; A tid %d self 0x%x errno@0x%x key %d; B tid %d self 0x%x key %d; "
                       "%d destructor call(s); %zu adoptions; g_thread_list %zu long%s%s\n",
               name.c_str(), mt.ret.i(), ms.ret.w(), a.tid, a.self, a.errno_slot, a.key_get, a.b_tid, a.b_self,
               a.b_key_get, dtors, pr.p->threads_adopted(), list.size(), a.why.empty() ? "" : " — ", a.why.c_str());
  CHECK_CTX(a.why.empty(), a.why);
  CHECK_CTX(a.tid != 0 && a.tid != mt.ret.i(), "adopted tid " + std::to_string(a.tid));
  CHECK_CTX(a.self != 0 && a.self != ms.ret.w(), "adopted pthread_self " + Hex(a.self));
  CHECK(a.bound);
  CHECK(a.adopted);
  CHECK(a.struct_matches);
  CHECK_CTX(a.struct_tid == static_cast<std::uint32_t>(a.tid), "struct tid " + std::to_string(a.struct_tid));
  CHECK_CTX(a.errno_slot == a.self + 0x29cu, "errno at " + Hex(a.errno_slot) + ", struct " + Hex(a.self));
  CHECK(a.in_list);
  CHECK_CTX(a.key_get == 7, "pt_key_get on A = " + std::to_string(a.key_get));
  CHECK_CTX(a.b_key_get == 0, "pt_key_get on B = " + std::to_string(a.b_key_get));
  CHECK(a.b_tid != 0 && a.b_tid != a.tid && a.b_tid != mt.ret.i());
  CHECK(a.b_self != 0 && a.b_self != a.self && a.b_in_list);
  CHECK_CTX(a.adopted_while_b_alive == 2u && a.adopted_after_b == 1u,
            std::to_string(a.adopted_while_b_alive) + " / " + std::to_string(a.adopted_after_b));
  CHECK_CTX(dtors == 1, "pt_key_destructor_count() = " + std::to_string(dtors));
  CHECK(!Has(list, a.self) && !Has(list, a.b_self) && Has(list, pr.p->main_thread()));
  CHECK(pr.p->adopted_threads() == 0u && pr.p->threads_adopted() == 2u);
  CHECK(pr.p->monitor().live_threads() == 0u);
  CHECK_CTX(!a_mapped, "the adopted thread's mapping is still mapped after its host thread exited");
  CHECK(runtime::BoundHere(pr.p->main_guest_thread()));  // main never moved off the LoadScope thread
  // The native oracle composes the same five facts.
  const int composite = (a.tid != 0 && a.tid != mt.ret.i() ? 1 : 0) + (a.self != ms.ret.w() ? 10 : 0) +
                        (a.key_get == 7 ? 100 : 0) + (a.b_key_get == 0 ? 1000 : 0) + (dtors == 1 ? 10000 : 0);
  const auto want = oracle.find("adopted_thread_keys()");
  CHECK_CTX(want != oracle.end() && std::to_string(composite) == want->second,
            name + " composite " + std::to_string(composite) + ", the device says " +
                (want == oracle.end() ? std::string{"(no row)"} : want->second));
  CHECK(pr.p->ggl().misuses() == 0u);
}

/// An adopted host thread that outlives its process: destroying the process leaves the host thread unbound
/// (ruling c — its GuestThread is gone, and its binding reads as absent rather than dangling), and the host
/// thread's later exit runs no teardown on the destroyed process.
void adopted_thread_outlives_process(int tier) {
  const std::string name = std::string{"adopted_thread_outlives_process/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  Gate gate;
  int tid = 0;
  bool bound = false, unbound_after = false;
  std::thread c([&] {
    tid = CallFixture(pr, tier, "pt_key_set", {abi::Value32::S32(9)}).ret.i() == 0
              ? CallFixture(pr, tier, "pt_self_tid", {}).ret.i()
              : -1;
    bound = runtime::CurrentGuestThread() != nullptr;
    gate.Set(1);
    gate.Wait(2);
    unbound_after = runtime::CurrentGuestThread() == nullptr;
  });
  CHECK(gate.Wait(1));
  CHECK(pr.p->adopted_threads() == 1u);
  pr.p.reset();  // the adopted host thread is still alive and bound
  gate.Set(2);
  c.join();  // its exit hook finds the process gone: nothing runs
  CHECK_CTX(tid > 0 && bound, name + ": tid " + std::to_string(tid));
  CHECK(unbound_after);
}

// ---- fix round 1: an adopted thread's exit that does not go cleanly never frees a node still on g_thread_list.

bool PageMapped(runtime::GuestProcess& p, std::uint32_t at) {
  runtime::Ggl::Held hold(p.ggl());
  return p.memory().IsMapped(at & ~0xfffu);
}

std::string Joined(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : " | ") + s;
  return out;
}

/// After a messy exit the process still works: pthread_create/join (the oracle's own row) and a fresh adoption.
void ProcessStillWorks(Proc& pr, int tier, const std::map<std::string, std::string>& oracle, const std::string& name,
                       std::uint32_t old_self) {
  const FixtureCall cj = CallFixture(pr, tier, "pt_create_join", {abi::Value32::S32(8)});
  const auto want = oracle.find("pt_create_join(8)");
  CHECK_CTX(cj.rr.reason == exec::StopReason::Returned && want != oracle.end() &&
                std::to_string(cj.ret.i()) == want->second,
            name + ": pt_create_join(8) afterwards = " + std::to_string(cj.ret.i()) + " " + cj.rr.detail);
  int tid = 0;
  std::uint32_t self = 0;
  std::thread n([&] {
    tid = CallFixture(pr, tier, "pt_self_tid", {}).ret.i();
    self = CallFixture(pr, tier, "pt_self", {}).ret.w();
  });
  n.join();
  (void)old_self;  // a freed mapping may be reused by this adoption: same address is fine once off the list
  CHECK_CTX(tid > 0 && self != 0 && !Has(ThreadList(*pr.p), self),
            name + ": a new adoption afterwards: tid " + std::to_string(tid) + " self " + Hex(self));
}

/// A TLS-key destructor that runs far past one guest call's 4M-step budget completes (the exit re-enters it),
/// then the thread is unlinked and its mapping freed.
void slow_key_destructor_completes(int tier) {
  const std::string name = std::string{"slow_key_destructor_completes/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(120));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  std::uint32_t self = 0;
  int set = -1;
  const auto t0 = Clock::now();
  std::thread a([&] {
    set = CallFixture(pr, tier, "pt_slow_key_set", {abi::Value32::S32(1000000)}).ret.i();
    self = CallFixture(pr, tier, "pt_self", {}).ret.w();
  });
  a.join();
  const double ms = Ms(t0, Clock::now());
  const int done = CallFixture(pr, tier, "pt_slow_dtor_count", {}).ret.i();
  std::fprintf(stderr, "#   %s: destructor count %d, exit took %.1f ms, faults: %s\n", name.c_str(), done, ms,
               Joined(pr.p->adoption_faults()).c_str());
  CHECK(set == 0);
  CHECK_CTX(done == 1, name + ": pt_slow_dtor_count() = " + std::to_string(done));
  CHECK_CTX(pr.p->adoption_faults().empty(), Joined(pr.p->adoption_faults()));
  CHECK(!Has(ThreadList(*pr.p), self));
  CHECK(!PageMapped(*pr.p, self));
  CHECK(pr.p->kept_mappings() == 0u && pr.p->adopted_threads() == 0u);
}

/// A C++ `thread_local` destructor (__cxa_thread_atexit_impl) registered on an adopted thread runs at its exit, as
/// pthread_exit's __cxa_thread_finalize runs it on a device; the thread's dynamic TLS is freed after the unlink.
void adopted_exit_runs_thread_finalizers(int tier) {
  const std::string name = std::string{"adopted_exit_runs_thread_finalizers/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  int reg = -1;
  std::uint32_t self = 0;
  std::thread a([&] {
    reg = CallFixture(pr, tier, "pt_tls_dtor_register", {}).ret.i();
    self = CallFixture(pr, tier, "pt_self", {}).ret.w();
  });
  a.join();
  const int runs = CallFixture(pr, tier, "pt_tls_dtor_count", {}).ret.i();
  std::fprintf(stderr, "#   %s: register %d, destructor runs %d, faults: %s\n", name.c_str(), reg, runs,
               Joined(pr.p->adoption_faults()).c_str());
  CHECK(reg == 0);
  CHECK_CTX(runs == 1, name + ": pt_tls_dtor_count() = " + std::to_string(runs));
  CHECK_CTX(pr.p->adoption_faults().empty(), Joined(pr.p->adoption_faults()));
  CHECK(!Has(ThreadList(*pr.p), self));
  CHECK(pr.p->kept_mappings() == 0u && pr.p->adopted_threads() == 0u);
}

/// A TLS-key destructor that faults: the exit is reported by name, the thread is still unlinked (so its mapping
/// can go), and the process carries on.
void faulting_key_destructor_is_named(int tier, const std::map<std::string, std::string>& oracle) {
  const std::string name = std::string{"faulting_key_destructor_is_named/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  std::uint32_t self = 0;
  int set = -1;
  std::thread a([&] {
    set = CallFixture(pr, tier, "pt_fault_key_set", {}).ret.i();
    self = CallFixture(pr, tier, "pt_self", {}).ret.w();
  });
  a.join();
  const auto faults = pr.p->adoption_faults();
  std::fprintf(stderr, "#   %s: %s\n", name.c_str(), Joined(faults).c_str());
  CHECK(set == 0);
  CHECK_CTX(faults.size() == 1u && faults[0].find("a TLS-key destructor did not complete") != std::string::npos &&
                faults[0].find("pthread_key_clean_all") != std::string::npos,
            Joined(faults));
  CHECK_CTX(!Has(ThreadList(*pr.p), self), name + ": the struct is still on g_thread_list");
  CHECK(!PageMapped(*pr.p, self));
  CHECK(pr.p->kept_mappings() == 0u && pr.p->adopted_threads() == 0u);
  CHECK(!pr.p->monitor().group_exited());
  ProcessStillWorks(pr, tier, oracle, name, self);
}

/// An exit whose unlink cannot happen (here: the adopted GuestThread was handed to another live host thread, so
/// the exiting one cannot run its guest code) keeps the mapping — the node is still linked — and names it; the
/// list stays walkable, so thread creation and adoption still work.
void unlinkable_exit_keeps_the_mapping(int tier, const std::map<std::string, std::string>& oracle) {
  const std::string name = std::string{"unlinkable_exit_keeps_the_mapping/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  Gate gate;
  GuestThread* g = nullptr;
  std::uint32_t self = 0;
  bool h_bound = false;
  bool h_still_bound = false;  // after `a` retired: `h`'s GuestThread was not freed under it
  std::thread a([&] {
    self = CallFixture(pr, tier, "pt_self", {}).ret.w();
    g = runtime::CurrentGuestThread();
    runtime::UnbindCurrentHostThread();
    gate.Set(1);
    gate.Wait(2);  // exits while `h` holds its guest thread
  });
  std::thread h([&] {
    gate.Wait(1);
    h_bound = g != nullptr && runtime::BindCurrentHostThread(*g);
    gate.Set(2);
    gate.Wait(3);
    h_still_bound = runtime::CurrentGuestThread() == g && g->tid != 0;
  });
  CHECK(gate.Wait(2));
  a.join();
  gate.Set(3);
  h.join();
  const auto faults = pr.p->adoption_faults();
  std::fprintf(stderr, "#   %s: %s\n", name.c_str(), Joined(faults).c_str());
  CHECK(h_bound);
  CHECK(h_still_bound);
  CHECK_CTX(faults.size() == 1u && faults[0].find("bound to another host thread") != std::string::npos &&
                faults[0].find("is kept (leaked), not freed") != std::string::npos,
            Joined(faults));
  CHECK(Has(ThreadList(*pr.p), self));
  CHECK(PageMapped(*pr.p, self));
  CHECK(pr.p->kept_mappings() == 1u && pr.p->adopted_threads() == 0u);
  ProcessStillWorks(pr, tier, oracle, name, self);
}

/// Adoption on a process that has exited is refused by name, not run.
void adoption_after_exit_is_refused(int tier) {
  const std::string name = std::string{"adoption_after_exit_is_refused/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(30));
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  {
    runtime::Ggl::Held hold(pr.p->ggl());
    pr.p->monitor().RequestGroupExit("the test ended the process");
  }
  FixtureCall c;
  std::thread t([&] { c = CallFixture(pr, tier, "pt_self_tid", {}); });
  t.join();
  CHECK_CTX(c.rr.reason == exec::StopReason::Internal &&
                c.rr.detail.find("adopting a host thread: the guest process has exited") != std::string::npos,
            c.rr.detail);
  CHECK(pr.p->threads_adopted() == 0u);
}

/// An import nothing answers faults *by name* when called, on the main thread and on an adopted host thread
/// alike (Angry Birds Go, 2026-10-17: `nativeInitialize` on its GL thread stopped at "instruction fetch failed
/// at 0xfffe0000" and nothing said which of 78 unanswered imports that was). Each unanswered import is bound to
/// its own trap address, and the run loop names the one a fault stopped on — beta as beta, not as the
/// first-bound alpha.
void unresolved_import_faults_by_name(int tier) {
  const std::string name = std::string{"unresolved_import_faults_by_name/"} + TierName(tier);
  Watchdog wd(name, std::chrono::seconds(60));
  const std::vector<std::uint8_t> image = ReadFile(RB_UNRESOLVED_FIXTURE_PATH);
  CHECK_CTX(!image.empty(), std::string{"missing "} + RB_UNRESOLVED_FIXTURE_PATH);
  if (image.empty()) return;
  Proc pr = BootPthreads(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  std::string e;
  const loader::GuestModule* lib = pr.p->LoadApp(image, "librbunresolved.so", &e);
  CHECK_CTX(lib != nullptr, e);
  if (lib == nullptr) return;
  {
    runtime::Ggl::Held hold(pr.p->ggl());
    const auto& un = pr.p->unanswered();
    CHECK(std::find(un.begin(), un.end(), "rb_never_defined_alpha") != un.end());
    CHECK(std::find(un.begin(), un.end(), "rb_never_defined_beta") != un.end());
  }
  const auto call = [&](const char* fn) {
    exec::RunResult rr{};
    const auto at = lib->FindExport(fn);
    if (!at) {
      rr.reason = exec::StopReason::Internal;
      rr.detail = std::string{"no export "} + fn;
      return rr;
    }
    pr.p->abi().set_executor(pr.p->engine(tier));
    pr.p->abi().CallGuest(at->tagged(), {abi::Value32::S32(5)}, abi::Shorty::For("II"), 1'000'000u, &rr);
    return rr;
  };
  const auto names = [](const exec::RunResult& rr, const char* want, const char* not_want) {
    return rr.reason == exec::StopReason::Fault && rr.detail.find(want) != std::string::npos &&
           rr.detail.find(not_want) == std::string::npos;
  };
  const exec::RunResult main_beta = call("uc_call_beta");
  CHECK_CTX(names(main_beta, "rb_never_defined_beta", "rb_never_defined_alpha"),
            name + " main thread: " + main_beta.detail);
  exec::RunResult adopted_alpha{}, adopted_beta{};
  bool adopted = false;
  std::thread t([&] {
    adopted_alpha = call("uc_call_alpha");
    GuestThread* g = runtime::CurrentGuestThread();
    adopted = g != nullptr && g->adopted;
    adopted_beta = call("uc_call_beta");
  });
  t.join();
  CHECK_CTX(adopted, name + ": the second host thread was not adopted");
  CHECK_CTX(names(adopted_alpha, "rb_never_defined_alpha", "rb_never_defined_beta"),
            name + " adopted thread: " + adopted_alpha.detail);
  CHECK_CTX(names(adopted_beta, "rb_never_defined_beta", "rb_never_defined_alpha"),
            name + " adopted thread: " + adopted_beta.detail);
  std::fprintf(stderr, "#   %s: %s\n", name.c_str(), adopted_beta.detail.c_str());
}

void guest_pthreads(int tier) {
  const auto oracle = Oracle();
  CHECK_CTX(!oracle.empty(), std::string{"no oracle rows at "} + RB_PTHREAD_ORACLE_PATH);
  Proc pr;
  {
    Watchdog wd(std::string{"boot/"} + TierName(tier), std::chrono::seconds(60));
    pr = BootPthreads(tier);
  }
  CHECK_CTX(pr.p != nullptr, std::string{"booting the pthread fixture on Tier "} + TierName(tier) + ": " + pr.err);
  if (!pr.p) return;
  clone_with_unknown_flags_is_refused(pr, tier);
  using V = abi::Value32;
  PthreadRow(pr, tier, oracle, "pt_create_join(8)", "pt_create_join", {V::S32(8)});
  PthreadRow(pr, tier, oracle, "pt_mutex_contend(2,100000)", "pt_mutex_contend", {V::S32(2), V::S32(100000)});
  PthreadRow(pr, tier, oracle, "pt_mutex_contend(4,2000)", "pt_mutex_contend", {V::S32(4), V::S32(2000)});
  PthreadRow(pr, tier, oracle, "pt_condvar_pingpong(500)", "pt_condvar_pingpong", {V::S32(500)});
  PthreadRow(pr, tier, oracle, "pt_keys_and_destructors()", "pt_keys_and_destructors", {});
  PthreadRow(pr, tier, oracle, "pt_detached(8)", "pt_detached", {V::S32(8)});
  PthreadRow(pr, tier, oracle, "pt_once()", "pt_once", {});
  PthreadRow(pr, tier, oracle, "pt_errno_per_thread()", "pt_errno_per_thread", {});
  PthreadRow(pr, tier, oracle, "pt_timeouts()", "pt_timeouts", {});
  PthreadRow(pr, tier, oracle, "pt_sleeps()", "pt_sleeps", {});
  // More syscalls in one guest thread than a turn's cap: the turn boundary lands on a syscall, which must be
  // answered, not skipped (final-review finding 1).
  PthreadRow(pr, tier, oracle, "pt_syscall_storm(150000)", "pt_syscall_storm", {V::S32(150000)});
  PthreadRow(pr, tier, oracle, "pt_tls_dtor_on_thread()", "pt_tls_dtor_on_thread", {});
  std::fprintf(stderr, "#   Tier %s: %zu guest threads started; futex: %llu blocked, %llu woken, %llu timed out; "
                       "%llu contended slices\n",
               TierName(tier), pr.p->threads_started(), static_cast<unsigned long long>(pr.p->futex().blocked()),
               static_cast<unsigned long long>(pr.p->futex().woken()),
               static_cast<unsigned long long>(pr.p->futex().timed_out()),
               static_cast<unsigned long long>(pr.p->abi().contended_slices()));
  CHECK(pr.p->ggl().misuses() == 0u);
  {
    Watchdog wd(std::string{"teardown/"} + TierName(tier), std::chrono::seconds(20));
    pr.p.reset();
  }
  exit_group_stops_every_thread(tier, oracle);
  clone_storm_then_exit_group(tier, oracle);
  art_thread_is_adopted(tier, oracle);
  adopted_thread_outlives_process(tier);
  adoption_after_exit_is_refused(tier);
  slow_key_destructor_completes(tier);
  adopted_exit_runs_thread_finalizers(tier);
  faulting_key_destructor_is_named(tier, oracle);
  unlinkable_exit_keeps_the_mapping(tier, oracle);
  unresolved_import_faults_by_name(tier);
}

}  // namespace

/// ---- C26: signal delivery into a handler the guest installed.
///
/// The whole point of the piece: a guest thread with a queued signal must *enter* the handler the guest
/// installed with `rt_sigaction`, and when the handler returns through the restorer's `rt_sigreturn`, the
/// interrupted code must resume where it stopped, with its registers and blocked mask put back.
///
/// The assertions are the guest's own register file and the memory the handler wrote — not a log line. The
/// handler is real Thumb code both tiers execute, so a delivery that built the frame wrong fails here
/// rather than in a game.
void signal_delivery_enters_the_guest_handler_and_resumes(int tier) {
  Rig r = Build(tier);
  if (!r.ok) return;
  mem::GuestMemory& m = *r.mem;

  // A page of its own for the handler, the restorer and the code being interrupted: `Build` already
  // turned the shared code page read-only-execute, and a handler is written before it is run. The
  // encodings are raw Thumb because this lane runs on the device, where there is no assembler; each
  // comment is the instruction it is.
  const exec::GuestAddr kPage = kCode + 0x1000;  // the page after the shared blob page
  std::string perr;
  CHECK_CTX(m.Map(kPage, mem::kGuestPage, mem::kRead | mem::kWrite, &perr), "map the handler page: " + perr);
  const std::uint16_t code[] = {
      0x4b01, 0x6018, 0x4770, 0x0000,  // 0x00 handler: ldr r3,[pc,#4]; str r0,[r3]; bx lr; padding
      0x0040, 0x0101,                  // 0x08 .word kData + 0x40, little-endian pair
      0x27ad, 0xdf00, 0xe7fe,          // 0x0c restorer: movs r7, #173 (rt_sigreturn); svc #0; b .
      0x2007, 0x4770,                  // 0x12 victim: movs r0, #7; bx lr
  };
  CHECK_CTX(m.Write(kPage, code, sizeof code), "write the handler bytes while the page is still writable");
  CHECK_CTX(m.Protect(kPage, mem::kGuestPage, mem::kRead | mem::kExec, &perr), "make it executable: " + perr);
  const exec::GuestAddr kHandler = kPage + 0x00;      // the clean entry; the ABI tag is on the handler value
  const exec::GuestAddr kRestorer = kPage + 0x0c + 1; // +1: Thumb
  const exec::GuestAddr kVictimPc = kPage + 0x12;     // clean, mode carried separately (this codebase's rule)

  GuestThread& t = r.abi->main_thread();

  // The runtime's half of the return path, which `GuestProcess` installs in a real launch: without it
  // `rt_sigreturn` is a named refusal, so a lane that forgot it would be testing nothing.
  r.mon->set_signal_return([&m](GuestThread& th) -> syscall::Outcome {
    std::string why;
    if (!ReturnFromSignal(th, m, &why)) return syscall::Outcome::Refused(syscall::err::kEnosys, why);
    return syscall::Outcome::Emulated(0, why);
  });

  // rt_sigaction(sig=30, &act, NULL) in the measured arm32 layout {handler, flags, restorer, mask}.
  // A Thumb handler is tagged |1, exactly as bionic's own `rt_sigaction` callers pass it.
  // sa_mask blocks SIGQUIT (3) in the handler: delivery must add it to the interrupted mask.
  const std::uint32_t act[5] = {kHandler | 1u, 0x1400'0000u /* SA_RESTART|SA_RESTORER */, kRestorer,
                                1u << (3 - 1), 0};
  CHECK(m.Write(kData + 0x20, act, sizeof act));
  exec::MachineState sa{};
  sa.mode = isa::Mode::Thumb;
  sa.pc = kReturn;
  sa.r[7] = syscall::nr::kRtSigaction;
  sa.r[0] = 30;
  sa.r[1] = kData + 0x20;
  r.mon->Handle(sa, 0, &t);
  CHECK_CTX(r.mon->disposition(30).catches(), "the handler is recorded before a delivery can enter it");

  // Nothing to deliver yet, and saying so is not a delivery.
  CHECK_CTX(t.deliveries.empty(), "no delivery is in progress before a signal is queued");
  std::string why;
  CHECK_CTX(!DeliverPendingSignal(t, m, *r.mon, &why), "no pending signal means no delivery: " + why);

  // Put the thread at the victim, with r1 aimed at the word the handler stores into — that is the
  // interrupted state the frame must preserve.
  t.state.mode = isa::Mode::Thumb;
  // A real interrupted state has the mode and CPSR bit 5 agreeing (both tiers keep that invariant); the
  // frame saves CPSR and the return derives the mode from it, so a fixture with only `mode` set would
  // resume in ARM.
  t.state.cpsr = exec::CpsrWithMode(t.state.cpsr, isa::Mode::Thumb);
  t.state.pc = kVictimPc;
  t.state.r[15] = kVictimPc;
  t.state.r[1] = kData + 0x40;  // the interrupted r1 the handler's argument overwrites, and must get back
  t.state.r[3] = 0xcafe'0001u;  // a scratch register the handler never touches: must survive
  // The ABI tags its return sentinel with the caller's mode (`return_address | T` in PlaceArgs); an even
  // sentinel would make the victim's `bx lr` switch to ARM, which is correct CPU behaviour and not what
  // the interrupted thread was doing.
  t.state.r[14] = kReturn | 1u;
  // Move off the very top of the stack, so the words at SP..SP+16 are mapped: they are the interrupted
  // function's saved r4..r7 (the generic `syscall()` wrapper keeps them there), and delivery must not
  // write a byte at or above SP. The bug this pins: a 64-byte zero chunk overran the 872-byte frame and
  // zeroed exactly these, so the pop restored r4=0 (Cars 3, 2026-10-20).
  t.state.r[13] = kStacks + kStackBytes - 0x100;
  const std::uint32_t above_sp[4] = {0x1111'1111u, 0x2222'2222u, 0x3333'3333u, 0x4444'4444u};
  CHECK(m.Write(t.state.r[13], above_sp, sizeof above_sp));
  const std::uint32_t sp_before = t.state.r[13];
  const std::uint32_t r3_before = t.state.r[3];
  const std::uint32_t mask_before = static_cast<std::uint32_t>(t.signal_mask);
  // A sigsuspend in progress: the temporary mask is 1<<5 (signal 6), the pre-suspend mask is mask_before
  // (0). The handler must run with the temporary mask, and rt_sigreturn must put the old one back.
  t.sigsuspend_restore = true;
  t.sigsuspend_old_mask = t.signal_mask;
  t.signal_mask = 1ull << 5;
  const std::uint32_t pristine = 0xaaaa'aaaa;
  CHECK(m.Write(kData + 0x40, &pristine, 4));

  // Queue one the way `tkill` does, then deliver at a block boundary — the seam the run loop uses.
  r.mon->QueueSignal(t.tid, 30);
  CHECK_CTX(DeliverPendingSignal(t, m, *r.mon, &why), "a queued signal with a handler must be delivered: " + why);

  // --- what the handler was entered with.
  CHECK_CTX(t.state.pc == kHandler, "pc must be the handler's clean entry, is " +
                                        std::to_string(static_cast<unsigned long long>(t.state.pc)));
  CHECK_CTX(t.state.mode == isa::Mode::Thumb && (t.state.cpsr & 0x20u) != 0,
            "the handler is entered in Thumb, with the mode bit in CPSR agreeing");
  CHECK_CTX(t.state.r[0] == 30, "the handler's first argument is the signal");
  const std::uint32_t frame = t.deliveries.empty() ? 0 : t.deliveries.back().frame;
  CHECK_CTX(t.state.r[1] + 128 == frame, "r1 is &siginfo, immediately before the ucontext");
  CHECK_CTX(t.state.r[2] == frame, "r2 is &ucontext");
  CHECK_CTX(t.state.r[14] == kRestorer, "lr must be the restorer the guest registered");
  CHECK_CTX(t.state.r[13] < sp_before, "the frame is below the interrupted stack pointer");
  CHECK_CTX(sp_before - t.state.r[13] >= 872, "the frame is at least one rt_sigframe deep");
  CHECK_CTX((t.state.r[13] & 7u) == 0, "the frame is 8-byte aligned");
  CHECK_CTX(!t.deliveries.empty() && t.deliveries.back().frame != 0, "the delivery is recorded on the thread");
  CHECK_CTX(t.deliveries.back().sig == 30, "the record names the signal");
  CHECK_CTX((t.signal_mask & (1ull << 2)) != 0,
            "the handler runs with the sigaction's sa_mask (SIGQUIT) blocked");
  CHECK_CTX((t.signal_mask & (1ull << 29)) != 0,
            "and with the signal itself blocked (no SA_NODEFER)");
  CHECK_CTX(r.mon->signals_delivered() == 1, "the delivery is counted");
  CHECK_CTX(r.mon->TakePendingSignal(t.tid) == 0, "the signal was taken, so a second look delivers nothing");

  // --- the handler runs, returns through the restorer's `rt_sigreturn`, and the victim resumes.
  // `RunThread` is the real run loop, so the `svc #173` in the restorer is answered by the monitor and the
  // frame is restored — exactly the path a game takes.
  exec::RunResult rr = r.abi->RunThread(t, 100000);
  CHECK_CTX(rr.reason == exec::StopReason::Returned || rr.reason == exec::StopReason::Budget,
            "the handler must run to its restorer and the victim must return, reason " +
                std::to_string(static_cast<int>(rr.reason)) + ": " + rr.detail);

  std::uint32_t written = 0;
  CHECK_CTX(m.Read(kData + 0x40, &written, 4) && written == 30,
            "the handler ran: it must have stored the signal number, found " + std::to_string(written));
  CHECK_CTX(t.deliveries.empty(), "rt_sigreturn pops the delivery record");
  std::uint32_t above_now[4] = {};
  CHECK_CTX(m.Read(sp_before, above_now, sizeof above_now) &&
                std::memcmp(above_now, above_sp, sizeof above_sp) == 0,
            "delivery wrote nothing at or above SP: the interrupted function's saved r4..r7 survive");
  CHECK_CTX(t.state.r[3] == r3_before, "the interrupted register file came back (r3)");
  CHECK_CTX(t.state.r[1] == kData + 0x40, "the interrupted r1 came back after the handler used it for &info");
  CHECK_CTX(t.state.r[13] == sp_before, "the stack pointer is back where it was interrupted");
  CHECK_CTX(static_cast<std::uint32_t>(t.signal_mask) == mask_before,
            "the blocked mask is back to what it was before the handler (the frame holds the old mask)");
  // The victim ran after the return: its first instruction is `movs r0, #7`, so r0 says it resumed there
  // and not somewhere else, and its `bx lr` reached the ABI's return sentinel.
  CHECK_CTX(t.state.r[0] == 7, "the victim resumed and ran after the handler returned, r0 is " +
                                   std::to_string(t.state.r[0]));
  CHECK_CTX(t.state.pc == kReturn, "execution resumed at the victim and returned to the sentinel");
  CHECK_CTX(t.state.mode == isa::Mode::Thumb && (t.state.cpsr & 0x20u) != 0,
            "the resumed mode is the interrupted state's Thumb, not the handler's leftovers");
}

/// ---- C26: a signal interrupts a blocked futex wait, and the handler runs before the guest retries.
///
/// This is the shape Mono's GC suspend takes in a real game: a worker is parked on a futex and the GC
/// thread sends SIGPWR. The kernel wakes the wait with EINTR and the thread takes the handler; delivery
/// that only happened at a `RunThread` turn boundary would never reach a parked thread. The handler's
/// store is the proof it ran; the futex's -EINTR is the proof the wake happened for the right reason.
void a_signal_interrupts_a_blocked_futex_and_enters_the_handler(int tier) {
  Watchdog wd(std::string{"futex_signal/"} + TierName(tier), std::chrono::seconds(30));
  Rig r = Build(tier);
  if (!r.ok) return;
  mem::GuestMemory& m = *r.mem;
  runtime::FutexTable ft;
  r.mon->set_futex_table(&ft);

  const exec::GuestAddr kPage = kCode + 0x2000;
  std::string perr;
  CHECK_CTX(m.Map(kPage, mem::kGuestPage, mem::kRead | mem::kWrite, &perr), "map the futex page: " + perr);
  const std::uint16_t code[] = {
      0x4803,                  // 0x00 ldr r0, [pc, #12] -> the uaddr word at 0x10
      0x6802,                  // 0x02 ldr r2, [r0]      (the expected value, 0)
      0x2100,                  // 0x04 movs r1, #0       (FUTEX_WAIT)
      0x2300,                  // 0x06 movs r3, #0       (no timeout)
      0x27f0,                  // 0x08 movs r7, #240     (futex)
      0xdf00,                  // 0x0a svc #0
      0x6020,                  // 0x0c str r0, [r4]      (the wait's return value)
      0x4770,                  // 0x0e bx lr
      0x0060, 0x0101,          // 0x10 .word kData + 0x60 (the futex word)
      0x0000, 0x0000,          // 0x14 padding
      0x6028, 0x4770,          // 0x18 handler: str r0, [r5]; bx lr
      0x27ad, 0xdf00, 0xe7fe,  // 0x1c restorer: movs r7, #173; svc #0; b .
  };
  CHECK_CTX(m.Write(kPage, code, sizeof code), "write the wait and handler bytes");
  CHECK_CTX(m.Protect(kPage, mem::kGuestPage, mem::kRead | mem::kExec, &perr), "make it executable: " + perr);
  const exec::GuestAddr kHandler = kPage + 0x18 + 1;
  const exec::GuestAddr kRestorer = kPage + 0x1c + 1;
  const std::uint32_t zero = 0;
  CHECK(m.Write(kData + 0x60, &zero, 4));

  // The three hooks a real launch installs: handler install, the rt_sigreturn restore, and delivery at
  // the ABI's boundaries.
  const std::uint32_t act[5] = {kHandler, 0x1400'0000u /* SA_RESTART|SA_RESTORER */, kRestorer, 0, 0};
  CHECK(m.Write(kData + 0x20, act, sizeof act));
  exec::MachineState sa{};
  sa.mode = isa::Mode::Thumb;
  sa.pc = kReturn;
  sa.r[7] = syscall::nr::kRtSigaction;
  sa.r[0] = 30;
  sa.r[1] = kData + 0x20;
  r.mon->Handle(sa, 0, &r.abi->main_thread());
  CHECK_CTX(r.mon->disposition(30).catches(), "the handler is recorded");
  r.mon->set_signal_return([&m](GuestThread& th) -> syscall::Outcome {
    std::string why;
    if (!ReturnFromSignal(th, m, &why)) return syscall::Outcome::Refused(syscall::err::kEnosys, why);
    return syscall::Outcome::Emulated(0, why);
  });
  r.abi->set_signal_deliver([&m, &r](GuestThread& th) {
    std::string why;
    (void)DeliverPendingSignal(th, m, *r.mon, &why);  // "nothing pending" is the ordinary answer
  });

  GuestThread t;
  Prepare(t, 1, 6001);
  t.state.pc = kPage;
  t.state.r[15] = kPage;
  t.state.cpsr = exec::CpsrWithMode(t.state.cpsr, isa::Mode::Thumb);
  t.state.r[4] = kData + 0x50;  // where the futex return value lands
  t.state.r[5] = kData + 0x54;  // where the handler writes the signal it saw
  t.state.r[14] = kReturn | 1u;

  std::atomic<bool> done{false};
  exec::RunResult rr{};
  std::thread worker([&] {
    if (!runtime::BindCurrentHostThread(t)) {
      std::fprintf(stderr, "FATAL: the futex fixture thread could not be bound (line %d)\n", __LINE__);
      std::abort();
    }
    rr = r.abi->RunThread(t, 400000);
    done.store(true);
  });
  CHECK_CTX(WaitFor([&] { return ft.waiters() != 0; }, std::chrono::seconds(5)),
            "the guest thread parked in futex WAIT");
  // The waiter is enqueued under the GGL just before the block lambda sets the marker, so wait for the marker
  // rather than sampling at the enqueue instant.
  CHECK_CTX(WaitFor([&] {
              return t.wait_kind.load() == static_cast<int>(GuestThread::WaitKind::kFutex) &&
                     t.wait_arg.load() == kData + 0x60;
            }, std::chrono::seconds(5)),
            "the stall marker names the blocked futex and its address");
  r.mon->QueueSignal(6001, 30);
  CHECK_CTX(WaitFor([&] { return done.load(); }, std::chrono::seconds(5)),
            "the signal cut the wait and the thread ran to its return");
  worker.join();

  std::uint32_t result = 0, handler_saw = 0;
  CHECK_CTX(m.Read(kData + 0x50, &result, 4), "read the futex result");
  CHECK_CTX(m.Read(kData + 0x54, &handler_saw, 4), "read the handler's word");
  CHECK_CTX(result == static_cast<std::uint32_t>(-4),
            "the blocked futex returned -EINTR, is " + std::to_string(result));
  CHECK_CTX(handler_saw == 30, "the handler ran before the guest resumed, saw " + std::to_string(handler_saw));
  CHECK_CTX(rr.reason == exec::StopReason::Returned, "the thread returned: " + rr.detail);
  CHECK_CTX(t.deliveries.empty(), "the delivery was closed by rt_sigreturn");
  CHECK_CTX(t.wait_kind.load() == static_cast<int>(GuestThread::WaitKind::kNone),
            "the wait marker is cleared after the wait");
}

/// ---- C26: a second signal nests inside the first, and each `rt_sigreturn` peels one layer.
///
/// Mono's suspend handler (SIGPWR) waits for the restart signal (SIGXCPU) to run inside it, so delivery
/// must stack: the inner return restores the suspend handler's state, and the outer return restores the
/// code the GC interrupted. One record would overwrite the first frame and resume in the wrong place.
void nested_delivery_restores_the_outer_handler_first() {
  Rig r = Build(2);
  if (!r.ok) return;
  mem::GuestMemory& m = *r.mem;

  const exec::GuestAddr kSuspendHandler = 0x005f'ab59u;   // Mono's handler, Thumb
  const exec::GuestAddr kRestartHandler = 0x005f'ab79u;   // the same code, one entry further
  const exec::GuestAddr kRestorer = 0x001a'7240u;         // not executed here: no run, just frames
  const auto install = [&](int sig, std::uint32_t handler) {
    const std::uint32_t act[5] = {handler, 0x1400'0000u, kRestorer, 0, 0};
    const exec::GuestAddr at = kData + 0x20 + static_cast<exec::GuestAddr>(sig) * 0x20;
    CHECK(m.Write(at, act, sizeof act));
    exec::MachineState sa{};
    sa.mode = isa::Mode::Thumb;
    sa.pc = kReturn;
    sa.r[7] = syscall::nr::kRtSigaction;
    sa.r[0] = static_cast<std::uint32_t>(sig);
    sa.r[1] = at;
    r.mon->Handle(sa, 0, &r.abi->main_thread());
  };
  install(30, kSuspendHandler);
  install(24, kRestartHandler);
  CHECK_CTX(r.mon->disposition(30).catches() && r.mon->disposition(24).catches(),
            "both of Mono's signal dispositions are installed");

  GuestThread& t = r.abi->main_thread();
  const exec::GuestAddr kVictimPc = 0x0100'0124u;  // clean, Thumb
  t.state.mode = isa::Mode::Thumb;
  t.state.cpsr = exec::CpsrWithMode(t.state.cpsr, isa::Mode::Thumb);
  t.state.pc = kVictimPc;
  t.state.r[15] = kVictimPc;
  t.state.r[3] = 0xcafe'0003u;
  t.state.r[14] = kReturn | 1u;
  const std::uint32_t sp_interrupted = t.state.r[13];

  std::string why;
  r.mon->QueueSignal(t.tid, 30);
  CHECK_CTX(DeliverPendingSignal(t, m, *r.mon, &why), "the suspend signal is delivered: " + why);
  CHECK_CTX(t.deliveries.size() == 1 && t.state.pc == (kSuspendHandler & ~1u),
            "depth 1, inside the suspend handler");
  const std::uint32_t suspend_sp = t.state.r[13];  // the handler's stack starts at the frame, not at its ucontext

  r.mon->QueueSignal(t.tid, 24);
  CHECK_CTX(DeliverPendingSignal(t, m, *r.mon, &why), "the restart signal nests: " + why);
  CHECK_CTX(t.deliveries.size() == 2 && t.state.pc == (kRestartHandler & ~1u),
            "depth 2, inside the restart handler");
  CHECK_CTX(t.state.r[13] < suspend_sp, "the inner frame sits below the suspend handler's stack");
  CHECK_CTX(t.state.r[0] == 24, "the inner handler's first argument is the restart signal");

  CHECK_CTX(ReturnFromSignal(t, m, &why), "the inner handler returns: " + why);
  CHECK_CTX(t.deliveries.size() == 1, "one delivery remains");
  CHECK_CTX(t.state.pc == (kSuspendHandler & ~1u), "the suspend handler's pc is back");
  CHECK_CTX(t.state.r[0] == 30, "the suspend handler's r0 (the signal it was entered with) is back");
  CHECK_CTX(t.state.r[13] == suspend_sp, "the suspend handler's stack pointer is back");

  CHECK_CTX(ReturnFromSignal(t, m, &why), "the outer handler returns: " + why);
  CHECK_CTX(t.deliveries.empty(), "no delivery remains");
  CHECK_CTX(t.state.pc == kVictimPc, "the interrupted pc is back");
  CHECK_CTX(t.state.r[3] == 0xcafe'0003u, "the interrupted register file is back");
  CHECK_CTX(t.state.r[13] == sp_interrupted, "the interrupted stack pointer is back");
}

/// ---- C26: a signal the thread has BLOCKED must not wake its futex wait.
///
/// The kernel only interrupts a futex for a deliverable signal. This lane is the counterpart of the one
/// above: same parked thread, same tkill queueing path, but signal 30 is in the thread's mask and stays
/// pending — the wait must run on and return 0 when a real wake arrives, not EINTR. The lookup is what
/// `GuestProcess` installs in a launch; without it the monitor cannot know the target's mask.
void a_blocked_signal_leaves_a_futex_wait_alone(int tier) {
  Watchdog wd(std::string{"blocked_futex_signal/"} + TierName(tier), std::chrono::seconds(30));
  Rig r = Build(tier);
  if (!r.ok) return;
  mem::GuestMemory& m = *r.mem;
  runtime::FutexTable ft;
  r.mon->set_futex_table(&ft);

  const exec::GuestAddr kPage = kCode + 0x3000;
  std::string perr;
  CHECK_CTX(m.Map(kPage, mem::kGuestPage, mem::kRead | mem::kWrite, &perr), "map the futex page: " + perr);
  const std::uint16_t code[] = {
      0x4803,                  // 0x00 ldr r0, [pc, #12] -> the uaddr word at 0x10
      0x6802,                  // 0x02 ldr r2, [r0]
      0x2100,                  // 0x04 movs r1, #0       (FUTEX_WAIT)
      0x2300,                  // 0x06 movs r3, #0       (no timeout)
      0x27f0,                  // 0x08 movs r7, #240     (futex)
      0xdf00,                  // 0x0a svc #0
      0x6020,                  // 0x0c str r0, [r4]
      0x4770,                  // 0x0e bx lr
      0x0060, 0x0101,          // 0x10 .word kData + 0x60
  };
  CHECK_CTX(m.Write(kPage, code, sizeof code), "write the wait bytes");
  CHECK_CTX(m.Protect(kPage, mem::kGuestPage, mem::kRead | mem::kExec, &perr), "make it executable: " + perr);
  const std::uint32_t zero = 0;
  CHECK(m.Write(kData + 0x60, &zero, 4));

  GuestThread t;
  Prepare(t, 1, 6002);
  t.state.pc = kPage;
  t.state.r[15] = kPage;
  t.state.cpsr = exec::CpsrWithMode(t.state.cpsr, isa::Mode::Thumb);
  t.state.r[4] = kData + 0x50;  // the futex result
  t.state.r[13] = kStacks + kStackBytes - 0x100;
  t.state.r[14] = kReturn | 1u;
  t.signal_mask = 1ull << (30 - 1);  // SIGPWR is blocked on this thread
  r.mon->set_thread_lookup([&t](std::int32_t tid) { return tid == t.tid ? &t : nullptr; });

  std::atomic<bool> done{false};
  exec::RunResult rr{};
  std::thread worker([&] {
    if (!runtime::BindCurrentHostThread(t)) {
      std::fprintf(stderr, "FATAL: the blocked-futex fixture thread could not be bound (line %d)\n", __LINE__);
      std::abort();
    }
    rr = r.abi->RunThread(t, 400000);
    done.store(true);
  });
  CHECK_CTX(WaitFor([&] { return ft.waiters() != 0; }, std::chrono::seconds(5)),
            "the guest thread parked in futex WAIT");
  r.mon->QueueSignal(t.tid, 30);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK_CTX(ft.waiters() == 1 && t.wait_kind.load() == static_cast<int>(GuestThread::WaitKind::kFutex),
            "a blocked signal must not interrupt the futex wait");
  CHECK_CTX(r.mon->TakePendingSignal(t.tid) == 30, "the blocked signal stayed pending");
  CHECK_CTX(ft.Wake(kData + 0x60, 1) == 1, "wake the waiter directly");
  CHECK_CTX(WaitFor([&] { return done.load(); }, std::chrono::seconds(5)),
            "the real wake let the thread return");
  worker.join();

  std::uint32_t result = 0xdead'beefu;
  CHECK_CTX(m.Read(kData + 0x50, &result, 4), "read the futex result");
  CHECK_CTX(result == 0, "the futex returned 0 (woken), not -EINTR: " + std::to_string(result));
  CHECK_CTX(rr.reason == exec::StopReason::Returned, "the thread returned: " + rr.detail);
}

int main() {
  monitor_blocking_verdict();
  ggl_handoff_latency();
  bind_refuses_a_live_owner_and_frees_a_dead_one();
  destroyed_guest_thread_leaves_no_dangling_binding();
  for (int tier : {1, 2}) {
    two_host_threads_share_one_process(tier);
    kuser_cmpxchg_shared_by_two_threads(tier);
    busy_thread_does_not_starve(tier);
    ggl_is_released_in_host_calls(tier);
    blocking_syscall_releases_the_ggl(tier);
    unbound_host_thread_is_refused(tier);
    each_thread_has_its_own_tid(tier);
    guest_pthreads(tier);
    signal_delivery_enters_the_guest_handler_and_resumes(tier);
    a_signal_interrupts_a_blocked_futex_and_enters_the_handler(tier);
    a_blocked_signal_leaves_a_futex_wait_alone(tier);
  }
  nested_delivery_restores_the_outer_handler_first();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
