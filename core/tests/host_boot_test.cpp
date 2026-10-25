// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::host::Boot` (S1): the in-process boot the Android app and the iOS shell share, run on a real APK
// written by the test. Rows:
//   * the scope boots from assets/rb32sys/, a DT_NEEDED sibling loads first, and every constructor runs in
//     bionic's order (leaf's global is visible to the dependent's constructor);
//   * a library boots once (a second call is a no-op with no report), and the published module is what a
//     native dispatch looks up;
//   * dlopen/dlsym answer the game's own libraries and refuse a name neither they nor the host define;
//   * a missing entry fails by name without poisoning the process, so the next call can still boot;
//   * a constructor fault after guest state changed poisons the process, and every later boot refuses.
//
// The images are the tablet's own (the gate's fixtures): a different libc build is refused by
// `GuestProcess` before any row here means anything.

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fake_java.h"
#include "rb/abi/abi.h"
#include "rb/host/boot.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/guest_process.h"
#include "zip_builder.h"

#ifndef RB_DEVICE_LIBC_PATH
#define RB_DEVICE_LIBC_PATH "/data/local/tmp/rb_device_libc.so"
#endif
#ifndef RB_GUEST_LIBM_PATH
#define RB_GUEST_LIBM_PATH "/data/local/tmp/libm.so"
#endif
#ifndef RB_GUEST_LIBZ_PATH
#define RB_GUEST_LIBZ_PATH "/data/local/tmp/libz.so"
#endif
#ifndef RB_LINK_LEAF_PATH
#define RB_LINK_LEAF_PATH "/data/local/tmp/librbleaf.so"
#endif
#ifndef RB_LINK_MID_PATH
#define RB_LINK_MID_PATH "/data/local/tmp/librbmid.so"
#endif
#ifndef RB_CTOR_FAULT_PATH
#define RB_CTOR_FAULT_PATH "/data/local/tmp/librbctorfault.so"
#endif

namespace {

using namespace rb;
using rb::runtime::GuestProcess;

constexpr std::uint32_t kTrap = 0x0bad'0000u;
constexpr std::uint32_t kDlSlotBase = 400;  ///< the test's own host slots, far above the JNIEnv's 0..232

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                   \
  do {                                            \
    ++checks;                                     \
    if (!(cond)) fail(#cond, __LINE__, (detail)); \
  } while (0)

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  if (n <= 0) return false;
  f.seekg(0, std::ios::beg);
  out->resize(static_cast<std::size_t>(n));
  f.read(reinterpret_cast<char*>(out->data()), n);
  return static_cast<bool>(f);
}

std::uint32_t Word(GuestProcess& p, std::uint32_t at) {
  std::uint32_t v = 0xdead'dead;
  if (!p.memory().Read(at, &v, 4)) return 0xdead'dead;
  return v;
}

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

struct Images {
  std::vector<std::uint8_t> libc, libm, libz, leaf, mid, ctorfault;
  bool ok = false;
  std::string missing;
};

Images LoadImages() {
  Images im;
  struct Item {
    const char* path;
    std::vector<std::uint8_t>* into;
  };
  const Item items[] = {{RB_DEVICE_LIBC_PATH, &im.libc},   {RB_GUEST_LIBM_PATH, &im.libm},
                        {RB_GUEST_LIBZ_PATH, &im.libz},     {RB_LINK_LEAF_PATH, &im.leaf},
                        {RB_LINK_MID_PATH, &im.mid},        {RB_CTOR_FAULT_PATH, &im.ctorfault}};
  for (const Item& it : items) {
    if (!ReadFile(it.path, it.into)) {
      im.missing = it.path;
      return im;
    }
  }
  im.ok = true;
  return im;
}

/// A zip in the repack layout: the three guest system libraries and the app's libraries.
std::vector<std::uint8_t> BuildApk(const Images& im) {
  test::ZipWriter w;
  w.AddDeflated("assets/rb32sys/libc.so", im.libc);
  w.AddDeflated("assets/rb32sys/libm.so", im.libm);
  w.AddDeflated("assets/rb32sys/libz.so", im.libz);
  w.AddDeflated("assets/rb32/librbleaf.so", im.leaf);
  w.AddDeflated("assets/rb32/librbmid.so", im.mid);
  w.AddDeflated("assets/rb32/librbctorfault.so", im.ctorfault);
  w.AddStored("AndroidManifest.xml", test::Bytes("the boot does not read this"));
  return w.Finish();
}

std::string WriteTempApk(const std::vector<std::uint8_t>& bytes) {
  const char* dir = std::getenv("TMPDIR");
  std::string tmpl = std::string(dir != nullptr && *dir != '\0' ? dir : "/data/local/tmp") + "/rb_boot_XXXXXX";
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  if (fd < 0) return {};
  const ssize_t n = ::write(fd, bytes.data(), bytes.size());
  ::close(fd);
  if (n != static_cast<ssize_t>(bytes.size())) {
    std::remove(path.data());
    return {};
  }
  return path.data();
}

/// What the boot's callbacks share: the line sink, the process the resolver answers slots from, and
/// whether the platform's bridge hook ran.
struct Rig {
  std::vector<std::string> lines;
  GuestProcess* proc = nullptr;
  bool bridged = false;
};

rb::host::BootConfig MakeConfig(const std::shared_ptr<Rig>& rig) {
  rb::host::BootConfig c;
  c.window_bytes = 128u << 20;
  c.unresolved_trap = kTrap;
  c.ctor_tier = 1;  // the iOS lane: Tier I boots a real libc and runs constructors
  c.monitor = false;
  c.log = [rig](const std::string& line) { rig->lines.push_back(line); };
  c.make_java = [](std::string*) -> std::unique_ptr<rb::abi::HostJava> {
    return std::make_unique<test::FakeJava>();
  };
  c.resolve_host = [rig](const std::string& name) -> std::optional<exec::GuestAddr> {
    if (rig->proc == nullptr) return std::nullopt;
    static const char* const kDl[] = {"dlopen", "android_dlopen_ext", "dlsym", "dlclose", "dladdr", "dlerror",
                                      "dl_unwind_find_exidx", "android_get_application_target_sdk_version"};
    for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
      if (name == kDl[i]) return rig->proc->abi().SlotFor(kDlSlotBase + i);
    }
    return std::nullopt;
  };
  c.on_bridge = [rig](rb::host::Boot&, GuestProcess& p) {
    rig->proc = &p;
    rig->bridged = true;
    static const char* const kDl[] = {"dlopen", "android_dlopen_ext", "dlsym", "dlclose", "dladdr", "dlerror",
                                      "dl_unwind_find_exidx", "android_get_application_target_sdk_version"};
    for (std::uint32_t i = 0; i < sizeof kDl / sizeof kDl[0]; ++i) {
      const bool sdk = std::strcmp(kDl[i], "android_get_application_target_sdk_version") == 0;
      p.abi().Bind(kDlSlotBase + i, [sdk](exec::MachineState& s, std::string*) {
        s.r[0] = sdk ? 34u : 0u;
        return true;
      });
    }
  };
  return c;
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string all;
  for (const std::string& l : lines) all += l + "\n";
  return all;
}

/// A loaded module by soname. `Boot::module` is the published set (what a native dispatch looks up: the
/// libraries a boot was requested for), so a DT_NEEDED dependency is found through the process's list.
const loader::GuestModule* Module(rb::host::Boot& boot, const std::string& soname) {
  if (const loader::GuestModule* m = boot.module(soname)) return m;
  for (const auto& m : boot.process()->modules()) {
    if (m->soname == soname) return m.get();
  }
  return nullptr;
}

/// A data export (`FindExport` is the code-address path; a global is in the export table as a Symbol).
const loader::Symbol* Export(rb::host::Boot& boot, const std::string& lib, const std::string& name) {
  const loader::GuestModule* m = Module(boot, lib);
  if (m == nullptr) return nullptr;
  for (const loader::Symbol& s : m->exports) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

void boots_a_scope_and_the_sibling_first(const std::string& apk) {
  auto rig = std::make_shared<Rig>();
  rb::host::Boot boot(MakeConfig(rig));
  const auto r = boot.BootLibrary(apk, "librbmid.so");
  CHECK_CTX(r.ok, r.report);
  CHECK(!r.already);
  CHECK(rig->bridged);
  CHECK_CTX(boot.poisoned().empty(), boot.poisoned());
  CHECK_CTX(boot.process() != nullptr, "no published process after a successful boot");
  CHECK(boot.java() != nullptr);
  CHECK(boot.jni() != nullptr);
  CHECK(boot.inbound() != nullptr);
  CHECK(Module(boot, "librbleaf.so") != nullptr);
  CHECK(boot.module("librbmid.so") != nullptr);
  CHECK_CTX(boot.process()->libc() != nullptr, "scope libc missing");
  CHECK(boot.booted("librbmid.so"));
  CHECK(!boot.booted("librbleaf.so"));  // loaded as a dependency, not booted as the request

  // The report says what the boot did, by name.
  CHECK_CTX(r.report.find("scope: booted once") != std::string::npos, "no scope line");
  CHECK_CTX(r.report.find("needs: librbmid.so -> librbleaf.so, a sibling in assets/rb32/, loaded first") !=
                std::string::npos,
            "the sibling was not named as loaded first");
  CHECK_CTX(r.report.find("RESULT: startup finished with no fault") != std::string::npos, "no RESULT line");

  GuestProcess& p = *boot.process();
  // Constructors ran, and ran in bionic's order: mid's constructor saw the leaf's value.
  const loader::Symbol* leaf = Export(boot, "librbleaf.so", "g_leaf_ran");
  CHECK(leaf != nullptr);
  if (leaf) CHECK_CTX(Word(p, leaf->value) == 0xA11A11u, "g_leaf_ran=" + Hex(Word(p, leaf->value)));
  const loader::Symbol* mid = Export(boot, "librbmid.so", "g_mid_ran");
  CHECK(mid != nullptr);
  if (mid) CHECK_CTX(Word(p, mid->value) == 0xB22B22u + 5u, "g_mid_ran=" + Hex(Word(p, mid->value)));
  const loader::Symbol* seen = Export(boot, "librbmid.so", "g_leaf_seen_by_mid");
  CHECK(seen != nullptr);
  if (seen) CHECK_CTX(Word(p, seen->value) == 0xA11A11u, "g_leaf_seen_by_mid=" + Hex(Word(p, seen->value)));

  // A second boot of the same library is a no-op and collects no report.
  const auto again = boot.BootLibrary(apk, "librbmid.so");
  CHECK(again.ok);
  CHECK(again.already);
  CHECK(again.report.empty());

  // dlopen/dlsym: the game's own library by name, a real export, and a name neither it nor the host has.
  std::string why;
  const std::uint32_t h = boot.DlOpen("librbleaf.so", &why);
  CHECK_CTX(h != 0, why);
  if (h != 0) {
    const auto add = boot.DlSym(h, "leaf_add");
    CHECK_CTX(add.has_value(), "leaf_add not found through the handle");
    CHECK(!boot.DlSym(h, "rb_no_such_symbol").has_value());
  }
  CHECK(!boot.DlSym(0xdead'beefu, "leaf_add").has_value());  // not a handle this runtime gave out
  const std::uint32_t global = boot.DlOpen("", &why);  // dlopen(NULL): the "main program"
  CHECK_CTX(global != 0, "dlopen(NULL) answered 0");
  CHECK_CTX(boot.DlSym(global, "leaf_add").has_value(), "dlsym on the global handle did not search every module");
}

void a_missing_entry_fails_by_name_and_can_be_retried(const std::string& apk) {
  auto rig = std::make_shared<Rig>();
  rb::host::Boot boot(MakeConfig(rig));
  const auto miss = boot.BootLibrary(apk, "libnothere.so");
  CHECK(!miss.ok);
  CHECK(!miss.already);
  CHECK_CTX(miss.report.find("libnothere.so") != std::string::npos, "the missing library was not named");
  CHECK_CTX(miss.report.find("no entry named assets/rb32/libnothere.so") != std::string::npos, miss.report);
  CHECK_CTX(boot.poisoned().empty(), boot.poisoned());

  // Nothing guest-side ran, so the same process can still boot a real library.
  const auto r = boot.BootLibrary(apk, "librbleaf.so");
  CHECK_CTX(r.ok, r.report);
  CHECK(!r.already);
  GuestProcess& p = *boot.process();
  const loader::Symbol* leaf = Export(boot, "librbleaf.so", "g_leaf_ran");
  CHECK(leaf != nullptr);
  if (leaf) CHECK(Word(p, leaf->value) == 0xA11A11u);
}

void a_constructor_fault_poisons_the_process(const std::string& apk) {
  auto rig = std::make_shared<Rig>();
  rb::host::Boot boot(MakeConfig(rig));
  const auto bad = boot.BootLibrary(apk, "librbctorfault.so");
  CHECK(!bad.ok);
  CHECK_CTX(bad.report.find("constructor") != std::string::npos, bad.report);
  CHECK_CTX(boot.poisoned().find("librbctorfault.so") != std::string::npos, boot.poisoned());

  const auto refused = boot.BootLibrary(apk, "librbleaf.so");
  CHECK(!refused.ok);
  CHECK_CTX(refused.report.find("refused") != std::string::npos, refused.report);
  CHECK_CTX(refused.report.find("half-initialized") != std::string::npos, refused.report);
}

}  // namespace

int main() {
  const Images im = LoadImages();
  if (!im.ok) {
    std::fprintf(stderr, "# host boot lane skipped: %s is missing (run tools/run_engine_tests.sh)\n", im.missing.c_str());
    std::printf("0 checks, 0 failures (skipped)\n");
    return 0;
  }
  const std::vector<std::uint8_t> apk_bytes = BuildApk(im);
  const std::string apk = WriteTempApk(apk_bytes);
  if (apk.empty()) {
    std::fprintf(stderr, "# host boot lane: could not write a temp APK\n");
    return 1;
  }
  boots_a_scope_and_the_sibling_first(apk);
  a_missing_entry_fails_by_name_and_can_be_retried(apk);
  a_constructor_fault_poisons_the_process(apk);
  std::remove(apk.c_str());
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
