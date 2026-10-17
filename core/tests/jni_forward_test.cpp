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

#include "bridge_rig.h"
#include "fake_java.h"
#include "rb/abi/abi.h"
#include "rb/abi/jni_forward.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/guest_process.h"

namespace {

using namespace rb;
using abi::Value32;
using test::FakeJava;
using test::Proc;
using test::Boot;
using test::Bits;

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


// ---------------------------------------------------------------------------------------------- Task 2 rows

unsigned Fnv(const std::vector<std::uint8_t>& b) {
  unsigned h = 2166136261u;
  for (const std::uint8_t c : b) h = (h ^ c) * 16777619u;
  return h;
}

/// A guest scratch page, filled with `bytes` at its start.
abi::GuestAddr Scratch(Proc& pr, const std::vector<std::uint8_t>& bytes = {}) {
  std::string e;
  runtime::Ggl::Held hold(pr.p->ggl());
  const abi::GuestAddr at = pr.p->MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (at != 0 && !bytes.empty()) pr.p->memory().Write(at, bytes.data(), bytes.size());
  return at;
}

bool HasNote(const Proc& pr, const std::string& needle) {
  for (const std::string& n : pr.fwd->notes()) {
    if (n.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string Notes(const Proc& pr) {
  std::string out;
  for (const std::string& n : pr.fwd->notes()) out += (out.empty() ? "" : " | ") + n;
  return out;
}

void mutf8_round_trips(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  // "a\0b😀é": the device ART's GetStringUTFChars (tests/corpus/oracle-jnibridge.txt, SM-X710 Android 16) gives NUL as
  // C0 80 and a well-formed surrogate pair as one 4-byte UTF-8 sequence: 10 bytes, FNV-1a 0x379bd93b.
  const std::u16string text{u'a', u'\0', u'b', char16_t{0xD83D}, char16_t{0xDE00}, char16_t{0xE9}};
  const std::vector<std::uint8_t> mutf8{0x61, 0xC0, 0x80, 0x62, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9};
  const abi::GuestAddr out = Scratch(pr);
  const abi::GuestAddr s = pr.fwd->ToGuestLocal(pr.java->NewJavaString(text));
  const Result r = Call(pr, tier, "fx_string_info", {Value32::Ptr(s), Value32::Ptr(out)});
  CHECK_CTX(Returned(r) && r.ret.i() == 0, r.rr.detail);
  std::int32_t got[7] = {};
  pr.p->memory().Read(out, got, sizeof got);
  CHECK_CTX(got[0] == 10 && got[1] == 6, "UTF length " + std::to_string(got[0]) + ", UTF-16 length " + std::to_string(got[1]));
  CHECK_CTX(static_cast<unsigned>(got[2]) == Fnv(mutf8) && Fnv(mutf8) == 0x379bd93bu, "the MUTF-8 bytes the guest saw differ from ART's");
  CHECK(got[3] == 1);
  CHECK_CTX(got[4] == (0 | (0x62 << 16)), "GetStringRegion(1, 2) = " + std::to_string(got[4]));
  // The device ART writes a NUL after the region (oracle-jnibridge.txt's strings row; the JNI spec is silent).
  CHECK_CTX(static_cast<unsigned>(got[5]) == Fnv({0x61, 0xC0, 0x80, 0x00}), "GetStringUTFRegion(0, 2) and its NUL");
  CHECK(got[6] == 0x61);
  // Back the other way: the same bytes through NewStringUTF give the same string.
  std::vector<std::uint8_t> z = mutf8;
  z.push_back(0);
  const Result back = Call(pr, tier, "fx_new_string_utf", {Value32::Ptr(Scratch(pr, z))}, "L");
  std::string e;
  CHECK_CTX(Returned(back) && pr.java->StringOf(pr.java->ObjectOf(pr.fwd->FromGuest(back.ret.w(), &e))) == text,
            back.rr.detail + e);
  // Standard 4-byte UTF-8 becomes a surrogate pair, as ART's converter does.
  const Result four = Call(pr, tier, "fx_new_string_utf", {Value32::Ptr(Scratch(pr, {0xF0, 0x9F, 0x98, 0x80, 0}))}, "L");
  CHECK(Returned(four) && pr.java->StringOf(pr.java->ObjectOf(pr.fwd->FromGuest(four.ret.w(), &e))) ==
                              std::u16string{char16_t{0xD83D}, char16_t{0xDE00}});
  // Invalid bytes are replaced (U+FFFD) and named, never handed to Java as they are.
  const Result bad = Call(pr, tier, "fx_new_string_utf", {Value32::Ptr(Scratch(pr, {0xFF, 0x41, 0}))}, "L");
  CHECK_CTX((Returned(bad) && pr.java->StringOf(pr.java->ObjectOf(pr.fwd->FromGuest(bad.ret.w(), &e))) ==
                                  std::u16string{char16_t{0xFFFD}, u'A'}),
            bad.rr.detail);
  CHECK_CTX(HasNote(pr, "sanitized"), Notes(pr));
  // NewString from UTF-16 code units.
  const std::vector<std::uint8_t> units{0x3a, 0x26, 0x78, 0x00};  // U+263A 'x', little-endian
  const Result ns = Call(pr, tier, "fx_new_string", {Value32::Ptr(Scratch(pr, units)), Value32::S32(2)}, "L");
  CHECK(Returned(ns) && pr.java->StringOf(pr.java->ObjectOf(pr.fwd->FromGuest(ns.ret.w(), &e))) ==
                            std::u16string{char16_t{0x263A}, u'x'});
}

void arrays_every_kind(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::GuestAddr obj = pr.fwd->ToGuestLocal(pr.java->NewInstance("rb/Fixture"));
  const Result r = Call(pr, tier, "fx_arrays", {Value32::Ptr(obj)});
  CHECK_CTX(Returned(r) && r.ret.i() == 0x1ff, r.rr.detail + " -> bits " + std::to_string(r.ret.i()) + " of 511");
}

void pending_exception_rules(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_exceptions", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 31, r.rr.detail + " -> bits " + std::to_string(r.ret.i()) + " of 31");
  CHECK_CTX(HasNote(pr, "while an exception is pending"), Notes(pr));
  CHECK(pr.java->PendingClass().empty());
}

void local_frames(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_frames", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 3, r.rr.detail + " -> " + std::to_string(r.ret.i()));
  // A local whose frame popped stays refused even after its slot is reused (the serial moved on).
  const Result stale = Call(pr, tier, "fx_use_after_pop", {});
  CHECK_CTX(stale.rr.reason == exec::StopReason::Fault && stale.rr.detail.find("stale local reference") != std::string::npos,
            stale.rr.detail);
}

void monitor_pairs(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::HostRef o = pr.java->NewInstance("rb/Fixture");
  const Result r = Call(pr, tier, "fx_monitor", {Value32::Ptr(pr.fwd->ToGuestLocal(o))});
  CHECK_CTX(Returned(r) && r.ret.i() == 0, r.rr.detail);
  CHECK(pr.java->monitor_depth(pr.java->ObjectOf(o)) == 0);
}

void register_natives_reaches_the_binder(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result refused = Call(pr, tier, "fx_register", {});
  CHECK_CTX(refused.rr.reason == exec::StopReason::Fault && refused.rr.detail.find("no native binder") != std::string::npos,
            refused.rr.detail);
  abi::GuestAddr seen_fn = 0;
  std::string seen_name, seen_sig;
  static int marker = 0;
  pr.fwd->set_native_binder([&](abi::GuestAddr fn, const std::string& name, const std::string& sig, std::string*) {
    seen_fn = fn;
    seen_name = name;
    seen_sig = sig;
    return static_cast<void*>(&marker);
  });
  const Result r = Call(pr, tier, "fx_register", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 0, r.rr.detail);
  const auto add = pr.fixture->FindExport("fx_native_add");
  CHECK_CTX(add && seen_fn == add->tagged(), "binder saw " + std::to_string(seen_fn));
  CHECK(seen_name == "nativeAdd" && seen_sig == "(II)I");
  const auto natives = pr.java->natives("rb/Fixture");
  CHECK(natives.size() == 1u && natives[0].name == "nativeAdd" && natives[0].sig == "(II)I" &&
        natives[0].host_fn == static_cast<void*>(&marker));
}

void java_vm_get_env(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_vm", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 7, r.rr.detail + " -> bits " + std::to_string(r.ret.i()) + " of 7");
}

void direct_buffer_over_guest_memory(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::GuestAddr page = Scratch(pr);
  const Result r = Call(pr, tier, "fx_direct", {Value32::Ptr(page)});
  CHECK_CTX(Returned(r) && r.ret.i() == 3, r.rr.detail + " -> " + std::to_string(r.ret.i()));
  // A Java buffer over host memory (ByteBuffer.allocateDirect, a mapped file): the guest gets a shadow copy in its
  // window, and writes on either side cross at the next JNI transition (M4 host-memory window service).
  static std::uint8_t host_only[8];
  std::memcpy(host_only, "ABCDEFGH", 8);
  std::uint8_t seen_by_java = 0;
  pr.java->DefineMethod("rb/Fixture", "touch", "()V", true,
                        [&seen_by_java](test::FakeJava&, int, const std::vector<std::uint64_t>&) -> std::uint64_t {
                          seen_by_java = host_only[0];
                          host_only[1] = 'y';
                          return 0;
                        });
  const abi::GuestAddr b = pr.fwd->ToGuestLocal(pr.java->NewHostDirectBuffer(host_only, 8));
  const Result h = Call(pr, tier, "fx_direct_rw", {Value32::Ptr(b)});
  const int sum = 'A' + 'B' + 'C' + 'D' + 'E' + 'F' + 'G' + 'H';
  CHECK_CTX(Returned(h) && h.ret.i() == sum * 256 + 'y',
            h.rr.detail + " -> " + std::to_string(h.ret.i()) + " (want " + std::to_string(sum * 256 + 'y') + ")");
  CHECK_CTX(seen_by_java == 'Z', std::string{"Java saw byte 0 = "} + static_cast<char>(seen_by_java));
  CHECK_CTX(host_only[0] == 'Z' && host_only[1] == 'y', "host buffer after: " + std::string(host_only, host_only + 8));
}

void reflection_round_trip(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const Result r = Call(pr, tier, "fx_reflect", {});
  CHECK_CTX(Returned(r) && r.ret.i() == 8, r.rr.detail + " -> " + std::to_string(r.ret.i()));
}
}  // namespace

// ---- final-review rows

/// Deleting a global, weak global or local twice: release ART logs it and carries on, so the native runs to its
/// end; the bridge names it in a note.
void double_delete_is_tolerated(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::GuestAddr o = pr.fwd->ToGuestLocal(pr.java->NewInstance("rb/Fixture"));
  const Result r = Call(pr, tier, "fx_double_delete", {Value32::Ptr(o)});
  CHECK_CTX(Returned(r) && r.ret.i() == 7, r.rr.detail + " / " + Notes(pr));
  CHECK_CTX(pr.java->PendingClass().empty(), "pending: '" + pr.java->PendingClass() + "'");
  CHECK_CTX(HasNote(pr, "already deleted"), Notes(pr));
}

/// A native's argument used after that native returned. Gumball's gumballJNIBegin saves `this` in a static on
/// every entry and never restores it, so a nested native (a Java callback's) leaves its own `this` there and the
/// outer native then calls GetObjectClass on it. ART reads a native's arguments from the call's stack, unchecked
/// without CheckJNI, so this works on a device: the bridge answers it with the object. A plain local of the same
/// popped frame stays refused, and a DeleteLocalRef of the old argument is ignored.
void argument_used_after_its_native_returned(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const abi::HostRef obj = pr.java->NewInstance("rb/Fixture");
  pr.fwd->PushFrame();  // the outer native
  pr.fwd->PushFrame();  // the nested one: `this` is its argument
  const abi::GuestAddr self = pr.fwd->ToGuestLocal(pr.java->NewLocalRef(obj));
  pr.fwd->ArgumentsIn();
  const abi::GuestAddr made = pr.fwd->ToGuestLocal(pr.java->NewLocalRef(obj));
  pr.fwd->PopFrame(0);
  // The outer native goes on making locals: the popped slots are reused under new serials.
  const abi::GuestAddr reuse1 = pr.fwd->ToGuestLocal(pr.java->NewLocalRef(obj));
  const abi::GuestAddr reuse2 = pr.fwd->ToGuestLocal(pr.java->NewLocalRef(obj));
  CHECK(((reuse1 ^ self) & 0xfffc0u) == 0 || ((reuse2 ^ self) & 0xfffc0u) == 0);  // the same slot, reused
  const Result r = Call(pr, tier, "fx_class_of", {Value32::Ptr(self)});
  CHECK_CTX(Returned(r) && r.ret.i() == 1, r.rr.detail);
  CHECK_CTX(HasNote(pr, "argument used after that native returned"), Notes(pr));
  const Result live = Call(pr, tier, "fx_class_of", {Value32::Ptr(reuse1)});
  CHECK_CTX(Returned(live) && live.ret.i() == 1, live.rr.detail);
  const Result del = Call(pr, tier, "fx_double_delete", {Value32::Ptr(self)});
  CHECK_CTX(Returned(del) && del.ret.i() == 7, del.rr.detail + " / " + Notes(pr));
  const Result still = Call(pr, tier, "fx_class_of", {Value32::Ptr(reuse1)});
  CHECK_CTX(Returned(still) && still.ret.i() == 1, "the reused slot after deleting the old argument: " + still.rr.detail);
  const Result plain = Call(pr, tier, "fx_class_of", {Value32::Ptr(made)});
  CHECK_CTX(plain.rr.reason == exec::StopReason::Fault &&
                plain.rr.detail.find("stale local reference") != std::string::npos,
            plain.rr.detail);
  pr.fwd->PopFrame(0);
}

/// Attach, make a local, detach — without DeleteLocalRef, as detach frees a thread's locals on ART: 20000 events
/// (past the 16384-slot table) leave no live local behind.
void detach_frees_the_threads_locals(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  const std::size_t before = pr.fwd->live_locals();
  const Result r = Call(pr, tier, "fx_attach_cycles", {Value32::Ptr(pr.fwd->vm()), Value32::S32(20000)});
  CHECK_CTX(Returned(r) && r.ret.i() == 0, "fx_attach_cycles = " + std::to_string(r.ret.i()) + " " + r.rr.detail +
                                               " / " + Notes(pr));
  CHECK_CTX(pr.fwd->live_locals() <= before, std::to_string(pr.fwd->live_locals()) + " live locals after, " +
                                                 std::to_string(before) + " before");
}

/// NewStringUTF of a 6000-byte string (a game's JSON or save blob): no length cap of the bridge's own.
void long_new_string_utf(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  std::vector<std::uint8_t> big(6000, 'x');
  big.push_back(0);
  std::string e;
  abi::GuestAddr at = 0;
  {
    runtime::Ggl::Held hold(pr.p->ggl());
    at = pr.p->MapFree(3 * mem::kGuestPage, mem::kRead | mem::kWrite, &e);
    pr.p->memory().Write(at, big.data(), big.size());
  }
  const Result r = Call(pr, tier, "fx_new_string_utf", {Value32::Ptr(at)}, "L");
  CHECK_CTX(Returned(r) && r.ret.w() != 0, r.rr.detail + " / " + Notes(pr));
  if (Returned(r) && r.ret.w() != 0) {
    const std::u16string got = pr.java->StringOf(pr.java->ObjectOf(pr.fwd->FromGuest(r.ret.w(), &e)));
    CHECK_CTX(got.size() == 6000u, std::to_string(got.size()) + " chars");
  }
}

/// A device-identifier read refused at a raised targetSdk (TelephonyManager.getDeviceId throws SecurityException at
/// 29+) comes back to the guest as null with nothing pending — what a phone without the permission answered the game
/// was written for — and a SecurityException from anywhere else stays pending, untouched. The fake builds the
/// throwable's stack the way the platform's is: framework frames first, then the app's.
void device_identifier_refusals_read_as_null(int tier) {
  Proc pr = Boot(tier);
  CHECK_CTX(pr.p != nullptr, pr.err);
  if (!pr.p) return;
  FakeJava& j = *pr.java;
  j.DefineClass("java/lang/Throwable", "java/lang/Object");
  j.DefineClass("java/lang/Exception", "java/lang/Throwable");
  j.DefineClass("java/lang/RuntimeException", "java/lang/Exception");
  j.DefineClass("java/lang/SecurityException", "java/lang/RuntimeException");
  j.DefineClass("java/lang/StackTraceElement", "java/lang/Object");
  j.DefineField("java/lang/Throwable", "trace", "[Ljava/lang/StackTraceElement;", false);
  j.DefineField("java/lang/StackTraceElement", "cls", "Ljava/lang/String;", false);
  j.DefineField("java/lang/StackTraceElement", "meth", "Ljava/lang/String;", false);
  j.DefineMethod("java/lang/Throwable", "getStackTrace", "()[Ljava/lang/StackTraceElement;", false,
                 [](FakeJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(static_cast<int>(f.FieldOf(self, "trace"))));
                 });
  j.DefineMethod("java/lang/StackTraceElement", "getClassName", "()Ljava/lang/String;", false,
                 [](FakeJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(static_cast<int>(f.FieldOf(self, "cls"))));
                 });
  j.DefineMethod("java/lang/StackTraceElement", "getMethodName", "()Ljava/lang/String;", false,
                 [](FakeJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(static_cast<int>(f.FieldOf(self, "meth"))));
                 });
  // Throws a SecurityException whose stack is `frames` (class, method), top first.
  const auto thrower = [](std::vector<std::pair<std::u16string, std::u16string>> frames) {
    return [frames](FakeJava& f, int, const std::vector<std::uint64_t>&) -> std::uint64_t {
      const abi::HostRef ex = f.NewInstance("java/lang/SecurityException");
      const abi::HostRef arr =
          f.NewArray('L', static_cast<std::int32_t>(frames.size()), f.FindClass("java/lang/StackTraceElement"), 0);
      for (std::size_t k = 0; k < frames.size(); ++k) {
        const abi::HostRef e = f.NewInstance("java/lang/StackTraceElement");
        f.PutField(f.ObjectOf(e), "cls", static_cast<std::uint64_t>(f.ObjectOf(f.NewJavaString(frames[k].first))));
        f.PutField(f.ObjectOf(e), "meth", static_cast<std::uint64_t>(f.ObjectOf(f.NewJavaString(frames[k].second))));
        f.SetObjectArrayElement(arr, static_cast<std::int32_t>(k), e);
      }
      f.PutField(f.ObjectOf(ex), "trace", static_cast<std::uint64_t>(f.ObjectOf(arr)));
      f.Throw(ex);
      return 0;
    };
  };
  j.DefineMethod("rb/Fixture", "deviceId", "()Ljava/lang/String;", true,
                 thrower({{u"android.os.Parcel", u"readException"},
                          {u"com.android.internal.telephony.ITelephony$Stub$Proxy", u"getDeviceIdWithFeature"},
                          {u"android.telephony.TelephonyManager", u"getDeviceId"},
                          {u"com.halfbrick.mortar.HBSupport", u"GetDeviceID"}}));
  // An app method that happens to be called getDeviceId, refused by something else: not an identifier read.
  j.DefineMethod("rb/Fixture", "otherRefusal", "()Ljava/lang/String;", true,
                 thrower({{u"android.app.ContextImpl", u"enforce"},
                          {u"com.example.Game", u"getDeviceId"},
                          {u"android.telephony.TelephonyManager", u"getDeviceId"}}));
  const Result a = Call(pr, tier, "fx_identifier", {Value32::S32(0)});
  CHECK_CTX(Returned(a) && a.ret.i() == 3, std::string{"Tier "} + TierName(tier) + ": deviceId -> bits " +
                                               std::to_string(a.ret.i()) + " (want 3: null, nothing pending)");
  CHECK_CTX(HasNote(pr, "device identifier"), Notes(pr));
  const Result b = Call(pr, tier, "fx_identifier", {Value32::S32(1)});
  CHECK_CTX(Returned(b) && b.ret.i() == 1, std::string{"Tier "} + TierName(tier) + ": otherRefusal -> bits " +
                                               std::to_string(b.ret.i()) + " (want 1: null, the exception kept)");
  CHECK(j.PendingClass().empty());
}

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
    mutf8_round_trips(tier);
    arrays_every_kind(tier);
    pending_exception_rules(tier);
    local_frames(tier);
    monitor_pairs(tier);
    register_natives_reaches_the_binder(tier);
    java_vm_get_env(tier);
    direct_buffer_over_guest_memory(tier);
    reflection_round_trip(tier);
    double_delete_is_tolerated(tier);
    argument_used_after_its_native_returned(tier);
    detach_frees_the_threads_locals(tier);
    long_new_string_utf(tier);
    device_identifier_refusals_read_as_null(tier);
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
