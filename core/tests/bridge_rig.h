// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The forwarding-JNI-bridge rig shared by rb_jni_forward_tests and rb_inbound_tests: a GuestProcess (the device
// libc, libm, libz) with libjnibridge_fixture.so loaded and constructed, a FakeJava modelling rb/Fixture and rb/Sub,
// and a ForwardingJni over them with a guest allocator.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fake_java.h"
#include "rb/abi/abi.h"
#include "rb/abi/jni_forward.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/guest_process.h"

namespace rb::test {

using abi::Value32;

inline std::vector<std::uint8_t> ReadFile(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return {};
  std::vector<std::uint8_t> out;
  std::uint8_t buf[65536];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

inline std::uint64_t Bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  return b;
}
inline std::uint64_t Bits(double d) {
  std::uint64_t b = 0;
  std::memcpy(&b, &d, 8);
  return b;
}

/// rb/Fixture (twice, mix, who = 1, <init>(I)V, fields i:I, d:D, static s) and rb/Sub (who = 2).
inline std::unique_ptr<FakeJava> MakeJava() {
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
  j->DefineClass("java/lang/IllegalStateException", "java/lang/Object");
  j->DefineClass("java/lang/Error", "java/lang/Object");
  return j;
}

struct Proc {
  std::unique_ptr<FakeJava> java;
  std::unique_ptr<runtime::GuestProcess> p;
  std::unique_ptr<abi::ForwardingJni> fwd;
  const loader::GuestModule* fixture = nullptr;
  std::string err;
};

inline Proc Boot(int tier) {
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
  if (pr.fwd) {
    runtime::GuestProcess* const p = pr.p.get();
    pr.fwd->set_allocator([p](std::uint32_t bytes, std::string* e) {
      runtime::Ggl::Held hold(p->ggl());
      return p->MapFree(bytes, mem::kRead | mem::kWrite, e);
    });
  }
  return pr;
}


}  // namespace rb::test
