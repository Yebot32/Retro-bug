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
#include "rb/linker/guest_linker.h"
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
  /// The global symbol scope. Owned here because the two modules below are its, not copies: `GuestLinker`
  /// keeps loaded modules in a `deque`, so these pointers stay valid for the linker's lifetime.
  std::unique_ptr<linker::GuestLinker> scope;
  loader::GuestModule* libm = nullptr;
  /// Held **by value**, not as a pointer into the `optional` that `LoadFromBytes` returned. The first
  /// version of this rig did the latter, and it produced a dangling `GuestModule*` the moment `build()`
  /// returned: every app-module lookup after that read freed memory, which presented as "the loaded
  /// library does not export `floatProbe`" on one device and as an unhandled SIGSEGV on the other. Two
  /// different symptoms, one cause, and neither of them where the message pointed.
  ///
  /// `libm` can be a pointer because it addresses a node of the linker's `deque`, which is heap-owned
  /// and stable; this one addresses a member of a `Rig` that is moved out of `build()` by value.
  std::optional<loader::GuestModule> app;
  std::unique_ptr<abi::AbiContext> abi_ctx;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::JitExecutor> jit;
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

  /**
   * What a library cannot answer from the loaded scope is asked of the host, and the host is this thin:
   * one slot that exists so a stack-protector prologue can read it, and the unmapped guard for anything
   * else. See the file header for why the unmapped binding is the deliberate choice.
   */
  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    return kUnresolved;
  };
  /**
   * `libm_image` is captured **by value**, which is not a stylistic choice: the linker keeps this
   * `std::function` for its whole lifetime, and a `[&]` here would leave it holding a reference to a
   * local that dies with `build()`. Nothing in this lane calls the provider after that, so the bug would
   * have been invisible — until a `dlopen` path did, which is what `DlopenGuest` is.
   */
  const linker::ImageProvider provider = [libm_image](const std::string& soname)
      -> std::optional<std::vector<std::uint8_t>> {
    if (soname == "libm.so") return libm_image;
    return std::nullopt;
  };
  rig.scope = std::make_unique<linker::GuestLinker>(*rig.mem, provider, host);

  // The guest's own libm first, so its exports can answer the app's imports.
  auto libm_mod = rig.scope->Link("libm.so", libm_image, &err);
  if (!libm_mod) {
    ++checks;
    fail("link the guest libm.so through C06", __LINE__, err);
    return rig;
  }
  rig.libm = *libm_mod;
  std::size_t functions = 0, thumb_functions = 0;
  for (const auto& sym : rig.libm->exports) {
    if (!sym.defined || !sym.is_function) continue;
    ++functions;
    if (sym.thumb) ++thumb_functions;
    /**
     * The linker's answer has to be the symbol's `st_value` **including the bit-0 Thumb tag**, which is
     * what a GOT entry and a `blx` expect: `FindExport`'s `CodeAddress` splits the tag out, and using
     * `.addr` instead was a real bug (caught by an independent review, not by this lane) — every import
     * resolved to an address with bit 0 clear, so a Thumb export would have been entered in ARM state.
     * With the real libm it stayed invisible because the functions this probe imports are all ARM; a
     * synthetic Thumb libm aborted the guest instead.
     *
     * Asserted over every export rather than over the three this app happens to import, because the
     * historical bug was invisible exactly when the imports were ARM-only, and a row that only covers the
     * imported three cannot see the tag going missing from the rest.
     */
    const auto got = rig.scope->Lookup(sym.name);
    CHECK_CTX(got.has_value() && *got == sym.value,
              "C06 Lookup('" + sym.name + "') = " + (got ? Hex(*got) : std::string{"<none>"}) +
                  ", the image's st_value is 0x" + Hex(sym.value));
  }
  std::fprintf(stderr, "# guest libm: %zu exported functions, %zu of them Thumb\n", functions,
               thumb_functions);

  /**
   * The app's own `DT_NEEDED` cannot be walked yet, and this lane says so out loud rather than by never
   * asking: `libmathprobe.so` needs `libdl.so` and `libc.so` and there is no guest libc to provide
   * (task #29). So the dependency-first path is *tried*, its refusal is *asserted* to be that and not
   * something else, and only then is the image placed directly into the same scope.
   *
   * The first two rows are a tripwire pointed at #29: the day a `libc.so` exists for the provider to
   * hand back, `Link` stops refusing and these checks go red with a message saying what to do — delete
   * the fallback below and let `RunInitializers` run the chain. Loading past an unresolvable `NEEDED` is
   * not something `GuestLinker` should ever learn to do silently.
   */
  std::string dep_err;
  const auto walked = rig.scope->Link("libmathprobe.so", *so, &dep_err);
  const std::string needed = [&] {
    std::string peek_err;
    std::string out;
    for (const auto& n : linker::PeekNeeded(*so, &peek_err)) out += (out.empty() ? "" : " ") + n;
    CHECK_CTX(peek_err.empty(), "pre-scanning the app's DT_NEEDED: " + peek_err);
    return out;
  }();
  std::fprintf(stderr, "# app DT_NEEDED [%s]; GuestLinker::Link refused: %s\n", needed.c_str(),
               dep_err.empty() ? "<no refusal>" : dep_err.c_str());
  CHECK_CTX(needed == "libdl.so libc.so",
            "the app's DT_NEEDED is '" + needed +
                "', which the readelf output recorded in docs/30 does not match — this lane's fallback "
                "below is justified by exactly that pair, so re-read why it exists if it changed");
  CHECK_CTX(!walked,
            "GuestLinker::Link resolved the app after all (" + dep_err +
                ") — a guest libc.so now exists, so #29 is done: drop the direct-load fallback below and "
                "run the chain through RunInitializers");
  CHECK_CTX(walked || dep_err.find("libc.so") != std::string::npos ||
                dep_err.find("libdl.so") != std::string::npos,
            "the refusal names neither of the two missing dependencies, so it is not the one this "
            "fallback exists for: " +
                dep_err);

  loader::LoadOptions app_opts;
  app_opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (const auto found = rig.scope->Lookup(name)) return found;
    return host(name);
  };
  auto app_mod = loader::LoadFromBytes(*rig.mem, *so, "libmathprobe.so", app_opts, &err);
  if (!app_mod) {
    ++checks;
    fail("load the APK's native library into guest memory", __LINE__, err);
    return rig;
  }
  // Deliberately *not* `scope->Adopt()`: the app is placed at the end of the address window but is not in
  // `order_`, so `RunInitializers` would skip its constructors. Nothing here calls it, and the reason it
  // cannot is the `DT_NEEDED` refusal asserted above. Recorded as a check so that the gap between "loaded"
  // and "linked" cannot quietly become the same word in someone's reading of this lane.
  rig.app = std::move(app_mod);
  CHECK_CTX(rig.scope->order().size() == 1,
            "C06's load order has " + std::to_string(rig.scope->order().size()) +
                " module(s), not the one this lane expects; the app is loaded outside the linker and so "
                "gets no constructors run — see the comment above");
  // `imports.size()` is NOT the number of symbols this library needs resolved, and printing it as if it
  // were is how "0 imports (0 unanswered)" appeared on a library that imports three functions: the
  // loader's `imports` vector only carries entries whose `.dynsym` type it recognises, and this probe's
  // are `NOTYPE`. What matters is which names went to the host, so `host_requests()` is printed.
  std::string asked;
  for (const auto& n : rig.scope->host_requests()) asked += (asked.empty() ? "" : " ") + n;
  std::fprintf(stderr,
               "# app .so: base 0x%s, extent 0x%s, %zu exported, %zu dynsym imports, host asked for "
               "[%s]\n",
               Hex(rig.app->base).c_str(), Hex(rig.app->extent).c_str(), rig.app->exports.size(),
               rig.app->imports.size(), asked.c_str());
  CHECK_CTX(asked.find("libc.so") == std::string::npos,
            "a DT_NEEDED soname was asked of the host as if it were a symbol: " + asked);

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
  exec::MachineState exit_state{};
  const abi::Ret32 ret =
      ctx.CallGuest(fn->tagged(), args, abi::Shorty::For(shorty), 4'000'000, &result, &exit_state);
  out.reason = result.reason;
  out.bits = ret.bits;
  out.detail = result.detail;
  out.steps = result.instructions_executed;
  char buf[256];
  /**
   * The callee's own last state, taken from `CallGuest`'s out-parameter.
   *
   * This used to read `ctx.state()`, which is the state *before* the call: `CallGuest` restores the
   * caller's registers on the way out because a nested call's caller is live, and so `ctx.state().r[0]`
   * after a top-level call is a zero that never had anything to do with the guest. Printing it beside a
   * non-zero result produced the conclusion that "the bridge returns a value its own state does not
   * hold" — a wrong diagnosis, published and then retracted, caused entirely by an instrument whose name
   * described the opposite of what it read.
   */
  const auto& st = exit_state;
  std::snprintf(buf, sizeof(buf),
                "kind=%d pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x sp=0x%08x "
                "d0=0x%016llx d1=0x%016llx cpsr=0x%08x",
                static_cast<int>(ret.kind), st.pc, st.r[14], st.r[0], st.r[1], st.r[2], st.r[3],
                st.r[13], static_cast<unsigned long long>(st.d[0]),
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
 * That is not a workaround for convenience — it is the experiment, and at the time it was the *only* way to
 * ask the question. Android's `armeabi-v7a` ABI is softfp, so a `double` parameter occupies a **core
 * register pair**, and passing the two halves as two `U32` arguments places them in r0:r1 exactly where such
 * a parameter belongs. These calls returned the hardware bits while `PlaceArgs` was putting `F64` arguments
 * in `d0` instead, which is what established the one as a defect rather than a reading of the ABI — and it
 * is why they still take the split form now, three revisions later: the row's job is to pin down *where a
 * guest's `double` parameter lives*, and once `PlaceArgs` agrees with them the second witness is worth
 * keeping rather than folding away. `docs/30` §Step 24, §Step 25.
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
      const Outcome got = call(*engine, rig, *rig.abi_ctx, s.name, args, "DLL", rig.libm);
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
      // The bridge's own view after the call, printed when the value is wrong. It is what settles
      // whether a disagreement between two instruments is in the engine or in one of the instruments:
      // the trace above walks its own `MachineState` with a budget of one, this is the state the call
      // itself left behind, and they have to describe the same moment.
      if (got.bits != s.want || !got.ok()) {
        std::fprintf(stderr, "       bridge after: %s\n", got.state_after.c_str());
      }
    }
  }
}

/// Appends `pc` unless it repeats the previous entry. Tier I runs several IR ops per guest encoding, so
/// its raw step trace repeats one address several times where Tier J's does not; collapsing the repeats
/// is what makes the two paths comparable at all.
void push_unique(std::vector<exec::GuestAddr>& path, exec::GuestAddr pc) {
  if (path.empty() || path.back() != pc) path.push_back(pc);
}

/**
 * Single-steps one tier through a guest call and returns the *guest* PC path it took.
 *
 * Why this exists rather than a comparison of end states: the tiers count different units (IR ops
 * against guest instructions), so their step counts are not comparable and never were meant to be — but
 * the sequence of addresses they visit is, and it is the only thing that localises a divergence to an
 * instruction instead of to a function. Consecutive repeats are collapsed, because Tier I executes
 * several IR ops per guest encoding and Tier J one; what is left is the path through the code.
 *
 * `docs/30` §Step 24's `sin`/`cos` failure was found this way after per-function probes had narrowed it
 * to "the reduction path" and no further: "the call returns with a leftover result" is consistent with
 * a wrong value and with a branch taken differently, and only the path distinguishes them.
 */
std::vector<exec::GuestAddr> trace_path(exec::Executor& engine, Rig& rig, const char* name,
                                        std::uint64_t arg_bits, std::size_t max_steps) {
  std::vector<exec::GuestAddr> path;
  const auto fn = rig.libm->FindExport(name);
  if (!fn) return path;
  exec::MachineState s{};
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  const abi::Args32 args = {abi::Value32::U32(static_cast<std::uint32_t>(arg_bits)),
                            abi::Value32::U32(static_cast<std::uint32_t>(arg_bits >> 32))};
  std::string err;
  if (!abi::PlaceArgs(*rig.mem, s, fn->tagged(), args, kReturnAddress,
                      abi::AbiContext::kCallFrameBytes, &err)) {
    return path;
  }
  push_unique(path, s.pc);
  for (std::size_t i = 0; i < max_steps; ++i) {
    const exec::RunResult r = engine.Run(s, 1);
    push_unique(path, s.pc);
    if (s.pc == kReturnAddress) break;
    if (r.reason != exec::StopReason::Budget && r.reason != exec::StopReason::Returned) {
      std::fprintf(stderr, "# trace of %s stopped early: reason=%d %s\n", name,
                   static_cast<int>(r.reason), r.detail.c_str());
      break;
    }
  }
  return path;
}

/// The first state each *distinct* PC was seen in, for one tier.
///
/// Comparing register values at the same address is the only instrument that finds a value bug when the
/// two tiers execute the same program: a path comparison cannot see it (they agree), and an end-state
/// comparison cannot localise it (it only says the answers differ). Where the two maps first disagree is
/// the instruction immediately after the one that computed something wrong.
std::map<exec::GuestAddr, exec::MachineState> first_states(exec::Executor& engine, Rig& rig,
                                                           const char* name,
                                                           std::uint64_t arg_bits) {
  std::map<exec::GuestAddr, exec::MachineState> seen;
  const auto fn = rig.libm->FindExport(name);
  if (!fn) return seen;
  exec::MachineState s{};
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  const abi::Args32 args = {abi::Value32::U32(static_cast<std::uint32_t>(arg_bits)),
                            abi::Value32::U32(static_cast<std::uint32_t>(arg_bits >> 32))};
  std::string err;
  if (!abi::PlaceArgs(*rig.mem, s, fn->tagged(), args, kReturnAddress,
                      abi::AbiContext::kCallFrameBytes, &err)) {
    return seen;
  }
  seen.emplace(s.pc, s);
  for (std::size_t i = 0; i < 200'000; ++i) {
    const exec::RunResult r = engine.Run(s, 1);
    seen.emplace(s.pc, s);
    if (s.pc == kReturnAddress) break;
    if (r.reason != exec::StopReason::Budget && r.reason != exec::StopReason::Returned) {
      // Printed rather than swallowed. The first version of this loop broke here silently, and the
      // instrument then reported "no register divergence anywhere" on a tier whose trace had simply
      // stopped early — a confident negative produced by a loop that gave up. A trace that ends for any
      // reason other than the sentinel is not a trace of the same thing the other tier ran.
      std::fprintf(stderr, "# trace[%s] stopped at 0x%s after %zu steps: reason=%d %s\n", name,
                   Hex(s.pc).c_str(), i, static_cast<int>(r.reason), r.detail.c_str());
      break;
    }
  }
  return seen;
}

std::string state_diff(const exec::MachineState& a, const exec::MachineState& b) {
  std::string out;
  for (int r = 0; r < 16; ++r) {
    if (a.r[r] != b.r[r]) {
      out += " r" + std::to_string(r) + "=0x" + Hex(a.r[r]) + "/0x" + Hex(b.r[r]);
    }
  }
  for (int d = 0; d < 8; ++d) {
    if (a.d[d] != b.d[d]) {
      out += " d" + std::to_string(d) + "=0x" + Hex64(a.d[d]) + "/0x" + Hex64(b.d[d]);
    }
  }
  if (a.cpsr != b.cpsr) out += " cpsr=0x" + Hex(a.cpsr) + "/0x" + Hex(b.cpsr);
  if (a.fpscr != b.fpscr) out += " fpscr=0x" + Hex(a.fpscr) + "/0x" + Hex(b.fpscr);
  return out;
}

/**
 * Runs one guest call to completion with a **single large budget**, the way `AbiContext` does, and
 * returns the final register state.
 *
 * This exists to answer one question that the step-by-step trace above cannot: is the tiers' divergence
 * in the engine, or is it an artifact of stepping with a budget of one? Repeated `Run(state, 1)` and one
 * `Run(state, N)` are *supposed* to be equivalent, and if they are not, then every stepper-based
 * instrument in this file — including three that already misled me — is suspect before the engine is.
 */
exec::MachineState run_to_sentinel(exec::Executor& engine, Rig& rig, const char* name,
                                   std::uint64_t arg_bits) {
  exec::MachineState s{};
  const auto fn = rig.libm->FindExport(name);
  if (!fn) return s;
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  const abi::Args32 args = {abi::Value32::U32(static_cast<std::uint32_t>(arg_bits)),
                            abi::Value32::U32(static_cast<std::uint32_t>(arg_bits >> 32))};
  std::string err;
  if (!abi::PlaceArgs(*rig.mem, s, fn->tagged(), args, kReturnAddress,
                      abi::AbiContext::kCallFrameBytes, &err)) {
    return s;
  }
  const exec::RunResult r = engine.Run(s, 4'000'000);
  std::fprintf(stderr, "# one-shot %s on Tier %s: reason=%d steps=%u r0=0x%s r1=0x%s d0=0x%s\n", name,
               engine.tier() == exec::CodeTier::Interp ? "I" : "J", static_cast<int>(r.reason),
               r.instructions_executed, Hex(s.r[0]).c_str(), Hex(s.r[1]).c_str(),
               Hex64(s.d[0]).c_str());
  return s;
}

void the_two_tiers_walk_the_same_path_through_libm() {
  Rig rig = build();
  if (!rig.ok) return;

  // sin(100.0): the one input of this function whose answer already differs between the tiers.
  run_to_sentinel(*rig.interp, rig, "sin", 0x4059'0000'0000'0000ull);
  run_to_sentinel(*rig.jit, rig, "sin", 0x4059'0000'0000'0000ull);
  const auto i_states = first_states(*rig.interp, rig, "sin", 0x4059'0000'0000'0000ull);
  const auto j_states = first_states(*rig.jit, rig, "sin", 0x4059'0000'0000'0000ull);

  std::fprintf(stderr, "# trace: Tier I reached %zu distinct addresses, Tier J %zu\n",
               i_states.size(), j_states.size());

  // Walk Tier J's visitation order is not recorded, so compare over the shared addresses in address
  // order: the first one whose state differs is still after the offending instruction, and printing a
  // handful of neighbours gives the reader the instruction to look at.
  unsigned compared = 0;
  bool found = false;
  exec::GuestAddr first_bad = 0;
  for (const auto& [pc, js] : j_states) {
    const auto it = i_states.find(pc);
    if (it == i_states.end()) continue;
    ++compared;
    if (!found) {
      const std::string diff = state_diff(it->second, js);
      if (!diff.empty()) {
        found = true;
        first_bad = pc;
        std::fprintf(stderr, "# trace: first register divergence at 0x%s:%s\n", Hex(pc).c_str(),
                     diff.c_str());
      }
    }
  }
  std::fprintf(stderr, "# trace: compared %u addresses both tiers reached\n", compared);
  CHECK_CTX(!found,
            "the tiers hold different registers at 0x" + Hex(first_bad) +
                " on the same input — the instruction before that address computed something "
                "differently");
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
      // method returns a `jdouble` — so it comes back in **r0:r1**, because the guest ABI is softfp and
      // that is where a softfp result of that width lands. An earlier draft of this comment said `d0`,
      // which is the hard-float answer, and the fix to `ReturnValue` left the sentence behind; a wrong
      // comment about the ABI in the file that tests the ABI is worth more attention than it looks.
      // What decided it was not the comment either way but the second draft's symptom: the first attempt
      // passed "IIDD", which declared an *int* return, so the bridge read r0 alone and the result's low 32
      // bits matched hardware exactly while its high half was zero. That is what a mis-declared return
      // looks like, and it is indistinguishable from a wrong answer unless the expectation is compared in
      // full.
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

/**
 * The five JNI methods that *take* a `jfloat`/`jdouble`, each one decided by a different placement rule.
 *
 * Why this is a separate set of rows and not an extension of `mathProbe`: `mathProbe` takes a `jint` and
 * returns a `jdouble`, so it exercises `ReturnValue` only. Until these existed, RetroBug had never placed
 * a floating-point argument across the host/guest boundary in anger — which is precisely how `PlaceArgs`
 * could spend weeks putting `F64` arguments into `d0..d7` (the hard-float convention) in a project whose
 * guests are all `-mfloat-abi=softfp`, where they belong in core register pairs. The fixture that would
 * have caught it earlier — `fp_args`, eight doubles passed *guest to guest* — never comes through the
 * bridge at all, so it could not have shown anything about it.
 *
 * The rows are chosen so that each fails for its own reason and no two are redundant:
 *
 *   - `floatProbe`      — a `jfloat` is one core register (r2), not `s0`.
 *   - `doubleProbe`     — a `jdouble` on an even slot is the pair r2:r3.
 *   - `intThenDouble`   — the pair rule *fails* to find two consecutive registers, so the double goes to
 *                         the stack and r3 is consumed by nothing. `jlong` cannot reach this case, because
 *                         an 8-byte integer in the tests below always still has r2:r3 free.
 *   - `twoDoubles`      — the first double exhausts the core bank, so the second is the first stack
 *                         argument.
 *   - `doubleIntDouble` — the block already holds one 4-byte argument when the double arrives, so it gets
 *                         an alignment pad. Every other row here starts the block at an offset that is
 *                         already 8-aligned, so none of them can see the pad.
 *
 * The expected values come from the same bytes run natively on the SM-X710
 * (`tools/build-guest-apk.sh` → `adb shell /data/local/tmp/oracle_linked`), and each was reproduced a
 * second time in node's IEEE arithmetic before being written down here.
 */
struct ArgRow {
  const char* method;
  const char* shorty;
  abi::Args32 args;
  std::uint64_t want;
};

abi::Args32 JniArgs(abi::Args32 tail) {
  // The two hidden JNI parameters, `env` and `clazz`, which is why every placement below starts at r2.
  // Both are null because none of these methods touches them — and that is worth stating, because a
  // probe that dereferenced `env` would be testing C09 rather than the ABI.
  abi::Args32 out = {abi::Value32::Ptr(0), abi::Value32::Ptr(0)};
  for (const abi::Value32& v : tail) out.push_back(v);
  return out;
}

void floating_point_arguments_land_where_softfp_puts_them() {
  Rig rig = build();
  if (!rig.ok) return;

  const std::vector<ArgRow> rows = {
      {"Java_com_retrobug_guestprobe_Probe_floatProbe", "FLLF",
       JniArgs({abi::Value32::F32(1.75f)}), 0x4068'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_floatProbe", "FLLF",
       JniArgs({abi::Value32::F32(-0.125f)}), 0x3f50'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_doubleProbe", "DLLD",
       JniArgs({abi::Value32::F64(10001.0)}), 0x40cd'4d40'0000'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_doubleProbe", "DLLD",
       JniArgs({abi::Value32::F64(0.1)}), 0x3ff2'6666'6666'6666ull},
      {"Java_com_retrobug_guestprobe_Probe_intThenDouble", "DLLID",
       JniArgs({abi::Value32::S32(7), abi::Value32::F64(10001.0)}), 0x40c3'8c00'0000'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_intThenDouble", "DLLID",
       JniArgs({abi::Value32::S32(-3), abi::Value32::F64(0.1)}), 0xc007'3333'3333'3333ull},
      {"Java_com_retrobug_guestprobe_Probe_twoDoubles", "DLLDD",
       JniArgs({abi::Value32::F64(10001.0), abi::Value32::F64(0.5)}), 0x40b3'8980'0000'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_doubleIntDouble", "DLLDL",
       JniArgs({abi::Value32::F64(2.25), abi::Value32::S32(7), abi::Value32::F64(0.125)}),
       0x401d'2000'0000'0000ull},
      {"Java_com_retrobug_guestprobe_Probe_doubleIntDouble", "DLLDL",
       JniArgs({abi::Value32::F64(0.1), abi::Value32::S32(-1), abi::Value32::F64(65537.0)}),
       0x40b9'98b3'3333'3334ull},
  };

  for (const ArgRow& r : rows) {
    Outcome interp{}, jit{};
    for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                         static_cast<exec::Executor*>(rig.jit.get())}) {
      const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
      const Outcome got = call(*engine, rig, *rig.abi_ctx, r.method, r.args, r.shorty);
      (engine->tier() == exec::CodeTier::Interp ? interp : jit) = got;
      CHECK_CTX(got.ok(), std::string{r.method} + " on Tier " + tier + ": reason=" +
                              std::to_string(static_cast<int>(got.reason)) + " " + got.detail);
      if (got.ok()) {
        CHECK_CTX(got.bits == r.want, std::string{r.method} + " on Tier " + tier + " = 0x" +
                                          Hex64(got.bits) + ", hardware said 0x" + Hex64(r.want));
      }
    }
    const char* short_name = strrchr(r.method, '_');
    std::fprintf(stderr, "[abi] %-16s hardware=0x%016llx I=0x%016llx%s J=0x%016llx%s\n",
                 short_name != nullptr ? short_name + 1 : r.method, static_cast<unsigned long long>(r.want),
                 static_cast<unsigned long long>(interp.bits), interp.bits == r.want ? "" : " <-- wrong",
                 static_cast<unsigned long long>(jit.bits), jit.bits == r.want ? "" : " <-- wrong");
    if (interp.bits != r.want) std::fprintf(stderr, "       I %s\n", interp.state_after.c_str());
    if (jit.bits != r.want) std::fprintf(stderr, "       J %s\n", jit.state_after.c_str());
    if (interp.ok() && jit.ok()) {
      CHECK_CTX(interp.bits == jit.bits,
                std::string{"the two tiers disagree about "} + r.method + ": I=0x" +
                    Hex64(interp.bits) + " J=0x" + Hex64(jit.bits));
    }
  }
}

}  // namespace

int main() {
  guest_libm_direct_calls_pin_the_softfp_convention();
  floating_point_arguments_land_where_softfp_puts_them();
  the_two_tiers_walk_the_same_path_through_libm();
  a_real_apk_library_loads_and_answers_like_hardware();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
