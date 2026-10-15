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
 * `HANG <row>` line and a non-zero exit instead of a suite that never finishes.
 */
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        std::fprintf(stderr, "HANG  %s: still running after %lld s — a guest thread is deadlocked or starved\n",
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
    runtime::BindCurrentHostThread(t);
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
      runtime::BindCurrentHostThread(t);
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
  });
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
    runtime::BindCurrentHostThread(b);
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

/// Ruling 1: a host thread with no binding, on a context whose main thread is bound elsewhere, is refused
/// by name — never silently run on somebody else's registers.
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
                c.rr.detail.find("host thread not bound to a guest thread; adoption arrives in Task 5") !=
                    std::string::npos,
            Why(c));
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

}  // namespace

int main() {
  monitor_blocking_verdict();
  for (int tier : {1, 2}) {
    two_host_threads_share_one_process(tier);
    kuser_cmpxchg_shared_by_two_threads(tier);
    busy_thread_does_not_starve(tier);
    ggl_is_released_in_host_calls(tier);
    blocking_syscall_releases_the_ggl(tier);
    unbound_host_thread_is_refused(tier);
    each_thread_has_its_own_tid(tier);
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
