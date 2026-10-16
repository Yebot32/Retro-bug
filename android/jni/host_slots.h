// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The host slots a guest process needs whatever app it runs: the dynamic-linker interface (`dlopen` and
// friends, which on a device live in the linker) and liblog. Shared by the launcher's `Inspect` and the
// in-app runtime (`runtime_boot.cpp`), so both answer libc's own constructors (netd's `dlopen`) and a
// game's log calls the same way. The GL set is the launcher's alone (M4 brings it to the runtime).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

  /// The slot address for `name` when it is one of this set, nullopt otherwise.
  std::optional<std::uint32_t> Resolve(const std::string& name) const;

  const std::vector<DlCall>& dl_calls() const { return dl_calls_; }
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
};

}  // namespace rb::launcher
