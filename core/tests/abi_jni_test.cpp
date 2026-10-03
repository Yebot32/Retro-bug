/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the fake `JNIEnv` (C08/C09 seam) and for Tier I's IT-block semantics.
 *
 * Both halves exist because the fixture now contains code that reaches them, and both assert against
 * numbers measured on the SM-X710 by `tools/run_armv7_oracle.sh` — recorded in
 * `tests/corpus/oracle-armv7.txt` — rather than against arithmetic done here. That matters more for the
 * IT half than it looks: `docs/32` §2.1 measured 8,387 IT-block instructions in bionic and named IT
 * handling the engine's biggest correctness risk, and a predicated instruction that executes on the
 * wrong leg still returns a *number*. Only a measured one catches it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/abi/jni_env.h"
#include "rb/exec/interp.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace abi = rb::abi;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace loader = rb::loader;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                     \
  do {                                                 \
    ++checks;                                          \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__);   \
  } while (0)

#define CHECK_CTX(cond, detail)                          \
  do {                                                   \
    ++checks;                                            \
    if (!(cond)) fail(#cond, __LINE__, detail);           \
  } while (0)

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
/// The JNIEnv region, in the window but behind the module and below the code cache's eventual home.
constexpr exec::GuestAddr kJniRegion = 0x0020'0000u;

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

struct Rig {
  std::unique_ptr<GuestMemory> mem;
  std::unique_ptr<exec::InterpExecutor> engine;
  std::unique_ptr<abi::AbiContext> bridge;
  std::unique_ptr<abi::JniEnv> jni;
  std::optional<loader::GuestModule> mod;
  bool ok = false;
};

Rig build() {
  Rig rig;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = GuestMemory::Create(64u << 20, model, &err);
  const auto bytes = read_file(RB_FIXTURE_PATH);
  if (!rig.mem || bytes.empty()) {
    fail("no guest memory or no fixture", __LINE__, err);
    return rig;
  }
  if (!rig.mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err)) {
    fail("could not map a guest stack", __LINE__, err);
    return rig;
  }
  loader::LoadOptions opts;
  // Imports resolve to an address nobody implements. Nothing under test here reaches one — the two
  // entry points used are `it_probe`, which is pure arithmetic, and `jni_utf`, which only calls out
  // through the table — so a synthetic resolver keeps C08's test independent of C06/C11.
  opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return 0x0900'0000u; };
  rig.mod = loader::LoadFromBytes(*rig.mem, bytes, "librb_fixture.so", opts, &err);
  if (!rig.mod) {
    fail("loader refused the fixture", __LINE__, err);
    return rig;
  }
  rig.bridge = std::make_unique<abi::AbiContext>(*rig.mem, kStackTop, kReturnAddress);
  rig.engine = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress, rig.bridge.get());
  rig.bridge->set_executor(*rig.engine);
  rig.jni = std::make_unique<abi::JniEnv>(*rig.mem, *rig.bridge, kJniRegion, 16u << 10, &err);
  if (!rig.jni->ok()) {
    fail("the JNIEnv could not be built", __LINE__, err);
    return rig;
  }
  rig.ok = true;
  return rig;
}

/// Calls a guest export the way C09 will: `env` and `clazz` first, then the declared parameters.
struct CallOut {
  exec::RunResult run{};
  std::int32_t i = 0;
  std::uint64_t bits = 0;
};

CallOut call_guest(Rig& rig, const char* export_name, const abi::Args32& tail, const char* shorty) {
  CallOut out;
  const auto where = rig.mod->FindExport(export_name);
  if (!where) {
    out.run.reason = exec::StopReason::Internal;
    out.run.detail = std::string{"no export named "} + export_name;
    return out;
  }
  abi::Args32 args = {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(0)};
  for (const auto& a : tail) args.push_back(a);
  const auto ret = rig.bridge->CallGuest(where->tagged(), args, abi::Shorty::For(shorty), 4'000'000,
                                         &out.run);
  out.bits = ret.bits;
  out.i = ret.i();
  return out;
}

// --------------------------------------------------------------------- the table itself

void the_guest_reads_the_table_we_actually_wrote() {
  Rig rig = build();
  if (!rig.ok) return;
  std::uint32_t table_at = 0;
  CHECK(rig.mem->Read(rig.jni->env(), &table_at, 4));
  CHECK_CTX(table_at == kJniRegion, "table at " + Hex(table_at));

  std::uint32_t entry = 0;
  // The offset the *guest* uses, measured out of its own encoding: `ldr.w r2, [r2, #0x29c]` in
  // `jni_utf`. Reading the same slot we publish at index 167 is the cross-check; if the constant in
  // jni_env.h and the fixture's instruction ever disagree, this is where it surfaces.
  CHECK(abi::jni::kNewStringUTF * 4u == 0x29cu);
  CHECK(rig.mem->Read(table_at + 0x29cu, &entry, 4));
  CHECK_CTX(entry == rig.bridge->SlotFor(abi::jni::kNewStringUTF),
            "entry=" + Hex(entry) + " slot=" + Hex(rig.bridge->SlotFor(abi::jni::kNewStringUTF)));

  // No table word is left zero, so a guest that reaches an unimplemented entry lands on a refusal.
  unsigned blanks = 0;
  for (std::uint32_t i = 0; i < abi::jni::kTableEntries; ++i) {
    std::uint32_t w = 0;
    if (!rig.mem->Read(table_at + i * 4u, &w, 4) || w != rig.bridge->SlotFor(i)) ++blanks;
  }
  CHECK_CTX(blanks == 0, std::to_string(blanks) + " table entries do not name their slot");
}

// --------------------------------------------------------------------- guest -> host -> guest

void a_guest_string_call_out_matches_hardware() {
  Rig rig = build();
  if (!rig.ok) return;
  // oracle-armv7.txt: `jni_utf(1)=115 jni_utf(0)=114 calls=2 utf=retrobug`. 115 is 1 + 'r', which the
  // guest computes only if NewStringUTF returned non-null — so the value alone proves the call left the
  // CPU, was answered, and came back through r0.
  const auto one = call_guest(rig, "Java_com_retrobug_fixture_MainActivity_jni_utf",
                              {abi::Value32::S32(1)}, "IILLI");
  CHECK_CTX(one.run.reason == exec::StopReason::Returned,
            "reason " + std::to_string(static_cast<int>(one.run.reason)) + " " + one.run.detail);
  CHECK_CTX(one.i == 115, "jni_utf(1) = " + std::to_string(one.i) + ", hardware said 115");
  CHECK_CTX(rig.jni->new_string_utf_calls() == 1,
            std::to_string(rig.jni->new_string_utf_calls()) + " calls");
  // The bytes crossed: the guest's own static string, read out of guest memory by the host.
  CHECK_CTX(rig.jni->last_string() == "retrobug", "host received \"" + rig.jni->last_string() + "\"");

  const auto zero = call_guest(rig, "Java_com_retrobug_fixture_MainActivity_jni_utf",
                               {abi::Value32::S32(0)}, "IILLI");
  CHECK_CTX(zero.i == 114, "jni_utf(0) = " + std::to_string(zero.i) + ", hardware said 114");
  CHECK_CTX(rig.jni->new_string_utf_calls() == 2, std::to_string(rig.jni->new_string_utf_calls()));
  std::fprintf(stderr, "[jni] jni_utf(1)=%d jni_utf(0)=%d utf=\"%s\"\n", one.i, zero.i,
               rig.jni->last_string().c_str());
}

void a_string_handle_round_trips_back_to_its_bytes() {
  Rig rig = build();
  if (!rig.ok) return;
  call_guest(rig, "Java_com_retrobug_fixture_MainActivity_jni_utf", {abi::Value32::S32(1)}, "IILLI");
  const exec::GuestAddr handle = rig.jni->last_handle();
  CHECK_CTX(handle != 0, "no handle was recorded");

  // Calling a host entry through the bridge, with the arguments where a guest would have left them, is
  // also how C09 will reach them — so this exercises the real path rather than the implementation.
  // The bytes after `is_copy` carry a sentinel: `isCopy` points at a *jboolean*, one byte, and the
  // 4-byte write an early draft made zeroed whatever the guest had declared beside it.
  const exec::GuestAddr is_copy_at = kStackTop - 0x40u;
  const std::uint32_t sentinel = 0x5a5a'a5a5u;
  CHECK(rig.mem->Write(is_copy_at, &sentinel, 4));
  exec::RunResult run{};
  const auto chars = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kGetStringUTFChars),
      {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(handle), abi::Value32::Ptr(is_copy_at)},
      abi::Shorty::For("LL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned,
            "GetStringUTFChars: " + std::to_string(static_cast<int>(run.reason)) + " " + run.detail);
  char buf[16] = {};
  CHECK_CTX(chars.bits != 0 && rig.mem->Read(static_cast<exec::GuestAddr>(chars.bits), buf, 9),
            "cannot read the returned chars pointer " + Hex(chars.w()));
  CHECK_CTX(std::strcmp(buf, "retrobug") == 0, std::string{"chars are \""} + buf + "\"");
  std::uint32_t copied = 0;
  CHECK(rig.mem->Read(is_copy_at, &copied, 4));
  CHECK_CTX((copied & 0xffu) == 1, "isCopy low byte=" + std::to_string(copied & 0xffu));
  CHECK_CTX((copied & 0xffff'ff00u) == (sentinel & 0xffff'ff00u),
            "isCopy wrote past the jboolean: bytes 1..3 are now 0x" + Hex(copied));

  const auto len = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kGetStringUTFLength),
      {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(handle)}, abi::Shorty::For("IL"), 1000,
      &run);
  CHECK_CTX(len.i() == 8, "GetStringUTFLength = " + std::to_string(len.i()));
  std::fprintf(stderr, "[jni] handle=0x%x chars=0x%x len=%d\n", handle, chars.w(), len.i());
}

/// A `jstring` is opaque, so anything arriving as one is either ours, corrupted, or a pointer to
/// something else entirely. Reading that last case as text would produce a plausible string out of
/// arbitrary guest memory, which is why the handle is validated against the arena and the magic.
void a_foreign_pointer_as_a_string_is_refused_not_read() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::RunResult run{};
  const auto before = rig.jni->last_string();
  const auto bogus = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kGetStringUTFChars),
      {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(kStackTop - 0x100u), abi::Value32::Ptr(0)},
      abi::Shorty::For("LL"), 1000, &run);
  // The reason is the whole assertion. There is deliberately no check on `bogus`: a refusal means no
  // result was produced, and r0 still holds whatever argument the caller placed — asserting it came
  // back zero would encode the very "a failed call can still be read as a value" confusion this
  // bridge is designed to make impossible.
  CHECK_CTX(run.reason == exec::StopReason::Fault,
            "reason " + std::to_string(static_cast<int>(run.reason)) + " " + run.detail);
  CHECK_CTX(run.detail.find("arena") != std::string::npos, run.detail);
  CHECK_CTX(rig.jni->last_string() == before, "a refused handle was read as a string");
  (void)bogus;
}

/// Entries 0..3 are JNI's reserved NULL slots; no correct guest calls them, and a guest that does has
/// a bug that deserves a named stop rather than a silent zero.
void unimplemented_entries_stop_naming_what_was_asked_for() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::RunResult run{};
  rig.bridge->CallGuest(rig.bridge->SlotFor(0), {}, abi::Shorty::For("VL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Fault, "reserved entry 0 was accepted");
  CHECK_CTX(run.detail.find("not bound") != std::string::npos, run.detail);

  rig.bridge->CallGuest(rig.bridge->SlotFor(abi::jni::kFindClass),
                        {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(0)},
                        abi::Shorty::For("LL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Fault, "FindClass was accepted");
  CHECK_CTX(run.detail.find("FindClass") != std::string::npos, run.detail);
}

/// The exception family answers rather than refuses: on an environment whose every implemented
/// function either cannot throw or refuses loudly, "no exception" is the true answer, and the
/// defensive `ExceptionOccurred(env)` after every call is the style real JNI code is written in. A
/// refusal here killed the guest call on its first check, naming C09 work that was never the guest's.
void the_exception_family_answers_no_exception_not_refuses() {
  Rig rig = build();
  if (!rig.ok) return;
  exec::RunResult run{};
  const auto occurred = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kExceptionOccurred),
      {abi::Value32::Ptr(rig.jni->env())}, abi::Shorty::For("LL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned,
            "ExceptionOccurred: " + std::to_string(static_cast<int>(run.reason)) + " " + run.detail);
  CHECK_CTX(occurred.bits == 0, "ExceptionOccurred = 0x" + Hex(occurred.w()));

  const auto check = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kExceptionCheck),
      {abi::Value32::Ptr(rig.jni->env())}, abi::Shorty::For("ZL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned,
            "ExceptionCheck: " + std::to_string(static_cast<int>(run.reason)) + " " + run.detail);
  CHECK_CTX(check.bits == 0, "ExceptionCheck = 0x" + Hex(check.w()));

  const auto describe = rig.bridge->CallGuest(
      rig.bridge->SlotFor(abi::jni::kExceptionDescribe),
      {abi::Value32::Ptr(rig.jni->env())}, abi::Shorty::For("VL"), 1000, &run);
  CHECK_CTX(run.reason == exec::StopReason::Returned,
            "ExceptionDescribe: " + std::to_string(static_cast<int>(run.reason)) + " " + run.detail);
}

// --------------------------------------------------------------------- IT blocks, on measured values

struct ItCase {
  std::int32_t a, b, want;
};

void predicated_thumb2_blocks_take_the_legs_the_hardware_took() {
  Rig rig = build();
  if (!rig.ok) return;
  // Straight from oracle-armv7.txt's `it_probe:` row, in that order. The two `ite` blocks each have one
  // leg taken and one not, the first block's predicates are 16-bit and the second's are 32-bit
  // (`addlt.w`/`orrge`), and the (7,3)/(5,5) pair distinguishes `gt` from `le` on the *same* result,
  // which is the case an engine gets wrong by inverting a condition and still looks healthy elsewhere.
  const ItCase cases[] = {
      {7, 3, 42}, {3, 7, 1008}, {5, 5, 42}, {101, 1, 104}, {0, 0, 1005}, {-5, 3, 1000},
  };
  for (const auto& c : cases) {
    const auto out = call_guest(rig, "Java_com_retrobug_fixture_MainActivity_it_probe",
                                {abi::Value32::S32(c.a), abi::Value32::S32(c.b)}, "IIILL");
    const std::string what = "it_probe(" + std::to_string(c.a) + "," + std::to_string(c.b) + ")";
    CHECK_CTX(out.run.reason == exec::StopReason::Returned,
              what + " stopped with " + std::to_string(static_cast<int>(out.run.reason)) + " " +
                  out.run.detail);
    CHECK_CTX(out.i == c.want, what + " = " + std::to_string(out.i) + ", hardware said " +
                                   std::to_string(c.want));
    std::fprintf(stderr, "[it] %s = %d\n", what.c_str(), out.i);
  }
}

/// IT state must not outlive the block that set it. A leaked predicate silently conditions every
/// following instruction, which is the failure mode `docs/32` §2.1 calls the engine's biggest risk — so
/// the state is asserted to be back at zero after a function that used two IT blocks.
void it_state_does_not_leak_past_the_block_that_set_it() {
  Rig rig = build();
  if (!rig.ok) return;
  const auto out = call_guest(rig, "Java_com_retrobug_fixture_MainActivity_it_probe",
                              {abi::Value32::S32(3), abi::Value32::S32(7)}, "IIILL");
  CHECK(out.i == 1008);
  CHECK_CTX(rig.bridge->state().itstate == 0,
            "itstate left at 0x" + Hex(rig.bridge->state().itstate));
  // And the guest's own IT encoding in CPSR is the same zero, not just the mirror field. The mask is
  // IT[1:0] at bits 26:25 (0x0600_0000) plus IT[7:2] at bits 15:10 (0x0fc00) — the first draft here
  // used 0x0c00_0000, which is bits 27:26 and so watched the wrong bit entirely.
  CHECK_CTX((rig.bridge->state().cpsr & 0x0600'fc00u) == 0,
            "cpsr=" + Hex(rig.bridge->state().cpsr));
}

/// Makes a guest string at `text_at` through the `NewStringUTF` slot and releases it again through
/// `ReleaseStringUTFChars`. The two calls are the guest's own path — the bridge, the table, the AAPCS
/// registers — rather than the implementation, which is what makes these tests about the lifecycle the
/// guest sees.
struct StringCalls {
  Rig* rig = nullptr;
  exec::GuestAddr text_at = 0;
  exec::RunResult run{};

  std::uint32_t make() {
    const auto made = rig->bridge->CallGuest(
        rig->bridge->SlotFor(abi::jni::kNewStringUTF),
        {abi::Value32::Ptr(rig->jni->env()), abi::Value32::Ptr(text_at)}, abi::Shorty::For("LL"), 1000,
        &run);
    return made.w();
  }
  bool release(std::uint32_t handle) {
    rig->bridge->CallGuest(
        rig->bridge->SlotFor(abi::jni::kReleaseStringUTFChars),
        {abi::Value32::Ptr(rig->jni->env()), abi::Value32::Ptr(handle), abi::Value32::Ptr(0)},
        abi::Shorty::For("VLLL"), 1000, &run);
    return run.reason == exec::StopReason::Returned;
  }
  bool delete_local(std::uint32_t handle) {
    rig->bridge->CallGuest(rig->bridge->SlotFor(abi::jni::kDeleteLocalRef),
                           {abi::Value32::Ptr(rig->jni->env()), abi::Value32::Ptr(handle)},
                           abi::Shorty::For("VL"), 1000, &run);
    return run.reason == exec::StopReason::Returned;
  }
};

/// Writes `text` (NUL-terminated) into the guest stack and hands back the address.
exec::GuestAddr put_text(Rig& rig, const std::string& text, exec::GuestAddr at) {
  if (!rig.mem->Write(at, text.c_str(), text.size() + 1u)) {
    fail("could not write the source text", __LINE__, text);
    return 0;
  }
  return at;
}

/// The audit's scenario, as a test: a guest that makes a string and releases it, every frame.
///
/// The arena was bump-allocated and freed only with the instance, so this loop walked it off its end —
/// 2000 rounds of a twenty-byte slot is ~40 KiB against a 16 KiB arena — and the failure arrived as a bare
/// "NewStringUTF failed", far from the loop that caused it. What is asserted is not that the calls
/// returned (they could all return and still leak) but that the arena did not *grow*: with the free list
/// and the bump-pointer rollback it stays at one string's size for the whole loop.
void a_released_string_does_not_grow_the_arena() {
  Rig rig = build();
  if (!rig.ok) return;
  StringCalls calls{&rig, put_text(rig, "retrobug", kStackTop - 0x200u)};
  const std::uint32_t first = calls.make();
  CHECK_CTX(calls.run.reason == exec::StopReason::Returned && first != 0,
            "NewStringUTF: reason " + std::to_string(static_cast<int>(calls.run.reason)) + " " +
                calls.run.detail);
  const unsigned one_string = rig.jni->arena_used();
  CHECK_CTX(one_string != 0, "the arena did not move for the first string");
  CHECK(calls.release(first));

  unsigned failed = 0;
  for (unsigned i = 1; i < 2000; ++i) {
    const std::uint32_t handle = calls.make();
    if (calls.run.reason != exec::StopReason::Returned || handle == 0) ++failed;
    if (!calls.release(handle)) ++failed;
  }
  CHECK_CTX(failed == 0, std::to_string(failed) + " of 1999 make/release rounds failed");
  CHECK_CTX(rig.jni->arena_used() <= one_string,
            "the arena grew to " + std::to_string(rig.jni->arena_used()) + " bytes over 2000 released"
            " strings (one string is " + std::to_string(one_string) + ")");
  CHECK_CTX(rig.jni->strings_made() == 2000u, std::to_string(rig.jni->strings_made()));
  CHECK_CTX(rig.jni->strings_freed() == 2000u, std::to_string(rig.jni->strings_freed()));
  std::fprintf(stderr, "[jni] 2000 make/release rounds: arena_used=%u one_string=%u free=%u\n",
               rig.jni->arena_used(), one_string, rig.jni->free_bytes());
}

/// A `jstring` kept past its release is stale, not still-good. The block's magic word is cleared when the
/// space goes back, so the handle is refused by name instead of being read as whatever the next string
/// wrote there — a use-after-release that produced a plausible string would be the worst version of this
/// bug, because nothing would look wrong.
void a_stale_handle_is_refused_not_read() {
  Rig rig = build();
  if (!rig.ok) return;
  StringCalls calls{&rig, put_text(rig, "retrobug", kStackTop - 0x200u)};
  const std::uint32_t first = calls.make();
  const std::uint32_t second = calls.make();  // keeps the arena's tail above `first`
  CHECK(first != 0 && second != 0);
  CHECK(calls.release(first));

  rig.bridge->CallGuest(rig.bridge->SlotFor(abi::jni::kGetStringUTFLength),
                        {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(first)},
                        abi::Shorty::For("IL"), 1000, &calls.run);
  CHECK_CTX(calls.run.reason == exec::StopReason::Fault,
            "a released handle was read: reason " +
                std::to_string(static_cast<int>(calls.run.reason)) + " " + calls.run.detail);
  CHECK_CTX(calls.run.detail.find("RetroBug string header") != std::string::npos, calls.run.detail);
  // The second string is untouched: releasing one block must not disturb its neighbour.
  const auto len = rig.bridge->CallGuest(rig.bridge->SlotFor(abi::jni::kGetStringUTFLength),
                                         {abi::Value32::Ptr(rig.jni->env()), abi::Value32::Ptr(second)},
                                         abi::Shorty::For("IL"), 1000, &calls.run);
  CHECK_CTX(len.i() == 8, "the neighbouring string became " + std::to_string(len.i()) + " bytes");
}

/// `DeleteLocalRef` is the other half of JNI's local-reference lifecycle, and it frees too. A double
/// release is refused rather than silently accepted, because the second one is the shape a guest bug
/// takes and it is cheaper to name than to reason about later.
void delete_local_ref_releases_too() {
  Rig rig = build();
  if (!rig.ok) return;
  StringCalls calls{&rig, put_text(rig, "retrobug", kStackTop - 0x200u)};
  const std::uint32_t first = calls.make();
  const unsigned after_make = rig.jni->arena_used();
  CHECK(after_make != 0);
  CHECK(calls.delete_local(first));
  CHECK_CTX(rig.jni->arena_used() == 0, "the arena kept " + std::to_string(rig.jni->arena_used()) +
                                            " bytes after DeleteLocalRef");
  // The second delete of the same handle is refused by name — and deliberately not wrapped in `CHECK`,
  // because the assertion is about *how* it failed rather than that it returned false: a refusal is a
  // Fault with a reason, and `CHECK(false)` would have hidden which one.
  calls.delete_local(first);
  // Which name depends on where the handle now
  // sits — the block went straight back to the bump pointer, so this is the range check rather than the
  // magic check — but it must be a refusal: accepting it silently is how a double release becomes a
  // double *allocation* later, with two live strings on one block.
  CHECK_CTX(calls.run.reason == exec::StopReason::Fault,
            "a double DeleteLocalRef was accepted: " + calls.run.detail);
  CHECK_CTX(calls.run.detail.find("release of") != std::string::npos, calls.run.detail);
  CHECK(calls.delete_local(0));  // JNI allows DeleteLocalRef(NULL)
  std::fprintf(stderr, "[jni] DeleteLocalRef: freed=%u arena_used=%u\n", rig.jni->strings_freed(),
               rig.jni->arena_used());
}

/// Two freed blocks that sit next to each other become one, which is what lets a later string use space no
/// single release could have served. The layout is arranged so the bump pointer cannot help: the middle
/// and first strings are freed while a third still sits above them, so the space only becomes usable
/// through coalescing.
void released_blocks_coalesce_for_a_larger_string() {
  Rig rig = build();
  if (!rig.ok) return;
  StringCalls calls{&rig, 0};
  // 132 bytes of arena each (8 header + 120 text + NUL, rounded up), so two merged holes hold 264 — enough
  // for the 212-byte `big` below, while neither hole alone is. An earlier version of this test used a
  // nine-character string, whose 20-byte blocks coalesced to 40 and could never have shown the merge.
  const std::string small(120, 'a');
  const std::string big(200, 'b');  // 212 bytes of arena, more than one block and less than two

  calls.text_at = put_text(rig, big, kStackTop - 0x400u);   // consume most of the arena with one string
  const std::uint32_t parked = calls.make();
  CHECK(parked != 0);
  calls.text_at = put_text(rig, small, kStackTop - 0x200u);
  const std::uint32_t a = calls.make();
  const std::uint32_t b = calls.make();
  const std::uint32_t c = calls.make();
  CHECK(a != 0 && b != 0 && c != 0);
  const unsigned before = rig.jni->arena_used();

  // Free the middle, then the one below it: adjacent holes, but not the tail, so nothing rolls back.
  CHECK(calls.release(b));
  CHECK(calls.release(a));
  CHECK_CTX(rig.jni->arena_used() == before,
            "the bump pointer moved for a hole that was not at the tail: " +
                std::to_string(rig.jni->arena_used()) + " vs " + std::to_string(before));

  // 212 bytes, more than either hole held on its own. There is free space above the bump pointer, so this
  // is not about the arena being full: it is that the allocation has to come out of the *coalesced* hole
  // rather than off the top, which is exactly what `arena_used()` staying put shows. A first-fit that
  // skipped the free list, or a free list that never merged neighbours, grows the arena here.
  calls.text_at = put_text(rig, big, kStackTop - 0x400u);
  const std::uint32_t reused = calls.make();
  CHECK_CTX(reused != 0 && calls.run.reason == exec::StopReason::Returned,
            "the reused allocation was refused: " + calls.run.detail);
  CHECK_CTX(rig.jni->arena_used() == before,
            "the coalesced space was not used: the arena grew from " + std::to_string(before) + " to " +
                std::to_string(rig.jni->arena_used()));
  std::fprintf(stderr, "[jni] coalesce: before=%u after=%u reused=0x%x\n", before,
               rig.jni->arena_used(), reused);
}

}  // namespace

int main() {
  the_guest_reads_the_table_we_actually_wrote();
  a_guest_string_call_out_matches_hardware();
  a_string_handle_round_trips_back_to_its_bytes();
  a_foreign_pointer_as_a_string_is_refused_not_read();
  a_released_string_does_not_grow_the_arena();
  a_stale_handle_is_refused_not_read();
  delete_local_ref_releases_too();
  released_blocks_coalesce_for_a_larger_string();
  unimplemented_entries_stop_naming_what_was_asked_for();
  the_exception_family_answers_no_exception_not_refuses();
  predicated_thumb2_blocks_take_the_legs_the_hardware_took();
  it_state_does_not_leak_past_the_block_that_set_it();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
