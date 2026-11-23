// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A focused repro for the build-22 crash: boot the KitKat scope (libc/libm/libz/libstdc++) with the
// libc AOT image in the profile. The device log ends just after the scope bytes are read and before the
// first `load:` line, so the crash is inside LoadScope; this reproduces it on Android, where adb can
// show the faulting thread. Not shipped — a diagnostic target.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "rb/exec/aot.h"
#include "rb/runtime/guest_process.h"

extern const rb::exec::AotImageInfo kAotKitkatLibcImage;

namespace {
const rb::exec::AotLibrary kLibraries[] = {
    {"libc.so", "8fc841e8568caf228e7fca49076991ae8c8e693d81c525fadf1ac888c5b152b4",
     &kAotKitkatLibcImage},
};
const rb::exec::AotProfile kProfile = {"scope", kLibraries, 1};

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const std::streamsize size = in.tellg();
  if (size <= 0) return {};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.seekg(0, std::ios::beg);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
  return bytes;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/data/local/tmp/aot-src/kitkat";
  const auto libc = ReadFile(dir + "/libc.so");
  const auto libm = ReadFile(dir + "/libm.so");
  const auto libz = ReadFile(dir + "/libz.so");
  const auto libstdcxx = ReadFile(dir + "/libstdc++.so");
  if (libc.empty() || libm.empty() || libz.empty()) {
    std::printf("# aot-scope: no scope images in %s — skipped\n", dir.c_str());
    return 0;
  }
  rb::runtime::GuestProcessOptions o;
  o.window_bytes = std::size_t{4} << 30;
  o.unresolved_trap = 0xfffe'0000u;
  o.ctor_tier = 1;
  o.enable_jit = false;
  o.aot = &kProfile;
  o.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
  std::string err;
  auto process = rb::runtime::GuestProcess::Create(o, &err);
  if (!process) {
    std::printf("FAIL create: %s\n", err.c_str());
    return 1;
  }
  std::printf("# aot-scope: process created, aot_active=%d\n", process->aot_active() ? 1 : 0);
  std::fflush(stdout);
  const bool ok = process->LoadScope(libc, libm, libz, libstdcxx, &err);
  std::printf("RESULT load_scope=%d aot_active=%d err=%s\n", ok ? 1 : 0,
              process->aot_active() ? 1 : 0, err.c_str());
  if (!ok) return 1;
  // The scope constructors are the next guest code the iOS boot runs — the first AOT code after the
  // IFUNC change. Running them here proves that step on the tablet, without the iPad.
  std::printf("# aot-scope: running constructors...\n");
  std::fflush(stdout);
  std::string ctor_err;
  const bool ctors = process->RunConstructors(&ctor_err);
  std::printf("RESULT constructors=%d aot_active=%d err=%s\n", ctors ? 1 : 0,
              process->aot_active() ? 1 : 0, ctor_err.c_str());
  return ctors ? 0 : 1;
}
