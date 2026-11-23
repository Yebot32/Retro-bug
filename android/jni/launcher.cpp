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
#include <dlfcn.h>
#include <fcntl.h>

#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
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
#include "rb/runtime/guest_process.h"

#include <android/native_window_jni.h>
#include <functional>

#include "gl_forward.h"
#include "host_slots.h"
#include "inspect.h"
#include "runtime_boot.h"


/// The milestones that a report line cannot cover. `Launch` returns its report as a string and the Java
/// half logs every line, but a `SIGSEGV` inside a guest call is not a C++ exception: it takes the process
/// before any string is built. These are the lines a tombstone then gets attributed to.
#define RB_LOG(...)                                    \
  do {                                                 \
    char _rb_line[512];                                \
    std::snprintf(_rb_line, sizeof _rb_line, __VA_ARGS__);         \
    __android_log_print(ANDROID_LOG_INFO, "RBLAUNCH", "%s", _rb_line); \
  } while (0)

using namespace rb::launcher;

namespace {

using namespace rb;
using namespace rb::mem;

/// `Launch`'s guest address layout (the probe self-test; `Inspect` runs on a `GuestProcess`, which places its
/// own), identical to `core/tests/guest_apk_test.cpp`'s rig so that the two lanes'
/// stack frames, sentinel and guard page have the same shape and a difference between them cannot be
/// blamed on where the code landed.
///
/// The stack is the one figure here that the probe rig did not need to get right and a real library does:
/// **32 MiB below the window's top (31–63 MiB)**, where Android gives a main thread 8 MiB, instead of
/// the 64 KiB this was. 64 KiB was obviously enough for the probe's handful of frames and is nothing like
/// enough for bionic: its `__libc_preinit` walked 65,640 bytes down it and the next store landed 104 bytes
/// past the bottom, which reads as `guest memory write at 0xeff90 refused` with `sp = 0xeff98` — an address
/// that sends a reader hunting a bug in libc when the bug is this constant. It sits at the top of the
/// window because the crowded part is the *bottom*: `kGuardPage` at 8 MiB, the sentinel at 12 MiB, and images placed
/// upward from 0x2000.
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x03f0'0000u;
constexpr exec::GuestAddr kGuardPage = 0x0080'0000u;
constexpr exec::GuestAddr kStackBytes = 32u << 20;
/// Nothing is mapped here. An unresolved import that is *called* faults and names this address instead
/// of returning a plausible zero; see the file header.
constexpr std::uint32_t kUnresolved = 0x0bad'0000u;
constexpr std::size_t kWindowBytes = 64u << 20;
/// The host-entry window: the return sentinel at `kReturnAddress` plus the `AbiContext` slot table,
/// which lives at `kReturnAddress + 0x1000` for `kMaxSlots * 4` bytes. Nothing here is mapped by the
/// bridge — the executor recognises these addresses before translation — and the memory model is told
/// that with a guard reservation, because an *unreserved* window is a loaded library that ends past
/// 12 MiB: the loader's free-space scan would place a module over the sentinel and the slots, and a
/// guest branch to a 4-aligned address inside the range would then report a bogus host call.
constexpr std::size_t kHostWindowBytes = 0x1000u + std::size_t{abi::AbiContext::kMaxSlots} * 4u;
/// One guest call's instruction budget, the same figure the APK lane uses. Tier I counts IR ops and Tier
/// J guest encodings, so this bounds the two differently — it exists to stop a runaway guest, not to
/// measure it.
constexpr unsigned kCallBudget = 4'000'000;
/// The game entry point's own budget. Angry Birds' `nativeInit` boots KA3D's embedded Lua VM and ran
/// out of kCallBudget inside Lua's GC (`singlestep` -> `sweeplist`, libangrybirds +0xa5304) about one
/// second in, on the SM-X710 — real work, not a loop. 50x that is ~50 s of Tier I, inside RB_WAIT=130.
constexpr unsigned kEntryBudget = 50 * kCallBudget;
constexpr std::int32_t kJniVersion16 = 0x0001'0006;
constexpr const char* kAppSoname = "libmathprobe.so";

/// The host's answers to a guest's `gl*` calls point into one guest page `Inspect` maps (with `MapFree`)
/// before any module is placed. Its whole content is the pair of strings `glGetString` hands back — a game
/// parses that result, so a defined string is the difference between "this host has no GL" and a null
/// dereference that reads as a bug in the game. `"RetroBug (no GL)"` is sixteen bytes and a NUL, so the
/// empty answer starts at the next word; the assert keeps a longer identity string from overwriting it.
constexpr unsigned kGlEmptyOffset = 20u;
static_assert(sizeof("RetroBug (no GL)") <= kGlEmptyOffset,
              "the identity string would run into the empty one");

/// `Launch`'s fixed guest pages (`Inspect` runs on a `GuestProcess`, which places its own), checked as a
/// set: the one collision this layout ever had wiped the GL identity strings on every game run.
constexpr exec::GuestAddr kReservedPages[] = {kReturnAddress, kGuardPage, kUnresolved, kStackTop};

constexpr bool ReservedPagesAreDistinctAndAligned() {
  constexpr std::size_t n = sizeof(kReservedPages) / sizeof(kReservedPages[0]);
  for (std::size_t i = 0; i < n; ++i) {
    // A reserved page that is not page-aligned is a typo, and `Map` would round it up onto its neighbour.
    if ((kReservedPages[i] & (kGuestPage - 1u)) != 0) return false;
    for (std::size_t j = i + 1; j < n; ++j) {
      if (kReservedPages[i] == kReservedPages[j]) return false;
    }
  }
  return true;
}
static_assert(ReservedPagesAreDistinctAndAligned(),
              "two reserved guest pages share an address, or one is not page-aligned");

/// The host-entry window is sized for what the bridge actually puts in it: the return sentinel at
/// `kReturnAddress` plus the slot table at `kReturnAddress + 0x1000` for `kMaxSlots` four-byte entries.
/// That arithmetic lived in prose, which is the one place a reader cannot check by counting.
static_assert(kHostWindowBytes >= 0x1000u + static_cast<std::size_t>(abi::AbiContext::kMaxSlots) * 4u,
              "the host-entry window is smaller than the sentinel page plus the slot table");

/// `kUnresolved` is deliberately **outside** the guest window, and that is what makes it a trap no module
/// can land on: the loader's placement scan only ever considers pages inside the window, so an import
/// answered with this address stays uncallable by arithmetic rather than by a reservation.
///
/// This assert is where a fixed bug now lives. An earlier revision read the audit's note about the trap
/// page as "reserve it", called `Map(kUnresolved, ...)` in both flows, and refused **every run** with
/// `mapping exceeds the guest window` before a single library was loaded — because 0x0bad0000 is 186 MiB
/// and `kWindowBytes` is 64. Nothing in the test suite runs the launcher, so only a device did. A layout
/// claim belongs in a static_assert; a runtime reservation can fail on somebody's phone.
static_assert(kUnresolved >= kWindowBytes,
              "the unresolved-import address must lie outside the guest window, or the loader could place "
              "a module on the trap");

/// Every reserved guest page, pairwise distinct at compile time — see `ReservedPagesAreDistinctAndAligned`
/// above the layout. The layout grew by hand, and the one collision it ever had (the GL strings and
/// bionic's thread struct both at 0x430000) wiped the GL identity strings on every game run without a
/// single test noticing.
static_assert(kStackTop - kStackBytes > kReturnAddress + kHostWindowBytes,
              "the guest stack would run into the host-entry window");

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

/// The dynamic-linker and liblog slots sit right above the GL window; they are `HostSlots`' (host_slots.h),
/// shared with the in-app runtime.
static_assert(kGlSlotBase + kGlSlots <= HostSlots::kDlSlotBase,
              "the GL window would alias the dl* host slots");

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
/// One report line, sanitized on the way in. Guest-supplied strings (a soname, a `DT_NEEDED` name, a
/// refused import's name) reach the report through `%s`, and a crafted image controls those bytes: an
/// embedded NUL truncated everything after it at `NewStringUTF`, and a newline forged report lines.
/// Control bytes are escaped; bytes >= 0x80 pass through untouched (release ART's `NewStringUTF` is
/// lenient about them), which is a stated residual rather than a silent one.
void AppendSanitized(std::string* out, const char* buf) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(buf); *p != 0; ++p) {
    if (*p < 0x20 || *p == 0x7f) {
      char esc[8];
      std::snprintf(esc, sizeof esc, "\\x%02x", *p);
      *out += esc;
    } else {
      out->push_back(static_cast<char>(*p));
    }
  }
}

/// What the screen shows while a run is still going: the latest report line (or a status a long loop sets
/// without adding a line), and how many lines the report has so far. Written by the run thread, read by
/// `nativeProgress` from the UI thread, hence the lock. Before this the screen said "reading <apk> ..." for
/// the whole run — 90 s of apparent nothing on Gumball's interpreter-tier frame loop.
std::mutex g_progress_mu;
std::string g_progress;
unsigned g_progress_lines = 0;

void SetProgress(const std::string& status) {
  std::lock_guard<std::mutex> lock(g_progress_mu);
  g_progress = status;
}

/**
 * Touches on the run screen, queued by the UI thread and drained by the frame loop — the same hand-off
 * Gumball's own Java makes. Transcribed from its DEX (`JNIGLSurface.onTouchEvent`, lines 655–697, and
 * `onDrawFrame`, lines 134–138), not guessed:
 *
 *     onTouchEvent: time = currentTimeMillis()
 *                   if nextTouchLock != 0:
 *                     if time < nextTouchLock: if ++jabcount > 5: drop ("Ouch! Quit jabbing me!")
 *                     else if jabcount > 0: --jabcount
 *                   nextTouchLock = time + 1
 *                   action = {DOWN: 1, UP: 2, MOVE: 3, CANCEL: 4}[getAction()], else 0
 *                   touches.add(Touch(x = (int) getY(), y = getWidth() - (int) getX(), action))
 *     onDrawFrame:  ... jniRun(dt); for each queued touch: jniTouch(x, y, action); touches.clear() ...
 *
 * The coordinate swap is the game's: its content is landscape inside a portrait surface. `getAction()`
 * carries the pointer index in its high bits, so a second finger's POINTER_DOWN maps to 0 there too.
 * Touches are only accepted while a frame loop is draining them; anything else would replay stale taps
 * into the next run.
 */
struct QueuedTouch {
  std::int32_t x, y, action;
  float raw_x, raw_y;  // the view's own getX/getY, for the report
};
std::mutex g_touch_mu;
std::vector<QueuedTouch> g_touches;
bool g_touch_open = false;
std::int64_t g_next_touch_lock = 0;
int g_jab_count = 0;
unsigned g_touch_dropped = 0;  // jab-throttled, as the Java drops them

void OpenTouches(bool open) {
  std::lock_guard<std::mutex> lock(g_touch_mu);
  g_touch_open = open;
  g_touches.clear();
  g_next_touch_lock = 0;
  g_jab_count = 0;
  g_touch_dropped = 0;
}

std::vector<QueuedTouch> TakeTouches() {
  std::lock_guard<std::mutex> lock(g_touch_mu);
  std::vector<QueuedTouch> out;
  out.swap(g_touches);
  return out;
}

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
  AppendSanitized(out, buf);
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
  {
    std::lock_guard<std::mutex> lock(g_progress_mu);
    g_progress = buf;
    ++g_progress_lines;
  }
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

/// True when a Java exception was pending and has been cleared. The allocating JNI calls here
/// (`GetByteArrayElements`, `GetStringUTFChars`, `NewStringUTF`) can throw `OutOfMemoryError`; a
/// pending exception left set supersedes this function's return value at the Java call site — the
/// finished report was discarded, and `runGame` catches only `IOException | RuntimeException`, so an
/// `Error` reached the default uncaught handler and the app died holding a report it never saw.
///
/// `note` is what keeps the *other* half from being lost: clearing an exception makes the failing call
/// look like an ordinary empty result, so the caller has to be told that one was pending. Passing
/// `nullptr` here is what made a run out of host memory report itself as a guest image that was not an
/// ELF.
bool ClearPendingException(JNIEnv* env, std::string* note) {
  if (env == nullptr || env->ExceptionCheck() != JNI_TRUE) return false;
  jthrowable pending = env->ExceptionOccurred();
  env->ExceptionClear();
  // `note != nullptr`, not `!note->empty() == false`: the latter parses as "append only when the note is
  // empty", which is the opposite of what the sentence needs and made the line dead code for every caller
  // that had already written something. The clause is written to follow existing text, so it reads as an
  // append whenever a caller asked for one.
  if (note != nullptr) *note += "; a pending Java exception was cleared";
  if (pending != nullptr) env->DeleteLocalRef(pending);
  return true;
}

/// A `jbyteArray` copied into a vector the engine can be handed. The images arrive as bytes rather than
/// as paths because that is the half of the job each side is good at: `rb::loader` takes bytes or an fd,
/// the guest library is one entry inside a zip, and `core/tests/guest_apk_test.cpp` carries a
/// stored-entry-only zip reader of its own to get at it. The Java side already has
/// `java.util.zip.ZipInputStream`, which handles either compression, so the extraction happens there and
/// this file sees only ELF images.
///
/// A null `GetByteArrayElements` returns an *empty* vector, which is indistinguishable from a zero-length
/// image. `note` carries the difference: it names the call that failed and whether a Java exception was
/// pending, so the caller can say "the host ran out of memory" instead of letting the loader report
/// "not an ELF image" about bytes the Java half never handed over.
std::vector<std::uint8_t> Bytes(JNIEnv* env, jbyteArray array, std::string* note) {
  if (env == nullptr || array == nullptr) return {};
  const jsize len = env->GetArrayLength(array);
  if (len <= 0) return {};
  jbyte* raw = env->GetByteArrayElements(array, nullptr);
  if (raw == nullptr) {
    if (note != nullptr) *note += "GetByteArrayElements returned null";
    ClearPendingException(env, note);
    return {};
  }
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
    const int saved = errno;  // captured before anything (an allocation, a stream write) can move it
    if (err != nullptr) *err = "open(" + path + "): " + std::strerror(saved);
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[1 << 16];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      const int saved = errno;
      if (err != nullptr) *err = "read(" + path + "): " + std::strerror(saved);
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    bytes.insert(bytes.end(), buf, buf + n);
  }
  ::close(fd);
  return bytes;
}

/// Whether `needle` occurs anywhere in the image bytes — how the launcher tells which game profile an app
/// is (its exported `Java_*` names live in .dynstr) before the image is loaded.
bool ImageHasString(const std::vector<std::uint8_t>& img, const std::string& needle) {
  return std::search(img.begin(), img.end(), needle.begin(), needle.end()) != img.end();
}

/// Width and height from an image file's header, without decoding it: PNG's IHDR, or a JPEG's first SOFn
/// marker. What `BitmapFactory.decodeStream(...).getWidth()/getHeight()` would report for the same bytes,
/// which is all a texture callback needs when no GL exists to upload pixels to. false for anything else.
bool ImageSize(const std::vector<std::uint8_t>& b, std::uint32_t& w, std::uint32_t& h) {
  static const std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (b.size() >= 24 && std::memcmp(b.data(), kPng, 8) == 0 && std::memcmp(b.data() + 12, "IHDR", 4) == 0) {
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return true;
  }
  if (b.size() >= 4 && b[0] == 0xff && b[1] == 0xd8) {
    std::size_t at = 2;
    while (at + 9 < b.size()) {
      if (b[at] != 0xff) return false;
      const std::uint8_t marker = b[at + 1];
      const std::size_t len = (b[at + 2] << 8) | b[at + 3];
      const bool sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
      if (sof) {
        h = (b[at + 5] << 8) | b[at + 6];
        w = (b[at + 7] << 8) | b[at + 8];
        return true;
      }
      at += 2 + len;
    }
  }
  return false;
}

}  // namespace

// Out of the anonymous namespace so `runtime_boot.cpp` can call it (declared in `inspect.h`).
/// One entry of a zip held in memory, by exact name — what `AssetManager.open` reads for a game's
/// `assets/` path. The central directory is the authority for sizes (a local header behind a data
/// descriptor carries zeros), stored and deflated entries are both served (Angry Birds 1.0.0 ships 250
/// of its 270 assets deflated), and everything else — ZIP64, another method, a field that runs off the
/// buffer, a size that does not inflate to what the directory claims — comes back as `nullopt` with
/// `why` saying which. `name` empty means "the entry with the lowest local offset", which is the one
/// `ZipInputStream.getNextEntry()` would return first.
std::optional<std::vector<std::uint8_t>> rb::launcher::ZipEntry(const std::vector<std::uint8_t>& zip,
                                                  const std::string& name, std::string* why) {
  const auto fail = [&](const std::string& w) -> std::optional<std::vector<std::uint8_t>> {
    if (why != nullptr) *why = w;
    return std::nullopt;
  };
  const auto u16 = [&](std::size_t at) -> std::uint32_t { return zip[at] | (zip[at + 1] << 8); };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    return zip[at] | (zip[at + 1] << 8) | (zip[at + 2] << 16) | (static_cast<std::uint32_t>(zip[at + 3]) << 24);
  };
  if (zip.size() < 22) return fail("shorter than an end-of-central-directory record");
  std::size_t eocd = zip.size() - 22;
  const std::size_t floor = zip.size() > 22 + 0xffff ? zip.size() - 22 - 0xffff : 0;
  while (u32(eocd) != 0x06054b50u) {
    if (eocd == floor) return fail("no end-of-central-directory record");
    --eocd;
  }
  const std::uint32_t entries = u16(eocd + 10);
  std::size_t at = u32(eocd + 16);
  if (at == 0xffff'ffffu || entries == 0xffffu) return fail("a ZIP64 archive, which this reader does not read");
  struct Hit {
    std::uint32_t method = 0, csize = 0, usize = 0, local = 0;
    std::string name;
  };
  std::optional<Hit> hit;
  for (std::uint32_t i = 0; i < entries; ++i) {
    if (at + 46 > zip.size() || u32(at) != 0x02014b50u) return fail("central directory entry " + std::to_string(i) + " is damaged");
    const std::uint32_t nlen = u16(at + 28), xlen = u16(at + 30), clen = u16(at + 32);
    if (at + 46 + nlen > zip.size()) return fail("central directory name runs off the archive");
    Hit h{u16(at + 10), u32(at + 20), u32(at + 24), u32(at + 42),
          std::string(reinterpret_cast<const char*>(zip.data() + at + 46), nlen)};
    if (name.empty() ? (!hit || h.local < hit->local) : h.name == name) {
      hit = h;
      if (!name.empty()) break;
    }
    at += 46 + nlen + xlen + clen;
  }
  if (!hit) return fail(name.empty() ? "the archive has no entries" : "no entry named " + name);
  const std::size_t lh = hit->local;
  if (lh + 30 > zip.size() || u32(lh) != 0x04034b50u) return fail(hit->name + ": local header is damaged");
  const std::size_t data = lh + 30 + u16(lh + 26) + u16(lh + 28);
  if (data + hit->csize > zip.size()) return fail(hit->name + ": data runs off the archive");
  if (hit->method == 0) {
    if (hit->csize != hit->usize) return fail(hit->name + ": stored, but the sizes disagree");
    return std::vector<std::uint8_t>(zip.begin() + static_cast<std::ptrdiff_t>(data),
                                     zip.begin() + static_cast<std::ptrdiff_t>(data + hit->csize));
  }
  if (hit->method != 8) return fail(hit->name + ": compression method " + std::to_string(hit->method) + " is not stored or deflate");
  std::vector<std::uint8_t> out(hit->usize);
  z_stream z{};
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail(hit->name + ": inflateInit2 failed");
  z.next_in = const_cast<Bytef*>(zip.data() + data);
  z.avail_in = hit->csize;
  z.next_out = out.data();
  z.avail_out = hit->usize;
  const int rc = inflate(&z, Z_FINISH);
  const std::size_t produced = z.total_out;
  inflateEnd(&z);
  if (rc != Z_STREAM_END || produced != hit->usize) {
    return fail(hit->name + ": inflate ended with " + std::to_string(rc) + " after " + std::to_string(produced) +
                " of " + std::to_string(hit->usize) + " bytes");
  }
  return out;
}

namespace {

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
             const char* symbol, const char* shorty, const abi::Args32& args,
             unsigned budget = kCallBudget) {
  Outcome out;
  const auto fn = mod.FindExport(symbol);
  if (!fn) {
    out.detail = std::string{"the loaded library does not export "} + symbol;
    return out;
  }
  ctx.set_executor(engine);
  exec::RunResult result{};
  const abi::Ret32 ret =
      ctx.CallGuest(fn->tagged(), args, abi::Shorty::For(shorty), budget, &result, &out.exit);
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
  // The host-entry window is reserved, not merely avoided: nothing maps it, but the loader's
  // free-space scan must see it as taken, or a library ending past 12 MiB is placed over the return
  // sentinel and the `AbiContext` slot table. `kNone` makes it a guard reservation — occupied for
  // placement, refused for any access.
  if (!mem->Map(kReturnAddress, kHostWindowBytes, kNone, &err)) {
    Say(&out, "FATAL: reserving the host-entry window failed: %s", err.c_str());
    return out;
  }
  // `kUnresolved` needs no reservation, and an earlier version of this file tried to make one and broke
  // every run: the address is *outside* the guest window (0x0bad0000 against kWindowBytes, 64 MiB), so
  // `Map` refuses it with "mapping exceeds the guest window" and the run stopped before loading anything.
  // The invariant that guard page was meant to buy is already true by arithmetic: the loader's placement
  // scan only ever considers pages inside `pages_`, so no module can be placed past the window.
  // `kUnresolved`'s own assert above is where that belongs — a claim about the layout, checked at compile
  // time, rather than a runtime reservation that can fail on a device.

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
  app_opts.unresolved_trap = kUnresolved;  // a weak import nothing answers is null, as on a device
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
  // the engine either reaches or does not, with no math in the way. A tier that reported itself
  // unavailable is skipped with a line rather than run — `JitExecutor::Run` answers `Internal` for a
  // dead tier, so the alternative is two rows of FAIL noise for the exact condition the line at the
  // top of this function already reported.
  const std::string want_onload = "0x" + Hex64(static_cast<std::uint64_t>(kJniVersion16));
  for (auto* engine : {static_cast<exec::Executor*>(&interp), static_cast<exec::Executor*>(&jit)}) {
    const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
    if (!engine->ok()) {
      Say(&out, "JNI_OnLoad tier=%s skipped: that tier reported itself unavailable", tier);
      continue;
    }
    RB_LOG("JNI_OnLoad on tier %s", tier);
    const Outcome got = Call(*engine, ctx, *app, "JNI_OnLoad", "II",
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0)});
    emit("JNI_OnLoad", tier, got, static_cast<std::uint64_t>(kJniVersion16), want_onload, true);
  }

  // Then the method, both tiers, against `kHardware`.
  for (auto* engine : {static_cast<exec::Executor*>(&interp), static_cast<exec::Executor*>(&jit)}) {
    const char* tier = engine->tier() == exec::CodeTier::Interp ? "I" : "J";
    if (!engine->ok()) {
      Say(&out, "%s tier=%s skipped: that tier reported itself unavailable", what.c_str(), tier);
      continue;
    }
    RB_LOG("%s on tier %s", what.c_str(), tier);
    const Outcome got = Call(*engine, ctx, *app, m->symbol, m->shorty,
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0), abi::Value32::S32(seed)});
    emit(what, tier, got, want_bits, want, have_hardware);
  }

  Say(&out, "summary: %u PASS, %u FAIL over %u calls", pass, fail, pass + fail);
  return out;
}

}  // namespace

// Out of the anonymous namespace so `runtime_boot.cpp` can call it (declared in `inspect.h`, with
// `RenderHooks`). Its helpers above stay file-local.
namespace rb::launcher {
using namespace rb::mem;

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
 * The guest process — memory, bridge, monitor, JNIEnv, both tiers, and the device's libc booted with its
 * IFUNC slots resolved and bionic's real main thread — is `rb::runtime::GuestProcess`, the one copy of that
 * boot (the in-app runtime uses it too). What stays here is the launcher's own: its 64 MiB window and trap,
 * the GL host slots, the per-game profiles (KA3D's renderer, Gumball's surface and frame loop) and the
 * report. `Launch` keeps its own hand-built layout: its PASS/FAIL table was measured against it.
 *
 * `tiers`: 0 = interpreter then JIT, 1 = interpreter only, 2 = JIT only.
 *
 * `libc_image`, `libm_image` and `libz_image` are the guest scope this APK ships — the device's bionic
 * `libc.so` and libm/libz built from the NDK archive; all three are required.
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
                    const std::vector<std::uint8_t>& libz_image,
                    const std::vector<std::uint8_t>& libstdcxx_image, const std::string& label,
                    int tiers, const std::string& apk_path, int surface_width,
                    int surface_height, const RenderHooks& render) {
  std::string out;
  {
    std::lock_guard<std::mutex> lock(g_progress_mu);
    g_progress = "starting";
    g_progress_lines = 0;
  }
  const unsigned page_shift = HostPageShift();
  Say(&out, "inspect: %s=%zu bytes libm.so=%zu libc.so=%zu libz.so=%zu libstdc++.so=%zu bytes host_page_shift=%u",
      label.c_str(), app_image.size(), libm_image.size(), libc_image.size(), libz_image.size(),
      libstdcxx_image.size(),
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

  if (libc_image.empty() || libz_image.empty()) {
    Say(&out, "FATAL: the guest %s asset is missing — the guest process boots the device's own bionic libc"
              " with libm and libz, and runs nothing without them",
        libc_image.empty() ? "libc" : "libz");
    return out;
  }

  /**
   * The guest process: memory, bridge, syscall monitor, JNIEnv, both tiers, the device's libc booted the
   * way its linker leaves a process (`rb/runtime/guest_process.h`) — the one copy of that boot, shared with
   * the in-app runtime. The launcher keeps its 64 MiB window and its trap at 0x0bad0000, outside the window
   * so no module can be placed on it. Constructors (and IFUNC resolvers) run on the tier this run chose:
   * Tier J when the JIT alone was asked for, Tier I otherwise — once, since they have side effects in one
   * guest memory.
   */
  const int ctor_tier_id = tiers == 2 ? 2 : 1;
  const char* ctor_tier = ctor_tier_id == 2 ? "J" : "I";
  /// What the launcher answers beyond the scope (set once the slots exist, below). The GL names are the
  /// launcher's own; the dl* and liblog set is `HostSlots`'.
  std::function<std::optional<std::uint32_t>(const std::string&)> host;
  std::string err;
  runtime::GuestProcessOptions popts;
  popts.window_bytes = kWindowBytes;
  popts.biased = true;
  popts.unresolved_trap = kUnresolved;
  popts.ctor_tier = ctor_tier_id;
  popts.log = [&out](const std::string& line) { Say(&out, "%s", line.c_str()); };
  popts.host_resolver = [&host](const std::string& name) -> std::optional<runtime::GuestAddr> {
    return host ? host(name) : std::nullopt;
  };
  std::unique_ptr<runtime::GuestProcess> proc = runtime::GuestProcess::Create(popts, &err);
  if (!proc) {
    Say(&out, "FATAL: %s", err.c_str());
    return out;
  }
  GuestMemory* const mem = &proc->memory();
  abi::AbiContext& ctx = proc->abi();
  abi::JniEnv& jni = proc->jni();
  syscall::Monitor& monitor = proc->monitor();
  exec::Executor& interp = proc->engine(1);
  exec::Executor& jit = proc->engine(2);
  exec::Executor& ctor_engine = proc->engine(ctor_tier_id);
  if (!jit.ok()) Say(&out, "tier J is unavailable before any guest code ran");
  Say(&out, "process: a %zu MiB biased guest window, unresolved imports bound to 0x%s, constructors on tier %s",
      kWindowBytes >> 20, Hex(kUnresolved).c_str(), ctor_tier);

  /**
   * The guest pages the entry points' arguments live in, mapped before anything is placed (the loader
   * takes the first free run, so a page mapped later could not be told apart from a module's own): a zeroed
   * page standing in for the Java object a game's entry point receives as `this` (and KA3D's `MyRenderer`
   * parameter), and a page holding the APK's absolute path as a NUL-terminated guest string — also the
   * launcher's scratch for the strings its profile calls hand the guest. Neither is a Java object: there is
   * no JavaVM here, and the call lines say what the addresses are.
   */
  exec::GuestAddr stand_in = proc->MapFree(kGuestPage, kRead | kWrite, &err);
  if (stand_in == 0) {
    Say(&out, "entry: the stand-in object page could not be mapped (%s) — the entry point is still called,"
              " with 0 where that object should be", err.c_str());
  }
  const exec::GuestAddr scratch_at = proc->MapFree(kGuestPage, kRead | kWrite, &err);
  exec::GuestAddr apk_path_guest = 0;
  if (apk_path.empty()) {
    Say(&out, "entry: the Java half passed no APK path, so the library's path argument is 0 and it has no"
              " way to reach the assets inside its own APK");
  } else if (apk_path.size() + 1 > kGuestPage) {
    Say(&out, "entry: the APK path is %zu bytes and does not fit the %u-byte path page — the path argument is 0",
        apk_path.size(), kGuestPage);
  } else if (scratch_at == 0) {
    Say(&out, "entry: the APK path page could not be mapped (%s) — the path argument is 0", err.c_str());
  } else if (!mem->Write(scratch_at, apk_path.c_str(), apk_path.size() + 1)) {
    Say(&out, "entry: guest memory at 0x%s refused the APK path — the path argument is 0", Hex(scratch_at).c_str());
  } else {
    apk_path_guest = scratch_at;
  }

  /**
   * The host-answered OpenGL ES 1.x set.
   *
   * Why this exists at all: 53 of the Angry Birds 1.0.0 library's 182 imports are `gl*`, and the client
   * library that would define them, `libGLESv1_CM.so`, is in no image here and is not being fabricated.
   * Left alone they land on `kUnresolved`, so the first one the game calls faults at an address that names
   * nothing. This answers them instead, and visibly: one host slot per name the run asks for, a tally per
   * name, and report lines that say exactly what happened — the sanctioned shape of "not implemented".
   *
   * What a call does without a live context: nothing but count, and leave r0 at 0 (`glGetError`'s 0 is
   * `GL_NO_ERROR`). `glGetString` is answered with bytes, because a game parses its result and a null would
   * crash it in a way that looks like a game bug.
   */
  const exec::GuestAddr gl_strings = proc->MapFree(kGuestPage, kRead | kWrite, &err);
  const exec::GuestAddr gl_empty = gl_strings + kGlEmptyOffset;
  std::string gl_why = err;
  const bool gl_ready = gl_strings != 0 &&
                        mem->Write(gl_strings, "RetroBug (no GL)", sizeof("RetroBug (no GL)")) &&
                        mem->Write(gl_empty, "", 1);
  if (!gl_ready) {
    Say(&out, "gl: the strings glGetString must return could not be placed (%s) — the gl* imports stay"
              " trapped at 0x%s, which faults the moment one is called",
        gl_strings == 0 ? gl_why.c_str() : "the page refused the writes", Hex(kUnresolved).c_str());
  }
  struct GlEntry {
    std::string name;
    unsigned calls = 0;
  };
  std::vector<GlEntry> gl_calls;
  /// The renderer's state for the GL slots: forwarding is live only while the run holds a current EGL
  /// context (the Gumball profile makes one on the launcher's window). Off, the slots are the counted
  /// no-ops they always were — headless runs and the scripted lanes are unchanged.
  rb::launcher::GlForward gl_forward;
  bool gl_live = false;
  std::map<std::string, std::string> gl_forward_fail;  ///< first refusal per name
  /// Hands one `gl*` name a host slot, allocating on first sight and reusing it after. `nullopt` is a
  /// refusal — no strings to point at, or the set is full — and the name then stays trapped.
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
    //
    // Bound `kHoldsGgl` (M2 Task 4, ruling i): no part of a forwarded GL call is pure host work. The driver
    // dereferences guest memory through translated pointers — the call's own pointer arguments, and client
    // arrays an earlier `gl*Pointer` registered, which `glDrawArrays` reads with no pointer argument at all —
    // and the counters and `gl_forward_fail` are run state. With the GGL released another guest thread could
    // munmap/mprotect (rebalance) those pages mid-read. A GL call never waits on another guest thread, so
    // holding the lock cannot deadlock; it only serializes GL with guest code, as one GL context does anyway.
    ctx.Bind(slot, [&gl_calls, &gl_forward, &gl_live, &gl_forward_fail, mem, gl_strings, gl_empty, index,
                    name](exec::MachineState& s, std::string*) {
      ++gl_calls[index].calls;
      const bool wants_a_string = name == "glGetString";
      // With a live context the call goes to the device's driver; glGetString stays answered from guest
      // memory, because its result is a pointer the guest dereferences. A call that cannot be forwarded
      // (no signature, no driver export, a non-guest pointer) keeps the counted no-op and is named once.
      if (gl_live && !wants_a_string && rb::launcher::GlForward::Known(name)) {
        std::string why;
        if (gl_forward.Call(name, s, *mem, &why)) return true;
        gl_forward_fail.emplace(name, why);
      }
      const bool parsed_by_the_game = s.r[0] == kGlVendor || s.r[0] == kGlRenderer || s.r[0] == kGlVersion;
      s.r[0] = wants_a_string ? (parsed_by_the_game ? gl_strings : gl_empty) : 0u;
      return true;
    }, abi::AbiContext::SlotLock::kHoldsGgl);
    return ctx.SlotFor(slot);
  };

  /// The dynamic-linker interface and liblog (`host_slots.h`), bound now: libc's own constructors call
  /// into the first (netd's `dlopen`), and the game's log lines land in this report through the second.
  HostSlots slots(*proc, /*target_sdk=*/34);
  /// Which bucket the app's non-scope imports landed in — read only while the app is loading, so the
  /// scope's own (libc's dlopen) are not counted as the app's.
  bool app_loading = false;
  std::vector<std::string> gl_answered, host_answered;
  const auto record = [](std::vector<std::string>& list, const std::string& name) {
    for (const std::string& seen : list) {
      if (seen == name) return;
    }
    list.push_back(name);
  };
  host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    std::optional<std::uint32_t> answer;
    if (IsGlName(name)) answer = gl_slot_for(name);
    if (answer) {
      if (app_loading) record(gl_answered, name);
      return answer;
    }
    answer = slots.Resolve(name);
    if (answer && app_loading) record(host_answered, name);
    return answer;
  };

  const abi::GuestAddr env_ptr = jni.ok() ? jni.env() : 0;
  if (jni.ok()) {
    Say(&out, "env: a fake JNIEnv at 0x%s — %u table entries of host-call slots (0..%u) and a string arena,"
              " all guest memory. It is RetroBug's, not a JavaVM's: an entry with no implementation refuses"
              " by name instead of branching to address 0.",
        Hex(env_ptr).c_str(), abi::jni::kTableEntries, abi::jni::kTableEntries - 1u);
  } else {
    Say(&out, "env: the guest process's JNIEnv is not usable — a guest JNI method is called with env=0");
  }

  /**
   * The one file the syscall monitor serves: the APK the entry point is handed, because the game opens it
   * to read its `assets/`. Read once, here, so a failure is a line in this report rather than an ENOENT
   * about a file that is sitting right there. Every other path is answered ENOENT, by name.
   */
  std::optional<std::vector<std::uint8_t>> apk_file;
  if (apk_path.empty()) {
    Say(&out, "file: no APK path arrived from the Java half, so the provider below has nothing to serve and"
              " every openat is answered ENOENT by name");
  } else {
    std::string read_err;
    apk_file = ReadHostFile(apk_path, &read_err);
    if (apk_file) {
      Say(&out, "file: %s (%zu bytes on the host) is the one path this run serves — the path the entry"
                " point is handed and the guest's openat of it are the same file",
          apk_path.c_str(), apk_file->size());
    } else {
      Say(&out, "file: %s could not be read (%s) — the guest's openat of it comes back ENOENT and this line"
                " says why",
          apk_path.c_str(), read_err.c_str());
    }
  }
  // By reference: the provider is consulted only from inside a guest call this function makes, and
  // `apk_file` outlives every one of them; the path is captured by value.
  monitor.set_file_provider([&apk_file, apk_path](const std::string& path)
                                -> std::optional<std::vector<std::uint8_t>> {
    if (path != apk_path || !apk_file) return std::nullopt;
    return apk_file;
  });

  /**
   * The class model's one Java class: the game's renderer, which is where KA3D gets its files.
   *
   * Angry Birds' `io::FileInputStream` does not open anything itself — it calls back into Java:
   * `GetObjectClass(renderer)`, `GetMethodID("readFile", "(Ljava/lang/String;)[B")`, then
   * `CallObjectMethodV` and copies the byte[] out (libangrybirds +0xf0bfc..0xf0ce4). The first run with
   * a budget large enough to reach it stopped there, on `GetObjectClass`. So the stand-in page the entry
   * point is handed as both `this` and `MyRenderer` is bound to `com/rovio/ka3d/MyRenderer`, and
   * `readFile` is answered from the APK this run already holds — transcribed from the game's own DEX,
   * not guessed:
   *
   *     readFile(n):   try readFileCompressed(n.endsWith(".zip") ? n : n + ".zip")
   *                    catch -> try readFileUncompressed(n) catch -> null
   *     readFileCompressed(z):   ZipInputStream(assets.open(z)).getNextEntry(), read to the end
   *     readFileUncompressed(n): assets.open(n), read available() bytes
   *
   * `assets.open(p)` is the APK entry `assets/p`. Every request is recorded and reported, found or not;
   * a miss returns the null the Java returns, which KA3D turns into its own FileNotFoundException.
   * Any *other* method the game asks the renderer for still refuses by name in `GetMethodID`.
   */
  struct FileAsk {
    std::string name, served, why;
    std::size_t bytes = 0;
    std::string callers;  ///< a stack scan's return addresses, for a request the name does not explain
  };
  /// The game module, once placed — the stack scan below names return addresses inside it.
  const loader::GuestModule* scan_mod = nullptr;
  /// A heuristic backtrace with no frame pointers to follow: words on the guest stack that point into
  /// the game module just after a call — `bl`, `blx reg`, or the `ldr pc, [...]` half of the
  /// `mov lr, pc; ldr pc, [...]` pair this 2009 compiler emits for virtual and JNI calls — each named by
  /// the nearest export at or below it. A stale word that passes is possible, which is why the report
  /// calls these candidates; the innermost few are reliably the live frames.
  const auto scan_callers = [&mem, &scan_mod](const exec::MachineState& s) {
    std::string out;
    if (scan_mod == nullptr) return out;
    const std::uint32_t lo = scan_mod->base, hi = scan_mod->base + scan_mod->extent;
    unsigned found = 0;
    for (std::uint32_t at = s.r[13]; at < s.r[13] + 4096u && found < 10; at += 4) {
      std::uint32_t w = 0, insn = 0;
      if (!mem->Read(at, &w, 4) || (w & 3u) != 0 || w < lo + 4 || w >= hi) continue;
      if (!mem->Read(w - 4, &insn, 4)) continue;
      const bool bl = (insn & 0x0F00'0000u) == 0x0B00'0000u;
      const bool blx_reg = (insn & 0x0FFF'FFF0u) == 0x012F'FF30u;
      const bool ldr_pc = (insn & 0x0C50'F000u) == 0x0410'F000u;
      if (!bl && !blx_reg && !ldr_pc) continue;
      const loader::Symbol* best = nullptr;
      for (const loader::Symbol& sym : scan_mod->exports) {
        if (!sym.defined || !sym.is_function) continue;
        const std::uint32_t v = sym.value & ~1u;
        if (v <= w && (best == nullptr || v > (best->value & ~1u))) best = &sym;
      }
      char line[64];
      std::snprintf(line, sizeof line, "%s+0x%x", found == 0 ? "" : " <- ", w - lo);
      out += line;
      if (best != nullptr) out += " " + best->name + "+0x" + Hex(w - (best->value & ~1u));
      ++found;
    }
    return out;
  };
  // Destroyed before `jni`, whose stored method captures it — safe for the same reason the provider's
  // capture of `apk_file` is: the method only runs inside a guest call this function is making.
  std::vector<FileAsk> file_asks;
  bool renderer_bound = false;
  /// The name the report gives the game's file callback — `readFile` for KA3D, `dataFromFile` for Gumball.
  std::string file_callback = "readFile";
  // KA3D's renderer only for a KA3D image: the stand-in is one object, and binding it to MyRenderer for
  // every game would leave Gumball's JNIGLSurface calls landing on the wrong class.
  const bool is_ka3d = ImageHasString(app_image, "Java_com_rovio_ka3d_MyRenderer_");
  if (jni.ok() && stand_in != 0 && is_ka3d) {
    std::string cerr;
    const abi::GuestAddr renderer = jni.DefineHostClass("com/rovio/ka3d/MyRenderer", &cerr);
    const auto read_file = [&apk_file, &file_asks, &scan_callers](abi::JniEnv& env,
                                                                  const std::vector<std::uint64_t>& a,
                                                                  std::uint64_t& ret, std::string* e) {
      std::string name;
      if (a.size() != 1 || !env.StringText(static_cast<abi::GuestAddr>(a[0]), name, e)) return false;
      FileAsk ask{name, {}, {}, 0, {}};
      // An empty name is a request the name cannot explain; who asked is the next question.
      if (name.empty() && env.calling_state() != nullptr) ask.callers = scan_callers(*env.calling_state());
      ret = 0;  // null: what readFile returns when both of its tries throw
      if (!apk_file) {
        ask.why = "the APK could not be read on the host";
        file_asks.push_back(std::move(ask));
        return true;
      }
      const bool zipped = name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
      const std::string zname = zipped ? name : name + ".zip";
      std::optional<std::vector<std::uint8_t>> bytes;
      std::string why;
      if (auto inner = ZipEntry(*apk_file, "assets/" + zname, &why)) {
        // ZipInputStream over something that is not a zip yields no entry and reads -1 at once, so the
        // Java returns an empty byte[] rather than throwing; the same holds here.
        bytes = ZipEntry(*inner, "", &why);
        if (!bytes) bytes.emplace();
        ask.served = "assets/" + zname + " (its first entry)";
      } else if ((bytes = ZipEntry(*apk_file, "assets/" + name, &why))) {
        ask.served = "assets/" + name;
      } else {
        ask.why = why;
      }
      if (bytes) {
        ask.bytes = bytes->size();
        ret = env.NewHostByteArray(std::move(*bytes));
        if (ret == 0) {
          if (e != nullptr) *e = "readFile(" + name + "): the byte[] handle space is exhausted";
          return false;
        }
      }
      file_asks.push_back(std::move(ask));
      return true;
    };
    renderer_bound = renderer != 0 &&
                     jni.DefineHostMethod(renderer, "readFile", "(Ljava/lang/String;)[B", read_file, &cerr) &&
                     jni.BindHostObject(stand_in, renderer, &cerr);
    if (renderer_bound) {
      Say(&out, "java: the stand-in object 0x%s is bound to com/rovio/ka3d/MyRenderer in the class model,"
                " with one host method — readFile(Ljava/lang/String;)[B, transcribed from the game's DEX and"
                " answered from %s. Any other method asked of it refuses by name.",
          Hex(stand_in).c_str(), apk_file ? "this APK's assets/" : "nothing (the APK was not read)");
    } else {
      Say(&out, "java: the renderer class could not be set up (%s) — GetObjectClass on the stand-in will"
                " refuse by name", cerr.c_str());
    }
  }

  /**
   * The scope — libc, libm, libz, and libstdc++ only when the app's DT_NEEDED names it (Angry Birds needs
   * none; Gumball 2013 does) — booted by the guest process: IFUNC slots resolved during each load, the
   * linker's shared globals and auxv published, bionic's real main thread built. Its report lines (loads,
   * IFUNC slots, the bionic layout, the thread) arrive through the process's log above.
   */
  const std::vector<std::string> app_needs = loader::NeededSonames(app_image);
  const bool wants_libstdcxx =
      std::find(app_needs.begin(), app_needs.end(), "libstdc++.so") != app_needs.end();
  if (wants_libstdcxx && libstdcxx_image.empty()) {
    Say(&out, "scope: %s needs libstdc++.so and this APK carries no such asset — its imports from it are"
              " trapped by name", label.c_str());
  }
  if (!proc->LoadScope(libc_image, libm_image, libz_image,
                       wants_libstdcxx ? libstdcxx_image : std::vector<std::uint8_t>{}, &err)) {
    Say(&out, "FATAL: %s", err.c_str());
    Say(&out, "RESULT: stopped before the app — the guest scope did not boot (the line above says why)");
    return out;
  }
  /// Scope imports the host could not answer, so the report can NAME them (a libc constructor can be
  /// their first caller — `netdClientInitImpl` called `dlopen` on 2026-10-09).
  const std::vector<std::string> scope_unanswered = proc->unanswered();

  /**
   * The app, against the scope. Every import is answered from the scope, then by the host (GL, dl*,
   * liblog), and otherwise bound to the trap — the buckets are recorded as the answers are given.
   */
  app_loading = true;
  const loader::GuestModule* app_mod = proc->LoadApp(app_image, label, &err);
  app_loading = false;
  if (app_mod == nullptr) {
    Say(&out, "FATAL: the loader refused %s: %s", label.c_str(), err.c_str());
    Say(&out, "  (a refusal names every slot it could not fill; a library built against a libc RetroBug"
              " does not ship yet is the expected shape of this)");
    return out;
  }
  const loader::GuestModule& app = *app_mod;
  std::vector<std::string> trapped(proc->unanswered().begin() + static_cast<std::ptrdiff_t>(scope_unanswered.size()),
                                   proc->unanswered().end());
  std::vector<std::string> answered;
  for (const loader::Symbol& sym : app.imports) {
    const auto in = [&](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), sym.name) != v.end(); };
    if (!in(gl_answered) && !in(host_answered) && !in(trapped)) record(answered, sym.name);
  }
  std::size_t defined_exports = 0;
  for (const auto& sym : app.exports) {
    if (sym.defined) ++defined_exports;
  }
  Say(&out, "loaded: %s at 0x%s extent 0x%s, %zu defined exports, %zu recorded dynsym imports",
      app.soname.empty() ? label.c_str() : app.soname.c_str(), Hex(app.base).c_str(),
      Hex(app.extent).c_str(), defined_exports, app.imports.size());
  scan_mod = &app;
  Say(&out, "unwind: %s's .ARM.exidx is %s — what dl_unwind_find_exidx answers for a pc inside it, so a"
            " C++ throw can find its catch",
      app.soname.empty() ? label.c_str() : app.soname.c_str(),
      app.exidx != 0 ? ("at 0x" + Hex(app.exidx) + ", " + std::to_string(app.exidx_count) + " entries").c_str()
                     : "absent (a throw through this module will terminate)");
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

  /**
   * The constructors: the scope's in load order, then the app's — `DT_INIT` then `DT_INIT_ARRAY`, as
   * bionic's `soinfo::call_constructors` — on the constructor tier, once. The sequence stops at the first
   * that does not return (a half-initialized module must not look loaded) and the process names it.
   */
  std::string ctor_failed;  // the first constructor that did not return, for the RESULT line
  const bool ctors_ok = proc->RunConstructors(&ctor_failed);
  // The app's own tags are the point of running these, so "it had none" is its own line.
  if (ctors_ok && app.init_array.empty() && app.init.addr == 0) {
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
  Say(&out, "imports: %zu of %zu answered by the guest scope, %zu by the host's GL set (below), %zu by"
            " other host slots (dl*, liblog), %zu still trapped at 0x%s",
      answered.size(), answered.size() + gl_answered.size() + host_answered.size() + trapped.size(),
      gl_answered.size(), host_answered.size(), trapped.size(), Hex(kUnresolved).c_str());

  // One place that writes the trap list, so the exits below cannot drift apart. `Say` is variadic and
  // takes a `const char*`, so the note is built as a string first: a ternary mixing `""` with a
  // concatenation hands the variadic call a `std::string` and clang refuses it.
  const auto say_trapped = [&]() {
    rb::runtime::Ggl::Held g(ctx.ggl());  // unanswered-import and trap state are GGL-protected
    // The scope's own unanswered imports first, and before the early return below: a *constructor* can
    // be their first caller (2026-10-09: libc's `netdClientInitImpl` called `dlopen`'s trapped slot),
    // and the shipping case has the app's list empty — an earlier revision of this block sat after the
    // `trapped.empty()` return and never ran, which is precisely the "named trap with no name" this
    // exists to prevent. Names, not a count: the count is what left that fault anonymous.
    if (!scope_unanswered.empty()) {
      std::string names;
      for (const std::string& one : scope_unanswered) names += (names.empty() ? "" : ", ") + one;
      Say(&out, "scope imports the host did not answer (bound to 0x%s): %s", Hex(kUnresolved).c_str(),
          names.c_str());
    }
    if (trapped.empty()) {
      Say(&out, "imports the guest scope did not answer: none");
      return;
    }
    // Weak references are a different fact from missing ones: a device's linker binds an unresolved weak
    // import to 0 and the app runs, because nothing calls it unless a feature is present. Gumball's three
    // (`__cxa_call_unexpected`, `__cxa_begin_cleanup`, `__cxa_type_match`) are weak on the device too, and
    // listing them as faults-in-waiting at the end of a successful run read as the run having failed.
    std::vector<std::string> strong, weak;
    for (const std::string& name : trapped) {
      bool is_weak = false;
      for (const std::string& w : app.weak_imports) {
        if (w.substr(0, w.find('@')) == name.substr(0, name.find('@'))) is_weak = true;
      }
      (is_weak ? weak : strong).push_back(name);
    }
    if (!strong.empty()) {
      Say(&out, "imports the guest scope did not answer: %zu (a call into one faults at 0x%s)", strong.size(),
          Hex(kUnresolved).c_str());
      for (std::size_t i = 0; i < strong.size() && i < 12; ++i) Say(&out, "  %s", strong[i].c_str());
      if (strong.size() > 12) Say(&out, "  ... and %zu more", strong.size() - 12);
    }
    if (!weak.empty()) {
      std::string names;
      for (const std::string& n : weak) names += (names.empty() ? "" : ", ") + n;
      Say(&out, "weak imports nothing defines: %zu (%s) — unbound, as a device's linker leaves them; only a"
                " call would matter, and it would fault at 0x%s by name",
          weak.size(), names.c_str(), Hex(kUnresolved).c_str());
    }
  };

  /// The dl* set's tally and the game's own log, in its own words — both `HostSlots`' (host_slots.h).
  const auto say_line = [&out](const std::string& s) { Say(&out, "%s", s.c_str()); };
  const auto say_dl = [&]() {
    rb::runtime::Ggl::Held g(ctx.ggl());  // dl slot counters are written under the GGL
    slots.ReportDl(say_line, kUnresolved);
  };
  const auto say_guest_log = [&]() {
    rb::runtime::Ggl::Held g(ctx.ggl());  // the log record is written by the log slot under the GGL
    slots.ReportLog(say_line);
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
  /// What the game asked its Java side for: each host method's tally, then every readFile request with
  /// the APK entry that answered it or the reason it came back null.
  const auto say_java = [&]() {
    rb::runtime::Ggl::Held g(ctx.ggl());  // JniEnv records are GGL-protected
    if (!renderer_bound) return;
    for (const auto& u : jni.host_method_uses()) {
      Say(&out, "java: %s.%s%s called x%u (%u refused)", u.cls.c_str(), u.name.c_str(), u.sig.c_str(),
          u.calls, u.refused);
    }
    std::size_t found = 0, bytes = 0;
    for (const FileAsk& f : file_asks) {
      if (!f.served.empty()) {
        ++found;
        bytes += f.bytes;
      }
    }
    Say(&out, "java: %s was asked for %zu file(s): %zu served (%zu bytes copied to the guest through"
              " GetByteArrayRegion), %zu returned null or empty. byte[]s made %u, deleted %u, still live %zu.",
        file_callback.c_str(), file_asks.size(), found, bytes, file_asks.size() - found, jni.arrays_made(), jni.arrays_freed(),
        jni.arrays_live());
    std::size_t shown = 0;
    for (const FileAsk& f : file_asks) {
      if (shown == 40) {
        Say(&out, "  ... and %zu more", file_asks.size() - shown);
        break;
      }
      if (f.served.empty()) {
        Say(&out, "  %s(\"%s\") -> nothing: %s", file_callback.c_str(), f.name.c_str(), f.why.c_str());
        if (!f.callers.empty()) {
          Say(&out, "    asked from (stack-scan candidates, innermost first): %s", f.callers.c_str());
        }
      } else {
        Say(&out, "  %s(\"%s\") -> %zu bytes from %s", file_callback.c_str(), f.name.c_str(), f.bytes, f.served.c_str());
      }
      ++shown;
    }
  };

  const auto say_gl = [&]() {
    rb::runtime::Ggl::Held g(ctx.ggl());  // GL counters/failures are updated by GL slots under the GGL
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
    // Most-called first: in table order the first dozen were mostly x0 and the calls that happened sat
    // in "and 41 more", which is the opposite of what a reader of a deeper run needs.
    std::vector<const GlEntry*> order;
    for (const GlEntry& e : gl_calls) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(),
                     [](const GlEntry* x, const GlEntry* y) { return x->calls > y->calls; });
    std::size_t shown = 0;
    for (const GlEntry* e : order) {
      if (shown == 12) {
        Say(&out, "  ... and %zu more, whose calls are in the total above", gl_calls.size() - shown);
        break;
      }
      Say(&out, "  %s x%u", e->name.c_str(), e->calls);
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
    rb::runtime::Ggl::Held g(ctx.ggl());  // monitor tables are GGL-protected; cloned game threads may run
    const std::string tally = monitor.TallyText();
    if (tally.empty()) {
      Say(&out, "syscalls: none were made — the run stopped before guest code asked the OS for anything");
      return;
    }
    Say(&out, "syscalls: %s", tally.c_str());
    // The tally counts; it cannot say whether a call was served or refused, and a report that lists
    // `openat x3` without that is the half-story the monitor's own notes exist to complete. They were
    // recorded on every refusal and read by nobody in production — only by the monitor's unit test — so
    // until this loop existed, a device report could not explain a single refusal.
    const std::vector<std::string>& notes = monitor.notes();
    for (const std::string& note : notes) {
      Say(&out, "syscall note: %s", note.c_str());
    }
    if (monitor.dropped_notes() != 0) {
      Say(&out, "syscall note: %zu further distinct refusals were recorded past the %zu-note cap and are"
                " counted, not listed",
          monitor.dropped_notes(), syscall::Monitor::kMaxNotes);
    }
    // The guest's own console output, which used to be dropped on the floor: fd 1/2 writes are captured
    // by the monitor now, and a report that stops at "exit_group(127)" without it would be hiding the
    // only words the guest spoke — measured, not imagined (the app's constructor printed to fd 2 four
    // times before exiting 127, and every byte went nowhere until this loop existed).
    const auto say_guest_output = [&](const char* stream, const std::string& text) {
      if (text.empty()) return;
      std::size_t begin = 0;
      while (begin < text.size()) {
        const std::size_t nl = text.find('\n', begin);
        const std::size_t end = nl == std::string::npos ? text.size() : nl;
        std::string line;
        AppendSanitized(&line, text.substr(begin, end - begin).c_str());
        if (!line.empty()) Say(&out, "guest %s: %s", stream, line.c_str());
        if (nl == std::string::npos) break;
        begin = nl + 1;
      }
      if (monitor.capture_dropped()) {
        Say(&out, "guest %s: (capture capped at %zu bytes — the overflow is counted, not shown)", stream,
            syscall::Monitor::kCaptureCap);
      }
    };
    say_guest_output("stdout", monitor.captured_stdout());
    say_guest_output("stderr", monitor.captured_stderr());
  };

  // A constructor that did not return leaves the module half-initialized, so its entry points are not
  // called: a real runtime would not be entering this library either. Named rather than silent, because
  // "why is there no JNI_OnLoad line" is the first question a reader of this report will have.
  if (!ctors_ok) {
    Say(&out, "JNI_OnLoad: not called — a constructor above did not return, so the library is not in a"
              " state a runtime would enter");
    Say(&out, "RESULT: stopped during startup — constructor %s did not return; the library's own code"
              " ran up to there, and the line above it names what this build is missing",
        ctor_failed.empty() ? "?" : ctor_failed.c_str());
    say_guest_log();
    say_gl();
    say_dl();
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
       * Gumball (twentysixdigital, 2013): driven the way its own `JNIGLSurface` (a GLSurfaceView.Renderer)
       * drives it — transcribed from the APK's DEX, not guessed:
       *
       *     onSurfaceCreated -> jniClearTextures()
       *     onSurfaceChanged -> jniSurfaceChanged(w, h)
       *     onDrawFrame      -> [first frame] jniInit(startState = 0, GumballActivity.onCreate's value)
       *                         runRunnables()          (async textures land here)
       *                         jniRun(dt seconds, capped at 0.25; 0 on the first frame)
       *                         one queued texture loaded (loadTextureImage on the GL thread)
       *                         jniAcceleration(ax, ay, az)
       *
       * Every method is an instance method on the surface, so `this` is the stand-in, bound below to
       * `com/twentysixdigital/gumballjava/JNIGLSurface` with the 15 methods the library names in its own
       * strings. Files come from the APK's assets/ exactly as `Databank.dataFromFile` reads them (an empty
       * byte[] when absent, as the Java returns). Textures are the one place a GPU would do work: with no GL
       * behind this build, `queueTexture` reads the image's size from its header (the `@2x` variant first,
       * halved, as `loadTextureImage` does; a 128x128 placeholder when neither exists, as it does too),
       * hands out a texture id, and calls `jniTextureReady(name, id, w, h)` — synchronously, as a nested
       * guest call, when the game asked for a synchronous load, and before the next jniRun otherwise.
       */
      if (app.FindExport("Java_com_twentysixdigital_gumballjava_JNIGLSurface_jniInit") && jni.ok() &&
          stand_in != 0) {
        constexpr const char* kSurface = "Java_com_twentysixdigital_gumballjava_JNIGLSurface_";
        const auto entry_name = [&](const char* m) { return std::string{kSurface} + m; };
        file_callback = "dataFromFile";
        // The renderer: a GLES1 context on the launcher's window, current on this thread for the whole
        // profile, so the game's GL calls reach the driver and each frame is presented. No window (a
        // scripted or headless run) or no context leaves the counted no-ops in place, said so here.
        std::unique_ptr<rb::launcher::EglTarget> egl;
        if (render.window != nullptr) {
          egl = std::make_unique<rb::launcher::EglTarget>(render.window);
          if (egl->ok()) {
            gl_live = true;
            Say(&out, "render: a GLES1 context on the launcher's window, %dx%d — the game's GL calls go to this"
                      " device's driver, textures are decoded and uploaded for real, and each frame is presented",
                egl->width(), egl->height());
          } else {
            Say(&out, "render: no GLES1 context (%s) — the GL calls stay counted no-ops", egl->error().c_str());
            egl.reset();
          }
        } else {
          Say(&out, "render: no window for this run (headless) — the GL calls are counted, not drawn");
        }
        std::string cerr;
        const abi::GuestAddr surface = jni.DefineHostClass("com/twentysixdigital/gumballjava/JNIGLSurface", &cerr);

        struct TexAsk {
          std::string name, served, why;
          std::uint32_t id = 0, w = 0, h = 0;
          bool raw = false, async = false, delivered = false, uploaded = false;
          exec::StopReason delivery = exec::StopReason::Returned;
        };
        std::vector<TexAsk> textures;
        std::vector<std::size_t> pending;  // async textures waiting for the next frame boundary
        std::map<std::string, std::vector<std::uint8_t>> databank;  // this run's saves, in memory
        struct Noted {
          std::string name;
          unsigned calls = 0;
          std::string last;
        };
        std::vector<Noted> noted;  // the no-op callbacks: what the game asked for, counted
        std::uint32_t next_texture = 1;
        const auto asset = [&](const std::string& path) -> std::optional<std::vector<std::uint8_t>> {
          if (!apk_file) return std::nullopt;
          std::string why;
          return ZipEntry(*apk_file, "assets/" + path, &why);
        };
        // By index, not reference: the nested jniTextureReady may queue more textures and reallocate.
        const auto deliver = [&](std::size_t idx) {
          TexAsk t = textures[idx];
          // Ruling iv (M2 Task 4): the name is written into guest memory and the env called under the GGL —
          // once guest threads exist, one may be running (and remapping) while this host code runs.
          rb::runtime::Ggl::Held guest_lock(ctx.ggl());
          // jniTextureReady(String name, int glTexture, int w, int h) on the surface object; the raw form
          // would hand RGBA pixels this build does not decode, so it is reported rather than faked.
          if (t.raw) {
            textures[idx].why = "raw (pixel) delivery needs a PNG decoder this build does not have — not delivered";
            return;
          }
          exec::RunResult rr{};
          const auto fn = app.FindExport(entry_name("jniTextureReady"));
          if (!fn) return;
          std::string err2;
          // The name travels as a jstring made by the env itself, so the game's GetStringUTFChars works.
          const abi::GuestAddr jname = [&]() -> abi::GuestAddr {
            const abi::GuestAddr scratch = scratch_at;  // a guest page the launcher owns
            std::string n = t.name;
            if (n.size() + 1 > kGuestPage) return 0;
            if (!mem->Write(scratch, n.c_str(), n.size() + 1)) return 0;
            exec::RunResult mk{};
            const abi::Ret32 made = ctx.CallGuest(ctx.SlotFor(abi::jni::kNewStringUTF),
                                                  {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(scratch)},
                                                  abi::Shorty::For("LL"), kCallBudget, &mk);
            return mk.reason == exec::StopReason::Returned ? made.w() : 0;
          }();
          ctx.CallGuest(fn->tagged(),
                        {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(stand_in), abi::Value32::Ptr(jname),
                         abi::Value32::S32(static_cast<std::int32_t>(t.id)),
                         abi::Value32::S32(static_cast<std::int32_t>(t.w)),
                         abi::Value32::S32(static_cast<std::int32_t>(t.h))},
                        abi::Shorty::For("V"), kCallBudget * 5, &rr);
          textures[idx].delivered = true;
          textures[idx].delivery = rr.reason;
          if (rr.reason != exec::StopReason::Returned) textures[idx].why = "jniTextureReady stopped: " + rr.detail;
        };
        const auto method = [&](const char* name, const char* sig, abi::JniEnv::HostMethod fn) {
          if (surface != 0 && !jni.DefineHostMethod(surface, name, sig, std::move(fn), &cerr)) {
            Say(&out, "java: could not define JNIGLSurface.%s%s: %s", name, sig, cerr.c_str());
          }
        };
        const auto text_arg = [&](abi::JniEnv& env, std::uint64_t h) {
          std::string s, e;
          if (h == 0) return std::string{"null"};
          return env.StringText(static_cast<abi::GuestAddr>(h), s, &e) ? "\"" + s + "\"" : "<" + e + ">";
        };
        const auto note = [&](const char* name) {
          return [&noted, &text_arg, name](abi::JniEnv& env, const std::vector<std::uint64_t>& a,
                                           std::uint64_t& ret, std::string*) {
            Noted* n = nullptr;
            for (Noted& x : noted) {
              if (x.name == name) n = &x;
            }
            if (n == nullptr) {
              noted.push_back(Noted{name, 0, {}});
              n = &noted.back();
            }
            ++n->calls;
            // The first String argument (if any) is the useful part of a sound or text command.
            n->last.clear();
            for (const std::uint64_t v : a) {
              const std::string t = text_arg(env, v);
              if (!t.empty() && t[0] == '"') {
                n->last = t;
                break;
              }
            }
            ret = 0;
            return true;
          };
        };
        method("dataFromFile", "(Ljava/lang/String;)[B",
               [&](abi::JniEnv& env, const std::vector<std::uint64_t>& a, std::uint64_t& ret, std::string* e) {
                 std::string name;
                 if (a.size() != 1 || !env.StringText(static_cast<abi::GuestAddr>(a[0]), name, e)) return false;
                 FileAsk ask{name, {}, {}, 0, {}};
                 auto bytes = asset(name);
                 if (bytes) {
                   ask.served = "assets/" + name;
                   ask.bytes = bytes->size();
                 } else {
                   ask.why = apk_file ? "no entry assets/" + name + " — Databank returns an empty byte[]"
                                      : "the APK was not read";
                   bytes.emplace();  // `new byte[0]`, what Databank.dataFromFile returns on an exception
                 }
                 ret = env.NewHostByteArray(std::move(*bytes));
                 file_asks.push_back(std::move(ask));
                 return ret != 0;
               });
        method("databankRead", "(Ljava/lang/String;)[B",
               [&](abi::JniEnv& env, const std::vector<std::uint64_t>& a, std::uint64_t& ret, std::string* e) {
                 std::string name;
                 if (a.size() != 1 || !env.StringText(static_cast<abi::GuestAddr>(a[0]), name, e)) return false;
                 const auto it = databank.find(name);
                 ret = it == databank.end() ? 0 : env.NewHostByteArray(it->second);  // null: no save yet
                 return true;
               });
        method("databankWrite", "(Ljava/lang/String;[B)V",
               [&](abi::JniEnv& env, const std::vector<std::uint64_t>& a, std::uint64_t& ret, std::string* e) {
                 std::string name;
                 if (a.size() != 2 || !env.StringText(static_cast<abi::GuestAddr>(a[0]), name, e)) return false;
                 const auto bytes = env.HostArray(static_cast<abi::GuestAddr>(a[1]));
                 databank[name] = bytes != nullptr ? *bytes : std::vector<std::uint8_t>{};
                 ret = 0;
                 return true;
               });
        method("queueTexture", "(Ljava/lang/String;ZZ)V",
               [&](abi::JniEnv& env, const std::vector<std::uint64_t>& a, std::uint64_t& ret, std::string* e) {
                 ret = 0;
                 if (a.size() != 3) return false;
                 if (a[0] == 0) return true;  // "Null texture name requested!" — logged and ignored by the Java
                 TexAsk t;
                 if (!env.StringText(static_cast<abi::GuestAddr>(a[0]), t.name, e)) return false;
                 t.raw = a[1] != 0;
                 t.async = a[2] != 0;
                 std::uint32_t w = 0, h = 0;
                 bool sized = false;
                 std::optional<std::vector<std::uint8_t>> chosen;  // the file a real load would decode
                 if (t.name.find("@2x") == std::string::npos) {
                   const std::size_t dot = t.name.rfind('.');
                   const std::string hi = dot == std::string::npos
                                              ? t.name + "@2x"
                                              : t.name.substr(0, dot) + "@2x" + t.name.substr(dot);
                   if (auto b = asset(hi); b && ImageSize(*b, w, h)) {
                     w /= 2;
                     h /= 2;
                     sized = true;
                     chosen = std::move(b);
                     t.served = "assets/" + hi + " (halved, as the @2x branch does)";
                   }
                 }
                 if (!sized) {
                   if (auto b = asset(t.name); b && ImageSize(*b, w, h)) {
                     sized = true;
                     chosen = std::move(b);
                     t.served = "assets/" + t.name;
                   }
                 }
                 if (!sized) {
                   w = h = 128;  // loadTextureImage's placeholder bitmap
                   t.served = "a 128x128 placeholder (no readable assets/" + t.name + ")";
                 }
                 t.w = w;
                 t.h = h;
                 t.id = 0;
                 // With a live context the pixels are real: decoded and uploaded the way textureLoaded does
                 // (BitmapFactory, power-of-two scale, LINEAR / CLAMP_TO_EDGE, GLUtils.texImage2D), and the
                 // game gets the driver's texture name. The logical size stays the one computed above.
                 if (gl_live && render.upload && chosen) {
                   std::uint32_t id = 0, bw = 0, bh = 0;
                   std::string why;
                   if (render.upload(*chosen, id, bw, bh, &why)) {
                     t.id = id;
                     t.uploaded = true;
                   } else {
                     t.why = "upload failed: " + why;
                   }
                 }
                 if (t.id == 0) t.id = (gl_live ? 0x10000u : 0u) + next_texture++;  // never a real GL name
                 textures.push_back(t);
                 if (t.async) {
                   pending.push_back(textures.size() - 1);
                 } else {
                   deliver(textures.size() - 1);  // nested: the Java loads it inside this very call
                 }
                 return true;
               });
        method("showAlert", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", note("showAlert"));
        method("soundCommand", "(Ljava/lang/String;I)V", note("soundCommand"));
        method("textCommand", "(IILjava/lang/String;IIIII)V", note("textCommand"));
        method("openBrowser", "(Ljava/lang/String;)V", note("openBrowser"));
        method("storeBuy", "(Ljava/lang/String;Ljava/lang/String;)V", note("storeBuy"));
        method("triggerShareInterface", "(I)V", note("triggerShareInterface"));
        method("thumbnailCommand", "(III)V", note("thumbnailCommand"));
        method("setThumbnailImage", "(II[B)V", note("setThumbnailImage"));
        method("chartboostShowInterstitial", "()V", note("chartboostShowInterstitial"));
        method("chartboostShowMoreGames", "()V", note("chartboostShowMoreGames"));
        method("pickPhoto", "()V", note("pickPhoto"));
        method("takePhoto", "()V", note("takePhoto"));
        renderer_bound = surface != 0 && jni.BindHostObject(stand_in, surface, &cerr);
        Say(&out, "java: the stand-in 0x%s is bound to com/twentysixdigital/gumballjava/JNIGLSurface with %zu"
                  " host methods (dataFromFile/databank from %s; queueTexture sized from image headers; sound,"
                  " text, alerts, store and sharing counted as no-ops)%s",
            Hex(stand_in).c_str(), jni.host_method_uses().size(),
            apk_file ? "this APK's assets/" : "nothing — the APK was not read",
            renderer_bound ? "" : (" — binding FAILED: " + cerr).c_str());

        const auto gl_total = [&]() {
          rb::runtime::Ggl::Held g(ctx.ggl());  // the GL slots update these counters under the GGL
          unsigned n = 0;
          for (const GlEntry& g : gl_calls) n += g.calls;
          return n;
        };
        // The entry calls run on the JIT unless the interpreter was asked for by name (tier setting 1): the
        // "both" default runs constructors on Tier I, and Gumball's frame loop there is 6 frames/s against
        // Tier J's 91.6 — the same game, scene switch on the same frame on both (2026-10-14). One shared
        // guest memory, so switching engines between calls is the bridge's ordinary per-call executor choice.
        std::string stop_note;  // the first call that did not return, for the verdict line
        exec::Executor& run_engine = (tiers != 1 && jit.ok()) ? static_cast<exec::Executor&>(jit) : ctor_engine;
        const char* run_tier = run_engine.tier() == exec::CodeTier::Interp ? "I" : "J";
        bool alive = true;
        const auto step = [&](const char* m, const char* shorty, abi::Args32 tail, unsigned budget) {
          abi::Args32 args = {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(stand_in)};
          for (const auto& v : tail) args.push_back(v);
          const Outcome got = Call(run_engine, ctx, app, entry_name(m).c_str(), shorty, args, budget);
          if (!got.ok()) {
            alive = false;
            stop_note = std::string{m} + " stopped (" + ReasonName(got.reason) + "): " + got.detail;
            Say(&out, "gumball: %s stopped: reason=%s steps=%u — %s", m, ReasonName(got.reason), got.steps,
                got.detail.c_str());
            Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s sp=0x%s", Hex(got.exit.pc).c_str(),
                Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(), Hex(got.exit.r[13]).c_str());
          }
          return got;
        };
        const auto said = [&](const char* m, const Outcome& o, unsigned gl_before) {
          if (o.ok()) {
            Say(&out, "gumball: %s returned, tier=%s steps=%u, %u GL call(s)", m, run_tier, o.steps,
                gl_total() - gl_before);
          }
        };
        unsigned g0 = gl_total();
        Outcome o = step("jniClearTextures", "V", {}, kCallBudget);
        said("jniClearTextures (onSurfaceCreated)", o, g0);
        if (alive) {
          g0 = gl_total();
          o = step("jniSurfaceChanged", "V",
                   {abi::Value32::S32(egl ? egl->width() : surface_width),
                    abi::Value32::S32(egl ? egl->height() : surface_height)},
                   kCallBudget);
          said("jniSurfaceChanged (onSurfaceChanged)", o, g0);
        }
        // Frames: as many as fit in the frame and wall-clock caps, each exactly onDrawFrame's sequence.
        // Headless: 1200 frames at a fixed 1/60 s (20 s of game time), as fast as the tier runs. Rendering:
        // real time, the way GLSurfaceView drives the Java side — dt is the wall time since the last frame,
        // capped at 0.25 s as onDrawFrame caps it, and eglSwapBuffers paces the loop to the display.
        const unsigned kFrames = gl_live ? 100000u : 1200u;
        // A drawn run is also a played one: its cap counts from the last touch, so someone tapping through
        // the menus keeps it going (to a hard 15 min), and an untouched run still ends after 60 s.
        const auto kWallCap = std::chrono::seconds(gl_live ? 60 : 90);
        const auto kHardCap = std::chrono::minutes(15);
        auto last_frame = std::chrono::steady_clock::now();
        const auto wall_start = std::chrono::steady_clock::now();
        auto idle_since = wall_start;
        // Touches: only a drawn run has a screen to touch. jniTouch(x, y, action) after jniRun, per frame.
        const bool touch_live = gl_live && app.FindExport(entry_name("jniTouch")).has_value();
        OpenTouches(touch_live);
        unsigned touch_counts[5] = {0, 0, 0, 0, 0};  // by mapped action: other, down, up, move, cancel
        unsigned touch_lines = 0;
        Say(&out, "input: %s", touch_live ? "touches on the run screen go to jniTouch(x, y, action) after each jniRun,"
                                            " mapped as JNIGLSurface.onTouchEvent maps them (x = getY, y = width - getX;"
                                            " down 1, up 2, move 3, cancel 4)"
                               : gl_live ? "the library has no jniTouch export — touches are not delivered"
                                         : "headless run — no screen to touch");
        unsigned frames = 0;
        // Scene switches, by class: the current scene object's vtable pointer minus 8 is the `_ZTV<class>`
        // symbol the game exports, so a switch can be named rather than shown as an address.
        const abi::GuestAddr scene_slot = app.base + 0xa5224u + 4u;  // Game::getCurrentGame's `current`
        const auto scene_name = [&](std::uint32_t obj) -> std::string {
          std::uint32_t vptr = 0;
          rb::runtime::Ggl::Held g(ctx.ggl());  // reads guest memory: other guest threads may be running
          if (obj == 0 || !mem->Read(obj, &vptr, 4)) return obj == 0 ? "none" : "unreadable";
          for (const loader::Symbol& s : app.exports) {
            if (s.defined && s.value == vptr - 8u && s.name.compare(0, 4, "_ZTV") == 0) {
              std::string n = s.name.substr(4);
              std::size_t i = 0;
              while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
              return n.substr(i);
            }
          }
          return "0x" + Hex(obj);
        };
        const auto frame_dt = [&]() -> float {
          if (!gl_live) return 1.0f / 60.0f;
          const auto now = std::chrono::steady_clock::now();
          const float dt = std::chrono::duration<float>(now - last_frame).count();
          last_frame = now;
          return dt > 0.25f ? 0.25f : dt;
        };
        std::uint32_t last_scene = 0;
        for (unsigned f = 0; alive && f < kFrames; ++f) {
          const auto now_wall = std::chrono::steady_clock::now();
          if (now_wall - idle_since > kWallCap || now_wall - wall_start > kHardCap) {
            Say(&out, "gumball: stopping after %u frame(s) — %s", frames,
                now_wall - wall_start > kHardCap
                    ? "the 15 min hard cap for a played run"
                    : (std::string{"the "} + std::to_string(kWallCap.count()) + " s" +
                       (touch_lines != 0 ? " since the last touch" : " wall-clock cap for this run"))
                          .c_str());
            break;
          }
          g0 = gl_total();
          if (f == 0) {
            o = step("jniInit", "V", {abi::Value32::S32(0)}, kEntryBudget);
            said("jniInit(startState=0) (first onDrawFrame)", o, g0);
            if (!alive) break;
          }
          // runRunnables: the async loads that completed since the last frame
          std::vector<std::size_t> due;
          due.swap(pending);
          for (const std::size_t i : due) deliver(i);
          const unsigned g1 = gl_total();
          const Outcome run_o =
              step("jniRun", "V", {abi::Value32::F32(f == 0 ? 0.0f : frame_dt())}, kCallBudget * 5);
          if (!alive) break;
          if (touch_live) {
            for (const QueuedTouch& t : TakeTouches()) {
              ++touch_counts[t.action >= 0 && t.action <= 4 ? t.action : 0];
              ++touch_lines;
              idle_since = std::chrono::steady_clock::now();
              std::uint32_t before = 0;
              { rb::runtime::Ggl::Held g(ctx.ggl()); mem->Read(scene_slot, &before, 4); }
              o = step("jniTouch", "V",
                       {abi::Value32::S32(t.x), abi::Value32::S32(t.y), abi::Value32::S32(t.action)}, kCallBudget);
              if (!alive) break;
              // Taps are the story of a played run; moves are counted, not listed.
              if (t.action != 3) {
                static const char* const kName[5] = {"other", "down", "up", "move", "cancel"};
                Say(&out, "input: frame %u — jniTouch(%d, %d, %s) in scene %s, steps=%u (view touch at %.0f, %.0f)",
                    f, t.x, t.y, kName[t.action >= 0 && t.action <= 4 ? t.action : 0], scene_name(before).c_str(),
                    o.steps, t.raw_x, t.raw_y);
              }
            }
            if (!alive) break;
          }
          o = step("jniAcceleration", "V",
                   {abi::Value32::F32(0.0f), abi::Value32::F32(0.0f), abi::Value32::F32(0.0f)}, kCallBudget);
          if (!alive) break;
          if (egl && !egl->Swap()) {  // presented; a failure means the surface is gone (screen left)
            stop_note = "presenting the frame failed: " + egl->error();
            Say(&out, "render: %s — stopping", stop_note.c_str());
            break;
          }
          ++frames;
          std::uint32_t now_scene = 0;
          { rb::runtime::Ggl::Held g(ctx.ggl()); mem->Read(scene_slot, &now_scene, 4); }
          if (f % 10 == 0) {  // the screen's live line; the report keeps only switches and samples
            SetProgress("gumball frame " + std::to_string(f) + " of " + std::to_string(kFrames) + " on tier " +
                        run_tier + " - scene " + scene_name(now_scene) + ", " + std::to_string(gl_total()) +
                        " GL calls so far");
          }
          if (now_scene != last_scene) {
            Say(&out, "gumball: frame %u — scene is now %s (game time %.2f s)", f, scene_name(now_scene).c_str(),
                f / 60.0);
            last_scene = now_scene;
          }
          if (f < 3 || f % 200 == 0) {
            // The texture list and the GL counters are filled by the game's host methods under the GGL, and
            // cloned game threads may be running now: read them under it (M2 Task 5 ruling f).
            unsigned gl_now = 0;
            std::size_t tex_now = 0;
            {
              rb::runtime::Ggl::Held g(ctx.ggl());
              gl_now = gl_total() - g1;
              tex_now = textures.size();
            }
            Say(&out, "gumball: frame %u — jniRun steps=%u, %u GL call(s) this frame, %zu texture(s) so far",
                f, run_o.steps, gl_now, tex_now);
          }
        }
        // The report from here on reads run state the GGL protects (monitor tables, JniEnv records, GL counters,
        // texture asks the game's host methods fill) while cloned game threads may still run: hold it to the end.
        rb::runtime::Ggl::Held report_lock(ctx.ggl());
        unsigned dropped = 0;
        {
          std::lock_guard<std::mutex> lock(g_touch_mu);
          dropped = g_touch_dropped;
        }
        OpenTouches(false);
        if (touch_live) {
          Say(&out, "input: %u touch(es) delivered — %u down, %u up, %u move, %u cancel, %u other; %u dropped by the"
                    " game's own jab throttle",
              touch_lines, touch_counts[1], touch_counts[2], touch_counts[3], touch_counts[4], touch_counts[0],
              dropped);
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        Say(&out, "gumball: %u frame(s) driven through jniRun%s, %.1f s of wall time (%.1f frames/s on tier %s)",
            frames, alive ? "" : " before a stop (above)", wall, wall > 0 ? frames / wall : 0.0, run_tier);
        if (egl) {
          Say(&out, "render: %u GL call(s) forwarded to the driver and %u frame(s) presented; %zu GL name(s)"
                    " could not be forwarded and stayed no-ops%s",
              gl_forward.forwarded(), frames, gl_forward_fail.size(), gl_forward_fail.empty() ? "" : ":");
          for (const auto& [n, why] : gl_forward_fail) Say(&out, "  %s — %s", n.c_str(), why.c_str());
        }
        gl_live = false;  // the probes below call guest code; nothing after the frames should draw

        // The scene machine's state, read out of the guest. `Game::getCurrentGame` (libgumball +0x3234c)
        // keeps {next, current} at +0xa5224 (decoded from its own pc-relative literals) and only switches
        // when `next->layout()` — the byte it caches at next+0x7a — reports ready; GumballSplash waits there
        // for its image (`mm_splash_cn.png`, index next+0x84). These are the facts that say why a frame
        // draws nothing. `checkUnclaimedTexturesFor` is the game's own by-name lookup of delivered textures.
        {
          rb::runtime::Ggl::Held guest_lock(ctx.ggl());  // guest reads/writes + calls below (ruling i/iv)
          const abi::GuestAddr scene = app.base + 0xa5224u;
          std::uint32_t next = 0, current = 0;
          mem->Read(scene, &next, 4);
          mem->Read(scene + 4u, &current, 4);
          std::uint8_t ready = 0xff;
          std::uint32_t idx = 0xffffffffu;
          if (next != 0) {
            mem->Read(next + 0x7au, &ready, 1);
            mem->Read(next + 0x84u, &idx, 4);
          }
          std::string probe = "unavailable";
          const char kName[] = "mm_splash_cn.png";
          if (mem->Write(scratch_at, kName, sizeof kName)) {
            const Outcome c = Call(ctor_engine, ctx, app, "_Z25checkUnclaimedTexturesForPKc", "Z",
                                   {abi::Value32::Ptr(scratch_at)}, kCallBudget);
            probe = c.ok() ? ((c.bits & 0xffu) ? "true (delivered, unclaimed)" : "false")
                           : std::string{"stopped: "} + c.detail;
          }
          Say(&out, "gumball: scene state — next=0x%s (ready byte %u, image index %u), current=0x%s;"
                    " checkUnclaimedTexturesFor(\"mm_splash_cn.png\") = %s",
              Hex(next).c_str(), ready, idx, Hex(current).c_str(), probe.c_str());
          // jniTextureReady copies the name with strlen + new[] + strcpy through the game's PLT; the GOT
          // slots (JUMP_SLOT at +0x9ddf0 strlen, +0x9df74 strcpy, +0x9df18 strcmp) hold what this run
          // resolved them to. Calling them on a known string says whether the copy can be trusted.
          const auto via_got = [&](std::uint32_t got_off, const abi::Args32& args) -> std::string {
            std::uint32_t fn = 0;
            if (!mem->Read(app.base + got_off, &fn, 4) || fn == 0) return "no GOT entry";
            exec::RunResult rr{};
            ctx.set_executor(ctor_engine);
            const abi::Ret32 r = ctx.CallGuest(fn, args, abi::Shorty::For("I"), kCallBudget, &rr);
            if (rr.reason != exec::StopReason::Returned) return "stopped: " + rr.detail;
            return std::to_string(static_cast<std::int32_t>(r.w())) + " (via 0x" + Hex(fn) + ")";
          };
          const abi::GuestAddr dst = scratch_at + 0x100u;
          const std::uint8_t poison[32] = {0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
                                           0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
                                           0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee};
          mem->Write(dst, poison, sizeof poison);
          const std::string len = via_got(0x9ddf0u, {abi::Value32::Ptr(scratch_at)});
          const std::string cmp = via_got(0x9df18u, {abi::Value32::Ptr(scratch_at), abi::Value32::Ptr(scratch_at)});
          const std::string cpy = via_got(0x9df74u, {abi::Value32::Ptr(dst), abi::Value32::Ptr(scratch_at)});
          Say(&out, "gumball: libc through the game's GOT on \"%s\" (16 bytes): strlen=%s strcmp(self)=%s"
                    " strcpy returned %s and wrote \"%s\"",
              kName, len.c_str(), cmp.c_str(), cpy.c_str(), GuestStringOrEmpty(*mem, dst).c_str());
        }

        // What the game asked its Java side for, beyond files.
        std::size_t delivered = 0, sized = 0;
        for (const TexAsk& t : textures) {
          if (t.delivered && t.delivery == exec::StopReason::Returned) ++delivered;
          if (t.served.find("placeholder") == std::string::npos) ++sized;
        }
        std::size_t uploaded = 0;
        for (const TexAsk& t : textures) uploaded += t.uploaded ? 1 : 0;
        Say(&out, "java: queueTexture asked for %zu texture(s): %zu sized from the APK, %zu uploaded with real"
                  " pixels, %zu delivered through jniTextureReady%s",
            textures.size(), sized, uploaded, delivered,
            egl ? "" : " (headless: no GL to upload pixels to, so ids are tokens)");
        for (std::size_t i = 0; i < textures.size() && i < 30; ++i) {
          const TexAsk& t = textures[i];
          Say(&out, "  #%u %s %ux%u%s%s%s from %s%s%s", t.id, t.name.c_str(), t.w, t.h, t.async ? " async" : "",
              t.raw ? " raw" : "", t.uploaded ? " uploaded" : "", t.served.c_str(), t.why.empty() ? "" : " — ", t.why.c_str());
        }
        if (textures.size() > 30) Say(&out, "  ... and %zu more", textures.size() - 30);
        for (const Noted& n : noted) {
          Say(&out, "java: %s x%u (counted no-op)%s%s", n.name.c_str(), n.calls, n.last.empty() ? "" : ", last ",
              n.last.c_str());
        }
        say_java();
        say_guest_log();
        say_gl();
        say_dl();
        say_syscalls();
        say_trapped();
        // The last line, and the one a reader should be able to stop at: did it run, how far, and the one
        // thing this build does not do yet. Without it the report ended on a list of weak symbols and read
        // as a failure on a run that had reached the main menu without a fault.
        std::uint32_t final_scene = 0;
        { rb::runtime::Ggl::Held g(ctx.ggl()); mem->Read(scene_slot, &final_scene, 4); }
        if (stop_note.empty()) {
          if (egl) {
            Say(&out, "RESULT: ran to the end — %u frame(s) on tier %s, final scene %s, no fault, drawn on screen"
                      " (%u GL calls forwarded to this device's driver).",
                frames, run_tier, scene_name(final_scene).c_str(), gl_forward.forwarded());
          } else {
            Say(&out, "RESULT: ran to the end — %u frame(s) on tier %s, final scene %s, no fault. Nothing was"
                      " drawn: this run had no window, so the game's GL calls (%u) were counted, not rendered.",
                frames, run_tier, scene_name(final_scene).c_str(), gl_total());
          }
        } else {
          Say(&out, "RESULT: stopped after %u frame(s) in scene %s — %s", frames, scene_name(final_scene).c_str(),
              stop_note.c_str());
        }
        return out;
      }

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
        // The DEX declares apkPath as `Ljava/lang/String;`, and the game's own code takes that at its
        // word: nativeInit's first JNI call is `GetStringUTFChars(env, path, 0)` — measured on the
        // SM-X710, 2026-10-10, where the raw char* this call used to pass failed the arena check and
        // faulted 102 steps in. Mint a real handle through this environment's own NewStringUTF, so it
        // lands in the arena that the later checks validate against and GetStringUTFChars returns the
        // path text, exactly as a real Activity's String would.
        std::uint32_t entry_path = apk_path_guest;
        if (env_ptr != 0 && apk_path_guest != 0) {
          exec::RunResult mint{};
          exec::MachineState mint_exit{};
          const abi::Ret32 made =
              ctx.CallGuest(ctx.SlotFor(abi::jni::kNewStringUTF),
                            {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(apk_path_guest)},
                            abi::Shorty::For("LL"), kCallBudget, &mint, &mint_exit);
          if (mint.reason == exec::StopReason::Returned && made.w() != 0) {
            entry_path = made.w();
          } else {
            Say(&out, "  apkPath: NewStringUTF refused the path (%s) — the entry point gets the raw"
                      " string pointer instead, and a GetStringUTFChars on it will fault by name",
                mint.detail.c_str());
          }
        }
        Say(&out, "  r0=env 0x%s (the fake JNIEnv above), r1=this 0x%s, r2=MyRenderer 0x%s, r3=width %d;"
                  " the stack carries height %d, then apkPath 0x%s, in that order.",
            Hex(env_ptr).c_str(), Hex(stand_in).c_str(), Hex(stand_in).c_str(), surface_width,
            surface_height, Hex(entry_path).c_str());
        if (stand_in != 0) {
          Say(&out, "  the two object arguments are one zeroed guest page: not Java objects — there is no"
                    " JavaVM here — so a callee that dereferences them reads zeroes. %s",
              renderer_bound ? "The class model knows the page as a MyRenderer (see the java: line above),"
                               " so GetObjectClass and readFile on it are answered; any other JNIEnv entry"
                               " on it refuses by name."
                             : "A JNIEnv entry called on them gets that entry's refusal by name.");
        } else {
          Say(&out, "  the two object arguments are 0 because the stand-in page could not be mapped — a"
                    " callee that dereferences them faults near address 0 (see the entry line above).");
        }
        Say(&out, "  width and height are the Activity's display pixels, measured on the Java side and"
                  " passed in on the game's behalf — the surface a GLSurfaceView.Renderer would have been"
                  " handed, not numbers this run computed.");
        if (apk_path_guest != 0) {
          Say(&out, "  apkPath is a real jstring (handle 0x%s) minted through this environment's own"
                    " NewStringUTF, so the GetStringUTFChars the DEX's prototype invites hands back the"
                    " path text; an earlier revision passed the raw guest char* and faulted on exactly"
                    " that call.",
              Hex(entry_path).c_str());
        }
        const Outcome got = Call(ctor_engine, ctx, app, kEntryPoint, "ZLIIL",
                                 {abi::Value32::Ptr(env_ptr), abi::Value32::Ptr(stand_in),
                                  abi::Value32::Ptr(stand_in), abi::Value32::S32(surface_width),
                                  abi::Value32::S32(surface_height), abi::Value32::Ptr(entry_path)},
                                 kEntryBudget);
        Say(&out, "%s tier=%s reason=%s steps=%u budget=%u value=0x%s (Z: the DEX's return type, so this"
                  " is r0's low byte as the bridge reads a jboolean%s)",
            kEntryPoint, ctor_tier, ReasonName(got.reason), got.steps, kEntryBudget,
            Hex(static_cast<std::uint32_t>(got.bits)).c_str(),
            got.ok() ? "" : "; the callee did not return, so it is whatever r0 held when it stopped");
        if (!got.detail.empty()) Say(&out, "    stopped because: %s", got.detail.c_str());
        Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s r1=0x%s sp=0x%s cpsr=0x%s",
            Hex(got.exit.pc).c_str(), Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(),
            Hex(got.exit.r[1]).c_str(), Hex(got.exit.r[13]).c_str(), Hex(got.exit.cpsr).c_str());
      }
    }
    say_java();
    say_guest_log();
    say_gl();
    say_dl();
    say_syscalls();
    say_trapped();
    return out;
  }

  Say(&out, "JNI_OnLoad: exported (0x%s). Calling it with env=0 clazz=0 — a real JavaVM is not something"
            " this host has, and a library that dereferences the null will fault *inside the emulator*,",
      Hex(static_cast<std::uint32_t>(onload->addr)).c_str());
  Say(&out, "  which is reported here as a Fault rather than crashing this process.");
  bool onload_returned = false;
  const auto call_one = [&](exec::Executor& engine, const char* tier) {
    const Outcome got = Call(engine, ctx, app, "JNI_OnLoad", "II",
                             {abi::Value32::Ptr(0), abi::Value32::Ptr(0)});
    onload_returned = got.reason == exec::StopReason::Returned;
    Say(&out, "JNI_OnLoad tier=%s reason=%s steps=%u value=0x%s", tier, ReasonName(got.reason), got.steps,
        Hex64(got.bits).c_str());
    if (got.reason != exec::StopReason::Returned && !got.detail.empty()) {
      Say(&out, "    stopped because: %s", got.detail.c_str());
    }
    Say(&out, "    callee's last state: pc=0x%s lr=0x%s r0=0x%s sp=0x%s cpsr=0x%s",
        Hex(got.exit.pc).c_str(), Hex(got.exit.r[14]).c_str(), Hex(got.exit.r[0]).c_str(),
        Hex(got.exit.r[13]).c_str(), Hex(got.exit.cpsr).c_str());
  };
  // The constructor tier, once — the same rule the constructor sequence above states: this is one
  // shared guest memory, and a real library's `JNI_OnLoad` has side effects (subsystem init,
  // reference tables, GL calls it makes are counted in this run's tally). Running it on both tiers
  // applied every one of them twice; the probe lane can run both because the probe's `JNI_OnLoad`
  // returns a constant and touches nothing.
  call_one(ctor_engine, ctor_tier);
  if (tiers == 0) {
    Say(&out, "JNI_OnLoad: not called on the other tier — a real library's startup has side effects in"
              " one guest memory, so it runs once, on the constructor tier (%s), like the constructors"
              " did.",
        ctor_tier);
  }
  // The verdict, because without one a report that ends on the unanswered-import list reads as a failure
  // even when nothing failed (Angry Birds Go, 2026-10-15: every constructor and JNI_OnLoad returned).
  {
    std::size_t java_entries = 0;
    for (const std::string& name : app.ExportNames()) {
      if (name.compare(0, 5, "Java_") == 0) ++java_entries;
    }
    if (onload_returned) {
      Say(&out, "RESULT: startup finished with no fault — every constructor and JNI_OnLoad returned. This build"
                " has no driver for this game's engine (%zu Java_* entry points its own Java would call), so"
                " nothing is drawn yet",
          java_entries);
    } else {
      Say(&out, "RESULT: the constructors finished, then JNI_OnLoad stopped — the lines above say where");
    }
  }
  say_guest_log();
  say_gl();
  say_dl();
  say_syscalls();
  say_trapped();
  return out;
}

}  // namespace rb::launcher

/// The run's live status for the report screen: "<lines so far>\n<latest line or status>". Cheap and
/// lock-guarded, so the UI can poll it a few times a second while `nativeInspect` works on another thread.
extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeProgress(JNIEnv* env, jclass /*clazz*/) {
  std::string s;
  {
    std::lock_guard<std::mutex> lock(g_progress_mu);
    s = std::to_string(g_progress_lines) + "\n" + g_progress;
  }
  // Guest text can hold bytes that are not modified UTF-8, which NewStringUTF may abort on.
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80 || (u < 0x20 && c != '\n')) c = '?';
  }
  return env->NewStringUTF(s.c_str());
}

/// One MotionEvent from the run screen's surface, raw: `onTouchEvent`'s throttle, action map and
/// coordinate swap (see `QueuedTouch`) happen here, on the UI thread, as they do in the game's Java.
/// Returns whether the event was consumed (a frame loop is taking touches).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeTouch(JNIEnv* /*env*/, jclass /*clazz*/, jint action,
                                                        jfloat x, jfloat y, jint view_width, jlong time_ms) {
  std::lock_guard<std::mutex> lock(g_touch_mu);
  if (!g_touch_open) return JNI_FALSE;
  if (g_next_touch_lock != 0) {
    if (time_ms < g_next_touch_lock) {
      if (++g_jab_count > 5) {
        ++g_touch_dropped;
        return JNI_TRUE;  // consumed and ignored, as the Java's `return true` does
      }
    } else if (g_jab_count > 0) {
      --g_jab_count;
    }
  }
  g_next_touch_lock = time_ms + 1;
  std::int32_t mapped = 0;
  switch (action) {
    case 0: mapped = 1; break;  // ACTION_DOWN
    case 1: mapped = 2; break;  // ACTION_UP
    case 2: mapped = 3; break;  // ACTION_MOVE
    case 3: mapped = 4; break;  // ACTION_CANCEL
    default: break;
  }
  // Java's float-to-int truncates toward zero (and saturates; a view coordinate never needs it).
  g_touches.push_back({static_cast<std::int32_t>(y), view_width - static_cast<std::int32_t>(x), mapped, x, y});
  return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeInspect(JNIEnv* env, jclass /*clazz*/,
                                                          jbyteArray guest_so, jbyteArray libm_so,
                                                          jbyteArray libc_so, jbyteArray libz_so,
                                                          jbyteArray libstdcxx_so,
                                                          jstring label, jint tiers,
                                                          jstring apk_path, jint surface_width,
                                                          jint surface_height, jobject surface) {
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
  // One note per image, joined, so a read that cleared a pending exception names the image it belonged to
  // rather than arriving as an empty vector the loader then reports as "not an ELF image".
  std::string image_note;
  const auto image = [&](jbyteArray a, const char* what) {
    std::string note;
    std::vector<std::uint8_t> bytes = Bytes(env, a, &note);
    if (!note.empty()) image_note += std::string{"\n  "} + what + ": " + note;
    return bytes;
  };
  const std::vector<std::uint8_t> app_image = image(guest_so, "the app's library");
  const std::vector<std::uint8_t> libm_image = image(libm_so, "libm");
  const std::vector<std::uint8_t> libc_image = image(libc_so, "libc");
  const std::vector<std::uint8_t> libz_image = image(libz_so, "libz");
  const std::vector<std::uint8_t> libstdcxx_image = image(libstdcxx_so, "libstdc++");
  // The render hooks. The window is the launcher's SurfaceView, when the Java half has one up; the upload
  // goes through LauncherActivity.uploadTexture on this same thread, so GLUtils lands on the EGL context
  // the run makes current here. Looked up once, now, while a Java frame of the app's own is on the stack
  // (FindClass resolves through that frame's class loader).
  RenderHooks render;
  render.window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
  jclass activity = env->FindClass("com/retrobug/launcher/LauncherActivity");
  jmethodID upload = activity != nullptr ? env->GetStaticMethodID(activity, "uploadTexture", "([B)[I") : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (upload != nullptr) {
    render.upload = [env, activity, upload](const std::vector<std::uint8_t>& encoded, std::uint32_t& id,
                                           std::uint32_t& w, std::uint32_t& h, std::string* why) {
      jbyteArray in = env->NewByteArray(static_cast<jsize>(encoded.size()));
      if (in == nullptr) {
        env->ExceptionClear();
        if (why != nullptr) *why = "the host could not allocate the image bytes";
        return false;
      }
      env->SetByteArrayRegion(in, 0, static_cast<jsize>(encoded.size()),
                              reinterpret_cast<const jbyte*>(encoded.data()));
      auto* res = static_cast<jintArray>(env->CallStaticObjectMethod(activity, upload, in));
      env->DeleteLocalRef(in);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (why != nullptr) *why = "uploadTexture threw";
        return false;
      }
      if (res == nullptr || env->GetArrayLength(res) < 3) {
        if (why != nullptr) *why = "BitmapFactory could not decode the image";
        if (res != nullptr) env->DeleteLocalRef(res);
        return false;
      }
      jint v[3];
      env->GetIntArrayRegion(res, 0, 3, v);
      env->DeleteLocalRef(res);
      id = static_cast<std::uint32_t>(v[0]);
      w = static_cast<std::uint32_t>(v[1]);
      h = static_cast<std::uint32_t>(v[2]);
      return true;
    };
  }
  std::string report;
  try {
    report = Inspect(app_image, libm_image, libc_image, libz_image, libstdcxx_image, name,
                     static_cast<int>(tiers), apk,
                     static_cast<int>(surface_width), static_cast<int>(surface_height), render);
  } catch (const std::exception& e) {
    report = std::string{"FATAL: unhandled exception while inspecting: "} + e.what();
  }
  if (render.window != nullptr) ANativeWindow_release(render.window);
  if (!image_note.empty()) {
    // Prepended, not appended: every line below this one is about bytes the run never received, so the
    // reason has to come first or the report reads as a statement about the guest's own image.
    report = "FATAL: the Java half could not hand over every guest image — a pending exception was cleared"
             " and the read returned nothing:" + image_note + "\n" + report;
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
  // Same reason as `Inspect` above: an empty image with a cleared pending exception is a host failure and
  // has to say so rather than be reported as a guest library that would not load.
  std::string image_note;
  const auto image = [&](jbyteArray a, const char* what) {
    std::string note;
    std::vector<std::uint8_t> bytes = Bytes(env, a, &note);
    if (!note.empty()) image_note += std::string{"\n  "} + what + ": " + note;
    return bytes;
  };
  const std::vector<std::uint8_t> app_image = image(guest_so, "the app's library");
  const std::vector<std::uint8_t> libm_image = image(libm_so, "libm");

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
  if (!image_note.empty()) {
    report = "FATAL: the Java half could not hand over every guest image — a pending exception was cleared"
             " and the read returned nothing:" + image_note + "\n" + report;
  }
  return env->NewStringUTF(report.c_str());
}

/// `-e bootcheck`: the in-process boot (`rb_runtime_boot`), run from the launcher against a staged APK with
/// the repackaged layout — in this process, whose guest process then lives as long as it does (a second
/// bootcheck reuses its scope). Returns a summary line plus the report file the boot wrote.
extern "C" JNIEXPORT jstring JNICALL
Java_com_retrobug_launcher_LauncherActivity_nativeBootCheck(JNIEnv* env, jclass /*clazz*/, jstring apk_path,
                                                            jstring lib_name) {
  const auto text = [env](jstring s) {
    std::string out;
    if (s != nullptr) {
      if (const char* chars = env->GetStringUTFChars(s, nullptr)) {
        out = chars;
        env->ReleaseStringUTFChars(s, chars);
      }
    }
    return out;
  };
  const std::string apk = text(apk_path);
  const std::string lib = text(lib_name);
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  // The path rb_runtime_boot writes (its own package, this user). A report left by an earlier run must not
  // be read back as this one's.
  const std::string report_path =
      "/data/user/" + std::to_string(::getuid() / 100000) + "/com.retrobug.launcher/files/rb-report.txt";
  std::remove(report_path.c_str());
  const int rc = rb_runtime_boot(vm, apk.c_str(), lib.c_str());
  // A boot that reached Java and failed leaves the throwable pending, exactly as a device's native code would.
  // This function is about to make JNI calls of its own (`NewStringUTF`), and ART aborts on those while an
  // exception is pending — which is how a Unity title's missing `NativeLoader` class killed the whole run
  // before its report was written. Clear it, and name it in the report instead.
  std::string pending_note;
  if (env->ExceptionCheck()) {
    jthrowable pending = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string described;
    if (pending != nullptr) {
      jclass cls = env->GetObjectClass(pending);
      jmethodID to_string = cls != nullptr ? env->GetMethodID(cls, "toString", "()Ljava/lang/String;") : nullptr;
      jstring s = to_string != nullptr ? static_cast<jstring>(env->CallObjectMethod(pending, to_string)) : nullptr;
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (s != nullptr) {
        if (const char* chars = env->GetStringUTFChars(s, nullptr)) {
          described = chars;
          env->ReleaseStringUTFChars(s, chars);
        }
        env->DeleteLocalRef(s);
      }
      if (cls != nullptr) env->DeleteLocalRef(cls);
      env->DeleteLocalRef(pending);
      if (env->ExceptionCheck()) env->ExceptionClear();
    }
    pending_note = "bootcheck: the boot left a Java exception pending: " +
                   (described.empty() ? std::string{"(no description)"} : described) + "\n";
  }
  char head[160];
  std::snprintf(head, sizeof head, "bootcheck: rb_runtime_boot(%s) returned 0x%x", lib.c_str(),
                static_cast<unsigned>(rc));
  std::string report = head;
  report += '\n';
  report += pending_note;
  FILE* f = std::fopen(report_path.c_str(), "rb");
  if (f == nullptr) {
    report += std::string{"bootcheck: no rb-report.txt to read back: "} + std::strerror(errno) + "\n";
  } else {
    char buf[1 << 14];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) report.append(buf, n);
    std::fclose(f);
  }
  for (char& c : report) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80 || (u < 0x20 && c != '\n')) c = '?';
  }
  return env->NewStringUTF(report.c_str());
}

/// `-e stubcheck`: dlopen one generated stub (RTLD_NOW, in this process, where librbguest.so is already loaded), then
/// for each export: dlsym it, decode its `ldr x16, <GOT>` and check the slot the linker filled is rb_stub_capture.
/// Never calls an export. One line: "stubcheck: <lib>: N exports resolved, dispatch at 0x…", or what failed.
extern "C" JNIEXPORT jstring JNICALL Java_com_retrobug_launcher_LauncherActivity_nativeStubCheck(JNIEnv* env,
                                                                                               jclass /*clazz*/,
                                                                                               jstring path,
                                                                                               jobjectArray symbols) {
  const char* p = env->GetStringUTFChars(path, nullptr);
  const std::string file = p != nullptr ? p : "";
  if (p != nullptr) env->ReleaseStringUTFChars(path, p);
  const std::string lib = file.substr(file.rfind('/') + 1);
  std::string out = "stubcheck: " + lib + ": ";
  const auto done = [&](const std::string& tail) {
    out += tail;
    __android_log_print(ANDROID_LOG_INFO, "RBLAUNCH", "%s", out.c_str());
    return env->NewStringUTF(out.c_str());
  };
  void* const capture = dlsym(RTLD_DEFAULT, "rb_stub_capture");
  if (capture == nullptr) return done("FAIL: rb_stub_capture is not loaded in this process");
  void* h = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr) return done(std::string{"FAIL: dlopen: "} + dlerror());
  const jsize n = env->GetArrayLength(symbols);
  int resolved = 0;
  std::string bad;
  for (jsize i = 0; i < n; ++i) {
    auto s = static_cast<jstring>(env->GetObjectArrayElement(symbols, i));
    const char* c = env->GetStringUTFChars(s, nullptr);
    const std::string name = c != nullptr ? c : "";
    if (c != nullptr) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    const auto* code = static_cast<const std::uint32_t*>(dlsym(h, name.c_str()));
    if (code == nullptr) {
      bad += " " + name + " (not found)";
      continue;
    }
    const std::uint32_t ldr = code[2];
    const std::int32_t imm19 = static_cast<std::int32_t>((ldr >> 5 & 0x7ffffu) << 13) >> 13;
    void* const slot = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(&code[2]) + 4 * imm19);
    if ((ldr & 0xff00001fu) != 0x58000010u || slot != capture) {
      bad += " " + name + " (GOT slot is not rb_stub_capture)";
      continue;
    }
    ++resolved;
  }
  char head[96];
  std::snprintf(head, sizeof head, "%d of %d exports resolved, dispatch at %p", resolved, static_cast<int>(n), capture);
  return done(std::string{head} + (bad.empty() ? "" : "; FAIL:" + bad));
}
