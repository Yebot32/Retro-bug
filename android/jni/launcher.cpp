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

#include <android/native_window_jni.h>
#include <functional>

#include "gl_forward.h"

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
/// **32 MiB below the window's top (31–63 MiB)**, where Android gives a main thread 8 MiB, instead of
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
/// The host-entry window: the return sentinel at `kReturnAddress` plus the `AbiContext` slot table,
/// which lives at `kReturnAddress + 0x1000` for `kMaxSlots * 4` bytes. Nothing here is mapped by the
/// bridge — the executor recognises these addresses before translation — and the memory model is told
/// that with a guard reservation, because an *unreserved* window is a loaded library that ends past
/// 12 MiB: the loader's free-space scan would place a module over the sentinel and the slots, and a
/// guest branch to a 4-aligned address inside the range would then report a bogus host call.
constexpr std::size_t kHostWindowBytes = 0x2000u;
/// TLS: the thread pointer page and bionic's own thread struct. The first draft put the thread struct
/// at 0x430000 — the same page as the GL strings below — and `Map`'s replace-on-overlap silently wiped
/// the identity strings on every game run, so `glGetString` answered an empty string while the report
/// claimed the strings were placed. The asserts below keep every reserved page pairwise distinct,
/// because "two constants happened to agree" is exactly the failure no test caught.
constexpr exec::GuestAddr kTlsBase = 0x0040'0000u;
constexpr exec::GuestAddr kThreadStruct = 0x0046'0000u;
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

/// The rest of the reserved guest pages, hoisted out of `Inspect` and `Launch` so the check below can see
/// them. Each was a function-local `constexpr`, which kept it outside the one assertion this file has
/// against two reserved constants agreeing — the failure class that wiped the GL identity strings at
/// 0x430000 on every game run. Values unchanged; only their scope moved.
constexpr exec::GuestAddr kLoaderGlobals = 0x0041'0000u;
constexpr exec::GuestAddr kLoaderStub = 0x0042'0000u;
constexpr exec::GuestAddr kStandInObject = 0x0044'0000u;
constexpr exec::GuestAddr kApkPathAt = 0x0045'0000u;
constexpr exec::GuestAddr kJniEnvRegion = 0x0050'0000u;

/// Every reserved guest address, so the check is over the whole set rather than over the pairs someone
/// remembered to write down. The hand-written pairwise list this replaces covered seven of twelve
/// constants, and the five it missed were exactly the ones still declared inside a function.
constexpr exec::GuestAddr kReservedPages[] = {
    kReturnAddress, kGuardPage, kTlsBase,       kLoaderGlobals, kLoaderStub,   kGlStrings,
    kStandInObject, kApkPathAt,  kThreadStruct, kJniEnvRegion,  kUnresolved,   kStackTop};

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

/// Host slots for the dynamic-linker interface, right above the GL window. On a device these names
/// (and `libdl.so` itself) live in the linker — libc imports them and calls them from its own
/// constructors — so a build with no linker behind it must answer them, or the first constructor that
/// reaches one faults at the trap address. See `kDlTable` for who answers what.
constexpr std::uint32_t kDlSlotBase = 320u;
constexpr std::size_t kDlSlots = 16;
static_assert(kGlSlotBase + kGlSlots <= kDlSlotBase &&
                  kDlSlotBase + kDlSlots <= abi::AbiContext::kMaxSlots,
              "the dl* host slots would either alias the GL window or be unreachable past kMaxSlots");

/// Host slots for liblog, right above the dl window. The device's `liblog.so` needs libc++, libm and libdl
/// behind it; what a game calls is three functions, and answering them here puts the game's own log lines
/// into this report — the first time a guest has been able to say what it thinks is happening.
constexpr std::uint32_t kLogSlotBase = 336u;
enum class LogKind { kPrint, kVprint, kWrite };
struct LogEntry {
  const char* name;
  LogKind kind;
};
constexpr LogEntry kLogTable[] = {
    {"__android_log_print", LogKind::kPrint},
    {"__android_log_vprint", LogKind::kVprint},
    {"__android_log_write", LogKind::kWrite},
};
static_assert(kDlSlotBase + kDlSlots <= kLogSlotBase &&
                  kLogSlotBase + sizeof(kLogTable) / sizeof(kLogTable[0]) <= abi::AbiContext::kMaxSlots,
              "the liblog host slots would alias the dl window or be unreachable past kMaxSlots");

std::optional<std::size_t> LogSlotIndex(const std::string& name) {
  for (std::size_t i = 0; i < sizeof(kLogTable) / sizeof(kLogTable[0]); ++i) {
    if (name == kLogTable[i].name) return i;
  }
  return std::nullopt;
}

std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at);

/// Where a guest printf's variadic arguments live: optionally one core register first (r3 for
/// `__android_log_print(prio, tag, fmt, ...)`), then guest memory — the stack, or a `va_list`'s area.
/// AAPCS puts a 64-bit value 8-aligned and never splits it into r3, which is the one rule that matters.
struct GuestVarargs {
  const mem::GuestMemory* mem = nullptr;
  bool reg_free = false;
  std::uint32_t reg = 0;
  std::uint32_t at = 0;
  bool ok = true;
  std::uint32_t Next32() {
    if (reg_free) {
      reg_free = false;
      return reg;
    }
    std::uint32_t v = 0;
    if (!mem->Read(at, &v, 4)) ok = false;
    at += 4;
    return v;
  }
  std::uint64_t Next64() {
    reg_free = false;
    at = (at + 7u) & ~7u;
    std::uint64_t v = 0;
    if (!mem->Read(at, &v, 8)) ok = false;
    at += 8;
    return v;
  }
};

/// A guest `printf` format rendered on the host: flags, width and precision (including `*`), the h/hh/l/
/// ll/z/j/t/L lengths, and d i u o x X c s p f F e E g G a A %. `%s` reads a bounded guest string; `%n`
/// writes nothing (a log call has no business storing through a guest pointer here). A conversion this
/// does not know is copied through as text, so the line still says what the guest wrote.
std::string GuestFormat(const mem::GuestMemory& mem, std::uint32_t fmt_at, GuestVarargs& va) {
  const std::string fmt = GuestStringOrEmpty(mem, fmt_at);
  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    std::string spec = "%";
    std::size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j]) != nullptr) spec.push_back(fmt[j++]);
    if (j < fmt.size() && fmt[j] == '*') {
      spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
      ++j;
    } else {
      while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
    }
    if (j < fmt.size() && fmt[j] == '.') {
      spec.push_back(fmt[j++]);
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
        ++j;
      } else {
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
      }
    }
    bool wide = false;  // a 64-bit integer argument
    while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j]) != nullptr) {
      if (fmt[j] == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l') {
        wide = true;
        ++j;
      } else if (fmt[j] == 'q' || fmt[j] == 'j' || fmt[j] == 'L') {
        wide = true;
      }
      ++j;  // h, hh, l, z, t are 32-bit on this ABI and promote to int
    }
    if (j >= fmt.size()) {
      out += fmt.substr(i);
      break;
    }
    const char c = fmt[j];
    char buf[512];
    buf[0] = 0;
    if (c == '%') {
      out.push_back('%');
    } else if (std::strchr("diuoxX", c) != nullptr) {
      const std::uint64_t raw = wide ? va.Next64() : va.Next32();
      const std::string s = spec + "ll" + c;
      if (c == 'd' || c == 'i') {
        const long long v = wide ? static_cast<long long>(raw)
                                 : static_cast<long long>(static_cast<std::int32_t>(raw));
        std::snprintf(buf, sizeof buf, s.c_str(), v);
      } else {
        std::snprintf(buf, sizeof buf, s.c_str(), static_cast<unsigned long long>(raw));
      }
      out += buf;
    } else if (c == 'c') {
      std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(va.Next32() & 0xffu));
      out += buf;
    } else if (c == 'p') {
      std::snprintf(buf, sizeof buf, "0x%x", va.Next32());
      out += buf;
    } else if (std::strchr("fFeEgGaA", c) != nullptr) {
      const std::uint64_t bits = va.Next64();  // `...` promoted float to double
      double d = 0;
      std::memcpy(&d, &bits, 8);
      std::snprintf(buf, sizeof buf, (spec + c).c_str(), d);
      out += buf;
    } else if (c == 's') {
      const std::uint32_t p = va.Next32();
      const std::string s = p == 0 ? std::string{"(null)"} : GuestStringOrEmpty(mem, p);
      std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
      out += buf;
    } else if (c == 'n') {
      va.Next32();
    } else {
      out += fmt.substr(i, j - i + 1);
    }
    i = j;
  }
  return out;
}

/// The dynamic-linker surface this build answers, and the answer each name gives. A fixed table rather
/// than `IsGlName`'s prefix shape: this is an interface, not a namespace — those names are what libc's
/// loader-side callers actually import, and an unlisted `dl*`-looking name must stay trapped rather
/// than collect a stub (a trap names itself since the scope-unanswered fix; a wrong stub does not).
enum class DlKind {
  kDlopen,
  kDlopenExt,
  kDlsym,
  kDlclose,
  kDlerror,
  kDladdr,
  kExidx,
  kTargetSdk,
  kAddDtor,
  kRemoveDtor,
  kNamespace,
};
struct DlEntry {
  const char* name;
  DlKind kind;
};
constexpr DlEntry kDlTable[] = {
    {"dlopen", DlKind::kDlopen},
    {"android_dlopen_ext", DlKind::kDlopenExt},
    {"dlsym", DlKind::kDlsym},
    {"dlclose", DlKind::kDlclose},
    {"dlerror", DlKind::kDlerror},
    {"dladdr", DlKind::kDladdr},
    {"dl_unwind_find_exidx", DlKind::kExidx},
    {"android_get_application_target_sdk_version", DlKind::kTargetSdk},
    {"__loader_add_thread_local_dtor", DlKind::kAddDtor},
    {"__loader_remove_thread_local_dtor", DlKind::kRemoveDtor},
    {"__loader_android_get_exported_namespace", DlKind::kNamespace},
};

/// The table index of `name`, or `nullopt` — see `kDlTable` for why this is an exact match.
std::optional<std::size_t> DlSlotIndex(const std::string& name) {
  for (std::size_t i = 0; i < sizeof(kDlTable) / sizeof(kDlTable[0]); ++i) {
    if (name == kDlTable[i].name) return i;
  }
  return std::nullopt;
}

/// A guest NUL-terminated string for diagnostic reports, bounded and refusal-free: a report line may
/// not fault the run it describes, so an unreadable or absurd path yields the empty string. Only used
/// for naming what a guest asked the host for.
std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at) {
  std::string out;
  if (at == 0 || at >= mem.window_bytes()) return out;
  for (unsigned i = 0; i < 256; ++i) {
    char c = 0;
    if (!mem.Read(at + i, &c, 1) || c == '\0') break;
    out.push_back(c);
  }
  return out;
}

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

/// Whether an ELF32 image names `soname` in its `DT_NEEDED` list — read from the bytes, because the
/// scope is placed before the app is loaded and the decision to place an optional library has to come
/// first. Bounds-checked throughout; anything malformed answers false (the loader will name it later).
bool NeedsSoname(const std::vector<std::uint8_t>& img, const std::string& soname) {
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return at + 2 <= img.size() ? (img[at] | (img[at + 1] << 8)) : 0u;
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    if (at + 4 > img.size()) return 0u;
    std::uint32_t v = 0;
    std::memcpy(&v, img.data() + at, 4);
    return v;
  };
  if (img.size() < 52) return false;
  const std::uint32_t phoff = u32(28), phentsize = u16(42), phnum = u16(44);
  std::uint32_t dyn_off = 0, dyn_sz = 0;
  const auto file_of = [&](std::uint32_t vaddr) -> std::size_t {
    for (std::uint32_t i = 0; i < phnum; ++i) {
      const std::size_t ph = phoff + std::size_t{i} * phentsize;
      if (u32(ph) != 1u) continue;  // PT_LOAD
      const std::uint32_t off = u32(ph + 4), va = u32(ph + 8), filesz = u32(ph + 16);
      if (vaddr >= va && vaddr - va < filesz) return off + (vaddr - va);
    }
    return 0;
  };
  for (std::uint32_t i = 0; i < phnum; ++i) {
    const std::size_t ph = phoff + std::size_t{i} * phentsize;
    if (u32(ph) == 2u) {  // PT_DYNAMIC
      dyn_off = u32(ph + 4);
      dyn_sz = u32(ph + 16);
    }
  }
  std::uint32_t strtab = 0;
  std::vector<std::uint32_t> needed;
  for (std::size_t at = dyn_off; dyn_off != 0 && at + 8 <= std::size_t{dyn_off} + dyn_sz; at += 8) {
    const std::uint32_t tag = u32(at), val = u32(at + 4);
    if (tag == 0u) break;
    if (tag == 1u) needed.push_back(val);  // DT_NEEDED
    if (tag == 5u) strtab = val;           // DT_STRTAB
  }
  const std::size_t str = strtab != 0 ? file_of(strtab) : 0;
  if (str == 0) return false;
  for (const std::uint32_t n : needed) {
    std::size_t at = str + n;
    std::string name;
    while (at < img.size() && img[at] != 0 && name.size() < 256) name.push_back(static_cast<char>(img[at++]));
    if (name == soname) return true;
  }
  return false;
}

/// One entry of a zip held in memory, by exact name — what `AssetManager.open` reads for a game's
/// `assets/` path. The central directory is the authority for sizes (a local header behind a data
/// descriptor carries zeros), stored and deflated entries are both served (Angry Birds 1.0.0 ships 250
/// of its 270 assets deflated), and everything else — ZIP64, another method, a field that runs off the
/// buffer, a size that does not inflate to what the directory claims — comes back as `nullopt` with
/// `why` saying which. `name` empty means "the entry with the lowest local offset", which is the one
/// `ZipInputStream.getNextEntry()` would return first.
std::optional<std::vector<std::uint8_t>> ZipEntry(const std::vector<std::uint8_t>& zip,
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

/**
 * The IFUNC half of one image's load: the hook `LoadOptions::resolve_ifunc` is asked for each slot, the
 * guest call behind it, and the tally the report is made of.
 *
 * Why the call happens *inside* the load rather than in a pass over `GuestModule::ifunc_slots`
 * afterwards: the slot is only writable until the load ends, because `LoadFromBytes` runs its RELRO pass
 * on the way out — `docs/30` §Step 27 measured all eight of a real `libc.so`'s slots refusing the write
 * once it returns. The hook is that window (`core/loader/guest_elf.cpp`, the loop between relocation and
 * `enforce_relro`), and returning an address from it is what makes the loader write one.
 *
 * Why a tally rather than report lines written from the hook: the `GuestModule` that names these slots is
 * built by the same call that consults the hook, so at hook time nothing can say which image a slot
 * belongs to. `Report` says it once the module exists. A slot that did not resolve is a line of its own
 * with the reason it did not — "0 resolved" with nothing beside it is the shape this report may not have.
 */
struct IfuncResolution {
  abi::AbiContext* ctx = nullptr;
  exec::Executor* engine = nullptr;  ///< The tier this run chose for guest code.
  const char* tier = "I";
  exec::Executor* spare = nullptr;   ///< The other tier, asked only when the first does not return.
  const char* spare_tier = "J";
  bool armed = false;                ///< False when the hook was never handed to a loader.
  std::size_t slots = 0;
  std::size_t resolved = 0;
  /// One line per slot, resolved or not, in the order the loader recorded them — so a name that appears
  /// in one place and not the other reads as a failure rather than as an omission.
  std::vector<std::string> lines;

  /**
   * The loader's hook. `slot_value` goes in r0, which is the AArch32 convention rather than a guess: for
   * an `R_ARM_IRELATIVE` slot that word *is* the addend the resolver is called with, and the device's own
   * ARM resolvers ignore it either way (`memset_resolver` and `strlen_resolver` both begin by calling
   * `get_cpu_variant` and take the answer from there). Only the return value is load-bearing.
   */
  std::optional<std::uint32_t> operator()(const loader::IfuncSlot& slot) {
    ++slots;
    const std::string name = slot.name.empty() ? "<R_ARM_IRELATIVE>" : slot.name;
    const std::string where = Hex(slot.where);
    std::string why;
    for (int attempt = 0; attempt < 2; ++attempt) {
      exec::Executor* on = attempt == 0 ? engine : spare;
      const char* on_tier = attempt == 0 ? tier : spare_tier;
      if (on == nullptr) continue;
      ctx->set_executor(*on);
      exec::RunResult result{};
      exec::MachineState exit{};
      const abi::Ret32 ret =
          ctx->CallGuest(slot.resolver.tagged(), {abi::Value32::U32(slot.slot_value)},
                         abi::Shorty::For("I"), kCallBudget, &result, &exit);
      if (result.reason == exec::StopReason::Returned && ret.w() != 0) {
        ++resolved;
        lines.push_back("  " + name + " slot 0x" + where + " -> implementation 0x" + Hex(ret.w()) +
                        " (resolver 0x" + Hex(slot.resolver.addr) + ", tier " + on_tier + ", " +
                        std::to_string(result.instructions_executed) + " steps)");
        return ret.w();
      }
      if (!why.empty()) why += "; ";
      why += "tier ";
      why += on_tier;
      if (result.reason == exec::StopReason::Returned) {
        // A resolver that returns zero is not an answer any caller can use, and writing it would trade
        // "executes resolver code" for "branches to the null page". Named, and the slot is left alone.
        why += " returned 0, which is not an implementation";
      } else {
        why += std::string{" stopped with "} + ReasonName(result.reason) + " after " +
               std::to_string(result.instructions_executed) + " steps" +
               (result.detail.empty() ? "" : " (" + result.detail + ")");
      }
    }
    lines.push_back("  " + name + " slot 0x" + where + " NOT resolved: " + why +
                    " — the slot still holds the resolver at 0x" + Hex(slot.resolver.addr) +
                    ", so a caller of this name executes resolver code as if it were the function");
    return std::nullopt;
  }

  /// The image's outcome, once its module exists. `armed` is the difference between "this image has no
  /// resolver to run" and "nothing was asked of it", and the two deserve different lines.
  void Report(std::string* out, const std::string& soname) const {
    if (!armed) {
      Say(out, "ifunc: %s: resolution did not run for this image, so any IFUNC slot it has still holds"
               " the resolver the loader wrote",
          soname.c_str());
      return;
    }
    if (slots == 0) {
      Say(out, "ifunc: %s has no slot to resolve — the loader found no relocation in it naming a"
               " resolver",
          soname.c_str());
      return;
    }
    Say(out, "ifunc: %s — %zu of %zu slot(s) resolved through the guest, %zu still holding a resolver",
        soname.c_str(), resolved, slots, slots - resolved);
    for (const std::string& line : lines) Say(out, "%s", line.c_str());
  }
};

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
 * What this has that `Launch` does not is IFUNC: every image loaded here — the three in the scope and the
 * app — has its `STT_GNU_IFUNC` slots resolved by running their resolvers through the bridge while the
 * image is still writable, and the report names each slot's implementation. A library loaded without that
 * hands the guest a *resolver* where it expects a routine, which is the shape of this launcher's last
 * hang rather than a subtlety of the ELF format.
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
/// What a run needs to draw: the launcher's window (null for a headless run — the scripted lanes, or no
/// screen), and how to turn an image file into a real GL texture on the context current on this thread.
/// `upload` is the Java half's BitmapFactory + GLUtils.texImage2D, the same calls the game's own
/// textureLoaded makes; it returns the GL name and the bitmap's size.
struct RenderHooks {
  ANativeWindow* window = nullptr;
  std::function<bool(const std::vector<std::uint8_t>& encoded, std::uint32_t& id, std::uint32_t& w,
                     std::uint32_t& h, std::string* why)>
      upload;
};

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
  // The host-entry window is reserved, not merely avoided: nothing maps it, but the loader's
  // free-space scan must see it as taken, or a library ending past 12 MiB is placed over the return
  // sentinel and the `AbiContext` slot table. `kNone` makes it a guard reservation — occupied for
  // placement, refused for any access.
  if (!mem->Map(kReturnAddress, kHostWindowBytes, kNone, &err)) {
    Say(&out, "FATAL: reserving the host-entry window failed: %s", err.c_str());
    return out;
  }
  // Same as `Launch`: no reservation, because the address is outside the window and cannot be reached by
  // the placement scan. The earlier attempt to map it here refused the run along with the other one.

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
  /// The renderer's state for the GL slots: forwarding is live only while the run holds a current EGL
  /// context (the Gumball profile makes one on the launcher's window). Off, the slots are the counted
  /// no-ops they always were — headless runs and the scripted lanes are unchanged.
  rb::launcher::GlForward gl_forward;
  bool gl_live = false;
  std::map<std::string, std::string> gl_forward_fail;  ///< first refusal per name
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
    ctx.Bind(slot, [&gl_calls, &gl_forward, &gl_live, &gl_forward_fail, &mem, index,
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
      s.r[0] = wants_a_string ? (parsed_by_the_game ? kGlStrings : kGlEmpty) : 0u;
      return true;
    });
    return ctx.SlotFor(slot);
  };

  /// The dynamic-linker interface, bound eagerly because the set is fixed. On a device this lives in
  /// the linker (`ld-android`): libc's own constructors call into it — `netdClientInitImpl` opens
  /// `libnetd_client.so` and checks the NULL (SM-X710, 2026-10-09) — and leaving it trapped made that
  /// call fault at the trap address with the report unable to say which symbol. The answers are the
  /// honest ones for a build with no linker behind it, every one is counted, and the report's `dl:`
  /// block names each with its call count.
  struct DlCall {
    std::string name;
    unsigned calls = 0;
    std::string last_path;
    unsigned answered = 0;  ///< dl_unwind_find_exidx: calls whose pc a loaded module's index covered
  };
  /// Each placed module's unwind index, appended as modules load (the slots are bound before any is):
  /// what `dl_unwind_find_exidx(pc, &count)` answers, the way bionic's linker walks its soinfo list.
  struct ExidxRange {
    std::uint32_t base = 0, extent = 0, exidx = 0, count = 0;
  };
  std::vector<ExidxRange> exidx_ranges;
  std::vector<DlCall> dl_calls;
  for (std::size_t i = 0; i < sizeof(kDlTable) / sizeof(kDlTable[0]); ++i) {
    dl_calls.push_back(DlCall{kDlTable[i].name, 0, {}, 0});
    const std::size_t index = i;
    ctx.Bind(kDlSlotBase + static_cast<std::uint32_t>(i),
             [&dl_calls, &mem, &exidx_ranges, index](exec::MachineState& s, std::string* serr) {
               DlCall& c = dl_calls[index];
               ++c.calls;
               switch (kDlTable[index].kind) {
                 case DlKind::kExidx: {
                   // `_Unwind_Ptr dl_unwind_find_exidx(_Unwind_Ptr pc, int* pcount)`: the module whose
                   // range holds pc gives its `.ARM.exidx` and entry count; no module, NULL and 0 —
                   // bionic's answer too. Answering NULL for everything made every C++ throw in the game
                   // terminate: libgcc's unwinder found no frame with unwind info, so no catch at all.
                   const std::uint32_t pc = s.r[0] & ~1u;
                   std::uint32_t at = 0, count = 0;
                   for (const ExidxRange& r : exidx_ranges) {
                     if (pc >= r.base && pc - r.base < r.extent && r.exidx != 0) {
                       at = r.exidx;
                       count = r.count;
                       break;
                     }
                   }
                   if (s.r[1] != 0 && !mem->Write(s.r[1], &count, 4)) {
                     if (serr != nullptr) *serr = "dl_unwind_find_exidx: pcount 0x" + Hex(s.r[1]) + " is not writable";
                     return false;
                   }
                   if (at != 0) ++c.answered;
                   s.r[0] = at;
                   return true;
                 }
                 case DlKind::kDlopen:
                 case DlKind::kDlopenExt:
                   // NULL: the library this build cannot provide. A caller that checks — netd's does —
                   // carries on; one that does not faults on the NULL it asked to be lied to about.
                   c.last_path = GuestStringOrEmpty(*mem, s.r[0]);
                   s.r[0] = 0;
                   return true;
                 case DlKind::kTargetSdk:
                   s.r[0] = 34u;  // this app's targetSdkVersion, what a device's linker would answer
                   return true;
                 default:
                   s.r[0] = 0;  // NULL / 0 / "no error since last clear": every caller checks
                   return true;
               }
             });
  }

  /// liblog, answered: every line the guest logs, formatted from its own format string and arguments,
  /// kept for the report's `guest log:` block. Bounded, with the overflow counted rather than dropped
  /// silently, because a game that logs per frame would otherwise fill the report.
  struct GuestLogLine {
    int prio = 0;
    std::string tag, text;
  };
  std::vector<GuestLogLine> guest_log;
  unsigned guest_log_dropped = 0;
  constexpr std::size_t kGuestLogMax = 300;
  for (std::size_t i = 0; i < sizeof(kLogTable) / sizeof(kLogTable[0]); ++i) {
    const LogKind kind = kLogTable[i].kind;
    ctx.Bind(kLogSlotBase + static_cast<std::uint32_t>(i),
             [&guest_log, &guest_log_dropped, &mem, kind](exec::MachineState& s, std::string*) {
               // (prio, tag, fmt, ...) / (prio, tag, fmt, va_list) / (prio, tag, msg)
               GuestLogLine line{static_cast<int>(s.r[0]), GuestStringOrEmpty(*mem, s.r[1]), {}};
               if (kind == LogKind::kWrite) {
                 line.text = GuestStringOrEmpty(*mem, s.r[2]);
               } else {
                 GuestVarargs va;
                 va.mem = mem.get();
                 if (kind == LogKind::kPrint) {
                   va.reg_free = true;
                   va.reg = s.r[3];
                   va.at = s.r[13];
                 } else {
                   va.at = s.r[3];  // AAPCS va_list: one pointer into the argument area
                 }
                 line.text = GuestFormat(*mem, s.r[2], va);
                 if (!va.ok) line.text += "  [some arguments were unreadable guest memory]";
               }
               while (!line.text.empty() && (line.text.back() == '\n' || line.text.back() == '\r')) {
                 line.text.pop_back();
               }
               if (guest_log.size() < kGuestLogMax) {
                 guest_log.push_back(std::move(line));
               } else {
                 ++guest_log_dropped;
               }
               s.r[0] = 1;  // liblog returns a positive byte count on success; 1 is truthful for "written"
               return true;
             });
  }

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
  // 1 MiB (was 32 KiB, originally 4): `Get<T>ArrayElements` copies live here too (Gumball 2013 reads its
  // data files that way), and `GetStringUTFChars` hands back a real copy in this arena (JNI's
  // `isCopy=1`, a `kCharsMagic` block told apart from the `jstring` by its magic), so one string in
  // flight can hold two blocks — and 4 KiB refused any message larger than a couple of KiB even before
  // that. The reserved gap is megabytes wide (the next reserved constant is the guard page 3 MiB up),
  // so the growth costs a few pages of guest address space and nothing else.
  constexpr unsigned kJniEnvArena = 1u << 20;
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
   * The two tiers, built here rather than beside the constructors below, because IFUNC resolution runs
   * *inside* each load: a resolver is guest code, so an executor has to exist before the first
   * `LoadFromBytes` — `LoadOptions::resolve_ifunc` is called between relocation and RELRO, and that call
   * has to be able to run something. Constructing an executor maps no guest page and runs no guest code,
   * so nothing the loads below place is affected by the order.
   */
  exec::InterpExecutor interp{*mem, kReturnAddress, &ctx};
  exec::JitExecutor jit{*mem, kReturnAddress, &ctx};
  if (!jit.ok()) Say(&out, "tier J is unavailable before any guest code ran: %s", jit.error().c_str());
  // The tier this run was asked for, by the same rule the constructor sequence below uses, and the other
  // tier as the fallback a resolver this one cannot execute falls through to. One `set_executor` call,
  // here, is what arms the bridge for everything that follows.
  exec::Executor& ctor_engine = (tiers == 2 && jit.ok()) ? static_cast<exec::Executor&>(jit)
                                                         : static_cast<exec::Executor&>(interp);
  const char* ctor_tier = ctor_engine.tier() == exec::CodeTier::Interp ? "I" : "J";
  ctx.set_executor(ctor_engine);

  /**
   * IFUNC resolution, armed with the guest call it needs and handed to every load below.
   *
   * Without it the loader leaves the **resolver's address** in each IFUNC slot, and the guest then calls
   * resolver *code* as if it were the function. That is not hypothetical: it is what this build was
   * doing, and a disassembly trace on the phone caught `strlen`'s slot holding `strlen_resolver`, so
   * `__strlen_chk` received a pointer where it expects a length, failed its fortify check, and recursed
   * through `async_safe_fatal_va_list` until the step budget and the stack were gone. One unresolved
   * IFUNC, one infinite recursion, and every symptom pointed at the wrong layer.
   *
   * A resolver that probes `/dev/cpu_variant:arm` is answered by the monitor attached above; its `ENOENT`
   * is the answer that makes bionic choose the generic implementation, which is the one that is right
   * without knowing which SoC this is. With no monitor there is no such answer, so the hook is not handed
   * over at all and each image's line below says its slots were left as they were.
   */
  IfuncResolution ifunc;
  ifunc.ctx = &ctx;
  ifunc.engine = &ctor_engine;
  ifunc.tier = ctor_tier;
  // A fallback tier is offered only when this run asked for both (`tiers` 0): 1 and 2 are requests to see
  // one engine alone, so a resolver that engine cannot execute is that tier's gap to name, not something
  // to cover with the engine the caller excluded. A JIT that could not be built is not a retry either.
  if (tiers == 0) {
    if (&ctor_engine == &interp) {
      if (jit.ok()) ifunc.spare = &jit;
    } else {
      ifunc.spare = &interp;
    }
  }
  ifunc.spare_tier = (&ctor_engine == &interp) ? "J" : "I";
  // Both halves of the requirement are checked rather than assumed: a resolver is guest code (an engine)
  // that probes for the CPU variant with a syscall (a monitor). With either missing, the hook is not
  // handed over at all and every image's line below says its slots were left as they were.
  ifunc.armed = ifunc.engine != nullptr && ctx.monitor() != nullptr;
  if (!ifunc.armed) {
    Say(&out, "ifunc: no engine and monitor pair is available to run a resolver — a resolver is guest"
              " code whose openat(\"/dev/cpu_variant:arm\") probe a monitor has to answer — so no image"
              " has its IFUNC slots resolved, and each one's line below says so as it is loaded");
  }

  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "__stack_chk_guard") return kGuardPage;
    if (name == "__loader_shared_globals") return loader_shared_globals;
    if (IsGlName(name)) {
      if (const auto slot = gl_slot_for(name)) return slot;
    }
    if (const auto dl = DlSlotIndex(name)) {
      return ctx.SlotFor(kDlSlotBase + static_cast<std::uint32_t>(*dl));
    }
    if (const auto lg = LogSlotIndex(name)) {
      return ctx.SlotFor(kLogSlotBase + static_cast<std::uint32_t>(*lg));
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
  std::optional<loader::GuestModule> libc_mod, libm_mod, libz_mod, libstdcxx_mod;
  /**
   * An import that names another module's IFUNC export must be bound to what the resolver *returns*, the
   * way bionic's linker does — `FindExport` can only do that when the defining module's own relocations
   * happened to resolve a slot of the same name. libc's do for strcmp/strlen/memcpy/memmove/memset and two
   * `_chk`s, and not for strcpy, stpcpy, strcat or the other `_chk`s: Gumball 2013's `strcpy` was bound to
   * libc's *resolver* (0xcb0a5), so every texture name it copied came back as untouched heap and its splash
   * screen waited forever for an image it had been given (measured with the game's own GOT, 2026-10-14).
   * Resolved once per name, through the same tiers and tally as the slot resolution above.
   */
  IfuncResolution export_ifunc = ifunc;
  std::map<std::string, std::uint32_t> export_ifunc_cache;
  const auto export_of = [&](const loader::GuestModule& m,
                             const std::string& name) -> std::optional<std::uint32_t> {
    const auto fn = m.FindExport(name);
    if (!fn) return std::nullopt;
    const std::string base = name.substr(0, name.find('@'));
    for (const loader::Symbol& s : m.exports) {
      if (!s.defined || !s.ifunc || (s.value & ~1u) != fn->addr) continue;
      if (s.name.substr(0, s.name.find('@')) != base) continue;
      // FindExport handed back the resolver itself: run it.
      if (const auto hit = export_ifunc_cache.find(base); hit != export_ifunc_cache.end()) return hit->second;
      if (!export_ifunc.armed) return fn->tagged();  // reported once below: nothing could run it
      loader::IfuncSlot slot{};
      slot.name = base;
      slot.resolver = *fn;
      const auto impl = export_ifunc(slot);
      if (!impl) return fn->tagged();  // the tally names the failure; the resolver stays bound
      export_ifunc_cache[base] = *impl;
      return *impl;
    }
    return fn->tagged();
  };
  /// Scope imports the host could not answer, so the report can NAME them. The app's imports were
  /// always recorded; the scope's were not, and on 2026-10-09 a libc constructor (`netdClientInitImpl`)
  /// called one — faulting at 0xbad0000 with "imports … did not answer: none" printed above it, which
  /// is the one shape this report must never produce: a named trap with no name.
  std::vector<std::string> scope_unanswered;
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
    // This image's own tally, with the bridge and the tiers armed above: the hook is per image because the
    // lines below name the module, and a failure is named per slot rather than counted. Captured by
    // reference like `opts.resolve` below — and that is not a style choice: assigning the object itself
    // would copy it into the `std::function`, and the loader would fill the *copy's* tally while this one
    // reported zero slots for every image.
    IfuncResolution image_ifunc = ifunc;
    if (ifunc.armed) {
      opts.resolve_ifunc = [&image_ifunc](const loader::IfuncSlot& s) { return image_ifunc(s); };
    }
    // By reference, and that is safe here where the file header's provider has to be by value: this
    // resolver is consulted only inside `LoadFromBytes`, which this lambda calls synchronously and does
    // not retain. It answers from what is already in the scope, which is what makes the load order below
    // dependency-first rather than a preference.
    opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
      for (const std::optional<loader::GuestModule>* placed : {&libc_mod, &libm_mod, &libz_mod, &libstdcxx_mod}) {
        if (*placed) {
          if (const auto fn = export_of(**placed, name)) return *fn;
        }
      }
      const std::optional<std::uint32_t> from_host = host(name);
      if (!from_host || *from_host == kUnresolved) {
        // An import can fill more than one slot, and each slot is asked separately — the same name must
        // not be printed once per relocation (the first device run listed two `__loader_*` hooks
        // twice). Dedup here rather than at print time: this is also the list a future answer loop will
        // walk, and it should have one entry per symbol.
        bool seen = false;
        for (const std::string& already : scope_unanswered) {
          if (already == name) {
            seen = true;
            break;
          }
        }
        if (!seen) scope_unanswered.push_back(name);
      }
      return from_host;
    };
    std::string load_err;
    slot = loader::LoadFromBytes(*mem, image, soname, opts, &load_err);
    if (!slot) {
      Say(&out, "scope: %s not loaded: %s", soname, load_err.c_str());
      // A refusal can be a resolved slot the loader could not write, which names itself in `load_err`
      // above; anything resolved before that is still a fact, so the tally is printed rather than lost.
      if (image_ifunc.slots != 0) image_ifunc.Report(&out, soname);
      return;
    }
    std::size_t exports = 0;
    for (const loader::Symbol& sym : slot->exports) {
      if (sym.defined) ++exports;
    }
    Say(&out, "scope: %s at 0x%s extent 0x%s, %zu defined exports", soname, Hex(slot->base).c_str(),
        Hex(slot->extent).c_str(), exports);
    exidx_ranges.push_back(ExidxRange{slot->base, slot->extent, slot->exidx, slot->exidx_count});
    if (!slot->unsupported_relocs.empty()) {
      std::string kinds;
      for (const std::string& one : slot->unsupported_relocs) {
        kinds += (kinds.empty() ? "" : ", ") + one;
      }
      Say(&out, "scope: %s: %zu relocation(s) tolerated but not applied — %s. Those slots still hold"
                " file data (placeholders); that is a lurking WRONG VALUE if the guest ever reads them,"
                " not a safe skip — the real linker resolves each from its module and TLS layout",
          soname, slot->unsupported_relocs.size(), kinds.c_str());
    }
    // Per image, now that the module that names the slots exists. Every slot gets a line, resolved or
    // not: a name missing from this list would be exactly the silent zero the launcher forbids.
    image_ifunc.Report(&out, soname);
  };
  load_scope("libc.so", libc_mod, libc_image);
  load_scope("libm.so", libm_mod, libm_image);
  load_scope("libz.so", libz_mod, libz_image);
  // Only when the app names it: an image no DT_NEEDED asks for would be a module in scope that a device
  // would not have loaded (Angry Birds needs no libstdc++; Gumball 2013 does). Placed after libc, its
  // only dependency.
  if (NeedsSoname(app_image, "libstdc++.so")) load_scope("libstdc++.so", libstdcxx_mod, libstdcxx_image);

  /**
   * The `auxv`, which on a real device the dynamic linker has already published by the time libc.so is
   * loaded: `__libc_init_main_thread_early` (compiled into the linker — `libc/bionic/__libc_init_main_
   * thread.cpp`) writes `__libc_shared_globals()->auxv = args.auxv`, and libc's own
   * `__libc_shared_globals()` is a thunk to `__loader_shared_globals` — the stub this launcher answers
   * from the zeroed page at 0x410000. Left zero, libc's `__libc_preinit` (its `DT_INIT_ARRAY[0]`)
   * reaches `getauxval` and faults reading address 0: that is the wall the SM-X710 run currently stops
   * at, pc inside `getauxval` one instruction past the globals load.
   *
   * The field's offset inside the globals struct is a private detail of the libc build, so it is
   * decoded from the image rather than assumed: `getauxval` calls `__libc_shared_globals` and then
   * loads the pointer with `ldr.w rX, [rX, #imm]`, so the scan below looks for exactly that pair in
   * the exported function's first instructions and takes the immediate as the offset. The shape and
   * the field's current contents (zero) are both checked before anything is written; a differing
   * shape is refused by name rather than guessed at.
   *
   * What goes in the array is measured rather than assumed: this libc has three `getauxval` call
   * sites — AT_SYSINFO_EHDR (`__libc_init_vdso`, which takes the absent path on 0), AT_RANDOM (the
   * setjmp cookie, read only when /dev/urandom is unreachable) and AT_CLKTCK (not on the constructor
   * path). So: AT_RANDOM with 16 bytes of host entropy, then AT_NULL. The absent vdso entry is the
   * honest answer — there is no vdso to point at — and `init_vdso` treats its absence as that case.
   */
  if (libc_mod) {
    const auto getauxval = libc_mod->FindExport("getauxval");
    if (!getauxval) {
      Say(&out, "auxv: libc.so exports no getauxval, so preinit's auxv read cannot be decoded — this"
                " libc predates the shared-globals mechanism this launcher plays the linker for");
    } else {
      std::uint16_t insn[16] = {};
      std::optional<std::uint32_t> field;
      if (mem->Read(getauxval->addr, insn, sizeof(insn))) {
        for (unsigned i = 0; i + 3 < 16 && !field; ++i) {
          const std::uint16_t h0 = insn[i], h1 = insn[i + 1];
          if ((h0 & 0xF800u) != 0xF000u || (h1 & 0xD000u) != 0xD000u) continue;  // BL / BLX
          const std::uint16_t ldr = insn[i + 2], imm = insn[i + 3];
          if ((ldr & 0xFFF0u) != 0xF8D0u) continue;  // LDR.W Rt,[Rn,#imm12]
          const unsigned rn = ldr & 0xFu, rt = (imm >> 12) & 0xFu;
          const std::uint32_t off = imm & 0xFFFu;
          if (rn == rt && off != 0 && (off & 3u) == 0 && off < kGuestPage) field = off;
        }
      }
      if (!field) {
        Say(&out, "auxv: getauxval at 0x%s does not match the shared-globals shape (no `bl` followed by"
                  " `ldr.w rX,[rX,#imm]` in its first instructions) — preinit will fault reading address"
                  " 0 when getauxval runs; the offset is a per-libc detail and is not guessed at",
            Hex(getauxval->addr).c_str());
      } else {
        std::uint32_t current = 0xffff'ffffu;
        if (!mem->Read(kLoaderGlobals + *field, &current, 4)) {
          Say(&out, "auxv: the auxv field at 0x%s could not be read",
              Hex(kLoaderGlobals + *field).c_str());
        } else if (current != 0) {
          Say(&out, "auxv: the auxv field at 0x%s already holds 0x%s — refusing to overwrite state this"
                    " run did not write",
              Hex(kLoaderGlobals + *field).c_str(), Hex(current).c_str());
        } else {
          const std::uint32_t array_at = kLoaderGlobals + ((*field + 16u + 3u) & ~3u);
          const std::uint32_t random_at = array_at + 16u;
          if (random_at + 16u > kLoaderGlobals + kGuestPage) {
            Say(&out, "auxv: the decoded field (+0x%s) leaves no room in the loader-globals page for the"
                      " array", Hex(*field).c_str());
          } else {
            std::random_device entropy;
            std::uint32_t rand_words[4];
            for (std::uint32_t& w : rand_words) w = entropy();
            const std::uint32_t array[4] = {25u /* AT_RANDOM */, random_at, 0u /* AT_NULL */, 0u};
            if (mem->Write(array_at, array, sizeof(array)) &&
                mem->Write(random_at, rand_words, sizeof(rand_words)) &&
                mem->Write(kLoaderGlobals + *field, &array_at, 4)) {
              Say(&out, "auxv: getauxval's globals-read decoded to +0x%s; AT_RANDOM (16 host-entropy"
                        " bytes at 0x%s) then AT_NULL at 0x%s, and the pointer written to 0x%s — the"
                        " linker's part, played before libc's preinit runs",
                  Hex(*field).c_str(), Hex(random_at).c_str(), Hex(array_at).c_str(),
                  Hex(kLoaderGlobals + *field).c_str());
            } else {
              Say(&out, "auxv: writing the array or the pointer into the loader-globals page failed");
            }
          }
        }
      }
    }
  }

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
  /// Non-GL names the host answered with a slot of its own (the dl* and liblog tables). They used to be
  /// filed under `trapped`, so Gumball's `__android_log_print` was listed as unanswered while bound.
  std::vector<std::string> host_answered;
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
  // The app's own IFUNC slots (if any) get what the scope's got: resolved during this load, by the same
  // hook, before RELRO makes the slots unwritable. Its `strlen` and friends come from the scope and are
  // already implementations by the time `FindExport` answers them.
  IfuncResolution app_ifunc = ifunc;
  if (ifunc.armed) {
    app_opts.resolve_ifunc = [&app_ifunc](const loader::IfuncSlot& s) { return app_ifunc(s); };
  }
  app_opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    for (const std::optional<loader::GuestModule>* mod : {&libc_mod, &libz_mod, &libm_mod, &libstdcxx_mod}) {
      if (*mod) {
        if (const auto fn = export_of(**mod, name)) {
          record(answered, name);
          return *fn;
        }
      }
    }
    const std::optional<std::uint32_t> from_host = host(name);
    // The bucket has to be the answer the name actually got, not the question that was asked: a `gl*`
    // name is in the host-answered set only when a slot really came back, so a refused slot set still
    // reports those names as trapped rather than claiming an answer nobody can call.
    if (IsGlName(name) && from_host && *from_host != kUnresolved) {
      record(gl_answered, name);
    } else if (from_host && *from_host != kUnresolved) {
      record(host_answered, name);
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
    if (app_ifunc.slots != 0) app_ifunc.Report(&out, label);
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
  exidx_ranges.push_back(ExidxRange{app.base, app.extent, app.exidx, app.exidx_count});
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
  app_ifunc.Report(&out, app.soname.empty() ? label : app.soname);
  // Imports bound to another module's IFUNC export (strcpy and friends), resolved by running the resolver.
  if (export_ifunc.slots != 0) export_ifunc.Report(&out, "cross-module IFUNC imports");

  /**
   * The thread pointer, before any guest constructor runs.
   *
   * `__get_tls()` in bionic is a read of the coprocessor register TPIDRURW — `MachineState::tp`, which
   * defaults to zero — and every per-thread thing a constructor touches goes through it. With it zero the
   * first TLS slot read is a read of a near-null address, and that is exactly what the run before this
   * one did: libc's own `DT_INIT_ARRAY[0]` stopped at `guest memory read at 0x14 refused` on
   * `ReadMemory32`, which is slot 5 of a null TLS block rather than anything libc did wrong.
   *
   * One page, read/write, at an address the file-scope layout constants leave free (they are declared
   * beside the other reserved pages, and the static_asserts there keep the two distinct — the first
   * draft of this block declared its own `kThreadStruct` at 0x430000, which was also `kGlStrings`, and
   * the map below silently replaced the identity strings with a zero page).
   */
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
    // One page *below* the thread-pointer block, for this libc's `*(TP - 4)` read. TP-4 is
    // `TLS_SLOT_BIONIC_TLS` (-1): the pointer to the thread's `bionic_tls` object, written on a device
    // by the linker's `__libc_init_main_thread_late` -> `__init_bionic_tls_ptrs` (pthread_create.cpp),
    // and read by preinit's tail as `mrc p15` (TP) ; `ldr r0, [r0, #-4]` ; `strb r5, [r0, #0xb49]`
    // (file 0x6e8b4-0x6e8bc, SM-X710 2026-10-09) — the byte at bionic_tls+0xb49 is the systrace
    // "disabled" flag, beside fdtrack's at +0xb48 (`libc/private/bionic_tls.h`). So the slot needs a
    // readable, zeroed, >=0xB4C-byte object; `kThreadStruct`'s page is exactly that, and its +8 is the
    // tid the syscall monitor answers with — one launcher-created zero page playing both the
    // bionic_tls and the thread-struct part a real linker would have allocated separately.
    if (!mem->Map(kTlsBase - kGuestPage, kGuestPage, kRead | kWrite, &err)) {
      Say(&out, "tls: could not map the page below the thread block (0x%s): %s — this libc's preinit"
                " reads *(TP-4) right after netd's constructor and will fault there",
          Hex(kTlsBase - kGuestPage).c_str(), err.c_str());
    } else {
      const std::uint32_t below = kThreadStruct;
      if (!mem->Write(kTlsBase - 4u, &below, 4)) {
        Say(&out, "tls: could not write the thread struct pointer at 0x%s (TP-4)",
            Hex(kTlsBase - 4u).c_str());
      }
    }
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
   * `guest_linker.cpp:238` establishes, verbatim from bionic's `soinfo::call_constructors`: `DT_INIT`
   * first, then `DT_INIT_ARRAY`, a zero entry skipped rather than called. This call site kept the old
   * array-first order (and cited the linker as having established it) after the linker itself was
   * fixed — and it is the only runner the shipping Inspect flow uses, so the bug stayed live on device
   * until the audit looked here. The app's tags come last because a constructor may call into the scope.
   *
   * One tier only, and that is a correctness rule rather than a speed one: a constructor has side effects
   * and this is one guest memory, so running the sequence on both tiers would run every constructor
   * twice. The engine and tier are the pair chosen above, before the loads, for the same reason the
   * IFUNC hook needed one.
   */
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
    if (slot->init.addr != 0) {
      if (run_one_ctor(slot->init)) {
        ++*ran;
      } else {
        say_ctor_failure(std::string{soname} + " DT_INIT at 0x" + Hex(slot->init.addr));
        return false;
      }
    }
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
    return true;
  };
  std::size_t libc_ctors = 0, libm_ctors = 0, libz_ctors = 0, libstdcxx_ctors = 0, app_ctors = 0;
  const bool ctors_ok = run_ctors("libc.so", libc_mod, &libc_ctors) &&
                        run_ctors("libm.so", libm_mod, &libm_ctors) &&
                        run_ctors("libz.so", libz_mod, &libz_ctors) &&
                        run_ctors("libstdc++.so", libstdcxx_mod, &libstdcxx_ctors) &&
                        run_ctors(label.c_str(), app_mod, &app_ctors);
  if (ctors_ok) {
    Say(&out, "constructors: %zu ran in total on tier=%s (libc.so %zu, libm.so %zu, libz.so %zu, libstdc++.so %zu, %s %zu)",
        libc_ctors + libm_ctors + libz_ctors + libstdcxx_ctors + app_ctors, ctor_tier, libc_ctors, libm_ctors,
        libz_ctors, libstdcxx_ctors,
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
  Say(&out, "imports: %zu of %zu answered by the guest scope, %zu by the host's GL set (below), %zu by"
            " other host slots (dl*, liblog), %zu still trapped at 0x%s",
      answered.size(), answered.size() + gl_answered.size() + host_answered.size() + trapped.size(),
      gl_answered.size(), host_answered.size(), trapped.size(), Hex(kUnresolved).c_str());

  // One place that writes the trap list, so the exits below cannot drift apart. `Say` is variadic and
  // takes a `const char*`, so the note is built as a string first: a ternary mixing `""` with a
  // concatenation hands the variadic call a `std::string` and clang refuses it.
  const auto say_trapped = [&]() {
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

  /**
   * One place that writes the GL tally, at every exit, for the same reason `say_trapped` exists.
   *
   * The list is of the names this run *bound*, each with its call count, rather than of the names that
   * were called: for the work ahead — forwarding these entry points to a host driver one day — which
   * entry points the app asks for is the useful fact, and a count of 0 says "bound and not reached yet"
   * where an omission would say nothing. The two are distinguishable at a glance and that matters here,
   * because a run that stops early and one that never bound the set produce the same silence otherwise.
   */
  /// The dl* set's tally, in `say_gl`'s shape. The disclosure is the point: what a device's linker
  /// provides, this build answers for — and each answer is named, because "the app loaded" is only
  /// honest alongside what its loader-side imports were actually given.
  const auto say_dl = [&]() {
    unsigned called = 0, calls = 0;
    for (const DlCall& e : dl_calls) {
      if (e.calls != 0) ++called;
      calls += e.calls;
    }
    Say(&out, "dl: %zu dynamic-linker import(s) were answered by host slots %u..%u — this build ships"
              " no linker: dlopen/android_dlopen_ext return NULL (a caller that checks, as netd's does,"
              " carries on), dl_unwind_find_exidx answers from the placed modules' PT_ARM_EXIDX (NULL for"
              " a pc no module holds, as bionic does), dlsym/dladdr/__loader_android_get_exported_namespace"
              " return NULL, dlclose returns 0, dlerror returns NULL (\"no error since last clear\"),"
              " android_get_application_target_sdk_version answers 34 (this app's targetSdkVersion), and"
              " the two __loader_*_thread_local_dtor hooks are counted no-ops. Disclosed, not silently"
              " stubbed.",
        dl_calls.size(), kDlSlotBase, kDlSlotBase + static_cast<std::uint32_t>(dl_calls.size()) - 1u);
    if (called == 0) {
      Say(&out, "dl: none has been called by this run yet — the set is bound, so a call arrives answered"
                " instead of faulting at 0x%s",
          Hex(kUnresolved).c_str());
      return;
    }
    Say(&out, "dl: %u of them have been called, %u calls in total:", called, calls);
    for (const DlCall& e : dl_calls) {
      if (e.calls == 0) continue;
      if (!e.last_path.empty()) {
        Say(&out, "  %s(\"%s\") x%u", e.name.c_str(), e.last_path.c_str(), e.calls);
      } else if (e.name == "dl_unwind_find_exidx") {
        Say(&out, "  %s x%u (%u found a module's index; the rest were a pc no module holds)", e.name.c_str(),
            e.calls, e.answered);
      } else {
        Say(&out, "  %s x%u", e.name.c_str(), e.calls);
      }
    }
  };

  /// The game's own log, in its own words, through the liblog slots.
  const auto say_guest_log = [&]() {
    if (guest_log.empty()) return;
    Say(&out, "guest log: %zu line(s) the game wrote through __android_log_*%s:", guest_log.size(),
        guest_log_dropped != 0 ? (", " + std::to_string(guest_log_dropped) + " more dropped past the cap").c_str()
                               : "");
    static const char* const kPrio[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
    for (const GuestLogLine& l : guest_log) {
      Say(&out, "  %s/%s: %s", (l.prio >= 0 && l.prio <= 8) ? kPrio[l.prio] : "?", l.tag.c_str(),
          l.text.c_str());
    }
  };

  /// What the game asked its Java side for: each host method's tally, then every readFile request with
  /// the APK entry that answered it or the reason it came back null.
  const auto say_java = [&]() {
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
            const abi::GuestAddr scratch = kApkPathAt;  // a guest page the launcher owns
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
                 const auto* bytes = env.HostArray(static_cast<abi::GuestAddr>(a[1]));
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
              mem->Read(scene_slot, &before, 4);
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
          mem->Read(scene_slot, &now_scene, 4);
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
            Say(&out, "gumball: frame %u — jniRun steps=%u, %u GL call(s) this frame, %zu texture(s) so far",
                f, run_o.steps, gl_total() - g1, textures.size());
          }
        }
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
          if (mem->Write(kApkPathAt, kName, sizeof kName)) {
            const Outcome c = Call(ctor_engine, ctx, app, "_Z25checkUnclaimedTexturesForPKc", "Z",
                                   {abi::Value32::Ptr(kApkPathAt)}, kCallBudget);
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
          const abi::GuestAddr dst = kApkPathAt + 0x100u;
          const std::uint8_t poison[32] = {0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
                                           0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
                                           0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee};
          mem->Write(dst, poison, sizeof poison);
          const std::string len = via_got(0x9ddf0u, {abi::Value32::Ptr(kApkPathAt)});
          const std::string cmp = via_got(0x9df18u, {abi::Value32::Ptr(kApkPathAt), abi::Value32::Ptr(kApkPathAt)});
          const std::string cpy = via_got(0x9df74u, {abi::Value32::Ptr(dst), abi::Value32::Ptr(kApkPathAt)});
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
        mem->Read(scene_slot, &final_scene, 4);
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
  say_guest_log();
  say_gl();
  say_dl();
  say_syscalls();
  say_trapped();
  return out;
}

}  // namespace

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
