/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The lane that asks the question M1 actually turns on: can RetroBug load and call the **device's own
 * 32-bit bionic `libc.so`**?
 *
 * Why the device's file rather than one built here. `tools/build-guest-libs.sh` could relink `libc.a` into
 * a `libc.so`, and `docs/34` §1.3 records why that is a shortcut rather than an equivalence: `libc.a` is
 * bionic's *static* variant, and a real guest expects the dynamic one, with `__libc_preinit`,
 * `libc_init_dynamic`, the allocator's init and TLS setup. The tablet's `/system/lib/libc.so` — which is
 * `/apex/com.android.runtime/lib/bionic/libc.so`, 1,267,328 bytes, ELF32 ARM DYN — *is* that artifact, and
 * `tests/guest/oracle_libc.c` links against the same file through the ordinary `DT_NEEDED` path. So both
 * sides of every comparison below are the same machine code, and a difference is RetroBug's and nobody
 * else's. That is the strongest evidence this project can currently produce about a real system library,
 * and it raises no licence question, because the bytes stay on the device that shipped them.
 *
 * **What this lane expects to fail on, and why that is a deliverable.** Three things, each measured from
 * the image rather than assumed, each asserted below so that a change in bionic is a red build and not a
 * surprise during a launch:
 *
 *   - **Eleven `STT_GNU_IFUNC` exports** — `memcpy`, `memmove`, `memset`, `strlen`, `strcmp`, `strcpy`,
 *     `stpcpy`, `strcat`, `__strcat_chk`, `__strcpy_chk`, `__memset_chk`. For those, `st_value` is a
 *     *resolver*, and the address a caller wants is what the resolver returns, which means running guest
 *     code during relocation. C05 cannot, so this lane does not pretend: it asserts the eleven names and
 *     calls none of them. They are the first routines every app calls, which makes IFUNC C06's first
 *     feature rather than a corner of one.
 *   - **One `R_ARM_IRELATIVE` and three TLS relocations** (`R_ARM_TLS_DTPMOD32` ×2, `R_ARM_TLS_TPOFF32` ×1),
 *     which the loader collects in `unsupported_relocs` instead of applying wrongly. `DT_FLAGS` says
 *     `STATIC_TLS`, so those three are fillable from an initial-thread block — once one exists.
 *   - **Nine GLOBAL undefined symbols** that are the linker's own face: `dlopen`, `dlclose`, `dlsym`,
 *     `dladdr`, `dlerror`, `dl_unwind_find_exidx`, `android_dlopen_ext`,
 *     `android_get_application_target_sdk_version`, `__loader_shared_globals`, with `DT_NEEDED` naming
 *     `ld-android.so` and `libdl.so`. The host-provided surface a real libc needs is therefore nine names,
 *     not the ~1,300 it exports. That is the most actionable number in this file, and it is C11's
 *     "what must the host provide" question answered with a list.
 *
 * The asserted rows are consequently chosen to need none of the above: string scanning and numeric
 * conversion, which are ordinary `STT_FUNC` code with no IFUNC, no TLS and no constructor. The deliberate
 * exceptions are the last group — `snprintf`, `qsort` — which are tried *because* they may need what
 * RetroBug lacks. `qsort`'s comparator is a guest function in another library, and `snprintf` reaches
 * stdio's `FILE` state; both are reported, and the report is the result.
 *
 * Both tiers run every row. Where Tier I disagrees and Tier J does not, that is `docs/30` §Step 24's open
 * defect surfacing on a new corpus, and it is named as such rather than smoothed over.
 *
 * --------------------------------------------------------------------------- what this image shows
 *
 * Measured here, on the pulled file, rather than taken from a document:
 *
 *   - All four special relocations — the IRELATIVE and the three TLS ones — have **`r_sym == 0`**, which
 *     in AArch32 means "this module": libc's own TLS block and one of its own function pointers. That,
 *     together with `DT_FLAGS` carrying `STATIC_TLS`, is why they are fillable without a dynamic-TLS
 *     model. It is also why the loader leaving them unfilled is not a clean fault: bionic writes those
 *     three slots unconditionally during load, so a guest that later reaches them reads garbage rather
 *     than trapping. `docs/30` records that as a decision for the owner rather than this lane flipping
 *     C05's refuse-versus-record semantics on its own.
 *   - The string `"/dev/cpu_variant:arm"` is in the image at file offset 120130, which identifies the
 *     eleven IFUNCs as bionic's `dynamic_function_dispatch.cpp` set. What follows from that is the
 *     useful part, and it is second-hand — upstream read by a sub-agent at `android-16.0.0_r1`, **[1A]**
 *     and not re-verified here: those arm32 resolvers take an `hwcap` argument they *ignore*, and select
 *     by `openat("/dev/cpu_variant:arm")`, falling back to a generic implementation on `ENOENT`. Two
 *     consequences: RetroBug needs no auxv for them, and it does need its guest `openat` path working
 *     *before* IFUNC can be resolved — which is why IFUNC lands in C06, next to the syscall surface, and
 *     not in C05 alone.
 *   - Also **[1A]**, and worth knowing before implementing anything: bionic's linker *re-calls* the
 *     resolver for every relocation naming an IFUNC symbol rather than caching it, so a resolver must be
 *     idempotent; and for `R_ARM_IRELATIVE` in REL form the value written is
 *     `resolver(load_bias + *slot)` — the addend comes out of the slot itself, not from the entry.
 *   - `DF_1_NOW`/`DT_BIND_NOW` turn out to be a non-question for a guest libc: bionic never does lazy
 *     binding (it applies `.rel.plt` eagerly and ignores the flag), so all nine linker-facing names must
 *     be non-null at load, and an unresolved non-weak one fails the whole module. RetroBug's loader
 *     already refuses that way, which is a coincidence worth recording rather than a design to take
 *     credit for.
 */
#include "rb/abi/abi.h"
#include "rb/exec/interp.h"
#include "rb/exec/jit.h"
#include "rb/linker/guest_linker.h"
#include "rb/loader/elf_defs.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/syscall/monitor.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_DEVICE_LIBC_PATH
#define RB_DEVICE_LIBC_PATH "/data/local/tmp/rb_device_libc.so"
#endif
#ifndef RB_GUEST_APK_PATH
#define RB_GUEST_APK_PATH "/data/local/tmp/rb_guestprobe.apk"
#endif
#ifndef RB_IFUNC_LIBC_USER_PATH
#define RB_IFUNC_LIBC_USER_PATH "/data/local/tmp/librbifunclibc.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace elf = rb::loader::elf;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ",
               detail.c_str());
}

#define CHECK(...)                                     \
  do {                                                 \
    ++checks;                                          \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__);   \
  } while (0)

#define CHECK_CTX(cond, detail)                        \
  do {                                                 \
    ++checks;                                          \
    if (!(cond)) fail(#cond, __LINE__, detail);         \
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

std::string Und(const char* name) {
  const std::string n(name);
  return n.substr(0, n.find('@'));
}

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;
constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
/// Where the lane's strings, arrays and out-parameters live. Mapped read/write, and not a page the
/// loader claims, so a guest write past the end of a buffer lands somewhere the lane can read back.
constexpr exec::GuestAddr kData = 0x0090'0000u;
/// Nothing is mapped here: a symbol the host has to provide but this lane does not implement faults with
/// its name in the outcome, rather than returning a plausible zero.
constexpr std::uint32_t kUnimplemented = 0x0bad'0000u;

// Every input string gets its own page-offset. Reusing one address for two rows is how a test starts
// asserting `strspn` against an accept set that a previous row overwrote, and reads the result as an
// engine bug. `oracle_libc.c` uses the same literals, at the same lengths, and reports every pointer
// result as an offset into them.
constexpr exec::GuestAddr kHay = kData + 0x000;
constexpr exec::GuestAddr kNeedle = kData + 0x040;
constexpr exec::GuestAddr kAbsent = kData + 0x080;
constexpr exec::GuestAddr kQuiet = kData + 0x0c0;
constexpr exec::GuestAddr kQuick = kData + 0x100;
constexpr exec::GuestAddr kAbc = kData + 0x140;
constexpr exec::GuestAddr kAbd = kData + 0x180;
constexpr exec::GuestAddr kDigits = kData + 0x1c0;
constexpr exec::GuestAddr kDecimal = kData + 0x200;
constexpr exec::GuestAddr kNum = kData + 0x240;
constexpr exec::GuestAddr kOctal = kData + 0x280;
constexpr exec::GuestAddr kDbl = kData + 0x2c0;
constexpr exec::GuestAddr kDbl2 = kData + 0x300;
constexpr exec::GuestAddr kDbl3 = kData + 0x340;
constexpr exec::GuestAddr kPlus42 = kData + 0x380;
constexpr exec::GuestAddr kNotNumber = kData + 0x3c0;
constexpr exec::GuestAddr kBig = kData + 0x400;
constexpr exec::GuestAddr kTokBuf = kData + 0x440;
constexpr exec::GuestAddr kComma = kData + 0x480;
constexpr exec::GuestAddr kFmt = kData + 0x4c0;
constexpr exec::GuestAddr kXy = kData + 0x500;
constexpr exec::GuestAddr kOutPtr = kData + 0x540;  ///< the `char**` out-parameter
constexpr exec::GuestAddr kArr = kData + 0x580;
constexpr exec::GuestAddr kSnprintfBuf = kData + 0x600;
/**
 * A thread pointer, for every row rather than only the TLS one.
 *
 * bionic reaches `errno` through TPIDRURW, so any call that can report an error — `atoi` on an overflow,
 * `snprintf`, `strtol`, `getpid` — walks `tp → [tp+4] → +0x29c` before it does anything else. With `tp`
 * zero that chain ends in a read of guest address 4, which RetroBug now reports cleanly instead of
 * aborting, but it is still a failure the *harness* caused rather than one the engine has. Seeding it costs
 * six lines and every row below stops being about the thread pointer and goes back to being about what it
 * claims to test.
 *
 * The block sits far enough above the strings that `+0x29c` stays inside the mapping, so an `errno` write
 * lands on real guest memory the lane could read back.
 */
constexpr exec::GuestAddr kTp = kData + 0x2000;
constexpr exec::GuestAddr kTlsBlock = kData + 0x2020;
constexpr std::size_t kDataBytes = 32u << 10;
constexpr std::size_t kArrN = 9;

struct Rig {
  std::unique_ptr<GuestMemory> mem;
  std::optional<loader::GuestModule> libc;
  std::optional<loader::GuestModule> probe;
  std::unique_ptr<abi::AbiContext> abi_ctx;
  /// Present because five of the eight IFUNC resolvers probe `/dev/cpu_variant:arm` with a real `openat`,
  /// and without a monitor an SVC is a *refusal* rather than a syscall. What the monitor answers is not the
  /// interesting part — that a resolver survives `ENOENT`/`ENOSYS` and falls back to its generic body is
  /// the thing being tested — so the tally it collects is printed at the end of the IFUNC row.
  std::unique_ptr<syscall::Monitor> monitor;
  std::unique_ptr<exec::InterpExecutor> interp;
  std::unique_ptr<exec::JitExecutor> jit;
  std::vector<std::string> libc_asked;   ///< names `libc.so` itself asked the host for
  std::vector<std::string> probe_asked;  ///< and the same for the probe library
  std::size_t ifunc_resolved = 0;
  std::vector<std::string> ifunc_unresolved;  ///< names whose resolver would not return
  std::vector<std::string> ifunc_log;         ///< name -> implementation, with the tier that got it
  bool ok = false;
};

/// The nine names a real `libc.so` asks the *host* for. Hard-coded as a requirement list, so that a
/// tenth is a change in what must be built rather than something noticed at launch.
const std::vector<std::string>& LinkerFacingNames() {
  static const std::vector<std::string> v = {
      "__loader_shared_globals", "android_dlopen_ext",
      "android_get_application_target_sdk_version", "dl_unwind_find_exidx", "dladdr", "dlclose",
      "dlerror", "dlopen", "dlsym"};
  return v;
}

/// The smallest stored-entry zip reader, so this lane can reach `cmpInt` inside the probe library.
/// Copied from `guest_apk_test.cpp` rather than shared: it is a test's way of getting at bytes, and a
/// header for one 40-line function would be a header for one function. If a third lane needs it, that
/// is the point at which it becomes a component.
std::optional<std::vector<std::uint8_t>> apk_entry(const std::vector<std::uint8_t>& zip,
                                                  const std::string& want, std::string* err) {
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return zip[at] | (static_cast<std::uint32_t>(zip[at + 1]) << 8);
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t { return u16(at) | (u16(at + 2) << 16); };
  if (zip.size() < 22) {
    if (err) *err = "file is too small to be a zip";
    return std::nullopt;
  }
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
      if (err) *err = "central directory is malformed";
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
        if (err) *err = want + " is compressed; stored entries only";
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

bool write_cstr(GuestMemory& mem, exec::GuestAddr at, const char* s) {
  return mem.Write(at, s, static_cast<std::uint32_t>(std::strlen(s) + 1));
}

/// The lane's `resolve_ifunc` hook: run the resolver on Tier J, then Tier I, and take what it returns.
/// Shared by the rig's own libc load and by the `GuestLinker` row, which binds imports through it.
std::optional<std::uint32_t> RunResolver(Rig& rig, const loader::IfuncSlot& s) {
  for (exec::Executor* engine : {static_cast<exec::Executor*>(rig.jit.get()),
                                 static_cast<exec::Executor*>(rig.interp.get())}) {
    rig.abi_ctx->set_executor(*engine);
    exec::RunResult out{};
    exec::MachineState exit_state{};
    const abi::Ret32 r = rig.abi_ctx->CallGuest(
        s.resolver.tagged(), {abi::Value32::U32(s.slot_value)}, abi::Shorty::For("ILI"), 200'000,
        &out, &exit_state);
    if (out.reason != exec::StopReason::Returned) continue;
    ++rig.ifunc_resolved;
    rig.ifunc_log.push_back(std::string{s.name.empty() ? "<IRELATIVE>" : s.name} + " -> 0x" +
                            Hex(static_cast<std::uint32_t>(r.bits)) + " on Tier " +
                            (engine->tier() == exec::CodeTier::Interp ? "I" : "J"));
    return static_cast<std::uint32_t>(r.bits);
  }
  rig.ifunc_unresolved.push_back(s.name.empty() ? "<IRELATIVE>" : s.name);
  return std::nullopt;
}

Rig build() {
  Rig rig;
  const auto image = read_file(RB_DEVICE_LIBC_PATH);
  if (image.empty()) {
    ++checks;
    fail("read the device's 32-bit libc.so", __LINE__,
         std::string{"nothing at "} + RB_DEVICE_LIBC_PATH + " — on a 32-bit-capable device: "
             "adb pull /system/lib/libc.so build/device-libs/libc.so, push it there");
    return rig;
  }
  std::fprintf(stderr, "# guest libc image: %zu bytes\n", image.size());

  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string err;
  rig.mem = GuestMemory::Create(64u << 20, model, &err);
  if (!rig.mem) {
    ++checks;
    fail("create guest memory", __LINE__, err);
    return rig;
  }
  if (!rig.mem->Map(kData, kDataBytes, kRead | kWrite, &err) ||
      !rig.mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err)) {
    ++checks;
    fail("map the data and stack windows", __LINE__, err);
    return rig;
  }
  // The strings, byte for byte as `oracle_libc.c` has them.
  write_cstr(*rig.mem, kHay, "the quick brown fox jumps over the lazy dog");
  write_cstr(*rig.mem, kNeedle, "over");
  write_cstr(*rig.mem, kAbsent, "zebra");
  write_cstr(*rig.mem, kQuiet, "the quiet");
  write_cstr(*rig.mem, kQuick, "the quick");
  write_cstr(*rig.mem, kAbc, "abc");
  write_cstr(*rig.mem, kAbd, "abd");
  write_cstr(*rig.mem, kDigits, "00123abcXY");
  write_cstr(*rig.mem, kDecimal, "0123456789");
  write_cstr(*rig.mem, kNum, "  -0x1f4garbage");
  write_cstr(*rig.mem, kOctal, "-020");
  write_cstr(*rig.mem, kDbl, "-1.7976931348623157e308xyz");
  write_cstr(*rig.mem, kDbl2, "  0.1");
  write_cstr(*rig.mem, kDbl3, "1e-320");
  write_cstr(*rig.mem, kPlus42, "  +42xyz");
  write_cstr(*rig.mem, kNotNumber, "not a number");
  write_cstr(*rig.mem, kBig, "9223372036854775808");
  write_cstr(*rig.mem, kTokBuf, "one,,two,three,");
  write_cstr(*rig.mem, kComma, ",");
  write_cstr(*rig.mem, kFmt, "%d/%s/%c");
  write_cstr(*rig.mem, kXy, "xy");
  const std::int32_t arr[kArrN] = {5, 3, 9, 1, -7, 0, 2147483647, -2147483647 - 1, 42};
  rig.mem->Write(kArr, arr, sizeof arr);
  const std::uint32_t zero = 0;
  rig.mem->Write(kOutPtr, &zero, 4);
  // The thread pointer chain: `tp` -> `[tp+4]` is the thread's `bionic_tls`, and `__errno()` adds 0x29c to
  // that. Both words are the lane's own choice of address; the 0x29c is bionic's, from its disassembly.
  const std::uint32_t block = kTlsBlock;
  rig.mem->Write(kTp + 4, &block, 4);

  loader::LoadOptions opts;
  // The TLS slots stay unresolved here and only here: this lane is measuring a real `libc.so` with no
  // guest linker yet, and `LoadOptions::tolerate_unresolved_relocs` is exactly the flag for "the caller
  // will finish this". The default is the opposite — see `a_load_without_that_flag_refuses_and_names_the_slots`.
  opts.tolerate_unresolved_relocs = true;
  opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    rig.libc_asked.push_back(name);
    return kUnimplemented;
  };
  // The bridge and both tiers exist *before* the load now, which is the whole point of the hook's shape:
  // C05 still runs no guest code, it asks somebody who does. Resolution happens inside the load because
  // RELRO would otherwise make the slot unwritable.
  rig.abi_ctx = std::make_unique<abi::AbiContext>(*rig.mem, kStackTop, kReturnAddress);
  rig.abi_ctx->state().tp = kTp;  // see the note above: no row should fail on a missing thread pointer
  rig.monitor = std::make_unique<syscall::Monitor>();
  rig.monitor->set_ids(4242, 4243);  // the guest's own identity, invented here the way the monitor requires
  rig.abi_ctx->set_monitor(rig.monitor.get());
  rig.interp = std::make_unique<exec::InterpExecutor>(*rig.mem, kReturnAddress);
  rig.jit = std::make_unique<exec::JitExecutor>(*rig.mem, kReturnAddress);
  opts.resolve_ifunc = [&](const loader::IfuncSlot& s) { return RunResolver(rig, s); };
  rig.libc = loader::LoadFromBytes(*rig.mem, image, "libc.so", opts, &err);
  if (!rig.libc) {
    ++checks;
    fail("load the device's libc.so", __LINE__, err);
    return rig;
  }
  std::size_t thumb = 0;
  std::size_t ifunc = 0;
  for (const auto& s : rig.libc->exports) {
    if (s.thumb) ++thumb;
    if (s.ifunc) ++ifunc;
  }
  std::fprintf(stderr, "# libc: %zu .dynsym functions, %zu Thumb-tagged, %zu IFUNC; %zu names asked "
                       "of the host\n",
               rig.libc->exports.size(), thumb, ifunc, rig.libc_asked.size());

  // The probe library, for `cmpInt`. Its imports are resolved against **libc**, which is the first time
  // RetroBug has cross-linked a guest library against bionic rather than against a relinked NDK archive.
  const auto apk = read_file(RB_GUEST_APK_PATH);
  if (!apk.empty()) {
    std::string zerr;
    if (auto so = apk_entry(apk, "lib/armeabi-v7a/libmathprobe.so", &zerr)) {
      loader::LoadOptions popts;
      popts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
        rig.probe_asked.push_back(name);
        if (const auto fn = rig.libc->FindExport(name)) return fn->tagged();
        return kUnimplemented;
      };
      rig.probe = loader::LoadFromBytes(*rig.mem, *so, "libmathprobe.so", popts, &err);
      if (!rig.probe) std::fprintf(stderr, "# the probe library did not load: %s\n", err.c_str());
    } else {
      std::fprintf(stderr, "# %s\n", zerr.c_str());
    }
  }

  // The bridge and both tiers were built before the load, not here — IFUNC resolution happens inside it, so
  // an executor has to exist first. See the note at `opts.resolve_ifunc`.
  rig.ok = true;
  return rig;
}

struct Outcome {
  exec::StopReason reason = exec::StopReason::Internal;
  std::uint64_t bits = 0;
  std::string detail;
  unsigned steps = 0;
  bool ok() const { return reason == exec::StopReason::Returned; }
};

Outcome call(exec::Executor& engine, Rig& rig, const char* name, const abi::Args32& args,
             const char* shorty) {
  Outcome out;
  const auto fn = rig.libc->FindExport(name);
  if (!fn) {
    out.detail = std::string{"libc.so does not export "} + name;
    return out;
  }
  rig.abi_ctx->set_executor(engine);
  exec::RunResult result{};
  exec::MachineState exit_state{};
  const abi::Ret32 ret = rig.abi_ctx->CallGuest(fn->tagged(), args, abi::Shorty::For(shorty),
                                                4'000'000, &result, &exit_state);
  out.reason = result.reason;
  out.bits = ret.bits;
  out.detail = result.detail;
  out.steps = result.instructions_executed;
  return out;
}

const char* Tier(exec::Executor& e) {
  return e.tier() == exec::CodeTier::Interp ? "I" : "J";
}

/// Both tiers over one call, with the refusal reported as loudly as the wrong answer.
template <typename Fn>
void both_tiers(Rig& rig, const char* name, const char* shorty, const abi::Args32& args, Fn&& fn) {
  for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                       static_cast<exec::Executor*>(rig.jit.get())}) {
    const Outcome got = call(*engine, rig, name, args, shorty);
    if (!got.ok()) {
      ++checks;
      fail(name, __LINE__, std::string{"Tier "} + Tier(*engine) + ": reason=" +
                               std::to_string(static_cast<int>(got.reason)) + " " + got.detail);
      continue;
    }
    fn(*engine, got);
  }
}

void int_row(Rig& rig, const char* name, const char* shorty, abi::Args32 args, std::int32_t want) {
  both_tiers(rig, name, shorty, args, [&](exec::Executor& e, const Outcome& got) {
    CHECK_CTX(static_cast<std::int32_t>(got.bits) == want,
              std::string{name} + " on Tier " + Tier(e) + " = " +
                  std::to_string(static_cast<std::int32_t>(got.bits)) + ", hardware said " +
                  std::to_string(want));
    std::fprintf(stderr, "[libc] %-22s hardware=%-12d Tier %s=%-12d\n", name, want, Tier(e),
                 static_cast<std::int32_t>(got.bits));
  });
}

/**
 * Like `int_row`, for a call whose contract is the **sign** and not the magnitude.
 *
 * `strncmp` is specified to return "an integer less than, equal to, or greater than zero"; bionic's arm32
 * version happens to return `-1` where the byte difference is `-2` (`'c'` against `'e'`), and RetroBug —
 * running that same code on both tiers — returned `-2`. The first draft of this row asserted the exact
 * value, so it failed on a difference the standard permits, which is the mirror image of the mistake this
 * lane exists to catch: a test that over-claims about an ABI makes the next real finding look like noise.
 *
 * The magnitudes are still printed, because the difference between `-1` and `-2` is itself a fact about
 * bionic's implementation worth keeping in the log rather than discarding.
 */
void sign_row(Rig& rig, const char* name, const char* shorty, abi::Args32 args, std::int32_t want) {
  const auto sign = [](std::int32_t v) { return (v > 0) - (v < 0); };
  both_tiers(rig, name, shorty, args, [&](exec::Executor& e, const Outcome& got) {
    const std::int32_t v = static_cast<std::int32_t>(got.bits);
    CHECK_CTX(sign(v) == sign(want), std::string{name} + " on Tier " + Tier(e) + " = " +
                                         std::to_string(v) + ", hardware said " + std::to_string(want) +
                                         " — and the contract is the sign, so only the sign may differ");
    std::fprintf(stderr, "[libc] %-22s hardware=%-12d Tier %s=%-12d (sign %d)\n", name, want, Tier(e), v,
                 sign(v));
  });
}

/// A pointer-returning call, compared as an offset into `kHay` — the only form that survives the trip
/// between two address spaces. `want < 0` means hardware returned NULL, which is a *different* answer
/// from "the empty string at offset 0" and is asserted as such.
void ptr_row(Rig& rig, const char* name, const char* shorty, abi::Args32 args, long want) {
  both_tiers(rig, name, shorty, args, [&](exec::Executor& e, const Outcome& got) {
    if (want < 0) {
      CHECK_CTX(got.bits == 0, std::string{name} + " on Tier " + Tier(e) + " = 0x" +
                                   Hex64(got.bits) + ", hardware said NULL");
      std::fprintf(stderr, "[libc] %-22s hardware=NULL      Tier %s=0x%s\n", name, Tier(e),
                   Hex64(got.bits).c_str());
      return;
    }
    const std::uint64_t off = got.bits - kHay;
    CHECK_CTX(off == static_cast<std::uint64_t>(want),
              std::string{name} + " on Tier " + Tier(e) + " = +" + std::to_string(off) +
                  ", hardware said +" + std::to_string(want));
    std::fprintf(stderr, "[libc] %-22s hardware=+%-11ld Tier %s=+%-11llu\n", name, want, Tier(e),
                 static_cast<unsigned long long>(off));
  });
}

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += x + " ";
  return s;
}

// ------------------------------------------------------------------ what the image demands

void the_load_reports_exactly_what_retrobug_cannot_yet_do() {
  Rig rig = build();
  if (!rig.ok) return;

  // 1. The relocation kinds the loader refused, grouped by type. `unsupported_reloc_types` is the raw
  //    `r_type` list the loader recorded, so this compares RetroBug's reading of the image against
  //    `llvm-readelf`'s rather than against a transcription of it — or, as an earlier draft of this row
  //    did, against a *printf* of it, which couples an assertion to a message's formatting.
  std::size_t irelative = 0, dtpmod = 0, dtpoff = 0, tpoff = 0, other = 0;
  const auto types = rig.libc->unsupported_reloc_types;
  for (const std::uint32_t t : types) {
    switch (t) {
      case elf::R_ARM_IRELATIVE: ++irelative; break;
      case elf::R_ARM_TLS_DTPMOD32: ++dtpmod; break;
      case elf::R_ARM_TLS_DTPOFF32: ++dtpoff; break;
      case elf::R_ARM_TLS_TPOFF32: ++tpoff; break;
      default: ++other; break;
    }
  }
  for (const auto& r : rig.libc->unsupported_relocs) {
    std::fprintf(stderr, "[libc] unsupported reloc: %s\n", r.c_str());
  }
  std::fprintf(stderr, "[libc] %zu refused relocations: %zu IRELATIVE, %zu TLS_DTPMOD32, "
                       "%zu TLS_DTPOFF32, %zu TLS_TPOFF32, %zu other\n",
               types.size(), irelative, dtpmod, dtpoff, tpoff, other);
  // `R_ARM_IRELATIVE` is no longer in this list, and that is the change, not an omission: it is handled
  // now (recorded into `ifunc_slots`, slot written with the resolver as the placeholder), so a refusal
  // count of zero for it is the assertion that says so.
  CHECK_CTX(irelative == 0, "R_ARM_IRELATIVE was refused rather than recorded; " +
                                std::to_string(irelative) + " slot(s)");
  // What remains is TLS, and only TLS. The per-kind counts are deliberately *not* asserted: `2 DTPMOD /
  // 0 DTPOFF / 1 TPOFF` is the Tab S9's `libc.so`, and the Reno 5's older bionic has none of the first
  // and the same one of the last — a literal here is a claim about a phone. Checking a new image's
  // distribution is a manual step against that image, and the command belongs with it:
  //   llvm-readelf -r /path/to/libc.so | awk '$3 ~ /^R_ARM_TLS/ {print $3}' | sort | uniq -c
  CHECK_CTX(dtpmod + dtpoff + tpoff == types.size(),
            std::to_string(types.size()) + " relocation(s) were refused but only " +
                std::to_string(dtpmod + dtpoff + tpoff) +
                " are TLS-family, so something is being refused that this row does not account for");
  CHECK_CTX(types.size() >= 1u,
            "nothing was refused, which contradicts the TLS relocations every Android libc.so has; the "
            "loader is silently applying, or silently dropping, what it should hand to the linker");

  // DT_INIT_ARRAY, which the loader had no `.dynamic` case for until §Step 32 — while `DT_INIT` is zero
  // on this image, so the array is the *only* path by which libc's constructors could ever have been
  // noticed. `llvm-readelf -d` says 20 bytes (5 entries) on both devices' builds; that number is printed
  // rather than asserted, because the assertion that survives a different bionic is the structural one:
  // there is at least one entry, and every entry is a real, executable address inside this module. An
  // array read from the wrong offset or the wrong endianness fails that immediately — it would name file
  // headers, or memory outside the mapping.
  CHECK_CTX(rig.libc->init.addr == 0u,
            "this libc sets DT_INIT after all, so the claim above needs re-deriving rather than being "
            "quietly kept");
  CHECK_CTX(!rig.libc->init_array.empty(),
            "DT_INIT_ARRAY recorded nothing for an image whose only constructor tag is that array — the "
            "same silent 'this module has no startup work' that the missing .dynamic case produced");
  for (std::size_t i = 0; i < rig.libc->init_array.size(); ++i) {
    const auto fn = rig.libc->init_array[i];
    // Executable-and-mapped, which is what an array read from the wrong offset or the wrong
    // endianness fails at once: it would name file headers, or a data page, or nothing at all. A
    // page-count bound was the tempting version of this check and would have been wrong on its own
    // numbers — this libc is ~300 pages, so any small constant turns a passing load into a failure.
    const unsigned prot = rig.mem->ProtAt(fn.addr);
    CHECK_CTX(fn.addr >= rig.libc->base && (prot & mem::kExec) != 0u,
              "init_array[" + std::to_string(i) + "] = 0x" + Hex(fn.addr) +
                  " is not an executable address above this module's base (prot=0x" + Hex(prot) +
                  "), so the array was read from the wrong place");
  }
  std::fprintf(stderr, "[libc] init_array: DT_INIT=0x%s entries=%zu first=0x%s\n",
               Hex(rig.libc->init.addr).c_str(), rig.libc->init_array.size(),
               rig.libc->init_array.empty() ? "0" : Hex(rig.libc->init_array[0].addr).c_str());
  CHECK_CTX(other == 0, "the loader refused " + std::to_string(other) +
                            " kinds beyond the four expected. A new kind is a new feature and should be "
                            "named here before it is implemented.");
  CHECK_CTX(rig.libc->ifunc_slots.size() >= 1u,
            "the IRELATIVE was neither refused nor recorded, so it went missing entirely");

  // 2. The IFUNC set, by name. "Eleven IFUNCs" without the list is a number nobody can act on.
  std::vector<std::string> ifuncs;
  for (const auto& s : rig.libc->exports) {
    if (s.ifunc) ifuncs.push_back(Und(s.name.c_str()));
  }
  std::sort(ifuncs.begin(), ifuncs.end());
  ifuncs.erase(std::unique(ifuncs.begin(), ifuncs.end()), ifuncs.end());
  std::vector<std::string> want = {"__memset_chk", "__strcat_chk", "__strcpy_chk", "memmove", "memcpy",
                                   "memset",      "strcat",      "stpcpy",      "strcmp",     "strcpy",
                                   "strlen"};
  std::sort(want.begin(), want.end());
  std::fprintf(stderr, "[libc] IFUNC exports: %s\n", Join(ifuncs).c_str());
  CHECK_CTX(ifuncs == want,
            "the IFUNC set is not the measured one — got [" + Join(ifuncs) + "] want [" + Join(want) +
                "]. A resolver RetroBug cannot run is a function no guest can call, so this list is the "
                "spec for C06's first feature.");

  // 3. What the host has to provide, taken from the loader's own record of the image's undefined symbols
  //    rather than from a count of resolver calls. Two reasons, and the first is the interesting one: a
  //    resolver call has no binding on it, so the weak imports and one nameless LOCAL entry are
  //    indistinguishable from the nine the guest really needs — and an assertion that mixes them is wrong
  //    in a way that reads like the *image* changed. bionic's own linker fails the whole module on an
  //    unresolved non-weak symbol and permits a null weak one, which is the distinction this row states.
  std::vector<std::string> required;
  std::vector<std::string> optional;
  for (const auto& s : rig.libc->imports) {
    if (s.defined) continue;
    if (s.name.empty()) {
      optional.push_back("<unnamed>");
      continue;
    }
    (s.weak ? optional : required).push_back(Und(s.name.c_str()));
  }
  const auto sorted = [](std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  };
  const std::vector<std::string> wanted = sorted(LinkerFacingNames());
  std::fprintf(stderr, "[libc] requires %zu host symbols: %s\n", required.size(),
               Join(sorted(required)).c_str());
  std::fprintf(stderr, "[libc] weak/optional: %s\n", Join(sorted(optional)).c_str());
  CHECK_CTX(sorted(required) == wanted,
            "the host-provided surface is not the nine linker names in the image — got [" +
                Join(sorted(required)) + "] want [" + Join(wanted) + "]");
  CHECK_CTX(!optional.empty(),
            "no weak imports at all? the image used to carry __loader_add_thread_local_dtor and two "
            "siblings, so their absence means the loader stopped recording undefined symbols");
  std::fprintf(stderr, "[libc] needed: %s\n", Join(rig.libc->needed).c_str());
  CHECK_CTX(rig.libc->needed.size() == 2u,
            "DT_NEEDED is [" + Join(rig.libc->needed) + "]; readelf said ld-android.so libdl.so");
}

// ------------------------------------------------------------------ the calls

/**
 * String scanning: `strncmp`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strtok_r`.
 *
 * Ordinary `STT_FUNC` code — no IFUNC, no TLS, no constructor — which is what makes it the first honest
 * thing to ask a real bionic. Two of the expected values read like typos and are not:
 * `strcspn(hay, "over") == 2` because the `'e'` of "the" is in the reject set, and `strchr(hay, 0)`
 * returns a pointer to the terminator at offset 43. Both were worked out again against the device's
 * output before being written here, and the device won both times.
 */
void string_scanning_matches_bionic() {
  Rig rig = build();
  if (!rig.ok) return;
  using abi::Value32;
  const Value32 hay = Value32::Ptr(kHay);
  const Value32 needle = Value32::Ptr(kNeedle);

  sign_row(rig, "strncmp", "ILIII", {hay, Value32::Ptr(kQuiet), Value32::U32(7)}, 0);
  sign_row(rig, "strncmp", "ILIII", {hay, Value32::Ptr(kQuick), Value32::U32(7)}, 0);
  // The same pair with n one larger: the row that separates "compared n bytes" from "compared to the
  // NUL", which is the difference between 0 and nonzero here.
  sign_row(rig, "strncmp", "ILIII", {hay, Value32::Ptr(kQuiet), Value32::U32(8)}, -1);
  sign_row(rig, "strncmp", "ILIII", {Value32::Ptr(kAbc), Value32::Ptr(kAbd), Value32::U32(3)}, -1);
  sign_row(rig, "strncmp", "ILIII", {Value32::Ptr(kAbd), Value32::Ptr(kAbc), Value32::U32(3)}, 1);
  sign_row(rig, "strncmp", "ILIII", {hay, needle, Value32::U32(0)}, 0);

  ptr_row(rig, "strchr", "ILII", {hay, Value32::S32('q')}, 4);
  ptr_row(rig, "strchr", "ILII", {hay, Value32::S32('z')}, 37);
  ptr_row(rig, "strchr", "ILII", {hay, Value32::S32(0)}, 43);
  ptr_row(rig, "strrchr", "ILII", {hay, Value32::S32('o')}, 41);
  ptr_row(rig, "strstr", "ILLI", {hay, needle}, 26);
  // The not-found case of `strstr` is *not* asserted, and the reason is the finding: bionic's `strstr`
  // reaches `strlen` when it has to scan to the end, `strlen` is one of the eleven IFUNCs, and the GOT slot
  // holds the *resolver* rather than its result — so the call runs the resolver as code, and the resolver's
  // first act is an `openat` syscall. That is `docs/30` §Step 26's evidence for how an unresolved IFUNC
  // actually behaves, and it is in the reported group where it can be observed instead of asserted away.
  int_row(rig, "strspn", "IZLL", {hay, Value32::Ptr(kQuick)}, 10);
  int_row(rig, "strcspn", "IZLL", {hay, needle}, 2);
  int_row(rig, "strspn", "IZLL", {Value32::Ptr(kDigits), Value32::Ptr(kDecimal)}, 5);

  // `strtok_r` keeps its cursor in a caller-owned pointer, so this is also the lane's only row that
  // makes four sequential guest calls and reads a mutated buffer back. The expected shape is the
  // oracle's: consecutive delimiters yield no empty token, and the trailing comma ends the run.
  std::uint32_t save = 0;
  rig.mem->Write(kOutPtr, &save, 4);
  const Value32 buf = Value32::Ptr(kTokBuf);
  const Value32 comma = Value32::Ptr(kComma);
  const Value32 out = Value32::Ptr(kOutPtr);
  std::string seen;
  for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                       static_cast<exec::Executor*>(rig.jit.get())}) {
    seen.clear();
    Value32 s = buf;
    for (int i = 0; i < 8; ++i) {
      const Outcome got = call(*engine, rig, "strtok_r", abi::Args32{s, comma, out}, "LLILL");
      if (!got.ok()) {
        ++checks;
        fail("strtok_r", __LINE__,
             std::string{"Tier "} + Tier(*engine) + ": " + got.detail);
        break;
      }
      if (got.bits == 0) break;
      char tok[16] = {};
      rig.mem->Read(static_cast<exec::GuestAddr>(got.bits), tok, sizeof tok - 1);
      seen += "[";
      seen += tok;
      seen += "]";
      s = Value32::Ptr(0);
    }
    CHECK_CTX(seen == "[one][two][three]",
              std::string{"strtok_r on Tier "} + Tier(*engine) + " gave " + seen +
                  ", hardware gave [one][two][three]");
    std::fprintf(stderr, "[libc] %-22s Tier %s=%s\n", "strtok_r", Tier(*engine), seen.c_str());
    write_cstr(*rig.mem, kTokBuf, "one,,two,three,");  // the next tier mutates the same buffer
  }
}

/**
 * Numeric conversion: `atoi`, `abs`, `strtol`, `strtod`.
 *
 * `strtod` is 3,580 bytes of correctly-rounded decimal conversion, the largest routine in the library
 * that needs no filesystem, and its expectations are bit patterns rather than decimals because a one-ulp
 * disagreement is invisible in `%f`. The three inputs are `-DBL_MAX`, the classic non-representable
 * literal `0.1`, and `1e-320`, which forces the subnormal path. The `endptr` offsets are asserted too:
 * a conversion that returns the right value and stops early is a different bug from one that gets both
 * wrong, and only the pair tells them apart.
 */
void numeric_conversion_matches_bionic() {
  Rig rig = build();
  if (!rig.ok) return;
  using abi::Value32;
  const Value32 out = Value32::Ptr(kOutPtr);

  int_row(rig, "atoi", "ILL", {Value32::Ptr(kPlus42)}, 42);
  int_row(rig, "atoi", "ILL", {Value32::Ptr(kNotNumber)}, 0);
  int_row(rig, "atoi", "ILL", {Value32::Ptr(kBig)}, 2147483647);
  int_row(rig, "abs", "IL", {Value32::S32(-2147483647)}, 2147483647);
  int_row(rig, "abs", "IL", {Value32::S32(-1)}, 1);

  struct LongRow {
    exec::GuestAddr at;
    std::int32_t base;
    std::int32_t want;
    std::uint32_t end_off;
  };
  const LongRow rows[] = {
      {kNum, 0, -500, 8}, {kNum, 10, 0, 4}, {kNum, 16, -500, 8}, {kOctal, 8, -16, 4},
  };
  for (const LongRow& r : rows) {
    std::uint32_t zero = 0;
    rig.mem->Write(kOutPtr, &zero, 4);
    both_tiers(rig, "strtol", "LLLI", {Value32::Ptr(r.at), out, Value32::S32(r.base)},
               [&](exec::Executor& e, const Outcome& got) {
                 CHECK_CTX(static_cast<std::int32_t>(got.bits) == r.want,
                           "strtol base " + std::to_string(r.base) + " on Tier " + Tier(e) + " = " +
                               std::to_string(static_cast<std::int32_t>(got.bits)) + ", hardware said " +
                               std::to_string(r.want));
                 std::uint32_t end = 0;
                 rig.mem->Read(kOutPtr, &end, 4);
                 CHECK_CTX(end >= r.at && end - r.at == r.end_off,
                           "strtol base " + std::to_string(r.base) + " on Tier " + Tier(e) +
                               " set *end to " + Hex(end) + ", hardware said +" +
                               std::to_string(r.end_off));
                 std::fprintf(stderr, "[libc] %-22s base=%-3d hardware=%-7d Tier %s=%-7d end=+%u\n",
                              "strtol", r.base, r.want, Tier(e),
                              static_cast<std::int32_t>(got.bits), end >= r.at ? end - r.at : 0u);
               });
  }

  struct DblRow {
    exec::GuestAddr at;
    std::uint64_t want;
    std::uint32_t end_off;
  };
  const DblRow drows[] = {{kDbl, 0xffef'ffff'ffff'ffffull, 23},
                          {kDbl2, 0x3fb9'9999'9999'999aull, 5},
                          {kDbl3, 0x0000'0000'0000'07e8ull, 6}};
  for (const DblRow& r : drows) {
    std::uint32_t zero = 0;
    rig.mem->Write(kOutPtr, &zero, 4);
    both_tiers(rig, "strtod", "DLLL", {Value32::Ptr(r.at), out}, [&](exec::Executor& e,
                                                                      const Outcome& got) {
      CHECK_CTX(got.bits == r.want, std::string{"strtod on Tier "} + Tier(e) + " = 0x" +
                                        Hex64(got.bits) + ", hardware said 0x" + Hex64(r.want));
      std::uint32_t end = 0;
      rig.mem->Read(kOutPtr, &end, 4);
      CHECK_CTX(end >= r.at && end - r.at == r.end_off,
                std::string{"strtod on Tier "} + Tier(e) + " set *end to " + Hex(end) +
                    ", hardware said +" + std::to_string(r.end_off));
      std::fprintf(stderr, "[libc] %-22s hardware=0x%s Tier %s=0x%s\n", "strtod",
                   Hex64(r.want).c_str(), Tier(e), Hex64(got.bits).c_str());
    });
  }
}

/**
 * `qsort`, with a comparator that lives in a *different guest library*.
 *
 * The interesting part is not the sort. It is that emulated bionic calls, by pointer, into a function
 * RetroBug never named and never placed an argument for — and that the call has to arrive in the right
 * ISA state, which is #27's bit-0 tag surviving a `blx` *inside libc* rather than through the bridge. A
 * comparator that decoded as ARM where it is Thumb gives either a wrong order or a fault, and the array
 * contents tell those two apart from each other.
 */
void libc_calls_back_into_a_guest_function() {
  Rig rig = build();
  if (!rig.ok) return;
  if (!rig.probe) {
    ++checks;
    fail("the probe library loaded, for cmpInt", __LINE__,
         std::string{"push "} + RB_GUEST_APK_PATH + " from tools/build-guest-apk.sh");
    return;
  }
  const auto cmp = rig.probe->FindExport("cmpInt");
  if (!cmp) {
    ++checks;
    fail("the probe exports cmpInt", __LINE__, "rebuild libmathprobe.so");
    return;
  }
  std::fprintf(stderr, "# cmpInt at 0x%s (%s state)\n", Hex(cmp->addr).c_str(),
               cmp->thumb ? "Thumb" : "ARM");
  using abi::Value32;
  both_tiers(rig, "qsort", "VLPILP",
             {Value32::Ptr(kArr), Value32::U32(kArrN), Value32::U32(4),
              Value32::Ptr(cmp->tagged())},
             [&](exec::Executor& e, const Outcome&) {
               std::int32_t out[kArrN] = {};
               rig.mem->Read(kArr, out, sizeof out);
               const std::int32_t want[kArrN] = {-2147483647 - 1, -7, 0, 1, 3, 5, 9, 42,
                                                 2147483647};
               CHECK_CTX(std::memcmp(out, want, sizeof out) == 0,
                         std::string{"qsort on Tier "} + Tier(e) + " produced the wrong order");
               std::fprintf(stderr, "[libc] %-22s Tier %s ->", "qsort", Tier(e));
               for (std::size_t i = 0; i < kArrN; ++i) std::fprintf(stderr, " %d", out[i]);
               std::fprintf(stderr, "\n");
             });
}

/**
 * The thread pointer, end to end: `mrc p15, #0, r0, c13, c0, #3` inside emulated bionic.
 *
 * `__errno()` in the device's `libc.so` is twelve bytes, and it is the whole of what RetroBug had to learn
 * to do in order to run any library code that reports an error:
 *
 *     76dec:  ee1d 0f70   mrc p15, #0x0, r0, c13, c0, #0x3   r0 = TPIDRURW
 *     76df0:  6840        ldr r0, [r0, #0x4]                 r0 = the thread's bionic_tls*
 *     76df2:  f500 7027   add.w r0, r0, #0x29c               r0 = &((bionic_tls*)->errno)
 *     76df6:  4770        bx lr
 *
 * So this row seeds `MachineState::tp` with a guest address, puts a second guest address four bytes above
 * it, and asks libc for the errno slot. The expected value is read out of that disassembly — the `+4` and
 * the `+0x29c` are the guest's own constants, not this file's — which makes it the first test in the
 * project that checks a *TLS-relative* computation against real code rather than against a model of it.
 *
 * It exists because the alternative was measured: before Tier J had a coprocessor handler, this same call
 * took down the process with `ASSERT_FALSE("Should raise coproc exception here")` in Dynarmic's AArch64
 * back end, and `atol`/`atoi`/`strtol` reach it through `errno`. A guest hitting `errno` is not an edge
 * case; it is arithmetic.
 */
void the_thread_pointer_reaches_a_guest_tls_block() {
  Rig rig = build();
  if (!rig.ok) return;
  if (rig.abi_ctx->state().tp != kTp) {
    ++checks;
    fail("the bridge carries a thread pointer into the callee", __LINE__,
         "tp is 0x" + Hex(rig.abi_ctx->state().tp));
  }
  // The offset bionic's own code adds is a property of *this build*, and asserting a remembered one was
  // the mistake: `+0x29c`, lifted from the Tab S9's disassembly, is wrong by 4 bytes on the Reno 5's
  // older bionic, and both tiers failed a row that was about the engine's fidelity to a constant in a
  // header. So the claim here is the one that holds for any bionic: the answer is inside the block that
  // was seeded for it, it is not the block's base (an implementation that drops the `add` returns the
  // base), it is word-aligned, and it is the same address on both tiers and on both reads.
  //
  // What that can no longer catch is stated so nobody mistakes the row for what it was: an offset that is
  // wrong but still lands inside the mapping passes. The actual number is printed for a human to read.
  const exec::GuestAddr tls_end = kData + kDataBytes;
  std::uint32_t seen[4] = {0, 0, 0, 0};
  int observations = 0;
  const auto once = [&](exec::Executor& e, const Outcome& got) {
    const auto addr = static_cast<exec::GuestAddr>(got.bits);
    ++observations;
    if (observations <= 4) seen[observations - 1] = addr;
    CHECK_CTX(addr > kTlsBlock && addr < tls_end,
              std::string{"__errno on Tier "} + Tier(e) + " = 0x" + Hex64(addr) +
                  ", outside the seeded TLS block (0x" + Hex(kTlsBlock) + "..0x" + Hex(tls_end) +
                  ") — either the tp chain broke or this build's offset runs past the mapping");
    CHECK_CTX((addr & 3u) == 0u,
              std::string{"__errno on Tier "} + Tier(e) + " = 0x" + Hex64(addr) +
                  ", not word-aligned — `errno` is an `int`");
    std::fprintf(stderr, "[libc] %-22s Tier %s: tp=0x%s -> 0x%s (offset +0x%s)  %s\n", "__errno",
                 Tier(e), Hex(kTp).c_str(), Hex(addr).c_str(), Hex(addr - kTlsBlock).c_str(),
                 got.detail.c_str());
  };
  both_tiers(rig, "__errno", "ILL", {}, once);
  // A second read, to show the register is not consumed by being read — ARM gives TPIDRURW no read-to-clear
  // behaviour, and an implementation that treated it as a handshake would pass the row above and fail this
  // one. `mcr p15, #0, rX, c13, c0, #3` is modelled for symmetry with the read (`__ARM_NR_set_tls` writes
  // that register in the kernel, and a guest that sets its own TLS must not find a hole), but `[UNVERIFIED]`
  // and deliberately unasserted: `llvm-objdump` over the whole of libc.so counts **zero** `mcr`
  // instructions in it, so there is no corpus evidence yet about what a real one expects.
  both_tiers(rig, "__errno", "ILL", {}, once);
  // Both tiers, both reads: four addresses that have to be one address. This is where the cross-tier claim
  // lives, since neither `both_tiers` call compares the two on its own.
  CHECK_CTX(observations == 4 && seen[0] == seen[1] && seen[0] == seen[2] && seen[0] == seen[3],
            "the four __errno observations differ: I=0x" + Hex(seen[0]) + " J=0x" + Hex(seen[1]) +
                " I=0x" + Hex(seen[2]) + " J=0x" + Hex(seen[3]));
}

/**
 * The functions most likely to need what RetroBug does not have, tried anyway and **reported**.
 *
 * No assertion here on purpose: these rows exist to produce a measurement, and turning an unknown into a
 * `CHECK` would make the lane red for a reason the lane does not yet understand. `snprintf` reaches
 * stdio's `FILE` machinery and its lock; `malloc` is what a 20-byte `DT_INIT_ARRAY` that C05 records but
 * does not run would have set up. The `reason`, `steps` and the engine's own detail string are printed
 * for both tiers, and whatever the outcome, it goes into `docs/30` as measured rather than predicted.
 */
void what_libc_needs_that_retrobug_does_not_have() {
  Rig rig = build();
  if (!rig.ok) return;
  using abi::Value32;
  struct Probe {
    const char* name;
    const char* shorty;
    abi::Args32 args;
  };
  const Probe probes[] = {
      // The IFUNC path, observed: bionic's `strstr` calls `strlen` when it runs out of string, the GOT slot
      // for `strlen` holds a resolver because nothing called it, and the resolver's first instruction is a
      // syscall. What the log shows is therefore *not* a wrong answer but an `SVC`, which is the whole
      // argument that IFUNC is a load-time feature and not a call-time one.
      {"strstr", "ILLI", {Value32::Ptr(kHay), Value32::Ptr(kAbsent)}},
      {"strlen", "ILL", {Value32::Ptr(kHay)}},
      {"memcpy", "LLLL", {Value32::Ptr(kData + 0x700), Value32::Ptr(kHay), Value32::U32(8)}},
      {"snprintf", "ILIPILC",
       {Value32::Ptr(kSnprintfBuf), Value32::U32(64), Value32::Ptr(kFmt), Value32::S32(42),
        Value32::Ptr(kXy), Value32::S32('!')}},
      {"malloc", "IL", {Value32::U32(64)}},
      {"getpid", "II", {}},
  };
  for (const Probe& p : probes) {
    for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                         static_cast<exec::Executor*>(rig.jit.get())}) {
      const Outcome got = call(*engine, rig, p.name, p.args, p.shorty);
      std::fprintf(stderr, "[libc] probe %-9s Tier %s: reason=%d steps=%u r0=0x%s  %s\n", p.name,
                   Tier(*engine), static_cast<int>(got.reason), got.steps,
                   Hex(static_cast<std::uint32_t>(got.bits)).c_str(), got.detail.c_str());
    }
  }
  char buf[80] = {};
  rig.mem->Read(kSnprintfBuf, buf, sizeof buf - 1);
  // Hardware's answer, from `oracle_libc` on the same device: `7` and `"42/xy/!"`. Printed next to
  // whatever RetroBug left in the buffer so the gap, if any, is visible rather than asserted away.
  std::fprintf(stderr, "[libc] snprintf buffer holds \"%s\" (hardware wrote \"42/xy/!\")\n", buf);
}

/**
 * The same image loaded under the default, and the load has to fail.
 *
 * This row exists because the alternative is the failure mode the owner named explicitly: a module that
 * "loads cleanly" while three of its relocation slots still contain file data. A TLS offset is not a
 * null pointer someone might notice — bionic computes it into a real address and the guest dereferences it
 * as if the load had finished, so the first symptom is a wrong value somewhere far from here.
 *
 * Asserting the *text* of the failure is deliberate too. "Refused 3 relocations" would pass on a loader
 * that refused the wrong three, and the whole value of the refusal is that it says which.
 */
void a_load_without_that_flag_refuses_and_names_the_slots() {
  const auto image = read_file(RB_DEVICE_LIBC_PATH);
  if (image.empty()) {
    ++checks;
    fail("read the device's libc.so for the strict-load row", __LINE__, RB_DEVICE_LIBC_PATH);
    return;
  }
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  std::string merr;
  auto mem = GuestMemory::Create(64u << 20, model, &merr);
  if (!mem) {
    ++checks;
    fail("guest memory for the strict-load row", __LINE__, merr);
    return;
  }
  loader::LoadOptions strict;  // default: tolerate_unresolved_relocs == false
  strict.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return kUnimplemented; };
  std::string err;
  const auto mod = loader::LoadFromBytes(*mem, image, "libc.so", strict, &err);
  CHECK_CTX(!mod.has_value(),
            "the device's libc.so loaded under the default options, with " +
                std::to_string(mod ? mod->unsupported_relocs.size() : 0u) +
                " unapplied relocations. It should refuse");
  if (mod) return;
  std::fprintf(stderr, "[libc] strict load refused: %s\n", err.c_str());
  // Every slot the loader refused has to appear in the refusal with its address — that is the rule this
  // path exists to satisfy, since a refusal that names nothing is only a slightly quieter corruption.
  // The count is derived from the image rather than written out: the Tab S9's bionic has three TLS
  // relocations and the Reno 5's has one, so literal addresses from either image would be a claim about a
  // phone rather than about the loader, and that mistake has already cost this project a red row.
  loader::LoadOptions tolerant;
  tolerant.tolerate_unresolved_relocs = true;
  tolerant.resolve = strict.resolve;
  std::string terr;
  const auto allowed = loader::LoadFromBytes(*mem, image, "libc.so", tolerant, &terr);
  ++checks;
  if (!allowed) fail("the tolerant load refused as well", __LINE__, terr);
  const std::size_t refused = allowed ? allowed->unsupported_reloc_types.size() : 0;
  CHECK_CTX(refused > 0,
            "the tolerant load left nothing unapplied, so this row cannot tell a named refusal from a "
            "clean load and would pass vacuously");
  std::size_t named = 0;
  for (auto at = err.find("at guest 0x"); at != std::string::npos; at = err.find("at guest 0x", at + 1)) {
    ++named;
  }
  CHECK_CTX(named == refused, "only " + std::to_string(named) + " of the " +
                                   std::to_string(refused) +
                                   " refused slot(s) are named in the refusal: " + err);
  // And the IRELATIVE must *not* be in there: it is handled, recorded, and left for the linker.
  CHECK_CTX(err.find("0xa0") == std::string::npos,
            "R_ARM_IRELATIVE was refused as if it were unknown, rather than recorded as needing a call: " +
                err);
}

/**
 * The IFUNC slots the load left for the linker, and what calling one of them costs.
 *
 * `ifunc_slots` is C05's new record of "this word holds a resolver; the value the guest wants is what it
 * returns". Two things were measured about the slots rather than assumed, and both are asserted: the count
 * (one symbolless `R_ARM_IRELATIVE` plus however many GOT entries name an IFUNC symbol — printing it is the
 * point, since the number is a property of the image and nobody has guessed it correctly so far), and
 * whether the slot is still *writable* after load, which is what decides where IFUNC resolution has to
 * happen. `DT_FLAGS`-driven RELRO strips write from `.data.rel.ro` at the end of a load, and an IFUNC GOT
 * entry lives there — so a linker that resolves after `LoadFromBytes` returns may be trying to write into
 * read-only guest memory, and RetroBug's memory model is allowed to say no.
 */
void the_ifunc_slots_are_recorded_for_the_linker() {
  Rig rig = build();
  if (!rig.ok) return;
  std::fprintf(stderr, "[libc] %zu IFUNC slot(s) left to resolve (module base 0x%s):\n",
               rig.libc->ifunc_slots.size(), Hex(rig.libc->base).c_str());
  std::size_t in_text = 0;
  for (const auto& s : rig.libc->ifunc_slots) {
    const bool inside = s.resolver.addr >= rig.libc->base &&
                        s.resolver.addr < rig.libc->base + rig.libc->extent;
    in_text += inside ? 1 : 0;
    std::fprintf(stderr, "  slot 0x%s <- resolver 0x%s%s  %s\n", Hex(s.where).c_str(),
                 Hex(s.resolver.tagged()).c_str(), s.resolver.thumb ? " (Thumb)" : "",
                 s.name.empty() ? "<R_ARM_IRELATIVE, no name>" : s.name.c_str());
  }
  CHECK_CTX(!rig.libc->ifunc_slots.empty(), "no IFUNC slot was recorded at all");
  CHECK_CTX(in_text == rig.libc->ifunc_slots.size(),
            "a recorded resolver points outside the module — the slot value was not the biased address");

  // Try resolving one, on Tier J, through the same bridge every other row uses.
  //
  // **None of the slot writes succeed, and that is the finding.** Every IFUNC GOT entry in this image sits
  // in memory that `enforce_relro` has already made read-only by the time `LoadFromBytes` returns, so a
  // linker that resolves after the load cannot write the answer back. Which decides #30's shape: the call
  // has to happen *inside* the load, between relocation and the RELRO protection pass — exactly where
  // bionic's own linker does it (`relocate()` runs before `protect_relro()` **[1A]**). It is asserted here
  // rather than reasoned about, because "the write is refused" is a fact about the guest memory model and
  // not about a document, and a future change that let these writes through would silently move the
  // resolution point back to where it does not belong.
  //
  // The two resolver families are worth reading off the same table: `strcmp`, `memcpy` and `memmove` return
  // an implementation pointer in four steps with no syscall at all (and the low bit of `0x956c1` / `0x94ac0`
  // says those implementations are Thumb), while `strlen`, `memset` and the `_chk` variants stop at an SVC
  // after 16 steps — the `/dev/cpu_variant:arm` probe. A monitor is attached now, and with it all eight
  // resolve on the strength of **one** `openat` (the tally says `openat x1`, because bionic remembers the
  // answer), so the five were never a loader gap: they were a syscall-surface one.
  // resolving all eleven is a strict superset of "call a guest function": it also needs the syscall path.
  std::size_t refused = 0;
  for (const auto& s : rig.libc->ifunc_slots) {
    const std::uint32_t probe = 0x5a5a'5a5au;
    if (!rig.mem->Write(s.where, &probe, 4)) ++refused;
  }
  CHECK_CTX(refused == rig.libc->ifunc_slots.size(),
            std::to_string(rig.libc->ifunc_slots.size() - refused) +
                " IFUNC slot(s) were writable after the load — resolution at that point is impossible on a "
                "real image, which is what puts the call inside LoadFromBytes. If this became writable "
                "because RELRO stopped being enforced, that is a regression elsewhere, not permission to "
                "resolve here.");
}

/**
 * The resolvers that ran, and what their implementations actually compute.
 *
 * Three of the eleven IFUNC resolvers answer with no syscall at all — `strcmp`, `memcpy`, `memmove`
 * returned an implementation pointer in four steps, the set low bit marking it Thumb — and five stop at the
 * `openat("/dev/cpu_variant:arm")` probe, which this build has no answer for. The latter is a
 * guest-filesystem question (C10's VFS), not a loader one, so the unresolved list is printed as the size
 * of the next piece of work rather than worked around.
 *
 * What is asserted is only what each API licenses. `strcmp`'s contract is the **sign**: the devices return
 * a word-derived difference (`-32` for `"abc"`/`"abd"`, on *both* SoCs), and RetroBug may select a
 * different IFUNC body than a phone does, so comparing magnitudes there would eventually be reported as an
 * engine bug. `memcpy` and `memmove` are compared byte for byte because that is what their standard pins —
 * including the overlapping case, the one behaviour that distinguishes `memmove` from `memcpy`.
 *
 * **What `.iplt` turned out to be, and why the detour was the point.** The non-overlapping `memcpy`/`memmove`
 * reached `SVC #0x0` on *both* tiers, and the reason was inside the resolved body: `0x94ac0` is
 * `memmove_a15` (bionic uses the memmove body for `memcpy` too), its forward-copy path ends in `b 0xd2b00`,
 * and `llvm-objdump` puts that address in **`.iplt`** — bionic's IFUNC PLT, whose stub is
 * `add r12, pc, …; ldr pc, [r12, #0xed8]!`. Decoding the stub's own arithmetic lands its target on
 * `0xd69e0`: **the same `R_ARM_IRELATIVE` slot the loader already records.** So `.iplt` was never a second
 * piece of work — it is one unfilled slot seen from the other end, and it fills itself as soon as the
 * load-time write goes to the right address. Which it did not: `IfuncSlot::where` held a `vaddr`, so the
 * write landed 0x2000 below its target and still reported success. Two lessons, kept visible on purpose:
 *
 *   - **A name-based lookup hides a slot-based bug.** `FindExport` was returning the right implementation
 *     while the memory bionic actually branches through still held the resolver, so the rows that read like
 *     proof of resolution were not looking at the thing that was resolved. The slot read-back below closes
 *     that, and it is the only row in this file that can fail for the bug that was here.
 *   - **Two tiers agreeing is not evidence of correctness.** They agreed on the `SVC` because both were
 *     faithfully running the same bad pointer — §Step 24's wrong-test-constant lesson from the other side.
 *
 * The overlapping `memmove` passed throughout because its backward path never touches the IPLT, which is
 * also why "some of it works" was not evidence that any of it was finished.
 */
void resolved_ifunc_implementations_compute_what_hardware_does() {
  Rig rig = build();
  if (!rig.ok) return;
  using abi::Value32;
  for (const auto& line : rig.ifunc_log) std::fprintf(stderr, "[libc] %s\n", line.c_str());
  std::fprintf(stderr, "[libc] %zu of %zu IFUNC slot(s) resolved; still needing an answer: %s\n",
               rig.ifunc_resolved, rig.libc->ifunc_slots.size(), Join(rig.ifunc_unresolved).c_str());
  std::fprintf(stderr, "[libc] syscalls the resolvers asked for: %s\n",
               rig.monitor ? rig.monitor->TallyText().c_str() : "(no monitor)");
  CHECK_CTX(rig.ifunc_resolved >= 3u,
            "fewer than the three no-syscall resolvers resolved: " + std::to_string(rig.ifunc_resolved));
  CHECK_CTX(rig.ifunc_unresolved.size() + rig.ifunc_resolved == rig.libc->ifunc_slots.size(),
            "slots went missing: resolved + unresolved does not equal the recorded count");
  for (const auto& s : rig.libc->ifunc_slots) {
    if (s.implementation == 0) continue;
    const std::uint32_t impl = s.implementation & ~1u;
    CHECK_CTX(impl >= rig.libc->base && impl < rig.libc->base + rig.libc->extent,
              (s.name.empty() ? std::string{"<IRELATIVE>"} : s.name) +
                  "'s resolved implementation is outside the module: 0x" + Hex(s.implementation));
  }

  /**
   * And the trap this whole row nearly fell into, now asserted instead of rediscovered: for a resolved
   * IFUNC, `FindExport` must return the *implementation*, because `st_value` is the resolver and calling
   * it returns a code pointer that looks exactly like an answer. That is what `strcmp` first reported —
   * `0x95601`, inside libc's own text, with `reason=Returned` and no sign of trouble.
   */
  for (const auto& s : rig.libc->ifunc_slots) {
    if (s.implementation == 0 || s.name.empty()) continue;
    const auto fn = rig.libc->FindExport(s.name);
    CHECK_CTX(fn.has_value() && fn->tagged() == s.implementation,
              s.name + ": FindExport still returns the resolver (0x" +
                  Hex(fn ? fn->tagged() : 0u) + ") instead of the implementation (0x" +
                  Hex(s.implementation) + ")");
  }

  /**
   * Read the slot back, which is the assertion that catches the bug this row was written for.
   *
   * Checking `FindExport` alone is not enough: it answers by name, so it stays right while the *slot* still
   * holds the resolver — and it is the slot that bionic's own `.iplt` stubs branch through, which is how a
   * resolved-but-unwritten `memcpy` was still copying nothing. First draft stored a `vaddr` in
   * `IfuncSlot::where` and wrote 0x2000 below the target with a cheerful success.
   */
  for (const auto& s : rig.libc->ifunc_slots) {
    if (s.implementation == 0) continue;
    std::uint32_t held = 0;
    rig.mem->Read(s.where, &held, 4);
    CHECK_CTX(held == s.implementation,
              (s.name.empty() ? std::string{"<IRELATIVE>"} : s.name) +
                  " slot at 0x" + Hex(s.where) + " holds 0x" + Hex(held) + ", not the implementation 0x" +
                  Hex(s.implementation));
  }

  const auto call_libc = [&](const char* name, const abi::Args32& args, const char* shorty,
                              exec::Executor& e) {
    Outcome o;
    const auto fn = rig.libc->FindExport(name);
    if (!fn) {
      o.detail = std::string{"libc.so does not export "} + name;
      return o;
    }
    rig.abi_ctx->set_executor(e);
    exec::RunResult out{};
    exec::MachineState exit_state{};
    const abi::Ret32 r = rig.abi_ctx->CallGuest(fn->tagged(), args, abi::Shorty::For(shorty),
                                                4'000'000, &out, &exit_state);
    o.reason = out.reason;
    o.bits = r.bits;
    o.detail = out.detail;
    o.steps = out.instructions_executed;
    return o;
  };

  constexpr exec::GuestAddr kAbc2 = kData + 0x900;
  constexpr exec::GuestAddr kAbd2 = kData + 0x940;
  constexpr exec::GuestAddr kCopySrc = kData + 0x980;
  constexpr exec::GuestAddr kCopyDst = kData + 0x9c0;
  constexpr exec::GuestAddr kOverlap = kData + 0xa00;
  write_cstr(*rig.mem, kAbc2, "abc");
  write_cstr(*rig.mem, kAbd2, "abd");
  write_cstr(*rig.mem, kCopySrc, "0123456789abcdef0123456789abcdef");

  for (auto* engine : {static_cast<exec::Executor*>(rig.jit.get()),
                       static_cast<exec::Executor*>(rig.interp.get())}) {
    const Outcome cmp =
        call_libc("strcmp", {Value32::Ptr(kAbc2), Value32::Ptr(kAbd2)}, "ILL", *engine);
    if (!cmp.ok()) {
      ++checks;
      fail("strcmp", __LINE__, std::string{"Tier "} + Tier(*engine) + ": " + cmp.detail);
    } else {
      const std::int32_t v = static_cast<std::int32_t>(cmp.bits);
      CHECK_CTX(v < 0, "strcmp(\"abc\",\"abd\") on Tier " + std::string(Tier(*engine)) + " = " +
                           std::to_string(v) + ", hardware said negative (both devices gave -32)");
      std::fprintf(stderr, "[libc] %-22s hardware=-32(sign-) Tier %s=%d  steps=%u\n", "strcmp",
                   Tier(*engine), v, cmp.steps);
    }

    for (const char* fn : {"memcpy", "memmove"}) {
      write_cstr(*rig.mem, kCopyDst, "XXXXXXXXX");
      const Outcome got =
          call_libc(fn, {Value32::Ptr(kCopyDst), Value32::Ptr(kCopySrc), Value32::U32(9)}, "LLLL",
                    *engine);
      if (!got.ok()) {
        ++checks;
        fail(fn, __LINE__, std::string{"Tier "} + Tier(*engine) + ": " + got.detail);
        continue;
      }
      char out[16] = {};
      rig.mem->Read(kCopyDst, out, sizeof out - 1);
      CHECK_CTX(std::strcmp(out, "012345678") == 0,
                std::string{fn} + " on Tier " + Tier(*engine) + " produced \"" + out +
                    "\", hardware produced \"012345678\"");
      std::fprintf(stderr, "[libc] %-22s Tier %s -> \"%s\"\n", fn, Tier(*engine), out);
    }

    // The overlapping move, which is the one thing `memmove` promises that `memcpy` does not: the source
    // and destination share four bytes, and a forward copy corrupts the tail.
    write_cstr(*rig.mem, kOverlap, "ABCDEFGH");
    const Outcome moved =
        call_libc("memmove", {Value32::Ptr(kOverlap + 2), Value32::Ptr(kOverlap), Value32::U32(8)},
                  "LLLL", *engine);
    char out[16] = {};
    if (moved.ok()) rig.mem->Read(kOverlap, out, sizeof out - 1);
    CHECK_CTX(moved.ok() && std::string{out} == "ABABCDEFGH",
              std::string{"memmove overlap on Tier "} + Tier(*engine) + " gave \"" + out +
                  "\" (ok=" + (moved.ok() ? "yes" : "no") + "), hardware gave \"ABABCDEFGH\"");
    std::fprintf(stderr, "[libc] %-22s Tier %s -> \"%s\"\n", "memmove overlap", Tier(*engine), out);
  }
}

/**
 * An importer of libc's `strcpy` and `strcat`, linked by `GuestLinker`, gets the copy made.
 *
 * Those two are IFUNC exports libc never calls through its own GOT, so loading libc resolves no slot for
 * them and `FindExport` answers with the *resolver*. Bound to that, `rb_strcpy(dst, src)` runs the
 * resolver with `dst` in r0, gets an implementation pointer back as its "result", and writes nothing —
 * Gumball 2013's texture names stayed untouched heap exactly this way (2026-10-14). So the row reads the
 * destination back, on both tiers, rather than trusting the call to have returned.
 *
 * libc's own `DT_NEEDED` is `ld-android.so` and `libdl.so`. They are read off the device this lane runs
 * on — the same 32-bit system that shipped the libc — rather than stubbed, so the chain is bionic's own.
 */
void an_importer_of_strcpy_and_strcat_gets_the_work_done() {
  Rig rig = build();
  if (!rig.ok) return;
  using abi::Value32;
  const auto user = read_file(RB_IFUNC_LIBC_USER_PATH);
  if (user.empty()) {
    ++checks;
    fail("read the strcpy/strcat importer", __LINE__,
         std::string{"nothing at "} + RB_IFUNC_LIBC_USER_PATH + " — pushed from tools/run_engine_tests.sh?");
    return;
  }
  const auto libc_image = read_file(RB_DEVICE_LIBC_PATH);
  const auto libdl = read_file("/system/lib/libdl.so");
  const auto ld_android = read_file("/system/lib/ld-android.so");
  // By value: the linker keeps the provider for as long as it lives.
  const linker::ImageProvider provider =
      [libc_image, libdl, ld_android, user](
          const std::string& soname) -> std::optional<std::vector<std::uint8_t>> {
    const std::vector<std::uint8_t>* img = soname == "libc.so"             ? &libc_image
                                           : soname == "libdl.so"          ? &libdl
                                           : soname == "ld-android.so"     ? &ld_android
                                           : soname == "librbifunclibc.so" ? &user
                                                                           : nullptr;
    if (img == nullptr || img->empty()) return std::nullopt;
    return *img;
  };
  std::vector<std::string> host_asked;
  const loader::SymbolResolver host = [&](const std::string& name) -> std::optional<std::uint32_t> {
    host_asked.push_back(name);
    return kUnimplemented;
  };
  loader::LoadOptions base;
  base.tolerate_unresolved_relocs = true;  // libc's TLS slots, exactly as `build()` loads it
  base.resolve_ifunc = [&](const loader::IfuncSlot& s) { return RunResolver(rig, s); };
  linker::GuestLinker scope{*rig.mem, provider, host, base};
  std::string err;
  const auto mod = scope.LinkNamed("librbifunclibc.so", &err);
  CHECK_CTX(mod.has_value(), "GuestLinker could not link the importer against the device libc: " + err);
  if (!mod) return;
  std::fprintf(stderr, "[libc] GuestLinker scope: %zu modules; host asked for: %s\n", scope.order().size(),
               Join(host_asked).c_str());
  for (const linker::IfuncExport& e : scope.ifunc_exports()) {
    std::fprintf(stderr, "[libc] cross-module IFUNC %s of %s: resolver 0x%s -> %s\n", e.name.c_str(),
                 e.module.c_str(), Hex(e.resolver.tagged()).c_str(),
                 e.implementation ? ("0x" + Hex(e.implementation)).c_str() : e.refusal.c_str());
  }

  const auto libc = scope.Find("libc.so");
  CHECK_CTX(libc.has_value(), "libc.so is not in the linker's scope");
  if (!libc) return;
  for (const char* name : {"strcpy", "strcat"}) {
    const auto resolver = linker::UnresolvedIfuncExport(**libc, name);
    CHECK_CTX(resolver.has_value(),
              std::string{name} + ": libc resolved a slot for it during its own load, so FindExport no "
                                  "longer hands out the resolver and this row has lost its premise");
    const auto bound = scope.Lookup(name);
    CHECK_CTX(bound.has_value() && resolver && *bound != resolver->tagged(),
              std::string{name} + ": the linker binds 0x" + Hex(bound.value_or(0)) +
                  ", which is libc's resolver — every call would return a code pointer");
  }

  const auto fn_cpy = (*mod)->FindExport("rb_strcpy");
  const auto fn_cat = (*mod)->FindExport("rb_strcat");
  CHECK_CTX(fn_cpy && fn_cat, "the importer does not export rb_strcpy and rb_strcat");
  if (!fn_cpy || !fn_cat) return;
  constexpr exec::GuestAddr kSrc = kData + 0xc00;
  constexpr exec::GuestAddr kDst = kData + 0xc40;
  write_cstr(*rig.mem, kSrc, "texture_0001.png");
  const auto run = [&](exec::Executor& e, loader::CodeAddress fn, Outcome* o) {
    rig.abi_ctx->set_executor(e);
    exec::RunResult result{};
    exec::MachineState exit_state{};
    const abi::Ret32 r = rig.abi_ctx->CallGuest(fn.tagged(), {Value32::Ptr(kDst), Value32::Ptr(kSrc)},
                                                abi::Shorty::For("LLL"), 4'000'000, &result, &exit_state);
    o->reason = result.reason;
    o->bits = r.bits;
    o->detail = result.detail;
  };
  for (auto* engine : {static_cast<exec::Executor*>(rig.interp.get()),
                       static_cast<exec::Executor*>(rig.jit.get())}) {
    // Pre-filled with a sentinel longer than the answer, so "wrote nothing" and "wrote too little" both
    // read back as something other than the source.
    write_cstr(*rig.mem, kDst, "XXXXXXXXXXXXXXXXXXXXXXXX");
    Outcome cpy;
    run(*engine, *fn_cpy, &cpy);
    char out[40] = {};
    rig.mem->Read(kDst, out, sizeof out - 1);
    CHECK_CTX(cpy.ok() && std::string{out} == "texture_0001.png" &&
                  static_cast<std::uint32_t>(cpy.bits) == kDst,
              std::string{"strcpy through the importer on Tier "} + Tier(*engine) + " left \"" + out +
                  "\" and returned 0x" + Hex(static_cast<std::uint32_t>(cpy.bits)) + " (ok=" +
                  (cpy.ok() ? "yes" : "no " + cpy.detail) + "); want \"texture_0001.png\" and 0x" +
                  Hex(kDst));
    std::fprintf(stderr, "[libc] %-22s Tier %s -> \"%s\"\n", "strcpy via importer", Tier(*engine), out);

    write_cstr(*rig.mem, kDst, "atlas/");
    Outcome cat;
    run(*engine, *fn_cat, &cat);
    std::memset(out, 0, sizeof out);
    rig.mem->Read(kDst, out, sizeof out - 1);
    CHECK_CTX(cat.ok() && std::string{out} == "atlas/texture_0001.png" &&
                  static_cast<std::uint32_t>(cat.bits) == kDst,
              std::string{"strcat through the importer on Tier "} + Tier(*engine) + " left \"" + out +
                  "\" and returned 0x" + Hex(static_cast<std::uint32_t>(cat.bits)) + " (ok=" +
                  (cat.ok() ? "yes" : "no " + cat.detail) + "); want \"atlas/texture_0001.png\" and 0x" +
                  Hex(kDst));
    std::fprintf(stderr, "[libc] %-22s Tier %s -> \"%s\"\n", "strcat via importer", Tier(*engine), out);
  }
}

}  // namespace

int main() {
  the_load_reports_exactly_what_retrobug_cannot_yet_do();
  a_load_without_that_flag_refuses_and_names_the_slots();
  the_ifunc_slots_are_recorded_for_the_linker();
  resolved_ifunc_implementations_compute_what_hardware_does();
  an_importer_of_strcpy_and_strcat_gets_the_work_done();
  string_scanning_matches_bionic();
  numeric_conversion_matches_bionic();
  libc_calls_back_into_a_guest_function();
  the_thread_pointer_reaches_a_guest_tls_block();
  what_libc_needs_that_retrobug_does_not_have();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
