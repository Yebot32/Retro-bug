// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_inbound_tests (arm64 hosts): Java calling guest natives. Each row binds a fixture native through the inbound
// trampoline pool and calls the pointer it gets as a real AArch64 function, with the arguments where ART puts them
// (x0 JNIEnv*, x1 this/class, then x2.., d0.., the stack). The guest runs on both tiers; the result must equal what
// the same C code computes on the host.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bridge_rig.h"
#include "rb/runtime/inbound.h"

namespace {

using namespace rb;
using test::Proc;

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

using MixFn = std::int64_t (*)(void*, void*, std::int32_t, std::int64_t, float, double, std::int32_t, std::int64_t,
                               float, double, std::int32_t, std::int32_t, std::int32_t, std::int32_t, float, float,
                               double, double, float, double);

/// The fixture's Java_rb_Fixture_mix, computed on the host.
std::int64_t MixReference(std::int32_t a, std::int64_t b, float c, double d, std::int32_t e, std::int64_t f, float g,
                          double h, std::int32_t i, std::int32_t j, std::int32_t k, std::int32_t l, float m, float n,
                          double o, double p, float q, double r) {
  return a + b * 3 + static_cast<std::int32_t>(c * 8) + static_cast<std::int32_t>(d * 16) + e * 5 + f * 7 +
         static_cast<std::int32_t>(g * 32) + static_cast<std::int32_t>(h * 64) + i * 11 + j * 13 + k * 17 + l * 19 +
         static_cast<std::int32_t>(m * 128) + static_cast<std::int32_t>(n * 256) + static_cast<std::int32_t>(o * 512) +
         static_cast<std::int32_t>(p * 1024) + static_cast<std::int32_t>(q * 2048) + static_cast<std::int32_t>(r * 4096);
}

struct Bound {
  Proc pr;
  std::unique_ptr<runtime::Inbound> in;
  abi::HostRef cls = 0;
};

Bound Make(int tier) {
  Bound b;
  b.pr = test::Boot(tier);
  if (!b.pr.p) return b;
  b.in = std::make_unique<runtime::Inbound>(*b.pr.p, *b.pr.fwd, *b.pr.java, tier);
  b.cls = b.pr.java->FindClass("rb/Fixture");
  return b;
}

void* Bind(Bound& b, const char* name, const char* shorty) {
  const auto at = b.pr.fixture->FindExport(name);
  if (!at) return nullptr;
  std::string e;
  void* fn = b.in->BindPool(runtime::InboundTarget{at->tagged(), shorty, name}, &e);
  if (fn == nullptr) std::fprintf(stderr, "#   BindPool %s: %s\n", name, e.c_str());
  return fn;
}

void every_argument_lands_where_the_guest_reads_it(int tier) {
  Bound b = Make(tier);
  CHECK_CTX(b.pr.p != nullptr, b.pr.err);
  if (!b.pr.p) return;
  auto mix = reinterpret_cast<MixFn>(Bind(b, "Java_rb_Fixture_mix", "JIJFDIJFDIIIIFFDDFD"));
  CHECK(mix != nullptr);
  if (mix == nullptr) return;
  const std::int64_t want = MixReference(3, std::int64_t{5} << 33, 1.5f, 2.25, -7, -(std::int64_t{1} << 40), 0.75f,
                                         -0.5, 100, 200, 300, 400, 0.125f, 0.25f, 0.375, 0.5, 0.625f, 0.875);
  const std::int64_t got = mix(nullptr, reinterpret_cast<void*>(b.cls), 3, std::int64_t{5} << 33, 1.5f, 2.25, -7,
                               -(std::int64_t{1} << 40), 0.75f, -0.5, 100, 200, 300, 400, 0.125f, 0.25f, 0.375, 0.5,
                               0.625f, 0.875);
  CHECK_CTX(got == want, std::string{"Tier "} + (tier == 2 ? "J" : "I") + ": guest " + std::to_string(got) + ", host " +
                             std::to_string(want));
  CHECK(b.pr.java->PendingClass().empty());
}

void float_and_double_results_come_back_in_s0_and_d0(int tier) {
  Bound b = Make(tier);
  CHECK_CTX(b.pr.p != nullptr, b.pr.err);
  if (!b.pr.p) return;
  auto half = reinterpret_cast<double (*)(void*, void*, float)>(Bind(b, "Java_rb_Fixture_half", "DF"));
  auto third = reinterpret_cast<float (*)(void*, void*, double)>(Bind(b, "Java_rb_Fixture_third", "FD"));
  CHECK(half != nullptr && third != nullptr);
  if (half == nullptr || third == nullptr) return;
  CHECK_CTX(half(nullptr, reinterpret_cast<void*>(b.cls), 3.5f) == 1.75, std::to_string(half(nullptr, nullptr, 3.5f)));
  const float t = third(nullptr, reinterpret_cast<void*>(b.cls), 1.5);
  CHECK_CTX(t == static_cast<float>(1.5 / 3.0), std::to_string(t));
}

void a_returned_object_is_the_callers_and_the_frame_is_gone(int tier) {
  Bound b = Make(tier);
  CHECK_CTX(b.pr.p != nullptr, b.pr.err);
  if (!b.pr.p) return;
  auto echo = reinterpret_cast<abi::HostRef (*)(void*, void*, abi::HostRef)>(
      Bind(b, "Java_rb_Fixture_echoObject", "LL"));
  CHECK(echo != nullptr);
  if (echo == nullptr) return;
  const abi::HostRef obj = b.pr.java->NewInstance("rb/Fixture");
  const std::size_t before = b.pr.fwd->live_locals();
  const abi::HostRef got = echo(nullptr, reinterpret_cast<void*>(b.cls), obj);
  CHECK_CTX(got != 0 && b.pr.java->IsSameObject(got, obj), "echoObject returned " + std::to_string(got));
  CHECK_CTX(b.pr.fwd->live_locals() == before, std::to_string(b.pr.fwd->live_locals()) + " live guest locals after, " +
                                                    std::to_string(before) + " before");
}

void a_guest_fault_is_a_java_error_naming_it(int tier) {
  Bound b = Make(tier);
  CHECK_CTX(b.pr.p != nullptr, b.pr.err);
  if (!b.pr.p) return;
  auto fails = reinterpret_cast<std::int32_t (*)(void*, void*)>(Bind(b, "Java_rb_Fixture_fails", "I"));
  CHECK(fails != nullptr);
  if (fails == nullptr) return;
  CHECK(fails(nullptr, reinterpret_cast<void*>(b.cls)) == 0);
  CHECK_CTX(b.pr.java->PendingClass() == "java/lang/Error", "pending: '" + b.pr.java->PendingClass() + "'");
  // And the process still works: the next native call runs.
  b.pr.java->ExceptionClear();
  auto half = reinterpret_cast<double (*)(void*, void*, float)>(Bind(b, "Java_rb_Fixture_half", "DF"));
  CHECK(half != nullptr && half(nullptr, reinterpret_cast<void*>(b.cls), 1.0f) == 0.5);
}

void a_bad_shorty_is_refused_by_name(int tier) {
  Bound b = Make(tier);
  CHECK_CTX(b.pr.p != nullptr, b.pr.err);
  if (!b.pr.p) return;
  std::string e;
  CHECK(b.in->BindPool(runtime::InboundTarget{0x1001, "IQ", "bad"}, &e) == nullptr &&
        e.find("not a JNI shorty") != std::string::npos);
  CHECK(runtime::ShortyOf("(IJLjava/lang/String;[B)Z") == "ZIJLL" && runtime::ShortyOf("(Q)V").empty());
}

}  // namespace

int main() {
  for (int tier : {1, 2}) {
    every_argument_lands_where_the_guest_reads_it(tier);
    float_and_double_results_come_back_in_s0_and_d0(tier);
    a_returned_object_is_the_callers_and_the_frame_is_gone(tier);
    a_guest_fault_is_a_java_error_naming_it(tier);
    a_bad_shorty_is_refused_by_name(tier);
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
