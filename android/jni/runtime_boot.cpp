// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb_runtime_boot`: M1's in-process boot. A repackaged game's `System.loadLibrary("X")` loads a 64-bit
// stub named libX.so (`stub.cpp`), whose `JNI_OnLoad` lands here with the game's own APK path. The 32-bit
// library and the guest system libraries travel inside that APK as assets (the repackager's layout:
// `assets/rb32/<libX.so>`, `assets/rb32sys/{libc,libm,libz,libstdc++}.so`), and this runs the launcher's
// existing `Inspect` on them, Tier J, headless.

#include "runtime_boot.h"

#include <android/log.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "inspect.h"

namespace {

constexpr const char* kTag = "RBRUNTIME";

/// A line of this file's own, logged under RBRUNTIME and kept for the report file. `Inspect`'s lines
/// reach logcat live through its own logging (the tag is switched to RBRUNTIME before it runs).
void Line(std::string* out, const std::string& s) {
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", s.c_str());
  *out += s;
  *out += '\n';
}

std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path, std::string* why) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *why = std::strerror(errno);
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[1 << 16];
  for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) {
    *why = "read error";
    return std::nullopt;
  }
  return bytes;
}

/// This process's package, from `/proc/self/cmdline` (the first NUL-terminated string; a `:service`
/// process suffix is dropped). Empty when it cannot be read.
std::string PackageName() {
  std::string why;
  auto cmd = ReadFile("/proc/self/cmdline", &why);
  if (!cmd) return {};
  std::string name(reinterpret_cast<const char*>(cmd->data()),
                   strnlen(reinterpret_cast<const char*>(cmd->data()), cmd->size()));
  const auto colon = name.find(':');
  if (colon != std::string::npos) name.resize(colon);
  return name;
}

std::mutex g_mu;
std::set<std::string> g_booted;

}  // namespace

extern "C" int rb_runtime_boot(JavaVM* /*vm*/, const char* apk_path, const char* lib_name) {
  const std::string lib = lib_name != nullptr ? lib_name : "";
  const std::string apk = apk_path != nullptr ? apk_path : "";
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_booted.insert(lib).second) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "rb_runtime_boot: %s already booted in this process",
                          lib.c_str());
      return JNI_VERSION_1_6;
    }
  }
  rb::launcher::SetLogTag(kTag);

  std::string report;
  Line(&report, "rb_runtime_boot: lib=" + (lib.empty() ? std::string{"(none)"} : lib) +
                    " apk=" + (apk.empty() ? std::string{"(none)"} : apk));

  std::vector<std::uint8_t> app, libc, libm, libz, libstdcxx;
  std::string why;
  bool have_apk = false;
  if (apk.empty()) {
    Line(&report, "FATAL: rb_runtime_boot: no APK path was given — the stub could not find the game's"
                  " sourceDir");
  } else if (auto file = ReadFile(apk, &why)) {
    have_apk = true;
    const auto entry = [&](const std::string& name, std::vector<std::uint8_t>* into) {
      std::string ewhy;
      if (auto bytes = rb::launcher::ZipEntry(*file, name, &ewhy)) {
        *into = std::move(*bytes);
        Line(&report, "rb_runtime_boot: " + name + " = " + std::to_string(into->size()) + " bytes");
      } else {
        Line(&report, "rb_runtime_boot: missing " + name + " in the APK (" + ewhy + ")");
      }
    };
    if (lib.empty()) {
      Line(&report, "FATAL: rb_runtime_boot: no library name was given");
    } else {
      entry("assets/rb32/" + lib, &app);
    }
    entry("assets/rb32sys/libc.so", &libc);
    entry("assets/rb32sys/libm.so", &libm);
    entry("assets/rb32sys/libz.so", &libz);
    entry("assets/rb32sys/libstdc++.so", &libstdcxx);
  } else {
    Line(&report, "FATAL: rb_runtime_boot: cannot read the APK " + apk + ": " + why);
  }

  // Inspect names an empty app or libm image itself (a FATAL line), so it runs whenever the APK was read.
  if (have_apk) {
    try {
      report += rb::launcher::Inspect(app, libm, libc, libz, libstdcxx, lib, /*tiers=*/2, apk, 0, 0,
                                      rb::launcher::RenderHooks{});
    } catch (const std::exception& e) {
      Line(&report, std::string{"FATAL: unhandled exception while booting: "} + e.what());
    }
  }

  const std::string pkg = PackageName();
  if (pkg.empty()) {
    Line(&report, "rb_runtime_boot: report not written — /proc/self/cmdline names no package");
  } else {
    const std::string dir = "/data/data/" + pkg + "/files";
    ::mkdir(dir.c_str(), 0771);  // usually there already; a failure shows up as the fopen's below
    const std::string path = dir + "/rb-report.txt";
    Line(&report, "rb_runtime_boot: report -> " + path);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "rb_runtime_boot: could not write %s: %s", path.c_str(),
                          std::strerror(errno));
    } else {
      std::fwrite(report.data(), 1, report.size(), f);
      std::fclose(f);
    }
  }
  return JNI_VERSION_1_6;
}
