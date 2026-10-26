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

namespace rb::loader {
class ApkArchive;
class ApkFile;
}

namespace rb::platform::ios {

class IosHost {
 public:
  static constexpr std::uint32_t kThunkSlotBase = 1024u;
  using Special = std::function<bool(exec::MachineState& s, std::string* why)>;

  IosHost(rb::runtime::GuestProcess& process, std::function<void(const std::string&)> log);
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

  /// The APK whose assets the guest's AAsset* calls read. Call before the guest's libraries load.
  bool OpenApk(const std::string& path, std::string* err);

 private:
  static void* HostSymbol(const std::string& name);

  rb::runtime::GuestProcess& p_;
  abi::HandleTable handles_;
  std::function<void(const std::string&)> log_;
  std::map<std::string, Special> specials_;
  std::map<std::string, std::uint32_t> slots_;
  std::map<std::string, std::uint32_t> strings_;
  std::set<std::string> refused_;  ///< names already reported once (a refusal is a log line, not a flood)

  void EmitLog(int priority, const std::string& tag, const std::string& text);
  std::unique_ptr<rb::loader::ApkArchive> archive_;
  int asset_manager_marker_ = 0;
  std::map<const rb::loader::ApkFile*, std::unique_ptr<rb::loader::ApkFile>> assets_;
};

}  // namespace rb::platform::ios
