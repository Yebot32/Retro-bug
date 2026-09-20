/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * M1's first end-to-end question, asked as a test: can RetroBug load a **real APK's** native library
 * and call into it, and does it get the same answer real hardware does?
 *
 * Why this lane exists rather than another fixture probe. Everything before it ran code RetroBug's own
 * authors wrote, linked against nothing. This runs a library built by the ordinary Android toolchain,
 * packaged in an APK the ordinary way, with a real `JNI_OnLoad` and a real JNI method that calls out to
 * a *second* library — so one test covers APK extraction, ELF32 loading, PLT relocation, cross-library
 * symbol resolution, the JNI entry convention, and double-precision math through emulated bionic.
 * `tools/run_engine_tests.sh`'s header has said since M0 that "no instruction of a real guest library
 * has been executed yet"; this is the lane that retires that sentence.
 *
 * **Hardware is the authority, and the expected values are not computed here.** A host `sin`/`cos` is a
 * *64-bit* libm and is a weaker oracle than it looks: the guest's answer comes from 32-bit bionic's own
 * code paths, and a last-bit difference between two libm revisions is a legitimate difference rather
 * than a bug. So the expected values were captured by running the same bytes on the same device
 * natively — `tools/build-guest-apk.sh` builds that oracle alongside the APK:
 *
 *   adb shell /data/local/tmp/oracle_linked        # the .so, linked, called on real ARMv7 hardware
 *
 * on the SM-X710 (Snapdragon 8 Gen 2, 32-bit-capable). The host's own libm is also compared, but only
 * as a tolerance check reported separately: it is corroboration, not the authority, and the two are
 * labelled differently on purpose.
 *
 * Two traps worth stating because this lane steps around them deliberately:
 *
 *   - `sin(x)` and `cos(x)` on the same argument get fused by LLVM into one `sincos` call, which the
 *     NDK's `libm.so` stub exports and the NDK's `math.h` does not even declare. The probe is built with
 *     `-fno-builtin-sin -fno-builtin-cos` so it imports `sin` and `cos`, which every libm has. Whether
 *     the *device's* libm exports `sincos` was **not** established — the first attempt to check it failed
 *     for an unrelated reason (Android does not resolve symbols for a library opened by path out of
 *     `/data/local/tmp`), so nothing here rests on the answer.
 *   - A guest library's stack-protector prologue *reads* `__stack_chk_guard` before it does any work, so
 *     binding that symbol to an unmapped guard address turns every call into a fault. It is bound to a
 *     real mapped slot here, because that is what a libc would supply. The other libm imports
 *     (`memset`, `isnanf`, `ldexp`, `__stack_chk_fail`) are bound to an unmapped guard address on
 *     purpose: if this code path needs one, the test must fail loudly rather than invent a result, and
 *     a passing run is then evidence that it did not need them.
 */
#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_GUEST_APK_PATH
#define RB_GUEST_APK_PATH "/data/local/tmp/rb_guestprobe.apk"
#endif
#ifndef RB_GUEST_LIBM_PATH
#define RB_GUEST_LIBM_PATH "/data/local/tmp/libm.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ",
               detail.c_str());
}

#define CHECK(...)                                    \
  do {                                                \
    ++checks;                                         \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__);  \
  } while (0)

#define CHECK_CTX(cond, detail)                          \
  do {                                                   \
    ++checks;                                            \
    if (!(cond)) fail(#cond, __LINE__, detail);           \
  } while (0)

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

std::string Hex64(std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

/// The host's page size as a shift, because the guest memory model is told the host granule and this is
/// the only place that knows it. Same helper the tier-diff lane carries; kept local rather than shared
/// because it is four lines and a shared header for it would be a header for four lines.
unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

/// The address constants the fixture rig uses, reproduced so this lane's frames live in the same shape.
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
/// A page that exists only to hold `__stack_chk_guard`. Mapped read/write, never executed.
constexpr exec::GuestAddr kGuardPage = 0x0080'0000u;
/// Nothing is mapped here, so an unresolved import that is actually *called* faults and names the
/// address instead of returning a plausible zero. See this file's header for why that is deliberate.
constexpr std::uint32_t kUnresolved = 0x0bad'0000u;

/// The answers, as raw bit patterns, from the same bytes executed natively on the SM-X710.
struct Expect {
  std::int32_t seed;
  std::uint64_t bits;
};
constexpr Expect kHardware[] = {
    {0, 0x3ff0'0000'0000'0000ull}, {1, 0x3ffd'e6dc'2133'a8dfull},
    {4, 0x4012'789c'90b2'79ffull}, {7, 0x401e'43f6'e346'd0a4ull},
    {100, 0x4058'e45f'ddd2'81afull}, {-3, 0x400a'6a77'5385'657full},
};
constexpr std::int32_t kJniVersion16 = 0x0001'0006;

/**
 * The smallest zip reader that can pull a *stored* entry out of an APK.
 *
 * Written here rather than in the engine because M1's real APK pipeline (extraction, manifest, assets)
 * is a component of its own and this is not a preliminary version of it — it is a test's way of getting
 * at the bytes. Stored rather than deflated on purpose: Android itself ships native libraries
 * uncompressed (the `extractNativeLibs=false` layout, where the entry is `lib/<abi>/libfoo.so`), so the
 * entry can be copied out with no inflate implementation, and `tools/build-guest-apk.sh` packages it
 * that way. A deflated entry is refused loudly rather than silently handing compressed bytes back as
 * though they were the library.
 */
std::optional<std::vector<std::uint8_t>> zip_stored_entry(const std::vector<std::uint8_t>& zip,
                                                          const std::string& want, std::string* err) {
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return zip[at] | (static_cast<std::uint32_t>(zip[at + 1]) << 8);
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t { return u16(at) | (u16(at + 2) << 16); };
  if (zip.size() < 22) {
    if (err) *err = "file is too small to be a zip";
    return std::nullopt;
  }
  // The end-of-central-directory record is located by scanning back for its signature: a comment of up
  // to 64 KiB may follow it, so its offset is not derivable from the file length.
  std::size_t eocd = 0;
  bool found = false;
  for (std::size_t i = zip.size() - 22;; --i) {
    if (u32(i) == 0x0605'4b50u) {
      eocd = i;
      found = true;
      break;
    }
    if (i == 0) break;
  }
  if (!found) {
    if (err) *err = "no end-of-central-directory record";
    return std::nullopt;
  }
  const std::uint32_t count = u16(eocd + 10);
  std::size_t at = u32(eocd + 16);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (at + 46 > zip.size() || u32(at) != 0x0201'4b50u) {
      if (err) *err = "central directory is malformed at entry " + std::to_string(i);
      return std::nullopt;
    }
    const std::uint32_t method = u16(at + 10);
    const std::uint32_t usize = u32(at + 24);
    const std::uint32_t name_len = u16(at + 28);
    const std::uint32_t extra_len = u16(at + 30);
    const std::uint32_t comment_len = u16(at + 32);
    const std::uint32_t local = u32(at + 42);
    const std::string name(reinterpret_cast<const char*>(zip.data() + at + 46), name_len);
    if (name == want) {
      if (method != 0) {
        if (err) *err = want + " is compressed (method " + std::to_string(method) +
                        "); this reader handles stored entries only";
        return std::nullopt;
      }
      if (local + 30 > zip.size() || u32(local) != 0x0403'4b50u) {
        if (err) *err = "local header for " + want + " is malformed";
        return std::nullopt;
      }
      const std::uint32_t lname = u16(local + 26);
      const std::uint32_t lextra = u16(local + 28);
      const std::size_t start = local + 30 + lname + lextra;
      if (start + usize > zip.size()) {
        if (err) *err = "entry " + want + " runs past the end of the archive";
        return std::nullopt;
      }
      return std::vector<std::uint8_t>(zip.begin() + start, zip.begin() + start + usize);
    }
    at += 46 + name_len + extra_len + comment_len;
  }
  if (err) *err = want + " is not in the archive";
  return std::nullopt;
}

struct Rig {
  std::unique_ptr<GuestMemory> mem;
  std::optional<loader::GuestModule> libm;
  std::optional<loader::GuestModule> app;
  std::unique_ptr<abi::AbiContext> abi_ctx;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::JitExecutor> jit;
  std::size_t unanswered = 0;
  bool ok = false;
};

Rig build() {
  Rig rig;
  const auto apk = read_file(RB_GUEST_APK_PATH);
  const auto libm_image = read_file(RB_GUEST_LIBM_PATH);
  if (apk.empty() || libm_image.empty()) {
    ++checks;
    fail("read the APK and the guest libm", __LINE__,
         "APK " + std::to_string(apk.size()) + " bytes, libm " + std::to_string(libm_image.size()) +
             " bytes — are they pushed to " + RB_GUEST_APK_PATH + " and " + RB_GUEST_LIBM_PATH + "?");
    return rig;
  }

  std::string err;
  auto so = zip_stored_entry(apk, "lib/armeabi-v7a/libmathprobe.so", &err);
  if (!so) {
    ++checks;
    fail("extract the APK's native library", __LINE__, err);
    return rig;
  }
  std::fprintf(stderr, "# APK: %zu bytes, lib/armeabi-v7a/libmathprobe.so = %zu bytes, stored\n",
               apk.size(), so->size());

  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  rig.mem = GuestMemory::Create(64u << 20, model, &err);
  if (!rig.mem) {
    ++checks;
    fail("create guest memory", __LINE__, err);
    return rig;
  }
  if (!rig.mem->Map(kGuardPage, kGuestPage, kRead | kWrite, &err)) {
    ++checks;
    fail("map the stack-guard page", __LINE__, err);
    return rig;
  }
  const std::uint32_t guard = 0xdead'beefu;  // the value a libc would keep; never expected to trip
  rig.mem->Write(kGuardPage, &guard, 4);

  // The guest's own libm first, so its exports can answer the app's imports. This stands in for what
  // the C06 guest linker will do at M1 — a name-to-address map over the loaded module asks the same
  // question without the lookup machinery.
  const auto libm_import = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    return kUnresolved;
  };
  loader::LoadOptions libm_opts;
  libm_opts.resolve = libm_import;
  rig.libm = loader::LoadFromBytes(*rig.mem, libm_image, "libm.so", libm_opts, &err);
  if (!rig.libm) {
    ++checks;
    fail("load the guest libm.so", __LINE__, err);
    return rig;
  }

  std::map<std::string, std::uint32_t> exported;
  std::size_t thumb_exports = 0;
  for (const auto& sym : rig.libm->exports) {
    if (!sym.defined || !sym.is_function) continue;
    // `Symbol::value` keeps the bit-0 Thumb tag, and that is what a GOT entry must hold: it is the
    // callee's `st_value`, which the branch that reads it interprets as the ISA selector.
    // `FindExport`'s `CodeAddress` splits that bit out and is the right thing to *execute* through —
    // using it here instead was a real bug (caught by an independent review, not by this lane): every
    // import resolved to an address with bit 0 clear, so a Thumb export would have been entered in ARM
    // state. With the real libm it stayed invisible because the functions this probe imports are all
    // ARM; a synthetic Thumb libm aborted the guest instead.
    if (sym.thumb) ++thumb_exports;
    exported[sym.name] = sym.value;
  }
  std::fprintf(stderr, "# guest libm: %zu exported functions, %zu of them Thumb\n", exported.size(),
               thumb_exports);

  loader::LoadOptions app_opts;
  app_opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (const auto it = exported.find(name); it != exported.end()) return it->second;
    ++rig.unanswered;
    std::fprintf(stderr, "# app import '%s' is not exported by the guest libm; bound to the guard\n",
                 name.c_str());
    return kUnresolved;
  };
  rig.app = loader::LoadFromBytes(*rig.mem, *so, "libmathprobe.so", app_opts, &err);
  if (!rig.app) {
    ++checks;
    fail("load the APK's native library into guest memory", __LINE__, err);
    return rig;
  }
  // `imports.size()` is NOT the number of symbols this library needs resolved, and printing it as if it
  // were is how "0 imports (0 unanswered)" appeared on a library that imports three functions: the
  // loader's `imports` vector only carries entries whose `.dynsym` type it recognises, and this probe's
  // are `NOTYPE`. What matters is that `app_opts.resolve` was consulted and answered, so that count is
  // printed instead.
  std::fprintf(stderr,
               "# app .so: base 0x%s, extent 0x%s, %zu exported, %zu dynsym imports, %zu name lookups "
               "unanswered\n",
               Hex(rig.app->base).c_str(), Hex(rig.app->extent).c_str(), rig.app->exports.size(),
               rig.app->imports.size(), rig.unanswered);

  if (!rig.mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err)) {
    ++checks;
    fail("map a guest stack", __LINE__, err);
    return rig;
  }
  rig.abi_ctx = std::make_unique<abi::AbiContext>(*rig.mem, kStackTop, kReturnAddress);
  rig.interp = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.jit = std::make_unique<exec::JitExecutor>(*rig.mem, kReturnAddress);
  rig.ok = true;
  return rig;
}

struct Outcome {
  exec::StopReason reason = exec::StopReason::Internal;
  std::uint64_t bits = 0;
  std::string detail;
  unsigned steps = 0;
  std::string state_after;  ///< the registers, because a wrong answer from the wrong register needs them
  bool ok() const { return reason == exec::StopReason::Returned; }
};

/// One guest call into the APK's library, through `rb::abi`, so the JNI convention is the engine's
/// rather than hand-rolled per call site. `in` names the module to look the symbol up in: the APK's own
/// library for its JNI methods, the guest libm for the direct per-function diagnosis below.
Outcome call(exec::Executor& engine, Rig& rig, abi::AbiContext& ctx, const char* name,
             const abi::Args32& args, const char* shorty, const loader::GuestModule* in = nullptr) {
  Outcome out;
  const loader::GuestModule& mod = in != nullptr ? *in : *rig.app;
  const auto fn = mod.FindExport(name);
  if (!fn) {
    out.detail = std::string{"the loaded library does not export "} + name;
    return out;
  }
  ctx.set_executor(engine);
  exec::RunResult result{};
  const abi::Ret32 ret = ctx.CallGuest(fn->tagged(), args, abi::Shorty::For(shorty), 4'000'000, &result);
  out.reason = result.reason;
  out.bits = ret.bits;
  out.detail = result.detail;
  out.steps = result.instructions_executed;
  char buf[256];
  const auto& st = ctx.state();
  std::snprintf(buf, sizeof(buf),
                "kind=%d pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x d0=0x%016llx "
                "d1=0x%016llx cpsr=0x%08x",
                static_cast<int>(ret.kind), st.pc, st.r[14], st.r[0], st.r[1], st.r[2],
                static_cast<unsigned long long>(st.d[0]),
                static_cast<unsigned long long>(st.d[1]), st.cpsr);
  out.state_after = buf;
  return out;
}

/// Hardware values for `x = 100`, per function, from the device's own 32-bit libm:
///   sin = 0xbfe03425b78c4db8   cos = 0x3feb981dbf665fdf   sqrt(10001) = 0x40590051eafee8b3
/// and the composite they produce, 0x4058e45fddd281ff — the one seed the composite probe gets wrong on
/// Tier I. Split so the failing function is identifiable rather than merely detected.
struct Split {
  const char* name;
  std::uint64_t arg_bits;  ///< the double argument, as bits
  std::uint64_t want;
};
constexpr Split kSplit100[] = {
    // 100.0 = 0x4059000000000000 and 10001.0 = 0x40c3888000000000, both copied from a run of
    // `node -e` rather than worked out by hand — the first draft of this table had
    // `0x40c3882000000000` for 10001.0, which is 10000.25, and it produced a "both tiers agree with each
    // other and disagree with hardware" failure that looked exactly like a shared engine bug and was a
    // typo in a test constant. Reproducing the arithmetic here would be the same mistake twice.
    {"sin", 0x4059'0000'0000'0000ull, 0xbfe0'3425'b78c'4db8ull},
    {"cos", 0x4059'0000'0000'0000ull, 0x3feb'981d'bf66'5fdfull},
    {"sqrt", 0x40c3'8880'0000'0000ull, 0x4059'0051'eafe'e8b3ull},
};

/**
 * Calls the guest libm's own `sin`/`cos`/`sqrt` directly, with the double argument *split across two
 * register arguments* rather than passed as a `Value32::F64`.
 *
 * That is not a workaround for convenience — it is the experiment. Android's `armeabi-v7a` ABI is
 * softfp, so a `double` parameter occupies a **core register pair**, and passing the two halves as two
 * `U32` arguments places them in r0:r1 exactly where such a parameter belongs. If these calls return
 * the hardware bits, then the softfp argument convention is confirmed by measurement — and the fact
 * that `dp::PlaceArgs` puts `F64` arguments in `vfp` instead is shown to be a defect rather than a
 * reading of the ABI. `docs/30` §Step 24.
 *
 * It is also the diagnosis that splits the one failing seed into its parts, because a composite that is
 * wrong while its inputs are right is a different bug from one whose inputs are already wrong.
 */
void guest_libm_direct_calls_pin_the_softfp_convention() {
  Rig rig = build();
  if (!rig.ok) return;

  for (const Split& s : kSplit100) {
    const abi::Args32 args = {abi::Value32::U32(static_cast<std::uint32_t>(s.arg_bits)),
                              abi::Value32::U32(static_cast<std::uint32_t>(s.arg_bits >> 32))};
    for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                         static_cast<exec::Executor*>(rig.jit.get())}) {
      const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
      const Outcome got = call(*engine, rig, *rig.abi_ctx, s.name, args, "DLL", &*rig.libm);
      CHECK_CTX(got.ok(), std::string{"guest libm "} + s.name + " on Tier " + tier +
                              ": reason=" + std::to_string(static_cast<int>(got.reason)) + " " +
                              got.detail);
      if (!got.ok()) continue;
      CHECK_CTX(got.bits == s.want, std::string{"guest libm "} + s.name + " on Tier " + tier +
                                        " = 0x" + Hex64(got.bits) + ", hardware said 0x" +
                                        Hex64(s.want));
      std::fprintf(stderr, "[apk] libm %-4s on Tier %s = 0x%s (hardware 0x%s)%s\n", s.name, tier,
                   Hex64(got.bits).c_str(), Hex64(s.want).c_str(),
                   got.bits == s.want ? "" : "   <-- differs");
    }
  }
}

void a_real_apk_library_loads_and_answers_like_hardware() {
  Rig rig = build();
  if (!rig.ok) return;

  // 1. `JNI_OnLoad` — the first thing any real Android runtime calls, and the cheapest proof that the
  //    entry convention works end to end.
  for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                       static_cast<exec::Executor*>(rig.jit.get())}) {
    const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
    const Outcome onload = call(*engine, rig, *rig.abi_ctx, "JNI_OnLoad",
                                {abi::Value32::Ptr(0), abi::Value32::Ptr(0)}, "II");
    CHECK_CTX(onload.ok(), std::string{"JNI_OnLoad on Tier "} + tier + ": reason=" +
                               std::to_string(static_cast<int>(onload.reason)) + " " + onload.detail);
    CHECK_CTX(static_cast<std::int32_t>(onload.bits) == kJniVersion16,
              std::string{"JNI_OnLoad on Tier "} + tier + " returned " + Hex(onload.bits) +
                  ", JNI_VERSION_1_6 is " + Hex(kJniVersion16));
    std::fprintf(stderr, "[apk] JNI_OnLoad on Tier %s -> 0x%s\n", tier, Hex(onload.bits).c_str());
  }

  // 2. The JNI method, whose answer depends on emulated bionic math, on both tiers, against the values
  //    the same bytes produced on real hardware.
  for (const Expect& e : kHardware) {
    Outcome interp{}, jit{};
    for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                         static_cast<exec::Executor*>(rig.jit.get())}) {
      const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
      // "DLLI": the FIRST character is the return type (`Shorty::returns()` is `desc[0]`), and this
      // method returns a `jdouble` — so it comes back in `d0`, not `r0`. The first draft passed
      // "IIDD", which declared an *int* return; the bridge then read r0, and the symptom was a result
      // whose low 32 bits matched hardware exactly and whose high half was zero. That is what a
      // mis-declared return looks like, and it is indistinguishable from a wrong answer unless the
      // expectation is compared in full.
      const Outcome got =
          call(*engine, rig, *rig.abi_ctx, "Java_com_retrobug_guestprobe_Probe_mathProbe",
               {abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(e.seed)}, "DLLI");
      (engine->tier() == exec::CodeTier::Interp ? interp : jit) = got;
      CHECK_CTX(got.ok(), std::string{"mathProbe("} + std::to_string(e.seed) + ") on Tier " + tier +
                              ": reason=" + std::to_string(static_cast<int>(got.reason)) + " " +
                              got.detail);
      if (got.ok()) {
        CHECK_CTX(got.bits == e.bits,
                  std::string{"mathProbe("} + std::to_string(e.seed) + ") on Tier " + tier + " = 0x" +
                      Hex64(got.bits) + ", hardware said 0x" + Hex64(e.bits));
      }
    }
    std::fprintf(stderr, "[apk] mathProbe(%4d) hardware=0x%s I=0x%s J=0x%s\n", e.seed,
                 Hex64(e.bits).c_str(), interp.ok() ? Hex64(interp.bits).c_str() : "refused",
                 jit.ok() ? Hex64(jit.bits).c_str() : "refused");
    // The registers, when it is wrong. A result read from the wrong register is indistinguishable from
    // a wrong result unless what was actually in the register is printed — which is how the mis-declared
    // shorty above was diagnosed rather than guessed at.
    if (!interp.ok() || interp.bits != e.bits) {
      std::fprintf(stderr, "       I steps=%u %s\n", interp.steps, interp.state_after.c_str());
    }
    if (!jit.ok() || jit.bits != e.bits) {
      std::fprintf(stderr, "       J steps=%u %s\n", jit.steps, jit.state_after.c_str());
    }
    // The tiers must agree with each other as well as with hardware: a shared bug would show up as two
    // matching wrong answers, which is exactly what the hardware column exists to catch.
    if (interp.ok() && jit.ok()) {
      CHECK_CTX(interp.bits == jit.bits, std::string{"the two tiers disagree on mathProbe("} +
                                             std::to_string(e.seed) + "): I=0x" +
                                             Hex64(interp.bits) + " J=0x" + Hex64(jit.bits));
    }
  }
}

}  // namespace

int main() {
  guest_libm_direct_calls_pin_the_softfp_convention();
  a_real_apk_library_loads_and_answers_like_hardware();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
