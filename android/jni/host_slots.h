// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The host slots a guest process needs whatever app it runs: the dynamic-linker interface (`dlopen` and
// friends, which on a device live in the linker) and liblog. Shared by the launcher's `Inspect` and the
// in-app runtime (`runtime_boot.cpp`), so both answer libc's own constructors (netd's `dlopen`) and a
// game's log calls the same way. The thunk set (M4: EGL, GLES 1/2, libandroid, OpenSL ES, jnigraphics; slots
// 1024..4095) is opt-in: the in-app runtime turns it on (`enable_thunks`); the launcher keeps its own GL slots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rb/abi/thunk.h"
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
  /// The thunk window (opt-in, `enable_thunks`): one slot per imported name, given on first sight.
  static constexpr std::uint32_t kThunkSlotBase = 1024u;
  static constexpr std::size_t kThunkSlots = abi::AbiContext::kMaxSlots - kThunkSlotBase;
  /// What dlopen("libnetd_client.so") answers with the thunks on (a stand-in only dlsym understands).
  static constexpr std::uint32_t kNetdHandle = 0xC0DE0001u;

  struct ThunkCall {
    std::string name;
    unsigned calls = 0;
    std::string refused;  ///< why the first refused call was refused; empty while every call was forwarded
    unsigned refusals = 0;
  };
  /// A hand-written thunk: reads its arguments from the guest state, does the call, writes r0 (r1). False with
  /// `why` when it could not be done (the guest then gets 0 and the name is reported once).
  using Special = std::function<bool(exec::MachineState& s, std::string* why)>;

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

  /// The slot address for `name` when it is one of this set, nullopt otherwise. With thunks on, a name with a
  /// table entry (thunks.h) or a registered special gets the next thunk slot on first sight; others stay
  /// unanswered (trapped, faulting by name). Also what `eglGetProcAddress` answers from.
  std::optional<std::uint32_t> Resolve(const std::string& name);

  /// Turns the thunk set on. Each bound call goes to this device's library on the calling thread (GL/EGL: the
  /// game's own GLSurfaceView made its context current there). A call that cannot be forwarded returns 0 and is
  /// named (live log, once per name, and in `ReportThunks`).
  void enable_thunks();
  /// Registers a hand-written thunk for `name` (before the guest's libraries are loaded). Wins over a table entry.
  void AddSpecial(const std::string& name, Special fn) { specials_[name] = std::move(fn); }
  /// Host pointers the guest holds (EGL objects, AAsset*, OpenSL objects, ...), shared by every thunk.
  abi::HandleTable& handles() { return handles_; }
  runtime::GuestProcess& process() { return p_; }
  /// Copies `text` into guest memory once per `key` (a string a host API returned) and returns its guest
  /// address, 0 when no guest memory is left. Called with the GGL held.
  std::uint32_t GuestString(const std::string& key, const std::string& text);

  const std::vector<DlCall>& dl_calls() const { return dl_calls_; }
  const std::deque<ThunkCall>& thunk_calls() const { return thunk_calls_; }
  void ReportThunks(const std::function<void(const std::string&)>& say) const;
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
  bool thunks_on_ = false;
  abi::HandleTable handles_;
  std::map<std::string, Special> specials_;
  std::deque<ThunkCall> thunk_calls_;  ///< a deque: a running thunk holds a reference while eglGetProcAddress or
                                      ///< dlsym may add another
  std::map<std::string, std::uint32_t> guest_strings_;  ///< GuestString's copies
  std::mutex counters_mu_;  ///< leaf: the thunk counters, which slots that release the GGL also touch
};

}  // namespace rb::launcher
