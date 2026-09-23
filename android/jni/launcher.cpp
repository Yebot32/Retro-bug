/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The host-side launcher's native half: a real installed `arm64-v8a` app asks RetroBug's own engine to
 * load a real `armeabi-v7a` JNI library out of an APK and answer a call, and reports what came back.
 *
 * Why this exists alongside `core/tests/guest_apk_test.cpp`, which already performs the same sequence.
 * That lane is an `adb`-pushed executable, so it runs in the **shell** domain; this runs inside an
 * installed app process — `u:r:untrusted_app`, the domain RetroBug ships into, and the distinction
 * `tools/app_domain_probe.sh` exists because the shell numbers are "true but not the ones the design
 * depends on". The probe already answered the single-syscall questions there: its two result files record
 * anon `PROT_READ|WRITE|EXEC` and memfd-exec as **granted** in that domain on both attached phones, and a
 * 4 GiB reservation as succeeding only at a **biased** address — which is the model this file picks. What
 * has not been asked is whether the whole engine still reaches the hardware's numbers when an app process
 * is the one asking: a 64 MiB guest window, then dynarmic's 8 MiB per-instance code cache
 * (`kCodeCacheBytes` in `core/exec/jit.cpp`), then a guest call that takes real time. So the engine is not
 * being re-tested here; the platform is, with the engine's own answer as the readout.
 *
 * The acceptance values are in `kHardware` below, **transcribed rather than shared** with the test
 * lane's table of the same name. A common header would let either file rename or "fix" a constant and
 * silently move the other's bar, and the two lanes are not redundant: `tools/run_engine_tests.sh` lists
 * `rb_guest_apk_tests` as *advisory* — red on purpose while it carries named gaps — so a launcher that
 * reports PASS while the lane reports FAIL is a finding, not a tautology. Both tables say the same
 * thing about their provenance: the numbers came from these bytes executed natively on a 32-bit-capable
 * SM-X710 by `tools/build-guest-apk.sh`'s `oracle_linked`, not from host arithmetic.
 *
 * Two traps, both inherited from the lane and both still live here:
 *
 *   - The `ImageProvider` has to capture the guest images **by value**. `GuestLinker` stores the
 *     `std::function` as a member for its whole lifetime, so a capture by reference would leave it
 *     pointing at this function's parameters. Nothing in this file calls the provider after the scope is
 *     linked, which is exactly why the mistake survives compilation and review.
 *   - An import nothing in the guest scope answers is bound to `kUnresolved`, an address deliberately
 *     left unmapped, rather than to 0. The loader refuses the load when a resolver returns nullopt for a
 *     non-weak import, so the alternative to a loud address is a load that never happens; binding the
 *     unmapped page lets the image place, lets this report name the symbol that went unanswered, and
 *     still makes the guest fault the moment it actually calls it. A zero would do neither.
 */
#include <jni.h>

#include <android/log.h>

#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/linker/guest_linker.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#define RB_TAG "RBLAUNCH"
/// The milestones that a report line cannot cover. `Launch` returns its report as a string and the Java
/// half logs every line, but a `SIGSEGV` inside a guest call is not a C++ exception: it takes the process
/// before any string is built. These are the lines a tombstone then gets attributed to.
#define RB_LOG(...)                                    \
  do {                                                 \
    char _rb_line[512];                                \
    std::snprintf(_rb_line, sizeof _rb_line, __VA_ARGS__);         \
    __android_log_print(ANDROID_LOG_INFO, RB_TAG, "%s", _rb_line); \
  } while (0)

namespace {

using namespace rb;
using namespace rb::mem;

/// The guest's address layout, identical to `core/tests/guest_apk_test.cpp`'s rig so that the two lanes'
/// stack frames, sentinel and guard page have the same shape and a difference between them cannot be
/// blamed on where the code landed.
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
constexpr exec::GuestAddr kGuardPage = 0x0080'0000u;
constexpr exec::GuestAddr kStackBytes = 64u << 10;
/// Nothing is mapped here. An unresolved import that is *called* faults and names this address instead
/// of returning a plausible zero; see the file header.
constexpr std::uint32_t kUnresolved = 0x0bad'0000u;
constexpr std::size_t kWindowBytes = 64u << 20;
/// One guest call's instruction budget, the same figure the APK lane uses. Tier I counts IR ops and Tier
/// J guest encodings, so this bounds the two differently — it exists to stop a runaway guest, not to
/// measure it.
constexpr unsigned kCallBudget = 4'000'000;
constexpr std::int32_t kJniVersion16 = 0x0001'0006;
constexpr const char* kAppSoname = "libmathprobe.so";

/// The host's page granule as a shift, because `AddressModel::host_page_shift` is told the host granule
/// and must be **no finer** than it (`rb/mem/guest_memory.h`: a finer value is refused by `Create`, a
/// coarser one accepted). This is the only place in the launcher that knows the number, and it decides
/// whether a guest's 4 KiB pages each own a host page or share one — which is the condition under which
/// `rb::mem` moves pages out of fastmem and serves them through `Read`/`Write` instead. Both attached
/// phones report 4096 for an app process (`tests/appdomain/results-*.txt` print it as `pagesize=`), so
/// the sharing path is the one this run does *not* exercise. It is printed in the report's first line so
/// that a device which does ship 16 KiB pages is visible in the output rather than assumed away.
unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

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

const char* ReasonName(exec::StopReason r) {
  switch (r) {
    case exec::StopReason::Budget: return "Budget";
    case exec::StopReason::Returned: return "Returned";
    case exec::StopReason::HostCall: return "HostCall";
    case exec::StopReason::Syscall: return "Syscall";
    case exec::StopReason::Suspended: return "Suspended";
    case exec::StopReason::Unimplemented: return "Unimplemented";
    case exec::StopReason::Fault: return "Fault";
    case exec::StopReason::Internal: return "Internal";
    case exec::StopReason::Exited: return "Exited";
  }
  return "outside-the-enum";
}

/// One report line. `printf`-checked on purpose: on a phone this string is the only instrument, and a
/// `%x` handed a 64-bit value is a report that reads as a wrong answer rather than as a bug here.
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void Say(std::string* out, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) buf[0] = '\0';
  *out += buf;
  *out += '\n';
}

/// The answers, as raw bit patterns, from the same bytes executed natively on the SM-X710. Transcribed
/// from `core/tests/guest_apk_test.cpp`, and deliberately a second copy rather than a shared header —
/// the reason is in this file's header.
struct Expect {
  std::int32_t seed;
  std::uint64_t bits;
};
constexpr Expect kHardware[] = {
    {0, 0x3ff0'0000'0000'0000ull}, {1, 0x3ffd'e6dc'2133'a8dfull},
    {4, 0x4012'789c'90b2'79ffull}, {7, 0x401e'43f6'e346'd0a4ull},
    {100, 0x4058'e45f'ddd2'81afull}, {-3, 0x400a'6a77'5385'657full},
};

bool HardwareFor(std::int32_t seed, std::uint64_t* out) {
  for (const Expect& e : kHardware) {
    if (e.seed == seed) {
      *out = e.bits;
      return true;
    }
  }
  return false;
}

/// What `nativeLaunch`'s `method` argument may say. The symbols are the guest library's own exports —
/// its Java class is `com.retrobug.guestprobe.Probe`, a different package from this launcher's, so the
/// mangling is *not* derived from `com.retrobug.launcher` and a reader checking it against this APK's
/// package will find nothing. Checked against the image rather than written from the convention:
/// `llvm-readelf --dyn-syms build/guest-apk/libmathprobe.so` lists
/// `Java_com_retrobug_guestprobe_Probe_mathProbe` among its FUNC entries.
///
/// The shorty's **first** character is the return type (`Shorty::returns()` is `desc[0]`), so `"DLLI"`
/// declares a `jdouble` result, which under the guest's softfp ABI arrives in `r0:r1` — mis-declaring it
/// as `"ILLI"` would read r0 alone and produce a value whose low half matched hardware exactly. That
/// mistake and its symptom are recorded in `core/tests/guest_apk_test.cpp`'s call site.
struct GuestMethod {
  const char* name;
  const char* symbol;
  const char* shorty;
};
constexpr GuestMethod kMethods[] = {
    {"mathProbe", "Java_com_retrobug_guestprobe_Probe_mathProbe", "DLLI"},
};

const GuestMethod* MethodFor(const std::string& name) {
  for (const GuestMethod& m : kMethods) {
    if (name == m.name) return &m;
  }
  return nullptr;
}

/// A `jbyteArray` copied into a vector the engine can be handed. The images arrive as bytes rather than
/// as paths because that is the half of the job each side is good at: `rb::loader` takes bytes or an fd,
/// the guest library is one entry inside a zip, and `core/tests/guest_apk_test.cpp` carries a
/// stored-entry-only zip reader of its own to get at it. The Java side already has
/// `java.util.zip.ZipInputStream`, which handles either compression, so the extraction happens there and
/// this file sees only ELF images.
std::vector<std::uint8_t> Bytes(JNIEnv* env, jbyteArray array) {
  if (env == nullptr || array == nullptr) return {};
  const jsize len = env->GetArrayLength(array);
  if (len <= 0) return {};
  jbyte* raw = env->GetByteArrayElements(array, nullptr);
  if (raw == nullptr) return {};
  const auto* first = reinterpret_cast<std::uint8_t*>(raw);
  std::vector<std::uint8_t> out(first, first + len);
  // `JNI_ABORT`: nothing here writes through the pointer, and mode 0 would copy the (unchanged) bytes
  // back into the Java array on the way out.
  env->ReleaseByteArrayElements(array, raw, JNI_ABORT);
  return out;
}

struct Outcome {
  exec::StopReason reason = exec::StopReason::Internal;
  std::uint64_t bits = 0;
  unsigned steps = 0;
  std::string detail;
  /// The callee's own last state, taken from `CallGuest`'s out-parameter. It is **not** `ctx.state()`,
  /// which is the state before the call: the bridge restores the caller's registers on the way out. The
  /// lane that misread that published a wrong diagnosis, and the header says so (`rb/abi/abi.h`).
  exec::MachineState exit{};
  bool ok() const { return reason == exec::StopReason::Returned; }
};

/// One guest call, through `rb::abi`, so the JNI convention is the engine's and not hand-rolled here.
/// `engine` is swapped under one `AbiContext` per tier — the bridge *is* the host-entry table the
/// executor consults, so the pair cannot be built the other way round.
Outcome Call(exec::Executor& engine, abi::AbiContext& ctx, const loader::GuestModule& mod,
             const char* symbol, const char* shorty, const abi::Args32& args) {
  Outcome out;
  const auto fn = mod.FindExport(symbol);
  if (!fn) {
    out.detail = std::string{"the loaded library does not export "} + symbol;
    return out;
  }
  ctx.set_executor(engine);
  exec::RunResult result{};
  const abi::Ret32 ret =
      ctx.CallGuest(fn->tagged(), args, abi::Shorty::For(shorty), kCallBudget, &result, &out.exit);
  out.reason = result.reason;
  out.bits = ret.bits;
  out.steps = result.instructions_executed;
  out.detail = result.detail;
  return out;
}

/// The whole launch, as the report the Java half prints. Every failure path returns a report rather than
/// an error code, because the caller is an Activity with no console and "it returned null" is the least
/// informative answer it can be given.
std::string Launch(const std::vector<std::uint8_t>& app_image,
                   const std::vector<std::uint8_t>& libm_image, const std::string& method,
                   std::int32_t seed) {
  std::string out;
  const unsigned page_shift = HostPageShift();
  Say(&out, "launch: %s=%zu bytes libm.so=%zu bytes method=%s seed=%d host_page_shift=%u",
      kAppSoname, app_image.size(), libm_image.size(), method.c_str(), static_cast<int>(seed),
      page_shift);

  if (app_image.empty() || libm_image.empty()) {
    Say(&out, "FATAL: a guest image arrived empty — the APK entry or the libm asset is missing");
    return out;
  }
  /*
   * The gate the import pipeline uses: class 1 (32-bit) and machine EM_ARM. If the Java half hands back
   * the wrong zip entry, or an entry's compressed bytes rather than its contents, this says so here
   * instead of the loader reporting a parse failure against a byte offset nobody can look up on a phone.
   */
  if (!loader::LooksLikeAArch32Elf(app_image.data(), app_image.size())) {
    Say(&out, "FATAL: the %s bytes are not an ELF32 ARM image — refusing to load them", kAppSoname);
    return out;
  }
  const GuestMethod* m = MethodFor(method);
  if (m == nullptr) {
    Say(&out, "FATAL: unknown method '%s'; this launcher knows:", method.c_str());
    for (const GuestMethod& known : kMethods) Say(&out, "  %s", known.name);
    return out;
  }

  std::string err;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = page_shift;
  std::unique_ptr<GuestMemory> mem = GuestMemory::Create(kWindowBytes, model, &err);
  if (!mem) {
    Say(&out, "FATAL: GuestMemory::Create(%zu) refused: %s", kWindowBytes, err.c_str());
    return out;
  }
  Say(&out, "mem: %zu-byte window, guest model kBiased, stack 0x%s..0x%s, sentinel 0x%s, guard 0x%s",
      kWindowBytes, Hex(kStackTop - kStackBytes).c_str(), Hex(kStackTop).c_str(),
      Hex(kReturnAddress).c_str(), Hex(kGuardPage).c_str());

  // The stack-protector slot first, because a protected guest function *reads* it in its prologue before
  // it does any work, so binding it to the unmapped guard below would turn every call into a fault. The
  // value is what a libc would keep there; nothing expects it to trip.
  if (!mem->Map(kGuardPage, kGuestPage, kRead | kWrite, &err)) {
    Say(&out, "FATAL: mapping the stack-guard page failed: %s", err.c_str());
    return out;
  }
  const std::uint32_t guard = 0xdead'beefu;
  mem->Write(kGuardPage, &guard, 4);
  if (!mem->Map(kStackTop - kStackBytes, kStackBytes, kRead | kWrite, &err)) {
    Say(&out, "FATAL: mapping the guest stack failed: %s", err.c_str());
    return out;
  }

  /*
   * The host half of symbol resolution: one slot a stack-protector prologue can read, and the unmapped
   * guard for everything else. The names that take the second branch are recorded, because "the app
   * loaded" is only an honest statement alongside "and these of its imports were answered with an address
   * that cannot be called".
   *
   * Measured on the bytes this APK carries (`cmp` says its `lib/armeabi-v7a/libmathprobe.so` entry and
   * `build/guest-apk/libmathprobe.so` are the same 3284 bytes, and `llvm-readelf --dyn-syms` on that file
   * gives an undefined set of exactly `sin`, `cos`, `sqrt`): all three are answered by the guest libm
   * below, so for this build the list this fills is expected to stay empty. It is here because the next
   * guest library will not necessarily be that one.
   */
  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    return kUnresolved;
  };
  // Captured by value — see the file header for what that prevents.
  const linker::ImageProvider provider = [libm_image](const std::string& soname)
      -> std::optional<std::vector<std::uint8_t>> {
    if (soname == "libm.so") return libm_image;
    return std::nullopt;
  };
  std::unique_ptr<linker::GuestLinker> linker =
      std::make_unique<linker::GuestLinker>(*mem, provider, host);

  auto libm_mod = linker->Link("libm.so", libm_image, &err);
  if (!libm_mod) {
    Say(&out, "FATAL: GuestLinker::Link(\"libm.so\") failed: %s", err.c_str());
    return out;
  }
  const loader::GuestModule* libm = *libm_mod;
  std::size_t libm_functions = 0;
  for (const auto& sym : libm->exports) {
    if (sym.defined && sym.is_function) ++libm_functions;
  }
  Say(&out, "linker: libm.so at 0x%s, %zu defined functions, %zu module(s) in load order",
      Hex(libm->base).c_str(), libm_functions, linker->order().size());

  /**
   * The app is placed with `LoadFromBytes` and is *not* run through `Link`, because its own `DT_NEEDED`
   * is `libdl.so libc.so` — read off this image with `llvm-readelf -d`, and the pair
   * `core/tests/guest_apk_test.cpp` asserts for it — and there is no guest libc for the provider to hand
   * back, which that lane's header records as the open item behind its own fallback. `Link` refuses to
   * place past an unresolvable `NEEDED`, so the choice is this or no launch at all. Its three PLT imports
   * are answered from the scope, and that cross-library resolution is the part of M1 being measured.
   *
   * Consequence worth naming: not being in `order()` means no constructors of this module would be run by
   * `RunInitializers`. Nothing here calls it; the two entry points below are reached directly.
   */
  std::vector<std::string> to_host;
  loader::LoadOptions app_opts;
  // Captured by reference, and that is safe here where the provider above had to be by value:
  // `LoadOptions::resolve` is only consulted inside `LoadFromBytes` below, which this function calls
  // synchronously and does not retain.
  app_opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (const auto found = linker->Lookup(name)) return found;
    to_host.push_back(name);
    return host(name);
  };
  auto app = loader::LoadFromBytes(*mem, app_image, kAppSoname, app_opts, &err);
  if (!app) {
    Say(&out, "FATAL: loading %s failed: %s", kAppSoname, err.c_str());
    return out;
  }
  std::string asked;
  for (const std::string& name : to_host) asked += (asked.empty() ? "" : " ") + name;
  // `imports.size()` is not the number of symbols that had to be resolved: the loader only records
  // `.dynsym` entries whose type it recognises, and this image's three are `NOTYPE` (measured with
  // `llvm-readelf --dyn-syms`), so the field can read 0 beside three real PLT slots. `to_host` below is
  // the list that means something.
  Say(&out, "app: %s at 0x%s extent 0x%s, %zu exports, %zu recorded dynsym imports", kAppSoname,
      Hex(app->base).c_str(), Hex(app->extent).c_str(), app->exports.size(), app->imports.size());
  Say(&out, "app: imports the guest scope did not answer: [%s]", asked.empty() ? "none" : asked.c_str());

  abi::AbiContext ctx{*mem, kStackTop, kReturnAddress};
  // The context is passed as the executors' `HostDispatch`, which `guest_apk_test.cpp` does not do and
  // which changes nothing for these two entry points (`mathProbe` ignores `env` and `clazz`, so the guest
  // never branches to a JNI table slot). What it buys is the failure message: a guest that *did* reach a
  // slot gets "host call at slot N is not bound" naming the slot, rather than a fault against an address.
  exec::InterpExecutor interp{*mem, kReturnAddress, &ctx};
  exec::JitExecutor jit{*mem, kReturnAddress, &ctx};
  if (!jit.ok()) Say(&out, "tier J is unavailable before any guest code ran: %s", jit.error().c_str());

  unsigned pass = 0, fail = 0;
  // One call's line, plus the two lines only a failure earns. `want_bits` is what is compared and
  // `have_hardware` says whether the comparison means anything: without the second, a seed with no
  // captured answer would be compared against a zero this file invented and **PASS** on the call that
  // happened to return one. The Java half only asks for seeds that have rows, so this is a guard against
  // the next caller rather than a bug the current one can show.
  std::uint64_t want_bits = 0;
  const bool have_hardware = HardwareFor(seed, &want_bits);
  const std::string want =
      have_hardware ? "0x" + Hex64(want_bits) : "no row for seed " + std::to_string(seed);
  const std::string what = method + " seed=" + std::to_string(seed);
  const auto emit = [&](const std::string& label, const char* tier, const Outcome& got,
                        std::uint64_t compare_to, const std::string& want_text, bool comparable) {
    const bool good = comparable && got.ok() && got.bits == compare_to;
    ++(good ? pass : fail);
    Say(&out, "%s tier=%s reason=%s steps=%u value=0x%s hardware=%s %s", label.c_str(), tier,
        ReasonName(got.reason), got.steps, Hex64(got.bits).c_str(), want_text.c_str(),
        good ? "PASS" : "FAIL");
    if (!good) {
      if (!got.detail.empty()) Say(&out, "    stopped because: %s", got.detail.c_str());
      Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s r1=0x%s sp=0x%s d0=0x%s cpsr=0x%s",
          Hex(got.exit.pc).c_str(), Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(),
          Hex(got.exit.r[1]).c_str(), Hex(got.exit.r[13]).c_str(), Hex64(got.exit.d[0]).c_str(),
          Hex(got.exit.cpsr).c_str());
    }
  };

  // `JNI_OnLoad` first: it is the first thing a real Android runtime calls, and its answer is a constant
  // the engine either reaches or does not, with no math in the way.
  const std::string want_onload = "0x" + Hex64(static_cast<std::uint64_t>(kJniVersion16));
  for (auto* engine : {static_cast<exec::Executor*>(&interp), static_cast<exec::Executor*>(&jit)}) {
    const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
    RB_LOG("JNI_OnLoad on tier %s", tier);
    const Outcome got = Call(*engine, ctx, *app, "JNI_OnLoad", "II",
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0)});
    emit("JNI_OnLoad", tier, got, static_cast<std::uint64_t>(kJniVersion16), want_onload, true);
  }

  // Then the method, both tiers, against `kHardware`.
  for (auto* engine : {static_cast<exec::Executor*>(&interp), static_cast<exec::Executor*>(&jit)}) {
    const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
    RB_LOG("%s on tier %s", what.c_str(), tier);
    const Outcome got = Call(*engine, ctx, *app, m->symbol, m->shorty,
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(seed)});
    emit(what, tier, got, want_bits, want, have_hardware);
  }

  Say(&out, "summary: %u PASS, %u FAIL over %u calls", pass, fail, pass + fail);
  return out;
}

/**
 * Load one guest library the way `Launch` does and report what it *is*, rather than whether it passed.
 *
 * Why this is a second function and not a mode of `Launch`: `Launch` answers a question about one library
 * nobody has to identify — it looks up a fixed probe symbol, compares the result against a table measured
 * on a phone, and prints PASS/FAIL. A game chosen from the library screen is a library no one has
 * measured, so the honest report is its shape: is it an ELF32 ARM image, what does it need, which of its
 * imports did the guest scope answer and which were answered by the trap address that faults when called,
 * and — if it exports one — what does its `JNI_OnLoad` return through the emulator.
 *
 * The memory layout and resolver setup are deliberately duplicated from `Launch` rather than shared with
 * it. `Launch`'s path is verified on hardware (`docs/30` §Step 37); a helper extracted for both would put
 * that verification behind a refactor no device has run, to save twenty lines.
 *
 * `tiers`: 0 = interpreter then JIT, 1 = interpreter only, 2 = JIT only.
 *
 * `libc_image` and `libz_image` are the other two guest libraries this APK ships — a bionic `libc.so`
 * pulled off a 32-bit-capable device and a `libz.so` built from the NDK archive. Either may be empty (the
 * APK then carries no such asset), which is still a run this report has to make and name.
 */
std::string Inspect(const std::vector<std::uint8_t>& app_image,
                    const std::vector<std::uint8_t>& libm_image,
                    const std::vector<std::uint8_t>& libc_image,
                    const std::vector<std::uint8_t>& libz_image, const std::string& label,
                    int tiers) {
  std::string out;
  const unsigned page_shift = HostPageShift();
  Say(&out, "inspect: %s=%zu bytes libm.so=%zu libc.so=%zu libz.so=%zu bytes host_page_shift=%u",
      label.c_str(), app_image.size(), libm_image.size(), libc_image.size(), libz_image.size(),
      page_shift);
  if (app_image.empty()) {
    Say(&out, "FATAL: no library bytes arrived — this APK has no lib/armeabi-v7a entry, or it could not"
              " be read out of the zip");
    return out;
  }
  if (libm_image.empty()) {
    Say(&out, "FATAL: the guest libm asset is missing, and a guest library is linked against it");
    return out;
  }
  if (!loader::LooksLikeAArch32Elf(app_image.data(), app_image.size())) {
    Say(&out, "FATAL: %s is not an ELF32 ARM image. This is what RetroBug executes, so an arm64 or a"
              " dex-only APK has nothing here it can run.", label.c_str());
    return out;
  }

  std::string err;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = page_shift;
  std::unique_ptr<GuestMemory> mem = GuestMemory::Create(kWindowBytes, model, &err);
  if (!mem) {
    Say(&out, "FATAL: GuestMemory::Create(%zu) refused: %s", kWindowBytes, err.c_str());
    return out;
  }
  if (!mem->Map(kGuardPage, kGuestPage, kRead | kWrite, &err)) {
    Say(&out, "FATAL: mapping the stack-guard page failed: %s", err.c_str());
    return out;
  }
  const std::uint32_t guard = 0xdead'beefu;
  mem->Write(kGuardPage, &guard, 4);
  if (!mem->Map(kStackTop - kStackBytes, kStackBytes, kRead | kWrite, &err)) {
    Say(&out, "FATAL: mapping the guest stack failed: %s", err.c_str());
    return out;
  }

  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    return kUnresolved;
  };

  /**
   * The scope, built by hand from the images the APK ships rather than by `GuestLinker::Link`.
   *
   * The reason is in the image: the device's `libc.so` declares `DT_NEEDED: ld-android.so, libdl.so`,
   * and `LinkAt` refuses a needed soname with no provider image (`core/linker/guest_linker.cpp:183-185`).
   * RetroBug ships no image for either and will not fabricate one — those two sonames say what the app
   * does not import from us, not what it needs to load — so a `Link`-built scope never contains libc at
   * all, which is exactly the failure this change exists to remove.
   *
   * A module that does not load is a report line, not an exit: the run against a scope with no libc is
   * the run this launcher had before libc was shipped, and the trapped list below is exactly what the
   * missing library costs.
   */
  std::optional<loader::GuestModule> libc_mod, libm_mod, libz_mod;
  const auto load_scope = [&](const char* soname, std::optional<loader::GuestModule>& slot,
                              const std::vector<std::uint8_t>& image) {
    if (image.empty()) {
      Say(&out, "scope: %s not loaded — no image arrived for it (this APK has no such asset)", soname);
      return;
    }
    loader::LoadOptions opts;
    // The opt-in is what lets a real bionic `libc.so` load at all: it has TLS relocation slots this loader
    // cannot apply, and without the opt-in every one of them fails the load by name. What it tolerates is
    // not hidden — each module keeps its skipped slots in `unsupported_relocs`, printed below.
    opts.tolerate_unresolved_relocs = true;
    // By reference, and that is safe here where the file header's provider has to be by value: this
    // resolver is consulted only inside `LoadFromBytes`, which this lambda calls synchronously and does
    // not retain. It answers from what is already in the scope, which is what makes the load order below
    // dependency-first rather than a preference.
    opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
      for (const std::optional<loader::GuestModule>* placed : {&libc_mod, &libm_mod, &libz_mod}) {
        if (*placed) {
          if (const auto fn = (*placed)->FindExport(name)) return fn->tagged();
        }
      }
      return host(name);
    };
    std::string load_err;
    slot = loader::LoadFromBytes(*mem, image, soname, opts, &load_err);
    if (!slot) {
      Say(&out, "scope: %s not loaded: %s", soname, load_err.c_str());
      return;
    }
    std::size_t exports = 0;
    for (const loader::Symbol& sym : slot->exports) {
      if (sym.defined) ++exports;
    }
    Say(&out, "scope: %s at 0x%s extent 0x%s, %zu defined exports", soname, Hex(slot->base).c_str(),
        Hex(slot->extent).c_str(), exports);
    if (!slot->unsupported_relocs.empty()) {
      Say(&out, "scope: %s: %zu relocation(s) tolerated but not applied — those slots still hold file"
                " data, not addresses",
          soname, slot->unsupported_relocs.size());
    }
  };
  load_scope("libc.so", libc_mod, libc_image);
  load_scope("libm.so", libm_mod, libm_image);
  load_scope("libz.so", libz_mod, libz_image);

  /**
   * The app's imports, answered from the hand-built scope and nothing else. `FindExport` is the whole
   * lookup and its tagged address is what a GOT entry wants (`rb/loader/guest_elf.h`) — the same answer
   * `GuestLinker::Lookup` produced, with the linker no longer in the way. Only a name none of the three
   * libraries defines falls through to the trap address, and only that falls through.
   */
  std::vector<std::string> answered;
  std::vector<std::string> trapped;
  // One entry per imported name, not per relocation slot: the loader asks once per slot, and an import
  // can fill more than one, so a plain push would count the same import twice.
  const auto record = [](std::vector<std::string>& list, const std::string& name) {
    for (const std::string& seen : list) {
      if (seen == name) return;
    }
    list.push_back(name);
  };
  loader::LoadOptions app_opts;
  app_opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    for (const std::optional<loader::GuestModule>* mod : {&libc_mod, &libz_mod, &libm_mod}) {
      if (*mod) {
        if (const auto fn = (*mod)->FindExport(name)) {
          record(answered, name);
          return fn->tagged();
        }
      }
    }
    record(trapped, name);
    return host(name);
  };
  auto app_mod = loader::LoadFromBytes(*mem, app_image, label, app_opts, &err);
  if (!app_mod) {
    Say(&out, "FATAL: the loader refused %s: %s", label.c_str(), err.c_str());
    Say(&out, "  (a refusal names every slot it could not fill; a library built against a libc RetroBug"
              " does not ship yet is the expected shape of this)");
    return out;
  }
  const loader::GuestModule& app = *app_mod;
  std::size_t defined_exports = 0;
  for (const auto& sym : app.exports) {
    if (sym.defined) ++defined_exports;
  }
  Say(&out, "loaded: %s at 0x%s extent 0x%s, %zu defined exports, %zu recorded dynsym imports",
      app.soname.empty() ? label.c_str() : app.soname.c_str(), Hex(app.base).c_str(),
      Hex(app.extent).c_str(), defined_exports, app.imports.size());
  if (!app.needed.empty()) {
    std::string needs;
    for (const std::string& one : app.needed) needs += (needs.empty() ? "" : ", ") + one;
    Say(&out, "needs: %s", needs.c_str());
  }
  if (!app.unsupported_relocs.empty()) {
    std::string relocs;
    for (const std::string& one : app.unsupported_relocs) relocs += (relocs.empty() ? "" : ", ") + one;
    Say(&out, "refused relocations: %s", relocs.c_str());
  }
  abi::AbiContext ctx{*mem, kStackTop, kReturnAddress};
  exec::InterpExecutor interp{*mem, kReturnAddress, &ctx};
  exec::JitExecutor jit{*mem, kReturnAddress, &ctx};
  if (!jit.ok()) Say(&out, "tier J is unavailable before any guest code ran: %s", jit.error().c_str());

  /**
   * The thread pointer, before any guest constructor runs.
   *
   * `__get_tls()` in bionic is a read of the coprocessor register TPIDRURW — `MachineState::tp`, which
   * defaults to zero — and every per-thread thing a constructor touches goes through it. With it zero the
   * first TLS slot read is a read of a near-null address, and that is exactly what the run before this
   * one did: libc's own `DT_INIT_ARRAY[0]` stopped at `guest memory read at 0x14 refused` on
   * `ReadMemory32`, which is slot 5 of a null TLS block rather than anything libc did wrong.
   *
   * One page, read/write, at an address the layout above leaves free (the stack ends at `kStackTop`, the
   * guard page is at `kGuardPage`, the sentinel at `kReturnAddress`, and modules are placed upward from
   * 0x2000). The guest's own TLS layout lives inside it; nothing here interprets it.
   */
  constexpr exec::GuestAddr kTlsBase = 0x0040'0000u;
  if (mem->Map(kTlsBase, kGuestPage, kRead | kWrite, &err)) {
    ctx.state().tp = kTlsBase;
    Say(&out, "tls: thread pointer set to 0x%s (one read/write page, %u bytes), because bionic reaches"
              " its per-thread data through the coprocessor register this seeds",
        Hex(kTlsBase).c_str(), kGuestPage);
  } else {
    Say(&out, "tls: could not map a thread-pointer page at 0x%s: %s — a guest constructor that touches"
              " per-thread data will fault near address 0",
        Hex(kTlsBase).c_str(), err.c_str());
  }

  /**
   * The constructors, run here for the first time: the scope's in dependency order, then the app's own.
   *
   * `GuestLinker::RunInitializers` cannot do this one. It walks the linker's `order()`, and neither the
   * app nor this scope is in it — the app is placed with `LoadFromBytes` (which is why it never runs
   * through `Link`; see above) and the scope's modules are not linked at all — so the app's own
   * `DT_INIT_ARRAY` was recorded and never run. Each module's two tags are walked the way
   * `guest_linker.cpp:222-238` established: `DT_INIT_ARRAY` first, then `DT_INIT`, a zero entry skipped
   * rather than called. The app's come last because a constructor may call into the scope.
   *
   * One tier only, and that is a correctness rule rather than a speed one: a constructor has side effects
   * and this is one guest memory, so running the sequence on both tiers would run every constructor
   * twice. `tiers == 2` asks for the JIT, but a JIT that could not be built is reported above, and the
   * interpreter is then the only engine left that can run one.
   */
  exec::Executor& ctor_engine = (tiers == 2 && jit.ok()) ? static_cast<exec::Executor&>(jit)
                                                         : static_cast<exec::Executor&>(interp);
  const char* ctor_tier = ctor_engine.tier() == exec::CodeTier::Interp ? "I" : "J";
  ctx.set_executor(ctor_engine);
  // `CallGuest` hands the callee's last state back through its out-parameter, not through `ctx.state()`,
  // which the bridge restores to the caller's registers on the way out (`rb/abi/abi.h`).
  Outcome ctor_call;
  const auto run_one_ctor = [&](loader::CodeAddress fn) {
    ctor_call = Outcome{};
    exec::RunResult result{};
    const abi::Ret32 ret = ctx.CallGuest(fn.tagged(), {}, abi::Shorty::For(""), kCallBudget, &result,
                                         &ctor_call.exit);
    ctor_call.reason = result.reason;
    ctor_call.bits = ret.bits;
    ctor_call.steps = result.instructions_executed;
    ctor_call.detail = result.detail;
    return result.reason == exec::StopReason::Returned;
  };
  // One failure, with everything needed to find where it stopped. `what` names the entry — module, tag and
  // index — because "a constructor failed" without one is a report a reader cannot act on, and `detail` is
  // where a named fault (an unanswered import called at 0xbad0000, an unimplemented IR op) appears.
  const auto say_ctor_failure = [&](const std::string& what) {
    Say(&out, "constructors: %s did not return on tier=%s: %s", what.c_str(), ctor_tier,
        ReasonName(ctor_call.reason));
    if (!ctor_call.detail.empty()) Say(&out, "    stopped because: %s", ctor_call.detail.c_str());
    Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s r1=0x%s sp=0x%s cpsr=0x%s",
        Hex(ctor_call.exit.pc).c_str(), Hex(ctor_call.exit.r[14]).c_str(),
        Hex(ctor_call.exit.r[0]).c_str(), Hex(ctor_call.exit.r[1]).c_str(),
        Hex(ctor_call.exit.r[13]).c_str(), Hex(ctor_call.exit.cpsr).c_str());
  };
  // Stops the whole sequence at the first constructor that does not return: the rest of the scope's and
  // the app's constructors are not run after a failure, because a half-initialized module must not look
  // loaded.
  const auto run_ctors = [&](const char* soname, const std::optional<loader::GuestModule>& slot,
                             std::size_t* ran) -> bool {
    if (!slot) return true;
    for (std::size_t i = 0; i < slot->init_array.size(); ++i) {
      const loader::CodeAddress fn = slot->init_array[i];
      if (fn.addr == 0) continue;  // a gap some linkers leave in the array, not a function to call
      if (run_one_ctor(fn)) {
        ++*ran;
        continue;
      }
      say_ctor_failure(std::string{soname} + " DT_INIT_ARRAY[" + std::to_string(i) + "] at 0x" +
                       Hex(fn.addr));
      return false;
    }
    if (slot->init.addr != 0) {
      if (run_one_ctor(slot->init)) {
        ++*ran;
      } else {
        say_ctor_failure(std::string{soname} + " DT_INIT at 0x" + Hex(slot->init.addr));
        return false;
      }
    }
    return true;
  };
  std::size_t libc_ctors = 0, libm_ctors = 0, libz_ctors = 0, app_ctors = 0;
  const bool ctors_ok = run_ctors("libc.so", libc_mod, &libc_ctors) &&
                        run_ctors("libm.so", libm_mod, &libm_ctors) &&
                        run_ctors("libz.so", libz_mod, &libz_ctors) &&
                        run_ctors(label.c_str(), app_mod, &app_ctors);
  if (ctors_ok) {
    Say(&out, "constructors: %zu ran in total on tier=%s (libc.so %zu, libm.so %zu, libz.so %zu, %s %zu)",
        libc_ctors + libm_ctors + libz_ctors + app_ctors, ctor_tier, libc_ctors, libm_ctors, libz_ctors,
        label.c_str(), app_ctors);
  }
  // The app's own tags are the point of running these by hand, so "it had none" is its own line rather
  // than a zero in the tuple above.
  if (app_ctors == 0 && app.init_array.empty() && app.init.addr == 0) {
    Say(&out, "constructors: %s carries no DT_INIT_ARRAY and no DT_INIT — no startup work of its own",
        label.c_str());
  }

  // The whole scope's effect in one line: how many of the names the app's relocations asked for were
  // answered from a guest library, and how many are still bound to the unmapped trap address. The counts
  // come from the resolver's own log rather than from `app.imports`, which holds only dynsym entries whose
  // type the loader recognises — this image's three are `NOTYPE`, so it reads 0 beside three real slots.
  Say(&out, "imports: %zu of %zu answered by the guest scope, %zu still trapped at 0x%s",
      answered.size(), answered.size() + trapped.size(), trapped.size(), Hex(kUnresolved).c_str());

  // One place that writes the trap list, so the exits below cannot drift apart. `Say` is variadic and
  // takes a `const char*`, so the note is built as a string first: a ternary mixing `""` with a
  // concatenation hands the variadic call a `std::string` and clang refuses it.
  const auto say_trapped = [&]() {
    if (trapped.empty()) {
      Say(&out, "imports the guest scope did not answer: none");
      return;
    }
    Say(&out, "imports the guest scope did not answer: %zu (a call into one faults at 0x%s)",
        trapped.size(), Hex(kUnresolved).c_str());
    for (std::size_t i = 0; i < trapped.size() && i < 12; ++i) Say(&out, "  %s", trapped[i].c_str());
    if (trapped.size() > 12) Say(&out, "  ... and %zu more", trapped.size() - 12);
  };

  // A constructor that did not return leaves the module half-initialized, so its entry points are not
  // called: a real runtime would not be entering this library either. Named rather than silent, because
  // "why is there no JNI_OnLoad line" is the first question a reader of this report will have.
  if (!ctors_ok) {
    Say(&out, "JNI_OnLoad: not called — a constructor above did not return, so the library is not in a"
              " state a runtime would enter");
    say_trapped();
    return out;
  }

  const auto onload = app.FindExport("JNI_OnLoad");
  if (!onload) {
    /**
     * No `JNI_OnLoad` is not the same as nothing to call, and the first version of this report said it
     * was. The Angry Birds library on the test phone exports seven `Java_*` functions and no `JNI_OnLoad`
     * at all — that is how an app of that era is built: the Java side calls into the library through JNI,
     * so the library has no reason to notice being loaded. Saying "no entry point" about it was wrong in
     * the one place a reader would take it as final.
     */
    std::vector<std::string> java_entries;
    for (const std::string& name : app.ExportNames()) {
      if (name.compare(0, 5, "Java_") == 0) java_entries.push_back(name);
    }
    if (java_entries.empty()) {
      Say(&out, "JNI_OnLoad: not exported, and no Java_* entry point either — there is nothing here for a"
                " Java runtime to call. Loading it is where this build stops.");
    } else {
      Say(&out, "JNI_OnLoad: not exported. This library exports %zu Java_* entry points instead, which is"
                " how an app of its era is driven — the Java side calls in through JNI.",
          java_entries.size());
      Say(&out, "  RetroBug cannot reach them yet: each takes a JNIEnv* and a jobject that only a Java"
                " runtime can supply, and this build runs a guest's native code, not its DEX.");
      for (std::size_t i = 0; i < java_entries.size() && i < 8; ++i) {
        Say(&out, "  %s", java_entries[i].c_str());
      }
      if (java_entries.size() > 8) Say(&out, "  ... and %zu more", java_entries.size() - 8);
    }
    say_trapped();
    return out;
  }

  Say(&out, "JNI_OnLoad: exported (0x%s). Calling it with env=0 clazz=0 — a real JavaVM is not something"
            " this host has, and a library that dereferences the null will fault *inside the emulator*,",
      Hex(static_cast<std::uint32_t>(onload->addr)).c_str());
  Say(&out, "  which is reported here as a Fault rather than crashing this process.");
  const auto call_one = [&](exec::Executor& engine, const char* tier) {
    const Outcome got = Call(engine, ctx, app, "JNI_OnLoad", "II",
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0)});
    Say(&out, "JNI_OnLoad tier=%s reason=%s steps=%u value=0x%s", tier, ReasonName(got.reason), got.steps,
        Hex64(got.bits).c_str());
    if (got.reason != exec::StopReason::Returned && !got.detail.empty()) {
      Say(&out, "    stopped because: %s", got.detail.c_str());
    }
    Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s sp=0x%s cpsr=0x%s",
        Hex(got.exit.pc).c_str(), Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(),
        Hex(got.exit.r[13]).c_str(), Hex(got.exit.cpsr).c_str());
  };
  if (tiers == 0 || tiers == 1) call_one(interp, "I");
  if ((tiers == 0 || tiers == 2) && jit.ok()) call_one(jit, "J");
  say_trapped();
  return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeInspect(JNIEnv* env, jclass /*clazz*/,
                                                          jbyteArray guest_so, jbyteArray libm_so,
                                                          jbyteArray libc_so, jbyteArray libz_so,
                                                          jstring label, jint tiers) {
  std::string name;
  if (label != nullptr) {
    const char* chars = env->GetStringUTFChars(label, nullptr);
    if (chars != nullptr) {
      name = chars;
      env->ReleaseStringUTFChars(label, chars);
    }
  }
  if (name.empty()) name = "guest.so";
  const std::vector<std::uint8_t> app_image = Bytes(env, guest_so);
  const std::vector<std::uint8_t> libm_image = Bytes(env, libm_so);
  const std::vector<std::uint8_t> libc_image = Bytes(env, libc_so);
  const std::vector<std::uint8_t> libz_image = Bytes(env, libz_so);
  std::string report;
  try {
    report = Inspect(app_image, libm_image, libc_image, libz_image, name, static_cast<int>(tiers));
  } catch (const std::exception& e) {
    report = std::string{"FATAL: unhandled exception while inspecting: "} + e.what();
  }
  return env->NewStringUTF(report.c_str());
}

// The self-test lane loads the probe through `Launch`, which links only libm; libc and libz are in this
// signature so the one APK has one asset set and the Java half hands it over uniformly. Unused on purpose:
// `Launch`'s PASS/FAIL table was measured against a libm-only scope and is not this change's to move.
extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeLaunch(JNIEnv* env, jclass /*clazz*/,
                                                         jbyteArray guest_so, jbyteArray libm_so,
                                                         jbyteArray /*libc_so*/, jbyteArray /*libz_so*/,
                                                         jstring method, jint seed) {
  std::string name;
  if (method != nullptr) {
    const char* chars = env->GetStringUTFChars(method, nullptr);
    if (chars != nullptr) {
      name = chars;
      env->ReleaseStringUTFChars(method, chars);
    }
  }
  const std::vector<std::uint8_t> app_image = Bytes(env, guest_so);
  const std::vector<std::uint8_t> libm_image = Bytes(env, libm_so);

  std::string report;
  try {
    report = Launch(app_image, libm_image, name, static_cast<std::int32_t>(seed));
  } catch (const std::exception& e) {
    // Every failure the engine itself can produce reaches this file as a return value — a null
    // `GuestMemory`, an empty `optional` plus `err`, a `StopReason` in the `RunResult` — so an exception
    // here is the host failing to allocate: the 64 MiB window, or a vector the size of a library. Caught
    // because an unhandled one crossing JNI kills the process and this launcher exists to produce a
    // report; a run that stops saying things is the failure mode it cannot afford.
    report = std::string{"FATAL: unhandled exception in the launcher: "} + e.what();
  }
  return env->NewStringUTF(report.c_str());
}
