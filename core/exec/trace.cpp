/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The code-map trace. See rb/exec/trace.h for the contract, the file format and the reasoning.
 *
 * The disabled path is one relaxed atomic load and a branch; every other object this file needs
 * (mutex, set, file, buffer) is reached only after that branch has been taken. The recorder itself is
 * a leaked singleton: tracing can be stopped from `atexit` and from `Boot`'s destructor, and a trace
 * that outlived those does not need to run a destructor at all.
 */
#include "rb/exec/trace.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rb::exec::trace {

namespace {

/// One megabyte of stdio buffering, as the plan asks. A tracing run is diagnostic and can afford the
/// memory; a normal one never allocates it because `Start` is never called.
constexpr std::size_t kBufferBytes = 1u << 20;

struct Recorder {
  std::mutex mu;
  std::FILE* file = nullptr;
  std::unique_ptr<char[]> buffer;
  /// `(pc << 1) | thumb` — one record per guest address and ISA. A branch in ARM state and the same
  /// address in Thumb state are different starts, so the Thumb bit belongs in the key.
  std::unordered_set<std::uint64_t> seen;
  std::uint64_t records = 0;
  std::string path;
};

/// Never destroyed: `Stop` can run from static destruction, and a recorder whose mutex or set has
/// already been destructed would turn "the app is exiting" into a crash in a diagnostic.
Recorder* const g_recorder = new Recorder;

std::once_flag g_atexit_once;

void PutU32Le(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8);
  out[2] = static_cast<std::uint8_t>(value >> 16);
  out[3] = static_cast<std::uint8_t>(value >> 24);
}

void PutU64Le(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i != 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

bool WriteHeader(std::FILE* file) {
  std::uint8_t header[kFileHeaderBytes] = {};
  std::memcpy(header, kFileMagic, sizeof kFileMagic);
  PutU32Le(header + 8, kFileVersion);
  PutU32Le(header + 12, kFileRecordBytes);
  return std::fwrite(header, 1, sizeof header, file) == sizeof header;
}

std::string ConfiguredPath() {
  const char* env = std::getenv("RB_CODEMAP");
  if (env != nullptr && env[0] != '\0') return env;
#if defined(__ANDROID__)
  // An installed app process inherits no shell environment, so `adb shell setprop` is the only way to
  // name the file from outside. `debug.*` properties are readable by an app and settable by the shell,
  // which is the same debuggable-build assumption the launcher already makes.
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.retrobug.codemap", value) > 0 && value[0] != '\0') {
    return value;
  }
#endif
  return {};
}

}  // namespace

namespace detail {

std::atomic<bool> enabled{false};

void RecordBlockEntry(std::uint32_t pc, bool thumb) {
  Recorder& r = *g_recorder;
  const std::uint64_t key = (static_cast<std::uint64_t>(pc) << 1) | (thumb ? 1u : 0u);
  std::lock_guard<std::mutex> lk(r.mu);
  if (r.file == nullptr) return;  // Stop raced this call; the record is simply not part of the file
  if (!r.seen.insert(key).second) return;  // already recorded: one record per reachable start

  std::uint8_t record[kFileRecordBytes] = {};
  PutU64Le(record, pc);
  record[8] = thumb ? 1u : 0u;
  record[9] = kSourceBlockEntry;
  if (std::fwrite(record, 1, sizeof record, r.file) != sizeof record) {
    // A full disk must not turn every later block entry into another failed write. Close, disable, and
    // leave the records already written on disk for the tool to use.
    std::fclose(r.file);
    r.file = nullptr;
    r.buffer.reset();
    enabled.store(false, std::memory_order_relaxed);
    return;
  }
  ++r.records;
}

}  // namespace detail

bool Start(const std::string& path, std::string* err) {
  if (err != nullptr) err->clear();
  if (path.empty()) {
    if (err != nullptr) *err = "the code-map trace path is empty";
    return false;
  }
  Stop();
  Recorder& r = *g_recorder;
  std::lock_guard<std::mutex> lk(r.mu);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    if (err != nullptr) *err = "cannot open " + path + " for writing: " + std::strerror(errno);
    return false;
  }
  auto buffer = std::make_unique<char[]>(kBufferBytes);
  // A libc that refuses the suggested buffer still writes correctly, just with its own default
  // buffering; that is not a reason to fail a diagnostic.
  (void)std::setvbuf(file, buffer.get(), _IOFBF, kBufferBytes);
  if (!WriteHeader(file)) {
    if (err != nullptr) *err = "cannot write the header to " + path + ": " + std::strerror(errno);
    std::fclose(file);
    return false;
  }
  r.file = file;
  r.buffer = std::move(buffer);
  r.seen.clear();
  r.records = 0;
  r.path = path;
  // Normal process exit flushes the trace. A process killed outright can lose the last stdio buffer,
  // which is why the boot also calls Stop() from its destructor and why the format tolerates a
  // truncated tail.
  std::call_once(g_atexit_once, [] { std::atexit(&Stop); });
  detail::enabled.store(true, std::memory_order_relaxed);
  return true;
}

bool StartFromEnvironment(std::string* err) {
  if (err != nullptr) err->clear();
  const std::string configured = ConfiguredPath();
  if (configured.empty()) return false;  // no trace asked for: the normal launch
  // A process boots one library at a time (each System.loadLibrary reaches the boot separately). The
  // first call opens the trace; later ones must append, not truncate the starts the earlier libraries
  // already reported. An explicit `Start` is different: its callers (and the test) ask for a fresh file.
  {
    Recorder& r = *g_recorder;
    std::lock_guard<std::mutex> lk(r.mu);
    if (detail::enabled.load(std::memory_order_relaxed) && r.file != nullptr && r.path == configured) {
      return true;
    }
  }
  return Start(configured, err);
}

void Stop() {
  detail::enabled.store(false, std::memory_order_relaxed);
  Recorder& r = *g_recorder;
  std::lock_guard<std::mutex> lk(r.mu);
  if (r.file != nullptr) {
    std::fflush(r.file);
    std::fclose(r.file);
    r.file = nullptr;
  }
  r.buffer.reset();
  r.seen.clear();
  r.path.clear();
}

bool enabled() noexcept { return detail::enabled.load(std::memory_order_relaxed); }

std::uint64_t record_count() {
  if (!detail::enabled.load(std::memory_order_relaxed)) return 0;
  Recorder& r = *g_recorder;
  std::lock_guard<std::mutex> lk(r.mu);
  return r.records;
}

std::string path() {
  Recorder& r = *g_recorder;
  std::lock_guard<std::mutex> lk(r.mu);
  return r.path;
}

}  // namespace rb::exec::trace
