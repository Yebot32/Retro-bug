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
 *
 * The one set of imports that is *not* left to the trap is `gl*`: `libGLESv1_CM.so` is an API this host
 * could forward to one day and nothing here implements it today, so those names get counted host slots
 * instead — see the block in `Inspect`. Trapping them would name nothing and answer nothing; a counted
 * no-op that says which entry point was called, and how often, is the honest form of "not yet".
 */
#include <jni.h>

#include <android/log.h>
#include <fcntl.h>

#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/abi/jni_env.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/linker/guest_linker.h"
#include "rb/syscall/monitor.h"
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
///
/// The stack is the one figure here that the probe rig did not need to get right and a real library does:
/// **8 MiB just below the window's top (55–63 MiB)**, where Android gives a main thread 8 MiB, instead of
/// the 64 KiB this was. 64 KiB was obviously enough for the probe's handful of frames and is nothing like
/// enough for bionic: its `__libc_preinit` walked 65,640 bytes down it and the next store landed 104 bytes
/// past the bottom, which reads as `guest memory write at 0xeff90 refused` with `sp = 0xeff98` — an address
/// that sends a reader hunting a bug in libc when the bug is this constant. It sits at the top of the
/// window because the crowded part is the *bottom*: `kGuardPage` at 8 MiB, the sentinel at 12 MiB, TLS at
/// 4 MiB, the loader stub and JNI environment above that, the GL strings at 5 MiB, and images placed
/// upward from 0x2000.
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x03f0'0000u;
constexpr exec::GuestAddr kGuardPage = 0x0080'0000u;
constexpr exec::GuestAddr kStackBytes = 32u << 20;
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

/// Where the host's answers to a guest's `gl*` calls live: one page, mapped before any module is placed,
/// in the same run of host pages as the loader stub. Its whole content is the pair of strings
/// `glGetString` hands back, and that is the reason it exists — a game parses that result, so a defined
/// string is the difference between "this host has no GL" and a null dereference that reads as a bug in
/// the game.
constexpr exec::GuestAddr kGlStrings = 0x0043'0000u;
/// `"RetroBug (no GL)"` is sixteen bytes and a NUL, so the empty answer starts at the next word. The
/// assert is why the two are joined by a constant rather than placed by hand: a longer identity string
/// would silently overwrite the empty one, and the only thing that would ever notice is a guest parsing
/// the result it was handed.
constexpr unsigned kGlEmptyOffset = 20u;
constexpr exec::GuestAddr kGlEmpty = kGlStrings + kGlEmptyOffset;
static_assert(sizeof("RetroBug (no GL)") <= kGlEmptyOffset,
              "the identity string would run into the empty one");

/// `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, transcribed from the NDK's own `GLES/gl.h`
/// (`sysroot/usr/include/GLES/gl.h:303-305`, with `GL_EXTENSIONS` at 306) rather than recalled: these are
/// the enums a game parses to decide what it is running on, so a wrong number here is a wrong answer that
/// no fault would ever name.
constexpr std::uint32_t kGlVendor = 0x1f00u;
constexpr std::uint32_t kGlRenderer = 0x1f01u;
constexpr std::uint32_t kGlVersion = 0x1f02u;

/// The host-slot ids the GL answers take — above bionic's JNI table, which `abi::JniEnv` fills from 0, so
/// the two answer tables cannot alias each other, and below `AbiContext::kMaxSlots`, past which no
/// address `SlotFor` produces maps back to its slot. `kGlSlots` bounds how many names one run may bind;
/// the set is refused rather than wrapped when it runs out.
constexpr std::uint32_t kGlSlotBase = 256u;
constexpr std::size_t kGlSlots = 64;
static_assert(kGlSlotBase >= abi::jni::kTableEntries &&
                  kGlSlotBase + kGlSlots <= abi::AbiContext::kMaxSlots,
              "a GL slot would either alias the JNI table or be unreachable past kMaxSlots");

/// Whether a name is one of the OpenGL ES client entry points the host answers. The set is deliberately
/// not a list: which entry points a run needs is whatever the app's own relocations ask for, and a list
/// written here would be a second inventory of `libGLESv1_CM.so` to keep in step with the first.
bool IsGlName(const std::string& name) {
  return name.size() > 2 && name.compare(0, 2, "gl") == 0;
}

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
  /**
   * Live, as well as accumulated.
   *
   * The report is returned to Java as one string and logged when the run finishes, which is fine until a
   * run does not finish: a constructor sequence through the interpreter, or a guest that loops, produces
   * *nothing* in logcat until it is over — and the reader is left deciding whether the app is thinking or
   * dead, with a screenshot that shows the previous screen. Every line is therefore also written to logcat
   * the moment it is produced. The Java half logs the finished report again; the duplication is the price
   * of being able to watch a slow run, and it is the difference between "still running, here is how far it
   * got" and silence.
   */
  RB_LOG("%s", buf);
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

/**
 * One host file, whole — the bytes the guest's `openat` of the game's own APK is answered with.
 *
 * A read loop with `errno` rather than `std::ifstream`, because the failure has to be nameable. The
 * monitor answers a path its provider does not serve with `ENOENT`, and an `ENOENT` about a file that is
 * sitting right there sends a reader hunting a wrong path; here the reason comes back as text
 * (`open(...): Permission denied`) and the caller puts it in the report beside the path it tried. The
 * APK this is pointed at is one the launcher copied into its own storage, so the failure this can
 * actually meet is not a missing file — which is exactly why the reason has to survive.
 *
 * Path handling deliberately stops at the caller: this function opens whichever single string it is
 * given, and every "which paths may a guest see" decision is made by the provider that captures its
 * result — see the one call site in `Inspect`.
 */
std::optional<std::vector<std::uint8_t>> ReadHostFile(const std::string& path, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != nullptr) *err = "open(" + path + "): " + std::strerror(errno);
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[1 << 16];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (err != nullptr) *err = "read(" + path + "): " + std::strerror(errno);
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    bytes.insert(bytes.end(), buf, buf + n);
  }
  ::close(fd);
  return bytes;
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
 *
 * `apk_path` is the chosen APK as the host filesystem names it (`File.getAbsolutePath()` on the Java
 * side), and it has two jobs: it becomes the NUL-terminated guest string the entry point is handed,
 * because the game opens that file to read the `assets/` it ships, and it is the one path this run's
 * syscall monitor will serve bytes for. `surface_width`/`surface_height` are the Activity's display in
 * pixels — the numbers a `GLSurfaceView.Renderer` would have reported to the game — supplied here on the
 * Java side's behalf; the report says they were given rather than measured by the game.
 */
std::string Inspect(const std::vector<std::uint8_t>& app_image,
                    const std::vector<std::uint8_t>& libm_image,
                    const std::vector<std::uint8_t>& libc_image,
                    const std::vector<std::uint8_t>& libz_image, const std::string& label,
                    int tiers, const std::string& apk_path, int surface_width,
                    int surface_height) {
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

  /**
   * The two guest pages the entry point's arguments live in, mapped here rather than at that call.
   *
   * Why here: the loader places every module in the first free run of the window it can find, so a page
   * mapped after the loads could be a page a module already sits on — `Map` replaces, and what would
   * disappear is the module's own text. Two pages, before anything is placed, cost nothing and take the
   * ordering question away; the JNIEnv below is built early for the same reason.
   *
   * What: a zeroed page to stand in for the `MyRenderer` object the entry point receives twice — as
   * `this` and as its first declared parameter — and the APK's absolute path as the NUL-terminated guest
   * string the Java side would have passed. Neither is a Java object: there is no JavaVM here, and the
   * call's own lines say what the addresses are.
   */
  constexpr exec::GuestAddr kStandInObject = 0x0044'0000u;
  constexpr exec::GuestAddr kApkPathAt = 0x0045'0000u;
  exec::GuestAddr stand_in = 0;
  exec::GuestAddr apk_path_guest = 0;
  if (mem->Map(kStandInObject, kGuestPage, kRead | kWrite, &err)) {
    stand_in = kStandInObject;
  } else {
    Say(&out, "entry: the stand-in object page at 0x%s could not be mapped (%s) — the entry point is"
              " still called, with 0 where that object should be",
        Hex(kStandInObject).c_str(), err.c_str());
  }
  if (apk_path.empty()) {
    Say(&out, "entry: the Java half passed no APK path, so the library's path argument is 0 and it has no"
              " way to reach the assets inside its own APK");
  } else if (apk_path.size() + 1 > kGuestPage) {
    Say(&out, "entry: the APK path is %zu bytes and does not fit this launcher's %u-byte path page — the"
              " path argument is 0",
        apk_path.size(), kGuestPage);
  } else if (!mem->Map(kApkPathAt, kGuestPage, kRead | kWrite, &err)) {
    Say(&out, "entry: the APK path page at 0x%s could not be mapped (%s) — the path argument is 0",
        Hex(kApkPathAt).c_str(), err.c_str());
  } else if (!mem->Write(kApkPathAt, apk_path.c_str(), apk_path.size() + 1)) {
    Say(&out, "entry: guest memory at 0x%s refused the APK path — the path argument is 0",
        Hex(kApkPathAt).c_str());
  } else {
    apk_path_guest = kApkPathAt;
  }

  /**
   * One function that belongs to the *dynamic linker*, answered with a synthesized stub.
   *
   * bionic's `__libc_preinit_impl` calls `__loader_shared_globals` and then writes into the struct it
   * returns (at +0x438 — read off the constructor's disassembly, cross-referenced to the GOT slot at
   * 0xd6b98 and its `R_ARM_JUMP_SLOT`). It is an import of `libc.so` rather than a definition, because
   * the real implementation lives in the linker, which no image here provides. So the answer is a page of
   * zeroed guest memory plus twelve bytes of ARM that returns its address:
   *
   *     ldr r0, [pc, #0]    @ the word below, which sits at pc+8
   *     bx  lr
   *     .word <globals page>
   *
   * That is the function's contract — "return the loader's shared globals" — implemented rather than
   * deferred, and the report says the loader here is RetroBug's stand-in so nobody reads it as Android's.
   * The alternative was leaving it trapped, which is what the previous run did: the call executed the
   * trap page and the constructor stopped with `NoExecuteFault` at an address that names nothing.
   *
   * Only this one, on purpose. Every other linker-provided name (`android_dlopen_ext`, `pthread_atfork`,
   * `dladdr`) stays trapped, so the next one the guest reaches names itself in the report instead of
   * being guessed at now.
   */
  constexpr exec::GuestAddr kLoaderGlobals = 0x0041'0000u;
  constexpr exec::GuestAddr kLoaderStub = 0x0042'0000u;
  std::uint32_t loader_shared_globals = kUnresolved;
  if (mem->Map(kLoaderGlobals, kGuestPage, kRead | kWrite, &err) &&
      mem->Map(kLoaderStub, kGuestPage, kRead | kWrite | kExec, &err)) {
    const std::uint32_t code[] = {0xe59f'0000u, 0xe12f'ff1eu, kLoaderGlobals};
    if (mem->Write(kLoaderStub, code, sizeof code)) {
      // ARM state on purpose: bit 0 stays clear, so a Thumb `blx` switches state on the way in and the
      // `bx lr` above returns to the caller's Thumb address — the mechanism `docs/30` §Step 25 records.
      loader_shared_globals = kLoaderStub;
      Say(&out, "host: __loader_shared_globals -> ARM stub at 0x%s returning the zeroed page at 0x%s"
                " (the real one lives in the linker, which no image here provides)",
          Hex(kLoaderStub).c_str(), Hex(kLoaderGlobals).c_str());
    }
  }
  if (loader_shared_globals == kUnresolved) {
    Say(&out, "host: __loader_shared_globals could not be stubbed (%s) — the first constructor that calls"
              " it will fault at 0x%s",
        err.c_str(), Hex(kUnresolved).c_str());
  }

  // The bridge, built here rather than beside the executors below, because the answer to a `gl*` import
  // is a host slot and only the bridge can hand one out. It has no executor yet, which is the ordering
  // `rb/abi/abi.h` describes: the bridge *is* the table the executor consults, so the executor takes it.
  abi::AbiContext ctx{*mem, kStackTop, kReturnAddress};

  /**
   * The host-answered OpenGL ES 1.x set.
   *
   * Why this exists at all: 53 of the Angry Birds 1.0.0 library's 182 imports are `gl*`, and that list was
   * the *entire* trapped list of the run before this one — the client library that would define them,
   * `libGLESv1_CM.so`, is in no image here and is not being fabricated. Left alone they land on
   * `kUnresolved`, so the first one the game calls faults at an address that names nothing, which is the
   * worst of both: no answer and no name. This answers them instead, and the answer is *visible*: one host
   * slot per name the run asks for, a tally per name, and report lines that say exactly what happened.
   * That is the sanctioned shape of "not implemented" here — counted, named, and explained — rather than
   * the silent zero the project forbids.
   *
   * What a call does: nothing but count, and leave r0 at 0. For a `void` function that is invisible, and
   * for the rest it is a defined value a caller can store; for `glGetError` a 0 is `GL_NO_ERROR`, the
   * answer a working driver gives. `glGetString` is the exception that has to be answered with bytes,
   * because a game parses its result and a null would crash it in a way that looks like a game bug — see
   * the two strings above.
   */
  std::string gl_why;
  const auto gl_ready = [&]() -> bool {
    if (!mem->Map(kGlStrings, kGuestPage, kRead | kWrite, &err)) {
      gl_why = "the page could not be mapped: " + err;
      return false;
    }
    if (!mem->Write(kGlStrings, "RetroBug (no GL)", sizeof("RetroBug (no GL)")) ||
        !mem->Write(kGlEmpty, "", 1)) {
      gl_why = "the mapped page refused the writes";
      return false;
    }
    return true;
  }();
  if (!gl_ready) {
    Say(&out, "gl: the strings glGetString must return could not be placed at 0x%s (%s) — the gl* imports"
              " stay trapped at 0x%s, which faults the moment one is called",
        Hex(kGlStrings).c_str(), gl_why.c_str(), Hex(kUnresolved).c_str());
  }
  struct GlEntry {
    std::string name;
    unsigned calls = 0;
  };
  std::vector<GlEntry> gl_calls;
  /// Hands one `gl*` name a host slot, allocating on first sight and reusing it after. `nullopt` is a
  /// refusal — no strings to point at, or the set is full — and the caller turns that into the trap
  /// address, so a GL answer is never a slot the executor would not recognise.
  const auto gl_slot_for = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (!gl_ready) return std::nullopt;
    for (std::size_t i = 0; i < gl_calls.size(); ++i) {
      if (gl_calls[i].name == name) return ctx.SlotFor(kGlSlotBase + static_cast<std::uint32_t>(i));
    }
    if (gl_calls.size() >= kGlSlots) return std::nullopt;
    const std::uint32_t slot = kGlSlotBase + static_cast<std::uint32_t>(gl_calls.size());
    const std::size_t index = gl_calls.size();
    gl_calls.push_back(GlEntry{name, 0});
    // The name is captured by value and the counter is reached by index, so a later `push_back` moving
    // the vector cannot leave this closure counting into freed memory.
    ctx.Bind(slot, [&gl_calls, index, name](exec::MachineState& s, std::string*) {
      ++gl_calls[index].calls;
      const bool wants_a_string = name == "glGetString";
      const bool parsed_by_the_game = s.r[0] == kGlVendor || s.r[0] == kGlRenderer || s.r[0] == kGlVersion;
      s.r[0] = wants_a_string ? (parsed_by_the_game ? kGlStrings : kGlEmpty) : 0u;
      return true;
    });
    return ctx.SlotFor(slot);
  };

  /**
   * The guest-callable `JNIEnv`, from `rb/abi/jni_env.h` — the machinery the JNI seam already has, used
   * here for its second caller rather than reimplemented.
   *
   * It is built before anything is loaded for one reason: it maps guest pages of its own, and the
   * loader's placement scan takes the first free run it finds, so a region mapped afterwards could be
   * occupied by an image. Built now, every module is placed around it instead.
   *
   * The table is guest *data*, not guest code: `jni_env.h` fills it with `AbiContext` slots, which the
   * executor recognises before it translates anything, so no page of the guest window has to hold host
   * instructions. The GL set above reaches the same slot mechanism independently; what both need is a
   * guest that calls through them, and for the JNIEnv that caller is the entry point below.
   */
  constexpr exec::GuestAddr kJniEnvRegion = 0x0050'0000u;
  constexpr unsigned kJniEnvArena = 4u << 10;
  std::string jni_err;
  abi::JniEnv jni{*mem, ctx, kJniEnvRegion, kJniEnvArena, &jni_err};
  // The value the entry point gets in r0: a real fake environment when the region above was granted, and
  // a null when it was not, which is the only other honest answer — the report says which one it was.
  const abi::GuestAddr env_ptr = jni.ok() ? jni.env() : 0;
  if (jni.ok()) {
    Say(&out, "env: a fake JNIEnv at 0x%s — %u table entries of host-call slots (0..%u) and a %u-byte"
              " string arena, all guest memory. It is RetroBug's, not a JavaVM's: a guest that"
              " dereferences it reads a real table, and an entry with no implementation refuses by name"
              " instead of branching to address 0.",
        Hex(env_ptr).c_str(), abi::jni::kTableEntries, abi::jni::kTableEntries - 1u, kJniEnvArena);
  } else {
    Say(&out, "env: the fake JNIEnv could not be built (%s) — a guest JNI method is called with env=0, so"
              " one that reads its table faults near address 0 rather than in a table this run could have"
              " provided",
        jni_err.c_str());
  }

  /**
   * The syscall monitor — the piece of the OS a guest `svc` instruction reaches.
   *
   * Without one, the first `svc` a guest libc makes stops the run with `no syscall monitor attached`,
   * which is what bionic's own preinit did on this phone the first time it ran far enough to ask the
   * kernel for anything. With one attached, each call is either *answered* — the monitor knows the call
   * and has the fact — or *refused by name*, and its tally says how many of each, which is the difference
   * between "the guest asked for nothing" and "the guest asked and nobody answered".
   *
   * Two setters and one path. `set_memory` is what lets a pointer-shaped call copy a path out of guest
   * memory or a `stat64` into it; without it every such call is a refusal rather than a write to address 0.
   * `set_ids` gives the guest its own pid and tid, because nothing in this path may ask the host kernel
   * for one and an unset id is refused rather than invented.
   *
   * The file provider is why this run is not the one before it. The entry point below is handed the APK's
   * path because the game opens that file to read its `assets/` — levels, textures, whatever the APK
   * carries — and a monitor with no provider answers that open with `no file provider is installed`,
   * which is where the game's first asset read stops. The provider answers that one path and nothing else:
   * every other path still comes back to the guest as a named `ENOENT`, and it cannot do otherwise,
   * because it compares against the single string captured here. "Serve this one file" therefore stays
   * distinguishable from "serve the host's filesystem", which is the decision this monitor's design asks
   * to be made per path.
   */
  syscall::Monitor monitor;
  monitor.set_memory(mem.get());
  monitor.set_ids(4242, 4243);
  // Read once, here, rather than inside the provider: the provider's contract returns the bytes by value
  // and the monitor keeps its own copy per open, so a read on each call would be the same bytes read
  // twice — and a failure has to become a line in *this* report rather than an ENOENT about a file that
  // is sitting right there.
  std::optional<std::vector<std::uint8_t>> apk_file;
  if (apk_path.empty()) {
    Say(&out, "file: no APK path arrived from the Java half, so the provider below has nothing to serve"
              " and every openat is still answered by name — ENOENT from the provider, carrying the"
              " path — rather than served from the host");
  } else {
    std::string read_err;
    apk_file = ReadHostFile(apk_path, &read_err);
    if (apk_file) {
      Say(&out, "file: %s (%zu bytes on the host) is the one path this run serves — the path the entry"
                " point is handed and the guest's openat of it are the same file",
          apk_path.c_str(), apk_file->size());
    } else {
      Say(&out, "file: %s could not be read (%s) — the provider is still attached but has nothing to"
                " answer with, so the guest's openat of it comes back ENOENT and this line says why",
          apk_path.c_str(), read_err.c_str());
    }
  }
  // Captured by reference, and here that is safe: the provider is consulted only from inside a guest call
  // this function is making — the monitor never invokes it on its own lifetime — and `apk_file` is alive
  // for every one of those calls. The by-value capture the file header insists on is for `GuestLinker`'s
  // provider, which is stored beyond the call that made it; this one is not. The path is captured by value
  // so the comparison does not depend on the caller's string outliving the monitor.
  monitor.set_file_provider([&apk_file, apk_path](const std::string& path)
                                -> std::optional<std::vector<std::uint8_t>> {
    if (path != apk_path || !apk_file) return std::nullopt;
    return apk_file;
  });
  ctx.set_monitor(&monitor);

  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    if (name == "__loader_shared_globals") return loader_shared_globals;
    if (IsGlName(name)) {
      if (const auto slot = gl_slot_for(name)) return slot;
    }
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
  /// The `gl*` names the app's relocations asked for and the host answered with a slot. Kept apart from
  /// `answered` on purpose: "the guest scope answered it" and "the host bound it to a counted no-op" are
  /// different facts, and the imports line below is a claim about the first one.
  std::vector<std::string> gl_answered;
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
    const std::optional<std::uint32_t> from_host = host(name);
    // The bucket has to be the answer the name actually got, not the question that was asked: a `gl*`
    // name is in the host-answered set only when a slot really came back, so a refused slot set still
    // reports those names as trapped rather than claiming an answer nobody can call.
    if (IsGlName(name) && from_host && *from_host != kUnresolved) {
      record(gl_answered, name);
    } else {
      record(trapped, name);
    }
    return from_host;
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
  constexpr exec::GuestAddr kThreadStruct = 0x0043'0000u;
  if (mem->Map(kTlsBase, kGuestPage, kRead | kWrite, &err) &&
      mem->Map(kThreadStruct, kGuestPage, kRead | kWrite, &err)) {
    ctx.state().tp = kTlsBase;
    /*
     * The thread struct, which is what the blank page above was missing.
     *
     * bionic's `__errno()` is `tls = TPIDRURW; return *(void**)(tls + 4) + 0x29c` — slot 1 of the TLS
     * block is `TLS_SLOT_THREAD_ID`, a pointer to the thread's `pthread_internal_t`, and errno lives
     * inside *that*, not inside the TLS block. With slot 1 left blank the arithmetic lands on 0x29c and
     * the first write faults, which is exactly what this run did before the fix: libc's own constructor
     * stopped at `guest memory write at 0x29c refused`, three frames below `mprotect`'s error path, in
     * `__set_errno_internal`.
     *
     * On a real device the *dynamic linker* allocates this and installs it during main-thread setup. This
     * image has no such initialiser — `llvm-objdump` over the whole `.text` finds no write to TPIDRURW,
     * and the one function that could do it (`__set_tls`) has no callers in the image — so the launcher
     * plays the linker's part with two stores. Slot 0 is set to the block's own address, the convention
     * bionic's ARM `__get_tls` assumes; `tid` matches the identity the syscall monitor hands the guest, so
     * `gettid()`'s cached copy reads the same number the syscalls do.
     */
    const std::uint32_t thread_self = kTlsBase;
    const std::uint32_t thread_struct = kThreadStruct;
    const std::uint32_t tid = 4243;
    if (!mem->Write(kTlsBase, &thread_self, 4) || !mem->Write(kTlsBase + 4, &thread_struct, 4) ||
        !mem->Write(kThreadStruct + 8, &tid, 4)) {
      Say(&out, "tls: the thread block could not be written — bionic's __errno() will compute a null"
                " thread struct and fault near 0x29c");
    }
    Say(&out, "tls: thread pointer set to 0x%s; TLS_SLOT_THREAD_ID (TP+4) -> 0x%s, a zeroed writable page"
              " whose +0x29c is the errno __errno() computes and whose +8 is tid %u. The linker would"
              " allocate these; this image has no initialiser, so the launcher does",
        Hex(kTlsBase).c_str(), Hex(kThreadStruct).c_str(), tid);
  } else {
    Say(&out, "tls: could not map the thread-pointer block (0x%s/0x%s): %s — a guest constructor that"
              " touches per-thread data will fault near address 0",
        Hex(kTlsBase).c_str(), Hex(kThreadStruct).c_str(), err.c_str());
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
  // answered from a guest library, how many went to the host's counted GL set, and how many are still
  // bound to the unmapped trap address. The counts come from the resolver's own log rather than from
  // `app.imports`, which holds only dynsym entries whose type the loader recognises — this image's three
  // are `NOTYPE`, so it reads 0 beside three real slots. The three buckets are the app's whole import list
  // and they are separate on purpose: "the guest scope answered it" and "the host bound it to a counted
  // no-op" are different facts, and the second one gets its own lines at the exit.
  Say(&out, "imports: %zu of %zu answered by the guest scope, %zu by the host's GL set (below), %zu still"
            " trapped at 0x%s",
      answered.size(), answered.size() + gl_answered.size() + trapped.size(), gl_answered.size(),
      trapped.size(), Hex(kUnresolved).c_str());

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

  /**
   * One place that writes the GL tally, at every exit, for the same reason `say_trapped` exists.
   *
   * The list is of the names this run *bound*, each with its call count, rather than of the names that
   * were called: for the work ahead — forwarding these entry points to a host driver one day — which
   * entry points the app asks for is the useful fact, and a count of 0 says "bound and not reached yet"
   * where an omission would say nothing. The two are distinguishable at a glance and that matters here,
   * because a run that stops early and one that never bound the set produce the same silence otherwise.
   */
  const auto say_gl = [&]() {
    if (gl_calls.empty()) {
      Say(&out, "gl: no gl* import was asked of the host in this run, so the host-answered GL set is empty"
                " (nothing was answered with GL semantics, and nothing needed to be)");
      return;
    }
    unsigned called = 0, calls = 0;
    bool get_string = false;
    for (const GlEntry& e : gl_calls) {
      if (e.calls != 0) ++called;
      calls += e.calls;
      if (e.name == "glGetString") get_string = true;
    }
    Say(&out, "gl: %zu gl* imports were answered by host slots %u..%u rather than by the trap address — no"
              " image here implements GLES1. RetroBug implements no GL: every one of these returns a"
              " defined zero (glGetError's is GL_NO_ERROR) and draws nothing. Counted, named, and reported"
              " here rather than silently ignored.",
        gl_calls.size(), kGlSlotBase,
        kGlSlotBase + static_cast<std::uint32_t>(gl_calls.size()) - 1u);
    if (called == 0) {
      Say(&out, "gl: none of them has been called by this run yet — the set is bound, so the first call"
                " that does arrive is counted and answered instead of faulting at 0x%s. What the app asks"
                " for, each with the number of calls reached:",
          Hex(kUnresolved).c_str());
    } else {
      Say(&out, "gl: %u of them have been called, %u calls in total. What the app asks for, each with the"
                " number of calls reached:",
          called, calls);
    }
    std::size_t shown = 0;
    for (const GlEntry& e : gl_calls) {
      if (shown == 12) {
        Say(&out, "  ... and %zu more, whose calls are in the total above", gl_calls.size() - shown);
        break;
      }
      Say(&out, "  %s x%u", e.name.c_str(), e.calls);
      ++shown;
    }
    if (get_string) {
      Say(&out, "gl: glGetString returns fabrications standing in for a driver — GL_VENDOR, GL_RENDERER and"
                " GL_VERSION read \"RetroBug (no GL)\" out of guest memory, and GL_EXTENSIONS and every"
                " other enum read the empty string. A game parses that answer, so it is bytes rather than"
                " a null; nothing behind them renders.");
    }
  };

  /// One place that writes the syscall tally, at every exit, for the reason the other two emitters exist:
  /// what the guest asked the OS for is the next thing a reader needs, and a run that stopped before any
  /// guest code ran has to say that rather than print an empty line.
  const auto say_syscalls = [&]() {
    const std::string tally = monitor.TallyText();
    if (tally.empty()) {
      Say(&out, "syscalls: none were made — the run stopped before guest code asked the OS for anything");
      return;
    }
    Say(&out, "syscalls: %s", tally.c_str());
  };

  // A constructor that did not return leaves the module half-initialized, so its entry points are not
  // called: a real runtime would not be entering this library either. Named rather than silent, because
  // "why is there no JNI_OnLoad line" is the first question a reader of this report will have.
  if (!ctors_ok) {
    Say(&out, "JNI_OnLoad: not called — a constructor above did not return, so the library is not in a"
              " state a runtime would enter");
    say_gl();
    say_syscalls();
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
      for (std::size_t i = 0; i < java_entries.size() && i < 8; ++i) {
        Say(&out, "  %s", java_entries[i].c_str());
      }
      if (java_entries.size() > 8) Say(&out, "  ... and %zu more", java_entries.size() - 8);

      /**
       * Why this one of the seven: it is the only name in the list above that is not a per-frame or
       * per-input callback — the engine's one-time startup, which is what a runtime's first call into a
       * game does before a frame exists. The name is checked against the image rather than assumed, so a
       * library that does not export it is reported as such instead of being skipped quietly.
       *
       * Its argument list is no longer a guess. The APK's own `classes.dex` declares it as
       * `Lcom/rovio/ka3d/MyRenderer;->nativeInit(Lcom/rovio/ka3d/MyRenderer;IILjava/lang/String;)Z`,
       * shorty `ZLIIL` — a *non-static* method, so the symbol's r0/r1 are the `JNIEnv*` and `this`, and
       * the declared parameters follow: r2 the `MyRenderer` instance, r3 the width, then the height and
       * the APK path on the stack in that order. That is the shape a real ART would call it with, and it
       * is exactly the shape `rb::abi`'s `PlaceArgs` builds from a six-slot list — which is why the call
       * below has six arguments and the shorty `"ZLIIL"` instead of the placeholder `"I"` this file used
       * while the parameter list still lived in the APK's Java half and not in these bytes.
       *
       * Called on the constructor tier, once, and that is a requirement rather than a preference: this
       * function has side effects in one shared guest memory (the rule the constructor sequence above
       * already follows), and calling it on both tiers would run the game's startup twice and count every
       * GL call it made twice.
       */
      constexpr const char* kEntryPoint = "Java_com_rovio_ka3d_MyRenderer_nativeInit";
      constexpr const char* kEntryPrototype = "(Lcom/rovio/ka3d/MyRenderer;IILjava/lang/String;)Z";
      const auto entry = app.FindExport(kEntryPoint);
      if (!entry) {
        Say(&out, "%s: not exported by this library either, so this run has no JNI method to call — the"
                  " names above are the list a Java side would drive.",
            kEntryPoint);
      } else {
        Say(&out, "%s: exported (0x%s). Calling it as its own DEX declares it: %s.", kEntryPoint,
            Hex(static_cast<std::uint32_t>(entry->addr)).c_str(), kEntryPrototype);
        Say(&out, "  r0=env 0x%s (the fake JNIEnv above), r1=this 0x%s, r2=MyRenderer 0x%s, r3=width %d;"
                  " the stack carries height %d, then apkPath 0x%s, in that order.",
            Hex(env_ptr).c_str(), Hex(stand_in).c_str(), Hex(stand_in).c_str(), surface_width,
            surface_height, Hex(apk_path_guest).c_str());
        if (stand_in != 0) {
          Say(&out, "  the two object arguments are one zeroed guest page: not Java objects — there is no"
                    " JavaVM here — so a callee that dereferences them reads zeroes, and one that calls a"
                    " JNIEnv entry on them gets that entry's refusal by name.");
        } else {
          Say(&out, "  the two object arguments are 0 because the stand-in page could not be mapped — a"
                    " callee that dereferences them faults near address 0 (see the entry line above).");
        }
        Say(&out, "  width and height are the Activity's display pixels, measured on the Java side and"
                  " passed in on the game's behalf — the surface a GLSurfaceView.Renderer would have been"
                  " handed, not numbers this run computed.");
        if (apk_path_guest != 0) {
          Say(&out, "  apkPath is a NUL-terminated guest string, not a jstring handle: the fake environment"
                    " mints a handle only while the guest is inside NewStringUTF, so a callee that hands"
                    " this pointer to GetStringUTFChars gets that entry's named refusal, not the text.");
        }
        const Outcome got = Call(ctor_engine, ctx, app, kEntryPoint, "ZLIIL",
                                 {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(stand_in),
                                  abi::Value32::Ptr(stand_in), abi::Value32::S32(surface_width),
                                  abi::Value32::S32(surface_height), abi::Value32::Ptr(apk_path_guest)});
        Say(&out, "%s tier=%s reason=%s steps=%u value=0x%s (Z: the DEX's return type, so this is r0's"
                  " low byte as the bridge reads a jboolean)",
            kEntryPoint, ctor_tier, ReasonName(got.reason), got.steps,
            Hex(static_cast<std::uint32_t>(got.bits)).c_str());
        if (!got.detail.empty()) Say(&out, "    stopped because: %s", got.detail.c_str());
        Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s r1=0x%s sp=0x%s cpsr=0x%s",
            Hex(got.exit.pc).c_str(), Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(),
            Hex(got.exit.r[1]).c_str(), Hex(got.exit.r[13]).c_str(), Hex(got.exit.cpsr).c_str());
      }
    }
    say_gl();
    say_syscalls();
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
  say_gl();
  say_syscalls();
  say_trapped();
  return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeInspect(JNIEnv* env, jclass /*clazz*/,
                                                          jbyteArray guest_so, jbyteArray libm_so,
                                                          jbyteArray libc_so, jbyteArray libz_so,
                                                          jstring label, jint tiers,
                                                          jstring apk_path, jint surface_width,
                                                          jint surface_height) {
  std::string name;
  if (label != nullptr) {
    const char* chars = env->GetStringUTFChars(label, nullptr);
    if (chars != nullptr) {
      name = chars;
      env->ReleaseStringUTFChars(label, chars);
    }
  }
  if (name.empty()) name = "guest.so";
  // The APK's absolute path as the Java half knows it: the string the guest is told and the one path the
  // monitor's provider will serve bytes for. Absent is not a failure here — the native side reports a run
  // without it — so it is copied like any other string and its presence is decided in `Inspect`.
  std::string apk;
  if (apk_path != nullptr) {
    const char* chars = env->GetStringUTFChars(apk_path, nullptr);
    if (chars != nullptr) {
      apk = chars;
      env->ReleaseStringUTFChars(apk_path, chars);
    }
  }
  const std::vector<std::uint8_t> app_image = Bytes(env, guest_so);
  const std::vector<std::uint8_t> libm_image = Bytes(env, libm_so);
  const std::vector<std::uint8_t> libc_image = Bytes(env, libc_so);
  const std::vector<std::uint8_t> libz_image = Bytes(env, libz_so);
  std::string report;
  try {
    report = Inspect(app_image, libm_image, libc_image, libz_image, name, static_cast<int>(tiers), apk,
                     static_cast<int>(surface_width), static_cast<int>(surface_height));
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
