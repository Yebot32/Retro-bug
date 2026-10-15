/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * `rb::runtime::GuestProcess` (M2 Task 1): the persistent guest process that boots the device's own 32-bit
 * bionic `libc.so` and plays the dynamic linker's part for it — the shared-globals page, the auxv, the
 * static-TLS layout and a real main thread (`pthread_internal_t` + `bionic_tcb` + `bionic_tls`, linked into
 * `g_thread_list`), the way `linker_main`/`__libc_init_main_thread_*` leave a process on a device.
 *
 * Every row runs on both tiers, each in its own process: a constructor has side effects, so one process
 * running its constructors twice would be a different (wrong) experiment.
 *
 * The expected values are the tablet's own: the layout constants are asserted by `GuestProcess` against the
 * image at load (a different libc build is a named refusal, and the last row proves that refusal fires),
 * and the main-thread shape is the one a native 32-bit process on R52Y106BECZ reports for its own main
 * thread (`__libc_get_static_tls_bounds` -> [start, start+0xba8) with the tcb at +0, bionic_tls at +0x2c,
 * libc's own TLS module at +0xb78; start_routine 0; the byte at +0x2c4 is 0). RetroBug registers no TLS
 * module, so its static TLS ends at 0xb78 where the device's continues with libc's 0x30-byte segment.
 *
 * Each expectation is tagged with where its value comes from, because only some of them can fail for a
 * reason outside GuestProcess:
 *   [libc code]     the guest's own bionic computed it (getauxval, pthread_self, __errno, strtol,
 *                   __libc_get_static_tls_bounds, __libc_init_common) — independent of the host.
 *   [image]         a fact of the libc file (llvm-readelf / llvm-objdump), asserted at load as well.
 *   [device probe]  the device's own main thread, measured by tests/guest/tls_layout_probe.c.
 *   [host echo]     a value GuestProcess wrote and the test reads back — it checks plumbing, not truth.
 */
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/guest_process.h"
#include "rb/syscall/monitor.h"

#ifndef RB_DEVICE_LIBC_PATH
#define RB_DEVICE_LIBC_PATH "/data/local/tmp/rb_device_libc.so"
#endif
#ifndef RB_GUEST_LIBM_PATH
#define RB_GUEST_LIBM_PATH "/data/local/tmp/libm.so"
#endif
#ifndef RB_GUEST_LIBZ_PATH
#define RB_GUEST_LIBZ_PATH "/data/local/tmp/libz.so"
#endif
#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif
#ifndef RB_STRIPPED_FIXTURE_PATH
#define RB_STRIPPED_FIXTURE_PATH "/data/local/tmp/librb_fixture_stripped.so"
#endif
#ifndef RB_CTOR_FAULT_PATH
#define RB_CTOR_FAULT_PATH "/data/local/tmp/librbctorfault.so"
#endif

namespace {

using namespace rb;
using runtime::GuestProcess;
using runtime::GuestProcessOptions;

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

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  while ((1L << shift) < ps) ++shift;
  return shift;
}

constexpr std::uint32_t kTrap = 0x0bad'0000u;
constexpr std::uint32_t kAtPagesz = 6, kAtHwcap = 16, kAtRandom = 25;
/// VFP | NEON | VFPv3 | TLS | VFPv4 | IDIVA | IDIVT — the brief's ARMv7 set (Linux `asm/hwcap.h` bits).
constexpr std::uint32_t kHwcap = 0x40u | 0x1000u | 0x2000u | 0x8000u | 0x1'0000u | 0x2'0000u | 0x4'0000u;
constexpr std::uint32_t kDlSlotBase = 400;  ///< the test's own host slots, far above the JNIEnv's 0..232

const char* TierName(int tier) { return tier == 1 ? "I" : "J"; }

std::uint32_t Word(GuestProcess& p, std::uint32_t at) {
  std::uint32_t v = 0xdead'dead;
  if (!p.memory().Read(at, &v, 4)) return 0xdead'dead;
  return v;
}

/// The dynamic-linker interface libc imports (`dlopen` & co.) is the *launcher's* to answer
/// (`host_resolver`); a process with no answer traps there, and libc's own `netdClientInitImpl` calls
/// `dlopen` from a constructor. So the test answers the set the way bionic's linker would for a process
/// with nothing more to load: NULL / 0, and 34 for the target SDK.
struct Booted {
  std::unique_ptr<GuestProcess> p;
  std::string err;
  std::vector<std::string> host_asked;
};

Booted Boot(int tier, const std::vector<std::uint8_t>& libc, const std::vector<std::uint8_t>& libm,
            const std::vector<std::uint8_t>& libz, bool run_ctors = true) {
  Booted b;
  // The host resolver is consulted during LoadScope, after Create returned, so it can reach the process
  // through this pointer; it is never called before `b.p` is set.
  auto holder = std::make_shared<GuestProcess*>(nullptr);
  auto asked = std::make_shared<std::vector<std::string>>();
  static const char* const kDl[] = {"dlopen", "android_dlopen_ext", "dlsym", "dlclose", "dladdr", "dlerror",
                                    "dl_unwind_find_exidx", "android_get_application_target_sdk_version"};
  GuestProcessOptions opts;
  opts.window_bytes = 64u << 20;
  // Biased: a flat identity window would have to start at guest 0 == host 0, which Android refuses
  // (mmap_min_addr) — measured: `flat-model low window reservation failed: Operation not permitted`.
  opts.biased = true;
  opts.unresolved_trap = kTrap;
  opts.ctor_tier = tier;
  // The process's own report lines: printed for the first boot (so a run shows what was built) and for
  // any boot that fails; kept quiet otherwise, since every row boots its own process.
  auto lines = std::make_shared<std::vector<std::string>>();
  opts.log = [lines](const std::string& line) { lines->push_back(line); };
  opts.host_resolver = [holder, asked](const std::string& name) -> std::optional<exec::GuestAddr> {
    asked->push_back(name);
    for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
      if (name == kDl[i]) return (*holder)->abi().SlotFor(kDlSlotBase + i);
    }
    return std::nullopt;
  };
  b.p = GuestProcess::Create(opts, &b.err);
  if (!b.p) return b;
  *holder = b.p.get();
  for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
    const bool sdk = std::strcmp(kDl[i], "android_get_application_target_sdk_version") == 0;
    b.p->abi().Bind(kDlSlotBase + i, [sdk](exec::MachineState& s, std::string*) {
      s.r[0] = sdk ? 34u : 0u;
      return true;
    });
  }
  if (!b.p->LoadScope(libc, libm, libz, {}, &b.err)) {
    b.p.reset();
    for (const std::string& l : *lines) std::fprintf(stderr, "#   %s\n", l.c_str());
    return b;
  }
  if (run_ctors) {
    std::string failed;
    if (!b.p->RunConstructors(&failed)) {
      b.err = "RunConstructors: " + failed;
      b.p.reset();
    }
  }
  static bool shown = false;
  if (!shown || !b.p) {
    for (const std::string& l : *lines) std::fprintf(stderr, "#   %s\n", l.c_str());
    shown = true;
  }
  b.host_asked = *asked;
  return b;
}

struct Call {
  bool ok = false;
  std::uint32_t r0 = 0;
  std::string why;
};

Call CallLibc(GuestProcess& p, int tier, const char* name, const abi::Args32& args, const char* shorty) {
  Call c;
  const auto fn = p.libc()->FindExport(name);
  if (!fn) {
    c.why = std::string{"libc exports no "} + name;
    return c;
  }
  p.abi().set_executor(p.engine(tier));
  exec::RunResult rr{};
  const abi::Ret32 ret = p.abi().CallGuest(fn->tagged(), args, abi::Shorty::For(shorty), 4'000'000, &rr);
  c.ok = rr.reason == exec::StopReason::Returned;
  c.r0 = ret.w();
  c.why = std::string{name} + " on Tier " + TierName(tier) + ": reason " +
          std::to_string(static_cast<int>(rr.reason)) + " " + rr.detail;
  return c;
}

struct Images {
  std::vector<std::uint8_t> libc, libm, libz;
  bool ok = false;
};

Images LoadImages() {
  Images im;
  im.libc = read_file(RB_DEVICE_LIBC_PATH);
  im.libm = read_file(RB_GUEST_LIBM_PATH);
  im.libz = read_file(RB_GUEST_LIBZ_PATH);
  CHECK_CTX(!im.libc.empty(), std::string{"nothing at "} + RB_DEVICE_LIBC_PATH);
  CHECK_CTX(!im.libm.empty(), std::string{"nothing at "} + RB_GUEST_LIBM_PATH);
  CHECK_CTX(!im.libz.empty(), std::string{"nothing at "} + RB_GUEST_LIBZ_PATH);
  im.ok = !im.libc.empty() && !im.libm.empty() && !im.libz.empty();
  return im;
}

// ------------------------------------------------------------------------------------------------ rows

/// Create, LoadScope(libc + libm + libz), RunConstructors -> true; and `__libc_preinit` demonstrably ran:
/// it wrote the shared-globals field +0x438 and copied the TP+0x14 stack guard into `__stack_chk_guard`.
/// `getauxval` answers from the auxv this process published.
void boots_the_real_libc(const Images& im, int tier) {
  Booted b = Boot(tier, im.libc, im.libm, im.libz);
  CHECK_CTX(b.p != nullptr, std::string{"Tier "} + TierName(tier) + ": " + b.err);
  if (!b.p) return;
  GuestProcess& p = *b.p;
  std::fprintf(stderr, "# Tier %s: libc at %s, %zu constructor(s) ran\n", TierName(tier),
               Hex(p.libc()->base).c_str(), p.constructors_run());
  CHECK(p.constructors_run() > 0);

  const Call rnd = CallLibc(p, tier, "getauxval", {abi::Value32::U32(kAtRandom)}, "II");
  CHECK_CTX(rnd.ok, rnd.why);
  CHECK_CTX(rnd.r0 == p.at_random() && rnd.r0 != 0,  // [libc code reads what the host published; at_random() itself is host echo]
            "getauxval(AT_RANDOM)=" + Hex(rnd.r0) + " want " + Hex(p.at_random()));
  const Call pg = CallLibc(p, tier, "getauxval", {abi::Value32::U32(kAtPagesz)}, "II");
  CHECK_CTX(pg.ok && pg.r0 == 4096u, pg.why + " = " + Hex(pg.r0));  // [libc code: getauxval]
  const Call hw = CallLibc(p, tier, "getauxval", {abi::Value32::U32(kAtHwcap)}, "II");
  CHECK_CTX(hw.ok && hw.r0 == kHwcap, hw.why + " = " + Hex(hw.r0) + " want " + Hex(kHwcap));  // [libc code: getauxval; value is the brief's set, host echo]

  // preinit_impl's first store: `str.w r1, [r0, #0x438]` into the globals __loader_shared_globals returned.
  CHECK_CTX(Word(p, p.shared_globals() + 0x438) != 0, "globals+0x438 still 0: __libc_preinit_impl did not run");  // [libc code: __libc_preinit_impl's store]
  // __libc_preinit: `ldr r1, [TP, #0x14]; str r1, [__stack_chk_guard]`.
  const std::uint32_t guard = Word(p, p.thread_pointer() + 0x14);
  const auto chk = p.libc()->FindExport("__stack_chk_guard");
  CHECK(chk.has_value());
  CHECK_CTX(guard != 0, "TP+0x14 (the main thread's stack guard) is 0");
  if (chk) {
    CHECK_CTX(Word(p, chk->addr) == guard,  // [libc code: __libc_preinit's copy]
              "__stack_chk_guard=" + Hex(Word(p, chk->addr)) + " TP+0x14=" + Hex(guard));
  }
}

/// The main thread is bionic's shape, reached through bionic's own code.
void main_thread_is_real(const Images& im, int tier) {
  Booted b = Boot(tier, im.libc, im.libm, im.libz);
  CHECK_CTX(b.p != nullptr, std::string{"Tier "} + TierName(tier) + ": " + b.err);
  if (!b.p) return;
  GuestProcess& p = *b.p;
  const std::uint32_t thread = p.main_thread();
  const std::uint32_t tp = p.thread_pointer();
  CHECK(thread != 0 && tp != 0);

  const Call self = CallLibc(p, tier, "pthread_self", {}, "I");
  CHECK_CTX(self.ok && self.r0 == thread, self.why + " = " + Hex(self.r0) + " want " + Hex(thread));  // [libc code: pthread_self]
  const Call tid = CallLibc(p, tier, "gettid", {}, "I");
  CHECK_CTX(tid.ok && tid.r0 == Word(p, thread + 0x8), tid.why + " = " + Hex(tid.r0));  // [libc code: gettid]
  CHECK(Word(p, thread + 0x8) == static_cast<std::uint32_t>(GuestProcess::kMainTid));  // [host echo]
  const Call err = CallLibc(p, tier, "__errno", {}, "I");
  CHECK_CTX(err.ok && err.r0 == thread + 0x29c, err.why + " = " + Hex(err.r0));  // [libc code: __errno]

  // errno round trip through bionic: strtol overflow sets ERANGE (34) via __errno() = thread + 0x29c.
  std::string merr;
  const std::uint32_t buf = p.MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &merr);
  CHECK_CTX(buf != 0, merr);
  if (buf != 0) {
    const char big[] = "99999999999999999999";
    p.memory().Write(buf, big, sizeof big);
    const Call r = CallLibc(p, tier, "strtol",
                            {abi::Value32::Ptr(buf), abi::Value32::Ptr(0), abi::Value32::S32(10)}, "ILLI");
    CHECK_CTX(r.ok && r.r0 == 0x7fff'ffffu, r.why + " = " + Hex(r.r0));  // [libc code: strtol]
    CHECK_CTX(Word(p, thread + 0x29c) == 34u, "errno at thread+0x29c = " + Hex(Word(p, thread + 0x29c)));  // [libc code: errno write]
  }

  const auto list = p.libc()->FindLocalSymbol("g_thread_list");
  CHECK(list.has_value());
  if (list) CHECK_CTX(Word(p, *list) == thread, "g_thread_list head = " + Hex(Word(p, *list)));  // [libc code: __libc_init_common]
  // The sole entry: next = prev = 0.
  CHECK_CTX(Word(p, thread + 0x0) == 0 && Word(p, thread + 0x4) == 0,  // [libc code + device probe: sole entry]
            "next = " + Hex(Word(p, thread)) + " prev = " + Hex(Word(p, thread + 4)));
  CHECK(Word(p, tp + 0x4) == thread);                                // TLS_SLOT_THREAD_ID  // [image + libc code: __init_tcb]
  CHECK(Word(p, tp - 0x4) == Word(p, thread + 0x298));               // TLS_SLOT_BIONIC_TLS  // [image]
  CHECK(Word(p, thread + 0x2a0) == tp - 0xc);                        // bionic_tcb  // [image]
  CHECK(Word(p, thread + 0x30) == 0);                                // start_routine 0: bionic's is_main()  // [device probe]
  std::uint8_t b2c4 = 0xff;
  p.memory().Read(thread + 0x2c4, &b2c4, 1);
  CHECK_CTX(b2c4 == 0, "byte at +0x2c4 = " + Hex(b2c4) + "; the device's own main thread has 0 there");  // [device probe]
  CHECK(Word(p, thread + 0x10) == 0x4u);                              // attr.flags, as the device's main thread  // [device probe]
  CHECK(p.monitor().clear_child_tid() == thread + 0x8);              // set_tid_address(&main_thread.tid)  // [host echo]
  // bionic_tls's temp-buffer pointer (basename/dirname): a writable 0x2000-byte region.
  const std::uint32_t bt = Word(p, thread + 0x298);
  const std::uint32_t bufs = Word(p, bt + 0x414);
  CHECK_CTX(bufs != 0 && p.memory().ProtAt(bufs) == (mem::kRead | mem::kWrite) &&  // [device probe: scratch at static-TLS end, 0x2000]
                p.memory().ProtAt(bufs + 0x1fffu) == (mem::kRead | mem::kWrite),
            "bionic_tls+0x414 = " + Hex(bufs));
}

/// The static-TLS fields equal what this image's `__init_static_tls`/`pthread_create` consume, the derived
/// sizes are the asserted ones, and libc's own `__libc_get_static_tls_bounds` agrees from the guest side.
void static_tls_fields_match_libc(const Images& im, int tier) {
  Booted b = Boot(tier, im.libc, im.libm, im.libz);
  CHECK_CTX(b.p != nullptr, std::string{"Tier "} + TierName(tier) + ": " + b.err);
  if (!b.p) return;
  GuestProcess& p = *b.p;
  const runtime::BionicLayout& l = p.layout();
  CHECK(l.thread_size == 0x2c8u);  // [image]
  CHECK(l.tcb_size == 0x2cu);  // [device probe]
  CHECK(l.tp_offset_in_tcb == 0xcu);  // [image]
  CHECK(l.bionic_tls_size == 0xb4cu);  // [device probe]
  CHECK(l.offset_bionic_tcb == 0u);  // [device probe]
  CHECK(l.offset_bionic_tls == 0x2cu);  // [device probe]
  CHECK(l.static_tls_size == 0xb78u);  // [derived from the above]

  const std::uint32_t g = p.shared_globals();
  CHECK_CTX(Word(p, g + 0x420) == l.static_tls_size, "+0x420 = " + Hex(Word(p, g + 0x420)));  // [host echo of the layout]
  CHECK_CTX(Word(p, g + 0x428) == l.offset_bionic_tcb, "+0x428 = " + Hex(Word(p, g + 0x428)));  // [host echo]
  CHECK_CTX(Word(p, g + 0x42c) == l.offset_bionic_tls, "+0x42c = " + Hex(Word(p, g + 0x42c)));  // [host echo]
  CHECK_CTX(Word(p, g + 0x464) == 0u, "tls_modules count = " + Hex(Word(p, g + 0x464)));  // [host echo]

  const std::uint32_t tp = p.thread_pointer();
  const std::uint32_t static_tls = tp - l.tp_offset_in_tcb - l.offset_bionic_tcb;
  CHECK(Word(p, tp - 4) == static_tls + l.offset_bionic_tls);  // [host echo]

  std::string merr;
  const std::uint32_t out = p.MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &merr);
  CHECK_CTX(out != 0, merr);
  if (out != 0) {
    const Call c = CallLibc(p, tier, "__libc_get_static_tls_bounds",
                            {abi::Value32::Ptr(out), abi::Value32::Ptr(out + 4)}, "VLL");
    CHECK_CTX(c.ok, c.why);
    CHECK_CTX(Word(p, out) == static_tls, "start = " + Hex(Word(p, out)) + " want " + Hex(static_tls));  // [libc code: __libc_get_static_tls_bounds]
    CHECK_CTX(Word(p, out + 4) - Word(p, out) == l.static_tls_size,  // [libc code]
              "end - start = " + Hex(Word(p, out + 4) - Word(p, out)));
  }
}

/// A libc whose `pthread_create` places a differently sized `pthread_internal_t` is refused by name.
void a_different_libc_layout_is_refused_by_name(const Images& im) {
  std::vector<std::uint8_t> patched = im.libc;
  // `sub.w r0, r0, #0x2c8` (f5a0 7032): pthread_create's `stack_top - sizeof(pthread_internal_t)`.
  const std::uint8_t sub[] = {0xa0, 0xf5, 0x32, 0x70};
  std::size_t hits = 0;
  for (std::size_t i = 0; i + 4 <= patched.size(); i += 2) {
    if (std::memcmp(&patched[i], sub, 4) == 0) {
      patched[i + 2] = 0x34;  // #0x2d0
      ++hits;
    }
  }
  CHECK_CTX(hits > 0, "the image carries no `sub.w r0, r0, #0x2c8` to patch");
  Booted b = Boot(2, patched, im.libm, im.libz, /*run_ctors=*/false);
  CHECK_CTX(b.p == nullptr, "LoadScope accepted a libc with a different pthread_internal_t size");
  CHECK_CTX(b.err.find("pthread_internal_t") != std::string::npos, "the refusal does not name it: " + b.err);
  std::fprintf(stderr, "# patched libc refused: %s\n", b.err.c_str());
}

/// `.symtab` lookups: locals by plain or (Itanium-mangled) source name; nothing without a `.symtab`.
void find_local_symbol(const Images& im) {
  Booted b = Boot(2, im.libc, im.libm, im.libz, /*run_ctors=*/false);
  CHECK_CTX(b.p != nullptr, b.err);
  if (b.p) {
    const loader::GuestModule* libc = b.p->libc();
    const auto st = libc->FindLocalSymbol("__start_thread");
    CHECK_CTX(st && *st == libc->base + 0x79124u, st ? Hex(*st) : std::string{"nullopt"});  // [image: llvm-readelf -s]
    const auto gl = libc->FindLocalSymbol("g_thread_list");  // `_ZL13g_thread_list`
    CHECK_CTX(gl && *gl == libc->base + 0xe40bcu, gl ? Hex(*gl) : std::string{"nullopt"});  // [image: llvm-readelf -s]
    const auto pc = libc->FindLocalSymbol("pthread_create");  // a global, also in .symtab
    CHECK(pc && *pc == libc->FindExport("pthread_create")->addr);  // [image: .dynsym]
    CHECK(!libc->FindLocalSymbol("no_such_symbol_anywhere"));
    // Before constructors the list is empty: it is libc's own __libc_init_common that links the main
    // thread (`__pthread_internal_add(__get_thread())`), not the linker — so not GuestProcess either.
    if (gl) CHECK_CTX(Word(*b.p, *gl) == 0, "g_thread_list before constructors = " + Hex(Word(*b.p, *gl)));
  }

  const auto load = [](const std::vector<std::uint8_t>& image, const char* name,
                       std::unique_ptr<mem::GuestMemory>& gm) -> std::optional<loader::GuestModule> {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    model.host_page_shift = HostPageShift();
    std::string err;
    gm = mem::GuestMemory::Create(16u << 20, model, &err);
    if (!gm) return std::nullopt;
    loader::LoadOptions opts;
    opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return kTrap; };
    opts.unresolved_trap = kTrap;
    auto m = loader::LoadFromBytes(*gm, image, name, opts, &err);
    if (!m) std::fprintf(stderr, "# %s did not load: %s\n", name, err.c_str());
    return m;
  };
  const auto fixture = read_file(RB_FIXTURE_PATH);
  const auto stripped = read_file(RB_STRIPPED_FIXTURE_PATH);
  CHECK_CTX(!fixture.empty(), RB_FIXTURE_PATH);
  CHECK_CTX(!stripped.empty(), RB_STRIPPED_FIXTURE_PATH);
  const char* fib = "Java_com_retrobug_fixture_MainActivity_fib";
  std::unique_ptr<mem::GuestMemory> gm1, gm2;
  // (An image with no section headers at all is not a case to test here: the loader refuses it before
  // FindLocalSymbol could be asked — it bounds .dynsym through its section header.)
  if (!fixture.empty()) {
    const auto m = load(fixture, "librb_fixture.so", gm1);
    CHECK(m.has_value());
    if (m) CHECK(m->FindLocalSymbol(fib) && *m->FindLocalSymbol(fib) == m->FindExport(fib)->addr);
  }
  if (!stripped.empty()) {
    const auto s = load(stripped, "librb_fixture_stripped.so", gm2);
    CHECK(s.has_value());
    if (s) {
      CHECK(s->FindExport(fib).has_value());   // still exported through .dynsym
      CHECK(!s->FindLocalSymbol(fib));         // but there is no .symtab to find it in
    }
  }
}

/// A trivial guest call through GuestProcess's bridge costs microseconds on Tier J, booted or not: 1000
/// calls of a planted `bx lr` and 200 of libc's `getpid` each under a generous bound. Guards the per-call
/// path against a regression that would make a boot of hundreds of constructors (Angry Birds Go: 258)
/// slow enough to ANR. Measured on R52Y106BECZ (shell): 0.8 ms and 3 ms; an app process whose screen is
/// off runs several times slower (background cpuset), which the bound leaves room for.
void trivial_tier_j_calls_are_fast(const Images& im) {
  Booted b = Boot(2, im.libc, im.libm, im.libz);
  CHECK_CTX(b.p != nullptr, b.err);
  if (!b.p) return;
  GuestProcess& p = *b.p;
  std::string e;
  const std::uint32_t at = p.MapFree(0x1000, mem::kRead | mem::kWrite, &e);
  const std::uint32_t bx = 0xe12f'ff1eu;  // bx lr
  CHECK_CTX(at != 0 && p.memory().Write(at, &bx, 4) && p.memory().Protect(at, 0x1000, mem::kRead | mem::kExec, &e), e);
  p.abi().set_executor(p.engine(2));
  const auto t0 = std::chrono::steady_clock::now();
  bool all = true;
  for (int i = 0; i < 1000 && all; ++i) {
    exec::RunResult rr{};
    p.abi().CallGuest(at, {}, abi::Shorty::For("V"), 1000, &rr);
    all = rr.reason == exec::StopReason::Returned;
  }
  const double bx_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(all);
  const auto t1 = std::chrono::steady_clock::now();
  Call c;
  for (int i = 0; i < 200; ++i) c = CallLibc(p, 2, "getpid", {}, "I");
  const double pid_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  CHECK_CTX(c.ok && c.r0 == static_cast<std::uint32_t>(GuestProcess::kPid), c.why);
  std::fprintf(stderr, "# Tier J: 1000 trivial calls %.1f ms, 200 getpid %.1f ms\n", bx_ms, pid_ms);
  CHECK_CTX(bx_ms < 1000.0, "1000 trivial Tier J calls took " + std::to_string(bx_ms) + " ms (bound 1000)");
  CHECK_CTX(pid_ms < 1000.0, "200 getpid calls on Tier J took " + std::to_string(pid_ms) + " ms (bound 1000)");
}

/// A module whose constructors fault part-way (tests/corpus/ctor_fault.c: [0] counts itself, [1] calls an
/// unmapped address). The first RunConstructors fails naming DT_INIT_ARRAY[1]; a second refuses by name
/// and does not run [0] again — `g_ctor_runs` stays 1 — because a half-initialized module must not be
/// initialized twice.
void faulted_constructors_are_not_rerun(const Images& im, int tier) {
  const std::vector<std::uint8_t> image = read_file(RB_CTOR_FAULT_PATH);
  CHECK_CTX(!image.empty(), std::string{"nothing at "} + RB_CTOR_FAULT_PATH);
  if (image.empty()) return;
  Booted b = Boot(tier, im.libc, im.libm, im.libz);
  CHECK_CTX(b.p != nullptr, b.err);
  if (!b.p) return;
  GuestProcess& p = *b.p;
  std::string err;
  const loader::GuestModule* m = p.LoadApp(image, "librbctorfault.so", &err);
  CHECK_CTX(m != nullptr, err);
  if (m == nullptr) return;
  const auto counter = m->FindExport("g_ctor_runs");
  CHECK(counter.has_value());
  if (!counter) return;
  std::string failed;
  CHECK(!p.RunConstructors(&failed));
  CHECK_CTX(failed.find("librbctorfault.so DT_INIT_ARRAY[1]") != std::string::npos, failed);
  CHECK_CTX(Word(p, counter->addr) == 1u, "after the first run g_ctor_runs=" + Hex(Word(p, counter->addr)));
  std::string again;
  CHECK(!p.RunConstructors(&again));
  CHECK_CTX(again.find("librbctorfault.so: its constructors faulted in an earlier RunConstructors") != std::string::npos,
            again);
  CHECK_CTX(Word(p, counter->addr) == 1u, "a second RunConstructors reran [0]: g_ctor_runs=" + Hex(Word(p, counter->addr)));
  std::fprintf(stderr, "# Tier %s: faulted constructors: %s\n#   then refused: %s\n", TierName(tier), failed.c_str(),
               again.c_str());
}

}  // namespace

int main() {
  const Images im = LoadImages();
  if (im.ok) {
    for (int tier : {1, 2}) {
      boots_the_real_libc(im, tier);
      main_thread_is_real(im, tier);
      static_tls_fields_match_libc(im, tier);
      faulted_constructors_are_not_rerun(im, tier);
    }
    a_different_libc_layout_is_refused_by_name(im);
    find_local_symbol(im);
    trivial_tier_j_calls_are_fast(im);
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
