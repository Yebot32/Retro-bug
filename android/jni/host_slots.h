// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The host slots a guest process needs whatever app it runs: the dynamic-linker interface (`dlopen` and
// friends, which on a device live in the linker) and liblog. Shared by the launcher's `Inspect` and the
// in-app runtime (`runtime_boot.cpp`), so both answer libc's own constructors (netd's `dlopen`) and a
// game's log calls the same way. The GLES1 set (slots 256..319) is opt-in: the in-app runtime turns it on
// (`enable_gl`); the launcher keeps its own GL slots there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gl_forward.h"
#include "rb/runtime/guest_process.h"

namespace rb::launcher {

/// A guest NUL-terminated string for diagnostic reports, bounded (256 bytes) and refusal-free: an
/// unreadable or absurd address yields the empty string.
std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at);

/// The `DT_NEEDED` sonames of an ELF32 image, read from its bytes (before it is loaded). Bounds-checked;
/// anything malformed yields what could be read (the loader names the damage later).
std::vector<std::string> NeededSonames(const std::vector<std::uint8_t>& image);

class HostSlots {
 public:
  /// Above bionic's JNI table and the launcher's GL window (256..319), below `AbiContext::kMaxSlots`.
  static constexpr std::uint32_t kDlSlotBase = 320u;
  static constexpr std::size_t kDlSlots = 16;
  static constexpr std::uint32_t kLogSlotBase = 336u;
  static constexpr std::size_t kLogMax = 300;
  /// The GLES1 window (opt-in, `enable_gl`): one slot per `gl*` name the guest imports, given on first sight.
  static constexpr std::uint32_t kGlSlotBase = 256u;
  static constexpr std::size_t kGlSlots = 64;

  struct GlCall {
    std::string name;
    unsigned calls = 0;
    std::string refused;  ///< why the first refused call was refused; empty while every call was forwarded
    unsigned refusals = 0;
  };

  struct DlCall {
    std::string name;
    unsigned calls = 0;
    std::string last_path;
    unsigned answered = 0;  ///< dl_unwind_find_exidx: calls whose pc a loaded module's index covered
  };
  struct LogLine {
    int prio = 0;
    std::string tag, text;
  };

  /// Binds every slot on `p`'s bridge now. `target_sdk` is what
  /// `android_get_application_target_sdk_version` answers. `live_log`, when set, also receives each guest
  /// log line as it is written ("<P>/<tag>: <text>").
  HostSlots(runtime::GuestProcess& p, int target_sdk, std::function<void(const std::string&)> live_log = {});
  HostSlots(const HostSlots&) = delete;
  HostSlots& operator=(const HostSlots&) = delete;

  /// The slot address for `name` when it is one of this set, nullopt otherwise. With GL on, a `gl*` name the
  /// forwarder knows (or `glGetString`) gets the next GL slot on first sight; others stay unanswered (trapped).
  std::optional<std::uint32_t> Resolve(const std::string& name);

  /// Turns the GLES1 set on: each bound `gl*` call goes to this device's libGLESv1_CM on the calling thread —
  /// the game's own GLSurfaceView made its context current there. A call that cannot be forwarded returns 0
  /// and is named (live log, once per name, and in `ReportGl`).
  void enable_gl() { gl_on_ = true; }

  const std::vector<DlCall>& dl_calls() const { return dl_calls_; }
  const std::vector<GlCall>& gl_calls() const { return gl_calls_; }
  void ReportGl(const std::function<void(const std::string&)>& say) const;
  const std::vector<LogLine>& log() const { return log_; }
  unsigned log_dropped() const { return log_dropped_; }

  /// The `dl:` block (what each name answers and which were called) and the `guest log:` block, one
  /// report line per call of `say`. `trap` is the unresolved-import address the report names.
  void ReportDl(const std::function<void(const std::string&)>& say, std::uint32_t trap) const;
  void ReportLog(const std::function<void(const std::string&)>& say) const;

 private:
  runtime::GuestProcess& p_;
  int target_sdk_;
  std::function<void(const std::string&)> live_log_;
  std::vector<DlCall> dl_calls_;
  std::vector<LogLine> log_;
  unsigned log_dropped_ = 0;
  bool gl_on_ = false;
  GlForward gl_;
  std::vector<GlCall> gl_calls_;
  std::map<std::uint32_t, std::uint32_t> gl_strings_;  ///< glGetString enum -> its copy in guest memory
};

}  // namespace rb::launcher
