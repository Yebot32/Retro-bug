// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::host::Boot`: the in-process boot without a platform in it. Loads a 32-bit library (and the
// `assets/rb32/` siblings its DT_NEEDED names) from an APK into one process-lifetime guest process whose
// libc/libm/libz scope is booted on the first call, runs the constructors and the guest JNI_OnLoad.
//
// This is the Android runtime's boot (`android/jni/runtime_boot.cpp`) with every Android decision moved to
// a callback: the Java implementation (ART on Android, a table shim on iOS), the host import answers
// (GLES/EGL/dl/log), the platform services that need the bridge (audio, thunks), the log sink and where the
// report goes. The loaded state, the poison rule and the dlopen/dlsym answers stay here, so both hosts boot
// a library the same way.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "rb/abi/host_java.h"
#include "rb/exec/machine_state.h"
#include "rb/runtime/guest_process.h"

namespace rb::abi {
class ForwardingJni;
}
namespace rb::loader {
class GuestModule;
class ZipReader;
}
namespace rb::runtime {
class Inbound;
}

namespace rb::host {

class Boot;

struct BootConfig {
  std::size_t window_bytes = std::size_t{4} << 30;
  rb::exec::GuestAddr unresolved_trap = 0xfffe'0000u;
  /// The tier constructors, IFUNC resolvers and guest entry points run on: 1 = Tier I, 2 = Tier J.
  int ctor_tier = 2;
  /// What `android_get_application_target_sdk_version` answers. Only the platform's host slots read it.
  int target_sdk = 24;

  /// One line per reportable fact. Called from the boot thread and from guest threads.
  std::function<void(const std::string&)> log;
  /// The Java behind the forwarding JNI bridge. Called once, on the first boot. nullptr + `err` refuses.
  std::function<std::unique_ptr<rb::abi::HostJava>(std::string* err)> make_java;
  /// The host's answers for imports no guest image defines (GLES/EGL/dl/log on Android; the gfx/audio
  /// adapters on iOS). Same contract as `GuestProcessOptions::host_resolver`.
  std::function<std::optional<rb::exec::GuestAddr>(const std::string&)> resolve_host;
  /// Platform services installed once the bridge is up and before any guest library loads: the host slots,
  /// audio, file providers, thunks that need the Java bridge. `boot.jni()` and `proc` are usable here.
  std::function<void(Boot& boot, rb::runtime::GuestProcess& proc)> on_bridge;
  /// Appended to the report while it is still attached (the platform's own tallies).
  std::function<void(std::string*, rb::runtime::GuestProcess&)> report_suffix;
  /// A Java thread dump for the stall report (empty when the platform has none).
  std::function<std::vector<std::string>()> java_thread_dump;
  /// The frame counters the stall report prints (Java-side GL swap and draw calls). Empty when none.
  std::function<std::string()> frame_stats;
  /// A guest native called exit(). The default logs the line and exits the process.
  std::function<void(int status)> on_guest_exit;
  /// Start the liveness supervisor (a stall dump every 10 s with no guest instruction retired).
  bool monitor = true;
};

struct BootResult {
  bool ok = false;
  /// False when the library had already booted in this process: no report was collected, as before.
  bool already = false;
  std::string report;
};

class Boot {
 public:
  explicit Boot(BootConfig config);
  ~Boot();
  Boot(const Boot&) = delete;
  Boot& operator=(const Boot&) = delete;

  /// Boots `lib` from the APK at `apk`. One at a time; a library is marked booted only on success, and a
  /// failure after guest state changed poisons the process for every later boot.
  BootResult BootLibrary(const std::string& apk, const std::string& lib);

  /// A line that belongs in the live log and, while a boot is collecting one, in its report.
  void Live(const std::string& line);

  /// The guest dlopen/android_dlopen_ext answer: a handle for a library this runtime can provide (the
  /// game's own from assets/rb32/, loaded with its DT_NEEDED and constructed), or 0 with `why`.
  std::uint32_t DlOpen(const std::string& path, std::string* why);
  /// The guest dlsym answer for a handle DlOpen gave out (or RTLD_DEFAULT/NEXT), then host answers.
  std::optional<std::uint32_t> DlSym(std::uint32_t handle, const std::string& name);
  /// Binds a guest native into the inbound pool (what a guest RegisterNatives reaches). 0 with `err`.
  void* BindNative(rb::exec::GuestAddr fn, const std::string& name, const std::string& sig, std::string* err);

  /// The live guest process (null before a scope booted). Set once, never cleared.
  rb::runtime::GuestProcess* process();
  /// The Java the bridge talks to, and the bridge itself (both live once the first boot reached the bridge).
  rb::abi::HostJava* java();
  rb::abi::ForwardingJni* jni();
  rb::runtime::Inbound* inbound();
  /// A library that booted, by file name (null when it did not).
  const rb::loader::GuestModule* module(const std::string& lib);
  bool booted(const std::string& lib) const;
  /// Why every later boot is refused, empty while the process is still usable.
  std::string poisoned() const;

 private:
  bool BootScope(const std::string& apk, rb::loader::ZipReader& zip, std::string* out);
  bool BootBridge(std::string* out);
  const rb::loader::GuestModule* LoadWithNeeded(rb::loader::ZipReader& zip, const std::string& lib,
                                                std::set<std::string>& in_progress, std::string* out);
  bool BootOne(const std::string& apk, const std::string& lib, std::string* out);
  void DropScope();
  void StartMonitor();
  void StopMonitor();
  std::uint32_t DlHandle(const rb::loader::GuestModule* m);
  void Say(const std::string& line);

  BootConfig cfg_;

  std::mutex boot_mu_;  ///< one boot at a time
  std::atomic<std::thread::id> boot_owner_{};  ///< the thread in BootLibrary, for DlOpen's reentrancy rule

  std::unique_ptr<rb::runtime::GuestProcess> proc_;
  std::unique_ptr<rb::abi::HostJava> java_;
  std::unique_ptr<rb::abi::ForwardingJni> jni_;
  std::unique_ptr<rb::runtime::Inbound> inbound_;
  std::map<std::string, const rb::loader::GuestModule*> loaded_;
  std::set<std::string> booted_;

  std::mutex report_mu_;   ///< leaf: held to append a line or swap the pointer
  std::string* report_ = nullptr;

  std::mutex publish_mu_;  ///< leaf: what rb_stub_dispatch reads without waiting for a boot
  rb::runtime::GuestProcess* published_proc_ = nullptr;
  std::map<std::string, const rb::loader::GuestModule*> published_libs_;

  std::string poisoned_;  ///< set when a boot failed after guest state changed
  bool touched_ = false;  ///< this boot changed guest state
  std::string failure_;   ///< this boot's failure, for `poisoned_`
  std::string apk_;       ///< the game's APK (where a guest dlopen finds assets/rb32/<lib>)
  std::vector<const rb::loader::GuestModule*> dl_handles_;

  std::thread monitor_;
  std::mutex monitor_mu_;
  std::condition_variable monitor_cv_;
  bool monitor_stop_ = false;
};

}  // namespace rb::host
