// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_runtime_boot`: the Android half of the in-process boot. A repackaged game's `System.loadLibrary("X")`
// loads a 64-bit stub named libX.so (`stub.cpp`), whose `JNI_OnLoad` lands here with the game's own APK path.
// The 32-bit libraries and the guest system libraries travel inside that APK as assets (the repackager's
// layout: `assets/rb32/<libX.so>`, `assets/rb32sys/{libc,libm,libz,libstdc++}.so`).
//
// The boot itself is `rb::host::Boot` (core/host/boot.cpp), shared with iOS. This file is the Android
// platform behind it: ART as the Java, HostSlots/OpenSL ES as the host services, logcat as the log sink, and
// the app's files directory as the report destination. The stub dispatch and the overlay stay here because
// both are Android-only (ARM64 inbound trampolines and the shim's Java, respectively).

#include "runtime_boot.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "art_java.h"
#include "host_slots.h"
#include "thunks_android.h"
#include "thunks_sles.h"
#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/inbound.h"
#include "rb/runtime/guest_process.h"

namespace {

using rb::runtime::GuestProcess;

constexpr const char* kTag = "RBRUNTIME";

std::string PackageName();

/// The player's per-game settings (the launcher's GameSettings), as the repack's shim saved them at start:
/// getFilesDir()/rb-settings.properties. Absent or unreadable -> "".
std::string SettingsText() {
  static const std::string text = [] {
    const std::string pkg = PackageName();
    if (pkg.empty()) return std::string{};
    const std::string path = "/data/user/" + std::to_string(::getuid() / 100000) + "/" + pkg + "/files/rb-settings.properties";
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      // The whole file (patch and mod lists make it longer than a few keys), bounded to keep a corrupt file harmless.
      char buf[4096];
      for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (std::size_t{64} << 10);) s.append(buf, n);
      std::fclose(f);
    }
    return s;
  }();
  return text;
}

/// The value of `key` in the settings ("key=value" lines, as GameSettings.format writes them), "" when absent.
/// Exact key match: "overlay=10" is not "overlay=1", and "xcpu=safe" is not "cpu".
std::string Setting(const std::string& key) {
  const std::string& t = SettingsText();
  std::size_t at = 0;
  while (at < t.size()) {
    std::size_t end = t.find('\n', at);
    if (end == std::string::npos) end = t.size();
    std::string line = t.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t eq = line.find('=');
    if (eq != std::string::npos && line.compare(0, eq, key) == 0 && eq == key.size()) return line.substr(eq + 1);
    at = end + 1;
  }
  return {};
}

/// The tier guest code runs on: Tier J (the JIT) unless the player chose "Safe" (Tier I, the interpreter).
int Tier() {
  static const int tier = Setting("cpu") == "safe" ? 1 : 2;
  return tier;
}

/// What a device's linker would answer for this app: the host's own answer, since the stub runs in it.
int TargetSdk() {
  using Fn = int (*)();
  if (auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "android_get_application_target_sdk_version"))) return fn();
  return 24;  // the repackager's floor (HANDOFF M1 deviation 3)
}

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

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

void LogLine(const std::string& s) { __android_log_print(ANDROID_LOG_INFO, kTag, "%s", s.c_str()); }

/// This process's Android platform: the VM the first stub handed in, the boot, and the host services the
/// boot installs. Process-lifetime and never destroyed (the guest process lives as long as the app does).
struct Platform {
  JavaVM* vm = nullptr;
  std::unique_ptr<rb::host::Boot> boot;
  std::unique_ptr<rb::launcher::HostSlots> slots;
  std::unique_ptr<rb::launcher::Sles> sles;  ///< OpenSL ES; uses slots' handles and the process's memory
  rb::launcher::ArtJava* art = nullptr;     ///< the Java behind the bridge (owned by the boot)
  std::mutex mu;                            ///< guards the first construction of `boot`
};

Platform& P() {
  static Platform* p = new Platform;
  return *p;
}

/// The host services that need the bridge: the import answers (GLES/EGL/dl/log, then OpenSL ES), the
/// Android natives, the guest dlopen/dlsym hooks, file I/O and the diagnostic traces.
void InstallPlatform(rb::host::Boot& b, GuestProcess& p) {
  Platform& plat = P();
  plat.slots = std::make_unique<rb::launcher::HostSlots>(p, TargetSdk(), [](const std::string& s) { P().boot->Live(s); });
  // The game's own GLSurfaceView makes its GLES1 context current on its GL thread, where its natives call gl*.
  plat.slots->enable_thunks();
  // libandroid and jnigraphics calls that take a Java object need the bridge (M4 Task 3).
  rb::launcher::RegisterAndroidThunks(*plat.slots, *b.jni(), plat.vm);
  // Guest dlopen/dlsym of the game's own libraries (Unity) and of system ones (dlsym'd GLES/EGL entry points).
  plat.slots->SetDlHooks([&b](const std::string& path, std::string* why) { return b.DlOpen(path, why); },
                         [&b](std::uint32_t h, const std::string& name) { return b.DlSym(h, name); });
  // OpenSL ES (M4 Task 4): engines, output mixes, players and their buffer queues, as guest objects whose
  // vtables are host slots. Callbacks come back on the host's audio threads, adopted to call the guest there.
  {
    rb::launcher::HostSlots* slots = plat.slots.get();
    rb::launcher::SlesHost h;
    h.mem = &p.memory();
    h.handles = &slots->handles();
    h.alloc = [&p](std::size_t bytes) -> std::uint32_t {
      std::string e;
      return p.MapFree(bytes, rb::mem::kRead | rb::mem::kWrite, &e);
    };
    h.bind = [slots](const std::string& name, rb::launcher::SlesHost::Method fn) -> std::uint32_t {
      slots->AddSpecial(name, std::move(fn));
      const auto at = slots->Resolve(name);
      return at ? *at : 0u;
    };
    h.call_guest = [&p](std::uint32_t fn, const std::vector<std::uint32_t>& args) {
      std::vector<rb::abi::Value32> v;
      for (std::uint32_t a : args) v.push_back(rb::abi::Value32::U32(a));
      rb::exec::RunResult rr{};
      p.abi().CallGuestToCompletion(fn, rb::abi::Args32{v}, rb::abi::Shorty::For("V"), GuestProcess::kThreadTurnBudget,
                                    &rr);
      if (rr.reason != rb::exec::StopReason::Returned) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%x", static_cast<unsigned>(fn));
        P().boot->Live(std::string{"sles: guest callback "} + buf + " did not return: " + rr.detail);
      }
    };
    h.unlocked = [&p](const std::function<void()>& f) {
      rb::runtime::Ggl::Released off(p.abi().ggl());
      f();
    };
    h.say = [](const std::string& s) { P().boot->Live(s); };
    plat.sles = std::make_unique<rb::launcher::Sles>(std::move(h));
    rb::launcher::Sles* sles = plat.sles.get();
    slots->AddResolver([sles](const std::string& n) { return sles->Resolve(n); });
  }
  // Files and sockets are the host's (M4 Task 3b): the game reads and writes its own sandbox and talks to the
  // network with the app's permissions, as its native code does on a device. No path is virtualized yet.
  p.monitor().set_file_provider([](const std::string&) -> std::optional<std::vector<std::uint8_t>> {
    return std::nullopt;
  });
  p.monitor().set_host_io(true);
  {
    // The first host-I/O answers go to logcat as they happen: what a game's file and network code did is
    // otherwise invisible after boot. Capped so a long run does not flood the log.
    auto left = std::make_shared<std::atomic<int>>(6000);
    p.monitor().set_host_io_trace([left](const std::string& line) {
      // Errors and everything but the routine file walk (stat, getdents, read, close) of a successful call.
      const bool error = line.find(" = -") != std::string::npos;
      const bool routine = line.find("fstatat64") != std::string::npos || line.find("nr:0x18d") != std::string::npos ||
                           line.find("getdents64") != std::string::npos ||
                           line.find("host close ") != std::string::npos || line.find("host fstat64 ") != std::string::npos ||
                           line.find("host nr:0x148") != std::string::npos ||
                           line.find("host read(") != std::string::npos || line.find("host nr:0x8c(") != std::string::npos;
      // A search-path probe that misses (a game looking for a file in each of its directories) is routine too: one
      // asset can cost a dozen of them, and they spent the whole budget in a minute of Fruit Ninja's loading.
      const bool probe_miss = line.find(" = -2") != std::string::npos &&
                              (line.find("fstatat64(") != std::string::npos || line.find("openat(") != std::string::npos ||
                               line.find("faccessat(") != std::string::npos);
      if (((error && !probe_miss) || !routine) && left->fetch_sub(1) > 0) P().boot->Live("io: " + line);
    });
    // Signal dispositions on their own channel, uncapped relative to the I/O trace and never deduplicated:
    // the set of handlers a guest installs is the fact C26's delivery is built from, and a report that could
    // show only the first three was the reason a wrong conclusion about signals survived a session.
    p.monitor().set_signal_trace([](const std::string& line) { P().boot->Live("sig: " + line); });
  }
}

rb::host::BootConfig MakeConfig(Platform& p) {
  rb::host::BootConfig c;
  c.ctor_tier = Tier();
  c.target_sdk = TargetSdk();
  c.log = LogLine;
  c.make_java = [&p](std::string* err) -> std::unique_ptr<rb::abi::HostJava> {
    if (p.vm == nullptr) {
      if (err != nullptr) *err = "no JavaVM was handed in";
      return nullptr;
    }
    auto java = std::make_unique<rb::launcher::ArtJava>(p.vm);
    p.art = java.get();
    return java;
  };
  c.resolve_host = [&p](const std::string& name) -> std::optional<rb::exec::GuestAddr> {
    return p.slots ? p.slots->Resolve(name) : std::nullopt;
  };
  c.on_bridge = [](rb::host::Boot& b, GuestProcess& proc) { InstallPlatform(b, proc); };
  c.report_suffix = [](std::string*, GuestProcess& proc) {
    // Guest threads the boot started may be making syscalls and dl calls right now: the monitor's tallies and
    // notes and the slots' dl records are GGL-protected, so they are read holding it.
    rb::runtime::Ggl::Held hold(proc.ggl());
    const auto say = [](const std::string& s) { P().boot->Live(s); };
    P().slots->ReportDl(say, 0xfffe'0000u);
    const std::string tally = proc.monitor().TallyText();
    say("syscalls (this process so far): " + (tally.empty() ? std::string{"none"} : tally));
    for (const std::string& note : proc.monitor().notes()) say("syscall note: " + note);
  };
  c.java_thread_dump = [&p]() -> std::vector<std::string> {
    return p.art != nullptr ? p.art->DumpThreads() : std::vector<std::string>{};
  };
  c.frame_stats = [&p]() -> std::string {
    if (!p.slots) return {};
    return "eglSwapBuffers " + std::to_string(p.slots->CallsOf("eglSwapBuffers")) + ", glClear " +
           std::to_string(p.slots->CallsOf("glClear")) + ", glDrawArrays " +
           std::to_string(p.slots->CallsOf("glDrawArrays")) + ", glDrawElements " +
           std::to_string(p.slots->CallsOf("glDrawElements")) + ", glBindTexture " +
           std::to_string(p.slots->CallsOf("glBindTexture"));
  };
  return c;
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
    rb::host::Boot* boot = P().boot.get();
    const rb::loader::GuestModule* m = boot != nullptr ? boot->module(lib) : nullptr;
    const auto at = m != nullptr ? m->FindExport(name) : std::nullopt;
    if (!at || boot == nullptr || boot->inbound() == nullptr) {
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
  rb::host::Boot* boot = P().boot.get();
  if (boot == nullptr || boot->inbound() == nullptr) return 0;
  return boot->inbound()->Dispatch(target, *args);
}

rb::runtime::GuestProcess* rb_runtime_process() {
  // Defined during a boot too: null until a scope finished booting, never waiting for the boot in progress.
  rb::host::Boot* boot = P().boot.get();
  return boot != nullptr ? boot->process() : nullptr;
}

extern "C" int rb_runtime_boot(JavaVM* vm, const char* apk_path, const char* lib_name) {
  const std::string lib = lib_name != nullptr ? lib_name : "";
  const std::string apk = apk_path != nullptr ? apk_path : "";
  Platform& p = P();
  {
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.vm == nullptr) p.vm = vm;  // one VM per process: the first stub's
    if (!p.boot) p.boot = std::make_unique<rb::host::Boot>(MakeConfig(p));
  }
  const rb::host::BootResult res = p.boot->BootLibrary(apk, lib);
  if (res.already) return JNI_VERSION_1_6;

  const std::string pkg = PackageName();
  if (pkg.empty()) {
    p.boot->Live("rb_runtime_boot: report not written — /proc/self/cmdline names no package");
    return JNI_VERSION_1_6;
  }
  const std::string dir = "/data/user/" + std::to_string(::getuid() / 100000) + "/" + pkg + "/files";
  ::mkdir(dir.c_str(), 0771);  // usually there already; a failure shows up as the fopen's below
  const std::string path = dir + "/rb-report.txt";
  FILE* f = std::fopen(path.c_str(), "wb");
  bool written = false;
  if (f != nullptr) {
    written = std::fwrite(res.report.data(), 1, res.report.size(), f) == res.report.size();
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

/// The performance overlay's numbers (the repack's shim, com.retrobug.shim.Overlay, asks once a second): frames since
/// the last call, counted at eglSwapBuffers, and the guest instruction rate from the slice count (kSliceSteps per
/// slice). The thunk counts are read under HostSlots' counters lock (CallsOf); the slice count is a plain racy read,
/// which is all a readout needs.
extern "C" JNIEXPORT jstring JNICALL Java_com_retrobug_shim_Overlay_nativeStats(JNIEnv* env, jclass) {
  static std::uint64_t last_frames = 0, last_slices = 0;
  static bool last_swapped = false;
  static auto last = std::chrono::steady_clock::now();
  Platform& p = P();
  // Frames: the guest's eglSwapBuffers when it swaps itself; a game drawn through Java's GLSurfaceView swaps in Java,
  // so its glClear calls stand in (games clear once per frame, nearly always).
  std::uint64_t frames = 0, swaps = 0, clears = 0, slices = 0;
  if (p.slots) {
    swaps = p.slots->CallsOf("eglSwapBuffers");
    clears = p.slots->CallsOf("glClear");
  }
  const bool swapped = swaps != 0;
  frames = swapped ? swaps : clears;
  // The count switched from glClear to eglSwapBuffers (the game's first own swap): a new baseline, not a negative
  // or a wild difference between two unrelated counters.
  if (swapped != last_swapped || frames < last_frames) last_frames = frames;
  last_swapped = swapped;
  GuestProcess* proc = p.boot ? p.boot->process() : nullptr;
  if (proc) slices = proc->abi().slices();
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::max(0.001, std::chrono::duration<double>(now - last).count());
  const double fps = static_cast<double>(frames - last_frames) / dt;
  const double mips = static_cast<double>(slices - last_slices) * rb::abi::AbiContext::kSliceSteps / dt / 1e6;
  last_frames = frames;
  last_slices = slices;
  last = now;
  char buf[96];
  // A Tier I slice counts IR steps, not guest instructions (AbiContext::kSliceSteps), so its rate is named as steps.
  std::snprintf(buf, sizeof buf, "%.0f fps \xc2\xb7 %.0f M %s/s \xc2\xb7 Tier %s", fps, mips,
                Tier() == 2 ? "instr" : "steps", Tier() == 2 ? "J" : "I");
  return env->NewStringUTF(buf);
}
