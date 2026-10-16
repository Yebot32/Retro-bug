// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_jni_forward_tests: the forwarding JNI bridge (M3) over a fake Java. A real GuestProcess (the device libc, libm,
// libz) loads libjnibridge_fixture.so — 32-bit guest code making ordinary JNI calls — and runs each fx_* function on
// both tiers with the bridge's guest JNIEnv; FakeJava records what reached "Java". Every row is a value the guest
// computed from what Java answered, so a wrong handle, ID, argument or result shows up as a wrong number.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fake_java.h"
#include "rb/abi/abi.h"
#include "rb/abi/jni_forward.h"
#include "rb/runtime/guest_process.h"

namespace {

using namespace rb;
using abi::Value32;
using test::FakeJava;

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

const char* TierName(int tier) { return tier == 2 ? "J" : "I"; }

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

std::uint64_t Bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}
std::uint64_t Bits(double d) {
  std::uint64_t b = 0;
  std::memcpy(&b, &d, 8);
  return b;
}

/// rb/Fixture (twice, mix, who = 1, <init>(I)V, fields i:I, d:D, static s) and rb/Sub (who = 2).
std::unique_ptr<FakeJava> MakeJava() {
  auto j = std::make_unique<FakeJava>();
  j->DefineClass("java/lang/Object");
  j->DefineClass("rb/Fixture", "java/lang/Object");
  j->DefineClass("rb/Sub", "rb/Fixture");
  j->DefineMethod("rb/Fixture", "twice", "(I)I", true, [](FakeJava&, int, const std::vector<std::uint64_t>& a) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(2 * static_cast<std::int32_t>(a[0])));
  });
  // mix: exactly (5, 3 << 40, 1.5f, 2.25, true) answers 0x1234'5678'9abc; anything else names what arrived.
  j->DefineMethod("rb/Fixture", "mix", "(IJFDZ)J", true, [](FakeJava&, int, const std::vector<std::uint64_t>& a) {
    const bool exact = a.size() == 5 && a[0] == 5 && a[1] == (std::uint64_t{3} << 40) && a[2] == Bits(1.5f) &&
                       a[3] == Bits(2.25) && a[4] == 1;
    if (!exact && a.size() == 5) {
      std::fprintf(stderr, "#   mix got %llx %llx %llx %llx %llx\n", (unsigned long long)a[0], (unsigned long long)a[1],
                   (unsigned long long)a[2], (unsigned long long)a[3], (unsigned long long)a[4]);
    }
    return exact ? std::uint64_t{0x1234'5678'9abc} : std::uint64_t{0};
  });
  j->DefineMethod("rb/Fixture", "who", "()I", false, [](FakeJava&, int, const std::vector<std::uint64_t>&) { return 1u; });
  j->DefineMethod("rb/Sub", "who", "()I", false, [](FakeJava&, int, const std::vector<std::uint64_t>&) { return 2u; });
  j->DefineMethod("rb/Fixture", "<init>", "(I)V", false,
                  [](FakeJava& f, int self, const std::vector<std::uint64_t>& a) {
                    f.PutField(self, "i", a[0]);
                    return 0u;
                  });
  j->DefineField("rb/Fixture", "i", "I", false);
  j->DefineField("rb/Fixture", "d", "D", false);
  j->DefineField("rb/Fixture", "s", "Ljava/lang/Object;", true);
  return j;
}

struct Proc {
  std::unique_ptr<FakeJava> java;
  std::unique_ptr<runtime::GuestProcess> p;
  std::unique_ptr<abi::ForwardingJni> fwd;
  const loader::GuestModule* fixture = nullptr;
  std::string err;
};

Proc Boot(int tier) {
  Proc pr;
  pr.java = MakeJava();
  const auto libc = ReadFile(RB_DEVICE_LIBC_PATH);
  const auto libm = ReadFile(RB_GUEST_LIBM_PATH);
  const auto libz = ReadFile(RB_GUEST_LIBZ_PATH);
  const auto fx = ReadFile(RB_JNIBRIDGE_FIXTURE_PATH);
  if (libc.empty() || libm.empty() || libz.empty() || fx.empty()) {
    pr.err = std::string{"missing images: "} + RB_DEVICE_LIBC_PATH + ", " + RB_GUEST_LIBM_PATH + ", " +
             RB_GUEST_LIBZ_PATH + " or " + RB_JNIBRIDGE_FIXTURE_PATH + " (pushed by tools/run_engine_tests.sh)";
    return pr;
  }
  // The dynamic-linker imports libc's constructors call, answered as guest_thread_test answers them.
  static const char* const kDl[] = {"dlopen", "android_dlopen_ext", "dlsym", "dlclose", "dladdr", "dlerror",
                                    "dl_unwind_find_exidx", "android_get_application_target_sdk_version"};
  constexpr std::uint32_t kDlSlotBase = 400;
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
  if (!pr.p->LoadScope(libc, libm, libz, {}, &pr.err)) {
    pr.p.reset();
    return pr;
  }
  pr.fixture = pr.p->LoadApp(fx, "libjnibridge_fixture.so", &pr.err);
  std::string failed;
  if (pr.fixture == nullptr || !pr.p->RunConstructors(&failed)) {
    if (pr.err.empty()) pr.err = "RunConstructors: " + failed;
    pr.p.reset();
    return pr;
  }
  const abi::GuestAddr page = pr.p->MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &pr.err);
  if (page == 0) {
    pr.p.reset();
    return pr;
  }
  pr.fwd = std::make_unique<abi::ForwardingJni>(pr.p->memory(), pr.p->abi(), page, *pr.java, &pr.err);
  if (!pr.fwd->ok()) {
    pr.fwd.reset();
    pr.p.reset();
  }
  return pr;
}

struct Result {
  abi::Ret32 ret{};
  exec::RunResult rr{};
};

Result Call(Proc& pr, int tier, const char* fn, std::vector<Value32> args, const char* shorty = "I") {
  Result r;
  const auto at = pr.fixture->FindExport(fn);
  if (!at) {
    r.rr.reason = exec::StopReason::Internal;
    r.rr.detail = std::string{"the fixture exports no "} + fn;
    return r;
  }
  pr.p->abi().set_executor(pr.p->engine(tier));
  args.insert(args.begin(), Value32::Ptr(pr.fwd->env()));
  r.ret = pr.p->abi().CallGuest(at->tagged(), args, abi::Shorty::For(shorty), 2'000'000'000u, &r.rr);
  return r;
}

bool Returned(const Result& r) { return r.rr.reason == exec::StopReason::Returned; }

void find_class_and_static_call(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_find_and_call", {});
  CHECK_CTX(Returned(r), r.rr.detail);
  CHECK_CTX(r.ret.i() == 42, std::string{"Tier "} + TierName(tier) + ": twice(21) = " + std::to_string(r.ret.i()));
  CHECK(pr.java->calls("FindClass(rb/Fixture)") == 1u && pr.java->calls("twice") == 1u);
}

void ids_are_a_bijection(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_ids", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 11, r.rr.detail + " -> " + std::to_string(r.ret.i()));
}

void fields_round_trip(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::HostRef obj = pr.java->NewInstance("rb/Fixture");
  const abi::GuestAddr g = pr.fwd->ToGuestLocal(obj);
  const Result r = Call(pr, tier, "fx_fields", {Value32::Ptr(g)});
  CHECK_CTX(Returned(r) && r.ret.i() == 112, r.rr.detail + " -> " + std::to_string(r.ret.i()));
  const int o = pr.java->ObjectOf(obj);
  CHECK(pr.java->FieldOf(o, "i") == 7u && pr.java->FieldOf(o, "d") == Bits(2.5));
  CHECK(pr.java->StaticOf("rb/Fixture", "s") == static_cast<std::uint64_t>(o));
}

void virtual_vs_nonvirtual(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::GuestAddr g = pr.fwd->ToGuestLocal(pr.java->NewInstance("rb/Sub"));
  const Result r = Call(pr, tier, "fx_virtual_vs_nonvirtual", {Value32::Ptr(g)});
  CHECK_CTX(Returned(r) && r.ret.i() == 21, r.rr.detail + " -> " + std::to_string(r.ret.i()));
}

void args_in_all_three_forms(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_args_in_all_forms", {}, "J");
  CHECK_CTX(Returned(r) && r.ret.j() == 0x1234'5678'9abc,
            r.rr.detail + " -> " + std::to_string(r.ret.j()) + " (0 = an argument arrived wrong, -1 = the forms disagree)");
  CHECK(pr.java->calls("mix") == 3u);
}

void new_object_runs_the_constructor(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_new_object", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 5, r.rr.detail + " -> " + std::to_string(r.ret.i()));
}

void many_locals_in_one_call(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  pr.fwd->PushFrame();
  const std::size_t before = pr.fwd->live_locals();
  const Result r = Call(pr, tier, "fx_locals", {Value32::S32(2000)});
  CHECK_CTX(Returned(r) && r.ret.i() == 1000, r.rr.detail + " -> " + std::to_string(r.ret.i()));
  pr.fwd->PopFrame(0);
  CHECK_CTX(pr.fwd->live_locals() <= before, std::to_string(pr.fwd->live_locals()) + " live locals after the frame");
}

void stale_local_is_refused_by_name(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_use_deleted", {});
  CHECK_CTX(r.rr.reason == exec::StopReason::Fault && r.rr.detail.find("stale local reference") != std::string::npos,
            r.rr.detail);
  const Result again = Call(pr, tier, "fx_find_and_call", {});
  CHECK_CTX(Returned(again) && again.ret.i() == 42, "the process after a refusal: " + again.rr.detail);
}

void foreign_thread_local_is_refused(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result made = Call(pr, tier, "fx_make_local", {}, "L");
  CHECK_CTX(Returned(made) && made.ret.w() != 0, made.rr.detail);
  Result used;
  std::thread b([&] { used = Call(pr, tier, "fx_class_of", {Value32::Ptr(made.ret.w())}); });
  b.join();
  CHECK_CTX(used.rr.reason == exec::StopReason::Fault &&
                used.rr.detail.find("another thread's local reference") != std::string::npos,
            used.rr.detail);
  const Result own = Call(pr, tier, "fx_class_of", {Value32::Ptr(made.ret.w())});
  CHECK_CTX(Returned(own) && own.ret.i() == 1, own.rr.detail);
}

void ggl_released_during_java(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  // While thread A is inside "Java" (twice), thread B makes a guest call. If the slot held the GGL, B could not
  // run guest code until A returned, and A waits for B: the wait times out and the row names it.
  std::mutex mu;
  std::condition_variable cv;
  bool b_done = false;
  bool timed_out = false;
  pr.java->DefineMethod("rb/Fixture", "twice", "(I)I", true, [&](FakeJava&, int, const std::vector<std::uint64_t>& a) {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(5), [&] { return b_done; });
    return static_cast<std::uint64_t>(2 * a[0]);
  });
  Result b_result;
  std::thread b([&] {
    // Wait until A is inside Java, then run a plain guest function.
    while (pr.java->calls("twice") == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    pr.p->abi().set_executor(pr.p->engine(tier));
    const auto at = pr.fixture->FindExport("fx_plain");
    b_result.ret = pr.p->abi().CallGuest(at->tagged(), {Value32::S32(41)}, abi::Shorty::For("I"), 1'000'000u,
                                         &b_result.rr);
    std::lock_guard<std::mutex> lk(mu);
    b_done = true;
    cv.notify_all();
  });
  const Result a = Call(pr, tier, "fx_find_and_call", {});
  b.join();
  CHECK_CTX(!timed_out, "thread B could not run guest code while thread A was inside Java: the GGL was held");
  CHECK_CTX(Returned(a) && a.ret.i() == 42, a.rr.detail);
  CHECK_CTX(Returned(b_result) && b_result.ret.i() == 42, b_result.rr.detail);
}

}  // namespace

int main() {
  for (int tier : {1, 2}) {
    find_class_and_static_call(tier);
    ids_are_a_bijection(tier);
    fields_round_trip(tier);
    virtual_vs_nonvirtual(tier);
    args_in_all_three_forms(tier);
    new_object_runs_the_constructor(tier);
    many_locals_in_one_call(tier);
    stale_local_is_refused_by_name(tier);
    foreign_thread_local_is_refused(tier);
    ggl_released_during_java(tier);
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
