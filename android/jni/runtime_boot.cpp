// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_runtime_boot`: the in-process boot. A repackaged game's `System.loadLibrary("X")` loads a 64-bit
// stub named libX.so (`stub.cpp`), whose `JNI_OnLoad` lands here with the game's own APK path. The 32-bit
// libraries and the guest system libraries travel inside that APK as assets (the repackager's layout:
// `assets/rb32/<libX.so>`, `assets/rb32sys/{libc,libm,libz,libstdc++}.so`).
//
// Boot-only (M2): one process-lifetime `GuestProcess` (4 GiB biased window, unresolved imports bound to the
// trap 0xfffe0000) whose scope — libc, libm, libz — is booted on the first call and reused by every later
// one; each call loads its library and any `assets/rb32/` sibling its DT_NEEDED names (libstdc++ from
// `assets/rb32sys/` on demand), runs the constructors and the guest `JNI_OnLoad`, on Tier J, synchronously.
// No per-game profile runs here: driving a game's entry points is the game's own Java's job (M3).
// The host answers the dynamic-linker and liblog imports (`host_slots.h`); GL imports stay trapped by name
// until M4.

#include "runtime_boot.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "art_java.h"
#include "host_slots.h"
#include "rb/abi/jni_forward.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/inbound.h"
#include "rb/runtime/guest_process.h"

namespace {

using rb::runtime::GuestProcess;

constexpr const char* kTag = "RBRUNTIME";
constexpr std::size_t kWindowBytes = std::size_t{4} << 30;
constexpr rb::exec::GuestAddr kTrap = 0xfffe'0000u;
constexpr int kTier = 2;  // Tier J

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

/// Guards the report being collected and `Runtime::report`: guest threads log into it from their own host
/// threads (holding the GGL, not g_mu) while the boot thread appends and finally detaches it. A leaf lock: held
/// only for an append or a pointer swap, never while taking anything else.
std::mutex g_report_mu;

/// A line of the report: logged under RBRUNTIME and, when a boot is collecting one, kept for the file.
void Line(std::string* out, const std::string& s) {
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", s.c_str());
  if (out != nullptr) {
    std::lock_guard<std::mutex> lk(g_report_mu);
    *out += s;
    *out += '\n';
  }
}

/**
 * The APK as a zip read through its fd: the central directory once (only the `assets/rb32*` entries are
 * kept), then one entry at a time — a stored one by `pread`, a deflated one inflated in 64 KiB chunks —
 * so no whole-APK buffer exists (M1's read of the whole file was a `bad_alloc` waiting for a 1 GB APK).
 * ZIP64, an unknown method, a damaged header and a CRC mismatch are each refused by name.
 */
class ApkZip {
 public:
  ~ApkZip() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool Open(const std::string& path, std::string* why) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return Fail(why, "open(" + path + "): " + std::strerror(errno));
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return Fail(why, "fstat(" + path + "): " + std::strerror(errno));
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    if (size < 22) return Fail(why, path + " is shorter than an end-of-central-directory record");
    const std::uint64_t tail_len = std::min<std::uint64_t>(size, 22 + 0xffff);
    std::vector<std::uint8_t> tail(tail_len);
    if (!ReadAt(size - tail_len, tail.data(), tail.size())) return Fail(why, "reading the zip's tail: " + err_);
    std::size_t eocd = tail.size() - 22;
    while (U32(tail, eocd) != 0x06054b50u) {
      if (eocd == 0) return Fail(why, path + " has no end-of-central-directory record");
      --eocd;
    }
    const std::uint32_t entries = U16(tail, eocd + 10), cd_size = U32(tail, eocd + 12), cd_at = U32(tail, eocd + 16);
    if (entries == 0xffffu || cd_at == 0xffff'ffffu || cd_size == 0xffff'ffffu) {
      return Fail(why, path + " is a ZIP64 archive, which this reader does not read");
    }
    std::vector<std::uint8_t> cd(cd_size);
    if (!ReadAt(cd_at, cd.data(), cd.size())) return Fail(why, "reading the central directory: " + err_);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < entries; ++i) {
      if (at + 46 > cd.size() || U32(cd, at) != 0x02014b50u) {
        return Fail(why, "central directory entry " + std::to_string(i) + " is damaged");
      }
      const std::uint32_t nlen = U16(cd, at + 28), xlen = U16(cd, at + 30), clen = U16(cd, at + 32);
      if (at + 46 + nlen > cd.size()) return Fail(why, "a central directory name runs off the directory");
      std::string name(reinterpret_cast<const char*>(cd.data() + at + 46), nlen);
      if (name.compare(0, 11, "assets/rb32") == 0) {
        entries_[name] = Entry{U16(cd, at + 10), U32(cd, at + 16), U32(cd, at + 20), U32(cd, at + 24), U32(cd, at + 42)};
      }
      at += 46 + nlen + xlen + clen;
    }
    return true;
  }

  bool Has(const std::string& name) const { return entries_.count(name) != 0; }

  std::optional<std::vector<std::uint8_t>> Read(const std::string& name, std::string* why) {
    const auto none = [&](const std::string& w) -> std::optional<std::vector<std::uint8_t>> {
      if (why != nullptr) *why = w;
      return std::nullopt;
    };
    const auto it = entries_.find(name);
    if (it == entries_.end()) return none("no entry named " + name);
    const Entry& e = it->second;
    std::vector<std::uint8_t> lh(30);
    if (!ReadAt(e.local, lh.data(), lh.size()) || U32(lh, 0) != 0x04034b50u) {
      return none(name + ": local header is damaged");
    }
    const std::uint64_t data = std::uint64_t{e.local} + 30 + U16(lh, 26) + U16(lh, 28);
    std::vector<std::uint8_t> out(e.usize);
    if (e.method == 0) {
      if (e.csize != e.usize) return none(name + ": stored, but the sizes disagree");
      if (!ReadAt(data, out.data(), out.size())) return none(name + ": " + err_);
    } else if (e.method == 8) {
      z_stream z{};
      if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return none(name + ": inflateInit2 failed");
      std::vector<std::uint8_t> chunk(1 << 16);
      std::uint64_t left = e.csize, pos = data;
      int rc = Z_OK;
      z.next_out = out.data();
      z.avail_out = static_cast<uInt>(out.size());
      while (rc == Z_OK && left > 0) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, chunk.size()));
        if (!ReadAt(pos, chunk.data(), n)) {
          inflateEnd(&z);
          return none(name + ": " + err_);
        }
        pos += n;
        left -= n;
        z.next_in = chunk.data();
        z.avail_in = static_cast<uInt>(n);
        rc = inflate(&z, left == 0 ? Z_FINISH : Z_NO_FLUSH);
        if (rc == Z_BUF_ERROR && z.avail_out == 0) break;  // more input than the directory's size says
      }
      const std::uint64_t produced = z.total_out;
      inflateEnd(&z);
      if (rc != Z_STREAM_END || produced != e.usize) {
        return none(name + ": inflate ended with " + std::to_string(rc) + " after " + std::to_string(produced) +
                    " of " + std::to_string(e.usize) + " bytes");
      }
    } else {
      return none(name + ": compression method " + std::to_string(e.method) + " is not stored or deflate");
    }
    const std::uint32_t crc = static_cast<std::uint32_t>(crc32(0L, out.data(), static_cast<uInt>(out.size())));
    if (crc != e.crc) return none(name + ": CRC " + Hex(crc) + " is not the directory's " + Hex(e.crc));
    return out;
  }

 private:
  struct Entry {
    std::uint32_t method = 0, crc = 0, csize = 0, usize = 0, local = 0;
  };
  static std::uint32_t U16(const std::vector<std::uint8_t>& b, std::size_t at) { return b[at] | (b[at + 1] << 8); }
  static std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
  }
  bool Fail(std::string* why, const std::string& w) {
    if (why != nullptr) *why = w;
    return false;
  }
  bool ReadAt(std::uint64_t off, std::uint8_t* into, std::size_t n) {
    while (n > 0) {
      const ssize_t got = ::pread(fd_, into, n, static_cast<off_t>(off));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) {
        err_ = got == 0 ? "unexpected end of file at offset " + std::to_string(off) : std::strerror(errno);
        return false;
      }
      into += got;
      off += static_cast<std::uint64_t>(got);
      n -= static_cast<std::size_t>(got);
    }
    return true;
  }

  int fd_ = -1;
  std::string err_;
  std::map<std::string, Entry> entries_;
};

/// This process's package, from `/proc/self/cmdline` (the first NUL-terminated string; a `:service`
/// process suffix is dropped). Empty when it cannot be read.
std::string PackageName() {
  FILE* f = std::fopen("/proc/self/cmdline", "rb");
  if (f == nullptr) return {};
  char buf[256] = {};
  const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  std::string name(buf, strnlen(buf, n));
  const auto colon = name.find(':');
  if (colon != std::string::npos) name.resize(colon);
  return name;
}

/// What a device's linker would answer for this app: the host's own answer, since the stub runs in it.
int TargetSdk() {
  using Fn = int (*)();
  if (auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "android_get_application_target_sdk_version"))) return fn();
  return 24;  // the repackager's floor (HANDOFF M1 deviation 3)
}

/// The process-lifetime runtime. Never destroyed: the guest process lives as long as the app does, and
/// tearing it down from a static destructor while an ART thread might still call in would be worse.
struct Runtime {
  std::unique_ptr<GuestProcess> proc;
  std::unique_ptr<rb::launcher::HostSlots> slots;
  // M3: the JNI bridge — real Java (the VM the first stub handed in), the guest JNIEnv/JavaVM over it, and the
  // dispatcher Java calls guest natives through.
  JavaVM* vm = nullptr;
  std::unique_ptr<rb::launcher::ArtJava> java;
  std::unique_ptr<rb::abi::ForwardingJni> fwd;
  std::unique_ptr<rb::runtime::Inbound> inbound;
  std::map<std::string, const rb::loader::GuestModule*> loaded;  ///< soname -> module (apps, libstdc++)
  std::set<std::string> booted;                                  ///< libraries whose boot succeeded
  std::string* report = nullptr;                                 ///< the boot in progress's report
  /// Set when a boot failed after guest state changed (a module placed, guest code run): the process is
  /// then half-initialized, so every later call refuses by name instead of running into its leftovers
  /// (a faulted module's constructors, a sibling whose constructors never ran) under another report.
  std::string poisoned;
  bool touched = false;  ///< this boot changed guest state
  std::string failure;   ///< this boot's failure, for `poisoned`
};

Runtime& R() {
  static Runtime* rt = new Runtime;
  return *rt;
}

/// A line from a guest thread (the process's log, a host slot's liblog): the report is read under its lock, so a
/// line that races the boot's end either lands in the report or only in logcat, never in a detached string.
void Live(const std::string& s) {
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", s.c_str());
  std::lock_guard<std::mutex> lk(g_report_mu);
  if (std::string* out = R().report) {
    *out += s;
    *out += '\n';
  }
}

/// Attaches (or, with nullptr, detaches) the report guest-thread lines go to.
void SetReport(Runtime& rt, std::string* report) {
  std::lock_guard<std::mutex> lk(g_report_mu);
  rt.report = report;
}

/// One boot at a time per process. Lock order: g_mu, then the process's GGL (taken inside each guest call);
/// nothing that holds the GGL — no host slot, no guest call — takes g_mu.
std::mutex g_mu;

/// What natives and rb_runtime_process reach without g_mu (which a boot holds for its whole length — and a guest
/// JNI_OnLoad that makes Java call one of an earlier library's natives would otherwise deadlock on it). A leaf lock:
/// held only to read or publish, never across a guest call.
std::mutex g_published_mu;
GuestProcess* g_published_proc = nullptr;
std::map<std::string, const rb::loader::GuestModule*> g_published_libs;  ///< booted libraries by file name

/// The JNI bridge over the scope just booted: ArtJava on the VM the stub handed in, the forwarding JNIEnv/JavaVM
/// in one guest page, its copy arena, and the inbound dispatcher (natives on Tier J). Guest RegisterNatives binds
/// through the dispatcher's trampoline pool; a guest exit inside a native exits the process, as on a device.
bool BootBridge(Runtime& rt, std::string* out) {
  if (rt.vm == nullptr) {
    Line(out, "FATAL: rb_runtime_boot: no JavaVM was handed in, so the JNI bridge cannot reach Java");
    return false;
  }
  GuestProcess& p = *rt.proc;
  std::string err;
  rb::exec::GuestAddr page = 0;
  {
    rb::runtime::Ggl::Held hold(p.ggl());
    page = p.MapFree(rb::mem::kGuestPage, rb::mem::kRead | rb::mem::kWrite, &err);
  }
  if (page == 0) {
    Line(out, "FATAL: rb_runtime_boot: no guest page for the JNI bridge: " + err);
    return false;
  }
  rt.java = std::make_unique<rb::launcher::ArtJava>(rt.vm);
  rt.fwd = std::make_unique<rb::abi::ForwardingJni>(p.memory(), p.abi(), page, *rt.java, &err);
  if (!rt.fwd->ok()) {
    Line(out, "FATAL: rb_runtime_boot: " + err);
    return false;
  }
  rt.fwd->set_allocator([&p](std::uint32_t bytes, std::string* e) {
    rb::runtime::Ggl::Held hold(p.ggl());
    return p.MapFree(bytes, rb::mem::kRead | rb::mem::kWrite, e);
  });
  rt.inbound = std::make_unique<rb::runtime::Inbound>(p, *rt.fwd, *rt.java, kTier);
  rt.fwd->set_native_binder([](rb::exec::GuestAddr fn, const std::string& name, const std::string& sig, std::string* e) -> void* {
    const std::string shorty = rb::runtime::ShortyOf(sig);
    if (shorty.empty()) {
      if (e != nullptr) *e = "'" + sig + "' is not a JNI method signature";
      return nullptr;
    }
    return R().inbound->BindPool(rb::runtime::InboundTarget{fn, shorty, name + sig}, e);
  });
  rt.inbound->set_on_exit([](int status) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "jni: a guest native called exit(%d); the app exits, as on a device",
                        status);
    ::_exit(status);
  });
  Line(out, "jni: forwarding JNI bridge up — JNIEnv at " + Hex(rt.fwd->env()) + ", JavaVM at " + Hex(rt.fwd->vm()) +
                ", natives on Tier " + (kTier == 2 ? "J" : "I"));
  return true;
}

/// A generated stub's descriptor table (written by StubElfWriter at repackage time), in the stub's own memory.
struct RbStubEntry {
  std::uint32_t name_off;    ///< from the table's start: the exported symbol (Java_..., JNI_OnLoad)
  std::uint32_t shorty_off;  ///< from the table's start: the JNI shorty after (env, this/class)
  std::uint32_t flags;
};
struct RbStubTable {
  std::uint32_t magic;  ///< kStubMagic
  std::uint32_t version;
  std::uint32_t lib_off;  ///< from the table's start: the 32-bit library's file name
  std::uint32_t count;
  RbStubEntry entries[1];
};
constexpr std::uint32_t kStubMagic = 0x54534252u;  // "RBST"
constexpr std::uint32_t kStubOnLoad = 1u;

/// The APK a stub was installed from: `<sourceDir>/lib/arm64/libX.so` -> `<sourceDir>/base.apk`.
std::string ApkOfStub(const void* addr_in_stub, std::string* lib) {
  Dl_info info{};
  if (dladdr(addr_in_stub, &info) == 0 || info.dli_fname == nullptr) return {};
  std::string path = info.dli_fname;
  // Loaded straight from the APK (extractNativeLibs=false): "<sourceDir>/base.apk!/lib/arm64-v8a/libX.so".
  if (const auto bang = path.find("!/"); bang != std::string::npos) {
    if (lib != nullptr) *lib = path.substr(path.rfind('/') + 1);
    return path.substr(0, bang);
  }
  const auto slash = path.rfind('/');
  if (lib != nullptr) *lib = slash == std::string::npos ? path : path.substr(slash + 1);
  for (int i = 0; i < 3 && !path.empty(); ++i) path.erase(path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
  return path.empty() ? std::string{} : path + "/base.apk";
}

}  // namespace

/// Where every export of a generated stub lands (through rb_stub_capture). JNI_OnLoad boots the library; a
/// Java_* entry runs its guest function through the inbound dispatcher.
extern "C" __attribute__((visibility("default"))) std::uint64_t rb_stub_dispatch(const void* tag, std::uint32_t index,
                                                                                 rb::runtime::CapturedArgs* args) {
  const auto* t = static_cast<const RbStubTable*>(tag);
  const char* base = static_cast<const char*>(tag);
  if (t == nullptr || t->magic != kStubMagic || t->version != 1 || index >= t->count) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "stub: export %u reached rb_stub_dispatch with a bad table", index);
    return 0;
  }
  const RbStubEntry& e = t->entries[index];
  const std::string lib = base + t->lib_off;
  const std::string name = base + e.name_off;
  if ((e.flags & kStubOnLoad) != 0) {  // JNI_OnLoad(JavaVM*, void*)
    const std::string apk = ApkOfStub(tag, nullptr);
    return static_cast<std::uint32_t>(rb_runtime_boot(reinterpret_cast<JavaVM*>(args->x[0]), apk.c_str(), lib.c_str()));
  }
  // Resolve once per export; the result is kept (stubs never unload).
  static std::mutex cache_mu;
  static std::map<std::pair<const void*, std::uint32_t>, rb::runtime::InboundTarget> cache;
  rb::runtime::InboundTarget target;
  bool have = false;
  {
    std::lock_guard<std::mutex> lk(cache_mu);
    const auto it = cache.find({tag, index});
    if (it != cache.end()) {
      target = it->second;
      have = true;
    }
  }
  if (!have) {
    const rb::loader::GuestModule* m = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_published_mu);
      const auto it = g_published_libs.find(lib);
      if (it != g_published_libs.end()) m = it->second;
    }
    const auto at = m != nullptr ? m->FindExport(name) : std::nullopt;
    if (!at || !R().inbound) {
      // ART only calls a native after its library loaded; a library whose boot failed is not published, so this is
      // the native that would have run in it — said by name, in the Java caller's stack trace.
      const std::string why = m == nullptr ? lib + " did not boot (its rb-report.txt says why)"
                                           : lib + " exports no " + name;
      __android_log_print(ANDROID_LOG_ERROR, kTag, "stub: %s: %s", name.c_str(), why.c_str());
      JNIEnv* env = reinterpret_cast<JNIEnv*>(args->x[0]);
      jclass c = env->FindClass("java/lang/UnsatisfiedLinkError");
      if (c != nullptr) env->ThrowNew(c, ("RetroBug: " + name + ": " + why).c_str());
      return 0;
    }
    target = rb::runtime::InboundTarget{at->tagged(), base + e.shorty_off, name};
    std::lock_guard<std::mutex> lk(cache_mu);
    cache[{tag, index}] = target;
  }
  return R().inbound->Dispatch(target, *args);
}

namespace {

bool BootScope(Runtime& rt, ApkZip& zip, std::string* out) {
  rb::runtime::GuestProcessOptions o;
  o.window_bytes = kWindowBytes;
  o.biased = true;
  o.unresolved_trap = kTrap;
  o.ctor_tier = kTier;
  o.log = [](const std::string& s) { Live(s); };
  o.host_resolver = [](const std::string& name) -> std::optional<rb::exec::GuestAddr> {
    Runtime& r = R();
    return r.slots ? r.slots->Resolve(name) : std::nullopt;
  };
  std::string err;
  auto proc = GuestProcess::Create(o, &err);
  if (!proc) {
    Line(out, "FATAL: " + err);
    return false;
  }
  rt.proc = std::move(proc);
  rt.slots = std::make_unique<rb::launcher::HostSlots>(*rt.proc, TargetSdk(),
                                                       [](const std::string& s) { Live(s); });
  if (!BootBridge(rt, out)) return false;
  // Boot-only: no asset or file is served yet (M3). A provider that answers nothing makes every guest
  // openat an ENOENT the monitor's notes name, rather than ENOSYS.
  rt.proc->monitor().set_file_provider([](const std::string&) -> std::optional<std::vector<std::uint8_t>> {
    return std::nullopt;
  });
  std::vector<std::uint8_t> sys[3];
  const char* const names[3] = {"libc.so", "libm.so", "libz.so"};
  for (int i = 0; i < 3; ++i) {
    const std::string entry = std::string{"assets/rb32sys/"} + names[i];
    auto bytes = zip.Read(entry, &err);
    if (!bytes) {
      Line(out, "FATAL: rb_runtime_boot: " + entry + ": " + err);
      rt.slots.reset();
      rt.proc.reset();
      return false;
    }
    sys[i] = std::move(*bytes);
    Line(out, "rb_runtime_boot: " + entry + " = " + std::to_string(sys[i].size()) + " bytes");
  }
  if (!rt.proc->LoadScope(sys[0], sys[1], sys[2], {}, &err)) {
    Line(out, "FATAL: " + err);
    // A scope that did not boot leaves nothing worth keeping; the next call starts over.
    rt.slots.reset();
    rt.proc.reset();
    return false;
  }
  Line(out, "scope: booted once — libc.so, libm.so, libz.so in a 4 GiB biased window, unresolved imports"
            " bound to " + Hex(kTrap) + "; process-lifetime, reused by every later rb_runtime_boot");
  return true;
}

/// Loads `lib` from `assets/rb32/` after every sibling its DT_NEEDED names (and libstdc++ on demand), as a
/// device's linker loads a library's dependencies first. nullptr after a FATAL line.
const rb::loader::GuestModule* LoadWithNeeded(Runtime& rt, ApkZip& zip, const std::string& lib,
                                              std::set<std::string>& in_progress, std::string* out) {
  if (const auto hit = rt.loaded.find(lib); hit != rt.loaded.end()) return hit->second;
  if (!in_progress.insert(lib).second) {
    Line(out, "FATAL: rb_runtime_boot: a DT_NEEDED cycle reaches " + lib + " again");
    return nullptr;
  }
  std::string err;
  auto image = zip.Read("assets/rb32/" + lib, &err);
  if (!image) {
    Line(out, "FATAL: rb_runtime_boot: assets/rb32/" + lib + ": " + err);
    return nullptr;
  }
  Line(out, "rb_runtime_boot: assets/rb32/" + lib + " = " + std::to_string(image->size()) + " bytes");
  std::string host_side;
  for (const std::string& n : rb::launcher::NeededSonames(*image)) {
    if (n == "libc.so" || n == "libm.so" || n == "libz.so" || rt.loaded.count(n) != 0) continue;
    // libstdc++ comes in through LoadApp, on demand, where the launcher's Inspect passes it to LoadScope:
    // this scope is booted once for every library the process will ever load, before anyone knows whether
    // one of them needs libstdc++, so it joins later the way a device's linker loads a DT_NEEDED — after
    // the scope, before its user, its constructors run with the user's.
    if (n == "libstdc++.so" && zip.Has("assets/rb32sys/libstdc++.so")) {
      auto cxx = zip.Read("assets/rb32sys/libstdc++.so", &err);
      rt.touched = true;
      const rb::loader::GuestModule* m = cxx ? rt.proc->LoadApp(*cxx, n, &err) : nullptr;
      if (m == nullptr) {
        Line(out, "FATAL: rb_runtime_boot: " + lib + " needs libstdc++.so, which did not load: " + err);
        return nullptr;
      }
      rt.loaded[n] = m;
      Line(out, "needs: " + lib + " -> libstdc++.so, loaded from assets/rb32sys/ on demand");
    } else if (zip.Has("assets/rb32/" + n)) {
      Line(out, "needs: " + lib + " -> " + n + ", a sibling in assets/rb32/, loaded first");
      if (LoadWithNeeded(rt, zip, n, in_progress, out) == nullptr) return nullptr;
    } else {
      host_side += (host_side.empty() ? "" : ", ") + n;
    }
  }
  if (!host_side.empty()) {
    Line(out, "needs: " + lib + " -> " + host_side + " — no image in the APK; its imports are answered by the"
              " host (dl*, liblog) or trapped by name at " + Hex(kTrap));
  }
  const std::size_t unanswered_before = rt.proc->unanswered().size();
  rt.touched = true;
  const rb::loader::GuestModule* m = rt.proc->LoadApp(*image, lib, &err);
  if (m == nullptr) {
    Line(out, "FATAL: " + err);
    return nullptr;
  }
  rt.loaded[lib] = m;
  const auto& un = rt.proc->unanswered();
  if (un.size() > unanswered_before) {
    std::string names;
    for (std::size_t i = unanswered_before; i < un.size(); ++i) names += (names.empty() ? "" : ", ") + un[i];
    Line(out, "imports: " + lib + ": " + std::to_string(un.size() - unanswered_before) +
              " nothing answers, bound to " + Hex(kTrap) + " (a call faults by name; weak ones are 0): " + names);
  }
  return m;
}

/// The whole boot of one library. True only when the scope, the loads, every constructor and (when
/// exported) `JNI_OnLoad` all returned.
bool BootOne(Runtime& rt, const std::string& apk, const std::string& lib, std::string* out) {
  if (apk.empty()) {
    Line(out, "FATAL: rb_runtime_boot: no APK path was given — the stub could not find the game's sourceDir");
    return false;
  }
  if (lib.empty() || lib.find('/') != std::string::npos) {
    Line(out, "FATAL: rb_runtime_boot: '" + lib + "' is not a library file name");
    return false;
  }
  ApkZip zip;
  std::string err;
  if (!zip.Open(apk, &err)) {
    Line(out, "FATAL: rb_runtime_boot: cannot read the APK: " + err);
    return false;
  }
  if (!rt.proc) {
    if (!BootScope(rt, zip, out)) return false;
  } else {
    Line(out, "scope: reused — booted by an earlier rb_runtime_boot in this process (" +
                  std::to_string(rt.proc->constructors_run()) + " constructors run so far); " + lib +
                  " loads against it");
    // A later boot can arrive on a different Java thread (each System.loadLibrary runs on its caller's thread).
    // The main guest thread stays with the host thread that booted the scope (M2 Task 5 ruling a): this one
    // is adopted on its first guest call (its own pthread, stack and TLS), as on a device.
  }
  GuestProcess& p = *rt.proc;
  std::set<std::string> in_progress;
  const rb::loader::GuestModule* app = LoadWithNeeded(rt, zip, lib, in_progress, out);
  if (app == nullptr) {
    rt.failure = lib + " did not load";
    Line(out, "RESULT: stopped during startup — " + lib + " did not load (the FATAL line above says why)");
    return false;
  }
  std::string failed;
  rt.touched = true;
  if (!p.RunConstructors(&failed)) {
    rt.failure = "constructor " + failed;
    Line(out, "RESULT: stopped during startup — constructor " + failed);
    return false;
  }
  if (app->init.addr == 0 && app->init_array.empty()) {
    Line(out, "constructors: " + lib + " carries no DT_INIT_ARRAY and no DT_INIT");
  }
  std::size_t java_entries = 0;
  for (const std::string& name : app->ExportNames()) {
    if (name.compare(0, 5, "Java_") == 0) ++java_entries;
  }
  const auto onload = app->FindExport("JNI_OnLoad");
  if (!onload) {
    Line(out, "JNI_OnLoad: " + lib + " exports none; its " + std::to_string(java_entries) +
                  " Java_* entry points are called by its own Java");
    Line(out, "RESULT: startup finished with no fault — every constructor returned (" + lib +
                  " has no JNI_OnLoad). Boot-only: nothing drives the game yet");
    return true;
  }
  // The guest JNI_OnLoad gets the bridge's JavaVM, synchronously on this (the loading) thread, inside a local frame
  // of its own as ART gives one.
  Line(out, "JNI_OnLoad: guest JavaVM at " + Hex(rt.fwd->vm()) + ", JNIEnv at " + Hex(rt.fwd->env()));
  p.abi().set_executor(p.engine(kTier));
  rb::exec::RunResult rr{};
  rb::exec::MachineState exit{};
  rt.fwd->PushFrame();
  const rb::abi::Ret32 ret = p.abi().CallGuest(onload->tagged(), {rb::abi::Value32::Ptr(rt.fwd->vm()), rb::abi::Value32::Ptr(0)},
                                               rb::abi::Shorty::For("II"), GuestProcess::kCallBudget, &rr, &exit);
  rt.fwd->PopFrame(0);
  if (rr.reason != rb::exec::StopReason::Returned) {
    Line(out, "JNI_OnLoad: did not return on Tier J after " + std::to_string(rr.instructions_executed) +
                  " steps: " + rr.detail + "; pc " + Hex(exit.pc) + " lr " + Hex(exit.r[14]));
    rt.failure = lib + " JNI_OnLoad did not return: " + rr.detail;
    Line(out, "RESULT: the constructors finished, then JNI_OnLoad stopped — the line above says where");
    return false;
  }
  Line(out, "JNI_OnLoad: returned " + Hex(ret.w()) + " on Tier J, " + std::to_string(rr.instructions_executed) + " steps");
  Line(out, "RESULT: startup finished with no fault — every constructor and JNI_OnLoad returned. Boot-only: " +
                std::to_string(java_entries) + " Java_* entry points wait for the game's own Java");
  return true;
}

void Tallies(Runtime& rt, std::string* out) {
  if (!rt.proc) return;
  // Guest threads the boot started (a JNI_OnLoad's worker) may be making syscalls and dl calls right now: the
  // monitor's tallies and notes and the slots' dl records are GGL-protected, so they are read holding it.
  rb::runtime::Ggl::Held hold(rt.proc->ggl());
  const auto say = [out](const std::string& s) { Line(out, s); };
  rt.slots->ReportDl(say, kTrap);
  const std::string tally = rt.proc->monitor().TallyText();
  Line(out, "syscalls (this process so far): " + (tally.empty() ? std::string{"none"} : tally));
  for (const std::string& note : rt.proc->monitor().notes()) Line(out, "syscall note: " + note);
}

}  // namespace

rb::runtime::GuestProcess* rb_runtime_process() {
  // Defined during a boot too: null until a scope finished booting, never waiting for the boot in progress.
  std::lock_guard<std::mutex> lk(g_published_mu);
  return g_published_proc;
}

extern "C" int rb_runtime_boot(JavaVM* vm, const char* apk_path, const char* lib_name) {
  const std::string lib = lib_name != nullptr ? lib_name : "";
  const std::string apk = apk_path != nullptr ? apk_path : "";
  std::lock_guard<std::mutex> lock(g_mu);  // held until return: one boot at a time
  Runtime& rt = R();
  if (rt.vm == nullptr) rt.vm = vm;  // one VM per process: the first stub's
  if (rt.booted.count(lib) != 0) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "rb_runtime_boot: %s already booted in this process", lib.c_str());
    return JNI_VERSION_1_6;
  }
  std::string report;
  SetReport(rt, &report);
  Line(&report, "rb_runtime_boot: lib=" + (lib.empty() ? std::string{"(none)"} : lib) +
                    " apk=" + (apk.empty() ? std::string{"(none)"} : apk));
  bool ok = false;
  rt.touched = false;
  rt.failure.clear();
  if (!rt.poisoned.empty()) {
    Line(&report, "FATAL: rb_runtime_boot: refused — the guest process faulted in an earlier boot: " + rt.poisoned +
                      ". It is half-initialized, so no guest work runs in this process again");
    Line(&report, "RESULT: stopped during startup — the guest process faulted in an earlier boot (above)");
  } else {
    try {
      ok = BootOne(rt, apk, lib, &report);
      Tallies(rt, &report);
    } catch (const std::exception& e) {
      Line(&report, std::string{"FATAL: unhandled exception while booting: "} + e.what());
      Line(&report, "RESULT: stopped during startup — a host exception (above)");
      rt.failure = std::string{"a host exception: "} + e.what();
      rt.touched = rt.touched || rt.proc != nullptr;
    } catch (...) {
      Line(&report, "FATAL: unhandled exception of a non-standard type while booting");
      Line(&report, "RESULT: stopped during startup — a host exception (above)");
      rt.failure = "a host exception of a non-standard type";
      rt.touched = rt.touched || rt.proc != nullptr;
    }
    if (ok) {
      rt.booted.insert(lib);
      std::lock_guard<std::mutex> lk(g_published_mu);
      g_published_proc = rt.proc.get();
      g_published_libs[lib] = rt.loaded[lib];
    } else if (rt.touched && rt.proc) {
      // Guest state changed before the failure: nothing in this process may run guest code again.
      rt.poisoned = lib + ": " + (rt.failure.empty() ? std::string{"see its report"} : rt.failure);
      Line(&report, "rb_runtime_boot: the guest process is now refused for every later boot (" + rt.poisoned + ")");
    }
    // A failure before any guest state changed (no APK, a missing entry, a scope that did not boot and was
    // discarded) leaves nothing behind, so a later call may try again.
  }
  SetReport(rt, nullptr);

  const std::string pkg = PackageName();
  if (pkg.empty()) {
    Line(nullptr, "rb_runtime_boot: report not written — /proc/self/cmdline names no package");
    return JNI_VERSION_1_6;
  }
  const std::string dir = "/data/user/" + std::to_string(::getuid() / 100000) + "/" + pkg + "/files";
  ::mkdir(dir.c_str(), 0771);  // usually there already; a failure shows up as the fopen's below
  const std::string path = dir + "/rb-report.txt";
  FILE* f = std::fopen(path.c_str(), "wb");
  bool written = false;
  if (f != nullptr) {
    written = std::fwrite(report.data(), 1, report.size(), f) == report.size();
    written = std::fclose(f) == 0 && written;
  }
  if (written) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "rb_runtime_boot: report -> %s", path.c_str());
  } else {
    __android_log_print(ANDROID_LOG_WARN, kTag, "rb_runtime_boot: could not write %s: %s", path.c_str(),
                        std::strerror(errno));
  }
  return JNI_VERSION_1_6;
}
