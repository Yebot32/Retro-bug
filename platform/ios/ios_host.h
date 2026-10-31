// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS host slots (S1): the answers a guest's imported GLES/EGL names bind to. This is the iOS twin of
// `HostSlots` on Android, without the parts Android alone has (the dl/log tables, libandroid's JNI-object
// thunks, OpenSL ES): those are either core's job now (dlopen/dlsym live in `rb::host::Boot`) or belong to
// the audio/Java adapter.
//
// A name in `rb::gfx::FindThunk` gets one host slot and is forwarded through `rb::abi::ThunkCaller` to the
// EAGL adapter or OpenGLES.framework. The handful a signature cannot express (returned strings, out-arrays
// of configs, a name's *guest* address for eglGetProcAddress) are hand-written here.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "rb/abi/thunk.h"
#include "rb/runtime/guest_process.h"

namespace rb::audio {
class SilentSles;
}

namespace rb::host {
class Boot;
}

namespace rb::loader {
class ApkArchive;
class ApkFile;
}

namespace rb::platform::ios {

/// The host slot currently executing (a stable string owned by its binding). Read from a signal
/// handler for the last-resort crash line; no allocation, no lock.
const char* LastHostCall() noexcept;

/// One entry of the host-call ring: the bound name (a stable string, the binding's own) and the four
/// register arguments the guest passed. Read from the crash handler, so it is plain data.
struct RecentHostCall {
  const char* name = nullptr;
  std::uint32_t arg[4] = {};
};

/// Writes the app's host-call ring, oldest first, through `emit` — allocation-free, for a signal handler.
/// A crash inside a host call shows the calls that led to it, which is the only evidence a driver fault
/// leaves: the stack is the driver's, not the guest's.
void DumpRecentHostCalls(void (*emit)(const char*));

class IosHost {
 public:
  static constexpr std::uint32_t kThunkSlotBase = 1024u;
  using Special = std::function<bool(exec::MachineState& s, std::string* why)>;

  IosHost(rb::runtime::GuestProcess& process, rb::host::Boot& boot,
          std::function<void(const std::string&)> log);
  // Out of line: the members own APK objects that are only forward-declared here.
  ~IosHost();
  IosHost(const IosHost&) = delete;
  IosHost& operator=(const IosHost&) = delete;

  /// The guest address `name` should bind to, or nullopt (it then stays a named trap). Repeated names answer
  /// the same slot. Called while the guest's libraries load, and by eglGetProcAddress at runtime.
  std::optional<std::uint32_t> Resolve(const std::string& name);

  abi::HandleTable& handles() { return handles_; }
  /// Copies `text` into guest memory once per `key` and returns its guest address; 0 when none is left.
  std::uint32_t GuestString(const std::string& key, const std::string& text);

  /// A hand-written thunk. Register before Resolve sees the name; a special wins over the shared table.
  void AddSpecial(const std::string& name, Special fn) { specials_[name] = std::move(fn); }
  /// An imported object whose guest address the guest reads rather than calls (an OpenSL ES IID).
  void AddDataSymbol(const std::string& name, std::uint32_t at) { data_symbols_[name] = at; }

  /// The APK whose assets the guest's AAsset* calls read. Call before the guest's libraries load.
  bool OpenApk(const std::string& path, std::string* err);

  /// The ring `DumpRecentHostCalls` reads. `written` is monotonic; slot = n % kRecentCalls.
  static constexpr std::uint32_t kRecentCalls = 256;
  const std::array<RecentHostCall, kRecentCalls>& recent_calls() const { return recent_; }
  std::uint32_t recent_calls_written() const { return recent_next_.load(std::memory_order_relaxed); }
  /// The ring as "host call: name r0 r1 r2 r3" lines, oldest first — for the guest fault report.
  std::vector<std::string> RecentCallLines() const;

 private:
  static void* HostSymbol(const std::string& name);

  rb::runtime::GuestProcess& p_;
  rb::host::Boot* boot_ = nullptr;
  abi::HandleTable handles_;
  std::function<void(const std::string&)> log_;
  std::unique_ptr<rb::audio::SilentSles> sles_;  ///< no audio device: a real object model over a muted sink
  std::map<std::string, Special> specials_;
  std::map<std::string, std::uint32_t> slots_;
  std::map<std::string, std::uint32_t> strings_;
  std::set<std::string> refused_;  ///< names already reported once (a refusal is a log line, not a flood)
  std::array<RecentHostCall, kRecentCalls> recent_{};
  std::atomic<std::uint32_t> recent_next_{0};  ///< relaxed: the crash reader accepts a torn tail

  void EmitLog(int priority, const std::string& tag, const std::string& text);
  std::unique_ptr<rb::loader::ApkArchive> archive_;
  int asset_manager_marker_ = 0;
  std::map<const rb::loader::ApkFile*, std::unique_ptr<rb::loader::ApkFile>> assets_;
  /// Data imports (the OpenSL ES interface IDs): their guest address is a cell holding a value, not a
  /// host slot — the guest loads the value rather than calling it.
  std::map<std::string, std::uint32_t> data_symbols_;
  std::uint32_t bound_fbo_ = 0;  ///< the guest's current framebuffer; 0 is our EAGL drawable
  bool manager_logged_ = false;
  int asset_logs_ = 0;
  struct DirState;
  std::map<std::uint32_t, std::unique_ptr<DirState>> dirs_;
};

}  // namespace rb::platform::ios
