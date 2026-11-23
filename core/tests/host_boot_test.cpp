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
//   * a stock APK (libraries under lib/armeabi-v7a, no assets/rb32sys) boots when the platform supplies the
//     system libraries, as the iOS app bundle does;
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
#include <thread>
#include <vector>

#include "fake_java.h"
#include "rb/abi/abi.h"
#include "rb/host/boot.h"
#include "rb/java/android_base.h"
#include "rb/java/dlgl.h"
#include "rb/java/halfbrick.h"
#include "rb/java/pikpok.h"
#include "rb/loader/guest_elf.h"
#include "rb/loader/apk_assets.h"
#include "rb/mem/guest_memory.h"
#include "rb/runtime/guest_process.h"
#include "zip_builder.h"
#include "../../platform/ios/unity_bootstrap.h"
#include "../../platform/ios/rb_ios.h"

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

/// A stock store APK: its own libraries where the platform looks, and no assets/rb32sys at all.
std::vector<std::uint8_t> BuildStockApk(const Images& im) {
  test::ZipWriter w;
  w.AddDeflated("lib/armeabi-v7a/librbleaf.so", im.leaf);
  w.AddDeflated("lib/armeabi-v7a/librbmid.so", im.mid);
  w.AddDeflated("lib/armeabi-v7a/librbctorfault.so", im.ctorfault);
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
  c.ctor_tier = 1;      // the iOS lane: Tier I boots a real libc and runs constructors
  c.enable_jit = false; // no AArch64 backend in this lane, as on iOS (it allocates executable memory)
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
  // The iOS shape: no JIT object exists, so both tier numbers answer the interpreter.
  CHECK_CTX(&p.engine(1) == &p.engine(2), "engine(2) is not the interpreter with enable_jit=false");
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

/// A stock APK has no assets/rb32sys, so the platform answers the three system libraries. The boot must
/// take them from there, read the game's own libraries from lib/armeabi-v7a, and run constructors as it
/// does for a repack.
void a_stock_apk_boots_from_platform_system_libraries(const Images& im) {
  auto rig = std::make_shared<Rig>();
  rb::host::BootConfig cfg = MakeConfig(rig);
  int reads = 0;
  cfg.read_system_lib = [&im, &reads](const std::string& name)
      -> std::optional<std::vector<std::uint8_t>> {
    ++reads;
    if (name == "libc.so") return im.libc;
    if (name == "libm.so") return im.libm;
    if (name == "libz.so") return im.libz;
    return std::nullopt;
  };
  const std::string apk = WriteTempApk(BuildStockApk(im));
  CHECK(!apk.empty());
  if (apk.empty()) return;
  rb::host::Boot boot(cfg);
  const auto r = boot.BootLibrary(apk, "librbmid.so");
  std::remove(apk.c_str());
  CHECK_CTX(r.ok, r.report);
  CHECK(reads == 3);
  CHECK_CTX(r.report.find("this is a stock APK") != std::string::npos, r.report);
  CHECK_CTX(r.report.find("libc.so from the app bundle") != std::string::npos, r.report);
  CHECK_CTX(r.report.find("needs: librbmid.so -> librbleaf.so, a sibling in lib/armeabi-v7a/") !=
                std::string::npos,
            r.report);
  const loader::Symbol* mid = Export(boot, "librbmid.so", "g_mid_ran");
  CHECK(mid != nullptr);
  if (mid) CHECK_CTX(Word(*boot.process(), mid->value) == 0xB22B22u + 5u, "g_mid_ran missing");
}

/// Without a platform answer the same stock APK still fails at the scope, by the name it looked for.
void a_stock_apk_without_platform_libs_is_refused_by_name(const Images& im) {
  auto rig = std::make_shared<Rig>();
  const std::string apk = WriteTempApk(BuildStockApk(im));
  CHECK(!apk.empty());
  if (apk.empty()) return;
  rb::host::Boot boot(MakeConfig(rig));
  const auto refused = boot.BootLibrary(apk, "librbmid.so");
  std::remove(apk.c_str());
  CHECK(!refused.ok);
  CHECK_CTX(refused.report.find("assets/rb32sys/libc.so") != std::string::npos, refused.report);
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

void unity_uses_the_imports_package_and_launch_intent() {
  java::TableJava java;
  java::AndroidEnv env;
  env.package_name = "com.example.unity";
  env.apk_path = "/games/test.apk";
  java::InstallAndroidBase(java, env);
  platform::ios::UnityBootstrap::InstallJava(java, env.apk_path, {env.package_name, u"1.5.6", 42});
  const auto activity = java.NewInstance("com/unity3d/player/UnityPlayerActivity");
  const auto cls = java.GetObjectClass(activity);
  const auto intent_method = java.GetMethodID(cls, "getIntent", "()Landroid/content/Intent;", false);
  const auto intent = java.Call('L', false, activity, 0, intent_method, {});
  CHECK(intent != 0);
  const auto object = java.FindClass("java/lang/Object");
  const auto to_string = java.GetMethodID(object, "toString", "()Ljava/lang/String;", false);
  const auto text = java.Call('L', false, intent, 0, to_string, {});
  CHECK(java.GetStringChars(text) == u"Intent { act=android.intent.action.MAIN }");

  const auto get_pm = java.GetMethodID(cls, "getPackageManager", "()Landroid/content/pm/PackageManager;", false);
  const auto manager = java.Call('L', false, activity, 0, get_pm, {});
  CHECK(manager != 0);
  const auto pm_class = java.GetObjectClass(manager);
  const auto get_package = java.GetMethodID(pm_class, "getPackageInfo",
                                            "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", false);
  const auto name = java.NewString(u"com.example.unity");
  const auto info = java.Call('L', false, manager, 0, get_package, {name, 0});
  CHECK(info != 0);
  const auto info_class = java.GetObjectClass(info);
  const auto version = java.GetFieldID(info_class, "versionName", "Ljava/lang/String;", false);
  const auto code = java.GetFieldID(info_class, "versionCode", "I", false);
  CHECK(java.GetStringChars(java.GetField('L', false, info, version)) == u"1.5.6");
  CHECK(java.GetField('I', false, info, code) == 42);
  const auto installer = java.GetMethodID(pm_class, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;",
                                           false);
  CHECK(installer != 0 && java.Call('L', false, manager, 0, installer, {name}) == 0);
  CHECK(!java.ExceptionCheck());
  const auto missing = java.NewString(u"com.example.missing");
  CHECK(java.Call('L', false, manager, 0, get_package, {missing, 0}) == 0);
  CHECK(java.PendingClass() == "android/content/pm/PackageManager$NameNotFoundException");
}

void unity_can_cache_its_player_callbacks_and_power_methods() {
  java::TableJava java;
  java::AndroidEnv env;
  java::InstallAndroidBase(java, env);
  std::string requested;
  bool shutdown = false;
  platform::ios::UnityBootstrap::JavaServices services;
  services.load_library = [&](const std::string& name) { requested = name; return name == "unity"; };
  services.request_shutdown = [&] { shutdown = true; };
  platform::ios::UnityBootstrap::InstallJava(java, "/games/test.apk", {env.package_name, u"1", 1}, services);
  const auto player_class = java.FindClass("com/unity3d/player/UnityPlayer");
  const auto player = java.NewInstance("com/unity3d/player/UnityPlayer");
  for (const auto& [name, sig] : {
           std::pair{"showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V"}, {"hideSoftInput", "()V"},
           {"setSoftInputStr", "(Ljava/lang/String;)V"}, {"showVideoPlayer", "(Ljava/lang/String;IIIZII)V"},
           {"Location_IsServiceEnabledByUser", "()Z"}, {"Location_SetDesiredAccuracy", "(F)V"},
           {"Location_SetDistanceFilter", "(F)V"}, {"Location_StartUpdatingLocation", "()V"},
           {"Location_StopUpdatingLocation", "()V"}, {"setFullscreen", "(Z)V"}, {"initCamera", "(IIII)[I"},
           {"closeCamera", "(I)V"}, {"getNumCameras", "()I"}, {"isCameraFrontFacing", "(I)Z"},
           {"getCameraOrientation", "(I)I"}, {"restartFMODAudioDevice", "()V"},
           {"startActivityIndicator", "()V"}, {"stopActivityIndicator", "()V"}, {"kill", "()V"},
           {"executeGLThreadJobs", "()V"}, {"getSplashMode", "()I"}, {"disableLogger", "()V"},
           {"loadLibrary", "(Ljava/lang/String;)Z"}, {"installPresentationDisplay", "(I)Z"}}) {
    CHECK_CTX(java.GetMethodID(player_class, name, sig, false) != 0, name);
  }
  const auto load = java.GetMethodID(player_class, "loadLibrary", "(Ljava/lang/String;)Z", false);
  CHECK(java.Call('Z', false, player, 0, load, {java.NewString(u"unity")}) == 1);
  CHECK(requested == "unity");
  CHECK(java.Call('Z', false, player, 0, load, {java.NewString(u"../unity")}) == 0 && requested == "unity");
  const auto kill = java.GetMethodID(player_class, "kill", "()V", false);
  java.Call('V', false, player, 0, kill, {});
  CHECK(shutdown);
  const auto context_class = java.FindClass("android/content/Context");
  const auto power_field = java.GetFieldID(context_class, "POWER_SERVICE", "Ljava/lang/String;", true);
  const auto power = java.GetField('L', true, context_class, power_field);
  CHECK(java.GetStringChars(power) == u"power");
  CHECK(java.GetFieldID(context_class, "MODE_PRIVATE", "I", true) != 0);
  const auto activity = java.NewInstance("com/unity3d/player/UnityPlayerActivity");
  const auto system = java.GetMethodID(context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", false);
  const auto manager = java.Call('L', false, activity, 0, system, {power});
  const auto wake = java.GetMethodID(java.GetObjectClass(manager), "newWakeLock",
                                    "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;", false);
  const auto lock = java.Call('L', false, manager, 0, wake, {1, java.NewString(u"fixture")});
  const auto lock_class = java.GetObjectClass(lock);
  const auto held = java.GetMethodID(lock_class, "isHeld", "()Z", false);
  CHECK(java.Call('Z', false, lock, 0, held, {}) == 0);
  java.Call('V', false, lock, 0, java.GetMethodID(lock_class, "acquire", "()V", false), {});
  CHECK(java.Call('Z', false, lock, 0, held, {}) == 1);
  java.Call('V', false, lock, 0, java.GetMethodID(lock_class, "release", "()V", false), {});
  CHECK(java.Call('Z', false, lock, 0, held, {}) == 0);
  CHECK(!java.ExceptionCheck());
}

void unity_reads_apk_asset_streams_and_finds_its_obb_directory() {
  test::ZipWriter zip;
  zip.AddStored("assets/bin/Data/boot.config", test::Bytes("xyz"));
  const auto path = WriteTempApk(zip.Finish());
  CHECK(!path.empty());
  if (path.empty()) return;
  java::TableJava java;
  java::AndroidEnv env;
  env.obb_dir = "/games/test/obb";
  java::InstallAndroidBase(java, env);
  platform::ios::UnityBootstrap::InstallJava(java, path, {env.package_name, u"1", 1});
  const auto manager = java.NewInstance("android/content/res/AssetManager");
  const auto cls = java.GetObjectClass(manager);
  const auto open = java.GetMethodID(cls, "open", "(Ljava/lang/String;)Ljava/io/InputStream;", false);
  const auto name = java.NewString(u"bin/Data/boot.config");
  const auto stream = java.Call('L', false, manager, 0, open, {name});
  const auto stream_class = java.GetObjectClass(stream);
  const auto read = java.GetMethodID(stream_class, "read", "()I", false);
  CHECK(java.Call('I', false, stream, 0, read, {}) == 'x');
  const auto read_bytes = java.GetMethodID(stream_class, "read", "([BII)I", false);
  const auto bytes = java.NewArray('B', 4, 0, 0);
  CHECK(java.Call('I', false, stream, 0, read_bytes, {bytes, 1, 3}) == 2);
  char data[4]{};
  CHECK(java.GetArrayRegion('B', bytes, 0, 4, data));
  CHECK(data[0] == 0 && data[1] == 'y' && data[2] == 'z' && data[3] == 0);
  CHECK(static_cast<std::int32_t>(java.Call('I', false, stream, 0, read, {})) == -1);
  const auto player = java.NewInstance("com/unity3d/player/UnityPlayerActivity");
  const auto activity = java.GetObjectClass(player);
  const auto get_dirs = java.GetMethodID(activity, "getObbDirs", "()[Ljava/io/File;", false);
  const auto dirs = java.Call('L', false, player, 0, get_dirs, {});
  CHECK(java.GetArrayLength(dirs) == 1);
  const auto file = java.GetObjectArrayElement(dirs, 0);
  const auto get_path = java.GetMethodID(java.GetObjectClass(file), "getAbsolutePath", "()Ljava/lang/String;", false);
  CHECK(java.GetStringChars(java.Call('L', false, file, 0, get_path, {})) == u"/games/test/obb");
  CHECK(!java.ExceptionCheck());
  unlink(path.c_str());
}

void unity_can_read_committed_preferences_and_probe_android_services() {
  java::TableJava java;
  java::AndroidEnv env;
  env.obb_dir = "/games/test/obb";
  java::InstallAndroidBase(java, env);
  java::InstallDlgl(java, env);
  platform::ios::UnityBootstrap::InstallJava(java, "/games/test.apk", {env.package_name, u"1", 1});
  const auto activity = java.NewInstance("com/unity3d/player/UnityPlayerActivity");
  const auto activity_class = java.GetObjectClass(activity);
  const auto get_obb = java.GetMethodID(activity_class, "getObbDir", "()Ljava/io/File;", false);
  const auto obb = java.Call('L', false, activity, 0, get_obb, {});
  const auto file_class = java.GetObjectClass(obb);
  const auto get_path = java.GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;", false);
  CHECK(java.GetStringChars(java.Call('L', false, obb, 0, get_path, {})) == u"/games/test/obb");
  const auto manager_class = java.FindClass("android/content/pm/PackageManager");
  const auto meta_data_flag = java.GetFieldID(manager_class, "GET_META_DATA", "I", true);
  CHECK(java.GetField('I', true, manager_class, meta_data_flag) == 0x80);
  const auto get_prefs = java.GetMethodID(activity_class, "getSharedPreferences",
                                         "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", false);
  const auto group = java.NewString(u"user");
  const auto first = java.Call('L', false, activity, 0, get_prefs, {group, 0});
  const auto second = java.Call('L', false, activity, 0, get_prefs, {group, 0});
  CHECK(first != 0 && second != 0);
  const auto prefs_class = java.GetObjectClass(first);
  const auto get_int = java.GetMethodID(prefs_class, "getInt", "(Ljava/lang/String;I)I", false);
  const auto edit = java.GetMethodID(prefs_class, "edit", "()Landroid/content/SharedPreferences$Editor;", false);
  const auto editor = java.Call('L', false, first, 0, edit, {});
  const auto editor_class = java.GetObjectClass(editor);
  const auto put = java.GetMethodID(editor_class, "putInt",
                                    "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", false);
  const auto apply = java.GetMethodID(editor_class, "apply", "()V", false);
  const auto key = java.NewString(u"score");
  java.Call('L', false, editor, 0, put, {key, 77});
  CHECK(java.Call('I', false, second, 0, get_int, {key, 12}) == 12);
  java.Call('V', false, editor, 0, apply, {});
  CHECK(java.Call('I', false, second, 0, get_int, {key, 12}) == 77);

  const auto manager = java.FindClass("android/os/PowerManager");
  const auto wake = java.GetFieldID(manager, "FULL_WAKE_LOCK", "I", true);
  CHECK(wake != 0 && java.GetField('I', true, manager, wake) == 0x1a);
  const auto bridge = java.FindClass("bitter/jnibridge/JNIBridge");
  const auto connection = java.FindClass("android/content/ServiceConnection");
  const auto proxy = java.GetMethodID(bridge, "newInterfaceProxy", "(JLjava/lang/Class;)Ljava/lang/Object;", true);
  CHECK(java.IsInstanceOf(java.Call('L', true, 0, bridge, proxy, {1, connection}), connection));
  const auto proxies = java.GetMethodID(bridge, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;", true);
  const auto classes = java.NewArray('L', 1, java.FindClass("java/lang/Object"), connection);
  CHECK(java.IsInstanceOf(java.Call('L', true, 0, bridge, proxies, {1, classes}), connection));
  const auto motion_class = java.FindClass("android/view/MotionEvent");
  const auto motion = java.NewInstance("android/view/MotionEvent");
  const auto obtain = java.GetMethodID(motion_class, "obtain",
                                        "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", true);
  const auto get_action = java.GetMethodID(motion_class, "getAction", "()I", false);
  java.PutField(java.ObjectOf(motion), "action", 2);
  const auto copy = java.Call('L', true, 0, motion_class, obtain, {motion});
  java.PutField(java.ObjectOf(motion), "action", 1);
  CHECK(!java.IsSameObject(motion, copy));
  CHECK(java.Call('I', false, copy, 0, get_action, {}) == 2);
  CHECK(java.Call('I', false, motion, 0, get_action, {}) == 1);
  CHECK(!java.ExceptionCheck());
}

void proc_maps_describes_guest_addresses_and_permissions() {
  runtime::GuestProcessOptions options;
  options.ctor_tier = 1;
  options.enable_jit = false;
  std::string error;
  auto process = GuestProcess::Create(options, &error);
  CHECK_CTX(process != nullptr, error);
  if (!process) return;
  CHECK_CTX(process->memory().Map(0x40000, 0x1000, mem::kRead, &error), error);
  CHECK_CTX(process->memory().Map(0x41000, 0x1000, mem::kRead | mem::kWrite, &error), error);
  CHECK_CTX(process->memory().Map(0x42000, 0x1000, mem::kNone, &error), error);
  CHECK(process->memory().ProtAt(0x40000) == mem::kRead);
  CHECK(process->memory().ProtAt(0x41000) == (mem::kRead | mem::kWrite));
  const auto maps = process->ProcMaps();
  CHECK_CTX(maps.find("00040000-00041000 r--p 00000000 00:00 0\n") != std::string::npos, maps);
  CHECK_CTX(maps.find("00041000-00042000 rw-p 00000000 00:00 0\n") != std::string::npos, maps);
  CHECK_CTX(maps.find("00042000-00043000 ---p 00000000 00:00 0\n") != std::string::npos, maps);
}

void older_game_bridges_answer_the_startup_lookups() {
  java::TableJava java;
  java::AndroidEnv env;
  java::InstallAndroidBase(java, env);
  java::InstallPikPok(java, env);
  java::InstallHalfbrick(java);
  for (const char* name : {"com/pikpok/turbo/DeviceOrientation", "com/pikpok/MabAlertView"}) {
    const auto cls = java.FindClass(name);
    CHECK_CTX(java.GetMethodID(cls, "<init>", "(J)V", false) != 0, name);
  }
  const auto support = java.FindClass("com/halfbrick/mortar/HBSupport");
  const auto get_id = java.GetMethodID(support, "GetDeviceID", "()Ljava/lang/String;", true);
  const auto id = java.Call('L', true, 0, support, get_id, {});
  CHECK(id != 0 && java.GetStringChars(id).empty());
  CHECK(!java.ExceptionCheck());
}

void unity_android_contract_covers_display_library_loading_and_main_looper() {
  java::TableJava j;
  java::InstallAndroidBase(j, {});
  platform::ios::ApkIdentity identity{"fixture.unity", u"3.51", 203513};
  identity.target_sdk = 27;
  identity.metadata[u"unityplayer.SkipPermissionsDialog"] = true;
  platform::ios::UnityBootstrap::JavaServices services;
  services.display = [] { return platform::ios::UnityBootstrap::DisplayInfo{2360, 1640, 3, 320, 60.f}; };
  std::string library;
  services.load_library = [&](const auto& name) { library = name; return name == "il2cpp"; };
  const auto tasks = platform::ios::UnityBootstrap::InstallJava(j, "/games/fixture.apk", identity, services);
  const auto activity = j.NewInstance("com/unity3d/player/UnityPlayerActivity");
  const auto invoke = [&](char kind, auto object, const char* method, const char* signature,
                          std::vector<std::uint64_t> args = {}) {
    const auto cls = j.GetObjectClass(object);
    const auto id = j.GetMethodID(cls, method, signature, false);
    CHECK_CTX(id != 0, method);
    return id ? j.Call(kind, false, object, 0, id, args) : 0;
  };
  const auto manager = invoke('L', activity, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                              {j.NewString(u"display")});
  const auto display = invoke('L', manager, "getDisplay", "(I)Landroid/view/Display;", {0});
  CHECK(invoke('I', display, "getRotation", "()I") == 3);
  const auto metrics = j.NewInstance("android/util/DisplayMetrics");
  invoke('V', display, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", {metrics});
  CHECK(j.FieldOf(j.ObjectOf(metrics), "widthPixels") == 2360);
  CHECK(j.FieldOf(j.ObjectOf(metrics), "densityDpi") == 320);

  const auto pm = invoke('L', activity, "getPackageManager", "()Landroid/content/pm/PackageManager;");
  const auto application = invoke('L', pm, "getApplicationInfo",
      "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", {j.NewString(u"fixture.unity"), 0x80});
  CHECK(j.FieldOf(j.ObjectOf(application), "targetSdkVersion") == 27);
  const auto metadata = j.MakeRef(j.FieldOf(j.ObjectOf(application), "metaData"));
  CHECK(invoke('Z', metadata, "getBoolean", "(Ljava/lang/String;Z)Z",
                {j.NewString(u"unityplayer.SkipPermissionsDialog"), 0}) == 1);

  const auto bytes = j.NewArray('B', 6, 0, 0);
  CHECK(j.SetArrayRegion('B', bytes, 0, 6, "il2cpp"));
  const auto string = j.FindClass("java/lang/String");
  const auto ctor = j.GetMethodID(string, "<init>", "([BLjava/lang/String;)V", false);
  const auto decoded = j.NewObject(string, ctor, {bytes, j.NewString(u"UTF-8")});
  const auto player = j.NewInstance("com/unity3d/player/UnityPlayer");
  CHECK(invoke('Z', player, "loadLibrary", "(Ljava/lang/String;)Z", {decoded}) == 1 && library == "il2cpp");

  unsigned runs = 0;
  j.DefineClass("fixture/Callback", "java/lang/Runnable");
  j.DefineMethod("fixture/Callback", "run", "()V", false, [&](auto&, int, const auto&) { ++runs; return 0u; });
  const auto runnable = j.NewInstance("fixture/Callback");
  const auto handler_class = j.FindClass("android/os/Handler");
  const auto handler = j.NewObject(handler_class, j.GetMethodID(handler_class, "<init>", "()V", false), {});
  CHECK(invoke('Z', handler, "post", "(Ljava/lang/Runnable;)Z", {runnable}) == 1 && runs == 0);
  std::string why;
  CHECK_CTX(platform::ios::UnityBootstrap::RunJavaTasks(j, tasks, &why), why);
  CHECK(runs == 1 && !j.ExceptionCheck());
  CHECK(invoke('Z', handler, "postDelayed", "(Ljava/lang/Runnable;J)Z", {runnable, 60000}) == 1);
  invoke('V', handler, "removeCallbacks", "(Ljava/lang/Runnable;)V", {runnable});
  CHECK_CTX(platform::ios::UnityBootstrap::RunJavaTasks(j, tasks, &why), why);
  CHECK(runs == 1);
}

void java_exceptions_are_isolated_by_thread() {
  java::TableJava j;
  j.DefineClass("java/lang/Object");
  j.SetPending("java/lang/IllegalStateException", "render thread");
  bool thread_was_clean = false;
  std::thread worker([&] {
    thread_was_clean = !j.ExceptionCheck();
    j.SetPending("java/lang/IllegalArgumentException", "worker thread");
  });
  worker.join();
  CHECK(thread_was_clean);
  CHECK(j.PendingClass() == "java/lang/IllegalStateException" && j.PendingMessage() == "render thread");
  j.ExceptionClear();
  CHECK(!j.ExceptionCheck());
}

void asset_reads_cross_guest_page_boundaries() {
  test::ZipWriter zip;
  const auto contents = test::Bytes("eightbytes");
  zip.AddStored("assets/data", contents);
  std::string error;
  auto archive = loader::ApkArchive::OpenMemory(zip.Finish(), &error);
  CHECK_CTX(archive != nullptr, error);
  if (!archive) return;
  auto file = archive->OpenFile("assets/data", &error);
  CHECK_CTX(file != nullptr, error);
  if (!file) return;
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = 14;
  auto memory = mem::GuestMemory::Create(4 * mem::kGuestPage, model, &error);
  CHECK_CTX(memory != nullptr, error);
  if (!memory) return;
  constexpr auto page = mem::kGuestPage;
  constexpr auto destination = 2 * page - 3;
  CHECK_CTX(memory->Map(page, page, mem::kRead | mem::kWrite, &error), error);
  CHECK_CTX(memory->Map(2 * page, page, mem::kRead, &error), error);
  CHECK_CTX(memory->Map(3 * page, page, mem::kNone, &error), error);
  const auto read = [&] {
    return file->ReadTo(30, [&](auto bytes) { return memory->Write(destination, bytes.data(), bytes.size()); });
  };
  CHECK(read() == -1 && file->Position() == 0);
  CHECK_CTX(memory->Protect(2 * page, page, mem::kRead | mem::kWrite, &error), error);
  CHECK(read() == static_cast<std::int64_t>(contents.size()));
  std::vector<std::uint8_t> copied(contents.size());
  CHECK(memory->Read(destination, copied.data(), copied.size()) && copied == contents);
  CHECK(read() == 0);
}

void apk_icon_uses_the_manifest_and_density() {
  using Bytes = std::vector<std::uint8_t>;
  using test::Put16;
  using test::Put32;
  const auto append = [](Bytes& out, const Bytes& data) { out.insert(out.end(), data.begin(), data.end()); };
  const auto wrap = [&](unsigned kind, const Bytes& header, const Bytes& body) {
    Bytes chunk;
    Put16(chunk, kind);
    Put16(chunk, 8 + header.size());
    Put32(chunk, 8 + header.size() + body.size());
    append(chunk, header);
    append(chunk, body);
    return chunk;
  };
  const auto pool = [&](std::initializer_list<std::string_view> values) {
    Bytes header, offsets, text;
    Put32(header, values.size());
    Put32(header, 0);
    Put32(header, 0x100);  // UTF-8 pool.
    Put32(header, 28 + values.size() * 4);
    Put32(header, 0);
    for (const auto value : values) {
      Put32(offsets, text.size());
      text.push_back(value.size());
      text.push_back(value.size());
      text.insert(text.end(), value.begin(), value.end());
      text.push_back(0);
    }
    append(offsets, text);
    return wrap(1, header, offsets);
  };
  const auto element = [&](unsigned name, const Bytes& attributes) {
    Bytes node;
    Put32(node, 0xffffffffu);  // No element namespace.
    Put32(node, name);
    Put16(node, 20);
    Put16(node, 20);
    Put16(node, attributes.size() / 20);
    node.resize(20);
    append(node, attributes);
    return wrap(0x0102, Bytes(8), node);
  };
  const auto low = "res/low.png", high = "res/high.png";
  auto xml_nodes = pool({"manifest", "application", "http://schemas.android.com/apk/res/android", "icon"});
  append(xml_nodes, element(0, {}));
  Bytes icon_attr;
  Put32(icon_attr, 2);
  Put32(icon_attr, 3);
  Put32(icon_attr, 0xffffffffu);
  Put32(icon_attr, 0x01000008); // TYPE_REFERENCE
  Put32(icon_attr, 0x7f010002);
  append(xml_nodes, element(1, icon_attr));

  const auto type = [&](unsigned density, unsigned string_id, bool sparse) {
    Bytes header{1, static_cast<std::uint8_t>(sparse ? 1 : 0), 0, 0};
    Put32(header, sparse ? 1 : 3);
    Put32(header, sparse ? 40 : 48);
    Put32(header, 16);
    header.resize(26);
    Put16(header, density);
    Bytes entries;
    if (sparse) {
      Put16(entries, 2);
      Put16(entries, 0);
    } else {
      Put32(entries, 0xffffffffu);
      Put32(entries, 0xffffffffu);
      Put32(entries, 0);
    }
    Put16(entries, 0); // Compact value, key index zero.
    Put16(entries, 0x0308);
    Put32(entries, string_id);
    return wrap(0x0201, header, entries);
  };
  auto package = type(160, 0, false);
  append(package, type(480, 1, true));
  Bytes package_header, table_header;
  Put32(package_header, 0x7f);
  package_header.resize(280);
  Put32(table_header, 1);
  auto table = pool({low, high});
  append(table, wrap(0x0200, package_header, package));

  test::ZipWriter zip;
  zip.AddStored("AndroidManifest.xml", wrap(3, {}, xml_nodes));
  zip.AddStored("resources.arsc", wrap(2, table_header, table));
  zip.AddStored("res/drawable/ic_launcher.png", test::Bytes("unrelated SDK icon"));
  zip.AddStored(low, test::Bytes("low"));
  zip.AddStored(high, test::Bytes("high"));
  const auto path = WriteTempApk(zip.Finish());
  CHECK(!path.empty());
  if (path.empty()) return;
  unsigned length = 0;
  auto* icon = rb_ios_apk_icon(path.c_str(), &length);
  CHECK(icon != nullptr && length == 4 && std::memcmp(icon, "high", 4) == 0);
  rb_ios_free_bytes(icon);
  unlink(path.c_str());
}

}  // namespace

int main() {
  unity_uses_the_imports_package_and_launch_intent();
  unity_can_cache_its_player_callbacks_and_power_methods();
  unity_can_read_committed_preferences_and_probe_android_services();
  unity_reads_apk_asset_streams_and_finds_its_obb_directory();
  proc_maps_describes_guest_addresses_and_permissions();
  older_game_bridges_answer_the_startup_lookups();
  unity_android_contract_covers_display_library_loading_and_main_looper();
  java_exceptions_are_isolated_by_thread();
  asset_reads_cross_guest_page_boundaries();
  apk_icon_uses_the_manifest_and_density();
  const Images im = LoadImages();
  if (!im.ok) {
    std::fprintf(stderr, "# host boot lane skipped: %s is missing (run tools/run_engine_tests.sh)\n", im.missing.c_str());
    std::printf("%d checks, %d failures (Unity profile only)\n", checks, failures);
    return failures == 0 ? 0 : 1;
  }
  const std::vector<std::uint8_t> apk_bytes = BuildApk(im);
  const std::string apk = WriteTempApk(apk_bytes);
  if (apk.empty()) {
    std::fprintf(stderr, "# host boot lane: could not write a temp APK\n");
    return 1;
  }
  boots_a_scope_and_the_sibling_first(apk);
  a_stock_apk_boots_from_platform_system_libraries(im);
  a_stock_apk_without_platform_libs_is_refused_by_name(im);
  a_missing_entry_fails_by_name_and_can_be_retried(apk);
  a_constructor_fault_poisons_the_process(apk);
  std::remove(apk.c_str());
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
